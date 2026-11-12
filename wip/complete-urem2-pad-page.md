# urem2: on-screen pad page

Status: Complete

## Description
The macro pad's hardware isn't ready for testing, so give urem2's activities that use the pad an extra page of
on-screen keys doing the pad's job: arrow keys, Enter, Escape and volume up/down.

The pad's keys are 2 rows of 3: top row Escape, Up, Enter; bottom row Left, Down, Right. The on-screen page should
follow that arrangement, plus volume +/- (which the pad does with its encoder).

## Notes
- **Pad bindings** (`urem2/layout/pad.mjs`): controls `key1..key7`, `enc1`. `padBindings(spec)` turns `{ keyN: { press|release|tap|long, repeat } }` into bindings; `padArrows(opsFor)` = key2 up, key4 left, key5 down, key6 right, on press with `ARROW_REPEAT {500,100}`. `volumeBindings(ctx)`: key7 tap = `yamahaToggle(mute)`, enc1 turn = `volumeScript` (`scripts/volume.lua`, reads `param` "delta= interval=").
- Physical map matches the description: key1 Escape, key2 Up, key3 Enter / key4 Left, key5 Down, key6 Right.
- `defineActivity(ctx, name, { pages, pad })` (`activities/activity.mjs`) builds pages via `grid()` (keys.mjs, slot = row*5+col, 0..14) and bindings `[...padBindings(pad), ...volumeBindings(ctx)]`; key7/enc1 reserved.
- Per activity pad use:
  - atv (1 page): key1 tap menu / long menu-hold, key3 tap select / long hold (atvKey), arrows atvKey on press.
  - hdd (3 pages): key1 press `pana.return`, key3 press `pana.ok`, arrows `pana.up` etc.
  - steam (2 pages): key2 voteUp, key4 prev, key5 voteDown, key6 next (slideshow), key1/key3 unbound.
  - playstation (1 page): no pad spec, volume only.
