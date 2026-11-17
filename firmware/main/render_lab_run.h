#pragma once
#include <stddef.h>
#include <stdint.h>
#include "blobpack.h"

// Render lab, device layer (wip/wip-on-device-render-feasibility.md, steps 8
// and 9): in the firmware only with -DCB_RENDER_LAB=ON. A thin layer over the
// host-tested cases and runner (render_lab_cases.h); this file itself is
// device only (FreeRTOS, heap caps, the model) and has no host test.

// The lab's assets: 4-byte aligned copies of render_lab.bin and path_vectors.bin.
// Call before anything else here.
void render_lab_init(const BLOBPACK *lab, const void *vectors, size_t vectors_size);

// The boot run: every case timed on a core 0 task (results logged, one line a
// case), then the 1U and 2U results left as keys on page 0 (replacing the
// stored layout's Home) with a state change timed until on screen. Blocks
// until done. Call after the display and the stored layout's Home are up,
// before touch, USB and Wi-Fi.
void render_lab_run(void);

// The stress run's render load: a core 0 task rendering case `index` and
// putting the result on image `id` (model_put_image, then Collect) back to
// back for ~18 ms, then a tick's rest, until stopped. `psram`: the picture
// buffer in PSRAM (a new heavy PSRAM writer), else internal RAM where it fits.
// False if the case or its buffers can't be had.
bool render_lab_churn_start(int index, uint32_t id, bool psram);

struct RenderLabChurnStats
{
    const char *name;               // the case
    uint32_t runs, failed;         // render + encode + put cycles, and those that failed
    uint64_t busy_us;               // their time in all
    char     where[24];             // where the buffers were (px/scratch/ws/out: I internal, P PSRAM, - none)
};

// Stop the task (waits for its current run) and free its buffers
void render_lab_churn_stop(RenderLabChurnStats *out);
