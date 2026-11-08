# urem2: port urem to ControlBlock (umbrella)

Status: Complete (2026-11-08: all three sub-jobs complete; verified on the hardware against the test server.
Real devices and the pad not yet tried.)

## Description
Port urem (`../urem/`, the user's Pi + Stream Deck + macro pad universal remote, Node.js) to a ControlBlock
standalone layout, as a proof-of-concept/example sub-project in `./urem2/`. Move over as much as possible and
produce a clear list of what can't be moved yet (missing ControlBlock features).

Requirements from the user (2026-11-08):
- Layout written as a set of `.mjs` files: well structured, DRY, TDD where possible.
- Same external pieces as urem: the remote **irtx** IR blaster for older appliances, and a **separate Apple TV
  daemon** process (urem's `atvdaemon/`).
- Key icons are mostly node-buttonImage built-ins (`svgFile: "#name"`); they may need different scaling on this
  device (128x128 keys, icon ~80-96 px).
- Not ported: urem's page-navigation buttons (swiping replaces them; leave those slots empty), the 3D printer
  (Moonraker) button, the clock button.
- urem's tables (IR codes etc.) come over as **complete tables**; button definitions refer to entries, they don't
  hard-code values.
- A **test server** that mimics the controlled devices, and a **bake-time switch** in the layout to target the test
  server or the real devices.
- Assume the macro pad is complete and working (it is specced, not built: `wip/wip-macro-pad.md`,
  `docs/layout-format.md` Bindings) and bind it.
- urem uses the `layers` library (`@toptensoftware/layers-core`, `layers-streamdeck`; source in
  `../urem/node_modules/@toptensoftware/`); docs are sparse, learn it from urem's use and its source.
- urem's structure is messy: don't mirror it.

## Sub-jobs
Investigation is shared and recorded here. At the Plan phase the work is split into these docs, done in order:

1. **Complete (2026-11-08):** `wip/complete-urem2-test-server.md`: **the shared tables and wire encoders first** (IR code table, Apple TV keys and
   apps, Yamaha commands, targets/addresses, the irtx packet encoder: the server decodes with the same tables the
   layout encodes with, so they belong to the first sub-job), then the fake devices (irtx, Apple TV daemon, Yamaha,
   HOT PC, slideshow, WoL) as a Node test server with tests. Also copies `atvdaemon/` over.
   Also the daemon's connect-retry change (decision 3).
2. **Complete (2026-11-08):** `wip/complete-switch-activity-order.md` (a ControlBlock firmware job, not urem2-specific): `switchActivity` runs the
   switch's lists before the rest of the current list (decision 2). Must be done before sub-job 3's switch-then-send
   keys; independent of sub-job 1.
3. **Complete (2026-11-08; verified against the test server on the hardware, real devices not yet tried).**
   `wip/complete-urem2-layout.md`: the layout itself (device op builders, Lua scripts, Home, activities, pad bindings,
   target switch), its tests, the "what's missing" report (`urem2/README.md`), the `docs/backlight.md` backlog line
   (decision 4) and the CLAUDE.md Repository Layout line for `urem2/` (approved).

(Adjusted after Investigate: tables moved into sub-job 1; the switch-order fix added as sub-job 2 by the user's
decision. The other ControlBlock features found missing (gap list below) are **not** part of this job; the README
lists them and each could become its own job.)

## Notes

### First look (2026-11-08, before Investigate)
- urem live code: `frontend/` (entry `index.js`, `homePage.js`, `core.js` activity switching, `activity-*.js`,
  `device/*.js`, `irtx.js`, `http.js`, `wol.js`, `volume.js`, `gpio.js` = the old macro pad on Pi GPIO),
  `atvdaemon/` (pyatv UDP text daemon, port 7777: `key <name>`, `app <bundle>`, `ping`, `disconnect`),
  `ircodes/ircodes.json` and `irtx/ircodes.json` (which is current?), `irtx/activities.js` + `activities.bin`
  (activities pushed to the blaster itself: understand what these are for). `obsolete/` and `pico/`: ignore.
  Not ported: `clock-button.js`, `moonraker*.js`, `streamdeck-pager.js`.
- Devices: Yamaha receiver (HTTP GET `http://yama/YamahaExtendedControl/v1/main/...`), LG TV and Panasonic DVR (IR
  via irtx), Apple TV (IR power + atvdaemon), HOT PC (WoL `04:d4:c4:4e:da:2b` + HTTP POST JSON to
  `10.1.1.105:4500`). irtx at `10.1.1.188` over UDP (binary protocol: `@toptensoftware/irtx-node`, also `../irtx`).
- urem's activity model maps closely onto ControlBlock's: per-activity `devices` (turn on new, off old), an
  `onActivate` hook (e.g. Yamaha input `hdmi2`), Home keys that switch activity then send something (Netflix etc.).
- Likely gaps / things to check in Investigate:
  - **Auto-repeat** while held (urem `repeat: 100`, `arrowRepeat` 500/100 ms on the pad arrows): does ControlBlock
    have it? Lua loop until release?
  - **Read-modify-write toggles** (Yamaha mute, 7ch stereo: `getStatus` then set): a Lua script with `http.get`;
    no JSON library in the sandbox.
  - **Encoder velocity steps** (volume step 1/3/5 by `velocityInterval`): the encoder's `param` to a script.
  - **Display off** when everything is powered off (`enableDisplay(false)`): the backlight is locked on.
  - **irtx packets**: encode at bake time into `udpPacket` hex (a table-driven helper).
  - Custom SVGs: `frontend/icons/{netflix,prime,youtube,lg,yamaha}.svg`.
  - Responses from the ATV daemon / irtx are ignored by `udpPacket` (one way).
- ControlBlock side: layout format `docs/layout-format.md`, examples `examples/activity-test.mjs`,
  `examples/test-common.mjs`, existing test server `examples/test-server/test-server.mjs` (see
  `wip/wip-node-test-server.md`, still in Implement), bake via `node cli/cli.js bake|upload`.

### User answers (2026-11-08)
- `urem2/` goes at the repo root (confirmed). Add it to CLAUDE.md's Repository Layout once it exists (approved).
- The two `ircodes.json` files may be identical: check which one urem actually uses.

### Test server: how to run (from sub-job 1, for sub-job 3's `urem2/README.md`)
- `node urem2/test-server/test-server.mjs` on the PC whose IP is `SERVER` in `examples/test-common.mjs`; bake the test
  entry of the layout. Allow node.exe through the Windows firewall. Not alongside `examples/test-server` (UDP :9):
  stop that one first, or the start fails with `EADDRINUSE 0.0.0.0:9`. Prints the bound ports (irtx UDP 4210, WoL
  UDP 9, ATV daemon UDP 7777, Yamaha HTTP 8080, HOT PC + slideshow HTTP 4500, slideshow UDP 4500). On this PC
  `127.0.0.1:8080` is WSL's Apache (`wslrelay`): poke the fake Yamaha at the LAN address.
- Tests: `node --test "urem2/**/*.test.mjs"` from the repo root (a directory argument doesn't work on Node 24).
- The ATV daemon (`urem2/atvdaemon/`, runs on the NAS) now retries the connection for up to ~20 s inside `key`/`app`/
  `text_*` (Decision 3); **not yet tested on the NAS** (see `wip/complete-urem2-test-server.md`, step 12).

### Investigate findings (2026-11-08)

Sub-job specific detail: device protocols and test-server design in `wip/complete-urem2-test-server.md` Notes; the
per-key feature map, icons and layout design in `wip/complete-urem2-layout.md` Notes. Shared findings here.

#### 1. IR codes: which table
- urem's frontend uses **`ircodes/ircodes.json`** (`frontend/irtx.js`: `import ircodesJson from
  '../ircodes/ircodes.json'`, `irtx.codes = ircodesJson`). `ircodes/send.js` (a CLI) uses it too.
- `irtx/ircodes.json` is only imported by `irtx/activities.js` (the blaster's own config, below). It is an older
  subset: identical except `lg` has only `powerOn/powerOff/powerToggle` (ircodes/ has 33 LG codes). **Use
  `ircodes/ircodes.json`.**
- Shape: `{ atv: {...}, lg: {...}, pana: {...}, yama: {...} }`, each `name: "PROTO:0xHEX"` strings. 116 `NEC:` codes
  (atv 28, lg 33, yama 55) and 53 `PANA:` codes (all `pana`, incl. `tv*` codes for a Panasonic TV). No raw timing
  arrays. (Plan correction: counted by key, lg has 35 and yama 53; the NEC total of 116 stands.) Duplicates exist on purpose (`atv.forward` = `atv.right`, `atv.menu` = `atv.home`). urem uses: `lg.powerOn/
  powerOff`, `atv.powerOn/powerOff`, and `pana.*` (btn0-9, transport, colours, guide, menus, arrows, ok, return,
  record, channel, sttl, driveSelect, lastChannel, delete, option, info). `yama.*` IR is unused (Yamaha goes by HTTP).

#### 2. irtx wire protocol (authority: `../irtx/readme.md` "UDP Protocol" + `firmware/wifi_udp.cpp`, `ir_tx.cpp`)
- UDP **port 4210**, all fields little endian, **no reply** for any command (one way: matches `udpPacket`).
- **cmd 4 "Transmit IR Code"** (what urem sends; irtx-node's `irSend("NEC:0x..")`), 17 bytes:
  `u16 cmd=4 | u16 reserved=0 | u32 protocolId (RIFF FourCC) | u64 code | u8 repeat`.
  `NEC` = bytes `4E 45 43 20` ("NEC " space-padded), `PANA` = `50 41 4E 41`. Only these two protocols exist.
  `repeat`=1 sends the NEC repeat frame (Panasonic has none); urem always sends 0.
  Example: `NEC:0x20dfb34c` (LG powerOn) = `04 00 00 00 4E 45 43 20 4C B3 DF 20 00 00 00 00 00`.
- cmd 1 = raw timings (`u16 cmd=1 | u16 0 | u32 carrier=38000 | u32 gap | u16 timings[]`), not needed (no raw codes).
  cmds 2/3 BLE, 5 switch the blaster's own activity, 6 simulate IR receive: unused by urem.
- **Fully precomputable at bake time** into `{ op: "udpPacket", host, port: 4210, data: { hex } }`: a pure
  `irPacket("NEC:0x...")` helper (BigInt for the 64-bit code; hex string out). Test vectors hand-computed per the
  readme (irtx-node is not in this repo; don't depend on it).
- **Blaster drops packets** (logged only): it holds one transmission in flight plus **one pending**; a third
  packet arriving before the first finishes (NEC frame ~68 ms + gap) is dropped. Also every cmd 1/4 packet is
  dropped while the blaster's own activities system is busy (`isActivitiesBusy()`). So back-to-back IR ops (device
  on/off lists, switch-then-send keys) may need short `delay` ops between sends; check on the hardware.
- `irtx/activities.js` / `activities.bin` / `push-activities`: a config for the **blaster's own standalone activity
  system** (binpack blob uploaded by `curl POST http://10.1.1.101/activities`), driven by the Panasonic physical
  remote's IR (Power+1/2/0 switch activity, volume via Yamaha HTTP, IR passthrough/translation to the ATV). Addresses
  in it point at a test server (`10.1.1.125:3000`). **Not part of urem's frontend and not ported** (it's a separate,
  parallel remote; ControlBlock's own activity design was adapted from it). Only relevance: if it is loaded on the
  blaster, its busy state drops urem2's IR packets.

#### 4. ControlBlock capabilities (checked against docs/layout-format.md, docs/standalone.md, host/src/bake/)
- Ops: `delay`, `switchActivity` (null = none: all devices off, Home), `hidKey`, `httpGet` (reply discarded),
  `httpPost` (`body` string, `contentType`, `headers`), `udpPacket` (`host` IP or name, `port`, `data` string or
  `{hex}`), `sendWol` (`mac`, `broadcast`?), `script` (`lua` source or `file`, 16 KB max).
- Gestures (touch keys): `press`, `release` (also on swipe cancel), `tap` (= `action`; fires on press when no `long`
  is bound, else on release if short), `long` (at `longPressMs`, default 500, while held). `release` **still fires
  after `long`** (urem suppresses it). **No auto-repeat** (confirmed: `GestureBank` has press/release/tap/long only;
  the macro pad job put button repeat out of scope). No double press.
- Activities: `devices` (ordered), hooks `willActivate`/`didActivate`/`willDeactivate`/`didDeactivate`, 1..7
  pages (an activity needs at least one page), `bindings` (pad/encoder), `displayName` (status bar). Switch order:
  old.willDeactivate, new.willActivate, turnOn (new's order, only devices not on), turnOff (old's order, devices the
  new doesn't use), new.didActivate, old.didDeactivate. **Selecting the current activity only glides** (no ops; urem
  re-ran every turnOn and onActivate, which re-synced drifted equipment). Limits: 32 devices, 32 activities, 7
  pages/activity, 256 bindings/list, 64 ops/list, op queue 40 list refs.
- **Op ordering after a switch (important):** `switchActivity` pushes the switch's hook/device lists at the **tail**
  of the queue, while the rest of the current list keeps running first. So `tap: [switchActivity atv, udp "app
  com.netflix.Netflix"]` sends the app launch **before** the devices are turned on. Same inside a script (its
  `switchActivity()` queues at the tail; a script `delay` holds the whole queue). See gap list.
- Pad/encoder (from `wip/wip-macro-pad.md` decisions; the layout-format doc's `macro1`/`volEnc`/`turnLeft` names are
  superseded): controls `key1`..`key32` (pad key n, 1-based; an encoder's push button is an ordinary key), events
  `press`/`release`/`tap`/`long` (synthesised like touch keys); encoder `enc1`, event **`turn`**, param
  **`delta=<signed detents> interval=<µs>`** where interval is urem's `velocityInterval` (average of last 4
  inter-edge intervals) in microseconds, saturated at 65535. urem's thresholds 8 ms/20 ms = `interval < 8000` /
  `< 20000`. One event may carry |delta| > 1 (5 ms coalescing). Pad bindings are **per activity only**: with no
  activity selected the pad and encoder do nothing; while Home is in view the selected activity's bindings work.
  Pad default pin map (`pad/main/pad_pins.h`) has **6 keys and no encoder push key**; urem has 7 buttons (index 6 =
  encoder push = mute). Pins on the C3 are all used (see flagged assumptions).
- Lua: `param` global; `http.get(url)` waits and returns `body, status` (10 s timeout, 16 KB); `http.post`,
  `udp.send(host, port, data)`, `wol.send`, `delay`, `switchActivity`, `log`, `millis`; `string`/`table`/`math`.
  **No JSON library** (string-scan with `string.find`/`match`). Scripts' globals are per script and **not shared**
  between scripts (so no shared state, e.g. a "held" flag set by a release script).
- Bake: `.mjs` layouts are imported (default export) by `host/src/bake/files.js`; `svg`/`icon`/`file` paths resolve
  relative to the layout file's directory; `#name` = node-buttonImage built-in (checked against its `images/`).
  **No way to pass arguments from the CLI to the layout module** (bake-time switch = separate entry files or an env
  var). Look props: `icon`, `iconColor` (svgForeColor), `iconScale` (svgScale), `iconOffset` (svgTranslate),
  `label`, `labelColor`, `font`, `labelOffset`, `labelVAlign`, `face`/`color`, `pressed`, `image` escape hatch.
  Sizes are node-buttonImage **virtual pixels (72)** scaled to 128, so urem's `svgScale`/`svgTranslate`/font px
  (authored for 72 px Stream Deck keys) carry over proportionally. Defaults chain: root → home/activity → page → key.
  With no `face`/`color` there is **no background, and then the default pressed look (a 35% overlay on the face) shows
  nothing**: a faceless urem-style key needs an explicit `pressed` look.
- Exports usable by urem2 (relative import `../host/index.js`): `bakeFile`, `loadLayoutFile`, `resolve`,
  `createNodeHost`, `BakeError`, `formatEntries`, consts. Host bake tests use `node --test` with a fake host
  (`host/test/bake/helpers.js`: `fakeHost`, `ok`, `run`); real-render test `render-real.test.js`.

#### 4b. Grid mapping
- urem's Stream Deck is the **15-key 5x3** model (72x72 px keys; `buttonIndex: [col,row]`, col 0..4, row 0..2,
  mapped by `CONTROLS.find(column,row)`). ControlBlock is 5x3 row-major: **index = row*5 + col**. Same grid, so every
  key keeps its position. Column 0 was urem's nav column (Home toggle `[0,0]` on every layer, pager arrows
  `[0,1]`/`[0,2]` on paged activities, clock `[0,1]` on Home/ATV): those slots stay empty, except `[0,2]` "7ch Stereo"
  on Home and ATV, which is a real action and is ported.

#### 5. Reuse (DRY)
- `examples/test-server/test-server.mjs` exports `startTestServer`, `timestamp`, `repr`; `wolNote` (WoL magic decode)
  is private (export it to reuse). Its routes (`/get`, `/hang`, `/mute`, port 8000/9999) are for the hardware test
  kit, not urem's devices, and it binds UDP :9 (so it can't run alongside a urem2 server that also listens on :9).
  urem2's server should be its own module that imports `timestamp`/`repr`/`wolNote` from there, following its
  testable shape (`start...({ports 0, log})` returning `close()`; `node --test`).
