# RMG-K Practice Protocol — Specification

**Status:** protocol version `1`. This is the contract between the emulator
(client, `Source/RMG-Core/Practice*.cpp`) and any practice server. A server
conforms by implementing §2–§6 exactly. Nothing else about the server's
design is specified here.

The emulator streams the live state of a Smash Remix 2.0.1 match to the
server once per emulated frame and applies the commands the server replies
with. All drill logic (when to reset, what to randomize, what counts as
success) lives in the server; the emulator is a sensor and actuator only.

## 1. Transport and conventions

- **TCP**, `127.0.0.1`, default port `46464` (emulator setting
  `Practice.Port`). The **server listens; the emulator connects** and retries
  once per second while disconnected, so the server may start before or after
  the emulator. One connection at a time; a second concurrent connection is
  refused.
- `TCP_NODELAY` must be set on both ends.
- **Little-endian** everywhere. Floats are IEEE-754 single precision.
- **Framing:** every message is
  `u32 length | u8 type | payload[length-1]` — `length` counts the type byte
  plus the payload, not itself. Maximum `length` is `1_048_576`; a larger
  value is a protocol error (the receiver closes the connection).
- Strings are fixed-width, NUL-padded, not necessarily NUL-terminated.
- Reserved bits/bytes are written as `0` and ignored on read.

## 2. Event streams (reused from `.rmgr`)

Game state is carried as `.rmgr` events — byte-identical to the structs in
[RMGR_SPEC.md](RMGR_SPEC.md) §4–§5, each encoded as `u8 code | payload`.
Codes used here: `0x02` MatchStart, `0x03` InputFrame, `0x04` StateFrame,
`0x05` MatchEnd, `0x06` ItemUpdate, `0x07` StageHazardUpdate,
`0x08` MatchSettings, `0x09` MatchResult. (`0x01` EventPayloads is not sent as
an event; its content is carried in `Hello`, §3.1.)

Payload **sizes** for each code are declared once, in `Hello.eventPayloads`.
A server must read sizes from there, never hardcode them, and must skip
unknown codes by their declared size (RMGR_SPEC.md §5.0, §6).

An "event stream" below means zero or more such `code|payload` records
concatenated with no framing between them, running to the end of the
message.

## 3. Emulator → server messages

### 3.1 `Hello` — type `0x01`

First message on every new connection. Layout:

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 2 | `u16` | `protocolVersion` (`1`) |
| 2 | 64 | `char[64]` | `goodName` (e.g. `SmashRemix2.0.1`) |
| 66 | 4 | `u32` | `recorderSchemaVersion` (RMGR_SPEC.md §3.2) |
| 70 | 1 | `u8` | `count` |
| 71 | 3×count | `(u8 code, u16 size)[count]` | `eventPayloads` — declared payload size of every event code used in this protocol |

### 3.2 `MatchBegin` — type `0x02`

Sent when a VS match goes live (the same moment the replay recorder opens
a file), and also immediately after connecting if a match is already live.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | `u32` | `matchSerial` — increments per `MatchBegin` for the life of the emulator process |
| 4 | … | event stream | `MatchStart` (`0x02`) then `MatchSettings` (`0x08`) |

### 3.3 `Frame` — type `0x03`

