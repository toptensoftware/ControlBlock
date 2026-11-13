// Tests for the PlayStation activity: urem's had no keys (its sound keys were global); urem2 gives it one page
// with the shared 7ch Stereo and mute keys where atv has them. From the repo root:
// node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { playstation } from "./playstation.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { sevenChKey, muteKey } from "../keys.mjs";
import { volumeBindings } from "../pad.mjs";
import { targets } from "../../targets.mjs";
import { assertSlots, layoutWith } from "../../test/slots.mjs";
import { cleanLayout } from "../../test/resolve.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);

    test(`${name}: playstation: meta, one page with 7ch Stereo and mute, volume bindings only`, () => {
        const { pages, bindings, pad, ...meta } = playstation(ctx);
        assert.deepEqual(meta, activityMeta(ctx, "playstation"));
        assert.deepEqual(pad, {});
        assert.equal(pages.length, 1);
        assertSlots(pages[0].keys, { 10: sevenChKey(ctx), 14: muteKey(ctx) });
        assert.deepEqual(bindings, volumeBindings(ctx));
    });

    test(`${name}: playstation resolves clean`, async () => {
        const tree = await cleanLayout(layoutWith(ctx, { activities: { playstation: playstation(ctx) } }));
        assert.equal(tree.activities[3].pages[0].keys.filter(k => k.normal).length, 2);
    });
}
