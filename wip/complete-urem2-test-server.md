# urem2: test server for the controlled devices

Status: Complete (shared investigation in `wip/wip-urem2.md`)

## Description
A Node test server in `urem2/` that stands in for the devices urem2's layout controls: the irtx IR blaster
(UDP, binary), the Apple TV daemon (UDP text, port 7777), the Yamaha receiver (HTTP, YamahaExtendedControl incl.
`getStatus` state for the toggles), the HOT PC (HTTP POST JSON on :4500) and Wake-on-LAN. It logs what each device
received in device terms (e.g. "LG: powerOff", "Yamaha: setInput hdmi2") and keeps enough state for read-modify-write
toggles. With tests. The layout's bake-time switch points at it.

Part of the umbrella job `wip/wip-urem2.md`; read that first.

## Notes
Shared findings (IR table choice, irtx wire format, reuse of `examples/test-server`, gap list) are in
`wip/wip-urem2.md` Investigate findings; device protocols here.

### Shared tables and encoders (first part of this sub-job; the layout imports them too)
- IR codes: copy urem's `ircodes/ircodes.json` complete (atv/lg/pana/yama) into urem2 (e.g. `urem2/tables/`).
- `irPacket("NEC:0x..."|"PANA:0x...")` → 17-byte hex (format in the umbrella §2); the server's decoder does the
  reverse and **looks the code up in the same table** to log device terms (`lg.powerOn`, `pana.guide`). Unknown
  protocol/length → logged as such. Note the table has duplicate codes (`atv.menu` = `atv.home`, `atv.right` =
  `atv.forward` = `forward10x`): log all names or the first.
- Apple TV key names (daemon README / pyatv): `up down left right select menu home home_hold play_pause play pause
  next previous stop volume_up volume_down top_menu channel_up channel_down screensaver`; actions `single`
  (default) `double` `hold`. App bundle IDs (complete list from the comment in urem `frontend/activity-atv.js`):
  Fitness com.apple.Fitness, 10 com.networkten.epg, Podcasts com.apple.podcasts, Movies com.apple.TVMovies, FV
  au.com.freeview.Freeview-Plus, TV com.apple.TVWatchList, Photos com.apple.TVPhotos, App Store com.apple.TVAppStore,
  Prime Video com.amazon.aiv.AIVApp, Stan au.com.stan, Arcade com.apple.Arcade, 9Now au.com.ninemsn.jump-in, VLC
  org.videolan.vlc-ios, AMC+ com.amcplus.amcn, Spotify com.spotify.client, 7plus com.swm.live, Computers
  com.apple.TVHomeSharing, SBS On Demand au.com.sbs.ondemand, BINGE au.com.streamotion.ares.prod, YouTube
  com.google.ios.youtube, Vevo com.vevo.appletv, ABC iview au.net.abc.ABCiView, Search com.apple.TVSearch, TV Shows
  com.apple.TVShows, Paramount+ com.cbs.canada.app, NASA gov.nasa.NASA, Disney+ com.disney.disneyplus, Acorn TV
  com.rljentertainment.AcornTV, Settings com.apple.TVSettings, F1 TV com.formula1.ott, Netflix com.netflix.Netflix,
  Music com.apple.TVMusic.
- Targets: `{ real, test }` address sets (irtx, atvd, yamaha base URL, hotpc, slideshow, WoL broadcast), so the
  test server and the layout read ports from one place.

### Devices to fake (all one way unless noted; the device never reads replies except Lua `http.get`)
- **irtx** (UDP 4210): cmd 4 packets as above. No reply.
- **Apple TV daemon** (UDP 7777, UTF-8 text, `atvdaemon/atvdaemon.py`): `ping` → `pong`/`connecting` (starts a
  background connect), `key <name> [single|double|hold]` → `ok`/`error: ...`, `app <bundle>` → `ok`, `disconnect` →
  `ok`, `text_set|text_append <t>`, `text_clear`. It **does reply** (to the sender's address/port; the device ignores
  it). `key`/`app` connect on demand if not connected. The fake should reply the same way and keep a connected flag
  (`disconnect` clears it, `ping` sets it after a short delay) so logs show e.g. "ATV: key menu hold".
  urem used: `ping` (via connect, on turnOn and before `app`), `disconnect` (turnOff), `key up|down|left|right|select|
  menu|play|pause|next|previous`, `key menu hold`, `key select hold`, `app <bundle>`.