- `examples/test-common.mjs`: `flatten` (flat faces) is reusable if urem2 uses faces; `SERVER`/`udp`/`get` are
  test-kit specific.
- `examples/activity-test.mjs` shows the `.mjs` layout idioms (helper functions, shared rows, `page()` builders).
- Tests: urem2 tests with `node --test urem2/` (no package.json; the host library's `node_modules` gives sharp,
  wasmoon). Layout tests: stage 1 via `resolve(layout, await createNodeHost(dir))` (fast, real icons/Lua check) and
  one full `bakeFile` (renders). Lua scripts could be unit tested by running them in wasmoon with fake `http`/`udp`
  (wasmoon is only reachable through `host/`; would need a small export or a relative path into
  `host/node_modules`: a Plan decision).

#### 8. Draft gap list (feature → why missing → workaround)
1. **Auto-repeat while held** (urem `repeat: 100` on ATV/Steam rewind/forward touch keys; `{500,100}` on the pad
   arrows in ATV and HDD) → no repeat gesture in `GestureBank` (pad job: out of scope) and scripts can't see the
   release (no shared state; a looping script holds the queue) → port as single-shot; a `repeat` gesture event
   (fires every period after an initial delay while held, touch and pad) would be the feature.
2. **Run ops after an activity switch from the same key** (Home: Netflix/Prime/YouTube = switch to ATV then `app`;
   list/guide/function = switch to HDD then IR) → `switchActivity` queues its lists at the queue tail, after the rest
   of the current list → workaround: put the follow-up on the key's `release` gesture (pushed at lift, after the
   switch's lists were queued); or a feature: run the switch's lists before the rest of the current list (insert at
   head), or a `then` list.
