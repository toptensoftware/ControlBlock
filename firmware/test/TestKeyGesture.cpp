// Key gestures (docs/standalone.md "Key gestures"): raw press / release /
// cancel per control become the triggers press, release, tap and long.

#include <stdio.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "key_gesture.h"

using namespace SimpleLib;

namespace
{
    // A fake host: bindings are "control event" strings; triggers are recorded
    struct Rig
    {
        GestureBank bank;
        uint32_t    now = 5000;
        char        bound[16][48];
        int         bound_count = 0;
        char        log[64][48];                // "control event" per trigger, in order
        int         logged = 0;

        Rig()
        {
            GestureHost h = {};
            h.ctx = this;
            h.has_binding = [](const char *control, const char *event, void *c) {
                Rig *r = (Rig *)c;
                char key[48];
                snprintf(key, sizeof(key), "%s %s", control, event);
                for (int i = 0; i < r->bound_count; i++)
                    if (!strcmp(r->bound[i], key))
                        return true;
                return false;
            };
            h.trigger = [](const char *control, const char *event, void *c) {
                Rig *r = (Rig *)c;
                if (r->logged < 64)
                    snprintf(r->log[r->logged++], 48, "%s %s", control, event);
            };
            bank.Init(h, 500);
        }

        void Bind(const char *control, const char *event)
        {
            snprintf(bound[bound_count++], 48, "%s %s", control, event);
        }

        // The log as one string: "k tap|k release"
        const char *Log()
        {
            static char out[1024];
            out[0] = 0;
            for (int i = 0; i < logged; i++) {
                if (i) strcat(out, "|");
                strcat(out, log[i]);
            }
            return out;
        }
        bool LogIs(const char *s) { return !strcmp(Log(), s); }
    };
}

Fact("Gesture Plain Key Fires Press And Tap At Once, Release On Lift")
{
    Rig r;
    r.Bind("k", "tap");
    r.bank.Press("k", r.now, 0);
    Assert(r.LogIs("k press|k tap"));               // no latency: tap doesn't wait for the lift
    r.now += 80;
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press|k tap|k release"));
}

Fact("Gesture Plain Key Never Fires Long, However Long Held")
{
    Rig r;
    r.Bind("k", "tap");
    r.bank.Press("k", r.now, 0);
    r.now += 5000;
    r.bank.Tick(r.now);
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press|k tap|k release"));
}

Fact("Gesture Key With Long Defers Tap To Release")
{
    Rig r;
    r.Bind("k", "tap");
    r.Bind("k", "long");
    r.bank.Press("k", r.now, 0);
    Assert(r.LogIs("k press"));
    r.now += 100;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press"));
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press|k release|k tap"));
}

Fact("Gesture Long Fires At The Threshold While Held, Then No Tap")
{
    Rig r;
    r.Bind("k", "tap");
    r.Bind("k", "long");
    r.bank.Press("k", r.now, 0);
    r.now += 499;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press"));
    r.now += 1;                                     // 500 ms: on the threshold, not on release
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press|k long"));
    r.now += 1000;
    r.bank.Tick(r.now);                             // only once
    Assert(r.LogIs("k press|k long"));
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press|k long|k release"));    // and the release is not a tap
}

Fact("Gesture Release Past The Threshold Fires Long Even If No Tick Came")
{
    Rig r;
    r.Bind("k", "tap");
    r.Bind("k", "long");
    r.bank.Press("k", r.now, 0);
    r.now += 700;
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press|k long|k release"));
}

Fact("Gesture Long Only Control Still Taps On A Short Press")
{
    Rig r;
    r.Bind("k", "long");
    r.bank.Press("k", r.now, 0);
    r.now += 100;
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press|k release|k tap"));     // a short press is still a tap; nothing is bound, so it's ignored downstream
}

Fact("Gesture Cancel Releases Without A Tap")
{
    Rig r;
    r.Bind("k", "tap");
    r.Bind("k", "long");
    r.bank.Press("k", r.now, 0);
    r.now += 50;
    r.bank.Cancel("k", r.now);
    Assert(r.LogIs("k press|k release"));           // a held key is never left stuck, but it didn't tap
    r.now += 1000;
    r.bank.Tick(r.now);                             // and long doesn't fire later
    Assert(r.LogIs("k press|k release"));
}

