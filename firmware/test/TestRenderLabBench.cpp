// Render lab host bench (wip/wip-on-device-render-feasibility.md, step 7): times
// every case of render_lab_cases.cpp and prints a table. Indicative only (x86,
// MSVC); the device run (render_lab_run.cpp) gives the real numbers. Runs only
// when RENDER_BENCH is set (so `mk run` stays quick):
//
//   RENDER_BENCH=1 mk config=release run
//
// The cases' pictures are checked by "Render Lab Cases Pass Every Check".

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"
#include "path_vectors.h"
#include "render_lab_cases.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace SimpleLib;

namespace
{
    // Nanoseconds from QueryPerformanceCounter (test-only timer: port::Cycles() is 0 here)
    uint64_t BenchClock()
    {
#ifdef _WIN32
        static LARGE_INTEGER freq = {};
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (uint64_t)((double)t.QuadPart * 1e9 / (double)freq.QuadPart);
#else
        return 0;
#endif
    }

    #define BENCH_BAND      16                  // banded render: the status bar's band height
    #define BENCH_TARGET_NS 100000000ull        // time each case for about this long
}

Fact("Render Lab Bench")
{
    if (!getenv("RENDER_BENCH"))
        return;

    static RenderLabAssets assets = {};
    assets.lab = LoadAsset("render_lab.bin");
    assets.vectors = PathVectorFile(&assets.vectors_size);

    // One set of buffers big enough for every case (the largest image)
    const size_t px_bytes = (size_t)KEY_GRID_W * KEY_GRID_H * 2;
    const size_t scratch_size = 256 * 1024, ws_size = IMAGE_ENCODE_WORKSPACE_SIZE(65535);
    const size_t out_size = 2 * px_bytes;
    RenderLabBuffers b = {};
    b.px = (uint16_t *)malloc(px_bytes);           b.px_bytes = px_bytes;
    b.scratch = malloc(scratch_size);               b.scratch_size = scratch_size;
    b.ws = malloc(ws_size);                         b.ws_size = ws_size;
    b.out = malloc(out_size);                       b.out_size = out_size;
    b.band_rows = BENCH_BAND;

    void *outs[32] = {};
    printf("\nRender lab bench (x86, %s): mean per run; banded = %d-row bands\n",
#ifdef NDEBUG
           "release",
#else
           "debug",
#endif
           BENCH_BAND);
    for (int i = 0; i < RenderLabCaseCount(); i++) {
        RenderLabCase c;
        Assert(RenderLabCaseGet(i, &assets, &c));
        const void *src = c.kind == RENDER_CASE_RECOLOR ? outs[c.source] : nullptr;

        // One run to find the cost, then enough reps for ~BENCH_TARGET_NS
        RenderLabResult r;
        uint64_t t0 = BenchClock();
        Assert(RenderLabCaseRun(&c, &b, src, BenchClock, 1, &r));
        int reps = RenderLabRepsFor(BenchClock() - t0, BENCH_TARGET_NS, 5000);
        Assert(RenderLabCaseRun(&c, &b, src, BenchClock, reps, &r));

        outs[i] = malloc(r.size);
        memcpy(outs[i], b.out, r.size);

        // The device run logs the same line (render_lab_run.cpp)
        char line[256];
        RenderLabFormatResult(&c, &r, line, sizeof(line));
        printf("%s | %d reps\n", line, reps);
    }
    printf("\n");

    for (void *p : outs)
        free(p);
    free(b.px);
    free(b.scratch);
    free(b.ws);
    free(b.out);
}