3. **Wait until the Apple TV daemon is connected** (urem `atv.connect()` polls `ping` up to 20 s before `app`) →
   `udpPacket` and `udp.send` can't read replies → `delay` before `app` (fixed guess), or extend the copied daemon
   (e.g. make `app`/`key` wait for the connection, or add an HTTP endpoint that `http.get` can poll).
4. **Display off / dim** (urem: off when no activity, dim after 8 s idle, first touch only wakes) → backlight locked
   on (PSRAM glitch) and no standalone idle policy → none; note only.
5. **Pad/encoder with no activity selected** (urem's volume layer — encoder + mute button — was global, active on
   Home too) → ControlBlock binds the pad per activity only → repeat the volume bindings in every activity (shared JS
   array); with no activity the receiver is off anyway.
6. **Read-modify-write toggles** (Yamaha mute, 7ch stereo) → not an op → Lua script: `http.get` getStatus, string
   scan, `http.get` set. Works (not a gap, but scripts are not yet run on the hardware).
7. **Encoder velocity steps** → Lua script parsing `param` (works; script not yet hardware tested).
8. **Re-selecting the current activity re-syncs devices** (urem re-sent all turnOn + onActivate) → ControlBlock only
   glides → user selects Off then the activity again; or a feature ("reselect re-runs turnOn/didActivate").
