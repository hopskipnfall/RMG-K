# Practice Protocol — Design

**Date:** 2026-10-01
**Status:** Draft for review. The authoritative wire format is [PRACTICE_PROTOCOL.md](../../PRACTICE_PROTOCOL.md); where this document and that one disagree, that one wins.

## 1. Goal

A personal fork of RMG-K for drilling specific skills in Smash Remix 2.0.1.
While a Remix ROM is loaded, the emulator streams per-frame game state to an
external **practice server** over localhost and applies the commands the
server sends back. All drill logic lives in the server; the emulator is a
dumb sensor/actuator.

First drill: **edgeguarding** — the opponent is offstage trying to recover,
the player tries to stop them. On success or failure the server resets the
situation instantly, with no menu/stop/restart.

### Non-goals (v1)

- Changing a fighter's character mid-match. No known safe path exists
  (confirmed against the decomp: Kirby's copy ability never changes `fkind`;
  Metal/Giant forms are separate spawn-time characters). The user sets up
  characters and stage in the Remix menus manually.
- Starting a match from the server (menu automation).
- Writing `actionStateId` directly (see §6.1) or calling guest game code
  (`SetAction`, deferred).
- Netplay/Kaillera sessions: the practice client never activates in them.

## 2. Architecture

New module `Source/RMG-Core/Practice.{cpp,hpp}`, a sibling of `Replay`.
Active only when all hold: `goodName == "SmashRemix2.0.1"`, no netplay
session, and the new Practice setting is enabled (off by default, separate
from replay recording).

Hook: `FrameCallback` in `Emulation.cpp` already calls `Replay::OnFrame()`
exactly once per real emulated frame on the emulation thread. `Practice::OnFrame()`
is called next to it.

```
FrameCallback(frame N) → Practice::OnFrame()
  1. snapshot state (shared ReplayMemory readers)
  2. send Frame{N, ...}                  ─┐ TCP, 127.0.0.1
  3. block ≤ ~4 ms for Commands{N}        ◄┘
  4. apply state writes / load / save now, before frame N+1 logic runs
  5. stash puppet inputs; the PIF controller-read hook substitutes them
     for frame N+1
```

- **Lockstep, blocking, short timeout.** Round trip on localhost is tens of
  µs against a 16 ms frame. A missing/late reply means "no commands" for that
  frame (counter incremented), never a stall.
- **Transport:** TCP on `127.0.0.1` with `TCP_NODELAY`. Chosen over Unix
  sockets because Windows is the primary platform. Default port is a
  setting. The emulator connects out and retries in a background thread, so
  the server may start before or after the emulator.
- **Puppet inputs** override the raw controller bytes in the PIF read path
  (same interception point as `KailleraPifSyncCallback`), not
  `PS_PROCESSED_BUTTONS`, so the game applies deadzones/buffering normally.
- **Shared event builders.** The `.rmgr` event structs and the code that
  fills them from game memory move out of `Replay.cpp` into
  `ReplayEvents.hpp` / `ReplayEventBuilder.cpp`, so Replay and Practice emit
  byte-identical events from one implementation.

## 3. Wire protocol

Framing: `u32 length | u8 type | payload`. Little-endian, `#pragma pack(1)`
structs, the same conventions as `.rmgr` (docs/RMGR_SPEC.md): append-only
fields, declared payload sizes so unknown types/fields can be skipped.

### 3.1 Emulator → server

| Type | Payload |
|---|---|
| `Hello` | protocol version, `goodName`, `recorderSchemaVersion`, per-type payload-size table (like `EventPayloads`) |
| `MatchBegin` | `MatchStart` + `MatchSettings` payloads, sent when a match goes live |
| `Frame` | `frame`, `flags` (`stateLoaded`), `lastCommandResult`, then per-port `InputFrame` + `StateFrame`, plus `ItemUpdate`/`HazardUpdate` events — byte-identical to the `.rmgr` structs |
| `MatchEnd` | `MatchEnd` + `MatchResult` payloads |

### 3.2 Server → emulator

`Commands{frame, count, commands[]}` — one reply per `Frame`, always sent
(empty list = no-op). The echoed `frame` lets the emulator drop a stale
reply instead of applying it to the wrong frame.

| Command | Meaning |
|---|---|
| `SetPlayerState{port, mask, ...}` | Masked writes: `positionX/Y`, `velocityX/Y`, `facingDirection`, `damagePercent`, `shieldHealth`, `stocksRemaining`, `characterSpecific`, `jumpsUsed`, `hurtboxState`/`specialHitStatus`, `scale`. Position/velocity writes are only safe while the fighter is airborne (§6.1). |
| `SetPuppetInput{port, buttons, stickX, stickY, durationFrames}` | Override a port's controller; duration 0 releases control. |
| `SetSetting{key, value}` | Write a `Toggles.asm` setting word (hitlag, DI, game speed, …). |
| `SetRng{seed}` | Write `sSYUtilsRandomSeed`. |
| `SaveState{slot}` | Capture the full emulator state into in-memory slot `slot`. |
| `LoadState{slot}` | Restore slot `slot`. |

