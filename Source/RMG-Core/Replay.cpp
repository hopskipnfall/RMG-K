/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "Replay.hpp"
#include "ReplayMemory.hpp"
#include "Settings.hpp"
#include "Callback.hpp"
#include "Library.hpp"
#include "RomSettings.hpp"
#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "kailleraclient.h"
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace
{
#pragma pack(push, 1)

struct FileHeader
{
    char     magic[4];     // "RMGR"
    uint8_t  version;      // 1
    uint8_t  reserved[3];  // zero
    uint32_t streamLength; // 0 while recording, patched to the real value at close
};
static_assert(sizeof(FileHeader) == 12, "FileHeader must be 12 bytes");

enum class EventCode : uint8_t
{
    EventPayloads = 0x01,
    GameStart     = 0x02,
    PreFrame      = 0x03,
    PostFrame     = 0x04,
    GameEnd       = 0x05,
};

struct GameStartPortInfo
{
    uint8_t slotType; // 0 human, 1 CPU, 2 empty
    uint8_t characterId;
    uint8_t costumeId;
    uint8_t teamColor;
};
static_assert(sizeof(GameStartPortInfo) == 4, "GameStartPortInfo must be 4 bytes");

struct GameStartEvent
{
    uint8_t           stageId;
    uint8_t           gameType;
    uint8_t           stockCountSetting;
    uint8_t           timeLimitMinutes;
    uint8_t           damageRatio;
    uint8_t           itemFrequency;
    GameStartPortInfo ports[4];
    char              playerNames[4][32];
    // v1 field-append (docs/RMGR_SPEC.md section 5): everything above this
    // line is the original 150-byte v1 layout, untouched. New fields are
    // appended here, never inserted earlier, so an old parser reading a
    // new file still sees the original layout intact at its original
    // offsets and just skips these trailing bytes via the size EventPayloads
    // declares for this event.
    uint8_t           teamsEnabled;    // 0 off, 1 on
    uint8_t           handicapMode;    // 0 off, 1 on, 2 auto
    uint8_t           portTeam[4];     // team number per port, index = port
    uint8_t           portHandicap[4]; // per-port handicap value (meaningful when handicapMode != 0)
    uint8_t           portCpuLevel[4]; // CPU difficulty per port (meaningless for human ports)
};
static_assert(sizeof(GameStartEvent) == 164, "GameStartEvent must be 164 bytes");

struct PreFrameEvent
{
    int32_t  frame;
    uint8_t  port;
    uint16_t buttons;
    int8_t   stickX;
    int8_t   stickY;
};
static_assert(sizeof(PreFrameEvent) == 9, "PreFrameEvent must be 9 bytes");

struct PostFrameEvent
{
    int32_t  frame;
    uint8_t  port;
    uint8_t  characterId;
    uint16_t actionStateId;
    float    positionX;
    float    positionY;
    int32_t  facingDirection;
    float    velocityX;
    float    velocityY;
    uint32_t damagePercent;
    int8_t   stocksRemaining;
    uint8_t  jumpsUsed;
    uint8_t  groundedState;
    uint8_t  hurtboxState;
    uint16_t hitstunCounter;
    uint32_t actionFrameCounter;
    // v1 field-append (docs/RMGR_SPEC.md section 5): everything above this
    // line is the original 42-byte v1 layout, untouched.
    //
    // Native engine combo tracking, not mod-added - tracked with the combo
    // meter display toggle off too, and Smash Remix additionally keeps the
    // chain alive across grabs/wall-bounces/tech-chases where vanilla would
    // reset it (see smashremix docs/ram-map.md section 13). Belongs to the
    // victim, not the attacker: how many hits THIS port has taken in its
    // current unbroken chain. 0 = no active chain, 1 = a single hit (not
    // yet a "combo" by convention), 2+ = an actual combo. Both zero the
    // instant the chain breaks - so a reader can count "neutral hits taken
    // this stock" by counting comboHitCount's 0->nonzero transitions.
    uint32_t comboHitCount;
    uint32_t comboDamage;
};
static_assert(sizeof(PostFrameEvent) == 50, "PostFrameEvent must be 50 bytes");

struct GameEndEvent
{
    uint8_t endReason; // 0 aborted (match-was-reset or process/emulation stopped mid-match), 1 normal end
    int8_t  placements[4]; // final stocks remaining per port, -1 if never seated
};
static_assert(sizeof(GameEndEvent) == 5, "GameEndEvent must be 5 bytes");

#pragma pack(pop)

enum class State
{
    Idle,            // feature disabled for this emulation session
    WaitingForMatch, // enabled, no file open, watching for a VS match to start
    Recording,       // file open, writing Pre/Post-Frame events every frame
};

State         s_State = State::Idle;
std::ofstream s_File;
int32_t       s_FrameNumber = 0;
uint32_t      s_StreamBytesWritten = 0;

// Guards s_State/s_File (and, transitively, everything the helpers below
// touch). OnFrame() runs on the emulation thread while OnEmulationStop()
// is always called from the UI thread (via CoreStopEmulation() and, for
// paths that don't reach that, MainWindow::on_Emulation_Finished), so
// without this a quit-mid-match can have both threads touching s_File at
// once. Locked at the top of each of the 3 public entry points; every
// static helper here is only ever reached through one of those.
std::mutex s_Mutex;

// Per-launch override for OnEmulationStart(), set via Replay::SetEnabledOverride().
// Consumed (cleared) the next time OnEmulationStart() runs - see that
// function and SetEnabledOverride's own doc comment in Replay.hpp.
bool s_HasOverride = false;
bool s_OverrideValue = false;

// This feature's memory offsets were only ever derived/verified against
// Smash Remix 2.0.1 (see docs/RMGR_SPEC.md); recording against any other
// ROM would pointer-chase addresses that mean nothing there. GoodName
// comes from mupen64plus-core's own ROM database (CoreRomSettings::GoodName,
// via CoreGetCurrentRomSettings()) - for a ROM/hack absent from that
// database it degrades to a filename-derived value, so this exact-match
// check can only ever be as reliable as that database entry.
bool IsSupportedGame(void)
{
    CoreRomSettings romSettings;
    if (!CoreGetCurrentRomSettings(romSettings))
    {
        return false;
    }
    return romSettings.GoodName == "SmashRemix2.0.1";
}

template <typename T>
void WriteEvent(EventCode code, const T& payload)
{
    uint8_t codeByte = static_cast<uint8_t>(code);
    s_File.write(reinterpret_cast<const char*>(&codeByte), sizeof(codeByte));
    s_File.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
    s_StreamBytesWritten += static_cast<uint32_t>(sizeof(codeByte) + sizeof(payload));
}

// The Event Payloads event (0x01) is always first: it declares the exact
// payload size of every other event code this file uses, so a future
// parser reading an unfamiliar/old-version file can skip unknown or
// resized events instead of breaking. See handoff doc section 4.2/4.6.
void WriteEventPayloadsEvent(void)
{
    struct EventSize
    {
        EventCode code;
        uint16_t  size;
    };
    static const EventSize sizes[] = {
        {EventCode::GameStart, static_cast<uint16_t>(sizeof(GameStartEvent))},
        {EventCode::PreFrame,  static_cast<uint16_t>(sizeof(PreFrameEvent))},
        {EventCode::PostFrame, static_cast<uint16_t>(sizeof(PostFrameEvent))},
        {EventCode::GameEnd,   static_cast<uint16_t>(sizeof(GameEndEvent))},
    };

    uint8_t codeByte = static_cast<uint8_t>(EventCode::EventPayloads);
    uint8_t count = static_cast<uint8_t>(sizeof(sizes) / sizeof(sizes[0]));
    s_File.write(reinterpret_cast<const char*>(&codeByte), sizeof(codeByte));
    s_File.write(reinterpret_cast<const char*>(&count), sizeof(count));
    s_StreamBytesWritten += sizeof(codeByte) + sizeof(count);

    for (const EventSize& entry : sizes)
    {
        uint8_t  entryCode = static_cast<uint8_t>(entry.code);
        uint16_t entrySize = entry.size;
        s_File.write(reinterpret_cast<const char*>(&entryCode), sizeof(entryCode));
        s_File.write(reinterpret_cast<const char*>(&entrySize), sizeof(entrySize));
        s_StreamBytesWritten += sizeof(entryCode) + sizeof(entrySize);
    }
}

std::string SanitizeForFilename(const std::string& input)
{
    std::string result = input.substr(0, 24);
    for (char& c : result)
    {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
        {
            c = '_';
        }
    }
    return result;
}

// Mirrors n02_client.cpp's "YYMMDDHHMMSS-Player1-Player2.krec" convention
// (section 3.1 of the handoff doc), simplified: no MAX_PATH budget
// juggling, each name flatly capped at 24 chars.
std::string BuildFileName(void)
{
    time_t now = time(nullptr);
    tm localNow{};
#ifdef _WIN32
    localtime_s(&localNow, &now);
#else
    localtime_r(&now, &localNow);
#endif
    char datePart[16];
    strftime(datePart, sizeof(datePart), "%y%m%d%H%M%S", &localNow);

    std::string filename = datePart;

#ifdef RMGK_HAVE_P2P_TRANSPORT
    for (int i = 0; i < 4; i++)
    {
        if (recording_player_names[i][0] != 0)
        {
            filename += "-";
            filename += SanitizeForFilename(recording_player_names[i]);
        }
    }
#endif

    filename += ".rmgr";
    return filename;
}

// Returns true if the file was opened and the header/GameStart event were
// written successfully; false if the caller should not transition into the
// Recording state (e.g. open() failed).
bool OpenNewFile(const ReplayMemory::MatchInfo& matchInfo)
{
    // Reset regardless of whether the open below succeeds, so a failed
    // open never leaves stale counts around for the next attempt.
    s_FrameNumber = 0;
    s_StreamBytesWritten = 0;

    // Bare relative path, resolved by CWD - deliberately mirrors krec's own
    // "records" directory convention (Source/n02/n02_client.cpp) exactly,
    // so .rmgr files land next to .krec files at the top level instead of
    // being nested under the per-platform user-data directory.
    std::filesystem::path directory("replays");
    std::filesystem::create_directories(directory);

    std::filesystem::path path = directory / BuildFileName();
    s_File.open(path, std::ios::binary | std::ios::trunc);
    if (!s_File.is_open())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "Replay: failed to open " + path.string() + " for recording");
        return false;
    }

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        "Replay: recording to " + path.string());

    FileHeader header{};
    std::memcpy(header.magic, "RMGR", 4);
    header.version = 1;
    header.streamLength = 0;
    s_File.write(reinterpret_cast<const char*>(&header), sizeof(header));

    WriteEventPayloadsEvent();

    GameStartEvent startEvent{};
    startEvent.stageId           = matchInfo.stageId;
    startEvent.gameType          = matchInfo.gameType;
    startEvent.stockCountSetting = matchInfo.stockCountSetting;
    startEvent.timeLimitMinutes  = matchInfo.timeLimitMinutes;
    startEvent.damageRatio       = matchInfo.damageRatio;
    startEvent.itemFrequency     = matchInfo.itemFrequency;
    startEvent.teamsEnabled      = matchInfo.teamsEnabled ? 1 : 0;
    startEvent.handicapMode      = matchInfo.handicapMode;

    for (int port = 0; port < 4; port++)
    {
        ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
        startEvent.ports[port].slotType    = portInfo.slotType;
        startEvent.ports[port].characterId = portInfo.characterId;
        startEvent.ports[port].costumeId   = portInfo.costumeId;
        startEvent.ports[port].teamColor   = portInfo.teamColor;

        // team/handicap/cpuLevel need the player-object/player-struct chase,
        // which can be unpopulated if the file opened during the pre-match
        // countdown (game_status == 0) before characters have spawned.
        // Left at their zero-initialized default in that case - same
        // tolerant "not currently available" handling as everywhere else
        // in this file, not a crash.
        ReplayMemory::PortPlayerState playerState = ReplayMemory::ReadPortPlayerState(matchInfo.matchInfoPtr, port);
        if (playerState.valid)
        {
            startEvent.portTeam[port]     = playerState.team;
            startEvent.portHandicap[port] = playerState.handicap;
            startEvent.portCpuLevel[port] = playerState.cpuLevel;
        }
    }

