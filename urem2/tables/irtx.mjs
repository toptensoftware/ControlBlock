// The irtx IR blaster's UDP "Transmit IR Code" packet (cmd 4; ../irtx/readme.md "UDP Protocol"), 17 bytes,
// little endian, no reply:  u16 cmd=4 | u16 0 | u32 protocol FourCC | u64 code | u8 repeat.
// irPacket encodes it for the layout (the udpPacket op's data.hex); decodeIrtxPacket is for the test server.

import { irCode, parseIrCode } from "./ir.mjs";

export const IRTX_PORT = 4210;

const CMD_SEND_IR = 4;
const PACKET_LEN = 17;
const FOURCC = { NEC: "NEC ", PANA: "PANA" };    // RIFF style, space padded

// "lg.powerOn", "NEC:0x20dfb34c" or { protocol, code } -> the packet as a hex string.
// repeat: send the NEC repeat frame (Panasonic has none; urem always sent 0).
export function irPacket(codeOrStr, { repeat = false } = {})
{
    const { protocol, code } = typeof codeOrStr === "string"
        ? parseIrCode(codeOrStr.includes(":") ? codeOrStr : irCode(codeOrStr))
        : codeOrStr;
    const buf = Buffer.alloc(PACKET_LEN);
    buf.writeUInt16LE(CMD_SEND_IR, 0);
    buf.write(FOURCC[protocol], 4, "latin1");
    buf.writeBigUInt64LE(code, 8);
    buf[16] = repeat ? 1 : 0;
    return buf.toString("hex");
}

// A received datagram -> { protocol, code: BigInt, repeat: bool }, or { error } if it isn't a valid cmd 4 packet
export function decodeIrtxPacket(buf)
{
    if (buf.length !== PACKET_LEN)
        return { error: `length ${buf.length}, expected ${PACKET_LEN}` };
    const cmd = buf.readUInt16LE(0);
    if (cmd !== CMD_SEND_IR)
        return { error: `cmd ${cmd}, expected ${CMD_SEND_IR}` };
    const fourcc = buf.subarray(4, 8).toString("latin1");
    const protocol = Object.keys(FOURCC).find(p => FOURCC[p] === fourcc);
    if (!protocol)
        return { error: `unknown protocol ${JSON.stringify(fourcc)}` };
    return { protocol, code: buf.readBigUInt64LE(8), repeat: buf[16] !== 0 };
}
