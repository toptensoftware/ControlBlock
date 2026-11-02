// Page compositor scenarios, one per behaviour in docs/page-compositor.md.
// World (harness.h) checks the invariants on every frame start as well.

#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"

using namespace SimpleLib;

#define W SCREEN_PAGE_W
#define ALL_KEYS 0x7FFF

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
        w.RunFrames(10);
        w.CheckSettledOn(page);
    }
}

Fact("Compositor Key Grid Has Equal Gaps And Is Centred")
{
    // Same pitch across and down
    int pitch_x = COMP_KEY_X(1) - COMP_KEY_X(0);
    int pitch_y = COMP_KEY_Y(COMP_COLS) - COMP_KEY_Y(0);
    Assert(pitch_x == pitch_y);
    Assert(pitch_x >= COMP_KEY_SIZE);

    // Centred in the page area, so the margins at the left and right (the
    // gap between pages) are equal, and likewise top and bottom
    int last = COMP_KEYS - 1;
    Assert(COMP_KEY_X(0) == SCREEN_PAGE_W - (COMP_KEY_X(COMP_COLS - 1) + COMP_KEY_SIZE));
    Assert(COMP_KEY_Y(0) == SCREEN_PAGE_H - (COMP_KEY_Y(last) + COMP_KEY_SIZE));

    // The gap fills the page area's height: about half a gap above and below
    // (within rounding), so the space to the bars matches the space between
    // keys, and it's the largest gap that does
    int gap = pitch_y - COMP_KEY_SIZE;
    Assert(COMP_KEY_Y(0) * 2 >= gap);
    Assert(COMP_KEY_Y(0) * 2 <= gap + COMP_ROWS);
    Assert(COMP_ROWS * COMP_KEY_SIZE + COMP_ROWS * (gap + 1) > SCREEN_PAGE_H);

    // Every cell on the grid, inside the page
    for (int k = 0; k < COMP_KEYS; k++) {
        Assert(COMP_KEY_X(k) == COMP_KEY_X(0) + (k % COMP_COLS) * pitch_x);
        Assert(COMP_KEY_Y(k) == COMP_KEY_Y(0) + (k / COMP_COLS) * pitch_y);
        Assert(COMP_KEY_X(k) >= 0 && COMP_KEY_X(k) + COMP_KEY_SIZE <= SCREEN_PAGE_W);
        Assert(COMP_KEY_Y(k) >= 0 && COMP_KEY_Y(k) + COMP_KEY_SIZE <= SCREEN_PAGE_H);
    }
}

Fact("Compositor Boot Shows Background Then First Page")
{
    World w(3);
    w.frame_start += FRAME_US;
    w.FrameStart();
    Assert(w.view_count == 0);          // nothing drawn yet: background
    Assert(w.view.px[0] == nullptr && w.view.px[1] == nullptr);

    // A full page: each key rendered, then copied in 32-row slices after
    // fills, about 5 fills a key: ~2.5 frames
    int frames = RunUntil(w, 6, [&] { return w.ShowsModel(0); });
    Assert(frames <= 4);
    w.CheckSettledOn(0);
}

Fact("Compositor Writes Page Buffers Only In Fill Gaps")
{
    World w(3);
    w.check_gap_writes = true;
    Boot(w, 0);                         // one page in view: 32-row slices
    w.scroll = 300;                     // page 1 built during the hold
    w.RunFrames(6);
    Assert(w.view_count == 2);
    w.ChangeKeys(0, ALL_KEYS, 0);       // two pages in view: 16-row slices
    w.ChangeKeys(1, 0x0F0F, COMP_FLUSH);
    RunUntil(w, 20, [&] { return w.ShowsModel(0) && w.ShowsModel(1); });
    Assert(w.ShowsModel(0) && w.ShowsModel(1));
}

Fact("Compositor Idle Work Makes A Back Copy Of The Front")
{
    World w(3);
    Boot(w, 0);
    int front = w.ShownBuffer(0);
    int copies = 0;
    for (int b = 0; b < COMP_BUFFERS; b++) {
        if (b == front)
            continue;
        bool same = true;
        for (int k = 0; k < COMP_KEYS; k++)
            same &= w.comp.GetRecord(b, k).bg == w.model[0][k].bg && w.comp.GetRecord(b, k).fg == w.model[0][k].fg;
        copies += same;
    }
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

    int frames = RunUntil(w, 6, [&] { return w.view_count == 2; });
    Assert(frames <= 4);                // a full page, ~2.5 frames, after the slop
    Assert(w.view.offset == 40);        // then it jumps to the finger
    Assert(w.ShowsModel(0) && w.ShowsModel(1));
}

