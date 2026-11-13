// Tests for the test helper assertKeys: order-free, positions ignored, but every key placed and nothing extra or
// missing. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { assertKeys } from "./pages.mjs";

const a = { label: "A", tap: [] }, b = { label: "B", tap: [] };

test("assertKeys: any order and any position or size match", () => {
    assertKeys([{ ...b, x: 4, y: 2, w: 0.5 }, { ...a, x: 0, y: 0, w: 2, h: 1.5 }], [a, b]);
    assertKeys([], []);
});

test("assertKeys: a missing, extra, changed, unplaced or null key fails", () => {
    const fails = (keys, expected) => assert.throws(() => assertKeys(keys, expected), assert.AssertionError);
    fails([{ ...a, x: 0, y: 0 }], [a, b]);
    fails([{ ...a, x: 0, y: 0 }, { ...b, x: 1, y: 0 }], [a]);
    fails([{ ...a, x: 0, y: 0 }, { ...a, x: 1, y: 0 }], [a, b]);
    fails([{ ...a, label: "C", x: 0, y: 0 }], [a]);
    fails([a], [a]);
    fails([{ ...a, x: 0 }], [a]);
    fails([null], []);
});
