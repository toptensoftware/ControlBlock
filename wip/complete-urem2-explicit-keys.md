# urem2: explicit keys

Status: Complete

## Description
The urem2 layout (`urem2/layout/`) has been factored so far that it's hard to edit: keys come out of group
helpers rather than being written where they appear, so moving a key means changing a helper's table. Roll back the
group helpers and have every page list each of its keys explicitly, with its position, so keys can be rearranged
(and resized: spans, half keys) just by editing the page. Keep helpers only for defining a **single** key (its look
and ops), e.g. `key()`, `irKey`, `num`, `ch`, `activityKey`, `sevenChKey`, `muteKey`.

Helpers to roll back (at least):
- `transportKeys` (`keys.mjs`, the `TRANSPORT` slot table): atv, hdd and steam list their transport keys themselves.
- `defineActivity`'s pad handling (`activities/activity.mjs`): `padPage: true` generating the on-screen pad page from
  the pad spec (`padPage` and the `PAD_KEYS` slot table in `pad.mjs`); each activity lists its pad page's keys
  explicitly.
- `grid()` slot maps (`{ slot: key }`, index = row * 5 + col): pages become key lists with explicit positions, the way
  the user has started doing in `home.mjs` (`{ ...look, x, y, w, h }`, uncommitted).

The user's request: "instead of the define activity 'pad' stuff and the 'transportKeys' helpers, keep each button
explicitly declared and just provide helpers for defining a single key."

## Notes
- **Uncommitted user edits** at the start of the job: `urem2/layout/home.mjs` (Home rewritten as an explicit key list
  with `x, y, w, h`: power 1U at (0,0), four 2x1.5 activity keys; the app, DVR and 7ch keys commented out) and
  `urem2/layout/pad.mjs` (`PAD_KEYS` slots moved down a row: 6-8, 11-13). Also untracked `urem2/urem2-test-pages/`
  and `firmware/scratch_edit.py`. Don't overwrite these; build on them.
- Tests to adjust first (TDD): `keys.test.mjs` (transportKeys, grid), `pad.test.mjs` (padPage, padArrows),
  `activities/*.test.mjs` (they check slots via `test/slots.mjs` `assertSlots` against `grid` indexes),
  `home.test.mjs`, `build.test.mjs`, `bake.test.mjs`. Run from the repo root:
  `node --test "urem2/**/*.test.mjs"`.
### Investigation findings (2026-11-13)

**Current files and what builds keys**
- `keys.mjs`: `withRepeat`/`checkRepeat` (urem's `repeat` check), `key(look, ops, { long, repeat })` (single key:
  keep), `grid(slots)` (slot map -> 15-entry array: remove), `TRANSPORT` + `transportKeys(opsByName, { repeat })`
  (group: remove), `sevenChKey`, `muteKey` (single: keep).
