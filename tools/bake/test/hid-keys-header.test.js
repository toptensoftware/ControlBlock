// The device parses a script's hid.key("ctrl+c") chords with the same key table as the bake
// (src/hid.js), generated into firmware/main/hid_keys.gen.h by gen-hid-keys.js. A table change
// without regenerating the header would make scripts and ops disagree about key names.

import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import { hidKeysHeader, HID_KEYS_HEADER_PATH } from "../gen-hid-keys.js";
import { KEYS, MODIFIERS } from "../src/hid.js";

test("the committed hid_keys.gen.h matches src/hid.js (run `node gen-hid-keys.js` to regenerate)", () => {
    const committed = fs.readFileSync(HID_KEYS_HEADER_PATH, "utf8");
    assert.equal(committed.replace(/\r\n/g, "\n"), hidKeysHeader());
});

test("the header holds every key and modifier name", () => {
    const h = hidKeysHeader();
    for (const name of Object.keys(KEYS))
        assert.ok(h.includes(JSON.stringify(name)), `key ${name}`);
    for (const name of Object.keys(MODIFIERS))
        assert.ok(h.includes(JSON.stringify(name)), `modifier ${name}`);
});
