# Screen Buffers

How the firmware allocates, draws and swaps the pixel buffers the bounce fill reads from. Status: **partly superseded by [`page-compositor.md`](page-compositor.md)**, which is implemented. Page tags and dirty bits, the frame table, the swipe task's wanted set and hold, and the page background cost are replaced there (see its [Changes to screen-buffers.md](page-compositor.md#changes-to-screen-buffersmd)). Key render timings, the status bar and pager, the memory budget, reload deferral and modes 2 and 3 still apply. It replaced two earlier designs: neighbours pre-rendered in a 4-buffer pool, and in-place drawing timed against the fill. See [Alternatives considered](#alternatives-considered).

## Summary

- **One rule: never draw into a buffer that's on screen.** All drawing goes to an off-screen buffer, which is then swapped in at a frame start. The ISR only latches buffer pointers and the scroll position at frame start, and does row copies and solid fills.
- **Three page buffers** in PSRAM (800x400 RGB565, 625 KB each). No fixed roles. Each one is tagged with the page it holds and a set of keys that are out of date.
- **Key updates at rest:** draw the changed keys into the back buffer (a copy of the current page), swap, then draw the same keys into the old front so it's a copy again. That's usually **two renders per changed key**, ~0.7 ms each.
- **A swipe waits for the incoming page.** When the drag passes the slop, the render task draws the incoming page (≈ 15–17 ms, about one frame). The page stays still until then, then jumps to where the finger has got to and follows it 1:1. The swipe never shows a page that isn't fully drawn.
- **Key updates during a swipe:** re-render each affected page in view into the spare buffer and swap it in, one page per frame. The two pages may update one or two frames apart.
- **Whole-layout reloads** (RESET, SET_PAGE_COUNT, mode 3 activity switch) are deferred until the swipe settles. The latest one wins. A reload goes to the first page (or one the client names), which is rendered off screen and swapped in.
- **Keep it simple.** Incremental updates, such as drawing the incoming page in parts or updating keys in place, are an option if something proves too glitchy. They aren't part of the design unless they're needed.
- **One render task** (core 1) does all drawing and is the only writer of the buffers and the frame table.

## Numbers this design rests on

