// Home (urem homePage.js + core.js). Not ported: the Home toggle (0: Home is page 0, swipe back), the Moonraker
// printer status (3) and the clock (5).

import { key, sevenChKey } from "./keys.mjs";
import { icon, label, COLORS } from "./look.mjs";
import { switchTo, activityKeyGestures } from "./devices.mjs";

export function home(ctx)
{
    const { ops } = ctx;
    const orange = { color: COLORS.orange };
    // tap switches; long switches and resyncs
    const activityKey = (look, a) => ({ ...look, ...activityKeyGestures(ctx, a) });
    // switch, then the key's own op (urem awaited the switch first)
    const thenKey = (look, a, op) => key(look, [switchTo(a), op]);
    const appKey = (svg, app) => thenKey(icon(`icons/${svg}.svg`), "atv", ops.atvApp(app));
    const dvrKey = (iconName, code) => thenKey(icon(iconName, orange), "hdd", ops.ir(`pana.${code}`));

    return { keys: [

        { ...activityKey(icon("#power", { color: COLORS.red, scale: 0.7 }), null), x: 0, y: 0 },    // long: everything off again


        { ...activityKey(icon("#playstation"), "playstation"), x: 1, y: 0, w: 2, h: 1.5 },
        { ...activityKey(icon("#steam"), "steam"), x: 3, y: 0, w: 2, h:1.5 },
        { ...activityKey(icon("#apple-tv", { color: COLORS.grey }), "atv"), x: 1, y: 1.5, w: 2, h: 1.5 },
        { ...activityKey(label("TV", { ...orange, font: "bold 38px sans-serif" }), "hdd"), x: 3, y: 1.5, w: 2, h: 1.5 },

        /*
        { ...sevenChKey(ctx), x: 0, y: 2 },
        7: appKey("netflix", "Netflix"),
        8: appKey("prime", "Prime Video"),
        9: appKey("youtube", "YouTube"),
        12: dvrKey("#list", "videoMenu"),
        13: dvrKey("#guide", "guide"),
        14: dvrKey("#function", "functionMenu"),
        */
    ] };
}
