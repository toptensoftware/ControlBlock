// The touch router (docs/interactions.md): swipe window, then PRESS /
// RELEASE / CANCEL, with margins, key edges (seams), empty areas and busy
// pages swipe-only. Keys are the page's rects (any size, in 0.25U steps),
// reported by id.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"            // Rng, Check
#include "touch_gate.h"

using namespace SimpleLib;

namespace
{
    const int WIN = TOUCH_SWIPE_WINDOW_MS;
    const int64_t MS = 1000;
    const int M = TOUCH_EDGE_AMBIGUOUS_PX;
    const int MAX_ID = 1024;            // the rig's ids stay below this

    struct Ev
    {
        int      page;
        uint16_t key;
        int      action;
        uint32_t ms;
    };

    // A key's edges and centre in screen coordinates (the page at rest)
    int L(KeyRect r)  { return KeyRectPixels(r).x; }
    int T(KeyRect r)  { return SCREEN_PAGE_TOP + KeyRectPixels(r).y; }
    int R(KeyRect r)  { return L(r) + KeyRectPixels(r).w; }
    int B(KeyRect r)  { return T(r) + KeyRectPixels(r).h; }
    int CX(KeyRect r) { return (L(r) + R(r)) / 2; }
    int CY(KeyRect r) { return (T(r) + B(r)) / 2; }

    // The same for plain grid slot k, and its id on page 0 (or `page`)
    int L(int k)  { return L(KeySlotRect(k)); }
    int T(int k)  { return T(KeySlotRect(k)); }
    int R(int k)  { return R(KeySlotRect(k)); }
    int CX(int k) { return CX(KeySlotRect(k)); }
    int CY(int k) { return CY(KeySlotRect(k)); }
    uint16_t ID(int k, int page = 0) { return KeySlotId(page, k); }

    // Pages other than the plain grid (ids clear of the grid's)
    const KeyRect SPAN_2U = { 0, 0, 8, 4 };     // slots 0 and 1 as one key
    const KeyRect SPAN_1U = { 8, 0, 4, 4 };
    const KeyRect QUARTER_A = { 0, 4, 1, 4 };   // 0.25U x 1U, side by side
    const KeyRect QUARTER_B = { 1, 4, 1, 4 };
    const KeyRect GROUP_PLUS = { 0, 8, 4, 4 };  // a group: two abutting keys
    const KeyRect GROUP_MINUS = { 4, 8, 4, 4 };
    const KeyRect GROUP_UP = { 16, 0, 4, 2 };   // and two half-height ones, one above the other
    const KeyRect GROUP_DOWN = { 16, 2, 4, 2 };
    const KeyRect MIXED_RECTS[] = { SPAN_2U, SPAN_1U, QUARTER_A, QUARTER_B, GROUP_PLUS, GROUP_MINUS, GROUP_UP, GROUP_DOWN };
    const uint16_t MIXED_IDS[]  = { 500, 501, 510, 511, 520, 521, 522, 523 };
    const int MIXED_COUNT = 8;

    TouchKey Key(uint16_t id, KeyRect r) { return TouchKey{ id, KeyRectPixels(r) }; }

    // The plain grid's keys for page p (15 x 1U)
    int GridKeys(int p, TouchKey *out)
    {
        for (int i = 0; i < KEY_GRID_SLOTS; i++)
            out[i] = Key(KeySlotId(p, i), KeySlotRect(i));
        return KEY_GRID_SLOTS;
    }

    int MixedKeys(TouchKey *out)
    {
        for (int i = 0; i < MIXED_COUNT; i++)
            out[i] = Key(MIXED_IDS[i], MIXED_RECTS[i]);
        return MIXED_COUNT;
    }

    // A router behind its wake gate (enabled unless a test turns it off) with a
    // fake host: every page a plain grid unless a test sets it, records key
    // events and wakes, any key can be made inactive
    struct Rig
    {
        TouchGate   router;
        Ev          ev[256];
        int         count = 0;
        int         wakes = 0;
        uint32_t    wake_ms = 0;
        TouchKey    keys[SCREEN_MAX_PAGES][COMP_MAX_KEYS];
        int         key_count[SCREEN_MAX_PAGES];
        bool        inactive[MAX_ID] = {};
        int64_t     now = 1000 * MS;
        int         page = 0;
        bool        look = false;           // a key shows its pressed look
        int         look_page = -1;
        uint16_t    look_key = KEY_ID_NONE;
        int         look_changes = 0;
        bool        look_bad = false;       // a look set on when on, or cleared when off, or for another key

