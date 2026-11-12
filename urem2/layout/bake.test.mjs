// The full bake (resolve, render every key image, pack) of both entries, as the CLI does it, with the device's key
// checks (examples/bake-check.mjs: ids, rects, opaque images of the key's size). Logs the blob size and the number of
// distinct images. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import { formatEntries, KEY_COLS, GRID_SLOTS } from "../../host/index.js";
import { bakeChecked } from "../../examples/bake-check.mjs";
import { UREM2_DIR } from "../test/resolve.mjs";

for (const entry of ["urem2-test.mjs", "urem2.mjs"])
{
    test(`${entry} bakes to a blob with no warnings, every key a 1U grid key`, { timeout: 120000 }, async t => {
        const { blob, warnings, pages, images } = await bakeChecked(path.join(UREM2_DIR, entry));
        assert.equal(warnings.length, 0, formatEntries(warnings));
        assert.ok(images.size > 0);

        // urem2 is all plain grid pages (grid(), padPage()): every key is 1U in a slot, with that slot's default id
        // (page number across the layout * 15 + slot + 1)
        pages.forEach(({ name, keys }, g) => {
            for (const k of keys)
            {
                const [x, y, w, h] = k.rect;
                assert.deepEqual([x % 4, y % 4, w, h], [0, 0, 4, 4], `${name}: key ${k.id} is a 1U slot key`);
                assert.equal(k.id, g * GRID_SLOTS + (y / 4) * KEY_COLS + x / 4 + 1, `${name}: key ${k.id}'s id`);
                assert.ok(k.normal && k.pressed, `${name}: key ${k.id} has both images`);
            }
        });
        t.diagnostic(`blob ${blob.length} bytes, ${images.size} distinct images, ${pages.length} pages`);
    });
}
