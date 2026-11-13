# Lua hardware check setup

Status: Implement (setup done; waiting for the user's hardware run. Kept as wip so the checklist can be ticked)

## Description
Set up the hardware checks for the Lua `script` op (job `wip/complete-lua-scripts.md`, CLAUDE.md Open Items), for the
user to run later:
1. A `RUN_STRESS` case with Lua churning its PSRAM heap, to show scripts don't make the bounce fill late.
2. A test layout with scripts for the remaining checks: op task stack, the `http.get` reply path (timeout, takeover
   while waiting), button-to-request latency, key-event latency while a script holds the lock, plus the limits.

## Notes
- `RUN_STRESS` runs in `app_main` (core 0, 3.5 KB stack): too small for Lua, so the stress script runs on its own
  core 0 task (12 KB stack, priority 3), like the op task. It runs a churn script back to back for ~18 ms, then
  sleeps a tick, while the stress case runs.
- The Lua heap's PSRAM realloc is shared by the op task and the stress run: `standalone_script_platform()` in
  `standalone_task.h` (one copy).
- The op task's "op queue empty" log line gains the script stats when scripts ran: runs, errors, heap peak, and the op
  task's stack high-water mark (check 2).
- Device-only code: not host testable (Rule 3 warning). The test layout is checked by baking it (Lua syntax check).

### Stress run A, first results (2026-11-07, release, COM4)
- Lua cases: 0 over budget, but fill max 391 µs (at rest), 530 µs (mid-swipe), 544 µs (lua + keys) against a 615 µs
  budget (previous worst 412 µs). Without Lua the same build gave 114 / 179 / 283 µs.
- `lua:` lines: 22 runs / 1 error at rest, then 21 runs / 21 errors in each mid-swipe case; heap peak 255 KB (the
  256 KB cap), 19 allocs refused in total. Errors are `not enough memory` and `script ran too long without waiting`.
- Diagnosis: the churn script is much heavier than intended. One run takes ~90 ms at rest (22 runs in 2 s), not the
  planned ~18 ms busy period, and over 100 ms mid-swipe (the fill competes for PSRAM), so it hits the slice. Its live
  set (800 strings of ~45 bytes plus the table, a ~37 KB concat that the buffer doubles and then copies, and the `u`
  table) sits near the cap. These failures come from the test script. They don't show a firmware fault.
- **Every case logged `keys 0, busy 0%`:** the compositor drew no keys in any case, so none of the numbers include
  compositor load. Not yet explained. Checked: init order unchanged, `test_keys.bin` unchanged, stats counting looks
  right. Suspects: `SetKeys` returning `NotFound` (results are ignored in `set_demo_layout`/`change_icons`), or an
  interaction with the stored layout. Earlier stress logs in the wip docs don't record key counts, so it's unknown
  when this started.
- The CH343P port drops at backlight-on and the stress run holds touch/USB/Wi-Fi for ~35 s (logs only at the end),
  which made flashing look broken (see the conversation; possible doc/logging change proposed, not applied).

## Flagged assumptions
- The stress churn script and its 18 ms busy / 1 tick idle pattern stand in for a busy script; real scripts are
  shorter. If the cases are fine, a real layout is too.
- `examples/test-server/net_test_server.py` gains `/mute` (GET state, POST toggle) and millisecond timestamps for the
  latency checks.

## Plan
- [x] 1. `standalone_script_platform()` shared; op task logs script stats and stack free when the queue drains.
  Line: `scripts: N runs, N errors, heap peak N KB, N allocs refused, op stack N bytes free`, after
  `op queue empty`, only when scripts ran since the last one.
- [x] 2. `RUN_STRESS` script cases: `lua churn, at rest`, `lua churn, mid-swipe`, `lua + keys, mid, flush`; each logs a
  `lua:` line under its `stress` line. The churn task is `stress_lua` (core 0, prio 3, 12 KB). `s_script_runs` is
  `volatile`: GCC rejects `++` on it (`-Werror=volatile`), so it is `x = x + 1` (one writer).
- [x] 3. `examples/script-test.mjs` (bakes: 81,516 bytes); `examples/test-common.mjs` now holds `SERVER`,
  `udp`/`get` and `flatten`, shared with `activity-test.mjs` (its bake is byte-identical to before).
  `examples/test-server/net_test_server.py`: ms timestamps, `/mute`.
- [x] 4. Builds: debug, release and debug + `RUN_STRESS` all 0 errors (the debug build dir was switched back to
  `CB_RUN_STRESS=OFF`). Host tests 553 pass, bake 139 pass.

### Fix after the first run (option 1, approved 2026-11-07)
- [x] 5. The churn script moved to `main/stress_script.h`; `Script Stress Churn Stays Well Inside The Heap Cap`
  (TestScriptHost.cpp) runs it 50 times under `SCRIPT_HEAP_CAP`: no errors, nothing refused, peak under half the
  cap. Red with the 800-iteration script (a run failed on the host too), green at 160 iterations (peak ~82 KB; most of
  it is garbage the incremental collector hasn't reached between back-to-back runs, so a quarter of the cap was too
  tight a bound). 160 should be ~18 ms a run on the device, scaled from the measured ~90 ms; to be confirmed by the
  `lua:` run counts (~80-100 runs per 2 s case if so).
- [x] 6. Stress diagnostics in `main.cpp`: `check_layout()` logs the first refused `SetPageCount`/`SetKeys` of each
  case (`stress: ... refused (N)`); each case logs `the compositor drew no keys while the pages built` if the 300 ms
  build drew nothing; a `stress run starting (~35 s)` line at the start. Device only (Rule 3 warning).
- [x] 7. Builds: release + `RUN_STRESS` and debug, 0 errors; host tests 554 pass (debug and release).
- [x] 8. The user re-runs A (below) and pastes the log, including any `stress:` error lines. Done (run 8). Cleanup
  after (approved): the retries and the misfiring `drew no keys` warning are gone; the refusal log (with the
  missing ID) and the one-line load report (refusals, PSRAM copy check: `| all accepted, PSRAM copy intact`)
  stay. CLAUDE.md updated (approved): COM4, the flash checksum warning, the stress run's ~35 s and new worst case,
  the backlight item (corrupts PSRAM; boot keeps it on), the Lua and status bar Open Items. Release + stress and
  release normal build, 0 errors; `build/release` is now back to `CB_RUN_STRESS=OFF`, ready for B.
  - Run 2 (2026-11-07): every case's stress layout `SetKeys` is refused with `NotFound` (2): an image ID the
    layout names isn't live in the store. Cases built from scratch (`flush`) then draw nothing; `incremental` ones
    draw leftovers. Not reproduced on the host: `LayoutApply Uploads After Home Stay Live` (TestLayoutApply.cpp)
    applies the sample layout's Home, uploads every test_keys image under the stress IDs, and all stay live.
    SimpleLib's `HashCore` read through: no fault found. Next run logs the first missing ID and the store's image
    count/usage in the refusal line, plus a `stress: N demo images loaded` line after the upload (release build, 0
    errors).
  - Runs 3-4: missing `0xde000200` (icon 0) and `0xde000110` (opaque face 0); the store held 25 images / 10 KB, then
    24 / 7 KB on the next boot: the demo upload stops at a point that varies (the old loop stopped at the first
    refusal, and the release build's `ESP_ERROR_CHECK` doesn't abort). The refusal's own log line is lost: it comes
    right after backlight-on, when the CH343P port drops. Host test `ImageStore Accepts Every Test Key Image At Its
    Blob Size` (every blob, at `get_blob_size` as the device does) passes, so the data is good: device-only and
    nondeterministic. Suspects: a supply dip at backlight-on corrupting PSRAM (the same dip Waveshare suspects for
    the CH343P), or an allocation failure.
  - The load now tries every image up to 3 times and reports each refusal (code, and whether a retry worked) in a
    `stress: N demo images ...` line printed with the results; `load_demo_images` is no longer under
    `ESP_ERROR_CHECK`.
  - Run 5: only blob 4 (opaque face 0, `0xde000110`, 2448 bytes) is refused: bad image (3), on all 3 tries. Every
    other image loads. Same `test_keys.bin` as the host tests (md5 checked), and `check_image` has nothing compiler
    dependent. Lua (160 iterations): 101 / 68 / 68 runs, 0 errors, heap peak 80 KB, fill max 264 / 347 / 366 µs, but
    still without the opaque layouts (cases with `keys 0`). Non-opaque cases now draw (keys 245-513, busy 28-61%).
  - Hypothesis: `load_pack` copies test_keys.bin into PSRAM right after `display_enable_backlight(true)`; the
    backlight inrush dips the supply (it drops the CH343P too) and corrupts the copy. Next run's report says whether
    the PSRAM copy differs from the original embedded in the app, and where.
  - User's idea (Waveshare says the backlight is on at power-up): leave it on through `ch422g_init` (today
    `INITIAL_OUT` turns it off, then `display_enable_backlight(true)` turns it back on). Doubles as the experiment.
  - Run 6: the PSRAM copy of blob 4 differs from the embedded original in 448 bytes (first at offset 330); the
    original checks ok. 448 = 7 x 64, the cache line size: looks like whole lines lost or overwritten rather than
    bit flips. Next run reports the whole pack's diffs right after the memcpy and again after the uploads
    (addresses, first 8 bytes copy/original) and a heap integrity check: wrong straight away = the copy itself
    (power or cache), wrong only later = a stray writer.
  - Run 7: **wrong straight after the memcpy**, unchanged after the uploads, heap intact. 379 bytes differ, spread
    over 0x3c38e22a..0x3c38e60e (~1 KB, about 10.8 KB into the 55 KB copy); sample bytes 02/00 (a single bit).
    Sparse bit errors in a short window at a fixed point in the copy, which starts right after
    `display_enable_backlight(true)`: consistent with the backlight's switch-on transient corrupting PSRAM
    transfers (the same transient that drops the CH343P). Not a firmware bug. Any PSRAM data written (or read) at a
    backlight switch-on is at risk (page buffers, the layout blob, the Lua heap; code and rodata are in PSRAM too),
    including runtime switch-ons (SET_BACKLIGHT, wake on touch). Next: 1-3-1 put to the user.
  - Option 1 chosen (2026-11-07): `INITIAL_OUT` in `ch422g.cpp` includes `CH422G_BACKLIGHT`, and `app_main` no
    longer switches it on (only `CB_NO_DISPLAY` switches it off). Release + stress and debug build, 0 errors.
    Device only (Rule 3 warning). Expected on the next run: `after the copy: ... 0 bytes differ`, every demo image
    accepted, no CH343P drop at boot. Runtime switch-ons still glitch: hardware fix (option 2) left for later.
  - Run 8 (after a flash that didn't take: the monitor's "checksum mismatch" showed the old app): **confirmed**. The
    copy matches the original, every demo image loads first time (43 images in the store), no CH343P drop, the
    whole boot log arrives. The cause was the backlight switch-on.
  - The `drew no keys while the pages built` warning misfires when a case uses the same layout and scroll as the one
    before (`opaque, mid, flush + status` after `status bar only`; `lua + keys` after `lua churn, mid-swipe`): the
    pages are already built. Both cases drew keys during the run.

