#pragma once
#include "rle565.h"

// Text placement shared by the status bar and the render lab (pure).

// Height of capital letters above the baseline ('H', else the ascent)
static inline int font_cap_height(const FONT *font)
{
    const FONT_GLYPH *cap = font_find_glyph(font, 'H');
    return cap ? -cap->top : font->ascent;
}

// Baseline, from the top of a box `box_h` high, that centres capitals in it
static inline int font_centre_baseline(const FONT *font, int box_h)
{
    return (box_h + font_cap_height(font)) / 2;
}
