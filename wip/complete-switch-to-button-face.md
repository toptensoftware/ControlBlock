# Switch from node-buttonImage to button-face

Status: Complete

## Description

This job covers switching from using `node-buttonImage` to `button-face` - a reworked
but breaking port of `node-buttonImage`.

Investigate should start by reading the `button-face` readme to learn about that module.

This job covers:

1. Removing the `node-buttonImage` git submodule
2. Removing the `package.json` dependencies on `node-buttonImage` and switching to the
   github hosted `toptensoftware/button-face` package.
3. Updating everywhere `node-buttonImage` is used to use the new library
4. Updating documentation that references old `node-buttonImage` definitions
5. Consider whether upgrading existing button definitions (tests, demos, urem2) to the new
   layer based definintions is worth it.  `button-face` exports a function
   `convertLegacyDefinition` that can be used to convert at runtime.  I'd prefer not
   have to do this and switch everything to the new format, but this might warrant a
   separate IPI - assess.

## Notes

### Source of the button-face information

- Local clone at `C:\Users\Brad\Projects\button-face` (sibling of the repo; remote
  `git@github.com:toptensoftware/button-face.git`, HEAD `728ea56` "Write full README..." = `origin/main`,
  clean apart from its gitignored `scratch/`). Read: `README.md` (full, 919 lines), `package.json`,
  `src/index.js`, `src/node.js`, `src/legacy.js`, `src/node/fileResolver.js`, `src/render.js` (head).
- GitHub: `toptensoftware/button-face` is **public**, default branch `main`, **no tags**.
  (`toptensoftware/node-buttonImage` is public too.)
- There is also a sibling clone `C:\Users\Brad\Projects\node-buttonImage` (outside this repo; leave it alone).

### button-face in brief

- `package.json`: name `@toptensoftware/button-face`, version `0.0.1`, `"type": "module"` (ESM only, plain JS,
  JSDoc, **no build step**), `engines.node >= 24`, `files: ["src/", "LICENSE"]` (the 42 icons are bundled as
  `src/icons.js`, generated), deps: `@napi-rs/canvas` ^1.1.1 only (prebuilt Skia; no sharp, no node-canvas);
  devDep `playwright-core`. Exports: `"."` -> `src/index.js` (portable core, browser-safe), `"./node"` ->
  `src/node.js` (core + `napiBackend`, file resolver, disk cache, `render` with Node defaults).
- Install: README says `npm install toptensoftware/button-face` (not on the npm registry); import as
  `@toptensoftware/button-face/node`. In package.json that is
  `"@toptensoftware/button-face": "github:toptensoftware/button-face"` (optionally `#<sha>`; the lock pins the
  commit anyway). `install-links=true` only affects `file:` deps, so it doesn't matter for this one.
