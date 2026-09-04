# `.rmgr` v2 — Format Redesign for Real-User Rollout

**Status:** Design, approved by user 2026-09-04 (revised same day: `version`
keeps incrementing rather than resetting). Spec doc (`docs/RMGR_SPEC.md`)
rewritten to match; code changes not yet implemented.

**Supersedes:** `docs/RMGR_SPEC.md` (current format version `4`, recorder schema
history up to `9`). This is an intentional, total break — old `.rmgr` files
are not expected to remain readable. The `version` counter does **not**
reset, though: it continues incrementing (`4` → `5`), specifically so an old
reader (or a human staring at a hex dump) sees an unfamiliar version number
and knows unambiguously "this isn't a format I understand" instead of the
number colliding in value with an unrelated earlier format.

## 1. Why

The format has been in prototype/experimentation mode: hitbox/hurtbox
tracking never worked, schema versions accumulated a long trail of
bug-fix history, and everything assumes exactly one game (`SmashRemix2.0.1`).
Before rolling `.rmgr` recording out to real users, six problems need
fixing:

1. Files average ~1MB/game and are almost entirely redundant frame-to-frame
   data (gzip alone gets them to ~1/3 size).
2. No clean way to add/remove fields per game-version without growing
   unrelated games' files.
3. No clean way to support a second, structurally different game (e.g.
   Mario Kart 64) without it inheriting Smash-only fields like damage/stocks.
4. Some spec language (historical bug narration, byte-level rationale) is
   dead weight now that this is a from-scratch rewrite.
5. The format should double as a game-agnostic input recording (like
   `.krec`), with per-game analysis data layered on top only when the
   recorder recognizes the loaded ROM.
6. Hitbox/hurtbox recording (§4.8/§4.9 of the old spec) was a failed
   experiment and should be removed outright.

## 2. Key decisions

Decided via user Q&A during brainstorming — not re-litigated here, just
recorded:

- **Compression:** whole event stream (not the header) wrapped in
  zlib/deflate, max compression level, one-shot (not streaming). No bespoke
  delta/bit-packing encoding — deflate already captures frame-to-frame
  redundancy well, and a hand-rolled scheme was explicitly not wanted.
