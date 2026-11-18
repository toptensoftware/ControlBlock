#pragma once
#include "screen.h"

// Frame rate experiment (wip/wip-frame-rate-experiment.md; PCLK_CYCLE, -DCB_PCLK_CYCLE=ON):
// steps the pixel clock through display_timing.h's PCLK_SCHEDULE, logging each switch (how long
// it took to apply, the frame periods around it, the measured clock) and each hold (fps, fill
// stats). Shows the clock as the status bar's left text. Device only. Phase 2 (VBLANK_CYCLE,
// -DCB_VBLANK_CYCLE=ON) steps VBLANK_SCHEDULE instead: the frame size (front porch) at 20 and 16 MHz.
// Phase 4 (PORCH_CYCLE, -DCB_PORCH_CYCLE=ON) steps PORCH_SCHEDULE: 500 <-> 744 lines, front or back
// porch, with or without lcd_update, and logs where VSYNC_END fires (its time to the y = 30 fill).

#if PCLK_CYCLE
// Once a second from main's loop, with that second's stats (diag_sample_second). May block for
// up to one frame while a new clock applies.
void pclk_cycle_second(const screen_stats_t *st);
#endif
