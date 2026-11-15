// A stand-in WebUSB USBDevice that answers ControlBlock requests, for
// testing the library without hardware. `handler(msg)` returns the response
// { status, data } for a request, or null to stay silent.

import { unpackRle } from "rle565/unpack.js";
import { ControlBlock } from "../src/ControlBlock.js";
import {
    MessageParser, encodeMessage, Writer, Reader, Flags, Msg, Status, InfoFlags, PROTOCOL_VERSION, USB_PACKET_SIZE, crc32,
    ClockSource, TIME_VALID_FROM_MS, KEYDEF_SIZE, KEY_EVENT_SIZE, KeyFlags, PUT_IMAGE_PREFIX, STATUS_TEXT_MAX_BYTES,
    BPAK_SIGNATURE, BPAK_HEADER_SIZE,
} from "../src/protocol.js";
import { BLOB_VERSION } from "../src/bake/consts.js";

// The firmware's constants (key_geometry.h, screen_layout.h, key_layout.h); a test checks them
// against the headers. The library itself never assumes these: it reads GET_INFO.
export const DEVICE = Object.freeze({
    screenWidth: 800, screenHeight: 480, statusHeight: 40,
    cols: 5, rows: 3, cell: 144, quarter: 36, maxKeys: 32, maxPages: 8, icon: 80,
    get gridWidth() { return this.cols * this.cell; },
    get gridHeight() { return this.rows * this.cell; },
    get gridX() { return (this.screenWidth - this.gridWidth) / 2; },
    get gridY() { return this.statusHeight + (this.screenHeight - this.statusHeight - this.gridHeight) / 2; },
});
const GRID_QW = DEVICE.cols * 4, GRID_QH = DEVICE.rows * 4;     // the grid in quarter units
const IMAGE_CAPACITY = 5000000;

// A stand-in layout blob of `size` bytes (at least 36): a BPAK header with the root's `version`
// (as the bake writes it), then pseudo-random bytes. uploadLayout only reads the version.
export function layoutBlobOf(size, { seed = 1, version = BLOB_VERSION } = {})
{
    const blob = new Uint8Array(size).map((_, i) => ((i + seed) * 2654435761) >>> 24);
    const view = new DataView(blob.buffer);
    view.setUint32(0, BPAK_SIGNATURE, true);
    view.setUint32(BPAK_HEADER_SIZE, version, true);
    return blob;
}

// GET_INFO's payload. blobVersion: null leaves the field out, like firmware from before it.
export function infoPayload({ maxPayload = 8192, pageCount = 1, currentPage = 0, imageUsed = 0, imageCount = 0, backlight = true, wifiCredentials = false, hosted = false, timeSet = false, layout, blobVersion = BLOB_VERSION, firmwareVersion = "1.0" } = {})
{
    const w = new Writer(128);
    const flags = (backlight ? InfoFlags.BACKLIGHT : 0) | (wifiCredentials ? InfoFlags.WIFI_CREDENTIALS : 0) | (hosted ? InfoFlags.HOSTED : 0) | (timeSet ? InfoFlags.TIME_SET : 0);
    w.u16(PROTOCOL_VERSION).u16(flags).u32(maxPayload).u32(IMAGE_CAPACITY).u32(imageUsed).u32(imageCount).u32(IMAGE_CAPACITY - imageUsed);
    w.u16(DEVICE.screenWidth).u16(DEVICE.screenHeight).u8(DEVICE.cols).u8(DEVICE.rows).u8(DEVICE.maxPages).u8(pageCount);
    w.u16(DEVICE.cell).u16(DEVICE.cell).u16(DEVICE.icon).u16(DEVICE.icon).u8(currentPage);
    for (const s of [firmwareVersion, "SERIAL1"]) {
        const b = new TextEncoder().encode(s);
        w.u8(b.length).raw(b);
    }
    w.u32(layout?.capacity ?? 0).u32(layout?.size ?? 0).u32(layout?.crc ?? 0);
    w.u16(DEVICE.gridX).u16(DEVICE.gridY).u16(DEVICE.gridWidth).u16(DEVICE.gridHeight).u16(DEVICE.quarter);
    w.u16(DEVICE.gridWidth).u16(DEVICE.gridHeight).u8(DEVICE.maxKeys);
    if (blobVersion != null)
        w.u16(blobVersion);
    return w.done();
}

