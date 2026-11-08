# urem2: the layout

Status: Complete (shared investigation in `wip/complete-urem2.md`). Verified against the test server on the
hardware 2026-11-08; real devices and the pad not yet tried.

## Description
The urem2 standalone layout as `.mjs` modules in `urem2/`: device modules (op builders), complete code tables
(IR codes, Apple TV keys/apps, Yamaha commands), Home, the activities (Apple TV, TV/HDD, Steam, PlayStation),
macro pad and encoder bindings, and a bake-time switch between the test server and the real devices. Tested
(bakes, op builders, tables) where possible. Ends with `urem2/README.md`: how to run it and what couldn't be ported
(missing ControlBlock features), per urem feature.

Part of the umbrella job `wip/complete-urem2.md`; read that first. Follows `wip/complete-urem2-test-server.md`.

## Notes
Shared findings (ControlBlock capabilities, grid mapping, gap list, questions) are in `wip/complete-urem2.md`; tables,
addresses and device protocols in `wip/complete-urem2-test-server.md`. Here: the urem feature map.

**Hardware check (2026-11-08):** everything worked against the test server except the Steam slideshow's six UDP
keys. The device sent them (its log showed each), but the server never saw them, while UDP 7777 (ATV) and HTTP on
TCP 4500 were fine. UDP 4500 is the IPsec NAT-T port, and Windows drops inbound UDP on it from other machines (a
packet sent from the server PC itself got through). Fix: `targets.test.slideshow.udpPort` is 4501, and a test in
`urem2/targets.test.mjs` keeps the test target's UDP ports off 500 and 4500. The real slideshow (NAS, Linux)
keeps 4500.

