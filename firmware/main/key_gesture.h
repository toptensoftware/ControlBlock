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
// One state machine per pressed control, so touch keys, the macro pad and
// anything else share it. Platform independent; the caller serialises calls
// and calls Tick() often enough for `long` to be on time (every frame is fine).

#include <stdint.h>

#define GESTURE_SLOTS               8       // controls held at once
#define GESTURE_NAME_MAX            24      // longest control name kept, with the NUL
#define GESTURE_LONG_PRESS_DEFAULT_MS 500

struct GestureHost
{
    // Does a binding exist for this control and event? (TriggerDispatcher::HasBinding)
    bool (*has_binding)(const char *control, const char *event, void *ctx);
    // Fire a trigger (TriggerDispatcher::Trigger; the param is empty)
    void (*trigger)(const char *control, const char *event, void *ctx);
    void *ctx;
};

class GestureBank
{
public:
    void Init(const GestureHost &host, uint32_t long_press_ms);

    // The default long-press time (the config's longPressMs); 0 = the firmware default
    void SetLongPressMs(uint32_t ms);

    // `long_ms`: this control's own long-press time, 0 = the default. Ignored
    // if the control is already held or all slots are in use.
    void Press(const char *control, uint32_t now_ms, uint32_t long_ms);
    void Release(const char *control, uint32_t now_ms);
    void Cancel(const char *control, uint32_t now_ms);     // release without a tap
    void Tick(uint32_t now_ms);

private:
    struct Slot
    {
        bool     used;
        bool     defer_tap;         // the control has a `long` binding
        bool     long_fired;
        char     control[GESTURE_NAME_MAX];
        uint32_t down_ms;
        uint32_t long_ms;
    };

    Slot *Find(const char *control);
    bool  LongDue(const Slot &s, uint32_t now_ms) const;
    void  FireLong(Slot &s);
    void  End(const char *control, uint32_t now_ms, bool tap);

    GestureHost m_host = {};
    uint32_t    m_long_ms = GESTURE_LONG_PRESS_DEFAULT_MS;
    Slot        m_slot[GESTURE_SLOTS] = {};
};

// "touch[col,row]" for a key on the page strip: page 0 is Home, and the column
// runs across all pages (page p, column c is column 5p + c). `buf` holds
// GESTURE_NAME_MAX bytes.
void TouchControlName(int page, int key, char *buf);
