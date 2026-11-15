// The self-contained CLI package for the `cli` branch (scripts/build-cli-package.mjs): built into a
// temp dir, then checked from the outside: its files, its package.json files, that every import in
// it resolves inside it (or names a declared dependency or a Node builtin), and that the CLI runs
// with nothing installed. CB_SMOKE=1 also installs it with npm (network) and runs a bake.

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { builtinModules } from "node:module";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { buildCliPackage, importSpecifiers, PACKAGE_NAME } from "../build-cli-package.mjs";
import { productVersion } from "../bump-version.mjs";
import { layoutBlobVersion } from "../../host/src/protocol.js";
import { BLOB_VERSION } from "../../host/src/bake/consts.js";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const RUN_TIMEOUT_MS = 60_000;

let out;
before(() => {
    out = fs.mkdtempSync(path.join(os.tmpdir(), "cb-cli-package-"));
    buildCliPackage(repoRoot, out);
});
after(() => fs.rmSync(out, { recursive: true, force: true }));

function walk(dir, base = dir) {
    const files = [];
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, e.name);
        if (e.isDirectory()) files.push(...walk(p, base));
        else files.push(path.relative(base, p).replaceAll("\\", "/"));
    }
    return files.sort();
}

const readJson = p => JSON.parse(fs.readFileSync(p, "utf8"));

// The package.json that scopes `file` (the nearest one up, inside the package)
function scopeOf(file) {
    for (let dir = path.dirname(file); ; dir = path.dirname(dir)) {
        const p = path.join(dir, "package.json");
        if (fs.existsSync(p)) return p;
        assert.notEqual(dir, out, `${file} has no package.json scope`);
    }
}

const packageName = spec => spec.split("/").slice(0, spec.startsWith("@") ? 2 : 1).join("/");
const inside = p => !path.relative(out, p).startsWith("..") && !path.isAbsolute(path.relative(out, p));

test("importSpecifiers finds static, multi-line, re-export, bare and dynamic imports, not comments", () => {
    const text = [
        `import fs from "node:fs";`,
        `import {`, `    a,`, `    b`, `} from "./ab.js"`,
        `export * from './star.js';`,
        `import "./side.js";`,
        `// import x from "./commented.js";`,
        `const m = await import("../lazy.js");`,
        `export function f() { return "from here"; }`,
    ].join("\n");
    assert.deepEqual(importSpecifiers(text).sort(),
        ["../lazy.js", "./ab.js", "./side.js", "./star.js", "node:fs"]);
});

test("the files: the CLI, the library, the vendored libraries and the root files; no tests", () => {
    const files = walk(out);
    const allowed = [/^(package\.json|README\.md|LICENSE|THIRD-PARTY-NOTICES\.md)$/, /^cli\/[^/]+\.(js|json)$/,
        /^host\/(index|browser)\.js$/, /^host\/src\/.+\.js$/, /^vendor\/(binpack|rle565)\/[^/]+$/];
    for (const f of files) {
        assert.ok(allowed.some(r => r.test(f)), `unexpected file ${f}`);
        assert.ok(!/(^|\/)test(\/|\.)|\.test\./.test(f), `test file ${f}`);
    }
    for (const f of ["package.json", "README.md", "LICENSE", "THIRD-PARTY-NOTICES.md", "cli/cli.js", "cli/args.js",
        "cli/version.js", "cli/package.json", "host/index.js", "host/browser.js", "host/src/ControlBlock.js",
        "host/src/images.js", "host/src/bake/bake.js", "vendor/binpack/index.js", "vendor/binpack/LICENSE",
        "vendor/rle565/pack.js", "vendor/rle565/LICENSE"])
        assert.ok(files.includes(f), `missing ${f}`);
    // dev-only files that reach outside the package
    assert.ok(!files.includes("host/src/bake/binpack.js"), "the #binpack shim escapes the package");
    assert.ok(!files.includes("host/src/bake/headers.js"), "headers.js reads firmware/");
    assert.ok(!files.some(f => f.startsWith("vendor/") && /cli\.js$/.test(f)), "a vendored library's CLI");
});

test("the package.json: name, version, bin, exports; no scripts, workspaces or devDependencies", () => {
    const pkg = readJson(path.join(out, "package.json"));
    assert.equal(pkg.name, PACKAGE_NAME);
    assert.equal(pkg.name, "@toptensoftware/controlblock");
    assert.equal(pkg.version, productVersion(repoRoot));
    assert.equal(pkg.type, "module");
    assert.equal(pkg.private, undefined);
    assert.deepEqual(pkg.bin, { controlblock: "cli/cli.js" });
    assert.equal(pkg.engines.node, readJson(path.join(repoRoot, "host/package.json")).engines.node);
    assert.equal(pkg.license, "MIT");
    assert.match(pkg.repository.url, /github\.com\/toptensoftware\/ControlBlock/);
    assert.deepEqual(pkg.exports, {
        ".": { browser: "./host/browser.js", default: "./host/index.js" },
        "./package.json": "./package.json",
    });
    for (const target of [pkg.exports["."].browser, pkg.exports["."].default, pkg.bin.controlblock])
        assert.ok(fs.existsSync(path.join(out, target)), target);
    // anything that would make npm "prepare" a git dependency, or that has no place in it
    for (const f of walk(out).filter(f => f.endsWith("package.json"))) {
        const p = readJson(path.join(out, f));
        for (const key of ["scripts", "workspaces", "devDependencies"])
            assert.equal(p[key], undefined, `${f} has ${key}`);
        assert.equal(p.version ?? pkg.version, pkg.version, `${f} version`);
    }
});

