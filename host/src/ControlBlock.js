// A connection to a ControlBlock over its USB vendor interface (mode 1).
// Works on any WebUSB USBDevice: the `usb` package's in Node (see open()),
// or navigator.usb's in a browser. Spec: docs/protocol.md.
//
// Requests are pipelined: each method sends at once and resolves when its
// response arrives (responses come back in order, matched by seq).
// Events: "key" ({ id, page, action, lost, timeMs }: the key's id from setKeys, the page it
// was touched on), "pageChanged"
// ({ page, cause }), "wake" ({ lost, timeMs }: the screen was touched while
// the backlight is off; docs/backlight.md), "wifiState" ({ state, stateName,
// lost, ip }: the Wi-Fi connection changed; docs/wifi.md), "error" (the read loop failed), "close".
// takeControl() / releaseControl() switch the device between standalone and hosted mode (docs/standalone.md).
// uploadLayout() saves a standalone layout in the device's flash (docs/protocol.md).
// setTime() / getTime() / setClock() run the device's clock (docs/protocol.md, Clock).
// setName() / getName() set the device name (mDNS/DHCP hostname; docs/protocol.md, Device name).

import { Emitter } from "./Emitter.js";
import {
    USB_VID, USB_PID, INTERFACE, ENDPOINT, USB_PACKET_SIZE, PROTOCOL_VERSION, REQ_RESET_STREAM,
    PUT_IMAGE_PREFIX, LAYOUT_DATA_PREFIX, KEYDEF_SIZE, Flags, Msg, Status, StatusText, encodeStatusText,
    encodeWifiSet, decodeWifiCredentials, decodeWifiStatus, WifiState, encodeTimeSet, decodeTime, encodeClockSet, encodeNameSet, decodeName,
    encodeMessage, MessageParser, encodeIds, encodePutImage, encodeKeys, encodeLayoutBegin, encodeLayoutData, crc32,
    decodeInfo, decodeQuery, decodeEvent, statusName, HEARTBEAT_INTERVAL_MS, layoutBlobVersion,
} from "./protocol.js";

const READ_SIZE        = 4096;      // a multiple of the 64-byte packet size
// node usb only: a read that gets no data for this long is cancelled. That
// must never happen while the device is alive: on Windows a cancelled bulk IN
// read leaves the pipe deaf (nothing more arrives until the device is
// reopened). The device sends a heartbeat every HEARTBEAT_INTERVAL_MS, so an
// idle read always completes first; this is well past it, as a margin.
// A pending read can't be cancelled (usb 3.x), and close() and the process
// exit wait for it: at most one heartbeat interval on a live device.
const READ_TIMEOUT_MS  = 5 * HEARTBEAT_INTERVAL_MS;
// While writes are queued, the next read isn't submitted (for up to this long): a pending bulk IN
// read that the device NAKs is polled so often that it halves the bulk OUT rate (measured: 251 vs
// 493 KB/s; wip/complete-upload-throughput.md). The replies wait in the device's 2 KB transmit
// FIFO meanwhile; the cap keeps it draining (the device drops a reply it can't send in 1 s).
// A read is only held back, never submitted and cancelled, so the deaf-pipe rule above holds.
// Only bulk-data requests hold reads: their replies are 16 bytes. Any other request queued keeps
// reads going, since its reply may be big (an 8 KB ping echo would fill the FIFO, block the
// device's writes and stall its reads: pipelined 8 KB pings fell from 702 to 259 KB/s).
const READ_HOLD_MS     = 20;
const SMALL_REPLY      = new Set([Msg.PUT_IMAGE, Msg.LAYOUT_DATA]);
const DEFAULT_TIMEOUT  = 5000;      // per request
const LAYOUT_COMMIT_TIMEOUT = 30000; // the device checks and reads back the whole layout
const LAYOUT_WINDOW    = 4;         // layout chunks in flight: each takes the device a while (flash erases)
const RESTART_TIMEOUT  = 15000;     // a device restarting after a layout commit
const RESTART_POLL     = 250;

function typeName(type)
{
    for (const [name, value] of Object.entries(Msg))
        if (value === type)
            return name;
    return `0x${type.toString(16)}`;
}

export class ControlBlockError extends Error
{
    constructor(type, status)
    {
        super(`${typeName(type)} failed: ${statusName(status)}`);
        this.type = type;
        this.status = status;
    }
}

