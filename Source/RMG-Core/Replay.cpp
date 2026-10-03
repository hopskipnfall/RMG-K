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
#include "ReplayEvents.hpp"
#include "ReplayEventBuilder.hpp"
#include "Settings.hpp"
#include "Callback.hpp"
#include "File.hpp"
#include "Library.hpp"
#include "RomSettings.hpp"
#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "kailleraclient.h"
#endif

#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace ReplayEvents;

#pragma pack(push, 1)

// v5, ground-up rewrite - see docs/RMGR_SPEC.md. Breaking change from
// everything recorded before this document existed: no migration path, none
// planned. `version` keeps incrementing (this fork's prior in-development
// numbering reached 4) rather than resetting to 1, purely so a reader that
// only understands the old, unspecified layout sees an unfamiliar number and
// correctly refuses to parse, instead of the value colliding with an
// unrelated earlier meaning.
struct FileHeader
{
    char     magic[4];        // "RMGR"
    uint8_t  version;         // 5
    uint8_t  reserved[3];     // zero
    // Which game-family extension event set (below) applies - a coarser,
    // slower-growing identity than goodName. Empty (all zero) if the loaded
    // ROM isn't recognized: the file is still a fully valid core-only
    // recording in that case, just with no extension events. See
    // docs/RMGR_SPEC.md section 2.1/3.2.
    char     gameFamily[16];
    // Exact ROM build identity (mupen64plus-core's own ROM database string) -
    // distinct from gameFamily: two different goodNames can share one family
    // (e.g. a future vanilla-SSB64 recorder alongside Smash Remix, both
    // "smash64"), each with its own recorderSchemaVersion numbering space.
    char     goodName[64];
    uint32_t recorderSchemaVersion; // see kRecorderSchemaVersion below; 0 when gameFamily is empty
    uint64_t recordedAtEpochMillis; // milliseconds since the Unix epoch (UTC)
    // Byte length of the event stream after decompression - lets a reader
    // preallocate its output buffer instead of growing it dynamically.
    uint32_t uncompressedLength;
    // Byte length of the deflate-compressed block immediately following this
    // header. Always correct on disk: the whole match is buffered in memory
    // and compressed once, at match end (see s_EventBuffer below), so there
    // is no "0 until finalized, patched via seek" convention to speak of -
    // and, as an accepted trade-off, a crash or force-quit mid-match now
    // produces no file at all rather than a truncated one.
    uint32_t compressedLength;
};
static_assert(sizeof(FileHeader) == 108, "FileHeader must be 108 bytes");


#pragma pack(pop)

enum class State
{
    Idle,            // feature disabled for this emulation session
    WaitingForMatch, // enabled, no match currently being recorded, watching for one to start
    Recording,       // buffering Input/StateFrame events every frame for the in-progress match
};

State                s_State = State::Idle;
int32_t              s_FrameNumber = 0;
// Previous frame's ReplayMemory::IsSaltyRunbackActive() reading, while
// s_State == Recording - see OnFrame()'s own doc comment on why this is
// edge-detected (0->1 transition) rather than a level check, and why it's
// re-baselined to the current value every time a new recording starts
// rather than reset to false.
bool                 s_PrevSaltyRunbackActive = false;
// Everything for the in-progress match accumulates here instead of being
// streamed to disk incrementally - see docs/RMGR_SPEC.md section 2 for the
// buffered/compressed-once rationale and its accepted crash-safety
// trade-off. Cleared in OpenNewFile(); written out (compressed, once) in
// FinalizeFile().
std::vector<uint8_t> s_EventBuffer;
bool                 s_HasPendingRecording = false; // true between a successful OpenNewFile() and the matching FinalizeFile()
std::filesystem::path s_PendingOutputPath;
// Whether the loaded ROM's game family is recognized for THIS pending
// recording - gates every smash64 extension event (StateFrame/ItemUpdate/
// StageHazardUpdate/MatchSettings/MatchResult); the core events (MatchStart/
// InputFrame/MatchEnd) are written unconditionally. Cached at OpenNewFile()
// time rather than re-checked every frame, since the loaded ROM can't change
// mid-session.
bool                 s_FamilyRecognized = false;
FileHeader           s_PendingHeader{};

