import test from "node:test";
import assert from "node:assert/strict";
import {
    encodeMessage, MessageParser, encodeKeys, encodeIds, encodePutImage, decodeInfo, decodeQuery, decodeEvent,
    Msg, Flags, KeyFlags, HEADER_SIZE, KEYDEF_SIZE, MAGIC, statusName, Status,
    encodeStatusText, StatusText, STATUS_TEXT_MAX_BYTES,
    crc32, encodeLayoutBegin, encodeLayoutData, LAYOUT_DATA_PREFIX, HEARTBEAT_INTERVAL_MS,
    InfoFlags, Writer, WifiState, WIFI_SSID_MAX_BYTES, WIFI_PASS_MIN_BYTES, WIFI_PASS_MAX_BYTES,
    encodeWifiSet, decodeWifiCredentials, decodeWifiStatus,
    ClockSource, TIME_VALID_FROM_MS, CLOCK_FIELD_MAX_BYTES, encodeTimeSet, decodeTime, encodeClockSet,
    DEVICE_NAME_MAX_BYTES, encodeNameSet, decodeName,
    KEY_EVENT_SIZE, keySlot, keyPixelSize, layoutBlobVersion, BPAK_SIGNATURE, BPAK_HEADER_SIZE,
} from "../src/protocol.js";
import { BLOB_VERSION } from "../src/bake/consts.js";
import { infoPayload, layoutBlobOf } from "./FakeDevice.js";

test("a message survives being split anywhere", () => {
    const a = encodeMessage(Msg.PING, 7, Uint8Array.of(1, 2, 3));
    const b = encodeMessage(Msg.RESET, 8);
    const stream = new Uint8Array([...a, ...b]);
    for (const piece of [1, 2, 5, 13, 100]) {
        const p = new MessageParser();
        const msgs = [];
        for (let i = 0; i < stream.length; i += piece)
            msgs.push(...p.push(stream.subarray(i, i + piece)));
        assert.equal(msgs.length, 2);
        assert.deepEqual([...msgs[0].payload], [1, 2, 3]);
        assert.equal(msgs[0].seq, 7);
        assert.equal(msgs[1].type, Msg.RESET);
        assert.equal(msgs[1].payload.length, 0);
    }
});

test("the header is little endian with the CB magic", () => {
    const m = encodeMessage(0x21, 0x1234, new Uint8Array(5), Flags.RESPONSE);
    assert.equal(m.length, HEADER_SIZE + 5);
    assert.deepEqual([...m.subarray(0, HEADER_SIZE)], [0x43, 0x42, 0x21, 0x01, 0x34, 0x12, 0, 0, 5, 0, 0, 0]);
    assert.equal(m[0] | m[1] << 8, MAGIC);
});

test("the parser skips garbage to the next message", () => {
    const p = new MessageParser();
    const good = encodeMessage(Msg.PING, 1, Uint8Array.of(9));
    const msgs = p.push(new Uint8Array([0xFF, 0x00, 0x13, ...good]));
    assert.equal(msgs.length, 1);
    assert.equal(msgs[0].payload[0], 9);
    assert.equal(p.resyncs, 1);
});

