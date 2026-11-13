// Tests for the PlayStation activity (urem's had no keys; urem2 gives it a page of sound keys): meta, the volume
// bindings only, and a clean resolve. The page isn't pinned. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { playstation } from "./playstation.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { volumeBindings } from "../pad.mjs";
import { targets } from "../../targets.mjs";
import { assertActivityResolves } from "../../test/pages.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);

    test(`${name}: playstation: meta, volume bindings only`, () => {
        const { pages, bindings, pad, ...meta } = playstation(ctx);
        assert.deepEqual(meta, activityMeta(ctx, "playstation"));
        assert.deepEqual(pad, {});
        assert.deepEqual(bindings, volumeBindings(ctx));
    });

    test(`${name}: playstation resolves clean`, () => assertActivityResolves(ctx, "playstation", playstation(ctx)));
}
