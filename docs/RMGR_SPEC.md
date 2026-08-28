# RMG-K Replay File Format (`.rmgr`) — Specification

**Status:** format version `3`, implemented in `Source/RMG-Core/Replay.cpp` /
`Source/RMG-Core/ReplayMemory.cpp` on the `feature/replay-file-format` branch.

**Target game:** Super Smash Bros. (N64) — Smash Remix 2.0.1. The container
format itself (header framing, event stream mechanics) isn't game-specific,
and the header's `goodName`/`recorderSchemaVersion` (§3.3) exist precisely so
a future file for a different ROM/game is identifiable without guessing —
but the `GameStart`/`PostFrame` event field sets below are still
Smash-specific; a genuinely different game would need its own event
definitions, not just a different `goodName` value.

This document is the authoritative description of the on-disk byte layout.
If code and this document disagree, treat that as a bug — in either the code
or the document — and fix the mismatch rather than trusting one side blindly.

## 1. Overview

A `.rmgr` file is a self-contained binary recording of one N64 match: every
seated player's controller inputs, frame by frame, plus a snapshot of game
state (position, damage, stocks, action state, …) read directly from
emulated RAM. It is designed for two distinct consumers:

1. **Deterministic replay** — the recorded inputs are enough to re-simulate
   the match from scratch.
2. **Direct analysis** — the recorded game state is enough to build stats,
   visualizations, or search tooling *without* re-running the emulator at
   all.

The design is modeled on [Slippi's `.slp` format](https://github.com/project-slippi/slippi-wiki/blob/master/SPEC.md)
(Super Smash Bros. Melee / Dolphin) — a self-describing binary event stream —
but is **not** byte-compatible with it, and deliberately drops or changes
several of Slippi's choices. See §2.

## 2. Design goals and explicit non-goals

- **Streamed, not buffered.** A file is written incrementally, event by
  event, for the duration of a match — never built up in memory and flushed
  once at the end. A whole match's worth of per-frame data held in RAM is
  wasteful, and streaming means a crash mid-match still leaves a usable,
  truncated file on disk rather than nothing at all.
- **Self-describing, forward-compatible event stream, no schema compiler.**
  Every event is a 1-byte command code followed by a payload whose size was
  declared up front by the very first event in the file (`EventPayloads`,
  §4.1). A parser that doesn't recognize a command code can still skip it
  correctly and keep reading — the file can grow new event types or new
  trailing fields on existing events without breaking old parsers, and an
  old file's smaller payload sizes are exactly as valid to a new parser.
- **No FlatBuffers, no UBJSON, no schema-compiler toolchain.** FlatBuffers is
  built around finishing one complete buffer atomically; adapting it to a
  growing append-only stream means wrapping each event in its own
  independently-finished message, at which point most of its actual benefit
  (shared schema, zero-copy across a whole buffer) is gone and the per-message
  overhead (vtable, root offset) is a real cost against file size at 50-60
  events/sec. Slippi wraps its raw event stream in a UBJSON container
  (`{"raw": <binary>, "metadata": {...}}`) to embed a binary blob inside a
  JSON-like document; this format skips that problem entirely by not having
  a JSON-like document — the file *is* the binary event stream, with a small
  fixed binary header instead of a UBJSON wrapper.
- **Little-endian**, matching the host platforms this project targets
  (Linux/Windows, both little-endian in practice) — not Slippi's
  big-endian, which was a holdover from the GameCube/Wii's native PowerPC
  byte order and has no reason to carry over here. All multi-byte
  integer and float fields in this document are little-endian unless
  stated otherwise.
- **Native struct layout, no bit-packing.** Every event payload is a
  fixed-size, `#pragma pack(push, 1)` C struct with no padding and no
  bitfields — the byte layout tables below are exactly `sizeof()` that
  struct, field by field, in declaration order.

## 3. File structure

```
+----------------+----------------------------------------+
| File Header    | 88 bytes, fixed                         |
+----------------+----------------------------------------+
| Event Stream   | variable length, sequence of events      |
|                | (EventPayloads, then GameStart, then     |
|                | interleaved PreFrame/PostFrame per real   |
|                | frame, then GameEnd)                      |
+----------------+----------------------------------------+
```

There is no footer, no trailing metadata block, and no UBJSON/JSON wrapper
of any kind — the event stream *is* the rest of the file, up to
`streamLength` bytes (§3.1) after the header, or up to EOF for a file whose
recording session never cleanly finished.

### 3.1 File header (88 bytes)

| Offset | Size | Type       | Field                  | Notes                                                  |
|-------:|-----:|------------|------------------------|---------------------------------------------------------|
| 0x00   | 4    | `char[4]`  | `magic`                | Always the ASCII bytes `R`, `M`, `G`, `R` (no NUL).      |
| 0x04   | 1    | `u8`       | `version`               | Format version. `3` for everything described here.      |
| 0x05   | 3    | `u8[3]`    | `reserved`              | Always zero. Reserved for future header fields.          |
| 0x08   | 4    | `u32`      | `streamLength`          | Byte length of the event stream that follows the header. |
| 0x0C   | 64   | `char[64]` | `goodName`              | The recorded ROM's `GoodName` (mupen64plus-core's ROM database identity string), UTF-8, NUL-padded — not necessarily NUL-terminated if it fills the field. Truncated if longer than 64 bytes. |
| 0x4C   | 4    | `u32`      | `recorderSchemaVersion` | This recorder's revision of its own understanding of `goodName`'s memory layout — see §3.3. |
| 0x50   | 8    | `u64`      | `recordedAtEpochSeconds`| Wall-clock time the recording started, seconds since the Unix epoch (UTC) — what `time(nullptr)` returns. Independent of the filename's timestamp (§3.4), though the recorder writes the same instant to both. |

**`streamLength` is written as `0` when the file is opened**, and is the
*only* field patched after the fact: once the match ends (or is otherwise
finalized), the recorder seeks back to offset `0x08` and writes the real
length, then closes the file. This is a direct crash-safety trick: a reader
can tell "was this file's recording session ever cleanly finished" just by
checking whether `streamLength` is nonzero, and a file left at `streamLength
== 0` (the process died, or the emulator was killed, mid-match) is still a
valid, parseable, truncated recording — a reader should fall back to reading
events until EOF instead of trusting the header's length in that case.

### 3.2 Event stream

A sequence of events, back to back, no padding between them:

```
+------+----------------------------+
| 1B   | command code               |
+------+----------------------------+
| N B  | payload (N declared by      |
|      | the EventPayloads event)   |
+------+----------------------------+
```

The very first event in every file is always `EventPayloads` (`0x01`, §4.1).
Every subsequent event is one of `GameStart` (`0x02`, once, immediately
after `EventPayloads`), `PreFrameUpdate` (`0x03`), `PostFrameUpdate`
(`0x04`), `ItemUpdate` (`0x06`, schema v2+, see §4.6), `StageHazardUpdate`
(`0x07`, schema v3+, see §4.7), or `GameEnd` (`0x05`, once, at the very end
— present only if the recording session finished cleanly).

For each real emulated frame that the match is actively ongoing
(`game_status == 1`, see §7.5) and has at least one seated port, the
recorder writes one `PreFrameUpdate` immediately followed by one
`PostFrameUpdate` for each seated port, ports visited in ascending order
(0, 1, 2, 3) — i.e. for a 2-player match on ports 0 and 1, frame N looks
like `Pre(port 0), Post(port 0), Pre(port 1), Post(port 1)`. Ports that are
empty or unseated that frame have no events at all — never a zeroed/dummy
event — so a reader must not assume every frame has all four ports present,
and must not assume a fixed number of events per frame.