export class ControlBlock extends Emitter
{
    // `device`: a WebUSB USBDevice. Call connect() before anything else.
    constructor(device, { timeoutMs = DEFAULT_TIMEOUT, readHoldMs = READ_HOLD_MS } = {})
    {
        super();
        this.device    = device;
        this.timeoutMs = timeoutMs;
        this.readHoldMs = readHoldMs;
        this._queuedWrites = 0;         // _write calls not yet done
        this._bigReplyWrites = 0;       // ... of which not small-reply ones: reads aren't held
        this._releaseRead  = null;      // ends a held read's wait (writes drained, or close)
        this.info      = null;
        this.connected = false;
        this._parser   = new MessageParser();
        this._pending  = new Map();     // seq -> { type, resolve, reject, timer }
        this._seq      = 0;
        this._writes   = Promise.resolve();
        this._restarting = false;       // a layout was committed: the device is about to drop off the bus
    }

    // Node: find a ControlBlock (by serial, or the first with our VID/PID),
    // connect and return it. `setTime` is passed on to connect().
    static async open({ serial, vendorId = USB_VID, productId = USB_PID, setTime, ...options } = {})
    {
        const { usb } = await import("usb");
        const device = serial
            ? await usb.findDeviceBySerial(serial)
            : await usb.findDeviceByIds(vendorId, productId);
        if (!device)
            throw new Error("No ControlBlock found");
        const cb = new ControlBlock(device, options);
        await cb.connect({ setTime });
        return cb;
    }

    // Claim the interface, reset the stream and read the device's info.
    // `setTime: true` also sets the device's clock from this machine's
    // (setTime()), so a host app keeps it right without Wi-Fi; off by default.
    async connect({ setTime = false } = {})
    {
        const d = this.device;
        if (!d.opened)
            await d.open();
        let config = null;
        try { config = d.configuration; } catch { /* not configured yet */ }
        if (!config || config.configurationValue !== 1)
            await d.selectConfiguration(1);
        await d.claimInterface(INTERFACE);
        this.connected = true;

        await this.resetStream();
        this._readLoop();
        this.info = await this.getInfo();
        if (this.info.protocolVersion !== PROTOCOL_VERSION)
            throw new Error(`Device speaks protocol ${this.info.protocolVersion}, expected ${PROTOCOL_VERSION}`);
        if (setTime)
            await this.setTime();
        return this.info;
    }

    async close()
    {
        if (!this.connected)
            return;
        this.connected = false;
        this._releaseRead?.();
        this._failAll(new Error("Connection closed"));
        try { await this.device.releaseInterface(INTERFACE); } catch { }
        try { await this.device.close(); } catch { }
        this.emit("close");
    }

    // RESET_STREAM: the device drops any partial message and upload. Every
    // request waiting here fails. Responses already on their way are
    // ignored (no request waits for their seq).
    async resetStream()
    {
        this._failAll(new Error("Stream reset"));
        const reset = () => this.device.controlTransferOut({
            requestType: "vendor",
            recipient:   "interface",
            request:     REQ_RESET_STREAM,
            value:       0,
            index:       INTERFACE,
        }, new Uint8Array(0));      // optional in WebUSB, but node usb needs it
        // After any writes already queued, so it lands between messages
        const p = this._writes.then(reset);
        this._writes = p.catch(() => { });
        const r = await p;
        if (r.status !== "ok")
            throw new Error(`RESET_STREAM failed: ${r.status}`);
    }

    // --- Requests ---

