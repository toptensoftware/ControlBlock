# Wi-Fi

**Built and working on the hardware:** it connects to a WPA2 and a WPA3 network (see [Built](#built-and-where-it-differs-from-the-design) for the differences from this design, and the plan's step 8 for the hardware checks still to do). The host sets the Wi-Fi credentials over USB; the device keeps them in NVS, reads them back on request, and connects in the background. Wi-Fi is needed by standalone mode's HTTP and UDP actions and, later, NTP for the status bar clock ([`standalone.md`](standalone.md)).

## Goals and non-goals

- Set, read back and clear one network's credentials over the vendor protocol ([`protocol.md`](protocol.md)), from the command line (`host/`) or any host app.
- Survive reboots and firmware updates. Never lose or half-write the credentials.
- Connect on boot without a host, reconnect after drops, and report the state to the host.
- Wi-Fi must not disturb the display. The bounce fill is the constraint behind most of the choices below.
- **Not in scope:** several saved networks, access-point mode or a captive-portal setup page, WPA2-Enterprise, a web UI (all later).

## Behaviour

- **Independent of the operating mode.** The Wi-Fi messages work in standalone and in hosted mode, and neither mode starts, stops or resets Wi-Fi. `RESET`, `RESET_STREAM`, taking control and releasing leave the credentials and the connection alone, like the stored config.
- **One network.** One SSID and one password, or none.
- **Boot:** if credentials are stored, start Wi-Fi and connect. If not, Wi-Fi is not started at all (no RAM spent, no radio).
- **Setting credentials** stores them, then connects (or reconnects) in the background. The response does not wait for the connection: the host polls `WIFI_STATUS` or listens for the `WIFI_STATE` event.
- **Setting the same credentials again** changes nothing: no flash write, no reconnect.
- **Clearing** (empty SSID) erases the stored credentials, disconnects and stops Wi-Fi.
- **Reading back** returns the SSID and the password in clear text (see Security).

## Protocol

New message types (`0x40`..`0x42`) and one event. The wire spec is in [`protocol.md`](protocol.md#wi-fi), which is the source of truth; this section is the design it came from.

| Type | Name | Purpose |
|---|---|---|
| `0x40` | WIFI_SET | Store credentials and connect; empty SSID clears |
| `0x41` | WIFI_GET | Read the stored credentials |
| `0x42` | WIFI_STATUS | Connection state, IP, signal |
| `0x84` | WIFI_STATE (event) | The state changed (`0x83` is HEARTBEAT) |

`GET_INFO` `flags` gains **bit 1: Wi-Fi credentials are stored**, so a host can tell without reading the password.

No new response status codes: `BAD_LENGTH`, `BAD_ARG` and `IO_ERROR` (the NVS write failed) cover everything.

### `0x40` WIFI_SET

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | ssid_len | 0..32 |
| u8 | pass_len | 0, or 8..63 |
| u8[ssid_len] | ssid | Raw bytes (an SSID need not be UTF-8) |
| u8[pass_len] | pass | Raw bytes. 0 = an open network |

- `BAD_LENGTH` if the payload is shorter than the two lengths say. Trailing bytes are ignored (the usual rule).
- `BAD_ARG` if `ssid_len` is over 32, `pass_len` is 1..7 or over 63, `ssid_len` is 0 with a non-zero `pass_len`, or either string contains a NUL byte (they are stored as C strings). Nothing changes.
- `ssid_len = 0` (and `pass_len = 0`) clears the credentials.
- A 64-character hex pre-shared key is not accepted in v1.
- `IO_ERROR` if the NVS write fails. The previously stored credentials are still in place (see Storage).
- On `OK` the credentials are stored. The connection attempt starts after the response.

### `0x41` WIFI_GET

**Request:** No payload.

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u8 | ssid_len | 0 if none are stored |
| u8 | pass_len | |
| u8[ssid_len] | ssid | |
| u8[pass_len] | pass | |

### `0x42` WIFI_STATUS

**Request:** No payload.

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u8 | state | See below |
| u8 | reason | The last disconnect reason (ESP-IDF `wifi_err_reason_t`), 0 if none. For diagnostics; the host shouldn't depend on the values |
| i8 | rssi | dBm while connected, else 0 |
| u8 | reserved | 0 |
| u8[4] | ip | `a.b.c.d` in that order while `CONNECTED`, else 0 |
| u8[6] | mac | The station MAC (handy for DHCP reservations). All zeros in `NO_CREDENTIALS` (the driver isn't running then) |

States:

| Value | Name | Meaning |
|---|---|---|
| 0 | `NO_CREDENTIALS` | Nothing stored; Wi-Fi is not running |
| 1 | `CONNECTING` | Associating or waiting for an IP |
| 2 | `CONNECTED` | Has an IP |
| 3 | `RETRYING` | The attempt failed or the link dropped; waiting to retry (`reason` says why) |

### `0x84` WIFI_STATE (event)

| Type | Field | Notes |
|---|---|---|
| u8 | state | As above |
| u8 | lost | Event-overflow flag (see [`protocol.md`](protocol.md#device--host-events)) |
| u8[2] | reserved | 0 |
| u8[4] | ip | As in `WIFI_STATUS` |

Sent when the state changes (not on every retry), so a host can show "connected" without polling. A host that doesn't care ignores it.

## Storage (NVS)

The `nvs` partition (24 KB at `0x9000`) already exists in `partitions.csv`.

- **Namespace** `cb_wifi`, **one key** `cred`: a single blob, written with one `nvs_set_blob`. NVS writes a new item and then invalidates the old one, so a power loss mid-write leaves the old credentials or the new ones, never a mix. Two separate keys (ssid, pass) could be left half updated, so don't split them.
- **Blob format** (little-endian): `u8 version (1)`, `u8 ssid_len`, `u8 pass_len`, `u8 reserved (0)`, then the ssid and pass bytes. Decoding validates the version and lengths with the same rules as `WIFI_SET`; a blob that fails is treated as "no credentials" (and erased), never as a boot failure.
- **Clearing** is `nvs_erase_key`.
- **Equality check before writing:** read the stored blob and skip the write if identical, so repeated sets cost no wear.
- **Boot:** `nvs_flash_init()` early in `main`. If it returns `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`, erase and init again (the credentials are lost; the host can set them again). The Wi-Fi driver uses NVS too (PHY calibration data), and so does the planned standalone state; they use their own namespaces.
- **The driver must not store credentials itself.** Call `esp_wifi_set_storage(WIFI_STORAGE_RAM)` before setting the config, so there is exactly one copy of the credentials (ours), and the driver does not write to flash on every `esp_wifi_set_config`.
- **Flash writes and the display:** an NVS write is a flash write, and flash writes stall the bounce ISR (`CONFIG_LCD_RGB_ISR_IRAM_SAFE` is off; this is the same issue as the config upload). A credentials write is small (one flash page, plus an occasional page erase when NVS garbage-collects) and rare. Not a problem on the hardware, since credentials are very rarely written.
- The Wi-Fi stack also writes to NVS on its own (PHY calibration data, when it changes) around `esp_wifi_start`. Check for a display glitch there on the hardware too.

## Security

- The credentials are plaintext in NVS. Flash encryption and NVS encryption are off in this project.
- `WIFI_GET` returns the password to anyone who can open the USB device. That is by design (the host can show and edit the saved values), and the device is a desktop control surface where USB access already means full control of it. If that stops being acceptable, the options are: leave out the password and return only its length, or require a physical action (a touch confirmation) before returning it. Not planned.
- Credentials are never logged (log only the SSID and the lengths).

## Wi-Fi stack setup

Station mode only. The sequence, once, at startup when credentials exist (`wifi.cpp`, the device layer):

1. `nvs_flash_init()` (in `main`, see Storage).
2. `esp_netif_init()` and `esp_event_loop_create_default()`. Nothing else in the firmware creates the default event loop yet; check before adding it.
3. `esp_netif_create_default_wifi_sta()`, then set the hostname (`controlblock-<last 3 MAC bytes>`) with `esp_netif_set_hostname`.
4. `esp_wifi_init()` with `WIFI_INIT_CONFIG_DEFAULT()` and the buffer settings below, `esp_wifi_set_storage(WIFI_STORAGE_RAM)`, `esp_wifi_set_mode(WIFI_MODE_STA)`.
5. Register handlers for `WIFI_EVENT` (any id) and `IP_EVENT_STA_GOT_IP` / `IP_EVENT_STA_LOST_IP`.
6. `esp_wifi_set_config(WIFI_IF_STA, …)` with the SSID and password. Set the auth threshold to `WIFI_AUTH_WPA2_PSK` for a password and `WIFI_AUTH_OPEN` for none (so a WPA2/WPA3 mixed AP works and a downgrade to WEP is refused), and `sae_pwe_h2e = WPA3_SAE_PWE_BOTH`.
7. `esp_wifi_start()`, then `esp_wifi_connect()` (or from the `STA_START` event).

To change or clear the credentials while running: `esp_wifi_disconnect()`, `esp_wifi_set_config()`, `esp_wifi_connect()`; to clear, `esp_wifi_stop()` (and `esp_wifi_deinit()` if the RAM is needed back). Keep init and teardown in one place so a clear followed by a set works.

**Event handling** (the handlers only post to the Wi-Fi task; they do no work themselves):
- `STA_DISCONNECTED` records `reason` and schedules a retry. Backoff: 1 s, doubling to a 30 s cap, reset on `GOT_IP`. For an authentication failure (wrong password, including a router password changed after connecting; reasons MIC failure, 802.1X failed and auth fail, but not the handshake timeouts, which a weak signal also causes and which keep retrying) stop retrying, with no timer: it can't succeed and some routers lock out. The state stays `RETRYING` with `reason` set; new credentials (`WIFI_SET`) or a reboot restart it. Showing this on the status bar is left for a later, broader change.
- `STA_CONNECTED` moves to waiting for an IP; `GOT_IP` moves to `CONNECTED` and posts a `WIFI_STATE` event.
- A new `WIFI_SET` resets the backoff and connects at once.

The state machine is separate from the driver calls (pure: events in, "connect now" / "retry in N ms" / "state changed" out; the device layer owns the timer) so it is host tested.

**Settings to review** (in `sdkconfig.defaults`; measure the heap before and after, see the RAM note):
- `esp_wifi_set_ps(WIFI_PS_NONE)` after connecting. The default modem sleep adds tens of ms to each request, which hurts button-to-action latency, so power save is off while USB powered. **Later:** the device should also run off a battery for short periods; revisit power save (and the backlight) then, measuring the latency cost.
- Fewer buffers: lower static RX buffers (`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM`), dynamic RX/TX buffers and the AMPDU windows from their defaults. The traffic is a few small HTTP/UDP requests, not streaming.
- Leave `CONFIG_ESP_WIFI_IRAM_OPT` / `RX_IRAM_OPT` off unless the IRAM is plentiful (they trade RAM for speed we don't need).
- Don't enable BLE or the Wi-Fi/BLE coexistence code.

### Cores, RAM and the bounce fill

- **Internal RAM** is the tight resource; see [Internal RAM](#internal-ram) for the measured budget (~106 KB free connected, release build).
- **Wi-Fi/lwIP buffers in PSRAM** (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) would save the internal RAM but means heavy PSRAM writes while receiving, and **nothing else may write PSRAM heavily while fills run** (CLAUDE.md). Only do this with a `RUN_STRESS` pass that adds Wi-Fi traffic, and look at the late-fill counts.
- **Cores:** keep the Wi-Fi driver and the lwIP task on core 0 (the defaults; check `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0` and `CONFIG_LWIP_TCPIP_TASK_AFFINITY`). Core 1 belongs to the compositor, swipe and touch tasks; the bounce fill ISR runs on core 0 too (`app_main` creates the panel there). Moving Wi-Fi and lwIP to core 1 was tried and helped only part of the way (see [Latency](#latency)). The Wi-Fi radio interrupt runs on whichever core started the driver, so start it from a task pinned to core 0.
- **Code in PSRAM:** the Wi-Fi driver is largely IRAM/flash code and has IRAM-safe parts; with `SPIRAM_FETCH_INSTRUCTIONS` on, the build and the connection work (WPA2 and WPA3 networks).
- **Boot order:** `i2c_init`, `ch422g_init`, display, touch, USB, then Wi-Fi last, so a Wi-Fi problem can never keep the screen from coming up.

## Code layout

| Part | Where | Tested |
|---|---|---|
| Validation and the blob format | `wifi_credentials.cpp/.h` (pure; owns the rules, so `Protocol` and the device layer share them) | host |
| Set/get/clear logic over an injected storage (`Load`, `Save`, `Erase` of one blob), equality check, change notification | same module | host, over a fake storage |
| Wire messages | `protocol.cpp` (new `ProtocolHost` callbacks `wifi_set`, `wifi_get`, `wifi_status`), `GET_INFO` flag bit 1, the `WIFI_STATE` event | host, raw bytes in and responses decoded, like the existing protocol tests |
| Connection state machine and backoff | `wifi_state.cpp/.h` (pure) | host |
| NVS storage, the driver calls, the event glue, the task | `wifi.cpp/.h` (device only) | hardware only |
| `host/` | `setWifi()`, `getWifi()`, `wifiStatus()`, a `wifiState` event | `npm test` with `FakeDevice.js` |
| Command-line tool | `node cli/cli.js wifi` (`set <ssid> [password]`, `get`, `clear`, `status`, `wait`) | by hand, on the device |

## Implementation plan

Each step: write or adjust the tests first, see them fail, then implement. Steps 1–5 and 7 run without hardware. The device layer (6) and everything after it can only be checked on the board: flag it every time.

1. **`wifi_credentials`: rules and blob** (`firmware/test`). Tests: valid and invalid SSID and password lengths, NUL bytes, clearing, blob round trip, a corrupt blob (bad version, lengths past the end, truncated) decodes as "none". Then the module.
2. **Set/get/clear over a fake storage.** Tests: set stores, get returns it, setting the same value again does no write, clear erases, a failing `Save` reports an error and leaves the old value, the change callback fires once per real change. Then implement.
3. **Protocol messages.** Extend the protocol tests: `WIFI_SET` / `GET` / `STATUS` byte for byte, `BAD_LENGTH`, `BAD_ARG` and `IO_ERROR` mappings, `GET_INFO` flag bit 1 before and after, trailing bytes ignored, the messages are not affected by `RESET` / `RESET_STREAM`, the `WIFI_STATE` event queues like the others. Add the `ProtocolHost` callbacks (the test host fakes them) and implement.
4. **`wifi_state`: the state machine.** Tests: first attempt, success, disconnect and backoff doubling to the cap, reset on success, authentication failure stops retrying, a new `WIFI_SET` retries at once, clear goes to `NO_CREDENTIALS`, a stale event after a clear is ignored. Then implement.
5. **Host library and CLI.** `host/` tests against `FakeDevice.js` for the three calls and the event (and `FakeDevice` gets the same messages); then the command line (now `node cli/cli.js wifi`, [`cli/README.md`](../cli/README.md)). Update `docs/protocol.md` with the wire spec as part of this step (keep firmware, `host/` and the spec in sync).
6. **Device layer** (`wifi.cpp`, hardware only). NVS init in `main`, the NVS storage for step 2, `esp_netif`/`esp_wifi` setup and the event glue as in "Wi-Fi stack setup", the task on core 0, `host_link.cpp` wiring the `ProtocolHost` callbacks, `WIFI_STATE` events. Add `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash` to the `main` component requirements. Build the firmware (debug and release) to check it compiles. Log the free heap and largest block with Wi-Fi up.
7. **Doc sweep:** `protocol.md` (done in step 5), `standalone.md` (its Wi-Fi section now points here), `status-bar.md` (RAM note), `CLAUDE.md` (Wi-Fi in the firmware module list and Open Items once built).
8. **Hardware checks** (the user flashes): set, read back, clear and reconnect from the CLI; credentials survive a reboot and a reflash; wrong password stops retrying until new credentials or a reboot; the router restarting recovers; flash write from `WIFI_SET` and the connect itself don't glitch the display; free heap and the largest block with Wi-Fi up; late-fill counts with Wi-Fi traffic running (extend `RUN_STRESS` with a UDP/HTTP loop); the `SPIRAM_FETCH_INSTRUCTIONS` combination; button-to-request latency with power save off.

## Built, and where it differs from the design

Steps 1 to 7 of the plan are done. Step 6 (`wifi.cpp`) connects on the board; credentials survive a reboot and a reflash, and the NVS write doesn't glitch the display. Not yet confirmed: wrong password stopping (built, host tested) and router restart recovery. Power save is for later (battery use).

- **Files:** `wifi_credentials.cpp/.h`, `wifi_state.cpp/.h` (pure, host tested), `wifi.cpp/.h` (device), `heap_log.h` (the heap log line, shared with `main.cpp`). Tests: `TestWifiCredentials.cpp`, `TestWifiState.cpp`, the Wi-Fi facts at the end of `TestProtocol.cpp`, with `wifi_env.h` (a fake storage). Host library: `setWifi`, `clearWifi`, `getWifi`, `wifiStatus`, the `wifiState` event and `waitWifiConnected`; the command line is `cli/cli.js wifi`.
- **Set and get are not `ProtocolHost` callbacks.** `Protocol` is given the `WifiConfig` (as it is given the `ConfigStore`) and calls `Set` / `Get` / `Stored` itself, so the rules and the storage logic exist once. Only `wifi_status` is a callback (the state lives in the Wi-Fi task). The change notification is `WifiConfig::SetChangedCallback`.
- **Equality check against the in-memory copy**, loaded from NVS at boot, rather than reading the blob back before every write. Same result without the extra flash read.
- **`RETRYING` stays `RETRYING` across the attempts** after the first failure, until it connects. This is what makes "events on state change, not on every retry" hold; `CONNECTING` is only the very first attempt (and the one after a new `WIFI_SET`, or after the IP is lost).
- **`STA_CONNECTED` is ignored**: it would not change the state (still waiting for an IP).
- **Our own disconnect is not a failure.** `esp_wifi_disconnect()` and stop produce a `STA_DISCONNECTED` with reason `ASSOC_LEAVE` (8). The machine remembers when it asked for one and ignores that event once. A stray disconnect when no attempt was made is ignored too, and so is every event after a clear.
- **Boot order:** `wifi_init()` (load the credentials from NVS; no radio, no task) runs before `host_link_init()`, so the first `GET_INFO` already has the flag. `wifi_start()` (the task, and the connect) runs last. A `WIFI_SET` between the two is queued and handled when the task starts.
- **NVS:** `nvs_flash_init()` is `init_nvs()` in `main.cpp`, early, with the erase-and-retry. A failure that remains is logged and the device boots without Wi-Fi settings (`WIFI_SET` answers `IO_ERROR`).
- **sdkconfig (`sdkconfig.defaults`):** static RX buffers 6 (default 10), dynamic RX and TX 16 (32), AMPDU windows RX 6 and TX 4, SoftAP support off, `ESP_WIFI_IRAM_OPT` and `RX_IRAM_OPT` off (they default to on, and IRAM and internal RAM are one SRAM), lwIP's task on core 0 (`LWIP_TCPIP_TASK_AFFINITY_CPU0`; the driver's own task is core 0 by default). `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` stays off, so nothing here writes PSRAM. The numbers are a first guess; the heap left (below) shows there is room.
- **Logging:** the SSID and the password length only. `wifi.cpp` logs the heap (free, largest block) with the driver up, down and connected.

## Internal RAM

The first hardware runs failed the WPA2 handshake (reason 204) and the WPA3 one (reason 202) because the driver left under 1 KB of internal RAM. The fix was moving the status bar's two 64 KB buffers to PSRAM ([`status-bar.md`](status-bar.md)). Both networks connect now.

Boot heap log (`heap_log.h`, internal RAM free, release build):

| Step | Free |
|---|---|
| before `esp_wifi_init()` | 139,879 (largest block 98,304) |
| after init | 129,751 |
| Wi-Fi driver up | 107,715 (largest 67,584) |
| connected | 105,831 (largest 64,512, minimum ever 103,119) |

These are from 2026-11-05, with the net worker, ops and Lua work in. A debug build has about 95 KB free with the driver up. The earlier figures (152,879 before init, 118,839 connected) predate that work.

- Fills at rest were unaffected by Wi-Fi coming up (61 fps, 0 over budget). Wi-Fi traffic during a swipe is not tested and not a concern: actions are a quick fire-and-forget after a press.
- `ESP_WIFI_IRAM_OPT` and `ESP_WIFI_RX_IRAM_OPT` stay off. IRAM and internal RAM are one SRAM, so turning them on takes memory the driver needs (with the memory then tight, the Wi-Fi task couldn't be created or `esp_wifi_init()` failed with `ESP_ERR_NO_MEM`). They were tried again during the latency work: they cost about 36 KB of internal RAM (about 59 KB free with the driver up) and did not help latency. Rejected.
- More room if ever needed: trim the Wi-Fi/lwIP buffers (static RX, dynamic RX/TX, TCP windows, mailboxes), or move them to PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`; needs a `RUN_STRESS` pass with Wi-Fi traffic).

## Latency

**Symptom:** pings to and from the device took 3-1400 ms with some loss (other clients on the same AP: 1 ms), even at -36 to -41 dBm with power save off.

**Cause:** radio interference from the RGB panel's pins at the default drive strength (2), most likely the 25 MHz PCLK. **Fix:** drive strength 0 on all 20 panel pins (`DISPLAY_PIN_DRIVE_CAP` in `waveshare_rgb_lcd_port.h`). Pings are now 2-10 ms with no loss, 10-20 ms while swiping, and the picture and the `RUN_STRESS` fill figures are unchanged.

How it was found (a bisection, kept in case it comes back):

- The stock ESP-IDF station example on the same board and AP: fast. The same with ControlBlock's `sdkconfig.defaults`: fast. So not the AP, the board, or our settings.
- ControlBlock with `-DCB_NO_DISPLAY=ON`: fast. So the display.
- Wi-Fi and lwIP on core 1 (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1`, `LWIP_TCPIP_TASK_AFFINITY_CPU1`): better but still bad. With the Wi-Fi IRAM optimisations as well: worse. So not core 0 CPU time or PSRAM code fetches alone.
- `-DCB_IDLE_FILL=ON` (the panel clocking, the fill doing nothing): as bad as ever. So not the fill's load: the running panel signals themselves.
- Drive strength 0: fixed.

Not tried: drive 1, or weak drive on PCLK only (only worth it if the picture ever shows problems). If latency returns, re-check these two switches first.

The pixel clock can also be lowered (`-DCB_PIXEL_CLOCK_MHZ`, see CLAUDE.md) to give the fill more headroom; that is separate from the Wi-Fi fix.

## Open questions

- Whether the right-hand status text should show the Wi-Fi state or the IP until NTP exists (the right text is blank for now).
- Several saved networks, and a way to set credentials without a host (touch entry, a setup access point): later.
- Whether `WIFI_GET` should stay in clear text (see Security).
- Whether the Wi-Fi task should wait for the host's first message or for a quiet moment at boot, if the connect shows up as a display glitch.
