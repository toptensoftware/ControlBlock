// Macro pad wire format in JS: the twin of pad/common/pad_protocol.h/.cpp. The golden
// bytes in test/pad_wire.test.js (and pad/common/pad_golden.md) keep the two in step.
//
// Packets are PacketEncoder packets (PacketEncoder/packenc.js). Payloads, little-endian:
//   STATE  u32 keys (bit n = key n+1 down), then per encoder u32 position + u16 interval
//          (microseconds, saturating); length = 4 + 6 * encoders.
//   HELLO  u8 version, u8 key count, u8 encoder count.
import { packet_encode, packet_decoder } from "../../../PacketEncoder/packenc.js";

export { packet_decoder };

export const PAD_MAX_KEYS = 32;
export const PAD_MAX_ENCODERS = 4;

export const PAD_CMD_STATE = 1;
export const PAD_CMD_HELLO = 2;

export const PAD_PROTOCOL_VERSION = 1;

// Timing (starting points, tune on the hardware)
export const PAD_STATE_REPEAT_MS = 100;
export const PAD_OFFLINE_TIMEOUT_MS = 300;
export const PAD_ENCODER_MIN_GAP_MS = 5;
export const PAD_HELLO_COUNT = 3;
export const PAD_HELLO_GAP_MS = 50;

export const PAD_STATE_BASE_BYTES = 4;
export const PAD_STATE_ENCODER_BYTES = 6;
export const PAD_STATE_MAX_BYTES = PAD_STATE_BASE_BYTES + PAD_STATE_ENCODER_BYTES * PAD_MAX_ENCODERS;
export const PAD_HELLO_BYTES = 3;
export const PAD_PAYLOAD_MAX_BYTES = PAD_STATE_MAX_BYTES;   // the decoder's payload buffer size

export const PAD_INTERVAL_MAX = 65535;

export function stateBytes(encoders) {
    return PAD_STATE_BASE_BYTES + PAD_STATE_ENCODER_BYTES * encoders;
}

// state = { keys, encoders: [{ position, interval }] }. Fields wrap (u32 keys/position) or
// saturate (u16 interval) to their wire widths.
export function buildState(state) {
    const n = state.encoders.length;
    if (n > PAD_MAX_ENCODERS) throw new RangeError(`too many encoders: ${n}`);
    const buf = Buffer.alloc(stateBytes(n));
    buf.writeUInt32LE(state.keys >>> 0, 0);
    state.encoders.forEach((e, i) => {
        const o = PAD_STATE_BASE_BYTES + PAD_STATE_ENCODER_BYTES * i;
        buf.writeUInt32LE(e.position >>> 0, o);
        buf.writeUInt16LE(Math.max(0, Math.min(PAD_INTERVAL_MAX, e.interval)), o + 4);
    });
    return buf;
}

// Returns the state, or null for any length other than 4 + 6n, n = 0..4
export function parseState(payload) {
    const len = payload.length;
    if (len < PAD_STATE_BASE_BYTES || (len - PAD_STATE_BASE_BYTES) % PAD_STATE_ENCODER_BYTES !== 0) return null;
    const n = (len - PAD_STATE_BASE_BYTES) / PAD_STATE_ENCODER_BYTES;
    if (n > PAD_MAX_ENCODERS) return null;
    const encoders = [];
    for (let i = 0; i < n; i++) {
        const o = PAD_STATE_BASE_BYTES + PAD_STATE_ENCODER_BYTES * i;
        encoders.push({ position: payload.readUInt32LE(o), interval: payload.readUInt16LE(o + 4) });
    }
    return { keys: payload.readUInt32LE(0), encoders };
}

// hello = { version, keyCount, encoderCount }
export function buildHello(hello) {
    return Buffer.from([hello.version, hello.keyCount, hello.encoderCount]);
}

export function parseHello(payload) {
    if (payload.length !== PAD_HELLO_BYTES) return null;
    return { version: payload[0], keyCount: payload[1], encoderCount: payload[2] };
}

// One whole packet (start signal to terminator) as a Buffer
export function encodePacket(seq, cmd, payload) {
    const out = [];
    packet_encode(b => out.push(b), seq, cmd, payload);
    return Buffer.from(out);
}
