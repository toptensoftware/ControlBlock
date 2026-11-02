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
- The user flashes and tests on the hardware.

### Host unit tests

`firmware/test/` builds the platform-independent firmware code into a Windows exe with SimpleLib's `UnitTesting.h` (see the `simplelib` skill). From `firmware/test`:

```sh
mk run                                   # build and run (~13 s); exit code 1 on failure
FUZZ_SEEDS=200 mk config=release run     # long fuzz soak
```

- Needs `mk` (`npm install -g toptensoftware/mk.js`) and MSVC. Reads `firmware/assets/*.bin` (`LoadAsset()`). Sources are listed in `test.mk.js`.
- `harness.h/.cpp`: a `World` runs a real `PageCompositor` against a fake client, bounce fill and clock, checking its invariants at every frame start. Preempt hooks run frame starts and client changes inside `Step()` to test races deterministically. `RetireUnused()` exercises sweeps: freed images are poisoned and their addresses reused straight away.
- `store_env.h/.cpp`: the image store's test platform (an allocator that poisons and reuses freed blocks, a fake sweep), shared by the store, key layout and protocol tests. Protocol tests feed raw bytes in and decode the responses.
- Keep device-only code (FreeRTOS, ESP-IDF, TinyUSB) out of the tested files: use `port.h`, pure functions and injected callbacks (`ImageStoreHost`, `ProtocolHost`), with a thin device layer around them (`compositor.cpp`, `screen.cpp`, `status.cpp`, `model.cpp`, `host_link.cpp`, `usb.cpp`).

### Host library tests

