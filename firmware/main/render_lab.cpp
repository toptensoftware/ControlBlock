#include <string.h>
#include "path_raster.h"
#include "render_lab.h"
#include "text_layout.h"

// ---------------------------------------------------------------------------
// Render

// Rows of a path layer's box a band of `rows` can hold, and the scratch they need
static int path_rows(const RenderLayer *l, int rows)
{
    return l->h < rows ? l->h : rows;
}

static size_t path_scratch(const RenderLayer *l, int rows)
{
    int r = path_rows(l, rows);
    if (l->w == 0 || r <= 0)
        return 0;
    return (PathRasterScratchSize(l->w, r) + (size_t)l->w * r + 3) & ~(size_t)3;
}

// A path layer's rows inside the band y0..y0+rows: rasterised into the
// scratch's mask (after the accumulator), then blended in its colour
static void draw_path(IMAGE_BUFFER *buf, const RenderLayer *l, int y0, int rows, void *scratch)
{
    int a = l->y > y0 ? l->y : y0;
    int b = l->y + l->h < y0 + rows ? l->y + l->h : y0 + rows;
    if (l->w == 0 || a >= b)
        return;
    size_t acc = PathRasterScratchSize(l->w, b - a);
    uint8_t *mask = (uint8_t *)scratch + acc;
    PathRaster(PathOpen(l->data), l->w, a - l->y, b - a, scratch, acc, mask);
    draw_alpha_mask(buf, l->x, a - y0, mask, l->w, l->w, b - a, l->color, l->alpha);
}

static void draw_layer(IMAGE_BUFFER *buf, const RenderLayer *l, int y0)
{
    int y = l->y - y0;              // every draw shifted up by the band's top
    switch (l->type) {
    case RENDER_LAYER_IMAGE: {
        int iw = (int)get_image_width(l->data), ih = (int)get_image_height(l->data);
        draw_image(buf, l->data, l->x, y, 0, 0, iw, ih, NULL, l->alpha, l->palette);
        break;
    }
    case RENDER_LAYER_FILL:
        fill_rect(buf, l->x, y, l->w, l->h, l->color);
        break;
    case RENDER_LAYER_TEXT: {
        const FONT *font = (const FONT *)l->data;
        int x = l->x;
        if (l->align != RENDER_ALIGN_LEFT) {
            int slack = l->w - font_measure_text(font, l->text, -1);
            x += l->align == RENDER_ALIGN_RIGHT ? slack : slack / 2;
        }
        draw_text(buf, font, x, y + font_centre_baseline(font, l->h), l->text, -1, l->color, l->alpha);
        break;
    }
    case RENDER_LAYER_MASK:
        draw_alpha_mask(buf, l->x, y, (const uint8_t *)l->data, l->w, l->w, l->h, l->color, l->alpha);
        break;
    }
}

size_t RenderLabScratchSize(const RenderLayer *layers, int n, int rows)
{
    size_t most = 0;
    for (int i = 0; i < n; i++) {
        size_t s = layers[i].type == RENDER_LAYER_PATH ? path_scratch(&layers[i], rows) : 0;
        most = s > most ? s : most;
    }
    return most;
}

bool RenderLabRender(const RenderLayer *layers, int n, int w, int y0, int rows, uint16_t *px,
                     void *scratch, size_t scratch_size)
{
    size_t need = RenderLabScratchSize(layers, n, rows);
    if (need && (!scratch || scratch_size < need))
        return false;
    IMAGE_BUFFER buf;
    init_image_buffer(&buf, w, rows, px, w);
    fill_rect(&buf, 0, 0, w, rows, 0);
    for (int i = 0; i < n; i++) {
        if (layers[i].type == RENDER_LAYER_PATH)
            draw_path(&buf, &layers[i], y0, rows, scratch);
        else
            draw_layer(&buf, &layers[i], y0);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Encode

size_t RenderLabWorkspaceSize(uint32_t colors)
{
    size_t enc = IMAGE_ENCODE_WORKSPACE_SIZE(colors);
    return enc > RENDER_LAB_COUNT_BYTES ? enc : RENDER_LAB_COUNT_BYTES;
}

// Bytes put_varlen writes for v (7 bits a byte)
static size_t varlen_size(uint32_t v)
{
    size_t n = 1;
    while (v >>= 7)
        n++;
    return n;
}

size_t RenderLabEncodeBound(int w, int h, uint32_t colors)
{
    // Header, row table, palette
    size_t fixed = sizeof(IMAGE_RLE) + 4 * (size_t)h + 4 + 4 * (size_t)colors;

    // A row of an opaque image (rle565enc.c encode_row): runs of >= 3 pixels
    // are RLE (header + one index); shorter runs coalesce into literal groups
    // (header + an index a pixel), and two groups never touch. A run of 3
    // costs at most 1 + isz <= 3 * isz - 1, which pays for the header of the
    // group before it, so a row costs at most an index a pixel, plus one
    // header for a group at the row's end, plus the extra header bytes of
    // runs of 64+ pixels (at most w / 64 of them), plus the run count.
    size_t isz = colors <= 256 ? 1 : varlen_size(colors - 1);
    size_t row = varlen_size((uint32_t)w)
               + (size_t)w * isz
               + 1
               + (size_t)(w / 64) * (varlen_size(((uint32_t)w << 1) | 1) - 1);
    return fixed + (size_t)h * row;
}

// Distinct colours in `px`, counted in a 65536-bit bitmap
static uint32_t count_colors(const uint16_t *px, int w, int h, uint32_t *bits)
{
    memset(bits, 0, RENDER_LAB_COUNT_BYTES);
    uint32_t colors = 0;
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
        uint32_t c = px[i], bit = 1u << (c & 31);
        if (!(bits[c >> 5] & bit)) {
            bits[c >> 5] |= bit;
            colors++;
        }
    }
    return colors;
}

size_t RenderLabEncode(const uint16_t *px, int w, int h, void *ws, size_t ws_size,
                       void *out, size_t out_size, RenderLabEncodeInfo *info)
{
    *info = {};
    if (!ws || ws_size < RENDER_LAB_COUNT_BYTES || w <= 0 || h <= 0)
        return 0;
    info->colors    = count_colors(px, w, h, (uint32_t *)ws);
    info->workspace = RenderLabWorkspaceSize(info->colors);
    if (ws_size < info->workspace)
        return 0;
    return encode_image_rgb565(px, (uint32_t)w, (uint32_t)h, 0, ws, ws_size, out, out_size);
}

// ---------------------------------------------------------------------------
// Recolour copy

void RenderLabRecolorPalette(const PALETTE *src, PALETTE *dst, uint32_t colorA, uint32_t colorB, int shift)
{
    if (dst != src)
        memcpy(dst, src, sizeof(uint32_t) * (1 + src->count));
    recolor_palette(dst, colorA, colorB, shift);
}

size_t RenderLabRecolorCopy(const void *src, void *dst, size_t dst_size, uint32_t colorA, uint32_t colorB, int shift)
{
    size_t size = get_image_packed_size(src);
    if (dst_size < size)
        return 0;
    memcpy(dst, src, size);
    PALETTE *pal = (PALETTE *)get_image_palette(dst);
    RenderLabRecolorPalette(pal, pal, colorA, colorB, shift);
    return size;
}
