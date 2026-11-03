// The trigger dispatcher and the global op queue (docs/standalone.md "Triggers
// and bindings", "Actions"). The config is the sample baked by tools/bake,
// read unrelocated (pointers are offsets: ConfigView adjusts by the blob's
// address). Home's bindings: touch[0,0] tap, touch[1,0] tap + long,
// touch[2,0] tap (switchActivity). Activity 0 ("Watch"): touch[5,0] tap
// (script), macro1 tap (hidKey). Activity 1 ("Listen"): none.

#include "UnitTesting.h"
#include "Core.h"
#include "config_env.h"
#include "config_view.h"
#include "op_queue.h"
#include "trigger_dispatch.h"

using namespace SimpleLib;
using namespace config_env;

namespace
{
    // The sample, a fake clock, and a recorder for the ops the queue runs
    struct Rig
    {
        List<uint8_t>     bytes = LoadSample();
        ConfigView        cfg   = ConfigView(bytes.GetBuffer(), (uintptr_t)bytes.GetBuffer());
        OpQueue           queue;
        TriggerDispatcher disp;
        uint32_t          now = 1000;
        uint32_t          run[256];             // op codes in the order they ran
        int               ran = 0;

        Rig()
        {
            OpHost h = {};
            h.ctx    = this;
            h.now_ms = [](void *c) { return ((Rig *)c)->now; };
            h.run    = [](const ConfigView &, const uint8_t *op, uint32_t code, void *c) {
                Rig *r = (Rig *)c;
                if (r->ran < 256)
                    r->run[r->ran++] = code;
            };
            queue.Init(&cfg, h);
            disp.Init(&cfg, &queue);
        }

        // Poll until the queue is empty or stuck (a delay), at the current time
        void Drain() { for (int i = 0; i < 1000 && queue.Poll(); i++) {} }
    };

    const uint32_t HID = 3, DELAY = 1, SWITCH = 2;      // op codes (consts.js OP)
}

Fact("Trigger Matches Home Binding Exactly")
{
    Rig r;
    Assert(r.disp.HasBinding("touch[0,0]", "tap"));
    Assert(!r.disp.HasBinding("touch[0,0]", "long"));
    Assert(!r.disp.HasBinding("touch[0,0]", "tap "));
    Assert(!r.disp.HasBinding("touch[0,0]", "Tap"));
    Assert(!r.disp.HasBinding("touch[9,9]", "tap"));
    Assert(!r.disp.HasBinding("", ""));
    Assert(!r.disp.HasBinding(nullptr, "tap"));
}

Fact("Trigger Enqueues The Op List And Runs It")
{
    Rig r;
    Assert(r.disp.Trigger("touch[0,0]", "tap", "") == 1);
    Assert(r.queue.Count() == 1);
    Assert(r.ran == 0);                 // nothing runs until polled
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);
    Assert(r.queue.Count() == 0);
}

Fact("Unmatched Trigger Is Ignored")
{
    Rig r;
    Assert(r.disp.Trigger("touch[9,9]", "tap", "") == 0);
    Assert(r.disp.Trigger("touch[0,0]", "long", "") == 0);
    Assert(r.disp.Trigger(nullptr, nullptr, nullptr) == 0);
    Assert(r.queue.Count() == 0 && r.queue.Dropped() == 0);
}

Fact("Same Control Different Events Are Separate Bindings")
{
    Rig r;
    Assert(r.disp.HasBinding("touch[1,0]", "tap"));
    Assert(r.disp.HasBinding("touch[1,0]", "long"));
    Assert(r.disp.Trigger("touch[1,0]", "long", "") == 1);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == DELAY);        // the long gesture's op, not the tap's
}

Fact("Activity Bindings Apply Only While Selected")
{
    Rig r;
    // No activity at boot: the activity's bindings don't exist
    Assert(!r.disp.HasBinding("macro1", "tap"));
    Assert(!r.disp.HasBinding("touch[5,0]", "tap"));
    Assert(r.disp.Trigger("macro1", "tap", "") == 0);

    Assert(r.disp.SetActivity(0));
    Assert(r.disp.HasBinding("macro1", "tap"));
    Assert(r.disp.HasBinding("touch[5,0]", "tap"));
    Assert(r.disp.HasBinding("touch[0,0]", "tap"));         // Home's still match
    Assert(r.disp.Trigger("macro1", "tap", "") == 1);
    r.Drain();
    Assert(r.ran == 1 && r.run[0] == HID);

    // Another activity: Watch's bindings go, Home's stay
    Assert(r.disp.SetActivity(1));
    Assert(!r.disp.HasBinding("macro1", "tap"));
    Assert(r.disp.HasBinding("touch[0,0]", "tap"));

    // Back to none
    Assert(r.disp.SetActivity(-1));
    Assert(r.disp.HasBinding("touch[0,0]", "tap"));
    Assert(!r.disp.HasBinding("touch[5,0]", "tap"));
}

Fact("SetActivity Rejects Bad Indices")
{
    Rig r;
    Assert(r.disp.SetActivity(0));
    Assert(!r.disp.SetActivity(2));                 // the sample has two
    Assert(!r.disp.SetActivity(-2));
    Assert(r.disp.HasBinding("macro1", "tap"));     // unchanged by a rejected call
}

Fact("Op Queue Runs In Order One Per Poll")
{
    Rig r;
    r.disp.Trigger("touch[0,0]", "tap", "");
    r.disp.Trigger("touch[2,0]", "tap", "");
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
    r.disp.Trigger("touch[1,0]", "long", "");       // delay 100 ms
    r.disp.Trigger("touch[0,0]", "tap", "");        // behind it
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
    r.disp.Trigger("touch[1,0]", "long", "");
    r.disp.Trigger("touch[0,0]", "tap", "");
    r.queue.Poll();
    r.now += 99;                                    // wraps past zero, still 1 ms short
    r.queue.Poll();
    Assert(r.ran == 1);
    r.now += 1;
    r.Drain();
    Assert(r.ran == 2);
}

Fact("Full Queue Drops Whole Lists And Counts Them")
{
    Rig r;
    for (int i = 0; i < OP_QUEUE_SIZE; i++)
        Assert(r.disp.Trigger("touch[0,0]", "tap", "") == 1);
    Assert(r.queue.Count() == OP_QUEUE_SIZE);
    Assert(r.queue.Dropped() == 0);

    // One more: nothing is added, one is dropped
    r.disp.Trigger("touch[0,0]", "tap", "");
    Assert(r.queue.Count() == OP_QUEUE_SIZE);
    Assert(r.queue.Dropped() == 1);

    // After the queue drains it takes triggers again, and the ring wrapped cleanly
    r.Drain();
    Assert(r.ran == OP_QUEUE_SIZE);
    Assert(r.disp.Trigger("touch[0,0]", "tap", "") == 1);
    r.Drain();
    Assert(r.ran == OP_QUEUE_SIZE + 1);
}

Fact("Ring Wraps Over Many Cycles")
{
    Rig r;
    int total = 0;
    for (int i = 0; i < 5 * OP_QUEUE_SIZE; i++) {
        Assert(r.disp.Trigger("touch[0,0]", "tap", "") == 1);
        Assert(r.disp.Trigger("touch[2,0]", "tap", "") == 1);
        r.ran = 0;
        r.Drain();
        Assert(r.ran == 2 && r.run[0] == HID && r.run[1] == SWITCH);    // in order, every cycle
        total += r.ran;
    }
    Assert(total == 10 * OP_QUEUE_SIZE);
    Assert(r.queue.Dropped() == 0 && r.queue.Count() == 0);
}
