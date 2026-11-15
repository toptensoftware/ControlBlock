// The test server's address lives in one place: test-common.mjs, overridden by CB_TEST_SERVER (an IP) and
// CB_TEST_SERVER_NAME (a host name for the same PC), and the test layouts take it from there.
// From the repo root:  node --test "examples/**/*.test.mjs"   (each test file runs in its own process)

import { test } from "node:test";
import assert from "node:assert/strict";

// Set before test-common.mjs is first imported (no static import of it here: it reads the environment once)
process.env.CB_TEST_SERVER = "192.0.2.7";
process.env.CB_TEST_SERVER_NAME = "test-pc.example";

test("SERVER, SERVER_NAME and HTTP honour CB_TEST_SERVER and CB_TEST_SERVER_NAME", async () => {
    const { SERVER, SERVER_NAME, HTTP, udp, get } = await import("./test-common.mjs");
    assert.equal(SERVER, "192.0.2.7");
    assert.equal(SERVER_NAME, "test-pc.example");
    assert.equal(HTTP, "http://192.0.2.7:8000");
    assert.equal(udp("x").host, "192.0.2.7");
    assert.equal(get("a=1").url, "http://192.0.2.7:8000/get?a=1");
});

test("the test layouts take every address from test-common.mjs", async () => {
    for (const file of ["net-test.mjs", "activity-test.mjs", "script-test.mjs"])
    {
        const text = JSON.stringify((await import(`./${file}`)).default);
        assert.ok(!text.includes("10.1.1.126"), `${file} names the default address`);
        assert.ok(!text.includes("braddev"), `${file} names the default host name`);
        assert.ok(text.includes("192.0.2.7"), `${file} doesn't use SERVER`);
    }
    const net = JSON.stringify((await import("./net-test.mjs")).default);
    assert.ok(net.includes("test-pc.example"), "net-test.mjs doesn't use SERVER_NAME");
});
