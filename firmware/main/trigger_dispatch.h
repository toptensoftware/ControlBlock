#pragma once

// Trigger dispatch (docs/standalone.md "Triggers and bindings"): a trigger is
// (control, event, param) strings. Every binding with exactly that control and
// event, in Home's list and then the selected activity's, has its op list
// pushed onto the op queue in layout order. Unmatched triggers are ignored.
// Platform independent; the caller serialises calls.
//
// `param` (null: none) goes into the queue's param pool with each pushed list
// and reaches every op of it as OpTask::param. A trigger whose param finds the
// pool full is dropped and counted (OpQueue::ParamDropped), like a full queue.

#include <stdint.h>
#include "layout_types.h"
#include "op_queue.h"

class TriggerDispatcher
{
public:
    // `root`: a validated, relocated layout (null: no layout, nothing matches)
    void Init(const layoutdata::layoutRoot *root, OpQueue *queue);

    // The selected activity (its bindings join Home's), or -1 for none.
    // False, and unchanged, for an index outside the layout.
    bool SetActivity(int index);

    // True if some binding matches: the key gesture asks before deferring `tap`
    bool HasBinding(const char *control, const char *event) const;

    // Push the matching op lists; returns how many bindings matched (a list
    // that was then dropped still counts)
    int Trigger(const char *control, const char *event, const char *param);

private:
    struct Bindings
    {
        uint32_t                count;
        const layoutdata::binding *items;      // null: none
    };

    // `fn(binding)` for every match in one list; `fn` returns false to stop
    template <class F> void ForMatches(const Bindings &list, const char *control, const char *event, F fn) const;
    template <class F> void ForAllMatches(const char *control, const char *event, F fn) const;

    const layoutdata::layoutRoot *m_root = nullptr;
    OpQueue          *m_queue    = nullptr;
    Bindings          m_home     = {};
    Bindings          m_activity = {};      // items null: none selected
};
