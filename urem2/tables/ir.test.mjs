// Tests for ir.mjs. From the repo root:  node --test "urem2/**/*.test.mjs"

import { test } from "node:test";
import assert from "node:assert/strict";
import { IR_CODES, irCode, parseIrCode, irNames } from "./ir.mjs";

test("the table has every device and code from urem", () => {
    const counts = Object.fromEntries(Object.entries(IR_CODES).map(([dev, codes]) => [dev, Object.keys(codes).length]));
    assert.deepEqual(counts, { atv: 28, lg: 35, pana: 53, yama: 53 });
});

test("irCode looks up a dotted name", () => {
    assert.equal(irCode("lg.powerOn"), "NEC:0x20dfb34c");
    assert.equal(irCode("pana.forward"), "PANA:0x40040d08a0a5");
});

test("irCode throws on an unknown device or name", () => {
    assert.throws(() => irCode("sony.powerOn"), /unknown IR device/);
    assert.throws(() => irCode("lg.nope"), /unknown IR code/);
    assert.throws(() => irCode("lg"), /unknown IR/);
    assert.throws(() => irCode("lg.toString"), /unknown IR code/);       // not inherited properties
});

test("parseIrCode splits NEC and PANA codes into a BigInt", () => {
    assert.deepEqual(parseIrCode("NEC:0x20dfb34c"), { protocol: "NEC", code: 0x20dfb34cn });
    assert.deepEqual(parseIrCode("NEC:0x20DF609F"), { protocol: "NEC", code: 0x20df609fn });
    assert.deepEqual(parseIrCode("PANA:0x40040d08989d"), { protocol: "PANA", code: 0x40040d08989dn });
});

test("parseIrCode throws on a bad string", () => {
    for (const bad of ["SONY:0x1234", "NEC:1234", "NEC:0x", "NEC:0xzz", "nec:0x12", "NEC:0x1ffffffffffffffff", "", 42])
        assert.throws(() => parseIrCode(bad), /bad IR code/, String(bad));
});

test("irNames returns every name for a code (duplicates are on purpose)", () => {
    assert.deepEqual(irNames("NEC", 0x77e1602an), ["atv.right", "atv.forward", "atv.forward10x"]);
    assert.deepEqual(irNames("NEC", 0x77e1c02an), ["atv.home", "atv.menu"]);
    assert.deepEqual(irNames("NEC", 0x20df609fn), ["lg.right"]);          // the table writes this one in upper case
    assert.deepEqual(irNames("NEC", 0x20dfb34cn), ["lg.powerOn"]);
});

test("irNames is empty for an unknown code or protocol", () => {
    assert.deepEqual(irNames("NEC", 0x12345678n), []);
    assert.deepEqual(irNames("PANA", 0x20dfb34cn), []);                   // right code, wrong protocol
    assert.deepEqual(irNames("SONY", 0x20dfb34cn), []);
});