- **Buffering:** the recorder holds the entire match's events in memory and
  writes the file once, at match end. **A crash mid-match now produces no
  file at all** (previously: a truncated-but-parseable file) — an accepted
  trade-off, not an oversight. This removes the old "write `streamLength =
  0`, seek back and patch it later" mechanism entirely: every header field
  is known before the first byte is written.
- **Header stays uncompressed**, fixed size, so a file browser/tool can read
  identity/timestamp fields without decompressing the (much larger) event
  stream.
- **New `gameFamily` field** (short NUL-padded ASCII string, e.g.
  `"smash64"`) in the header, orthogonal to `goodName`. `goodName` still
  identifies the exact ROM build; `gameFamily` tells a reader which
  extension-event *definitions* apply, without needing an ever-growing
  goodName-to-game lookup table maintained independently in every
  downstream tool.
- **Core vs. game-family-extension event split.** A small, fixed set of
  "core" events (player names/slots, controller inputs, match end/frame
  count) is always recorded for any N64 ROM, recognized or not — this is
  the krec-equivalent layer. A second, game-family-specific set of
  "extension" events (position, damage, stocks, items, hazards, …) is only
  defined/recorded when the loaded ROM's family is recognized. Recording
  now arms unconditionally (previously gated entirely on a hardcoded
  supported-ROM check); an unrecognized ROM still yields a valid, useful
  input-only recording.
- **`recorderSchemaVersion` stays scoped per-`goodName`**, unchanged in
  concept. This is deliberately how a shared `gameFamily` (e.g. `smash64`)
  accommodates ROM variants with different field sets — e.g. Smash Remix's
  extra settings (teams, handicap, CPU level, item frequency, wider
  character-ID range) that vanilla SSB64 doesn't have are just later-schema
  field-appends on the shared `smash64` extension events, using the same
  append-only mechanism the format already has. A vanilla-only reader
  never advances far enough in schema version to see Remix-only trailing
  fields; the per-file `EventPayloads` declared size means it never
  misreads them either.
- **Hitbox/hurtbox removed entirely**, no replacement, no migration path.
  Revisit as a fresh design later if wanted.

## 3. Header layout (112 bytes, uncompressed)

| Offset | Size | Type       | Field                    | Notes |
|-------:|-----:|------------|---------------------------|-------|
| 0x00   | 4    | `char[4]`  | `magic`                   | `R`,`M`,`G`,`R`, unchanged. |
| 0x04   | 1    | `u8`       | `version`                 | `5` — continues incrementing from the old spec's `4`, not reset. An old reader (or a human inspecting a file) sees a version number it has never seen before and correctly treats the file as unparseable, rather than the value colliding with an unrelated earlier format. |
| 0x05   | 3    | `u8[3]`    | `reserved`                | Always zero. |
| 0x08   | 16   | `char[16]` | `gameFamily`              | NUL-padded ASCII, e.g. `"smash64"`. Empty (all zero) if the loaded ROM wasn't recognized — core events are still valid in that case. |
| 0x18   | 64   | `char[64]` | `goodName`                | Unchanged from v1: exact ROM build identity, NUL-padded, truncated if longer. |
| 0x58   | 4    | `u32`      | `recorderSchemaVersion`   | Unchanged concept: scoped per-`goodName`. `0` if `gameFamily` is empty (no extension schema applies). |
| 0x5C   | 8    | `u64`      | `recordedAtEpochMillis`   | Unchanged. |
| 0x64   | 4    | `u32`      | `recordedAtNanosOffset`   | Unchanged. |
| 0x68   | 4    | `u32`      | `uncompressedLength`      | **New.** Byte length of the raw (decompressed) event stream — lets a reader preallocate its output buffer. |
| 0x6C   | 4    | `u32`      | `compressedLength`        | **New, replaces `streamLength`.** Byte length of the deflate blob immediately following the header. Always correct on disk — no more "0 until finalized" convention, since nothing is written until the match ends. |

Total: 0x70 = 112 bytes (up from 92).

## 4. Event stream (inside the deflate blob)

Same self-describing mechanism as before (`EventPayloads` first, declaring
every other event's payload size; unrecognized codes are skipped by
declared size) — that mechanism was already game-agnostic and needs no
change. What changes is which events exist and which layer they belong to.

**Core (always present, any recognized-or-not N64 ROM):**

| Code | Event         | Payload | Notes |
|-----:|---------------|--------:|-------|
| 0x01 | `EventPayloads` | variable | Unchanged mechanism. |
| 0x02 | `MatchStart`  | 132 B   | `playerNames` (4×32B, unchanged) + `slotType[4]` (u8 each: 0 human/1 CPU/2 empty). **No longer carries** stage/character/costume/team/stock/damage/items — those moved to `MatchSettings` (family-specific, below). |
| 0x03 | `InputFrame`  | 9 B     | Same fields as old `PreFrameUpdate` (frame, port, buttons, stickX, stickY), renamed for clarity — this event no longer has a Smash-specific "Pre" counterpart baked into its name. |
| 0x05 | `MatchEnd`    | 5 B     | `finalFrame` (i32) + `endReason` (u8, same 0=aborted/1=normal meaning as old `GameEnd`). **No longer carries** `placements` — moved to `MatchResult` (family-specific, below), since "stocks remaining" is a Smash concept, not a universal one. |

**`smash64` family extension (present only when `gameFamily == "smash64"`):**

| Code | Event            | Payload | Notes |
|-----:|-------------------|--------:|-------|
| 0x04 | `StateFrame`      | 50 B    | Same fields as old `PostFrameUpdate`, unchanged byte-for-byte — just recategorized as family-specific and renamed. Written immediately after each port's `InputFrame`, same as today. |
| 0x06 | `ItemUpdate`      | 25 B    | Unchanged from old spec. |
| 0x07 | `StageHazardUpdate` | 5 B  | Unchanged from old spec. |
| 0x08 | `MatchSettings`   | 32 B    | **New**, split out of old `GameStart`: `stageId, gameType, stockCountSetting, timeLimitMinutes, damageRatio, itemFrequency, teamsEnabled, handicapMode` (1B each, 8B total) + per-port `characterId[4], costumeId[4], teamColor[4], portTeam[4], portHandicap[4], portCpuLevel[4]` (4B each array, 24B total). Written once, immediately after `MatchStart`. |
| 0x09 | `MatchResult`     | 4 B     | **New**, split out of old `GameEnd`: `placements[4]` (i8 each, stocks remaining, `-1` = never seated). Written once, immediately after `MatchEnd`. |

0x08/0x09 reuse the code space vacated by removing `HitboxUpdate`/
`HurtboxUpdate` rather than leaving a gap.

**Stream order**, mirroring today's structure: `EventPayloads` →
`MatchStart` → `MatchSettings` (if `smash64`) → per real frame:
[`InputFrame`×seated ports → `StateFrame`×seated ports (if `smash64`) →
`ItemUpdate`×live objects (if `smash64`) → `StageHazardUpdate`×0-1 (if
`smash64`)] → `MatchEnd` → `MatchResult` (if `smash64`).

## 5. Space efficiency, expected impact

Per-match payload sizes drop slightly even before compression (e.g.
`MatchSettings` is 32B vs. old `GameStart`'s 164B, since player names moved
to the always-present core `MatchStart` and everything else got tighter),
but the real win is compression: wrapping the full event stream in deflate
at max level should meet or beat the ~3x reduction already measured with
default-settings gzip. No further action needed here unless real-world
testing after implementation shows deflate isn't sufficient — bespoke
encoding was explicitly ruled out for this pass.

## 6. Spec-language trims

Once implemented, `docs/RMGR_SPEC.md` gets rewritten, not patched. Cut
during that rewrite:
- All of old §5's schema-history narration (schema `2` through `9` bug
  fixes) — describes a numbering scheme that no longer exists.
- §4.8/§4.9 (`HitboxUpdate`/`HurtboxUpdate`) and every §8 "known
  limitations" bullet that only existed to caveat them.
- The `streamLength`/seek-and-patch crash-safety explanation in old §3.1,
  replaced by §3 above (buffered, single-pass write).
- Old §2's "Streamed, not buffered" design-goal bullet, replaced with the
  buffered-then-compressed rationale from §2 above.

Keep: the general self-describing-event-stream rationale, the
little-endian/native-struct-layout goals, the `goodName`/
`recorderSchemaVersion` two-axis explanation (extended with `gameFamily` as
a third axis), the filename convention (§3.4 of the old spec, unaffected by
any of this).

## 7. Open items for the implementation plan (not blocking this design)

- `IsSupportedGame()` (`Replay.cpp`) currently gates *all* recording on a
  single hardcoded `goodName`. Needs restructuring into: always arm core
  recording; separately resolve `gameFamily` (and whether extension
  recording is possible) from the loaded ROM.
- `ReplayMemory.cpp` has zero per-game dispatch today — everything is
  flat, Smash-specific functions. The `smash64` extension reader becomes
  "the existing `ReplayMemory.cpp` code, invoked only when `gameFamily ==
  smash64`"; no dispatch table is needed yet since there's still only one
  family, but the code should be organized so adding a second family later
  doesn't require re-touching Smash's reading code.
- Three repos need coordinated changes: `RMG-K` (writer, `Replay.cpp`/
  `ReplayMemory.cpp`), `rmgr-ts` (reader/types), `rmgr-viewer` (consumes
  `rmgr-ts`, will need updates wherever it reads old field names/locations,
  e.g. anything reading `GameStart`'s now-moved settings fields).
- The `SettingsID::GameStats_RecordHitboxData` checkbox and its Settings
  dialog entry should be removed as part of implementation, not just the
  spec section.
- Filename convention (old §3.4) is unaffected structurally, but should be
  re-verified against the new player-name/port-slot source (`MatchStart`)
  once implemented.
