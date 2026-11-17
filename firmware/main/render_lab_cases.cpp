#include <stdio.h>
#include <string.h>
#include "key_geometry.h"
#include "path_raster.h"
#include "render_lab_assets.h"
#include "render_lab_cases.h"

// ---------------------------------------------------------------------------
// Path vector file

#define VECTORS_MAGIC   0x43455650u     // "PVEC"
#define VECTORS_VERSION 1
#define VECTORS_HEAD    8
#define ENTRY_HEAD      12

static size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }

// The entry count of a file with a good header, or -1
static int vectors_count(const uint8_t *b, size_t size)
{
    if (!b || size < VECTORS_HEAD || u32(b) != VECTORS_MAGIC || u16(b + 4) != VECTORS_VERSION)
        return -1;
    return u16(b + 6);
}

// Read the entry at *pos into `v` and step past it; false if it runs past the end
static bool vectors_next(const uint8_t *b, size_t size, size_t *pos, RenderLabVector *v)
{
    size_t p = *pos;
    if (size - p < ENTRY_HEAD)
        return false;
    *v = {};
    v->w = u16(b + p);
    v->h = u16(b + p + 2);
    size_t name_len = u16(b + p + 4);
    v->x = (int16_t)u16(b + p + 6);
    v->path_size = u32(b + p + 8);
    p += ENTRY_HEAD;
    size_t mask = (size_t)v->w * v->h;
    if (name_len >= sizeof(v->name) || size - p < pad4(name_len))
        return false;
    memcpy(v->name, b + p, name_len);
    p += pad4(name_len);
    if (size - p < pad4(v->path_size))
        return false;
    v->path = b + p;
    p += pad4(v->path_size);
    if (size - p < pad4(mask))
        return false;
    v->mask = b + p;
    *pos = p + pad4(mask);
    return true;
}

int RenderLabVectorsRead(const void *file, size_t size, RenderLabVector *out, int max)
{
    const uint8_t *b = (const uint8_t *)file;
    int count = vectors_count(b, size);
    if (count < 0 || count > max)
        return -1;
    size_t pos = VECTORS_HEAD;
    for (int i = 0; i < count; i++)
        if (!vectors_next(b, size, &pos, &out[i]))
            return -1;
    return pos == size ? count : -1;
}