#ifdef RMGK_HAVE_P2P_TRANSPORT
    std::memcpy(startEvent.playerNames, recording_player_names, sizeof(startEvent.playerNames));
#endif

    WriteEvent(EventCode::GameStart, startEvent);

    return true;
}

void FinalizeFile(uint8_t endReason, const ReplayMemory::MatchInfo& matchInfo)
{
    if (!s_File.is_open())
    {
        return;
    }

    GameEndEvent endEvent{};
    endEvent.endReason = endReason;
    for (int port = 0; port < 4; port++)
    {
        ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
        endEvent.placements[port] = portInfo.seated ? portInfo.stocksRemaining : -1;
    }
    WriteEvent(EventCode::GameEnd, endEvent);

    s_File.flush();
    s_File.seekp(offsetof(FileHeader, streamLength));
    uint32_t length = s_StreamBytesWritten;
    s_File.write(reinterpret_cast<const char*>(&length), sizeof(length));
    s_File.close();
}

void RecordFrame(const ReplayMemory::MatchInfo& matchInfo)
{
    for (int port = 0; port < 4; port++)
    {
        ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
        if (!portInfo.seated)
        {
            continue;
        }

        ReplayMemory::PortPlayerState state = ReplayMemory::ReadPortPlayerState(matchInfo.matchInfoPtr, port);
        if (!state.valid)
        {
            continue;
        }

        PreFrameEvent pre{};
        pre.frame   = s_FrameNumber;
        pre.port    = static_cast<uint8_t>(port);
        pre.buttons = state.processedButtons;
        pre.stickX  = state.stickX;
        pre.stickY  = state.stickY;
        WriteEvent(EventCode::PreFrame, pre);

        PostFrameEvent post{};
        post.frame           = s_FrameNumber;
        post.port             = static_cast<uint8_t>(port);
        post.characterId       = state.characterId;
        post.actionStateId      = state.actionStateId;
        post.positionX           = state.positionX;
        post.positionY            = state.positionY;
        post.facingDirection       = state.facingDirection;
        post.velocityX              = state.velocityX;
        post.velocityY               = state.velocityY;
        post.damagePercent            = state.damagePercent;
        post.stocksRemaining           = portInfo.stocksRemaining;
        post.jumpsUsed                  = static_cast<uint8_t>(state.jumpsUsed);
        post.groundedState               = state.groundedState;
        post.hurtboxState                 = state.hurtboxState;
        post.hitstunCounter                = state.hitstunCounter;
        post.actionFrameCounter             = state.actionFrameCounter;
        post.comboHitCount                   = portInfo.comboHitCount;
        post.comboDamage                      = portInfo.comboDamage;
        WriteEvent(EventCode::PostFrame, post);
    }

    s_FrameNumber++;
}
} // namespace

