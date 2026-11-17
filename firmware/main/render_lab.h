#pragma once
#include <stddef.h>
#include <stdint.h>
#include "rle565.h"

// Render lab (wip/wip-on-device-render-feasibility.md): key looks drawn on
// the device from a few layers with rle565's renderer, then encoded to an
// opaque rle565 image with rle565's C encoder. Pure (host tested); compiled
// into the firmware only with -DCB_RENDER_LAB=ON. Experiment code: nothing
// outside the lab may depend on it.

enum : uint8_t
{
    RENDER_LAYER_IMAGE,     // rle565 image `data` at (x, y), `alpha`, optional `palette` override
    RENDER_LAYER_FILL,      // rect (x, y, w, h) in `color` (a slider or toggle value fill)
    RENDER_LAYER_TEXT,      // UTF-8 `text` in font `data`, `color`, `alpha`, in box (x, y, w, h):
                            // aligned by `align`, capitals centred vertically (not clipped to the box)
    RENDER_LAYER_MASK,      // 8-bit coverage `data` (w x h, stride w) at (x, y) in `color`, `alpha`
    RENDER_LAYER_PATH,      // path `data` (path_raster.h, checked) in its box (x, y, w, h): non-zero
                            // fill in `color`, `alpha` (rasterised a band at a time into the scratch)
};

enum : uint8_t
{
    RENDER_ALIGN_LEFT,
    RENDER_ALIGN_CENTER,
    RENDER_ALIGN_RIGHT,
};

struct RenderLayer
{
    uint8_t  type;          // RENDER_LAYER_*
    uint8_t  align;         // text: RENDER_ALIGN_*
    uint8_t  alpha;         // image, text, mask: overall opacity (255 = opaque)
    uint16_t color;         // fill, text, mask: RGB565
    int16_t  x, y;          // top left in the key image
    uint16_t w, h;          // fill, text box, mask size
    const void *data;       // image: rle565; text: FONT; mask: coverage bytes
    const char *text;       // text: NUL terminated UTF-8
    const PALETTE *palette; // image: prepared palette to draw with, NULL = its own
};

// Scratch RenderLabRender() needs for bands of up to `rows` rows (a multiple
// of 4; 0 if no layer needs any): a path layer's accumulator and mask for the
// rows of its box a band can hold, the largest of them
size_t RenderLabScratchSize(const RenderLayer *layers, int n, int rows);

// Draw rows y0..y0+rows-1 of a `w` wide look into `px` (w * rows pixels,
// stride w): black, then each layer in order. A whole image is y0 = 0,
// rows = h; any banding gives the same pixels (status_bar_render_rows' way:
// every draw is shifted up by y0 and clipped to the band). `scratch`
// (4-byte aligned) is for path layers. Returns false, drawing nothing, if
// scratch_size is under RenderLabScratchSize(layers, n, rows).
bool RenderLabRender(const RenderLayer *layers, int n, int w, int y0, int rows, uint16_t *px,
                     void *scratch = nullptr, size_t scratch_size = 0);

// Workspace RenderLabEncode() needs for `colors` distinct colours: the
// encoder's IMAGE_ENCODE_WORKSPACE_SIZE, but at least the 8 KB colour count
// bitmap (65536 bits) it first uses the same memory for
#define RENDER_LAB_COUNT_BYTES  (65536 / 8)
size_t RenderLabWorkspaceSize(uint32_t colors);

// Worst case encoded size of an opaque w x h image with `colors` distinct
// colours (an output buffer this size never fails)
size_t RenderLabEncodeBound(int w, int h, uint32_t colors);

struct RenderLabEncodeInfo
{
    uint32_t colors;        // distinct colours in the picture (0 if not counted)
    size_t   workspace;     // RenderLabWorkspaceSize(colors)
};

// Encode `px` (w x h, stride w, RGB565) to an opaque rle565 image in `out`.
// Counts the colours first (in `ws`, 4-byte aligned) and fills `info`.
// Returns the size, or 0 if `ws` is smaller than RENDER_LAB_COUNT_BYTES (no
// count) or than info->workspace, or `out` is too small. `out` NULL = size only.
size_t RenderLabEncode(const uint16_t *px, int w, int h, void *ws, size_t ws_size,
                       void *out, size_t out_size, RenderLabEncodeInfo *info);

// The cheap alternative to a render: recolour a baked image through its
// palette. `dst` (room for 1 + src->count words) gets `src`'s entries,
// recoloured by rle565's recolor_palette(colorA, colorB, channelShift):
// each entry's channel at `shift` (0 blue, 8 green, 16 red) interpolates
// from colorA to colorB (0xRRGGBB). In place is fine (dst == src).
void RenderLabRecolorPalette(const PALETTE *src, PALETTE *dst, uint32_t colorA, uint32_t colorB, int shift);

// Copy the rle565 image `src` to `dst` (4-byte aligned) with its own palette
// recoloured as RenderLabRecolorPalette: no render, no encode. Returns the
// size (src's), or 0 if dst_size is too small. `src` is untouched.
size_t RenderLabRecolorCopy(const void *src, void *dst, size_t dst_size, uint32_t colorA, uint32_t colorB, int shift);
