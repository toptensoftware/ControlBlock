// Page compositor scenarios, one per behaviour in docs/page-compositor.md.
// World (harness.h) checks the invariants on every frame start and step as
// well (versions, flushes, layouts, pixels, gap writes and deadlines).

#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"

using namespace SimpleLib;

#define W SCREEN_PAGE_W
#define ALL_KEYS 0xFFFFFFFFu

namespace
{
    // Run frames until `done` or `max` frames; returns the frames run
    template <typename F>
    int RunUntil(World &w, int max, F done)
    {
        for (int i = 1; i <= max; i++) {
            w.RunFrame();
            if (done())
                return i;
        }
        return max + 1;
    }

    // Boot on `page` and run until settled, idle work included
    void Boot(World &w, int page)
    {
        w.scroll = page * W;
        w.RunFrames(16);
        w.CheckSettledOn(page);
    }

    // Records of buffer `b` that match buffer `other`'s at the same rect
    int SameKeys(World &w, int b, int other)
    {
        int same = 0;
        for (int k = 0; k < w.comp.RecordCount(b); k++) {
            const CompRecord &r = w.comp.GetRecord(b, k);
            for (int j = 0; j < w.comp.RecordCount(other); j++) {
                const CompRecord &o = w.comp.GetRecord(other, j);
                same += r.known && o.known && r.rect.x == o.rect.x && r.rect.y == o.rect.y &&
                        r.rect.w == o.rect.w && r.rect.h == o.rect.h && r.img == o.img;
            }
        }
        return same;
    }
}

Fact("Compositor Key Grid Cells Abut And The Block Is Centred")
{
    comp_page_t page;
    MakeLayout(LAYOUT_1U, &page);
    Assert(page.count == KEY_GRID_COLS * KEY_GRID_ROWS);
    for (int k = 0; k < page.count; k++) {
        KeyPixelRect r = page.keys[k].rect;
        Assert(r.w == KEY_CELL && r.h == KEY_CELL);
        Assert(r.x == KEY_GRID_X0 + (k % KEY_GRID_COLS) * KEY_CELL);    // edge to edge
        Assert(r.y == KEY_GRID_Y0 + (k / KEY_GRID_COLS) * KEY_CELL);
    }
    // Centred in the page area: equal margins left and right, top and bottom
    Assert(KEY_GRID_X0 * 2 + KEY_GRID_W == SCREEN_PAGE_W);
    Assert(KEY_GRID_Y0 * 2 + KEY_GRID_H == SCREEN_PAGE_H);
}

Fact("Compositor Boot Shows Background Then First Page")
{
    World w(3);
    w.frame_start += FRAME_US;
    w.FrameStart();
    Assert(w.view_count == 0);          // nothing drawn yet: background
    Assert(w.view.px[0] == nullptr && w.view.px[1] == nullptr);

    // A full page of 1U keys, ~41 KB each, in ~1 KB chunks against each
    // gap's deadline: ~5 chunks a gap
    int frames = RunUntil(w, 12, [&] { return w.ShowsModel(0); });
    Assert(frames <= 8);
    w.CheckSettledOn(0);                // the margins are background too
    Assert(w.comp.TotalErases() == 0);  // the buffers start out background
}

Fact("Compositor Writes Page Buffers Only In Fill Gaps")
{
    World w(3, 1, true);                // 1U, 2U and sparse pages
    w.check_gap_writes = true;
    Boot(w, 0);                         // one page in view
    w.scroll = 300;                     // page 1 built during the hold
    w.RunFrames(10);
    Assert(w.view_count == 2);
    w.ChangeKeys(0, ALL_KEYS, 0);       // two pages in view
    w.ChangeKeys(1, 0x0F0F, COMP_FLUSH);
    RunUntil(w, 40, [&] { return w.ShowsModel(0) && w.ShowsModel(1); });
    Assert(w.ShowsModel(0) && w.ShowsModel(1));
    w.scroll = 2 * W - 300;             // the sparse page: areas to fill
    RunUntil(w, 40, [&] { return w.ShowsModel(1) && w.ShowsModel(2); });
    Assert(w.ShowsModel(1) && w.ShowsModel(2));
}

