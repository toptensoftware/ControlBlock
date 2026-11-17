// Render lab path rasteriser (path_raster.cpp) against the JS reference masks
// in data/path_vectors.bin (tools/render-lab/gen-path-vectors.mjs).
// wip/wip-on-device-render-feasibility.md, step 6.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"
#include "path_raster.h"
#include "path_vectors.h"

using namespace SimpleLib;

typedef PathVector Vector;

const void *PathVectorFile(size_t *size)
{
    static size_t s_size = 0;
    static const void *s_file = LoadTestData("path_vectors.bin", &s_size);
    *size = s_size;
    return s_file;
}

const List<PathVector> &PathVectors()
{
    static List<PathVector> s_vectors;
    static bool s_loaded = false;
    if (s_loaded)
        return s_vectors;
    s_loaded = true;
    size_t size = 0;
    const void *file = PathVectorFile(&size);
    PathVector all[64];
    int count = RenderLabVectorsRead(file, size, all, 64);
    Assert(count > 20);
    for (int i = 0; i < count; i++)
        s_vectors.Add(all[i]);
    return s_vectors;
}

Fact("Render Lab Vector Reader Refuses Damaged Files")
{
    size_t size = 0;
    const uint8_t *file = (const uint8_t *)PathVectorFile(&size);
    PathVector all[64];
    int count = RenderLabVectorsRead(file, size, all, 64);
    Assert(count > 20);
    Assert(RenderLabVectorsRead(file, size, all, count - 1) == -1);     // more than max
    Assert(RenderLabVectorsRead(file, size, all, count) == count);
    for (size_t cut : { (size_t)0, (size_t)7, (size_t)20, size - 1 })
        Assert(RenderLabVectorsRead(file, cut, all, 64) == -1);            // truncated
    Assert(RenderLabVectorsRead(nullptr, 0, all, 64) == -1);

    List<uint8_t> copy;
    copy.SetCount((int)size, 0);
    memcpy(copy.GetBuffer(), file, size);
    copy.GetBuffer()[0] ^= 1;                                               // magic
    Assert(RenderLabVectorsRead(copy.GetBuffer(), size, all, 64) == -1);
    copy.GetBuffer()[0] ^= 1;
    copy.GetBuffer()[4] = 2;                                                // version
    Assert(RenderLabVectorsRead(copy.GetBuffer(), size, all, 64) == -1);

    // Per-glyph entries carry their x in the run (left to right); nothing else has one
    int glyphs = 0, last_x = -1;
    for (int i = 0; i < count; i++) {
        if (strncmp(all[i].name, "glyph-", 6) != 0) {
            Assert(all[i].x == 0);
            continue;
        }
        Assert(all[i].x > last_x);
        last_x = all[i].x;
        glyphs++;
    }
    Assert(glyphs == 3);
}

const PathVector &FindPathVector(const char *name)
{
    const List<PathVector> &all = PathVectors();
    for (int i = 0; i < all.GetCount(); i++)
        if (strcmp(all.GetBuffer()[i].name, name) == 0)
            return all.GetBuffer()[i];
    fprintf(stderr, "no path vector '%s'\n", name);
    Assert(false);
    return all.GetBuffer()[0];
}

namespace
{
    #define MASK_POISON     0xA5
    #define MAX_DIFF        2           // per pixel, float vs the reference's double

    // Heap scratch of exactly `bytes` (the debug CRT catches an overrun on free)
    struct Bytes
    {
        explicit Bytes(size_t bytes) : n(bytes), p((uint8_t *)malloc(bytes ? bytes : 1)) { Assert(p != nullptr); }
        ~Bytes() { free(p); }
        Bytes(const Bytes &) = delete;
        Bytes &operator=(const Bytes &) = delete;
        size_t n;
        uint8_t *p;
    };

