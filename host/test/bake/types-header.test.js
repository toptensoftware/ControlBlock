// The C structs for the packed config are generated from the binpack schema (host/src/bake/schema.js) into
// firmware/main/config_types.gen.h by tools/codegen/gen-types.js. The firmware reads the blob through them, so
// a schema change without regenerating the header would corrupt what the device reads.

import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import { typesHeader, TYPES_HEADER_PATH } from "../../src/bake/headers.js";

test("the committed config_types.gen.h matches the schema (run `node tools/codegen/gen-types.js` to regenerate)", () => {
    const committed = fs.readFileSync(TYPES_HEADER_PATH, "utf8");
    assert.equal(committed.replace(/\r\n/g, "\n"), typesHeader());
});

test("the header declares every struct the firmware reads", () => {
    const h = typesHeader();
    for (const name of ["configRoot", "home", "key", "binding", "device", "activity", "page",
                        "delayOp", "hidKeyOp", "httpGetOp", "httpPostOp", "udpPacketOp", "sendWolOp", "scriptOp"])
        assert.match(h, new RegExp(`struct ${name}\\s*\\n\\{`), name);
});

test("no struct has a member named like its own type (GCC rejects 'changes meaning')", () => {
    const h = typesHeader();
    // "\t/* off */\t<type> <member>;" with the type and member the same word
    const clash = [...h.matchAll(/\*\/\t(\w+) (\w+);/g)].filter(m => m[1] === m[2]).map(m => m[1]);
    assert.deepEqual(clash, []);
});
