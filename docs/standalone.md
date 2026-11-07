# Standalone mode

Mostly design. Built so far: the bake tool, the layout upload, loading the stored layout's Home at boot, take control / release, and touch-to-op wiring (see Status). Key presses run the op queue on the device, and `hidKey` ops send USB HID keyboard reports (milestone step 3).

**Status:** the design below is settled except for the items under "Still open". **Built:**
- The bake pipeline (`host/src/bake/`: stage 1 resolve, stage 2 render, stage 3 pack, plus the upload; the CLI is `cli/cli.js`), specified by [`layout-format.md`](layout-format.md); and the layout upload to the `layout` partition ([`protocol.md`](protocol.md#layout-upload)).
- The boot loader, host tested (build order step 4): `layout_blob` (validate and relocate), `layout_apply` (Home onto the key layout), `ImageStore::AddExternal`, `LayoutStore::Read`. The device layer `layout_boot.cpp` reads the stored blob into PSRAM at boot and shows Home's keys. Works on the hardware (a flashed layout loads). Home's keys show their images; pressing does nothing.
- `binpack/` fixes found on the way: `finalize()` pads to 4 bytes, one Buffer can be shared by two fields, and a shared Buffer first referenced early and again from a later nested reference (an image used by Home and by an activity page) now gets a pointer both times (it used to be null the second time; test added in `binpack/test.js` and `host/test/bake/pack.test.js`).
- The trigger side, host tested and wired to the device by milestone step 2 (works on the hardware): `layout_types.h` / `layout_types.gen.h` (the binpack-generated C structs and op codes, shared with the validator; they replaced an earlier hand-written struct header and view class), `op_queue` (a ring of 40 list references: see "Actions"; one op started per `Poll()`, `delay` holds the queue, a list that doesn't fit is dropped whole and counted), `trigger_dispatch` (`Trigger`, `HasBinding`, `SetActivity`: exact match, Home's bindings then the selected activity's), and `key_gesture` (`GestureBank`: press, release, tap and long per control; `TouchControlName(page, key)` gives `touch[col,row]` across the page strip). The queue passes every op, `delay` included, to an injected `run` callback, where the executors plug in. `param` is accepted by `Trigger` but not carried through the queue until script ops exist. Neither class locks: the device layer must serialise `Trigger`, gestures and `Poll`.

- Take control and release (milestone step 1), host tested, **works on the hardware** (checks below passed): `TAKE_CONTROL` (`0x06`) / `RELEASE_CONTROL` (`0x07`), `NOT_HOSTED` (status 11) for screen requests while standalone, `GET_INFO` `flags` bit 2, `Protocol::SetHome` / `Hosted` / `PostDisconnect` / `ServiceDisconnect` (the one `Release()` path), `ProtocolHost::mode_changed`. `usb.cpp` reports unmount and suspend (`CONFIG_TINYUSB_SUSPEND_CALLBACK=y`), and `host_link.cpp` posts them. `host/` has `takeControl()` / `releaseControl()` and `info.hosted`; the demos take control first. In standalone, key events go to the gestures and bindings (step 2); while hosted they go to `Protocol::PostKey`.

- Activity and device management (job `wip/complete-activity-device-management.md`), host tested and **run on the hardware** (2026-11-07, against a UDP/HTTP test server; real equipment not yet tried): the `switchActivity` op (including the "none" target), device on/off tracking, the switch sequence, the page swap, glide and status text. See "Devices and activity switching" below.
- The net ops (`httpGet`, `httpPost`, `udpPacket`, `sendWol`) work on the hardware, and so do device `turnOn`/`turnOff` lists made from them (with activity switching).
- The `script` op (Lua 5.4.7), host tested, **not yet run on the hardware** (job `wip/complete-lua-scripts.md`): `script_host`, `script_api`, `lua_heap`. See "Actions" below; the script API is in [`layout-format.md`](layout-format.md#script-api).

**Next:** hardware checks of the Lua scripts (see CLAUDE.md, Open Items). Freeing the blob on a hosted takeover is a later optimisation.

## Plan: first standalone milestone

**Goal:** the device boots into standalone mode with its flashed Home, pressing a Home key sends a USB HID keyboard key to the PC, and a host can take control (mode 1, as today) and give it back, after which the device is standalone again. **Out of scope for the milestone:** activity switching (a layout for testing has Home plus at most one activity that is never selected, or none), devices, `switchActivity`, scripts, HTTP/UDP/WoL, the macro pad and encoder.

Order, each step test first (host tests where the code is pure; hardware-only parts are flagged and listed for you to run):

1. **Take control and release (protocol, firmware, `host/`, docs). Done; hardware checks below.**
   - Messages: `TAKE_CONTROL` and `RELEASE_CONTROL` (codes to pick from the unassigned range in `protocol.md`; `0x04` is reserved). The mode is reported in GET_INFO `flags`, and a PAGE/KEY event stream only exists while hosted.
   - Boot state is standalone. `TAKE_CONTROL` resets the model exactly as RESET does (no keys, images freed, status text cleared) and switches key events to `Protocol::PostKey`. The layout blob stays resident in PSRAM for now (freeing it on takeover is the later optimisation).
   - `RELEASE_CONTROL`, USB unmount and USB suspend all go through one `Release()` path: cancel anything in flight, rebuild Home from the blob (`LayoutApplyHome` after the reset, with no activity selected) and switch key events to the standalone path. A heartbeat timeout for hung hosts is later.
   - Requests that define the keys (`SET_KEYS`, `SET_PAGE_COUNT`, `SET_PAGE`, `PUT_IMAGE`, `FREE_IMAGES`, `RESET`, `SET_STATUS_TEXT`) are refused with an error status while standalone (decided: no implicit takeover), so a host can't half-edit Home by accident and must send `TAKE_CONTROL` first. Always allowed: PING, GET_INFO, QUERY_IMAGES, backlight, layout upload, Wi-Fi.
   - **Backlight decision:** `SET_BACKLIGHT` stays allowed in both modes (it is device state, not keys, and a tool may want the screen dark). Because a dark standalone device would have no host to answer the `WAKE` events, releasing control, a USB unmount or suspend, and a disconnect while already standalone all turn the backlight on. Documented in `protocol.md` and `backlight.md`.
   - **Done as:** the mode lives in `Protocol` (atomic, readable from any task). A disconnect is posted from the TinyUSB task and serviced on the protocol task (`ServiceDisconnect`, after the bytes read before it), which also resets the stream state like `RESET_STREAM`; `Release()` rebuilds Home with `LayoutApplyHome` from the summary and blob given to `SetHome`. `PostKey` and `PostPageChanged` drop their events while standalone, and `host_link.cpp`'s `on_key` is the one switch point for step 2.
   - **Hardware checks for step 1:** a cold boot shows Home and the demo refuses until `takeControl()`; the demo (`node examples/demo.js`) and `examples/bench.js` work end to end; Ctrl+C in the demo, unplugging the native USB cable and putting the PC to sleep (suspend) each bring Home back with the light on; re-plugging and taking control again works; check that the suspend callback does not fire spuriously while the host is simply idle (a false release would pull the screen from under a running host app). Delete `build/<preset>/sdkconfig` once so `CONFIG_TINYUSB_SUSPEND_CALLBACK` applies.
   - The pure part (protocol state: standalone/hosted, which requests are accepted, what Take/Release call on the injected host) goes into `protocol.cpp` and is tested with the existing protocol tests, including release-on-disconnect and the rebuild of Home through a fake model. `host_link.cpp` and `usb.cpp` (unmount/suspend callbacks) are the thin device layer and hardware only.
   - `host/`: `takeControl()` / `releaseControl()` on the client, and the demos and bench call `takeControl()` first (the existing hosted behaviour must keep working end to end on the hardware). `FakeDevice.js` gets the mode.

2. **Wire touches to gestures and bindings (device layer). Done; hardware checks passed.**
   - **Done as:** `standalone.h/.cpp` (`Standalone`, pure and tested) owns the blob root, `OpQueue`, `TriggerDispatcher` and `GestureBank`, with the clock and `run` injected and no lock. `Init(host, blob, adjust)`, `Key(page, key, action, time_ms)`, `Tick()`, `Poll()`, `SetHosted(bool)`, `SelectActivity(i)`. Home keys pass their own `longPressMs` as the per-press override; the root `longPressMs` goes to `SetLongPressMs`. `SetHosted(true/false)` re-arms from the blob (held keys and queued ops dropped silently, no `release` fired; no activity selected). `standalone_task.h/.cpp` is the device layer: one mutex, and an op task on core 0 (priority 3) woken when work is queued and polling every 5 ms while a `delay` holds the queue; its `run` only records the op code and the task logs it after releasing the lock. `swipe_set_tick_cb` runs the gesture tick each frame. `KEY_LONGPRESS = 32` added to the op header (now `layout_types.gen.h`). Activity pages (1 and up) get no per-key `longPressMs` yet, and `SelectActivity` is only a pass-through for tests.
   - **Hardware checks for step 2 (passed):** gesture timing against the swipe window, a swipe from a key fires no tap, the frame lock causes no late fills, `op N` logged on a Home key press, Home keys work again after a release, unplug or suspend.
   - In standalone, the router's key events go to `GestureBank::Press/Release/Cancel` with `TouchControlName(page, key)`, `GestureBank::Tick` runs each frame, and `GestureHost::trigger` calls `TriggerDispatcher::Trigger`. The layout's `longPressMs` goes to `SetLongPressMs`; a key's own `longPressMs` is the per-press override. In hosted mode the same router events go to `Protocol::PostKey` as now. One switch point, driven by the mode from step 1 (this is where DRY matters: don't copy the router callback).
   - A small owner module holds the blob root, `OpQueue`, `TriggerDispatcher`, `GestureBank` and one lock, and is the only thing that touches them (the swipe task calls in for touches and ticks, the op task polls). The pure part (an owner that takes the injected clock and `run`) is host tested with the sample blob: press a Home key, see the op run; a hosted takeover cancels held keys; release rebuilds. The task and the lock are the device layer.
   - Tests also cover: a `long` binding defers `tap`; a touch that becomes a swipe fires `release` and no `tap`; nothing fires while hosted.
   - Hardware check: the gesture timing against the swipe window (see Hardware checks below), and that taking the model lock on key events doesn't delay frames.

3. **USB HID keyboard and the `hidKey` executor. Done; hardware checks passed (tap, chord, hold, takeover/unplug, vendor interface, layout upload).**
   - **Done as:** `hid_keyboard.h/.cpp` (`HidKeyboard`, pure, injected `HidSink`) sends an 8-byte boot report only when the state changes. A tap sends down, then up `HID_TAP_GAP_MS` later from `Poll()`, and holds the queue a further `HID_TAP_SETTLE_MS` (both 20 ms, tune on the hardware) via `OpQueue::Hold`, which `delay` uses too. An up for a key never down sends nothing. `ReleaseAll()` (also cancels a tap in flight) runs on takeover, release and a disconnect while already standalone. `Standalone` runs the `hidKey` op itself, then forwards every op to the host's `run`. **`hold` uses a release tag:** queue entries carry whether the `release` event queued them (`OpQueue::Releasing()`, set by `PushList(ops, release)`), so one `hold` op does down on press and up on release (decided; the alternative of the bake emitting `down`/`up` was rejected because it changes the packed format). hidKey op offsets (byte fields padded to 4): mode 4, modifiers 8, key 12 (the op header, now `layout_types.gen.h`). Held keys are tracked per modifier bit, not counted: releasing one of two held chords that share a modifier lifts the shared modifier. `usb.cpp` adds the keyboard behind `USB_HID_KEYBOARD` (follows `CONFIG_TINYUSB_HID_COUNT`; back out with `=0`, delete the generated sdkconfig and bump `USB_BCD_DEVICE`); `USB_BCD_DEVICE` is 0x0102 and the MS OS 2.0 descriptor now has configuration and function subset headers. `usb_hid_send` waits up to 20 ms for the previous report and is called from the op and protocol tasks (watch for the "HID report dropped" warning under load). Sample layout keys 6 (`ctrl+c`) and 7 (`shift+a`, hold) are in the host test sample only; `examples/upload-test.json` has the same two for the device.
   - Pure and host tested: HID report building (chord → modifier byte + one key code; modes `tap`, `down`, `up`, and `hold` as down on `press` and up on `release`), including a release that arrives after the key was never down, and all keys up on takeover/disconnect so nothing sticks. The executor uses an injected `HidSink` (send report); the op's fields (`mode`, `modifiers`, `key` bytes) is already validated.
   - Device layer, hardware only: add a HID keyboard interface to the composite descriptor in `usb.cpp` (interface map at the top of the file), an `HidSink` backed by `esp_tinyusb`, and bump `USB_BCD_DEVICE` (Windows caches the MS OS 2.0 descriptor). Check the vendor interface, WinUSB and WebUSB still enumerate and that the host library still works.
   - `tap` mode sends down, a short fixed gap, then up, through the op queue, so ordering with other ops is kept.
   - The test layout has a few keys: a plain tap key (`space`), a chord (`ctrl+c`), a `long` + `tap` pair, and a `hold` key. Hardware checks: the PC receives each, a held key doesn't stick when the device is unplugged or taken over, the keyboard survives a layout upload restart.

4. **Milestone check on the hardware. Done; passed.** Cold boot straight into Home; keys send HID (tap, chord, hold); the demo host takes control and shows its keys; releasing and unplugging the host both return to Home; keys work again afterwards; a key held through a takeover (`scratch/takeover-delay.js`) is released and stays quiet. **The first standalone milestone is complete.** CLAUDE.md's Operating Modes and Open Items are updated.

Notes for the order: step 1 doesn't depend on HID, so hosted mode keeps working throughout. Step 2 is testable without HID by recording ops through the injected `run`. Step 3 is the only step with new USB descriptor risk, so it comes last and can be backed out without touching the others.

## Runtime modes

- The device **boots into standalone mode**, loading its layout from flash. With no layout it shows one empty page.
- A host enables **hosted mode** (mode 1, [`protocol.md`](protocol.md)) with a "take control" message, and hands back with a "release" message (no further requirements). The host's keys replace the model's and key events go to the host. **Standalone is fully deactivated while hosted:** no actions or scripts run, in-flight runners are cancelled (held HID keys are released), and pad and encoder events go to the host as events and never invoke the activity's actions.
- On release, or when the host disconnects (USB unmount or suspend; a heartbeat timeout may be added for hosts that hang), the device **reverts to standalone**: it rebuilds Home only, with no activity selected.
- The model, compositor, swipe and touch router are shared. Only the keys' source and the key events' destination differ: hosted posts to `Protocol::PostKey`, standalone runs the key's actions.

## Page strip and activities

```
[ Home ] [ A.p1 ] [ A.p2 ] [ A.p3 ]      activity A selected
   0        1        2        3
```

- **Home** is always page 0: one page of 15 configurable keys, some of which select an activity.
- Pages 1..n are the selected activity's pages. Selecting an activity swaps pages 1..n for its pages (Home's keys and images are kept) and glides to page 1. Swiping right from page 1 returns to Home, so there is no home button.
- At boot only Home exists, with no activity selected.
- Selecting activity B while A is selected: run the switch ops (see "Devices and activity switching" below), swap the pages and glide to page 1. Selecting the current activity only glides to page 1 and runs no actions.
- `switchActivity` works from any page. The swap reuses `key_layout`'s all-or-nothing `SetKeys` (pages 1..n; Home's page is untouched), so it is atomic and ref counted. A `switchActivity` with a `null` target ("none") collapses the strip to Home and clears the status text.
- The selected activity is not remembered across reboots.

### Status bar

- **Left text:** always the selected activity's name, or its optional `displayName` if set. With no activity selected, it is empty.
- **Right text:** blank for now. It will show the time once Wi-Fi and NTP are set up.
- **Pager pills:** one pill per page, including Home.

### Wi-Fi

Credentials are set over USB from the command line (`node cli/cli.js wifi ...`) and kept in NVS; a web UI comes later. Built: see [`wifi.md`](wifi.md) for the design, the protocol messages (`WIFI_SET`, `WIFI_GET`, `WIFI_STATUS`) and the RAM it takes.

## Layout

The layout is authored as a JS object on the host (the user-level file is specified in [`layout-format.md`](layout-format.md)) and packed by [binpack](https://github.com/toptensoftware/binpack) (the `binpack/` submodule; the author is happy to change it where that's easier) into a BPAK blob that matches C structs (header generated by binpack). The device does not parse JSON. **One file defines everything:** the rle565 key images (from the host encoder) are packed into the same binpack blob as the structs, so there is no separate image storage or filesystem. A Node CLI uploads the blob over USB bulk and the device writes it to flash. A web front end comes later (see "Build pipeline and sources").

The design is adapted from the sister project `irtx` (`../irtx`: `firmware/binpack.js` for the schema, `firmware/activities.cpp` for the runtime). The code is not shared: take what's useful and modify it.

### Build pipeline and sources

```
layout definition (JS/JSON: buttons with inline actions, svg + label + colours)
   1. resolve (sync, pure):  defaults and inheritance, separate each button from its
                             binding, touch[col,row] names, names → indices, Lua syntax check
   2. render (async):        node-buttonImage → RGBA → rle565 encode; one canonical
                             Buffer per distinct encoded image
   3. pack (sync, plain):    binpack, no field mappers  → one .bin
   → CLI uploads over USB bulk → device flash
```

- **Stage 1** produces a plain, fully specified object tree (every key has its images' definitions, every binding its exact names). It is testable on its own.
- **Stage 2** walks that tree (it knows where the images are) and replaces each image definition with its rle565 `Buffer`, awaiting the renders. The cache is keyed by a hash of the encoded bytes and always returns the **same Buffer object** for identical content, so binpack's pointer dedupe stores each image once. The renderer is one injectable async function (`sharp` in Node; something else in a browser).
- **Stage 3** is a plain synchronous `pack()`. No field-level mappers: the schema stays dumb.

- **Defaults:** resolved in stage 1 by inheritance (root, activity, page, key), so a key with nothing specified inherits its backgrounds. Because identical definitions give identical encoded bytes, repeated defaults cost one render and one stored copy.
- **Name resolution:** device and activity names → indices (done by `packMapper` in `irtx`) moves into stage 1 as well.
- **Verified:** `host/test/bake/binpack-dedupe.test.js` shows binpack stores one copy of a shared Buffer object, 4-aligned, with the same pointer from every key; that distinct-but-equal Buffer objects are not merged; and that an absent image packs as length 0 and a null pointer.
- How the button definition (svg + label + colours) maps onto node-buttonImage calls, and from there to the four images per key, is still to work out. The existing `host/` library already encodes node-buttonImage output to rle565 and is the starting point.
- **Sources on the device (later, with the web interface):** it would be nice to keep the original materials (the unpacked definition and the `.svg` files) on the device, probably on the SD card, so everything can be edited with the device attached. Either way everything ends up binpacked to flash; the SD copy is only the editable source. The web front end would read the sources from the device, bake them in the browser, and upload the blob.
- **Constraint to carry now:** the bake pipeline (node-buttonImage, the rle565 encoder, binpack, the Lua check) should avoid dependencies that can't run in a browser, so the same code can be used by the CLI and by a web front end. Known status: binpack works in a browser (`irtx` uses it that way); the rle565 packer should work in a browser but is untested; **node-buttonImage uses `sharp`, which does not work in a browser**, so a workaround is needed later (stage 2's renderer is the one injectable function, so it is isolated there). The Lua syntax check (`wasmoon` or similar) is unchecked. SD access also needs a driver (CS is on the CH422G).

### Loading

- BPAK header (32 bytes: signature, version, relocation count and offset), the data, then a uint32 relocation table.
- Load into memory (PSRAM is fine; the layout is read on key events and activity switches, not by the bounce fill), add the base address to every relocated pointer, and cast the root to the struct.
- Validate while loading: signature, relocation offsets in range, counts and limits. A bad layout is rejected and the device stays on an empty Home.
- The loader is pure (bytes in, root out) so it can be host tested, including with corrupt and truncated blobs.

**As built (`layout_blob.cpp/.h`, `layout_apply.cpp/.h`, `layout_boot.cpp/.h`):**

- `LayoutValidate(blob, size, &summary)` walks the whole structure and reads the blob through the binpack-generated structs (`layout_types.gen.h`, regenerated with `node tools/codegen/gen-types.js` from the repo root). Pointers are 4 bytes in the blob, so the host tests are built 32-bit (`platform: "x86"` in `test.mk.js`) and the structs have the same shape as on the device. `firmware/test/data/sample_layout.json` is baked by the real packer into `sample_layout.bin`, so a schema change that isn't mirrored there fails the tests; re-bake it from the repo root with `node cli/cli.js bake firmware/test/data/sample_layout.json -o firmware/test/data/sample_layout.bin`). It checks: the BPAK header (signature, version, a relocation table that is exactly the end of the file), the table (aligned, in range, strictly ascending), then every pointer, count (against `LAYOUT_MAX_*` limits), string (NUL-terminated), op (known code, fields in range, `switchActivity` and device indices), and every image (4-byte aligned in memory, `check_image`, 128x128).
- **The relocation table must be exactly the set of non-null pointer slots the walk reaches** (a null slot in it, a pointer missing from it, or an entry the walk never reaches is rejected), so `LayoutRelocate(blob, size, base)` can't touch anything else.
- The summary has the version, long-press time, counts and Home's 15 keys x 4 image refs (offset and size). `LayoutApplyHome` registers each distinct image as an external image (ID = its offset in the blob) and sets page 0's keys; a key with no images is empty. It is all or nothing.
- `layout_boot_load()` runs before `model_init()` (the image store sizes itself from the PSRAM left after the blob) and before `screen_init()` (the flash read and the checks must not compete with the display fill); `layout_boot_apply()` runs after `screen_init()`. The blob stays in PSRAM, relocated.
- A missing, unreadable (CRC), invalid or oversized layout is logged and leaves the empty Home.
- Limits (`layout_blob.h`): 32 devices, 32 activities, 7 pages per activity (Home is page 0), 256 bindings per list, 64 ops per list.

### Schema (provisional)

```
root:       { version, home: { keys: key[15], bindings: binding[] },
              devices: device[], activities: activity[] }
page:       { keys: key[15] }
device:     { name, turnOn: op**, turnOff: op** }
activity:   { name, displayName?, devices: int[] (indices),
              pages: page[], bindings: binding[],
              willDeactivate, willActivate, didActivate, didDeactivate: op** }
key:        { normalBg, normalFg, pressedBg, pressedFg: each a length + byte* (rle565 data; null = none), longPressMs? }
binding:    { control: string, event: string, ops: op** }     (see "Triggers and bindings")
op:         polymorphic by `op` code (below)
```

- Names in the JS source (device names, activity names) are resolved to indices in the bake's stage 1, not by binpack mappers.
- Polymorphic types use binpack's `baseType` / `resolveVirtualType`, stored as pointer arrays (`op**`) because the subtypes differ in size.
- **Images:** each key's four image fields are `byte*` pointers straight to the rle565 data in the blob (no image table). binpack packs a distinct Buffer object once, aligns it to 4 bytes, and points every user at the same copy, so the bake step must hand out the **same Buffer object** for identical content (a canonical cache keyed by a hash of the encoded bytes). The loader checks every image with `check_image()` and rejects misaligned data before accepting the layout. binpack's `finalize()` now pads the blob area and the total length to 4 bytes, so the relocation table that follows in the file is aligned; the loader still reads table entries with `memcpy`, so it doesn't depend on that.
- The device checks image and page limits when loading and rejects an oversized layout. The PSRAM image store is the real limit: the three page buffers are fixed.

### Devices and activity switching

A **device** is external equipment (TV, receiver) with `turnOn` and `turnOff` op lists. An activity lists the devices it uses. On a switch the firmware only powers devices whose use changes (no TV power cycle between two activities that both use the TV). Switch order, as ops in the queue:

`old.willDeactivate` → `new.willActivate` → turnOn for devices the new activity needs and aren't on → turnOff for devices the old activity had and the new doesn't → `new.didActivate` → `old.didDeactivate`.

The page swap and glide do **not** wait for these ops: the selected activity changes and the pages swap at once, while the ops run through the queue (scripts compile on their first run, see "Actions"). A device's on/off state is updated when its ops are enqueued (as in `irtx`), so a second switch requested while the first one's ops are still queued is never dropped: A to B then B to C queue the right ops in the right order.

**As built** (host tested; hardware checks pending):

- **The plan** (`activity_switch.cpp/.h`, pure): `ActivitySwitchPlan(root, from, to, devices_on, out)` returns up to `ACTIVITY_SWITCH_MAX_LISTS` (4 + 32 = 36) list references in the order above, plus the new device mask. One list per device (each has its own `turnOn`/`turnOff`), empty lists skipped. `turnOn` runs in the new activity's `devices` order, `turnOff` in the old activity's order, and only for devices whose mask bit is set (on) or clear (off). A device used by both activities is not touched. `from == to` is a success with no lists.
- **The "none" target:** `switchActivity` with `"activity": null` (packed as index `0xFFFFFFFF`, `LAYOUT_ACTIVITY_NONE`) switches to no activity. From an activity to none, every device that is on is turned off, including devices in the mask that the old activity did not use (index order, after the old activity's). From none to an activity, stale devices in the mask are left alone. Selecting none while none is selected does nothing.
- **Hooks and device lists cannot switch:** the bake rejects `switchActivity` inside the four activity hooks and a device's `turnOn`/`turnOff` (including through `use`). The blob validator does not check this (it is a bake-only rule), so the executor does not assume it: a `switchActivity` found in a list still works, because the executor only enqueues, and it is planned from the state at the time the op runs.
- **The executor** (`Standalone::SwitchActivity`): selecting the current activity calls the host's `glide` only. Otherwise it computes the plan, pushes it with `OpQueue::PushLists` (all or nothing, at the queue tail) and, **only if the push succeeded**, commits the device mask and the selection (`TriggerDispatcher::SetActivity`), then calls the host's `apply`. If the ring is full the executor returns an error ("activity switch refused: op queue full"), which the op task logs and `OpQueue::Errors()` counts; nothing changes (no mask, selection or page change).
- **Held keys:** a switch resets the gesture bank (`GestureBank::Reset()`, no events fired) and calls `HidKeyboard::ReleaseAll()`, as hosted takeover does, so no HID key sticks. The key that started the switch is forgotten too, so its release does nothing. Release events would otherwise be looked up in the new activity's bindings.
- **Mask rules:** all devices are assumed off at boot. The mask is kept across hosted takeover and release (the equipment does not change), while the selection returns to none. The mask is an assumption, not truth: ops are fire and forget, and a failed `turnOn` or ops cancelled by a takeover leave it wrong. A toggle-style power command can drift; the user fixes it by selecting the activity again after an "off".
- **Tail pushes:** switch ops go at the tail. Two switch keys pressed before the first one's op has run queue both `switchActivity` ops before either one's hook and device ops; the second is planned from the state the first committed, so the final mask and device order are right.
- **The host** (`ActivityHost`, injected through `StandaloneHost::activity`): `apply(index)` and `glide()`, neither can refuse. `ActivityApplier` (`activity_host.cpp/.h`, pure and tested) implements it over injected screen calls: with the model lock held it runs `LayoutApplyActivity(root, blob, from, to, store, keys)` (`layout_apply.cpp`), then, with the lock released, `set_page_count(1 + n)`, `go_to_page(1, true)` (not for none) and `set_status_left` (`displayName`, else `name`; empty for none; the status bar cuts long text). `LayoutApplyActivity` stages the new images (checked `AddExternal`, skipping IDs already registered), builds all 15 keys of pages 1..n (empty ones overwrite the old activity's), sets the page count, calls `SetKeys`, then frees the old activity's IDs that neither Home nor the new activity uses. A refusal can only happen while staging and leaves store, layout and page count as they were; the applier then counts a failure and leaves the screen alone (the selection and queued ops have already been committed: the host contract has no refusal). The applier remembers the previous selection itself, and `Reset()` (called on hosted takeover and release, after the protocol has reset the store) sets it to none.
- **Lock order** (documented at the top of `standalone_task.cpp`): the standalone mutex, then the model lock, never the reverse. The swipe task takes the model lock only inside `model_key_active` and `model_set_key_pressed`, never while it calls the key callback, so it cannot deadlock with the op task. The swipe event queue (32 deep) drops silently when full; a switch sends two events (page count, go to page).
- **Not needed:** per-key `longPressMs` for activity pages (`Standalone::KeyLongPressMs` returns the layout default for pages other than Home, by design). **Not built:** a trusted `AddExternal` without `check_image` (the checked call runs on every switch under the model lock; add one only if the hardware timing shows a need).


## Triggers and bindings

The input side is a **binding to the action manager**: a trigger goes in, the matching op lists go onto the global queue and run in order. A trigger is three strings, which keeps the mechanism loosely coupled to whatever hardware, scripts or transport drives it:

| String | Meaning | Examples |
|---|---|---|
| `control` | which control | `touch[4,3]`, `macro1`, `volEnc` |
| `event` | what happened | `press`, `release`, `tap`, `long`, `turnLeft` |
| `param` | free-form string for the script | `velocity=10`, empty |

- A **binding** maps a `(control, event)` pair to an op list. Matching is exact on the two strings; there are no wildcards on the device (the host's authoring layer expands any patterns). The binding's `param` is the trigger's, passed to the op list for scripts to parse (see Parameter).
- **Touch control names span the page strip.** `touch[col,row]` uses a column index that runs across all pages: `touch[0,0]` is the first key on Home, `touch[5,0]` is the first key on the selected activity's first page, `touch[10,0]` the first key on its second page, and so on (page `p`, column `c` is column `5p + c`). The name therefore identifies the page, so bindings need no per-page lists.
- **Where bindings live:** Home's bindings (touch only: Home has no pad or encoder bindings) and the selected activity's bindings (its touch keys with columns of 5 or more, the macro pad, the encoder, anything else). A trigger is matched against Home's list and the current activity's. The pad and encoder therefore keep working on the current activity while Home is in view, and with no activity selected they do nothing.
- **Authoring vs the packed form:** in the user's layout the op lists are written inline with the button definition, and the host's baking step turns them into these bindings with the exact touch names. The device only ever sees binding lists. (Resolving inline lists on the device at load would also work, but isn't planned.)
- **Every matching binding is enqueued** in layout order (usually there is only one).
- Everything that can trigger uses the same entry point, `Trigger(control, event, param)`: the gestures below, the macro pad and encoder (from a second MCU over a serial connection; the format is not defined yet, the binding mechanism just has to accept it later), and scripts or tests. Hosted mode reports to the host instead of running anything: touch keys as now, and raw `press` and `release` for the macro pad (the host detects long and double presses).
- Unmatched triggers are ignored.

## Key gestures

The touch router gives PRESS, RELEASE and CANCEL (a press that became a swipe). A pure `key_gesture` module (unit tested like `touch_router`) takes those plus a clock tick and emits the events below as triggers (`touch[col,row]` is the control; the param is empty):

| Trigger | Fires |
|---|---|
| `press` | on PRESS, immediately |
| `release` | on RELEASE, and on CANCEL (touch only), so a held key is never left stuck |
| `tap` | on RELEASE, if shorter than the long-press time and `long` didn't fire |
| `long` | at the long-press time, while still held |

- The gesture engine runs **per control**, for touch keys and for the macro pad alike. The macro pad (and any other external button) delivers only raw `press` and `release`; the device synthesizes `tap` and `long` from them.
- It asks the dispatcher `HasBinding(control, event)`. `tap` is deferred to RELEASE only if the control has a `long` binding. Otherwise it fires on PRESS, so plain keys add no latency. (Built: `tap` is triggered whether or not it is bound; unmatched triggers are ignored. On RELEASE `release` fires before a deferred `tap`. A release after the threshold fires `long` first if no tick came in between. A cancel fires `release` only.)
- Built as `GestureBank` (`key_gesture.cpp/.h`): up to 8 controls held at once, names kept to 23 characters, time compared with a wrapping signed difference. A press while all slots are used, or of a control already held, is ignored.
- `long` fires at the threshold, not on release.
- HID keys have a mode: `tap` (press and release together), `hold` (key down on `press`, key up on `release`), or an explicit key down or key up.
- The long-press time is one global default (`GESTURE_LONG_PRESS_DEFAULT_MS`, 500 ms, tune on the hardware; the layout's `longPressMs` replaces it, 0 = this default) with the optional per-key `longPressMs`.
- Double press is out of scope for now.

## Actions

Adapted from `irtx`'s op machine. Triggers (key gestures, activity hooks, device lists) all just enqueue op lists.

The rule: **ops are the simplest possible sequence, and anything more complex goes to a script.** Most actions are fire and forget, so they stay a flat op list. Logic, state, round trips and math go in a script.

- **Queue:** a ring of **list references** (`OP_QUEUE_SIZE` = 40 entries of ~16 bytes, `{ops pointer, count, param slot, release flag}`, ~640 bytes, so any legal switch of up to 36 lists fits an idle queue; the op arrays stay in the resident blob), not of expanded ops. The executor walks the current list op by op, starting one op per poll cycle. `PushList` pushes one list; `PushLists(refs, n)` pushes several all or nothing (refused if the ring would overflow, with nothing queued and the param slots rolled back). Empty lists are skipped and take no entry. `Count()` is ops not yet started, `Lists()` the ring entries in use, `Dropped()` the lists refused. Instant ops finish when started. Timed ops (`delay`, a held HID report, a running script that is waiting) keep the queue paused until done. Ops allocated at runtime are flagged and freed after they run.
- **Ops:** each has an `op` code and an executor (start function, plus an optional poll function for timed ops), looked up by a table indexed by code. Planned set: `delay`, `switchActivity`, `hidKey`, `httpGet`, `httpPost`, `udpPacket`, `sendWol`, `script`. There are no registers, no `searchString`, `ifTrue` or `waitHttp` (`irtx`'s way of doing the complex 1%): a script does all of that. (`irtx` also has `sendIr`, `setIrReg` and `led`; not planned here.)
- **Parameter:** a trigger can pass a single **string** to the op list (e.g. the encoder's velocity). Only `script` ops use it: simple ops take no parameters. The script parses the string however it likes.
- **Scripts:** one `script` op runs a short embedded script (**Lua 5.4**, decided). Examples:
  - Mute toggle (round trip): GET the status, test the reply, POST mute or unmute.
  - Volume (math only): map the encoder velocity to a delta step, then fire and forget one request.
  - The script API mirrors the ops (`delay`, `switchActivity`, `hid.key`, `http.get`/`post`, `udp.send`, `wol.send`, plus `param`, `log`, `millis`), so a script can do everything an op can. Each call fills a scratch blob op and runs it through the same executors as the queue (a parity test runs every op both ways). Full table: [`layout-format.md`](layout-format.md#script-api).
- **Script execution:** each script runs as a coroutine. A call that has to wait (`http.get`, `delay`) starts the work through the injected host interface and yields. The script op becomes a timed op, and the poll function resumes the coroutine when the result arrives (a mailbox from the HTTP task; only the interpreter task touches the Lua state). Fire-and-forget calls (`http.post`, `udp`) don't yield, so a script like the volume map runs straight through. Calls return `nil, err` on failure or timeout rather than throwing.
- **Script compilation:** the blob stores script **source**. The pack tool compiles every script with a real Lua 5.4 and fails on a syntax error (needs a Lua 5.4 compiler in Node, e.g. `wasmoon` or a local `luac`). On the device (built): a script is compiled **on its first run**, keyed by its source pointer in the blob, and cached with its own globals until the next re-arm (takeover or release), when the whole Lua state is closed. Scripts are not released per activity (simpler, and a deactivation script is never discarded early); revisit if a layout has many large scripts. A compile failure is logged once and that script's op ends in an error each time it runs. Compile cost lands on the first run, not in the switch.
- **Script limits (built):** an instruction-count hook stops a script that runs **100 ms** without yielding (`SCRIPT_SLICE_MS`); a **30 s** wall timeout cancels a run (`SCRIPT_WALL_MS`; a late reply is dropped by its generation number); the Lua heap is capped at **256 KB** in PSRAM; a waited `http.get` reply is cut at **16 KB** (one shared buffer: only the one running script waits). A script error is logged and the queue carries on. The script holds the standalone lock while it runs (up to one slice), so a key event can wait that long (hardware check).
- **Rate (decided, no coalescing):** the encoder can tick many times a second, and each tick runs the volume script and fires a request. `http.post` copies the URL and body into a bounded request queue and returns at once; it never yields, so a script that only posts finishes in one `lua_resume` and never pauses the op queue. One persistent HTTP worker task drains the queue in order and reuses its connection (keep-alive); `irtx`'s task per request won't hold at encoder rates. If the queue fills (slow or offline receiver) the oldest request is dropped, so the latest knob position wins, and the script call never blocks or errors. Only `http.get` yields.
- **Host testable:** the queue, `switchActivity` ordering, the device on/off tracking and the script runtime are pure and unit tested. The executors that touch the world (HTTP, UDP, WoL, HID, clock) go behind an injected interface, like `ProtocolHost`, with a fake in the tests: a fake `http.get` records the request and the test completes it explicitly, so waits, timeouts, failures and late replies are deterministic. Only the thin device layer (esp_http_client, lwIP, TinyUSB) is hardware only.
- **First milestone:** `hidKey` (a HID interface added to the composite descriptor in `usb.cpp`), plus `delay` and `switchActivity`.
- **HTTP/UDP:** use whatever is easiest under ESP-IDF (`esp_http_client`, lwIP sockets). `irtx` uses Arduino (`HTTPClient`, `WiFiUDP`, `LittleFS`, `millis`, `String`), so the executors are rewritten.
- **Script API surface:** built; see [`layout-format.md`](layout-format.md#script-api).
- **Known `irtx` rough edges not to copy:** `httpPost` factory uses the GET code, some factories emit `type:` instead of `op:`, `udpPacketOp` has no port field, and a switch is dropped if the queue is busy.

## Open questions

Noted, not designed yet.

### Decided

- **Reusable actions:** handled host side for now. The host's authoring layer defines a named action once and expands it wherever it's used (the packed blob has the ops inlined). The device has no `callAction` op.
- **Pad and encoder inputs (decided):** an attached **6-key macro pad** and a single **rotary encoder** are bound **per activity, not per page**. Home has no bindings of its own: the pad and encoder always use the **current activity's** bindings, even while Home is in view. With no activity selected they do nothing. The encoder reports a velocity; a script maps it to a delta and fires one fire-and-forget request, with no coalescing (see Rate).
- **Lua:** Lua 5.4. The heap location (internal RAM or PSRAM) is whatever works best and easiest; the first scripts string-scan the HTTP reply and don't need a JSON decoder. Libraries: no `io` or `os`.

### Storage, images and inputs (decided details)

- **External triggers (pad and encoder).** Decided: the three-string trigger (see Triggers and bindings). The macro pad sends only raw `press` and `release`; the device synthesizes `tap` and `long` (decided). Open, later: the serial line format from the second MCU (the binding mechanism just has to accept it), and the protocol's new events. Decided: in hosted mode the pad and encoder are **reported to the host** (like `PostKey` for touch keys) and never run activity actions, so `protocol.md` needs pad-key and encoder events (the encoder's velocity as a string or number) when this is built.
- **Image IDs (built):** to keep `key_layout` and the compositor unchanged, the loader walks the keys and registers each distinct image pointer with the image store as an **external** entry (a pointer into the blob; never copied or freed). The ID is the image's byte offset in the blob (4-aligned and never 0, so unique and stable; the same pointer always gives the same ID, which deduplicates). `ImageStore::AddExternal(id, ptr, len)` checks the image like an upload and uses it in place. `ImageEntry` now carries `pixels` and `opaque` (the layout reads those, not `data`), and `data` is null for an external entry. The compositor asks `ImageStore::PixelsRetired()`, which knows the external range: an external image has no `ImageData` header in front of it for the static `IsRetired()` to read, so all external images must lie in one region holding nothing else (the blob). Uploading over an external ID replaces it. The store is reset on hosted takeover (RESET already does) and rebuilt on release (not built: needs the take-control and release messages).
- **Where the blob lives at runtime (decided, revisit if it causes issues):** copy the whole blob into PSRAM at boot and relocate (so relocation just works). If it doesn't fit alongside the page buffers (~2.1 MB) and everything else, the alternative is mapping the flash read-only. That can't be relocated in place (the mapped address isn't fixed), so the struct part would be copied to RAM with image data left in flash. Measure a real blob first.
- **Layout storage and upload (decided):** a raw `layout` flash partition (no filesystem) holding one blob. Why not LittleFS: wear isn't a concern (flash sectors endure ~100,000 erases and the layout is rewritten only on an upload; the Wi-Fi credentials go in NVS, which wear-levels itself); LittleFS's real benefit would be copy-on-write power-loss safety, but it needs room for the old and new files at once, adds code and RAM, and files aren't contiguous so the flash-mapping fallback above would be impossible. Two raw partitions (A/B) would give the same safety later, and stay mappable. USB messages (vendor protocol): begin (size, CRC), data chunks, commit. The device checks the CRC, writes the partition, and applies by rebooting (decided; the CLI waits for the device to come back). Interrupted uploads: one partition (decided; no A/B). Flash writes stall the bounce ISR, so the screen is blanked (backlight off) while uploading. The 16 MB flash holds the app plus the partition(s).
  - **Built (upload only; the boot loader is not):** [`protocol.md`](protocol.md#layout-upload) has the messages; the partition map is here. The `layout` partition is 4 MB at `0x210000` (`firmware/partitions.csv`; the app has 2 MB; the label was `config` before the rename, so a board flashed before it needs one full `idf.py flash` to get the new partition table, after which the stored blob still loads). Its first 4 KB sector is a **header** (five u32s: magic `0x46434243` ("CBCF", from when the layout was called the config: kept so stored blobs survive the rename), version 1, blob size, blob CRC-32, CRC-32 of the first 16 bytes) written last, and the blob starts at `0x1000`. A begin erases the header, so an interrupted upload leaves no valid header and the boot loader shows an empty Home without having to CRC the blob. The boot loader checks the header, and may re-check the blob's CRC (`LayoutStore::Verify`, which the commit already uses to read the flash back).
- **Queue responsiveness:** there is one global queue for now (decided), so strict ordering comes free but input behind a waiting op (`delay`, `http.get`) waits for it. Mitigate with a short HTTP timeout. Revisit with parallel runners (per-source lanes) only if this hurts on the hardware. A full queue drops new triggers and logs.

### Still open

- **Button definition → images:** how a button's definition (svg + label + colours) maps onto node-buttonImage calls and the four images per key (normal and pressed, background and foreground); and the defaults inheritance chain (proposed: root, activity, page, key; not yet confirmed). A proposal is in [`layout-format.md`](layout-format.md) (the user-level layout file spec), which needs confirming. The `host/` library already encodes node-buttonImage output to rle565.
- **Browser support for the bake pipeline:** `sharp` workaround; rle565 packer untested in a browser; Lua syntax check library (see Build pipeline).
- **Pad and encoder:** the serial line format from the second MCU, and the exact event names for the encoder (`turnLeft`, `turnRight`, velocity in the param).
- **Protocol changes needed** (`protocol.md`, firmware and `host/` kept in sync): "take control" and "release" messages; pad-key and encoder events in hosted mode. (Layout upload is done: [`protocol.md`](protocol.md#layout-upload); `node cli/cli.js upload` sends a layout.)
- **Lua JSON decoder:** none yet (the first scripts string-scan HTTP replies). Heap (256 KB, PSRAM), HTTP timeout (10 s) and op queue size (40) are settled.
- **Blob size and PSRAM fit:** measure a real layout (see "Where the blob lives at runtime").
- **SD card:** the driver (CS is on the CH422G) and keeping the editable sources there, with the web interface.
- **Backlight and idle in standalone:** not discussed. Today the backlight and wake-on-touch gate are driven by the host ([`backlight.md`](backlight.md)); standalone has no host.
- **Hardware checks when we get there:** gesture timing against the swipe window (long-press time, tune on the device), flash writes stalling the bounce ISR during upload (blank the screen while uploading), glide smoothness while an activity's scripts compile, and the page strip's pager pills.
- **`CLAUDE.md` (a rules file, so only with your OK):** add `binpack/` and the bake (now `host/src/bake/` and `cli/`) to the Repository Layout, the bake tests to the build and test sections, and the standalone open items.

## Testing

- Unit tested (host tests, test first): the page strip and activity switching, `key_gesture`, the BPAK loader (valid, corrupt, truncated), the op queue, the script runtime (against a fake host interface), and HID report building. Test blobs come from binpack, run from a script at test build time or checked in.
- Hardware only (flag when we reach them): flash storage, USB HID, gesture timing against the swipe window, and flash writes during upload stalling the bounce ISR (see CLAUDE.md Open Items).
