# Status Bar

The fixed 800x40 band at the top of the screen. Status: **implemented and measured on the hardware**, empty at boot until the host sets its texts.

```
+--------------------------------------------------------------------+
| AppleTV                      ▬ ━━ ▬                        12.35am |
+--------------------------------------------------------------------+
  left text                   pager pills                 right text
```

## Content

Four properties:

| Property | Setter | Drawn as |
|---|---|---|
| Left text | `status_set_left()` | White, `STATUS_BAR_MARGIN` (16 px) from the left |
| Right text | `status_set_right()` | White, right aligned, 16 px from the right |
| Pill count | `status_set_pill_count()` | One pill per page, centred, `STATUS_BAR_PILL_GAP` (8 px) apart; 0 for none |
| Selected pill | `status_set_selected_pill()` | The wide white pill; the others are short and grey. Out of range: none selected |

- Text is UTF-8, at most `STATUS_BAR_TEXT_MAX - 1` (47) bytes; longer is cut on a character boundary. NULL is the same as "".
- Text is vertically centred on its cap height (the 'H' glyph), so mixed case and digits sit evenly.
- Black background (`STATUS_BAR_BACKGROUND`, the same as the page area's `COMP_BACKGROUND`; `screen.cpp` checks), no border.
- The swipe task sets the selected pill every frame to the page most in view (`screen_page_most_in_view()`), so it switches as a swipe passes halfway and follows rubber-banding at the ends. Only an actual change causes a redraw.

The setters are safe from any task (not ISRs). Setting a property to its current value does nothing.

The host sets the two texts with `SET_STATUS_TEXT` ([protocol.md](protocol.md)); the protocol task calls `status_set_left/right()`, which only copy 48 bytes under a short spinlock and wake the status task. Drawing stays in that task (internal RAM only). `RESET` clears both. The device boots with both texts empty.

## Assets

`firmware/assets/ui.bin`, a blobpack built by `tools/test-icons/gen.js` (layout in `ui_assets.h`):

| Blob | Content |
|---|---|
| `UI_ASSET_STATUS_FONT` | Inter SemiBold, 20 px, ASCII 32–126, 4 subpixel positions (~66 KB). Source: `tools/test-icons/fonts/` (SIL OFL 1.1) |
| `UI_ASSET_PAGER_CURRENT` | 24x6 white rounded pill (rle565 with alpha) |
| `UI_ASSET_PAGER_OTHER` | 12x6 grey (#606060) rounded pill |

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
  - `StatusBar`: the four properties and two buffers. Setters return whether the value changed. `BeginDraw(shown, &content)` copies the properties and returns the back buffer, or NULL if nothing changed or the fill hasn't latched the front yet; `EndDraw()` makes it the front.
- **`status.cpp/.h`** (device): owns the `StatusBar`, its buffers in PSRAM, the scratch band in internal RAM and a task (core 1, priority 3, below the compositor and swipe) that waits for a change, renders a band, copies it in a fill gap, and swaps after the last. If a change is waiting on a swap, it polls once a tick.

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
