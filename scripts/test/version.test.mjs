// The product version: root, host/ and cli/ package.json agree, and
// scripts/bump-version.mjs moves all of them (plus the root lock) together.

import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { bumpVersion, nextVersion, readVersions, VERSION_FILES } from "../bump-version.mjs";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const script = path.join(repoRoot, "scripts/bump-version.mjs");

function readJson(root, file) {
    return JSON.parse(fs.readFileSync(path.join(root, file), "utf8"));
}

// A temp repo root holding copies of the version files (and the lock).
function tempRepo(t, { lock = true } = {}) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cb-version-"));
    t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
    const files = [...VERSION_FILES, ...(lock ? ["package-lock.json"] : [])];
    for (const f of files) {
        fs.mkdirSync(path.dirname(path.join(dir, f)), { recursive: true });
        fs.copyFileSync(path.join(repoRoot, f), path.join(dir, f));
    }
    return dir;
}

function setVersion(root, file, version) {
    const p = path.join(root, file);
    fs.writeFileSync(p, fs.readFileSync(p, "utf8").replace(/^  "version": "[^"]*"/m, `  "version": "${version}"`));
}

test("root, host and cli package.json versions agree", () => {
    const versions = readVersions(repoRoot);
    assert.deepEqual(Object.keys(versions).sort(), [...VERSION_FILES].sort());
    const root = versions["package.json"];
    assert.match(root, /^\d+\.\d+\.\d+$/);
    for (const [file, v] of Object.entries(versions))
        assert.equal(v, root, `${file} is ${v}, root package.json is ${root}`);
});

test("the root lock records the same version", () => {
    const lock = readJson(repoRoot, "package-lock.json");
    const root = readJson(repoRoot, "package.json").version;
    assert.equal(lock.version, root);
    for (const key of ["", "host", "cli"])
        assert.equal(lock.packages[key]?.version, root, `package-lock.json packages["${key}"]`);
});

test("nextVersion: patch, minor, major and explicit", () => {
    assert.equal(nextVersion("0.1.0", "patch"), "0.1.1");
    assert.equal(nextVersion("0.1.7", "minor"), "0.2.0");
    assert.equal(nextVersion("0.9.3", "major"), "1.0.0");
    assert.equal(nextVersion("0.1.0", "0.3.0"), "0.3.0");
    assert.equal(nextVersion("0.1.0", "10.20.30"), "10.20.30");
});

test("nextVersion rejects non-semver input", () => {
    for (const bad of ["", "1", "1.2", "v1.2.3", "1.2.3.4", "1.2.x", "01.2.3", "1.2.3-beta", "bump"])
        assert.throws(() => nextVersion("0.1.0", bad), /not a version/i, bad);
});

test("nextVersion refuses to go backwards or stay put, unless forced", () => {
    assert.throws(() => nextVersion("0.2.0", "0.1.9"), /not newer/i);
    assert.throws(() => nextVersion("1.0.0", "0.9.9"), /not newer/i);
    assert.throws(() => nextVersion("0.2.0", "0.2.0"), /not newer/i);
    assert.equal(nextVersion("0.2.0", "0.1.9", { force: true }), "0.1.9");
    // Numeric, not string, comparison.
    assert.equal(nextVersion("0.9.0", "0.10.0"), "0.10.0");
});

test("bumpVersion updates every file and keeps their formatting", (t) => {
    const dir = tempRepo(t);
    const before = Object.fromEntries(
        [...VERSION_FILES, "package-lock.json"].map(f => [f, fs.readFileSync(path.join(dir, f), "utf8")]));
    const old = readJson(dir, "package.json").version;

    // A version no dependency in the lock has, so the round trip below is exact.
    const expected = "97.98.99";
    const result = bumpVersion(dir, expected);
    assert.equal(result.from, old);
    assert.equal(result.to, expected);

    for (const f of [...VERSION_FILES, "package-lock.json"]) {
        const after = fs.readFileSync(path.join(dir, f), "utf8");
        // Only the version strings change: the rest of each file is byte for byte the same.
        assert.equal(after.split(`"version": "${expected}"`).join(`"version": "${old}"`), before[f], f);
        assert.ok(after.endsWith("}\n"), `${f} keeps its trailing newline`);
    }
    for (const v of Object.values(readVersions(dir))) assert.equal(v, expected);
    const lock = readJson(dir, "package-lock.json");
    assert.equal(lock.version, expected);
    for (const key of ["", "host", "cli"]) assert.equal(lock.packages[key].version, expected);
    // Lock entries for other packages are untouched.
    assert.equal(lock.packages["node_modules/sharp"].version, readJson(repoRoot, "package-lock.json").packages["node_modules/sharp"].version);
});

test("bumpVersion works without a lock", (t) => {
    const dir = tempRepo(t, { lock: false });
    const result = bumpVersion(dir, "4.5.6");
    assert.equal(result.to, "4.5.6");
    assert.equal(result.lock, false);
    for (const v of Object.values(readVersions(dir))) assert.equal(v, "4.5.6");
    assert.ok(!fs.existsSync(path.join(dir, "package-lock.json")));
});

test("bumpVersion refuses when the files disagree, and writes nothing", (t) => {
    const dir = tempRepo(t);
    setVersion(dir, "cli/package.json", "0.0.9");
    const before = fs.readFileSync(path.join(dir, "host/package.json"), "utf8");
    assert.throws(() => bumpVersion(dir, "patch"), /disagree/i);
    assert.equal(fs.readFileSync(path.join(dir, "host/package.json"), "utf8"), before);
});

test("bumpVersion refuses a package.json with no version", (t) => {
    const dir = tempRepo(t);
    const p = path.join(dir, "cli/package.json");
    fs.writeFileSync(p, fs.readFileSync(p, "utf8").replace(/^  "version": "[^"]*",\n/m, ""));
    assert.throws(() => bumpVersion(dir, "patch"), /no version/i);
});

test("CLI: bumps a given root and prints the git commands, never runs them", (t) => {
    const dir = tempRepo(t);
    const old = readJson(dir, "package.json").version;
    const to = nextVersion(old, "patch");
    const r = spawnSync(process.execPath, [script, "patch", "--root", dir], { encoding: "utf8" });
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, new RegExp(`${old.replace(/\./g, "\\.")} -> ${to.replace(/\./g, "\\.")}`));
    assert.ok(r.stdout.includes(`git commit -m "Version ${to}"`), r.stdout);
    assert.ok(r.stdout.includes(`git tag v${to}`), r.stdout);
    assert.equal(readJson(dir, "package.json").version, to);
    assert.ok(!fs.existsSync(path.join(dir, ".git")));
});

test("CLI: bad input and usage errors exit non-zero", (t) => {
    const dir = tempRepo(t);
    const before = fs.readFileSync(path.join(dir, "package.json"), "utf8");
    for (const args of [[], ["1.2"], ["0.0.1"], ["patch", "minor"], ["patch", "--root"], ["patch", "--bogus"]]) {
        const r = spawnSync(process.execPath, [script, ...args, ...(args.includes("--root") ? [] : ["--root", dir])], { encoding: "utf8" });
        assert.notEqual(r.status, 0, args.join(" "));
        assert.ok(r.stderr.length > 0, args.join(" "));
    }
    assert.equal(fs.readFileSync(path.join(dir, "package.json"), "utf8"), before);
    const r = spawnSync(process.execPath, [script, "--help"], { encoding: "utf8" });
    assert.equal(r.status, 0);
    assert.match(r.stdout, /usage/i);
});

test("CLI: --force allows going backwards", (t) => {
    const dir = tempRepo(t);
    setVersion(dir, "package.json", "0.5.0");
    setVersion(dir, "host/package.json", "0.5.0");
    setVersion(dir, "cli/package.json", "0.5.0");
    const r = spawnSync(process.execPath, [script, "0.4.0", "--force", "--root", dir], { encoding: "utf8" });
    assert.equal(r.status, 0, r.stderr);
    assert.equal(readJson(dir, "cli/package.json").version, "0.4.0");
});
