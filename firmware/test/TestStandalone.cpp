// The standalone owner (docs/standalone.md milestone step 2): router key events
// go through the gestures and bindings to the op queue, and the host's
// take-over / release switches it off and re-arms it. The config is the
// sample baked by tools/bake (longPressMs 650). Home: touch[0,0] tap (hidKey),
// touch[1,0] tap (hidKey) + long (delay), touch[2,0] tap (switchActivity).

#include "UnitTesting.h"
#include "Core.h"
#include "config_env.h"
#include "standalone.h"
#include "hid_keyboard.h"
#include "touch_router.h"       // TOUCH_KEY_*

using namespace SimpleLib;
using namespace config_env;

namespace
{
    const uint32_t HID = 3, DELAY = 1, SWITCH = 2;      // op codes (consts.js OP)
    const uint32_t LONG_MS = 650;                       // the sample's settings.longPressMs

    struct Rig
    {
        List<uint8_t> bytes = LoadSample();
        Standalone    sa;
        uint32_t      now = 1000;
        uint32_t      run[64];                  // op codes in the order they ran
        int           ran = 0;
        uint8_t       reports[32][8];           // HID reports sent
        int           sent = 0;

        explicit Rig(bool start = true) { if (start) Start(); }

        void Start()
        {
            StandaloneHost h = {};
            h.ctx    = this;
            h.now_ms = [](void *c) { return ((Rig *)c)->now; };
            h.run    = [](const ConfigView &, const uint8_t *, uint32_t code, void *c) {
                Rig *r = (Rig *)c;
                if (r->ran < 64)
                    r->run[r->ran++] = code;
            };
            h.hid.ctx  = this;
            h.hid.send = [](const uint8_t *rep, void *c) {
                Rig *r = (Rig *)c;
                if (r->sent < 32)
                    memcpy(r->reports[r->sent++], rep, 8);
            };
            sa.Init(h, bytes.GetBuffer(), (uintptr_t)bytes.GetBuffer());
        }

        void Drain() { for (int i = 0; i < 1000 && sa.Poll(); i++) {} }
        // The last report's [modifiers, first key]
        bool LastIs(uint8_t mods, uint8_t key) const { return sent && reports[sent - 1][0] == mods && reports[sent - 1][2] == key; }
        void Advance(uint32_t ms) { now += ms; sa.Tick(); }
        void Key(int key, int action, int page = 0) { sa.Key(page, key, action, now); }
    };
}

Fact("Standalone Press Of A Home Key Runs Its Op")
{
    Rig r;
    r.Key(0, TOUCH_KEY_PRESS);              // tap with no `long` binding fires at once
    Assert(r.ran == 0);                     // queued, not run, until polled
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1);                     // the release adds nothing
}

Fact("Standalone Key Without A Binding Does Nothing")
{
    Rig r;
    r.Key(4, TOUCH_KEY_PRESS);              // touch[4,0]: not bound
    r.Key(4, TOUCH_KEY_RELEASE);
    r.Key(0, TOUCH_KEY_PRESS, 1);           // page 1: no activity selected
    r.Key(0, TOUCH_KEY_RELEASE, 1);
    r.Drain();
    Assert(r.ran == 0);
}

Fact("Standalone Long Binding Defers Tap")
{
    Rig r;
    r.Key(1, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.ran == 0);                     // tap waits for the release

    // A short press: tap on release
    r.Advance(100);
    r.Key(1, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);

    // A long press: `long` at the threshold (the config's 650 ms), and no tap
    r.ran = 0;
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS - 1);
    r.Drain();
    Assert(r.ran == 0);
    r.Advance(1);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);
    r.Key(1, TOUCH_KEY_RELEASE);
    r.Drain();
    r.now += 1000;                          // let the delay pass
    r.Drain();
    Assert(r.ran == 1);
}

Fact("Standalone Key's Own Long Press Time Overrides The Config's")
{
    Rig r;
    // Home key 1's longPressMs (after its four images) set to 900
    Put32(r.bytes, CB_HOME_KEY(1) + 32, 900);
    r.Start();
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS);                     // the config's time has passed: not long yet
    r.Drain();
    Assert(r.ran == 0);
    r.Advance(900 - LONG_MS);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);
}

Fact("Standalone Touch That Becomes A Swipe Fires No Tap And Frees The Key")
{
    Rig r;
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(100);
    r.Key(1, TOUCH_KEY_CANCEL);
    r.Advance(5000);                        // nothing is left held: no `long`
    r.Drain();
    Assert(r.ran == 0);

    r.Key(1, TOUCH_KEY_RELEASE);            // a stray release does nothing either
    r.Drain();
    Assert(r.ran == 0);

    r.Key(1, TOUCH_KEY_PRESS);              // and the key can be pressed again
    r.Key(1, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);
}

Fact("Standalone Fires Nothing While Hosted")
{
    Rig r;
    r.sa.SetHosted(true);
    Assert(r.sa.Hosted());
    r.Key(0, TOUCH_KEY_PRESS);
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(5000);
    r.Key(1, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 0 && r.sa.Queued() == 0);
}

