#!/usr/bin/env node
// Helpers for the release workflow (.github/workflows/release.yml; wip/wip-versioning-deployment.md, step 9).
//
//   node scripts/release.mjs check-tag <tag> [--root <dir>]
//       Prints the product version if <tag> is v<version> (root, host/ and cli/ package.json agree),
//       else fails.
//   node scripts/release.mjs assets <version> <flasher_args.json> <outDir>
//       Stages the Release assets from a firmware build: the three parts under the flasher's names,
//       flasher_args.json and the merged image (FULL_IMAGE in the build folder, written by
//       .github/workflows/firmware.yml) as controlblock-<version>-full.bin.
//   node scripts/release.mjs notes <version> <owner/repo> <flasher_args.json> <outFile>
//       Renders the release notes from .github/release-notes.md.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { productVersion } from "./bump-version.mjs";
import { buildParts, copyParts } from "./make-flasher-manifest.mjs";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const NOTES_TEMPLATE = path.join(repoRoot, ".github/release-notes.md");
// The merged image's name in the build folder (idf.py merge-bin -o, in .github/workflows/firmware.yml)
export const FULL_IMAGE = "controlblock-full.bin";

const fullImageName = version => `controlblock-${version}-full.bin`;

// The version a release tag (v<x.y.z>, or refs/tags/v<x.y.z>) stands for; it must be the product version.
export function tagVersion(tag, root = repoRoot)
{
    const m = /^(?:refs\/tags\/)?v(\d.*)$/.exec(tag ?? "");
    if (!m) throw new Error(`"${tag}" isn't a release tag (v<version>)`);
    const version = productVersion(root);
    if (m[1] !== version) throw new Error(`tag v${m[1]} doesn't match the product version ${version} (package.json)`);
    return version;
}

// The GitHub Pages project site of "owner/name"
export function pagesUrl(repo)
{
    const m = /^([^/\s]+)\/([^/\s]+)$/.exec(repo ?? "");
    if (!m) throw new Error(`"${repo}" isn't a repository (owner/name)`);
    return `https://${m[1].toLowerCase()}.github.io/${m[2]}/`;
}

// Replaces {{name}} with vars[name]; every placeholder must have a value and every value must be used.
export function renderTemplate(template, vars)
{
    const used = new Set();
    const out = template.replace(/\{\{\s*([A-Za-z_]\w*)\s*\}\}/g, (_, name) => {
        if (!Object.hasOwn(vars, name)) throw new Error(`unknown placeholder {{${name}}}`);
        used.add(name);
        return vars[name];
    });
    const unused = Object.keys(vars).find(k => !used.has(k));
    if (unused) throw new Error(`the template never uses {{${unused}}}`);
    return out;
}

// "0x0 bootloader.bin 0x8000 ..." for esptool write-flash, from the build's flasher_args.json
export function flashCommandParts(version, argsFile)
{
    return buildParts(version, argsFile).map(p => `0x${p.offset.toString(16)} ${p.path}`).join(" ");
}

export function releaseNotes(version, repo, argsFile, { template = NOTES_TEMPLATE } = {})
{
    return renderTemplate(fs.readFileSync(template, "utf8"), {
        version,
        repo,
        flasher_url: pagesUrl(repo),
        flash_parts: flashCommandParts(version, argsFile),
    });
}

// Stages the Release assets in outDir; returns their names. Checks everything before writing.
export function releaseAssets(version, argsFile, outDir)
{
    const parts = buildParts(version, argsFile);
    const full = path.join(path.dirname(path.resolve(argsFile)), FULL_IMAGE);
    if (!fs.statSync(full, { throwIfNoEntry: false })?.isFile()) throw new Error(`missing full image: ${full}`);
    copyParts(parts, outDir);
    fs.copyFileSync(argsFile, path.join(outDir, "flasher_args.json"));
    fs.copyFileSync(full, path.join(outDir, fullImageName(version)));
    return [...parts.map(p => p.path), "flasher_args.json", fullImageName(version)];
}

const USAGE = `usage: node scripts/release.mjs check-tag <tag> [--root <dir>]
       node scripts/release.mjs assets <version> <flasher_args.json> <outDir>
       node scripts/release.mjs notes <version> <owner/repo> <flasher_args.json> <outFile>`;

function main(argv)
{
    if (argv.includes("--help") || argv.includes("-h")) {
        console.log(USAGE);
        return 0;
    }
    const [command, ...rest] = argv;
    const expect = n => { if (rest.length !== n) throw new Error(`${command} takes ${n} argument(s)`); };
    switch (command) {
        case "check-tag": {
            let root = repoRoot;
            const i = rest.indexOf("--root");
            if (i >= 0) {
                if (i + 1 >= rest.length) throw new Error("--root needs a directory");
                root = path.resolve(rest[i + 1]);
                rest.splice(i, 2);
            }
            expect(1);
            console.log(tagVersion(rest[0], root));
            return 0;
        }
        case "assets": {
            expect(3);
            const files = releaseAssets(...rest);
            console.log(`assets ${rest[0]}: ${files.join(", ")} in ${path.resolve(rest[2])}`);
            return 0;
        }
        case "notes": {
            expect(4);
            const [version, repo, argsFile, outFile] = rest;
            fs.writeFileSync(outFile, releaseNotes(version, repo, argsFile));
            return 0;
        }
        default:
            throw new Error(command ? `unknown command ${command}` : "no command given");
    }
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`release: ${e.message}`);
        console.error(USAGE);
        process.exitCode = 1;
    }
}
