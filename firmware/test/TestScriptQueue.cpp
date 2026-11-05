// The `script` op in the op queue: the active slot is the script's while it waits (the queue
// is held), a script error ends only that op, takeover cancels a waiting script, and ops a
// script queues (switchActivity) run after it.

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
        cfgdata::scriptOp  s1, s2;
        cfgdata::hidKeyOp  key_a, key_x;
        cfgdata::op       *list1[2];
        cfgdata::op       *list2[1];
        int        pushed_switches = 0;

        QRig()
        {
            r.exec.queue = &q;
            q.Init(OpRegistryHost(&r.exec));
            key_a = Key(0x04);
            key_x = Key(0x1b);
        }

        static cfgdata::hidKeyOp Key(uint8_t k)
        {
            cfgdata::hidKeyOp o = {};
            o.base.op = OP_HID_KEY; o.mode = HID_MODE_DOWN; o.key = k;
            return o;
        }

        // Poll until idle, the clock moving 1 ms each time
        void Drain(int max = 100000)
        {
            for (int i = 0; i < max && q.Poll(); i++)
                r.now++;
        }
    };
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

Fact("ScriptQueue Ops A Script Queues Run After It")
{
    QRig g;
    // The fake switchActivity pushes one more op at the queue tail, as the real one does
    static QRig *s_g;
    s_g = &g;
    g.r.exec.switch_activity = [](int, void *) -> const char * {
        s_g->list2[0] = &s_g->key_x.base;
        return s_g->q.PushList(1, s_g->list2) ? nullptr : "full";
    };

    g.s1 = g.r.Op("switchActivity('Watch') hid.key('b', 'down') delay(10) hid.key('c', 'down')");
    g.list1[0] = &g.s1.base;
    Assert(g.q.PushList(1, g.list1));
    g.Drain();
    // b and c (the script) first, then x (queued by the switch)
    Assert(g.r.sent == 3);
    Assert(g.r.reports[0][2] == 0x05);
    Assert(g.r.reports[1][3] == 0x06);                      // c joins b
    Assert(g.r.reports[2][4] == 0x1b);                      // x last
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
