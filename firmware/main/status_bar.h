#pragma once
#include <stdint.h>
#include "rle565.h"
#include "blobpack.h"
#include "color.h"
#include "port.h"
#include "screen_layout.h"

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
    int  pill_count;                    // pager pills; 0 for none
    int  selected_pill;                 // highlighted pill (none if out of range)
} status_bar_content_t;

// The style from ui.bin (layout in ui_assets.h)
status_bar_style_t status_bar_style(const BLOBPACK *ui);

// Draw the bar into `px` (STATUS_BAR_PIXELS): background, text vertically
// centred on its cap height, pager pills centred.
void status_bar_render(uint16_t *px, const status_bar_style_t *style, const status_bar_content_t *content);

// The bar's properties and its two buffers. Setters may be called from any
// task; a drawing task calls BeginDraw()/EndDraw() and shows Front().
class StatusBar
{
public:
    // Two buffers of STATUS_BAR_PIXELS. Nothing is drawn until a property is set.
    void Init(uint16_t *buffer0, uint16_t *buffer1);

    // Each returns true if the value changed (the bar needs drawing)
    bool SetLeft(const char *text);
    bool SetRight(const char *text);
    bool SetPillCount(int count);
    bool SetSelectedPill(int index);

    // A change hasn't been drawn yet
    bool Pending() const { return m_pending; }

    // If there's a change to draw and the back buffer is free, copy the
    // properties to `out` and return the back buffer to draw them into;
    // otherwise NULL. The back buffer is free once the bounce fill has
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

    port::SpinLock       m_lock;
    status_bar_content_t m_content = {};
    volatile bool        m_pending = false;
    uint16_t            *m_buffers[2] = {};
    uint16_t            *m_front = nullptr;
    uint16_t            *m_drawing = nullptr;
};
