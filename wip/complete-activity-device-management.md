# Activity and device management

Status: Complete

## Description
Add the standalone-mode "activity" and "device" layers: the device holds a Home page plus
activities (each a set of pages) in flash, and keys can switch activity and operate
devices (things the ControlBlock controls, e.g. a TV or a PC reached over HID, HTTP, UDP or
WoL). Goal: define what an activity and a device are in the config format and on the
device, how switching works (selected-activity state, bindings, power on/off sequences,
per-device state), and what firmware, bake tool and docs need to change.

Starting points: `docs/standalone.md` (spec), `docs/standalone-ops-plan.md` (the earlier
plan explicitly left activities out of Phase A), `docs/config-format.md`.

Out of scope for this job: HTTPS, media keys, MIDI, Lua (so `script` ops in hooks stay
no-ops, as now).

## Notes

### 1. What the specs already say (settled design, no need to re-decide)

Sources: `docs/standalone.md` (Page strip and activities; Devices and activity switching;
Triggers and bindings), `docs/config-format.md` (Home and activities; Devices).

- **Strip:** Home is always page 0 (15 keys). Pages 1..n are the selected activity's. Selecting
  an activity swaps pages 1..n (Home kept) and glides to page 1. Swiping right from page 1
  returns to Home; no home button. Boot: Home only, no activity. Selection is not remembered
  across reboots. `goto-activity` works from any page; the swap reuses `key_layout`'s
  all-or-nothing `SetKeys`. Selecting the current activity only glides to page 1 and runs no ops.
- **Status bar:** left text = selected activity's `displayName` or `name`; empty with none
  selected. Right text blank for now. One pager pill per page including Home.
- **Device:** external equipment with `turnOn` and `turnOff` op lists. An activity lists the
  devices it uses (indices). Only devices whose use changes are powered.
- **Switch order, as queued ops:** `old.willDeactivate` -> `new.willActivate` -> turnOn for
  devices the new activity needs and that aren't on -> turnOff for devices the old had and the
  new doesn't -> `new.didActivate` -> `old.didDeactivate`.
- **Page swap does not wait for those ops:** the selected activity changes and the pages swap
  at once; the ops run through the queue. A device's on/off state is updated **when its ops are
  enqueued** (as in irtx), so A->B then B->C quickly queue the right ops in order.
- **Bindings:** Home's bindings (touch only) plus the **selected activity's** bindings are
  matched per trigger. Touch column index runs across the strip: page p column c = `5p + c`, so
  an activity's first page starts at column 5 and the name identifies the page. The macro pad
  and encoder always use the current activity's bindings, even while Home is in view; with no
  activity selected they do nothing.
- **Hosted/standalone:** hosted takeover and release both re-arm with **no activity selected**
  and rebuild Home only (`Protocol::Release` -> `ResetModel(true)` -> `ConfigApplyHome`).
- A Home key selects an activity with an ordinary `switchActivity` op (name resolved to an
  index by the bake); no special key type.
- `docs/standalone.md` "Next" list says the order is: `switchActivity` ordering and device
  on/off tracking on the op queue; then page strip and activity switching (selecting an
  activity, freeing the blob on a hosted takeover); then Lua; then HTTP/UDP/WoL (the latter
  is now built, phase A).

### 2. What is built vs missing

**Built (and host tested):**
- Bake tool (`tools/bake/src/resolve.js`): resolves `devices` (turnOn/turnOff as "hook" op
  lists), `activities` (name, displayName, devices -> indices, pages with strip-aware touch
  names `5*(p+1)+col`, activity `bindings`, the four hooks), `switchActivity` name -> index
  (`ops.js`), warnings for a device used by no activity. Schema (`schema.js`) and generated C
  types (`config_types.gen.h`) carry all of it.
- Blob validator (`config_blob.cpp`): walks devices and activities, checks device indices
  (activity `devices`), `switchActivity` index < activity count, pages 1..7 per activity
  (`CONFIG_MAX_ACTIVITY_PAGES = SCREEN_MAX_PAGES - 1`), limits 32 devices / 32 activities /
  256 bindings per list / 64 ops per list.
- `TriggerDispatcher::SetActivity(index)` swaps the second binding list (-1 = none, false for
  an out-of-range index). `Standalone::SelectActivity(index)` is a pass-through to it, used only
  by tests (`TestStandalone`: "Release Selects No Activity").
- `Standalone` gestures use `TouchControlName(page, key)` across the strip already, so a key on
  strip page p gets the right `touch[5p+c, row]` name once such pages exist.
- Op queue/executor protocol (`op_exec.h`, `op_registry.cpp`): `ExecCtx` already has a
  `queue` pointer "for ops that push more (switchActivity, script)".
- Net ops (httpGet/httpPost/udpPacket/sendWol) are built and host tested, **not yet verified on
  hardware** (Wi-Fi hardware testing pending), so device turnOn/turnOff lists built from them are
  unverified too. **Update 2026-11-06:** the net ops now work on the hardware (see CLAUDE.md, Net
  ops); turnOn/turnOff lists are still to be run (step 9 g).

**Missing (everything runtime):**
- `OP_SWITCH_ACTIVITY` has **no executor** (`op_registry.cpp` entry is all nullptr: the op is
  Done and does nothing). `script` likewise.
- Nothing holds "selected activity" or "device on/off" state. `Standalone` has no selected
  index, no device mask, no knowledge of the activity-device relation.
- No code pushes the switch sequence. The queue only has `PushList(count, ops[], release,
  param)` for **one** contiguous op-pointer array; the sequence is made of up to six lists.