## Hardware checklist (for the user)
Before you start: `node examples/test-server/test-server.mjs` on the PC (10.1.1.126; change `SERVER` in
`examples/test-common.mjs` if the address has moved).

**A. Stress (Lua writing PSRAM):** `idf.py --preset release -DCB_RUN_STRESS=ON -p COM4 flash monitor`, then
`-DCB_RUN_STRESS=OFF` afterwards.
- [x] The three `lua` cases show 0 over budget; compare their fill max with `image replaces, mid-swipe` (412 µs before).
  Run 8 (2026-11-07, backlight kept on at boot): `lua churn, at rest` 277 µs, `lua churn, mid-swipe` 351 µs,
  `lua + keys, mid, flush` 423 µs (keys 248, busy 38%, slowest key 3577 µs against ~2550 without Lua), all 0 over.
  `image replaces, mid-swipe` was 211 µs this run. Lua + compositor mid-swipe is the new worst case: 69% of the
  615 µs budget, above the 250 µs aim. Lua alone adds ~150 µs mid-swipe (196 -> 351).
- [x] Their `lua:` lines: errors 0, allocs refused 0, heap peak well under 256 KB. Run 8: 101 / 68 / 52 runs, 0 errors,
  0 refused, peak 80 KB.
- [x] Note the internal RAM free after init (`log_heap`) for the RAM item in Open Items. Run 8 (release +
  `RUN_STRESS`): 111,095 after init (largest 69,632), 89,059 with the Wi-Fi driver up (largest 49,152), 87,175
  connected (min ever 84,463, largest 46,080). CLAUDE.md has ~130 / ~108 / ~106 KB (largest 64 KB): ~19 KB less. The
  stress build still holds the `stress_lua` task's 12 KB internal stack (never deleted), and the op task's stack
  grew to 12 KB in the Lua job; re-read it from a normal release build during B before updating CLAUDE.md.
  Gateway pings after connecting: 137 / 55 / 786 / 85 ms, but the user's PC-to-device pings are 4-5 ms
  afterwards: just the first moments after connecting, the latency fix still holds.

