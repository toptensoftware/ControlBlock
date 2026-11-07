# ControlBlock

A StreamDeck-style control surface: a grid of configurable touch "buttons" on an LCD panel that trigger actions on a host machine.

## Hardware

Board: [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm)

- **SoC:** ESP32-S3N16R8: dual-core 240 MHz, 16 MB flash, 8 MB octal PSRAM, Wi-Fi/BLE.
- **Display:** 800x480 RGB565 parallel RGB panel. **The 20 panel pins run at the weakest drive strength** (`DISPLAY_PIN_DRIVE_CAP` = `GPIO_DRIVE_CAP_0`, set after `esp_lcd_new_rgb_panel()`): at the default (2) the RGB bus, most likely the 25 MHz PCLK, interfered with the Wi-Fi radio (pings of 100-1000 ms with loss, even at -40 dBm). Drive 0 gives 2-10 ms pings and a clean picture; don't raise it. **Touch:** GT911, 5-point, I2C (SDA 8, SCL 9, 400 kHz); INT is GPIO4.
- **CH422G IO expander** (I2C): drives touch reset, backlight, LCD reset, SD CS and the USB/CAN mux (low = native USB), so these aren't plain GPIOs. Always go through `ch422g.cpp`.
- **USB:** native USB (TinyUSB device: vendor now, HID/MIDI later) and a CH343P USB-UART port (flashing and logs). The firmware takes the native port, so it's no longer a serial/JTAG port: flash and monitor over the UART port (or hold BOOT and press RESET to flash over native USB).

## Building

