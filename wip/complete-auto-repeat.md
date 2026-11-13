# Auto repeat for standalone keys

Status: Complete

## Description
Add auto repeat to standalone mode. It isn't needed in hosted mode, where the
host sees press and release and can repeat on its own.

Requirements (from the user):
- Works for onscreen keys (`touch[id]`) and macro pad keys (`wip/wip-macro-pad.md`;
  the S3 side of the pad isn't built yet, so the design must cover pad controls
  even if they can only be tested once that phase lands).
- Each key can have its own initial delay and repeat rate, set in the layout.
- While a key is held, it sends repeated press events; releasing it sends one
  release event.
- If possible, the repeat iteration (0 for the first press, then 1, 2, ...) is
  passed to the bound ops as a parameter, so a script can take bigger steps the
  longer the key is held (e.g. volume).
- Auto repeat and long press can't both be on for one key.

## Notes

### How gestures work today
- `GestureBank` (`firmware/main/key_gesture.cpp/.h`, tests `firmware/test/TestKeyGesture.cpp`): one slot per
  held control (8 slots, names up to 23 chars). `Press(control, now, long_ms)` fires `press` at once, and `tap`
  at once too unless the control has a `long` binding (then `tap` waits for the release). `Tick(now)` fires `long`
  at the threshold. `Release` fires `release` (plus a deferred `tap`); `Cancel` (swipe, finger off the key) fires
  `release` only. `Reset()` drops every slot silently (activity switch, takeover, release of control).
- The gesture host is `has_binding(control, event)` and `trigger(control, event)`; `Standalone::GestureTrigger`
  calls `TriggerDispatcher::Trigger(control, event, "")`, so **gesture triggers carry no param today**. The
  `GestureHost::trigger` signature needs a param.
