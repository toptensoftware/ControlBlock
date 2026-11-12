#pragma once

// Layout of firmware/assets/test_keys.bin, built by tools/test-icons/gen.js.
// A blobpack of rle565 images:
//   faces (bg):     TEST_KEYS_FACES 128x128 faces with transparent padding,
//                   then the same faces opaque (drawn on COMP_BACKGROUND)
//   icons (fg):     128x128, from TEST_KEYS_FIRST_ICON to the opaque keys
//   opaque keys:    TEST_KEYS_KEYS_1U 1U keys (KEY_CELL x KEY_CELL), then
//                   TEST_KEYS_KEYS_2U 2U keys (2 x KEY_CELL wide): fully
//                   opaque, fully baked (face, icon, label over black)
//   last blob:      names, NUL separated
// The device (RUN_STRESS) uses only the opaque keys. The 128 faces and icons
// stay for the host tests, which need small and transparent images (the image
// store's own tests, refusals of non-opaque images).

#define TEST_KEYS_FACE_SIZE     128 // faces and icons: TEST_KEYS_FACE_SIZE square
#define TEST_KEYS_FACES         4   // red, green, blue, pressed
#define TEST_KEYS_FACE_PRESSED  3
#define TEST_KEYS_FIRST_ICON    (TEST_KEYS_FACES * 2)

#define TEST_KEYS_KEYS_1U       15
#define TEST_KEYS_KEYS_2U       5
#define TEST_KEYS_KEYS          (TEST_KEYS_KEYS_1U + TEST_KEYS_KEYS_2U)

// Blob index of face `i` (0..TEST_KEYS_FACES-1), transparent or opaque
#define TEST_KEYS_FACE(i, opaque)   ((i) + ((opaque) ? TEST_KEYS_FACES : 0))

// In a pack with `count` blobs: the first opaque key, and the number of icons
#define TEST_KEYS_FIRST_KEY(count)  ((int)(count) - 1 - TEST_KEYS_KEYS)
#define TEST_KEYS_ICON_COUNT(count) (TEST_KEYS_FIRST_KEY(count) - TEST_KEYS_FIRST_ICON)

// Blob index of opaque 1U key `i` (0..TEST_KEYS_KEYS_1U-1) or 2U key `i`
#define TEST_KEYS_KEY_1U(count, i)  (TEST_KEYS_FIRST_KEY(count) + (i))
#define TEST_KEYS_KEY_2U(count, i)  (TEST_KEYS_FIRST_KEY(count) + TEST_KEYS_KEYS_1U + (i))
