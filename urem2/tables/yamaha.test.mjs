// Tests for yamaha.mjs. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { YAMAHA_API, YAMAHA_INPUTS, YAMAHA_PROGRAMS, setPower, setInput, setVolume, setMute, setSoundProgram, getStatus }
    from "./yamaha.mjs";

test("constants", () => {
    assert.equal(YAMAHA_API, "/YamahaExtendedControl/v1");
    assert.deepEqual(YAMAHA_INPUTS, ["hdmi1", "hdmi2", "hdmi3", "hdmi4"]);
    assert.deepEqual(YAMAHA_PROGRAMS, ["7ch_stereo", "surr_decoder"]);
});

test("each builder's exact path", () => {
    assert.equal(setPower(true), "/main/setPower?power=on");
    assert.equal(setPower(false), "/main/setPower?power=standby");
    assert.equal(setInput("hdmi2"), "/main/setInput?input=hdmi2");
    assert.equal(setVolume("up", 3), "/main/setVolume?volume=up&step=3");
    assert.equal(setVolume("down", 1), "/main/setVolume?volume=down&step=1");
    assert.equal(setVolume("down"), "/main/setVolume?volume=down&step=1");
    assert.equal(setMute(true), "/main/setMute?enable=true");
    assert.equal(setMute(false), "/main/setMute?enable=false");
    assert.equal(setSoundProgram("7ch_stereo"), "/main/setSoundProgram?program=7ch_stereo");
    assert.equal(setSoundProgram("surr_decoder"), "/main/setSoundProgram?program=surr_decoder");
    assert.equal(getStatus(), "/main/getStatus");
});

test("bad arguments throw", () => {
    assert.throws(() => setPower("on"), /bad Yamaha power/);
    assert.throws(() => setInput("hdmi5"), /bad Yamaha input/);
    assert.throws(() => setInput("HDMI1"), /bad Yamaha input/);
    assert.throws(() => setVolume("sideways", 1), /bad Yamaha volume direction/);
    for (const step of [0, -1, 1.5, "3", NaN])
        assert.throws(() => setVolume("up", step), /bad Yamaha volume step/, String(step));
    assert.throws(() => setMute("true"), /bad Yamaha mute/);
    assert.throws(() => setSoundProgram("straight"), /bad Yamaha sound program/);
});