9. **Release suppressed after long press** (urem) → ControlBlock fires `release` after `long` → not an issue: urem2
   binds `tap` + `long` (tap is not fired after long).
10. **Press feedback** (urem shrank the image) → default pressed look needs a face → give keys a `pressed` look.
11. **PlayStation activity has no keys** → ControlBlock needs ≥1 page → one empty page (or a page with the shared
    keys like 7ch Stereo / mute).
12. Not ported by request: clock, Moonraker, pager/nav buttons. HID: urem has none.

## Flagged assumptions
- `urem2/` imports the host library by relative path like `cli/` and `examples/` (no `package.json`
  dependencies) unless Investigate finds a reason not to. (Investigate: no reason found.)
- **Pad key numbering:** urem GPIO button index n → `key{n+1}` (key1..key6 = the six pad keys in urem's order:
  0 back/menu, 1 up, 2 select/ok, 3 left, 4 down, 5 right), and the encoder push (urem index 6, mute) → **`key7`**.
  The pad's default pin map has only 6 keys and the C3's safe pins are used up, so key7 needs a pin-map change (or
  the mute binding is inert until then).
- **Bake-time switch:** two thin entry files, e.g. `urem2/urem2.mjs` (real devices) and `urem2/urem2-test.mjs`
  (test server), each `export default buildLayout(targets.real|test)`; no env var (the CLI can't pass options, and an
  env var is invisible in the command). Plan to confirm.
