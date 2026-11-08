// A stand-in WebUSB USBDevice that answers ControlBlock requests, for
// testing the library without hardware. `handler(msg)` returns the response
// { status, data } for a request, or null to stay silent.

import { ControlBlock } from "../src/ControlBlock.js";
import { MessageParser, encodeMessage, Writer, Reader, Flags, Msg, Status, InfoFlags, PROTOCOL_VERSION, USB_PACKET_SIZE, crc32, ClockSource, TIME_VALID_FROM_MS } from "../src/protocol.js";

export function infoPayload({ maxPayload = 8192, pageCount = 1, backlight = true, wifiCredentials = false, hosted = false, timeSet = false, layout } = {})
{
    const w = new Writer(128);
    const flags = (backlight ? InfoFlags.BACKLIGHT : 0) | (wifiCredentials ? InfoFlags.WIFI_CREDENTIALS : 0) | (hosted ? InfoFlags.HOSTED : 0) | (timeSet ? InfoFlags.TIME_SET : 0);
    w.u16(PROTOCOL_VERSION).u16(flags).u32(maxPayload).u32(5000000).u32(1234).u32(3).u32(4000000);
    w.u16(800).u16(480).u8(5).u8(3).u8(8).u8(pageCount);
    w.u16(128).u16(128).u16(80).u16(80).u8(0);
    for (const s of ["1.0", "SERIAL1"]) {
        const b = new TextEncoder().encode(s);
        w.u8(b.length).raw(b);
    }
    if (layout)
        w.u32(layout.capacity).u32(layout.size).u32(layout.crc);
    return w.done();
}

const USB_RX_TRANSFER = 512;    // the firmware's receive transfer (CONFIG_TINYUSB_VENDOR_EPSIZE)

// Requests that edit the keys, images or status bar: refused while standalone
const HOSTED_REQUESTS = [Msg.RESET, Msg.SET_STATUS_TEXT, Msg.PUT_IMAGE, Msg.FREE_IMAGES, Msg.SET_PAGE_COUNT, Msg.SET_KEYS, Msg.SET_PAGE];

export class FakeDevice
{
    // layout: emulate the standalone layout upload (docs/protocol.md): { capacity, latencyMs, failAt }
    //   latencyMs: how long each LAYOUT_DATA takes to answer; failAt: a chunk offset that gets IO_ERROR
    //   After a good commit the device "restarts": the connection drops, and (if
    //   restartMs is set) it comes back after that long, with the layout in GET_INFO.
    // wifi: emulate the Wi-Fi messages (docs/wifi.md): { ssid, pass } already stored (strings),
    //   and `status` ({ state, reason, rssi, ip, mac }) is what WIFI_STATUS reports (settable by tests)
    // hosted: a host has taken control (the device itself boots standalone, `hosted: false`). While
    //   standalone, requests that edit the screen get NOT_HOSTED; TAKE_CONTROL clears `model` ("home"
    //   standalone, "empty" hosted) and RELEASE_CONTROL puts Home back and the backlight on.
    //   controlStatus: if not OK, TAKE_CONTROL and RELEASE_CONTROL answer with it and change nothing.
    // clock: emulate the clock messages (docs/protocol.md, Clock): { offset, source, tz, server }.
    //   offset: device time minus Date.now() in ms, null while the time isn't known (the default).
    constructor({ maxPayload = 8192, handler, backlight = true, layout, wifi, clock, hosted = false, controlStatus = Status.OK } = {})
    {
        this.clock = clock ? { offset: null, source: 0, tz: "UTC0", server: "pool.ntp.org", ...clock } : null;
        this.hosted = hosted;
        this.model = hosted ? "empty" : "home";
        this.controlStatus = controlStatus;
        this.wifi = wifi ? { ssid: "", pass: "", status: { state: 0, reason: 0, rssi: 0, ip: [0, 0, 0, 0], mac: [0, 0, 0, 0, 0, 0] }, ...wifi } : null;
        this.layout = layout ? {
            capacity: 1 << 20, latencyMs: 0, commitMs: 0, restartMs: 0, failAt: -1,
            stored: null,               // { size, crc } once committed
            upload: null,               // { size, crc, received, chunks: [] } in progress
            inflight: 0, maxInflight: 0,
            ...layout,
        } : null;
        this.gone = false;              // disconnected: transferIn throws
        this.maxReadTimeout = 0;        // longest timeout a transferIn asked for (ms)
        this.backlight = backlight;     // follows SET_BACKLIGHT; reported by GET_INFO
        this.opened = false;
        this.claimed = false;
        this.resets = 0;
        this.requests = [];             // every request message received
        this.maxPayload = maxPayload;
        this.handler = handler ?? (() => ({ status: Status.OK, data: new Uint8Array(0) }));
        this._parser = new MessageParser();
        this._held = new Uint8Array(0);     // received but not delivered (see _receive)
        this._out = [];                 // chunks for transferIn
        this._waiter = null;
    }

