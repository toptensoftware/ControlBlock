// The touch router (docs/interactions.md): swipe window, then PRESS /
// RELEASE / CANCEL, with gaps, key edges and busy pages swipe-only.

#include <stdio.h>
#include <stdlib.h>
#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"            // Rng, Check
#include "touch_router.h"

using namespace SimpleLib;

namespace
{
    const int WIN = TOUCH_SWIPE_WINDOW_MS;
    const int64_t MS = 1000;

    struct Ev
    {
        int      page, key, action;
        uint32_t ms;
    };

    // A router with a fake host: records key events, any key can be made inactive
    struct Rig
    {
        TouchRouter router;
        Ev          ev[256];
        int         count = 0;
        bool        inactive[SCREEN_MAX_PAGES][COMP_KEYS] = {};
        int64_t     now = 1000 * MS;
        int         page = 0;
        bool        look = false;           // a key shows its pressed look
        int         look_page = -1, look_key = -1;
        int         look_changes = 0;
        bool        look_bad = false;       // a look set on when on, or cleared when off, or for another key

        Rig()
        {
            TouchRouterHost h = {};
            h.key_look = [](int page, int key, bool pressed, void *ctx) {
                Rig *r = (Rig *)ctx;
                if (pressed == r->look)
                    r->look_bad = true;
                if (!pressed && (page != r->look_page || key != r->look_key))
                    r->look_bad = true;
                r->look = pressed;
                r->look_page = page;
                r->look_key = key;
                r->look_changes++;
            };
            h.key_active = [](int page, int key, void *ctx) { return !((Rig *)ctx)->inactive[page][key]; };
            h.key_event  = [](int page, int key, int action, uint32_t ms, void *ctx) {
                Rig *r = (Rig *)ctx;
                if (r->count < 256)
                    r->ev[r->count++] = Ev{ page, key, action, ms };
            };
            h.ctx = this;
            router.Init(h);
        }

        void Advance(int ms)
        {
            // Frames at 16 ms, like the swipe task
            int64_t end = now + ms * MS;
            while (now < end) {
                now = now + 16 * MS < end ? now + 16 * MS : end;
                router.Tick(now, page);
            }
        }

        void Down(int x, int y, bool busy = false) { router.Down(x, y, now, page, busy); }
        bool Move(int x, int y) { return router.Move(x, y, now); }
        void Up(int x, int y) { router.Up(x, y, now); }

        bool Is(int i, int key, int action) const
        {
            return i < count && ev[i].key == key && ev[i].action == action;
        }
    };

    // Centre of a key, in screen coordinates
    int CX(int key) { return COMP_KEY_X(key) + COMP_KEY_SIZE / 2; }
    int CY(int key) { return SCREEN_PAGE_TOP + COMP_KEY_Y(key) + COMP_KEY_SIZE / 2; }
}

// --- Hit testing ---

Fact("TouchRouter Hit Test Finds The Key Under A Point")
{
    for (int k = 0; k < COMP_KEYS; k++)
        Assert(touch_key_at(CX(k), CY(k), TOUCH_EDGE_AMBIGUOUS_PX) == k);
}

Fact("TouchRouter Hit Test Edge Zone")
{
    int m = TOUCH_EDGE_AMBIGUOUS_PX;
    int left = COMP_KEY_X(0), top = SCREEN_PAGE_TOP + COMP_KEY_Y(0);
    int right = left + COMP_KEY_SIZE, bottom = top + COMP_KEY_SIZE;
    int cy = CY(0), cx = CX(0);

    // Just inside the safe area on every side
    Assert(touch_key_at(left + m, cy, m) == 0);
    Assert(touch_key_at(right - m - 1, cy, m) == 0);
    Assert(touch_key_at(cx, top + m, m) == 0);
    Assert(touch_key_at(cx, bottom - m - 1, m) == 0);

    // Just inside the edge zone, on the edge, and outside the cell
    Assert(touch_key_at(left + m - 1, cy, m) == -1);
    Assert(touch_key_at(right - m, cy, m) == -1);
    Assert(touch_key_at(cx, top + m - 1, m) == -1);
    Assert(touch_key_at(cx, bottom - m, m) == -1);
    Assert(touch_key_at(left - 1, cy, m) == -1);
}

