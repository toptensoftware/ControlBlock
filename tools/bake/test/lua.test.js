import { test } from "node:test";
import assert from "node:assert/strict";
import { createLuaChecker } from "../src/lua.js";

test("the Lua checker accepts valid Lua 5.4 and returns null", async () => {
    const check = await createLuaChecker();
    assert.equal(check("return 1"), null);
    assert.equal(check("local n = tonumber(param) // 10\nhttp.post('http://x', 'd=' .. n)"), null);
    assert.equal(check(""), null);
    // Lua 5.4 features: integer division, goto, attribs
    assert.equal(check("local x <const> = 5 // 2; goto done; ::done::"), null);
});

test("the Lua checker reports syntax errors with a line number, and does not run the code", async () => {
    const check = await createLuaChecker();
    assert.match(check("x = = 1"), /script:1:.*unexpected symbol/);
    assert.match(check("local a = 1\nlocal b = ("), /script:2:/);
    // checking must not execute: this would loop forever or throw if it ran
    assert.equal(check("while true do end"), null);
    assert.equal(check("error('boom')"), null);
});

test("calling the checker many times is fine", async () => {
    const check = await createLuaChecker();
    for (let i = 0; i < 200; i++)
        assert.equal(check(`return ${i}`), null);
});

// The device runs Lua 5.4 (the lua/ submodule, v5.4.7) with its default 64-bit integers and doubles,
// like wasmoon: a script that parses here means the same on the device. Only the major.minor can be
// read from wasmoon (_VERSION); the patch level is not visible.
test("the checker's Lua matches the device's: 5.4, 64-bit integers", async () => {
    const { LuaFactory } = await import("wasmoon");
    const lua = await new LuaFactory().createEngine();
    assert.equal(await lua.doString("return _VERSION"), "Lua 5.4");
    assert.equal(await lua.doString("return tostring(math.maxinteger)"), "9223372036854775807");
});