    get configuration() { return this.opened ? { configurationValue: 1 } : null; }
    async open() { this.opened = true; }
    async close() { this.opened = false; this.push(null); }
    async selectConfiguration() { }
    async claimInterface() { this.claimed = true; }
    async releaseInterface() { this.claimed = false; }
    async clearHalt() { }

    async controlTransferOut(setup)
    {
        if (setup.requestType === "vendor" && setup.request === 0x10)
            this.resets++;
        return { status: "ok", bytesWritten: 0 };
    }

    // Like the firmware's USB receive (docs/protocol.md, "Zero-length packets"): bytes arrive in
    // 512-byte receive transfers of 64-byte packets, and a transfer only completes when it is full
    // or a short packet (or a ZLP) ends it. So a transfer that ends on a whole packet leaves the
    // bytes since the last 512 boundary undelivered until the next short packet or ZLP.
    _receive(bytes)
    {
        if (!bytes.length) {
            const held = this._held;
            this._held = new Uint8Array(0);
            return held;
        }
        const all = new Uint8Array(this._held.length + bytes.length);
        all.set(this._held);
        all.set(bytes, this._held.length);
        const keep = bytes.length % USB_PACKET_SIZE ? 0 : all.length % USB_RX_TRANSFER;
        this._held = all.slice(all.length - keep);
        return all.subarray(0, all.length - keep);
    }

    async transferOut(ep, data)
    {
        for (const msg of this._parser.push(this._receive(new Uint8Array(data)))) {
            this.requests.push(msg);
            if (msg.type === Msg.SET_BACKLIGHT && msg.payload.length >= 1)
                this.backlight = msg.payload[0] !== 0;
            if (this.layout && msg.type >= Msg.LAYOUT_BEGIN && msg.type <= Msg.LAYOUT_COMMIT) {
                this._layout(msg);
                continue;
            }
            const r = msg.type === Msg.TAKE_CONTROL || msg.type === Msg.RELEASE_CONTROL
                ? this._control(msg)
                : !this.hosted && HOSTED_REQUESTS.includes(msg.type)
                    ? { status: Status.NOT_HOSTED, data: new Uint8Array(0) }
                : msg.type === Msg.GET_INFO
                ? { status: Status.OK, data: infoPayload({ maxPayload: this.maxPayload, backlight: this.backlight, hosted: this.hosted, wifiCredentials: !!this.wifi?.ssid, timeSet: this.clock?.offset != null, layout: this.layout && { capacity: this.layout.capacity, size: this.layout.stored?.size ?? 0, crc: this.layout.stored?.crc ?? 0 } }) }
                : this.wifi && msg.type >= Msg.WIFI_SET && msg.type <= Msg.WIFI_STATUS
                    ? this._wifi(msg)
                : this.clock && msg.type >= Msg.TIME_SET && msg.type <= Msg.CLOCK_SET
                    ? this._clock(msg)
                : msg.type === Msg.PING
                    ? { status: Status.OK, data: msg.payload }
                    : this.handler(msg);
            if (r)
                this.respond(msg, r.status, r.data);
        }
        return { status: "ok", bytesWritten: data.length };
    }

    // TAKE_CONTROL / RELEASE_CONTROL, as the firmware does them
    _control(msg)
    {
        if (this.controlStatus !== Status.OK)
            return { status: this.controlStatus, data: new Uint8Array(0) };
        const take = msg.type === Msg.TAKE_CONTROL;
        if (take || this.hosted) {
            this.model = take ? "empty" : "home";
            if (!take)
                this.backlight = true;
            this.hosted = take;
        }
        return { status: Status.OK, data: new Uint8Array(0) };
    }

    // The Wi-Fi messages, as the firmware does them (the rules are checked by the firmware's own tests)
    _wifi(msg)
    {
        const w = this.wifi, p = msg.payload;
        if (msg.type === Msg.WIFI_SET) {
            if (p.length < 2 || p.length < 2 + p[0] + p[1])
                return { status: Status.BAD_LENGTH, data: new Uint8Array(0) };
            w.ssid = new TextDecoder().decode(p.subarray(2, 2 + p[0]));
            w.pass = new TextDecoder().decode(p.subarray(2 + p[0], 2 + p[0] + p[1]));
            return { status: Status.OK, data: new Uint8Array(0) };
        }
        if (msg.type === Msg.WIFI_GET) {
            const ssid = new TextEncoder().encode(w.ssid), pass = new TextEncoder().encode(w.pass);
            return { status: Status.OK, data: new Writer(2 + ssid.length + pass.length).u8(ssid.length).u8(pass.length).raw(ssid).raw(pass).done() };
        }
        const st = w.status;
        return { status: Status.OK, data: new Writer(14).u8(st.state).u8(st.reason).u8(st.rssi & 0xFF).u8(0).raw(Uint8Array.from(st.ip)).raw(Uint8Array.from(st.mac)).done() };
    }

