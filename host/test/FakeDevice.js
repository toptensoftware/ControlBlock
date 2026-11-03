// A stand-in WebUSB USBDevice that answers ControlBlock requests, for
// testing the library without hardware. `handler(msg)` returns the response
// { status, data } for a request, or null to stay silent.

import { MessageParser, encodeMessage, Writer, Reader, Flags, Msg, Status, InfoFlags, PROTOCOL_VERSION, crc32 } from "../src/protocol.js";

export function infoPayload({ maxPayload = 8192, pageCount = 1, backlight = true, config } = {})
{
    const w = new Writer(128);
    w.u16(PROTOCOL_VERSION).u16(backlight ? InfoFlags.BACKLIGHT : 0).u32(maxPayload).u32(5000000).u32(1234).u32(3).u32(4000000);
    w.u16(800).u16(480).u8(5).u8(3).u8(8).u8(pageCount);
    w.u16(128).u16(128).u16(80).u16(80).u8(0);
    for (const s of ["1.0", "SERIAL1"]) {
        const b = new TextEncoder().encode(s);
        w.u8(b.length).raw(b);
    }
    if (config)
        w.u32(config.capacity).u32(config.size).u32(config.crc);
    return w.done();
}

export class FakeDevice
{
    // config: emulate the standalone config upload (docs/protocol.md): { capacity, latencyMs, failAt }
    //   latencyMs: how long each CONFIG_DATA takes to answer; failAt: a chunk offset that gets IO_ERROR
    //   After a good commit the device "restarts": the connection drops, and (if
    //   restartMs is set) it comes back after that long, with the config in GET_INFO.
    constructor({ maxPayload = 8192, handler, backlight = true, config } = {})
    {
        this.config = config ? {
            capacity: 1 << 20, latencyMs: 0, commitMs: 0, restartMs: 0, failAt: -1,
            stored: null,               // { size, crc } once committed
            upload: null,               // { size, crc, received, chunks: [] } in progress
            inflight: 0, maxInflight: 0,
            ...config,
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

    async transferOut(ep, data)
    {
        for (const msg of this._parser.push(new Uint8Array(data))) {
            this.requests.push(msg);
            if (msg.type === Msg.SET_BACKLIGHT && msg.payload.length >= 1)
                this.backlight = msg.payload[0] !== 0;
            if (this.config && msg.type >= Msg.CONFIG_BEGIN && msg.type <= Msg.CONFIG_COMMIT) {
                this._config(msg);
                continue;
            }
            const r = msg.type === Msg.GET_INFO
                ? { status: Status.OK, data: infoPayload({ maxPayload: this.maxPayload, backlight: this.backlight, config: this.config && { capacity: this.config.capacity, size: this.config.stored?.size ?? 0, crc: this.config.stored?.crc ?? 0 } }) }
                : msg.type === Msg.PING
                    ? { status: Status.OK, data: msg.payload }
                    : this.handler(msg);
            if (r)
                this.respond(msg, r.status, r.data);
        }
        return { status: "ok", bytesWritten: data.length };
    }

    // The config upload, as the firmware does it. Answers (possibly later) itself.
    _config(msg)
    {
        const c = this.config;
        const r = new Reader(msg.payload);
        let status = Status.OK, delay = 0;
        if (msg.type === Msg.CONFIG_BEGIN) {
            const size = r.u32(), crc = r.u32();
            if (size === 0) status = Status.BAD_ARG;
            else if (size > c.capacity) status = Status.NO_SPACE;
            else { c.stored = null; c.upload = { size, crc, received: 0, bytes: new Uint8Array(size) }; }
        } else if (msg.type === Msg.CONFIG_DATA) {
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
            if (msg.type === Msg.CONFIG_COMMIT && status === Status.OK) {
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
