// The activity switch sequence (activity_switch.h): from (old, new, devices on)
// to the op lists to queue, in the spec's order, and the new device mask. Pure;
// the layout is built in memory here (real pointers: the tests build 32-bit).
// Then the Standalone state (selected activity, device mask) around it.

#include "UnitTesting.h"
#include "Core.h"
#include "layout_env.h"
#include "activity_switch.h"
#include "standalone.h"

using namespace SimpleLib;
using namespace layout_env;

namespace
{
    // 3 devices (tv 0, avr 1, pc 2) and 3 activities:
    //   A uses tv, avr   B uses avr, pc   C uses nothing and has no hooks.
    // Every hook and device list is one op, so a list is told apart by its `ops` pointer.
    struct Cfg
    {
        layoutdata::op         ops[40] = {};
        layoutdata::op        *ptrs[40] = {};
        int                 n = 0;
        layoutdata::device     dev[3] = {};
        layoutdata::activity   act[3] = {};
        int32_t             a_devs[2] = { 0, 1 }, b_devs[2] = { 1, 2 };
        layoutdata::layoutRoot root = {};

        layoutdata::op **List()            // a fresh one-op list
        {
            ops[n].op = OP_DELAY;
            ptrs[n]   = &ops[n];
            return &ptrs[n++];
        }

        Cfg()
        {
            for (int i = 0; i < 3; i++) {
                dev[i].turnOn_count  = 1; dev[i].turnOn  = List();
                dev[i].turnOff_count = 1; dev[i].turnOff = List();
            }
            for (int i = 0; i < 2; i++) {
                layoutdata::activity &a = act[i];
                a.willDeactivate_count = 1; a.willDeactivate = List();
                a.willActivate_count   = 1; a.willActivate   = List();
                a.didActivate_count    = 1; a.didActivate    = List();
                a.didDeactivate_count  = 1; a.didDeactivate  = List();
            }
            act[0].devices_count = 2; act[0].devices = a_devs;
            act[1].devices_count = 2; act[1].devices = b_devs;
            root.devices_count    = 3; root.devices    = dev;
            root.activities_count = 3; root.activities = act;
        }
    };

    const int A = 0, B = 1, C = 2, NONE = -1;
    const uint32_t TV = 1u << 0, AVR = 1u << 1, PC = 1u << 2;

    // The plan's list `i` is the op list `p` of `count` ops
    bool Is(const ActivitySwitch &s, int i, layoutdata::op *const *p, uint32_t count = 1)
    {
        return i < s.count && s.lists[i].ops == p && s.lists[i].count == count;
    }
}

Fact("ActivitySwitch None To A Runs willActivate, TurnOns, didActivate")
{
    Cfg c;
    ActivitySwitch s;
    Assert(ActivitySwitchPlan(&c.root, NONE, A, 0, s));
    Assert(s.count == 4);
    Assert(Is(s, 0, c.act[A].willActivate));
    Assert(Is(s, 1, c.dev[0].turnOn));          // tv, then avr: the activity's `devices` order
    Assert(Is(s, 2, c.dev[1].turnOn));
    Assert(Is(s, 3, c.act[A].didActivate));
    Assert(s.devices_on == (TV | AVR));
}

Fact("ActivitySwitch A To B Follows The Spec Order And Keeps The Shared Device On")
{
    Cfg c;
    ActivitySwitch s;
    Assert(ActivitySwitchPlan(&c.root, A, B, TV | AVR, s));
    // old.willDeactivate, new.willActivate, turnOn pc, turnOff tv, new.didActivate, old.didDeactivate
    Assert(s.count == 6);
    Assert(Is(s, 0, c.act[A].willDeactivate));
    Assert(Is(s, 1, c.act[B].willActivate));
    Assert(Is(s, 2, c.dev[2].turnOn));
    Assert(Is(s, 3, c.dev[0].turnOff));
    Assert(Is(s, 4, c.act[B].didActivate));
    Assert(Is(s, 5, c.act[A].didDeactivate));
    Assert(s.devices_on == (AVR | PC));         // avr never power cycled
}

Fact("ActivitySwitch B Back To A")
{
    Cfg c;
    ActivitySwitch s;
    Assert(ActivitySwitchPlan(&c.root, B, A, AVR | PC, s));
    Assert(s.count == 6);
    Assert(Is(s, 2, c.dev[0].turnOn));          // tv on
    Assert(Is(s, 3, c.dev[2].turnOff));         // pc off
    Assert(s.devices_on == (TV | AVR));
}

