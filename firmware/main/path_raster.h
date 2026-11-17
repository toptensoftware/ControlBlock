#pragma once
#include <stddef.h>
#include <stdint.h>

// Render lab (wip/wip-on-device-render-feasibility.md, step 6): an anti-aliased
// path rasteriser for icons and glyph outlines. A port of rle565's font
// rasteriser (rle565/encoder/font.js: flattenPath, accumulateLine and the
// prefix sum): exact signed-area coverage, non-zero fill, curves flattened to
// lines. Single-precision float, no heap: the caller gives the accumulator.
// Pure (host tested); in the firmware only with -DCB_RENDER_LAB=ON.
//
// Path format (tools/render-lab/path-format.mjs), little endian, absolute
// commands, pixels of the path's box (y down):
//
//   u16 cmdCount, u16 pointCount
//   u8  cmds[cmdCount]          PATH_CMD_*: M 1 point, L 1, Q 2, C 3, Z 0
//   pad to 2 bytes
//   i16 points[pointCount][2]   x, y in 1/PATH_POINT_SCALE px

enum : uint8_t
{
    PATH_CMD_M,
    PATH_CMD_L,
    PATH_CMD_Q,
    PATH_CMD_C,
    PATH_CMD_Z,
    PATH_CMD_COUNT,
};

#define PATH_POINT_SCALE    16

struct PathView
{
    uint16_t       cmd_count;
    uint16_t       point_count;
    const uint8_t *cmds;
    const int16_t *points;      // 2-byte aligned
};

// Check a path from outside (`size` bytes, 2-byte aligned): commands known,
// point count matching them and the size. Fills `out` if not NULL.
bool PathCheck(const void *data, size_t size, PathView *out);

// A checked path, without the checks
PathView PathOpen(const void *data);

// Accumulator bytes PathRaster() needs for a `w` wide band of `rows` rows
size_t PathRasterScratchSize(int w, int rows);

// Rasterise rows y0..y0+rows-1 of the path's `w` wide box into `mask`
// (w * rows coverage bytes, stride w, 0..255). Whatever lies outside the box
// is clipped (to the left of it still counts toward the coverage inside, as
// the whole path would). Any banding gives the same bytes as a whole render.
// Returns false (mask untouched) if `scratch` (4-byte aligned) is smaller
// than PathRasterScratchSize(w, rows), or w or rows isn't positive.
bool PathRaster(const PathView &path, int w, int y0, int rows,
                void *scratch, size_t scratch_size, uint8_t *mask);