- No page swap: `ConfigApplyHome` only does page 0 and works from `ConfigSummary` (which holds
  Home's images only). There is no equivalent for an activity's pages, no `SetPageCount` call
  for standalone, no go-to-page, no status text, no release/free of the previous activity's
  images.
- `Standalone::KeyLongPressMs` returns 0 for pages != 0 (activity keys get the config default
  only; per-key `longPressMs` for activity pages isn't read; the packed `key` has the field).
- Hooks (`willActivate` etc.) and device lists only exist as data.
- Docs say "freeing the blob on a hosted takeover" as a later optimisation; unrelated to this
  job unless we decide otherwise (see memory).
- No spec way to turn everything off / deselect the activity (see Q1).

### 3. What the config schema already carries (no schema change needed for the base feature)

Root `{version, longPressMs, home{keys[15], bindings[]}, devices[], activities[]}`;
`device{name, turnOn, turnOff}`; `activity{name, displayName?, devices int[], pages page[],
bindings[], willDeactivate, willActivate, didActivate, didDeactivate}`;
`switchActivityOp{index uint}`. `ConfigSummary` has `device_count`, `activity_count` and
Home's images only.

Schema gaps / candidate changes (see questions): no "none"/"all off" target for
`switchActivity`; `displayName`/`name` length vs `STATUS_BAR_TEXT_MAX` (48 bytes with NUL, longer
is cut by the status bar; the bake may not warn); `device` has no initial/assumed state field.

### 4. How a switch changes the runtime (traced through the code)

`switchActivity` op starts on the op task (core 0) under the `standalone_task` mutex, inside
`Standalone::Poll()` -> `OpQueue::StartNext` -> registry `start`. The executor needs a new
injected host (suggested: an `ActivityHost` next to `HidSink`/`NetHost` in `StandaloneHost`, and
a hook in `ExecCtx`, because the pure `Standalone` owns the dispatcher/queue and the
device layer owns the model/swipe/status). What has to change, in order:

1. **State (pure, in `Standalone`):** `m_activity` (-1 none), `m_devices_on` (32-bit mask).
   Same activity requested -> only "glide to page 1" (host call), no ops.
2. **Compute the op sequence** from `root->activities[old]`, `[new]`, `root->devices[]`, the mask.
   Pure function, easily tested (TDD target): input (old, new, mask) -> list of op-list refs +
   new mask.
3. **Push atomically.** `OpQueue` ring is 32 entries, `CONFIG_MAX_OPS` is 64 per list, and one
   switch may be six lists of up to 64: a long switch always "doesn't fit" and is dropped.
   Needs a multi-list all-or-nothing push (or a bigger ring, or a cap on hook length) and the
   device mask / selection must be committed **only if the push succeeded**, or state drifts
   (ops lost, mask says "on").
4. **Bindings:** `TriggerDispatcher::SetActivity(new)` (exists).
5. **Layout (model lock):** new function (sibling of `ConfigApplyHome`, e.g.
   `ConfigApplyActivity`), pure and tested with `ImageStore`/`KeyLayout`. Order forced by
   `KeyLayout`: `SetPageCount(1 + n)` first (`SetKeys` rejects pages >= count; shrinking clears
   removed pages), then one `SetKeys` for pages 1..n (all 15 keys of each page, empty ones as
   `KEYDEF_EMPTY` so the old activity's keys are overwritten), after registering the new images
   with `AddExternal(offset, blob + offset, size)` (offset = pointer - blob base; the sample/test
   build is 32-bit so this works in host tests). Home (page 0) untouched.
6. **Screen/swipe (device layer, any task):** `swipe_set_page_count(1+n)` then
   `swipe_go_to_page(1, true)` (both queue to the swipe task in FIFO order, so count lands
   first). The pager pills follow the count; the compositor rebuilds changed pages (`COMP_FLUSH`).
   Page-changed callbacks are dropped while standalone (`PostPageChanged`), so no protocol work.
7. **Status text:** `status_set_left(displayName ?: name)` directly (the status task draws).
   Note `ph_set_status_text` in `host_link.cpp` is static; the device layer calls
   `status_set_left` itself.
8. **Gestures/HID:** keys held across the switch: a `hold` key pressed under the old activity
   gets its `release` looked up in the **new** bindings, no match -> HID key stays down. So the
   switch should `m_gestures`-cancel/`m_hid.ReleaseAll()` (same as `Arm()` does) or keep the old
   binding list for release events. Also `Key()` pressed-look is handled by the swipe task.
9. **Locking:** the executor runs under the standalone mutex and then takes `model_lock`
   (layout) -> order standalone -> model. The swipe task calls `standalone_key` (standalone
   mutex) from its key callback; need to confirm the swipe task never holds the model lock when
   it calls the key callback (CLAUDE.md notes the swipe task takes the model lock on key
   actions). A review/hardware check item, not yet verified.

Edge cases to cover in tests: switch to current (no ops); A->B->C queued back to back; hosted
takeover mid-switch (queued ops cancelled by `Arm()`, `m_devices_on` vs reality); queue full;
activity with fewer/more pages than the previous (page count grows and shrinks); activity
sharing an image with Home (same offset -> same ID, see Memory); out-of-range index (bake and
validator already prevent it).

### 5. Per-device state

- Spec: a 32-bit "on" mask in `Standalone` (CONFIG_MAX_DEVICES = 32), set when the ops are
  enqueued. All devices assumed **off at boot**.
- Fixed by spec: only changed devices run `turnOn`/`turnOff`; no power cycle between two
  activities sharing the TV.
- Unspecified: what happens to the mask on hosted takeover/release (`Arm()` re-arms; physical
  devices don't change), on a lost/dropped push, on a failed turnOn op (network down), and when
  the user presses a Home key meaning "everything off" (Q1, Q3, Q4).
- Net ops are fire and forget, with no confirmation, so the mask is an assumption, not truth.
- Device ops' order across devices follows the activity's `devices` array order? Spec says only
  "turnOn for devices the new activity needs"; I assume `devices` list order for on and old list
  order for off (flagged).
- Delays between ops are the author's job (a `delay` op in the hook/turnOn list); there is no
  "device ready" wait. A `delay` holds the queue, so touches queue behind a TV warm-up (global
  queue, decided already).

### 6. Memory and image store implications

- The blob is resident in PSRAM already (all activities' images are in it); nothing extra is
  copied on a switch. External images cost image-store **bookkeeping only** (entries in the
  store's list, internal heap), no pixel memory.
- Risk: `AddExternal` runs `check_image` per image (a full walk of each rle565 image, read from
  PSRAM) under the model lock, on a switch: up to 7 pages x 15 keys x 4 images. The blob was
  already validated at boot, so a no-check/trusted variant (or a precomputed per-activity ID list)
  would avoid the repeat cost. Needs timing on hardware; a hosted layout does the same on
  `SET_KEYS` so it is the same order of cost, but nothing measures it yet.
- Freeing the old activity's images: `KeyLayout` ref-counts slots, but external store
  **entries stay live until `Free(id)`** (never freed pixels). If entries are never freed, worst
  case 32 x 7 x 15 x 4 = 13,440 entries over a session of switches; realistic configs have far
  fewer distinct images (dedup by Buffer). If freeing, IDs shared with Home (or with the new
  activity) must be skipped, otherwise `Free` unregisters an ID a live key still uses (later
  `AddExternal` would then add a second entry). Options in Q5.
- `ImageStore::Reset()` on hosted takeover drops all externals, and release re-registers only
  Home: consistent with "no activity selected after release".
- PSRAM page buffers: three fixed; unaffected. Compositor needs the new pages built before they
  glide in view: same path as hosted `SET_KEYS` (changed pages flagged), hardware check that the
  glide to page 1 doesn't show stale/blank keys.
- `CONFIG_MAX_ACTIVITY_PAGES = 7` and `SCREEN_MAX_PAGES = 8` bound the strip.

### 7. Bake tool and docs touch points

Base feature needs **no** bake change (activity/device/hook resolution exists). Changes appear
only if we pick: a "none"/all-off target (op/schema/validator/tests/sample re-bake), name length
warning for the status bar, `activity.devices` ordering rules, an initial device state.
Docs to update (propose only, per Rule 4): `docs/standalone.md` (as built, plus any decided
changes), `docs/config-format.md` (if schema/ops change), CLAUDE.md (new modules, Open Items).
If the packed layout changes: re-bake `firmware/test/data/sample_config.bin`, regenerate
`config_types.gen.h` (`node gen-types.js` in `tools/bake`) and update the `config_blob`
validator.

### 8. Test plan shape (for the Plan phase; TDD, Rule 3)

Pure/host-testable: switch-sequence computation (all orderings, shared devices, from-none,
same-activity); mask commit-only-on-success; multi-list push; `ConfigApplyActivity` (pages,
images, page-count grow/shrink, shared image with Home, all-or-nothing); `Standalone` end to end
with fake hosts (key press -> switchActivity -> ops recorded in the right order, bindings
swapped, held-key release); hosted takeover/release clears the selection. The sample config
(`firmware/test/data/sample_config.json`) already has an activity "Watch" with a `didActivate`
op and touch[5,0] bound; it needs devices, hooks and a second activity with different page
counts for these tests.
**Not automatable (warn, Rule 3):** the real glide with the compositor (stale/blank keys, frame
lock/late fills during `AddExternal`), lock ordering between swipe and op tasks, device on/off
against real equipment, Wi-Fi-dependent turnOn ops.

### 9. Implementation notes (step 1, done)
- `OpQueue` (`op_queue.h/.cpp`) now stores `Entry{ops, count, slot, release}` per list; ring
  `OP_QUEUE_SIZE` = 16 (raised to 40 later, see section 13). The current list is `m_cur`/`m_cur_next`; `StartNext` walks it before
  popping the ring. The param pool is unchanged (a slot is refcounted by op count, one release per
  finished or cancelled op), so `param_pool.h` and its tests needed no change.
- `Count()` still means ops not yet started (rest of the current list included); new `Lists()` is
  the ring entries in use (the list being walked is not counted). `Dropped()` counts lists
  refused on a full ring. Empty lists succeed and take no entry.
- `trigger_dispatch.cpp` already pushed one list per binding, so it needed no change; nor did
  `standalone.cpp`, `op_registry`. The pushed op arrays must stay valid until run (they are in
  the resident blob).
- Tests added to `TestOpQueue.cpp` (ring = 16, 64-op lists take one entry, list walking, pending
  mid-list, releasing tag per op, cancel mid-list). Host tests: 445 passed, debug and release;
  debug device build 0 errors (first try).

### 10. Implementation notes (step 2, done)
- `op_queue.h`: new `OpListRef{ops, count, release, param}` and `OpQueue::PushLists(const OpListRef*, int n)`;
  `PushList` now builds one ref and calls it (single code path).
- Semantics: refused if ring entries in use + non-empty lists > `OP_QUEUE_SIZE`; `Dropped()` then
  grows by the number of non-empty lists refused (a refused single push still counts 1). Param slots are
  acquired for all lists first; if the pool fills, slots already taken in that call are released and
  nothing is queued (`ParamDropped()` +1 from the pool). Empty lists are skipped, take no entry or slot.
  The caller's refs array need only live for the call; the op arrays they point to must stay valid.
- Tests: six new Facts in `TestOpQueue.cpp` (order, empty skipped, exactly-fits/one-too-many with
  unchanged queue, empty lists not counted, per-list param/release and pool rollback, PushList == PushLists of one).
- Host tests: 451 passed, debug and release; debug device build 0 errors.

### 11. Implementation notes (step 3, done)
- Sentinel: `switchActivityOp.index == 0xFFFFFFFF` means "none". Named `ACTIVITY_NONE` in
  `tools/bake/src/consts.js` and `CONFIG_ACTIVITY_NONE` in `firmware/main/config_types.h`
  (no schema change, so `config_types.gen.h` was not regenerated). Config file form:
  `"activity": null` (the property is still required; a string is a normal name lookup).
- `ops.js`: `switchActivity` build rejects `gesture === "hook"` (already passed for device
  turnOn/turnOff and the four activity hooks, including through `use`; the error for a `use` is
  reported at `actions.<name>`). `config_blob.cpp` `Op()` accepts index < activity count or the sentinel;
  it does NOT reject switchActivity inside hooks (bake-only rule; the step 5 executor must not assume it).
- Sample config: Home key 8 is a color-only key with `switchActivity null`; Listen now has
  `devices: ["avr"]` and a `willActivate` delay. Watch (2 pages) vs Listen (1 page) already differ in
  page count. Re-baked `sample_config.bin` (45324 bytes). No existing host test needed changes.
- `TestConfigBlob.cpp`: `FindHomeOp` gained an `nth` argument.
- Tests: bake 135 pass; host 451 passed in debug and release (the new checks are inside existing
  Facts, so counts are unchanged); debug device build 0 errors first try.

### 12. Implementation notes (step 4, done)
- New `activity_switch.h/.cpp` (pure; in `CMakeLists.txt` and `test.mk.js`):
  `ActivitySwitchPlan(root, from, to, devices_on, ActivitySwitch &out)` -> `out.lists[]` (`OpListRef`,
  no release flag, no param), `out.count`, `out.devices_on`. -1 = none. False (empty plan) for an
  index outside the config; from == to is success with no lists.
- The plan said "max 6 refs": wrong, each device has its own turnOn/turnOff list. Max is
  `ACTIVITY_SWITCH_MAX_LISTS = 4 + CONFIG_MAX_DEVICES` (36). Real switches of 3+ devices need more than
  the (then) ring's 16 only at 12+ changed devices (the ring is now 40); a refused push (step 5) simply changes nothing.
- turnOff set: devices that are on, used by the old activity and not the new. To none: also every
  other device left in the mask (index order, after the old activity's). From none: stale devices in
  the mask are left alone (flagged). Device indices outside `devices_count` are ignored.
- `Standalone`: `Activity()`, `DevicesOn()`; `SelectActivity` now records the selection; `Arm()`
  sets it to -1; the mask is zeroed in `Init` only, so `SetHosted` keeps it (Q3). Nothing sets the
  mask yet: step 5 does (it needs a setter or to do it inside the executor, as a member).
- `activity_switch.h` includes `config_blob.h` just for `CONFIG_MAX_DEVICES`.
- The hook rule: the plan's lists can contain `switchActivity` ops (validator does not reject them);
  step 5's executor must cope (it only enqueues, so no recursion, but the plan is computed from the
  state at the time the op starts).
- Tests: new `TestActivitySwitch.cpp` (13 Facts on an in-memory config with all four hooks per
  activity, plus 2 Standalone state Facts). Host tests 466 passed, debug and release; bake 135 pass;
  debug device build 0 errors first try.

### 13. Implementation notes (step 5, done)
- `op_exec.h`: `ExecCtx::switch_activity(index, ctx)` returns null on success or an error string
  (index 0xFFFFFFFF is mapped to -1 by the executor in `op_registry.cpp`); a null function = the op
  does nothing (as before). The ctx is the `Standalone` (`this`), so no separate host struct is needed for it.
- `standalone.h`: new `ActivityHost{ctx, apply(index), glide()}` and `StandaloneHost::activity`
  (const pointer, may be null). `apply` = layout swap, page count, status text, glide to page 1
  (step 7 implements it); `glide` = the same activity chosen again. Neither can refuse. The device
  layer passes null for now, so `standalone_task.cpp` is unchanged.
- `Standalone::SwitchActivity(to)` (private, `standalone.cpp`): same activity -> `glide` only (and only for
  an activity, not none); else plan (`ActivitySwitchPlan`), `PushLists` at the tail, and ONLY on success:
  mask, `SelectActivity(to)` (dispatcher bindings), `m_gestures.Reset()`, `m_hid.ReleaseAll()`, then `apply`.
  New `GestureBank::Reset()` (Init calls it) clears held controls without firing; the key that triggered
  the switch is therefore forgotten, so its release does nothing.
- Refusal (ring full, bad index, hosted): executor returns Error with a string ("activity switch refused:
  op queue full"), so `OpQueue::Errors()`/`LastError()` count it and the op task already logs it
  ("op failed: ..."); `OpQueue::Dropped()` also counts the refused lists. Nothing changes, `apply` not called. Test: a
  full ring (switch + 15 taps) refuses Watch (3 lists).
- Ordering consequence of tail pushes (Q2): two switch keys pressed before the first runs start both
  switch ops before either one's device/hook ops (A's ops are appended behind B's switch op). The
  second is planned from the state the first committed, so the final mask and order of device ops is
  right; tested ("Queued Back To Back").
- A `switchActivity` inside a hook works (only enqueues; planned from the state when it runs); tested.
- Existing registry test used switchActivity as the "no executor" example: it now uses `script`.
- Sample config note: Home keys are numbered by array index, so the "none" key is key 9 (not 8 as in
  section 11); key 2 selects Watch.
- Lists per switch vs the 16 ring (FLAGGED QUESTION for the user, not a blocker): a switch needs
  (non-empty hooks, max 4) + (one list per changed device that has the on/off list). Typical: from
  none to an activity with 3 devices and one hook = 4 lists; A->B with 2 devices on, 1 off and two hooks = 5;
  home theatre with 6 changing devices and all hooks = 10. The ring is shared with everything else queued
  (each key press = 1 entry), so 16 is fine in normal use but a switch is refused if the ring already holds
  more than 16 - needed. The absolute worst is 36 (32 devices, 4 hooks). A refusal loses the whole switch
  (and the user sees nothing change besides the log). Recommendation: raise `OP_QUEUE_SIZE` to 40
  (about 16 bytes per entry, so ~400 bytes more of internal RAM; ring entries are `{ptr, count, slot, release}`)
  so any legal switch fits an idle queue; the executor and tests use the constant, so it is a one-line change.
  RESOLVED: the user approved; `OP_QUEUE_SIZE` is now 40 (~640 bytes of ring, 24 entries more than before
  = ~384 bytes extra internal RAM). Tests derive from the constant; `TestOpQueue` asserts 40, and the
  step 5 full-queue refusal test still fills the ring (switch + `OP_QUEUE_SIZE - 1` taps) and checks the refusal.
  Docs (`standalone.md`, `standalone-ops-plan.md`) updated.
- Tests: `TestStandalone.cpp` (+9 Facts, with a fake `ActivityHost` in `Rig`), `TestOpRegistry.cpp`
  (+2). Host tests 478 passed, debug and release; bake 135 pass; debug device build 0 errors first try.

### 14. Implementation notes (step 6, done)
- `config_apply.h/.cpp`: new `ConfigApplyActivity(root, blob, from, to, store, layout)` (`from`/`to` = activity
  index or -1 none; `root` = the validated blob relocated to `blob`, as `ActivitySwitchPlan` takes). The step 7
  host passes the `Standalone`'s old selection as `from` (-1 after a Reset/hosted release, since the store was reset).
- Flow: stage (register new images with the checked `AddExternal`, build `KeyDef`s for all 15 keys of pages 1..n, empty
  ones `KEYDEF_EMPTY`) -> `SetPageCount(1+n)` -> `SetKeys` -> free `from`'s IDs not in Home or `to`. A refusal
  happens only in staging (before any layout change), so it leaves store, layout and page count as they were; newly
  added IDs are freed again. IDs already live are skipped (no repeat `check_image`, no replace).
- DRY: `ConfigApplyHome` now uses the same `Stage`/`Unstage` helpers (template over an image getter); its
  stack arrays became heap `List`s (boot only). `PackedImage` reads a packed key's image via `ConfigKeyImageOffset/PtrOffset`.
- Trusted/no-check `AddExternal` variant: NOT added (flagged assumption). Cost only shows on hardware (step 9 d).
- Page count 1 + n is set even when n = 0 (none): `SetPageCount(1)`; no `SetKeys` call then.
- Tests: 7 Facts appended to `TestConfigApply.cpp` (the Fixture now also relocates the blob, `root`). I wrote the code
  before the tests (TDD order slip); the tests were then run and pass. Host tests 486 passed, debug and release;
  bake 135 pass; debug device build 0 errors first try.

### 15. Implementation notes (step 7, done; the device layer is NOT automatable)
- New pure `activity_host.h/.cpp` (`ActivityApplier`, in `CMakeLists.txt` and `test.mk.js`): implements
  `Standalone`'s `ActivityHost` over injected `ActivityScreen` calls (lock, unlock, set_page_count, go_to_page,
  set_status_left). `apply(index)`: lock, `ConfigApplyActivity(root, blob, from, index, store, layout)`, read
  `layout->PageCount()`, unlock; then (outside the model lock) `set_page_count`, `go_to_page(1, true)` (not for none),
  `set_status_left(displayName if non-empty else name; "" for none)`. `glide` = `go_to_page(1, true)`. The applier
  remembers `from` itself (Standalone's own selection is already the new one when `apply` runs, so it cannot be
  passed from there); `Reset()` sets it to -1. A refused layout (store full / bad image) bumps `Failures()` and
  leaves the screen alone (Standalone has already committed the selection and queued the ops: the cost of the host
  "cannot refuse" contract; the layout, store and page count are untouched).
- `standalone_task.cpp`: creates the applier with `model_lock/unlock`, `swipe_set_page_count`, `swipe_go_to_page`,
  `status_set_left`; `standalone_set_hosted` (both directions) calls `Reset()` since the protocol has reset the store.
  Lock order is documented at the top of that file. Status text cutting is left to `StatusBar::SetText`
  (UTF-8 safe, tested in `TestStatusBar`), so the applier passes the whole string (no second limit).
- Deadlock review (finding: NO deadlock): the swipe task never holds the model lock when it calls the key callback.
  `swipe.cpp` touches the model only via `model_key_active` and `model_set_key_pressed`, each of which takes and
  releases the lock inside the call; the router calls `key_active`, `key_look` and `key_event` one after another,
  never nested. The protocol task's `ResetModel` releases the model lock before `mode_changed` ->
  `standalone_set_hosted`. The compositor takes only the model lock. So the only nesting is standalone mutex -> model lock.
- Remaining (hardware / flagged): between `ResetModel` and `mode_changed(true)` in `OnTakeControl` the standalone side
  is still armed, so an op-task switch in that window would use a stale `from` (harmless: freeing unknown IDs is
  refused) and write the layout the host is about to own; `mode_changed(true)` cancels it afterwards. Not changed.
  The swipe event queue is 32 deep and drops silently when full; a switch sends two events.
- Tests: new `TestActivityHost.cpp` (7 Facts: call order lock/unlock then count, page, status; displayName vs name;
  none = count 1 + empty text + no glide; same activity only glides; images freed on swap; Reset after a model reset;
  nothing called while hosted). TDD slip: I wrote `activity_host.cpp` before the test file; the tests were run
  immediately after (one test bug fixed: it picked the wrong Home switch op). Host tests 493 passed, debug and release;
  bake 135 pass; device debug and release builds 0 errors, first try.

### 16. Implementation notes (step 8, done; docs are NOT tested)
- Updated `docs/standalone.md` (status, "Next", `config_types` instead of `config_layout`/`ConfigView`, 32-bit
  host tests, queue as list references in "Actions", a new "As built" block under "Devices and activity
  switching": plan, none target, hook rule, executor, held keys, mask rules, tail pushes, `ActivityApplier`,
  lock order, per-key `longPressMs` not built), `docs/config-format.md` (`switchActivity` null target,
  hook/device-list rejection, device order matters at run time, `delay` for warm-up), and
  `docs/standalone-ops-plan.md` (a status block at the top marking the step 1/2 offset-codegen text, the
  queue entry layout and "activities out of scope" as superseded). Docs are not checked by any test: please
  skim them. Not changed: the old milestone-plan prose in `standalone.md` that still names `ConfigView`
  (historical).
- Host tests re-run: 493 passed.

### 17. Step 9 hardware run (2026-11-06)
- **Config over the device limit:** the first `activity-test.mjs` had 34 devices; the device said "config rejected
  (over a limit): empty Home" (`CONFIG_MAX_DEVICES` 32). The bake doesn't check device limits yet (config-format.md
  says so), so it baked without complaint. Fixed in the config (28 dummy devices). Offered: bake checks for the
  device/activity/page/op-list limits (tests first).
- **OPEN, come back to it: slow pings with the rejected config.** While the over-limit config was stored (empty
  Home), pings to the device were often very slow, up to ~1 s. No code path found: a rejected config frees the blob
  and returns, the op task sleeps on `portMAX_DELAY`, and Wi-Fi doesn't depend on the config. Not yet known whether it
  was the config or a coincidence (shortly after the upload restart, router, RF). Next: ping with the good config; if
  fine, re-upload a deliberately rejected blob to try to reproduce it; if it comes back, a `-DCB_WIFI_DIAG=ON` build.
- **FIXED (verified on the hardware: the step 9 checklist passed with it): watchdog during a `delay`.** `Watch Listen Lights` tripped the task watchdog
  (IDLE0 starved, the `ops` task running `delay_poll`) during the projector's 6 s warm-up. Cause: the tick is 100 Hz,
  so `pdMS_TO_TICKS(OP_BUSY_POLL_MS = 5)` is 0 and the op task spun through every `delay` without blocking.
  `standalone_task.cpp` now waits at least one tick (`OP_BUSY_POLL_TICKS`). Device layer only: not host testable.
  Every other `pdMS_TO_TICKS` in `main/` is 10 ms or more.
- **OPEN: lasting corruption after the watchdog.** After the watchdog fired, the left ~1/8 of the screen had
  corrupted pixels from then on (the screen also jumped). Likely the watchdog ISR's backtrace print on core 0 stalled
  the bounce fill, and the RGB/DMA stream didn't resync (`LCD_RGB_RESTART_IN_VSYNC` should limit a late fill to one bad
  frame). With the spin fixed this shouldn't recur, but a long ISR stall leaving the panel shifted for good is worth
  checking separately.
- **Logging added for the run:** `standalone_task.cpp` logs `activity: <old> -> <new>` when a switch commits (or a
  takeover/release drops it) and `op queue empty (busy N ms)` when the queue drains; the touch (`main.cpp`) and
  `swipe: page a -> b` lines are now `ESP_LOGD` (compiled out at `CONFIG_LOG_MAXIMUM_LEVEL=3`).
- **Result (2026-11-07, user): every check a-j passed** with the watchdog fix. Fill times (release, stats log): ~137 µs
  typical, under 200 µs during a switch, 200-250 µs during a swipe (budget 615), so the `check_image` cost under the
  model lock is fine and no timing log is needed. Not tested: real equipment (g3); the user marked the job complete.
- Follow-ups outside this job: a busy indicator on the device while the op queue runs (the user's idea, after the
  slow switches); bake checks for device limits; the two OPEN items above. The suspected display problem on the
  gradient keys turned out to be plain RGB565 gradient banding (flat faces look perfect; `FLAT` in
  `activity-test.mjs`), so its IPI doc was deleted.

## CLAUDE.md proposals (propose only; CLAUDE.md NOT edited, awaiting the user's confirmation)
1. **Firmware module list**, add entries for: `activity_switch.cpp/.h` (pure switch plan: list refs in spec
   order + new device mask), `activity_host.cpp/.h` (`ActivityApplier`: layout swap under the model lock,
   then page count, glide and status text), `config_types.h` / `config_types.gen.h` (binpack-generated
   structs, limits, `CONFIG_ACTIVITY_NONE`), `op_exec.h` (executor protocol, `ExecCtx`), `op_registry.cpp/.h`
   (op table), `param_pool.h` (trigger `param` slots), `net_host.h`, `net_queue.cpp/.h`, `exec_net.cpp/.h`
   (net ops over an injected `NetHost`), `wol_packet.h`, `net_worker.cpp/.h` (device layer).
2. **`op_queue` entry**: the queue holds list references (ring of 16, `PushLists` all or nothing), not
   expanded ops.
3. **Remove** `config_layout.h`, `config_view.h`/`ConfigView` and the "walks it by offsets rather than casting
   to structs" wording in the `config_blob` entry and the `trigger side` entry: `config_blob` reads through
   the generated structs; after a schema change run `node gen-types.js` in `tools/bake` as well as re-baking
   the sample.
4. **Host tests**: built 32-bit (`platform: "x86"` in `test.mk.js`) so blob structs match the device; the
   "64-bit" remark goes.
5. **Build switches**: `-DCB_STATS_LOG=ON` (stats log with net counters) and `-DCB_WIFI_DIAG=ON` (Wi-Fi
   diagnostics), both in `main/CMakeLists.txt`; presets `debug-iram` / `release-iram`
   (`sdkconfig.wifi_iram`, Wi-Fi in IRAM, not the default build).
6. **Standalone description and Open Items**: update the "Standalone" operating-mode bullet (activity
   switching, devices and HTTP/UDP/WoL are built and host tested, not yet run on the hardware; scripts
   still not built; the link to `standalone-ops-plan.md`). Add Open Items: activity switching hardware
   checks (step 9 list), net ops unverified on hardware, `check_image` cost per switch under the model
   lock, the takeover-window race noted in section 15, and "per-key `longPressMs` for activity pages not
   built (dropped)". Remove "Held keys are dropped silently on takeover" sentence if superseded (the switch
   and takeover both release HID keys).

## Open design questions (1-3-1)

None is blocking, but Q1-Q4 change what gets built; recommendations are my picks and are also
listed under Flagged assumptions where the Plan would otherwise need to guess.

**Q1. How does the user turn everything off / leave the activity?**
- Problem: swiping back to Home leaves the activity (and its devices) selected; the spec has no
  way to deselect, so devices can never be powered off by the device itself.
- Options: (a) allow `switchActivity` to a "none" target (op index sentinel / bake accepts
  `"activity": null` or `"none"`): powers off all devices that are on, runs the old activity's
  will/didDeactivate, collapses the strip to Home; (b) a separate op (`allOff`); (c) nothing: the
  user makes an "Off" activity with no devices and `turnOff` hooks.
- Recommendation: (a). One op, no new concept, reuses the same sequence logic; (c) is a
  workaround that still leaves an "activity" selected. Needs a validator/bake change.

**Q2. Where in the queue do the switch ops go relative to the rest of the triggering list?**
- Problem: `switchActivity` is one op in a list (e.g. `[switchActivity, httpPost]`); the switch
  sequence can be appended at the tail (after `httpPost`) or inserted straight after the
  switchActivity op.
- Options: (a) tail of the queue (simple, matches the script plan "pushes land after");
  (b) front, right after the switchActivity op; (c) tail but run the rest of the triggering
  list first only for the same trigger.
- Recommendation: (a). The ring is FIFO today; a front insert needs a new queue operation, and
  authors can put switchActivity last in their list.

**Q3. What happens to the device on/off mask on hosted takeover, release and reboot?**
- Problem: `Arm()` re-arms everything, but physical devices keep their state; the mask is only
  an assumption.
- Options: (a) keep the mask across takeover/release (devices don't change), clear on reboot;
  (b) clear to "all off" on every re-arm; (c) treat as unknown and re-send turnOn for what the
  next activity needs.
- Recommendation: (a) for takeover/release, all off at boot (document that a toggle-style
  power command can drift; fixing it is user-driven by selecting the activity again after an
  Off). Test: mask survives `SetHosted`.

**Q4. A lost or failed push (queue full, ops cancelled by takeover, a failing turnOn op).**
- Problem: the mask is updated at enqueue; if the ops don't run, the mask is wrong.
- Options: (a) commit selection/mask only if the whole sequence was queued (reject the switch
  otherwise and log), accept drift for failures/cancels; (b) also roll back the mask on Cancel;
  (c) update the mask when each op completes (contradicts the spec's "enqueue" decision, which
  exists so A->B->C queued quickly stays right).
- Recommendation: (a) plus Q3(a). Keep the spec's enqueue rule.

**Q5. Image store entries for activity images.**
- Problem: `AddExternal` entries persist until `Free`; sharing with Home complicates freeing.
- Options: (a) register at switch, free the old activity's IDs that neither Home nor the new
  activity uses; (b) register at switch, never free (bounded bookkeeping); (c) register all
  activities' images at boot.
- Recommendation: (a), implemented in the pure `ConfigApplyActivity` and tested; (b) is the
  acceptable fallback if (a) proves fiddly. (c) wastes entries and makes the boot all-or-nothing
  bigger.

**Q6. Multi-list push and queue size.**
- Problem: ring is 32 entries, lists can be 64 ops; a six-list switch can never fit when large.
- Options: (a) add an all-or-nothing multi-list push and keep 32 (drop and log oversized
  switches); (b) raise `OP_QUEUE_SIZE` (RAM: entries are small) and add the multi-list push;
  (c) cap hook list length in the bake/validator to something that fits.
- Recommendation: (b) modestly (e.g. 64 or 128) plus the multi-list push; changing the ring size
  needs `TestOpQueue`/`TestTriggerDispatch` adjusted.

**Q7. Held keys across a switch.**
- Problem: see 4.8; release events after a switch no longer match the old activity's bindings.
- Options: (a) cancel gestures and `ReleaseAll()` HID on every activity change (like `Arm()`,
  without clearing the queue); (b) resolve `release` against the binding list that handled the
  press; (c) ignore.
- Recommendation: (a): simplest, safe, nothing sticks. A held key is lost on a switch, which is
  rare (a switch normally comes from a tap).

**Q8. (DROPPED, out of scope) Per-key `longPressMs` for activity pages.**
- Problem: `Standalone::KeyLongPressMs` only reads Home's.
- Options: (a) read it from the selected activity's page keys (needs `m_activity`);
  (b) leave at the default.
- Recommendation: (a), a small change in the same step as selecting an activity.

**Q9. Switch started from a script or a hook (`switchActivity` inside hooks).**
- Problem: a `switchActivity` op inside `didActivate` etc. would recurse.
- Options: (a) allow, queue-based (no recursion since it enqueues); (b) bake rejects
  `switchActivity` in hook/device lists; (c) leave as is.
- Recommendation: (b) for now (bake error, nothing in the validator) unless the user wants
  chaining; Lua is out of scope.

## User decisions (recorded in the Plan phase)
- Q1-Q5 and Q7-Q9: the recommendations are approved as written. Q1: `switchActivity` may target
  "none" (validator and bake change). Q2: switch ops go at the queue tail. Q3: mask kept across
  takeover/release, all off at boot. Q4: commit selection and mask only if the whole sequence
  was queued. Q5: register at switch, free the old activity's unshared IDs. Q7: cancel gestures
  and `ReleaseAll()` on a switch. Q8: **dropped (user, after planning):** no per-key
  `longPressMs` for activity pages; the one global setting is enough, and it is out of scope for
  this job. Q9: bake rejects `switchActivity` inside hooks and device lists.
- **Q6 decided as option three, changed from the recommendation:** the op queue holds LIST
  REFERENCES (pointer, count, release flag, param slot) instead of expanding every op into the
  ring. Small fixed ring (~16 entries). The executor walks the current list op by op. A switch
  is up to 6 list entries pushed all-or-nothing. (This supersedes the "raise OP_QUEUE_SIZE" idea;
  `delay` still holds the queue.)
- All Flagged assumptions below are approved as written.

## Flagged assumptions
- Step 4: turning off on a switch to none includes devices in the mask that the old activity did not
  use; switching from none leaves stale mask devices on; turnOff only runs for devices whose mask bit
  is set; the switch needs at most 36 list refs (not 6).
- Step 3: the "none" target is `"activity": null` only (not the string "none", which could clash
  with an activity of that name); sentinel 0xFFFFFFFF.
- Step 6 (page swap): register activity images with the normal checked `AddExternal`
  (which runs `check_image` over each image); add an unchecked variant only if hardware timing
  shows a need.
- Device on/off order: `turnOn` for devices in the new activity's `devices` array order;
  `turnOff` in the old activity's array order.
- Switching to the already selected activity only glides to page 1 (as the spec says) even if
  the user is on a later page of it.
- Swiping back to Home does not deselect or power anything (spec).
- Device state is a 32-bit mask in `Standalone`, all off at boot, kept across hosted takeover
  (Q3 recommendation).
- Selection and mask are committed only when the whole switch sequence was queued (Q4).
- Activity status text uses `displayName` else `name`, cut at 47 bytes by the status bar.
- No new protocol messages are needed (PAGE/KEY events exist only hosted; standalone page
  changes need not be reported).
- Existing hosted-takeover behaviour (Reset, no activity) already matches the spec; unchanged.
- The net ops, which turnOn/turnOff mostly use, are unverified on hardware; this job's hardware
  checks depend on that testing.
- The Plan should carry a "TDD first" test for every step and list the hardware-only checks in
  section 8.

## Plan
Rules for every step: TDD (write or adjust the named tests first, see them fail, then code);
DRY (grep for existing helpers before adding; refactor rather than duplicate); the step must
leave the host tests (`mk run` in `firmware/test`, debug and release test counts equal),
`tools/bake` `npm test` (when touched) and the device debug build (and release build where
marked) at 0 errors. Steps marked **[HW]** need the user to verify on hardware after the step;
the agent flags it but carries on. No commits unless asked. Only PROPOSE CLAUDE.md changes.

- [x] 1. **Op queue holds list references.** Tests first: rewrite/extend `TestOpQueue.cpp` and
  `TestTriggerDispatch.cpp` for entries of (ops pointer, count, release flag, param slot), ring
  of ~16 (constant in `op_queue.h`), the executor walking the current list op by op, `delay`
  holding the queue mid-list, `Releasing()` tag still correct, `Cancel`/clear dropping the
  current list and the ring, full ring refusing a push. Then change `op_queue.cpp/.h`
  (stop expanding ops), `trigger_dispatch.cpp` (push one reference per matched binding) and
  `standalone.cpp` callers; keep the `run` callback contract and `param_pool` slot release
  semantics (check `TestParamPool.cpp`, `TestStandalone.cpp`, `TestExecNet.cpp` still pass).
  Grep for every user of the old `PushList`/entry layout first (DRY). Build: host tests + device.
- [x] 2. **All-or-nothing multi-list push.** Tests first (`TestOpQueue.cpp`): push of 1..6 list
  references succeeds only if all fit (ring of 16 with 11 used refuses a 6-list push and leaves
  the queue unchanged, 10 used accepts it); empty lists (count 0) are skipped and do not use a
  slot; order preserved; param slot refcount/release correct when refused. Then add
  `PushLists(refs[], n)` (single code path; make single `PushList` a call into it). Build.
- [x] 3. **Bake/schema/validator: "none" target and hook rejection.** Tests first, in
  `tools/bake/test` (resolve/ops/schema tests, e.g. `types-header.test.js` and the resolve tests)
  and `firmware/test/TestConfigBlob.cpp`: (a) `switchActivity` accepts `"none"` (or `null`) and
  packs a sentinel index (decide the sentinel in the test, e.g. 0xFFFF/uint max, and document it
  in `config_layout`/`config_types.h`); validator accepts the sentinel and still rejects any
  other index >= activity count; (b) bake errors on `switchActivity` inside any hook
  (`willActivate` etc.) or device `turnOn`/`turnOff` list, with a clear message naming the
  path. Then change `tools/bake/src/{ops,resolve,schema}.js`, regenerate types with
  `node gen-types.js` (`config_types.gen.h`), update `config_blob.cpp` validator and
  `config_types.h` if the sentinel is named there, add a "none" key and a second activity with
  different page counts, devices and hooks to `firmware/test/data/sample_config.json`, re-bake
  `sample_config.bin` (`node cli.js ../../firmware/test/data/sample_config.json -o
  ../../firmware/test/data/sample_config.bin` from `tools/bake`), and fix any host test that
  depends on the old sample (record what changed in Notes). Build: bake `npm test`, host tests,
  device.
- [x] 4. **Pure `Standalone` state and switch-sequence computation.** Tests first (new
  `TestActivitySwitch.cpp`, added to `test.mk.js`, using the extended sample config): a pure
  function `(old, new, deviceMask, root view) -> up to 6 list refs in the spec order
  (old.willDeactivate, new.willActivate, turnOn for new-needed devices not on in `devices`
  array order, turnOff for old devices the new lacks in old array order, new.didActivate,
  old.didDeactivate) + new mask`. Cases: from none, to none (all on devices off, strip collapses),
  A->B sharing a device (no power cycle), B->A, same activity (no lists), device already on,
  devices on from an earlier switch kept across none, empty hooks skipped, max 6 refs.
  Then add the function (own file, platform independent, e.g. `activity_switch.cpp/.h`, no
  device headers) and `Standalone` members `m_activity` (-1 none) and `m_devices_on` plus
  getters. Do not wire the executor yet. Build.
- [x] 5. **`ActivityHost` interface and the `OP_SWITCH_ACTIVITY` executor.** Tests first
  (`TestStandalone.cpp`, `TestOpRegistry.cpp`, with a fake `ActivityHost` recording calls): a key
  bound to `switchActivity` -> the executor computes the sequence, pushes it with the step 2
  all-or-nothing push at the tail (Q2), and only on success commits `m_activity`/mask, calls
  `TriggerDispatcher::SetActivity`, cancels gestures and `m_hid.ReleaseAll()` (Q7), and calls the
  host. Host interface (injected in `StandaloneHost`, next to `HidSink`/`NetHost`): at least
  `ApplyActivity(index)` (layout, status, page count, glide) and the "none" form; check call
  order and that a refused push changes nothing (queue full test, mask unchanged, host not
  called); same activity -> only a glide call and no ops; recorded op order with fakes matches
  the spec incl. A->B->C queued back to back; `SetHosted` keeps the mask (Q3) but clears the
  selection and drops queued ops; hosted release leaves no activity. Then implement
  (`op_registry.cpp` entry, `ExecCtx`, `standalone.cpp/.h`). Device build compiles with a stub
  host in `standalone_task.cpp` (no behaviour yet). Build.
- [x] 6. **Activity page swap in `config_apply` + image store register/free.** Tests first
  (`TestConfigApply.cpp` or the existing config-apply test file; with `ImageStore`/`KeyLayout`
  from `store_env.h`): new `ConfigApplyActivity(index or none)` is pure and all-or-nothing:
  `SetPageCount(1+n)` then one `SetKeys` for pages 1..n (all 15 keys, empty ones overwritten so
  the old keys never leak); register new images with `AddExternal` (offset = pointer minus blob
  base) skipping IDs already registered (shared with Home or the old activity); free the old
  activity's IDs that neither Home nor the new activity uses (Q5); grow and shrink page counts;
  "none" collapses to 1 page and frees all activity-only IDs; failure (bad image, store full)
  leaves layout, store and page count untouched; Home page 0 and its images never change;
  repeated A->B->A leaves entry count stable (no leak); deferred free respects keys still in use.
  Then implement in `config_apply.cpp/.h` reusing `ConfigApplyHome`'s helpers (DRY: factor out
  the shared key-to-keydef and register code first, keep Home tests green). Decide (record in
  Notes) whether `AddExternal` needs a trusted/no-`check_image` variant: only add it if a test or
  the HW timing step in 9 shows a need (flag as assumption: use the checked path first). Build.
- [x] 7. **Device layer wiring (model/swipe/status, lock order).** Not fully automatable: warn.
  Tests first for what is pure: extend `TestStandalone.cpp` with a fake `ActivityHost` to check
  the exact sequence of host calls (page count before go-to-page, status text = `displayName` else
  `name`, empty for none, text cut length at the status bar maximum) and that nothing is called
  while hosted. Then implement the real `ActivityHost` in `standalone_task.cpp` (or a small
  `activity_host.cpp` if that file is too big): take `model_lock` for the layout apply, call
  `swipe_set_page_count(1+n)` then `swipe_go_to_page(1, true)`, `status_set_left(...)`. Document the
  lock order (standalone mutex then model lock) at the top of the file, and review that the swipe
  task never holds the model lock when it calls the key callback (record the finding in Notes;
  if it can, stop and use the 1-3-1 format). Build debug and release for the device. **[HW]**
- [x] 8. **Docs.** No tests (documentation); warn that docs are not checked. Update
  `docs/standalone.md` (as built: reference-based queue, switch sequence, "none" target, hooks may
  not switch, mask rules, held keys, lock order), `docs/config-format.md` (the `"none"` target,
  the hook rejection, ordering of device lists), `docs/standalone-ops-plan.md` where it mentions
  the queue or activities. Add a "CLAUDE.md proposals" section to this document (propose only, do
  not edit CLAUDE.md): new modules (`activity_switch`, the `ActivityHost`), the queue now holding
  list references, Open Items for the hardware checks below, the "Next" order in
  `docs/standalone.md` changes. Build unaffected; run host tests once more.
- [x] 9. **Final hardware test checklist for the user. [HW] (not automatable, flagged per Rule 3).**
  Write the checklist into this document (agent does not run it) and ask the user to run it:
  (a) boot to Home, press an activity key: pages 1..n appear, glide to page 1 with no stale or
  blank keys, status text shows the name, pager pills match; (b) swipe back to Home and select a
  second activity with a different page count (grow and shrink), then "none"; (c) the same
  activity key twice only glides; (d) frame lock: late-fill counts in the stats log during a
  switch, `check_image` cost under the model lock, release build; (e) held key during a switch
  leaves no stuck HID key; (f) A->B->C pressed quickly: ops run in order, queue ring full case
  logs and does nothing; (g) device turnOn/turnOff with real equipment or a UDP/HTTP listener
  (the net ops and Wi-Fi work on the hardware since 2026-11-06), shared device not power cycled, long `delay`
  warm-up holds touches; (h) hosted `TAKE_CONTROL`/`RELEASE_CONTROL` mid-switch: Home only, no
  activity, mask kept; (i) swipe task and op task lock-order soak (no deadlock, no watchdog) with
  repeated rapid switches; (j) `RUN_STRESS` with a stored config containing activities. Results
  go in Notes; failures become new steps or a 1-3-1.

### Step 9 checklist (for the user to run)

**Setup.**
- Config: `tools/bake/examples/activity-test.mjs` (bakes to ~290 KB). Upload from `tools/bake`:
  `node cli.js examples/activity-test.mjs --upload`. Change `SERVER` at its top if the PC's IP isn't 10.1.1.126.
- Run `python scratch/net_test_server.py` on the PC. Every hook and device op prints a line: UDP messages
  (`UDP :9999 ... b'tv on'`) or a GET (`/get?avr=on`).
- Watch the UART log (COM3). A release build with `-DCB_STATS_LOG=ON` gives the once-a-second late-fill counts for (d).
- Activities: **Watch** (3 pages, devices tv + avr, status "Watch TV"), **Listen** (1 page, avr + light),
  **Lights** (2 pages, light + projector; the projector's turnOn holds the queue for 6 s), **Big** (1 page, 28
  dummy devices). Each activity's keys are labelled `W p1 k0` and so on, so a stale or blank key shows up.
  Row 3 of every activity's page 1 is the switch row: Watch, Listen, Lights, None, Big.
- Home: row 1 = the switch row; row 2 = `Watch Listen Lights` (three switches in one tap), `Hold a (no
  switch)`, `Hold a long: Listen`, `Big in 5 s`, `Noop`; row 3 = `Ping Home`.

**Expected server lines per switch** (all four hooks print, in this order):

| Switch | Lines |
|---|---|
| none -> Watch | Watch willActivate, tv on, avr=on, Watch didActivate |
| Watch -> Listen | Watch willDeactivate, Listen willActivate, light on, tv off, Listen didActivate, Watch didDeactivate (no avr: shared) |
| Listen -> Lights | Listen willDeactivate, Lights willActivate, projector on, (6 s), projector warm, avr=off, Lights didActivate, Listen didDeactivate (no light: shared) |
| Lights -> none | Lights willDeactivate, light off, projector off, Lights didDeactivate |

**Checks.**
- [x] a. Boot to Home, tap **Watch**: the screen glides to page 1 with all 15 `W p1` keys right (none blank or
  left over from Home). Status "Watch TV", 4 pager pills. Swipe through `W p2` and `W p3`. Server: row 1 of the table.
- [x] b. Swipe back to Home, tap **Lights** (3 pages to 2), then **Listen** (2 to 1), then **Watch** (1 to 3),
  then **None** (Home only, 1 pill, status text empty). Keys and pills right each time. The server lines follow the
  table's pattern: old willDeactivate, new willActivate, turnOns, turnOffs, new didActivate, old didDeactivate.
- [x] c. Tap **Watch** twice: the second tap only glides to page 1. Swipe to `W p3` and tap the Watch key in the
  switch row: glide back to page 1, and no new server lines.
- [x] d. Release build with stats: count late fills during the switches in a and b (expect 0). Watch for a stutter
  in the glide. The `check_image` cost under the model lock isn't measured anywhere: if the glide stutters or fills go
  late, a timing log around `ConfigApplyActivity` is the next step.  (Results: 137 typical, <200 during activity 
  switch 200-250 during swipe, budget 615)
- [x] e. Held HID key. Open Notepad on the PC.
  1. Home `Hold a (no switch)`: hold it for 2 s. `aaaa` repeats until you lift. This is the control.
  2. Home `Hold a long: Listen`: hold it. `a` repeats; at 1.5 s the switch to Listen happens and the repeat stops
     (`ReleaseAll`). Lift: nothing more is typed.
  3. In Watch, page 1 key 9 `Hold a long: Lights`: hold it. At 1.5 s Lights' page appears under your finger with
     `Must not fire` there. The repeat stops; lifting must not print `Lights k9 tapped`.
  4. While holding `Hold a (no switch)`, swipe away: the repeat stops (release on swipe).
- [x] f. Rapid switches and a full queue.
  1. From Home with none selected, tap `Watch Listen Lights`: Lights ends up selected (2 pages, status "Lights").
     Server: the three table rows (none -> Watch, then Watch -> Listen, then Listen -> Lights), complete and in
     order. (All three switches run before any of their device ops. That is by design, so the lines come after.)
  2. On an activity's page 1, tap Watch, Listen, Lights in the switch row as fast as you can: same result and order.
  3. Queue full: tap None, then `Big in 5 s`, then tap `Noop` 12 or more times inside the 5 s. The UART log shows
     `op failed: activity switch refused: op queue full`, and the screen and status don't change. (From no activity
     the switch needs 30 entries, 2 hooks + 28 devices; 11 or more queued Noops leave too few of the 40.)
  4. Then tap `Big` with nothing queued: it switches (1 page `B p1`), so 30 lists fit an idle queue. Tap None after.
- [x] g. Devices and warm-up.
  1. Shared devices aren't power cycled (avr on Watch -> Listen, light on Listen -> Lights: see the table).
  2. Switch to Lights, then tap `Ping Lights` 3 times during the 6 s warm-up. The pings print only after
     `projector warm`, and only then `avr=off` etc. Touch feedback (pressed look, swipes) still works during the wait.
  3. Optional: real equipment instead of the test server. (not tested yet)
- [x] h. Takeover in the middle of a switch.
  1. From None, tap Lights, and while the projector warms up run `node scratch/takeover.mjs 3` (from the repo
     root). Server: `Lights willActivate`, `light on`, `projector on`, then nothing more: `projector warm` and
     `Lights didActivate` are dropped with the queue.
  2. After the release: Home only, no activity selected (1 pill, status empty).
  3. Tap Lights again: only `Lights willActivate` and `Lights didActivate` print, with no `light on`/`projector on`.
     The device mask was kept, so light and projector count as already on.
  4. The narrow race (a switch on the op task writing the layout while the host takes over) can't be hit by hand.
     Look out for odd keys straight after a takeover.
- [x] i. Soak: for a minute or two, mix rapid switch-row taps, `Watch Listen Lights` taps, swipes and pings
  (`Ping Watch` etc.). No hang, no watchdog reset or panic in the log, and the screen keeps up. Some
  `op queue full` refusals are fine.
- [x] j. `RUN_STRESS` (`-DCB_RUN_STRESS=ON`, release) with this config stored: 0 late fills, as before. Afterwards
  the device is empty: reboot to get the config back.
