# Status Bar

The fixed 800x40 band at the top of the screen. Status: **implemented and measured on the hardware**, the left text empty and the right `{clock}` at boot (see Clock).

```
+--------------------------------------------------------------------+
| AppleTV                      ▬ ━━ ▬                     12:35  ! … • |
+--------------------------------------------------------------------+
  left text                   pager pills           right text  icons
```

## Content

Four properties:

| Property | Setter | Drawn as |
|---|---|---|
| Left text | `status_set_left()` | White, `STATUS_BAR_MARGIN` (16 px) from the left |
| Right text | `status_set_right()` | White, right aligned, 16 px from the right |
| Pill count | `status_set_pill_count()` | One pill per page, centred, `STATUS_BAR_PILL_GAP` (8 px) apart; none for 0 or 1 page (one page has nothing to page between) |
| Selected pill | `status_set_selected_pill()` | The wide white pill; the others are short and grey. Out of range: none selected |

- Text is UTF-8, at most `STATUS_BAR_TEXT_MAX - 1` (47) bytes; longer is cut on a character boundary. NULL is the same as "".
- Text is vertically centred on its cap height (the 'H' glyph), so mixed case and digits sit evenly.
- Black background (`STATUS_BAR_BACKGROUND`, the same as the page area's `COMP_BACKGROUND`; `screen.cpp` checks), no border.
- The swipe task sets the selected pill every frame to the page most in view (`screen_page_most_in_view()`), so it switches as a swipe passes halfway and follows rubber-banding at the ends. Only an actual change causes a redraw.

The setters are safe from any task (not ISRs). Setting a property to its current value does nothing.

The host sets the two texts with `SET_STATUS_TEXT` ([protocol.md](protocol.md)); the protocol task calls `status_set_left/right()`, which only copy 48 bytes under a short spinlock and wake the status task. Drawing stays in that task (internal RAM only). `RESET` and `TAKE_CONTROL` clear both. Standalone, the left text is the activity name and the right text is `{clock}` (set at boot, restored on release and disconnect; [standalone.md](standalone.md#status-bar)).

## Icons

Built and host tested; **not yet run on the hardware.** Firmware owned: no protocol message sets or hides them.

- **Placement:** just left of the right text (the clock), which stays at the right margin. Left to right: error or busy, Wi-Fi, then the clock, each icon centred in a `STATUS_ICON_SLOT_W` (22 px) slot, `STATUS_ICON_TEXT_GAP` (8 px) before the text. Only the icons showing take a slot, and they move with the clock's width. With no right text they end at the margin.
- **Wi-Fi:** always shown. Connected: the Wi-Fi waves, white like the text; anything else (no credentials, connecting, retrying): a grey dot. State only: no signal bars, no polling. Fed from `host_link.cpp`'s `on_wifi_state`.
- **Busy:** a small white hourglass. Shown while the op queue runs or the net worker has requests queued or in flight (a fire-and-forget request outlives its op), kept until both are idle, but at least `STATUS_BUSY_MIN_MS` (500 ms) from the latest start, so a short HID tap still shows briefly rather than flickering. Fed from the op task's busy edges (`status_busy`) and the net worker (`status_net_busy`: set on a submit, cleared when its queue drains, both under its lock).
- **Error:** a red circle with a white x, for `STATUS_ERROR_SHOW_MS` (5 s) after an op that reaches outside the device fails for external reasons; a new failure restarts the 5 s. It shares the busy icon's place and wins over it (`StatusIcons::Visible` never reports both). What counts: [standalone.md](standalone.md#status-bar). Configuration errors (Lua errors, refused switches, bad params) never show it; they are logged (`op failed: ...`).
- **Hosted:** only the Wi-Fi icon shows. A takeover clears busy and error.
- **State:** `StatusIcons` (`status_icons.cpp/.h`, pure, `TestStatusIcons.cpp`) takes the inputs with injected times and gives the visible bits (`Visible(now)`) and the ms to the next busy-minimum or error expiry (`NextDeadlineMs`). `status.cpp` holds it under its own spinlock (setters `status_set_wifi`, `status_busy`, `status_error`, `status_set_hosted`, safe from any task) and passes the bits to `StatusBar::SetIcons` (redraws only on a change).
- **Wake:** the status task waits `status_wait_ms(clock, icons)`: the sooner of the clock's minute wake and the icon deadline. Each icon change is a full redraw (rare: a few per op run).
- **Images:** ui.bin blobs `UI_ASSET_STATUS_ICON` + bit (see Assets).

## Clock

Built and host tested; **not yet run on the hardware.**

- **Placeholder:** every `{clock}` in the left or right text is replaced by the local time, `h:mm` on a 12 hour clock without am/pm ("2:05", "12:30"; hour 12 at noon and midnight, no leading zero). Only exactly `{clock}` is replaced; other braces stay as they are. The placeholder works whatever set the text (the host, an activity name).
- **Templates:** `StatusBar` keeps the texts as set (templates, cut to 47 bytes) and the current clock text (`SetClock`); `BeginDraw` expands them into the content it hands out. `SetClock` asks for a redraw only when a template holds `{clock}` and the text changed. `{clock}` (7 bytes) always expands to at most 5, so expansion never makes a text longer; a `{clock}` cut by the 47-byte limit is lost.
- **Blank until known:** `{clock}` expands to "" until the time is known, i.e. the clock reads 2025-01-01 UTC or later (`clock_time_valid`). After power-up the RTC timer starts at 1970; it keeps running over `esp_restart` (a layout commit), so the time survives a soft restart but not power-off (the board has no battery-backed RTC).
- **Time sources:** NTP while Wi-Fi is up ([wifi.md](wifi.md#ntp)), or a host with `TIME_SET` ([protocol.md](protocol.md#clock); `node cli/cli.js time`), allowed while standalone. The latest wins.
- **Time zone:** a POSIX TZ string (default `UTC0`; e.g. Sydney `AEST-10AEDT,M10.1.0,M4.1.0/3`) stored in NVS with the NTP server (`clock_config.cpp`, namespace `cb_clock`), set with `CLOCK_SET` or `node cli/cli.js time zone <posix>`. Applied with `setenv("TZ")`/`tzset` and read with `localtime_r` in `clock.cpp`, under one mutex (the TZ is set on the protocol task and read on the status task). The C library is picolibc; DST rules (`M` rules) are its job. A malformed string is accepted and treated as UTC.
- **Minute wake:** while a template holds `{clock}` (`HasClock()`), the status task waits until just after the next minute boundary (`clock_wait_ms`: ms to the next UTC minute + `CLOCK_WAKE_MARGIN_MS` 20 ms, rounded up to ticks); otherwise it waits for a change as before. Every zone offset is a whole number of minutes, so UTC and local minutes turn over together. On each wake it refreshes the clock text, so a new time or zone shows at once. A redraw takes 10-20 fill gaps, so the minute appears up to ~0.35 s late. Once a minute is far below the redraw rate tested in `RUN_STRESS`.
- **Code:** `clock_text.cpp/.h` (pure, `TestClockText.cpp`: format, expand, next-minute wait, time-valid check, UTF-8 cut), `status_bar.cpp` (templates, `TestStatusBar.cpp`), `clock_config.cpp/.h` (pure, `TestClockConfig.cpp`), `clock.cpp/.h` and `status.cpp` (device only).
- **Stack:** `localtime_r` runs on the status task's 3072-byte stack. Debug builds log `status: stack: N bytes free` once, after the first real local time; raise the stack if that is under ~512.

## Assets

`firmware/assets/ui.bin`, a blobpack built by `tools/test-icons/gen.js` (layout in `ui_assets.h`):

| Blob | Content |
|---|---|
| `UI_ASSET_STATUS_FONT` | Inter SemiBold, 20 px, ASCII 32–126, 4 subpixel positions (~66 KB). Source: `tools/test-icons/fonts/` (SIL OFL 1.1) |
| `UI_ASSET_PAGER_CURRENT` | 24x6 white rounded pill (rle565 with alpha) |
| `UI_ASSET_PAGER_OTHER` | 12x6 grey (#606060) rounded pill |
| `UI_ASSET_STATUS_ICON` + 0..3 | 20 px status icons with alpha, placeholders: Wi-Fi waves (white), Wi-Fi dot (grey #a0a0a0), busy (white hourglass), error (red circle, white x). To replace one, edit its SVG in `gen.js` (`STATUS_ICONS`), `npm run build` in `tools/test-icons`, rebuild the firmware |

`main.cpp` copies the pack to PSRAM (rle565 needs 4-byte alignment); `status_init()` validates it (`check_font()`, `check_image_header()`). Pill sizes come from the images, so the layout follows any change to them.

## Drawing and buffering

The bounce fill can't decode rle565, so the bar is drawn ahead of time into a whole-bar buffer (800x40 RGB565, 64 KB) that the fill copies row by row.

- **PSRAM (implemented, not yet run on the hardware):** the two buffers were in internal RAM (128 KB), which left the Wi-Fi driver under 1 KB ([`wifi.md`](wifi.md)). They are now `heap_caps_aligned_alloc(64, ..., MALLOC_CAP_SPIRAM)`. The fill reads the bar every frame, so it is kept warm: `preload_chunk()` in `screen.cpp` preloads the bar's rows for the next chunk (the first chunk of the next frame at the end of the last fill), unless the chunk has more page rows than bar rows (only one range can be preloaded).
- **Written only in fill gaps, in bands:** a redraw would write 64 KB of PSRAM, which starves the fill. So `status_bar_render_rows()` renders a few rows into a small internal scratch band (`SCREEN_W * STATUS_BAR_BAND_ROWS_MAX` pixels, 6.4 KB), and the status task copies one band to the back buffer (and writes it back) per fill gap: `STATUS_BAND_ROWS_ONE_PAGE` (4 rows, 6.4 KB) or `STATUS_BAND_ROWS_TWO_PAGES` (2 rows, 3.2 KB, mid-swipe) at the top of `status.cpp`. A redraw takes 10 or 20 gaps (0.17 or 0.33 s). The compositor's slices are 8 KB and 3 KB; those and the band sizes are first guesses.
- **Sharing gaps:** `fill_gap.cpp/.h` replaces `comp_fill_done()`. The compositor and the status task each register a client; while a client has something to write, each gap goes to one of them in turn (`gap_share.h`, host tested), so two writers never overlap. A gap a task reaches more than `COMP_GAP_FRESH_US` after the fill is skipped.
- **Double buffered:** two buffers. The status task draws into the one not shown, then hands it to the screen (`screen_set_status_bar()`), which latches it at the next frame start. It draws again only once the fill has latched it (`screen_status_bar_shown()` returns the front), so a buffer is never drawn while the fill may read it, and the bar never shows half-drawn (the swap happens only after the last band). The content is copied at `BeginDraw`, so a redraw is one consistent state.
- **At most one redraw at a time.** Changes made while a redraw is waiting or running are combined into the next.

Code:

- **`status_bar.cpp/.h`** (platform independent, host tested in `firmware/test/TestStatusBar.cpp`):
  - `status_bar_render(px, style, content)`: draws the whole bar. `status_bar_render_rows(px, y0, rows, ...)` draws a band of it, identical pixels (host tested at several band sizes).
  - `StatusBar`: the four properties (texts as templates, see Clock), the clock text and two buffers. Setters return whether the value changed. `BeginDraw(shown, &content)` copies the properties and returns the back buffer, or NULL if nothing changed or the fill hasn't latched the front yet; `EndDraw()` makes it the front.
- **`status.cpp/.h`** (device): owns the `StatusBar`, its buffers in PSRAM, the scratch band in internal RAM and a task (core 1, priority 3, below the compositor and swipe) that waits for a change (or the next minute while a text holds `{clock}`), renders a band, copies it in a fill gap, and swaps after the last. If a change is waiting on a swap, it polls once a tick.

The host test writes `firmware/test/build/status_bar.bmp`, handy for checking the look without flashing.

## Measured

`RUN_STRESS` (release build, bar in PSRAM; status redrawn every 20 ms, mid-swipe), against the earlier internal-RAM bar:

| | Slowest fill, PSRAM bar | Internal-RAM bar | Fills over budget |
|---|---|---|---|
| Compositor flat out, at rest | 121-123 µs | ~115 µs | 0 |
| Compositor flat out, mid-swipe | 286-292 µs | | 0 |
| Status bar redraws alone, mid-swipe | 353 µs | 206 µs | 0 |
| Compositor (opaque) + status bar, mid-swipe | 316 µs | 357 µs | 0 |
| Image replaces, at rest | 280 µs | not run | 0 |
| Image replaces, mid-swipe | 412 µs | not run | 0 |

- No late fills in any case. At rest the bar costs nothing measurable.
- The bar alone mid-swipe is slower (353 vs 206 µs): its first chunk is a cold PSRAM read. With the compositor also running it is no slower than before, since the two now share the fill gaps.
- Image replaces mid-swipe (412 µs) leave the least margin of the cases (80 µs under budget). This was their first run on the hardware.
- With the demo (clock redraw each second, swiping), a debug build reached 475 µs once in a one-second window, still under budget.

The status font and rle565 code are read from PSRAM (code and rodata are mapped there), so a redraw adds a little PSRAM traffic.

## Open items

- **PSRAM bar, measured** (see Measured): no late fills in `RUN_STRESS`, and Wi-Fi comes up with ~120 KB internal RAM free. The redraw latency of 0.17-0.33 s for pill changes looks fine on the hardware (the band rows are tuning constants in `status.cpp`). Wi-Fi traffic during a swipe is not tested and not a concern: actions are one quick fire-and-forget after a press. The mid-swipe fill maximum is above the 250 µs aim in the stress cases, but never late.
- **Fallback if the fill goes late:** keep only the parts that change (each text and the pills) as small internal buffers and fill the rest black in the bounce fill.
- The preload choice (bar rows vs page rows) and the band row choice in `status.cpp` are device-only and not covered by the host tests.
