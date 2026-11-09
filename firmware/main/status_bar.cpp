#include <string.h>
#include "status_bar.h"
#include "ui_assets.h"

status_bar_style_t status_bar_style(const BLOBPACK *ui)
{
    status_bar_style_t s = {};
    s.font          = (const FONT *)get_blob(ui, UI_ASSET_STATUS_FONT);
    s.pill_selected = get_blob(ui, UI_ASSET_PAGER_CURRENT);
    s.pill_other    = get_blob(ui, UI_ASSET_PAGER_OTHER);
    for (int i = 0; i < STATUS_ICON_IMAGES; i++)
        s.icon[i] = get_blob(ui, UI_ASSET_STATUS_ICON + i);
    return s;
}

// ---------------------------------------------------------------------------
// Drawing

// Baseline that centres capital letters vertically in the bar
static int text_baseline(const FONT *font)
{
    const FONT_GLYPH *cap = font_find_glyph(font, 'H');
    int cap_h = cap ? -cap->top : font->ascent;
    return (SCREEN_STATUS_H + cap_h) / 2;
}

// All drawing is relative to `y0`, the bar row at the top of `buf`
static void draw_pager(IMAGE_BUFFER *buf, int y0, const status_bar_style_t *style, int selected, int count)
{
    if (count <= 1)
        return;                         // one page: nothing to page between

    int total = (count - 1) * STATUS_BAR_PILL_GAP;
    for (int i = 0; i < count; i++)
        total += get_image_width(i == selected ? style->pill_selected : style->pill_other);

    int x = (SCREEN_W - total) / 2;
    for (int i = 0; i < count; i++) {
        const void *img = i == selected ? style->pill_selected : style->pill_other;
        int w = get_image_width(img), h = get_image_height(img);
        draw_image(buf, img, x, (SCREEN_STATUS_H - h) / 2 - y0, 0, 0, w, h, NULL, 255, NULL);
        x += w + STATUS_BAR_PILL_GAP;
    }
}

// Icon image index (STATUS_ICON_* bit order) of the first bit in `mask` that's set in `icons`, or -1
static int first_icon(int icons, int mask)
{
    for (int i = 0; i < STATUS_ICON_IMAGES; i++)
        if (icons & mask & (1 << i))
            return i;
    return -1;
}

// Right to left from `right` (the right text's start, less the gap): Wi-Fi, then
// error or busy (error wins), each centred in a STATUS_ICON_SLOT_W slot
static void draw_icons(IMAGE_BUFFER *buf, int y0, const status_bar_style_t *style, int icons, int right)
{
    int status = first_icon(icons, STATUS_ICON_ERROR);
    if (status < 0)
        status = first_icon(icons, STATUS_ICON_BUSY);
    int order[] = { first_icon(icons, STATUS_ICON_WIFI_CONNECTED | STATUS_ICON_WIFI_OFF), status };
    int x = right;
    for (int i : order) {
        if (i < 0)
            continue;
        x -= STATUS_ICON_SLOT_W;
        const void *img = style->icon[i];
        int w = get_image_width(img), h = get_image_height(img);
        draw_image(buf, img, x + (STATUS_ICON_SLOT_W - w) / 2, (SCREEN_STATUS_H - h) / 2 - y0, 0, 0, w, h, NULL, 255, NULL);
    }
}

void status_bar_render_rows(uint16_t *px, int y0, int rows, const status_bar_style_t *style, const status_bar_content_t *content)
{
    // Drawing is clipped to the band, so shifting everything up by y0 draws
    // just those rows
    IMAGE_BUFFER buf;
    init_image_buffer(&buf, SCREEN_W, rows, px, SCREEN_W);
    fill_rect(&buf, 0, 0, SCREEN_W, rows, STATUS_BAR_BACKGROUND);

    int baseline = text_baseline(style->font) - y0;
    if (content->left[0])
        draw_text(&buf, style->font, STATUS_BAR_MARGIN, baseline, content->left, -1, STATUS_BAR_TEXT, 255);
    // The right text (the clock) at the margin, the icons just left of it
    int icons_right = SCREEN_W - STATUS_BAR_MARGIN;
    if (content->right[0]) {
        int x = icons_right - font_measure_text(style->font, content->right, -1);
        draw_text(&buf, style->font, x, baseline, content->right, -1, STATUS_BAR_TEXT, 255);
        icons_right = x - STATUS_ICON_TEXT_GAP;
    }
    draw_icons(&buf, y0, style, content->icons, icons_right);

    draw_pager(&buf, y0, style, content->selected_pill, content->pill_count);
}

