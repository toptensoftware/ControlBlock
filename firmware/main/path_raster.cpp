#include <math.h>
#include <string.h>
#include "path_raster.h"

// A port of rle565/encoder/font.js's rasteriser (flattenPath, accumulateLine,
// rasterizeGlyph's prefix sum) to single-precision float. Differences:
// segments are accumulated as they're flattened (no line list), straight
// into the box's own columns (what falls left of the box goes into column 0,
// where the prefix sum would have carried it; right of it is dropped), and
// each row's end points come from the line's equation rather than a running
// x, so a band's rows are the same arithmetic as the whole render's.

static const uint8_t s_cmd_points[PATH_CMD_COUNT] = { 1, 1, 2, 3, 0 };

#define FLATTEN_TOLERANCE   0.02f   // max deviation from a curve, px (as font.js)
#define FLATTEN_MAX_SEGS    64

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static inline size_t cmds_bytes(uint16_t count)
{
    return 4 + count + (count & 1);
}

PathView PathOpen(const void *data)
{
    const uint8_t *b = (const uint8_t *)data;
    PathView pv;
    pv.cmd_count   = rd16(b);
    pv.point_count = rd16(b + 2);
    pv.cmds        = b + 4;
    pv.points      = (const int16_t *)(b + cmds_bytes(pv.cmd_count));
    return pv;
}

bool PathCheck(const void *data, size_t size, PathView *out)
{
    if (!data || ((uintptr_t)data & 1) || size < 4)
        return false;
    const uint8_t *b = (const uint8_t *)data;
    uint16_t count = rd16(b), points = rd16(b + 2);
    size_t head = cmds_bytes(count);
    if (size < head)
        return false;
    uint32_t need = 0;
    for (int i = 0; i < count; i++) {
        if (b[4 + i] >= PATH_CMD_COUNT)
            return false;
        need += s_cmd_points[b[4 + i]];
    }
    if (need != points || size != head + (size_t)points * 4)
        return false;
    if (out)
        *out = PathOpen(data);
    return true;
}

size_t PathRasterScratchSize(int w, int rows)
{
    return w > 0 && rows > 0 ? (size_t)w * rows * sizeof(float) : 0;
}

namespace
{
    // The accumulator for rows top..top+rows-1 of a w wide box
    struct Accum
    {
        float *acc;
        int    w, top, rows;

        // Add d to column x of `row`, clamped as described at the top
        inline void add(float *row, int x, float d) const
        {
            if (x >= w)
                return;
            row[x < 0 ? 0 : x] += d;
        }

        // accumulateLine: one line's signed area
        void line(float x0, float y0, float x1, float y1)
        {
            if (y0 == y1)
                return;
            float dir = 1.0f;
            if (y0 > y1) {
                dir = -1.0f;
                float t = x0; x0 = x1; x1 = t;
                t = y0; y0 = y1; y1 = t;
            }
            int ys = (int)floorf(y0), ye = (int)ceilf(y1);
            if (ys < top)
                ys = top;
            if (ye > top + rows)
                ye = top + rows;
            if (ys >= ye)
                return;
            float dxdy = (x1 - x0) / (y1 - y0);

            for (int y = ys; y < ye; y++) {
                float *row = acc + (size_t)(y - top) * w;
                float ya = (float)y > y0 ? (float)y : y0;
                float yb = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
                float xs = ya == y0 ? x0 : x0 + (ya - y0) * dxdy;
                float xn = yb == y1 ? x1 : x0 + (yb - y0) * dxdy;
                float d  = (yb - ya) * dir;
                float xa = xs < xn ? xs : xn;
                float xb = xs < xn ? xn : xs;
                float xa_floor = floorf(xa);
                int   xai = (int)xa_floor;
                int   xbi = (int)ceilf(xb);

                if (xbi <= xai + 1) {
                    // Within one pixel column
                    float xmf = 0.5f * (xs + xn) - xa_floor;
                    add(row, xai, d - d * xmf);
                    add(row, xai + 1, d * xmf);
                } else {
                    // Spans columns: trapezoid areas at each end, constant between
                    float s   = 1.0f / (xb - xa);
                    float xaf = xa - xa_floor;
                    float a0  = 0.5f * s * (1.0f - xaf) * (1.0f - xaf);
                    float xbf = xb - (float)xbi + 1.0f;
                    float am  = 0.5f * s * xbf * xbf;
                    add(row, xai, d * a0);
                    if (xbi == xai + 2) {
                        add(row, xai + 1, d * (1.0f - a0 - am));
                    } else {
                        float a1 = s * (1.5f - xaf);
                        add(row, xai + 1, d * (a1 - a0));
                        int lo = xai + 2, hi = xbi - 1;         // constant columns [lo, hi)
                        if (hi > w)
                            hi = w;
                        float ds = d * s;
                        if (lo < 0) {
                            // Left of the box: all of it into column 0 at once
                            int left = (hi < 0 ? hi : 0) - lo;
                            if (left > 0)
                                row[0] += ds * (float)left;
                            lo = 0;
                        }
                        for (int x = lo; x < hi; x++)
                            row[x] += ds;
                        float a2 = a1 + (float)(xbi - xai - 3) * s;
                        add(row, xbi - 1, d * (1.0f - a2 - am));
                    }
                    add(row, xbi, d * am);
                }
            }
        }

