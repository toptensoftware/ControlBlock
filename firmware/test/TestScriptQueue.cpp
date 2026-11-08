// The `script` op in the op queue: the active slot is the script's while it waits (the queue
// is held), a script error ends only that op, and takeover cancels a waiting script.
// switchActivity() waits for the switch's ops: the script is parked (OpQueue::ParkActive),
// they run, then the rest of the script, then the rest of its list; the parked time is not
// the script's (SCRIPT_WALL_MS). A switch that would run a script is refused from a script,
// and a second script can never start while one runs.

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "op_queue.h"
#include "script_env.h"

using namespace SimpleLib;
using namespace script_env;

namespace
{
    struct QRig
    {
        Rig        r;
        OpQueue    q;
        layoutdata::scriptOp  s1, s2;
        layoutdata::hidKeyOp  key_a, key_x, key_y;
        layoutdata::delayOp   wait = {};
        layoutdata::op       *list1[2];
        layoutdata::op       *watch[1];           // what the fake switch to Watch (0) runs
        layoutdata::op       *listen[1];          // and to Listen (1); none (-1) runs nothing

        // A fake switchActivity that pushes its list to run next, as the real one does
        QRig()
        {
            r.exec.queue = &q;
            q.Init(OpRegistryHost(&r.exec));
            key_a = Key(0x04);
            key_x = Key(0x1b);
            key_y = Key(0x1c);
            wait.base.op = OP_DELAY;
            watch[0]  = &key_x.base;
            listen[0] = &key_y.base;
            s_cur = this;
            r.exec.switch_activity = [](int index, void *) -> const char * {
                QRig *g = s_cur;
                OpListRef ref;
                ref.ops   = index == 0 ? g->watch : g->listen;
                ref.count = index < 0 ? 0 : 1;
                return g->q.PushListsNext(&ref, 1) ? nullptr : "full";
            };
        }

        static layoutdata::hidKeyOp Key(uint8_t k)
        {
            layoutdata::hidKeyOp o = {};
            o.base.op = OP_HID_KEY; o.mode = HID_MODE_DOWN; o.key = k;
            return o;
        }

        // Poll until idle, the clock moving 1 ms each time
        void Drain(int max = 100000)
        {
            for (int i = 0; i < max && q.Poll(); i++)
                r.now++;
        }

        // The key added by report `i` (each is a key down joining the ones before)
        uint8_t Added(int i) const { return r.reports[i][2 + i]; }

        static QRig *s_cur;
    };
    QRig *QRig::s_cur = nullptr;
}

Fact("ScriptQueue A Waiting Script Holds The Queue")
{
    QRig g;
    g.s1 = g.r.Op("delay(30) log('script done')");
    g.list1[0] = &g.s1.base;
    g.list1[1] = &g.key_a.base;
    Assert(g.q.PushList(2, g.list1));

    Assert(g.q.Poll());                                     // the script starts and waits
    Assert(g.r.sent == 0);
    for (int i = 0; i < 20; i++) {
        g.r.now++;
        Assert(g.q.Poll());
    }
    Assert(g.r.sent == 0 && g.r.log_lines == 0);            // still held
    g.Drain();
    Assert(strcmp(g.r.log, "script done") == 0);
    Assert(g.r.sent == 1 && g.r.reports[0][2] == 0x04);     // the next op ran after it
    Assert(g.q.Errors() == 0);
}

Fact("ScriptQueue A Script Error Ends Only That Op")
{
    QRig g;
    g.s1 = g.r.Op("error('bad')");
    g.list1[0] = &g.s1.base;
    g.list1[1] = &g.key_a.base;
    Assert(g.q.PushList(2, g.list1));
    g.Drain();
    Assert(g.q.Errors() == 1 && g.q.LastError() != nullptr);
    Assert(g.r.sent == 1);                                  // the key still went
}

Fact("ScriptQueue Takeover Cancels A Waiting Script")
{
    QRig g;
    g.s1 = g.r.Op("http.get('http://tv/x') log('never')");
    g.list1[0] = &g.s1.base;
    g.list1[1] = &g.key_a.base;
    Assert(g.q.PushList(2, g.list1));
    Assert(g.q.Poll());
    Assert(g.r.host.Running());

    g.q.Cancel();
    Assert(!g.r.host.Running());
    Assert(g.r.net.n_abandoned == 1);
    Assert(!g.q.Poll());
    Assert(g.r.sent == 0 && !g.r.Logged("never"));
}

Fact("ScriptQueue Takeover Lets Go Of Keys A Script Pressed")
{
    QRig g;
    g.s1 = g.r.Op("hid.key('shift', 'down') delay(1000)");
    g.list1[0] = &g.s1.base;
    Assert(g.q.PushList(1, g.list1));
    Assert(g.q.Poll());
    Assert(g.r.sent == 1 && g.r.reports[0][0] == 0x02);
    g.q.Cancel();                                           // the owner also calls HidKeyboard::ReleaseAll
    g.r.hid.ReleaseAll();
    Assert(g.r.reports[g.r.sent - 1][0] == 0);
}

