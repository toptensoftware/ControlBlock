// Tests for the macro pad and encoder bindings.
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { padBindings, volumeBindings, padArrows, padPage, volumeStep, ARROW_REPEAT, PAD_CONTROLS } from "./pad.mjs";
import { key, grid, muteKey } from "./keys.mjs";
import { icon, COLORS } from "./look.mjs";
import { setVolume } from "../tables/yamaha.mjs";
import { makeContext } from "./context.mjs";
import { volumeScript, yamahaToggle } from "./scripts.mjs";
import { ROOT_DEFAULTS } from "./look.mjs";
import { targets } from "../targets.mjs";
import { cleanLayout } from "../test/resolve.mjs";

const ctx = makeContext(targets.test);
const { ops } = ctx;

test("pad controls: key1..key7 and enc1", () => {
    assert.deepEqual(PAD_CONTROLS, ["key1", "key2", "key3", "key4", "key5", "key6", "key7", "enc1"]);
});

test("padBindings: control/event/do in key order, then gesture order", () => {
    assert.deepEqual(padBindings({
        key2: { press: ops.atvKey("up"), repeat: { initialDelay: 500, period: 100 } },
        key1: { long: ops.atvKey("menu", "hold"), tap: ops.atvKey("menu") },
    }), [
        { control: "key1", event: "tap", do: ops.atvKey("menu") },
        { control: "key1", event: "long", do: ops.atvKey("menu", "hold") },
        { control: "key2", event: "press", do: ops.atvKey("up") },
    ]);
    assert.deepEqual(padBindings({}), []);
});

test("padBindings: unknown controls, events and bad repeats throw", () => {
    assert.throws(() => padBindings({ key8: { press: [] } }), /unknown pad control/);
    assert.throws(() => padBindings({ macro1: { press: [] } }), /unknown pad control/);
    assert.throws(() => padBindings({ key1: { hold: [] } }), /unknown event/);
    assert.throws(() => padBindings({ key1: { turn: [] } }), /unknown event/);
    assert.throws(() => padBindings({ enc1: { tap: [] } }), /unknown event/);
    assert.throws(() => padBindings({ key2: { press: [], repeat: 0 } }), /repeat/);
});

test("volumeBindings: key7 tap mutes, enc1 turn runs the volume script", () => {
    assert.deepEqual(volumeBindings(ctx), [
        { control: "key7", event: "tap", do: yamahaToggle(ctx.target, "mute") },
        { control: "enc1", event: "turn", do: volumeScript(ctx.target) },
    ]);
});

test("an activity with pad and volume bindings is clean", async () => {
    const tree = await cleanLayout({
        version: 1, defaults: ROOT_DEFAULTS, home: { keys: [] },
        activities: { a: { pages: [{ keys: [] }], bindings: [
            ...padBindings({ key3: { press: ops.ir("pana.ok") }, key4: { press: ops.ir("pana.left"), repeat: 100 } }),
            ...volumeBindings(ctx),
        ] } },
    });
    assert.deepEqual(tree.activities[0].bindings.map(b => `${b.control} ${b.event}`),
                     ["key3 press", "key4 press", "key7 tap", "enc1 turn"]);
});

test("padArrows: urem's arrow keys key2/4/5/6 = up/left/down/right on press, with the arrow repeat", () => {
    assert.deepEqual(ARROW_REPEAT, { initialDelay: 500, period: 100 });
    const ir = name => ops.ir(`pana.${name}`);
    const spec = padArrows(ir);
    assert.deepEqual(spec, {
        key2: { press: ir("up"), repeat: ARROW_REPEAT },
        key4: { press: ir("left"), repeat: ARROW_REPEAT },
        key5: { press: ir("down"), repeat: ARROW_REPEAT },
        key6: { press: ir("right"), repeat: ARROW_REPEAT },
    });
    assert.deepEqual(padBindings(spec).map(b => `${b.control} ${b.event}`),
                     ["key2 press", "key4 press", "key5 press", "key6 press"]);
});

test("volumeStep: one Yamaha step per tap", () => {
    assert.deepEqual(volumeStep(ctx, "up"), ops.yamaha(setVolume("up", 1)));
    assert.deepEqual(volumeStep(ctx, "down"), ops.yamaha(setVolume("down", 1)));
    assert.throws(() => volumeStep(ctx, "sideways"), /volume direction/);
});

test("padPage: the pad's keys in its 2x3 shape, tap ?? press and long, then Vol+/Vol-/mute", () => {
    const k = n => ops.atvKey(n);
    const spec = { key1: { tap: k("menu"), long: k("home") }, key3: { press: k("select") }, ...padArrows(k) };
    const white = { color: COLORS.white }, purple = { color: COLORS.purple };
    assert.deepEqual(padPage(ctx, spec), {
        1: key(icon("#cross", { color: COLORS.red }), k("menu"), { long: k("home") }),
        2: key(icon("#arrow-up", white), k("up")),
        3: key(icon("#check", { color: COLORS.green }), k("select")),
        6: key(icon("#arrow-left", white), k("left")),
        7: key(icon("#arrow-down", white), k("down")),
        8: key(icon("#arrow-right", white), k("right")),
        4: key(icon("#volume-up", purple), volumeStep(ctx, "up")),
        9: key(icon("#volume-down", purple), volumeStep(ctx, "down")),
        14: muteKey(ctx),
    });
});

test("padPage: unbound controls leave their slots empty", () => {
    const slots = padPage(ctx, { key2: { press: ops.slideshow("voteUp") } });
    assert.deepEqual(Object.keys(slots).map(Number).sort((a, b) => a - b), [2, 4, 9, 14]);
});

test("padPage resolves clean (icons included)", async () => {
    const spec = { key1: { tap: ops.atvKey("menu") }, key3: { tap: ops.atvKey("select") }, ...padArrows(ops.atvKey) };
    const tree = await cleanLayout({ version: 1, defaults: ROOT_DEFAULTS, home: { keys: grid(padPage(ctx, spec)) } });
    assert.equal(tree.home.keys.filter(k => k.normal).length, 9);
});
