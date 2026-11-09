# Status bar icons

Status: Implement (steps 1-10 done; only the user hardware check, step 11, remains)

## Description
Status icons in the status bar, to the right of the clock:

- **Wi-Fi:** connectivity (e.g. off / connecting / connected, maybe signal strength).
- **Busy:** while the op queue is running (e.g. an activity switch powering devices on, a waited HTTP request).
- **Error:** a temporary indicator after an op fails (e.g. shown for ~5 s, then cleared).

Should work in standalone mode; how it coexists with hosted mode (host status text, whether a host can show or
hide the icons) is to be decided.

## Notes

**Status bar rendering**
- `status_bar.cpp`: `status_bar_render_rows()` draws left text, right text right-aligned at `SCREEN_W - STATUS_BAR_MARGIN - font_measure_text(...)` (line ~56), then `draw_pager()` pills centred. Pure, host tested (`TestStatusBar.cpp` checks banded == whole-bar render byte for byte).
- `status_bar_content_t` (left/right 48-byte templates, pill_count, selected_pill) is copied out under `BeginDraw()` with `{clock}` expanded (`clock_text_expand`). Icons fit naturally as a new content field (e.g. a `uint8_t icons` bitmask / wifi level enum) set through a `StatusBar::SetIcons()` returning changed, like `SetInt`.
- `status.cpp`: status task (core 1) renders `STATUS_BAR_BAND_ROWS_MAX`=4 rows into internal scratch, copies one band per fill gap (`fill_gap`), swaps buffers. Any change = full 40-row redraw (10 bands); fine for rare icon changes, but a flickering busy icon would cost redraws (cheap: text + a few small blits).
- Timed wake already exists: the task waits `clock_wait_ticks()` while `HasClock()`. A 5 s error expiry needs a similar "next deadline" (pure: `StatusBar` returns ms until the next icon expiry; the task takes min with the clock wait).
- Setters are thread-safe (`status_set_*` → `changed()` → `xTaskNotify`).

**Assets**
- `ui.bin` from `tools/test-icons/gen.js` (blob 0 Inter SemiBold 20 px font, 1-2 pager pills); indices in `ui_assets.h`, validated in `status.cpp` `ui_ok()` (`UI_ASSET_COUNT`, `check_image_header`). gen.js already renders node-buttonImage built-in icons to rle565 (used for test_keys), so status icons (~20-24 px, alpha) can be added as more ui.bin blobs. Adding blobs needs `ui_assets.h`, `ui_ok`, `status_bar_style()` and the host test's asset load updated.
- Alternative: glyphs in the font (no new image path) — but rle565 images with alpha already work (pills).

**Wi-Fi source**
- `wifi_state.h`: `WifiState` NoCredentials/Connecting/Connected/Retrying. `wifi_set_state_cb()` (single callback, currently taken by `host_link.cpp` `on_wifi_state` → `PostWifiState`). Would need fan-out (call status from `on_wifi_state` or allow a second cb).
- RSSI: only live via `wifi_get_status()` (`esp_wifi_sta_get_ap_info`), no event on change. Signal bars would need periodic polling (e.g. status task or a timer every ~5-10 s).

**Busy source**
- `standalone_task.cpp` `op_task`: `busy = s_sa.Poll()` and the `s_was_busy` edge (logs "op queue empty (busy N ms)"). Natural hook: on edges call a status setter. Waited `http.get` in scripts and `delay` keep Poll() busy, so they're covered. Short ops (a HID tap ~40 ms) would flash the icon → needs a show delay (e.g. show only after busy ≥ 300-500 ms) — a pure timing helper testable on host.

**Error sources**
- `OpQueue::Errors()`/`LastError()` (via `Standalone::Errors()`), counted for every executor `OpStatus::Error` (net op with Wi-Fi down, net queue full, refused switch, script errors end the op in Error too). `op_task` already detects `errors > s_logged_errors` — the hook point for "show error icon for 5 s". Note: async net failures (HTTP 404/timeouts of fire-and-forget requests) finish in `net_worker.cpp` after the op is Done, so they are NOT counted; `ScriptHost::Errors()` counted separately.

**Hosted mode**
- Host owns left/right text via SET_STATUS_TEXT; release resets right to `{clock}` (`STATUS_TEXT_STANDALONE_RIGHT`, protocol.h). Wi-Fi icon is meaningful in both modes; busy/error are standalone-only (queue re-armed on takeover). No protocol change needed if icons are firmware-owned; an optional future `SET_STATUS_ICONS`/flag could let a host hide them.

