/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_GAMESTATSTYPES_HPP
#define CORE_GAMESTATSTYPES_HPP

// Public shared-memory contract for the per-frame game stats channel.
//
// This header is intentionally self-contained (no other RMG-Core includes)
// so it can be copied as-is into a separate reader application.
//
// The segment is published under the name RMGK_GAMESTATS_SHM_NAME:
//   - POSIX: shm_open("/RMGK_GameStats", ...)
//   - Windows: CreateFileMappingA(..., "Local\\RMGK_GameStats")
//
// Reader protocol (seqlock): read `sequence`, read the rest of the struct,
// read `sequence` again. If either read observed an odd value, or the two
// reads of `sequence` differ, the frame was torn mid-write — discard it and
// retry. This lets the writer publish every frame without ever blocking on
// a reader.

#include <cstdint>

#define RMGK_GAMESTATS_SHM_NAME "RMGK_GameStats"
#define RMGK_GAMESTATS_MAGIC 0x4B474D52u // 'RMGK' (little-endian bytes)
#define RMGK_GAMESTATS_VERSION 2u
#define RMGK_GAMESTATS_MAX_PLAYERS 4

#pragma pack(push, 1)

struct GameStatsPlayerFrame
{
    uint8_t  active;          // 1 = this port has a live fighter this frame;
                               // all other fields are only meaningful when active == 1
    uint8_t  character;
    int8_t   facingDirection; // 1 = right, -1 = left
    float    positionX;
    float    positionY;
    uint32_t state;           // action/state id
};

struct GameStatsSharedFrame
{
    uint32_t magic;       // RMGK_GAMESTATS_MAGIC
    uint32_t version;     // RMGK_GAMESTATS_VERSION, bump on layout change
    uint32_t sequence;    // seqlock: odd = writer mid-update, even = stable
    uint32_t frameIndex;
    uint32_t playerCount; // always RMGK_GAMESTATS_MAX_PLAYERS (array length);
                           // check each entry's `active` flag for whether a
                           // port actually has a fighter in it right now
    GameStatsPlayerFrame players[RMGK_GAMESTATS_MAX_PLAYERS];
};

#pragma pack(pop)

#endif // CORE_GAMESTATSTYPES_HPP
