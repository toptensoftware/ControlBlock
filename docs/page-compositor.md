# Page Compositor

Draws key images into the page buffers and decides, each frame, which buffers the bounce fill shows. Status: **implemented**: the core in `firmware/main/page_compositor.cpp/.h` (unit tested on the host by `firmware/test/`), run in a task by `compositor.cpp/.h`. Keys are single opaque images, drawn row by row **straight into the PSRAM page buffer**, only in the gap after each bounce fill, in ~1 KB chunks that stop at a deadline (see [Drawing a key](#drawing-a-key-straight-into-psram-in-fill-gaps)). The drawing method was measured on the hardware by a spike (2026-11-12: no late fills, no lasting shift); the compositor built on it has passed the host tests but its stress run (`RUN_STRESS`) has not been run on the hardware yet. It replaces the page tags, dirty bits, frame table and swipe-start hold in [`screen-buffers.md`](screen-buffers.md) (see [Changes to screen-buffers.md](#changes-to-screen-buffersmd)).

Until 2026-11-12 keys were 15 fixed 128x128 cells, each drawn from a background and a foreground image with alpha into a 32 KB render cell in internal RAM and copied to PSRAM in slices. That design is gone (job `wip/wip-key-render-redesign.md` has its measurements).

## Summary

- **Input:** a page is up to 32 keys (`COMP_MAX_KEYS`). Each key is a **rect** in the page area and one **opaque** rle565 image exactly the rect's size, or NULL (drawn as background). Rects don't overlap. Whatever no key covers is black (`COMP_BACKGROUND`).
- **Pointer compare.** Each page buffer keeps a **record** per rect: the image drawn there. A key is redrawn when the page's image pointer at that rect differs from the record. Pressed state, tint and labels are just different images.
- **Opaque only:** no layers, no blending, no clear. Drawing a key is pure writes of its decoded rows.
- **Three buffers:** at most two on screen (during a swipe) and **one back** being drawn into.
- **A negotiation at each frame start.** The bounce fill asks for the one or two pages the scroll shows. The compositor returns at least one page, and the second only if it's ready. When the second isn't ready, the bounce fill shows the first page at rest. **That's the swipe-start hold.** The swipe code never knows about it.
- **Incremental by default.** A page on screen is updated in place over several frames: at each frame start, whatever is ready is swapped in, and the old front becomes the back.
- **Flush** holds a page back until all its pending changes are drawn, then hands it over in one go. A page coming into view is always flushed.
- **One page at a time.** The compositor finishes a page's pending changes before working on another.
- **Image lifetime is the client's job.** The client must not free an image the compositor still references (see [Image pointers](#image-pointers)).

## Terms

| Term | Meaning |
|---|---|
| **Key rect** | The area of a page buffer a key draws into. Rects on a page don't overlap. |
| **Layout** | A buffer's (or page's) set of rects, in the page's key order. |
| **Record** | Per buffer, per rect: the image drawn there (NULL = background). Or **unknown**, which matches nothing. |
| **Erase list** | Per buffer: rects that may still hold pixels outside its layout (left there by a different layout). They're filled with background before the buffer is offered. |
| **Unit** | One piece of drawing: a key, or an erase. Drawn in chunks over several fill gaps. |
| **Target** | A page's keys as the client defines them now. |
| **Snapshot** | A copy of a page's target, taken when the compositor starts work on that page. Work on the page is done when its snapshot is on screen. |
| **On screen** | A buffer handed to the bounce fill at the last frame start. The compositor never draws into it. |
| **Front** | The on-screen buffer for a page. |
| **Back** | The one buffer being drawn into. |
| **Offer** | The back, put up as ready for a page. The bounce fill takes it at the next frame start if it asks for that page. |
| **Free** | Not on screen and not the back. It keeps its pixels and records. |

## Keys

```c
#define COMP_MAX_KEYS   32      // keys per page (key_geometry.h)

typedef struct {
    KeyPixelRect rect;      // in the page area: x, y, w, h (int16)
    const void  *img;       // opaque rle565 image exactly rect.w x rect.h, or NULL: COMP_BACKGROUND
} comp_key_t;

typedef struct {
    int        count;
    comp_key_t keys[COMP_MAX_KEYS];
} comp_page_t;
```

- **Drawing a key:** decode the image's rows straight into the page buffer at the rect (`opaque_draw_rows`, `opaque_row.cpp`), a chunk at a time in fill gaps (see [Drawing a key](#drawing-a-key-straight-into-psram-in-fill-gaps)).
- **Opaque:** every pixel is written, so nothing is cleared first and nothing underneath is read. The image store refuses images that aren't fully opaque, so the client only ever has opaque ones.
- **Size:** the image must be exactly the rect's size; the client checks this when keys are set. An image whose header doesn't fit the rect (`opaque_image_fits`) is drawn as background, never past the rect, and debug builds assert on a width mismatch.
- **NULL** is drawn as background: a key with no image is a black rect.
- **Padding** between the visible faces is baked into the images (black edges).

## Layout

Keys sit on a grid of 1U cells, `KEY_CELL` = 144 px, 5x3 cells packed edge to edge: 720x432, centred in the page area (800x440 below the 40 px status bar; there is no pager bar: the pager is pills in the status bar). The grid's origin is (40, 4) in the page area (`KEY_GRID_X0`, `KEY_GRID_Y0`), (40, 44) on the screen. Key rects are placed and sized in quarter units (0.25U = 36 px, `KEY_QUARTER`) from that origin, so a key can be 1U, 2U wide, 2x2, half a unit tall, or sit off the whole-unit grid; `key_geometry.h` converts them to pixels (`KeyRectPixels`) and checks them (inside the grid, no overlaps, at most 32 a page). These constants are reported to the host in `GET_INFO` ([`protocol.md`](protocol.md)).

Everything outside the grid, and any part of the grid no key covers, is background. Buffers are filled with background once, when they're allocated, and start with no records, so a boot or a build with the same layout fills nothing.

A host test checks that the cells abut and the grid is centred in the page area.

## Uncovered areas: the erase list

Pages can have different layouts, and the three buffers are shared by every page, so a buffer that last showed one layout may be given another (a swipe between differently laid out pages, a layout change on a page). Areas the old layout drew and the new one doesn't cover must be filled with background. Only then, never on a normal build:

- **`Relayout`** gives the back the snapshot's layout before any drawing (it writes no pixels). Rects in both layouts keep their records; new rects are unknown; rects the buffer leaves go on its **erase list** (if the list is full, it becomes one rect round everything).
- **An erase unit** fills its rect minus the layout's rects (per row, the spans between the keys), so it never touches a key and erases can run in any order. Erases come before any key and block the offer.
- **Invariant** (checked by the host harness on every buffer): outside its layout and its erase list, a buffer is background.

## Drawing a key: straight into PSRAM in fill gaps

**The problem it solves.** Writing a PSRAM page buffer on core 1 competes with the bounce fill's PSRAM reads on core 0: writes that overlap a fill make it late (flicker, shifted or corrupted-looking lines). So page buffers are written only in the gap after each fill, and the work in a gap must end before the next fill is due.

**How a unit is drawn:**
1. **Begin** (`BeginKey` / `BeginErase`): picks the unit; its record becomes unknown until it's done. No pixels are written.
2. **Chunks** (`DrawChunk`): in a fill gap, the next ~`COMP_CHUNK_BYTES` (1 KB, at least one row: 3 rows of a 1U key, 1 row of a 2U key) is decoded straight from the image in PSRAM into the page buffer, then **written back to PSRAM at once** (`port::CacheWriteback`), so its bus traffic stays in the gap and each write-back is short.
3. **Finish** (`FinishUnit`): after the last chunk the record is set (or the erase rect removed), and for an incremental job the back is offered if the swap rule holds. A key is "drawn" only when its last chunk is.

**The deadline is measured from the fill's start.** The next fill is due one fill period (`DISPLAY_BOUNCE_BUDGET_US`, ~615 µs at 20 MHz) after the last one started. Work stops `COMP_GAP_MARGIN_US` (65 µs) before that, and a chunk is started only if its worst case, `COMP_CHUNK_WORST_US` (140 µs, write-back included: a chunk measured up to 131 µs under PSRAM contention), still fits. So the deadline is ~550 µs after the fill start, and a long fill, or a gap reached late, gets less work by itself. Several chunks fit in a gap, and one gap can finish a key and carry on into the next.

**Fill gaps.** At the end of every fill the bounce fill calls `fill_gap_done(fill_start_cycles)` (`fill_gap.cpp`), which wakes the compositor task (only while it has a unit to draw: `WantsGaps()`) or the status bar's task, one per gap in turn, so two PSRAM writers never overlap. Each client gets the time its fill started. The compositor task keeps the last gap it was given and passes it to every `Step(now, gap)`; the deadline makes an old one harmless. The status bar still uses `fill_gap_fresh(COMP_GAP_FRESH_US)` (50 µs after the fill end). A host test checks that page buffers are written only in gaps, and that no chunk starts with less than its worst case left.

**Measured on the hardware** (spike v2, release `RUN_STRESS`, 2026-11-12: a page of opaque keys drawn into an off-screen PSRAM buffer this way, with deadlines at fixed times after the fill's end):

| Gap work ends after the fill's end | Drawn per gap | Full page (keys + margins) |
|---|---|---|
| 150 µs | ~1.6–1.8 KB | ~255–287 ms |
| 250 µs | ~3.2–3.5 KB | ~130–140 ms |
| 350 µs | ~4.8–5.1 KB | ~89–95 ms |

- In every case: no fill started more than 3 µs late, fills unchanged (144 µs at rest, 186 µs mid-swipe), none missed, every frame present, pixels correct, nothing seen on screen.
- A ~1 KB chunk took at most ~82 µs, its write-back at most ~48 µs. Decoding writes ~65 µs per KB (~15 MB/s).
- Drawing from the image in PSRAM, from a copy in internal RAM, or from a copy of just the chunk's rows made no difference, so images are read straight from PSRAM.
- The chosen deadline (550 µs after the fill start, so ~400 µs after a 144 µs fill) is close to the 350 µs case.
- **An earlier version overran the gap** (8–12 KB slices: gap work up to 0.5–1 ms) and wrote a whole page with one write-back: that gave flickering lines and once a lasting shift of the picture (see [Fill timing](#fill-timing)). Hence the small chunks, the immediate write-backs and the deadline.

**Costs:**
- **A 1U key** (41 KB) takes ~8–9 gaps, **~5 ms**. A full page of 15 1U keys takes **~90 ms** (~5 frames), about **2.5x** the old render-and-copy design (~36 ms). Not yet measured with the real compositor on the hardware (the stress run's `build` column).
- **A reset** abandons the unit being drawn (its record stays unknown), so it never waits for a key.
- **The frame-start guard** estimates a unit's time at `COMP_GAP_BYTES_EST` (3 KB) per fill period.
- **No render cell:** the 32 KB internal-RAM cell of the old design is gone.

### Fill timing

`screen.cpp` measures each fill's start as well as its length, because a fill that **starts** late (its ISR held off) can be short and still miss the DMA. Within a frame the fills are evenly spaced, one fill period apart, so each start is compared with that grid. `screen_stats_t` has `max_late_us` (latest start), `max_end_us` (lateness plus duration: must stay under the period), `missed` (fills that ended past it that way), `seq_breaks` (a fill's lines didn't follow the last fill's: the driver resynced) and `buffer_mismatches` (a fill wrote the bounce buffer the DMA is sending: a lasting desync). The stress run prints them on every line.

**The lasting shift.** In bounce mode the IDF v6.0 RGB driver picks the buffer to fill from a count of EOF interrupts (`bb_eof_count % 2`); with `LCD_RGB_RESTART_IN_VSYNC` the VSYNC restart never resets that count. If an EOF interrupt is ever lost or merged (core 0 held off for more than a fill period), the parity flips for good: every fill then writes the buffer being sent, and the picture stays shifted by one 15-line chunk. A late fill on its own spoils only its frame. `buffer_mismatches` detects it.

## Frame start: the negotiation

At the start of each frame (`y == 0`), the bounce fill latches the scroll position as now. It works out the one or two pages the scroll shows, skipping any before the first page or past the last. Then it calls `comp_frame()` (in ISR context) with those pages.

**What `comp_frame()` does:**
1. **Takes the offer** if there is one and its page was asked for. That buffer becomes the page's new front, and the old front becomes free.
2. **Returns a buffer for each page asked for that has one** (a front, old or new).
3. **If none of them has one,** it returns what was on screen last frame. This is for after a reload (see [Reload](#reload)). At boot there's nothing, so it returns nothing.
4. **Frees** every buffer that was on screen and isn't returned now. The ISR has just stopped reading it, so it's free straight away; there's no retiring state.
5. **Wakes the compositor task,** telling it which pages were asked for and which of them were missing.

**What the bounce fill does with the result:**

| Asked for | Returned | Bounce fill shows |
|---|---|---|
| The pages it asked for | All of them | Scroll as latched |
| Two pages | One of them | That page at rest (offset 0), ignoring the scroll: **the hold** |
| Pages it didn't ask for | (last frame's, after a reload) | The first one at rest |
| | Nothing | `COMP_BACKGROUND` |

Rubber-banding needs no special case: past the ends there's no page to ask for, and the bounce fill shows background there as now.

**`comp_frame()` must be fast** (it's in the ISR): a handful of compares and one atomic exchange. It never draws, copies a target, or calls the client.

### Swipe start

1. The drag passes the slop, and the swipe moves the scroll so page *q* comes into view. It doesn't wait for anything.
2. **Next frame start:** the bounce fill asks for {*p*, *q*}. *q* has no buffer, so only *p* comes back, and *p* is shown at rest.
3. The compositor task wakes, sees *q* missing, and builds it as a flush into the free buffer that differs least from *q* (≈ 5 frames for a full page; nothing if a free buffer already holds *q*, e.g. the page just left). If that buffer last held another layout, its uncovered areas are erased first. Then it offers it.
4. **The frame start after that:** the offer is taken and both pages come back. The bounce fill uses the real scroll, so the page jumps to where the finger is and follows it 1:1.

- **Delay:** for a full page about **6 frames (≈ 120 ms) after the slop** (it was 3–4 frames with the old render cell; not yet judged on the hardware). It's 1 frame when a free buffer already holds *q*. The jump to the finger reads as a small kick at the start of the drag.
- **The jump to the finger** is the one `screen-buffers.md` already planned for, but now it falls out of the negotiation.
- **A flick released before *q* is ready:** the swipe glides as normal. The page jumps to wherever the glide has got to when *q* arrives.
- **Direction reversal** (dragging towards *q*, then back past *p* towards *o*): the bounce fill asks for {*o*, *p*}. *q* is no longer asked for, so its buffer is freed, and *p* is held at rest until *o* is ready. Reversing again towards *q* finds *q*'s old buffer free and unchanged (a 0-key build, so it can be offered at once).
- **Multi-page drags** work the same way: the screen holds at the last page that's ready.

**Press highlight:** the press-to-drag cancel removes the press highlight from a key on *p*, which is an update to *p* just as *q* is needed. Under [one page at a time](#work-order), it would delay *q*. So the client **delays the un-highlight** until the swipe is under way (e.g. 0.5 s). The highlighted key slides off with *p*, which looks fine.

## Updating a page on screen

### The swap rule: never go backwards

For page *p* with front F, back B and snapshot S, the back may be offered when:
- B has S's layout and nothing left on its erase list,
- **every key** in B matches either F (unchanged) or S (updated), and
- **at least one key** in B differs from F (it's ahead), or B's layout differs from F's.

Keys match by rect. The first condition stops a swap from putting an older image back on screen. Unknown records match nothing, so an unknown key must be drawn first.

**Draw order in the back:**
1. **Erases:** areas the back's previous layout left (see [Uncovered areas](#uncovered-areas-the-erase-list)). These block the offer.
2. **Catch-up keys:** keys where B matches neither F nor S. These block the offer.
3. **Progress keys:** keys where B doesn't match S yet.

Keys in the page's key order. A unit is never split across buffers.

### Incremental (the default)

After each key, if the swap rule holds, the compositor puts up the offer. At the next frame start the bounce fill takes it, and the old front comes back as the new back. That buffer was last drawn into two frames ago, so it must first catch up on the keys the other buffer got. The records say exactly which.

**Example:** 15 keys on the page in view change at once, and (for the example) about 8 keys get drawn between frame starts.

| Frame | Back | On screen |
|---|---|---|
| 1 | B: draws keys 0–7, offers B | A: all old |
| 2 | A: catches up 0–7 | B: 0–7 new |
| 3 | A: draws 8–14, offers A | B: 0–7 new |
| 4 | B: catches up 8–14 (idle work) | A: all new |

- **Every key is drawn twice:** once to show it, once to catch up the other buffer. So incremental shows about half as many new keys per frame as could be drawn. A single changed key (a press highlight, say) shows at the next frame start after its last chunk (a 1U key is ~5 ms).
- **Not starting a key across a frame start.** While drawing a key, the compositor withdraws its offer so a half-drawn key is never taken. If it were busy all the time, it would always be mid-key at frame start and swaps would rarely happen. So, with an offer up, it doesn't start a unit unless it can finish it (estimated at `COMP_GAP_BYTES_EST` per fill period, plus one fill) at least `COMP_SWAP_GUARD_US` (500 µs) before the next frame start; otherwise it waits for the frame start and continues after. If a key overruns anyway (gaps cut short, the task pre-empted), that frame just doesn't swap.
- **The fill sets the pace.** The compositor writes page buffers only in fill gaps, up to each gap's deadline. That's the throttle: it can't starve the bounce fill of PSRAM bandwidth.

### Flush

`comp_page_changed(page, COMP_FLUSH)` marks the page's next snapshot as a flush: the back is offered only once it matches the snapshot on every key. Then all the changes appear in the same frame.

- **For a host batch:** a `SET_KEYS` that should appear all at once.
- **Call it under the client lock.** The client calls `comp_page_changed(page, COMP_FLUSH)` before releasing the lock it changed the model under. The compositor checks the flag again after taking the snapshot, so a snapshot that contains the change always sees the flag. (Without this, a snapshot taken between the change and the flag would show the batch incrementally; the fuzz tests found it.) A flag can then apply to one more snapshot than needed, which only delays a change, never splits one.
- **A page not on screen** has no front to compare against: until it's fully drawn, its rects show whatever the buffer held before. So it's **always flushed.**

## Work order

**One page at a time.** Once the compositor has taken a snapshot of a page, it finishes it (gets it on screen) before working on another. Switching pages would overwrite the back, and the first page would then need a near-full redraw.

**Only the snapshot counts.** Changes that arrive after the snapshot was taken wait their turn. A page the host keeps changing (a meter redrawn 30 times a second, say) can't then starve the other pages.

**Except a page that's no longer asked for.** If the page being built is swiped out of view, or a reversal means it's no longer wanted, its job is dropped at the next unit boundary. Nothing visible is lost: it isn't on screen, and its records stay accurate if it's asked for again.

When the current page is done, the compositor picks the next job:

1. **A page asked for but missing.** The drag is waiting for it.
2. **A page on screen whose target differs from its front.** If both pages in view have changed, the one with more of it on screen goes first.
3. **Idle work:** with one page at rest, bring the free buffer closest to the front up to a copy of it, so the next update starts from a copy. Idle work isn't a page job: it stops at a unit boundary as soon as there's anything else to do, and loses nothing, because that buffer is the obvious back for the next update anyway.

**Choosing the back:** the free buffer whose records differ from the snapshot on the fewest keys (a different layout counts against it). On a tie:
- **Updating a page on screen:** the one closest to its front, so there are fewest catch-up keys before the first swap. (Otherwise an incremental update could pick a blank buffer and become a flush in effect.)
- **Building a page that isn't on screen:** the one least like a page on screen, so a copy of the front survives. (Otherwise a swipe that's abandoned before the page is ready has its partial build overwritten by idle work restoring the copy.)
- Then the one freed longest ago, so the page just left is kept longest.

At rest there are two free buffers (the back and the spare); during a swipe, only one. Idle work picks the free buffer closest to the front, with the same last tie-break.

## Reload

`comp_reset()` is for a whole-layout reload (`RESET`, `SET_PAGE_COUNT`, takeover and release):

1. It abandons any unit being drawn, then drops the current snapshot and offer.
2. **Every record becomes unknown,** and no buffer belongs to a page any more. The buffers on screen stay on screen, so the old layout stays visible. (Layouts and erase lists stay: they describe pixels, which haven't changed.)
3. When it returns, the compositor references no client images, and the client can free them.

**After a reload,** the client sets the scroll to the new start page. The bounce fill asks for it, gets nothing for it, and is given last frame's buffers, so it keeps showing the old layout at rest. The new page is built as a flush (every key is unknown), and the next frame start swaps it in. The user sees the old layout until the new page appears complete, with no blank frame.

Deferring a reload until a swipe settles stays as in `screen-buffers.md`; that's the layout module's job, not the compositor's.

## API

```c
// Client: copy page `page`'s keys into `out` under the client's own lock.
// Return false if the page doesn't exist. Called on the compositor task.
typedef bool (*comp_get_page_t)(int page, comp_page_t *out, void *ctx);

// Client: is `img` retired (to be freed once a sweep has run)? Called on
// the compositor task during a sweep, without the client's lock.
typedef bool (*comp_is_retired_t)(const void *img, void *ctx);

// Allocate the 3 page buffers in PSRAM, fill each with COMP_BACKGROUND,
// with no records, and start the compositor task (core 1).
esp_err_t comp_init(comp_get_page_t get_page, comp_is_retired_t is_retired, void *ctx);

// Drop references to retired images at the compositor's next step. Returns a
// generation; comp_swept() reaches it (compare as int32_t) once done.
uint32_t comp_request_sweep(void);
uint32_t comp_swept(void);

// Page `page`'s target has changed. Wakes the compositor task.
// flags: COMP_FLUSH holds the page back until all its changes are drawn;
// call it while still holding the client lock (see Flush).
// Any task.
void comp_page_changed(int page, uint32_t flags);

// Whole-layout reload. Blocks until the compositor task has done it (a key
// being drawn is abandoned). On return no client image is referenced. Any
// task except the compositor's.
void comp_reset(void);

// --- Bounce fill side (screen.cpp), ISR context ---

// At the end of every fill: fill_gap_done(fill_start_cycles) in fill_gap.h,
// shared with the status bar. Wakes the compositor only while it's drawing.

// At frame start:

typedef struct {
    int16_t         page;
    const uint16_t *px;     // rows SCREEN_PAGE_W pixels apart
} comp_view_t;

// Ask for `count` (0-2) pages starting at `first`. `offset` is how far the
// first page is scrolled off the left edge; it only decides which page to
// update first when both change. Fills `out` in page order and returns how
// many buffers to show (0-2). See "Frame start".
int comp_frame(int first, int count, int offset, comp_view_t out[2]);

// Counters since the last call (resets them): see Stats.
void comp_get_stats(comp_stats_t *stats);
```

- **The core** (`PageCompositor`) has the same shape: `Frame()` for the ISR side, `PageChanged()`, `RequestReset()` and `RequestSweep()` for clients, and `Step(now_us, gap)` for the task. `Init()` takes the buffers, the client callbacks and a clock (`esp_timer` on the device, simulated in the tests). `Step()` does one piece of work: with a unit active and a gap given, chunks up to the gap's deadline; otherwise work that doesn't write page buffers (snapshots, `Relayout`, picking the next unit, offers, sweeps, resets). `WantsGaps()` says whether a unit is active.
- **`offset`** was added to `comp_frame()` in the implementation: work-order rule 2 ("the one with more of it on screen goes first") needs it, and the compositor doesn't otherwise see the scroll.
- **Taking the snapshot** calls `get_page()`, so the client's lock makes it atomic: a host message is either all in a snapshot or not at all. A key whose rect isn't inside the page area is dropped from the snapshot (the client checks rects; this only stops a bad one writing outside the buffer).
- **The client doesn't track buffers, pages on screen, or holds.** It defines pages, says when they change, and resets. On the device the client is `KeyLayout::GetPage` (`key_layout.cpp`, under the model lock): each key's pixel rect and its shown image (the pressed one while pressed), or NULL if the image isn't the rect's size.

### Stats

`comp_stats_t`: `keys` (keys drawn), `swaps`, `holds`, `max_key_us`, `busy_us`, `erases` (uncovered areas filled), `chunks`, `gaps` (gaps that drew at least one chunk), `bytes` (page buffer bytes drawn), `max_chunk_us` (slowest chunk with its write-back, against `COMP_CHUNK_WORST_US`), `max_gap_end_us` (latest end of a gap's work from its fill's start, against period − `COMP_GAP_MARGIN_US`) and `late_gaps` (gaps whose work ended past the deadline: a chunk took longer than its worst case). The stress run prints them as `B/gap`, `chunk max N/140 us`, `gap end max N/550 us` and `N late`. Tuning: if fills stay well under the period and nothing is late, the margin and the worst case can come down (more per gap, faster builds); `missed` fills or buffer mismatches mean the other way.

## Image pointers

Making sure an image isn't referenced before it's freed is **the client's job**, not the compositor's.

- **The compositor references an image** while it's in the current snapshot (it reads it), in the unit being drawn, or in any record (it compares it).
- **Records count** even though they're never followed: a freed address that's reused for a different image would compare equal, and the key wouldn't be redrawn.
- **Changing an image in place** (same address, new pixels) is the same as freeing it.
- **`comp_reset()`** drops every reference at once. That's the simple way to free a whole layout.
- **Freeing single images:** a non-blocking sweep request, `comp_request_sweep()`/`comp_swept()`, drops references to retired images at the next step. Records that use them become unknown, a job whose snapshot uses them is dropped, and a unit drawing one is abandoned (any other unit carries on). See [image-assets.md, Compositor references](image-assets.md#compositor-references).

## Concurrency

### Who runs where

| Who | Core, priority | Woken by | Does |
|---|---|---|---|
| **Bounce fill** | ISR, core 0 | LCD DMA, every 15 lines (32× a frame) | At `y == 0`: latches the scroll, calls `comp_frame()`, wakes the swipe and compositor tasks. Every chunk: copies rows from the on-screen buffers, then calls `fill_gap_done()`. |
| **Touch task** | core 1, 5 | 100 Hz timer | Reads the GT911 over I2C, passes touches to the swipe code and the touch router. |
| **Swipe task** | core 1, 10 | Frame start | Works out the next scroll and sets it. Never touches the compositor. |
| **Compositor task** | core 1, 4 | Frame start, `comp_page_changed()`, `comp_reset()`, and each fill gap given to it while drawing | Takes snapshots, draws chunks into the back in fill gaps, puts up and withdraws the offer. |
| **Client tasks** (USB, protocol, key router) | core 0 | Host messages, key presses | Change the page model under the client lock, then call `comp_page_changed()`. Call `comp_reset()` on a reload. |

**Why these priorities:**
- **Compositor below touch:** a full page takes several frames, much longer than a touch poll period. Touch and swipe pre-empt it for a few tens of µs each; a gap cut short that way just does less (the deadline is from the fill start).
- **Client tasks on core 0:** keeps host traffic off the core that draws. Core 0 also takes the LCD ISR, which pre-empts everything there anyway.

### One frame (~19.7 ms at 20 MHz)

| When | Core 0 | Core 1 |
|---|---|---|
| **Frame start** (`y == 0`) | Bounce fill: latch scroll, `comp_frame()` (take the offer, update the on-screen set, free buffers no longer shown), wake the swipe and compositor tasks. Fill the status-bar chunk. | |
| Just after | | **Swipe task** sets the next scroll (µs). |
| | | **Compositor task** reads the frame info, picks or continues its job and its next unit. After each key's last chunk, it puts up the offer if the swap rule holds. |
| Every ~10 ms | | **Touch task** pre-empts briefly to poll the GT911. |
| Chunks 1–31 | Bounce fill copies rows from the on-screen buffers. It never sees the back. | |
| After each fill | `fill_gap_done()` | **Compositor task** (while drawing; alternating with the status task when it also has rows to copy) draws ~1 KB chunks into the back, each written back to PSRAM at once, until the deadline ~550 µs after the fill started. |
| Any time | Client tasks change the model and call `comp_page_changed()`. | |
| Last ~0.5 ms+ | | With an offer up, the compositor **waits for the frame start** rather than starting a unit it can't finish (the guard). |

### Shared state

| State | Written by | Read by | How |
|---|---|---|---|
| **Offer** (buffer + page) | Compositor (put up, withdraw); bounce fill (take) | Both | One word, atomic exchange or compare-and-swap. The only thing both cores write in normal running. |
| **Frame info:** the on-screen set (buffer per page), pages asked for, pages missing | Bounce fill at frame start; compositor on reset | Compositor after waking | A spinlock (`portENTER_CRITICAL`), held for a few µs to copy it. |
| **Changed pages** (bitmask + flush bits) | Client tasks | Compositor | Atomic OR to set, atomic exchange to read and clear. Plus a task notification to wake it. |
| **Page model** | Client tasks | Compositor, in `get_page()` | The client's own lock, held only while the snapshot is copied. |
| **Snapshot, records, erase lists, back pixels** | Compositor only | Compositor only | None needed. |
| **On-screen pixels** | Nobody | Bounce fill | They don't change while on screen. |

### The rules that make it safe

1. **A buffer goes on screen only through the offer.** The bounce fill shows a buffer that's already on screen, or takes the offer. So a buffer the compositor sees as free can't appear on screen without the compositor offering it.
2. **Withdraw before drawing.** Before drawing a unit into the back, the compositor atomically withdraws the offer. If the exchange finds the offer gone, the bounce fill has taken it: that buffer is now on screen, and the compositor must re-read the frame info and pick the new back. It never draws into a buffer the exchange didn't hand back. An offered back already has its snapshot's layout, so `Relayout` never changes a buffer that may go on screen.
3. **Pixels before the offer.** Every chunk is written back to PSRAM as it's drawn, and putting up the offer is a release store (a `memw` on Xtensa), so the key's pixels are in place before the bounce fill can see the offer.
4. **Freed means free.** A buffer dropped from the on-screen set at frame start isn't read again: the bounce fill reads page buffers only inside its fills, and the frame's first chunk is the status bar.
5. **No drawing under a lock.** The compositor holds the client lock only to copy a snapshot, and the frame-info spinlock only to copy that. Drawing reads images without a lock, which is why images in a snapshot must stay valid.
6. **PSRAM writes only in fill gaps.** Page buffers are written only by `DrawChunk()`, in ~1 KB chunks, each started only if its worst case ends before the gap's deadline. This is about bandwidth, not correctness: the back is never on screen either way.

### Races and what happens

| Race | Outcome |
|---|---|
| Frame start while the compositor is mid-key | The offer was withdrawn before the key, so nothing is taken. That frame shows the old front. |
| Frame start just as the compositor withdraws the offer | The atomic exchange picks one winner. Either the bounce fill shows the buffer and the compositor moves to the old front, or the compositor keeps it and the frame shows the old front. |
| The host changes a page while the compositor is building it | The change sets its bit and waits. It's picked up in the next snapshot. |
| The page being built stops being asked for (swiped away, or a reversal) | The job is dropped at the next unit boundary (see [Work order](#work-order)). Its records stay accurate, so if it's asked for again the work isn't lost. |
| An offer is up for a page that's no longer asked for | The bounce fill doesn't take it. The compositor withdraws it when it wakes. |
| `comp_reset()` while a key is being drawn | The reset is done by the compositor task at its next step: the key is abandoned (its record stays unknown). The caller blocks on a semaphore until then. |
| A sweep while a key is drawn from a retired image | That unit is abandoned; its record stays unknown and it's drawn again from the new snapshot. |

### `comp_reset()` in detail

1. The client task sets a reset request, wakes the compositor task, and blocks.
2. The compositor task abandons any unit, then withdraws the offer and drops its job and snapshot.
3. It marks every record unknown, and, under the frame-info spinlock, removes the page from every on-screen entry. The buffers stay on screen as the fallback, but belong to no page.
4. It signals the semaphore. The client can now free the old images.

## Costs

Release build, on the hardware, from the spike (see [Drawing a key](#drawing-a-key-straight-into-psram-in-fill-gaps)). The stress run in `main.cpp` (`RUN_STRESS`) is the regression check; its numbers for this design are still to come.

| Step | Measured |
|---|---|
| Decode + write-back to PSRAM | ~65 µs per KB (~15 MB/s); a ~1 KB chunk ≤ ~82 µs, its write-back ≤ ~48 µs |
| Per gap | ~5 KB with the work ending ~350 µs after the fill (the chosen deadline is about there) |
| **One 1U key** (144x144, 41 KB) | ~8–9 gaps, **~5 ms** |
| **Full page, 15 1U keys** | **~90 ms** (~2.5x the old design's ~36 ms) |
| Uncovered areas | **0** on a normal build; filled only when a buffer gets another layout |

In the host harness (simulated 65 µs chunks) a full 1U page builds in ≤ 8 frames, ~5 chunks a gap.

## Memory

- **Buffers:** 3 × 688 KB (800x440 RGB565) in PSRAM.
- **No render cell** (the old design's 32 KB of internal RAM is free).
- **Records:** up to 32 × (rect + pointer + known) ≈ 512 B per buffer, plus an erase list of up to 32 rects (256 B).
- **Snapshot:** 32 × 12 B + count ≈ 390 B.

## Changes to screen-buffers.md

Once this is agreed, `screen-buffers.md` changes as follows:
- **Gone:** page tags and dirty bits; the frame table with its publish, latch and retiring states; the swipe task's wanted set and hold; the "copy key state and clear its dirty bit under the mutex" rule; the key render timings (keys are no longer rendered).
- **Replaced by:** `comp_frame()` at frame start, snapshots, and pointer records by rect.
- **The swipe task** just sets the scroll. It never waits for a page.
- **Freed buffers** are free at once, since the bounce fill is their only reader and it lets go at frame start.
- **Page background** cost drops out of the estimates (uncovered areas are filled only on a layout change).
- The status bar and pager, the memory budget and modes 2 and 3 stay. The status bar and pager are handled separately, not by the compositor.

## Measurements to make

1. **The stress run on the hardware** (release `RUN_STRESS`): late and missed fills, start lateness, buffer mismatches, page build times (`build` column), `chunk max` against 140 µs and `gap end max` against 550 µs, then tune `COMP_GAP_MARGIN_US` / `COMP_CHUNK_WORST_US`.
2. **The swipe-start hold** with ~90 ms page builds: does the kick at the start of a drag still feel fine?
3. **The `-Og` build** with the stress run: it must still keep the display fed.
4. **Lua and fill starts:** the Lua heap's PSRAM writes on core 0 delayed fill starts by up to 139 µs in the spike's run (worst end 421 µs, still under 615 µs). Watch it.

## Open questions

Settled: 3 buffers; one opaque image per key state, pressed state and labels being different image pointers; image lifetime is the client's job; flush holds a page until complete, and there's no forced redraw; padding is baked into the images; one fixed background colour; the status bar and pager are handled separately; one page at a time, limited to its snapshot; the press un-highlight is delayed by the client; keys are drawn straight into PSRAM in time-limited chunks in fill gaps; the compositor runs at priority 4 on core 1.

1. **`COMP_SWAP_GUARD_US`, `COMP_GAP_BYTES_EST`, `COMP_GAP_MARGIN_US`, `COMP_CHUNK_WORST_US`:** 500 µs, 3 KB per fill, 65 µs and 140 µs. The worst case was raised from the spike's 85 µs after the first stress run on the hardware (2026-11-12: chunks up to 131 µs with image replaces or Lua writing PSRAM, gap ends up to 573 µs); the others are still the spike's.
2. **Reload start page:** the scroll is set by the client after `comp_reset()`. Does `RESET` (or an activity) name a start page, and does `PAGE_CHANGED` need a "layout reloaded" cause? (Carried over from `screen-buffers.md`.)