        Rig()
        {
            for (int p = 0; p < SCREEN_MAX_PAGES; p++)
                key_count[p] = GridKeys(p, keys[p]);

            TouchRouterHost h = {};
            h.page_keys = [](int page, TouchKey *out, void *ctx) {
                Rig *r = (Rig *)ctx;
                if (page < 0 || page >= SCREEN_MAX_PAGES)
                    return 0;
                memcpy(out, r->keys[page], sizeof(TouchKey) * r->key_count[page]);
                return r->key_count[page];
            };
            h.key_look = [](int page, uint16_t key, bool pressed, void *ctx) {
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
            h.key_active = [](int page, uint16_t key, void *ctx) {
                Rig *r = (Rig *)ctx;
                return r->Has(page, key) && !r->inactive[key];
            };
            h.key_event  = [](int page, uint16_t key, int action, uint32_t ms, void *ctx) {
                Rig *r = (Rig *)ctx;
                if (r->count < 256)
                    r->ev[r->count++] = Ev{ page, key, action, ms };
            };
            h.ctx = this;
            router.Init(h, [](uint32_t ms, void *ctx) { ((Rig *)ctx)->wakes++; ((Rig *)ctx)->wake_ms = ms; }, this);
        }

        // The key is on the page
        bool Has(int p, uint16_t id) const
        {
            for (int i = 0; i < key_count[p]; i++)
                if (keys[p][i].id == id)
                    return true;
            return false;
        }

        void SetMixed(int p) { key_count[p] = MixedKeys(keys[p]); }
        void SetEmpty(int p) { key_count[p] = 0; }

        void SetEnabled(bool on) { router.SetEnabled(on, now); }

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

        bool Is(int i, uint16_t key, int action) const
        {
            return i < count && ev[i].key == key && ev[i].action == action;
        }

        // A full tap held past the window: the key it pressed (0 if none)
        uint16_t Press(int x, int y)
        {
            int before = count;
            Down(x, y);
            Advance(WIN + 20);
            Up(x, y);
            return count == before + 2 && ev[before].action == TOUCH_KEY_PRESS ? ev[before].key : KEY_ID_NONE;
        }
    };

    int HitGrid(int x, int y, int margin)
    {
        TouchKey keys[COMP_MAX_KEYS];
        int n = GridKeys(0, keys);
        return touch_key_at(keys, n, x, y, margin);
    }

