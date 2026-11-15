// Test helper for layouts in this repo (the examples and urem2): bakes a layout file as the CLI does and checks its
// keys the way the device's layout validator does (firmware/main/layout_blob.cpp): ids 1..65535 and unique across
// the whole layout, rects inside the grid with no overlaps on a page, at most MAX_KEYS_PER_PAGE keys a page, each
// image fully opaque and exactly its key's size, and every touch[id] binding naming a key it can reach (Home's keys
// for Home, the activity's own pages for an activity).

import assert from "node:assert/strict";
import { bakeFile, KEY_COLS, KEY_ROWS, KEY_QUARTER, MAX_KEYS_PER_PAGE, MAX_KEY_ID } from "../host/index.js";
import { unpackRle } from "rle565/unpack.js";

const GRID_QW = KEY_COLS * 4, GRID_QH = KEY_ROWS * 4;    // the grid in quarter units
export const IMAGE_STATES = ["normal", "pressed"];

// The tree's pages in layout order (Home, then each activity's pages): [{ name, keys, scope }], where scope names the
// bindings the page's keys belong to ("home" or the activity's index)
export function treePages(tree)
{
    return [
        { name: "home", keys: tree.home.keys, scope: "home" },
        ...tree.activities.flatMap((a, i) => a.pages.map((p, j) => ({ name: `${a.name}.pages[${j}]`, keys: p.keys, scope: i }))),
    ];
}

const overlap = (a, b) => a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];

// -> { pages, images }: pages as treePages, images = the distinct image buffers
export function checkTreeKeys(tree)
{
    const pages = treePages(tree);
    const ids = new Set(), scopeIds = new Map();
    const images = new Map();     // buffer -> { width, height }

    for (const { name, keys, scope } of pages)
    {
        assert.ok(keys.length <= MAX_KEYS_PER_PAGE, `${name}: ${keys.length} keys, over ${MAX_KEYS_PER_PAGE}`);
        if (!scopeIds.has(scope))
            scopeIds.set(scope, new Set());
        keys.forEach((k, i) => {
            const at = `${name}.keys[${i}] (id ${k.id})`;
            assert.ok(Number.isInteger(k.id) && k.id >= 1 && k.id <= MAX_KEY_ID, `${at}: bad id`);
            assert.ok(!ids.has(k.id), `${at}: id used twice in the layout`);
            ids.add(k.id);
            scopeIds.get(scope).add(k.id);

            const [x, y, w, h] = k.rect;
            assert.ok(w > 0 && h > 0 && x + w <= GRID_QW && y + h <= GRID_QH, `${at}: rect ${k.rect} not inside the grid`);
            keys.slice(0, i).forEach((o, j) =>
                assert.ok(!overlap(o.rect, k.rect), `${at}: overlaps ${name}.keys[${j}]`));

            for (const state of IMAGE_STATES)
            {
                const buf = k[state];
                if (!buf)
                    continue;
                if (!images.has(buf))
                {
                    const img = unpackRle(buf);
                    for (let a = 3; a < img.data.length; a += 4)
                        assert.equal(img.data[a], 255, `${at}: ${state} image has a transparent pixel`);
                    images.set(buf, { width: img.width, height: img.height });
                }
                assert.deepEqual(images.get(buf), { width: w * KEY_QUARTER, height: h * KEY_QUARTER },
                                 `${at}: ${state} image isn't the key's size`);
            }
        });
    }

    const touched = (bindings, scope, name) => {
        for (const b of bindings)
        {
            const m = /^touch\[(\d+)\]$/.exec(b.control);
            if (m)
                assert.ok(scopeIds.get(scope)?.has(Number(m[1])), `${name}: ${b.control} names no key of its own`);
        }
    };
    touched(tree.home.bindings, "home", "home");
    tree.activities.forEach((a, i) => touched(a.bindings, i, a.name));

    return { pages, images: new Set(images.keys()) };
}

// Bakes `file` (throws a BakeError on errors) and checks its keys. -> { blob, tree, warnings, pages, images }
export async function bakeChecked(file)
{
    const { blob, tree, warnings } = await bakeFile(file);
    assert.equal(blob.subarray(0, 4).toString("latin1"), "BPAK");
    return { blob, tree, warnings, ...checkTreeKeys(tree) };
}
