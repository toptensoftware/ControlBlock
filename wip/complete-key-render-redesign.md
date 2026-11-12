# Key rendering redesign (tight packing, spanning keys, opaque key images)

Status: Complete

## Decisions (user, 2026-11-12)
- Tight packing and spanning keys are merged into this one job.
- **Opaque key images:** every key image is one fully opaque, fully baked image per state (normal, pressed),
  composed by the host over the fixed black background. No bg/fg layers, no blending, no clear. The compositor
  draws rows of the image straight into the PSRAM page buffer in fill gaps, in ~1 KB chunks limited by time
  (see step 2's decisions; the earlier "rows per gap = byte budget / (key width x 2)" is superseded). The
  internal 32 KB render cell goes away. Areas no key covers are solid-filled black in gaps. The device refuses
  non-opaque key images.
- **Units and key placement (user, 2026-11-12, supersedes "whole-slot spans"):** 1U = **144 px** baked image
  (cells edge to edge; grid 5x3 = 720x432, centred in the 800x440 page area: origin (40, 4)). **Fractional
  positions and sizes in 0.25U steps** (0.25U = 36 px exactly). Each key has a position (x, y in slots), a size
  (w, h in slots), both multiples of 0.25U, and an **id** (u16, unique across the layout, not per page) used for click reporting (KEY events), updates and
  bindings. On the wire and in the blob, x/y/w/h are counts of quarter units (u8; x 0..20, y 0..12); the device
  converts to pixels (x 36). Keys may not overlap and must lie inside the grid; the image size must equal the
  key's size in pixels.
- ~3.5x image bytes and slower standalone uploads are accepted. Deflate on transmission: not in this job (later).
- ~~The image being drawn is copied into an internal SRAM buffer (~8 KB).~~ Superseded by the spike (step 2):
  images are drawn straight from PSRAM, no source copy.
- Rejected: a blocking "wait for gap" restructure of the compositor.
- Clean break for existing 128 images and layouts is acceptable (the investigation found no reason against:
  PROTO_VERSION and LAYOUT_BLOB_VERSION bump; a stored old layout leaves the empty Home until re-uploaded).

## Description
Merged 2026-11-12 from two jobs (their investigation notes are kept below):
1. **Tight packing:** fold today's gap into the key: cells the size of the current pitch (~146x146), edge to edge,
   the block still centred with a margin; spacing drawn in the images. Allows grouped/split keys (`+`/`-` with one
   "Volume" label across both).
2. **Spanning keys:** keys covering several slots, possibly keyboard-style widths (1U, 1.5U, 2U), so pages don't
   have messy holes. Changes key identity, touch hit-testing, the compositor.

Device side first; the authoring side (layout format, bake) is sketched only.

## Direction (user, 2026-11-12; chosen)
**Every key image is one fully opaque, fully baked image per state** (normal, pressed): face, icon, label and
anti-aliasing already composed over the fixed black background by the host. No bg layer, no fg layer, no clear,
no blending. The compositor then draws rows y0..y1 of the image (rle565 row offsets + `draw_image` source rect)
**straight into the PSRAM page buffer, in fill gaps**, with rows per gap = byte budget / (key width x 2).
- Opaque means pure writes (no PSRAM read-modify-write): that's what makes drawing into PSRAM viable.
- Removes the 32 KB internal render cell (instead of growing it to 42 KB, 80 KB for 2U, 160 KB for 2Ux2U).
- Any key size; grouped and spanning keys become non-overlapping rects (no layers, no quadrants). Areas no key
  covers are solid-filled black in gaps (also the erase needed when pages have different layouts).
- The device can refuse non-opaque images (`check_image` already reports opacity).
- Unchanged: triple buffering, snapshot, offer/swap rule, the bounce fill.
- Rejected: restructuring the compositor as a blocking "wait for gap" task (user: doesn't help much). The gap
  slicing itself is required: PSRAM writes from core 1 overlapping the fill on core 0 make fills late
  (docs/page-compositor.md:79), regardless of task priority.

### Measured: upload size (2026-11-12, `scratch/opaque-size.mjs`)
urem2 (both entries identical in images; 165 key slots), unique images after dedupe:

| Scheme | Images | Total | Avg | Max |
|---|---|---|---|---|
| Today: transparent layers, 128 | 64 | 120.9 KB | 1.89 KB | 4.18 KB |
| Layers padded to 146 | 64 | 121.6 KB | 1.90 KB | 4.19 KB |
| One opaque image per state, 128 | 125 | 402.9 KB | 3.22 KB | 4.93 KB |
| One opaque image per state, 146 (128 face + black pad) | 125 | 420.5 KB | 3.36 KB | 5.07 KB |

So ~3.5x the image bytes (the blob would go from ~150 KB to ~450 KB): no more sharing of the common face, and
every key's two states are separate full images. Fits the 4 MB layout partition and PSRAM easily; upload and flash
write take a few seconds longer. urem2's faces are flat black with a border; gradient faces would cost more.

**User (2026-11-12): 3.5x image bytes is OK, and slower standalone uploads are fine. Concern: hosted mode.**

Deflate per image (raw deflate, level 9, each image on its own): opaque 146 420.5 KB -> 174.9 KB (2.4x, ~1.4 KB
avg); today's layers 121.6 -> 81.3 KB (1.5x). So deflated opaque images are ~1.4x today's raw bytes, not 3.5x.
Hosted-mode cost at USB full speed (~0.5-1 MB/s, not yet measured: `examples/bench.js` never run): a full page
(15 keys x 2 states, ~100 KB) ~0.1-0.2 s raw, ~0.05-0.08 s deflated; one key change (~7 KB) ~10 ms vs ~4 ms.
Device side would be small: miniz `tinfl` is in the ESP32-S3 ROM; inflating straight into the image's PSRAM slot
(non-wrapping output buffer = no 32 KB window), decompressor state ~11 KB (PSRAM or stack), streamed input.
Could be an optional per-upload flag (UPLOAD_BEGIN), and the same for the layout blob upload.
Suggested: not in this job; measure real throughput first (bench.js) and add it only if hosted updates feel slow.

### Source image in internal RAM (user question, 2026-11-12)
Worth doing, and cheap. The data cache is 64 KB (64-byte lines). Mid-swipe each fill reads ~24 KB of page rows and
preloads the next ~24 KB, so ~48 KB passes through the cache per fill: a 3-5 KB source image will usually be evicted
between gaps, and re-reading it inside the gap spends gap time on PSRAM reads instead of writes. Copy the image
being drawn into an internal buffer once per key (outside the gaps, as today's render already reads images outside
them); 8 KB covers every urem2 image (max 5.07 KB), with a fallback for bigger images (2U keys): copy just the
rows of the current slice, or draw from PSRAM. Measure both in the spike.

## Notes
### Spike (Plan step 1, built 2026-11-12; device only, not unit testable)
**Files.** `firmware/main/stress_opaque.cpp/.h` (new), hooks in `main.cpp` (`stress_opaque_init` after `comp_init`,
before `model_init`/`screen_init`; `stress_opaque_run()` after the existing cases, before the cleanup;
`stress_opaque_log(late_fills)` after their results), `main/CMakeLists.txt` (`stress_opaque.cpp` and
`assets/spike_keys.bin` only with `CB_RUN_STRESS`), `fill_gap.h` (`FILL_GAP_MAX_CLIENTS` 2 -> 3: the spike is a third
gap client), `tools/test-icons/gen.js` (new section, writes `firmware/assets/spike_keys.bin`; `test_keys.bin` and
`ui.bin` regenerate byte-identical). To remove: those files and lines, plus `spike_keys.bin`.

**Images** (`spike_keys.bin`, 111 KB): 15 opaque 1U keys 144x144 (gradient face 128 with a border, 8 px black
padding, 64 px icon, bold label) at 4.4-6.9 KB, and 5 opaque 2U keys 288x144 at 5.9-7.9 KB. All of them fit the
8 KB internal source buffer whole, so the "copy" source never falls back to slices here (the 2U images that don't fit
8 KB from the plan don't occur with this content; "slice" is measured as its own variant). PNGs in
`tools/test-icons/out/spike-*.png`.

**What it does.** The spike has its own PSRAM page buffer (off screen, like the compositor's back buffer; the fill
keeps showing the compositor's demo pages, at rest = page 0, mid = scroll 400 with pages 0 and 1, the compositor idle:
`comp keys` in the log line shows it drew nothing). A task on core 1 (priority 4, as the compositor) registered on
`fill_gap` builds one page over and over: the black margins round the 720x432 grid at (40, 4) (`fill_rect`), then
each key, rows per gap = budget / (w x 2), each slice drawn with `draw_image` (source rect) straight into the PSRAM
buffer and written back (`port::CacheWriteback`) inside the gap, after `fill_gap_fresh(COMP_GAP_FRESH_US)` (a gap
reached late is skipped and counted as `stale`). Sources: `psram` (draw from the image in PSRAM), `copy` (whole image
copied to an 8 KB internal buffer before its first slice, outside the gaps), `slice` (only the slice's rows copied in
before each gap). The copies are self-contained rle565 images (header, row table for those rows, palette, the rows'
run data, `firstRow` = the first row copied: `stage_rows`), so `draw_image` takes them unchanged. Pages: `1U` = 15 x
1U; `2U` = rows {2,1,1,1}, {1,2,2}, {2,2,1} (5 x 2U + 5 x 1U). Cases: {rest, mid} x {1U, 2U} x {psram, copy, slice} x
{3, 4, 8, 12 KB} = 48, each at least 1 s and 2 builds. Every case's last build is CRC-checked against a reference
drawn at boot (before the display starts), after poisoning the buffer before the case (outside its measurement).

**How to run (user, on the hardware).** From PowerShell in `firmware/` (the release preset's cache was left with
`CB_RUN_STRESS=ON` and built, so this only flashes):
```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
idf.py --preset release -DCB_RUN_STRESS=ON -p COM4 flash monitor
```
App-only flash is fine (no partition change). The run takes ~95 s after boot (~35 s of the existing cases, then
~60 s of spike cases); the board looks hung meanwhile; touch, USB and Wi-Fi start after it. Results are logged at the
end: first the existing `stress ...` lines (the comparison), then a `---- spike: ...` header and 48 lines like
```
spike rest, 1U, copy, 8 KB     fill max  NNN us,   0 over | build   NN.N ms (max   NN.N),  NNN gaps, gap max NNN us, N stale, N builds | comp keys 0 | src a/b/c | ok
```
- `fill max` / `over`: slowest bounce fill and fills over budget (~615 us at 20 MHz) during the case; a late-fill
  split follows at the end of the line if any were late. **The go/no-go numbers.**
- `build`: average (max) time to build the whole page (keys + margins); `gaps`: gaps used per build; `gap max`: the
  longest draw + write-back inside one gap (must stay well inside the ~400 us gap); `stale`: gaps reached too late.
- `comp keys`: keys the compositor drew during the case (should be 0); `src`: slices drawn from a whole copy /
  a slice copy / PSRAM (totals); `ok` / `PIXELS WRONG`: the CRC check.
Paste the whole log from `---- stress:` to the last `spike` line into step 2. Afterwards, back to normal:
`idf.py --preset release -DCB_RUN_STRESS=OFF -p COM4 flash`.

**Build state left:** `build/debug` built normal (`CB_RUN_STRESS=OFF`); `build/release` built with
`CB_RUN_STRESS=ON` (pass `-DCB_RUN_STRESS=OFF` to undo). Debug and release stress builds and the normal debug build:
0 errors (only the toolchain's usual `times is not implemented` link warning). Host tests: 698 passed.

**Learned.** rle565 stores rows in order after the palette with no row sharing (`rle565/encoder/pack.js`), so a row
range's run data is one contiguous block and a self-contained copy is cheap to make. `draw_image` already clips to
the stored rows and starts from `rowOffsets[src_y - firstRow]`, so slices need no decoder change. `fill_rect` is a
plain per-pixel loop (fine for the margins; the real compositor should use a 32-bit fill like `fill_px` or the
existing background row copy, especially under `-Og`).

### Spike run 1 results (user, release RUN_STRESS, 2026-11-12)
- Existing stress cases unchanged: fills 141-452 us, 0 over.
- Spike: all 48 cases `ok` (pixel CRC), 0 fills over budget. **Source psram/copy/slice made no difference
  anywhere** (identical times), so the internal source buffer is dropped.
- At rest (fills stayed 141 us): 3 KB budget: gap max ~223-235 us, build 162 ms; 4 KB: gap max ~295 us, build
  ~120 ms; 8 KB: gap max ~534 us, 0 stale, build ~65 ms; 12 KB: gap max ~826 us, ~100-220 stale, build ~45-50 ms.
- Mid-swipe: 3/4 KB as at rest, fills 183 us; 8 KB: fills 441-450 us, gap max ~712 us, 13-16 stale; 12 KB:
  fills 432-462 us, gap max ~1000 us, ~400 stale.
- 2U mix ~3% faster builds than 1U. Decode writes ~65 us/KB (~15 MB/s), against today's cell copy ~25 MB/s.
- Internal heap 150,847 B free (release + stress).
- **Seen on screen:** flickery horizontal lines at various points down the **left** side of the screen (when is
  unknown; probably the 8/12 KB cases). And **after the stress run, the status bar clock was a few pixels off the top
  of the screen, wrapped to the bottom, and stayed** (a lasting vertical shift, like an RGB panel desync, despite
  `LCD_RGB_RESTART_IN_VSYNC`).

### Analysis of run 1 (2026-11-12)
- **The late-fill counter only times the fill** (`screen.cpp` `fill_lines`: cycles from its own entry to its end,
  `over` when > budget). A fill that **starts** late (ISR held off) and is short is never counted, yet the DMA can
  reach the bounce buffer before it's written: stale lines, partly written lines (the fill writes each line left to
  right, so a line it's still writing when the DMA sends it shows the new left part and the old right part, or the
  reverse). Run 1's gap work overran the gap: at 8 KB the gap took up to 534 us at rest / 712 us mid-swipe, and at
  12 KB up to 826 / 1000 us, against a ~615 us fill period with ~430-470 us free after the fill. So the spike's
  PSRAM writes ran into the next fill, which matches the elevated mid-swipe fills (441-462 us).
- **v1 also poisoned the whole page between cases with one `memset` + one 704 KB write-back on core 1**, with no
  gap control: a large PSRAM write while fills ran, 48 times. A likely source of glitches between cases (and of the
  shift, below). v2 poisons through the gaps.
- **Cache ops (`port::CacheWriteback` = `esp_cache_msync` C2M, ESP-IDF v6.0 `esp_mm/esp_cache_msync.c`):** it
  takes `esp_os_enter_critical_safe(&s_spinlock)` around `cache_hal_writeback_addr` (ROM `Cache_WriteBack_Addr`),
  i.e. **interrupts off on the calling core (core 1) for the whole write-back**, in one piece
  (`CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS` is off, and it only chunks at 32 KB anyway). The spinlock is shared
  across cores, but core 0's fill ISR doesn't take it (the RGB driver only calls `esp_cache_msync` when the bounce
  buffers are behind a cache; ours are internal), so it doesn't block the fill directly. The fill ISR and the LCD
  VSYNC ISR are on core 0 (allocated by `esp_lcd_new_rgb_panel` from `app_main`). Not settled from the sources (the
  ESP32-S3 TRM isn't in the repo): whether the shared L1 data cache stalls core 0's accesses, or serialises core 0's
  `Cache_Start_DCache_Preload` (called at the end of each fill), while core 1's write-back sync runs; the ROM code
  isn't public. The fill's measured time includes the preload call, so a preload that waits for a write-back would
  show in `fill max`; a stall of the ISR's entry wouldn't, which is why v2 measures the start time. v1 wrote back
  8-12 KB slices in one call (and the 704 KB poison in one call), holding the bus for long stretches; v2 writes back
  ~1 KB at a time and logs the longest write-back (`wb max`), so the hardware run answers it.
- **The lasting shift (from the IDF v6.0 RGB driver, `esp_lcd/rgb/esp_lcd_panel_rgb.c`):** in bounce mode the EOF
  ISR fills buffer `bb_eof_count % 2` with lines from `bounce_pos_px`; with `LCD_RGB_RESTART_IN_VSYNC` the VSYNC ISR
  restarts the DMA at buffer 0 every frame and resets `bounce_pos_px` only if it's past 2 buffers, but **never
  resets `bb_eof_count`** (only the non-RESTART branch does). So if an EOF interrupt is lost, merged or handled out
  of order around the restart (e.g. core 0 held off long enough that EOF and VSYNC are both pending), the buffer
  parity flips for good: every fill then writes the buffer the DMA is sending and the panel shows a lasting offset
  of about one bounce chunk (15 lines) — consistent with the clock moved up and wrapped to the bottom. A late fill by
  itself (no lost interrupt) only spoils the frame it's in. So the shift points to core 0 being held off for longer
  than a fill period at least once, most likely by v1's whole-page poison or the 12 KB gaps. v2 detects it: every
  fill checks that chunk k is in bounce buffer k % 2 (`buffer mismatches`), that the lines follow on (`seq breaks`:
  the driver resynced), and each case counts frames against the expected 48.8 fps.

### Spike v2 (built 2026-11-12, for the user's run)
**Changes.**
- Source: PSRAM only (copy/slice variants and the 8 KB internal buffer removed).
- **Gaps limited by time:** each gap is worked in chunks of ~1 KB (`SPIKE_CHUNK_BYTES`: 3 rows of a 1U key, 1 row of
  a 2U key, 12 rows of a side margin), each chunk written back straight after it's drawn; the next chunk is started
  only if the time since the fill ended plus the last chunk's time stays within the deadline. Deadlines **150, 250
  and 350 us** after the fill's end: the period is ~615 us, the fill ~141 us at rest / ~183 us mid-swipe, so
  ~430-470 us is free, and the next chunk's cache preload needs the bus in it too; 150 leaves the preload most of
  the gap, 350 + 183 = 533 us leaves ~80 us for ISR latency and overshoot, 250 between.
- **Opaque row decoder** (`decode_row` in `stress_opaque.cpp`, rle565 format per `rle565/src/rle565.core.h`: run
  count, runs `count << 1 | rle`, an alpha bit in literal runs when `DRAW_FLAGS_ALPHA_PIXELS`, byte or varlen
  palette indices): repeat runs with `fill_px` (32-bit stores), literals two pixels per store. Checked at boot
  against `draw_image` (CRC of both pages) and per case. The margins use `fill_px` in the same time-limited chunks.
  `fill_px` moved from `screen.cpp` to a shared `fill_px.h` (IRAM, inline; the bounce fill and the spike use it,
  and the real compositor will).
- **Fill start timing** in `screen.cpp` (`fill_start_late`, all builds, a few instructions per fill): within a
  frame the fills for lines 30..465, 0, 15 are evenly spaced (period = budget), so each fill's start is compared
  with that grid (restarted at line 30, the first after the blanking, and moved to any earlier start).
  `screen_stats_t` gains `max_late_us`, `max_end_us` (start lateness + duration; must stay under ~615 us), `missed`
  (fills that ended past the budget that way), `seq_breaks`, `buffer_mismatches`. `main.cpp`'s `fill_timing()`
  appends them to every existing `stress` line and to the spike lines.
- `fill_gap_age_us()` added to `fill_gap.h/.cpp` (time since the last fill's end; `fill_gap_fresh` uses it).
- Visual check: 12 cases ({rest, mid} x {1U, 2U} x {150, 250, 350 us}), 3 s each, each logged live at its start
  (`spike case N/12: ...`) and with a short `done:` line at its end; the existing stress cases are also logged
  live (`stress case: ...`). Summary lines at the end as before.

**How to run v2 (user, on the hardware).** From PowerShell in `firmware/` (`build/release` is built with
`CB_RUN_STRESS=ON`, so this only flashes):
```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
idf.py --preset release -DCB_RUN_STRESS=ON -p COM4 flash monitor
```
App-only flash is fine. The run takes ~80 s after boot (~35 s of the existing cases, then ~40 s of spike cases);
touch, USB and Wi-Fi start after it. **The stored layout doesn't load in a RUN_STRESS build** (the device is left
empty afterwards): expected.
- **Watch the screen** during the run, especially the left side and the status bar, and note the `stress case:` /
  `spike case N/12:` line on the monitor when anything flickers, tears or shifts. After the run, check whether the
  status bar is still in place (a lasting shift).
- **Log lines.** Live: `stress case: <name>`, `spike case N/12: <name>` and its `done:` line (fill max, start late
  max, end max, missed, seq breaks, buffer mismatches, frames/expected, pixel check). At the end: the `stress ...`
  lines (now with `| start late max N us, end max N us, N missed`, plus `seq breaks` / `BUFFER MISMATCHES` if any),
  then `---- spike v2: ... decoder vs draw_image: ok ----` and 12 lines like
  ```
  spike rest, 1U, 250 us  fill max NNN us, N over | build NN.N ms (max NN.N), NNN gaps, NNNN B/gap, gap end max NNN us (N past), chunk max NN us, wb max NN us, N stale | frames NNN/NNN | comp keys 0 | ok | start late max NN us, end max NNN us, N missed
  ```
  - `end max` / `missed`: the go/no-go numbers now (start lateness + fill time against the ~615 us budget);
    `fill max` / `over` as before.
  - `seq breaks` / `BUFFER MISMATCHES` / frames below expected: a resync or a lasting desync in that case.
  - `B/gap`, `build`: throughput at that deadline; `gap end max` (`past`): the gap work's latest end after the fill
    (should stay near the deadline); `chunk max` / `wb max`: longest ~1 KB chunk and its write-back.
- Paste the log from the first `stress case:` line to the last `spike` line into step 2. Afterwards, back to
  normal: `idf.py --preset release -DCB_RUN_STRESS=OFF -p COM4 flash`.

**Build state left (v2):** `build/release` built with `CB_RUN_STRESS=ON` (ready to flash); `build/debug` built
normal (`CB_RUN_STRESS=OFF`; the debug stress build was built too, 0 errors). Host tests: 698 passed.

### Spike v2 results (user, run 2, release RUN_STRESS, 2026-11-12) — GO
- **No artifacts seen** on screen (no flicker, no lasting shift).
- All 12 spike cases: fill start late <= 3 us, fills unchanged (144 us at rest / 186 us mid-swipe), 0 missed,
  frames as expected, pixel checks ok.
- Throughput by deadline after the fill's end (bytes per gap -> full page build, keys + margins):
  150 us ~1.6-1.8 KB, ~255-287 ms; 250 us ~3.2-3.5 KB, ~130-140 ms; 350 us ~4.8-5.1 KB, ~89-95 ms. Today's
  compositor builds a full page in ~36 ms, so ~2.5x slower at 350 us; a single key ~5 ms.
- Chunk (~1 KB) max ~82 us, including its write-back, max ~48 us.
- Internal heap 160,867 B free (release + stress; the 8 KB source buffer is gone).
- Existing stress cases (now with the new columns): the Lua cases show fill start late up to 74 / 109 / 139 us
  (the Lua heap's PSRAM writes on core 0; invisible before this measurement; worst end 421 us < 615), the others
  <= 10 us.

### Step 2 decisions (user GO, 2026-11-12)
- **Draw straight from PSRAM** (no source copy, no internal source buffer) with the spike's 32-bit opaque row
  decoder (`decode_row` in `stress_opaque.cpp`; repeat runs with `fill_px`, literals two pixels per store).
- **~1 KB chunks, each written back immediately** after it's drawn (short cache write-backs).
- **The gap deadline is measured from the fill START**, not its end: stop ~65 us before the next fill is due,
  i.e. deadline = fill start + chunk period (budget, ~615 us) - margin (~65 us); a chunk starts only if its worst
  case (~85 us) still fits before the deadline. This adapts to long fills (a long fill leaves less gap) instead of
  a fixed time after the fill's end.
- **Margins and uncovered areas are filled black only when the page layout changes** (not on every build).
- **Keep `screen.cpp`'s new start-late / missed / frame-count measurements permanently** (`screen_stats_t`
  `max_late_us`, `max_end_us`, `missed`, `seq_breaks`, `buffer_mismatches`, and `fill_gap_age_us()`), and keep
  them as columns of the stress lines.
- Byte budgets per gap (`COMP_SLICE_ROWS_*`, "rows per gap = budget / (w x 2)") no longer apply.

### Step 3: geometry (done 2026-11-12)
- `firmware/main/key_geometry.h/.cpp` (in `main/CMakeLists.txt` and `test.mk.js`; nothing uses it yet):
  `KEY_CELL` 144, `KEY_GRID_COLS/ROWS` 5x3, `KEY_GRID_W/H` 720x432, `KEY_GRID_X0/Y0` (40, 4) derived from the page
  area, `KEY_QUARTER` 36, `KEY_GRID_QW/QH` 20x12, `COMP_MAX_KEYS` 32, `KEY_ID_NONE` 0. `KeyRect { u8 x, y, w, h }`
  (quarter units), `KeyPixelRect` (int16, page-area pixels), `KeyRectPixels`, `KeyRectValid` (non-empty, inside the
  grid; computed in int so no u8 wrap), `KeyRectsOverlap` (abutting = no overlap), `KeyRectsCheck(rects, n)` and
  `KeyIdsCheck(ids, n, others, n_others)` returning `KeyGeomResult` (`OK`, `OUT_OF_GRID`, `OVERLAP`, `TOO_MANY`,
  `BAD_ID`, `DUPLICATE_ID`). For SET_KEYS pass the other pages' ids as `others`; for a whole blob pass all ids
  with no `others`.
- `page_compositor.h`'s old `COMP_KEY_*` geometry is untouched; it goes in step 5. `COMP_MAX_KEYS` lives in
  `key_geometry.h` (no clash with `COMP_KEYS` 15).
- fw-test: 706 passed debug and release (8 new). fw-build (debug): 0 errors.

### Step 4: image store and test assets (done 2026-11-12)
- `ImageStore::Init(host, capacity, max_w, max_h, require_opaque)`: any size from 1x1 up to max (was exactly
  w x h); with `require_opaque` an image with any transparent pixel is BadImage (upload, replace and
  `AddExternal`; a refused replace keeps the old image). `ImageEntry` gains `width`/`height` (u16, set on install,
  replace and `AddExternal`; a replace may change them, set before the listener runs).
- **Interim policy:** `model.cpp` and the tests that sit on the 128 compositor (TestKeyLayout, TestLayoutApply,
  TestProtocol, TestActivityHost, and TestImageStore's default fixture) use `Init(..., COMP_KEY_SIZE,
  COMP_KEY_SIZE, false)`, because the harness and the compositor still draw 128 bg/fg layers until steps 5-6.
  Side effect until step 6: the device store now also accepts images *smaller* than 128 in hosted mode, which the
  old compositor asserts on (`comp_add_image_rows`). Harmless as nothing is flashed before step 13; step 6 switches
  the model to `Init(..., KEY_GRID_W, KEY_GRID_H, true)`.
- `test_keys.bin` keeps the 128 faces and icons (the harness, RUN_STRESS's old cases and several suites still need
  them) and **appends the spike's opaque keys**: `TEST_KEYS_KEYS_1U` 15 x 144x144, then `TEST_KEYS_KEYS_2U` 5 x
  288x144, then the names (166 KB). `test_keys.h`: `TEST_KEYS_FIRST_KEY(count)`, `TEST_KEYS_KEY_1U/2U(count, i)`;
  `TEST_KEYS_ICON_COUNT` now stops at the first key. `TestImages`/`StoreEnv` gain `Key1U(i)`/`Key2U(i)`.
- **Spike generator folded in:** `gen.js`'s spike section became `addOpaqueKey` writing into test_keys.bin
  (`addKey` takes w/h); `spike_keys.bin` is deleted; `stress_opaque.cpp` reads the opaque keys from test_keys.bin
  (`main.cpp` passes it a PSRAM copy of test_keys.bin), and `load_demo_images` uploads only the blobs before the
  first opaque key. `ui.bin` regenerated byte-identical.
- fw-test: 710 passed debug and release (4 new: any size up to max, too wide/tall refused, transparent refused
  when required, entry size; "rejects bad images" now checks a 144 key in a 128 store instead of the wrong-size
  pill; "every test key image" covers both sections). fw-build debug normal and debug `RUN_STRESS`: 0 errors.
  `build/release` was not rebuilt (its cache still has `CB_RUN_STRESS=ON`; CMake re-runs on the next build).

### Step 5: compositor core (done 2026-11-12)
- **Types** (`page_compositor.h`): `comp_key_t { KeyPixelRect rect; const void *img; }` (img NULL = background;
  an image whose header doesn't fit the rect, `opaque_image_fits`, is drawn as background, with a debug assert on
  a width mismatch), `comp_page_t { count, keys[COMP_MAX_KEYS] }`, `comp_gap_t { fill_start_us, period_us }`,
  `comp_clock_t`. `comp_get_page_t` fills a `comp_page_t`. `Init(buffers, get_page, is_retired, clock, ctx)` (no
  cell). `Step(now_us, const comp_gap_t *gap)`. Gone: `COMP_SLICE_ROWS_*`, `COMP_RENDER_EST_US`,
  `comp_add_image_rows`, `comp_clear_rows`, `comp_image_is_opaque` (the store checks opacity).
- **Shared decoder:** `main/opaque_row.h/.cpp` (`opaque_image_fits`, `opaque_draw_rows`), the spike's `decode_row`;
  tested against `draw_image` in `test/TestOpaqueRow.cpp` (all 20 test keys, even and odd destination column, row
  ranges); `stress_opaque.cpp` uses it. No test key has a large palette, so that path is untested.
- **Records keyed by rect:** each buffer has a layout (records: rect + image/background + known) and an **erase
  list** (rects that may still hold pixels outside the layout). `Relayout(b, page)` (no pixel writes) gives a
  buffer a page's rects: identical rects keep their record, new ones are unknown, rects left go on the erase list
  (full list -> one bounding rect). An erase unit fills its rect minus the layout's rects (per-row span
  subtraction), so it never touches a key and can run in any order. Invariant (checked by the harness on every
  buffer): outside the layout and the erase list a buffer is background. Erases block the offer and come before
  catch-up/progress keys. So the black fill happens only when a buffer gets a different layout (swiping between
  differently laid out pages, a layout change), never on a normal build; boot needs none (buffers start black,
  no records).
- **Units, chunks, deadline:** a unit is a key (its record is unknown from the start until its last chunk) or an
  erase. `Step` with a unit active draws chunks of ~`COMP_CHUNK_BYTES` (1 KB, at least a row: 3 rows of a 1U
  key, 1 of a 2U), each written back at once, while `clock() + COMP_CHUNK_WORST_US (85) <= fill_start + period -
  COMP_GAP_MARGIN_US (65)`; returns Wait while the unit is unfinished. The next unit is picked by the next `Step`
  (no pixel writes), so one gap can finish a key and continue the next. **Reset no longer waits for a key:** it
  abandons the unit (records go unknown anyway). **A sweep abandons a unit drawing a retired image** (else it
  continues). The swap rule, snapshot, offer, flush, work order and idle copy are unchanged, generalised to rects
  (a back whose layout differs from the front's counts as ahead; keys match the front by rect).
- **Frame-start guard:** a key's time is estimated at `COMP_GAP_BYTES_EST` (3 KB) per fill period.
- **Device (`compositor.cpp`):** no render cell (frees 32 KB internal RAM); `clock` = `esp_timer_get_time`; the
  task keeps the last gap given to it (from `fill_gap_client_t.fill_start_us`) and passes it to every Step (the
  deadline makes a stale one harmless). `fill_gap_done(fill_start_cycles)` (screen.cpp passes its start cycle
  count) records each client's fill start; `FILL_GAP_PERIOD_US` = `DISPLAY_BOUNCE_BUDGET_US`. The status bar keeps
  `fill_gap_fresh(COMP_GAP_FRESH_US)`.
- **Interim adapter:** `KeyLayout::GetPage(page, comp_page_t *)` gives 15 keys, each the slot's 128x128 rect
  centred in its 144 cell (`COMP_KEY_X/Y` now derive from `key_geometry.h`, `COMP_KEY_GAP` = 16), with the shown
  bg (else fg) if it is 128x128, else NULL. Transparent 128 faces/icons don't pass `opaque_image_fits` and draw
  black: visuals wrong until step 6, as planned. The bg/fg view the tests use moved to `KeyShown` /
  `GetShown(page, KeyShown[COMP_KEYS])` (TestKeyLayout, TestLayoutApply, TestProtocol renamed mechanically);
  `COMP_KEY_BG_OPAQUE` moved to `key_layout.h` (step 6 removes both). The legacy slot macros (`COMP_KEYS`,
  `COMP_COLS/ROWS`, `COMP_KEY_SIZE` 128, `COMP_KEY_X/Y`, `COMP_KEY_GAP`) stay in `page_compositor.h` for the touch
  router, protocol, layout blob and key layout until steps 6-8.
- **Harness:** pages are `comp_page_t` with layouts `LAYOUT_1U` (15 x 1U), `LAYOUT_2U` (rows of 2U/1U) and
  `LAYOUT_SPARSE` (1U and 2U keys, odd-sized rects with no image, holes); `World(pages, seed, mixed)` gives page p
  layout p % 3 when mixed; `ChangeLayout`. Simulated clock: `chunk_cost_us` (65) per chunk, fills `fill_one_us`
  150 / `fill_two_us` 190, `gap_delay_us`; `CHUNK_US` 615 (20 MHz), `FRAME_US` = 32 x 615. Checked every step: chunks
  only with a gap, the last started a worst case before the deadline; with `check_gap_writes`, only the drawing
  buffer changes, by at most chunks x a row. Checked every frame: a shown buffer's rects are a layout its page has
  had and it has nothing to erase; versions per rect never go backwards; flushed batches appear whole; a buffer
  coming on screen matches the oracle (opaque images blitted at their rects over black, whole page). Keys with no
  image are pixel-checked but not version-checked (their versions can't be told apart). Mutations checked: no
  erase fill, erases not queued, erase over keys -> the fuzz and layout scenarios fail.
- **Tests:** TestCompositor rewritten (grid abuts/centred, deadline chunk counts, late gap does less / nothing,
  reload abandons a key, layouts: swipe fills uncovered areas, fill only on layout change, flushed layout change,
  sweep abandons a key being drawn, image that doesn't fit draws background; the old opaque-face tests went);
  TestCompositorFuzz: chunk cost and late gaps randomised, layouts and `ChangeLayout` in 6 of 8 runs, new "Fuzz
  Layouts". fw-test: **721 passed, debug and release** (was 710). fw-build debug normal and debug `RUN_STRESS`: 0
  errors (only the usual `times` link warning); `build/debug` left normal (`CB_RUN_STRESS=OFF`). RUN_STRESS
  unchanged in content (step 12): its 128 demo keys draw through the adapter.
- Measured in the harness (simulated, 65 us chunks): a full 1U page builds in <= 8 frames, ~5 chunks a gap.

### Step 6: key layout (done 2026-11-12)
- **`KeyDef { u16 id, u8 page, KeyRect rect, u16 flags, u32 normal, u32 pressed }`** (internal, not the wire).
  Flags: `KEYDEF_DISABLED`; `KEYDEF_ANY_SIZE` (0x8000, interim, below). `KEYDEF_EMPTY`, the bg/fg slots,
  `KeyShown`/`GetShown`, `COMP_KEY_BG_OPAQUE` and the GetPage adapter are gone. `KeyState` (id, rect, flags,
  pressed, `normal`/`pressed_image` entries) per key, `COMP_MAX_KEYS` per page.
- **`SetKeys(first, pages, defs, count, change_flags)`** replaces pages first..first+pages-1 with exactly `defs`
  (pages outside the range untouched; every page of the range reported), all or nothing. Checks in order:
  BadArg (range past the page count, a def's page outside the range, unknown flag), per page `KeyRectsCheck`
  (OutOfGrid / Overlap / TooMany), `KeyIdsCheck` of the defs against the pages outside the range (BadId /
  DuplicateId: a key may keep or move its id within the range), then images (NotFound; BadSize unless the image
  is the rect's pixel size, normal and pressed). **A key may keep an image that is no longer live** (freed while
  in use) by naming it again under the same key id (needed so re-sending a page doesn't fail; a new key can't
  take it). A key that keeps its id stays pressed. Pressed 0 = keep the normal image (no feedback, no change
  report). New refs before old releases, as before.
- Lookup: `Find(id, &page)`, `IsActive(id)`, `SetPressed(id, bool)`, `KeyCount(page)` (-1 past the count),
  `GetKeys(page, KeyDef *out)`, static `ShownEntry(key)`. `GetPage` emits each key's pixel rect and its shown
  image, or NULL (background) when the entry's size isn't the rect's (ANY_SIZE keys, or a replace with another
  size: `ImageEntry` width/height). `OnImageReplaced` just reports the pages using the entry.
- Scratch for SetKeys lives in the class (ids, resolved entries, held flags: ~2.8 KB), so `KeyLayout` grew from
  ~5 KB to ~8 KB of internal RAM (`model.cpp`'s static); step 5 freed 32 KB.
- `key_geometry.h` gains the plain grid helpers `KEY_GRID_SLOTS` (15), `KeySlotRect(slot)`, `KeySlotId(page,
  slot)` = page * 15 + slot + 1 (tested).
- **`model.cpp`: store `Init(KEY_GRID_W, KEY_GRID_H, true)`.** Consequence until step 11: the stored layout
  blob's transparent 128 images are refused by `AddExternal`, so the device boots with an empty Home (not flashed
  before step 13 anyway).
- **Interim callers (replaced by steps 7, 8, 11, 12):**
  - `KeyDefFromSlots(page, slot, flags, ids[4])` (`key_layout.h`): an old slot key as a 1U key in its plain grid
    slot with id `KeySlotId`; normal = bg else fg, pressed = pressed bg else pressed fg.
  - `layout_apply.cpp`: blob slot keys via `KeyDefFromSlots` with `KEYDEF_ANY_SIZE` (the blob's 128 images are
    kept and referenced but drawn as background); slots with no image have no key; Home = `SetKeys(0, 1, ...)`,
    activity = `SetKeys(1, pages, ...)`. All blob images are still registered (fg ones too). The blob-side
    `KEY_SLOTS` / `KEY_SLOT_*` moved from `key_layout.h` to `layout_types.h`.
  - `protocol.h/.cpp`: the old wire struct is now `ProtoKeyDef` (20 bytes, `PROTO_KEYDEF_EMPTY` removes a key,
    `PROTO_KEYDEF_DISABLED`); `OnSetKeys` re-sends the touched page range (`GetKeys`, a heap `List`) with the named
    slots replaced/removed. New refusals (geometry, ids, size) answer `ST_BAD_ARG`.
  - `model.cpp`: `model_key_active` / `model_set_key_pressed(page, key)` use `KeySlotId(page, key)` (touches still
    come as slots until step 7).
  - `main.cpp` RUN_STRESS: demo keys are the 15 opaque 1U test keys (`DEMO_ICON_ID`, pressed = the next one) on
    plain grid pages; the 128 faces/icons are no longer uploaded (they'd be refused), so the "opaque" cases
    repeat the plain ones; `replace_icon` cycles the 1U keys.
- Tests: `TestKeyLayout` rewritten (16 facts: rects/images to the compositor, range replace, every refusal
  changes nothing, ids across pages, ref counts, freed image kept by its key, pressed swap incl. spans, lookup,
  replace, replace with another size, interim ANY_SIZE, page count, clear, external). `TestKeyGeometry` + plain
  grid slots. `TestLayoutApply` / `TestProtocol` moved to `Shown(page, slot)` / `Active(page, slot)` helpers via
  `KeySlotId`; TestProtocol's hosted fixtures upload opaque 1U keys (store `Init(KEY_GRID_W, KEY_GRID_H, false)`:
  `UseHome`'s blob is still transparent 128), plus checks that another SET_KEYS keeps the page's other keys and
  that a 128 image is refused. TestActivityHost unchanged.
- fw-test: **726 passed, debug and release** (was 721). fw-build debug normal and debug `RUN_STRESS`: 0 errors
  (only the usual `times` link warning); `build/debug` left normal (`CB_RUN_STRESS=OFF`).

### Step 7: touch hit-testing (done 2026-11-12)
- **`touch_router.h`:** `TouchKey { u16 id; KeyPixelRect rect }` (page-area pixels); pure `touch_in_rect(rect, x, y,
  margin)` (screen point, page at rest: y offset `SCREEN_PAGE_TOP`) and `touch_key_at(keys, count, x, y, margin)`
  (index or -1). `TouchRouterHost` gains `page_keys(page, out, ctx)` (count, up to `COMP_MAX_KEYS`); `key_active`,
  `key_event` and `key_look` take the key **id** (u16; the page stays, the one touched down on). The router calls
  `page_keys` once at touch-down into a member scratch (`m_keys[COMP_MAX_KEYS]`, 320 B, not the swipe task's stack),
  hit-tests with the 12 px zone, and keeps the key's id and rect: leaving the key (Pending or Held) is judged
  against that rect (no host call per move). It includes `key_geometry.h` instead of `page_compositor.h`.
- **Device:** `KeyLayout::PageKeys(page, &count)` (the live `KeyState`s in place; tested). `model.h/.cpp`:
  `model_page_keys(page, TouchKey *)` (under the model lock, rects via `KeyRectPixels`), `model_key_active(page,
  id)` (active **and** on that page: a key that moved to another page counts as gone), `model_set_key_pressed(id,
  pressed)`. `swipe.cpp` wires `page_keys`; `swipe_set_key_cb`'s callback takes `(page, uint16_t id, ...)`.
- **Interim id -> slot mapping** (until steps 8 and 11): `KeyIdSlot(page, id)` in `key_geometry.h` (the inverse of
  `KeySlotId`, -1 if the id isn't one of that page's plain grid slots; tested). `host_link.cpp`'s `on_key` maps the
  id back to a slot before `Protocol::PostKey` / `standalone_key`, and drops (with a `ESP_LOGW`) a key that isn't a
  grid slot (none today: every key is still made from a slot by `KeyDefFromSlots` / the interim protocol). Step 8
  (KEY events by id) and step 11 (standalone `touch[id]`) remove it.
- **Tests:** `TestTouchRouter.cpp` rig: per-page `TouchKey` lists (plain grid with `KeySlotId` ids by default, a
  "mixed" page: a 2U span, a 1U key, two 0.25U keys, a side-by-side group and a stacked group of half-height keys;
  an empty page), inactive by id, `key_active` = on the page and not inactive. Old gap points re-pointed at the
  grid margin and the seam (edge zones). New facts: hit test seams (24 px swipe-only strip, rows too), margins,
  span with no inner seam, 0.25U keys (12 px active strip), group seams, empty areas; router: the page's own
  layout, span presses as one key and holds across its width, quarter key, group seam only swipes, empty area
  only swipes, held key removed from its page cancels. Fuzz: random layouts per page (grid / mixed / empty)
  mid-touch, and checks that a PRESS is for an active key on the touched page and RELEASE/CANCEL name the
  pressed key. `TestKeyGeometry` (`KeyIdSlot`), `TestKeyLayout` (`PageKeys`).
- fw-test: **737 passed, debug and release** (was 726). fw-build debug normal: 0 errors (usual `times` link
  warning). RUN_STRESS not rebuilt (nothing it uses changed).

### Step 8: protocol v2 (done 2026-11-12)
- **`PROTO_VERSION` 2.** docs/protocol.md rewritten for v2 (design summary, status table, GET_INFO, PUT_IMAGE,
  SET_KEYS, KEY, typical session, a new *Version history*). Other docs (image-assets.md etc.) still describe v1
  keys until step 14.
- **GET_INFO:** unchanged order; `cols`/`rows` = `KEY_GRID_COLS/ROWS` (5, 3), `key_w`/`key_h` = `KEY_CELL` (144),
  `icon_w/h` 80. **Appended** after `layout_crc` (15 bytes): u16 `grid_x` 40, u16 `grid_y` 44 (screen pixels:
  `SCREEN_PAGE_TOP + KEY_GRID_Y0`), u16 `grid_w` 720, u16 `grid_h` 432, u16 `quarter` 36, u16 `max_image_w` 720,
  u16 `max_image_h` 432 (`KEY_GRID_W/H`, the store's limit in `model.cpp`), u8 `max_keys` 32. Response buffer grew
  to 96 + 2 x 256.
- **SET_KEYS KeyDef (20 bytes):** u16 id, u8 page, u8 x, y, w, h (quarter units), u8 reserved, u16 flags, u16
  reserved, u32 normal, u32 pressed. Flags: `PROTO_KEYDEF_REMOVE` 0x0001 (replaces v1's EMPTY), `DISABLED`
  0x0002; any other bit (incl. the internal `KEYDEF_ANY_SIZE`) is BAD_ARG. Parsed with the `Reader` (no wire
  struct; `ProtoKeyDef` is gone).
- **Semantics: edits by id.** A def adds a key or replaces the one with its id; REMOVE removes it (unknown id:
  ignored); keys not named stay. The device collects the touched pages' keys (`GetKeys`, a heap `List`, as the
  interim did), minus every named id, plus the new defs, and calls `SetKeys(lo, hi-lo+1, ...)` over the range of
  touched pages (each def's page and each removed key's page), so the whole message is checked on the result
  (moves/swaps within a page work in one message). Refusals: id 0 or an id twice in the message (`KeyIdsCheck`
  on the message's ids) -> BAD_ARG; an existing id named on another page -> BAD_ARG (a key stays on its page);
  page past the count, out of grid, empty, overlap, too many -> BAD_ARG; image missing -> NOT_FOUND; **image not
  the rect's size -> BAD_IMAGE** (`KeyLayoutResult::BadSize`); BAD_LENGTH for a short payload.
- **KEY event (12 bytes):** u16 id, u8 page, u8 action, u8 lost, u8[3] reserved, u32 time_ms (`lost_byte` 4;
  `Event.data` grew to `PROTO_KEY_EVENT_SIZE` 12). `PostKey(page, uint16_t id, action, time_ms)`.
- **Non-opaque upload refused:** the store already does it (`require_opaque`, step 4); TestProtocol's fixture now
  inits the store opaque-only like the device (`Fixture(capacity, hosted, opaque = true)`); the UseHome tests pass
  `opaque = false` (the sample blob's transparent 128 images, until step 11).
- **Interim mappings removed from the hosted path:** `ProtoKeyDef`/`KeyDefFromSlots` gone from protocol.cpp;
  `host_link.cpp` `on_key` posts the id straight to `PostKey` when hosted. **Still interim (until step 11):**
  standalone keys go through `KeyIdSlot` in `on_key` (dropped with a warning if not a plain grid slot), and
  `layout_apply.cpp` builds keys with `KeyDefFromSlots` (its only user now).
- Tests (TestProtocol): Get Info (all v2 fields, `m.len` exact), Set Keys (any id, rect, pressed swap), spans and
  quarter units (2U + offset 1U + disabled, pixel rects via GetPage), edits by id (redefine/move in page, refused
  cross-page id, remove then define elsewhere, unknown remove, in-page move + new key + other page in one message,
  empty message), refusals (16 cases, each alone and with a good key; too many keys), length errors, non-opaque
  upload refused (and a refused replace keeps the old image), KEY event layout and overflow flag at byte 4.
  fw-test: **741 passed, debug and release** (was 737). fw-build debug normal: 0 errors (usual `times` warning).
  host `npm test`: 229 passed (host/ unchanged; its FakeDevice and ControlBlock.js still speak v1 until step 9).

### Step 9: host library (done 2026-11-12)
- **`protocol.js`:** `PROTOCOL_VERSION` 2 (connect refuses another version, as before), `KEY_EVENT_SIZE` 12,
  `KeyFlags { REMOVE, DISABLED }`. `decodeInfo` adds `gridX`, `gridY`, `gridWidth`, `gridHeight`, `quarter`,
  `maxImageWidth`, `maxImageHeight`, `maxKeys`. `decodeEvent` KEY: `{ type: "key", id, page, action, lost, timeMs }`.
  `encodeKeys(keys)`: `{ id, page, x, y, w = 1, h = 1, normal, pressed, disabled }` in grid units (multiples of
  0.25, converted to quarter counts) or `{ id, remove: true }`; images are IDs or anything with `.id`; throws
  RangeError for what can't be encoded (id 0 / > 65535 / non-integer, a value not a multiple of 0.25, w/h 0,
  negatives, > 63.75U, page > 255, image ID out of u32). Grid/overlap/size checks are left to the device.
  New pure helpers `keySlot(slot, cols = 5)` -> `{ x, y, w: 1, h: 1 }` and `keyPixelSize({ w, h }, info)` ->
  `{ width, height }` (from `info.quarter`).
- **`ControlBlock.js`:** `setKeys(keys)` (same shape as `encodeKeys`), new `removeKeys(ids)`; "key" event doc.
- **`images.js`** (Node): `keyImage(layers, width, height)` -> `{ id, data, width, height }`, one opaque image:
  `composeOverBlack(layers, width, height)` (sharp composite over an opaque black canvas, alpha forced to 255;
  a layer `{ rgba, width, height, x, y }` is centred when x/y are missing and must fit, else it throws).
  `svgLayer(svg, w, h)`, `buttonLayer(def, w, h)`, `renderButtonImage(def, w, h = w)` (non-square now; the
  bake's `render-node.js` call is unchanged), `centreImage(rgba, w, h, outW, outH = outW)`, `FACE_MARGIN` 8
  (128 face in a 144 cell; step 10 can import it rather than define its own). **Removed:** `keyImageFromSvg`,
  `buttonKeyImage` and the old `keyImage(rgba, w, h, size)` (transparent 128 images the device now refuses).
- **`test/FakeDevice.js`** now models the firmware's screen side: `DEVICE` constants (checked against
  `key_geometry.h`, `screen_layout.h`, `key_layout.h`, `protocol.h` by a test), the v2 GET_INFO (always with the
  layout fields, then the v2 fields; page count, current page and image count/used from the model), an image
  store (`PutBegin`/`PutEnd` rules: BAD_ARG / NO_SPACE / BAD_SEQUENCE; on the last chunk `unpackRle` + every
  alpha 255 + size 1x1..720x432, else BAD_IMAGE; a replace mutates the entry in place, a refused one keeps it),
  FREE/QUERY, SET_PAGE_COUNT (drops pages' keys, clamps the current page), SET_PAGE, SET_STATUS_TEXT (lengths
  only), RESET / TAKE_CONTROL / RELEASE_CONTROL clear it, and SET_KEYS exactly as `Protocol::OnSetKeys` +
  `KeyLayout::Check` (same check order and statuses; a key keeps a freed image it holds; kept keys are re-checked
  by image ID, so like the firmware an unrelated edit on a page whose key image was replaced with another size
  gets BAD_IMAGE). `dev.keys(page)` (quarter units, image IDs), `dev.images`, `dev.pageCount`, `dev.currentPage`,
  `dev.statusText`, `dev.keyEvent(id, action, { timeMs, lost })` (throws for standalone / unknown / disabled keys,
  as the device sends none). **`handler` changed meaning:** asked first for screen requests; `undefined` now
  falls through to the model (`null` still = silent), so existing handler tests are unchanged. The putImage
  chunking test (not an image) passes a handler.
- Tests: protocol (KeyDef bytes, encode refusals, slot/pixel helpers, v2 GET_INFO, 12-byte KEY), images (layers,
  compose, opacity of the encoded result, spans), ControlBlock (headers match, v2 info, version refusal, opaque
  upload/replace, setKeys by id with rects and sizes, 9 refusal cases alone and with a good key + raw unknown
  flag / short payload, edits by id incl. swap and remove, freed image kept, page count / reset / takeControl,
  KEY events by id), entries. **host 245 passed** (was 229), cli 27, examples 17, urem2 193. Bake untouched.
- **demo.js / bench.js adapted, not run on a device (the user must run them):** demo renders each key's normal
  and pressed images (face at key size minus `FACE_MARGIN`, icon+label layer, composed over black), ids
  page * 15 + slot + 1, page 2 has a 2U key (id 100) and two 1U x 0.5U keys stacked in slot 13 (ids 101/102),
  Power by id; bench uploads a noisy opaque 1U image x 40 and a page of 30 realistic key images (KB/s per page:
  the deflate decision). Both smoke-run against FakeDevice with `node scratch/demo-fake.mjs demo|bench`
  (scratch, gitignored). examples/README.md and host/README.md updated.

### Step 10: bake (done 2026-11-12)
- **Authoring format** (layout file version stays 1; for layout-format.md in step 14; the example is
  `exampleKeys()` in `host/test/bake/fixtures.js`, tested in `keys.test.js`):
  - A page's `keys` list: entry i (not null) goes in **slot i** of the 5x3 grid (row-major), 1U, unless it gives
    **`x` and `y`** (grid units from the top-left, multiples of 0.25; both or neither). **`w`, `h`** (default 1,
    multiples of 0.25, > 0) size it; without x/y a span starts at the entry's slot (`[{ w: 2 }, null, ...]`).
    Entries past index 14 have no slot and need x/y. `null` = no key. Up to 32 keys a page (group members count).
  - **`id`** (1..65535) optional. Default: a key whose top-left is on a whole unit gets that slot's plain grid id
    **g * 15 + slot + 1**, g = the page's number **across the whole layout** (Home 0, then each activity's pages
    in file order), so plain grids keep `KeySlotId` ids and ids are unique layout-wide; a key whose corner is
    fractional gets the lowest free id above `pages * 15`, in layout order. Duplicate ids (named or default) are errors at the later key.
  - **Group**: an entry with its own **`keys`** array: `{ x?, y?, w, h, <look: color, face, label, icon...>,
    keys: [ member, ... ] }`. The group's face and its own icon/label are drawn once across the group's rect;
    each member is a key (own `id`, gestures, `longPressMs`, icon/label) at `x`/`y` (default 0 each) and `w`/`h`
    (default 1) **relative to the group**. Members inherit the chain plus the group's look minus its
    icon/iconText/label/image (`groupLookForKeys`); they may not set `face`, `color`, `image` or `pressed.face/color`
    (error: set it on the group). A group has no gestures and no id (unknown property); no nested groups. Members
    must lie inside the group and not overlap; a warning if they don't cover it (that part shows black). The group
    is one item for the page's overlap check.
  - Rect checks: inside the 5x3 grid, no overlaps (abutting is fine), errors name the earlier item.
  - Bindings: `touch[id]` (activity bindings: its pages' touch bindings in page/key/gesture order, then its own).
- **Tree / blob (BLOB_VERSION 2):** key = `{ id: u16, rect: byte[4] (x, y, w, h quarter units = the firmware's
  KeyRect), normal, pressed (bytes), longPressMs }` (28 bytes; binpack pads every `byte` field to 4, so the rect is
  a `byte[4]`); `page` = `{ keys_count, keys* }`; `home` = `{ keys_count, keys*, bindings }`. Empty slots have no
  key; a key with no look has `normal`/`pressed` absent (step 11: draw as background). The committed
  `layout_types.gen.h` is now stale on purpose: `types-header.test.js`'s drift test is `todo` until step 11
  regenerates it (remove the `todo` there).
- **Images:** stage 1 makes per state an image spec `{ width, height, layers: [{ layer: "bg"|"fg", def, x, y,
  width, height }] }` (`look.js` `imageSpecs`; null if nothing is drawn). Face box = the rect minus `FACE_MARGIN`
  (8) each side (`faceBox`); faces are SVG at the face box size (`faceSvg(..., size)`), fg layers get
  `virtualSize` = 72 per 128 px both ways when not 128x128 (so 1U definitions are unchanged; text keeps its size on
  spans; icon size follows the face height, node-buttonImage's rule). Group members: the group's face/fg layers
  placed at `faceBox(groupW, groupH, memberOffsetX, memberOffsetY)` (negative offsets) under the member's fg; the
  pressed spec uses the group's pressed face (overlay) + pressed fg, so a member presses alone.
- `render-node.js`: renders each layer once per renderer (memo by size + `stable(def)`, `stable` now exported
  from `render.js`), clips it with the new `images.js` `clipLayer(layer, w, h)` (pure row copy; tested) and
  composes with `composeOverBlack`, encodes opaque rle565. `render.js`/`pack.js` walk `IMAGES` (`tree.js`);
  `allKeys` reports layout paths through a WeakMap (`setKeyPath`), so render errors name `home.keys[4].keys[1].normal`.
- New `host/src/bake/keys.js` (placement, groups, rect checks, ids, `touchBindings`); `resolve.js` numbers pages,
  calls `assignIds` once every page is resolved, then prepends touch bindings. consts: `KEY_CELL` 144,
  `KEY_QUARTER`, `GRID_SLOTS`, `MAX_KEYS_PER_PAGE` 32, `MAX_KEY_ID`, `FACE_SIZE` 128, `FACE_MARGIN` (now the one
  definition; `images.js` re-exports it); `KEY_SIZE`/`KEYS_PER_PAGE` removed (`entries.test.js` updated).
  `render-real.test.js` checks the constants against `key_geometry.h` and renders 1U, span, fractional and group
  keys for real (opaque, sizes, seamless group seam, per-member pressed). Visual check of the example:
  `node scratch/keys-png.mjs` (gitignored) writes `scratch/keys-png/normal.png` / `pressed.png`; looked right.
- urem2/cli adapted minimally (urem2 layouts unchanged, still slot lists): counts `k.normal` instead of
  `k.normalFg`, controls `touch[1]`.., `look.test.mjs` flattens specs to layer defs, `bake.test.mjs` counts
  `normal`/`pressed` images; cli reads `keys[0].normal`. **Suites:** host 262 pass + 1 todo, cli 27, urem2 193,
  examples 17. Firmware untouched (its sample blob and generated header are still v1).

### Step 11: firmware blob side (done 2026-11-12)
- `layout_types.gen.h` regenerated (key 28 bytes: `id` u16 + pad, `rect[4]`, `normal`/`pressed` (count, ptr),
  `longPressMs`; `page` = `{ keys_count, keys* }`, `home` = `{ keys_count, keys*, bindings }`, root 40 bytes).
  The host drift test's `todo` is gone; `render-real.test.js` also checks `BLOB_VERSION` against
  `LAYOUT_BLOB_VERSION`.
- **Sample** (`firmware/test/data/sample_layout.json`, re-baked, 105 KB): Home gains a 2U span (slot 10, id 11, its
  own `longPressMs` 900, tap `w`), two 0.5U keys on row 2 (x 2 -> id 13 by the slot rule; x 2.5 -> assigned id 61,
  the first free above 4 pages x 15), and a 2U group "Vol" at (3, 2) with members id 14 (+) and 15 (-). Listen's
  page gains a second key `{ color: "#aa2222" }` (id 47) whose normal image is identical to Home's key 5, so
  Home/activity image sharing is still exercised (the old shared 128 face no longer exists: every key is its
  own composite). Ids: Home 1,2,3,5,7,8,10,11,13,61,14,15; Watch 16 / 31; Listen 46, 47. Keys 5 and 10 share their
  pressed image. `scratch/dump-sample-tree.mjs` (gitignored) prints a layout's ids, rects and bindings.
- **`layout_types.h`:** no `KEY_SLOT*`; `LAYOUT_KEY_NORMAL/PRESSED/IMAGES` with `LayoutKeyImageOffset/PtrOffset`;
  `LayoutKeyRect(rect[4])` -> `KeyRect` (static_assert the field is 4 bytes); includes `key_geometry.h` instead of
  `page_compositor.h` (no page-size static_assert any more).
- **`layout_blob`:** `LAYOUT_BLOB_VERSION` 2 (1 -> `BadVersion`). New `LayoutError::BadKey` (rect empty / outside the
  grid / overlapping on its page, id 0 or used twice anywhere in the layout: the validator keeps a `Set` of ids, Home
  then activities in order). Keys per page > `COMP_MAX_KEYS` -> `BadLimit`. Images: opaque (`check_image`'s opaque
  out; cached per offset only once opaque) and exactly the rect's pixel size (checked per key, so a shared image
  must suit every key using it) -> else `BadImage`. A key with no images is valid (drawn as background).
  `LayoutSummary` now has `home_count` + `LayoutKey home[COMP_MAX_KEYS]` (`LayoutKey { id, rect, normal, pressed }`,
  ~25 B each); `LayoutKeyOf(const key &, blob)` gives the same from a relocated blob.
- **`layout_apply`:** one `StageKey` (register the key's not-yet-live images, stage a `KeyDef` with the blob's id and
  rect) for Home (from the summary) and activities (`LayoutKeyOf`); every blob key becomes a key, no flags.
  `CollectIds` walks keys by count.
- **Standalone on ids:** `TouchControlName(id, buf)` -> `"touch[id]"`; `Standalone::Key(id, action, time)`;
  `KeyLongPressMs(id)` searches Home's keys, then the selected activity's pages (0 = layout default);
  `standalone_key(id, action, time)`; `host_link.cpp`'s `on_key` passes the id straight through.
- **Interim mappings removed:** `KeyDefFromSlots`, `KEYDEF_ANY_SIZE` (`KEYDEF_FLAGS` = `KEYDEF_DISABLED`; protocol
  test's `any_size` case dropped, `0x8000` stays covered by `bad_flag`), `KeyIdSlot` (and its test), the slot
  mapping in `on_key`. No interim key mapping is left. What remains of the legacy slot geometry (`COMP_KEYS`,
  `COMP_KEY_SIZE`, `COMP_KEY_X/Y`... in `page_compositor.h`) is used only by `main.cpp`'s RUN_STRESS and the host
  tests' 128 images (TestCompositor's slot-geometry check, TestImageStore's 128 fixture): step 12.
- Tests: TestLayoutBlob (sample ids/rects/opaque sizes, shared pressed image, `LayoutKeyOf` = summary, key without
  images, version 1 refused, bad rects, bad/duplicate ids incl. across activities, keys per page cap, non-opaque
  image, pressed image of another size; header/relocation tests re-pointed: `HomeKey(b, i)` in `layout_env.h`,
  `CB_HOME_KEYS(_PTR)`, `CB_KEY_ID/RECT`); TestLayoutTypes; TestLayoutApply (rewritten by id: rects, GetPage draws
  every key, pressed for 1U/2U/0.5U, key with no images, layout refusal unstages, activity pages by id); TestKeyGesture
  (`touch[id]`); TestStandalone (`K_*` id constants, own long press for Home and for the selected activity's key,
  span/fractional/group keys run by id); TestActivityHost; TestTriggerDispatch (control names); TestProtocol (store
  always opaque: the `opaque` fixture parameter is gone; `ShowsHome(i)` by id; hosted uploads use `Key1U`).
  fw-test: **750 passed, debug and release** (was 741). fw-build debug normal and debug `RUN_STRESS`: 0 errors (usual
  `times` warning); `build/debug` left normal. host 264 (0 todo), cli 27, urem2 193, examples 17.

### Step 12: stress run (done 2026-11-12; device only, run by the user)
- **Compositor stats** (`comp_stats_t`, tested in TestCompositor: "Stats Report Chunks Gaps Bytes And Deadline
  Overruns", "Stats Count Erases"): `erases`, `chunks`, `gaps` (gaps that drew; a gap's several Steps count once, by
  its fill start), `bytes`, `max_chunk_us` (by the injected clock, write-back included), `max_gap_end_us` (latest end
  of a gap's work from its fill start), `late_gaps` (ended past fill start + period - margin: a chunk over
  `COMP_CHUNK_WORST_US`). These replace the spike's `B/gap`, `chunk max`, `gap end max`/`past`. The spike's `wb max`
  and `stale` are gone (chunk max includes the write-back; the compositor has no stale gaps, only shorter ones).
- **Spike removed:** `stress_opaque.cpp/.h`, its CMake lines (`STRESS_SRCS`) and `main.cpp` hooks;
  `FILL_GAP_MAX_CLIENTS` back to 2. `fill_gap_age_us()` stays (decision; `fill_gap_fresh` uses it). Legacy
  `COMP_KEYS` / `COMP_COLS/ROWS` / `COMP_KEY_*` removed from `page_compositor.h` (TestCompositor's slot check gone;
  TestImageStore uses the new `TEST_KEYS_FACE_SIZE` 128 in `test_keys.h`).
- **test_keys.bin keeps its 128 faces and icons** (flagged): the host tests still need small and transparent images
  (TestImageStore ~50 uses, the non-opaque refusals in TestImageStore/TestProtocol, TestOpaqueRow, TestKeyLayout).
  The device's stress run uploads only the 20 opaque keys. Not regenerated (gen.js: a comment only), so `ui.bin`
  and `test_keys.bin` are unchanged.
- **`main.cpp` RUN_STRESS, ported:** images = the 15 opaque 1U (`DEMO_1U_ID`) and 5 2U (`DEMO_2U_ID`) keys. Page
  layouts in quarter units: *plain* (15 x 1U) and *mixed* (`MIXED_RECTS`: 2U spans, 1U keys at x 0.5U/1.5U, two
  0.5U keys with no image = background; 12 keys). Pages 0 and 2 plain, page 1 mixed, ids `page * 32 + i + 1`. Each
  case: set the layouts, scroll, settle 600 ms, then 2 s of load. 16 cases: at rest incr/flush, spans at rest,
  mid-swipe (pages 0|1: plain|mixed) incr/flush, **page build 1U / spans** (every key in view changed with
  COMP_FLUSH, timed until the compositor's swap, to the 10 ms tick), **layout changes** at rest / mid (the pages in
  view switch plain <-> mixed every round: erases), status bar only / + keys, image replaces at rest / mid, Lua
  churn at rest / mid / + keys. `comp_get_stats` resets, so cases sum it (`comp_add_stats`). ~50 s.
- **Log:** header `---- stress: fill budget N us; compositor: ~1024 B chunks, chunk worst case COMP_CHUNK_WORST_US 85
  us, margin COMP_GAP_MARGIN_US 65 us (gap deadline N us after the fill start), guard estimate COMP_GAP_BYTES_EST
  3072 B/fill ----`, the demo load line, then per case:
  ```
  stress <case> fill max N us, N over | keys N (max N us), N erases, busy N% | N B/gap, chunk max N/85 us, gap end max N/550 us, N late | frames N/N [| build N ms (max N), N builds] | start late max N us, end max N us, N missed [, seq breaks, BUFFER MISMATCHES] [late split]
  ```
  `chunk max N/85`: slowest chunk against `COMP_CHUNK_WORST_US`; `gap end max N/550`: against the deadline (period
  - `COMP_GAP_MARGIN_US`); `late` > 0 means the worst case is too small. Tuning: if `end max` (fill) stays well under
  615 us and nothing is late, the margin can shrink / the worst case can come down toward `chunk max` (more B/gap,
  faster builds); `missed` or BUFFER MISMATCHES mean the other way.
- **Not ported from the spike:** its per-case pixel CRC (the host harness checks pixels) and the boot-time
  decoder-vs-`draw_image` check (TestOpaqueRow does it).
- fw-test: **752 passed, debug and release** (was 750). fw-build: debug normal, debug `RUN_STRESS` and release
  `RUN_STRESS`: 0 errors (usual `times` warning). **`build/release` is left built with `CB_RUN_STRESS=ON`** (ready
  to flash: `idf.py --preset release -DCB_RUN_STRESS=ON -p COM4 flash monitor`; afterwards `-DCB_RUN_STRESS=OFF`);
  `build/debug` built normal. Not run on the hardware yet (step 16's checklist).

### Step 13: urem2 and examples (done 2026-11-12)
- **Everything already baked clean** after step 10/11's minimal adaptation (urem2 is plain slot lists; grid() and
  padPage() unchanged and still work). No layout content needed migrating. activity-test.mjs keeps its two old,
  intentional warnings ("has a long gesture but no tap" on the hold keys).
- **Blob sizes, before (old 128 transparent layers, scratch bakes of 2026-11-07..09) -> now (opaque 144 per state):**
  urem2 ~148 KB -> 440 KB (449,940 B real, 450,200 B test; 124 distinct images, 11 pages; README said ~134 KB / 56
  images), activity-test 286 KB -> 671 KB (192 images), net-test 53 -> 119 KB, script-test 80 -> 136 KB,
  upload-test 26 -> 45 KB, new key-shapes 71 KB (24 images). As predicted (~3x).
- **New shared test helper `examples/bake-check.mjs`** (`bakeChecked(file)`, `checkTreeKeys(tree)`, `treePages`):
  bakes as the CLI does and checks the keys like `layout_blob.cpp`: ids 1..65535 unique layout-wide, rects inside the
  grid without overlaps, <= 32 keys a page, every image opaque (decoded with `rle565/unpack.js` from
  `host/node_modules`) and exactly the rect's pixel size, and every `touch[id]` binding names a key of its own scope
  (Home's keys / the activity's pages). Used by `examples/layouts.test.mjs` (all five example layouts, expected
  warnings listed) and `urem2/layout/bake.test.mjs` (now bakes **both** entries, and checks every urem2 key is a 1U
  slot key with id g*15+slot+1 and both images). urem2 already imported `examples/test-common.mjs`, so the
  cross-import isn't new.
- **New example `examples/key-shapes.mjs`** (Home only, flat faces): 2U span "Wide", 1U, grouped **Volume -/+**
  (w 2, `labelVAlign: "bottom"`, member icons `#volume-down`/`#volume-up` with `iconOffset` y -7 so they clear the
  group label: without that the centred label ran through the icons), half-height A/B, 2x2 "Big", half-width L/R,
  2U-tall "Tall", Esc, and a 0.5U "q" at (3.25, 2.25) with `id: 500`. Each key types a different HID key (w, 1,
  minus, plus, a, b, g, left, right, t, esc, q), so a text editor shows which key fired: for step 16's look/press
  checks. Ids: 1, 3, 4, 5, 6, 16 (B), 7, 9, 17 (R), 10, 11, 500 (tested). Visual check: `node scratch/layout-png.mjs
  examples/key-shapes.mjs` (gitignored; writes `scratch/layout-png/normal.png` / `pressed.png`, holes in dark red).
- READMEs: examples (key-shapes, bake-check/layouts.test rows), urem2 (bake size, both-entry test, keys are 1U with
  default ids). **For step 15 (CLAUDE.md proposal):** the Repository Layout's `examples/` line should list
  `key-shapes.mjs`, and "From the repo root: node --test examples/**" now also bakes every example layout.
- **urem2 pages with holes** (for the user: candidates for spans or groups; column 0 is empty everywhere, urem's
  navigation column; `scratch/urem2-holes.mjs` prints the map):
  - Home: empty 0, 3, 5 (row 0 `.XX.X`, row 1 `.XXXX`, row 2 full).
  - Pad pages (atv/hdd/steam pages[0]): empty 0, 5, 10-13; **Vol+ (4) / Vol- (9) are a natural vertical Volume
    group**, mute at 14; the empty bottom row (10-13) could take a wide key. Steam's pad page also has 1 and 3 empty
    (unbound cross/tick).
  - atv.pages[1] (transport): empty 0, 3, 4, 5, 8, 9 (right of the transport keys: a 2x2 hole at 3-4/8-9).
  - hdd.pages[1], [2]: only column 0 (0, 5, 10). hdd.pages[3]: 0, 5, 7, 8, 10.
  - steam.pages[1]: 0, 3, 5, 8, 10 (a 1x2 hole at 3/8). steam.pages[2]: 0, 5, 7-10, 12-14 (mostly empty).
  - playstation.pages[0]: only 10 (7ch Stereo) and 14 (mute): 13 empty slots.
- Suites: urem2 **194** (was 193), examples **23** (was 17), cli 27, host 264.

### Step 14: docs (done 2026-11-12)
- **docs/page-compositor.md** rewritten for opaque rects: keys `{ KeyPixelRect, img }` up to 32 a page, records by
  rect, the erase list (`Relayout`, black fill only on a layout change), drawing straight into PSRAM in ~1 KB chunks
  with immediate write-back, the deadline from the fill start (`COMP_GAP_MARGIN_US` 65, `COMP_CHUNK_WORST_US` 85,
  ~550 us after the fill start), the spike v2 table, the new "Fill timing" subsection (start-late / missed /
  seq breaks / buffer mismatches and the IDF EOF-count lasting-shift theory), stats (`comp_stats_t` and the stress
  columns), reset/sweep abandon a unit, no render cell, costs (1U key ~5 ms, full page ~90 ms, ~2.5x), memory,
  measurements to make. Anchors other files use are kept (`#keys`, `#swipe-start`, `#reload`, `#image-pointers`,
  `#changes-to-screen-buffersmd`); new: `#layout`, `#costs`, `#drawing-a-key-straight-into-psram-in-fill-gaps`.
  Frame now ~19.7 ms (20 MHz); swipe-start hold estimated ~6 frames for a full page (not judged on the hardware).
- **docs/image-assets.md:** one opaque image per state, any size up to 720x432, sizes = key rect pixels (checked at
  SET_KEYS / blob apply, not upload), non-opaque refused (`require_opaque`, also `AddExternal`), current
  `ImageData`/`ImageEntry` structs, pressed 0 = no feedback, replace with another size draws background, sweep
  abandons a unit, upload sizes (deflate later).
- **docs/interactions.md:** hit test by key rect at touch-down (`page_keys`, `touch_key_at`), 12 px edge zone on every
  side (~4 px into a face), 24 px seams incl. inside groups, spans have no inner seam, 0.25U keys keep a 12 px active
  strip, the touch-down rect kept for the exit test, key moved to another page cancels.
- **docs/layout-format.md:** new **Keys** section (cell 144, 0.25U steps, x/y/w/h, slot default, ids unique across
  the layout with the `g*15+slot+1` rule and the fractional rule, groups and their rules, 32 keys a page, the
  `examples/key-shapes.mjs` keys with their ids); look = two layers composed into one opaque image per state, the
  8 px margin / 128 face, virtual size scaling; pipeline table (`touch[id]`, key `{ id, rect, normal, pressed,
  longPressMs }`); new validation errors/warnings; the example's ids (Mute 20, back 21, Listen 31); tests.
- **docs/standalone.md:** status bullet for the redesign, `TouchControlName(id)` / `Key(id, ...)` notes in the
  milestone history, schema (blob v2, key `{ id, rect[4], normal, pressed, longPressMs }`, `key[]`), validator checks
  (`BadKey`, `BadLimit`, opaque + rect size images, v1 refused), summary (`LayoutKey`), activity apply, per-key
  long press by id for activities, triggers and gestures use `touch[id]`.
- **docs/screen-buffers.md:** status line and the "Key render timings" section marked superseded.
- Unchanged (already current or nothing about keys): docs/protocol.md (checked: consistent), docs/status-bar.md,
  host/README.md (step 9), cli/README.md.
- **Left in code comments (not touched; a later code step):** `page_compositor.h` says the spec is "being updated
  for opaque keys" and `key_geometry.h` says "Spec: wip/... until the docs are updated": both can now point at
  docs/page-compositor.md and docs/layout-format.md / protocol.md.

### Step 15: CLAUDE.md changes (approved by the user and applied 2026-11-12)
Applied as proposed below (items 1-8; item 9 needed nothing). One wording merge: the protocol bullet reads
"every request (`PROTO_VERSION` 2: ...; also SET_STATUS_TEXT: ...)" rather than two parentheses in a row. Code
comments that pointed at this doc "until the docs are updated" now point at the docs: `page_compositor.h` (spec and
the spike measurement: docs/page-compositor.md), `page_compositor.cpp`, `key_geometry.h` (docs/layout-format.md
Keys, docs/protocol.md SET_KEYS) and `key_layout.h` (docs/image-assets.md, Key definitions and references; also had
the stale note). `opaque_row.cpp`'s "from the key render spike, wip/... step 2" is history and was left. fw-build
debug normal: 0 errors (usual `times` warning); fw-test: 752 passed.

### Step 16: hardware checklist (for the user)
All firmware commands from PowerShell in `firmware/`, after:
```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1
```
Node commands from the repo root. Power the board from the native USB port (Wi-Fi). **Throughout: watch for flicker,
torn lines (especially the left side) and a lasting shift (status bar clock moved up / wrapped to the bottom); note
what was happening when it appeared.**

**A. Stress run (release, `RUN_STRESS`).** `build/release` is currently configured and built with `CB_RUN_STRESS=ON`.
1. Full flash (not app only) and monitor: `idf.py --preset release -DCB_RUN_STRESS=ON -p COM4 flash monitor`. Check for
   `Hash of data verified.` (no "checksum mismatch").
2. Wait ~50 s (the board looks hung; each case is logged as it starts, results at the end). Watch the screen during it.
3. Paste the log from `---- stress:` to the last `stress` / `lua:` line here. Look for, per case:
   - `over` 0, `missed` 0, `end max` well under ~615 us (the go/no-go numbers); no `seq breaks` / `BUFFER MISMATCHES`;
     `frames N/N` as expected.
   - `start late max`: <= ~10 us except the Lua cases (up to ~140 us seen before; accepted).
   - `chunk max N/85`: N under 85 (else `COMP_CHUNK_WORST_US` is too small); `gap end max N/550`: under 550;
     `late` 0. Much headroom on both = room to raise throughput later (smaller margin / worst case).
   - `build N ms` on the page-build cases: expected ~90 ms for a 1U page (harness/spike estimate), spans a bit less.
   - `erases` > 0 on the layout-change cases, and no stale pixels on screen when the layouts switch.
   - No `stress: ... refused` lines.
   - The `heap ...: internal N free` lines (boot): expect roughly 29 KB more than before the redesign.
4. After the run, check the status bar is in place (no lasting shift).

**B. Normal release build.**
5. `idf.py --preset release -DCB_RUN_STRESS=OFF -p COM4 flash monitor` (app flash is fine after step 1's full flash).
   Note `heap after init: internal N free` and, once Wi-Fi is up, `heap with Wi-Fi connected` (CLAUDE.md's Internal RAM
   item: was ~130 KB / ~106 KB; expect ~+29 KB).
6. **Old layout (only if the board still has a layout stored from before the redesign):** at this boot the log shows
   `layout rejected (...): empty Home` (blob version 1) and the screen shows the empty Home. Skip if nothing was stored.

**C. Standalone.**
7. `node cli/cli.js upload urem2/urem2.mjs` (or `urem2/urem2-test.mjs` with the test server). Note the upload time
   (~440 KB now, was ~148 KB). The device restarts: note the boot-load time from the monitor timestamps (between
   `heap after the compositor` and `heap after the layout load`; `layout loaded: N bytes ...`).
8. On urem2: every key renders (faces, icons, labels, black margins, no holes drawn wrong), pressed looks appear at
   touch-down and revert on a swipe, keys fire, activity switches glide to page 1 with no stale keys, the pad page works
   (and its keys press correctly). Swipe feel: a page not already built may hold the swipe start longer (~6 frames);
   judge whether it's noticeable.
9. `node cli/cli.js upload examples/key-shapes.mjs`, with a text editor focused on the PC (each key types a different
   key: Wide w, 1, Volume - minus / + plus, A a, B b, Big g, L left, R right, Tall t, Esc esc, small q). Check:
   - every shape renders at its place (2U Wide, 2x2 Big, Tall, half-height A/B, half-width L/R, the 0.5U q at
     (3.25, 2.25)), black where no key is;
   - each key presses alone and anywhere on its face (spans have no dead seam inside); Volume - and + press separately;
   - the seam between two keys (also the middle of the Volume group) only swipes (~24 px strip); a touch ~4 px inside a
     face edge still presses (12 px edge zone); the q key is easy enough to hit;
   - press look on each shape, including the group members.
10. Swipe between pages with different layouts (urem2 Home <-> activity pages, or an activity switch from a page with
    holes to a full one) and watch for stale pixels where a key used to be.

**D. Hosted.**
11. `node examples/demo.js`: takes control, shows its keys (a 2U key and half-height keys among them), key events
    print with ids, release returns to Home.
12. `node examples/bench.js`: note the upload throughput (KB/s). This decides deflate (Open Item): at ~0.5-1 MB/s a full
    page of opaque images (~100 KB) takes ~0.1-0.2 s.

Paste results (logs and observations) under a "Step 16 results" heading here; then the job can be completed.

### Step 15 proposal (as approved; kept for reference)
Each item: the current text (abridged where long) and the exact replacement. Inline replacement text that contains code spans is quoted «like this».

**1. Repository Layout, `examples/` line.** Replace `examples/          Hardware test layouts (net-test.json,
upload-test.json, activity-test.mjs, script-test.mjs; shared helpers and the test server's address in
test-common.mjs), demo.js and bench.js;` with:
```
examples/          Hardware test layouts (net-test.json, upload-test.json, activity-test.mjs, script-test.mjs, key-shapes.mjs: spans, a group, half and quarter keys; shared helpers and the test server's address in test-common.mjs; bake-check.mjs bakes a layout and checks its keys like the device), demo.js and bench.js;
```
(rest of the line unchanged). In the `urem2/` line, after "Verified against the test server on the hardware
2026-11-08;" add: `the blob is ~440 KB since keys became opaque images (was ~148 KB);`.

**2. CLI and binpack tests.** Replace «and `node --test "examples/**/*.test.mjs"` (the examples test server).» with
«and `node --test "examples/**/*.test.mjs"` (the examples test server, plus a bake and key check of every example
layout).»

**3. Screen and Keys.** Replace the first three bullets ("5x3 keys per page, 128x128 cells...", "Pager bar
disabled...", and "Key images: each key names four host images...") and "Icon size" with:
```
- **Grid:** 5x3 cells of 144 px (1U) packed edge to edge, 720x432, centred in the 800x440 page area (origin (40, 4); (40, 44) on screen). Several pages swiped left/right. Black background throughout. Geometry in `key_geometry.h`.
- **Keys are rects with ids:** placed and sized in quarter units (0.25U = 36 px) on the grid: 1U, spans (2U, 2x2, tall), half and quarter keys, or a group (neighbouring keys sharing one picture, each pressing alone). Inside the grid, no overlaps, up to 32 a page (`COMP_MAX_KEYS`); whatever no key covers is black and only swipes. Each key has an id (u16, 1..65535, unique across the layout, all pages): KEY events, `SET_KEYS` edits and standalone `touch[id]` bindings use it.
- **Pager bar disabled** (`SCREEN_PAGER_H 0`): the 432 px grid needs the full 440 px page area, so bringing the pager back (20) means a smaller cell (`key_geometry.h`'s `static_assert` catches it). The status bar must stay at least 15 lines tall (`static_assert` in `screen.cpp`).
- **Swipe feel:** very smooth, like the iPhone home screen. No LVGL-style spring-back, no overshoot.
- **Key images:** each key names two host images by ID, normal and pressed (the device swaps to pressed itself; pressed 0 = no feedback). Each is **one fully opaque image exactly the key's pixel size**, composed by the host over black: face, icon, label and the margin (a 1U key is a 128 px face with 8 px of black round it). The device refuses images that aren't fully opaque, and images not the key's size. No stock face, tint or font in the firmware. Spec: [`docs/image-assets.md`](docs/image-assets.md).
- **Icon size:** 80 px (`KEY_ICON_SIZE`, GET_INFO `icon_w`), still being tuned. Keep sizes as firmware constants reported to the host (GET_INFO: cell, quarter, grid, max image, max keys); the bake's `consts.js` mirrors them (checked by a test). Don't hard-code them elsewhere.
```

**4. Rendering.** After the rle565 paragraph add:
```
Key images are drawn by `opaque_row.cpp` (`opaque_draw_rows`: opaque rows straight into a page buffer, 32-bit stores, no alpha), not by rle565's `draw_image` (the status bar still uses rle565's renderer). A host test checks it against `draw_image`.
```

**5. Communication (mode 1).** Replace "- Images go rle565-encoded under host-chosen 32-bit IDs (0 = none; a CRC makes
a good one) and stay on the device," with `- Images go rle565-encoded (fully opaque, the size of the key they're
for) under host-chosen 32-bit IDs (0 = none; a CRC makes a good one) and stay on the device,` (rest unchanged).
Replace the "Host-side key images" bullet with:
```
- **Host-side key images:** [node-buttonImage](https://github.com/toptensoftware/node-buttonImage) renders faces and icons from SVG to RGBA (stock icons `svgFile: "#name"` make handy test content); `host/src/images.js` composes them over black (`keyImage(layers, w, h)`, `FACE_MARGIN` 8) into one opaque rle565 image per state, the key's size.
```
Add a bullet: «- **Protocol v2** (2026-11-12): keys are quarter-unit rects with u16 ids (`SET_KEYS` edits by id, `KEY`
events carry the id, `GET_INFO` appends the grid); a host checks `protocol_version` (the library refuses
another version).»

**6. Firmware modules.**
- `screen.cpp/.h` bullet: append «It also times each fill's start against the frame's evenly spaced fills
  (`screen_stats_t` `max_late_us`, `max_end_us`, `missed`, `seq_breaks`, `buffer_mismatches`): a fill that
  starts late can miss the DMA even if it's short.»
- Replace the `page_compositor.cpp/.h, compositor.cpp/.h` bullet with:
```
- **`page_compositor.cpp/.h`, `compositor.cpp/.h`:** draw keys into three PSRAM page buffers and decide which the fill shows. A page is up to 32 keys, each a pixel rect and one opaque image (NULL = black). Keys are drawn **straight into the PSRAM page buffer**, only in fill gaps, in ~1 KB chunks (`COMP_CHUNK_BYTES`) each written back at once, up to a deadline measured from the fill's start (stop `COMP_GAP_MARGIN_US` 65 µs before the next fill; start a chunk only if `COMP_CHUNK_WORST_US` 85 µs fits). No render cell. Records are kept by rect; when a buffer gets another layout, the areas it leaves are filled black (erase list), never on a normal build. A reset or a sweep abandons the key being drawn. Core is platform independent and tested (harness oracle: images blitted at their rects over black); `compositor.cpp` runs it in a task. Spec: [`docs/page-compositor.md`](docs/page-compositor.md).
```
- Add after it:
```
- **`key_geometry.h/.cpp`:** the grid (`KEY_CELL` 144, 5x3, origin (40, 4) in the page area), `KeyRect` (quarter units, as on the wire and in the blob) to `KeyPixelRect`, rect and id checks (`KeyRectsCheck`, `KeyIdsCheck`), `COMP_MAX_KEYS` 32, plain grid slots (`KeySlotRect`, `KeySlotId` = page * 15 + slot + 1). Pure and tested.
- **`opaque_row.cpp/.h`, `fill_px.h`:** the opaque rle565 row decoder the compositor draws with (`opaque_image_fits`, `opaque_draw_rows`; tested against `draw_image`), and the 32-bit pixel fill shared by it and the bounce fill.
```
- `fill_gap` bullet: append «`fill_gap_done(fill_start_cycles)` gives each client the time its fill started (the
  compositor's deadline); `fill_gap_age_us()` is the time since the last fill ended.»
- Replace the `touch_router.cpp/.h` bullet with:
```
- **`touch_router.cpp/.h`:** swipe or key press. At touch-down it reads the page's keys (`page_keys`: ids and pixel rects, under the model lock) and hit-tests by rect with the 12 px edge zone on every side (`touch_key_at`), so the seam between two keys, including inside a group, is a 24 px swipe-only strip; then the swipe window and PRESS/RELEASE/CANCEL by key id, leaving the key judged against its touch-down rect. Pure and tested; tuning constants in the header. Spec: [`docs/interactions.md`](docs/interactions.md).
```
- `layout_blob` bullet: after "then reads it through the binpack-generated structs (...)." insert «Blob version 2
  (`LAYOUT_BLOB_VERSION`; a version 1 blob is refused, leaving the empty Home until a re-bake and upload): a key is
  `{ id, rect[4] (quarter units), normal, pressed, longPressMs }`, and the validator checks rects (inside the grid,
  no overlaps, at most 32 a page), ids (non-zero, unique across the whole layout) and images (opaque, exactly the
  rect's size).» In the same bullet, «`LayoutApplyHome`» "sets page 0's keys" becomes "sets page 0's keys with the
  blob's ids and rects".
- `standalone.cpp/.h` bullet: replace «`Key(page, key, action, time_ms)`» with «`Key(id, action, time_ms)`» and add
  «Touch controls are `touch[id]` (`TouchControlName(id)`); `KeyLongPressMs(id)` looks in Home's keys, then the
  selected activity's.»
- Replace the `image_store.cpp/.h` bullet's first sentence with: «the host's rle565 images under 32-bit IDs in PSRAM,
  **opaque only** (`require_opaque`), any size from 1x1 up to the key grid (720x432; `ImageEntry.width/height`):
  streamed uploads, replace in place (also with another size: its keys draw black until set again), deferred free,
  reset;» (rest unchanged).
- Replace the `key_layout.cpp/.h` bullet with:
```
- **`key_layout.cpp/.h`:** pages of keys, each `{ id, rect (quarter units), flags, normal, pressed }` (`KeyDef`), up to 32 a page, images ref counted. `SetKeys(first, pages, defs)` replaces a range of pages all or nothing (rects, ids unique across pages, images live and exactly the rect's size); lookups by id (`Find`, `SetPressed`, `IsActive`). The compositor's client (`GetPage`: pixel rects and the shown image, or black if its size doesn't fit) and the touch router's (`PageKeys`). Tested.
```
- `protocol.cpp/.h` bullet: after "the mode 1 protocol: stream parser, every request" add «(`PROTO_VERSION` 2:
  `SET_KEYS` edits keys by id with quarter-unit rects, collected per touched page range and applied through
  `SetKeys`; `KEY` events carry the key id; `GET_INFO` appends the grid)».
- `model.cpp/.h` bullet: append «; `model_page_keys`, `model_key_active(page, id)` and `model_set_key_pressed(id)`
  for the touch router».
- `host_link.cpp/.h` bullet: «`on_key` is the single switch point for key events» becomes «`on_key` is the single
  switch point for key events (by key id)».
- `main.cpp` `RUN_STRESS` sub-bullet, replace from "(off by default; build with ..." to "... (69% of the budget, 0
  over)." with:
```
  - `RUN_STRESS` (off by default; build with `idf.py --preset debug -DCB_RUN_STRESS=ON build`, which embeds `assets/test_keys.bin`: its opaque 1U 144x144 and 2U 288x144 keys are the device's test images, the 128 faces and icons are for the host tests; key layout in `test_keys.h`; `-DCB_RUN_STRESS=OFF` turns it off again; the device is left empty afterwards): at boot, drives the compositor (and status bar) flat out with opaque keys on plain and mixed pages (2U spans, offset keys, black holes): at rest and mid-swipe, incremental and flush, page builds (timed until the swap), layout changes (erases), the status bar, image replaces at ~USB speed, and Lua churning its PSRAM heap on core 0 (a `stress_lua` task, alone and with the compositor; a `lua:` line under each). It takes ~50 s and runs before touch, USB and Wi-Fi start (the board looks hung meanwhile; each case is logged as it starts, results at the end). Run it after changing the compositor, the bounce fill or anything that touches PSRAM. Per case: `fill max N us, N over | keys N (max N us), N erases, busy N% | N B/gap, chunk max N/85 us, gap end max N/550 us, N late | frames N/N [| build N ms (max N), N builds] | start late max N us, end max N us, N missed` (plus `seq breaks` / `BUFFER MISMATCHES` if any): `end max` and `missed` (a fill's start lateness plus its time, against the ~615 µs budget) are the go/no-go numbers; `late` gaps mean `COMP_CHUNK_WORST_US` is too small. A refused stress key layout is logged (`stress: ... refused`). The churn script is `stress_script.h` (a host test keeps it inside the heap cap). **Not yet run on the hardware since the opaque-key redesign** (2026-11-12). Before it, the worst case was `lua + keys, mid, flush` at 423 µs (69% of the budget, 0 over); the redesign's spike measured fills unchanged (144 µs at rest, 186 µs mid-swipe), starts ≤ 3 µs late, and a ~1 KB chunk ≤ 82 µs.
```

**7. Bounce fill rules.**
- Replace "Currently ~120 µs at rest, ~200 µs mid-swipe, up to ~330 µs mid-swipe with the compositor flat out
  (release `RUN_STRESS`)." with «Measured ~144 µs at rest and ~186 µs mid-swipe with keys being drawn (spike,
  2026-11-12); up to ~330-423 µs mid-swipe with the compositor flat out under the old render-and-copy design (release
  `RUN_STRESS`, to be re-measured).»
- Replace "The compositor writes page buffers, and the status bar its buffers, only in the gap after a fill, a measured
  slice at a time; they share the gaps through `fill_gap`." with «The compositor draws keys straight into page buffers
  only in the gap after a fill, in ~1 KB chunks each written back at once, up to a deadline ~65 µs before the next
  fill is due (measured from the fill's start, so a long fill leaves less); the status bar writes its buffers a band
  per gap; they share the gaps through `fill_gap`.»
- Add a bullet: «- **A fill that starts late misses too.** The ISR can be held off (e.g. by PSRAM writes on core 0,
  or a long cache write-back), and a short fill that starts late can still be too late for the DMA: `screen.cpp`
  measures the start (`max_late_us`, `max_end_us`, `missed`). Keep cache write-backs short (~1 KB): a long one
  holds interrupts off on its core.»

**8. Open Items.**
- Remove **Second render cell** (there is no render cell).
- Internal RAM item: append «The compositor's 32 KB render cell is gone (2026-11-12, key render redesign); `KeyLayout`
  grew ~3 KB (its SetKeys scratch) and the touch router has a 320 B scratch, so expect ~29 KB more free than the
  figures above: re-read from a normal release build.»
- Status bar in PSRAM item: after "(... Lua + keys mid-swipe, 423 µs, now has the least margin)" add `(figures from
  before the opaque-key redesign)`.
- Key presses item: replace "Tune `TOUCH_SWIPE_WINDOW_MS` and `TOUCH_EDGE_AMBIGUOUS_PX`," with «Tune
  `TOUCH_SWIPE_WINDOW_MS` and `TOUCH_EDGE_AMBIGUOUS_PX` (12 px from every key edge: a 24 px swipe-only seam between
  keys, also inside a group),».
- Activity switching item: replace "Activity pages use the global long-press time (per-key `longPressMs` there is not
  needed)." with «`KeyLongPressMs(id)` covers activity keys too.»
- Layout boot load item: "whether a real large layout fits in PSRAM" becomes `whether a real large layout fits in
  PSRAM (urem2 is ~440 KB now)`.
- Not yet run on the hardware: `examples/bench.js` becomes `examples/demo.js and examples/bench.js since protocol
  v2 (adapted 2026-11-12: opaque images, rect keys by id, a 2U key and half-height keys in demo.js; bench measures
  upload KB/s, which decides deflate)`.
- Add:
```
- **Key render redesign** (job `wip/wip-key-render-redesign.md`; built and host tested 2026-11-12, **not yet run on the hardware**: its step 16 is the checklist). Keys are opaque rect images drawn straight into PSRAM in fill gaps. **Page builds are ~2.5x slower** than the old render cell (~90 ms for a full page of 1U keys vs ~36 ms; a 1U key ~5 ms), so the swipe-start hold grows to ~6 frames for a page not already in a buffer: judge the feel. Tune `COMP_GAP_MARGIN_US` / `COMP_CHUNK_WORST_US` from the stress run (`chunk max`, `gap end max`, `late`). A stored layout from before the redesign (blob version 1) is refused: empty Home until re-baked and uploaded. Old hosts (protocol v1) are refused.
- **Lua delays fill starts:** the Lua heap's PSRAM writes on core 0 delayed fill starts by up to 139 µs in the stress run (seen once starts were measured; worst end 421 µs, under the 615 µs budget). Accepted for now; watch `start late max` on the Lua lines.
- **Lasting shift (theory):** in bounce mode the IDF v6.0 RGB driver fills buffer `bb_eof_count % 2`, and with `LCD_RGB_RESTART_IN_VSYNC` the VSYNC restart never resets that count. If an EOF interrupt is lost or merged (core 0 held off for more than a fill period), the parity flips for good and the picture stays shifted by one 15-line chunk (seen once, after an early spike that overran its gaps). `screen.cpp` detects it (`buffer_mismatches`, `seq_breaks` on the stress lines). If it shows up in normal running: resync the parity (reset the count at VSYNC, a driver patch) or keep core 0 from being held off.
- **Deflate for uploads, later:** opaque images are ~3.5x the old bytes (urem2 ~440 KB); raw deflate per image would bring them to ~1.4x (miniz `tinfl` is in the ESP32-S3 ROM, inflating straight into the image's PSRAM slot; an optional flag on `PUT_IMAGE` and the layout upload). Decide after `examples/bench.js` measures hosted upload speed.
- **urem2 pages with holes:** urem2 keeps plain 1U slot keys; several pages have holes that could become spans or groups (column 0 is empty everywhere; the pad pages' Vol+/Vol- are a natural vertical Volume group; atv's transport page has a 2x2 hole). List in `wip/wip-key-render-redesign.md` (step 13 notes; `scratch/urem2-holes.mjs` prints the map). Waiting for the user's call.
```

**9. Docs list (Repository Layout `docs/` line):** no change needed (`layout-format.md` is already listed);
optionally note "(screen-buffers.md is partly superseded)" stays as is.

## Questions for the user
(The questions in the "Earlier investigation" sections are answered by the Decisions above or moot, except where
repeated here.)
- Max keys per page: 15 is too few with 0.25U keys (a full page of 0.25U keys would be 240); assumed 32 below.
- Edge-ambiguous zone with no gaps: assumed 12 px from every key rect edge (see Flagged assumptions).
- Hosted pressed image: may it be 0 ("no pressed look", the normal image stays)? Assumed yes.

## Flagged assumptions
- The spike decides go/no-go (Plan steps 1-2); nothing else is built before the user's decision.
- **Device keys are quarter-unit rects** (x, y, w, h in 0.25U counts; pixels = count x 36, from the grid origin;
  inside the 720x432 grid; no overlaps; image size == rect size in pixels). The bake takes positions and sizes in
  slots (multiples of 0.25) and also offers grouped/split keys (adjacent keys with one picture across them).
- **Key identity = the key's id** (user decision 2026-11-12): **u16, unique across the whole layout** (all pages, as
  today's `touch[c,r]` names span the page strip), 1..65535, 0 = none. KEY events carry the id (plus the page, for
  information); SET_KEYS and single-key updates address keys by id; bindings by id (internal control names
  `touch[id]`, bake generated). The device refuses a SET_KEYS whose ids clash with another page's keys. Max 32 keys
  per page (`COMP_MAX_KEYS`). For plain grids the bake gives page p's slot i the id p*15 + i + 1, so
  urem2's `grid()` keeps working; layouts may name ids explicitly.
- One pressed image per key: a span presses as one key (it is one key); a grouped `+` presses only its own rect.
  The bake renders a group's pressed images per member (the member's part of the group picture, pressed).
- Edge-ambiguous zone: 12 px in from every key rect edge (`TOUCH_EDGE_AMBIGUOUS_PX` unchanged). With the bake's
  9 px of black padding that reaches only ~3 px into a face, and a group seam gets a 24 px swipe-only strip; tune
  on the hardware (checklist). Per-key insets only if the hardware says so.
- Default look stays nearly identical (8 px of padding per side instead of today's 9 px gap halves): the bake renders faces/fg at 128 as now, composes them over black and pads
  to 144 (`centreImage`), so `iconScale` etc. don't drift. Two constants: cell 144, face 128.
- GET_INFO: `key_w`/`key_h` become the cell pitch (144); new appended fields: grid origin and size, max keys per
  page, max image width/height. PROTO_VERSION 2. SET_KEYS's KeyDef becomes { id, x, y, w, h (quarter units), flags,
  normal, pressed }.
- Images: the store takes any size from 1x1 up to the grid (720x432), opaque only; the size is checked against the
  key's rect when keys are set (SET_KEYS, blob apply), not at upload.
- ~~Byte budgets per gap~~ (superseded by step 2): gap work is time-limited ~1 KB chunks against a deadline from
  the fill start; the black fill of uncovered areas uses the same chunks. Assumed margin ~65 us and chunk worst
  case ~85 us as constants (tune with the stress run). Page builds are ~2.5x slower than today (~90 ms full page
  vs ~36 ms) at a 350 us-equivalent deadline: assumed acceptable (single key ~5 ms).
- The Lua heap's PSRAM writes on core 0 delay fill starts up to ~139 us (seen once the start was measured); worst
  end 421 us is still under 615 us. Assumed acceptable for now; not changed in this job.
- `RUN_STRESS` and the host tests' `test_keys.bin` are regenerated as opaque 144 (and 2U) images.
- Old 128 layouts: refused at boot by the version bump (empty Home), not migrated.
- Step 5: the interim adapter draws each slot's 128 image in a 128x128 rect centred in its 144 cell (the plan said
  "slot rect"; a 144 rect can't take a 128 image, since the compositor needs images the size of their rect).
- Step 5: `comp_reset()` now abandons a key mid-draw instead of waiting for it (a full-grid key could take ~5
  frames); a sweep abandons the unit only if it draws a retired image.
- Step 5: the erase fill covers the old rects minus the new layout's rects (not the whole complement of the new
  layout), and buffers start black with no records, so boot and same-layout builds fill nothing.
- Step 5: a chunk's worst case (85 us), the margin (65 us) and the guard's 3 KB-per-fill estimate are constants
  from the spike, to be tuned in step 12's stress run.
- Step 6: `KeyLayout::SetKeys` replaces a **range of pages** wholesale (`first, pages`), which suits the blob apply
  (Home, activity pages) and a per-page SET_KEYS; step 8 decides the wire semantics (per page, or edits by id on
  top of `GetKeys` as the interim protocol does).
- Step 6: a key may name an image that is no longer live if that same key (same id, in the range) already holds
  it (so re-sending a page after FREE_IMAGES doesn't fail); a new key may not.
- Step 6: an image replaced by one of another size keeps its keys but draws background until the keys are set
  again (no refusal at upload: the size is only checked against rects when keys are set).
- Step 6: the interim blob path keeps the old blob's 128 images on 1U keys with `KEYDEF_ANY_SIZE` (drawn as
  background) instead of refusing them, so the layout apply / activity / protocol-release tests keep their
  coverage until step 11 re-bakes the sample blob. On the device the opaque-only store refuses those images
  anyway (empty Home until step 11).
- Step 6: new SetKeys refusals (geometry, ids, size) map to `ST_BAD_ARG` in the interim protocol; step 8 may add
  status codes.
- Step 7: the router takes the key's rect at touch-down and keeps it for the exit test; if the host moves a held
  key (same id, new rect) the old rect still applies until the touch ends (`key_active` checks the id is still
  on the page every frame, so a key removed or moved to another page cancels).
- Step 7: where rects overlap (they can't: `KeyRectsCheck`) the first key in the page's list wins.
- Step 7: an interim key id that isn't a plain grid slot of its page is dropped (logged) in `host_link.cpp`'s
  `on_key` until steps 8/11; none occur yet.
- Step 8: SET_KEYS **edits keys by id** (add/replace/REMOVE; unnamed keys stay) rather than replacing whole pages,
  keeping v1's incremental style; a key can't change page in place (an existing id named on another page is
  BAD_ARG: remove it, then define it in a later message). The same id twice in one message (incl. REMOVE + define)
  is BAD_ARG.
- Step 8: an image of the wrong size in SET_KEYS answers **BAD_IMAGE** (not BAD_ARG); all geometry and id
  refusals answer BAD_ARG (no new status codes).
- Step 8: GET_INFO's grid origin is in **screen** pixels (40, 44), not page-area pixels; `max_keys` is a u8 at
  the very end.
- Step 8: SET_KEYS re-sends every page between the lowest and highest touched page (SetKeys takes a contiguous
  range), so an untouched page in between is reported changed (its keys are unchanged, so the compositor's
  records stay known; cheap).
- Step 8: the KEY event's page is the page touched on (informational); the id identifies the key.
- Step 9: host `setKeys` takes positions and sizes in grid units (0.25 steps) and converts them to quarter
  counts, rather than taking quarter counts directly; `keySlot(i)` is the plain-grid convenience and
  `removeKeys(ids)` the removal one. The library only checks what it can't encode; grid, overlap, max keys and
  image size are left to the device (no host-side pre-check against `info`).
- Step 9: `images.js`'s old API (`keyImageFromSvg`, `buttonKeyImage`, `keyImage(rgba, w, h, size)`) was removed
  outright rather than kept alongside (its transparent images are refused by v2 devices); nothing outside
  host/ and the demos used it.
- Step 9: `composeOverBlack` forces alpha to 255 after compositing over the opaque black canvas (belt and
  braces against rounding), and layers must fit inside the key (no clipping).
- Step 9: FakeDevice's `handler` is now consulted first only for screen requests (and unknown types), with
  `undefined` meaning "let the model answer"; the image-memory figures (`imageUsed`) count live images only
  (the firmware also counts retired ones until released).
- Step 10: "slot i on page p gets id p*15+i+1" counts **p across the whole layout** (Home 0, then every activity's
  pages in file order), not per activity strip, so ids are unique layout-wide as decided (activity A's page 1 and
  B's page 1 would otherwise clash). Ids of keys that aren't on a whole-unit corner are auto-assigned (not stable if
  pages are added; name an `id` for a stable one).
- Step 10: the blob key's rect is a `byte[4]` `rect` (x, y, w, h) rather than four `byte` fields, because binpack
  pads each scalar field to 4 bytes (40-byte keys); `uint8_t rect[4]` also matches `KeyRect`. Step 11 reads it so.
- Step 10: empty slots (`null`) produce no key; a non-null entry with no look still is a key (a touchable area,
  `normal`/`pressed` absent). Step 11 decides how the device takes a key without images (background).
- Step 10: group authoring: the group's own face/icon/label span its rect; members set only icon/label/gestures,
  placed relative to the group (x/y default 0 each, not "both or neither"); group pressed = its pressed face
  (default overlay) under the member's pressed fg, per member. A group can't have gestures or an id; members not
  covering the group is a warning, not an error.
- Step 10: on spans and fractional keys the fg uses `virtualSize` scaled 72 per 128 px in both directions (text,
  padding and offsets keep their 1U size); the icon follows the face height (bigger on tall keys, smaller on
  short ones). The face box keeps the 8 px margin on every key (a 0.25U key's face is 20 px).
- Step 10: `types-header.test.js`'s drift test is marked `todo` (not deleted) so host stays green while the
  committed header is stale until step 11.
- Step 11: `KeyLongPressMs(id)` also looks in the selected activity's pages (before, only Home's keys had their
  own time); CLAUDE.md said that wasn't needed, but by id it costs nothing and the bake emits `longPressMs` for
  activity keys too.
- Step 11: a layout refuses (`BadKey`) rather than skips a bad key; ids must be unique across the whole blob
  (Home and every activity), as the bake guarantees, so an activity swap's `SetKeys` can't clash with Home's ids.
- Step 11: an image shared by keys of different sizes is refused (each use is checked against its key's rect); the
  bake never shares across sizes since the image is rendered at the rect's size.
- Step 11: the sample's Listen page gained a key just so an activity shares an image with Home (keeps
  LayoutApplyActivity's sharing test meaningful).
- Step 12: test_keys.bin **keeps** its 128 faces/icons (the plan said drop them once unused): the host tests still
  use them as small and transparent images; replacing them would mean rewriting TestImageStore and the refusal
  tests. Only the device stopped using them. `TEST_KEYS_FACE_SIZE` replaces `COMP_KEY_SIZE` there.
- Step 12: page builds are timed to the FreeRTOS tick (10 ms), polling the compositor's swap count; the stress
  constants are only printed (not overridable from CMake): tune by editing `page_compositor.h` and re-running.
- Step 13: the span/group demo is a **new** layout (`examples/key-shapes.mjs`, Home only, HID keys) rather than an
  edit of activity-test.mjs, so the hardware-checked activity layout stays as it was. urem2 is not redesigned (holes
  listed in Notes for the user).
- Step 13: the shared bake checker lives in `examples/bake-check.mjs` (a test helper beside the layouts) and urem2's
  test imports it, rather than a new export of the host library.
- Step 4: test_keys.bin keeps its 128 faces/icons alongside the new opaque keys (rather than being replaced
  outright) until the compositor and stress cases no longer need them; and the interim model store accepts images
  smaller than 128 until step 6 (nothing is flashed before step 13).

## Plan
Suites: **fw-build** = `idf.py --preset debug build` (plus `-DCB_RUN_STRESS=ON` where stated); **fw-test** =
`mk run` in `firmware/test` (debug and release test counts must match); **host** = `npm test` in `host/`; **cli** =
`npm test` in `cli/`; **urem2** / **examples** = `node --test "urem2/**/*.test.mjs"` / `"examples/**/*.test.mjs"`.
Every step leaves all suites green. Steps 5-11 change the device in stages: the firmware builds after each, but
don't flash it until step 13 (hosted and standalone content are only consistent again after step 11).

- [x] 1. **Spike (device only, not unit testable; warn).** `tools/test-icons` emits `assets/spike_keys.bin`
  (opaque 144x144 keys and 288x144 2U keys, realistic content; embedded only under `RUN_STRESS`). New
  `firmware/main/stress_opaque.cpp` (RUN_STRESS only): with the compositor idle, a writer registered on
  `fill_gap` draws a page of opaque keys straight into a PSRAM page buffer with `draw_image` source rects, rows
  per gap = byte budget / (w x 2), plus black fill of the uncovered margins. Cases: at rest and mid-swipe; 15 x 1U
  and a 2U mix; source in PSRAM vs copied to an 8 KB internal buffer (outside the gap) vs per-slice row copy (2U);
  budgets 3/4/8/12 KB. Log per case: late fills, worst fill, page build time, plus the existing compositor cases
  for comparison in the same run. fw-build (stress and normal), fw-test. Record the build/run command in Notes.
- [x] 2. **STOP: user runs the spike on the hardware (release `RUN_STRESS`) and decides go/no-go.** Record results,
  chosen budgets and the source-copy strategy in Notes. No further steps until then.
  - Run 1 done (results and analysis in Notes: flicker and a lasting shift seen); v2 run done: **GO** (user,
    2026-11-12). Results in Notes (Spike v2 results). Decisions: draw straight from PSRAM with the spike's 32-bit
    opaque decoder (no source copy); ~1 KB chunks, each written back immediately; the gap deadline measured from
    the fill start (fill start + chunk period - ~65 us margin; a chunk starts only if its worst case, ~85 us,
    fits), so it adapts to long fills; margins/uncovered areas filled only when the page layout changes; keep
    `screen.cpp`'s start-late / missed / frame-count measurements permanently (stress columns).
- [x] 3. **Geometry (pure, tested).** New `key_geometry.h`: cell 144, grid 5x3 at 720x432, origin centred in the
  page area (40, 4), `KeyRect` in quarter units (0.25U = 36 px) -> pixels, rect validation (inside grid, no overlap), id validation (u16, 1..65535, unique across all pages), `COMP_MAX_KEYS` 32. Tests
  first (`TestKeyGeometry.cpp`, added to `test.mk.js`). Nothing uses it yet. fw-test, fw-build.
- [x] 4. **Image store and test assets.** Tests first in `TestImageStore.cpp`: `Init(max_w, max_h, require_opaque)`,
  any size up to max, non-opaque refused (upload and `AddExternal`), entry exposes w/h. Regenerate
  `assets/test_keys.bin` as opaque 144 and 2U images (fold in the spike's generator). `model.cpp` keeps the old
  policy for now (128, not required) if the harness still needs it; say so in Notes. fw-test, fw-build.
- [x] 5. **Compositor core (pure, tested).** Tests first: harness oracle = opaque images blitted at their rects
  over black; `TestCompositor` geometry (cells abut, block centred, margins black; from `key_geometry.h`),
  uncovered-area fill only when the rects change between pages (not on every build), gap work in ~1 KB chunks
  that stop at the deadline (injected clock: a chunk starts only if its worst case fits before fill start + period
  - margin; a gap that starts late does less), retired images mid-draw, the fuzz. `page_compositor`: client page =
  list of { rect, image } (up to `COMP_MAX_KEYS`), records keyed by rect + image, rows drawn straight from the
  image in PSRAM into the page buffer with the spike's 32-bit opaque decoder (move `decode_row` out of
  `stress_opaque.cpp` into a shared pure file, tested against `draw_image`), each chunk written back straight
  away, black fill of uncovered areas, triple buffering / snapshot / offer-swap unchanged. `compositor.cpp`: drop
  the 32 KB render cell (no source buffer); the gap deadline from the fill start (`fill_gap` gives the fill's start
  time and period). Interim adapter in `KeyLayout::GetPage`: slot rect + the key's shown image (old bg/fg
  layering dropped; visuals wrong until step 6). fw-test, fw-build.
- [x] 6. **Key layout (pure, tested).** Tests first in `TestKeyLayout.cpp`: per-page key list { id, rect, flags, normal,
  pressed }, all-or-nothing `SetKeys` with rect validation (`KeyRectsCheck`), ids u16 unique across all pages
  (`KeyIdsCheck` against the other pages' ids) and image size == rect size, refcounts, pressed swap per
  key, page count. Remove the bg/fg slots and `COMP_KEY_BG_OPAQUE`. `model.cpp`: store `Init(720, 432, true)`.
  fw-test, fw-build.
- [x] 7. **Touch hit-testing (pure, tested).** Tests first in `TestTouchRouter.cpp` (re-point the old gap points at
  margins / edge zones; add spans, groups, empty areas): `touch_key_at` via a `TouchRouterHost` callback that walks
  the current page's rects with the 12 px edge zone; `swipe.cpp` wires it under the model lock. fw-test, fw-build.
- [x] 8. **Protocol (pure, tested) + docs/protocol.md.** Tests first in `TestProtocol.cpp`: PROTO_VERSION 2,
  GET_INFO fields, SET_KEYS KeyDef with id + quarter-unit rects (refusals: overlap, out of grid, size mismatch, duplicate or 0 id, too many keys),
  upload of a non-opaque image refused (BAD_IMAGE), KEY events carry the key id (u16, unique across the layout). Update protocol.md with it.
  fw-test, fw-build.
- [x] 9. **Host library.** Tests first: `test/FakeDevice.js` speaks PROTO 2 (new GET_INFO, SET_KEYS, opacity
  check); `ControlBlock.js` setKeys with rects; `images.js` composes over black, opaque output at rect size; README;
  `examples/demo.js` and `bench.js` adapted. host, cli, examples.
- [x] 10. **Bake.** Tests first in `host/test/bake/`: `consts.js` cell 144 / face 128 / BLOB_VERSION 2 (render-real
  test checks against `key_geometry.h`); schema keys get id, quarter-unit rects and two image refs; `resolve.js` positions/sizes in slots (0.25 steps)
  (`w`x`h` slots) and groups, rect validation, max keys; `look.js`/`render-node.js` render one opaque image per
  state at the key's rect (128 face padded to 144; spans and groups drawn across their rect; group pressed per
  member). Layout-format doc examples in the tests. host, cli, urem2, examples (firmware untouched: old generated
  header and sample blob still in place).
- [x] 11. **Firmware blob side.** `node tools/codegen/gen-types.js`; re-bake `firmware/test/data/sample_layout.bin`
  (add a span and a group to `sample_layout.json`). Tests first in `TestLayoutBlob`, `TestLayoutApply`,
  `TestKeyGesture`, `TestStandalone`, `TestActivityHost`, `TestTriggerDispatch`: LAYOUT_BLOB_VERSION 2 (old refused),
  key rects and image size/opacity checks, `LayoutApplyHome`/`LayoutApplyActivity` with rects, `touch[id]`
  control names, `KeyLongPressMs` by id. fw-test, fw-build, host (gen drift tests).
- [x] 12. **Stress run.** Port `main.cpp`'s `RUN_STRESS` cases to the new compositor (opaque keys, a span page, page
  changes with different layouts, Lua + keys); keep the start late / end max / missed / seq breaks / buffer
  mismatches / frames columns on every line; fold the spike's cases in or delete `stress_opaque.cpp` (and
  the third `fill_gap` client if nothing else needs it; `spike_keys.bin` is already gone, its images live on in
  test_keys.bin, step 4), and drop test_keys.bin's 128 faces/icons once nothing uses them. fw-build
  (stress and normal), fw-test (stress script test).
- [x] 13. **urem2 and examples.** Re-bake and check urem2 (both entries) and the examples' layouts; add a span and a
  grouped Volume +/- to an example layout (and to urem2 where a page has holes, if the user wants: flag);
  update their tests. urem2, examples, cli.
- [x] 14. **Docs.** page-compositor.md (opaque rects, draw from PSRAM in time-limited ~1 KB chunks, the deadline
  from the fill start, the spike's measurements, black fill on layout change, no render cell, the fill start-late
  / missed / frame stats), image-assets.md (opaque per state, any size, refusal), interactions.md (rect hit test, edge zone),
  layout-format.md (cell 144, 0.25U positions and sizes, key ids, groups), standalone.md (control names, key identity), host README, cli README;
  mark screen-buffers.md parts superseded if touched.
- [x] 15. **Propose CLAUDE.md update (Rule 4: propose only).** Write the proposed text in Notes (Screen and Keys,
  Rendering, compositor/key_layout/touch_router/image_store bullets, internal RAM, Open Items, deflate later) and
  ask the user; apply only on confirmation.
  - Approved and applied 2026-11-12 (Notes, "Step 15: CLAUDE.md changes"); the code comments pointing at this doc
    now point at the docs.
  - Checklist for step 16 written (Notes, "Step 16: hardware checklist").
- [x] 16. **Hardware checklist for the user (STOP).** Write it in Notes: flash full; release `RUN_STRESS` (late
  fills, worst fill, internal RAM free); hosted demo.js/bench.js (upload throughput, decides deflate); standalone
  upload of urem2 and the example layout (upload time, boot load time); spans/groups look and press; edge zone and
  group seam feel; page swipes between differently laid out pages (no stale pixels); old layout gives empty Home.

## Earlier investigation: tight-key-packing
(Kept for its file:line pointers and measurements. Superseded by the Decisions: the render cell, bg/fg layers,
opaque-skip flags, slice rows and the C2 "tiles with layers" model no longer apply.)
### Notes
Starting points for the investigation (not yet checked in depth):
- Current geometry (`firmware/main/page_compositor.h`): `COMP_KEY_SIZE` 128, 5x3 grid, page area 800x440;
  `COMP_KEY_GAP` = (440 - 3*128) / 3 = 18, pitch 146, grid origin from centring.
- **Cell size (user, 2026-11-12): not stretched to fill the page.** Fold today's gap into the key: cells the size of
  the current pitch (~146x146), packed edge to edge, the block of keys still centred with a margin around it.
  At 146: grid 730x438, margins ~35 px left/right, ~1 px top/bottom. The visible key and its spacing are drawn in
  the image (a 128 px face with 9 px of padding each side looks as today).
- The compositor renders each key into a `COMP_KEY_SIZE`^2 internal-RAM cell (32 KB at 128x128) before copying it to
  the PSRAM page: a bigger cell costs internal RAM (see the parked compositor strip idea in memory).
- Key sizes/icon area are reported to the host in GET_INFO (CLAUDE.md: keep sizes as firmware constants reported to
  the host); host renderers (`host/`, node-buttonImage, the bake) assume 128x128 with transparent padding.
- Touch hit-testing: `touch_router` / `swipe.cpp` map a touch to a key index; gaps between keys may count today.
- The pressed look swaps a key's image pair: with a group, does pressing `+` change only its half? (likely yes)
- **Render optimisation (user, 2026-11-12):** skip clearing the key area when the background or the face image is
  fully opaque. Partly there already: `page_compositor.cpp` (~line 378-395) skips the clear for an opaque bg
  (`COMP_KEY_BG_OPAQUE`, from `comp_image_is_opaque` / `check_image`; `ImageEntry.opaque` in the image store).
  Investigate what's left: an opaque fg (face) over a transparent/absent bg, the pressed pair, whether the bake
  can flag opacity too, and how tight packing (images carrying their padding, usually transparent corners) affects it.
- Bounce fill rules (CLAUDE.md) don't change: the compositor still writes page buffers in the fill gaps.

### Investigation findings (2026-11-12)

**Geometry is one header.** Everything positional goes through `COMP_KEY_SIZE` / `COMP_KEY_X/Y(i)`
(`page_compositor.h:16,28-33`); nothing in `swipe.cpp`, `screen.cpp` or `screen_view.cpp` uses key geometry. Tight
packing = `COMP_KEY_SIZE` 146 and `COMP_KEY_GAP` 0: pitch 146, grid 730x438, `COMP_GRID_X0` 35, `COMP_GRID_Y0` 1.
`COMP_KEY_SIZE` is used as both width and height everywhere; a non-square cell would need `COMP_KEY_W/H` (not needed
for 146x146). Key indices (row*5+col), SET_KEYS, KEY events, the layout blob's bindings and urem2's `grid()` don't change.

**Compositor (`page_compositor.cpp`).**
- Render/copy are already size-generic: `BeginKey` (419-447) clears, draws bg, fg at 0,0 into a `COMP_KEY_SIZE`^2 cell;
  `CopySlice` (451-470) copies full-width cell rows. `s_bg_row` (19-24) is one cell row. `CompRecord.y0/y1` are
  `uint8_t` (`page_compositor.h:113`): fine up to 255.
- **Slice sizes are in rows, but the measured limits are in bytes** (`page_compositor.h:44-45`; docs/page-compositor.md
  88-97: 8 KB per gap at rest, ~3 KB mid-swipe, 4 KB "just past a threshold"). At 146 px, 32 rows = 9.3 KB and 12 rows
  = 3.5 KB, both over what was measured. Keep the bytes: 28 rows (8.2 KB) and 10 rows (2.9 KB). Better: derive the rows
  from a byte budget so the cell width can't silently push them over.
- Cost: a full cell is 1.30x the pixels. In practice rle565 trims transparent top/bottom rows (`comp_add_image_rows`,
  332-352), so a face with 13 px padding copies ~120 rows x 292 B ≈ 35 KB vs ~30.7 KB today (+14%); a key with an
  opaque/full-height image copies 146 rows (+30%). More slices per key (full cell: 6 vs 4 at rest, 15 vs 11 mid-swipe),
  so page builds slow ~15-30%. Needs a release `RUN_STRESS` on the hardware (mid-swipe already 294-423 µs).
- Opaque assert `KeyRows` 386 and the whole-cell copy for opaque bg (391-393) are size-generic.

**Internal RAM.** The render cell (`compositor.cpp:64-65`, `MALLOC_CAP_INTERNAL`) grows from 32,768 to 42,632 B
(+9.6 KB). CLAUDE.md: ~106 KB free connected (release), ~95 KB debug, largest block 64 KB (memory note says ~60 KB with
the net worker). It should fit, but it eats about a tenth of the headroom. If it's too much, the parked strip option
(memory: compositor-strip-option; render one slice's rows into an ~8 KB strip) reclaims all of it plus ~24 KB, at the
cost of image-lifetime care per slice. The host test harness mallocs the cell (`firmware/test/harness.cpp:63`).

**Touch hit-testing (`touch_router.cpp:4-18`).** Hit test is the cell shrunk by `TOUCH_EDGE_AMBIGUOUS_PX` (12,
`touch_router.h:16`). Today touches in the 18 px gap hit no key (swipe-only) and the swipe-only zone reaches 12 px into
the 128 face. With 146 cells and 9 px baked padding, the same 12 px reaches only 3 px into the face. To keep today's
feel the margin would be ~21 px. **Grouped keys break this**: the seam between `+` and `-` would get a 2x21 = 42 px
swipe-only strip in the middle of the visible group. The device can't see where the faces' edges are, so either the
margin is applied per side from the host (per-key insets), or only on the outer edges of a group, or it's dropped to
the padding (9 px) and grouped keys accept a small seam. `docs/interactions.md:60-76` describes the zone in terms of
128 cells and the gap. Exit slop (`TOUCH_KEY_EXIT_SLOP_PX` 0) against the full cell is fine.
Tests that use the gap: `TestTouchRouter.cpp:124,283,421,432` (`gap_x = ... + COMP_KEY_GAP / 2`): with no gap these
points become the neighbouring key's edge zone; they need re-pointing at the edge zone or the page margin.

**Pressed look.** Per key (`key_layout.cpp:33-42`, `Rebuild`): pressing `+` swaps only its own cell's pair. Right for
split keys. A *spanning* key drawn as several cells would need all its cells to go pressed together (see below).

**GET_INFO / protocol.** Already reports `key_w`/`key_h` separately (`protocol.cpp:455-458`, protocol.md:155) and the
icon size (`KEY_ICON_SIZE` 80, `key_layout.h:30`); no grid origin or gap is reported, and none is needed (cells abut).
No wire format change. Images must be exactly `key_w`x`key_h` (`image_store.cpp:175-180`, protocol.md:13,92,263), so
every existing 128 image is refused: uploads get BAD_IMAGE, and a stored layout fails `layout_blob.cpp:195-196` at boot
(empty Home until re-baked and re-uploaded). A PROTO_VERSION / BLOB_VERSION bump would make the break explicit.
Hosted apps that use `info.keyWidth` (`examples/demo.js:65`, `examples/bench.js:12`, `host/src/images.js:58-64`) adapt
by themselves, but `demo.js:30-41` bakes a 4 px pad, so its faces grow and the visible gap shrinks to 8 px.

**Image store.** Size check only (`ImageStore::Init(..., COMP_KEY_SIZE, COMP_KEY_SIZE)`, `model.cpp:45`). Opacity is
already computed on the device for every image, uploaded (`Check`, `image_store.cpp:182-190`) and external
(`AddExternal`, 192-213), so the bake never needs to flag it. Image sizes barely change (transparent padding is nearly
free in rle565).

**Opaque-skip optimisation (what's left).**
- Done: an opaque bg skips the clear (`BeginKey` 433-434) and copies the whole cell; the pressed pair is covered
  because `Rebuild` sets `COMP_KEY_BG_OPAQUE` from whichever bg is shown (`key_layout.cpp:41`).
- Missing: an opaque **fg** (with or without a bg) could skip the clear *and* the bg draw (the bg is fully hidden). Needs
  a `COMP_KEY_FG_OPAQUE` flag set in `Rebuild` from `fg->opaque`, used in `KeyRows`/`BeginKey`, plus a harness check
  (the harness's reference render, `harness.cpp:494-512`, already composes bg then fg so it stays the oracle). Rare in
  practice: the bake's fg is icon+label on transparent (`look.js:131-158`).
- Value is small now: page-compositor.md:75 notes the clear became cheap once rendering moved to internal RAM (it was
  0.8-0.9 ms of 2.2 ms when drawn straight into PSRAM). A 146 clear is a ~42 KB internal memcpy; untimed (guess
  ~30-60 µs of an ~800 µs render estimate).
- Tight packing works *against* opacity: the bake's faces have transparent padding and rounded corners
  (`look.js:93-108`, `FACE_PADDING` 4), so they're never opaque. A face only becomes opaque if the bake paints its
  padding/corners `COMP_BACKGROUND` black instead of transparent (page-compositor.md:73 already describes this). But an
  opaque image always copies all 146 rows, while a transparent-padded one copies only its drawn rows, so opaque faces
  trade a cheaper render for a bigger copy, and the copy (PSRAM, gap-limited) is the scarcer resource. Probably not
  worth making the bake's faces opaque by default; measure with `USE_OPAQUE_FACES` in the stress run.

**Host side and bake.**
- `host/src/bake/consts.js:10` `KEY_SIZE = 128` (checked against the firmware header by `test/bake/render-real.test.js`);
  `render-node.js:8` renders at it; `look.js:97-107` draws the face SVG in KEY_SIZE pixels with `FACE_PADDING` 4.
- **node-buttonImage scales everything with the render size** (virtual 72x72: fonts, `textPadding`, `svgTranslate`;
  icon height = render height x `svgScale`, `drawButton.js:30-32,68`). Rendering the existing looks at 146 would enlarge
  icons and labels by 14% and urem2's tuned `ICON_SCALE` (`urem2/layout/look.mjs:19-20,53`) would drift. Simplest way to
  keep every existing look pixel-identical: render faces and fg at 128 as now and pad them to 146 with
  `centreImage` (`host/src/images.js:42-56`, already does exactly this). Only grouped/split keys then need the full
  146 canvas (future authoring work). This means `KEY_SIZE` (cell) and a "face size" (128) become two constants.
- `host/test/FakeDevice.js:14` hard-codes 128/80 in GET_INFO; `host/README.md:47` says 128;
  `docs/layout-format.md:141-163,417` describes 128 cells.
- `tools/test-icons/gen.js:77-139` (`KEY_SIZE` 128, `FACE_SIZE` 120): regenerate `firmware/assets/test_keys.bin`
  (used by `RUN_STRESS` and by the host tests: `harness.cpp:19`, `TestImageStore.cpp:568`).
- `firmware/test/data/sample_layout.bin` must be re-baked (`TestLayoutBlob.cpp:85` checks the image size).
- urem2 and examples' layout files have no pixel sizes of their own beyond look tuning (`iconScale`, `iconOffset`,
  `labelPadding`); with the "render at 128, pad to 146" approach they need only a re-bake and re-upload.

**Firmware tests to change first (TDD).** `TestCompositor.cpp:39-63` (geometry: gap symmetry, "half a gap above and
below"; becomes "cells abut, block centred, fits"), `harness.cpp:522-536` (gaps never drawn: becomes the margins only),
`TestTouchRouter.cpp` gap points (above), `TestProtocol.cpp:330` (fine, uses the constant). Everything else uses the
constants and follows.

**Docs to update.** page-compositor.md (geometry, slice sizes, cell size), interactions.md (hit-test/edge zone),
image-assets.md:52 (128x128), layout-format.md:163, CLAUDE.md (Screen and Keys: "128x128 cells"; Rendering), host
README.

**Relation to `wip-spanning-keys.md`.** Tight packing is a natural prerequisite. With cells abutting, a key spanning
N whole slots is exactly N cells wide (292 px for 2U), so the host can draw one picture and split it across N cells —
the same trick as grouped keys — and get spanning keys *visually* with no compositor change. What that still lacks,
and what the spanning job would add on the device: (a) one control for several cells (a touch anywhere in the span is
one key for events/bindings), (b) the pressed look on all the span's cells together, (c) no edge zone on internal
seams, and (d) fractional widths (1.5U), which need variable-size cells and can't be done by splitting. Suggest: do
tight packing first; the spanning job then decides between "linked cells" (a/b/c, whole slots only, small) and true
variable-size keys (d, a much bigger compositor/protocol change). Grouped `+`/`-` here is the opposite of a span: two
controls sharing one picture, which needs nothing beyond tight packing (except the seam's edge zone).

### Questions for the user
1. **Old 128 images:** a clean break (every image must be 146; stored layouts refused at boot until re-uploaded;
   bump PROTO/BLOB version), or should the device also accept 128x128 images and draw them centred in the 146 cell
   (keeps old layouts and hosted apps working, costs an offset in the compositor and the image store's size check)?
2. **Existing look:** keep today's appearance pixel for pixel (bake renders at 128 and pads to 146), or let the default
   look grow into the bigger cell (larger faces/icons, smaller gaps; urem2 icon scales re-tuned)?
3. **Edge-ambiguous zone:** with no gaps, how should touch-down near an edge behave? (a) raise the margin to ~21 px
   from the cell edge (today's feel; a dead 42 px seam inside a group), (b) keep 12 px / drop to the padding and accept
   a less forgiving edge, or (c) per-key edge insets from the host (layout/protocol change, lets a group have no seam).
4. **Render cell RAM:** accept +9.6 KB internal RAM for a 146x146 cell, or do the parked strip rendering at the same
   time (frees ~24 KB more, bigger change)?
5. **Opaque fg skip:** worth adding `COMP_KEY_FG_OPAQUE` now (small, but rarely hit), or measure the clear first and
   leave it if it's tens of µs?
6. Should GET_INFO also report a "face"/content size (the 128 area the default look draws in), so hosted apps can keep
   the old look without hard-coding 128?

### Flagged assumptions
- Cell size 146x146 (the current pitch, from the 440 px page area), square, block centred: 35 px side margins, 1 px
  top/bottom. A pager bar (`SCREEN_PAGER_H` 20) would make the pitch 140; assumed the cell size stays derived from the
  page area as now.
- Slice limits are byte limits (from the docs' measurements), so slice rows should shrink to 28 / 10 to keep 8 KB / 3 KB.
- The +9.6 KB render cell fits the current internal RAM headroom (not measured).
- Clear cost at 146 guessed at ~30-60 µs (not measured).
- The pressed look stays per cell (grouped halves press independently), as the description guessed.
- No change to key indices, the protocol wire format, or the blob schema is needed for tight packing itself.

## Earlier investigation: spanning-keys
### Notes
Starting points (from the Start phase):
- Today: 5x3 fixed grid, 128x128 cells, key index = row*5 + col (`COMP_KEY_X/Y(i)` in `page_compositor.h`); keys
  identified by index in the protocol (SET_KEYS, KEY events), the layout blob (`layoutdata`, bindings by control),
  `key_layout.cpp` (pages of keys, four image slots each) and urem2's `grid()` (slot maps).
- Questions to answer: units and their pixel size; can rows have different heights? key identity; touch mapping;
  GET_INFO; hosted mode and standalone layouts; migration of existing layouts.

### Investigation findings (2026-11-12)

**Where "15 fixed 128x128 slots" is baked in (device):**
- Geometry: `page_compositor.h:13-33` (`COMP_KEYS/COLS/ROWS/KEY_SIZE`, gap, pitch, `COMP_KEY_X/Y(i)`).
- Compositor: every per-key structure is `[COMP_KEYS]` indexed by slot: `CompRecord rec[COMP_KEYS]`
  (`page_compositor.h:199`), job snapshot (`:306`), `comp_get_page_t` (`:87`). Rendering: `BeginKey`
  (`page_compositor.cpp:419-447`) clears/draws one 128x128 cell; `CopySlice` (`:451-470`) copies cell rows to
  `COMP_KEY_X/Y(k)`. `comp_add_image_rows` asserts 128x128 images (`:338`); `comp_image_is_opaque` too (`:364`).
  Records keep only a row span `uint8_t y0, y1` (`:113`).
- **The compositor never erases anything outside the fixed slot cells:** buffers are filled with background once at
  `Init` (`:47-58`; docs/page-compositor.md:377 "gaps are filled once per buffer"). Any model where key rects move
  (per page, and buffers are shared by all pages, so on every swipe to a differently laid out page) needs erase logic
  the compositor doesn't have today, **unless the compositor keeps fixed tiles that cover the page edge to edge**.
- Render cell: `compositor.cpp:64-65`, `COMP_KEY_SIZE^2` internal RAM (32 KB).
- rle565 helps: images have per-row offsets (`rle565.core.h`, `rowOffsets[iy]`) and `draw_image` takes a source
  rect (`rle565.h:117`), so drawing part of a larger image into a tile (or a band) is cheap; horizontal clipping walks
  runs from the row start.
- Image store: one fixed image size for every image (`model.cpp:45` `Init(..., COMP_KEY_SIZE, COMP_KEY_SIZE)`,
  `image_store.h:92-94`); the layout blob checks 128x128 too (`layout_blob.cpp:195-196`).
- Key layout: `m_keys[KEY_LAYOUT_MAX_PAGES][COMP_KEYS]` (`key_layout.h:106`), `KeyDef { page, key, flags, ids[4] }`
  (`key_layout.h:32-38`); `SetPressed` changes one key and reports its page (`key_layout.cpp:124-135`).
- Touch: `touch_key_at` loops the 15 fixed cells (`touch_router.cpp:4-18`) with `TOUCH_EDGE_AMBIGUOUS_PX` 12
  (`touch_router.h:16`): today's active zone is the middle 104x104 of each 128 cell; the 18 px gaps only swipe.
  `in_key` is also the exit test (`touch_router.cpp:82, 89`). It's a static function with no page argument, so a
  per-page geometry needs a host callback (`TouchRouterHost`, `touch_router.h:28-39`; wired in `swipe.cpp:465-470`).
- Protocol: GET_INFO sends cols/rows and `key_w/key_h` = "size of a key, and of every image" (`protocol.cpp:451-458`,
  `docs/protocol.md:151-156`). SET_KEYS `KeyDef.key` is "0..cols*rows-1, row-major" (`docs/protocol.md:320`);
  `flags` is u16 with 2 bits used (room for more). KEY events carry (page, key) u8.
- Standalone: the schema has fixed `key[15]` pages (`host/src/bake/schema.js:58, 62`, static_assert
  `layout_types.h:28`, `layout_blob.h:60`, `layout_apply.cpp:26, 74`, `layout_blob.cpp:214, 357`). Bindings are by
  control name `touch[c,r]` with c = 5*page + col across the page strip (`key_gesture.cpp:11-14`,
  `resolve.js:190`, `docs/standalone.md:186`, `docs/layout-format.md:299-301`). Users never write `touch[...]`
  (the bake rejects it, `resolve.js:164`), so the naming is internal and can change cheaply.
  `Standalone::KeyLongPressMs` indexes `homePage.keys[key]` (`standalone.cpp:115-119`).
- Host/bake: `KEY_COLS`, `KEYS_PER_PAGE`, `KEY_SIZE` 128 (`host/src/bake/consts.js:7-10`); look rendering is square
  `KEY_SIZE` (`look.js:99`); `resolveKeys` maps list index -> slot (`resolve.js:175-226`). urem2 `grid()` is a slot
  map to 15 row-major keys (`urem2/layout/keys.mjs:37-51`).

**Costs that don't change much:** the bounce fill and its rules are untouched (it only reads page buffers). Page
build cost goes with pixels drawn and copied (docs/page-compositor.md:369-376: copy dominates, ~25 MB/s): wide keys
draw about the same area as the slots they replace. The render cell size is set by the tile, not by key width, in the
recommended model.

**Models considered (device side):**

A. *Fixed grid with slot spans* (key = top-left slot + w x h in slots). Key identity, KEY events, `touch[c,r]` names
   (anchor slot) and urem2 `grid()` all survive. With tight packing (cells edge to edge) a span can even be done
   with **no compositor change**: the host slices the wide picture into per-slot tile images, and the device only
   learns "slot j is part of key i" (a link: touches on j go to i, pressing i shows the pressed tiles of every
   member). That's the same mechanism as tight packing's grouped keys. No 1.5U.
   + small, cheap, compatible. - whole slots only; hosts must slice images; more image IDs.

B. *Keyboard rows of unit widths* (rows of keys, widths 1U/1.25U/1.5U/2U...). Really an authoring format: on the
   device it has to become per-key rects (C). Key identity becomes a list index, not a slot.

C. *Free rects per key* (x, y, w, h per key per page). Two ways to compose:
   C1. *Per-key cells:* render each key once into a cell as big as the largest key (or in row bands), copy to its
       rect; records keyed by rect; old rects not covered by the new geometry must be erased (new compositor work,
       and on every swipe between pages with different geometry). Swap rule/diff machinery needs reworking.
   C2. **Fixed tiles with layers (recommended):** keep the compositor's fixed tiles (the 15 tight-packed pitch
       cells, edge to edge), but a tile's content becomes a short list of layers (image, dx, dy) instead of one
       bg/fg pair. The client (KeyLayout's `GetPage`) intersects key rects with tiles and emits each key's bg/fg as
       clipped layers. Every tile is redrawn whole, so erasure stays free; records compare layer lists; the job,
       offer, swap rule and slice copying are unchanged in shape. Pressing a key changes the tiles it touches.
       Cell stays tile-sized (no extra RAM beyond tight packing). A key crossing a tile boundary is rendered in
       pieces (cheap: row offsets + clipping). Needs a cap on layers per tile (e.g. 6-8: up to 3-4 keys meeting
       in one tile), checked on SET_KEYS / in the bake. Images become key-sized (w x h): the image store and blob
       checks take a range of sizes instead of one fixed size. Variant: the host pre-slices each key into tile-size
       images (offset always 0), keeping the store's fixed size at the cost of more images and host work.
   Model A is the special case of C2 where rects align with tiles, so C2 doesn't block A as a first step.

**Key identity under C:** per-page list index (0..N-1). KEY events keep (page, key u8), the meaning changes from
slot to list index. Control names change to something like `touch[page,index]` (internal; docs and
`TouchControlName` change). Default rect for a key = its slot, so existing hosts, layouts and urem2 `grid()` keep
working unchanged when no rect is given (beyond tight packing's own image-size migration).

**Touch:** a per-page rect lookup with the 12 px edge margin per rect. With keys edge to edge (tight packing), two
abutting keys leave a ~24 px swipe-only band between them, close to today's 18 + 24. Lookup under the model lock
(the router already calls `key_active` there).

**Units:** with tight packing 1U = pitch = 146 px (730x438 grid). 0.5U = 73 px, but 0.25U = 36.5 px is not whole:
rects should be pixels on the wire and in the blob, with the bake rounding unit edges (edge = round(u * 146)) so
abutting keys share an edge exactly.

**GET_INFO:** report the tile/unit pitch, the grid origin and size, max keys per page and max layers per tile;
`key_w/h` no longer means "every image" (protocol version bump, or new fields appended).

**How this fits with `wip-tight-key-packing.md`:** tight packing is a prerequisite: spanning keys (A or C2) need
tiles that cover the grid contiguously, or a wide key has holes at the old gaps. Recommend one design, implemented in
sequence: (1) tight packing (tiles = pitch, images carry padding), shaping the compositor's tile content type so it
can grow into a layer list; (2) slot links/groups (model A: spans + tight packing's grouped keys share it);
(3) only if fractional widths are wanted, rects + layers (C2). The tight-packing "Volume +/-" group label could be an
inert key (`DISABLED`, no touch) whose rect spans both keys, drawn as a layer over them under C2, or just two halves
of one picture under A.

### Questions for the user
1. Are whole-slot spans (2U, 3U, 2x2) enough, or are fractional widths (1.5U, 1.25U, keys not on a slot) really
   wanted? This decides A (small) vs. C2 (large).
2. Rows of different heights / keys spanning rows (e.g. a tall 1x2 key): needed?
3. Max keys per page: stay at 15, or more (keyboard rows of 0.5U-1U keys could exceed 15)?
4. Merge this job with tight packing into one job (one design, sequenced steps), or keep two jobs with tight packing
   first?
5. Hosted mode: must the existing SET_KEYS format keep working (default rect = slot), or can the protocol version
   change outright (host library and examples updated together)?
6. Images: key-sized images (store takes many sizes) or host-sliced tile-size images (store keeps one size)?

### Flagged assumptions
- Tight packing (tiles = pitch, edge to edge) lands first or together; this job builds on it.
- Rects are whole pixels in the protocol and blob; units are an authoring concept rounded by the bake.
- Key identity becomes a per-page list index; existing slot indexes stay valid as "key i defaults to slot i".
- `touch[...]` control names are internal (bake-generated) and may be renamed without a user-visible break.
- The bounce fill and its rules don't change.
- Holes on sparse pages are the motivating case; urem2/examples have such pages (not counted).


### Step 16 results (user, 2026-11-12)
- **Release `RUN_STRESS`: passed.** No corruption seen. 0 fills over, 0 missed, start late <= 3 us except Lua
  (48/92/137 us, as before); worst fill end 394 us of 615. Page build ~82 ms (1U and spans), 1U key ~3 ms, 2U ~5 ms;
  98 erases in "layout changes, mid" fine. Internal heap 193,651 free (largest 151,552). **Chunk worst case too
  small:** under PSRAM contention chunks took up to 102-108 us (image replaces) and 131 us (Lua + keys): gap ends up
  to 573 us (deadline 550), 2/2/29 late gaps, no harm (next fill at 615). **Fixed:** `COMP_CHUNK_WORST_US` 85 -> 140
  (docs updated). Re-run the stress once to confirm `late` is 0 (optional).
- **Standalone:** urem2 (re-baked) works; key-shapes.mjs works; page render delay on swipe feels no different to
  before.
- **Pager bar dropped for good** (user): `SCREEN_PAGER_H` and the fill's blank pager branch removed; the pager is
  the status bar's pills. Debug build 0 errors, fw-test 752 (debug and release).
- **Not yet run:** hosted `examples/demo.js` and `examples/bench.js` (upload throughput decides deflate).
- **Hosted (user, 2026-11-12):** `demo.js` works. `bench.js`: ping 0.63 ms; ping 8 KB x 50 705 KB/s both ways;
  upload 40 x 21.8 KB 251 KB/s; a page of keys 30 images, 142.5 KB in 556 ms (256 KB/s, 4.7 KB/image); image memory
  1014 KB -> 0 after freeing. Uploads run at ~1/3 of the link: the bottleneck is on the device side (per-chunk round
  trips, the opacity check, or the PSRAM copy), so deflate alone may not help. Follow-up: investigate upload
  throughput before deciding on deflate (not part of this job).
