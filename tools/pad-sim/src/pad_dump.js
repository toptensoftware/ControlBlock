// Decodes the pad link into readable events, the way the S3 will read it (the S3 reacts to
// the difference between consecutive STATEs, never to a packet). Pure: bytes in, events out
// through a callback, the clock injected. dump.js is the thin serial wrapper.
import * as W from "./pad_wire.js";

const TURN_RESET_DELTA = 1 << 15;   // |delta| over this is a baseline reset, not a turn (as on the S3)
const SEQ_RESET_MIN = 0x80000000;   // a seq this far "behind" means the C3 restarted

// The decoder reports errors as text: name them for the counters
export function classifyError(message) {
    if (message.includes("new packet signal")) return "newSignal";
    if (message.includes("stuff byte")) return "stuff";
    if (message.includes("separator")) return "separator";
    if (message.includes("length exceeded")) return "tooLarge";
    if (message.includes("checksum")) return "crc";
    if (message.includes("terminator")) return "terminator";
    return "other";
}

export class PadDump {
    // opts: { now: () => ms, onEvent: (event) => void }
    constructor({ now, onEvent }) {
        this.now = now;
        this.onEvent = onEvent;
        this.rx = W.packet_decoder(
            (seq, cmd, payload) => this.#packet(seq, cmd, Buffer.from(payload)),
            message => this.#error(message),
            W.PAD_PAYLOAD_MAX_BYTES);
        this.#resetStats();
        this.prev = null;            // last STATE since the baseline, null = no baseline
        this.lastStateAt = 0;
        this.expectedSeq = null;
    }

    #resetStats() {
        this.counts = { ok: 0, states: 0, hellos: 0, repeats: 0, badPayload: 0, unknown: 0, seqGaps: 0, seqResets: 0, errors: {} };
    }

    stats() {
        return { ...this.counts, errors: { ...this.counts.errors } };
    }

    feed(bytes) {
        for (const b of bytes) this.rx(b);
    }

    // Call regularly (every 20 ms or so): detects the 300 ms silence
    tick() {
        if (this.prev && this.now() - this.lastStateAt >= W.PAD_OFFLINE_TIMEOUT_MS) {
            this.prev = null;
            this.#emit({ type: "offline" });
        }
    }

    #emit(e) {
        this.onEvent({ ...e, t: this.now() });
    }

    #error(message) {
        const name = classifyError(message);
        this.counts.errors[name] = (this.counts.errors[name] || 0) + 1;
        this.#emit({ type: "error", name, message });
    }

    #packet(seq, cmd, payload) {
        this.counts.ok++;
        if (this.expectedSeq !== null) {
            const gap = (seq - this.expectedSeq) >>> 0;
            if (gap >= SEQ_RESET_MIN) this.counts.seqResets++;
            else this.counts.seqGaps += gap;
        }
        this.expectedSeq = (seq + 1) >>> 0;

        if (cmd === W.PAD_CMD_STATE) this.#state(payload);
        else if (cmd === W.PAD_CMD_HELLO) this.#hello(seq, payload);
        else {
            this.counts.unknown++;
            this.#emit({ type: "unknown", seq, cmd, length: payload.length });
        }
    }

    #hello(seq, payload) {
        const h = W.parseHello(payload);
        if (!h) return this.#bad(W.PAD_CMD_HELLO, payload);
        this.counts.hellos++;
        this.prev = null;
        this.#emit({ type: "hello", seq, version: h.version, keyCount: h.keyCount, encoderCount: h.encoderCount });
    }

    #bad(cmd, payload) {
        this.counts.badPayload++;
        this.#emit({ type: "bad", cmd, length: payload.length });
    }

    #state(payload) {
        const s = W.parseState(payload);
        if (!s) return this.#bad(W.PAD_CMD_STATE, payload);
        this.counts.states++;
        this.lastStateAt = this.now();

        const prev = this.prev;
        this.prev = s;
        if (!prev || prev.encoders.length !== s.encoders.length) {
            const keysDown = [];
            for (let i = 0; i < W.PAD_MAX_KEYS; i++) if ((s.keys >>> i) & 1) keysDown.push(i + 1);
            this.#emit({ type: "baseline", keysDown });
            return;
        }

        let changed = false;
        const diff = (prev.keys ^ s.keys) >>> 0;
        for (let i = 0; i < W.PAD_MAX_KEYS; i++) {
            if (!((diff >>> i) & 1)) continue;
            changed = true;
            this.#emit({ type: "key", index: i + 1, down: ((s.keys >>> i) & 1) === 1 });
        }
        s.encoders.forEach((e, i) => {
            const delta = (e.position - prev.encoders[i].position) | 0;
            if (delta === 0) return;
            changed = true;
            if (Math.abs(delta) > TURN_RESET_DELTA) this.#emit({ type: "reset", encoder: i + 1, delta });
            else this.#emit({ type: "turn", encoder: i + 1, delta, interval: e.interval });
        });
        if (!changed && prev.encoders.every((e, i) => e.interval === s.encoders[i].interval)) this.counts.repeats++;
    }
}

function stamp(t) {
    return (t / 1000).toFixed(3).padStart(8);
}

export function formatEvent(e) {
    const head = `${stamp(e.t)}  `;
    switch (e.type) {
    case "hello": return `${head}HELLO version ${e.version}, ${e.keyCount} keys, ${e.encoderCount} encoders (seq ${e.seq})`;
    case "baseline": return `${head}baseline${e.keysDown.length ? ` (held: ${e.keysDown.map(k => `key${k}`).join(" ")})` : ""}`;
    case "key": return `${head}key${e.index} ${e.down ? "down" : "up"}`;
    case "turn": {
        const sat = e.interval >= W.PAD_INTERVAL_MAX ? ", saturated" : "";
        return `${head}enc${e.encoder} turn ${e.delta > 0 ? "+" : ""}${e.delta} (interval ${(e.interval / 1000).toFixed(1)} ms${sat})`;
    }
    case "reset": return `${head}enc${e.encoder} position jumped by ${e.delta}: baseline reset, not a turn`;
    case "offline": return `${head}pad offline (no STATE for ${W.PAD_OFFLINE_TIMEOUT_MS} ms)`;
    case "error": return `${head}decode error ${e.name}: ${e.message}`;
    case "bad": return `${head}bad payload length ${e.length} for cmd ${e.cmd}`;
    case "unknown": return `${head}unknown cmd ${e.cmd} (seq ${e.seq}, ${e.length} bytes)`;
    default: return `${head}${JSON.stringify(e)}`;
    }
}

export function formatStats(s) {
    const errs = Object.entries(s.errors).map(([k, v]) => `${k} ${v}`).join(", ");
    return `stats: ok ${s.ok} (states ${s.states}, hellos ${s.hellos}), errors ${errs || "none"}, `
        + `gaps ${s.seqGaps}, seq resets ${s.seqResets}, repeats ${s.repeats}, bad ${s.badPayload}, unknown ${s.unknown}`;
}
