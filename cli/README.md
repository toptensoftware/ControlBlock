# ControlBlock command line

Bakes and uploads standalone-mode layouts, and sets up the device's Wi-Fi. It is a thin
wrapper: argument parsing and output only. Everything it does is the library's
(`../host`: `bakeFile`, `uploadBlob`, `ControlBlock`), imported by relative path, so there
is nothing to install here beyond `npm install` in `host/`.

Run it from the repo root:

```sh
node cli/cli.js <command> ...
node cli/cli.js                      # prints the usage
```

Optionally `npm link` in `cli/` puts it on the path as `controlblock`.

## Commands

### `bake <layout> [-o out.bin] [--upload [--serial S] [--force]]`

Bakes a layout (`.json`, or a `.js`/`.mjs` module whose default export is the layout;
[`docs/layout-format.md`](../docs/layout-format.md)) into the blob the device loads. Asset
and script paths in the layout are relative to the layout file.

- Writes `<layout name>.bin` next to the layout, or the file `-o` names. With `--upload`
  and no `-o`, nothing is written.
- Prints any warnings, then `<file>: N bytes`.
- A layout with errors prints every error with its path in the layout and exits 1;
  nothing is written.
- `--upload` also uploads the blob, as `upload` does.

### `upload <layout|baked.bin> [--serial S] [--force]`

Uploads to the device over USB ([`docs/protocol.md`](../docs/protocol.md#layout-upload)):
sends the blob, waits for the device to restart and checks it kept the same blob. A `.bin`
is sent as it is; a layout is baked in memory first. If the device already has the same
blob nothing is sent, unless `--force`.

### `wifi <action> [--serial S]`

The device's Wi-Fi network ([`docs/wifi.md`](../docs/wifi.md)):

| Action | |
|---|---|
| `set <ssid> [password]` | store a network and connect (no password, or `""`: an open network); waits up to 30 s and prints each new status |
| `get` | show the stored network (the password in clear text) |
| `clear` | forget it and turn Wi-Fi off |
| `status` | show the connection state, e.g. `CONNECTED, 192.168.1.5, -52 dBm, MAC 34:85:18:...` |
| `wait [seconds]` | wait until connected (default 30 s), printing each new status |

A password that starts with `-` goes after `--`: `wifi set Home -- -secret-`.

### Options

- `--serial S`: the device to use, by serial number (default: the first one found).
- `--force`: upload even if the device already has the blob.

## Exit codes

| Code | |
|---|---|
| 0 | done (`wifi set`/`wait`: connected) |
| 1 | failed: a bad layout, a device error, no device, or (`wifi set`/`wait`) not connected in time or no network stored |
| 2 | usage error (the usage is printed) |

## Examples

```sh
node cli/cli.js bake examples/net-test.json                         # writes examples/net-test.bin
node cli/cli.js bake examples/activity-test.mjs -o scratch/a.bin
node cli/cli.js upload examples/activity-test.mjs                   # bake in memory and upload
node cli/cli.js upload scratch/a.bin --serial 3C84279E1D50 --force
node cli/cli.js wifi set MyNetwork "my password"
node cli/cli.js wifi status
```

## Tests

`npm test` in `cli/` (no device: the upload and wifi commands run against the library's
`FakeDevice`; the bake runs the real renderer). `cli.js` exports
`main(argv, { open, reconnect, stdout, stderr })`, which resolves the exit code, and only
runs itself when executed directly. `args.js` (`parseArgs`, `USAGE`, `UsageError`) is the
pure argument parser.
