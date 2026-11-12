#pragma once

// The touch router: decides whether the first finger is a swipe or a key
// press, and turns key touches into PRESS / RELEASE / CANCEL. Platform
// independent and tested; swipe.cpp drives it. Spec: docs/interactions.md.
//
// Not thread safe: one task feeds it (the swipe task).

#include <stdint.h>
#include "key_geometry.h"
#include "screen_layout.h"

// Tuning (docs/interactions.md, Constants)
#define TOUCH_SWIPE_WINDOW_MS    150   // after touch-down, a swipe can start; then the key owns the touch
#define TOUCH_DRAG_SLOP_PX       10     // travel (x) that starts a swipe inside the window
#define TOUCH_EDGE_AMBIGUOUS_PX  12     // touch-down this near a key's edge only swipes
#define TOUCH_KEY_EXIT_SLOP_PX   0      // margin around the key before a held press cancels

// Key actions: the values are the protocol's KEY actions
#define TOUCH_KEY_PRESS    0
#define TOUCH_KEY_RELEASE  1
#define TOUCH_KEY_CANCEL   2

// A key as the hit test sees it: its id and its rect in page-area pixels
struct TouchKey
{
    uint16_t     id;
    KeyPixelRect rect;
};

// The rect (page-area pixels), shrunk by `margin` on every side (grown if
// negative), contains the screen point (x, y), on a page at rest
bool touch_in_rect(const KeyPixelRect &rect, int x, int y, int margin);

// The index of the key in `keys` whose rect, shrunk by `margin`, contains the
// screen point (x, y), on a page at rest; -1 if none (a margin, a seam's edge
// zones, an area no key covers)
int touch_key_at(const TouchKey *keys, int count, int x, int y, int margin);

// Keys are named by id (unique across the layout); the page is the one the
// touch went down on
struct TouchRouterHost
{
    // The page's keys (up to COMP_MAX_KEYS into `out`), for the hit test at
    // touch-down; their count (0 for none or no such page)
    int  (*page_keys)(int page, TouchKey *out, void *ctx);
    // Is the key on the page and reporting presses (not disabled)?
    bool (*key_active)(int page, uint16_t key, void *ctx);
    // A key action, with the device time in milliseconds
    void (*key_event)(int page, uint16_t key, int action, uint32_t time_ms, void *ctx);
    // Show (or clear) the key's pressed look. On from touch-down on an active
    // key, so a tap gives feedback at once; off again if the touch turns into
    // a swipe, or when the press ends. Always paired, one key at a time.
    void (*key_look)(int page, uint16_t key, bool pressed, void *ctx);
    void *ctx;
};

class TouchRouter
{
public:
    void Init(const TouchRouterHost &host);

    // The first finger lands. `page` is the page under it; `swipe_busy` is
    // set if a swipe or glide is under way (the swipe owns the touch).
    void Down(int x, int y, int64_t now_us, int page, bool swipe_busy);

    // The finger moves. Returns true once, when the touch becomes a swipe
    // that the caller should start from here.
    bool Move(int x, int y, int64_t now_us);

    // The finger lifts
    void Up(int x, int y, int64_t now_us);

    // Call every frame with the current page: ends the swipe window, and
    // cancels a held key that has gone or whose page has changed.
    void Tick(int64_t now_us, int page);

    // End the touch in progress without lifting: a held key gets a CANCEL, a
    // pending one just loses its pressed look. The rest of the touch is
    // ignored by the caller (TouchGate).
    void Abort(int64_t now_us);

private:
    enum class State
    {
        Idle,           // no touch
        Pending,        // on a key, inside the swipe window
        Held,           // PRESS sent; the key owns the touch
        SwipeOnly,      // can only swipe (margin, edge zone, no key, inactive key, left the key early)
        Swiping,        // the swipe owns the touch (started, or busy at touch-down)
        Dead,           // a held press was cancelled; ignored until lift
    };

    void Emit(int action, int64_t now_us);
    void UpdateLook();      // the look is on while Pending or Held

    bool            m_look = false;

    TouchRouterHost m_host = {};
    State           m_state = State::Idle;
    int             m_page = 0;
    uint16_t        m_key = KEY_ID_NONE;
    KeyPixelRect    m_rect = {};        // the key's rect at touch-down (for leaving it)
    TouchKey        m_keys[COMP_MAX_KEYS];  // the hit test's scratch
    int             m_down_x = 0;
    int64_t         m_down_us = 0;
};
