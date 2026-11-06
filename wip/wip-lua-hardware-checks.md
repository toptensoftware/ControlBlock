# Lua hardware check setup

Status: Implement (setup done; waiting for the user's hardware run. Kept as wip so the checklist can be ticked)

## Description
Set up the hardware checks for the Lua `script` op (job `wip/complete-lua-scripts.md`, CLAUDE.md Open Items), for the
user to run later:
1. A `RUN_STRESS` case with Lua churning its PSRAM heap, to show scripts don't make the bounce fill late.
2. A test config with scripts for the remaining checks: op task stack, the `http.get` reply path (timeout, takeover
   while waiting), button-to-request latency, key-event latency while a script holds the lock, plus the limits.

## Notes
- `RUN_STRESS` runs in `app_main` (core 0, 3.5 KB stack): too small for Lua, so the stress script runs on its own
  core 0 task (12 KB stack, priority 3), like the op task. It runs a churn script back to back for ~18 ms, then
  sleeps a tick, while the stress case runs.
- The Lua heap's PSRAM realloc is shared by the op task and the stress run: `standalone_script_platform()` in
  `standalone_task.h` (one copy).
- The op task's "op queue empty" log line gains the script stats when scripts ran: runs, errors, heap peak, and the op
  task's stack high-water mark (check 2).
- Device-only code: not host testable (Rule 3 warning). The test config is checked by baking it (Lua syntax check).

## Flagged assumptions
- The stress churn script and its 18 ms busy / 1 tick idle pattern stand in for a busy script; real scripts are
  shorter. If the cases are fine, a real config is too.
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

## Hardware checklist (for the user)
Before you start: `python examples/test-server/net_test_server.py` on the PC (10.1.1.126; change `SERVER` in
`examples/test-common.mjs` if the address has moved).

**A. Stress (Lua writing PSRAM):** `idf.py --preset release -DCB_RUN_STRESS=ON -p COM3 flash monitor`, then
`-DCB_RUN_STRESS=OFF` afterwards.
- [ ] The three `lua` cases show 0 over budget; compare their fill max with `image replaces, mid-swipe` (412 µs before).
- [ ] Their `lua:` lines: errors 0, allocs refused 0, heap peak well under 256 KB.
- [ ] Note the internal RAM free after init (`log_heap`) for the RAM item in Open Items.

**B. Script config:** normal build flashed, then `node cli/cli.js upload examples/script-test.mjs` from the repo root.
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
  `activity: none -> Scripts` and `Scripts didActivate`. On its page: `GET reply` works, `None (script)` goes back
  (`Scripts willDeactivate`, `activity: Scripts -> none`).

## Doc changes (approved and applied 2026-11-07)
- CLAUDE.md: the `RUN_STRESS` bullet names the Lua churn cases; the Lua module entry has the `scripts:` log line and
  `standalone_script_platform()`; the Lua Open Item points here; the layout lists the bake test configs and
  `tools/test-server/`.
- `net_test_server.py` and `takeover.mjs` moved from `scratch/` (gitignored) to `tools/test-server/` (tracked).
  `takeover.mjs` now imports `../../host/index.js`. Run them from the repo root as
  `python tools/test-server/net_test_server.py` and `node tools/test-server/takeover.mjs 3`. (The project reorg has since
  moved them to `examples/test-server/`, with the configs in `examples/`; the checklist above uses the new paths.)
  `wip/complete-activity-device-management.md` still names the old paths (left as written).