// Guards all of the above (and, transitively, everything the helpers below
// touch). OnFrame() runs on the emulation thread while OnEmulationStop() is
// always called from the UI thread (via CoreStopEmulation() and, for paths
// that don't reach that, MainWindow::on_Emulation_Finished), so without this
// a quit-mid-match can have both threads touching shared state at once.
// Locked at the top of each of the 3 public entry points; every static
// helper here is only ever reached through one of those.
std::mutex s_Mutex;

// Per-launch override for OnEmulationStart(), set via Replay::SetEnabledOverride().
// Consumed (cleared) the next time OnEmulationStart() runs - see that
// function and SetEnabledOverride's own doc comment in Replay.hpp.
bool s_HasOverride = false;
bool s_OverrideValue = false;

// Per-session output path override, set via Replay::SetOutputPathOverride().
// Applies to every OpenNewFile() call for the rest of this session; cleared
// at OnEmulationStop() - see that function and SetOutputPathOverride's own
// doc comment in Replay.hpp.
bool        s_HasOutputPathOverride = false;
std::string s_OutputPathOverride;
// Which numbered match this session is on for the override path (see
// OpenNewFile()) - reset to 0 whenever a new override is set.
int s_OverrideMatchNumber = 0;

// Per-session playerNames override, set via Replay::SetPlayerNamesOverride().
// Applies to every OpenNewFile() call for the rest of this session; cleared
// at OnEmulationStop() - see that function and SetPlayerNamesOverride's own
// doc comment in Replay.hpp.
bool                       s_HasPlayerNamesOverride = false;
std::array<std::string, 4> s_PlayerNamesOverride;

// Per-session recordedAtEpochMillis override, set via
// Replay::SetRecordedAtBaseOverride(). Applies to every OpenNewFile() call
// for the rest of this session; cleared at OnEmulationStop() - see that
// function and SetRecordedAtBaseOverride's own doc comment in Replay.hpp.
// Still stored as whole seconds - that's all the .krec format this is
// derived from actually has (see OpenNewFile(), which converts to
// milliseconds when combining it with the frame-derived offset).
bool                          s_HasRecordedAtBaseOverride = false;
uint64_t                      s_RecordedAtBaseEpochSeconds = 0;
Replay::FrameIndexProvider    s_RecordedAtFrameIndexProvider = nullptr;


// Whether the currently-loaded ROM is a recognized smash64-family build.
// Only Smash Remix 2.0.1 is recognized today - see kSmashRemixGoodName's
// doc comment. Returns "" (not recognized - the recording, if any, stays
// core-only) or kSmash64Family.
//
// NOTE: this only decides whether the smash64 EXTENSION layer gets written.
// The recording *trigger* itself (OnFrame()'s state machine below) still
// depends entirely on ReplayMemory::IsInVsMatchScreen()/ReadMatchInfo(),
// which are themselves Smash-specific memory reads - there is currently no
// game-agnostic "a match is happening, here are this port's inputs" source
// wired into this recorder (RMG-K's PIF-level controller sync, used
// elsewhere for Kaillera netplay, would be the natural one). In practice
// this means core-only recording for an unrecognized game isn't reachable
// yet even though the wire format (docs/RMGR_SPEC.md section 2.1) already
// supports it - a real gap to close in a follow-up, not something this pass
// claims to have finished.
std::string DetermineGameFamily(const CoreRomSettings& romSettings)
{
    if (romSettings.GoodName == kSmashRemixGoodName)
    {
        return kSmash64Family;
    }
    return "";
}

