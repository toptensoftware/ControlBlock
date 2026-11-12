// Every standalone layout in examples/ bakes (as `node cli/cli.js bake` does) and its keys pass the device's checks
// (bake-check.mjs); key-shapes.mjs's ids and rects are as its comment draws them.
// From the repo root:  node --test "examples/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { formatEntries } from "../host/index.js";
import { bakeChecked } from "./bake-check.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));

// layout file -> the warnings it is expected to bake with (formatted)
const LAYOUTS = {
    "upload-test.json": [],
    "net-test.json": [],
    "script-test.mjs": [],
    "key-shapes.mjs": [],
    // the "Hold a, long: ..." keys hold on press and switch on long, with no tap on purpose
    "activity-test.mjs": ["home.keys[7]: has a long gesture but no tap",
                          "activities.Watch.pages[0].keys[9]: has a long gesture but no tap"],
};

for (const [file, expected] of Object.entries(LAYOUTS))
{
    test(`${file} bakes and its keys pass the device's checks`, { timeout: 120000 }, async t => {
        const { blob, warnings, images } = await bakeChecked(path.join(here, file));
        assert.deepEqual(formatEntries(warnings).split("\n").filter(Boolean), expected);
        t.diagnostic(`blob ${blob.length} bytes, ${images.size} distinct images`);
    });
}

test("key-shapes.mjs: spans, the Volume group, fractional keys and a named id", { timeout: 120000 }, async () => {
    const { tree } = await bakeChecked(path.join(here, "key-shapes.mjs"));
    // id: [x, y, w, h] in quarter units. Keys on a whole-unit corner get slot + 1; the two off it (B, R) the first
    // free ids above the 15 slots, in layout order; q names its own.
    assert.deepEqual(Object.fromEntries(tree.home.keys.map(k => [k.id, k.rect])), {
        1: [0, 0, 8, 4],        // Wide: 2U span from slot 0
        3: [8, 0, 4, 4],        // 1U
        4: [12, 0, 4, 4],       // Volume -
        5: [16, 0, 4, 4],       // Volume +
        6: [0, 4, 4, 2],        // A
        16: [0, 6, 4, 2],       // B
        7: [4, 4, 8, 8],        // Big
        9: [12, 4, 2, 4],       // L
        17: [14, 4, 2, 4],      // R
        10: [16, 4, 4, 8],      // Tall
        11: [0, 8, 4, 4],       // Esc
        500: [13, 9, 2, 2],     // q
    });
    // one binding per key (its tap), by id
    assert.deepEqual(tree.home.bindings.map(b => `${b.control} ${b.event}`),
                     tree.home.keys.map(k => `touch[${k.id}] tap`));
    // the group's keys are different parts of one picture, and press alone
    const [minus, plus] = [4, 5].map(id => tree.home.keys.find(k => k.id === id));
    assert.notDeepEqual(minus.normal, plus.normal);
    assert.notDeepEqual(minus.normal, minus.pressed);
});
