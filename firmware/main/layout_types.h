#pragma once

// The packed layout structs, as the firmware sees them. They are generated from
// the binpack schema (host/src/bake/schema.js -> layout_types.gen.h, run
// `node tools/codegen/gen-types.js`) so nothing here calculates struct offsets.
//
// The generated header declares generic global names (key, page, op, ...), so
// it is wrapped in the namespace `layoutdata`. Its OP_* and HID_MODE_* macros are
// global.
//
// The blob stores 32-bit pointers: the structs match it only where a pointer is
// 4 bytes (the device, and the host tests, which build as 32-bit). A relocated
// blob (LayoutRelocate) can then be read straight through these structs.

#include <stddef.h>
#include <stdint.h>
#include "key_geometry.h"           // KeyRect

namespace layoutdata
{
    #include "layout_types.gen.h"
}

static_assert(sizeof(void *) == 4, "the layout blob holds 32-bit pointers: build for a 32-bit target (host tests: platform x86)");

#define HID_MODE_MAX    HID_MODE_UP

// switchActivityOp's index for "no activity" (layout file: `"activity": null`).
// Keep in step with ACTIVITY_NONE in host/src/bake/consts.js.
#define LAYOUT_ACTIVITY_NONE    0xFFFFFFFFu

// A key's images: one opaque image per state, each a (count, pointer) pair
#define LAYOUT_KEY_NORMAL   0
#define LAYOUT_KEY_PRESSED  1
#define LAYOUT_KEY_IMAGES   2

// Offsets, within a key, of image `state`'s count and of its pointer
inline uint32_t LayoutKeyImageOffset(int state)
{
    static const uint32_t offsets[LAYOUT_KEY_IMAGES] = {
        offsetof(layoutdata::key, normal_count),
        offsetof(layoutdata::key, pressed_count),
    };
    return offsets[state];
}

inline uint32_t LayoutKeyImagePtrOffset(int state)
{
    static const uint32_t offsets[LAYOUT_KEY_IMAGES] = {
        offsetof(layoutdata::key, normal),
        offsetof(layoutdata::key, pressed),
    };
    return offsets[state];
}

// A packed key's rect: rect[] is x, y, w, h in quarter units (the bake's order)
static_assert(sizeof(((layoutdata::key *)nullptr)->rect) == sizeof(KeyRect), "the blob key's rect is a KeyRect");

inline KeyRect LayoutKeyRect(const uint8_t rect[4])
{
    return KeyRect{ rect[0], rect[1], rect[2], rect[3] };
}