- **Lua scripts get their addresses at bake time:** `.lua` files kept as files, read by the `.mjs` and emitted as
  `lua:` source with a generated prelude (`local YAMAHA = "http://..."`), since scripts can't read the target.
- Real addresses from urem: irtx `10.1.1.188`, Yamaha hostname `yama` port 80 (no IP in the live code; the irtx config
  had `10.1.1.103` commented out), HOT PC `10.1.1.105` (WoL `04:d4:c4:4e:da:2b`, HTTP :4500), slideshow commands to
  `10.1.1.2:4500` (UDP and HTTP), ATV daemon `127.0.0.1:7777` (it ran on the Pi). Used as given until the user says
  otherwise; see questions.

## Decisions (user, 2026-11-08, walking the gap list)
1. **Auto-repeat (gap 1):** port as single shot now; write urem2 so adding repeat later is a one-line change per
   key/binding (e.g. a `repeat` setting on the key definitions that the builders ignore for now). A ControlBlock
   `repeat` gesture is a later, separate job. List it in the README.
2. **Switch-then-send (gap 2):** fix ControlBlock **first**, as its own job before urem2's layout: `switchActivity`
   runs the switch's lists (hooks, device on/off) straight after itself, before the rest of the current list (in
   ops and in Lua `switchActivity()`). urem2 then writes these keys as one plain list
   `[switchActivity, follow-up]`. No release workaround.
