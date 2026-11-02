#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"
#include "status_bar.h"

using namespace SimpleLib;

#define W SCREEN_W
#define H SCREEN_STATUS_H

namespace
{
    uint16_t px[W * H];

    struct Box { int x0, y0, x1, y1; bool any; };   // inclusive

    // Render through StatusBar's setters, as the device does
    void Render(const char *left, const char *right, int selected = 0, int pill_count = 0)
    {
        static BLOBPACK *ui = LoadAsset("ui.bin");
        static uint16_t spare[W * H];
        StatusBar bar;
        bar.Init(px, spare);
        bar.SetLeft(left);
        bar.SetRight(right);
        bar.SetPillCount(pill_count);
        bar.SetSelectedPill(selected);

        status_bar_style_t style = status_bar_style(ui);
        status_bar_content_t content;
        uint16_t *back = bar.BeginDraw(nullptr, &content);
        if (!back) {                        // nothing set: draw the defaults
            back = px;
            content = {};
        }
        Assert(back == px);
        status_bar_render(back, &style, &content);
        bar.EndDraw();
    }

    bool Lit(int x, int y) { return px[y * W + x] != STATUS_BAR_BACKGROUND; }

    // Bounding box of everything drawn in columns x0..x1
    Box Drawn(int x0 = 0, int x1 = W - 1)
    {
        Box b = { W, H, -1, -1, false };
        for (int y = 0; y < H; y++)
            for (int x = x0; x <= x1; x++)
                if (Lit(x, y)) {
                    if (x < b.x0) b.x0 = x;
                    if (x > b.x1) b.x1 = x;
                    if (y < b.y0) b.y0 = y;
                    if (y > b.y1) b.y1 = y;
                    b.any = true;
                }
        return b;
    }

    bool HasPixel(uint16_t color)
    {
        for (int i = 0; i < W * H; i++)
            if (px[i] == color)
                return true;
        return false;
    }

    // Widths of the runs of drawn pixels along row y
    List<int> Runs(int y)
    {
        List<int> runs;
        int n = 0;
        for (int x = 0; x <= W; x++) {
            if (x < W && Lit(x, y))
                n++;
            else if (n) {
                runs.Add(n);
                n = 0;
            }
        }
        return runs;
    }

    // Save the bar as a 24-bit BMP to look at
    void WritePreview(const char *path)
    {
        FILE *f = fopen(path, "wb");
        if (!f)
            return;
        uint32_t row = W * 3, size = 54 + row * H;
        uint8_t hdr[54] = { 'B', 'M' };
        auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) hdr[at + i] = (uint8_t)(v >> (i * 8)); };
        put32(2, size);
        put32(10, 54);
        put32(14, 40);
        put32(18, W);
        put32(22, (uint32_t)-H);            // top down
        hdr[26] = 1;
        hdr[28] = 24;
        fwrite(hdr, 1, sizeof(hdr), f);
        for (int i = 0; i < W * H; i++) {
            uint16_t c = px[i];
            uint8_t bgr[3] = { (uint8_t)((c & 0x1F) << 3), (uint8_t)(((c >> 5) & 0x3F) << 2), (uint8_t)((c >> 11) << 3) };
            fwrite(bgr, 1, 3, f);
        }
        fclose(f);
    }
}

Fact("StatusBar Empty Is Background")
{
    Render(nullptr, "", 0, 0);
    Assert(!Drawn().any);
}

Fact("StatusBar Left Text Starts At Margin")
{
    Render("AppleTV", nullptr);
    Box b = Drawn();
    Assert(b.any);
    Assert(b.x0 >= STATUS_BAR_MARGIN && b.x0 <= STATUS_BAR_MARGIN + 2);
    Assert(b.x1 < W / 2);
    Assert(HasPixel(STATUS_BAR_TEXT));
}

Fact("StatusBar Right Text Ends At Margin")
{
    Render(nullptr, "12.35am");
    Box b = Drawn();
    Assert(b.any);
    Assert(b.x1 <= W - 1 - STATUS_BAR_MARGIN && b.x1 >= W - 1 - STATUS_BAR_MARGIN - 2);
    Assert(b.x0 > W / 2);
    Assert(HasPixel(STATUS_BAR_TEXT));
}

Fact("StatusBar Text Centred On Cap Height")
{
    Render("H", "H");
    Box b = Drawn();
    int above = b.y0, below = H - 1 - b.y1;
    Assert(above - below >= -1 && above - below <= 1);
}