## Tweaks (user, 2026-11-09, after the first build; supersede the decisions and notes below where they differ)
- Wi-Fi: waves = connected, dot = not connected (first built the other way round). Waves white like the clock text and heavier.
- Busy and error share one place; the error overrides busy (`StatusIcons::Visible`). Busy is a small white hourglass (the dim dots were too subtle). Error is a red circle with a white x (was a red triangle).
- Placement: icons just **left** of the clock (left to right: error/busy, Wi-Fi, clock), moving with its width; the clock stays at the right margin. No fixed strip: only the icons showing take a slot (`status_bar.cpp` `draw_icons`).

## Decisions (user, 2026-11-09)
1. **Wi-Fi: state only, two distinct icons** (not dimming): connected = the usual Wi-Fi "radar waves" icon; not connected = a dot (corrected by the user 2026-11-09; first built the other way round). No signal bars, no polling. Placeholders are fine; the user may supply icons later.
2. **Busy: shown immediately** when the op queue starts, and kept for **at least a minimum time, default 500 ms** (a tunable constant). Must be **subtle**: only noticed when looked for.
3. **Error icon (5 s): only for things that reach outside the device and fail** for external/connectivity reasons (things that normally work when the configuration is right): net ops failing (connect/timeout, Wi-Fi down, presumably HTTP error statuses), including async fire-and-forget HTTP failures. **Not** hard configuration errors (e.g. Lua runtime errors): those don't show the icon but must be clearly visible in the log.
4. **Placement: right of the clock** (icons at the far right, clock just left of them). **Hosted mode: only the Wi-Fi icon shows**; no protocol change.


**Implemented (steps 1-4)**
- `status_icons.h/.cpp` (pure, `TestStatusIcons.cpp`): `StatusIcons` (SetWifi/SetBusy/ReportError/SetHosted/Visible/NextDeadlineMs), bits `STATUS_ICON_WIFI_CONNECTED/WIFI_OFF/BUSY/ERROR` (bit n = icon image n), `STATUS_BUSY_MIN_MS` 500 (from the latest start edge), `STATUS_ERROR_SHOW_MS` 5000, `STATUS_NO_DEADLINE` -1, `status_wait_ms(clock, icons)`. Added to `test.mk.js` and `main/CMakeLists.txt`.
- Status bar: `status_bar_content_t.icons`, `StatusBar::SetIcons`, `style.icon[4]`. Strip of 3 fixed 22 px slots (error, busy, Wi-Fi left to right) at the far right; right text ends `STATUS_ICON_TEXT_GAP` (8) left of it and doesn't move as icons change; icons == 0 → old layout. Preview: `firmware/test/build/status_bar_icons.bmp`.
- ui.bin blobs 3-6 (`UI_ASSET_STATUS_ICON`, `UI_ASSET_COUNT` 7): 20 px SVG placeholders in `tools/test-icons/gen.js` (`STATUS_ICONS`). **To replace an icon:** edit its SVG there (any ~20 px square with alpha), `npm run build` in tools/test-icons (rewrites firmware/assets/ui.bin), rebuild. `ui_ok` checks every image blob. Host test asset load needed no change (`LoadAsset`).
- `status.cpp`: `s_icons` under its own spinlock, monotonic `esp_timer` ms; the task refreshes icons between draws and waits `status_wait_ms(clock, icons)`. Setters `status_set_wifi/busy/error/set_hosted` (status.h) notify the task. Not called by anything yet (steps 5-8). Device-only parts untested on host.
- Tests: 649 debug and release.

**Implemented (steps 5-8)**
- Wi-Fi: `host_link.cpp` `on_wifi_state` also calls `status_set_wifi` (cb set before `wifi_start`, so every change arrives; boot shows the dot from `StatusIcons` default NoCredentials). `ph_mode_changed` calls `status_set_hosted` (take/release/disconnect all go through it).
- Busy: `op_task` calls `status_busy(busy)` on each `s_was_busy` edge.
- External errors: `NetFailureIsExternal(result, status)` in `net_host.h` (Timeout, Failed, HTTP >= 400). `NetHost::external_error` (optional cb): `NetSend` (`exec_net.cpp`, also used by the script API) calls it for Wi-Fi down and queue full. Scripts, refused switches, bad params never touch it (structurally: only `NetSend` calls it). Tests in `TestExecNet.cpp`, `TestNetRequest.cpp`; fake counts calls (`net_env.h`).
- Device: `net_worker.cpp` sets `external_error` to `status_error()` and calls `status_error()` after any finished request (fire and forget or waited) that `NetFailureIsExternal`. Wiring done in the worker, not `standalone_task.cpp` (it owns the NetHost). Config errors keep the existing `ESP_LOGE "op failed: <why>"` line in `op_task`. Requests the queue expires/drops unsent don't show the icon.
- Tests: 652 debug and release; debug device build 0 errors. Hardware-only: all of 5, 6, 8.
**Docs and final runs (steps 9-10)**
- Docs: `docs/status-bar.md` (new Icons section, diagram, assets table), `docs/standalone.md` (Status bar: icons, what counts as an error), `docs/wifi.md` (icon note under WIFI_STATE), `docs/protocol.md` (SET_STATUS_TEXT: icons firmware owned).
- Runs (2026-11-09): host tests 652 debug, 652 release; debug and release device builds 0 errors; host/ 225 pass, cli/ 23 pass, urem2 182 pass, examples 17 pass, 0 fail.

