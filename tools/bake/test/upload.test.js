// Uploading a baked blob to the device (docs/protocol.md "Configuration upload"),
// against the host library's fake device, and the CLI's argument parsing.
import { test } from "node:test";
import assert from "node:assert/strict";
import { ControlBlock } from "../../../host/src/ControlBlock.js";
import { Msg, crc32 } from "../../../host/src/protocol.js";
import { FakeDevice } from "../../../host/test/FakeDevice.js";
import { uploadBlob } from "../src/upload.js";
import { parseArgs } from "../src/args.js";

const blobOf = (size, seed = 3) => new Uint8Array(size).map((_, i) => ((i + seed) * 2654435761) >>> 24);

// An `open` that connects to the fake device (what ControlBlock.open does for a real one)
function opener(dev, opened = []) {
    return async options => {
        opened.push(options);
        const cb = new ControlBlock(dev, { timeoutMs: 500 });
        await cb.connect();
        return cb;
    };
}

function logger() {
    const lines = [];
    const log = line => lines.push(line);
    log.lines = lines;
    return log;
}

const fast = { timeoutMs: 3000, intervalMs: 10 };

test("uploads the blob, waits for the restart and checks what the device kept", async () => {
    const dev = new FakeDevice({ maxPayload: 1000, config: { restartMs: 60 } });
    const blob = blobOf(5000);
    const opened = [];
    const log = logger();
    const r = await uploadBlob(blob, { open: opener(dev, opened), log, serial: "SERIAL1", reconnect: fast });

    assert.deepEqual(r, { uploaded: true, verified: true, size: 5000, crc: crc32(blob) });
    assert.deepEqual([...dev.config.stored.bytes], [...blob]);
    assert.equal(opened[0].serial, "SERIAL1");              // the one asked for
    assert.equal(opened[1].serial, "SERIAL1");              // and the same one again afterwards
    assert.ok(log.lines.some(l => /5000 bytes/.test(l)));
    assert.ok(log.lines.some(l => /restart/i.test(l)));
    assert.ok(log.lines.at(-1).includes("verified") || log.lines.some(l => /verified/.test(l)));
});

test("a blob the device already has is not sent again, unless forced", async () => {
    const blob = blobOf(800);
    const dev = new FakeDevice({ config: { restartMs: 40, stored: { size: 800, crc: crc32(blob) } } });
    const log = logger();
    const r = await uploadBlob(blob, { open: opener(dev), log, reconnect: fast });
    assert.deepEqual(r, { uploaded: false, verified: true, size: 800, crc: crc32(blob) });
    assert.ok(!dev.requests.some(m => m.type === Msg.CONFIG_BEGIN));
    assert.ok(log.lines.some(l => /already/i.test(l)));

    const forced = await uploadBlob(blob, { open: opener(dev), log, force: true, reconnect: fast });
    assert.equal(forced.uploaded, true);
    assert.ok(dev.requests.some(m => m.type === Msg.CONFIG_BEGIN));
});

test("a device that comes back without the config is an error", async () => {
    const dev = new FakeDevice({ config: { restartMs: 40 } });
    let calls = 0;
    const open = async o => {
        if (++calls === 2)
            dev.config.stored = { size: 1, crc: 0 };        // it restarted with something else
        return opener(dev)(o);
    };
    await assert.rejects(uploadBlob(blobOf(500), { open, log: logger(), reconnect: fast }), /different config|not match/i);
});

test("a device that never comes back is an error", async () => {
    const dev = new FakeDevice({ config: { restartMs: 1000 } });
    await assert.rejects(uploadBlob(blobOf(500), {
        open: opener(dev), log: logger(), reconnect: { timeoutMs: 200, intervalMs: 20 },
    }), /did not come back/);
});

test("a refused upload says why and leaves the connection closed", async () => {
    const dev = new FakeDevice({ config: { capacity: 1 << 20 } });
    const open = opener(dev);
    let cb;
    const spy = async o => {
        cb = await open(o);
        dev.config.capacity = 100;              // less than it said at connect: the device is the judge
        return cb;
    };
    await assert.rejects(uploadBlob(blobOf(500), { open: spy, log: logger() }), /NO_SPACE/);
    assert.equal(cb.connected, false);
});

test("progress is reported as it goes", async () => {
    const dev = new FakeDevice({ maxPayload: 200, config: { restartMs: 30 } });
    const log = logger();
    await uploadBlob(blobOf(196 * 10), { open: opener(dev), log, reconnect: fast });
    assert.ok(log.lines.some(l => /%/.test(l)));
});

// --- Command line ---

test("args: bake only, as before", () => {
    assert.deepEqual(parseArgs(["config.json"]), { input: "config.json", out: undefined, header: undefined, upload: false, serial: undefined, force: false, binary: false });
    const a = parseArgs(["config.json", "-o", "out.bin", "--header", "t.h"]);
    assert.equal(a.out, "out.bin");
    assert.equal(a.header, "t.h");
    assert.equal(a.upload, false);
});

test("args: --upload, --serial and --force", () => {
    const a = parseArgs(["config.json", "--upload", "--serial", "ABC123", "--force"]);
    assert.equal(a.upload, true);
    assert.equal(a.serial, "ABC123");
    assert.equal(a.force, true);
    assert.equal(a.binary, false);
});

test("args: a .bin is uploaded as it is", () => {
    const a = parseArgs(["out.bin"]);
    assert.equal(a.binary, true);
    assert.equal(a.upload, true);                           // that's all there is to do with one
    assert.throws(() => parseArgs(["out.bin", "-o", "x.bin"]), /already baked/i);
    assert.throws(() => parseArgs(["out.bin", "--header", "t.h"]), /already baked/i);
});

test("args: mistakes are reported with the usage", () => {
    assert.throws(() => parseArgs([]), /usage/);
    assert.throws(() => parseArgs(["a.json", "b.json"]), /usage/);
    assert.throws(() => parseArgs(["a.json", "--nope"]), /unknown option --nope/);
    assert.throws(() => parseArgs(["a.json", "--serial"]), /needs a value/);
    assert.throws(() => parseArgs(["a.json", "-o"]), /needs a value/);
    assert.throws(() => parseArgs(["a.json", "--serial", "X"]), /--upload/);   // --serial without --upload does nothing
    assert.throws(() => parseArgs(["a.json", "--force"]), /--upload/);
});
