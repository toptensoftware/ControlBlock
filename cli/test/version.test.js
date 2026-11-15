// The CLI's own version and the firmware version check (cli/version.js: pure but for reading
// the CLI's package.json).
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import { cliVersion, parseVersion, firmwareVersionNote } from "../version.js";

const readVersion = rel => JSON.parse(fs.readFileSync(new URL(rel, import.meta.url), "utf8")).version;

test("version: the CLI's version is its package.json's, which is the product version", () => {
    assert.equal(cliVersion(), readVersion("../package.json"));
    assert.equal(cliVersion(), readVersion("../../package.json"));
});

test("version: parseVersion reads major.minor.patch, with or without a pre-release or build suffix", () => {
    assert.deepEqual(parseVersion("0.1.0"), { major: 0, minor: 1, patch: 0 });
    assert.deepEqual(parseVersion("1.12.3-dev+75ac767.dirty"), { major: 1, minor: 12, patch: 3 });
    assert.deepEqual(parseVersion("0.2.0-dev"), { major: 0, minor: 2, patch: 0 });
    assert.deepEqual(parseVersion("v2.0.1"), { major: 2, minor: 0, patch: 1 });
    assert.deepEqual(parseVersion("1.0"), { major: 1, minor: 0, patch: 0 });
    for (const bad of ["75ac767", "75ac767-dirty", "1", "", "1.x.0", "a1.0.0", undefined])
        assert.equal(parseVersion(bad), null, `${bad}`);
});

test("version: firmwareVersionNote warns when the firmware's major.minor is older than the CLI's", () => {
    // same or newer major.minor (patch and suffixes don't matter): nothing to say
    for (const fw of ["0.1.0", "0.1.5", "0.1.0-dev+75ac767.dirty", "0.2.0", "1.0.0"])
        assert.equal(firmwareVersionNote(fw, "0.1.3"), null, fw);
    // older
    for (const fw of ["0.0.9", "0.0.1-dev+abc1234"])
        assert.match(firmwareVersionNote(fw, "0.1.0"), /^warning: the device's firmware \S+ is older than this CLI \(0\.1\.0\): update the firmware/);
    assert.match(firmwareVersionNote("1.9.0", "2.0.0"), /^warning: /);
    // not a version (firmware from before versioning gives a git hash): can't tell
    assert.match(firmwareVersionNote("75ac767-dirty", "0.1.0"), /^note: the device's firmware version "75ac767-dirty" isn't a release version: can't compare it with this CLI's \(0\.1\.0\)/);
});