    // Rasterise rows y0..y0+rows of `path` in a `w` wide box into `mask` (poisoned first)
    void Raster(const PathView &path, int w, int y0, int rows, uint8_t *mask)
    {
        Bytes acc(PathRasterScratchSize(w, rows));
        memset(mask, MASK_POISON, (size_t)w * rows);
        Assert(PathRaster(path, w, y0, rows, acc.p, acc.n, mask));
    }

    PathView Open(const Vector &v)
    {
        PathView pv = {};
        Assert(PathCheck(v.path, v.path_size, &pv));
        return pv;
    }

    // A copy of a path's bytes with every point moved by (dx, dy) whole pixels
    void Translated(const Vector &v, int dx, int dy, Bytes &out)
    {
        Assert(out.n == v.path_size);
        memcpy(out.p, v.path, v.path_size);
        PathView pv = {};
        Assert(PathCheck(out.p, out.n, &pv));
        int16_t *pts = (int16_t *)pv.points;
        for (int i = 0; i < pv.point_count; i++) {
            pts[i * 2]     = (int16_t)(pts[i * 2] + dx * PATH_POINT_SCALE);
            pts[i * 2 + 1] = (int16_t)(pts[i * 2 + 1] + dy * PATH_POINT_SCALE);
        }
    }

    // Build a path from commands and points (in pixels) into `out` (2-byte aligned)
    size_t MakePath(const uint8_t *cmds, int ncmds, const float *pts, int npts, uint8_t *out)
    {
        out[0] = (uint8_t)ncmds; out[1] = 0;
        out[2] = (uint8_t)npts;  out[3] = 0;
        memcpy(out + 4, cmds, ncmds);
        size_t p = 4 + ncmds + (ncmds & 1);
        if (ncmds & 1)
            out[4 + ncmds] = 0;
        for (int i = 0; i < npts * 2; i++, p += 2) {
            int16_t q = (int16_t)(pts[i] * PATH_POINT_SCALE);
            memcpy(out + p, &q, 2);
        }
        return p;
    }

    bool AllZero(const uint8_t *m, size_t n)
    {
        for (size_t i = 0; i < n; i++)
            if (m[i])
                return false;
        return true;
    }
}

Fact("Path Raster Matches The JS Reference")
{
    int max_diff = 0;
    uint64_t sum_diff = 0, pixels = 0, differ = 0;
    for (int vi = 0; vi < PathVectors().GetCount(); vi++) {
        const Vector &v = PathVectors().GetBuffer()[vi];
        PathView pv = Open(v);
        Bytes mask((size_t)v.w * v.h);
        Raster(pv, v.w, 0, v.h, mask.p);
        bool inked = false;
        for (int i = 0; i < v.w * v.h; i++) {
            int d = abs((int)mask.p[i] - (int)v.mask[i]);
            if (d > max_diff)
                max_diff = d;
            sum_diff += d;
            differ += d != 0;
            inked |= mask.p[i] != 0;
            if (d > MAX_DIFF)
                fprintf(stderr, "%s: pixel (%d, %d) %d, reference %d\n", v.name, i % v.w, i / v.w, mask.p[i], v.mask[i]);
            Assert(d <= MAX_DIFF);
        }
        Assert(inked);
        pixels += (uint64_t)v.w * v.h;
    }
    printf("    path raster vs JS: max diff %d, mean %.5f, %llu of %llu pixels differ\n",
           max_diff, (double)sum_diff / (double)pixels, (unsigned long long)differ, (unsigned long long)pixels);
}

Fact("Path Raster Banded Equals Whole")
{
    for (int vi = 0; vi < PathVectors().GetCount(); vi++) {
        const Vector &v = PathVectors().GetBuffer()[vi];
        PathView pv = Open(v);
        Bytes whole((size_t)v.w * v.h);
        Raster(pv, v.w, 0, v.h, whole.p);
        for (int band : { 1, 7, 16 }) {
            Bytes strip((size_t)v.w * band);
            for (int y0 = 0; y0 < v.h; y0 += band) {
                int rows = v.h - y0 < band ? v.h - y0 : band;
                Raster(pv, v.w, y0, rows, strip.p);
                Assert(memcmp(strip.p, whole.p + (size_t)y0 * v.w, (size_t)v.w * rows) == 0);
            }
        }
    }
}

