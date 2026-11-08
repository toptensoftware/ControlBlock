# Status bar clock

Status: Investigate

## Description
A clock built into the device, shown at the right of the status bar.

- **Time source:** NTP when Wi-Fi is enabled. Without Wi-Fi, a host API sets
  the time, so a small client script can set it. Setting the time must work
  without a host takeover (i.e. while standalone).
- **Display:** a `{clock}` placeholder in the status text shows the current
  time (e.g. setting the right text to `{clock}`). It must coexist with the
  hosted-mode status text: a host that has taken control can set its own text
  or use `{clock}`.
- Covers firmware, the protocol spec (`docs/protocol.md`), the host library
  (`host/`), a small client script or CLI command, and docs.

## Notes

### 1. Status bar today
- `status_bar.h/.cpp` (pure, tested in `firmware/test/TestStatusBar.cpp`): `StatusBar` holds `status_bar_content_t` (`left`/`right` char[`STATUS_BAR_TEXT_MAX` = 48], pills). `SetLeft/SetRight` -> `SetText` copy under a spinlock, cut to 47 bytes on a UTF-8 boundary, return true only if changed. `BeginDraw/EndDraw` double buffer.
- Render (`status_bar_render_rows`): left text at x=16; right text right aligned (`SCREEN_W - 16 - font_measure_text`). **No overlap check or ellipsis**: long left + right + pills can overlap. The font (Inter SemiBold 20 px) has proportional digits (no `tnum` in `tools/test-icons/gen.js` `packFont`), so a right-aligned time's left edge shifts slightly as digits change (right edge fixed: fine).
- Font: `UI_ASSET_STATUS_FONT`, ASCII 32-126 (`charset: "32-126"`): digits, `:`, space, `a m p A M P` all present. Nothing outside ASCII.
- `status.cpp` (device): `status_task` (core 1, prio 3, **stack 3072**) waits on `xTaskNotifyWait(..., portMAX_DELAY)` (or 1 tick while a change waits on a swap). A change -> `BeginDraw`, renders 4-row (or 2-row mid-swipe) bands into a 6.4 KB internal scratch, copies one band per fill gap (shared with the compositor via `fill_gap`/`gap_share.h`), swaps after the last band. One redraw = 10-20 gaps (0.17-0.33 s, so the minute change appears up to ~0.3 s late; fine).
- Redraw cost: measured in `RUN_STRESS` with the bar redrawn every 20 ms: 0 late fills (docs/status-bar.md "Measured"; an old demo redrew a clock each second, 475 µs worst once in debug). Once a minute (or once a second) is far below what was tested.
- Triggering a timed redraw: no timer exists today. Simplest: the status task computes the time to the next minute (or second) boundary and uses it as the `xTaskNotifyWait` timeout while any text holds a placeholder, then re-expands and calls `SetLeft/SetRight` (redraws only if the expanded text changed). Tick is 100 Hz (10 ms granularity; wait >= 1 tick).
- Setters: `status_set_left/right()` (`status.h`), safe from any task. Callers: `host_link.cpp:ph_set_status_text` (SET_STATUS_TEXT), `standalone_task.cpp:as_set_status_left` (activity name), `main.cpp` stress run (`status_set_right` with round numbers).