One per emulated frame while a match is live and `gameStatus == ongoing`.
**The emulator blocks after sending this until it receives the matching
`Commands` reply or the timeout expires (§5).**

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | `i32` | `frame` — emulator-side counter, `0` at the first `Frame` after `MatchBegin`, **+1 per `Frame` sent, never rewinds** (not even after `LoadState`). Distinct from the `frame` fields inside events, which carry the same value. |
| 4 | 1 | `u8` | `flags` — bit 0 `stateLoaded` (see §6.5), bit 1 `replyTimedOutPreviously` (the previous `Frame`'s reply was not applied because it arrived late or never) |
| 5 | 1 | `u8` | `resultCount` |
| 6 | `resultCount` | `u8[]` | `results` — one result code (§4.3) for each command of the **most recent applied** `Commands` message, in order |
| 6+resultCount | … | event stream | Per seated port, in port order: `InputFrame` (`0x03`) then `StateFrame` (`0x04`); then zero or more `ItemUpdate` (`0x06`); then zero or one `StageHazardUpdate` (`0x07`) — exactly the per-frame content of a `.rmgr` file |

### 3.4 `MatchEnd` — type `0x04`

Sent once when the match ends or is abandoned (including Salty Runback and
leaving the VS screen). No reply.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | `u32` | `matchSerial` |
| 4 | … | event stream | `MatchEnd` (`0x05`) then `MatchResult` (`0x09`) |

## 4. Server → emulator messages

### 4.1 `HelloAck` — type `0x81`

Must be the server's first message after receiving `Hello`.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 2 | `u16` | `protocolVersion` the server speaks |
| 2 | 1 | `u8` | `accepted` — `1` to proceed, `0` to refuse |

If `accepted == 0` or the version differs from the emulator's, the emulator
closes the connection and **does not retry until the emulator restarts**.
The emulator sends no `MatchBegin`/`Frame` before it has received an
accepting `HelloAck`.

### 4.2 `Commands` — type `0x82`

The reply to exactly one `Frame`.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | `i32` | `frame` — the `Frame.frame` being answered |
| 4 | 1 | `u8` | `count` |
| 5 | … | `Command[count]` | commands, in order |

Each `Command` is `u8 code | u16 size | payload[size]`. The emulator skips
a command with an unknown code (result `Unknown`) by its declared `size`.
`count == 0` is a valid no-op reply, and **the server must reply to every
`Frame`**, even with an empty list.

A `Commands` whose `frame` is not the most recent `Frame`'s is discarded
without effect (stale).

### 4.3 Result codes

Reported back in the next `Frame.results`.

| Code | Name | Meaning |
|---:|---|---|
| 0 | `Ok` | Applied. |
| 1 | `Unknown` | Unknown command code; skipped. |
| 2 | `Malformed` | Declared `size` does not match the command, or a field is out of range. |
| 3 | `Rejected` | Not valid in the current state (e.g. port not seated, or a state-write sent in the same reply as `LoadState`, §6.2). |
| 4 | `SlotEmpty` | `LoadState` of a slot never saved. |
| 5 | `Failed` | The core refused the operation. |
| 6 | `Pending` | `LoadState` accepted; it lands before the next frame — confirmation is the `stateLoaded` flag on a later `Frame` (§6.5). |

### 4.4 Commands

#### `SetPlayerState` — code `0x01`, size `41`

Writes the masked fields of one fighter. Unmasked fields are ignored (send
zeros).

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 1 | `u8` | `port` `0`–`3` (must be a seated port) |
| 1 | 4 | `u32` | `mask` — bit *n* enables field *n* below |
| 5 | 4 | `f32` | bit 0 `positionX` |
| 9 | 4 | `f32` | bit 1 `positionY` |
| 13 | 4 | `f32` | bit 2 `velocityX` |
| 17 | 4 | `f32` | bit 3 `velocityY` |
| 21 | 4 | `i32` | bit 4 `facingDirection` (`1` right, `-1` left) |
| 25 | 4 | `u32` | bit 5 `damagePercent` |
| 29 | 4 | `i32` | bit 6 `shieldHealth` |
| 33 | 1 | `i8` | bit 7 `stocksRemaining` |
| 34 | 4 | `i32` | bit 8 `characterSpecific` |
| 38 | 1 | `u8` | bit 9 `jumpsUsed` |
| 39 | 1 | `u8` | bit 10 `hurtboxState` |
| 40 | 1 | `u8` | bit 11 `specialHitStatus` |

Field meanings are those of `StateFrame` (RMGR_SPEC.md §5.2). **Position and
velocity writes are only safe while the fighter is airborne**: teleporting a
grounded or ledge-hanging fighter leaves an inconsistent action state. Use
`LoadState` to reset into a known-good situation, then tweak.
`actionStateId` is deliberately not writable (RMGR_SPEC.md-adjacent: a real
action change requires running the game's `ftMainSetStatus`). The command id
`0x08` is reserved for a future `SetAction`.

#### `SetPuppetInput` — code `0x02`, size `7`

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 1 | `u8` | `port` `0`–`3` |
| 1 | 2 | `u16` | `buttons` — same bit layout as `InputFrame.buttons` |
| 3 | 1 | `i8` | `stickX` |
| 4 | 1 | `i8` | `stickY` |
| 5 | 2 | `u16` | `durationFrames` — `0` releases control of the port immediately |

For `durationFrames` = N > 0, the port's controller is overridden for the
next N controller polls (one per frame), beginning with the frame **after**
the one this reply answers. A new `SetPuppetInput` for the same port
replaces the old one. Overrides are cleared by `MatchEnd` and on disconnect.

#### `SetSetting` — code `0x03`, size `3`

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 2 | `u16` | `key` — index into the `MatchSettings` Remix fields: `0` = `hitstun` … `38` = `yoshiIslandCloudAnims`, i.e. `key = (field offset in MatchSettings) - 0x24` (RMGR_SPEC.md §5.1) |
| 2 | 1 | `u8` | `value` — meanings per RMGR_SPEC.md §5.1.1/§5.1.2 |

Best effort: a setting the game only reads at match start has no effect
mid-match.

#### `SetRng` — code `0x04`, size `4`

`i32 seed` — written to `sSYUtilsRandomSeed` (RMGR_SPEC.md §5.1).

#### `SaveState` — code `0x05`, size `1`

`u8 slot` (`0`–`15`). Captures the full emulator state into an in-memory
slot owned by the emulator process (lost on emulator exit). Overwrites the
slot. The capture happens **after** all `SetPlayerState`/`SetSetting`/`SetRng`
commands in the same reply have applied.

#### `LoadState` — code `0x06`, size `1`

`u8 slot` (`0`–`15`). Restores a slot saved earlier in this emulator
process, within the same ROM. Returns `Pending` (§4.3).

## 5. Timing

- The emulator sends `Frame` N and waits up to `Practice.TimeoutMs`
  (default **8 ms**) for `Commands{frame: N}`. On timeout it applies nothing
  and proceeds; the late reply is discarded when it eventually arrives, and
  the next `Frame` carries `replyTimedOutPreviously`.
- Commands answering `Frame` N take effect **before the game simulates
  frame N+1**, except `SetPuppetInput` (applied at frame N+1's controller
  poll — the same instant) and `LoadState` (§6.5).
- Eight consecutive timeouts disconnect and trigger reconnect.

## 6. Semantics a server must account for

1. **Application order within one `Commands`** is fixed regardless of the
   order sent: `LoadState`; then `SetPlayerState`, `SetSetting`, `SetRng`
   in sent order; then `SaveState`; then `SetPuppetInput`.
2. **`LoadState` is deferred.** The core performs the load at its next safe
   interrupt boundary, which would overwrite any same-reply memory writes.
   So if a `Commands` contains `LoadState`, every `SetPlayerState`,
   `SetSetting`, `SetRng` and `SaveState` in it is answered `Rejected` and
   not applied. `SetPuppetInput` is still honored.
3. **Reset-and-tweak pattern.** To reset to a saved scenario and vary it:
   reply to frame N with `[LoadState{slot}]`; wait for the `Frame` with
   `stateLoaded`; reply to that frame with the `SetPlayerState`/`SetRng`
   tweaks.
4. **`Frame.frame` never rewinds**, even across `LoadState`; the server must
   not assume game state is continuous across a load.
5. **`stateLoaded`** is set on the first `Frame` whose state was captured
   after a requested `LoadState` landed. Its `StateFrame`s describe the
   restored state.
6. A `LoadState` that never lands (core failure) is reported as `Failed` in
   the next `Frame.results`, not silently dropped.

## 7. Example bytes

`HelloAck` accepting version 1:

```
04 00 00 00   length = 4
81            type HelloAck
01 00         protocolVersion = 1
01            accepted
```

`Commands` answering frame 5 with one `SaveState{slot 2}`:

```
0A 00 00 00   length = 10
82            type Commands
05 00 00 00   frame = 5
01            count = 1
05 01 00 02   code 0x05, size 1, slot 2
```

## 8. Versioning

`protocolVersion` changes only for breaking changes to framing or to an
existing message layout. New commands use new codes (skippable by size); new
trailing fields on events follow RMGR_SPEC.md §6; new trailing fields on
messages are not permitted without a version bump.
