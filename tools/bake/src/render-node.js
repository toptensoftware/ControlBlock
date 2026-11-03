// The Node renderer for stage 2: an image definition (node-buttonImage settings) -> rle565 with
// alpha, 128x128. Uses sharp, which does not run in a browser: a web front end supplies its own
// renderer to renderImages() instead.

import { ButtonImage } from "@toptensoftware/node-buttonImage";
import { packRle } from "rle565";
import { KEY_SIZE } from "./consts.js";

export function createRenderer({ size = KEY_SIZE } = {})
{
    return async def => {
        const rgba = await new ButtonImage(def).render(size, size, 4);
        const packed = await packRle({ data: Buffer.from(rgba.buffer, rgba.byteOffset, rgba.byteLength), info: { width: size, height: size, channels: 4 } }, {});
        return Buffer.from(packed.data);
    };
}
