# Documentation and licensing

Status: Complete

## Description
The repo has no README.md and no LICENSE. Add:

1. **LICENSE:** standard MIT, "Copyright 2026 Topten Software". Also licence fields and notices wherever
   needed (package.json files, third-party components and submodules).
2. **README.md** at the repo root: features, the target device (Waveshare ESP32-S3-Touch-LCD-4.3), and basic
   instructions for hosted mode and standalone mode.
3. **Detailed user documentation** for each mode as separate .md files: hosted mode (driving the device from a
   PC with the host library) and standalone mode (writing, baking and uploading a layout; activities, devices,
   ops, scripts). These are user guides, distinct from the existing specs in `docs/`.

## Notes

Investigated 2026-11-15. The repo (`toptensoftware/ControlBlock`) is **PRIVATE** on GitHub, with no LICENSE and no
root README. Git authors: "Brad Robinson" at `contact@` (39 commits) and `brad@toptensoftware.com` (1).

**Re-checked 2026-11-15, after the button-face switch** (`wip/complete-switch-to-button-face.md`; uncommitted in
the working tree, no commits since `aab3962`): the `node-buttonImage` submodule is gone (staged removal in
`.gitmodules`), replaced by the npm dependency `@toptensoftware/button-face` (github, not a submodule) in `host/`
and `tools/test-icons/`. Node `canvas` is gone (button-face uses `@napi-rs/canvas`). `host` and `cli` now need
Node >= 24. The sections below are updated to match; nothing else in scope changed.

### 1. Licensing

**Submodules** (`.gitmodules`; licence from the local tree and `gh api repos/.../license`):

| Submodule | Repo | Visibility | Licence | Used by |
|---|---|---|---|---|
| `rle565` | toptensoftware/rle565 | public | MIT, "Copyright (c) 2026 Topten Software" | firmware (`rle565.c`), host (encoder), tools |
| `SimpleLib` | toptensoftware/simplelib (moved; `.gitmodules` has the old `SimpleLib` URL, which redirects) | public | **none** (no LICENSE file, no headers) | firmware, host tests |
| `binpack` | toptensoftware/binpack | public | MIT, same `[2026] [Topten Software]` brackets; package.json `MIT` | host bake |
| `PacketEncoder` | toptensoftware/PacketEncoder | **PRIVATE**, and `.gitmodules` uses an **SSH URL** (`git@github.com:...`) | MIT | pad firmware, tools/pad-sim (not the S3 firmware) |
| `lua` | lua/lua (official) | public | MIT (notice at the end of `lua.h`; no separate file in the repo) | firmware, host tests |
| `urem2/atvdaemon` | toptensoftware/atv-daemon | public | **none** (README only); it depends on pyatv (MIT) | urem2 only |

- **button-face** (`@toptensoftware/button-face` 0.0.1, npm dependency from `git+https://github.com/toptensoftware/
  button-face.git`, public, pinned by each lock): MIT, "Copyright © 2026 Topten Software" (clean, no brackets);
  `package.json` has `license: MIT`, `author`, and ships `LICENSE`. Its README has a proper License section
  crediting Material Symbols (Apache-2.0) and the brand logos (see Fonts and icons). Used by the host bake and
  tools/test-icons; no longer a submodule, so `node-buttonImage`'s bracketed copyright line is moot here.
