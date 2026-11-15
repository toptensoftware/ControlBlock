# Versioning and deployment

Status: Investigate

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
1. Make the repo (and PacketEncoder) public? Affects Release downloads, Pages/web flasher, Actions minutes,
   npm provenance, and the submodule checkout in CI.
2. Product version source: root `package.json` (with workspaces) or a `VERSION` file? Start at 0.1.0?
3. npm names: the CLI/library package (`@toptensoftware/controlblock` scoped vs the free unscoped
   `controlblock`, which gives `npx controlblock ...`), one package (b) or library + separate CLI (a); the
   button image rename (`@toptensoftware/buttonimage`?); `rle565` unscoped (free) or `@toptensoftware/rle565`;
   `@toptensoftware/binpack`.
4. Publish clargs to npm, or drop binpack's dependency on it for the library?
5. CI: run the Windows host unit tests in CI (2x minutes on a private repo), or only Linux node/python tests +
   firmware build?
6. Release assets: include the ELF? A starter layout blob? A web flasher now or later?
7. ESP-IDF in CI: pin `v6.0` (matches local) or move both local and CI to `v6.0.3`?
8. Add `layout_blob_version` to GET_INFO (protocol change, firmware + host + docs) so uploads refuse a blob the
   firmware can't load?

## Flagged assumptions
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
