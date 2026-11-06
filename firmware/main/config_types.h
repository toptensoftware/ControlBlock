#pragma once

// The packed config structs, as the firmware sees them. They are generated from
// the binpack schema (host/src/bake/schema.js -> config_types.gen.h, run
// `node tools/codegen/gen-types.js`) so nothing here calculates a layout.
//
// The generated header declares generic global names (key, page, op, ...), so
// it is wrapped in the namespace `cfgdata`. Its OP_* and HID_MODE_* macros are
// global.
//
// The blob stores 32-bit pointers: the structs match it only where a pointer is
// 4 bytes (the device, and the host tests, which build as 32-bit). A relocated
// blob (ConfigRelocate) can then be read straight through these structs.

#include <stddef.h>
#include <stdint.h>
#include "key_layout.h"             // KEY_SLOTS
#include "page_compositor.h"        // COMP_KEYS

namespace cfgdata
{
    #include "config_types.gen.h"
}

static_assert(sizeof(void *) == 4, "the config blob holds 32-bit pointers: build for a 32-bit target (host tests: platform x86)");

// A page of keys is the 5x3 grid
static_assert(sizeof(cfgdata::page) == sizeof(cfgdata::key) * COMP_KEYS, "COMP_KEYS must match the schema's page");

#define HID_MODE_MAX    HID_MODE_UP

// switchActivityOp's index for "no activity" (config file: `"activity": null`).
// Keep in step with ACTIVITY_NONE in host/src/bake/consts.js.
#define CONFIG_ACTIVITY_NONE    0xFFFFFFFFu

// The key's four (count, pointer) image pairs, in KEY_SLOT_* order
static_assert(KEY_SLOT_BG == 0 && KEY_SLOT_FG == 1 && KEY_SLOT_PRESSED_BG == 2 && KEY_SLOT_PRESSED_FG == 3 && KEY_SLOTS == 4,
              "KEY_SLOT_* order must match the schema's key");

// Offsets, within a key, of image `slot`'s count and of its pointer
inline uint32_t ConfigKeyImageOffset(int slot)
{
    static const uint32_t offsets[KEY_SLOTS] = {
        offsetof(cfgdata::key, normalBg_count),
        offsetof(cfgdata::key, normalFg_count),
        offsetof(cfgdata::key, pressedBg_count),
        offsetof(cfgdata::key, pressedFg_count),
    };
    return offsets[slot];
}

inline uint32_t ConfigKeyImagePtrOffset(int slot)
{
    static const uint32_t offsets[KEY_SLOTS] = {
        offsetof(cfgdata::key, normalBg),
        offsetof(cfgdata::key, normalFg),
        offsetof(cfgdata::key, pressedBg),
        offsetof(cfgdata::key, pressedFg),
    };
    return offsets[slot];
}
