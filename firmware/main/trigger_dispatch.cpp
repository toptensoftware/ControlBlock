#include <string.h>
#include "trigger_dispatch.h"

using namespace config_layout;

void TriggerDispatcher::Init(const ConfigView *cfg, OpQueue *queue)
{
    m_cfg        = cfg;
    m_queue      = queue;
    m_home       = cfg->GetArray(cfg->Root() + ROOT_HOME_BINDINGS);
    m_activities = cfg->GetArray(cfg->Root() + ROOT_ACTIVITIES);
    m_activity   = {};
}

bool TriggerDispatcher::SetActivity(int index)
{
    if (index < 0) {
        if (index != -1)
            return false;
        m_activity = {};
        return true;
    }
    if ((uint32_t)index >= m_activities.count)
        return false;
    m_activity = m_cfg->GetArray(m_activities.items + (uint32_t)index * ACTIVITY_SIZE + ACT_BINDINGS);
    return true;
}

// Exact string match on both fields; no wildcards on the device
template <class F>
void TriggerDispatcher::ForMatches(const ConfigView::Array &list, const char *control, const char *event, F fn) const
{
    for (uint32_t i = 0; i < list.count; i++) {
        const uint8_t *b = list.items + i * BINDING_SIZE;
        if (strcmp(m_cfg->Str(b + BIND_CONTROL), control) == 0 && strcmp(m_cfg->Str(b + BIND_EVENT), event) == 0)
            if (!fn(b))
                return;
    }
}

template <class F>
void TriggerDispatcher::ForAllMatches(const char *control, const char *event, F fn) const
{
    if (!control || !event)
        return;
    ForMatches(m_home, control, event, fn);
    ForMatches(m_activity, control, event, fn);
}

bool TriggerDispatcher::HasBinding(const char *control, const char *event) const
{
    bool found = false;
    ForAllMatches(control, event, [&](const uint8_t *) { return !(found = true); });
    return found;
}

int TriggerDispatcher::Trigger(const char *control, const char *event, const char *param)
{
    (void)param;
    int matched = 0;
    bool release = event && strcmp(event, "release") == 0;
    ForAllMatches(control, event, [&](const uint8_t *b) {
        matched++;
        m_queue->PushList(m_cfg->GetArray(b + BIND_OPS), release);
        return true;
    });
    return matched;
}
