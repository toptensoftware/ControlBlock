# Status Bar

The fixed 800x40 band at the top of the screen. Status: **implemented and measured on the hardware**, with mock content (`main.cpp`).

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

- **Internal RAM, not PSRAM:** the fill copies the bar every frame; from PSRAM the first fills of each frame would read cold (~285 µs per 15 lines vs ~70 µs).
- **Double buffered:** two buffers. The status task draws into the one not shown, then hands it to the screen (`screen_set_status_bar()`), which latches it at the next frame start. It draws again only once the fill has latched it (`screen_status_bar_shown()` returns the front), so a buffer is never drawn while the fill may read it, and the bar never shows half-drawn.
- **At most one redraw a frame.** Changes made while a redraw is waiting or running are combined into the next.

Code:

- **`status_bar.cpp/.h`** (platform independent, host tested in `firmware/test/TestStatusBar.cpp`):
  - `status_bar_render(px, style, content)`: draws the whole bar.
  - `StatusBar`: the four properties and two buffers. Setters return whether the value changed. `BeginDraw(shown, &content)` copies the properties and returns the back buffer, or NULL if nothing changed or the fill hasn't latched the front yet; `EndDraw()` makes it the front.
- **`status.cpp/.h`** (device): owns the `StatusBar`, its buffers in internal RAM and a task (core 1, priority 3, below the compositor and swipe) that waits for a change, draws, and swaps. If a change is waiting on a swap, it polls once a tick.

The host test writes `firmware/test/build/status_bar.bmp`, handy for checking the look without flashing.

## Measured

`RUN_STRESS` (release build; status redrawn every 20 ms, mid-swipe):

| | Slowest fill | Fills over budget |
|---|---|---|
| Status bar redraws alone | 206 µs | 0 |
| On top of the compositor flat out (opaque faces) | 357 µs | 0 |

At rest the bar adds nothing measurable to the fill (~115 µs, as before). The status font and rle565 code are read from PSRAM (code and rodata are mapped there), so a redraw adds a little PSRAM traffic; it showed only as extra contention with the compositor flat out, within budget.

## Open items

- **Internal RAM:** the two buffers take 128 KB; ~130 KB is left after init (largest block 86 KB). If Wi-Fi needs more, keep only the parts that change (each text and the pills) as small buffers and fill the rest black in the bounce fill, or move Wi-Fi/lwIP buffers to PSRAM.
- Real content: the activity name and time (needs a clock source) from the host or the device; the time format.
