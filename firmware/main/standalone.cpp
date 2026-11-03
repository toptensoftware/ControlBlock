#include "standalone.h"
#include "touch_router.h"       // TOUCH_KEY_*

using namespace config_layout;

bool Standalone::GestureHasBinding(const char *control, const char *event, void *ctx)
{
    return ((Standalone *)ctx)->m_dispatch.HasBinding(control, event);
}

void Standalone::GestureTrigger(const char *control, const char *event, void *ctx)
{
    ((Standalone *)ctx)->m_dispatch.Trigger(control, event, "");
}

void Standalone::Init(const StandaloneHost &host, const uint8_t *blob, uintptr_t adjust)
{
    m_host   = host;
    m_blob   = blob;
    m_adjust = adjust;
    m_hosted = false;
    m_hid.Init(host.hid);
    Arm();
}

// The executors that live here run first; every op is then passed on to the
// host's `run` (the device layer's, for the ones not built yet)
void Standalone::RunOp(const ConfigView &cfg, const uint8_t *op, uint32_t code, void *ctx)
{
    Standalone *s = (Standalone *)ctx;
    if (code == OP_HID_KEY) {
        uint32_t hold = s->m_hid.Run(s->m_host.now_ms(s->m_host.ctx), cfg.U8(op + HIDOP_MODE),
                                     cfg.U8(op + HIDOP_MODS), cfg.U8(op + HIDOP_KEY), s->m_queue.Releasing());
        if (hold)
            s->m_queue.Hold(hold);
    }
    if (s->m_host.run)
        s->m_host.run(cfg, op, code, s->m_host.ctx);
}

// Everything held or queued is dropped (and every key let go); the config's
// long-press time is read again
void Standalone::Arm()
{
    m_hid.ReleaseAll();
    m_cfg = m_blob ? ConfigView(m_blob, m_adjust) : ConfigView();
    if (!m_cfg.Valid())
        return;

    OpHost oh = {};
    oh.ctx    = this;
    oh.now_ms = [](void *c) { Standalone *s = (Standalone *)c; return s->m_host.now_ms(s->m_host.ctx); };
    oh.run    = RunOp;
    m_queue.Init(&m_cfg, oh);
    m_dispatch.Init(&m_cfg, &m_queue);

    GestureHost gh = {};
    gh.ctx         = this;
    gh.has_binding = GestureHasBinding;
    gh.trigger     = GestureTrigger;
    m_gestures.Init(gh, m_cfg.U32(m_cfg.Root() + ROOT_LONGPRESS));
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
    return Active() && m_dispatch.SetActivity(index);
}

// Home's keys carry their own long-press time; an activity's pages aren't
// selectable yet (0 = the config's default)
uint32_t Standalone::KeyLongPressMs(int page, int key) const
{
    if (page != 0)
        return 0;
    return m_cfg.U32(m_cfg.Root() + ROOT_HOME_KEYS + key * KEY_SIZE + KEY_LONGPRESS);
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
    m_hid.Poll(m_host.now_ms(m_host.ctx));      // before the queue: a tap's up is due before the next op
    return m_queue.Poll();
}
