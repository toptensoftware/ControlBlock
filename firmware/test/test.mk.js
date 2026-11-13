// Host unit tests for the firmware's platform-independent code.
// Run from this folder: `mk run` (needs `npm install -g toptensoftware/mk.js`).

import { readFileSync } from "node:fs";

export default async function ()
{
    await this.use("c-cpp");

    this.set({
        // 32-bit: the layout blob holds 32-bit pointers, so the structs generated from the
        // binpack schema (main/layout_types.gen.h) match the device's struct offsets only in a 32-bit build.
        platform: "x86",
        sourceFiles: (base) => [...base, ...this.otherSourceFiles],
        otherSourceFiles: [
            "../main/page_compositor.cpp",
            "../main/key_geometry.cpp",
            "../main/opaque_row.cpp",
            "../main/screen_view.cpp",
            "../main/status_bar.cpp",
            "../main/status_icons.cpp",
            "../main/log_ring.cpp",
            "../main/diag_status.cpp",
            "../main/clock_text.cpp",
            "../main/image_store.cpp",
            "../main/link_stats.cpp",
            "../main/key_layout.cpp",
            "../main/protocol.cpp",
            "../main/layout_store.cpp",
            "../main/layout_blob.cpp",
            "../main/layout_apply.cpp",
            "../main/wifi_credentials.cpp",
            "../main/wifi_state.cpp",
            "../main/clock_config.cpp",
            "../main/device_name.cpp",
            "../main/touch_router.cpp",
            "../main/touch_gate.cpp",
            "../main/key_gesture.cpp",
            "../main/hid_keyboard.cpp",
            "../main/op_queue.cpp",
            "../main/op_registry.cpp",
            "../main/exec_net.cpp",
            "../main/lua_heap.cpp",
            "../main/script_host.cpp",
            "../main/script_api.cpp",
            "../main/hid_chord.cpp",
            "../main/net_queue.cpp",
            "../main/trigger_dispatch.cpp",
            "../main/activity_switch.cpp",
            "../main/activity_host.cpp",
            "../main/standalone.cpp",
            "../../rle565/src/rle565.c",
            // Lua: the same list as the device build (firmware/lua_sources.txt)
            ...readFileSync(new URL("../lua_sources.txt", import.meta.url), "utf8")
                .split(/\r?\n/).filter(Boolean).map(f => "../../lua/" + f),
        ],
        includePath: [
            "../../SimpleLib",
            "../main",
            "../../rle565/src",
            "../../lua",
        ],
        define: [
            "_CRT_SECURE_NO_WARNINGS",
            'LUA_USER_H="lua_user.h"',     // our Lua settings (main/lua_user.h), as on the device
        ],
        gcc_cpp_args: [
            "-Wno-unused-variable",
        ],
        msvc_cl_args: [
            "/EHsc",            // Assert() throws
            "/wd4200",          // zero-sized arrays in rle565 headers
        ],
    });
};
