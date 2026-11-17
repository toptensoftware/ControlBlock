#pragma once
#include <stddef.h>
#include <stdint.h>
#include "blobpack.h"
#include "key_geometry.h"
#include "render_lab.h"

// Render lab benchmark cases (wip/wip-on-device-render-feasibility.md, step 7):
// the key looks the host bench (firmware/test/TestRenderLabBench.cpp) and the
// device run (render_lab_run.cpp) both time, built over the lab's assets, and
// the runner that times one. Pure (host tested); in the firmware only with
// -DCB_RENDER_LAB=ON.

// --- Path vector file (tools/render-lab/path-format.mjs) -------------------

// One entry of a "PVEC" file: a path in the render lab's path format
// (path_raster.h) in a w x h box, and the JS reference rasteriser's mask
struct RenderLabVector
{
    char           name[32];
    int            x;               // the box's x in its glyph run (glyph-<run>-<i>), else 0
    int            w, h;
    const uint8_t *path;            // 4-byte aligned if the file is
    size_t         path_size;
    const uint8_t *mask;            // w * h coverage bytes
};

// Read up to `max` entries of the vector file (`size` bytes) into `out`.
// Returns the entry count, or -1 if the file is malformed or has more than `max`.
int RenderLabVectorsRead(const void *file, size_t size, RenderLabVector *out, int max);

// --- Cases -----------------------------------------------------------------

struct RenderLabAssets
{
    const BLOBPACK *lab;            // firmware/assets/render_lab.bin (render_lab_assets.h)
    const void     *vectors;        // firmware/test/data/path_vectors.bin
    size_t          vectors_size;
};

enum : uint8_t
{
    RENDER_CASE_RENDER,             // render `layers` at w x h, then encode
    RENDER_CASE_RECOLOR,            // recolour copy (RenderLabRecolorCopy) of case `source`'s encoded image
};

#define RENDER_LAB_CASE_LAYERS      8
#define RENDER_LAB_CASE_PALETTE     256     // recoloured palette entries a case can hold

struct RenderLabCase
{
    const char *name;
    uint8_t     kind;               // RENDER_CASE_*
    int         w, h;
    int         n;                  // layers used
    RenderLayer layers[RENDER_LAB_CASE_LAYERS];
    int         source;             // recolour: the case whose image it copies
    uint32_t    color_a, color_b;   // recolour (copy, or a layer's palette override): 0xRRGGBB
    int         shift;
    uint32_t    palette[1 + RENDER_LAB_CASE_PALETTE];   // a palette override's storage (PALETTE)
};

// The cases, by index (RenderLabCaseGet)
enum
{
    RENDER_LAB_CASE_FACE_LABEL,         // 1U face + 20 px label
    RENDER_LAB_CASE_FACE_LABEL_SP4,     // 1U face + 27 px subpixel 4 label
    RENDER_LAB_CASE_RECOLOR_LABEL,      // 1U face through a recoloured palette + label
    RENDER_LAB_CASE_SLIDER,             // 1U face, value track and fill, 27 px value
    RENDER_LAB_CASE_ICON_LABEL,         // 1U rle565 icon + label
    RENDER_LAB_CASE_PATH_ICON_LABEL,    // 1U path icon + label
    RENDER_LAB_CASE_GLYPHS,             // 1U 27 px text from glyph outlines
    RENDER_LAB_CASE_2U,                 // 2U two faces, icon, 27 px text
    RENDER_LAB_CASE_FULL,               // 720x432 panel, face, value bar, text
    RENDER_LAB_CASE_RECOLOR_COPY,       // 1U recolour copy of FACE_LABEL (no render, no encode)
    RENDER_LAB_CASE_LARGE_PALETTE,      // 1U with > 256 colours (the large-palette decode)
    RENDER_LAB_CASE_COUNT,
};

// Number of cases (RENDER_LAB_CASE_COUNT)
int RenderLabCaseCount();

// Build case `i` over `assets` into `out` (its layers point into the assets
// and into `out` itself: don't copy it). False if `i` is out of range or an
// asset is missing.
bool RenderLabCaseGet(int i, const RenderLabAssets *assets, RenderLabCase *out);

// --- Runner ----------------------------------------------------------------

// Nanoseconds from any fixed point (host: QueryPerformanceCounter; device: esp_timer)
typedef uint64_t (*RenderLabClock)(void);

struct RenderLabBuffers
{
    uint16_t *px;      size_t px_bytes;         // the whole image, w * h RGB565
    void     *scratch; size_t scratch_size;     // path scratch (4-byte aligned)
    void     *ws;      size_t ws_size;          // encoder workspace (4-byte aligned)
    void     *out;     size_t out_size;         // encoded image (4-byte aligned)
    int       band_rows;                        // also time a render in bands of this many rows (0 = no)
};

struct RenderLabResult
{
    // Sizes (RenderLabCaseSizes; px and scratch for the whole image and for a band)
    size_t   px_whole, px_band;
    size_t   scratch_whole, scratch_band;
    // From the run
    uint64_t render_ns;             // a whole render (recolour: the copy), mean of the reps
    uint64_t band_ns;               // the same picture in bands of band_rows (0 if not timed)
    uint64_t encode_ns;             // colour count + encode (0 for a recolour)
    uint32_t colors;                // distinct colours (recolour: palette entries)
    size_t   workspace;             // encoder workspace needed (RenderLabWorkspaceSize)
    size_t   bound;                 // RenderLabEncodeBound for the colours
    size_t   size;                  // encoded (or copied) size, 0 if it failed
};

// What case `c` needs for bands of `band_rows` rows (0 = whole only): fills the
// size fields of `r` (the rest zeroed)
void RenderLabCaseSizes(const RenderLabCase *c, int band_rows, RenderLabResult *r);

// Run case `c` `reps` times (at least once): a whole render into b->px, the
// banded render if asked, then the encode into b->out; a recolour copies
// `source` (case c->source's encoded image) into b->out. `clock` may be NULL
// (times 0). Fills `r`; returns false if a buffer is too small (r->workspace
// and r->colors still tell what the encode needed) or the render failed.
bool RenderLabCaseRun(const RenderLabCase *c, const RenderLabBuffers *b, const void *source,
                      RenderLabClock clock, int reps, RenderLabResult *r);

// Repeats to time a case for about `target_ns`, given one run took `once_ns`
// (0 = too quick to time): target / once, at most `max`, at least 3
int RenderLabRepsFor(uint64_t once_ns, uint64_t target_ns, int max);

// One result line (host bench and device log alike): the case, its size, the
// times in us, colours, encoded size, bound, workspace, and the 565 and path
// scratch whole/band in KB. Writes at most `size` bytes (terminated, cut if
// need be); returns the full line's length, as snprintf.
size_t RenderLabFormatResult(const RenderLabCase *c, const RenderLabResult *r, char *buf, size_t size);

// --- Showing results as keys ------------------------------------------------

// Place a w x h pixel key (whole cells) at grid cell `*cursor` (row-major) or,
// if it doesn't fit the rest of that row, at the start of the next row; then
// move the cursor past it (past its last row if it's more than one row tall).
// False, with `*cursor` and `out` untouched, if it isn't whole cells or
// there's no room left.
bool RenderLabPlaceKey(int w, int h, int *cursor, KeyRect *out);