3. **ATV daemon connect wait (gap 3):** change urem2's copy of the daemon (`urem2/atvdaemon/atvdaemon.py`): `app`
   and `key` retry the connect (scan + connect) for up to ~20 s before replying `error:` (urem's `connect()` poll,
   moved into the daemon). No `delay` in the layout. Python change, not automated: tested by hand (warn in the plan).
   The fake daemon in the test server can mimic it (connect delay) but it's not a test of the real one.
4. **Display off / idle dim (gap 4):** README lists it as missing (blocked on the backlight hardware), and add a
   backlog line to `docs/backlight.md`'s open items: standalone idle dim and screen off with no activity (urem: off
   with no activity, dim after 8 s idle, first touch only wakes), to pick up once the backlight can switch.
5. **Pad/encoder with no activity (gap 5):** one shared volume bindings array (encoder + mute) spread into every
   activity's `bindings`. Not a urem gap in practice (no activity = receiver off). README mentions root-level
   global bindings only as a possible ControlBlock feature.
6. **Reselect re-sync (gap 6):** urem2 only, no firmware change: **long press** on an activity key runs that
   activity's device power-on lists plus its setup (`didActivate`) list, built from the same device/activity modules
   (no duplicated ops in the source). Tap stays a plain `switchActivity`. README notes the difference from urem
   (urem re-synced on a plain tap) and a possible `resync` option on `switchActivity` as a feature.
