// Shared by the hardware test layouts (activity-test.mjs, script-test.mjs): the PC running
// examples/test-server/test-server.mjs, ops that announce themselves to it, and flat key faces.

// The PC running examples/test-server/test-server.mjs (HTTP :8000, UDP :9999)
export const SERVER = "10.1.1.126";
export const HTTP = `http://${SERVER}:8000`;

export const udp = data => ({ op: "udpPacket", host: SERVER, port: 9999, data });
export const get = query => ({ op: "httpGet", url: `${HTTP}/get?${query}` });

// Every look's `color` (keys, defaults, pressed) becomes a flat face. `color` alone draws a
// top-to-bottom shade, which bands visibly in RGB565 on dark faces; a face whose top and
// bottom are the same colour is flat.
export function flatten(v)
{
    if (Array.isArray(v))
        return v.map(flatten);
    if (v === null || typeof v !== "object")
        return v;
    const out = {};
    for (const [k, x] of Object.entries(v))
    {
        if (k === "color" && typeof x === "string")
            out.face = { top: x, bottom: x };
        else
            out[k] = flatten(x);
    }
    return out;
}
