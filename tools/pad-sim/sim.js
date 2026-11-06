#!/usr/bin/env node
// Plays a simulated pad onto a serial port, as the C3 would (for testing the S3 without it).
//   node sim.js <COMx|-> [script-file] [-e "press 3 200; spin 1 +10 5"] [options]
// "-" writes to stdout instead (pipe it into `node dump.js --file -`).
// Options: --noise <rate>  --seed <n>  --seq-start <n>  --keys <n>  --encoders <n>
//          --lead <ms>  --loop  --duration <ms>
// The script format and the pad behaviour are in README.md and src/pad_sim.js (tested).
import fs from "node:fs";
import { parseArgs } from "node:util";
import { SerialPort } from "serialport";
import * as W from "./src/pad_wire.js";
import { PadSim, parseScript, makeRng } from "./src/pad_sim.js";

const BAUD = 115200;
const POLL_MS = 1;
const LINGER_MS = W.PAD_STATE_REPEAT_MS * 3;   // after the script ends, keep repeating STATE a little

const usage = `usage: node sim.js <COMx|-> [script-file] [-e "commands"] [--noise <rate>] [--seed <n>]
                  [--seq-start <n>] [--keys <n>] [--encoders <n>] [--lead <ms>] [--loop] [--duration <ms>]`;

let args;
try {
    args = parseArgs({
        allowPositionals: true,
        options: {
            e: { type: "string", short: "e" }, noise: { type: "string", default: "0" }, seed: { type: "string" },
            "seq-start": { type: "string", default: "0" }, keys: { type: "string", default: "6" }, encoders: { type: "string", default: "1" },
            lead: { type: "string" }, loop: { type: "boolean" }, duration: { type: "string" }, help: { type: "boolean", short: "h" },
        },
    });
} catch (e) {
    console.error(`${e.message}\n${usage}`);
    process.exit(2);
}
const v = args.values;
const num = (name, value) => {
    const n = Number(value);
    if (!Number.isFinite(n) || n < 0) { console.error(`--${name} must be a non-negative number\n${usage}`); process.exit(2); }
    return n;
};
if (v.help || args.positionals.length < 1 || args.positionals.length > 2) {
    console.error(usage);
    process.exit(v.help ? 0 : 2);
}

let script;
try {
    const text = [v.e, args.positionals[1] && fs.readFileSync(args.positionals[1], "utf8")].filter(s => s !== undefined && s !== null).join("\n");
    script = parseScript(text);
} catch (e) {
    console.error(e.message);
    process.exit(2);
}

const noise = num("noise", v.noise);
let sim;
try {
    sim = new PadSim({
        keys: num("keys", v.keys), encoders: num("encoders", v.encoders), script,
        seqStart: num("seq-start", v["seq-start"]), loop: !!v.loop, noise,
        rng: v.seed !== undefined ? makeRng(num("seed", v.seed)) : Math.random,
        ...(v.lead !== undefined && { lead: num("lead", v.lead) }),
    });
} catch (e) {
    console.error(e.message);
    process.exit(2);
}
const duration = v.duration !== undefined ? num("duration", v.duration) : null;

let write = buf => process.stdout.write(buf);
let close = cb => process.stdout.write("", () => cb());   // flush the pipe before exiting
const toStdout = args.positionals[0] === "-";
if (!toStdout) {
    const path = args.positionals[0];
    const port = new SerialPort({ path, baudRate: BAUD, dataBits: 8, stopBits: 1, parity: "none", autoOpen: false });
    write = buf => port.write(buf);
    close = cb => (port.isOpen ? port.drain(() => port.close(() => cb())) : cb());
    port.on("error", e => console.error(`serial error: ${e.message}`));
    await new Promise(resolve => port.open(async err => {
        if (err) {
            console.error(`cannot open ${path}: ${err.message}`);
            const ports = await SerialPort.list().catch(() => []);
            if (ports.length) console.error(`available: ${ports.map(p => p.path).join(", ")}`);
            process.exit(1);
        }
        resolve();
    }));
}
// Status goes to stderr so stdout stays clean as the data stream
console.error(`playing to ${toStdout ? "stdout" : args.positionals[0]} (noise ${noise}${v.loop ? ", looping" : ""}); Ctrl+C to stop`);

const t0 = performance.now();
sim.start(0);
const timer = setInterval(() => {
    const now = performance.now() - t0;
    for (const chunk of sim.poll(now)) write(chunk);
    const done = duration !== null ? now >= duration : sim.finished && sim.elapsed >= sim.script.end + LINGER_MS;
    if (done) stop();
}, POLL_MS);

let stopped = false;
function stop() {
    if (stopped) return;
    stopped = true;
    clearInterval(timer);
    close(() => process.exit(0));
}
process.on("SIGINT", stop);
