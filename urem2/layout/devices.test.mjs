// Tests for the devices, the activity meta (devices, hook, display name), switching and the long-press resync.
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { devices, ACTIVITIES, activityMeta, hooks, switchTo, resync, activityKeyGestures } from "./devices.mjs";
import { makeContext } from "./context.mjs";
import { label, ROOT_DEFAULTS } from "./look.mjs";
import { setPower, setInput } from "../tables/yamaha.mjs";
import { targets } from "../targets.mjs";
import { cleanLayout } from "../test/resolve.mjs";

const MAX_OPS = 64;     // ops per list (docs/standalone.md, layout_blob.h)

for (const [name, target] of Object.entries(targets))
{
    const ctx = makeContext(target);
    const { ops } = ctx;
    const devs = devices(ctx);

    test(`${name}: devices in order with their power lists`, () => {
        assert.deepEqual(Object.keys(devs), ["yama", "lgtv", "atv", "hotpc"]);
        assert.deepEqual(devs.yama, { turnOn: [ops.yamaha(setPower(true))], turnOff: [ops.yamaha(setPower(false))] });
        assert.deepEqual(devs.lgtv, { turnOn: ops.irPower("lg.powerOn"), turnOff: ops.irPower("lg.powerOff") });
        assert.deepEqual(devs.atv, {
            turnOn: [...ops.irPower("atv.powerOn"), ops.atvPing()],
            turnOff: [...ops.irPower("atv.powerOff"), ops.atvDisconnect()],
        });
        assert.deepEqual(devs.hotpc, { turnOn: [ops.wake()] });
    });

    test(`${name}: activity meta`, () => {
        assert.deepEqual(Object.keys(ACTIVITIES), ["atv", "hdd", "steam", "playstation"]);
        const expect = {
            atv: [["yama", "lgtv", "atv"], "hdmi2", "Apple TV"],
            hdd: [["yama", "lgtv"], "hdmi1", "TV"],
            steam: [["yama", "lgtv", "hotpc"], "hdmi3", "Steam"],
            playstation: [["yama", "lgtv"], "hdmi4", "PlayStation"],
        };
        for (const [a, [devices, input, displayName]] of Object.entries(expect))
        {
            assert.deepEqual(hooks(ctx, a), { didActivate: [ops.yamaha(setInput(input))] });
            assert.deepEqual(activityMeta(ctx, a), { displayName, devices, hooks: hooks(ctx, a) });
        }
    });

    test(`${name}: switchTo and the resync`, () => {
        assert.deepEqual(switchTo("atv"), { op: "switchActivity", activity: "atv" });
        assert.deepEqual(switchTo(null), { op: "switchActivity", activity: null });
        assert.deepEqual(resync(ctx, "atv"), [
            ...devs.yama.turnOn, ...devs.lgtv.turnOn, ...devs.atv.turnOn, ...hooks(ctx, "atv").didActivate,
        ]);
        assert.deepEqual(activityKeyGestures(ctx, "steam"), {
            tap: [switchTo("steam")],
            long: [switchTo("steam"), ...resync(ctx, "steam")],
        });
    });

    test(`${name}: unknown activities throw`, () => {
        assert.throws(() => switchTo("xbox"), /unknown activity/);
        assert.throws(() => activityMeta(ctx, "xbox"), /unknown activity/);
        assert.throws(() => resync(ctx, "xbox"), /unknown activity/);
    });

    test(`${name}: every list fits the device's ${MAX_OPS} ops`, () => {
        const lists = [
            ...Object.values(devs).flatMap(d => Object.values(d)),
            ...Object.keys(ACTIVITIES).flatMap(a => [hooks(ctx, a).didActivate, ...Object.values(activityKeyGestures(ctx, a))]),
        ];
        for (const list of lists)
            assert.ok(list.length <= MAX_OPS, `${list.length} ops`);
    });

    test(`${name}: a layout with the devices, four activities and the activity keys is clean`, async () => {
        const names = Object.keys(ACTIVITIES);
        const tree = await cleanLayout({
            version: 1, defaults: ROOT_DEFAULTS,
            devices: devs,
            home: { keys: names.map(a => ({ ...label(a), ...activityKeyGestures(ctx, a) })) },
            activities: Object.fromEntries(names.map(a => [a, { ...activityMeta(ctx, a), pages: [{ keys: [] }] }])),
        });
        assert.deepEqual(tree.devices.map(d => d.name), ["yama", "lgtv", "atv", "hotpc"]);
        assert.deepEqual(tree.activities.map(a => a.devices), [[0, 1, 2], [0, 1], [0, 1, 3], [0, 1]]);
        assert.equal(tree.home.bindings.length, 8);
    });
}
