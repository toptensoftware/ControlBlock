import test from "node:test";
import assert from "node:assert/strict";
import {
    encodeMessage, MessageParser, encodeKeys, encodeIds, encodePutImage, decodeInfo, decodeQuery, decodeEvent,
    Msg, Flags, KeyFlags, HEADER_SIZE, KEYDEF_SIZE, MAGIC, statusName, Status,
    encodeStatusText, StatusText, STATUS_TEXT_MAX_BYTES,
    crc32, encodeLayoutBegin, encodeLayoutData, LAYOUT_DATA_PREFIX, HEARTBEAT_INTERVAL_MS,
    InfoFlags, Writer, WifiState, WIFI_SSID_MAX_BYTES, WIFI_PASS_MIN_BYTES, WIFI_PASS_MAX_BYTES,
    encodeWifiSet, decodeWifiCredentials, decodeWifiStatus,
} from "../src/protocol.js";
import { infoPayload } from "./FakeDevice.js";

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

test("key definitions match the firmware's KeyDef", () => {
    const b = encodeKeys([
        { page: 2, key: 14, bg: 0x11223344, fg: 5, pressedBg: 6 },
        { page: 0, key: 1, bg: 9, empty: true },
        { page: 1, key: 3, fg: 7, disabled: true },
    ]);
    assert.equal(b.length, 2 + 3 * KEYDEF_SIZE);
    const v = new DataView(b.buffer, b.byteOffset);
    assert.equal(v.getUint16(0, true), 3);
    assert.equal(v.getUint8(2), 2);
    assert.equal(v.getUint8(3), 14);
    assert.equal(v.getUint16(4, true), 0);
    assert.equal(v.getUint32(6, true), 0x11223344);
    assert.equal(v.getUint32(10, true), 5);
    assert.equal(v.getUint32(14, true), 6);
    assert.equal(v.getUint32(18, true), 0);
    assert.equal(v.getUint16(2 + KEYDEF_SIZE + 2, true), KeyFlags.EMPTY);
    assert.equal(v.getUint32(2 + KEYDEF_SIZE + 4, true), 0);      // empty: IDs zeroed
    assert.equal(v.getUint16(2 + 2 * KEYDEF_SIZE + 2, true), KeyFlags.DISABLED);
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
    const info = decodeInfo(infoPayload({ maxPayload: 4096, pageCount: 3 }));
    assert.equal(info.protocolVersion, 1);
    assert.equal(info.maxPayload, 4096);
    assert.equal(info.imageCount, 3);
    assert.equal(info.screenWidth, 800);
    assert.equal(info.cols, 5);
    assert.equal(info.rows, 3);
    assert.equal(info.pageCount, 3);
    assert.equal(info.keyWidth, 128);
    assert.equal(info.iconWidth, 80);
    assert.equal(info.firmwareVersion, "1.0");
    assert.equal(info.serial, "SERIAL1");
});

test("query results are a bitmap", () => {
    assert.deepEqual(decodeQuery(Uint8Array.of(10, 0, 0b10000101, 0b10)),
                     [true, false, true, false, false, false, false, true, false, true]);
});

test("events", () => {
    const key = decodeEvent({ type: Msg.KEY, seq: 4, payload: Uint8Array.of(1, 7, 2, 1, 0x10, 0x27, 0, 0) });
    assert.deepEqual(key, { type: "key", seq: 4, page: 1, key: 7, action: 2, lost: true, timeMs: 10000 });
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
    const old = decodeInfo(infoPayload());
    assert.equal(old.layoutCapacity, 0);
    assert.equal(old.layoutSize, 0);
    assert.equal(old.layoutCrc, 0);
    assert.equal(old.serial, "SERIAL1");
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
