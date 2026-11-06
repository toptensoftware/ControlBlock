// Stage 2: image definitions -> canonical rle565 Buffers.
import { test } from "node:test";
import assert from "node:assert/strict";
import { ok, fakeRenderer } from "./helpers.js";
import { renderImages } from "../../src/bake/render.js";

const config = (keys, extra = {}) => ({ version: 1, ...extra, home: { keys } });

test("every image definition becomes a Buffer; absent images stay null", async () => {
    const tree = ok(config([{ color: "red", icon: "#a" }, {}]));
    await renderImages(tree, fakeRenderer());
    const k = tree.home.keys[0];
    for (const f of ["normalBg", "normalFg", "pressedBg", "pressedFg"])
        assert.ok(Buffer.isBuffer(k[f]), f);
    assert.equal(tree.home.keys[1].normalBg, null);
    assert.equal(tree.home.keys[14].pressedFg, null);
});

test("identical definitions render once and share one Buffer object", async () => {
    const tree = ok(config([{ color: "red", icon: "#a" }, { color: "red", icon: "#a" }, { color: "red", icon: "#b" }]));
    const render = fakeRenderer();
    await renderImages(tree, render);
    const [a, b, c] = tree.home.keys;
    assert.strictEqual(a.normalBg, b.normalBg);
    assert.strictEqual(a.pressedFg, b.pressedFg);
    assert.strictEqual(a.normalBg, c.normalBg);       // same face
    assert.notStrictEqual(a.normalFg, c.normalFg);    // different icon
    // icon-only look: pressed foreground is the same definition as the normal one
    assert.strictEqual(a.normalFg, a.pressedFg);
    // distinct definitions: face, pressed face, icon a, icon b = 4 renders
    assert.equal(render.calls.length, 4);
});

test("different definitions that encode to the same bytes still share one Buffer object", async () => {
    const tree = ok(config([{ icon: "#a" }, { icon: "#b" }]));
    // a renderer that ignores the definition
    await renderImages(tree, async () => Buffer.from([1, 2, 3, 4, 5]));
    const [a, b] = tree.home.keys;
    assert.strictEqual(a.normalFg, b.normalFg);
});

test("activity page keys are rendered too", async () => {
    const tree = ok({ version: 1, home: {}, activities: { A: { pages: [{ keys: [{ icon: "#a" }] }, { keys: [{ icon: "#b" }] }] } } });
    await renderImages(tree, fakeRenderer());
    assert.ok(Buffer.isBuffer(tree.activities[0].pages[0].keys[0].normalFg));
    assert.ok(Buffer.isBuffer(tree.activities[0].pages[1].keys[0].normalFg));
});

test("renders run concurrently, up to the limit", async () => {
    const tree = ok(config(Array.from({ length: 10 }, (_, i) => ({ icon: "#i" + i }))));
    let active = 0, peak = 0;
    const render = async def => {
        active++; peak = Math.max(peak, active);
        await new Promise(r => setTimeout(r, 5));
        active--;
        return Buffer.from(JSON.stringify(def));
    };
    await renderImages(tree, render, { concurrency: 3 });
    assert.ok(peak > 1 && peak <= 3, `peak ${peak}`);
});

test("a render failure names the key and layer", async () => {
    const tree = ok(config([{}, { icon: "#a" }]));
    await assert.rejects(renderImages(tree, async () => { throw new Error("no such icon"); }),
        /home\.keys\[1\]\.normalFg.*no such icon/);
});

test("the renderer must return bytes", async () => {
    const tree = ok(config([{ icon: "#a" }]));
    await assert.rejects(renderImages(tree, async () => "text"), /Buffer|bytes/);
    await assert.rejects(renderImages(ok(config([{ icon: "#a" }])), async () => Buffer.alloc(0)), /empty/);
});