Fact("ActivitySwitch To None Turns Every Device That Is On Off")
{
    Cfg c;
    ActivitySwitch s;
    Assert(ActivitySwitchPlan(&c.root, A, NONE, TV | AVR, s));
    Assert(s.count == 4);
    Assert(Is(s, 0, c.act[A].willDeactivate));
    Assert(Is(s, 1, c.dev[0].turnOff));         // the old activity's `devices` order
    Assert(Is(s, 2, c.dev[1].turnOff));
    Assert(Is(s, 3, c.act[A].didDeactivate));
    Assert(s.devices_on == 0);
}

Fact("ActivitySwitch To None Ends With didDeactivate And Also Turns Off Stale Devices")
{
    Cfg c;
    ActivitySwitch s;
    // pc was left on by an earlier hosted session: "everything off" includes it, after the old activity's own
    Assert(ActivitySwitchPlan(&c.root, A, NONE, TV | AVR | PC, s));
    Assert(s.count == 5);
    Assert(Is(s, 0, c.act[A].willDeactivate));
    Assert(Is(s, 1, c.dev[0].turnOff));
    Assert(Is(s, 2, c.dev[1].turnOff));
    Assert(Is(s, 3, c.dev[2].turnOff));
    Assert(Is(s, 4, c.act[A].didDeactivate));
    Assert(s.devices_on == 0);
}

Fact("ActivitySwitch Only Devices That Change Run, Not Those Already On Or Off")
{
    Cfg c;
    ActivitySwitch s;
    // From none with tv already on (kept across a takeover): only avr is turned on
    Assert(ActivitySwitchPlan(&c.root, NONE, A, TV, s));
    Assert(s.count == 3);
    Assert(Is(s, 0, c.act[A].willActivate));
    Assert(Is(s, 1, c.dev[1].turnOn));
    Assert(Is(s, 2, c.act[A].didActivate));
    Assert(s.devices_on == (TV | AVR));

    // A -> B where the mask says pc is already on and tv already off: nothing to do for them
    Assert(ActivitySwitchPlan(&c.root, A, B, AVR | PC, s));
    Assert(s.count == 4);
    Assert(Is(s, 0, c.act[A].willDeactivate));
    Assert(Is(s, 1, c.act[B].willActivate));
    Assert(Is(s, 2, c.act[B].didActivate));
    Assert(Is(s, 3, c.act[A].didDeactivate));
    Assert(s.devices_on == (AVR | PC));
}

Fact("ActivitySwitch Stale Devices Stay On When Switching From None To An Activity")
{
    Cfg c;
    ActivitySwitch s;
    Assert(ActivitySwitchPlan(&c.root, NONE, B, PC | TV, s));      // tv on from before, not B's
    Assert(s.count == 3);                                          // avr on; pc already on; tv left alone
    Assert(Is(s, 1, c.dev[1].turnOn));
    Assert(s.devices_on == (TV | AVR | PC));
}

Fact("ActivitySwitch To The Same Activity Is Nothing")
{
    Cfg c;
    ActivitySwitch s;
    Assert(ActivitySwitchPlan(&c.root, A, A, TV | AVR, s));
    Assert(s.count == 0 && s.devices_on == (TV | AVR));
    Assert(ActivitySwitchPlan(&c.root, NONE, NONE, PC, s));
    Assert(s.count == 0 && s.devices_on == PC);
}

Fact("ActivitySwitch Skips Empty Hooks")
{
    Cfg c;
    ActivitySwitch s;
    // C has no hooks and no devices: from none, nothing at all to queue
    Assert(ActivitySwitchPlan(&c.root, NONE, C, 0, s));
    Assert(s.count == 0 && s.devices_on == 0);
    // A -> C: A's hooks and its devices going off
    Assert(ActivitySwitchPlan(&c.root, A, C, TV | AVR, s));
    Assert(s.count == 4);
    Assert(Is(s, 0, c.act[A].willDeactivate));
    Assert(Is(s, 1, c.dev[0].turnOff));
    Assert(Is(s, 2, c.dev[1].turnOff));
    Assert(Is(s, 3, c.act[A].didDeactivate));
    Assert(s.devices_on == 0);

    // A device with no turnOn (or turnOff) list takes no entry but the mask still changes
    c.dev[0].turnOn_count = 0;
    Assert(ActivitySwitchPlan(&c.root, NONE, A, 0, s));
    Assert(s.count == 3);
    Assert(s.devices_on == (TV | AVR));
}

