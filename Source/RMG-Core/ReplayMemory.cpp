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
constexpr uint32_t PORT_COMBO_DAMAGE     = 0x50;
constexpr uint32_t PORT_COMBO_HIT_COUNT  = 0x54;
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

// Shared GObj (universal engine object) linked list - independent of
// MatchInfo, a fixed global head pointer. See smashremix docs/ram-map.md
// section 10.4 - confirmed against the real SSB64 decompilation
// (VetriTheRetri/ssb-decomp-re) after an earlier version of that doc
// misdocumented GOBJ_LINK_ID as a 32-bit "item ID" (it's actually 4 packed
// single bytes; only the first, link_id, matters here).
constexpr uint32_t ADDR_ITEM_LIST_HEAD = 0x80046700;
constexpr uint32_t GOBJ_NEXT           = 0x04;
constexpr uint32_t GOBJ_LINK_ID        = 0x0C; // u8: 3 = Fighter, 4 = Item, 5 = Weapon
constexpr uint32_t GOBJ_OBJ_PTR        = 0x74; // -> DObj (position/scale)
constexpr uint32_t GOBJ_USER_DATA_PTR  = 0x84; // -> ITStruct* (Item) or WPStruct* (Weapon)
constexpr uint8_t  GOBJ_LINK_ID_ITEM   = 4;
constexpr uint8_t  GOBJ_LINK_ID_WEAPON = 5;
// Both ITStruct and WPStruct happen to place `kind` at the same sub-offset
// (a coincidence of parallel struct design per the ram-map, not a rule).
constexpr uint32_t IT_OR_WP_STRUCT_KIND = 0x0C; // s32: ITKind or WPKind, per GOBJ_LINK_ID
constexpr uint32_t DOBJ_POSITION_X      = 0x1C;
constexpr uint32_t DOBJ_POSITION_Y      = 0x20;
constexpr uint32_t DOBJ_POSITION_Z      = 0x24;

// Slippi caps its own per-frame item event count at 15 (see
// rmgk-replay-file-agent-prompt.md section 4.4); reused here as a sane
// per-frame budget, mainly to bound a corrupt/cyclic list to a fixed number
// of reads rather than looping until something crashes.
constexpr int ITEM_LIST_MAX_OBJECTS = 32;

// Stage hazards. See smashremix docs/ram-map.md section 10.3 - Dream
// Land's live hazard state (Whispy's wind) lives in a fixed global that is
// a *union* shared by every "common ground" stage; these exact offsets are
// only valid when the current stage is actually Dream Land.
constexpr uint8_t  STAGE_ID_DREAM_LAND       = 0x06;
constexpr uint32_t ADDR_PUPUPU_WHISPY_STATUS = 0x80131416; // gGRCommonStruct (0x801313F0) + 0x26, Dream Land's union view
constexpr uint8_t  WHISPY_STATUS_BLOW        = 4;          // grPupupuWhispyWindStatus::Blow

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
    info.comboDamage              = m64p::Core.DebugMemRead32(base + PORT_COMBO_DAMAGE);
    info.comboHitCount             = m64p::Core.DebugMemRead32(base + PORT_COMBO_HIT_COUNT);
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

std::vector<ItemObject> ReadItemObjects(void)
{
    std::vector<ItemObject> objects;

    uint32_t current = m64p::Core.DebugMemRead32(ADDR_ITEM_LIST_HEAD);
    for (int i = 0; i < ITEM_LIST_MAX_OBJECTS && IsValidRdramPointer(current); i++)
    {
        const uint32_t next = m64p::Core.DebugMemRead32(current + GOBJ_NEXT);

        const uint8_t linkId = m64p::Core.DebugMemRead8(current + GOBJ_LINK_ID);
        if (linkId == GOBJ_LINK_ID_ITEM || linkId == GOBJ_LINK_ID_WEAPON)
        {
            ItemObject object{};
            object.objectAddress = current;
            object.linkId        = linkId;

            const uint32_t userData = m64p::Core.DebugMemRead32(current + GOBJ_USER_DATA_PTR);
            if (IsValidRdramPointer(userData))
            {
                object.kind = static_cast<int32_t>(m64p::Core.DebugMemRead32(userData + IT_OR_WP_STRUCT_KIND));
            }

            const uint32_t dObj = m64p::Core.DebugMemRead32(current + GOBJ_OBJ_PTR);
            if (IsValidRdramPointer(dObj))
            {
                object.positionX = ReadFloat(dObj + DOBJ_POSITION_X);
                object.positionY = ReadFloat(dObj + DOBJ_POSITION_Y);
                object.positionZ = ReadFloat(dObj + DOBJ_POSITION_Z);
            }
            objects.push_back(object);
        }
        // else: Fighter (3) or some other GObj kind - not an item/weapon,
        // and user_data isn't guaranteed to be an ITStruct*/WPStruct* for
        // those, so there's nothing meaningful to chase further.

        current = next;
    }

    return objects;
}

StageHazards ReadStageHazards(uint8_t stageId)
{
    StageHazards hazards{};
    if (stageId == STAGE_ID_DREAM_LAND)
    {
        hazards.whispyBlowing =
            m64p::Core.DebugMemRead8(ADDR_PUPUPU_WHISPY_STATUS) == WHISPY_STATUS_BLOW;
    }
    return hazards;
}
} // namespace ReplayMemory
