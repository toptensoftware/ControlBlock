#pragma once

// Drawing fully opaque rle565 images straight into a page buffer, a few rows
// at a time: the page compositor's key drawing (page_compositor.cpp). Pure;
// tested against rle565's draw_image on the host (TestOpaqueRow.cpp).
//
// An opaque image (check_image() says every pixel is opaque) stores every
// row, every row's runs cover exactly the width, and every palette entry is
// 0xFF00<rgb565>. So a row can be written with no clipping, no blending and
// no read of the destination: repeat runs with fill_px (32-bit stores),
// literal runs two pixels a store.

#include <stdint.h>

// True if `img` is an rle565 image exactly w x h with every row stored and a
// palette of its own: the header facts opaque_draw_rows relies on. The row
// data's opacity is checked once, when the image is loaded (the image store's
// require_opaque), not here.
bool opaque_image_fits(const void *img, int w, int h);

// Rows [y0, y0 + rows) of `img` into `dst`, which points at where row y0,
// column 0 goes; rows are `stride` pixels apart. `img` must pass
// opaque_image_fits() and check_image() as opaque.
void opaque_draw_rows(uint16_t *dst, int stride, const void *img, int y0, int rows);
