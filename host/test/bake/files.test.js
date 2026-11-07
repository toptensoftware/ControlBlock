// The Node file helpers: load a layout file (.json or a JS module) and bake it with its assets
// resolved relative to the file's directory.
import { test } from "node:test";
import assert from "node:assert/strict";
import { loadLayoutFile, bakeFile } from "../../src/bake/files.js";
import { BakeError } from "../../src/bake/bake.js";
import { fakeRenderer } from "./helpers.js";
import { example, exampleDir } from "./fixtures.js";

test("loadLayoutFile reads a .json layout", async () => {
    const { file, cleanup } = exampleDir();
    try {
        assert.deepEqual(await loadLayoutFile(file), example());
    } finally {
        cleanup();
    }
});

test("loadLayoutFile takes a JS module's default export", async () => {
    const { file, cleanup } = exampleDir({ name: "layout.mjs", layout: `export default ${JSON.stringify(example())};` });
    try {
        assert.deepEqual(await loadLayoutFile(file), example());
    } finally {
        cleanup();
    }
});

test("bakeFile finds scripts next to the layout, wherever the cwd is", async () => {
    const { file, cleanup } = exampleDir();
    try {
        const render = fakeRenderer();
        const { blob, tree, warnings } = await bakeFile(file, { renderer: render });
        assert.equal(blob.readUInt32LE(0), 0x4B415042);
        assert.equal(warnings.length, 0);
        assert.equal(tree.activities.length, 2);
        assert.ok(render.calls.length > 0);
    } finally {
        cleanup();
    }
});

test("bakeFile rejects a bad layout with a BakeError", async () => {
    const cfg = example();
    cfg.actions.mute.file = "scripts/missing.lua";
    const { file, cleanup } = exampleDir({ layout: cfg });
    try {
        await assert.rejects(bakeFile(file, { renderer: fakeRenderer() }), err => err instanceof BakeError && /missing\.lua/.test(err.message));
    } finally {
        cleanup();
    }
});
