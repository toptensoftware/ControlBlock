# ControlBlock

*"It's a block... for controlling things"*

ControlBlock is a flexible control surface built 
on an inexpensive ESP32-S3 touch panel. 

* Thr user inteface is a a grid of touch keys.
* Swipe to seirch betweeb Multiple pages of keys.
* Keys can send key presses like a USB leyboard or
  make http rewuests, send UDP requests, run Lua 
  scripts and more.  
* Standalone mode works independently of PC
* Hosted mode allows PC to control whats on scrren
  and handle key presses. 
* Wifi and USB connectivity. 
* Built-im status bar and clock

## Hardware

The [Waveshare ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm): an ESP32-S3 with
16 MB flash and 8 MB PSRAM, and an 800x480 capacitive touch screen. Nothing else is needed.

The board has two USB-C ports: **native USB** is the ControlBlock itself (and its power: use it, the UART port
alone can't run Wi-Fi reliably), and the **UART** port is for flashing and logs. See
[Getting started](docs/guide/getting-started.md#the-two-usb-ports).

## Getting started

1. **Flash the firmware** with the [web flasher](https://toptensoftware.github.io/ControlBlock/) (Chrome or Edge,
   nothing to install): plug the board in by its native USB port, hold BOOT and press RESET, click Install, then
   press RESET. Or use `esptool` with the files on the [Releases page](https://github.com/toptensoftware/ControlBlock/releases).
2. **Run the command line tool** with npx (needs [Node.js](https://nodejs.org/) 24 or later and
   [git](https://git-scm.com/)):

   ```sh
   npx github:toptensoftware/ControlBlock#cli info                          # the device's firmware, name, ...
   npx github:toptensoftware/ControlBlock#cli wifi set MyNetwork "password"
   ```

   `#cli` runs the latest release's CLI; `#cli-v0.1.0` pins one. `npm install -g github:toptensoftware/ControlBlock#cli`
   puts it on your path as `controlblock`.

A new board boots into an empty Home page. [Getting started](docs/guide/getting-started.md) covers it all:
flashing (also building the firmware from source), connecting to your PC, Wi-Fi, the clock and the name.

**Versions:** the firmware, the CLI and the host library share one version number (`0.1.0`); `info` shows the
firmware's and warns when it's older than the CLI.

## Standalone mode in brief

Write a layout:

```json
{
  "version": 1,
  "home": {
    "keys": [
      { "icon": "#play-pause", "label": "Play", "action": { "op": "hidKey", "keys": "space" } },
      { "label": "Copy", "action": { "op": "hidKey", "keys": "ctrl+c" } },
      { "label": "Paste", "action": { "op": "hidKey", "keys": "ctrl+v" } }
    ]
  }
}
```

Preview it and send it to the device:

```sh
controlblock render my-layout.json      # PNGs of each page, no device needed
controlblock upload my-layout.json      # bake, upload; the device restarts with your Home page
```

(`controlblock` is `npx github:toptensoftware/ControlBlock#cli`, an installed `controlblock`, or
`node cli/cli.js` in a clone.)

The [standalone guide](docs/guide/standalone-mode.md) goes on to placing keys, the look, gestures, network
actions, activities and devices, and scripts. The full layout reference is
[`docs/layout-format.md`](docs/layout-format.md).

## Hosted mode in brief

```sh
npm install github:toptensoftware/ControlBlock#cli     # the library ships with the CLI
```

```js
import { ControlBlock, KeyAction } from "@toptensoftware/controlblock";

const cb = await ControlBlock.open();       // the first ControlBlock on USB
await cb.takeControl();                     // the device boots standalone: take the screen
await cb.setStatusText({ left: "Hello", right: "{clock}" });
cb.on("key", e => console.log("key", e.id, e.action === KeyAction.RELEASE ? "tapped" : e.action));
```

Keys need pictures: the [hosted guide](docs/guide/hosted-mode.md#a-first-app) starts with a complete first app
(key images, keys, pages, events) and then covers every call. The library itself is described in
[`host/README.md`](host/README.md).

## Repository layout

| Folder | What |
|---|---|
| [`firmware/`](firmware/) | The device firmware (ESP-IDF, C++), and its host unit tests in `firmware/test/` |
| [`host/`](host/README.md) | The JavaScript host library: device API, key images, layout bake and upload |
| [`cli/`](cli/README.md) | The command line tool |
| [`examples/`](examples/README.md) | Example and test layouts, hosted demos, and a test server for network actions |
| [`urem2/`](urem2/README.md) | A real standalone layout: a home theatre remote (TV, Apple TV, receiver, PC) |
| [`docs/`](docs/) | The user guides (`docs/guide/`), the layout reference and the specs |
| [`flasher/`](flasher/), [`scripts/`](scripts/), [`.github/`](.github/) | The web flasher page, the release scripts (version bump, CLI package, flasher manifest, release assets) and the CI and release workflows |
| [`pad/`](pad/README.md), [`tools/`](tools/) | A macro pad (in progress), and build tools (test assets, code generation) |
| `rle565/`, `binpack/`, `SimpleLib/`, `lua/`, `PacketEncoder/` | Submodules: image codec, blob packer, C++ library, Lua, serial framing |

## Documentation

User guides:

- [Getting started](docs/guide/getting-started.md): hardware, flashing, connecting, Wi-Fi, clock, name.
- [Standalone mode](docs/guide/standalone-mode.md): writing, baking and uploading a layout.
- [Hosted mode](docs/guide/hosted-mode.md): driving the device from your own app.

Reference:

- [Command line tool](cli/README.md)
- [Host library](host/README.md)
- [Layout file format](docs/layout-format.md)
- [USB protocol](docs/protocol.md), for hosts in other languages
- [Diagnostics page](docs/diagnostics.md)

Design specs, for contributors: [standalone mode](docs/standalone.md), [image assets](docs/image-assets.md),
[page compositor](docs/page-compositor.md), [touch interactions](docs/interactions.md),
[status bar](docs/status-bar.md), [Wi-Fi](docs/wifi.md), [backlight](docs/backlight.md) and
[screen buffers](docs/screen-buffers.md) (partly superseded).

## Status and known limitations

Both modes work on the hardware, and the author uses a standalone layout ([urem2](urem2/README.md)) as a home
theatre remote. Known limitations:

- The backlight stays on: turning it off is disabled while a hardware glitch is investigated
  ([`docs/backlight.md`](docs/backlight.md)).
- Network actions are HTTP only (no HTTPS).
- No double press, and no media keys for `hidKey`.
- The macro pad (physical keys and an encoder) and a web front end are planned, not available.
- Windows is the tested host OS; Linux and macOS should work but are untested.
- The CLI and library aren't on the npm registry: they run and install from GitHub (`#cli`), which needs git.
- Flashing a release needs download mode (BOOT + RESET) and Chrome or Edge for the web flasher (or esptool).

Each guide ends with its own list of limitations.

## Development

```sh
git clone --recursive https://github.com/toptensoftware/ControlBlock.git
cd ControlBlock
npm install          # once: npm workspaces (host, cli, tools/test-icons, rle565/encoder), one root lock
npm test             # every Node suite
```

Tests that need no device:

| Package | From | Command |
|---|---|---|
| All the Node suites below (host, CLI, binpack, scripts, examples, urem2) | repo root | `npm test` |
| Host library and bake | `host/` | `npm test` |
| CLI | `cli/` | `npm test` |
| binpack | repo root | `npm test --prefix binpack` |
| Release scripts (version bump, CLI package, flasher, release) | repo root | `node --test "scripts/**/*.test.mjs"` |
| Examples | repo root | `node --test "examples/**/*.test.mjs"` |
| urem2 layout | repo root | `node --test "urem2/**/*.test.mjs"` |
| Firmware unit tests (Windows, MSVC, [mk](https://github.com/toptensoftware/mk.js)) | `firmware/test/` | `mk run` |
| Firmware version string (CMake) | repo root, ESP-IDF shell | `cmake -P firmware/test/version_test.cmake` |
| urem2 Apple TV daemon | `urem2/atvdaemon/` | `python -m unittest` |

Building the firmware is in [Getting started](docs/guide/getting-started.md#option-b-build-from-source). CI
(`.github/workflows/`) runs the tests and the firmware build on every push to `main`.

**Releasing:** `node scripts/bump-version.mjs <x.y.z|patch|minor|major>` sets the one product version (root,
`host/` and `cli/` package.json and the lock) and prints the git commands; pushing the tag `v<version>` runs the
release workflow: the firmware build, the GitHub Release, the web flasher on Pages and the CLI package on the `cli`
branch (tag `cli-v<version>`). Builds not made at a release tag report `<version>-dev+<commit>[.dirty]`.

## Licence

MIT: see [LICENSE](LICENSE). Third-party components (ESP-IDF, TinyUSB, Lua, the Inter font, icons and others) and
their licences are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
