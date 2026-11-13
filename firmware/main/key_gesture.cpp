#include <stdio.h>
#include <string.h>
#include "key_gesture.h"

static const char EV_PRESS[]   = "press";
static const char EV_RELEASE[] = "release";
static const char EV_TAP[]     = "tap";
static const char EV_LONG[]    = "long";
static const char NO_PARAM[]   = "";

void TouchControlName(uint16_t id, char *buf)
{
    snprintf(buf, GESTURE_NAME_MAX, "touch[%u]", (unsigned)id);
}

void GestureBank::Init(const GestureHost &host, uint32_t long_press_ms)
{
    m_host = host;
    Reset();
    SetLongPressMs(long_press_ms);
}

void GestureBank::Reset()
{
    memset(m_slot, 0, sizeof(m_slot));
}

void GestureBank::SetLongPressMs(uint32_t ms)
{
    m_long_ms = ms ? ms : GESTURE_LONG_PRESS_DEFAULT_MS;
}

// Names longer than a slot holds are cut the same way every time, so a
// control still matches itself
GestureBank::Slot *GestureBank::Find(const char *control)
{
    for (Slot &s : m_slot)
        if (s.used && strncmp(s.control, control, GESTURE_NAME_MAX - 1) == 0)
            return &s;
    return nullptr;
}

// Signed difference: right across the clock wrapping
bool GestureBank::LongDue(const Slot &s, uint32_t now_ms) const
{
    return s.defer_tap && !s.long_fired && (int32_t)(now_ms - s.down_ms - s.long_ms) >= 0;
}

void GestureBank::FireLong(Slot &s)
{
    s.long_fired = true;
    m_host.trigger(s.control, EV_LONG, NO_PARAM, m_host.ctx);
}

// The press (and its tap unless deferred), or a repeat of them: the one place
// the repeat param is made
void GestureBank::FirePressTap(Slot &s)
{
    char param[GESTURE_PARAM_MAX] = "";
    if (s.repeat_ms)
        snprintf(param, sizeof(param), "repeat=%u", (unsigned)s.repeat_n);
    m_host.trigger(s.control, EV_PRESS, param, m_host.ctx);
    if (!s.defer_tap)
        m_host.trigger(s.control, EV_TAP, param, m_host.ctx);
}

void GestureBank::Press(const char *control, uint32_t now_ms, const GestureTiming &timing)
{
    if (Find(control))
        return;
    Slot *free_slot = nullptr;
    for (Slot &s : m_slot)
        if (!s.used) { free_slot = &s; break; }
    if (!free_slot)
        return;

    Slot &s = *free_slot;
    s = {};
    s.used = true;
    strncpy(s.control, control, GESTURE_NAME_MAX - 1);
    s.down_ms   = now_ms;
    s.long_ms   = timing.long_ms ? timing.long_ms : m_long_ms;
    s.repeat_ms = timing.repeat_ms;
    if (s.repeat_ms) {
        // Repeating: never `long`, so the tap is never deferred
        s.repeat_delay_ms = timing.repeat_delay_ms ? timing.repeat_delay_ms : timing.repeat_ms;
        s.not_before_ms   = timing.not_before_ms;
    } else {
        s.defer_tap = m_host.has_binding(s.control, EV_LONG, m_host.ctx);
    }
    FirePressTap(s);
}

// Unsigned elapsed time: right across the clock wrapping
void GestureBank::TickRepeat(Slot &s, uint32_t now_ms)
{
    uint32_t held = now_ms - s.down_ms;
    if (held < s.repeat_delay_ms || held < s.not_before_ms)
        return;
    uint32_t n = 1 + (held - s.repeat_delay_ms) / s.repeat_ms;
    if (n <= s.repeat_n)
        return;
    if (m_host.busy && m_host.busy(m_host.ctx))
        return;         // skipped; the next one fired still counts the time
    s.repeat_n = n;
    FirePressTap(s);
}

void GestureBank::End(const char *control, uint32_t now_ms, bool tap)
{
    Slot *s = Find(control);
    if (!s)
        return;
    // A tick may not have come since the threshold passed
    if (tap && LongDue(*s, now_ms))
        FireLong(*s);
    m_host.trigger(s->control, EV_RELEASE, NO_PARAM, m_host.ctx);
    if (tap && s->defer_tap && !s->long_fired)
        m_host.trigger(s->control, EV_TAP, NO_PARAM, m_host.ctx);
    s->used = false;
}

void GestureBank::Release(const char *control, uint32_t now_ms) { End(control, now_ms, true); }
void GestureBank::Cancel(const char *control, uint32_t now_ms)  { End(control, now_ms, false); }

void GestureBank::Tick(uint32_t now_ms)
{
    for (Slot &s : m_slot) {
        if (!s.used)
            continue;
        if (s.repeat_ms)
            TickRepeat(s, now_ms);
        else if (LongDue(s, now_ms))
            FireLong(s);
    }
}
