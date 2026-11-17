// rle565's C encoder (rle565/src/rle565enc.c) round tripped through the
// firmware's opaque key path: encode a 565 picture, check it as the image
// store would (check_image, opaque_image_fits) and draw it back with
// opaque_draw_rows. Render lab (wip/wip-on-device-render-feasibility.md).

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"
#include "key_geometry.h"
#include "opaque_row.h"

using namespace SimpleLib;

namespace
{
    #define ENC_W           (2 * KEY_CELL)          // big enough for a 2U key
    #define ENC_H           KEY_CELL
    #define ENC_MAX_COLORS  4096
    #define ENC_OUT_WORDS   (64 * 1024)             // 256 KB: far more than any test image
    #define POISON          0x5A5A

    // 4-byte aligned scratch, exactly sized for the largest case
    uint16_t s_src[ENC_W * ENC_H];
    uint16_t s_back[ENC_W * ENC_H];
    uint32_t s_ws[IMAGE_ENCODE_WORKSPACE_SIZE(ENC_MAX_COLORS) / 4];
    uint32_t s_out[ENC_OUT_WORDS];

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

    // `img` drawn into s_src (w x h, stride w)
    void DrawToSource(const void *img, int w, int h)
    {
        for (int i = 0; i < w * h; i++)
            s_src[i] = POISON;
        IMAGE_BUFFER view;
        init_image_buffer(&view, w, h, s_src, w);
        draw_image(&view, img, 0, 0, 0, 0, w, h, nullptr, 255, nullptr);
    }

    size_t Encode(int w, int h, void *out, size_t out_size, size_t ws_size = sizeof(s_ws))
    {
        return encode_image_rgb565(s_src, w, h, 0, s_ws, ws_size, out, out_size);
    }

    // The encoded s_out (size bytes) passes the image store's checks and draws back to s_src
    void CheckRoundTrip(int w, int h, size_t size)
    {
        bool opaque = false;
        Assert(check_image(s_out, size, &opaque));
        Assert(opaque);
        Assert(opaque_image_fits(s_out, w, h));
        Assert(get_image_packed_size(s_out) == size);

        for (int i = 0; i < w * h; i++)
            s_back[i] = POISON;
        opaque_draw_rows(s_back, w, s_out, 0, h);
        Assert(memcmp(s_src, s_back, (size_t)w * h * 2) == 0);
    }
}

Fact("Rle Encode Round Trips Every Opaque Test Key")
{
    List<TestKey> keys = AllKeys();
    int byte_equal = 0;
    for (int i = 0; i < keys.GetCount(); i++) {
        TestKey k = keys[i];
        DrawToSource(k.img, k.w, k.h);

        // (a) the dry run gives the written size
        size_t dry = Encode(k.w, k.h, nullptr, 0);
        Assert(dry > 0);
        size_t size = Encode(k.w, k.h, s_out, sizeof(s_out));
        Assert(size == dry);

        // (b), (c) checks and draw back
        CheckRoundTrip(k.w, k.h, size);

        // (d) byte for byte the JS encoder's output (counted; see the job's Notes)
        uint32_t orig = get_image_packed_size(k.img);
        if (orig == size && memcmp(k.img, s_out, size) == 0)
            byte_equal++;
    }
    Assert(byte_equal == keys.GetCount());
}

Fact("Rle Encode Refuses A Small Output Or Workspace")
{
    TestKey k = AllKeys()[0];
    DrawToSource(k.img, k.w, k.h);
    size_t size = Encode(k.w, k.h, s_out, sizeof(s_out));
    Assert(size > 0);

    // (e) one byte short of the output
    Assert(Encode(k.w, k.h, s_out, size - 1) == 0);

    // (e) room for one colour fewer than the image has (it has its own palette)
    uint32_t colors = get_image_palette(s_out)->count;
    Assert(colors > 1);
    Assert(Encode(k.w, k.h, nullptr, 0, IMAGE_ENCODE_WORKSPACE_SIZE(colors - 1)) == 0);
    Assert(Encode(k.w, k.h, s_out, sizeof(s_out), IMAGE_ENCODE_WORKSPACE_SIZE(colors - 1)) == 0);
    Assert(Encode(k.w, k.h, s_out, sizeof(s_out), IMAGE_ENCODE_WORKSPACE_SIZE(colors)) == size);
}

Fact("Rle Encode Large Palette Round Trips")
{
    // (f) a 1U picture with 1000 distinct colours: bands of runs and single pixels
    const int w = KEY_CELL, h = KEY_CELL, colors = 1000;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int c = (y * w + x) / 3 % colors;           // short runs
            if (y % 4 == 0)
                c = y % colors;                         // a long run per row
            s_src[y * w + x] = (uint16_t)(c * 37 + 1);  // distinct for c < 1000
        }

    size_t dry = Encode(w, h, nullptr, 0);
    size_t size = Encode(w, h, s_out, sizeof(s_out));
    Assert(size > 0 && size == dry);
    Assert(get_image_palette(s_out)->count > 256);
    Assert((((const IMAGE *)s_out)->flags & DRAW_FLAGS_LARGE_PALETTE) != 0);
    CheckRoundTrip(w, h, size);
}
