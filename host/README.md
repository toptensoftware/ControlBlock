# ControlBlock host library

One library for everything a host does with a ControlBlock:

- **Device API:** talks to the device over its USB vendor interface (mode 1, [`docs/protocol.md`](../docs/protocol.md)).
- **Layout bake:** turns a standalone layout ([`docs/layout-format.md`](../docs/layout-format.md)) into the blob the device loads, and uploads it.

The command line ([`cli/`](../cli/README.md)) and the scripts in [`examples/`](../examples/README.md) are thin users of it.

```sh
npm install
npm test                    # library and bake tests, no device needed
node ../examples/demo.js    # on a device: pages, a 2U key, half-height keys, toggles, an in-place clock, a freed image
node ../examples/bench.js   # latency, throughput and upload speed
```

- **Windows:** no driver to install, because the device asks for WinUSB itself (MS OS 2.0 descriptors).
- **Linux:** needs a udev rule for `303a:4020`.

## Entry points

- **`index.js`** (Node, the package's default): everything below.
- **`browser.js`** (the package's `browser` entry): the device API only (`ControlBlock`, `ControlBlockError`, `Emitter`, `src/protocol.js`: constants, encoders, `crc32`). It imports nothing Node-only, so it works on any WebUSB `USBDevice` (`navigator.usb`'s) and in Node.
- **Node only:** the key image helpers (`src/images.js`: sharp and the rle565 encoder) and the bake (`src/bake/`: it uses Node's `Buffer`, the file system and node-buttonImage). A browser bake needs a `Buffer` shim and a browser renderer first.

## Layout

| Path | What |
|---|---|
| `src/ControlBlock.js`, `src/protocol.js`, `src/Emitter.js` | the device API |
| `src/images.js` | `imageId`, `encodeImage`, `keyImage`, `composeOverBlack`, `centreImage`, `svgLayer`, `buttonLayer`, `renderButtonImage`, `FACE_MARGIN` |
| `src/bake/` | the bake: `resolve.js` (stage 1), `render.js` (stage 2), `pack.js`/`schema.js` (stage 3, BPAK through binpack), `bake.js` (all three), `files.js` (`loadLayoutFile`, `bakeFile`), `upload.js` (`uploadBlob`), `headers.js` (the firmware headers `tools/codegen` writes) |
| `test/` | device API tests against `test/FakeDevice.js` (a fake WebUSB device); `test/bake/` the bake tests |

binpack is imported by relative path (`../../../binpack/`), not installed, so edits to it apply at once. The other `file:` dependencies (node-buttonImage, the rle565 encoder) are copied in by `npm install` (`install-links=true`): after changing one, bump its version, delete its folder under `node_modules` and `npm install` again.

- **Windows:** no driver to install, because the device asks for WinUSB itself (MS OS 2.0 descriptors).
- **Linux:** needs a udev rule for `303a:4020`.

## Use

```js
import { ControlBlock, keyImage, keyPixelSize, keySlot, svgLayer, buttonLayer, FACE_MARGIN } from "./index.js";

const cb = await ControlBlock.open();           // first device, or { serial }
await cb.takeControl();                         // the device boots standalone: take the screen first

// One opaque image per key state, exactly the key's size: a 1U key is 144x144 (info.keyWidth),
// a 2U key 288x144. The usual look: a 128 face, its icon and label, centred over black.
const { width, height } = keyPixelSize({ w: 1, h: 1 }, cb.info);
const fw = width - 2 * FACE_MARGIN, fh = height - 2 * FACE_MARGIN;
const icon = await buttonLayer({ backColor: "transparent", svgFile: "#play", text: "Play", textColor: "white" }, fw, fh);
const normal  = await keyImage([await svgLayer(faceSvg, fw, fh), icon], width, height);     // { id: CRC-32, data: rle565 }
const pressed = await keyImage([await svgLayer(pressedFaceSvg, fw, fh), icon], width, height);
await cb.syncImages([normal, pressed]);         // uploads only what the device lacks

await cb.setPageCount(2);
await cb.setKeys([
    { id: 1, page: 0, ...keySlot(0), normal, pressed },        // slot 0 of the 5x3 grid
    { id: 2, page: 0, x: 1, y: 0, w: 2, normal: wide2U },      // a 2U key (images 288x144)
]);

cb.on("pageChanged", e => console.log("page", e.page));
cb.on("key", e => console.log("key", e.id, e.page, e.action));     // action: KeyAction.PRESS / RELEASE / CANCEL
```

- **Keys** (protocol v2, [`docs/protocol.md`](../docs/protocol.md#0x21-set_keys)):
  - `setKeys(keys)` adds or replaces keys by **id** (1..65535, unique across all pages; KEY events report it): `{ id, page, x, y, w = 1, h = 1, normal, pressed, disabled }`. `x`/`y`/`w`/`h` are in grid units (1U = one 144 px cell) in 0.25 steps, so a key can be 1.5U wide or half a unit tall; `keySlot(i)` is plain grid slot `i`. `{ id, remove: true }` (or `removeKeys(ids)`) removes a key. Keys not named stay. A key stays on its page: remove it before defining it on another.
  - `normal`/`pressed` are image IDs or `keyImage` results. Each must be the key's size (`keyPixelSize(key, info)`), fully opaque; `pressed` 0 = no press feedback.
  - The device checks the whole message and applies it all or nothing: overlaps, keys outside the grid, more than `info.maxKeys` (32) on a page or a clashing id are `BAD_ARG`, a missing image `NOT_FOUND`, an image of the wrong size `BAD_IMAGE`. A value that can't be encoded (id 0, x 0.3) throws before anything is sent.
  - `info` has the grid: `keyWidth`/`keyHeight` (1U), `quarter` (0.25U in px), `gridX`/`gridY`/`gridWidth`/`gridHeight` (screen px), `maxImageWidth`/`maxImageHeight`, `maxKeys`, `iconWidth` (the icon area in a 1U key).
- **Images** (`src/images.js`, Node only): a layer is `{ rgba, width, height, x, y }` (straight-alpha RGBA; centred when `x`/`y` are missing). `svgLayer(svg, w, h)` and `buttonLayer(def, w, h)` (node-buttonImage) make them; `keyImage(layers, width, height)` composes them over black into one opaque image and encodes it: `{ id, data, width, height }`. `composeOverBlack` is the same without the encode. The device refuses an image that isn't fully opaque (`BAD_IMAGE`).

- **Modes** ([`docs/standalone.md`](../docs/standalone.md)):
  - The device boots **standalone**, showing its own Home. In that mode it refuses every request that edits the screen (`reset`, `putImage`, `freeImages`, `setPageCount`, `setKeys`, `setPage`, `setStatusText`) with a `NOT_HOSTED` error, so call `takeControl()` first.
  - `takeControl()` clears the keys and images like `reset()`, and starts the `key` and `pageChanged` events. `info.hosted` says which mode the device is in.
  - `releaseControl()` gives the screen back: the device rebuilds its Home and turns the backlight on. Unplugging, closing the host or a USB suspend does the same by itself.
  - `ping`, `getInfo`, `queryImages`, `setBacklight`, `uploadLayout`, the Wi-Fi calls and the clock calls work in both modes.
- **Requests are pipelined:** every method returns a promise for its own response. A status other than OK throws a `ControlBlockError`.
- **`putImage(id, data)`:**
  - It splits the image into chunks the device can take.
  - Uploading to an ID the device already has replaces the image everywhere it's used.
  - Use fixed IDs for images you replace, and content IDs (`keyImage`) for the rest.
  - A replacement of another size is accepted, but its keys show black until they are set again with images of their size.
- **`setStatusText({ left, right })`:**
  - It sets the status bar's texts. Only the fields given change, and `""` clears one.
  - Each is at most 47 bytes of UTF-8 (the font is ASCII only for now). A longer or invalid text throws before anything is sent.
  - `{clock}` in either text shows the device's clock (`2:05`, blank until the clock is set; see Clock below), e.g. `setStatusText({ right: "{clock}" })`.
- **`uploadLayout(blob, { force, onProgress })`:**
  - It saves a standalone layout (the BPAK blob from the bake, below) in the device's flash ([`docs/protocol.md`](../docs/protocol.md#layout-upload)).
  - The device's screen goes dark while it writes, and the device **restarts** when it's done: the connection closes by itself (no `error` event). `reconnect()` waits for the same device (by serial) to come back and resolves to a new `ControlBlock`; its `info.layoutSize` and `info.layoutCrc` show what the device kept.
  - It resolves `{ uploaded: false }` and sends nothing if the device already has exactly this blob (`force: true` sends it anyway).
  - A failure (`ControlBlockError`: `NO_SPACE`, `BAD_CRC`, `IO_ERROR`…) leaves the device with no layout until the next good upload.
  - `crc32(bytes)` (zlib's) is exported, as are `info.layoutCapacity`, `layoutSize` and `layoutCrc`.
- **Wi-Fi** ([`docs/wifi.md`](../docs/wifi.md)): works in any mode, and `reset()` leaves it alone.
  - `setWifi(ssid, password)` stores a network and connects in the background; `clearWifi()` forgets it and turns Wi-Fi off. A missing password is an open network. A bad length or a NUL throws before anything is sent (ssid 32 bytes, password 8 to 63).
  - `getWifi()` returns `{ ssid, password, ssidBytes }` (the password in clear text). `info.wifiCredentials` says whether any are stored.
  - `wifiStatus()` returns `{ state, stateName, reason, rssi, ip, mac }` (`ip` is null unless connected), and a `"wifiState"` event ({ state, stateName, lost, ip }) fires when it changes.
  - `waitWifiConnected({ timeoutMs, intervalMs, onStatus })` polls `wifiStatus()` until connected (`true`), or no network is stored or time runs out (`false`); `onStatus` gets each new status.
  - From the command line: `node cli/cli.js wifi set <ssid> [password] | get | clear | status | wait [seconds]` ([`cli/README.md`](../cli/README.md)).
- **Clock** ([`docs/protocol.md`](../docs/protocol.md#clock)): works in any mode. NTP sets the clock while Wi-Fi is up; without Wi-Fi a host must set it after each power-up (the board has no battery-backed clock).
  - `setTime(time = new Date())` sets the device's clock (UTC; a `Date` or ms). A time before 2025 throws before anything is sent. `info.timeSet` says whether the device knows the time.
  - `ControlBlock.open({ setTime: true })` / `connect({ setTime: true })` also sets it from this machine's clock on connect (off by default).
  - `getTime()` returns `{ known, source, sourceName, time, timeZone, ntpServer }` (`time` is a `Date`, null while not known; `source` is `ClockSource`: `NONE`, `HOST` or `NTP`).
  - `setClock({ timeZone, ntpServer })` stores either or both: a POSIX TZ string (e.g. `AEST-10AEDT,M10.1.0,M4.1.0/3`) and the NTP server; `""` puts one back to its default (`UTC0`, `pool.ntp.org`). An invalid value throws before anything is sent.
  - From the command line: `node cli/cli.js time [set] | get | zone <posix>|--default | server <host>|--default` ([`cli/README.md`](../cli/README.md)).
- **Device name** (`docs/protocol.md`, Device name; any mode): `setName(name)` stores the mDNS/DHCP name (`a-z 0-9 -`, up to 32; capitals lowercased; `""` = the default `controlblock-xxxxxx`), `getName()` returns `{ name, isDefault }`. An invalid name throws before anything is sent.
- **`freeImages(ids)`:** the ID goes at once, and the memory once no key uses it.

## Bake

```js
import { bakeFile, uploadBlob, formatEntries } from "./index.js";

const { blob, warnings } = await bakeFile("examples/upload-test.json");   // throws a BakeError listing every problem
if (warnings.length) console.warn(formatEntries(warnings));
await uploadBlob(blob, { log: console.log });   // send, wait for the restart, check what the device kept
```

- `loadLayoutFile(file)` reads a `.json` layout, or the default export of a `.js`/`.mjs` module. `bakeFile(file)` loads and bakes it, with asset and script paths relative to the layout's folder, and resolves `{ blob, tree, warnings }`.
- `bake(layout, host, render)` is the bake itself, with file access (`createNodeHost(dir)`) and the key renderer (`createRenderer()`) passed in.
- `uploadBlob(blob, { serial, force, log })` is the whole upload: `uploadLayout()`, `reconnect()`, then a check of `layoutSize`/`layoutCrc`.
