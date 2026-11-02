// ControlBlock wire format (docs/protocol.md): constants, message framing and
// payload encoding. Pure functions, no I/O, so they run anywhere (Node or a
// browser) and are tested without a device.

export const USB_VID   = 0x303A;    // Espressif, with a development PID
export const USB_PID   = 0x4020;
export const INTERFACE = 0;         // the vendor interface
export const ENDPOINT  = 1;         // bulk OUT 0x01 / IN 0x81

export const PROTOCOL_VERSION  = 1;
export const MAGIC             = 0x4243;
export const HEADER_SIZE       = 12;
export const REQ_RESET_STREAM  = 0x10;
export const KEYDEF_SIZE       = 20;
export const PUT_IMAGE_PREFIX  = 12;

export const Flags = Object.freeze({
    RESPONSE: 0x01,
    EVENT:    0x02,
});

export const Msg = Object.freeze({
    PING:           0x00,
    GET_INFO:       0x01,
    RESET:          0x02,
    SET_BACKLIGHT:  0x05,
    PUT_IMAGE:      0x10,
    FREE_IMAGES:    0x11,
    QUERY_IMAGES:   0x12,
    SET_PAGE_COUNT: 0x20,
    SET_KEYS:       0x21,
    SET_PAGE:       0x22,
    KEY:            0x80,       // events
    PAGE_CHANGED:   0x81,
});

export const Status = Object.freeze({
    OK:           0,
    UNKNOWN_TYPE: 1,
    BAD_LENGTH:   2,
    BAD_ARG:      3,
    NO_SPACE:     4,
    BAD_IMAGE:    5,
    NOT_FOUND:    6,
    BAD_SEQUENCE: 8,
});

export const KeyAction = Object.freeze({ DOWN: 0, UP: 1, CANCEL: 2 });
export const PageCause = Object.freeze({ USER: 0, HOST: 1 });
export const KeyFlags  = Object.freeze({ EMPTY: 0x0001, DISABLED: 0x0002 });

export function statusName(status)
{
    for (const [name, value] of Object.entries(Status))
        if (value === status)
            return name;
    return `STATUS_${status}`;
}

// A request (or any message): header + payload
export function encodeMessage(type, seq, payload = new Uint8Array(0), flags = 0)
{
    const out = new Uint8Array(HEADER_SIZE + payload.length);
    const v = new DataView(out.buffer);
    v.setUint16(0, MAGIC, true);
    v.setUint8(2, type);
    v.setUint8(3, flags);
    v.setUint16(4, seq & 0xFFFF, true);
    v.setUint16(6, 0, true);
    v.setUint32(8, payload.length, true);
    out.set(payload, HEADER_SIZE);
    return out;
}

// Splits the device's byte stream into messages. push() returns the complete
// ones. The device never splits a message, but if bytes go missing (it gives
// up writing to a host that stopped reading), a bad header is skipped a byte
// at a time until the next one that looks right; `resyncs` counts them.
export class MessageParser
{
    constructor(maxPayload = 65536)
    {
        this.maxPayload = maxPayload;
        this.resyncs = 0;
        this.reset();
    }

    reset()
    {
        this.buf = new Uint8Array(0);
    }

    push(bytes)
    {
        const joined = new Uint8Array(this.buf.length + bytes.length);
        joined.set(this.buf, 0);
        joined.set(bytes, this.buf.length);

        const msgs = [];
        let pos = 0, skipping = false;
        while (joined.length - pos >= HEADER_SIZE) {
            const v = new DataView(joined.buffer, joined.byteOffset + pos, HEADER_SIZE);
            const len = v.getUint32(8, true);
            if (v.getUint16(0, true) !== MAGIC || len > this.maxPayload) {
                if (!skipping)
                    this.resyncs++;
                skipping = true;
                pos++;
                continue;
            }
            skipping = false;
            if (joined.length - pos < HEADER_SIZE + len)
                break;
            msgs.push({
                type:    v.getUint8(2),
                flags:   v.getUint8(3),
                seq:     v.getUint16(4, true),
                payload: joined.slice(pos + HEADER_SIZE, pos + HEADER_SIZE + len),
            });
            pos += HEADER_SIZE + len;
        }
        this.buf = joined.slice(pos);
        return msgs;
    }
}