- Definition: `{ virtualSize?: {width, height} (default 72x72), layers: [...] }`, layers painted in order.
  Layer types (each also `visible`, `opacity`):
  - `fill`: `color` (default black).
  - `svg`: `src` (`#name` built-in, `@name?style=&fill=&weight=&grade=&size=` Material Symbols downloaded and
    cached in `<tmpdir>/button-face/material`, else a file path relative to cwd or the resolver's `baseDir`)
    **or** `svg` (inline markup); `text` (replaces `$(x)`), `colors` (colour map over paint values),
    `color` (what `currentColor` is; default `#848484`), `tint`, `scale` (fraction of device height,
    default 1), `translate` {x,y} (virtual px).
  - `text`: `text`, `font` (default `bold 16px sans-serif`), `color` (white), `align`, `valign` (center),
    `lineSpacing` (1), `padding` (3; number, [tb, lr] or [t,r,b,l]), `translate`.
- `render(definition, width, height, { channels = 3 | 4, backend, resolveSvg })` is **async** and resolves
  to `{ data: Uint8Array, width, height, channels }` (4 = straight RGBA, not premultiplied). The `/node`
  entry defaults backend and resolver. Strict validation (bad colours, alignments, `scale: 0`, ... reject).
  **An unconverted node-buttonImage definition is rejected** ("looks like a node-buttonImage definition
  (has 'backColor') - convert it with convertLegacyDefinition()").
- `convertLegacyDefinition(old)`: pure mapping, returns a definition with `layers` unchanged. Mapping:
  `backColor` (default black) -> first `fill` layer; `svgCode`/`svgFile` -> svg `svg`/`src`; `svgText` ->
  `text`; `svgForeColor` -> `colors: { "#848484": c }`; `svgScale`/`svgTranslate` -> `scale`/`translate`;
  `text` -> last `text` layer; `textColor/textAlign/textVAlign/font/textLineSpacing/textPadding/textTranslate`
  -> `color/align/valign/font/lineSpacing/padding/translate`; `virtualSize` kept. Ignores `channels`,
  `states`, `onInvalidate`, unknown keys.
- Other exports used here: `icons` (frozen `{ name: svg }`: replaces our `hasIcon` lookup of
  `node-buttonImage/images/*.svg`), `parseColor` (could validate look colours in stage 1),
  `parseMaterialRef`/`validateMaterialParams` (if `@` icons are allowed in layouts), `createNodeSvgResolver`.
- Same 42 built-in icon names as node-buttonImage 1.1.3 (diffed: identical name lists). 20 of the SVGs differ
  (strokes/text converted to paths, line endings, `currentColor`), so icons can look slightly different.
- Rendering engine: Skia (`@napi-rs/canvas`) instead of sharp/librsvg (SVG) + node-canvas/Pango (text):
  "expect tiny pixel differences, mostly in text". Defaults match the old ones (svgScale 1, font
  `bold 16px sans-serif`, padding 3, white text, centre/centre), so converted definitions look the same.
- Recolouring differs: old `svgForeColor` replaced the literal text `#848484` anywhere; new `colors` matches
  by value in paint values only (fill, stroke, stop-color, ...). For built-ins, `colors: {"#848484": c}`
  also recolours `currentColor` (the key matches the default `color`). Our own SVGs (`urem2/icons/`:
  netflix, prime, youtube) contain no `#848484`, so nothing changes for them.
- Browser: the core works in a browser (`browserBackend`), which removes one blocker for a browser bake
  (the other: `images.js`'s `composeOverBlack`/`svgLayer`/`centreImage` and `tools/test-icons` still use
  sharp). Out of scope here; worth a line in the docs that say "node-buttonImage uses sharp".

### Every use of node-buttonImage in the repo (excluding node_modules, the submodule itself, complete-*.md)

Build / packaging:
- `.gitmodules:4-6`: the `node-buttonImage` submodule (path `node-buttonImage`). Removal: `git submodule
  deinit -f node-buttonImage`, `git rm node-buttonImage`, delete `.git/modules/node-buttonImage`.
- `host/package.json:22`: `"@toptensoftware/node-buttonImage": "file:../node-buttonImage"`; also
  `engines.node ">=22"` (button-face needs >= 24; local Node is v24.14.0). `host/.npmrc` `install-links=true`
  stays (rle565 is still `file:`). `host/package-lock.json` will change (drops `canvas`, adds `@napi-rs/canvas`).
- `tools/test-icons/package.json:10`: same dep (`file:../../node-buttonImage`); `.npmrc` `install-links=true`
  stays (rle565). Its lock changes too.
- Installed copies to delete before `npm install`: `host/node_modules/@toptensoftware/node-buttonImage`,
  `tools/test-icons/node_modules/@toptensoftware/node-buttonImage`.
- `cli/`, `examples/`, `urem2/` have no deps (import `../host/index.js` by relative path), so they follow host.

Code:
- `host/src/images.js:2,12,29-39`: `import { ButtonImage }`; `renderButtonImage(def, w, h=w)` =
  `new ButtonImage(def).render(w, h, 4)` -> RGBA bytes; `buttonLayer(def, w, h)` wraps it as a layer
  `{ rgba, width, height }`. Public library exports (via `host/index.js:4`). New body:
  `(await render(def, w, h, { channels: 4 })).data` from `@toptensoftware/button-face/node`.
  `svgLayer`, `centreImage`, `composeOverBlack`, `encodeImage` use sharp, not node-buttonImage: unchanged.
- `host/src/bake/render-node.js:2,7,19`: stage 2 renderer; calls `renderButtonImage(def, w, h)` per layer
  definition (cache keyed by `stable(def)`, fine for any plain object). Only a comment needs changing.
- `host/src/bake/host-node.js:8-9,16`: `iconDir` via `import.meta.resolve("@toptensoftware/node-buttonImage")`
  + `images/`; `hasIcon: name => fs.existsSync(iconDir/name.svg)`. Replace with `Object.hasOwn(icons, name)`.
- `host/src/bake/look.js`: **the main place legacy definitions are built** (header comment lines 1-5).
  - `faceDef` (lines 131-150): `{ backColor: "transparent", svgCode: faceSvg(...) }` (face is our own SVG
    string: gradient, optional border stroke, shade, `pressed-overlay` rect).
  - `foregroundDef` (155-183): `backColor: "transparent"`, `virtualSize` (72 per 128 px when not 128x128),
    `svgFile` (`#name` or resolved absolute path), `svgForeColor` (default white), `svgScale`, `svgText`,
    `svgTranslate` (default y -7 with a label), `text`, `textColor`, `textAlign`, `textVAlign`, `font`,
    `textPadding` [9, 4], `textTranslate`, `textLineSpacing`. `VIRTUAL_SIZE = 72` (111).
  - `overrideLayer` (186-197): the `image` escape hatch: `{ backColor: "transparent", ...userSettings }`,
    `svgFile` resolved/checked. **This is the only place the old settings shape reaches users.**
  - `checkIcon` (199-212): `#name` via `rc.host.hasIcon`; anything else is a file path (so a Material
    `@name` would be "file not found" today).
  - The look validator (`color` = any non-empty string, lines 16-43) doesn't validate CSS colours; button-face
    rejects invalid ones at render time, i.e. in stage 2 without a JSON path. Consider `parseColor` in stage 1.
- `host/src/bake/files.js:20`: comment "(node-buttonImage + rle565)".
- `tools/test-icons/gen.js:1,15,59-64`: `new ButtonImage({})`, sets `backColor = "transparent"`,
  `svgFile = "#"+name`, `svgForeColor = color`, `render(112, 112, 4)` -> Buffer, then sharp crops to 80.
  New: `render({ layers: [{ type: "svg", src: "#"+name, colors: {"#848484": color} }] }, 112, 112,
  { channels: 4 })` and wrap `data` in a Buffer for sharp. Everything else in gen.js is sharp.
- `examples/demo.js:16,49-61,80`: `iconDef(svgFile, label, color)` builds a legacy def; `buttonLayer(icon, fw, fh)`.
- `examples/bench.js:8,56-57`: one legacy def passed to `buttonLayer`.
- `scratch/demo-render.mjs`: gitignored throwaway; ignore.

Tests:
- `host/test/images.test.js:2,9,39-47`: one legacy def (`backColor, svgFile #heart, svgForeColor, text,
  textColor`) through `renderButtonImage`/`buttonLayer` (checks sizes only).
- `host/test/entries.test.js:48`: export list includes `renderButtonImage`, `buttonLayer` (only if renamed).
- `host/test/bake/look.test.js:3,38-39,47-48,58-174,208-241`: ~60 asserts on legacy field names of the
  generated layer definitions (`svgCode`, `svgFile`, `svgForeColor`, `svgScale`, `svgTranslate`, `svgText`,
  `backColor`, `text*`, `virtualSize`), plus the `image` escape hatch test (165-179:
  `{ normalFg: { svgCode: "<svg/>", textColor: "red" }, pressedBg: null, pressedFg: "pic.svg" }`).
  Note line 96 asserts `svgFile "#play_arrow"` (not a real icon; the test host has no `hasIcon`).
- `host/test/bake/keys.test.js:149-150`: `svgFile "#add"`, `svgForeColor "red"` on group members.
- `urem2/layout/look.test.mjs:52-71`: `svgCode` regexes, `svgScale 0.9`, `svgForeColor "orange"`, `.text`.
- `host/test/bake/render-real.test.js:1`: comment; the tests render real images but only check sizes,
  opacity and a few "not black"/"differs" pixels: should pass unchanged with Skia.
- `host/test/bake/preview.test.js`: compares the preview with the same renderer's output (self-consistent).
- No golden-image/snapshot tests anywhere (host, cli, examples, urem2).
- node-buttonImage's own tests (`wrapLines.test.js`, "label wrapping" in CLAUDE.md line 56) go away with
  the submodule; button-face has its own `test/wrapLines.test.js`.

Docs (to update; never touch `complete-*.md`):
- `CLAUDE.md:56` (node-buttonImage `npm test`), `:60` (install-links note names node-buttonImage), `:75`
  (repository layout line), `:184` (Host-side key images: node-buttonImage, `svgFile: "#name"`).
- `docs/layout-format.md:8` (built-in icon names / "not Material names"), `:29` (`#name` = node-buttonImage
  icon), `:214-217,219-220,222` (look properties described as `svgForeColor`, `svgScale`, `svgTranslate`,
  `svgText`, `textColor`, `textAlign/VAlign`, `textPadding/Translate/LineSpacing`), `:224` (`image` escape
  hatch = raw node-buttonImage settings), `:226` (virtual pixels = node-buttonImage's `virtualSize`), `:519`
  (open point).
- `docs/standalone.md:103` (pipeline diagram), `:117`, `:119` ("node-buttonImage uses sharp, which does not
  work in a browser" - now partly untrue), `:279`.
- `docs/screen-buffers.md:49` (historical, partly superseded; one mention of the icons' origin).
- `host/README.md:24, 31, 35, 72`.
- `urem2/README.md:126` ("Icons are node-buttonImage built-ins").
- `urem2/layout/activities/hdd.mjs:19` (comment).
- Other open wip docs mention it (`wip/wip-versioning-deployment.md`: npm name `@toptensoftware/node-buttonImage`
  can't be published due to capitals, button-face's lowercase name solves that; `wip/wip-docs-licensing.md`:
  icon licences now come from button-face's README/LICENSE). Not this job's to edit; the coordinator should
  mention the overlap to the user.
- No skills (`.claude/` has only `settings.local.json`) or memory files mention node-buttonImage.

### Old API -> new API

| Old (node-buttonImage) | New (button-face) |
|---|---|
| `import { ButtonImage } from "@toptensoftware/node-buttonImage"` | `import { render, icons } from "@toptensoftware/button-face/node"` |
| `await new ButtonImage(def).render(w, h, 4)` -> Buffer RGBA | `(await render(def, w, h, { channels: 4 })).data` -> Uint8Array RGBA |
| flat settings object | `{ virtualSize?, layers: [...] }` |
| `backColor: "transparent"` | omit the fill layer (RGBA with no fill = transparent) or `{ type: "fill", color: "transparent" }` |
| `svgCode` / `svgFile` | `{ type: "svg", svg }` / `{ type: "svg", src }` |
| `svgForeColor: c` | `colors: { "#848484": c }` (also works for user SVGs drawn in #848484) or `color: c` (currentColor only) |
| `svgScale`, `svgTranslate`, `svgText` | `scale`, `translate`, `text` |
| `text`, `textColor`, `textAlign`, `textVAlign`, `font`, `textPadding`, `textTranslate`, `textLineSpacing` | `{ type: "text", text, color, align, valign, font, padding, translate, lineSpacing }` |
| built-ins `images/<name>.svg` on disk | `icons` export (same 42 names, some redrawn) |
| sync file read when `svgFile` set | `src` resolved at render (`baseDir` or absolute path; we already pass absolute paths) |

Nothing we rely on is missing. New capabilities: several icons/texts per key, `tint`, `opacity`, Material
Symbols via `@name` (downloads at bake time, cached in tmpdir; offline uncached icons fail).

### Item 5: existing button definitions

Finding: **users never write node-buttonImage definitions.** Layouts (urem2: ~48 `color`, 31 `label`,
icons via helpers; `examples/*.json|mjs`; `firmware/test/data/sample_layout.json`) use ControlBlock's own
look properties (`icon`, `iconColor`, `iconScale`, `iconOffset`, `label`, `font`, `labelVAlign`, ...), which
`look.js` turns into legacy definitions. So there is nothing to convert in urem2 or the sample layouts. The
old shape exists only in:
1. `look.js` (`faceDef`, `foregroundDef`, `overrideLayer`): ~50 lines generating legacy objects.
2. Tests asserting those generated objects: `look.test.js` (~60 asserts), `keys.test.js` (2),
   `urem2/layout/look.test.mjs` (~10), `images.test.js` (1 def).
3. `examples/demo.js` (`iconDef`), `examples/bench.js` (1 def), `tools/test-icons/gen.js` (1 instance).
4. The user-facing `image` escape hatch (`{ normalBg, normalFg, pressedBg, pressedFg }` of raw settings),
   used by **no** layout (only `look.test.js:165-179`), and documented in `docs/layout-format.md:224`.
   `host/src/bake/schema.js` (the blob schema) does not expose definitions at all.

Assessment: converting is small and mechanical and touches the same files this job must touch anyway, so
**do it in this job, not a separate IPI**, and don't use `convertLegacyDefinition` at runtime:
- `look.js` emits button-face definitions directly: face = `{ layers: [{ type: "svg", svg: faceSvg(...) }] }`;
  foreground = `{ virtualSize?, layers: [svg layer?, text layer?] }` with the same defaults
  (`colors: {"#848484": iconColor ?? "white"}`, translate y -7 with a label, padding [9, 4], fonts).
- Update the tests' asserts to the layer shape (one mechanical pass; e.g. a `layerOfType(def, "svg")` helper).
  A scratch script with `convertLegacyDefinition` isn't worth it: the definitions are generated by code, not
  stored as data.
- `demo.js`/`bench.js`/`gen.js`: rewrite their 3 definitions by hand.
- `image` escape hatch: take button-face definitions (`{ layers: [...] }`) or an `.svg` path; resolve
  relative `src` paths against the layout dir. Because there are no users, a clean switch is fine
  (render() already rejects a legacy object with a helpful hint). Optionally run user values through
  `convertLegacyDefinition` for leniency - recommended against (keeps the old shape alive in docs).
- `renderButtonImage`/`buttonLayer` keep their names and signatures but take new definitions.

### Risks

- **Pixel differences** (Skia vs librsvg/Pango; 20 redrawn icons; text measured by a different engine, so
  wrap points and label widths can shift, e.g. urem2's wrapped labels). No test compares pixels exactly, so
  tests should pass; the look must be checked by eye (`node cli/cli.js render` PNG mockups of urem2 and
  `examples/key-shapes.mjs`, then on the hardware). Baked layouts get new image bytes/ids (harmless: a
  re-upload replaces them).
- **Fonts**: `sans-serif` resolves through Skia's font manager rather than Pango/fontconfig; on Windows
  probably Arial-ish either way, but check bold label sizes look the same.
- **Committed binaries**: `firmware/test/data/sample_layout.bin` doesn't need re-baking (no schema change);
  if re-baked its image bytes change but the firmware tests only check structure/sizes/opacity.
  `firmware/assets/test_keys.bin` is only regenerated by running `tools/test-icons` (`npm run build`), which
  also rewrites `ui.bin` (font and pills; no button-face involvement, should be byte-identical). Firmware
  host tests (`firmware/test`, `mk run`) and `RUN_STRESS` read `test_keys.bin`; regenerating changes icon
  pixels/sizes slightly. Either leave the committed bins alone or regenerate and run `mk run`.
- **npm/git dependency**: a `github:` dep needs `git` on PATH for npm and network; the lock pins the commit,
  so picking up a newer button-face needs `npm install git+https://github.com/toptensoftware/button-face.git` (or `npm update`)
  in both `host/` and `tools/test-icons/`. No build step needed (plain ESM, `files` has `src/`).
  `@napi-rs/canvas` ships a prebuilt win32-x64 binary (no compiler). The old stale-copy dance
  (bump version, delete folder) no longer applies to the image renderer; CLAUDE.md line 60 should say so
  (it still applies to `rle565/encoder`).
- **Node >= 24**: bump `host/package.json` `engines` (and mention in docs); `cli`/`examples` run on host's.
- **Material icons**: `@...` in a layout `icon` currently fails `checkIcon` ("file not found"), and a
  `@`-prefixed relative file path would now be routed to Material. Supporting `@` needs `checkIcon` to
  accept/validate it and network at bake time.
- `canvas` (node-canvas) drops out of host's tree; sharp stays (images.js, test-icons).

### Planning notes

- **Why look.js and images.js switch in one step (step 4):** the old renderer given a `{ layers }` object
  renders it as an empty settings object (blank), and button-face rejects a legacy object. Switching either
  side alone breaks `render-real.test.js` / `preview.test.js` ("not black" checks) and the urem2/examples bake
  checks. Doing both together avoids a temporary `convertLegacyDefinition` shim.
- **Validator mismatches found:** button-face rejects `scale <= 0` (`validatePositive("svg", "scale")`) and
  `lineSpacing <= 0` (`validatePositive("text", "lineSpacing")`), but `look.js` allows `iconScale` and
  `labelLineSpacing` `min: 0`. A value of 0 would now pass stage 1 and fail in stage 2 without a JSON path:
  tighten both to "> 0" in stage 1 (step 4). `align` (left/center/right) and `valign` (top/center/bottom)
  match look.js's `oneOf` lists exactly. Colours: button-face throws on an invalid colour at render; step 6
  (optional) checks them in stage 1 with `parseColor`.
- **`hasIcon` coverage:** `host/test/bake/helpers.js` fakes `hasIcon`; the real one (`host-node.js`) is used
  by `urem2/test/resolve.mjs` (`createNodeHost(UREM2_DIR)`), so the urem2 tests already check every urem2 icon
  name against the real built-in list. No host test covers `createNodeHost().hasIcon` directly: step 3 adds one.
- **Before/after PNGs:** step 1 renders "before" mockups with the old renderer (`node cli/cli.js render`,
  `--pressed` too) into `scratch/` (gitignored) so step 11 can compare them with the new renderer's output.
- **`image` override, new shape (step 5):** a string stays an `.svg` path (-> `{ layers: [{ type: "svg",
  src: <absolute path> }] }`); an object must be a button-face definition (`{ virtualSize?, layers: [...] }`,
  checked in stage 1: `layers` an array of objects with a `type`); each svg layer's `src` that doesn't start
  with `#` or `@` is resolved against the layout dir and checked to exist (error path
  `image.<layer>.layers[i].src`). An object with no `layers` (e.g. a legacy `{ svgCode }`) gets a stage-1
  error naming the new shape. Not deep-validated beyond that: button-face validates at render.
- **cli engines:** `cli/package.json` also says `node >=22` and runs the host library, so it is bumped with
  host (step 2). `tools/pad-sim` (`>=22`) doesn't use the library: unchanged. `rle565/encoder` is a submodule: unchanged.
- No CI config exists (`.github/` absent), so nothing else pins the Node version.

### Implement log

- **Step 1 baseline (2026-11-15, old renderer):** host `npm test` 289 pass; cli `npm test` 31; examples
  `node --test "examples/**/*.test.mjs"` 35; urem2 `node --test "urem2/**/*.test.mjs"` 175; binpack 81. All 0 fail.
  "Before" PNGs in `scratch/bf-before/`: `urem2/` and `urem2-pressed/` (12 pages each), `key-shapes/` and
  `key-shapes-pressed/` (4 each).
- **Step 2:** `npm install` in `host/` added button-face + `@napi-rs/canvas` (+ win32-x64-msvc binary) with
  no trouble (git on PATH). The lock records it as
  `git+ssh://git@github.com/toptensoftware/button-face.git#728ea56...` (npm's default for a `github:` spec):
  an install from the lock on a machine without GitHub SSH keys may fail; switching the spec to
  `git+https://github.com/toptensoftware/button-face.git` would avoid that (see Flagged assumptions).
  `npm audit` reports 3 high in `sharp` (pre-existing, not from button-face). host 289, cli 31: unchanged.
- **Step 3:** new `host/test/bake/host-node.test.js` (`hasIcon("heart")` true, `"no_such_icon"` and `"toString"`
  false; it passed on the old file-based lookup too, as a behaviour test should). `host-node.js` now uses
  `Object.hasOwn(icons, name)` from `@toptensoftware/button-face/node` (no more `node-buttonImage` import there).
  host 290 (289 + 1), urem2 175, examples 35: all pass, so every urem2/examples icon name exists in button-face.
- **Step 4:** tests first (18 host tests failed as expected, then pass). `images.js` `renderButtonImage` =
  `(await render(def, w, h, { channels: 4 })).data` (already a `Uint8Array` view: no copy). `look.js`: face =
  `{ layers: [{ type: "svg", svg }] }`; foreground = `{ virtualSize?, layers: [svg?, text?] }` (no fill layer:
  transparent), icon colour as `colors: { "#848484": c }` (`ICON_GREY` constant); `iconScale`/`labelLineSpacing`
  use a local `positive` checker ("must be greater than 0"). Test helpers: `layerOfType`/`svgOf`/`textOf`/`faceSvg`/
  `iconColor` in `look.test.js`, `layerOfType`/`faceSvg` in `urem2/layout/look.test.mjs`, an inline `ofType` in
  `keys.test.js`. `images.test.js` now checks the render isn't blank (>10% pixels drawn, corner transparent), a fill
  layer, and that a legacy def rejects (new test). Counts: host **291** (290 + the legacy-reject test; the
  zero-scale/line-spacing asserts went into the existing "look property validation" test), cli 31, examples 35,
  urem2 175: all 0 fail. `overrideLayer` (the `image` escape hatch) is untouched and still emits the legacy shape:
  its test checks only the generated object, so it passes; no layout uses it; step 5 converts it.
- **Step 4 visual check** (`scratch/bf-step4/`: urem2 normal, key-shapes normal and pressed, vs `scratch/bf-before/`):
  no gross regressions: faces, borders, icon colours and sizes match; labels render. Text differs as expected: Skia's
  `sans-serif` is Arial-like (Pango's looked DejaVu-like), slightly narrower, and labels sit ~4-6 px higher
  (e.g. "Sub-titles", "Volume", the key-shapes labels). For the user to judge at step 11.
- **Step 5:** tests first (4 look tests failed, then pass). `look.js`: the `image` validator checks an object is
  `{ layers: [{ type: string, ... }] }` (errors at `image.<layer>`, `.layers` or `.layers[i]`; a legacy object gets
  "expected a button-face definition: { virtualSize?, layers: [{ type, ... }] }"; other values "an .svg path, a
  button-face definition or null"). `overrideLayer`: a path -> `{ layers: [{ type: "svg", src }] }`; a definition is
  copied (layout object untouched) with svg layers' file `src` resolved and checked (`#`/`@` left alone). New shared
  `resolveFile(src, rc, path)` also used by `checkIcon` (DRY; same "file not found" message). A missing `.svg` path's
  error is now at `image.<layer>` (was `image.<layer>.svgFile`). New tests: 2 in `look.test.js` (definition src
  resolution, shape errors; the old two rewritten), 1 in `render-real.test.js` (an override definition renders over
  the face). `docs/layout-format.md`: the `image` row (with example + README link) and the top "not yet" bullet.
  Counts: host **294** (291 + 3), cli 31, urem2 175, examples 35: all 0 fail.
- **Step 7:** `tools/test-icons/package.json` now depends on `"@toptensoftware/button-face": "github:toptensoftware/button-face"`
  (same spec as host; the lock again resolves it to `git+ssh://...#728ea56`). `npm install` removed 34 packages
  (node-buttonImage's tree incl. `canvas`) and added 3 (button-face, `@napi-rs/canvas` + win32-x64-msvc); `npm audit`:
  2 high (sharp, pre-existing). `gen.js`: `render({ layers: [{ type: "svg", src: "#"+name, colors: { "#848484": color } }] },
  112, 112, { channels: 4 })` from `@toptensoftware/button-face/node`, `data` wrapped with `Buffer.from(buffer, offset,
  length)` for sharp; header comment names button-face. No automated test for gen.js (verified by running it).
  `npm run build`: **`ui.bin` byte-identical** (68644 B, no git diff); **`test_keys.bin` changed** 166376 -> 166580 B
  (+204 B: icon pixels from Skia and the redrawn built-ins; same blob count: 8 faces + 14 icons + 15 1U + 5 2U keys).
  Regenerated bin kept (per Flagged assumptions). `firmware/test` `mk run` (debug) and `mk config=release run`: both
  **789 tests, all passed** (test counts don't depend on the bin's pixels). On-device check of the new `test_keys.bin`
  (a `RUN_STRESS` build) is part of step 11.
- **Step 8:** `examples/demo.js` `iconDef(icon, label, color)` -> `{ layers: [svg?, text] }` (svg: `src "#"+icon`,
  `colors { "#848484": color }`, `translate { x: 0, y: -7 }`; text: white, centre, `valign` bottom/centre, fonts
  11/15 px, `padding [9, 4]`; no svg layer for text-only keys); `examples/bench.js` its one def the same way.
  **Warning: demo.js and bench.js need a device; they are not covered by any automated test.** Checked with
  `scratch/bf-demo-defs.mjs` (copies of the defs rendered through `buttonLayer`: all 14 demo icons, the 2U, half,
  clock and bench defs render at the right size with drawn pixels) and `node --check` on both. Removed
  `@toptensoftware/node-buttonImage` from `host/package.json`, deleted its `node_modules` copy, `npm install`: the
  lock has no `node-buttonImage` or `canvas` left. Comments fixed: `demo.js` header, `render-real.test.js` header,
  `urem2/layout/activities/hdd.mjs:19`. Grep (host, cli, examples, urem2, tools, binpack; excl. node_modules) for
  `node-buttonImage|ButtonImage|svgFile|svgCode|svgForeColor|backColor`: only `renderButtonImage` (kept name) and the
  deliberate legacy-reject test in `look.test.js` remain (docs are step 9). Counts: host 294, cli 31, examples 35,
  urem2 175, binpack 81: all 0 fail (same as step 7).
- **Step 9 (docs):** `docs/layout-format.md` (built-in icons are button-face's, `@name` not accepted in `icon`; `#name` in
  File basics; the look table rows now say what each property does, with defaults, instead of `svg*`/`text*` names;
  a new paragraph saying how the look becomes button-face layers; `virtualSize`; the open point). `docs/standalone.md`
  (pipeline diagram, stage 2 renderer, the mapping line, the browser constraint: button-face has a browser backend,
  `images.js`'s sharp and the bake's `Buffer` still block a browser bake; Still open x2). `docs/screen-buffers.md:49`
  (historical: kept "node-buttonImage's built-in icons", added that they're now rendered from button-face's).
  `host/README.md` (Node only line, Node >= 24, dependency paragraph: rle565 `file:` dance, button-face from GitHub
  pinned by the lock, the `buttonLayer` example as a layer definition, `buttonLayer`/`renderButtonImage` description).
  `urem2/README.md` (icons are button-face built-ins). `cli/README.md`, `examples/README.md`: no Node version or
  renderer mention, unchanged. No skills mention it. host `npm test` 294 pass.
  **Remaining grep hits** (`buttonImage|node-buttonImage|svgFile|backColor`, excl. node_modules, .git, the submodule,
  complete-*.md, scratch): `.gitmodules:4-6` (step 10 removes); `CLAUDE.md:56,60,75,184` (proposed changes below,
  not edited); `docs/layout-format.md:224` (deliberate: says a node-buttonImage settings object is an error);
  `docs/screen-buffers.md:49` (historical, accurate); `docs/standalone.md:119` (history: "it replaced node-buttonImage");
  `host/test/bake/look.test.js:197`, `host/test/images.test.js:63-64` (the legacy-reject tests); `renderButtonImage`
  (kept name); this job doc. Other open wip docs, left alone for the user: `wip/wip-docs-licensing.md:28,59,62,75,87,91,226,238`
  (licence table, built-in icon credits, copyright brackets: now button-face's README/LICENSE) and
  `wip/wip-versioning-deployment.md:14,31,63,67,96,182,185,186,206,218,226,236,261` (dependency tables, `file:` deps,
  `canvas`, submodule checkout list, `import.meta.resolve` icon lookup, the npm-name capitals problem, workspaces).

- **Dependency spec (coordinator, after step 9, user's call):** `host/package.json` and
  `tools/test-icons/package.json` now say `"git+https://github.com/toptensoftware/button-face.git"` (was
  `github:...`); reinstalled in both, host 294 pass. npm still writes the lock's `resolved` as
  `git+ssh://git@github.com/...#728ea56` (its normal form for GitHub); for a public repo npm fetches it over
  https, so a machine without SSH keys should still install (not tested on another machine). `host/README.md` and
  the proposal below updated to match.

- **Step 10 (coordinator, user approved):** the submodule had no local changes or unpushed commits (HEAD
  d422bcc). `git submodule deinit -f node-buttonImage`, `git rm -f node-buttonImage`, `.git/modules/node-buttonImage`
  deleted; `.gitmodules` entry gone (staged). Re-run after: host 294, cli 31, examples 35, urem2 175, 0 fail;
  `tools/test-icons` `npm run build` OK, `ui.bin` still unchanged.

### Proposed CLAUDE.md changes (step 9; approved by the user and applied to CLAUDE.md 2026-11-15, all six incl. the optional ones)

1. **Host library tests / CLI and binpack tests (line ~56):** old: "From `binpack/`: `node --test test.js`. From
   `node-buttonImage/`: `npm test` (label wrapping)." -> new: "From `binpack/`: `node --test test.js`."
2. **Stale copies (line ~60):** old: "`host` and `tools/test-icons` copy their `file:` dependencies (`install-links=true`
   in their `.npmrc`), so edits to `node-buttonImage` (or `rle565/encoder`) don't reach them by themselves: bump the
   library's version, delete its folder under each project's `node_modules/@toptensoftware/` (or `node_modules/rle565`),
   then `npm install` there." -> new: "`host` and `tools/test-icons` copy their `file:` dependency `rle565/encoder`
   (`install-links=true` in their `.npmrc`), so edits to it don't reach them by themselves: bump its version, delete
   `node_modules/rle565` in each project, then `npm install` there. The key renderer, button-face
   (`@toptensoftware/button-face`, `git+https://github.com/toptensoftware/button-face.git`), is pinned to a commit by each lock: to pick
   up a newer one run `npm install git+https://github.com/toptensoftware/button-face.git` in both `host/` and `tools/test-icons/`. The
   host library, the CLI and button-face need Node 24 or later." (Rest of the paragraph unchanged.)
3. **Repository Layout (line ~75):** delete the line "`node-buttonImage/  Submodule: host-side key image rendering`".
   Optionally extend the `host/` line: "... and the layout bake (src/bake/: layout file to BPAK blob, upload; key
   images rendered with button-face, a GitHub npm dependency)".
4. **Communication, "Host-side key images" (line ~184):** old: "[node-buttonImage](https://github.com/toptensoftware/node-buttonImage)
   renders faces and icons from SVG to RGBA (stock icons `svgFile: \"#name\"` make handy test content);" -> new:
   "[button-face](https://github.com/toptensoftware/button-face) renders layer definitions (`{ virtualSize?, layers:
   [fill | svg | text] }`) to RGBA (built-in icons `{ type: \"svg\", src: \"#name\" }` make handy test content; the
   bake's `look.js` builds the definitions from a layout's look properties);" (rest unchanged).
5. **Rendering section (optional):** after the rle565 paragraph, "Host side, key faces, icons and labels are
   rendered by button-face (Skia via `@napi-rs/canvas`) and composed with sharp (`host/src/images.js`)."
6. **Open Items, "Bake, left for later" (optional):** "... and a browser renderer" -> "... and a browser bake
   (button-face has a browser backend; `images.js`'s sharp and the `Buffer` global still block it)". The existing
   "The bake core ... uses Node's `Buffer` global" sentence stays.

## Flagged assumptions

- Assumed the old-format conversion (item 5) is best done inside this job by changing `look.js` to emit
  layer definitions directly, rather than a separate IPI or runtime `convertLegacyDefinition` (reasoning in
  Notes, "Item 5"). The user said they'd prefer not to switch everything; this recommendation is because
  no stored data uses the old shape, only code and tests. **Confirmed by the user 2026-11-15: convert
  everything to the new format.**
- Assumed the `image` escape hatch may change shape (to button-face definitions) without a compatibility
  path, since no layout uses it.
- Assumed `renderButtonImage` / `buttonLayer` keep their names (fewer changes to exports, docs and
  `entries.test.js`).
- Assumed Material Symbols (`@name`) support in layout `icon` is **out of scope** (docs keep saying built-ins
  are `#name`; `checkIcon` keeps rejecting anything else that isn't a file). Easy follow-up if wanted.
- Assumed the dependency is spelled `"github:toptensoftware/button-face"` without a pinned `#sha`/tag (no
  tags exist; the lock file pins the commit).
- (Step 2) Kept the `github:` spec although the lock resolves it to `git+ssh://` (works here; this machine
  has SSH access). If installs elsewhere (no SSH key) matter, change the spec to
  `git+https://github.com/toptensoftware/button-face.git` in `host/` and `tools/test-icons/` and re-install.
- Assumed the committed firmware bins (`sample_layout.bin`, `test_keys.bin`, `ui.bin`) are left as they are
  unless the Plan decides gen.js must be re-run to prove the port (then re-run `mk run`).
- Assumed `host/package.json` `engines.node` goes to `>=24` to match button-face.
- (Plan) `cli/package.json` `engines.node` also goes to `>=24` (it runs the host library).
- (Plan) `look.js` uses `colors: { "#848484": iconColor ?? "white" }` (not `color:`) for the icon colour, so
  user SVGs drawn in `#848484` keep recolouring as before.
- (Plan) `iconScale` and `labelLineSpacing` become "> 0" in stage 1 (were `min: 0`), matching button-face.
  A layout using 0 would now be a bake error (none does).
- (Plan) The firmware bins are **regenerated** in step 7 (gen.js must run once to prove its port) and kept,
  with `mk run` re-run; `ui.bin` is expected byte-identical (if not, stop and record why). If the user prefers
  the committed bins untouched, step 7 ends with `git checkout firmware/assets/test_keys.bin` instead.
- (Step 4) The `positive` (> 0) checker is local to `look.js` rather than a new `t.positive` in `check.js` (only
  two users). The foreground definition has no `fill` layer (button-face leaves the background transparent).
- (Step 5) In an `image` definition, an svg layer's `src` starting with `@` is passed to button-face as a Material
  Symbol (downloaded at bake time) without a stage-1 check, although layout `icon` still rejects `@` (out of scope,
  above). Only svg `src` paths are resolved; no other layer type takes a file.
- (Plan) Step 6 (stage-1 colour checks with `parseColor`) is optional polish the user may drop.
- (Plan) The submodule removal (step 10) is hard to reverse locally (`.git/modules` is deleted): the
  implementing agent asks the user before running it.
- (Plan) CLAUDE.md is not edited by an agent: step 9 writes the proposed wording into Notes for the user to
  approve (Rule 4); the edit is applied only after confirmation.

## Plan

- [x] 1. **Baseline.** Before any change, run every suite and record the pass counts in Notes: `host/` `npm test`,
  `cli/` `npm test`, `node --test "examples/**/*.test.mjs"`, `node --test "urem2/**/*.test.mjs"` (repo root),
  `binpack/` `node --test test.js`. Render "before" mockups with the old renderer into `scratch/bf-before/`:
  `node cli/cli.js render urem2/urem2.mjs -o scratch/bf-before/urem2`, the same with `--pressed` into
  `.../urem2-pressed`, and `examples/key-shapes.mjs` into `.../key-shapes` (and `--pressed`). No code changes.
- [x] 2. **Add button-face alongside node-buttonImage; bump Node.** `host/package.json`: add
  `"@toptensoftware/button-face": "github:toptensoftware/button-face"` (keep the old dep for now),
  `engines.node` `">=24"`; `cli/package.json` `engines.node` `">=24"`. In `host/`: `npm install`
  (`.npmrc` `install-links=true` stays: rle565 is still `file:`). Check `node_modules/@toptensoftware/button-face/src/`
  and `@napi-rs/canvas` installed (prebuilt win32-x64, needs git on PATH for the `github:` spec). No code
  changes; run host `npm test` and cli `npm test` (same counts as step 1). Files: `host/package.json`,
  `host/package-lock.json`, `cli/package.json`.
- [x] 3. **Built-in icon lookup from button-face.** Test first: add a host test (e.g. in
  `host/test/bake/render-real.test.js` or a new `host/test/bake/host-node.test.js`) that `createNodeHost(dir)`'s
  `hasIcon("heart")` is true and `hasIcon("no_such_icon")` false. Then `host/src/bake/host-node.js`: drop
  `iconDir`/`import.meta.resolve`, `hasIcon: name => Object.hasOwn(icons, name)` with `icons` from
  `@toptensoftware/button-face/node`. Verify: host `npm test`, `node --test "urem2/**/*.test.mjs"` (checks every
  urem2 icon name against the real list), `node --test "examples/**/*.test.mjs"`.
- [x] 4. **Emit and render button-face definitions** (one step: see Planning notes). Tests first:
  - `host/test/images.test.js`: the def becomes `{ layers: [{ type: "fill", color: ... }, { type: "svg", src: "#heart",
    colors: { "#848484": ... } }, { type: "text", text, color }] }`; add asserts that the output has
    non-transparent pixels (so a blank render fails) and that a legacy def (`{ backColor, svgFile }`) rejects.
  - `host/test/bake/look.test.js`: add a small helper (`layerOfType(def, "svg" | "text")`) and rewrite the ~60
    legacy-field asserts to the layer shape (`svgCode` -> svg layer `svg`, `svgFile` -> `src`, `svgForeColor` ->
    `colors["#848484"]`, `svgScale/svgTranslate/svgText` -> `scale/translate/text`, `text*`/`font` -> text layer
    fields, `backColor` gone, `virtualSize` unchanged). Leave the `image` override test (lines ~165-179) for
    step 5 (mark it `todo` if it fails here). Add tests that `iconScale: 0` and `labelLineSpacing: 0` are stage-1
    errors.
  - `host/test/bake/keys.test.js:149-150` and `urem2/layout/look.test.mjs:52-71`: same mapping.
  Then code: `host/src/images.js` (`renderButtonImage` = `(await render(def, w, h, { channels: 4 })).data` from
  `@toptensoftware/button-face/node`, names and signatures kept; `buttonLayer` unchanged around it);
  `host/src/bake/look.js` (`faceDef` -> `{ layers: [{ type: "svg", svg: faceSvg(...) }] }`; `foregroundDef` ->
  `{ virtualSize?, layers: [svg layer?, text layer?] }` with the same defaults; `iconScale`/`labelLineSpacing`
  > 0; header comment and `VIRTUAL_SIZE` comment name button-face). Comments only: `host/src/bake/render-node.js`,
  `host/src/bake/files.js:20`. Verify: host `npm test` (incl. `render-real`, `preview`), cli `npm test`,
  `node --test "examples/**/*.test.mjs"`, `node --test "urem2/**/*.test.mjs"`.
- [x] 5. **`image` override takes button-face definitions.** Tests first in `host/test/bake/look.test.js`: an
  `.svg` path -> `{ layers: [{ type: "svg", src: <resolved> }] }`; an object with `layers` passes through with
  relative svg `src` resolved (and `#`/`@` left alone); a missing file is an error at
  `image.<layer>.layers[i].src`; an object without `layers` (legacy `{ svgCode }`) is a stage-1 error; `null`
  still clears. Add one `render-real` case rendering a key with an `image` override (catches a shape button-face
  rejects). Code: `host/src/bake/look.js` (`layer` validator message "an .svg path, a button-face definition or
  null", `overrideLayer`). Doc: the `image` paragraph in `docs/layout-format.md` (~line 224: raw node-buttonImage
  settings -> button-face definition, with a short example and a link to button-face's README). Verify: host
  `npm test`, cli `npm test`.
- [x] 6. **SKIPPED (user, 2026-11-15).** ~~**(Optional) Stage-1 colour checks.**~~ Tests first (`look.test.js`): `color`, `iconColor`, `labelColor`,
  `face.top/bottom/border` with an invalid CSS colour (e.g. `"notacolour"`) give a stage-1 error at the property's
  path; valid forms (`#rgb`, `#rrggbb`, names, `rgba()`, `transparent`) pass. Code: `look.js`'s `color` checker
  uses button-face's `parseColor`. Verify: host `npm test`, urem2 and examples tests (every real layout colour
  must still pass).
- [x] 7. **tools/test-icons on button-face.** `tools/test-icons/package.json`: replace the `file:` node-buttonImage
  dep with `"@toptensoftware/button-face": "github:toptensoftware/button-face"`; delete
  `tools/test-icons/node_modules/@toptensoftware/node-buttonImage`; `npm install` (`.npmrc` stays for rle565).
  `gen.js`: `render({ layers: [{ type: "svg", src: "#" + name, colors: { "#848484": color } }] }, 112, 112,
  { channels: 4 })`, `Buffer.from(data.buffer, data.byteOffset, data.byteLength)` for sharp; header comment.
  No automated test exists for gen.js: verify by `npm run build`, then `git diff --stat firmware/assets/`
  (`ui.bin` must be unchanged; `test_keys.bin` may change), then `mk run` in `firmware/test` (debug) and
  `mk config=release run`: same test counts as each other and as before. See Flagged assumptions for keeping
  the regenerated bin.
- [x] 8. **Hand-written definitions in the examples; drop the old dep from host.** `examples/demo.js`
  (`iconDef(svgFile, label, color)` -> `{ layers: [{ type: "svg", src, colors: { "#848484": color }, translate },
  { type: "text", text: label, ... }] }`, same values) and `examples/bench.js` (its one def). These need a device:
  no automated test; check with a scratch script (`scratch/bf-demo-defs.mjs`, run by path) that renders both
  defs through `buttonLayer` and asserts non-transparent pixels, plus `node --check` on both files. Then remove
  `@toptensoftware/node-buttonImage` from `host/package.json`, delete
  `host/node_modules/@toptensoftware/node-buttonImage`, `npm install` in `host/` (lock drops `canvas`). Grep the
  repo (excluding `node_modules`, the submodule and `complete-*.md`) for `node-buttonImage`, `ButtonImage`,
  `svgFile`, `svgCode`, `svgForeColor`, `backColor`: only docs should remain. Fix the comment in
  `urem2/layout/activities/hdd.mjs:19`. Verify: all suites from step 1.
- [x] 9. **Docs.** Update (never `complete-*.md`): `docs/layout-format.md` (lines ~8, 29, 214-226, 519: icons are
  button-face built-ins, the look properties described by what they do rather than as `svg*`/`text*` fields,
  virtual pixels = button-face `virtualSize`, the open point), `docs/standalone.md` (~103, 117, 119: the renderer
  is now browser-capable, only `images.js`'s sharp use and `Buffer` block a browser bake; ~279), `docs/screen-buffers.md:49`,
  `host/README.md` (24, 31, 35, 72: dependency, Node >= 24, `renderButtonImage`/`buttonLayer` take button-face
  definitions), `urem2/README.md:126`, `cli/README.md` if it states a Node version. **CLAUDE.md: propose only.**
  Write the proposed replacement text for CLAUDE.md lines ~56 (drop the node-buttonImage `npm test`), ~60 (the
  install-links note: button-face is a `github:` dep, pinned by the lock, updated with
  `npm install git+https://github.com/toptensoftware/button-face.git` in `host/` and `tools/test-icons/`; the stale-copy dance now only
  applies to `rle565/encoder`), ~75 (repository layout: remove the node-buttonImage line) and ~184 (host-side key
  images: button-face, layer definitions, `#name` built-ins) into Notes as "Proposed CLAUDE.md changes", and ask
  the user to approve before applying (Rule 4). Docs only: no tests to write; re-run host `npm test` as a sanity check.
- [x] 10. **Remove the node-buttonImage submodule** (hard to reverse locally: **ask the user before running**).
  From the repo root: `git submodule deinit -f node-buttonImage`, `git rm node-buttonImage` (also edits
  `.gitmodules`), then delete `.git/modules/node-buttonImage`. Leave the sibling clone
  `C:\Users\Brad\Projects\node-buttonImage` alone. Verify: `git status` shows `.gitmodules` and the gitlink removed;
  grep finds no remaining code reference; all suites from step 1 pass (step 1's counts plus the tests added in steps 3-6);
  `tools/test-icons` `npm run build` still runs. Apply the approved CLAUDE.md wording from step 9 if the user has
  confirmed it by now.
- [x] 11. **Visual check (not automatable).** Passed: the user checked the renders and the hardware, "all looks good" (2026-11-15). Render "after" mockups into `scratch/bf-after/` with the same commands
  as step 1 and ask the user to compare them with `scratch/bf-before/` (expect tiny Skia differences, the 20
  redrawn icons, possibly changed label wrap points and bold label widths). Then on the hardware (user): bake and
  upload urem2 (`node cli/cli.js upload urem2/urem2.mjs`) and look at every page normal and pressed; run
  `examples/key-shapes.mjs`; if `test_keys.bin` was regenerated, a `RUN_STRESS` build should still show its keys.
  Record the user's verdict in Notes, then mark the job complete (rename to `complete-switch-to-button-face.md`).