- **Yamaha** (HTTP GET, urem `http://yama:80/YamahaExtendedControl/v1/...`, keep-alive): `main/setPower?power=on|
  standby`, `main/setInput?input=hdmi1..4`, `main/setVolume?volume=up|down&step=1|3|5`, `main/setMute?enable=true|
  false`, `main/setSoundProgram?program=7ch_stereo|surr_decoder`, `main/getStatus`. Replies are JSON; setters return
  `{"response_code":0}`. getStatus (real YXC field names; compact JSON, no spaces): `response_code, power ("on"|
  "standby"), sleep, volume (int), max_volume, mute (bool), input, distribution_enable, sound_program,
  surr_decoder_type, direct, enhancer, tone_control{mode,bass,treble}, dialogue_level, subwoofer_volume,
  actual_volume{mode,value,unit}, ...`. urem reads only `mute` and `sound_program`. The fake keeps power, input,
  volume (clamped 0..max_volume, step applied), mute, sound_program, so the toggle scripts can be tested end to end.
  Port: configurable (Windows port 80 may be taken; the test target can use e.g. 8080).
- **HOT PC** (`10.1.1.105`): WoL (MAC `04:d4:c4:4e:da:2b`) to turn on; no turnOff. HTTP POST JSON `{}` (Content-Type
  `application/json`) to `:4500/launch-steam`, `/launch-kiosk`, `/shutdown`.