// The vector called `name` (a checked path), or false
static bool find_vector(const RenderLabAssets *a, const char *name, RenderLabVector *v)
{
    const uint8_t *b = (const uint8_t *)a->vectors;
    int count = vectors_count(b, a->vectors_size);
    size_t pos = VECTORS_HEAD;
    for (int i = 0; i < count; i++) {
        if (!vectors_next(b, a->vectors_size, &pos, v))
            return false;
        if (strcmp(v->name, name) == 0)
            return PathCheck(v->path, v->path_size, nullptr);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Cases

#define FACE_M          ((KEY_CELL - RENDER_LAB_FACE_SIZE) / 2)     // the face's margin on a 1U key
#define WHITE           0xFFFF
#define RECOLOR_A       0x102040        // the recolour cases: grey face to blue, on the green channel
#define RECOLOR_B       0x60A0FF
#define RECOLOR_SHIFT   8
#define GLYPH_RUN       "55%"

static RenderLayer image_layer(const void *img, int x, int y)
{
    RenderLayer l = {};
    l.type  = RENDER_LAYER_IMAGE;
    l.x = (int16_t)x; l.y = (int16_t)y;
    l.alpha = 255;
    l.data  = img;
    return l;
}

static RenderLayer fill_layer(int x, int y, int w, int h, uint16_t color)
{
    RenderLayer l = {};
    l.type  = RENDER_LAYER_FILL;
    l.x = (int16_t)x; l.y = (int16_t)y; l.w = (uint16_t)w; l.h = (uint16_t)h;
    l.color = color;
    return l;
}

static RenderLayer text_layer(const void *font, const char *text, int x, int y, int w, int h, uint8_t align)
{
    RenderLayer l = {};
    l.type  = RENDER_LAYER_TEXT;
    l.x = (int16_t)x; l.y = (int16_t)y; l.w = (uint16_t)w; l.h = (uint16_t)h;
    l.color = WHITE;
    l.alpha = 255;
    l.align = align;
    l.data  = font;
    l.text  = text;
    return l;
}

static RenderLayer path_layer(const RenderLabVector &v, int x, int y)
{
    RenderLayer l = {};
    l.type  = RENDER_LAYER_PATH;
    l.x = (int16_t)x; l.y = (int16_t)y; l.w = (uint16_t)v.w; l.h = (uint16_t)v.h;
    l.color = WHITE;
    l.alpha = 255;
    l.data  = v.path;
    return l;
}

// The 1U label: a box along the bottom of the face
static RenderLayer label(const void *font, const char *text)
{
    return text_layer(font, text, FACE_M, 96, RENDER_LAB_FACE_SIZE, 40, RENDER_ALIGN_CENTER);
}

int RenderLabCaseCount()
{
    return RENDER_LAB_CASE_COUNT;
}

static void add(RenderLabCase *c, const RenderLayer &l)
{
    c->layers[c->n++] = l;
}

bool RenderLabCaseGet(int i, const RenderLabAssets *a, RenderLabCase *c)
{
    if (i < 0 || i >= RENDER_LAB_CASE_COUNT || !a->lab || a->lab->count < RENDER_LAB_ASSET_COUNT)
        return false;
    memset(c, 0, sizeof(*c));
    c->kind = RENDER_CASE_RENDER;
    c->w = KEY_CELL;
    c->h = KEY_CELL;
    c->source = -1;
    c->color_a = RECOLOR_A;
    c->color_b = RECOLOR_B;
    c->shift = RECOLOR_SHIFT;

    const void *font20 = get_blob(a->lab, RENDER_LAB_ASSET_FONT_20);
    const void *font27 = get_blob(a->lab, RENDER_LAB_ASSET_FONT_27);
    const void *font27sp4 = get_blob(a->lab, RENDER_LAB_ASSET_FONT_27_SP4);
    const void *face = get_blob(a->lab, RENDER_LAB_ASSET_FACE);
    const void *icon = get_blob(a->lab, RENDER_LAB_ASSET_ICON);
    const RenderLayer face1u = image_layer(face, FACE_M, FACE_M);
    const int icon_x = (KEY_CELL - RENDER_LAB_ICON_SIZE) / 2, icon_y = 14;

    switch (i) {
    case RENDER_LAB_CASE_FACE_LABEL:
        c->name = "1U face + label 20";
        add(c, face1u);
        add(c, label(font20, "Mute"));
        break;

    case RENDER_LAB_CASE_FACE_LABEL_SP4:
        c->name = "1U face + label 27 sp4";
        add(c, face1u);
        add(c, label(font27sp4, "Mute"));
        break;

    case RENDER_LAB_CASE_RECOLOR_LABEL: {
        // The face drawn through a recoloured copy of its palette
        c->name = "1U recoloured face + label";
        const PALETTE *pal = get_image_palette(face);
        if (pal->count > RENDER_LAB_CASE_PALETTE)
            return false;
        RenderLabRecolorPalette(pal, (PALETTE *)c->palette, c->color_a, c->color_b, c->shift);
        add(c, face1u);
        c->layers[0].palette = (const PALETTE *)c->palette;
        add(c, label(font20, "Mute"));
        break;
    }

    case RENDER_LAB_CASE_SLIDER:
        // Face, a value track and its fill, the value
        c->name = "1U slider";
        add(c, face1u);
        add(c, fill_layer(24, 104, 96, 10, 0x2104));
        add(c, fill_layer(24, 104, 53, 10, 0x05FF));
        add(c, text_layer(font27, "55%", FACE_M, 30, RENDER_LAB_FACE_SIZE, 48, RENDER_ALIGN_CENTER));
        break;

    case RENDER_LAB_CASE_ICON_LABEL:
        // The baked (rle565) icon: the path icon's baseline
        c->name = "1U image icon + label";
        add(c, face1u);
        add(c, image_layer(icon, icon_x, icon_y));
        add(c, label(font20, "Volume"));
        break;

    case RENDER_LAB_CASE_PATH_ICON_LABEL: {
        c->name = "1U path icon + label";
        RenderLabVector v;
        if (!find_vector(a, "icon-volume-up", &v))
            return false;
        add(c, face1u);
        add(c, path_layer(v, icon_x, icon_y));
        add(c, label(font20, "Volume"));
        break;
    }

    case RENDER_LAB_CASE_GLYPHS: {
        // 27 px text from glyph outlines: a path layer a glyph, at its place in the run
        c->name = "1U glyphs from outlines";
        RenderLabVector run;
        if (!find_vector(a, "glyphs-" GLYPH_RUN, &run))
            return false;
        add(c, face1u);
        int ox = (KEY_CELL - run.w) / 2, oy = 35;
        for (int g = 0; ; g++) {
            char name[sizeof(run.name)];
            static const char prefix[] = "glyph-" GLYPH_RUN "-";
            memcpy(name, prefix, sizeof(prefix) - 1);
            name[sizeof(prefix) - 1] = (char)('0' + g);
            name[sizeof(prefix)] = 0;
            RenderLabVector v;
            if (g > 9 || !find_vector(a, name, &v))
                break;
            if (c->n == RENDER_LAB_CASE_LAYERS)
                return false;
            add(c, path_layer(v, ox + v.x, oy));
        }
        if (c->n < 2)
            return false;
        break;
    }

    case RENDER_LAB_CASE_2U:
        c->name = "2U faces + text 27";
        c->w = 2 * KEY_CELL;
        add(c, face1u);
        add(c, image_layer(face, KEY_CELL + FACE_M, FACE_M));
        add(c, image_layer(icon, 24, icon_y));
        // The label on the second face (the text layer clips to the image, not its box,
        // so it must fit the face: a right-aligned box over both faces ran into the gap)
        add(c, text_layer(font27, "Wide", KEY_CELL + FACE_M, 46, RENDER_LAB_FACE_SIZE, 40, RENDER_ALIGN_CENTER));
        break;

    case RENDER_LAB_CASE_FULL:
        // The largest image: a panel, a face, a value bar and text
        c->name = "720x432 face + text + fill";
        c->w = KEY_GRID_W;
        c->h = KEY_GRID_H;
        add(c, fill_layer(8, 8, KEY_GRID_W - 16, KEY_GRID_H - 16, 0x18E3));
        add(c, image_layer(face, (KEY_GRID_W - RENDER_LAB_FACE_SIZE) / 2, 150));
        add(c, fill_layer(40, 360, KEY_GRID_W - 80, 24, 0x2104));
        add(c, fill_layer(40, 360, (KEY_GRID_W - 80) * 55 / 100, 24, 0x05FF));
        add(c, text_layer(font27, "Living room volume 55%", 0, 40, KEY_GRID_W, 60, RENDER_ALIGN_CENTER));
        break;

    case RENDER_LAB_CASE_RECOLOR_COPY:
        c->name = "1U recolour copy (no render)";
        c->kind = RENDER_CASE_RECOLOR;
        c->source = RENDER_LAB_CASE_FACE_LABEL;
        break;

    case RENDER_LAB_CASE_LARGE_PALETTE: {
        // The face at half alpha over six coloured stripes: each stripe gets its own copy of the
        // face's shades, well over 256 colours (the encoder's varlen indices, opaque_draw_rows'
        // per-pixel decode)
        c->name = "1U large palette >256";
        static const uint16_t STRIPES[] = { 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF };
        const int n = (int)(sizeof(STRIPES) / sizeof(STRIPES[0])), sw = KEY_CELL / n;
        for (int s = 0; s < n; s++)
            add(c, fill_layer(s * sw, 0, sw, KEY_CELL, STRIPES[s]));
        RenderLayer f = face1u;
        f.alpha = 128;
        add(c, f);
        add(c, label(font20, "Colours"));
        break;
    }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Runner

void RenderLabCaseSizes(const RenderLabCase *c, int band_rows, RenderLabResult *r)
{
    *r = {};
    r->px_whole = (size_t)c->w * c->h * 2;
    r->scratch_whole = RenderLabScratchSize(c->layers, c->n, c->h);
    if (band_rows > 0) {
        r->px_band = (size_t)c->w * band_rows * 2;
        r->scratch_band = RenderLabScratchSize(c->layers, c->n, band_rows);
    }
}

static uint64_t now(RenderLabClock clock)
{
    return clock ? clock() : 0;
}

bool RenderLabCaseRun(const RenderLabCase *c, const RenderLabBuffers *b, const void *source,
                      RenderLabClock clock, int reps, RenderLabResult *r)
{
    RenderLabCaseSizes(c, b->band_rows, r);
    if (reps < 1)
        reps = 1;

    if (c->kind == RENDER_CASE_RECOLOR) {
        if (!source || !b->out)
            return false;
        uint64_t t0 = now(clock);
        size_t size = 0;
        for (int i = 0; i < reps; i++)
            if (!(size = RenderLabRecolorCopy(source, b->out, b->out_size, c->color_a, c->color_b, c->shift)))
                return false;
        r->render_ns = (now(clock) - t0) / reps;
        r->colors = get_image_palette(source)->count;
        r->size = r->bound = size;
        return true;
    }

    if (!b->px || b->px_bytes < r->px_whole)
        return false;

    // Whole
    uint64_t t0 = now(clock);
    for (int i = 0; i < reps; i++)
        if (!RenderLabRender(c->layers, c->n, c->w, 0, c->h, b->px, b->scratch, b->scratch_size))
            return false;
    uint64_t t1 = now(clock);
    r->render_ns = (t1 - t0) / reps;

    // Banded: the same picture, a band at a time
    if (b->band_rows > 0) {
        for (int i = 0; i < reps; i++)
            for (int y0 = 0; y0 < c->h; y0 += b->band_rows) {
                int rows = c->h - y0 < b->band_rows ? c->h - y0 : b->band_rows;
                if (!RenderLabRender(c->layers, c->n, c->w, y0, rows, b->px + (size_t)y0 * c->w,
                                     b->scratch, b->scratch_size))
                    return false;
            }
        r->band_ns = (now(clock) - t1) / reps;
    }

    // Encode (counts the colours first)
    RenderLabEncodeInfo info = {};
    size_t size = 0;
    uint64_t t2 = now(clock);
    for (int i = 0; i < reps; i++)
        if (!(size = RenderLabEncode(b->px, c->w, c->h, b->ws, b->ws_size, b->out, b->out_size, &info)) || !b->out)
            break;
    r->encode_ns = (now(clock) - t2) / reps;
    r->colors = info.colors;
    r->workspace = info.workspace;
    r->bound = info.colors ? RenderLabEncodeBound(c->w, c->h, info.colors) : 0;
    if (!size || !b->out)
        return false;
    r->size = size;
    return true;
}

int RenderLabRepsFor(uint64_t once_ns, uint64_t target_ns, int max)
{
    uint64_t reps = once_ns ? target_ns / once_ns : (uint64_t)max;
    if (reps > (uint64_t)max)
        reps = (uint64_t)max;
    return reps < 3 ? 3 : (int)reps;
}

size_t RenderLabFormatResult(const RenderLabCase *c, const RenderLabResult *r, char *buf, size_t size)
{
    char dims[16];
    snprintf(dims, sizeof(dims), "%dx%d", c->w, c->h);
    int n = snprintf(buf, size,
                     "%-28s %-7s | render %.1f us, banded %.1f us, encode %.1f us | %u colours, %u B "
                     "(bound %u), ws %u B | 565 %.1f/%.1f KB, path %.1f/%.1f KB",
                     c->name, dims, r->render_ns / 1000.0, r->band_ns / 1000.0, r->encode_ns / 1000.0,
                     (unsigned)r->colors, (unsigned)r->size, (unsigned)r->bound, (unsigned)r->workspace,
                     r->px_whole / 1024.0, r->px_band / 1024.0, r->scratch_whole / 1024.0, r->scratch_band / 1024.0);
    return n < 0 ? 0 : (size_t)n;
}

// ---------------------------------------------------------------------------
// Showing results as keys

bool RenderLabPlaceKey(int w, int h, int *cursor, KeyRect *out)
{
    if (w <= 0 || h <= 0 || w % KEY_CELL || h % KEY_CELL)
        return false;
    int cw = w / KEY_CELL, ch = h / KEY_CELL;
    int row = *cursor / KEY_GRID_COLS, col = *cursor % KEY_GRID_COLS;
    if (cw > KEY_GRID_COLS)
        return false;
    if (col + cw > KEY_GRID_COLS) {
        row++;
        col = 0;
    }
    if (row + ch > KEY_GRID_ROWS)
        return false;
    *out = KeyRect{ (uint8_t)(col * 4), (uint8_t)(row * 4), (uint8_t)(cw * 4), (uint8_t)(ch * 4) };
    *cursor = ch > 1 ? (row + ch) * KEY_GRID_COLS : row * KEY_GRID_COLS + col + cw;
    return true;
}
