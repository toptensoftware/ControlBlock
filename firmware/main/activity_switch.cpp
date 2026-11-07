#include "activity_switch.h"

namespace
{
    // Append list `ops` of `count` ops (skipped if empty)
    void Add(ActivitySwitch &out, layoutdata::op *const *ops, uint32_t count)
    {
        if (count == 0 || !ops)
            return;
        OpListRef &r = out.lists[out.count++];
        r = {};
        r.ops   = ops;
        r.count = count;
    }

    // Device `d` of the activity's list, or -1 if the entry is outside the layout
    int DeviceAt(const layoutdata::layoutRoot *root, const layoutdata::activity *a, uint32_t i)
    {
        int32_t d = a->devices[i];
        return d >= 0 && (uint32_t)d < root->devices_count && d < LAYOUT_MAX_DEVICES ? d : -1;
    }

    bool Uses(const layoutdata::layoutRoot *root, const layoutdata::activity *a, int device)
    {
        for (uint32_t i = 0; a && i < a->devices_count; i++)
            if (DeviceAt(root, a, i) == device)
                return true;
        return false;
    }

    void TurnOff(const layoutdata::layoutRoot *root, ActivitySwitch &out, int device)
    {
        const layoutdata::device &d = root->devices[device];
        Add(out, d.turnOff, d.turnOff_count);
        out.devices_on &= ~(1u << device);
    }
}

bool ActivitySwitchPlan(const layoutdata::layoutRoot *root, int from, int to, uint32_t devices_on, ActivitySwitch &out)
{
    out = {};
    out.devices_on = devices_on;
    if (!root || from < -1 || to < -1 || from >= (int)root->activities_count || to >= (int)root->activities_count)
        return false;
    if (from == to)
        return true;

    const layoutdata::activity *o = from >= 0 ? &root->activities[from] : nullptr;
    const layoutdata::activity *n = to >= 0 ? &root->activities[to] : nullptr;

    if (o)
        Add(out, o->willDeactivate, o->willDeactivate_count);
    if (n)
        Add(out, n->willActivate, n->willActivate_count);

    for (uint32_t i = 0; n && i < n->devices_count; i++) {
        int d = DeviceAt(root, n, i);
        if (d < 0 || (out.devices_on & (1u << d)))
            continue;
        Add(out, root->devices[d].turnOn, root->devices[d].turnOn_count);
        out.devices_on |= 1u << d;
    }

    for (uint32_t i = 0; o && i < o->devices_count; i++) {
        int d = DeviceAt(root, o, i);
        if (d >= 0 && (out.devices_on & (1u << d)) && !Uses(root, n, d))
            TurnOff(root, out, d);
    }
    if (!n) {
        // Everything else the mask holds goes off too
        for (int d = 0; d < LAYOUT_MAX_DEVICES && d < (int)root->devices_count; d++)
            if (out.devices_on & (1u << d))
                TurnOff(root, out, d);
    }

    if (n)
        Add(out, n->didActivate, n->didActivate_count);
    if (o)
        Add(out, o->didDeactivate, o->didDeactivate_count);
    return true;
}