// zlib deflate, max compression level - see docs/RMGR_SPEC.md section 3.4.
// Compression only ever runs once per match, at FinalizeFile() time, not
// per-frame, so the cost of the highest level is a non-issue. Returns an
// empty vector on failure (extremely unlikely - compressBound() already
// sizes the destination generously); the caller must check for that and
// skip writing a file rather than write a bogus one.
std::vector<uint8_t> DeflateCompress(const std::vector<uint8_t>& input)
{
    uLongf boundLen = compressBound(static_cast<uLong>(input.size()));
    std::vector<uint8_t> output(boundLen);
    uLongf actualLen = boundLen;

    const Bytef* sourcePtr = input.empty() ? nullptr : reinterpret_cast<const Bytef*>(input.data());
    int result = compress2(output.data(), &actualLen, sourcePtr,
        static_cast<uLong>(input.size()), Z_BEST_COMPRESSION);
    if (result != Z_OK)
    {
        return {};
    }

    output.resize(actualLen);
    return output;
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

// Resolves the effective player names for this match: the per-session
// override (see SetPlayerNamesOverride) if one was set, otherwise the
// netplay room's own recording_player_names global. Used for BOTH the
// filename suffix (BuildFileName below) and MatchStart.playerNames, so the
// two can never disagree about whose names these are - previously
// BuildFileName read recording_player_names directly and ignored the
// override entirely, so headless .krec export produced files with correct
// in-header names but no player-name suffix on disk.
std::array<std::string, 4> ResolvePlayerNames(void)
{
    if (s_HasPlayerNamesOverride)
    {
        return s_PlayerNamesOverride;
    }

    std::array<std::string, 4> names{};
#ifdef RMGK_HAVE_P2P_TRANSPORT
    for (int i = 0; i < 4; i++)
    {
        names[i] = recording_player_names[i];
    }
#endif
    return names;
}

// Loosely mirrors n02_client.cpp's "<date>-Player1-Player2.krec" convention
// - player-name suffix and 24-char cap the same, but YYYYMMDD-HHMMSS
// (4-digit year, dashed) rather than krec's compact YYMMDDHHMMSS, for a
// name that reads as a timestamp at a glance. `now` is passed in (rather
// than this function calling time(nullptr) itself) so the caller can derive
// it from the exact same instant it writes into the file's own
// recordedAtEpochMillis header field - the filename and the header should
// never disagree about when the recording started, even though the
// filename itself is only second-resolution.
std::string BuildFileName(time_t now, const std::array<std::string, 4>& playerNames)
{
    tm localNow{};
#ifdef _WIN32
    localtime_s(&localNow, &now);
#else
    localtime_r(&now, &localNow);
#endif
    char datePart[16];
    strftime(datePart, sizeof(datePart), "%Y%m%d-%H%M%S", &localNow);

    std::string filename = datePart;

    for (const std::string& name : playerNames)
    {
        if (!name.empty())
        {
            filename += "-";
            filename += SanitizeForFilename(name);
        }
    }

    filename += ".rmgr";
    return filename;
}

// Resets per-match state and buffers the header/MatchStart(/MatchSettings)
// events in memory - nothing touches disk here. Returns true if recording
// should proceed (the caller transitions to State::Recording); false if the
// caller should stay in WaitingForMatch and retry next frame (e.g. the
// output directory couldn't be created).
bool OpenNewFile(const ReplayMemory::MatchInfo& matchInfo)
{
    s_FrameNumber = 0;
    s_EventBuffer.clear();
    // A rough head start on capacity so the first ~1000 frames' worth of
    // events (well past a typical short stock) don't force repeated
    // reallocate+copy growth; the buffer still grows geometrically beyond
    // this for a longer match, same as any std::vector.
    s_EventBuffer.reserve(256 * 1024);
    s_HasPendingRecording = false;

    // Default: live recording, stamped with wall-clock "now" - read from
    // system_clock (not time(nullptr)) for millisecond precision. Headless
    // .krec export overrides this (see SetRecordedAtBaseOverride's doc
    // comment) so the file reflects the match's *original* recording time
    // rather than when the headless replay (which can run at up to 2000%
    // speed) happened to reach it.
    std::chrono::system_clock::time_point nowTimePoint;
    if (s_HasRecordedAtBaseOverride)
    {
        const int elapsedFrames = s_RecordedAtFrameIndexProvider != nullptr ? s_RecordedAtFrameIndexProvider() : 0;
        // Milliseconds, not truncated whole seconds, so multiple matches
        // recorded from the same .krec land at distinguishable,
        // frame-accurate (~16.67ms) offsets from krecBaseEpochSeconds
        // instead of all bucketing into whichever whole second they
        // happened to start in.
        const int64_t elapsedMillis = static_cast<int64_t>(std::llround(elapsedFrames * 1000.0 / 60.0));
        const int64_t baseMillis = static_cast<int64_t>(s_RecordedAtBaseEpochSeconds) * 1000;
        nowTimePoint = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(baseMillis) + std::chrono::milliseconds(elapsedMillis));
    }
    else
    {
        nowTimePoint = std::chrono::system_clock::now();
    }
    const uint64_t nowEpochMillis = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nowTimePoint.time_since_epoch()).count());
    // BuildFileName()'s date part is still just local wall-clock seconds -
    // see docs/RMGR_SPEC.md section 3.3.
    const time_t now = static_cast<time_t>(nowEpochMillis / 1000);

    const std::array<std::string, 4> playerNames = ResolvePlayerNames();

    std::filesystem::path path;
    if (s_HasOutputPathOverride)
    {
        // Not consumed here - see SetOutputPathOverride's doc comment. Every
        // match this session gets its own explicitly-numbered file -
        // "<override>-1.ext", "<override>-2.ext", ... - not just
        // "<override>.ext" for the first, so a multi-game .krec's export
        // still reads as "<krec name>-<game number>.rmgr" and plainly
        // corresponds back to its source .krec by name.
        // CoreFindCollisionFreePath() is still a safety net in case this
        // same krec was already exported before (so "-1" is already taken).
        s_OverrideMatchNumber++;
        const std::filesystem::path base(s_OutputPathOverride);
        const std::filesystem::path numberedPath = base.parent_path() /
            (base.stem().string() + "-" + std::to_string(s_OverrideMatchNumber) + base.extension().string());
        path = CoreFindCollisionFreePath(numberedPath);
    }
    else
    {
        // Bare relative path, resolved by CWD - deliberately mirrors krec's
        // own "records" directory convention (Source/n02/n02_client.cpp)
        // exactly, so .rmgr files land next to .krec files at the top level
        // instead of being nested under the per-platform user-data directory.
        path = CoreFindCollisionFreePath(std::filesystem::path("replays") / BuildFileName(now, playerNames));
    }

    std::error_code createDirErrorCode;
    std::filesystem::create_directories(path.parent_path(), createDirErrorCode);
    if (createDirErrorCode)
    {
        if (s_HasOutputPathOverride)
        {
            s_OverrideMatchNumber--; // undo - see the increment above, this attempt never happened
        }
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "Replay: failed to create directory for " + path.string() + " - not recording");
        return false;
    }

    // Test-open (and immediately close) the actual destination now, before
    // buffering a single event - a match's worth of data can be several MB,
    // and the buffered/compressed-once design (see FinalizeFile()) only
    // writes it out once the match ends, so a write failure discovered only
    // then would silently lose the whole recording instead of never having
    // started it. This intentionally leaves a truncated (0-byte) file at
    // `path` if the match never finishes - a much smaller regression than
    // losing a full match, and FinalizeFile() overwrites it with the real
    // content (also truncating) on success.
    {
        std::ofstream testOpen(path, std::ios::binary | std::ios::trunc);
        if (!testOpen.is_open())
        {
            if (s_HasOutputPathOverride)
            {
                s_OverrideMatchNumber--; // undo - see the increment above, this attempt never happened
            }
            CoreAddCallbackMessage(CoreDebugMessageType::Warning,
                "Replay: failed to open " + path.string() + " for recording");
            return false;
        }
    }
    s_PendingOutputPath = path;

    // Reaching here already implies replay recording is enabled (see
    // OnEmulationStart()) - this is just re-fetching the loaded ROM's
    // identity to decide whether the smash64 extension layer applies. The
    // loaded ROM can't change mid-session.
    CoreRomSettings romSettings;
    CoreGetCurrentRomSettings(romSettings);
    const std::string gameFamily = DetermineGameFamily(romSettings);
    s_FamilyRecognized = !gameFamily.empty();

    s_PendingHeader = FileHeader{};
    std::memcpy(s_PendingHeader.magic, "RMGR", 4);
    s_PendingHeader.version = 5;
    WriteFixedString(s_PendingHeader.gameFamily, sizeof(s_PendingHeader.gameFamily), gameFamily);
    WriteFixedString(s_PendingHeader.goodName, sizeof(s_PendingHeader.goodName), romSettings.GoodName);
    s_PendingHeader.recorderSchemaVersion = s_FamilyRecognized ? kRecorderSchemaVersion : 0;
    s_PendingHeader.recordedAtEpochMillis = nowEpochMillis;
    // uncompressedLength/compressedLength are filled in by FinalizeFile()
    // once the whole match's events are known.

    ReplayEventBuilder::AppendEventPayloadsEvent(s_EventBuffer, s_FamilyRecognized);
    ReplayEventBuilder::AppendMatchStart(s_EventBuffer, matchInfo, playerNames);

    if (s_FamilyRecognized)
    {
        ReplayEventBuilder::AppendMatchSettings(s_EventBuffer, matchInfo);
    }

    s_HasPendingRecording = true;
    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        "Replay: recording match, will write to " + s_PendingOutputPath.string() + " once it ends");
    return true;
}

