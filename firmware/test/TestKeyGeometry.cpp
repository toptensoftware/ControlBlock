#include "UnitTesting.h"
#include "Core.h"
#include "key_geometry.h"

using namespace SimpleLib;

// Key geometry: 144 px cells edge to edge in a 5x3 grid centred in the page
// area, key rects in quarter units (36 px), rect and id validation

Fact("KeyGeometry Grid Is Centred In The Page Area")
{
    Assert(KEY_CELL == 144);
    Assert(KEY_QUARTER == 36);
    Assert(KEY_GRID_W == 720 && KEY_GRID_H == 432);
    Assert(KEY_GRID_X0 == 40 && KEY_GRID_Y0 == 4);
    Assert(KEY_GRID_X0 * 2 + KEY_GRID_W == SCREEN_PAGE_W);
    Assert(KEY_GRID_Y0 * 2 + KEY_GRID_H == SCREEN_PAGE_H);
    Assert(KEY_GRID_QW == 20 && KEY_GRID_QH == 12);
    Assert(COMP_MAX_KEYS == 32);
}

Fact("KeyGeometry Rect To Pixels")
{
    KeyPixelRect p = KeyRectPixels(KeyRect{ 0, 0, 4, 4 });
    Assert(p.x == 40 && p.y == 4 && p.w == 144 && p.h == 144);

    // the bottom-right 1U slot
    p = KeyRectPixels(KeyRect{ 16, 8, 4, 4 });
    Assert(p.x == 40 + 576 && p.y == 4 + 288 && p.w == 144 && p.h == 144);
    Assert(p.x + p.w == KEY_GRID_X0 + KEY_GRID_W && p.y + p.h == KEY_GRID_Y0 + KEY_GRID_H);

    // fractional: 1.5U wide at 0.25U, 0.5U tall at 2.75U
    p = KeyRectPixels(KeyRect{ 1, 11, 6, 2 });
    Assert(p.x == 40 + 36 && p.y == 4 + 396 && p.w == 216 && p.h == 72);
}

Fact("KeyGeometry Rect Inside Grid")
{
    Assert(KeyRectValid(KeyRect{ 0, 0, 4, 4 }));
    Assert(KeyRectValid(KeyRect{ 0, 0, 20, 12 }));      // the whole grid
    Assert(KeyRectValid(KeyRect{ 19, 11, 1, 1 }));      // the smallest, in the last corner
    Assert(!KeyRectValid(KeyRect{ 0, 0, 0, 4 }));       // empty
    Assert(!KeyRectValid(KeyRect{ 0, 0, 4, 0 }));
    Assert(!KeyRectValid(KeyRect{ 17, 0, 4, 4 }));      // past the right edge
    Assert(!KeyRectValid(KeyRect{ 0, 9, 4, 4 }));       // past the bottom
    Assert(!KeyRectValid(KeyRect{ 20, 0, 1, 1 }));
    Assert(!KeyRectValid(KeyRect{ 0, 0, 255, 255 }));
    Assert(!KeyRectValid(KeyRect{ 250, 0, 10, 4 }));    // no u8 wrap
}

Fact("KeyGeometry Rects Overlap")
{
    KeyRect a{ 0, 0, 4, 4 };
    Assert(KeyRectsOverlap(a, a));
    Assert(!KeyRectsOverlap(a, KeyRect{ 4, 0, 4, 4 }));     // abutting across
    Assert(!KeyRectsOverlap(a, KeyRect{ 0, 4, 4, 4 }));     // abutting down
    Assert(KeyRectsOverlap(a, KeyRect{ 3, 3, 4, 4 }));      // a quarter unit's corner
    Assert(KeyRectsOverlap(KeyRect{ 3, 3, 4, 4 }, a));
    Assert(KeyRectsOverlap(KeyRect{ 0, 0, 20, 12 }, KeyRect{ 8, 4, 1, 1 }));   // contains
    Assert(!KeyRectsOverlap(a, KeyRect{ 4, 4, 4, 4 }));     // touching corners
}

