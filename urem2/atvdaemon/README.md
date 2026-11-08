# atvdaemon

A UDP daemon that controls an Apple TV via [pyatv](https://pyatv.dev). Keeps a persistent connection to the device and accepts simple text commands over UDP, making it easy to drive from any language.

## Setup

Pair your Apple TV with pyatv first if you haven't already:

```bash
atvremote --id <device-id> --scan-hosts <ip> pair
```

Credentials are stored in `~/.pyatv.conf`.

## Running

```bash
./start.sh
```

`start.sh` creates a virtualenv at `~/.venv/pyatv`, installs pyatv if needed, then launches the daemon. Edit `atvdaemon.py` to change the device ID, IP, or port (defaults: `2E:10:43:01:B6:8F`, `10.1.1.169`, UDP `7777`).

## UDP Protocol

Send UTF-8 text commands to port `7777`. Each command gets a single-line response.

| Command | Response | Description |
|---|---|---|
| `ping` | `pong` / `connecting` | `pong` if connected to ATV, `connecting` if not |
| `key <name>` | `ok` / `error: ...` | Press a remote control key |
| `app <bundle-id>` | `ok` / `error: ...` | Launch an app by bundle ID |
| `disconnect` | `ok` | Drop the ATV connection |

### Key names

`up` `down` `left` `right` `select` `menu` `home` `home_hold` `play_pause` `play` `pause` `next` `previous` `stop` `volume_up` `volume_down` `top_menu` `channel_up` `channel_down` `screensaver`

### Quick test

```bash
echo "ping" | nc -u -w2 127.0.0.1 7777
echo "key menu" | nc -u -w2 127.0.0.1 7777
echo "app com.netflix.Netflix" | nc -u -w2 127.0.0.1 7777
```

## Node.js client

```js
import { atv } from "./atv.js";

// Wait for ATV to be ready (polls ping, up to 20s)
await atv.connect();

// Send keys
await atv.key("select");
await atv.key("volume_up");

// Launch an app
await atv.app("com.netflix.Netflix");

// When turning the ATV off manually
await atv.disconnect();
```

### Typical power-on flow

```js
// 1. Power on via IR blaster
// 2. Wait for daemon to establish connection
await atv.connect(20000);  // throws if not connected within 20s

// 3. Send commands
await atv.key("home");
```

### Typical power-off flow

```js
// 1. Power off via IR blaster
// 2. Tell the daemon to drop its connection immediately
await atv.disconnect();
// Daemon will also detect passive disconnects (e.g. ATV auto-sleep) via background health check
```

## Connection behaviour

- On startup the daemon connects immediately; if that fails it retries on the first command.
- `key`, `app` and `text_*` wait for the connection: if not connected they retry every 2 s (`CONNECT_RETRY`) for up to ~20 s (`CONNECT_WAIT`), waiting on a background connect started by `ping` rather than starting a second one. So a command sent while the ATV wakes replies `ok` once connected; one that can't connect replies `error: ...` after ~20 s. Callers don't need to poll `ping` first.
- A background health check runs every 5 seconds. If the ATV stops responding on the network the connection is dropped and `ping` starts returning `connecting`.
- On any command failure the connection is invalidated and re-established on the next command.
- `ping` returning `connecting` also triggers a reconnection attempt in the background.