    int HitMixed(int x, int y, int margin)
    {
        TouchKey keys[COMP_MAX_KEYS];
        int n = MixedKeys(keys);
        return touch_key_at(keys, n, x, y, margin);
    }
}

// --- Hit testing ---

Fact("TouchRouter Hit Test Finds The Key Under A Point")
{
    for (int k = 0; k < KEY_GRID_SLOTS; k++)
        Assert(HitGrid(CX(k), CY(k), M) == k);
    for (int i = 0; i < MIXED_COUNT; i++)
        Assert(HitMixed(CX(MIXED_RECTS[i]), CY(MIXED_RECTS[i]), M) == i);
}

Fact("TouchRouter Hit Test Edge Zone")
{
    KeyRect k = KeySlotRect(0);
    int cy = CY(k), cx = CX(k);

    // Just inside the safe area on every side
    Assert(HitGrid(L(k) + M, cy, M) == 0);
    Assert(HitGrid(R(k) - M - 1, cy, M) == 0);
    Assert(HitGrid(cx, T(k) + M, M) == 0);
    Assert(HitGrid(cx, B(k) - M - 1, M) == 0);

    // Just inside the edge zone, on the edge, and outside the key
    Assert(HitGrid(L(k) + M - 1, cy, M) == -1);
    Assert(HitGrid(R(k) - M, cy, M) == -1);
    Assert(HitGrid(cx, T(k) + M - 1, M) == -1);
    Assert(HitGrid(cx, B(k) - M, M) == -1);
    Assert(HitGrid(L(k) - 1, cy, M) == -1);
}

Fact("TouchRouter Hit Test Margins And Status Bar")
{
    // The margins round the grid, the status bar and the screen's last row
    Assert(KEY_GRID_X0 > 0 && KEY_GRID_Y0 > 0);
    Assert(HitGrid(KEY_GRID_X0 / 2, CY(0), 0) == -1);
    Assert(HitGrid(SCREEN_W - KEY_GRID_X0 / 2, CY(4), 0) == -1);
    Assert(HitGrid(CX(0), SCREEN_PAGE_TOP + KEY_GRID_Y0 / 2, 0) == -1);
    Assert(HitGrid(CX(0), SCREEN_STATUS_H / 2, 0) == -1);
    Assert(HitGrid(CX(10), SCREEN_H - 1, 0) == -1);

    // The grid's own edges, with no margin
    Assert(HitGrid(L(0), T(0), 0) == 0);
    Assert(HitGrid(L(0) - 1, T(0), 0) == -1);
    Assert(HitGrid(L(0), T(0) - 1, 0) == -1);
}

Fact("TouchRouter Hit Test Seam Between Abutting Keys")
{
    // Keys 0 and 1 share an edge: no gap, a swipe-only strip of twice the zone
    int seam = R(0);
    Assert(seam == L(1));
    Assert(HitGrid(seam - 1, CY(0), 0) == 0);
    Assert(HitGrid(seam, CY(0), 0) == 1);
    for (int x = seam - M; x < seam + M; x++)
        Assert(HitGrid(x, CY(0), M) == -1);
    Assert(HitGrid(seam - M - 1, CY(0), M) == 0);
    Assert(HitGrid(seam + M, CY(0), M) == 1);

    // The same across rows (keys 0 and 5)
    int row = B(KeySlotRect(0));
    Assert(HitGrid(CX(0), row - M, M) == -1 && HitGrid(CX(0), row + M - 1, M) == -1);
    Assert(HitGrid(CX(0), row - M - 1, M) == 0 && HitGrid(CX(0), row + M, M) == 5);
}

Fact("TouchRouter Hit Test Negative Margin Grows The Key")
{
    Assert(HitGrid(L(0) - 3, CY(0), 0) == -1);
    Assert(HitGrid(L(0) - 3, CY(0), -4) == 0);
}

Fact("TouchRouter Hit Test Span Has No Inner Seam")
{
    // Where slots 0 and 1 meet is the middle of the 2U key
    int old_seam = R(0);
    Assert(HitMixed(old_seam, CY(SPAN_2U), M) == 0);
    Assert(HitMixed(old_seam - M, CY(SPAN_2U), M) == 0);
    Assert(HitMixed(L(SPAN_2U) + M, CY(SPAN_2U), M) == 0);
    Assert(HitMixed(R(SPAN_2U) - M - 1, CY(SPAN_2U), M) == 0);
    Assert(HitMixed(R(SPAN_2U) - M, CY(SPAN_2U), M) == -1);       // the seam with the 1U key next to it
    Assert(HitMixed(L(SPAN_1U) + M, CY(SPAN_1U), M) == 1);
}

Fact("TouchRouter Hit Test Quarter Unit Keys")
{
    // 36 px wide: 12 px of each side is the edge zone, 12 px in the middle press
    Assert(KeyRectPixels(QUARTER_A).w == KEY_QUARTER);
    int y = CY(QUARTER_A);
    Assert(HitMixed(L(QUARTER_A) + M - 1, y, M) == -1);
    for (int x = L(QUARTER_A) + M; x < R(QUARTER_A) - M; x++)
        Assert(HitMixed(x, y, M) == 2);
    Assert(HitMixed(R(QUARTER_A) - M, y, M) == -1);
    Assert(HitMixed(L(QUARTER_B) + M, y, M) == 3);
    Assert(HitMixed(R(QUARTER_B) - M - 1, y, M) == 3);

    // With no margin, every pixel is one key or the other
    for (int x = L(QUARTER_A); x < R(QUARTER_B); x++)
        Assert(HitMixed(x, y, 0) == (x < R(QUARTER_A) ? 2 : 3));
}

Fact("TouchRouter Hit Test Group Seams")
{
    // Side by side
    int seam = R(GROUP_PLUS);
    Assert(HitMixed(seam - M - 1, CY(GROUP_PLUS), M) == 4);
    Assert(HitMixed(seam - M, CY(GROUP_PLUS), M) == -1);
    Assert(HitMixed(seam + M - 1, CY(GROUP_PLUS), M) == -1);
    Assert(HitMixed(seam + M, CY(GROUP_PLUS), M) == 5);

    // One above the other (72 px tall each)
    seam = B(GROUP_UP);
    Assert(HitMixed(CX(GROUP_UP), seam - M - 1, M) == 6);
    Assert(HitMixed(CX(GROUP_UP), seam - M, M) == -1);
    Assert(HitMixed(CX(GROUP_UP), seam + M - 1, M) == -1);
    Assert(HitMixed(CX(GROUP_UP), seam + M, M) == 7);
}

Fact("TouchRouter Hit Test Empty Areas")
{
    // Slots the mixed page leaves empty, and an empty page
    Assert(HitMixed(CX(3), CY(3), 0) == -1);
    Assert(HitMixed(CX(7), CY(7), 0) == -1);
    Assert(HitMixed(CX(12), CY(12), 0) == -1);
    Assert(HitMixed(R(QUARTER_B) + 20, CY(QUARTER_B), 0) == -1);       // right of the quarter keys
    Assert(touch_key_at(nullptr, 0, CX(0), CY(0), 0) == -1);
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
    Assert(r.Is(0, ID(7), TOUCH_KEY_PRESS));
    Assert(r.Is(1, ID(7), TOUCH_KEY_RELEASE));
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
    Assert(r.Is(0, ID(3), TOUCH_KEY_PRESS));

    r.Advance(2000);
    Assert(r.count == 1);       // a long press adds nothing
    r.Up(CX(3), CY(3));
    Assert(r.count == 2);
    Assert(r.Is(1, ID(3), TOUCH_KEY_RELEASE));
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
    Assert(r.Is(0, ID(1), TOUCH_KEY_PRESS));
    r.Up(CX(1), CY(1));
    Assert(r.Is(1, ID(1), TOUCH_KEY_RELEASE));
}

Fact("TouchRouter Pages Are Reported With The Key")
{
    Rig r;
    r.page = 5;
    r.Down(CX(2), CY(2));
    r.Up(CX(2), CY(2));
    Assert(r.count == 2 && r.ev[0].page == 5 && r.ev[1].page == 5);
    Assert(r.ev[0].key == ID(2, 5));            // page 5's own key at that spot
}

// --- Keys of any size: the page's own rects ---

Fact("TouchRouter Hit Test Uses The Page Under The Finger")
{
    Rig r;
    r.SetMixed(1);
    Assert(r.Press(CX(1), CY(1)) == ID(1));     // page 0: the grid
    r.page = 1;
    Assert(r.Press(CX(1), CY(1)) == 500);       // page 1: slot 1 is the 2U key's right half
    Assert(r.Press(CX(3), CY(3)) == KEY_ID_NONE);       // ...and slot 3 is empty
    Assert(r.count == 4);
}

Fact("TouchRouter Span Presses As One Key")
{
    Rig r;
    r.SetMixed(0);
    int y = CY(SPAN_2U);
    Assert(r.Press(L(SPAN_2U) + M, y) == 500);
    Assert(r.Press(R(0), y) == 500);                    // the old seam between slots 0 and 1
    Assert(r.Press(R(SPAN_2U) - M - 1, y) == 500);

    // Held, the finger can slide the whole width of the span
    r.Down(L(SPAN_2U) + M, y);
    r.Advance(WIN + 20);
    Assert(!r.Move(L(SPAN_2U) + M + 5, y));
    r.Advance(100);
    Assert(!r.Move(R(SPAN_2U) - 1, y));
    Assert(r.look && r.look_key == 500);
    r.Up(R(SPAN_2U) - 1, y);
    Assert(r.count == 8 && r.Is(6, 500, TOUCH_KEY_PRESS) && r.Is(7, 500, TOUCH_KEY_RELEASE));

    // ...and leaving it cancels
    r.Down(CX(SPAN_2U), y);
    r.Advance(WIN + 20);
    r.Move(R(SPAN_2U), y);                              // into the 1U key next to it
    Assert(r.count == 10 && r.Is(9, 500, TOUCH_KEY_CANCEL));
    Assert(!r.look && !r.look_bad);
}

Fact("TouchRouter Quarter Unit Key Presses In Its Middle")
{
    Rig r;
    r.SetMixed(0);
    int y = CY(QUARTER_A);
    Assert(r.Press(L(QUARTER_A) + M, y) == 510);
    Assert(r.Press(R(QUARTER_A) - M - 1, y) == 510);
    Assert(r.Press(R(QUARTER_A) - M, y) == KEY_ID_NONE);
    Assert(r.Press(L(QUARTER_B) + M, y) == 511);
}

Fact("TouchRouter Group Seam Only Swipes")
{
    Rig r;
    r.SetMixed(0);
    int seam = R(GROUP_PLUS), y = CY(GROUP_PLUS);
    r.Down(seam - 1, y);
    Assert(!r.look);
    r.Advance(WIN * 3);
    Assert(r.Move(seam - 1 + TOUCH_DRAG_SLOP_PX + 1, y));
    r.Up(seam + 40, y);
    Assert(r.count == 0 && r.look_changes == 0);

    Assert(r.Press(seam - M - 1, y) == 520);
    Assert(r.Press(seam + M, y) == 521);
    Assert(r.Press(CX(GROUP_UP), B(GROUP_UP) - M - 1) == 522);
    Assert(r.Press(CX(GROUP_UP), B(GROUP_UP) + M) == 523);
    Assert(r.Press(CX(GROUP_UP), B(GROUP_UP)) == KEY_ID_NONE);
}

Fact("TouchRouter Empty Area Only Swipes")
{
    Rig r;
    r.SetMixed(0);
    r.Down(CX(7), CY(7));
    Assert(!r.look);
    r.Advance(WIN * 3);
    Assert(r.Move(CX(7) + TOUCH_DRAG_SLOP_PX + 1, CY(7)));
    r.Up(CX(7) + 50, CY(7));

    r.SetEmpty(1);
    r.page = 1;
    r.Down(CX(0), CY(0));
    r.Up(CX(0), CY(0));
    Assert(r.count == 0 && r.look_changes == 0);
}

Fact("TouchRouter Held Key Removed From Its Page Cancels")
{
    Rig r;
    r.Down(CX(1), CY(1));
    r.Advance(WIN + 20);
    r.SetMixed(0);                      // the layout changes under the finger: key 1 is gone
    r.Advance(32);
    Assert(r.count == 2 && r.Is(1, ID(1), TOUCH_KEY_CANCEL));
    Assert(!r.look && !r.look_bad);
}

// --- Pressed look: shown from touch-down, reverted if it turns into a swipe ---

Fact("TouchRouter Look Shows At Touch Down")
{
    Rig r;
    r.page = 2;
    r.Down(CX(7), CY(7));
    Assert(r.look && r.look_page == 2 && r.look_key == ID(7, 2));
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
    r.Down(CX(6), T(6) + M);
    Assert(r.look);
    r.Move(CX(6), T(6) - 3);            // into key 1, above
    Assert(!r.look && !r.look_bad);
}

Fact("TouchRouter Look Reverts On Cancel")
{
    Rig r;
    r.Down(CX(6), CY(6));
    r.Advance(WIN + 20);
    Assert(r.look);
    r.Move(R(6) + 5, CY(6));
    Assert(!r.look && !r.look_bad);
    r.Move(CX(6), CY(6));               // back over the key: stays off
    Assert(!r.look);
}

Fact("TouchRouter Look Reverts When The Held Key Goes")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.Advance(WIN + 20);
    r.inactive[ID(5)] = true;
    r.Advance(32);
    Assert(!r.look && !r.look_bad);
}

