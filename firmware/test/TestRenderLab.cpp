// Render lab core (render_lab.cpp): key looks drawn on the device from
// layers, encoded to opaque rle565. wip/wip-on-device-render-feasibility.md.

#include <stdlib.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"
#include "key_geometry.h"
#include "opaque_row.h"
#include "path_raster.h"
#include "path_vectors.h"
#include "render_lab.h"
#include "render_lab_assets.h"
#include "render_lab_cases.h"
#include "text_layout.h"
#include "ui_assets.h"

using namespace SimpleLib;

namespace
{
    #define POISON      0x5A5A

    // Heap scratch of exactly `count` T (the debug CRT catches an overrun on free)
    template <typename T>
    struct Scratch
    {
        explicit Scratch(size_t count) : n(count), p((T *)malloc(count * sizeof(T))) { Assert(p != nullptr); }
        ~Scratch() { free(p); }
        Scratch(const Scratch &) = delete;
        Scratch &operator=(const Scratch &) = delete;
        size_t n;
        T *p;
    };

    const FONT *UiFont()
    {
        static BLOBPACK *ui = LoadAsset("ui.bin");
        return (const FONT *)get_blob(ui, UI_ASSET_STATUS_FONT);
    }

    // Render rows y0..y0+rows with scratch of exactly the size asked for
    void Render(const RenderLayer *layers, int n, int w, int y0, int rows, uint16_t *px)
    {
        size_t need = RenderLabScratchSize(layers, n, rows);
        Scratch<uint32_t> scratch(need / 4);
        Assert(need % 4 == 0);
        Assert(RenderLabRender(layers, n, w, y0, rows, px, need ? scratch.p : nullptr, need));
    }

    // A whole render of `n` layers at w x h, poisoned first so every pixel must be drawn
    void RenderWhole(const RenderLayer *layers, int n, int w, int h, uint16_t *px)
    {
        for (int i = 0; i < w * h; i++)
            px[i] = POISON;
        Render(layers, n, w, 0, h, px);
    }

    // The same picture in bands of `band` rows equals the whole render
    void CheckBanded(const RenderLayer *layers, int n, int w, int h, const uint16_t *whole, int band)
    {
        Scratch<uint16_t> strip((size_t)w * band);
        for (int y0 = 0; y0 < h; y0 += band) {
            int rows = h - y0 < band ? h - y0 : band;
            for (size_t i = 0; i < strip.n; i++)
                strip.p[i] = POISON;
            Render(layers, n, w, y0, rows, strip.p);
            Assert(memcmp(strip.p, whole + (size_t)y0 * w, (size_t)w * rows * 2) == 0);
        }
    }

    // Encode `px`, check it as the image store would, draw it back, check the bound
    void CheckEncode(const uint16_t *px, int w, int h)
    {
        // No workspace: nothing counted
        RenderLabEncodeInfo info = {};
        Assert(RenderLabEncode(px, w, h, nullptr, 0, nullptr, 0, &info) == 0);
        Assert(info.colors == 0);

        // The count bitmap alone reports what's needed
        {
            Scratch<uint32_t> count(RENDER_LAB_COUNT_BYTES / 4);
            size_t dry = RenderLabEncode(px, w, h, count.p, RENDER_LAB_COUNT_BYTES, nullptr, 0, &info);
            Assert(info.colors > 0);
            Assert(info.workspace == RenderLabWorkspaceSize(info.colors));
            Assert((dry != 0) == (info.workspace == RENDER_LAB_COUNT_BYTES));
        }

        Scratch<uint32_t> ws((info.workspace + 3) / 4);
        size_t bound = RenderLabEncodeBound(w, h, info.colors);
        Scratch<uint32_t> out((bound + 3) / 4);
        size_t size = RenderLabEncode(px, w, h, ws.p, info.workspace, out.p, bound, &info);
        Assert(size > 0);
        Assert(size <= bound);
        Assert(get_image_palette(out.p)->count == info.colors);

        bool opaque = false;
        Assert(check_image(out.p, size, &opaque));
        Assert(opaque);
        Assert(opaque_image_fits(out.p, w, h));

        Scratch<uint16_t> back((size_t)w * h);
        for (size_t i = 0; i < back.n; i++)
            back.p[i] = POISON;
        opaque_draw_rows(back.p, w, out.p, 0, h);
        Assert(memcmp(back.p, px, (size_t)w * h * 2) == 0);
    }

    // Every check for one look: banded == whole (1, 7, 16 rows), encode round trip
    void CheckLook(const RenderLayer *layers, int n, int w, int h, uint16_t *whole)
    {
        RenderWhole(layers, n, w, h, whole);
        for (int band : { 1, 7, 16 })
            CheckBanded(layers, n, w, h, whole, band);
        CheckEncode(whole, w, h);
    }