- **Slideshow** (`10.1.1.2:4500` in urem's Steam activity): UDP text commands `prevDay nextDay backward20 forward20
  play pause voteUp voteDown prev next`; HTTP POST JSON `{"<element>":"toggle"}` to `/display-elements`, elements
  `all transport date time rating location other`. Same port number as the HOT PC's HTTP: in the test target both
  are on one machine, so **one HTTP listener serves both route sets** (or give them different test ports).
- **WoL**: UDP broadcast to :9 (ControlBlock `sendWol`); decode with `wolNote` (to be exported from
  `examples/test-server/test-server.mjs`) and name the MAC ("HOT PC: wake").

### atvdaemon
- `urem/atvdaemon/`: `atvdaemon.py` (pyatv, config constants `ATV_ID`, `ATV_HOST 10.1.1.169`, UDP 7777, binds
  0.0.0.0), `start.sh` (venv + pyatv), `README.md`, `atv.js` (Node client; not needed: the device talks UDP itself).
  Copy `atvdaemon.py`, `start.sh`, `README.md` as is into `urem2/atvdaemon/` (it's not ControlBlock-specific and
  already reachable over the LAN). A possible later change for gap 3 (wait for connection) is a separate decision.
- urem's `atvoff.py` (one-off pyatv power-off script) is unused by the frontend: not ported.
- Daemon internals for the connect-retry change (Decision 3): every command goes through `ATVController._ensure()`
  (under `self._lock`), which calls `connect()` once (scan by `ATV_ID`/`ATV_HOST`, then `pyatv.connect`) and raises on
  failure; `_handle` turns an exception into `error: <msg>` and `invalidate()`s. `ping` starts `connect_bg()` (guarded
  by `_connecting`). So the retry belongs in `_ensure()` (a loop: try `connect()`, on failure sleep ~2 s, give up after
  `CONNECT_WAIT` ~20 s and re-raise the last error); it then covers `key`, `app` and `text_*` alike. If a background
  connect is in flight, `_ensure` should wait for it rather than start a second one (poll `connected` /
  `_connecting` inside the same deadline).

### Plan decisions (2026-11-08)
- **Test command:** `node --test "urem2/**/*.test.mjs"` from the repo root (quoted glob; works in Git Bash and
  PowerShell with Node 24.14). **A directory argument does not work** on this Node: `node --test examples/test-server/`
  fails ("test at examples\test-server:1:1 ✖", it tries to run the directory as a file). The comment at the top of
  `examples/test-server/test-server.test.mjs` says the directory form: fix it to the glob in step 1.
- **No package.json.** Relative imports only (`../../examples/test-server/test-server.mjs`). JSON tables loaded with
  `import ... with { type: "json" }` (stable in Node 24; the bake imports `.mjs` layouts with plain `import()`, so the
  layout will load the same way).
- **Module map** (the layout, sub-job 3, imports everything under `urem2/tables/` and `urem2/targets.mjs`; the server
  imports the same files, so encoder and decoder can't drift):
  - `urem2/tables/ircodes.json`: verbatim copy of urem's `ircodes/ircodes.json`.
  - `urem2/tables/ir.mjs`: `IR_CODES` (the JSON), `irCode("lg.powerOn")` → `"NEC:0x20dfb34c"` (throws on an unknown
    device or name, so a layout typo fails the bake/test), `parseIrCode(str)` → `{ protocol: "NEC"|"PANA", code:
    BigInt }`, `irNames(protocol, code)` → every `device.name` with that code (duplicates are on purpose).
  - `urem2/tables/irtx.mjs`: `IRTX_PORT = 4210`, `irPacket(codeOrStr, { repeat = false } = {})` → hex string (17 bytes,
    the `udpPacket` `data.hex` form), `decodeIrtxPacket(buf)` → `{ protocol, code, repeat }` or `{ error }`.
  - `urem2/tables/appletv.mjs`: `ATV_PORT = 7777`, `ATV_KEYS`, `ATV_ACTIONS`, `ATV_APPS` (display name → bundle id),
    `atvKey(name, action?)` → `"key menu hold"`, `atvApp(nameOrBundle)` → `"app com.netflix.Netflix"`, `ATV_PING`,
    `ATV_DISCONNECT`, and `parseAtvCommand(text)` (mirrors the daemon's `_dispatch` split rules) for the fake.
  - `urem2/tables/yamaha.mjs`: `YAMAHA_API = "/YamahaExtendedControl/v1"`, `YAMAHA_INPUTS` (hdmi1..4),
    `YAMAHA_PROGRAMS` (`7ch_stereo`, `surr_decoder`), path builders returning `/main/...` (relative to the target's base
    URL): `setPower(on)`, `setInput(input)`, `setVolume(dir, step)`, `setMute(on)`, `setSoundProgram(p)`, `getStatus()`;
    each validates its argument.
  - `urem2/tables/hotpc.mjs`: `HOTPC_MAC`, `HOTPC_COMMANDS` (`launch-steam`, `launch-kiosk`, `shutdown`), and the
    slideshow's `SLIDESHOW_COMMANDS` (UDP) and `SLIDESHOW_ELEMENTS` (display-elements) with
    `displayElementsBody(el)` → `{"<el>":"toggle"}` (both live on PCs on :4500; one small module).
  - `urem2/targets.mjs`: `targets = { real, test }`, each `{ irtx: {host, port}, atvd: {host, port}, yamaha: baseUrl
    (incl. `YAMAHA_API`), hotpc: {host, port, mac, wolBroadcast?}, slideshow: {host, udpPort, httpPort} }`. Real per
    Decisions 8/9 (`irtx 10.1.1.188:4210`, `atvd 10.1.1.2:7777` (cool.lan), `http://yama:80/...`, HOT PC
    `10.1.1.105:4500`, slideshow `10.1.1.2:4500`). Test: the PC from `examples/test-common.mjs` `SERVER` with the same
    port numbers except Yamaha on **8080** (Windows port 80 is often taken by http.sys/IIS). The server's defaults
    come from `targets.test`, so the two can't disagree.