Fact("Compositor Swipe Back To The Page Just Left Needs No Drawing")
{
    World w(3);
    Boot(w, 0);
    w.scroll = 400;
    w.RunFrames(4);
    w.scroll = W;                       // settle on page 1
    w.RunFrames(10);
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
    w.ChangeKeys(0, ALL_KEYS, 0);       // ~6 keys a frame, each drawn twice
    int frames = RunUntil(w, 20, [&] { return w.ShowsModel(0); });
    Assert(frames <= 10);
    Assert(w.partial_frames > 0);       // some keys showed before others
}

Fact("Compositor Flush Shows Every Key In One Frame")
{
    World w(3);
    Boot(w, 0);
    w.ChangeKeys(0, ALL_KEYS, COMP_FLUSH);
    int frames = RunUntil(w, 20, [&] { return w.ShowsModel(0); });
    Assert(frames <= 5);
    Assert(w.partial_frames == 0);      // World also checks this every frame
}

Fact("Compositor Swiping Away Drops The Job And Keeps Its Work")
{
    World w(3);
    Boot(w, 0);
    w.key_cost_us = 5000;               // slow: page 1 takes several frames

    w.scroll = 40;
    w.RunFrame();
    w.RunFrame();
    Assert(w.comp.JobPage() == 1);
    uint32_t drawn_first = w.comp.TotalKeys();

    // Back to page 0 before page 1 is ready
    w.scroll = 0;
    w.RunFrames(3);
    Assert(w.comp.JobPage() != 1);
    Assert(w.AtRestOn(0));

    // Coming back finishes page 1 from where it got to
    w.scroll = 40;
    uint32_t before = w.comp.TotalKeys();
    RunUntil(w, 10, [&] { return w.view_count == 2; });
    Assert(w.ShowsModel(1));
    Assert(w.comp.TotalKeys() - before < 15);
    (void)drawn_first;
}

Fact("Compositor Direction Reversal Holds The Page")
{
    World w(3);
    Boot(w, 1);
    w.scroll = W + 40;                  // towards page 2
    w.RunFrames(6);
    Assert(w.view_count == 2);

    w.scroll = W - 40;                  // back past page 1 towards page 0
    w.RunFrame();
    Assert(w.AtRestOn(1));              // held at page 1 until page 0 is ready
    RunUntil(w, 6, [&] { return w.view_count == 2; });
    Assert(w.ShowsModel(0) && w.ShowsModel(1));
}

Fact("Compositor Updates Both Pages Mid Swipe")
{
    World w(3);
    Boot(w, 0);
    w.scroll = 300;
    w.RunFrames(4);
    w.ChangeKeys(0, 0x00F0, 0);
    w.ChangeKeys(1, 0x0F00, COMP_FLUSH);
    // Slow but allowed: with two pages in view slices are 16 rows (~10 fills
    // a key), and page 1's only free buffer holds page 0, so all 15 of its
    // keys are redrawn. Measured: 9 frames.
    int frames = RunUntil(w, 20, [&] { return w.ShowsModel(0) && w.ShowsModel(1); });
    Assert(frames <= 12);
}

Fact("Compositor Reload Keeps The Old Layout Until The New Page Is Complete")
{
    World w(3);
    Boot(w, 1);
    w.key_cost_us = 3000;

    w.Reload();
    w.scroll = 0;                       // the client resets to the start page
    bool seen_new = false;
    for (int i = 0; i < 20 && !seen_new; i++) {
        w.RunFrame();
        Assert(w.view_count > 0);       // never a blank frame
        seen_new = w.ShowsModel(0);
        if (!seen_new) {
            Assert(w.views[0].page == COMP_NO_PAGE);   // the old layout, at rest
            Assert(w.view.offset == 0);
        }
    }
    Assert(seen_new);
    w.RunFrames(5);
    w.CheckSettledOn(0);
}

Fact("Compositor Change During A Build Is Picked Up Next")
{
    World w(3);
    Boot(w, 0);
    w.key_cost_us = 4000;
    w.scroll = 40;
    w.RunFrame();                       // building page 1
    w.ChangeKeys(1, 0x0003, 0);         // after its snapshot
    w.RunFrames(20);
    Assert(w.ShowsModel(1));
}

