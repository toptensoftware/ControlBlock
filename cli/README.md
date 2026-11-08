# ControlBlock command line

Bakes and uploads standalone-mode layouts, and sets up the device's Wi-Fi and clock. It is a thin
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

### `time [action] [--serial S]`

The device's clock, shown by `{clock}` in the status bar (standalone, the right text;
[`docs/protocol.md`](../docs/protocol.md#clock)). None of these take control of the
device: they work while it runs its standalone layout.

| Action | |
|---|---|
| `set` (or no action) | set the device's clock from this computer's; prints `clock set to <UTC time>` |
| `get` | show the device's time (UTC) and who set it (the host or NTP), its time zone and NTP server |
| `zone <posix TZ>` / `zone --default` | store the time zone, a POSIX TZ string (default `UTC0`); applies at once, daylight saving by the string's rules |
| `server <host>` / `server --default` | store the NTP server (default `pool.ntp.org`); used from the next sync |

The time zone is a POSIX TZ string, not a name like `Australia/Sydney`: standard name,
offset **west** of UTC (so east is negative), then the daylight saving name and its start
and end rules. For example:

| Place | TZ string |
|---|---|
| Sydney, Melbourne | `AEST-10AEDT,M10.1.0,M4.1.0/3` |
| Brisbane (no DST) | `AEST-10` |
| New York | `EST5EDT,M3.2.0,M11.1.0` |
| London | `GMT0BST,M3.5.0/1,M10.5.0` |

With Wi-Fi the device sets its clock by NTP (hourly). Without it, the clock is lost at
power-off (the board has no battery-backed clock) and stays blank until something sets
it: run `node cli/cli.js time` after each power-up, for example at logon from Windows Task
Scheduler (it fails while another app has the device open: WinUSB is exclusive). A host
app can do the same on connect with the library's `ControlBlock.open({ setTime: true })`.

### Options

- `--serial S`: the device to use, by serial number (default: the first one found).
- `--force`: upload even if the device already has the blob.
- `--default`: (`time zone`, `time server`) put the setting back to the device's default.

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
node cli/cli.js time zone AEST-10AEDT,M10.1.0,M4.1.0/3             # Sydney
node cli/cli.js time                                                # set the clock from this PC
node cli/cli.js time get
```

## Tests

`npm test` in `cli/` (no device: the upload, wifi and time commands run against the library's
`FakeDevice`; the bake runs the real renderer). `cli.js` exports
`main(argv, { open, reconnect, stdout, stderr })`, which resolves the exit code, and only
runs itself when executed directly. `args.js` (`parseArgs`, `USAGE`, `UsageError`) is the
pure argument parser.
