#pragma once
#include "esp_err.h"
#include "page_compositor.h"

// Page compositor on the device: runs PageCompositor (page_compositor.h,
// which has the constants and types) in its own task.
// Spec: docs/page-compositor.md.

// Allocate the 3 page buffers in PSRAM, fill each with COMP_BACKGROUND,
// mark every record unknown, and start the compositor task (core 1).
// Call before the display starts.
esp_err_t comp_init(comp_get_page_t get_page, void *ctx);

// Page `page`'s target has changed. Wakes the compositor task.
// flags: COMP_FLUSH holds the page back until all its changes are drawn;
// call it while still holding the lock the change was made under.
// Any task.
void comp_page_changed(int page, uint32_t flags);

// Whole-layout reload. Blocks until no key is being drawn. On return no
// client image is referenced. Any task except the compositor's.
void comp_reset(void);

// Bounce fill (screen.cpp), ISR context, at frame start.
// See PageCompositor::Frame().
int comp_frame(int first, int count, int offset, comp_view_t out[2]);

// Bounce fill (screen.cpp), ISR context, at the end of every fill: the gap
// before the next fill is when the compositor copies key slices to PSRAM.
void comp_fill_done(void);

// Counters since the last call (resets them).
void comp_get_stats(comp_stats_t *stats);