Fact("TouchRouter Hit Test Gaps And Status Bar")
{
    // Between keys 0 and 1
    int gap_x = COMP_KEY_X(0) + COMP_KEY_SIZE + COMP_KEY_GAP / 2;
    Assert(touch_key_at(gap_x, CY(0), 0) == -1);
    Assert(touch_key_at(CX(0), SCREEN_STATUS_H / 2, 0) == -1);
    Assert(touch_key_at(CX(0), SCREEN_H - 1, 0) == -1);
}

Fact("TouchRouter Hit Test Negative Margin Grows The Cell")
{
    int left = COMP_KEY_X(0);
    Assert(touch_key_at(left - 3, CY(0), 0) == -1);
    Assert(touch_key_at(left - 3, CY(0), -4) == 0);
}

// --- Taps and holds ---

Fact("TouchRouter Quick Tap Sends Press Then Release At Lift")
{
    Rig r;
    r.Down(CX(7), CY(7));
    r.Advance(WIN / 2);
    Assert(r.count == 0);
    r.Up(CX(7), CY(7));
    Assert(r.count == 2);
    Assert(r.Is(0, 7, TOUCH_KEY_PRESS));
    Assert(r.Is(1, 7, TOUCH_KEY_RELEASE));
    Assert(r.ev[0].page == 0);
}

Fact("TouchRouter Press Fires When The Window Ends")
{
    Rig r;
    r.Down(CX(3), CY(3));
    r.Advance(WIN - 20);
    Assert(r.count == 0);
    r.Advance(40);
    Assert(r.count == 1);
    Assert(r.Is(0, 3, TOUCH_KEY_PRESS));

    r.Advance(2000);
    Assert(r.count == 1);       // a long press adds nothing
    r.Up(CX(3), CY(3));
    Assert(r.count == 2);
    Assert(r.Is(1, 3, TOUCH_KEY_RELEASE));
}

Fact("TouchRouter Event Times Are Device Milliseconds")
{
    Rig r;
    r.Down(CX(0), CY(0));
    r.Advance(500);
    r.Up(CX(0), CY(0));
    Assert(r.count == 2);
    Assert(r.ev[0].ms >= (uint32_t)(r.now / MS) - 500 + WIN - 16);
    Assert(r.ev[1].ms == (uint32_t)(r.now / MS));
}

Fact("TouchRouter Wobble Inside The Slop Still Presses")
{
    Rig r;
    r.Down(CX(1), CY(1));
    Assert(!r.Move(CX(1) + TOUCH_DRAG_SLOP_PX, CY(1) + 5));
    Assert(!r.Move(CX(1) - TOUCH_DRAG_SLOP_PX, CY(1)));
    r.Advance(WIN + 20);
    Assert(r.Is(0, 1, TOUCH_KEY_PRESS));
    r.Up(CX(1), CY(1));
    Assert(r.Is(1, 1, TOUCH_KEY_RELEASE));
}

Fact("TouchRouter Pages Are Reported With The Key")
{
    Rig r;
    r.page = 5;
    r.Down(CX(2), CY(2));
    r.Up(CX(2), CY(2));
    Assert(r.count == 2 && r.ev[0].page == 5 && r.ev[1].page == 5);
}

// --- Pressed look: shown from touch-down, reverted if it turns into a swipe ---

Fact("TouchRouter Look Shows At Touch Down")
{
    Rig r;
    r.page = 2;
    r.Down(CX(7), CY(7));
    Assert(r.look && r.look_page == 2 && r.look_key == 7);
    Assert(r.count == 0);               // no events yet
    r.Advance(WIN + 20);
    Assert(r.look);                     // stays through the PRESS
    r.Up(CX(7), CY(7));
    Assert(!r.look && !r.look_bad);
}

Fact("TouchRouter Look Of A Quick Tap Ends At Lift")
{
    Rig r;
    r.Down(CX(7), CY(7));
    r.Advance(WIN / 2);
    Assert(r.look);
    r.Up(CX(7), CY(7));
    Assert(!r.look && !r.look_bad);
}

