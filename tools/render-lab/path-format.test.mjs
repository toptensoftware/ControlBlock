// The render lab's path format and test vector file (path-format.mjs).
// Run from the repo root: node --test "tools/render-lab/*.test.mjs"
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { CMD, POINT_SCALE, parseSvgPath, encodePath, decodePath, rasterizePath,
         writeVectors, readVectors } from "./path-format.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const VECTORS = path.resolve(here, "../../firmware/test/data/path_vectors.bin");

test("parses absolute M/L/Q/C/Z and refuses anything else", () => {
    const p = parseSvgPath("M1 2L3.5 -4Q5 6 7 8C1e1 11 12 13 14 15Z");
    assert.deepEqual(p.map(c => c.cmd), [CMD.M, CMD.L, CMD.Q, CMD.C, CMD.Z]);
    assert.deepEqual(p[1].pts, [3.5, -4]);
    assert.deepEqual(p[3].pts, [10, 11, 12, 13, 14, 15]);
    assert.throws(() => parseSvgPath("M0 0l1 1"), /unsupported/);
    assert.throws(() => parseSvgPath("M0 0A1 1 0 0 0 2 2"), /unsupported/);
});

test("a path round trips at the fixed point resolution", () => {
    const p = parseSvgPath("M0.03 0L79.97 0.5Q40 40 0 79.9C10 70 20 60 30.06 50ZM-3 -2L5 5L-1 6Z");
    const bin = encodePath(p);
    assert.equal(bin.length % 2, 0);
    const back = decodePath(bin);
    assert.deepEqual(back.map(c => c.cmd), p.map(c => c.cmd));
    for (let i = 0; i < p.length; i++)
        for (let j = 0; j < p[i].pts.length; j++)
            assert.ok(Math.abs(back[i].pts[j] - p[i].pts[j]) <= 0.5 / POINT_SCALE);
    // Decoding what was decoded and re-encoded changes nothing
    assert.deepEqual(encodePath(back), bin);
});

test("points out of the int16 range are refused", () => {
    assert.throws(() => encodePath(parseSvgPath("M0 0L3000 0Z")), /range/);
});

test("a vector file round trips", () => {
    const sq = encodePath(parseSvgPath("M2 2L10 2L10 10L2 10Z"));
    const entries = [
        { name: "square", w: 12, h: 13, path: sq, mask: rasterizePath(decodePath(sq), 12, 13) },
        { name: "x", x: -7, w: 3, h: 1, path: encodePath([]), mask: new Uint8Array(3) },
    ];
    const back = readVectors(writeVectors(entries));
    assert.equal(back.length, 2);
    for (let i = 0; i < 2; i++) {
        assert.equal(back[i].name, entries[i].name);
        assert.equal(back[i].x, entries[i].x ?? 0);
        assert.equal(back[i].w, entries[i].w);
        assert.equal(back[i].h, entries[i].h);
        assert.deepEqual([...back[i].path], [...entries[i].path]);
        assert.deepEqual([...back[i].mask], [...entries[i].mask]);
    }
    // The square covers exactly 2..10 in both directions
    const m = back[0].mask;
    for (let y = 0; y < 13; y++)
        for (let x = 0; x < 12; x++)
            assert.equal(m[y * 12 + x], x >= 2 && x < 10 && y >= 2 && y < 10 ? 255 : 0);
});

test("the checked-in vectors are current and every mask is non-empty", () => {
    const entries = readVectors(fs.readFileSync(VECTORS));
    assert.ok(entries.length >= 10);
    for (const e of entries) {
        assert.equal(e.mask.length, e.w * e.h);
        assert.ok(e.mask.some(v => v > 0), e.name);
        // The reference mask is what the JS rasteriser makes of the stored path
        assert.deepEqual([...rasterizePath(decodePath(e.path), e.w, e.h)], [...e.mask], e.name);
    }
});

test("per-glyph vectors line up with their run", () => {
    // glyph-<run>-<i> boxes sit at x in the run's box, full height; their
    // masks laid side by side add up to the run's mask (within rounding)
    const entries = readVectors(fs.readFileSync(VECTORS));
    const run = entries.find(e => e.name == "glyphs-55%");
    const glyphs = entries.filter(e => e.name.startsWith("glyph-55%-"));
    assert.equal(glyphs.length, 3);
    const sum = new Float64Array(run.w * run.h);
    for (const g of glyphs) {
        assert.equal(g.h, run.h);
        assert.ok(g.x >= 0 && g.x + g.w <= run.w, g.name);
        for (let y = 0; y < g.h; y++)
            for (let x = 0; x < g.w; x++)
                sum[y * run.w + g.x + x] += g.mask[y * g.w + x];
    }
    for (let i = 0; i < sum.length; i++)
        assert.ok(Math.abs(Math.min(255, sum[i]) - run.mask[i]) <= 2, `pixel ${i}`);
});