// Little-endian payload builder
export class Writer
{
    constructor(size)
    {
        this.bytes = new Uint8Array(size);
        this.view = new DataView(this.bytes.buffer);
        this.pos = 0;
    }
    u8(v)  { this.view.setUint8(this.pos, v); this.pos += 1; return this; }
    u16(v) { this.view.setUint16(this.pos, v, true); this.pos += 2; return this; }
    u32(v) { this.view.setUint32(this.pos, v >>> 0, true); this.pos += 4; return this; }
    raw(b) { this.bytes.set(b, this.pos); this.pos += b.length; return this; }
    done() { return this.bytes.subarray(0, this.pos); }
}

// Little-endian payload reader; reads past the end give `def` (fields missing
// from the end of a payload take their default)
export class Reader
{
    constructor(bytes, pos = 0)
    {
        this.bytes = bytes;
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        this.pos = pos;
    }
    has(n) { return this.pos + n <= this.bytes.length; }
    u8(def = 0)  { if (!this.has(1)) return def; const v = this.view.getUint8(this.pos); this.pos += 1; return v; }
    u16(def = 0) { if (!this.has(2)) return def; const v = this.view.getUint16(this.pos, true); this.pos += 2; return v; }
    u32(def = 0) { if (!this.has(4)) return def; const v = this.view.getUint32(this.pos, true); this.pos += 4; return v; }
    str()
    {
        const n = this.u8();
        const s = new TextDecoder().decode(this.bytes.subarray(this.pos, this.pos + n));
        this.pos += n;
        return s;
    }
}

// --- Payloads ---

export function encodeIds(ids)
{
    const w = new Writer(2 + ids.length * 4).u16(ids.length);
    for (const id of ids)
        w.u32(id);
    return w.done();
}

// One PUT_IMAGE chunk
export function encodePutImage(id, total, offset, chunk)
{
    return new Writer(PUT_IMAGE_PREFIX + chunk.length).u32(id).u32(total).u32(offset).raw(chunk).done();
}

// keys: [{ page, key, bg, fg, pressedBg, pressedFg, disabled, empty }], IDs 0/undefined = none
export function encodeKeys(keys)
{
    const w = new Writer(2 + keys.length * KEYDEF_SIZE).u16(keys.length);
    for (const k of keys) {
        const flags = (k.empty ? KeyFlags.EMPTY : 0) | (k.disabled ? KeyFlags.DISABLED : 0);
        w.u8(k.page).u8(k.key).u16(flags);
        for (const id of [k.bg, k.fg, k.pressedBg, k.pressedFg])
            w.u32(k.empty ? 0 : (id ?? 0));
    }
    return w.done();
}

// GET_INFO response data (after the status)
export function decodeInfo(data)
{
    const r = new Reader(data);
    const info = {};
    info.protocolVersion   = r.u16(); r.u16();
    info.maxPayload        = r.u32();
    info.imageCapacity     = r.u32();
    info.imageUsed         = r.u32();
    info.imageCount        = r.u32();
    info.imageLargestFree  = r.u32();
    info.screenWidth       = r.u16();
    info.screenHeight      = r.u16();
    info.cols              = r.u8();
    info.rows              = r.u8();
    info.maxPages          = r.u8();
    info.pageCount         = r.u8();
    info.keyWidth          = r.u16();
    info.keyHeight         = r.u16();
    info.iconWidth         = r.u16();
    info.iconHeight        = r.u16();
    info.currentPage       = r.u8();
    info.firmwareVersion   = r.str();
    info.serial            = r.str();
    return info;
}

// QUERY_IMAGES response data: one bool per ID asked about
export function decodeQuery(data)
{
    const r = new Reader(data);
    const count = r.u16();
    const present = [];
    for (let i = 0; i < count; i++)
        present.push((data[2 + (i >> 3)] >> (i & 7) & 1) === 1);
    return present;
}

// An event message: { type: "key", page, key, action, lost, timeMs } or
// { type: "pageChanged", page, cause }, or null if unknown
export function decodeEvent(msg)
{
    const r = new Reader(msg.payload);
    if (msg.type === Msg.KEY) {
        const page = r.u8(), key = r.u8(), action = r.u8(), lost = (r.u8() & 1) !== 0;
        return { type: "key", seq: msg.seq, page, key, action, lost, timeMs: r.u32() };
    }
    if (msg.type === Msg.PAGE_CHANGED)
        return { type: "pageChanged", seq: msg.seq, page: r.u8(), cause: r.u8() };
    return null;
}
