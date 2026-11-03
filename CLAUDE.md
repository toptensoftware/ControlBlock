# ControlBlock

A StreamDeck-style control surface: a grid of configurable touch "buttons" on an LCD panel that trigger actions on a host machine.

## Hardware

Board: [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm)

- **SoC:** ESP32-S3N16R8: dual-core 240 MHz, 16 MB flash, 8 MB octal PSRAM, Wi-Fi/BLE.
- **Display:** 800x480 RGB565 parallel RGB panel. **Touch:** GT911, 5-point, I2C (SDA 8, SCL 9, 400 kHz); INT is GPIO4.
- **CH422G IO expander** (I2C): drives touch reset, backlight, LCD reset, SD CS and the USB/CAN mux (low = native USB), so these aren't plain GPIOs. Always go through `ch422g.cpp`.
- **USB:** native USB (TinyUSB device: vendor now, HID/MIDI later) and a CH343P USB-UART port (flashing and logs). The firmware takes the native port, so it's no longer a serial/JTAG port: flash and monitor over the UART port (or hold BOOT and press RESET to flash over native USB).

## Building

ESP-IDF v6.0 at `C:\esp\v6.0\esp-idf`, tools in `C:\Espressif`. From PowerShell in `firmware/`:

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue   # idf.py refuses to run under Git Bash's MSYS variable
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
idf.py --preset debug build              # or: idf.py --preset release -p COMx flash monitor
```

- Presets (`firmware/CMakePresets.json`): `debug` (`build/debug`, `-Og`, assertions on; the default) and `release` (`build/release`, `-O2`, `NDEBUG`; use for timing). `$env:IDF_PRESET = "release"` skips `--preset`.
- Settings: `firmware/sdkconfig.defaults` plus `sdkconfig.release`; the sdkconfigs under `build/` are generated. Notable: octal PSRAM at 80 MHz, 64 KB data cache with 64-byte lines, `LCD_RGB_RESTART_IN_VSYNC` (a late bounce fill costs one bad frame instead of a lasting shift). Code and rodata are mapped to PSRAM.
- Logs go to UART0 (CH343P) only (`ESP_CONSOLE_SECONDARY_NONE`: TinyUSB has the native port).
- Components: `esp_tinyusb` (2.x) and the GT911 driver, from the component manager (`main/idf_component.yml`). TinyUSB's vendor FIFO sizes are in `sdkconfig.defaults`. After changing `sdkconfig.defaults`, delete (or move aside) `build/<preset>/sdkconfig` so the change applies: values already in a generated sdkconfig win over the defaults.
- `CONFIG_TINYUSB_SUSPEND_CALLBACK=y` is in `sdkconfig.defaults` (USB suspend releases host control): delete the generated `build/<preset>/sdkconfig` once for it to apply.
- The user flashes and tests on the hardware.

### Host unit tests

`firmware/test/` builds the platform-independent firmware code into a Windows exe with SimpleLib's `UnitTesting.h` (see the `simplelib` skill). From `firmware/test`:

```sh
mk run                                   # build and run (~13 s); exit code 1 on failure
FUZZ_SEEDS=200 mk config=release run     # long fuzz soak
```

- A test that overruns a buffer can silently corrupt the test registry's statics in the debug exe (the runner then skips tests and still reports "All passed"): if the debug and release test counts differ, suspect an out-of-bounds write in a test. This once hid 81 tests (`TestStatusBar` rendered 7-row bands into a 4-row scratch).

- Needs `mk` (`npm install -g toptensoftware/mk.js`) and MSVC. Reads `firmware/assets/*.bin` (`LoadAsset()`). Sources are listed in `test.mk.js`.
- `harness.h/.cpp`: a `World` runs a real `PageCompositor` against a fake client, bounce fill and clock, checking its invariants at every frame start. Preempt hooks run frame starts and client changes inside `Step()` to test races deterministically. `RetireUnused()` exercises sweeps: freed images are poisoned and their addresses reused straight away.
- `store_env.h/.cpp`: the image store's test platform (an allocator that poisons and reuses freed blocks, a fake sweep), shared by the store, key layout and protocol tests. Protocol tests feed raw bytes in and decode the responses. `TestGapShare.cpp` covers the fill-gap round-robin, and `TestStatusBar.cpp` checks that the banded status bar render matches the whole-bar render byte for byte.
- Keep device-only code (FreeRTOS, ESP-IDF, TinyUSB) out of the tested files: use `port.h`, pure functions and injected callbacks (`ImageStoreHost`, `ProtocolHost`), with a thin device layer around them (`compositor.cpp`, `screen.cpp`, `status.cpp`, `model.cpp`, `host_link.cpp`, `usb.cpp`).

### Host library tests

From `host/`: `npm install` once, then `npm test` (Node's test runner, no device: `test/FakeDevice.js` stands in for the WebUSB device). On a device: `npm run demo` and `node demo/bench.js`.

### Bake tool and binpack tests

From `tools/bake/`: `npm install` once, then `npm test` (no device; includes a CLI test that renders real images). From `binpack/`: `node --test test.js`.

## Repository Layout

```
docs/              Specs: protocol.md, image-assets.md, page-compositor.md, status-bar.md, interactions.md, backlight.md, standalone.md, config-format.md, config-upload-plan.md, wifi.md, standalone-ops-plan.md (plan for the remaining ops and Lua; not yet approved), backlight-waveshare-support.md (text for a Waveshare support request) (screen-buffers.md is partly superseded)
firmware/          ESP-IDF project for the device (C++)
  assets/          Generated binaries embedded in the firmware (ui.bin; test_keys.bin only for the stress run)
  test/            Host unit tests (test/data/sample_config.json and .bin: a sample config baked by the real tool)
host/              Node.js client library (WebUSB API over the `usb` package or a browser; mode 1), demo and bench scripts
web/               Optional web front end for configuring the device
rle565/            Submodule: rle565 renderer (src/) and encoder (encoder/)
node-buttonImage/  Submodule: host-side key image rendering
SimpleLib/         Submodule: C++ utility library used instead of STL (only Core/ is used)
binpack/           Submodule: packs JS objects into C-struct blobs (BPAK); used by the bake tool for the standalone config
tools/test-icons/  Generates firmware/assets/*.bin (npm install && npm run build)
tools/bake/        Bakes a standalone config file (docs/config-format.md) into the BPAK blob for the device (npm install, then node cli.js config.json -o out.bin)
scratch/           Throwaway scripts (gitignored)
```

Clone with `--recursive` or run `git submodule update --init`.

## Operating Modes

1. **Host-controlled:** Like a Stream Deck. PC software decides what each key shows; the device reports key events and the PC acts on them. Long press and double press are detected host side. Built.
2. **Standalone (first milestone built and verified on the hardware; the default):** the device boots into it. It holds a Home page plus "activities" (each a set of pages) in flash, and keys run actions on the device: USB HID keyboard first, then MIDI, HTTP and UDP. A host must send `TAKE_CONTROL` before any layout request (they are refused with `NOT_HOSTED` while standalone); `RELEASE_CONTROL`, USB unmount or USB suspend returns the device to Home and turns the backlight on. The milestone (boot into Home, touch gestures and bindings, `hidKey` over a USB HID keyboard, take/release control) works on the hardware; activity switching, devices, scripts and HTTP/UDP/WoL are not built (see [`docs/standalone-ops-plan.md`](docs/standalone-ops-plan.md)). Spec: [`docs/standalone.md`](docs/standalone.md).

## Firmware

Modules in `firmware/main/` (all C++; `rle565.c` from the submodule stays C):

- **Drivers** (`waveshare_rgb_lcd_port`, `waveshare_touch_port`, `i2c`, `ch422g`): adapted from the sister project `../aclock/clock/main/`; adapt these, don't write new ones. Init order: `i2c_init()`, `ch422g_init()`, display, `touch_init()`.
  - `display_init_bounce(fill_cb, ctx)`: bounce-buffer mode, no framebuffers. `fill_cb` gets 15 lines at a time, in order (`y == 0` starts a frame); budget `DISPLAY_BOUNCE_BUDGET_US` (~492 µs) per fill, ~61 fps.
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
- **`config_store.cpp/.h`, `config_flash.cpp/.h`, `crc32.h`:** the standalone config upload to the `config` flash partition (`partitions.csv`: app 2 MB, config 4 MB; header sector written last). `config_store` is platform independent and tested over a fake NOR flash; `config_flash` is the device layer; `host_link.cpp` blanks the screen during an upload and restarts after the commit. Spec: [`docs/protocol.md`](docs/protocol.md#configuration-upload), [`docs/config-upload-plan.md`](docs/config-upload-plan.md). Run on the hardware with a small blob. A changed partition table needs a full `idf.py flash` (not app only). The boot-time loader isn't built.
- **`config_blob.cpp/.h`, `config_apply.cpp/.h`, `config_boot.cpp/.h`:** the standalone config at boot. `config_blob` (pure, tested) validates a BPAK blob and relocates its pointers, walking it by offsets rather than casting to structs (host tests are 64-bit); the struct layouts are written out in the file, so keep them in step with the binpack schema and re-bake `test/data/sample_config.bin` after any schema change (`node cli.js ../../firmware/test/data/sample_config.json -o ../../firmware/test/data/sample_config.bin` from `tools/bake`). `config_apply` (pure, tested) registers Home's images and sets page 0's keys, all or nothing. `config_boot` (device) reads the partition into PSRAM before the display starts (`config_boot_load`, before `model_init`/`screen_init`) and applies it after `screen_init`, before the backlight. A missing, damaged or invalid config leaves the empty Home. The blob stays resident in PSRAM. `ConfigStore::Read` reads and CRC-checks the stored blob.
- **`config_layout.h`, `config_view.h`, `op_queue.cpp/.h`, `trigger_dispatch.cpp/.h`, `key_gesture.cpp/.h`:** the standalone trigger side (all platform independent and tested; wired to the device by `standalone.cpp` and `standalone_task.cpp` below, works on the hardware). `config_layout.h` holds the packed struct offsets and op codes shared with `config_blob.cpp`; `ConfigView` reads the blob by offset (adjust 0 on the device, the blob address in the host tests). `TriggerDispatcher` matches (control, event) against Home's and the selected activity's bindings and pushes their op lists on the `OpQueue` (32 entries; `delay` holds it; every op goes to an injected `run` callback). `GestureBank` turns press/release/cancel into press, release, tap and long per control. None of them lock: the device layer serialises. Spec: [`docs/standalone.md`](docs/standalone.md).
- **`standalone.cpp/.h`, `standalone_task.cpp/.h`:** the owner of the trigger side. `Standalone` (pure, tested with the sample blob; injected clock and `run`, no lock) holds the `ConfigView`, `OpQueue`, `TriggerDispatcher` and `GestureBank`: `Key(page, key, action, time_ms)`, `Tick()`, `Poll()`, `SetHosted(bool)` (both directions re-arm from the blob: held keys and queued ops dropped silently, no activity selected). `standalone_task` is the device layer: one mutex plus an op task on core 0 (priority 3, polls every 5 ms while a `delay` holds the queue); `host_link.cpp`'s `on_key` routes standalone keys to it and `swipe_set_tick_cb` (in `swipe.cpp`) runs its tick each frame. `Standalone` runs `hidKey` itself through `HidKeyboard` and forwards every op to `run`.
- **`hid_keyboard.cpp/.h`:** pure, tested HID boot-keyboard report building behind an injected `HidSink` (`usb_hid_send` on the device): tap (down, `HID_TAP_GAP_MS`, up, then `HID_TAP_SETTLE_MS`), down, up and hold (the op queue's `Releasing()` tag says which event queued it); `ReleaseAll()` on takeover, release and disconnect so nothing sticks. Spec: [`docs/standalone.md`](docs/standalone.md).
- **`image_store.cpp/.h`:** the host's rle565 images under 32-bit IDs in PSRAM: streamed uploads, replace in place, deferred free, reset; `AddExternal(id, ptr, len)` registers an image inside the config blob (never copied or freed; all external images must lie in one region that holds nothing else). Read an entry through `ImageEntry.pixels`/`opaque`, and use `PixelsRetired` (not the `ImageData` header) for the compositor's retired check. Platform independent and tested. Spec: [`docs/image-assets.md`](docs/image-assets.md).
- **`key_layout.cpp/.h`:** pages of keys, four image slots each (normal and pressed bg/fg), ref counted; all-or-nothing `SetKeys`. The compositor's client. Tested.
- **`protocol.cpp/.h`:** the mode 1 protocol: stream parser, every request (including SET_STATUS_TEXT: the host sets the status bar's left and right text), event queue. Also the standalone/hosted mode (atomic): `TAKE_CONTROL`, `RELEASE_CONTROL` and disconnects (`PostDisconnect`, `ServiceDisconnect`) all go through `ResetModel`; release rebuilds Home with `ConfigApplyHome`; layout requests are refused while standalone; KEY/PAGE events exist only while hosted. Platform independent and tested. Spec: [`docs/protocol.md`](docs/protocol.md).
- **`model.cpp/.h`:** the device's store + layout under one lock; the compositor's `get_page`/`is_retired`.
- **`usb.cpp/.h`:** TinyUSB device: a composite of the vendor interface and a HID keyboard (interface 1, EP 0x82 IN; behind `USB_HID_KEYBOARD`, which follows `CONFIG_TINYUSB_HID_COUNT`: set it to 0, delete the generated sdkconfig and bump `USB_BCD_DEVICE` to back out; the interface map and how to add MIDI are at the top), `usb_hid_send()`, WinUSB/WebUSB, `RESET_STREAM`, vendor read/write; a `disconnected` callback fires on unmount and suspend.
- **`host_link.cpp/.h`:** the protocol task (core 0): USB bytes in, responses and events out; applies status text to `status_set_left/right`. `on_key` is the single switch point for key events (hosted: `Protocol::PostKey`); runs `ServiceDisconnect` after each read pass and sets Home from `config_summary()` (in `config_boot`).
- **`main.cpp`:** init and the stats log. The device boots empty (one empty page, no keys, empty status text); the host supplies everything. Logs free heap at boot, then FPS, fill, compositor and image stats once a second.
  - `RUN_STRESS` (off by default; build with `idf.py --preset debug -DCB_RUN_STRESS=ON build`, which embeds `assets/test_keys.bin` as its test images, layout in `test_keys.h`; `-DCB_RUN_STRESS=OFF` turns it off again; the device is left empty afterwards): at boot, drives the compositor (and status bar) flat out at rest and mid-swipe, plus image replaces at ~USB speed, and logs late fills for each case. Run it after changing the compositor, the bounce fill or anything that touches PSRAM.
  - `BACKLIGHT_CYCLE_S` (off by default; `idf.py --preset debug -DCB_BACKLIGHT_CYCLE_S=5 build` toggles the backlight every 5 s and logs `backlight cycle: on/off`; `-DCB_BACKLIGHT_CYCLE_S=0` turns it off again): tests the backlight hardware without the host. It calls `display_enable_backlight()` directly, bypassing the touch gate and host state, so don't use it with a host app.

### Bounce fill rules

The fill runs in ISR context:
- Plain row copies and solid fills only. No rle565 decoding, no floating point.
- Keep it well under the ~492 µs budget (aim < 250 µs). Currently ~115 µs at rest, ~200 µs mid-swipe, up to ~360 µs mid-swipe with the compositor flat out.
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
- Development IDs `303a:4020`. Windows caches the MS OS 2.0 descriptor per VID/PID/bcdDevice: bump `USB_BCD_DEVICE` in `usb.cpp` when it changes.
- Composite device from the start, so standalone mode can add HID keyboard/MIDI interfaces.
- Vendor bulk carries every mode-1 message (not split with HID: one interface keeps uploads and key definitions in order).
- Images go rle565-encoded under host-chosen 32-bit IDs (0 = none; a CRC makes a good one) and stay on the device, so the host only resends what changed. Uploading to an existing ID replaces it everywhere; freeing is deferred while keys still use the image. See [`docs/image-assets.md`](docs/image-assets.md).
- **Host-side key images:** [node-buttonImage](https://github.com/toptensoftware/node-buttonImage) renders faces and icons from SVG to RGBA (stock icons `svgFile: "#name"` make handy test content); `host/` encodes them to rle565 with alpha, 128x128 with transparent padding.
- **Web front end:** undecided: hosted on the device (Wi-Fi) or a public site using WebUSB.

## Open Items

- If dragging looks steppy: use the GT911 interrupt and/or predict the finger position per frame.
- Untested: the `-Og` build under the stress run.
- **Internal RAM:** measured: ~143 KB free after init (largest block 102 KB), ~121 KB with the Wi-Fi driver up and ~119 KB connected (largest block 76 KB); Wi-Fi connects to WPA2 and WPA3 networks ([`docs/wifi.md`](docs/wifi.md)). Further savings if needed: the protocol buffer and stacks to PSRAM, or Wi-Fi/lwIP buffers to PSRAM (with a stress run).
- **Wi-Fi:** connects on the hardware, and credentials survive a reboot and reflash. NVS write glitches aren't a concern (rarely written). Not yet checked: wrong-password backoff and recovery after a router restart. Later: power save for short battery runs (it is off now, for latency; see [`docs/wifi.md`](docs/wifi.md)).
- **Status bar in PSRAM:** works on the hardware, 0 late fills in the release `RUN_STRESS` (slowest fills 121 µs at rest, 286-353 µs mid-swipe, 412 µs for image replaces mid-swipe, the least margin), and the pager pill lag looks fine. Mid-swipe the fill is above the 250 µs aim. Fallback if fills ever go late: small internal buffers for the text and pills, the rest of the bar filled black in the fill ([`docs/status-bar.md`](docs/status-bar.md)).
- **Second render cell** (render the next key while one copies): ~20% faster page builds. Not needed yet.
- **Key presses:** the touch router is built and unit tested, and wired to the model and `Protocol::PostKey`, but **not yet run on the hardware**. Tune `TOUCH_SWIPE_WINDOW_MS` and `TOUCH_EDGE_AMBIGUOUS_PX`, and judge the pressed look flashing on swipes (it shows at touch-down and reverts when a swipe starts). The swipe task takes the model lock on key actions: check it doesn't delay frames. Per-key "no swipe window" mode: add only if needed.
- **Not yet run on the hardware:** `demo/bench.js`.
- **Backlight wake-on-touch:** built and unit tested, not run on the hardware. Hardware-only checks: GT911 reporting while the light is off, wake feel.
- **Backlight-on USB glitch:** when the backlight turns on, the CH343P UART USB port disconnects and reconnects (Windows connect sound), with both USB ports powered. Cause unknown: the CH422G bytes match `aclock`'s and `USB_SEL` is untouched, and a web search found nothing. Isolate with `BACKLIGHT_CYCLE_S`: check for an ESP32 reboot in the log, try with only one cable, and see whether it happens on every wake.
- Host library: long and double press helpers (protocol.md says the host detects them).
- Flash writes (icon cache, NVS) may stall the bounce ISR (`CONFIG_LCD_RGB_ISR_IRAM_SAFE` is off). Test for display glitches when they arrive.
- **Config upload:** works on the hardware (16 KB sample: blank, write, restart, re-enumerate, verify, skip when unchanged, `--force`, survives a reboot). Not yet checked: a host that vanishes mid-upload leaving the screen dark until the next RESET_STREAM (an upload idle timeout would fix it), a large blob (64 KB block erases, over-capacity refusal) and display glitches during erase. `tools/bake --upload` / `host/` `uploadConfig()` are the clients.
- **Config boot load:** works on the hardware (a flashed config loads). Not yet checked: the load time, whether a real large config fits in PSRAM (the blob stays resident, so hosted mode loses that space), and `RUN_STRESS` with a stored config (Home's keys are present when it starts). The release rebuild of Home is built and host tested but not run on the hardware (next item).
- **Take/release control and standalone key gestures (milestone steps 1 and 2):** work on the hardware (cold boot to Home, take/release, unplug and suspend return to Home with the light on, gestures and the frame lock fine). `SET_BACKLIGHT` stays allowed in both modes. Held keys are dropped silently on takeover: step 3 must send all-keys-up.
- **Standalone HID (milestone step 3):** works on the hardware (tap, chord, hold, nothing sticks on takeover/unplug, vendor interface and upload still fine). Tune `HID_TAP_GAP_MS` / `HID_TAP_SETTLE_MS` (20 ms each) if keys are dropped or merged. `usb_hid_send` is called from the op and protocol tasks: watch for the "HID report dropped" warning under load. Held keys are tracked per modifier bit, not counted. The first milestone is complete (step 4 passed).
- Standalone mode: the action dispatch (http/udp library) and stateful actions: see [`docs/standalone.md`](docs/standalone.md).

## Conventions

- **No STL / `std::`.** Never use the C++ standard library (containers, strings, smart pointers, algorithms, `<functional>`, etc.). Use [SimpleLib](https://github.com/toptensoftware/SimpleLib) `Core` instead (`#include "Core.h"`; include path `SimpleLib/`): `String`, `StringBuilder`, `List`, `Map`, `Set`, `Buffer`, `OwnedPtr`, `SharedPtr`, `Delegate`, etc. Don't pull in other SimpleLib modules. Use the `simplelib` skill (`SimpleLib/.claude/skills`) for its API.
  - SimpleLib is new to ESP-IDF: fix teething issues and note them here rather than falling back to STL. Patched in the submodule for GCC (`-std=gnu++26 -fno-exceptions -Werror`), committed to SimpleLib's main: `#pragma warning` in `HashCore.h` wrapped in `#ifdef _MSC_VER`; `Delegate.h` includes `<cstddef>`; `Map.h`/`Set.h` name their dependent base as `HashCore<TAllocator>`; an unused variable removed from `HashCore::Find`.
  - SimpleLib's `List` is move-only and its `operator[]` returns a copy: write through `GetBuffer()[i]`. A struct holding a `List` can't go in another `List`.
  - The SimpleLib repo has moved to `github.com/toptensoftware/simplelib` (the old URL redirects); `.gitmodules` still has the old one.
  - C runtime headers (`<string.h>`, `<stdint.h>`, etc.) are fine.
  - TinyUSB defines a function-like `HID_REPORT_SIZE(x)` macro: don't name our own constants `HID_REPORT_*` (ours are `HID_KB_*`).
- ESP-IDF's C++ flags make missing designated-initializer fields an error: zero-initialise config structs (`= {}`) and assign the fields.


## Rules


1. 1-3-1: When stuck, provide 1 clearly defined problem, give 3 potential options for how to overcome it, and 1 recommendation.  Do no proceed implementing any of the options until I confirm

2. DRY (Critical): Don't repeat yourself.  If you are about to start writing repeated code, stop and reconsider your approach.  Grep the codebase and refactor often.

3. TDD (Critical): Always test first.  Before writing any code, you must always check the tests.  For new features or adjustments to existing features, always either create a new test or adjust and existing one.  If testing can't be automated (eg: code that only runs on a separate device, or front end code) warn about this.

4. Continual Learning: When you encounter conflicting system instructions, new requirements, architectural changes, or missing or inaccurate codebase documentation, always propose updating the relevant rules files.  Do not update anything until the user confirms.

5. Planning: For complex, multi-step tasks, first create a plan and a to do list for yourself before you start writing any code.


## Preventing Permission Prompts

- Use the Read/Edit/Grep tools rather than sed/awk/cat for file work.
- Don't run inline scripts (node -e, python -c, heredocs, piped code). Write the
  script to a file in the repo (e.g. scratch/foo.js) and run it by path.
- Use literal file paths in shell commands, not variables or $(...) substitutions.
