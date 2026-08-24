# Replay File Format Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new, independent per-match "replay file" (`.rmgr`) recording feature to RMG-K that captures per-frame controller inputs and Smash-Remix game state via mupen64plus-core's `DEBUGGER=1` memory API, using a Slippi-style event stream, without touching the existing `.krec` format.

**Architecture:** Two new RMG-Core files — `ReplayMemory.{hpp,cpp}` (isolated N64-memory pointer-chase reader, wraps `m64p::Core.DebugMemRead8/16/32`) and `Replay.{hpp,cpp}` (event-stream file writer + per-match state machine) — hooked into the existing `FrameCallback()`/`CoreStartEmulation()`/`CoreStopEmulation()` lifecycle in `Emulation.cpp`. A new `GAME_STATS` CMake option gates both the expensive `DEBUGGER=1` mupen64plus-core rebuild (binutils dependency) and a `RMGK_GAME_STATS` compile define; a single persistent Settings checkbox (not per-dialog, unlike krec) enables/disables recording for both online and offline matches.

**Tech Stack:** C++20, CMake, Qt6 (`.ui` Designer file for one new checkbox), mupen64plus-core's `DebugMemRead8/16/32` debugger API.

## Global Constraints

- krec (`Source/n02/`) must remain **completely untouched** — no edits to any file under `Source/n02/`.
- The new feature has its **own independent enable/disable setting**, distinct from `Kaillera_RecordingEnabled`.
- **No FlatBuffers** — plain 1-byte-command + fixed-size-payload event stream only, matching the already-agreed design in the handoff doc (`/Users/ness/workspaces/rmgk-replay-file-agent-prompt.md`, §2, §3.3, §4).
- **Little-endian**, native struct layout (`#pragma pack(push,1)`), no manual byte-swapping — matches this project's existing convention.
- File length field is written as `0` at file open and patched to the real value only when the match's recording is finalized (crash-safety pattern from §4.6 of the handoff doc).
- **No Claude/Anthropic attribution** in any commit message.
- This machine (macOS/Darwin) cannot do a full native build of this Linux/Windows-targeted project (no X11/mupen64plus Linux build path, `DEBUGGER=1` needs GNU binutils dev headers not present on macOS). Per-task verification uses direct `-fsyntax-only`-style compiler invocations and `cmake` configure-only checks (mirroring the doc's own local-verification technique, §3.2); **full build + in-emulator smoke test is deferred to CI / a Linux or Windows machine** and is called out explicitly, not silently skipped.
- Every commit message explains *why*, not just *what* (established project norm, doc §7). Commit locally after each task; **do not push** without the user's explicit go-ahead.

---

### Task 1: CMake plumbing — `GAME_STATS` option + `RMGK_GAME_STATS` define + Settings entry

**Files:**
- Modify: `CMakeLists.txt:10` (root)
- Modify: `Source/3rdParty/CMakeLists.txt:135`
- Modify: `Source/RMG-Core/CMakeLists.txt:62-66`
- Modify: `Source/RMG/CMakeLists.txt:35-37`
- Modify: `Source/RMG-Core/Settings.hpp:560-561` (enum)
- Modify: `Source/RMG-Core/Settings.cpp:77` (section macro), `Source/RMG-Core/Settings.cpp:1651-1653` (case)

**Interfaces:**
- Produces: CMake option `GAME_STATS` (default `OFF`). Compile define `RMGK_GAME_STATS`, visible in both the `RMG-Core` and `RMG` targets when `GAME_STATS=ON`. `SettingsID::GameStats_ReplayEnabled` (bool, default `false`, config key `[Rosalie's Mupen GUI GameStats] ReplayEnabled`).

- [ ] **Step 1: Add the `GAME_STATS` option to the root CMakeLists.txt**

In `CMakeLists.txt`, right after line 10 (`option(NETPLAY "Enables netplay" ON)`), add:

```cmake
option(GAME_STATS       "Enables replay-file recording via mupen64plus-core's DEBUGGER=1 memory API (pulls in binutils as a link dependency)" OFF)
```

- [ ] **Step 2: Gate `DEBUGGER=1` behind the new option**

In `Source/3rdParty/CMakeLists.txt:135`, change:

```cmake
        DEBUGGER=1
```
to:
```cmake
        DEBUGGER=$<BOOL:${GAME_STATS}>
```

(This is the exact same generator-expression pattern already used one line above it for `NETPLAY=$<BOOL:${NETPLAY}>` — same file, same `ExternalProject_Add` block, already proven to work.)

- [ ] **Step 3: Define `RMGK_GAME_STATS` in `Source/RMG-Core/CMakeLists.txt`**

In `Source/RMG-Core/CMakeLists.txt`, between the existing `PORTABLE_INSTALL` block and `add_library`:

```cmake
if (PORTABLE_INSTALL)
    add_definitions(-DPORTABLE_INSTALL)
endif(PORTABLE_INSTALL)

add_library(RMG-Core SHARED ${RMG_CORE_SOURCES})
```
becomes:
```cmake
if (PORTABLE_INSTALL)
    add_definitions(-DPORTABLE_INSTALL)
endif(PORTABLE_INSTALL)

if (GAME_STATS)
    add_definitions(-DRMGK_GAME_STATS)
endif(GAME_STATS)

add_library(RMG-Core SHARED ${RMG_CORE_SOURCES})
```

Do **not** add `Replay.cpp`/`ReplayMemory.cpp` to `RMG_CORE_SOURCES` yet — those files don't exist until Task 2/3. That line gets added in Task 3.

- [ ] **Step 4: Define `RMGK_GAME_STATS` in `Source/RMG/CMakeLists.txt`**

CMake's `add_definitions()` is directory-scoped and does **not** propagate from `Source/RMG-Core/CMakeLists.txt` to `Source/RMG/CMakeLists.txt` (confirmed: `NETPLAY`'s `-DNETPLAY` define is independently re-declared in both files today). So this needs its own block. In `Source/RMG/CMakeLists.txt`, between the `NETPLAY` block and the `find_package(SDL3 REQUIRED)` line:

