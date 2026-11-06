import { test } from "node:test";
import assert from "node:assert/strict";
import * as W from "../src/pad_wire.js";
import { PadDump } from "../src/pad_dump.js";
import { parseScript, PadSim, corrupt, makeRng } from "../src/pad_sim.js";

// Runs a sim on a fake clock with a 1 ms poll; returns what a decoder saw, with arrival times
function run(sim, endMs, { noise = false } = {}) {
    const events = [];
    const packets = [];
    let t = 0;
    const dump = new PadDump({ now: () => t, onEvent: e => events.push(e) });
    const raw = W.packet_decoder((seq, cmd, p) => packets.push({ t, seq, cmd, payload: Buffer.from(p) }), () => {}, 32);
    sim.start(0);
    for (t = 0; t <= endMs; t++) {
        for (const chunk of sim.poll(t)) {
            dump.feed(chunk);
            for (const b of chunk) raw(b);
        }
    }
    return { events, packets, dump };
}

test("parseScript: commands, comments, semicolons", () => {
    const s = parseScript("wait 10\n# comment\npress 3 200 ; down 2\nup 2  # trailing\nspin 1 +3 5\nspin 1 -2 5\nhello");
    assert.deepEqual(s.actions.filter(a => a.kind === "key").map(a => [a.at, a.index, a.down]),
        [[10, 3, true], [210, 3, false], [210, 2, true], [210, 2, false]]);
    const turns = s.actions.filter(a => a.kind === "turn");
    // 3 detents at 5 ms per edge: one detent per 20 ms
    assert.deepEqual(turns.map(a => [a.at, a.enc, a.delta, a.interval]),
        [[230, 1, 1, 5000], [250, 1, 1, 5000], [270, 1, 1, 5000], [290, 1, -1, 5000], [310, 1, -1, 5000]]);
    assert.equal(s.actions.at(-1).kind, "hello");
    assert.equal(s.actions.at(-1).at, 310);
    assert.equal(s.end, 310);
});

test("parseScript: errors name the line", () => {
    assert.throws(() => parseScript("wait 1\nbogus 2"), /line 2.*bogus/);
    assert.throws(() => parseScript("press 0 10"), /line 1.*key/);
    assert.throws(() => parseScript("press 33 10"), /line 1/);
    assert.throws(() => parseScript("spin 5 +1 5"), /line 1/);
    assert.throws(() => parseScript("spin 1 x 5"), /line 1/);
    assert.throws(() => parseScript("wait"), /line 1/);
    assert.throws(() => parseScript("wait -5"), /line 1/);
    assert.throws(() => parseScript("spin 1 +1 0"), /line 1/);
});

test("boot: HELLO x3 at 50 ms, then STATE, then repeats every 100 ms", () => {
    const sim = new PadSim({ keys: 6, encoders: 1, script: parseScript("") });
    const { packets } = run(sim, 520);
    const hello = packets.filter(p => p.cmd === W.PAD_CMD_HELLO);
    assert.deepEqual(hello.map(p => p.t), [0, 50, 100]);
    assert.deepEqual(W.parseHello(hello[0].payload), { version: 1, keyCount: 6, encoderCount: 1 });
    const states = packets.filter(p => p.cmd === W.PAD_CMD_STATE);
    // no STATE until the last HELLO is out, then at once, then every 100 ms
    assert.deepEqual(states.map(p => p.t), [100, 200, 300, 400, 500]);
    assert.deepEqual(W.parseState(states[0].payload), { keys: 0, encoders: [{ position: 0, interval: 65535 }] });
    assert.deepEqual(packets.map(p => p.seq), packets.map((_, i) => i));
});

test("seqStart, and the seq wraps at 2^32", () => {
    const sim = new PadSim({ keys: 6, encoders: 0, seqStart: 0xFFFFFFFE, script: parseScript("") });
    const { packets } = run(sim, 200);
    assert.deepEqual(packets.slice(0, 4).map(p => p.seq), [0xFFFFFFFE, 0xFFFFFFFF, 0, 1]);
});

test("a key press is sent at once when it changes, and held for the time given", () => {
    const sim = new PadSim({ keys: 6, encoders: 0, lead: 0, script: parseScript("wait 130\npress 3 20") });
    const { packets } = run(sim, 300);
    const states = packets.filter(p => p.cmd === W.PAD_CMD_STATE).map(p => [p.t, W.parseState(p.payload).keys]);
    assert.deepEqual(states, [[100, 0], [130, 4], [150, 0], [250, 0]]);
});

test("the script starts after the lead-in so the S3 has a baseline first", () => {
    const sim = new PadSim({ keys: 6, encoders: 0, script: parseScript("press 1 10") });   // default lead 300
    const { events } = run(sim, 400);
    assert.deepEqual(events.filter(e => e.type !== "hello").map(e => [e.type, e.t]),
        [["baseline", 100], ["key", 300], ["key", 310]]);
});

test("encoder spin: a turn per detent, delta and interval as the dump sees them", () => {
    const sim = new PadSim({ keys: 6, encoders: 1, lead: 0, script: parseScript("wait 120\nspin 1 +3 5\nspin 1 -1 10") });
    const { events } = run(sim, 400);
    const turns = events.filter(e => e.type === "turn");
    assert.deepEqual(turns.map(e => [e.delta, e.interval]), [[1, 5000], [1, 5000], [1, 5000], [-1, 10000]]);
});

