import { test } from "node:test";
import assert from "node:assert/strict";
import { opsOf, errorText, errorPaths } from "./helpers.js";
import { OP, HID_MODE } from "../src/consts.js";

const one = (op, extra) => {
    const { r, ops } = opsOf(op, extra);
    assert.equal(r.errors.length, 0, errorText(r));
    assert.equal(ops.length, 1);
    return ops[0];
};

test("an op list may be one op, an array, or empty/null", () => {
    assert.equal(opsOf({ op: "delay", ms: 5 }).ops.length, 1);
    assert.equal(opsOf([{ op: "delay", ms: 5 }, { op: "delay", ms: 6 }]).ops.length, 2);
    // an empty list generates no binding at all
    assert.equal(opsOf([]).ops, undefined);
    assert.equal(opsOf(null).ops, undefined);
});

test("delay", () => {
    assert.deepEqual(one({ op: "delay", ms: 250 }), { op: OP.delay, duration: 250 });
    assert.match(errorText(opsOf({ op: "delay", ms: 0 }).r), /ms/);
    assert.match(errorText(opsOf({ op: "delay", ms: 60001 }).r), /ms/);
    assert.match(errorText(opsOf({ op: "delay" }).r), /ms.*required/);
});

test("switchActivity resolves the name to an index", () => {
    const activities = { A: { pages: [{ keys: [] }] }, B: { pages: [{ keys: [] }] } };
    assert.deepEqual(one({ op: "switchActivity", activity: "B" }, { activities }), { op: OP.switch_activity, index: 1 });
    const r = opsOf({ op: "switchActivity", activity: "Nope" }, { activities }).r;
    assert.match(errorText(r), /Nope/);
});

test("hidKey: chord, default tap mode, hold under press/release", () => {
    assert.deepEqual(one({ op: "hidKey", keys: "ctrl+c" }), { op: OP.hid_key, mode: HID_MODE.tap, modifiers: 1, key: 0x06 });
    assert.equal(one({ op: "hidKey", keys: "a", mode: "hold" }, { gesture: "press" }).mode, HID_MODE.hold);
    assert.equal(one({ op: "hidKey", keys: "a", mode: "hold" }, { gesture: "release" }).mode, HID_MODE.hold);
    assert.equal(one({ op: "hidKey", keys: "a", mode: "down" }, { gesture: "long" }).mode, HID_MODE.down);
    assert.equal(one({ op: "hidKey", keys: "a", mode: "up" }).mode, HID_MODE.up);
});

test("hidKey errors: hold outside press/release, bad key name, bad mode", () => {
    assert.match(errorText(opsOf({ op: "hidKey", keys: "a", mode: "hold" }, { gesture: "tap" }).r), /hold/);
    assert.match(errorText(opsOf({ op: "hidKey", keys: "blorp" }).r), /unknown key/i);
    assert.match(errorText(opsOf({ op: "hidKey", keys: "a", mode: "sideways" }).r), /mode/);
});

test("httpGet", () => {
    assert.deepEqual(one({ op: "httpGet", url: "http://x/y" }), { op: OP.http_get, url: "http://x/y", headers: null });
    assert.equal(one({ op: "httpGet", url: "http://x/", headers: { A: "1", B: "2" } }).headers, "A: 1\r\nB: 2");
});

test("httpPost has default content type and body bytes", () => {
    const o = one({ op: "httpPost", url: "http://x/", body: "a=1" });
    assert.equal(o.op, OP.http_post);
    assert.deepEqual(o.data, Buffer.from("a=1"));
    assert.equal(o.contentType, "application/x-www-form-urlencoded");
    assert.equal(o.headers, null);
    const o2 = one({ op: "httpPost", url: "http://x/", contentType: "application/json" });
    assert.equal(o2.contentType, "application/json");
    assert.equal(o2.data.length, 0);
});

