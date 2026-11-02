#include "screen_view.h"

IRAM_ATTR PagesInView screen_pages_in_view(int scroll, int num_pages)
{
    PagesInView p;

    // Split the scroll position into the page at the left edge of the
    // screen and how far into it we are (rounding down for negatives)
    p.left   = scroll >= 0 ? scroll / SCREEN_PAGE_W
                           : -((-scroll + SCREEN_PAGE_W - 1) / SCREEN_PAGE_W);
    p.offset = scroll - p.left * SCREEN_PAGE_W;

    // The one or two pages in view, skipping any before the first page or
    // past the last (rubber-banding shows background there)
    p.first = p.left;
    p.count = p.offset ? 2 : 1;
    if (p.first < 0) {
        p.count += p.first;
        p.first  = 0;
    }
    if (p.first + p.count > num_pages)
        p.count = num_pages - p.first;
    if (p.count < 0)
        p.count = 0;
    return p;
}

int screen_page_most_in_view(int scroll, int num_pages)
{
    // The page at the left edge once scrolled by half a page
    int page = screen_pages_in_view(scroll + SCREEN_PAGE_W / 2, num_pages).left;
    if (page >= num_pages)
        page = num_pages - 1;
    return page < 0 ? 0 : page;
}

IRAM_ATTR FrameView screen_resolve_view(const PagesInView &pages, const comp_view_t *v, int n)
{
    FrameView f;
    f.px[0]  = f.px[1] = nullptr;
    f.offset = pages.offset;

    if (n > 0 && n == pages.count && v[0].page == pages.first) {
        // Every page asked for: show them as scrolled
        for (int i = 0; i < n; i++) {
            int slot = v[i].page - pages.left;
            if (slot == 0 || slot == 1)
                f.px[slot] = v[i].px;
        }
    } else if (n > 0) {
        // A page asked for isn't ready (or a reload is pending): hold the
        // first buffer returned at rest
        f.px[0]  = v[0].px;
        f.offset = 0;
    }
    return f;
}
