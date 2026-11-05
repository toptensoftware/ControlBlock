#pragma once

// The op lists a switch of the selected activity queues (docs/standalone.md
// "Devices and activity switching"). Pure: the config and the devices-on mask in,
// the lists in order and the new mask out. Nothing is queued or committed here;
// the executor pushes the lists (OpQueue::PushLists, all or nothing) and keeps
// the new mask only if that worked.
//
// Order: old.willDeactivate, new.willActivate, turnOn of each device the new
// activity uses that is not on (the new `devices` order), turnOff of each device
// that is on and that the old activity used and the new does not (the old
// `devices` order), new.didActivate, old.didDeactivate. Empty lists are left out.
//
// Switching to "none" (-1) turns off every device that is on: the old
// activity's, then any other the mask holds (index order), e.g. left on across a
// hosted session. Switching from none turns on what the new activity needs and
// leaves other devices alone.

#include <stdint.h>
#include "config_blob.h"        // CONFIG_MAX_DEVICES
#include "config_types.h"
#include "op_queue.h"           // OpListRef

// Four hooks and one list per device (a device is turned on or off, not both)
#define ACTIVITY_SWITCH_MAX_LISTS   (4 + CONFIG_MAX_DEVICES)

struct ActivitySwitch
{
    OpListRef lists[ACTIVITY_SWITCH_MAX_LISTS];
    int       count = 0;
    uint32_t  devices_on = 0;       // the mask once the lists have been queued
};

// `from` and `to` are activity indices or -1 for none. False for an index
// outside the config (`out` is then empty). from == to is success with no lists.
bool ActivitySwitchPlan(const cfgdata::configRoot *root, int from, int to, uint32_t devices_on, ActivitySwitch &out);
