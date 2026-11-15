// Stage 2: replace each key's image specs (normal, pressed) with rle565 Buffers. The renderer is
// injected (spec -> Promise<Buffer>: the spec's layers composed over black at its pixel size) so
// this runs anywhere; the Node one is in render-node.js.
//
// Identical content always gives the SAME Buffer object, so binpack's pointer dedupe stores each
// image once (docs/standalone.md "Build pipeline and sources"). Two levels: specs that are equal
// render once, and renders that produce equal bytes share one Buffer.

import { childPath } from "./diag.js";
import { IMAGES, allKeys } from "./tree.js";

// JSON with sorted keys, so equal definitions always give equal strings
export const stable = v =>
    Array.isArray(v) ? `[${v.map(stable).join(",")}]`
    : v && typeof v === "object" ? `{${Object.keys(v).sort().map(k => `${JSON.stringify(k)}:${stable(v[k])}`).join(",")}}`
    : JSON.stringify(v) ?? "null";

// Mutates the tree (definitions -> Buffers) and returns it. `concurrency` renders at a time.
export async function renderImages(tree, render, { concurrency = 4 } = {})
{
    const byDefinition = new Map();   // stable(def) -> Promise<Buffer>
    const byContent = new Map();      // the bytes as a latin1 string -> canonical Buffer

    const renderOnce = (def, where) => {
        const id = stable(def);
        if (!byDefinition.has(id))
            byDefinition.set(id, (async () => {
                let bytes;
                try { bytes = await render(def); }
                catch (e) { throw new Error(`${where}: ${e.message}`, { cause: e }); }
                if (!(bytes instanceof Uint8Array))
                    throw new Error(`${where}: the renderer must return a Buffer (got ${typeof bytes})`);
                if (bytes.length === 0)
                    throw new Error(`${where}: the renderer returned an empty image`);
                const buf = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
                const content = buf.toString("latin1");
                if (!byContent.has(content))
                    byContent.set(content, buf);
                return byContent.get(content);
            })());
        return byDefinition.get(id);
    };

    // A simple pool: `concurrency` workers pulling from the job list
    const jobs = [];
    for (const [key, path] of allKeys(tree))
        for (const image of IMAGES)
            if (key[image])
                jobs.push(async () => { key[image] = await renderOnce(key[image], childPath(path, image)); });

    let next = 0;
    const worker = async () => { while (next < jobs.length) await jobs[next++](); };
    await Promise.all(Array.from({ length: Math.min(concurrency, jobs.length) }, worker));
    return tree;
}
