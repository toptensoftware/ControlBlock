// End to end: the example in docs/layout-format.md through all three stages, with the real
// Lua checker and a fake renderer (the cli's own tests, with the real renderer, are in cli/test).
import { test } from "node:test";
import assert from "node:assert/strict";
import { unpack } from "../../../binpack/index.js";
import { bake, BakeError } from "../../src/bake/bake.js";
import { createLuaChecker } from "../../src/bake/lua.js";
import { ROOT_TYPE } from "../../src/bake/pack.js";
import { fakeRenderer, fakeHost } from "./helpers.js";
import { example, files } from "./fixtures.js";

const makeHost = async () => fakeHost(files, { luaCheck: await createLuaChecker() });

test("the docs example bakes to a valid BPAK file", async () => {
    const { blob, tree, warnings } = await bake(example(), await makeHost(), fakeRenderer());
    assert.equal(blob.readUInt32LE(0), 0x4B415042);
    assert.equal(warnings.length, 0, JSON.stringify(warnings));

    // Mute is touch[9,0], back is touch[5,1]
    const controls = tree.activities[0].bindings.map(b => `${b.control}/${b.event}`);
    assert.deepEqual(controls, ["touch[5,0]/tap", "touch[6,0]/tap", "touch[9,0]/tap", "touch[5,1]/tap", "touch[5,1]/long",
                                "macro1/tap", "volEnc/turnLeft", "volEnc/turnRight"]);
});

test("the blob reads back (strip the 32-byte header, unpack at offset 32)", async () => {
    const { blob } = await bake(example(), await makeHost(), fakeRenderer());
    const count = blob.readUInt32LE(8), tableOffset = blob.readUInt32LE(12);
    assert.ok(count > 0 && tableOffset % 4 === 0 && tableOffset < blob.length);
    const root = unpack(ROOT_TYPE, blob, 32);   // pointers are file-relative in the combined format
    assert.equal(root.activities.length, 2);
    assert.equal(root.activities[0].name, "Watch TV");
    assert.equal(root.activities[0].pages[0].keys[0].longPressMs, 0);
    assert.equal(root.longPressMs, 600);
    // the shared face and the shared pressed face are single stored copies across all 4 keys that use them
    assert.ok(blob.length < 20000, `blob is ${blob.length} bytes`);
});

test("bake is deterministic: same input, same bytes", async () => {
    const a = (await bake(example(), await makeHost(), fakeRenderer())).blob;
    const b = (await bake(example(), await makeHost(), fakeRenderer())).blob;
    assert.deepEqual(a, b);
});

test("errors throw a BakeError listing every problem, and nothing is rendered", async () => {
    const cfg = example();
    cfg.activities["Watch TV"].devices = ["ghost"];
    cfg.home.keys[0].iconScale = "x";
    const render = fakeRenderer();
    await assert.rejects(bake(cfg, await makeHost(), render), err => {
        assert.ok(err instanceof BakeError);
        assert.equal(err.errors.length, 2);
        assert.match(err.message, /ghost/);
        assert.match(err.message, /iconScale/);
        return true;
    });
    assert.equal(render.calls.length, 0);
});

test("a real Lua syntax error is caught", async () => {
    const f = { ...files, "scripts/mute.lua": "local x = = 1" };
    const host = fakeHost(f, { luaCheck: await createLuaChecker() });
    await assert.rejects(bake(example(), host, fakeRenderer()), /scripts\/mute\.lua|script:1/);
});