Fact("Compositor Chunks Stop At The Gap Deadline")
{
    // ~5 chunks fit between a 150 us fill's end and the deadline (fill start
    // + period - margin, the last chunk starting a worst case before it).
    // World checks every step against the deadline.
    World w(1);
    w.RunFrames(3);
    int expect = (CHUNK_US - COMP_GAP_MARGIN_US - COMP_CHUNK_WORST_US - w.fill_one_us) / w.chunk_cost_us + 1;
    Assert((int)w.max_chunks_per_gap == expect);

    // Slower chunks: fewer a gap
    World slow(1);
    slow.chunk_cost_us = 150;
    slow.RunFrames(3);
    Assert((int)slow.max_chunks_per_gap == (CHUNK_US - COMP_GAP_MARGIN_US - COMP_CHUNK_WORST_US - slow.fill_one_us) / 150 + 1);
}

Fact("Compositor Stats Report Chunks Gaps Bytes And Deadline Overruns")
{
    // A full 1U page from boot: every chunk, gap and byte counted, the
    // latest end of a gap's work against the deadline
    World w(1);
    comp_stats_t st;
    w.comp.GetStats(&st);
    RunUntil(w, 12, [&] { return w.ShowsModel(0); });
    w.comp.GetStats(&st);
    Assert(st.keys >= KEY_GRID_SLOTS);                          // (and any idle copy)
    Assert(st.chunks == w.comp.TotalChunks());
    // All 1U, no erases: every chunk is ~COMP_CHUNK_BYTES of whole rows (3 of 144)
    const uint32_t chunk_bytes = COMP_CHUNK_BYTES / (KEY_CELL * 2) * KEY_CELL * 2;
    Assert(st.bytes == st.chunks * chunk_bytes);
    Assert(st.bytes >= st.keys * KEY_CELL * KEY_CELL * sizeof(uint16_t));
    Assert(st.gaps > 0 && st.gaps <= st.chunks);
    Assert(st.gaps * w.max_chunks_per_gap >= st.chunks);       // gaps counted once each, not per Step
    Assert(st.max_chunk_us == (uint32_t)w.chunk_cost_us);
    Assert(st.max_gap_end_us > (uint32_t)w.fill_one_us);
    Assert(st.max_gap_end_us <= CHUNK_US - COMP_GAP_MARGIN_US);
    Assert(st.late_gaps == 0);
    Assert(st.erases == 0);

    // Read once: the counters start again
    w.comp.GetStats(&st);
    Assert(st.chunks == 0 && st.gaps == 0 && st.bytes == 0 && st.max_chunk_us == 0 && st.max_gap_end_us == 0);

    // Chunks slower than COMP_CHUNK_WORST_US end past the deadline: counted
    World slow(1);
    slow.chunk_cost_us = COMP_CHUNK_WORST_US + 65;
    slow.RunFrames(3);
    slow.comp.GetStats(&st);
    Assert(st.late_gaps > 0);
    Assert(st.max_chunk_us == (uint32_t)slow.chunk_cost_us);
    Assert(st.max_gap_end_us > CHUNK_US - COMP_GAP_MARGIN_US);
}

Fact("Compositor Stats Count Erases")
{
    // Swiping onto a differently laid out page fills the uncovered areas
    World w(3, 1, true);
    Boot(w, 0);
    comp_stats_t st;
    w.comp.GetStats(&st);
    uint32_t erases0 = w.comp.TotalErases();
    w.scroll = 2 * W;                   // the sparse page
    RunUntil(w, 60, [&] { return w.ShowsModel(2); });
    w.RunFrames(16);
    w.comp.GetStats(&st);
    Assert(w.comp.TotalErases() > erases0);
    Assert(st.erases == w.comp.TotalErases() - erases0);
}

