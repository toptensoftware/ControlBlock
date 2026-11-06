#!/usr/bin/env node
// Decodes the pad link and prints it: HELLO, key down/up, encoder turns, errors, stats.
//   node dump.js <COMx|/dev/ttyUSB0>           read a serial port (115200 8N1)
//   node dump.js --file <capture|->            replay a capture file, or stdin with "-"
// Options: --raw (also hex-dump every chunk), --stats <seconds> (default 5, 0 = off)
// The decoding is in src/pad_dump.js (tested); this file only moves bytes.
import fs from "node:fs";
import { parseArgs } from "node:util";
import { SerialPort } from "serialport";
import { PadDump, formatEvent, formatStats } from "./src/pad_dump.js";

const BAUD = 115200;
const TICK_MS = 20;

const usage = `usage: node dump.js <COMx> [--raw] [--stats <seconds>]
       node dump.js --file <capture|-> [--raw] [--stats <seconds>]`;

let args;
try {
    args = parseArgs({
        allowPositionals: true,
        options: { file: { type: "string" }, raw: { type: "boolean" }, stats: { type: "string", default: "5" }, help: { type: "boolean", short: "h" } },
    });
} catch (e) {
    console.error(`${e.message}\n${usage}`);
    process.exit(2);
}
const { file, raw, help } = args.values;
const statsSeconds = Number(args.values.stats);
if (help || (!file && args.positionals.length !== 1) || !(statsSeconds >= 0)) {
    console.error(usage);
    process.exit(help ? 0 : 2);
}

const t0 = performance.now();
const now = () => performance.now() - t0;
const dump = new PadDump({ now, onEvent: e => console.log(formatEvent(e)) });

function onData(chunk) {
    if (raw) console.log(`${(now() / 1000).toFixed(3).padStart(8)}  raw ${chunk.length}: ${chunk.toString("hex").replace(/(..)/g, "$1 ").trim()}`);
    dump.feed(chunk);
}

const timers = [setInterval(() => dump.tick(), TICK_MS)];
if (statsSeconds > 0) timers.push(setInterval(() => console.log(formatStats(dump.stats())), statsSeconds * 1000));

let closing = false;
function finish(code = 0) {
    if (closing) return;
    closing = true;
    timers.forEach(clearInterval);
    console.log(formatStats(dump.stats()));
    close(() => process.exit(code));
}
let close = cb => cb();
process.on("SIGINT", () => finish(0));

if (file) {
    const stream = file === "-" ? process.stdin : fs.createReadStream(file);
    stream.on("data", onData);
    stream.on("end", () => finish(0));
    stream.on("error", e => { console.error(`${file}: ${e.message}`); finish(1); });
} else {
    const path = args.positionals[0];
    const port = new SerialPort({ path, baudRate: BAUD, dataBits: 8, stopBits: 1, parity: "none", autoOpen: false });
    close = cb => (port.isOpen ? port.close(() => cb()) : cb());
    port.on("data", onData);
    port.on("error", e => console.error(`serial error: ${e.message}`));
    port.on("close", () => { if (!closing) { console.error("port closed"); finish(1); } });
    port.open(async err => {
        if (err) {
            console.error(`cannot open ${path}: ${err.message}`);
            const ports = await SerialPort.list().catch(() => []);
            if (ports.length) console.error(`available: ${ports.map(p => p.path).join(", ")}`);
            finish(1);
            return;
        }
        console.log(`listening on ${path} at ${BAUD} 8N1 (Ctrl+C to stop)`);
    });
}
