// Constants shared by the bake stages and the packed schema. Anything the firmware also
// knows is noted; test/bake/render-real.test.js checks them against the firmware headers.

export const BLOB_VERSION = 2;        // the packed blob's version (layout files have their own, see FILE_VERSION). Firmware: LAYOUT_BLOB_VERSION
export const FILE_VERSION = 1;        // the user-level layout file format (docs/layout-format.md)

// The key grid (firmware: key_geometry.h). Keys are rects in units of one cell (1U), placed and
// sized in quarter units; the blob stores quarter counts.
export const KEY_COLS = 5;            // KEY_GRID_COLS
export const KEY_ROWS = 3;            // KEY_GRID_ROWS
export const GRID_SLOTS = KEY_COLS * KEY_ROWS;   // KEY_GRID_SLOTS: the plain grid's 1U slots per page
export const KEY_CELL = 144;          // KEY_CELL: 1U in pixels
export const KEY_QUARTER = KEY_CELL / 4;         // KEY_QUARTER: 0.25U in pixels
export const MAX_KEYS_PER_PAGE = 32;  // COMP_MAX_KEYS
export const MAX_KEY_ID = 65535;      // key ids are u16, 1..65535 (0 = none)

// The default look draws a FACE_SIZE face (and its icon and label) in a 1U cell, with black
// round it: (KEY_CELL - FACE_SIZE) / 2 each side (images.js FACE_MARGIN). Bigger keys keep the margin.
export const FACE_SIZE = 128;
export const FACE_MARGIN = (KEY_CELL - FACE_SIZE) / 2;

// Op codes in the packed blob (docs/standalone.md "Actions")
export const OP = {
    delay: 1,
    switch_activity: 2,
    hid_key: 3,
    http_get: 4,
    http_post: 5,
    udp_packet: 6,
    send_wol: 7,
    script: 8,
};

// switchActivity's index for "no activity" (layout file: `"activity": null`). Firmware:
// LAYOUT_ACTIVITY_NONE in layout_types.h.
export const ACTIVITY_NONE = 0xFFFFFFFF;

// hidKey modes
export const HID_MODE = { tap: 0, hold: 1, down: 2, up: 3 };

// The gestures a touch key can have, in the order their bindings are generated
export const GESTURES = ["press", "release", "tap", "long"];

// Sanity range for long-press times (ms); layout-format.md only says "int"
export const LONG_PRESS_MIN = 100;
export const LONG_PRESS_MAX = 10000;

// The longest script source, in bytes. Keep in step with LAYOUT_MAX_SCRIPT in firmware/main/layout_blob.h.
export const MAX_SCRIPT_BYTES = 16384;
