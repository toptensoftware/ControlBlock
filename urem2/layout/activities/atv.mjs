// Apple TV (urem activity-atv.js): keys through the Apple TV daemon. Not ported: the clock (5).

import { defineActivity } from "./activity.mjs";
import { transportKeys, sevenChKey, muteKey } from "../keys.mjs";
import { padArrows } from "../pad.mjs";

export function atv(ctx)
{
    const k = name => ctx.ops.atvKey(name);
    // urem fired these on release (so a long press doesn't also tap): tap, and long for the hold
    const tapHold = name => ({ tap: k(name), long: ctx.ops.atvKey(name, "hold") });

    return defineActivity(ctx, "atv", {
        pages: [{
            ...transportKeys({ previous: k("previous"), next: k("next"), rewind: k("left"), forward: k("right"),
                               play: k("play"), pause: k("pause"), stop: k("menu") }, { repeat: 100 }),
            10: sevenChKey(ctx),
            14: muteKey(ctx),
        }],
        pad: { key1: tapHold("menu"), key3: tapHold("select"), ...padArrows(k) },
    });
}
