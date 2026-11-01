# ControlBlock

A StreamDeck-style control surface: a grid of configurable touch "buttons" on an LCD panel that trigger actions on a host machine.

## Hardware

Board: [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm)

- **SoC:** ESP32-S3N16R8. Dual-core LX7 at 240 MHz, 16 MB flash, 8 MB octal PSRAM, Wi-Fi and BLE 5
- **Display:** 4.3" IPS, 800x480, RGB565, parallel RGB interface (`esp_lcd` RGB panel)
- **Touch:** 5-point capacitive over I2C with an interrupt line (GT911)
- **IO expander:** CH422G over I2C. It drives the LCD/touch reset, backlight enable and SD card CS lines, so these are not plain GPIOs
- **USB:** Two Type-C ports:
  - Native USB, which can run as a device for HID, CDC and so on
  - A separate CH343P USB-to-UART port for flashing and logs
- **Other:** microSD slot, CAN (TJA1051), RS485 (SP3485), Li-ion charger and battery header

## Toolchain

- **Firmware:** ESP-IDF (C)
- **Host library:** Node.js (probably)
- **Web front end:** Optional, see below

## Rendering

All rendering is custom. Do **not** use LVGL.

Graphics use [rle565](https://github.com/toptensoftware/rle565), an earlier toptensoftware project:
- **On device:** The C renderer (`rle565.c`, `rle565.h`, `rle565.core.h`) draws run-length encoded RGB565 images and anti-aliased text straight from flash into the framebuffer, with alpha, clipping, fade and recoloring.
- **Assets:** The Node.js encoder (`encoder/cli.js`) converts PNGs to `.rle565` and fonts to `.rfnt` ahead of time.
- **Limits:** The renderer needs little-endian hardware and 4-byte-aligned data, and supports RGB565 only.

## Project Config (from aclock)

The aclock project (`../aclock/clock/`) runs on this same board and is known to work. Copy its project config rather than working settings out from scratch:
- **`sdkconfig`:** Key settings are:
  - 16 MB flash
  - Octal PSRAM at 80 MHz, with `SPIRAM_FETCH_INSTRUCTIONS`, `SPIRAM_RODATA` and malloc support
  - 240 MHz CPU
  - 32 KB data cache with 32-byte lines
  - `ESP_LCD_TOUCH_MAX_POINTS=5`

  Prefer distilling these into an `sdkconfig.defaults` rather than copying the full `sdkconfig`. Leave out aclock's custom partition table setting.
- **Not needed here:** aclock's `partitions.csv` and the asset-flashing step in its `CMakeLists.txt`.
- **Maybe reuse `main/assets.c`:** It memory-maps a flash partition (`esp_partition_mmap`) and looks up rle565 images and fonts by index from a blobpack, so assets reach the renderer straight from flash with no copy. Adapt it only if it fits how this project stores assets.

## Display and Touch Drivers

Reuse the existing Waveshare drivers from the aclock project. Copy or adapt them into the firmware; don't write new drivers. They need ESP-IDF >= 6.0 and the component `espressif/esp_lcd_touch_gt911: "^1"`.

Files in `../aclock/clock/main/`:
- `waveshare_rgb_lcd_port.c/.h`: Display driver
- `waveshare_touch_port.c/.h`: Touch driver
- `i2c.c/.h`: Shared I2C bus (SDA=GPIO8, SCL=GPIO9, 400 kHz)

**Display** (`display_*`):
- `display_init(ready_cb)` sets up the RGB panel at 25 MHz pixel clock with a 15-line bounce buffer.
- The driver allocates two full 800x480 RGB565 framebuffers in PSRAM. Get them with `display_get_frame_buffers()`.
- Double-buffered flow:
  1. Draw into the back buffer.
  2. Call `display_set_framebuffer(fb)` to swap at the next vsync. The swap is tear-free and does no copy.
  3. `ready_cb(freed_fb)` fires from the vsync ISR when the old buffer is safe to draw into again.
- Only one swap can be pending at a time (asserted).
- `display_enable_backlight(bool)` drives the backlight through the CH422G. It is on/off only, no PWM.

**Touch** (`touch_*`):
- `touch_init(cb)` must be called after `i2c_init()` and `display_init()`.
- The GT911 is polled at 50 Hz from its own task. There is no interrupt; reset goes through the CH422G and GPIO4.
- The callback `cb(id, TOUCH_DOWN|TOUCH_MOVE|TOUCH_UP, x, y)` reports each finger separately and runs on the touch task.

**CH422G:** The I2C address selects the register: 0x24 is the system register, 0x38 the output register. The display and touch drivers each write to it on their own.

## Repository Layout

Proposed:

```
firmware/   ESP-IDF project for the device
host/       Node.js client library (USB vendor bulk/WebUSB, mode 1)
web/        Optional web front end for configuring the device
```

## Operating Modes

1. **Host-controlled (initial goal):** Works like a Stream Deck.
   - PC software decides which buttons are shown and what they look like.
   - The device reports button press and release events back to the PC, and the PC acts on them.
   - Communication is a custom protocol over a USB vendor bulk interface (WebUSB), with a Node.js client library in `host/`.
2. **Pre-programmed (later):** Runs on its own without host software.
   - It appears to the computer as a USB keyboard and/or a USB MIDI device.
   - Each button sends its configured keystrokes or MIDI messages.
3. **Stand-alone (later):** Works like a universal remote over Wi-Fi.
   - It switches between "activities", each with its own pages of buttons.
   - Buttons send UDP or HTTP requests.

Build mode 1 first. Keep modes 2 and 3 in mind so the design doesn't block them, but don't build them yet.

## Communication

**Mode 1: USB vendor bulk interface with WebUSB**
- Runs on the native USB port using ESP-IDF's TinyUSB component (`esp_tinyusb`). The CH343P UART port stays for flashing and logs.
- Uses a vendor-class interface with one bulk IN and one bulk OUT endpoint.
- **No driver on any platform:**
  - **Windows:** The device reports MS OS 2.0 descriptors so WinUSB binds automatically.
  - **Chrome/Edge:** It reports WebUSB BOS descriptors. TinyUSB's `webusb_serial` example shows how.
  - **Linux:** Needs a udev rule for user access.
- **Clients:**
  - Node.js uses the `usb` package (libusb).
  - Browsers use WebUSB.
  - Both talk the same protocol.
- **Why not HID:**
  - Full-speed HID is capped at one 64-byte interrupt packet per ms, about 64 KB/s.
  - Bulk on the ESP32-S3 (still full-speed only) typically reaches ~500 KB/s–1 MB/s.
  - 45 icons at 128x128 (~300 KB rle565-encoded) take about 5–9 s over HID versus about 0.3–0.6 s over bulk.
- **Fallback:** If the WebUSB/WinUSB descriptors prove troublesome, use CDC-ACM serial (Node `serialport`, browser Web Serial). It has the same bulk speed.
- **Images:**
  - Send images rle565-encoded. The host side is Node.js, so it can use the rle565 encoder directly.
  - Cache images on the device so the host only resends buttons that changed.
  - Consider drawing text labels on the device with rle565 fonts, so changing a label doesn't need a new image.
- **USB device setup:**
  - Make it a composite device from the start, so modes 2 and 3 can add standard HID keyboard and/or USB MIDI interfaces beside the vendor interface.
  - Button events can go over the vendor interface's bulk IN endpoint; a separate HID interface isn't needed for mode 1.

**Web front end**: Still undecided. Two options:
- Hosted on the device itself (Wi-Fi)
- Hosted on a public website that talks to the device over WebUSB, as VIA/QMK do over WebHID

## Functionality

### Screen layout

```
+------------------------------------------+
| status bar (time, status)    — fixed     |
+------------------------------------------+
|  [ ]   [ ]   [ ]   [ ]   [ ]             |
|  [ ]   [ ]   [ ]   [ ]   [ ]   — page    |  swipes left/right
|  [ ]   [ ]   [ ]   [ ]   [ ]             |
+------------------------------------------+
| pager buttons (optional)     — fixed     |
+------------------------------------------+
```

- Buttons sit in a 5x3 grid (15 buttons per page), like a Stream Deck 15.
- Several pages of buttons are stored on the device. The user swipes left/right between them.
- A thin status bar at the top shows the time and other status. It does not move with the swipe.
- There may be pager buttons at the bottom. They don't move with the swipe either.

### Swipe feel

The swipe must be very smooth and feel like the iPhone home screen. **Not** like LVGL's clunky animation and spring-back.

- **While dragging:** The page follows the finger 1:1.
- **On release:**
  - Use the finger's speed and how far it has travelled to pick the target page.
  - Glide to that page with a smooth ease-out.
  - No bounce-back overshoot.
- **At the first/last page:** Possibly add resistance (rubber-banding).

### Rendering approach: custom bounce buffer fill

There are no full-screen framebuffers. The display is composed scanline by scanline in the RGB panel's bounce-buffer fill callback.

- **Panel setup:** Configure the RGB panel with `num_fbs = 0` and an `on_bounce_empty` callback. This is a change from aclock's double-buffered driver, which swaps framebuffers at vsync.
- **Page buffers:** Each page is pre-rendered with rle565 into its own off-screen RGB565 buffer in PSRAM. Each buffer covers only the page area (800 wide x the page area height).
- **Filling each line:** The fill routine checks which band the line is in:
  - **Status bar band:** Copy from the status bar buffer.
  - **Pager band:** Copy from the pager buffer.
  - **Page band:** Copy from whichever one or two page buffers are on screen. Use the current swipe offset to split each row between the outgoing and incoming page.
- **Updating content:** The swipe offset is sampled once per frame, at vsync, so a frame never mixes two offsets. Changing what's displayed is then just changing the offset or a pointer; nothing needs redrawing.
- **ISR budget:** The callback runs in ISR context and must stay fast: plain row copies only, no rle565 decoding. Use the 25 MHz pixel clock and 15-line bounce buffer from aclock as the starting point.
- **Memory:** One page buffer is about 640 KB if the page area is 400 lines tall. Keep only the current page and its neighbours rendered, and render the next neighbour in the background once a swipe settles.

### Open concerns

- **Touch polling rate:** The aclock touch driver polls at 50 Hz, which is likely too coarse for a smooth 1:1 drag. Consider polling faster, using the GT911 interrupt, and/or smoothing or predicting the finger position per frame.

### More to come

- USB protocol details (message framing, image upload, button events)
- How pages and buttons are stored and configured for modes 2 and 3

## Conventions

_TBD_
