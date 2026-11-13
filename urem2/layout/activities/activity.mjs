// The shape every urem2 activity shares: its meta (devices, hook, display name), its pages as key lists, and its
// pad bindings followed by the volume bindings (urem bound the volume globally; ControlBlock binds per activity).

import { activityMeta } from "../devices.mjs";
import { padBindings, padControls, volumeBindings } from "../pad.mjs";

// The volume's controls: every activity gets them from volumeBindings, so an activity's pad spec can't use them
const VOLUME_CONTROLS = ["key7", "enc1"];
const OPTIONS = ["pages", "pad"];

// pages: [[key, ...], ...] (at least one), each key placed with x, y (and w, h); pad: padBindings spec for
// key1..key6 (its repeats become the activity's `pad` map, padControls)
export function defineActivity(ctx, name, options)
{
    for (const o of Object.keys(options))
        if (!OPTIONS.includes(o))
            throw new Error(`activity ${name}: unknown option ${JSON.stringify(o)}`);
    const { pages, pad = {} } = options;
    const meta = activityMeta(ctx, name);
    if (!Array.isArray(pages) || pages.length === 0)
        throw new Error(`activity ${name} needs at least one page`);
    pages.forEach((keys, i) => {
        if (!Array.isArray(keys))
            throw new Error(`activity ${name}: page ${i} must be an array of keys`);
    });
    for (const control of VOLUME_CONTROLS)
        if (Object.hasOwn(pad, control))
            throw new Error(`activity ${name}: ${control} is the volume's (volumeBindings)`);
    return {
        ...meta,
        pages: pages.map(keys => ({ keys })),
        bindings: [...padBindings(pad), ...volumeBindings(ctx)],
        pad: padControls(pad),
    };
}
