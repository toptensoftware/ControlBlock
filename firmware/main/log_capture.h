#pragma once

// Captures ESP_LOGx output into a 32 KB PSRAM LogRing for the diagnostics
// endpoints, and still sends it to the UART. Device only.
//
// The vprintf hook never blocks: a line logged while another task is writing
// the ring is left out of the ring (it still reaches the UART). Early/ISR logs
// bypass vprintf and are not captured.

#include <stddef.h>
#include <stdint.h>
#include "log_ring.h"

#define LOG_CAPTURE_RING_SIZE   (32 * 1024)
// One log call is formatted into this much PSRAM; a longer message is cut (on
// a character boundary). Multi-line messages (TinyUSB's descriptor table, ~1.5 KB
// of box drawing) need room.
#define LOG_CAPTURE_MSG_MAX     (4 * 1024)

// First thing in app_main. Safe to call once; logs and carries on if PSRAM is short.
void log_capture_init(void);

// Copies lines after `since` (all when !hasSince) into out; see LogRing::Read.
// Waits (briefly) for a writer to finish rather than dropping the read.
LogReadResult log_capture_read(bool hasSince, uint32_t since, char *out, size_t cap);

// The next line's sequence number (0 before init)
uint32_t log_capture_next_seq(void);