Fact("TouchRouter Look Reverts When A Swipe Starts")
{
    Rig r;
    r.Down(CX(4), CY(4));
    Assert(r.look);
    Assert(!r.Move(CX(4) + TOUCH_DRAG_SLOP_PX, CY(4)));
    Assert(r.look);
    Assert(r.Move(CX(4) + TOUCH_DRAG_SLOP_PX + 1, CY(4)));
    Assert(!r.look && !r.look_bad);
    r.Up(CX(4) + 100, CY(4));
    Assert(r.look_changes == 2);        // on, off: nothing more
}

Fact("TouchRouter Look Reverts When The Finger Leaves The Key Early")
{
    Rig r;
    int top = SCREEN_PAGE_TOP + COMP_KEY_Y(6);
    r.Down(CX(6), top + TOUCH_EDGE_AMBIGUOUS_PX);
    Assert(r.look);
    r.Move(CX(6), top - 3);
    Assert(!r.look && !r.look_bad);
}

Fact("TouchRouter Look Reverts On Cancel")
{
    Rig r;
    r.Down(CX(6), CY(6));
    r.Advance(WIN + 20);
    Assert(r.look);
    r.Move(COMP_KEY_X(6) + COMP_KEY_SIZE + 5, CY(6));
    Assert(!r.look && !r.look_bad);
    r.Move(CX(6), CY(6));               // back over the key: stays off
    Assert(!r.look);
}

Fact("TouchRouter Look Reverts When The Held Key Goes")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.Advance(WIN + 20);
    r.inactive[0][5] = true;
    r.Advance(32);
    Assert(!r.look && !r.look_bad);
}

Fact("TouchRouter Look Reverts When The Key Goes Before The Press")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.inactive[0][5] = true;
    r.Advance(WIN + 20);
    Assert(!r.look && !r.look_bad);
}

Fact("TouchRouter No Look For Touches That Can Only Swipe")
{
    Rig r;
    int gap_x = COMP_KEY_X(0) + COMP_KEY_SIZE + COMP_KEY_GAP / 2;
    r.Down(gap_x, CY(0));
    r.Up(gap_x, CY(0));
    r.Down(CX(0), SCREEN_STATUS_H / 2);
    r.Up(CX(0), SCREEN_STATUS_H / 2);
    r.Down(COMP_KEY_X(2) + TOUCH_EDGE_AMBIGUOUS_PX - 1, CY(2));
    r.Up(0, 0);
    r.inactive[0][8] = true;
    r.Down(CX(8), CY(8));
    r.Up(CX(8), CY(8));
    r.Down(CX(9), CY(9), true);
    r.Up(CX(9), CY(9));
    Assert(r.look_changes == 0);
}

Fact("TouchRouter Lost Up Clears The Old Look Before The Next")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.Down(CX(9), CY(9));               // no UP in between, still pending
    Assert(r.look && r.look_key == 9 && !r.look_bad);
    r.Advance(WIN + 20);
    r.Down(CX(1), CY(1));               // and again, this time held
    Assert(r.look && r.look_key == 1 && !r.look_bad);
}

// --- Swipes ---

Fact("TouchRouter Swipe Inside The Window Sends No Key Events")
{
    Rig r;
    r.Down(CX(4), CY(4));
    r.Advance(30);
    Assert(!r.Move(CX(4) + TOUCH_DRAG_SLOP_PX, CY(4)));
    Assert(r.Move(CX(4) + TOUCH_DRAG_SLOP_PX + 1, CY(4)));    // starts exactly once
    Assert(!r.Move(CX(4) + 80, CY(4)));
    r.Advance(500);
    r.Up(CX(4) + 200, CY(4));
    Assert(r.count == 0);
}

Fact("TouchRouter Swipe Leftwards Too")
{
    Rig r;
    r.Down(CX(4), CY(4));
    Assert(r.Move(CX(4) - TOUCH_DRAG_SLOP_PX - 1, CY(4)));
}

