# Frame rate experiment

Status: Complete (idea abandoned 2026-11-18; kept for reference)

## Conclusion

The dynamic frame rate idea (slow refresh at rest, full rate while swiping) is **abandoned**:
- **Pixel clock:** changes at runtime are clean, but the panel's floor is ~16 MHz (self-test colours below it), so a clock change alone only buys 48.8 -> 39 fps.
- **Frame size (vertical blanking):** the panel accepts any frame down to ~19 fps, but **every switch to a longer frame shows a visible jump**, and every switch back is clean. That holds for front and back porch alike, for steps as small as 1.1x (500 -> 550 lines), and for a 25-line-a-frame ramp. `lcd_update` itself is innocent (re-latching the same values never jumped), and it is required (unlatched writes never apply). Likely the panel's timing controller reacts when a frame runs past the previous frame's line count; not confirmed.
- **User's floor:** 32.5 fps is as low as the user would go at rest (borderline), so even a clean switch would only have bought ~1.5x.
- **Untried:** a longer *line* (horizontal front porch, `lcd_ht_width` 12 bits) keeping the line count fixed, like the clean clock change (would also grow the fill budget); a 1-line-a-frame ramp. VSYNC_END fires at the start of the sync pulse, after the front porch (phase 4).

## Description

Find out on the hardware whether the display's refresh rate can be changed at runtime, cleanly and with no visible artifacts, and how low it can go. This must be answered before more design decisions.

The idea: run a slow refresh at rest, and full rate (20 MHz pixel clock, ~49 fps) only while swiping, with no on-device key renders during a swipe. A slower clock gives each bounce fill a bigger budget (budget = 15 x 820 / MHz µs: 615 µs at 20 MHz, ~1230 µs at 10 MHz). That would end most of the fill-gap slicing pressure, and it saves some power for the battery build.

This is an **experiment only**: a build switch (off by default, like `CB_BACKLIGHT_CYCLE_S`) that changes the pixel clock at runtime and logs fill stats per rate. The user watches the screen. No design changes.

Questions:
1. Does `esp_lcd_rgb_panel_set_pclk()` switch cleanly (no tear, shift, roll, flash or lasting-shift parity break) going down and up?
2. What is the lowest clock without visible flicker? Try e.g. 16, 12, 10, 8, 6 MHz; also at low backlight-relevant content like dark and mid-grey areas.
3. How long from requesting a change until it applies (the next VSYNC_END)? Is switching up at touch-down fast enough that a swipe never starts slow?
4. Do the fill stats (`end max`, `missed`, seq breaks, buffer mismatches) stay clean across switches? Note the firmware's compile-time assumptions (`DISPLAY_BOUNCE_BUDGET_US`, compositor gap deadlines, `screen.cpp` fill timing) and what they report at other rates.
5. Optionally: Wi-Fi ping latency at the lower clocks (the RGB bus once interfered with Wi-Fi).

## Notes

- ESP-IDF v6.0: `esp_lcd_rgb_panel_set_pclk(panel, hz)` sets `need_update_pclk`; the RGB ISR applies the new divider in `lcd_rgb_panel_try_update_pclk()` at `LCD_LL_EVENT_VSYNC_END` (`C:\esp\v6.0\esp-idf\components\esp_lcd\rgb\esp_lcd_panel_rgb.c:476, 1209-1248`). The test app `test_apps/rgb_lcd/main/test_rgb_panel.c:211` uses it.
- Existing: `-DCB_PIXEL_CLOCK_MHZ=n` sets the clock at build time (`waveshare_rgb_lcd_port.h:17-37`). 18 and 16 MHz have never been tried; the panel's minimum clock is unknown.
- Swipe maths is time based (px/s), so speeds don't depend on frame rate.
- Terminology (proposed): **key render** = making a key image from layers; **page draw** = the compositor drawing key images into page buffers; **bounce fill** = the scan-out copy to the panel.

### Investigation (2026-11-17)

- **Driver:** `esp_lcd_rgb_panel_set_pclk()` only stores the requested Hz and sets `need_update_pclk` (under the panel spinlock). The LCD ISR, on `LCD_LL_EVENT_VSYNC_END`, first calls the user's `on_vsync` callback, then `lcd_rgb_panel_try_update_pclk()`, which computes the divider (`lcd_hal_cal_pclk_freq`) and writes it. So the new clock runs from the first VSYNC_END after the call: worst case one old frame (~20.5 ms at 20 MHz, ~68 ms at 6 MHz). There is no public getter for the rounded clock (`timings.pclk_hz` is private).
- **Rounding:** source is PLL_F160M (`LCD_CLK_SRC_DEFAULT` on the S3), pixel prescale `mo` = 2, so the divider is 80 MHz / f with a fractional part: 20 → 4, 16 → 5, 12 → 6 2/3, 10 → 8, 8 → 10, 6 → 13 1/3. All exact on paper; the experiment measures it anyway (from the VSYNC period).
- **Port** (`waveshare_rgb_lcd_port.cpp`): `s_panel` is the handle; only `on_bounce_empty` is registered in bounce mode. `DISPLAY_PIXEL_CLOCK_HZ` / `DISPLAY_BOUNCE_BUDGET_US` are compile time (`-DCB_PIXEL_CLOCK_MHZ`). Frame = `DISPLAY_H_TOTAL` 820 x `DISPLAY_V_TOTAL` 500 = 410,000 clocks.
- **Compile-time rate assumptions:**
  - `screen.cpp` `BUDGET_CYCLES` (= `DISPLAY_BOUNCE_BUDGET_US` x CPU MHz): the expected spacing of fills (`fill_start_late`'s grid: `s_expect_start += BUDGET_CYCLES`, restarted at line 30 each frame), and the `missed` / `overruns` judgement. At a slower clock the real spacing is longer, so with the fixed 20 MHz spacing every fill would look late by (real − 615 µs) and count as `missed`: **wrong at other rates**, so the experiment build makes it a variable (below).
  - `fill_gap.h` `FILL_GAP_PERIOD_US` = `DISPLAY_BOUNCE_BUDGET_US`: the compositor's deadline (next fill due 615 µs after this one's start, stop 65 µs before) and the status bar's freshness check. At a slower clock that's **conservative** (the gap is longer than assumed, so the writers just stop early): safe, left alone. Page builds get slower at lower rates anyway (fewer gaps per second, same work per gap). Never go *above* the compile-time clock with this unchanged.
  - `main.cpp`: the stats line prints `DISPLAY_BOUNCE_BUDGET_US`; the stress run's `frames_expected` uses `DISPLAY_PIXEL_CLOCK_HZ`. Not used by the experiment.
  - Swipe maths is time based; the swipe task is woken per frame, so a lower rate only means fewer, bigger steps.