Fact("TouchRouter Look Reverts When The Key Goes Before The Press")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.inactive[ID(5)] = true;
    r.Advance(WIN + 20);
    Assert(!r.look && !r.look_bad);
}

Fact("TouchRouter No Look For Touches That Can Only Swipe")
{
    Rig r;
    int margin_x = KEY_GRID_X0 / 2;
    r.Down(margin_x, CY(0));            // the margin left of the grid
    r.Up(margin_x, CY(0));
    r.Down(R(0), CY(0));                // the seam between keys 0 and 1
    r.Up(R(0), CY(0));
    r.Down(CX(0), SCREEN_STATUS_H / 2);
    r.Up(CX(0), SCREEN_STATUS_H / 2);
    r.Down(L(2) + M - 1, CY(2));
    r.Up(0, 0);
    r.inactive[ID(8)] = true;
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
    Assert(r.look && r.look_key == ID(9) && !r.look_bad);
    r.Advance(WIN + 20);
    r.Down(CX(1), CY(1));               // and again, this time held
    Assert(r.look && r.look_key == ID(1) && !r.look_bad);
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
    Assert(r.Is(0, ID(6), TOUCH_KEY_PRESS));

    // Slide far along the key's row inside the key: never a swipe
    Assert(!r.Move(CX(6) + 40, CY(6)));
    Assert(!r.Move(CX(6) - 40, CY(6)));
    r.Advance(1000);
    Assert(!r.Move(CX(6) + 60, CY(6)));
    r.Up(CX(6) + 60, CY(6));
    Assert(r.count == 2 && r.Is(1, ID(6), TOUCH_KEY_RELEASE));
}

