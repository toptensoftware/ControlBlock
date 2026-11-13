// The full bake (resolve, render every key image, pack) of both entries, as the CLI does it, with the device's key
// checks (examples/bake-check.mjs: ids, rects, opaque images of the key's size). Logs the blob size and the number of
// distinct images. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import { formatEntries } from "../../host/index.js";
import { bakeChecked } from "../../examples/bake-check.mjs";
import { UREM2_DIR } from "../test/resolve.mjs";

for (const entry of ["urem2-test.mjs", "urem2.mjs"])
{
    test(`${entry} bakes to a blob with no warnings, every key with both images`, { timeout: 120000 }, async t => {
        const { blob, warnings, pages, images } = await bakeChecked(path.join(UREM2_DIR, entry));
        assert.equal(warnings.length, 0, formatEntries(warnings));
        assert.ok(images.size > 0);

        // Keys may be any size and anywhere (bakeChecked checks rects, overlaps and ids as the device does)
        for (const { name, keys } of pages)
            for (const k of keys)
                assert.ok(k.normal && k.pressed, `${name}: key ${k.id} has both images`);
        t.diagnostic(`blob ${blob.length} bytes, ${images.size} distinct images, ${pages.length} pages`);
    });
}
