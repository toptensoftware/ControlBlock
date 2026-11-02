#pragma once

// Frame-start maths for the bounce fill, kept pure so the host tests can
// check it: which pages the scroll position shows, and what to show given
// the buffers the compositor returned.

#include <stdint.h>
#include "page_compositor.h"

// The pages a scroll position shows
struct PagesInView
{
    int left;       // page at the left edge of the screen (may be < 0 or past the last)
    int offset;     // how far `left` is scrolled off the screen, 0..SCREEN_PAGE_W-1
    int first;      // first page to ask the compositor for (existing pages only)
    int count;      // how many (0-2)
};

// What the bounce fill shows for a frame
struct FrameView
{
    const uint16_t *px[2];  // the page at the left edge and the one after (NULL = background)
    int             offset; // how far px[0] is scrolled off the screen
};

// Pages shown at `scroll` (page i is fully in view at i * SCREEN_PAGE_W),
// skipping any before page 0 or from `num_pages` on.
PagesInView screen_pages_in_view(int scroll, int num_pages);

// What to show, given the `n` buffers comp_frame() returned in `v`:
//  - every page asked for: show them as scrolled
//  - otherwise the first buffer at rest (the hold), or background if none
FrameView screen_resolve_view(const PagesInView &pages, const comp_view_t *v, int n);
