// Tests for the Apple TV activity: meta, the macro pad bindings (urem's activity-atv.js) and a clean resolve. The
// pages aren't pinned: rearrange them freely. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { atv } from "./atv.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { volumeBindings, ARROW_REPEAT } from "../pad.mjs";
import { targets } from "../../targets.mjs";
import { assertActivityResolves } from "../../test/pages.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    const k = n => ctx.ops.atvKey(n);
    const hold = n => ctx.ops.atvKey(n, "hold");
    const repeat = ARROW_REPEAT;

    test(`${name}: atv meta and the pad's repeats`, () => {
        const { pages, bindings, pad, ...meta } = atv(ctx);
        assert.deepEqual(meta, activityMeta(ctx, "atv"));
        assert.deepEqual(pad, { key2: { repeat }, key4: { repeat }, key5: { repeat }, key6: { repeat } });
    });

    test(`${name}: atv pad: menu/select tap and hold, arrows on press, then the volume`, () => {
        assert.deepEqual(atv(ctx).bindings, [
            { control: "key1", event: "tap", do: k("menu") },
            { control: "key1", event: "long", do: hold("menu") },
            { control: "key2", event: "press", do: k("up") },
            { control: "key3", event: "tap", do: k("select") },
            { control: "key3", event: "long", do: hold("select") },
            { control: "key4", event: "press", do: k("left") },
            { control: "key5", event: "press", do: k("down") },
            { control: "key6", event: "press", do: k("right") },
            ...volumeBindings(ctx),
        ]);
    });

    test(`${name}: atv resolves clean`, () => assertActivityResolves(ctx, "atv", atv(ctx)));
}