    RenderLayer ImageLayer(const void *img, int x, int y)
    {
        RenderLayer l = {};
        l.type  = RENDER_LAYER_IMAGE;
        l.x     = x;
        l.y     = y;
        l.alpha = 255;
        l.data  = img;
        return l;
    }

    RenderLayer FillLayer(int x, int y, int w, int h, uint16_t color)
    {
        RenderLayer l = {};
        l.type  = RENDER_LAYER_FILL;
        l.x = x; l.y = y; l.w = w; l.h = h;
        l.color = color;
        return l;
    }

    RenderLayer TextLayer(const FONT *font, const char *text, int x, int y, int w, int h, uint8_t align, uint16_t color)
    {
        RenderLayer l = {};
        l.type  = RENDER_LAYER_TEXT;
        l.x = x; l.y = y; l.w = w; l.h = h;
        l.color = color;
        l.alpha = 255;
        l.align = align;
        l.data  = font;
        l.text  = text;
        return l;
    }

    // The 128 px alpha face centred on a 1U key (8 px margin)
    RenderLayer FaceLayer(int i)
    {
        return ImageLayer(TestImages::Get().Face(i), (KEY_CELL - 128) / 2, (KEY_CELL - 128) / 2);
    }
}

Fact("Render Lab Empty Look Is Black")
{
    Scratch<uint16_t> px(KEY_CELL * KEY_CELL);
    CheckLook(nullptr, 0, KEY_CELL, KEY_CELL, px.p);
    for (size_t i = 0; i < px.n; i++)
        Assert(px.p[i] == 0);
}

Fact("Render Lab Fill Sets Exactly Its Rect")
{
    const int w = KEY_CELL, h = KEY_CELL;
    const uint16_t c = 0xF81F;
    RenderLayer layers[] = { FillLayer(10, 20, 50, 30, c) };
    Scratch<uint16_t> px(w * h);
    CheckLook(layers, 1, w, h, px.p);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool in = x >= 10 && x < 60 && y >= 20 && y < 50;
            Assert(px.p[y * w + x] == (in ? c : 0));
        }

    // Partly off the image: clipped
    RenderLayer off[] = { FillLayer(-5, h - 3, 10, 10, c) };
    CheckLook(off, 1, w, h, px.p);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            Assert(px.p[y * w + x] == ((x < 5 && y >= h - 3) ? c : 0));
}

Fact("Render Lab Text Equals A Direct draw_text")
{
    const int w = KEY_CELL, h = KEY_CELL;
    const FONT *font = UiFont();
    const char *text = "Vol+ Ag";
    const int bx = 8, by = 90, bw = 128, bh = 40;
    Scratch<uint16_t> px(w * h);
    Scratch<uint16_t> direct(w * h);

    for (uint8_t align : { RENDER_ALIGN_LEFT, RENDER_ALIGN_CENTER, RENDER_ALIGN_RIGHT }) {
        RenderLayer layers[] = { TextLayer(font, text, bx, by, bw, bh, align, 0xFFE0) };
        CheckLook(layers, 1, w, h, px.p);

        int tw = font_measure_text(font, text, -1);
        int x = align == RENDER_ALIGN_LEFT ? bx : align == RENDER_ALIGN_RIGHT ? bx + bw - tw : bx + (bw - tw) / 2;
        memset(direct.p, 0, direct.n * 2);
        IMAGE_BUFFER buf;
        init_image_buffer(&buf, w, h, direct.p, w);
        draw_text(&buf, font, x, by + font_centre_baseline(font, bh), text, -1, 0xFFE0, 255);
        Assert(memcmp(px.p, direct.p, direct.n * 2) == 0);

        bool any = false;
        for (size_t i = 0; i < px.n; i++)
            any |= px.p[i] != 0;
        Assert(any);
    }
}

Fact("Render Lab Mask Equals A Direct draw_alpha_mask")
{
    const int w = KEY_CELL, h = KEY_CELL, mw = 37, mh = 23;
    Scratch<uint8_t> mask(mw * mh);
    for (int i = 0; i < mw * mh; i++)
        mask.p[i] = (uint8_t)(i * 7);

    RenderLayer l = {};
    l.type = RENDER_LAYER_MASK;
    l.x = 100; l.y = -4; l.w = mw; l.h = mh;           // partly off the right and top
    l.color = 0x07E0;
    l.alpha = 200;
    l.data = mask.p;
    Scratch<uint16_t> px(w * h);
    CheckLook(&l, 1, w, h, px.p);

    Scratch<uint16_t> direct(w * h);
    memset(direct.p, 0, direct.n * 2);
    IMAGE_BUFFER buf;
    init_image_buffer(&buf, w, h, direct.p, w);
    draw_alpha_mask(&buf, 100, -4, mask.p, mw, mw, mh, 0x07E0, 200);
    Assert(memcmp(px.p, direct.p, direct.n * 2) == 0);
}