From `host/`: `npm install` once, then `npm test` (Node's test runner, no device: `test/FakeDevice.js` stands in for the WebUSB device). On a device: `npm run demo` and `node demo/bench.js`.

## Repository Layout

```
docs/              Specs: protocol.md, image-assets.md, page-compositor.md, status-bar.md, interactions.md (screen-buffers.md is partly superseded)
firmware/          ESP-IDF project for the device (C++)
  assets/          Generated binaries embedded in the firmware (ui.bin; test_keys.bin only for the stress run)
  test/            Host unit tests
host/              Node.js client library (WebUSB API over the `usb` package or a browser; mode 1), demo and bench scripts
web/               Optional web front end for configuring the device
rle565/            Submodule: rle565 renderer (src/) and encoder (encoder/)
node-buttonImage/  Submodule: host-side key image rendering
SimpleLib/         Submodule: C++ utility library used instead of STL (only Core/ is used)
tools/test-icons/  Generates firmware/assets/*.bin (npm install && npm run build)
scratch/           Throwaway scripts (gitignored)
```

Clone with `--recursive` or run `git submodule update --init`.

## Operating Modes

1. **Host-controlled (current goal):** Like a Stream Deck. PC software decides what each key shows; the device reports key events and the PC acts on them. Long press and double press are detected host side.
2. **Pre-programmed (later):** USB keyboard and/or USB MIDI; each key sends configured keystrokes or MIDI.
3. **Stand-alone (later):** Wi-Fi universal remote with "activities", each with its own pages; keys send UDP or HTTP requests.

Build mode 1 now; don't block modes 2 and 3.

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
- **`status_bar.cpp/.h`, `status.cpp/.h`, `ui_assets.h`:** the status bar (text, pager pills). Spec: [`docs/status-bar.md`](docs/status-bar.md).
- **`swipe.cpp/.h`:** drag, flick and spring glide between pages, in a task woken each frame; tuning constants at the top. Sets the status bar's selected pill. Also takes the host's page count and go-to-page (through its queue) and reports page changes once settled. Feeds touches through the touch router and shows each key's pressed look from touch-down (reverted if it becomes a swipe) until RELEASE/CANCEL.
- **`touch_router.cpp/.h`:** swipe or key press: the swipe window, edge-ambiguous zone, and PRESS/RELEASE/CANCEL. Pure and tested; tuning constants in the header. Spec: [`docs/interactions.md`](docs/interactions.md).
- **`image_store.cpp/.h`:** the host's rle565 images under 32-bit IDs in PSRAM: streamed uploads, replace in place, deferred free, reset. Platform independent and tested. Spec: [`docs/image-assets.md`](docs/image-assets.md).
- **`key_layout.cpp/.h`:** pages of keys, four image slots each (normal and pressed bg/fg), ref counted; all-or-nothing `SetKeys`. The compositor's client. Tested.
- **`protocol.cpp/.h`:** the mode 1 protocol: stream parser, every request (including SET_STATUS_TEXT: the host sets the status bar's left and right text), event queue. Platform independent and tested. Spec: [`docs/protocol.md`](docs/protocol.md).
- **`model.cpp/.h`:** the device's store + layout under one lock; the compositor's `get_page`/`is_retired`.
- **`usb.cpp/.h`:** TinyUSB device: descriptors (composite-ready; the interface map and how to add HID/MIDI are at the top), WinUSB/WebUSB, `RESET_STREAM`, vendor read/write.
- **`host_link.cpp/.h`:** the protocol task (core 0): USB bytes in, responses and events out; applies status text to `status_set_left/right`.
- **`main.cpp`:** init and the stats log. The device boots empty (one empty page, no keys, empty status text); the host supplies everything. Logs free heap at boot, then FPS, fill, compositor and image stats once a second.
  - `RUN_STRESS` (off by default; build with `idf.py --preset debug -DCB_RUN_STRESS=ON build`, which embeds `assets/test_keys.bin` as its test images, layout in `test_keys.h`; `-DCB_RUN_STRESS=OFF` turns it off again; the device is left empty afterwards): at boot, drives the compositor (and status bar) flat out at rest and mid-swipe, plus image replaces at ~USB speed, and logs late fills for each case. Run it after changing the compositor, the bounce fill or anything that touches PSRAM.

### Bounce fill rules

The fill runs in ISR context:
- Plain row copies and solid fills only. No rle565 decoding, no floating point.
- Keep it well under the ~492 µs budget (aim < 250 µs). Currently ~115 µs at rest, ~200 µs mid-swipe, up to ~360 µs mid-swipe with the compositor flat out.
- A 15-line chunk costs ~285 µs read cold from PSRAM but ~68 µs in cache. The next chunk is preloaded at the end of each fill (`Cache_Start_DCache_Preload`); only one range can be, so mid-swipe the smaller page is read cold. Anything the fill reads every frame (like the status bar) belongs in internal RAM.
- Per-pixel loops must be fast under `-Og` too (see `fill_px`: 32-bit stores; a naive 16-bit loop cost ~350 µs).
- Content changes are pointer/offset changes latched at frame start; nothing is redrawn in the ISR.
- **Nothing else may write PSRAM heavily while fills run.** Writes from core 1 starve the fill's PSRAM reads. The compositor writes page buffers only in the gap after a fill, a measured slice at a time. Anything new that writes a lot of PSRAM (icon cache, labels) needs the same treatment, or a stress run to show it's safe.

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
- Development IDs `303a:4020`. Windows caches the MS OS 2.0 descriptor per VID/PID/bcdDevice: bump `USB_BCD_DEVICE` in `usb.cpp` when it changes.
- Composite device from the start, so modes 2 and 3 can add HID keyboard/MIDI interfaces.
- Vendor bulk carries every mode-1 message (not split with HID: one interface keeps uploads and key definitions in order).
- Images go rle565-encoded under host-chosen 32-bit IDs (0 = none; a CRC makes a good one) and stay on the device, so the host only resends what changed. Uploading to an existing ID replaces it everywhere; freeing is deferred while keys still use the image. See [`docs/image-assets.md`](docs/image-assets.md).
- **Host-side key images:** [node-buttonImage](https://github.com/toptensoftware/node-buttonImage) renders faces and icons from SVG to RGBA (stock icons `svgFile: "#name"` make handy test content); `host/` encodes them to rle565 with alpha, 128x128 with transparent padding.
- **Web front end:** undecided: hosted on the device (Wi-Fi) or a public site using WebUSB.

## Open Items

- If dragging looks steppy: use the GT911 interrupt and/or predict the finger position per frame.
- Untested: the `-Og` build under the stress run.
- **Internal RAM:** ~95 KB free after init (largest block 53 KB), down from ~130 KB before USB (TinyUSB FIFOs, the protocol's 8 KB buffer, two task stacks). Wi-Fi may need more: see [`docs/status-bar.md`](docs/status-bar.md) for shrinking the status buffers, move the protocol buffer and stacks to PSRAM, or move Wi-Fi/lwIP buffers to PSRAM (with a stress run).
- **Second render cell** (render the next key while one copies): ~20% faster page builds. Not needed yet.
- **Key presses:** the touch router is built and unit tested, and wired to the model and `Protocol::PostKey`, but **not yet run on the hardware**. Tune `TOUCH_SWIPE_WINDOW_MS` and `TOUCH_EDGE_AMBIGUOUS_PX`, and judge the pressed look flashing on swipes (it shows at touch-down and reverts when a swipe starts). The swipe task takes the model lock on key actions: check it doesn't delay frames. Per-key "no swipe window" mode: add only if needed.
- **Not yet run on the hardware:** `RUN_STRESS`'s image-replace cases (do uploads make fills late?) and `demo/bench.js`.
- Host library: long and double press helpers (protocol.md says the host detects them).
- Flash writes (icon cache, NVS) may stall the bounce ISR (`CONFIG_LCD_RGB_ISR_IRAM_SAFE` is off). Test for display glitches when they arrive.
- How pages and keys are stored and configured for modes 2 and 3.

## Conventions

- **No STL / `std::`.** Never use the C++ standard library (containers, strings, smart pointers, algorithms, `<functional>`, etc.). Use [SimpleLib](https://github.com/toptensoftware/SimpleLib) `Core` instead (`#include "Core.h"`; include path `SimpleLib/`): `String`, `StringBuilder`, `List`, `Map`, `Set`, `Buffer`, `OwnedPtr`, `SharedPtr`, `Delegate`, etc. Don't pull in other SimpleLib modules. Use the `simplelib` skill (`SimpleLib/.claude/skills`) for its API.
  - SimpleLib is new to ESP-IDF: fix teething issues and note them here rather than falling back to STL. Patched in the submodule for GCC (`-std=gnu++26 -fno-exceptions -Werror`), committed to SimpleLib's main: `#pragma warning` in `HashCore.h` wrapped in `#ifdef _MSC_VER`; `Delegate.h` includes `<cstddef>`; `Map.h`/`Set.h` name their dependent base as `HashCore<TAllocator>`; an unused variable removed from `HashCore::Find`.
  - SimpleLib's `List` is move-only and its `operator[]` returns a copy: write through `GetBuffer()[i]`. A struct holding a `List` can't go in another `List`.
  - The SimpleLib repo has moved to `github.com/toptensoftware/simplelib` (the old URL redirects); `.gitmodules` still has the old one.
  - C runtime headers (`<string.h>`, `<stdint.h>`, etc.) are fine.
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
