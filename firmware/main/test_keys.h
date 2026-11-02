#pragma once

// Layout of firmware/assets/test_keys.bin, built by tools/test-icons/gen.js.
// A blobpack of 128x128 rle565 images:
//   faces (bg):     TEST_KEYS_FACES with transparent padding, then the same
//                   faces opaque (drawn on COMP_BACKGROUND)
//   icons (fg):     from TEST_KEYS_FIRST_ICON to the last blob
//   last blob:      names, NUL separated

#define TEST_KEYS_FACES         4   // red, green, blue, pressed
#define TEST_KEYS_FACE_PRESSED  3
#define TEST_KEYS_FIRST_ICON    (TEST_KEYS_FACES * 2)

// Blob index of face `i` (0..TEST_KEYS_FACES-1), transparent or opaque
#define TEST_KEYS_FACE(i, opaque)   ((i) + ((opaque) ? TEST_KEYS_FACES : 0))

// Number of icons in a pack with `count` blobs
#define TEST_KEYS_ICON_COUNT(count) ((int)(count) - 1 - TEST_KEYS_FIRST_ICON)
