// Tests for the TV/HDD activity (Panasonic DVR over IR): meta, the macro pad bindings (urem's activity-hdd.js) and a
// clean resolve. The pages aren't pinned: rearrange them freely. From the repo root:
// node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { hdd } from "./hdd.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { volumeBindings, ARROW_REPEAT } from "../pad.mjs";
import { targets } from "../../targets.mjs";
import { assertActivityResolves } from "../../test/pages.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    const ir = code => ctx.ops.ir(`pana.${code}`);
    const repeat = ARROW_REPEAT;

    test(`${name}: hdd meta and the pad's repeats`, () => {
        const { pages, bindings, pad, ...meta } = hdd(ctx);
        assert.deepEqual(meta, activityMeta(ctx, "hdd"));
        assert.deepEqual(pad, { key2: { repeat }, key4: { repeat }, key5: { repeat }, key6: { repeat } });
    });

    test(`${name}: hdd pad: return/up/ok/left/down/right on press, then the volume`, () => {
        assert.deepEqual(hdd(ctx).bindings, [
            { control: "key1", event: "press", do: ir("return") },
            { control: "key2", event: "press", do: ir("up") },
            { control: "key3", event: "press", do: ir("ok") },
            { control: "key4", event: "press", do: ir("left") },
            { control: "key5", event: "press", do: ir("down") },
            { control: "key6", event: "press", do: ir("right") },
            ...volumeBindings(ctx),
        ]);
    });

    test(`${name}: hdd resolves clean`, () => assertActivityResolves(ctx, "hdd", hdd(ctx)));
}
