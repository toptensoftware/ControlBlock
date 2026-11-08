// Tests for hotpc.mjs. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { HOTPC_MAC, HOTPC_COMMANDS, SLIDESHOW_COMMANDS, SLIDESHOW_ELEMENTS, displayElementsBody } from "./hotpc.mjs";

test("HOT PC MAC and commands", () => {
    assert.equal(HOTPC_MAC, "04:d4:c4:4e:da:2b");
    assert.deepEqual(HOTPC_COMMANDS, ["launch-steam", "launch-kiosk", "shutdown"]);
});

test("slideshow commands and elements", () => {
    assert.deepEqual(SLIDESHOW_COMMANDS,
        ["prevDay", "nextDay", "backward20", "forward20", "play", "pause", "voteUp", "voteDown", "prev", "next"]);
    assert.deepEqual(SLIDESHOW_ELEMENTS, ["all", "transport", "date", "time", "rating", "location", "other"]);
});

test("displayElementsBody builds the toggle body as a string", () => {
    assert.equal(displayElementsBody("date"), '{"date":"toggle"}');
    assert.equal(displayElementsBody("all"), '{"all":"toggle"}');
    assert.throws(() => displayElementsBody("weather"), /unknown slideshow element/);
});