**Proposed CLAUDE.md updates (not applied; for the user to confirm)**
- Firmware module list, after the status bar entry: "**`status_icons.cpp/.h`:** the status bar's icons right of the clock (Wi-Fi dot/waves, busy with a 500 ms minimum, error for 5 s after an external net failure: `NetFailureIsExternal` in `net_host.h`). Pure and tested; `status.cpp` feeds it and wakes for its deadlines. Hosted: Wi-Fi only. Spec: [`docs/status-bar.md`](docs/status-bar.md#icons)."
- Activity switching Open Item: replace "Follow-ups: a busy indicator while the op queue runs;" with "The status bar's busy icon shows while the op queue runs;".
- Open Items: "**Status icons:** built and host tested, not run on the hardware (checklist in `wip/wip-status-icons.md` step 11). Placeholder icons; replace via `STATUS_ICONS` in `tools/test-icons/gen.js`."

## Flagged assumptions
- (Superseded by decision 4: icons go right of the clock.) Icons sit just left of the right text (clock), right text keeps its right alignment; the icon strip width shifts the text.
- Busy/error icons cleared on takeover (`SetHosted`); Wi-Fi icon kept in hosted mode.
- (Superseded by decision 3.) Error = any op that ended in `OpStatus::Error`.
- Net queue full counts as external (transient). Expired/dropped-unsent requests don't raise the icon.
- Steps 2-3 done together (assets first). Icon placeholders: dot and waves grey #a0a0a0, busy three dim #505050 dots, error red triangle. Icon strip keeps fixed slots (text never shifts); strip always present in practice since a Wi-Fi icon always shows.

