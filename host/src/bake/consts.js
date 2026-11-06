// Constants shared by the bake stages and the packed schema. Anything the firmware also
// knows is noted; test/render-real.test.js checks KEY_SIZE against the firmware header.

export const BLOB_VERSION = 1;        // the packed blob's version (config files have their own, see FILE_VERSION)
export const FILE_VERSION = 1;        // the user-level config file format (docs/config-format.md)

export const KEY_COLS = 5;
export const KEY_ROWS = 3;
export const KEYS_PER_PAGE = KEY_COLS * KEY_ROWS;
export const KEY_SIZE = 128;          // firmware: COMP_KEY_SIZE in page_compositor.h

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

// switchActivity's index for "no activity" (config file: `"activity": null`). Firmware:
// CONFIG_ACTIVITY_NONE in config_types.h.
export const ACTIVITY_NONE = 0xFFFFFFFF;

// hidKey modes
export const HID_MODE = { tap: 0, hold: 1, down: 2, up: 3 };

// The gestures a touch key can have, in the order their bindings are generated
export const GESTURES = ["press", "release", "tap", "long"];

// Sanity range for long-press times (ms); config-format.md only says "int"
export const LONG_PRESS_MIN = 100;
export const LONG_PRESS_MAX = 10000;

// The longest script source, in bytes. Keep in step with CONFIG_MAX_SCRIPT in firmware/main/config_blob.h.
export const MAX_SCRIPT_BYTES = 16384;