Fact("Render Lab Palette Override Recolours")
{
    // An opaque key drawn with a palette of one colour everywhere is that colour
    TestImages &ti = TestImages::Get();
    const void *key = ti.Key1U(0);
    uint32_t n = get_image_palette(key)->count;
    Scratch<uint32_t> pal(1 + n);
    pal.p[0] = n;
    for (uint32_t i = 0; i < n; i++)
        pal.p[1 + i] = 0xFF000000 | 0x001F;             // prepared opaque: 0xFF00 + rgb565

    RenderLayer layers[] = { ImageLayer(key, 0, 0) };
    layers[0].palette = (const PALETTE *)pal.p;
    Scratch<uint16_t> px(KEY_CELL * KEY_CELL);
    CheckLook(layers, 1, KEY_CELL, KEY_CELL, px.p);
    for (size_t i = 0; i < px.n; i++)
        Assert(px.p[i] == 0x001F);
}

Fact("Render Lab Typical Looks Pass Every Check")
{
    TestImages &ti = TestImages::Get();
    const FONT *font = UiFont();

    // 1U: face + label
    {
        RenderLayer layers[] = { FaceLayer(0), TextLayer(font, "Mute", 8, 96, 128, 40, RENDER_ALIGN_CENTER, 0xFFFF) };
        Scratch<uint16_t> px(KEY_CELL * KEY_CELL);
        CheckLook(layers, 2, KEY_CELL, KEY_CELL, px.p);
    }
    // 1U slider: face + value fill + value text (faded face)
    {
        RenderLayer layers[] = {
            FaceLayer(1),
            FillLayer(16, 100, 70, 12, 0x05FF),
            TextLayer(font, "55%", 8, 30, 128, 40, RENDER_ALIGN_CENTER, 0xFFFF),
        };
        layers[0].alpha = 160;
        Scratch<uint16_t> px(KEY_CELL * KEY_CELL);
        CheckLook(layers, 3, KEY_CELL, KEY_CELL, px.p);
    }
    // 2U: two faces, an icon and text
    {
        const int w = 2 * KEY_CELL, h = KEY_CELL;
        const void *icon = ti.Icon(0);
        RenderLayer layers[] = {
            FaceLayer(0), FaceLayer(2),
            ImageLayer(icon, 20, 20),
            TextLayer(font, "Wide key, right", 0, 100, w - 16, 30, RENDER_ALIGN_RIGHT, 0xFFFF),
        };
        layers[1].x += KEY_CELL;
        Scratch<uint16_t> px(w * h);
        CheckLook(layers, 4, w, h, px.p);
    }
}

Fact("Render Lab Encode Bound Holds For Hostile Pictures")
{
    // Patterns that cost the most per pixel: lone pixels between runs of 3,
    // alternating short groups and long runs, noise, a large palette
    const int w = KEY_CELL, h = 40;
    Scratch<uint16_t> px(w * h);
    Rng rng(7);
    for (int pattern = 0; pattern < 5; pattern++) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint16_t c;
                switch (pattern) {
                case 0:  c = (uint16_t)((x % 4 == 0) ? x * 3 + 1 : (x / 4) * 3 + 2); break;   // 1 + 3
                case 1:  c = (uint16_t)((x % 5 < 2) ? x + 1 : 1000 + x / 5); break;          // 2 + 3
                case 2:  c = (uint16_t)(rng.Next() % 256); break;                           // noise, small palette
                case 3:  c = (uint16_t)(rng.Next() % 3000); break;                          // noise, large palette
                default: c = (uint16_t)((x / 70) * 9 + y); break;                           // long runs
                }
                px.p[y * w + x] = c;
            }
        CheckEncode(px.p, w, h);
    }
}

Fact("Render Lab Encode Refuses A Small Output")
{
    TestImages &ti = TestImages::Get();
    RenderLayer layers[] = { FaceLayer(0) };
    Scratch<uint16_t> px(KEY_CELL * KEY_CELL);
    RenderWhole(layers, 1, KEY_CELL, KEY_CELL, px.p);

    RenderLabEncodeInfo info = {};
    Scratch<uint32_t> ws(RENDER_LAB_COUNT_BYTES / 4);
    RenderLabEncode(px.p, KEY_CELL, KEY_CELL, ws.p, RENDER_LAB_COUNT_BYTES, nullptr, 0, &info);
    Assert(info.workspace == RENDER_LAB_COUNT_BYTES);       // a face has few colours
    Scratch<uint32_t> out(KEY_CELL * KEY_CELL);
    size_t size = RenderLabEncode(px.p, KEY_CELL, KEY_CELL, ws.p, info.workspace, out.p, out.n * 4, &info);
    Assert(size > 0);
    Assert(RenderLabEncode(px.p, KEY_CELL, KEY_CELL, ws.p, info.workspace, out.p, size - 1, &info) == 0);
    Assert(RenderLabEncode(px.p, KEY_CELL, KEY_CELL, ws.p, info.workspace - 4, out.p, out.n * 4, &info) == 0);
    (void)ti;
}

