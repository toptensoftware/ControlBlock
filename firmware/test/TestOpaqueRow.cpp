// The opaque row decoder (opaque_row.h) against rle565's own draw_image.

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"
#include "key_geometry.h"
#include "opaque_row.h"

using namespace SimpleLib;

namespace
{
    #define POISON      0x5A5A
    #define BUF_W       (2 * KEY_CELL + 3)      // room for an odd column offset and a guard
    #define BUF_H       (KEY_CELL + 2)

    // Every opaque test key with its size
    struct TestKey { const void *img; int w, h; };

    List<TestKey> AllKeys()
    {
        TestImages &ti = TestImages::Get();
        List<TestKey> keys;
        for (int i = 0; i < TEST_KEYS_KEYS_1U; i++)
            keys.Add(TestKey{ ti.Key1U(i), KEY_CELL, KEY_CELL });
        for (int i = 0; i < TEST_KEYS_KEYS_2U; i++)
            keys.Add(TestKey{ ti.Key2U(i), 2 * KEY_CELL, KEY_CELL });
        return keys;
    }

    // draw_image's rows [y0, y0 + rows) of `img` at (x, 1) of a poisoned buffer
    void Reference(uint16_t *buf, const TestKey &k, int x, int y0, int rows)
    {
        for (int i = 0; i < BUF_W * BUF_H; i++)
            buf[i] = POISON;
        IMAGE_BUFFER view;
        init_image_buffer(&view, BUF_W, BUF_H, buf, BUF_W);
        draw_image(&view, k.img, x, 1 + y0, 0, y0, k.w, rows, nullptr, 255, nullptr);
    }
}

Fact("Opaque Row Images Fit Only Their Own Size")
{
    TestImages &ti = TestImages::Get();
    for (int i = 0; i < AllKeys().GetCount(); i++) {
        TestKey k = AllKeys()[i];
        Assert(opaque_image_fits(k.img, k.w, k.h));
        Assert(!opaque_image_fits(k.img, k.w - 1, k.h));
        Assert(!opaque_image_fits(k.img, k.w, k.h + 1));
    }
    Assert(!opaque_image_fits(nullptr, KEY_CELL, KEY_CELL));

    // A 128 face with transparent padding doesn't store every row
    const IMAGE_RLE *face = (const IMAGE_RLE *)ti.Face(0, false);
    if (face->firstRow != 0 || face->rowCount != face->base.height)
        Assert(!opaque_image_fits(face, face->base.width, face->base.height));
}

Fact("Opaque Row Decoder Matches draw_image")
{
    static uint16_t ref[BUF_W * BUF_H], out[BUF_W * BUF_H];
    List<TestKey> keys = AllKeys();
    for (int i = 0; i < keys.GetCount(); i++) {
        TestKey k = keys[i];
        // Even and odd columns (the odd one takes the unaligned first pixel path)
        for (int x = 0; x < 2; x++) {
            Reference(ref, k, x, 0, k.h);
            for (int p = 0; p < BUF_W * BUF_H; p++)
                out[p] = POISON;
            opaque_draw_rows(out + BUF_W + x, BUF_W, k.img, 0, k.h);
            Assert(memcmp(ref, out, sizeof(ref)) == 0);     // every pixel, and nothing outside
        }
    }
}

Fact("Opaque Row Decoder Draws A Range Of Rows")
{
    static uint16_t ref[BUF_W * BUF_H], out[BUF_W * BUF_H];
    TestKey k = AllKeys()[TEST_KEYS_KEYS_1U];        // a 2U key
    static const int ranges[][2] = { { 0, 1 }, { 3, 7 }, { 100, 44 }, { KEY_CELL - 1, 1 } };
    for (const auto &r : ranges) {
        Reference(ref, k, 1, r[0], r[1]);
        for (int p = 0; p < BUF_W * BUF_H; p++)
            out[p] = POISON;
        opaque_draw_rows(out + (1 + r[0]) * BUF_W + 1, BUF_W, k.img, r[0], r[1]);
        Assert(memcmp(ref, out, sizeof(ref)) == 0);
    }
}