Fact("TouchRouter Sliding Out After The Press Cancels")
{
    Rig r;
    r.Down(CX(6), CY(6));
    r.Advance(WIN + 20);

    Assert(!r.Move(R(6) + 5, CY(6)));
    Assert(r.count == 2 && r.Is(1, ID(6), TOUCH_KEY_CANCEL));

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
    r.Move(CX(6), T(6) - 1);
    Assert(r.count == 2 && r.Is(1, ID(6), TOUCH_KEY_CANCEL));
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

Fact("TouchRouter Exit Slop Is Added To The Key")
{
    Rig r;
    r.Down(CX(6), CY(6));
    r.Advance(WIN + 20);
    r.Move(R(6) + TOUCH_KEY_EXIT_SLOP_PX - 1, CY(6));
    Assert(r.count == 1);
    r.Move(R(6) + TOUCH_KEY_EXIT_SLOP_PX, CY(6));
    Assert(r.count == 2 && r.Is(1, ID(6), TOUCH_KEY_CANCEL));
}

Fact("TouchRouter Leaving The Key Early Is Swipe Only")
{
    Rig r;
    r.Down(CX(6), T(6) + M);
    r.Move(CX(6), T(6) - 3);            // out of the key, inside the window
    r.Advance(WIN * 2);
    r.Up(CX(6), T(6) - 3);
    Assert(r.count == 0);
}

Fact("TouchRouter Leaving The Key Early Can Still Swipe")
{
    Rig r;
    r.Down(CX(6), T(6) + M);
    r.Move(CX(6), T(6) - 3);
    r.Advance(WIN * 2);
    Assert(r.Move(CX(6) + TOUCH_DRAG_SLOP_PX + 1, T(6) - 3));
}

// --- Touches that can only swipe ---

Fact("TouchRouter Margin Touch Only Swipes")
{
    Rig r;
    int x = KEY_GRID_X0 / 2;
    r.Down(x, CY(0));
    r.Advance(WIN * 3);
    Assert(r.Move(x + TOUCH_DRAG_SLOP_PX + 1, CY(0)));      // still free to swipe after the window
    r.Up(x + 50, CY(0));
    Assert(r.count == 0);
}

Fact("TouchRouter Seam Tap Does Nothing")
{
    Rig r;
    r.Down(R(0), CY(0));
    r.Up(R(0), CY(0));
    r.Down(R(0) - 1, CY(0));
    r.Advance(WIN * 3);
    r.Up(R(0) - 1, CY(0));
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
    int x = L(2) + M - 1;
    r.Down(x, CY(2));
    r.Advance(WIN * 3);
    Assert(r.Move(x + TOUCH_DRAG_SLOP_PX + 1, CY(2)));
    r.Up(x + 40, CY(2));
    Assert(r.count == 0);

    // One pixel further in presses
    r.Down(x + 1, CY(2));
    r.Advance(WIN + 20);
    Assert(r.Is(0, ID(2), TOUCH_KEY_PRESS));
}

Fact("TouchRouter Inactive Key Only Swipes")
{
    Rig r;
    r.inactive[ID(8)] = true;
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
    r.inactive[ID(5)] = true;
    r.Advance(32);
    Assert(r.count == 2 && r.Is(1, ID(5), TOUCH_KEY_CANCEL));
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
    Assert(r.count == 2 && r.Is(1, ID(5), TOUCH_KEY_CANCEL) && r.ev[1].page == 0);
}

Fact("TouchRouter Key Disabled Before The Press Falls Back To Swiping")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.inactive[ID(5)] = true;
    r.Advance(WIN * 3);
    Assert(r.count == 0);
    Assert(r.Move(CX(5) + TOUCH_DRAG_SLOP_PX + 1, CY(5)));
}