Fact("Render Lab Recolour Copy Draws As The Recoloured Palette")
{
    // Every opaque 1U and 2U test key, recoloured on its green channel
    TestImages &ti = TestImages::Get();
    const uint32_t a = 0x102030, b = 0xF0C040;
    const int shift = 8;
    for (int i = 0; i < TEST_KEYS_KEYS_1U + TEST_KEYS_KEYS_2U; i++) {
        bool wide = i >= TEST_KEYS_KEYS_1U;
        const void *src = wide ? ti.Key2U(i - TEST_KEYS_KEYS_1U) : ti.Key1U(i);
        int w = wide ? 2 * KEY_CELL : KEY_CELL, h = KEY_CELL;
        size_t size = get_image_packed_size(src);

        // Snapshot of the source to check it's untouched
        Scratch<uint8_t> before(size);
        memcpy(before.p, src, size);

        // Too small a destination is refused
        Scratch<uint32_t> dst((size + 3) / 4);
        Assert(RenderLabRecolorCopy(src, dst.p, size - 1, a, b, shift) == 0);
        Assert(RenderLabRecolorCopy(src, dst.p, size, a, b, shift) == size);
        Assert(memcmp(before.p, src, size) == 0);

        bool opaque = false;
        Assert(check_image(dst.p, size, &opaque));
        Assert(opaque);
        Assert(opaque_image_fits(dst.p, w, h));

        // Expected: the source drawn with the recoloured palette
        const PALETTE *sp = get_image_palette(src);
        Scratch<uint32_t> pal(1 + sp->count);
        RenderLabRecolorPalette(sp, (PALETTE *)pal.p, a, b, shift);
        RenderLayer layers[] = { ImageLayer(src, 0, 0) };
        layers[0].palette = (const PALETTE *)pal.p;
        Scratch<uint16_t> want((size_t)w * h);
        RenderWhole(layers, 1, w, h, want.p);

        Scratch<uint16_t> got((size_t)w * h);
        opaque_draw_rows(got.p, w, dst.p, 0, h);
        Assert(memcmp(got.p, want.p, want.n * 2) == 0);

        // ...and not the original picture (the recolour did something)
        layers[0].palette = nullptr;
        RenderWhole(layers, 1, w, h, want.p);
        Assert(memcmp(got.p, want.p, want.n * 2) != 0);
    }
}

namespace
{
    BLOBPACK *LabAssets()
    {
        static BLOBPACK *pack = LoadAsset("render_lab.bin");
        return pack;
    }
}

Fact("Render Lab Assets Load And Check")
{
    BLOBPACK *pack = LabAssets();
    Assert(pack->count == RENDER_LAB_ASSET_COUNT);

    static const struct { int size, subpixel; } fonts[RENDER_LAB_FONTS] = { { 20, 1 }, { 27, 1 }, { 27, 4 } };
    for (int i = 0; i < RENDER_LAB_FONTS; i++) {
        const FONT *font = (const FONT *)get_blob(pack, i);
        Assert(((uintptr_t)font & 3) == 0);
        Assert(check_font(font, get_blob_size(pack, i)));
        Assert(font->size == fonts[i].size);
        Assert(font->subpixel == fonts[i].subpixel);
        Assert(font_find_glyph(font, 'A') != nullptr);
        Assert(font_find_glyph(font, '~') != nullptr);
    }

    static const struct { int index, size; } images[] = {
        { RENDER_LAB_ASSET_FACE, RENDER_LAB_FACE_SIZE },
        { RENDER_LAB_ASSET_ICON, RENDER_LAB_ICON_SIZE },
    };
    for (const auto &im : images) {
        const void *img = get_blob(pack, im.index);
        Assert(((uintptr_t)img & 3) == 0);
        bool opaque = true;
        Assert(check_image(img, get_blob_size(pack, im.index), &opaque));
        Assert(!opaque);                                    // alpha: drawn over other layers
        Assert(get_image_width(img) == (uint32_t)im.size);
        Assert(get_image_height(img) == (uint32_t)im.size);
    }
}

Fact("Render Lab Asset Fonts Render Text That Passes Every Check")
{
    BLOBPACK *pack = LabAssets();
    const void *face = get_blob(pack, RENDER_LAB_ASSET_FACE);
    const int m = (KEY_CELL - RENDER_LAB_FACE_SIZE) / 2;
    for (int i = 0; i < RENDER_LAB_FONTS; i++) {
        RenderLayer layers[] = {
            ImageLayer(face, m, m),
            TextLayer((const FONT *)get_blob(pack, i), "Vol+ 55%", 8, 90, 128, 40, RENDER_ALIGN_CENTER, 0xFFFF),
        };
        Scratch<uint16_t> px(KEY_CELL * KEY_CELL);
        CheckLook(layers, 2, KEY_CELL, KEY_CELL, px.p);

        // The text drew something the face alone doesn't have
        Scratch<uint16_t> bare(KEY_CELL * KEY_CELL);
        RenderWhole(layers, 1, KEY_CELL, KEY_CELL, bare.p);
        Assert(memcmp(px.p, bare.p, bare.n * 2) != 0);
    }

    // The icon over the face
    RenderLayer layers[] = {
        ImageLayer(face, m, m),
        ImageLayer(get_blob(pack, RENDER_LAB_ASSET_ICON), (KEY_CELL - RENDER_LAB_ICON_SIZE) / 2, 16),
    };
    Scratch<uint16_t> px(KEY_CELL * KEY_CELL);
    CheckLook(layers, 2, KEY_CELL, KEY_CELL, px.p);
}