Fact("Compositor Gap Reached Late Does Less")
{
    // A long fill (two pages in view) or a task that gets to the gap late
    // leaves less of it
    World w(1);
    w.gap_delay_us = 200;
    w.RunFrames(3);
    Assert((int)w.max_chunks_per_gap == (CHUNK_US - COMP_GAP_MARGIN_US - COMP_CHUNK_WORST_US - w.fill_one_us - 200) / w.chunk_cost_us + 1);

    // Too late for even one chunk: nothing is written at all
    World late(1);
    late.gap_delay_us = CHUNK_US - COMP_GAP_MARGIN_US - COMP_CHUNK_WORST_US - late.fill_one_us + 1;
    late.RunFrames(5);
    Assert(late.comp.TotalChunks() == 0);
    Assert(!late.ShowsModel(0));
}

Fact("Compositor Idle Work Makes A Back Copy Of The Front")
{
    World w(3);
    Boot(w, 0);
    int front = w.ShownBuffer(0);
    int copies = 0;
    for (int b = 0; b < COMP_BUFFERS; b++)
        if (b != front && w.Matches(b, 0))
            copies++;
    Assert(copies == 1);
}

Fact("Compositor Swipe Start Holds Until The Next Page Is Ready")
{
    World w(3);
    Boot(w, 0);

    // The drag passes the slop: page 1 comes into view
    w.scroll = 40;
    w.RunFrame();
    Assert(w.AtRestOn(0));              // held: page 0 at rest, ignoring the scroll

    int frames = RunUntil(w, 12, [&] { return w.view_count == 2; });
    Assert(frames <= 8);                // a full page after the slop
    Assert(w.view.offset == 40);        // then it jumps to the finger
    Assert(w.ShowsModel(0) && w.ShowsModel(1));
}

Fact("Compositor Swipe Back To The Page Just Left Needs No Drawing")
{
    World w(3);
    Boot(w, 0);
    w.scroll = 400;
    w.RunFrames(10);
    w.scroll = W;                       // settle on page 1
    w.RunFrames(16);
    w.CheckSettledOn(1);

    // Page 0 is still in a free buffer, unchanged
    uint32_t keys = w.comp.TotalKeys();
    w.scroll = W - 40;
    int frames = RunUntil(w, 5, [&] { return w.view_count == 2; });
    Assert(frames <= 2);
    Assert(w.comp.TotalKeys() == keys);
}

Fact("Compositor Rubber Band Needs No Page")
{
    World w(3);
    Boot(w, 0);
    uint32_t held = w.held_frames;
    w.scroll = -150;
    w.RunFrame();
    Assert(w.held_frames == held);
    Assert(w.view.px[0] == nullptr && w.view.px[1] == w.comp.GetBuffer(w.ShownBuffer(0)));
    Assert(w.view.offset == W - 150);
}

Fact("Compositor Key Update At Rest Shows At The Next Frame Start")
{
    World w(3);
    Boot(w, 0);
    w.ChangeKeys(0, 1 << 7, 0);
    w.RunFrame();                       // draws it and offers it
    w.RunFrame();                       // taken at this frame start
    Assert(w.ShowsModel(0));

    // And the old front catches up so the next update starts from a copy
    w.RunFrames(3);
    w.CheckSettledOn(0);
    uint32_t keys = w.comp.TotalKeys();
    w.ChangeKeys(0, 1 << 3, 0);
    w.RunFrames(4);
    Assert(w.ShowsModel(0));
    Assert(w.comp.TotalKeys() - keys == 2);   // once to show it, once to catch up
}

Fact("Compositor Incremental Update Shows Over Several Frames")
{
    World w(3);
    Boot(w, 0);
    w.ChangeKeys(0, ALL_KEYS, 0);       // each key drawn twice
    int frames = RunUntil(w, 30, [&] { return w.ShowsModel(0); });
    Assert(frames <= 16);
    Assert(w.partial_frames > 0);       // some keys showed before others
}

