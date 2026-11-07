#!/usr/bin/env node
// The ControlBlock command line (cli/README.md): argument parsing and output only; the
// work is the library's (../host). Run `node cli/cli.js` for the commands.
//
// Exit codes: 0 done, 1 failed, 2 usage error.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { ControlBlock, BakeError, WifiState, bakeFile, formatEntries, uploadBlob } from "../host/index.js";
import { parseArgs, UsageError } from "./args.js";

// Bake a layout file; prints its warnings. Resolves the blob.
async function bakeLayout(input, io)
{
    const { blob, warnings } = await bakeFile(input);
    if (warnings.length)
        io.stderr(`warnings:\n${formatEntries(warnings)}`);
    return blob;
}

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

    async wifi(a, io)
    {
        const cb = await io.open({ serial: a.serial });
        try
        {
            return await WIFI[a.action](cb, a, io);
        }
        finally
        {
            await cb.close();
        }
    },
};

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
