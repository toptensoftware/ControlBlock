#!/usr/bin/env node
// Builds the web flasher's site (GitHub Pages) from a firmware build
// (wip/wip-versioning-deployment.md, step 7).
//
//   node scripts/make-flasher-manifest.mjs <version> <flasher_args.json> <outDir>
//
// Reads ESP-IDF's flasher_args.json (build/<preset>/flasher_args.json; its paths are relative to its
// folder) and writes into <outDir>: manifest.json for ESP Web Tools, the three parts it lists
// (bootloader, partition table, app; never a merged image, whose 0xFF padding would wipe NVS) under
// flat names, and the page (flasher/index.html). The part names match the GitHub Release assets.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const PAGE = path.join(repoRoot, "flasher/index.html");

export const CHIP = "esp32s3";
export const CHIP_FAMILY = "ESP32-S3";
export const PRODUCT_NAME = "ControlBlock";

// The parts the flasher writes: flasher_args.json's role key and the published file name
export const PARTS = [
    { role: "bootloader", name: () => "bootloader.bin" },
    { role: "partition-table", name: () => "partition-table.bin" },
    { role: "app", name: version => `controlblock-${version}.bin` },
];

// A version safe in a file name (x.y.z, or a dev suffix like 0.1.0-dev+abc1234)
const VERSION = /^[0-9]+\.[0-9]+\.[0-9]+[0-9A-Za-z.+-]*$/;

// "0x10000" -> 65536
export function parseOffset(text)
{
    if (typeof text !== "string" || !/^0x[0-9a-f]+$/i.test(text)) throw new Error(`bad offset "${text}"`);
    return Number.parseInt(text, 16);
}

// The parts to flash from a parsed flasher_args.json: [{ role, file (as given), offset (number) }],
// in PARTS order. Offsets come from flash_files (what idf.py flash writes); each role's entry says
// which file is which. Refuses another chip, a missing role, or a flash_files entry that isn't one
// of the three parts (it would be left out of the flasher).
export function flashParts(args)
{
    const chip = args?.extra_esptool_args?.chip;
    if (chip !== CHIP) throw new Error(`flasher_args.json is for chip "${chip}", not ${CHIP}`);
    const files = args.flash_files ?? {};
    const byFile = new Map(Object.entries(files).map(([offset, file]) => [file, offset]));
    const parts = PARTS.map(({ role }) => {
        const file = args[role]?.file;
        if (!file) throw new Error(`flasher_args.json has no ${role}`);
        const offset = byFile.get(file);
        if (offset === undefined) throw new Error(`flasher_args.json's flash_files has no ${file} (${role})`);
        return { role, file, offset: parseOffset(offset) };
    });
    const extra = Object.values(files).filter(f => !parts.some(p => p.file === f));
    if (extra.length) throw new Error(`flasher_args.json flashes more than the three parts: ${extra.join(", ")}`);
    return parts;
}

// The ESP Web Tools manifest for `parts` ([{ path, offset }])
export function manifest(version, parts)
{
    return {
        name: PRODUCT_NAME,
        version,
        // Offer "erase device" (unticked): erasing also wipes Wi-Fi settings and the stored layout
        new_install_prompt_erase: true,
        // The firmware doesn't speak Improv (and owns the native USB port): don't wait for it
        new_install_improv_wait_time: 0,
        builds: [{ chipFamily: CHIP_FAMILY, parts: parts.map(({ path, offset }) => ({ path, offset })) }],
    };
}

// The three parts of the build that `argsFile` (flasher_args.json) describes, checked to exist:
// [{ role, file, offset, src (absolute), path (published name) }]. Writes nothing.
export function buildParts(version, argsFile)
{
    if (!VERSION.test(version ?? "")) throw new Error(`"${version}" is not a version (x.y.z[-suffix])`);
    const args = JSON.parse(fs.readFileSync(argsFile, "utf8"));
    const buildDir = path.dirname(path.resolve(argsFile));
    return flashParts(args).map(p => {
        const src = path.join(buildDir, p.file);
        if (!fs.statSync(src, { throwIfNoEntry: false })?.isFile()) throw new Error(`missing ${p.role}: ${src}`);
        return { ...p, src, path: PARTS.find(d => d.role === p.role).name(version) };
    });
}

// Copies `parts` (from buildParts) into outDir (created if needed) under their published names
export function copyParts(parts, outDir)
{
    fs.mkdirSync(outDir, { recursive: true });
    for (const p of parts) fs.copyFileSync(p.src, path.join(outDir, p.path));
}

// Writes the site into outDir (created if needed). Returns { manifest, files } (names in outDir).
export function makeFlasherSite(version, argsFile, outDir, { page = PAGE } = {})
{
    const parts = buildParts(version, argsFile);
    if (!fs.existsSync(page)) throw new Error(`missing page: ${page}`);

    const m = manifest(version, parts);
    copyParts(parts, outDir);
    fs.copyFileSync(page, path.join(outDir, "index.html"));
    fs.writeFileSync(path.join(outDir, "manifest.json"), JSON.stringify(m, null, 2) + "\n");
    return { manifest: m, files: [...parts.map(p => p.path), "index.html", "manifest.json"] };
}

const USAGE = "usage: node scripts/make-flasher-manifest.mjs <version> <flasher_args.json> <outDir>";

function main(argv)
{
    if (argv.includes("--help") || argv.includes("-h")) {
        console.log(USAGE);
        return 0;
    }
    if (argv.length !== 3) throw new Error("expected three arguments");
    const [version, argsFile, outDir] = argv;
    const { files } = makeFlasherSite(version, argsFile, outDir);
    console.log(`flasher ${version}: ${files.join(", ")} in ${path.resolve(outDir)}`);
    return 0;
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`make-flasher-manifest: ${e.message}`);
        console.error(USAGE);
        process.exitCode = 1;
    }
}
