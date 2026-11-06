import { test } from "node:test";
import assert from "node:assert/strict";
import { Diagnostics, childPath } from "../../src/bake/diag.js";
import { t, required, checkObject } from "../../src/bake/check.js";

test("childPath builds JSON paths", () => {
    assert.equal(childPath("", "home"), "home");
    assert.equal(childPath("home", "keys"), "home.keys");
    assert.equal(childPath("home.keys", 3), "home.keys[3]");
    assert.equal(childPath("", 0), "[0]");
    assert.equal(childPath("activities", "Watch TV"), 'activities["Watch TV"]');
    assert.equal(childPath("a", "b-c"), 'a["b-c"]');
});

test("Diagnostics collects errors and warnings and drops exact duplicates", () => {
    const d = new Diagnostics();
    d.error("a", "bad");
    d.error("a", "bad");
    d.error("b", "bad");
    d.warn("c", "hmm");
    assert.equal(d.errors.length, 2);
    assert.equal(d.warnings.length, 1);
    assert.deepEqual(d.errors[0], { path: "a", message: "bad" });
});

const spec = { n: required(t.integer(1, 10)), s: t.string(), e: t.oneOf("x", "y") };

test("checkObject accepts a valid object", () => {
    const d = new Diagnostics();
    assert.equal(checkObject({ n: 3, s: "hi", e: "x" }, spec, "p", d), true);
    assert.equal(d.errors.length, 0);
});

test("checkObject reports unknown, missing, wrong type and out of range together", () => {
    const d = new Diagnostics();
    assert.equal(checkObject({ s: 4, e: "z", typo: 1 }, spec, "p", d), false);
    const text = d.errors.map(e => `${e.path}: ${e.message}`).join("\n");
    assert.match(text, /p\.n: .*required/);
    assert.match(text, /p\.s: .*string/);
    assert.match(text, /p\.e: .*x, y/);
    assert.match(text, /p\.typo: unknown property/);
});

test("integer bounds and non-integers are errors", () => {
    for (const v of [0, 11, 1.5, "3", NaN])
    {
        const d = new Diagnostics();
        checkObject({ n: v }, spec, "p", d);
        assert.equal(d.errors.length, 1, String(v));
    }
});

test("properties starting with _ or $ are ignored", () => {
    const d = new Diagnostics();
    assert.equal(checkObject({ n: 1, _note: "x", $id: 4 }, spec, "p", d), true);
});

test("a non-object where an object is expected is an error", () => {
    for (const v of [null, 3, "s", [1]])
    {
        const d = new Diagnostics();
        assert.equal(checkObject(v, spec, "p", d), false);
        assert.equal(d.errors.length, 1);
    }
});

test("nested object and array checkers report full paths", () => {
    const d = new Diagnostics();
    const nested = { o: t.object({ a: t.number() }), l: t.array(t.string(), { max: 2 }) };
    checkObject({ o: { a: "x" }, l: ["a", 3, "c"] }, nested, "r", d);
    const text = d.errors.map(e => `${e.path}: ${e.message}`).join("\n");
    assert.match(text, /r\.o\.a/);
    assert.match(text, /r\.l\[1\]/);
    assert.match(text, /r\.l: .*at most 2/);
});

test("null is accepted where nullable", () => {
    const d = new Diagnostics();
    checkObject({ s: null }, { s: t.nullable(t.string()) }, "p", d);
    assert.equal(d.errors.length, 0);
});
