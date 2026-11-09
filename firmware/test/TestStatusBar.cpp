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
    for (int count = 2; count <= 5; count++) {
        Render(nullptr, nullptr, 0, count);
        Box b = Drawn();
        Assert(b.any);
        int left = b.x0, right = W - 1 - b.x1;
        Assert(left - right >= -1 && left - right <= 1);
        int above = b.y0, below = H - 1 - b.y1;
        Assert(above - below >= -1 && above - below <= 1);
    }
}

// One page (Home with no activity, or a host's single page) has nothing to page between
Fact("StatusBar One Page Shows No Pager")
{
    Render(nullptr, nullptr, 0, 1);
    Assert(!Drawn().any);
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
// Drawing in bands: the device renders a few rows at a time into a small
// internal buffer and copies them to the PSRAM bar in fill gaps

namespace
{
    uint16_t banded[W * H];

    // Draw the bar `rows` rows at a time through a scratch buffer
    void RenderBanded(int rows, const status_bar_content_t *content)
    {
        static BLOBPACK *ui = LoadAsset("ui.bin");
        status_bar_style_t style = status_bar_style(ui);
        static uint16_t scratch[W * STATUS_BAR_BAND_ROWS_MAX];
        // A band bigger than the scratch would overrun it (and, in the debug
        // build, silently stomp the test registry's statics)
        Assert(rows <= STATUS_BAR_BAND_ROWS_MAX);
        memset(banded, 0xAB, sizeof(banded));       // anything not drawn shows
        for (int y = 0; y < H; y += rows) {
            int n = rows < H - y ? rows : H - y;
            status_bar_render_rows(scratch, y, n, &style, content);
            memcpy(banded + y * W, scratch, n * W * sizeof(uint16_t));
        }
    }
}

Fact("StatusBar Bands Match The Whole Bar")
{
    static BLOBPACK *ui = LoadAsset("ui.bin");
    status_bar_style_t style = status_bar_style(ui);
    status_bar_content_t content = {};
    strcpy(content.left, "AppleTV");
    strcpy(content.right, "12.35am");
    content.pill_count = 4;
    content.selected_pill = 2;

    status_bar_render(px, &style, &content);
    int sizes[] = { 1, 2, 3, STATUS_BAR_BAND_ROWS_MAX };      // 3 doesn't divide the bar's height
    for (int rows : sizes) {
        RenderBanded(rows, &content);
        Assert(memcmp(px, banded, sizeof(px)) == 0);
    }
}

Fact("StatusBar Bands Match When Empty")
{
    static BLOBPACK *ui = LoadAsset("ui.bin");
    status_bar_style_t style = status_bar_style(ui);
    status_bar_content_t content = {};
    status_bar_render(px, &style, &content);
    RenderBanded(2, &content);
    Assert(memcmp(px, banded, sizeof(px)) == 0);
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

// ---------------------------------------------------------------------------
// Templates and the clock: the texts are kept as set, and `{clock}` is
// expanded when drawing

Fact("StatusBar Keeps The Template And Expands The Clock")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    status_bar_content_t c;

    Assert(!bar.HasClock());
    Assert(bar.SetRight("{clock}"));
    Assert(bar.HasClock());
    Assert(!bar.SetRight("{clock}"));               // the template is what's compared

    // Not known yet: blank
    Assert(bar.BeginDraw(nullptr, &c) == buf0);
    Assert(strcmp(c.right, "") == 0);
    bar.EndDraw();

    Assert(bar.SetClock("2:05"));
    Assert(bar.Pending());
    Assert(bar.BeginDraw(buf0, &c) == buf1);
    Assert(strcmp(c.right, "2:05") == 0);
    bar.EndDraw();

    Assert(!bar.SetClock("2:05"));                  // unchanged
    Assert(!bar.Pending());
}

Fact("StatusBar Clock Change Without A Template Is No Redraw")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    status_bar_content_t c;

    Assert(!bar.SetClock("2:05"));
    Assert(!bar.Pending());

    // The clock is kept: a template set later shows it
    Assert(bar.SetLeft("At {clock}"));
    Assert(bar.HasClock());
    Assert(bar.BeginDraw(nullptr, &c) == buf0);
    Assert(strcmp(c.left, "At 2:05") == 0);
    Assert(strcmp(c.right, "") == 0);
    bar.EndDraw();

    // A template change re-expands
    Assert(bar.SetLeft("{clock} x"));
    Assert(bar.BeginDraw(buf0, &c) == buf1);
    Assert(strcmp(c.left, "2:05 x") == 0);
    bar.EndDraw();

    // Without a template the clock no longer matters
    Assert(bar.SetLeft("Kitchen"));
    Assert(!bar.HasClock());
    Assert(!bar.SetClock("2:06"));
}