Fact("TouchRouter Held Key Locks Out Swiping")
{
    Rig r;
    r.Down(CX(6), CY(6));
    r.Advance(WIN + 20);
    Assert(r.Is(0, 6, TOUCH_KEY_PRESS));

    // Slide far along the key's row inside the cell: never a swipe
    Assert(!r.Move(CX(6) + 40, CY(6)));
    Assert(!r.Move(CX(6) - 40, CY(6)));
    r.Advance(1000);
    Assert(!r.Move(CX(6) + 60, CY(6)));
    r.Up(CX(6) + 60, CY(6));
    Assert(r.count == 2 && r.Is(1, 6, TOUCH_KEY_RELEASE));
}

Fact("TouchRouter Sliding Out After The Press Cancels")
{
    Rig r;
    r.Down(CX(6), CY(6));
    r.Advance(WIN + 20);

    Assert(!r.Move(COMP_KEY_X(6) + COMP_KEY_SIZE + 5, CY(6)));
    Assert(r.count == 2 && r.Is(1, 6, TOUCH_KEY_CANCEL));

    // Dead until lift: back over the key does nothing, far away is no swipe
    Assert(!r.Move(CX(6), CY(6)));
    Assert(!r.Move(CX(6) + 300, CY(6)));
    r.Up(CX(6) + 300, CY(6));
    Assert(r.count == 2);
}

Fact("TouchRouter Slide Out Vertically Cancels Too")
{
    Rig r;
    r.Down(CX(6), CY(6));
    r.Advance(WIN + 20);
    r.Move(CX(6), SCREEN_PAGE_TOP + COMP_KEY_Y(6) - 1);
    Assert(r.count == 2 && r.Is(1, 6, TOUCH_KEY_CANCEL));
}

Fact("TouchRouter Slide Out And Back Does Not Press Again")
{
    Rig r;
    r.Down(CX(6), CY(6));
    r.Advance(WIN + 20);
    r.Move(CX(6), 0);
    r.Move(CX(6), CY(6));
    r.Advance(WIN * 3);
    r.Up(CX(6), CY(6));
    Assert(r.count == 2);       // one press, one cancel
}

Fact("TouchRouter Exit Slop Is Added To The Cell")
{
    Rig r;
    r.Down(CX(6), CY(6));
    r.Advance(WIN + 20);
    r.Move(COMP_KEY_X(6) + COMP_KEY_SIZE + TOUCH_KEY_EXIT_SLOP_PX - 1, CY(6));
    Assert(r.count == 1);
    r.Move(COMP_KEY_X(6) + COMP_KEY_SIZE + TOUCH_KEY_EXIT_SLOP_PX, CY(6));
    Assert(r.count == 2 && r.Is(1, 6, TOUCH_KEY_CANCEL));
}

Fact("TouchRouter Leaving The Key Early Is Swipe Only")
{
    Rig r;
    int top = SCREEN_PAGE_TOP + COMP_KEY_Y(6);
    r.Down(CX(6), top + TOUCH_EDGE_AMBIGUOUS_PX);
    r.Move(CX(6), top - 3);             // out of the cell, inside the window
    r.Advance(WIN * 2);
    r.Up(CX(6), top - 3);
    Assert(r.count == 0);
}

Fact("TouchRouter Leaving The Key Early Can Still Swipe")
{
    Rig r;
    int top = SCREEN_PAGE_TOP + COMP_KEY_Y(6);
    r.Down(CX(6), top + TOUCH_EDGE_AMBIGUOUS_PX);
    r.Move(CX(6), top - 3);
    r.Advance(WIN * 2);
    Assert(r.Move(CX(6) + TOUCH_DRAG_SLOP_PX + 1, top - 3));
}

// --- Touches that can only swipe ---

Fact("TouchRouter Gap Touch Only Swipes")
{
    Rig r;
    int gap_x = COMP_KEY_X(0) + COMP_KEY_SIZE + COMP_KEY_GAP / 2;
    r.Down(gap_x, CY(0));
    r.Advance(WIN * 3);
    Assert(r.Move(gap_x + TOUCH_DRAG_SLOP_PX + 1, CY(0)));      // still free to swipe after the window
    r.Up(gap_x + 50, CY(0));
    Assert(r.count == 0);
}

