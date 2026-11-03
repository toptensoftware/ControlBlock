# Bake tool plan

Implements the pipeline in `docs/config-format.md` (input) and `docs/standalone.md` (blob). Scope: bake (config file -> `.bin`), plus the upload to the device (`src/upload.js`, `src/args.js`; the protocol is in `docs/protocol.md`, the design in `docs/config-upload-plan.md`). Test first for every item (`npm test`).

Layout (`tools/bake/`):

| File | Role |
|---|---|
| `src/diag.js` | error/warning collector with JSON paths |
| `src/check.js` | tiny declarative property checkers (unknown keys, types, ranges) |
| `src/hid.js` | HID key name table, chord parser |
| `src/ops.js` | op table: validation + packed form, one place per op |
| `src/look.js` | look properties -> four image definitions (bg/fg, normal/pressed) |
| `src/resolve.js` | **stage 1** (sync, pure; host injected: readText, exists, luaCheck) |
| `src/lua.js` | wasmoon syntax checker (async factory, sync check) |
| `src/render.js` | **stage 2** walker + canonical-Buffer cache; default renderer (node-buttonImage + rle565) |
| `src/schema.js` | binpack type registrations (the device structs) |
| `src/pack.js` | **stage 3** binpack + BPAK combined file |
| `src/bake.js` | orchestration |
| `src/args.js`, `src/upload.js` | command line parsing; upload a blob with the host library, wait for the restart, verify |
| `cli.js` | `node cli.js config.json -o out.bin`; `--upload` bakes and uploads; `node cli.js out.bin` uploads a baked blob |

Tasks (each: tests, then code):
1. [x] diag + check helpers
2. [x] hid table
3. [x] ops (all eight ops, bad input, `use` expansion context)
4. [x] look: defaults chain merge, images defs, pressed treatment
5. [x] stage 1 resolve: structure, names -> indices, touch control names, binding order, validation, warnings
6. [x] lua syntax check (wasmoon)
7. [x] stage 2: canonical buffers, render dedupe, absent images
8. [x] schema + stage 3: pack, round trip via unpack, alignment, BPAK file
9. [x] bake() end to end with the doc's example + CLI
10. [x] real renderer smoke test (node-buttonImage + rle565 + `check_image` style sanity)

Done: all ten. Left for later (not in scope here): device limit checks (pages, images, blob size, op list length), the
"icon larger than the icon area" warning, PNG layers in `image`, browser renderer. Upload is done (`--upload`; not yet run against a real device).
Findings: binpack could not share one Buffer between two different fields (fixed in the submodule, test added);
node-buttonImage's built-in icon names are kebab-case (`#play`, `#apple-tv`), not Material names.
