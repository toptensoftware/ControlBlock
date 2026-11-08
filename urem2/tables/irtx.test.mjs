// Tests for irtx.mjs. From the repo root:  node --test "urem2/**/*.test.mjs"
// Vectors hand-computed from the irtx readme: u16 cmd=4 | u16 0 | FourCC | u64 code (LE) | u8 repeat.

import { test } from "node:test";
import assert from "node:assert/strict";
import { IRTX_PORT, irPacket, decodeIrtxPacket } from "./irtx.mjs";
import { IR_CODES, parseIrCode } from "./ir.mjs";

const LG_POWER_ON = "040000004e4543204cb3df200000000000";
const PANA_9D = "0400000050414e419d98080d0440000000";

test("irtx listens on UDP 4210", () => {
    assert.equal(IRTX_PORT, 4210);
});

test("irPacket encodes a NEC code, by name or raw", () => {
    assert.equal(irPacket("lg.powerOn"), LG_POWER_ON);
    assert.equal(irPacket("NEC:0x20dfb34c"), LG_POWER_ON);
    assert.equal(irPacket("NEC:0x20DFB34C"), LG_POWER_ON);
});

test("irPacket encodes a PANA code", () => {
    assert.equal(irPacket("PANA:0x40040d08989d"), PANA_9D);
});

test("irPacket sets the repeat byte", () => {
    assert.equal(irPacket("lg.powerOn", { repeat: true }), LG_POWER_ON.slice(0, -2) + "01");
    assert.equal(irPacket("lg.powerOn", { repeat: false }), LG_POWER_ON);
});

test("irPacket throws on a bad name or code", () => {
    assert.throws(() => irPacket("lg.nope"), /unknown IR code/);
    assert.throws(() => irPacket("SONY:0x12"), /unknown IR device|bad IR code/);
});

test("decodeIrtxPacket reads the vectors back", () => {
    assert.deepEqual(decodeIrtxPacket(Buffer.from(LG_POWER_ON, "hex")),
                     { protocol: "NEC", code: 0x20dfb34cn, repeat: false });
    assert.deepEqual(decodeIrtxPacket(Buffer.from(PANA_9D, "hex")),
                     { protocol: "PANA", code: 0x40040d08989dn, repeat: false });
    assert.deepEqual(decodeIrtxPacket(Buffer.from(LG_POWER_ON.slice(0, -2) + "01", "hex")),
                     { protocol: "NEC", code: 0x20dfb34cn, repeat: true });
});

test("every code in the table round-trips", () => {
    let n = 0;
    for (const [device, codes] of Object.entries(IR_CODES))
        for (const key of Object.keys(codes))
        {
            const hex = irPacket(`${device}.${key}`);
            assert.equal(hex.length, 34);
            assert.deepEqual(decodeIrtxPacket(Buffer.from(hex, "hex")), { ...parseIrCode(codes[key]), repeat: false });
            n++;
        }
    assert.equal(n, 169);
});

test("decodeIrtxPacket reports a bad packet", () => {
    const good = Buffer.from(LG_POWER_ON, "hex");
    assert.match(decodeIrtxPacket(good.subarray(0, 16)).error, /length/);
    assert.match(decodeIrtxPacket(Buffer.concat([good, Buffer.from([0])])).error, /length/);
    assert.match(decodeIrtxPacket(Buffer.alloc(0)).error, /length/);

    const cmd1 = Buffer.from(good);
    cmd1[0] = 1;
    assert.match(decodeIrtxPacket(cmd1).error, /cmd 1/);

    const sony = Buffer.from(good);
    sony.write("SONY", 4, "latin1");
    assert.match(decodeIrtxPacket(sony).error, /protocol/);
});