```cmake
if (NETPLAY)
    find_package(Qt6 COMPONENTS WebSockets Network REQUIRED)
    add_definitions(-DNETPLAY)
endif(NETPLAY)

find_package(SDL3 REQUIRED)
```
becomes:
```cmake
if (NETPLAY)
    find_package(Qt6 COMPONENTS WebSockets Network REQUIRED)
    add_definitions(-DNETPLAY)
endif(NETPLAY)

if (GAME_STATS)
    add_definitions(-DRMGK_GAME_STATS)
endif(GAME_STATS)

find_package(SDL3 REQUIRED)
```

- [ ] **Step 5: Add the `GameStats_ReplayEnabled` setting**

In `Source/RMG-Core/Settings.hpp`, the `SettingsID` enum currently ends:
```cpp
    GCAInput_Map_CUp,
    GCAInput_Map_CDown,
    GCAInput_Map_CLeft,
    GCAInput_Map_CRight,

    // Internal Settings (not persisted to config file)
    Internal_InputPluginSwitchRequested,

    Invalid
};
```
Change to:
```cpp
    GCAInput_Map_CUp,
    GCAInput_Map_CDown,
    GCAInput_Map_CLeft,
    GCAInput_Map_CRight,

    // Game Stats / Replay Settings
    GameStats_ReplayEnabled,

    // Internal Settings (not persisted to config file)
    Internal_InputPluginSwitchRequested,

    Invalid
};
```

In `Source/RMG-Core/Settings.cpp`, add a new section macro after line 77 (`#define SETTING_SECTION_KAILLERA        SETTING_SECTION_GUI  " Kaillera"`):
```cpp
#define SETTING_SECTION_GAMESTATS       SETTING_SECTION_GUI  " GameStats"
```

Then in the `get_setting()` switch, currently:
```cpp
        setting = {SETTING_SECTION_GCA, "Map_CRight", 2};
        break;

    // Internal settings (runtime-only, not persisted)
    case SettingsID::Internal_InputPluginSwitchRequested:
```
becomes:
```cpp
        setting = {SETTING_SECTION_GCA, "Map_CRight", 2};
        break;

    // Game Stats / Replay Settings
    case SettingsID::GameStats_ReplayEnabled:
        setting = {SETTING_SECTION_GAMESTATS, "ReplayEnabled", false};
        break;

    // Internal settings (runtime-only, not persisted)
    case SettingsID::Internal_InputPluginSwitchRequested:
```

- [ ] **Step 6: Verify — CMake configures cleanly both ways**

```bash
cmake -S /Users/ness/workspaces/RMG-K -B /tmp/rmgk-cmake-off -DGAME_STATS=OFF -DUPDATER=OFF 2>&1 | tail -20
cmake -S /Users/ness/workspaces/RMG-K -B /tmp/rmgk-cmake-on  -DGAME_STATS=ON  -DUPDATER=OFF 2>&1 | tail -20
grep "^GAME_STATS:BOOL" /tmp/rmgk-cmake-off/CMakeCache.txt
grep "^GAME_STATS:BOOL" /tmp/rmgk-cmake-on/CMakeCache.txt
```
Expected: both configure runs finish without a CMake error (they'll print normal `-- Found ...` / `-- Configuring done` output — don't expect a full build here, just configure success), the first cache shows `GAME_STATS:BOOL=OFF`, the second shows `GAME_STATS:BOOL=ON`. Clean up afterward:
```bash
rm -rf /tmp/rmgk-cmake-off /tmp/rmgk-cmake-on
```

- [ ] **Step 7: Commit**

```bash
cd /Users/ness/workspaces/RMG-K
git add CMakeLists.txt Source/3rdParty/CMakeLists.txt Source/RMG-Core/CMakeLists.txt Source/RMG/CMakeLists.txt Source/RMG-Core/Settings.hpp Source/RMG-Core/Settings.cpp
git commit -m "$(cat <<'EOF'
Gate DEBUGGER=1 behind a new GAME_STATS CMake option

DEBUGGER=1 was unconditional, permanently pulling in binutils
(libopcodes/libbfd, plus libsframe/zstd/libintl/zlib transitively on
Windows) as a link dependency for every RMG-K build even though
nothing used the debugger API yet. Make it opt-in (default OFF) so
only builds that actually want replay-file recording pay that cost.

Also adds the GameStats_ReplayEnabled setting the recording feature
will read to decide whether to record a match.
EOF
)"
```

