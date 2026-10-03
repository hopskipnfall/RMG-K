/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef REPLAY_EVENTS_HPP
#define REPLAY_EVENTS_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The .rmgr event structs and codes (docs/RMGR_SPEC.md sections 4-5), shared
// by the file recorder (Replay.cpp) and the practice client (Practice*.cpp)
// so both emit byte-identical events.
namespace ReplayEvents
{
#pragma pack(push, 1)

enum class EventCode : uint8_t
{
    EventPayloads = 0x01,
    // Core (always present, any recognized-or-not N64 ROM):
    MatchStart = 0x02,
    InputFrame = 0x03,
    MatchEnd   = 0x05,
    // smash64 game-family extension (present only when gameFamily ==
    // "smash64" - see IsSmash64() below):
    StateFrame        = 0x04,
    ItemUpdate        = 0x06,
    StageHazardUpdate = 0x07,
    MatchSettings     = 0x08,
    MatchResult       = 0x09,
};

// Core event, code 0x02. Written exactly once, immediately after
// EventPayloads. Player display names are sourced from netplay room
// metadata (RMG-K's own slot-indexed name table), never from any in-game
// name tag - for an offline match, or a port with no assigned name, the
// corresponding playerNames entry is all zero bytes. Game-family-specific
// match settings (stage, character, stock count, damage ratio, items,
// teams, handicap, CPU difficulty, ...) are NOT part of this event - see
// MatchSettingsEvent below.
struct MatchStartEvent
{
    char    playerNames[4][32]; // NUL-padded; not necessarily NUL-terminated if it fills the field. UTF-8.
    uint8_t slotType[4];        // 0 human, 1 CPU, 2 empty - per port 0-3
};
static_assert(sizeof(MatchStartEvent) == 132, "MatchStartEvent must be 132 bytes");

// Core event, code 0x03. Input-side data, captured before the game
// processes that frame's inputs. One event per seated port per frame. Uses
// the game's already-processed button/stick values, the one input
// representation available uniformly for both human and CPU-controlled
// ports.
struct InputFrameEvent
{
    int32_t  frame;
    uint8_t  port;
    uint16_t buttons;
    int8_t   stickX;
    int8_t   stickY;
};
static_assert(sizeof(InputFrameEvent) == 9, "InputFrameEvent must be 9 bytes");

// Core event, code 0x05. Written exactly once, as the last event in the
// stream. Final per-port results (e.g. smash64's stocks-remaining
// placements) aren't a universal concept across N64 titles and are NOT part
// of this event - see MatchResultEvent below.
struct MatchEndEvent
{
    int32_t finalFrame; // last frame value seen in any InputFrame event this match
    uint8_t endReason;  // 0 aborted (match-was-reset or process/emulation stopped mid-match), 1 normal end
};
static_assert(sizeof(MatchEndEvent) == 5, "MatchEndEvent must be 5 bytes");

// smash64 extension event, code 0x08. Written exactly once, immediately
// after MatchStart - the game-family-specific counterpart split out of what
// used to be one combined GameStart event. Everything here is
// Smash-specific and static for the whole match.
struct MatchSettingsEvent
{
    uint8_t stageId;
    uint8_t gameType;          // 1 time, 2 stock, 3 both (Remix always forces stock)
    uint8_t stockCountSetting; // 0-based (i.e. 2 means "3 stocks")
    uint8_t timeLimitMinutes;  // 100 = infinite
    uint8_t damageRatio;       // 50 = 50%, 200 = 200%
    uint8_t itemFrequency;     // 0 none .. 5 high
    uint8_t teamsEnabled;      // 0 off, 1 on
    uint8_t handicapMode;      // 0 off, 1 on, 2 auto
    uint8_t characterId[4];    // per port 0-3
    uint8_t costumeId[4];
    uint8_t teamColor[4];
    uint8_t portTeam[4];       // team number per port
    uint8_t portHandicap[4];   // meaningful only when handicapMode != 0
    uint8_t portCpuLevel[4];   // meaningless for a human port
    // Added in recorder schema 3 (kRecorderSchemaVersion above) - see
    // docs/RMGR_SPEC.md for what every value below means (enum lookup
    // tables); this struct only documents shape, not semantics, to avoid
    // keeping two copies of the same table.
    int32_t rngSeed; // sSYUtilsRandomSeed at match start - see ReplayMemory::MatchInfo::rngSeed
    // Gameplay Settings (31, Remix Toggles.asm)
    uint8_t hitstun;
    uint8_t hitlag;
    uint8_t di;
    uint8_t japaneseSounds;
    uint8_t japaneseStunSleep;
    uint8_t momentumSlide;
    uint8_t shieldStun;
    uint8_t zCancel;
    uint8_t punishFailedZCancel;
    uint8_t improvedAI;
    uint8_t tripping;
    uint8_t rage;
    uint8_t footstoolJumping;
    uint8_t airDodging;
    uint8_t jabLocking;
    uint8_t edgeCJumping;
    uint8_t perfectShielding;
    uint8_t parrying;
    uint8_t spotDodging;
    uint8_t fastFallAerials;
    uint8_t ledgeTrumping;
    uint8_t wallTeching;
    uint8_t chargeSmashes;
    uint8_t itemContainers;
    uint8_t gameSpeed;
    uint8_t specialZoom;
    uint8_t blastzoneWarp;
    uint8_t singleButtonMode;
    uint8_t allItemsRDropAerial;
    uint8_t moveStaling;
    uint8_t stopwatchItem;
    // Stage Settings (8, Remix Toggles.asm)
    uint8_t stageSelectLayout;
    uint8_t hazardMode;
    uint8_t whispyMode;
    uint8_t saffronPokemonRate;
    uint8_t pokemonAnnouncer;
    uint8_t dragonKingHUD;
    uint8_t cameraMode;
    uint8_t yoshiIslandCloudAnims;
};
static_assert(sizeof(MatchSettingsEvent) == 75, "MatchSettingsEvent must be 75 bytes");

// smash64 extension event, code 0x04. State-side data, captured after that
// frame's physics/collision resolution - the resulting state. One event per
// seated port per frame, always immediately following that port's
// InputFrame in the stream.
struct StateFrameEvent
{
    int32_t  frame; // same frame counter as the paired InputFrame
    uint8_t  port;
    uint8_t  characterId;
    uint16_t actionStateId;
    float    positionX;
    float    positionY;
    int32_t  facingDirection; // 1 right, -1 left
    float    velocityX;
    float    velocityY;
    uint32_t damagePercent;
    int8_t   stocksRemaining; // 0-based; negative once eliminated
    // jumpsMax (per-character, from FTAttributes) minus jumps_used
    // (playerStruct+0x148, a u8 that resets to 0 on landing). 0 through
    // most of a grounded match is normal; Remix can also force this to 0
    // without that many real jump inputs (e.g. certain up-specials).
    uint8_t  jumpsRemaining;
    uint8_t  groundedState; // 0 grounded, 1 airborne
    uint8_t  hurtboxState;  // motion-script GMHitStatus: 0 off, 1 normal, 2 invincible, 3 intangible
    uint16_t hitstunCounter;
    uint32_t actionFrameCounter;
    // Native engine combo tracking, not mod-added. Belongs to the victim
    // (this port), not the attacker: hits taken in the current unbroken
    // chain. 0 = no active chain, 1 = a single hit, 2+ = an actual combo.
    // Both zero the instant the chain breaks.
    uint32_t comboHitCount;
    uint32_t comboDamage;
    // Appended in recorder schema 2 (docs/RMGR_SPEC.md sections 5.2 and 6);
    // schema-1 files' StateFrame ends after comboDamage. scaleX/scaleY: the
    // fighter's render scale (root joint DObj scale); characterSpecific:
    // Samus/DK charge level or Kirby's copied fighter (see ReplayMemory.cpp's
    // PS_PASSIVE_VAR).
    float    scaleX;
    float    scaleY;
    int32_t  characterSpecific;
    // Also schema 2: shield health, the timed hit status (GMHitStatus -
    // respawn invincibility etc., separate from hurtboxState's motion-script
    // one), and temporary knockback armor (Yoshi's double jump).
    int32_t  shieldHealth;
    uint8_t  specialHitStatus;
    float    knockbackResist;
};
static_assert(sizeof(StateFrameEvent) == 71, "StateFrameEvent must be 71 bytes");

// smash64 extension event, code 0x06. Zero or more per frame - one per live
// Item or Weapon GObj (ReplayMemory::ItemObject) currently not held by a
// fighter, following that frame's InputFrame/StateFrame pairs. "Weapon" is
// a free-flying character special-move projectile (boomerang, fireball,
// ...); "Item" covers thrown/spawned items and hazard objects, including
// some fighter-held things like Link's pulled bomb.
struct ItemUpdateEvent
{
    int32_t  frame;         // same numbering as InputFrame/StateFrame
    uint32_t objectAddress; // the object's own RDRAM address - not a semantic spawn ID, see ReplayMemory::ItemObject
    uint8_t  linkId;        // 4 = Item, 5 = Weapon - which enum `kind` below means (docs/RMGR_SPEC.md section 8.6)
    int32_t  kind;          // ITKind (linkId == 4) or WPKind (linkId == 5)
    float    positionX;
    float    positionY;
    float    positionZ;
    // Appended in recorder schema 2 (docs/RMGR_SPEC.md sections 5.3 and 6) -
    // the object's render scale (DObj scale x/y). Schema-1 files' ItemUpdate
    // ends after positionZ; EventPayloads' declared size tells readers which.
    float    scaleX;
    float    scaleY;
};
static_assert(sizeof(ItemUpdateEvent) == 33, "ItemUpdateEvent must be 33 bytes");

// smash64 extension event, code 0x07. Zero or one per frame, following that
// frame's ItemUpdate events - written only when at least one tracked hazard
// is currently active, same sparse convention as ItemUpdate. Currently
// tracks exactly one hazard: Whispy Woods' wind on Dream Land.
struct StageHazardUpdateEvent
{
    int32_t frame;
    uint8_t hazardFlags; // bit 0 = Whispy Woods currently blowing (Dream Land only);
                          // bit 1 = blowing direction (0 = left, 1 = right) -
                          // only meaningful when bit 0 is set, and only ever
                          // written alongside it (see below)
};
static_assert(sizeof(StageHazardUpdateEvent) == 5, "StageHazardUpdateEvent must be 5 bytes");

constexpr uint8_t kHazardFlagWhispyBlowing      = 0x01;
constexpr uint8_t kHazardFlagWhispyBlowingRight = 0x02;

// smash64 extension event, code 0x09. Written exactly once, immediately
// after the core MatchEnd event - the game-family-specific counterpart
// split out of what used to be one combined GameEnd event, since "stocks
// remaining" is a Smash concept, not a universal one.
struct MatchResultEvent
{
    int8_t placements[4]; // final stocks remaining per port, -1 if never seated
};
static_assert(sizeof(MatchResultEvent) == 4, "MatchResultEvent must be 4 bytes");

#pragma pack(pop)

// This feature's memory offsets were only ever derived/verified against
// Smash Remix 2.0.1 (see docs/RMGR_SPEC.md); recording its extension events
// against any other ROM would pointer-chase addresses that mean nothing
// there. GoodName comes from mupen64plus-core's own ROM database
// (CoreRomSettings::GoodName, via CoreGetCurrentRomSettings()) - for a
// ROM/hack absent from that database it degrades to a filename-derived
// value, so this exact-match check can only ever be as reliable as that
// database entry.
inline constexpr const char* kSmashRemixGoodName = "SmashRemix2.0.1";
inline constexpr const char* kSmash64Family      = "smash64";

// Bump whenever this recorder's interpretation of a goodName's memory
// layout changes in a way that affects what an smash64-family reader gets -
// not just when a field is newly appended (which the per-event
// EventPayloads declared-size mechanism, docs/RMGR_SPEC.md section 6,
// already handles on its own), but also e.g. a bugfix to an existing
// field's offset that silently changes recorded *values* without changing
// any event's byte size. This is its own counter per goodName - see
// docs/RMGR_SPEC.md section 3.2.
//
// Starts fresh at 1 for this container rewrite: every memory-offset fix
// this recorder previously accumulated (schema 2 through 9 under the old,
// unspecified container layout - see git history for that trail) is already
// reflected as correct in ReplayMemory.cpp today. There's nothing left to
// carry forward; the old numbering tracked a struct layout (GameStart/
// PostFrameUpdate) that no longer exists.
//
// History (this container):
//   1 - initial version.
//   2 - ItemUpdate gains trailing scaleX/scaleY (render scale), and
//       StateFrame gains trailing scaleX/scaleY/characterSpecific/
//       shieldHealth/specialHitStatus/knockbackResist -
//       docs/RMGR_SPEC.md sections 5.2 and 5.3.
//   3 - MatchSettings gains trailing rngSeed plus 31 Gameplay Settings and
//       8 Stage Settings from Remix's Toggles.asm - docs/RMGR_SPEC.md
//       section 5.1.
inline constexpr uint32_t kRecorderSchemaVersion = 3;

// Copies as much of `s` as fits into `dest` (size `destSize`), NUL-padding
// or truncating as needed - `dest` is assumed zero-initialized already, so
// this only needs to write the bytes that actually fit.
inline void WriteFixedString(char* dest, size_t destSize, const std::string& s)
{
    std::memcpy(dest, s.data(), std::min(s.size(), destSize));
}

// Appends raw bytes to `out`.
inline void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t size)
{
    const size_t offset = out.size();
    out.resize(offset + size);
    std::memcpy(out.data() + offset, data, size);
}

template <typename T>
void AppendValue(std::vector<uint8_t>& out, const T& value)
{
    AppendBytes(out, &value, sizeof(value));
}

// Appends one event (code byte + payload) to `out`.
template <typename T>
void AppendEvent(std::vector<uint8_t>& out, EventCode code, const T& payload)
{
    AppendValue(out, static_cast<uint8_t>(code));
    AppendValue(out, payload);
}
} // namespace ReplayEvents

#endif // REPLAY_EVENTS_HPP
