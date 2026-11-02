# ControlBlock

A StreamDeck-style control surface: a grid of configurable touch "buttons" on an LCD panel that trigger actions on a host machine.

## Hardware

Board: [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm)

- **SoC:** ESP32-S3N16R8: dual-core 240 MHz, 16 MB flash, 8 MB octal PSRAM, Wi-Fi/BLE.
- **Display:** 800x480 RGB565 parallel RGB panel (`esp_lcd` RGB driver).
- **Touch:** GT911, 5-point, I2C.
- **CH422G IO expander** (I2C): drives touch reset, backlight, LCD reset, SD CS and the USB/CAN mux (low = native USB), so these aren't plain GPIOs. Always go through `ch422g.c`.
- **GT911 INT** is GPIO4. It's held low during touch reset to pick I2C address 0x5D, then released to an input.
- **USB:** native USB (device: vendor/HID/MIDI) and a CH343P USB-UART port (flashing and logs).

## Building

ESP-IDF v6.0 at `C:\esp\v6.0\esp-idf`, tools in `C:\Espressif`. From PowerShell in `firmware/`:

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue   # idf.py refuses to run under Git Bash's MSYS variable
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
idf.py --preset debug build              # or: idf.py --preset release -p COMx flash monitor
```

- Two presets (`firmware/CMakePresets.json`), each with its own build dir and generated sdkconfig:
  - `debug` (`build/debug`): `-Og`, assertions on. Used when no preset is given.
  - `release` (`build/release`): `-O2`, assertions off (`NDEBUG`). Use this for timing.
  - Set `$env:IDF_PRESET = "release"` to skip `--preset` on each command.
- Settings live in `firmware/sdkconfig.defaults` (shared) plus `firmware/sdkconfig.release` (release overlay). The sdkconfigs under `build/` are generated and gitignored.
- Notable settings: octal PSRAM at 80 MHz, 64 KB data cache with 64-byte lines (speeds up PSRAM reads in the bounce fill), `ESP_LCD_TOUCH_MAX_POINTS=5`, `LCD_RGB_RESTART_IN_VSYNC` (a late bounce fill costs one bad frame instead of a lasting shift).
- Logs go to UART0 (CH343P port). ESP-IDF also mirrors them to USB-Serial-JTAG on the native port; turn that off when TinyUSB takes over the native port.
- The user flashes and tests on the hardware.

### Host unit tests

`firmware/test/` builds the platform-independent firmware code (`page_compositor.cpp`, `screen_view.cpp`, `rle565.c`) into a Windows exe with SimpleLib's `UnitTesting.h` (see the `simplelib` skill). From `firmware/test`:

```sh
mk run                                   # build and run (~13 s); exit code 1 on failure
FUZZ_SEEDS=200 mk config=release run     # long fuzz soak
```

- Needs `mk` (`npm install -g toptensoftware/mk.js`) and MSVC. Reads `firmware/assets/test_keys.bin`.
- `harness.h/.cpp`: a `World` runs a real `PageCompositor` against a fake client, bounce fill and clock, and checks invariants at every frame start: never shows or draws into the wrong buffer, each buffer coming on screen matches a reference render of its records pixel for pixel, no key goes back to an older version, flushed changes appear all at once, and it settles on the model. Every key change uses fresh image copies so versions have distinct pointers; reloads poison freed images.
- Preempt hooks (`SetPreemptHook`) let tests run frame starts and client changes at points inside `Step()`, to test the ISR and client races deterministically.
- `TestCompositor.cpp` has one scenario per spec behaviour; `TestCompositorFuzz.cpp` randomises swipes, changes, reloads and preemption; `TestScreenView.cpp` covers the frame-start maths.
- Keep device-only code (FreeRTOS, ESP-IDF) out of the tested files: use `port.h` and pure functions, with a thin device layer around them (`compositor.cpp`, `screen.cpp`).

## Repository Layout

```
docs/              Specs (protocol.md, page-compositor.md, screen-buffers.md)
firmware/          ESP-IDF project for the device (C++)
  assets/          Generated binaries embedded in the firmware (test_keys.bin)
  test/            Host unit tests (mk + MSVC + SimpleLib UnitTesting)