Fact("Compositor Flush Shows Every Key In One Frame")
{
    World w(3);
    Boot(w, 0);
    w.ChangeKeys(0, ALL_KEYS, COMP_FLUSH);
    int frames = RunUntil(w, 20, [&] { return w.ShowsModel(0); });
    Assert(frames <= 8);
    Assert(w.partial_frames == 0);      // World also checks this every frame
}

Fact("Compositor Swiping Away Drops The Job And Keeps Its Work")
{
    World w(3);
    Boot(w, 0);
    w.chunk_cost_us = 300;              // slow: page 1 takes many frames

    w.scroll = 40;
    w.RunFrame();
    w.RunFrame();
    Assert(w.comp.JobPage() == 1);

    // Back to page 0 before page 1 is ready
    w.scroll = 0;
    w.RunFrames(3);
    Assert(w.comp.JobPage() != 1);
    Assert(w.AtRestOn(0));

    // Coming back finishes page 1 from where it got to
    w.chunk_cost_us = 65;
    w.scroll = 40;
    uint32_t before = w.comp.TotalKeys();
    RunUntil(w, 12, [&] { return w.view_count == 2; });
    Assert(w.ShowsModel(1));
    Assert(w.comp.TotalKeys() - before < 15);
}

Fact("Compositor Direction Reversal Holds The Page")
{
    World w(3);
    Boot(w, 1);
    w.scroll = W + 40;                  // towards page 2
    w.RunFrames(10);
    Assert(w.view_count == 2);

    w.scroll = W - 40;                  // back past page 1 towards page 0
    w.RunFrame();
    Assert(w.AtRestOn(1));              // held at page 1 until page 0 is ready
    RunUntil(w, 12, [&] { return w.view_count == 2; });
    Assert(w.ShowsModel(0) && w.ShowsModel(1));
}

Fact("Compositor Updates Both Pages Mid Swipe")
{
    World w(3);
    Boot(w, 0);
    w.scroll = 300;
    w.RunFrames(10);
    w.ChangeKeys(0, 0x00F0, 0);
    w.ChangeKeys(1, 0x0F00, COMP_FLUSH);
    // Slow but allowed: page 1's only free buffer holds page 0, so all 15 of
    // its keys are redrawn
    int frames = RunUntil(w, 30, [&] { return w.ShowsModel(0) && w.ShowsModel(1); });
    Assert(frames <= 16);
}

Fact("Compositor Reload Keeps The Old Layout Until The New Page Is Complete")
{
    World w(3);
    Boot(w, 1);
    w.chunk_cost_us = 200;

    w.Reload();
    w.scroll = 0;                       // the client resets to the start page
    bool seen_new = false;
    for (int i = 0; i < 40 && !seen_new; i++) {
        w.RunFrame();
        Assert(w.view_count > 0);       // never a blank frame
        seen_new = w.ShowsModel(0);
        if (!seen_new) {
            Assert(w.views[0].page == COMP_NO_PAGE);   // the old layout, at rest
            Assert(w.view.offset == 0);
        }
    }
    Assert(seen_new);
    w.chunk_cost_us = 65;
    w.RunFrames(16);
    w.CheckSettledOn(0);
}

Fact("Compositor Reload Abandons A Key Being Drawn")
{
    World w(1);
    Boot(w, 0);
    w.chunk_cost_us = 400;              // one chunk a gap
    w.ChangeKeys(0, 1, COMP_FLUSH);
    w.RunFrame();
    Assert(w.comp.DrawingBuffer() >= 0);
    w.Reload();                         // done at once (World checks nothing is referenced)
    w.chunk_cost_us = 65;
    w.RunFrames(16);
    w.CheckSettledOn(0);
}

