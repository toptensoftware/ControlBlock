// The op registry and the built-in executors (delay, hidKey): start / poll /
// cancel with a fake clock and a recording HID sink. The registry has one entry
// per op code; the ones still to be built have a name and no executor.

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "op_registry.h"
#include "hid_keyboard.h"

using namespace SimpleLib;

namespace
{
    struct Rig
    {
        uint32_t     now = 5000;
        HidKeyboard  hid;
        uint8_t      reports[16][8];
        int          sent = 0;
        ExecCtx      exec = {};
        OpTask       task = {};
        uint32_t     started[16]; int n_started = 0;

        Rig(bool with_hid = true)
        {
            HidSink sink = {};
            sink.ctx  = this;
            sink.send = [](const uint8_t *rep, void *c) {
                Rig *r = (Rig *)c;
                if (r->sent < 16)
                    memcpy(r->reports[r->sent++], rep, 8);
            };
            hid.Init(sink);
            exec.ctx     = this;
            exec.now_ms  = [](void *c) { return ((Rig *)c)->now; };
            exec.hid     = with_hid ? &hid : nullptr;
            exec.started = [](const layoutdata::op *op, void *c) {
                Rig *r = (Rig *)c;
                if (r->n_started < 16)
                    r->started[r->n_started++] = op->op;
            };
            task.param = "";
        }
        bool LastIs(uint8_t mods, uint8_t key) const { return sent && reports[sent - 1][0] == mods && reports[sent - 1][2] == key; }
    };

    layoutdata::delayOp Delay(uint32_t ms)
    {
        layoutdata::delayOp d = {};
        d.base.op  = OP_DELAY;
        d.duration = ms;
        return d;
    }

    layoutdata::hidKeyOp Hid(uint8_t mode, uint8_t mods, uint8_t key)
    {
        layoutdata::hidKeyOp h = {};
        h.base.op   = OP_HID_KEY;
        h.mode      = mode;
        h.modifiers = mods;
        h.key       = key;
        return h;
    }
}

Fact("OpRegistry Has An Entry For Every Op Code")
{
    const char *names[] = { "delay", "switchActivity", "hidKey", "httpGet", "httpPost", "udpPacket", "sendWol", "script" };
    for (uint32_t code = 1; code <= 8; code++) {
        const OpDesc *d = OpFind(code);
        Assert(d != nullptr);
        Assert(d->code == code);
        Assert(strcmp(d->name, names[code - 1]) == 0);
    }
    Assert(OpFind(0) == nullptr);
    Assert(OpFind(9) == nullptr);
}

Fact("OpRegistry Op Without An Executor Is Done At Once But Still Reported Started")
{
    Rig r;
    layoutdata::op script = {};
    script.op = OP_SCRIPT;
    Assert(OpStart(r.exec, &script, r.task) == OpStatus::Done);
    Assert(r.n_started == 1 && r.started[0] == OP_SCRIPT);

    layoutdata::op unknown = {};
    unknown.op = 77;
    Assert(OpStart(r.exec, &unknown, r.task) == OpStatus::Done);
    Assert(OpPoll(r.exec, &unknown, r.task) == OpStatus::Done);
    OpCancel(r.exec, &unknown, r.task);                 // harmless
}

Fact("OpRegistry SwitchActivity Asks The Host And Maps The None Index")
{
    Rig r;
    static struct { int index; const char *fail; } seen;
    seen = { 99, nullptr };
    r.exec.switch_activity = [](int index, void *) { seen.index = index; return seen.fail; };

    layoutdata::switchActivityOp s = {};
    s.base.op = OP_SWITCH_ACTIVITY;
    s.index   = 2;
    Assert(OpStart(r.exec, &s.base, r.task) == OpStatus::Done);
    Assert(seen.index == 2);

    s.index = LAYOUT_ACTIVITY_NONE;
    Assert(OpStart(r.exec, &s.base, r.task) == OpStatus::Done);
    Assert(seen.index == -1);

    seen.fail = "queue full";
    Assert(OpStart(r.exec, &s.base, r.task) == OpStatus::Error);
    Assert(strcmp(r.task.error, "queue full") == 0);
}

Fact("OpRegistry SwitchActivity Without A Host Is Done At Once")
{
    Rig r;
    layoutdata::switchActivityOp s = {};
    s.base.op = OP_SWITCH_ACTIVITY;
    Assert(OpStart(r.exec, &s.base, r.task) == OpStatus::Done);
    Assert(r.n_started == 1 && r.started[0] == OP_SWITCH_ACTIVITY);
}

Fact("OpRegistry Delay Is Pending Until Its Time Has Passed")
{
    Rig r;
    layoutdata::delayOp d = Delay(100);
    Assert(OpStart(r.exec, &d.base, r.task) == OpStatus::Pending);
    Assert(r.n_started == 1 && r.started[0] == OP_DELAY);
    r.now += 99;
    Assert(OpPoll(r.exec, &d.base, r.task) == OpStatus::Pending);
    r.now += 1;
    Assert(OpPoll(r.exec, &d.base, r.task) == OpStatus::Done);
}