    // Send a request; resolves to { status, data } (data after the status)
    request(type, payload = new Uint8Array(0), { timeoutMs = this.timeoutMs } = {})
    {
        if (!this.connected)
            return Promise.reject(new Error("Not connected"));
        const seq = this._nextSeq();
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => this._timeout(seq), timeoutMs);
            this._pending.set(seq, { type, resolve, reject, timer });
            this._write(encodeMessage(type, seq, payload), SMALL_REPLY.has(type)).catch(err => this._fail(seq, err));
        });
    }

    // A request that must succeed: resolves to its data, or throws a
    // ControlBlockError
    async call(type, payload, options)
    {
        const r = await this.request(type, payload, options);
        if (r.status !== Status.OK)
            throw new ControlBlockError(type, r.status);
        return r.data;
    }

    async ping(data = new Uint8Array(0))
    {
        return this.call(Msg.PING, data);
    }

    async getInfo()
    {
        this.info = decodeInfo(await this.call(Msg.GET_INFO));
        return this.info;
    }

    // Take control of the screen (docs/standalone.md "Runtime modes"). The
    // device boots standalone, showing its own Home, and refuses every request
    // that edits the screen (reset, setKeys, putImage...) with NOT_HOSTED until
    // you do this. Taking control clears the keys and images like reset().
    // Key and page events flow while you have control.
    async takeControl()
    {
        await this.call(Msg.TAKE_CONTROL);
        if (this.info)
            this.info.hosted = true;
    }

    // Hand the screen back: the device rebuilds its Home, turns the backlight
    // on and runs its own actions again. Also happens by itself when the USB
    // connection drops or the bus suspends.
    async releaseControl()
    {
        await this.call(Msg.RELEASE_CONTROL);
        if (this.info) {
            this.info.hosted = false;
            this.info.backlight = true;
        }
    }

    // Clear every key and image; one empty page. Needs control (takeControl).
    async reset()
    {
        await this.call(Msg.RESET);
    }

    // Status bar texts: { left, right }. Only the fields given change; "" clears
    // one. Nothing is sent if any is invalid (too long: 47 bytes of UTF-8).
    async setStatusText(texts)
    {
        const fields = { left: StatusText.LEFT, right: StatusText.RIGHT };
        for (const name of Object.keys(texts))
            if (!(name in fields))
                throw new TypeError(`Unknown status text "${name}" (left or right)`);
        const payloads = Object.entries(fields)
            .filter(([name]) => texts[name] !== undefined)
            .map(([name, field]) => encodeStatusText(field, texts[name]));
        await Promise.all(payloads.map(p => this.call(Msg.SET_STATUS_TEXT, p)));
    }

    // Turn the backlight on or off. While off, a touch sends a "wake" event
    // instead of acting on keys; the device never turns it on itself. The
    // state at connect is info.backlight; after that it's yours to track.
    async setBacklight(on)
    {
        await this.call(Msg.SET_BACKLIGHT, Uint8Array.of(on ? 1 : 0));
        if (this.info)
            this.info.backlight = !!on;
    }

    // Wi-Fi (docs/wifi.md). Independent of the operating mode.
    // Store a network and connect to it in the background (an empty ssid
    // clears it; a missing password is an open network). Resolves once the
    // credentials are stored: watch "wifiState" or poll wifiStatus() for the
    // connection. Throws before sending anything if the device would refuse.
    async setWifi(ssid, password = "")
    {
        await this.call(Msg.WIFI_SET, encodeWifiSet(ssid, password));
        if (this.info)
            this.info.wifiCredentials = ssid.length > 0;
    }

    // Forget the stored network and turn Wi-Fi off
    async clearWifi()
    {
        await this.setWifi("");
    }

    // The stored network: { ssid, password, ssidBytes } (empty if none).
    // The password is in clear text (docs/wifi.md, Security).
    async getWifi()
    {
        return decodeWifiCredentials(await this.call(Msg.WIFI_GET));
    }

    // { state, stateName, reason, rssi, ip, mac }: state is WifiState; `ip`
    // is null unless connected; `reason` is the last disconnect reason
    // (diagnostics only)
    async wifiStatus()
    {
        return decodeWifiStatus(await this.call(Msg.WIFI_STATUS));
    }

    // Poll wifiStatus() every `intervalMs` until connected (true), or until no
    // network is stored or `timeoutMs` has passed (false). `onStatus(st)` gets
    // the first status and each one that differs from the last.
    async waitWifiConnected({ timeoutMs = 30000, intervalMs = 500, onStatus } = {})
    {
        const deadline = Date.now() + timeoutMs;
        let last = "";
        while (true) {
            const st = await this.wifiStatus();
            const key = JSON.stringify(st);
            if (key !== last)
                onStatus?.(st);
            last = key;
            if (st.state === WifiState.CONNECTED)
                return true;
            if (st.state === WifiState.NO_CREDENTIALS || Date.now() >= deadline)
                return false;
            await new Promise(resolve => setTimeout(resolve, intervalMs));
        }
    }

    // Clock (docs/protocol.md, Clock). Independent of the operating mode.
    // Set the device's clock (UTC) to `time` (a Date or ms; default now). It
    // shows in the status bar's {clock} in the device's time zone. Throws
    // before sending anything for a time before 2025.
    async setTime(time = new Date())
    {
        await this.call(Msg.TIME_SET, encodeTimeSet(time));
        if (this.info)
            this.info.timeSet = true;
    }

    // { known, source, sourceName, time, timeZone, ntpServer }: `time` is a
    // Date, null while the device doesn't know the time; `source` is ClockSource
    async getTime()
    {
        return decodeTime(await this.call(Msg.TIME_GET));
    }

    // Store the clock settings: { timeZone, ntpServer }, either or both.
    // timeZone is a POSIX TZ string (e.g. "AEST-10AEDT,M10.1.0,M4.1.0/3");
    // "" puts a setting back to its default (UTC0, pool.ntp.org). Throws
    // before sending anything if the device would refuse.
    async setClock(settings)
    {
        await this.call(Msg.CLOCK_SET, encodeClockSet(settings));
    }

    // Device name (docs/protocol.md, Device name). Independent of the operating mode.
    // Store the name the device goes by on the network (<name>.local, DHCP
    // hostname); "" puts back the default (controlblock-xxxxxx from its MAC).
    // A-Z are lowercased. Throws before sending anything if the device would refuse.
    async setName(name)
    {
        await this.call(Msg.NAME_SET, encodeNameSet(name));
    }

    // { name, isDefault }
    async getName()
    {
        return decodeName(await this.call(Msg.NAME_GET));
    }

    // Upload (or replace) an rle565 image under `id`, in pipelined chunks
    async putImage(id, data)
    {
        if (!data.length)
            throw new Error("Empty image");
        const chunk = this._maxPayload() - PUT_IMAGE_PREFIX;
        const sends = [];
        for (let offset = 0; offset < data.length; offset += chunk)
            sends.push(this.call(Msg.PUT_IMAGE, encodePutImage(id, data.length, offset, data.subarray(offset, offset + chunk))));
        await Promise.all(sends);
    }

    // Save a standalone layout (a BPAK blob from src/bake) in the device's
    // flash. Skipped (resolves { uploaded: false }) if the device already has
    // exactly this blob, unless `force`. onProgress(sent, total) after each
    // chunk. The device restarts when it's done: the connection ends, and
    // reconnect() finds it again. A blob of another version than the firmware
    // loads (GET_INFO's layoutBlobVersion) is refused before anything is sent,
    // even with `force`; firmware that doesn't report it isn't checked.
    async uploadLayout(blob, { force = false, onProgress } = {})
    {
        if (!blob.length)
            throw new Error("Empty layout");
        const want = this.info?.layoutBlobVersion;
        if (want !== undefined) {
            const have = layoutBlobVersion(blob);
            if (have === undefined)
                throw new Error(`Not a layout blob (no BPAK header); the device loads layout blob version ${want}`);
            if (have !== want)
                throw new Error(`Layout blob version ${have}, but the device's firmware loads version ${want}: `
                    + (have > want ? "update the firmware" : "re-bake the layout with a matching version of the tools"));
        }
        const size = blob.length, crc = crc32(blob);
        const capacity = this.info?.layoutCapacity;
        if (capacity && size > capacity)
            throw new RangeError(`Layout is ${size} bytes; the device stores at most ${capacity}`);
        if (!force && this.info && this.info.layoutSize === size && this.info.layoutCrc === crc)
            return { uploaded: false, size, crc };

        await this.call(Msg.LAYOUT_BEGIN, encodeLayoutBegin(size, crc));

        // A small window of chunks in flight (the device answers each once
        // it's in flash); after a failure nothing more is sent
        const chunk = this._maxPayload() - LAYOUT_DATA_PREFIX;
        const active = new Set();
        let offset = 0, sent = 0, failure = null;
        while (true) {
            while (!failure && offset < size && active.size < LAYOUT_WINDOW) {
                const part = blob.subarray(offset, offset + chunk);
                offset += part.length;
                const p = this.call(Msg.LAYOUT_DATA, encodeLayoutData(offset - part.length, part))
                    .then(() => { sent += part.length; onProgress?.(sent, size); },
                          err => { failure ??= err; })
                    .finally(() => active.delete(p));
                active.add(p);
            }
            if (!active.size)
                break;
            await Promise.race(active);
        }
        if (failure)
            throw failure;

        this._restarting = true;        // expected from here: the device drops off the bus
        try {
            await this.call(Msg.LAYOUT_COMMIT, undefined, { timeoutMs: LAYOUT_COMMIT_TIMEOUT });
        } catch (err) {
            this._restarting = false;
            throw err;
        }
        if (this.info) {
            this.info.layoutSize = size;
            this.info.layoutCrc = crc;
        }
        return { uploaded: true, size, crc };
    }

    // After uploadLayout() (or any restart): wait for this connection to end,
    // then for the same device (by serial) to come back. Resolves to a new,
    // connected ControlBlock. This one is closed.
    async reconnect({ timeoutMs = RESTART_TIMEOUT, intervalMs = RESTART_POLL, opener } = {})
    {
        const serial = this.info?.serial;
        const deadline = Date.now() + timeoutMs;
        while (this.connected && Date.now() < deadline)
            await new Promise(resolve => setTimeout(resolve, Math.min(intervalMs, 50)));
        await this.close();
        return ControlBlock.waitOpen({ serial, timeoutMs: Math.max(0, deadline - Date.now()), intervalMs, opener });
    }

    // Open a device, trying again until it appears (it takes a second or two
    // to restart). `opener` replaces ControlBlock.open (for tests).
    static async waitOpen({ timeoutMs = RESTART_TIMEOUT, intervalMs = RESTART_POLL, opener = o => ControlBlock.open(o), ...options } = {})
    {
        const deadline = Date.now() + timeoutMs;
        let last = null;
        while (true) {
            try {
                return await opener(options);
            } catch (err) {
                last = err;
            }
            if (Date.now() >= deadline)
                throw new Error(`Device did not come back within ${timeoutMs} ms: ${last?.message}`);
            await new Promise(resolve => setTimeout(resolve, intervalMs));
        }
    }

    // The ID goes at once; the memory once no key uses it
    async freeImages(ids)
    {
        await Promise.all(this._idBatches(ids).map(b => this.call(Msg.FREE_IMAGES, encodeIds(b))));
    }

    // One bool per ID: is it on the device?
    async queryImages(ids)
    {
        const results = await Promise.all(this._idBatches(ids).map(b => this.call(Msg.QUERY_IMAGES, encodeIds(b))));
        return results.flatMap(decodeQuery);
    }

    // Upload whichever of `images` ({ id, data }) the device doesn't have.
    // Returns how many were uploaded.
    async syncImages(images)
    {
        const list = [...images];
        const present = await this.queryImages(list.map(i => i.id));
        const missing = list.filter((_, i) => !present[i]);
        await Promise.all(missing.map(i => this.putImage(i.id, i.data)));
        return missing.length;
    }

    async setPageCount(count)
    {
        await this.call(Msg.SET_PAGE_COUNT, Uint8Array.of(count));
        if (this.info)
            this.info.pageCount = count;
    }

    // Define, replace or remove keys by id (docs/protocol.md, SET_KEYS):
    //   { id, page, x, y, w = 1, h = 1, normal, pressed, disabled }   add the key, or replace the one with this id
    //   { id, remove: true }                                         remove it
    // id: 1..65535, unique across all pages; a key stays on its page (remove it first to move it).
    // x, y, w, h: in grid units (1U = info.keyWidth px), multiples of 0.25 (keySlot(i) gives a
    // plain grid slot). normal / pressed: image IDs (or keyImage results), each exactly the key's
    // size (keyPixelSize(key, info)) and opaque; pressed 0 or missing = no press feedback. Keys not
    // named stay. Applied in one go, all or nothing: the device refuses overlaps, keys outside the
    // grid, more than info.maxKeys on a page (BAD_ARG), a missing image (NOT_FOUND) or one of the
    // wrong size (BAD_IMAGE). A value that can't be encoded throws before anything is sent.
    async setKeys(keys)
    {
        const max = Math.floor((this._maxPayload() - 2) / KEYDEF_SIZE);
        if (keys.length > max)
            throw new Error(`Too many keys in one SET_KEYS (${keys.length}, max ${max})`);
        await this.call(Msg.SET_KEYS, encodeKeys(keys));
    }

    // Remove keys by id (ids with no key are ignored)
    async removeKeys(ids)
    {
        await this.setKeys(ids.map(id => ({ id, remove: true })));
    }

    async setPage(page, { animate = false } = {})
    {
        await this.call(Msg.SET_PAGE, Uint8Array.of(page, animate ? 1 : 0));
    }

    // --- Internals ---

    _maxPayload()
    {
        return this.info?.maxPayload ?? 1024;
    }

    _idBatches(ids)
    {
        const per = Math.floor((this._maxPayload() - 2) / 4);
        const batches = [];
        for (let i = 0; i < ids.length; i += per)
            batches.push(ids.slice(i, i + per));
        return batches.length ? batches : [[]];
    }

    _nextSeq()
    {
        do
            this._seq = (this._seq + 1) & 0xFFFF;
        while (this._pending.has(this._seq));
        return this._seq;
    }

    // Writes go out in order, one transfer at a time. A transfer of a whole
    // number of packets is ended with a zero-length packet: the device only
    // sees the end of a transfer at a short packet (docs/protocol.md)
    // `smallReply`: the message's reply is small, so reads may be held while it's queued (READ_HOLD_MS)
    _write(bytes, smallReply = false)
    {
        this._queuedWrites++;
        if (!smallReply) {
            this._bigReplyWrites++;
            this._releaseRead?.();
        }
        const p = this._writes.then(async () => {
            await this._transferOut(bytes);
            if (bytes.length % USB_PACKET_SIZE === 0)
                await this._transferOut(new Uint8Array(0));
        }).finally(() => {
            this._queuedWrites--;
            if (!smallReply)
                this._bigReplyWrites--;
            if (!this._queuedWrites)
                this._releaseRead?.();
        });
        this._writes = p.catch(() => { });
        return p;
    }

    // Before submitting a read: wait while only small-reply writes are queued, up to readHoldMs
    _holdRead()
    {
        if (!this._queuedWrites || this._bigReplyWrites || !this.connected)
            return null;
        return new Promise(resolve => {
            const done = () => {
                clearTimeout(timer);
                this._releaseRead = null;
                resolve();
            };
            const timer = setTimeout(done, this.readHoldMs);
            this._releaseRead = done;
        });
    }

    async _transferOut(bytes)
    {
        const r = await this.device.transferOut(ENDPOINT, bytes);
        if (r.status !== "ok")
            throw new Error(`transferOut: ${r.status}`);
    }

    async _readLoop()
    {
        while (this.connected) {
            await this._holdRead();
            if (!this.connected)
                break;
            let r;
            try {
                r = await this.device.transferIn(ENDPOINT, READ_SIZE, READ_TIMEOUT_MS);
            } catch (err) {
                if (!this.connected)
                    break;
                // Nothing to read yet: node usb reports a read timeout as
                // "Cancelled" (other versions may say "timed out")
                if (/time|cancel/i.test(String(err?.message ?? err)))
                    continue;
                if (this._restarting) {         // the device restarted after a layout commit
                    await this.close();
                    break;
                }
                this.emit("error", err);
                await this.close();
                break;
            }
            if (r.status === "stall") {
                try { await this.device.clearHalt("in", ENDPOINT); } catch { }
                continue;
            }
            if (r.data && r.data.byteLength)
                this._onBytes(new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength));
        }
    }

    _onBytes(bytes)
    {
        for (const msg of this._parser.push(bytes)) {
            if (msg.flags & Flags.EVENT) {
                const ev = decodeEvent(msg);
                if (ev)
                    this.emit(ev.type, ev);
                continue;
            }
            const p = this._pending.get(msg.seq);
            if (!(msg.flags & Flags.RESPONSE) || !p || p.type !== msg.type)
                continue;               // stale (from before a stream reset)
            this._pending.delete(msg.seq);
            clearTimeout(p.timer);
            const status = msg.payload.length >= 2 ? msg.payload[0] | msg.payload[1] << 8 : Status.BAD_LENGTH;
            p.resolve({ status, data: msg.payload.subarray(4) });
        }
    }

    _fail(seq, err)
    {
        const p = this._pending.get(seq);
        if (!p)
            return;
        this._pending.delete(seq);
        clearTimeout(p.timer);
        p.reject(err);
    }

    _failAll(err)
    {
        for (const seq of [...this._pending.keys()])
            this._fail(seq, err);
    }

    // A response didn't come: the stream is out of step, so start afresh
    _timeout(seq)
    {
        const p = this._pending.get(seq);
        if (!p)
            return;
        this._fail(seq, new Error(`${typeName(p.type)} timed out`));
        if (this.connected)
            this.resetStream().catch(err => this.emit("error", err));
    }
}
