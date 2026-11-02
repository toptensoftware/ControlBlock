# Page Compositor

Draws key images into the page buffers and decides, each frame, which buffers the bounce fill shows. Status: **implemented and measured on the hardware**: the core in `firmware/main/page_compositor.cpp/.h` (unit tested on the host by `firmware/test/`), run in a task by `compositor.cpp/.h`. Keys are rendered in internal RAM and copied to the page buffers in slices, one slice in the gap after each bounce fill (see [Drawing a key](#drawing-a-key-render-then-copy-in-fill-gaps)); with that, no fill goes over budget even with the compositor drawing flat out. It replaces the page tags, dirty bits, frame table and swipe-start hold in [`screen-buffers.md`](screen-buffers.md) (see [Changes to screen-buffers.md](#changes-to-screen-buffersmd)).

## Summary

- **Input:** a page is 15 keys. Each key is a pair of rle565 image pointers, a **background** and a **foreground**, both 128x128. Either can be NULL.
- **Pointer compare.** Each page buffer keeps a **record** of the image pointers drawn in each key cell. A key is redrawn when the page's pointers differ from the record. Tint, pressed state and labels are just different images.
- **Three buffers:** at most two on screen (during a swipe) and **one back** being drawn into.
- **A negotiation at each frame start.** The bounce fill asks for the one or two pages the scroll shows. The compositor returns at least one page, and the second only if it's ready. When the second isn't ready, the bounce fill shows the first page at rest. **That's the swipe-start hold.** The swipe code never knows about it.
- **Incremental by default.** A page on screen is updated in place over several frames: at each frame start, whatever is ready is swapped in, and the old front becomes the back.
- **Flush** holds a page back until all its pending changes are drawn, then hands it over in one go. A page coming into view is always flushed.
- **One page at a time.** The compositor finishes a page's pending changes before working on another.
- **Image lifetime is the client's job.** The client must not free an image the compositor still references (see [Image pointers](#image-pointers)).

## Terms

| Term | Meaning |
|---|---|
| **Key cell** | The 128x128 area of a page buffer that a key draws into. Cells don't overlap. |
| **Record** | Per buffer, per key: the `bg` and `fg` drawn in that cell, and the rows touched. Or **unknown**, which matches nothing. |
| **Target** | A page's 15 keys as the client defines them now. |
| **Snapshot** | A copy of a page's target, taken when the compositor starts work on that page. Work on the page is done when its snapshot is on screen. |
| **On screen** | A buffer handed to the bounce fill at the last frame start. The compositor never draws into it. |
| **Front** | The on-screen buffer for a page. |
| **Back** | The one buffer being drawn into. |
| **Offer** | The back, put up as ready for a page. The bounce fill takes it at the next frame start if it asks for that page. |
| **Free** | Not on screen and not the back. It keeps its pixels and record. |

## Keys

```c
#define COMP_KEYS       15      // 5 x 3
#define COMP_KEY_SIZE   128     // cell width and height

typedef struct {
    const void *bg;     // rle565 image, 128x128, or NULL
    const void *fg;     // rle565 image, 128x128, or NULL
    uint32_t    flags;  // COMP_KEY_BG_OPAQUE: bg covers the whole cell
} comp_key_t;
```

- **Drawing a key:** clear the rows to be drawn to `COMP_BACKGROUND` (see [Clearing a cell](#clearing-a-cell)), draw `bg`, then draw `fg`, both at the cell origin with alpha. This happens in a cell in internal RAM; the result is then copied to the page buffer (see [Drawing a key](#drawing-a-key-render-then-copy-in-fill-gaps)).
- **Sparse is cheap.** rle565 stores only rows `firstRow` to `firstRow + rowCount` and skips transparent runs. A 128x128 image that only uses the middle 80x80 costs about the same as an 80x80 one.
- **Size:** images must be exactly 128x128. Whatever loads them (the icon cache) checks this. The compositor asserts it in debug builds and clips to the cell either way.
- **NULL** draws nothing, so a key with both NULL is just the cleared cell.
- **Padding** between keys is baked into the images (transparent edges).

## Layout

Cells sit on a fixed grid in the page area: 800x440 below the 40 px status bar, with the pager bar disabled for now (`SCREEN_PAGER_H` 0; 800x420 with a 20 px pager). The gaps between cells are filled with `COMP_BACKGROUND`, one fixed colour for every page, when a buffer is allocated. They're never drawn again.

The gap (`COMP_KEY_GAP`) is worked out from the page area's height: the largest gap, the same across and down, that leaves about half a gap above and below the grid, so the space to the bars matches the space between keys. The grid is centred (`COMP_GRID_X0`, `COMP_GRID_Y0`).

| | No pager (now) | 20 px pager |
|---|---|---|
| Gap between cells, pitch | 18 px, 146 px | 12 px, 140 px |
| Columns (5), first at x | 44 (88 px between pages) | 56 (112 px between pages) |
| Rows (3), first at y | 10 | 6 |
| Visible gap between faces (4 px padding each) | 26 px | 20 px |
| Cell | 128x128 | 128x128 |

A host test checks the gaps are equal, the grid is centred, and the gap is the largest with about half a gap above and below. These are firmware constants (`COMP_KEY_X(i)`, `COMP_KEY_Y(i)`) and are reported to the host in `GET_INFO`.

## Clearing a cell

Only rows that change are cleared and copied:

- **The record keeps a row span** (`y0..y1`) per key: the rows the drawn `bg` and `fg` cover, from their `firstRow` and `rowCount`.
- **Redrawing a key** clears (in the render cell) and copies (to the page buffer) the hull of the old span (to erase it) and the new span (so alpha blends over background, not stale pixels).
- **An unknown record** covers the whole cell.

**Opaque faces skip the clear.** A key with `COMP_KEY_BG_OPAQUE` in `comp_key_t.flags` has a `bg` that covers every pixel of the cell, so nothing is cleared: the face overwrites the cell, all 128 rows are copied, and its record spans all 128 rows (so a later non-opaque key clears them all). The padding between keys is then baked into the face in `COMP_BACKGROUND`, not left transparent. The client sets the flag only for images that pass `comp_image_is_opaque()`, which decodes the image once when it's loaded.

When keys were drawn straight into PSRAM, the clear was the biggest single cost (0.75–0.95 ms of a 2.2 ms key with the display on) and opaque faces saved ~30%. Now that keys are rendered in internal RAM the clear is cheap, so opaque faces matter much less; they're kept as an option (`USE_OPAQUE_FACES` in the test client).

## Drawing a key: render, then copy in fill gaps

**The problem it solves.** Drawing into a PSRAM page buffer on core 1 competes with the bounce fill's PSRAM reads on core 0. Measured with the compositor drawing flat out straight into PSRAM, more than half of all fills went over the 492 µs budget (slowest ~1 ms), whether or not the clear was skipped. Late fills showed as flicker and shifted, corrupted-looking icons. Reducing the work per key didn't help: the fix was to stop PSRAM writes overlapping the fills.

**How a key is drawn now:**
1. **Render** (`BeginKey`): clear, `bg`, `fg` into a 128x128 cell in internal RAM (32 KB). CPU only; it reads the images from PSRAM but writes nothing there.
2. **Copy** (`CopySlice`): the rows to copy go to the page buffer in slices of whole rows, **one slice per fill gap**, and each slice is written back to PSRAM at once (`port::CacheWriteback`) so all its bus traffic stays in the gap.
3. **Finish** (`FinishKey`): the record is updated, and for an incremental job the back is offered if the swap rule holds. A key is "drawn" only when its last slice is copied.

**Fill gaps.** At the end of every fill the bounce fill calls `comp_fill_done()`, which wakes the compositor task (only while a key is being copied). The task calls `Step(now, gap)` with `gap` true only if it got there within `COMP_GAP_FRESH_US` (50 µs) of the fill ending; a gap it reaches late (say, while rendering) is skipped. Page buffers are written only in gap steps; a host test checks this, and that no step writes more than a slice.

**Slice size** (`COMP_SLICE_ROWS_*`), from a copy-gap experiment on the device (copy + write-back straight after each fill, compositor idle):

| Pages in view | Largest slice with no late fills | Slowest fill | Chosen |
|---|---|---|---|
| One (at rest, or held at swipe start) | 8 KB | 70 µs | **32 rows** (8 KB) |
| Two (mid-swipe, one read cold) | 4 KB (8 KB: most fills late) | 262 µs | **12 rows** (3 KB) |

16 KB slices made fills late in every case. Copying without the immediate write-back also worked at these sizes but left less margin (the write-backs land in later fills).

With two pages in view the copy-gap experiment allowed 16 rows (4 KB), and that was used at first. With the compositor drawing flat out it held at a 400-line page area (446 µs, opaque faces) but not at 440 lines: opaque faces mid-swipe went to 527 µs with ~190 fills a second over budget, all in the page area. 12 rows fixed it, with far more margin than the smaller copy alone would suggest (4 KB per gap seems to be just past a threshold).

**Measured with the compositor drawing flat out** (`RUN_STRESS`: every key in view changing every 20 ms for 2 s; 440-line page area, 12-row slices mid-swipe):

| | Slowest fill | Fills over budget | Keys drawn in 2 s |
|---|---|---|---|
| At rest | 120 µs | 0 | ~600 |
| Mid-swipe, transparent faces | 300 µs | 0 | ~305 |
| Mid-swipe, opaque faces | 294 µs | 0 | ~305 |
| Mid-swipe, opaque faces, status bar redrawn every 20 ms | 357 µs | 0 | ~305 |
| Mid-swipe, status bar redrawn every 20 ms, no keys | 206 µs | 0 | 0 |

12-row slices cost ~16% of the keys drawn mid-swipe (~365 with 16 rows); at rest nothing changes.

**Costs:**
- **A full page** takes about 5 fills a key (render, then 4 slices): **~2.5 frames**, against ~1.5 when drawing straight into PSRAM. With two pages in view, ~12 fills a key (11 slices).
- **A reset** waits for the key being copied to finish (a few ms).
- **The frame-start guard** now covers a whole key (render + its slices), worked out per key.
- **A second render cell** (render the next key while the previous one copies) would save about one fill a key (~20%). Not done: the swipe-start hold already feels fine.

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
3. The compositor task wakes, sees *q* missing, and builds it as a flush into the free buffer that differs least from *q* (≈ 2.5 frames for a full page, with *p* held at rest so it uses the larger slices; nothing if a free buffer already holds *q*, e.g. the page just left). Then it offers it.
4. **The frame start after that:** the offer is taken and both pages come back. The bounce fill uses the real scroll, so the page jumps to where the finger is and follows it 1:1.

- **Delay:** about **3–4 frames (≈ 50–65 ms) after the slop** for a full page. It's 1 frame when a free buffer already holds *q*. On the hardware this is barely noticeable; the jump to the finger reads as a small kick at the start of the drag.
- **The jump to the finger** is the one `screen-buffers.md` already planned for, but now it falls out of the negotiation.
- **A flick released before *q* is ready:** the swipe glides as normal. The page jumps to wherever the glide has got to when *q* arrives.
- **Direction reversal** (dragging towards *q*, then back past *p* towards *o*): the bounce fill asks for {*o*, *p*}. *q* is no longer asked for, so its buffer is freed, and *p* is held at rest until *o* is ready. Reversing again towards *q* finds *q*'s old buffer free and unchanged (a 0-key build, so it can be offered at once).
- **Multi-page drags** work the same way: the screen holds at the last page that's ready.

**Press highlight:** the press-to-drag cancel removes the press highlight from a key on *p*, which is an update to *p* just as *q* is needed. Under [one page at a time](#work-order), it would delay *q*. So the client **delays the un-highlight** until the swipe is under way (e.g. 0.5 s). The highlighted key slides off with *p*, which looks fine.

## Updating a page on screen

### The swap rule: never go backwards

For page *p* with front F, back B and snapshot S, the back may be offered when:
- **every key** in B matches either F (unchanged) or S (updated), and
- **at least one key** in B differs from F (it's ahead).

The first condition stops a swap from putting an older image back on screen. Unknown records match nothing, so an unknown key must be drawn first.

**Draw order in the back:**
1. **Catch-up keys:** keys where B matches neither F nor S. These block the offer.
2. **Progress keys:** keys where B doesn't match S yet.

Both in raster order. A key is never split.

### Incremental (the default)

After each key, if the swap rule holds, the compositor puts up the offer. At the next frame start the bounce fill takes it, and the old front comes back as the new back. That buffer was last drawn into two frames ago, so it must first catch up on the keys the other buffer got. The records say exactly which.

**Example:** 15 keys on the page in view change at once, and about 8 keys fit in a frame.

| Frame | Back | On screen |
|---|---|---|
| 1 | B: draws keys 0–7, offers B | A: all old |
| 2 | A: catches up 0–7 | B: 0–7 new |
| 3 | A: draws 8–14, offers A | B: 0–7 new |
| 4 | B: catches up 8–14 (idle work) | A: all new |

- **Every key is drawn twice:** once to show it, once to catch up the other buffer. So incremental shows about half as many new keys per frame as could be drawn. A single changed key (a press highlight, say) shows at the next frame start after its last slice is copied (a few fills after the change).
- **Not starting a key across a frame start.** While drawing a key (render and every slice), the compositor withdraws its offer so a half-drawn key is never taken. If it were busy all the time, it would always be mid-key at frame start and swaps would rarely happen. So, with an offer up, it doesn't start a key unless it can finish it (estimated: `COMP_RENDER_EST_US` plus one fill per slice, plus one) at least `COMP_SWAP_GUARD_US` (500 µs) before the next frame start; otherwise it waits for the frame start and continues after. If a key overruns anyway (gaps missed, the task pre-empted), that frame just doesn't swap.
- **The fill sets the pace.** The compositor renders whenever it has work, at a priority below the swipe task, but writes page buffers only in fill gaps, one slice per gap. That's the throttle: it can't starve the bounce fill of PSRAM bandwidth.

### Flush

`comp_page_changed(page, COMP_FLUSH)` marks the page's next snapshot as a flush: the back is offered only once it matches the snapshot on every key. Then all the changes appear in the same frame.

- **For a host batch:** a `SET_KEYS` that should appear all at once.
- **Call it under the client lock.** The client calls `comp_page_changed(page, COMP_FLUSH)` before releasing the lock it changed the model under. The compositor checks the flag again after taking the snapshot, so a snapshot that contains the change always sees the flag. (Without this, a snapshot taken between the change and the flag would show the batch incrementally; the fuzz tests found it.) A flag can then apply to one more snapshot than needed, which only delays a change, never splits one.
- **A page not on screen** has no front to compare against: until it's fully drawn, its cells show whatever the buffer held before. So it's **always flushed.**

## Work order

**One page at a time.** Once the compositor has taken a snapshot of a page, it finishes it (gets it on screen) before working on another. Switching pages would overwrite the back, and the first page would then need a near-full redraw.

**Only the snapshot counts.** Changes that arrive after the snapshot was taken wait their turn. A page the host keeps changing (a meter redrawn 30 times a second, say) can't then starve the other pages.

**Except a page that's no longer asked for.** If the page being built is swiped out of view, or a reversal means it's no longer wanted, its job is dropped at the next key boundary. Nothing visible is lost: it isn't on screen, and its records stay accurate if it's asked for again.

When the current page is done, the compositor picks the next job:

1. **A page asked for but missing.** The drag is waiting for it.
2. **A page on screen whose target differs from its front.** If both pages in view have changed, the one with more of it on screen goes first.
3. **Idle work:** with one page at rest, bring the free buffer closest to the front up to a copy of it, so the next update starts from a copy. Idle work isn't a page job: it stops at a key boundary as soon as there's anything else to do, and loses nothing, because that buffer is the obvious back for the next update anyway.

**Choosing the back:** the free buffer whose record differs from the snapshot on the fewest keys. On a tie:
- **Updating a page on screen:** the one closest to its front, so there are fewest catch-up keys before the first swap. (Otherwise an incremental update could pick a blank buffer and become a flush in effect.)
- **Building a page that isn't on screen:** the one least like a page on screen, so a copy of the front survives. (Otherwise a swipe that's abandoned before the page is ready has its partial build overwritten by idle work restoring the copy.)
- Then the one freed longest ago, so the page just left is kept longest.

At rest there are two free buffers (the back and the spare); during a swipe, only one. Idle work picks the free buffer closest to the front, with the same last tie-break.

## Reload

`comp_reset()` is for a whole-layout reload (`RESET`, `SET_PAGE_COUNT`, mode 3 activity switch):

1. It waits for any key being drawn to finish, then drops the current snapshot and offer.
2. **Every record becomes unknown,** and no buffer belongs to a page any more. The buffers on screen stay on screen, so the old layout stays visible.
3. When it returns, the compositor references no client images, and the client can free them.

**After a reload,** the client sets the scroll to the new start page. The bounce fill asks for it, gets nothing for it, and is given last frame's buffers, so it keeps showing the old layout at rest. The new page is built as a flush (every key is unknown), and the next frame start swaps it in. The user sees the old layout until the new page appears complete, with no blank frame.

Deferring a reload until a swipe settles stays as in `screen-buffers.md`; that's the layout module's job, not the compositor's.

## API

```c
// Client: copy page `page`'s 15 keys into `keys` under the client's own
// lock. Return false if the page doesn't exist. Called on the compositor task.
typedef bool (*comp_get_page_t)(int page, comp_key_t keys[COMP_KEYS], void *ctx);

// Allocate the 3 page buffers in PSRAM, fill each with COMP_BACKGROUND,
// mark every record unknown, and start the compositor task (core 1).
esp_err_t comp_init(comp_get_page_t get_page, void *ctx);

// Page `page`'s target has changed. Wakes the compositor task.
// flags: COMP_FLUSH holds the page back until all its changes are drawn;
// call it while still holding the client lock (see Flush).
// Any task.
void comp_page_changed(int page, uint32_t flags);

// Whole-layout reload. Blocks until no key is being drawn. On return no
// client image is referenced. Any task except the compositor's.
void comp_reset(void);

// --- Bounce fill side (screen.cpp), ISR context ---

// At the end of every fill: the gap before the next fill is when the
// compositor copies key slices to PSRAM. Wakes it only while it's copying.
void comp_fill_done(void);

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

// Counters since the last call: keys drawn, swaps, held frames, slowest key.
void comp_get_stats(comp_stats_t *stats);
```

- **The core** (`PageCompositor`) has the same shape: `Frame()` for the ISR side, `PageChanged()` and `RequestReset()` for clients, and `Step(now_us, gap)` for the task. `Step()` does one unit of work (render a key, copy one slice when `gap` is set, or other work that doesn't write page buffers). `WantsGaps()` says whether it's mid-key.
- **`offset`** was added to `comp_frame()` in the implementation: work-order rule 2 ("the one with more of it on screen goes first") needs it, and the compositor doesn't otherwise see the scroll.
- **Taking the snapshot** calls `get_page()`, so the client's lock makes it atomic: a host message is either all in a snapshot or not at all.
- **The client doesn't track buffers, pages on screen, or holds.** It defines pages, says when they change, and resets.

## Image pointers

Making sure an image isn't referenced before it's freed is **the client's job**, not the compositor's.

- **The compositor references an image** while it's in the current snapshot (it reads it) or in any record (it compares it).
- **Records count** even though they're never followed: a freed address that's reused for a different image would compare equal, and the key wouldn't be redrawn.
- **Changing an image in place** (same address, new pixels) is the same as freeing it.
- **`comp_reset()`** drops every reference at once. That's the simple way to free a whole layout.

## Concurrency

### Who runs where

| Who | Core, priority | Woken by | Does |
|---|---|---|---|
| **Bounce fill** | ISR, core 0 | LCD DMA, every 15 lines (32× a frame) | At `y == 0`: latches the scroll, calls `comp_frame()`, wakes the swipe and compositor tasks. Every chunk: copies rows from the on-screen buffers, then calls `comp_fill_done()`. |
| **Touch task** | core 1, 5 | 100 Hz timer | Reads the GT911 over I2C, passes touches to the swipe code (and later the key router). |
| **Swipe task** | core 1, 10 | Frame start | Works out the next scroll and sets it. Never touches the compositor. |
| **Compositor task** | core 1, 4 | Frame start, `comp_page_changed()`, `comp_reset()`, and each fill end while copying a key | Takes snapshots, renders keys into the cell, copies slices into the back in fill gaps, puts up and withdraws the offer. |
| **Client tasks** (USB, protocol, key router) | core 0 (proposed) | Host messages, key presses | Change the page model under the client lock, then call `comp_page_changed()`. Call `comp_reset()` on a reload. |

**Why these priorities:**
- **Compositor below touch:** a full page takes ~2.5 frames, much longer than a touch poll period. Touch and swipe pre-empt it for a few tens of µs each; a gap missed that way is just skipped.
- **Client tasks on core 0:** keeps host traffic off the core that draws. Core 0 also takes the LCD ISR, which pre-empts everything there anyway.

### One frame (16.4 ms)

| When | Core 0 | Core 1 |
|---|---|---|
| **Frame start** (`y == 0`) | Bounce fill: latch scroll, `comp_frame()` (take the offer, update the on-screen set, free buffers no longer shown), wake the swipe and compositor tasks. Fill the status-bar chunk. | |
| Just after | | **Swipe task** sets the next scroll (µs). |
| | | **Compositor task** reads the frame info, picks or continues its job, renders keys into the cell one at a time. After each key's last slice, it puts up the offer if the swap rule holds. |
| Every ~10 ms | | **Touch task** pre-empts briefly to poll the GT911. |
| Chunks 1–31 | Bounce fill copies rows from the on-screen buffers. It never sees the back. | |
| After each fill | `comp_fill_done()` | **Compositor task** (while mid-key) copies one slice into the back and writes it back to PSRAM, before the next fill. |
| Any time | Client tasks change the model and call `comp_page_changed()`. | |
| Last ~2 ms | | With an offer up, the compositor **waits for the frame start** rather than starting a key (the guard). |

### Shared state

| State | Written by | Read by | How |
|---|---|---|---|
| **Offer** (buffer + page) | Compositor (put up, withdraw); bounce fill (take) | Both | One word, atomic exchange or compare-and-swap. The only thing both cores write in normal running. |
| **Frame info:** the on-screen set (buffer per page), pages asked for, pages missing | Bounce fill at frame start; compositor on reset | Compositor after waking | A spinlock (`portENTER_CRITICAL`), held for a few µs to copy it. |
| **Changed pages** (bitmask + flush bits) | Client tasks | Compositor | Atomic OR to set, atomic exchange to read and clear. Plus a task notification to wake it. |
| **Page model** | Client tasks | Compositor, in `get_page()` | The client's own lock, held only while the snapshot is copied. |
| **Snapshot, records, back pixels** | Compositor only | Compositor only | None needed. |
| **On-screen pixels** | Nobody | Bounce fill | They don't change while on screen. |

### The rules that make it safe

1. **A buffer goes on screen only through the offer.** The bounce fill shows a buffer that's already on screen, or takes the offer. So a buffer the compositor sees as free can't appear on screen without the compositor offering it.
2. **Withdraw before drawing.** Before drawing a key into the back, the compositor atomically withdraws the offer. If the exchange finds the offer gone, the bounce fill has taken it: that buffer is now on screen, and the compositor must re-read the frame info and pick the new back. It never draws into a buffer the exchange didn't hand back.
3. **Pixels before the offer.** Putting up the offer is a release store (a `memw` on Xtensa), so the key's pixel writes are done before the bounce fill can see the offer. Both cores share the same data cache for PSRAM, so no cache flush is needed.
4. **Freed means free.** A buffer dropped from the on-screen set at frame start isn't read again: the bounce fill reads page buffers only inside its fills, and the frame's first chunk is the status bar.
5. **No drawing under a lock.** The compositor holds the client lock only to copy a snapshot, and the frame-info spinlock only to copy that. Drawing reads images without a lock, which is why images in a snapshot must stay valid.
6. **PSRAM writes only in fill gaps.** Page buffers are written only by `CopySlice()`, one slice per gap, straight after a fill. This is about bandwidth, not correctness: the back is never on screen either way.

### Races and what happens

| Race | Outcome |
|---|---|
| Frame start while the compositor is mid-key | The offer was withdrawn before the key, so nothing is taken. That frame shows the old front. |
| Frame start just as the compositor withdraws the offer | The atomic exchange picks one winner. Either the bounce fill shows the buffer and the compositor moves to the old front, or the compositor keeps it and the frame shows the old front. |
| The host changes a page while the compositor is building it | The change sets its bit and waits. It's picked up in the next snapshot. |
| The page being built stops being asked for (swiped away, or a reversal) | The job is dropped at the next key boundary (see [Work order](#work-order)). Its records stay accurate, so if it's asked for again the work isn't lost. |
| An offer is up for a page that's no longer asked for | The bounce fill doesn't take it. The compositor withdraws it when it wakes. |
| `comp_reset()` while a key is being drawn | The reset is done by the compositor task between keys. The caller blocks on a semaphore until then (≤ one key: its remaining slices, a few ms). |

### `comp_reset()` in detail

1. The client task sets a reset request, wakes the compositor task, and blocks.
2. The compositor task finishes the key it's on, then withdraws the offer and drops its job and snapshot.
3. It marks every record unknown, and, under the frame-info spinlock, removes the page from every on-screen entry. The buffers stay on screen as the fallback, but belong to no page.
4. It signals the semaphore. The client can now free the old images.

## Costs (measured)

Release build, on the hardware. The render bench and the copy-gap experiment that produced these were removed once the design settled (they're in git history); the stress run in `main.cpp` (`RUN_STRESS`) is kept as the regression check.

| Step | Measured |
|---|---|
| 128x128 key drawn straight into PSRAM, display on | ~2.2 ms (clear 0.8–0.9, face ~0.9, icon ~0.37); ~1.4 ms with an opaque face and no clear. Display off: ~0.7 ms. |
| Render into the internal-RAM cell | ~0.6 ms (CPU only) |
| Copy + write-back to PSRAM | ~25 MB/s: 8 KB in ~325 µs, 4 KB in ~166 µs |
| **Render + copy, per key** | ~2.5 ms of task time, spread over ~5 fills (one page in view) or ~10 (two) |
| **Full page, 15 keys** | **~2.5 frames** with one page in view |
| Throughput, flat out | ~300 keys/s at rest, ~180 mid-swipe |
| Page background | **0**: gaps are filled once per buffer |

## Memory

- **Buffers:** 3 × 688 KB (800x440 RGB565) in PSRAM.
- **Render cell:** 32 KB in internal RAM.
- **Records:** 15 × (2 pointers + flags + span) ≈ 200 B per buffer.
- **Snapshot:** 15 × 12 B = 180 B.

## Changes to screen-buffers.md

Once this is agreed, `screen-buffers.md` changes as follows:
- **Gone:** page tags and dirty bits; the frame table with its publish, latch and retiring states; the swipe task's wanted set and hold; the "copy key state and clear its dirty bit under the mutex" rule.
- **Replaced by:** `comp_frame()` at frame start, snapshots, and pointer records.
- **The swipe task** just sets the scroll. It never waits for a page.
- **Freed buffers** are free at once, since the bounce fill is their only reader and it lets go at frame start.
- **Page background** cost drops out of the estimates.
- Key render timings, the status bar and pager, the memory budget and modes 2 and 3 stay. The status bar and pager are handled separately, not by the compositor.

## Measurements to make

Done: per-key time with a 128x128 face, span vs full clear, fill impact flat out (at rest and mid-swipe), and the swipe-start hold on the hardware (see [Drawing a key](#drawing-a-key-render-then-copy-in-fill-gaps) and [Costs](#costs-measured)). Still to do:
1. **The `-Og` build** with the stress run: it must still keep the display fed.
2. **Mid-swipe margin** once more PSRAM traffic arrives (labels, USB, the icon cache): watch the slowest fill mid-swipe (446 µs of 492 now).

## Open questions

Settled: 3 buffers; tint, pressed state and labels are different image pointers; image lifetime is the client's job; flush holds a page until complete, and there's no forced redraw; padding is baked into the images; one fixed background colour; the status bar and pager are handled separately; one page at a time, limited to its snapshot; the press un-highlight is delayed by the client; keys are rendered in internal RAM and copied in fill gaps; the compositor runs at priority 4 on core 1.

1. **`COMP_SWAP_GUARD_US` and `COMP_RENDER_EST_US`:** 500 µs margin and an 800 µs render estimate, not tuned. If incremental updates swap less often than expected, tune them from the stress run's key times.
2. **Task placement for client tasks:** USB and protocol tasks on core 0. Proposed, not decided.
3. **Reload start page:** the scroll is set by the client after `comp_reset()`. Does `RESET` (or a mode 3 activity) name a start page, and does `PAGE_CHANGED` need a "layout reloaded" cause? (Carried over from `screen-buffers.md`.)
