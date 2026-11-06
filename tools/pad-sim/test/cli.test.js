// End to end without hardware: the real dump.js and sim.js as processes (sim writes to stdout with "-")
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import * as W from "../src/pad_wire.js";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");

function runNode(script, args, input) {
    return new Promise((resolve, reject) => {
        const p = spawn(process.execPath, [path.join(root, script), ...args], { stdio: ["pipe", "pipe", "pipe"] });
        const out = [];
        let stderr = "";
        p.stdout.on("data", d => out.push(d));
        p.stderr.on("data", d => (stderr += d));
        p.on("error", reject);
        p.on("close", code => {
            const bytes = Buffer.concat(out);
            resolve({ code, stdout: bytes.toString(), bytes, stderr });
        });
        if (input !== undefined) p.stdin.end(input);
        else p.stdin.end();
    });
}

test("dump --file replays a capture and prints readable lines", async () => {
    const capture = Buffer.concat([
        Buffer.from([0x13, 0x37]),                                                                  // noise
        W.encodePacket(0, W.PAD_CMD_HELLO, W.buildHello({ version: 1, keyCount: 6, encoderCount: 1 })),
        W.encodePacket(1, W.PAD_CMD_STATE, W.buildState({ keys: 0, encoders: [{ position: 0, interval: 65535 }] })),
        W.encodePacket(2, W.PAD_CMD_STATE, W.buildState({ keys: 4, encoders: [{ position: 0, interval: 65535 }] })),
        W.encodePacket(3, W.PAD_CMD_STATE, W.buildState({ keys: 4, encoders: [{ position: 0xFFFFFFFD, interval: 6500 }] })),
        W.encodePacket(5, W.PAD_CMD_STATE, W.buildState({ keys: 0, encoders: [{ position: 0xFFFFFFFD, interval: 6500 }] })),
    ]);
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "padsim-")), "capture.bin");
    fs.writeFileSync(file, capture);
    const r = await runNode("dump.js", ["--file", file, "--stats", "0"]);
    assert.equal(r.code, 0, r.stderr);
    assert.match(r.stdout, /HELLO version 1, 6 keys, 1 encoders/);
    assert.match(r.stdout, /key3 down/);
    assert.match(r.stdout, /enc1 turn -3 \(interval 6\.5 ms\)/);
    assert.match(r.stdout, /key3 up/);
    assert.match(r.stdout, /stats: ok 5/);
    assert.match(r.stdout, /gaps 1/);
    fs.rmSync(path.dirname(file), { recursive: true });
});

test("dump --raw hex-dumps and dump reads stdin", async () => {
    const pkt = W.encodePacket(0, W.PAD_CMD_HELLO, W.buildHello({ version: 1, keyCount: 6, encoderCount: 1 }));
    const r = await runNode("dump.js", ["--file", "-", "--raw", "--stats", "0"], pkt);
    assert.equal(r.code, 0, r.stderr);
    assert.match(r.stdout, /raw \d+: aa aa aa 00 00 02 03 01 06 01 d5 86 de d7 55/);
    assert.match(r.stdout, /HELLO/);
});

test("dump reports a missing file and usage errors", async () => {
    const r = await runNode("dump.js", ["--file", path.join(root, "no-such-file"), "--stats", "0"]);
    assert.equal(r.code, 1);
    assert.match(r.stderr, /no-such-file/);
    assert.equal((await runNode("dump.js", [])).code, 2);
});

test("sim - | dump --file -: a scripted pad is read back", async () => {
    const sim = await runNode("sim.js", ["-", "-e", "press 3 40; spin 1 +2 5", "--lead", "250", "--seq-start", "100", "--duration", "700"]);
    assert.equal(sim.code, 0, sim.stderr);
    assert.ok(sim.bytes.length > 50);
    const r = await runNode("dump.js", ["--file", "-", "--stats", "0"], sim.bytes);
    assert.match(r.stdout, /HELLO/);
    assert.match(r.stdout, /key3 down/);
    assert.match(r.stdout, /key3 up/);
    assert.equal((r.stdout.match(/enc1 turn \+1 \(interval 5\.0 ms\)/g) || []).length, 2);
    assert.match(r.stdout, /gaps 0/);
});

test("sim rejects bad scripts and options with a message", async () => {
    let r = await runNode("sim.js", ["-", "-e", "press 99 1"]);
    assert.equal(r.code, 2);
    assert.match(r.stderr, /line 1/);
    r = await runNode("sim.js", ["-", "-e", "press 7 1"]);        // default pad has 6 keys
    assert.equal(r.code, 2);
    assert.match(r.stderr, /key 7/);
    r = await runNode("sim.js", ["-", "--noise", "x"]);
    assert.equal(r.code, 2);
});