### 2. Where status text comes from
- Hosted: `Protocol::OnSetStatusText` (`protocol.cpp`; u8 field, u8 len, UTF-8 <= 47 bytes, validated by `valid_utf8`) -> `ProtocolHost::set_status_text` -> `host_link.cpp:ph_set_status_text`. Refused `NOT_HOSTED` while standalone (`Protocol::NeedsHost`).
- `Protocol::ResetModel` (RESET, TAKE_CONTROL, release/disconnect via `Release`) sets both texts to "". So after a release, standalone Home has **empty left and right** text.
- Standalone: only the left text is ever set: `ActivityApplier::Apply` (`activity_host.cpp`) sets left = activity `displayName` (else `name`), "" for none. **Nothing sets the right text in standalone**; the layout schema (`host/src/bake/schema.js`) has no status-text field (root: version, longPressMs, home, devices, activities; activity: name, displayName, ...).
- Hence standalone needs a source for `{clock}`: either a device default (right = `{clock}` whenever standalone), or a layout field (e.g. root `statusRight: "{clock}"`, maybe per activity), default `{clock}`.
- Pure placement: a new pure module (e.g. `status_text.cpp/.h` or `clock_format.cpp/.h`) with (a) a formatter: broken-down local time (h, m, s) + format -> "12:35 pm"/"12:35", and (b) an expander: template + time-known flag + local time -> output (cut to 47 bytes); returns whether the template contains a placeholder (so the task knows to tick). The *template* (as set) must be kept separately from the *expanded* text: either `StatusBar` stores templates and expansion happens in `BeginDraw` (then the expander is called from a pure, tested class) or `status.cpp` keeps the templates and pushes expanded strings into `StatusBar`. Keeping templates in `StatusBar` + an injected "now" makes the whole thing host-testable.
- Placeholder in left text too: costs nothing if the expander runs on both fields. Layout-file text (displayName) would get it for free if it passes through the same setter. Watch the 47-byte limit: a template `{clock}` (7 bytes) expands to <= 8 ("12:59 pm", "23:59:59"); expansion can grow text past 47 -> cut.
- `SET_STATUS_TEXT` while hosted can send `{clock}` unchanged (no protocol change needed for the placeholder). A literal `{` would then need an escape (e.g. `{{`) or only exact known placeholders are replaced (simpler: replace only `{clock}`/`{clock:...}`, leave other braces alone).

### 3. Protocol
- Message IDs (`protocol.h` enum, `docs/protocol.md` table): 0x00-0x07 general (0x04 **reserved** "for whether the device gets a clock, other status bar content" per protocol.md Open questions), 0x10-0x12 images, 0x20-0x22 pages, 0x30-0x32 layout, 0x40-0x42 Wi-Fi, events 0x80-0x84. Candidates: `0x04` SET_TIME (uses the reserved slot), or a new `0x50` group (TIME_SET / TIME_GET / CLOCK_CONFIG) mirroring Wi-Fi's 0x40 group.
- Allowed while standalone: anything not in `Protocol::NeedsHost` (PING, GET_INFO, QUERY_IMAGES, SET_BACKLIGHT, LAYOUT_*, WIFI_*, TAKE/RELEASE). A new time request just stays out of `NeedsHost` (plus the table in protocol.md "TAKE_CONTROL" and `HOSTED_REQUESTS` in `host/test/FakeDevice.js` stays unchanged).
- Adding a request touches: `protocol.h` (enum, handler decl, maybe a `ProtocolHost` callback e.g. `set_time(int64 epoch_ms)` / `time_status()`), `protocol.cpp` (`Dispatch`, handler), `host_link.cpp` (the device callback: `settimeofday`), `firmware/test/TestProtocol.cpp` (fake host lambdas at ~line 100), `docs/protocol.md`, `host/src/protocol.js` (Msg id, encode/decode), `host/src/ControlBlock.js` (method), `host/test/FakeDevice.js` (+ tests), `cli/cli.js` + `cli/args.js` + `cli/README.md` + `cli/test/`.
- GET_INFO (`Protocol::OnGetInfo`): flags bits 0-2 used (`INFO_FLAG_BACKLIGHT/WIFI_CREDENTIALS/HOSTED`); bit 3 could be "time is set". Fields could also be appended at the end (device time) if `decodeInfo` tolerates it.
- Suggested payload: SET_TIME `i64 unix_ms` (or u32 s + u16 ms) UTC; optional TZ string (or a separate request). A TIME_GET/TIME_STATUS response: set?, source (none/host/ntp), unix time, TZ string, NTP server, last sync.

