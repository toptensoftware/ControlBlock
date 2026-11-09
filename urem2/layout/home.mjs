// Home (urem homePage.js + core.js). Not ported: the Home toggle (0: Home is page 0, swipe back), the Moonraker
// printer status (3) and the clock (5).

import { key, grid, sevenChKey } from "./keys.mjs";
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

    return { keys: grid({
        1: activityKey(icon("#playstation"), "playstation"),
        2: activityKey(icon("#steam"), "steam"),
        4: activityKey(icon("#power", { color: COLORS.red, scale: 0.7 }), null),     // long: everything off again
        6: activityKey(icon("#apple-tv", { color: COLORS.grey }), "atv"),
        7: appKey("netflix", "Netflix"),
        8: appKey("prime", "Prime Video"),
        9: appKey("youtube", "YouTube"),
        10: sevenChKey(ctx),
        11: activityKey(label("TV", { ...orange, font: "bold 28px sans-serif" }), "hdd"),
        12: dvrKey("#list", "videoMenu"),
        13: dvrKey("#guide", "guide"),
        14: dvrKey("#function", "functionMenu"),
    }) };
}
