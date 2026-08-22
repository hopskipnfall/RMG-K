# Per-Frame Game Stats via Shared Memory

Date: 2026-08-22

## Problem

The user wants to track per-player game stats (position, facing direction,
character, state) derived from N64 emulated memory, sampled once per frame,
for consumption by a separate stats/overlay application. The target game is
Super Smash Bros. (N64) — Smash Remix 2.0.1; the addressing scheme (a
pointer chase, documented below) came from a hand-written RAM map
(`/Users/ness/workspaces/smashremix/docs/ram-map.md`) derived from the mod's
assembly source, supplied partway through this design.

## Constraints from investigation

- RMG-K's mupen64plus-core fork has already been patched for rollback
  netcode (GekkoNet). The registered frame callback
  (`M64CMD_SET_FRAME_CALLBACK` → `FrameCallback()` in
  `Source/RMG-Core/Emulation.cpp:255`) already fires exactly once per
  *visible* frame and is skipped for hidden rollback resimulation/runahead
  steps (`new_frame()` in
  `Source/3rdParty/mupen64plus-core/src/main/main.c:1614-1615`, gated by
  `l_RollbackHiddenStepActive`, itself driven by `rollback_execute_begin_frame()`
  in `Source/RMG-Core/rmgk_gekko.cpp`). This means hooking into
  `FrameCallback()` gives exactly one call per game frame (60fps NTSC /
  50fps PAL), never inflated by resimulation — the user confirmed this rate
  guarantee is what matters, not excluding speculative-but-later-corrected
  frames.
- mupen64plus-core's debugger memory API (`DebugMemRead8/16/32`,
  `DebugMemGetPointer`) is always exported, but is a no-op (returns 0/NULL
  and logs an error) unless the core was built with `DEBUGGER=1`. RMG-K's
  build (`Source/3rdParty/CMakeLists.txt`) does not currently pass that
  flag.
