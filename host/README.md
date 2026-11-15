# ControlBlock host library

The JavaScript library for everything a PC does with a ControlBlock:

- **Device API:** drives the device over its USB vendor interface (hosted mode), in Node or in a browser (WebUSB).
- **Key images:** renders and encodes key pictures ([button-face](https://github.com/toptensoftware/button-face#readme) definitions, SVGs, raw pixels).
- **Layout bake:** turns a standalone layout ([`docs/layout-format.md`](../docs/layout-format.md)) into the blob the device loads, and uploads it.

**How to use it:** [the hosted mode guide](../docs/guide/hosted-mode.md) (connecting, keys, images, pages, events, errors, uploading a layout from code). The wire protocol is [`docs/protocol.md`](../docs/protocol.md). The [CLI](../cli/README.md) and the scripts in [`examples/`](../examples/README.md) are built on this library.

## Install

Needs Node.js 24 or later and git (button-face comes from GitHub). In an app, install a release from GitHub (not
the npm registry) and import `@toptensoftware/controlblock`:

```sh
npm install github:toptensoftware/ControlBlock#cli            # or #cli-v0.1.0 to pin a release
```

In a clone of the repo: `npm install` once in the repo root (npm workspaces), then import by path; `npm test` here
runs the library and bake tests (no device needed). Setting up the device and the PC (USB driver, udev rule) is in
[Getting started](../docs/guide/getting-started.md).

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

binpack and the rle565 encoder are imported through this package's `imports` map: `#binpack` (`src/bake/binpack.js`, a one-line shim over the `binpack/` submodule, which isn't a workspace because its repo has its `node_modules` checked in) and `#rle565` (the `rle565/encoder` workspace, linked, not copied). Edits to either apply at once. The release's package (`scripts/build-cli-package.mjs`, the `cli` branch) points both at vendored copies. button-face (`@toptensoftware/button-face`) comes from GitHub (`git+https://github.com/toptensoftware/button-face.git`), pinned to a commit by the root `package-lock.json`: `npm install git+https://github.com/toptensoftware/button-face.git -w host -w tools/test-icons` in the repo root picks up a newer one.