// The image's size if the device would take it (a valid rle565 image, 1x1 up to the grid, every
// pixel opaque: the firmware's check_image with opaque required), else null
function checkImage(bytes)
{
    let img;
    try {
        img = unpackRle(Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength));
    } catch {
        return null;
    }
    if (img.width < 1 || img.height < 1 || img.width > DEVICE.gridWidth || img.height > DEVICE.gridHeight)
        return null;
    for (let i = 3; i < img.data.length; i += 4)
        if (img.data[i] !== 255)
            return null;
    return { width: img.width, height: img.height };
}

const rectValid = k => k.w > 0 && k.h > 0 && k.x + k.w <= GRID_QW && k.y + k.h <= GRID_QH;
const rectsOverlap = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;

const USB_RX_TRANSFER = 4096;   // the firmware's receive transfer (CONFIG_TINYUSB_VENDOR_EPSIZE)

// Requests that edit the keys, images or status bar: refused while standalone
const HOSTED_REQUESTS = [Msg.RESET, Msg.SET_STATUS_TEXT, Msg.PUT_IMAGE, Msg.FREE_IMAGES, Msg.SET_PAGE_COUNT, Msg.SET_KEYS, Msg.SET_PAGE];
const FAKE_DEFAULT_NAME = "controlblock-a1b2c3";   // NAME_GET with no name stored

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
    // blobVersion: GET_INFO's layout_blob_version (default the bake's BLOB_VERSION); null leaves it out.
    // firmwareVersion: GET_INFO's fw_version (default "1.0").
    // clock: emulate the clock messages (docs/protocol.md, Clock): { offset, source, tz, server }.
    //   offset: device time minus Date.now() in ms, null while the time isn't known (the default).
    // name: emulate NAME_SET/NAME_GET (docs/protocol.md, Device name): { name } stored, null (the default) for none.
    // The screen requests (RESET, SET_STATUS_TEXT, PUT_IMAGE, FREE_IMAGES, QUERY_IMAGES, SET_PAGE_COUNT,
    //   SET_KEYS, SET_PAGE) run against a model of the firmware's image store and key layout (protocol
    //   v2: opaque images only, keys by id with quarter-unit rects, the same checks and statuses).
    //   Read it with keys(page), `images`, `pageCount`, `currentPage`, `statusText`; keyEvent() sends a
    //   KEY event for a key, as a touch would.
    // handler(msg): if given, asked first about every screen request (and any other type the fake
    //   doesn't know): a { status, data } answers, null stays silent, undefined leaves it to the model.
    constructor({ maxPayload = 8192, handler, backlight = true, layout, wifi, clock, name, hosted = false, controlStatus = Status.OK, blobVersion = BLOB_VERSION, firmwareVersion = "1.0" } = {})
    {
        this.blobVersion = blobVersion;     // GET_INFO's layout_blob_version; null: not reported (older firmware)
        this.firmwareVersion = firmwareVersion;
        this.clock = clock ? { offset: null, source: 0, tz: "UTC0", server: "pool.ntp.org", ...clock } : null;
        this.name = name ? { name: null, ...name } : null;
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
        this.handler = handler ?? (() => undefined);
        this._eventSeq = 0;
        this._resetScreen();
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
    // 4096-byte receive transfers of 64-byte packets, and a transfer only completes when it is full
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
        if (this.gone)                      // off the bus (restarting): nothing is received or answered
            throw new Error("LIBUSB_ERROR_NO_DEVICE");
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
                ? { status: Status.OK, data: infoPayload({
                    maxPayload: this.maxPayload, backlight: this.backlight, hosted: this.hosted, wifiCredentials: !!this.wifi?.ssid,
                    timeSet: this.clock?.offset != null, pageCount: this.pageCount, currentPage: this.currentPage,
                    imageCount: this.images.size, imageUsed: [...this.images.values()].reduce((n, e) => n + e.size, 0),
                    layout: this.layout && { capacity: this.layout.capacity, size: this.layout.stored?.size ?? 0, crc: this.layout.stored?.crc ?? 0 },
                    blobVersion: this.blobVersion, firmwareVersion: this.firmwareVersion,
                }) }
                : this.wifi && msg.type >= Msg.WIFI_SET && msg.type <= Msg.WIFI_STATUS
                    ? this._wifi(msg)
                : this.clock && msg.type >= Msg.TIME_SET && msg.type <= Msg.CLOCK_SET
                    ? this._clock(msg)
                : this.name && (msg.type === Msg.NAME_SET || msg.type === Msg.NAME_GET)
                    ? this._name(msg)
                : msg.type === Msg.PING
                    ? { status: Status.OK, data: msg.payload }
                    : this._screenRequest(msg);
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
            this._resetScreen();
            if (!take)
                this.backlight = true;
            this.hosted = take;
        }
        return { status: Status.OK, data: new Uint8Array(0) };
    }

    // --- The screen: image store and key layout, as the firmware has them (protocol.cpp,
    // image_store.cpp, key_layout.cpp; the rules are checked by the firmware's own tests) ---

    // Every key and image goes, one empty page, the status texts cleared (RESET, TAKE_CONTROL, RELEASE_CONTROL)
    _resetScreen()
    {
        this.images = new Map();        // live images: id -> entry { id, width, height, size }
        this._upload = null;
        this._pages = [[]];             // per page: keys { id, page, x, y, w, h (quarter units), flags, normal, pressed (entries) }
        this.currentPage = 0;
        this.statusText = ["", ""];
    }

    get pageCount() { return this._pages.length; }

    // A page's keys as the host defined them: { id, page, x, y, w, h (quarter units), disabled, normal, pressed (image IDs, 0 = none) }
    keys(page)
    {
        return (this._pages[page] ?? []).map(k => ({
            id: k.id, page: k.page, x: k.x, y: k.y, w: k.w, h: k.h, disabled: (k.flags & KeyFlags.DISABLED) !== 0,
            normal: k.normal?.id ?? 0, pressed: k.pressed?.id ?? 0,
        }));
    }

    // Send a KEY event for key `id`, as a touch on it would. Throws for what the device can't send
    // (standalone, no such key, a disabled key).
    keyEvent(id, action, { timeMs = 0, lost = false } = {})
    {
        const found = this._findKey(id);
        if (!this.hosted || !found || found.key.flags & KeyFlags.DISABLED)
            throw new Error(`The device sends no KEY event for key ${id} (${!this.hosted ? "standalone" : !found ? "no such key" : "disabled"})`);
        const w = new Writer(KEY_EVENT_SIZE).u16(id).u8(found.page).u8(action).u8(lost ? 1 : 0).u8(0).u8(0).u8(0).u32(timeMs);
        this.event(Msg.KEY, this._eventSeq++ & 0xFFFF, w.done());
    }

    _findKey(id)
    {
        for (let page = 0; page < this._pages.length; page++) {
            const key = this._pages[page].find(k => k.id === id);
            if (key)
                return { page, key };
        }
        return null;
    }

    _screenRequest(msg)
    {
        const r = this.handler(msg);
        if (r !== undefined)
            return r;
        const reply = (status, data = new Uint8Array(0)) => ({ status, data });
        const p = msg.payload, rd = new Reader(p);
        switch (msg.type) {
        case Msg.RESET:
            this._resetScreen();
            return reply(Status.OK);
        case Msg.SET_STATUS_TEXT: {
            if (p.length < 2 || p.length < 2 + p[1])
                return reply(Status.BAD_LENGTH);
            if (p[0] > 1 || p[1] > STATUS_TEXT_MAX_BYTES)
                return reply(Status.BAD_ARG);
            this.statusText[p[0]] = new TextDecoder().decode(p.subarray(2, 2 + p[1]));
            return reply(Status.OK);
        }
        case Msg.PUT_IMAGE:
            return reply(p.length < PUT_IMAGE_PREFIX ? Status.BAD_LENGTH : this._putImage(rd.u32(), rd.u32(), rd.u32(), p.subarray(PUT_IMAGE_PREFIX)));
        case Msg.FREE_IMAGES:
        case Msg.QUERY_IMAGES: {
            const count = rd.u16();
            if (p.length < 2 || !rd.has(count * 4))
                return reply(Status.BAD_LENGTH);
            const ids = Array.from({ length: count }, () => rd.u32());
            if (msg.type === Msg.FREE_IMAGES) {
                ids.forEach(id => this.images.delete(id));      // keys keep the entries they hold
                return reply(Status.OK);
            }
            const out = new Uint8Array(2 + Math.ceil(count / 8));
            new DataView(out.buffer).setUint16(0, count, true);
            ids.forEach((id, i) => { if (this.images.has(id)) out[2 + (i >> 3)] |= 1 << (i & 7); });
            return reply(Status.OK, out);
        }
        case Msg.SET_PAGE_COUNT: {
            if (p.length < 1)
                return reply(Status.BAD_LENGTH);
            if (p[0] < 1 || p[0] > DEVICE.maxPages)
                return reply(Status.BAD_ARG);
            this._pages = Array.from({ length: p[0] }, (_, i) => this._pages[i] ?? []);
            this.currentPage = Math.min(this.currentPage, p[0] - 1);
            return reply(Status.OK);
        }
        case Msg.SET_PAGE:
            if (p.length < 1)
                return reply(Status.BAD_LENGTH);
            if (p[0] >= this.pageCount)
                return reply(Status.BAD_ARG);
            this.currentPage = p[0];
            return reply(Status.OK);
        case Msg.SET_KEYS:
            return reply(this._setKeys(rd));
        }
        return reply(Status.OK);        // anything else the fake doesn't know
    }

    // One PUT_IMAGE chunk (ImageStore::PutBegin/PutEnd)
    _putImage(id, total, offset, chunk)
    {
        if (id === 0 || total === 0 || offset + chunk.length > total)
            return Status.BAD_ARG;
        if (offset === 0) {
            const used = [...this.images.values()].reduce((n, e) => n + e.size, 0);
            if (used + total > IMAGE_CAPACITY) {
                this._upload = null;
                return Status.NO_SPACE;
            }
            this._upload = { id, total, received: 0, bytes: new Uint8Array(total) };
        } else if (!this._upload || this._upload.id !== id || this._upload.total !== total || this._upload.received !== offset) {
            this._upload = null;
            return Status.BAD_SEQUENCE;
        }
        const u = this._upload;
        u.bytes.set(chunk, offset);
        u.received += chunk.length;
        if (u.received < u.total)
            return Status.OK;
        this._upload = null;
        const size = checkImage(u.bytes);
        if (!size)
            return Status.BAD_IMAGE;
        const entry = this.images.get(id);
        if (entry)
            Object.assign(entry, size, { size: total });        // replaced in place: its keys show it
        else
            this.images.set(id, { id, ...size, size: total });
        return Status.OK;
    }

    // SET_KEYS (Protocol::OnSetKeys): edits by id, checked and applied together over the touched pages
    _setKeys(r)
    {
        if (!r.has(2))
            return Status.BAD_LENGTH;
        const count = r.u16();
        if (!r.has(count * KEYDEF_SIZE))
            return Status.BAD_LENGTH;
        const defs = [];
        for (let i = 0; i < count; i++) {
            const id = r.u16(), page = r.u8(), x = r.u8(), y = r.u8(), w = r.u8(), h = r.u8();
            r.u8();
            const flags = r.u16();
            r.u16();
            const normal = r.u32(), pressed = r.u32();
            if (flags & ~(KeyFlags.REMOVE | KeyFlags.DISABLED))
                return Status.BAD_ARG;
            defs.push({ id, page, x, y, w, h, flags: flags & KeyFlags.DISABLED, remove: (flags & KeyFlags.REMOVE) !== 0, normal, pressed });
        }
        const named = new Set(defs.map(d => d.id));
        if (named.has(0) || named.size !== defs.length)
            return Status.BAD_ARG;

        // The pages touched: each def's, and each removed key's; a key stays on its page
        let lo = Infinity, hi = -1;
        for (const d of defs) {
            const found = this._findKey(d.id);
            if (d.remove && !found)
                continue;
            if (!d.remove && found && found.page !== d.page)
                return Status.BAD_ARG;
            const page = d.remove ? found.page : d.page;
            lo = Math.min(lo, page);
            hi = Math.max(hi, page);
        }
        if (hi < 0)
            return Status.OK;
        if (hi >= this.pageCount)
            return Status.BAD_ARG;

        // The touched pages' keys, minus the ones named, plus the new definitions
        const keys = [];
        for (let p = lo; p <= hi; p++)
            for (const k of this._pages[p])
                if (!named.has(k.id))
                    keys.push({ ...k, normal: k.normal?.id ?? 0, pressed: k.pressed?.id ?? 0 });
        keys.push(...defs.filter(d => !d.remove));
        return this._applyKeys(lo, hi, keys);
    }

    // KeyLayout::SetKeys: pages lo..hi become exactly `keys`, all or nothing
    _applyKeys(lo, hi, keys)
    {
        for (let p = lo; p <= hi; p++) {
            const rects = keys.filter(k => k.page === p);
            if (rects.length > DEVICE.maxKeys)
                return Status.BAD_ARG;
            for (let i = 0; i < rects.length; i++)
                if (!rectValid(rects[i]) || rects.slice(0, i).some(o => rectsOverlap(o, rects[i])))
                    return Status.BAD_ARG;
        }
        for (let p = 0; p < this.pageCount; p++)
            if ((p < lo || p > hi) && this._pages[p].some(k => keys.some(n => n.id === k.id)))
                return Status.BAD_ARG;

        // Images: live, or one the same key (on a page being replaced) already holds; the rect's size
        const resolve = (imageId, keyId) => {
            if (this.images.has(imageId))
                return this.images.get(imageId);
            const old = this._findKey(keyId);
            if (!old || old.page < lo || old.page > hi)
                return null;
            return [old.key.normal, old.key.pressed].find(e => e?.id === imageId) ?? null;
        };
        const resolved = [];
        for (const k of keys) {
            const entries = [];
            for (const imageId of [k.normal, k.pressed]) {
                const e = imageId ? resolve(imageId, k.id) : null;
                if (imageId && !e)
                    return Status.NOT_FOUND;
                if (e && (e.width !== k.w * DEVICE.quarter || e.height !== k.h * DEVICE.quarter))
                    return Status.BAD_IMAGE;
                entries.push(e);
            }
            resolved.push(entries);
        }
        for (let p = lo; p <= hi; p++)
            this._pages[p] = [];
        keys.forEach((k, i) => this._pages[k.page].push({
            id: k.id, page: k.page, x: k.x, y: k.y, w: k.w, h: k.h, flags: k.flags, normal: resolved[i][0], pressed: resolved[i][1],
        }));
        return Status.OK;
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

    // The device name messages, as the firmware does them (the rules are checked by the firmware's own tests)
    _name(msg)
    {
        const p = msg.payload;
        if (msg.type === Msg.NAME_SET) {
            if (p.length < 1 || p.length !== 1 + p[0])
                return { status: Status.BAD_LENGTH, data: new Uint8Array(0) };
            this.name.name = new TextDecoder().decode(p.subarray(1)).toLowerCase() || null;
            return { status: Status.OK, data: new Uint8Array(0) };
        }
        const n = new TextEncoder().encode(this.name.name ?? FAKE_DEFAULT_NAME);
        return { status: Status.OK, data: new Writer(2 + n.length).u8(this.name.name ? 0 : 1).u8(n.length).raw(n).done() };
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
                    setTimeout(() => {          // a restarted device has nothing left to send or half-received
                        this._out = [];
                        this._held = new Uint8Array(0);
                        this._parser = new MessageParser();
                        this.gone = false;
                    }, c.restartMs);
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