Fact("StatusBar Clock In Both Texts")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    status_bar_content_t c;

    bar.SetClock("12:30");
    bar.SetLeft("{clock}");
    bar.SetRight("[{clock}]");
    Assert(bar.BeginDraw(nullptr, &c) == buf0);
    Assert(strcmp(c.left, "12:30") == 0);
    Assert(strcmp(c.right, "[12:30]") == 0);
    bar.EndDraw();

    Assert(bar.SetClock(""));                       // forgotten: blank again
    Assert(bar.BeginDraw(buf0, &c) == buf1);
    Assert(strcmp(c.left, "") == 0);
    Assert(strcmp(c.right, "[]") == 0);
}

Fact("StatusBar Expanded Clock Is Cut To Fit")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    status_bar_content_t c;

    char tmpl[STATUS_BAR_TEXT_MAX];
    memset(tmpl, 'a', 40);
    strcpy(tmpl + 40, "{clock}");                   // 47 bytes: fits as a template
    Assert(bar.SetLeft(tmpl));
    bar.SetClock("12:30");
    bar.BeginDraw(nullptr, &c);
    Assert(strlen(c.left) == 45);
    Assert(strcmp(c.left + 40, "12:30") == 0);
    bar.EndDraw();

    // The template is cut as it's set: a `{clock}` across the cut is lost
    char longer[64];
    memset(longer, 'b', 44);
    strcpy(longer + 44, "{clock}");                 // 51 bytes
    Assert(bar.SetLeft(longer));
    Assert(!bar.HasClock());
    bar.BeginDraw(buf0, &c);
    Assert(strlen(c.left) == STATUS_BAR_TEXT_MAX - 1);
    Assert(strcmp(c.left + 44, "{cl") == 0);
}

Fact("StatusBar Clock Renders Like Plain Text")
{
    static BLOBPACK *ui = LoadAsset("ui.bin");
    static uint16_t a[W * H], b[W * H];
    status_bar_style_t style = status_bar_style(ui);
    status_bar_content_t c;

    StatusBar bar;
    bar.Init(a, b);
    bar.SetClock("2:05");
    bar.SetRight("{clock}");
    bar.SetPillCount(3);
    Assert(bar.BeginDraw(nullptr, &c) == a);
    status_bar_render(a, &style, &c);
    bar.EndDraw();

    status_bar_content_t plain = {};
    strcpy(plain.right, "2:05");
    plain.pill_count = 3;
    status_bar_render(px, &style, &plain);
    Assert(memcmp(px, a, sizeof(px)) == 0);

    RenderBanded(STATUS_BAR_BAND_ROWS_MAX, &c);
    Assert(memcmp(px, banded, sizeof(px)) == 0);
}

// ---------------------------------------------------------------------------
// Status icons: just left of the right text (the clock), right to left:
// Wi-Fi, then busy or error. They move with the text's width.

namespace
{
    // Render `content` whole into px
    void RenderContent(const status_bar_content_t *content)
    {
        static BLOBPACK *ui = LoadAsset("ui.bin");
        status_bar_style_t style = status_bar_style(ui);
        status_bar_render(px, &style, content);
    }

    const int RIGHT_END = W - STATUS_BAR_MARGIN;            // the right text ends here
    // The x where icon slot n (0 = nearest the text) starts, for text starting at text_x0
    int SlotX0(int text_x0, int slot) { return text_x0 - STATUS_ICON_TEXT_GAP - (slot + 1) * STATUS_ICON_SLOT_W; }

    // Draws `right` alone and returns where its ink starts
    int TextX0(const char *right)
    {
        status_bar_content_t content = {};
        strcpy(content.right, right);
        RenderContent(&content);
        return Drawn().x0;
    }
}