## Plan
- [x] 1. **Icon state model (pure, TDD).** New `firmware/main/status_icons.h/.cpp` + `firmware/test/TestStatusIcons.cpp` (add to `test.mk.js`). Write tests first: Wi-Fi state → `WifiIcon` {Connected=waves, NotConnected=dot} (Connecting/Retrying/NoCredentials → dot); busy shows on the start edge immediately, stays until max(end edge, start + `STATUS_BUSY_MIN_MS` = 500); a new start during the minimum extends correctly; error shows on `ReportError(now)` for `STATUS_ERROR_SHOW_MS` = 5000, re-report restarts the 5 s; `SetHosted(true)` clears busy/error and hides them while hosted (Wi-Fi kept); `Visible(now)` returns the icon set; `NextDeadlineMs(now)` = ms to the next busy-minimum/error expiry, or "none". Clock injected (`now_ms` parameters), no FreeRTOS. Build + `mk run` green.
- [x] 2. **Status bar content + render placement (TDD).** Extend `status_bar_content_t` with an icons field (bitmask/enum from step 1) and `StatusBar::SetIcons()` returning changed (like `SetInt`). Tests first in `TestStatusBar.cpp`: icons drawn at the far right (`SCREEN_W - STATUS_BAR_MARGIN - strip width`), right text (clock) right-aligned just left of the strip, no icons → layout unchanged (existing tests still pass), banded render == whole-bar render with icons, `SetIcons` same value → not changed. Use temporary test images (or the step-3 assets if done first: implementer may swap 2 and 3). Keep the icon strip width a constant in `status_bar.h`. Check debug and release test counts match.
- [x] 3. **Placeholder assets.** `tools/test-icons/gen.js`: add three ~20 px alpha rle565 icons to `ui.bin` (Wi-Fi dot, Wi-Fi waves, busy, error; busy a dim/low-contrast look so it's subtle, error a clear but small mark), from node-buttonImage stock icons or simple drawn shapes. Update `ui_assets.h` indices/`UI_ASSET_COUNT`, `status.cpp` `ui_ok()`, `status_bar_style()`, and the host test's asset load. Regenerate `firmware/assets/ui.bin`; `mk run` green. Note in Notes how the user can replace the icons later.
- [x] 4. **Status task wake (pure helper + device).** Pure `status_wait_ms(clock_wait_ms, icon_deadline_ms)` (min, "none" handling) with a test; `status.cpp` uses it so the task wakes for busy-minimum/error expiry as well as the clock, recomputes `Visible(now)` and calls `SetIcons`. Thread-safe setters `status_set_wifi(WifiState)`, `status_busy(bool)`, `status_error()`, `status_set_hosted(bool)` (lock/notify like `status_set_*`). Device build (debug) 0 errors.
- [x] 5. **Wi-Fi fan-out.** `host_link.cpp` `on_wifi_state` also calls `status_set_wifi` (or make `wifi_set_state_cb` allow two callbacks; prefer the simple call, DRY). Set the initial state at boot. Hosted/standalone switch (`ResetModel` path for take/release/disconnect) calls `status_set_hosted`. Device build green. (Device only: not host testable — note.)
- [x] 6. **Busy edges.** `standalone_task.cpp` `op_task`: on the existing `s_was_busy` edges call `status_busy(true/false)`. Device build green. (Device only.)
- [x] 7. **External-failure classification (TDD).** Tests first (`exec_net`/`net_queue` tests, and a new net-result test): a pure `NetFailureIsExternal(result)` — connect error, timeout, Wi-Fi down, net queue full(?) and HTTP status ≥ 400 → external; anything else not. Add an injected `report_error` callback (e.g. on `NetHost`/`ExecCtx`) so: synchronous net op errors (Wi-Fi down) report; Lua runtime errors, refused switches, bad params do **not** (they keep `OpStatus::Error` and get a clear `ESP_LOGE` line naming op/script and reason — check existing logs, add where missing). Tests assert which paths call the callback. `mk run` green.
- [x] 8. **Async net failures + device wiring.** `net_worker.cpp`: when a fire-and-forget (and waited) request finishes with an external failure (step 7 helper), call `status_error()`; `standalone_task.cpp` wires the step-7 callback to `status_error()`. Device build green. (Device only.)
- [x] 9. **Docs.** `docs/status-bar.md` (icon strip, placement, busy minimum, error rules, wake deadline, hosted shows Wi-Fi only), `docs/standalone.md` (busy/error indicator; what counts as an error), `docs/wifi.md` (icon), note in `docs/protocol.md` that icons are firmware-owned (no protocol change). Propose (don't apply without the user) CLAUDE.md updates: new module `status_icons`, the busy follow-up item resolved.
- [x] 10. **Final builds and tests.** `mk run` and `mk config=release run` (counts match), `idf.py --preset debug build` and `--preset release build` 0 errors; `host/` `npm test` still green (ui.bin unaffected there, sanity only).
- [ ] 11. **Hardware check (user).** Flash debug or release, layout with net ops (`examples/net-test.json` / `activity-test.mjs` with the test server):
  - [ ] a. Boot: a grey dot just left of the clock (clock at the right margin); white Wi-Fi waves once connected, about as bright as the clock.
  - [ ] b. Turn the AP off (or the test Wi-Fi): the dot returns; back on: waves.
  - [ ] c. HID tap key: the hourglass shows about 0.5 s, left of the Wi-Fi icon; noticeable when looked for, not distracting.
  - [ ] d. `delay` op / activity switch: busy stays for the whole run, clears after.
  - [ ] d2. A fire-and-forget HTTP request to a slow or unreachable host: the hourglass stays until it completes or times out (then the error icon if it failed).
  - [ ] e. HTTP to an unreachable host and a 404, fire-and-forget and waited (script `http.get`): red circle with white x for ~5 s, in the hourglass's place (replacing it while busy); a second failure restarts it.
  - [ ] f. Net op with Wi-Fi down: error mark shows.
  - [ ] g. A Lua runtime error: no error mark, clear `op failed`/lua line in the log.
  - [ ] h. Take control (`examples/test-server/takeover.mjs`): busy/error hidden, Wi-Fi icon stays; release: all back.
  - [ ] i. Stats log: 0 late fills while icons change; the clock stays at the right margin and the icons move with its width.
  - [ ] j. Judge the placeholder icons' look; swap SVGs in `tools/test-icons/gen.js` if wanted.
