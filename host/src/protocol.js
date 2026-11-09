// ControlBlock wire format (docs/protocol.md): constants, message framing and
// payload encoding. Pure functions, no I/O, so they run anywhere (Node or a
// browser) and are tested without a device.

export const USB_VID   = 0x303A;    // Espressif, with a development PID
export const USB_PID   = 0x4020;
export const INTERFACE = 0;         // the vendor interface
export const ENDPOINT  = 1;         // bulk OUT 0x01 / IN 0x81
export const USB_PACKET_SIZE = 64;  // full-speed bulk packet: a write of a whole number of these is ended with a ZLP

export const PROTOCOL_VERSION  = 1;
export const MAGIC             = 0x4243;
export const HEADER_SIZE       = 12;
export const REQ_RESET_STREAM  = 0x10;
export const KEYDEF_SIZE       = 20;
export const PUT_IMAGE_PREFIX  = 12;
export const LAYOUT_DATA_PREFIX = 4;       // LAYOUT_DATA: u32 offset, then the bytes

export const STATUS_TEXT_MAX_BYTES = 47;        // SET_STATUS_TEXT: bytes of UTF-8 per text
export const StatusText = Object.freeze({ LEFT: 0, RIGHT: 1 });

// Wi-Fi credentials (docs/wifi.md)
export const WIFI_SSID_MAX_BYTES = 32;
export const WIFI_PASS_MIN_BYTES = 8;
export const WIFI_PASS_MAX_BYTES = 63;
export const WifiState = Object.freeze({ NO_CREDENTIALS: 0, CONNECTING: 1, CONNECTED: 2, RETRYING: 3 });

// Clock (docs/protocol.md, Clock)
export const TIME_VALID_FROM_MS = Date.UTC(2025, 0, 1);    // earlier reads as "not known"; TIME_SET refuses it
export const CLOCK_FIELD_MAX_BYTES = 63;                   // CLOCK_SET: the time zone and the NTP server
export const ClockSource = Object.freeze({ NONE: 0, HOST: 1, NTP: 2 });
const ClockSetMask = Object.freeze({ TZ: 0x01, SERVER: 0x02 });

// Device name (docs/protocol.md, Device name): the mDNS name and DHCP hostname
export const DEVICE_NAME_MAX_BYTES = 32;

export const Flags = Object.freeze({
    RESPONSE: 0x01,
    EVENT:    0x02,
});

export const Msg = Object.freeze({
    PING:           0x00,
    GET_INFO:       0x01,
    RESET:          0x02,
    SET_STATUS_TEXT: 0x03,
    SET_BACKLIGHT:  0x05,
    TAKE_CONTROL:   0x06,
    RELEASE_CONTROL: 0x07,
    PUT_IMAGE:      0x10,
    FREE_IMAGES:    0x11,
    QUERY_IMAGES:   0x12,
    SET_PAGE_COUNT: 0x20,
    SET_KEYS:       0x21,
    SET_PAGE:       0x22,
    LAYOUT_BEGIN:   0x30,
    LAYOUT_DATA:    0x31,
    LAYOUT_COMMIT:  0x32,
    WIFI_SET:       0x40,
    WIFI_GET:       0x41,
    WIFI_STATUS:    0x42,
    TIME_SET:       0x50,
    TIME_GET:       0x51,
    CLOCK_SET:      0x52,
    NAME_SET:       0x60,
    NAME_GET:       0x61,
    KEY:           0x80,       // events
    PAGE_CHANGED:   0x81,
    WAKE:           0x82,
    HEARTBEAT:      0x83,
    WIFI_STATE:     0x84,
});

// GET_INFO flags
export const InfoFlags = Object.freeze({ BACKLIGHT: 0x0001, WIFI_CREDENTIALS: 0x0002, HOSTED: 0x0004, TIME_SET: 0x0008 });

export const Status = Object.freeze({
    OK:           0,
    UNKNOWN_TYPE: 1,
    BAD_LENGTH:   2,
    BAD_ARG:      3,
    NO_SPACE:     4,
    BAD_IMAGE:    5,
    NOT_FOUND:    6,
    BAD_SEQUENCE: 8,
    BAD_CRC:      9,
    IO_ERROR:     10,
    NOT_HOSTED:   11,
});