- `pad.mjs`: `PAD_KEYS` (the pad page's slot/icon/colour/dir table, now 6-8 / 11-13 after the user's edit),
  `PAD_CONTROLS`, `padEntries` (checks), `padBindings(spec)`, `padControls(spec)` (the activity's `pad` repeat map),
  `volumeBindings(ctx)` (key7 mute + enc1 volume script), `ARROW_REPEAT`, `padArrows(opsFor)` (group: key2/4/5/6),
  `volumeStep(ctx, dir)`, `padPage(ctx, spec)` (group: builds the on-screen pad page from the spec plus Vol+ (4),
  Vol- (9), mute (14)).
- `activities/activity.mjs` `defineActivity(ctx, name, { pages, pad, padPage })`: meta (`activityMeta`), checks
  (at least one page, key7/enc1 reserved for the volume), pages = optional `padPage` + slot maps through `grid`,
  `bindings: [...padBindings(pad), ...volumeBindings(ctx)]`, `pad: padControls(pad)`.
- Activities: `atv` (pad page + 1 page; `transportKeys` with `stop: menu`, 7ch 10, mute 14; pad `tapHold` key1/key3 +
  `padArrows`), `hdd` (pad page + 3 pages; transport, numbers, colour keys; pad all on press), `steam` (pad page +
  2 pages; transport without stop, its own slot 13; pad key2/4/5/6 only), `playstation` (1 page: 7ch 10, mute 14).
- `home.mjs` (user's uncommitted edit) already shows the target style: `{ ...activityKey(...), x, y, w, h }` in a plain
  `keys` array, no `grid`. `grid` and `key` are still imported but `grid` is unused there.

**Bake key format** (`docs/layout-format.md`, Keys): a page's `keys` is an array; entry `i` defaults to slot `i`
unless it gives `x` and `y` (both or neither; multiples of 0.25); `w`/`h` default 1. So an explicit list is just
`[{ ...key, x, y }, ...]` in any order, with no `null` padding. **Ids:** a key whose corner is on a whole unit gets
the slot's plain id (`page * 15 + slot + 1`); any other key gets the lowest free id above `pages * 15`, which changes
when pages are added. urem2 binds touch keys only through the key's own `tap`/`long` (generated `touch[id]`
bindings), and nothing outside the layout names a urem2 key id, so default ids are fine; no named `id`s needed.
The bake and `examples/bake-check.mjs` already enforce the grid, overlaps and the 32-key limit.

**Tests now: 16 of 198 fail, all from the user's uncommitted edits** (`node --test "urem2/**/*.test.mjs"`):
- Home's slot test, its long-press-indexes test and "resolves clean" (expects 12 keys), x2 targets.
- `pad.test.mjs` `padPage` (2) and every activity's "pad page" test (atv, hdd, steam x2): pinned to slots 1-3 / 6-8.
- `bake.test.mjs` (x2): asserts every urem2 key is a 1U slot key with its slot id. Home's 2x1.5 keys break it.
This is the cost of pinning positions in tests: every rearrangement means test edits. Tests that pin positions:
`test/slots.mjs` `assertSlots` (Home, atv, hdd, steam, playstation), the pad page tests, and `bake.test.mjs`'s
1U assertion; `keys.test.mjs` tests `grid` and `transportKeys` directly.

**Other references to update:** `urem2/README.md` (line 24 lists `grid()` and the transport row; lines 89-91 say every
key is a plain 1U slot key with no spans; lines ~112-114 describe `padPage()` deriving the pad page from the pad
spec). `CLAUDE.md` (urem2 entry, "urem2 pages with holes" open item) and the `wip/complete-key-render-redesign.md`
holes list mention the 1U slot grid: propose a CLAUDE.md update to the user (Rule 4), don't touch the complete doc.

### Proposed shape (for the Plan)
- **Pages** are explicit key arrays: `{ keys: [ { ...key(...), x: 1, y: 0 }, ... ] }`, each key with its own `x`, `y`
  (and `w`, `h` when not 1U), as in the user's `home.mjs`. Every single-key helper returns a positionless key and the
  page adds the position by spread. Same for Home.
- **Single-key helpers kept/added:** `key`, `withRepeat`, `sevenChKey`, `muteKey`, plus `volUpKey(ctx)` /
  `volDownKey(ctx)` (the pad page's Vol+/Vol-, from `volumeStep`); per-activity local helpers (`irKey`, `num`, `ch`,
  `pc`, `toggle`, `tapHold`, Home's `activityKey`/`appKey`/`dvrKey`) stay, since they each make one key.
- **Removed:** `grid`, `TRANSPORT`/`transportKeys`, `PAD_KEYS`, `padPage`, `padArrows`, `test/slots.mjs`
  `assertSlots`.
- **On-screen pad page:** each of atv, hdd and steam writes its pad page's keys out (cross, up, check, left, down,
  right, Vol+, Vol-, mute) like any other page. The physical pad spec and the pad page then name the same ops twice;
  an activity can hold them in locals (`const menu = k("menu")`) to keep that short.
- **Physical pad bindings:** keep the per-activity `pad: { key1: {...}, ... }` spec (`padBindings`, `padControls`,
  `padEntries`' checks): it's already one entry per control and isn't about screen layout. Only `padArrows` goes:
  each activity writes key2/4/5/6 with `ARROW_REPEAT` itself.
- **`defineActivity`** keeps meta, the checks, `bindings: [...padBindings(pad), ...volumeBindings(ctx)]` and
  `pad: padControls(pad)`; drops `padPage` and `grid` (pages are passed through as `{ keys }`, or as key arrays it
  wraps). The volume stays automatic: it's identical in every activity and has no screen position.
- **Tests:** stop pinning positions. Activity/Home tests check content, not place: the page count, that each page has
  the expected keys (found by look/op, order-free), the bindings and `pad` map (unchanged), resolve clean. Geometry is
  left to the bake (`bake.test.mjs` keeps the full bake with `bake-check`'s device checks, but drops the "every key is
  a 1U slot key" assertion). A small shared test helper (e.g. `assertKeys(keys, expected)` comparing with `x/y/w/h`
  stripped, as multisets) replaces `assertSlots`.

### Implementation notes (2026-11-13)
- Done as proposed, in one pass (small, similar rewires). `node --test "urem2/**/*.test.mjs"`: 193 pass, 0 fail
  (was 16 failing from the user's edits); `node --test "examples/**/*.test.mjs"`: 35 pass. Both urem2 entries bake
  clean (~430 KB, 114 images, 11 pages).
- `keys.mjs` now also holds `volumeStep`, `volUpKey`, `volDownKey` (moved from `pad.mjs`, which imports from
  `keys.mjs`, so they couldn't live there without a cycle). `pad.mjs` is bindings only.
- `defineActivity(ctx, name, { pages, pad })`: `pages` is a list of key arrays; unknown options (e.g. the old
  `padPage`) throw, and a page that isn't an array (an old slot map) throws.
- Positions kept exactly as before (slot s -> `x: s % 5, y: floor(s / 5)`), pad page on the user's new rows. Keys
  in each page are listed in reading order.
- `test/slots.mjs` removed (git rm); `test/pages.mjs` has `assertKeys` and `layoutWith`, with its own tests
  (`test/pages.test.mjs`).
- Home: only the `grid` import dropped. Its test now checks the user's current five keys; the old app-bundle check
  went with the commented-out app keys (the bundles are still covered by the ops tests).
- `urem2/README.md` updated (file list, key map, Home, pad page). Proposed for CLAUDE.md, not applied (Rule 4): see
  the summary to the user.

## Flagged assumptions
- Physical pad bindings stay as the per-activity `pad` spec (only `padArrows` is rolled back); the volume bindings stay
  automatic in `defineActivity`. The user's request named "the define activity pad stuff", read here as the generated
  on-screen pad page (`padPage: true`).
- Positions are written as `{ ...helper(...), x, y, w, h }` (the user's `home.mjs` style) rather than passed into
  `key()`, so every helper (including `activityKey`, which isn't built on `key()`) places the same way.
- Tests stop pinning key positions (order-free content checks instead), so rearranging a page needs no test edits.
  If the user wants positions pinned, keep `assertSlots`-style checks on `x/y` instead.
- Each activity's existing layout is kept as it is now (same slots, written as `x`/`y`); the pad page uses the user's
  new rows (6-8 / 11-13 = y 1 and 2, x 1-3). Home keeps the user's uncommitted edit as is (commented-out keys left
  for the user).
- Default (generated) key ids are fine for urem2; no named `id`s.

## Plan
Slot s of the old grid is `x: s % 5, y: Math.floor(s / 5)`. Test command (repo root): `node --test "urem2/**/*.test.mjs"`.

- [x] 1. Test helper: replace `urem2/test/slots.mjs` with `urem2/test/pages.mjs`: `assertKeys(keys, expected)`
      (order-free: each key compared with `x`/`y`/`w`/`h` stripped, as multisets; no `null` entries allowed) and
      `layoutWith` (moved unchanged).
- [x] 2. `keys.mjs`: tests first (drop the `grid` and `transportKeys` tests; add `volUpKey`/`volDownKey` and
      `volumeStep`, moved from `pad.mjs`); remove `grid`, `TRANSPORT`, `transportKeys`; add `volumeStep`,
      `volUpKey(ctx)`, `volDownKey(ctx)`.
- [x] 3. `pad.mjs`: tests first (drop `padArrows`, `padPage`, `volumeStep` tests); remove `PAD_KEYS`, `padArrows`,
      `padPage`, `volumeStep`. Keep `padBindings`, `padControls`, `volumeBindings`, `ARROW_REPEAT`, `PAD_CONTROLS`.
- [x] 4. `defineActivity`: tests first; `pages` is a list of key arrays (each wrapped as `{ keys }`, each must be an
      array); drop `padPage` and `grid`.
- [x] 5. Activities (atv, hdd, steam, playstation): every page written out as `{ ...key, x, y }`, the pad page
      included (the user's rows: cross/up/tick at y 1, arrows at y 2, x 1-3; Vol+/Vol-/mute down column 4), pad arrows
      written per control. Tests rewritten with `assertKeys` (content, not place) plus bindings, `pad` and resolve.
- [x] 6. Home: drop the `grid` import (the user's edit is otherwise kept); tests to its current keys with
      `assertKeys`.
- [x] 7. `bake.test.mjs`: drop the "every key a 1U slot key" assertion (keep the full bake and the device checks,
      and both images on every key).
- [x] 8. `urem2/README.md` (file list, key map, pad page); propose the `CLAUDE.md` wording to the user.
