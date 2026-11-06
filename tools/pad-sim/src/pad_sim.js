// A simulated pad: plays a script as the C3 would (HELLO x3 at boot, STATE at once on a key
// change, at most one encoder-only STATE per 5 ms, a repeat every 100 ms), with optional
// line noise. Pure (clock passed in, bytes returned); sim.js is the thin serial wrapper.
// The policy mirrors pad/common/pad_scanner.cpp.
import * as W from "./pad_wire.js";

const EDGES_PER_DETENT = 4;
const DEFAULT_LEAD_MS = W.PAD_HELLO_GAP_MS * (W.PAD_HELLO_COUNT - 1) + 2 * W.PAD_STATE_REPEAT_MS;

// ---- script -------------------------------------------------------------------------

// Commands (newline or ';' separated, '#' starts a comment), run one after the other:
//   wait <ms>
//   press <key> <ms>        key down, hold, key up
//   down <key> / up <key>   key edge, no time passes
//   spin <enc> <+n|-n> <ms> n detents, <ms> between quadrature edges (a detent is 4 edges);
//                           the STATE interval is <ms> in microseconds
//   hello                   a C3 reboot: HELLO x3 (keys and positions kept)
// Returns { actions: [{ at, kind: "key"|"turn"|"hello", ... }], end } (times in ms).
export function parseScript(text) {
    const actions = [];
    let cursor = 0;
    const lines = text.split(/\r?\n/);
    lines.forEach((raw, i) => {
        const line = raw.replace(/#.*/, "");
        for (const cmd of line.split(";")) {
            const words = cmd.trim().split(/\s+/).filter(Boolean);
            if (words.length) command(words, i + 1);
        }
    });
    return { actions, end: cursor };

    function fail(lineNo, message) {
        throw new Error(`script line ${lineNo}: ${message}`);
    }

    function num(lineNo, word, what, { min, max, integer = true } = {}) {
        const v = Number(word);
        if (word === undefined || !Number.isFinite(v) || (integer && !Number.isInteger(v))) fail(lineNo, `${what} must be a number, got ${word === undefined ? "nothing" : `"${word}"`}`);
        if (min !== undefined && v < min) fail(lineNo, `${what} must be at least ${min}`);
        if (max !== undefined && v > max) fail(lineNo, `${what} must be at most ${max}`);
        return v;
    }

    function command(words, lineNo) {
        const [name, ...args] = words;
        const want = n => { if (args.length !== n) fail(lineNo, `${name} takes ${n} argument${n === 1 ? "" : "s"}`); };
        const key = w => num(lineNo, w, "key", { min: 1, max: W.PAD_MAX_KEYS });
        switch (name) {
        case "wait":
            want(1);
            cursor += num(lineNo, args[0], "ms", { min: 0 });
            break;
        case "press": {
            want(2);
            const index = key(args[0]);
            const ms = num(lineNo, args[1], "ms", { min: 0 });
            actions.push({ at: cursor, kind: "key", index, down: true });
            cursor += ms;
            actions.push({ at: cursor, kind: "key", index, down: false });
            break;
        }
        case "down":
        case "up":
            want(1);
            actions.push({ at: cursor, kind: "key", index: key(args[0]), down: name === "down" });
            break;
        case "spin": {
            want(3);
            const enc = num(lineNo, args[0], "encoder", { min: 1, max: W.PAD_MAX_ENCODERS });
            const n = num(lineNo, args[1], "detents");
            const ms = num(lineNo, args[2], "ms per edge", { min: 1 });
            const interval = Math.min(W.PAD_INTERVAL_MAX, ms * 1000);
            for (let k = 0; k < Math.abs(n); k++) {
                cursor += ms * EDGES_PER_DETENT;
                actions.push({ at: cursor, kind: "turn", enc, delta: Math.sign(n), interval });
            }
            break;
        }
        case "hello":
            want(0);
            actions.push({ at: cursor, kind: "hello" });
            break;
        default:
            fail(lineNo, `unknown command "${name}"`);
        }
    }
}

// ---- noise --------------------------------------------------------------------------

// Seedable random numbers in [0, 1) (mulberry32)
export function makeRng(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6D2B79F5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// Each byte, with probability 'rate', is dropped, has a bit flipped, or gets a random byte inserted before it
export function corrupt(buf, rate, rng) {
    if (!(rate > 0)) return Buffer.from(buf);
    const out = [];
    for (const b of buf) {
        if (rng() >= rate) {
            out.push(b);
            continue;
        }
        const kind = rng();
        if (kind < 1 / 3) continue;
        if (kind < 2 / 3) out.push(b ^ (1 << Math.floor(rng() * 8)));
        else out.push(Math.floor(rng() * 256), b);
    }
    return Buffer.from(out);
}

// ---- the simulated pad --------------------------------------------------------------

export class PadSim {
    // opts: keys, encoders (reported in HELLO), script (parseScript result), seqStart,
    // lead (ms before the script starts, so the S3 has a baseline), loop, noise (byte rate), rng
    constructor({ keys = 6, encoders = 1, script, seqStart = 0, lead = DEFAULT_LEAD_MS, loop = false, noise = 0, rng = Math.random }) {
        this.keyCount = keys;
        this.encoderCount = encoders;
        for (const a of script.actions) {
            if (a.kind === "key" && a.index > keys) throw new Error(`script uses key ${a.index} but the pad has ${keys}`);
            if (a.kind === "turn" && a.enc > encoders) throw new Error(`script uses encoder ${a.enc} but the pad has ${encoders}`);
        }
        this.script = script;
        this.seq = seqStart >>> 0;
        this.lead = lead;
        this.loop = loop;
        this.noise = noise;
        this.rng = rng;
        this.keys = 0;
        this.enc = Array.from({ length: encoders }, () => ({ position: 0, interval: W.PAD_INTERVAL_MAX }));
    }

    start(now) {
        this.t0 = now;
        this.index = 0;
        this.cycle = 0;                 // offset of the current script pass
        this.elapsed = 0;
        this.hellosLeft = W.PAD_HELLO_COUNT;
        this.nextHelloAt = now;
        this.stateDue = false;          // send a STATE at the next chance
        this.keyDirty = false;
        this.encDirty = false;
        this.lastStateAt = now;
    }

    get finished() {
        return !this.loop && this.elapsed >= this.script.end;
    }

    // Everything due by 'now': an array of Buffers (one per packet), noise applied
    poll(now) {
        this.elapsed = now - this.t0 - this.lead;       // negative during the lead-in
        if (this.elapsed >= 0) this.#apply();

        const out = [];
        if (this.hellosLeft > 0 && now >= this.nextHelloAt) {
            out.push(this.#packet(W.PAD_CMD_HELLO, W.buildHello({ version: W.PAD_PROTOCOL_VERSION, keyCount: this.keyCount, encoderCount: this.encoderCount })));
            this.nextHelloAt = now + W.PAD_HELLO_GAP_MS;
            if (--this.hellosLeft === 0) this.stateDue = true;
        }
        if (this.hellosLeft === 0) {
            const since = now - this.lastStateAt;
            if (this.stateDue || this.keyDirty || (this.encDirty && since >= W.PAD_ENCODER_MIN_GAP_MS) || since >= W.PAD_STATE_REPEAT_MS) {
                out.push(this.#packet(W.PAD_CMD_STATE, W.buildState({ keys: this.keys, encoders: this.enc })));
                this.lastStateAt = now;
                this.stateDue = this.keyDirty = this.encDirty = false;
            }
        }
        return out.map(p => corrupt(p, this.noise, this.rng));
    }

    #packet(cmd, payload) {
        const pkt = W.encodePacket(this.seq, cmd, payload);
        this.seq = (this.seq + 1) >>> 0;
        return pkt;
    }

    #apply() {
        const { actions, end } = this.script;
        for (;;) {
            while (this.index < actions.length && actions[this.index].at + this.cycle <= this.elapsed)
                this.#act(actions[this.index++]);
            if (this.index < actions.length || !this.loop || this.elapsed < this.cycle + end) return;
            this.cycle += Math.max(end, 1);
            this.index = 0;
        }
    }

    #act(a) {
        switch (a.kind) {
        case "key": {
            const bit = (1 << (a.index - 1)) >>> 0;
            this.keys = (a.down ? this.keys | bit : this.keys & ~bit) >>> 0;
            this.keyDirty = true;
            break;
        }
        case "turn": {
            const e = this.enc[a.enc - 1];
            e.position = (e.position + a.delta) >>> 0;
            e.interval = a.interval;
            this.encDirty = true;
            break;
        }
        case "hello":
            this.hellosLeft = W.PAD_HELLO_COUNT;
            this.nextHelloAt = 0;
            break;
        }
    }
}