- `Source/RMG-Core` has no Qt dependency; the app's existing socket code
  (`QWebSocket` in RMG's `LobbyClient`) is unrelated and lives in a
  different, Qt-based target. The stats producer must not introduce a Qt
  dependency into RMG-Core.
- RMG-Core follows a consistent internal convention: every exported function
  is declared plainly in a `.hpp` (no export macro in the header) and
  defined with `CORE_EXPORT` in the matching `.cpp`; diagnostics go through
  `CoreAddCallbackMessage(CoreDebugMessageType, std::string)`
  (`Source/RMG-Core/Callback.hpp`); cross-DLL calls go through the
  `m64p::Core` global (`Source/RMG-Core/m64p/Api.hpp`).

## Decisions made with the user

1. **Sink:** shared memory (not a log file, not a socket). Same-machine only
   is acceptable. The reader is a **separate application**, out of scope for
   this repo — this change only needs to *publish* a stable, documented
   layout.
2. **Game scope:** memory knowledge lives in one file, scoped to the game
   currently being targeted (not keyed by ROM/CRC). Multi-game support can
   be added later.
3. **Approach:** enable `DEBUGGER=1` unconditionally in the core build (no
   new CMake option) and read memory via the existing debugger API, called
   from inside the already-filtered `FrameCallback()`.

## Design

### 1. Build change

`Source/3rdParty/CMakeLists.txt`: add `DEBUGGER=1` to the mupen64plus-core
`ExternalProject_Add` `BUILD_COMMAND` make invocation, alongside the
existing `NETPLAY=$<BOOL:${NETPLAY}>` etc. flags.

### 2. Core API wrapper

Extend `Source/RMG-Core/m64p/CoreApi.{hpp,cpp}` with hooks for
`DebugMemGetPointer`, `DebugMemRead8`, `DebugMemRead16`, `DebugMemRead32`,
using the existing `HOOK_FUNC` macro (same pattern as every other wrapped
core function). Include `api/m64p_debugger.h` for the typedefs.

### 3. Memory map and resolution chain (the structured, editable part)

New header `Source/RMG-Core/GameStatsProbes.hpp`, kept separate from the
reading logic so it's the one file to touch if Smash Remix updates and
these addresses shift, or to retarget a different game. It holds named
offset constants (`GameStatsAddresses::MatchInfoPtr`, `Port_Character`,
`Player_Facing`, `Player_PositionPtr`, etc.) plus the valid-pointer-range
bounds (`0x80000000`–`0x80800000`, the 8MB expansion-pak RDRAM window) used
to sanity-check every pointer hop below.

A flat `address + stride` table (the original placeholder design) can't
express this data — the game reaches a fighter's live state through a
pointer chase, not a fixed per-player offset:

```
matchInfo    = *MatchInfoPtr                                    // match info block
matchStruct  = matchInfo + PortArrayOffset + port * PortStride  // cheap: character/stocks/damage
playerObject = *(matchStruct + Port_PlayerObject)                // 0 => port not in a live match
playerStruct = *(playerObject + Object_PlayerStruct)
state        = u32  at playerStruct + Player_ActionState
facing       = i32  at playerStruct + Player_Facing  (only ever ±1)
positionVec  = *(playerStruct + Player_PositionPtr)
x, y         = f32  at positionVec + 0x00 / + 0x04
```

`GameStats.cpp`'s `read_player_frame(port, out)` implements this chain
directly (not via a generic table-driven reader), range-checking every
pointer hop and returning early — leaving that player's slot inactive —
the moment a hop is invalid. An empty port (`slotType == 2`) or one not
currently in a live match (`playerObject` out of range) both fall out of
this naturally, with no separate "is this port active" check needed.

Per the mupen64plus debugger API's own documentation, `DebugMemRead*`
already returns host-byte-order values, so — unlike a raw-RDRAM-buffer
reader — no manual endianness/word-swap (`^2`/`^3`) correction is needed on
top of it.

### 4. Shared struct layout (the public contract)

New header `Source/RMG-Core/GameStatsTypes.hpp`, self-contained (no other
RMG-Core includes) so it can be copied wholesale into a separate reader
project:

```cpp
#pragma pack(push, 1)
struct GameStatsPlayerFrame
{
    uint8_t  active;          // 1 = this port has a live fighter this frame;
                               // other fields only meaningful when active == 1
    uint8_t  character;
    int8_t   facingDirection; // 1 = right, -1 = left
    float    positionX;
    float    positionY;
    uint32_t state;           // action/state id
};

struct GameStatsSharedFrame
{
    uint32_t magic;       // 'RMGK'
    uint32_t version;     // bump on layout change (currently 2)
    uint32_t sequence;    // seqlock: odd = writer mid-update, even = stable
    uint32_t frameIndex;
    uint32_t playerCount; // always 4 (array length); use each entry's `active` flag
    GameStatsPlayerFrame players[4];
};
#pragma pack(pop)
```

Position is `float` (the game stores world-space stage coordinates as f32,
not an integer) and `state` is `uint32_t` (the action/state ID space
extends past 16 bits for character-specific moves) — both corrected from
the original placeholder guesses once the real types were known. The
`active` flag is new: with real semantics in play, a reader needs to
distinguish "this port has no fighter right now" from "all stats happen to
be zero."

Segment name: `RMGK_GameStats` (`shm_open("/RMGK_GameStats", ...)` on
Linux/macOS, `CreateFileMappingA(..., "Local\\RMGK_GameStats")` on
Windows). The writer uses `std::atomic_ref<uint32_t>` (C++20, already the
project standard) over the plain `sequence` field to get atomicity/ordering
without changing the field's on-the-wire type, so a reader in any language
can implement the same odd/even seqlock protocol without needing
`std::atomic`'s in-memory representation to match.

### 5. GameStats module

New `Source/RMG-Core/GameStats.{hpp,cpp}`:

- `CoreInitGameStats()` — creates/maps the shared memory segment, zeroes it,
  and checks `DebugMemGetPointer(M64P_DBG_PTR_RDRAM)` once, logging a
  one-time warning via `CoreAddCallbackMessage(CoreDebugMessageType::Warning, ...)`
  if it's `NULL` (DEBUGGER wasn't actually compiled in) or if the segment
  couldn't be created. On either failure, subsequent per-frame work is
  skipped (no per-frame log spam).