---

### Task 2: `ReplayMemory` — the N64 memory pointer-chase reader

**Files:**
- Create: `Source/RMG-Core/ReplayMemory.hpp`
- Create: `Source/RMG-Core/ReplayMemory.cpp`

**Interfaces:**
- Consumes: `m64p::Core.DebugMemRead8(uint32_t)/DebugMemRead16(uint32_t)/DebugMemRead32(uint32_t)` (declared in `Source/RMG-Core/m64p/CoreApi.hpp`, global instance `m64p::Core` from `Source/RMG-Core/m64p/Api.hpp`) — already hooked up, no-ops (return 0) unless the core was built with `DEBUGGER=1`.
- Produces (all in `namespace ReplayMemory`, used by Task 3's `Replay.cpp`):
  - `struct PortMatchInfo { uint8_t slotType; bool seated; bool isCpu; uint8_t characterId; uint8_t costumeId; uint8_t teamColor; int8_t stocksRemaining; };`
  - `struct PortPlayerState { bool valid; uint8_t characterId; uint16_t actionStateId; uint32_t actionFrameCounter; int32_t facingDirection; float velocityX; float velocityY; float positionX; float positionY; uint32_t jumpsUsed; uint8_t groundedState; uint16_t processedButtons; int8_t stickX; int8_t stickY; uint8_t hurtboxState; uint16_t hitstunCounter; uint32_t damagePercent; };`
  - `struct MatchInfo { bool valid; uint32_t matchInfoPtr; uint8_t gameMode; uint8_t stageId; uint8_t gameType; uint8_t timeLimitMinutes; uint8_t stockCountSetting; uint8_t damageRatio; uint8_t itemFrequency; uint8_t gameStatus; bool matchWasReset; };`
  - `bool IsInVsMatchScreen(void);`
  - `MatchInfo ReadMatchInfo(void);`
  - `PortMatchInfo ReadPortMatchInfo(uint32_t matchInfoPtr, int port);`
  - `PortPlayerState ReadPortPlayerState(uint32_t matchInfoPtr, int port);`

All addresses/offsets below are taken verbatim from §6 of `/Users/ness/workspaces/rmgk-replay-file-agent-prompt.md` — do not re-derive or guess at them.

- [ ] **Step 1: Write `Source/RMG-Core/ReplayMemory.hpp`**

```cpp
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
    uint8_t slotType;   // 0 human, 1 CPU, 2 empty (raw value read from memory)
    bool    seated;     // slotType != 2
    bool    isCpu;      // slotType == 1
    uint8_t characterId;
    uint8_t costumeId;
    uint8_t teamColor;
    int8_t  stocksRemaining;
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
```

- [ ] **Step 2: Write `Source/RMG-Core/ReplayMemory.cpp`**

```cpp
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
constexpr uint32_t MI_GAME_TYPE          = 0x03;
constexpr uint32_t MI_TIME_LIMIT         = 0x06;
constexpr uint32_t MI_STOCK_COUNT        = 0x07;
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

    uint32_t positionPtr = m64p::Core.DebugMemRead32(playerStruct + PS_POSITION_PTR);
    if (IsValidRdramPointer(positionPtr))
    {
        state.positionX = ReadFloat(positionPtr + 0x00);
        state.positionY = ReadFloat(positionPtr + 0x04);
    }

    return state;
}
} // namespace ReplayMemory
```

- [ ] **Step 3: Verify — syntax-only compile**

These two files only depend on RMG-Core's own vendored headers and the standard library (no Qt, no SDL, no Windows headers), so a plain native compile works without any MSYS2/mingw package downloads:

```bash
cd /Users/ness/workspaces/RMG-K
clang++ -std=c++20 -fsyntax-only -I Source/RMG-Core -DRMGK_GAME_STATS Source/RMG-Core/ReplayMemory.cpp
echo "exit code: $?"
```
Expected: `exit code: 0`, no diagnostics printed. If it fails on a missing header, check whether it's reaching for something outside `Source/RMG-Core` (it shouldn't need to) before adding more `-I` paths.

- [ ] **Step 4: Commit**

```bash
cd /Users/ness/workspaces/RMG-K
git add Source/RMG-Core/ReplayMemory.hpp Source/RMG-Core/ReplayMemory.cpp
git commit -m "$(cat <<'EOF'
Add ReplayMemory: Smash Remix RAM-layout reader for replay recording

Isolates the hand-derived pointer-chase (match info block, per-port
match struct, player object -> player struct, controller/position
sub-structs) from the event-stream writer that will consume it, so
the "hairy N64 addresses" stay in one small, independently reasoned
about file. All addresses come from prior RAM-map research reproduced
in rmgk-replay-file-agent-prompt.md section 6, not re-derived here.
EOF
)"
```

---

### Task 3: `Replay` — event-stream writer and per-match state machine

**Files:**
- Create: `Source/RMG-Core/Replay.hpp`
- Create: `Source/RMG-Core/Replay.cpp`
- Modify: `Source/RMG-Core/CMakeLists.txt` (the `if (GAME_STATS)` block added in Task 1, Step 3)