Fact("Path Raster Clips At Every Edge")
{
    // Each path moved left/up past the box's edge, and the box cut short on
    // the right/bottom: what's left matches the reference's matching pixels
    static const struct { int dx, dy, cut_w, cut_h; } cases[] = {
        { -17, 0, 0, 0 }, { 0, -23, 0, 0 }, { -30, -30, 0, 0 },
        { 0, 0, 25, 0 }, { 0, 0, 0, 19 }, { -11, -9, 13, 21 },
    };
    for (int vi = 0; vi < PathVectors().GetCount(); vi++) {
        const Vector &v = PathVectors().GetBuffer()[vi];
        Bytes moved(v.path_size);
        for (const auto &c : cases) {
            int w = v.w + c.dx - c.cut_w, h = v.h + c.dy - c.cut_h;
            if (w <= 0 || h <= 0)
                continue;
            Translated(v, c.dx, c.dy, moved);
            PathView pv = Open(v);
            Assert(PathCheck(moved.p, moved.n, &pv));
            Bytes mask((size_t)w * h);
            Raster(pv, w, 0, h, mask.p);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int want = v.mask[(y - c.dy) * v.w + (x - c.dx)];
                    Assert(abs((int)mask.p[y * w + x] - want) <= MAX_DIFF);
                }
        }

        // A band above and below the box: rows outside it are just empty
        PathView pv = Open(v);
        Bytes mask((size_t)v.w * 8);
        Raster(pv, v.w, -40, 8, mask.p);
        Raster(pv, v.w, v.h + 40, 8, mask.p);
        Assert(AllZero(mask.p, mask.n));
    }
}

Fact("Path Raster Empty And Degenerate Paths Are Empty")
{
    const int w = 40, h = 30;
    Bytes mask((size_t)w * h);
    alignas(4) uint8_t buf[256];
    PathView pv = {};

    // No commands
    size_t n = MakePath(nullptr, 0, nullptr, 0, buf);
    Assert(PathCheck(buf, n, &pv));
    Raster(pv, w, 0, h, mask.p);
    Assert(AllZero(mask.p, mask.n));

    // A lone move; horizontal lines; a line out and back (no area)
    {
        static const uint8_t c[] = { PATH_CMD_M };
        static const float p[] = { 10, 10 };
        Assert(PathCheck(buf, MakePath(c, 1, p, 1, buf), &pv));
        Raster(pv, w, 0, h, mask.p);
        Assert(AllZero(mask.p, mask.n));
    }
    {
        static const uint8_t c[] = { PATH_CMD_M, PATH_CMD_L, PATH_CMD_L, PATH_CMD_Z };
        static const float p[] = { 2, 10, 30, 10, 5, 10 };
        Assert(PathCheck(buf, MakePath(c, 4, p, 3, buf), &pv));
        Raster(pv, w, 0, h, mask.p);
        Assert(AllZero(mask.p, mask.n));
    }
    {
        static const uint8_t c[] = { PATH_CMD_M, PATH_CMD_L, PATH_CMD_Z };
        static const float p[] = { 3, 3, 31, 25 };
        Assert(PathCheck(buf, MakePath(c, 3, p, 2, buf), &pv));
        Raster(pv, w, 0, h, mask.p);
        Assert(AllZero(mask.p, mask.n));
    }
    // A collapsed curve (every control point the same)
    {
        static const uint8_t c[] = { PATH_CMD_M, PATH_CMD_C, PATH_CMD_Q, PATH_CMD_Z };
        static const float p[] = { 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7 };
        Assert(PathCheck(buf, MakePath(c, 4, p, 6, buf), &pv));
        Raster(pv, w, 0, h, mask.p);
        Assert(AllZero(mask.p, mask.n));
    }
    // Squares wholly left of, right of, above and below the box: nothing inside
    for (int s = 0; s < 4; s++) {
        static const uint8_t c[] = { PATH_CMD_M, PATH_CMD_L, PATH_CMD_L, PATH_CMD_L, PATH_CMD_Z };
        float x = s == 0 ? -30.5f : s == 1 ? w + 3.5f : 10.0f;
        float y = s == 2 ? -30.5f : s == 3 ? h + 3.5f : 10.0f;
        const float p[] = { x, y, x + 20, y, x + 20, y + 15, x, y + 15 };
        Assert(PathCheck(buf, MakePath(c, 5, p, 4, buf), &pv));
        Raster(pv, w, 0, h, mask.p);
        Assert(AllZero(mask.p, mask.n));
    }
    // A path without a leading M starts at (0, 0), as the JS rasteriser does:
    // this square still fills
    {
        static const uint8_t c[] = { PATH_CMD_L, PATH_CMD_L, PATH_CMD_L, PATH_CMD_Z };
        static const float p[] = { 20, 0, 20, 20, 0, 20 };
        Assert(PathCheck(buf, MakePath(c, 4, p, 3, buf), &pv));
        Raster(pv, w, 0, h, mask.p);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                Assert(mask.p[y * w + x] == ((x < 20 && y < 20) ? 255 : 0));
    }
}