test("http urls must be http://", () => {
    assert.match(errorText(opsOf({ op: "httpGet", url: "https://x/" }).r), /http:\/\//);
    assert.match(errorText(opsOf({ op: "httpGet", url: "ftp://x/" }).r), /http:\/\//);
    assert.match(errorText(opsOf({ op: "httpGet", url: "" }).r), /url/);
});

test("httpGet/httpPost headers must be string values", () => {
    assert.match(errorText(opsOf({ op: "httpGet", url: "http://x/", headers: { A: 1 } }).r), /headers\.A/);
});

test("udpPacket: string or hex data", () => {
    const a = one({ op: "udpPacket", host: "10.0.0.2", port: 9, data: "hi" });
    assert.deepEqual(a, { op: OP.udp_packet, host: "10.0.0.2", port: 9, data: Buffer.from("hi") });
    assert.deepEqual(one({ op: "udpPacket", host: "h", port: 9, data: { hex: "DEADBEEF" } }).data, Buffer.from([0xde, 0xad, 0xbe, 0xef]));
    assert.match(errorText(opsOf({ op: "udpPacket", host: "h", port: 0, data: "" }).r), /port/);
    assert.match(errorText(opsOf({ op: "udpPacket", host: "h", port: 65536, data: "" }).r), /port/);
    assert.match(errorText(opsOf({ op: "udpPacket", host: "h", port: 9, data: { hex: "XYZ" } }).r), /hex/);
    assert.match(errorText(opsOf({ op: "udpPacket", host: "h", port: 9, data: { hex: "ABC" } }).r), /hex/);   // odd length
});

test("sendWol parses the MAC", () => {
    const o = one({ op: "sendWol", mac: "AA:bb:CC:dd:EE:ff" });
    assert.equal(o.op, OP.send_wol);
    assert.deepEqual(o.mac, [0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff]);
    assert.equal(o.broadcast, null);
    assert.deepEqual(one({ op: "sendWol", mac: "aa-bb-cc-dd-ee-ff" }).mac, [0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff]);
    assert.equal(one({ op: "sendWol", mac: "aa:bb:cc:dd:ee:ff", broadcast: "192.168.1.255" }).broadcast, "192.168.1.255");
    assert.match(errorText(opsOf({ op: "sendWol", mac: "aa:bb:cc" }).r), /mac/i);
    assert.match(errorText(opsOf({ op: "sendWol", mac: "gg:bb:cc:dd:ee:ff" }).r), /mac/i);
});

test("script: inline lua or file, exactly one", () => {
    assert.deepEqual(one({ op: "script", lua: "return 1" }), { op: OP.script, source: "return 1" });
    assert.equal(one({ op: "script", file: "s/a.lua" }, { files: { "s/a.lua": "x = 1" } }).source, "x = 1");
    assert.match(errorText(opsOf({ op: "script" }).r), /lua.*file|file.*lua/);
    assert.match(errorText(opsOf({ op: "script", lua: "a", file: "b" }).r), /lua.*file|file.*lua/);
    assert.match(errorText(opsOf({ op: "script", file: "missing.lua" }).r), /missing\.lua/);
});

test("script syntax errors are reported with the path", () => {
    const { r } = opsOf({ op: "script", lua: "SYNTAXERR" });
    assert.match(errorText(r), /home\.keys\[0\]\.tap\.lua: .*unexpected symbol/);
});

test("unknown op code and unknown properties are errors", () => {
    assert.match(errorText(opsOf({ op: "explode" }).r), /unknown op/i);
    assert.match(errorText(opsOf({ op: "delay", ms: 5, extra: 1 }).r), /extra/);
    assert.match(errorText(opsOf({ ms: 5 }).r), /op.*required|missing/i);
    assert.match(errorText(opsOf(5).r), /op/i);
});

test("error paths point into the array", () => {
    const { r } = opsOf([{ op: "delay", ms: 5 }, { op: "delay", ms: -1 }]);
    assert.deepEqual(errorPaths(r), ["home.keys[0].tap[1].ms"]);
});

test("use expands a named action, recursively, inline", () => {
    const actions = {
        a: [{ op: "delay", ms: 1 }, { use: "b" }],
        b: { op: "delay", ms: 2 },
    };
    const { r, ops } = opsOf([{ use: "a" }, { op: "delay", ms: 3 }], { actions });
    assert.equal(r.errors.length, 0, errorText(r));
    assert.deepEqual(ops.map(o => o.duration), [1, 2, 3]);
});

test("use errors: unknown action, cycle", () => {
    assert.match(errorText(opsOf({ use: "nope" }).r), /nope/);
    const actions = { a: { use: "b" }, b: { use: "a" } };
    assert.match(errorText(opsOf({ use: "a" }, { actions }).r), /cycle.*a.*b.*a/i);
});

test("hold check applies at the use site, with the use site's gesture", () => {
    const actions = { h: { op: "hidKey", keys: "a", mode: "hold" } };
    assert.equal(opsOf({ use: "h" }, { actions, gesture: "press" }).r.errors.length, 0);
    assert.match(errorText(opsOf({ use: "h" }, { actions, gesture: "tap" }).r), /hold/);
});

test("errors inside an unused action are still reported", () => {
    const { r } = opsOf(null, { actions: { bad: { op: "delay", ms: -4 } } });
    assert.match(errorText(r), /actions\.bad\.ms/);
});

test("the same error from several uses is reported once", () => {
    const actions = { bad: { op: "delay", ms: -4 } };
    const { r } = opsOf([{ use: "bad" }, { use: "bad" }], { actions });
    assert.equal(r.errors.filter(e => e.path === "actions.bad.ms").length, 1);
});

test("a use object may not have other properties", () => {
    assert.match(errorText(opsOf({ use: "a", ms: 3 }, { actions: { a: [] } }).r), /ms/);
});
