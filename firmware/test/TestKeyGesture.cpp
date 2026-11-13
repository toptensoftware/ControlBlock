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
    // as "control event", plus " param" when the param isn't empty
    struct Rig
    {
        GestureBank bank;
        uint32_t    now = 5000;
        bool        busy = false;               // what the host's busy() answers
        char        bound[16][48];
        int         bound_count = 0;
        char        log[64][48];                // one entry per trigger, in order
        int         logged = 0;

        explicit Rig(bool with_busy = true)
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
            h.trigger = [](const char *control, const char *event, const char *param, void *c) {
                Rig *r = (Rig *)c;
                if (r->logged < 64)
                    snprintf(r->log[r->logged++], 48, *param ? "%s %s %s" : "%s %s%s", control, event, param);
            };
            if (with_busy)
                h.busy = [](void *c) { return ((Rig *)c)->busy; };
            bank.Init(h, 500);
        }

        void Bind(const char *control, const char *event)
        {
            snprintf(bound[bound_count++], 48, "%s %s", control, event);
        }

        // The log as one string: "k tap|k release"
        const char *Log()
        {
            static char out[64 * 48 + 64];
            out[0] = 0;
            for (int i = 0; i < logged; i++) {
                if (i) strcat(out, "|");
                strcat(out, log[i]);
            }
            return out;
        }
        bool LogIs(const char *s) { return !strcmp(Log(), s); }
        void Clear() { logged = 0; }

        // Advance the clock and tick
        void TickTo(uint32_t ms_since, uint32_t down) { now = down + ms_since; bank.Tick(now); }
    };

    // Repeating timings: (delay, period[, not before]); no long-press time of its own
    GestureTiming Repeat(uint32_t delay, uint32_t period, uint32_t not_before = 0)
    {
        GestureTiming t = {};
        t.repeat_delay_ms = delay;
        t.repeat_ms       = period;
        t.not_before_ms   = not_before;
        return t;
    }
}

Fact("Gesture Plain Key Fires Press And Tap At Once, Release On Lift")
{
    Rig r;
    r.Bind("k", "tap");
    r.bank.Press("k", r.now, {});
    Assert(r.LogIs("k press|k tap"));               // no latency: tap doesn't wait for the lift
    r.now += 80;
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press|k tap|k release"));
}

Fact("Gesture Plain Key Never Fires Long, However Long Held")
{
    Rig r;
    r.Bind("k", "tap");
    r.bank.Press("k", r.now, {});
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
    r.bank.Press("k", r.now, {});
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
    r.bank.Press("k", r.now, {});
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
    r.bank.Press("k", r.now, {});
    r.now += 700;
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press|k long|k release"));
}

Fact("Gesture Long Only Control Still Taps On A Short Press")
{
    Rig r;
    r.Bind("k", "long");
    r.bank.Press("k", r.now, {});
    r.now += 100;
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press|k release|k tap"));     // a short press is still a tap; nothing is bound, so it's ignored downstream
}

Fact("Gesture Cancel Releases Without A Tap")
{
    Rig r;
    r.Bind("k", "tap");
    r.Bind("k", "long");
    r.bank.Press("k", r.now, {});
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
    r.bank.Press("k", r.now, { 200 });
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
    r.bank.Press("k", r.now, {});
    r.now += 649;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press"));
    r.now += 1;
    r.bank.Tick(r.now);
    Assert(r.LogIs("k press|k long"));
    r.bank.SetLongPressMs(0);                       // 0 = the firmware default
    r.bank.Release("k", r.now);
    r.bank.Press("k", r.now, {});
    r.now += 500;
    r.bank.Tick(r.now);
    Assert(r.logged == 5);                          // press long release press long
}

Fact("Gesture Controls Are Independent")
{
    Rig r;
    r.Bind("a", "long");
    r.Bind("b", "tap");
    r.bank.Press("a", r.now, {});
    r.bank.Press("b", r.now, {});
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
    r.bank.Press("k", r.now, {});
    r.bank.Press("k", r.now, {});                    // a second press of a held control
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
        r.bank.Press("k", r.now, {});
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
        r.bank.Press(name[i], r.now, {});
    }
    Assert(r.logged == 2 * GESTURE_SLOTS);          // press + tap each; the extra press fired nothing
    r.bank.Release(name[GESTURE_SLOTS], r.now);     // and its release is a stray
    Assert(r.logged == 2 * GESTURE_SLOTS);
    r.bank.Release(name[0], r.now);                 // frees a slot
    r.bank.Press(name[GESTURE_SLOTS], r.now, {});
    Assert(r.logged == 2 * GESTURE_SLOTS + 3);
}

