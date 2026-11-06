import { test } from "node:test";
import assert from "node:assert/strict";
import { parseChord, MOD } from "../../src/bake/hid.js";

test("single keys map to HID usage ids", () => {
    assert.deepEqual(parseChord("a"), { modifiers: 0, key: 0x04 });
    assert.deepEqual(parseChord("z"), { modifiers: 0, key: 0x1d });
    assert.deepEqual(parseChord("1"), { modifiers: 0, key: 0x1e });
    assert.deepEqual(parseChord("0"), { modifiers: 0, key: 0x27 });
    assert.deepEqual(parseChord("enter"), { modifiers: 0, key: 0x28 });
    assert.deepEqual(parseChord("esc"), { modifiers: 0, key: 0x29 });
    assert.deepEqual(parseChord("space"), { modifiers: 0, key: 0x2c });
    assert.deepEqual(parseChord("f1"), { modifiers: 0, key: 0x3a });
    assert.deepEqual(parseChord("f5"), { modifiers: 0, key: 0x3e });
    assert.deepEqual(parseChord("f12"), { modifiers: 0, key: 0x45 });
    assert.deepEqual(parseChord("right"), { modifiers: 0, key: 0x4f });
    assert.deepEqual(parseChord("up"), { modifiers: 0, key: 0x52 });
});

test("chords combine modifiers with one key", () => {
    assert.deepEqual(parseChord("ctrl+c"), { modifiers: MOD.lctrl, key: 0x06 });
    assert.deepEqual(parseChord("ctrl+shift+esc"), { modifiers: MOD.lctrl | MOD.lshift, key: 0x29 });
    assert.deepEqual(parseChord("alt+f4"), { modifiers: MOD.lalt, key: 0x3d });
    assert.deepEqual(parseChord("gui+r"), { modifiers: MOD.lgui, key: 0x15 });
    assert.deepEqual(parseChord("rctrl+a"), { modifiers: MOD.rctrl, key: 0x04 });
});

test("names are case insensitive and spaces around + are allowed", () => {
    assert.deepEqual(parseChord("Ctrl + Shift + ESC"), parseChord("ctrl+shift+esc"));
});

test("a modifier alone is allowed (key 0)", () => {
    assert.deepEqual(parseChord("shift"), { modifiers: MOD.lshift, key: 0 });
});

test("punctuation keys by name and by character", () => {
    assert.equal(parseChord("minus").key, 0x2d);
    assert.equal(parseChord("-").key, 0x2d);
    assert.equal(parseChord("plus").key, 0x2e);   // '=' key, shifted; named so '+' stays the separator
    assert.equal(parseChord("comma").key, 0x36);
});

test("errors: unknown name, two keys, empty, repeated modifier", () => {
    assert.match(parseChord("blorp").error, /unknown key/i);
    assert.match(parseChord("a+b").error, /one key/i);
    assert.match(parseChord("").error, /empty/i);
    assert.match(parseChord("ctrl+ctrl+a").error, /repeated|twice/i);
    assert.match(parseChord("ctrl+").error, /empty|unknown/i);
});