Fact("Path Raster Refuses Bad Paths And Small Scratch")
{
    alignas(4) uint8_t buf[64];
    static const uint8_t c[] = { PATH_CMD_M, PATH_CMD_L, PATH_CMD_Q, PATH_CMD_Z };
    static const float p[] = { 1, 1, 9, 1, 9, 9, 1, 9 };
    size_t n = MakePath(c, 4, p, 4, buf);
    PathView pv = {};
    Assert(PathCheck(buf, n, &pv));
    Assert(pv.cmd_count == 4 && pv.point_count == 4);

    Assert(!PathCheck(buf, n - 1, nullptr));            // short
    Assert(!PathCheck(buf, n + 2, nullptr));            // trailing bytes
    Assert(!PathCheck(buf, 3, nullptr));                // no header
    Assert(!PathCheck(nullptr, 0, nullptr));
    Assert(!PathCheck(buf + 1, n, nullptr));            // misaligned points
    buf[2] = 5;                                         // point count doesn't match the commands
    Assert(!PathCheck(buf, n + 4, nullptr));
    buf[2] = 4;
    buf[4 + 2] = PATH_CMD_COUNT;                        // unknown command
    Assert(!PathCheck(buf, n, nullptr));
    buf[4 + 2] = PATH_CMD_Q;
    Assert(PathCheck(buf, n, &pv));

    // Scratch one float short, or bad sizes: refused, mask untouched
    const int w = 12, rows = 10;
    size_t need = PathRasterScratchSize(w, rows);
    Assert(need >= (size_t)w * rows * sizeof(float));
    Bytes acc(need);
    Bytes mask((size_t)w * rows);
    memset(mask.p, MASK_POISON, mask.n);
    Assert(!PathRaster(pv, w, 0, rows, acc.p, need - 4, mask.p));
    Assert(!PathRaster(pv, w, 0, rows, nullptr, need, mask.p));
    Assert(!PathRaster(pv, 0, 0, rows, acc.p, need, mask.p));
    Assert(!PathRaster(pv, w, 0, 0, acc.p, need, mask.p));
    for (size_t i = 0; i < mask.n; i++)
        Assert(mask.p[i] == MASK_POISON);
    Assert(PathRaster(pv, w, 0, rows, acc.p, need, mask.p));
    Assert(!AllZero(mask.p, mask.n));
}