namespace
{
    RenderLayer PathLayer(const PathVector &v, int x, int y, uint16_t color, uint8_t alpha)
    {
        RenderLayer l = {};
        l.type  = RENDER_LAYER_PATH;
        l.x = x; l.y = y; l.w = v.w; l.h = v.h;
        l.color = color;
        l.alpha = alpha;
        l.data  = v.path;
        return l;
    }
}

Fact("Render Lab Path Layer Equals Its Mask Drawn Directly")
{
    const int w = KEY_CELL, h = KEY_CELL;
    const PathVector &icon = FindPathVector("icon-volume-up");
    const PathVector &ring = FindPathVector("shape-ring");
    // An icon in the middle, and a ring partly off the left and bottom edges
    const struct { const PathVector *v; int x, y; uint16_t color; uint8_t alpha; } paths[] = {
        { &icon, (w - icon.w) / 2, 16, 0xFFFF, 255 },
        { &ring, -30, h - 50, 0xF800, 180 },
    };

    Scratch<uint16_t> direct(w * h);
    memset(direct.p, 0, direct.n * 2);
    IMAGE_BUFFER buf;
    init_image_buffer(&buf, w, h, direct.p, w);
    RenderLayer layers[2];
    for (int i = 0; i < 2; i++) {
        const PathVector &v = *paths[i].v;
        layers[i] = PathLayer(v, paths[i].x, paths[i].y, paths[i].color, paths[i].alpha);
        PathView pv = {};
        Assert(PathCheck(v.path, v.path_size, &pv));
        Scratch<uint32_t> acc(PathRasterScratchSize(v.w, v.h) / 4);
        Scratch<uint8_t> mask((size_t)v.w * v.h);
        Assert(PathRaster(pv, v.w, 0, v.h, acc.p, acc.n * 4, mask.p));
        draw_alpha_mask(&buf, paths[i].x, paths[i].y, mask.p, v.w, v.w, v.h, paths[i].color, paths[i].alpha);
    }

    Scratch<uint16_t> px(w * h);
    CheckLook(layers, 2, w, h, px.p);
    Assert(memcmp(px.p, direct.p, direct.n * 2) == 0);

    // The scratch is for the tallest path band; none without a path layer
    Assert(RenderLabScratchSize(layers, 2, h) >= (size_t)ring.w * ring.h * 5);
    Assert(RenderLabScratchSize(layers, 2, 7) < RenderLabScratchSize(layers, 2, h));
    Assert(RenderLabScratchSize(layers, 0, h) == 0);

    // Too little scratch: refused, nothing drawn
    size_t need = RenderLabScratchSize(layers, 2, h);
    Scratch<uint32_t> scratch(need / 4);
    for (size_t i = 0; i < px.n; i++)
        px.p[i] = POISON;
    Assert(!RenderLabRender(layers, 2, w, 0, h, px.p, scratch.p, need - 4));
    Assert(!RenderLabRender(layers, 2, w, 0, h, px.p, nullptr, 0));
    for (size_t i = 0; i < px.n; i++)
        Assert(px.p[i] == POISON);
}

Fact("Render Lab Path Icon And Glyphs Over A Face Pass Every Check")
{
    BLOBPACK *pack = LabAssets();
    const void *face = get_blob(pack, RENDER_LAB_ASSET_FACE);
    const int m = (KEY_CELL - RENDER_LAB_FACE_SIZE) / 2;
    const PathVector &icon = FindPathVector("icon-volume-up");
    const PathVector &text = FindPathVector("glyphs-Vol+");
    RenderLayer layers[] = {
        ImageLayer(face, m, m),
        PathLayer(icon, (KEY_CELL - icon.w) / 2, 14, 0xFFFF, 255),
        PathLayer(text, (KEY_CELL - text.w) / 2, 96, 0xFFE0, 255),
    };
    Scratch<uint16_t> px(KEY_CELL * KEY_CELL);
    CheckLook(layers, 3, KEY_CELL, KEY_CELL, px.p);

    // The paths drew something the face alone doesn't have
    Scratch<uint16_t> bare(KEY_CELL * KEY_CELL);
    RenderWhole(layers, 1, KEY_CELL, KEY_CELL, bare.p);
    Assert(memcmp(px.p, bare.p, bare.n * 2) != 0);
}

