// Tests for Home: every slot against urem's homePage.js (wip/complete-urem2-layout.md, Notes, Home).
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { home } from "./home.mjs";
import { makeContext } from "./context.mjs";
import { key, sevenChKey } from "./keys.mjs";
import { icon, label, COLORS } from "./look.mjs";
import { switchTo, activityKeyGestures } from "./devices.mjs";
import { targets } from "../targets.mjs";
import { assertSlots, layoutWith } from "../test/slots.mjs";
import { cleanLayout } from "../test/resolve.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    const { ops } = ctx;
    const orange = COLORS.orange;
    // tap switches, long switches and resyncs
    const activityKey = (look, a) => ({ ...look, ...activityKeyGestures(ctx, a) });

    test(`${name}: Home's 15 slots match urem`, () => {
        assertSlots(home(ctx).keys, {
            // 0 Home toggle, 3 Moonraker, 5 clock: not ported
            1: activityKey(icon("#playstation"), "playstation"),
            2: activityKey(icon("#steam"), "steam"),
            4: activityKey(icon("#power", { color: COLORS.red, scale: 0.7 }), null),   // long: resync none
            6: activityKey(icon("#apple-tv", { color: COLORS.grey }), "atv"),
            7: key(icon("icons/netflix.svg"), [switchTo("atv"), ops.atvApp("Netflix")]),
            8: key(icon("icons/prime.svg"), [switchTo("atv"), ops.atvApp("Prime Video")]),
            9: key(icon("icons/youtube.svg"), [switchTo("atv"), ops.atvApp("YouTube")]),
            10: sevenChKey(ctx),
            11: activityKey(label("TV", { color: orange, font: "bold 28px sans-serif" }), "hdd"),
            12: key(icon("#list", { color: orange }), [switchTo("hdd"), ops.ir("pana.videoMenu")]),
            13: key(icon("#guide", { color: orange }), [switchTo("hdd"), ops.ir("pana.guide")]),
            14: key(icon("#function", { color: orange }), [switchTo("hdd"), ops.ir("pana.functionMenu")]),
        });
    });

    test(`${name}: the app keys launch the right bundles; only the activity and power keys have a long press`, () => {
        const keys = home(ctx).keys;
        assert.equal(keys[7].tap[1].data, "app com.netflix.Netflix");
        assert.equal(keys[8].tap[1].data, "app com.amazon.aiv.AIVApp");
        assert.equal(keys[9].tap[1].data, "app com.google.ios.youtube");
        assert.deepEqual(keys.flatMap((k, i) => k?.long ? [i] : []), [1, 2, 4, 6, 11]);
        assert.deepEqual(Object.keys(home(ctx)), ["keys"]);
    });

    test(`${name}: Home resolves clean (custom SVGs found, scripts compile)`, async () => {
        const tree = await cleanLayout(layoutWith(ctx, { home: home(ctx) }));
        assert.equal(tree.home.keys.filter(k => k.normalFg).length, 12);
    });
}