Fact("ActivitySwitch Lists Carry No Release Flag Or Param")
{
    Cfg c;
    ActivitySwitch s;
    Assert(ActivitySwitchPlan(&c.root, A, B, TV | AVR, s));
    for (int i = 0; i < s.count; i++)
        Assert(!s.lists[i].release && s.lists[i].param == nullptr);
}

Fact("ActivitySwitch Rejects An Index Outside The Layout And Leaves The Plan Empty")
{
    Cfg c;
    ActivitySwitch s;
    Assert(!ActivitySwitchPlan(&c.root, NONE, 3, 0, s));
    Assert(!ActivitySwitchPlan(&c.root, 3, A, 0, s));
    Assert(!ActivitySwitchPlan(&c.root, NONE, -2, 0, s));
    Assert(!ActivitySwitchPlan(nullptr, NONE, A, 0, s));
    Assert(s.count == 0);
}

Fact("ActivitySwitch Ignores A Device Index Outside The Layout")
{
    Cfg c;
    c.a_devs[1] = 7;                            // the validator prevents this; the plan must not read past devices[]
    ActivitySwitch s;
    Assert(ActivitySwitchPlan(&c.root, NONE, A, 0, s));
    Assert(s.devices_on == TV);
    Assert(s.count == 3);
}

Fact("ActivitySwitch Fits The Most A Switch Can Need")
{
    // 32 devices all changing, plus the four hooks
    static_assert(ACTIVITY_SWITCH_MAX_LISTS == 4 + LAYOUT_MAX_DEVICES, "four hooks and one list per device");
    Cfg c;
    layoutdata::device many[LAYOUT_MAX_DEVICES] = {};
    int32_t from[LAYOUT_MAX_DEVICES], to[LAYOUT_MAX_DEVICES];
    for (int i = 0; i < LAYOUT_MAX_DEVICES; i++) {
        many[i] = c.dev[0];
        from[i] = i;
    }
    c.root.devices_count = LAYOUT_MAX_DEVICES;
    c.root.devices       = many;
    c.act[0].devices_count = LAYOUT_MAX_DEVICES / 2;        // A: 0..15
    c.act[0].devices       = from;
    for (int i = 0; i < LAYOUT_MAX_DEVICES / 2; i++)
        to[i] = LAYOUT_MAX_DEVICES / 2 + i;                 // B: 16..31
    c.act[1].devices_count = LAYOUT_MAX_DEVICES / 2;
    c.act[1].devices       = to;
    ActivitySwitch s;
    Assert(ActivitySwitchPlan(&c.root, A, B, 0xFFFFu, s));
    Assert(s.count == 4 + LAYOUT_MAX_DEVICES);
    Assert(s.devices_on == 0xFFFF0000u);
}

// ---- Standalone state ----

Fact("Standalone Starts With No Activity And No Devices On")
{
    List<uint8_t> bytes = LoadSample();
    const layoutdata::layoutRoot *root = Relocate(bytes);
    Standalone sa;
    StandaloneHost h = {};
    h.now_ms = [](void *) { return (uint32_t)0; };
    sa.Init(h, root);
    Assert(sa.Activity() == -1 && sa.DevicesOn() == 0);
}

Fact("Standalone SelectActivity Tracks The Selection And Re-Arming Clears It")
{
    List<uint8_t> bytes = LoadSample();
    const layoutdata::layoutRoot *root = Relocate(bytes);
    Standalone sa;
    StandaloneHost h = {};
    h.now_ms = [](void *) { return (uint32_t)0; };
    sa.Init(h, root);

    Assert(sa.SelectActivity(1));
    Assert(sa.Activity() == 1);
    Assert(!sa.SelectActivity((int)root->activities_count));    // refused: unchanged
    Assert(sa.Activity() == 1);
    Assert(sa.SelectActivity(-1));
    Assert(sa.Activity() == -1);

    // Takeover and release both end with none selected
    Assert(sa.SelectActivity(0));
    sa.SetHosted(true);
    Assert(sa.Activity() == -1);
    sa.SetHosted(false);
    Assert(sa.Activity() == -1);
    Assert(sa.DevicesOn() == 0);                // (the mask itself is kept across a re-arm: step 5 sets it)
}
