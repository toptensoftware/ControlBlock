#!/usr/bin/env node
// Builds the self-contained CLI package for the `cli` branch, which users run with
// `npx github:toptensoftware/ControlBlock#cli` (wip/wip-versioning-deployment.md, step 6).
//
//   node scripts/build-cli-package.mjs <outDir> [--clean]
//
// The package is @toptensoftware/controlblock at the product version: the CLI (bin `controlblock`)
// and the library (`exports`, as host/package.json). It holds only the files the CLI and the library
// import (followed from their entry points, so tests and dev-only files stay out), the libraries
// from submodules copied under vendor/ (the host's `#binpack`/`#rle565` imports point there), and
// dependencies for the npm packages those files import. Its package.json files have no scripts or
// workspaces: either would make npm "prepare" the git dependency (npm install inside it).
//
// <outDir> must be empty but for a .git folder; --clean empties it first (keeping .git).

import fs from "node:fs";
import path from "node:path";
import { builtinModules } from "node:module";
import { fileURLToPath } from "node:url";
import { productVersion } from "./bump-version.mjs";

export const PACKAGE_NAME = "@toptensoftware/controlblock";
const REPOSITORY = "https://github.com/toptensoftware/ControlBlock";

// Where the files the package is built from start (repo-relative)
const ENTRIES = ["cli/cli.js", "host/index.js", "host/browser.js"];
// Shipped source folders: the closure of ENTRIES must stay inside these
const SOURCE_DIRS = ["cli", "host"];
// The host's `#` imports that name a library from a submodule: copied to vendor/<name>/ (the entry
// file and whatever it imports, plus the license) and the import pointed at the copy
const VENDOR = {
    "#binpack": { name: "binpack", dir: "binpack", entry: "index.js", license: "binpack/LICENSE" },
    "#rle565": { name: "rle565", dir: "rle565/encoder", entry: "pack.js", license: "rle565/LICENSE" },
};
// Copied to the package root as they are
const ROOT_FILES = ["LICENSE", "THIRD-PARTY-NOTICES.md"];

// Every module specifier in a JS source: static imports/exports (over several lines too), and
// import("...") with a string literal
export function importSpecifiers(text)
{
    const out = [];
    for (const m of text.matchAll(/^\s*(?:import|export)\s[^;]*?\sfrom\s*["']([^"']+)["']/gm)) out.push(m[1]);
    for (const m of text.matchAll(/^\s*import\s*["']([^"']+)["']/gm)) out.push(m[1]);
    for (const m of text.matchAll(/\bimport\(\s*["']([^"']+)["']\s*\)/g)) out.push(m[1]);
    return out;
}

const isBuiltin = spec => spec.startsWith("node:") || builtinModules.includes(spec);
const packageName = spec => spec.split("/").slice(0, spec.startsWith("@") ? 2 : 1).join("/");
const readJson = file => JSON.parse(fs.readFileSync(file, "utf8"));
const slash = p => p.replaceAll("\\", "/");

// The files reachable from `entries` (absolute) by relative imports. Returns { files, hashes, packages }:
// absolute file paths, the `#` specifiers and the npm package names they import.
function closure(entries)
{
    const files = new Set(), hashes = new Set(), packages = new Set();
    const queue = [...entries];
    while (queue.length) {
        const file = queue.pop();
        if (files.has(file)) continue;
        if (!fs.statSync(file, { throwIfNoEntry: false })?.isFile()) throw new Error(`missing ${file}`);
        files.add(file);
        for (const spec of importSpecifiers(fs.readFileSync(file, "utf8"))) {
            if (spec.startsWith(".")) queue.push(path.resolve(path.dirname(file), spec));
            else if (spec.startsWith("#")) hashes.add(spec);
            else if (!isBuiltin(spec)) packages.add(packageName(spec));
        }
    }
    return { files, hashes, packages };
}

// A git dependency spec pinned to the commit the root lock resolved it to, over https (npm records
// GitHub git deps as git+ssh, which needs keys)
function pinnedGitSpec(root, name)
{
    const resolved = readJson(path.join(root, "package-lock.json")).packages?.[`node_modules/${name}`]?.resolved;
    const m = /^git\+(?:ssh:\/\/git@|https:\/\/)github\.com\/(.+?)#([0-9a-f]{40})$/.exec(resolved ?? "");
    if (!m) throw new Error(`package-lock.json has no GitHub commit for ${name} (${resolved})`);
    return `git+https://github.com/${m[1]}#${m[2]}`;
}

// The range for each package: the host's, else a vendored library's; git specs pinned
function dependencies(root, names, vendorPkgs)
{
    const host = readJson(path.join(root, "host/package.json")).dependencies ?? {};
    const out = {};
    for (const name of [...names].sort()) {
        const range = host[name] ?? vendorPkgs.map(p => p.dependencies?.[name]).find(Boolean);
        if (!range) throw new Error(`${name} is imported but no package.json lists it`);
        out[name] = /^(git\+|github:)/.test(range) ? pinnedGitSpec(root, name) : range;
    }
    return out;
}

// host/package.json's exports with the targets moved under host/ (package.json is the package's own)
function moveExports(value)
{
    if (typeof value === "string")
        return value === "./package.json" ? value : `./host/${value.replace(/^\.\//, "")}`;
    return Object.fromEntries(Object.entries(value).map(([k, v]) => [k, moveExports(v)]));
}

