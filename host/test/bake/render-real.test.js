// The real renderer (node-buttonImage + sharp + rle565 encoder) and the constants shared with the firmware.
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createRenderer } from "../../src/bake/render-node.js";
import { KEY_SIZE, KEYS_PER_PAGE, KEY_COLS } from "../../src/bake/consts.js";

const here = path.dirname(fileURLToPath(import.meta.url));

test("KEY_SIZE matches the firmware's COMP_KEY_SIZE", () => {
    const h = fs.readFileSync(path.join(here, "../../../firmware/main/page_compositor.h"), "utf8");
    const m = h.match(/#define\s+COMP_KEY_SIZE\s+(\d+)/);
    assert.ok(m, "COMP_KEY_SIZE not found");
    assert.equal(KEY_SIZE, Number(m[1]));
    assert.equal(KEYS_PER_PAGE, 15);
    assert.equal(KEY_COLS, 5);
});

test("the real renderer makes a 128x128 rle565 image with alpha from a face definition", { timeout: 60000 }, async () => {
    const render = createRenderer();
    const face = await render({ backColor: "transparent", svgCode: '<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128"><rect x="8" y="8" width="112" height="112" rx="14" fill="#336699"/></svg>' });
    assert.ok(Buffer.isBuffer(face));
    assert.equal(face.readUInt32LE(0), 0x35363552);        // "R565"
    assert.ok(face.length > 16 && face.length < 8192, `size ${face.length}`);
});

test("the real renderer draws text and built-in icons", { timeout: 60000 }, async () => {
    const render = createRenderer();
    const fg = await render({ backColor: "transparent", svgFile: "#heart", svgForeColor: "red", text: "Hi", textColor: "white", textVAlign: "bottom" });
    assert.equal(fg.readUInt32LE(0), 0x35363552);
    const blank = await render({ backColor: "transparent", text: "" });
    assert.notDeepEqual(fg, blank);
});

test("the same definition renders to identical bytes", { timeout: 60000 }, async () => {
    const render = createRenderer();
    const def = { backColor: "transparent", text: "Same", textColor: "white" };
    assert.deepEqual(await render(def), await render({ ...def }));
});
