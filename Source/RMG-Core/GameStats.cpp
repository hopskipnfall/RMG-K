/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "GameStats.hpp"
#include "GameStatsProbes.hpp"
#include "GameStatsTypes.hpp"
#include "Callback.hpp"
#include "Library.hpp"

#include "m64p/Api.hpp"
#include "m64p/api/m64p_types.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifdef RMGK_HAVE_P2P_TRANSPORT
// Canonical slot(port)-indexed player-name table, already kept up to date by
// every netplay path (rollback lobby, legacy Kaillera, p2p core itself) as
// part of preparing the .krec recording header. Defined in
// Source/n02/n02_client.cpp; declared here directly (rather than including
// the internal n02 header) since this is the only symbol needed from it.
extern char recording_player_names[4][32];
#endif

namespace
{
#ifdef _WIN32
HANDLE s_MappingHandle = nullptr;
#else
int s_ShmFd = -1;
#endif

GameStatsSharedFrame* s_SharedFrame     = nullptr;
bool                  s_StatsEnabled    = false;
bool                  s_DebuggerChecked = false;
bool                  s_WarnedOnce      = false;

void warn_once(const std::string& message)
{
    if (s_WarnedOnce)
    {
        return;
    }
    s_WarnedOnce = true;
    CoreAddCallbackMessage(CoreDebugMessageType::Warning, message);
}

bool is_valid_pointer(uint32_t address)
{
    return address >= GameStatsAddresses::ValidPointerRangeStart &&
           address < GameStatsAddresses::ValidPointerRangeEnd;
}

float read_f32(uint32_t address)
{
    return std::bit_cast<float>(m64p::Core.DebugMemRead32(address));
}

// resolves and reads one port's live fighter data, following the chain:
// match info -> per-port match struct -> player object -> player struct
// -> position vector. Returns false (leaving `out` zeroed/inactive) when
// the port is empty or isn't currently in a live match.
bool read_player_frame(int port, GameStatsPlayerFrame& out)
{
    out = {};

    const uint32_t matchInfo = m64p::Core.DebugMemRead32(GameStatsAddresses::MatchInfoPtr);
    if (!is_valid_pointer(matchInfo))
    {
        return false;
    }

    const uint32_t matchStruct =
        matchInfo + GameStatsAddresses::PortArrayOffset + GameStatsAddresses::PortStride * static_cast<uint32_t>(port);

    const uint8_t slotType = m64p::Core.DebugMemRead8(matchStruct + GameStatsAddresses::Port_SlotType);
    if (slotType == 2) // empty port
    {
        return false;
    }
    out.isHuman = (slotType == 0) ? 1 : 0; // 0 = human, 1 = CPU per the match struct

    const uint32_t playerObject = m64p::Core.DebugMemRead32(matchStruct + GameStatsAddresses::Port_PlayerObject);
    if (!is_valid_pointer(playerObject)) // port occupied but not currently in a live match
    {
        return false;
    }

    const uint32_t playerStruct = m64p::Core.DebugMemRead32(playerObject + GameStatsAddresses::Object_PlayerStruct);
    if (!is_valid_pointer(playerStruct))
    {
        return false;
    }

    out.character = m64p::Core.DebugMemRead8(matchStruct + GameStatsAddresses::Port_Character);
    out.state      = m64p::Core.DebugMemRead32(playerStruct + GameStatsAddresses::Player_ActionState);

    // source field is a 32-bit int but only ever holds 1 or -1
    const int32_t facing = static_cast<int32_t>(m64p::Core.DebugMemRead32(playerStruct + GameStatsAddresses::Player_Facing));
    out.facingDirection   = static_cast<int8_t>(facing);

    const uint32_t positionVec = m64p::Core.DebugMemRead32(playerStruct + GameStatsAddresses::Player_PositionPtr);
    if (is_valid_pointer(positionVec))
    {
        out.positionX = read_f32(positionVec + 0x00);
        out.positionY = read_f32(positionVec + 0x04);
    }

#ifdef RMGK_HAVE_P2P_TRANSPORT
    // Room metadata, not game memory: empty ("") whenever this port isn't a
    // named netplay seat (offline play, no netplay session, CPU-filled slot
    // that was never assigned a seat, etc).
    std::memcpy(out.tag, recording_player_names[port], sizeof(out.tag));
    out.tag[sizeof(out.tag) - 1] = '\0';
#endif

    out.active = 1;
    return true;
}

// Temporary verification aid: throttled dump of the resolved per-port state
// to the normal debug log (visible in RMG's Log dialog, or on stdout with
// --debug-messages), so the addresses/chase in GameStatsProbes.hpp can be
// checked against what's actually on screen before anything consumes the
// shared-memory segment for real. Remove once that's confirmed.
constexpr unsigned int kLogIntervalFrames = 60; // ~once/sec at 60fps

void log_frame_snapshot(unsigned int frameIndex, const GameStatsPlayerFrame players[RMGK_GAMESTATS_MAX_PLAYERS])
{
    std::ostringstream stream;
    stream << "GameStats frame=" << frameIndex;

    for (int port = 0; port < RMGK_GAMESTATS_MAX_PLAYERS; port++)
    {
        const GameStatsPlayerFrame& player = players[port];
        stream << " | p" << port << ": ";
        if (!player.active)
        {
            stream << "inactive";
            continue;
        }
        stream << "char=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(player.character)
               << std::dec << " pos=(" << player.positionX << "," << player.positionY << ")"
               << " facing=" << static_cast<int>(player.facingDirection) << " state=0x" << std::hex << player.state
               << std::dec;
    }

    CoreAddCallbackMessage(CoreDebugMessageType::Info, stream.str());
}

bool create_shared_memory(void)
{
#ifdef _WIN32
    s_MappingHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(GameStatsSharedFrame), "Local\\" RMGK_GAMESTATS_SHM_NAME);
    if (s_MappingHandle == nullptr)
    {
        warn_once("CoreInitGameStats: CreateFileMappingA failed, game stats disabled");
        return false;
    }

