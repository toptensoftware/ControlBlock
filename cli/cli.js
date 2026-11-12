#!/usr/bin/env node
// The ControlBlock command line (cli/README.md): argument parsing and output only; the
// work is the library's (../host). Run `node cli/cli.js` for the commands.
//
// Exit codes: 0 done, 1 failed, 2 usage error.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { ControlBlock, BakeError, WifiState, ClockSource, bakeFile, previewFile, encodePng, formatEntries, uploadBlob } from "../host/index.js";
import { parseArgs, UsageError } from "./args.js";

function printWarnings(warnings, io)
{
    if (warnings.length)
        io.stderr(`warnings:\n${formatEntries(warnings)}`);
}

// Bake a layout file; prints its warnings. Resolves the blob.
async function bakeLayout(input, io)
{
    const { blob, warnings } = await bakeFile(input);
    printWarnings(warnings, io);
    return blob;
}

// A page's file name: its number in the whole layout and its name, e.g. 01-watch-tv-1.png
const pageFile = p => `${String(p.index).padStart(2, "0")}-${p.name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "")}.png`;

const defaultBlobPath = input => path.join(path.dirname(input), path.basename(input, path.extname(input)) + ".bin");

function upload(blob, a, io)
{
    return uploadBlob(blob, { serial: a.serial, force: a.force, log: io.stdout, open: io.open, reconnect: io.reconnect });
}

const COMMANDS = {
    async bake(a, io)
    {
        const blob = await bakeLayout(a.input, io);
        // Uploading doesn't need the file; baking alone writes it next to the input
        const out = a.out ?? (a.upload ? undefined : defaultBlobPath(a.input));
        if (out)
            fs.writeFileSync(out, blob);
        io.stdout(`${out ?? a.input}: ${blob.length} bytes`);
        if (a.upload)
            await upload(blob, a, io);
        return 0;
    },

    async upload(a, io)
    {
        let blob;
        if (a.binary)
            blob = fs.readFileSync(a.input);
        else
        {
            blob = await bakeLayout(a.input, io);
            io.stdout(`${a.input}: ${blob.length} bytes`);
        }
        await upload(blob, a, io);
        return 0;
    },

    async render(a, io)
    {
        const { pages, warnings } = await previewFile(a.input, { state: a.state, page: a.page, grid: a.grid });
        printWarnings(warnings, io);
        fs.mkdirSync(a.out, { recursive: true });
        for (const p of pages)
        {
            const file = path.join(a.out, pageFile(p));
            fs.writeFileSync(file, await encodePng(p));
            io.stdout(file);
        }
        return 0;
    },

    wifi: (a, io) => runAction(WIFI, a, io),
    time: (a, io) => runAction(TIME, a, io),
    name: (a, io) => runAction(NAME, a, io),
};

// Opens the device, runs the command's action from `actions` and closes it again;
// resolves the action's exit code
async function runAction(actions, a, io)
{
    const cb = await io.open({ serial: a.serial });
    try
    {
        return await actions[a.action](cb, a, io);
    }
    finally
    {
        await cb.close();
    }
}

// A Wi-Fi status on one line: state, address and signal once connected, the last
// disconnect reason while retrying, and the MAC once there's a network to join
function wifiText(st)
{
    const parts = [st.stateName];
    if (st.ip)
        parts.push(st.ip, `${st.rssi} dBm`);
    if (st.state === WifiState.RETRYING)
        parts.push(`last disconnect reason ${st.reason}`);
    if (st.state !== WifiState.NO_CREDENTIALS)
        parts.push(`MAC ${st.mac}`);
    return parts.join(", ");
}

// Wait until connected (exit 0) or the time is up (exit 1), printing each new status
async function waitConnected(cb, seconds, io)
{
    return await cb.waitWifiConnected({ timeoutMs: seconds * 1000, onStatus: st => io.stdout(wifiText(st)) }) ? 0 : 1;
}

// wifi actions (docs/wifi.md): resolve the exit code
const WIFI = {
    async set(cb, a, io)
    {
        await cb.setWifi(a.ssid, a.password);
        io.stdout(`stored "${a.ssid}"`);
        return waitConnected(cb, a.seconds, io);
    },
    async get(cb, a, io)
    {
        const w = await cb.getWifi();
        io.stdout(w.ssid ? `ssid: ${w.ssid}\npassword: ${w.password || "(none: open network)"}` : "no network stored");
        return 0;
    },
    async clear(cb, a, io)
    {
        await cb.clearWifi();
        io.stdout("cleared");
        return 0;
    },
    async status(cb, a, io)
    {
        io.stdout(wifiText(await cb.wifiStatus()));
        return 0;
    },
    wait: (cb, a, io) => waitConnected(cb, a.seconds, io),
};

const SOURCE_TEXT = { [ClockSource.HOST]: " (set by the host)", [ClockSource.NTP]: " (set by NTP)" };

// The device's clock settings, one line each, as `time get` shows them
const clockLines = {
    zone: t => `time zone: ${t.timeZone}`,
    server: t => `ntp server: ${t.ntpServer}`,
};

// time actions (docs/protocol.md, Clock): resolve the exit code. None needs control of
// the device: they work while it runs standalone.
const TIME = {
    async set(cb, a, io)
    {
        const now = new Date();
        await cb.setTime(now);
        io.stdout(`clock set to ${now.toISOString()}`);
        return 0;
    },
    async get(cb, a, io)
    {
        const t = await cb.getTime();
        io.stdout(t.known ? `time: ${t.time.toISOString()}${SOURCE_TEXT[t.source] ?? ""}` : "time: not set");
        for (const line of Object.values(clockLines))
            io.stdout(line(t));
        return 0;
    },
    zone: (cb, a, io) => setClock(cb, "zone", { timeZone: a.value }, io),
    server: (cb, a, io) => setClock(cb, "server", { ntpServer: a.value }, io),
};

// Store one clock setting, then show what the device now uses (the default, after --default)
async function setClock(cb, action, settings, io)
{
    await cb.setClock(settings);
    io.stdout(clockLines[action](await cb.getTime()));
    return 0;
}

// name actions (docs/protocol.md, Device name): resolve the exit code. Standalone is fine.
const showName = async (cb, io) => {
    const n = await cb.getName();
    io.stdout(`name: ${n.name}${n.isDefault ? " (default)" : ""}`);
    return 0;
};
const NAME = {
    get: (cb, a, io) => showName(cb, io),
    async set(cb, a, io)
    {
        await cb.setName(a.value);
        return showName(cb, io);
    },
};

// Runs a command line; resolves the exit code. `open(options)` opens a device (default
// ControlBlock.open) and `reconnect` ({ timeoutMs, intervalMs }) tunes the wait for a
// restart: tests inject a fake device. `stdout(line)` and `stderr(line)` take the output.
export async function main(argv, { open = o => ControlBlock.open(o), reconnect, stdout = l => console.log(l), stderr = l => console.error(l) } = {})
{
    let args;
    try
    {
        args = parseArgs(argv);
    }
    catch (err)
    {
        if (!(err instanceof UsageError))
            throw err;
        stderr(err.message);
        return 2;
    }

    try
    {
        return await COMMANDS[args.command](args, { open, reconnect, stdout, stderr });
    }
    catch (err)
    {
        stderr(err instanceof BakeError ? err.message : `error: ${err.message}`);
        return 1;
    }
}

// Run when executed (not when imported by the tests)
if (process.argv[1] && fs.realpathSync(process.argv[1]) === fileURLToPath(import.meta.url))
    process.exit(await main(process.argv.slice(2)));
