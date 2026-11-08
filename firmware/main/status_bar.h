#pragma once
#include <stdint.h>
#include "rle565.h"
#include "blobpack.h"
#include "color.h"
#include "port.h"
#include "screen_layout.h"
#include "clock_text.h"

// Status bar, platform independent (host tested): its properties, drawing,
// and the two buffers the bounce fill shows (screen_set_status_bar()).
// status.cpp runs it on the device.

#define STATUS_BAR_BACKGROUND   RGB565(0, 0, 0)
#define STATUS_BAR_TEXT         RGB565(255, 255, 255)
#define STATUS_BAR_MARGIN       16      // left and right text inset
#define STATUS_BAR_PILL_GAP     8       // between pager pills
#define STATUS_BAR_TEXT_MAX     48      // bytes per text, with the NUL; longer is cut
#define STATUS_BAR_PIXELS       (SCREEN_W * SCREEN_STATUS_H)

typedef struct {
    const FONT *font;
    const void *pill_selected;          // rle565 pager pill for the page in view
    const void *pill_other;             // and for the other pages
} status_bar_style_t;

typedef struct {
    char left[STATUS_BAR_TEXT_MAX];     // left aligned text (UTF-8)
    char right[STATUS_BAR_TEXT_MAX];    // right aligned text
    int  pill_count;                    // pager pills; none drawn for 0 or 1
    int  selected_pill;                 // highlighted pill (none if out of range)
} status_bar_content_t;

// The style from ui.bin (layout in ui_assets.h)
status_bar_style_t status_bar_style(const BLOBPACK *ui);

// Most rows status_bar_render_rows() is asked for at once: the size of the
// device's internal scratch band (SCREEN_W * this * 2 bytes)
#define STATUS_BAR_BAND_ROWS_MAX  4

// Draw rows y0..y0+rows-1 of the bar into `px` (SCREEN_W * rows pixels), the
// same pixels status_bar_render() puts there. The device renders a band into
// internal RAM and copies it to the PSRAM bar in a fill gap.
void status_bar_render_rows(uint16_t *px, int y0, int rows, const status_bar_style_t *style, const status_bar_content_t *content);

// Draw the whole bar into `px` (STATUS_BAR_PIXELS): background, text
// vertically centred on its cap height, pager pills centred.
void status_bar_render(uint16_t *px, const status_bar_style_t *style, const status_bar_content_t *content);

// The bar's properties and its two buffers. Setters may be called from any
// task; a drawing task calls BeginDraw()/EndDraw() and shows Front().
class StatusBar
{
public:
    // Two buffers of STATUS_BAR_PIXELS. Nothing is drawn until a property is set.
    void Init(uint16_t *buffer0, uint16_t *buffer1);

    // Each returns true if the value changed (the bar needs drawing). The
    // texts are templates, kept as set: `{clock}` in them shows the clock.
    bool SetLeft(const char *text);
    bool SetRight(const char *text);
    bool SetPillCount(int count);
    bool SetSelectedPill(int index);

    // The clock's text ("" while the time isn't known). Returns true only if
    // it changed and a text shows it.
    bool SetClock(const char *text);

    // A text holds `{clock}` (the drawing task wakes each minute)
    bool HasClock();

    // A change hasn't been drawn yet
    bool Pending() const { return m_pending; }

    // If there's a change to draw and the back buffer is free, copy the
    // properties to `out` (texts expanded) and return the back buffer to draw
    // them into; otherwise NULL. The back buffer is free once the bounce fill has
    // latched the front one: `shown` is what it latched
    // (screen_status_bar_shown()).
    uint16_t *BeginDraw(const uint16_t *shown, status_bar_content_t *out);

    // The buffer from BeginDraw() is drawn: it becomes the front
    void EndDraw();

    // The buffer to show (NULL until the first draw)
    const uint16_t *Front() const { return m_front; }

private:
    bool SetText(char *dst, const char *text);
    bool SetInt(int *dst, int value);
    bool HasClockLocked() const;

    port::SpinLock       m_lock;
    status_bar_content_t m_content = {};        // texts as templates
    char                 m_clock[CLOCK_TEXT_MAX] = {};
    volatile bool        m_pending = false;
    uint16_t            *m_buffers[2] = {};
    uint16_t            *m_front = nullptr;
    uint16_t            *m_drawing = nullptr;
};