Fact("StatusBar Icons Sit Left Of The Clock: Error Or Busy, Then Wifi")
{
    struct { int bits, slot; } cases[] = {
        { STATUS_ICON_WIFI_CONNECTED, 0 }, { STATUS_ICON_WIFI_OFF, 0 },
        { STATUS_ICON_WIFI_OFF | STATUS_ICON_BUSY, 1 }, { STATUS_ICON_WIFI_OFF | STATUS_ICON_ERROR, 1 },
    };
    // The text's ink can start a pixel or two right of its pen position
    int text_x = TextX0("12:35");
    for (auto c : cases) {
        status_bar_content_t content = {};
        strcpy(content.right, "12:35");
        content.icons = c.bits;
        RenderContent(&content);
        int x0 = SlotX0(text_x, c.slot) - 2, x1 = SlotX0(text_x, c.slot) + STATUS_ICON_SLOT_W;
        Box b = Drawn(x0, x1 - 1);
        Assert(b.any);
        // Nothing else between the slots and the text, or left of the outermost slot
        int outer = c.bits & (STATUS_ICON_BUSY | STATUS_ICON_ERROR) ? 1 : 0;
        Assert(!Drawn(W / 2, SlotX0(text_x, outer) - 3).any);
    }
}

Fact("StatusBar Icons Follow The Clock's Width")
{
    status_bar_content_t content = {};
    content.icons = STATUS_ICON_WIFI_CONNECTED;
    strcpy(content.right, "1:05");
    RenderContent(&content);
    int narrow = Drawn().x0;
    strcpy(content.right, "12:35");
    RenderContent(&content);
    int wide = Drawn().x0;
    Assert(wide < narrow);
}

Fact("StatusBar Icons Without Right Text End At The Margin")
{
    status_bar_content_t content = {};
    content.icons = STATUS_ICON_WIFI_CONNECTED;
    RenderContent(&content);
    Box b = Drawn();
    Assert(b.any && b.x1 < RIGHT_END && b.x1 >= RIGHT_END - STATUS_ICON_SLOT_W);
}

// Connected is the Wi-Fi waves (wide), not connected a small dot
Fact("StatusBar Wifi Connected Is The Waves, Off The Dot")
{
    status_bar_content_t content = {};
    content.icons = STATUS_ICON_WIFI_CONNECTED;
    RenderContent(&content);
    Box waves = Drawn();
    content.icons = STATUS_ICON_WIFI_OFF;
    RenderContent(&content);
    Box dot = Drawn();
    Assert(waves.any && dot.any);
    Assert(waves.x1 - waves.x0 > 2 * (dot.x1 - dot.x0));
}

Fact("StatusBar Right Text Stays At The Margin With Icons")
{
    status_bar_content_t content = {};
    strcpy(content.right, "12:35");
    RenderContent(&content);
    static uint16_t plain[W * H];
    memcpy(plain, px, sizeof(px));
    int text_x = Drawn().x0;

    content.icons = STATUS_ICON_WIFI_OFF | STATUS_ICON_ERROR;
    RenderContent(&content);
    for (int y = 0; y < H; y++)
        Assert(memcmp(plain + y * W + text_x, px + y * W + text_x, (W - text_x) * sizeof(uint16_t)) == 0);
}

Fact("StatusBar Bands Match With Icons")
{
    status_bar_content_t content = {};
    strcpy(content.left, "AppleTV");
    strcpy(content.right, "12:35");
    content.pill_count = 3;
    content.icons = STATUS_ICON_WIFI_OFF | STATUS_ICON_BUSY | STATUS_ICON_ERROR;
    RenderContent(&content);
    WritePreview("build/status_bar_icons.bmp");
    int sizes[] = { 1, 2, 3, STATUS_BAR_BAND_ROWS_MAX };
    for (int rows : sizes) {
        RenderBanded(rows, &content);
        Assert(memcmp(px, banded, sizeof(px)) == 0);
    }
}

Fact("StatusBar SetIcons Reports Only Real Changes")
{
    StatusBar bar;
    bar.Init(buf0, buf1);
    status_bar_content_t c;
    Assert(bar.SetIcons(STATUS_ICON_WIFI_OFF));
    Assert(!bar.SetIcons(STATUS_ICON_WIFI_OFF));
    Assert(bar.BeginDraw(nullptr, &c) == buf0);
    Assert(c.icons == STATUS_ICON_WIFI_OFF);
}
