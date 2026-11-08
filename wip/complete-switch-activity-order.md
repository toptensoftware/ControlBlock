# switchActivity runs the switch before the rest of the list

Status: Complete

## Description
Today `switchActivity` (the op and Lua's `switchActivity()`) pushes the switch's lists (hooks, device on/off) at the
**tail** of the op queue, and the rest of the current list keeps running first. So
`tap: [switchActivity atv, udp "app com.netflix.Netflix"]` sends the app launch before the devices are on, and a
`delay` can't help (it holds the whole queue). Change it so the switch's lists run **straight after the
`switchActivity` op, before the rest of the current list** (what a layout author expects; urem awaited the switch).
Firmware (`op_queue`, `standalone`/`activity_switch`, the Lua API), spec (`docs/standalone.md`,
`docs/layout-format.md`) and tests. All-or-nothing push and the full-queue refusal must keep working.

Found by the urem2 investigation (`wip/wip-urem2.md`, gap 2, decision 2); urem2's layout job depends on it.

## Notes

### How it works today
- **`OpQueue`** (`firmware/main/op_queue.h/.cpp`): a FIFO ring `m_ring[OP_QUEUE_SIZE=40]` of `Entry {ops, count,
  slot, release}` (`m_head`, `m_count`), plus the list being walked, `m_cur` (not a ring entry) with `m_cur_next`
  (next op to start). `m_waiting` = ops not started (rest of `m_cur` + every ring entry); `Count()` returns it,
  `Lists()` returns `m_count`. `StartNext()` takes `m_ring[m_head]` into `m_cur` when `m_cur` is spent, does
  `m_cur_next++` **before** calling `host.start`, so while an executor runs, `m_cur.ops[m_cur_next..count)` is
  exactly "the rest of the current list". One active op (`m_active`); a Pending op holds the queue (`delay`, a
  waiting script). Param slots: each not-yet-finished op holds one ref on its list's `ParamPool` slot (acquired
  `count` refs at push; `Finish` releases the active op's; `Cancel` releases the rest of `m_cur` and every ring
  entry's count).
- **`PushLists(refs, n)`**: all or nothing. Counts non-empty lists; if `m_count + needed > 40` adds `needed` to
  `m_dropped` and refuses; acquires every param slot first and rolls back on `PARAM_FULL`; then appends at the
  tail. `PushList` = `PushLists` of one. Users: `TriggerDispatcher` (bindings, stays tail) and
  `Standalone::SwitchActivity`.
- **Switch** (`standalone.cpp` `SwitchActivity`, called synchronously from the `switchActivity` executor
  `op_registry.cpp switch_start` via `ExecCtx::switch_activity`): same activity -> glide only; else
  `ActivitySwitchPlan` (pure, `activity_switch.cpp`, up to `ACTIVITY_SWITCH_MAX_LISTS` = 36 lists, no param, no
  release flag) -> `m_queue.PushLists` (tail) -> only on success commit mask, `SelectActivity`, gestures reset,
  `HidKeyboard::ReleaseAll`, `ActivityHost::apply`. Refusal returns "activity switch refused: op queue full"
  (op Error, nothing changes).
- **Lua**: `script_api.cpp SwitchActivity` fills the scratch op `m_op.sw` and runs it through `RunOp` -> same
  executor -> same `Standalone::SwitchActivity`, synchronously, while the script op is the queue's **active**
  op (`m_cur` = the script's list, `m_cur_next` past the script op). Returns `true` / `nil, err`. The script
  keeps running (holding the queue) and the pushed lists run after it. `ScriptHost` has one Lua state and **one
  run at a time** (`m_running`, one `m_thread`).
- **`delay`** is just a Pending executor: it holds the whole queue; with the change, a `delay` in a hook/turnOn
  now also holds back the rest of the triggering list (the point of the job).

### Chosen approach: insert "next" by splitting the current list
New `OpQueue::PushListsNext(refs, n)` (tail `PushLists` unchanged; the two share the count/param-acquire code,
DRY). It inserts the lists at the **front of the ring**, ahead of everything not yet started:
1. If `m_cur` still has ops left (`m_cur_next < m_cur.count`), its remainder becomes a ring entry
   `{ops + m_cur_next, count - m_cur_next, same slot, same release}` and `m_cur` is marked spent
   (`m_cur.count = m_cur_next`). The remaining ops' param refs move with it unchanged (same slot, same count), and
   `m_waiting` is unchanged.
2. The new lists go in **before** that remainder, and **after any lists already inserted "next" by the same
   active op** (a new member, e.g. `m_next_inserted`, reset in `StartNext` when a new op starts). Without this, a
   script calling `switchActivity()` twice would run the second switch's lists before the first's. Inserting in
   the middle of the ring shifts the later entries (at most 40; or move `m_head` back when inserting at the very
   front).
3. Room check: `needed` non-empty lists + 1 if a remainder still has to be split off. Refusal changes nothing
   (no split, params rolled back), counts `needed` in `Dropped()` as today. Worst case 36 + 1 = 37 <= 40, so any
   legal switch still fits an idle queue.
- `Standalone::SwitchActivity` calls `PushListsNext` instead of `PushLists`; commit-after-push and the refusal path
  stay as they are.
- **Cancel/takeover mid-switch**: unchanged code; the split remainder is an ordinary ring entry, so `Cancel`
  releases its refs. Test that `ParamsInUse()` returns to 0 after a split with a param, and after Cancel.
- **Nesting** (a `switchActivity` in a hook list; the bake forbids it but the executor tolerates it): the nested
  switch's lists go right after it, before the rest of that hook list, then the outer switch's later lists, then
  the rest of the original list: depth first. `TestStandalone` "Inside A Hook" keeps its order (the hook is last).
- **Rapid A->B->C** (two keys tapped before anything ran: two ring entries `[swB]`, `[swC]`): new order is
  `swB, B's lists, swC, C's lists` (was `swB, swC, B's lists, C's lists`). C is still planned from the state B
  committed; mask and device order stay right, and B's devices are now on before C is planned/runs. A single list
  `[swW, swL, swLt]` (activity-test's "Watch Listen Lights" key) now runs each switch's lists before the next
  switch (page swaps spread out over the ops). `docs/standalone.md` "Tail pushes" bullet is replaced.
- **Script**: see Decision 1 below. With the recommended option the switch's lists run as soon as the script op
  ends, before the rest of the script's list (falls out of the queue change; no Lua code change).

### Tests that change / are added
- `firmware/test/TestOpQueue.cpp`: new facts for `PushListsNext` (into an idle queue; mid list: inserted lists run
  before the remainder and before older ring entries; two inserts by one active op keep their order; remainder
  keeps param and release flag; refusal leaves the queue unchanged incl. the needed +1 for the split; ParamsInUse
  back to 0 after drain and after Cancel; a wrap-around case). Existing tail tests unchanged.
- `firmware/test/TestStandalone.cpp`: "Queued Back To Back Runs In Order" new expected order
  `SWITCH, WOL, GET, UDP, SWITCH, POST`; new fact: a list `[switchActivity, op]` runs the switch's lists before the
  op (needs a binding with a follow-up op: patch a test layout in memory, e.g. point a Home binding at a 2-op list
  built in the test, as "Inside A Hook" does, or add a key to `test/data/sample_layout.json` and re-bake: prefer
  the in-memory patch); refusal fact still passes (switch alone in its list: needs 3, ring full); "Inside A Hook"
  unchanged; takeover fact unchanged (`Queued()` is ops).
- `firmware/test/TestScriptQueue.cpp` "Ops A Script Queues Run After It": the fake `switch_activity` pushes at the
  tail; change it to `PushListsNext` and add a follow-up op after the script in its list to show the order:
  script's own calls, then the switch's op, then the list's next op. Also two `switchActivity()` calls in one script
  keep their order.
- `TestActivitySwitch.cpp`, `TestScriptParity.cpp`, `TestOpRegistry.cpp`, `TestActivityHost.cpp`: no change expected.

### Text that describes the order
- `docs/standalone.md`: line ~157 (still true), executor bullet (~164, "at the queue tail"), "Tail pushes" bullet
  (~167), Queue bullet (~216: mention `PushListsNext`), hook-switch bullet (~163, "only enqueues").
- `docs/layout-format.md`: `switchActivity` op row (~207: add "its switching ops run before the rest of the list"),
  hooks/delay bullet (~104), script API row (~237: "Queued at the tail ... runs after the script").
- `docs/standalone-ops-plan.md` section F (~105) and line ~9 say "tail": it is a plan doc ("not yet approved");
  add a one-line "superseded" note rather than rewrite (flagged).
- Code comments: `standalone.h:91`, `op_queue.h` (header comment), `activity_switch.h:6`, `script_api.cpp` header
  if Decision 1 changes behaviour, `TestScriptQueue.cpp` header comment.
- `CLAUDE.md` `standalone.cpp/.h` bullet: "pushes it at the queue tail all or nothing" -> needs the user's OK
  (Rule 4); and the Activity switching hardware bullet's "rapid A->B->C in order" (still true, order changed).
- `examples/activity-test.mjs`: no ordering claims in comments; the server output for the 3-switch key and rapid
  taps will differ from the recorded run in `wip/complete-activity-device-management.md` (don't touch that doc).
- Host bake (`host/src/bake/`): no ordering text; no change. `docs/protocol.md`: no change.

### Implementation notes (steps 1-3)
- **Step 1:** `OpQueue::PushListsNext` built as planned. Private helpers shared with `PushLists`: `Reserve`
  (count, room check with `extra`, param acquire with rollback, into an `int8_t slots[40]`) and `Insert(at, ...)`
  (one entry at offset `at` from `m_head`, shifting later entries back; a list version loops over it). A
  40-entry `Entry` array on the stack was avoided on purpose: `PushLists` also runs from the swipe task (4 KB
  stack) via triggers. `m_next_inserted` is reset in `Init`, `StartNext` and `Cancel`. The split only happens
  when the push has at least one non-empty list (an all-empty push succeeds and changes nothing, even on a full
  ring). Test rig: ops/list arrays grew to 16 and the fake start has an `on_start` hook (to push from inside an
  executor, like the switch). 9 new facts. Host tests 565 debug = 565 release.
- **Step 2:** `Standalone::SwitchActivity` calls `PushListsNext`. TestStandalone: "Back To Back" new order; new
  facts "Runs Its Ops Before The Rest Of Its List" (`[switch Watch, hidKey]`), "Hook Delay Holds Back The Rest Of
  The List" (Listen's willActivate delay holds the hidKey), "Two Switches In One List" (each switch's ops before
  the next). Lists are patched in memory (a local `op*` array assigned to the Home binding's `ops`); a
  `HomeSwitchBinding` helper was added and `HomeSwitch` now uses it. Refusal, "Inside A Hook" and takeover facts
  unchanged and green. 568 debug = 568 release.
- **Step 3:** docs updated: `standalone.md` (hook-switch bullet, executor bullet, "Tail pushes" replaced by "Switch
  ops run next", Queue bullet), `layout-format.md` (hooks/delay bullet, `switchActivity` op row; the script API
  row at ~237 left for step 4), `standalone-ops-plan.md` (superseded notes in section F and at line 9).
  `TestScriptQueue.cpp`'s fake still pushes at the tail (step 4).
- **Step 4a:** `OpQueue::ParkActive()` and `Parked()` built as designed. The active op is now a `Running {op, task,
  slot}` (`m_run`, `m_parked_run`); `Settle(st)` after every start/poll keeps a Pending op, parks it if a park was
  requested, or `Finish`es it (a lapsed request leaves its marker, which `StartNext` skips). `StartNext` returns bool
  and loops over spent lists/markers; reaching a marker with an op parked calls `Resume()` (restores it, resets
  `m_next_inserted`, polls it through `Settle`, so it can park again). `m_active` is now set **before** `start` so
  `ParkActive` can require a running op (an idle `PushListsNext` leaves `m_next_inserted` > 0; without the check a
  later op would park). `ParkActive` also refuses while another op is parked or a request is pending.
  **Room check:** `PushListsNext` now always reserves `split + 1` (the +1 keeps an entry for the marker, whether or
  not the op parks), so a succeeded push can always be followed by a park; the step-1 room facts moved by one. Worst
  switch: 36 + 1 + 1 = 38 <= 40. **4d must update `docs/standalone.md` line ~167** ("36 lists + 1" -> + 1 more for the
  marker; "needs one more entry" -> two). `Cancel` tells the active op, then the parked op (`host.cancel`, releases
  its slot). Rig: per-op `pend[]`/`left[]`, an event log (+start/-poll), `on_poll` hook, `tag`. 10 new facts.
  Host tests 578 debug = 578 release.
- **Step 4b:** `ScriptWait::Switch`, `ScriptHost::ParkForSwitch()` (null queue or `ParkActive()` false: no park;
  else sets the wait and `m_parked_at`). The API's `SwitchActivity` calls `StartOp()` itself (not `RunOp`): Pending
  or (Done and parked) yields with `Continue` (returns `true`), else `Results`. `ScriptHost::Poll` adds
  `Now() - m_parked_at` to `m_wall_deadline` **before** the timeout check when the wait is Switch, then resumes
  (`m_last` is still the switch's Done). `AbortRun` needs nothing for Switch (the queue cancels the parked op).
  TestScriptQueue: `QRig` now has a fake switch (static `s_cur`) that `PushListsNext`es `watch[]` (index 0),
  `listen[]` (1) or nothing (-1); `Added(i)` reads the key report `i` added. Facts: waits for the switch's ops
  (x, b, c, then the list's a), two switches keep order, a switch with nothing to run doesn't park, parked time
  not counted (2 x 20 s parks + 25 s own; checked it fails without the shift), own time still times out after a
  switch, takeover while parked. 583 tests.
- **Step 4c:** pure `ActivitySwitchRunsScript(plan)` in `activity_switch.cpp/.h` (+ a TestActivitySwitch fact);
  `Standalone::SwitchActivity` refuses (after the plan, before the push) when `m_script.Running()` and the plan
  runs a script: logs `script: switchActivity refused: the switch to <name|none> runs a script` through
  `ScriptHost::Log`, returns "switchActivity: the switch runs a script". `ScriptHost::Start` while running: logs
  and returns Error "script: another script is running" (not counted in `Errors()`). Tests: TestScriptQueue
  "A Second Script Cannot Start While One Runs"; TestStandalone (Rig now keeps `script_log` text) refusal from a
  script (note `run` order: a script's sub-op is reported before the script, since `started` fires after start)
  and allowed from an op. 587 tests.
- **Step 4d:** `layout-format.md` script API row and Limits line; `standalone.md` "Switch ops run next" room text
  (36 + split + marker = 38), new bullet "A script's `switchActivity()` waits" (incl. the bake warning for later),
  Queue bullet (`ParkActive`), script execution and limits bullets; headers of `script_api.cpp`,
  `TestScriptQueue.cpp`, `script_host.h`; comment in `standalone.h`. **Bake warning, for later (not built):** warn
  when a script calls `switchActivity` to an activity whose switch lists (hooks, device turnOn/turnOff) hold a
  `script` op. 587 debug = 587 release.

### Decision 1 (1-3-1): what `switchActivity()` means inside a script
**Problem:** urem2 decision 2 says the fix applies "in ops and in Lua `switchActivity()`". A script is the queue's
one active op and `ScriptHost` runs one script at a time, so the switch's lists cannot run *during* the script
without new machinery: Lua code after `switchActivity()` necessarily runs before the switch's hooks/device ops
unless the call waits.
**Options:**
1. **Lists run when the script ends** (before the rest of the script's list). Falls out of the queue change; no Lua
   change. Spec: "`switchActivity()` returns at once; the switch's ops run when the script finishes, before the
   next op of its list. To act after the switch, put the follow-up in the list after the script op." Cheap, no new
   failure modes.
2. **`switchActivity()` waits for its lists** (yields until they ran). Needs the queue to suspend the active op and
   run inserted lists under it (a stack of current lists), and `ScriptHost` to run nested scripts (a hook list that
   has a `script` op) or refuse them with an error. Matches the user's literal intent inside Lua; sizeable, risky
   (re-entrancy, cancel of a suspended script, the 30 s wall timeout while suspended).
3. **Option 1 now, plus a later opt-in** `switchActivity(name, { wait = true })` if a real layout needs it.
**Recommendation:** option 1 (urem2's layout only needs switch-then-op in plain lists; no script there switches
and then continues). The plan below assumes it; step 4 waits for the user's answer.

### Decision 1: answer (user, 2026-11-08)
**Option 2: Lua's `switchActivity()` waits** for the switch's lists, then returns; **a switch whose lists contain a
`script` op is an error when called from a script**. Feasibility assessed and approved as steps 4a-4d. Design:
- `OpQueue`: one parked slot (`m_parked`: op, `OpTask`, param slot) and a **resume marker** ring entry
  (`ops == nullptr`). `bool ParkActive()`: false if the active op pushed no next-lists (`m_next_inserted == 0`); else
  inserts the marker at `m_next_inserted` (after the switch's lists, before the split remainder) and sets
  `m_park_requested`. When the active op's start/poll returns Pending with the request set, the queue moves it to
  `m_parked` and clears `m_active` without `Finish` (param ref kept). Reaching the marker restores the parked op as
  active, resets `m_next_inserted`, and polls it; a marker with nothing parked is skipped. `Poll` busy while
  `m_waiting > 0 || m_parked`. Room check +1 for the marker (worst 36 + 1 + 1 = 38 <= 40). Nested switch from a hook:
  its lists go before the marker (depth first). `delay` in inserted lists: an ordinary Pending active op. Two
  `switchActivity()` calls: park/resume each in turn. `Cancel`: active, then parked (release its slot), then the ring;
  markers hold no refs.
- `ScriptHost`: `ScriptWait::Switch`; `Poll` sees it, clears the wait and resumes. Script API `SwitchActivity`: after
  `RunOp` returns Done, `if (queue->ParkActive()) { SetWaiting(Switch); lua_yieldk(..., Continue) }`. Yielding drops
  the lock between polls (100 ms slice intact). Wall timeout: parked time not counted (push `m_wall_deadline` back by
  the parked time on unpark).
- Error rule: up-front in `Standalone::SwitchActivity`: if `m_script.Running()`, scan the plan's ops for `OP_SCRIPT`;
  if found refuse before the push (nothing changes). Lua gets `nil, "switchActivity: the switch runs a script"`; log
  `script: switchActivity refused: ... runs a script`. Also covers a nested hook switch while a script is parked (that
  op ends in Error). Belt and braces: `ScriptHost::Start` while running returns Error "script: another script is
  running". Bake: warn only, later.

## Flagged assumptions
- API shape: a new `OpQueue::PushListsNext(refs, n)` beside `PushLists` (tail), sharing a private helper; name is a
  guess. Triggers keep pushing at the tail.
- "Next" inserts made by the same active op keep their order among themselves (FIFO), ahead of the split remainder.
- Splitting the current list costs one ring entry; the room check includes it, and `Dropped()` still counts only
  the refused lists.
- `docs/standalone-ops-plan.md` gets a short "superseded" note only (it is an unapproved plan doc).
- Rapid A->B->C now interleaves each switch with its own ops (behaviour change, documented); considered better.
- No hardware run is automated: the activity-test re-check (step 6) is the user's.
- (4a) `PushListsNext` always reserves the marker entry, even for an op that never parks (the switch op itself):
  a near-full queue refuses a next-push one entry sooner. Simpler than reserving only for scripts, and the worst
  legal switch still fits an idle queue.
- (4a) The executor is told to cancel the parked op on `Cancel` (after the active one), like the active op.
- (4c) The refusal log names the target ("the switch to Watch runs a script", "none" for none); the
  "another script is running" refusal is logged but not counted in `ScriptHost::Errors()` (the queue counts it).
- (4c) The script-op check lives in a pure helper `ActivitySwitchRunsScript` (activity_switch) rather than inline in
  `Standalone`, so it is unit tested on its own.

## Plan
- [x] 1. `OpQueue::PushListsNext` (TDD): add the `TestOpQueue.cpp` facts listed in Notes first, then implement in
  `op_queue.h/.cpp` (split the remainder of `m_cur`, insertion offset reset in `StartNext`, shared count/param
  helper with `PushLists`, room check +1 for the split). Update the `op_queue.h` comments. `mk run` green, and
  check debug and release test counts match (CLAUDE.md: ring writes out of bounds can hide tests).
- [x] 2. Switch uses it (TDD): update `TestStandalone.cpp` ("Back To Back" new order; new `[switchActivity, op]`
  fact; a two-switch single list; check refusal/hook/takeover facts still hold), then change
  `Standalone::SwitchActivity` to `PushListsNext` and the comments in `standalone.h` and `activity_switch.h`.
  `mk run` green.
- [x] 3. Spec: `docs/standalone.md` (executor bullet, replace "Tail pushes" with the new order incl. nesting and
  rapid switches, Queue bullet, hook-switch bullet) and `docs/layout-format.md` (`switchActivity` op row, the
  hooks/delay bullet: a `delay` in a hook now also holds back the rest of the triggering list); one-line
  "superseded" note in `docs/standalone-ops-plan.md` section F.
- [x] 4a. `OpQueue` park (TDD, `TestOpQueue.cpp` first; design in "Decision 1: answer" above): park runs the inserted
  lists then resumes the parked op before the remainder; no inserted lists = no park; nested next-push lands before
  the marker; `delay` in an inserted list; room check +1 for the marker; `Cancel` while parked cancels both and
  `ParamsInUse` returns to 0; wrap-around. Debug and release test counts must match.
- [x] 4b. Script waits (TDD, `TestScriptQueue.cpp`): fake `switch_activity` uses `PushListsNext`; code after
  `switchActivity()` sees the switch's ops done; two calls keep order; wall time excludes the parked time;
  takeover mid-wait. Add `ScriptWait::Switch`, the API park/yield and the wall-deadline shift.
- [x] 4c. Error rule (TDD, `TestStandalone`/`TestScriptQueue`): a switch whose lists hold a `script` op is refused
  from a running script (`nil, "switchActivity: the switch runs a script"`, logged, nothing changes) but allowed from
  an op; `ScriptHost::Start` reentry guard ("script: another script is running").
- [x] 4d. Docs: script API row in `docs/layout-format.md`, `docs/standalone.md`, the `script_api.cpp` and
  `TestScriptQueue.cpp` headers. Bake warning (a script op in the lists of an activity a script switches to) noted
  for later, not built. `mk run` green.
- [x] 5. (Done 2026-11-08: debug device build 0 errors, app 42% free; `FUZZ_SEEDS=200` release soak 587/587 passed.
  CLAUDE.md wording proposed to the user.) Device build: `idf.py --preset debug build` (0 errors); `FUZZ_SEEDS=200 mk config=release run` soak.
  Propose to the user the CLAUDE.md wording for the `standalone.cpp/.h` bullet ("pushes it straight after the
  switch op, ahead of the rest of its list, all or nothing") — don't edit without approval.
- [x] 6. Hardware (user): `examples/activity-test.mjs` rapid A->B->C and the "Watch Listen Lights" key (expect each
  switch's hooks/devices before the next switch), the "Big" switch (refusal headroom), takeover mid-switch.
