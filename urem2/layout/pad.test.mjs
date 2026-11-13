// Tests for the macro pad and encoder bindings.
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { padBindings, padControls, volumeBindings, ARROW_REPEAT, PAD_CONTROLS } from "./pad.mjs";
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

test("padControls: each repeating key's repeat, in control order; no repeat, no entry", () => {
    assert.deepEqual(Object.entries(padControls({
        key6: { press: [], repeat: 100 },
        key1: { tap: [] },
        key2: { press: [], repeat: ARROW_REPEAT },
    })), [["key2", { repeat: ARROW_REPEAT }], ["key6", { repeat: 100 }]]);
    assert.deepEqual(padControls({}), {});
    assert.deepEqual(padControls({ key1: { tap: [], long: [] } }), {});
});

test("padControls: the same checks as padBindings; a repeat on the encoder or with long throws", () => {
    assert.throws(() => padControls({ key8: { press: [], repeat: 100 } }), /unknown pad control/);
    assert.throws(() => padControls({ key2: { press: [], repeat: 0 } }), /repeat/);
    assert.throws(() => padControls({ enc1: { turn: [], repeat: 100 } }), /repeat/);
    assert.throws(() => padControls({ key1: { tap: [], long: [], repeat: 100 } }), /long/);
    assert.throws(() => padBindings({ key1: { tap: [], long: [], repeat: 100 } }), /long/);
});

test("volumeBindings: key7 tap mutes, enc1 turn runs the volume script", () => {
    assert.deepEqual(volumeBindings(ctx), [
        { control: "key7", event: "tap", do: yamahaToggle(ctx.target, "mute") },
        { control: "enc1", event: "turn", do: volumeScript(ctx.target) },
    ]);
});

test("an activity with pad and volume bindings and the pad's repeats is clean", async () => {
    const spec = { key3: { press: ops.ir("pana.ok") }, key4: { press: ops.ir("pana.left"), repeat: 100 } };
    const tree = await cleanLayout({
        version: 1, defaults: ROOT_DEFAULTS, home: { keys: [] },
        activities: { a: { pages: [{ keys: [] }], bindings: [
            ...padBindings(spec),
            ...volumeBindings(ctx),
        ], pad: padControls(spec) } },
    });
    assert.deepEqual(tree.activities[0].bindings.map(b => `${b.control} ${b.event}`),
                     ["key3 press", "key4 press", "key7 tap", "enc1 turn"]);
    assert.deepEqual(tree.activities[0].controls,
                     [{ control: "key4", longPressMs: 0, repeatDelayMs: 100, repeatMs: 100 }]);
});

test("ARROW_REPEAT: urem's arrowRepeat", () => {
    assert.deepEqual(ARROW_REPEAT, { initialDelay: 500, period: 100 });
});
