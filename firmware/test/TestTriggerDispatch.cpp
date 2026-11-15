// The trigger dispatcher and the global op queue (docs/standalone.md "Triggers
// and bindings", "Actions"). The layout is the sample baked by host/src/bake,
// relocated in place and read through the generated structs. Home's bindings:
// touch[1] tap, touch[2] tap + long, touch[3] tap (switchActivity).
// Activity 0 ("Watch"): touch[16] tap (script), key2 tap (hidKey).
// Activity 1 ("Listen"): none.

#include "UnitTesting.h"
#include "Core.h"
#include "layout_env.h"
#include "op_queue.h"
#include "op_registry.h"
#include "trigger_dispatch.h"

using namespace SimpleLib;
using namespace layout_env;

namespace
{
    // The sample, a fake clock, the registry's executors (no HID keyboard, so
    // hidKey is Done at once; delay really waits) and a recorder for the ops
    // that start
    struct Rig
    {
        List<uint8_t>     bytes = LoadSample();
        const layoutdata::layoutRoot *root = nullptr;
        OpQueue           queue;
        TriggerDispatcher disp;
        ExecCtx           exec = {};
        uint32_t          now = 1000;
        uint32_t          run[256];             // op codes in the order they ran
        int               ran = 0;

        Rig()
        {
            root = Relocate(bytes);
            exec.ctx     = this;
            exec.now_ms  = [](void *c) { return ((Rig *)c)->now; };
            exec.started = [](const layoutdata::op *op, void *c) {
                Rig *r = (Rig *)c;
                if (r->ran < 256)
                    r->run[r->ran++] = op->op;
            };
            exec.queue = &queue;
            queue.Init(OpRegistryHost(&exec));
            disp.Init(root, &queue);
        }

        // Poll until the queue is empty or stuck (a delay), at the current time
        void Drain() { for (int i = 0; i < 1000 && queue.Poll(); i++) {} }
    };

    const uint32_t HID = OP_HID_KEY, DELAY = OP_DELAY, SWITCH = OP_SWITCH_ACTIVITY;
}

Fact("Trigger Matches Home Binding Exactly")
{
    Rig r;
    Assert(r.disp.HasBinding("touch[1]", "tap"));
    Assert(!r.disp.HasBinding("touch[1]", "long"));
    Assert(!r.disp.HasBinding("touch[1]", "tap "));
    Assert(!r.disp.HasBinding("touch[1]", "Tap"));
    Assert(!r.disp.HasBinding("touch[999]", "tap"));
    Assert(!r.disp.HasBinding("", ""));
    Assert(!r.disp.HasBinding(nullptr, "tap"));
}

Fact("Trigger Enqueues The Op List And Runs It")
{
    Rig r;
    Assert(r.disp.Trigger("touch[1]", "tap", "") == 1);
    Assert(r.queue.Count() == 1);
    Assert(r.ran == 0);                 // nothing runs until polled
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);
    Assert(r.queue.Count() == 0);
}

Fact("Unmatched Trigger Is Ignored")
{
    Rig r;
    Assert(r.disp.Trigger("touch[999]", "tap", "") == 0);
    Assert(r.disp.Trigger("touch[1]", "long", "") == 0);
    Assert(r.disp.Trigger(nullptr, nullptr, nullptr) == 0);
    Assert(r.queue.Count() == 0 && r.queue.Dropped() == 0);
}

Fact("Same Control Different Events Are Separate Bindings")
{
    Rig r;
    Assert(r.disp.HasBinding("touch[2]", "tap"));
    Assert(r.disp.HasBinding("touch[2]", "long"));
    Assert(r.disp.Trigger("touch[2]", "long", "") == 1);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);        // the long gesture's op, not the tap's
}

Fact("Activity Bindings Apply Only While Selected")
{
    Rig r;
    // No activity at boot: the activity's bindings don't exist
    Assert(!r.disp.HasBinding("key2", "tap"));
    Assert(!r.disp.HasBinding("touch[16]", "tap"));
    Assert(r.disp.Trigger("key2", "tap", "") == 0);

    Assert(r.disp.SetActivity(0));
    Assert(r.disp.HasBinding("key2", "tap"));
    Assert(r.disp.HasBinding("touch[16]", "tap"));
    Assert(r.disp.HasBinding("touch[1]", "tap"));         // Home's still match
    Assert(r.disp.Trigger("key2", "tap", "") == 1);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);

    // Another activity: Watch's bindings go, Home's stay
    Assert(r.disp.SetActivity(1));
    Assert(!r.disp.HasBinding("key2", "tap"));
    Assert(r.disp.HasBinding("touch[1]", "tap"));

    // Back to none
    Assert(r.disp.SetActivity(-1));
    Assert(r.disp.HasBinding("touch[1]", "tap"));
    Assert(!r.disp.HasBinding("touch[16]", "tap"));
}

Fact("SetActivity Rejects Bad Indices")
{
    Rig r;
    Assert(r.disp.SetActivity(0));
    Assert(!r.disp.SetActivity(2));                 // the sample has two
    Assert(!r.disp.SetActivity(-2));
    Assert(r.disp.HasBinding("key2", "tap"));     // unchanged by a rejected call
}

