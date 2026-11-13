#pragma once

// Key gestures (docs/standalone.md "Key gestures"): turns raw press / release /
// cancel on a control into the triggers
//
//     press    on press, at once
//     release  on release, and on cancel (so a held key is never left stuck)
//     tap      on release if shorter than the long-press time and `long` didn't
//              fire (not on cancel); at press instead if the control has no
//              `long` binding, so plain keys add no latency
//     long     at the long-press time, while still held
//
// Auto repeat: a control pressed with a repeat period (GestureTiming::repeat_ms)
// fires `press` and `tap` at the press and again at down + delay, then every
// period while held, each with the param `repeat=<n>` (n = 0 at the press, then
// the periods elapsed since down + delay, plus 1: time based, so a skipped repeat
// doesn't slow a script's acceleration). A repeat is skipped while the host is
// busy (its op queue has lists waiting), and at most one fires per Tick. The
// release (or cancel) fires one `release`, no param. A repeating control never
// fires `long` and never defers `tap`. Other controls' triggers have an empty param.
//
// One state machine per pressed control, so touch keys, the macro pad and
// anything else share it. Platform independent; the caller serialises calls
// and calls Tick() often enough for `long` and repeats to be on time (every frame is fine).

#include <stdint.h>

#define GESTURE_SLOTS               8       // controls held at once
#define GESTURE_NAME_MAX            24      // longest control name kept, with the NUL
#define GESTURE_LONG_PRESS_DEFAULT_MS 500
#define GESTURE_PARAM_MAX           24      // "repeat=<n>", with the NUL

// A control's timing, given at the press. All zero = a plain control with the
// default long-press time.
struct GestureTiming
{
    uint32_t long_ms;           // the control's own long-press time, 0 = the default
    uint32_t repeat_delay_ms;   // first repeat after the press, 0 = repeat_ms
    uint32_t repeat_ms;         // repeat period, 0 = no repeat
    uint32_t not_before_ms;     // no repeat before this long after the press (touch: the
                                // swipe window); the schedule and `n` stay as above
};

struct GestureHost
{
    // Does a binding exist for this control and event? (TriggerDispatcher::HasBinding)
    bool (*has_binding)(const char *control, const char *event, void *ctx);
    // Fire a trigger (TriggerDispatcher::Trigger); `param` is "" or "repeat=<n>"
    void (*trigger)(const char *control, const char *event, const char *param, void *ctx);
    // Skip repeats now? (the op queue has lists waiting); null = never busy
    bool (*busy)(void *ctx);
    void *ctx;
};

class GestureBank
{
public:
    void Init(const GestureHost &host, uint32_t long_press_ms);

    // The default long-press time (the layout's longPressMs); 0 = the firmware default
    void SetLongPressMs(uint32_t ms);

    // Forget every held control, firing nothing (a later release of one is ignored)
    void Reset();

    // Ignored if the control is already held or all slots are in use.
    void Press(const char *control, uint32_t now_ms, const GestureTiming &timing);
    void Release(const char *control, uint32_t now_ms);
    void Cancel(const char *control, uint32_t now_ms);     // release without a tap
    void Tick(uint32_t now_ms);

private:
    struct Slot
    {
        bool     used;
        bool     defer_tap;         // the control has a `long` binding (never when repeating)
        bool     long_fired;
        char     control[GESTURE_NAME_MAX];
        uint32_t down_ms;
        uint32_t long_ms;
        uint32_t repeat_delay_ms;   // resolved (never 0 when repeating)
        uint32_t repeat_ms;         // 0 = not repeating
        uint32_t not_before_ms;
        uint32_t repeat_n;          // the last repeat fired (0 = the press)
    };

    Slot *Find(const char *control);
    bool  LongDue(const Slot &s, uint32_t now_ms) const;
    void  FireLong(Slot &s);
    void  FirePressTap(Slot &s);
    void  TickRepeat(Slot &s, uint32_t now_ms);
    void  End(const char *control, uint32_t now_ms, bool tap);

    GestureHost m_host = {};
    uint32_t    m_long_ms = GESTURE_LONG_PRESS_DEFAULT_MS;
    Slot        m_slot[GESTURE_SLOTS] = {};
};

// "touch[id]" for the touch key with `id` (unique across the layout, so the
// name is the same on whichever page it sits). `buf` holds GESTURE_NAME_MAX bytes.
void TouchControlName(uint16_t id, char *buf);
