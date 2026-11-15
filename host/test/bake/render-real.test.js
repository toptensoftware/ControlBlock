// The real renderer (button-face + sharp + rle565 encoder) and the constants shared with the firmware.
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { unpackRle } from "rle565/unpack.js";
import { createRenderer } from "../../src/bake/render-node.js";
import { FACE_MARGIN } from "../../src/images.js";
import {
    KEY_CELL, KEY_COLS, KEY_ROWS, GRID_SLOTS, KEY_QUARTER, MAX_KEYS_PER_PAGE, MAX_KEY_ID, FACE_SIZE, BLOB_VERSION, MAX_CONTROLS,
} from "../../src/bake/consts.js";
import { ok } from "./helpers.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const define = (h, name) => {
    const m = h.match(new RegExp(`#define\\s+${name}\\s+(\\d+)`));
    assert.ok(m, `${name} not found`);
    return Number(m[1]);
};

test("the grid constants match the firmware's key_geometry.h", () => {
    const h = fs.readFileSync(path.join(here, "../../../firmware/main/key_geometry.h"), "utf8");
    assert.equal(KEY_CELL, define(h, "KEY_CELL"));
    assert.equal(KEY_COLS, define(h, "KEY_GRID_COLS"));
    assert.equal(KEY_ROWS, define(h, "KEY_GRID_ROWS"));
    assert.equal(MAX_KEYS_PER_PAGE, define(h, "COMP_MAX_KEYS"));
    assert.match(h, /#define\s+KEY_QUARTER\s+\(KEY_CELL \/ 4\)/);
    assert.equal(KEY_QUARTER, 36);
    assert.equal(GRID_SLOTS, 15);
    assert.equal(MAX_KEY_ID, 65535);
    // the default look: a 128 face in the 144 cell, the host library's margin each side
    assert.equal(FACE_SIZE, 128);
    assert.equal(FACE_SIZE + 2 * FACE_MARGIN, KEY_CELL);
});

test("the blob version matches the firmware's layout_blob.h", () => {
    const h = fs.readFileSync(path.join(here, "../../../firmware/main/layout_blob.h"), "utf8");
    assert.equal(BLOB_VERSION, define(h, "LAYOUT_BLOB_VERSION"));
    assert.equal(BLOB_VERSION, 3);
    assert.equal(MAX_CONTROLS, define(h, "LAYOUT_MAX_CONTROLS"));
});

const decode = buf => {
    assert.ok(Buffer.isBuffer(buf));
    assert.equal(buf.readUInt32LE(0), 0x35363552);        // "R565"
    return unpackRle(buf);
};
const pixel = (img, x, y) => [...img.data.subarray((y * img.width + x) * 4, (y * img.width + x) * 4 + 4)];
const allOpaque = img => { for (let i = 3; i < img.data.length; i += 4) if (img.data[i] !== 255) return false; return true; };

test("the real renderer makes one opaque 144x144 image per state of a 1U key", { timeout: 60000 }, async () => {
    const render = createRenderer();
    const k = ok({ version: 1, home: { keys: [{ color: "#336699", icon: "#heart", label: "Hi" }] } }).home.keys[0];
    const normal = decode(await render(k.normal)), pressed = decode(await render(k.pressed));
    for (const img of [normal, pressed])
    {
        assert.equal(img.width, 144);
        assert.equal(img.height, 144);
        assert.ok(allOpaque(img), "every pixel opaque, as the device requires");
        assert.deepEqual(pixel(img, FACE_MARGIN - 1, 72), [0, 0, 0, 255]);   // black round the face
    }
    assert.notDeepEqual(pixel(normal, 20, 20), [0, 0, 0, 255]);              // the face
    assert.notDeepEqual(pixel(normal, 20, 20), pixel(pressed, 20, 20));      // the pressed face is darker
});

test("a span and a fractional key render at their pixel size", { timeout: 60000 }, async () => {
    const render = createRenderer();
    const keys = ok({ version: 1, home: { keys: [{ w: 2, color: "red", label: "Wide" }, null, { x: 2, y: 0, w: 0.75, h: 0.5, color: "blue" }] } }).home.keys;
    const wide = decode(await render(keys[0].normal)), small = decode(await render(keys[1].normal));
    assert.deepEqual([wide.width, wide.height], [288, 144]);
    assert.deepEqual([small.width, small.height], [108, 72]);
    assert.ok(allOpaque(wide) && allOpaque(small));
    assert.notDeepEqual(pixel(wide, 144, 20), [0, 0, 0, 255]);              // one face across the span
});

test("a group's keys are the parts of one picture, pressed per key", { timeout: 60000 }, async () => {
    const render = createRenderer();
    const [left, right] = ok({ version: 1, home: { keys: [
        { w: 2, color: "#22aa44", label: "Volume", keys: [{ label: "-" }, { x: 1, label: "+" }] },
    ] } }).home.keys;
    const l = decode(await render(left.normal)), r = decode(await render(right.normal));
    const lp = decode(await render(left.pressed));
    assert.deepEqual([l.width, r.width], [144, 144]);
    // the face runs across the seam (no black margin there), and the halves meet seamlessly
    assert.notDeepEqual(pixel(l, 143, 20), [0, 0, 0, 255]);
    assert.notDeepEqual(pixel(r, 0, 20), [0, 0, 0, 255]);
    assert.deepEqual(pixel(l, 143, 20), pixel(r, 0, 20));
    assert.deepEqual(pixel(l, FACE_MARGIN - 1, 20), [0, 0, 0, 255]);         // the group's outer margin
    // the left key pressed: its half darkens
    assert.notDeepEqual(pixel(lp, 143, 20), pixel(l, 143, 20));
});

test("an image override (a button-face definition) renders", { timeout: 60000 }, async () => {
    const render = createRenderer();
    const k = ok({ version: 1, home: { keys: [{ color: "#336699", image: { normalFg: { layers: [
        { type: "svg", src: "#heart", colors: { "#848484": "#ff0000" } },
        { type: "text", text: "Hi", color: "yellow", valign: "bottom" },
    ] } } }] } }).home.keys[0];
    const plain = ok({ version: 1, home: { keys: [{ color: "#336699" }] } }).home.keys[0];
    const img = decode(await render(k.normal)), face = decode(await render(plain.normal));
    assert.ok(allOpaque(img));
    let differs = 0;
    for (let i = 0; i < img.data.length; i += 4)
        if (img.data[i] !== face.data[i] || img.data[i + 1] !== face.data[i + 1] || img.data[i + 2] !== face.data[i + 2])
            differs++;
    assert.ok(differs > 500, `the override draws over the face (${differs} pixels differ)`);
});

test("the same image renders to identical bytes", { timeout: 60000 }, async () => {
    const render = createRenderer();
    const k = ok({ version: 1, home: { keys: [{ label: "Same" }] } }).home.keys[0];
    assert.deepEqual(await render(k.normal), await render(structuredClone(k.normal)));
});