Fact("Op Queue Runs In Order One Per Poll")
{
    Rig r;
    r.disp.Trigger("touch[1]", "tap", "");
    r.disp.Trigger("touch[3]", "tap", "");
    Assert(r.queue.Count() == 2);
    Assert(r.queue.Poll());
    Assert(r.ran == 1 && r.run[0] == HID);
    Assert(r.queue.Poll());
    Assert(r.ran == 2 && r.run[1] == SWITCH);
    Assert(!r.queue.Poll());
    Assert(r.queue.Count() == 0);
}

Fact("Delay Holds The Queue Until It Has Passed")
{
    Rig r;
    r.disp.Trigger("touch[2]", "long", "");       // delay 100 ms
    r.disp.Trigger("touch[1]", "tap", "");        // behind it
    r.queue.Poll();                                 // starts the delay
    Assert(r.ran == 1 && r.run[0] == DELAY);

    Assert(r.queue.Poll());                         // still waiting: busy, nothing runs
    r.now += 99;
    Assert(r.queue.Poll());
    Assert(r.ran == 1);

    r.now += 1;                                     // exactly 100 ms
    r.Drain();
    Assert(r.ran == 2 && r.run[1] == HID);
    Assert(r.queue.Count() == 0);
}

Fact("Delay Survives The Clock Wrapping")
{
    Rig r;
    r.now = 0xFFFFFFF0u;
    r.disp.Trigger("touch[2]", "long", "");
    r.disp.Trigger("touch[1]", "tap", "");
    r.queue.Poll();
    r.now += 99;                                    // wraps past zero, still 1 ms short
    r.queue.Poll();
    Assert(r.ran == 1);
    r.now += 1;
    r.Drain();
    Assert(r.ran == 2);
}

Fact("Trigger Param Reaches The Ops And Is Freed Afterwards")
{
    Rig r;
    // touch[2] long = a delay: it is Pending, so the param is still held
    Assert(r.disp.Trigger("touch[2]", "long", "state=on") == 1);
    Assert(r.queue.ParamsInUse() == 1);
    r.queue.Poll();                                 // starts the delay
    Assert(r.queue.ParamsInUse() == 1);
    r.now += 1000;
    r.Drain();
    Assert(r.queue.ParamsInUse() == 0);

    // No param (null or empty) never takes a slot
    r.disp.Trigger("touch[1]", "tap", nullptr);
    r.disp.Trigger("touch[1]", "tap", "");
    Assert(r.queue.ParamsInUse() == 0);
    Assert(r.queue.Count() == 2);
}

Fact("Trigger With A Param Is Dropped When The Pool Is Full")
{
    Rig r;
    for (int i = 0; i < PARAM_SLOTS; i++)
        r.disp.Trigger("touch[1]", "tap", "p");
    Assert(r.queue.Count() == PARAM_SLOTS);
    Assert(r.disp.Trigger("touch[1]", "tap", "q") == 1);     // it matched, but could not be queued
    Assert(r.queue.Count() == PARAM_SLOTS);
    Assert(r.queue.ParamDropped() == 1);
    r.Drain();
    Assert(r.queue.ParamsInUse() == 0);
    r.disp.Trigger("touch[1]", "tap", "q");
    Assert(r.queue.Count() == 1);
}

Fact("Full Queue Drops Whole Lists And Counts Them")
{
    Rig r;
    for (int i = 0; i < OP_QUEUE_SIZE; i++)
        Assert(r.disp.Trigger("touch[1]", "tap", "") == 1);
    Assert(r.queue.Count() == OP_QUEUE_SIZE);
    Assert(r.queue.Dropped() == 0);

    // One more: nothing is added, one is dropped
    r.disp.Trigger("touch[1]", "tap", "");
    Assert(r.queue.Count() == OP_QUEUE_SIZE);
    Assert(r.queue.Dropped() == 1);

    // After the queue drains it takes triggers again, and the ring wrapped cleanly
    r.Drain();
    Assert(r.ran == OP_QUEUE_SIZE);
    Assert(r.disp.Trigger("touch[1]", "tap", "") == 1);
    r.Drain();
    Assert(r.ran == OP_QUEUE_SIZE + 1);
}

Fact("Ring Wraps Over Many Cycles")
{
    Rig r;
    int total = 0;
    for (int i = 0; i < 5 * OP_QUEUE_SIZE; i++) {
        Assert(r.disp.Trigger("touch[1]", "tap", "") == 1);
        Assert(r.disp.Trigger("touch[3]", "tap", "") == 1);
        r.ran = 0;
        r.Drain();
        Assert(r.ran == 2 && r.run[0] == HID && r.run[1] == SWITCH);    // in order, every cycle
        total += r.ran;
    }
    Assert(total == 10 * OP_QUEUE_SIZE);
    Assert(r.queue.Dropped() == 0 && r.queue.Count() == 0);
}
