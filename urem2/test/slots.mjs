// Helpers for the Home and activity tests: check a page's 15 slots against an expected map, and wrap a Home or
// activity in a full layout (root defaults, devices, every activity) so it can be resolved.

import assert from "node:assert/strict";
import { ROOT_DEFAULTS } from "../layout/look.mjs";
import { devices, ACTIVITIES, activityMeta } from "../layout/devices.mjs";

// keys: a page's keys array; expected: { index: key } (every other slot must be null)
export function assertSlots(keys, expected)
{
    assert.equal(keys.length, 15, "15 slots");
    for (let i = 0; i < 15; i++)
        assert.deepEqual(keys[i], expected[i] ?? null, `slot ${i}`);
    for (const i of Object.keys(expected))
        assert.ok(Number(i) >= 0 && Number(i) < 15, `expected slot ${i} is on the grid`);
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