- On-screen keys (`key()`): `tap` + optional `long`; no `press` event for touch keys in keys.mjs. `repeat` is accepted and **dropped** (`withRepeat`: firmware has no repeat gesture yet). So arrow/volume auto-repeat on screen isn't possible today.
- Volume on screen: key taps carry no `param`, so `volumeScript` can't be reused as is; need a fixed step (e.g. httpGet of `target.yamaha + setVolume(dir, 1)`, or the script with a fixed param). Mute: `muteKey(ctx)` exists (keys.mjs).
- Pages: device `SCREEN_MAX_PAGES` 8 (`firmware/main/screen_layout.h`); home + pages per activity well under that (max hdd 3 -> 4).
- Icons (node-buttonImage/images): `#arrow-up/down/left/right`, `#volume-up/down/mute`. No enter/escape icon: use labels ("OK"/"Back"/"Menu") or an SVG in `urem2/icons/`.
- Tests: `activities/*.test.mjs` use `assertSlots(page.keys, {slot: key(...)})` and `cleanLayout(...)` per target; run `node --test "urem2/**/*.test.mjs"`.
- DRY route: one pad spec per activity -> derive both `padBindings` and the on-screen page from it (map control -> slot + icon; take `tap ?? press` as the key's tap, `long` as long).
- Proposed page (slots): 1 Escape, 2 Up, 3 Enter, 6 Left, 7 Down, 8 Right; 4 Vol+, 9 Vol-, 14 Mute (right column, like other pages' mute at 14). Leaves 0/5/10 free.

## Decisions (user, 2026-11-09)
1. **The pad page is the first page** of atv, hdd and steam (their existing pages move along one).
2. **PlayStation gets no pad page** (no pad keys).
3. **Escape and Enter are icons:** a red cross (x style, cancel) for Escape, a green tick for Enter. Where unbound (steam key1/key3) the slot stays empty.
4. **Volume +/-: 1 step per tap.**

## Flagged assumptions
- On-screen arrows and Escape/Enter fire on tap (touch keys have no press event); the pad's long-press holds (atv menu/select) map to the key's `long`.
- Pad page layout as proposed in Notes (slots 1-3 / 6-8 for the pad, 4 Vol+, 9 Vol-, 14 Mute).

Open questions: all answered (Decisions above).

## Plan
Run tests from the repo root: `node --test "urem2/**/*.test.mjs"` (all green after every step).

- [x] 1. **Icons.** Add `urem2/icons/cross.svg` (red X, cancel style) and `urem2/icons/tick.svg` (green check), matching the size/viewBox of the existing icons there; check how `look.mjs` references `urem2/icons/*` and add a test in `look.test.mjs` (or keys.test) that both resolve/render through the same path the netflix/prime icons use.
- [x] 2. **Fixed-step volume ops (test first).** In `pad.test.mjs`/`scripts.test.mjs`, test a helper (e.g. `volumeStep(ctx, dir)` in `pad.mjs`, reusing `volumeScript` with a fixed `param` "delta=±1", or the existing Yamaha setVolume op if simpler) giving 1 step per tap. Implement; no duplicated URL/op building with `volumeBindings`.
- [x] 3. **Pad page from the pad spec (test first).** In `pad.test.mjs`, test `padPage(ctx, spec)`: one table `PAD_SLOTS` (key1 -> slot 1 cross, key2 -> 2 arrow-up, key3 -> 3 tick, key4 -> 6 arrow-left, key5 -> 7 arrow-down, key6 -> 8 arrow-right) used by both `padArrows`/`padBindings` naming and the page; key's `tap` = spec `tap ?? press`, `long` = spec `long`, `repeat` passed through (dropped by `withRepeat`); unbound controls leave the slot empty; fixed keys 4 Vol+, 9 Vol- (step 2), 14 `muteKey(ctx)`. Implement in `pad.mjs` with `grid()`/`key()`.
- [x] 4. **defineActivity option (test first).** In `activity.test.mjs`, test `defineActivity(ctx, name, { pages, pad, padPage: true })` puts `padPage(ctx, pad)` first, then `pages`; without it pages are unchanged (playstation). Bindings still come from the same `pad` spec. Implement in `activities/activity.mjs`.
- [x] 5. **Activities (test first).** Update `atv.test.mjs`, `hdd.test.mjs`, `steam.test.mjs`: page 0 is the pad page (atv: cross tap menu/long menu-hold, tick tap select/long hold; hdd: return/ok, pana arrows; steam: arrows only, slots 1/3 empty), existing page assertions shift by one index. `playstation.test.mjs` unchanged. Enable `padPage` in `atv.mjs`, `hdd.mjs`, `steam.mjs`. Check `build.test.mjs`/`home.test.mjs` for page-count or index assumptions (go-to-page, max pages <= `SCREEN_MAX_PAGES` 8) and fix.
- [x] 6. **Bake check.** Bake both entries with the CLI (`node cli/cli.js bake` on `urem2/layout/urem2.mjs` and `urem2-test.mjs`, outputs under `scratch/`); confirm no errors and the new icons render. `bake.test.mjs` passes.
- [x] 7. **README.** Update `urem2/README.md`: the pad page (first page of atv/hdd/steam, layout, tap-only arrows with no auto-repeat, 1-step volume) and why (pad hardware not ready).
- [x] 8. **Hardware check (user).** Upload the test layout (`node cli/cli.js upload`), against the test server: each activity opens on the pad page; arrows, cross, tick (tap and long on atv) send the expected requests; Vol+/Vol- move 1 step; mute toggles; steam slots 1/3 empty; playstation has no pad page; other pages still work after shifting. Not automatable.

## Implementation notes (2026-11-09)
- `pad.mjs`: one `PAD_KEYS` table (control -> slot, icon, arrow dir) drives both `padArrows` and `padPage(ctx, spec)`
  (returns a slot map; `defineActivity` grids it). `volumeStep(ctx, dir)` = `ops.yamaha(setVolume(dir, 1))` (plain
  httpGet, no script). Arrow icons white, Vol+/- purple like mute; cross red, check green: built-ins `#cross`/`#check` from node-buttonImage (1.1.2), colour in `PAD_KEYS`
  (the first-pass `urem2/icons/cross.svg`/`tick.svg` were replaced and deleted).
- `defineActivity(..., { padPage: true })` prepends the page; atv, hdd, steam set it.
- Tests: 193 pass. Both entries bake clean (`scratch/urem2.bin`, `scratch/urem2-test.bin`, ~150 KB).
- Step 8 (hardware) left for the user.
