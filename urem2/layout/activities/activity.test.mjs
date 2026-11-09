// Tests for defineActivity: the meta, the pages from slot maps, and the pad bindings followed by the volume ones.
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { defineActivity } from "./activity.mjs";
import { makeContext } from "../context.mjs";
import { activityMeta } from "../devices.mjs";
import { key, grid, muteKey } from "../keys.mjs";
import { icon } from "../look.mjs";
import { padBindings, volumeBindings, padPage } from "../pad.mjs";
import { targets } from "../../targets.mjs";

const ctx = makeContext(targets.test);
const { ops } = ctx;

test("defineActivity: meta, one page per slot map, pad bindings then volume", () => {
    const a = key(icon("#play"), ops.ir("pana.play"));
    const pad = { key3: { press: ops.ir("pana.ok") } };
    assert.deepEqual(defineActivity(ctx, "hdd", { pages: [{ 11: a }, { 14: muteKey(ctx) }], pad }), {
        ...activityMeta(ctx, "hdd"),
        pages: [{ keys: grid({ 11: a }) }, { keys: grid({ 14: muteKey(ctx) }) }],
        bindings: [...padBindings(pad), ...volumeBindings(ctx)],
    });
});

test("defineActivity: no pad spec gives the volume bindings only; no pages or an unknown activity throws", () => {
    assert.deepEqual(defineActivity(ctx, "playstation", { pages: [{}] }).bindings, volumeBindings(ctx));
    assert.throws(() => defineActivity(ctx, "playstation", { pages: [] }), /at least one page/);
    assert.throws(() => defineActivity(ctx, "xbox", { pages: [{}] }), /unknown activity/);
    assert.throws(() => defineActivity(ctx, "hdd", { pages: [{}], pad: { key7: { tap: [] } } }), /key7/);
});

test("defineActivity: padPage puts the pad page first, bindings unchanged", () => {
    const a = key(icon("#play"), ops.ir("pana.play"));
    const pad = { key3: { press: ops.ir("pana.ok") } };
    const withPage = defineActivity(ctx, "hdd", { pages: [{ 11: a }], pad, padPage: true });
    assert.deepEqual(withPage.pages, [{ keys: grid(padPage(ctx, pad)) }, { keys: grid({ 11: a }) }]);
    assert.deepEqual(withPage.bindings, defineActivity(ctx, "hdd", { pages: [{ 11: a }], pad }).bindings);
});
