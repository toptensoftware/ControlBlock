// The full bake (resolve, render every key image, pack) of the test entry, as the CLI does it. Logs the blob size
// and the number of distinct images. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import { bakeFile, formatEntries } from "../../host/index.js";
import { UREM2_DIR } from "../test/resolve.mjs";

const LAYERS = ["normalBg", "normalFg", "pressedBg", "pressedFg"];

test("urem2-test.mjs bakes to a blob with no warnings", async t => {
    const { blob, tree, warnings } = await bakeFile(path.join(UREM2_DIR, "urem2-test.mjs"));
    assert.equal(warnings.length, 0, formatEntries(warnings));
    assert.ok(blob.length > 0);
    assert.equal(blob.subarray(0, 4).toString("latin1"), "BPAK");

    const keys = [...tree.home.keys, ...tree.activities.flatMap(a => a.pages.flatMap(p => p.keys))];
    const images = new Set(keys.flatMap(k => LAYERS.map(l => k[l]).filter(Boolean)));
    assert.ok(images.size > 0);
    t.diagnostic(`blob ${blob.length} bytes, ${images.size} distinct images`);
});