That same frame N is then followed by zero or more `ItemUpdate` events, one
per Item/Weapon `GObj` currently live and not currently held by a fighter
(§4.6) — zero if none are live this frame, again never a zeroed/dummy event.
Items and Weapons live on two separate `GObj` lists, not one shared list
(§4.6/§7.6). After
those, zero or one `StageHazardUpdate` event (§4.7) — written only if at
least one tracked hazard is currently active. A reader correlates both
event types to a frame via their own `frame` field, the same way it
correlates a `PreFrameUpdate`/`PostFrameUpdate` pair.

### 3.3 `goodName` and `recorderSchemaVersion` — two independent axes

These two fields exist to answer two different questions, and conflating
them is the mistake to avoid:

- **`goodName`** identifies *which ROM build* produced the file — a unique
  identity, not a family. `SmashRemix2.0.1` and a hypothetical
  `SmashRemix2.0.2` are different `goodName`s even though they're the "same"
  mod, because their memory layouts can differ.
- **`recorderSchemaVersion`** identifies *which revision of this recorder's
  interpretation* of that specific `goodName`'s memory layout produced the
  file. It's bumped whenever that interpretation changes in a way that
  affects recorded output — which includes but isn't limited to adding a new
  field. A fix to an *existing* field's offset (silently changing recorded
  *values* without changing any event's declared byte size) needs a bump
  too, since the per-event `EventPayloads` declared-size mechanism (§5) has
  no way to signal that on its own.

`recorderSchemaVersion` is its own counter **per `goodName`**, not global:
`SmashRemix2.0.1` schema `3` and `SmashRemix2.0.2` schema `1` are unrelated
numbering spaces, each starting fresh at `1` for that `goodName`'s first
supported revision.

A reader that only knows how to interpret one specific `(goodName,
recorderSchemaVersion)` pair should check both explicitly before trusting
any event payload's semantics, rather than assuming every file it can parse
was produced by the same game and recorder revision it was built against.

### 3.4 Filename convention

Not part of the on-disk format itself (a reader must not depend on it — the
header's own `recordedAtEpochSeconds` is the source of truth for when a
recording started, precisely because filenames get renamed/copied/re-shared
and can't be trusted), but the recorder names files
`YYYYMMDD-HHMMSS[-Player1][-Player2]...rmgr` — 4-digit year, 24-hour clock,
local time, one hyphen-joined segment per seated player's name (each capped
at 24 characters, filesystem-unsafe characters replaced with `_`). The
timestamp reflects the same instant written to `recordedAtEpochSeconds`,
just rendered as local wall-clock time instead of a UTC epoch value.

If that name is already taken (e.g. two matches recorded within the same
second), the recorder appends `-2`, `-3`, ... before the extension until it
finds a free name, rather than overwriting an existing file.

Headless export of an existing `.krec` (see `Replay::SetOutputPathOverride()`)
uses a different, caller-chosen base name instead - by convention
`<krec name>.rmgr`, the same stem as the source `.krec` it was exported from,
so the two plainly correspond by name. Each match within that `.krec` gets
its own explicitly-numbered file from that base: `<krec name>-1.rmgr`,
`<krec name>-2.rmgr`, ... (not just `<krec name>.rmgr` for the first match).
The same collision-avoidance above still applies on top, e.g. if the same
`.krec` is exported a second time.

For this headless export path, `recordedAtEpochSeconds` is **not** wall-clock
export time (headless replay can run at up to 2000% speed, so that would
reflect when the export happened to reach that match, not when it was
originally played). Instead it's derived from the source `.krec`'s own
recording-start timestamp (that file's header, or a filename-derived
fallback if the header's is missing/zero - see
`KailleraExport::ParseKrecFile()`), plus how many of the `.krec`'s own input
frames have been consumed by the time that match is reached, divided by 60
under the assumption that the original recording ran at a constant 60fps
(true for any real Kaillera session) - see
`Replay::SetRecordedAtBaseOverride()`. The exported filename itself is still
just `<krec name>-N.rmgr` as above, not timestamp-based, for this path.

## 4. Events

### 4.1 Event Payloads — code `0x01`

Always the first event in the file. Declares the exact payload size (not
including the 1-byte command code) of every other event type this file
uses. This is the entire forward-compatibility mechanism: a parser that
doesn't recognize a command code looks it up here and skips exactly that
many bytes, rather than guessing or breaking.

| Offset | Size    | Type      | Field         | Notes                                          |
|-------:|--------:|-----------|---------------|--------------------------------------------------|
| 0x00   | 1       | `u8`      | `count`       | Number of `(code, size)` entries that follow.    |
| 0x01   | 3×count | see below | `entries`     | `count` repetitions of the 3-byte entry below.   |

Each entry:

| Offset (rel.) | Size | Type  | Field  | Notes                                    |
|---------------:|-----:|-------|--------|-------------------------------------------|
| +0x00          | 1    | `u8`  | `code` | The event command code this entry describes. |
| +0x01          | 2    | `u16` | `size` | That event's payload size, in bytes.      |

