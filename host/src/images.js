// Key images for the device: RGBA in, rle565 (with alpha) out, with a
// content ID. Node only (sharp, the rle565 encoder). docs/image-assets.md.

import zlib from "node:zlib";
import sharp from "sharp";
import { packRle } from "rle565";

// The image's ID: CRC-32 of the encoded file, 0 mapped to 1 (0 means none).
// The same image gets the same ID in every session and app.
export function imageId(data)
{
    const crc = zlib.crc32(data) >>> 0;
    return crc === 0 ? 1 : crc;
}

// Encode width x height RGBA pixels to rle565
export async function encodeImage(rgba, width, height)
{
    const data = Buffer.from(rgba.buffer, rgba.byteOffset, rgba.byteLength);
    const packed = await packRle({ data, info: { width, height, channels: 4 } }, {});
    return new Uint8Array(packed.data);
}

// RGBA centred in a size x size image with transparent padding (icons are
// smaller than the key; the padding costs next to nothing in rle565)
export async function centreImage(rgba, width, height, size)
{
    if (width === size && height === size)
        return rgba;
    if (width > size || height > size)
        throw new Error(`Image ${width}x${height} is bigger than the key (${size})`);
    const left = (size - width) >> 1, top = (size - height) >> 1;
    const out = await sharp(Buffer.from(rgba.buffer, rgba.byteOffset, rgba.byteLength),
                            { raw: { width, height, channels: 4 } })
        .extend({ left, top, right: size - width - left, bottom: size - height - top,
                  background: { r: 0, g: 0, b: 0, alpha: 0 } })
        .raw()
        .toBuffer();
    return new Uint8Array(out.buffer, out.byteOffset, out.byteLength);
}

// A key image ready to upload: { id, data }. `size` is the key size
// (info.keyWidth); smaller artwork is centred.
export async function keyImage(rgba, width, height, size)
{
    const data = await encodeImage(await centreImage(rgba, width, height, size), size, size);
    return { id: imageId(data), data };
}

// The same from an SVG string, rendered at the key size
export async function keyImageFromSvg(svg, size)
{
    const rgba = await sharp(Buffer.from(svg)).resize(size, size).ensureAlpha().raw().toBuffer();
    return keyImage(new Uint8Array(rgba.buffer, rgba.byteOffset, rgba.byteLength), size, size, size);
}