namespace Replay
{
CORE_EXPORT void OnEmulationStart(void)
{
    std::lock_guard<std::mutex> lock(s_Mutex);

    // Defensively handle a leftover open file from an abnormal prior
    // session end (e.g. OnEmulationStop() never ran on that session).
    // open()'ing an already-open ofstream just sets failbit and leaves
    // the old stream open/is_open()==true, which would otherwise silently
    // disable recording for the rest of the process.
    if (s_File.is_open())
    {
        s_File.close();
        s_File.clear();
    }

    bool enabled;
    if (s_HasOverride)
    {
        enabled = s_OverrideValue;
        s_HasOverride = false; // consumed - see SetEnabledOverride's doc comment
    }
    else
    {
        enabled = CoreSettingsGetBoolValue(SettingsID::GameStats_ReplayEnabled);
    }

    if (enabled && !IsSupportedGame())
    {
        // Every offset in ReplayMemory.cpp was only ever derived/verified
        // against Smash Remix 2.0.1 - recording against anything else would
        // pointer-chase addresses that mean nothing there. Log this
        // specifically rather than silently doing nothing, since "the
        // checkbox was on but nothing happened" is otherwise indistinguishable
        // from "never reached a VS match".
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
            "Replay: enabled, but the loaded ROM isn't Smash Remix 2.0.1 - not recording");
        enabled = false;
    }

