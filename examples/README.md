# Examples

Scripts and configs for trying ControlBlock on the hardware: the configs used for hardware testing
during development, the host library demos and a small test server. Run everything from the repo
root. Nothing here needs an `npm install`: the scripts import the library by relative path
(`../host/index.js`), so install the library once (`npm install` in `host/`).

| File | What it is |
| --- | --- |
| `demo.js` | Host-controlled (mode 1) demo: takes control, uploads faces and icons, defines two pages, shows off the image store. `node examples/demo.js` |
| `bench.js` | Link benchmark: request latency, pipelined throughput, image upload speed. `node examples/bench.js` |
| `upload-test.json` | A small standalone config: a few `hidKey` keys on Home. |
| `net-test.json` | Standalone config for the net ops (`httpGet`, `httpPost`, `udpPacket`, `sendWol`), against the test server. |
| `activity-test.mjs` | Standalone config for activity switching and devices; every hook announces itself to the test server. |
| `script-test.mjs` | Standalone config for the Lua `script` op (checklist in `wip/wip-lua-hardware-checks.md`). |
| `test-common.mjs` | Shared by `activity-test.mjs` and `script-test.mjs`: the test server's address, ops that announce themselves to it, flat key faces. |
| `test-server/net_test_server.py` | Test server for the configs above: HTTP on :8000, UDP on :9 (WoL) and :9999. Python 3, standard library only. `python examples/test-server/net_test_server.py` |
| `test-server/takeover.mjs` | Takes control of the device, holds it, then releases it. `node examples/test-server/takeover.mjs [seconds]` (default 3) |

## Standalone configs

Bake and upload one with the CLI (see [`cli/README.md`](../cli/README.md)):

```sh
node cli/cli.js upload examples/activity-test.mjs                  # bake in memory and upload
node cli/cli.js bake examples/net-test.json -o scratch/net-test.bin # bake only
```

A `.mjs` config is a module whose default export is the config object. Asset paths in a config
resolve relative to the config file's folder. The format is in
[`docs/config-format.md`](../docs/config-format.md).

**The test server's address is hard-coded:** `SERVER` in `test-common.mjs` and every URL/host in
`net-test.json` name `10.1.1.126`. Change them to the PC that runs `net_test_server.py` (and allow
the server through its firewall).
