// Look properties -> the four image definitions per key (docs/config-format.md, "Button definition").
import { test } from "node:test";
import assert from "node:assert/strict";
import { run, ok, errorText } from "./helpers.js";

// Resolve one Home key (plus optional config extras) and return that key
const keyOf = (button, extra = {}, files) =>
    ok({ version: 1, ...extra, home: { ...(extra.home ?? {}), keys: [button] } }, files).home.keys[0];

test("a key with no look has no images at all", () => {
    const k = keyOf({});
    assert.equal(k.normalBg, null);
    assert.equal(k.normalFg, null);
    assert.equal(k.pressedBg, null);
    assert.equal(k.pressedFg, null);
    assert.equal(k.longPressMs, 0);
});

test("an empty slot has no images", () => {
    const t = ok({ version: 1, home: { keys: [null] } });
    assert.deepEqual(t.home.keys[0], { normalBg: null, normalFg: null, pressedBg: null, pressedFg: null, longPressMs: 0 });
});

test("color alone makes a face; the pressed face is the same face with the darkening overlay", () => {
    const k = keyOf({ color: "#3a86ff" });
    assert.equal(k.normalFg, null);
    assert.equal(k.pressedFg, null);
    assert.equal(k.normalBg.backColor, "transparent");
    assert.match(k.normalBg.svgCode, /#3a86ff/);
    assert.doesNotMatch(k.normalBg.svgCode, /pressed-overlay/);
    assert.match(k.pressedBg.svgCode, /#3a86ff/);
    assert.match(k.pressedBg.svgCode, /pressed-overlay/);
});

test("the face is a rounded square by default; square has no radius", () => {
    assert.match(keyOf({ color: "red" }).normalBg.svgCode, /rx="1[0-9]"/);
    assert.match(keyOf({ color: "red", face: "square" }).normalBg.svgCode, /rx="0"/);
});

test("face: none removes the face even with a color", () => {
    const k = keyOf({ color: "red", face: "none", icon: "#play" });
    assert.equal(k.normalBg, null);
    assert.equal(k.pressedBg, null);
    assert.notEqual(k.normalFg, null);
});

test("face: a shape name with no color uses the built-in face color", () => {
    const k = keyOf({ face: "rounded" });
    assert.notEqual(k.normalBg, null);
});

test("face object: top, bottom, border, radius, padding", () => {
    const k = keyOf({ face: { top: "#111", bottom: "#222", border: "#333", radius: 20, padding: 6 } });
    const s = k.normalBg.svgCode;
    assert.match(s, /#111/);
    assert.match(s, /#222/);
    assert.match(s, /#333/);
    assert.match(s, /rx="20"/);
});

test("icon only: foreground with a built-in icon, white by default", () => {
    const k = keyOf({ icon: "#play_arrow" });
    assert.equal(k.normalBg, null);
    assert.equal(k.normalFg.svgFile, "#play_arrow");
    assert.equal(k.normalFg.svgForeColor, "white");
    assert.equal(k.normalFg.backColor, "transparent");
    assert.equal(k.normalFg.text, undefined);
    // nothing to change when pressed: the same definition (so one render, one stored image)
    assert.deepEqual(k.pressedFg, k.normalFg);
});

test("label only is centred; icon plus label puts the label at the bottom and nudges the icon up", () => {
    const l = keyOf({ label: "Play" }).normalFg;
    assert.equal(l.text, "Play");
    assert.equal(l.textVAlign, "center");
    assert.equal(l.textAlign, "center");
    assert.equal(l.textColor, "white");
    assert.equal(l.svgFile, undefined);

    const il = keyOf({ icon: "#play", label: "Play" }).normalFg;
    assert.equal(il.textVAlign, "bottom");
    assert.deepEqual(il.svgTranslate, { x: 0, y: -7 });
    assert.notEqual(il.font, l.font);
});

test("icon and label properties map onto node-buttonImage settings", () => {
    const f = keyOf({
        icon: "#tv", iconColor: "red", iconScale: 0.8, iconOffset: { y: 3 }, iconText: "7",
        label: "TV", labelColor: "yellow", labelAlign: "left", labelVAlign: "top",
        font: "italic 9px serif", labelPadding: [1, 2], labelOffset: { x: 4 }, labelLineSpacing: 1.5,
    }).normalFg;
    assert.equal(f.svgForeColor, "red");
    assert.equal(f.svgScale, 0.8);
    assert.deepEqual(f.svgTranslate, { x: 0, y: 3 });
    assert.equal(f.svgText, "7");
    assert.equal(f.textColor, "yellow");
    assert.equal(f.textAlign, "left");
    assert.equal(f.textVAlign, "top");
    assert.equal(f.font, "italic 9px serif");
    assert.deepEqual(f.textPadding, [1, 2]);
    assert.deepEqual(f.textTranslate, { x: 4, y: 0 });
    assert.equal(f.textLineSpacing, 1.5);
});

test("an icon file is resolved through the host and must exist", () => {
    const k = keyOf({ icon: "icons/a.svg" }, {}, { "icons/a.svg": "<svg/>" });
    assert.equal(k.normalFg.svgFile, "icons/a.svg");
    const r = run({ version: 1, home: { keys: [{ icon: "icons/missing.svg" }] } });
    assert.match(errorText(r), /home\.keys\[0\]\.icon: .*missing\.svg/);
});

test("pressed: merged over the normal look", () => {
    const k = keyOf({ color: "#111111", icon: "#play", iconColor: "white", pressed: { color: "#ff0000", iconColor: "black" } });
    assert.match(k.pressedBg.svgCode, /#ff0000/);
    assert.doesNotMatch(k.pressedBg.svgCode, /pressed-overlay/);   // an explicit pressed face is used as it is
    assert.match(k.normalBg.svgCode, /#111111/);
    assert.equal(k.pressedFg.svgForeColor, "black");
    assert.equal(k.normalFg.svgForeColor, "white");
});

test("pressed that only changes the icon still gets the darkened face", () => {
    const k = keyOf({ color: "blue", icon: "#play", pressed: { iconColor: "black" } });
    assert.match(k.pressedBg.svgCode, /pressed-overlay/);
    assert.equal(k.pressedFg.svgForeColor, "black");
});

test("pressed.face: none removes the pressed face", () => {
    const k = keyOf({ color: "blue", pressed: { face: "none" } });
    assert.notEqual(k.normalBg, null);
    assert.equal(k.pressedBg, null);
});

test("image: escape hatch replaces a layer; null removes it", () => {
    const k = keyOf({
        color: "red", icon: "#play",
        image: { normalFg: { svgCode: "<svg/>", textColor: "red" }, pressedBg: null, pressedFg: "pic.svg" },
    }, {}, { "pic.svg": "<svg/>" });
    assert.equal(k.normalFg.svgCode, "<svg/>");
    assert.equal(k.normalFg.backColor, "transparent");   // layer defaults still apply
    assert.equal(k.pressedBg, null);
    assert.equal(k.pressedFg.svgFile, "pic.svg");
    assert.notEqual(k.normalBg, null);                    // untouched layers stay generated
});

test("image: only .svg paths are supported for now; unknown layer names are errors", () => {
    assert.match(errorText(run({ version: 1, home: { keys: [{ image: { normalFg: "a.png" } }] } }, { "a.png": "x" })), /svg/i);
    assert.match(errorText(run({ version: 1, home: { keys: [{ image: { middle: null } }] } })), /image\.middle/);
});

test("identical looks give identical definitions (so one render and one stored image)", () => {
    const t = ok({ version: 1, defaults: { color: "#222222" }, home: { keys: [{ icon: "#a" }, { icon: "#b" }, { icon: "#a" }] } });
    const [a, b, c] = t.home.keys;
    assert.deepEqual(a.normalBg, b.normalBg);
    assert.deepEqual(a, c);
    assert.notDeepEqual(a.normalFg, b.normalFg);
});

test("look property validation", () => {
    const bad = (button, re) => assert.match(errorText(run({ version: 1, home: { keys: [button] } })), re);
    bad({ iconScale: "big" }, /home\.keys\[0\]\.iconScale/);
    bad({ labelAlign: "middle" }, /labelAlign/);
    bad({ labelVAlign: "middle" }, /labelVAlign/);
    bad({ iconOffset: { z: 1 } }, /iconOffset\.z/);
    bad({ face: "round" }, /face/);
    bad({ face: { colour: "red" } }, /face\.colour/);
    bad({ labelPadding: [1, 2, 3] }, /labelPadding/);
    bad({ pressed: { pressed: {} } }, /pressed\.pressed/);
    bad({ pressed: { tap: {} } }, /pressed\.tap/);
    bad({ color: 5 }, /color/);
});

// ---------------------------------------------------------------- inheritance

test("defaults chain: root < home < key", () => {
    const k = keyOf({ icon: "#x" }, { defaults: { iconColor: "red", color: "#010101" }, home: { defaults: { iconColor: "green" } } });
    assert.equal(k.normalFg.svgForeColor, "green");
    assert.match(k.normalBg.svgCode, /#010101/);
    assert.equal(keyOf({ icon: "#x", iconColor: "blue" }, { defaults: { iconColor: "red" }, home: { defaults: { iconColor: "green" } } }).normalFg.svgForeColor, "blue");
});

test("defaults chain for activities: root < activity < page < key", () => {
    const t = ok({
        version: 1,
        defaults: { iconColor: "root", color: "#aaaaaa" },
        home: { keys: [] },
        activities: { A: {
            defaults: { iconColor: "act", longPressMs: 700 },
            pages: [
                { defaults: { iconColor: "page" }, keys: [{ icon: "#x" }, { icon: "#x", iconColor: "key" }] },
                { keys: [{ icon: "#x" }] },
            ] } },
    });
    const [p0, p1] = t.activities[0].pages;
    assert.equal(p0.keys[0].normalFg.svgForeColor, "page");
    assert.equal(p0.keys[1].normalFg.svgForeColor, "key");
    assert.equal(p1.keys[0].normalFg.svgForeColor, "act");
    assert.match(p1.keys[0].normalBg.svgCode, /#aaaaaa/);
    assert.equal(p1.keys[0].longPressMs, 700);
});

test("objects merge recursively; null clears an inherited value", () => {
    const k = keyOf({ icon: "#x", pressed: { color: "red" } }, { defaults: { pressed: { iconColor: "black" }, iconOffset: { y: 5 } } });
    assert.equal(k.pressedFg.svgForeColor, "black");   // inherited pressed.iconColor survived
    assert.match(k.pressedBg.svgCode, /red/);
    assert.equal(k.normalFg.svgTranslate.y, 5);

    const c = keyOf({ label: "L", icon: null }, { defaults: { icon: "#x" } });
    assert.equal(c.normalFg.svgFile, undefined);
    assert.equal(c.normalFg.text, "L");
});

test("labels and gestures do not inherit, and are not allowed in defaults", () => {
    assert.match(errorText(run({ version: 1, defaults: { label: "x" }, home: {} })), /defaults\.label/);
    assert.match(errorText(run({ version: 1, defaults: { tap: [] }, home: {} })), /defaults\.tap/);
});

test("empty slots inherit nothing", () => {
    const t = ok({ version: 1, defaults: { color: "red", longPressMs: 900 }, home: { keys: [null] } });
    assert.equal(t.home.keys[0].normalBg, null);
    assert.equal(t.home.keys[0].longPressMs, 0);
});

test("inherited face with no icon gives the face only", () => {
    const k = keyOf({}, { defaults: { color: "red" } });
    assert.notEqual(k.normalBg, null);
    assert.equal(k.normalFg, null);
});

test("a built-in icon the host doesn't have is an error (when the host can tell)", () => {
    const host = { icons: ["play", "heart"] };
    const bad = run({ version: 1, home: { keys: [{ icon: "#play" }, { icon: "#tv" }, { label: "x", pressed: { icon: "#nope" } }] } }, {}, host);
    assert.match(errorText(bad), /home\.keys\[1\]\.icon: .*#tv/);
    assert.match(errorText(bad), /home\.keys\[2\]\.pressed\.icon: .*#nope/);
    assert.equal(bad.errors.length, 2);
    // a host that can't tell (no icon list) accepts anything
    assert.equal(run({ version: 1, home: { keys: [{ icon: "#anything" }] } }).errors.length, 0);
});
