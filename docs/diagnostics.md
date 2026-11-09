# Diagnostics over Wi-Fi

Read-only HTTP endpoints for checking the device's status and recent logs with `curl`, a browser or a small script. Always on while Wi-Fi is connected; LAN trust only (no password, no toggle, nothing that changes the device).

## Finding the device

- **Name:** the device name (default `controlblock-` + the last three MAC bytes in hex, e.g. `controlblock-a1b2c3`) is both the mDNS name (`<name>.local`) and the DHCP hostname. Set it with `node cli/cli.js name set <name>` (`--default` goes back to the MAC-based one) and read it with `node cli/cli.js name`; protocol: [`NAME_SET` / `NAME_GET`](protocol.md#device-name).
- **mDNS** (`espressif/mdns`): hostname and instance name = the device name, one `_http._tcp` service on port 80. A rename applies to mDNS at once and to the DHCP hostname on the next lease (reconnect or reboot).
- The IP is also in the boot log and in `/status`.

## Endpoints

Port 80 (`DIAG_HTTP_PORT`). All responses send `Cache-Control: no-store`.

| Path | Response |
|---|---|
| `GET /` | A small HTML page: status refreshed every 5 s, log tail every 2 s |
| `GET /status` | JSON snapshot (below) |
| `GET /log` | `text/plain`: every line in the log ring |
| `GET /log?since=N` | Lines after sequence number `N` only |

`/log` headers:
- `X-Log-Next`: the sequence number of the last line returned; pass it back as `since` to poll for new lines.
- `X-Log-Dropped: 1`: lines after `since` were already evicted (or `since` is newer than anything in the ring), so the response starts from the oldest line held.
- A `since` that isn't a number gets `400`.

### Polling the log

```sh
curl -s -D - http://controlblock-a1b2c3.local/log          # whole ring; note X-Log-Next
curl -s http://controlblock-a1b2c3.local/log?since=1234    # only newer lines
```

A tail loop (bash):

```sh
since=; while true; do
  next=$(curl -s -D /dev/stderr "http://controlblock-a1b2c3.local/log${since:+?since=$since}" 2>&1 >/dev/tty \
         | tr -d '\r' | sed -n 's/^X-Log-Next: //Ip')
  [ -n "$next" ] && since=$next; sleep 2
done
```

## Status JSON

```json
{
  "uptimeMs": 123456,
  "app": { "version": "...", "idf": "v6.0", "built": "Nov  9 2026 15:00:00" },
  "name": "controlblock-a1b2c3",
  "heap": { "internalFree": 0, "internalMinFree": 0, "internalLargest": 0,
            "psramFree": 0, "psramMinFree": 0, "psramLargest": 0 },
  "screen": { "fps": 49, "fillMaxUs": 140, "lateFills": 0 },
  "compositor": { "builds": 0, "buildAvgUs": 0 },
  "images": { "count": 0, "bytes": 0 },
  "wifi": { "state": "connected", "reason": 0, "ip": "192.168.1.20", "rssi": -48, "mac": "aa:bb:cc:a1:b2:c3" },
  "ntp": { "enabled": true, "synced": true },
  "net": { "requests": 0, "errors": 0, "dropped": 0 },
  "ops": { "busy": false, "scriptRuns": 0, "scriptErrors": 0, "scriptHeapPeak": 0 },
  "layout": { "loaded": true, "activities": 3, "activity": "", "summary": "version 1, 16384 bytes, 2 devices" },
  "log": { "nextSeq": 1235 }
}
```

- `screen` and `compositor` are sampled once a second by `main.cpp` (`diag_sample_second`), so they are up to a second old: fps, the slowest fill and the running total of late fills; keys drawn in the last second and their average time.
- `heap.internalMinFree` is the low-water mark since boot.
- `net`: requests = ok + failed + timed out; errors = failed + timed out; dropped = dropped + refused.
- `ops.scriptHeapPeak` in bytes; `layout.activity` is empty with none selected.
- The field list and shape are fixed by `firmware/test/TestDiagStatus.cpp`.

## Log capture

- `ESP_LOGx` output is unchanged on UART0; `log_capture.cpp` hooks `esp_log_set_vprintf` and copies each line into a 32 KB ring in PSRAM (`log_ring.cpp`, pure and tested), then chains to the previous vprintf.
- Lines are numbered with a 32-bit sequence; lines over 240 bytes are truncated; the oldest whole lines are evicted on wrap.
- Never blocks: a try-lock flag, and a line that finds it taken (another task logging, or a `/log` read copying the ring) is left out of the ring (still on UART). ISR and early logs bypass vprintf and aren't captured.
- Logs may show SSIDs, IPs and URLs from layout ops; passwords are not logged.

## Server

`diag_http.cpp`: `esp_http_server` on core 0, priority 2, 5 KB stack, `max_open_sockets` 2 with LRU purge, 3 handlers. Started when Wi-Fi connects (after mDNS), stopped (with `mdns_free`) on any other Wi-Fi state. `/log` allocates a 32 KB PSRAM scratch per request and sends 1 KB chunks. Sockets: listener + 2 sessions + control = 4, the net worker 1-2, of `LWIP_MAX_SOCKETS` 10.