- **Stats:** `screen_get_stats()` resets its maxima on each call; `diag_sample_second()` (main's loop, once a second) is its one reader. The experiment takes those per-second samples from main's loop (no second reader) and aggregates them per hold.
- **Seq breaks / buffer mismatches** (`screen.cpp` `fill_start_late`) don't depend on the rate: they show a driver resync or the lasting-shift parity flip if a switch causes one.

### Design (experiment build only, `-DCB_PCLK_CYCLE=ON` → `PCLK_CYCLE=1`)

- `display_timing.h` (pure, header only, host tested in `TestDisplayTiming.cpp`): fill spacing in µs and CPU cycles for any clock, fps x10, the clock from a measured frame period, the experiment's schedule (steps of `{ mhz, hold_s }`) and a per-hold stats accumulator.
- Port, all only with `PCLK_CYCLE` (normal builds are unchanged: same binary size): `display_set_pixel_clock(hz)` (calls `esp_lcd_rgb_panel_set_pclk` under the port's vsync spinlock) and an `on_vsync` callback that stamps each VSYNC_END (`esp_timer_get_time()`: per-core cycle counters can't be compared across cores) and, if a clock change is pending, marks it applied at that VSYNC (the driver applies it right after `on_vsync` in the same interrupt). `display_get_vsync()` reads count, last two stamps, applied Hz and when it applied.
- `screen.cpp`, only with `PCLK_CYCLE`: the fill spacing is a variable. `screen_set_pixel_clock(hz)` computes the new spacing in task context (pure helper), then requests the clock; the fill ISR adopts it at the grid restart (line 30, the first fill after VSYNC_END) once the port says that clock is applied. Normal builds keep the `BUDGET_CYCLES` constant (no change).
- `pclk_cycle.cpp/.h` (device): driven from main's 1 s loop with the per-second stats. At each step change: set the clock, poll every tick (10 ms, shorter than any frame) until the port reports it applied, log the latency (set call → applying VSYNC_END), the frame periods before and after it, and the measured clock; at the end of each hold, log the hold's fps (frames/elapsed from VSYNC stamps) and fill stats. Sets the status bar's left text to `pclk N MHz` so the screen shows the rate.
- **Latency method:** `t_set` = `esp_timer_get_time()` just before the set call; applied = the stamp of the first VSYNC_END that saw the pending change. A race (the set call landing between `on_vsync` and the driver's update in the same interrupt) would report it one frame late; the logged frame periods before/after show that (the "before" period would already be the new length).

### Phase 2 investigation (2026-11-17): a longer vertical blanking

- **Where:** `lcd_ll_set_vertical_timing(dev, vsw, vbp, active_height, vfp)` is in the `esp_hal_lcd` component, `C:\esp\v6.0\esp-idf\components\esp_hal_lcd\esp32s3\include\hal\lcd_ll.h:656` (`static inline`; `&LCD_CAM` is the device, `LCD_LL_GET_HW(0)`). `esp_hal_lcd` is a public requirement of `esp_lcd`, so `main` can include `hal/lcd_ll.h` with no CMake change. The driver calls it once, in `rgb_panel_init` (`esp_lcd_panel_rgb.c:610`); there's no runtime API.
- **Fields** (`soc/esp32s3/register/soc/lcd_cam_struct.h`): `lcd_ctrl2.lcd_vsync_width` (7 bits) = vsw − 1; `lcd_ctrl1.lcd_vb_front` (8 bits) = vbp + vsw − 1; `lcd_ctrl.lcd_va_height` (10 bits) = active − 1; `lcd_ctrl.lcd_vt_height` (10 bits) = vsw + vbp + active + vfp − 1. There is no VFP field: the front porch is whatever the total leaves after the active lines.
- **Hard limit: `lcd_vt_height` is 10 bits, so a frame is at most 1024 lines** (VFP ≤ 532 with our 4 + 8 + 480). 1500 and 2000 lines are impossible: the deepest slow-down from the porch alone is ~2.05x: 48.8 → 23.8 fps at 20 MHz, 39.0 → 19.1 fps at 16 MHz. (The horizontal total, `lcd_ht_width`, is 12 bits, up to 4096 clocks a line, so a longer *line* could go further; not tried: it changes the fill spacing and the HSYNC rate the panel sees.)
- **Latch:** `lcd_user.lcd_update` ("1: update LCD registers; cleared by hardware") is set by `lcd_ll_start` before start ("update parameters before start transaction") and by `lcd_ll_stop`. The TRM text doesn't say whether the timing registers need it while running; the clock (`lcd_ll_set_group_clock_coeff`, phase 1) didn't. The experiment writes the fields and then sets `lcd_update`, as `lcd_ll_start` does (flagged below). If the frame period doesn't change, the log says so (`not applied`/`old`).
- **When:** `LCD_LL_EVENT_VSYNC_END` ("LCD frame end") is the frame boundary: our `on_vsync` runs first in that interrupt, then the driver's pclk update and its `LCD_RGB_RESTART_IN_VSYNC` DMA restart. Writing the total there is safe both ways: going up can't wrap the line counter; going down (1000 → 500), the counter is at the frame boundary (a few lines in at most, or at the start of the front porch, ≤ 492 either way), below the new total. It matches how the driver applies pclk (same interrupt, right after `on_vsync`), so the existing "applied at the first VSYNC_END that sees it pending" bookkeeping covers both.
- **Bounce EOF parity (confirmed by reading the driver):** EOFs come only from the DMA finishing a bounce buffer, and the panel only takes data in active lines (DE). During the porch the DMA can only fill the small LCD/GDMA FIFOs, never finish a 24 KB bounce buffer, so a frame has 32 EOFs (480 / 15) whatever the porch; `bb_eof_count % 2` keeps its parity. The VSYNC restart (`lcd_rgb_panel_try_restart_transmission`) only resets FIFO/DMA and restarts at bounce buffer 0, as now. Our fill callback still gets y = 0, 15 (prefills of the next frame, at the end of the active lines), then after the porch 30, 45 ... 
- **`screen.cpp` stats stay correct with no change:** the fills within a frame keep their spacing (the line length and clock don't change), and the porch falls between the y = 15 and y = 30 fills, where the grid restarts (`fill_start_late`). So `late`, `end max`, `missed`, `overruns`, seq breaks and buffer mismatches mean the same at any porch. The fill-gap deadline (`FILL_GAP_PERIOD_US`) is untouched; the writers just get a long idle stretch per frame (fill_gap fires only after fills).
- **Side effect to note:** the frame is latched at the y = 0 fill, at the end of the previous frame's active lines, so a long porch adds up to ~21 ms (532 lines at 20 MHz) between the latch and the frame's first visible line. Swipe maths is time based; the swipe task is woken per frame, so fewer, bigger steps.
- **ISR safety:** `lcd_ll_set_vertical_timing` and `DisplayVfpForTotal` are inline; `on_vsync` is `IRAM_ATTR` but already calls `esp_timer_get_time`, and `CONFIG_LCD_RGB_ISR_IRAM_SAFE` is off. Fine unless a flash write lands at that moment (the experiment does none).

### Phase 2 design (`-DCB_VBLANK_CYCLE=ON` → `VBLANK_CYCLE=1`, and `PCLK_CYCLE=1` with it)

- A sibling switch on the phase 1 machinery: it turns `PCLK_CYCLE` on too (variable fill spacing, vsync stamps, `pclk_cycle.cpp`) and picks `VBLANK_SCHEDULE` instead of `PCLK_SCHEDULE`. `CB_PCLK_CYCLE=ON` alone still runs phase 1.
- `display_timing.h`: `DISPLAY_TIMING_V_TOTAL` (500; the port `static_assert`s it equals `DISPLAY_V_TOTAL`), `DISPLAY_V_TOTAL_HW_MAX` (1024), `DisplayVfpForTotal`, `DisplayTimingFpsX10` (fps from clock and lines), `DisplayVTotalForFpsX10` (lines for a target fps, clamped), `DisplayVTotalFromPeriods` (lines measured from VSYNC stamps at a known clock); `PclkStep` gained `lines` (default 500, so phase 1's schedule is unchanged); `VBLANK_SCHEDULE` / `VblankScheduleStep`.
- Port: `display_set_vertical_total(lines)` (500..1024, else `ESP_ERR_INVALID_ARG`) stores a pending total; `on_vsync` writes it (`lcd_ll_set_vertical_timing` + `lcd_update`) and stamps it applied like a clock change; `display_vsync_t.v_total` is the total in effect.
- `pclk_cycle.cpp`: each step changes the clock or the lines (never both: the schedule test checks); logs both per switch and per hold; the status bar shows `20 MHz, vfp 532 lines, 23.8 fps`.
- Schedule (`VBLANK_SCHEDULE`, ~106 s a cycle, starting 10 s after boot): 20 MHz at 500, 600, 750, 900, 1024, 500 lines (48.8, 40.7, 32.5, 27.1, 23.8, 48.8 fps), then 16 MHz at the same (39.0, 32.5, 26.0, 21.7, 19.1, 39.0 fps), 8 s each; then 20 MHz toggling 500 ↔ 1000 lines (48.8 ↔ 24.4 fps) every 1 s, 10 switches. The 16 MHz fps overlap the 20 MHz ones (32.5 fps both ways), which tells a VSYNC-rate limit from a line-rate one.

### Phase 3 investigation (2026-11-17): the display timing as a stored setting

Goal: the user sets the clock and the frame size by hand (no reflash), it survives a reboot, so they can live with e.g. 32.5 fps (20 MHz, 750 lines) and judge the swipe feel.

- **Pattern to follow:** the device name (`device_name.h/.cpp`: a pure settings class over an injected `BlobStorage`, NVS through `nvs_blob.cpp` on the device; `SettingResult`; a changed callback), `Protocol::Init` takes the settings objects (one call site in `host_link.cpp`, one in `TestProtocol.cpp`'s fixture), the host's `encodeNameSet`/`decodeName` + `ControlBlock.setName/getName`, FakeDevice's `name` option, and the CLI's `name` command.
- **NVS is up before the display:** `app_main` runs `init_nvs()` and `clock_init()` before `screen_init()`, so the stored timing can go straight into the panel config (`esp_lcd_rgb_panel_config_t.timings.pclk_hz` / `vsync_front_porch`) when the driver is created. **That is the least visible way to apply it at boot** (no runtime switch, so no one-off VFP jump; the driver sets the vertical timing once at init anyway).
- **What a normal build must now have** (phases 1-2 had them only under `PCLK_CYCLE`): the port's runtime clock and frame-size change (`on_vsync` writing the LCD_CAM vertical timing; `esp_lcd_rgb_panel_set_pclk`), the clock in effect for the fill (`display_pixel_clock_hz`), and `screen.cpp`'s variable fill spacing (the clock can change). The VSYNC stamps (`display_get_vsync`) stay experiment only.
- **Limits:** clock 16 MHz (the panel's floor, phase 1) to the build's clock (`DISPLAY_PIXEL_CLOCK_HZ`, 20; `fill_gap.h`'s deadline assumes it, so never above); lines 500 (`DISPLAY_V_TOTAL`) to 1024 (`DISPLAY_V_TOTAL_HW_MAX`). If a build's clock is below 16 (`-DCB_PIXEL_CLOCK_MHZ`), the minimum is the build's clock.
- **Old firmware:** an unknown type gets `ST_UNKNOWN_TYPE` (`protocol.cpp` default); the host turns that into a clear error.
- **Stress run / render lab:** `frames_expected` and the logged budget use the build's clock; with a stored timing the device runs at it. Noted (flagged), not changed.

### Phase 3 design

- **`display_timing.h`** (pure): `DisplayTimingLimits` (`DisplayTimingLimitsFor(build_mhz)`: default/min/max clock and lines), `DisplayTimingValid`. Header comment no longer "experiment only".
- **`display_config.h/.cpp`** (pure, tested in `TestDisplayConfig.cpp`): `DisplayConfig` like `DeviceName`: `Init(storage, limits)`, `Set(mhz, lines)` (0 in a field = that field's default; out of range = `BadArg`; the defaults erase the blob; unchanged = no write), `Get()`, `IsDefault()`, `Limits()`, changed callback. Blob: u8 version 1, u8 mhz, u16 lines (LE). A stored timing outside this build's limits (another clock) is dropped and erased at load.
- **Protocol** (`PROTO_VERSION` stays 2: new requests only): `0x70 DISPLAY_SET { u8 pclk_mhz, u16 v_total }` (exactly 3 bytes; 0 = default per field; `BAD_ARG` out of range), `0x71 DISPLAY_GET` -> `u8 pclk_mhz, u16 v_total, u8 flags (bit 0: default, nothing stored), u8 default_mhz, u16 default_v_total, u8 min_mhz, u8 max_mhz, u16 min_v_total, u16 max_v_total, u16 h_total, u16 fill_lines` (17 bytes; the host works out fps and the fill budget from them, nothing hard-coded). Allowed in both modes. Applied at the next VSYNC through the changed callback.
- **Device:** port `display_set_timing(hz, lines)` (before init: the timing the panel starts with; after: requested, applied at the next VSYNC_END; only what differs is changed), replacing the experiment's `display_set_pixel_clock` / `display_set_vertical_total`; `screen_set_timing(hz, lines)` replaces `screen_set_pixel_clock` (spacing first, then the port); `display_settings.cpp/.h` (device glue: loads `DisplayConfig` from NVS namespace `cb_display`, gives the boot timing to the screen before `screen_init`, applies changes). `pclk_cycle.cpp` uses `screen_set_timing` and starts from the timing in effect.
- **Host:** `encodeDisplaySet`, `decodeDisplayTiming` (adds `fps`, `fillBudgetUs`), `displayLinesForFps`; `ControlBlock.getDisplayTiming()`, `setDisplayTiming({ clockMhz, lines })` (both or either; an omitted one is the device's default; checks the range from `getDisplayTiming` first and throws `RangeError`), `resetDisplayTiming()`. An `UNKNOWN_TYPE` reply becomes "the device's firmware doesn't support display timing (needs v0.2)". FakeDevice `display` option.
- **CLI:** `display [--clock MHZ] [--lines N | --fps F] [--reset] [--serial S]`: no option shows it; `--clock` alone keeps the lines, `--lines` alone keeps the clock, `--fps` picks the lines at the (given or current) clock. Uses only `host/browser.js` (lazy loading test covers it).

### Phase 4 investigation (2026-11-18): a clean change of the frame size

- **Why:** phase 2's line changes showed a jump, but phase 1's clock changes (20 <-> 16 MHz, which also change the frame period) were clean. Both are applied in the same VSYNC_END interrupt. The difference is that the driver's clock update (`lcd_rgb_panel_try_update_pclk`, `esp_lcd_panel_rgb.c:1209`) never sets `lcd_update`, while our `set_vertical_total` does (flagged in phase 2). Second idea (user): grow the **back porch** instead of the front.
- **The back porch's limit:** `lcd_vb_front` (vsw + vbp - 1) is an **8-bit** field (`lcd_cam_struct.h:279`), so vsw + vbp <= 256: with the 8-line front porch, at most **744 lines** (32.8 fps at 20 MHz; 750 = 32.5 fps would need the front porch at 14).
- **DE is wired** (GPIO5), so the panel should take the first DE line as row 0: a longer back porch shouldn't move the picture (if it does, the panel counts lines from VSYNC).
- **Where VSYNC_END fires** is unknown. The driver's comment (`esp_lcd_panel_rgb.c:1123`) says the interrupt "has the entirety of the VBlank time", which suggests the end of the active lines. If it fires after the sync pulse instead, shortening the back porch there could cut under the line counter. Measured: the time from VSYNC_END to the y = 30 fill (bounce buffer 0's EOF, once lines 0-14 are out) is the blanking after VSYNC_END plus ~15 lines.
- **The driver restarts the DMA in every VSYNC_END** (`LCD_RGB_RESTART_IN_VSYNC`: `lcd_rgb_panel_try_restart_transmission`, FIFO and GDMA reset), but never calls `lcd_ll_start`, so nothing else sets `lcd_update` while running. Without it, the vertical registers may not apply at all (the log's frame periods say: `after ... (old)`), or only some fields may.

### Phase 4 design (`-DCB_PORCH_CYCLE=ON` -> `PORCH_CYCLE=1`, and `PCLK_CYCLE=1` with it)

- `display_timing.h` (pure, tested): `DISPLAY_PORCH_BACK` / `DISPLAY_PORCH_NO_LATCH` (variant bits; 0 = phase 2's front porch, latched), `DISPLAY_VB_FRONT_HW_MAX` 256, `DisplayPorchesForTotal(total, vsw, vbp, vfp, v_res, back)`, `DisplayVTotalMaxBackPorch(vfp, v_res)` (744), `DisplayLinesFromUs`, `PclkStep.variant`, `PORCH_SCHEDULE`.
- Port: `set_vertical_total` writes the porches for the variant (normal builds: always variant 0, as before), `lcd_update` unless `NO_LATCH`; a back porch that won't fit falls back to the front. Experiment only: `display_set_porch_variant()`, and the fill ISR stamps the y = 30 fill against the last VSYNC_END (`display_vsync_t.vsync_to_y30_us`).
- `pclk_cycle.cpp`: sets the variant before each step; a step that changes only the variant (at 500 lines, where all variants write the same registers) switches nothing; logs the porches and variant per switch, and per hold also `VSYNC_END to y30 fill N lines`. Status bar: `back unlatched 744 ln vbp 252 vfp 8 32.8 fps`.
- `CMakeLists.txt`: `PCLK_CYCLE=1` if any of `CB_PCLK_CYCLE`, `CB_VBLANK_CYCLE`, `CB_PORCH_CYCLE`.

## Flagged assumptions

- Status bar left text is used to show the rate (overwrites the standalone left text until the next activity switch). Harmless for an experiment.
- The fill-gap deadline (`FILL_GAP_PERIOD_US`) stays at the 20 MHz value: conservative below 20 MHz; the experiment never goes above the build's clock.
- Phase 2: the frame tops out at 1024 lines (hardware field width), not the 1500/2000 asked for; the schedule's deepest step is 1024 (19.1 fps at 16 MHz). Not a blocker: it still covers the 29-39 fps gap phase 1 left open and goes below it.
- Phase 2: `lcd_update` is set after writing the vertical timing (as `lcd_ll_start` does). Unknown whether the S3 needs it while running or whether it glitches the output; the switch log's frame periods show whether the change applied, and the user watches for a glitch at each switch. If it glitches, try without it (one line in `set_vertical_total`, `waveshare_rgb_lcd_port.cpp`).
- Phase 2: a sibling switch (`CB_VBLANK_CYCLE`) rather than a new mode of `CB_PCLK_CYCLE`, so phase 1 stays runnable as it was.
- Phase 3: the boot applies the stored timing by creating the panel with it (no runtime switch), chosen as the least visible. The log's `Install RGB LCD panel driver` line now shows the clock, lines, fps and budget in use (and the build's), and `display: stored timing: ...` when one is stored.
- Phase 3: `DISPLAY_SET` takes 0 per field as "the build's" (both 0 = reset). The library's `setDisplayTiming({...})` follows that (an omitted field is the default); the CLI keeps the current value for an option not given (it reads first). The defaults (also when spelled out) aren't stored.
- Phase 3: `DISPLAY_GET` reports the timing **set** (in effect from the next frame), not one measured from VSYNC stamps (those stay experiment only).
- Phase 3: a stored timing outside a later build's limits (e.g. a build with another `CB_PIXEL_CLOCK_MHZ`) is dropped and erased at boot.
- Phase 3: `RUN_STRESS` / `RENDER_LAB` still assume the build's clock (`frames_expected`, the logged budget): reset the timing (`display --reset`) before a stress run. Not enforced.
- Phase 3: the stats line (`STATS_LOG`) and the stress summary still print `DISPLAY_BOUNCE_BUDGET_US` (the build's budget); the `missed`/`over` judgements use the spacing in effect.
- Phase 3: the user docs (getting-started, hosted-mode, cli/README) are marked "(v0.2, unreleased; experimental)"; the README's limitations list is unchanged (not a limitation).
- Phase 3: `display_set_timing` changes only what differs from the last request, so the experiment's one-change-a-step schedule still produces one kind of switch per step.
- Phase 3: FakeDevice without its `display` option answers the display messages `UNKNOWN_TYPE` (firmware from before them); with it, it emulates a 20 MHz build (`DISPLAY`, checked against the firmware headers).

## Plan
- [x] 1. Investigate (Notes above).
- [x] 2. Tests first: `firmware/test/TestDisplayTiming.cpp` for `main/display_timing.h` (spacing, fps, clock from period, schedule, hold accumulator).
- [x] 3. `display_timing.h`; port `display_set_pixel_clock` + `PCLK_CYCLE` vsync stamps; `screen.cpp` variable spacing under `PCLK_CYCLE`; `pclk_cycle.cpp/.h`; main loop hook; `CB_PCLK_CYCLE` in `main/CMakeLists.txt`.
- [x] 4. Build: `mk run` debug + release (equal counts); firmware debug + release with the switch off, and on; leave both caches OFF.
- [x] 5. HW: user runs it (see *Hardware run* below).

### Phase 2: longer vertical blanking at runtime (approved 2026-11-17)
- [x] 6. Investigate (Notes: *Phase 2 investigation*, *Phase 2 design*).
- [x] 7. Tests first: `TestDisplayTiming.cpp` (front porch for a total, fps from clock and lines, lines for a target fps, lines from measured periods, phase 1's schedule keeps 500 lines, `VBLANK_SCHEDULE`: one change a step, 16-20 MHz, 500..1024 lines, toggles).
- [x] 8. `display_timing.h` maths and `VBLANK_SCHEDULE`; port `display_set_vertical_total` (written in `on_vsync`, `v_total` in `display_vsync_t`); `pclk_cycle.cpp` steps either schedule and logs clock and lines; `CB_VBLANK_CYCLE` in `main/CMakeLists.txt`. `screen.cpp` unchanged (Notes).
- [x] 9. Build: `mk run` debug + release (equal counts); firmware debug + release with the switches off and on; leave the caches OFF.
- [x] 10. HW: user ran it (*Phase 2 hardware results*) (see *Hardware run, phase 2* below).

### Phase 3: display timing as a stored setting (approved 2026-11-17)
- [x] 11. Investigate and design (Notes: *Phase 3 investigation*, *Phase 3 design*).
- [x] 12. Tests first, firmware: `TestDisplayTiming.cpp` (limits), `TestDisplayConfig.cpp` (blob, defaults, set, range refusals, load of an out-of-range blob), `TestProtocol.cpp` (DISPLAY_SET/GET raw bytes, refusals, standalone).
- [x] 13. Firmware pure code: `display_timing.h` limits, `display_config.h/.cpp`, protocol `MSG_DISPLAY_SET/GET` + `Protocol::Init` takes the `DisplayConfig`; `test.mk.js`. `mk run` debug + release.
- [x] 14. Firmware device layer: port `display_set_timing` (always built; boot timing into the panel config), `screen_set_timing` (variable spacing always), `display_settings.cpp/.h`, `main.cpp` and `host_link.cpp` wiring, `pclk_cycle.cpp` on the new calls. Build debug + release (and `CB_VBLANK_CYCLE=ON` release), caches left OFF.
- [x] 15. Tests first, host: `protocol.test.js` (encode/decode, lines for fps), `ControlBlock.test.js` (get/set/reset against FakeDevice, range errors, old firmware), FakeDevice `display`; then `protocol.js`, `ControlBlock.js`.
- [x] 16. Tests first, CLI: `args.test.js`, `cli.test.js` (show, set, keep the other field, --fps, --reset, old firmware), `lazy-child.mjs` runs `display`; then `args.js`, `cli.js`. Root `npm test`.
- [x] 17. Docs: `docs/protocol.md` (requests, history), `docs/guide/hosted-mode.md`, `cli/README.md`, `docs/guide` mention, marked "(v0.2, unreleased)" and experimental; CLAUDE.md proposals in Notes.
- [x] 18. HW: user used it (settled on 32.5 fps as the borderline floor; the full checklist was not run) (see *Hardware run, phase 3* below).

### Phase 4: front vs back porch, with and without lcd_update (approved 2026-11-18)
- [x] 19. Investigate and design (Notes: *Phase 4 investigation*, *Phase 4 design*).
- [x] 20. Tests first: `TestDisplayTiming.cpp` (porches for a total either way, the 744-line back porch limit, lines from us, `PORCH_SCHEDULE`: 20 MHz, 500/744 only, variants change only at 500 lines, >= 8 switches each).
- [x] 21. `display_timing.h`, port (variant, y30 stamp), `pclk_cycle.cpp/.h`, `CB_PORCH_CYCLE`. `mk run` debug + release: 852 each. Firmware: release with `CB_PORCH_CYCLE=ON` (app 0x12fac0), debug with it OFF (0x141dd0); no new warnings.
- [x] 22. HW: user ran it (*Phase 4 hardware results*). Visual: unlatched never jumped (nothing applied); front and back latched both "often jump, but not always" (hard to tell which).

### Phase 5: does the latch alone cause the jump? (approved 2026-11-18)
- [x] 23. Tests first: the porch schedule test rewritten for phase 5 (blocks: latch only, front, back; `PORCH_BLOCK_EVENTS` 10 each; 6 s rest before each; variants change only at 500 lines; latch only never changes the lines).
- [x] 24. `DISPLAY_PORCH_RELATCH`, `PORCH_SCHEDULE` replaced (phase 4's is in this doc), port `display_relatch()` (the same values rewritten and latched at the next VSYNC_END, counted as applied), `pclk_cycle.cpp` (re-latch events, `#n/10` counter on the status bar and in the switch log). `mk run` debug + release 852; firmware release `CB_PORCH_CYCLE=ON` (0x12fc40), debug normal (0x141dd0, unchanged).
- [x] 25. HW: user ran it (2026-11-18): **latch only 0/10, front 5/10, back 5/10, every second switch** (one direction only; which one not yet known). So `lcd_update` is innocent, the porch doesn't matter, and the jump comes from the change of frame length in one direction. Phase 1's clock changes (20 <-> 16 MHz, a 1.25x period change) were clean; 500 <-> 744 lines is 1.49x: the panel may tolerate only small steps (next: step-size sweep and a gradual ramp).
  - Phase 5's schedule (replaced by phase 6's): a 6 s rest then 10 re-latches of the same 500 lines (`latch only`), then the same rest and 10 switches 500 <-> 744 with the front porch, then with the back porch; 2 s apart.

### Phase 6: step-size sweep and a ramp (approved 2026-11-18, option 3)
- [x] 26. Tests first: `DisplayRampNext` (a step a frame toward the target, never past it; 500 -> 744 in 10 frames at `DISPLAY_RAMP_LINES_PER_FRAME` 25), the schedule test rewritten (rests at 500 lines; blocks of 10 switches to 550, 600, 650, 700 in one step, then 744 ramped).
- [x] 27. `DISPLAY_PORCH_RAMP`, `DisplayRampNext`, `PORCH_SCHEDULE` replaced; port: a lines change in ramp mode sets a target that `on_vsync` approaches 25 lines a frame (each step written and latched; counted as applied at the last step); `pclk_cycle.cpp` names the blocks `step` / `ramp`. `mk run` debug + release 853; firmware release `CB_PORCH_CYCLE=ON` (0x12fe50), debug normal (0x141dd0, unchanged).
- [x] 28. HW: user ran it (see *Hardware run, phase 6* below). First report (2026-11-18): **the odd events jump**, i.e. the switch to the longer frame; going back to 500 lines is clean. Then: **none of the odd events was clean, in any block**: even 500 -> 550 (1.1x) and the ramp (25 lines, ~5% a frame) jump. Every switch back to 500 is clean.
  - **Reading:** a longer frame always jumps, a shorter one never does, and a longer frame from a lower clock (phase 1, 20 -> 16 MHz, 1.25x) never did. The clock change keeps the lines a frame and the clocks a line; a lines change doesn't. Likely the panel's timing controller counts lines (HSYNCs) per frame and misbehaves for one frame when one runs past the last frame's count (an early frame is fine). Not confirmed. Next options (1-3-1 to the user): a much finer ramp (1-5 lines a frame, to find any tolerance), or a longer *line* (horizontal front porch, `lcd_ht_width` 12 bits) so the line count never changes, like a clock change (it also grows the fill budget, like a slower clock).

## Hardware results (2026-11-17, user)

- **16 MHz (~39 fps): fine.** No flicker, no tearing or other artifacts. 20 MHz likewise.
- **Below 16 MHz (12, 10, 8, 6): the panel shows a solid colour, a different one per clock (black, red, blue, white).** This looks like the panel's timing controller rejecting the signal and falling back to its built-in self-test colours, not a driver fault. The panel's minimum lies between 12 and 16 MHz (or a minimum frame rate between ~29 and ~39 fps, if it checks VSYNC rather than DCLK).
- **20↔8 MHz toggles:** app → black → app. The picture comes back intact at 20 MHz: no lasting shift seen.
- The `pclk:` log lines weren't pasted (switch latency not recorded); the visual result answers the main question.

**Answer:** changing the clock at runtime is clean, but the panel's floor is ~16 MHz, so a pixel clock change only buys 20 → 16 MHz (fill budget 615 → 769 µs, 49 → 39 fps). A deep slow-down at rest can't come from the pixel clock.

**Untried alternative:** keep the clock at 16-20 MHz and lengthen the vertical blanking (front porch) at rest, e.g. 500 → 1000+ total lines to halve the frame rate. The DCLK stays in range; whether the panel accepts a lower VSYNC rate is the open question. During blanking no fills run, so PSRAM is free for long stretches. The ESP32-S3 sets it with `lcd_ll_set_vertical_timing()` (called once at init, `esp_lcd_panel_rgb.c:610`); IDF has no runtime API, so it would mean writing the LCD_CAM registers in the VSYNC callback (a driver-level hack).

## Build results (2026-11-17)

- `mk run`: 826 tests pass in debug and release (7 new `DisplayTiming` tests).
- Firmware: debug and release build with `CB_PCLK_CYCLE` OFF and ON (no new warnings). With it OFF the debug app is the same size as before the change (0x141440). Both caches left with `CB_PCLK_CYCLE=OFF`.
- Not flashed (step 5 is the user's).

## Hardware run

Power from the native USB port (as for Wi-Fi work) as well as the UART port. The stored layout (Home) is the test page; the status bar's left text shows the current clock (`pclk N MHz`).

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
cd C:\Users\Brad\Projects\ControlBlock\firmware
idf.py --preset release -DCB_PCLK_CYCLE=ON -p COM4 flash monitor
```

(Optionally the same with `--preset debug` afterwards: fills are slower under `-Og`, so the fill stats have less margin.)

Schedule (`display_timing.h` `PCLK_SCHEDULE`), starting 10 s after boot, ~67 s a cycle, repeating:
1. Steps, 8 s each: 20 → 16 → 12 → 10 → 8 → 6 → 20 MHz (~48.8, 39.0, 29.3, 24.4, 19.5, 14.6, 48.8 fps).
2. Toggle, 1 s each: 8 ↔ 20 MHz, 11 switches.

What to watch (let it run at least two cycles):
- **Flicker** at each step: look at dark areas and mid-grey areas (key faces, the black background next to them) and the status bar text. Note the lowest clock that looks steady.
- **Swipe** between pages at each 8 s rate: does it feel smooth enough, any tearing?
- **At each switch**, especially the toggle phase: a tear, roll, flash, a one-frame glitch, or a picture that stays shifted by a 15-line band (the lasting-shift parity break).
- Optional: `ping` the device's IP during the steps (Wi-Fi latency at the lower clocks).

Log lines (tag `pclk`):
- Per switch: `20 -> 12 MHz: applied X ms after the call (VSYNC n after it); frame before A us (old), after B us (new) = pclk 12.000 MHz`. "applied" is the first VSYNC_END after the set call (expect n = 1 and X ≤ one old frame); `before`/`after` should read `old`/`new` (a `new` before means the switch landed one frame earlier than reported; `neither` means the period isn't either clock).
- Per hold: `hold 12.000 MHz: 8 s, N frames, F fps, measured pclk M MHz | fill max, end max E/B us, missed, over | seq breaks, buffer mismatches`. B is the fill spacing at that clock; the hold's stats include the switch into it. Expect 0 missed, 0 buffer mismatches; seq breaks only if a switch made the driver resync.

Paste back: every `pclk:` line from at least two cycles, any `LCD underrun` / error / warning lines, and what you saw (flicker per rate, switch artifacts, swipe feel).

Turn it off afterwards (the switch sticks in the CMake cache):

```powershell
idf.py --preset release -DCB_PCLK_CYCLE=OFF -p COM4 flash
idf.py --preset debug -DCB_PCLK_CYCLE=OFF build      # if the debug build was used
```

## Build results, phase 2 (2026-11-17)

- `mk run`: 832 tests pass in debug and release (6 new `DisplayTiming` tests).
- Firmware: release with `CB_VBLANK_CYCLE=ON`, with `CB_PCLK_CYCLE=ON` alone (phase 1), and with both OFF; debug with `CB_VBLANK_CYCLE=ON` and both OFF. No new warnings (the `times is not implemented` linker warning was there before). Debug with both OFF is still 0x141440 (unchanged). The release cache had `CB_PCLK_CYCLE=ON` left from the phase 1 run; both caches are now `CB_PCLK_CYCLE=OFF`, `CB_VBLANK_CYCLE=OFF`.
- Not flashed (step 10 is the user's).

## Hardware run, phase 2

Power from the native USB port as well as the UART port. The stored layout (Home) is the test page; the status bar's left text shows the current timing (`20 MHz, vfp 532 lines, 23.8 fps`).

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
cd C:\Users\Brad\Projects\ControlBlock\firmware
idf.py --preset release -DCB_VBLANK_CYCLE=ON -p COM4 flash monitor
```

(Check for `Hash of data verified.`) Schedule, starting 10 s after boot, ~106 s a cycle, repeating:
1. 20 MHz, 8 s each: 500 → 600 → 750 → 900 → 1024 → 500 lines (48.8, 40.7, 32.5, 27.1, 23.8, 48.8 fps).
2. 16 MHz, 8 s each: the same lines (39.0, 32.5, 26.0, 21.7, 19.1, 39.0 fps).
3. Toggle at 20 MHz, 1 s each: 500 ↔ 1000 lines (48.8 ↔ 24.4 fps), 10 switches.

What to watch (at least two cycles):
- **Rejected:** a solid self-test colour (as below 16 MHz in phase 1) at any step: note the step (MHz and lines from the status bar or log). If 32.5 fps is rejected at both clocks, the panel checks the VSYNC rate; if only at one, something else.
- **Flicker** at the low rates, especially on dark and mid-grey areas (key faces, the black next to them) and the status bar text. Note the lowest rate that looks steady.
- **Switches,** especially the 1 s toggles: a tear, roll, flash, one-frame glitch, or a picture that stays shifted by a 15-line band.
- **Swipe** between pages at each 8 s step: smooth enough? Any tearing?

Log lines (tag `pclk`):
- Per switch: `20 MHz 500 lines (vfp 8) -> 20 MHz 750 lines (vfp 258): applied X ms after the call (VSYNC n after it); frame before A us (old), after B us (new) = pclk ... MHz at 750 lines, 750 lines at 20 MHz`. Expect `after ... (new)` and the measured lines equal to the asked ones. `after ... (old)`, or `not applied after 500 ms`, means the LCD_CAM didn't take the new timing.
- Per hold: `hold 20.000 MHz, 750 lines (vfp 258): 8 s, N frames, F fps (expect E), measured ... | fill max, end max E/B us, missed, over | seq breaks, buffer mismatches`. Expect F ≈ E, 0 missed, 0 buffer mismatches.

**Paste back:** every `pclk:` line from at least two cycles (all of them, from the `frame rate experiment (vblank)` line on), any `LCD underrun` / error / warning lines, and what you saw: which steps showed a solid colour, flicker per rate, switch artifacts, swipe feel.

Turn it off afterwards (the switch sticks in the CMake cache):

```powershell
idf.py --preset release -DCB_VBLANK_CYCLE=OFF -p COM4 flash
```

## Phase 2 hardware results (2026-11-17, user)

- **The panel accepts every step:** 20 MHz at 500/600/750/900/1024 lines (48.8/40.7/32.5/27.1/23.8 fps) and 16 MHz at the same (39.0/32.5/26.0/21.7/19.1 fps). No self-test colours and **no flicker at any rate**, down to 19.1 fps. So the panel's limit is the pixel clock (phase 1), not the frame rate.
- **Measured exactly as expected:** fps and clocks match to 0.1%. Every change applied at the first VSYNC after the call (1.3-33 ms), with frame periods confirming old and new. 0 missed, 0 over, 0 seq breaks, 0 buffer mismatches across both cycles and all 22 toggles (no lasting shift). Fill max 138-160 us, two one-off 344 us maxima (16 MHz 600 lines, 20 MHz 1000 lines), within budget.
- **But each VFP change shows a very noticeable jump/flicker on screen** (user). Clock-only changes at 500 lines (phase 1, 20<->16 MHz) showed no artifact. Likely the panel's timing controller reacting to a sudden change in frame length (re-sync, or a one-frame brightness step from the longer hold time); not confirmed.
- Swipe feel at the low rates wasn't reported.

## Phase 3 build results (2026-11-17)

- `mk run`: 848 tests pass in debug and release (16 new: `DisplayTiming` limits, `Display Config` blob/set/load, `Protocol Display` raw bytes).
- Root `npm test`: passes (host 305, cli 46 among them; the CB_SMOKE package test skipped as usual). New: protocol encode/decode and maths, `ControlBlock` get/set/reset, range refusals, old firmware, FakeDevice's `DISPLAY` checked against `display_timing.h`/`protocol.h`/the port; CLI args, `display` show/set/keep/--fps/--reset/errors, and `display` in the lazy-loading run.
- Firmware: debug and release build (normal), and release with `CB_VBLANK_CYCLE=ON` (the release cache had it ON from the phase 2 run). No new warnings. Both caches now `CB_PCLK_CYCLE=OFF`, `CB_VBLANK_CYCLE=OFF`. Release app 0x12eaf0.
- Not flashed (step 18 is the user's).

### Proposed CLAUDE.md updates (not applied: for the user to confirm)

- Firmware modules: add `display_config.cpp/.h` (pure, tested: the stored display timing, NVS `cb_display`, limits from `display_timing.h` `DisplayTimingLimitsFor`: 16 MHz..the build's clock, 500..1024 lines) and `display_settings.cpp/.h` (device: loads it before `screen_init`, so the panel starts with it; applies changes through `screen_set_timing`).
- Drivers bullet: `display_set_timing(hz, lines)` (port; before init = the panel's start timing; after = applied at the next VSYNC_END: the clock through `esp_lcd_rgb_panel_set_pclk`, the lines written to the LCD_CAM in `on_vsync`, which is now always registered). Note the pixel clock line ("20 MHz by default"): a stored timing can run it at 16-20 MHz and up to 1024 lines (down to ~19 fps); a change of the lines shows a one-off jump.
- `screen.cpp`: the fill spacing (`late`, `missed`, `overruns`) is a variable in every build now, adopted at the grid restart once the new clock is in effect; `fill_gap.h`'s deadline stays at the build's clock (so the stored clock must never exceed it).
- Communication: `DISPLAY_SET` 0x70 / `DISPLAY_GET` 0x71 (2026-11-17, experimental; `PROTO_VERSION` stays 2; old firmware answers `UNKNOWN_TYPE`, which the library turns into "doesn't support display timing").
- CLI list: `display` (with bake, render, upload, wifi, time, name, info).
- `RUN_STRESS`: reset the display timing before a stress run (its expectations assume the build's clock).
- Open Items: "Display timing setting: built and host tested, not run on the hardware" (until step 18 passes).

## Hardware run, phase 3

Power from the native USB port as well as the UART port. A normal release build (no experiment switches):

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
cd C:\Users\Brad\Projects\ControlBlock\firmware
idf.py --preset release -p COM4 flash monitor
```

(Check for `Hash of data verified.` At boot the log shows `Install RGB LCD panel driver: pixel clock 20.000 MHz, 500 lines (~48.8 fps), fill budget 615 us (build: 20 MHz, 500 lines)`.)

Then, from the repo root in another terminal:

1. `node cli/cli.js display` → `display: 20 MHz, 500 lines, 48.8 fps, fill budget 615 us (default)` and the limits line.
2. `node cli/cli.js display --lines 750` (or `--fps 32.5`) → 32.5 fps. Monitor: `display: timing set: 20 MHz, 750 lines (~32.5 fps)`. Watch the screen: the one-off jump at the switch (expected), then a steady picture. **Swipe between pages for a while: does 32.5 fps feel smooth enough?**
3. Reboot (press RESET, or unplug/replug). Monitor: `display: stored timing: 20 MHz, 750 lines` and the driver line with 750 lines. **Watch the boot: no jump** (the panel starts at 750 lines). `node cli/cli.js display` shows 750 lines (not `(default)`).
4. Try `--lines 600` (40.7 fps), `--lines 900` (27.1), `--lines 1024` (23.8), and `--clock 16` (keeps the lines; a clock-only change: expect no jump), `--clock 16 --lines 500` (39 fps). Swipe at each.
5. Refusals: `node cli/cli.js display --clock 21` and `--lines 1100` → exit 1 with the range; the timing stays.
6. Hosted mode still fine at a low rate: `node examples/demo.js` while at e.g. 750 lines (keys draw, swipe, presses).
7. `node cli/cli.js display --reset` → back to `(default)`; reboot and confirm it stays at 500 lines.

What to watch: the jump at each change of the lines (only then), any lasting shift or tearing, swipe feel per rate, page builds after a switch (fewer gaps a second at a lower rate: a page of keys draws a little slower), and the monitor for `LCD underrun`, errors or warnings.

**Report back:** the swipe feel at each rate (and the lowest you'd live with), whether the boot with a stored timing shows any jump, whether clock-only changes are clean, the `display:` and driver log lines, and any errors. Leave the timing at whatever you want to live with, or `--reset`.

## Phase 4 hardware results (2026-11-18, log only; the visual result is still to come)

- **`lcd_update` is required:** both unlatched ways never applied (every switch to 744 lines logged `after ... (old)`, holds measured 500 lines at 48.8 fps). So unlatched isn't an option, and the latch can't be what tells phase 1's clean clock changes from phase 2's jumpy line changes (the clock isn't a latched register).
- **VSYNC_END fires after the front porch, at the start of the sync pulse:** VSYNC_END to the y = 30 fill was 27 lines at 500 lines (4 pulse + 8 back porch + 15), still 27 at 744 with the front porch, and 271 at 744 with the back porch (4 + 252 + 15). So a front porch written there applies to the *next* frame's end, and a back porch to the one about to run (the counter is inside the 4-line pulse, below any new back porch, so shortening it is safe too). The driver's VSYNC restart therefore has only pulse + back porch (12 lines, ~490 us at 20 MHz) before the first active line, not the whole blanking as its comment suggests.
- **Both latched ways applied on the first VSYNC, frame periods exact** (old then new each time); 0 missed, 0 over, 0 seq breaks, 0 buffer mismatches throughout; fill max 138-182 us.

## Hardware run, phase 4

Power from the native USB port as well as the UART port. Reset any stored timing first (`node cli/cli.js display --reset`): the schedule assumes 20 MHz.

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
cd C:\Users\Brad\Projects\ControlBlock\firmware
idf.py --preset release -DCB_PORCH_CYCLE=ON -p COM4 flash monitor
```

(Check for `Hash of data verified.`) Schedule (`PORCH_SCHEDULE`), starting 10 s after boot, 80 s a cycle, repeating. Four ways of writing the change, in order; each starts with a 4 s rest at 500 lines, then toggles 500 <-> 744 lines (48.8 <-> 32.8 fps) every 2 s, 8 switches:
1. `front latched` (phase 2's way: expect the jump again, as the baseline)
2. `front unlatched`
3. `back latched`
4. `back unlatched`

The status bar shows the way and the timing (`back unlatched 744 ln vbp 252 vfp 8 32.8 fps`).

What to watch, for each way (at least two cycles):
- **A jump/flicker at the switches**, as in phase 2: none, the same, or different (e.g. only going up, or only going down).
- **The picture moving down** at 744 lines in the `back` ways (the panel counting from VSYNC rather than DE).
- Any lasting shift by a 15-line band, tearing, or a self-test colour.

Log lines (tag `pclk`):
- Per switch: `20 MHz 500 lines (vbp 8, vfp 8) -> 20 MHz 744 lines (vbp 252, vfp 8, back unlatched): applied ...; frame before A us (old), after B us (new) = ...`. **`after ... (old)` means that way didn't apply the new frame size** (expected possibly for the unlatched ones); `neither` means only part of it applied.
- Per hold: `... VSYNC_END to y30 fill N lines`. Where VSYNC_END fires: if at the end of the active lines, expect ~35 at 500 lines and ~279 at 744 (either porch); if after the front porch (at the sync pulse), ~27 at 500, ~27 at 744 front, ~271 at 744 back. (A line or two less is the DMA running ahead through the FIFO.)
- Per hold: the fill stats line as before (expect 0 missed, 0 buffer mismatches).

**Paste back:** every `pclk:` line from at least one full cycle (from the `frame rate experiment (porch)` line on), any `LCD underrun` / error lines, and per way: jump or not, picture moved or not.

Turn it off afterwards (the switch sticks in the CMake cache):

```powershell
idf.py --preset release -DCB_PORCH_CYCLE=OFF -p COM4 flash
```

## Hardware run, phase 5

Same build switch as phase 4 (the schedule changed), timing reset first (`node cli/cli.js display --reset`):

```powershell
idf.py --preset release -DCB_PORCH_CYCLE=ON -p COM4 flash monitor
```

Schedule, starting 10 s after boot, 78 s a cycle, repeating. Three blocks; each starts with a 6 s rest at 500 lines (`#0/10`), then 10 events, 2 s apart:
1. `latch only #n/10`: the same 500-line values rewritten and latched (`lcd_update`); the frame never changes.
2. `front latched #n/10`: 500 <-> 744 lines with the front porch.
3. `back latched #n/10`: the same with the back porch.

**Count the jumps per block** (e.g. "latch only 0/10, front 6/10, back 5/10"); one or two cycles is enough. If the latch-only block jumps too, `lcd_update` itself is the glitch; if it never does, the frame length change is.

The log as phase 4 (each switch now carries `#n`; a re-latch logs as `500 lines -> 500 lines (..., latch only #n)` with both frames `(new)`).

Turn it off afterwards: `idf.py --preset release -DCB_PORCH_CYCLE=OFF -p COM4 flash`.

## Hardware run, phase 6

Same build switch (the schedule changed), timing reset first (`node cli/cli.js display --reset`):

```powershell
idf.py --preset release -DCB_PORCH_CYCLE=ON -p COM4 flash monitor
```

Schedule, starting 10 s after boot, ~140 s a cycle, repeating. Five blocks, each after a 6 s rest at 500 lines (`#0/10`), 10 switches each:
1. `step #n/10 550 ln`: 500 <-> 550 lines in one step (48.8 <-> 44.3 fps, 1.1x), 2 s apart
2. `step ... 600 ln`: 500 <-> 600 (40.7 fps, 1.2x)
3. `step ... 650 ln`: 500 <-> 650 (37.5 fps, 1.3x)
4. `step ... 700 ln`: 500 <-> 700 (34.8 fps, 1.4x)
5. `ramp #n/10`: 500 <-> 744 (32.8 fps) over 10 frames (~0.3 s) each way, 3 s apart

Odd events (#1, #3 ...) go to the longer frame, even ones (#2, #4 ...) back to 500.

**Report per block:** jumps out of 10, and whether they come on the odd events (to the long frame) or the even ones (back to 500). E.g. "550: 0, 600: 0, 650: 3 odd, 700: 5 odd, ramp: 0".

Log: ramp switches log the frame before the last step as `(neither)` (it's a ramp step, not the old timing); that's expected.

Turn it off afterwards: `idf.py --preset release -DCB_PORCH_CYCLE=OFF -p COM4 flash`.