- `CoreStopGameStats()` — unmaps/closes and removes the segment.
- `CoreUpdateGameStats(unsigned int frameIndex)` — calls
  `read_player_frame(port, out)` for each of the 4 ports (§3's resolution
  chain), writes the results into a local `GameStatsSharedFrame`, then
  publishes it into the mapped segment using the seqlock write protocol
  (bump `sequence` to odd, write fields, bump to even).

### 6. Wiring into emulation lifecycle

`Source/RMG-Core/Emulation.cpp`:
- Call `CoreInitGameStats()` right after the existing
  `m64p::Core.DoCommand(M64CMD_SET_FRAME_CALLBACK, ...)` registration in
  `CoreStartEmulation()` (~line 1021).
- Call `CoreUpdateGameStats(frameIndex)` from inside the existing
  `FrameCallback()` (~line 255), after updating `s_CurrentFrame`.
- Call `CoreStopGameStats()` in `CoreStopEmulation()`, mirroring the
  existing Kaillera player-number cleanup right before the final `return`.

### 7. Build wiring

Add `GameStats.cpp` to `RMG_CORE_SOURCES` in
`Source/RMG-Core/CMakeLists.txt`.

## Error handling

This is a diagnostic side-channel and must never be able to affect
emulation correctness or stability:
- Missing `DEBUGGER` support or failed shared-memory creation → log once,
  skip all per-frame work (init fails soft, not fatal to
  `CoreStartEmulation`).
- Every pointer hop in the resolution chain is range-checked
  (`0x80000000`–`0x80800000`); an invalid hop leaves that player's slot
  `active = 0` rather than reading through a bad pointer.
- No locks/blocking calls in the per-frame path; the seqlock write is a few
  non-blocking memory writes.

## Testing

- Shared-memory segment is created on emulation start and destroyed on
  stop.
- `sequence`/`frameIndex` in the mapped segment advance by exactly one
  visible-frame's worth per game frame (confirmed via the existing
  `l_RollbackHiddenStepActive` filtering — not re-verified here, already
  established during design).
- All new/changed files were syntax-checked (`-fsyntax-only -Wall -Wextra`)
  against the real repo headers with both a native compiler and the
  `x86_64-w64-mingw32-g++` cross compiler (covering the Windows shared-memory
  branch); zero warnings on all touched files. No full project build was
  performed in this session (no working Linux/Windows/MSYS2 toolchain
  available) — a real build is still needed before relying on this.
- Not yet done: the RAM map's own §8 calibration procedure (screen ID
  sanity, `MatchInfoPtr` pointer sanity, value-range sanity) hasn't been run
  against a live game. This is the main open risk — the addresses are
  transcribed correctly from the guide, but unverified against actual
  RDRAM.
- **Verification plan (in progress):** `GameStats.cpp` throttled-logs a
  one-line-per-second summary (`log_frame_snapshot`, gated to every 60th
  frame) of all 4 ports' `active`/`character`/`positionX,Y`/`facing`/`state`
  via `CoreAddCallbackMessage(CoreDebugMessageType::Info, ...)` — visible in
  RMG's Log dialog, or on stdout with `--debug-messages`. This is the chosen
  first-pass way to eyeball correctness (move the character, watch position
  change; check the logged character byte against §5 of the RAM map; watch
  `state` change on jump/attack) before building any real consumer. It's
  explicitly temporary — remove once the addresses are confirmed correct.
  A standalone shared-memory reader (a first cut of the eventual separate
  Rust consumer app) is the planned follow-up verification step, once the
  logged values look right.
