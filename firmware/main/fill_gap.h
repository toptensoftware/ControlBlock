#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Fill gaps on the device: the time after each bounce fill, when PSRAM may be
// written (see "Bounce fill rules" in CLAUDE.md). Tasks that write PSRAM
// register a client; while `wants` is set, each gap goes to one client in
// turn (gap_share.h), as a task notification of `bit`. The task checks
// fill_gap_fresh() before writing: a gap it reaches late is skipped.

#define FILL_GAP_MAX_CLIENTS  2

typedef struct {
    TaskHandle_t  task;
    uint32_t      bit;          // notification bit set when it is given a gap
    volatile bool wants;        // the task has something to write
} fill_gap_client_t;

// At init, before the display starts. The client must stay valid.
void fill_gap_register(fill_gap_client_t *client);

// Bounce fill (screen.cpp), ISR context, at the end of every fill.
void fill_gap_done(void);

// The last fill ended less than `max_age_us` ago
bool fill_gap_fresh(uint32_t max_age_us);