- **Server shape:** `urem2/test-server/test-server.mjs` exports `startUremServer({ ports = defaults from
  targets.test, host = "0.0.0.0", log = console.log, now, atvConnectMs = 1500 })` → `{ ports, yamaha /* live state,
  for tests */, close() }`; port 0 = free port (tests). Listeners: UDP irtx 4210, UDP atvd 7777, UDP slideshow 4500,
  UDP WoL 9, HTTP Yamaha 8080, HTTP :4500 (HOT PC and slideshow routes on one listener: the paths don't overlap).
  Can't run alongside `examples/test-server` (both bind UDP :9). Run block at the bottom like the examples server.
- **Log format** (one line per received message, `timestamp()` first, device terms): `IR lg.powerOn` (all names when
  a code is shared: `IR atv.right/atv.forward/...`; `IR unknown NEC:0x...`; malformed → `irtx: bad packet <repr>`),
  `ATV: key menu hold -> ok`, `ATV: app Netflix (com.netflix.Netflix) -> ok`, `Yamaha: setInput hdmi2`, `Yamaha:
  getStatus (mute=false, sound_program=surr_decoder)`, `HOT PC: launch-steam`, `HOT PC: wake (04:d4:c4:4e:da:2b)`,
  `Slideshow: nextDay`, `Slideshow: toggle date`. Unknown routes/commands are logged too (and 404 / `error:`).
- **WoL decode:** refactor `examples/test-server/test-server.mjs`'s private `wolNote` into an exported
  `wolMac(data)` → `{ mac: "aa:bb:...", ok }` or `null`, with `wolNote` (unchanged output) built on it and also
  exported. urem2 names the MAC from `HOTPC_MAC`.
- **Fake ATV:** `connected` flag; `ping` → `pong` if connected, else `connecting` and connected after
  `atvConnectMs`; `key`/`app` when not connected wait `atvConnectMs` then reply `ok` (mimics Decision 3's retry; not a
  test of the real daemon); `disconnect` → `ok`, clears the flag; unknown key/action/command → `error: ...` (same
  messages as the daemon where it has one). Replies go to the sender's address/port.
- **Fake Yamaha:** state `{ power, input, volume, max_volume: 161, mute, sound_program }`; setters reply
  `{"response_code":0}`; bad parameter → `{"response_code":4}` (YXC "invalid parameter"), unknown path → 404;
  `getStatus` compact JSON (`JSON.stringify`, no spaces) with the full field list from the Notes, so the layout's Lua
  string patterns (`'"mute"%s*:%s*true'`) are exercised against the real shape. Keep-alive like the examples server.

### Implement notes
- Step 1: `examples/test-server/test-server.mjs` exports `wolMac(data)` → `{ mac, ok }` | `null` and `wolNote(data)`
  (same output as before). Both header comments now give `node --test "examples/**/*.test.mjs"` (17 tests).
- Step 2: `ircodes.json` hex case is mixed (`NEC:0x20DF609F` for `lg.right`), so `irNames` compares BigInts, never
  strings. Duplicates found: `atv.right` = `atv.forward` = `atv.forward10x`, `atv.left` = `atv.reverse`, `atv.home` =
  `atv.menu`. `irCode` only accepts own properties (`lg.toString` throws) and exactly one dot. `parseIrCode` requires
  upper-case `NEC`/`PANA`, a `0x` prefix and 1..16 hex digits (fits u64). Total 169 codes.
- Step 3: `irPacket` also takes a parsed `{ protocol, code }`; a string containing `:` is treated as a raw code, else a
  dotted name. `decodeIrtxPacket` errors read `length N, expected 17`, `cmd N, expected 4`, `unknown protocol "SONY"`
  (the server can log them after `irtx: bad packet`). Uses `Buffer` (Node only: fine, the bake runs in Node).
- Step 4: `parseAtvCommand` returns `{ cmd, key, action }` (action defaults to `"single"`), `{ cmd, arg }` (app,
  text_set, text_append), `{ cmd }` (ping, disconnect, text_clear) or `{ error }` = the text after `error: ` in the
  daemon's reply (Python-repr quoting copied: `unknown command: 'launch'`, `Unknown key: 'nope'`). The verb and the
  action are lower-cased (as the daemon), the key name isn't (pyatv's `getattr` is case-sensitive). The action is
  checked before the key (daemon order: action at dispatch, key inside `press_key`). `atvKey(name)` with no action
  leaves it out (`"key up"`); `atvApp` accepts own-property display names or any listed bundle id.
- Step 5: `setVolume(dir, step = 1)`; step must be an integer >= 1 (no upper limit checked). Builders throw
  `bad Yamaha <what> <value>`; `setPower`/`setMute` take booleans only.
- Step 6: `targets.{real,test}.hotpc` is `{ host, port, mac }` with **no `wolBroadcast`** (the device's default
  broadcast reaches the test PC, as `examples/net-test.json` showed). `targets.mjs` imports `SERVER` from
  `examples/test-common.mjs` (no side effects there). Test count after step 6: 33.