Fact("Gesture Long Name Is Safe")
{
    Rig r;
    const char *longname = "touch[1234567890,1234567890,1234567890,1234567890]";
    r.bank.Press(longname, r.now, {});
    r.bank.Release(longname, r.now);
    Assert(r.logged == 3);                          // press, tap, release: matched on the truncated name
}

Fact("Gesture Time Wraps")
{
    Rig r;
    r.Bind("k", "long");
    r.now = 0xFFFFFF00u;
    r.bank.Press("k", r.now, {});
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

// Auto repeat (docs/standalone.md "Key gestures"): a repeating control fires
// press + tap at the press and again at down + delay, then every period, each
// with the param repeat=<n> (n = periods elapsed, 0 at the press)

Fact("Gesture Repeat Fires Press And Tap At The Press, Then After The Delay, Then Every Period")
{
    Rig r;
    r.Bind("k", "tap");
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(500, 100));
    Assert(r.LogIs("k press repeat=0|k tap repeat=0"));
    r.Clear();
    r.TickTo(499, down);
    Assert(r.logged == 0);
    r.TickTo(500, down);
    Assert(r.LogIs("k press repeat=1|k tap repeat=1"));
    r.Clear();
    r.TickTo(599, down);
    Assert(r.logged == 0);
    r.TickTo(600, down);
    r.TickTo(650, down);
    r.TickTo(700, down);
    Assert(r.LogIs("k press repeat=2|k tap repeat=2|k press repeat=3|k tap repeat=3"));
    r.Clear();
    r.now = down + 750;
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k release"));                   // one release, no param, no tap
    r.Clear();
    r.TickTo(2000, down);
    Assert(r.logged == 0);                          // and repeating stopped
}

Fact("Gesture Repeat Delay 0 Means The Period")
{
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(0, 100));
    r.Clear();
    r.TickTo(99, down);
    Assert(r.logged == 0);
    r.TickTo(100, down);
    Assert(r.LogIs("k press repeat=1|k tap repeat=1"));
}

Fact("Gesture Repeat Released Before The First Repeat Just Releases")
{
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(500, 100));
    r.now = down + 200;
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k press repeat=0|k tap repeat=0|k release"));
}

Fact("Gesture Repeat Cancel Releases And Stops Repeating")
{
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(100, 100));
    r.TickTo(100, down);
    r.Clear();
    r.now = down + 150;
    r.bank.Cancel("k", r.now);
    Assert(r.LogIs("k release"));
    r.Clear();
    r.TickTo(1000, down);
    Assert(r.logged == 0);
}

Fact("Gesture Repeat Never Fires Long Nor Defers Tap, Even With A Long Binding")
{
    Rig r;
    r.Bind("k", "tap");
    r.Bind("k", "long");
    uint32_t down = r.now;
    GestureTiming t = Repeat(500, 100);
    t.long_ms = 200;
    r.bank.Press("k", down, t);
    Assert(r.LogIs("k press repeat=0|k tap repeat=0"));     // tap at once, not deferred
    r.Clear();
    r.TickTo(300, down);                            // past the long-press time
    Assert(r.logged == 0);
    r.now = down + 400;
    r.bank.Release("k", r.now);
    Assert(r.LogIs("k release"));                   // no long, no tap at the release
}

Fact("Gesture Repeat Not Before Holds The First Repeat But Keeps The Schedule")
{
    // delay 100, held until 150 (the touch swipe window), period 100: n 1 at 150, n 2 at 200
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(100, 100, 150));
    r.Clear();
    r.TickTo(100, down);
    r.TickTo(149, down);
    Assert(r.logged == 0);
    r.TickTo(150, down);
    Assert(r.LogIs("k press repeat=1|k tap repeat=1"));
    r.Clear();
    r.TickTo(199, down);
    Assert(r.logged == 0);
    r.TickTo(200, down);
    Assert(r.LogIs("k press repeat=2|k tap repeat=2"));
}