host/              Node.js client library (USB vendor bulk/WebUSB, mode 1)
web/               Optional web front end for configuring the device
rle565/            Submodule: rle565 renderer (src/) and encoder (encoder/)
node-buttonImage/  Submodule: host-side key image rendering
SimpleLib/         Submodule: C++ utility library used instead of STL (only Core/ is used)
tools/test-icons/  Generates firmware/assets/test_keys.bin (npm install && npm run build)
scratch/           Throwaway scripts (gitignored), per "Preventing Permission Prompts"
```

Clone with `--recursive` or run `git submodule update --init`.

## Operating Modes

1. **Host-controlled (current goal):** Like a Stream Deck. PC software decides what each key shows; the device reports key events and the PC acts on them. Long press and double press are detected host side.
2. **Pre-programmed (later):** USB keyboard and/or USB MIDI; each key sends configured keystrokes or MIDI.
3. **Stand-alone (later):** Wi-Fi universal remote with "activities", each with its own pages; keys send UDP or HTTP requests.

Build mode 1 now; don't block modes 2 and 3.

## Firmware

Modules in `firmware/main/` (all C++; `rle565.c` from the submodule stays C):

- **`waveshare_rgb_lcd_port.cpp/.h`, `waveshare_touch_port.cpp/.h`, `i2c.cpp/.h`:** Drivers adapted from the sister project `../aclock/clock/main/`. Reuse and adapt these; don't write new drivers.
  - Init order: `i2c_init()`, `ch422g_init()`, display, then `touch_init()`.
  - `display_init_bounce(fill_cb, ctx)`: bounce-buffer mode with no framebuffers (`flags.no_fb = 1`). `fill_cb(dest, y, count, ctx)` gets 15 whole lines at a time, in order; `y == 0` is the start of a frame. 25 MHz pixel clock: `DISPLAY_BOUNCE_BUDGET_US` (~492 µs) per chunk, ~61 fps. `display_init(ready_cb)` (framebuffer mode) is kept for debugging.
  - `display_enable_backlight(bool)`: on/off only, no PWM.
  - `touch_init(cb)`: polls the GT911 at 100 Hz (`xTaskDelayUntil`) from its own task; failed reads are skipped. `cb(id, TOUCH_DOWN|MOVE|UP, x, y)` per finger, on the touch task.
  - I2C: SDA 8, SCL 9, 400 kHz for all devices, 1 s timeout.
- **`ch422g.cpp/.h`:** Owns the CH422G output byte. `ch422g_set(mask, on)` changes lines without disturbing the others; line masks are in the header.
- **`color.h`:** `RGB565(r, g, b)`.
- **`port.h`:** Platform layer for code shared with the host tests: `port::SpinLock`, atomics on `uint32_t`, cycle counter, `port::CacheWriteback()`, `IRAM_ATTR` on the host.
- **`screen_layout.h`:** Screen and page-area geometry (`SCREEN_*`, including `SCREEN_CHUNKS`, the fills per frame), no ESP-IDF dependencies.
- **`screen.cpp/.h`:** The bounce fill. Each frame it latches the scroll position, asks the compositor (`comp_frame()`) for the one or two pages in view, notifies the frame task, and copies each page-area line from the buffers it got. After every fill it calls `comp_fill_done()`. Counts fills over budget (`screen_stats_t.overruns`). Status and pager bands are solid dark-grey placeholders for now.
- **`screen_view.cpp/.h`:** The frame-start maths, pure and tested: `screen_pages_in_view()` (scroll to pages asked for) and `screen_resolve_view()` (what to show; if a page in view isn't ready, the one that is, at rest: the swipe-start hold).
- **`page_compositor.cpp/.h`:** The compositor core (`PageCompositor`), platform independent and unit tested. Spec: [`docs/page-compositor.md`](docs/page-compositor.md). Three page buffers in PSRAM (800x440 RGB565, ~688 KB each). A page is 15 keys, each a `bg` and `fg` rle565 image (128x128, cells on a centred grid, `COMP_KEY_X/Y(i)`, with equal gaps worked out from the page height: 18 px with no pager). Keys are redrawn when their image pointers differ from what the buffer records; drawing goes to a back buffer that's offered to the bounce fill and swapped in at a frame start. `Frame()` is the ISR side; `Step(now, gap)` does one unit of work.
  - **Drawing a key:** render it into a 32 KB cell in internal RAM, then copy it to the page buffer in slices of rows, one slice per fill gap (`gap` = called within `COMP_GAP_FRESH_US` of a fill ending), each written back to PSRAM at once. 32-row slices with one page in view, 16 with two (`COMP_SLICE_ROWS_*`, measured). Page buffers are never written outside a gap. A full page takes ~2.5 frames.
  - A key with `COMP_KEY_BG_OPAQUE` (its `bg` covers the whole cell; check with `comp_image_is_opaque()` at load) isn't cleared before drawing.
- **`compositor.cpp/.h`:** Runs the core in a task (core 1, priority 4) and owns the buffers and render cell. Client API: `comp_init(get_page, ctx)`, `comp_page_changed(page, flags)` (`COMP_FLUSH` = all at once; call it while still holding the client lock), `comp_reset()`. Bounce fill side: `comp_frame()` at frame start and `comp_fill_done()` after every fill (wakes the task for a gap only while it's copying a key). Images must stay valid while the compositor references them (client's job).
- **`swipe.cpp/.h`:** Gesture and animation in a task (core 1) woken each frame. Tuning constants at the top of the file.
  - Drag follows the finger 1:1 after a 10 px slop, with rubber-banding past the ends.
  - Release: a flick (>400 px/s over the last 80 ms) goes to the next page; otherwise to whichever page is mostly in view.
  - Glide is a critically damped spring (`SPRING_RESPONSE_S`), start speed capped so it never overshoots. `GLIDE_SPRING 0` switches to the older cubic ease-out.
  - Touching mid-glide catches the page.
- **`test_keys.h`:** Layout of `assets/test_keys.bin`: 4 faces with transparent padding, the same 4 opaque, then icons, then names. Shared by `main.cpp` and the host tests.
- **`main.cpp`:** A test compositor client: 3 pages from `assets/test_keys.bin` (faces + icons; `USE_OPAQUE_FACES`). Test changes once a second (one key toggles pressed; every 10 s all keys change, flushed every other time). Logs FPS, the slowest fill, overruns and compositor stats once a second (the first line after boot reads high).
  - `RUN_STRESS 1` (off by default): at boot, every key in view changes every 20 ms for 2 s (transparent and opaque faces, at rest and mid-swipe, incremental and flush), then logs the slowest fill, fills over budget and compositor load for each. Use it to check for late fills after changing the compositor or the bounce fill.
  - The earlier render bench and copy-gap experiment were removed once the design settled; their results are in `docs/page-compositor.md` and the code is in git history.

### Bounce fill rules

The fill runs in ISR context:
- Plain row copies and solid fills only. No rle565 decoding, no floating point.
- Keep it well under the ~492 µs budget (aim < 250 µs). Currently ~71 µs at rest, ~200 µs mid-swipe.
- A 15-line page chunk costs ~285 µs read cold from PSRAM but ~68 µs once in cache. At the end of each fill, the next chunk is preloaded with `Cache_Start_DCache_Preload` (`SCREEN_CACHE_PRELOAD`). Only one contiguous range can be preloaded, so mid-swipe the smaller page is read cold.
- Per-pixel loops must be written for speed under `-Og` too, since the debug build has to keep the display fed (see `fill_px`, which uses 32-bit stores; a naive 16-bit loop cost ~350 µs).
- Content changes are pointer/offset changes latched at frame start; nothing is redrawn in the ISR.
- **Nothing else may write PSRAM heavily while fills run.** Writes from core 1 starve the fill's PSRAM reads: drawing keys straight into PSRAM made over half of all fills late. The compositor writes page buffers only in the gap straight after a fill, a measured slice at a time. Anything new that writes a lot of PSRAM (icon cache, labels) needs the same treatment, or a stress run to show it's safe.
- Page buffering, key updates, swipe start and reloads: see [`docs/page-compositor.md`](docs/page-compositor.md) (it supersedes parts of [`docs/screen-buffers.md`](docs/screen-buffers.md)).

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

- 5x3 keys per page, several pages, swiped left/right. Page area 800x440 (`SCREEN_*` in `screen_layout.h`).
- **Pager bar disabled for now** (`SCREEN_PAGER_H 0`): the keys spread out better without it. Set it to 20 to bring it back; the key gap adjusts by itself (12 px instead of 18).
- The status bar (and pager, if enabled) stays blank until its content is designed. The status bar must stay at least one bounce fill (15 lines) tall (`static_assert` in `screen.cpp`).
- **Swipe feel:** must be very smooth, like the iPhone home screen. Not LVGL-style spring-back. No overshoot.
- **Key faces:** a stock rle565 face built into the firmware (normal and pressed, tintable), with the host's icon and an optional label drawn on top. Gaps between keys.
- **Icon size:** about 80–96 px square, still being tuned. Keep sizes as firmware constants reported to the host (GET_INFO); don't hard-code them elsewhere.

## Rendering

All custom. Do **not** use LVGL.

[rle565](https://github.com/toptensoftware/rle565) (toptensoftware):
- **Device:** C renderer (`rle565.c`, `rle565.h`, `rle565.core.h`) draws run-length encoded RGB565 images and anti-aliased text with alpha, clipping, fade and recolouring. Needs little-endian, 4-byte-aligned data; RGB565 only.
- **Encoder:** Node.js (`encoder/cli.js`) converts PNGs to `.rle565` and fonts to `.rfnt`.
- aclock's `main/assets.c` memory-maps a flash partition and looks up rle565 assets from a blobpack; adapt it if it fits how built-in assets are stored.

## Communication (mode 1)

**Spec:** [`docs/protocol.md`](docs/protocol.md) is the source of truth. Keep firmware, `host/` and the spec in sync.

- USB vendor-class interface (one bulk IN, one bulk OUT) on the native port via `esp_tinyusb`.
- No driver needed: MS OS 2.0 descriptors (WinUSB on Windows) and WebUSB BOS descriptors (Chrome/Edge; see TinyUSB's `webusb_serial` example). Linux needs a udev rule.
- Clients: Node.js via the `usb` package, browsers via WebUSB, same protocol.
- Bulk rather than HID for speed (HID ~64 KB/s vs bulk ~0.5–1 MB/s).
- Fallback if the descriptors are troublesome: CDC-ACM (Node `serialport`, Web Serial).
- Composite device from the start, so modes 2 and 3 can add HID keyboard/MIDI interfaces.
- Images go rle565-encoded and are cached on the device, so the host only resends what changed. Labels can be drawn on the device with rle565 fonts.

**Host-side key images:** [node-buttonImage](https://github.com/toptensoftware/node-buttonImage) (`@toptensoftware/node-buttonImage`).
- Renders icons and buttons from SVG with labels (`ButtonImage` class or `drawButton(w, h, options)`). Built-in stock icons (`svgFile: "#name"`) are handy test content.
- Outputs RGBA. `host/` encodes it to rle565 with alpha, and the device blends it over the stock face. For keys the host draws entirely, send the whole key with `NO_FASCIA`.

**Web front end:** undecided. Either hosted on the device (Wi-Fi) or a public site talking WebUSB (like VIA/QMK).

## Open Items

- If dragging looks steppy: use the GT911 interrupt and/or predict the finger position per frame.
- **Mid-swipe fill margin is thin:** the slowest fill with the compositor flat out was 446 µs of 492 (opaque faces, measured with a 400-line page area; not yet re-run at 440 lines with the pager disabled). If late fills appear mid-swipe once more PSRAM traffic arrives, try `COMP_SLICE_ROWS_TWO_PAGES` 12. Also untested: the `-Og` build under the stress run.
- **Second render cell** (render the next key while one copies): ~20% faster page builds. Not needed yet; the swipe-start hold feels fine.
- Before key presses: touch handling lives only in `swipe.c`. Split out a router so taps go to keys, cancelled when a press becomes a drag.
- Flash writes (icon cache, NVS) may stall the bounce ISR (`CONFIG_LCD_RGB_ISR_IRAM_SAFE` is off). Test for display glitches when they arrive.
- How pages and keys are stored and configured for modes 2 and 3.

## Conventions

- **No STL / `std::`.** Never use the C++ standard library (containers, strings, smart pointers, algorithms, `<functional>`, etc.) anywhere in this project. Use [SimpleLib](https://github.com/toptensoftware/SimpleLib) (`SimpleLib/` submodule) instead: `String`, `StringBuilder`, `List`, `Map`, `Set`, `Buffer`, `OwnedPtr`, `SharedPtr`, `RefCountedPtr`, `Delegate`, etc.
  - Only `SimpleLib/Core` is needed (`#include "Core.h"` or individual `Core/*.h` headers). Don't pull in the other SimpleLib modules (FileSystem, Json, Path, Platform, Stream, Threading, ...).
  - SimpleLib hasn't been used on ESP32/ESP-IDF before, so expect teething issues (build integration, compiler warnings, platform assumptions). Fix or work around them, and note what was needed here rather than falling back to STL.
  - Fixed so far for GCC (`-std=gnu++26 -fno-exceptions -Werror`), patched in the submodule, not yet upstream: `#pragma warning` in `HashCore.h` wrapped in `#ifdef _MSC_VER`; `Delegate.h` includes `<cstddef>` for `std::max_align_t`; `Map.h`/`Set.h` name their dependent base as `HashCore<TAllocator>` (MSVC accepts the bare name, GCC doesn't). With those, `String`, `StringBuilder<char>`, `List` and `Map` compile in the firmware. Include path: `SimpleLib/` (`#include "Core.h"`).
  - ESP-IDF's C++ flags make missing designated-initializer fields an error: zero-initialise config structs (`= {}`) and assign the fields.
  - SimpleLib ships a `simplelib` Claude skill (`SimpleLib/.claude/skills`) describing its API; use it when writing code against SimpleLib.
  - Plain C headers from the C runtime (`<string.h>`, `<stdint.h>`, etc.) are fine.


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