    s_SharedFrame = static_cast<GameStatsSharedFrame*>(
        MapViewOfFile(s_MappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GameStatsSharedFrame)));
    if (s_SharedFrame == nullptr)
    {
        warn_once("CoreInitGameStats: MapViewOfFile failed, game stats disabled");
        CloseHandle(s_MappingHandle);
        s_MappingHandle = nullptr;
        return false;
    }
#else
    s_ShmFd = shm_open("/" RMGK_GAMESTATS_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (s_ShmFd == -1)
    {
        warn_once("CoreInitGameStats: shm_open failed, game stats disabled");
        return false;
    }

    if (ftruncate(s_ShmFd, sizeof(GameStatsSharedFrame)) == -1)
    {
        warn_once("CoreInitGameStats: ftruncate failed, game stats disabled");
        close(s_ShmFd);
        s_ShmFd = -1;
        return false;
    }

    s_SharedFrame = static_cast<GameStatsSharedFrame*>(
        mmap(nullptr, sizeof(GameStatsSharedFrame), PROT_READ | PROT_WRITE, MAP_SHARED, s_ShmFd, 0));
    if (s_SharedFrame == MAP_FAILED)
    {
        warn_once("CoreInitGameStats: mmap failed, game stats disabled");
        close(s_ShmFd);
        s_ShmFd = -1;
        s_SharedFrame = nullptr;
        return false;
    }
#endif

    std::memset(s_SharedFrame, 0, sizeof(GameStatsSharedFrame));
    s_SharedFrame->magic       = RMGK_GAMESTATS_MAGIC;
    s_SharedFrame->version     = RMGK_GAMESTATS_VERSION;
    s_SharedFrame->playerCount = RMGK_GAMESTATS_MAX_PLAYERS;
    return true;
}

void destroy_shared_memory(void)
{
    if (s_SharedFrame != nullptr)
    {
#ifdef _WIN32
        UnmapViewOfFile(s_SharedFrame);
#else
        munmap(s_SharedFrame, sizeof(GameStatsSharedFrame));
#endif
        s_SharedFrame = nullptr;
    }

#ifdef _WIN32
    if (s_MappingHandle != nullptr)
    {
        CloseHandle(s_MappingHandle);
        s_MappingHandle = nullptr;
    }
#else
    if (s_ShmFd != -1)
    {
        close(s_ShmFd);
        shm_unlink("/" RMGK_GAMESTATS_SHM_NAME);
        s_ShmFd = -1;
    }
#endif
}
} // namespace

CORE_EXPORT void CoreInitGameStats(void)
{
    s_WarnedOnce      = false;
    s_StatsEnabled    = false;
    s_DebuggerChecked = false;

    if (!m64p::Core.IsHooked() || m64p::Core.DebugMemGetPointer == nullptr)
    {
        warn_once("CoreInitGameStats: core debugger API not available, game stats disabled");
        return;
    }

    // Whether the core was actually built with DEBUGGER=1 can't be checked
    // here yet: DebugMemGetPointer(RDRAM) only returns non-NULL once RDRAM
    // has been allocated, which happens later on the emulation thread (in
    // main_run() -> init_device()), not during this synchronous setup call.
    // That check happens lazily on the first CoreUpdateGameStats() instead,
    // by which point a frame has actually run.
    if (!create_shared_memory())
    {
        return;
    }

    s_StatsEnabled = true;
}

CORE_EXPORT void CoreStopGameStats(void)
{
    destroy_shared_memory();
    s_StatsEnabled = false;
}

CORE_EXPORT void CoreUpdateGameStats(unsigned int frameIndex)
{
    if (!s_StatsEnabled || s_SharedFrame == nullptr)
    {
        return;
    }

    if (!s_DebuggerChecked)
    {
        s_DebuggerChecked = true;
        // DebugMem* functions are always exported, but only functional
        // (return non-NULL/non-zero) when the core was built with
        // DEBUGGER=1. RDRAM is guaranteed to exist by now since a frame is
        // actually running.
        if (m64p::Core.DebugMemGetPointer(M64P_DBG_PTR_RDRAM) == nullptr)
        {
            warn_once("CoreUpdateGameStats: core was not built with DEBUGGER=1, game stats disabled");
            s_StatsEnabled = false;
            return;
        }
    }

    GameStatsPlayerFrame players[RMGK_GAMESTATS_MAX_PLAYERS] = {};

    for (int port = 0; port < RMGK_GAMESTATS_MAX_PLAYERS; port++)
    {
        read_player_frame(port, players[port]);
    }

    if (frameIndex % kLogIntervalFrames == 0)
    {
        log_frame_snapshot(frameIndex, players);
    }

    // seqlock: bump to odd (writer in progress), write fields, bump to even
    // (stable). A reader retries whenever it observes an odd sequence or
    // the value changes between its own before/after reads.
    std::atomic_ref<uint32_t> sequence(s_SharedFrame->sequence);
    sequence.fetch_add(1, std::memory_order_release);

    s_SharedFrame->frameIndex = frameIndex;
    std::memcpy(s_SharedFrame->players, players, sizeof(players));

    sequence.fetch_add(1, std::memory_order_release);
}