### layers library semantics (as urem uses it)
- `Layer.add(handlers)`: activating a layer calls `onActivate` on each handler; a `StreamDeckButton.onActivate`
  calls `manager.setButton(index, this)` and remembers the previous handler, restored on deactivate. So **the last
  activated button wins a slot** (a stack). `core.js` keeps a root layer (`[0,0]` Home toggle) and swaps the current
  "activities layer" (Home or an activity's `rootLayer`) inside it. `Group` = one active layer of several
  (`StreamDeckPager` pages, with up/down arrow buttons `[0,1]`/`[0,2]`): **pages map to ControlBlock pages** (swipe).
- GPIO (`gpio.js`): **all** registered listeners for a pad button are called (not last-wins); in practice each
  activity binds buttons 0..5 and `volume.js` binds button 6 + the encoder **globally** (always active, also on Home).
- Button options: `press`, `release`, `longPress` (+`longPressDelay`, 500 ms; suppresses the following release, ends
  repeat), `repeat` (number = period and initial delay; object `{initialDelay, period}`: `press` re-fired while held).
  `autoPressEffect` shrinks the image while pressed (Stream Deck only).

### Activity switching (core.js) → ControlBlock
- `switchActivity(a)`: `onDeactivate` of old (no activity defines one), turn on `a.devices` (all, in order, awaited),
  turn off old ones not in `a.devices`, set layer, `a.onActivate()`. ControlBlock: `devices` + `didActivate` hook,
  same order. `switchActivity(null)` (Home power key): all devices off, Home, display off → `{op:"switchActivity",
  activity:null}` (display off: gap 4).
- Devices (op lists):
  - `yama`: on `httpGet .../main/setPower?power=on`, off `...power=standby`.
  - `lgtv`: on IR `lg.powerOn`, off IR `lg.powerOff`.
  - `atv`: on IR `atv.powerOn` + daemon `ping` (urem `connect()`); off IR `atv.powerOff` + daemon `disconnect`.
  - `pana` (DVR): no on/off (empty device; keep it so the activity lists it, or drop it: Plan).
  - `hotpc`: on WoL `04:d4:c4:4e:da:2b`; no off.
- Activities (name → devices, didActivate, displayName):
  `atv` [yama, lgtv, atv] `setInput hdmi2`; `hdd` [yama, lgtv, pana] `hdmi1`; `steam` [yama, lgtv, hotpc] `hdmi3`;
  `playstation` [yama, lgtv] `hdmi4`, no keys.

### Home (urem `homePage.js` + `core.js`), index = row*5+col
- 0 `[0,0]` Home toggle: empty. 1 `#playstation` → switch playstation. 2 `#steam` → switch steam. 3 Moonraker: empty.
  4 `#power` red, svgScale 0.7 → switch none.
- 5 clock: empty. 6 `#apple-tv` `#606060` → switch atv. 7 `icons/netflix.svg` → switch atv + `app
  com.netflix.Netflix`. 8 `icons/prime.svg` → atv + `app com.amazon.aiv.AIVApp`. 9 `icons/youtube.svg` → atv + `app
  com.google.ios.youtube` (gap 2 for 7-9; plus gap 3: urem awaited the daemon connection before `app`).
- 10 label "7ch Stereo" purple → Yamaha 7ch toggle (script). 11 label "TV" orange `bold 28px sans-serif` → switch hdd.
  12 `#list` orange → hdd + IR `pana.videoMenu`. 13 `#guide` orange → hdd + `pana.guide`. 14 `#function` orange → hdd
  + `pana.functionMenu` (gap 2).
- Unused in urem (commented out): `icons/yamaha.svg`, `icons/lg.svg` keys.

### Activity atv (1 page) — daemon UDP `key <name>` unless noted
- 1 `#previous-track` white → `previous`. 2 `#next-track` white → `next`. 6 `#rewind` → `left` (repeat 100). 7
  `#forward` → `right` (repeat 100). 10 "7ch Stereo" purple → toggle. 11 `#play` orange → `play`. 12 `#pause` orange
  → `pause`. 13 `#stop` orange → `menu`. 14 `#volume-mute` purple → Yamaha mute toggle (script). 0, 5 empty (nav,
  clock).
- Pad: key1 tap → `menu`, long → `key menu hold`; key2 → `up` (repeat 500/100); key3 tap → `select`, long → `key select
  hold`; key4 → `left` (repeat); key5 → `down` (repeat); key6 → `right` (repeat). In urem keys 1/3 fire on
  **release** (tap), arrows on **press** → `tap` with no `long` fires on press. Repeats: gap 1.

### Activity hdd (3 pages) — IR `pana.<name>` via irtx
- Page 1: 1 `#previous-track` → previousTrack; 2 `#next-track` → nextTrack; 3 label "-10" → skipBack10; 4 `#option`
  green → option; 6 `#rewind` → reverse; 7 `#forward` → forward; 8 label "+60" → skipForward60; 9 `#info` green → info;
  11 `#play` orange → play; 12 `#pause` orange → pause; 13 `#stop` orange → stop; 14 `#volume-mute` purple → mute
  toggle.
- Page 2: number keys (label `"N"`, font `bold 24px sans-serif` (urem typo "san-serif"), labelOffset y -3) → `btnN`:
  1,2,3 at 1,2,3; 4,5,6 at 6,7,8; 7,8,9 at 11,12,13; 0 at 14. 4 `#refresh` orange scale 0.7 offset y -10 + label "CH"
  offset y 13 → lastChannel. 9 `#guide` orange → guide.
- Page 3: 1..4 `#square` scale 0.6 red/green/yellow/blue → red/green/yellow/blue; 6 label "Sub- titles" → sttl; 9
  `#arrow-up` white scale 0.8 offset y -13 + "CH" offset y 3 → channelUp; 11 "Drive Select" → driveSelect; 12
  `#delete` red → delete; 13 `#circle` red scale 0.7 offset y -10 + "Record" labelVAlign bottom offset y -6 → record;
  14 `#arrow-down` white scale 0.8 offset y 9 + "CH" offset y -11 → channelDown.
- Pad (all on press): key1 → return; key2 up (repeat); key3 ok; key4 left (repeat); key5 down (repeat); key6 right
  (repeat).

### Activity steam (2 pages)
- Page 1: 4 "Steam" → HOT PC `launch-steam`; 9 "Slide Show" → `launch-kiosk`; 14 "Shut Down" → `shutdown` (HTTP POST
  JSON `{}`). Slideshow UDP: 1 `#previous-track` → `prevDay`; 2 `#next-track` → `nextDay`; 6 `#rewind` →
  `backward20` (repeat 100); 7 `#forward` → `forward20` (repeat 100); 11 `#play` orange → `play`; 12 `#pause` orange →
  `pause`; 13 `#info` green → display-elements `all` toggle.
- Page 2 (display-elements toggles, labels): 1 "Play State" → `transport`; 2 "Date" → `date`; 3 "Time" → `time`; 4
  "Rating" → `rating`; 6 "Place" → `location`; 11 "Other" → `other`.
- Pad: key1, key3 nothing; key2 → `voteUp`; key4 → `prev`; key5 → `voteDown`; key6 → `next`.

### Activity playstation: no keys, no pad bindings (needs one page; gap 11).

### Volume (urem `volume.js`, global) → every activity's bindings
- Encoder: per tick `step = interval < 8 ms ? 5 : < 20 ms ? 3 : 1`; `setVolume?volume=up|down&step=<step>`. →
  `enc1`/`turn` script: parse `delta`/`interval`; one request per event with direction from the sign; |delta| > 1
  (coalesced) → send |delta| requests or multiply the step (Plan decision; Yamaha accepts `step` as an int).
- Pad key7 (encoder push) tap → mute toggle script (see flagged assumption on key7).
- Toggle scripts: `http.get(base.."/main/getStatus")`, then `body:find('"mute"%s*:%s*true')` →
  `setMute?enable=false|true`; `'"sound_program"%s*:%s*"7ch_stereo"'` → `setSoundProgram?program=surr_decoder|
  7ch_stereo`. On a failed get urem behaves as "not muted"/"not 7ch" (sets mute on / 7ch on).

### Icons
- Built-ins used (all present in this repo's node-buttonImage 1.1.1 `images/`; urem had 1.0.0): `home playstation
  steam power apple-tv list guide function previous-track next-track rewind forward play pause stop volume-mute option
  info refresh square delete circle arrow-up arrow-down` (`home` only on the nav button: unused).
- Colours: white (default), orange, purple, green, red, yellow, blue, `#606060`; `power` 0.7, `square` 0.6, `circle`
  0.7, arrows 0.8, `refresh` 0.7 (with offsets above).
- Custom SVGs (copy into `urem2/icons/`): `netflix.svg`, `prime.svg`, `youtube.svg` (used); `yamaha.svg`, `lg.svg`
  (unused). Note they have their own colours (no `#848484` to recolour).
- Scaling: node-buttonImage draws in 72 virtual px scaled to 128, so urem's values keep proportions. Material
  built-ins (`viewBox` with ~12% margin) fill ~77% of the key ≈ 99 px at 128: a little over the 80–96 px icon target;
  consider a global icon scale (~0.85) and check the look on the hardware. `iconScale` inherits by replacement, not
  multiplication, so a global factor has to be multiplied into the per-key scales in JS (one helper). ControlBlock's default
  `iconOffset.y` is -7 when a label is present (urem: 0 unless set).
- Labels: urem text-only keys used node-buttonImage's default font; ControlBlock defaults to `bold 15px` without an
  icon, `bold 11px` under an icon. "Sub- titles" relies on wrapping (node-buttonImage label wrapping exists).

### Layout structure ideas (for Plan)
- Modules: `tables/` (shared with the server), `targets.mjs`, `devices/*.mjs` (op builders: `irtx.send(code)`,
  `atv.key(name, action)`, `atv.app(id)`, `yamaha.get(path)`, `hotpc.post(cmd)`, `slideshow.cmd/toggle`), `scripts/
  *.lua`, `home.mjs`, `activities/*.mjs`, `pad.mjs` (shared volume bindings + per-activity pad helper), `build.mjs`
  (`buildLayout(target)`), entry files per target.
- Tests: op builders (byte-exact IR hex, URLs, JSON bodies), layout resolves with no errors/warnings for both targets
  (stage 1, real `createNodeHost`), every Home/activity key position matches the urem map, one full bake, Lua scripts
  against fakes if wasmoon is reachable.

### Plan decisions (2026-11-08)
- **Module map** (all under `urem2/`; relative imports, no package.json; tests are `*.test.mjs` beside the module,
  shared test helpers in `urem2/test/` (not named `.test.mjs`, so the glob doesn't run them)):
  - `layout/ops.mjs`: `makeOps(target)` → op builders over the shared tables (`ir`, `irPower`, `atvKey`, `atvApp`,
    `atvPing`, `atvDisconnect`, `yamaha(path)`, `hotpc(cmd)`, `wake()`, `slideshow(cmd)`, `slideshowToggle(el)`).
    Builders only wrap: every code/path/name comes from `tables/*.mjs` (which throw on unknown names), every address
    from the target.
  - `layout/lua.mjs`: `luaLiteral(v)` and `luaScript(file, consts)` → `{ op: "script", lua }` = generated prelude
    (`local NAME = <literal>` per const) + the `.lua` file from `urem2/scripts/` (read with `fs` relative to
    `import.meta.url`). Emitted as `lua:` source (not `file:`), so the bake still compile-checks and dedupes it.
  - `layout/scripts.mjs`: `yamahaToggle(target, "mute"|"sevenCh")` and `volumeScript(target)` (consts built from the
    `tables/yamaha.mjs` path builders, so no URL format is written twice).
  - `scripts/yamaha-toggle.lua` (one script for both toggles: `STATUS_URL`, `ON_PATTERN`, `ON_URL`, `OFF_URL`
    injected: pattern found → OFF_URL, else (incl. a failed get, as urem) → ON_URL), `scripts/volume.lua`.
  - `layout/look.mjs` (root defaults, colours, `ICON_SCALE`, `icon()`/`label()` look helpers), `layout/keys.mjs`
    (`key()` with the `repeat` placeholder, the shared 7ch/mute keys), `layout/pad.mjs` (`padBindings`,
    `volumeBindings`), `layout/devices.mjs` (devices, activity meta, hooks, `switchTo`, `resync`, activity key
    gestures), `layout/home.mjs`, `layout/activities/{atv,hdd,steam,playstation}.mjs` (each `(ctx) => activity`),
    `layout/build.mjs` (`buildLayout(target)`), entries `urem2/urem2.mjs` (real) and `urem2/urem2-test.mjs` (test).
  - `icons/{netflix,prime,youtube}.svg` (verbatim copies). Icon paths in the layout are relative to `urem2/` (the
    entry files' directory, which the bake resolves against).
- **Bake-time switch: two entry files** (confirmed: no better option; the CLI can't pass options and an env var would
  be invisible). Each is one line: `export default buildLayout(targets.real|test)`.
- **Encoder coalesced |delta|:** one request per event, `step = stepFor(interval) * |delta|` clamped to
  `VOL_MAX_STEP = 10` (urem's largest per-tick step is 5; coalescing at fast spin gives 2-3 detents). One request keeps
  the net queue (8, drops the oldest) from overflowing on a fast spin. Thresholds `VOL_FAST_US = 8000`,
  `VOL_MEDIUM_US = 20000`, steps 5/3/1 are JS constants injected into the script (one source for tests and Lua).
  Direction from the sign; delta 0 or an unparseable param → nothing sent. The URL per direction and step 1..10 is
  injected as two Lua arrays built with `setVolume(dir, step)` (exact, validated; ~1.5 KB).
- **Script requests:** the toggles wait for `getStatus` (`http.get(url)`), then set with the fire-and-forget form
  (`http.get{ url = ..., reply = false }`); the volume script is fire-and-forget only (no waiting holds the queue).
- **pana device dropped:** urem's DVR has no power control; an empty device adds nothing (ControlBlock would track an
  on/off state that does nothing). `hdd` lists `[yama, lgtv]`. Root devices order: `yama, lgtv, atv, hotpc`.
- **IR gap:** the blaster drops a third packet while one is in flight and one pending. Device power lists use
  `irPower(name)` = `[udpPacket, { op: "delay", ms: IR_GAP_MS }]` with `IR_GAP_MS = 120` (NEC frame ~68 ms + gap), so a
  switch (or a resync after a switch) never has three IR packets in flight. Single IR keys send one packet, no delay.
- **Resync (Decision 6):** Home's activity keys (playstation, steam, apple-tv, "TV"): `tap: switchTo(a)`, `long:
  [switchTo(a), ...resync(a)]`, `resync(a)` = every device's `turnOn` in the activity's order + its `didActivate`,
  taken from the same device/activity objects. Switching first makes long press also work from another activity (the
  switch's own lists run first, then the resync repeats them: harmless, and the IR gap keeps the blaster happy).
  Only the four activity keys; Netflix/Prime/YouTube and list/guide/function keep `tap` only.
- **Look (Decision 7):** root `defaults`: `face: { top: "#000000", bottom: "#000000", border: BORDER, radius: 14 }`
  (opaque black: a top/bottom pair gets no shade and bakes as long rle565 runs; "transparent" stop colours are a
  render risk), `pressed: { face: { top: PRESSED, bottom: PRESSED } }` (an explicit pressed face, so no 35% overlay;
  the border merges in). `BORDER = "#a0a0a0"`, `PRESSED = "#606060"` (mid grey; flagged). The bake supports this
  (`look.js` `faceDef`: `face` object with `border` → 2 px stroke; `explicitFace` → no overlay): no `image` hatch.
- **Global icon scale:** `ICON_SCALE = 0.9` (built-ins fill ~99 px at scale 1, over the 80-96 px target; 0.9 ≈ 89 px).
  `icon(name, { scale })` multiplies it into every key's `iconScale` (inheritance replaces, so one helper does it).
  urem's offsets are kept as authored. Tune on the hardware (one constant).
- **PlayStation page:** one page with the shared "7ch Stereo" (10) and mute (14) keys, the same positions as on atv;
  urem's PlayStation activity had no keys (its sound keys were global). Flagged.
- **Repeat placeholder (Decision 1):** `key(..., { repeat })` and pad specs accept `repeat: 100` or `{ initialDelay,
  period }` exactly where urem had it; `withRepeat()` in `keys.mjs` is the one place that ignores it today (later: emit
  the `repeat` gesture). Tests check the value is accepted and dropped from the output.
- **Pad events:** urem fired the arrows and the hdd/steam keys on press → bindings use `press` (exactly urem, and no
  question of when `tap` fires on the pad); atv key1/key3 → `tap` + `long` (`key menu hold`, `key select hold`).
  Volume bindings (`enc1`/`turn` → volume script, `key7`/`tap` → mute toggle) are spread into every activity.
  Control names `key1..key7`, `enc1` (macro pad decisions; the bake doesn't check names yet: `wip-macro-pad.md` step
  10 will, and these pass it).
- **Lua tests reach wasmoon** through `createRequire(<urem2>/../host/package.json).resolve("wasmoon")` + dynamic
  `import()` in one helper (`urem2/test/lua-harness.mjs`): no change to the host library's exports, no hard-coded
  `node_modules` layout. The harness defines the script API the scripts use (`http.get` positional → `body, status`
  or `nil, err`; table form with `reply = false` → `true`; `log`, `millis`; `param` global) over a sync JS fake, and
  the fake Yamaha's request logic is shared with the test server (refactored out as a pure `createFakeYamaha()`), so
  the toggles run against the real `getStatus` JSON shape.
- **Stage-1 test helper:** `urem2/test/resolve.mjs`: `resolveLayout(layout)` = `resolve(layout, await
  createNodeHost(<urem2 dir>))` (real icons, real files, real Lua compile check), plus `cleanLayout(...)` asserting no
  errors and no warnings with the messages in the failure.
- **Test command (every step):** from the repo root `node --test "urem2/**/*.test.mjs"` (all pass). Steps that touch
  nothing outside `urem2/` need nothing else.

### Implement notes
- Step 1: `createFakeYamaha()` (exported from `test-server/test-server.mjs`) → `{ state, handle(method, url) }`,
  `handle` → `{ status, type, body, log }` (url: a path or a full URL; the host is ignored). The HTTP listener
  (`fakeYamahaHttp`) says `log` and replies; log lines unchanged. Each call makes its own state. Tests: 61.
- Step 2: `test/resolve.mjs` exports `UREM2_DIR`, `resolveLayout`, `cleanLayout` (asserts 0 errors + 0 warnings,
  lists them) and `homeLayout(keys)` (`{ version: 1, home: { keys } }`); it imports `../../host/index.js` and caches
  one `createNodeHost(UREM2_DIR)`. `makeOps(target)` returns `ir, irPower, atvKey, atvApp, atvPing(), atvDisconnect(),
  yamaha(path), hotpc(cmd), wake(), slideshow(cmd), slideshowToggle(el)`; `IR_GAP_MS` exported. `yamaha(path)` only
  takes `/main/...` paths (from the table builders); `hotpc`/`slideshow` throw `unknown HOT PC command` / `unknown
  slideshow command`. ATV ops send the text as a plain string `data`. Tests: 77.
- Step 3: `layout/lua.mjs`: `luaLiteral(v)` (strings, safe integers, booleans, arrays, plain objects as
  `{["k"] = v}`; anything else, incl. floats and null, throws `luaLiteral: ...`; control bytes as `\ddd`),
  `luaSource(body, consts)` (pure: one `local` per const in object order, then the body; bad names/keywords throw),
  `luaScript(file, consts)` → `{ op: "script", lua }` (`file` resolved against `SCRIPTS_DIR` = `urem2/scripts/`, so
  an absolute path works: the toy fixture is `test/toy.lua`). `test/lua-harness.mjs`: `evalLua(src)` (returns the
  first value) and `runLua(src, { param, get(url, headers) → { status, body } | null, now })` →
  `{ error, logs, requests: [{ url, reply }] }`. A fire-and-forget get still calls `get` (the fake's state changes).
  The harness removes `load dofile loadfile require io os package debug coroutine` like the device. **wasmoon gotchas:**
  a JS object returned to Lua arrives as indexable userdata (fine for `r.body`), Lua tables come back as JS
  arrays/objects, and **strings are cut at a NUL** on the way back to JS (the NUL round-trip is compared inside Lua).
  Tests: 90.
- Step 4: `scripts/yamaha-toggle.lua` (3 lines) + `yamahaToggle(target, "mute"|"sevenCh")`. `ON_PATTERN` is built
  from the field and value (`"mute"%s*:%s*true`, `"sound_program"%s*:%s*"7ch_stereo"`) with Lua pattern magic
  escaped. A body that isn't a string (failed get) or lacks the pattern → `ON_URL`. Tests: 95.
- Step 5: `scripts/volume.lua` + `volumeScript(target)`; exports `VOL_FAST_US`, `VOL_MEDIUM_US`, `VOL_STEPS
  { fast, medium, slow }`, `VOL_MAX_STEP`. Thresholds exclusive (`interval < 8000` → 5, as urem). Tests: 100.
- Step 6: `layout/look.mjs` exports `BORDER`, `PRESSED`, `ROOT_DEFAULTS`, `COLORS` (urem's names + `grey`
  `#606060`), `ICON_SCALE`, `icon(name, { color, scale, offset })` → `{ icon, iconColor?, iconScale (always set,
  rounded to 6 places: 0.9 × 0.7 = 0.63), iconOffset? }` and `label(text, { color, font, valign, offset })` →
  `{ label, labelColor?, font?, labelVAlign?, labelOffset? }`. Offsets fill a missing x/y with 0. Unknown options,
  colours not in `COLORS`, a bad valign and an icon that is neither `#name` nor `.svg` throw. Combine an icon and a
  label by spreading both. Tests: 105.
- Step 7: `layout/context.mjs`: `makeContext(target)` → `{ target, ops: makeOps(target) }`, the `ctx` every module
  takes. `layout/keys.mjs`: `key(look, ops, { long, repeat })` → `{ ...look, tap, long? }`; `withRepeat(spec)`
  validates (`positive int` or exactly `{ initialDelay, period }`, positive ints) and drops `repeat`; `sevenChKey(ctx)`,
  `muteKey(ctx)`. `layout/pad.mjs`: `PAD_CONTROLS` (`key1..key7, enc1`), `padBindings(spec)` (spec per control:
  events + optional `repeat`; output in `PAD_CONTROLS` order, then `press, release, tap, long` (`turn` for enc1));
  `volumeBindings(ctx)` = key7 tap mute, then enc1 turn volume. Tests: 116.
- Step 8: `layout/devices.mjs`: `devices(ctx)` (root map, order yama, lgtv, atv, hotpc), `ACTIVITIES` (plain data:
  `devices`, `input`, `displayName`), `hooks(ctx, a)`, `activityMeta(ctx, a)` → `{ displayName, devices, hooks }`
  (spread into an activity with its pages/bindings), `switchTo(a | null)`, `resync(ctx, a)`,
  `activityKeyGestures(ctx, a)` → `{ tap, long }` (spread into a Home key with its look). Unknown activity names throw.
  The longest list is a resync long (steam: 1 + 1 + 2 + 1 + 1 = 6 ops), far under 64. Tests: 128.
- Steps 9-12 shared helpers: `keys.mjs` `grid({ slot: key })` → 15 keys, `null` elsewhere (throws on a slot outside
  0..14) and `transportKeys({ previous, next, rewind, forward, play, pause, stop? }, { repeat })` → urem's transport
  row (slots 1, 2, 6, 7 white; 11, 12, 13 orange; `repeat` on rewind/forward; `stop` optional for steam's slot 13).
  `pad.mjs` `ARROW_REPEAT` and `padArrows(opsFor)` (key2/4/5/6 = up/left/down/right on press, with the repeat; the
  ATV keys and the pana IR codes share those names). `layout/activities/activity.mjs` `defineActivity(ctx, name, {
  pages: [slotMap...], pad })` → `{ ...activityMeta, pages: [{ keys: grid(...) }], bindings: [...padBindings(pad),
  ...volumeBindings(ctx)] }` (refuses no pages, and `key7`/`enc1` in `pad`: they are the volume's). Test helper
  `test/slots.mjs`: `assertSlots(keys, { index: key })` (every other slot must be `null`) and `layoutWith(ctx, { home,
  activities })` (root defaults, devices, all four activities, stubs for the ones not given; activities keep
  `ACTIVITIES` order, so `tree.activities[i]` is atv, hdd, steam, playstation). Tests spell every slot out with
  `key`/`icon`/`label` and the op builders (not through `transportKeys`/`padArrows`), against the urem sources.
  Two-target loops (real and test) in every module test.
- Step 9: `icons/{netflix,prime,youtube}.svg` copied (`cmp` identical). `home(ctx)` → `{ keys }`. The custom SVGs
  resolve clean (no warnings). Tests: 137 (incl. the step 9-12 helper tests above).
- Step 10: `activities/atv.mjs`. The `repeat` placeholder can't be seen in the output (dropped by `withRepeat`), so the
  tests can only show it is accepted where urem had it. Tests: 147.
- Step 11: `activities/hdd.mjs` (local `irKey`, `num`, `ch` (icon + "CH" label), `square` helpers). urem's hdd page 1
  rewind/forward have **no** repeat (unlike atv/steam): kept. Tests: 159.
- Step 12: `activities/steam.mjs`, `activities/playstation.mjs`. Tests: 173.
- Step 13: `layout/build.mjs` `buildLayout(target)` → `{ version: 1, defaults: ROOT_DEFAULTS, devices, home,
  activities }` (activities in `ACTIVITIES` order through a name → builder map). Entries `urem2.mjs` (real) and
  `urem2-test.mjs` (test), one line each plus a header comment with the bake/upload commands. `build.test.mjs` walks
  the resolved tree for every packed op (`OP` codes from the host library) and checks every UDP host, HTTP URL host
  and every `http://` host inside a Lua source is one of the target's own hosts and none of the other target's. The
  entry files are checked with `loadLayoutFile` (deep-equal). Tests: 180.
- Step 14: `layout/bake.test.mjs` bakes `urem2-test.mjs` with `bakeFile` (BPAK magic, no warnings) and logs size and
  image count as a test diagnostic: **133,804 bytes, 56 distinct images** (~1 s). CLI by hand: `scratch/urem2-test.bin`
  133,804 bytes, `scratch/urem2.bin` 133,560 bytes (both exit 0, no warnings). Tests: 181.
  - **Rendered keys** (`scratch/urem2-keys.mjs` renders normal | pressed, bg + fg over black, to
    `scratch/urem2-keys/*.png` and prints the fg's opaque bounding box): the border (2 px `#a0a0a0`, radius 14) and the
    grey pressed fill look as intended; icons and labels sit on top unchanged when pressed. Labels ("7ch Stereo",
    "Sub- titles", "Steam", "Record") are bold, centred and wrap cleanly.
  - **Icon size: smaller than the target, not larger.** The ~99 px estimate was the icon's box; the glyphs inside are
    smaller. At `ICON_SCALE` 0.9: play (circle) 67 px, playstation 67x51, list 67x53, apple-tv 83x41, power (0.63)
    49 px, Netflix 33x57, Prime 72x45, YouTube 65x45. So 80-96 px would need `ICON_SCALE` ~1.1-1.3 (play at 1.2 ≈
    89 px). Left at 0.9 for the hardware look (one constant; flagged).
  - **Bug found and fixed: "CH" labels were off the key or misplaced.** urem's `textTranslate` offsets are from the
    centre (node-buttonImage's default `textVAlign`), but ControlBlock puts a label under an icon at the bottom, so
    `refresh` + "CH" (y 13) drew "CH" below the key edge and the arrow keys' "CH" sat at the bottom. `hdd.mjs`'s `ch`
    helper now sets `valign: "center"` (test updated first); all three now match urem (CH under refresh, under the up
    arrow, over the down arrow). The Record key already had urem's explicit `bottom`.
- Step 15: `urem2/README.md` written (what it is, file map, tests, bake/upload per target, test server, ATV daemon,
  key map, a "What couldn't be ported" table per urem feature, what's not yet checked on the hardware).
  `docs/backlight.md` gained an "Open items" section with the idle dim / screen-off backlog line (Decision 4). The
  CLAUDE.md lines were already approved and added (below); nothing new proposed. Tests: 181.

### Proposed CLAUDE.md lines (Rule 4: for the user to approve; not edited by this job's agents)
- **Done** (user approved 2026-11-08; added to CLAUDE.md). Repository Layout, after `examples/`: `urem2/            Port of urem (the Pi + Stream Deck remote) as a standalone
  layout: tables/ (IR codes, Apple TV, Yamaha, HOT PC), layout/ (.mjs modules; entries urem2.mjs = real devices,
  urem2-test.mjs = test server), scripts/ (Lua), test-server/ (fake devices), atvdaemon/ (runs on the NAS); README.md
  lists what couldn't be ported`
- Tests (from sub-job 1): **done** (added to CLAUDE.md "CLI and binpack tests" on 2026-11-08, user approved).

## Flagged assumptions
- Activity names `atv`, `hdd`, `steam`, `playstation` with displayNames "Apple TV", "TV", "Steam", "PlayStation".
- ~~Faceless urem look kept~~: superseded by Decision 7 (border face, grey pressed fill).
- (Plan) Border `#a0a0a0`, pressed fill `#606060`, radius 14 (bake default), opaque black fill.
- (Plan) `ICON_SCALE = 0.9`, urem's per-key scales multiplied by it; offsets unchanged.
- (Plan) Encoder: one request per event, step × |delta| clamped at 10.
- (Plan) `IR_GAP_MS = 120` after each IR power packet in device lists (tune on the hardware).
- (Plan) `pana` device dropped (no ops); hdd's devices `[yama, lgtv]`.
- (Plan) PlayStation gets one page with the shared 7ch Stereo and mute keys (urem: no keys).
- (Plan) Long-press resync switches first, then repeats turnOn + didActivate (so it works from any activity).
- (Plan) Unused urem icons `yamaha.svg`, `lg.svg` not copied (urem had their keys commented out).
- (Plan) urem's CSS colour names (`orange`, `purple`, ...) kept as the colour strings.
- (Step 5) Encoder `param` with a `delta` but no `interval` counts as a slow turn (step 1 × |delta|) rather than
  sending nothing; only a missing/zero/garbage `delta` sends nothing.
- (Step 6) The look helpers only accept colours from `COLORS` (catches typos; add a colour there to use it).
- (Step 7) A small `layout/context.mjs` (`makeContext(target)` → `{ target, ops }`) was added for the `ctx` the plan's
  modules take (not in the module map).
- (Step 7) `volumeBindings` order: key7 (mute) before enc1 (volume), following the pad control order.
- (Step 9-12) `defineActivity` refuses `key7`/`enc1` in an activity's own pad spec (the volume bindings own them).
- (Step 9) Home's app keys (7-9) and DVR keys (12-14) tap only, no long; power (4) is `[switchTo(null)]` only.
- (Step 3) `luaLiteral` refuses non-integer numbers (no float constants are needed; avoids int/float surprises).
- (Step 14) `ICON_SCALE` stays 0.9 although glyphs measure ~50-83 px (under the 80-96 px target): the look is judged
  on the hardware (step 17); ~1.2 would reach the target.
- (Step 14) urem's label sizes not matched: urem's labels used node-buttonImage's default font, ControlBlock's
  defaults (`bold 15px` text-only, `bold 11px` under an icon) are kept; they read well in the renders.
- (Step 3) Harness `http.get` with a non-`http://` URL returns `nil, "http.get: url must start with http://"` (the
  device's exact message not checked).

## Plan
TDD: every code step writes or extends its tests first, then the code. Every step ends with `node --test
"urem2/**/*.test.mjs"` from the repo root, all passing (count noted in Notes). Paths below are under `urem2/`.

- [x] 1. **Fake Yamaha as a pure core.** Tests first in `test-server/test-server.test.mjs`: `createFakeYamaha()` →
  `{ state, handle(method, url) → { status, body, log } }` without sockets: `getStatus` body is compact JSON with the
  state, `setMute?enable=true` changes `state.mute` and returns `{"response_code":0}`, bad parameter → code 4,
  unknown path → 404. Then refactor `test-server.mjs`: export `createFakeYamaha`; the HTTP listener wraps it (log
  lines and existing HTTP tests unchanged).
- [x] 2. **Op builders.** Add `test/resolve.mjs` (`resolveLayout`, `cleanLayout`, see Notes). Tests first
  (`layout/ops.test.mjs`): for both targets, exact op objects: `ir("lg.powerOn")` = `udpPacket` to irtx with hex
  `040000004e4543204cb3df200000000000`; `irPower` adds the `IR_GAP_MS` delay; `atvKey("menu","hold")` → `"key menu
  hold"` to atvd; `atvApp("Netflix")`; `atvPing`/`atvDisconnect`; `yamaha(setInput("hdmi2"))` → `httpGet` on the
  target's base URL; `hotpc("launch-steam")` → `httpPost` body `{}` `application/json`; `wake()` → `sendWol` with
  `HOTPC_MAC`; `slideshow("nextDay")` → `udpPacket`; `slideshowToggle("date")` → `httpPost` JSON body. Unknown
  names throw. One layout using every builder is clean (`cleanLayout`). Then `layout/ops.mjs`.
- [x] 3. **Lua prelude and harness.** Tests first (`layout/lua.test.mjs`): `luaLiteral` for strings (quotes,
  backslash, newline, `%`), integers, booleans, arrays and string-keyed tables; each literal round-trips through
  wasmoon (`return <literal>` equals the JS value); `luaScript` prepends one `local` per const in a stable order and
  the result compiles; the harness runs a toy script that calls `http.get(url)` (gets `body, status`; `nil, err` when
  the fake returns nothing), `http.get{ url=, reply=false }` (→ `true`, recorded as fire-and-forget), `log`, reads
  `param`, and reports a Lua error. Then `layout/lua.mjs` and `test/lua-harness.mjs` (wasmoon via `createRequire` on
  `host/package.json`; multiple returns may need a small Lua wrapper over a JS function returning a table).
- [x] 4. **Yamaha toggle script.** Tests first (`layout/scripts.test.mjs`, harness + `createFakeYamaha`): mute toggles
  false → true → false; 7ch toggles `surr_decoder` → `7ch_stereo` → `surr_decoder`; a failed get (fake returns
  nothing, or status 500 with an HTML body) sets mute on / 7ch on, as urem; the set uses the fire-and-forget form;
  the sources compile and are under 16 KB; the two toggles share one `.lua` body (only the prelude differs). Then
  `scripts/yamaha-toggle.lua` and `yamahaToggle(target, which)` in `layout/scripts.mjs`.
- [x] 5. **Encoder volume script.** Tests first (same file): `delta=1 interval=30000` → one `setVolume up step 1`;
  `delta=-1 interval=5000` → down 5; `interval=10000` → 3; `delta=2 interval=5000` → 10; `delta=3 interval=5000` →
  10 (clamp); `delta=0`, empty and garbage `param` → no request, no error; the fake Yamaha's volume moves by the
  step; fire-and-forget only. Then `scripts/volume.lua` and `volumeScript(target)` (constants exported for the test).
- [x] 6. **Look.** Tests first (`layout/look.test.mjs`, through `resolveLayout`): `icon("#play", { scale: 0.7 })`
  gives `iconScale` 0.63 (`ICON_SCALE` 0.9 × 0.7) and 0.9 with no scale; a key under `ROOT_DEFAULTS` has a
  `normalBg` whose SVG has the border stroke and a black fill, and a `pressedBg` filled `PRESSED` with the border and
  no `pressed-overlay`; a label-only key gets the same face; a `null` slot has no images. Then `layout/look.mjs`
  (`ROOT_DEFAULTS`, `COLORS`, `ICON_SCALE`, `icon()`, `label()` helpers matching urem's props: colour, scale, offset,
  font, `labelVAlign`, `labelOffset`).
- [x] 7. **Keys and pad helpers.** Tests first (`layout/keys.test.mjs`, `layout/pad.test.mjs`): `key(look, ops, {
  long, repeat })` → button with `tap` (and `long`); `repeat: 100` and `{ initialDelay: 500, period: 100 }` accepted
  and absent from the output; `padBindings({ key1: { tap, long }, key2: { press, repeat } })` → bindings in key order
  with `control`/`event`/`do`; an unknown control (`key8`, `macro1`) or event throws; `volumeBindings(ctx)` = `enc1`
  `turn` → volume script and `key7` `tap` → mute toggle; the shared `sevenChKey`/`muteKey`; a layout using them is
  clean. Then `layout/keys.mjs` and `layout/pad.mjs` (`withRepeat` is the one place that drops `repeat`).
- [x] 8. **Devices and activity meta.** Tests first (`layout/devices.test.mjs`): devices `yama` (setPower on /
  standby), `lgtv` (`irPower` lg.powerOn / lg.powerOff), `atv` (`irPower` atv.powerOn + ping / atv.powerOff +
  disconnect), `hotpc` (wake, no turnOff), in that order; activity meta (`atv` [yama, lgtv, atv] hdmi2 "Apple TV",
  `hdd` [yama, lgtv] hdmi1 "TV", `steam` [yama, lgtv, hotpc] hdmi3 "Steam", `playstation` [yama, lgtv] hdmi4
  "PlayStation"); `hooks(a)` = `didActivate` setInput; `resync("atv")` = the three turnOn lists then setInput, built
  from the same objects; `activityKeyGestures(a)` = `{ tap: switchTo(a), long: [switchTo(a), ...resync(a)] }`; every
  list ≤ 64 ops; a layout with the devices and four minimal activities is clean. Then `layout/devices.mjs`.
- [x] 9. **Home.** Copy `../urem/frontend/icons/{netflix,prime,youtube}.svg` to `icons/` verbatim (`cmp`). Tests first
  (`layout/home.test.mjs`): all 15 slots against the urem map (Notes, Home): empty 0, 3, 5; each key's icon/label,
  colours and scales (via the look helpers), and ops built with the same builders (e.g. 7 = `[switchTo("atv"),
  atvApp("Netflix")]`, 12 = `[switchTo("hdd"), ir("pana.videoMenu")]`, 4 = `switchActivity null`, 10 = 7ch toggle);
  activity keys 1, 2, 6, 11 have the resync `long`. Then `layout/home.mjs` (`home(ctx)`).
- [x] 10. **Activity atv.** Tests first (`layout/activities/atv.test.mjs`): the page's 15 slots per the urem map
  (empty 0, 3, 4, 5, 8, 9; rewind/forward carry `repeat: 100`); pad bindings key1 tap `menu` + long `menu hold`, key3
  tap `select` + long `select hold`, key2/4/5/6 press up/left/down/right with `repeat {500,100}`, then the volume
  bindings; devices/hooks/displayName from the meta. Then `layout/activities/atv.mjs`.
- [x] 11. **Activity hdd.** Tests first (`layout/activities/hdd.test.mjs`): the three pages, every slot per the urem
  map (number keys with font `bold 24px sans-serif` and offset y -3; the CH/Record icon+label keys with their scales
  and offsets; the coloured squares), each key's `ir("pana.<name>")`; pad press bindings return/up/ok/left/down/right
  (arrows with `repeat`) + volume. Then `layout/activities/hdd.mjs` (a number-key helper, not 10 literal keys).
- [x] 12. **Activities steam and playstation.** Tests first (`layout/activities/steam.test.mjs`,
  `playstation.test.mjs`): steam page 1 (HOT PC posts, slideshow UDP and the `all` toggle, rewind/forward `repeat`),
  page 2 (six display-element toggles), pad key2/4/5/6 press voteUp/prev/voteDown/next, key1/key3 unbound, + volume;
  playstation: one page with 7ch Stereo (10) and mute (14), only the volume bindings. Then the two modules.
- [x] 13. **buildLayout and the entry files.** Tests first (`layout/build.test.mjs`): `buildLayout(targets.test)` and
  `(targets.real)` are clean (0 errors, 0 warnings, real `createNodeHost`); activities in order atv, hdd, steam,
  playstation; devices in order; every `udpPacket`/`httpGet`/`httpPost`/`sendWol` host in the resolved tree belongs to
  the chosen target (none of the other target's addresses); exactly 3 distinct Lua sources per target (mute toggle,
  7ch toggle, volume); `loadLayoutFile("urem2/urem2.mjs")` / `urem2-test.mjs` deep-equal `buildLayout(targets.real)`
  / `(targets.test)`. Then `layout/build.mjs`, `urem2.mjs`, `urem2-test.mjs` (header comments: what each targets,
  the bake/upload command).
- [x] 14. **Full bake.** Test first (`layout/bake.test.mjs`): `bakeFile("urem2/urem2-test.mjs")` gives a blob, no
  warnings; log the blob size and distinct image count to the test output. Then run by hand `node cli/cli.js bake
  urem2/urem2-test.mjs -o scratch/urem2-test.bin` and `... urem2/urem2.mjs -o scratch/urem2.bin` (both succeed; sizes
  in Notes). Look at a few rendered keys if a quick way exists (e.g. `renderImages` + sharp to PNG in `scratch/`):
  border, pressed fill, icon size vs the 80-96 px target; record what was seen.
- [x] 15. **README and docs.** `urem2/README.md`: what urem2 is; the file map; tests; the test server (from the
  umbrella's "Test server: how to run"); baking/uploading each target (`node cli/cli.js upload urem2/urem2-test.mjs`;
  `upload urem2/urem2.mjs` for the house); the ATV daemon on the NAS (`atvdaemon/`, Decision 3, untested there);
  the key map in brief; **what can't be ported**, per urem feature (from the gap list and Decisions 1-10:
  auto-repeat, display off/idle dim, reselect re-sync (long press instead; possible `resync` option), global pad
  bindings (repeated per activity; possible root bindings), pad key7 inert until the pin map has it, press shrink
  (grey fill instead), no reply reading from irtx/ATV daemon (daemon now waits instead), clock/Moonraker/nav not
  ported by request, PlayStation keys added, `yama` name resolution to check). Add an "Open items" section to
  `docs/backlight.md` with the backlog line (Decision 4: standalone idle dim and screen off with no activity; urem:
  off with no activity, dim after 8 s idle, first touch only wakes; pick up once the backlight can switch). Put the
  CLAUDE.md lines (Notes, "Proposed CLAUDE.md lines") to the user; don't edit CLAUDE.md.
- [x] 16. **Wrap up.** (2026-11-08: urem2 181/181, examples 17/17, host 212/212 pass.) Every listed file present, `node --test "urem2/**/*.test.mjs"` and `node --test
  "examples/**/*.test.mjs"` pass, `cd host; npm test` passes (untouched, sanity). Record counts. Rename this doc to
  `complete-urem2-layout.md` once the user has done or waived step 17; then the umbrella can complete.
- [x] 17. **Hardware check (the user; not automatable).** (2026-11-08: against the test server, working perfectly
  per the user, after moving the test target's slideshow UDP to 4501: Windows dropped inbound UDP 4500 from the
  device, see Notes. **Not yet done:** the real target at home, and (h) the pad, which doesn't exist yet.) Stop `examples/test-server`, run `node
  urem2/test-server/test-server.mjs` on the `SERVER` PC, `node cli/cli.js upload urem2/urem2-test.mjs`. Look at:
  (a) Home: border faces, grey pressed fill, icon sizes (tune `ICON_SCALE`), labels ("Sub- titles" wraps, "7ch
  Stereo"); (b) each activity key: the server logs `Yamaha: setPower on`, `IR lg.powerOn`, ... in the device order,
  then `Yamaha: setInput hdmiN`; switching away turns off only the unused devices; no IR line missing (if one is,
  raise `IR_GAP_MS`); (c) Netflix/Prime/YouTube: the `ATV: app ...` line comes after the switch's lines; list/guide/
  function: the `IR pana.*` line after them; (d) long press on an activity key: the turnOn lines and setInput repeat;
  (e) every activity key sends the logged command (spot check against the map); (f) 7ch and mute keys: `getStatus`
  then the opposite set, toggling each press (first hardware run of scripts from a layout: also the Lua checks in
  `wip/wip-lua-hardware-checks.md` checklist B); (g) power key: everything off, Home; (h) pad/encoder once the pad
  exists: volume steps by speed, key7 mute. Then, at home, the real target (`upload urem2/urem2.mjs`): `yama`
  resolves (else `yama.lan` in `targets.real`), the ATV daemon on the NAS answers (Decision 3 checks in
  `wip/complete-urem2-test-server.md` step 12). Record results in Notes.
