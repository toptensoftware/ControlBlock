#pragma once
#include <stdint.h>
#include "port.h"

// Fill `n` RGB565 pixels with a solid colour, two pixels per 32-bit store. A
// plain per-pixel loop was ~350 us for a 15-line bounce chunk under -Og, more
// than copying a page from PSRAM. In IRAM: the bounce fill (screen.cpp) uses it.
IRAM_ATTR static inline void fill_px(uint16_t *dest, int n, uint16_t color)
{
    if (n > 0 && ((uintptr_t)dest & 2)) {
        *dest++ = color;
        n--;
    }

    uint32_t  c2 = color | ((uint32_t)color << 16);
    uint32_t *d  = (uint32_t *)dest;
    int pairs = n / 2;
    while (pairs >= 8) {
        d[0] = c2; d[1] = c2; d[2] = c2; d[3] = c2;
        d[4] = c2; d[5] = c2; d[6] = c2; d[7] = c2;
        d += 8;
        pairs -= 8;
    }
    while (pairs--)
        *d++ = c2;

    if (n & 1)
        *(uint16_t *)d = color;
}
