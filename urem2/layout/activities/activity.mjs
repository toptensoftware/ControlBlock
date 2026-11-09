// The shape every urem2 activity shares: its meta (devices, hook, display name), its pages as slot maps, and its
// pad bindings followed by the volume bindings (urem bound the volume globally; ControlBlock binds per activity).

import { activityMeta } from "../devices.mjs";
import { grid } from "../keys.mjs";
import { padBindings, volumeBindings, padPage } from "../pad.mjs";

// The volume's controls: every activity gets them from volumeBindings, so an activity's pad spec can't use them
const VOLUME_CONTROLS = ["key7", "enc1"];

// pages: [{ slot: key }, ...] (at least one); pad: padBindings spec for key1..key6; padPage: put the on-screen pad
// page (pad.mjs padPage) first
export function defineActivity(ctx, name, { pages, pad = {}, padPage: withPadPage = false })
{
    const meta = activityMeta(ctx, name);
    if (!Array.isArray(pages) || pages.length === 0)
        throw new Error(`activity ${name} needs at least one page`);
    for (const control of VOLUME_CONTROLS)
        if (Object.hasOwn(pad, control))
            throw new Error(`activity ${name}: ${control} is the volume's (volumeBindings)`);
    return {
        ...meta,
        pages: [...(withPadPage ? [padPage(ctx, pad)] : []), ...pages].map(slots => ({ keys: grid(slots) })),
        bindings: [...padBindings(pad), ...volumeBindings(ctx)],
    };
}
