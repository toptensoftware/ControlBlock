# Wi-Fi latency

Status: Implement

## Description
Pings from the ESP32 to the gateway take ~250 ms, where other machines on the same network
take ~1 ms. Find the cause and fix it, so standalone net ops (`httpGet`, `httpPost`,
`udpPacket`, `sendWol`) fire with low button-to-request latency. Candidate causes so far:
distance from the AP (weak signal), the Wi-Fi/lwIP code running from PSRAM (code and rodata
are mapped to PSRAM, and the display's bounce fill competes for it), or something else
(power save not taking effect, buffer/AMPDU settings, the AP itself).

**Resolved:** none of those. The cause was radio interference from the RGB panel's pins at
their default drive strength (2), most likely the 25 MHz PCLK. Drive 0 on all 20 panel pins
fixed it (pings 2-10 ms, no loss; the picture is fine; stress run clean).

## Handover (2026-11-05, the user is moving to another PC)
- **Where it stands:** Implement phase. Steps 1-5 done. Step 6 (pixel clock) in progress:
  20 MHz tried and fine (see Notes, "Step 6, 20 MHz"); 18 and 16 MHz not tried. **Open
  decision for the user:** make 20 MHz the default (change `DISPLAY_PIXEL_CLOCK_HZ` in
  `waveshare_rgb_lcd_port.h`, keep the `CB_PIXEL_CLOCK_MHZ` switch for further tests), or try
  18/16 first. Then step 7 (docs: propose the changes to the user before editing) and step 8.
- **Nothing is committed.** All of this job's changes are in the working tree of the old PC:
  `CLAUDE.md` (COM3 in the build example), `firmware/CMakePresets.json`,
  `firmware/main/CMakeLists.txt`, `main.cpp`, `screen.cpp`, `screen.h`,
  `waveshare_rgb_lcd_port.cpp/.h`, `wifi.cpp`, `firmware/sdkconfig.defaults`, the deleted
  `firmware/sdkconfig.wifi_iram`, and this (untracked) document. They must be committed and
  pushed (or copied) to reach the new PC.
- **Not carried over:** `build/` (gitignored: CMake caches with switches like
  `CB_WIFI_DIAG=ON` in `build/debug`, and `CB_PIXEL_CLOCK_MHZ` / `CB_RUN_STRESS` in
  `build/release` from the last trials; a fresh clone starts at the defaults: 25 MHz, stress
  off) and `scratch/wifi-baseline/` (gitignored; rebuild from Notes if needed). The board's COM
  port (COM3 here, also written into CLAUDE.md) may differ on the new PC.
- **Current firmware defaults:** panel drive 0 (permanent), pixel clock 25 MHz, Wi-Fi and lwIP
  on core 0, IRAM opts off. Build switches added by this job: `CB_NO_DISPLAY`, `CB_IDLE_FILL`
  (diagnostics, kept), `CB_PIXEL_CLOCK_MHZ` (experiment; empty value = default).

## Notes
- Spec: `docs/wifi.md`. Device layer: `firmware/main/wifi.cpp`. Settings: the Wi-Fi block in
  `firmware/sdkconfig.defaults`.
- **Symptom:** gateway ping ~250 ms from the ESP32, ~1 ms from other machines.
- **Diagnostics already built** (commit `13a325f`):
  - Every connect: `wifi.cpp` `log_link()` logs gateway, mask, RSSI and channel, then pings the
    gateway 4 times (`GATEWAY_PINGS_AT_CONNECT`), logging each time in ms.
  - `-DCB_WIFI_DIAG=ON`: the same RSSI line and a gateway ping every 5 s while connected
    (`WIFI_DIAG_PERIOD_MS`).
  - `-DCB_STATS_LOG=ON`: the once-a-second stats line with net counters.
  - Presets `debug-iram` / `release-iram` (`firmware/CMakePresets.json`): layer
    `sdkconfig.wifi_iram` over the defaults (`ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT`,
    `ESP_WIFI_EXTRA_IRAM_OPT`, `LWIP_IRAM_OPTIMIZATION`), own build dirs for comparison.
- **Power save:** `esp_wifi_set_ps(WIFI_PS_NONE)` is called right after `esp_wifi_start()`
  (`wifi.cpp`, `driver_up`). ~100-300 ms pings are the classic modem-sleep signature, so
  confirm it actually takes effect (e.g. read it back with `esp_wifi_get_ps` after connect).
- **IRAM history:** IRAM opts were tried earlier and `esp_wifi_init()` ran out of internal RAM
  (~20 KB free then). Since the status bar buffers moved to PSRAM there is ~120 KB free with
  Wi-Fi connected (`docs/wifi.md` Internal RAM), so they may fit now. Check the boot heap lines.
- **Current buffer settings** (smaller than defaults): static RX 6, dynamic RX/TX 16, AMPDU RX
  window 6, TX window 4. Wi-Fi/lwIP buffers stay in internal RAM
  (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` off). Driver and lwIP on core 0.
- **Run 1 (2026-11-05, `WIFI_DIAG` on, gateway pings from the device):**
  - AP `tts4_2g`, channel 11, BW20, WPA2-PSK, phy bgn. Beacon interval 102.4 ms, **DTIM 3**
    (so a DTIM every ~307 ms). The station advertises listen interval 3.
  - Board about 3 m from the AP. **RSSI -36 to -41 dBm** throughout: excellent. **Distance/signal is ruled out.**
  - **Power save is off:** the driver logs `Set ps type: 0` and `pm start, type: 0`
    (`WIFI_PS_NONE`). Modem sleep on our side is ruled out.
  - Ping times: 4, 97, 3, 74, 13 (connect), then one every 5 s: 138, 18, 674, 46, 228, 553,
    177 ms. **Very jittery rather than a steady 250 ms**: anywhere from 3 ms to 674 ms. The
    worst is about two DTIM periods (2 x 307 ms), which fits the AP holding frames for the
    station as if it were dozing (hypothesis, unconfirmed).
  - Internal heap with the driver up: 95,283 free (largest 55,296); connected 93,399
    (largest 52,224). That's ~25 KB less than the `docs/wifi.md` figures (120,715 / 118,839),
    which suggests this was an `-iram` preset build (see Flagged assumptions).
  - The 34,467 ms sample is missing (pings at 29,467 then 39,467): one diag period skipped.
- **Run 2 (2026-11-05, PC pings the device, `ping 10.1.1.137`):** 613, 330, 220, 75 ms
  (avg 309, no loss). **Just as slow and jittery as the device's own pings**, so the on-device
  ping task's scheduling is ruled out as the cause. The delay is in the radio path: the AP,
  the air, or the device's driver/lwIP layer (or something stalling them on core 0).
- **Run 3 (2026-11-05, same laptop on Wi-Fi via the same AP, pinging the wired NAS
  `10.1.1.2`):** 1, 1, 7, 4 ms. The laptop's own Wi-Fi hop through this AP is fast, so the AP
  isn't generally slow. Not yet known: whether the laptop is on the 2.4 GHz SSID (`tts4_2g`) or
  a 5 GHz one. If it's on 5 GHz, the AP's 2.4 GHz radio (or its handling of a bgn-only
  client) is still a suspect.
  The laptop is on `tts4_2g` too, so the AP's 2.4 GHz side is fine for other clients.
  **The board is the prime suspect.**
- **Baseline firmware:** `scratch/wifi-baseline/` (gitignored, builds clean). ESP-IDF's station
  example: no display, PSRAM, USB or CH422G, Wi-Fi settings at the defaults, power save off.
  Uses ControlBlock's `partitions.csv` and reads the credentials ControlBlock stored in NVS
  (`cb_wifi/cred`, never erases NVS), so the credentials and the stored config survive. Logs
  RSSI every 5 s and pings the gateway once a second. Flash with `idf.py -p COMx flash monitor`
  from that folder. Afterwards reflash ControlBlock with a full `idf.py flash`, which also puts
  back its bootloader.
- **Run 4 (2026-11-05, baseline firmware, PC pings the board):** 6, 2, 5, 6 ms. **Fast.** Same
  board, AP and spot, so the board and the AP are fine: **the cause is in ControlBlock's
  firmware**, either its build settings or what it does at run time (display fill ISR, PSRAM
  traffic, core 0 load, USB).
- **Next split, settings vs run time:** the baseline built with ControlBlock's
  `firmware/sdkconfig.defaults` (PSRAM octal 80 MHz, code and rodata in PSRAM, 64 KB data cache
  with 64-byte lines, QIO flash, our Wi-Fi buffers/BA windows, IRAM opts off, core 0
  affinity). Build dir `scratch/wifi-baseline/build-ours`. Unknown-symbol warnings for TinyUSB,
  touch, LCD and HTTP client are expected (those components aren't in the baseline). Flash:
  `idf.py -B build-ours -D "SDKCONFIG_DEFAULTS=../../firmware/sdkconfig.defaults" -D SDKCONFIG=build-ours/sdkconfig -p COMx flash monitor`.
  Slow = a setting (then bisect the settings); fast = run time (then switch off parts of
  ControlBlock: display fill first).
- **Run 5 (2026-11-05, baseline + ControlBlock's sdkconfig, PC pings):** first run 953, 29, 9,
  4 ms; second run 2, 9, 4, 4 ms. The 953 ms is the first ping after a reboot (ARP). **Fast.**
  **Our settings are fine: the cause is something ControlBlock does at run time.**
- **Lead: the bounce fill ISR shares core 0 with Wi-Fi.** `app_main` runs on core 0
  (`ESP_MAIN_TASK_AFFINITY_CPU0`) and `screen_init` creates the RGB panel from it, so the LCD ISR
  (and the fill) is on core 0 (`docs/page-compositor.md` and `screen-buffers.md` agree). The
  Wi-Fi driver task, its radio interrupt, lwIP and our protocol/ops/net/wifi tasks are on core 0
  too. The fill runs ~1,950 times a second at ~115 µs at rest (~22% of core 0) and up to
  ~360 µs mid-swipe (~70%), and it saturates PSRAM while it reads, while the Wi-Fi code runs
  from PSRAM (`SPIRAM_FETCH_INSTRUCTIONS`). Core 1 has the compositor (prio 4), touch (5) and
  swipe (10), mostly idle at rest.
  - `docs/wifi.md` ("Core 1 belongs to the compositor and the fill") is wrong about the fill:
    it's on core 0. Fix that doc when this job settles (propose to the user first).
  - Not shown yet that the fill is the cause. Other run-time suspects: TinyUSB, the
    once-a-second heartbeat, touch polling over I2C, our core 0 tasks.
- **Diagnostic switch `-DCB_NO_DISPLAY=ON`** (added 2026-11-05, option 1 of the 1-3-1; the user
  agreed): `screen_init()` skips `display_init_bounce()` (no panel, no fill ISR, no PSRAM reads
  by the fill) and `main.cpp` leaves the backlight off. The default lives in `screen.h`; the
  switch is in `main/CMakeLists.txt`. Everything else (USB, touch, compositor and swipe tasks,
  Wi-Fi) starts as normal; the compositor and swipe tasks just never get woken. Builds clean
  (debug). Hardware only, no unit test. Turn off with `-DCB_NO_DISPLAY=OFF` (it sticks in the
  CMake cache). If it stays as a permanent switch, add it to CLAUDE.md beside `RUN_STRESS` /
  `BACKLIGHT_CYCLE_S` (propose first).
  - Fast with it = the display fill is the cause; next try Wi-Fi on core 1 (option 2) as the fix.
  - Slow with it = not the display; next suspects USB, touch I2C polling, our core 0 tasks.
- **Run 6 (2026-11-05, ControlBlock debug with `CB_NO_DISPLAY=ON`, PC pings):** 2, 2, 5, 2 ms.
  **Fast. Confirmed: the display (the bounce fill on core 0) causes the Wi-Fi latency.** Still
  open: whether it's the CPU time on core 0 or PSRAM contention (Wi-Fi code runs from PSRAM
  while the fill saturates it). Run 1 may have been an IRAM build and was still slow, which
  points at CPU/interrupt time rather than PSRAM, but that build wasn't confirmed.
- **Option 2 built (2026-11-05, the user agreed):** `firmware/sdkconfig.wifi_core1` (the two
  settings below), presets `debug-core1` / `release-core1` in `CMakePresets.json`, and
  `WIFI_TASK_CORE` in `wifi.cpp` now follows `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1` (so the
  task that starts the driver is always on the driver's core). The default build is unchanged
  (core 0). `debug-core1` builds clean. The first attempt hit a one-off GCC internal compiler
  error (segfault in IDF's `esp_lcd_panel_rgb.c`); a rerun built fine. Waiting on the hardware
  ping test.
- **Run 7 (2026-11-05, `debug-core1`, display on, PC pings, three runs):** 254, 4, 146, 336 /
  48, 7, 140, 149 / 8, 86, 9, 4 ms (averages 185, 86, 26). **Better than core 0 (~300 avg) but
  still bad.** Core 0 CPU time was part of it; the rest is probably PSRAM contention: the fill
  saturates PSRAM while the Wi-Fi/lwIP code (and rodata, `SPIRAM_RODATA`) is fetched from it,
  whichever core runs it.
- **Next: `debug-core1-iram` preset** (core 1 + `sdkconfig.wifi_iram`), builds clean, all six
  settings confirmed in the generated sdkconfig. Check the boot heap lines (the IRAM opts cost
  internal RAM; run 1's ~95 KB free with the driver up is probably that). If still slow: the
  rodata and remaining driver code are still in PSRAM. Options then: keep more Wi-Fi/lwIP
  code and data out of PSRAM (linker fragments), or have the fill leave PSRAM gaps.
  The presets list is growing (6 experiment presets): prune once the job settles.
- **Run 8 (2026-11-05, `debug-core1-iram`, display on, PC pings, three runs):** 123, 136, 53,
  453 / 144, 133, lost, lost / 470, 227, 86, 1411 ms. **Worse, with packet loss.** So moving the
  Wi-Fi code out of PSRAM doesn't help either: the PSRAM code-fetch theory looks wrong.
  Caveat: 4 pings a run is a small sample against this much jitter; use `ping -n 50` for
  comparisons from now on.
- **New lead: radio interference from the panel.** The RGB panel clocks 16 data lines plus
  PCLK at 25 MHz (`DISPLAY_PIXEL_CLOCK_HZ`) next to the PCB antenna. Interference raises the
  noise floor, causing MAC retries and lower rates: latency jitter and loss even at -40 dBm, and
  independent of cores and PSRAM. Fits every run so far (display off = fast; any change to
  where Wi-Fi runs barely matters). This kind of interference is widely reported for ESP32-S3
  RGB panel boards (from memory, not checked in this job).
  - `aclock` (sister project, same board, same 25 MHz PCLK, driver-managed bounce from PSRAM
    framebuffers, IRAM opts on) uses Wi-Fi only for NTP, which wouldn't show latency.
- **Open question (1-3-1 put to the user):** interference from the panel signals vs the fill's
  ISR and PSRAM load. Proposed test: the panel running as normal but the fill doing nothing
  (no PSRAM reads, a near-empty ISR). Slow then = interference.
- **Heap with the IRAM opts (run 8 boot log):** before the driver 91,659 free (largest 51,200);
  driver up 59,103 (largest 31,744); connected 57,227 (largest 31,744). That's ~36 KB less than
  run 1 (95,283 with the driver up). **The IRAM opts cost too much internal RAM to keep.** It
  also means run 1 was the normal build, not an IRAM one (assumption resolved). The
  `docs/wifi.md` / CLAUDE.md RAM figures (~120 KB free with the driver up) are out of date: run 1
  shows ~95 KB, presumably since the net, ops and Lua work. Propose updating them when the job
  settles.
- **Diagnostic switch `-DCB_IDLE_FILL=ON`** (added 2026-11-05, option 1 of the second 1-3-1;
  the user agreed): `fill_lines()` returns at once (counts frames only), so the panel keeps
  clocking out the stale bounce buffers at 25 MHz with no PSRAM reads and almost no ISR time.
  Default in `screen.h`, switch in `main/CMakeLists.txt`. Built in `build/debug` (Wi-Fi on core
  0, as in run 1) with `-DCB_NO_DISPLAY=OFF -DCB_IDLE_FILL=ON`; builds clean. Both stick in the
  CMake cache. Slow = interference from the panel signals; fast = the fill's load.
- **Run 9 (2026-11-05, `debug` + `CB_IDLE_FILL=ON`, Wi-Fi on core 0, `ping -n 50`, 27 lines
  pasted):** 1551, 9, 621, lost, 11, 335, 385, 709, 797, 8, lost, 417, 88, 926, 1178, 1091, 1053,
  534, 215, 231, 10, 265, 32, 283, 1219, 600, 380 ms. **As bad as ever, or worse (two lost, many
  over 1 s), with the fill doing no work.** So it is not the fill's CPU or PSRAM load: **the
  running panel itself breaks Wi-Fi**, almost certainly radio interference from the RGB bus.
  - ~~Worse because of noisy stripes~~: the user says the screen was plain black, so the data
    lines were still. The noise must come from the always-running signals: PCLK (25 MHz),
    HSYNC, DE and VSYNC. PCLK is the prime suspect.
  - Remaining non-radio possibility (unlikely): the LCD's GDMA reading the bounce buffers
    disturbing the Wi-Fi MAC. Not separable without changing the panel signals.
- **Fix options (third 1-3-1, put to the user):** (1) lower the drive strength of the RGB
  panel GPIOs (`gpio_set_drive_capability`, default `GPIO_DRIVE_CAP_2`; slower edges carry much
  less energy at 2.4 GHz, no frame rate cost if the panel still latches cleanly); (2) lower
  `DISPLAY_PIXEL_CLOCK_HZ` (costs fps and changes the bounce budget); (3) no code: move the AP
  to channel 1 or 6, reorient the board, ferrite/shielding. Recommended: (1), with the channel
  swap from (3) as a free side test.
- **Option (1) built (2026-11-05, the user agreed): `-DCB_LCD_DRIVE_CAP=0..3`.**
  `waveshare_rgb_lcd_port.cpp` sets `gpio_set_drive_capability` on HSYNC, VSYNC, DE, PCLK and
  the 16 data pins right after `esp_lcd_new_rgb_panel()` (which configures the pins; checked
  in IDF's `esp_lcd_panel_rgb.c`), and logs a warning with the level. Unset = the driver
  default (2). The switch is in `main/CMakeLists.txt` (`-UCB_LCD_DRIVE_CAP` to remove).
  `build/debug` is now `CB_NO_DISPLAY=OFF`, `CB_IDLE_FILL=OFF`, `CB_LCD_DRIVE_CAP=0`; builds
  clean. Watch the panel for shimmer, wrong colours or a shifted image at the weak drive. If
  0 breaks the picture, try 1. If it helps but not fully, next try only PCLK weak and/or a
  lower pixel clock.
- **Run 10 (2026-11-05, `debug`, display on as normal, `CB_LCD_DRIVE_CAP=0`, Wi-Fi on core 0,
  `ping -n 50`, 34 lines pasted):** 193 (first ping), then 12, 11, 4, 4, 4, 5, 5, 4, 4, 8, 10,
  4, 3, 3, 15, 4, 3, 4, 5, 4, 5, 6, 7, 4, 4, 4, 11, 8, 5, 3, 3, 7, 4 ms. No loss. **Fixed: as
  good as with the display off.** Root cause: radio interference from the RGB panel's pins at
  the default drive strength (2), most likely the 25 MHz PCLK. Picture quality at drive 0 not
  yet reported. Not yet tried: drive 1, or weak drive on PCLK only.
- **Step 5, release build (2026-11-05):** `ping -n 50` at rest: 242 (first ping), then 2-10 ms,
  one 20 ms. While swiping, pings rise to 10-20 ms: the user is happy with that.
  Boot log: gateway pings #1-#4 at 18, 7, 5, 6 ms ("4 sent, 4 received, 36 ms in all"), so the
  sequence-number fix works and the device's own pings are fast too. Release heap (internal):
  before the driver 139,879 (largest 98,304); after init 129,751; driver up 107,715 (largest
  67,584); connected 105,831 (largest 64,512, min ever 103,119). (Run 1's ~95 KB was a debug
  build.) These are the figures for step 7's RAM update.
  `RUN_STRESS` (release, drive 0): **0 over budget in every case.** Fill max: at rest 122-126 µs,
  mid-swipe 291-296, status bar only mid-swipe 363, opaque mid flush + status 410, image
  replaces at rest 279 and mid-swipe 411 µs. Same as before the change (CLAUDE.md: 121 at rest,
  286-353 mid-swipe, 412 image replaces mid-swipe). Compositor: keys max ~2.5-3.0 ms, busy
  71-78% at rest, 36-39% mid-swipe.
- **Step 6, 20 MHz (2026-11-05, release, `RUN_STRESS`):** the user says it looks OK. Budget
  615 µs, **0 over in every case.** Fill max (25 MHz in brackets): at rest 120 (122), mid-swipe
  195-200 (291-296), status bar only mid-swipe 282 (363), opaque mid flush + status 327 (410),
  image replaces at rest 151 (279), mid-swipe 210 (411). **The fills themselves got faster**,
  not just the budget bigger (probably the preload has longer to finish between fills, so
  the next chunk is warmer). Worst case 327 of 615 µs (53%) against 411 of 492 (84%) at 25 MHz.
  Compositor throughput down ~17% (keys at rest 489-512 vs 610-615; image replaces 595 vs
  628), as expected from fewer gaps a second. Pings at 20 MHz not reported.
- **Board COM port:** COM3 (now in CLAUDE.md's build example).
- Option 2 design notes: Wi-Fi on core 1. Needs
  `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1=y`, `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y` and
  `WIFI_TASK_CORE 1` in `wifi.cpp` (esp_wifi_init runs on that task, so the radio interrupt
  follows). Try it as an overlay first (like `sdkconfig.wifi_iram`). Fast = fixed, but then
  run `RUN_STRESS` to check that Wi-Fi's PSRAM code fetches on core 1 don't make fills late. Slow =
  PSRAM contention: try it combined with the IRAM overlay.
- **Ways to tell those apart** (cheapest first):
  1. Same board, different AP: point it at a phone hotspot (`host/demo/wifi.js set`) and ping
     it from a device on the hotspot. Fast there means our AP is the problem (DTIM/buffering).
  2. Same AP, different 2.4 GHz client: ping a laptop or another ESP32 on `tts4_2g` from the PC.
  3. Same board and AP, without our firmware: flash the stock ESP-IDF station example
     (`examples/wifi/getting_started/station`, with `esp_wifi_set_ps(WIFI_PS_NONE)` added) and
     ping it. Fast means something in our firmware (display fill, PSRAM, core 0 load,
     buffer settings) is to blame; slow means the board or AP.
- **Diagnostic code bugs** (`wifi.cpp`, link diagnostics; cosmetic, the ms values are fine):
  - Ping sequence numbers print as garbage (`#2181890049` = `0x820D0001`):
    `ESP_PING_PROF_SEQNO` is a `uint16_t`, read into an uninitialised `uint32_t`, so the top
    bytes are stack junk. Same risk for the other profile reads. Fix: `uint16_t seq` (or
    zero-initialise).
  - At connect, two sessions ran at once (two `link:` lines at 4,457 and 4,467; the 4-ping
    connect session and a 1-ping diag session interleave). Overlapping sessions may skew times.
- **Task layout:** the ping callbacks run on the esp_ping task (default priority 2), and its
  timing includes that task's scheduling delay. Core 0 also has the protocol, ops, net and wifi
  tasks plus lwIP and the driver; core 1 has the compositor (`comp`), `swipe` (prio 10) and
  `touch` (prio 5). A starved low-priority ping task would inflate the numbers without the air
  being slow. Pinging the device from the PC tells the two apart: lwIP answers echo requests
  in its own thread, so neither the ping task nor our tasks are involved.
- All checks here are hardware only (the user flashes and reads the UART log).
- **Steps 1-4 done (2026-11-05):**
  - `DISPLAY_PIN_DRIVE_CAP` (`GPIO_DRIVE_CAP_0`) in `waveshare_rgb_lcd_port.h`, applied always
    in `waveshare_rgb_lcd_port.cpp` after `esp_lcd_new_rgb_panel()`; INFO log line
    `panel pins at drive capability 0`. `CB_LCD_DRIVE_CAP` is gone from `main/CMakeLists.txt`.
  - `idf.py` has no `-U` option: the stale `CB_LCD_DRIVE_CAP` entry was removed from
    `build/debug/CMakeCache.txt` by hand, then `idf.py --preset debug reconfigure build`.
  - Presets are back to `debug` and `release`; `sdkconfig.wifi_core1` (untracked) and
    `sdkconfig.wifi_iram` (tracked: shows as deleted in git) removed, plus `build/debug-core1`
    and `build/debug-core1-iram` (the other experiment dirs didn't exist). `wifi.cpp`'s header
    comment no longer claims the fill is on core 1.
  - Ping diagnostics: `uint16_t` SEQNO, all profile reads zero-initialised, the end line also
    gives the total ms. One session at a time via `s_ping_busy` (`port::AtomicCompareExchange`
    in `log_link`, cleared in `on_ping_end` or when starting fails): the diag ping right after
    connect is now skipped (its link line still logs).
  - `build/debug` still has `CB_WIFI_DIAG=ON` in its cache (left as it was). No
    `LCD_DRIVE_CAP`, `NO_DISPLAY=1` or `IDLE_FILL=1` in its compile commands. Debug and release
    build with 0 errors.

- **Step 6 audit: what assumes ~61 fps / 25 MHz** (2026-11-05). Frame = 820 x 500 px
  (H/V totals with porches), so fps = pclk / 410,000: 25 MHz 61.0, 20 MHz 48.8, 18 MHz 43.9,
  16 MHz 39.0. Budget = 15 x 820 / MHz: 492, 615, 683, 768 us.
  - `swipe.cpp`: **real time, not per frame.** The glide (spring `SPRING_RESPONSE_S`, or the
    ease-out's `SETTLE_*_S`) is evaluated at `esp_timer_get_time()` each frame; flick speed is
    px/s over an 80 ms sample window of time-stamped touches. A slower clock keeps the same
    speeds and durations, just in fewer, bigger steps (at 39 fps a fast glide moves ~1.6x as far
    per frame): judge smoothness by eye. No fix needed.
  - `touch_router.h` / `touch_gate` / `standalone` tick: thresholds in ms on time-stamped events;
    `Tick` runs once per frame, so the swipe-window end and long-press firing get the frame period
    as resolution (16 -> 26 ms at 16 MHz). Negligible. Touch polling is 100 Hz on its own task.
  - `page_compositor`: measures the frame period (`COMP_FRAME_PERIOD_DEFAULT_US` 16400 is only
    used until two frame starts have been seen); the swap guard and the chunk estimate follow the
    measured period. Fine.
  - Compositor slices (`COMP_SLICE_ROWS_*`) and status bar bands (`STATUS_BAND_ROWS_*`) are fixed
    per fill gap, and gaps per second fall with the fps (1,950/s at 25 MHz, 1,560 at 20, 1,250 at
    16), so page builds and status redraws take ~25% (20 MHz) to ~56% (16 MHz) longer in wall
    time. Each gap is longer though (fill time unchanged, budget bigger), so bigger slices would
    fit: that's the payoff of the experiment, but resizing them is a separate change (needs a
    stress run). Not changed.
  - `fill_gap` (`COMP_GAP_FRESH_US` 50 us freshness): independent of the clock.
  - `screen.cpp`: `BUDGET_CYCLES` follows `DISPLAY_BOUNCE_BUDGET_US`, so late-fill counting scales.
  - `main.cpp`: the stats line's fps is measured; the stress log prints the budget from the macro;
    stress timings (keys every 20 ms for 2 s) are wall time. Nothing hard-codes 61 fps.
  - Host tests: `test/harness.h` hard-codes `FRAME_US 16400` for its fake clock and
    `TestTouchRouter` simulates 16 ms frames, but the tests don't include
    `waveshare_rgb_lcd_port.h`, so the switch can't affect them (and they exercise logic that
    takes the period as input). No tested code changed, so the host tests weren't rerun.
  - **Panel minimum pixel clock: not known.** Nothing in this repo or `../aclock` gives the
    panel's DCLK range (both use 25 MHz). Check Waveshare's panel datasheet / wiki before reading
    anything into a bad picture at 16 MHz (flicker, rolling or blank screen = below the panel's
    range, not our code).
- **Step 6 code done (2026-11-05):** `-DCB_PIXEL_CLOCK_MHZ=<n>` in `main/CMakeLists.txt` defines
  `PIXEL_CLOCK_MHZ`; `waveshare_rgb_lcd_port.h` uses it for `DISPLAY_PIXEL_CLOCK_HZ` (default 25
  MHz). Empty value = unset (`if()` is false for an empty string), which is how to remove it
  (idf.py has no `-U`). The VSYNC porches became `DISPLAY_VSYNC_*` / `DISPLAY_V_TOTAL` (used by
  the driver config and the log). `display_init_common()` logs at INFO:
  `Install RGB LCD panel driver: pixel clock 25 MHz (~60.9 fps), fill budget 492 us`.
  Builds (0 errors): release default (no define in compile_commands), release `=20`
  (`PIXEL_CLOCK_MHZ=20` in 73 entries), release `=` (define gone, cache `CB_PIXEL_CLOCK_MHZ=`
  empty), debug default (no define). `build/release` is back at the default.

## Flagged assumptions
- The ~250 ms figure is from the gateway ping in the connect log (or an external ping to the
  device); which one wasn't recorded.
- ~~Run 1 was an `-iram` preset build~~ Resolved: run 1 was the normal build (an IRAM build has
  ~59 KB free with the driver up, not ~95 KB).
- The "other machines take 1 ms" figure may be from a wired PC, which wouldn't be a fair
  comparison. A phone or laptop on the same 2.4 GHz SSID would be.

- Drive 0 is kept for all 20 panel pins (the user says the picture looks fine). Drive 1, or weak
  drive on PCLK only, wasn't tried: only worth trying if the picture ever shows problems.
- `scratch/wifi-baseline/` is kept (gitignored) in case it's useful again.
- A ping requested while a session runs is skipped, not queued (only diagnostics; the next
  diag period pings again).
- Step 6: compositor slice and status band sizes are left as they are at a lower clock (the gaps
  get longer, so pages and status redraws build ~25-56% slower in wall time). Raising them to use
  the extra headroom is a design decision for the user, with a stress run, if a lower clock is
  adopted.
- Step 6: the panel's minimum pixel clock is unknown (not in the repo or `aclock`).
- `sdkconfig.defaults` line "core 1 is the compositor's" left as is (true; the fill's core is
  a step 7 docs fix).

## Plan
Device-only work throughout: no step can be covered by the host unit tests (`firmware/test`
doesn't build the drivers, `wifi.cpp` or the presets). Each step builds debug and release with
0 errors; the hardware checks are the user's.

- [x] 1. **Make the weak panel drive permanent.** In `waveshare_rgb_lcd_port.h` add
  `DISPLAY_PIN_DRIVE_CAP` (`GPIO_DRIVE_CAP_0`) beside the pin list, with a comment on why
  (Wi-Fi interference, this job). Apply it to the 20 panel pins in
  `display_init_common()` always (the existing block, minus the `#ifdef`). Remove the
  `CB_LCD_DRIVE_CAP` switch from `main/CMakeLists.txt` (and `-UCB_LCD_DRIVE_CAP` from
  `build/debug`'s cache by building with it). Keep the log line at INFO.
- [x] 2. **Remove the experiments that didn't help.** Delete the presets `debug-core1`,
  `release-core1`, `debug-core1-iram`, `debug-iram`, `release-iram` and the overlays
  `sdkconfig.wifi_core1`, `sdkconfig.wifi_iram` (and their `build/` dirs). Keep
  `WIFI_TASK_CORE` following `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1` in `wifi.cpp` (it's correct
  either way; reword the comment so it no longer mentions the overlay). Update the
  `sdkconfig.defaults` IRAM comment: the opts cost ~36 KB internal RAM and didn't help latency.
- [x] 3. **Fix the gateway ping diagnostics in `wifi.cpp`.** `uint16_t` for
  `ESP_PING_PROF_SEQNO`, zero-initialised profile reads, and don't start a diag session while
  the connect session is still running (one session at a time).
- [x] 4. **Decide on the diagnostic switches.** Keep `CB_NO_DISPLAY` and `CB_IDLE_FILL` (cheap,
  and they tell display load from interference next time); document them in step 7.
- [x] 5. **Hardware checks (user):** release build: `ping -n 50` at rest and while swiping, the
  picture (fine detail, colours, swiping), the `RUN_STRESS` run (late fills unchanged), and the
  gateway ping log lines now showing sane sequence numbers.
- [ ] 6. **Pixel clock experiment** (for fill headroom, not needed for Wi-Fi): add
  `-DCB_PIXEL_CLOCK_MHZ=` overriding `DISPLAY_PIXEL_CLOCK_HZ` (the bounce budget already
  follows it). The user tries e.g. 20, 18 and 16 MHz (about 49, 44 and 39 fps) and judges swipe
  smoothness, `RUN_STRESS` late fills and fill headroom (budget ~492 µs at 25 MHz, ~615 µs at
  20). Check first for anything that assumes ~61 fps: `swipe.cpp` steps once per frame (its
  glide tuning may be per frame rather than per ms), the status bar band copy per gap,
  `DISPLAY_BOUNCE_BUDGET_US` users. Record results here; changing the default clock is a
  separate decision for the user (could become its own job).
  - *Code part done (see Notes, "Step 6 audit" and "Step 6 code done"); the hardware trials are
    the user's.* From `firmware/` (PowerShell, after the usual env setup), for n = 20, 18, 16:
    `idf.py --preset release -DCB_PIXEL_CLOCK_MHZ=n -DCB_RUN_STRESS=OFF -p COM3 flash monitor`
    (swipe feel, picture, `ping -n 50` at rest and swiping), then
    `idf.py --preset release -DCB_PIXEL_CLOCK_MHZ=n -DCB_RUN_STRESS=ON -p COM3 flash monitor`
    (fill max and over-budget counts per case against the logged budget: 615 / 683 / 768 us).
    Check the boot line `pixel clock n MHz (~fps), fill budget ... us`. Judge: glides as smooth
    at ~49 / 44 / 39 fps? (tuning is real time, so speed and duration stay the same; only the
    step size grows), any panel flicker or rolling (could be below the panel's minimum pclk,
    unknown), fill headroom gained. Afterwards go back to the default:
    `idf.py --preset release -DCB_PIXEL_CLOCK_MHZ= -DCB_RUN_STRESS=OFF -p COM3 flash monitor`.
  - *Trials:* 20 MHz done (stress clean, looks OK; pings not reported). 18 and 16 MHz not tried.
    Waiting on the user: adopt 20 MHz as the default, or try 18/16 first. If 20 MHz is adopted,
    consider (separately, with a stress run) larger compositor slices / status bands to use
    the longer gaps.
- [ ] 7. **Docs** (propose the changes to the user first, Rule 4): CLAUDE.md (Hardware: panel pin
  drive and why; Firmware: the diagnostic switches and `CB_PIXEL_CLOCK_MHZ`; Open Items: Wi-Fi
  latency resolved, internal RAM figures (release: ~108 KB free with the driver up, ~106 KB
  connected, largest block 64 KB; see Notes, "Step 5"); the fill figures if the pixel clock
  changes); `docs/wifi.md` (Cores: the fill is on core
  0, not core 1; Internal RAM figures; a short "Latency" section with the cause and fix;
  IRAM opts tried and rejected); `docs/backlight-waveshare-support.md` doesn't need it.
- [ ] 8. Rename this file to `wip/complete-wifi-latency.md`.
