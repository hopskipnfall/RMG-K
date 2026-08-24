/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef REPLAY_MEMORY_HPP
#define REPLAY_MEMORY_HPP

#include <cstdint>

// Smash Remix (N64) memory-layout reader for the replay-file feature.
// Every address/offset here comes from the hand-derived RAM map in
// rmgk-replay-file-agent-prompt.md section 6 - do not re-derive.
namespace ReplayMemory
{
struct PortMatchInfo
{
    uint8_t  slotType;   // 0 human, 1 CPU, 2 empty (raw value read from memory)
    bool     seated;     // slotType != 2
    bool     isCpu;      // slotType == 1
    uint8_t  characterId;
    uint8_t  costumeId;
    uint8_t  teamColor;
    int8_t   stocksRemaining;
    // Native engine combo tracking, not mod-added - tracked with the combo
    // meter display toggle off too. Belongs to the victim (this port), not
    // the attacker: how many hits *this port* has taken in its current
    // unbroken chain. 0 = no active chain, 1 = a single hit (not yet a
    // "combo" by convention), 2+ = an actual combo. Both zero the instant
    // the chain breaks. See docs/RMGR_SPEC.md section 7.5 / smashremix
    // docs/ram-map.md section 13.
    uint32_t comboHitCount;
    uint32_t comboDamage;
};

struct PortPlayerState
{
    bool     valid;     // false if the player-object/player-struct pointer
                         // chase failed (port not currently in a live match)
    uint8_t  characterId;
    uint16_t actionStateId;
    uint32_t actionFrameCounter;
    int32_t  facingDirection; // 1 = right, -1 = left
    float    velocityX;
    float    velocityY;
    float    positionX;
    float    positionY;
    uint32_t jumpsUsed;
    uint8_t  groundedState;    // 0 grounded, 1 airborne
    uint16_t processedButtons; // works for human AND CPU ports
    int8_t   stickX;
    int8_t   stickY;
    uint8_t  hurtboxState;
    uint16_t hitstunCounter;
    uint32_t damagePercent;
    uint8_t  team;      // team number this port is assigned to
    uint8_t  handicap;  // this port's handicap value (meaningful when MatchInfo::handicapMode != 0)
    uint8_t  cpuLevel;  // CPU difficulty; meaningless for human ports
};

struct MatchInfo
{
    bool     valid;
    uint32_t matchInfoPtr;
    uint8_t  gameMode;
    uint8_t  stageId;
    uint8_t  gameType;         // 1 time, 2 stock, 3 both
    uint8_t  timeLimitMinutes; // 100 = infinite
    uint8_t  stockCountSetting;
    uint8_t  damageRatio;      // 50 = 50%, 200 = 200%
    uint8_t  itemFrequency;
    uint8_t  gameStatus;       // 0 pre-match countdown, 1 ongoing, 2 paused, 5 ended
    bool     matchWasReset;
    bool     teamsEnabled;
    uint8_t  handicapMode;     // 0 off, 1 on, 2 auto
};

// current_screen == 0x16 ("in a VS match")
bool IsInVsMatchScreen(void);

// Reads the match info block at *0x800A50E8. MatchInfo::valid is false if
// the pointer isn't in the valid KSEG0 RDRAM window (0x80000000-0x80800000).
MatchInfo ReadMatchInfo(void);

// Cheap per-port fields, no further pointer chasing.
// port is 0-3. matchInfoPtr must come from a valid ReadMatchInfo() result.
PortMatchInfo ReadPortMatchInfo(uint32_t matchInfoPtr, int port);

// Chases matchStruct+0x58 -> playerObject+0x84 -> playerStruct. Returns
// PortPlayerState::valid == false (not a crash) if that port isn't
// currently seated in a live match.
PortPlayerState ReadPortPlayerState(uint32_t matchInfoPtr, int port);
} // namespace ReplayMemory

#endif // REPLAY_MEMORY_HPP