- Steps 7-10: `urem2/test-server/test-server.mjs` exports `DEFAULT_PORTS` (`irtx 4210, wol 9, atvd 7777, yamaha 8080`
  (parsed from `targets.test.yamaha`), `pc 4500` (HTTP, HOT PC + slideshow routes), `slideshow 4500` (UDP)) and
  `startUremServer({ ports, host, log, now, atvConnectMs = 1500 })` → `{ ports /* bound */, atv /* .connected */,
  yamaha /* live state */, close() /* Promise */ }`. `ports` is merged over the defaults. Listeners bind one at a time;
  a failure (e.g. `EADDRINUSE`) closes the ones already open and rejects. Small helpers `udpListener`/`httpListener`
  (body read before the handler; `keepAliveTimeout = 0`) and `reply`/`replyJson` are shared by every fake.
  Log lines as in the Plan decisions, plus: `IR pana.guide (repeat)`, `irtx: bad packet (<error>) b'...'`,
  `WoL: wake <mac>` for other MACs, `, bad repeats` suffix, `WoL: not a magic packet (N bytes) b'...'`, `ATV:
  (connected)` when a connect completes, ATV keys always show the action (`ATV: key up single -> ok`), apps not in
  `ATV_APPS` show the bundle only, `Yamaha: setVolume up 3 -> 13`, `Yamaha: <name> <query values> -> invalid
  parameter`, `Yamaha: unknown GET <url> -> 404`, `Slideshow: display-elements bad body b'...'` (400),
  `PC: unknown POST <url> -> 404`, `Slideshow: unknown command b'...'`. The fake ATV replies parse errors at once
  (no connect wait). Fake Yamaha starts `power standby, input hdmi1, volume 60, mute false, sound_program
  surr_decoder`; `actual_volume.value` = `(volume - 161) / 2` dB. Test count after step 10: 57 (urem2), 17 (examples).
- **Real-port run (2026-11-08, step 10):** UDP :9 was taken by the user's running `examples/test-server` (node,
  `test-server.mjs`): the server refused to start (`EADDRINUSE 0.0.0.0:9`) as designed; not stopped. With WoL on a free
  port, every other real port bound (4210, 7777, 8080, 4500 TCP + UDP) and each fake answered over `10.1.1.126`
  (= `SERVER`, this PC). **Port 8080 on localhost is taken by `wslrelay`** (an Apache in WSL, `127.0.0.1:8080` and
  `::1:8080`): our `0.0.0.0:8080` still binds and serves the LAN address (what the device uses), but
  `http://127.0.0.1:8080` on this PC reaches WSL's Apache (404 HTML). Use the LAN address when poking the fake Yamaha
  by hand.

- Step 11: `atvdaemon.py`, `start.sh`, `README.md` copied with `cp -p`; `cmp` identical to `../urem/atvdaemon/`. Line
  endings: urem's index holds LF (`git ls-files --eol`: `i/lf w/crlf`), its working copy is CRLF through
  `core.autocrlf=true`, and ControlBlock has the same setting, so the copies are CRLF on disk and become LF when
  committed (as in urem). `core.filemode` is false in both repos: **when committing, run `git update-index --chmod=+x
  urem2/atvdaemon/start.sh`** (urem's index has it 100755) so it stays executable on the NAS. `atv.js` not copied (as
  planned).
