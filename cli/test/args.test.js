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
    assert.deepEqual(parseArgs(["bake", "layout.json"]), { command: "bake", input: "layout.json", out: undefined, upload: false, serial: undefined, force: false });
    const a = parseArgs(["bake", "layout.json", "-o", "out.bin"]);
    assert.equal(a.out, "out.bin");
    assert.equal(parseArgs(["bake", "layout.mjs", "--out", "x.bin"]).out, "x.bin");
    assert.equal(a.upload, false);
});

test("args: bake --upload, --serial and --force", () => {
    const a = parseArgs(["bake", "layout.json", "--upload", "--serial", "ABC123", "--force"]);
    assert.equal(a.upload, true);
    assert.equal(a.serial, "ABC123");
    assert.equal(a.force, true);
});

test("args: upload takes a layout or a baked .bin", () => {
    assert.deepEqual(parseArgs(["upload", "out.bin"]), { command: "upload", input: "out.bin", binary: true, serial: undefined, force: false });
    assert.deepEqual(parseArgs(["upload", "cfg.mjs", "--serial", "S1", "--force"]), { command: "upload", input: "cfg.mjs", binary: false, serial: "S1", force: true });
    assert.equal(parseArgs(["upload", "OUT.BIN"]).binary, true);
    usage(["upload", "out.bin", "-o", "x.bin"], /unknown option -o/);     // nothing to write
    usage(["bake", "out.bin"], /already baked/i);                           // and nothing to bake
});

test("args: mistakes are reported with the usage", () => {
    usage(["bake", "a.json", "b.json"], /one layout/);
    usage(["bake"], /one layout/);
    usage(["bake", "a.json", "--nope"], /unknown option --nope/);
    usage(["bake", "a.json", "--serial"], /needs a value/);
    usage(["bake", "a.json", "-o"], /needs a value/);
    usage(["bake", "a.json", "--serial", "X"], /--upload/);    // --serial without --upload does nothing
    usage(["bake", "a.json", "--force"], /--upload/);
    usage(["bake", "a.json", "--header", "t.h"], /unknown option --header/);   // dropped: tools/codegen writes the header
    usage(["upload"], /one layout/);
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

test("args: time set (the default), get, zone and server", () => {
    assert.deepEqual(parseArgs(["time"]), { command: "time", action: "set", serial: undefined });
    assert.deepEqual(parseArgs(["time", "set", "--serial", "S1"]), { command: "time", action: "set", serial: "S1" });
    assert.deepEqual(parseArgs(["time", "--serial", "S1"]), { command: "time", action: "set", serial: "S1" });
    assert.deepEqual(parseArgs(["time", "get"]), { command: "time", action: "get", serial: undefined });
    assert.deepEqual(parseArgs(["time", "zone", "AEST-10AEDT,M10.1.0,M4.1.0/3"]), { command: "time", action: "zone", serial: undefined, value: "AEST-10AEDT,M10.1.0,M4.1.0/3" });
    assert.deepEqual(parseArgs(["time", "zone", "--default"]), { command: "time", action: "zone", serial: undefined, value: "" });
    assert.deepEqual(parseArgs(["time", "server", "time.example.com", "--serial", "S2"]), { command: "time", action: "server", serial: "S2", value: "time.example.com" });
    assert.equal(parseArgs(["time", "--default", "server"]).value, "");
});

test("args: time mistakes are reported with the usage", () => {
    usage(["time", "sync"], /unknown time command sync/);
    usage(["time", "get", "now"], /takes no arguments/);
    usage(["time", "set", "12:00"], /takes no arguments/);
    usage(["time", "zone"], /time zone needs/);
    usage(["time", "zone", "UTC0", "EST5"], /time zone needs/);
    usage(["time", "zone", "UTC0", "--default"], /time zone needs/);
    usage(["time", "server"], /time server needs/);
    usage(["time", "get", "--default"], /--default/);
    usage(["time", "--default"], /--default/);
    usage(["time", "zone", "UTC0", "--force"], /unknown option --force/);
});

test("args: name get (the default) and set", () => {
    assert.deepEqual(parseArgs(["name"]), { command: "name", action: "get", serial: undefined });
    assert.deepEqual(parseArgs(["name", "get", "--serial", "S1"]), { command: "name", action: "get", serial: "S1" });
    assert.deepEqual(parseArgs(["name", "set", "den-tv"]), { command: "name", action: "set", serial: undefined, value: "den-tv" });
    assert.deepEqual(parseArgs(["name", "set", "--default"]), { command: "name", action: "set", serial: undefined, value: "" });
});

test("args: name mistakes are reported with the usage", () => {
    usage(["name", "rename"], /unknown name command rename/);
    usage(["name", "get", "x"], /takes no arguments/);
    usage(["name", "set"], /name set needs/);
    usage(["name", "set", "a", "b"], /name set needs/);
    usage(["name", "get", "--default"], /--default/);
});

test("args: render, with its options and the default output folder", () => {
    assert.deepEqual(parseArgs(["render", "dir/living-room.mjs"]),
                     { command: "render", input: "dir/living-room.mjs", out: "living-room-pages", state: "normal", page: undefined, grid: true });
    assert.deepEqual(parseArgs(["render", "l.json", "-o", "shots", "--pressed", "--page", "Watch TV", "--no-grid"]),
                     { command: "render", input: "l.json", out: "shots", state: "pressed", page: "Watch TV", grid: false });
    assert.equal(parseArgs(["render", "l.json", "--out", "x"]).out, "x");
});

test("args: render mistakes are reported with the usage", () => {
    usage(["render"], /one layout/);
    usage(["render", "a.json", "b.json"], /one layout/);
    usage(["render", "a.bin"], /baked/);
    usage(["render", "a.json", "--page"], /needs a value/);
    usage(["render", "a.json", "--serial", "X"], /unknown option --serial/);
});