ESP-IDF v6.0 at `C:\esp\v6.0\esp-idf`, tools in `C:\Espressif`. From PowerShell in `firmware/`:

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue   # idf.py refuses to run under Git Bash's MSYS variable
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
idf.py --preset debug build              # or: idf.py --preset release -p COM4 flash monitor (the board is on COM4)
```

- Presets (`firmware/CMakePresets.json`): `debug` (`build/debug`, `-Og`, assertions on; the default) and `release` (`build/release`, `-O2`, `NDEBUG`; use for timing). `$env:IDF_PRESET = "release"` skips `--preset`.
- Settings: `firmware/sdkconfig.defaults` plus `sdkconfig.release`; the sdkconfigs under `build/` are generated. Notable: octal PSRAM at 80 MHz, 64 KB data cache with 64-byte lines, `LCD_RGB_RESTART_IN_VSYNC` (a late bounce fill costs one bad frame instead of a lasting shift). Code and rodata are mapped to PSRAM.
- Logs go to UART0 (CH343P) only (`ESP_CONSOLE_SECONDARY_NONE`: TinyUSB has the native port).
- Components: `esp_tinyusb` (2.x) and the GT911 driver, from the component manager (`main/idf_component.yml`). TinyUSB's vendor FIFO sizes are in `sdkconfig.defaults`. After changing `sdkconfig.defaults`, delete (or move aside) `build/<preset>/sdkconfig` so the change applies: values already in a generated sdkconfig win over the defaults.
- `CONFIG_TINYUSB_SUSPEND_CALLBACK=y` is in `sdkconfig.defaults` (USB suspend releases host control): delete the generated `build/<preset>/sdkconfig` once for it to apply.
- The user flashes and tests on the hardware. If the monitor warns of a "checksum mismatch between flashed and built applications", the flash didn't take and the board runs an older app: flash again (check for `Hash of data verified.`).

### Host unit tests

`firmware/test/` builds the platform-independent firmware code into a Windows exe with SimpleLib's `UnitTesting.h` (see the `simplelib` skill). From `firmware/test`:

```sh
mk run                                   # build and run (~13 s); exit code 1 on failure
FUZZ_SEEDS=200 mk config=release run     # long fuzz soak
```

- A test that overruns a buffer can silently corrupt the test registry's statics in the debug exe (the runner then skips tests and still reports "All passed"): if the debug and release test counts differ, suspect an out-of-bounds write in a test. This once hid 81 tests (`TestStatusBar` rendered 7-row bands into a 4-row scratch).

- Built 32-bit (`platform: "x86"` in `test.mk.js`) so the blob structs match the device's struct shapes.
- Needs `mk` (`npm install -g toptensoftware/mk.js`) and MSVC. Reads `firmware/assets/*.bin` (`LoadAsset()`). Sources are listed in `test.mk.js`.
- `harness.h/.cpp`: a `World` runs a real `PageCompositor` against a fake client, bounce fill and clock, checking its invariants at every frame start. Preempt hooks run frame starts and client changes inside `Step()` to test races deterministically. `RetireUnused()` exercises sweeps: freed images are poisoned and their addresses reused straight away.
- `store_env.h/.cpp`: the image store's test platform (an allocator that poisons and reuses freed blocks, a fake sweep), shared by the store, key layout and protocol tests. Protocol tests feed raw bytes in and decode the responses. `TestGapShare.cpp` covers the fill-gap round-robin, and `TestStatusBar.cpp` checks that the banded status bar render matches the whole-bar render byte for byte.
- Keep device-only code (FreeRTOS, ESP-IDF, TinyUSB) out of the tested files: use `port.h`, pure functions and injected callbacks (`ImageStoreHost`, `ProtocolHost`), with a thin device layer around them (`compositor.cpp`, `screen.cpp`, `status.cpp`, `model.cpp`, `host_link.cpp`, `usb.cpp`).

### Host library tests

From `host/`: `npm install` once, then `npm test` (Node's test runner, no device: `test/FakeDevice.js` stands in for the WebUSB device). This includes the bake tests (`test/bake/`, one of which renders real images). On a device, from the repo root: `node examples/demo.js` and `node examples/bench.js`.

### CLI and binpack tests

From `cli/`: `npm test` (no device: upload and wifi run against the library's `FakeDevice`; nothing to install, it uses `host/`'s). From `binpack/`: `node --test test.js`. From `node-buttonImage/`: `npm test` (label wrapping).

`host` and `tools/test-icons` copy their `file:` dependencies (`install-links=true` in their `.npmrc`), so edits to `node-buttonImage` (or `rle565/encoder`) don't reach them by themselves: bump the library's version, delete its folder under each project's `node_modules/@toptensoftware/` (or `node_modules/rle565`), then `npm install` there. `cli/` and `examples/` have no dependencies: they import the library by relative path (`../host/index.js`), so they never go stale. The library imports `binpack` by relative path too.

## Repository Layout

```
docs/              Specs: protocol.md, image-assets.md, page-compositor.md, status-bar.md, interactions.md, backlight.md, standalone.md, layout-format.md, wifi.md, standalone-ops-plan.md (plan for the remaining ops and Lua; not yet approved), backlight-waveshare-support.md (text for a Waveshare support request) (screen-buffers.md is partly superseded)
firmware/          ESP-IDF project for the device (C++)
  assets/          Generated binaries embedded in the firmware (ui.bin; test_keys.bin only for the stress run)
  test/            Host unit tests (test/data/sample_layout.json and .bin: a sample layout baked by the real tool)
host/              Node.js library: the device API (WebUSB over the `usb` package or a browser; mode 1; browser.js is the browser entry) and the layout bake (src/bake/: layout file to BPAK blob, upload)
cli/               Command line over the library (node cli/cli.js: bake, upload, wifi); cli/README.md
examples/          Hardware test layouts (net-test.json, upload-test.json, activity-test.mjs, script-test.mjs; shared helpers and the test server's address in test-common.mjs), demo.js and bench.js; test-server/ has net_test_server.py (HTTP :8000, UDP :9 and :9999) and takeover.mjs (take control, hold, release)
web/               Planned, not created yet: a web front end for configuring the device
rle565/            Submodule: rle565 renderer (src/) and encoder (encoder/)
node-buttonImage/  Submodule: host-side key image rendering
SimpleLib/         Submodule: C++ utility library used instead of STL (only Core/ is used)
binpack/           Submodule: packs JS objects into C-struct blobs (BPAK); used by the bake (host/src/bake) for the standalone layout
PacketEncoder/     Submodule: packet framing over a byte stream (sync, sequence number, command, CRC-32; matching C and JS); the macro pad's wire format (pad/, tools/pad-sim)
pad/               ESP32-C3 macro pad firmware (keys + encoder, one-way serial to the S3); job wip/wip-macro-pad.md
tools/pad-sim/     Node tools for the pad's serial line (dump.js, sim.js)
lua/               Submodule: Lua 5.4.7 (official lua/lua repo) for the `script` op; sources listed in firmware/lua_sources.txt
tools/test-icons/  Generates firmware/assets/*.bin (npm install && npm run build)
tools/codegen/     Writes the firmware headers generated from the bake's sources (gen-types.js, gen-hid-keys.js)
scratch/           Throwaway scripts (gitignored)
```

Clone with `--recursive` or run `git submodule update --init`.

## Operating Modes

1. **Host-controlled:** Like a Stream Deck. PC software decides what each key shows; the device reports key events and the PC acts on them. Long press and double press are detected host side. Built.
2. **Standalone (first milestone built and verified on the hardware; the default):** the device boots into it. It holds a Home page plus "activities" (each a set of pages) in flash, and keys run actions on the device: USB HID keyboard first, then MIDI, HTTP and UDP. A host must send `TAKE_CONTROL` before any screen request (they are refused with `NOT_HOSTED` while standalone); `RELEASE_CONTROL`, USB unmount or USB suspend returns the device to Home and turns the backlight on. The milestone (boot into Home, touch gestures and bindings, `hidKey` over a USB HID keyboard, take/release control) works on the hardware; activity switching, devices and HTTP/UDP/WoL also work on the hardware (against a test server; real equipment not yet tried). The `script` op (Lua 5.4) is built and host tested but not run on the hardware. Spec: [`docs/standalone.md`](docs/standalone.md).

## Firmware

Modules in `firmware/main/` (all C++; `rle565.c` from the submodule stays C):

- **Drivers** (`waveshare_rgb_lcd_port`, `waveshare_touch_port`, `i2c`, `ch422g`): adapted from the sister project `../aclock/clock/main/`; adapt these, don't write new ones. Init order: `i2c_init()`, `ch422g_init()`, display, `touch_init()`.
  - `display_init_bounce(fill_cb, ctx)`: bounce-buffer mode, no framebuffers. `fill_cb` gets 15 lines at a time, in order (`y == 0` starts a frame); budget `DISPLAY_BOUNCE_BUDGET_US` (~615 µs) per fill, ~49 fps. **The pixel clock is 20 MHz by default** (was 25 MHz, ~61 fps, 492 µs; may change after further tests); `-DCB_PIXEL_CLOCK_MHZ=n` overrides it (budget = 15 x 820 / n µs, fps = n MHz / 410,000; an empty value goes back to the default). `RUN_STRESS` at 20 MHz: 0 over budget, worst fill 327 µs (53%) against 411 µs (84%) at 25 MHz, compositor throughput ~17% lower. 18 and 16 MHz untried, the panel's minimum clock unknown.
  - `touch_init(cb)`: polls the GT911 at 100 Hz on its own task; `cb(id, DOWN|MOVE|UP, x, y)` per finger.
- **`port.h`:** platform layer for code shared with the host tests (spinlock, atomics, cycle counter, cache write-back).
- **`screen_layout.h`:** screen geometry (`SCREEN_*`).
- **`screen.cpp/.h`:** the bounce fill (rules below). Latches the scroll position, the pages from the compositor and the status bar buffer at frame start, then copies lines. Counts late fills, split by where they happened.
- **`screen_view.cpp/.h`:** frame-start maths, pure and tested (pages in view, what to show, page most in view).
- **`page_compositor.cpp/.h`, `compositor.cpp/.h`:** draw keys into three PSRAM page buffers and decide which the fill shows. Core is platform independent and tested; `compositor.cpp` runs it in a task. Spec: [`docs/page-compositor.md`](docs/page-compositor.md).
- **`status_bar.cpp/.h`, `status.cpp/.h`, `ui_assets.h`:** the status bar (text, pager pills). Both 64 KB buffers are in PSRAM; a redraw is rendered in bands into a small internal scratch and copied across one band per fill gap. Spec: [`docs/status-bar.md`](docs/status-bar.md).
- **`fill_gap.cpp/.h`, `gap_share.h`:** the gap after each bounce fill, shared round-robin between the PSRAM writers (the compositor and the status bar) so two never write at once. `gap_share.h` (the `GapRotor` picker) is pure and tested; `fill_gap` is the device layer, called from the fill ISR.
- **`swipe.cpp/.h`:** drag, flick and spring glide between pages, in a task woken each frame; tuning constants at the top. Sets the status bar's selected pill. Also takes the host's page count and go-to-page (through its queue) and reports page changes once settled. Feeds touches through the touch router and shows each key's pressed look from touch-down (reverted if it becomes a swipe) until RELEASE/CANCEL.
- **`touch_router.cpp/.h`:** swipe or key press: the swipe window, edge-ambiguous zone, and PRESS/RELEASE/CANCEL. Pure and tested; tuning constants in the header. Spec: [`docs/interactions.md`](docs/interactions.md).
- **Backlight wake gate** ([`docs/backlight.md`](docs/backlight.md); built and unit tested, **not yet run on the hardware**): while the host has the backlight off, touches skip the router and swipe and send a WAKE event (`0x82`) per touch-down instead; the rest of that touch is swallowed. The state is in GET_INFO `flags` bit 0. Pure and tested: `touch_gate.cpp/.h` wraps `touch_router`; `swipe.cpp` feeds it, `host_link.cpp` holds the state and drives the light and the gate.
- **Naming:** "layout" is the standalone configuration (the user's layout file, the baked layout blob, the stored layout in flash); "key layout" (`KeyLayout`) is the live pages of keys. Hosted-mode requests that set keys, images or pages are "screen requests". Other senses of "config" (sdkconfig/Kconfig `CONFIG_*`, driver config structs, USB configuration descriptors, `WifiConfig`) are unrelated. (The standalone layout was called "config" until 2026-11-08; `complete-*.md` docs keep the old names.)
- **`layout_store.cpp/.h`, `layout_flash.cpp/.h`, `crc32_table.h`:** the standalone layout upload to the `layout` flash partition (`partitions.csv`: app 2 MB, layout 4 MB; header sector written last; the header magic `LAYOUT_MAGIC` still reads "CBCF" so blobs stored before the rename load). `layout_store` is platform independent and tested over a fake NOR flash; `layout_flash` is the device layer; `host_link.cpp` blanks the screen during an upload and restarts after the commit. Spec: [`docs/protocol.md`](docs/protocol.md#layout-upload); the partition map is in [`docs/standalone.md`](docs/standalone.md) (Layout storage). Run on the hardware with a small blob. A changed partition table needs a full `idf.py flash` (not app only): the partition was renamed from `config` to `layout`, so a board flashed before that needs one full flash (the stored data survives).
- **`layout_blob.cpp/.h`, `layout_apply.cpp/.h`, `layout_boot.cpp/.h`:** the standalone layout at boot. `layout_blob` (pure, tested) validates a BPAK blob and relocates its pointers, then reads it through the binpack-generated structs (`layout_types.gen.h`, wrapped by `layout_types.h`: namespace `layoutdata`, limits, `LAYOUT_ACTIVITY_NONE`). After a schema change (`host/src/bake/schema.js`) run `node tools/codegen/gen-types.js` (regenerates `layout_types.gen.h`) and re-bake `test/data/sample_layout.bin` (`node cli/cli.js bake firmware/test/data/sample_layout.json -o firmware/test/data/sample_layout.bin`), both from the repo root. `layout_apply` (pure, tested) registers Home's images and sets page 0's keys, all or nothing (`LayoutApplyHome`); `LayoutApplyActivity(root, blob, from, to, store, keys)` swaps the activity pages 1..n the same way (registers the new images with the checked `AddExternal`, frees the old activity's unshared ones; -1 = none). `layout_boot` (device) reads the partition into PSRAM before the display starts (`layout_boot_load`, before `model_init`/`screen_init`) and applies it after `screen_init`, before the backlight. A missing, damaged or invalid layout leaves the empty Home. The blob stays resident in PSRAM. `LayoutStore::Read` reads and CRC-checks the stored blob.
- **`op_exec.h`, `op_registry.cpp/.h`, `param_pool.h`, `op_queue.cpp/.h`, `trigger_dispatch.cpp/.h`, `key_gesture.cpp/.h`:** the standalone trigger side (all platform independent and tested; wired to the device by `standalone.cpp` and `standalone_task.cpp` below). `op_exec.h` is the executor protocol (start/poll/cancel, `OpStatus`, `OpTask`, `ExecCtx`); `op_registry` is the op table; `param_pool.h` holds trigger `param` strings (8 slots, refcounted). `TriggerDispatcher` matches (control, event) against Home's and the selected activity's bindings and pushes their op lists on the `OpQueue`. The queue holds list references (ops pointer, count, release flag, param slot) in a ring of `OP_QUEUE_SIZE` = 40, walks the current list op by op, and `PushLists` adds several lists all or nothing; `delay` holds it. `GestureBank` turns press/release/cancel into press, release, tap and long per control. None of them lock: the device layer serialises. Spec: [`docs/standalone.md`](docs/standalone.md).
- **`standalone.cpp/.h`, `standalone_task.cpp/.h`:** the owner of the trigger side. `Standalone` (pure, tested with the sample blob; injected clock and `run`, no lock) holds the blob root, `OpQueue`, `TriggerDispatcher` and `GestureBank`, plus the selected activity and the device on/off mask: `Key(page, key, action, time_ms)`, `Tick()`, `Poll()`, `SetHosted(bool)` (both directions re-arm from the blob: held keys and queued ops dropped silently, no activity selected, device mask kept). The `switchActivity` executor plans the switch (`activity_switch.cpp/.h`: pure, list references in spec order plus the new mask), pushes it at the queue tail all or nothing, and only then commits the selection and mask, resets gestures, calls `HidKeyboard::ReleaseAll` and the injected `ActivityHost`; a refused push ends the op in an error and changes nothing. `activity_host.cpp/.h` (`ActivityApplier`, pure, tested) is the `ActivityHost`: `LayoutApplyActivity` under the model lock, then page count, glide to page 1 and the status text. Lock order: standalone mutex, then model lock. `standalone_task` is the device layer: one mutex plus an op task on core 0 (priority 3, polls every tick while a `delay` holds the queue; logs `activity: <old> -> <new>` when a switch commits and `op queue empty (busy N ms)` when the queue drains); `host_link.cpp`'s `on_key` routes standalone keys to it and `swipe_set_tick_cb` (in `swipe.cpp`) runs its tick each frame. `Standalone` runs `hidKey` itself through `HidKeyboard` and forwards every op to `run`.
- **`hid_keyboard.cpp/.h`:** pure, tested HID boot-keyboard report building behind an injected `HidSink` (`usb_hid_send` on the device): tap (down, `HID_TAP_GAP_MS`, up, then `HID_TAP_SETTLE_MS`), down, up and hold (the op queue's `Releasing()` tag says which event queued it); `ReleaseAll()` on takeover, release and disconnect so nothing sticks. Spec: [`docs/standalone.md`](docs/standalone.md).
- **`script_host.cpp/.h`, `script_api.cpp`, `lua_heap.cpp/.h`, `hid_chord.cpp/.h`, `lua_include.h`:** the `script` op (Lua 5.4.7 from the `lua/` submodule, default 64-bit integers and doubles; compiled into `main` and the tests from the one list `firmware/lua_sources.txt`). All pure and tested. `ScriptHost` owns one Lua state on a capped PSRAM heap (`lua_heap`, `SCRIPT_HEAP_CAP` 256 KB), a sandbox (base without `dofile`/`loadfile`/`load`, `string`, `table`, `math`, `utf8`; text chunks only; each script has its own env), a compile cache keyed by the source pointer (compiled on first run, kept until the next re-arm), a fresh coroutine per run, the 100 ms no-yield slice (`SCRIPT_SLICE_MS`) and the 30 s wall timeout (`SCRIPT_WALL_MS`). `script_api.cpp` is the Lua API (list at its top; spec in [`docs/layout-format.md`](docs/layout-format.md#scripts)): each call fills a scratch blob op and runs it through the same executors as the queue. A waited `http.get` uses `NetHost::take_reply`/`abandon` and a 16 KB PSRAM response buffer in `net_worker.cpp`. `hid_chord` parses key chords over `hid_keys.gen.h`: after changing `host/src/bake/hid.js` run `node tools/codegen/gen-hid-keys.js` (a test in `host/` catches drift). `lua_include.h` works around this toolchain leaving `LLONG_MAX` undefined in C++. `Standalone` owns the `ScriptHost` and closes it on every re-arm; `standalone_task.cpp` gives it the PSRAM realloc and `ESP_LOGI("lua")` through `standalone_script_platform()` (shared with the stress run; op task stack 12 KB), and after a run of the op queue in which scripts ran it logs `scripts: N runs, N errors, heap peak N KB, N allocs refused, op stack N bytes free`. A script holds the standalone lock while it runs (up to one slice).
- **`image_store.cpp/.h`:** the host's rle565 images under 32-bit IDs in PSRAM: streamed uploads, replace in place, deferred free, reset; `AddExternal(id, ptr, len)` registers an image inside the layout blob (never copied or freed; all external images must lie in one region that holds nothing else). Read an entry through `ImageEntry.pixels`/`opaque`, and use `PixelsRetired` (not the `ImageData` header) for the compositor's retired check. Platform independent and tested. Spec: [`docs/image-assets.md`](docs/image-assets.md).
- **`key_layout.cpp/.h`:** pages of keys, four image slots each (normal and pressed bg/fg), ref counted; all-or-nothing `SetKeys`. The compositor's client. Tested.
- **`protocol.cpp/.h`:** the mode 1 protocol: stream parser, every request (including SET_STATUS_TEXT: the host sets the status bar's left and right text), event queue. Also the standalone/hosted mode (atomic): `TAKE_CONTROL`, `RELEASE_CONTROL` and disconnects (`PostDisconnect`, `ServiceDisconnect`) all go through `ResetModel`; release rebuilds Home with `LayoutApplyHome`; screen requests are refused while standalone; KEY/PAGE events exist only while hosted. Platform independent and tested. Spec: [`docs/protocol.md`](docs/protocol.md).
- **`model.cpp/.h`:** the device's image store + key layout under one lock; the compositor's `get_page`/`is_retired`.
- **`usb.cpp/.h`:** TinyUSB device: a composite of the vendor interface and a HID keyboard (interface 1, EP 0x82 IN; behind `USB_HID_KEYBOARD`, which follows `CONFIG_TINYUSB_HID_COUNT`: set it to 0, delete the generated sdkconfig and bump `USB_BCD_DEVICE` to back out; the interface map and how to add MIDI are at the top), `usb_hid_send()`, WinUSB/WebUSB, `RESET_STREAM`, vendor read/write; a `disconnected` callback fires on unmount and suspend.
- **`host_link.cpp/.h`:** the protocol task (core 0): USB bytes in, responses and events out; applies status text to `status_set_left/right`. `on_key` is the single switch point for key events (hosted: `Protocol::PostKey`); runs `ServiceDisconnect` after each read pass and sets Home from `layout_summary()` (in `layout_boot`).
- **`main.cpp`:** init and the stats log. The device boots empty (one empty page, no keys, empty status text); the host supplies everything. Logs free heap at boot, then FPS, fill, compositor and image stats once a second.
  - `RUN_STRESS` (off by default; build with `idf.py --preset debug -DCB_RUN_STRESS=ON build`, which embeds `assets/test_keys.bin` as its test images, key layout in `test_keys.h`; `-DCB_RUN_STRESS=OFF` turns it off again; the device is left empty afterwards): at boot, drives the compositor (and status bar) flat out at rest and mid-swipe, plus image replaces at ~USB speed and Lua churning its PSRAM heap on core 0 (a `stress_lua` task, alone and with the compositor; a `lua:` line under each), and logs late fills for each case. Run it after changing the compositor, the bounce fill or anything that touches PSRAM. It takes ~35 s and runs before touch, USB and Wi-Fi start (the board looks hung meanwhile; results are logged at the end). A refused stress key layout is logged (`stress: ... refused`, with the missing image): a case showing `keys 0` without one is a case that doesn't change keys. The results start with a `stress: N demo images` line (refusals, and a check of the PSRAM copy of the images). The churn script is `stress_script.h` (a host test keeps it inside the heap cap). Worst case so far: `lua + keys, mid, flush` at 423 µs (69% of the budget, 0 over).
  - `BACKLIGHT_CYCLE_S` (off by default; `idf.py --preset debug -DCB_BACKLIGHT_CYCLE_S=5 build` toggles the backlight every 5 s and logs `backlight cycle: on/off`; `-DCB_BACKLIGHT_CYCLE_S=0` turns it off again): tests the backlight hardware without the host (and unlocks switching, like `-DCB_BACKLIGHT_SWITCH=ON`: see the backlight Open Item). It calls `display_enable_backlight()` directly, bypassing the touch gate and host state, so don't use it with a host app.
  - `CB_NO_DISPLAY` (off by default; `-DCB_NO_DISPLAY=ON`) skips the panel, the fill ISR and the backlight; `CB_IDLE_FILL` (`-DCB_IDLE_FILL=ON`) keeps the panel clocking but makes the fill do nothing. Both tell display load from RF interference when something else misbehaves (how the Wi-Fi latency was found). Switches stick in the CMake cache; `idf.py` has no `-U`, so remove one with `OFF` (or `=` for `CB_PIXEL_CLOCK_MHZ`). `CB_STATS_LOG` and `CB_WIFI_DIAG` are described under Net ops.

### Bounce fill rules

The fill runs in ISR context:
- Plain row copies and solid fills only. No rle565 decoding, no floating point.
- Keep it well under the ~615 µs budget at 20 MHz (aim < 250 µs). Currently ~120 µs at rest, ~200 µs mid-swipe, up to ~330 µs mid-swipe with the compositor flat out (release `RUN_STRESS`).
- A 15-line chunk costs ~285 µs read cold from PSRAM but ~68 µs in cache. The next chunk is preloaded at the end of each fill (`Cache_Start_DCache_Preload`); only one range can be, so mid-swipe the smaller page is read cold. Anything the fill reads every frame (like the status bar) belongs in internal RAM.
- Per-pixel loops must be fast under `-Og` too (see `fill_px`: 32-bit stores; a naive 16-bit loop cost ~350 µs).
- Content changes are pointer/offset changes latched at frame start; nothing is redrawn in the ISR.
- **Nothing else may write PSRAM heavily while fills run.** Writes from core 1 starve the fill's PSRAM reads. The compositor writes page buffers, and the status bar its buffers, only in the gap after a fill, a measured slice at a time; they share the gaps through `fill_gap`. The status bar is read from PSRAM by the fill, so `screen.cpp` preloads its rows like the pages'. Anything new that writes a lot of PSRAM (icon cache, labels) needs the same treatment, or a stress run to show it's safe.

## Screen and Keys

```
+------------------------------------------+
| status bar              — fixed (40 px)  |
+------------------------------------------+
|  [ ]   [ ]   [ ]   [ ]   [ ]             |
|  [ ]   [ ]   [ ]   [ ]   [ ]   — page    |  swipes left/right
|  [ ]   [ ]   [ ]   [ ]   [ ]             |
+------------------------------------------+
  (pager bar: disabled for now)
```

- 5x3 keys per page, 128x128 cells, several pages swiped left/right. Page area 800x440. Black background throughout.
- **Pager bar disabled** (`SCREEN_PAGER_H 0`); set it to 20 to bring it back (the key gap adjusts). The status bar must stay at least 15 lines tall (`static_assert` in `screen.cpp`).
- **Swipe feel:** very smooth, like the iPhone home screen. No LVGL-style spring-back, no overshoot.
- **Key images:** each key names four host images by ID: normal bg/fg and pressed bg/fg (the device swaps to the pressed pair itself). Faces, icons and labels are all drawn by the host; labels are baked in. No stock face, tint or font in the firmware. Spec: [`docs/image-assets.md`](docs/image-assets.md).
- **Icon size:** about 80–96 px square, still being tuned. Keep sizes as firmware constants reported to the host (GET_INFO); don't hard-code them elsewhere.

## Rendering

All custom. Do **not** use LVGL.

[rle565](https://github.com/toptensoftware/rle565): a C renderer for run-length encoded RGB565 images and anti-aliased text (alpha, clipping, fade, recolouring; needs 4-byte-aligned data), and a Node.js encoder (PNG to `.rle565`, fonts to `.rfnt`). Assets are packed into blobpacks by `tools/test-icons/gen.js`. `check_image()` validates a whole image (and reports if it's opaque) without writing pixels: use it for anything from outside.

## Communication (mode 1)

**Spec:** [`docs/protocol.md`](docs/protocol.md) is the source of truth. Keep firmware, `host/` and the spec in sync.

- USB vendor-class interface (one bulk IN, one bulk OUT) on the native port via `esp_tinyusb`. Bulk rather than HID for speed.
- No driver needed: MS OS 2.0 descriptors (WinUSB) and WebUSB BOS descriptors (see TinyUSB's `webusb_serial` example). Linux needs a udev rule. Fallback if troublesome: CDC-ACM.
- Clients: Node.js (`usb` package 3.x, which follows the WebUSB API) and browsers (WebUSB), same code in `host/src/ControlBlock.js`. Quirks of `usb` 3.x: a read timeout throws "Cancelled"; `controlTransferOut` needs a data argument.
- **A cancelled bulk IN read deafens the pipe (Windows/WinUSB):** once a `transferIn` times out and is cancelled, the device still receives requests but nothing it sends reaches the host until the device is reopened. `clearHalt` and `RESET_STREAM` don't recover it, and a pending read can't be cancelled on close. So reads must never time out on a live device: the firmware sends a `HEARTBEAT` event (`0x83`) about once a second, written straight out (not queued) and only while half the transmit FIFO is free, and the host's read timeout (`READ_TIMEOUT_MS`, 5 s) is well past it. `close()` on an idle connection therefore takes up to a second. Anything that changes the read loop, the timeout or the heartbeat must keep this. See [`docs/protocol.md`](docs/protocol.md) (HEARTBEAT).
- **Hosts must send a ZLP after any write that is a multiple of 64 bytes:** the device receives in 512-byte transfers that only end when full or at a short packet, so without it the tail of the message sits on the device and the request times out. `ControlBlock._write` does it, and the test `FakeDevice` holds back data like the device. See [`docs/protocol.md`](docs/protocol.md#zero-length-packets).
- Development IDs `303a:4020`. Windows caches the MS OS 2.0 descriptor per VID/PID/bcdDevice: bump `USB_BCD_DEVICE` in `usb.cpp` when it changes.
- Composite device from the start, so standalone mode can add HID keyboard/MIDI interfaces.
- Vendor bulk carries every mode-1 message (not split with HID: one interface keeps uploads and key definitions in order).
- Images go rle565-encoded under host-chosen 32-bit IDs (0 = none; a CRC makes a good one) and stay on the device, so the host only resends what changed. Uploading to an existing ID replaces it everywhere; freeing is deferred while keys still use the image. See [`docs/image-assets.md`](docs/image-assets.md).
- **Host-side key images:** [node-buttonImage](https://github.com/toptensoftware/node-buttonImage) renders faces and icons from SVG to RGBA (stock icons `svgFile: "#name"` make handy test content); `host/` encodes them to rle565 with alpha, 128x128 with transparent padding.
- **Web front end:** undecided: hosted on the device (Wi-Fi) or a public site using WebUSB.

## Open Items

- If dragging looks steppy: use the GT911 interrupt and/or predict the finger position per frame.
- Untested: the `-Og` build under the stress run.
- **Internal RAM:** measured: release build, with the net worker: ~130 KB free after init, ~108 KB with the Wi-Fi driver up and ~106 KB connected (largest block 64 KB; a debug build has ~95 KB); Wi-Fi connects to WPA2 and WPA3 networks ([`docs/wifi.md`](docs/wifi.md)). Further savings if needed: the protocol buffer and stacks to PSRAM, or Wi-Fi/lwIP buffers to PSRAM (with a stress run).
- **Wi-Fi:** connects on the hardware, and credentials survive a reboot and reflash. NVS write glitches aren't a concern (rarely written). Not yet checked: wrong password stopping retries (until new credentials or a reboot; not shown on the status bar yet) and recovery after a router restart. Later: power save for short battery runs (it is off now, for latency; see [`docs/wifi.md`](docs/wifi.md)).
- **Status bar in PSRAM:** works on the hardware, 0 late fills in the release `RUN_STRESS` (slowest fills 121 µs at rest, 286-353 µs mid-swipe, 412 µs for image replaces mid-swipe; Lua + keys mid-swipe, 423 µs, now has the least margin), and the pager pill lag looks fine. Mid-swipe the fill is above the 250 µs aim. Fallback if fills ever go late: small internal buffers for the text and pills, the rest of the bar filled black in the fill ([`docs/status-bar.md`](docs/status-bar.md)).
- **Second render cell** (render the next key while one copies): ~20% faster page builds. Not needed yet.
- **Key presses:** the touch router is built and unit tested, and wired to the model and `Protocol::PostKey`, but **not yet run on the hardware**. Tune `TOUCH_SWIPE_WINDOW_MS` and `TOUCH_EDGE_AMBIGUOUS_PX`, and judge the pressed look flashing on swipes (it shows at touch-down and reverts when a swipe starts). The swipe task takes the model lock on key actions: check it doesn't delay frames. Per-key "no swipe window" mode: add only if needed.
- **Not yet run on the hardware:** `examples/bench.js`.
- **Backlight wake-on-touch:** built and unit tested, not run on the hardware. Hardware-only checks: GT911 reporting while the light is off, wake feel.
- **Backlight switch-on glitch (corrupts PSRAM):** when the backlight turns on, the CH343P UART USB port disconnects and reconnects (Windows connect sound), and **PSRAM transfers in progress are corrupted**: sparse bit errors over ~1 KB, shortly after the switch-on (found 2026-11-07: the stress run's copy of `test_keys.bin`, made just after the backlight came on, differed from the original in 16-448 bytes; `wip/wip-lua-hardware-checks.md`). Code, rodata, page buffers, the layout blob and the Lua heap are all in PSRAM. **The backlight is locked on until the user has experimented with the supply:** `ch422g_init`'s `INITIAL_OUT` keeps it on (it is on at power-up, per Waveshare), and `display_enable_backlight()` does nothing unless built with `-DCB_BACKLIGHT_SWITCH=ON` (or `CB_BACKLIGHT_CYCLE_S`). `SET_BACKLIGHT`, `GET_INFO` and the wake gate still work, so a host's "off" leaves the screen lit with touches gated; the layout upload no longer darkens the screen. The likely cause is a 3.3 V/5 V supply dip. Experiments, the MP3302 PWM/soft-start mod (costs the macro pad's pin) and the full write-up: [`docs/backlight.md`](docs/backlight.md) (Hardware). A software fallback (quiet PSRAM writers around a switch-on) can't protect code fetches.
- Host library: long and double press helpers (protocol.md says the host detects them).
- **Bake, left for later:** device limit checks (pages, images, blob size, op list length; the device limit of 32 is noted under activity switching), the "icon larger than the icon area" warning, PNG layers in `image`, and a browser renderer. The bake core (`host/src/bake/`) also uses Node's `Buffer` global, which keeps it out of the browser entry (`host/browser.js`) until it is shimmed.
- Flash writes (icon cache, NVS) may stall the bounce ISR (`CONFIG_LCD_RGB_ISR_IRAM_SAFE` is off). Test for display glitches when they arrive.
- **Layout upload:** works on the hardware (16 KB sample: blank, write, restart, re-enumerate, verify, skip when unchanged, `--force`, survives a reboot). Not yet checked: a host that vanishes mid-upload leaving the screen dark until the next RESET_STREAM (an upload idle timeout would fix it), a large blob (64 KB block erases, over-capacity refusal) and display glitches during erase. `node cli/cli.js upload` / `host/` `uploadLayout()` are the clients.
- **Layout boot load:** works on the hardware (a flashed layout loads). Not yet checked: the load time, whether a real large layout fits in PSRAM (the blob stays resident, so hosted mode loses that space), and `RUN_STRESS` with a stored layout (Home's keys are present when it starts). The release rebuild of Home is built and host tested but not run on the hardware (next item).
- **Take/release control and standalone key gestures (milestone steps 1 and 2):** work on the hardware (cold boot to Home, take/release, unplug and suspend return to Home with the light on, gestures and the frame lock fine). `SET_BACKLIGHT` stays allowed in both modes. Held keys are dropped silently on takeover, and the HID keys are released (`ReleaseAll`) by the takeover and by an activity switch.
- **Standalone HID (milestone step 3):** works on the hardware (tap, chord, hold, nothing sticks on takeover/unplug, vendor interface and upload still fine). Tune `HID_TAP_GAP_MS` / `HID_TAP_SETTLE_MS` (20 ms each) if keys are dropped or merged. `usb_hid_send` is called from the op and protocol tasks: watch for the "HID report dropped" warning under load. Held keys are tracked per modifier bit, not counted. The first milestone is complete (step 4 passed).
- **Net ops** (`httpGet`, `httpPost`, `udpPacket`, `sendWol`): `net_host.h` (injected `NetHost`), `net_queue.cpp/.h`, `exec_net.cpp/.h` and `wol_packet.h` are pure and tested; `net_worker.cpp/.h` is the device layer (worker task on core 0, esp_http_client over http only, lwIP sockets). **Work on the hardware** (2026-11-06, `examples/net-test.json` against `examples/test-server/net_test_server.py`: GET with headers, form and JSON POST, redirect, 404, server hang and unreachable host time out, 5 MB download, connection reused per host and reopened on a host change, UDP by IP/name/hex, WoL, a 12-request burst drops the oldest 4 as designed with `NET_QUEUE_SIZE` 8). Each finished request logs one `net:` line (status and body size). `esp_http_client_delete_all_headers` also drops the client's own `Host` and `User-Agent`, and with no headers left the client sends nothing (not even the request line) and times out: `set_headers` in `net_worker.cpp` puts them back (`NetUrlHost`) before the op's headers. Build switches: `-DCB_STATS_LOG=ON` (once-a-second stats line with net counters) and `-DCB_WIFI_DIAG=ON` (Wi-Fi link diagnostics); presets `debug-iram`/`release-iram` (`sdkconfig.wifi_iram`, Wi-Fi in IRAM, an experiment, not the default).
- **Activity switching and devices:** **work on the hardware** (2026-11-07, `examples/activity-test.mjs` against `examples/test-server/net_test_server.py`; checklist and results in `wip/complete-activity-device-management.md`, step 9): page swap and glide with no stale keys, grow/shrink/none, held HID keys released on a switch, rapid A->B->C in order, the full-queue refusal, shared devices not power cycled, a `delay` warm-up holding touches, takeover mid-switch (Home, no activity, mask kept), soak, `RUN_STRESS` with the layout stored. Fills ~137 µs typical, under 200 µs during a switch, so the `check_image` cost under the model lock is fine. Not yet tried with real equipment. Follow-ups: a busy indicator while the op queue runs; the bake doesn't check the device limit (32), so an over-limit layout is only refused at boot. Activity pages use the global long-press time (per-key `longPressMs` there is not needed).
- **Lua scripts:** built and host tested (`wip/complete-lua-scripts.md`). The `RUN_STRESS` Lua cases passed on the hardware (2026-11-07: 0 late fills, worst fill 423 µs with the compositor, 0 script errors, heap peak 80 KB; checklist A in `wip/wip-lua-hardware-checks.md`). Internal RAM with scripts to be re-read from a normal build (the stress build gave 111 KB free after init, 87 KB with Wi-Fi connected). **Scripts not yet run from a layout on the hardware**; checks waiting to run (checklist B there, `examples/script-test.mjs`): the op task's stack high-water mark with a deeply recursive script, the `http.get` reply path end to end (timeout, abandon on takeover), button-to-request latency, and key-event latency while a script is mid-slice (it holds the standalone lock up to 100 ms). Compiled scripts stay cached until the next takeover/release, not freed per activity: revisit if a layout has many large scripts.

## Conventions

- **No STL / `std::`.** Never use the C++ standard library (containers, strings, smart pointers, algorithms, `<functional>`, etc.). Use [SimpleLib](https://github.com/toptensoftware/SimpleLib) `Core` instead (`#include "Core.h"`; include path `SimpleLib/`): `String`, `StringBuilder`, `List`, `Map`, `Set`, `Buffer`, `OwnedPtr`, `SharedPtr`, `Delegate`, etc. Don't pull in other SimpleLib modules. Use the `simplelib` skill (`SimpleLib/.claude/skills`) for its API.
  - SimpleLib is new to ESP-IDF: fix teething issues and note them here rather than falling back to STL. Patched in the submodule for GCC (`-std=gnu++26 -fno-exceptions -Werror`), committed to SimpleLib's main: `#pragma warning` in `HashCore.h` wrapped in `#ifdef _MSC_VER`; `Delegate.h` includes `<cstddef>`; `Map.h`/`Set.h` name their dependent base as `HashCore<TAllocator>`; an unused variable removed from `HashCore::Find`.
  - SimpleLib's `List` is move-only and its `operator[]` returns a copy: write through `GetBuffer()[i]`. A struct holding a `List` can't go in another `List`.
  - The SimpleLib repo has moved to `github.com/toptensoftware/simplelib` (the old URL redirects); `.gitmodules` still has the old one.
  - C runtime headers (`<string.h>`, `<stdint.h>`, etc.) are fine.
  - TinyUSB defines a function-like `HID_REPORT_SIZE(x)` macro: don't name our own constants `HID_REPORT_*` (ours are `HID_KB_*`).
- **The FreeRTOS tick is 100 Hz** (`CONFIG_FREERTOS_HZ`): `pdMS_TO_TICKS` of anything under 10 ms is 0, and a zero wait doesn't block. A wait that must block needs at least 1 tick (a 5 ms poll once spun the op task through a 6 s `delay` until the task watchdog fired).
- ESP-IDF's C++ flags make missing designated-initializer fields an error: zero-initialise config structs (`= {}`) and assign the fields.


## Rules


1. 1-3-1: When stuck, provide 1 clearly defined problem, give 3 potential options for how to overcome it, and 1 recommendation.  Do no proceed implementing any of the options until I confirm

2. DRY (Critical): Don't repeat yourself.  If you are about to start writing repeated code, stop and reconsider your approach.  Grep the codebase and refactor often.

3. TDD (Critical): Always test first.  Before writing any code, you must always check the tests.  For new features or adjustments to existing features, always either create a new test or adjust and existing one.  If testing can't be automated (eg: code that only runs on a separate device, or front end code) warn about this.

4. Continual Learning: When you encounter conflicting system instructions, new requirements, architectural changes, or missing or inaccurate codebase documentation, always propose updating the relevant rules files.  Do not update anything until the user confirms.

5. Planning: For complex, multi-step tasks, first create a plan and a to do list for yourself before you start writing any code.


## Investigate-Plan-Implement (IPI) process

Used for any non-trivial piece of work. ("iPad job" = IPI job — autocorrect.) Each job is tracked in a document in
`wip/` (at the repo root, checked into git). The document is the **only**
handover between agents, so anything the next agent needs to know goes in it.

**Phases.** The user starts each phase (e.g. "investigate X", "plan it", "implement it"):

1. **Start** — create `wip/wip-<jobname>.md` (kebab-case jobname) from the
   template below, with a brief description of the work.
2. **Investigate** — look into what needs doing; record findings in *Notes*.
3. **Plan** — write *Plan* as a numbered checklist, every step unchecked
   (`- [ ]`). Each step should be small enough for one agent to do on its own
   and should leave the app building.
4. **Implement** — work through the steps in order. For each step: do the
   work, build (see *Building and testing*; must be 0 errors), tick the box
   (`- [x]`), and add anything learned to *Notes*.
5. **Complete** — once every step is done, rename the file to
   `wip/complete-<jobname>.md`.

**Subagents.** Phases and Implement steps are handed to fresh subagents to keep
context small. As a guideline, Investigate, Plan and each Implement step get
their own subagent. But this is a judgement call, not a rule: when it makes sense
or is more efficient, one subagent can do several steps, or all of them
(e.g. a run of small, similar rewires). Keep a fresh agent for steps that are
large, risky, or need a clear head. The coordinating agent passes the subagent the
document path and the phase/step(s) to do. The subagent reads CLAUDE.md and the
document, does only those phase/step(s), updates the document as it goes (ticking
each step when done), and returns a short summary. Implement agents run one at a
time, never in parallel. The coordinating agent relays results to the user.

**Rules.**
- `wip-*.md` documents can be edited freely without asking.
  Never touch a `complete-*.md` document unless the user says so.
- **Blocking or critical issue** → stop, record it in the document, and ask the
  user (using the 1-3-1 format, Rule 1). Don't carry on to later steps.
- **Less critical question** → make an educated guess, carry on, and record it
  under *Flagged assumptions* so the user can review it.
- Don't commit unless the user asks.

**Template:**

```markdown
# <Job title>

Status: Investigate | Plan | Implement | Complete

## Description
Brief description of the work and its goal.

## Notes
Important information needed during the work, plus anything discovered along
the way (relevant files, APIs, gotchas, decisions made).

## Flagged assumptions
- Educated guesses made instead of asking, for the user to review.

## Plan
- [ ] 1. ...
- [ ] 2. ...
```


## Preventing Permission Prompts

- Use the Read/Edit/Grep tools rather than sed/awk/cat for file work.
- Don't run inline scripts (node -e, python -c, heredocs, piped code). Write the
  script to a file in the repo (e.g. scratch/foo.js) and run it by path.
- Use literal file paths in shell commands, not variables or $(...) substitutions.
