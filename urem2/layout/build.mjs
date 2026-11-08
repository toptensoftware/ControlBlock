// The whole urem2 layout for one target (urem2/targets.mjs): root look, devices, Home and the activities. The entry
// files (urem2/urem2.mjs, urem2/urem2-test.mjs) are one call each.

import { makeContext } from "./context.mjs";
import { ROOT_DEFAULTS } from "./look.mjs";
import { devices, ACTIVITIES } from "./devices.mjs";
import { home } from "./home.mjs";
import { atv } from "./activities/atv.mjs";
import { hdd } from "./activities/hdd.mjs";
import { steam } from "./activities/steam.mjs";
import { playstation } from "./activities/playstation.mjs";

const BUILDERS = { atv, hdd, steam, playstation };

export function buildLayout(target)
{
    const ctx = makeContext(target);
    return {
        version: 1,
        defaults: ROOT_DEFAULTS,
        devices: devices(ctx),
        home: home(ctx),
        // In ACTIVITIES order (the order the device lists them in)
        activities: Object.fromEntries(Object.keys(ACTIVITIES).map(a => [a, BUILDERS[a](ctx)])),
    };
}