void status_bar_render(uint16_t *px, const status_bar_style_t *style, const status_bar_content_t *content)
{
    status_bar_render_rows(px, 0, SCREEN_STATUS_H, style, content);
}

// ---------------------------------------------------------------------------
// StatusBar

void StatusBar::Init(uint16_t *buffer0, uint16_t *buffer1)
{
    m_content    = {};
    m_clock[0]   = 0;
    m_pending    = false;
    m_buffers[0] = buffer0;
    m_buffers[1] = buffer1;
    m_front      = nullptr;
    m_drawing    = nullptr;
}

// Copy `text` (NULL = "") into `dst` (size bytes), cut to fit without
// splitting a UTF-8 sequence; returns its length
static size_t copy_cut(char *dst, size_t size, const char *text)
{
    size_t len = utf8_cut_len(text, size - 1);
    if (len)
        memcpy(dst, text, len);
    dst[len] = 0;
    return len;
}

bool StatusBar::SetText(char *dst, const char *text)
{
    char t[STATUS_BAR_TEXT_MAX];
    size_t len = copy_cut(t, sizeof(t), text);

    m_lock.Enter();
    bool changed = strcmp(dst, t) != 0;
    if (changed) {
        memcpy(dst, t, len + 1);
        m_pending = true;
    }
    m_lock.Exit();
    return changed;
}

bool StatusBar::SetInt(int *dst, int value)
{
    m_lock.Enter();
    bool changed = *dst != value;
    if (changed) {
        *dst = value;
        m_pending = true;
    }
    m_lock.Exit();
    return changed;
}

bool StatusBar::SetLeft(const char *text)   { return SetText(m_content.left, text); }
bool StatusBar::SetRight(const char *text)  { return SetText(m_content.right, text); }
bool StatusBar::SetPillCount(int count)     { return SetInt(&m_content.pill_count, count); }
bool StatusBar::SetSelectedPill(int index)  { return SetInt(&m_content.selected_pill, index); }
bool StatusBar::SetIcons(int icons)         { return SetInt(&m_content.icons, icons); }

bool StatusBar::HasClockLocked() const
{
    return clock_text_has_clock(m_content.left) || clock_text_has_clock(m_content.right);
}

bool StatusBar::HasClock()
{
    m_lock.Enter();
    bool has = HasClockLocked();
    m_lock.Exit();
    return has;
}

bool StatusBar::SetClock(const char *text)
{
    char t[CLOCK_TEXT_MAX];
    size_t len = copy_cut(t, sizeof(t), text);

    m_lock.Enter();
    bool redraw = strcmp(m_clock, t) != 0 && HasClockLocked();
    memcpy(m_clock, t, len + 1);
    if (redraw)
        m_pending = true;
    m_lock.Exit();
    return redraw;
}

uint16_t *StatusBar::BeginDraw(const uint16_t *shown, status_bar_content_t *out)
{
    // Until the fill latches the front buffer it may still read the other
    if (!m_pending || shown != m_front)
        return nullptr;

    char clock[CLOCK_TEXT_MAX];
    m_lock.Enter();
    *out = m_content;
    memcpy(clock, m_clock, sizeof(clock));
    m_pending = false;
    m_lock.Exit();

    clock_text_expand(out->left, sizeof(out->left), out->left, clock);
    clock_text_expand(out->right, sizeof(out->right), out->right, clock);

    m_drawing = m_front == m_buffers[0] ? m_buffers[1] : m_buffers[0];
    return m_drawing;
}

void StatusBar::EndDraw()
{
    if (m_drawing)
        m_front = m_drawing;
    m_drawing = nullptr;
}