Fact("Compositor Change During A Build Is Picked Up Next")
{
    World w(3);
    Boot(w, 0);
    w.chunk_cost_us = 300;
    w.scroll = 40;
    w.RunFrame();                       // building page 1
    w.ChangeKeys(1, 0x0003, 0);         // after its snapshot
    w.chunk_cost_us = 65;
    w.RunFrames(30);
    Assert(w.ShowsModel(1));
}

// ---------------------------------------------------------------------------
// Layouts: pages with different rects share the buffers, so areas a new
// layout doesn't cover are filled with background, only when it changes

Fact("Compositor Swipe To A Page With Another Layout Fills Uncovered Areas")
{
    World w(3, 1, true);                // page 0: 1U, page 1: 2U, page 2: sparse
    Boot(w, 0);
    Assert(w.comp.TotalErases() == 0);

    w.scroll = 2 * W;                   // jump to the sparse page: its holes held 1U keys
    w.RunFrames(16);
    w.CheckSettledOn(2);
    Assert(w.comp.TotalErases() > 0);

    w.scroll = W;                       // and the 2U page, which covers the grid
    w.RunFrames(16);
    w.CheckSettledOn(1);
}

Fact("Compositor Fills Uncovered Areas Only When The Layout Changes")
{
    World w(1, 1, true);
    w.ChangeLayout(0, LAYOUT_SPARSE, COMP_FLUSH);
    Boot(w, 0);
    uint32_t erases = w.comp.TotalErases();
    Assert(erases == 0);                // the buffers start out background

    // Key changes in the same layout: no filling
    for (int i = 0; i < 4; i++) {
        w.ChangeKeys(0, ALL_KEYS, i & 1 ? COMP_FLUSH : 0);
        w.RunFrames(16);
        w.CheckSettledOn(0);
    }
    Assert(w.comp.TotalErases() == erases);

    // A new layout leaves the old rects to fill, in every buffer that drew them
    w.ChangeLayout(0, LAYOUT_1U, COMP_FLUSH);
    w.RunFrames(16);
    w.CheckSettledOn(0);
    w.ChangeLayout(0, LAYOUT_SPARSE, COMP_FLUSH);
    w.RunFrames(16);
    w.CheckSettledOn(0);
    Assert(w.comp.TotalErases() > erases);
    erases = w.comp.TotalErases();
    w.ChangeKeys(0, ALL_KEYS, 0);
    w.RunFrames(16);
    w.CheckSettledOn(0);
    Assert(w.comp.TotalErases() == erases);
}

Fact("Compositor Layout Change Flushed Shows In One Frame")
{
    World w(2, 1, true);
    Boot(w, 0);
    w.ChangeLayout(0, LAYOUT_2U, COMP_FLUSH);
    int frames = RunUntil(w, 20, [&] { return w.ShowsModel(0); });
    Assert(frames <= 8);
    Assert(w.partial_frames == 0);
    w.RunFrames(16);
    w.CheckSettledOn(0);
}

// ---------------------------------------------------------------------------
// Sweeps: dropping references to retired images (docs/image-assets.md)

Fact("Compositor Sweep Forgets Records Of Retired Images")
{
    World w(2);
    Boot(w, 0);                         // the front and an idle copy hold page 0's images
    w.ChangeKeys(0, 0x0007, 0);
    w.RunFrames(10);
    Assert(w.ShowsModel(0));

    // Keys 0-2's old images are now only in records (the old front, the copy)
    w.RetireUnused();
    Assert(w.RetiredPending() > 0);
    uint32_t gen = w.comp.Swept();
    w.RunFrames(2);                     // World checks nothing references them, then frees them
    Assert(w.comp.Swept() != gen);
    Assert(w.RetiredPending() == 0);
    Assert(w.images_freed > 0);

    // New images reuse the freed addresses: the forgotten records mustn't
    // match them
    w.ChangeKeys(0, 0x0007, 0);
    w.RunFrames(12);
    w.CheckSettledOn(0);
}

