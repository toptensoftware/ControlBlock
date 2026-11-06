// The command line, parsed (cli/args.js: pure).
import { test } from "node:test";
import assert from "node:assert/strict";
import { parseArgs, UsageError, WIFI_WAIT_S } from "../args.js";

const usage = (argv, pattern) => assert.throws(() => parseArgs(argv), err => {
    assert.ok(err instanceof UsageError, `${err}`);
    assert.match(err.message, pattern);
    assert.match(err.message, /usage/);
    return true;
});

test("args: bake only", () => {
    assert.deepEqual(parseArgs(["bake", "config.json"]), { command: "bake", input: "config.json", out: undefined, upload: false, serial: undefined, force: false });
    const a = parseArgs(["bake", "config.json", "-o", "out.bin"]);
    assert.equal(a.out, "out.bin");
    assert.equal(parseArgs(["bake", "config.mjs", "--out", "x.bin"]).out, "x.bin");
    assert.equal(a.upload, false);
});

test("args: bake --upload, --serial and --force", () => {
    const a = parseArgs(["bake", "config.json", "--upload", "--serial", "ABC123", "--force"]);
    assert.equal(a.upload, true);
    assert.equal(a.serial, "ABC123");
    assert.equal(a.force, true);
});

test("args: upload takes a config or a baked .bin", () => {
    assert.deepEqual(parseArgs(["upload", "out.bin"]), { command: "upload", input: "out.bin", binary: true, serial: undefined, force: false });
    assert.deepEqual(parseArgs(["upload", "cfg.mjs", "--serial", "S1", "--force"]), { command: "upload", input: "cfg.mjs", binary: false, serial: "S1", force: true });
    assert.equal(parseArgs(["upload", "OUT.BIN"]).binary, true);
    usage(["upload", "out.bin", "-o", "x.bin"], /unknown option -o/);     // nothing to write
    usage(["bake", "out.bin"], /already baked/i);                           // and nothing to bake
});

test("args: mistakes are reported with the usage", () => {
    usage(["bake", "a.json", "b.json"], /one config/);
    usage(["bake"], /one config/);
    usage(["bake", "a.json", "--nope"], /unknown option --nope/);
    usage(["bake", "a.json", "--serial"], /needs a value/);
    usage(["bake", "a.json", "-o"], /needs a value/);
    usage(["bake", "a.json", "--serial", "X"], /--upload/);    // --serial without --upload does nothing
    usage(["bake", "a.json", "--force"], /--upload/);
    usage(["bake", "a.json", "--header", "t.h"], /unknown option --header/);   // dropped: tools/codegen writes the header
    usage(["upload"], /one config/);
});

test("args: no command, or an unknown one", () => {
    usage([], /usage/);
    usage(["frobnicate"], /unknown command frobnicate/);
    usage(["--serial", "X"], /unknown command --serial/);
});

test("args: -- ends the options", () => {
    assert.equal(parseArgs(["bake", "--", "-odd.json"]).input, "-odd.json");
});

test("args: wifi set, get, clear, status and wait", () => {
    assert.deepEqual(parseArgs(["wifi", "set", "Home", "secret"]), { command: "wifi", action: "set", serial: undefined, ssid: "Home", password: "secret", seconds: WIFI_WAIT_S });
    assert.equal(parseArgs(["wifi", "set", "Cafe"]).password, "");            // an open network
    assert.equal(parseArgs(["wifi", "set", "Home", ""]).password, "");
    assert.deepEqual(parseArgs(["wifi", "get", "--serial", "S1"]), { command: "wifi", action: "get", serial: "S1" });
    assert.deepEqual(parseArgs(["wifi", "--serial", "S1", "clear"]), { command: "wifi", action: "clear", serial: "S1" });
    assert.deepEqual(parseArgs(["wifi", "status"]), { command: "wifi", action: "status", serial: undefined });
    assert.deepEqual(parseArgs(["wifi", "wait"]), { command: "wifi", action: "wait", serial: undefined, seconds: WIFI_WAIT_S });
    assert.equal(parseArgs(["wifi", "wait", "2.5"]).seconds, 2.5);
    assert.equal(parseArgs(["wifi", "set", "Home", "--", "-starts-with-a-dash"]).password, "-starts-with-a-dash");
});

test("args: wifi mistakes are reported with the usage", () => {
    usage(["wifi"], /wifi needs/);
    usage(["wifi", "connect"], /unknown wifi command connect/);
    usage(["wifi", "set"], /ssid/);
    usage(["wifi", "set", "a", "b", "c"], /ssid/);
    usage(["wifi", "get", "extra"], /takes no arguments/);
    usage(["wifi", "status", "--force"], /unknown option --force/);
    usage(["wifi", "wait", "soon"], /seconds/);
    usage(["wifi", "wait", "0"], /seconds/);
    usage(["wifi", "wait", "1", "2"], /seconds/);
});
