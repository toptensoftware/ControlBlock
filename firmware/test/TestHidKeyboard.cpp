// HID keyboard report building (docs/standalone.md milestone step 3): a chord
// becomes a modifier byte and one key code; tap, down, up and hold; nothing
// stays down after ReleaseAll.

#include "UnitTesting.h"
#include "Core.h"
#include "hid_keyboard.h"

using namespace SimpleLib;

namespace
{
    const uint8_t CTRL = 0x01, SHIFT = 0x02;
    const uint8_t KEY_A = 0x04, KEY_C = 0x06, KEY_SPACE = 0x2c;

    struct Rig
    {
        HidKeyboard kb;
        uint8_t     reports[32][8];
        int         count = 0;

        Rig()
        {
            HidSink s = {};
            s.ctx  = this;
            s.send = [](const uint8_t *report, void *c) {
                Rig *r = (Rig *)c;
                if (r->count < 32)
                    memcpy(r->reports[r->count++], report, 8);
            };
            kb.Init(s);
        }

        // The last report is [modifiers, 0, key x 6]
        bool Is(int i, uint8_t mods, uint8_t key0, uint8_t key1 = 0) const
        {
            const uint8_t *r = reports[i];
            return r[0] == mods && r[1] == 0 && r[2] == key0 && r[3] == key1 && !r[4] && !r[5] && !r[6] && !r[7];
        }
    };
}

Fact("HidKeyboard Tap Sends Down, A Gap, Then Up")
{
    Rig r;
    uint32_t hold = r.kb.Run(1000, HID_MODE_TAP, 0, KEY_SPACE, false);
    Assert(r.count == 1 && r.Is(0, 0, KEY_SPACE));
    Assert(hold == HID_TAP_GAP_MS + HID_TAP_SETTLE_MS);

    r.kb.Poll(1000 + HID_TAP_GAP_MS - 1);
    Assert(r.count == 1);                           // not yet
    r.kb.Poll(1000 + HID_TAP_GAP_MS);
    Assert(r.count == 2 && r.Is(1, 0, 0));
    r.kb.Poll(5000);
    Assert(r.count == 2);                           // only once
}

Fact("HidKeyboard Chord Is Modifiers Plus One Key")
{
    Rig r;
    r.kb.Run(0, HID_MODE_TAP, CTRL | SHIFT, KEY_C, false);
    Assert(r.count == 1 && r.Is(0, CTRL | SHIFT, KEY_C));
    r.kb.Poll(HID_TAP_GAP_MS);
    Assert(r.count == 2 && r.Is(1, 0, 0));
}

Fact("HidKeyboard Modifier Only Chord")
{
    Rig r;
    r.kb.Run(0, HID_MODE_DOWN, SHIFT, 0, false);
    Assert(r.count == 1 && r.Is(0, SHIFT, 0));
    r.kb.Run(0, HID_MODE_UP, SHIFT, 0, false);
    Assert(r.count == 2 && r.Is(1, 0, 0));
}

Fact("HidKeyboard Down And Up Send One Report Each And Don't Hold The Queue")
{
    Rig r;
    Assert(r.kb.Run(0, HID_MODE_DOWN, CTRL, KEY_A, false) == 0);
    Assert(r.count == 1 && r.Is(0, CTRL, KEY_A));
    Assert(r.kb.Run(0, HID_MODE_UP, CTRL, KEY_A, false) == 0);
    Assert(r.count == 2 && r.Is(1, 0, 0));
}

Fact("HidKeyboard Up Of A Key That Was Never Down Sends Nothing")
{
    Rig r;
    r.kb.Run(0, HID_MODE_UP, CTRL, KEY_A, false);
    Assert(r.count == 0);
    r.kb.Run(0, HID_MODE_HOLD, CTRL, KEY_A, true);  // a hold's release with no press before it
    Assert(r.count == 0);
}

Fact("HidKeyboard Hold Is Down On Press And Up On Release")
{
    Rig r;
    Assert(r.kb.Run(0, HID_MODE_HOLD, SHIFT, KEY_A, false) == 0);
    Assert(r.count == 1 && r.Is(0, SHIFT, KEY_A));
    r.kb.Poll(10000);
    Assert(r.count == 1);                           // stays down
    Assert(r.kb.Run(0, HID_MODE_HOLD, SHIFT, KEY_A, true) == 0);
    Assert(r.count == 2 && r.Is(1, 0, 0));
}

Fact("HidKeyboard Two Keys Held Then Released One At A Time")
{
    Rig r;
    r.kb.Run(0, HID_MODE_DOWN, 0, KEY_A, false);
    r.kb.Run(0, HID_MODE_DOWN, 0, KEY_C, false);
    Assert(r.Is(1, 0, KEY_A, KEY_C));
    r.kb.Run(0, HID_MODE_UP, 0, KEY_A, false);
    Assert(r.Is(2, 0, KEY_C));
    r.kb.Run(0, HID_MODE_DOWN, 0, KEY_C, false);    // already down: no change, no report
    Assert(r.count == 3);
}

Fact("HidKeyboard Release All Lets Go Of Everything")
{
    Rig r;
    r.kb.ReleaseAll();
    Assert(r.count == 0);                           // nothing down: nothing sent

    r.kb.Run(0, HID_MODE_DOWN, CTRL, KEY_A, false);
    r.kb.ReleaseAll();
    Assert(r.count == 2 && r.Is(1, 0, 0));
    r.kb.ReleaseAll();
    Assert(r.count == 2);
}

Fact("HidKeyboard Release All Cancels A Tap In Flight")
{
    Rig r;
    r.kb.Run(0, HID_MODE_TAP, 0, KEY_A, false);
    r.kb.ReleaseAll();
    Assert(r.count == 2 && r.Is(1, 0, 0));
    r.kb.Poll(HID_TAP_GAP_MS + 100);                // the tap's own up is gone
    Assert(r.count == 2);
}

Fact("HidKeyboard Tap Gap Survives The Clock Wrapping")
{
    Rig r;
    uint32_t t = 0xFFFFFFFFu - 5;
    r.kb.Run(t, HID_MODE_TAP, 0, KEY_A, false);
    r.kb.Poll(t + HID_TAP_GAP_MS - 1);
    Assert(r.count == 1);
    r.kb.Poll(t + HID_TAP_GAP_MS);
    Assert(r.count == 2);
}

Fact("HidKeyboard A Second Tap Finishes The First")
{
    Rig r;
    r.kb.Run(0, HID_MODE_TAP, 0, KEY_A, false);
    r.kb.Run(1, HID_MODE_TAP, 0, KEY_C, false);     // a caller that ignored the hold
    Assert(r.count == 3 && r.Is(1, 0, 0) && r.Is(2, 0, KEY_C));
}

Fact("HidKeyboard Ignores An Unknown Mode")
{
    Rig r;
    Assert(r.kb.Run(0, 9, 0, KEY_A, false) == 0);
    Assert(r.count == 0);
}
