// Tests for defineActivity: the meta, the pages from key lists, and the pad bindings followed by the volume ones.
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { defineActivity } from "./activity.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { key, muteKey } from "../keys.mjs";
import { icon } from "../look.mjs";
import { padBindings, padControls, volumeBindings, ARROW_REPEAT } from "../pad.mjs";
import { targets } from "../../targets.mjs";

const ctx = makeContext(targets.test);
const { ops } = ctx;

test("defineActivity: meta, one page per key list (kept as written), pad bindings then volume", () => {
    const a = { ...key(icon("#play"), ops.ir("pana.play")), x: 1, y: 2 };
    const m = { ...muteKey(ctx), x: 4, y: 2, w: 0.5 };
    const pad = { key3: { press: ops.ir("pana.ok") } };
    assert.deepEqual(defineActivity(ctx, "hdd", { pages: [[a], [m]], pad }), {
        ...activityMeta(ctx, "hdd"),
        pages: [{ keys: [a] }, { keys: [m] }],
        bindings: [...padBindings(pad), ...volumeBindings(ctx)],
        pad: {},
    });
});

test("defineActivity: no pad spec gives the volume bindings only; bad pages or an unknown activity throw", () => {
    assert.deepEqual(defineActivity(ctx, "playstation", { pages: [[]] }).bindings, volumeBindings(ctx));
    assert.throws(() => defineActivity(ctx, "playstation", { pages: [] }), /at least one page/);
    assert.throws(() => defineActivity(ctx, "playstation", { pages: [{ 10: muteKey(ctx) }] }), /page 0.*array/);
    assert.throws(() => defineActivity(ctx, "xbox", { pages: [[]] }), /unknown activity/);
    assert.throws(() => defineActivity(ctx, "hdd", { pages: [[]], pad: { key7: { tap: [] } } }), /key7/);
    assert.throws(() => defineActivity(ctx, "hdd", { pages: [[]], padPage: true }), /unknown option/);
});

test("defineActivity: pad is padControls of the pad spec (the pad's repeats)", () => {
    const pad = { key3: { press: ops.ir("pana.ok") }, key5: { press: ops.ir("pana.down"), repeat: ARROW_REPEAT } };
    const a = defineActivity(ctx, "hdd", { pages: [[]], pad });
    assert.deepEqual(a.pad, padControls(pad));
    assert.deepEqual(a.pad, { key5: { repeat: ARROW_REPEAT } });
    assert.deepEqual(defineActivity(ctx, "playstation", { pages: [[]] }).pad, {});
});
