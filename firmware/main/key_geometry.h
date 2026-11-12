#pragma once

// Key geometry: where keys sit in the page area. Pure; shared with the host
// tests. Keys are rects on a grid of 144 px cells (1U) packed edge to edge,
// 5x3 cells centred in the page area; positions and sizes are in quarter
// units (0.25U = 36 px). Each key has an id (u16, 1..65535, unique across
// the whole layout; 0 = none). Spec: docs/layout-format.md (Keys)
// and docs/protocol.md (SET_KEYS).

#include <stdint.h>
#include "screen_layout.h"

#define KEY_CELL          144       // 1U: cell width and height in pixels
#define KEY_GRID_COLS     5
#define KEY_GRID_ROWS     3
#define KEY_GRID_W        (KEY_GRID_COLS * KEY_CELL)
#define KEY_GRID_H        (KEY_GRID_ROWS * KEY_CELL)
#define KEY_GRID_X0       ((SCREEN_PAGE_W - KEY_GRID_W) / 2)     // in the page area
#define KEY_GRID_Y0       ((SCREEN_PAGE_H - KEY_GRID_H) / 2)

#define KEY_QUARTER       (KEY_CELL / 4)                        // 0.25U in pixels
#define KEY_GRID_QW       (KEY_GRID_COLS * 4)                   // grid size in quarter units
#define KEY_GRID_QH       (KEY_GRID_ROWS * 4)

#define COMP_MAX_KEYS     32        // keys per page
#define KEY_ID_NONE       0

static_assert(KEY_CELL % 4 == 0, "quarter units must be whole pixels");
static_assert(KEY_GRID_X0 >= 0 && KEY_GRID_Y0 >= 0, "the grid must fit the page area");

// A key's rect in quarter units from the grid origin (as on the wire and in
// the layout blob)
struct KeyRect
{
    uint8_t x, y, w, h;
};

// A key's rect in pixels in the page area
struct KeyPixelRect
{
    int16_t x, y, w, h;
};

inline KeyPixelRect KeyRectPixels(KeyRect r)
{
    return KeyPixelRect{
        (int16_t)(KEY_GRID_X0 + r.x * KEY_QUARTER),
        (int16_t)(KEY_GRID_Y0 + r.y * KEY_QUARTER),
        (int16_t)(r.w * KEY_QUARTER),
        (int16_t)(r.h * KEY_QUARTER),
    };
}

// A plain grid of 1U keys: slot i (0..KEY_GRID_SLOTS-1, row-major) and the id
// page p's slot i gets (p * 15 + i + 1: the bake's ids for plain grids)
#define KEY_GRID_SLOTS    (KEY_GRID_COLS * KEY_GRID_ROWS)

inline KeyRect KeySlotRect(int slot)
{
    return KeyRect{ (uint8_t)(slot % KEY_GRID_COLS * 4), (uint8_t)(slot / KEY_GRID_COLS * 4), 4, 4 };
}

inline uint16_t KeySlotId(int page, int slot)
{
    return (uint16_t)(page * KEY_GRID_SLOTS + slot + 1);
}

enum KeyGeomResult
{
    KEY_GEOM_OK,
    KEY_GEOM_OUT_OF_GRID,       // empty, or not inside the grid
    KEY_GEOM_OVERLAP,           // two keys overlap
    KEY_GEOM_TOO_MANY,          // more than COMP_MAX_KEYS
    KEY_GEOM_BAD_ID,            // id 0
    KEY_GEOM_DUPLICATE_ID,      // an id used twice, or already used by another page
};

// Not empty and inside the grid
bool KeyRectValid(KeyRect r);

// The rects share pixels (abutting rects don't)
bool KeyRectsOverlap(KeyRect a, KeyRect b);

// One page's rects: at most COMP_MAX_KEYS, each inside the grid, none overlapping
KeyGeomResult KeyRectsCheck(const KeyRect *rects, int count);

// Key ids: none 0, no two alike, none in `others` (the ids of other pages'
// keys; may be null). Pass a whole layout's ids with no `others` to check it.
KeyGeomResult KeyIdsCheck(const uint16_t *ids, int count, const uint16_t *others, int others_count);
