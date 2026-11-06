// Stage 3: pack the rendered tree with binpack, and wrap it in the BPAK file format.

import { pack, formatTypes } from "../../../binpack/index.js";
import { ROOT_TYPE } from "./schema.js";
import { allKeys, LAYERS } from "./tree.js";
import { childPath } from "./diag.js";

export { ROOT_TYPE };

const SIGNATURE = 0x4B415042;   // "BPAK" (32-bit pointers)
const FORMAT_VERSION = 1;       // the BPAK container version
const HEADER_SIZE = 32;

// Plain synchronous pack -> { binary, relocations }
export function packTree(tree)
{
    for (const [key, path] of allKeys(tree))
        for (const layer of LAYERS)
            if (key[layer] && !Buffer.isBuffer(key[layer]))
                throw new Error(`${childPath(path, layer)} is still an image definition: run the render stage before packing`);
    return pack(ROOT_TYPE, tree);
}

// The combined file: 32-byte header, the data with every pointer made file-relative, then the
// relocation table. Byte for byte what binpack's own CLI writes (a test compares them); written
// here because the CLI module pulls in Node-only code and this must run in a browser too.
export function toBpak({ binary, relocations })
{
    const tableOffset = HEADER_SIZE + binary.length;
    const file = Buffer.alloc(tableOffset + relocations.length * 4);
    file.writeUInt32LE(SIGNATURE, 0);
    file.writeUInt32LE(FORMAT_VERSION, 4);
    file.writeUInt32LE(relocations.length, 8);
    file.writeUInt32LE(tableOffset, 12);
    binary.copy(file, HEADER_SIZE);
    relocations.forEach((offset, i) => {
        const at = HEADER_SIZE + offset;
        file.writeUInt32LE(file.readUInt32LE(at) + HEADER_SIZE, at);
        file.writeUInt32LE(at, tableOffset + i * 4);
    });
    return file;
}

// The C declarations of the structs and op codes, for the firmware loader
export const headerText = () => formatTypes();