Fact("OpRegistry Delay Survives The Clock Wrapping")
{
    Rig r;
    r.now = 0xFFFFFFF0u;
    layoutdata::delayOp d = Delay(100);
    OpStart(r.exec, &d.base, r.task);
    r.now += 99;                                        // wraps past zero, 1 ms short
    Assert(OpPoll(r.exec, &d.base, r.task) == OpStatus::Pending);
    r.now += 1;
    Assert(OpPoll(r.exec, &d.base, r.task) == OpStatus::Done);
}

Fact("OpRegistry Zero Delay Is Done At Once")
{
    Rig r;
    layoutdata::delayOp d = Delay(0);
    Assert(OpStart(r.exec, &d.base, r.task) == OpStatus::Done);
}

Fact("OpRegistry Hid Tap Is Pending Through The Gap And The Settle")
{
    Rig r;
    layoutdata::hidKeyOp h = Hid(HID_MODE_TAP, 0x01, 0x06);        // ctrl+c
    Assert(OpStart(r.exec, &h.base, r.task) == OpStatus::Pending);
    Assert(r.sent == 1 && r.LastIs(0x01, 0x06));                // down

    r.now += HID_TAP_GAP_MS - 1;
    Assert(OpPoll(r.exec, &h.base, r.task) == OpStatus::Pending);
    Assert(r.sent == 1);
    r.now += 1;
    Assert(OpPoll(r.exec, &h.base, r.task) == OpStatus::Pending);   // up sent, settling
    Assert(r.sent == 2 && r.LastIs(0, 0));
    r.now += HID_TAP_SETTLE_MS - 1;
    Assert(OpPoll(r.exec, &h.base, r.task) == OpStatus::Pending);
    r.now += 1;
    Assert(OpPoll(r.exec, &h.base, r.task) == OpStatus::Done);
    Assert(r.sent == 2);
}

Fact("OpRegistry Hid Down Up And Hold Are Done At Once")
{
    Rig r;
    layoutdata::hidKeyOp down = Hid(HID_MODE_DOWN, 0x02, 0x04);
    Assert(OpStart(r.exec, &down.base, r.task) == OpStatus::Done);
    Assert(r.sent == 1 && r.LastIs(0x02, 0x04));

    layoutdata::hidKeyOp up = Hid(HID_MODE_UP, 0x02, 0x04);
    Assert(OpStart(r.exec, &up.base, r.task) == OpStatus::Done);
    Assert(r.sent == 2 && r.LastIs(0, 0));

    // hold: down for a press trigger, up for a release one (the task says which)
    layoutdata::hidKeyOp hold = Hid(HID_MODE_HOLD, 0, 0x05);
    r.task.releasing = false;
    Assert(OpStart(r.exec, &hold.base, r.task) == OpStatus::Done);
    Assert(r.sent == 3 && r.LastIs(0, 0x05));
    r.task.releasing = true;
    Assert(OpStart(r.exec, &hold.base, r.task) == OpStatus::Done);
    Assert(r.sent == 4 && r.LastIs(0, 0));
}

Fact("OpRegistry Hid Cancel Lets Go Of A Tap In Flight")
{
    Rig r;
    layoutdata::hidKeyOp h = Hid(HID_MODE_TAP, 0, 0x06);
    OpStart(r.exec, &h.base, r.task);
    Assert(r.sent == 1);
    OpCancel(r.exec, &h.base, r.task);
    Assert(r.sent == 2 && r.LastIs(0, 0));
    r.now += 1000;
    OpPoll(r.exec, &h.base, r.task);                    // the tap's own up is not sent again
    Assert(r.sent == 2);
}

Fact("OpRegistry Hid Without A Keyboard Is A No-op")
{
    Rig r(false);
    layoutdata::hidKeyOp h = Hid(HID_MODE_TAP, 0, 0x06);
    Assert(OpStart(r.exec, &h.base, r.task) == OpStatus::Done);
    OpCancel(r.exec, &h.base, r.task);
    Assert(r.sent == 0);
}

Fact("OpRegistry Host Adapter Runs The Registry Through An OpQueue")
{
    Rig r;
    OpQueue q;
    q.Init(OpRegistryHost(&r.exec));
    layoutdata::delayOp d = Delay(50);
    layoutdata::hidKeyOp h = Hid(HID_MODE_DOWN, 0, 0x04);
    layoutdata::op *ops[2] = { &d.base, &h.base };
    Assert(q.PushList(2, ops));
    Assert(q.Poll());
    Assert(r.n_started == 1 && r.sent == 0);            // the delay holds the hid op
    r.now += 50;
    Assert(q.Poll());
    Assert(r.n_started == 2 && r.sent == 1);
    Assert(!q.Poll());
}