const README = (version) => `# ControlBlock CLI and host library

The command line and Node/browser library for [ControlBlock](${REPOSITORY}), a touch control
surface on a Waveshare ESP32-S3-Touch-LCD-4.3. Version ${version}.

Built from the ControlBlock repository for the \`cli\` branch: don't edit it here. Needs Node 24 and git.

    npx github:toptensoftware/ControlBlock#cli            (lists the commands)
    npx github:toptensoftware/ControlBlock#cli bake my-layout.mjs -o my-layout.bin

The commands, the layout format and the library's API are documented in the repository:
[cli/README.md](${REPOSITORY}/blob/main/cli/README.md), [docs/](${REPOSITORY}/tree/main/docs).

MIT licensed (LICENSE); third-party components in THIRD-PARTY-NOTICES.md.
`;

function prepareOutDir(outDir, clean)
{
    fs.mkdirSync(outDir, { recursive: true });
    const entries = fs.readdirSync(outDir).filter(e => e !== ".git");
    if (entries.length && !clean)
        throw new Error(`${outDir} is not empty (${entries.slice(0, 3).join(", ")}...): use --clean to empty it`);
    for (const e of entries) fs.rmSync(path.join(outDir, e), { recursive: true, force: true });
}

function copy(src, dest)
{
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.copyFileSync(src, dest);
}

const writeJson = (file, data) => fs.writeFileSync(file, JSON.stringify(data, null, 2) + "\n");

// Builds the package from the repo at `root` into `outDir`. Returns { version, files } (package paths).
export function buildCliPackage(root, outDir, { clean = false } = {})
{
    root = path.resolve(root);
    outDir = path.resolve(outDir);
    const version = productVersion(root);
    const hostPkg = readJson(path.join(root, "host/package.json"));
    const cliPkg = readJson(path.join(root, "cli/package.json"));

    // The CLI and the library
    const src = closure(ENTRIES.map(e => path.join(root, e)));
    const copies = [];      // [source, package path]
    for (const file of src.files) {
        const rel = slash(path.relative(root, file));
        if (!SOURCE_DIRS.some(d => rel.startsWith(`${d}/`)))
            throw new Error(`${rel} is outside ${SOURCE_DIRS.join(", ")}: the package can't ship it`);
        copies.push([file, rel]);
    }

    // The host's `#` imports: vendored libraries
    const imports = {}, packages = new Set(src.packages), vendorPkgs = [];
    for (const spec of [...src.hashes].sort()) {
        const v = VENDOR[spec];
        if (!v) throw new Error(`${spec} isn't a vendored library (VENDOR in build-cli-package.mjs)`);
        if (!(spec in (hostPkg.imports ?? {}))) throw new Error(`${spec} isn't in host/package.json's imports`);
        const dir = path.join(root, v.dir);
        const lib = closure([path.join(dir, v.entry)]);
        if (lib.hashes.size) throw new Error(`${v.name} uses # imports`);
        for (const file of lib.files) {
            const rel = slash(path.relative(dir, file));
            if (rel.startsWith("..")) throw new Error(`${v.name} imports ${file}, outside ${v.dir}`);
            copies.push([file, `vendor/${v.name}/${rel}`]);
        }
        copies.push([path.join(root, v.license), `vendor/${v.name}/LICENSE`]);
        imports[spec] = `./vendor/${v.name}/${v.entry}`;
        lib.packages.forEach(p => packages.add(p));
        vendorPkgs.push(readJson(path.join(dir, "package.json")));
    }
    for (const f of ROOT_FILES) copies.push([path.join(root, f), f]);

    // package.json: the host's library fields, moved under host/, plus the CLI's bin. There's no
    // host/package.json in the package, so this one scopes host/ (type, imports).
    const pkg = {
        name: PACKAGE_NAME,
        version,
        description: "ControlBlock command line and host library: bake and upload layouts, set up the device, drive it over USB",
        license: hostPkg.license,
        author: hostPkg.author,
        repository: { type: "git", url: `git+${REPOSITORY}.git` },
        homepage: REPOSITORY,
        type: "module",
        bin: { controlblock: `cli/${cliPkg.bin.controlblock.replace(/^\.\//, "")}` },
        main: `host/${hostPkg.main}`,
        exports: moveExports(hostPkg.exports),
        imports,
        engines: hostPkg.engines,
        dependencies: dependencies(root, packages, vendorPkgs),
    };
    // cli/package.json scopes cli/ (cli/version.js reads its version): the source one, minus what a
    // packaged copy mustn't have
    const { scripts, devDependencies, workspaces, ...cliOut } = cliPkg;
    cliOut.version = version;

    prepareOutDir(outDir, clean);
    for (const [from, to] of copies) copy(from, path.join(outDir, to));
    writeJson(path.join(outDir, "package.json"), pkg);
    writeJson(path.join(outDir, "cli/package.json"), cliOut);
    fs.writeFileSync(path.join(outDir, "README.md"), README(version));
    const files = [...copies.map(c => c[1]), "package.json", "cli/package.json", "README.md"].sort();
    return { version, files };
}

const USAGE = "usage: node scripts/build-cli-package.mjs <outDir> [--clean]";

function main(argv)
{
    let outDir, clean = false;
    for (const a of argv) {
        if (a === "--help" || a === "-h") {
            console.log(USAGE);
            return 0;
        } else if (a === "--clean") clean = true;
        else if (a.startsWith("-")) throw new Error(`unknown option ${a}`);
        else if (outDir !== undefined) throw new Error(`unexpected argument ${a}`);
        else outDir = a;
    }
    if (outDir === undefined) throw new Error("no output directory given");
    const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
    const { version, files } = buildCliPackage(root, outDir, { clean });
    console.log(`${PACKAGE_NAME} ${version}: ${files.length} files in ${path.resolve(outDir)}`);
    return 0;
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`build-cli-package: ${e.message}`);
        console.error(USAGE);
        process.exitCode = 1;
    }
}