v1 always declares exactly 4 entries, in this order: `GameStart`,
`PreFrameUpdate`, `PostFrameUpdate`, `GameEnd` — currently sized 164, 9, 50,
and 5 bytes respectively (`GameStart` grew from its original 150 bytes and
`PostFrameUpdate` from its original 42 bytes via the field-append mechanism
in §5; see §4.2/§4.4's notes on that). Recorder schema v2 (§3.3) declares a
5th entry, `ItemUpdate` (§4.6); schema v3 declares a 6th, `StageHazardUpdate`
(§4.7); schema v5 declares a 7th and 8th, `HitboxUpdate` (§4.8) and
`HurtboxUpdate` (§4.9) — all new event types, not field appends, per §5's
second mechanism. A future format version could declare still more entries or the
same entries with even larger sizes — see §5. **A parser must always read
an event's size from that file's own `EventPayloads` event, never hardcode
it, and must always read `count` itself rather than assuming a fixed number
of entries** — this is exactly why: an old parser reading a newer-schema
file that correctly implements both of those still parses it correctly,
skipping entries it doesn't recognize.

### 4.2 Game Start — code `0x02`

Written exactly once, immediately after `EventPayloads`. Everything static
for the whole match — nothing here changes frame to frame. **Player display
names are sourced from netplay room metadata (RMG-K's own slot-indexed name
table, populated by every netplay path), never from Smash Remix's in-game
name tags** — for an offline match, or a port with no assigned name, the
corresponding `playerNames` entry is all zero bytes.

Payload size: **164 bytes.**

| Offset | Size | Type      | Field                | Notes                                                    |
|-------:|-----:|-----------|-----------------------|------------------------------------------------------------|
| 0x00   | 1    | `u8`      | `stageId`             | See §7.2.                                                  |
| 0x01   | 1    | `u8`      | `gameType`             | `1` time, `2` stock, `3` both (Remix always forces stock). |
| 0x02   | 1    | `u8`      | `stockCountSetting`    | 0-based (i.e. `2` means "3 stocks").                       |
| 0x03   | 1    | `u8`      | `timeLimitMinutes`     | `100` = infinite.                                          |
| 0x04   | 1    | `u8`      | `damageRatio`          | `50` = 50%, `200` = 200%.                                  |
| 0x05   | 1    | `u8`      | `itemFrequency`        | `0` none .. `5` high.                                      |
| 0x06   | 16   | struct[4] | `ports`                | 4× the 4-byte `PortSettings` struct below, port 0-3 in order. |
| 0x16   | 128  | char[4][32]| `playerNames`         | 4× a 32-byte, NUL-padded (not necessarily NUL-terminated if exactly 32 chars) name string, port 0-3 in order. |
| 0x96   | 1    | `u8`      | `teamsEnabled`         | `0` off, `1` on. **Appended field** — see the note below the table. |
| 0x97   | 1    | `u8`      | `handicapMode`         | `0` off, `1` on, `2` auto.                                 |
| 0x98   | 4    | `u8[4]`   | `portTeam`             | Team number per port, index = port 0-3.                    |
| 0x9C   | 4    | `u8[4]`   | `portHandicap`         | Per-port handicap value, meaningful only when `handicapMode != 0`. |
| 0xA0   | 4    | `u8[4]`   | `portCpuLevel`         | CPU difficulty per port; meaningless for a `human` port.    |

**`teamsEnabled` through `portCpuLevel` (offsets `0x96`-`0xA3`) were appended
after the original v1 fields** (`stageId` through `playerNames`, offsets
`0x00`-`0x95`, unchanged since the format's first version) — per §5's
field-addition rule, this is why they sit after `playerNames` rather than
next to the other match-wide settings at the top of the struct. The four
`port*` arrays require the same player-object/player-struct pointer chase
as Post-Frame Update (§4.4); if a ReadPortMatchInfo/ReadPortPlayerState open
finds a port's characters not yet spawned (e.g. `GameStart` was written
during the pre-match countdown, before `game_status` reaches `1`), that
port's `portTeam`/`portHandicap`/`portCpuLevel` entries are left at `0`
rather than the real value — a reader can't distinguish "genuinely 0" from
"not available yet" for these three fields alone.

`PortSettings` (4 bytes, repeated 4× inline above at offset `0x06` — **not**
a separately declared event, just a fixed sub-layout within `GameStart`,
and distinct from the appended `portTeam`/`portHandicap`/`portCpuLevel`
arrays above):

| Offset (rel.) | Size | Type | Field         | Notes                              |
|---------------:|-----:|------|---------------|--------------------------------------|
| +0x00          | 1    | `u8` | `slotType`     | `0` human, `1` CPU, `2` empty.       |
| +0x01          | 1    | `u8` | `characterId`  | See §7.1. Meaningless if `slotType == 2`. |
| +0x02          | 1    | `u8` | `costumeId`    |                                        |
| +0x03          | 1    | `u8` | `teamColor`    |                                        |

### 4.3 Pre-Frame Update — code `0x03`

Input-side data, captured **before** the game processes that frame's inputs.
One event per seated port per frame (§3.2). Uses the game's already-processed
button/stick values (`playerStruct+0x1BC/+0x1C2/+0x1C3` — see
`Source/RMG-Core/ReplayMemory.cpp`), which is the one input representation
available uniformly for **both** human and CPU-controlled ports; the raw
physical-controller struct (`+0x1B0`) is human-ports-only and is not
recorded.

Payload size: **9 bytes.**

| Offset | Size | Type   | Field     | Notes                                                        |
|-------:|-----:|--------|-----------|-----------------------------------------------------------------|
| 0x00   | 4    | `i32`  | `frame`   | Frame counter, `0` at the first frame this match's recording enters the "ongoing" game state (`game_status == 1`). Monotonically increasing, one recorded frame per real emulated frame. |
| 0x04   | 1    | `u8`   | `port`    | `0`-`3`.                                                        |
| 0x05   | 2    | `u16`  | `buttons` | Processed button bitmask. See §7.4.                             |
| 0x07   | 1    | `i8`   | `stickX`  | Processed stick X, signed.                                      |
| 0x08   | 1    | `i8`   | `stickY`  | Processed stick Y, signed.                                      |

### 4.4 Post-Frame Update — code `0x04`

State-side data, captured **after** that frame's physics/collision
resolution — the resulting state. One event per seated port per frame,
always immediately following that port's `PreFrameUpdate` in the stream.

Payload size: **50 bytes.**

| Offset | Size | Type   | Field                | Notes                                                            |
|-------:|-----:|--------|------------------------|---------------------------------------------------------------------|
| 0x00   | 4    | `i32`  | `frame`                | Same frame counter as the paired `PreFrameUpdate`.                   |
| 0x04   | 1    | `u8`   | `port`                 | `0`-`3`.                                                              |
| 0x05   | 1    | `u8`   | `characterId`           | See §7.1.                                                             |
| 0x06   | 2    | `u16`  | `actionStateId`         | See §7.3.                                                             |
| 0x08   | 4    | `f32`  | `positionX`             | IEEE-754 single precision.                                            |
| 0x0C   | 4    | `f32`  | `positionY`             |                                                                        |
| 0x10   | 4    | `i32`  | `facingDirection`       | `1` = facing right, `-1` = facing left. (Integer in this game, not a float like Slippi's Melee-derived field.) |
| 0x14   | 4    | `f32`  | `velocityX`             |                                                                        |
| 0x18   | 4    | `f32`  | `velocityY`             |                                                                        |
| 0x1C   | 4    | `u32`  | `damagePercent`         | Whole-number percent, as the game itself stores it (not a float).     |
| 0x20   | 1    | `i8`   | `stocksRemaining`       | 0-based; negative once eliminated.                                    |
| 0x21   | 1    | `u8`   | `jumpsRemaining`        | Schema v7+. `jumpsMax` (per-character, from `FTAttributes`) minus `jumps_used` (`playerStruct+0x148`, a `u8` that resets to `0` on landing). `0` through most of a grounded match is normal, not a sign of a broken read; Smash Remix can also force this to `0` without that many real jump inputs (e.g. certain up-specials write `jumps_used = jumps_max` directly). Named/interpreted as `jumpsUsed` through schema v6 - that read was at the wrong width (`u32` instead of `u8`, landing on padding on a word-swapped emulator without the byte-read address XOR) and read a constant `0` for an entire match, every port, regardless of real jump activity. **Schema v6 and earlier files' byte here is meaningless - it's that bug's output, not real data.** |
| 0x22   | 1    | `u8`   | `groundedState`         | `0` grounded, `1` airborne.                                           |
| 0x23   | 1    | `u8`   | `hurtboxState`          | `0x03` = intangible/invincible; see `ReplayMemory.cpp` for the full set observed. |
| 0x24   | 2    | `u16`  | `hitstunCounter`        | Non-zero while in hitstun.                                            |
| 0x26   | 4    | `u32`  | `actionFrameCounter`    | Frame counter of the current action state (resets when the action state changes). |
| 0x2A   | 4    | `u32`  | `comboHitCount`         | v1 field-append (§5). Native engine combo counter, not mod-added - tracked even with the in-game combo meter display off. Belongs to the *victim* (this port), not the attacker: hits taken in the current unbroken chain. `0` = no active chain, `1` = a single hit (not yet a "combo" by convention), `2+` = an actual combo. Zeroes the instant the chain breaks - Smash Remix extends what counts as "unbroken" to survive grabs/wall-bounces/tech-chases, which vanilla would reset. Source: smashremix `docs/ram-map.md` §13. |
| 0x2E   | 4    | `u32`  | `comboDamage`           | v1 field-append (§5). Running damage dealt within the same chain as `comboHitCount`; zeroes at the same instant. |

### 4.5 Game End — code `0x05`

Written exactly once, at the very end of a cleanly-finished recording
session, immediately before the header's `streamLength` is patched. **A
truncated file (crash, force-quit) has no `GameEnd` event at all** — its
absence, together with `streamLength == 0`, is how a reader distinguishes
an incomplete recording from a genuinely short match.

Payload size: **5 bytes.**

| Offset | Size | Type    | Field          | Notes                                                                 |
|-------:|-----:|---------|-----------------|--------------------------------------------------------------------------|
| 0x00   | 1    | `u8`    | `endReason`     | `0` aborted (match reset, or the emulator/process stopped mid-match — these two causes are not currently distinguished), `1` normal end. |
| 0x01   | 4    | `i8[4]` | `placements`    | Final stocks remaining, per port 0-3. `-1` for any port that was never seated. |

### 4.6 Item Update — code `0x06`

**New in recorder schema v2** for `SmashRemix2.0.1` (§3.3); **the type field's
meaning changed in schema v3** — a schema-v2 file's `ItemUpdate.kind` field
does not exist (it was `typeId`, and that field's *value* was wrong — see
below); a schema-v1 file has no `ItemUpdate` event at all. Both are new
event types per §5's second mechanism, **not header `version` bumps** — an
old parser that reads `EventPayloads`'s `count` field dynamically and skips
codes it doesn't recognize parses a newer-schema file with no changes
required.

**Schema v2's `typeId` field was a bug, not just a gap — discard any data
captured under schema v2.** It read the object's `+0x0C` offset as one
32-bit value, following a Remix ASM comment that called it "projectile ID."
Cross-checked against a real SSB64 decompilation
([VetriTheRetri/ssb-decomp-re](https://github.com/VetriTheRetri/ssb-decomp-re)),
`+0x0C` is actually four packed single bytes (`link_id`, `dl_link_id`,
`frame_draw_last`, `obj_kind`) — reading it as a `u32` produces a large,
constantly-changing value (the `frame_draw_last` byte alone guarantees
that), never a stable type. See smashremix `docs/ram-map.md` §10.4 for the
full story.

Zero or more per frame — one per live Item or Weapon `GObj` (the engine's
universal object base), following that frame's `PreFrameUpdate`/
`PostFrameUpdate` pairs. Items and Weapons live on two **separate** fixed
global `GObj` lists — `gGCCommonLinks[4]` (Item) and `gGCCommonLinks[5]`
(Weapon), not one shared list filtered by `link_id`
(`ReplayMemory::ReadItemObjects()` walks both). An earlier version of this
recorder (schema v3 and below) only walked the Item list, so **no file
recorded under schema v3 or earlier ever contains a real Weapon
`ItemUpdate`**, even though the wire format already supported `linkId ==
5` — see §5's v3→v4 note. `4` = **Item** (thrown/spawned items, stage
hazard objects, and some fighter-held things like Link's pulled bomb), `5`
= **Weapon** (a free-flying character special-move projectile: boomerang,
fireball, charge shot, …).

A **held** Item (e.g. Link's bomb while still in his hand, before it's
thrown) is not emitted at all: while held, the engine re-parents the
item's position data onto the holding fighter's hand-bone joint, so it
stops being a world coordinate and reads as a meaningless local offset
near `(0,0,0)` instead — "position still reads near `(0,0,0)`" is used as
a proxy for "currently held" and such objects are skipped (schema v4+;
skipped because there's nothing meaningful to report until the item is
thrown/dropped, not because held items don't exist). **Not**
`ITStruct::owner_gobj != NULL`, despite schema v4/v5's use of exactly that
proxy: decomp-confirmed (`itMainSetFighterRelease()`) that `owner_gobj` is
deliberately retained across a throw/drop for later damage/KO attribution
and is only cleared by a separate, not-always-called function — it reads
non-NULL for essentially an item's *entire* lifetime, held or not, so it
never actually distinguished the two states. Schema v6 fixes this — see
§5. Weapons are never held, so this doesn't apply to `linkId == 5`.
Fighters and any other `GObj` kind that might appear on either list are
also skipped, since the further pointer chase below only makes sense for
Items/Weapons.

Payload size: **25 bytes.**

| Offset | Size | Type    | Field           | Notes                                                                 |
|-------:|-----:|---------|------------------|--------------------------------------------------------------------------|
| 0x00   | 4    | `i32`   | `frame`          | Same frame counter as that frame's `PreFrameUpdate`/`PostFrameUpdate`.    |
| 0x04   | 4    | `u32`   | `objectAddress`  | The `GObj`'s own RDRAM address. **Not a semantic spawn ID** the engine assigns — just the closest available stable per-object identity, valid for as long as that object is alive. Two `ItemUpdate` events across different frames with the same `objectAddress` are very likely (not guaranteed) the same live object; the address can be reused once an object is freed. |
| 0x08   | 1    | `u8`    | `linkId`         | `4` = Item, `5` = Weapon — which enum `kind` below means. See §7.6.       |
| 0x09   | 4    | `i32`   | `kind`           | `ITKind` (`linkId == 4`) or `WPKind` (`linkId == 5`) — the real, named per-instance type, one further pointer hop past `linkId`: `GObj+0x84` → `ITStruct*`/`WPStruct*` → `+0x0C`. Full enums in §7.6. |
| 0x0D   | 4    | `f32`   | `positionX`      | IEEE-754 single precision. World-space, via `GObj+0x74` → `DObj*` → `+0x1C`. |
| 0x11   | 4    | `f32`   | `positionY`      | `DObj+0x20`.                                                              |
| 0x15   | 4    | `f32`   | `positionZ`      | `DObj+0x24`. Position (X/Y/Z alike) is now **confirmed exactly** against the decomp — no longer "inferred by pattern" as earlier drafts of this doc said. |

Deliberately not captured (not yet mapped in memory — see §8): velocity,
damage/knockback dealt, size (though for Samus's Charge Shot specifically,
`kind`'s `WPStruct` exposes a discrete 0-7 `charge_size` level — not
captured as its own field, since it's meaningful only for that one `kind`),
owner/attacker port, and any per-object timer/expiration. A future schema
version can append any of these as trailing fields (§5) once mapped,
without breaking this version's readers.

### 4.7 Stage Hazard Update — code `0x07`

**New in recorder schema v3.** Zero or one per frame, following that
frame's `ItemUpdate` events — written only when at least one tracked hazard
is currently active, same sparse convention as `ItemUpdate` (never a
zeroed/placeholder event for "nothing active"). Currently tracks exactly
one hazard: Whispy Woods' wind on Dream Land. More hazards (Zebes' rising
acid, Duel Zone's disappearing platforms, …) can claim more bits in
`hazardFlags` later via the field-append mechanism (§5), without needing a
new event type or breaking existing readers.

Payload size: **5 bytes.**

| Offset | Size | Type  | Field         | Notes                                                                 |
|-------:|-----:|-------|----------------|--------------------------------------------------------------------------|
| 0x00   | 4    | `i32` | `frame`        | Same frame counter as that frame's `PreFrameUpdate`/`PostFrameUpdate`.    |
| 0x04   | 1    | `u8`  | `hazardFlags`  | Bitmask, currently only bit `0x01` defined: Whispy Woods currently blowing (Dream Land only — the underlying memory this reads from is a per-stage union, so this flag is only ever set to `1` when `GameStart.stageId` is Dream Land; see smashremix `docs/ram-map.md` §10.3). |

### 4.8 Hitbox Update — code `0x08`

**New in recorder schema v5.** Zero or more per frame, one per currently
*active* hitbox slot, following that frame's `StageHazardUpdate` event (if
any) — sparse like `ItemUpdate`: a disabled slot (`attackState == 0`) is
never emitted, never a zeroed/placeholder event. Fighters have 4
simultaneous hitbox slots (`FTAttackColl`); each live Item or Weapon
(§4.6) has up to 2 (`ITAttackColl`/`WPAttackColl`). Hitboxes are **spheres**
— a world-space center plus a single radius, not a box — per smashremix
`docs/ram-map.md` §14's intro.

This is deliberately verbose rather than deduplicated against action state:
the plan is to record every active slot every frame for now, and — once
it's confirmed that a character's hitbox geometry is reliably derivable
from `(characterId, actionStateId, actionFrameCounter)` alone — stop
recording it for that character and compute it instead. See §8.

**Confidence caveat:** Fighter hitbox fields (`ownerKind == 0`) are
high-confidence — confirmed both via real Remix ASM call sites and the
decomp, agreeing exactly. Item/Weapon hitbox fields (`ownerKind == 1` or
`2`) have high-confidence field *order* (read directly from source) but
their exact byte *offsets* are hand-derived, not compiler-verified — see
smashremix `docs/ram-map.md` §14.5 for the full caveat, including why
`MPCollData`'s 208-byte size is the largest single source of possible
error.

Payload size: **55 bytes.**

| Offset | Size | Type  | Field              | Notes                                                                 |
|-------:|-----:|-------|---------------------|--------------------------------------------------------------------------|
| 0x00   | 4    | `i32` | `frame`              | Same frame counter as that frame's `PreFrameUpdate`/`PostFrameUpdate`.    |
| 0x04   | 1    | `u8`  | `ownerKind`          | `0` = Fighter, `1` = Item, `2` = Weapon — which struct this hitbox came from, and how `ownerId` below is interpreted. |
| 0x05   | 4    | `u32` | `ownerId`            | Fighter: the port (`0`-`3`), zero-extended. Item/Weapon: the owning `GObj`'s own RDRAM address — same identity as `ItemUpdate.objectAddress` (§4.6), so a `HitboxUpdate` can be correlated to that frame's `ItemUpdate` for the same live object. |
| 0x09   | 1    | `u8`  | `slotIndex`          | Fighter: `0`-`3`. Item/Weapon: `0`-`1`.                                   |
| 0x0A   | 1    | `u8`  | `attackState`        | `1` = fresh (became active this frame), `2` = transfer, `3` = interpolate. Never `0` — disabled slots aren't emitted at all. |
| 0x0B   | 4    | `i32` | `damage`             |                                                                            |
| 0x0F   | 4    | `f32` | `positionX`          | World-space, already transformed (`pos_curr`).                           |
| 0x13   | 4    | `f32` | `positionY`          |                                                                            |
| 0x17   | 4    | `f32` | `positionZ`          |                                                                            |
| 0x1B   | 4    | `f32` | `size`               | Radius.                                                                   |
| 0x1F   | 4    | `i32` | `angle`              | Knockback angle.                                                          |
| 0x23   | 4    | `i32` | `knockbackScale`     |                                                                            |
| 0x27   | 4    | `i32` | `knockbackWeight`    |                                                                            |
| 0x2B   | 4    | `i32` | `knockbackBase`      |                                                                            |
| 0x2F   | 4    | `i32` | `element`            |                                                                            |
| 0x33   | 4    | `i32` | `shieldDamage`       |                                                                            |

Deliberately not captured: attack group ID, which body-part joint a
Fighter hitbox is bone-anchored to, `fgm`/motion-attack bitfield flags,
`interact_mask` (Item/Weapon only — which object classes a hitbox can hit),
priority (Item/Weapon only), and already-hit-target tracking
(`attack_records`/`GMAttackRecord`, so a reader currently cannot tell
whether two `HitboxUpdate`s on different frames already hit the same
target or are two separate hits). A future schema version can append any
of these (§5) once there's a concrete use for them.

### 4.9 Hurtbox Update — code `0x09`

**New in recorder schema v5.** One per fighter hurtbox slot (11 per seated
port — `FTDamageColl`, one per body region), following that frame's
`HitboxUpdate` events. **Unlike every other per-frame event in this
format, this one is NOT sparse** — a seated port's 11 slots are (almost)
always all present, since hurtboxes exist essentially continuously while a
fighter is alive; there's no "disabled" state analogous to a hitbox's
`attackState == 0` to filter on. Fighter-only: items/weapons have at most
a single *static*, per-item-type hurtbox template
(`ITAttributes.damage_coll_offset`/`damage_coll_size`) with no live
per-instance struct traced yet, so there is nothing per-frame to report
for them.

Same verbosity rationale as `HitboxUpdate` (§4.8) — record exhaustively
now, prune later once shown to be derivable from action state alone. See
§8.

Payload size: **51 bytes.**

| Offset | Size | Type  | Field         | Notes                                                                 |
|-------:|-----:|-------|----------------|--------------------------------------------------------------------------|
| 0x00   | 4    | `i32` | `frame`        | Same frame counter as that frame's `PreFrameUpdate`/`PostFrameUpdate`.    |
| 0x04   | 1    | `u8`  | `port`         | `0`-`3`.                                                                  |
| 0x05   | 1    | `u8`  | `slotIndex`    | `0`-`10`.                                                                 |
| 0x06   | 4    | `i32` | `hitStatus`    | Per-bone Vulnerable/Invincible/Intangible. Raw value — the exact numeric mapping for this *per-bone* field isn't independently confirmed the way the whole-character convention is (`PostFrameUpdate.hurtboxState`, §4.4, `3` = intangible). |
| 0x0A   | 4    | `i32` | `placement`    | `0` = low, `1` = middle, `2` = high.                                     |
| 0x0E   | 1    | `u8`  | `isGrabbable`  | `0`/`1`.                                                                  |
| 0x0F   | 4    | `f32` | `positionX`    | **Approximation, not the true hurtbox center** — the bone's own world-space joint position (its `DObj`'s translate). Does NOT apply `offsetX/Y/Z` below or the bone's rotation on top. |
| 0x13   | 4    | `f32` | `positionY`    |                                                                            |
| 0x17   | 4    | `f32` | `positionZ`    |                                                                            |
| 0x1B   | 4    | `f32` | `offsetX`      | Authored, bone-relative, untransformed — the raw value that would need to be composed with the bone's rotation to get the true center.    |
| 0x1F   | 4    | `f32` | `offsetY`      |                                                                            |
| 0x23   | 4    | `f32` | `offsetZ`      |                                                                            |
| 0x27   | 4    | `f32` | `sizeX`        | Anisotropic — a `Vec3f`, not a single radius like a hitbox's `size`.      |
| 0x2B   | 4    | `f32` | `sizeY`        |                                                                            |
| 0x2F   | 4    | `f32` | `sizeZ`        |                                                                            |

Deliberately not captured: no per-hurtbox weak-point flag or damage
multiplier exists in the underlying struct (`FTStruct.damage_mul` is a
related but different, *whole-fighter* multiplier, not per-hurtbox) — see
smashremix `docs/ram-map.md` §14.2.

## 5. Versioning and forward compatibility

Two independent mechanisms, matching §4.6 of the original design rationale:

- **Field additions to an existing event:** always append new fields to the
  *end* of that event's payload, never insert in the middle. An old parser
  — which learned the event's size from that file's own `EventPayloads`
  event, which will correctly declare the *old*, smaller size for an old
  file — simply never reads the new trailing bytes. A new parser reading an
  old file sees the old, smaller declared size in that file's own
  `EventPayloads` event and correctly knows not to read fields that were
  never written.
- **New event types:** an old parser encountering a command code it doesn't
  recognize looks up its declared size in `EventPayloads` and skips exactly
  that many bytes, then continues from the next event.

`FileHeader.version` (currently `3`) is reserved for a breaking change to
the *header* or the overall framing itself — not for anything the two
mechanisms above already cover, and not for tracking which game/ROM
produced a file or how that recorder's understanding of it has evolved
either — that's `goodName`/`recorderSchemaVersion` (§3.3), a deliberately
separate axis from the container format itself.

**Compatibility note:** `version` jumped `1` → `2` (adding `goodName` and
`recorderSchemaVersion`) → `3` (adding `recordedAtEpochSeconds`), each a
breaking change to files already recorded under the prior version (they
lack those fields entirely, at a different header size) — accepted
deliberately both times, since no file predating this spec's current form
has any external consumer yet. A `version 1` or `version 2` file is not
expected to parse under this spec.

**Recorder schema history, for `SmashRemix2.0.1`** (§3.3's separate,
non-breaking axis): schema `1` is the original event set described above.
Schema `2` added the `ItemUpdate` event (§4.6) — additive only, so a
schema-`1`-aware parser correctly reads a schema-`2` file's other events and
simply never sees `ItemUpdate`. **Schema `2`'s `ItemUpdate.typeId` field was
wrong, not just incomplete** (§4.6) — reading `+0x0C` on the raw object as a
32-bit value, when it's actually a packed byte. Schema `3` replaced it with
correctly-derived `linkId`/`kind` fields (a different `ItemUpdate` payload
size — 25 bytes, not 24 — so a parser that reads the size from
`EventPayloads` as this spec requires handles the difference correctly) and
added the `StageHazardUpdate` event (§4.7). **Any data captured under
schema `2` should be discarded and re-recorded, not migrated** — its
`typeId` values were never meaningful.

Schema `4` fixed two bugs in `ReadItemObjects()` that changed *which*
objects get emitted, not the `ItemUpdate` payload's byte layout (still 25
bytes, same as schema `3`): (1) Weapons were never actually reachable —
schema `3` and earlier only walked the Item list (`gGCCommonLinks[4]`), so
despite `ItemUpdate.linkId` supporting `5` (Weapon) in the wire format, no
file recorded before schema `4` contains a real Weapon event (fireballs,
boomerang, charge shot, PK Fire/Thunder, …); (2) held Items (e.g. Link's
bomb while still in his hand) were recorded with a meaningless, re-parented
position (typically `(0,0,0)`) instead of being skipped — see §4.6. **Data
captured under schema `3` and earlier undercounts real projectile activity
(no Weapons, phantom held-item entries) but isn't corrupt the way schema
`2` was** — it doesn't need discarding, just doesn't reflect Weapons at
all.

Schema `5` added the `HitboxUpdate` (§4.8) and `HurtboxUpdate` (§4.9)
events — additive only, so a schema-`4`-aware parser correctly reads a
schema-`5` file's other events and simply never sees these two. Both are
deliberately verbose (every active hitbox slot, and *every* hurtbox slot
every frame, not just active ones) rather than deduplicated against action
state — the intent is to record exhaustively while the RAM mapping is
still being validated against real gameplay, then, once it's confirmed
that a character's hitbox/hurtbox geometry is reliably derivable from
`(characterId, actionStateId, actionFrameCounter)` alone, stop recording
it for that character (starting with the original 12, across both
versions) and compute it instead in a future schema. See §8.

Schema `6` fixed `ReadItemObjects()`'s "currently held" check, which
schema `4`/`5` got wrong: it used `ITStruct::owner_gobj != NULL` as a
proxy for "held," but decomp confirms (`itMainSetFighterRelease()`)
`owner_gobj` is deliberately *retained* across a throw/drop for later
damage/KO attribution — it's non-NULL for essentially an item's entire
lifetime, not just while held, so that check silently skipped nearly every
Item's flight, not just its brief held phase, for every schema `4`/`5`
file. Fixed by switching to a position-based proxy (still reads near
`(0,0,0)`, the hand-parented placeholder written while genuinely held —
see §4.6) that actually flips at the real release moment. Byte layout
unchanged — same class of fix as `3`→`4`. **Item `ItemUpdate` data from
schema `4`/`5` is missing most or all of every thrown/dropped Item's real
flight and should be re-recorded, not treated as complete** (Weapons are
unaffected — this check never applied to `linkId == 5`).

Schema `7` fixed `PostFrameUpdate.jumpsUsed`, which every prior schema got
wrong: it was read as a `u32` at `playerStruct+0x148`, but the real field
(decomp-confirmed) is a single `u8` there, with `+0x14A`-`0x14B` as padding
— on a word-swapped emulator, a *byte* read needs its address XORed with
`3` to land correctly, so the old `u32` read landed on that padding
instead, reading a constant `0` for an entire match, every port, no matter
how much jumping happened. Fixed the read width **and** switched what
gets exported: this field is now `jumpsRemaining` (`jumpsMax`, chased from
the per-character `FTAttributes`, minus the corrected `jumps_used`)
instead of `jumpsUsed` directly — more directly useful, and the original
goal. Same wire position/size (still a `u8`) — a pure "what this byte
means" fix, not a layout change, same class as `5`→`6`. **Schema `6` and
earlier files' byte here is meaningless — it's the constant-`0` bug's
output, not real data.**

## 6. Byte order and encoding

Everything in this file — the header and every event payload — is
**little-endian**. There is no manual byte-swapping anywhere in the
reference implementation: values are read directly from emulated memory via
`DebugMemRead8/16/32` (which already normalize to host byte order) and
written to disk as raw native-layout structs on little-endian host
platforms.

Floats are IEEE-754 single precision (32-bit), stored as the exact bit
pattern the game itself holds in memory — reinterpret the 4 bytes as a
`float`/`f32`, don't scale or convert.

Strings (`playerNames` in `GameStart`) are fixed-width byte arrays,
NUL-padded, **not necessarily NUL-terminated** if the name fills the full
field width — always read up to the declared field width and trim trailing
NULs, never scan for a terminator past the field boundary.

## 7. Reference tables

### 7.1 Character IDs

Valid range `0x00`-`0x60`.

**Vanilla (`0x00`-`0x1C`):** `0x00` Mario · `0x01` Fox · `0x02` Donkey Kong ·
`0x03` Samus · `0x04` Luigi · `0x05` Link · `0x06` Yoshi ·
`0x07` Captain Falcon · `0x08` Kirby · `0x09` Pikachu · `0x0A` Jigglypuff ·
`0x0B` Ness · `0x0C` Master Hand · `0x0D` Metal Mario ·
`0x0E`-`0x19` Polygon {Mario, Fox, DK, Samus, Luigi, Link, Yoshi, Falcon,
Kirby, Pikachu, Jigglypuff, Ness} in that order · `0x1A` Giant DK ·
`0x1B` Random · `0x1C` unused.

**Remix fighters (`0x1D`-`0x4C`):** `0x1D` Falco · `0x1E` Ganondorf ·
`0x1F` Young Link · `0x20` Dr. Mario · `0x21` Wario · `0x22` Dark Samus ·
`0x23` Link (EU) · `0x24` Samus (JP) · `0x25` Ness (JP) · `0x26` Lucas ·
`0x27` Link (JP) · `0x28` Falcon (JP) · `0x29` Fox (JP) · `0x2A` Mario (JP) ·
`0x2B` Luigi (JP) · `0x2C` DK (JP) · `0x2D` Pikachu (EU) ·
`0x2E` Jigglypuff (JP) · `0x2F` Jigglypuff (EU) · `0x30` Kirby (JP) ·
`0x31` Yoshi (JP) · `0x32` Pikachu (JP) · `0x33` Samus (EU) · `0x34` Bowser ·
`0x35` Giga Bowser · `0x36` Piano · `0x37` Wolf · `0x38` Conker ·
`0x39` Mewtwo · `0x3A` Marth · `0x3B` Sonic · `0x3C` Sandbag ·
`0x3D` Super Sonic · `0x3E` Sheik · `0x3F` Marina · `0x40` King Dedede ·
`0x41` Goemon · `0x42` Peppy · `0x43` Slippy · `0x44` Banjo ·
`0x45` Metal Luigi · `0x46` Ebisumaru · `0x47` Dragon King · `0x48` Crash ·
`0x49` Peach · `0x4A` Roy · `0x4B` Dr. Luigi · `0x4C` Lanky Kong.

**Remix polygons (`0x4D`-`0x60`):** `0x4D` Wario · `0x4E` Lucas ·
`0x4F` Bowser · `0x50` Wolf · `0x51` Dr. Mario · `0x52` Sonic ·
`0x53` Sheik · `0x54` Marina · `0x55` Falco · `0x56` Ganondorf ·
`0x57` Dark Samus · `0x58` Marth · `0x59` Mewtwo · `0x5A` Dedede ·
`0x5B` Young Link · `0x5C` Goemon · `0x5D` Conker · `0x5E` Banjo ·
`0x5F` Peach · `0x60` Crash.

### 7.2 Stage IDs

**Vanilla:** `0x00` Peach's Castle · `0x01` Sector Z · `0x02` Congo Jungle ·
`0x03` Planet Zebes · `0x04` Hyrule Castle · `0x05` Yoshi's Island ·
`0x06` Dream Land · `0x07` Saffron City · `0x08` Mushroom Kingdom ·
`0x09`-`0x0A` Dream Land Beta 1-2 · `0x0B` How to Play ·
`0x0C` Mini Yoshi's Island · `0x0D` Meta Crystal · `0x0E` Duel Zone ·
`0x0F` Race to the Finish · `0x10` Final Destination.

Remix adds a very large number of additional stages (`0x29` onward, into the
`0xD0`+ range) not enumerated here — see the *Known limitations* note in §8.

### 7.3 Action state IDs

`0x000`-`0x0DB` are shared across every character; `>= 0x0DC` is
character-specific (special moves — see §8).

```
0x000 DeadD(KO bottom)   0x001 DeadS(KO side)     0x002 DeadU(KO top)
0x003 ScreenKO           0x004 ScreenKOWait       0x005 Entry(spawn)
0x007 Revive1            0x008 Revive2            0x009 ReviveWait
0x00A Idle                0x00B-0x00D Walk1-3      0x00F Dash
0x010 Run                 0x011 RunBrake           0x012 Turn
0x013 TurnRun             0x014 JumpSquat          0x015 ShieldJumpSquat
0x016 JumpF               0x017 JumpB              0x018 JumpAerialF
0x019 JumpAerialB         0x01A Fall               0x01B FallAerial
0x01C Crouch              0x01D CrouchIdle         0x01E CrouchEnd
0x01F LandingLight        0x020 LandingHeavy       0x021 Pass(platform drop)
0x022 ShieldDrop          0x023 Teeter             0x024 TeeterStart
0x025-0x027 DamageHigh1-3 0x028-0x02A DamageMid1-3 0x02B-0x02D DamageLow1-3
0x02E-0x030 DamageAir1-3  0x031-0x032 DamageElec1-2
0x033 DamageFlyHigh       0x034 DamageFlyMid       0x035 DamageFlyLow
0x036 DamageFlyTop        0x037 DamageFlyRoll      0x038 WallBounce
0x039 Tumble              0x03A FallSpecial        0x03B LandingSpecial
0x03C Tornado             0x03D Barrel             0x03E-0x041 Pipe
0x042 CeilingBonk         0x043-0x048 Knocked down/getup
0x049-0x04A TechF/TechB   0x04B-0x04E Getup roll fwd/back
0x04F DownAttackD         0x050 DownAttackU        0x051 Tech
0x052 Clang               0x053 ClangRecoil        0x054 CliffCatch
0x055 CliffWait           0x056 CliffQuick         0x057-0x058 CliffClimbQuick1-2
0x059 CliffSlow           0x05A-0x05B CliffClimbSlow1-2
0x05C-0x05F CliffAttack Quick/Slow    0x060-0x063 CliffEscape Quick/Slow
0x064-0x07D Item pickup/throw actions 0x07E-0x097 Item-specific attacks
0x098 ShieldOn            0x099 Shield             0x09A ShieldOff
0x09B ShieldStun          0x09C RollF              0x09D RollB
0x09E ShieldBreak         0x09F ShieldBreakFall    0x0A0-0x0A3 Stun land/start
0x0A4 Stun                0x0A5 Sleep              0x0A6 Grab
0x0A7 GrabPull            0x0A8 GrabWait           0x0A9 ThrowF
0x0AA ThrowB              0x0AB-0x0B3 Captured/inhaled/egg-laid
0x0B5-0x0BC Being thrown  0x0BD Taunt              0x0BE Jab1
0x0BF Jab2                0x0C0 DashAttack         0x0C1-0x0C5 FTilt(High->Low)
0x0C7 UTilt                0x0C9 DTilt              0x0CA-0x0CE FSmash(High->Low)
0x0CF USmash               0x0D0 DSmash             0x0D1 Nair
0x0D2 Fair                 0x0D3 Bair               0x0D4 Uair
0x0D5 Dair                 0x0D6-0x0DA Aerial landing lag (N/F/B/U/D)
0x0DB LandingAirX(Z-cancel)
```

Derived predicates a consumer may find useful: dead/being-KO'd =
`actionStateId <= 0x004`; respawning = `actionStateId == 0x005` or
`0x007`-`0x009`; in hitstun = `actionStateId` in `0x025`-`0x039` (or check
`hitstunCounter` directly, §4.4); shielding = `actionStateId` in
`0x098`-`0x09B`; grabbed = `actionStateId` in `0x0AB`-`0x0BC`; attacking =
`actionStateId >= 0x0BE`. Airborne state should come from `groundedState`
(§4.4), not be inferred from `actionStateId` alone.

### 7.4 Controller button bits (`PreFrameUpdate.buttons`)

```
0x8000 A       0x0400 D-Down   0x0020 L
0x4000 B       0x0200 D-Left   0x0010 R
0x2000 Z       0x0100 D-Right  0x0008 C-Up
0x1000 Start   0x0004 C-Down   0x0002 C-Left
0x0800 D-Up                    0x0001 C-Right
```

### 7.5 `game_status` (internal, not directly exposed as an event field)

Governs the recorder's own state machine (not written to the file directly,
but explains the `frame` counter's start point and `GameEnd.endReason`):
`0` pre-match countdown, `1` ongoing (this is the only state that produces
`PreFrameUpdate`/`PostFrameUpdate` events, and `frame == 0` is the first
frame this state is observed), `2` paused, `5` ended.

### 7.6 `ItemUpdate.linkId` and `.kind` (schema v3+)

`linkId` (`ItemUpdate` offset `0x08`) is `4` (Item) or `5` (Weapon) — this
recorder never emits any other value (§4.6). It selects which of the two
enums below `kind` is a value from.

**`WPKind`** (`linkId == 5` — free-flying character special-move
projectiles), confirmed against the real SSB64 decompilation
([VetriTheRetri/ssb-decomp-re](https://github.com/VetriTheRetri/ssb-decomp-re)):

| Value | Name | | Value | Name |
|---:|---|---|---:|---|
| `0x00` | Fireball | | `0x10` | BulletNormal |
| `0x01` | Blaster | | `0x11` | BulletHard |
| `0x02` | ChargeShot | | `0x12` | ArwingLaser2D |
| `0x03` | SamusBomb | | `0x13` | ArwingLaser3D |
| `0x04` | Cutter | | `0x14` | LGunAmmo |
| `0x05` | EggThrow | | `0x15` | FFlowerFlame |
| `0x06` | YoshiStar | | `0x16` | StarRodStar |
| `0x07` | Boomerang | | `0x17`-`0x1F` | Pokémon/monster weapons (not individually enumerated here) |
| `0x08` | SpinAttack | | | |
| `0x09` | ThunderJoltAir | | | |
| `0x0A` | ThunderJoltGround | | | |
| `0x0B` | ThunderHead | | | |
| `0x0C` | ThunderTrail | | | |
| `0x0D` | PKFire | | | |
| `0x0E` | PKThunderHead | | | |
| `0x0F` | PKThunderTrail | | | |

`0x1F` is `nWPKindMonsterEnd` — Remix's own mod-added weapon IDs
(`src/Projectile.asm`, e.g. Banjo's egg, Sonic's spring) continue numbering
from there, per smashremix `docs/ram-map.md` §14.

**`ITKind`** (`linkId == 4` — thrown/spawned items, stage hazard objects,
and some fighter-held things like Link's pulled bomb). Two values are
confirmed directly against the decomp (`Bomb = 0x15`, matching Link's bomb;
`PKFirePillar = 0x14`, matching Ness's PK Fire pillar); the rest of this
table is `Hazards.standard`/`stage`/`pokemon` from Smash Remix's own
`src/Hazards.asm`, which the decomp cross-check confirms uses the *same*
numbering (only the offset this spec originally read it from — §4.6 — was
wrong, not this enum):

| Value | Name | | Value | Name | | Value | Name |
|---:|---|---|---:|---|---|---:|---|
| `0x00` | Crate | | `0x0F` | Bob-omb | | `0x1E` | Venusaur |
| `0x01` | Barrel | | `0x10` | Bumper | | `0x1F` | Porygon |
| `0x02` | Capsule | | `0x11` | GreenShell | | `0x20` | Onix |
| `0x03` | Egg | | `0x12` | RedShell | | `0x21` | Snorlax |
| `0x04` | MaximTomato | | `0x13` | Pokéball | | `0x22` | Goldeen |
| `0x05` | Heart | | `0x14` | PKFirePillar | | `0x23` | Meowth |
| `0x06` | Star | | `0x15` | Bomb | | `0x24` | Charizard |
| `0x07` | BeamSword | | `0x16` | PowBlock | | `0x25` | Beedrill |
| `0x08` | HomeRunBat | | `0x17` | Bumper (stage) | | `0x26` | Blastoise |
| `0x09` | Fan | | `0x18` | PiranhaPlant | | `0x27` | Chansey |
| `0x0A` | StarRod | | `0x19` | Target | | `0x28` | Starmie |
| `0x0B` | RayGun | | `0x1A` | RTTFBomb | | `0x29` | Hitmonlee |
| `0x0C` | FireFlower | | `0x1B` | Chansey (stage) | | `0x2A` | Koffing |
| `0x0D` | Hammer | | `0x1C` | Electrode | | `0x2B` | Clefairy |
| `0x0E` | MotionSensorBomb | | `0x1D` | Charmander | | `0x2C` | Mew |

Two entries repeat a name at a different value (`Bumper` at both `0x10` and
`0x17`; `Chansey` at both `0x1B` and `0x27`) — that's the source enum's own
structure (a `standard`/`stage`/`pokemon` grouping with some overlap), not a
transcription error here. Remix's `BOWSER_BOMB` is a custom out-of-range
value (`0x011A`) specific to one stage hazard, not part of the normal
`0x00`-`0x2C` span.

### 7.7 `StageHazardUpdate.hazardFlags` bits (schema v3+)

| Bit | Meaning |
|---:|---|
| `0x01` | Whispy Woods is currently blowing (Dream Land only — see §4.7). |

All other bits are currently always `0`, reserved for hazards not yet
tracked (Zebes' rising acid, Duel Zone's disappearing platforms, …).

## 8. Known limitations / not yet implemented

These are deliberate v1 scope cuts, not oversights — later format versions
can add any of them as new event types or appended fields per §5, without
breaking existing files or parsers:

- **Item/weapon tracking has no velocity, damage, owner/attacker port, or
  expiration data.** `ItemUpdate` (§4.6) records a named `kind` (§7.6) and
  position for every live, non-held Item/Weapon object, but none of those
  additional fields have been mapped in memory yet.
- **Stage hazard tracking covers exactly one hazard.** Schema v3's
  `StageHazardUpdate` (§4.7) currently only tracks Whispy Woods' wind on
  Dream Land — other built-in hazards (Zebes' acid, Duel Zone's platforms,
  …) are known to be resolvable the same way (smashremix `docs/ram-map.md`
  §10.5) but haven't been added yet.
- **No RNG/desync-detection event.** No `FrameStart`-equivalent event (RNG
  seed, internal scene frame counter); no known Smash Remix RNG seed address
  has been identified.
- **No aggregate damage-dealt/taken breakdown or incoming-damage-this-hit
  field**, even though the emulator exposes them — deferred as not
  essential for a first version. (A hitbox's own `shieldDamage` *attribute*
  is captured per-slot in `HitboxUpdate`, §4.8 — this is about a different,
  still-missing per-victim aggregate.)
- **Hitbox/hurtbox tracking (§4.8/§4.9) is opt-in and off by default** (the
  Settings dialog's "Include hitbox and hurtbox data" checkbox /
  `SettingsID::GameStats_RecordHitboxData`) precisely because of the
  verbosity below - a typical user gets small files unless they explicitly
  turn it on. **It's also deliberately temporary, not a final design.** It records every active hitbox slot and
  every hurtbox slot, every frame, with no deduplication against action
  state — the plan is to keep doing that only until it's shown that a
  character's geometry is reliably derivable from `(characterId,
  actionStateId, actionFrameCounter)` alone (starting with the original 12
  characters, both versions), at which point recording can stop for that
  character and a lookup/formula can replace it in a future schema. Also:
  no already-hit-target tracking (`attack_records`/`GMAttackRecord`) is
  captured, so two `HitboxUpdate`s can't currently be told apart as "same
  hit, still active" vs. "a new hit"; Item/Weapon hitbox byte offsets are
  hand-derived, not compiler-verified (§4.8's own caveat); and items have
  no live per-instance hurtbox data, only a static per-type template not
  currently read at all.
- **`GameEnd.endReason` cannot currently distinguish time-out from
  stock-out** — both collapse to `1` ("normal end"); only "aborted vs. not"
  is currently derivable from available memory.
- **Character-specific action states (`>= 0x0DC`) have no shared table** —
  meaning is entirely per-character and would need to be derived
  empirically per character if finer-grained special-move detection is
  wanted.
- **Remix-specific stage IDs (`0x29`+) are not enumerated** in §7.2 — would
  need re-deriving from the mod's assembly source.
- **No ROM-identity check.** The recorder does not verify the loaded ROM is
  actually Smash Remix before recording; enabling the feature on a
  different game will produce a `.rmgr` file with garbage bytes (it does not
  crash — the pointer-validity checks in `ReplayMemory.cpp` cause it to stay
  in "waiting for a match" indefinitely rather than write nonsense, in the
  overwhelming majority of cases, but this is not a guarantee).

## 9. Reference implementation

- **Writer:** `Source/RMG-Core/Replay.cpp` / `Source/RMG-Core/Replay.hpp`
  (C++, streamed, single writer instance per emulation session).
- **Memory reader:** `Source/RMG-Core/ReplayMemory.cpp` /
  `Source/RMG-Core/ReplayMemory.hpp` (the N64 RAM pointer-chase that feeds
  the writer above; not part of the file format itself, but the source of
  truth for every field's semantics).
- **TypeScript reader/writer + tests:** [`rmgr-ts/`](../rmgr-ts/) at the
  repository root — a standalone package (no dependency on the C++ build)
  intended to be extracted into its own repository. See
  [`rmgr-ts/README.md`](../rmgr-ts/README.md) for its API.
