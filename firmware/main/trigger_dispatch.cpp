#include <string.h>
#include "trigger_dispatch.h"

void TriggerDispatcher::Init(const layoutdata::layoutRoot *root, OpQueue *queue)
{
    m_root     = root;
    m_queue    = queue;
    m_home     = root ? Bindings{ root->homePage.bindings_count, root->homePage.bindings } : Bindings{};
    m_activity = {};
}

bool TriggerDispatcher::SetActivity(int index)
{
    if (index < 0) {
        if (index != -1)
            return false;
        m_activity = {};
        return true;
    }
    if (!m_root || (uint32_t)index >= m_root->activities_count)
        return false;
    const layoutdata::activity &a = m_root->activities[index];
    m_activity = Bindings{ a.bindings_count, a.bindings };
    return true;
}

// Exact string match on both fields; no wildcards on the device
template <class F>
void TriggerDispatcher::ForMatches(const Bindings &list, const char *control, const char *event, F fn) const
{
    for (uint32_t i = 0; i < list.count; i++) {
        const layoutdata::binding &b = list.items[i];
        if (strcmp(b.control, control) == 0 && strcmp(b.event, event) == 0)
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
    ForAllMatches(control, event, [&](const layoutdata::binding &) { return !(found = true); });
    return found;
}

int TriggerDispatcher::Trigger(const char *control, const char *event, const char *param)
{
    int matched = 0;
    bool release = event && strcmp(event, "release") == 0;
    ForAllMatches(control, event, [&](const layoutdata::binding &b) {
        matched++;
        m_queue->PushList(b.ops_count, b.ops, release, param);
        return true;
    });
    return matched;
}