| Item | Value | Source |
|---|---|---|
| Frame | 20.5 ms (~49 fps at 20 MHz; was 16.4 ms, ~61 fps at 25 MHz), 32 chunks of 15 lines | CLAUDE.md, `waveshare_rgb_lcd_port.h` |
| Fill budget per chunk | ~615 µs at 20 MHz (was ~492; aim < 250) | `DISPLAY_BOUNCE_BUDGET_US` |
| Fill now | ~71 µs at rest, ~200 µs mid-swipe | CLAUDE.md |
| Page chunk (24,000 B) read from PSRAM | ~285 µs cold, ~68 µs preloaded | `screen.c` comment |
| Page buffer | 800 x 400 x 2 = 640,000 B | `screen.h` |
| **One key** (96x96 face + 80x80 icon) | **~0.68 ms** average, 0.88 ms worst (display on, cold) | `bench.c`, see [Key render timings](#key-render-timings-measured) |
| **15 keys** | **~10.2 ms** (display on, cold), 5.3 ms best case | `bench.c` |
| Page background (gaps between keys, ~360 KB) | **≈ 5–7 ms**, estimated from the cold blank rate (51–78 MB/s) | estimate, not measured |
| **Full page** (keys + background) | **≈ 15–17 ms, about one frame** | 10.2 ms measured + estimate |
| Swap latency | ≤ 16.4 ms: a publish shows at the next frame start | frame period |
| Glide (`SPRING_RESPONSE_S` 0.30) | ≈ 350–500 ms to settle within 0.5 px | `swipe.c` (ω = 2π/0.3) |
| Drag slop | 10 px; the drag then starts from offset 0 (no jump) | `swipe.c` |

**What these mean:**
- A full page is about **one frame**, and **under 5% of a glide**. Rendering a page on demand is fast enough, so there's no need to keep neighbours pre-rendered.
- **A key update** costs ~0.7 ms to draw plus ≤ 1 frame to swap in. The second render, which brings the old front up to date, happens after the swap and adds no latency.
- **Holding a swipe start** for one full render delays the first movement by about **1–2 frames** (16–33 ms). See [Swipe start](#swipe-start-wait-for-the-incoming-page).
- Labels, an rle565 key face, the `-Og` build and 120x112 keys will all add to these. See [Measurements to make](#measurements-to-make).

## Key render timings (measured)

Measured 2026-11-02 by `firmware/main/bench.c` (since removed; see git history) at boot, release build (`-O2`, asserts off), on core 1. The LCD ISR is on core 0.

**What one key render is in this test:**
1. **Blank** the key with a 96x96 uncompressed RGB565 background held in internal SRAM: one `memcpy` per row (192 B × 96 rows = 18 KB) into a PSRAM page buffer. This stands in for the stock key face.
2. **Icon:** draw an 80x80 rle565 icon with alpha, stored in PSRAM, centred on the key (`draw_image`, blended over the background).

No label is drawn. The icons are 14 of node-buttonImage's built-in icons, 1.1–3.8 KB encoded, 27–402 palette colours. They're cropped so they fill most of the 80 px square, which makes these worst-case-ish timings (generated by `tools/test-icons`).

**Modes:**
- **warm:** the same key is drawn once untimed first, so the icon and the destination rows are already in cache. This is the best case.
- **cold:** the icon data and the destination rows are written back and evicted from the cache before each draw. This is closer to drawing a newly uploaded icon into a buffer the ISR hasn't touched recently.
- **display off:** run before the panel starts.
- **display on:** run while the bounce fill streams pages from PSRAM (at rest, not swiping).

**Averages per key** (14 icons × 20 draws each):

| Run | Background | Icon | Total per key | Min / max | 15 keys |
|---|---|---|---|---|---|
| Display off, warm | 64.1 µs | 288.2 µs | 352.4 µs | 215.8 / 439.0 µs | 5.29 ms |
| Display off, cold | 235.1 µs | 304.0 µs | 539.1 µs | 376.1 / 681.1 µs | 8.09 ms |
| Display on, warm | 134.3 µs | 321.8 µs | 456.1 µs | 225.9 / 761.1 µs | 6.84 ms |
| **Display on, cold** | **358.3 µs** | **319.6 µs** | **677.9 µs** | 377.5 / 880.9 µs | **10.17 ms** |

"Display on, cold" is the realistic figure to plan with: **~0.7 ms per key, ~10 ms for 15 keys**, not counting labels or the page background.

**Icon time per icon** (µs; min/max above are per key, background included):

| Icon | Bytes | Colours | Off, warm | Off, cold | On, warm | On, cold |
|---|---|---|---|---|---|---|
| home | 2419 | 130 | 324.7 | 339.5 | 388.2 | 354.1 |
| settings | 2968 | 214 | 370.8 | 390.6 | 422.2 | 408.3 |
| play | 2594 | 167 | 324.4 | 339.8 | 366.8 | 357.8 |
| pause | 2734 | 158 | 346.9 | 363.4 | 380.4 | 380.7 |
| volume-up | 2183 | 136 | 269.0 | 282.6 | 278.1 | 293.6 |
| volume-mute | 2549 | 155 | 319.7 | 335.3 | 335.5 | 351.1 |
| power | 2486 | 152 | 292.4 | 308.9 | 311.8 | 327.1 |
| heart | 2456 | 200 | 279.4 | 295.8 | 310.5 | 313.7 |
| star | 2411 | 195 | 273.1 | 288.2 | 302.1 | 301.7 |
| apple-tv | 2270 | 186 | 270.3 | 285.8 | 295.7 | 301.4 |
| steam | 3803 | 402 | 264.3 | 287.4 | 323.5 | 310.5 |
| refresh | 1973 | 167 | 219.5 | 232.3 | 235.6 | 241.6 |
| next-track | 1122 | 27 | 151.7 | 159.5 | 157.8 | 165.9 |
| info | 2693 | 188 | 329.1 | 346.6 | 397.4 | 366.7 |

**Observations:**
- **The background blank is bus-bound.** 18 KB took 64 µs warm (≈ 290 MB/s) but 235 µs cold (≈ 78 MB/s), and 358 µs cold with the display running (≈ 51 MB/s). It roughly doubles under display contention.
- **Write-allocate:** the cold blank only writes, yet it's ~3.7× slower than warm. That suggests the cache fetches each line on a write miss. It's not proven, though: the cold run also includes write-back of the lines the blank evicts.
- **The icon draw is CPU-bound.** Cold is only ~5% slower than warm, and the display adds only ~10%. Its cost follows how many pixels are drawn and how many edge (alpha) pixels there are. File size and palette size matter less: `steam` (402 colours, which uses the >256-colour path) is mid-pack, and `next-track`, which draws few pixels, is the cheapest at ~155 µs.
- **With the display on,** some icons read slower warm than cold. That's noise from contention with the bounce fill, not a real effect. Treat per-icon differences under ~30 µs as noise.
- **Background vs icon:** cold with the display on, the blank costs about as much as the icon. Making the key face cheaper to write matters as much as making the icon cheaper.

## Buffers

| Buffer | Count × size | Where |
|---|---|---|
| Page buffers | 3 × 625 KB | PSRAM |
| Status bar, pager | 2 each × 62.5 KB (front + back) | PSRAM |
| Bounce buffers | 2 × 24 KB | internal SRAM (LCD driver, as now) |

**Each page buffer has:**
- `page`: the page it holds, or none.
- `dirty`: a 15-bit set of keys on that page that changed in the model since this buffer drew them. A buffer is a **clean copy** of its page when `dirty` is empty.
- A state, worked out from the frame table: **mapped** (in the published table), **retiring** (just unmapped; the ISR may still be reading it until the next frame start), or **free**. Only a free buffer may be drawn into.

**Roles** are just which buffer is doing what at the moment:

| Situation | Mapped | Free |
|---|---|---|
| At rest on page *p* | front (*p*) | back: a copy of *p*, kept clean. spare: usually the last page left, still tagged. |
| Swipe *p* → *q* | outgoing (*p*), incoming (*q*) | spare: usually the back copy of *p* |

**Why three:**
- During a swipe, two buffers are on screen. A third is needed to re-render either page without drawing on screen.
- At rest, the third lets the back stay a clean copy of the current page, so key updates stay cheap. The spare holds the incoming page during a swipe and keeps the page last left, so swiping straight back needs no render.
- Never more than three are needed. At most two are mapped, and every render goes into the one free buffer.

**Page buffers stay in PSRAM:** 625 KB doesn't fit in the ~512 KB of internal SRAM.

## Frame table and the ISR

```c
typedef struct {
    uint32_t seq;                 // publish sequence number
    struct { int16_t page; const uint16_t *px; } slot[2];   // page -1 = unused
} frame_table_t;
```

- **Publish:** there are three tables. The render task fills the one that's neither published nor latched, then release-stores the pointer.
- **Latch:** at frame start the ISR latches `s_published` and the scroll together, and writes `s_latched_seq`.
- **When a buffer is free:** a buffer dropped from the table becomes free once `s_latched_seq` ≥ the seq that dropped it. That's the next frame start, ≤ 16.4 ms later. To wait, the render task asks for a wake at frame start once that seq is latched; this sits next to the existing swipe notification.
- **Per row:** the ISR copies from the slot whose page matches each page in view. A page with no slot gets a solid fill: `COLOR_BACKGROUND`, as now, past the first or last page. This should only ever happen while rubber-banding (see below).
- **Preload:** as now.
- **Coherency:** no flushes are needed. Both sides go through the same data cache, and the LCD DMA reads only the bounce buffers. The release store orders pixel writes before the pointer.

## Render task

- **Placement:** pinned to core 1, below the swipe task (10), above USB and app tasks.
- **It is the only writer** of buffers and tables. The swipe task never blocks on it.
- **How it draws page *p* into a free buffer *b*:**
  - If `b.page == p`, it redraws only `b.dirty`.
  - Otherwise it renders the full page, background and all 15 keys (≈ 15–17 ms), and sets `b.page = p`.
  - Key state is copied under the layout mutex, and a key's dirty bit is cleared in the same critical section. An update that arrives later sets the bit again, so it's never lost.
- **Model changes** (`SET_KEYS` and friends): under the mutex, for each changed key, the bit is set in `dirty` of every buffer tagged with that page. Then the render task is notified.

**Work order, each pass:**
1. Pages the swipe is waiting for (swipe start, reversal, a flick waiting for its page).
2. Out-of-date pages on screen: the front at rest, both pages during a swipe.
3. A pending reload, once the swipe is idle.
4. Idle work: bring the back up to date (a clean copy of the front).

## Key updates at rest

Front A (page *p*), back B (page *p*, clean), spare C:

1. `SET_KEYS` changes keys 4 and 7 on page *p*. Both A and B get `dirty = {4, 7}`.
2. The render task draws keys 4 and 7 into B (≈ 1.4 ms). B is clean.
3. It publishes a table mapping *p* → B. A is retiring.
4. At the next frame start the ISR latches it, and both keys appear in the same frame. A is now free.
5. It draws keys 4 and 7 into A (≈ 1.4 ms). A is the clean back copy again.

- **Cost:** two renders per changed key, and the update shows ≤ 1 frame + render time after the message.
- **Batching:** no extra logic is needed. Updates that arrive during step 2 or while waiting for the latch just add bits. The next pass draws everything dirty into the back and swaps once. So there's at most one swap per frame (~60/s), and a burst costs about one render per changed key per buffer.
- **The second render** (step 5) can wait. If another update comes first, its back render covers both sets of keys. Doing it at idle keeps the next update fast.
- **A back that isn't a copy** (e.g. just after a swipe or reload) costs a full render on the first update (≈ 15–17 ms). The idle step after a swipe settles avoids that (see below).
- **Press feedback** is a key update (pressed face, then normal face on release or cancel), shown ≤ 1 frame + ~0.7 ms after the touch. No overlay is needed.
- **Atomicity:** a `SET_KEYS` is applied to the model under the mutex, and the render copies state under it. So a partly applied message is never shown, and all its keys on a page appear in the same frame.

## Swipe start: wait for the incoming page

**Rule: the swipe never scrolls to show a page that isn't mapped.** The swipe task keeps a *wanted* set: the pages in view plus any page it is waiting for. The render task maps pages in that set and unmaps only pages the swipe has dropped. The swipe drops a page only once it's out of view. So a page the swipe can see stays mapped until the swipe lets go of it.

**Sequence: swipe from page 2 to page 3** (A front = 2, B back = 2, C spare):

| When | Swipe task | Render task | Screen |
|---|---|---|---|
| Frame F0 | Finger passes the slop moving left. Press is cancelled. Wanted = {2, 3}. **Hold:** scroll stays at page 2. | Draws page 3 into C. That's a full render (≈ 15–17 ms), or none if C is already a clean copy of 3. | Page 2, still |
| F0 + ~17 ms | | Publishes slot 3 → C. Wakes the swipe task. | |
| Same pass | Sees page 3 mapped. **Starts the drag**, jumping to the finger's current offset from the slop point. | | |
| F1–F2 | Page follows the finger 1:1 | | Pages 2 and 3 |
| Release, glide | Glides to 3 | | |
| Settled | Wanted = {3} | Unmaps 2 (A). At idle, makes a clean copy of 3 in B (a full render). A keeps page 2 as the spare. | Page 3 |

- **Delay:** the render plus at most a frame, so the page starts moving **1–2 frames (16–33 ms) after the slop** instead of 1. That should be hard to notice, because the first movement is slow and touch is already polled at 100 Hz. It's zero when the spare is already a clean copy of the incoming page (swiping back to the page just left).
- **Catching up: jump to the finger.** The drag keeps its anchor at the slop point, as today. When the page is ready, the scroll jumps to where 1:1 tracking would have put it, then follows 1:1.
  - The jump is roughly the finger's travel during the wait, typically a few tens of px, in one frame at the very start of the motion. That's unlikely to be noticed, whereas a page sitting out of place under the finger for the whole swipe would be.
  - If the jump does turn out to be visible, easing it in over a few frames is an option.
- **A flick released before the page is ready:** the swipe task keeps the release velocity. When the page becomes mapped, it glides there with that velocity (capped as now, so no overshoot). A slow release before the page is ready is just a cancelled drag: the page never moved, so nothing happens. A late page is still released; it stays mapped as the spare.
- **Rubber-banding** at the first or last page needs no page. The swipe doesn't wait, and the ISR shows `COLOR_BACKGROUND` past the end, as now.
- **Spring-back:** settles on 2. Slot 3 is unmapped, and C stays tagged 3 (clean) as the spare.
- **Catch mid-glide:** both pages are already mapped, so there's no wait.

**Direction reversal** (dragging towards 3, then back past the origin towards 1):
1. The swipe task adds 1 to the wanted set and **clamps the scroll at page 2** (offset 0). The page waits at rest while the finger carries on.
2. The render task draws page 1 into the free buffer (B, the back copy of 2) and publishes slot 1. Slot 3 is out of view, so it is unmapped, and C keeps page 3 as the spare.
3. The swipe jumps to the finger's offset and continues 1:1.
4. **Reversing again** towards 3: C is a clean copy of 3 (unless page 3 changed), so it's remapped with no wait.

**Multi-page drags** (catch a glide and drag on to page 4) follow the same rule. The scroll is clamped at the edge of the last mapped page until 4 is mapped, and the page that left the view frees a buffer.

## Key updates during a swipe

Both pages in view are mapped (A = outgoing 2, C = incoming 3). B is free.

1. A `SET_KEYS` touches page 2, page 3, or both. All buffers tagged with those pages get dirty bits.
2. The render task picks the dirty page with more of it on screen. It draws it into B: only the dirty keys if B is a copy of that page (usually true for the outgoing page, since B was its back copy), otherwise a full render (≈ 15–17 ms). Then it publishes the swap.
3. At the next frame start, the old buffer for that page retires, then becomes free.
4. If the other page is dirty too, it's drawn into the newly free buffer (usually a full render) and swapped in the same way.

- **Updates during a swipe needn't be immediate.** They can take several frames if needed. The render task may coalesce them, or defer them (even until the swipe settles) to keep the motion smooth. The swipe-start hold has priority.
- **The two pages may update 1–2 frames apart** (≈ 16–33 ms). That's acceptable, and it only happens when the host changes both pages during the second or so of a swipe.
- **Swiping keeps going:** scroll and table are latched together each frame, so a page swapped mid-glide keeps moving smoothly.
- **Waits:** a reversal or settle that needs a buffer while one is retiring waits ≤ 1 frame for it, under the same hold rule.

## Reloading the whole layout

A **reload** replaces every page: `RESET`, `SET_PAGE_COUNT`, or a mode 3 activity switch.

**Deferred while swiping** (dragging, waiting for a page, or gliding):
1. The layout module starts a **pending layout** and applies the reload to it. Every model change after that, such as the host's `SET_PAGE_COUNT` and `SET_KEYS` after a `RESET`, also goes to the pending layout. A second reload clears it again, so only the latest counts.
2. **The user sees the old content** until the swipe ends. The live layout and the buffers are untouched.
3. **When the swipe is idle** (finger up, glide settled):
   1. The pending layout becomes live, and every buffer's tag is cleared.
   2. **The page resets.** For the user a reload is a mode switch, so the old page index doesn't carry over. The new current page is page 0, or a page the client names with the reload.
   3. The new current page is rendered into a free buffer (≈ 15–17 ms).
   4. The swap is published. If the page index changed, the render task hands the table to the swipe task, which stores the new scroll and the table pointer in the same pass, so both are latched at the same frame start.
   5. `PAGE_CHANGED` is sent if the index changed.
   6. At idle, the render task makes a clean back copy.

**Not swiping** (idle, or a finger resting on a key): the reload is applied to the live layout straight away, from step 3.1. A held key gets a `KEY` cancel.

**Touch during the reload render** (~1 frame):
- **A drag** needs no special case. Its incoming page is requested and the hold rule makes it wait until the reload is published.
- **A key press** that starts in that window is ignored, because the key it landed on is about to be replaced.

## Status bar and pager

- **Double buffered,** once they have content. Each update re-renders the whole bar into the back (≈ 1–1.5 ms) and swaps it at frame start. The bars are small, so the "draw it twice" trick isn't worth it.
- **Until then** they stay as solid fills in the ISR.

## Memory budget (8 MB PSRAM)

| Use | 4-buffer pool (first design) | 2 buffers, in place (second) | **This design** |
|---|---|---|---|
| Page buffers | 2,500 KB | 1,250 KB | **1,875 KB** (3 × 625) |
| Status + pager | 250 KB | 125 KB | **250 KB** |
| Overlay tiles (SRAM) | ~54 KB | 0 | **0** |
| **Left for resources** (icons, fonts), Wi-Fi in mode 3 | ~3.8–4.8 MB | ~5.2–6.2 MB | **~4.6–5.6 MB** |

Code and rodata from PSRAM (~0.5–1.5 MB) and the model and staging (~0.1 MB) are the same in all three. Simplicity costs 750 KB more than the in-place design, which still leaves plenty for icons (~5–20 KB each). `GET_INFO.resource_capacity` should report what's actually left.

## Modes 2 and 3

- **Only the source of the model changes** (flash or NVS, not the host). The buffers, the table and the render task are untouched.
- **A mode 3 activity switch** is a reload, so it's deferred while swiping.
- **Flash writes** may stall the bounce ISR (`CONFIG_LCD_RGB_ISR_IRAM_SAFE` is off). They stall the render task too, which only delays updates. Nothing is ever half-drawn on screen.

## Alternatives considered

| Alternative | Why not |
|---|---|
| **Neighbours pre-rendered** (4-buffer pool, per-key signatures, press overlay) | Designed when render cost was unknown. A page renders in about a frame, so holding the swipe start is simpler. It also cost 2.5 MB. |
| **Drawing in place in the beam's wake** (ISR reports fill progress, draws timed to it) | Saves one buffer, but needs ISR line wakes, a timing rule, and handling for preempted draws (a half-drawn key for a frame). Off-screen-then-swap can never tear. |
| **Column-by-column incoming render** with a partly drawn page shown | Avoids the 1–2 frame hold at swipe start, but needs a "ready columns" field in the table and placeholder fills in the ISR. Holding the start is simpler, and the delay should be hard to notice. |
| **Speculative render on touch-down** | Direction is unknown, and most touches are taps. Revisit only if the swipe-start delay is noticeable: the cheaper fix is to keep the spare as a clean copy of the likely next neighbour at idle. |
| **Two page buffers** | Works at rest and for swipes, but there's no free buffer to update a page mid-swipe (updates would wait for settle), and every update after a swipe costs a full render. |
| **Copy-forward by memcpy** (625 KB) | ≈ 15–25 ms of saturated PSRAM bus, and it evicts the ISR's preloaded chunk. The two-render rule does the same job for ~0.7 ms per key. |
| **Per-key tiles composed by the ISR** | Many small copies per row from scattered tiles. One contiguous preload can't cover them. |
| **GDMA (`esp_async_memcpy`)** | Offloads the CPU but not the PSRAM bus, and adds cache sync. Not needed. |

## Measurements to make

Log `max_fill_us` (already printed once a second) for each.

1. **The swipe-start delay on the hardware.** Does a 1–2 frame hold feel laggy, or is the jump to the finger visible? If the hold is the problem, keep the spare as the likely next neighbour at idle. If the jump is, ease it in.
2. **Fill impact of a full-page render** on core 1, at rest and mid-glide. If the slowest fill goes above ~350 µs, renders need throttling. That would lengthen the swipe-start hold, so measure it first.
3. **Page background cost** (~360 KB of gaps): estimated at 5–7 ms, it could cost as much as the keys. It sets the hold time.
4. **Labels, an rle565 tinted face, 120x112 keys, and the `-Og` build.** The hold grows with all of these. In the debug build it could be several frames.
5. **Cache write-allocate:** whether a write miss to PSRAM fetches the line. This affects the background cost.

## Open questions

- **Reload batches in mode 1:** `RESET` then `SET_KEYS` can briefly show empty keys at rest. One fix is to let a `RESET` defer display until the next `SET_KEYS` (or a short timeout), or to add a flag to `RESET`. This needs a protocol decision.
- **The page a reload starts on:** page 0, or should `RESET` (or the mode 3 activity) carry a start page? Also, `PAGE_CHANGED` after a reload may need its own `cause` value ("layout reloaded"), since the page was set by the reload rather than by a swipe or `SET_PAGE`.
