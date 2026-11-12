// bench-sink.mjs against the library's FakeDevice (which refuses image id 0 like the firmware).
// From the repo root:  node --test "examples/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { ControlBlock, Msg, Reader } from "../host/index.js";
import { FakeDevice } from "../host/test/FakeDevice.js";
import { sinkSend } from "./bench-sink.mjs";

test("sinkSend sends full-size PUT_IMAGEs for id 0, all refused, nothing stored", async () => {
    const dev = new FakeDevice({ hosted: true });
    const cb = new ControlBlock(dev, { timeoutMs: 300 });
    await cb.connect();
    const bytes = await sinkSend(cb, 5);

    const sent = dev.requests.filter(m => m.type === Msg.PUT_IMAGE);
    assert.equal(sent.length, 5);
    for (const m of sent) {
        assert.equal(m.payload.length, cb.info.maxPayload);
        assert.equal(new Reader(m.payload).u32(), 0);
    }
    assert.equal(bytes, 5 * cb.info.maxPayload);
    assert.equal(dev.images.size, 0);
    await cb.close();
});

test("sinkSend throws if the device takes the data", async () => {
    const dev = new FakeDevice({ hosted: true, handler: () => ({ status: 0, data: new Uint8Array(0) }) });
    const cb = new ControlBlock(dev, { timeoutMs: 300 });
    await cb.connect();
    await assert.rejects(sinkSend(cb, 2), /BAD_ARG/);
    await cb.close();
});
