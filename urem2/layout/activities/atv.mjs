// Apple TV (urem activity-atv.js): keys through the Apple TV daemon. Not ported: the clock.

import { defineActivity } from "./activity.mjs";
import { key, sevenChKey, muteKey, volUpKey, volDownKey } from "../keys.mjs";
import { icon, COLORS } from "../look.mjs";
import { ARROW_REPEAT } from "../pad.mjs";

export function atv(ctx)
{
    const k = name => ctx.ops.atvKey(name);
    const hold = name => ctx.ops.atvKey(name, "hold");
    const white = { color: COLORS.white }, orange = { color: COLORS.orange };
    const red = { color: COLORS.red }, green = { color: COLORS.green };
    const repeat = ARROW_REPEAT;

    return defineActivity(ctx, "atv", {
        pages: [
            // ATV Main Page
            [
                // Transport
                { ...key(icon("#previous-track", white), k("previous")), x: 0, y: 0 },
                { ...key(icon("#next-track", white), k("next")), x: 1, y: 0 },
                { ...key(icon("#rewind", white), k("left"), { repeat: 100 }), x: 2, y: 0 },
                { ...key(icon("#forward", white), k("right"), { repeat: 100 }), x: 3, y: 0 },
                { ...key(icon("#play-pause", orange), k("play_pause")), x: 0, y: 2 },

                // Volume
                { ...volUpKey(ctx), x: 4, y: 0 },
                { ...volDownKey(ctx), x: 4, y: 1 },
                { ...muteKey(ctx), x: 4, y: 2 },
                { ...sevenChKey(ctx), x: 0, y: 1 },

                // Navigation
                { ...key(icon("#cross", red), k("menu"), { long: hold("menu") }), x: 1, y: 1 },
                { ...key(icon("#arrow-up", white), k("up"), { repeat }), x: 2, y: 1 },
                { ...key(icon("#check", green), k("select"), { long: hold("select") }), x: 3, y: 1 },
                { ...key(icon("#arrow-left", white), k("left"), { repeat }), x: 1, y: 2 },
                { ...key(icon("#arrow-down", white), k("down"), { repeat }), x: 2, y: 2 },
                { ...key(icon("#arrow-right", white), k("right"), { repeat }), x: 3, y: 2 },
            ],
        ],
        // the macro pad. urem fired menu and select on release (so a long press doesn't also tap): tap, and long
        // for the hold
        pad: {
            key1: { tap: k("menu"), long: hold("menu") },
            key2: { press: k("up"), repeat },
            key3: { tap: k("select"), long: hold("select") },
            key4: { press: k("left"), repeat },
            key5: { press: k("down"), repeat },
            key6: { press: k("right"), repeat },
        },
    });
}