Fact("TouchRouter Gap Tap Does Nothing")
{
    Rig r;
    int gap_x = COMP_KEY_X(0) + COMP_KEY_SIZE + COMP_KEY_GAP / 2;
    r.Down(gap_x, CY(0));
    r.Up(gap_x, CY(0));
    Assert(r.count == 0);
}

Fact("TouchRouter Status Bar Touch Only Swipes")
{
    Rig r;
    r.Down(CX(0), SCREEN_STATUS_H / 2);
    r.Advance(WIN * 3);
    r.Up(CX(0), SCREEN_STATUS_H / 2);
    Assert(r.count == 0);
}

Fact("TouchRouter Edge Zone Touch Only Swipes")
{
    Rig r;
    int x = COMP_KEY_X(2) + TOUCH_EDGE_AMBIGUOUS_PX - 1;
    r.Down(x, CY(2));
    r.Advance(WIN * 3);
    Assert(r.Move(x + TOUCH_DRAG_SLOP_PX + 1, CY(2)));
    r.Up(x + 40, CY(2));
    Assert(r.count == 0);

    // One pixel further in presses
    r.Down(x + 1, CY(2));
    r.Advance(WIN + 20);
    Assert(r.Is(0, 2, TOUCH_KEY_PRESS));
}

Fact("TouchRouter Inactive Key Only Swipes")
{
    Rig r;
    r.inactive[0][8] = true;
    r.Down(CX(8), CY(8));
    r.Advance(WIN * 3);
    r.Up(CX(8), CY(8));
    Assert(r.count == 0);

    // A quick tap too
    r.Down(CX(8), CY(8));
    r.Up(CX(8), CY(8));
    Assert(r.count == 0);
}

Fact("TouchRouter Busy Page Only Swipes")
{
    Rig r;
    r.Down(CX(8), CY(8), true);         // a glide or drag is under way
    Assert(!r.Move(CX(8) + 100, CY(8)));        // the swipe already owns it
    r.Advance(WIN * 3);
    r.Up(CX(8), CY(8));
    Assert(r.count == 0);

    r.Down(CX(8), CY(8), true);
    r.Advance(WIN * 3);
    r.Up(CX(8), CY(8));
    Assert(r.count == 0);
}

Fact("TouchRouter Busy Page Quick Tap Does Nothing")
{
    Rig r;
    r.Down(CX(8), CY(8), true);
    r.Up(CX(8), CY(8));
    Assert(r.count == 0);
}

// --- Changes under a finger ---

Fact("TouchRouter Key Disabled While Held Cancels")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.Advance(WIN + 20);
    r.inactive[0][5] = true;
    r.Advance(32);
    Assert(r.count == 2 && r.Is(1, 5, TOUCH_KEY_CANCEL));
    r.Up(CX(5), CY(5));
    Assert(r.count == 2);
}

Fact("TouchRouter Page Changed While Held Cancels")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.Advance(WIN + 20);
    r.page = 1;
    r.Advance(32);
    Assert(r.count == 2 && r.Is(1, 5, TOUCH_KEY_CANCEL) && r.ev[1].page == 0);
}

Fact("TouchRouter Key Disabled Before The Press Falls Back To Swiping")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.inactive[0][5] = true;
    r.Advance(WIN * 3);
    Assert(r.count == 0);
    Assert(r.Move(CX(5) + TOUCH_DRAG_SLOP_PX + 1, CY(5)));
}

Fact("TouchRouter Key Disabled Before A Quick Tap Lifts Sends Nothing")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.inactive[0][5] = true;
    r.Up(CX(5), CY(5));
    Assert(r.count == 0);
}

Fact("TouchRouter Lost Up Cancels A Held Key On The Next Down")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.Advance(WIN + 20);
    r.Down(CX(9), CY(9));               // no UP in between
    Assert(r.count == 2 && r.Is(1, 5, TOUCH_KEY_CANCEL));
    r.Advance(WIN + 20);
    Assert(r.count == 3 && r.Is(2, 9, TOUCH_KEY_PRESS));
}

Fact("TouchRouter Stray Events Without A Down Do Nothing")
{
    Rig r;
    Assert(!r.Move(CX(0), CY(0)));
    r.Up(CX(0), CY(0));
    r.Advance(500);
    Assert(r.count == 0);
}