// ---------------------------------------------------------------------------
// Benchmark cases (render_lab_cases.cpp): every case the host bench and the
// device run time is checked here, so their pictures are known good

namespace
{
    const RenderLabAssets *CaseAssets()
    {
        static RenderLabAssets a = {};
        if (!a.lab) {
            a.lab = LabAssets();
            a.vectors = PathVectorFile(&a.vectors_size);
        }
        return &a;
    }
}

Fact("Render Lab Cases Pass Every Check")
{
    const RenderLabAssets *assets = CaseAssets();
    const int count = RenderLabCaseCount();
    Assert(count >= 9);
    RenderLabCase c;
    Assert(!RenderLabCaseGet(-1, assets, &c));
    Assert(!RenderLabCaseGet(count, assets, &c));

    // Missing assets: refused
    RenderLabAssets none = {};
    Assert(!RenderLabCaseGet(0, &none, &c));

    void *outs[32] = {};
    Assert(count <= 32);
    bool any_path = false, any_recolor = false, any_large = false;
    for (int i = 0; i < count; i++) {
        Assert(RenderLabCaseGet(i, assets, &c));
        Assert(c.name && c.name[0]);
        Assert(c.w > 0 && c.h > 0 && c.w <= KEY_GRID_W && c.h <= KEY_GRID_H);

        RenderLabResult sizes;
        RenderLabCaseSizes(&c, 16, &sizes);
        Assert(sizes.px_whole == (size_t)c.w * c.h * 2);
        Assert(sizes.px_band == (size_t)c.w * 16 * 2);
        Assert(sizes.scratch_whole == RenderLabScratchSize(c.layers, c.n, c.h));
        Assert(sizes.scratch_band == RenderLabScratchSize(c.layers, c.n, 16));
        Assert(sizes.render_ns == 0 && sizes.size == 0);

        if (c.kind == RENDER_CASE_RECOLOR) {
            // A copy of an earlier case's image, recoloured: as the recolour test checks
            Assert(c.source >= 0 && c.source < i && outs[c.source]);
            const void *src = outs[c.source];
            size_t size = get_image_packed_size(src);
            Scratch<uint32_t> out((size + 3) / 4);
            RenderLabBuffers b = {};
            b.out = out.p; b.out_size = size - 1;
            RenderLabResult r;
            Assert(!RenderLabCaseRun(&c, &b, src, nullptr, 1, &r));
            b.out_size = size;
            Assert(RenderLabCaseRun(&c, &b, src, nullptr, 1, &r));
            Assert(r.size == size);
            Assert(r.colors == get_image_palette(src)->count);
            bool opaque = false;
            Assert(check_image(out.p, size, &opaque) && opaque);
            Assert(opaque_image_fits(out.p, c.w, c.h));
            Assert(memcmp(out.p, src, size) != 0);
            any_recolor = true;
            continue;
        }

        // The picture: banded == whole, encode round trip
        Assert(c.kind == RENDER_CASE_RENDER);
        Assert(c.n > 0 && c.n <= RENDER_LAB_CASE_LAYERS);
        Scratch<uint16_t> whole((size_t)c.w * c.h);
        CheckLook(c.layers, c.n, c.w, c.h, whole.p);
        for (int l = 0; l < c.n; l++)
            any_path |= c.layers[l].type == RENDER_LAYER_PATH;

        // The runner with buffers of exactly the size asked for
        Scratch<uint16_t> px((size_t)c.w * c.h);
        Scratch<uint32_t> scratch(sizes.scratch_whole / 4);
        Scratch<uint32_t> count_ws(RENDER_LAB_COUNT_BYTES / 4);
        RenderLabBuffers b = {};
        b.px = px.p; b.px_bytes = sizes.px_whole;
        b.scratch = sizes.scratch_whole ? scratch.p : nullptr; b.scratch_size = sizes.scratch_whole;
        b.band_rows = 16;

        // First with only the count bitmap and no output: refused, but the needs are known
        b.ws = count_ws.p; b.ws_size = RENDER_LAB_COUNT_BYTES;
        RenderLabResult r;
        Assert(!RenderLabCaseRun(&c, &b, nullptr, nullptr, 1, &r));
        Assert(r.colors > 0);
        Assert(r.workspace == RenderLabWorkspaceSize(r.colors));
        Assert(r.bound == RenderLabEncodeBound(c.w, c.h, r.colors));
        Assert(memcmp(px.p, whole.p, sizes.px_whole) == 0);

        Scratch<uint32_t> ws((r.workspace + 3) / 4);
        Scratch<uint32_t> out((r.bound + 3) / 4);
        b.ws = ws.p; b.ws_size = r.workspace;
        b.out = out.p; b.out_size = r.bound;
        Assert(RenderLabCaseRun(&c, &b, nullptr, nullptr, 2, &r));
        Assert(r.size > 0 && r.size <= r.bound);
        Assert(get_image_palette(out.p)->count == r.colors);
        any_large |= r.colors > 256;
        if (i == RENDER_LAB_CASE_LARGE_PALETTE) {
            // The stress run's large-palette decode case (opaque_draw_rows' varlen path)
            Assert(r.colors > 256);
            Assert((((const IMAGE *)out.p)->flags & DRAW_FLAGS_LARGE_PALETTE) != 0);
        }

        bool opaque = false;
        Assert(check_image(out.p, r.size, &opaque) && opaque);
        Assert(opaque_image_fits(out.p, c.w, c.h));
        Scratch<uint16_t> back((size_t)c.w * c.h);
        opaque_draw_rows(back.p, c.w, out.p, 0, c.h);
        Assert(memcmp(back.p, whole.p, sizes.px_whole) == 0);

        // Kept for the recolour cases
        outs[i] = malloc(r.size);
        memcpy(outs[i], out.p, r.size);

        // Too small a picture buffer or scratch: refused
        RenderLabBuffers small = b;
        RenderLabResult refused;
        small.px_bytes = sizes.px_whole - 2;
        Assert(!RenderLabCaseRun(&c, &small, nullptr, nullptr, 1, &refused));
        if (sizes.scratch_whole) {
            small = b;
            small.scratch_size = sizes.scratch_whole - 4;
            Assert(!RenderLabCaseRun(&c, &small, nullptr, nullptr, 1, &refused));
        }
    }
    Assert(any_path && any_recolor && any_large);
    for (void *p : outs)
        free(p);
}