**Interfaces:**
- Consumes: everything from Task 2's `ReplayMemory` namespace; `CoreSettingsGetBoolValue(SettingsID)` / `SettingsID::GameStats_ReplayEnabled` (Task 1); `CoreGetUserDataDirectory()` (`Source/RMG-Core/Directories.hpp`, already exists); `recording_player_names` global (`char[4][32]`, declared in `Source/n02/kailleraclient.h`, only when `RMGK_HAVE_P2P_TRANSPORT` is defined — guard every reference).
- Produces (used by Task 5's `Emulation.cpp`): `namespace Replay { void OnEmulationStart(void); void OnEmulationStop(void); void OnFrame(void); }`

- [ ] **Step 1: Write `Source/RMG-Core/Replay.hpp`**

```cpp
/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef REPLAY_HPP
#define REPLAY_HPP

// New, independent per-match ".rmgr" replay recorder. Entirely separate
// from Source/n02's .krec format - does not read, write, or otherwise
// touch anything under Source/n02/.
namespace Replay
{
// Call once, right after CoreStartEmulation registers the frame callback.
// Reads SettingsID::GameStats_ReplayEnabled and arms (or doesn't) the
// per-match state machine driven by OnFrame().
void OnEmulationStart(void);

// Call from CoreStopEmulation. Finalizes (patches the length field and
// closes) any file still open, e.g. if the user quit mid-match.
void OnEmulationStop(void);

// Call once per real emulated frame (i.e. from the same place krec's
// frame-counter hook lives) - never during GekkoNet rollback resimulation.
// Owns the whole "waiting for a match to start" / "recording" state
// machine; safe to call every frame regardless of current state.
void OnFrame(void);
} // namespace Replay

#endif // REPLAY_HPP
```

- [ ] **Step 2: Write `Source/RMG-Core/Replay.cpp`**

```cpp
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
#include "Directories.hpp"
#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "n02_client.h"
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
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
};
static_assert(sizeof(GameStartEvent) == 150, "GameStartEvent must be 150 bytes");

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
};
static_assert(sizeof(PostFrameEvent) == 42, "PostFrameEvent must be 42 bytes");

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
    tm* localNow = localtime(&now);
    char datePart[16];
    strftime(datePart, sizeof(datePart), "%y%m%d%H%M%S", localNow);

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

void OpenNewFile(const ReplayMemory::MatchInfo& matchInfo)
{
    std::filesystem::path directory = CoreGetUserDataDirectory() / "Replays";
    std::filesystem::create_directories(directory);

    std::filesystem::path path = directory / BuildFileName();
    s_File.open(path, std::ios::binary | std::ios::trunc);
    if (!s_File.is_open())
    {
        return;
    }

    FileHeader header{};
    std::memcpy(header.magic, "RMGR", 4);
    header.version = 1;
    header.streamLength = 0;
    s_File.write(reinterpret_cast<const char*>(&header), sizeof(header));

    s_StreamBytesWritten = 0;
    WriteEventPayloadsEvent();

    GameStartEvent startEvent{};
    startEvent.stageId           = matchInfo.stageId;
    startEvent.gameType          = matchInfo.gameType;
    startEvent.stockCountSetting = matchInfo.stockCountSetting;
    startEvent.timeLimitMinutes  = matchInfo.timeLimitMinutes;
    startEvent.damageRatio       = matchInfo.damageRatio;
    startEvent.itemFrequency     = matchInfo.itemFrequency;

    for (int port = 0; port < 4; port++)
    {
        ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
        startEvent.ports[port].slotType    = portInfo.slotType;
        startEvent.ports[port].characterId = portInfo.characterId;
        startEvent.ports[port].costumeId   = portInfo.costumeId;
        startEvent.ports[port].teamColor   = portInfo.teamColor;
    }

#ifdef RMGK_HAVE_P2P_TRANSPORT
    std::memcpy(startEvent.playerNames, recording_player_names, sizeof(startEvent.playerNames));
#endif

    WriteEvent(EventCode::GameStart, startEvent);

    s_FrameNumber = 0;
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
        WriteEvent(EventCode::PostFrame, post);
    }

    s_FrameNumber++;
}
} // namespace

namespace Replay
{
void OnEmulationStart(void)
{
    bool enabled = CoreSettingsGetBoolValue(SettingsID::GameStats_ReplayEnabled);
    s_State = enabled ? State::WaitingForMatch : State::Idle;
    s_FrameNumber = 0;
    s_StreamBytesWritten = 0;
}

void OnEmulationStop(void)
{
    if (s_State == State::Recording)
    {
        ReplayMemory::MatchInfo matchInfo = ReplayMemory::ReadMatchInfo();
        FinalizeFile(0 /* aborted: emulation stopped mid-match */, matchInfo);
    }
    s_State = State::Idle;
}

void OnFrame(void)
{
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
            OpenNewFile(matchInfo);
            s_State = State::Recording;
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
```

- [ ] **Step 3: Add the two new files to `Source/RMG-Core/CMakeLists.txt`**

The `if (GAME_STATS)` block from Task 1, Step 3 currently reads:
```cmake
if (GAME_STATS)
    add_definitions(-DRMGK_GAME_STATS)
endif(GAME_STATS)
```
Change to:
```cmake
if (GAME_STATS)
    list(APPEND RMG_CORE_SOURCES
        ReplayMemory.cpp
        Replay.cpp
    )
    add_definitions(-DRMGK_GAME_STATS)
endif(GAME_STATS)
```
This must run before `add_library(RMG-Core SHARED ${RMG_CORE_SOURCES})`, which it already does (that block sits directly above `add_library` — see Task 1 Step 3).

- [ ] **Step 4: Verify — struct layout via a standalone size check**

This compiles and *runs* natively (no project include paths needed at all), and is the closest thing to a real unit test available for this file: it locks down the exact on-disk byte layout independently of the full `Replay.cpp` (which needs RMG-Core headers to build).

```bash
cat > /tmp/rmgr_layout_check.cpp << 'EOF'
#include <cstdint>
#include <cstdio>

#pragma pack(push, 1)
struct FileHeader { char magic[4]; uint8_t version; uint8_t reserved[3]; uint32_t streamLength; };
struct GameStartPortInfo { uint8_t slotType; uint8_t characterId; uint8_t costumeId; uint8_t teamColor; };
struct GameStartEvent { uint8_t stageId; uint8_t gameType; uint8_t stockCountSetting; uint8_t timeLimitMinutes; uint8_t damageRatio; uint8_t itemFrequency; GameStartPortInfo ports[4]; char playerNames[4][32]; };
struct PreFrameEvent { int32_t frame; uint8_t port; uint16_t buttons; int8_t stickX; int8_t stickY; };
struct PostFrameEvent { int32_t frame; uint8_t port; uint8_t characterId; uint16_t actionStateId; float positionX; float positionY; int32_t facingDirection; float velocityX; float velocityY; uint32_t damagePercent; int8_t stocksRemaining; uint8_t jumpsUsed; uint8_t groundedState; uint8_t hurtboxState; uint16_t hitstunCounter; uint32_t actionFrameCounter; };
struct GameEndEvent { uint8_t endReason; int8_t placements[4]; };
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 12);
static_assert(sizeof(GameStartPortInfo) == 4);
static_assert(sizeof(GameStartEvent) == 150);
static_assert(sizeof(PreFrameEvent) == 9);
static_assert(sizeof(PostFrameEvent) == 42);
static_assert(sizeof(GameEndEvent) == 5);

int main() { printf("all layouts OK\n"); return 0; }
EOF
clang++ -std=c++20 /tmp/rmgr_layout_check.cpp -o /tmp/rmgr_layout_check && /tmp/rmgr_layout_check
rm -f /tmp/rmgr_layout_check.cpp /tmp/rmgr_layout_check
```
Expected: `all layouts OK`. (These are the exact same struct definitions as in `Replay.cpp` — if you changed a field while implementing Step 2, update this check to match, don't just delete the assert.)

- [ ] **Step 5: Verify — syntax-only compile of `Replay.cpp`**

```bash
cd /Users/ness/workspaces/RMG-K
clang++ -std=c++20 -fsyntax-only -I Source/RMG-Core -DRMGK_GAME_STATS Source/RMG-Core/Replay.cpp
echo "exit code: $?"
```
Expected: `exit code: 0`. This intentionally compiles *without* `-DRMGK_HAVE_P2P_TRANSPORT`, so it also exercises the `#ifdef`-disabled path (no `recording_player_names` reference) — check both configurations if you have a way to stub `n02_client.h` locally; otherwise note in the task summary that the `RMGK_HAVE_P2P_TRANSPORT` branch is untested locally and defer to CI.

- [ ] **Step 6: Commit**

```bash
cd /Users/ness/workspaces/RMG-K
git add Source/RMG-Core/Replay.hpp Source/RMG-Core/Replay.cpp Source/RMG-Core/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add Replay: Slippi-style event-stream writer for .rmgr replay files

Slippi-modeled design agreed in the handoff doc: 1-byte command code +
fixed payload per event, an Event Payloads event declaring every other
event's size up front for forward compatibility, and a length field
kept at 0 and patched at match end for crash-safety during live
recording. Explicitly not FlatBuffers (see handoff doc section 3.3).

The per-match state machine lives entirely in OnFrame(), driven by
ReplayMemory's game_status/current_screen reads - open a new file
when a VS match starts, write Pre/Post-Frame events every real frame,
finalize on a 1->5 game_status transition or on unexpected screen
change / emulation stop.
EOF
)"
```

---

### Task 4: Settings UI — independent "Record replay" checkbox

**Files:**
- Modify: `Source/RMG/UserInterface/Dialog/SettingsDialog.ui`
- Modify: `Source/RMG/UserInterface/Dialog/SettingsDialog.cpp:848-849`, `:1114-1115`, `:1409`

**Interfaces:**
- Consumes: `SettingsID::GameStats_ReplayEnabled` (Task 1); `CoreSettingsGetBoolValue`/`CoreSettingsGetDefaultBoolValue`/`CoreSettingsSetValue` (already exist, generic).
- Produces: a `QCheckBox` named `replayEnabledCheckBox` in the `coreTab` page of `SettingsDialog.ui`, wired to `GameStats_ReplayEnabled`. This is deliberately **not** a per-lobby-dialog checkbox like krec's `m64p_recordCheck` — it's a single persistent setting, since this feature (unlike krec) also applies to offline play, which has no "lobby" to hang a per-session checkbox off of.

- [ ] **Step 1: Add the checkbox to `SettingsDialog.ui`**

In `Source/RMG/UserInterface/Dialog/SettingsDialog.ui`, the `coreTab` page's main `verticalLayout_24` currently ends with an "Changes will be applied on next emulation run" info row and then closes:
```xml
             <item>
              <widget class="QLabel" name="label_49">
               ...
               <property name="text">
                <string>Changes will be applied on next emulation run</string>
               </property>
               ...
              </widget>
             </item>
            </layout>
           </item>
          </layout>
         </widget>
         <widget class="QWidget" name="gameTab">
```
Insert a new groupbox `<item>` right before that final `</layout></item></layout></widget>` close of `coreTab` (i.e. right after the `emulationInfoLayout_0` item's closing `</item>`, before the outer `</layout>` that closes `verticalLayout_24`):
```xml
           <item>
            <widget class="QGroupBox" name="replaySettingsGroupBox">
             <property name="title">
              <string>Replay Recording</string>
             </property>
             <layout class="QVBoxLayout" name="replaySettingsLayout">
              <item>
               <widget class="QCheckBox" name="replayEnabledCheckBox">
                <property name="text">
                 <string>Record replay file (.rmgr)</string>
                </property>
                <property name="toolTip">
                 <string>Records per-frame inputs and game state for the current match to a .rmgr file, separate from krec. Works for both online and offline matches.</string>
                </property>
               </widget>
              </item>
             </layout>
            </widget>
           </item>
          </layout>
         </widget>
         <widget class="QWidget" name="gameTab">
```
(This groupbox is always present in the `.ui` regardless of `GAME_STATS` — Qt Designer forms aren't conditionally compiled per-widget. Step 3 below makes it visibly disabled with an explanatory tooltip in builds where `RMGK_GAME_STATS` isn't defined, matching how the rest of this dialog handles settings that aren't always meaningful.)

- [ ] **Step 2: Populate the checkbox when the dialog opens**

In `SettingsDialog.cpp`, currently:
```cpp
    this->kailleraRecordByDefaultCheckBox->setChecked(
        CoreSettingsGetBoolValue(SettingsID::Kaillera_RecordingEnabled));
    this->kailleraPortSpinBox->setValue(kailleraPort);
```
becomes:
```cpp
    this->kailleraRecordByDefaultCheckBox->setChecked(
        CoreSettingsGetBoolValue(SettingsID::Kaillera_RecordingEnabled));
#ifdef RMGK_GAME_STATS
    this->replayEnabledCheckBox->setChecked(
        CoreSettingsGetBoolValue(SettingsID::GameStats_ReplayEnabled));
#else
    this->replayEnabledCheckBox->setChecked(false);
    this->replayEnabledCheckBox->setEnabled(false);
    this->replayEnabledCheckBox->setToolTip(
        "This build was compiled without GAME_STATS support (-DGAME_STATS=ON), "
        "so replay recording is unavailable.");
#endif
    this->kailleraPortSpinBox->setValue(kailleraPort);
```

- [ ] **Step 3: Reset-to-default handling**

Currently:
```cpp
    this->kailleraRecordByDefaultCheckBox->setChecked(
        CoreSettingsGetDefaultBoolValue(SettingsID::Kaillera_RecordingEnabled));
    this->kailleraPortSpinBox->setValue(kailleraPort);
```
becomes:
```cpp
    this->kailleraRecordByDefaultCheckBox->setChecked(
        CoreSettingsGetDefaultBoolValue(SettingsID::Kaillera_RecordingEnabled));
#ifdef RMGK_GAME_STATS
    this->replayEnabledCheckBox->setChecked(
        CoreSettingsGetDefaultBoolValue(SettingsID::GameStats_ReplayEnabled));
#endif
    this->kailleraPortSpinBox->setValue(kailleraPort);
```

- [ ] **Step 4: Save on dialog accept**

Currently:
```cpp
    CoreSettingsSetValue(SettingsID::Kaillera_RecordingEnabled, this->kailleraRecordByDefaultCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Kaillera_Port, this->kailleraPortSpinBox->value());
```
becomes:
```cpp
    CoreSettingsSetValue(SettingsID::Kaillera_RecordingEnabled, this->kailleraRecordByDefaultCheckBox->isChecked());
#ifdef RMGK_GAME_STATS
    CoreSettingsSetValue(SettingsID::GameStats_ReplayEnabled, this->replayEnabledCheckBox->isChecked());
#endif
    CoreSettingsSetValue(SettingsID::Kaillera_Port, this->kailleraPortSpinBox->value());
```

- [ ] **Step 5: Verify — `.ui` XML is well-formed and `uic` accepts it**

```bash
cd /Users/ness/workspaces/RMG-K
python3 -c "import xml.dom.minidom; xml.dom.minidom.parse('Source/RMG/UserInterface/Dialog/SettingsDialog.ui')" && echo "XML OK"
/opt/homebrew/opt/qt/share/qt/libexec/uic Source/RMG/UserInterface/Dialog/SettingsDialog.ui -o /tmp/ui_SettingsDialog_check.h
echo "uic exit code: $?"
grep -c "replayEnabledCheckBox" /tmp/ui_SettingsDialog_check.h
rm -f /tmp/ui_SettingsDialog_check.h
```
Expected: `XML OK`, `uic exit code: 0`, and at least one match for `replayEnabledCheckBox` in the generated header (confirms `uic` recognized the new widget and will generate `this->replayEnabledCheckBox` as a real member). If the `uic` binary isn't at that exact path on your machine, find it with `find /opt/homebrew -name uic 2>/dev/null` first.

- [ ] **Step 6: Commit**

```bash
cd /Users/ness/workspaces/RMG-K
git add Source/RMG/UserInterface/Dialog/SettingsDialog.ui Source/RMG/UserInterface/Dialog/SettingsDialog.cpp
git commit -m "$(cat <<'EOF'
Add independent "Record replay file" checkbox to Settings > Core

A single persistent setting rather than a per-lobby-dialog checkbox
like krec's - this feature also covers offline play, which has no
lobby/room UI to attach a per-session checkbox to. Disabled with an
explanatory tooltip in builds compiled without GAME_STATS=ON.
EOF
)"
```

---

### Task 5: Wire `Replay::` into the emulation lifecycle

**Files:**
- Modify: `Source/RMG-Core/Emulation.cpp:26-28` (includes), `:255-263` (`FrameCallback`), `:1021` (`CoreStartEmulation`), `:1138-1167` (`CoreStopEmulation`)

**Interfaces:**
- Consumes: `Replay::OnEmulationStart()`, `Replay::OnFrame()`, `Replay::OnEmulationStop()` (Task 3).

- [ ] **Step 1: Include `Replay.hpp`**

In `Source/RMG-Core/Emulation.cpp`, currently:
```cpp
#include "rmgk_gekko.hpp"
#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "n02_client.h"
#endif

#include "m64p/Api.hpp"
```
becomes:
```cpp
#include "rmgk_gekko.hpp"
#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "n02_client.h"
#endif
#ifdef RMGK_GAME_STATS
#include "Replay.hpp"
#endif

#include "m64p/Api.hpp"
```

- [ ] **Step 2: Call `Replay::OnFrame()` from `FrameCallback`**

Currently (lines 255-263):
```cpp
static void FrameCallback(unsigned int frameIndex)
{
    s_CurrentFrame = frameIndex;
#ifdef NETPLAY
    // Reset sync flag at the start of each new frame
    // This ensures we sync exactly once per frame regardless of PIF polling timing
    s_SyncedThisFrame = false;
#endif
}
```
becomes:
```cpp
static void FrameCallback(unsigned int frameIndex)
{
    s_CurrentFrame = frameIndex;
#ifdef NETPLAY
    // Reset sync flag at the start of each new frame
    // This ensures we sync exactly once per frame regardless of PIF polling timing
    s_SyncedThisFrame = false;
#endif
#ifdef RMGK_GAME_STATS
    // This callback is already skipped during GekkoNet rollback resimulation
    // (see l_RollbackHiddenStepActive in mupen64plus-core's main.c), so this
    // is exactly one call per real emulated frame - never inflated by resim.
    Replay::OnFrame();
#endif
}
```

- [ ] **Step 3: Call `Replay::OnEmulationStart()` after the frame callback is registered**

Currently (around line 1021):
```cpp
        // Register frame callback for frame counter (used by Kaillera)
        s_CurrentFrame = 0;
        m64p::Core.DoCommand(M64CMD_SET_FRAME_CALLBACK, 0, (void*)FrameCallback);

#ifdef NETPLAY
```
becomes:
```cpp
        // Register frame callback for frame counter (used by Kaillera)
        s_CurrentFrame = 0;
        m64p::Core.DoCommand(M64CMD_SET_FRAME_CALLBACK, 0, (void*)FrameCallback);

#ifdef RMGK_GAME_STATS
        Replay::OnEmulationStart();
#endif

#ifdef NETPLAY
```

- [ ] **Step 4: Call `Replay::OnEmulationStop()` from `CoreStopEmulation`**

Currently (lines 1138-1167):
```cpp
CORE_EXPORT bool CoreStopEmulation(void)
{
    std::string error;
    m64p_error ret;

#ifdef NETPLAY
    rmgk_gekko::request_stop();
#endif

    if (!m64p::Core.IsHooked())
    {
        return false;
    }
```
becomes:
```cpp
CORE_EXPORT bool CoreStopEmulation(void)
{
    std::string error;
    m64p_error ret;

#ifdef NETPLAY
    rmgk_gekko::request_stop();
#endif

#ifdef RMGK_GAME_STATS
    // Finalizes (patches the length field, closes) any .rmgr file still
    // open - covers the "user quit mid-match" case.
    Replay::OnEmulationStop();
#endif

    if (!m64p::Core.IsHooked())
    {
        return false;
    }
```

- [ ] **Step 5: Verify — syntax-only compile of `Emulation.cpp` with `RMGK_GAME_STATS`**

`Emulation.cpp` has many more includes than `Replay.cpp`/`ReplayMemory.cpp` (`Netplay.hpp`, `Kaillera.hpp`, `rmgk_gekko.hpp`, etc.), some of which may reach for SDL3 or other third-party headers not installed locally. Try the direct compile first; if it fails on a missing third-party header unrelated to this change, that's a pre-existing local-environment gap, not a bug in this task — note it and defer to CI instead of chasing package installs:

```bash
cd /Users/ness/workspaces/RMG-K
clang++ -std=c++20 -fsyntax-only -I Source/RMG-Core -I Source/RMG-Core/m64p \
    -DRMGK_GAME_STATS -DCORE_INTERNAL \
    Source/RMG-Core/Emulation.cpp 2>&1 | head -60
```
Expected: either `exit code 0`, or errors *only* about headers this task didn't touch (e.g. missing `SDL3/SDL.h`, missing a generated `Config.hpp`). If every error is about lines/files this task changed (the new `#ifdef RMGK_GAME_STATS` blocks specifically), fix those before moving on.

- [ ] **Step 6: Commit**

```bash
cd /Users/ness/workspaces/RMG-K
git add Source/RMG-Core/Emulation.cpp
git commit -m "$(cat <<'EOF'
Hook Replay:: into the emulation lifecycle

Replay::OnEmulationStart() arms the per-match state machine right
after the frame callback is registered (same success-path branch krec
piggybacks its own frame-counter reset on). Replay::OnFrame() runs
from FrameCallback(), which mupen64plus-core already skips during
GekkoNet rollback resimulation - so this is exactly once per real
frame with no extra filtering needed on this side.
Replay::OnEmulationStop() finalizes any still-open file when the user
quits mid-match.
EOF
)"
```

---

### Task 6: Integration verification and §2 scope check

**Files:** none (verification only)

- [ ] **Step 1: Attempt a full local build (best-effort)**

This machine is macOS, and the project's documented build paths are Linux/Windows-only, so this step may simply fail on platform-specific pieces (X11, VidExt, etc.) unrelated to this feature — that's expected, not a sign this task's changes are wrong. Still worth trying, since Qt6/SDL3 are present via Homebrew:

```bash
cd /Users/ness/workspaces/RMG-K
cmake -S . -B /tmp/rmgk-build-attempt -DGAME_STATS=ON -DUPDATER=OFF -DNETPLAY=ON 2>&1 | tail -40
cmake --build /tmp/rmgk-build-attempt --target RMG-Core -j4 2>&1 | tail -80
```
Record whatever happens (clean build, or the specific error) in the task summary. If it fails on something unrelated to `Replay.cpp`/`ReplayMemory.cpp`/`Emulation.cpp`/the CMake changes from this plan, note that explicitly and move on — do not try to fix unrelated platform issues as part of this feature.

```bash
rm -rf /tmp/rmgk-build-attempt
```

- [ ] **Step 2: Push the branch and let CI do the authoritative full build**

CI is the actual source of truth for whether this links and runs on real Linux/Windows targets (per the doc's own established norm, section 7). This is a **push** — confirm with the user before running it:

```bash
git push origin feature/replay-file-format
```

- [ ] **Step 3: Scope check against the handoff doc's §2 hard requirements**

Go through `/Users/ness/workspaces/rmgk-replay-file-agent-prompt.md` section 2 point by point and confirm each is actually true of what got built:

1. **krec untouched, own independent checkbox** — run `git diff master..feature/replay-file-format -- Source/n02/` and confirm it's empty. Confirm `replayEnabledCheckBox` (Task 4) is a distinct `SettingsID::GameStats_ReplayEnabled` value, never read or written anywhere `Kaillera_RecordingEnabled` is.
2. **No FlatBuffers, event-stream design** — confirm no new dependency was added anywhere (`git diff master..feature/replay-file-format -- '*.txt' '*.cmake'` for any `find_package`/`pkg_check_modules` addition beyond what Task 1 added), and that `Replay.cpp`'s event stream matches the command-byte + declared-payload-size shape from doc §4.2.
3. **Written incrementally, length patched at end** — confirm `OpenNewFile` writes `streamLength = 0` and only `FinalizeFile` patches it, matching doc §4.6.
4. **Player tags from room metadata, not game memory** — confirm `GameStartEvent::playerNames` is populated from `recording_player_names` (guarded by `RMGK_HAVE_P2P_TRANSPORT`), never from any Smash-Remix in-memory name-tag address.
5. **Little-endian, native struct layout** — confirm every multi-byte field in `Replay.cpp`'s event structs is read via `m64p::Core.DebugMemRead8/16/32` (already host-byte-order per the doc) with no manual byte-swap code anywhere in `ReplayMemory.cpp`.
6. **`DEBUGGER=1` gating was a real, explicit decision** — already resolved via this session's confirmation (gated behind `GAME_STATS`, default `OFF`); no further action needed, just confirm `Source/3rdParty/CMakeLists.txt:135` still reads `DEBUGGER=$<BOOL:${GAME_STATS}>`.

- [ ] **Step 4: Report results to the user**

Summarize: what was built, what got verified locally vs. deferred to CI (per the Global Constraints section above), the outcome of the §2 scope check, and — since a real emulator run couldn't happen on this machine — that an actual in-game manual test (enable the checkbox, play a match, inspect the resulting `.rmgr` file's bytes) still needs to happen on a Linux or Windows build before considering this feature done, not just "code complete."