// Compresses `eventBuffer` and writes `header` + the compressed blob to
// `outputPath` - the actual disk I/O for a finished match. Deliberately a
// free function taking everything by value/move rather than touching any
// s_* state: it runs on a detached worker thread (see FinalizeFile()) so
// compressing a multi-MB buffer and writing it out never blocks the caller
// of OnEmulationStop()/OnFrame() - frequently the UI thread via
// CoreStopEmulation(). Needs no locking of its own since nothing it touches
// is shared with any other thread.
void CompressAndWriteFile(std::vector<uint8_t> eventBuffer, FileHeader header, std::filesystem::path outputPath)
{
    const std::vector<uint8_t> compressed = DeflateCompress(eventBuffer);
    if (compressed.empty() && !eventBuffer.empty())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "Replay: failed to compress recorded match data - not writing " + outputPath.string());
        return;
    }

    header.uncompressedLength = static_cast<uint32_t>(eventBuffer.size());
    header.compressedLength   = static_cast<uint32_t>(compressed.size());

    std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "Replay: failed to open " + outputPath.string() + " for writing");
        return;
    }

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!compressed.empty())
    {
        file.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
    }
    file.close();

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        "Replay: wrote " + outputPath.string() + " (" +
        std::to_string(compressed.size()) + " bytes compressed, " +
        std::to_string(eventBuffer.size()) + " bytes uncompressed)");
}

