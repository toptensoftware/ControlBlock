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
- On your PC: [Node.js](https://nodejs.org/) 24 or later and [git](https://git-scm.com/) (the host library
  installs one of its dependencies from GitHub).
- To build the firmware yourself: ESP-IDF v6.0 (see [Option B](#option-b-build-from-source)).

## The two USB ports

The board has two USB-C ports, and ControlBlock uses them for different things:

| Port | Used for |
|---|---|
| **Native USB** (the ESP32-S3's own USB) | The ControlBlock device itself: the PC talks to it here (the CLI, the host library), and it appears as a USB keyboard for standalone `hidKey` actions. |
| **UART** (CH343P USB-serial chip) | Flashing the firmware and reading its log. |

**Power the board from the native USB port** (on its own, or together with the UART port). The UART port alone
doesn't supply enough power for Wi-Fi to work reliably: it may connect but then lose nearly all traffic.

While developing, plugging in both cables is easiest: native USB for power and the device, UART for flashing and
logs.

## Getting the firmware

### Option A: prebuilt release

> **Coming soon.** Prebuilt firmware images and flashing instructions will be added here by the release work.
> Until then, build from source (Option B).

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

**Updating the firmware later** is the same `idf.py ... flash` command. Your stored layout and Wi-Fi settings
survive it. If a firmware update changes the layout format, the device ignores the old layout and shows an
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

The command line tool (CLI) and the host library are in the repo. Install the library's dependencies once:

```sh
cd host
npm install
```

The CLI needs nothing more. Run it from the repo root:

```sh
node cli/cli.js            # prints the usage
```

Optionally, `npm link` in `cli/` puts it on your path as `controlblock`. Every command is described in
[`cli/README.md`](../../cli/README.md).

## First boot

Power the board from the native USB port. The device boots into **standalone mode**, showing **Home**. A new
board has no layout stored, so Home is empty and the screen stays mostly black (no keys, and no clock until
Wi-Fi is set up). That's expected. You give it keys either by uploading a layout ([standalone guide](./standalone-mode.md)) or by
driving it from an app ([hosted guide](./hosted-mode.md)).

The backlight is always on for now. Turning it off is disabled in the firmware while a hardware problem with
switching it on is investigated, so a request to turn it off leaves the screen lit (see
[`docs/backlight.md`](../backlight.md)).

## Wi-Fi

Wi-Fi is optional. You need it for standalone actions that use the network (HTTP requests, UDP packets,
wake-on-LAN), for setting the clock automatically, and for the diagnostics page. The ESP32-S3 supports 2.4 GHz
networks.

```sh
node cli/cli.js wifi set MyNetwork "my password"    # store the network and connect
node cli/cli.js wifi status                         # e.g. CONNECTED, 192.168.1.5, -52 dBm, ...
```

The device keeps one network, survives reboots and firmware updates, and reconnects by itself. `wifi get`,
`wifi clear` and `wifi wait` are in [`cli/README.md`](../../cli/README.md#wifi-action---serial-s). The design
(including how the password is stored) is in [`docs/wifi.md`](../wifi.md).

## Clock and time zone

The status bar can show a clock. With Wi-Fi, the device sets its clock by NTP. Tell it your time zone as a POSIX
TZ string, for example for Sydney:

```sh
node cli/cli.js time zone AEST-10AEDT,M10.1.0,M4.1.0/3
```

Without Wi-Fi, the board has no battery-backed clock, so the time is lost at power-off. Set it from your PC with
`node cli/cli.js time` after each power-up. More time zone examples and the NTP server option are in
[`cli/README.md`](../../cli/README.md#time-action---serial-s).

## Device name

The device's network name is `controlblock-` plus the last three bytes of its MAC address, e.g.
`controlblock-a1b2c3`. It's used for mDNS (`<name>.local`) and as its DHCP hostname. To change it:

```sh
node cli/cli.js name                 # show the current name
node cli/cli.js name set den-panel   # now den-panel.local
```

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