test("key definitions match the firmware's v2 KeyDef: id, page, quarter-unit rect, flags, two images", () => {
    const b = encodeKeys([
        { id: 0x1234, page: 2, x: 1.25, y: 2, w: 1.5, h: 0.75, normal: 0x11223344, pressed: 5 },
        { id: 7, remove: true, page: 3, x: 1, normal: 9 },
        { id: 65535, page: 1, x: 4, y: 0, normal: { id: 8, data: Uint8Array.of(1) }, disabled: true },
    ]);
    assert.equal(KEYDEF_SIZE, 20);
    assert.equal(b.length, 2 + 3 * KEYDEF_SIZE);
    const v = new DataView(b.buffer, b.byteOffset);
    assert.equal(v.getUint16(0, true), 3);
    const at = i => 2 + i * KEYDEF_SIZE;
    assert.deepEqual([...b.subarray(at(0), at(0) + 8)], [0x34, 0x12, 2, 5, 8, 6, 3, 0]);   // id, page, x y w h (quarters), reserved
    assert.equal(v.getUint16(at(0) + 8, true), 0);              // flags
    assert.equal(v.getUint16(at(0) + 10, true), 0);             // reserved
    assert.equal(v.getUint32(at(0) + 12, true), 0x11223344);
    assert.equal(v.getUint32(at(0) + 16, true), 5);
    // REMOVE: only the id (everything else zero)
    assert.deepEqual([...b.subarray(at(1), at(2))], [7, 0, 0, 0, 0, 0, 0, 0, KeyFlags.REMOVE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
    // w and h default to 1U, y to 0; an image may be given as { id } (keyImage's result)
    assert.deepEqual([...b.subarray(at(2), at(2) + 8)], [0xFF, 0xFF, 1, 16, 0, 4, 4, 0]);
    assert.equal(v.getUint16(at(2) + 8, true), KeyFlags.DISABLED);
    assert.equal(v.getUint32(at(2) + 12, true), 8);
    assert.equal(v.getUint32(at(2) + 16, true), 0);             // no pressed image
    assert.deepEqual({ ...KeyFlags }, { REMOVE: 0x0001, DISABLED: 0x0002 });
});

test("key definitions refuse what can't be encoded, before anything is sent", () => {
    const ok = { id: 1, page: 0, x: 0, y: 0 };
    assert.equal(encodeKeys([ok]).length, 2 + KEYDEF_SIZE);
    assert.equal(encodeKeys([]).length, 2);
    for (const bad of [
        { ...ok, id: 0 }, { ...ok, id: 65536 }, { ...ok, id: 1.5 }, { ...ok, id: undefined },
        { ...ok, x: 0.3 }, { ...ok, y: -0.25 }, { ...ok, w: 0 }, { ...ok, h: -1 }, { ...ok, w: 1.1 },
        { ...ok, x: 64 }, { ...ok, w: "1" }, { ...ok, page: 256 }, { ...ok, page: -1 }, { ...ok, page: undefined },
        { ...ok, normal: -1 }, { ...ok, pressed: 2 ** 32 },
        { id: 0, remove: true },
    ])
        assert.throws(() => encodeKeys([ok, bad]), RangeError, JSON.stringify(bad));
});

test("keySlot places a 1U key in a plain grid slot; keyPixelSize is the image size a key needs", () => {
    assert.deepEqual(keySlot(0), { x: 0, y: 0, w: 1, h: 1 });
    assert.deepEqual(keySlot(7), { x: 2, y: 1, w: 1, h: 1 });
    assert.deepEqual(keySlot(14), { x: 4, y: 2, w: 1, h: 1 });
    assert.deepEqual(keySlot(5, 4), { x: 1, y: 1, w: 1, h: 1 });
    const info = decodeInfo(infoPayload());
    assert.deepEqual(keyPixelSize({}, info), { width: 144, height: 144 });
    assert.deepEqual(keyPixelSize({ w: 2 }, info), { width: 288, height: 144 });
    assert.deepEqual(keyPixelSize({ w: 1.5, h: 0.25 }, info), { width: 216, height: 36 });
    assert.throws(() => keyPixelSize({ w: 0.1 }, info), RangeError);
});

test("ids and image chunks", () => {
    const ids = encodeIds([1, 0xFFFFFFFF]);
    assert.deepEqual([...ids], [2, 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF]);
    const put = encodePutImage(3, 100, 40, Uint8Array.of(7, 8));
    assert.deepEqual([...put], [3, 0, 0, 0, 100, 0, 0, 0, 40, 0, 0, 0, 7, 8]);
});

test("status text is a field, a length and UTF-8", () => {
    assert.deepEqual([...encodeStatusText(StatusText.LEFT, "Hi")], [0, 2, 72, 105]);
    assert.deepEqual([...encodeStatusText(StatusText.RIGHT, "")], [1, 0]);
    assert.deepEqual([...encodeStatusText(StatusText.LEFT, "é")], [0, 2, 0xC3, 0xA9]);       // bytes, not characters
    assert.equal(encodeStatusText(StatusText.RIGHT, "x".repeat(STATUS_TEXT_MAX_BYTES)).length, 2 + STATUS_TEXT_MAX_BYTES);
    assert.throws(() => encodeStatusText(StatusText.LEFT, "x".repeat(STATUS_TEXT_MAX_BYTES + 1)), RangeError);
    assert.throws(() => encodeStatusText(StatusText.LEFT, "é".repeat(24)), RangeError);       // 48 bytes
    assert.throws(() => encodeStatusText(StatusText.LEFT, "a\0b"), TypeError);
    assert.throws(() => encodeStatusText(StatusText.LEFT, 12), TypeError);
});

test("get info decodes every field", () => {
    const info = decodeInfo(infoPayload({ maxPayload: 4096, pageCount: 3, currentPage: 2 }));
    assert.equal(info.protocolVersion, 2);
    assert.equal(info.maxPayload, 4096);
    assert.equal(info.imageCount, 0);
    assert.equal(info.screenWidth, 800);
    assert.equal(info.cols, 5);
    assert.equal(info.rows, 3);
    assert.equal(info.maxPages, 8);
    assert.equal(info.pageCount, 3);
    assert.equal(info.keyWidth, 144);
    assert.equal(info.keyHeight, 144);
    assert.equal(info.iconWidth, 80);
    assert.equal(info.currentPage, 2);
    assert.equal(info.firmwareVersion, "1.0");
    assert.equal(info.serial, "SERIAL1");
    // v2's fields, after the layout's
    assert.equal(info.gridX, 40);
    assert.equal(info.gridY, 44);
    assert.equal(info.gridWidth, 720);
    assert.equal(info.gridHeight, 432);
    assert.equal(info.quarter, 36);
    assert.equal(info.maxImageWidth, 720);
    assert.equal(info.maxImageHeight, 432);
    assert.equal(info.maxKeys, 32);
    // appended later in v2: the layout blob version the firmware loads
    assert.equal(info.layoutBlobVersion, BLOB_VERSION);
});

test("get info from older firmware has no layout blob version", () => {
    const info = decodeInfo(infoPayload({ blobVersion: null }));
    assert.equal(info.layoutBlobVersion, undefined);
    assert.equal(info.maxKeys, 32);                         // the fields before it are all there
    assert.equal(decodeInfo(infoPayload({ blobVersion: 7 })).layoutBlobVersion, 7);
});

test("a layout blob's version is the root's first field, after the BPAK header", () => {
    assert.equal(BPAK_HEADER_SIZE, 32);
    assert.equal(BPAK_SIGNATURE, 0x4B415042);               // "BPAK"
    assert.equal(layoutBlobVersion(layoutBlobOf(100)), BLOB_VERSION);
    assert.equal(layoutBlobVersion(layoutBlobOf(100, { version: 9 })), 9);
    const v = layoutBlobOf(100, { version: 0x01020304 });
    assert.deepEqual([...v.subarray(0, 4)], [0x42, 0x50, 0x41, 0x4B]);
    assert.deepEqual([...v.subarray(32, 36)], [4, 3, 2, 1]);  // little-endian
    // not a layout blob: no version
    assert.equal(layoutBlobVersion(new Uint8Array(100)), undefined);
    assert.equal(layoutBlobVersion(layoutBlobOf(100).subarray(0, 35)), undefined);     // too short for the version
    assert.equal(layoutBlobVersion(new Uint8Array(0)), undefined);
    // a view into a bigger buffer, at an odd offset
    const big = new Uint8Array(103);
    big.set(layoutBlobOf(100, { version: 5 }), 3);
    assert.equal(layoutBlobVersion(big.subarray(3)), 5);
});

test("query results are a bitmap", () => {
    assert.deepEqual(decodeQuery(Uint8Array.of(10, 0, 0b10000101, 0b10)),
                     [true, false, true, false, false, false, false, true, false, true]);
});

test("events", () => {
    // KEY (v2, 12 bytes): u16 id, u8 page, u8 action, u8 lost, u8[3] reserved, u32 time
    const key = decodeEvent({ type: Msg.KEY, seq: 4, payload: Uint8Array.of(0x34, 0x12, 1, 2, 1, 0, 0, 0, 0x10, 0x27, 0, 0) });
    assert.deepEqual(key, { type: "key", seq: 4, id: 0x1234, page: 1, action: 2, lost: true, timeMs: 10000 });
    assert.equal(KEY_EVENT_SIZE, 12);
    const page = decodeEvent({ type: Msg.PAGE_CHANGED, seq: 5, payload: Uint8Array.of(3, 1) });
    assert.deepEqual(page, { type: "pageChanged", seq: 5, page: 3, cause: 1 });
    assert.equal(decodeEvent({ type: 0x99, seq: 0, payload: new Uint8Array(0) }), null);
});

test("wake event", () => {
    const wake = decodeEvent({ type: Msg.WAKE, seq: 9, payload: Uint8Array.of(1, 0, 0, 0, 0x10, 0x27, 0, 0) });
    assert.deepEqual(wake, { type: "wake", seq: 9, lost: true, timeMs: 10000 });
    const plain = decodeEvent({ type: Msg.WAKE, seq: 1, payload: Uint8Array.of(0, 0, 0, 0, 1, 0, 0, 0) });
    assert.deepEqual(plain, { type: "wake", seq: 1, lost: false, timeMs: 1 });
    assert.equal(Msg.WAKE, 0x82);
});

test("heartbeat event", () => {
    const hb = decodeEvent({ type: Msg.HEARTBEAT, seq: 4, payload: Uint8Array.of(0x10, 0x27, 0, 0) });
    assert.deepEqual(hb, { type: "heartbeat", seq: 4, timeMs: 10000 });
    assert.equal(Msg.HEARTBEAT, 0x83);
    assert.equal(HEARTBEAT_INTERVAL_MS, 1000);      // the firmware's PROTO_HEARTBEAT_MS
});

test("get info reports the backlight flag", () => {
    assert.equal(decodeInfo(infoPayload()).backlight, true);
    assert.equal(decodeInfo(infoPayload({ backlight: false })).backlight, false);
    assert.equal(decodeInfo(infoPayload({ backlight: false })).maxPayload, 8192);   // fields after it still line up
});

test("status names", () => {
    assert.equal(statusName(Status.NOT_FOUND), "NOT_FOUND");
    assert.equal(statusName(77), "STATUS_77");
});

test("crc32 is zlib's", async () => {
    const zlib = await import("node:zlib");
    assert.equal(crc32(new TextEncoder().encode("123456789")), 0xCBF43926);
    assert.equal(crc32(new Uint8Array(0)), 0);
    assert.equal(crc32(Uint8Array.of(0x61)), 0xE8B7BE43);
    const data = new Uint8Array(10000).map((_, i) => (i * 2654435761) >>> 24);
    assert.equal(crc32(data), zlib.crc32(data));
    // Chained across pieces, and always unsigned
    assert.equal(crc32(data.subarray(3000), crc32(data.subarray(0, 3000))), crc32(data));
    assert.ok(crc32(Uint8Array.of(1, 2, 3)) >= 0);
});

test("layout messages match the firmware's wire format", () => {
    assert.deepEqual([...encodeLayoutBegin(0x01020304, 0xAABBCCDD)], [4, 3, 2, 1, 0xDD, 0xCC, 0xBB, 0xAA]);
    const d = encodeLayoutData(0x100, Uint8Array.of(9, 8, 7));
    assert.deepEqual([...d], [0, 1, 0, 0, 9, 8, 7]);
    assert.equal(Msg.LAYOUT_BEGIN, 0x30);
    assert.equal(Msg.LAYOUT_DATA, 0x31);
    assert.equal(Msg.LAYOUT_COMMIT, 0x32);
    assert.equal(Status.BAD_CRC, 9);
    assert.equal(Status.IO_ERROR, 10);
    assert.equal(statusName(9), "BAD_CRC");
    assert.equal(LAYOUT_DATA_PREFIX, 4);
});

test("GET_INFO ends with the stored layout, and older devices have none", () => {
    const info = decodeInfo(infoPayload({ layout: { capacity: 4190208, size: 1234, crc: 0xDEADBEEF } }));
    assert.equal(info.layoutCapacity, 4190208);
    assert.equal(info.layoutSize, 1234);
    assert.equal(info.layoutCrc, 0xDEADBEEF);
    const none = decodeInfo(infoPayload());
    assert.equal(none.layoutCapacity, 0);
    assert.equal(none.layoutSize, 0);
    assert.equal(none.layoutCrc, 0);
    assert.equal(none.serial, "SERIAL1");
    assert.equal(none.quarter, 36);                         // the v2 fields after it still line up
});

// --- Wi-Fi (docs/wifi.md) ---

test("wifi message types and the info flag", () => {
    assert.equal(Msg.WIFI_SET, 0x40);
    assert.equal(Msg.WIFI_GET, 0x41);
    assert.equal(Msg.WIFI_STATUS, 0x42);
    assert.equal(Msg.WIFI_STATE, 0x84);
    assert.equal(InfoFlags.WIFI_CREDENTIALS, 0x0002);
    assert.deepEqual({ ...WifiState }, { NO_CREDENTIALS: 0, CONNECTING: 1, CONNECTED: 2, RETRYING: 3 });
});

test("get info reports whether wifi credentials are stored, independent of the backlight", () => {
    assert.equal(decodeInfo(infoPayload()).wifiCredentials, false);
    assert.equal(decodeInfo(infoPayload({ wifiCredentials: true })).wifiCredentials, true);
    const both = decodeInfo(infoPayload({ wifiCredentials: true, backlight: false }));
    assert.equal(both.wifiCredentials, true);
    assert.equal(both.backlight, false);
    assert.equal(both.maxPayload, 8192);
});

test("wifi set is two lengths, the ssid and the password", () => {
    assert.deepEqual([...encodeWifiSet("Home", "password1")],
                     [4, 9, ...new TextEncoder().encode("Home"), ...new TextEncoder().encode("password1")]);
    assert.deepEqual([...encodeWifiSet("Cafe")], [4, 0, 67, 97, 102, 101]);            // open network
    assert.deepEqual([...encodeWifiSet("Cafe", "")], [4, 0, 67, 97, 102, 101]);
    assert.deepEqual([...encodeWifiSet("")], [0, 0]);                                  // clears
    assert.deepEqual([...encodeWifiSet("é", "")], [2, 0, 0xC3, 0xA9]);                 // bytes, not characters
    assert.deepEqual([...encodeWifiSet(Uint8Array.of(0xFF, 0xFE), "")], [2, 0, 0xFF, 0xFE]);   // raw ssid bytes
});

test("wifi set refuses what the device would refuse", () => {
    assert.throws(() => encodeWifiSet("x".repeat(WIFI_SSID_MAX_BYTES + 1)), RangeError);
    assert.equal(encodeWifiSet("x".repeat(WIFI_SSID_MAX_BYTES)).length, 2 + WIFI_SSID_MAX_BYTES);
    assert.throws(() => encodeWifiSet("é".repeat(17)), RangeError);                    // 34 bytes
    assert.throws(() => encodeWifiSet("Home", "short"), RangeError);                   // 1..7
    assert.throws(() => encodeWifiSet("Home", "x".repeat(WIFI_PASS_MIN_BYTES - 1)), RangeError);
    assert.equal(encodeWifiSet("Home", "x".repeat(WIFI_PASS_MIN_BYTES)).length, 2 + 4 + WIFI_PASS_MIN_BYTES);
    assert.equal(encodeWifiSet("Home", "x".repeat(WIFI_PASS_MAX_BYTES)).length, 2 + 4 + WIFI_PASS_MAX_BYTES);
    assert.throws(() => encodeWifiSet("Home", "x".repeat(WIFI_PASS_MAX_BYTES + 1)), RangeError);
    assert.throws(() => encodeWifiSet("", "password1"), TypeError);                    // a password needs an ssid
    assert.throws(() => encodeWifiSet("a\0b"), TypeError);
    assert.throws(() => encodeWifiSet("Home", "pass\0word"), TypeError);
    assert.throws(() => encodeWifiSet(12), TypeError);
    assert.throws(() => encodeWifiSet("Home", 12), TypeError);
});

test("wifi credentials decode", () => {
    const none = decodeWifiCredentials(Uint8Array.of(0, 0));
    assert.deepEqual(none, { ssid: "", password: "", ssidBytes: new Uint8Array(0) });
    const got = decodeWifiCredentials(encodeWifiSet("Home é", "password1"));
    assert.equal(got.ssid, "Home é");
    assert.equal(got.password, "password1");
    assert.deepEqual([...got.ssidBytes], [...new TextEncoder().encode("Home é")]);
    // Trailing bytes are ignored
    assert.equal(decodeWifiCredentials(Uint8Array.of(1, 0, 65, 99, 99)).ssid, "A");
});

test("wifi status decodes", () => {
    const w = new Writer(14).u8(2).u8(201).u8(0xC7).u8(0).raw(Uint8Array.of(192, 168, 1, 50)).raw(Uint8Array.of(0xA0, 1, 2, 3, 4, 0xFF));
    assert.deepEqual(decodeWifiStatus(w.done()), {
        state: WifiState.CONNECTED, stateName: "CONNECTED", reason: 201, rssi: -57,
        ip: "192.168.1.50", mac: "a0:01:02:03:04:ff",
    });
    const idle = decodeWifiStatus(new Uint8Array(14));
    assert.equal(idle.state, WifiState.NO_CREDENTIALS);
    assert.equal(idle.stateName, "NO_CREDENTIALS");
    assert.equal(idle.ip, null);
    assert.equal(idle.rssi, 0);
    assert.equal(idle.mac, "00:00:00:00:00:00");
    // Retrying: the address is not reported even if the bytes are there
    const retry = decodeWifiStatus(new Writer(14).u8(3).u8(15).u8(0).u8(0).raw(Uint8Array.of(1, 2, 3, 4)).raw(new Uint8Array(6)).done());
    assert.equal(retry.stateName, "RETRYING");
    assert.equal(retry.ip, null);
});

test("wifi state event", () => {
    const ev = decodeEvent({ type: Msg.WIFI_STATE, seq: 6, payload: Uint8Array.of(2, 0, 0, 0, 10, 0, 0, 7) });
    assert.deepEqual(ev, { type: "wifiState", seq: 6, state: WifiState.CONNECTED, stateName: "CONNECTED", lost: false, ip: "10.0.0.7" });
    const lost = decodeEvent({ type: Msg.WIFI_STATE, seq: 7, payload: Uint8Array.of(3, 1, 0, 0, 0, 0, 0, 0) });
    assert.deepEqual(lost, { type: "wifiState", seq: 7, state: WifiState.RETRYING, stateName: "RETRYING", lost: true, ip: null });
});

// --- Take control and release (docs/protocol.md, docs/standalone.md) ---

test("take control message types, status and the info flag", () => {
    assert.equal(Msg.TAKE_CONTROL, 0x06);
    assert.equal(Msg.RELEASE_CONTROL, 0x07);
    assert.equal(Status.NOT_HOSTED, 11);
    assert.equal(statusName(Status.NOT_HOSTED), "NOT_HOSTED");
    assert.equal(InfoFlags.HOSTED, 0x0004);
});

test("get info reports the mode, independent of the other flags", () => {
    assert.equal(decodeInfo(infoPayload()).hosted, false);
    assert.equal(decodeInfo(infoPayload({ hosted: true })).hosted, true);
    const all = decodeInfo(infoPayload({ hosted: true, wifiCredentials: true, backlight: false }));
    assert.equal(all.hosted, true);
    assert.equal(all.wifiCredentials, true);
    assert.equal(all.backlight, false);
    assert.equal(all.maxPayload, 8192);                     // fields after the flags still line up
});

// --- Clock (docs/protocol.md, Clock) ---

test("clock message types, the info flag and the sources", () => {
    assert.equal(Msg.TIME_SET, 0x50);
    assert.equal(Msg.TIME_GET, 0x51);
    assert.equal(Msg.CLOCK_SET, 0x52);
    assert.equal(InfoFlags.TIME_SET, 0x0008);
    assert.deepEqual({ ...ClockSource }, { NONE: 0, HOST: 1, NTP: 2 });
    assert.equal(TIME_VALID_FROM_MS, Date.UTC(2025, 0, 1));
    assert.equal(CLOCK_FIELD_MAX_BYTES, 63);
});

test("get info reports whether the clock is set, independent of the other flags", () => {
    assert.equal(decodeInfo(infoPayload()).timeSet, false);
    const all = decodeInfo(infoPayload({ timeSet: true, hosted: true }));
    assert.equal(all.timeSet, true);
    assert.equal(all.hosted, true);
    assert.equal(all.wifiCredentials, false);
    assert.equal(all.maxPayload, 8192);
});

test("time set is an i64 of UTC milliseconds, from a Date or a number", () => {
    const ms = Date.UTC(2026, 10, 8, 7, 59, 1, 234);
    const bytes = encodeTimeSet(new Date(ms));
    assert.equal(bytes.length, 8);
    assert.equal(new DataView(bytes.buffer, bytes.byteOffset).getBigInt64(0, true), BigInt(ms));
    assert.deepEqual([...encodeTimeSet(ms)], [...bytes]);
    assert.equal(encodeTimeSet(TIME_VALID_FROM_MS).length, 8);
});

test("time set refuses what the device would refuse", () => {
    assert.throws(() => encodeTimeSet(TIME_VALID_FROM_MS - 1), RangeError);       // before 2025: "not known"
    assert.throws(() => encodeTimeSet(new Date(0)), RangeError);
    assert.throws(() => encodeTimeSet(new Date(NaN)), RangeError);
    assert.throws(() => encodeTimeSet("2026-01-01"), TypeError);
});

function timeGetPayload(known, source, ms, tz, server)
{
    const t = new TextEncoder().encode(tz), s = new TextEncoder().encode(server);
    return new Writer(12 + t.length + s.length).u8(known).u8(source).i64(ms).u8(t.length).raw(t).u8(s.length).raw(s).done();
}

test("time get decodes", () => {
    const ms = Date.UTC(2026, 10, 8, 7, 59);
    assert.deepEqual(decodeTime(timeGetPayload(1, 2, ms, "AEST-10AEDT,M10.1.0,M4.1.0/3", "time.example.com")), {
        known: true, source: ClockSource.NTP, sourceName: "NTP", time: new Date(ms),
        timeZone: "AEST-10AEDT,M10.1.0,M4.1.0/3", ntpServer: "time.example.com",
    });
    const unknown = decodeTime(timeGetPayload(0, 0, 5000, "UTC0", "pool.ntp.org"));
    assert.equal(unknown.known, false);
    assert.equal(unknown.time, null);                   // meaningless while not known
    assert.equal(unknown.sourceName, "NONE");
    assert.equal(unknown.timeZone, "UTC0");
    assert.equal(decodeTime(timeGetPayload(1, 1, ms, "UTC0", "x")).sourceName, "HOST");
});

test("clock set is a mask, then a length and text for each field given", () => {
    const enc = s => [...new TextEncoder().encode(s)];
    assert.deepEqual([...encodeClockSet({ timeZone: "UTC0" })], [1, 4, ...enc("UTC0")]);
    assert.deepEqual([...encodeClockSet({ ntpServer: "pool.ntp.org" })], [2, 12, ...enc("pool.ntp.org")]);
    assert.deepEqual([...encodeClockSet({ timeZone: "EST5EDT", ntpServer: "10.0.0.1" })], [3, 7, ...enc("EST5EDT"), 8, ...enc("10.0.0.1")]);
    assert.deepEqual([...encodeClockSet({ ntpServer: "a", timeZone: "b" })], [3, 1, 98, 1, 97]);     // always tz first
    assert.deepEqual([...encodeClockSet({ timeZone: "", ntpServer: "" })], [3, 0, 0]);             // the defaults
    assert.deepEqual([...encodeClockSet({ timeZone: "<+0530>-5:30" })], [1, 12, ...enc("<+0530>-5:30")]);
});

test("clock set refuses what the device would refuse", () => {
    assert.throws(() => encodeClockSet({}), TypeError);                                     // nothing to set
    assert.throws(() => encodeClockSet({ zone: "UTC0" }), TypeError);                       // unknown field
    assert.throws(() => encodeClockSet({ timeZone: 5 }), TypeError);
    assert.throws(() => encodeClockSet({ timeZone: "x".repeat(CLOCK_FIELD_MAX_BYTES + 1) }), RangeError);
    assert.equal(encodeClockSet({ timeZone: "x".repeat(CLOCK_FIELD_MAX_BYTES) }).length, 2 + CLOCK_FIELD_MAX_BYTES);
    for (const tz of ["UTC 0", "UTC\t0", "é", "UTC\u007F"])
        assert.throws(() => encodeClockSet({ timeZone: tz }), RangeError, tz);
    for (const server of ["pool.ntp.org ", "a_b", "host:123", "é"])
        assert.throws(() => encodeClockSet({ ntpServer: server }), RangeError, server);
    assert.throws(() => encodeClockSet({ ntpServer: "x".repeat(CLOCK_FIELD_MAX_BYTES + 1) }), RangeError);
});

test("name set is a length and the name, lowercased; empty is the default", () => {
    const enc = s => [...new TextEncoder().encode(s)];
    assert.equal(Msg.NAME_SET, 0x60);
    assert.equal(Msg.NAME_GET, 0x61);
    assert.deepEqual([...encodeNameSet("den-tv")], [6, ...enc("den-tv")]);
    assert.deepEqual([...encodeNameSet("Den-TV")], [6, ...enc("den-tv")]);
    assert.deepEqual([...encodeNameSet("")], [0]);
    assert.equal(encodeNameSet("a".repeat(DEVICE_NAME_MAX_BYTES)).length, 1 + DEVICE_NAME_MAX_BYTES);
});

test("name set refuses what the device would refuse", () => {
    assert.throws(() => encodeNameSet(5), TypeError);
    assert.throws(() => encodeNameSet("a".repeat(DEVICE_NAME_MAX_BYTES + 1)), RangeError);
    for (const name of ["-den", "den-", "a.b", "a_b", "a b", "é"])
        assert.throws(() => encodeNameSet(name), RangeError, name);
});

test("name get decodes is_default and the name", () => {
    const n = new TextEncoder().encode("controlblock-a1b2c3");
    assert.deepEqual(decodeName(Uint8Array.of(1, n.length, ...n)), { name: "controlblock-a1b2c3", isDefault: true });
    assert.deepEqual(decodeName(Uint8Array.of(0, 3, 100, 101, 110)), { name: "den", isDefault: false });
});
