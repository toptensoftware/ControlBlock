#include "key_geometry.h"

bool KeyRectValid(KeyRect r)
{
    return r.w > 0 && r.h > 0 && r.x + r.w <= KEY_GRID_QW && r.y + r.h <= KEY_GRID_QH;
}

bool KeyRectsOverlap(KeyRect a, KeyRect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

KeyGeomResult KeyRectsCheck(const KeyRect *rects, int count)
{
    if (count > COMP_MAX_KEYS)
        return KEY_GEOM_TOO_MANY;
    for (int i = 0; i < count; i++) {
        if (!KeyRectValid(rects[i]))
            return KEY_GEOM_OUT_OF_GRID;
        for (int j = 0; j < i; j++)
            if (KeyRectsOverlap(rects[i], rects[j]))
                return KEY_GEOM_OVERLAP;
    }
    return KEY_GEOM_OK;
}

static bool contains(const uint16_t *ids, int count, uint16_t id)
{
    for (int i = 0; i < count; i++)
        if (ids[i] == id)
            return true;
    return false;
}

KeyGeomResult KeyIdsCheck(const uint16_t *ids, int count, const uint16_t *others, int others_count)
{
    for (int i = 0; i < count; i++) {
        if (ids[i] == KEY_ID_NONE)
            return KEY_GEOM_BAD_ID;
        if (contains(ids, i, ids[i]) || (others && contains(others, others_count, ids[i])))
            return KEY_GEOM_DUPLICATE_ID;
    }
    return KEY_GEOM_OK;
}
