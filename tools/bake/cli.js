#!/usr/bin/env node
// node cli.js config.(json|js) [-o out.bin] [--header types.h] [--upload [--serial S] [--force]]
// node cli.js baked.bin [--serial S] [--force]
// Bakes a standalone-mode config (docs/config-format.md) into the BPAK blob for the device,
// and/or uploads the blob to the device over USB (docs/protocol.md "Configuration upload").

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { bake, BakeError } from "./src/bake.js";
import { createNodeHost } from "./src/host-node.js";
import { createRenderer } from "./src/render-node.js";
import { headerText } from "./src/pack.js";
import { formatEntries } from "./src/diag.js";
import { parseArgs } from "./src/args.js";
import { uploadBlob } from "./src/upload.js";

async function loadConfig(file)
{
    if (path.extname(file).toLowerCase() === ".json")
        return JSON.parse(fs.readFileSync(file, "utf8"));
    return (await import(pathToFileURL(path.resolve(file)).href)).default;
}

async function main(argv)
{
    const args = parseArgs(argv);
    let blob;

    if (args.binary)
        blob = fs.readFileSync(args.input);
    else
    {
        const host = await createNodeHost(path.dirname(path.resolve(args.input)));
        const baked = await bake(await loadConfig(args.input), host, createRenderer());
        blob = baked.blob;
        if (baked.warnings.length)
            console.error(`warnings:\n${formatEntries(baked.warnings)}`);

        // Uploading doesn't need the file; baking alone writes it next to the input
        const out = args.out ?? (args.upload ? undefined : path.join(path.dirname(args.input), path.basename(args.input, path.extname(args.input)) + ".bin"));
        if (out)
        {
            fs.writeFileSync(out, blob);
            console.log(`${out}: ${blob.length} bytes`);
        }
        else
            console.log(`${args.input}: ${blob.length} bytes`);
        if (args.header)
            fs.writeFileSync(args.header, headerText());
    }

    if (args.upload)
        await uploadBlob(blob, { serial: args.serial, force: args.force, log: line => console.log(line) });
}

main(process.argv.slice(2)).catch(err => {
    console.error(err instanceof BakeError ? err.message : `error: ${err.message}`);
    process.exit(1);
});
