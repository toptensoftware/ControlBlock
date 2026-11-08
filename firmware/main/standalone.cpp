#include <stdio.h>
#include "standalone.h"
#include "touch_router.h"       // TOUCH_KEY_*

bool Standalone::GestureHasBinding(const char *control, const char *event, void *ctx)
{
    return ((Standalone *)ctx)->m_dispatch.HasBinding(control, event);
}

void Standalone::GestureTrigger(const char *control, const char *event, void *ctx)
{
    ((Standalone *)ctx)->m_dispatch.Trigger(control, event, "");
}

void Standalone::Init(const StandaloneHost &host, const layoutdata::layoutRoot *root)
{
    m_host   = host;
    m_root   = root;
    m_hosted = false;
    m_devices_on = 0;
    m_hid.Init(host.hid);
    Arm();
}

// Everything held or queued is dropped (the active op is told to stop and every
// key let go); the layout's long-press time is read again
void Standalone::Arm()
{
    m_queue.Cancel();                   // a waiting script is cancelled here
    m_hid.ReleaseAll();
    m_script.Close();                   // its compiled scripts point into the old blob
    m_activity = -1;
    if (!m_root)
        return;

    // The registry's executors (op_registry.cpp); every op that starts is then
    // passed on to the host's `run`
    m_exec         = {};
    m_exec.ctx     = this;
    m_exec.now_ms  = [](void *c) { Standalone *s = (Standalone *)c; return s->m_host.now_ms(s->m_host.ctx); };
    m_exec.started = [](const layoutdata::op *op, void *c) {
        Standalone *s = (Standalone *)c;
        if (s->m_host.run)
            s->m_host.run(op, op->op, s->m_host.ctx);
    };
    m_exec.hid     = &m_hid;
    m_exec.net     = m_host.net;
    m_exec.queue   = &m_queue;
    m_exec.root    = m_root;
    m_script.Init(m_host.script, m_root);
    m_exec.script  = m_host.script.realloc ? &m_script : nullptr;
    m_exec.switch_activity = [](int index, void *c) { return ((Standalone *)c)->SwitchActivity(index); };
    m_queue.Init(OpRegistryHost(&m_exec));
    m_dispatch.Init(m_root, &m_queue);

    GestureHost gh = {};
    gh.ctx         = this;
    gh.has_binding = GestureHasBinding;
    gh.trigger     = GestureTrigger;
    m_gestures.Init(gh, m_root->longPressMs);
}

void Standalone::SetHosted(bool hosted)
{
    if (hosted == m_hosted) {
        m_hid.ReleaseAll();         // e.g. an unplug while standalone: the PC's keyboard state is gone
        return;
    }
    m_hosted = hosted;
    Arm();      // hosted: nothing stays held or queued; released: fresh from the blob
}

bool Standalone::SelectActivity(int index)
{
    if (!Active() || !m_dispatch.SetActivity(index))
        return false;
    m_activity = index;
    return true;
}

const char *Standalone::SwitchActivity(int to)
{
    if (!Active())
        return "not standalone";
    if (to == m_activity) {                     // chosen again: no ops, just show its first page
        if (to >= 0 && m_host.activity && m_host.activity->glide)
            m_host.activity->glide(m_host.activity->ctx);
        return nullptr;
    }
    ActivitySwitch plan;
    if (!ActivitySwitchPlan(m_root, m_activity, to, m_devices_on, plan))
        return "activity switch: bad index";
    if (m_script.Running() && ActivitySwitchRunsScript(plan)) {
        // The script waits for these lists, and there is one Lua state: refused, nothing changes
        char line[96];
        snprintf(line, sizeof(line), "script: switchActivity refused: the switch to %s runs a script",
                 to >= 0 ? m_root->activities[to].name : "none");
        m_script.Log(line);
        return "switchActivity: the switch runs a script";
    }
    if (!m_queue.PushListsNext(plan.lists, plan.count))     // before the rest of the switching list
        return "activity switch refused: op queue full";     // nothing changed (the queue counts the drop)

    m_devices_on = plan.devices_on;
    SelectActivity(to);                         // the new bindings (the index was checked by the plan)
    m_gestures.Reset();                         // a key held under the old bindings would never find its release
    m_hid.ReleaseAll();
    if (m_host.activity && m_host.activity->apply)
        m_host.activity->apply(to, m_host.activity->ctx);
    return nullptr;
}

// Home's keys carry their own long-press time; an activity's pages aren't
// selectable yet (0 = the layout's default)
uint32_t Standalone::KeyLongPressMs(int page, int key) const
{
    if (page != 0 || key < 0 || key >= COMP_KEYS)
        return 0;
    return m_root->homePage.keys[key].longPressMs;
}

void Standalone::Key(int page, int key, int action, uint32_t time_ms)
{
    if (!Active())
        return;
    char control[GESTURE_NAME_MAX];
    TouchControlName(page, key, control);
    switch (action) {
    case TOUCH_KEY_PRESS:   m_gestures.Press(control, time_ms, KeyLongPressMs(page, key)); break;
    case TOUCH_KEY_RELEASE: m_gestures.Release(control, time_ms); break;
    case TOUCH_KEY_CANCEL:  m_gestures.Cancel(control, time_ms); break;
    }
}

void Standalone::Tick()
{
    if (Active())
        m_gestures.Tick(m_host.now_ms(m_host.ctx));
}

bool Standalone::Poll()
{
    if (!Active())
        return false;
    return m_queue.Poll();      // a tap's up is sent by its hidKey executor while it is the active op
}
