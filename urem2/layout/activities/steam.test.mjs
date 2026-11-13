// Tests for the Steam activity (HOT PC and the photo slideshow): meta, the macro pad bindings (urem's
// activity-steam.js) and a clean resolve. The pages aren't pinned: rearrange them freely. From the repo root:
// node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { steam } from "./steam.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { volumeBindings } from "../pad.mjs";
import { targets } from "../../targets.mjs";
import { assertActivityResolves } from "../../test/pages.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    const show = cmd => ctx.ops.slideshow(cmd);

    test(`${name}: steam meta, no pad repeats`, () => {
        const { pages, bindings, pad, ...meta } = steam(ctx);
        assert.deepEqual(meta, activityMeta(ctx, "steam"));
        assert.deepEqual(pad, {});  // urem had no repeat on these
    });

    test(`${name}: steam pad: key2/4/5/6 vote and step on press, key1/key3 unbound, then the volume`, () => {
        assert.deepEqual(steam(ctx).bindings, [
            { control: "key2", event: "press", do: show("voteUp") },
            { control: "key4", event: "press", do: show("prev") },
            { control: "key5", event: "press", do: show("voteDown") },
            { control: "key6", event: "press", do: show("next") },
            ...volumeBindings(ctx),
        ]);
    });

    test(`${name}: steam resolves clean`, () => assertActivityResolves(ctx, "steam", steam(ctx)));
}
