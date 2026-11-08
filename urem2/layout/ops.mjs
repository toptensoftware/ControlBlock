// Op builders for urem2's devices: each wraps one table entry (urem2/tables/) in a ControlBlock op addressed to the
// chosen target (urem2/targets.mjs). Codes, paths and names come only from the tables, which throw on unknown
// names, so a typo fails the test or the bake.

import { irPacket } from "../tables/irtx.mjs";
import { atvKey, atvApp, ATV_PING, ATV_DISCONNECT } from "../tables/appletv.mjs";
import { HOTPC_COMMANDS, SLIDESHOW_COMMANDS, displayElementsBody } from "../tables/hotpc.mjs";

// After each IR power packet in a device list: the blaster holds one code in flight and one pending and drops a
// third, so a switch never queues three IR packets at once (NEC frame ~68 ms plus a gap). Tune on the hardware.
export const IR_GAP_MS = 120;

const JSON_TYPE = "application/json";

function oneOf(list, value, what)
{
    if (!list.includes(value))
        throw new Error(`unknown ${what} ${JSON.stringify(value)}`);
    return value;
}

export function makeOps(target)
{
    const udp = ({ host, port }, data) => ({ op: "udpPacket", host, port, data });
    const postJson = (url, body) => ({ op: "httpPost", url, body, contentType: JSON_TYPE });
    const pcUrl = `http://${target.hotpc.host}:${target.hotpc.port}`;
    const showUdp = { host: target.slideshow.host, port: target.slideshow.udpPort };
    const showUrl = `http://${target.slideshow.host}:${target.slideshow.httpPort}`;
    const atv = text => udp(target.atvd, text);
    const ir = name => udp(target.irtx, { hex: irPacket(name) });

    return {
        // "lg.powerOn": one IR code through the blaster
        ir,
        // An IR code in a device's power list, followed by the IR gap
        irPower: name => [ir(name), { op: "delay", ms: IR_GAP_MS }],

        atvKey: (name, action) => atv(atvKey(name, action)),
        atvApp: app => atv(atvApp(app)),
        atvPing: () => atv(ATV_PING),
        atvDisconnect: () => atv(ATV_DISCONNECT),

        // path: from a tables/yamaha.mjs builder ("/main/...")
        yamaha(path)
        {
            if (typeof path !== "string" || !path.startsWith("/main/"))
                throw new Error(`bad Yamaha path ${JSON.stringify(path)}`);
            return { op: "httpGet", url: target.yamaha + path };
        },

        hotpc: cmd => postJson(`${pcUrl}/${oneOf(HOTPC_COMMANDS, cmd, "HOT PC command")}`, "{}"),
        wake: () => ({ op: "sendWol", mac: target.hotpc.mac }),

        slideshow: cmd => udp(showUdp, oneOf(SLIDESHOW_COMMANDS, cmd, "slideshow command")),
        slideshowToggle: el => postJson(`${showUrl}/display-elements`, displayElementsBody(el)),
    };
}