    s_State = enabled ? State::WaitingForMatch : State::Idle;
    if (enabled)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
            "Replay: enabled, watching for a match to start");
    }
    s_FrameNumber = 0;
    s_StreamBytesWritten = 0;
}

CORE_EXPORT void SetEnabledOverride(bool enabled)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_HasOverride = true;
    s_OverrideValue = enabled;
}

CORE_EXPORT void OnEmulationStop(void)
{
    std::lock_guard<std::mutex> lock(s_Mutex);

    if (s_State == State::Recording)
    {
        ReplayMemory::MatchInfo matchInfo = ReplayMemory::ReadMatchInfo();
        FinalizeFile(0 /* aborted: emulation stopped mid-match */, matchInfo);
    }
    s_State = State::Idle;
}

CORE_EXPORT void OnFrame(void)
{
    std::lock_guard<std::mutex> lock(s_Mutex);

    if (s_State == State::Idle)
    {
        return;
    }

    if (!ReplayMemory::IsInVsMatchScreen())
    {
        if (s_State == State::Recording)
        {
            ReplayMemory::MatchInfo matchInfo = ReplayMemory::ReadMatchInfo();
            FinalizeFile(0 /* aborted: left the VS match screen unexpectedly */, matchInfo);
        }
        s_State = State::WaitingForMatch;
        return;
    }

    ReplayMemory::MatchInfo matchInfo = ReplayMemory::ReadMatchInfo();
    if (!matchInfo.valid)
    {
        return;
    }

    if (s_State == State::WaitingForMatch)
    {
        if (matchInfo.gameStatus == 0 || matchInfo.gameStatus == 1)
        {
            if (OpenNewFile(matchInfo))
            {
                s_State = State::Recording;
            }
            // else: stay in WaitingForMatch and retry next frame.
        }
        return;
    }

    // s_State == State::Recording
    if (matchInfo.gameStatus == 5)
    {
        uint8_t endReason = matchInfo.matchWasReset ? 0 : 1;
        FinalizeFile(endReason, matchInfo);
        s_State = State::WaitingForMatch;
        return;
    }

    if (matchInfo.gameStatus == 1)
    {
        RecordFrame(matchInfo);
    }
}
} // namespace Replay
