/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_GAMESTATSPROBES_HPP
#define CORE_GAMESTATSPROBES_HPP

// N64 memory addresses/offsets for Smash Remix 2.0.1. This is the file to
// edit if the mod updates and these shift, or to retarget a different game
// entirely.
//
// These values come from a hand-written RAM map derived from the mod's
// assembly source (see docs/superpowers/specs for the session that added
// this). All addresses are N64 virtual addresses (KSEG0, 0x80000000-based),
// used directly with the mupen64plus debugger API. That API already returns
// host-byte-order values (see m64p_debugger.h), so unlike a raw-RDRAM-buffer
// reader, no manual endianness/word-swap correction is needed here.
//
// GameStats.cpp resolves a player's live data as a pointer chase, not a
// flat table, because the game stores it that way:
//
//   MI              = *MatchInfoPtr                          (match info block)
//   matchStruct     = MI + PortArrayOffset + port*PortStride  (per-port, cheap: character/stocks/damage)
//   playerObject    = *(matchStruct + Port_PlayerObjectOffset)  (0 => port not currently in a match)
//   playerStruct    = *(playerObject + Object_PlayerStructOffset)
//   ... read state/facing/position fields directly off playerStruct ...
//   positionVec     = *(playerStruct + Player_PositionPtrOffset)
//   x, y            = f32 at positionVec + 0x00 / + 0x04

#include <cstdint>

namespace GameStatsAddresses
{
// --- globals ---
inline constexpr uint32_t MatchInfoPtr = 0x800A50E8; // ptr -> match info block (MI)

// --- match info block (relative to MI) ---
inline constexpr uint32_t PortArrayOffset = 0x20; // start of the per-port match struct array
inline constexpr uint32_t PortStride      = 0x74; // bytes between consecutive ports' match structs
inline constexpr uint32_t MaxPorts        = 4;

// --- per-port match struct (relative to a port's own base) ---
inline constexpr uint32_t Port_SlotType     = 0x02; // u8: 0 human, 1 CPU, 2 empty
inline constexpr uint32_t Port_Character    = 0x03; // u8: character id, valid range 0x00-0x60
inline constexpr uint32_t Port_PlayerObject = 0x58; // ptr -> player object; 0 = port not in a live match

// --- player object -> player struct ---
inline constexpr uint32_t Object_PlayerStruct = 0x84; // ptr

// --- player struct (relative to the struct itself) ---
inline constexpr uint32_t Player_ActionState  = 0x24; // u32: action/state id
inline constexpr uint32_t Player_Facing       = 0x44; // i32: 1 = right, -1 = left (integer, not float)
inline constexpr uint32_t Player_PositionPtr  = 0x78; // ptr -> [x:f32, y:f32, z:f32], world-space stage coords

// pointers into RDRAM live in 0x80000000-0x80800000 (KSEG0, 8MB expansion pak).
// anything outside that range read back from a "pointer" field means the
// chase hit garbage/uninitialized memory and should be treated as absent.
inline constexpr uint32_t ValidPointerRangeStart = 0x80000000;
inline constexpr uint32_t ValidPointerRangeEnd   = 0x80800000; // exclusive
} // namespace GameStatsAddresses

#endif // CORE_GAMESTATSPROBES_HPP
