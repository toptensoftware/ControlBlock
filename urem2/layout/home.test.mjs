// Tests for Home: its keys (what they are, not where), against urem's homePage.js (wip/complete-urem2-layout.md,
// Notes, Home). From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { home } from "./home.mjs";
import { makeContext } from "./context.mjs";
import { icon, label, COLORS } from "./look.mjs";
import { activityKeyGestures } from "./devices.mjs";
import { targets } from "../targets.mjs";
import { assertKeys, layoutWith } from "../test/pages.mjs";
import { cleanLayout } from "../test/resolve.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    // tap switches, long switches and resyncs
    const activityKey = (look, a) => ({ ...look, ...activityKeyGestures(ctx, a) });

    test(`${name}: Home's keys: power and the four activities`, () => {
        assertKeys(home(ctx).keys, [
            activityKey(icon("#power", { color: COLORS.red, scale: 0.7 }), null),   // long: resync none
            activityKey(icon("#playstation"), "playstation"),
            activityKey(icon("#steam"), "steam"),
            activityKey(icon("#apple-tv", { color: COLORS.grey }), "atv"),
            activityKey(label("TV", { color: COLORS.orange, font: "bold 38px sans-serif" }), "hdd"),
        ]);
        assert.deepEqual(Object.keys(home(ctx)), ["keys"]);
    });

    test(`${name}: Home resolves clean (custom SVGs found, scripts compile)`, async () => {
        const tree = await cleanLayout(layoutWith(ctx, { home: home(ctx) }));
        assert.equal(tree.home.keys.filter(k => k.normal).length, home(ctx).keys.length);
    });
}
