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
cb.on("key", e => console.log("key", e.page, e.key, e.action));    // not sent by the firmware yet
```

- **Requests are pipelined:** every method returns a promise for its own response. A status other than OK throws a `ControlBlockError`.
- **`putImage(id, data)`:**
  - It splits the image into chunks the device can take.
  - Uploading to an ID the device already has replaces the image everywhere it's used.
  - Use fixed IDs for images you replace, and content IDs (`keyImage`) for the rest.
- **`freeImages(ids)`:** the ID goes at once, and the memory once no key uses it.
- **Browsers:** `ControlBlock` and `src/protocol.js` work on any WebUSB `USBDevice`, so you can pass `navigator.usb`'s. `src/images.js` is Node only (it uses sharp and the rle565 encoder).
