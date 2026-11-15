# ControlBlock host library

The JavaScript library for everything a PC does with a ControlBlock:

- **Device API:** drives the device over its USB vendor interface (hosted mode), in Node or in a browser (WebUSB).
- **Key images:** renders and encodes key pictures ([button-face](https://github.com/toptensoftware/button-face#readme) definitions, SVGs, raw pixels).
- **Layout bake:** turns a standalone layout ([`docs/layout-format.md`](../docs/layout-format.md)) into the blob the device loads, and uploads it.

**How to use it:** [the hosted mode guide](../docs/guide/hosted-mode.md) (connecting, keys, images, pages, events, errors, uploading a layout from code). The wire protocol is [`docs/protocol.md`](../docs/protocol.md). The [CLI](../cli/README.md) and the scripts in [`examples/`](../examples/README.md) are built on this library.

## Install

Needs Node.js 24 or later and git (button-face comes from GitHub). Not on npm yet: from this folder,

```sh
npm install
npm test                    # library and bake tests, no device needed
```

and import it by path. Setting up the device and the PC (USB driver, udev rule) is in [Getting started](../docs/guide/getting-started.md).

| Entry | Has |
|---|---|
| `index.js` (Node, the package's default) | everything |
| `browser.js` (the package's `browser` entry) | the device API only; imports nothing Node-only |

## Example

```js
import { ControlBlock, KeyAction, keySlot, keyPixelSize, keyImage, buttonLayer } from "./index.js";

const cb = await ControlBlock.open();           // the first ControlBlock on USB
await cb.takeControl();                         // the device boots standalone: take the screen first

const { width, height } = keyPixelSize({ w: 1, h: 1 }, cb.info);     // 144 x 144
const play = await keyImage([await buttonLayer({ layers: [
    { type: "fill", color: "#2a6f97" },
    { type: "svg", src: "#play", color: "white", scale: 0.6 },
] }, width, height)], width, height);
await cb.syncImages([play]);                    // uploads it unless the device has it already
await cb.setKeys([{ id: 1, page: 0, ...keySlot(0), normal: play }]);

cb.on("key", e => console.log("key", e.id, e.action === KeyAction.RELEASE ? "tapped" : e.action));
```

On a device, from this folder: `node ../examples/demo.js` (pages, a 2U key, half-height keys, toggles, a clock replaced in place, a freed image) and `node ../examples/bench.js` (latency, throughput and upload speed).

## Development

| Path | What |
|---|---|
| `src/ControlBlock.js`, `src/protocol.js`, `src/Emitter.js` | the device API |
| `src/images.js` | the key image helpers |
| `src/bake/` | the bake: `resolve.js` (stage 1), `render.js` (stage 2), `pack.js`/`schema.js` (stage 3, BPAK through binpack), `bake.js` (all three), `files.js` (`loadLayoutFile`, `bakeFile`), `upload.js` (`uploadBlob`), `headers.js` (the firmware headers `tools/codegen` writes) |
| `test/` | device API tests against `test/FakeDevice.js` (a fake WebUSB device); `test/bake/` the bake tests |

binpack is imported by relative path (`../../../binpack/`), not installed, so edits to it apply at once. The rle565 encoder is a `file:` dependency, copied in by `npm install` (`install-links=true`): after changing it, bump its version, delete its folder under `node_modules` and `npm install` again. button-face (`@toptensoftware/button-face`) comes from GitHub (`git+https://github.com/toptensoftware/button-face.git`), pinned to a commit by `package-lock.json`: `npm install git+https://github.com/toptensoftware/button-face.git` picks up a newer one.
