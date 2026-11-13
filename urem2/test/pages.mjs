// Helpers for the Home and activity tests: check a page's keys (what they are, not where they are, so a page can be
// rearranged without touching its tests), and wrap a Home or activity in a full layout (root defaults, devices, every
// activity) so it can be resolved.

import assert from "node:assert/strict";
import { ROOT_DEFAULTS } from "../layout/look.mjs";
import { devices, ACTIVITIES, activityMeta } from "../layout/devices.mjs";
import { cleanLayout } from "./resolve.mjs";

const PLACEMENT = ["x", "y", "w", "h"];
const unplaced = k => Object.fromEntries(Object.entries(k).filter(([p]) => !PLACEMENT.includes(p)));
const placed = k => typeof k.x === "number" && typeof k.y === "number";

// keys: a page's keys array; expected: the keys it should hold (positionless), in any order. Every key must be placed
// (x and y) and no entry may be null.
export function assertKeys(keys, expected)
{
    assert.ok(Array.isArray(keys), "keys is an array");
    for (const [i, k] of keys.entries())
    {
        assert.ok(k !== null && typeof k === "object", `key ${i} is a key`);
        assert.ok(placed(k), `key ${i} has x and y`);
    }
    const remaining = expected.map(unplaced);
    for (const [i, k] of keys.entries())
    {
        const actual = unplaced(k);
        const at = remaining.findIndex(e => { try { assert.deepEqual(actual, e); return true; } catch { return false; } });
        assert.ok(at >= 0, `key ${i} (${JSON.stringify(actual.icon ?? actual.label)}) is expected`);
        remaining.splice(at, 1);
    }
    assert.equal(remaining.length, 0, `missing keys: ${remaining.map(e => JSON.stringify(e.icon ?? e.label)).join(", ")}`);
}

// An activity resolves clean inside a full layout, and every key on its pages has a picture
export async function assertActivityResolves(ctx, name, activity)
{
    const tree = await cleanLayout(layoutWith(ctx, { activities: { [name]: activity } }));
    const { pages } = tree.activities.find(a => a.name === name);
    assert.equal(pages.length, activity.pages.length);
    pages.forEach((p, i) => p.keys.forEach(k => assert.ok(k.normal, `${name} page ${i}: key ${k.id} has an image`)));
}

// A layout with Home (empty unless given) and every activity (a one-page stub unless given)
export function layoutWith(ctx, { home = { keys: [] }, activities = {} } = {})
{
    return {
        version: 1, defaults: ROOT_DEFAULTS,
        devices: devices(ctx),
        home,
        activities: Object.fromEntries(Object.keys(ACTIVITIES).map(a =>
            [a, activities[a] ?? { ...activityMeta(ctx, a), pages: [{ keys: [] }] }])),
    };
}