// ---------------------------------------------------------------------------
// Sweeps: dropping references to retired images (docs/image-assets.md)

Fact("Compositor Sweep Forgets Records Of Retired Images")
{
    World w(2);
    Boot(w, 0);                         // the front and an idle copy hold page 0's images
    w.ChangeKeys(0, 0x0007, 0);
    w.RunFrames(6);
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
    w.RunFrames(8);
    w.CheckSettledOn(0);
}

Fact("Compositor Sweep Drops A Job Whose Snapshot Uses A Retired Image")
{
    World w(2);
    Boot(w, 0);
    w.key_cost_us = 4000;               // slow: the job takes a few frames
    w.ChangeKeys(0, ALL_KEYS, COMP_FLUSH);
    w.RunFrame();
    Assert(w.comp.JobPage() == 0);

    // The client changes them all again, so the snapshot's images are no
    // longer in the model, and retires them
    w.ChangeKeys(0, ALL_KEYS, COMP_FLUSH);
    w.RetireUnused();
    w.RunFrames(2);
    Assert(w.RetiredPending() == 0);    // swept and freed (checked unreferenced)

    w.key_cost_us = 600;
    w.RunFrames(10);
    w.CheckSettledOn(0);
}

Fact("Compositor Sweep Of A Shown Key Redraws It")
{
    World w(1);
    Boot(w, 0);
    w.key_cost_us = 20000;              // the update can't start before the sweep
    w.ChangeKeys(0, 1 << 4, 0);
    w.RetireUnused();                   // key 4's old images are still on screen
    w.RunFrames(3);
    Assert(w.RetiredPending() == 0);

    w.key_cost_us = 600;
    w.RunFrames(10);
    w.CheckSettledOn(0);
}

Fact("Compositor Keys With No Images Draw Background")
{
    World w(1);
    Boot(w, 0);
    for (int k = 0; k < COMP_KEYS; k++)
        w.SetKey(0, k, nullptr, nullptr);
    w.comp.PageChanged(0, COMP_FLUSH);
    w.RunFrames(4);
    int b = w.ShownBuffer(0);
    for (int k = 0; k < COMP_KEYS; k++) {
        const uint16_t *cell = w.comp.GetBuffer(b) + COMP_KEY_Y(k) * W + COMP_KEY_X(k);
        for (int y = 0; y < COMP_KEY_SIZE; y++)
            for (int x = 0; x < COMP_KEY_SIZE; x++)
                Assert(cell[y * W + x] == COMP_BACKGROUND);
    }
}

Fact("Compositor Image Is Opaque Only For Faces That Cover The Cell")
{
    TestImages &ti = TestImages::Get();
    for (int f = 0; f < TEST_KEYS_FACES; f++) {
        Assert(comp_image_is_opaque(ti.Face(f, true), ti.Size(ti.Face(f, true))));
        Assert(!comp_image_is_opaque(ti.Face(f, false), ti.Size(ti.Face(f, false))));
    }
    Assert(!comp_image_is_opaque(ti.Icon(0), ti.Size(ti.Icon(0))));

    // Truncated or garbage data isn't opaque (or a crash)
    Assert(!comp_image_is_opaque(ti.Face(0, true), 16));
    static const uint32_t junk[64] = { 0x12345678 };
    Assert(!comp_image_is_opaque(junk, sizeof(junk)));
}

Fact("Compositor Opaque Face Skips The Clear And Draws Correctly")
{
    TestImages &ti = TestImages::Get();
    World w(1);
    Boot(w, 0);

    // Transparent face -> opaque face (no clear) -> transparent again (must
    // clear all 128 rows the opaque face covered) -> no images at all
    for (int round = 0; round < 3; round++) {
        for (int k = 0; k < COMP_KEYS; k++) {
            if (round == 0)
                w.SetKey(0, k, ti.Face(k % 4, true), ti.Icon(k % ti.IconCount()), COMP_KEY_BG_OPAQUE);
            else if (round == 1)
                w.SetKey(0, k, ti.Face(k % 4, false), (k & 1) ? ti.Icon(k % ti.IconCount()) : nullptr);
            else
                w.SetKey(0, k, (k & 1) ? nullptr : ti.Face(k % 4, true), ti.Icon(k % ti.IconCount()),
                         (k & 1) ? 0 : COMP_KEY_BG_OPAQUE);
        }
        w.comp.PageChanged(0, COMP_FLUSH);
        w.RunFrames(6);
        w.CheckSettledOn(0);
    }
}