Fact("TouchRouter Key Disabled Before A Quick Tap Lifts Sends Nothing")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.inactive[ID(5)] = true;
    r.Up(CX(5), CY(5));
    Assert(r.count == 0);
}

Fact("TouchRouter Lost Up Cancels A Held Key On The Next Down")
{
    Rig r;
    r.Down(CX(5), CY(5));
    r.Advance(WIN + 20);
    r.Down(CX(9), CY(9));               // no UP in between
    Assert(r.count == 2 && r.Is(1, ID(5), TOUCH_KEY_CANCEL));
    r.Advance(WIN + 20);
    Assert(r.count == 3 && r.Is(2, ID(9), TOUCH_KEY_PRESS));
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
    Assert(r.count == 2 && r.Is(0, ID(1), TOUCH_KEY_PRESS) && r.Is(1, ID(1), TOUCH_KEY_RELEASE));
}

// --- Wake gate: the backlight is off (docs/backlight.md) ---

Fact("TouchGate Touch While Off Sends One Wake And Nothing Else")
{
    Rig r;
    r.SetEnabled(false);
    r.Down(CX(7), CY(7));
    Assert(r.wakes == 1);
    Assert(r.wake_ms == (uint32_t)(r.now / MS));
    Assert(!r.look && r.look_changes == 0);

    // Moving (even far enough to swipe), a long hold and the lift add nothing
    Assert(!r.Move(CX(7) + 100, CY(7)));
    Assert(!r.Move(CX(7) + 300, CY(7)));
    r.Advance(WIN * 4);
    r.Up(CX(7) + 300, CY(7));
    Assert(r.wakes == 1 && r.count == 0 && r.look_changes == 0);
}