Fact("StatusBar Pager Centred")
{
    for (int count = 1; count <= 5; count++) {
        Render(nullptr, nullptr, 0, count);
        Box b = Drawn();
        Assert(b.any);
        int left = b.x0, right = W - 1 - b.x1;
        Assert(left - right >= -1 && left - right <= 1);
        int above = b.y0, below = H - 1 - b.y1;
        Assert(above - below >= -1 && above - below <= 1);
    }
}

Fact("StatusBar Pager Highlights Page In View")
{
    for (int page = 0; page < 4; page++) {
        Render(nullptr, nullptr, page, 4);
        List<int> runs = Runs(H / 2);
        Assert(runs.GetCount() == 4);
        for (int i = 0; i < 4; i++)
            if (i != page)
                Assert(runs[page] > runs[i]);
    }
}

Fact("StatusBar Text And Pager Together")
{
    Render("AppleTV", "12.35am", 1, 3);
    Assert(Drawn(0, W / 4).any);
    Assert(Drawn(W * 3 / 4, W - 1).any);
    Assert(Runs(H / 2).GetCount() >= 3);       // the pills, plus any text crossing the row
    WritePreview("build/status_bar.bmp");
}

// ---------------------------------------------------------------------------
// Properties and buffers

namespace
{
    uint16_t buf0[1], buf1[1];      // never drawn into here
}

Fact("StatusBar Setters Report Only Real Changes")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    Assert(!bar.Pending());

    Assert(bar.SetLeft("AppleTV"));
    Assert(!bar.SetLeft("AppleTV"));
    Assert(bar.SetLeft("Kitchen"));
    Assert(bar.SetRight("12.35am"));
    Assert(!bar.SetRight("12.35am"));
    Assert(bar.SetRight(nullptr));
    Assert(!bar.SetRight(""));                  // NULL is the same as ""
    Assert(bar.SetPillCount(3));
    Assert(!bar.SetPillCount(3));
    Assert(bar.SetSelectedPill(1));
    Assert(!bar.SetSelectedPill(1));
    Assert(bar.Pending());
}

Fact("StatusBar Nothing To Draw Until A Change")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    status_bar_content_t c;
    Assert(bar.BeginDraw(nullptr, &c) == nullptr);
    Assert(bar.Front() == nullptr);

    // Setting a value back to what's drawn isn't a change
    bar.SetSelectedPill(0);
    Assert(!bar.Pending());
}

Fact("StatusBar Draws Into The Back Buffer And Swaps")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    status_bar_content_t c;

    bar.SetLeft("A");
    Assert(bar.BeginDraw(nullptr, &c) == buf0);
    Assert(strcmp(c.left, "A") == 0);
    bar.EndDraw();
    Assert(bar.Front() == buf0 && !bar.Pending());

    // Until the fill latches buf0 it may still be reading buf1: wait
    bar.SetRight("B");
    Assert(bar.BeginDraw(nullptr, &c) == nullptr);
    Assert(bar.Pending());

    // Once latched, draw into the other buffer, with both texts
    Assert(bar.BeginDraw(buf0, &c) == buf1);
    Assert(strcmp(c.left, "A") == 0 && strcmp(c.right, "B") == 0);
    bar.EndDraw();
    Assert(bar.Front() == buf1);

    // And back again
    bar.SetPillCount(4);
    Assert(bar.BeginDraw(buf0, &c) == nullptr);
    Assert(bar.BeginDraw(buf1, &c) == buf0);
    Assert(c.pill_count == 4);
    bar.EndDraw();
    Assert(bar.Front() == buf0);
}

Fact("StatusBar Change While Drawing Draws Again")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    status_bar_content_t c;

    bar.SetSelectedPill(1);
    Assert(bar.BeginDraw(nullptr, &c) == buf0);
    Assert(bar.SetSelectedPill(2));
    bar.EndDraw();
    Assert(bar.Pending());
    Assert(bar.BeginDraw(buf0, &c) == buf1);
    Assert(c.selected_pill == 2);
    bar.EndDraw();
    Assert(!bar.Pending());
}

Fact("StatusBar Long Text Is Cut On A Character Boundary")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    status_bar_content_t c;

    char text[STATUS_BAR_TEXT_MAX * 2 + 1] = {};
    for (int i = 0; i < STATUS_BAR_TEXT_MAX; i++) {
        text[i * 2]     = (char)0xC3;      // U+00E9, 2 bytes
        text[i * 2 + 1] = (char)0xA9;
    }
    Assert(bar.SetLeft(text));
    Assert(!bar.SetLeft(text));             // the same long text again isn't a change

    bar.BeginDraw(nullptr, &c);
    size_t len = strlen(c.left);
    Assert(len < STATUS_BAR_TEXT_MAX && len % 2 == 0 && len >= STATUS_BAR_TEXT_MAX - 2);
    Assert(memcmp(c.left, text, len) == 0);
}