Fact("Render Lab Case Names Match The Case Enum")
{
    // The device run and the stress cases pick cases by RENDER_LAB_CASE_*
    const RenderLabAssets *assets = CaseAssets();
    Assert(RenderLabCaseCount() == RENDER_LAB_CASE_COUNT);
    RenderLabCase c;
    Assert(RenderLabCaseGet(RENDER_LAB_CASE_2U, assets, &c) && c.w == 2 * KEY_CELL && c.h == KEY_CELL);
    Assert(RenderLabCaseGet(RENDER_LAB_CASE_FULL, assets, &c) && c.w == KEY_GRID_W && c.h == KEY_GRID_H);
    Assert(RenderLabCaseGet(RENDER_LAB_CASE_FACE_LABEL, assets, &c) && c.w == KEY_CELL && c.h == KEY_CELL);
    Assert(RenderLabCaseGet(RENDER_LAB_CASE_RECOLOR_COPY, assets, &c) && c.kind == RENDER_CASE_RECOLOR);
    Assert(RenderLabCaseGet(RENDER_LAB_CASE_LARGE_PALETTE, assets, &c) && c.w == KEY_CELL && c.h == KEY_CELL);
    Assert(strstr(c.name, "256") != nullptr);
}

Fact("Render Lab 2U Case Draws Only On Its Faces")
{
    // The 2U case is two 1U faces side by side. Everything it draws must lie on
    // one of the faces: the black between and around them is the key's margin
    // (on the device it reads as the gap between keys). Found on the hardware:
    // the right-aligned "Wide key" label ran left off the second face.
    const RenderLabAssets *assets = CaseAssets();
    RenderLabCase c;
    Assert(RenderLabCaseGet(RENDER_LAB_CASE_2U, assets, &c));
    Scratch<uint16_t> px((size_t)c.w * c.h);
    RenderWhole(c.layers, c.n, c.w, c.h, px.p);

    const int m = (KEY_CELL - RENDER_LAB_FACE_SIZE) / 2;
    int outside = 0;
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++) {
            bool on_face = y >= m && y < m + RENDER_LAB_FACE_SIZE &&
                ((x >= m && x < m + RENDER_LAB_FACE_SIZE) ||
                 (x >= KEY_CELL + m && x < KEY_CELL + m + RENDER_LAB_FACE_SIZE));
            if (!on_face && px.p[y * c.w + x] != 0)
                outside++;
        }
    Assert(outside == 0);
}

Fact("Render Lab Reps Aim At A Target Time")
{
    // One run's time -> repeats for about `target`, clamped to [3, max]
    Assert(RenderLabRepsFor(1000, 100000, 5000) == 100);
    Assert(RenderLabRepsFor(0, 100000, 5000) == 5000);         // too quick to time
    Assert(RenderLabRepsFor(1, 100000, 5000) == 5000);
    Assert(RenderLabRepsFor(60000, 100000, 5000) == 3);         // slow: the minimum
    Assert(RenderLabRepsFor(1000000, 100000, 50) == 3);
    Assert(RenderLabRepsFor(1000, 100000, 50) == 50);
    Assert(RenderLabRepsFor(1000, 100000, 2) == 3);             // the minimum wins over max
}