**B. Script layout:** normal build flashed, then `node cli/cli.js upload examples/script-test.mjs` from the repo root.
Home keys, by row:
- [ ] Row 1, `GET reply`: `lua: get: 200 ok`. `GET 404`: `404: 404 not found` (the server shows `X-Test: lua`).
- [ ] `GET big`: `big: 200 16384`.
- [ ] `Mute toggle`: the server's mute flips each tap; the log shows the old and new state.
- [ ] `GET hang`: after ~10 s `hang: nil timeout`. Tap it again, and within the 10 s run `node examples/test-server/takeover.mjs 3`:
  no `hang:` line, no crash, and after the release `GET reply` still works.
- [ ] Row 2, `Latency op+script`: the server's two `latency:` lines are close together (the gap is the script's
  cost; tap twice, since the first run includes the compile). Note the touch-to-packet feel.
- [ ] `UDP x6`: `burst 1..6` in order, then 4 bytes `de ad be ef`.
- [ ] `Busy 10 s`: during it, tap `Ping (op)` and swipe. The ping arrives only after `busy: done` (the queue is busy),
  but taps and swipes should still feel responsive: the lock is held at most 80 ms at a time. Note any lag.
- [ ] `Too long (150 ms)`: `script: ... script ran too long without waiting`, no `(bad)` line.
- [ ] Row 3, `C stack deep`: `C recursion stopped at depth N` (Lua's C limit, ~200), no crash. Then the `scripts:`
  line's `op stack N bytes free`: well above 0 (the worst case; 12 KB stack).
- [ ] `Lua stack 2000`: `Lua recursion 2000`.
- [ ] `Heap cap`: `script: not enough memory`, `allocs refused` > 0 in the `scripts:` line, no `(bad)` line, and the
  next script still runs.
- [ ] `Type Hi!` (focus a text box first): types `Hi!`.
- [ ] `Switch Scripts`: `bad name: nil unknown activity`, `switch: true`, the `after` line, then
  `activity: none -> Scripts` and `Scripts didActivate` (a UDP line on the test server: the hooks are plain ops
  since a script's switch that would run a script is refused). On its page: `GET reply` works, `None (script)`
  goes back (`Scripts willDeactivate` on the test server, `activity: Scripts -> none`).

## Doc changes (approved and applied 2026-11-07)
- CLAUDE.md: the `RUN_STRESS` bullet names the Lua churn cases; the Lua module entry has the `scripts:` log line and
  `standalone_script_platform()`; the Lua Open Item points here; the repository layout lists the bake test layouts and
  `tools/test-server/`.
- `net_test_server.py` and `takeover.mjs` moved from `scratch/` (gitignored) to `tools/test-server/` (tracked).
  `takeover.mjs` now imports `../../host/index.js`. Run them from the repo root as
  `python tools/test-server/net_test_server.py` and `node tools/test-server/takeover.mjs 3`. (The project reorg has since
  moved them to `examples/test-server/`, with the layouts in `examples/`; the checklist above uses the new paths.)
  `wip/complete-activity-device-management.md` still names the old paths (left as written).

## Progress (2026-11-12)
- Scripts run from a layout on the hardware: urem2's Yamaha toggles (mute, 7ch Stereo: `yamaha-toggle.lua`, a waited
  `http.get` then a set) work against the real receiver. Checklist B above (`script-test.mjs`) not yet run: stack depth,
  `http.get` timeout/abandon, latency and the rest stay unticked.