### 4. Wi-Fi / NTP
- `wifi.cpp`: one Wi-Fi task (core 0). `driver_up()` does `esp_netif_init`, the default event loop and handlers once (`s_handlers`), creates the STA netif. `apply()` sees `WifiState::Connected` (GotIp) and calls `s_state_cb`. Natural SNTP hooks: init once after `esp_netif_init` in `driver_up` (or on the first GotIp in `apply`), restart (`esp_netif_sntp_start()`) on each GotIp. Credentials cleared -> `driver_down` (could `esp_netif_sntp_deinit()`).
- ESP-IDF v6.0 API: `C:\esp\v6.0\esp-idf\components\esp_netif\include\esp_netif_sntp.h`: `esp_netif_sntp_init(&cfg)` with `ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org")` (fields smooth_sync, server_from_dhcp, wait_for_sync, start, sync_cb, renew_servers_after_new_IP, ...), `esp_netif_sntp_start()`, `esp_netif_sntp_deinit()`, `esp_netif_sntp_sync_wait(ticks)`, event `NETIF_SNTP_EVENT/NETIF_SNTP_TIME_SYNC`. The older `esp_sntp.h` (lwip/include/apps) still exists (aclock uses it). C++ designated-initializer rule: build the config with `= {}` and assign fields rather than the macro if it trips `-Werror` (the macro lists every field, so it may be fine).
- Current sdkconfig (`build/release/sdkconfig`): `CONFIG_LWIP_SNTP_MAX_SERVERS=1`, `CONFIG_LWIP_SNTP_UPDATE_DELAY=3600000` (hourly resync), `CONFIG_LWIP_DHCP_GET_NTP_SRV` off, startup delay up to 5 s. Time syscalls `CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT=y`: `time()/gettimeofday` run off the RTC timer + esp_timer, so **the time survives a soft restart** (`esp_restart`, e.g. after a layout commit) but not power-off.
- RAM: lwIP SNTP is a UDP pcb + a timer in the tcpip task (well under 1 KB); `wait_for_sync` creates a semaphore. Negligible against ~106 KB free connected; measure anyway.
- NTP server: aclock defaults to `pool.ntp.org`. Configurable server would follow the Wi-Fi pattern: NVS blob via an injected storage interface (`wifi_credentials.h` `WifiStorage`/`WifiConfig`: pure validation + `wifi.cpp` `nvs_load/save/erase`, namespace `cb_wifi`), set through a protocol request and the CLI `wifi set` path (`cli/cli.js` `WIFI` table).

