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