    // The clock messages, as the firmware does them (the rules are checked by the firmware's own tests)
    _clock(msg)
    {
        const c = this.clock, r = new Reader(msg.payload);
        const reply = (status, data = new Uint8Array(0)) => ({ status, data });
        if (msg.type === Msg.TIME_SET) {
            if (!r.has(8))
                return reply(Status.BAD_LENGTH);
            const ms = r.i64();
            if (ms < TIME_VALID_FROM_MS)
                return reply(Status.BAD_ARG);
            c.offset = ms - Date.now();
            c.source = ClockSource.HOST;
            return reply(Status.OK);
        }
        if (msg.type === Msg.TIME_GET) {
            const tz = new TextEncoder().encode(c.tz), server = new TextEncoder().encode(c.server);
            const known = c.offset != null;
            return reply(Status.OK, new Writer(12 + tz.length + server.length)
                .u8(known ? 1 : 0).u8(known ? c.source : ClockSource.NONE).i64(Date.now() + (c.offset ?? 0))
                .u8(tz.length).raw(tz).u8(server.length).raw(server).done());
        }
        const mask = r.u8();            // CLOCK_SET: the time zone, then the server; "" = the default
        if (mask & 1)
            c.tz = r.str() || "UTC0";
        if (mask & 2)
            c.server = r.str() || "pool.ntp.org";
        return reply(Status.OK);
    }

    // The layout upload, as the firmware does it. Answers (possibly later) itself.
    _layout(msg)
    {
        const c = this.layout;
        const r = new Reader(msg.payload);
        let status = Status.OK, delay = 0;
        if (msg.type === Msg.LAYOUT_BEGIN) {
            const size = r.u32(), crc = r.u32();
            if (size === 0) status = Status.BAD_ARG;
            else if (size > c.capacity) status = Status.NO_SPACE;
            else { c.stored = null; c.upload = { size, crc, received: 0, bytes: new Uint8Array(size) }; }
        } else if (msg.type === Msg.LAYOUT_DATA) {
            const offset = r.u32(), data = msg.payload.subarray(4);
            const u = c.upload;
            if (!data.length) status = Status.BAD_LENGTH;
            else if (!u || offset !== u.received) { status = Status.BAD_SEQUENCE; c.upload = null; }
            else if (offset + data.length > u.size) { status = Status.BAD_ARG; c.upload = null; }
            else if (offset === c.failAt) { status = Status.IO_ERROR; c.upload = null; }
            else { u.bytes.set(data, offset); u.received += data.length; delay = c.latencyMs; }
        } else {
            const u = c.upload;
            if (!u || u.received !== u.size) status = Status.BAD_SEQUENCE;
            else if (crc32(u.bytes) !== u.crc) { status = Status.BAD_CRC; c.upload = null; }
            else { c.stored = { size: u.size, crc: u.crc, bytes: u.bytes }; c.upload = null; delay = c.commitMs; }
        }
        const done = () => {
            c.inflight--;
            this.respond(msg, status);
            if (msg.type === Msg.LAYOUT_COMMIT && status === Status.OK) {
                this.gone = true;               // restart
                this.push(null);
                if (c.restartMs)
                    setTimeout(() => { this.gone = false; }, c.restartMs);
            }
        };
        c.inflight++;
        c.maxInflight = Math.max(c.maxInflight, c.inflight);
        if (delay) setTimeout(done, delay); else done();
    }

    // Send a response (or anything) to the host
    respond(msg, status, data = new Uint8Array(0))
    {
        const payload = new Uint8Array(4 + data.length);
        payload[0] = status & 0xFF;
        payload[1] = status >> 8;
        payload.set(data, 4);
        this.push(encodeMessage(msg.type, msg.seq, payload, Flags.RESPONSE));
    }

    event(type, seq, payload)
    {
        this.push(encodeMessage(type, seq, payload, Flags.EVENT));
    }

    push(bytes)
    {
        this._out.push(bytes);
        if (this._waiter) {
            const w = this._waiter;
            this._waiter = null;
            w();
        }
    }

    async transferIn(ep, length, timeout = 1000)
    {
        this.maxReadTimeout = Math.max(this.maxReadTimeout, timeout);
        if (this.gone)
            throw new Error("LIBUSB_ERROR_NO_DEVICE");
        if (!this._out.length) {
            const arrived = new Promise(resolve => { this._waiter = resolve; });
            // node usb reports a read timeout as "Cancelled"
            const timedOut = new Promise((_, reject) => setTimeout(() => reject(new Error("Cancelled")), Math.min(timeout, 50)));
            try { await Promise.race([arrived, timedOut]); }
            finally { this._waiter = null; }
        }
        const bytes = this._out.shift();
        if (!bytes)
            return { status: "ok", data: new DataView(new ArrayBuffer(0)) };
        return { status: "ok", data: new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength) };
    }
}

// An `open(options)` that connects a ControlBlock to `dev` (what ControlBlock.open does for a
// real device), for code that takes an injected opener (uploadBlob, the cli). Each call's
// options are pushed onto `opened`.
export function fakeOpener(dev, { opened = [], timeoutMs = 500 } = {})
{
    return async options => {
        opened.push(options);
        const cb = new ControlBlock(dev, { timeoutMs });
        await cb.connect();
        return cb;
    };
}