Fact("Standalone Takeover Cancels Held Keys And Queued Ops")
{
    Rig r;
    r.Key(1, TOUCH_KEY_PRESS);              // held, tap deferred
    r.Key(0, TOUCH_KEY_PRESS);              // an op queued, not yet run
    Assert(r.sa.Queued() == 1);

    r.sa.SetHosted(true);
    Assert(r.sa.Queued() == 0);
    r.Drain();
    Assert(r.ran == 0);

    // Back to standalone: the key that was held is not held any more
    r.sa.SetHosted(false);
    r.Advance(5000);                        // no `long`
    r.Key(1, TOUCH_KEY_RELEASE);            // no tap
    r.Drain();
    Assert(r.ran == 0);
}

Fact("Standalone Release Re-arms From The Blob")
{
    Rig r;
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    Assert(!r.sa.Hosted());

    r.Key(0, TOUCH_KEY_PRESS);              // Home's bindings work again
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);

    // The long-press time is read again too
    r.ran = 0;
    r.Key(1, TOUCH_KEY_PRESS);
    r.Advance(LONG_MS);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);
}

Fact("Standalone Release Selects No Activity")
{
    Rig r;
    r.sa.SelectActivity(0);                 // Watch: touch[5,0] is bound
    r.Key(0, TOUCH_KEY_PRESS, 1);
    r.Key(0, TOUCH_KEY_RELEASE, 1);
    r.Drain();
    Assert(r.ran == 1);                     // its script op

    r.ran = 0;
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    r.Key(0, TOUCH_KEY_PRESS, 1);
    r.Key(0, TOUCH_KEY_RELEASE, 1);
    r.Drain();
    Assert(r.ran == 0);
}

// Home keys: 0 space, 6 ctrl+c (tap), 7 shift+a held while pressed
Fact("Standalone Hid Key Press Sends The Chord Then Lets Go")
{
    Rig r;
    r.Key(6, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.sent == 1 && r.LastIs(0x01, 0x06));        // ctrl+c down
    r.Advance(HID_TAP_GAP_MS - 1);
    r.Drain();
    Assert(r.sent == 1);
    r.Advance(1);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));              // up, from Poll
}

Fact("Standalone Taps Complete In Turn With A Settle Gap")
{
    Rig r;
    r.Key(0, TOUCH_KEY_PRESS);                          // space
    r.Key(6, TOUCH_KEY_PRESS);                          // ctrl+c, queued behind it
    r.Drain();
    Assert(r.sent == 1 && r.sa.Queued() == 1);             // the second waits
    r.Advance(HID_TAP_GAP_MS + HID_TAP_SETTLE_MS - 1);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));              // up sent, next not started
    r.Advance(1);
    r.Drain();
    Assert(r.sent == 3 && r.LastIs(0x01, 0x06));
    r.Advance(HID_TAP_GAP_MS);
    r.Drain();
    Assert(r.sent == 4 && r.LastIs(0, 0));
}

Fact("Standalone Hold Key Is Down While Pressed And Up On Release")
{
    Rig r;
    r.Key(7, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.sent == 1 && r.LastIs(0x02, 0x04));        // shift+a
    r.Advance(5000);
    r.Drain();
    Assert(r.sent == 1);
    r.Key(7, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));
}

Fact("Standalone Hold Key That Becomes A Swipe Is Let Go")
{
    Rig r;
    r.Key(7, TOUCH_KEY_PRESS);
    r.Drain();
    r.Key(7, TOUCH_KEY_CANCEL);
    r.Drain();
    Assert(r.sent == 2 && r.LastIs(0, 0));
}

Fact("Standalone Takeover Lets Go Of Held Keys And A Tap In Flight")
{
    Rig r;
    r.Key(7, TOUCH_KEY_PRESS);
    r.Key(6, TOUCH_KEY_PRESS);
    r.Drain();                                          // shift+a down, then ctrl+c down too
    Assert(r.sent == 2);
    r.sa.SetHosted(true);
    Assert(r.sent == 3 && r.LastIs(0, 0));              // all up at once
    r.Advance(5000);
    r.Drain();
    Assert(r.sent == 3);                                // nothing more, not even the tap's up
}

Fact("Standalone Disconnect While Already Standalone Lets Go Of Held Keys")
{
    Rig r;
    r.Key(7, TOUCH_KEY_PRESS);
    r.Drain();
    Assert(r.sent == 1);
    r.sa.SetHosted(false);                              // the protocol's Release() on an unplug
    Assert(r.sent == 2 && r.LastIs(0, 0));
}

Fact("Standalone Release Of A Hold Whose Press Never Ran Sends Nothing")
{
    Rig r;
    r.Key(7, TOUCH_KEY_PRESS);
    r.sa.SetHosted(true);                               // the press was queued, then dropped
    r.sa.SetHosted(false);
    r.Key(7, TOUCH_KEY_RELEASE);
    r.Drain();
    Assert(r.sent == 0);
}

Fact("Standalone Without A Config Ignores Everything")
{
    Rig r(false);
    StandaloneHost h = {};
    h.ctx    = &r;
    h.now_ms = [](void *c) { return ((Rig *)c)->now; };
    h.run    = [](const ConfigView &, const uint8_t *, uint32_t, void *c) { ((Rig *)c)->ran++; };
    r.sa.Init(h, nullptr, 0);
    r.Key(0, TOUCH_KEY_PRESS);
    r.Key(0, TOUCH_KEY_RELEASE);
    r.Advance(5000);
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    r.Key(0, TOUCH_KEY_PRESS);
    Assert(!r.sa.Poll());
    Assert(r.ran == 0);
}
