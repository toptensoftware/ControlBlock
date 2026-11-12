#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "waveshare_rgb_lcd_port.h"

// Fill gaps on the device: the time after each bounce fill, when PSRAM may be
// written (see "Bounce fill rules" in CLAUDE.md). Tasks that write PSRAM
// register a client; while `wants` is set, each gap goes to one client in
// turn (gap_share.h), as a task notification of `bit`, with the time the
// fill started. The next fill is due FILL_GAP_PERIOD_US after that start, so
// a client works to a deadline from it (the compositor), or checks
// fill_gap_fresh() before writing (the status bar): a gap it reaches late is
// used less, or skipped.

#define FILL_GAP_MAX_CLIENTS  2     // compositor, status bar

// From one fill's start to the next's
#define FILL_GAP_PERIOD_US    DISPLAY_BOUNCE_BUDGET_US

typedef struct {
    TaskHandle_t      task;
    uint32_t          bit;              // notification bit set when it is given a gap
    volatile bool     wants;            // the task has something to write
    volatile uint32_t fill_start_us;    // when the fill that gave it its last gap started
                                        // (low 32 bits of esp_timer_get_time())
} fill_gap_client_t;

// At init, before the display starts. The client must stay valid.
void fill_gap_register(fill_gap_client_t *client);

// Bounce fill (screen.cpp), ISR context, at the end of every fill.
// `fill_start_cycles`: the CPU cycle count when the fill started.
void fill_gap_done(uint32_t fill_start_cycles);

// Time since the last fill that gave out a gap ended, in us
uint32_t fill_gap_age_us(void);

// The last fill ended less than `max_age_us` ago
bool fill_gap_fresh(uint32_t max_age_us);
