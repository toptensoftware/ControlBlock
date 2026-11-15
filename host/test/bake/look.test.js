// Look properties -> one image per key state (docs/layout-format.md, "Button definition"). Each state's
// image is a spec: { width, height, layers: [{ layer: "bg" | "fg", def, x, y, width, height }] }, the
// layers (button-face definitions) drawn in order over black at the key's pixel size.
import { test } from "node:test";
import assert from "node:assert/strict";
import { run, ok, errorText } from "./helpers.js";

// A state's layer definition ("bg" = face, "fg" = icon and label), or null
const layerOf = (spec, layer) => spec?.layers.find(l => l.layer === layer)?.def ?? null;
// A button-face definition's layer of one type ("svg" = icon or face, "text" = label), or undefined
const layerOfType = (def, type) => def?.layers.find(l => l.type === type);
const svgOf = def => layerOfType(def, "svg");
const textOf = def => layerOfType(def, "text");
const faceSvg = def => svgOf(def).svg;      // a face definition's SVG markup
const iconColor = def => svgOf(def).colors["#848484"];   // the icon's colour (built-ins are drawn in #848484)

// Resolve one Home key (plus optional layout extras) and return its layer definitions
const keyOf = (button, extra = {}, files) => {
    const k = ok({ version: 1, ...extra, home: { ...(extra.home ?? {}), keys: [button] } }, files).home.keys[0];
    return { normalBg: layerOf(k.normal, "bg"), normalFg: layerOf(k.normal, "fg"),
             pressedBg: layerOf(k.pressed, "bg"), pressedFg: layerOf(k.pressed, "fg"), longPressMs: k.longPressMs };
};
const specOf = (button) => ok({ version: 1, home: { keys: [button] } }).home.keys[0];

test("a key with no look has no images at all", () => {
    const k = specOf({});
    assert.equal(k.normal, null);
    assert.equal(k.pressed, null);
    assert.equal(k.longPressMs, 0);
});

test("null is an empty slot: no key", () => {
    assert.deepEqual(ok({ version: 1, home: { keys: [null] } }).home.keys, []);
});

test("a 1U key's images are 144x144: the face and the icon/label drawn 128x128 with 8 px of black round them", () => {
    const k = specOf({ color: "red", icon: "#play" });
    for (const spec of [k.normal, k.pressed])
    {
        assert.equal(spec.width, 144);
        assert.equal(spec.height, 144);
        assert.deepEqual(spec.layers.map(l => [l.layer, l.x, l.y, l.width, l.height]), [["bg", 8, 8, 128, 128], ["fg", 8, 8, 128, 128]]);
    }
    assert.match(faceSvg(layerOf(k.normal, "bg")), /width="128" height="128"/);
    assert.equal(layerOf(k.normal, "fg").virtualSize, undefined);     // button-face's default 72x72
});

test("a span or fractional key is drawn across its rect, with the same scale for text as a 1U key", () => {
    const k = specOf({ w: 2, h: 0.5, color: "red", label: "Wide" });
    assert.equal(k.normal.width, 288);
    assert.equal(k.normal.height, 72);
    assert.deepEqual(k.normal.layers.map(l => [l.x, l.y, l.width, l.height]), [[8, 8, 272, 56], [8, 8, 272, 56]]);
    assert.match(faceSvg(layerOf(k.normal, "bg")), /width="272" height="56"/);
    assert.deepEqual(layerOf(k.normal, "fg").virtualSize, { width: 153, height: 31.5 });   // 72 per 128 px both ways
});

test("an icon-only key with nothing to change when pressed has the same layers both ways", () => {
    const k = specOf({ icon: "#play" });
    assert.deepEqual(k.pressed, k.normal);
});

