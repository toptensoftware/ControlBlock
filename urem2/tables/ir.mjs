// urem's IR code table (ircodes.json, a verbatim copy of urem's ircodes/ircodes.json) and helpers.
// Shared by the urem2 layout (encode) and the test server (decode), so the two can't drift.
// Codes are "NEC:0x..." or "PANA:0x..." strings; the hex case varies in the table, so compare as BigInt.

import IR_CODES from "./ircodes.json" with { type: "json" };

export { IR_CODES };

const own = (obj, key) => Object.prototype.hasOwnProperty.call(obj, key);

// "lg.powerOn" -> "NEC:0x20dfb34c". Throws on an unknown device or name, so a layout typo fails.
export function irCode(name)
{
    const [device, key, ...rest] = String(name).split(".");
    if (!own(IR_CODES, device))
        throw new Error(`unknown IR device in "${name}"`);
    if (key === undefined || rest.length || !own(IR_CODES[device], key))
        throw new Error(`unknown IR code "${name}"`);
    return IR_CODES[device][key];
}

// "NEC:0x20dfb34c" -> { protocol: "NEC", code: 0x20dfb34cn }. Throws on anything else (code must fit 64 bits).
export function parseIrCode(str)
{
    const m = typeof str === "string" && /^(NEC|PANA):0x([0-9a-fA-F]{1,16})$/.exec(str);
    if (!m)
        throw new Error(`bad IR code ${JSON.stringify(str)}`);
    return { protocol: m[1], code: BigInt("0x" + m[2]) };
}

// Every "device.name" with this protocol and code, in table order ([] if none)
export function irNames(protocol, code)
{
    const names = [];
    for (const [device, codes] of Object.entries(IR_CODES))
        for (const [key, str] of Object.entries(codes))
        {
            const c = parseIrCode(str);
            if (c.protocol === protocol && c.code === code)
                names.push(`${device}.${key}`);
        }
    return names;
}
