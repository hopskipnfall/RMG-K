/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ReplayMemory.hpp"
#include "m64p/Api.hpp"

#include <cstring>

namespace
{
constexpr uint32_t ADDR_CURRENT_SCREEN   = 0x800A4AD0;
constexpr uint32_t ADDR_MATCH_RESET_FLAG = 0x800A4AE2;
constexpr uint32_t ADDR_MATCH_INFO_PTR   = 0x800A50E8;

constexpr uint32_t MI_GAME_MODE          = 0x00;
constexpr uint32_t MI_STAGE_ID           = 0x01;
constexpr uint32_t MI_TEAMS_ENABLED      = 0x02;
constexpr uint32_t MI_GAME_TYPE          = 0x03;
constexpr uint32_t MI_TIME_LIMIT         = 0x06;
constexpr uint32_t MI_STOCK_COUNT        = 0x07;
constexpr uint32_t MI_HANDICAP_MODE      = 0x08;
constexpr uint32_t MI_DAMAGE_RATIO       = 0x0B;
constexpr uint32_t MI_GAME_STATUS        = 0x11;
constexpr uint32_t MI_ITEM_FREQUENCY     = 0x1C;
constexpr uint32_t MI_PORT_STRUCT_BASE   = 0x20;
constexpr uint32_t MI_PORT_STRUCT_STRIDE = 0x74;

constexpr uint32_t PORT_SLOT_TYPE        = 0x02;
constexpr uint32_t PORT_CHARACTER_ID     = 0x03;
constexpr uint32_t PORT_COSTUME_ID       = 0x06;
constexpr uint32_t PORT_TEAM_COLOR       = 0x07;
constexpr uint32_t PORT_STOCKS_REMAINING = 0x0B;
constexpr uint32_t PORT_PLAYER_OBJECT    = 0x58;

constexpr uint32_t PLAYER_OBJECT_TO_STRUCT = 0x84;

constexpr uint32_t PS_ACTION_FRAME_COUNTER = 0x1C;
constexpr uint32_t PS_ACTION_STATE_ID      = 0x24;
constexpr uint32_t PS_DAMAGE_PERCENT       = 0x2C;
constexpr uint32_t PS_FACING_DIRECTION     = 0x44;
constexpr uint32_t PS_VELOCITY_X           = 0x48;
constexpr uint32_t PS_VELOCITY_Y           = 0x4C;
constexpr uint32_t PS_POSITION_PTR         = 0x78;
constexpr uint32_t PS_JUMPS_USED           = 0x148;
constexpr uint32_t PS_KINETIC_STATE        = 0x14C;
constexpr uint32_t PS_PROCESSED_BUTTONS    = 0x1BC;
constexpr uint32_t PS_STICK_X              = 0x1C2;
constexpr uint32_t PS_STICK_Y              = 0x1C3;
constexpr uint32_t PS_HURTBOX_STATE        = 0x5BB;
constexpr uint32_t PS_HITSTUN_COUNTER      = 0xB1A;
constexpr uint32_t PS_TEAM                 = 0x0C;
constexpr uint32_t PS_HANDICAP             = 0x12;
constexpr uint32_t PS_CPU_LEVEL            = 0x13;

// KSEG0, 8MB expansion-pak RDRAM window. A value outside this range means a
// pointer chase hit garbage - treat as "not currently available", not a crash.
bool IsValidRdramPointer(uint32_t ptr)
{
    return ptr >= 0x80000000u && ptr < 0x80800000u;
}

float ReadFloat(uint32_t address)
{
    uint32_t bits = m64p::Core.DebugMemRead32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
} // namespace

namespace ReplayMemory
{
bool IsInVsMatchScreen(void)
{
    return m64p::Core.DebugMemRead8(ADDR_CURRENT_SCREEN) == 0x16;
}

MatchInfo ReadMatchInfo(void)
{
    MatchInfo info{};
    info.valid = false;

    uint32_t matchInfoPtr = m64p::Core.DebugMemRead32(ADDR_MATCH_INFO_PTR);
    if (!IsValidRdramPointer(matchInfoPtr))
    {
        return info;
    }

    info.valid             = true;
    info.matchInfoPtr      = matchInfoPtr;
    info.gameMode          = m64p::Core.DebugMemRead8(matchInfoPtr + MI_GAME_MODE);
    info.stageId            = m64p::Core.DebugMemRead8(matchInfoPtr + MI_STAGE_ID);
    info.gameType            = m64p::Core.DebugMemRead8(matchInfoPtr + MI_GAME_TYPE);
    info.timeLimitMinutes    = m64p::Core.DebugMemRead8(matchInfoPtr + MI_TIME_LIMIT);
    info.stockCountSetting   = m64p::Core.DebugMemRead8(matchInfoPtr + MI_STOCK_COUNT);
    info.damageRatio         = m64p::Core.DebugMemRead8(matchInfoPtr + MI_DAMAGE_RATIO);
    info.itemFrequency       = m64p::Core.DebugMemRead8(matchInfoPtr + MI_ITEM_FREQUENCY);
    info.gameStatus          = m64p::Core.DebugMemRead8(matchInfoPtr + MI_GAME_STATUS);
    info.matchWasReset       = m64p::Core.DebugMemRead8(ADDR_MATCH_RESET_FLAG) != 0;
    info.teamsEnabled        = m64p::Core.DebugMemRead8(matchInfoPtr + MI_TEAMS_ENABLED) != 0;
    info.handicapMode        = m64p::Core.DebugMemRead8(matchInfoPtr + MI_HANDICAP_MODE);
    return info;
}

PortMatchInfo ReadPortMatchInfo(uint32_t matchInfoPtr, int port)
{
    PortMatchInfo info{};
    uint32_t base = matchInfoPtr + MI_PORT_STRUCT_BASE +
        static_cast<uint32_t>(port) * MI_PORT_STRUCT_STRIDE;

    uint8_t slotType = m64p::Core.DebugMemRead8(base + PORT_SLOT_TYPE);
    info.slotType          = slotType;
    info.seated             = slotType != 2;
    info.isCpu               = slotType == 1;
    info.characterId          = m64p::Core.DebugMemRead8(base + PORT_CHARACTER_ID);
    info.costumeId             = m64p::Core.DebugMemRead8(base + PORT_COSTUME_ID);
    info.teamColor              = m64p::Core.DebugMemRead8(base + PORT_TEAM_COLOR);
    info.stocksRemaining         = static_cast<int8_t>(m64p::Core.DebugMemRead8(base + PORT_STOCKS_REMAINING));
    return info;
}

PortPlayerState ReadPortPlayerState(uint32_t matchInfoPtr, int port)
{
    PortPlayerState state{};
    state.valid = false;

    uint32_t base = matchInfoPtr + MI_PORT_STRUCT_BASE +
        static_cast<uint32_t>(port) * MI_PORT_STRUCT_STRIDE;

    uint32_t playerObject = m64p::Core.DebugMemRead32(base + PORT_PLAYER_OBJECT);
    if (!IsValidRdramPointer(playerObject))
    {
        return state;
    }

    uint32_t playerStruct = m64p::Core.DebugMemRead32(playerObject + PLAYER_OBJECT_TO_STRUCT);
    if (!IsValidRdramPointer(playerStruct))
    {
        return state;
    }

    state.valid               = true;
    state.characterId          = m64p::Core.DebugMemRead8(base + PORT_CHARACTER_ID);
    state.actionStateId         = static_cast<uint16_t>(m64p::Core.DebugMemRead32(playerStruct + PS_ACTION_STATE_ID));
    state.actionFrameCounter    = m64p::Core.DebugMemRead32(playerStruct + PS_ACTION_FRAME_COUNTER);
    state.facingDirection        = static_cast<int32_t>(m64p::Core.DebugMemRead32(playerStruct + PS_FACING_DIRECTION));
    state.velocityX               = ReadFloat(playerStruct + PS_VELOCITY_X);
    state.velocityY                = ReadFloat(playerStruct + PS_VELOCITY_Y);
    state.jumpsUsed                 = m64p::Core.DebugMemRead32(playerStruct + PS_JUMPS_USED);
    state.groundedState              = static_cast<uint8_t>(m64p::Core.DebugMemRead32(playerStruct + PS_KINETIC_STATE));
    state.processedButtons            = m64p::Core.DebugMemRead16(playerStruct + PS_PROCESSED_BUTTONS);
    state.stickX                       = static_cast<int8_t>(m64p::Core.DebugMemRead8(playerStruct + PS_STICK_X));
    state.stickY                        = static_cast<int8_t>(m64p::Core.DebugMemRead8(playerStruct + PS_STICK_Y));
    state.hurtboxState                   = m64p::Core.DebugMemRead8(playerStruct + PS_HURTBOX_STATE);
    state.hitstunCounter                  = m64p::Core.DebugMemRead16(playerStruct + PS_HITSTUN_COUNTER);
    state.damagePercent                    = m64p::Core.DebugMemRead32(playerStruct + PS_DAMAGE_PERCENT);
    state.team                              = m64p::Core.DebugMemRead8(playerStruct + PS_TEAM);
    state.handicap                           = m64p::Core.DebugMemRead8(playerStruct + PS_HANDICAP);
    state.cpuLevel                            = m64p::Core.DebugMemRead8(playerStruct + PS_CPU_LEVEL);

    uint32_t positionPtr = m64p::Core.DebugMemRead32(playerStruct + PS_POSITION_PTR);
    if (IsValidRdramPointer(positionPtr))
    {
        state.positionX = ReadFloat(positionPtr + 0x00);
        state.positionY = ReadFloat(positionPtr + 0x04);
    }

    return state;
}
} // namespace ReplayMemory
