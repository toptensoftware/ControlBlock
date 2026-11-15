// Preloaded with `node --import` by the CLI's lazy-loading test: refuses the image stack (sharp,
// button-face and its native canvas, wasmoon, and the rle565 encoder, which loads sharp) to the
// library's and the CLI's own modules, so a command that would load it fails. Test helpers are
// let through: FakeDevice decodes images with rle565's unpack.js, which loads sharp.
import { registerHooks } from "node:module";

const BLOCKED = /^(sharp|wasmoon|@toptensoftware\/button-face|@napi-rs\/canvas|#?rle565)(\/|$)/;
const GUARDED_PARENT = /\/(host\/index\.js|host\/src\/|cli\/(?!test\/))/;

registerHooks({
    resolve(specifier, context, nextResolve)
    {
        if (BLOCKED.test(specifier) && GUARDED_PARENT.test(context.parentURL ?? ""))
            throw new Error(`blocked: ${specifier} (imported by ${context.parentURL})`);
        return nextResolve(specifier, context);
    },
});