void FinalizeFile(uint8_t endReason, const ReplayMemory::MatchInfo& matchInfo)
{
    if (!s_HasPendingRecording)
    {
        return;
    }

    ReplayEventBuilder::AppendMatchEnd(s_EventBuffer, s_FrameNumber > 0 ? (s_FrameNumber - 1) : 0, endReason,
                                       matchInfo, s_FamilyRecognized);

    // Hand the buffer off to a detached worker thread for compression +
    // writing (see CompressAndWriteFile()'s doc comment) and reset our own
    // state immediately - the caller (often the UI thread) doesn't wait for
    // either to finish.
    std::thread(CompressAndWriteFile, std::move(s_EventBuffer), s_PendingHeader, s_PendingOutputPath).detach();

    s_HasPendingRecording = false;
    s_EventBuffer.clear();
}

void RecordFrame(const ReplayMemory::MatchInfo& matchInfo)
{
    ReplayEventBuilder::AppendFrameEvents(s_EventBuffer, s_FrameNumber, matchInfo, s_FamilyRecognized);
    s_FrameNumber++;
}
} // namespace

namespace Replay
{
CORE_EXPORT void OnEmulationStart(void)
{
    std::lock_guard<std::mutex> lock(s_Mutex);

    // Defensively drop any leftover buffered-but-unwritten recording from
    // an abnormal prior session end (e.g. OnEmulationStop() never ran on
    // that session) - matches this project's accepted "a crash mid-match
    // loses the recording" trade-off (docs/RMGR_SPEC.md section 2) rather
    // than trying to resurrect it here.
    s_HasPendingRecording = false;
    s_EventBuffer.clear();

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

    // NOTE: this is a coarser gate than the format itself supports - see
    // DetermineGameFamily()'s doc comment for why core-only recording of an
    // unrecognized game isn't actually reachable yet (the recording
    // trigger below is itself a Smash-specific memory read). Once that gap
    // is closed, this early-out should only skip the extension layer, not
    // recording entirely.
    if (enabled)
    {
        CoreRomSettings romSettings;
        if (!CoreGetCurrentRomSettings(romSettings) || DetermineGameFamily(romSettings).empty())
        {
            CoreAddCallbackMessage(CoreDebugMessageType::Info,
                "Replay: enabled, but the loaded ROM isn't a recognized game - not recording");
            enabled = false;
        }
    }

    s_State = enabled ? State::WaitingForMatch : State::Idle;
    if (enabled)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
            "Replay: enabled, watching for a match to start");
    }
    s_FrameNumber = 0;
}

