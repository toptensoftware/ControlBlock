// Key images for the device: RGBA layers in, one fully opaque rle565 image out, with a content
// ID. Node only (sharp, node-buttonImage, the rle565 encoder). docs/image-assets.md.
//
// Protocol v2: each key state is one opaque image exactly the key's size (keyPixelSize in
// protocol.js: 144x144 for 1U, 288x144 for 2U...), composed by the host over the device's black
// background. A layer is { rgba, width, height, x, y }: straight-alpha RGBA pixels, placed at
// (x, y) in the key, or centred when x/y are missing. The usual 1U key is a 128 face (and its
// icon/label) centred in the 144 cell: FACE_MARGIN px of black each side.

import sharp from "sharp";
import { packRle } from "rle565";
import { ButtonImage } from "@toptensoftware/node-buttonImage";
import { crc32 } from "./protocol.js";
// Black round a face in a key image (each side): a 1U cell of 144 holds a 128 face. Defined with
// the bake's key geometry.
export { FACE_MARGIN } from "./bake/consts.js";

// The image's ID: CRC-32 of the encoded file, 0 mapped to 1 (0 means none).
// The same image gets the same ID in every session and app.
export function imageId(data)
{
    const crc = crc32(data);
    return crc === 0 ? 1 : crc;
}

const toBuffer = rgba => Buffer.from(rgba.buffer, rgba.byteOffset, rgba.byteLength);
const toBytes = buf => new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);

// A node-buttonImage definition (its settings object) rendered to width x height RGBA
export async function renderButtonImage(def, width, height = width)
{
    return toBytes(await new ButtonImage(def).render(width, height, 4));
}

// The same as a layer
export async function buttonLayer(def, width, height = width)
{
    return { rgba: await renderButtonImage(def, width, height), width, height };
}

// An SVG string rendered (stretched) to width x height, as a layer
export async function svgLayer(svg, width, height = width)
{
    const rgba = await sharp(Buffer.from(svg)).resize(width, height, { fit: "fill" }).ensureAlpha().raw().toBuffer();
    return { rgba: toBytes(rgba), width, height };
}

// Encode width x height RGBA pixels to rle565
export async function encodeImage(rgba, width, height)
{
    const packed = await packRle({ data: toBuffer(rgba), info: { width, height, channels: 4 } }, {});
    return new Uint8Array(packed.data);
}

// RGBA centred in an outWidth x outHeight image with transparent padding
export async function centreImage(rgba, width, height, outWidth, outHeight = outWidth)
{
    if (width === outWidth && height === outHeight)
        return rgba;
    if (width > outWidth || height > outHeight)
        throw new Error(`Image ${width}x${height} is bigger than ${outWidth}x${outHeight}`);
    const left = (outWidth - width) >> 1, top = (outHeight - height) >> 1;
    const out = await sharp(toBuffer(rgba), { raw: { width, height, channels: 4 } })
        .extend({ left, top, right: outWidth - width - left, bottom: outHeight - height - top,
                  background: { r: 0, g: 0, b: 0, alpha: 0 } })
        .raw()
        .toBuffer();
    return toBytes(out);
}


// The part of a layer that falls inside a width x height image, as a layer at its place there
// (the same layer when it fits; null when none of it does). Lets a picture larger than a key,
// like a group's face, be drawn in parts.
export function clipLayer(layer, width, height)
{
    const x = layer.x ?? (width - layer.width) >> 1, y = layer.y ?? (height - layer.height) >> 1;
    const x0 = Math.max(0, -x), y0 = Math.max(0, -y);
    const x1 = Math.min(layer.width, width - x), y1 = Math.min(layer.height, height - y);
    if (x1 <= x0 || y1 <= y0)
        return null;
    if (x0 === 0 && y0 === 0 && x1 === layer.width && y1 === layer.height)
        return layer;
    const w = x1 - x0, h = y1 - y0, rgba = new Uint8Array(w * h * 4);
    for (let r = 0; r < h; r++)
    {
        const from = ((y0 + r) * layer.width + x0) * 4;
        rgba.set(layer.rgba.subarray(from, from + w * 4), r * w * 4);
    }
    return { rgba, width: w, height: h, x: x + x0, y: y + y0 };
}

// Layers (one, or an array, drawn in order) composed over black into width x height RGBA that
// is opaque everywhere. Each layer must fit inside.
export async function composeOverBlack(layers, width, height)
{
    const inputs = [layers].flat().map(l => {
        const left = l.x ?? (width - l.width) >> 1, top = l.y ?? (height - l.height) >> 1;
        if (left < 0 || top < 0 || left + l.width > width || top + l.height > height)
            throw new Error(`A ${l.width}x${l.height} layer at (${left}, ${top}) doesn't fit a ${width}x${height} key`);
        return { input: toBuffer(l.rgba), raw: { width: l.width, height: l.height, channels: 4 }, left, top };
    });
    const out = await sharp({ create: { width, height, channels: 4, background: { r: 0, g: 0, b: 0, alpha: 1 } } })
        .composite(inputs)
        .raw()
        .toBuffer();
    const rgba = toBytes(out);
    for (let i = 3; i < rgba.length; i += 4)        // over opaque black: make sure no rounding leaves 254
        rgba[i] = 255;
    return rgba;
}

// A key image ready to upload: layers composed over black at the key's size (width x height,
// from keyPixelSize), encoded: { id: CRC-32, data: rle565, width, height }
export async function keyImage(layers, width, height)
{
    const data = await encodeImage(await composeOverBlack(layers, width, height), width, height);
    return { id: imageId(data), data, width, height };
}