        // Whether a curve with these control point y values can touch the band
        bool touches(const float *ys, int n) const
        {
            float lo = ys[0], hi = ys[0];
            for (int i = 1; i < n; i++) {
                lo = ys[i] < lo ? ys[i] : lo;
                hi = ys[i] > hi ? ys[i] : hi;
            }
            return hi > (float)top && lo < (float)(top + rows);
        }
    };

    inline int segments(float dev)
    {
        int n = (int)ceilf(sqrtf(dev / FLATTEN_TOLERANCE));
        return n < 1 ? 1 : n > FLATTEN_MAX_SEGS ? FLATTEN_MAX_SEGS : n;
    }

    inline float hyp(float x, float y)
    {
        return sqrtf(x * x + y * y);
    }
}

bool PathRaster(const PathView &path, int w, int y0, int rows,
                void *scratch, size_t scratch_size, uint8_t *mask)
{
    if (w <= 0 || rows <= 0 || !scratch || scratch_size < PathRasterScratchSize(w, rows))
        return false;
    Accum a = { (float *)scratch, w, y0, rows };
    memset(a.acc, 0, PathRasterScratchSize(w, rows));

    // flattenPath
    const float k = 1.0f / PATH_POINT_SCALE;
    const int16_t *p = path.points;
    float sx = 0, sy = 0, cx = 0, cy = 0;
    for (int i = 0; i < path.cmd_count; i++) {
        uint8_t cmd = path.cmds[i];
        float q[6];
        for (int j = 0; j < s_cmd_points[cmd] * 2; j++)
            q[j] = (float)p[j] * k;
        p += s_cmd_points[cmd] * 2;

        switch (cmd) {
        case PATH_CMD_M:
            if (cx != sx || cy != sy)
                a.line(cx, cy, sx, sy);
            sx = cx = q[0];
            sy = cy = q[1];
            break;

        case PATH_CMD_L:
            a.line(cx, cy, q[0], q[1]);
            cx = q[0]; cy = q[1];
            break;

        case PATH_CMD_Q: {
            float ys[3] = { cy, q[1], q[3] };
            if (a.touches(ys, 3)) {
                int n = segments(hyp(cx - 2 * q[0] + q[2], cy - 2 * q[1] + q[3]) / 4);
                float px = cx, py = cy;
                for (int s = 1; s <= n; s++) {
                    float t = (float)s / (float)n, u = 1.0f - t;
                    float x = u * u * cx + 2 * u * t * q[0] + t * t * q[2];
                    float y = u * u * cy + 2 * u * t * q[1] + t * t * q[3];
                    a.line(px, py, x, y);
                    px = x; py = y;
                }
            }
            cx = q[2]; cy = q[3];
            break;
        }

        case PATH_CMD_C: {
            float ys[4] = { cy, q[1], q[3], q[5] };
            if (a.touches(ys, 4)) {
                float d1 = hyp(cx - 2 * q[0] + q[2], cy - 2 * q[1] + q[3]);
                float d2 = hyp(q[0] - 2 * q[2] + q[4], q[1] - 2 * q[3] + q[5]);
                int n = segments(0.75f * (d1 > d2 ? d1 : d2));
                float px = cx, py = cy;
                for (int s = 1; s <= n; s++) {
                    float t = (float)s / (float)n, u = 1.0f - t;
                    float x = u * u * u * cx + 3 * u * u * t * q[0] + 3 * u * t * t * q[2] + t * t * t * q[4];
                    float y = u * u * u * cy + 3 * u * u * t * q[1] + 3 * u * t * t * q[3] + t * t * t * q[5];
                    a.line(px, py, x, y);
                    px = x; py = y;
                }
            }
            cx = q[4]; cy = q[5];
            break;
        }

        case PATH_CMD_Z:
            a.line(cx, cy, sx, sy);
            cx = sx; cy = sy;
            break;
        }
    }
    if (cx != sx || cy != sy)
        a.line(cx, cy, sx, sy);

    // Prefix sum per row: non-zero coverage
    for (int y = 0; y < rows; y++) {
        const float *row = a.acc + (size_t)y * w;
        uint8_t *out = mask + (size_t)y * w;
        float sum = 0;
        for (int x = 0; x < w; x++) {
            sum += row[x];
            float v = fabsf(sum);
            out[x] = (uint8_t)((v < 1.0f ? v : 1.0f) * 255.0f + 0.5f);
        }
    }
    return true;
}
