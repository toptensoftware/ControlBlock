# Getting started

This guide takes you from a new board to a running ControlBlock: flashing the firmware, connecting it to your
PC, and setting up Wi-Fi, the clock and the device's name. After that, pick a mode:
[standalone](./standalone-mode.md) (the device runs a layout on its own) or [hosted](./hosted-mode.md) (an app on
your PC drives the screen).

Windows is the tested host OS. The tools are Node.js and should work on Linux and macOS too, but that is untested.

## What you need

- A **Waveshare ESP32-S3-Touch-LCD-4.3**
  ([product page](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm)): an ESP32-S3 with 16 MB flash and 8 MB
  PSRAM, and an 800x480 touch screen.
- A USB-C cable (two are handy, see below).
- To flash the firmware: **Chrome or Edge** on a desktop computer (the web flasher), or Espressif's `esptool`.
- On your PC: [Node.js](https://nodejs.org/) 24 or later and [git](https://git-scm.com/) for the command line tool
  (npx fetches it from GitHub, and one of its dependencies comes from GitHub too).
- To build the firmware yourself: ESP-IDF v6.0 (see [Option B](#option-b-build-from-source)).

## The two USB ports

The board has two USB-C ports, and ControlBlock uses them for different things:

| Port | Used for |
|---|---|
| **Native USB** (the ESP32-S3's own USB) | The ControlBlock device itself: the PC talks to it here (the CLI, the host library), and it appears as a USB keyboard for standalone `hidKey` actions. Also for flashing a release in download mode (BOOT + RESET): the web flasher uses it. |
| **UART** (CH343P USB-serial chip) | Flashing a firmware you built, and reading its log. |

**Power the board from the native USB port** (on its own, or together with the UART port). The UART port alone
doesn't supply enough power for Wi-Fi to work reliably: it may connect but then lose nearly all traffic.

While developing, plugging in both cables is easiest: native USB for power and the device, UART for flashing and
logs.

## Getting the firmware

### Option A: prebuilt release

Every release is on the [GitHub Releases page](https://github.com/toptensoftware/ControlBlock/releases), and the
latest one is on the **web flasher**: <https://toptensoftware.github.io/ControlBlock/>.

**With the web flasher** (Chrome or Edge; nothing to install):

1. Plug the board into your computer by its **native USB port** (no driver needed).
2. Put the board in download mode: **hold BOOT**, press and release **RESET**, then release BOOT.
3. Open the [web flasher](https://toptensoftware.github.io/ControlBlock/), click **Install** and pick the board's
   port.
4. Leave **"Erase device" unticked** to keep your Wi-Fi settings and stored layout. Ticking it wipes everything:
   only for a fresh start.
5. When it's done, press **RESET**. The firmware starts and the port the flasher used goes away; that's normal
   (the firmware uses the native port for itself).

**With esptool** (if the web flasher can't be used): get Espressif's standalone
[`esptool`](https://github.com/espressif/esptool/releases) executable (no Python needed) or `pip install esptool`,
and download the release's files from the Releases page. Put the board in download mode as above, then write the
three parts (replace `COM5` with the board's port, and `0.1.0` with the release's version):

```sh
esptool --chip esp32s3 -p COM5 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 controlblock-0.1.0.bin
```

That keeps your settings and stored layout. The release also has `controlblock-<version>-full.bin`, the same three
parts merged into one image for a fresh install (`write-flash 0x0 controlblock-<version>-full.bin`): its padding
**wipes the Wi-Fi, name and clock settings**, though the stored layout survives.

Press RESET when esptool has finished.

### Option B: build from source

1. **Install ESP-IDF v6.0** by following Espressif's
   [getting started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html)
   (on Windows, the ESP-IDF installer). Other ESP-IDF versions aren't tested.

2. **Clone the repo with its submodules:**

   ```sh
   git clone --recursive https://github.com/toptensoftware/ControlBlock.git
   ```

   If you already cloned without `--recursive`, run `git submodule update --init` in the repo.

3. **Open an ESP-IDF shell** (the "ESP-IDF PowerShell" or "ESP-IDF Command Prompt" shortcut the installer adds,
   or run ESP-IDF's `export.ps1` / `export.sh`) and go to the `firmware` folder.

   If your shell has the `MSYSTEM` variable set (it does when started from Git Bash), clear it first, or
   `idf.py` refuses to run:

   ```powershell
   Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
   ```

4. **Build and flash** over the UART port. Replace `COM4` with your board's port (on Linux something like
   `/dev/ttyACM0` or `/dev/ttyUSB0`):

   ```sh
   idf.py --preset release -p COM4 flash monitor
   ```

   `flash` writes the bootloader, the partition table and the app, which a new board needs. `monitor` then shows
   the device's log (press `Ctrl+]` to leave it). There are two build presets: `release` (optimised; use this)
   and `debug` (`build/debug`, with assertions on).

   If the monitor warns of a "checksum mismatch between flashed and built applications", the flash didn't take:
   run it again and check for `Hash of data verified.`

If flashing over the UART port doesn't work, you can flash over the native port instead: hold the **BOOT**
button, press and release **RESET**, then release BOOT, and flash to the port that appears.

**Updating the firmware later** is the same as the first time: the web flasher (with "Erase device" unticked),
the three-part esptool command, or `idf.py ... flash`. Your stored layout and Wi-Fi settings survive it. If a firmware update changes the layout format, the device ignores the old layout and shows an
empty Home until you bake and upload it again (see the [standalone guide](./standalone-mode.md)).

## Connecting to your PC

The device is a USB device with ID `303a:4020` on the native port.

- **Windows:** no driver to install. The device asks Windows for the built-in WinUSB driver itself.
- **Linux:** needs a udev rule so your user can open the device. *Untested*; a standard rule is:

  ```sh
  # /etc/udev/rules.d/70-controlblock.rules
  SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4020", MODE="0660", TAG+="uaccess"
  ```

  Then reload the rules and replug the device:

  ```sh
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```

- **macOS:** untested.
- **Browsers:** Chrome and Edge can talk to the device directly over WebUSB (see the
  [hosted guide](./hosted-mode.md)).

**One app at a time.** Only one program can have the device open (on Windows, WinUSB is exclusive). If the CLI
reports that it can't find or open the device, close any other app using it (a hosted app, a browser tab) and
try again.

## Installing the tools

The command line tool (CLI) sets up Wi-Fi, the clock and the name, and bakes and uploads layouts. It needs
Node.js 24 or later and git. The simplest way to run it is with npx, straight from GitHub, nothing to install
first:

```sh
npx github:toptensoftware/ControlBlock#cli            # prints the usage
```

`#cli` is the latest release's CLI. To pin one release (for scripts, or to match your firmware), name its tag:

```sh
npx github:toptensoftware/ControlBlock#cli-v0.1.0 --version
```

The first run downloads the CLI and its dependencies (some tens of MB: image and USB libraries); later runs use
npm's cache. Two other ways to run it:

- **Installed:** `npm install -g github:toptensoftware/ControlBlock#cli` puts it on your path as `controlblock`.
- **From a clone of the repo** (for development): `npm install` once in the repo root, then `node cli/cli.js`
  from the repo root.

The rest of the guides write **`controlblock`** for whichever you use: read `controlblock info` as
`npx github:toptensoftware/ControlBlock#cli info` if you use npx. Every command is described in
[`cli/README.md`](../../cli/README.md).

## First boot

Power the board from the native USB port. The device boots into **standalone mode**, showing **Home**. A new
board has no layout stored, so Home is empty and the screen stays mostly black (no keys, and no clock until
Wi-Fi is set up). That's expected. You give it keys either by uploading a layout ([standalone guide](./standalone-mode.md)) or by
driving it from an app ([hosted guide](./hosted-mode.md)).

To check that the CLI finds the board, and which firmware it runs:

```sh
controlblock info          # firmware, protocol and layout blob versions, name, key grid
controlblock --version     # the CLI's own version
```

`info` warns if the firmware is older than the CLI: flash the matching release (or run the CLI release that
matches your firmware, `#cli-v<version>`).

The backlight is always on for now. Turning it off is disabled in the firmware while a hardware problem with
switching it on is investigated, so a request to turn it off leaves the screen lit (see
[`docs/backlight.md`](../backlight.md)).

## Wi-Fi

Wi-Fi is optional. You need it for standalone actions that use the network (HTTP requests, UDP packets,
wake-on-LAN), for setting the clock automatically, and for the diagnostics page. The ESP32-S3 supports 2.4 GHz
networks.

```sh
controlblock wifi set MyNetwork "my password"    # store the network and connect
controlblock wifi status                         # e.g. CONNECTED, 192.168.1.5, -52 dBm, ...
```

The device keeps one network, survives reboots and firmware updates, and reconnects by itself. `wifi get`,
`wifi clear` and `wifi wait` are in [`cli/README.md`](../../cli/README.md#wifi-action---serial-s). The design
(including how the password is stored) is in [`docs/wifi.md`](../wifi.md).

## Clock and time zone

The status bar can show a clock. With Wi-Fi, the device sets its clock by NTP. Tell it your time zone as a POSIX
TZ string, for example for Sydney:

```sh
controlblock time zone AEST-10AEDT,M10.1.0,M4.1.0/3
```

Without Wi-Fi, the board has no battery-backed clock, so the time is lost at power-off. Set it from your PC with
`controlblock time` after each power-up. More time zone examples and the NTP server option are in
[`cli/README.md`](../../cli/README.md#time-action---serial-s).

## Device name

The device's network name is `controlblock-` plus the last three bytes of its MAC address, e.g.
`controlblock-a1b2c3`. It's used for mDNS (`<name>.local`) and as its DHCP hostname. To change it:

```sh
controlblock name                 # show the current name
controlblock name set den-panel   # now den-panel.local
```

## Display timing (v0.2, unreleased; experimental)

The screen refreshes about 49 times a second. To try a lower frame rate (it gives the device more time per frame),
set the display timing; the device keeps it over a restart:

```sh
controlblock display              # show the timing and its limits
controlblock display --fps 32.5   # 750 lines a frame at 20 MHz
controlblock display --reset      # back to the firmware's own
```

Changing the frame rate shows a one-off jump on the screen. The options are in
[`cli/README.md`](../../cli/README.md#display---clock-mhz---lines-n----fps-f----reset----serial-s-v02-unreleased-experimental).

## Diagnostics page

While the device is on Wi-Fi, it serves a small status page at `http://<name>.local/` (for example
`http://controlblock-a1b2c3.local/`): status, memory, Wi-Fi, the layout, and its recent log. The endpoints are
described in [`docs/diagnostics.md`](../diagnostics.md).

**The page has no password.** Anyone on your local network can read it. It's read-only (nothing on it changes
the device), but the log may show your network's name, IP addresses and the URLs your layout calls.

## Next steps

- [Standalone mode](./standalone-mode.md): write a layout of pages and keys that send keystrokes, HTTP and UDP
  requests or run scripts, bake it, and upload it to the device.
- [Hosted mode](./hosted-mode.md): drive the screen from your own Node.js or browser app with the host library.
