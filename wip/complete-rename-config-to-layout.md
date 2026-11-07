# Rename standalone "config" to "layout"

Status: Complete

## Description
Rename the uploadable standalone configuration from "config" to "layout"
everywhere the term means that thing: firmware code (modules such as
`config_store`, `config_flash`, `config_blob`, `config_apply`, `config_boot`,
`config_types`, the `config` flash partition, identifiers, log text), the
protocol and its constants (config upload requests), the host library API
(`uploadConfig()`, the bake), the CLI, tests and test data
(`sample_config.json/.bin`), examples, docs (`config-format.md`,
`protocol.md`, `standalone.md`, ...), CLAUDE.md and generated headers.

Uses of "config" that mean something else (ESP-IDF/sdkconfig, `CONFIG_*`
Kconfig macros, driver config structs, build configs, `test.mk.js` config)
stay as they are.

Goal: one consistent name ("layout") for the standalone configuration across
firmware, host, CLI, protocol spec and docs, with everything still building
and all tests passing.

## Notes
- Watch for clashes with existing "layout" names: `key_layout.cpp/.h`
  (pages of keys), `screen_layout.h`, the model's "store + layout", and the
  protocol's hosted-mode "layout requests". The investigation should list
  these and propose how to keep them distinct.

### Investigation (2026-11-08)

Method: case-insensitive grep for "config" outside submodules, `node_modules`,
`build/` and `complete-*.md` (1638 hits in 140 files, most of them other senses),
then by identifier. Counts below are approximate and only the standalone sense.

#### 0. Senses of "config" that STAY (do not touch)
- ESP-IDF/Kconfig: `sdkconfig*`, `CONFIG_TINYUSB_*`, `CONFIG_ESP_*`, `CONFIG_LCD_*`,
  `CONFIG_FREERTOS_HZ`, `CONFIG_SPIRAM_*`, `CONFIG_ESP_LCD_TOUCH_MAX_POINTS`, etc.
- Driver/IDF structs: `panel_config`, `tp_io_config`, `gpio_config(_t)`, `i2c_*_config_t`,
  `esp_http_client_config_t`, `esp_ping_config_t`, `tinyusb_config_t`, `wifi_config_t`,
  `WIFI_INIT_CONFIG_DEFAULT`, `esp_wifi_set_config`, `full_speed_config`.
- USB configuration descriptors: `usb.cpp` (`TUD_CONFIG_*`, `MS_OS_20_CONFIG_LEN`,
  `bNumConfigurations`); **host**: `ControlBlock.js` lines ~94-97 (`d.configuration`,
  `configurationValue`, `selectConfiguration`) and `host/test/FakeDevice.js` lines 68-71.
- Wi-Fi credentials store: `WifiConfig` class, `wifi_config()`, `wifi_credentials.*`,
  `TestWifiCredentials.cpp` (40 hits), `wifi_env.h`, docs/wifi.md's IDF bits. Different
  thing (credentials in NVS); stays. (`Protocol::Init(..., ConfigStore *config, WifiConfig *wifi, ...)`
  becomes `LayoutStore *layout_store, WifiConfig *wifi`.)
- Build configs: `CMakePresets.json`, `mk config=release`, `test.mk.js`'s mk config,
  `.gitignore`s, all of `pad/` (PadScanner `config` struct, its sdkconfig), binpack,
  SimpleLib, lua, `scratch/` (gitignored).
- English "configure/configured" in comments unrelated to the blob.

#### 1. Standalone-config sense, by area

**firmware/main — module files (rename files + identifiers):**
| Now | Proposed |
|---|---|
| `config_store.cpp/.h` (~70 hits) | `layout_store.cpp/.h` |
| `config_flash.cpp/.h` (~14) | `layout_flash.cpp/.h` |
| `config_blob.cpp/.h` (~140) | `layout_blob.cpp/.h` |
| `config_apply.cpp/.h` (~17) | `layout_apply.cpp/.h` |
| `config_boot.cpp/.h` (~38) | `layout_boot.cpp/.h` |
| `config_types.h` | `layout_types.h` |
| `config_types.gen.h` (generated) | `layout_types.gen.h` |
- `CMakeLists.txt` lines 38-42 list the 5 .cpp files. `crc32_table.h` keeps its name.
- Identifiers (counts across firmware main+test): `cfgdata` namespace (248) -> e.g. `layoutdata`;
  `configRoot` struct (59, from schema.js `ROOT_TYPE`) -> `layoutRoot`; `ConfigError` (177),
  `ConfigResult` (104), `ConfigInfo` (36), `ConfigSummary` (29), `ConfigStore` (20),
  test names `Fact("ConfigBlob ...")`/`Fact("ConfigApply ...")`/`Fact("ConfigStore ...")` etc. (also
  quoted in wip-lua-hardware-checks.md),
  `ConfigImage` (15), `ConfigFlash` (13), `ConfigValidate` (13), `ConfigApplyActivity` (13),
  `ConfigApplyHome` (9), `ConfigRelocate` (8), `ConfigRoot()` fn (6), `ConfigKeyImageOffset` (9),
  `ConfigKeyImagePtrOffset` (7), `ConfigErrorName` (4); free functions `config_boot_load`,
  `config_boot_apply`, `config_blob()`, `config_blob_size()`, `config_summary()`, `config_flash_open`;
  statics `s_config` (host_link.cpp), `m_config` (protocol); macros `CONFIG_SECTOR`,
  `CONFIG_DATA_OFFSET`, `CONFIG_ERASE_AHEAD`, `CONFIG_MAGIC`, `CONFIG_HEADER_VERSION`,
  `CONFIG_HEADER_SIZE`, `CONFIG_BLOB_VERSION`, `CONFIG_MAX_DEVICES/ACTIVITIES/BINDINGS/OPS/
  ACTIVITY_PAGES/OP_DATA/SCRIPT`, `CONFIG_ACTIVITY_NONE`, `CONFIG_PARTITION_NAME`,
  `CONFIG_PARTITION_SUBTYPE`. Renaming our `CONFIG_*` macros to `LAYOUT_*` is a bonus: they
  currently share the Kconfig `CONFIG_` namespace.
- Other firmware files using these (includes, `cfgdata::configRoot`, comments "the config"):
  `activity_host.cpp/.h`, `activity_switch.cpp/.h`, `standalone.cpp/.h`, `standalone_task.cpp/.h`,
  `trigger_dispatch.cpp/.h`, `script_host.cpp/.h`, `script_api.cpp`, `op_exec.h`, `op_queue.h`,
  `op_registry.cpp`, `hid_keyboard.h`, `key_gesture.h`, `image_store.h` (comment), `protocol.cpp/.h`,
  `host_link.cpp`, `main.cpp` (includes, `config_boot_*` calls, log "after the config load"),
  `screen.cpp` (2, check).
- Log strings: `config_boot.cpp`/`config_flash.cpp` `TAG = "config"`; "no stored config: empty Home",
  "config read or CRC failed", "config rejected (%s)", "config loaded: ...", host_link.cpp
  "config saved: restarting". (examples/ or wip checklists may grep for these log lines.)

**firmware/partitions.csv:** partition `config` (data, subtype 0x40, 0x210000, 4 MB) + comment.
`layout_flash` finds it by subtype + `CONFIG_PARTITION_NAME`. Renaming the label changes the
partition table: needs a full `idf.py flash` once (partition table + app). An app with the new name
on a board with the old table finds no partition (empty Home, uploads fail with IO_ERROR or similar).
The data at 0x210000 survives a table reflash, so a stored blob still loads afterwards **if the
header magic is unchanged**.

