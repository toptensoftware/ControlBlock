// The Node renderer for stage 2: a key image spec ({ width, height, layers }) -> one opaque rle565
// image, width x height: each layer's node-buttonImage definition rendered at its size, clipped
// to the key (a group's picture is larger than each of its keys) and composed over black. Uses
// sharp, which does not run in a browser: a web front end supplies its own renderer to
// renderImages() instead.

import { renderButtonImage, encodeImage, composeOverBlack, clipLayer } from "../images.js";
import { stable } from "./render.js";

// spec -> Promise<RGBA> (opaque, width x height): the composition both the bake and the page
// previews (preview.js) use
export function createPixelRenderer()
{
    // A layer drawn once per renderer, however many keys share it (a group's face, a common icon)
    const layers = new Map();
    const layerPixels = ({ def, width, height }) => {
        const id = `${width}x${height}:${stable(def)}`;
        if (!layers.has(id))
            layers.set(id, renderButtonImage(def, width, height));
        return layers.get(id);
    };

    return async ({ width, height, layers: list }) => {
        const placed = await Promise.all(list.map(async l =>
            clipLayer({ rgba: await layerPixels(l), width: l.width, height: l.height, x: l.x, y: l.y }, width, height)));
        return composeOverBlack(placed.filter(Boolean), width, height);
    };
}

// spec -> Promise<Buffer> (rle565): the bake's renderer
export function createRenderer()
{
    const pixels = createPixelRenderer();
    return async spec => {
        const data = await encodeImage(await pixels(spec), spec.width, spec.height);
        return Buffer.from(data.buffer, data.byteOffset, data.byteLength);
    };
}