- A public ControlBlock with a private PacketEncoder submodule (and an SSH URL) breaks `git clone --recursive` for
  everyone else. Fix: make PacketEncoder public and switch the URL to https (user's call: they own it).
- SimpleLib and atv-daemon need a LICENSE added in their own repos (user's repos; can't be done from here).
- `binpack`'s dependency `@toptensoftware/clargs` (github dep): package.json says `Apache-2.0`, GitHub detects
  no licence (NOASSERTION). Bake/CLI only; minor, the user's repo.

**Firmware third-party code** (what a released firmware binary contains):
- ESP-IDF v6.0: Apache-2.0 (bundles FreeRTOS MIT, lwIP BSD-3, mbedTLS Apache-2.0, newlib, and Espressif's
  redistributable Wi-Fi/PHY binary blobs).
- Managed components (`firmware/main/idf_component.yml`; `firmware/managed_components/` is gitignored, not in the repo):
  `espressif/esp_lcd_touch` and `esp_lcd_touch_gt911` (Apache-2.0), `espressif/esp_tinyusb` (Apache-2.0),
  `espressif/tinyusb` (MIT, "Copyright (c) 2018, hathach (tinyusb.org)"), `espressif/mdns` (Apache-2.0).
  `mdns` isn't in CLAUDE.md's component list (used by the Wi-Fi diagnostics, `docs/diagnostics.md`).
- Lua 5.4.7 (MIT, Lua.org, PUC-Rio), rle565 (MIT), SimpleLib (unlicensed, see above).
- `firmware/main/waveshare_rgb_lcd_port.cpp` keeps an Espressif header `SPDX-License-Identifier: CC0-1.0` (Espressif
  example code via Waveshare's demo, via `../aclock`). CC0 needs no notice; keep the header. The other adapted
  drivers (`waveshare_touch_port`, `i2c`, `ch422g`) have no headers (written in aclock): fine as MIT.
- No other files in `firmware/main` carry copyright headers.

**Fonts and icons:**
- `tools/test-icons/fonts/Inter-SemiBold.ttf` (tracked): Inter by Rasmus Andersson, **SIL OFL 1.1** (noted in
  `tools/test-icons/fonts/README.md`). It is packed into `firmware/assets/ui.bin` (status bar font), so every
  firmware binary embeds it. OFL allows embedding; the repo should carry the OFL text (or a link) and the copyright
  line, e.g. an `OFL.txt` beside the font plus a notices entry.
- button-face's built-in icons (42, the `#name` icons, bundled in its `src/icons.js`): its README's License section
  says many are **derived from Google Material Symbols (Apache-2.0)** and bundled under that licence, and that
  `steam`, `playstation` and `apple-tv` are brand logos, trademarks of their owners. button-face credits this
  itself (done). Layouts can also name **`@name` Material Symbols** (the bake allows them, `look.js` `isFileSrc`;
  no example or urem2 uses one yet): downloaded at bake time and baked into the blob, so a blob can carry them.
  Built-in icons are baked into layouts and into `firmware/assets/test_keys.bin` (stress builds only), so our
  notices should still carry one Material Symbols line.
- `urem2/icons/` (`netflix.svg`, `prime.svg`, `youtube.svg`): brand logos, origin not recorded. Trademarked; fine
  for a personal remote, but flag if the repo goes public.

**package.json `license` fields:**

| File | name | license | private |
|---|---|---|---|
| `host/package.json` | `@toptensoftware/controlblock` 0.1.0 (Node >= 24) | **missing** | true |
| `cli/package.json` | `controlblock-cli` (Node >= 24) | **missing** | true |
| `tools/test-icons/package.json` | `controlblock-test-icons` | **missing** | true |
| `tools/pad-sim/package.json` | `pad-sim` 0.1.0 (Node >= 22) | **missing** | true |
| `binpack`, `rle565/encoder`, `PacketEncoder` (submodules); `button-face` (github dep) | | MIT | |

(`examples/` and `urem2/` have no package.json.) Add `"license": "MIT"` to the four of ours (and `author` if
wanted). The versioning job (`wip/wip-versioning-deployment.md`) will make the CLI npx-runnable, so it will publish
packages: their `license`/`files` must include the LICENSE then (coordinate; that job owns publishing).

**npm dependencies at a glance:** `usb` MIT, `wasmoon` MIT, `@napi-rs/canvas` MIT (prebuilt Skia, via button-face;
replaces node `canvas`), `fontkit` MIT (via `rle565/encoder`), `serialport` MIT, `sharp`
Apache-2.0 (its prebuilt `@img/sharp-libvips-*` binaries are LGPL-3.0, installed by npm, not redistributed by
us). All compatible with MIT; nothing is vendored into this repo.

**Recommendations:**
- Root `LICENSE`: standard MIT text, "Copyright (c) 2026 Topten Software" (the same form as rle565/PacketEncoder).
  Optionally fix the `[2026] [Topten Software]` brackets in binpack (its repo).
- A root `THIRD-PARTY-NOTICES.md` **is warranted**, mainly because the CI release will ship firmware binaries
  (Apache-2.0/MIT/BSD/OFL all require the notice to travel with binaries): list ESP-IDF (and its bundled
  FreeRTOS/lwIP/mbedTLS), TinyUSB, esp_tinyusb, esp_lcd_touch(+gt911), mdns, Lua, Inter (OFL), Material Symbols
  (via button-face's built-in and `@` icons, in baked layouts), and our own MIT submodules and button-face. Link to each licence rather than pasting full texts, except the
  short MIT/OFL ones if wanted. The release job should attach it (or point to it) with the binaries.
- Per-file licence headers: **not needed** for MIT (the root LICENSE covers the repo). Keep the existing Espressif
  CC0 header. Optional SPDX lines are a nicety, not a requirement; recommend not adding them.

### 2. Privacy / publishing concerns

The repo is private today, so nothing is exposed yet. If it goes public, these point at the user's home network
and machines (nothing secret found: no Wi-Fi passwords, tokens or keys; test passwords are placeholders like
`password1`/`secret`):

| What | Where | Note |
|---|---|---|
| Home LAN IPs `10.1.1.188` (irtx), `10.1.1.2` (NAS `cool`), `10.1.1.105` (HOT PC) | `urem2/targets.mjs`, `urem2/README.md`, `CLAUDE.md`, `wip/complete-urem2*.md`, `wip/complete-atvdaemon-docker.md` | private RFC1918 addresses; low risk |
| Host names `yama`, `cool`/`cool.lan` | `urem2/targets.mjs`, `urem2/README.md`, CLAUDE.md | low risk |
| **A real MAC address** `04:d4:c4:4e:da:2b` (HOT PC WoL) | `urem2/tables/hotpc.mjs` | the only real hardware identifier found; low risk but personal |
| Test server PC `10.1.1.126` and host name `braddev` | `examples/test-common.mjs` (`SERVER`), `examples/net-test.json` (23 hits), `examples/README.md`; `braddev` also in `firmware/test/TestNetRequest.cpp` (as a parse fixture) | users must edit these anyway |
| Local paths `C:/Users/Brad/...` | `.vscode/settings.json` (tracked; `cmake.sourceDirectory`), `wip/*.md`, CLAUDE.md | |
| Sister projects `../urem`, `../aclock`, `../irtx` | CLAUDE.md, urem2/README.md, wip docs | `urem` is private, `irtx` public |
| Dev history | `wip/` (22 complete-*.md, ~800 KB), `docs/standalone-ops-plan.md`, `docs/backlight-waveshare-support.md` (a support request) | not secret, but internal |
| Wi-Fi diagnostics | `docs/diagnostics.md` | the device serves `/status` and `/log` on port 80 to the LAN with no auth; logs show SSIDs, IPs and URLs (not passwords). A user-facing "known limitations"/security note |

Recommendations (no change made):
- **Fine to keep:** private IPs and host names in urem2 (it is explicitly "my house" config) and the CLAUDE.md/wip
  history, if the user accepts them being public.
- **Better for a public repo:** move the test server address out of source into one place users edit or an env
  variable (e.g. `CB_TEST_SERVER` read by `test-common.mjs`; `net-test.json` can't read env, so turn it into a
  `.mjs` that imports `SERVER`). For urem2, either keep as an example of a real layout or move `targets.real` into a
  gitignored `targets.local.mjs` with a checked-in sample. Replace the HOT PC MAC with a placeholder if moved.
- `.vscode/settings.json`: drop the absolute path (a relative `${workspaceFolder}/firmware`) or untrack it.
- urem2's brand icons: keep (personal layout) or note them as trademarks of their owners in the notices.
- None of this blocks adding LICENSE/README while the repo is private; it matters only when going public.

### 3. Documentation inventory and outline

**Existing docs** (audience; currency, checked against CLAUDE.md):

| Doc | Audience | State |
|---|---|---|
| `docs/protocol.md` (45 KB) | spec (host implementers) | current (v2; source of truth) |
| `docs/layout-format.md` (41 KB) | **user-level reference** (says so in its title) with some spec/pipeline detail | mostly current; stale bits: "Look properties *(proposed)*", "Inheritance *(proposed; not yet confirmed)*", the Bindings section still names `macro1..6`/`volEnc` and `turnLeft`/`turnRight` as "proposed", while the bake/urem2 use `key1..key32` and `enc1` with event `turn` (pad not yet built on the S3) |
| `docs/standalone.md` (59 KB) | design spec/firmware internals | current design, but its Status header is stale (says the `script` op is "not yet run on the hardware"; it works) |
| `docs/image-assets.md`, `page-compositor.md`, `interactions.md`, `status-bar.md`, `screen-buffers.md` (partly superseded) | firmware internals | current except screen-buffers |
| `docs/wifi.md` | design + implementation | current |
| `docs/diagnostics.md` | user + spec (endpoints, mDNS) | current; feature not in CLAUDE.md's module list |
| `docs/backlight.md` | design + hardware notes | current (backlight locked on) |
| `docs/standalone-ops-plan.md`, `docs/backlight-waveshare-support.md` | internal (plan, support request) | historical |
| `host/README.md` | developer/user (library API) | current; **the Windows/Linux driver lines appear twice** (lines 17-18 and 37-38); "Use" example has a pre-v2-style flow but matches v2 |
| `cli/README.md` | user | current and good (bake, upload, render, wifi, time, name; POSIX TZ table) |
| `examples/README.md` | developer/tester | current; says the test server address is hard-coded |
| `urem2/README.md` | the user's own setup | current |
| `pad/README.md` | developer | pad firmware not run on a board; S3 side not built |

Re-check after the button-face switch: `host/README.md`, `docs/layout-format.md`, `docs/standalone.md` and
`urem2/README.md` were edited by that job (node-buttonImage -> button-face wording only). The duplicated driver lines
in `host/README.md` (now lines 17-18 and 39-40), layout-format.md's "proposed" markers and `macro1`/`volEnc`/
`turnLeft` names, and standalone.md's stale Status lines (13, 19) are all still there.

**Facts a user needs** (gathered):
- **Hardware:** Waveshare ESP32-S3-Touch-LCD-4.3 (https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm): ESP32-S3,
  16 MB flash, 8 MB PSRAM, 800x480 touch LCD (GT911). Two USB-C ports: native USB (the device's USB: vendor
  interface + HID keyboard) and the CH343P UART (flashing, logs). **Power from the native port** (the UART port
  alone can't run Wi-Fi reliably). Flash over the UART port (or hold BOOT, press RESET, flash over native USB).
- **Screen:** 40 px status bar (left/right text, `{clock}`, pager pills), 5x3 grid of 144 px keys (720x432),
  spans/half/quarter keys and groups, up to 32 keys a page, swipe between pages, key ids.
- **Firmware build/flash:** ESP-IDF v6.0; `idf.py --preset release -p COMx flash monitor` from `firmware/`
  (presets `debug`/`release`); clone with `--recursive`. A first flash needs the full `idf.py flash` (partition
  table: app 2 MB, layout 4 MB). The versioning job will add CI-built release binaries (flash with esptool or a
  web flasher) and an npx CLI: write the install sections as "Option A: prebuilt release (coming)" / "Option B:
  build from source", with placeholders the versioning job fills in.
- **USB on the PC:** VID:PID `303a:4020`; Windows needs no driver (WinUSB via MS OS 2.0); Linux needs a udev rule
  (no rule text exists in the repo yet: the guide should give one); macOS untested/unmentioned. WinUSB is exclusive:
  one app at a time (the CLI fails while a host app has it open). Browsers: WebUSB (Chrome/Edge) via `host/browser.js`.
- **Host library** (`host/`, `@toptensoftware/controlblock`, Node >= 24, ESM; `npm install` fetches native `usb`,
  `sharp`, `@napi-rs/canvas`, and button-face from GitHub, so it needs git): `ControlBlock.open({ serial, setTime })`, `info` (grid, limits, mode, layout, Wi-Fi, time),
  `takeControl()`/`releaseControl()`, `reset()`, `setPageCount`, `setKeys`/`removeKeys` (ids, grid-unit rects),
  `setPage(page, { animate })`, `putImage`/`syncImages`/`freeImages`/`queryImages`, `setStatusText({ left, right })`,
  `setBacklight(on)`, Wi-Fi (`setWifi`, `getWifi`, `clearWifi`, `wifiStatus`, `waitWifiConnected`), clock
  (`setTime`, `getTime`, `setClock`), name (`setName`, `getName`), `uploadLayout`/`reconnect`, `close()`. Events:
  `key` ({ id, page, action PRESS/RELEASE/CANCEL }), `pageChanged`, `wake`, `wifiState`, `close`, `error`. Image
  helpers: `keyPixelSize`, `keySlot`, `keyImage(layers, w, h)`, `svgLayer`, `buttonLayer` (now takes a button-face
  definition `{ virtualSize?, layers: [fill | svg | text] }`), `FACE_MARGIN`; key looks are button-face layer
  definitions (built-in `#name` icons, `@name` Material Symbols); the guide should link button-face's README for
  the definition format rather than restate it. Long/double press are
  the host's job (no helpers yet). `examples/demo.js` and `bench.js` (demo.js not yet re-run on hardware since v2).
- **CLI** (`node cli/cli.js`, or `npm link` -> `controlblock`): `bake`, `upload`, `render`, `wifi`, `time`, `name`
  (cli/README.md covers it fully).
- **Layout format:** layout-format.md (Home, activities, pages, keys/rects/ids/groups, look, gestures press/release/
  tap/long/action, auto repeat, op lists and `use`, ops delay/switchActivity/hidKey/httpGet/httpPost/udpPacket/
  sendWol/script, Lua API, devices and hooks, bindings and `pad`, inheritance, validation). Examples:
  `upload-test.json`, `key-shapes.mjs`, `net-test.json`, `activity-test.mjs`, `script-test.mjs`, urem2.
- **Wi-Fi/clock/name/diagnostics:** `wifi set`, `time zone <POSIX TZ>`, `name set`, `http://<name>.local/`.
- **Known limitations:** backlight locked on (switch-on glitch; `setBacklight(false)` leaves it lit with touches
  gated); http only (no https); no double press; macro pad not available yet; bake doesn't check device limits
  (over-limit layout refused at boot: empty Home); a re-bake is needed after blob version changes (empty Home
  until then); the device boots to an empty Home until a layout is uploaded; the clock is lost at power-off without
  Wi-Fi; diagnostics HTTP is unauthenticated; status text is ASCII only, 47 bytes; `web/` front end not started.

**Proposed README.md outline** (root; short, links out):
1. Title + one-paragraph pitch (touch control surface on a cheap ESP32-S3 panel) + a screenshot
   (e.g. one of `urem2/urem2-test-pages/*.png` or a `cli render` mockup; user's choice).
2. Features (bullets: 15-key grid with spans/groups, swipe pages, two modes, USB HID keyboard, HTTP/UDP/WoL, Lua
   scripts, activities and devices, Wi-Fi + NTP clock, WebUSB/Node library, CLI, no driver on Windows).
3. Hardware (board + link, the two USB ports, power note).
4. Getting started: get the firmware (prebuilt release: placeholder for the versioning job; or build from source
   in brief), connect, first run (empty Home).
5. Standalone mode quick start (write a tiny layout, `bake`/`upload`, Wi-Fi) -> link to the standalone guide.
6. Hosted mode quick start (10-line `ControlBlock.open`/`takeControl`/`setKeys` example) -> link to the hosted guide.
7. Repository layout (short table, from CLAUDE.md, user-relevant folders only).
8. Documentation index (guides, CLI, layout reference, protocol and design specs).
9. Status / known limitations (short, link to the guides).
10. Development (tests per package, one line each; link CLAUDE.md is not user-facing, so list the commands).
11. Licence (MIT, link LICENSE and THIRD-PARTY-NOTICES).

**Proposed guides:** a `docs/guide/` folder, kept apart from the specs (the specs stay in `docs/`):
- `docs/guide/getting-started.md` (optional, recommended): hardware, flashing (prebuilt/source), USB/udev, first
  connection, Wi-Fi, clock, name. Shared by both guides so neither repeats it (DRY). If the user wants exactly two
  files, fold it into the standalone guide and link from hosted.
- `docs/guide/standalone-mode.md`: concepts (Home, activities, pages, devices, the op queue); tutorial (first
  layout -> `render` -> `bake` -> `upload`); keys and layout on the grid (spans, groups, ids); the look; gestures and
  auto repeat; ops (with a worked HTTP/UDP/WoL example); activities, devices and hooks; scripts (when to use,
  a short example) ; Wi-Fi/clock in the status bar; take/release by a host; troubleshooting (empty Home after an
  upgrade, errors from the bake, test server, logs at `<name>.local/log`); limitations. Reference tables (every
  property, every op, the full Lua API, validation rules) **link to layout-format.md** rather than copy it.
- `docs/guide/hosted-mode.md`: install the library (Node; native deps), connect (Node, browser WebUSB), the mode
  model (boots standalone, `takeControl`, release on close/unplug/suspend), the grid and key sizes, images (opaque,
  key-sized, `keyImage`/`syncImages`, ids and replacement), keys (`setKeys`, rects, ids, remove), pages and
  `setPage`, events (key, pageChanged, wake), status text and `{clock}`, backlight (locked on), Wi-Fi/clock/name
  calls, pipelining and errors (`ControlBlockError` statuses), uploading a layout from code (`bakeFile`/
  `uploadBlob`), the deaf-pipe/one-app-at-a-time notes, writing a host in another language (link protocol.md).
  API details **link to host/README.md** (or move its API section into the guide and slim host/README to a pointer:
  decide in Plan; don't keep both).
- **Cross-link, don't duplicate:** CLI usage -> `cli/README.md`; layout reference -> `docs/layout-format.md`
  (user level enough as a reference, after tidying its stale "proposed" markers and pad names); protocol bytes ->
  `docs/protocol.md`; Wi-Fi design -> `docs/wifi.md`; diagnostics endpoints -> `docs/diagnostics.md`; backlight ->
  `docs/backlight.md`. CLAUDE.md stays the developer/agent doc and is not linked as user documentation.
- Small fixes worth folding into the plan: remove the duplicated driver lines in `host/README.md`; refresh
  `standalone.md`'s Status header; tidy layout-format.md's stale markers; add a udev rule example.

**Open questions for the user:**
1. Is the repo going public (and when)? That decides how much of section 2 to act on, and PacketEncoder must go
   public with an https URL (or be dropped from the public tree) before then.
2. Add LICENSE files to SimpleLib and atv-daemon (your repos), and fix the bracketed copyright line in binpack?
   (node-buttonImage is no longer used; button-face's licence is already clean.)
3. Keep urem2 (real IPs, a MAC, brand icons) in the public repo as is, or move its real targets to a gitignored
   local file?
4. Guides in `docs/guide/` with a shared getting-started page (three files), or exactly two files?
5. Slim `host/README.md` to a pointer and move its API detail into the hosted guide, or keep host/README as the
   API reference and have the guide link to it?
6. Should this job also tidy the stale spec bits (layout-format.md "proposed" markers and pad control names,
   standalone.md's Status header), or leave them for another job?

### User's answers (2026-11-15)

1. **Going public soon.** PacketEncoder is already public (checked: `gh api` says `public`); its `.gitmodules` URL is
   still SSH, so switch it to https.
2. **No:** don't add licences to SimpleLib/atv-daemon or fix binpack's brackets (leave the user's other repos alone).
3. **urem2 stays as is** (real IPs, the MAC, brand icons).
4. **Three guide files** in `docs/guide/`: getting-started, standalone-mode, hosted-mode.
5. **Yes:** move `host/README.md`'s API detail into the hosted guide; slim host/README to a short pointer.
6. **Yes:** tidy the stale spec bits (layout-format.md markers and pad names, standalone.md Status).

Planning notes:
- Q1 means acting on section 2 where it's cheap and doesn't touch urem2: the test server address
  (`examples/test-common.mjs` `SERVER`, also imported by `urem2/targets.mjs` for the test target) and
  `.vscode/settings.json`. `braddev` in `firmware/test/TestNetRequest.cpp` is a parse fixture: leave it.
- The versioning job (`wip/wip-versioning-deployment.md`, Investigate) owns publishing, `files` whitelists, CI
  releases and the npx CLI. This job only adds `license` fields and writes install sections with clearly marked
  placeholders ("prebuilt release: coming") for that job to fill in.
- Docs can't be tested automatically (Rule 3): step 10 checks every relative link with a scratch script and runs
  the test suites; the user reviews the prose. Steps 3 and 4 change code and adjust tests first.

### Implement notes (steps 1-3, 2026-11-15)

- Step 1: root `LICENSE` is a byte copy of `rle565/LICENSE` (MIT, "Copyright (c) 2026 Topten Software", CRLF like
  the original). `license: MIT` + `author: Topten Software` added to host, cli, tools/test-icons, tools/pad-sim
  package.json (lock files untouched; npm doesn't need them updated for these fields). `tools/test-icons/fonts/
  OFL.txt` is Inter's `LICENSE.txt` verbatim (fetched with `gh api repos/rsms/inter/contents/LICENSE.txt`): it
  carries "Copyright (c) 2016 The Inter Project Authors" and the full OFL 1.1 text. fonts/README.md now links it.
  `host` npm test 294/294, `cli` npm test 31/31.
- Step 2: `THIRD-PARTY-NOTICES.md` at the root, three parts: in the firmware binary (table), in baked layouts and
  host tools (table), npm packages (not redistributed), plus a trademarks paragraph (button-face and urem2 brand
  logos). Checked: `mdns` is always linked (`wifi.cpp`, `REQUIRES
  espressif__mdns`), not only in a diagnostics build. Locked component versions (`firmware/dependencies.lock`):
  esp_lcd_touch 1.2.1, gt911 1.2.1, esp_tinyusb 2.1.1, mdns 1.11.0, tinyusb 0.19.0~2, IDF 6.0.0. The Wi-Fi/PHY
  blobs are Apache-2.0 in object form (README of `esp-idf/components/esp_wifi/lib`). Lua is 5.4.7 (`lua.h`,
  copyright 1994-2024). SimpleLib and atv-daemon are listed as "no licence file upstream; by the same author".
- Step 3: `.gitmodules` PacketEncoder -> `https://github.com/toptensoftware/PacketEncoder.git`, SimpleLib ->
  `https://github.com/toptensoftware/simplelib.git` (the staged node-buttonImage removal kept; `.gitmodules` is now
  `MM`: staged removal + unstaged URL change). `git submodule sync` + `status`: all six at their pinned commits;
  `git ls-remote` on both new URLs returns those commits. Note `git submodule sync` also rewrote the `origin` remote
  inside the local `PacketEncoder/` and `SimpleLib/` checkouts to the https URLs (pushing from them now uses https
  credentials instead of SSH). `.vscode/settings.json` uses `${workspaceFolder}/firmware`.

### Implement notes (step 4, 2026-11-15)

- `examples/test-common.mjs`: `SERVER = process.env.CB_TEST_SERVER || "10.1.1.126"` and a new
  `SERVER_NAME = process.env.CB_TEST_SERVER_NAME || "braddev"` (net-test's "GET 2nd host" and "UDP name" keys need a
  host name for the same PC). `||` rather than `??` so an empty variable falls back too.
- `examples/net-test.mjs` replaces `net-test.json` (deleted; unstaged deletion in the working tree): same keys, with
  small `get`/`post`/`udp` helpers local to the file (test-common's `udp`/`get` take different arguments). Baked
  both with `node cli/cli.js bake`: **byte-identical** blobs (120568 bytes, `cmp`). The `10.255.255.1` unroutable
  address and the WoL MAC stay literal (not the server).
- New `examples/test-common.test.mjs`: sets both variables before a dynamic import (node --test runs each file in its
  own process), checks `SERVER`/`SERVER_NAME`/`HTTP`/`udp`/`get`, and that net-test, activity-test and script-test
  carry the override and neither default. `layouts.test.mjs` bakes `net-test.mjs`.
- Updated `examples/README.md` (table rows, bake example, the address paragraph now documents the two variables with
  a sh and a PowerShell example), `cli/README.md` examples, `test-server.mjs`'s header comment.
- Tests: `node --test "examples/**/*.test.mjs"` 37/37, `node --test "urem2/**/*.test.mjs"` 175/175 (urem2's test
  target still imports `SERVER`; untouched).
- CLAUDE.md (step 11): Repository Layout's `examples/` line names `net-test.json` -> `net-test.mjs`; "the test
  server's address in test-common.mjs" could add "(`CB_TEST_SERVER`, `CB_TEST_SERVER_NAME`)"; the test list could
  mention `examples/test-common.test.mjs` (already covered by the glob).

### Implement notes (step 5, 2026-11-15)

Checked against the code:
- **Look properties:** every property in the table is validated and used by `host/src/bake/look.js` (`LOOK`, `faceDef`,
  `foregroundDef`, `image` override, pressed = 35% overlay by default). "*(proposed)*" dropped from the heading (anchor
  now `#look-properties`; the two links to `#look-properties-proposed`, layout-format.md:133 and standalone.md:117,
  fixed). **Inheritance:** `resolve.js` builds the chain root -> home/activity -> page defaults, `inheritLook` merges
  recursively with `null` clearing, `DEFAULTS_SPEC` has look + `longPressMs` only, groups via `groupLookForKeys`:
  marker dropped. The "Open points" entries for both became "built" / removed.
- **Pad names:** the bake's `bindings` take any non-empty `control`/`event` and only refuse `touch[...]`
  (`resolve.js` `resolveBindings`); `pad` keys must be `key1`..`key32`. urem2 uses `key1`..`key7` (press/release/tap/
  long) and `enc1` with event `turn`; the `turn` param is `delta=<signed detents> interval=<µs>` (no direction in the
  event name: the sign of `delta`), per `wip/wip-macro-pad.md` (decision 2 round 2, plan step 3) and
  `urem2/layout/scripts.mjs`. Encoders are `enc1`..`enc4` (macro pad job). Bindings section and example rewritten
  with those names, plus a note that the pad isn't available on the S3 yet (C3 firmware written, S3 side not built).
- **Not implemented, kept and noted:** the bake doesn't check pad control/event names (planned in the macro pad
  job, step for the bake); said so in layout-format.md's Open points.
- standalone.md: intro line and Status refreshed (keys as rects and scripts now "works on the hardware", per
  CLAUDE.md; Home keys "pressing does nothing" and "`param` not carried through the queue" removed; a "Not yet run
  on the hardware" line: backlight wake gate, pad repeat; "Not built": the pad's S3 side and hosted pad events;
  Next: the pad's S3 phase). Also fixed stale markers further down: trigger table examples (`key1`, `enc1`, `turn`,
  `delta=-3 interval=6500`), encoder "velocity" -> delta and interval, "6-key macro pad and a single encoder" ->
  `key1`..`key32`/`enc1`..`enc4`, serial format "not defined yet" -> decided and built on the C3, the image store
  "rebuilt on release (not built)" -> `LayoutApplyHome`, "Built (upload only; the boot loader is not)", the "Button
  definition -> images" Still-open item marked built, take control/release dropped from "Protocol changes needed",
  and the stale "CLAUDE.md (only with your OK)" Still-open item removed (CLAUDE.md already lists binpack, the bake,
  its tests and the standalone open items).
- **Left alone (behaviour descriptions, out of scope):** layout-format.md says `color` is "shorthand for
  `face: { top: c, bottom: darker(c) }`", but `look.js` uses `top = bottom = c` plus a translucent shade overlay
  (same visual intent; the Open points' "Face shade bands" item describes the shade). layout-format.md says `icon` is
  "an SVG file path, or `#name`": a `@name` Material Symbol works only inside `image` layers (`isFileSrc`), not in
  `icon` (`checkIcon` treats it as a file). standalone.md "Layout storage" still says the screen is blanked
  (backlight off) while uploading; the backlight is locked on now (CLAUDE.md), so it isn't. The test fixtures
  (`host/test/bake/fixtures.js`, `bake.test.js`, `resolve.test.js`, `pack.test.js`, the firmware's
  `sample_layout.json` and tests) still use `macro1`/`volEnc`/`turnLeft` as free-form strings: valid, but
  layout-format.md's Testing section says the doc's example is a test fixture, and the doc example now uses
  `key1`/`enc1`/`turn`. Not changed (would need a re-bake of `sample_layout.bin`); worth a follow-up.

### Implement notes (step 6, 2026-11-15)

- `docs/guide/getting-started.md` sections: What you need; The two USB ports; Getting the firmware (Option A
  prebuilt: a "Coming soon" blockquote for the versioning job; Option B: install IDF v6.0, clone `--recursive`,
  IDF shell + `MSYSTEM` note, `idf.py --preset release -p COM4 flash monitor`, checksum-mismatch tip, BOOT/RESET
  fallback, updating keeps layout and Wi-Fi but a format change means re-bake); Connecting to your PC (Windows,
  Linux udev rule marked untested, macOS untested, browsers, one app at a time); Installing the tools (`npm install`
  in `host/`, `node cli/cli.js`, `npm link`); First boot (empty Home, backlight locked on); Wi-Fi; Clock and time
  zone; Device name; Diagnostics page (unauthenticated note); Next steps.
- Links into `cli/README.md` use GitHub slugs `#wifi-action---serial-s` and `#time-action---serial-s`: step 10's
  link checker should confirm its slug rules match these.
- Verified: VID:PID in `firmware/main/usb.cpp`; presets in `firmware/CMakePresets.json`; `CONFIG_IDF_TARGET="esp32s3"`
  is in `sdkconfig.defaults` (so no `set-target` step); Node >= 24 in host/cli `engines`.

### Implement notes (step 7, 2026-11-15)

- `docs/guide/standalone-mode.md` sections: Concepts; Tutorial (write, render, upload, repeat); Layout files (JSON or
  JS); Placing keys (+ Key ids); The look (+ Icons, Material Symbols); Gestures (+ Auto repeat); Actions (Op lists,
  Keyboard keys, Network actions); Activities and devices; Scripts; The status bar; When a host takes control; A
  complete example; More examples; Troubleshooting (Bake errors, Home is empty, Logs, The test server, Other
  problems); Limitations. Reference detail links to layout-format.md anchors and button-face's README.
- Every example was baked as written: `scratch/guide-extract.mjs` pulls the tutorial JSON, the complete `.mjs`
  example and `scripts/mute.lua` out of the guide into `scratch/`; `node cli/cli.js bake` (tutorial 32756 bytes,
  example 87564 bytes, no warnings), `render` (4 pages) and `scratch/guide-check.mjs` (`bakeChecked` from
  `examples/bake-check.mjs`: the device's key checks) all pass. The bake error messages quoted in the guide are real
  output (`scratch/guide-bad.json`, and the overlap error from forgetting `null` after a 2-wide group).
  `scratch/guide-links.mjs <file>` checks one file's relative links and anchors (GitHub slugs); only
  `./hosted-mode.md` dangles (step 8). Step 10 can reuse or extend it.
- Found while baking: a `@name` Material Symbol in an `image` foreground replaces the generated foreground
  **including the label**, and `pressedFg` must be set too or the pressed picture loses it (look.js `lookImages`:
  the override is per layer). To match the generated look, the guide's definition uses the bake's own values (svg
  `translate: { y: -7 }`, text `bold 11px sans-serif`, `valign: bottom`, `padding: [9, 4]`); button-face's defaults
  (16 px text, padding 3) overlap the icon.
- A two-line label with an icon ("Task\nManager") overlaps the icon at the default font: the tutorial uses one line.
- The first bake that downloaded a Material Symbol wrote the blob but exited 127 with a libuv assertion
  (`!(handle->flags & UV_HANDLE_CLOSING)`, `src\win\async.c`, Node 24.14 on Windows); later bakes (symbol cached)
  exit 0. Not reproduced; likely a fetch handle still closing at exit. Worth a look in the CLI (e.g. not exiting
  with `process.exit` while a download socket closes) if it recurs.
- `hidKey` has no media/consumer keys (`hid.js`): listed as a limitation.

### Implement notes (step 8, 2026-11-15)

- `docs/guide/hosted-mode.md` sections: Installing (by path; entries table); A first app (`hello-host.mjs`, based
  on demo.js: rounded face via `svgLayer` + button-face foreground, `keyImage`, `syncImages`, a 2U key, two pages,
  status text, key/page events, release on Ctrl+C); Connecting (Node, browser WebUSB with `requestDevice`, one app
  at a time); Modes (take/release, what releases, which calls need control); The grid and key sizes (size table,
  every `info` field); Key images (helpers table, IDs, replace in place, freeing, memory); Keys (`setKeys` fields,
  all-or-nothing, groups via `clipLayer`); Pages; Events (all seven, key actions, a long-press timer snippet);
  The status bar; Backlight (locked on); Wi-Fi, clock and name; Requests and errors (pipelining, statuses,
  client-side throws, timeouts, disconnects); Uploading a layout from code (`bakeFile`/`uploadBlob`, `bake` with
  `createNodeHost`/`createRenderer`, `uploadLayout` + `reconnect`); Other languages (protocol.md, HEARTBEAT and
  ZLP anchors, rle565); Limitations.
- **Verified by running:** `scratch/guide-hosted-check.mjs` extracts the guide's first app plus its Keys and clock
  snippets and runs them against `host/test/FakeDevice.js` (which checks image sizes and opacity): 13 images, 2
  pages, OK. `scratch/readme-host-check.mjs` does the same for host/README's example. Links:
  `scratch/guide-links.mjs` all ok for hosted-mode.md, host/README.md, standalone-mode.md, getting-started.md.
  `host` `npm test` 294/294.
- `host/README.md` is now: what it is (device API, images, bake), a pointer to the guide and protocol.md, install
  + test, the entries table, a minimal runnable example (one `fill` + `#play` key), demo/bench, and a Development
  section (source layout table, the binpack/rle565/button-face dependency notes). Driver lines removed (they're in
  getting-started). No repo file linked into the removed host/README anchors (grep; only wip docs mention it).
- **host/README claims the code contradicts** (all dropped or corrected in the guide):
  - "Unplugging, **closing the host** or a USB suspend does the same by itself": closing the connection does not
    release control; the device only sees unmount and suspend (protocol.md, Device state lifetime;
    `ControlBlock.releaseControl`'s comment says so too). The guide says so explicitly. The same slip was in
    standalone-mode.md ("releases control, closes, or ..."): fixed there in this step.
  - `uploadLayout`: "The device's screen goes dark while it writes": no longer true with the backlight locked on
    (`host_link.cpp` calls `display_enable_backlight(false)`, which does nothing in a normal build). The guide
    doesn't say it. (`ControlBlock.uploadLayout`'s comment "The device blanks its screen" is stale the same way;
    standalone.md's Layout storage too, noted in step 5.)
  - The old Use example referenced undefined `faceSvg`, `pressedFaceSvg`, `wide2U`: not runnable.
- Not previously documented, now in the guide: the `heartbeat`, `close` and `error` events, `cb.off`,
  `clipLayer`, `imageId`/`encodeImage`, the `timeoutMs` constructor option, `reconnect({ opener })` for browsers,
  `uploadBlob` opening its own connection (close yours first), `info.maxPages` (8), and that `info.backlight`
  reports the requested state while the light is locked on (`host_link.cpp` `s_backlight`).

### Implement notes (step 9, 2026-11-15)

- Root `README.md` sections: pitch + picture; Features; Hardware; Getting started (clone `--recursive`, one
  paragraph -> getting-started); Standalone mode in brief (3-key JSON, `render`, `upload` -> guide); Hosted mode in
  brief (5-line `open`/`takeControl`/`setStatusText`/`key` snippet with no images, so it doesn't repeat host/README's
  example -> the guide's first app); Repository layout; Documentation (guides, reference, design specs for
  contributors; `standalone-ops-plan.md` and `backlight-waveshare-support.md` omitted); Status and known
  limitations; Development (test command table); Licence.
- Picture: `docs/guide/images/key-shapes.png`, from `node cli/cli.js render examples/key-shapes.mjs --no-grid`
  (800x480, 29 KB). urem2's Home was also rendered but is mostly brand logos with an empty column; key-shapes
  shows the key shapes better. The user may swap in a photo of the device.
- The quick-start layout was baked as written (`scratch/readme-quick.json`, 8772 bytes, exit 0).
- `node scratch/guide-links.mjs README.md`: all links ok.

### Implement notes (step 10, 2026-11-15)

- **Links:** `scratch/guide-links.mjs` became `scratch/check-md-links.mjs <file.md>...` (the old file removed).
  GitHub slug rules: link text kept, backticks/asterisks dropped, lowercased, everything but letters, digits, `_`,
  `-` and spaces stripped, spaces to hyphens, repeated headings `-1`, `-2`; also `<a id/name>` anchors; fenced code
  and inline code skipped. A built-in self test checks `` `wifi <action> [--serial S]` `` -> `wifi-action---serial-s`
  (matches cli/README's headings). Run on README.md, THIRD-PARTY-NOTICES.md, docs/guide/*.md, host/README.md,
  cli/README.md, examples/README.md, tools/test-icons/fonts/README.md, docs/layout-format.md, docs/standalone.md:
  **all links ok** (checked it catches a bad file and a bad anchor). Nothing to fix.
- **Stale statements fixed:** `ControlBlock.uploadLayout`'s comment no longer says the device blanks its screen.
  standalone.md (Layout storage): the firmware turns the backlight off while uploading (`host_link.cpp`
  `cf_busy`), but while the backlight is locked on that call does nothing, so the screen stays lit. layout-format.md
  `color`: now describes the bake (plain `color` = flat `c` plus a black 0 -> 30% top-to-bottom shade; `face: { top,
  bottom }` = exact gradient, no shade; a missing end falls back to `c`). Also layout-format.md's `image` row:
  "A node-buttonImage settings object ... is an error" -> "An object without `layers` (such as a settings object from
  the old renderer, `svgFile`, `svgCode`, ...) is an error" (the bake refuses any object without `layers`).
- **Stale-name grep** (node-buttonImage, `net-test.json`, `C:/Users`, `C:\Users`, `macro1`, `volEnc`, `turnLeft`,
  "(proposed)"): only the layout-format.md `image` row above (fixed) and standalone.md:121's history line ("it
  replaced node-buttonImage, which used `sharp`"), kept as history.
- **Tests:** host 294/294, cli 31/31, binpack 81/81, examples 37/37, urem2 175/175, atvdaemon 9 OK, firmware `mk run`
  789 passed (mk found MSVC from this shell).
- Still open (from step 5, not done here): layout-format.md says `icon` takes "an SVG file path, or `#name`" (correct:
  `@name` works only in `image` layers); the test fixtures' `macro1`/`volEnc`/`turnLeft` names (needs a re-bake of
  `sample_layout.bin`).

### CLI exit crash (fixed 2026-11-15, outside the plan, at the user's request)

The "exit 127 after a Material Symbol download" seen in step 7 is a Node 24 / Windows bug: `process.exit()` while
fetch's (undici) keep-alive HTTPS sockets are open trips `Assertion failed: !(handle->flags & UV_HANDLE_CLOSING),
src\win\async.c` (`scratch/fetch-exit-repro.mjs`: 3/3 crash with `process.exit`, 0/2 with `process.exitCode`; a local
plain-HTTP fetch doesn't reproduce it, so no offline test can). Only the first bake of an uncached icon hit it (the
blob was written; only the exit code was wrong). Fix: `cli/cli.js` sets `process.exitCode` instead of calling
`process.exit()`; the spawned bake test in `cli/test/cli.test.js` now has a timeout so a hang fails. Verified: the
uncached-icon bake exits 0; `wifi status` with no device exits 1; cli 31/31. **Still to check on the device (user):**
the commands that open the device (`upload`, `wifi`, `time`, `name`) still exit by themselves (an open USB handle
would now hang the CLI instead of being cut off).

## Flagged assumptions
- Step 9: the README says "the author uses urem2 as a home theatre remote", calls urem2's equipment "TV, Apple TV,
  receiver, PC", links mk as `https://github.com/toptensoftware/mk.js` (from CLAUDE.md's `npm install -g
  toptensoftware/mk.js`), and describes `pad/` as "a macro pad (in progress)". The hosted snippet wasn't run against
  the FakeDevice (it uses only `open`, `takeControl`, `setStatusText` and the `key` event, all as in the guide).
- Step 8: the hosted guide says `npm install <path to the repo>/host` adds the library to another project
  (untested; npm's `file:` handling), that WebUSB pages need `https://` or `http://localhost` and a click for
  `requestDevice` (standard WebUSB rules, not tried with this device), that a browser app can pre-render images in
  Node and upload the bytes, and that the press window is "about 150 ms" (as the standalone guide says). The
  half-height Vol keys in the Keys snippet use an icon and a label at 72 px high (renders, but may look cramped).
- Step 7: the guide says the UART log is at 115200 baud (ESP-IDF default; `sdkconfig.defaults` doesn't set it), and
  that `turnOn` lists can hold a `delay` "while touches wait in the queue". Mentions the status bar icons (Wi-Fi,
  hourglass, error mark) from standalone.md's Status bar section without checking them on the hardware. The
  complete example's addresses, MAC, ports and HTTP paths are placeholders, not a real device's API.
- Step 6: the getting-started guide says "always use `idf.py ... flash`" (no `app-flash` mention), names the clone
  URL `https://github.com/toptensoftware/ControlBlock.git`, says the S3 is 2.4 GHz only (chip fact, not in the repo),
  and gives the udev rule as `MODE="0660", TAG+="uaccess"` (untested). The empty Home is described as "a black
  screen with the status bar across the top" (not checked on the hardware). Linux serial port names
  (`/dev/ttyACM0`/`/dev/ttyUSB0`) are guesses for the CH343P.
- Step 5: wrote that the macro pad's C3 firmware is "written" (CLAUDE.md: C3 phase done and committed; `pad/README.md`
  says it hasn't run on a board) and that pad bindings "bake and upload but never fire" until the S3 side exists.
- Step 2: THIRD-PARTY-NOTICES.md says anyone redistributing a firmware binary should pass the file on with it; the
  Material Symbols, button-face, binpack and atv-daemon rows sit under "baked layouts and host tools". (A third-party
  product trademark line was added, then removed at the user's request: no mention of it anywhere in the docs.)
- Step 3: `git submodule sync` changed the local submodule checkouts' `origin` to https (see Notes); if the user
  pushes PacketEncoder/SimpleLib over SSH, they can set `git -C PacketEncoder remote set-url origin git@...` back
  locally without affecting the superproject.
- Test server address: `examples/test-common.mjs` reads `process.env.CB_TEST_SERVER` and falls back to the current
  `10.1.1.126`, so the user's setup keeps working (a private address; urem2 already exposes the same network).
  `net-test.json` becomes `net-test.mjs` importing `SERVER`, so the address lives in one place.
- Step 4: added `CB_TEST_SERVER_NAME` (default `braddev`) alongside `CB_TEST_SERVER`, since net-test needs a
  resolvable host name for the same PC; the defaults stay the author's test PC so the current setup keeps working.
- README picture: a `node cli/cli.js render` mockup of an example layout's page (e.g. `examples/key-shapes.mjs` or
  urem2 Home), committed under `docs/guide/images/`. The user can swap in a photo of the device later.
- Linux udev rule text is written from the VID:PID and standard practice, marked untested; Windows is the tested
  host OS; macOS is not mentioned beyond "untested".
- Inter's OFL: add `tools/test-icons/fonts/OFL.txt` (the standard OFL 1.1 text with Inter's copyright line) rather
  than only a link, since the font is embedded in every firmware binary.
- The SimpleLib submodule URL is updated to the moved repo (`toptensoftware/simplelib`) alongside the PacketEncoder
  change (same file, same purpose: a clean public clone).
- Assumed the firmware binaries will be redistributed (the versioning job's CI release), so a third-party notices
  file is needed now rather than later.
- button-face's README now states the icon provenance (many derived from Material Symbols, Apache-2.0; steam,
  playstation, apple-tv are brand logos), so that is no longer a guess. urem2's netflix/prime/youtube logos still
  have unknown origin.
- Assumed Linux/macOS hosts are untested; the docs should say Windows is the tested host OS.
- Assumed the macro pad stays out of the user guides except as "planned" (S3 side not built).

## Plan
- [x] 1. **LICENSE and licence fields.** Root `LICENSE`: standard MIT, "Copyright (c) 2026 Topten Software" (same
  form as rle565). Add `"license": "MIT"` and `"author": "Topten Software"` to `host/`, `cli/`, `tools/test-icons/`
  and `tools/pad-sim/` `package.json` (no `npm install` needed; check `host` `npm test` and `cli` `npm test` still
  pass). Add `tools/test-icons/fonts/OFL.txt` (SIL OFL 1.1 with Inter's copyright line, from the Inter repo) and
  point `tools/test-icons/fonts/README.md` at it.
- [x] 2. **THIRD-PARTY-NOTICES.md** at the root: one entry per component (name, use, licence, copyright, link):
  ESP-IDF (and its bundled FreeRTOS, lwIP, mbedTLS, newlib, Espressif Wi-Fi/PHY blobs), TinyUSB, esp_tinyusb,
  esp_lcd_touch + gt911, mdns, Lua 5.4.7, the Espressif CC0 driver code, Inter (OFL, link OFL.txt), Material
  Symbols (via button-face icons, in baked layouts), button-face, rle565, binpack, PacketEncoder, SimpleLib (ours,
  noted as unlicensed upstream: say "by the same author" without inventing a licence), npm dependencies installed
  by npm (usb, sharp + LGPL libvips binaries, @napi-rs/canvas, wasmoon, fontkit, serialport; not redistributed).
  Trademarks line: brand logos in button-face and urem2 belong to their owners. Say which parts ship in the
  firmware binary.
- [x] 3. **Public clone fixes.** `.gitmodules`: PacketEncoder to `https://github.com/toptensoftware/PacketEncoder.git`,
  SimpleLib to `https://github.com/toptensoftware/simplelib.git`; `git submodule sync`; check `git submodule status`.
  `.vscode/settings.json`: `"cmake.sourceDirectory": "${workspaceFolder}/firmware"`.
- [x] 4. **Test server address in one place.** Tests first: update `examples/layouts.test.mjs` (and any test naming
  `net-test.json`) for `net-test.mjs`, and add a check that `SERVER` honours `CB_TEST_SERVER`. Then
  `examples/test-common.mjs` reads `process.env.CB_TEST_SERVER ?? "10.1.1.126"`; convert `examples/net-test.json`
  to `examples/net-test.mjs` (same layout, every address from `SERVER`/`HTTP`; check it bakes to the same blob as
  the old JSON before deleting it). Update `examples/README.md`, `cli/README.md:124`, the comment in
  `examples/test-server/test-server.mjs`. Run `node --test "examples/**/*.test.mjs"` and
  `node --test "urem2/**/*.test.mjs"`.
- [x] 5. **Tidy the specs.** `docs/layout-format.md`: drop "*(proposed)*" from Look properties and Inheritance
  where the bake implements them (check `host/src/bake/` for each); replace `macro1..6`/`volEnc`/`turnLeft`/
  `turnRight` with the names the bake and urem2 use (`key1..key32`, `enc1`, event `turn`; verify against
  `host/src/bake/schema.js`/resolve and `urem2/layout`), saying the macro pad isn't available on the S3 yet.
  `docs/standalone.md`: refresh the Status header (keys as rects, scripts, etc. all work on the hardware, per
  CLAUDE.md). Don't change behaviour descriptions beyond that.
- [x] 6. **`docs/guide/getting-started.md`:** hardware (board link, the two USB ports, power from native USB);
  getting the firmware (Option A prebuilt release: placeholder for the versioning job; Option B build from source:
  clone `--recursive`, ESP-IDF v6.0, `idf.py --preset release -p COMx flash`, first flash is a full flash);
  connecting (Windows no driver; Linux udev rule, untested; one app at a time); first boot (empty Home); Wi-Fi,
  clock/time zone and name via the CLI (link `cli/README.md`); diagnostics page (link `docs/diagnostics.md`, note
  it's unauthenticated on the LAN); Node >= 24 and git for the tools; then "next: standalone or hosted".
- [x] 7. **`docs/guide/standalone-mode.md`:** concepts (Home, activities, pages, devices, op queue); tutorial: a
  small layout -> `render` -> `bake` -> `upload`; keys on the grid (spans, groups, ids); look (link button-face's
  README for definitions); gestures and auto repeat; ops with a worked HTTP/UDP/WoL example; activities, devices
  and hooks; scripts (when and a short example); status bar text and clock; a host taking control; troubleshooting
  (empty Home after a firmware upgrade needs a re-bake, bake errors, the test server and `CB_TEST_SERVER`, logs);
  limitations. Reference tables link to `docs/layout-format.md`; examples link to `examples/` and urem2.
- [x] 8. **`docs/guide/hosted-mode.md` and slim `host/README.md`.** Move host/README's API detail into the guide
  (install, connect from Node and the browser, modes and `takeControl`/release on close/unplug/suspend, grid and key
  sizes, images (opaque, key-sized, `keyImage`/`syncImages`, ids), `setKeys`/`removeKeys`, pages, events, status
  text, backlight (locked on), Wi-Fi/clock/name, errors, uploading a layout from code, one app at a time, other
  languages -> `docs/protocol.md`), checking every call against `host/src/ControlBlock.js`. host/README becomes a
  short pointer (what it is, install, a minimal example, link to the guide), with the duplicated driver lines gone.
  Check nothing else links to removed host/README anchors.
- [x] 9. **Root `README.md`** per the outline in Notes (pitch + picture (render it, see Flagged assumptions),
  features, hardware, getting started -> guide, standalone and hosted quick starts -> guides, repository layout,
  documentation index (guides, CLI, layout reference, protocol and design specs), status/known limitations,
  development (test commands per package), licence -> LICENSE and THIRD-PARTY-NOTICES.md).
- [x] 10. **Check.** A scratch script (`scratch/check-md-links.mjs`) that resolves every relative link and `#anchor`
  in README.md, LICENSE-adjacent docs, `docs/guide/*`, `host/README.md`, `cli/README.md`, `examples/README.md` and
  the edited specs; fix what it finds. Run all test suites (`host`, `cli`, examples, urem2, binpack, firmware
  `mk run`). Grep the new docs for stale names (node-buttonImage, `net-test.json`, absolute paths).
- [x] 11. (Done 2026-11-15: the user approved all six CLAUDE.md changes: repository layout, host/README pointer,
  SimpleLib/PacketEncoder URLs, `mdns`, the CLI exit rule, and a "user docs follow the code" convention. Review
  outcomes: README picture and wording kept; getting-started's empty Home reworded to "mostly black (no keys, no
  clock until Wi-Fi is set up)"; every mention of the other key pad product removed from the README, guides,
  notices, urem2/README, tools/test-icons/gen.js and CLAUDE.md (complete-*.md left alone). Follow-ups, not in this
  job: the old pad names in the bake fixtures and `firmware/test/data/sample_layout.json` (re-bake needed); the user
  checks on the device that `upload`/`wifi`/`time`/`name` still exit by themselves after the CLI exit change.)
  **Propose CLAUDE.md updates (don't apply until the user confirms, Rule 4):** repository layout (LICENSE,
  THIRD-PARTY-NOTICES.md, `docs/guide/`, `net-test.mjs`, `CB_TEST_SERVER`), the SimpleLib URL note (now fixed),
  `mdns` in the component list, the host/README change. Then set Status: Complete and rename to
  `wip/complete-docs-licensing.md` once the user is happy.