Fact("TouchRouter Next Touch Is Independent")
{
    Rig r;
    r.Down(CX(0), CY(0));
    Assert(r.Move(CX(0) + 50, CY(0)));
    r.Up(CX(0) + 50, CY(0));

    r.Down(CX(1), CY(1));
    r.Advance(WIN + 20);
    r.Up(CX(1), CY(1));
    Assert(r.count == 2 && r.Is(0, 1, TOUCH_KEY_PRESS) && r.Is(1, 1, TOUCH_KEY_RELEASE));
}

// --- Fuzz ---

namespace
{
    // Random touches against random changes, checking after every step:
    //  - every PRESS is followed by exactly one RELEASE or CANCEL for the same key
    //  - no key event outside a press, and none for a touch that swiped
    //  - a swipe never starts after a PRESS, nor a PRESS after a swipe
    //  - an inactive key never gets a PRESS
    void FuzzTouches(uint32_t seed)
    {
        Rng rng(seed);
        Rig r;
        bool down = false;
        bool swiped = false;
        bool pressed = false;       // a PRESS sent for this touch, not yet closed
        int  checked = 0;
        int  page_count = 3;

        for (int step = 0; step < 4000; step++) {
            int dice = rng.Range(0, 99);
            if (dice < 12 && !down) {
                int x, y;
                if (rng.Chance(70)) {
                    int k = rng.Range(0, COMP_KEYS - 1);
                    x = COMP_KEY_X(k) + rng.Range(-4, COMP_KEY_SIZE + 4);
                    y = SCREEN_PAGE_TOP + COMP_KEY_Y(k) + rng.Range(-4, COMP_KEY_SIZE + 4);
                } else {
                    x = rng.Range(0, SCREEN_W - 1);
                    y = rng.Range(0, SCREEN_H - 1);
                }
                down = true;
                swiped = rng.Chance(8);
                r.Down(x, y, swiped);
            } else if (dice < 55 && down) {
                int mx = rng.Chance(30) ? rng.Range(0, SCREEN_W - 1) : rng.Range(0, 60);
                if (r.Move(mx, rng.Range(0, SCREEN_H - 1))) {
                    Check(!swiped);
                    Check(!pressed);
                    swiped = true;
                }
            } else if (dice < 70 && down) {
                r.Up(rng.Range(0, SCREEN_W - 1), rng.Range(0, SCREEN_H - 1));
                down = false;
                swiped = false;
            } else if (dice < 80) {
                r.inactive[r.page][rng.Range(0, COMP_KEYS - 1)] = rng.Chance(50);
            } else if (dice < 84) {
                r.page = rng.Range(0, page_count - 1);
            } else {
                r.Advance(rng.Range(1, 80));
            }

            for (; checked < r.count; checked++) {
                const Ev &e = r.ev[checked];
                if (e.action == TOUCH_KEY_PRESS) {
                    Check(!pressed);
                    Check(!swiped);
                    pressed = true;
                } else {
                    Check(pressed);
                    pressed = false;
                }
            }

            // The look is never wrong, never outlives the touch, is off
            // once the touch is a swipe, and is on while a press is open
            Check(!r.look_bad);
            if (!down || swiped)
                Check(!r.look);
            if (pressed)
                Check(r.look);
        }

        // End the touch: whatever is open closes
        if (down)
            r.Up(0, 0);
        r.Advance(WIN * 2);
        for (; checked < r.count; checked++) {
            if (r.ev[checked].action == TOUCH_KEY_PRESS) {
                Check(!pressed);
                pressed = true;
            } else {
                Check(pressed);
                pressed = false;
            }
        }
        Check(!pressed);
    }
}

Fact("TouchRouter Fuzz")
{
    const char *env = getenv("FUZZ_SEEDS");
    int count = env && atoi(env) > 0 ? atoi(env) : 20;
    for (uint32_t seed = 1; seed <= (uint32_t)count; seed++) {
        try {
            FuzzTouches(seed * 104729);
        } catch (const AssertionFailed &) {
            fprintf(stderr, "  (fuzz seed %u)\n", seed * 104729);
            throw;
        }
    }
}
