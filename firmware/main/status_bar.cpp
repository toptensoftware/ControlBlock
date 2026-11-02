#include <string.h>
#include "status_bar.h"
#include "ui_assets.h"

status_bar_style_t status_bar_style(const BLOBPACK *ui)
{
    status_bar_style_t s = {};
    s.font          = (const FONT *)get_blob(ui, UI_ASSET_STATUS_FONT);
    s.pill_selected = get_blob(ui, UI_ASSET_PAGER_CURRENT);
    s.pill_other    = get_blob(ui, UI_ASSET_PAGER_OTHER);
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

static void draw_pager(IMAGE_BUFFER *buf, const status_bar_style_t *style, int selected, int count)
{
    if (count <= 0)
        return;

    int total = (count - 1) * STATUS_BAR_PILL_GAP;
    for (int i = 0; i < count; i++)
        total += get_image_width(i == selected ? style->pill_selected : style->pill_other);

    int x = (SCREEN_W - total) / 2;
    for (int i = 0; i < count; i++) {
        const void *img = i == selected ? style->pill_selected : style->pill_other;
        int w = get_image_width(img), h = get_image_height(img);
        draw_image(buf, img, x, (SCREEN_STATUS_H - h) / 2, 0, 0, w, h, NULL, 255, NULL);
        x += w + STATUS_BAR_PILL_GAP;
    }
}

void status_bar_render(uint16_t *px, const status_bar_style_t *style, const status_bar_content_t *content)
{
    IMAGE_BUFFER buf;
    init_image_buffer(&buf, SCREEN_W, SCREEN_STATUS_H, px, SCREEN_W);
    fill_rect(&buf, 0, 0, SCREEN_W, SCREEN_STATUS_H, STATUS_BAR_BACKGROUND);

    int baseline = text_baseline(style->font);
    if (content->left[0])
        draw_text(&buf, style->font, STATUS_BAR_MARGIN, baseline, content->left, -1, STATUS_BAR_TEXT, 255);
    if (content->right[0]) {
        int x = SCREEN_W - STATUS_BAR_MARGIN - font_measure_text(style->font, content->right, -1);
        draw_text(&buf, style->font, x, baseline, content->right, -1, STATUS_BAR_TEXT, 255);
    }

    draw_pager(&buf, style, content->selected_pill, content->pill_count);
}

// ---------------------------------------------------------------------------
// StatusBar

void StatusBar::Init(uint16_t *buffer0, uint16_t *buffer1)
{
    m_content    = {};
    m_pending    = false;
    m_buffers[0] = buffer0;
    m_buffers[1] = buffer1;
    m_front      = nullptr;
    m_drawing    = nullptr;
}

bool StatusBar::SetText(char *dst, const char *text)
{
    // Cut to fit, without splitting a UTF-8 sequence
    char t[STATUS_BAR_TEXT_MAX];
    size_t len = text ? strlen(text) : 0;
    if (len >= STATUS_BAR_TEXT_MAX) {
        len = STATUS_BAR_TEXT_MAX - 1;
        while (len > 0 && ((uint8_t)text[len] & 0xC0) == 0x80)
            len--;
    }
    if (len)
        memcpy(t, text, len);
    t[len] = 0;

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

uint16_t *StatusBar::BeginDraw(const uint16_t *shown, status_bar_content_t *out)
{
    // Until the fill latches the front buffer it may still read the other
    if (!m_pending || shown != m_front)
        return nullptr;

    m_lock.Enter();
    *out = m_content;
    m_pending = false;
    m_lock.Exit();

    m_drawing = m_front == m_buffers[0] ? m_buffers[1] : m_buffers[0];
    return m_drawing;
}

void StatusBar::EndDraw()
{
    if (m_drawing)
        m_front = m_drawing;
    m_drawing = nullptr;
}
