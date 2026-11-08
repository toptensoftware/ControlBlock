// Tests for the op builders. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { makeOps, IR_GAP_MS } from "./ops.mjs";
import { targets } from "../targets.mjs";
import { setInput, getStatus } from "../tables/yamaha.mjs";
import { HOTPC_MAC } from "../tables/hotpc.mjs";
import { cleanLayout, homeLayout } from "../test/resolve.mjs";

for (const [name, target] of Object.entries(targets))
{
    const ops = makeOps(target);
    const pc = `http://${target.hotpc.host}:${target.hotpc.port}`;
    const show = `http://${target.slideshow.host}:${target.slideshow.httpPort}`;

    test(`${name}: ir sends the irtx packet, byte exact`, () => {
        assert.deepEqual(ops.ir("lg.powerOn"), {
            op: "udpPacket", host: target.irtx.host, port: target.irtx.port,
            data: { hex: "040000004e4543204cb3df200000000000" },
        });
        assert.equal(ops.ir("pana.guide").data.hex.slice(8, 16), "50414e41");      // "PANA"
    });

    test(`${name}: irPower adds the IR gap`, () => {
        assert.deepEqual(ops.irPower("lg.powerOff"), [ops.ir("lg.powerOff"), { op: "delay", ms: IR_GAP_MS }]);
        assert.equal(IR_GAP_MS, 120);
    });

    test(`${name}: Apple TV daemon commands`, () => {
        const atv = text => ({ op: "udpPacket", host: target.atvd.host, port: target.atvd.port, data: text });
        assert.deepEqual(ops.atvKey("menu", "hold"), atv("key menu hold"));
        assert.deepEqual(ops.atvKey("up"), atv("key up"));
        assert.deepEqual(ops.atvApp("Netflix"), atv("app com.netflix.Netflix"));
        assert.deepEqual(ops.atvPing(), atv("ping"));
        assert.deepEqual(ops.atvDisconnect(), atv("disconnect"));
    });

    test(`${name}: yamaha GETs on the target's base URL`, () => {
        assert.deepEqual(ops.yamaha(setInput("hdmi2")),
                         { op: "httpGet", url: target.yamaha + "/main/setInput?input=hdmi2" });
        assert.equal(ops.yamaha(getStatus()).url, target.yamaha + "/main/getStatus");
    });

    test(`${name}: HOT PC posts and wake`, () => {
        assert.deepEqual(ops.hotpc("launch-steam"), {
            op: "httpPost", url: `${pc}/launch-steam`, body: "{}", contentType: "application/json",
        });
        assert.deepEqual(ops.wake(), { op: "sendWol", mac: HOTPC_MAC });
    });

    test(`${name}: slideshow UDP and display-element toggles`, () => {
        assert.deepEqual(ops.slideshow("nextDay"), {
            op: "udpPacket", host: target.slideshow.host, port: target.slideshow.udpPort, data: "nextDay",
        });
        assert.deepEqual(ops.slideshowToggle("date"), {
            op: "httpPost", url: `${show}/display-elements`, body: '{"date":"toggle"}', contentType: "application/json",
        });
    });

    test(`${name}: unknown names throw`, () => {
        assert.throws(() => ops.ir("lg.nope"));
        assert.throws(() => ops.irPower("nope.powerOn"));
        assert.throws(() => ops.atvKey("jump"));
        assert.throws(() => ops.atvKey("menu", "triple"));
        assert.throws(() => ops.atvApp("Kayo"));
        assert.throws(() => ops.yamaha("main/setPower?power=on"), /path/);     // must be a builder's "/main/..." path
        assert.throws(() => ops.hotpc("reboot"), /HOT PC command/);
        assert.throws(() => ops.slideshow("rewind"), /slideshow command/);
        assert.throws(() => ops.slideshowToggle("weather"));
    });

    test(`${name}: a layout using every builder is clean`, async () => {
        const label = text => ({ label: text });
        await cleanLayout(homeLayout([
            { ...label("ir"), tap: ops.ir("pana.guide") },
            { ...label("irPower"), tap: ops.irPower("atv.powerOn") },
            { ...label("key"), tap: ops.atvKey("select", "hold") },
            { ...label("app"), tap: ops.atvApp("YouTube") },
            { ...label("ping"), tap: [ops.atvPing(), ops.atvDisconnect()] },
            { ...label("yamaha"), tap: ops.yamaha(setInput("hdmi1")) },
            { ...label("hotpc"), tap: ops.hotpc("shutdown") },
            { ...label("wake"), tap: ops.wake() },
            { ...label("show"), tap: ops.slideshow("play") },
            { ...label("toggle"), tap: ops.slideshowToggle("all") },
        ]));
    });
}
