// Tests for the Apple TV activity: every slot and pad binding against urem's activity-atv.js
// (wip/complete-urem2-layout.md, Notes, Activity atv). From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { atv } from "./atv.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { key, sevenChKey, muteKey } from "../keys.mjs";
import { icon, COLORS } from "../look.mjs";
import { volumeBindings } from "../pad.mjs";
import { targets } from "../../targets.mjs";
import { assertSlots, layoutWith } from "../../test/slots.mjs";
import { cleanLayout } from "../../test/resolve.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    const k = n => ctx.ops.atvKey(n);
    const white = { color: COLORS.white }, orange = { color: COLORS.orange };

    test(`${name}: atv meta and one page`, () => {
        const a = atv(ctx);
        const { pages, bindings, ...meta } = a;
        assert.deepEqual(meta, activityMeta(ctx, "atv"));
        assert.equal(pages.length, 1);
    });

    test(`${name}: atv's 15 slots match urem`, () => {
        assertSlots(atv(ctx).pages[0].keys, {
            // 0 Home toggle, 5 clock: not ported
            1: key(icon("#previous-track", white), k("previous")),
            2: key(icon("#next-track", white), k("next")),
            6: key(icon("#rewind", white), k("left"), { repeat: 100 }),
            7: key(icon("#forward", white), k("right"), { repeat: 100 }),
            10: sevenChKey(ctx),
            11: key(icon("#play", orange), k("play")),
            12: key(icon("#pause", orange), k("pause")),
            13: key(icon("#stop", orange), k("menu")),
            14: muteKey(ctx),
        });
    });

    test(`${name}: atv pad: menu/select tap and hold, arrows on press, then the volume`, () => {
        assert.deepEqual(atv(ctx).bindings, [
            { control: "key1", event: "tap", do: k("menu") },
            { control: "key1", event: "long", do: ctx.ops.atvKey("menu", "hold") },
            { control: "key2", event: "press", do: k("up") },
            { control: "key3", event: "tap", do: k("select") },
            { control: "key3", event: "long", do: ctx.ops.atvKey("select", "hold") },
            { control: "key4", event: "press", do: k("left") },
            { control: "key5", event: "press", do: k("down") },
            { control: "key6", event: "press", do: k("right") },
            ...volumeBindings(ctx),
        ]);
    });

    test(`${name}: atv resolves clean`, async () => {
        const tree = await cleanLayout(layoutWith(ctx, { activities: { atv: atv(ctx) } }));
        assert.equal(tree.activities[0].pages[0].keys.filter(k => k.normalFg).length, 9);
    });
}
