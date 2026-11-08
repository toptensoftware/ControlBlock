// Tests for urem2's Lua scripts, run in wasmoon against the test server's fake Yamaha.
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { yamahaToggle, volumeScript, VOL_FAST_US, VOL_MEDIUM_US, VOL_STEPS, VOL_MAX_STEP } from "./scripts.mjs";
import { targets } from "../targets.mjs";
import { setMute, setSoundProgram, setVolume } from "../tables/yamaha.mjs";
import { createFakeYamaha } from "../test-server/test-server.mjs";
import { runLua } from "../test/lua-harness.mjs";
import { cleanLayout, homeLayout } from "../test/resolve.mjs";

const target = targets.test;
const MAX_SCRIPT_BYTES = 16384;

// A fake Yamaha behind the harness's http.get; `down` = no reply at all, `broken` = a 500 with an HTML body
function yamahaGet(y, { down = false, broken = false } = {})
{
    return url => {
        if (down)
            return null;
        if (broken)
            return { status: 500, body: "<html><body>Internal Server Error</body></html>" };
        const r = y.handle("GET", url);
        return { status: r.status, body: r.body };
    };
}

async function run(op, y, opts)
{
    const r = await runLua(op.lua, { get: yamahaGet(y, opts) });
    assert.equal(r.error, null);
    return r;
}

// The script's body: the source without its generated `local` prelude
const bodyOf = src => src.split("\n").filter(l => !/^local [A-Z_]+ = /.test(l)).join("\n");

test("mute toggle: false -> true -> false, getStatus waited, the set fire and forget", async () => {
    const y = createFakeYamaha();
    const op = yamahaToggle(target, "mute");
    let r = await run(op, y);
    assert.equal(y.state.mute, true);
    assert.deepEqual(r.requests, [
        { url: target.yamaha + "/main/getStatus", reply: true },
        { url: target.yamaha + setMute(true), reply: false },
    ]);
    r = await run(op, y);
    assert.equal(y.state.mute, false);
    assert.deepEqual(r.requests[1], { url: target.yamaha + setMute(false), reply: false });
});

test("7ch toggle: surr_decoder -> 7ch_stereo -> surr_decoder", async () => {
    const y = createFakeYamaha();
    const op = yamahaToggle(target, "sevenCh");
    await run(op, y);
    assert.equal(y.state.sound_program, "7ch_stereo");
    const r = await run(op, y);
    assert.equal(y.state.sound_program, "surr_decoder");
    assert.deepEqual(r.requests[1], { url: target.yamaha + setSoundProgram("surr_decoder"), reply: false });
});

test("a failed get turns mute / 7ch on, as urem did", async () => {
    for (const fail of [{ down: true }, { broken: true }])
    {
        const y = createFakeYamaha();
        y.state.mute = true;                                        // even when it is on already
        let r = await run(yamahaToggle(target, "mute"), y, fail);
        assert.deepEqual(r.requests.at(-1), { url: target.yamaha + setMute(true), reply: false }, JSON.stringify(fail));
        r = await run(yamahaToggle(target, "sevenCh"), y, fail);
        assert.deepEqual(r.requests.at(-1), { url: target.yamaha + setSoundProgram("7ch_stereo"), reply: false });
    }
});

test("an unknown toggle throws", () => {
    assert.throws(() => yamahaToggle(target, "loudness"), /toggle/);
});

test("toggles: one shared body, under 16 KB, clean in a layout for both targets", async () => {
    const mute = yamahaToggle(target, "mute");
    const seven = yamahaToggle(target, "sevenCh");
    assert.notEqual(mute.lua, seven.lua);
    assert.equal(bodyOf(mute.lua), bodyOf(seven.lua));
    for (const t of Object.values(targets))
    {
        const ops = [yamahaToggle(t, "mute"), yamahaToggle(t, "sevenCh")];
        for (const op of ops)
            assert.ok(Buffer.byteLength(op.lua) < MAX_SCRIPT_BYTES);
        await cleanLayout(homeLayout(ops.map((op, i) => ({ label: `t${i}`, tap: op }))));
    }
});

// ---- encoder volume ----

test("volume: the constants are urem's (5/3/1 under 8 ms/20 ms) and the clamp is 10", () => {
    assert.equal(VOL_FAST_US, 8000);
    assert.equal(VOL_MEDIUM_US, 20000);
    assert.deepEqual(VOL_STEPS, { fast: 5, medium: 3, slow: 1 });
    assert.equal(VOL_MAX_STEP, 10);
});

// Runs the volume script with `param`; -> { requests, volume change }
async function turn(param, y = createFakeYamaha())
{
    const before = y.state.volume;
    const r = await runLua(volumeScript(target).lua, { param, get: yamahaGet(y) });
    assert.equal(r.error, null, param);
    return { requests: r.requests, moved: y.state.volume - before };
}

const vol = (dir, step) => [{ url: target.yamaha + setVolume(dir, step), reply: false }];

test("volume: one fire-and-forget request per event, step by speed and |delta|, clamped", async () => {
    const cases = [
        ["delta=1 interval=30000", "up", 1],
        ["delta=-1 interval=5000", "down", 5],
        ["delta=1 interval=10000", "up", 3],
        ["delta=-1 interval=8000", "down", 3],               // the thresholds are exclusive, as urem's
        ["delta=1 interval=20000", "up", 1],
        ["delta=2 interval=5000", "up", 10],
        ["delta=3 interval=5000", "up", 10],                 // 15, clamped
        ["delta=-4 interval=30000", "down", 4],
        ["delta=2 interval=10000", "up", 6],
    ];
    for (const [param, dir, step] of cases)
    {
        const { requests, moved } = await turn(param);
        assert.deepEqual(requests, vol(dir, step), param);
        assert.equal(moved, dir === "up" ? step : -step, param);
    }
});

test("volume: delta 0, an empty or garbage param sends nothing and raises nothing", async () => {
    for (const param of ["delta=0 interval=5000", "", "garbage", "delta=abc interval=5000", "interval=5000"])
        assert.deepEqual((await turn(param)).requests, [], param);
});

test("volume: a missing interval counts as a slow turn", async () => {
    assert.deepEqual((await turn("delta=-2")).requests, vol("down", 2));
});

test("volume: under 16 KB and clean in a layout for both targets", async () => {
    for (const t of Object.values(targets))
    {
        const op = volumeScript(t);
        assert.ok(Buffer.byteLength(op.lua) < MAX_SCRIPT_BYTES);
        await cleanLayout(homeLayout([{ label: "vol", tap: op }]));
    }
});
