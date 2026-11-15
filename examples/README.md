# Examples

Scripts and layouts for trying ControlBlock on the hardware: the layouts used for hardware testing
during development, the host library demos and a small test server. Run everything from the repo
root. Nothing here needs an `npm install`: the scripts import the library by relative path
(`../host/index.js`), so install the library once (`npm install` in `host/`).

| File | What it is |
| --- | --- |
| `demo.js` | Host-controlled (mode 1, protocol v2) demo: takes control, renders and uploads each key's opaque normal and pressed images, defines two pages (with a 2U key and two half-height keys), shows off the image store. `node examples/demo.js` |
| `bench.js` | Link benchmark: request latency, pipelined throughput, image upload speed (a noisy 1U image, and a page of realistic keys: 30 opaque images). `node examples/bench.js` |
| `upload-test.json` | A small standalone layout: a few `hidKey` keys on Home. |
| `key-shapes.mjs` | Standalone layout showing the key shapes on Home: a 2U span, a 2x2 and a 2U-tall key, a grouped Volume -/+ (one picture, two keys), half-height and half-width keys, and a 0.5U key off the whole-unit grid with a named id. Each key types a different key over USB HID. |
| `bake-check.mjs`, `layouts.test.mjs` | Test helper and tests: every layout here (and urem2) bakes and its keys pass the device's checks (ids, rects, opaque images of the key's size). `node --test "examples/**/*.test.mjs"` |
| `net-test.mjs` | Standalone layout for the net ops (`httpGet`, `httpPost`, `udpPacket`, `sendWol`), against the test server. |
| `activity-test.mjs` | Standalone layout for activity switching and devices; every hook announces itself to the test server. |
| `script-test.mjs` | Standalone layout for the Lua `script` op (checklist in `wip/complete-lua-hardware-checks.md`); its Scripts activity also has two auto-repeat keys (`wip/complete-auto-repeat.md`). |
| `test-common.mjs` | Shared by `net-test.mjs`, `activity-test.mjs` and `script-test.mjs`: the test server's address (`CB_TEST_SERVER`, `CB_TEST_SERVER_NAME`), ops that announce themselves to it, flat key faces. Its test: `test-common.test.mjs`. |
| `test-server/test-server.mjs` | Test server for the layouts above: HTTP on :8000, UDP on :9 (WoL) and :9999. Node built-ins only. `node examples/test-server/test-server.mjs`; its tests: `node --test examples/test-server/test-server.test.mjs` |
| `test-server/takeover.mjs` | Takes control of the device, holds it, then releases it. `node examples/test-server/takeover.mjs [seconds]` (default 3) |

## Standalone layouts

Bake and upload one with the CLI (see [`cli/README.md`](../cli/README.md)):

```sh
node cli/cli.js upload examples/activity-test.mjs                  # bake in memory and upload
node cli/cli.js bake examples/net-test.mjs -o scratch/net-test.bin  # bake only
```

A `.mjs` layout is a module whose default export is the layout object. Asset paths in a layout
resolve relative to the layout file's folder. The format is in
[`docs/layout-format.md`](../docs/layout-format.md).

**The test server's address** is in one place, `test-common.mjs`, and every test layout takes it
from there (as does urem2's test target). Set `CB_TEST_SERVER` to the IP address of the PC that runs
`test-server.mjs`, and `CB_TEST_SERVER_NAME` to a host name the device can resolve to that PC (used by
`net-test.mjs`'s by-name requests), when you bake or upload; the defaults are the author's test PC.
Allow the server through the PC's firewall.

```sh
CB_TEST_SERVER=192.168.1.20 CB_TEST_SERVER_NAME=mypc node cli/cli.js upload examples/net-test.mjs
```

(PowerShell: `$env:CB_TEST_SERVER = "192.168.1.20"` first.)
