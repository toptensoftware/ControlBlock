#include "UnitTesting.h"
#include "Core.h"
#include "screen_view.h"

using namespace SimpleLib;

#define W SCREEN_PAGE_W

namespace
{
    bool Is(const PagesInView &p, int left, int offset, int first, int count)
    {
        return p.left == left && p.offset == offset && p.first == first && p.count == count;
    }

    uint16_t px_a[4], px_b[4];
}

Fact("ScreenView Page At Rest")
{
    Assert(Is(screen_pages_in_view(0, 3), 0, 0, 0, 1));
    Assert(Is(screen_pages_in_view(W, 3), 1, 0, 1, 1));
    Assert(Is(screen_pages_in_view(2 * W, 3), 2, 0, 2, 1));
}

Fact("ScreenView Between Two Pages")
{
    Assert(Is(screen_pages_in_view(100, 3), 0, 100, 0, 2));
    Assert(Is(screen_pages_in_view(W + W - 1, 3), 1, W - 1, 1, 2));
}

Fact("ScreenView Rubber Band Before First Page")
{
    // Page -1 doesn't exist: only page 0 is asked for
    Assert(Is(screen_pages_in_view(-1, 3), -1, W - 1, 0, 1));
    Assert(Is(screen_pages_in_view(-300, 3), -1, W - 300, 0, 1));
    Assert(Is(screen_pages_in_view(-W, 3), -1, 0, 0, 0));
}

Fact("ScreenView Rubber Band Past Last Page")
{
    Assert(Is(screen_pages_in_view(2 * W + 300, 3), 2, 300, 2, 1));
    Assert(Is(screen_pages_in_view(3 * W, 3), 3, 0, 3, 0));
}

Fact("ScreenView Single Page")
{
    Assert(Is(screen_pages_in_view(0, 1), 0, 0, 0, 1));
    Assert(Is(screen_pages_in_view(200, 1), 0, 200, 0, 1));
    Assert(Is(screen_pages_in_view(-200, 1), -1, W - 200, 0, 1));
}

Fact("ScreenView Resolve Both Pages As Scrolled")
{
    PagesInView p = screen_pages_in_view(100, 3);
    comp_view_t v[2] = { { 0, px_a }, { 1, px_b } };
    FrameView f = screen_resolve_view(p, v, 2);
    Assert(f.px[0] == px_a && f.px[1] == px_b && f.offset == 100);
}

Fact("ScreenView Resolve Rubber Band Shows Background Off The End")
{
    PagesInView p = screen_pages_in_view(-300, 3);
    comp_view_t v[1] = { { 0, px_a } };
    FrameView f = screen_resolve_view(p, v, 1);
    Assert(f.px[0] == nullptr && f.px[1] == px_a && f.offset == W - 300);

    p = screen_pages_in_view(2 * W + 300, 3);
    v[0] = { 2, px_b };
    f = screen_resolve_view(p, v, 1);
    Assert(f.px[0] == px_b && f.px[1] == nullptr && f.offset == 300);
}

Fact("ScreenView Resolve Hold Shows Ready Page At Rest")
{
    // Asked for 0 and 1, only 0 came back
    PagesInView p = screen_pages_in_view(100, 3);
    comp_view_t v[1] = { { 0, px_a } };
    FrameView f = screen_resolve_view(p, v, 1);
    Assert(f.px[0] == px_a && f.px[1] == nullptr && f.offset == 0);

    // Only 1 came back
    v[0] = { 1, px_b };
    f = screen_resolve_view(p, v, 1);
    Assert(f.px[0] == px_b && f.px[1] == nullptr && f.offset == 0);
}

Fact("ScreenView Resolve Fallback After Reload Shows First At Rest")
{
    PagesInView p = screen_pages_in_view(W, 3);
    comp_view_t v[2] = { { COMP_NO_PAGE, px_a }, { COMP_NO_PAGE, px_b } };
    FrameView f = screen_resolve_view(p, v, 2);
    Assert(f.px[0] == px_a && f.px[1] == nullptr && f.offset == 0);
}

Fact("ScreenView Resolve Nothing Shows Background")
{
    PagesInView p = screen_pages_in_view(0, 3);
    FrameView f = screen_resolve_view(p, nullptr, 0);
    Assert(f.px[0] == nullptr && f.px[1] == nullptr);
}