Fact("Gesture Repeat Not Before Shorter Than The Delay Changes Nothing")
{
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(500, 100, 150));
    r.Clear();
    r.TickTo(499, down);
    Assert(r.logged == 0);
    r.TickTo(500, down);
    Assert(r.LogIs("k press repeat=1|k tap repeat=1"));
}

Fact("Gesture Repeat Is Skipped While Busy, And The Next Carries The Time Based Count")
{
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(100, 100));
    r.Clear();
    r.busy = true;
    r.TickTo(100, down);                            // n 1 due: skipped
    r.TickTo(200, down);                            // n 2 due: skipped
    Assert(r.logged == 0);
    r.busy = false;
    r.TickTo(300, down);
    Assert(r.LogIs("k press repeat=3|k tap repeat=3"));     // not 1: the count is time based
    r.Clear();
    r.TickTo(310, down);
    Assert(r.logged == 0);
    r.TickTo(400, down);
    Assert(r.LogIs("k press repeat=4|k tap repeat=4"));
}

Fact("Gesture Repeat Busy Clearing Mid Period Fires The Missed Repeat Then")
{
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(100, 100));
    r.Clear();
    r.busy = true;
    r.TickTo(100, down);
    r.busy = false;
    r.TickTo(140, down);
    Assert(r.LogIs("k press repeat=1|k tap repeat=1"));
    r.Clear();
    r.TickTo(199, down);
    Assert(r.logged == 0);
    r.TickTo(200, down);
    Assert(r.LogIs("k press repeat=2|k tap repeat=2"));
}

Fact("Gesture Repeat Press Fires Even While Busy")
{
    Rig r;
    r.busy = true;
    r.bank.Press("k", r.now, Repeat(100, 100));
    Assert(r.LogIs("k press repeat=0|k tap repeat=0"));     // only repeats are skipped
}

Fact("Gesture Repeat Without A Busy Callback Is Never Busy")
{
    Rig r(false);
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(100, 100));
    r.Clear();
    r.TickTo(100, down);
    Assert(r.LogIs("k press repeat=1|k tap repeat=1"));
}

Fact("Gesture Repeat Fires At Most One Per Tick")
{
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(100, 100));
    r.Clear();
    r.TickTo(450, down);                            // n 1..4 all due: one fires, with n 4
    Assert(r.LogIs("k press repeat=4|k tap repeat=4"));
    r.Clear();
    r.TickTo(499, down);
    Assert(r.logged == 0);
    r.TickTo(500, down);
    Assert(r.LogIs("k press repeat=5|k tap repeat=5"));
}

Fact("Gesture Repeat Time Wraps")
{
    Rig r;
    r.now = 0xFFFFFF00u;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(100, 100));
    r.Clear();
    r.TickTo(99, down);
    Assert(r.logged == 0);
    r.TickTo(100, down);
    r.TickTo(200, down);
    r.TickTo(299, down);                            // the clock wrapped at 256
    Assert(r.LogIs("k press repeat=1|k tap repeat=1|k press repeat=2|k tap repeat=2"));
    r.Clear();
    r.TickTo(300, down);
    Assert(r.LogIs("k press repeat=3|k tap repeat=3"));
}

Fact("Gesture Repeat Stops On Reset")
{
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("k", down, Repeat(100, 100));
    r.Clear();
    r.bank.Reset();
    r.TickTo(1000, down);
    r.bank.Release("k", r.now);
    Assert(r.logged == 0);                          // nothing repeats, and the release is a stray
}

Fact("Gesture Repeat And Plain Controls Are Independent")
{
    Rig r;
    uint32_t down = r.now;
    r.bank.Press("rep", down, Repeat(100, 100));
    r.bank.Press("plain", down, {});
    r.TickTo(100, down);
    r.bank.Release("plain", r.now);
    r.bank.Release("rep", r.now);
    Assert(r.LogIs("rep press repeat=0|rep tap repeat=0|plain press|plain tap"
                   "|rep press repeat=1|rep tap repeat=1|plain release|rep release"));
}
