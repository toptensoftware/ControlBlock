# ControlBlock host library

Talks to a ControlBlock over its USB vendor interface (mode 1, [`docs/protocol.md`](../docs/protocol.md)).

```sh
npm install
npm test            # library tests, no device needed
npm run demo        # on a device: pages, toggles, an in-place clock, a freed image
node demo/bench.js  # latency, throughput and upload speed
```

- **Windows:** no driver to install, because the device asks for WinUSB itself (MS OS 2.0 descriptors).
- **Linux:** needs a udev rule for `303a:4020`.

## Use

```js
import { ControlBlock, keyImage } from "./index.js";

const cb = await ControlBlock.open();           // first device, or { serial }
const size = cb.info.keyWidth;                  // 128: every image is size x size

const face = await keyImage(faceRgba, size, size, size);    // { id: CRC-32, data: rle565 }
const icon = await keyImage(iconRgba, 80, 80, size);        // centred, transparent padding
await cb.syncImages([face, icon]);              // uploads only what the device lacks

await cb.setPageCount(2);
await cb.setKeys([{ page: 0, key: 0, bg: face.id, fg: icon.id, pressedBg: face.id, pressedFg: icon.id }]);

cb.on("pageChanged", e => console.log("page", e.page));
cb.on("key", e => console.log("key", e.page, e.key, e.action));    // action: KeyAction.PRESS / RELEASE / CANCEL
```

- **Requests are pipelined:** every method returns a promise for its own response. A status other than OK throws a `ControlBlockError`.
- **`putImage(id, data)`:**
  - It splits the image into chunks the device can take.
  - Uploading to an ID the device already has replaces the image everywhere it's used.
  - Use fixed IDs for images you replace, and content IDs (`keyImage`) for the rest.
- **`setStatusText({ left, right })`:**
  - It sets the status bar's texts. Only the fields given change, and `""` clears one.
  - Each is at most 47 bytes of UTF-8 (the font is ASCII only for now). A longer or invalid text throws before anything is sent.
  - The device has no clock: send the time yourself, e.g. `setStatusText({ right: "12:35" })`.
- **`uploadConfig(blob, { force, onProgress })`:**
  - It saves a standalone config (the BPAK blob from `tools/bake`) in the device's flash ([`docs/protocol.md`](../docs/protocol.md#configuration-upload)).
  - The device's screen goes dark while it writes, and the device **restarts** when it's done: the connection closes by itself (no `error` event). `reconnect()` waits for the same device (by serial) to come back and resolves to a new `ControlBlock`; its `info.configSize` and `info.configCrc` show what the device kept.
  - It resolves `{ uploaded: false }` and sends nothing if the device already has exactly this blob (`force: true` sends it anyway).
  - A failure (`ControlBlockError`: `NO_SPACE`, `BAD_CRC`, `IO_ERROR`…) leaves the device with no config until the next good upload.
  - `crc32(bytes)` (zlib's) is exported, as are `info.configCapacity`, `configSize` and `configCrc`.
- **Wi-Fi** ([`docs/wifi.md`](../docs/wifi.md)): works in any mode, and `reset()` leaves it alone.
  - `setWifi(ssid, password)` stores a network and connects in the background; `clearWifi()` forgets it and turns Wi-Fi off. A missing password is an open network. A bad length or a NUL throws before anything is sent (ssid 32 bytes, password 8 to 63).
  - `getWifi()` returns `{ ssid, password, ssidBytes }` (the password in clear text). `info.wifiCredentials` says whether any are stored.
  - `wifiStatus()` returns `{ state, stateName, reason, rssi, ip, mac }` (`ip` is null unless connected), and a `"wifiState"` event ({ state, stateName, lost, ip }) fires when it changes.
  - `node demo/wifi.js set <ssid> [password] | get | clear | status | wait [seconds]`
- **`freeImages(ids)`:** the ID goes at once, and the memory once no key uses it.
- **Browsers:** `ControlBlock` and `src/protocol.js` work on any WebUSB `USBDevice`, so you can pass `navigator.usb`'s. `src/images.js` is Node only (it uses sharp and the rle565 encoder).
