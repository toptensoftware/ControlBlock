// Tests for the key helpers and the shared sound keys.
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { key, withRepeat, sevenChKey, muteKey, volumeStep, volUpKey, volDownKey, VOLUME_REPEAT } from "./keys.mjs";
import { setVolume } from "../tables/yamaha.mjs";
import { makeContext } from "./context.mjs";
import { icon, label, COLORS, ROOT_DEFAULTS } from "./look.mjs";
import { yamahaToggle } from "./scripts.mjs";
import { targets } from "../targets.mjs";
import { cleanLayout } from "../test/resolve.mjs";

const ctx = makeContext(targets.test);
const { ops } = ctx;

test("makeContext: the target and its op builders", () => {
    assert.equal(ctx.target, targets.test);
    assert.deepEqual(ctx.ops.ir("pana.guide"), ops.ir("pana.guide"));
});

test("key: look plus tap, and long when given", () => {
    const look = icon("#play", { color: COLORS.orange });
    assert.deepEqual(key(look, ops.atvKey("play")), { ...look, tap: ops.atvKey("play") });
    assert.deepEqual(key(look, ops.atvKey("menu"), { long: ops.atvKey("menu", "hold") }),
                     { ...look, tap: ops.atvKey("menu"), long: ops.atvKey("menu", "hold") });
});

test("repeat is kept where urem had it (the bake's key repeat), absent when not given", () => {
    const look = icon("#rewind");
    const plain = key(look, ops.atvKey("left"));
    assert.equal(Object.hasOwn(plain, "repeat"), false);
    assert.deepEqual(key(look, ops.atvKey("left"), { repeat: 100 }), { ...plain, repeat: 100 });
    assert.deepEqual(key(look, ops.atvKey("left"), { repeat: { initialDelay: 500, period: 100 } }),
                     { ...plain, repeat: { initialDelay: 500, period: 100 } });
    assert.deepEqual(withRepeat({ press: [], repeat: 100 }), { press: [], repeat: 100 });
    assert.deepEqual(withRepeat({ press: [], repeat: undefined }), { press: [] });
    assert.deepEqual(withRepeat({ press: [] }), { press: [] });
});

test("bad repeat values and unknown key options throw", () => {
    const look = icon("#rewind");
    for (const repeat of [0, -5, "100", { period: 100 }, { initialDelay: 500 }, { initialDelay: 5, period: 1, x: 1 }])
        assert.throws(() => key(look, [], { repeat }), /repeat/, JSON.stringify(repeat));
    assert.throws(() => key(look, [], { hold: [] }), /unknown key option/);
});

test("the shared sound keys: 7ch Stereo label and mute icon, purple, Yamaha toggles", () => {
    assert.deepEqual(sevenChKey(ctx),
                     key(label("7ch Stereo", { color: COLORS.purple }), yamahaToggle(ctx.target, "sevenCh")));
    assert.deepEqual(muteKey(ctx),
                     key(icon("#volume-mute", { color: COLORS.purple }), yamahaToggle(ctx.target, "mute")));
});

test("volumeStep: one Yamaha step per tap", () => {
    assert.deepEqual(volumeStep(ctx, "up"), ops.yamaha(setVolume("up", 1)));
    assert.deepEqual(volumeStep(ctx, "down"), ops.yamaha(setVolume("down", 1)));
    assert.throws(() => volumeStep(ctx, "sideways"), /volume direction/);
});

test("Vol+ and Vol-: purple volume icons, one step per tap, repeating while held", () => {
    const purple = { color: COLORS.purple };
    assert.deepEqual(VOLUME_REPEAT, { initialDelay: 500, period: 100 });
    const repeat = VOLUME_REPEAT;
    assert.deepEqual(volUpKey(ctx), key(icon("#volume-up", purple), volumeStep(ctx, "up"), { repeat }));
    assert.deepEqual(volDownKey(ctx), key(icon("#volume-down", purple), volumeStep(ctx, "down"), { repeat }));
});

test("a layout with the key helpers is clean", async () => {
    const tree = await cleanLayout({ version: 1, defaults: ROOT_DEFAULTS, home: { keys: [
        key(icon("#forward"), ops.atvKey("right"), { repeat: 100 }),
        key(icon("#play"), ops.atvKey("menu"), { long: ops.atvKey("menu", "hold") }),
        sevenChKey(ctx), muteKey(ctx), volUpKey(ctx), volDownKey(ctx),
    ] } });
    assert.deepEqual(tree.home.bindings.map(b => `${b.control} ${b.event}`),
                     ["touch[1] tap", "touch[2] tap", "touch[2] long", "touch[3] tap", "touch[4] tap", "touch[5] tap",
                      "touch[6] tap"]);
    assert.equal(tree.home.keys.filter(k => k.normal).length, 6);
    // urem's meaning: a number is both the initial delay and the period
    assert.deepEqual(tree.home.keys.map(k => [k.repeatDelayMs, k.repeatMs]),
                     [[100, 100], [0, 0], [0, 0], [0, 0], [500, 100], [500, 100]]);
});

test("key: repeat and long together throw (the bake refuses them)", () => {
    assert.throws(() => key(icon("#play"), [], { long: [], repeat: 100 }), /repeat.*long|long.*repeat/);
});