Fact("TouchGate Each Touch Down Wakes Once")
{
    Rig r;
    r.SetEnabled(false);
    for (int i = 0; i < 3; i++) {
        r.Down(CX(i), CY(i));
        r.Advance(30);
        r.Up(CX(i), CY(i));
        Assert(r.wakes == i + 1);
    }
    Assert(r.count == 0 && r.look_changes == 0);
}

Fact("TouchGate Quick Tap While Off Presses Nothing")
{
    Rig r;
    r.SetEnabled(false);
    r.Down(CX(7), CY(7));
    r.Up(CX(7), CY(7));
    Assert(r.wakes == 1 && r.count == 0);
}

Fact("TouchGate Touch Down Before Turning On Stays Swallowed")
{
    Rig r;
    r.SetEnabled(false);
    r.Down(CX(7), CY(7));
    r.SetEnabled(true);
    Assert(r.wakes == 1);
    r.Advance(WIN * 2);                 // would be a PRESS if it reached the key
    Assert(!r.Move(CX(7) + 100, CY(7)));
    r.Up(CX(7), CY(7));
    Assert(r.wakes == 1 && r.count == 0 && r.look_changes == 0);

    // The next touch goes to the key as usual
    r.Down(CX(7), CY(7));
    r.Advance(WIN + 20);
    r.Up(CX(7), CY(7));
    Assert(r.wakes == 1 && r.count == 2 && r.Is(0, ID(7), TOUCH_KEY_PRESS) && r.Is(1, ID(7), TOUCH_KEY_RELEASE));
}

Fact("TouchGate Off While Held Cancels The Key")
{
    Rig r;
    r.Down(CX(7), CY(7));
    r.Advance(WIN + 20);
    Assert(r.count == 1 && r.look);
    r.SetEnabled(false);
    Assert(r.count == 2 && r.Is(1, ID(7), TOUCH_KEY_CANCEL));
    Assert(!r.look && !r.look_bad);
    Assert(r.wakes == 0);               // the finger was already down: no wake

    // The rest of the touch is swallowed
    r.Move(R(7) + 50, CY(7));
    r.Up(CX(7), CY(7));
    Assert(r.count == 2 && r.wakes == 0);

    // The next touch down wakes
    r.Down(CX(7), CY(7));
    Assert(r.wakes == 1);
}

Fact("TouchGate Off While Pending Sends Nothing And Reverts The Look")
{
    Rig r;
    r.Down(CX(7), CY(7));
    Assert(r.look);
    r.SetEnabled(false);
    Assert(r.count == 0 && !r.look && !r.look_bad && r.wakes == 0);
    r.Advance(WIN * 2);                 // the window ending presses nothing
    r.Up(CX(7), CY(7));
    Assert(r.count == 0 && r.wakes == 0 && r.look_changes == 2);
}

Fact("TouchGate Off Mid Swipe Swallows The Rest")
{
    Rig r;
    r.Down(CX(7), CY(7));
    Assert(r.Move(CX(7) + 50, CY(7)));          // a swipe has started
    r.SetEnabled(false);
    Assert(!r.Move(CX(7) + 100, CY(7)));        // no further swipe decisions
    r.Up(CX(7) + 100, CY(7));
    Assert(r.count == 0 && r.wakes == 0 && !r.look);
}