### 5. Time zone / DST
- **How aclock does it** (`C:\Users\Brad\Projects\aclock\clock\main\ntp.c`): **no geo-IP, no config page, no automatic detection**. The TZ is a user-entered **POSIX TZ string stored in NVS** (namespace `ntp`, keys `tz` and `server`; `nvs_read/nvs_write`, strings), default `"UTC0"` and `pool.ntp.org`. `ntp_init()` reads both, `setenv("TZ", tz, 1); tzset();`, registers `on_got_ip` (IP_EVENT_STA_GOT_IP -> `sntp_start()`: `esp_sntp_setoperatingmode(POLL)`, `setservername(0, ...)`, `set_sync_interval(1 h)`, `sntp_set_time_sync_notification_cb(on_sync)`, `esp_sntp_init`). Set via console command `cmd_settime` (`settime <server> <tz>`, e.g. `EST5EDT,M3.2.0,M11.1.0`; saved to NVS, applied at once, SNTP restarted) and `cmd_syncntp` (`esp_sntp_restart`). DST is entirely newlib's POSIX rule handling (`localtime_r`). `ntp_is_synced()` = a sync has happened (`s_last_sync != 0`); `digital.c:digital_update` shows nothing until then and formats 12 h itself (`tm_hour` > 12 -> -12, 0 -> 12), redrawing when h*60+m changes. `status_ntp` reports server/tz/sync state as JSON.
- **Reuse:** the approach (NVS-stored POSIX TZ + server, `setenv/tzset`, SNTP start on GotIp, hourly resync, "synced" flag gating the display) carries over directly. The code itself doesn't: it's C with `esp_console`/telnet and aclock's `status`/`json` modules, and ControlBlock has no console; its storage should follow `WifiConfig` (pure + injected NVS) and use `esp_netif_sntp_*`. The 12 h conversion is trivially reimplemented in the pure formatter.
- ESP-IDF newlib supports full POSIX TZ (`std offset dst [offset],start[/time],end[/time]` with `Mm.w.d` rules). Example Sydney: `AEST-10AEDT,M10.1.0,M4.1.0/3`.
- Host tests: MSVC's CRT doesn't understand POSIX `M` rules, so `localtime_r` can't be host tested. Options: keep TZ conversion device-only (pure formatter tests take a `struct tm`/h:m:s), or write a small pure POSIX TZ parser + DST calculator (testable, ~150 lines, avoids `setenv` thread-safety issues: newlib's `setenv`/`tzset` are global; calling `setenv` on the protocol task while the status task calls `localtime_r` is a race).
- Storage options for TZ: (a) NVS via a protocol request + CLI (works without a layout, matches aclock and Wi-Fi), (b) layout file field (e.g. root `clock: { timeZone, format, ntpServer }`): uploaded with the layout, but tied to a restart and needs a schema change + `gen-types.js` + re-baked sample. A host can't easily derive a POSIX string from an IANA name in Node (Intl gives offsets, not rules); options: user supplies POSIX string, or the CLI bundles an IANA->POSIX table (e.g. the posix_tz_db CSV), or the CLI derives the current offset only (wrong after a DST change unless re-run).
- RTC: the Waveshare ESP32-S3-Touch-LCD-4.3 has no battery-backed RTC chip (aclock uses only the ESP32's internal RTC timer; nothing in aclock or our drivers talks to an RTC). After power-off the time is unknown until NTP or a host sets it.

### 6. Host side
- `cli/cli.js`: `COMMANDS` table (`bake`, `upload`, `wifi`); `wifi` opens with `io.open({serial})`, dispatches to the `WIFI` action table, closes. Args in `cli/args.js` (+ `cli/test/args.test.js`, `cli/test/cli.test.js` with the FakeDevice). A `time` (or `clock`) command fits the same pattern: `time set` (from the PC clock, default), `time get`, `time zone <posix>` / `time server <host>`.
- `ControlBlock.connect()` (`host/src/ControlBlock.js`) already does RESET_STREAM + GET_INFO; it could set the time on connect (opt-out option), so any host app keeps the device's clock right. Allowed without TAKE_CONTROL if the request stays out of `NeedsHost`.
- Without Wi-Fi something must run after each power-up: a host app, or a tiny script (`node cli/cli.js time set`) run at logon / on device arrival (e.g. Windows Task Scheduler). WinUSB is exclusive: it fails while another app has the device open.

### 7. Testing
- Host-testable (`firmware/test/`, sources listed in `test.mk.js` `otherSourceFiles`): the formatter/expander, `StatusBar` with templates (if expansion moves there; inject "now"), the protocol request (`TestProtocol.cpp` fake host), a time/TZ config class over an injected storage (like `wifi_credentials`), a POSIX TZ parser if written. `port.h` has no clock; injected clocks are done by callback/ctx (`Standalone`'s injected clock `clock_ms`, `ProtocolHost` callbacks, `WifiStorage`).
- Host library: `host/test/FakeDevice.js` + `ControlBlock.test.js`; CLI: `cli/test/`.
- Hardware only: SNTP sync, `settimeofday`, newlib TZ/DST, survival across `esp_restart`, the status task's timed wake, RAM with SNTP running.

### 8. Open questions for the user
- 12 h ("12:35 pm" like the status-bar.md mock-up "12.35am") or 24 h; configurable?
- Seconds shown? (per-second redraws are affordable but busier).
- Before the time is known: blank, `--:--`, or hide?
- `{clock}` only, or `{clock:<format>}` (e.g. `{clock:HH:mm}`), or `{time}`/`{date}` too?
- Placeholders in the left text and layout `displayName` too?
- Standalone right text: always `{clock}` by default, or from a layout field?
- TZ source: NVS via CLI/protocol (POSIX string), layout file, or IANA name mapped by the CLI? NTP server configurable or hard-coded `pool.ntp.org`?
- Should a host set the time automatically on connect?
- Should a host-set time be overridden by NTP when both exist (proposed: yes, NTP wins and resyncs hourly)?

## Flagged assumptions
- Board has no battery-backed RTC (none used in aclock or our drivers); time is lost on power-off.
- SNTP's RAM cost is negligible (not measured).
- The set-time request carries UTC; the TZ lives on the device so NTP and DST work without a host.
- Status task stack (3072) may need raising if `localtime_r`/formatting runs in it (newlib TZ code); to be checked with the stack high-water mark.

## Plan
