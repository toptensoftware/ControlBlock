// The Node renderer for stage 2: an image definition (node-buttonImage settings) -> rle565 with
// alpha, 128x128. Uses sharp, which does not run in a browser: a web front end supplies its own
// renderer to renderImages() instead.

import { renderButtonImage, encodeImage } from "../images.js";
import { KEY_SIZE } from "./consts.js";

export function createRenderer({ size = KEY_SIZE } = {})
{
    return async def => {
        const data = await encodeImage(await renderButtonImage(def, size), size, size);
        return Buffer.from(data.buffer, data.byteOffset, data.byteLength);
    };
}
