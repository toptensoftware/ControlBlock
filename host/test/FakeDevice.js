// A stand-in WebUSB USBDevice that answers ControlBlock requests, for
// testing the library without hardware. `handler(msg)` returns the response
// { status, data } for a request, or null to stay silent.

import { MessageParser, encodeMessage, Writer, Flags, Msg, Status, InfoFlags, PROTOCOL_VERSION } from "../src/protocol.js";

export function infoPayload({ maxPayload = 8192, pageCount = 1, backlight = true } = {})
{
    const w = new Writer(128);
    w.u16(PROTOCOL_VERSION).u16(backlight ? InfoFlags.BACKLIGHT : 0).u32(maxPayload).u32(5000000).u32(1234).u32(3).u32(4000000);
    w.u16(800).u16(480).u8(5).u8(3).u8(8).u8(pageCount);
    w.u16(128).u16(128).u16(80).u16(80).u8(0);
    for (const s of ["1.0", "SERIAL1"]) {
        const b = new TextEncoder().encode(s);
        w.u8(b.length).raw(b);
    }
    return w.done();
}

export class FakeDevice
{
    constructor({ maxPayload = 8192, handler, backlight = true } = {})
    {
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
            const r = msg.type === Msg.GET_INFO
                ? { status: Status.OK, data: infoPayload({ maxPayload: this.maxPayload, backlight: this.backlight }) }
                : msg.type === Msg.PING
                    ? { status: Status.OK, data: msg.payload }
                    : this.handler(msg);
            if (r)
                this.respond(msg, r.status, r.data);
        }
        return { status: "ok", bytesWritten: data.length };
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