export const KeyAction = Object.freeze({ PRESS: 0, RELEASE: 1, CANCEL: 2 });
export const PageCause = Object.freeze({ USER: 0, HOST: 1 });
export const KeyFlags  = Object.freeze({ EMPTY: 0x0001, DISABLED: 0x0002 });

export function statusName(status)
{
    return enumName(Status, status, "STATUS");
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

// CRC-32 as zlib's crc32() (the device's layout check: firmware/main/crc32_table.h).
// Pass the previous result as `crc` to chain pieces. Always unsigned.
const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
        let c = i;
        for (let k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
        t[i] = c >>> 0;
    }
    return t;
})();

export function crc32(data, crc = 0)
{
    crc = ~crc >>> 0;
    for (let i = 0; i < data.length; i++)
        crc = CRC_TABLE[(crc ^ data[i]) & 0xFF] ^ (crc >>> 8);
    return ~crc >>> 0;
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
    i64(v) { this.view.setBigInt64(this.pos, BigInt(v), true); this.pos += 8; return this; }
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
    i64(def = 0) { if (!this.has(8)) return def; const v = Number(this.view.getBigInt64(this.pos, true)); this.pos += 8; return v; }
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

// SET_STATUS_TEXT: one field ("" clears it). The device takes at most
// STATUS_TEXT_MAX_BYTES bytes of UTF-8; this throws rather than cut.
export function encodeStatusText(field, text)
{
    if (typeof text !== "string")
        throw new TypeError("Status text must be a string");
    if (text.includes("\0"))
        throw new TypeError("Status text can't contain NUL");
    const bytes = new TextEncoder().encode(text);
    if (bytes.length > STATUS_TEXT_MAX_BYTES)
        throw new RangeError(`Status text is ${bytes.length} bytes; the limit is ${STATUS_TEXT_MAX_BYTES}`);
    return new Writer(2 + bytes.length).u8(field).u8(bytes.length).raw(bytes).done();
}

// WIFI_SET: the ssid (a string, or raw bytes) and password (a string; "" or
// missing = an open network). An empty ssid clears. Throws rather than send
// what the device would refuse.
export function encodeWifiSet(ssid, password = "")
{
    const ssidBytes = typeof ssid === "string" ? new TextEncoder().encode(ssid) : ssid;
    if (!(ssidBytes instanceof Uint8Array))
        throw new TypeError("The ssid must be a string or bytes");
    if (typeof password !== "string")
        throw new TypeError("The password must be a string");
    const pass = new TextEncoder().encode(password);
    if (ssidBytes.length > WIFI_SSID_MAX_BYTES)
        throw new RangeError(`The ssid is ${ssidBytes.length} bytes; the limit is ${WIFI_SSID_MAX_BYTES}`);
    if (pass.length && (pass.length < WIFI_PASS_MIN_BYTES || pass.length > WIFI_PASS_MAX_BYTES))
        throw new RangeError(`The password is ${pass.length} bytes; it must be ${WIFI_PASS_MIN_BYTES} to ${WIFI_PASS_MAX_BYTES} (or empty for an open network)`);
    if (!ssidBytes.length && pass.length)
        throw new TypeError("A password needs an ssid");
    if (ssidBytes.includes(0) || pass.includes(0))
        throw new TypeError("The ssid and password can't contain NUL");
    return new Writer(2 + ssidBytes.length + pass.length).u8(ssidBytes.length).u8(pass.length).raw(ssidBytes).raw(pass).done();
}

// TIME_SET: a Date or UTC milliseconds. Throws for a time the device would
// refuse (before 2025, when its clock reads "not known").
export function encodeTimeSet(time)
{
    const ms = time instanceof Date ? time.getTime() : time;
    if (typeof ms !== "number")
        throw new TypeError("The time must be a Date or milliseconds");
    if (!(ms >= TIME_VALID_FROM_MS) || !Number.isSafeInteger(Math.trunc(ms)))
        throw new RangeError(`The time must be 2025-01-01 or later (got ${time})`);
    return new Writer(8).i64(Math.trunc(ms)).done();
}

// CLOCK_SET: { timeZone, ntpServer }, either or both; "" sets one back to the
// device's default (UTC0, pool.ntp.org). Throws rather than send what the
// device would refuse.
const CLOCK_FIELDS = [
    // name, mask bit, allowed characters (all ASCII, so length = bytes) and how to say them
    ["timeZone",  ClockSetMask.TZ,     /^[\x21-\x7E]*$/,     "printable ASCII without spaces"],
    ["ntpServer", ClockSetMask.SERVER, /^[A-Za-z0-9.-]*$/,  "A-Z a-z 0-9 . -"],
];

export function encodeClockSet(settings)
{
    for (const name of Object.keys(settings))
        if (!CLOCK_FIELDS.some(([field]) => field === name))
            throw new TypeError(`Unknown clock setting "${name}" (timeZone or ntpServer)`);
    let mask = 0;
    const parts = [];
    for (const [name, bit, chars, charsText] of CLOCK_FIELDS) {
        const value = settings[name];
        if (value === undefined)
            continue;
        if (typeof value !== "string")
            throw new TypeError(`${name} must be a string`);
        if (!chars.test(value))
            throw new RangeError(`${name} "${value}" must be ${charsText}`);
        if (value.length > CLOCK_FIELD_MAX_BYTES)
            throw new RangeError(`${name} is ${value.length} bytes; the limit is ${CLOCK_FIELD_MAX_BYTES}`);
        mask |= bit;
        parts.push(new TextEncoder().encode(value));
    }
    if (!mask)
        throw new TypeError("Nothing to set: give timeZone and/or ntpServer");
    const w = new Writer(1 + parts.reduce((n, p) => n + 1 + p.length, 0)).u8(mask);
    for (const p of parts)
        w.u8(p.length).raw(p);
    return w.done();
}

// NAME_SET: a hostname label (A-Z are lowercased, as the device does), or ""
// for the default. Throws rather than send what the device would refuse.
export function encodeNameSet(name)
{
    if (typeof name !== "string")
        throw new TypeError("The name must be a string");
    const lower = name.toLowerCase();
    if (lower && !/^[a-z0-9]([a-z0-9-]*[a-z0-9])?$/.test(lower))
        throw new RangeError(`The name "${name}" must be a-z 0-9 - and not start or end with -`);
    if (lower.length > DEVICE_NAME_MAX_BYTES)
        throw new RangeError(`The name is ${lower.length} bytes; the limit is ${DEVICE_NAME_MAX_BYTES}`);
    return new Writer(1 + lower.length).u8(lower.length).raw(new TextEncoder().encode(lower)).done();
}

// One PUT_IMAGE chunk
export function encodePutImage(id, total, offset, chunk)
{
    return new Writer(PUT_IMAGE_PREFIX + chunk.length).u32(id).u32(total).u32(offset).raw(chunk).done();
}

// LAYOUT_BEGIN: the blob's size and CRC-32
export function encodeLayoutBegin(size, crc)
{
    return new Writer(8).u32(size).u32(crc).done();
}

// One LAYOUT_DATA chunk
export function encodeLayoutData(offset, chunk)
{
    return new Writer(LAYOUT_DATA_PREFIX + chunk.length).u32(offset).raw(chunk).done();
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
    info.protocolVersion   = r.u16();
    const flags            = r.u16();
    info.backlight         = (flags & InfoFlags.BACKLIGHT) !== 0;
    info.wifiCredentials   = (flags & InfoFlags.WIFI_CREDENTIALS) !== 0;
    info.hosted            = (flags & InfoFlags.HOSTED) !== 0;
    info.timeSet           = (flags & InfoFlags.TIME_SET) !== 0;
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
    info.layoutCapacity    = r.u32();      // 0 from a device without layout storage
    info.layoutSize        = r.u32();      // 0: nothing stored
    info.layoutCrc         = r.u32();
    return info;
}

// WIFI_GET response data: { ssid, password, ssidBytes } (all empty if none are stored)
export function decodeWifiCredentials(data)
{
    const r = new Reader(data);
    const ssidLen = r.u8(), passLen = r.u8();
    const ssidBytes = data.slice(r.pos, r.pos + ssidLen);
    const pass = data.subarray(r.pos + ssidLen, r.pos + ssidLen + passLen);
    return { ssid: new TextDecoder().decode(ssidBytes), password: new TextDecoder().decode(pass), ssidBytes };
}

// The name of `value` in an enum table, or `${prefix}_${value}` for one it doesn't have
function enumName(table, value, prefix)
{
    return Object.keys(table).find(k => table[k] === value) ?? `${prefix}_${value}`;
}

const wifiStateName = state => enumName(WifiState, state, "STATE");

function ipString(state, ip)
{
    return state === WifiState.CONNECTED ? `${ip[0]}.${ip[1]}.${ip[2]}.${ip[3]}` : null;
}

// WIFI_STATUS response data. `ip` is null unless connected.
export function decodeWifiStatus(data)
{
    const r = new Reader(data);
    const state = r.u8(), reason = r.u8();
    const rssi = r.u8() << 24 >> 24;
    r.u8();                             // reserved
    const ip = data.subarray(r.pos, r.pos + 4);
    const mac = [...data.subarray(r.pos + 4, r.pos + 10)].map(b => b.toString(16).padStart(2, "0")).join(":");
    return { state, stateName: wifiStateName(state), reason, rssi, ip: ipString(state, ip), mac };
}

// TIME_GET response data: { known, source, sourceName, time, timeZone, ntpServer }.
// `time` (a Date) is null while the clock isn't known; `source` is ClockSource.
export function decodeTime(data)
{
    const r = new Reader(data);
    const known = r.u8() !== 0, source = r.u8(), ms = r.i64();
    return {
        known, source, sourceName: enumName(ClockSource, source, "SOURCE"),
        time: known ? new Date(ms) : null,
        timeZone: r.str(), ntpServer: r.str(),
    };
}

// NAME_GET response data: { name, isDefault }
export function decodeName(data)
{
    const r = new Reader(data);
    const isDefault = r.u8() !== 0;
    return { name: r.str(), isDefault };
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

// How often the device sends a HEARTBEAT (the firmware's PROTO_HEARTBEAT_MS).
// A bulk IN read that times out is cancelled, and a cancelled read leaves the
// pipe deaf on Windows, so the host's read timeout must be well past this.
export const HEARTBEAT_INTERVAL_MS = 1000;

// An event message: { type: "key", page, key, action, lost, timeMs } or
// { type: "pageChanged", page, cause } or { type: "wake", lost, timeMs } or
// { type: "heartbeat", timeMs } or { type: "wifiState", state, stateName, lost, ip },
// or null if unknown
export function decodeEvent(msg)
{
    const r = new Reader(msg.payload);
    if (msg.type === Msg.KEY) {
        const page = r.u8(), key = r.u8(), action = r.u8(), lost = (r.u8() & 1) !== 0;
        return { type: "key", seq: msg.seq, page, key, action, lost, timeMs: r.u32() };
    }
    if (msg.type === Msg.PAGE_CHANGED)
        return { type: "pageChanged", seq: msg.seq, page: r.u8(), cause: r.u8() };
    if (msg.type === Msg.WAKE) {
        const lost = (r.u8() & 1) !== 0;
        r.pos += 3;                     // reserved
        return { type: "wake", seq: msg.seq, lost, timeMs: r.u32() };
    }
    if (msg.type === Msg.HEARTBEAT)
        return { type: "heartbeat", seq: msg.seq, timeMs: r.u32() };
    if (msg.type === Msg.WIFI_STATE) {
        const state = r.u8(), lost = (r.u8() & 1) !== 0;
        r.pos += 2;                     // reserved
        return { type: "wifiState", seq: msg.seq, state, stateName: wifiStateName(state), lost, ip: ipString(state, msg.payload.subarray(r.pos, r.pos + 4)) };
    }
    return null;
}