test("color alone makes a face; the pressed face is the same face with the darkening overlay", () => {
    const k = keyOf({ color: "#3a86ff" });
    assert.equal(k.normalFg, null);
    assert.equal(k.pressedFg, null);
    // one svg layer, nothing under it (the background stays transparent)
    assert.deepEqual(k.normalBg.layers.map(l => l.type), ["svg"]);
    assert.match(faceSvg(k.normalBg), /#3a86ff/);
    assert.doesNotMatch(faceSvg(k.normalBg), /pressed-overlay/);
    assert.match(faceSvg(k.pressedBg), /#3a86ff/);
    assert.match(faceSvg(k.pressedBg), /pressed-overlay/);
});

test("the face is a rounded square by default; square has no radius", () => {
    assert.match(faceSvg(keyOf({ color: "red" }).normalBg), /rx="1[0-9]"/);
    assert.match(faceSvg(keyOf({ color: "red", face: "square" }).normalBg), /rx="0"/);
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
    const s = faceSvg(k.normalBg);
    assert.match(s, /#111/);
    assert.match(s, /#222/);
    assert.match(s, /#333/);
    assert.match(s, /rx="20"/);
});

test("icon only: foreground with a built-in icon, white by default", () => {
    const k = keyOf({ icon: "#play_arrow" });
    assert.equal(k.normalBg, null);
    assert.deepEqual(k.normalFg.layers.map(l => l.type), ["svg"]);     // no fill: transparent background, no label
    assert.equal(svgOf(k.normalFg).src, "#play_arrow");
    assert.deepEqual(svgOf(k.normalFg).colors, { "#848484": "white" });
    // nothing to change when pressed: the same definition (so one render, one stored image)
    assert.deepEqual(k.pressedFg, k.normalFg);
});

test("label only is centred; icon plus label puts the label at the bottom and nudges the icon up", () => {
    const lDef = keyOf({ label: "Play" }).normalFg;
    assert.deepEqual(lDef.layers.map(l => l.type), ["text"]);
    const l = textOf(lDef);
    assert.equal(l.text, "Play");
    assert.equal(l.valign, "center");
    assert.equal(l.align, "center");
    assert.equal(l.color, "white");

    const ilDef = keyOf({ icon: "#play", label: "Play" }).normalFg;
    assert.deepEqual(ilDef.layers.map(l => l.type), ["svg", "text"]);   // the icon, then the label over it
    assert.equal(textOf(ilDef).valign, "bottom");
    assert.deepEqual(svgOf(ilDef).translate, { x: 0, y: -7 });
    assert.notEqual(textOf(ilDef).font, l.font);
});

test("icon and label properties map onto button-face svg and text layers", () => {
    const f = keyOf({
        icon: "#tv", iconColor: "red", iconScale: 0.8, iconOffset: { y: 3 }, iconText: "7",
        label: "TV", labelColor: "yellow", labelAlign: "left", labelVAlign: "top",
        font: "italic 9px serif", labelPadding: [1, 2], labelOffset: { x: 4 }, labelLineSpacing: 1.5,
    }).normalFg;
    assert.deepEqual(svgOf(f), {
        type: "svg", src: "#tv", colors: { "#848484": "red" }, scale: 0.8, text: "7", translate: { x: 0, y: 3 },
    });
    assert.deepEqual(textOf(f), {
        type: "text", text: "TV", color: "yellow", align: "left", valign: "top", font: "italic 9px serif",
        padding: [1, 2], translate: { x: 4, y: 0 }, lineSpacing: 1.5,
    });
});

test("an icon file is resolved through the host and must exist", () => {
    const k = keyOf({ icon: "icons/a.svg" }, {}, { "icons/a.svg": "<svg/>" });
    assert.equal(svgOf(k.normalFg).src, "icons/a.svg");
    const r = run({ version: 1, home: { keys: [{ icon: "icons/missing.svg" }] } });
    assert.match(errorText(r), /home\.keys\[0\]\.icon: .*missing\.svg/);
});

test("pressed: merged over the normal look", () => {
    const k = keyOf({ color: "#111111", icon: "#play", iconColor: "white", pressed: { color: "#ff0000", iconColor: "black" } });
    assert.match(faceSvg(k.pressedBg), /#ff0000/);
    assert.doesNotMatch(faceSvg(k.pressedBg), /pressed-overlay/);   // an explicit pressed face is used as it is
    assert.match(faceSvg(k.normalBg), /#111111/);
    assert.equal(iconColor(k.pressedFg), "black");
    assert.equal(iconColor(k.normalFg), "white");
});

test("pressed that only changes the icon still gets the darkened face", () => {
    const k = keyOf({ color: "blue", icon: "#play", pressed: { iconColor: "black" } });
    assert.match(faceSvg(k.pressedBg), /pressed-overlay/);
    assert.equal(iconColor(k.pressedFg), "black");
});

test("pressed.face: none removes the pressed face", () => {
    const k = keyOf({ color: "blue", pressed: { face: "none" } });
    assert.notEqual(k.normalBg, null);
    assert.equal(k.pressedBg, null);
});

test("image: escape hatch replaces a layer with a button-face definition or an .svg path; null removes it", () => {
    const fg = { layers: [{ type: "svg", svg: "<svg/>" }, { type: "text", text: "Hi", color: "red" }] };
    const k = keyOf({
        color: "red", icon: "#play",
        image: { normalFg: fg, pressedBg: null, pressedFg: "pic.svg" },
    }, {}, { "pic.svg": "<svg/>" });
    assert.deepEqual(k.normalFg, fg);                                           // a definition passes through
    assert.equal(k.pressedBg, null);
    assert.deepEqual(k.pressedFg, { layers: [{ type: "svg", src: "pic.svg" }] }); // a path becomes an svg layer
    assert.notEqual(k.normalBg, null);                                          // untouched layers stay generated
});

test("image: a definition's relative svg src is resolved and checked; #built-in and @material are left alone", () => {
    const def = { virtualSize: { width: 72, height: 72 }, layers: [
        { type: "fill", color: "black" },
        { type: "svg", src: "icons/a.svg" },
        { type: "svg", src: "#heart" },
        { type: "svg", src: "@home" },
    ] };
    const k = keyOf({ image: { normalFg: def } }, {}, { "icons/a.svg": "<svg/>" });
    assert.deepEqual(k.normalFg.layers.map(l => l.src), [undefined, "icons/a.svg", "#heart", "@home"]);
    assert.deepEqual(k.normalFg.virtualSize, { width: 72, height: 72 });
    assert.equal(def.layers[1].src, "icons/a.svg");                             // the layout's object isn't modified
    // a missing file names the layer's path
    assert.match(errorText(run({ version: 1, home: { keys: [{ image: { normalFg: def } }] } })),
        /image\.normalFg\.layers\[1\]\.src: file not found: icons\/a\.svg/);
});

test("image: a definition needs a layers array of typed objects; a node-buttonImage settings object is an error", () => {
    const err = image => errorText(run({ version: 1, home: { keys: [{ image: { normalFg: image } }] } }));
    assert.match(err({ svgCode: "<svg/>", textColor: "red" }), /image\.normalFg.*button-face definition/);
    assert.match(err({ layers: "x" }), /image\.normalFg\.layers/);
    assert.match(err({ layers: [{ src: "#heart" }] }), /image\.normalFg\.layers\[0\]/);
    assert.match(err({ layers: [null] }), /image\.normalFg\.layers\[0\]/);
    assert.match(err(42), /an \.svg path, a button-face definition or null/);
});

test("image: only .svg paths are supported for now; unknown layer names are errors", () => {
    assert.match(errorText(run({ version: 1, home: { keys: [{ image: { normalFg: "a.png" } }] } }, { "a.png": "x" })), /svg/i);
    assert.match(errorText(run({ version: 1, home: { keys: [{ image: { middle: null } }] } })), /image\.middle/);
    assert.match(errorText(run({ version: 1, home: { keys: [{ image: { normalFg: "missing.svg" } }] } })), /image\.normalFg: file not found/);
});

test("identical looks give identical definitions (so one render and one stored image)", () => {
    const t = ok({ version: 1, defaults: { color: "#222222" }, home: { keys: [{ icon: "#a" }, { icon: "#b" }, { icon: "#a" }] } });
    const [a, b, c] = t.home.keys;
    assert.deepEqual(layerOf(a.normal, "bg"), layerOf(b.normal, "bg"));
    assert.deepEqual([a.normal, a.pressed], [c.normal, c.pressed]);
    assert.notDeepEqual(layerOf(a.normal, "fg"), layerOf(b.normal, "fg"));
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
    // button-face rejects a zero scale or line spacing at render: caught here, with a path
    bad({ iconScale: 0 }, /home\.keys\[0\]\.iconScale/);
    bad({ labelLineSpacing: 0 }, /home\.keys\[0\]\.labelLineSpacing/);
    bad({ pressed: { iconScale: -1 } }, /home\.keys\[0\]\.pressed\.iconScale/);
    assert.equal(run({ version: 1, home: { keys: [{ icon: "#x", iconScale: 0.01, label: "x", labelLineSpacing: 0.5 }] } }).errors.length, 0);
});

// ---------------------------------------------------------------- inheritance

test("defaults chain: root < home < key", () => {
    const k = keyOf({ icon: "#x" }, { defaults: { iconColor: "red", color: "#010101" }, home: { defaults: { iconColor: "green" } } });
    assert.equal(iconColor(k.normalFg), "green");
    assert.match(faceSvg(k.normalBg), /#010101/);
    assert.equal(iconColor(keyOf({ icon: "#x", iconColor: "blue" }, { defaults: { iconColor: "red" }, home: { defaults: { iconColor: "green" } } }).normalFg), "blue");
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
    assert.equal(iconColor(layerOf(p0.keys[0].normal, "fg")), "page");
    assert.equal(iconColor(layerOf(p0.keys[1].normal, "fg")), "key");
    assert.equal(iconColor(layerOf(p1.keys[0].normal, "fg")), "act");
    assert.match(faceSvg(layerOf(p1.keys[0].normal, "bg")), /#aaaaaa/);
    assert.equal(p1.keys[0].longPressMs, 700);
});

test("objects merge recursively; null clears an inherited value", () => {
    const k = keyOf({ icon: "#x", pressed: { color: "red" } }, { defaults: { pressed: { iconColor: "black" }, iconOffset: { y: 5 } } });
    assert.equal(iconColor(k.pressedFg), "black");   // inherited pressed.iconColor survived
    assert.match(faceSvg(k.pressedBg), /red/);
    assert.equal(svgOf(k.normalFg).translate.y, 5);

    const c = keyOf({ label: "L", icon: null }, { defaults: { icon: "#x" } });
    assert.equal(svgOf(c.normalFg), undefined);
    assert.equal(textOf(c.normalFg).text, "L");
});

test("labels and gestures do not inherit, and are not allowed in defaults", () => {
    assert.match(errorText(run({ version: 1, defaults: { label: "x" }, home: {} })), /defaults\.label/);
    assert.match(errorText(run({ version: 1, defaults: { tap: [] }, home: {} })), /defaults\.tap/);
});

test("empty slots inherit nothing", () => {
    const t = ok({ version: 1, defaults: { color: "red", longPressMs: 900 }, home: { keys: [null] } });
    assert.deepEqual(t.home.keys, []);
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
