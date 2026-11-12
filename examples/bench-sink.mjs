// The "sink" case of bench.js: full-size PUT_IMAGE messages for image id 0, which the device
// refuses (BAD_ARG) and whose data it skips without touching the image store (no PSRAM write,
// no check_image). Sent pipelined like an upload, so the time is the USB pipe's alone.

import { Msg, Status, encodePutImage, PUT_IMAGE_PREFIX } from "../host/index.js";

// Send `count` messages of the device's maximum payload; resolves to the bytes sent (message
// payloads). Throws if the device answers anything but BAD_ARG.
export async function sinkSend(cb, count)
{
    const data = new Uint8Array(cb.info.maxPayload - PUT_IMAGE_PREFIX).map((_, i) => i * 37);
    const message = encodePutImage(0, data.length, 0, data);
    const rs = await Promise.all(Array.from({ length: count }, () => cb.request(Msg.PUT_IMAGE, message)));
    const bad = rs.find(r => r.status !== Status.BAD_ARG);
    if (bad)
        throw new Error(`sink: the device answered status ${bad.status}, not BAD_ARG`);
    return message.length * count;
}