CORE_EXPORT void SetEnabledOverride(bool enabled)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_HasOverride = true;
    s_OverrideValue = enabled;
}

CORE_EXPORT void SetOutputPathOverride(const std::string& path)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_HasOutputPathOverride = true;
    s_OutputPathOverride = path;
    s_OverrideMatchNumber = 0;
}

CORE_EXPORT void SetPlayerNamesOverride(const std::array<std::string, 4>& names)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_HasPlayerNamesOverride = true;
    s_PlayerNamesOverride = names;
}

CORE_EXPORT void SetRecordedAtBaseOverride(uint64_t krecBaseEpochSeconds, FrameIndexProvider frameIndexProvider)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_HasRecordedAtBaseOverride = true;
    s_RecordedAtBaseEpochSeconds = krecBaseEpochSeconds;
    s_RecordedAtFrameIndexProvider = frameIndexProvider;
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

    // Both overrides apply for the whole session that just ended - every
    // OpenNewFile() call in between reused them (see SetOutputPathOverride/
    // SetPlayerNamesOverride's own doc comments) - so clear them now,
    // otherwise a later, unrelated session (e.g. a plain offline ROM launch
    // with no override call of its own) would silently inherit them.
    s_HasOutputPathOverride = false;
    s_OutputPathOverride.clear();
    s_OverrideMatchNumber = 0;
    s_HasPlayerNamesOverride = false;
    s_PlayerNamesOverride = {};
    s_HasRecordedAtBaseOverride = false;
    s_RecordedAtBaseEpochSeconds = 0;
    s_RecordedAtFrameIndexProvider = nullptr;
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
                // Re-baseline to whatever the flag currently reads, not
                // false - see IsSaltyRunbackActive()'s doc comment: if this
                // very match was itself opened by a runback (or two
                // runbacks happened back-to-back with the flag never
                // dropping to 0 in between), the flag may already be
                // non-zero right now. Starting from false here would
                // immediately misfire a spurious 0->1 "edge" on this
                // match's very first frame.
                s_PrevSaltyRunbackActive = ReplayMemory::IsSaltyRunbackActive();
            }
            // else: stay in WaitingForMatch and retry next frame.
        }
        return;
    }

    // s_State == State::Recording
    //
    // Smash Remix's Salty Runback (ReplayMemory::IsSaltyRunbackActive)
    // bypasses the normal end-of-match flow: it restarts play directly
    // from the no-contest/results screen without going through the
    // character-select/results transition IsInVsMatchScreen() above would
    // catch, and game_status's transition through 5 (ended) on the way
    // may be a single-frame spike this per-frame poll can miss entirely.
    // Detected here via a 0->1 *edge*, deliberately not a level check:
    // the flag likely stays set for this match's own entire replacement
    // match too (nothing clears it until that next match's own
    // end-of-match decision runs), so a level check would immediately -
    // and repeatedly, every frame - misfire on the match this runback
    // itself just started. Edge detection means two runbacks triggered
    // back-to-back with no non-runback match end between them may not be
    // independently caught - see docs/RMGR_SPEC.md's Known Limitations.
    const bool saltyRunbackActive = ReplayMemory::IsSaltyRunbackActive();
    const bool saltyRunbackTriggered = saltyRunbackActive && !s_PrevSaltyRunbackActive;
    s_PrevSaltyRunbackActive = saltyRunbackActive;

    if (matchInfo.gameStatus == 5 || saltyRunbackTriggered)
    {
        // matchWasReset is unrelated to Salty Runback (it's the pause-menu
        // abort combo - see ReplayMemory.cpp) and correctly reads false
        // here, so a runback-triggered close reports as a normal end (1) -
        // the match genuinely concluded, just via "no contest" rather than
        // a win/loss, a distinction MatchResult can't represent yet
        // regardless (see Known Limitations).
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