Fact("TouchGate Off With A Finger Down Wakes On The Next Touch Down Only")
{
    Rig r;
    r.Down(CX(2), CY(2));
    r.SetEnabled(false);
    r.Advance(500);
    Assert(r.wakes == 0);
    r.Up(CX(2), CY(2));
    Assert(r.wakes == 0);
    r.Down(CX(2), CY(2));
    Assert(r.wakes == 1);
}

Fact("TouchGate Setting The Current State Changes Nothing")
{
    Rig r;
    // On while a key is held
    r.Down(CX(3), CY(3));
    r.Advance(WIN + 20);
    r.SetEnabled(true);
    Assert(r.count == 1 && r.look);
    r.Up(CX(3), CY(3));
    Assert(r.count == 2 && r.Is(1, ID(3), TOUCH_KEY_RELEASE));

    // Off while a touch is swallowed: still swallowed, still no wake
    r.SetEnabled(false);
    r.Down(CX(3), CY(3));
    r.SetEnabled(false);
    Assert(r.wakes == 1);
    r.Up(CX(3), CY(3));
    Assert(r.wakes == 1 && r.count == 2);
}

Fact("TouchGate Turning On Then Off Again Mid Touch")
{
    Rig r;
    r.SetEnabled(false);
    r.Down(CX(1), CY(1));
    r.SetEnabled(true);
    r.SetEnabled(false);
    r.Up(CX(1), CY(1));
    r.SetEnabled(true);
    Assert(r.wakes == 1 && r.count == 0);
}

// --- Fuzz ---

namespace
{
    // Random touches against random changes (keys going inactive, pages
    // changing, a page's layout switching between the grid, the mixed keys
    // and nothing), checking after every step:
    //  - every PRESS is followed by exactly one RELEASE or CANCEL for the same key
    //  - no key event outside a press, and none for a touch that swiped
    //  - a swipe never starts after a PRESS, nor a PRESS after a swipe
    //  - a PRESS is for an active key on the page the touch went down on
    //  - the look is never wrong, never outlives the touch, and is on while a press is open
    void FuzzTouches(uint32_t seed)
    {
        Rng rng(seed);
        Rig r;
        bool down = false;
        bool swiped = false;
        bool pressed = false;       // a PRESS sent for this touch, not yet closed
        uint16_t pressed_key = KEY_ID_NONE;
        int  down_page = 0;
        int  checked = 0;
        int  page_count = 3;

        for (int step = 0; step < 4000; step++) {
            int dice = rng.Range(0, 99);
            if (dice < 12 && !down) {
                int x, y;
                if (rng.Chance(70) && r.key_count[r.page] > 0) {
                    const KeyPixelRect &k = r.keys[r.page][rng.Range(0, r.key_count[r.page] - 1)].rect;
                    x = k.x + rng.Range(-4, k.w + 4);
                    y = SCREEN_PAGE_TOP + k.y + rng.Range(-4, k.h + 4);
                } else {
                    x = rng.Range(0, SCREEN_W - 1);
                    y = rng.Range(0, SCREEN_H - 1);
                }
                down = true;
                down_page = r.page;
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
                if (r.key_count[r.page] > 0)
                    r.inactive[r.keys[r.page][rng.Range(0, r.key_count[r.page] - 1)].id] = rng.Chance(50);
            } else if (dice < 84) {
                r.page = rng.Range(0, page_count - 1);
            } else if (dice < 86) {
                int p = rng.Range(0, page_count - 1);
                int kind = rng.Range(0, 9);
                if (kind < 5)
                    r.key_count[p] = GridKeys(p, r.keys[p]);
                else if (kind < 9)
                    r.SetMixed(p);
                else
                    r.SetEmpty(p);
            } else {
                r.Advance(rng.Range(1, 80));
            }

            for (; checked < r.count; checked++) {
                const Ev &e = r.ev[checked];
                Check(e.page == down_page);
                if (e.action == TOUCH_KEY_PRESS) {
                    Check(!pressed);
                    Check(!swiped);
                    Check(r.Has(e.page, e.key) && !r.inactive[e.key]);
                    pressed = true;
                    pressed_key = e.key;
                } else {
                    Check(pressed);
                    Check(e.key == pressed_key);
                    pressed = false;
                }
            }

            Check(!r.look_bad);
            if (!down || swiped)
                Check(!r.look);
            if (pressed)
                Check(r.look && r.look_key == pressed_key);
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
