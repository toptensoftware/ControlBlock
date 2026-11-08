// Tests for the Lua prelude helpers and the Lua test harness. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import { luaLiteral, luaSource, luaScript } from "./lua.mjs";
import { evalLua, runLua } from "../test/lua-harness.mjs";
import { UREM2_DIR } from "../test/resolve.mjs";

const TOY = path.join(UREM2_DIR, "test", "toy.lua");

// ---- luaLiteral ----

test("luaLiteral: strings escape quotes, backslashes, newlines and control bytes", () => {
    assert.equal(luaLiteral('a"b'), '"a\\"b"');
    assert.equal(luaLiteral("a\\b"), '"a\\\\b"');
    assert.equal(luaLiteral("a\nb"), '"a\\nb"');
    assert.equal(luaLiteral("100%"), '"100%"');
});

test("luaLiteral: integers, booleans, arrays and string-keyed tables", () => {
    assert.equal(luaLiteral(5), "5");
    assert.equal(luaLiteral(-12), "-12");
    assert.equal(luaLiteral(true), "true");
    assert.equal(luaLiteral([1, "x"]), '{1, "x"}');
    assert.equal(luaLiteral({ a: 1 }), '{["a"] = 1}');
    assert.equal(luaLiteral([]), "{}");
});

test("luaLiteral: anything else throws", () => {
    for (const v of [1.5, NaN, Infinity, null, undefined, () => 1, [null], { a: undefined }, new Date()])
        assert.throws(() => luaLiteral(v), /luaLiteral/, String(v));
});

test("luaLiteral: every literal round-trips through Lua", async () => {
    const values = [
        "", "plain", 'quote " and \\ backslash', "line\nbreak\r\ttab", "pattern %s*:%s*true",
        "digits after escape \u0001" + "23", "utf8 é ✓",
        0, 7, -3, 2 ** 40, true, false,
        [1, 2, 3], ["a", ["b", true]],
        { mute: "/main/setMute?enable=true", nested: { n: 1, list: [5] } },
    ];
    for (const v of values)
        assert.deepEqual(await evalLua(`return ${luaLiteral(v)}`), v, JSON.stringify(v));
    // wasmoon cuts strings at a NUL on the way back to JS, so compare this one inside Lua
    assert.equal(await evalLua(`return ${luaLiteral("nul\0byte")} == "nul" .. string.char(0) .. "byte"`), true);
});

// ---- luaSource / luaScript ----

test("luaSource: one local per const, in the given order, then the body", () => {
    const src = luaSource("return A .. B", { B: "b", A: "a" });
    const locals = src.split("\n").filter(l => l.startsWith("local "));
    assert.deepEqual(locals, ['local B = "b"', 'local A = "a"']);
    assert.ok(src.endsWith("return A .. B"));
    assert.ok(src.indexOf("local A") < src.indexOf("return A .. B"));
});

test("luaSource: bad const names throw", () => {
    assert.throws(() => luaSource("", { "not-a-name": 1 }), /name/);
    assert.throws(() => luaSource("", { end: 1 }), /name/);
    assert.throws(() => luaSource("", { "1x": 1 }), /name/);
});

test("luaScript: a script op whose source is prelude + file, and it compiles and runs", async () => {
    const op = luaScript(TOY, { GREETING: "hi", COUNT: 3 });
    assert.deepEqual(Object.keys(op), ["op", "lua"]);
    assert.equal(op.op, "script");
    assert.ok(op.lua.includes('local GREETING = "hi"'));
    assert.equal(await evalLua(op.lua), "hi 3");
});

test("luaScript: a missing file throws", () => {
    assert.throws(() => luaScript("no-such-script.lua", {}));
});

// ---- the harness ----

test("harness: http.get waits and returns body, status", async () => {
    const r = await runLua(`
        local body, status = http.get("http://h/x")
        log("got", body, status)
        local b2, err = http.get("http://h/none")
        log("none", b2, err)`,
        { get: url => url.endsWith("/x") ? { status: 200, body: "hello" } : null });
    assert.equal(r.error, null);
    assert.deepEqual(r.logs, ["got\thello\t200", "none\tnil\ttimeout"]);
    assert.deepEqual(r.requests, [{ url: "http://h/x", reply: true }, { url: "http://h/none", reply: true }]);
});

test("harness: http.get{ reply = false } is fire and forget, returns true, still reaches the fake", async () => {
    const seen = [];
    const r = await runLua(`log(http.get{ url = "http://h/set", reply = false })`,
                           { get: url => { seen.push(url); return { status: 200, body: "" }; } });
    assert.equal(r.error, null);
    assert.deepEqual(r.logs, ["true"]);
    assert.deepEqual(r.requests, [{ url: "http://h/set", reply: false }]);
    assert.deepEqual(seen, ["http://h/set"]);
});

test("harness: http.get table form with a reply, and a bad url", async () => {
    const r = await runLua(`
        log(http.get{ url = "http://h/t" })
        log(http.get("ftp://x"))`, { get: () => ({ status: 404, body: "no" }) });
    assert.deepEqual(r.logs, ["no\t404", "nil\thttp.get: url must start with http://"]);
});

test("harness: param, print and millis", async () => {
    const r = await runLua(`print(param, type(millis()))`, { param: "delta=1 interval=30000" });
    assert.deepEqual(r.logs, ["delta=1 interval=30000\tnumber"]);
});

test("harness: a Lua error is reported; the device's missing libraries are missing", async () => {
    let r = await runLua(`error("boom")`);
    assert.match(r.error, /boom/);
    r = await runLua(`log(type(load), type(io), type(os), type(require), type(dofile))`);
    assert.deepEqual(r.logs, ["nil\tnil\tnil\tnil\tnil"]);
});