Fact("KeyGeometry Page Of Rects")
{
    // a full 5x3 page of 1U keys
    KeyRect grid[15];
    for (int i = 0; i < 15; i++)
        grid[i] = KeyRect{ (uint8_t)(i % 5 * 4), (uint8_t)(i / 5 * 4), 4, 4 };
    Assert(KeyRectsCheck(grid, 15) == KEY_GEOM_OK);
    Assert(KeyRectsCheck(grid, 0) == KEY_GEOM_OK);          // an empty page

    // a 2U span and a 1.5U key beside it
    KeyRect mix[] = { { 0, 0, 8, 4 }, { 8, 0, 6, 4 }, { 14, 0, 6, 4 }, { 0, 4, 20, 8 } };
    Assert(KeyRectsCheck(mix, 4) == KEY_GEOM_OK);

    KeyRect out[] = { { 0, 0, 4, 4 }, { 18, 0, 4, 4 } };
    Assert(KeyRectsCheck(out, 2) == KEY_GEOM_OUT_OF_GRID);

    KeyRect over[] = { { 0, 0, 4, 4 }, { 8, 0, 4, 4 }, { 2, 2, 4, 4 } };
    Assert(KeyRectsCheck(over, 3) == KEY_GEOM_OVERLAP);
}

Fact("KeyGeometry At Most COMP_MAX_KEYS Per Page")
{
    // 0.25U x 0.5U keys never overlap: 20 x 6 of them fit the grid
    KeyRect r[COMP_MAX_KEYS + 1];
    for (int i = 0; i <= COMP_MAX_KEYS; i++)
        r[i] = KeyRect{ (uint8_t)(i % 20), (uint8_t)(i / 20 * 2), 1, 2 };
    Assert(KeyRectsCheck(r, COMP_MAX_KEYS) == KEY_GEOM_OK);
    Assert(KeyRectsCheck(r, COMP_MAX_KEYS + 1) == KEY_GEOM_TOO_MANY);
}

Fact("KeyGeometry Ids Are 1 To 65535 And Unique")
{
    uint16_t ok[] = { 1, 2, 65535, 300 };
    Assert(KeyIdsCheck(ok, 4, nullptr, 0) == KEY_GEOM_OK);
    Assert(KeyIdsCheck(ok, 0, nullptr, 0) == KEY_GEOM_OK);

    uint16_t zero[] = { 1, 0, 3 };
    Assert(KeyIdsCheck(zero, 3, nullptr, 0) == KEY_GEOM_BAD_ID);

    uint16_t dup[] = { 5, 6, 7, 6 };
    Assert(KeyIdsCheck(dup, 4, nullptr, 0) == KEY_GEOM_DUPLICATE_ID);
}

Fact("KeyGeometry Plain Grid Slots")
{
    // A plain grid: 1U slot i row-major, page p's slot i has id p * 15 + i + 1 (the bake's ids)
    Assert(KEY_GRID_SLOTS == 15);
    KeyRect r = KeySlotRect(0);
    Assert(r.x == 0 && r.y == 0 && r.w == 4 && r.h == 4);
    r = KeySlotRect(7);
    Assert(r.x == 8 && r.y == 4 && r.w == 4 && r.h == 4);
    r = KeySlotRect(14);
    Assert(r.x == 16 && r.y == 8 && KeyRectValid(r));
    Assert(KeySlotId(0, 0) == 1 && KeySlotId(0, 14) == 15 && KeySlotId(1, 0) == 16 && KeySlotId(2, 4) == 35);

    KeyRect grid[KEY_GRID_SLOTS];
    for (int i = 0; i < KEY_GRID_SLOTS; i++)
        grid[i] = KeySlotRect(i);
    Assert(KeyRectsCheck(grid, KEY_GRID_SLOTS) == KEY_GEOM_OK);
}

Fact("KeyGeometry Ids Unique Across Pages")
{
    // ids already used by other pages' keys
    uint16_t others[] = { 1, 2, 3, 16, 17 };
    uint16_t fresh[] = { 4, 5, 18 };
    Assert(KeyIdsCheck(fresh, 3, others, 5) == KEY_GEOM_OK);

    uint16_t clash[] = { 4, 17 };
    Assert(KeyIdsCheck(clash, 2, others, 5) == KEY_GEOM_DUPLICATE_ID);
}