- Step 12: constants `CONNECT_WAIT = 20`, `CONNECT_RETRY = 2`, `CONNECT_POLL = 0.2` in the Configuration block.
  `_ensure()` returns at once if connected; otherwise loops until a deadline: if `_connecting` is set (ping's
  `connect_bg`) it polls every `CONNECT_POLL` s, else it sets `_connecting` itself (so a `ping` meanwhile replies
  `connecting` and doesn't start a second connect), calls `connect()`, logs a failure, clears the flag, then sleeps
  `min(CONNECT_RETRY, remaining)`. Past the deadline it raises the last connect error (or `RuntimeError("ATV not
  connected after 20 s")` if only a background connect failed) → `_handle` replies `error: <msg>`. The deadline is
  checked after each attempt, so the reply can come one scan (pyatv's scan timeout, a few s) past 20 s. `_ensure` runs
  under `self._lock`, so commands queue behind a waiting one. Module docstring (+ `disconnect`, which was missing) and
  README "Connection behaviour" updated. `python -m py_compile` (Python 3.11.9): OK (the `__pycache__` it made was
  deleted). **Not automated: hardware check for the user on the NAS** (copy `urem2/atvdaemon/` over, `./start.sh`):
  (a) Apple TV unplugged/unreachable: `echo "key menu" | nc -u -w25 <nas> 7777` replies `error: ...` after ~20 s, log
  shows a `Connect failed` line about every 2 s plus scan time; (b) Apple TV asleep then woken (or daemon started
  while it's off, then turned on): `key`/`app` replies `ok` once connected, within 20 s; (c) `ping` then `key` straight
  away: one connect in the log, not two; (d) connected: replies are immediate as before.
- Step 13: every listed file present; `node --test "urem2/**/*.test.mjs"` 57/57 pass, `node --test
  "examples/**/*.test.mjs"` 17/17 pass. "How to run" copied to the umbrella's Notes. CLAUDE.md test-command line put to
  the user (not edited).

### How to run (for sub-job 3's README)
- `node urem2/test-server/test-server.mjs` on the PC whose IP is `SERVER` in `examples/test-common.mjs`; bake the test
  entry of the layout. Allow node.exe through the Windows firewall. Not alongside `examples/test-server` (UDP :9):
  stop that one first, or the start fails with `EADDRINUSE 0.0.0.0:9`. Prints the bound ports. On this PC
  `127.0.0.1:8080` is WSL's Apache (`wslrelay`): poke the fake Yamaha at the LAN address.
- Tests: `node --test "urem2/**/*.test.mjs"`.

## Flagged assumptions
- The urem2 server lives in `urem2/test-server/` and imports `timestamp`/`repr`/`wolNote` from
  `examples/test-server/test-server.mjs` (exporting `wolNote` there), rather than copying them.
- The fake ATV daemon is in Node (part of the test server), not the Python daemon in a fake mode.
- (Plan) The test target's host is `SERVER` imported from `examples/test-common.mjs` (the same PC as the hardware
  test kit), not a separate constant; urem2 then depends on `examples/` for that one value and for `wolMac`/`timestamp`/
  `repr`.
- (Plan) Test target ports: same as the real devices (4210, 7777, 4500, 9) except Yamaha on 8080.
- (Plan) `irCode` takes one dotted string (`"pana.guide"`) rather than two arguments; `irPacket` accepts either that
  dotted name or a raw `"NEC:0x..."` string.
- (Plan) Fake Yamaha `max_volume` 161 and bad-parameter `response_code` 4 (YXC conventions from memory, not checked
  against a receiver); urem reads only `mute` and `sound_program`, so nothing depends on them.
- (Plan) The fake daemon's `key`/`app` wait the same `atvConnectMs` as `ping` when not connected, then succeed (no
  simulated failure mode; add one only if the layout needs it).
- (Plan) The daemon's retry: `CONNECT_WAIT = 20` s, `CONNECT_RETRY = 2` s, applied in `_ensure()` so `text_*` get it too.
- (Plan) Propose (Rule 4, needs the user's OK; not done here) a CLAUDE.md line under "CLI and binpack tests": "From the
  repo root: `node --test "urem2/**/*.test.mjs"` (a directory argument doesn't work with Node 24's `--test`)". Sub-job 3
  adds the Repository Layout line.
- (Implement) `irPacket` and `decodeIrtxPacket` use Node's `Buffer` (the layout is baked in Node; the bake core already
  uses `Buffer`), not a browser-safe `DataView`.
- (Implement) `irPacket` accepts a parsed `{ protocol, code }` object as well as the two string forms.
- (Implement) `ATV_KEYS` is the README list in the Notes (20 keys); the real daemon accepts any pyatv
  `RemoteControl` method (e.g. `skip_forward`, `turn_on`), so the fake replies `error: Unknown key` to those.
- (Implement) Yamaha volume step: any integer >= 1 accepted (YXC's real upper limit not checked).
- (Implement) No `wolBroadcast` in targets: `sendWol` uses the device default for both real and test.
- (Implement) Fake HOT PC / slideshow HTTP: routes need `POST` (other methods → 404); replies are plain `ok` (the
  real servers' reply bodies weren't checked; the device ignores them). `/display-elements` accepts several elements in
  one body (each must be a listed element with value `"toggle"`), logged `Slideshow: toggle date, time`. Slideshow UDP
  commands are trimmed and case-sensitive.
- (Implement) Fake Yamaha accepts only `volume=up|down` (no absolute volume) and any method; no `power` check (setters
  work in standby, which a real receiver may refuse).
- (Implement) Fake ATV logs a `ATV: (connected)` line when its simulated connect completes (not something the daemon
  prints; useful to see the connect delay).
- (Implement) Windows `wslrelay` holds `127.0.0.1:8080` on this PC: the fake Yamaha stays on 8080 (it serves the LAN
  address fine); move it (`targets.test.yamaha`) only if that WSL Apache ever binds `0.0.0.0`.
- (Implement) Daemon: while a command waits in `_ensure`, it holds `self._lock`, so later commands queue behind it
  for up to ~20 s (then each gets its own ~20 s wait if still unreachable). Accepted: the alternative (fail fast
  after the first timeout) would lose the wake-up case.
- (Implement) Daemon: the copied files stay CRLF on disk (autocrlf), LF in git; `start.sh` needs `git update-index
  --chmod=+x` at commit time.

## Plan
TDD: each code step writes or extends its tests first, then the code; finish each step with
`node --test "urem2/**/*.test.mjs"` (and step 1 also `node --test "examples/**/*.test.mjs"`) all passing.

- [x] 1. **Export the WoL decode.** In `examples/test-server/test-server.test.mjs` first add unit tests for
  `wolMac` (valid magic → `{mac, ok: true}`, a bad repeat → `ok: false`, wrong length / no FF header → `null`) and
  `wolNote` (output unchanged). Then refactor `test-server.mjs`: `export function wolMac(data)`, `export function
  wolNote(data)` built on it. Fix the test file's header comment to `node --test "examples/**/*.test.mjs"`.
- [x] 2. **IR table.** Copy urem's `ircodes/ircodes.json` to `urem2/tables/ircodes.json` verbatim (check with `cmp`).
  Tests first (`urem2/tables/ir.test.mjs`): per-device counts (atv 28, lg 35, pana 53, yama 53: counted with `JSON.parse` at Plan; the umbrella's 33/55 was
  off), `irCode("lg.powerOn")`
  = `"NEC:0x20dfb34c"`, unknown device/name throws, `parseIrCode` (NEC and PANA, BigInt, bad prefix throws),
  `irNames` returns every duplicate (`atv.right` and `atv.forward`) and `[]` for an unknown code. Then `ir.mjs`.
- [x] 3. **irtx packet encoder and decoder.** Tests first (`urem2/tables/irtx.test.mjs`) with hand-computed vectors
  (layout per the umbrella §2: `u16 cmd=4 | u16 0 | FourCC | u64 code LE | u8 repeat`):
  `lg.powerOn` (`NEC:0x20dfb34c`) → `040000004e4543204cb3df200000000000`;
  `PANA:0x40040d08989d` → `0400000050414e419d98080d0440000000`; repeat → last byte `01`. Decoder: the vectors back,
  a round trip over every code in the table, and `{error}` for a wrong length, cmd ≠ 4, an unknown FourCC. Then
  `irtx.mjs` (`IRTX_PORT`, `irPacket`, `decodeIrtxPacket`).
- [x] 4. **Apple TV table.** Tests first (`urem2/tables/appletv.test.mjs`): `atvKey("menu","hold")` = `"key menu
  hold"`, `atvKey("up")` = `"key up"`, unknown key/action throws; `atvApp("Netflix")` and `atvApp("com.netflix.Netflix")`
  both → `"app com.netflix.Netflix"`, unknown app throws; all 32 apps from the Notes present; `parseAtvCommand` on each
  command form (case of the verb ignored, extra spaces, missing argument). Then `appletv.mjs`.
- [x] 5. **Yamaha table.** Tests first (`urem2/tables/yamaha.test.mjs`): each builder's exact path (e.g.
  `setVolume("up", 3)` = `/main/setVolume?volume=up&step=3`, `setMute(true)` = `/main/setMute?enable=true`,
  `setPower(false)` = `...power=standby`), bad arguments throw (`hdmi5`, step 0, unknown program). Then `yamaha.mjs`.
- [x] 6. **HOT PC / slideshow table and targets.** Tests first (`urem2/tables/hotpc.test.mjs`,
  `urem2/targets.test.mjs`): command and element lists, `displayElementsBody("date")` = `{"date":"toggle"}` as a
  string; `targets.real` and `targets.test` have the same keys (deep shape), real values per Decisions 8/9, test host =
  `SERVER`, Yamaha base URLs end in `YAMAHA_API`. Then `hotpc.mjs` and `targets.mjs`.
- [x] 7. **Server skeleton: irtx and WoL.** Tests first (`urem2/test-server/test-server.test.mjs`, same helpers as the
  examples test: free ports on 127.0.0.1, captured log, `waitFor`): an `irPacket` datagram logs `IR lg.powerOn`; a
  shared code logs all names; unknown code and malformed packet logged as such; a WoL packet for `HOTPC_MAC` logs
  `HOT PC: wake`, another MAC logs it raw; `close()` frees the ports. Then `startUremServer` with the irtx and WoL
  listeners, the run block (prints the ports) and the header comment (how to run, test command, the :9 clash).
- [x] 8. **Fake ATV daemon.** Tests first: `ping` → `connecting` then `pong` after `atvConnectMs` (short in tests);
  `key menu hold` → `ok` and the log line; `key`/`app` while disconnected reply `ok` only after the connect delay;
  `disconnect` → `ok` and the next `ping` is `connecting`; unknown key/action/command and missing argument → `error:
  ...`; replies reach the sender's port. Then the UDP 7777 listener.
- [x] 9. **Fake Yamaha.** Tests first (HTTP over a keep-alive agent): each setter changes the state and returns
  `{"response_code":0}`; volume steps clamp at 0 and `max_volume`; bad parameter → `response_code` 4; unknown path →
  404; `getStatus` is compact JSON with the listed fields and reflects the state; a mute toggle done the way the
  layout's Lua will (get, match `"mute":true`, set the opposite) flips it twice; the log says `Yamaha: setInput hdmi2`.
  Then the HTTP listener on `ports.yamaha`.
- [x] 10. **Fake HOT PC and slideshow.** Tests first: POST JSON to each `HOTPC_COMMANDS` path logs `HOT PC: <cmd>` and
  replies 200; POST `/display-elements` `{"date":"toggle"}` logs `Slideshow: toggle date`; bad JSON / unknown element /
  unknown path logged and answered 400/404; UDP slideshow commands log `Slideshow: nextDay`, unknown ones logged. Then
  the shared HTTP :4500 listener and the slideshow UDP listener. Run the server once by hand
  (`node urem2/test-server/test-server.mjs`) to check it binds the real ports and prints them; Ctrl+C.
- [x] 11. **Copy the ATV daemon.** Copy `../urem/atvdaemon/atvdaemon.py`, `start.sh`, `README.md` to
  `urem2/atvdaemon/` unchanged (check with `cmp`; `start.sh` keeps LF endings and its executable intent). No
  automated test (Python).
- [x] 12. **Daemon connect retry (Decision 3).** In `urem2/atvdaemon/atvdaemon.py`: `_ensure()` retries
  scan+connect every `CONNECT_RETRY` (2 s) for up to `CONNECT_WAIT` (20 s), waiting on an in-flight background connect
  instead of starting a second, then re-raises the last error (→ `error: ...`); constants in the Configuration block.
  Update the docstring and `README.md` (key/app/text commands wait up to ~20 s for the connection). Check with
  `python -m py_compile urem2/atvdaemon/atvdaemon.py`. **Not automated: test by hand** on the NAS with the Apple TV
  asleep/unreachable (reply after ~20 s with `error:`) and waking (reply `ok` once connected); record as a hardware
  check for the user.
- [x] 13. **Wrap up.** Check every listed file exists, run both test globs, copy the "How to run" note into the
  umbrella doc's Notes for sub-job 3's README, and put the CLAUDE.md test-command proposal (Flagged assumptions) to
  the user. Rename this doc to `complete-urem2-test-server.md`.