- Per-control timing is passed in at `Press`: `Standalone::Key(id, ...)` looks up `KeyLongPressMs(id)` (Home's keys,
  then the selected activity's pages). Pad keys (`key1`..`key32`, macro pad job step 3) will call `GestureBank`
  directly and have **no per-key settings anywhere in the blob**: they're only named in activity `bindings`
  (`{ control, event, ops }`).
- `Standalone::Tick()` runs once a frame (`swipe_set_tick_cb`, ~49 fps = ~20 ms). So repeats land up to ~20 ms
  late; a schedule computed from the press time (`down + delay + n x period`) keeps them from drifting.
- Touch timing: PRESS comes at touch-down; a swipe that starts inside `TOUCH_SWIPE_WINDOW_MS` (150 ms) gives
  CANCEL. An initial delay shorter than 150 ms could fire a repeat that a swipe then cancels (Question 6).

### Param
- `TriggerDispatcher::Trigger(control, event, param)` puts the param into `ParamPool` (`param_pool.h`: 8 slots x
  48 B, refcounted until the list's ops finish; an empty param takes no slot). Only `script` ops read it (global
  `param`, a string; `docs/layout-format.md` Scripts). The encoder already uses `delta=<n> interval=<us>`, so
  `repeat=<n>` fits the same `key=value` style (`tonumber(param:match("repeat=(%d+)"))`).
- Pool pressure: every repeat of a key with a param takes a slot until its list finishes. With a slow list (an
  `http.get` script) repeats would fill the pool and the 40-entry queue. See "Backlog" below.

### Backlog (repeats faster than the ops)
- One global queue: if a repeat's ops are still queued when the next is due, repeats pile up and run long after
  the finger lifts (the classic sticky autorepeat). Proposed: **skip a repeat while the previous one from the same
  control hasn't started yet** (or simply while the queue has lists waiting: `OpQueue::Lists() > 0`), and keep the
  iteration time-based (`n` = periods elapsed since the first repeat), so a skipped repeat doesn't slow the
  acceleration. Simplest is the queue-wide check through a new `GestureHost::busy()` callback; per-control
  tracking needs the queue to report which lists are outstanding (more code, little gain with one queue).

### Long press and tap on a repeating key
- Incompatible with `long` (requirement). On a repeating control `GestureBank` never fires `long` and never defers
  `tap`. The bake errors on a key/control that has both `repeat` and a `long` gesture (and on `longPressMs` set on
  a repeating key).
- `tap`: most layouts (urem2, the examples) bind actions to `tap`/`action`, and a non-long key fires `tap` at the
  press. urem's `repeat` re-runs its press handler, and urem2 puts its repeating actions on `tap`
  (`urem2/layout/keys.mjs` `key(look, ops, { repeat })`). So a repeat should probably fire **`press` and `tap`**
  each time (see Question 1).
- HID: a `hidKey` `hold` (down on press, up on release) under a repeating key would send key-down again each
  repeat; the PC repeats a held key itself. The bake should error on `hold` under a repeating key. `tap` mode
  repeats fine.
- Releases: one `release` on release or cancel, as now. `HidKeyboard::ReleaseAll` on switch/takeover is unchanged.
- Activity switch / takeover / release / pad offline: `Reset()`/`Cancel` already end the slot, so repeating stops
  with it.

### Where the settings live (layout and blob)
- Authoring syntax already exists in urem2: `repeat: 100` or `{ initialDelay, period }`
  (`urem2/layout/keys.mjs` `checkRepeat`/`withRepeat`, which drops it today: "one place to change once repeat
  exists"). In urem (`urem/frontend/gpio.js:253-285`) **a plain number sets both the period and the initial delay**
  (`repeat: 100` = first repeat 100 ms after the press, then every 100 ms), and an object's `initialDelay` defaults
  to its `period`. urem2 uses `repeat: 100` on its arrows and transport keys, so with urem's meaning the first
  repeat comes at 100 ms, inside the 150 ms swipe window (see Question 6).
- Touch keys: add `repeatDelayMs` and `repeatMs` (0 = no repeat) to the blob `key` struct next to `longPressMs`
  (`host/src/bake/schema.js`, `keys.js`, `look.js` if it inherits like `longPressMs`). That changes the struct: bump
  `LAYOUT_BLOB_VERSION` (2 -> 3, `layout_blob.h` and `consts.js` `BLOB_VERSION`), `node tools/codegen/gen-types.js`,
  re-bake `firmware/test/data/sample_layout.bin`; stored layouts need a re-bake and upload (a v2 blob is refused).
- Pad keys: no struct exists. Options in Question 2. The macro pad job's step 3 (`Standalone::PadKey`) would pass
  the timing into `GestureBank::Press` the way `Key()` does.
- `Standalone::KeyLongPressMs(id)` becomes a timing lookup returning `{ longMs, repeatDelayMs, repeatMs }` (one
  walk, DRY), and `GestureBank::Press` takes that struct.

### Files the work touches
- Firmware (pure, tested): `key_gesture.cpp/.h` + `TestKeyGesture.cpp`; `standalone.cpp/.h` + `TestStandalone.cpp`
  (sample layout gets a repeating key); `layout_blob.cpp/.h` (+ validator: repeat values, and maybe "repeat with a
  long binding") + `TestLayoutBlob.cpp`; `layout_types.gen.h` (generated); `trigger_dispatch` unchanged.
- Bake: `host/src/bake/schema.js`, `keys.js`, `look.js`, `resolve.js` (pad controls, if Question 2 puts settings
  there), `ops.js` (`hold` check), `consts.js` (version, limits); tests in `host/test/bake/`.
- urem2: `withRepeat()` emits the new property instead of dropping it; `keys.test.mjs` updated; README's "couldn't
  port" row closed.
- Docs: `docs/standalone.md` (Key gestures), `docs/layout-format.md` (key property, script `param`), CLAUDE.md
  (GestureBank line, blob version).
- Hardware only: the repeat feel (period jitter at 49 fps ticks), swipe vs repeat on touch. Pad repeat can only be
  tested on the hardware once the macro pad's S3 phase is built; host tests cover it through `GestureBank`.

### Plan-phase findings (2026-11-13, checked against the code)
- `GestureBank` has one user, `Standalone` (`standalone.cpp:56-60`, `Key()` at 136-147); its host has no param and no
  busy callback. `Standalone::GestureTrigger` passes `""` to `TriggerDispatcher::Trigger(control, event, param)`.
- `OpQueue::Lists()` = ring entries waiting (the current list isn't one), `Count()` = ops not yet started. "Busy" for
  repeats = `Lists() > 0`: the active list may still run, at most one repeat's lists wait behind it.
- `TOUCH_SWIPE_WINDOW_MS` (150) is in `touch_router.h`, which `standalone.cpp` already includes.
- Blob limits live in `layout_blob.h` (`LAYOUT_MAX_*`), mirrored in `consts.js` where the bake needs them. The
  validator walks activities in `Validator::Activity` (`layout_blob.cpp:344`); a `Controls()` check goes beside
  `Bindings()`. `firmware/test/layout_env.h` has `CB_OFF` offsets of root fields (check after the schema change).
- Bake: `longPressMs` is a look property (`look.js` `BUTTON_LOOK_SPEC`, inherits through `defaults`); gestures are in
  `keys.js` (`GESTURE_SPEC`, `NOT_LOOK`, `gesturesOf`, `makeKey`, `warnDuplicates` signature). `ops.js:68-70` already
  limits `hidKey` `hold` to `press`/`release`, given the gesture name; `resolveOpList(value, path, rc, gesture)`.
  Activity sections are checked in `resolve.js` `resolveActivities`; `resolveBindings` rejects `touch[...]` controls.
  The bake doesn't validate pad control names yet (macro pad step 10, not done).
- schema.js: a C++ member can't be named like its type (see the root's `home`/`homePage` cname), so the controls
  table's struct needs a type name other than `control` (its field is `control`).
- urem2: `withRepeat(spec)` (`layout/keys.mjs`) is used by `key()` and `padBindings()` (`layout/pad.mjs`); pad
  repeats are on `press` (`padArrows`, `ARROW_REPEAT` = `{ initialDelay: 500, period: 100 }`), touch repeats on `tap`
  (`transportKeys`, atv and steam `{ repeat: 100 }`). `padPage` passes the pad spec's `repeat` to its on-screen keys.
  Activities spread `padBindings(...)` into `bindings`, so pad repeats need a second output for the activity's `pad`
  map (a `padControls(spec)` helper next to `padBindings`).
- Macro pad job (`wip/wip-macro-pad.md` step 3, not built): `Standalone::PadKey/PadTurn/PadOffline`, pad keys go
  straight into `GestureBank`, `GestureBank::CancelMatching(prefix)` for offline. This job adds
  `Standalone::ControlTiming(name)` (the selected activity's `controls` entry), which `PadKey` will call; nothing in
  this job calls it from a device path yet.

### Step 1 done (2026-11-13)
- `key_gesture.h`: `GestureTiming { long_ms, repeat_delay_ms, repeat_ms, not_before_ms }` replaces `Press`'s
  `long_ms`; `GestureHost::trigger(control, event, param, ctx)`, `GestureHost::busy(ctx)` (null = never busy);
  `GESTURE_PARAM_MAX` 24. The param is made in one place (`GestureBank::FirePressTap`, `repeat=%u`); non-repeating
  controls and every `release`/`long` get `""`. A repeating slot resolves `repeat_delay_ms` 0 to the period, skips
  the `has_binding(long)` check (so `defer_tap` is false and `long` can't fire). `TickRepeat`: unsigned
  `held = now - down` (wrap-safe), due once `held >= delay` and `held >= not_before`, `n = 1 + (held - delay) /
  period`, fires only if `n > last n` and not busy (so one per tick, and a busy-cleared tick mid period fires the
  missed `n` at once). The press itself fires even while busy (only repeats are skipped).
- `Standalone`: `GestureTrigger` passes the param through, `GestureBusy` = `m_queue.Lists() > 0`, `Key()` presses
  with `{ KeyLongPressMs(id), 0, 0, TOUCH_SWIPE_WINDOW_MS }` (repeat stays 0 until step 4's `KeyTiming`).
- 15 new tests in `TestKeyGesture.cpp`; `mk run` 773 passed, debug and release counts equal. Device build not run
  (only pure files changed; step 9 builds the device).

### Step 2 done (2026-11-13)
- Blob version 3: `key` gains `repeatDelayMs`, `repeatMs` (u32, after `longPressMs`; key 28 -> 36 bytes);
  `controlTiming { control (string), longPressMs, repeatDelayMs, repeatMs }` (16 bytes); `activity` gets
  `controls_count`/`controls` after `bindings` (activity 64 -> 72 bytes). `LAYOUT_MAX_CONTROLS` 32 /
  `MAX_CONTROLS` 32 (`consts.js`, checked against `layout_blob.h` in `render-real.test.js`).
- schema.js: the three timing fields come from one `timing()` function shared by `key` and `controlTiming`. It
  must be a function: **binpack writes into its field objects** (`field.offset`, `field.type`), so sharing one
  array between two types corrupted the packing (rects read back as 0).
- `Validator::Controls` (names only, non-null and terminated; count <= limit), called from `Activity` after
  `Bindings`. The test aliases the Watch bindings as a control table (same size, `control` first) since the
  sample's tables are empty until step 3.
- The bake emits `repeatDelayMs: 0, repeatMs: 0` (`keys.js` `makeKey`) and `controls: []` (`resolve.js`).
- `layout_env.h` needed no change (all offsets via `CB_OFF`). Sample re-baked (105484 bytes).
- Tests: `mk run` 774 passed, release 774 (equal); host `npm test` 277; cli 31; urem2 194; examples 34;
  binpack 81. Stored layouts on the board need a re-bake and upload (v2 is refused).

### Step 3 done (2026-11-13)
- `keys.js` exports the shared repeat pieces (used by touch keys and pad entries): `repeatCheck` (a number, or
  `{ initialDelay?, period }`, each `REPEAT_MIN_MS` 40..`REPEAT_MAX_MS` = `LONG_PRESS_MAX` 10000, in `consts.js`),
  `timingOf(own, longPressMs, path, diag)` -> `{ longPressMs, repeatDelayMs, repeatMs }` (errors on the control's own
  `longPressMs` with `repeat`; an inherited one is dropped to 0), and `checkRepeatGesture(event, ops, path, diag)`
  (error on `long`, and on a resolved `hidKey` op with mode `hold` under `press`, so it also catches holds inside
  `use`d actions). `repeat` is in `BUTTON_SPEC`/`MEMBER_SPEC` and `NOT_LOOK` (not in `GROUP_SPEC` or the defaults
  spec: both give "unknown property"). `gesturesOf` runs the gesture check per non-empty list on a repeating key;
  `warnDuplicates` includes the repeat fields.
- `resolve.js`: an activity's `pad` (`t.map`) -> `resolvePad` -> `controls` sorted by key number; names must be
  `key1`..`key<MAX_CONTROLS>` (`padKeyNumber`, regex `^key([1-9][0-9]?)$`, no leading zero); entries take only
  `repeat` and `longPressMs` (same range as `settings.longPressMs`, the checker is shared). `resolveBindings` gets the
  pad list and runs `checkRepeatGesture` on a repeating control's bindings (error path = the binding,
  `activities.X.bindings[i]`). Warning on a pad entry whose control has no binding in the activity.
- Sample layout: Home key `{ id: 400, label: "Rpt", repeat: 100, tap: script "return param" }` in the empty slot 3
  (file index 3, so Home's summary indices after it moved by one: tests updated, `home_count` 13); Watch gets binding
  `key1` `press` -> script `return param` and `pad: { key1: { repeat: { initialDelay: 300, period: 100 } } }`. Re-baked
  (111424 bytes).
- `TestLayoutBlob`'s `WithControls` now drops the sample's own control-name slots from the relocation table when it
  repoints the table (the validator requires every relocated slot to be reached). `TestLayoutTypes` reads the
  repeating key's fields and Watch's `controls[0]`.
- Tests: host `npm test` 289 (14 new: 7 in `keys.test.js`, 5 in `resolve.test.js`, pack round trip extended); `mk run`
  774 debug, 774 release; cli 31; urem2 194; examples 34; binpack 81. urem2 still drops `repeat` in `withRepeat`
  (step 5), so it bakes unchanged.

### Step 4 done (2026-11-13)
- `Standalone::KeyTiming(id)` (private) replaces `KeyLongPressMs`: one walk (Home's keys, then the selected
  activity's pages) to the key, its fields copied by a shared `TimingOf(T)` template (keys and `controlTiming`
  entries have the same three field names), `not_before_ms` = `TOUCH_SWIPE_WINDOW_MS` for every touch key (also an
  unknown id; harmless, it only delays repeats). `Key()` presses with `KeyTiming(id)`.
- `Standalone::ControlTiming(control)` (public, for the macro pad's `PadKey`): the selected activity's `controls`
  entry by name, `not_before_ms` 0; zeros with no activity, while hosted, or for a name not in the table. Nothing on
  the device calls it yet.
- 7 new tests in `TestStandalone.cpp` (`RepeatRig`: K_RPT's tap and a repurposed touch[1] binding as its release,
  both scripts that `log` their param): params `repeat=0..3` and the swipe-window hold (first repeat at 150, not
  100), release once with an empty param, cancel, busy skip with a 1000 ms `delay` (queue stays at 1 list), activity
  switch and takeover stop it, an activity page key's repeat, `ControlTiming`. Gotcha: a press list not yet polled
  counts as busy, so a test must `Drain()` after the press or the first repeat is (correctly) skipped. The existing
  long-press tests needed no change.
- `mk run` 781 debug, 781 release. Device build not run (only pure files changed, nothing device side called
  `KeyLongPressMs`; step 9 builds).

### Step 5 done (2026-11-13)
- `urem2/layout/keys.mjs` `withRepeat` keeps `repeat` (drops it only when undefined) and throws on `repeat` with
  `long` (the bake would refuse it; no urem2 key has both). `pad.mjs`: a private `padEntries(spec)` checks controls,
  events and repeat once (and refuses a repeat on `enc1`); `padBindings` (bindings, no repeat) and the new
  `padControls(spec)` (`{ key2: { repeat }, ... }`, control order, repeating keys only) both build on it.
  `defineActivity` always emits `pad: padControls(pad)` (`{}` for steam and playstation: urem had no repeat on
  steam's pad).
- Effect: atv/steam rewind/forward repeat (`repeat: 100`), atv/hdd pad arrows `{ 500, 100 }` in the activity's `pad`
  map and on the pad page's on-screen arrows (padPage already passed the spec's repeat to `key()`). The urem2 bake
  test (both entries, no warnings) passes, so the bake accepts all of it.
- README: the "Auto-repeat while held" row removed from "What couldn't be ported", an Auto-repeat paragraph under Key
  map (pad hardware repeat waits for the macro pad S3 phase), the pad page line updated.
- Tests: urem2 194 -> 198 (keys: repeat kept, resolved `repeatDelayMs`/`repeatMs`, repeat + long throws; pad:
  `padControls` x2, the clean activity now carries `controls`; activity: `pad` map; per-activity `pad` asserts and
  pad-page arrows with the repeat). Examples 34.

### Step 6 done (2026-11-13)
- `examples/script-test.mjs`: Home is full (15 keys), so the two keys went on the Scripts activity's page (switch
  with Home's "Switch Scripts" key): "Repeat 100 ms" (`repeat: 100`, tap logs `repeat repeat=<n>`, release logs the
  empty param) and "Repeat slow op" (`{ initialDelay: 300, period: 100 }`, tap = script log + a 400 ms `delay`: n
  should jump by ~4 per run, and at most one run follows the release).
- New test in `examples/layouts.test.mjs` (resolved timing fields, the bindings, the delay). Resolved ops use numeric
  op codes and `duration` for `delay` (not `op: "delay"`, `ms`). Examples 35 pass; urem2 198.
- examples/README's script-test row mentions the keys.

### Step 7 done (2026-11-13)
- `docs/standalone.md`: a Built entry for auto repeat; `GestureBank` in the trigger-side entry; validator (`controls`
  table, version 3, older blobs refused); limits (`LAYOUT_MAX_CONTROLS` 32); Schema (root version 3, key
  `longPressMs, repeatDelayMs, repeatMs`, activity `controls`, `controlTiming`, a Timing bullet); "Per-key timing"
  (`KeyTiming`, `ControlTiming`) replaces "Per-key `longPressMs`"; `param` example `repeat=3`; Key gestures intro and
  a `GestureTiming` bullet; new "Auto repeat" subsection (press + tap, `repeat=<n>`, release, busy skip, one per tick,
  swipe-window hold, no `long`, param pool); hardware-check and testing lines.
- `docs/layout-format.md`: intro note (blob version 3); overview and activity block show `pad`; group keys take
  `repeat`; Gestures table `repeat` row; new "Auto repeat" section (forms, range, events, param, busy skip, swipe
  window, the errors, not inherited, `pad` map); Scripts `param`; Bindings note on `pad`; Inheritance (repeat doesn't
  inherit); pipeline table (key fields, `activity.pad` -> `controls[]`); Validation errors and the `pad` warning.
- Pad control names in "Bindings" (`macro1`..`macro6`) left alone: the macro pad job renames them (`key1`..).

### Step 10 findings
- `examples/script-test.mjs`: Home's "Switch Scripts" key (a script) was refused ("the switch to Scripts runs a
  script") because the Scripts activity's hooks were scripts: a stale layout from before the switch-order job's rule.
  Fixed 2026-11-13: the hooks are `udp` ops now (`Scripts didActivate`/`willDeactivate` show on the test server);
  `wip/wip-lua-hardware-checks.md` checklist B updated to match.

### Proposed CLAUDE.md changes (step 8; confirmed and applied 2026-11-13)
1. Firmware, `layout_blob` line. Old: "Blob version 2 (`LAYOUT_BLOB_VERSION`; a version 1 blob is refused, leaving
   the empty Home until a re-bake and upload): a key is `{ id, rect[4] (quarter units), normal, pressed, longPressMs }`,
   and the validator checks rects" -> New: "Blob version 3 (`LAYOUT_BLOB_VERSION`; an older blob is refused, leaving
   the empty Home until a re-bake and upload): a key is `{ id, rect[4] (quarter units), normal, pressed, longPressMs,
   repeatDelayMs, repeatMs }`, an activity also has `controls` (`controlTiming { control, longPressMs, repeatDelayMs,
   repeatMs }`: macro pad keys' timing, up to `LAYOUT_MAX_CONTROLS` 32), and the validator checks rects"
2. Firmware, trigger-side line. Old: "`GestureBank` turns press/release/cancel into press, release, tap and long per
   control." -> New: "`GestureBank` turns press/release/cancel into press, release, tap and long per control, plus
   auto repeat: a control pressed with a repeat period (`GestureTiming { long_ms, repeat_delay_ms, repeat_ms,
   not_before_ms }`) fires press + tap again each period with the param `repeat=<n>` (time based), skipped while the
   host's `busy()` (op queue has lists waiting), never `long`; one `release` at the end. Spec: `docs/standalone.md`
   (Auto repeat)."
3. Firmware, `standalone` line. Old: "`KeyLongPressMs(id)` looks in Home's keys, then the selected activity's." ->
   New: "`KeyTiming(id)` (a key's `GestureTiming`: long press and repeat, `not_before_ms` = the swipe window) looks in
   Home's keys, then the selected activity's; `ControlTiming(name)` reads the selected activity's `controls` table for
   a pad key (for the macro pad's `PadKey`, not called on the device yet)."
4. Open Items, "Activity switching and devices". Old: "`KeyLongPressMs(id)` covers activity keys too." -> New:
   "`KeyTiming(id)` covers activity keys too."
5. Open Items, "Key render redesign". Old: "A stored layout from before the redesign (blob version 1) is refused:
   empty Home until re-baked and uploaded." -> New: "A stored layout from before auto repeat (blob version 1 or 2) is
   refused: empty Home until re-baked and uploaded."
6. Repository Layout, `urem2/` line: after "README.md lists what couldn't be ported." add "Auto repeat is ported
   (touch keys; pad keys through the activity's `pad` map, untested until the macro pad's S3 phase)."
7. Open Items, new entry: "**Auto repeat** (job `wip/wip-auto-repeat.md`): built and host tested, **not yet run on
   the hardware** (blob version 3: re-bake and upload every stored layout). Checks (`examples/script-test.mjs`, the
   Scripts activity's two repeat keys): feel and period jitter at ~49 fps ticks, the `repeat <n>` log lines, a swipe
   from a repeating key runs its action once and never a repeat, release stops at once with nothing queued after
   (busy skip), takeover and activity switch stop a held repeat; urem2-test's atv/steam rewind/forward. Pad repeat
   waits for the macro pad's S3 phase."

## Decisions (user, 2026-11-12)
All six recommendations below accepted: (1) a repeat fires `press` and `tap`; (2) pad keys' settings in a
per-activity `controls` table (`{ control, longPressMs, repeatDelayMs, repeatMs }`), which also gives pad keys a
per-key long press; (3) skip a repeat while the op queue has lists waiting; (4) param `repeat=<n>`, n = 0 at the
press, counting periods elapsed; the release has no param; (5) repeat + `long` is a bake error, and `GestureBank`
ignores `long` on a repeating control (no validator walk); (6) urem's meaning (`repeat: <ms>` = period and initial
delay), with a touch key's first repeat held until the swipe window has passed; pad keys repeat exactly as urem.

## Questions for the user (answered: see Decisions)
1. **Which events repeat?** (a) `press` and `tap` (recommended: `tap` already fires at the press on non-long keys,
   and urem2 binds its repeating actions to `tap`); (b) `press` only (layouts must bind repeating actions to
   `press`; urem2's helper would move them).
2. **Where do pad keys' repeat settings live?** (a) a per-activity `controls` table in the blob (`{ control,
   longPressMs, repeatDelayMs, repeatMs }`), authored as e.g. `pad: { key3: { repeat: 100 } }` (recommended: also
   gives pad keys a per-key `longPressMs`, which they lack); (b) on the binding itself (`{ control, event, repeat,
   do }`: no new table, but two bindings for one control could disagree); (c) one table for every control, touch
   keys included (`touch[id]`), replacing `longPressMs` on the key struct (most uniform, biggest change).
3. **Backlog:** skip a repeat while the queue still has lists waiting (recommended), or always queue every repeat?
4. **Param format:** `repeat=<n>` with `n` = 0 on the first press, counting periods elapsed (recommended), or
   repeats actually fired? And should the `release` carry the last `n` (e.g. so a script knows how long it was
   held)? Recommended: the release gets no param.
5. **Long press conflict on the device:** bake error only, with `GestureBank` ignoring `long` on a repeating
   control (recommended, cheap), or also have the blob validator refuse such a layout (needs a walk of the
   bindings per key at load)?

6. **Short initial delays vs swipes:** a touch key's first repeat at 100 ms (urem's `repeat: 100`) can fire before
   the 150 ms swipe window closes, so a swipe that starts on a repeating key could run its action twice before
   cancelling. (a) Keep urem's meaning (number = period and delay), but on touch keys hold the first repeat until
   the swipe window has passed (recommended: urem2 keeps its numbers; pad keys, which can't swipe, repeat exactly
   as urem); (b) make a plain number the period only, with the initial delay defaulting to 500 ms (differs from
   urem); (c) bake error for an initial delay under the swipe window.

## Flagged assumptions
- Job name `auto-repeat`.
- Syntax as urem: `repeat: <ms>` (period and initial delay) or `{ initialDelay, period }`.
- Minimum period ~40 ms (two frames at ~49 fps); bake error below it.
- ~~Description earlier said the device validator should refuse repeat + long; now an open question (5).~~
  Resolved by decision 5: bake error only, `GestureBank` ignores `long` on a repeating control, no validator walk.
- (Plan) Authoring: an activity's `pad` map, `pad: { key3: { repeat: 100, longPressMs: 800 } }` (the name from
  Question 2), becomes the blob's per-activity `controls` table. Names checked as `key1`..`key32` (encoders have no
  timing); macro pad step 10 should reuse the same check. Pad controls exist only in activities (as their bindings).
- (Plan) Blob struct type `controlTiming` `{ control (string), longPressMs, repeatDelayMs, repeatMs }` (type can't be
  named `control`, its field is); key struct gets `repeatDelayMs`, `repeatMs` after `longPressMs`. The bake always
  writes the resolved delay (a number = both), so `repeatMs` 0 = no repeat; firmware treats `repeatDelayMs` 0 as the
  period anyway.
- (Plan) `repeat` is a behaviour, not a look property: it is not inherited through `defaults` (unlike `longPressMs`).
  On a repeating key, an inherited `longPressMs` is ignored; only the key's own `longPressMs` is an error.
- (Plan) Limits: period and initial delay each 40..10000 ms (`REPEAT_MIN_MS` 40, max = `LONG_PRESS_MAX`);
  `LAYOUT_MAX_CONTROLS` 32 per activity (= pad keys), mirrored in `consts.js`. Validator checks only the table's
  pointers, strings and count, not the ms values.
- (Plan) The `repeat=<n>` param goes only on a repeating control's `press` and `tap` (n = 0 at the press);
  non-repeating controls keep the empty param (no param pool slot). The release has none (decision 4).
- (Plan) `GestureBank::Tick` fires at most one repeat per slot per tick (no catch-up burst after a late tick or a
  busy skip); `n` stays time-based: `n = 1 + floor((now - down - delay) / period)`.
- (Plan) Swipe-window hold as a `not_before_ms` field of the timing passed to `Press` (touch: `TOUCH_SWIPE_WINDOW_MS`,
  pad: 0), so the schedule and `n` keep urem's numbers and only the first repeat waits.
- (Plan) The `hold` check: a `hidKey` with mode `hold` under the `press` of a repeating key/control is an error
  (under `release` it only sends the up, so it stays allowed).
- (Plan) Spec docs (`standalone.md`, `layout-format.md`) are edited in the job's docs step as part of the work;
  only CLAUDE.md is proposed for the user's confirmation (Rule 4).
- (Step 3) `longPressMs: null` alongside `repeat` is allowed (null only clears an inherited value). A pad entry's
  `longPressMs` has no null form. `long: null` on a repeating key is no error (an empty list makes no binding).
- (Step 3) The `pad` name check uses `MAX_CONTROLS` (32) as the highest key number; macro pad step 10 should reuse
  `padKeyNumber` in `resolve.js` (export it then) rather than add its own.

- (Step 5) urem2's `defineActivity` always emits a `pad` map, `{}` when nothing repeats (the bake takes an empty map).
  urem2's `withRepeat` also refuses `repeat` + `long` early, ahead of the bake's error.
- (Step 6) The repeat keys are on the Scripts activity's page, not Home (Home's 15 slots are full).

## Plan
Test commands: firmware host tests `mk run` in `firmware/test` (also compare the test count with
`mk config=release run`, per CLAUDE.md); host library and bake `npm test` in `host/`; urem2
`node --test "urem2/**/*.test.mjs"` and examples `node --test "examples/**/*.test.mjs"` from the repo root; CLI
`npm test` in `cli/`; device `idf.py --preset debug build` (PowerShell recipe in CLAUDE.md).

- [x] 1. **`GestureBank` repeat (pure).** Tests first in `firmware/test/TestKeyGesture.cpp`: a repeating control
  fires `press` + `tap` at the press with param `repeat=0`, then `press` + `tap` at `down + delay` with `repeat=1`,
  then every period (`repeat=2`, ...); `release` (no param) on release, also on cancel, and repeating stops;
  `long` never fires and `tap` is never deferred on a repeating control even with a `long` binding; `not_before_ms`
  holds the first repeat but keeps the schedule and `n` (delay 100, hold 150, period 100: n 1 at 150, n 2 at 200);
  a tick while `busy()` skips the repeat and the next one fired carries the time-based `n`; one repeat per tick at
  most (a 350 ms tick gap fires one, with the right `n`); clock wrap; `Reset()` stops repeating; non-repeating
  controls unchanged with empty params (existing tests adapted to the new signatures). Then `key_gesture.h/.cpp`:
  `struct GestureTiming { long_ms, repeat_delay_ms, repeat_ms, not_before_ms }` passed to `Press` (DRY: one struct,
  replaces the `long_ms` argument), `GestureHost::trigger(control, event, param, ctx)` and
  `GestureHost::busy(ctx)` (may be null = never busy), the param formatted in one place (`repeat=%u`). Adapt
  `Standalone` minimally so everything builds: `GestureTrigger` passes the param through, `busy` =
  `m_queue.Lists() > 0`, `Key()` passes `{ KeyLongPressMs(id), 0, 0, TOUCH_SWIPE_WINDOW_MS }`. `mk run` (debug and
  release counts equal).
- [x] 2. **Blob v3 schema (no behaviour yet).** Tests first: `TestLayoutBlob.cpp` (`LAYOUT_BLOB_VERSION == 3`, a v2
  blob refused with `BadVersion`, a `controls` table with a bad string pointer / over `LAYOUT_MAX_CONTROLS` refused),
  `TestLayoutTypes.cpp` (the new key fields read 0, an activity's `controls_count` 0), host `test/bake/pack.test.js`
  (the new key fields and an empty `controls` pack), the existing `render-real.test.js` version/drift checks. Then:
  `schema.js` (key `repeatDelayMs`, `repeatMs` after `longPressMs`; type `controlTiming`; activity
  `...list("controls", "controlTiming*")` after `bindings`), `consts.js` `BLOB_VERSION` 3 and `MAX_CONTROLS` 32,
  `layout_blob.h` `LAYOUT_BLOB_VERSION` 3 and `LAYOUT_MAX_CONTROLS` 32, `layout_blob.cpp` `Validator::Controls`
  called from `Activity`; the bake emits `repeatDelayMs: 0, repeatMs: 0` (`keys.js` `makeKey`) and `controls: []`
  (`resolve.js`). Run `node tools/codegen/gen-types.js`, re-bake `firmware/test/data/sample_layout.bin`
  (`node cli/cli.js bake firmware/test/data/sample_layout.json -o firmware/test/data/sample_layout.bin`), check
  `firmware/test/layout_env.h` offsets. `mk run`, `host` `npm test`, `cli` `npm test`, urem2 and examples tests.
  Note: stored layouts on the board are refused after this until re-baked and uploaded (empty Home).
- [x] 3. **Bake authoring.** Tests first in `host/test/bake/` (`keys.test.js`, `resolve.test.js`, plus `ops`
  coverage where the hold check lives): `repeat: 100` -> `repeatDelayMs` 100, `repeatMs` 100;
  `{ initialDelay: 500, period: 100 }` -> 500/100; on buttons and group members; not inherited from `defaults`
  (`repeat` in a defaults block is an error, it isn't a look property); errors with paths for: a bad shape or
  values outside 40..10000, `repeat` with a `long` gesture, `repeat` with the key's own `longPressMs`, `hidKey`
  `hold` under the `press` of a repeating key; activity `pad: { key3: { repeat: 100, longPressMs: 800 } }` ->
  `controls` (in control order), names must be `key1`..`key32`, the same errors against that activity's
  `key3` bindings (`long` binding, `hold` under `press`), `pad` on a key that has no binding is a warning;
  `warnDuplicates` counts repeat. Then `keys.js` (repeat in `BUTTON_SPEC`/`MEMBER_SPEC`, `NOT_LOOK`, one shared
  `repeatOf(value, path, diag)` used by keys and pad entries), `resolve.js` (`pad` in the activity section), the
  hold check (one helper over resolved op lists, used for keys and pad controls), `consts.js` `REPEAT_MIN_MS`.
  Extend `firmware/test/data/sample_layout.json` with a repeating Home key (explicit id so no existing id moves;
  e.g. a script that returns `param`) and an activity `pad` entry with a `key1` binding; re-bake the sample.
  `host` `npm test`, `mk run` (existing firmware tests must still pass with the extended sample).
- [x] 4. **`Standalone` timing lookup and wiring.** Tests first in `firmware/test/TestStandalone.cpp` (with the step 3
  sample): the repeating Home key runs its ops at the press and again after the delay, with `param` `repeat=0`,
  `repeat=1`, ... reaching a `script` op; its first repeat waits for `TOUCH_SWIPE_WINDOW_MS` when the delay is
  shorter; release runs `release` once with no param; a cancel stops it; repeats are skipped while the op queue
  has lists waiting (a slow `delay` op), and the queue doesn't grow while held; an activity switch and
  `SetHosted` stop it; `ControlTiming("key1")` returns the selected activity's entry, and zeros for no activity /
  an unknown name / Home; activity keys' repeat read from the activity pages. Then `standalone.cpp/.h`: replace
  `KeyLongPressMs(id)` with `KeyTiming(id)` -> `GestureTiming` (one walk, DRY; `not_before_ms` =
  `TOUCH_SWIPE_WINDOW_MS`), add `ControlTiming(const char *control)` for the macro pad's `PadKey` (step 3 of that
  job; `not_before_ms` 0). Adjust the existing `KeyLongPressMs` tests. `mk run` (debug and release counts equal).
- [x] 5. **urem2.** Tests first: `layout/keys.test.mjs` (`key(..., { repeat: 100 })` emits `repeat: 100`,
  `withRepeat` keeps it, bad values still throw), `layout/pad.test.mjs` (`padBindings` no longer accepts/drops
  `repeat` silently: the repeat goes to a new `padControls(spec)` -> `{ key2: { repeat }, ... }`; `padPage`'s
  on-screen keys carry the repeat), the activity tests (`atv.test.mjs`, `steam.test.mjs`, any using `padArrows`:
  the activity has a `pad` map). Then `keys.mjs` `withRepeat` passes the property through (comment updated),
  `pad.mjs` `padControls`, each activity that uses pad repeats sets `pad: padControls(spec)`. Check no repeating
  key also has `long` (the bake would refuse it). README: close the "Auto-repeat while held" row (move it to what
  was ported, note pad repeat waits for the macro pad S3 phase). `node --test "urem2/**/*.test.mjs"`.
- [x] 6. **Example for the hardware check.** Add to `examples/script-test.mjs` (or a small new example if that page
  is full) a repeating key whose script logs `param` (e.g. `log("repeat " .. param)`) and one with a slow op to show
  the busy skip; the examples bake check must pass: `node --test "examples/**/*.test.mjs"`.
- [x] 7. **Docs.** `docs/standalone.md`: Key gestures (repeat: press + tap each period, `repeat=<n>` param, busy
  skip, swipe-window hold, no `long` on a repeating control), schema (version 3, key `repeatDelayMs`/`repeatMs`,
  activity `controls`, `LAYOUT_MAX_CONTROLS`). `docs/layout-format.md`: the key `repeat` property (urem's meaning),
  the activity `pad` map, the errors, script `param` `repeat=<n>`, the pipeline table's key and activity rows.
  No tests (docs only).
- [x] 8. **Propose the CLAUDE.md update (Rule 4: do not edit until the user confirms).** *Confirmed by the user
  and applied 2026-11-13 (all seven changes in Notes).* Write the proposed text into
  *Notes* and report it: the `key_gesture` / `standalone` lines (repeat, `GestureTiming`, `KeyTiming`,
  `ControlTiming`), the blob version 3 and key struct in the `layout_blob` line, the urem2 line (repeat ported),
  and an Open Item for the hardware checks in step 10.
- [x] 9. **Device build.** `idf.py --preset debug build` and `idf.py --preset release build`, 0 errors (all the
  changed firmware is pure, but `standalone_task.cpp` and the device include paths must still compile). Record
  internal RAM/flash changes if any are visible.
  *Done 2026-11-13: both built first time, 0 errors, no fixes needed (key_gesture, standalone, layout_blob,
  standalone_task, host_link etc. recompiled). App image: debug 0x141180 (37% of the 2 MB partition free),
  release 0x12df50 (41% free). Only warning: the existing picolibc link warning "times is not implemented"
  (not from this job). No baseline to compare static RAM against; internal RAM is read at boot on the hardware
  (step 10).*
- [x] 10. **Hardware checklist for the user (not automatable).** *Passed on the hardware 2026-11-13 (user: "all
  good"), after the script-test layout fix in Notes (Step 10 findings). Pad repeat still waits for the macro pad.* Flash; re-bake and upload every stored layout (v2
  is refused: empty Home until then); with `examples/script-test.mjs` (step 6): repeat feel and period jitter at
  ~49 fps ticks, the `repeat <n>` log lines, a swipe started on a repeating key runs its action once and never a
  repeat, release stops at once with nothing queued after (busy skip), takeover and activity switch stop a held
  repeat; urem2-test: atv/steam rewind/forward repeat. Pad repeat can only be tried once the macro pad's S3 phase
  (`wip/wip-macro-pad.md` steps 2-6) is built; until then it is covered by host tests only (steps 1 and 4).

Steps that can't be covered by automated tests: 7 and 8 (docs), 9 (build only), 10 (hardware).
