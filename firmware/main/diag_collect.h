#pragma once

// Fills the diagnostics snapshot (diag_status.h) from the device's getters.
// Device layer only (hardware).

#include "diag_status.h"
#include "compositor.h"
#include "screen.h"

// Once a second from main's loop: reads (and so resets) the screen and
// compositor stats, keeps them as "the last second" for diag_collect, and
// hands them back for the stats log. The only reader of those stats.
void diag_sample_second(screen_stats_t *st, comp_stats_t *cs);

// The snapshot now (any task; takes the model lock briefly)
void diag_collect(DiagStatus *out);
