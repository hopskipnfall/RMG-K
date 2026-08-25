# Session Handoff — 2026-08-25

Branch: `feature/replay-file-format` (PR [#2](https://github.com/hopskipnfall/RMG-K/pull/2), Linux+Windows CI green)

This doc exists because the previous session ran out of context. It has two parts:
**(1)** a summary of everything that landed this session (committed + uncommitted), and
**(2)** the state of an in-progress `superpowers:brainstorming` session for a new "match stats" feature — **no code has been written for part (2) yet**, it's still in the clarifying-questions phase per the brainstorming skill's hard gate.

---

## Part 1: What's done, and what's NOT committed

### Committed this session (already on `feature/replay-file-format`, pushed, CI green)
- `.rmgr` format bumped to **v3**. See [docs/RMGR_SPEC.md](../RMGR_SPEC.md) — the authoritative spec, keep it in sync with any format change.
  - Header (88 bytes) gained `goodName` (64-byte UTF-8), `recorderSchemaVersion` (u32), `recordedAtEpochSeconds` (u64). Versioning philosophy: `FileHeader.version` (container format), `goodName` (which ROM/mod), `recorderSchemaVersion` (per-goodName field layout) — three independent axes, see spec §3.3.
  - `PostFrameUpdate` event (50 bytes) gained `comboHitCount`/`comboDamage`, sourced directly from Smash Remix's own combo-tracking memory (not a heuristic) — see [Source/RMG-Core/ReplayMemory.hpp](../../Source/RMG-Core/ReplayMemory.hpp)/[.cpp](../../Source/RMG-Core/ReplayMemory.cpp) `PortMatchInfo`, offsets `PORT_COMBO_DAMAGE=0x50`/`PORT_COMBO_HIT_COUNT=0x54`.
  - Recorded filename format changed from krec's compact `YYMMDDHHMMSS` to `YYYYMMDD-HHMMSS` (24h). Logic in [Source/RMG-Core/Replay.cpp](../../Source/RMG-Core/Replay.cpp) `BuildFileName()`/`OpenNewFile()`.
  - Compatibility with pre-v3 files was explicitly NOT preserved (user's call — "nobody is using it yet").
- Per-room "Record replay file" checkboxes (replacing the old global Settings switch), gated on GoodName match, across the netplay lobby dialogs + [KailleraPlaybackDialog](../../Source/RMG/UserInterface/Dialog/Kaillera/KailleraPlaybackDialog.cpp).
- **Linux CI fully fixed** (was broken since `DEBUGGER=1`/`GAME_STATS` was turned on): two real root causes, both fixed —
  1. SDL3's `pkg-config` prefix didn't match its install location → added `-DCMAKE_INSTALL_PREFIX="/usr"` to the SDL3 build step in `.github/workflows/build.yml`.
  2. Forced `-include` flags in `SDL_CFLAGS_FOR_CORE` were leaking into `.S` assembly compilation via the shared `CFLAGS` var, breaking ARM64 → removed the forced includes from [Source/3rdParty/CMakeLists.txt](../../Source/3rdParty/CMakeLists.txt), added a direct `#include <stdio.h>` to the one file that actually needed it (`dbg_breakpoints.c`).

### Uncommitted — export feature (**do not commit until the user explicitly says so** — standing instruction, not yet rescinded)
Headless `.rmgr`-only export (mirrors the existing "Export MP4" feature but skips video/audio), triggered from a new "Export Replay" button in [KailleraPlaybackDialog](../../Source/RMG/UserInterface/Dialog/Kaillera/KailleraPlaybackDialog.cpp) (`.cpp`/`.hpp` both modified).
- New files: [Source/RMG/Utilities/KailleraExport/ReplayFileExport.hpp](../../Source/RMG/Utilities/KailleraExport/ReplayFileExport.hpp)/[.cpp](../../Source/RMG/Utilities/KailleraExport/ReplayFileExport.cpp) (the pipeline, ~390 lines), [ReplayHook.hpp](../../Source/RMG/Utilities/KailleraExport/ReplayHook.hpp)/[.cpp](../../Source/RMG/Utilities/KailleraExport/ReplayHook.cpp) (a `void*`/`bool`/`string`-only isolation boundary — needed because `EmulatorProxy.hpp`'s self-contained mupen64plus type defs collide with RMG-Core's real ones if both are included in one TU).
- Filename-collision avoidance (`FindCollisionFreePath()` in `ReplayFileExport.cpp`) — if two exports would land on the same-second filename, appends `-2`, `-3`, etc.
- [Source/RMG-Core/Replay.hpp](../../Source/RMG-Core/Replay.hpp)/[.cpp](../../Source/RMG-Core/Replay.cpp) gained `SetOutputPathOverride()` (consume-once) to support this.
- [Source/RMG/CMakeLists.txt](../../Source/RMG/CMakeLists.txt) and [Source/RMG/main.cpp](../../Source/RMG/main.cpp) wired up (new CLI dispatch, new source files added to the existing `if (WIN32)` block inside the `NETPLAY` guard).
- This whole subsystem (`Utilities/KailleraExport/`) only compiles on `WIN32` at the CMake level. To locally syntax-check on macOS, there are two throwaway local-only patches used and always reverted afterward — **do not leave these applied**:
  1. [Source/RMG/CMakeLists.txt](../../Source/RMG/CMakeLists.txt) line ~141: temporarily change `if (WIN32)` → `if (TRUE)` around the `KailleraExport` source list.
  2. [Source/n02/common/k_socket.cpp](../../Source/n02/common/k_socket.cpp) line 197: temporarily change `#if !defined(linux)` → `#if defined(_WIN32)` (this one's arguably a real bug worth fixing for real at some point — it's currently backwards for any non-Windows, non-Linux platform, e.g. macOS, but only matters for local test builds since the whole file is Windows-only in shipped builds).
- Verified compiling via full partial CMake builds and `clang++ -fsyntax-only`, functionally not yet run end-to-end (no Windows box in this session).

### Untracked at repo root — **just sample data, safe to ignore/gitignore**
`20260825-1{05731,10022,10209,10422,10616,10824}-Wario-Player.rmgr` — real v3-format sample replays the user added for testing. `.DS_Store` and `.claude/` are also untracked, unrelated noise.

### Uncommitted — `rmgr-ts` / `rmgr-viewer` (**standing instruction: never commit without explicit permission** — repeated several times this session)
- `rmgr-ts/` — TypeScript parser/serializer for `.rmgr`, kept in sync with the v3 format (32/32 tests passing): `readU64`/`writeU64`/`readFixedUtf8String` in `binary.ts`, `FORMAT_VERSION=3`/`HEADER_SIZE=88` in `constants.ts`, strict version-check parsing in `parse.ts` (size-bounded `PostFrameUpdate` reads, an actual bug fix — it previously had no bounds-checking at all, unlike `GameStart`'s already-correct pattern), full serialization in `serialize.ts`.
- `rmgr-viewer/` — canvas-based replay visualizer:
  - [rmgr-viewer/src/stageGeometry.ts](../../rmgr-viewer/src/stageGeometry.ts) — **empirically-derived** Dream Land geometry (scanned real replays for the "Teeter" action state to find platform/ground edge X coordinates exactly). Ground edges at **X = ±2278**. Side platforms: left X -1801..-991 @ Y=904, right X 991..1852 @ Y=907. Top platform X -530..530 @ Y=1542.
  - [rmgr-viewer/src/characterSizes.ts](../../rmgr-viewer/src/characterSizes.ts) — per-character marker sizing tuned to match real proportions, JP-region variants alias their base character's size.
  - [rmgr-viewer/src/neutralHits.ts](../../rmgr-viewer/src/neutralHits.ts) — first actual "match stat": per-frame running count of neutral hits landed this stock, per port. Detects a "fresh hit" via `comboHitCount` transitioning from 0 to >0, resets the counter when `stocksRemaining` decrements. Wired into `main.ts`'s per-player panel.
  - `main.ts`'s `loadDefault()` now loads `/replays/20260825-105731-Wario-Player.rmgr` as the default sample (old v1 sample was deleted, can no longer parse against the v3-only parser).
  - Camera/renderer got hover-world-coordinate display; the old per-character position label was removed as redundant.

---

## Part 2: In-progress brainstorm — match/session stats analysis

**Status: `superpowers:brainstorming` skill is active. HARD GATE: no code, no spec file yet — still in the "ask clarifying questions one at a time" phase.** Do not skip ahead to implementation or even to drafting the design doc until the open question below is resolved and the user has had a chance to sanity-check the full definition.

### The ask
User wants to load a `.rmgr` file and see stats about that match (e.g. "average neutral hits required to take a stock" — already prototyped, see `neutralHits.ts` above). Longer-term: persist these stats to a database, see trends over time and across character matchups. A "session" (multiple matches) may eventually be a first-class concept, but today one `.rmgr` file = one match, and there's no session concept in the format yet.

Earlier in the brainstorm I laid out a rough tiering of what's buildable now vs. blocked:
- **Buildable now**: anything derivable from a single match's per-frame data as already recorded (neutral hits/stock is the first example).
- **Needs an action-state taxonomy**: classifying "what kind of hit/situation is this" beyond raw `actionStateId` numbers.
- **Blocked on attacker-port attribution**: we don't currently record *who* landed a hit, only that a hit landed on a given port. Anything needing "who did this to whom" is blocked until/unless that's added to the format. (Good news: the edge-guard/recovery feature below turns out NOT to need this — see below.)
- **Needs a "session" concept**: cross-match trends, matchup history — not buildable until the format/DB layer supports grouping multiple `.rmgr` files.

### Current concrete idea: edge-guard / recovery stat (Dream Land only, 2-player only for now)

**Geometry** (user-proposed, on Dream Land): draw a line from `(2916, 58)` to `(3570, 4158)`, mirror it across the stage center to get a matching line on the left (`(-2916, 58)` to `(-3570, 4158)`). This is NOT a flat vertical wall — the X threshold widens as Y increases (2916 near ground level, out to 3570 high up), which lines up with recovery intuition: a flat, low knockback is dangerous almost immediately past the stage edge, while a high vertical launch has more room before it's genuinely a "situation." Note the boundary sits well outside the stage's own edges (ground edge is X=±2278, right platform edge is 1852) — you can walk/dash past the physical edge without it counting as a situation yet.

I flagged one open item I haven't gotten an answer on: the given points' Y range (58 to 4158) only defines a **side** boundary — there's no stated top or bottom bound, so a straight-down spike (large negative Y, small X change) might never cross this line at all even though it's a classic kill/edge-guard scenario. Not yet discussed with the user — worth raising before finalizing the geometry.

**"Situation" ownership model** (inferred from the user's own chase-scenario example, stated back to them and NOT yet explicitly confirmed as correct, though it's the only reading consistent with their example): this is a single shared state per player-pair, not independent per-player "am I outside the zone" flags. Whoever leaves the zone first (from a neutral state) claims the "recovering" role; the other player is the "edge-guarding" role. If the edge-guarder *also* steps outside the zone later (to chase), that does NOT start a second, separate situation for them — they're just the aggressor inside the existing one. This was the user's explicit correction to a naive per-player-independent model: "if player 1 goes out... and then player 2 goes out too to prevent them from recovering, that does not count as a recovery situation for player 2."

**Resolution condition — user's latest refinement, and there's an internal contradiction to resolve before implementing:**

> "Once an edge guard/recovery situation begins, the edge guard is considered successful (and therefore the recovery is considered unsuccessful) if the character is able to either grab ledge OR they are able to land on the ground and be in a state where they can act for 0.5s. If the stock is lost before reaching that same condition, that is a successful edge guard and a failed recovery."

As literally written, **both sentences describe "successful edge guard"** — the first ties it to grabbing ledge / landing-and-actionable-for-0.5s, the second ties it to losing the stock. Those can't both be true; this reads like a wording slip (typing "edge guard" where "recovery" was meant in the first sentence). The far more likely intended meaning, consistent with the whole conversation up to this point:
- **Successful recovery / failed edge guard**: the recovering player either grabs the ledge, or returns to the ground and is actionable (not in hitstun/tumble) for a continuous 0.5s.
- **Successful edge guard / failed recovery**: the recovering player loses the stock before achieving either of those.

This is a much smaller ask than the original 1-second version discussed earlier in the session — note the threshold also dropped from **1 second to 0.5 seconds** in this latest message; worth confirming that change was intentional and not just restating the number loosely.

**Next step**: confirm with the user which reading of the resolution condition is correct (I'd bet on the swap above, but don't assume — ask directly) before drafting anything. After that, remaining open items to work through per the brainstorming skill's one-at-a-time process:
1. The top/bottom-of-zone gap noted above (straight-down kills).
2. Whether getting hit again *during* the 0.5s actionable window resets the clock (situation continues) or ends it immediately as a failure — this was asked in the previous turn and got superseded by the user's redefinition above; re-ask once the contradiction is resolved, since the new "actionable for 0.5s" framing may already imply an answer (a hit that puts you back in hitstun makes you non-actionable, which naturally resets the clock without needing a separate rule — worth confirming this reading too).
3. The explicitly-acknowledged 2-player-only limitation — user's own words: "this doesn't really work in a situation when there are more than 2 players on the screen though." Likely just scope this out explicitly (N/A for 3+ players) rather than solving it now, but that's a decision for the design doc, not yet made.

**What this stat needs from existing data** (confirmed no new recorded fields needed, unlike the neutral-hits/attacker-attribution tier):
- Position (X/Y) per frame per port — already recorded, for the zone-crossing check.
- `groundedState` — for "landed."
- `hitstunCounter` (or equivalent actionable/hitstun state) — for "actionable."
- `actionStateId` transitioning into `CliffCatch`/`CliffWait`/`CliffQuick`/`CliffSlow` (already in the lookup table per `docs/RMGR_SPEC.md`) — for "grabbed ledge."
- `stocksRemaining` decrementing — for "stock lost." Notably this does NOT require attacker-port attribution; "did they die while the situation was open" is sufficient to score it against whichever player owns the situation's "edge-guarding" role.

### Brainstorming skill process reminder for whoever picks this up
Per `superpowers:brainstorming`: ask one question at a time, only ONE per message; once the idea is understood, propose 2-3 approaches with tradeoffs; present the design in sections with approval after each; then write the spec to `docs/superpowers/specs/YYYY-MM-DD-<topic>-design.md` and commit it; self-review for placeholders/contradictions/scope/ambiguity; have the user review the committed spec; only then invoke `writing-plans`. **Do not invoke any implementation skill or write code before that final step.**

---

## Other standing instructions still in force
- Don't commit the export-button/collision C++ work (Part 1's "uncommitted" section above) until the user explicitly says so.
- Don't commit anything under `rmgr-ts/` or `rmgr-viewer/` without explicit permission.
- No "🤖 Generated with Claude Code" attribution in commits/PRs (user preference, saved to memory).