test("encoder-only changes are capped to one STATE per 5 ms and lose nothing", () => {
    // 1 ms per edge = a detent every 4 ms, faster than the cap
    const sim = new PadSim({ keys: 6, encoders: 1, lead: 0, script: parseScript("wait 120\nspin 1 +20 1") });
    const { packets, events } = run(sim, 400);
    const sent = packets.filter(p => p.cmd === W.PAD_CMD_STATE && p.t > 120 && p.t <= 200);
    for (let i = 1; i < sent.length; i++) assert.ok(sent[i].t - sent[i - 1].t >= 5, `gap at ${sent[i].t}`);
    assert.ok(sent.length < 20);
    const total = events.filter(e => e.type === "turn").reduce((a, e) => a + e.delta, 0);
    assert.equal(total, 20);
});

test("a key change goes out at once even right after an encoder state", () => {
    const sim = new PadSim({ keys: 6, encoders: 1, lead: 0, script: parseScript("wait 120\nspin 1 +1 1\ndown 2") });
    // detent at 124, key down at 124 too: both in one STATE at 124
    const { packets } = run(sim, 200);
    const st = packets.filter(p => p.cmd === W.PAD_CMD_STATE && p.t === 124);
    assert.equal(st.length, 1);
    assert.deepEqual(W.parseState(st[0].payload), { keys: 2, encoders: [{ position: 1, interval: 1000 }] });
});

test("hello in the script restarts the boot sequence without changing state", () => {
    const sim = new PadSim({ keys: 6, encoders: 0, lead: 0, script: parseScript("down 1\nwait 150\nhello\nwait 300") });
    const { packets } = run(sim, 400);
    const helloAt = packets.filter(p => p.cmd === W.PAD_CMD_HELLO).map(p => p.t);
    assert.deepEqual(helloAt, [0, 50, 100, 150, 200, 250]);
    const afterHello = packets.filter(p => p.cmd === W.PAD_CMD_STATE && p.t >= 250);
    assert.equal(W.parseState(afterHello[0].payload).keys, 1);
});

test("finished, and loop", () => {
    const sim = new PadSim({ keys: 6, encoders: 0, lead: 0, script: parseScript("wait 130\npress 1 20") });
    sim.start(0);
    sim.poll(100);
    assert.equal(sim.finished, false);
    sim.poll(160);
    assert.equal(sim.finished, true);

    const loop = new PadSim({ keys: 6, encoders: 0, lead: 0, loop: true, script: parseScript("wait 130\npress 1 20") });
    const { events } = run(loop, 600);
    const downs = events.filter(e => e.type === "key" && e.down).map(e => e.t);
    assert.deepEqual(downs, [130, 280, 430, 580]);
    assert.equal(loop.finished, false);
});

test("noise 0 changes nothing; same seed gives the same noise", () => {
    const buf = Buffer.from(Array.from({ length: 200 }, (_, i) => i));
    assert.deepEqual(corrupt(buf, 0, makeRng(1)), buf);
    const a = corrupt(buf, 0.1, makeRng(7));
    const b = corrupt(buf, 0.1, makeRng(7));
    assert.deepEqual(a, b);
    assert.notDeepEqual(a, buf);
    assert.notDeepEqual(corrupt(buf, 0.1, makeRng(8)), a);
});

test("noise does all three: drops, flips and inserts", () => {
    const buf = Buffer.alloc(5000, 0x11);
    const out = corrupt(buf, 0.05, makeRng(3));
    assert.ok(out.length < buf.length + 400 && out.length > buf.length - 400);
    assert.ok(out.some(b => b !== 0x11));
    let seenLonger = false, seenShorter = false;
    for (let seed = 1; seed < 30; seed++) {
        const o = corrupt(Buffer.alloc(40, 0x11), 0.2, makeRng(seed));
        if (o.length > 40) seenLonger = true;
        if (o.length < 40) seenShorter = true;
    }
    assert.ok(seenLonger && seenShorter);
});

test("with noise the decoder only ever delivers real packets, and the good ones still arrive", () => {
    const sim = new PadSim({
        keys: 6, encoders: 1, lead: 0, noise: 0.01, rng: makeRng(42),
        script: parseScript("wait 120\npress 1 30\nspin 1 +10 5\npress 2 30\nwait 1000"),
    });
    const { packets } = run(sim, 3000);
    assert.ok(packets.length > 20);
    for (const p of packets) {
        if (p.cmd === W.PAD_CMD_STATE) {
            const s = W.parseState(p.payload);
            assert.ok(s, "state length");
            assert.equal(s.encoders.length, 1);
            assert.ok(s.keys === 0 || s.keys === 1 || s.keys === 2, `keys ${s.keys}`);
            assert.ok(s.encoders[0].position <= 10);
        } else {
            assert.equal(p.cmd, W.PAD_CMD_HELLO);
            assert.deepEqual(W.parseHello(p.payload), { version: 1, keyCount: 6, encoderCount: 1 });
        }
    }
});
