#!/usr/bin/env node
// Bumps the product version (root package.json, shared by the firmware, host/ and cli/).
//
//   node scripts/bump-version.mjs <x.y.z|patch|minor|major> [--force] [--root <dir>]
//
// Writes the version into the root, host/ and cli/ package.json files and the root
// package-lock.json's entries for them, then prints the git commands to commit and tag.
// It never runs git. --force allows a version that isn't newer; --root picks another
// repo root (the tests use a temp copy).

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

// The package.json files that carry the product version, relative to the repo root.
export const VERSION_FILES = ["package.json", "host/package.json", "cli/package.json"];
// Their entries in the root lock's `packages` map ("" is the root).
const LOCK_PACKAGES = ["", "host", "cli"];
const LOCK_FILE = "package-lock.json";

const SEMVER = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;
// The top-level "version" line: package.json files are 2-space indented.
const VERSION_LINE = /^(  "version": ")([^"]*)(")/m;

function parse(version) {
    const m = SEMVER.exec(version);
    if (!m) throw new Error(`"${version}" is not a version (x.y.z)`);
    return m.slice(1).map(Number);
}

function compare(a, b) {
    const pa = parse(a), pb = parse(b);
    for (let i = 0; i < 3; i++) if (pa[i] !== pb[i]) return pa[i] - pb[i];
    return 0;
}

// The version after `current` for `spec` (patch, minor, major or an explicit x.y.z).
export function nextVersion(current, spec, { force = false } = {}) {
    const [major, minor, patch] = parse(current);
    let next;
    if (spec === "patch") next = `${major}.${minor}.${patch + 1}`;
    else if (spec === "minor") next = `${major}.${minor + 1}.0`;
    else if (spec === "major") next = `${major + 1}.0.0`;
    else {
        parse(spec);
        next = spec;
    }
    if (!force && compare(next, current) <= 0)
        throw new Error(`${next} is not newer than ${current} (use --force to allow it)`);
    return next;
}

// { file: version } for every VERSION_FILES entry.
export function readVersions(root) {
    const out = {};
    for (const file of VERSION_FILES) {
        const text = fs.readFileSync(path.join(root, file), "utf8");
        const version = JSON.parse(text).version;
        if (typeof version !== "string" || !VERSION_LINE.test(text))
            throw new Error(`${file} has no version`);
        out[file] = version;
    }
    return out;
}

// The product version: the one version every VERSION_FILES entry carries. Throws if they disagree.
export function productVersion(root) {
    const versions = readVersions(root);
    const version = versions[VERSION_FILES[0]];
    if (Object.values(versions).some(v => v !== version))
        throw new Error(`versions disagree: ${Object.entries(versions).map(([f, v]) => `${f} ${v}`).join(", ")}`);
    return version;
}

// Sets `version` on a lock entry, keeping it straight after "name" as npm writes it.
function setLockVersion(entry, version) {
    if ("version" in entry || !("name" in entry)) {
        entry.version = version;
        return entry;
    }
    const out = {};
    for (const [k, v] of Object.entries(entry)) {
        out[k] = v;
        if (k === "name") out.version = version;
    }
    return out;
}

// Bumps every version file under `root`. Returns { from, to, lock } (lock: whether
// package-lock.json was updated). Checks everything before writing anything.
export function bumpVersion(root, spec, { force = false } = {}) {
    const from = productVersion(root);
    const to = nextVersion(from, spec, { force });

    const writes = [];
    for (const file of VERSION_FILES) {
        const p = path.join(root, file);
        writes.push([p, fs.readFileSync(p, "utf8").replace(VERSION_LINE, `$1${to}$3`)]);
    }

    // npm writes its lock as JSON.stringify(lock, null, 2) plus a newline.
    const lockPath = path.join(root, LOCK_FILE);
    const lock = fs.existsSync(lockPath);
    if (lock) {
        const text = fs.readFileSync(lockPath, "utf8");
        const data = JSON.parse(text);
        if ("version" in data) data.version = to;
        for (const key of LOCK_PACKAGES)
            if (data.packages?.[key]) data.packages[key] = setLockVersion(data.packages[key], to);
        const eol = text.includes("\r\n") ? "\r\n" : "\n";
        writes.push([lockPath, JSON.stringify(data, null, 2).replace(/\n/g, eol) + eol]);
    }

    for (const [p, text] of writes) fs.writeFileSync(p, text);
    return { from, to, lock };
}

const USAGE = `usage: node scripts/bump-version.mjs <x.y.z|patch|minor|major> [--force] [--root <dir>]`;

function main(argv) {
    let spec, force = false;
    let root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === "--help" || a === "-h") {
            console.log(USAGE);
            return 0;
        } else if (a === "--force") force = true;
        else if (a === "--root") {
            if (i + 1 >= argv.length) throw new Error("--root needs a directory");
            root = path.resolve(argv[++i]);
        } else if (a.startsWith("-")) throw new Error(`unknown option ${a}`);
        else if (spec !== undefined) throw new Error(`unexpected argument ${a}`);
        else spec = a;
    }
    if (spec === undefined) throw new Error("no version given");

    const { from, to, lock } = bumpVersion(root, spec, { force });
    const files = [...VERSION_FILES, ...(lock ? [LOCK_FILE] : [])];
    console.log(`Version ${from} -> ${to}`);
    for (const f of files) console.log(`  updated ${f}`);
    console.log(`\nTo commit and tag the release (not done for you):`);
    console.log(`  git add ${files.join(" ")}`);
    console.log(`  git commit -m "Version ${to}"`);
    console.log(`  git tag v${to}`);
    console.log(`  git push && git push origin v${to}`);
    return 0;
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`bump-version: ${e.message}`);
        console.error(USAGE);
        process.exitCode = 1;
    }
}