Application order within one reply is fixed: `LoadState` → `SetPlayerState` /
`SetSetting` / `SetRng` → `SaveState` → `SetPuppetInput`. Loading first means
tweaks apply on top of the restored state.

## 4. Savestates

The server must be able to both **create and load** savestates (hard
requirement).

- `LoadState` is **deferred**: the core only supports performing the load at
  its next safe interrupt boundary (a synchronous load mid-frame is known to
  desync; see the spectate-keyframe code in `Emulation.cpp`). A same-reply
  memory write would be clobbered by the landing load, so the protocol makes
  state-writes in a `LoadState` reply `Rejected`; the server resets with
  `LoadState`, waits for the `stateLoaded` frame, then sends its tweaks.
- Slots live in memory, keyed by id; the server owns their lifecycle. An
  optional file-backed variant (`SaveStateToFile`/`LoadStateFromFile`) for
  persistent scenario libraries is a later addition.
- Save runs synchronously inside `OnFrame` via `CoreRollbackSaveGameState`;
  load uses `CoreRollbackLoadGameStateDeferred` (the pointer must stay valid
  until the core drains it, so slot buffers live for the process).
  Assumption to verify on real hardware: the load lands before the next VI
  callback.
- Result reporting: the next `Frame` carries `lastCommandResult` (ok, or a
  failure code such as "slot empty"). After a successful load it also sets
  `stateLoaded` and `frame` is rewound, so the server doesn't mistake the
  jump for a glitch.

## 5. Practice server (separate repo)

Written by the user (Rust, chosen for latency). **This repo does not design
or implement it** — it only defines the protocol the server must conform to
(PRACTICE_PROTOCOL.md). The emulator never learns drill logic.

**Edgeguard drill flow** (illustrative, to motivate the protocol — the
server's implementation is out of scope here):

1. User reaches a scenario once (opponent offstage, airborne, at some %) and
   the server sends `SaveState{slot}`.
2. Server watches `Frame`s. Success: opponent loses a stock. Failure:
   opponent grounded on stage or in a ledge state.
3. On either outcome: `LoadState{slot}`, then `SetPlayerState` to randomize
   damage, position offset and velocity so each rep varies.

## 6. Constraints from the game

### 6.1 State writes

- `damagePercent` is a plain `u32` (`player_struct+0x2C`). Assumed the HUD
  reads it live (to verify).
- Position is behind `PS_POSITION_PTR` (+0x78); velocity are plain fields.
  Teleporting an *airborne* fighter is fine (also write the collision
  data's previous position to avoid tunnelling misreads — to verify with
  Game Expert).
- Teleporting a grounded or ledge-hanging fighter into the air leaves a
  wrong action state (e.g. Wait while falling) and glitches. Savestates
  avoid this entirely, which is why they're in v1.
- `actionStateId` is **not writable**: setting an action requires
  `ftMainSetStatus` (`0x800E6F24`; confirmed from the decomp and from
  Remix's own ASM) to run for real — it reinitializes hitboxes, hitstatus,
  model/texture overrides, effects, and animation. There is no
  pending-status flag; transitions are direct calls.

### 6.2 Deferred: `SetAction`

If later needed: an RDRAM mailbox (pending flag, fighter gobj, status id,
frame begin, anim speed, flags) plus a small MIPS stub patched into a
per-frame game point that does `jal 0x800E6F24`. Remix's `on_action_changed`
hook (`0x800E7AA4`, ftMainSetStatus+0xB80) is a safe post-change injection
point. Alternative: interpreter re-entry (`DEBUGGER=1` build forces the
interpreter). Reserve the command id in the schema now.

## 7. Error handling

- **No server:** silent retry every second; game plays normally.
- **`Hello` mismatch** (protocol version / `goodName`): log, disconnect, stop
  retrying until restart.
- **Reply timeout:** apply nothing, count it, show the count in the OSD; a
  sustained streak disconnects.
- **Malformed/unknown command:** skip by declared size, keep the rest.
- **Netplay:** never activates.

## 8. Testing

- Host-side C++ tests (no emulator needed): wire codec with golden bytes,
  event builders and memory writes against a fake RDRAM, command applier,
  and the transport against a loopback fake server.
- Fake-server test for the emulator side: timeout, reconnect, stale-frame
  reply, `Hello` mismatch.
- Manual end-to-end on Windows with the edgeguard drill (acceptance; Remix
  can't be run in the dev environment).

## 9. Plan-time verifications

1. Collision-data fields to write alongside position (Game Expert).
2. HUD damage source — live read vs cached copy (Game Expert).
3. Synchronous save/load callable from `OnFrame` (core inspection).
4. `DebugMemWrite8/16/32` need hooking in `CoreApi` (exported by core, not
   yet wired).
5. PIF read-path override point for puppet input.

Status (2026-10-03): item 3 resolved by using the core's deferred rollback
load and a synchronous rollback save; item 4 done (`CoreApi` hooks); item 5
done (`PracticePifSyncCallback` in `Emulation.cpp`). Items 1 and 2 remain open
and are covered by the on-hardware checklist in the implementation plan.
