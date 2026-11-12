// Page previews (src/bake/preview.js): each page of a resolved layout drawn as the screen would
// show it, with the real renderer.
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import sharp from "sharp";
import { renderPages, encodePng } from "../../src/bake/preview.js";
import { previewFile } from "../../src/bake/files.js";
import { createPixelRenderer } from "../../src/bake/render-node.js";
import { BakeError } from "../../src/bake/bake.js";
import {
    SCREEN_W, SCREEN_H, SCREEN_STATUS_H, KEY_GRID_X0, KEY_GRID_Y0, KEY_QUARTER, KEY_CELL, KEY_COLS, KEY_ROWS,
} from "../../src/bake/consts.js";
import { example, exampleKeys, files, exampleDir } from "./fixtures.js";
import { ok } from "./helpers.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const T = { timeout: 120000 };

const pixel = (p, x, y) => [...p.rgba.subarray((y * p.width + x) * 4, (y * p.width + x) * 4 + 4)];
const BLACK = [0, 0, 0, 255];

// A key's rect in screen pixels
const screenRect = key => ({
    x: KEY_GRID_X0 + key.rect[0] * KEY_QUARTER, y: SCREEN_STATUS_H + KEY_GRID_Y0 + key.rect[1] * KEY_QUARTER,
    w: key.rect[2] * KEY_QUARTER, h: key.rect[3] * KEY_QUARTER,
});

test("preview: the screen geometry matches the firmware's screen_layout.h and key_geometry.h", () => {
    const h = fs.readFileSync(path.join(here, "../../../firmware/main/screen_layout.h"), "utf8");
    const define = name => Number(h.match(new RegExp(`#define\\s+${name}\\s+(\\d+)`))[1]);
    assert.equal(SCREEN_W, define("SCREEN_W"));
    assert.equal(SCREEN_H, define("SCREEN_H"));
    assert.equal(SCREEN_STATUS_H, define("SCREEN_STATUS_H"));
    // key_geometry.h: the grid centred in the page area (below the status bar)
    assert.equal(KEY_GRID_X0, (SCREEN_W - KEY_COLS * KEY_CELL) / 2);
    assert.equal(KEY_GRID_Y0, (SCREEN_H - SCREEN_STATUS_H - KEY_ROWS * KEY_CELL) / 2);
});

test("preview: one 800x480 page per page of the layout, Home first", T, async () => {
    const pages = await renderPages(ok(example(), files));
    assert.deepEqual(pages.map(p => [p.name, p.activity, p.page]), [["home", null, 0], ["Watch TV-1", "Watch TV", 0], ["Listen-1", "Listen", 0]]);
    assert.deepEqual(pages.map(p => p.title), ["Home", "TV", "Listen"]);
    for (const p of pages)
    {
        assert.deepEqual([p.width, p.height], [SCREEN_W, SCREEN_H]);
        assert.equal(p.rgba.length, SCREEN_W * SCREEN_H * 4);
    }
});

test("preview: each key's image sits at its rect, pixel for pixel", T, async () => {
    const tree = ok(exampleKeys());
    const [home] = await renderPages(tree);
    const pixels = createPixelRenderer();
    for (const key of tree.home.keys)
    {
        const img = await pixels(key.normal), r = screenRect(key);
        assert.equal(img.length, r.w * r.h * 4);
        for (const [x, y] of [[0, 0], [r.w >> 1, r.h >> 1], [r.w - 1, r.h - 1], [20, r.h >> 2]])
            assert.deepEqual(pixel(home, r.x + x, r.y + y), [...img.subarray((y * r.w + x) * 4, (y * r.w + x) * 4 + 4)], `key ${key.id} at ${x},${y}`);
    }
});

test("preview: without the grid an empty slot is black; with it, the grid shows there", T, async () => {
    // slot 1 (column 1, row 0) is empty: a dashed line runs along its top edge
    const tree = ok({ version: 1, home: { keys: [{ label: "A" }] } });
    const x = KEY_GRID_X0 + KEY_CELL + 2, y = SCREEN_STATUS_H + KEY_GRID_Y0;     // on the grid's top line, in a dash
    const [plain] = await renderPages(tree, { grid: false });
    const [grid] = await renderPages(tree);
    for (let dy = 0; dy < KEY_CELL; dy += 7)
        for (let dx = 0; dx < KEY_CELL; dx += 7)
            assert.deepEqual(pixel(plain, KEY_GRID_X0 + KEY_CELL + dx, y + dy), BLACK);
    assert.notDeepEqual(pixel(grid, x, y), BLACK);
    // the status bar is drawn either way
    assert.notDeepEqual(pixel(plain, SCREEN_W - 2, 2), BLACK);
});

test("preview: the pressed state shows the pressed look", T, async () => {
    const tree = ok(example(), files);
    const [normal] = await renderPages(tree, { page: "home" });
    const [pressed] = await renderPages(tree, { page: "home", state: "pressed" });
    const r = screenRect(tree.home.keys[1]);      // the default pressed colour (the first key has its own colour)
    assert.notDeepEqual(pixel(normal, r.x + 20, r.y + 20), pixel(pressed, r.x + 20, r.y + 20));
});

test("preview: the page filter picks Home or one activity's pages; an unknown one throws", T, async () => {
    const tree = ok(example(), files);
    assert.deepEqual((await renderPages(tree, { page: "home" })).map(p => p.name), ["home"]);
    assert.deepEqual((await renderPages(tree, { page: "Listen" })).map(p => p.name), ["Listen-1"]);
    await assert.rejects(renderPages(tree, { page: "Nope" }), /no page or activity "Nope"/);
    await assert.rejects(renderPages(tree, { state: "hover" }), /state/);
});

test("preview: encodePng gives a PNG of the page", T, async () => {
    const [p] = await renderPages(ok(exampleKeys()), { grid: false });
    const png = await encodePng(p);
    assert.ok(Buffer.isBuffer(png));
    const meta = await sharp(png).metadata();
    assert.deepEqual([meta.format, meta.width, meta.height], ["png", SCREEN_W, SCREEN_H]);
});

test("preview: previewFile resolves a layout file; a bad layout throws a BakeError", T, async () => {
    const { file, cleanup } = exampleDir();
    try {
        const { pages, warnings } = await previewFile(file, { page: "Watch TV" });
        assert.deepEqual(pages.map(p => p.name), ["Watch TV-1"]);
        assert.ok(Array.isArray(warnings));
    } finally {
        cleanup();
    }
    const bad = exampleDir({ layout: { version: 1, home: { keys: [{ iconScale: "x" }] } } });
    try {
        await assert.rejects(previewFile(bad.file), e => e instanceof BakeError && /iconScale/.test(e.message));
    } finally {
        bad.cleanup();
    }
});
