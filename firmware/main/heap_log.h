#pragma once

// One log line for the heap. Internal RAM is the scarce one (USB, Wi-Fi and
// the display all want it): the free total, the largest block and the lowest
// the free total has ever been.

#include "esp_heap_caps.h"
#include "esp_log.h"

static inline void log_heap(const char *tag, const char *when)
{
    ESP_LOGI(tag, "heap %s: internal %u free (largest %u, min ever %u), PSRAM %u free",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