test("dependencies: exactly the packages imported, button-face pinned to the locked sha over https", () => {
    const pkg = readJson(path.join(out, "package.json"));
    const host = readJson(path.join(repoRoot, "host/package.json")).dependencies;
    assert.deepEqual(Object.keys(pkg.dependencies).sort(), ["@toptensoftware/button-face", "sharp", "usb", "wasmoon"]);
    for (const name of ["sharp", "usb", "wasmoon"]) assert.equal(pkg.dependencies[name], host[name]);

    const lock = readJson(path.join(repoRoot, "package-lock.json"));
    const sha = /#([0-9a-f]{40})$/.exec(lock.packages["node_modules/@toptensoftware/button-face"].resolved)[1];
    assert.equal(pkg.dependencies["@toptensoftware/button-face"],
        `git+https://github.com/toptensoftware/button-face.git#${sha}`);
});

test("every import resolves inside the package, to a dependency or to a Node builtin", () => {
    const pkg = readJson(path.join(out, "package.json"));
    const used = new Set();
    let checked = 0;
    for (const f of walk(out).filter(f => /\.(m?js)$/.test(f))) {
        const file = path.join(out, f);
        for (const spec of importSpecifiers(fs.readFileSync(file, "utf8"))) {
            checked++;
            let target;
            if (spec.startsWith(".")) target = path.resolve(path.dirname(file), spec);
            else if (spec.startsWith("#")) {
                const scope = scopeOf(file);
                const map = readJson(scope).imports ?? {};
                assert.ok(spec in map, `${f}: ${spec} isn't in ${path.relative(out, scope)}'s imports`);
                assert.match(map[spec], /^\.\//, `${spec} must map to a file`);
                target = path.resolve(path.dirname(scope), map[spec]);
            } else if (spec.startsWith("node:") || builtinModules.includes(spec)) continue;
            else {
                const name = packageName(spec);
                assert.ok(name in pkg.dependencies, `${f}: ${spec} isn't a dependency`);
                used.add(name);
                continue;
            }
            assert.ok(inside(target), `${f}: ${spec} leaves the package`);
            assert.ok(fs.statSync(target, { throwIfNoEntry: false })?.isFile(), `${f}: ${spec} doesn't exist`);
        }
    }
    assert.ok(checked > 50, `only ${checked} imports found`);
    assert.deepEqual([...used].sort(), Object.keys(pkg.dependencies).sort(), "a dependency nothing imports");
});

test("node cli/cli.js --version runs with nothing installed", () => {
    assert.ok(!fs.existsSync(path.join(out, "node_modules")));
    const r = spawnSync(process.execPath, ["cli/cli.js", "--version"], { cwd: out, encoding: "utf8", timeout: RUN_TIMEOUT_MS });
    assert.equal(r.status, 0, r.stderr);
    assert.equal(r.stdout.trim(), productVersion(repoRoot));
});

test("building into a non-empty directory is refused; --clean keeps only .git", () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cb-cli-clean-"));
    try {
        fs.mkdirSync(path.join(dir, ".git"));
        fs.writeFileSync(path.join(dir, ".git", "HEAD"), "ref: refs/heads/cli\n");
        buildCliPackage(repoRoot, dir);     // .git alone is fine
        fs.writeFileSync(path.join(dir, "stale.js"), "");
        assert.throws(() => buildCliPackage(repoRoot, dir), /not empty/);
        buildCliPackage(repoRoot, dir, { clean: true });
        assert.ok(!fs.existsSync(path.join(dir, "stale.js")));
        assert.ok(fs.existsSync(path.join(dir, ".git", "HEAD")));
        assert.deepEqual(walk(dir).filter(f => !f.startsWith(".git/")), walk(out));
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

// Needs the network (npm registry, GitHub for button-face) and git: CI runs it.
test("smoke: npm installs the packed package; npx controlblock --version and bake", { skip: process.env.CB_SMOKE !== "1" && "set CB_SMOKE=1" }, () => {
    const work = fs.mkdtempSync(path.join(os.tmpdir(), "cb-cli-smoke-"));
    // npm and npx are .cmd scripts on Windows: run through the shell, as one quoted command line.
    // stdin closed (nothing may prompt); stderr streamed so a CI log shows how far it got.
    const run = (cmd, args) => {
        const line = [cmd, ...args.map(a => `"${a}"`)].join(" ");
        console.error(`# smoke: ${line}`);
        const r = spawnSync(line, { cwd: work, encoding: "utf8", shell: true, timeout: 600_000,
            stdio: ["ignore", "pipe", "inherit"] });
        assert.equal(r.status, 0, `${line} (status ${r.status}, signal ${r.signal})\n${r.stdout}`);
        return r.stdout;
    };
    try {
        run("npm", ["pack", out, "--pack-destination", work]);
        const tarball = fs.readdirSync(work).find(f => f.endsWith(".tgz"));
        fs.writeFileSync(path.join(work, "package.json"), JSON.stringify({ name: "cb-smoke", private: true }));
        run("npm", ["install", "--no-audit", "--no-fund", `./${tarball}`]);
        assert.equal(run("npx", ["controlblock", "--version"]).trim(), productVersion(repoRoot));
        const layout = path.join(repoRoot, "firmware/test/data/sample_layout.json");
        run("npx", ["controlblock", "bake", layout, "-o", path.join(work, "out.bin")]);
        const blob = fs.readFileSync(path.join(work, "out.bin"));
        // Labels are drawn with the system's fonts, so only a Windows bake matches the sample (baked on Windows)
        if (process.platform === "win32")
            assert.deepEqual(blob, fs.readFileSync(path.join(repoRoot, "firmware/test/data/sample_layout.bin")));
        else
            assert.equal(layoutBlobVersion(blob), BLOB_VERSION);
    } finally {
        fs.rmSync(work, { recursive: true, force: true });
    }
});
