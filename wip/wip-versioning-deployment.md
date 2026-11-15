# Versioning and deployment

Status: Implement

## Description
Get ControlBlock ready to ship releases, starting with v0.1:

1. **Version clocking:** one clear scheme for bumping and stamping versions across the firmware (GET_INFO
   `fw_version`, from `esp_app_get_description()->version`, i.e. `git describe` today), the host library
   (`host/package.json`, 0.1.0) and the CLI. Decide where the version lives, how it's bumped, and how a build
   that isn't from a tag is marked.
2. **GitHub Action:** build the firmware (ESP-IDF v6.0, release preset, submodules) and publish it (GitHub
   Release on a version tag, with flashable binaries). Consider also running the test suites in CI.
3. **CLI via npx:** refactor `cli/` (and whatever it depends on: `host/`, `binpack`, `node-buttonImage`,
   `rle565/encoder`) so the CLI can run with `npx <something>` without cloning or installing the repo.

## Notes

**Update 2026-11-15 (later the same day; supersedes the stale facts below):**
- **The repo will be public** (user's decision; it is still PRIVATE until the user flips it). So: free
  Actions minutes (Windows included), free Pages, public Release downloads, npm provenance possible.
- Since the investigation: `node-buttonImage` is gone, replaced by **`@toptensoftware/button-face`** (a `git+https`
  dependency of `host/` and `tools/test-icons`, pinned by each lock; **not on npm**, repo public). The
  rename-for-npm problem below no longer applies, but button-face must be published to npm (or the dep stays a
  git URL, which makes npm need git at install time) before the host package can be.
- **PacketEncoder is public and on https** now; SimpleLib's URL is the new one. So `actions/checkout` with
  `submodules: recursive` works (no PAT); per-job `git submodule update --init <list>` is still faster.
- Root `README.md`, `LICENSE` (MIT), `THIRD-PARTY-NOTICES.md` exist. `host/` and `cli/` say `engines.node >=24`.
- `@toptensoftware/clargs` is still not on npm; binpack still depends on it from `github:`.

**Step 1 (root workspaces), done 2026-11-15:**
- Root `package.json`: `controlblock-repo` 0.1.0, private, `"type": "module"` (without it, `tools/codegen/*.js`
  warn MODULE_TYPELESS_PACKAGE_JSON now that a package.json sits above them; every loose `.js` in the repo is ESM,
  and `mk run` still works), engines Node >=24, `workspaces`: `host`, `cli`, `tools/test-icons`, `rle565/encoder`.
  Root `node_modules/` added to `.gitignore`. One root `package-lock.json`; `host/` and `tools/test-icons` lost
  their `.npmrc` (install-links) and locks.
- **Root test command:** `npm test` at the root =
  `npm test -w host -w cli && npm test --prefix binpack && node --test "urem2/**/*.test.mjs" "examples/**/*.test.mjs"`.
  Results: host 294, cli 31, binpack 81, urem2+examples 212, all pass. `rle565/encoder`'s placeholder test script
  and `tools/test-icons` (none) aren't run.
- **binpack is NOT a workspace** (deviation from the plan): the binpack repo has its `node_modules/` checked in
  (clargs), and as a workspace (or as a `file:` dep) npm hoists clargs to the root and **deletes those tracked
  files**, plus bin-links rewrites `binpack/cli.js`'s line endings: the submodule shows dirty after every install.
  So `host/package.json` `"imports"` has `"#binpack": "./src/bake/binpack.js"`, a one-line shim
  (`export * from "../../../binpack/index.js"`), and `"#rle565": "rle565"` (the workspace package). binpack's own
  `cli.js` keeps finding clargs in its checked-in `node_modules`. Step 6 must point `#binpack` at the vendored copy
  and leave the shim out of the package (it escapes the package). If binpack's repo drops its checked-in
  `node_modules` (user's call: it's the submodule's repo), binpack can become a workspace and `#binpack` ->
  `@toptensoftware/binpack`.
- `#binpack`/`#rle565` are used in `host/src/bake/pack.js`, `schema.js`, `host/src/images.js` and the host bake tests
  (`bake.test.js`, `binpack-dedupe.test.js`, `pack.test.js`; `pack.test.js` still imports `../../../binpack/cli.js`
  for `buildCombinedBuffer`). Host tests keep `rle565/unpack.js` by package name (resolves through the workspace
  link). `cli/test/cli.test.js` keeps `../../binpack/index.js` (cli is its own package scope: `#binpack` is host's).
  `examples/bake-check.mjs` imported `../host/node_modules/rle565/unpack.js`; now `rle565/unpack.js` (root
  `node_modules`). `tools/test-icons/gen.js` imports `rle565` by name (works through the workspace).
- `npm ls sharp`: one copy (0.34.5), deduped for host, test-icons and rle565.
- button-face stays `git+https://...button-face.git`; the root lock pins `728ea56` (the same sha as the old locks,
  = its HEAD today). **Gotcha:** npm records it as `"resolved": "git+ssh://git@github.com/..."` (the old locks did
  too); check that `npm ci` on a CI runner without ssh keys fetches it (npm should fall back to https for GitHub;
  step 8 will tell).
- Checked: `tools/codegen` gen-types/gen-hid-keys and `npm run build -w tools/test-icons` regenerate byte-identical
  files; `node cli/cli.js bake firmware/test/data/sample_layout.json` matches the checked-in `.bin`; `mk run`
  (789) passes; the binpack and rle565 submodules stay clean.

**Step 2 (bump script), done 2026-11-15:**
- `scripts/bump-version.mjs <x.y.z|patch|minor|major> [--force] [--root <dir>]`; exports `VERSION_FILES`,
  `readVersions(root)`, `nextVersion(current, spec, {force})`, `bumpVersion(root, spec, {force})` (returns
  `{ from, to, lock }`). Checks everything before writing; refuses if the three package.json versions disagree.
  package.json files are edited by replacing only the top-level 2-space `"version"` line (so `cli/package.json`'s
  inline `{ ... }` objects survive); the lock is `JSON.stringify(_, null, 2) + "\n"` (exactly npm's format: the
  test round-trips it byte for byte). Prints `git add ...`, `git commit -m "Version X"`, `git tag vX`,
  `git push && git push origin vX`; never runs git.
- **Lock entries npm records:** top-level `version`, `packages[""].version`, `packages["host"].version`,
  `packages["cli"].version` (the `node_modules/<name>` link entries carry no version). The script updates those
  four. `cli/package.json` got `"version": "0.1.0"` and the lock's `packages["cli"]` the matching line (added by
  hand after `name`, where npm puts it; not re-checked with `npm install`, which should leave it as is).
- `scripts/test/version.test.mjs`: 12 tests (versions agree incl. the lock, nextVersion, bump on a temp copy,
  no lock, disagreement, missing version, the CLI's output/errors/--force). Root `npm test` now also runs
  `node --test "scripts/**/*.test.mjs"` (a directory argument doesn't work, as CLAUDE.md says). Root `npm test`:
  host 294, cli 31, binpack 81, scripts+urem2+examples 224, all pass. Also fixed a missing space (`&&node`) in
  the root test script.
- Step 9's release check (tag == root version) can import `readVersions` from the script.

**Step 3 (firmware version), done 2026-11-15:**
- `firmware/version.cmake`, included by `firmware/CMakeLists.txt` before IDF's `project.cmake`;
  `cb_set_project_version()` sets `PROJECT_VER` (IDF then skips its own `git describe`). Pure, tested pieces:
  `cb_read_package_version(out err file)` (top-level `version`, must be a non-empty string) and
  `cb_format_version(out err [OVERRIDE v] VERSION v GIT_OK b TAG t HASH h DIRTY b)`. Device-only:
  `cb_git_state(prefix dir version)` (`git rev-parse --short=7 HEAD`; `git status --porcelain
  --untracked-files=no` for dirty, so untracked files don't count but modified submodules do, like IDF's
  `describe --dirty`; `git tag --points-at HEAD --list v<version>`). Any git failure -> `<version>-dev` (checked by
  hand outside a repo: `GIT_OK FALSE`).
- **Too long = configure error**, not a cut (a cut would drop the hash or `.dirty`). `<ver>-dev+<7 hash>.dirty` is
  18 + len(ver), so versions up to 13 chars fit; a longer short hash (git lengthens it when ambiguous) also counts.
- **`-DCB_VERSION=x` is one-shot:** read, then `unset(CB_VERSION CACHE)`, logged as `Firmware version: x (from
  -DCB_VERSION)`. But the version is only computed at configure: the override value stays in that build folder
  until the next configure (package.json change, commit, checkout, `idf.py --preset <p> reconfigure`, fullclean).
  Checked: override -> `0.1.0`, a plain build afterwards keeps `0.1.0` (no reconfigure), `reconfigure` goes back to
  `0.1.0-dev+75ac767.dirty`. CI builds in a fresh folder, so it doesn't matter there.
- **Configure depends:** `../package.json`, the repo's `.git/HEAD` and the current branch's loose ref file
  (`git rev-parse --path-format=absolute --git-path refs/heads/<branch>`; a commit rewrites it), so a bump, commit or
  checkout reconfigures. **Not** `.git/index`: `git status` during configure refreshes the index, which would make
  every build reconfigure. So the `.dirty` mark is only as fresh as the last configure (checked: an unchanged
  rebuild doesn't reconfigure). A packed ref (after `git gc`, no loose file) isn't watched; harmless.
- Gotcha: `find_package(Git)` inside a function leaves `GIT_FOUND` function-local; each function calls it.
  `cmake_policy(SET CMP0174 NEW)` in `version.cmake` (own policy scope via `include()`) so `TAG ""` is an empty string,
  not an unset var with a dev warning.
- Test: `firmware/test/version_test.cmake`, 24 checks (release, dirty at tag, no/other/prefix tags, no git, override,
  31/32 chars, missing version, root package.json, bad fixtures written to `./cb_version_test_fixtures` and removed).
  Run: `C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe -P firmware/test/version_test.cmake` (or plain `cmake -P ...`
  after sourcing the IDF profile); exit 1 on failure (`SEND_ERROR`). **Not added to root `npm test`**: cmake isn't on
  PATH outside the IDF shell. Step 8's firmware CI job (IDF image has cmake) should run it before the build.
- Both presets build (0 errors); `build/debug` and `build/release` `project_description.json`: `"project_version":
  "0.1.0-dev+75ac767.dirty"`. `mk run` not needed (nothing it builds changed).

**Step 4 (GET_INFO `layout_blob_version`), done 2026-11-15:**
- Firmware: `protocol.cpp` `OnGetInfo` appends `u16 LAYOUT_BLOB_VERSION` after `max_keys` (`layout_blob.h` was
  already included through `protocol.h`). `PROTO_VERSION` stays 2: protocol.md says payloads can grow at the end
  and readers ignore trailing bytes (Byte order and encoding); noted in its Version history and GET_INFO table.
  `TestProtocol.cpp`: "Protocol Get Info" expects the u16; "Get Info Reports The Stored Layout" counts the fields
  after the layout's (`V2_FIELDS` 15 -> 17).
- Host: `decodeInfo` -> `info.layoutBlobVersion` (`r.u16(undefined)`: absent = `undefined`). `protocol.js` exports
  `BPAK_SIGNATURE`, `BPAK_HEADER_SIZE` (32) and `layoutBlobVersion(blob)` (the u32 at offset 32 if the blob starts
  with "BPAK", else `undefined`); a host test checks both constants and that `layoutRoot.version` is at offset 0
  against `layout_blob.h` / `layout_types.gen.h`. The check is in **`ControlBlock.uploadLayout`** (so `uploadBlob`,
  the CLI and browsers all get it): when the device reports a version, a non-BPAK blob or another version is
  refused before anything is sent, also with `force` and also when the device already has that blob. The message
  says "update the firmware" (blob newer) or "re-bake ..." (blob older).
- `FakeDevice` option `blobVersion` (default `BLOB_VERSION` from `bake/consts.js`; `null` omits the field), and
  `infoPayload({ blobVersion })`. New shared helper `layoutBlobOf(size, { seed, version })` in `FakeDevice.js`:
  pseudo-random bytes with a BPAK signature and the version stamped in. The old random `blobOf`s in
  `ControlBlock.test.js`, `bake/upload.test.js` and the CLI's upload test now use it (random bytes would be refused).
- Tests added: protocol (decode, absent = undefined, `layoutBlobVersion`), ControlBlock (mismatch both ways,
  non-BPAK, force/already stored, match uploads; no field = no check), upload.test.js (refused, nothing sent), CLI
  (exit 1, `error: ... loads version 99`, nothing stored).
- Docs: protocol.md (GET_INFO table, upload flow, Version history), hosted-mode.md (`info` table, upload refusal).
- `mk run` 789 (debug and release), debug firmware build 0 errors, root `npm test` all pass (host 299, cli 31,
  binpack 81, scripts+urem2+examples 224). Hardware check: step 11.
- **Gotchas:** `Reader.u16(undefined)` returns the default 0 (a JS default parameter), so "absent" is
  `r.has(2) ? r.u16() : undefined`. A ControlBlock test whose assertion fails before `cb.close()` leaves the read
  loop running and **hangs the whole `node --test` run** (that happened once here); the new tests close in
  `finally`. Run a suspect file with `node --test --test-timeout=20000 <file>`.

**Step 5 (CLI `--version` and `info`), done 2026-11-15:**
- `cli/version.js` (the one helper): `cliVersion()` (reads `cli/package.json` by `import.meta.url`, cached),
  `parseVersion(text)` (`[v]X.Y[.Z]` plus any `-...`/`+...` suffix -> `{ major, minor, patch }`, else null),
  `firmwareVersionNote(fw, cli = cliVersion())` (a `warning: ...` line when fw major.minor < the CLI's, a `note: ...
  isn't a release version` line when either doesn't parse, e.g. a git hash; else null). Step 6's package build can
  reuse `cliVersion`/the package.json, nothing else reads the version.
- `args.js`: `--version` (`{ command: "version" }`, no arguments) and `info [--serial S]`; USAGE lists both.
- `cli.js`: `info` prints `cli`, `device` (serial), `firmware`, `protocol`, `layout blob version` (`unknown` when
  undefined), `name` (`unknown (<error>)` if NAME_GET fails, e.g. firmware from before the name), `grid` (`5x3 cells of
  144 px, 720x432 at (40, 44)`); the note goes to stderr, exit 0. No takeover.
- **Lazy loading:** `cli.js` statically imports only `../host/browser.js` (ControlBlock, WifiState, ClockSource) and
  `../host/src/bake/upload.js` (imports only ControlBlock); `bake`, `render` and `upload <layout>` do
  `await import("../host/index.js")` (`loadBakeLib()`, kept in a module variable). `main`'s catch tests
  `bakeLib && err instanceof bakeLib.BakeError` (only a command that loaded it can throw one). `host/index.js` is
  unchanged.
- **Lazy test:** `cli/test/block-image-stack.mjs` (`module.registerHooks` resolve hook, preloaded with
  `node --import <file URL>`; a bare Windows path fails with ERR_UNSUPPORTED_ESM_URL_SCHEME) refuses `sharp`, `wasmoon`,
  `@toptensoftware/button-face`, `@napi-rs/canvas` and `rle565`/`#rle565` **when the importer is `host/index.js`,
  `host/src/**` or `cli/**` (not `cli/test/`)**. Guarding by importer, not by specifier alone, because the test's own
  FakeDevice imports `rle565/unpack.js`, which imports `./pack.js` -> sharp. `cli/test/lazy-child.mjs` runs `--version`,
  `info`, `wifi status`, `time get`, `name`, `upload <.bin>` through `main()` against a FakeDevice and then checks that
  importing `host/index.js` is refused (the hook works). Checked by hand that switching `cli.js` back to
  `../host/index.js` fails the test (`blocked: sharp (imported by host/src/images.js)`).
- `FakeDevice` / `infoPayload` got a `firmwareVersion` option (default `"1.0"`, as before).
- Tests: args (+1), version.test.js (3, new), cli (+5: `--version` spawned, lazy, info, warning/note, unknown name).
  cli 31 -> 40. Root `npm test`: host 299, cli 40, binpack 81, scripts+urem2+examples 224, all pass.
- Docs: `cli/README.md` (intro: root `npm install`, which commands load the image stack; `info`, `--version`
  sections, examples, tests), `docs/guide/getting-started.md` (First boot: `info` and `--version`). getting-started
  still says `cd host; npm install` (stale since step 1): left for step 10.

**Step 6 (CLI package build), done 2026-11-15:**
- `scripts/build-cli-package.mjs <outDir> [--clean]`; exports `buildCliPackage(root, outDir, { clean })` (returns
  `{ version, files }`), `importSpecifiers(text)` (static/multi-line/re-export/side-effect imports at line start, and
  `import("literal")`) and `PACKAGE_NAME`. `<outDir>` must be empty but for `.git`; `--clean` empties it keeping `.git`
  (for step 9: check out the `cli` branch, build into it with `--clean`, commit).
- **Files by import closure, not by folder:** it follows relative imports from `cli/cli.js`, `host/index.js`,
  `host/browser.js` (39 files today), so tests, `bake/headers.js` and the `bake/binpack.js` shim stay out by themselves;
  anything the closure reaches outside `cli/`/`host/` is an error. A file read some other way (fs) would be missed:
  none today (only `cli/version.js` reads `cli/package.json`, which is generated).
- **No `host/package.json` in the package** (deviation): Node refuses `imports` targets with `..`, so a
  `host/package.json` couldn't point `#binpack` at a root `vendor/`. The root package.json scopes `host/` instead
  (`type: module`, `imports` `#binpack` -> `./vendor/binpack/index.js`, `#rle565` -> `./vendor/rle565/pack.js`). An
  unknown `#` key is a build error (`VENDOR` table in the script). Vendored: binpack `index.js` + `buf.js`, rle565
  encoder `pack.js` + `BufferBuilder.js` (each by its own import closure) + their `LICENSE`.
- `cli/package.json` is the source one minus `scripts`/`devDependencies`/`workspaces`, version = root, so
  `cliVersion()` works unchanged. Root package.json: host's `license`/`author`/`engines`/`exports` (targets moved to
  `./host/...`, `./package.json` stays), `main` `host/index.js`, `bin.controlblock` `cli/cli.js`, `repository`.
- **Dependencies = the npm packages the shipped files import** (button-face, sharp, usb, wasmoon; fontkit and clargs
  aren't imported), ranges from host/package.json (else the vendored library's); a git range is pinned to the root
  lock's sha, ssh normalised to `git+https://github.com/...#<sha>`. Version via new `productVersion(root)` in
  `bump-version.mjs` (throws if the three versions disagree; `bumpVersion` uses it too).
- Test `scripts/test/cli-package.test.mjs` (7 + 1 skipped smoke; already covered by the root `scripts/**/*.test.mjs`
  glob): scanner, file list, package.json files, dependencies vs imports, every relative/`#` import resolves inside the
  package, `node cli/cli.js --version` in the temp build (no node_modules anywhere above `os.tmpdir()`), `--clean`.
  **Smoke (`CB_SMOKE=1`) run once locally: passes** (~7 s with a warm npm cache): `npm pack` the build, `npm install`
  the tarball into a temp project (fetches button-face over https at the pinned sha), `npx controlblock --version`,
  and `npx controlblock bake firmware/test/data/sample_layout.json` matches the checked-in `.bin` byte for byte.
- Gotcha: building into a folder inside the repo (e.g. `scratch/`) "works" without installing anything because Node
  finds the repo's root `node_modules`; test the package from a temp dir outside the repo.
- Root `npm test`: host 299, cli 40, binpack 81, scripts+urem2+examples 232 (1 skipped: the smoke test), all pass.

**Step 7 (web flasher), done 2026-11-15:**
- `scripts/make-flasher-manifest.mjs <version> <flasher_args.json> <outDir>`; exports `makeFlasherSite(version,
  argsFile, outDir, { page })` (returns `{ manifest, files }`), `flashParts(args)`, `parseOffset(hex)`,
  `manifest(version, parts)`, `PARTS`, `PAGE`. outDir becomes **the whole Pages site**: `manifest.json`,
  `index.html` (copied from `flasher/index.html`) and the three parts. Created if missing, not emptied (CI builds
  into a fresh folder). Nothing from `scripts/` was reusable (no shared helper fits; `productVersion` isn't used
  because the version is an argument: step 9 passes the tag's).
- **Part names (step 9 must publish the same names as Release assets):** `bootloader.bin`, `partition-table.bin`,
  `controlblock-<ver>.bin`. Roles come from `flasher_args.json`'s `bootloader`/`partition-table`/`app` entries;
  offsets (as numbers) from `flash_files`, matched by file. Refuses: chip not `esp32s3`, a missing role, a role whose
  file isn't in `flash_files`, any extra `flash_files` entry (it would be left out of the flasher), a missing bin
  (checked before anything is written), a version that isn't `x.y.z[suffix]` (`[0-9A-Za-z.+-]`, it goes in a file
  name). Checked against the real `firmware/build/release/flasher_args.json`: offsets 0, 32768, 65536.
- Manifest: `name` "ControlBlock", `version`, `new_install_prompt_erase: true`, `new_install_improv_wait_time: 0`,
  `builds: [{ chipFamily: "ESP32-S3", parts }]` (no `serialType`: such a build is the fallback for any connection).
  Format checked against esp-web-tools 10.4.0's `dist/const.d.ts`.
- `flasher/index.html`: ESP Web Tools **10.4.0** from unpkg (`dist/web/install-button.js?module`; released
  2026-07-15, newest is 10.5.2 from 2026-11-12), `<esp-web-install-button manifest="manifest.json">` with
  `unsupported`/`not-allowed` slots, version fetched from `manifest.json` (also fills the app name into the esptool
  fallback command), the steps, `npx github:toptensoftware/ControlBlock#cli info`, Releases link + three-part esptool
  command. Light/dark by `prefers-color-scheme`; phone width fine (the `pre` blocks scroll sideways).
- **Can't be tested in a browser automatically.** The test only checks what the page references (pinned
  version, `manifest.json`, slots, the npx line). The real check is step 11 (flash a board from Pages).
- **THIRD-PARTY-NOTICES (step 10):** nothing ships in the repo (ESP Web Tools and its bundled esptool-js load from
  unpkg at run time), but the Pages site uses it: a short mention (ESP Web Tools, Apache-2.0, loaded from a CDN by
  the flasher page) seems right.
- Test `scripts/test/flasher-manifest.test.mjs` (12; fixture `scripts/test/fixtures/flasher/`: a copy of the real
  `flasher_args.json` and three tiny text "bins"). Root `npm test`: host 299, cli 40, binpack 81,
  scripts+urem2+examples 244 (1 skipped: the smoke test), all pass.

**Steps 8 and 9 (CI and release workflows), done 2026-11-15:**
- Files: `.github/workflows/ci.yml` (push to `main`, PRs; cancels superseded runs) only calls two reusable
  workflows: **`tests.yml`** (`workflow_call`: jobs `node` and `unit-tests`) and **`firmware.yml`**
  (`workflow_call`, optional `version` input -> `-DCB_VERSION`). `release.yml` calls the same two (DRY: the
  firmware build and the tests exist once). Job names in the UI are `tests / Node and Python tests`, `tests /
  Firmware host unit tests (MSVC x86)`, `firmware / Firmware (release)`. Shared setup is the composite action
  **`.github/actions/prepare`** (after `actions/checkout`): `safe.directory '*'`, `url."https://github.com/".insteadOf`
  for `ssh://git@github.com/` and `git@github.com:`, `git submodule update --init <submodules>`, Node 24
  (`node: "false"` in the IDF container).
- **button-face lock entry:** the root lock really says `"resolved": "git+ssh://git@github.com/toptensoftware/
  button-face.git#728ea56..."`. pacote fetches GitHub git deps as codeload tarballs first (no ssh), and the
  insteadOf rewrite covers a fallback clone too.
- `node` job (ubuntu): checkout `fetch-depth: 0`, submodules `rle565` (workspace) `binpack` `urem2/atvdaemon`,
  `npm ci`, root `npm test`, then `CB_SMOKE=1 node --test scripts/test/cli-package.test.mjs`, then
  `python3 -m unittest` in `urem2/atvdaemon` (`python3`: `python` isn't guaranteed on ubuntu-latest). Nothing in the
  root tests reads SimpleLib, lua or PacketEncoder (grepped).
- `unit-tests` (windows-latest): **no msvc-dev-cmd step**: mk's `tools/msvc.mjs` finds VS itself (`vswhere -latest
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`, then `vcvarsall.bat <platform>` with test.mk.js's
  `x86`), which the runner's VS 2022 has. Submodules SimpleLib, lua, rle565 (test.mk.js's sources/includes).
  `npm install -g toptensoftware/mk.js`, `mk run` in `firmware/test`.
- `firmware` (ubuntu, `container: espressif/idf:v6.0`, bash): checkout `fetch-depth: 0`, submodules rle565
  SimpleLib lua, `. "$IDF_PATH/export.sh"` in each step (a `container:` job doesn't run the image's entrypoint),
  `cmake -P firmware/test/version_test.cmake` from the repo root, `idf.py --preset release [-DCB_VERSION=v] build`,
  then **a second call** `idf.py --preset release merge-bin -o controlblock-full.bin` (v6.0's merge-bin runs esptool in
  the build dir with `@flash_args`, so `-o` is relative to `build/release`; a call without `-D` adds no cache entries,
  so idf.py doesn't reconfigure and the one-shot CB_VERSION stays). With a version, the step fails unless
  `project_description.json` has exactly that `project_version`. Artifact `firmware`: bootloader, partition table,
  app, `controlblock-full.bin`, ELF, `flasher_args.json`, `flash_args`, `sdkconfig`, `project_description.json`,
  under their build-folder paths (upload-artifact strips the common `firmware/build/release/`, so the downloaded
  `flasher_args.json`'s relative paths still work).
- `release.yml` (tag `v*`; `permissions: contents/pages/id-token: write`; `concurrency: release`): `version` job
  (`node scripts/release.mjs check-tag "$GITHUB_REF_NAME"` -> output), then `tests` and `firmware` (with the
  version), then `pages` and `cli` in parallel, then `release` last (so a Release never points at a missing flasher
  or `cli-v` tag). `pages`: `make-flasher-manifest.mjs` on the downloaded build into `site/`,
  `actions/upload-pages-artifact@v3`, `actions/deploy-pages@v4`, environment `github-pages`. `cli`: exits early if tag
  `cli-v<ver>` exists (re-runs); else `git worktree add -B cli <tmp> FETCH_HEAD` from `origin/cli`, or
  `git worktree add --orphan -b cli <tmp>` (git >= 2.42) the first time; `build-cli-package.mjs <tmp> --clean`
  (a worktree's `.git` is a file; the script keeps any entry named `.git`), commit as `github-actions[bot]`, tag,
  `git push origin cli refs/tags/cli-v<ver>`. Pushes with GITHUB_TOKEN don't trigger workflows, and `cli-v*` doesn't
  match `v*` anyway. `release`: `release.mjs assets`, `zip` the ELF as `controlblock-<ver>-elf.zip`, `release.mjs
  notes`, `gh release create <tag> assets/* --verify-tag` (or `upload --clobber` + `edit` if it exists: re-runs).
- **`scripts/release.mjs`** (`check-tag <tag> [--root]`, `assets <ver> <flasher_args.json> <outDir>`, `notes <ver>
  <owner/repo> <flasher_args.json> <outFile>`): exports `tagVersion` (uses `productVersion`), `pagesUrl`
  (`https://<owner lowercased>.github.io/<name>/`), `renderTemplate` (`{{name}}`; unknown or unused placeholders are
  errors), `flashCommandParts` (esptool's `0x0 bootloader.bin 0x8000 ...` from flasher_args.json),
  `releaseNotes`, `releaseAssets` (checks the merged image exists before writing), `FULL_IMAGE`
  (`controlblock-full.bin`, the name firmware.yml gives merge-bin's output), `NOTES_TEMPLATE`. Notes template:
  **`.github/release-notes.md`** (`{{version}}`, `{{repo}}`, `{{flasher_url}}`, `{{flash_parts}}`): web flasher link,
  the three-part esptool command keeps settings and layout, the full image wipes Wi-Fi/name/clock settings (NVS) but
  keeps the stored layout, `npx github:<repo>#cli-v<ver> info` (git + Node 24) and `#cli` for the latest, the files.
- DRY refactor: `make-flasher-manifest.mjs` now exports `buildParts(version, argsFile)` (validate, resolve, name the
  three parts; writes nothing) and `copyParts(parts, outDir)`; `makeFlasherSite` and `releaseAssets` both use them,
  so Release assets and the flasher's files have the same names by construction.
- Test `scripts/test/release.test.mjs` (11; in the root `scripts/**/*.test.mjs` glob). Root `npm test`: host 299,
  cli 40, binpack 81, scripts+urem2+examples 255 (1 skipped: smoke), all pass.
- **Lint:** actionlint 1.7.7 + shellcheck 0.10.0 (downloaded into `scratch/`, gitignored): 0 errors on all four
  workflows (actionlint doesn't lint the composite action's script). **Nothing here has run on GitHub: the first
  push to `main` (CI) and the first `v*` tag (release) are the real tests.** Likely first-run trouble spots:
  Linux fonts in the host render tests, `npm ci` fetching button-face, the IDF container's `export.sh`, the
  orphan worktree, the Pages environment rule below.
- **What the user must do on GitHub (step 11):**
  1. Make the repo public.
  2. Settings > Pages > Source: **GitHub Actions**.
  3. Settings > Environments > `github-pages` (created by step 2) > Deployment branches and tags: **add a tag rule
     `v*`**. By default it allows only the default branch, and the release runs on a tag: without the rule
     `deploy-pages` fails with "Tag v0.1.0 is not allowed to deploy to github-pages".
  4. Settings > Actions > General > Workflow permissions: either "Read and write" or leave "Read" (the workflows
     ask for `contents: write` themselves; an org/enterprise policy that caps it at read would block the cli push
     and the Release). No branch/tag protection rule may block `github-actions[bot]` pushing `cli` / `cli-v*`.
  5. `node scripts/bump-version.mjs 0.1.0 --force` isn't needed (already 0.1.0): commit, `git tag v0.1.0`, push
     `main` and the tag.

**Step 10 (docs), done 2026-11-15:**
- `README.md`: Getting started is now flash (web flasher link, esptool + Releases) and run the CLI with npx
  (`#cli`, `#cli-v0.1.0`, `npm install -g ...#cli` for `controlblock`), a versions one-liner; hosted example
  installs `github:...#cli` and imports `@toptensoftware/controlblock`; limitations (no "no prebuilt firmware",
  now "not on the npm registry, needs git" and "download mode + Chrome/Edge"); Development: clone, root
  `npm install`/`npm test`, the test table (root `npm test`, binpack via `--prefix`, scripts glob, the cmake
  version test), CI, and a "Releasing" paragraph; repo layout row for `flasher/`, `scripts/`, `.github/`.
- `docs/guide/getting-started.md`: What you need (Chrome/Edge or esptool), the ports table (native USB also
  flashes releases), Option A filled in (web flasher steps, esptool three-part command, the full image wipes
  settings), updating, **Installing the tools** rewritten (npx, pinned tag, global install, clone), and from there
  on the guide writes **`controlblock`** for whichever way the CLI runs (defined once in Installing the tools).
- `docs/guide/standalone-mode.md`: same `controlblock` convention (examples/test server commands keep `node ...`
  and say they need a clone); `my-layout.json` in any folder; "Home is empty" points at the matching
  `#cli-v<version>` and the upload refusal.
- `docs/guide/hosted-mode.md`: Installing = `npm install github:...#cli` (or `#cli-v0.1.0`), import by name; the
  guide's code still imports `./host/index.js` (a clone) and says to swap in the package name. **There is no
  `./browser` subpath export**: `host/package.json` (and so the built package) only has `.` with a `browser`
  condition, so a Node app can't import the browser entry by name; the guide says bundlers get it through the
  condition. Limitation updated.
- `host/README.md` (install + the stale install-links paragraph), `examples/README.md` (root `npm install`),
  `cli/README.md` (new "Running it" section: npx forms, global install, the `cli` branch, clone; "How it's built").
- `THIRD-PARTY-NOTICES.md`: rle565 encoder row in the host tools table (was only in the firmware table), a note
  that the `cli` package vendors binpack and the encoder with their licence files, and a new "On the web flasher
  page" table (ESP Web Tools 10.4.0 + esptool-js, Apache-2.0, loaded from unpkg, not shipped).
- Grep for `install-links`, `host/node_modules`, `cd host`, `npm link`, `file:../rle565`, `node-buttonImage`: the
  remaining hits are history (`docs/standalone.md`, `docs/screen-buffers.md`), test comments, CLAUDE.md (see the
  proposal) and `wip/`. Nothing in the tests reads the docs; root `npm test`: host 299, cli 40, binpack 81,
  scripts+urem2+examples 255 (1 skipped), all pass.

### Proposed CLAUDE.md changes (consolidated)

Not applied (Rule 4): the user confirms first. This replaces the separate step 1 and step 3 proposals (folded in
below). Each item quotes the current CLAUDE.md text it changes, then the new text, ready to paste.

**1. *Building*: a new bullet after "Presets (...)":**

> - **Firmware version** (GET_INFO `fw_version`, `esp_app_desc_t.version`): `firmware/version.cmake` (included by
>   `firmware/CMakeLists.txt` before IDF's `project.cmake`) sets `PROJECT_VER` from the root `package.json`:
>   `<ver>` for a clean build at tag `v<ver>`, else `<ver>-dev+<7-char hash>[.dirty]` (`<ver>-dev` if git fails;
>   dirty = tracked files or submodules changed, untracked files don't count). Longer than 31 chars is a configure
>   error, not a cut. `-DCB_VERSION=x` overrides it once (removed from the cache, but the value stays in that build
>   folder until the next configure). Computed at configure: a version bump, commit or checkout reconfigures
>   (`package.json`, `.git/HEAD` and the branch ref are configure depends; `.git/index` isn't), so `.dirty` only
>   refreshes on a configure (`idf.py --preset <p> reconfigure`). Test: `cmake -P firmware/test/version_test.cmake`
>   from the repo root (cmake from the IDF shell, or `C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe`); not in
>   `npm test` (cmake isn't on PATH outside the IDF shell), CI's firmware job runs it.

**2. *Host library tests*: replace the first sentence** "From `host/`: `npm install` once, then `npm test` (Node's
test runner, ...)" with:

> `npm install` once at the repo root (npm workspaces: `host`, `cli`, `tools/test-icons`, `rle565/encoder`; one root
> `package-lock.json`, no per-package locks or `.npmrc`), then `npm test` at the root runs every Node suite (host,
> cli, binpack, and the `scripts`, `urem2` and `examples` globs), or `npm test` in `host/` or `cli/` for one (Node's
> test runner, ...

and add at the end of the paragraph:

> A ControlBlock test whose assertion fails before `cb.close()` leaves the read loop running and hangs the whole
> `node --test` run: close in `finally`, and run a suspect file with `node --test --test-timeout=20000 <file>`.
> `FakeDevice` options: `firmwareVersion`, `blobVersion` (`null` omits the field); `layoutBlobOf(size, { seed,
> version })` makes a fake blob the upload check accepts (random bytes are refused).

**3. *CLI and binpack tests*: replace the first paragraph** ("From `cli/`: `npm test` (no device: ...). From
`binpack/`: `node --test test.js`.") with:

> From `cli/`: `npm test` (after the root `npm install`; no device: upload and wifi run against the library's
> `FakeDevice`). binpack: `npm test --prefix binpack` from the root (or `node --test test.js` in `binpack/`).
> Release scripts: `node --test "scripts/**/*.test.mjs"` (in the root `npm test`; `CB_SMOKE=1` also runs the CLI
> package smoke test, which needs the network).

**4. Replace the paragraph** "`host` and `tools/test-icons` copy their `file:` dependency `rle565/encoder`
(`install-links=true` ...) ... `The library imports `binpack` by relative path too.`" with:

> `rle565/encoder` is a workspace, linked (not copied): edits reach `host` and `tools/test-icons` at once. binpack
> isn't a workspace (its repo has its `node_modules` checked in, which npm would delete, and bin-links rewrites its
> `cli.js` line endings): `host` reaches it through `#binpack` (`host/package.json` `imports` ->
> `src/bake/binpack.js`, a one-line shim over `../../../binpack/index.js`) and the encoder through `#rle565`; use
> those in `host/src`, never relative paths out of `host/` (the CLI package build refuses them). If binpack's repo
> drops its checked-in `node_modules`, it can become a workspace. The key renderer, button-face
> (`@toptensoftware/button-face`, `git+https://github.com/toptensoftware/button-face.git`), is pinned to a commit
> by the root lock (recorded as `git+ssh://...`; npm fetches GitHub tarballs over https, and CI rewrites ssh URLs
> to https): to pick up a newer one run `npm install git+https://github.com/toptensoftware/button-face.git -w host
> -w tools/test-icons` at the root. The host library, the CLI and button-face need Node 24 or later. The root
> `package.json` is `"type": "module"` (every loose `.js` in the repo is ESM). `cli/` and `examples/` import the
> library by relative path (`../host/index.js`), so they never go stale.

**5. A new subsection after *CLI and binpack tests*:**

> ### Versions and releases
>
> - **One product version** (`0.1.0`) in the root `package.json`, shared by the firmware (`fw_version`), the host
>   library and the CLI: root, `host/` and `cli/` `package.json` and the root lock's four entries must agree (a test
>   checks). Bump with `node scripts/bump-version.mjs <x.y.z|patch|minor|major> [--force]`: it edits those files
>   and prints the `git add/commit/tag/push` commands; it never runs git (plain `x.y.z` only; a version that isn't
>   newer needs `--force`). The format versions stay separate and change only with their format: `PROTO_VERSION`,
>   `LAYOUT_BLOB_VERSION`, `FILE_VERSION`, and `USB_BCD_DEVICE` (descriptor cache only). The submodule libraries keep
>   their own versions.
> - **Compatibility:** `protocol_version` is the hard check. GET_INFO's `layout_blob_version` makes
>   `ControlBlock.uploadLayout` refuse a blob of another version (or without a BPAK header), even with `force`;
>   older firmware without the field isn't checked. The CLI's `info` warns (stderr, exit 0) when the firmware's
>   major.minor is older than the CLI's (`cli/version.js`).
> - **CI** (`.github/workflows/`): `ci.yml` (push to `main`, PRs) calls the reusable `tests.yml` (`node` on ubuntu:
>   `npm ci`, root `npm test`, the `CB_SMOKE=1` CLI package test, `python3 -m unittest` in `urem2/atvdaemon`;
>   `unit-tests` on windows: `mk run`, mk finds MSVC itself) and `firmware.yml` (`container: espressif/idf:v6.0`,
>   `version_test.cmake`, `idf.py --preset release build`, `merge-bin -o controlblock-full.bin`, a `firmware`
>   artifact; optional `version` input -> `-DCB_VERSION`). Shared setup: the composite action
>   `.github/actions/prepare` (safe.directory, ssh -> https rewrite, only the submodules each job needs, Node 24).
>   Keep the IDF image tag in step with the local IDF. Lint workflows with actionlint.
> - **Release** (`release.yml`, on a tag `v*`): `scripts/release.mjs check-tag` (tag == root version), then the
>   tests and the firmware (with `-DCB_VERSION`), then **Pages** (the web flasher) and the **`cli` branch** in
>   parallel, then the **GitHub Release** last (`release.mjs assets`/`notes`, template `.github/release-notes.md`).
>   Release assets and the flasher's files are named by the same code (`make-flasher-manifest.mjs` `buildParts`):
>   `bootloader.bin`, `partition-table.bin`, `controlblock-<ver>.bin`, plus `controlblock-<ver>-full.bin` (merged;
>   its 0xFF padding wipes NVS: Wi-Fi, name, clock settings; the layout survives) and `controlblock-<ver>-elf.zip`.
>   Re-running a release is safe (an existing `cli-v` tag is skipped, an existing Release updated).
> - **CLI package** (`scripts/build-cli-package.mjs <outDir> [--clean]`): the release commits it to the `cli` branch
>   (one commit per release) and tags `cli-v<ver>`; users run `npx github:toptensoftware/ControlBlock#cli` (or
>   `#cli-v<ver>`; needs git and Node 24) and apps `npm install github:toptensoftware/ControlBlock#cli` and import
>   `@toptensoftware/controlblock` (one package: the CLI's bin and the library's exports). Files are the relative
>   import closure of `cli/cli.js`, `host/index.js` and `host/browser.js` (a file read with `fs` instead of imported
>   would be missed; an import outside `cli/`/`host/` is an error); binpack and the rle565 encoder are vendored
>   (`VENDOR` table; a new `#` import needs an entry), the root package.json carries the `imports` (Node forbids
>   `..` in `imports` targets, so there's no `host/package.json`), and npm dependencies are what the shipped files
>   import (button-face pinned to the lock's sha). **It must have no `workspaces` and no
>   install/prepare/build scripts:** npm would run "git dep preparation" on a GitHub install. Test a built package
>   from a temp folder outside the repo (inside it, the repo's `node_modules` hides missing dependencies).
> - **CLI lazy loading:** `cli.js` statically imports only `../host/browser.js` and `../host/src/bake/upload.js`;
>   `bake`, `render` and `upload <layout>` `await import("../host/index.js")`. `cli/test/block-image-stack.mjs` (a
>   resolve hook) fails the test if the other commands load sharp, button-face, wasmoon or rle565.
> - **Web flasher** (`flasher/index.html`, deployed to GitHub Pages, https://toptensoftware.github.io/ControlBlock/):
>   ESP Web Tools 10.4.0 pinned from unpkg; `manifest.json` from `make-flasher-manifest.mjs` lists the **three
>   parts** with offsets from `flasher_args.json` (never the merged image, which would wipe NVS). Users flash over
>   the native USB port in download mode (hold BOOT, press RESET), then press RESET. Not testable automatically.
> - **GitHub settings it needs:** Pages source "GitHub Actions"; environment `github-pages` with a deployment tag
>   rule `v*`; workflows may push `cli` / `cli-v*` (no protection rule blocking `github-actions[bot]`).

**6. *Firmware*, `protocol.cpp/.h` bullet:** "`GET_INFO` appends the grid" -> "`GET_INFO` appends the grid, then
`layout_blob_version` (u16, `LAYOUT_BLOB_VERSION`)". **`layout_blob` bullet:** after "an older blob is refused,
leaving the empty Home until a re-bake and upload" add "; hosts refuse to upload a blob of another version (GET_INFO
`layout_blob_version`)".

**7. *Communication*: after the "Protocol v2" bullet, add:** "- GET_INFO grew `layout_blob_version` (u16, after
`max_keys`, 2026-11-15; `PROTO_VERSION` stays 2: payloads grow at the end and readers ignore trailing bytes)."

**8. *Repository Layout*:** add

```
package.json       Root: the product version, npm workspaces (host, cli, tools/test-icons, rle565/encoder), `npm test` (every Node suite)
scripts/           Release scripts (bump-version.mjs, build-cli-package.mjs, make-flasher-manifest.mjs, release.mjs) and their tests (scripts/test/)
flasher/           The web flasher page (index.html; published to GitHub Pages with the release's bins and manifest)
.github/           CI and release workflows (workflows/), the shared setup action (actions/prepare), the release notes template
```

and change `cli/`'s line "(node cli/cli.js: bake, render (page PNG mockups), upload, wifi, time, name)" -> "(node
cli/cli.js: bake, render (page PNG mockups), upload, wifi, time, name, info, --version; released as the `cli` branch,
run with `npx github:toptensoftware/ControlBlock#cli`)", and `tools/test-icons/`'s "(npm install && npm run build)"
-> "(`npm run build -w tools/test-icons` from the root)".

**9. *Open Items*: add:**

> - **Releases** (job `wip/complete-versioning-deployment.md`): the CI and release workflows, the Pages flasher and
>   the `cli` branch have **not run on GitHub yet**: the first push to `main` and the first `v*` tag are the test.
>   Likely trouble: Linux fonts in the host render tests, `npm ci` fetching button-face, the IDF container's
>   `export.sh`, the orphan `cli` worktree, the Pages environment rule. Also unchecked: flashing from the Pages
>   flasher over native USB, `fw_version` = `0.1.0` in `info`, `npx ...#cli` on a clean machine, the blob version
>   refusal against real firmware. Later: a "reboot to bootloader" vendor request (no BOOT/RESET), firmware update
>   over USB, publishing to the npm registry (needs button-face and clargs on npm).

**10. *Conventions*, "User docs follow the code":** add "The user guides write the CLI as `controlblock` (defined in
getting-started's *Installing the tools*: npx, a global install, or `node cli/cli.js` in a clone); repo-only commands
(`examples/`, the test server) keep `node ...` paths."

**How users flash (answer to "minimum to flash without IDF", 2026-11-15):**
- **Primary: a web flasher on GitHub Pages** (ESP Web Tools, `<esp-web-install-button manifest=...>`,
  esptool-js over Web Serial; Chrome/Edge only). Needs only a browser and a cable. The release workflow
  copies the bins + manifest to Pages (Release assets have no CORS headers, Pages does). User: hold BOOT,
  press RESET on the **native USB port** (the S3 ROM's USB-Serial/JTAG: driverless on Windows 10/11, macOS,
  Linux; avoids the CH343P driver question), click Install, press RESET afterwards (the firmware takes the
  native port for TinyUSB).
- **The manifest lists the three parts** (0x0 bootloader, 0x8000 partition table, 0x10000 app), **not the merged
  image**: the merged image's 0xFF padding wipes NVS. ESP Web Tools' "erase device" box is the fresh-install
  path (erases everything, the stored layout too).
- **Fallback: Espressif's standalone `esptool` executable** (GitHub releases, no Python):
  `esptool --chip esp32s3 -p COMx write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 controlblock.bin`
  (or `pip install esptool`). The merged `-full.bin` is still published for one-file fresh installs.
- Flashing alone leaves an empty Home: Wi-Fi and layouts need the CLI (`npx ... wifi`, `bake`, `upload`), so
  area 3 is part of the minimum. The planned `web/` front end (WebUSB) could later make setup install-free.
- Later, not v0.1: a "reboot to bootloader" vendor request (force download boot, then restart: no BOOT/RESET
  dance; register for IDF v6.0 unchecked); firmware update over the vendor interface (OTA partitions fit in the
  8 MB spare flash).

**User's answers 2026-11-15:** version source = root `package.json` (with workspaces); IDF pinned at `v6.0`
(local and CI); add `layout_blob_version` to GET_INFO in this job; CLI to run **"directly from GitHub:
toptensoftware/ControlBlock if possible"** (instead of npm).

**npx from GitHub: what npm (11.9, local) actually does** (read in pacote `lib/git.js`):
- A `github:` spec is fetched as GitHub's **codeload tarball, which has no submodules**. A full clone (which
  would init them) is only the fallback when the tarball download fails over HTTP.
- npm resolves the branch/tag to a commit with `git ls-remote`, so **the user needs git installed**, as well as Node.
- If the fetched package.json has **`workspaces`** or a `prepare`/`build`/install script, npm runs `npm install`
  inside it ("git dep preparation"). With the submodule folders empty, that fails. So a root package.json
  with `workspaces` **can't** also be the `npx github:toptensoftware/ControlBlock` package.
- npm-package-arg parses `#path:<subdir>` (gitSubdir), but **pacote ignores it**, so `rle565/encoder` can't be a git
  dependency (the rle565 repo has no root package.json; the encoder package is in `encoder/`).
- `git archive HEAD` of this repo is ~5.7 MB, which would be downloaded on every new version.
- The host imports `rle565` and `@toptensoftware/button-face/node` by package name, binpack by relative path
  (`host/src/bake/pack.js`, `schema.js`). button-face depends on `@napi-rs/canvas` (native, per-platform
  optional packages), so it can't be vendored or bundled into a CI-built package: it must stay a real dependency
  (git or npm).

Investigated 2026-11-15. Repo facts that affect all three areas:

- **`toptensoftware/ControlBlock` is PRIVATE** (default branch `main`). So are GitHub Releases, Pages (paid plan
  only for private repos) and the Actions minutes budget (2,000 free min/month; Windows runners count 2x).
  `wip/wip-docs-licensing.md` (README + MIT LICENSE) is the related job if the repo goes public.
- Submodules (`.gitmodules`), all commits pushed to their `origin/main` (lua: `origin/v5.4`), none dirty:
  | Path | URL | Visibility | Needed by |
  |---|---|---|---|
  | `rle565` | https | public | firmware (`src/rle565.c`), host (`encoder/`) |
  | `SimpleLib` | https, **old URL** `SimpleLib.git` (redirects to `simplelib`) | public | firmware, host unit tests |
  | `lua` | https `lua/lua` | public | firmware, host unit tests |
  | `binpack` | https | public | host bake, cli tests |
  | `node-buttonImage` | https | public | host |
  | `urem2/atvdaemon` | https (push URL is ssh) | public | Python tests only |
  | `PacketEncoder` | **ssh** `git@github.com:...` | **PRIVATE** | `pad/` and `tools/pad-sim` only; **not** the S3 firmware yet (no reference in `firmware/`) |
  `actions/checkout` with `submodules: recursive` will fail on PacketEncoder (ssh URL, and the job's
  `GITHUB_TOKEN` can't read another private repo). Fix: check out without submodules and run
  `git submodule update --init <list>` for the ones each job needs, or switch the URL to https and give the
  workflow a PAT/deploy key (needed anyway once the S3 firmware uses PacketEncoder for the macro pad).
- No `.github/` directory, no tags, no root `package.json`, no root README/LICENSE.

### 1. Version clocking

**Current state**
- Firmware: `firmware/CMakeLists.txt` is just `project(controlblock)`: no `PROJECT_VER`, no `version.txt`, no
  `CONFIG_APP_PROJECT_VER_FROM_CONFIG`. ESP-IDF v6.0 (`tools/cmake/project.cmake` ~line 687) picks, in order:
  `PROJECT_VER` already defined (set before `project()`, or `-DPROJECT_VER=` on the command line, since the test
  is `if(NOT DEFINED PROJECT_VER)`) > `version.txt` in `firmware/` > `project(... VERSION x.y.z)` (numeric only)
  > `git describe --always --tags --dirty` (run in `firmware/`, i.e. this repo) > `"1"`. Then
  `CONFIG_APP_PROJECT_VER_FROM_CONFIG` can override it. It lands in `esp_app_desc_t.version` (`char[32]`: 31 chars
  max, truncated).
- With no tags `git describe` gives the short hash (`aab3962`, plus `-dirty`). It runs at **CMake configure
  time only**: `build/release/project_description.json` shows `"85488bc-dirty"` although HEAD is now
  `aab3962`, so local builds carry stale versions until reconfigured. In CI (fresh build dir) it's right,
  provided the checkout has tags (`fetch-depth: 0`) and git works in the container (the IDF docker image runs
  as root over a runner-owned checkout: git refuses with "dubious ownership" and the version silently becomes
  `1` unless `git config --global --add safe.directory '*'` runs first).
- Where it's shown: GET_INFO `fw_version` (`host_link.cpp:230` -> `protocol.cpp:459`, length-prefixed string,
  `docs/protocol.md` table), and the diag status page (`diag_collect.cpp:65`: `appVersion`, `idfVersion`,
  build date/time). The host library parses it (`info.fwVersion`, not checked). The CLI has no `--version`
  and doesn't print the firmware version anywhere obvious.
- package.json versions:
  | Package | name | version | private | notes |
  |---|---|---|---|---|
  | `host/` | `@toptensoftware/controlblock` | 0.1.0 | yes | deps `file:../node-buttonImage`, `file:../rle565/encoder`, `sharp`, `usb`, `wasmoon`; `.npmrc` `install-links=true` |
  | `cli/` | `controlblock-cli` | **none** | yes | `bin: { controlblock: ./cli.js }`, no deps, Node >=22 |
  | `tools/pad-sim` | `pad-sim` | 0.1.0 | yes | `serialport` |
  | `tools/test-icons` | `controlblock-test-icons` | none | yes | same `file:` deps as host |
  | `node-buttonImage` (submodule) | `@toptensoftware/node-buttonImage` | 1.1.3 | no | deps `canvas` ^3.2.1, `sharp` |
  | `binpack` (submodule) | `@toptensoftware/binpack` | 1.0.18 | no | dep `@toptensoftware/clargs` from `github:` (only its `cli.js` uses it) |
  | `rle565/encoder` (submodule) | `rle565` | 1.0.0 | no | `fontkit`, `sharp` |
  | `PacketEncoder` (submodule) | `packetencoder` | none | no | |
  `examples/` and `urem2/` have no package.json (plain `.mjs`, relative imports).
- **Format versions, keep separate from the product version:** `PROTO_VERSION` 2 (`protocol.h:28`, mirrored
  as `PROTOCOL_VERSION` in `host/src/protocol.js`, test-checked); `LAYOUT_BLOB_VERSION` 3 (`layout_blob.h:33` /
  `BLOB_VERSION` in `host/src/bake/consts.js`, test-checked); `FILE_VERSION` 1 (layout file format,
  `consts.js`). These change only when the format does.
- **`USB_BCD_DEVICE` 0x0102** (`usb.cpp:64`) exists only to bust Windows' MS OS 2.0 descriptor cache
  (per VID/PID/bcdDevice). It is not a product version; keep it separate and bump it only when the
  descriptors change. (Tying it to the release would be harmless but adds a registry entry per release and
  mixes two meanings.)
- **Compatibility checks today:** the library refuses another `protocol_version`
  (`ControlBlock.js:123`). Nothing checks the blob version: GET_INFO doesn't report `LAYOUT_BLOB_VERSION`, so a
  CLI that bakes v4 blobs uploads happily to v3 firmware, which then refuses the blob at boot (empty Home).

**Options for the single source**
- A. **Tag only** (`vX.Y.Z`), firmware from `git describe`, package.json versions set by CI from the tag at
  publish. Least to maintain, but the repo's package.json files lie between releases and local npm work never
  sees the real version.
- B. **A version file in the repo** (root `package.json` `version` if area 3 adds npm workspaces, otherwise a
  root `VERSION` file) + a bump script that writes it into every lockstep package.json, commits and creates the
  tag `vX.Y.Z`. Firmware CMake reads it before `project()` (`file(READ)` + `string(JSON ... GET version)`;
  CMake >= 3.19, fine) and sets `PROJECT_VER`. CI on a tag checks tag == file.
- C. `firmware/version.txt` as the source: simplest for IDF, but then IDF never runs `git describe`, so dev
  builds aren't marked, and npm still needs its own copy.

**Recommendation: B.** One product version for firmware + host library + CLI (they ship together and the
protocol already guards wire compatibility). The submodule libraries (node-buttonImage, rle565, binpack) keep
their own independent semver: they're separate repos with other users.
- Release build (HEAD exactly at tag `v<version>`, clean): `PROJECT_VER` = `0.1.0`.
- Any other build: `0.1.0-dev.<n>+<hash>[.dirty]` (or `0.1.0-<n>-g<hash>-dirty` straight from
  `git describe --tags --match "v*"`; with no tag yet, `0.1.0-dev+aab3962`). Keep it <= 31 chars. Fall back to
  `<version>-dev` when git isn't usable. Add `.git/HEAD` / `.git/index` to `CMAKE_CONFIGURE_DEPENDS` (or a
  custom target) only if the user cares that local builds show a stale hash.
- Allow `-DCB_VERSION=...` to override (CI can pass the tag directly, sidestepping git-in-docker).
- Bump script: `scripts/bump-version.mjs <x.y.z|patch|minor|major>` (in repo, run by path, no inline
  scripts), updates the version file and lockstep package.json(s), `git commit` + `git tag vX.Y.Z` only when the
  user asks (repo rule: no commits without asking), or prints the commands.
- After bumping: the npm packages' `version` = product version; pre-releases use npm's `-dev`/`-beta` with a
  dist-tag.
- Tests to add (TDD): a node test that the version file and the lockstep package.json versions agree, and one
  for the bump script. The firmware's version-string logic must stay in CMake (the IDF docker image has no
  Node): keep it trivial, optionally checked with a `cmake -P` script-mode test. Device-side display of the
  version can't be automated beyond the existing GET_INFO protocol test.
- **Firmware version check in the host/CLI:** don't hard-fail on `fw_version` (protocol_version already covers
  the wire). Recommended additions: CLI `--version` printing the CLI's version, and `info`/`version` command
  printing the device's `fw_version`; a warning (not an error) when the device's version is older than the
  library's major.minor. For the blob, append `u16 layout_blob_version` to GET_INFO (appending fields is how
  GET_INFO has grown before) so `uploadBlob` refuses a blob the firmware can't load: a real bug class today.
  (Firmware change: needs a unit test in `TestProtocol.cpp` and `docs/protocol.md`.)

### 2. GitHub Action: build and publish firmware

**Firmware build inputs (all checked in, nothing generated in CI)**
- `firmware/CMakePresets.json`: `release` = `build/release`, `SDKCONFIG_DEFAULTS` `sdkconfig.defaults;
  sdkconfig.release`, `SDKCONFIG` `./build/release/sdkconfig`. `idf.py --preset release build` from `firmware/`
  is what's run locally, so it works with v6.0's idf.py. (A stray gitignored `firmware/sdkconfig` exists locally;
  the presets don't use it.)
- `sdkconfig.defaults`: 16 MB flash, QIO 80 MHz (the image header says `dio`; the bootloader switches to QIO:
  normal for S3), custom `partitions.csv`, TinyUSB sizes etc. `sdkconfig.release`: `-O2` + assertions off.
- `partitions.csv`: nvs 0x9000 (24 KB), phy_init 0xf000, factory app 0x10000 (2 MB; app is ~1.2 MB),
  layout 0x210000 (4 MB). Bootloader at 0x0, partition table 0x8000.
- `build/release/flasher_args.json` already lists: `--flash-mode dio --flash-freq 80m --flash-size 16MB`,
  `0x0 bootloader/bootloader.bin`, `0x8000 partition_table/partition-table.bin`, `0x10000 controlblock.bin`,
  chip `esp32s3`.
- Component manager: `main/idf_component.yml` (gt911 ^1, esp_tinyusb ^2.0.1~1, mdns ^1, idf >=6.0.0);
  `firmware/dependencies.lock` is checked in (pins idf **6.0.0**, esp_tinyusb 2.1.1, tinyusb 0.19.0~2,
  gt911 1.2.1, mdns 1.11.0). `managed_components/` is gitignored, downloaded at configure (needs network,
  fine in CI). Building with a different IDF patch rewrites the lock's `idf` version (harmless, but CI should
  match local or the user should bump both).
- Embedded assets: `firmware/assets/ui.bin` and `test_keys.bin` are **checked in** (generated by
  `tools/test-icons` on demand). Generated headers (`layout_types.gen.h`, `hid_keys.gen.h`) are checked in too.
  **CI needs no Node to build the firmware.**
- Firmware submodules needed: `rle565`, `SimpleLib`, `lua` (paths in `main/CMakeLists.txt`).
- Local IDF is `v6.0` exactly (`C:\esp\v6.0\esp-idf`, `git describe` = `v6.0`).

**ESP-IDF in CI**
- Docker Hub `espressif/idf` has `v6.0`, `v6.0.1`, `v6.0.2`, `v6.0.3` (2026-09-02) and rolling
  `release-v6.0`. `espressif/esp-idf-ci-action@v1` (inputs `esp_idf_version` = docker tag, `target`, `path`,
  `command` default `idf.py build`, `extra_docker_args`) wraps that image on `ubuntu-latest`.
- Options: (a) `esp-idf-ci-action@v1` with `esp_idf_version: v6.0`, `target: esp32s3`, `path: firmware`,
  `command: git config --global --add safe.directory '*' && idf.py --preset release build && idf.py --preset
  release merge-bin` (the command is run through a shell in the container; verify quoting); (b) a job with
  `container: espressif/idf:v6.0` and plain `run:` steps (more control: several commands, caching
  `~/.cache`/component manager, env vars; the image's entrypoint sources export.sh, so for `container:` jobs
  add `. $IDF_PATH/export.sh` in the step); (c) install IDF on the runner (slow, pointless).
  **Recommend (b)** with the tag pinned to the local version (`v6.0`); bump local and CI together later
  (6.0.3 has fixes).

**What to publish (GitHub Release on tag `v*`)**
- `controlblock-<ver>.bin`: `controlblock.bin` (app only, at 0x10000): for updates.
- `bootloader.bin`, `partition-table.bin`, `flasher_args.json` / `flash_args` (offsets + settings).
- A merged image `controlblock-<ver>-full.bin` (`idf.py --preset release merge-bin` -> `build/release/
  merged-binary.bin`, or `esptool --chip esp32s3 merge-bin -o ... --flash-mode dio --flash-size 16MB
  --flash-freq 80m 0x0 ... 0x8000 ... 0x10000 ...`) flashed at 0x0. **Gotcha:** merge-bin pads the gaps with
  0xFF, so the merged image **overwrites NVS at 0x9000** (Wi-Fi credentials, device name, clock settings) and
  phy_init. It ends at the app's end, so the stored layout at 0x210000 survives. So: merged image = fresh
  install; the three separate parts = update that keeps settings. Say so in the release notes.
- The ELF (`controlblock.elf`, ~15 MB, for decoding backtraces) and the generated `sdkconfig`: as release
  assets or workflow artifacts (recommend: attach the ELF zipped; it's what makes user crash reports useful).
- Optionally baked layouts (`examples/*` are hardware test layouts and urem2 is the user's own setup: probably
  not release material; a "starter" layout would be new work). Open question.
- How users flash: `esptool --chip esp32s3 -p COMx write-flash 0x0 controlblock-<ver>-full.bin` (or the three
  parts with their offsets), `pip install esptool` or the copy bundled with IDF; over the CH343P UART port
  (Windows may need WCH's CH343 driver) or over native USB in download mode (hold BOOT, press RESET). The
  firmware owns native USB, so `idf.py flash` over native USB only works in download mode.
- **Web flasher:** ESP Web Tools (`<esp-web-install-button manifest=...>`, manifest lists `chipFamily:
  "ESP32-S3"` and `parts` with offsets) or esptool-js. Needs the bins served over HTTPS with CORS: GitHub
  Release asset downloads have no CORS headers, so the bins must be copied to GitHub Pages (needs a public repo,
  or a paid plan) or another host. Web Serial only in Chromium browsers. Natural fit with the planned `web/`
  front end (WebUSB). Suggest a later step, not v0.1.

**Can CI run the tests?**
- Node tests (`host/` `npm test`, `cli/` `npm test`, `binpack/` `node --test test.js`, `node-buttonImage/`
  `npm test`, root `node --test "urem2/**/*.test.mjs"` and `"examples/**/*.test.mjs"`): yes, on
  `ubuntu-latest` with Node 22/24. Need `npm ci` in `host/` (has `package-lock.json`; `install-links` copies the
  `file:` submodule deps, so `node-buttonImage` and `rle565` must be checked out first) and probably in
  `node-buttonImage/` (its own lock). `sharp`, `canvas` 3.x and `usb` install prebuilt binaries on linux-x64.
  Risk: label rendering uses canvas's system `sans-serif`, which differs on Linux; tests appear to check
  layout/metrics rather than golden pixels, but the first CI run will tell. `usb` needs `libudev` at runtime
  only when a device is opened (tests use `FakeDevice`).
- Python: `cd urem2/atvdaemon && python -m unittest` (stdlib only).
- Host unit tests (`firmware/test`, `mk run`): need Windows + MSVC (x86) + `mk` (`npm install -g
  toptensoftware/mk.js`, public GitHub repo). `windows-latest` has VS 2022; may need `ilammy/msvc-dev-cmd`
  with `arch: x86` unless mk finds MSVC itself via vswhere (to check). ~13 s run plus a few minutes setup;
  2x minutes cost on a private repo.
- Firmware build ~5-10 min including the image pull.

**Recommended workflow layout**
- `.github/workflows/ci.yml`: on push to `main` and PRs. Jobs: `node-tests` (ubuntu), `python-tests` (ubuntu,
  can fold into node-tests), `unit-tests` (windows, mk), `firmware` (ubuntu, `container: espressif/idf:v6.0`,
  release preset, upload the bins as a workflow artifact).
- `.github/workflows/release.yml`: on tag `v*`: same firmware build (reuse via `workflow_call` or one workflow
  with `if: startsWith(github.ref, 'refs/tags/v')`), check tag == version file, merge-bin, create the Release
  (`softprops/action-gh-release` or `gh release create`) with the assets above; optionally `npm publish`
  (needs an `NPM_TOKEN` secret, or npm trusted publishing/provenance, which needs a public repo for provenance).
- Checkout: `fetch-depth: 0` (tags for the version), no `submodules: recursive`; per job
  `git submodule update --init rle565 SimpleLib lua` (firmware/unit tests), `rle565 node-buttonImage binpack`
  (node), `urem2/atvdaemon` (python). Also fix the SimpleLib URL in `.gitmodules` to the new one.

### 3. CLI via npx

**Current state**
- `cli/cli.js` (shebang `#!/usr/bin/env node`, ESM, runs `main()` when `realpath(argv[1])` is itself, which
  also works through npm's bin symlinks/shims) imports everything from `../host/index.js`; `cli/args.js` parses.
  Commands: `bake`, `render`, `upload`, `wifi`, `time`, `name`. No `--version`. `cli/package.json`: private, no
  version, no deps, `bin.controlblock`, Node >=22. README says run `node cli/cli.js` or `npm link`.
- `cli/test/cli.test.js` imports `../../binpack/index.js`, `../../host/index.js`, `../../host/test/FakeDevice.js`,
  `../../host/test/bake/fixtures.js`, and spawns `cli.js` by path.
- `host/index.js` eagerly re-exports `src/images.js` (imports `sharp`, `rle565`, `node-buttonImage` -> `canvas`)
  and the bake, so **every** command, even `wifi`/`upload`, loads sharp + canvas. `usb` is imported lazily in
  `ControlBlock.open` (`await import("usb")`). `src/bake/lua.js` uses `wasmoon` (Lua syntax check).
- **Paths that escape the package** (must change for any npm publish): `host/src/bake/pack.js` and
  `schema.js` import `../../../binpack/index.js`; `host/src/bake/headers.js` points at `../../../firmware/main`
  (only used by `tools/codegen` and tests; move it out of `src/` or exclude it from the package). Host tests
  read firmware headers (`ControlBlock.test.js`, `bake/render-real.test.js`, `bake/preview.test.js`): fine,
  tests aren't published.
- `host-node.js` finds the stock icons via `import.meta.resolve("@toptensoftware/node-buttonImage")` + `images/`:
  works with a real package dependency, breaks if bundled.
- Native deps: `sharp` 0.34 (prebuilt `@img/*` per platform, Node >=20.9), `canvas` 3.x (prebuild-install
  downloads from GitHub releases; falls back to node-gyp + cairo if no prebuild: check linux-arm64/musl
  coverage), `usb` 3.x (bundled N-API prebuilds; Linux needs a udev rule to open the device). rle565's
  `pack.js` needs only `sharp` (`fontkit` is for `font.js`). First `npx` run will download ~tens of MB.
- npm registry (read-only `npm view`, 2026-11-15): `@toptensoftware/controlblock`, `controlblock`,
  `controlblock-cli`, `@toptensoftware/controlblock-cli`, `@toptensoftware/binpack`, `rle565`,
  `@toptensoftware/rle565`, `@toptensoftware/clargs`, `@toptensoftware/node-buttonimage`: **all unpublished
  (free)**. `binpack` (unscoped) is taken by someone else. The user owns the `@toptensoftware` scope (publishes
  `@toptensoftware/moe-js`, `bundle-free`, `flashy`, ...). **`@toptensoftware/node-buttonImage` can't be
  published under that name: npm no longer accepts capital letters** -> rename (e.g.
  `@toptensoftware/buttonimage` or `@toptensoftware/node-buttonimage`) and update the imports in `host/src/
  images.js`, `host/src/bake/host-node.js`, `tools/test-icons`.
- binpack's `github:` dependency on clargs makes npm need git at install time; only binpack's own `cli.js`
  uses it. Publish clargs to npm, or move it to an optional/dev dependency, before publishing binpack.

**Options**
- (a) **Publish each library** (`rle565` encoder, button image lib, binpack) + `@toptensoftware/controlblock`
  (host) + a separate CLI package depending on it. Clean, reusable, but five packages to version and publish;
  the CLI must import `@toptensoftware/controlblock` instead of `../host/index.js`.
- (b) **Same, but the CLI ships inside the host package** (`bin: { controlblock: "cli/cli.js" }`, CLI sources
  moved into `host/cli/` or `host/bin/`). One product package, one version (matches area 1), npx runs its
  only bin: `npx @toptensoftware/controlblock bake layout.mjs`. Library users get ~15 KB of CLI they don't use.
  Still needs the three submodule libraries on npm (or vendored).
- (c) `npx github:toptensoftware/ControlBlock`: needs a root package.json with `bin` and `prepare`; the repo is
  private (fails for anyone else); npm's git fetcher prefers GitHub's codeload tarball, which **has no
  submodules** (a full git clone does init them via @npmcli/git, but that path isn't guaranteed and
  PacketEncoder is private + ssh). Slow (whole repo incl. firmware). Not recommended.
- (d) **Bundle** (esbuild) the CLI + host + binpack + rle565 + button-image into one file, keeping `sharp`,
  `canvas`, `usb`, `wasmoon` external deps. One package, no library publishing; but the stock icon SVGs and
  wasmoon's `.wasm` must be copied and their path lookups patched, and the library itself still isn't on npm
  for hosted-mode apps. More build machinery to maintain.

**Recommendation: (b) on top of npm workspaces.**
- Add a private root `package.json` with `workspaces: ["host", "node-buttonImage", "rle565/encoder",
  "binpack", "tools/test-icons"]` (cli folded into host, or kept as its own workspace if the user picks (a)).
  Dependencies become normal semver ranges (`"@toptensoftware/buttonimage": "^1.1.3"`), resolved locally by
  workspace symlinks in dev and from the registry when published. This also removes the `install-links` +
  bump-version + delete-folder dance in CLAUDE.md (one `npm install` at the root). Check that hoisting keeps a
  single `sharp` (the likely reason for `install-links=true`).
- Replace the relative binpack imports with the package name.
- Lazy-load the image stack: make `host/index.js` not import `images.js`/bake eagerly, or have the CLI import
  only what each command needs (so `wifi`/`upload <bin>` don't load canvas; install still pulls it).
- `package.json` `files` whitelist (`index.js`, `browser.js`, `src/`, `cli/`, README, LICENSE), `engines.node
  ">=22"`, `repository`, `license` (ties into the licensing job).
- CLI: add `--version` (reads its own package.json); README gets the `npx` form.
- Tests: `cli.test.js` paths follow the move (`FakeDevice`, fixtures, binpack via the package name); add a
  test that the packed tarball has no paths outside the package (`npm pack --dry-run --json` and check the file
  list / that `bin` resolves), and a smoke test running the bin from a temp `npm install <tarball>` (could be a
  CI-only job).
- Publishing order: clargs (or drop the dep) -> binpack -> rle565 -> button image -> controlblock. The user
  publishes (agents mustn't); a CI `npm publish` on tag is optional.

### Open questions for the user
1. ~~Make the repo public?~~ **Answered: yes** (PacketEncoder already is).
2. ~~Product version source?~~ **Answered: root `package.json`** (start at 0.1.0 assumed).
3. ~~How is npx-from-GitHub packaged?~~ **Answered: option 2, a CI-built `cli` branch** (see Plan). Old
   question, now moot: npm names: the CLI/library package (`@toptensoftware/controlblock` scoped vs the free unscoped
   `controlblock`, which gives `npx controlblock ...`), one package (b) or library + separate CLI (a); the
   button image rename (`@toptensoftware/buttonimage`?); `rle565` unscoped (free) or `@toptensoftware/rle565`;
   `@toptensoftware/binpack`.
4. Publish clargs to npm, or drop binpack's dependency on it for the library?
5. CI: run the Windows host unit tests in CI (2x minutes on a private repo), or only Linux node/python tests +
   firmware build?
6. Release assets: include the ELF? A starter layout blob? ~~A web flasher now or later?~~ **Answered: web
   flasher on Pages in v0.1** (three-part manifest), standalone esptool as the documented fallback.
7. ~~ESP-IDF pin?~~ **Answered: `v6.0`.**
8. ~~Add `layout_blob_version` to GET_INFO?~~ **Answered: yes, in this job.**

## Flagged assumptions
- Step 1: binpack isn't a workspace; `#binpack` goes through a shim (`host/src/bake/binpack.js`) so npm never
  deletes the binpack repo's checked-in `node_modules`. Cleaner alternative needing a binpack commit: drop its
  `node_modules` from git, make it a workspace (Notes, "Step 1").
- Step 1: root `package.json` is `"type": "module"` and named `controlblock-repo`.
- Step 2: the bump script accepts only plain `x.y.z` (no pre-release like `0.2.0-beta.1`; add it if wanted), and
  refuses a version that isn't newer (equal or older) unless `--force`.
- Step 3: a firmware version over 31 chars fails the configure instead of being truncated.
- Step 3: `-DCB_VERSION` is one-shot (removed from the cache), unlike the sticky `CB_*` build switches; its value
  still lasts until the next configure of that build folder.
- Step 3: configure depends on `.git/HEAD` + the branch ref (fresh hash on commit/checkout) but not `.git/index`, so
  `.dirty` can be stale until a reconfigure.
- Step 3: "dirty" = tracked files or submodules changed; untracked files don't count. A release build needs the tag
  `v<ver>` exactly at HEAD (lightweight or annotated).
- Step 3: the cmake test isn't in root `npm test` (no cmake on PATH); CI's firmware job should run it.
- Step 4: the blob version check lives in `ControlBlock.uploadLayout`, not only `uploadBlob`, and also refuses a
  blob without a BPAK header (when the device reports a version). It applies even with `force`.
- Step 4: `layoutBlobVersion` checks only the BPAK signature, not the BPAK format version (offset 4); the device
  still refuses a wrong format at boot.
- Step 5: an unparseable firmware version (a git hash from firmware built before versioning) prints a `note:` line
  to stderr ("can't compare"), not a warning; exit 0 either way. A firmware *newer* than the CLI says nothing.
- Step 5: `info` also prints the CLI's own version (`cli: X`) as its first line, and falls back to
  `name: unknown (...)` instead of failing when NAME_GET is refused.
- Step 6: the built package has no `host/package.json` (Node forbids `..` in `imports` targets); the root
  package.json carries the `imports` and scopes `host/`. Vendor folders are at the package root as planned.
- Step 6: the package README is a short generated one (points at the repo's `cli/README.md` and `docs/`), not a copy of
  `cli/README.md` (which talks about running from the repo). Step 10 may want to revisit its text.
- Step 6: the smoke test packs with `npm pack` and installs the tarball (not `npm install <dir>`, which would symlink).
- Step 7: the manifest sets `new_install_improv_wait_time: 0` (the firmware doesn't speak Improv, and after the
  RESET the native port belongs to TinyUSB), so ESP Web Tools doesn't wait 10 s for it after flashing.
- Step 7: the board's ports are called `USB` (native) and `UART` on the page, as the task brief said; not checked
  against the board's silkscreen.
- Step 7: ESP Web Tools pinned at 10.4.0 (4 months old), loaded from unpkg; the esptool example uses `COM5`.
- Step 7: the flasher's app is `controlblock-<ver>.bin` (same as the Release asset), and the site folder isn't
  emptied first.
- Steps 8/9: ci.yml's jobs live in the reusable `tests.yml` (node, unit-tests) and `firmware.yml`, so release.yml
  reruns the tests on the tag (another ~15 min of runners; the tag commit is usually already tested on `main`).
- Steps 8/9: no npm or IDF caching (simpler; add `actions/cache` / setup-node `cache: npm` if runs are slow).
- Step 9: order is tests + firmware -> Pages and the `cli` branch -> GitHub Release last; a Release that fails
  after Pages deployed leaves Pages on the new version (re-run the workflow: the cli job skips an existing tag,
  the Release step updates an existing Release).
- Step 9: the release notes' esptool examples use `COM5` (as the flasher page does); Release isn't marked
  pre-release for 0.x.
- Step 9: the ELF asset is `controlblock-<ver>-elf.zip`; `sdkconfig`/`project_description.json` are only in the
  workflow artifact, not the Release.
- Step 10: the Pages URL is written `https://toptensoftware.github.io/ControlBlock/` (Pages lower-cases the owner;
  the repo name keeps its case in the path, as `release.mjs` `pagesUrl` builds it). Check it once Pages is live.
- Step 10: the user guides call the CLI `controlblock` after defining it once (npx / global install / clone),
  rather than spelling out `npx github:toptensoftware/ControlBlock#cli` in every command.
- Step 10: `npm install -g github:toptensoftware/ControlBlock#cli` (documented as the way to get `controlblock` on
  the path) hasn't been tried; the smoke test only covers `npm install <tarball>` + `npx controlblock`.
- Step 10: hosted-mode's examples still import `./host/index.js` (repo clone) with a note to use the package
  name; a `./browser` subpath export might be wanted for Node apps that want the device API only (not added: code).
- Step 10: the ESP Web Tools copyright holder is given as "the ESP Web Tools authors (ESPHome)", not checked
  against its LICENSE file.
- Step 2: the printed commands include `git push && git push origin vX.Y.Z` as well as commit and tag.
- npx packaging is option 2 (the user's choice, 2026-11-15): users need git and Node 24. Not npm.
- ~~The CLI package is named `controlblock`~~ **User: the built package is `@toptensoftware/controlblock`**
  (the same name as `host/`). It also exports the library (`"exports"` -> `host/index.js` / `host/browser.js`),
  so a hosted-mode app can `npm install github:toptensoftware/ControlBlock#cli` and import it by name.
- The `cli` branch keeps a history (one commit per release) rather than being force-pushed, so old
  `cli-vX.Y.Z` tags stay reachable.
- Pages hosts only the latest release's flasher.
- A blob version mismatch is a hard refusal in `uploadLayout` (older firmware that doesn't report the field
  isn't checked).
- `info` warns when the device's major.minor is older than the CLI's; nothing refuses on `fw_version`.
- Firmware, host library and CLI share one product version; the submodule libraries keep their own.
- `USB_BCD_DEVICE`, `PROTO_VERSION`, `LAYOUT_BLOB_VERSION` and `FILE_VERSION` stay independent of the product
  version.
- Dev builds should be visibly marked in `fw_version` (suffix with commit count/hash, `dirty`), and a release
  build is one made from a clean checkout at the tag.
- A host/CLI should warn, not refuse, on a firmware version mismatch: `protocol_version` remains the hard check.
- The merged "full" image is for fresh installs (it wipes NVS settings); updates use the separate parts.
- The S3 firmware doesn't need PacketEncoder yet; once the macro pad's S3 phase uses it, CI needs access to it.
- Not verified: whether `mk` finds MSVC on a GitHub Windows runner without a dev-cmd step; canvas prebuild
  coverage on linux-arm64; whether label-rendering tests pass with Linux fonts.

## Plan

**Decisions this plan rests on** (2026-11-15): the repo goes public; one product version in the root
`package.json` (0.1.0), shared by the firmware, host library and CLI; IDF `v6.0` in CI; GET_INFO gains
`layout_blob_version`; the CLI is run with **`npx github:toptensoftware/ControlBlock#cli`** (option 2): the
release workflow builds a self-contained package and pushes it to a `cli` branch (tagged `cli-vX.Y.Z`); `main`
keeps npm workspaces for development. Users flash with the web flasher on Pages (standalone esptool is the
fallback).

**How the CLI package resolves its submodule libraries:** `host/package.json` gets an `"imports"` map
(`"#binpack"`, `"#rle565"`). In dev, `#rle565` points at the workspace package `rle565` and `#binpack` at the shim
`./src/bake/binpack.js` (binpack isn't a workspace: see Notes, "Step 1"; leave the shim out of the package). In the built
package, the build script rewrites them to `./vendor/...` copies (plain JS: binpack's `index.js` and its
files, the encoder's `pack.js` + `BufferBuilder.js`; check that nothing else is needed). The host's sources stay
the same in both. `@toptensoftware/button-face` stays a git dependency, pinned to the sha in `host/package-lock.json`
(native `@napi-rs/canvas`: can't vendor it). The built package.json must have **no `workspaces` and no
install/prepare/build scripts** (they would trigger npm's git-dep preparation).

- [x] 1. **Root workspaces.** (binpack kept out of the workspaces: see Notes, "Step 1".) Add a private root `package.json` (`version` 0.1.0, `workspaces`: `host`, `cli`,
  `tools/test-icons`, `binpack`, `rle565/encoder`, `"test"` script running every Node suite). Switch
  `host/` and `tools/test-icons` from `file:` deps to semver ranges resolved through the workspace, delete their
  `.npmrc` (`install-links`) and their own locks in favour of one root lock. Add the `"#binpack"`/`"#rle565"`
  `imports` map to `host/package.json` and use it in `host/src/images.js`, `bake/pack.js` and `bake/schema.js`
  (and wherever else `rle565` or `../../../binpack` is imported; grep). Check that `npm ls sharp` shows one copy. All Node suites
  pass (host, cli, binpack, urem2 and examples globs). Write down the CLAUDE.md changes (install steps, the
  install-links paragraph, the button-face update command) as a proposal in Notes; don't apply them (Rule 4).
- [x] 2. **Version bump script.** TDD: `scripts/test/version.test.mjs` checks that root, `host/` and `cli/`
  package.json versions agree (give `cli/package.json` a version), and tests `scripts/bump-version.mjs`
  (`<x.y.z|patch|minor|major>`, works on a temp copy in tests). The script updates the three files and prints
  the `git commit`/`git tag vX.Y.Z` commands (it never commits). Add it to the root `test` script.
- [x] 3. **Firmware version from the root package.json.** `firmware/version.cmake`: a function that sets
  `PROJECT_VER` before `project()`. `-DCB_VERSION=` wins; otherwise `<version>` from `../package.json`
  (`string(JSON)`). It is exactly `<version>` when HEAD is clean and at tag `v<version>`, otherwise
  `<version>-dev+<shorthash>[.dirty]`, or `<version>-dev` if git fails. At most 31 chars. Test with a
  `cmake -P` script (`firmware/test/version_test.cmake`; inputs injected, no git in the test). Build the
  debug and release presets and check `project_description.json`.
- [x] 4. **GET_INFO `layout_blob_version`.** TDD: `TestProtocol.cpp` expects a u16 appended to GET_INFO
  (= `LAYOUT_BLOB_VERSION`); host `protocol.test.js` parses it (absent on older firmware = `undefined`);
  `uploadLayout`/`uploadBlob` refuses a blob whose version differs when the device reports one (and only when it
  reports one), with a clear error; `FakeDevice` reports it. Update `docs/protocol.md` and
  `docs/guide/hosted-mode.md`. `mk run`, the host tests and the firmware build pass. (Hardware check: step 11.)
- [x] 5. **CLI `--version` and `info`.** TDD in `cli/test/`: `--version` prints the product version (the CLI's
  package.json); `info` prints the device's `fw_version`, protocol and blob versions, name and grid (against
  `FakeDevice`), and warns (not fails) when the device's major.minor is older than the CLI's. Make the CLI
  load the image/bake stack lazily so `--version`, `info`, `wifi`, `time`, `name` and `upload <bin>` don't import
  sharp/button-face. Update `cli/README.md`.
- [x] 6. **CLI package build.** `scripts/build-cli-package.mjs <outDir>` assembles the self-contained package
  (root package.json: name `@toptensoftware/controlblock`, version, `bin.controlblock` -> `cli/cli.js`,
  `exports` for the library (same conditions as `host/package.json`), deps `sharp`, `usb`,
  `wasmoon`, button-face at its locked sha; `cli/`, `host/index.js`, `host/src/`, a rewritten `host/package.json`
  with vendor `imports`, `vendor/`, README, LICENSE, THIRD-PARTY-NOTICES). TDD: `scripts/test/
  cli-package.test.mjs` builds it into a temp dir and checks the file list (no tests, no paths outside the
  package: grep every relative import), no scripts or workspaces, version = root, that every `#` import
  target exists, and that `node cli/cli.js --version` runs in it with no `node_modules`. A full
  `npm install <dir>` smoke test needs the network: put it behind an env var (`CB_SMOKE=1`), run in CI.
- [x] 7. **Web flasher.** `flasher/index.html` (ESP Web Tools from a pinned CDN version, install steps:
  native USB port, hold BOOT, press RESET, Chrome/Edge; a note that "erase" also wipes the stored layout) and
  `scripts/make-flasher-manifest.mjs <version> <flasher_args.json> <outDir>`, which writes `manifest.json`
  (`chipFamily: "ESP32-S3"`, the **three parts** with offsets read from `flasher_args.json`, never the merged
  image) and copies the bins. TDD with a fixture `flasher_args.json`. Can't be tested in a browser
  automatically: say so.
- [x] 8. **CI workflow** `.github/workflows/ci.yml` (push to `main`, PRs): `node` (ubuntu, Node 24, `npm ci` at
  the root, root `npm test`, `CB_SMOKE=1` CLI package test, Python `urem2/atvdaemon` unittest); `unit-tests`
  (windows, MSVC x86, `npm install -g toptensoftware/mk.js`, `mk run` in `firmware/test`); `firmware` (ubuntu,
  `container: espressif/idf:v6.0`, `safe.directory`, `idf.py --preset release build`, upload the bins + ELF as
  an artifact). Checkout `fetch-depth: 0` and only the submodules each job needs. Can't be run locally: check
  it with `actionlint` if available; the first push is the real test.
- [x] 9. **Release workflow** `.github/workflows/release.yml` (tag `v*`): fail if tag != root version; firmware
  build with `-DCB_VERSION=<version>`, `merge-bin`; GitHub Release with `controlblock-<ver>.bin`,
  `bootloader.bin`, `partition-table.bin`, `flasher_args.json`, `controlblock-<ver>-full.bin` (notes: the full
  image wipes settings), the zipped ELF; Pages deploy of the flasher (step 7's script on the built bins); build the CLI
  package (step 6), commit it to the `cli` branch (create it orphaned the first time) and tag `cli-v<ver>`
  (`permissions: contents: write, pages: write, id-token: write`). Share the firmware build with `ci.yml`
  (`workflow_call` or a composite action), don't copy it. Same testing caveat as step 8.
- [x] 10. **Docs.** README (install: web flasher link, `npx github:toptensoftware/ControlBlock#cli`, git + Node
  24 as prerequisites, pinned `#cli-v0.1.0` form), `docs/guide/getting-started.md` (flashing: web flasher, then
  esptool fallback with the three-part command; first Wi-Fi and layout steps with npx), `cli/README.md`,
  THIRD-PARTY-NOTICES if the flasher page ships anything (ESP Web Tools is Apache-2.0, loaded from a CDN).
  Propose (don't apply) CLAUDE.md updates: the versioning scheme, the bump script, workspaces, CI, release, the
  `cli` branch.
- [ ] 11. **User checks (manual, on the hardware and GitHub).** The user makes the repo public, sets Pages
  to "GitHub Actions", bumps/tags `v0.1.0` and pushes. Then: CI is green, the Release has its assets, the web
  flasher flashes a board over native USB (settings and layout kept), `fw_version` = `0.1.0` in `cli info`,
  `npx github:toptensoftware/ControlBlock#cli --version` works on a clean machine, and uploading a blob with
  the wrong version is refused.
