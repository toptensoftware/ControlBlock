# Third-party notices

ControlBlock's own code is MIT licensed (see [LICENSE](LICENSE)). It builds on the third-party components below,
each under its own licence. Licences are linked rather than copied, except where noted.

## In the firmware binary

A ControlBlock firmware image (built from `firmware/`) contains the following. Anyone redistributing a firmware
binary should pass this file on with it.

| Component | Used for | Licence | Copyright |
|---|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) v6.0 | SDK, drivers, RTOS, networking | [Apache-2.0](https://github.com/espressif/esp-idf/blob/master/LICENSE) | Espressif Systems (Shanghai) Co., Ltd. |
| [FreeRTOS](https://www.freertos.org/) (bundled with ESP-IDF) | RTOS kernel | [MIT](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/LICENSE.md) | Amazon.com, Inc. or its affiliates |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) (bundled with ESP-IDF) | TCP/IP stack | [BSD-3-Clause](https://git.savannah.nongnu.org/cgit/lwip.git/tree/COPYING) | Swedish Institute of Computer Science |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (bundled with ESP-IDF) | crypto (Wi-Fi security) | [Apache-2.0](https://github.com/Mbed-TLS/mbedtls/blob/development/LICENSE) | The Mbed TLS Contributors |
| [newlib](https://sourceware.org/newlib/) (bundled with the ESP-IDF toolchain) | C library | [BSD-style and others](https://sourceware.org/newlib/COPYING.NEWLIB) | Red Hat, Inc. and others |
| Espressif Wi-Fi and PHY libraries (bundled with ESP-IDF, binary only) | Wi-Fi radio | Apache-2.0, in object form ([esp32-wifi-lib](https://github.com/espressif/esp32-wifi-lib), [esp-phy-lib](https://github.com/espressif/esp-phy-lib)) | Espressif Systems (Shanghai) Co., Ltd. |
| [TinyUSB](https://github.com/hathach/tinyusb) (`espressif/tinyusb` 0.19) | USB device stack | [MIT](https://github.com/hathach/tinyusb/blob/master/LICENSE) | Copyright (c) 2018, hathach (tinyusb.org) |
| [esp_tinyusb](https://components.espressif.com/components/espressif/esp_tinyusb) 2.x | TinyUSB integration | Apache-2.0 | Espressif Systems (Shanghai) Co., Ltd. |
| [esp_lcd_touch](https://components.espressif.com/components/espressif/esp_lcd_touch) and [esp_lcd_touch_gt911](https://components.espressif.com/components/espressif/esp_lcd_touch_gt911) 1.x | GT911 touch driver | Apache-2.0 | Espressif Systems (Shanghai) Co., Ltd. |
| [mdns](https://components.espressif.com/components/espressif/mdns) 1.x | `<name>.local` on the network | Apache-2.0 | Espressif Systems (Shanghai) Co., Ltd. |
| `firmware/main/waveshare_rgb_lcd_port.cpp` (adapted Espressif example code, via Waveshare's demo) | RGB panel setup | CC0-1.0 (header kept in the file) | Espressif Systems (Shanghai) Co., Ltd. |
| [Lua](https://www.lua.org/) 5.4.7 (`lua/` submodule) | the `script` op | [MIT](https://www.lua.org/license.html) (notice at the end of `lua/lua.h`) | Copyright (C) 1994-2024 Lua.org, PUC-Rio |
| [Inter](https://rsms.me/inter/) SemiBold (packed into `firmware/assets/ui.bin`) | status bar font | SIL Open Font License 1.1, full text in [`tools/test-icons/fonts/OFL.txt`](tools/test-icons/fonts/OFL.txt) | Copyright (c) 2016 The Inter Project Authors |
| [rle565](https://github.com/toptensoftware/rle565) (`rle565/` submodule) | image and text renderer | [MIT](rle565/LICENSE) | Copyright (c) 2026 Topten Software |
| [SimpleLib](https://github.com/toptensoftware/simplelib) (`SimpleLib/` submodule, `Core/` only) | C++ utility library | no licence file upstream; by the same author as ControlBlock | Topten Software |

The ESP-IDF managed components (`espressif/*`) are fetched by the ESP-IDF component manager at build time
(`firmware/main/idf_component.yml`, versions pinned in `firmware/dependencies.lock`); their licence files are in
`firmware/managed_components/` after a build.

## In baked layouts and the host tools

| Component | Used for | Licence | Copyright |
|---|---|---|---|
| [button-face](https://github.com/toptensoftware/button-face) (npm dependency of `host/` and `tools/test-icons/`) | key face, icon and label rendering | MIT | Copyright (c) 2026 Topten Software |
| [Material Symbols](https://github.com/google/material-design-icons) | many of button-face's built-in `#name` icons are derived from them; layouts can also name `@name` symbols, which the bake downloads. Either way the icons end up in baked layouts (and in `firmware/assets/test_keys.bin`, stress builds only) | [Apache-2.0](https://github.com/google/material-design-icons/blob/master/LICENSE) | Google LLC |
| [binpack](https://github.com/toptensoftware/binpack) (`binpack/` submodule) | layout blob packing | [MIT](binpack/LICENSE) | Topten Software |
| [PacketEncoder](https://github.com/toptensoftware/PacketEncoder) (`PacketEncoder/` submodule) | macro pad wire format (`pad/`, `tools/pad-sim/`) | [MIT](PacketEncoder/LICENSE) | Copyright (c) 2026 Topten Software |
| [atv-daemon](https://github.com/toptensoftware/atv-daemon) (`urem2/atvdaemon/` submodule) | Apple TV bridge for the urem2 layout; uses [pyatv](https://github.com/postlund/pyatv) (MIT) | no licence file upstream; by the same author as ControlBlock | Topten Software |

### npm packages

These are installed by `npm install` from the npm registry (or GitHub); this repository doesn't include or
redistribute them. Each comes with its own licence in `node_modules/`.

| Package | Used by | Licence |
|---|---|---|
| [usb](https://github.com/node-usb/node-usb) | `host/` (USB access from Node) | MIT |
| [sharp](https://github.com/lovell/sharp) | `host/`, `tools/test-icons/` (image composition) | Apache-2.0; its prebuilt `@img/sharp-libvips-*` binaries are LGPL-3.0-or-later |
| [@napi-rs/canvas](https://github.com/Brooooooklyn/canvas) (via button-face) | rendering (Skia) | MIT |
| [wasmoon](https://github.com/ceifa/wasmoon) | `host/` bake (Lua syntax check) | MIT |
| [fontkit](https://github.com/foliojs/fontkit) (via `rle565/encoder`) | font packing | MIT |
| [serialport](https://github.com/serialport/node-serialport) | `tools/pad-sim/` | MIT |

## Trademarks

Brand logos among button-face's built-in icons (`steam`, `playstation`, `apple-tv`) and in `urem2/icons/`
(`netflix.svg`, `prime.svg`, `youtube.svg`) are trademarks of their respective owners, used here only to label
keys of a personal remote.
