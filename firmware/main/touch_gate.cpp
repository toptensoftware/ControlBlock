#include "touch_gate.h"

void TouchGate::Init(const TouchRouterHost &host, void (*wake)(uint32_t, void *), void *wake_ctx)
{
    m_router.Init(host);
    m_wake       = wake;
    m_wake_ctx   = wake_ctx;
    m_enabled    = true;
    m_down       = false;
    m_swallowed  = false;
}

void TouchGate::SetEnabled(bool enabled, int64_t now_us)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (!enabled && m_down) {
        m_router.Abort(now_us);
        m_swallowed = true;
    }
}

void TouchGate::Down(int x, int y, int64_t now_us, int page, bool swipe_busy)
{
    m_down = true;
    if (!m_enabled) {
        m_swallowed = true;
        if (m_wake)
            m_wake((uint32_t)(now_us / 1000), m_wake_ctx);
        return;
    }
    m_swallowed = false;
    m_router.Down(x, y, now_us, page, swipe_busy);
}

bool TouchGate::Move(int x, int y, int64_t now_us)
{
    return !m_swallowed && m_router.Move(x, y, now_us);
}

void TouchGate::Up(int x, int y, int64_t now_us)
{
    m_down = false;
    if (m_swallowed) {
        m_swallowed = false;
        return;
    }
    m_router.Up(x, y, now_us);
}

void TouchGate::Tick(int64_t now_us, int page)
{
    m_router.Tick(now_us, page);
}