7. **Look / press feedback (gap 10, question 4):** every key (that has content) gets a **silver/grey rounded-square
   border** with a black or transparent fill (background is black anyway; pick whichever bakes smaller/cleaner).
   **Pressed:** the rounded square **filled mid grey**. Icons and labels as in urem on top. Set once in the root
   defaults (`face` object: `border`, `radius`, `top`/`bottom` fill; check the bake supports a border-only face and
   a `pressed` face override; if not, use the `image` escape hatch in one shared helper). Empty slots stay empty.
8. **ATV daemon host (question 1a):** the user's home NAS, `cool.lan` = `10.1.1.2`, UDP 7777. Real target uses the
   IP `10.1.1.2` (no DNS dependency), with `cool.lan` in a comment. Same host as urem's slideshow (`10.1.1.2:4500`).
9. **Yamaha and slideshow (question 1b/c):** as in urem: Yamaha `http://yama:80/YamahaExtendedControl/v1`,
   slideshow `10.1.1.2:4500` (UDP + HTTP), HOT PC `10.1.1.105:4500`. Flag for the hardware check: lwIP may not append
   the DHCP domain to a bare name, so `yama` might need to be `yama.lan` (one value in the address table).
10. **Encoder push / pad key 7 (question 2):** bind `key7` tap = Yamaha mute in the shared volume bindings anyway;
    inert until the pad sends `key7` (pin map is a macro pad job decision). README notes it.

All questions below are answered by the decisions above.

## Questions for the user (answered, see Decisions)
1. **Addresses:** where will the Apple TV daemon run now (urem used `127.0.0.1` on the Pi)? Does the device resolve
   `yama`, or what is the receiver's IP? Is the slideshow host really `10.1.1.2` (Steam activity) while the HOT PC is
   `10.1.1.105`?
2. **Pad key 7** (encoder push = mute): add a 7th pad key (pin map change), or leave mute on the touch keys only?
3. **Auto-repeat and switch-then-send** (gaps 1, 2): accept the workarounds (single shot; follow-up on `release`)
   for the port, and list the features in the README? Or do a ControlBlock feature job first?
4. **Look:** urem drew icons on black with no face. Keep that (with an explicit pressed look), or use ControlBlock's
   rounded faces?

## Plan
(Written per sub-job at the Plan phase.)
