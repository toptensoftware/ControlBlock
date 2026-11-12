// Opaque rle565 row decoder (from the key render spike, wip/wip-key-render-redesign.md step 2).
// Format: rle565/src/rle565.h (row format) and rle565.core.h.

#include "rle565.h"
#include "fill_px.h"
#include "opaque_row.h"

static inline uint32_t read_varlen(const uint8_t *&p)
{
    uint32_t v = 0;
    uint8_t  b;
    do {
        b = *p++;
        v = (v << 7) | (b & 0x7F);
    } while (b & 0x80);
    return v;
}

// One whole row: a run count, then runs of `count << 1 | rle` (literal runs
// of images with DRAW_FLAGS_ALPHA_PIXELS carry an alpha bit below the rle
// bit); palette indices are bytes, or varlens with a large palette.
static void decode_row(uint16_t *dst, const uint8_t *p, const uint32_t *pal, bool large, bool alpha_bits)
{
    uint32_t runs = read_varlen(p);
    while (runs--) {
        uint32_t n = read_varlen(p);
        bool rle = n & 1;
        n >>= 1;
        if (rle) {
            uint32_t pi = large ? read_varlen(p) : *p++;
            fill_px(dst, (int)n, (uint16_t)pal[pi]);
            dst += n;
            continue;
        }
        if (alpha_bits)
            n >>= 1;
        if (large) {
            while (n--)
                *dst++ = (uint16_t)pal[read_varlen(p)];
            continue;
        }
        if (n && ((uintptr_t)dst & 2)) {
            *dst++ = (uint16_t)pal[*p++];
            n--;
        }
        uint32_t *d = (uint32_t *)dst;
        for (; n >= 2; n -= 2, p += 2)
            *d++ = (uint16_t)pal[p[0]] | ((uint32_t)(uint16_t)pal[p[1]] << 16);
        dst = (uint16_t *)d;
        if (n)
            *dst++ = (uint16_t)pal[*p++];
    }
}

bool opaque_image_fits(const void *img, int w, int h)
{
    const IMAGE_RLE *hd = (const IMAGE_RLE *)img;
    return img && hd->base.signature == IMAGE_SIGNATURE && hd->base.format == IMAGE_FORMAT_RLE &&
           (int)hd->base.width == w && (int)hd->base.height == h &&
           hd->firstRow == 0 && (int)hd->rowCount == h && hd->paletteOffset != 0;
}

void opaque_draw_rows(uint16_t *dst, int stride, const void *img, int y0, int rows)
{
    const IMAGE_RLE *hd      = (const IMAGE_RLE *)img;
    const uint8_t   *base    = (const uint8_t *)img;
    const uint32_t  *offsets = (const uint32_t *)(hd + 1);
    const PALETTE   *pal     = (const PALETTE *)(base + hd->paletteOffset);
    bool large = (hd->base.flags & DRAW_FLAGS_LARGE_PALETTE) != 0;
    bool alpha = (hd->base.flags & DRAW_FLAGS_ALPHA_PIXELS) != 0;
    for (int r = 0; r < rows; r++, dst += stride)
        decode_row(dst, base + offsets[y0 + r], pal->colors, large, alpha);
}
