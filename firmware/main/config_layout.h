#pragma once

// Byte layouts of the packed config structs (tools/bake/src/schema.js;
// `node cli.js config.json --header types.h` prints the C declarations) and
// the op codes (tools/bake/src/consts.js OP). Shared by the validator
// (config_blob.cpp) and the runtime readers (config_view.h, the dispatcher),
// which read by offset instead of casting to structs so they also run in the
// 64-bit host tests. Keep in step with the schema: TestConfigBlob bakes the
// sample with the real packer.

#include "page_compositor.h"        // COMP_KEYS
#include "screen_layout.h"          // SCREEN_MAX_PAGES

namespace config_layout
{
    enum
    {
        KEY_SIZE            = 36,       // 4 x (count, ptr), longPressMs
        KEY_LONGPRESS       = 32,       // after the four (count, ptr) images
        PAGE_SIZE           = KEY_SIZE * COMP_KEYS,
        DEVICE_SIZE         = 20,       // name, turnOn (count, ptr), turnOff (count, ptr)
        ACTIVITY_SIZE       = 64,

        // binding: control, event, ops (count, ptr to op pointers)
        BINDING_SIZE        = 16,
        BIND_CONTROL = 0, BIND_EVENT = 4, BIND_OPS = 8,

        ROOT_SIZE           = 572,
        ROOT_VERSION        = 0,
        ROOT_LONGPRESS      = 4,
        ROOT_HOME_KEYS      = 8,
        ROOT_HOME_BINDINGS  = 8 + PAGE_SIZE,        // count, ptr
        ROOT_DEVICES        = ROOT_HOME_BINDINGS + 8,
        ROOT_ACTIVITIES     = ROOT_DEVICES + 8,

        ACT_NAME = 0, ACT_DISPLAY = 4, ACT_DEVICES = 8, ACT_PAGES = 16, ACT_BINDINGS = 24, ACT_HOOKS = 32,

        // op: u32 code, then the fields for that code
        OP_CODE = 0,
        OP_DELAY_MS = 4,                // delay: u32 ms
        HIDOP_MODE = 4, HIDOP_MODS = 8, HIDOP_KEY = 12,     // hidKey: three bytes, each padded to 4
    };
    static_assert(ROOT_ACTIVITIES + 8 == ROOT_SIZE, "root layout");

    enum { OP_DELAY = 1, OP_SWITCH_ACTIVITY, OP_HID_KEY, OP_HTTP_GET, OP_HTTP_POST, OP_UDP_PACKET, OP_SEND_WOL, OP_SCRIPT };
}

// hidKey modes (tools/bake/src/consts.js HID_MODE)
#define HID_MODE_TAP        0
#define HID_MODE_HOLD       1       // down on `press`, up on `release`
#define HID_MODE_DOWN       2
#define HID_MODE_UP         3
#define HID_MODE_MAX        3
#define CONFIG_MAX_OP_DATA  4096