**Flash header magic:** `CONFIG_MAGIC 0x46434243` ("CBCF") in the header sector. Changing it
(e.g. "CBLY") invalidates every stored blob (re-upload needed). Recommend keep the value.

**firmware/test:**
- Files: `TestConfigApply.cpp` (30), `TestConfigBlob.cpp` (116), `TestConfigStore.cpp` (141),
  `TestConfigTypes.cpp` (13), `config_env.h` (21, namespace `config_env`, `CB_OFF(configRoot, ...)`),
  `flash_env.h` (6; FakeFlash for the store, uses `CONFIG_SECTOR`; name can stay).
  Proposed: `TestLayoutApply/Blob/Store/Types.cpp`, `layout_env.h` (namespace `layout_env`).
  Tests are discovered from the folder (test.mk.js lists only `../main/*.cpp` sources).
- Also touched: `TestProtocol.cpp` (~60 of 105: `MSG_CONFIG_*`, `ConfigStore`, `ConfigResult`,
  `CONFIG_*`), `TestActivitySwitch.cpp` (18), `TestActivityHost.cpp` (8), `TestStandalone.cpp` (9),
  `TestTriggerDispatch.cpp` (4), `TestScriptHost.cpp` (2), `TestScriptParity.cpp`, `TestOpQueue.cpp`,
  `TestOpRegistry.cpp`, `TestImageStore.cpp` (1 each), `script_env.h` (4).
- `test.mk.js`: sources `../main/config_store.cpp`, `config_blob.cpp`, `config_apply.cpp` + comment.
- Test data: `firmware/test/data/sample_config.json/.bin` -> `sample_layout.json/.bin`; referenced
  in `config_env.h`, `TestConfigBlob.cpp`, `config_blob.cpp` comment, CLAUDE.md, docs/standalone.md,
  docs/standalone-ops-plan.md, wip-macro-pad.md.
- The blob bytes do not depend on any name: BPAK (`host/src/bake/pack.js`) writes no type names, so
  renaming `configRoot` or files leaves `sample_*.bin` byte-identical. Re-bake anyway and confirm
  (a diff = a mistake).

**Protocol (firmware `protocol.h/.cpp`, `host/src/protocol.js`, `docs/protocol.md`):**
- Wire unchanged: opcodes 0x30/0x31/0x32 and the GET_INFO field positions stay; only names change.
  No protocol version bump needed.
- `MSG_CONFIG_BEGIN/DATA/COMMIT` -> `MSG_LAYOUT_*`; `OnConfigBegin/Data/Commit` -> `OnLayout*`;
  `status_of(ConfigResult)`. Spec: `CONFIG_BEGIN/DATA/COMMIT`, `CONFIG_*` in the both-modes table,
  GET_INFO `config_capacity/config_size/config_crc` -> `layout_*`, "## Configuration upload"
  section (anchor `#configuration-upload`, 17 links across repo) -> "## Layout upload"
  (`#layout-upload`), status table rows 8-10 "Config upload".
- host `protocol.js`: `Msg.CONFIG_*`, `CONFIG_DATA_PREFIX`, `encodeConfigBegin/Data`,
  `info.configCapacity/configSize/configCrc` (public API).

**host/ (library):**
- `src/ControlBlock.js` (26, minus the USB ones): `uploadConfig()`, `CONFIG_COMMIT_TIMEOUT`,
  `CONFIG_WINDOW`, error text "Empty config", "Config is N bytes", `info.config*`.
- `src/bake/`: `schema.js` (`ROOT_TYPE = "configRoot"`), `headers.js` (`TYPES_HEADER_PATH` ->
  `config_types.gen.h`), `files.js` (`loadConfigFile`), `bake.js` (`bake(config, ...)`, error text
  "in the config"), `consts.js` (comments: FILE_VERSION, ACTIVITY_NONE <-> `CONFIG_ACTIVITY_NONE`,
  MAX_SCRIPT_BYTES <-> `CONFIG_MAX_SCRIPT`), `upload.js` (log/error text, `info.config*`),
  `resolve.js`, `diag.js`, `look.js`, `ops.js`, `host-node.js` (comments/links to config-format.md).
- `index.js` (exports `loadConfigFile`; comments), `browser.js` (comment), `README.md` (11).
- Tests: `test/ControlBlock.test.js` (54), `FakeDevice.js` (21; keep the USB `configuration`
  lines), `protocol.test.js` (16), `entries.test.js` (1, export list?), `bake/upload.test.js` (14),
  `bake/files.test.js` (10), `bake/helpers.js` (10), `bake/render.test.js` (8),
  `bake/fixtures.js` (6), `bake/types-header.test.js` (4, checks the generated header path),
  `bake/pack.test.js` (3), `bake/look.test.js` (2), `bake/bake.test.js`, `bake/resolve.test.js` (1).
- No `node_modules` reinstall needed: nothing renamed is a `file:` dependency (`node-buttonImage`,
  `rle565/encoder` untouched); `cli/` and `examples/` import `../host/index.js` by path.

**tools/codegen:** `gen-types.js` (comment; output path comes from `host/src/bake/headers.js`).
Regenerate `layout_types.gen.h` after the schema/path change; delete the old `.gen.h`.
`tools/test-icons`: no hits.