Fact("Render Lab Keys Flow Across The Grid")
{
    // Whole-cell keys placed row by row; one that doesn't fit the row's rest starts the next
    int cursor = 0;
    KeyRect r;
    Assert(RenderLabPlaceKey(KEY_CELL, KEY_CELL, &cursor, &r));
    Assert(r.x == 0 && r.y == 0 && r.w == 4 && r.h == 4 && cursor == 1);
    for (int i = 0; i < 3; i++)
        Assert(RenderLabPlaceKey(KEY_CELL, KEY_CELL, &cursor, &r));
    Assert(r.x == 12 && r.y == 0 && cursor == 4);
    // A 2U key doesn't fit the last cell of row 0: row 1, the cell skipped
    Assert(RenderLabPlaceKey(2 * KEY_CELL, KEY_CELL, &cursor, &r));
    Assert(r.x == 0 && r.y == 4 && r.w == 8 && r.h == 4 && cursor == KEY_GRID_COLS + 2);
    // Not whole cells, or bigger than the grid: refused, cursor kept
    int before = cursor;
    Assert(!RenderLabPlaceKey(KEY_CELL + 1, KEY_CELL, &cursor, &r));
    Assert(!RenderLabPlaceKey(KEY_GRID_W, KEY_GRID_H + KEY_CELL, &cursor, &r));
    Assert(!RenderLabPlaceKey(0, KEY_CELL, &cursor, &r));
    Assert(cursor == before);
    // Fill the rest of the grid, then no room
    int placed = 0;
    while (RenderLabPlaceKey(KEY_CELL, KEY_CELL, &cursor, &r))
        placed++;
    Assert(placed == KEY_GRID_SLOTS - before);
    Assert(r.x == 16 && r.y == 8);
    Assert(KeyRectsCheck(&r, 1) == KEY_GEOM_OK);
    // The whole grid as one key only from the start
    cursor = 0;
    Assert(RenderLabPlaceKey(KEY_GRID_W, KEY_GRID_H, &cursor, &r));
    Assert(r.w == 20 && r.h == 12 && cursor == KEY_GRID_SLOTS);
}

Fact("Render Lab Result Line Has The Case And Its Numbers")
{
    const RenderLabAssets *assets = CaseAssets();
    RenderLabCase c;
    Assert(RenderLabCaseGet(RENDER_LAB_CASE_FACE_LABEL, assets, &c));
    RenderLabResult r;
    RenderLabCaseSizes(&c, 16, &r);
    r.render_ns = 49900; r.band_ns = 54100; r.encode_ns = 341000;
    r.colors = 84; r.size = 3270; r.bound = 22412; r.workspace = 8192;

    char line[256];
    size_t n = RenderLabFormatResult(&c, &r, line, sizeof(line));
    Assert(n == strlen(line) && n > 0 && n < sizeof(line));
    for (const char *want : { c.name, "144x144", "render 49.9 us", "banded 54.1 us", "encode 341.0 us",
                              "84 colours", "3270 B", "bound 22412", "ws 8192" })
        Assert(strstr(line, want) != nullptr);

    // A short buffer: cut, still terminated, and the full length returned
    char small[20];
    memset(small, 'x', sizeof(small));
    Assert(RenderLabFormatResult(&c, &r, small, sizeof(small)) == n);
    Assert(strlen(small) == sizeof(small) - 1);
}

Fact("Render Lab Glyph Case Draws The Run")
{
    // The per-glyph path layers draw (near enough) what the whole run's path does
    const RenderLabAssets *assets = CaseAssets();
    RenderLabCase c;
    int i = 0;
    for (; i < RenderLabCaseCount(); i++) {
        Assert(RenderLabCaseGet(i, assets, &c));
        if (strstr(c.name, "glyphs"))
            break;
    }
    Assert(i < RenderLabCaseCount());

    // The glyph layers alone, then the run's path at the first glyph's run origin
    int first = -1, glyphs = 0;
    for (int l = 0; l < c.n; l++)
        if (c.layers[l].type == RENDER_LAYER_PATH) {
            if (first < 0)
                first = l;
            glyphs++;
        }
    Assert(glyphs == 3);
    const PathVector &g0 = FindPathVector("glyph-55%-0");
    const PathVector &run = FindPathVector("glyphs-55%");
    RenderLayer r = c.layers[first];
    r.x = (int16_t)(r.x - g0.x);
    r.w = (uint16_t)run.w;
    r.data = run.path;

    Scratch<uint16_t> a((size_t)c.w * c.h), b((size_t)c.w * c.h);
    RenderWhole(&c.layers[first], glyphs, c.w, c.h, a.p);
    RenderWhole(&r, 1, c.w, c.h, b.p);
    int differ = 0;
    for (size_t k = 0; k < a.n; k++) {
        int d = (int)(a.p[k] >> 11) - (int)(b.p[k] >> 11);     // red channel, 5 bits
        Assert(d >= -1 && d <= 1);
        differ += a.p[k] != b.p[k];
    }
    Assert(differ < 40);
}
