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

namespace
{
#pragma pack(push, 1)

struct FileHeader
{
    char     magic[4];     // "RMGR"
    uint8_t  version;      // 4 - container/framing format only, see docs/RMGR_SPEC.md section 5
    uint8_t  reserved[3];  // zero
    uint32_t streamLength; // 0 while recording, patched to the real value at close
    // v2: which game produced this file, and which revision of this
    // recorder's understanding of that game's memory layout wrote it -
    // distinct axes, since a bugfix to an offset (or a newly-tracked field)
    // for the SAME goodName needs its own version bump even though the
    // container format itself hasn't changed. recorderSchemaVersion is its
    // own counter per goodName, not a global one - "SmashRemix2.0.1 schema
    // v3" and "SmashRemix2.0.2 schema v1" are unrelated numbering spaces.
    char     goodName[64];           // NUL-padded; not necessarily NUL-terminated if it fills the field. UTF-8.
    uint32_t recorderSchemaVersion;  // see IsSupportedGame()/kRecorderSchemaVersion below
    // v3/v4: wall-clock time this recording started, independent of the
    // filename (which is derived from local time - see BuildFileName() -
    // and can't be trusted to round-trip through arbitrary filesystems/
    // renames the way this field can). v4 replaced v3's whole-seconds
    // recordedAtEpochSeconds with recordedAtEpochMillis - see OpenNewFile()
    // and docs/RMGR_SPEC.md section 5.
    uint64_t recordedAtEpochMillis;  // milliseconds since the Unix epoch (UTC)
    // v4: nanosecond offset within recordedAtEpochMillis's millisecond, for
    // callers with a clock source finer than millisecond resolution - see
    // OpenNewFile(). 0 when the value being written wasn't read from a
    // clock directly (e.g. the .krec-derived override path).
    uint32_t recordedAtNanosOffset;
};
static_assert(sizeof(FileHeader) == 92, "FileHeader must be 92 bytes");

enum class EventCode : uint8_t
{
    EventPayloads = 0x01,
    GameStart     = 0x02,
    PreFrame      = 0x03,
    PostFrame     = 0x04,
    GameEnd       = 0x05,
    // v2/v3 (docs/RMGR_SPEC.md section 5, new event types - not a header
    // version bump): one per live Item/Weapon GObj, per frame. See
    // ItemUpdateEvent below.
    ItemUpdate        = 0x06,
    // v3: emitted only on frames where at least one tracked stage hazard is
    // active. See StageHazardUpdateEvent below.
    StageHazardUpdate = 0x07,
    // v5: one per currently-active hitbox slot, per frame. See
    // HitboxUpdateEvent below.
    HitboxUpdate      = 0x08,
    // v5: one per fighter hurtbox slot, per seated port, per frame. See
    // HurtboxUpdateEvent below.
    HurtboxUpdate     = 0x09,
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
    // Named/interpreted as jumpsUsed through schema v6 - that read a
    // constant 0 all game (wrong width AND wrong emulator byte-swizzle for
    // a sub-word read, see ReplayMemory::ReadPortPlayerState()). Schema v7
    // fixes the read and switches this to jumps *remaining* instead
    // (jumpsMax - jumpsUsed) - more directly useful, and what this project
    // wanted to export in the first place. Same wire position/size as
    // before - a pure "what this byte means" fix, not a layout change.
    uint8_t  jumpsRemaining;
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

// v2 new event type (docs/RMGR_SPEC.md section 5): one per live Item or
// Weapon GObj (ReplayMemory::ItemObject), per frame - zero or more of these
// follow each frame's Pre/Post-Frame pairs. "Weapon" is a free-flying
// character special-move projectile (boomerang, fireball, ...); "Item"
// covers thrown/spawned items and hazard objects, including some
// fighter-held things like Link's pulled bomb. See
// ReplayMemory::ItemObject's own doc comment and
// smashremix docs/ram-map.md section 10.4.
struct ItemUpdateEvent
{
    int32_t  frame;         // same numbering as Pre/PostFrame
    uint32_t objectAddress; // the object's own RDRAM address - not a semantic spawn ID, see ReplayMemory::ItemObject
    uint8_t  linkId;        // 4 = Item, 5 = Weapon - which enum `kind` below means (docs/RMGR_SPEC.md section 7.6)
    int32_t  kind;          // ITKind (linkId == 4) or WPKind (linkId == 5)
    float    positionX;
    float    positionY;
    float    positionZ; // confirmed exactly via the decomp - see docs/RMGR_SPEC.md section 4.6
};
static_assert(sizeof(ItemUpdateEvent) == 25, "ItemUpdateEvent must be 25 bytes");

// v3: currently just Whispy Woods' wind on Dream Land - only emitted on a
// frame where at least one bit is set (never a placeholder event with
// hazardFlags == 0), same sparse-event convention as ItemUpdate. More
// hazards can set more bits later via the field-append mechanism (section
// 5) without needing a new event type.
struct StageHazardUpdateEvent
{
    int32_t frame;
    uint8_t hazardFlags; // bit 0 = Whispy Woods currently blowing (Dream Land only)
};
static_assert(sizeof(StageHazardUpdateEvent) == 5, "StageHazardUpdateEvent must be 5 bytes");

constexpr uint8_t kHazardFlagWhispyBlowing = 0x01;

// v5 new event type: one per currently-active hitbox slot (a fighter's own
// attack, or an item's/weapon's), per frame - zero or more of these follow
// that frame's ItemUpdate/StageHazardUpdate events, same sparse convention
// (a disabled slot is never emitted). See ReplayMemory::HitboxObject's own
// doc comment for the confidence caveat on Item/Weapon offsets, and
// docs/RMGR_SPEC.md section 4.8.
//
// This is deliberately verbose (every active slot, every frame) rather than
// deduplicated - the plan is to record exhaustively for now and, once
// hitbox/hurtbox geometry is shown to be reliably derivable from
// (characterId, actionStateId, actionFrameCounter) alone for a given
// character, stop recording it for that character and compute it instead.
struct HitboxUpdateEvent
{
    int32_t  frame;
    uint8_t  ownerKind;   // 0 = Fighter, 1 = Item, 2 = Weapon - see ReplayMemory::HitboxObject
    uint32_t ownerId;     // Fighter: port (0-3). Item/Weapon: GObj address, correlates with that frame's ItemUpdate.objectAddress
    uint8_t  slotIndex;   // Fighter: 0-3. Item/Weapon: 0-1.
    uint8_t  attackState; // 1 fresh, 2 transfer, 3 interpolate - never 0 (disabled slots aren't emitted)
    int32_t  damage;
    float    positionX;   // world-space, already transformed
    float    positionY;
    float    positionZ;
    float    size;        // radius - hitboxes are spheres, not boxes
    int32_t  angle;
    int32_t  knockbackScale;
    int32_t  knockbackWeight;
    int32_t  knockbackBase;
    int32_t  element;
    int32_t  shieldDamage;
};
static_assert(sizeof(HitboxUpdateEvent) == 55, "HitboxUpdateEvent must be 55 bytes");

// v5 new event type: one per fighter hurtbox slot (11 per seated port), per
// frame - see ReplayMemory::HurtboxObject's own doc comment, including the
// approximation noted for positionX/Y/Z, and docs/RMGR_SPEC.md section 4.9.
// Unlike ItemUpdate/HitboxUpdate this isn't sparse - a seated port's 11
// slots are (almost) always all present, since hurtboxes exist essentially
// continuously while a fighter is alive. See HitboxUpdateEvent's doc
// comment above for why this verbosity is intentional for now.
struct HurtboxUpdateEvent
{
    int32_t  frame;
    uint8_t  port;
    uint8_t  slotIndex;   // 0-10
    int32_t  hitStatus;   // per-bone Vulnerable/Invincible/Intangible - raw value, see ReplayMemory::HurtboxObject
    int32_t  placement;   // 0 low, 1 middle, 2 high
    uint8_t  isGrabbable;
    float    positionX;   // APPROXIMATION - the bone's own joint position, not the true transformed hurtbox center
    float    positionY;
    float    positionZ;
    float    offsetX;     // authored, bone-relative, untransformed
    float    offsetY;
    float    offsetZ;
    // Anisotropic (Vec3f, not a single radius like a hitbox) - and, per the
    // decomp's fighter-hurtbox init, stored PRE-HALVED (size.{x,y,z} *= 0.5F
    // there): these are half-extents, not full extents. A consumer that
    // wants the real box dimensions needs to double these, not use them
    // directly as width/height/depth.
    float    sizeX;
    float    sizeY;
    float    sizeZ;
};
static_assert(sizeof(HurtboxUpdateEvent) == 51, "HurtboxUpdateEvent must be 51 bytes");

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
// Set once per session in OnEmulationStart() from
// SettingsID::GameStats_RecordHitboxData (off by default) - gates
// HitboxUpdate/HurtboxUpdate in RecordFrame() only; every other event type
// is unaffected. See that setting's own doc comment in Settings.hpp for why
// this exists as a separate opt-in from GameStats_ReplayEnabled itself.
bool s_RecordHitboxData = false;

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

// This feature's memory offsets were only ever derived/verified against
// Smash Remix 2.0.1 (see docs/RMGR_SPEC.md); recording against any other
// ROM would pointer-chase addresses that mean nothing there. GoodName
// comes from mupen64plus-core's own ROM database (CoreRomSettings::GoodName,
// via CoreGetCurrentRomSettings()) - for a ROM/hack absent from that
// database it degrades to a filename-derived value, so this exact-match
// check can only ever be as reliable as that database entry.
//
// Single hardcoded game for now, not a per-GoodName profile table - that's
// a bigger refactor for when a second GoodName actually needs supporting
// (see docs/RMGR_SPEC.md section 3 for the reasoning).
constexpr const char* kSupportedGoodName = "SmashRemix2.0.1";

// Bump whenever this recorder's interpretation of kSupportedGoodName's
// memory layout changes in a way that affects what gets written - not just
// when a field is newly appended (which the per-event EventPayloads
// declared-size mechanism, docs/RMGR_SPEC.md section 5, already handles on
// its own), but also e.g. a bugfix to an existing field's offset that
// silently changes recorded *values* without changing any event's byte
// size. This is its own counter per goodName; a different goodName starts
// its own numbering from 1, unrelated to this one.
//
// v1 -> v2: added the ItemUpdate event (docs/RMGR_SPEC.md section 5/4.6) -
// a wholly new capability rather than a field append, but still exactly the
// kind of "recorded output changed" this counter exists to track.
// v2 -> v3: ItemUpdate's type field was wrong in v2 - +0x0C on the raw GObj
// is a packed byte (link_id), not a 32-bit type ID; the real type needs a
// further pointer chase through ITStruct/WPStruct. v2 files' ItemUpdate
// data should be treated as garbage, not just "coarser." Also added
// StageHazardUpdate (Whispy Woods' wind on Dream Land).
// v3 -> v4: ReadItemObjects() was walking only the Item list
// (gGCCommonLinks[4]) - Weapons (fireballs, boomerang, charge shot, PK
// Fire/Thunder, ...) live on a separate list (gGCCommonLinks[5]) that was
// never actually reached, so no v3 file ever contains a real Weapon
// ItemUpdate despite the wire format supporting linkId == 5. Also, held
// Items (e.g. Link's bomb while still in his hand) were being recorded with
// a meaningless re-parented position (typically (0,0,0)) instead of being
// skipped. Byte layout is unchanged from v3 - this is a pure "which objects
// get emitted" fix, same class of change as the v2->v3 bump. See
// ReplayMemory::ReadItemObjects()'s doc comment.
// v4 -> v5: added HitboxUpdate and HurtboxUpdate (docs/RMGR_SPEC.md section
// 5/4.8/4.9) - deliberately verbose (every active hitbox slot and every
// hurtbox slot, every frame) for now, to exhaustively capture real data
// before it's known whether hitbox/hurtbox geometry can be derived purely
// from (characterId, actionStateId, actionFrameCounter) instead. Expect
// this to shrink again in a future schema once that's confirmed per
// character.
// v5 -> v6: ReadItemObjects()'s "currently held" check was wrong -
// decomp-confirmed (itMainSetFighterRelease()) that ITStruct::owner_gobj is
// deliberately retained across a throw/drop for later damage/KO
// attribution, not cleared on release, so `owner_gobj != NULL` was
// non-NULL for essentially an item's *entire* lifetime and silently
// swallowed every thrown/dropped Item for its whole flight - not just the
// brief hand-held window it was meant to skip. Replaced with a position-
// based proxy (still reads near (0,0,0), the hand-parented placeholder)
// that actually flips at the release moment. v4/v5 files' Item ItemUpdate
// data (Weapons unaffected - this check never applied to them) is missing
// most or all of every thrown/dropped Item's flight, not just its held
// phase - re-record rather than treat as complete. Byte layout unchanged,
// same class of change as v3->v4. See ReplayMemory::IsHeldItemPosition()'s
// doc comment.
// v6 -> v7: PostFrameUpdate.jumpsUsed was read at the wrong width - a u32
// read at playerStruct+0x148, where the real field (decomp-confirmed) is a
// single byte (u8); +0x14A-0x14B is padding. On a word-swapped emulator, a
// *byte* read needs its address XORed with 3 to land correctly - without
// that, this landed on the padding instead, reading a constant 0 for an
// entire match, every port, no matter how much jumping happened. Fixed the
// read width AND switched what gets exported: this field is now
// jumpsRemaining (jumpsMax, chased from the per-character FTAttributes,
// minus the corrected jumpsUsed) instead of jumpsUsed directly - more
// directly useful, and what this project wanted from the start. Same wire
// position/size (still a u8) - a pure "what this byte means" fix, not a
// layout change. v6 and earlier files' jumpsUsed byte should be treated as
// meaningless (it's the constant-0 bug's output, not real data) rather
// than reinterpreted as anything.
// v7 -> v8: ReplayMemory::ReadHurtboxes()'s "is this slot in use" check was
// wrong - decomp-confirmed (ftmanager.c's fighter-hurtbox init) that an
// unused FTDamageColl slot leaves its `joint` field untouched (not NULL,
// not necessarily even outside the valid RDRAM range - just whatever was
// in that memory before), while `hitstatus` is explicitly set to
// nGMHitStatusNone (0) for unused slots and Normal/Invincible/Intangible
// for used ones. Gating on IsValidRdramPointer(joint) instead of
// `hitstatus != 0` filtered out every slot, used or not, every frame - a
// real match with real hurtboxes the whole time produced zero
// HurtboxUpdate events. Fixed by gating on hitStatus first. Byte layout
// unchanged, same class of fix as v5->v6/v6->v7. **v7 and earlier files'
// HurtboxUpdate data is empty/missing, not incomplete** - re-record rather
// than treat as a real "no hurtboxes this match" result.
constexpr uint32_t kRecorderSchemaVersion = 8;

bool IsSupportedGame(void)
{
    CoreRomSettings romSettings;
    if (!CoreGetCurrentRomSettings(romSettings))
    {
        return false;
    }
    return romSettings.GoodName == kSupportedGoodName;
}

// Copies as much of `s` as fits into `dest` (size `destSize`), NUL-padding
// or truncating as needed - `dest` is assumed zero-initialized already, so
// this only needs to write the bytes that actually fit.
void WriteFixedString(char* dest, size_t destSize, const std::string& s)
{
    std::memcpy(dest, s.data(), std::min(s.size(), destSize));
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
        {EventCode::GameStart,  static_cast<uint16_t>(sizeof(GameStartEvent))},
        {EventCode::PreFrame,   static_cast<uint16_t>(sizeof(PreFrameEvent))},
        {EventCode::PostFrame,  static_cast<uint16_t>(sizeof(PostFrameEvent))},
        {EventCode::GameEnd,    static_cast<uint16_t>(sizeof(GameEndEvent))},
        // v2/v3/v5 additions (docs/RMGR_SPEC.md section 5) - an old parser
        // that reads `count` dynamically and skips codes it doesn't
        // recognize (exactly what this declared-size mechanism exists for)
        // still parses a file with these extra entries correctly.
        {EventCode::ItemUpdate,        static_cast<uint16_t>(sizeof(ItemUpdateEvent))},
        {EventCode::StageHazardUpdate, static_cast<uint16_t>(sizeof(StageHazardUpdateEvent))},
        {EventCode::HitboxUpdate,      static_cast<uint16_t>(sizeof(HitboxUpdateEvent))},
        {EventCode::HurtboxUpdate,     static_cast<uint16_t>(sizeof(HurtboxUpdateEvent))},
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

// Loosely mirrors n02_client.cpp's "<date>-Player1-Player2.krec" convention
// (section 3.1 of the handoff doc) - player-name suffix and 24-char cap the
// same, but YYYYMMDD-HHMMSS (4-digit year, dashed) rather than krec's
// compact YYMMDDHHMMSS, for a name that reads as a timestamp at a glance.
// `now` is passed in (rather than this function calling time(nullptr)
// itself) so the caller can derive it from the exact same instant it
// writes into the file's own recordedAtEpochMillis header field - the
// filename and the header should never disagree about when the recording
// started, even though the filename itself is only second-resolution.
std::string BuildFileName(time_t now)
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

// If `desiredPath` already exists on disk, tries "<stem>-2<ext>",
// "<stem>-3<ext>", ... until a free one is found. One emulation session
// (one OnEmulationStart()..OnEmulationStop() span) can open more than one
// file here - a single headless export of a multi-game .krec produces one
// .rmgr per match, all sharing the same base name (see
// SetOutputPathOverride's doc comment) - so without this, match 2 would
// silently overwrite match 1's file.
std::filesystem::path FindCollisionFreePath(const std::filesystem::path& desiredPath)
{
    std::error_code errorCode;
    if (!std::filesystem::exists(desiredPath, errorCode))
    {
        return desiredPath;
    }

    const std::filesystem::path directory = desiredPath.parent_path();
    const std::filesystem::path extension = desiredPath.extension();
    const std::string stem = desiredPath.stem().string();

    for (int suffix = 2; suffix < 10000; suffix++)
    {
        std::filesystem::path candidate = directory / (stem + "-" + std::to_string(suffix) + extension.string());
        if (!std::filesystem::exists(candidate, errorCode))
        {
            return candidate;
        }
    }

    // Pathological case (10000 collisions) - fall through to the original
    // path rather than loop forever; the caller's own file open just
    // overwrites it same as before this function existed.
    return desiredPath;
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

    // Default: live recording, stamped with wall-clock "now" - read from
    // system_clock (not time(nullptr)) so recordedAtEpochMillis/
    // recordedAtNanosOffset below get real sub-second precision. Headless
    // .krec export overrides this (see SetRecordedAtBaseOverride's doc
    // comment) so the file reflects the match's *original* recording time
    // rather than when the headless replay (which can run at up to 2000%
    // speed) happened to reach it - that path has no sub-millisecond
    // information to offer, so recordedAtNanosOffset is always 0 for it.
    std::chrono::system_clock::time_point nowTimePoint;
    uint32_t                              nowNanosOffset = 0;
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
        const auto sinceEpoch = nowTimePoint.time_since_epoch();
        nowNanosOffset = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count() % 1'000'000);
    }
    const uint64_t nowEpochMillis = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nowTimePoint.time_since_epoch()).count());
    // BuildFileName()'s date part is still just local wall-clock seconds -
    // see docs/RMGR_SPEC.md section 3.4.
    const time_t now = static_cast<time_t>(nowEpochMillis / 1000);
    std::filesystem::path path;
    if (s_HasOutputPathOverride)
    {
        // Not consumed here - see SetOutputPathOverride's doc comment. Every
        // match this session gets its own explicitly-numbered file -
        // "<override>-1.ext", "<override>-2.ext", ... - not just
        // "<override>.ext" for the first, so a multi-game .krec's export
        // still reads as "<krec name>-<game number>.rmgr" and plainly
        // corresponds back to its source .krec by name.
        // FindCollisionFreePath() is still a safety net in case this same
        // krec was already exported before (so "-1" is already taken).
        s_OverrideMatchNumber++;
        const std::filesystem::path base(s_OutputPathOverride);
        const std::filesystem::path numberedPath = base.parent_path() /
            (base.stem().string() + "-" + std::to_string(s_OverrideMatchNumber) + base.extension().string());
        path = FindCollisionFreePath(numberedPath);
        std::filesystem::create_directories(path.parent_path());
    }
    else
    {
        // Bare relative path, resolved by CWD - deliberately mirrors krec's
        // own "records" directory convention (Source/n02/n02_client.cpp)
        // exactly, so .rmgr files land next to .krec files at the top level
        // instead of being nested under the per-platform user-data directory.
        std::filesystem::path directory("replays");
        std::filesystem::create_directories(directory);
        path = FindCollisionFreePath(directory / BuildFileName(now));
    }

    s_File.open(path, std::ios::binary | std::ios::trunc);
    if (!s_File.is_open())
    {
        if (s_HasOutputPathOverride)
        {
            s_OverrideMatchNumber--; // undo - see the increment above, this attempt never happened
        }
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "Replay: failed to open " + path.string() + " for recording");
        return false;
    }

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        "Replay: recording to " + path.string());

    FileHeader header{};
    std::memcpy(header.magic, "RMGR", 4);
    header.version = 4;
    header.streamLength = 0;
    // Reaching here already implies IsSupportedGame() was true (see
    // OnEmulationStart()), so this is just re-fetching the same value to
    // write it out - the loaded ROM can't change mid-session.
    CoreRomSettings romSettings;
    CoreGetCurrentRomSettings(romSettings);
    WriteFixedString(header.goodName, sizeof(header.goodName), romSettings.GoodName);
    header.recorderSchemaVersion = kRecorderSchemaVersion;
    header.recordedAtEpochMillis = nowEpochMillis;
    header.recordedAtNanosOffset = nowNanosOffset;
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
    // Not consumed here (unlike the old per-file behavior) - see
    // SetPlayerNamesOverride's doc comment. Every match recorded during
    // this session uses the same names.
    if (s_HasPlayerNamesOverride)
    {
        for (int port = 0; port < 4; port++)
        {
            WriteFixedString(startEvent.playerNames[port], sizeof(startEvent.playerNames[port]), s_PlayerNamesOverride[port]);
        }
    }
    else
    {
        std::memcpy(startEvent.playerNames, recording_player_names, sizeof(startEvent.playerNames));
    }
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
        // Clamped rather than a raw cast: state.jumpsRemaining is signed
        // and defaults to 0 if the FTAttributes pointer chase ever fails,
        // but could in principle read momentarily negative mid-transition -
        // wrapping that to a large uint8_t via a raw cast would be actively
        // misleading, not just imprecise.
        post.jumpsRemaining             = static_cast<uint8_t>(std::max(0, state.jumpsRemaining));
        post.groundedState               = state.groundedState;
        post.hurtboxState                 = state.hurtboxState;
        post.hitstunCounter                = state.hitstunCounter;
        post.actionFrameCounter             = state.actionFrameCounter;
        post.comboHitCount                   = portInfo.comboHitCount;
        post.comboDamage                      = portInfo.comboDamage;
        WriteEvent(EventCode::PostFrame, post);
    }

    // One ItemUpdate per currently-live Item/Weapon GObj (see
    // docs/RMGR_SPEC.md section 4.6) - after every seated port's Pre/Post-
    // Frame pair, same as the per-port events above. Zero events written
    // when the list is empty this frame - never a zeroed/placeholder event,
    // same convention as an unseated port.
    for (const ReplayMemory::ItemObject& item : ReplayMemory::ReadItemObjects())
    {
        ItemUpdateEvent itemEvent{};
        itemEvent.frame         = s_FrameNumber;
        itemEvent.objectAddress = item.objectAddress;
        itemEvent.linkId        = item.linkId;
        itemEvent.kind          = item.kind;
        itemEvent.positionX     = item.positionX;
        itemEvent.positionY     = item.positionY;
        itemEvent.positionZ     = item.positionZ;
        WriteEvent(EventCode::ItemUpdate, itemEvent);
    }

    // StageHazardUpdate - only written when at least one tracked hazard is
    // active, same sparse convention as ItemUpdate above.
    const ReplayMemory::StageHazards hazards = ReplayMemory::ReadStageHazards(matchInfo.stageId);
    uint8_t hazardFlags = 0;
    if (hazards.whispyBlowing)
    {
        hazardFlags |= kHazardFlagWhispyBlowing;
    }
    if (hazardFlags != 0)
    {
        StageHazardUpdateEvent hazardEvent{};
        hazardEvent.frame       = s_FrameNumber;
        hazardEvent.hazardFlags = hazardFlags;
        WriteEvent(EventCode::StageHazardUpdate, hazardEvent);
    }

    // HitboxUpdate/HurtboxUpdate - gated behind
    // SettingsID::GameStats_RecordHitboxData (off by default): both are
    // deliberately verbose (every active hitbox slot, and literally every
    // hurtbox slot, every frame - see HurtboxUpdateEvent's own doc comment)
    // and dominate a .rmgr file's size when on. Skipping the read entirely
    // when off, not just the write, since ReplayMemory::ReadHitboxes()/
    // ReadHurtboxes() aren't free (multiple pointer chases per port/slot).
    if (s_RecordHitboxData)
    {
        // HitboxUpdate - one per currently-active hitbox slot (see
        // docs/RMGR_SPEC.md section 4.8). Sparse like ItemUpdate: a
        // disabled slot is never written.
        for (const ReplayMemory::HitboxObject& hitbox : ReplayMemory::ReadHitboxes(matchInfo.matchInfoPtr))
        {
            HitboxUpdateEvent hitboxEvent{};
            hitboxEvent.frame           = s_FrameNumber;
            hitboxEvent.ownerKind       = hitbox.ownerKind;
            hitboxEvent.ownerId         = hitbox.ownerId;
            hitboxEvent.slotIndex       = hitbox.slotIndex;
            hitboxEvent.attackState     = hitbox.attackState;
            hitboxEvent.damage          = hitbox.damage;
            hitboxEvent.positionX       = hitbox.positionX;
            hitboxEvent.positionY       = hitbox.positionY;
            hitboxEvent.positionZ       = hitbox.positionZ;
            hitboxEvent.size            = hitbox.size;
            hitboxEvent.angle           = hitbox.angle;
            hitboxEvent.knockbackScale  = hitbox.knockbackScale;
            hitboxEvent.knockbackWeight = hitbox.knockbackWeight;
            hitboxEvent.knockbackBase   = hitbox.knockbackBase;
            hitboxEvent.element         = hitbox.element;
            hitboxEvent.shieldDamage    = hitbox.shieldDamage;
            WriteEvent(EventCode::HitboxUpdate, hitboxEvent);
        }

        // HurtboxUpdate - one per fighter hurtbox slot, per seated port
        // (see docs/RMGR_SPEC.md section 4.9). NOT sparse like the events
        // above - see HurtboxUpdateEvent's own doc comment for why.
        for (const ReplayMemory::HurtboxObject& hurtbox : ReplayMemory::ReadHurtboxes(matchInfo.matchInfoPtr))
        {
            HurtboxUpdateEvent hurtboxEvent{};
            hurtboxEvent.frame       = s_FrameNumber;
            hurtboxEvent.port        = hurtbox.port;
            hurtboxEvent.slotIndex   = hurtbox.slotIndex;
            hurtboxEvent.hitStatus   = hurtbox.hitStatus;
            hurtboxEvent.placement   = hurtbox.placement;
            hurtboxEvent.isGrabbable = hurtbox.isGrabbable ? 1 : 0;
            hurtboxEvent.positionX   = hurtbox.positionX;
            hurtboxEvent.positionY   = hurtbox.positionY;
            hurtboxEvent.positionZ   = hurtbox.positionZ;
            hurtboxEvent.offsetX     = hurtbox.offsetX;
            hurtboxEvent.offsetY     = hurtbox.offsetY;
            hurtboxEvent.offsetZ     = hurtbox.offsetZ;
            hurtboxEvent.sizeX       = hurtbox.sizeX;
            hurtboxEvent.sizeY       = hurtbox.sizeY;
            hurtboxEvent.sizeZ       = hurtbox.sizeZ;
            WriteEvent(EventCode::HurtboxUpdate, hurtboxEvent);
        }
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

    // Only meaningful when replay recording itself is enabled - no override
    // mechanism needed for this one (unlike `enabled` above), so this is a
    // plain per-session settings read.
    s_RecordHitboxData = enabled && CoreSettingsGetBoolValue(SettingsID::GameStats_RecordHitboxData);

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