**cli/:** command names are `bake`, `upload`, `wifi` (no "config"). Touch: `args.js` (help text
`<config.json|.js|.mjs>`, "<config name>.bin", "bake needs one config file", "upload needs one config
or .bin file"), `cli.js` (`bakeConfig()`), `package.json` description, `README.md` (11),
`test/cli.test.js` (12), `test/args.test.js` (8; may assert on the help/error text).

**examples/:** `activity-test.mjs`, `script-test.mjs` (`const config = ...`, header comments),
`test-common.mjs`, `README.md` (10; "## Standalone configs" section, link to config-format.md),
`test-server/test-server.mjs`, `test-server/net_test_server.py` (1 each, comments). No example
file names contain "config" (`upload-test.json`, `net-test.json`).

**docs/:**
- `config-format.md` -> `layout-format.md` (title "Config file format (user level)"; ~10 hits;
  linked from CLAUDE.md, cli/README.md, examples/README.md, host/README.md, host/index.js,
  host/src/bake/{consts,look,ops,resolve}.js, host/test/bake/{bake.test,fixtures,look.test}.js,
  docs/standalone.md, docs/standalone-ops-plan.md, wip-macro-pad.md; anchors `#script-api` etc.).
- `standalone.md` (48; sections "## Config", "Config storage" (anchor referenced in CLAUDE.md and
  `config_store.h`), mentions of `config_layout.h`/`config_view.h`/`ConfigView` (historic names),
  `CONFIG_*` limits, `ConfigApplyHome`... ; also `CONFIG_TINYUSB_SUSPEND_CALLBACK` stays).
- `protocol.md` (33, above), `standalone-ops-plan.md` (14), `backlight.md` (2: "the config blob",
  "the config upload's dark screen"), `wifi.md` (2 of 15: "like the stored config", "the config
  upload"), `screen-buffers.md`/`backlight-waveshare-support.md`: IDF sense only, stay.

**CLAUDE.md:** ~20 standalone-sense hits: firmware module bullets (config_store/flash/blob/apply/
boot, `cfgdata`, `CONFIG_ACTIVITY_NONE`, `ConfigApplyHome`, re-bake command with sample_config),
Repository Layout docs list (config-format.md), host/ description ("the config bake"), Open Items
("Config upload", "Config boot load", "the config blob", "Scripts ... from a config"), test data.

**wip/:** `wip-lua-hardware-checks.md` (10: "test config", TestConfigApply/`ConfigApply ...` test
name, "stored config"), `wip-macro-pad.md` (~15 standalone-sense: config_store.cpp,
TestConfigStore.cpp, config-format.md, sample_config.bin, "bake/config side"),
`wip-node-test-server.md` (1). `complete-*.md` must not be touched (they'll keep the old names).

#### 2. Existing "layout" uses that clash
| Existing | Meaning | Clash | Proposal |
|---|---|---|---|
| `key_layout.cpp/.h`, `KeyLayout` (43), `model_layout()` (10), params/members `layout`, `m_layout`, `s_layout` (~230) | the live pages of keys (hosted and standalone) | **Real.** `ConfigApplyHome(summary, blob, store, KeyLayout *layout)` becomes `LayoutApplyHome(..., KeyLayout *layout)`; `config_apply.h` says "Put a ... config blob's Home on the key layout"; `Protocol::Init(store, layout, config, ...)` | Keep `KeyLayout`/`key_layout` (the `Key` prefix distinguishes). In APIs that take both, rename the `KeyLayout*` parameter/member to `keys` (or `key_layout`), e.g. `LayoutApplyHome(summary, blob, store, keys)`; prose says "the key layout" / "the keys", never bare "layout", for this sense. |
| `LayoutResult` enum (57, key_layout.h:40) | KeyLayout SetKeys/SetPageCount result | **Direct name collision** if `ConfigResult` -> `LayoutResult` | Rename key_layout's to `KeyLayoutResult` first; the store's becomes `LayoutStoreResult` (or `LayoutResult` once freed — prefer `LayoutStoreResult`, unambiguous). |
| `LAYOUT_MAX_PAGES` (13, key_layout.h:26) | KeyLayout page limit | Sits next to new `LAYOUT_MAX_DEVICES/ACTIVITIES/ACTIVITY_PAGES/...` and would read as a blob limit | Rename to `KEY_LAYOUT_MAX_PAGES` (or use `SCREEN_MAX_PAGES` directly). |
| `layout_page_changed_t` (4) | KeyLayout callback | Mild | -> `key_layout_page_changed_t`. |
| `check_layout`, `set_demo_layout`, `s_layout_refused` (main.cpp, stress) | demo/stress key layouts | Mild | Leave, or `check_keys`/`set_demo_keys`. Optional. |
| `screen_layout.h` (`SCREEN_*`) | screen geometry | Low (different prefix) | Leave. |
| Hosted-mode "layout" in docs: protocol.md ("key layouts", "the layout is cleared", "layout requests", NOT_HOSTED "edits the layout", "Layout and images are never saved to flash. Only the standalone config is"), CLAUDE.md ("any layout request", "store + layout"), host/README.md lines 61-62, ControlBlock.js 179-180, standalone.md step 1 ("Requests that define the layout") | the keys/images a host sets | **Real ambiguity**: after the rename "Layout ... never saved to flash. Only the standalone layout is" is self-contradictory | Reword the hosted sense to "keys"/"key layout"/"screen" (e.g. "requests that edit the screen" / "screen requests"); `FakeDevice.js` `LAYOUT_REQUESTS` -> `HOSTED_REQUESTS` (or `SCREEN_REQUESTS`). |
| Struct/memory "layout" (config_types.h "nothing here calculates a layout", test.mk.js "the device's layout", CLAUDE.md "match the device layout", standalone.md "partition layout", config_blob.h "File layout", CLAUDE.md "## Repository Layout", RUN_STRESS "layout in test_keys.h") | byte/struct layout | Confusing in the blob files | Reword where it sits next to the new term: "struct layout" -> "struct shape"/"memory layout", "File layout" -> "File format", "partition layout" -> "partition map". Leave "Repository Layout". |
| JSON schema | — | None: no `layout` key in the user format, schema.js or sample JSON | — |

#### 3. Things needing care
- User-written files: JSON has top-level `version`, `settings`, `defaults`, `devices`, `home`,
  `activities`, ... — no key or extension contains "config"; `FILE_VERSION` stays 1. Nothing for
  users to migrate.
- BPAK magic/format: unchanged (generic container). Blob version (`CONFIG_BLOB_VERSION`/
  `BLOB_VERSION`) stays 1.
- Flash header magic "CBCF": see above (keep the value; rename the macro only).
- Partition label: see above (full flash once; or keep the label).
- NVS: only Wi-Fi credentials; no config keys. USB strings: none mention config.
- CLI: command names have no "config"; only help/error text and the positional name change.
- npm/host API renames (breaking): `uploadConfig`, `loadConfigFile`, `info.configCapacity/Size/
  Crc`, `Msg.CONFIG_*`, `encodeConfig*`, `CONFIG_DATA_PREFIX`. Consumers in-repo only: cli/,
  examples/, tests, (planned web/).
- Generated: `layout_types.gen.h` via `node tools/codegen/gen-types.js` (repo root); the host
  test `types-header.test.js` catches drift. `hid_keys.gen.h` unaffected.
- Device-only code (`config_flash.cpp`, `config_boot.cpp`, `host_link.cpp`, `main.cpp`,
  `standalone_task.cpp`) can only be checked by `idf.py build` + hardware (Rule 3 warning).
- `sdkconfig.defaults`: no standalone-config entries.

#### 4. Sizing and suggested order
Rough sizes (standalone sense): firmware/main ~25 files / ~650 occurrences (5 module pairs +
2 type headers renamed); firmware/test ~20 files / ~400 (5 renamed + data pair); host ~25 files /
~200; cli 6 files / ~40; examples 6 files / ~20; docs 6 files / ~120 (+1 rename); CLAUDE.md ~20;
wip 3 files / ~25. Mostly mechanical; best done with careful per-identifier replaces, not a blind
`config` -> `layout` sweep (the senses in section 0 are interleaved, e.g. TestProtocol.cpp,
ControlBlock.js, FakeDevice.js, protocol.h).

Suggested order (each leaves `mk run` (debug and release), `npm test` in host/ and cli/, and
`idf.py build` green):
1. Free the names: `LayoutResult` -> `KeyLayoutResult`, `LAYOUT_MAX_PAGES` -> `KEY_LAYOUT_MAX_PAGES`,
   `layout_page_changed_t`; KeyLayout params next to blob APIs -> `keys`; FakeDevice
   `LAYOUT_REQUESTS`. Firmware + host tests.
2. Schema/codegen: `ROOT_TYPE` -> `layoutRoot`, `TYPES_HEADER_PATH` -> `layout_types.gen.h`,
   regenerate; `config_types.h` -> `layout_types.h`, `cfgdata` -> new namespace, `Config*Key*Offset`,
   `CONFIG_ACTIVITY_NONE`; all includes. Host bake tests + firmware tests.
3. Blob/apply/boot modules + their tests, `config_env.h`, `CONFIG_MAX_*`/`CONFIG_BLOB_VERSION`,
   `sample_config.*` -> `sample_layout.*` (re-bake, confirm byte-identical), test.mk.js, CMakeLists.
4. Store/flash modules + TestConfigStore, flash header macros (magic value kept), partition label
   + `CONFIG_PARTITION_NAME`, log strings, host_link.cpp/main.cpp.
5. Firmware protocol names (`MSG_LAYOUT_*`, `OnLayout*`, members) + TestProtocol.
6. Host library: protocol.js, ControlBlock.js, bake files, index.js/browser.js, upload.js, all host
   tests, host/README.md.
7. CLI (args, cli.js, tests, README, package.json) and examples/.
8. Docs: `git mv` config-format.md -> layout-format.md and fix every link; protocol.md (names,
   anchor, hosted-sense rewording), standalone.md, standalone-ops-plan.md, backlight.md, wifi.md.
9. CLAUDE.md (Rule 4: propose, apply only with the user's OK) and wip-*.md docs; final grep for
   leftover standalone-sense "config" / `cfgdata` / `sample_config`.
10. Hardware (user): full `idf.py flash` (partition table), check the stored blob still loads at
    boot (`layout loaded: ...`), then `node cli/cli.js upload examples/upload-test.json`.

### Decided (user, 2026-11-08)
The user accepted all the recommended defaults listed under *Flagged
assumptions* below; they are now decisions, not guesses:
- Partition label renamed `config` -> `layout` (one full `idf.py flash` needed; noted in the plan
  and docs). Flash header magic value `0x46434243` ("CBCF") KEPT so stored blobs survive.
- Module names `layout_*`, `cfgdata` -> `layoutdata`, root struct `layoutRoot`, store result enum
  `LayoutStoreResult`, `docs/config-format.md` -> `docs/layout-format.md`.
- No back-compat aliases for the host API (`uploadConfig`, `loadConfigFile`, `info.config*`, ...).
- Pre-rename clash fixes: `LayoutResult` -> `KeyLayoutResult`, `LAYOUT_MAX_PAGES` ->
  `KEY_LAYOUT_MAX_PAGES`, `layout_page_changed_t` -> `key_layout_page_changed_t`, `KeyLayout *layout`
  params next to blob APIs -> `keys`, `FakeDevice` `LAYOUT_REQUESTS` -> `HOSTED_REQUESTS`.
- Hosted-mode "layout" in docs/comments reworded to "keys"/"key layout"/"screen" as proposed.
- **CLAUDE.md update APPROVED** by the user for this rename (Rule 4 satisfied): plan step 13.
- `wip/complete-*.md` never touched; `wip/wip-*.md` may be edited.

### Implement log
- **Baseline (before step 1):** FW tests 556 (debug) / 556 (release); host `npm test` 212 pass.
- **Step 1 (done):** `KeyLayoutResult`, `KEY_LAYOUT_MAX_PAGES`, `key_layout_page_changed_t` in
  key_layout.h/.cpp, config_apply.cpp, main.cpp, protocol.cpp, TestKeyLayout.cpp, TestProtocol.cpp.
  `KeyLayout *layout` -> `KeyLayout *keys` in `ConfigApplyHome`/`ConfigApplyActivity`,
  `ActivityApplier::Init` (member `m_keys`), `Protocol::Init` (member `m_layout` -> `m_keys`; no
  clash, neither class had an `m_keys`). FakeDevice `HOSTED_REQUESTS` (+ its comment "edit the keys").
  Hosted-sense "layout" prose in config_apply.h/protocol comments left for steps 3/6. Optional
  main.cpp `check_layout`/`set_demo_layout` NOT renamed (wip-lua-hardware-checks.md quotes them; they
  are stress-demo key layouts, not the blob). After: FW 556/556, host 212 pass, `idf.py --preset
  debug build` OK.
- **Step 2 (done):** schema.js `ROOT_TYPE = "layoutRoot"`, headers.js `TYPES_HEADER_PATH` ->
  `layout_types.gen.h` (+ comment), `git mv config_types.h layout_types.h`, regenerated
  `layout_types.gen.h` (`git add -N`), `git rm` old `.gen.h`. `layoutdata`, `layoutRoot`,
  `LayoutKeyImage(Ptr)Offset`, `LAYOUT_ACTIVITY_NONE` and includes swept with word-boundary sed over
  firmware/main + test (39 files). layout_types.h: "calculates a layout" -> "calculates struct
  offsets", "config file/blob" -> "layout". types-header.test.js, gen-types.js, consts.js comments.
  Left for later steps: `ConfigRelocate` mention in layout_types.h (step 3), "struct layouts" in
  config_blob.cpp and test.mk.js comment (step 3), docs/CLAUDE.md `config_types`/`cfgdata`/
  `CONFIG_ACTIVITY_NONE` mentions (steps 10/12/13). After: FW 556/556, host 212, `idf.py --preset
  debug build` OK, re-bake byte-identical. Stale `test/build/**/*.d` files naming config_types.h
  did not break `mk` (it rebuilt fine).
- **Step 3 (done):** `git mv` layout_blob.*, layout_apply.*, TestLayoutBlob/Apply/Types.cpp,
  layout_env.h, sample_layout.json/.bin. Word-boundary sed over firmware main+test (34 files):
  `LayoutError(Name)`, `LayoutSummary`, **`LayoutInfo`** (defined in config_store.h; renamed here as
  the step lists it, so step 4 only has `ConfigStore`/`ConfigResult`/`ConfigFlash`/macros),
  `LayoutImage`, `LayoutValidate`, `LayoutRelocate`, `LayoutRoot()`, `LayoutApplyHome/Activity`,
  `LAYOUT_BLOB_VERSION`, `LAYOUT_MAX_*`, `layout_env`, includes, test.mk.js + CMakeLists sources,
  Fact names `LayoutApply/LayoutBlob/LayoutTypes ...` (TestConfigStore's `Fact("Config...")` left
  for step 4). `LayoutApplyHome`'s `LayoutSummary &config` param -> `summary`. The `config_blob()`
  *function* (config_boot) deliberately NOT touched (step 5): only `config_blob.h/.cpp` file refs.
  Prose: "config blob" -> "layout blob" in image_store.h, net_host.h, op_queue.h, TestImageStore.cpp;
  key-layout sense in layout_apply.* comments -> "keys"/"key layout"; "File layout" -> "File
  format", "struct layouts" -> "struct shapes"; consts.js cross-refs (`LAYOUT_MAX_SCRIPT` in
  layout_blob.h, "layout file(s)"). layout_apply.h still names `config_boot.cpp` (step 5).
  After: FW 556/556, host 212, `idf.py --preset debug build` OK, re-bake to `sample_layout.bin`
  byte-identical (same git blob hash `1cda0239`). Remaining old names only in CLAUDE.md, docs/
  standalone*.md, wip-lua-hardware-checks.md (test name), wip-macro-pad.md (later steps).
- **Step 4 (done):** `git mv` layout_store.*, layout_flash.*, TestLayoutStore.cpp. Word-boundary sed
  over firmware main+test: `LayoutStore`, `LayoutStoreResult`, `LayoutFlash`, `layout_flash_open`,
  `LAYOUT_SECTOR/DATA_OFFSET/ERASE_AHEAD/MAGIC/HEADER_VERSION/HEADER_SIZE/PARTITION_NAME/SUBTYPE`,
  includes, test.mk.js + CMakeLists. Magic value kept, with a comment above `LAYOUT_MAGIC` on why it
  still reads "CBCF". partitions.csv label `layout` (+ comment: one full `idf.py flash`; build output
  shows `layout,data,64,0x210000,4M`); `LAYOUT_PARTITION_NAME "layout"`; layout_flash TAG `"layout"`.
  `Protocol::Init(..., LayoutStore *layout_store, ...)`, member `m_layout_store`; host_link
  `s_layout_store`; TestProtocol fixture member `config` -> `layout_store`. `Fact("Config Store ...")`
  -> `Fact("Layout Store ...")` ("Stored Config" -> "Stored Layout" in two names). Untouched on
  purpose: usb.cpp `s_config`/`CONFIG_TOTAL_LEN` (USB descriptor), wifi.cpp `s_config` (WifiConfig);
  protocol.cpp's GET_INFO local `LayoutInfo config` (step 6); links to "Configuration upload"/
  "Config storage" doc sections (steps 11/12). After: FW 556/556, `idf.py --preset debug build` OK.
- **Step 5 (done):** `git mv` layout_boot.*; `layout_boot_load/apply`, `layout_blob()`,
  `layout_blob_size()`, `layout_summary()` (no clash: nothing else was named so; `layout_blob.h` is
  only a file name); TAG `"layout"` and log lines "no stored layout: empty Home", "no PSRAM for a %u
  byte layout", "layout read or CRC failed", "layout rejected (%s)", "layout loaded: ...", host_link
  "layout saved: restarting", main.cpp "after the layout load"; comments in host_link, main.cpp
  ("the key layout starts with one empty page"), standalone_task.h, layout_apply.h. Grep of
  examples/, cli/, host/, docs/, CLAUDE.md and wip-*.md: **no file quotes the old log lines** (only
  this job doc and step 15's expected `layout loaded: ...`), so steps 9/14 have nothing to update for
  them. After: FW 556/556, `idf.py --preset debug build` OK.
- **Step 6 (done):** `MSG_LAYOUT_BEGIN/DATA/COMMIT`, `OnLayoutBegin/Data/Commit`; GET_INFO has no
  named fields in the firmware (just `w.U32` writes): the local `LayoutInfo config` -> `stored`.
  Comments: "layout commit/committed", "the layout's Home", hosted sense -> "edit the keys/screen",
  `ST_NOT_HOSTED` "a key/screen request", "demo keys", "wire layout" -> "wire format". TestProtocol:
  helpers `LayoutBegin/LayoutData/UploadLayout`, `Fact("Protocol Layout ...")` (incl. "Backlight
  Layout And Wifi", "Without A Layout"), "sample layout". Its `// Layout upload (docs/protocol.md
  "Configuration upload")` section name left for step 11. After: FW 556/556, FW build OK.
- **Step 7 (done):** host `Msg.LAYOUT_*`, `LAYOUT_DATA_PREFIX`, `encodeLayoutBegin/Data`,
  `info.layoutCapacity/Size/Crc`, `uploadLayout()`, `LAYOUT_COMMIT_TIMEOUT`/`LAYOUT_WINDOW`, error
  text "Empty layout"/"Layout is N bytes", upload.js log/error text ("different layout"). FakeDevice's
  upload emulation option/member `config` -> `layout` (`_layout()`, `infoPayload({ layout })`; USB
  `configuration` lines kept), so `dev.layout.stored` in host tests and **cli/test/cli.test.js**
  (lines 82-104, needed for step 7 to pass). takeControl comment and host/README.md modes bullets
  reworded to "edits the screen"/"clears the keys". `"Configuration upload"` doc-section mentions
  (upload.js, upload.test.js, ControlBlock.test.js, README anchor) left for step 11. After: host 212,
  CLI 17.
- **Step 8 (done):** `loadLayoutFile` (files.js, index.js, entries.test.js, README); word-boundary
  `config`/`Config`/`configs` -> `layout*` over bake sources, index/browser.js, bake tests, helpers,
  fixtures (`exampleDir({ layout, name = "layout.json" })`), README (with `config-format` links
  protected; they stay until step 10). Because of the fixture default, cli.test.js line 46
  (`exampleDir({ layout: ... })`) and 52 (`"layout.bin"`) changed here; the rest of cli.test.js /
  args.test.js wording is step 9. After: host 212, CLI 17, re-bake of `sample_layout.bin`
  byte-identical (git shows a pure rename).
- **Step 9 (done):** cli `args.js` usage/errors, `bakeLayout()`, package.json, README (wording),
  args/cli tests (`layout.json` etc.); examples: `const layout`, header comments, README "##
  Standalone layouts", test-server comments. No old log lines were quoted anywhere (step 5).
  After: CLI 17, host 212, test-server 14, `node --check` on the 4 example .mjs OK.
- **Step 10 (done):** `git mv docs/layout-format.md`, retitled, body reworded (also its
  `config_types.gen.h` -> `layout_types.gen.h`, `#layout-upload`). Links fixed in the 14 files
  listed (incl. two bare "config-format" words in wip-macro-pad.md). Only CLAUDE.md (step 13)
  still names `config-format.md`. Host 212.
- **Step 11 (done):** protocol.md renamed + hosted sense reworded (RESET "Clear the keys",
  NOT_HOSTED "edits the screen", "screen requests", "Keys and images are never saved to flash. Only
  the standalone layout is"); one-sentence note in "## Layout upload" that only the names changed
  (worded without the old `CONFIG_*` names so the grep stays clean). `#configuration-upload` and
  quoted `"Configuration upload"` fixed in cli/host READMEs, standalone.md, layout_store.h,
  TestLayoutStore.cpp, TestProtocol.cpp, upload.js, upload.test.js, ControlBlock.test.js. Only
  CLAUDE.md left. FW 556/556, host 212.
- **Step 12 (done):** standalone.md (identifiers, "## Layout", "Layout storage and upload", the
  `layout` partition with a note on the old `config` label + one full `idf.py flash`, the "CBCF"
  magic note, hosted-sense rewording, historic `config_layout.h`/`config_view.h`/`ConfigView` reworded
  to "hand-written struct header and view class"/"the blob root"/"the op header, now
  `layout_types.gen.h`"); "Config storage" quoted in partitions.csv and TestLayoutStore.cpp ->
  "Layout storage"; standalone-ops-plan.md (names, `LAYOUT_MAX_OP_DATA`, "binpack's own struct
  offsets"); backlight.md (layout blob/upload, RESET "clears the keys", "WAKE event format");
  wifi.md ("stored layout", "layout upload", `LayoutStore`; IDF "setting the config" kept). Also
  image-assets.md hosted-sense lines 13/86/168/191 (see Flagged). Docs grep: only Kconfig/IDF/USB,
  "configurable/configured" and the two deliberate historic notes remain. FW 556/556, host 212,
  CLI 17.
- **Step 13 (done):** CLAUDE.md: new "Naming" bullet at the top of the firmware module list (layout
  = standalone file/blob/stored; key layout = live keys; hosted "screen requests"; other config senses
  unrelated; old name until 2026-11-08, complete-*.md keep it); module bullets (`layout_*`,
  `layoutdata`, `LAYOUT_ACTIVITY_NONE`, `LayoutApplyHome/Activity(..., keys)`, `LayoutStore::Read`,
  `layout_boot_load`, `layout_summary()`), `layout` partition + one full `idf.py flash` + "CBCF" magic
  kept, `#layout-upload`, "Layout storage", re-bake/regenerate commands with `sample_layout.*`/
  `layout_types.gen.h`, Repository Layout list (`layout-format.md`, sample_layout, "layout bake",
  "Hardware test layouts", binpack "standalone layout"), Open Items ("Layout upload", "Layout boot
  load", `uploadLayout()`, layout blob/upload/stored/"from a layout"). Hosted sense: "any screen
  request", "screen requests are refused", model "image store + key layout", RUN_STRESS "key layout
  in test_keys.h"/"stress key layout"; test build "match the device's struct shapes". Kept:
  sdkconfig/`CONFIG_*`, "configurable", "configuring the device" (web/), "config structs" convention,
  `mk config=release`, "Repository Layout".
- **Step 14 (done):** wip-lua-hardware-checks.md (test layout, stored layout, `LayoutApply Uploads
  After Home Stay Live` (TestLayoutApply.cpp), layout blob, "B. Script layout", "repository layout
  lists the bake test layouts"), wip-macro-pad.md (layout_store.cpp/TestLayoutStore.cpp in the crc32
  notes, bake/layout side, layout upload/blob, the layout's `control` strings, `sample_layout.bin`),
  wip-node-test-server.md ("Layouts:"). **Sweep:** `git grep` for the old identifiers (`cfgdata`,
  `sample_config`, `config_types`, `config_(store|flash|blob|apply|boot|summary|env)`, `Config*`
  types, `configRoot`, our `CONFIG_*` macros, `uploadConfig`, `loadConfigFile`, `info.config*`,
  `config-format`, `configuration-upload`, `bakeConfig`, `OnConfig`): only Kconfig
  `CONFIG_PARTITION_TABLE_CUSTOM*` in sdkconfig.defaults. Case-insensitive `config` per file: leftovers
  found and fixed in comments/test names of activity_switch.cpp/.h, key_gesture.h, op_exec.h,
  script_host.h, standalone.cpp/.h, trigger_dispatch.h, TestActivityHost/ActivitySwitch/ScriptHost/
  Standalone/TriggerDispatch.cpp, script_env.h ("the layout", "sample layout", Fact names "...Outside
  The Layout...", "Overrides The Layout's", "Without A Layout"). Everything else is allow-listed:
  Kconfig/sdkconfig, IDF/driver structs, USB descriptors (usb.cpp, ControlBlock.js 94-97,
  FakeDevice.js 68-71), Wi-Fi (`WifiConfig`, `wifi_config()`, wifi_credentials, wifi.md IDF bits),
  build configs, pad/ mentions in wip-macro-pad.md, "configurable/configured", the "CBCF" comments
  (layout_store.h, standalone.md), partitions.csv's old-label note and protocol.md's rename note.
  **Verify:** FW 556/556 (debug/release, equal to the baseline), `idf.py --preset debug build` and
  `--preset release build` OK, host 212, CLI 17, re-bake of `sample_layout.bin` byte-identical
  (git shows a pure rename). Status left at Implement: step 15 (hardware) is the user's.

## Flagged assumptions
- Step 13: added a short "Naming" glossary bullet to CLAUDE.md's firmware module list (the plan asked
  for a one-line glossary; placement was my choice).
- Steps 9-12: `docs/image-assets.md` was not in step 12's list but had hosted-sense "layout"
  that collided ("Not saved to flash ... same as the rest of the layout"): reworded 4 lines to
  "keys"/"key layout". `page-compositor.md`/`screen-buffers.md` "whole-layout reload" left as is
  (internal compositor/key-layout sense; screen-buffers.md was to stay unchanged).
- Step 12: two deliberate mentions of the old name stay as history: protocol.md's "Layout upload"
  note ("the standalone "config" was renamed") and standalone.md's partition/magic notes (old
  `config` label, "CBCF"). Step 14's sweep should allow them.
- Step 1: the optional `check_layout`/`set_demo_layout` renames in main.cpp were skipped (less churn;
  referenced by wip-lua-hardware-checks.md).
(All accepted by the user: see *Decided* above.)
- **Module names:** `layout_store`, `layout_flash`, `layout_blob`, `layout_apply`, `layout_boot`,
  `layout_types(.gen).h`; C++ prefix `Layout*` (`LayoutStore`, `LayoutError`, `LayoutSummary`,
  `LayoutApplyHome`...), macros `LAYOUT_*`, functions `layout_boot_load()` etc. `KeyLayout`/
  `key_layout` keep their names; the "free the names" renames in order step 1 are recommended.
- **Store result enum:** `LayoutStoreResult` (not `LayoutResult`, which is KeyLayout's today).
- **Namespace:** `cfgdata` -> `layoutdata` (alternatives: `lyt`, `ldata`). Root struct `layoutRoot`.
- **Partition label:** rename `config` -> `layout` (needs one full `idf.py flash`; the stored data
  survives). Alternative: keep the label `config` and only rename the macro, avoiding the reflash.
- **Flash header magic:** keep `0x46434243` ("CBCF") so stored blobs stay valid; only the macro
  name changes (`LAYOUT_MAGIC`), with a comment on why the bytes still say CF.
- **Protocol:** names only (`LAYOUT_BEGIN/DATA/COMMIT`, GET_INFO `layout_capacity/size/crc`), wire
  and protocol version unchanged; doc section "Layout upload" (`#layout-upload`).
- **No back-compat aliases** in the host library (`uploadConfig`, `loadConfigFile`, `info.config*`)
  or the CLI: all consumers are in this repo and nothing is published. Say so if a deprecated
  alias is wanted.
- **CLI:** command names unchanged (`bake`, `upload`); help text uses `<layout.json|.js|.mjs>`.
- **Docs file:** `docs/config-format.md` -> `docs/layout-format.md` (title "Layout file format").
  "Layout file" for the user's JSON/JS, "layout blob" for the baked BPAK, "stored layout" for flash.
- **Hosted-sense "layout"** in protocol.md, host README/ControlBlock.js comments, CLAUDE.md and
  standalone.md is reworded to "keys"/"key layout"/"screen", and `NOT_HOSTED`'s description to
  "a request that edits the screen". Status name `NOT_HOSTED` unchanged.
- **Wi-Fi `WifiConfig`** keeps its name (credentials, a different thing).
- **`complete-*.md`** keep the old names (not touched); `scratch/` ignored.
- **CLAUDE.md** edits are part of this job but, per Rule 4, are proposed to the user before applying.

## Plan

General rules for every step:
- Rename by identifier, never a blind `config` -> `layout` sweep (the senses in Notes section 0
  stay). Files are renamed with `git mv` so history follows.
- It is a rename: tests are renamed with the code, no test is dropped. Before a step, record the
  firmware test count from `mk run` (debug and release) and the `npm test` pass counts; after it
  they must be the same (and debug == release; a mismatch means a buffer overrun, see CLAUDE.md).
- Verification names used below:
  - **FW tests:** `mk run` and `mk config=release run` in `firmware/test`, both green with equal
    counts.
  - **FW build:** `idf.py --preset debug build` per CLAUDE.md's PowerShell recipe, 0 errors (the
    only check for device-only files: `layout_flash`, `layout_boot`, `host_link`, `main`,
    `standalone_task`; Rule 3: these can't be unit tested, hardware checks in step 15).
  - **Host tests:** `npm test` in `host/`. **CLI tests:** `npm test` in `cli/`.
  - **Bake check:** re-bake the sample from the repo root and confirm the `.bin` is byte-identical
    to the committed one (`git diff --stat` shows no change to it).
- Each step ends with a grep for the identifiers it renamed (expect only the allowed senses) and
  notes anything learned in *Notes*.

- [x] 1. **Free the clashing names (no "config" renames yet).** `key_layout.h/.cpp`:
  `LayoutResult` -> `KeyLayoutResult`, `LAYOUT_MAX_PAGES` -> `KEY_LAYOUT_MAX_PAGES`,
  `layout_page_changed_t` -> `key_layout_page_changed_t`, and every user (model, protocol,
  config_apply, activity_host, swipe, main, tests). Rename `KeyLayout *layout` parameters/members in
  APIs that also take the blob or store to `keys` (`ConfigApplyHome`, `ConfigApplyActivity`,
  `ActivityApplier`, `Protocol::Init` and its member). Optional per Notes: `check_layout`/
  `set_demo_layout` in main.cpp -> `check_keys`/`set_demo_keys`. Host: `test/FakeDevice.js`
  `LAYOUT_REQUESTS` -> `HOSTED_REQUESTS` (+ its users). Verify: FW tests, FW build, Host tests.
- [x] 2. **Schema, codegen and the types header.** `host/src/bake/schema.js` `ROOT_TYPE` ->
  `"layoutRoot"`; `headers.js` `TYPES_HEADER_PATH` -> `layout_types.gen.h`; `git mv`
  `config_types.h` -> `layout_types.h` and regenerate with `node tools/codegen/gen-types.js` (repo
  root), then `git rm` the old `config_types.gen.h`. In the same step (it won't build otherwise):
  namespace `cfgdata` -> `layoutdata`, `configRoot` -> `layoutRoot`, `ConfigKeyImageOffset`/
  `ConfigKeyImagePtrOffset` -> `Layout*`, `CONFIG_ACTIVITY_NONE` -> `LAYOUT_ACTIVITY_NONE`, every
  `#include "config_types.h"`, `config_env.h`'s `CB_OFF(configRoot, ...)`, `gen-types.js` comment,
  `TestConfigTypes.cpp` contents (file renamed in step 3), `host/test/bake/types-header.test.js`,
  `consts.js` comments. Reword "struct layout" wording in `layout_types.h` (Notes section 2, last
  rows). Verify: Host tests (types-header drift test), FW tests, FW build, Bake check.
- [x] 3. **Blob and apply modules + test data.** `git mv` `config_blob.*` -> `layout_blob.*`,
  `config_apply.*` -> `layout_apply.*`, `TestConfigBlob/Apply/Types.cpp` -> `TestLayout*.cpp`,
  `config_env.h` -> `layout_env.h` (namespace `layout_env`), `test/data/sample_config.json/.bin` ->
  `sample_layout.json/.bin`. Identifiers: `ConfigError`(+`ConfigErrorName`) -> `LayoutError`,
  `ConfigSummary`, `ConfigInfo`, `ConfigImage`, `ConfigValidate`, `ConfigRelocate`, `ConfigRoot()`,
  `ConfigApplyHome`/`ConfigApplyActivity` -> `Layout*`; macros `CONFIG_BLOB_VERSION`, `CONFIG_MAX_*`
  -> `LAYOUT_*`; `Fact("Config...")` names -> `Fact("Layout...")`. Update every consumer
  (activity_host, activity_switch, standalone, trigger_dispatch, script_host/api, op_*, protocol,
  host_link, config_boot's includes, test files, `script_env.h`), `test.mk.js` sources,
  `firmware/main/CMakeLists.txt`, `host/src/bake/consts.js` cross-reference comments. "File layout"
  -> "File format" in `layout_blob.h`. Re-bake to the new name:
  `node cli/cli.js bake firmware/test/data/sample_layout.json -o firmware/test/data/sample_layout.bin`.
  Verify: FW tests (same count), FW build, Host tests, Bake check (byte-identical to the old .bin).
- [x] 4. **Store and flash modules + the partition.** `git mv` `config_store.*` -> `layout_store.*`,
  `config_flash.*` -> `layout_flash.*`, `TestConfigStore.cpp` -> `TestLayoutStore.cpp`.
  `ConfigStore` -> `LayoutStore`, `ConfigResult` -> `LayoutStoreResult`, `ConfigFlash` ->
  `LayoutFlash`, `config_flash_open` -> `layout_flash_open`, header macros `CONFIG_SECTOR`,
  `CONFIG_DATA_OFFSET`, `CONFIG_ERASE_AHEAD`, `CONFIG_MAGIC`, `CONFIG_HEADER_VERSION`,
  `CONFIG_HEADER_SIZE`, `CONFIG_PARTITION_NAME`/`SUBTYPE` -> `LAYOUT_*`. **Keep the magic value
  `0x46434243`** and add a comment saying the bytes still read "CBCF" so stored blobs survive the
  rename. `firmware/partitions.csv`: label `config` -> `layout` (+ comment: changing it needs a
  full `idf.py flash`); `LAYOUT_PARTITION_NAME "layout"`. `flash_env.h` keeps its name (update its
  macro uses). Users: protocol (`ConfigStore *config` param/`m_config` -> `layout_store`/
  `m_layout_store`), host_link (`s_config`), TestProtocol's store uses, `Fact` names. TAG `"config"`
  -> `"layout"` in layout_flash. Verify: FW tests, FW build.
- [x] 5. **Boot module and device log text.** `git mv` `config_boot.*` -> `layout_boot.*`;
  `config_boot_load`/`config_boot_apply`/`config_blob()`/`config_blob_size()`/`config_summary()` ->
  `layout_*`; TAG and log strings ("no stored layout: empty Home", "layout read or CRC failed",
  "layout rejected (%s)", "layout loaded: ...", host_link "layout saved: restarting", main.cpp
  "after the layout load"); callers in main.cpp, host_link.cpp, standalone_task.cpp, CMakeLists.
  Grep `examples/` and `wip/wip-*.md` for the old log lines and note which need updating (done in
  steps 9 and 14). Verify: FW build (device-only code), FW tests.
- [x] 6. **Firmware protocol names.** `protocol.h/.cpp`: `MSG_CONFIG_BEGIN/DATA/COMMIT` ->
  `MSG_LAYOUT_*`, `OnConfig*` -> `OnLayout*`, GET_INFO field names `config_capacity/size/crc` ->
  `layout_*` (positions unchanged), comments; hosted-sense "layout" comments -> "keys"/"screen".
  `TestProtocol.cpp` (keep `WifiConfig` and USB senses). Opcodes 0x30-0x32 and the wire are
  unchanged: no protocol version bump. Verify: FW tests, FW build.
- [x] 7. **Host protocol layer and device API.** `host/src/protocol.js`: `Msg.CONFIG_*` ->
  `Msg.LAYOUT_*`, `CONFIG_DATA_PREFIX`, `encodeConfigBegin/Data` -> `encodeLayout*`, `info.configCapacity/
  Size/Crc` -> `info.layout*`. `ControlBlock.js`: `uploadConfig()` -> `uploadLayout()`,
  `CONFIG_COMMIT_TIMEOUT`/`CONFIG_WINDOW` -> `LAYOUT_*`, error text ("Empty layout", "Layout is N
  bytes"), comments (keep the USB `configuration` lines; hosted-sense "layout" -> "keys"/"screen").
  Callers: `src/bake/upload.js` (`info.layout*`, log/error text), `FakeDevice.js` (keep USB lines),
  `test/ControlBlock.test.js`, `protocol.test.js`, `bake/upload.test.js`, and any use in `cli/` or
  `examples/` (grep `uploadConfig|configSize|configCrc|configCapacity`). No aliases. Verify: Host
  tests, CLI tests.
- [x] 8. **Host bake and library surface.** `files.js` `loadConfigFile` -> `loadLayoutFile` (+
  `index.js` export, `entries.test.js` export list), `bake.js` (`bake(layout, ...)`, "in the
  layout"), `resolve.js`, `diag.js`, `look.js`, `ops.js`, `host-node.js`, `index.js`, `browser.js`
  comments; bake tests and helpers (`files.test`, `helpers.js`, `render.test`, `fixtures.js`,
  `pack.test`, `look.test`, `bake.test`, `resolve.test`); `host/README.md` (API names, "layout
  file"/"layout blob", hosted-sense rewording at ~61-62). Doc links to `config-format.md` stay
  until step 10. Verify: Host tests, CLI tests, Bake check.
- [x] 9. **CLI and examples.** `cli/args.js` help/errors (`<layout.json|.js|.mjs>`, "<layout
  name>.bin", "bake needs one layout file", "upload needs one layout or .bin file"), `cli.js`
  `bakeConfig()` -> `bakeLayout()`, `package.json` description, `cli/README.md`,
  `test/cli.test.js`, `test/args.test.js`. Command names unchanged. `examples/`: `activity-test.mjs`,
  `script-test.mjs` (`const config` -> `const layout`, header comments), `test-common.mjs`,
  `README.md` ("## Standalone layouts"), `test-server/test-server.mjs` (+ its test),
  `net_test_server.py` comments, and any old log lines found in step 5. Verify: CLI tests, Host
  tests, `node --test` on `examples/test-server/test-server.test.mjs`, and `node --check` on the
  touched `.mjs` examples (they run only against hardware).
- [x] 10. **Rename the format doc and fix every link.** `git mv docs/config-format.md
  docs/layout-format.md`, retitle "Layout file format (user level)", reword its body ("layout
  file"/"layout blob"/"stored layout"). Fix every link (grep `config-format`): cli/README.md,
  examples/README.md, host/README.md, host/index.js, host/src/bake/{consts,look,ops,resolve}.js,
  host/test/bake/{bake.test,fixtures,look.test}.js, docs/standalone.md, docs/standalone-ops-plan.md,
  wip-macro-pad.md (CLAUDE.md in step 13). Anchors (`#script-api`...) unchanged. Verify: Host
  tests, grep shows no `config-format` outside `complete-*.md`.
- [x] 11. **protocol.md.** `CONFIG_BEGIN/DATA/COMMIT` -> `LAYOUT_*`, both-modes table, GET_INFO
  `layout_capacity/size/crc`, "## Configuration upload" -> "## Layout upload", status rows 8-10,
  hosted-sense "layout" -> "keys"/"screen" (NOT_HOSTED: "a request that edits the screen"; "Keys and
  images are never saved to flash. Only the standalone layout is"). Note that the wire is
  unchanged. Fix every `#configuration-upload` link across the repo (grep; ~17: firmware headers,
  host, docs, CLAUDE.md is step 13). Verify: grep for `configuration-upload` and `CONFIG_BEGIN`
  (only complete-*.md); Host tests (comments only, sanity).
- [x] 12. **Other docs.** `docs/standalone.md` ("## Layout", "Config storage" -> "Layout storage"
  and fix its anchor's links incl. `layout_store.h`; module/macro names; historic `config_layout.h`/
  `config_view.h` mentions; "partition layout" -> "partition map"; partition label `layout` and the
  one-time full flash; keep `CONFIG_TINYUSB_SUSPEND_CALLBACK`; step 1 "Requests that define the
  layout" -> "the keys"), `standalone-ops-plan.md`, `backlight.md` (2), `wifi.md` (2 of 15).
  `screen-buffers.md`, `backlight-waveshare-support.md` unchanged. Verify: grep docs/ for
  standalone-sense `config`/`Config` (allow-list in step 14).
- [x] 13. **CLAUDE.md (approved by the user).** Firmware module bullets (`layout_store/flash/blob/
  apply/boot`, `layoutdata`, `LAYOUT_ACTIVITY_NONE`, `LayoutApplyHome`/`LayoutApplyActivity`,
  `LayoutStore::Read`, `layout_boot_load`, `layout_summary()`), the regenerate/re-bake commands
  (`layout_types.gen.h`, `sample_layout.json/.bin`), the Repository Layout docs list
  (`layout-format.md`), host/ description ("the layout bake"), protocol anchors (`#layout-upload`),
  Open Items ("Layout upload", "Layout boot load", "the layout blob", `uploadLayout()`, "from a
  layout"), host test data line; hosted-sense rewording ("any key request"/"screen request" for
  "any layout request", model "store + keys"); add the partition note (label `layout`, a full
  `idf.py flash` once after this rename) and a one-line glossary: "layout" = the standalone
  config (file / blob / stored), "key layout" = the live pages of keys. Keep "Repository Layout",
  sdkconfig/`CONFIG_*` Kconfig and `WifiConfig` senses.
- [x] 14. **wip docs and final sweep.** Update `wip/wip-lua-hardware-checks.md` (test names
  `LayoutApply ...`, "test layout", "stored layout", log lines), `wip/wip-macro-pad.md`
  (module/test/data names, `layout-format.md`, "bake/layout side"), `wip/wip-node-test-server.md`.
  Never touch `wip/complete-*.md`. Then grep the repo (excluding submodules, `node_modules`,
  `build/`, `scratch/`, `complete-*.md`) for `config`/`Config`/`cfgdata`/`sample_config`/
  `config_types`/`CONFIG_(MAX|BLOB|MAGIC|SECTOR|PARTITION|ACTIVITY|HEADER|DATA|ERASE)` and check
  each hit is in the allow-list: Kconfig `CONFIG_*`/sdkconfig, IDF/driver config structs, USB
  configuration descriptors (`usb.cpp`, `ControlBlock.js` ~94-97, `FakeDevice.js` ~68-71), Wi-Fi
  credentials (`WifiConfig`, `wifi_config()`, docs/wifi.md IDF bits), build configs (`mk config`,
  `CMakePresets.json`, `test.mk.js`), `pad/`, English "configure(d)" unrelated to the blob, the
  "CBCF" magic comment, and this job document. Fix any leftover. Verify: FW tests (debug and release,
  counts equal to the start of the job), FW build, `idf.py --preset release build`, Host tests, CLI
  tests, Bake check. Then set the document to Complete per the IPI process (rename to
  `complete-rename-config-to-layout.md`) only after step 15 is reported by the user, or leave it
  for the coordinating agent to decide.
- [x] 15. (2026-11-08, user: full flash, stored layout loads, upload works; (d) not run
  separately — the rename didn't touch the take/release paths beyond message names.
  The upload run that printed "(root): expected an object, got undefined" was `examples/demo.js`
  passed to the CLI: it isn't a layout (no default export), and importing it ran the demo.)
  **Hardware checks (user only; can't be automated).** (a) Full `idf.py --preset release
  -p COM4 flash monitor` (not app-only: the partition table changed), check `Hash of data
  verified.`; (b) the blob stored before the rename still loads at boot (`layout loaded: ...`, Home
  keys shown; the data at 0x210000 survives and the magic is unchanged); (c) `node cli/cli.js
  upload examples/upload-test.json` uploads, restarts and verifies, and a second run skips as
  unchanged; (d) take/release control still works (`examples/test-server/takeover.mjs`).
