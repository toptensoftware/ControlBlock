#include <stdlib.h>
#include "touch_router.h"

bool touch_in_rect(const KeyPixelRect &r, int x, int y, int margin)
{
    int top = SCREEN_PAGE_TOP + r.y;
    return x >= r.x + margin && x < r.x + r.w - margin &&
           y >= top + margin && y < top + r.h - margin;
}

int touch_key_at(const TouchKey *keys, int count, int x, int y, int margin)
{
    for (int k = 0; k < count; k++)
        if (touch_in_rect(keys[k].rect, x, y, margin))
            return k;
    return -1;
}

void TouchRouter::Init(const TouchRouterHost &host)
{
    m_host  = host;
    m_state = State::Idle;
    m_look  = false;
}

void TouchRouter::Emit(int action, int64_t now_us)
{
    m_host.key_event(m_page, m_key, action, (uint32_t)(now_us / 1000), m_host.ctx);
}

// The pressed look follows the state: on while the touch might still be a
// press (Pending) or is one (Held). m_page and m_key still name the key
// whenever it's turned off, so call this before they change.
void TouchRouter::UpdateLook()
{
    bool want = m_state == State::Pending || m_state == State::Held;
    if (want == m_look)
        return;
    m_look = want;
    m_host.key_look(m_page, m_key, want, m_host.ctx);
}

void TouchRouter::Down(int x, int y, int64_t now_us, int page, bool swipe_busy)
{
    // A lost UP: close the press that's still open, and its look
    if (m_state == State::Held)
        Emit(TOUCH_KEY_CANCEL, now_us);
    m_state = State::Idle;
    UpdateLook();

    m_page    = page;
    m_down_x  = x;
    m_down_us = now_us;
    m_key     = KEY_ID_NONE;

    if (swipe_busy) {
        m_state = State::Swiping;
        return;
    }

    int count = m_host.page_keys(page, m_keys, m_host.ctx);
    int k = touch_key_at(m_keys, count, x, y, TOUCH_EDGE_AMBIGUOUS_PX);
    if (k >= 0 && m_host.key_active(page, m_keys[k].id, m_host.ctx)) {
        m_key   = m_keys[k].id;
        m_rect  = m_keys[k].rect;
        m_state = State::Pending;
    } else {
        m_state = State::SwipeOnly;
    }
    UpdateLook();
}

bool TouchRouter::Move(int x, int y, int64_t now_us)
{
    bool swipe = false;

    switch (m_state) {
    case State::Pending:
    case State::SwipeOnly:
        if (abs(x - m_down_x) > TOUCH_DRAG_SLOP_PX) {
            m_state = State::Swiping;
            swipe = true;
        } else if (m_state == State::Pending && !touch_in_rect(m_rect, x, y, -TOUCH_KEY_EXIT_SLOP_PX)) {
            // Out of the key before the window ended: no key, but still free to swipe
            m_state = State::SwipeOnly;
        }
        break;

    case State::Held:
        if (!touch_in_rect(m_rect, x, y, -TOUCH_KEY_EXIT_SLOP_PX)) {
            Emit(TOUCH_KEY_CANCEL, now_us);
            m_state = State::Dead;
        }
        break;

    default:
        break;
    }

    UpdateLook();
    return swipe;
}

void TouchRouter::Up(int, int, int64_t now_us)
{
    if (m_state == State::Held) {
        Emit(TOUCH_KEY_RELEASE, now_us);
    } else if (m_state == State::Pending) {
        // A tap that ended inside the window
        if (m_host.key_active(m_page, m_key, m_host.ctx)) {
            Emit(TOUCH_KEY_PRESS, now_us);
            Emit(TOUCH_KEY_RELEASE, now_us);
        }
    }
    m_state = State::Idle;
    UpdateLook();
}

void TouchRouter::Abort(int64_t now_us)
{
    if (m_state == State::Held)
        Emit(TOUCH_KEY_CANCEL, now_us);
    m_state = State::Idle;
    UpdateLook();
}

void TouchRouter::Tick(int64_t now_us, int page)
{
    if (m_state == State::Pending && now_us - m_down_us >= TOUCH_SWIPE_WINDOW_MS * 1000LL) {
        if (page == m_page && m_host.key_active(m_page, m_key, m_host.ctx)) {
            m_state = State::Held;
            Emit(TOUCH_KEY_PRESS, now_us);
        } else {
            m_state = State::SwipeOnly;
        }
    } else if (m_state == State::Held) {
        if (page != m_page || !m_host.key_active(m_page, m_key, m_host.ctx)) {
            Emit(TOUCH_KEY_CANCEL, now_us);
            m_state = State::Dead;
        }
    }
    UpdateLook();
}
