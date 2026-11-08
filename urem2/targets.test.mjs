// Tests for targets.mjs. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { targets } from "./targets.mjs";
import { SERVER } from "../examples/test-common.mjs";
import { YAMAHA_API } from "./tables/yamaha.mjs";
import { IRTX_PORT } from "./tables/irtx.mjs";
import { ATV_PORT } from "./tables/appletv.mjs";
import { HOTPC_MAC } from "./tables/hotpc.mjs";

// The nested key names of an object (values ignored)
function shape(obj)
{
    return typeof obj === "object" && obj !== null
        ? Object.fromEntries(Object.keys(obj).sort().map(k => [k, shape(obj[k])]))
        : typeof obj;
}

test("real and test targets have the same shape", () => {
    assert.deepEqual(Object.keys(targets).sort(), ["real", "test"]);
    assert.deepEqual(shape(targets.test), shape(targets.real));
    assert.deepEqual(Object.keys(targets.real).sort(), ["atvd", "hotpc", "irtx", "slideshow", "yamaha"]);
});

test("real addresses", () => {
    assert.deepEqual(targets.real, {
        irtx: { host: "10.1.1.188", port: IRTX_PORT },
        atvd: { host: "10.1.1.2", port: ATV_PORT },
        yamaha: "http://yama:80" + YAMAHA_API,
        hotpc: { host: "10.1.1.105", port: 4500, mac: HOTPC_MAC },
        slideshow: { host: "10.1.1.2", udpPort: 4500, httpPort: 4500 },
    });
});

test("test addresses are the test server's PC, Yamaha on 8080", () => {
    assert.deepEqual(targets.test, {
        irtx: { host: SERVER, port: 4210 },
        atvd: { host: SERVER, port: 7777 },
        yamaha: `http://${SERVER}:8080${YAMAHA_API}`,
        hotpc: { host: SERVER, port: 4500, mac: HOTPC_MAC },
        slideshow: { host: SERVER, udpPort: 4501, httpPort: 4500 },
    });
});

// Windows drops inbound UDP from other machines on the IPsec ports (500 IKE, 4500 NAT-T) before it reaches Node
test("test target's UDP ports avoid the Windows IPsec ports", () => {
    const t = targets.test;
    for (const port of [t.irtx.port, t.atvd.port, t.slideshow.udpPort])
        assert.ok(port !== 500 && port !== 4500, `UDP port ${port}`);
});

test("Yamaha base URLs end in the API path", () => {
    for (const t of Object.values(targets))
        assert.ok(t.yamaha.endsWith(YAMAHA_API), t.yamaha);
});