Fact("Gesture Per Key Long Press Time Overrides The Default")
{
    Rig r;
    r.Bind("k", "long");
    r.bank.Press("k", r.now, 200);
    r.now += 199;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press"));
    r.now += 1;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press|k long"));
}

Fact("Gesture Default Long Press Time Is Settable")
{
    Rig r;
    r.Bind("k", "long");
    r.bank.SetLongPressMs(650);
    r.bank.Press("k", r.now, 0);
    r.now += 649;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press"));
    r.now += 1;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press|k long"));
    r.bank.SetLongPressMs(0);                       // 0 = the firmware default
    r.bank.Release("k", r.now);
    r.bank.Press("k", r.now, 0);
    r.now += 500;
    r.bank.Tick(r.now);
    Assert(r.logged == 5);                          // press long release press long
}

Fact("Gesture Controls Are Independent")
{
    Rig r;
    r.Bind("a", "long");
    r.Bind("b", "tap");
    r.bank.Press("a", r.now, 0);
    r.bank.Press("b", r.now, 0);
    r.now += 600;
    r.bank.Tick(r.now);
    r.bank.Release("b", r.now);
    r.bank.Release("a", r.now);
    Assert(r.LogIs("a press|b press|b tap|a long|b release|a release"));
}

Fact("Gesture Strays Are Ignored")
{
    Rig r;
    r.bank.Release("k", r.now);                     // never pressed
    r.bank.Cancel("k", r.now);
    Assert(r.logged == 0);
    r.bank.Press("k", r.now, 0);
    r.bank.Press("k", r.now, 0);                    // a second press of a held control
    Assert(r.LogIs("k press|k tap"));
    r.bank.Release("k", r.now);
    r.bank.Release("k", r.now);                     // a second release
    Assert(r.LogIs("k press|k tap|k release"));
}

Fact("Gesture Control Can Be Pressed Again After Release")
{
    Rig r;
    r.Bind("k", "tap");
    for (int i = 0; i < 3; i++) {
        r.bank.Press("k", r.now, 0);
        r.now += 10;
        r.bank.Release("k", r.now);
    }
    Assert(r.logged == 9);
}

Fact("Gesture Bank Full Ignores Further Controls")
{
    Rig r;
    char name[GESTURE_SLOTS + 2][8];
    for (int i = 0; i < GESTURE_SLOTS + 1; i++) {
        snprintf(name[i], 8, "c%d", i);
        r.bank.Press(name[i], r.now, 0);
    }
    Assert(r.logged == 2 * GESTURE_SLOTS);          // press + tap each; the extra press fired nothing
    r.bank.Release(name[GESTURE_SLOTS], r.now);     // and its release is a stray
    Assert(r.logged == 2 * GESTURE_SLOTS);
    r.bank.Release(name[0], r.now);                 // frees a slot
    r.bank.Press(name[GESTURE_SLOTS], r.now, 0);
    Assert(r.logged == 2 * GESTURE_SLOTS + 3);
}

Fact("Gesture Long Name Is Safe")
{
    Rig r;
    const char *longname = "touch[1234567890,1234567890,1234567890,1234567890]";
    r.bank.Press(longname, r.now, 0);
    r.bank.Release(longname, r.now);
    Assert(r.logged == 3);                          // press, tap, release: matched on the truncated name
}

Fact("Gesture Time Wraps")
{
    Rig r;
    r.Bind("k", "long");
    r.now = 0xFFFFFF00u;
    r.bank.Press("k", r.now, 0);
    r.now += 499;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press"));
    r.now += 1;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press|k long"));
}

Fact("Gesture Touch Control Names Are The Key Ids")
{
    // Ids are unique across the layout, so the name doesn't depend on the page
    char buf[GESTURE_NAME_MAX];
    TouchControlName(1, buf);
    Assert(!strcmp(buf, "touch[1]"));
    TouchControlName(16, buf);                      // the bake's id for the first page after Home's slot 0
    Assert(!strcmp(buf, "touch[16]"));
    TouchControlName(200, buf);
    Assert(!strcmp(buf, "touch[200]"));
    TouchControlName(65535, buf);                   // the longest fits
    Assert(!strcmp(buf, "touch[65535]"));
    static_assert(sizeof("touch[65535]") <= GESTURE_NAME_MAX, "every touch name fits a gesture slot");
}
