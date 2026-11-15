// Tests for the look helpers and the root defaults (border face, grey pressed fill).
// From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { ROOT_DEFAULTS, COLORS, ICON_SCALE, BORDER, PRESSED, icon, label } from "./look.mjs";
import { cleanLayout } from "../test/resolve.mjs";

// A Home of `keys` under the root defaults -> each resolved key's layer definitions (face "bg",
// icon and label "fg") per state. Empty slots have no key.
const layerOf = (spec, layer) => spec?.layers.find(l => l.layer === layer)?.def ?? null;
// A button-face definition's layer of one type ("svg": the face or icon, "text": the label)
const layerOfType = (def, type) => def?.layers.find(l => l.type === type);
const faceSvg = def => layerOfType(def, "svg").svg;
async function resolveKeys(keys)
{
    const tree = await cleanLayout({ version: 1, defaults: ROOT_DEFAULTS, home: { keys } });
    return tree.home.keys.map(k => ({ normalBg: layerOf(k.normal, "bg"), normalFg: layerOf(k.normal, "fg"),
                                      pressedBg: layerOf(k.pressed, "bg"), pressedFg: layerOf(k.pressed, "fg") }));
}

test("icon multiplies ICON_SCALE into the key's scale", () => {
    assert.equal(ICON_SCALE, 0.9);
    assert.equal(icon("#play", { scale: 0.7 }).iconScale, 0.63);
    assert.equal(icon("#play").iconScale, 0.9);
    assert.deepEqual(icon("#square", { color: COLORS.red, scale: 0.6 }),
                     { icon: "#square", iconColor: "red", iconScale: 0.54 });
});

test("icon and label carry urem's colour, offset, font and alignment", () => {
    assert.deepEqual(icon("#refresh", { color: COLORS.orange, scale: 0.7, offset: { y: -10 } }),
                     { icon: "#refresh", iconColor: "orange", iconScale: 0.63, iconOffset: { x: 0, y: -10 } });
    assert.deepEqual(label("Record", { valign: "bottom", offset: { y: -6 } }),
                     { label: "Record", labelVAlign: "bottom", labelOffset: { x: 0, y: -6 } });
    assert.deepEqual(label("1", { font: "bold 24px sans-serif", offset: { y: -3 } }),
                     { label: "1", font: "bold 24px sans-serif", labelOffset: { x: 0, y: -3 } });
    assert.deepEqual(label("7ch Stereo", { color: COLORS.purple }), { label: "7ch Stereo", labelColor: "purple" });
    assert.equal(COLORS.grey, "#606060");
});

test("look helpers refuse typos", () => {
    assert.throws(() => icon("#play", { colour: "orange" }), /unknown icon option/);
    assert.throws(() => icon("#play", { color: "ornage" }), /unknown colour/);
    assert.throws(() => icon("play"), /icon/);                       // neither "#name" nor an .svg path
    assert.throws(() => label("x", { valign: "middle" }), /valign/);
    assert.throws(() => label("x", { size: 3 }), /unknown label option/);
    assert.throws(() => label(""), /label/);
});

const BLACK_FILL = /stop-color="#000000"/;
const borderStroke = new RegExp(`stroke="${BORDER}" stroke-width="2"`);

test("an icon key gets the border face and the grey pressed fill, no overlay", async () => {
    const [k] = await resolveKeys([{ ...icon("#play", { color: COLORS.orange }), tap: [] }]);
    assert.match(faceSvg(k.normalBg), borderStroke);
    assert.match(faceSvg(k.normalBg), BLACK_FILL);
    assert.doesNotMatch(faceSvg(k.normalBg), /pressed-overlay/);
    assert.match(faceSvg(k.pressedBg), borderStroke);
    assert.match(faceSvg(k.pressedBg), new RegExp(`stop-color="${PRESSED}"`));
    assert.doesNotMatch(faceSvg(k.pressedBg), BLACK_FILL);
    assert.doesNotMatch(faceSvg(k.pressedBg), /pressed-overlay/);
    assert.equal(layerOfType(k.normalFg, "svg").scale, 0.9);
    assert.deepEqual(layerOfType(k.normalFg, "svg").colors, { "#848484": "orange" });
});

test("a label-only key gets the same face; an empty slot has no key", async () => {
    const keys = await resolveKeys([
        icon("#play"), label("Steam"), null,
    ]);
    const [iconKey, labelKey] = keys;
    assert.equal(keys.length, 2);
    assert.equal(faceSvg(labelKey.normalBg), faceSvg(iconKey.normalBg));
    assert.equal(faceSvg(labelKey.pressedBg), faceSvg(iconKey.pressedBg));
    assert.equal(layerOfType(labelKey.normalFg, "text").text, "Steam");
});
