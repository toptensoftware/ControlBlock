#include "op_registry.h"
#include "hid_keyboard.h"
#include "exec_net.h"
#include "script_host.h"

// ---- delay: Pending until its time has passed ----

static OpStatus delay_start(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    uint32_t ms = ((const layoutdata::delayOp *)op)->duration;
    if (ms == 0)
        return OpStatus::Done;
    task.deadline = exec.Now() + ms;
    return OpStatus::Pending;
}

static OpStatus delay_poll(ExecCtx &exec, const layoutdata::op *, OpTask &task)
{
    return OpReached(exec.Now(), task.deadline) ? OpStatus::Done : OpStatus::Pending;
}

// ---- hidKey: a tap is Pending through its down-to-up gap and the settle after ----
// (HidKeyboard::Run says how long; its Poll sends the up when due)

static OpStatus hid_start(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    if (!exec.hid)
        return OpStatus::Done;
    const layoutdata::hidKeyOp *k = (const layoutdata::hidKeyOp *)op;
    uint32_t now  = exec.Now();
    uint32_t hold = exec.hid->Run(now, k->mode, k->modifiers, k->key, task.releasing);
    if (!hold)
        return OpStatus::Done;
    task.deadline = now + hold;
    return OpStatus::Pending;
}

static OpStatus hid_poll(ExecCtx &exec, const layoutdata::op *, OpTask &task)
{
    uint32_t now = exec.Now();
    if (exec.hid)
        exec.hid->Poll(now);
    return OpReached(now, task.deadline) ? OpStatus::Done : OpStatus::Pending;
}

// Abandoned mid-tap: nothing may stay down
static void hid_cancel(ExecCtx &exec, const layoutdata::op *, OpTask &)
{
    if (exec.hid)
        exec.hid->ReleaseAll();
}

// ---- switchActivity: the owner does the work (it holds the state); a refusal is an error ----

static OpStatus switch_start(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    if (!exec.switch_activity)
        return OpStatus::Done;
    uint32_t index = ((const layoutdata::switchActivityOp *)op)->index;
    task.error = exec.switch_activity(index == LAYOUT_ACTIVITY_NONE ? -1 : (int)index, exec.ctx);
    return task.error ? OpStatus::Error : OpStatus::Done;
}

// ---- script: the Lua runtime (script_host.cpp) runs it; Pending while the script waits ----

static OpStatus script_start(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    return exec.script ? exec.script->Start(exec, (const layoutdata::scriptOp *)op, task) : OpStatus::Done;
}

static OpStatus script_poll(ExecCtx &exec, const layoutdata::op *, OpTask &task)
{
    return exec.script ? exec.script->Poll(exec, task) : OpStatus::Done;
}

static void script_cancel(ExecCtx &exec, const layoutdata::op *, OpTask &task)
{
    if (exec.script)
        exec.script->Cancel(exec, task);
}

// ---- the table ----

static const OpDesc s_ops[] = {
    { OP_DELAY,           "delay",          delay_start, delay_poll, nullptr },
    { OP_SWITCH_ACTIVITY, "switchActivity", switch_start, nullptr,    nullptr },
    { OP_HID_KEY,         "hidKey",         hid_start,   hid_poll,   hid_cancel },
    { OP_HTTP_GET,        "httpGet",        ExecHttpGet,    nullptr, nullptr },
    { OP_HTTP_POST,       "httpPost",       ExecHttpPost,   nullptr, nullptr },
    { OP_UDP_PACKET,      "udpPacket",      ExecUdpPacket,  nullptr, nullptr },
    { OP_SEND_WOL,        "sendWol",        ExecSendWol,    nullptr, nullptr },
    { OP_SCRIPT,          "script",         script_start, script_poll, script_cancel },
};

const OpDesc *OpFind(uint32_t code)
{
    for (const OpDesc &d : s_ops)
        if (d.code == code)
            return &d;
    return nullptr;
}

OpStatus OpStart(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    const OpDesc *d = OpFind(op->op);
    OpStatus st = d && d->start ? d->start(exec, op, task) : OpStatus::Done;
    if (exec.started)
        exec.started(op, exec.ctx);
    return st;
}

OpStatus OpPoll(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    const OpDesc *d = OpFind(op->op);
    return d && d->poll ? d->poll(exec, op, task) : OpStatus::Done;
}

void OpCancel(ExecCtx &exec, const layoutdata::op *op, OpTask &task)
{
    const OpDesc *d = OpFind(op->op);
    if (d && d->cancel)
        d->cancel(exec, op, task);
}

OpHost OpRegistryHost(ExecCtx *exec)
{
    OpHost h = {};
    h.ctx    = exec;
    h.start  = [](const layoutdata::op *op, OpTask &t, void *c) { return OpStart(*(ExecCtx *)c, op, t); };
    h.poll   = [](const layoutdata::op *op, OpTask &t, void *c) { return OpPoll(*(ExecCtx *)c, op, t); };
    h.cancel = [](const layoutdata::op *op, OpTask &t, void *c) { OpCancel(*(ExecCtx *)c, op, t); };
    return h;
}
