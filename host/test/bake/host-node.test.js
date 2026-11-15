// The Node host for stage 1: built-in icon names come from button-face's icon set.
import { test } from "node:test";
import assert from "node:assert/strict";
import { createNodeHost } from "../../src/bake/host-node.js";

test("createNodeHost hasIcon knows the built-in icons", async () => {
    const host = await createNodeHost(".");
    assert.equal(host.hasIcon("heart"), true);
    assert.equal(host.hasIcon("no_such_icon"), false);
    assert.equal(host.hasIcon("toString"), false);
});