Fact("ScriptQueue SwitchActivity Waits For The Switch's Ops")
{
    QRig g;
    g.s1 = g.r.Op("local ok = switchActivity('Watch') log(tostring(ok)) hid.key('b', 'down') delay(10) hid.key('c', 'down')");
    g.list1[0] = &g.s1.base;
    g.list1[1] = &g.key_a.base;
    Assert(g.q.PushList(2, g.list1));
    Assert(g.q.Poll());                                     // the script switches and steps aside
    Assert(g.q.Parked() && g.r.host.Running());
    g.Drain();
    // x (the switch's op) first, then the rest of the script (b, c), then the list's next op (a)
    Assert(g.r.sent == 4);
    Assert(g.Added(0) == 0x1b && g.Added(1) == 0x05 && g.Added(2) == 0x06 && g.Added(3) == 0x04);
    Assert(strcmp(g.r.log, "true") == 0);
    Assert(g.q.Errors() == 0 && !g.q.Parked() && g.q.ParamsInUse() == 0);
}

Fact("ScriptQueue Two Switches In A Script Keep Their Order")
{
    QRig g;
    g.s1 = g.r.Op("switchActivity('Watch') switchActivity('Listen') hid.key('b', 'down')");
    g.list1[0] = &g.s1.base;
    Assert(g.q.PushList(1, g.list1));
    g.Drain();
    Assert(g.r.sent == 3);
    Assert(g.Added(0) == 0x1b && g.Added(1) == 0x1c && g.Added(2) == 0x05);
    Assert(g.q.Errors() == 0);
}

Fact("ScriptQueue A Switch With Nothing To Run Does Not Wait")
{
    QRig g;
    g.s1 = g.r.Op("local ok = switchActivity(nil) log(tostring(ok)) hid.key('b', 'down')");
    g.list1[0] = &g.s1.base;
    Assert(g.q.PushList(1, g.list1));
    Assert(g.q.Poll());                                     // ran to the end without parking
    Assert(!g.q.Parked() && !g.r.host.Running());
    Assert(strcmp(g.r.log, "true") == 0 && g.r.sent == 1);
}

Fact("ScriptQueue Time Spent Waiting For A Switch Is Not The Script's")
{
    QRig g;
    g.wait.duration = 20000;                                // each switch's ops take 20 s
    g.watch[0] = &g.wait.base;
    // 40 s waiting for switches plus 25 s of its own: over SCRIPT_WALL_MS in all, under it alone
    g.s1 = g.r.Op("switchActivity('Watch') switchActivity('Watch') delay(25000) log('done')");
    g.list1[0] = &g.s1.base;
    Assert(g.q.PushList(1, g.list1));
    g.Drain();
    Assert(g.r.Logged("done") && !g.r.Logged("timed out"));
    Assert(g.q.Errors() == 0);
}

Fact("ScriptQueue The Script's Own Time Still Times Out After A Switch")
{
    QRig g;
    g.wait.duration = 20000;
    g.watch[0] = &g.wait.base;
    g.s1 = g.r.Op("switchActivity('Watch') delay(20000) delay(20000) log('never')");
    g.list1[0] = &g.s1.base;
    Assert(g.q.PushList(1, g.list1));
    g.Drain();
    Assert(g.r.Logged("timed out") && !g.r.Logged("never"));
    Assert(g.q.Errors() == 1);
}

Fact("ScriptQueue A Second Script Cannot Start While One Runs")
{
    QRig g;
    g.s2 = g.r.Op("log('nested')");
    g.listen[0] = &g.s2.base;                               // the switch to Listen runs a script
    g.s1 = g.r.Op("switchActivity('Listen') log('after')");
    g.list1[0] = &g.s1.base;
    Assert(g.q.PushList(1, g.list1));
    g.Drain();
    // The nested script ends in an error; the parked one carries on
    Assert(g.q.Errors() == 1 && strcmp(g.q.LastError(), "script: another script is running") == 0);
    Assert(g.r.Logged("after") && !g.r.Logged("nested"));
    Assert(!g.r.host.Running() && g.q.ParamsInUse() == 0);
}

Fact("ScriptQueue Takeover Cancels A Script Waiting For A Switch")
{
    QRig g;
    g.wait.duration = 20000;
    g.watch[0] = &g.wait.base;
    g.s1 = g.r.Op("switchActivity('Watch') log('after')");
    g.list1[0] = &g.s1.base;
    g.list1[1] = &g.key_a.base;
    Assert(g.q.PushList(2, g.list1, false, "p"));
    for (int i = 0; i < 10; i++) {
        Assert(g.q.Poll());
        g.r.now++;
    }
    Assert(g.q.Parked() && g.r.host.Running());             // the switch's delay holds the queue
    g.q.Cancel();
    Assert(!g.r.host.Running() && !g.q.Parked());
    Assert(!g.q.Poll());
    Assert(!g.r.Logged("after") && g.r.sent == 0);
    Assert(g.q.ParamsInUse() == 0);
}

Fact("ScriptQueue A Refused Switch Comes Back To The Script")
{
    QRig g;
    g.r.exec.switch_activity = [](int, void *) -> const char * { return "activity switch refused: op queue full"; };
    g.s1 = g.r.Op("local ok, why = switchActivity('Watch') log(tostring(ok), why)");
    g.list1[0] = &g.s1.base;
    Assert(g.q.PushList(1, g.list1));
    g.Drain();
    Assert(g.r.Logged("nil\tactivity switch refused"));
}
