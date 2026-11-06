#pragma once

// Key chord text ("ctrl+shift+esc", "f5") to a HID modifier mask and key code, with the
// bake's names (hid_keys.gen.h, generated from host/src/bake/hid.js) and the same rules:
// modifiers plus at most one key, case and spaces around names ignored.
// Platform independent.

#include <stddef.h>
#include <stdint.h>

// Null on success (key 0 = modifiers only), else why not (a string literal)
const char *HidParseChord(const char *text, size_t len, uint8_t *modifiers, uint8_t *key);