Fact("Compositor Sweep Drops A Job Whose Snapshot Uses A Retired Image")
{
    World w(2);
    Boot(w, 0);
    w.chunk_cost_us = 300;              // slow: the job takes a few frames
    w.ChangeKeys(0, ALL_KEYS, COMP_FLUSH);
    w.RunFrame();
    Assert(w.comp.JobPage() == 0);

    // The client changes them all again, so the snapshot's images are no
    // longer in the model, and retires them
    w.ChangeKeys(0, ALL_KEYS, COMP_FLUSH);
    w.RetireUnused();
    w.RunFrames(2);
    Assert(w.RetiredPending() == 0);    // swept and freed (checked unreferenced)

    w.chunk_cost_us = 65;
    w.RunFrames(16);
    w.CheckSettledOn(0);
}

Fact("Compositor Sweep Abandons A Key Being Drawn From A Retired Image")
{
    World w(1);
    Boot(w, 0);
    w.chunk_cost_us = 400;              // one chunk a gap: a key takes ~1.5 frames
    w.ChangeKeys(0, 1 << 4, COMP_FLUSH);
    w.RunFrame();
    const void *drawing = w.comp.DrawingImage();
    Assert(drawing != nullptr);

    // Replaced and retired mid-draw: the sweep stops the draw
    w.ChangeKeys(0, 1 << 4, COMP_FLUSH);
    w.RetireUnused();
    w.RunFrame();
    Assert(w.RetiredPending() == 0);    // World checked the drawing key isn't it
    Assert(w.comp.DrawingImage() != drawing);

    w.chunk_cost_us = 65;
    w.RunFrames(16);
    w.CheckSettledOn(0);
}

Fact("Compositor Sweep Of A Shown Key Redraws It")
{
    World w(1);
    Boot(w, 0);
    // Every gap reached too late for a chunk: the update can't be drawn
    // before the sweep
    w.gap_delay_us = CHUNK_US - COMP_GAP_MARGIN_US - COMP_CHUNK_WORST_US - w.fill_one_us + 1;
    w.ChangeKeys(0, 1 << 4, 0);
    w.RetireUnused();                   // key 4's old image is still on screen
    w.RunFrames(3);
    Assert(w.RetiredPending() == 0);

    w.gap_delay_us = 0;
    w.RunFrames(16);
    w.CheckSettledOn(0);
}

Fact("Compositor Keys With No Images Draw Background")
{
    World w(1);
    Boot(w, 0);
    for (int k = 0; k < w.model[0].count; k++)
        w.SetKey(0, k, nullptr);
    w.comp.PageChanged(0, COMP_FLUSH);
    w.RunFrames(10);
    w.CheckSettledOn(0);
    int b = w.ShownBuffer(0);
    for (int i = 0; i < W * SCREEN_PAGE_H; i++)
        Assert(w.comp.GetBuffer(b)[i] == COMP_BACKGROUND);
}

Fact("Compositor Image That Does Not Fit Its Rect Draws Background")
{
    // The client gives images the size of their rect; one that isn't (here
    // a 1U key's header claiming a missing row) is drawn as background, not
    // past the rect
    TestImages &ti = TestImages::Get();
    size_t size = ti.Size(ti.Key1U(0));
    uint8_t *bad = (uint8_t *)malloc(size);     // outlives the World
    memcpy(bad, ti.Key1U(0), size);
    ((IMAGE_RLE *)bad)->rowCount--;
    {
        World w(1);
        Boot(w, 0);
        w.SetKey(0, 2, bad);
        w.comp.PageChanged(0, COMP_FLUSH);
        w.RunFrames(10);
        w.CheckSettledOn(0);            // the oracle knows it too
        int b = w.ShownBuffer(0);
        Assert(w.comp.GetRecord(b, 2).img == bad);
        KeyPixelRect r = w.model[0].keys[2].rect;
        for (int y = r.y; y < r.y + r.h; y++)
            for (int x = r.x; x < r.x + r.w; x++)
                Assert(w.comp.GetBuffer(b)[y * W + x] == COMP_BACKGROUND);
    }
    free(bad);
}
