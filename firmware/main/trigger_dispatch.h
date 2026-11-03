#pragma once

// Trigger dispatch (docs/standalone.md "Triggers and bindings"): a trigger is
// (control, event, param) strings. Every binding with exactly that control and
// event, in Home's list and then the selected activity's, has its op list
// pushed onto the op queue in config order. Unmatched triggers are ignored.
// Platform independent; the caller serialises calls.
//
// `param` is accepted so every source has one entry point, but it isn't used
// yet: only script ops take it, and the queue doesn't carry it until they exist.

#include <stdint.h>
#include "config_view.h"
#include "op_queue.h"

class TriggerDispatcher
{
public:
    void Init(const ConfigView *cfg, OpQueue *queue);

    // The selected activity (its bindings join Home's), or -1 for none.
    // False, and unchanged, for an index outside the config.
    bool SetActivity(int index);

    // True if some binding matches: the key gesture asks before deferring `tap`
    bool HasBinding(const char *control, const char *event) const;

    // Push the matching op lists; returns how many bindings matched
    int Trigger(const char *control, const char *event, const char *param);

private:
    // `fn(binding)` for every match in one list; `fn` returns false to stop
    template <class F> void ForMatches(const ConfigView::Array &list, const char *control, const char *event, F fn) const;
    template <class F> void ForAllMatches(const char *control, const char *event, F fn) const;

    const ConfigView *m_cfg      = nullptr;
    OpQueue          *m_queue    = nullptr;
    ConfigView::Array m_home     = {};
    ConfigView::Array m_activity = {};      // items null: none selected
    ConfigView::Array m_activities = {};
};
