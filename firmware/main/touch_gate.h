#pragma once

// The wake gate: sits in front of the touch router. While it is disabled (the
// backlight is off) touches never reach the router: each first-finger
// touch-down sends one wake and the rest of that touch is swallowed, also
// after the gate is enabled again. Platform independent and tested; spec:
// docs/backlight.md. Not thread safe: the same task as the router uses it.

#include "touch_router.h"

class TouchGate
{
public:
    // `wake(time_ms, wake_ctx)` is called on each touch-down while disabled.
    // Starts enabled.
    void Init(const TouchRouterHost &host, void (*wake)(uint32_t time_ms, void *ctx), void *wake_ctx);

    // Enabled = backlight on. Setting the current value changes nothing.
    // Disabling mid-touch ends the router's interaction (CANCEL for a held
    // key, pressed look reverted) and swallows the rest of the touch, without
    // a wake. Enabling leaves a swallowed touch swallowed until it lifts.
    void SetEnabled(bool enabled, int64_t now_us);
    bool Enabled() const { return m_enabled; }

    // Same as TouchRouter
    void Down(int x, int y, int64_t now_us, int page, bool swipe_busy);
    bool Move(int x, int y, int64_t now_us);
    void Up(int x, int y, int64_t now_us);
    void Tick(int64_t now_us, int page);

private:
    TouchRouter m_router;
    void      (*m_wake)(uint32_t time_ms, void *ctx) = nullptr;
    void       *m_wake_ctx = nullptr;
    bool        m_enabled = true;
    bool        m_down = false;         // the first finger is down
    bool        m_swallowed = false;    // ...and the router must not see it
};
