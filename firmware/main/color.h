#pragma once
#include <stdint.h>

// 8-bit-per-channel RGB to RGB565
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
