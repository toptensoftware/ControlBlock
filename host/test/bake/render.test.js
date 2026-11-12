// Stage 2: each key's image specs -> canonical rle565 Buffers.
import { test } from "node:test";
import assert from "node:assert/strict";
import { ok, fakeRenderer } from "./helpers.js";
import { renderImages } from "../../src/bake/render.js";

const layout = (keys, extra = {}) => ({ version: 1, ...extra, home: { keys } });

test("every image spec becomes a Buffer; a key with no look stays without images", async () => {
    const tree = ok(layout([{ color: "red", icon: "#a" }, {}]));
    await renderImages(tree, fakeRenderer());
    const k = tree.home.keys[0];
    for (const f of ["normal", "pressed"])
        assert.ok(Buffer.isBuffer(k[f]), f);
    assert.equal(tree.home.keys[1].normal, null);
    assert.equal(tree.home.keys[1].pressed, null);
});

test("identical specs render once and share one Buffer object", async () => {
    const tree = ok(layout([{ color: "red", icon: "#a" }, { color: "red", icon: "#a" }, { color: "red", icon: "#b" }]));
    const render = fakeRenderer();
    await renderImages(tree, render);
    const [a, b, c] = tree.home.keys;
    assert.strictEqual(a.normal, b.normal);
    assert.strictEqual(a.pressed, b.pressed);
    assert.notStrictEqual(a.normal, c.normal);        // different icon
    assert.notStrictEqual(a.normal, a.pressed);       // the pressed face is darker
    // distinct specs: a normal, a pressed, c normal, c pressed = 4 renders
    assert.equal(render.calls.length, 4);
});

test("the renderer gets the spec: the key's pixel size and its layers", async () => {
    const tree = ok(layout([{ w: 2, color: "red" }]));
    const render = fakeRenderer();
    await renderImages(tree, render);
    assert.deepEqual(render.calls.map(s => [s.width, s.height, s.layers.length]), [[288, 144, 1], [288, 144, 1]]);
});

test("different specs that encode to the same bytes still share one Buffer object", async () => {
    const tree = ok(layout([{ icon: "#a" }, { icon: "#b" }]));
    // a renderer that ignores the spec
    await renderImages(tree, async () => Buffer.from([1, 2, 3, 4, 5]));
    const [a, b] = tree.home.keys;
    assert.strictEqual(a.normal, b.normal);
});

test("activity page keys and group keys are rendered too", async () => {
    const tree = ok({ version: 1, home: { keys: [{ w: 2, color: "red", keys: [{ label: "-" }, { x: 1, label: "+" }] }] },
                      activities: { A: { pages: [{ keys: [{ icon: "#a" }] }, { keys: [{ icon: "#b" }] }] } } });
    await renderImages(tree, fakeRenderer());
    assert.ok(Buffer.isBuffer(tree.home.keys[1].pressed));
    assert.ok(Buffer.isBuffer(tree.activities[0].pages[0].keys[0].normal));
    assert.ok(Buffer.isBuffer(tree.activities[0].pages[1].keys[0].normal));
});

test("renders run concurrently, up to the limit", async () => {
    const tree = ok(layout(Array.from({ length: 10 }, (_, i) => ({ icon: "#i" + i }))));
    let active = 0, peak = 0;
    const render = async spec => {
        active++; peak = Math.max(peak, active);
        await new Promise(r => setTimeout(r, 5));
        active--;
        return Buffer.from(JSON.stringify(spec));
    };
    await renderImages(tree, render, { concurrency: 3 });
    assert.ok(peak > 1 && peak <= 3, `peak ${peak}`);
});

test("a render failure names the key (its path in the layout) and the state", async () => {
    const tree = ok(layout([{}, null, { icon: "#a" }]));
    await assert.rejects(renderImages(tree, async () => { throw new Error("no such icon"); }),
        /home\.keys\[2\]\.normal.*no such icon/);
    const group = ok(layout([{ w: 2, keys: [{}, { x: 1, icon: "#a" }] }]));
    await assert.rejects(renderImages(group, async () => { throw new Error("bad"); }), /home\.keys\[0\]\.keys\[1\]\.normal.*bad/);
});

test("the renderer must return bytes", async () => {
    const tree = ok(layout([{ icon: "#a" }]));
    await assert.rejects(renderImages(tree, async () => "text"), /Buffer|bytes/);
    await assert.rejects(renderImages(ok(layout([{ icon: "#a" }])), async () => Buffer.alloc(0)), /empty/);
});
