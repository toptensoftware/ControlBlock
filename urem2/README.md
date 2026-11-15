# urem2

A port of **urem** (`../urem/`: the Raspberry Pi + USB key pad + macro pad universal remote) to a ControlBlock
standalone layout. The layout drives the same equipment urem did: a Yamaha receiver (HTTP), an LG TV and a Panasonic
DVR (IR through the **irtx** blaster), an Apple TV (IR power plus the **Apple TV daemon**), and the HOT PC and its
slideshow (Wake-on-LAN, HTTP, UDP). It also serves as a worked example of a larger `.mjs` layout.

Everything runs from the repo root, needs no `npm install` of its own (it imports `../host/index.js` by relative
path), and works on Node 24.

## Files

```
urem2.mjs              Entry: the house (targets.real)
urem2-test.mjs         Entry: the test server (targets.test)
targets.mjs            Device addresses for each target
tables/                Complete tables, shared with the test server: IR codes (ircodes.json from urem) and the irtx
                       packet encoder, Apple TV keys and apps, Yamaha paths, HOT PC and slideshow commands
layout/                The layout as modules:
  build.mjs              buildLayout(target): root look, devices, Home, the activities
  context.mjs            ctx = { target, ops }: what every module builds from
  ops.mjs                Op builders over the tables (ir, irPower, atvKey, atvApp, yamaha, hotpc, wake, slideshow, ...)
  look.mjs               Root look (border face, grey pressed fill), colours, ICON_SCALE, icon()/label()
  keys.mjs               key(), the repeat check, the shared single keys (7ch Stereo, mute, Vol+, Vol-)
  pad.mjs                Macro pad and encoder bindings; the volume bindings every activity gets
  devices.mjs            Devices (power op lists), activity meta (devices, Yamaha input, name), switch and resync
  lua.mjs, scripts.mjs   Lua scripts with a generated prelude of constants (addresses, patterns, step tables)
  home.mjs               Home
  activities/            atv, hdd, steam, playstation
scripts/               Lua: yamaha-toggle.lua (mute and 7ch Stereo), volume.lua (encoder)
icons/                 Netflix, Prime Video and YouTube SVGs (copied from urem)
test-server/           Fake devices for the hardware test (logs what each one received)
atvdaemon/             The Apple TV daemon (Python, pyatv), runs on the NAS (submodule: toptensoftware/atv-daemon)
test/                  Test helpers (resolve, page key checks, a wasmoon Lua harness)
```

Every code, path and command comes from `tables/`; every address from `targets.mjs`. The layout and the test server
use the same tables, so they can't disagree.

## Tests

```sh
node --test "urem2/**/*.test.mjs"
```

(A directory argument doesn't work on Node 24.) Covers the tables and encoders, the test server, every op builder,
the Lua scripts (run in wasmoon against the same fake Yamaha the test server uses), every key on Home and in each
activity against urem's map, both targets resolving clean and talking only to their own addresses, and a full bake of both entries with the device's key checks (`examples/bake-check.mjs`: ids,
1U rects, opaque images of the key's size).

## Baking and uploading

```sh
node cli/cli.js upload urem2/urem2-test.mjs          # against the test server
node cli/cli.js upload urem2/urem2.mjs               # the house
node cli/cli.js bake urem2/urem2-test.mjs -o scratch/urem2-test.bin    # bake only (~440 KB, 124 images)
```

The target is chosen by the entry file (the CLI can't pass options to a layout). Both entries are one line:
`export default buildLayout(targets.real | targets.test)`.

## Test server

```sh
node urem2/test-server/test-server.mjs
```

Run it on the PC whose IP is `SERVER` in `examples/test-common.mjs` (the test target's address) and upload
`urem2-test.mjs`. It logs every request in device terms (`IR lg.powerOn`, `Yamaha: setInput hdmi2`, `ATV: app ...`,
`HOT PC: wake (...)`) and prints the ports it bound: irtx UDP 4210, WoL UDP 9, ATV daemon UDP 7777, Yamaha HTTP 8080,
HOT PC and slideshow HTTP 4500, slideshow UDP 4501 (the house uses 4500, but Windows drops inbound UDP from other
machines on 4500, the IPsec NAT-T port, so the test target moves it).

- Allow node.exe through the Windows firewall.
- It can't run alongside `examples/test-server` (both bind UDP 9): stop that one first, or the start fails with
  `EADDRINUSE 0.0.0.0:9`.
- On the development PC `127.0.0.1:8080` is WSL's Apache (`wslrelay`): poke the fake Yamaha at the LAN address.

## Apple TV daemon

`atvdaemon/` is urem's daemon (UDP text commands on 7777: `key <name>`, `app <bundle>`, `ping`, `disconnect`; see its
README), moved from the Pi to the NAS (`cool.lan`, `10.1.1.2`). One change: `key` and `app` now retry the connection
for up to ~20 s before answering `error:`, because urem waited for the connection (`ping`) before sending and
ControlBlock can't read UDP replies. It runs in Docker on the NAS (`compose.yaml`, host networking; settings from
`ATVD_*` environment variables) and `atvbench.py` times round trips; see its README. **Not yet tested on the NAS.**

## Key map

Every page lists each of its keys explicitly with its position, `{ ...key(...), x, y }` (plus `w`, `h` when not 1U;
grid units, 5 x 3), so a key moves or resizes by editing its own line. The helpers in `layout/keys.mjs` (and the
activities' local ones, like hdd's `irKey`, `num` and `ch`) each make **one** key with no position; there are no
helpers that lay out groups of keys. Keys get the bake's default ids (bindings `touch[id]`). The activity tests
don't pin pages (only meta, the macro pad bindings and a clean resolve with every key drawn), so keys can be moved,
added or dropped without test changes; the bake test checks the geometry as the device does. Home's test still
lists its keys (`test/pages.mjs` `assertKeys`, any order and position). Most pages still follow urem's 15-key 5x3 grid, with urem's
column 0 navigation keys (Home toggle, pager arrows, clock) left empty: swipe instead.

**Auto-repeat** is ported with urem's settings (`wip/complete-auto-repeat.md`): rewind/forward on Apple TV and Steam
(`repeat: 100`, so the first repeat waits for the swipe window, 150 ms) and the pad arrows on Apple TV and TV
(`{ initialDelay: 500, period: 100 }`; on-screen on the pad page too). The pad's repeats go in each activity's `pad`
map (`padControls()` in `layout/pad.mjs`); the physical pad's repeat waits for the macro pad's S3 phase
(`wip/wip-macro-pad.md`).

- **Home:** power (everything off, 1U) and PlayStation, Steam, Apple TV and TV as 2 x 1.5 keys. Being reworked: the
  Netflix / Prime Video / YouTube keys (switch to Apple TV, then open the app), 7ch Stereo and the DVR list / guide /
  function keys (switch to TV, then the IR key) are commented out in `layout/home.mjs` for now. **Long press** an
  activity key (PlayStation, Steam, Apple TV, TV) to switch and then re-send every device's power-on and the receiver
  input (re-sync drifted equipment).
- **Apple TV:** transport, 7ch Stereo, mute. Pad: menu (long: menu hold), arrows, select (long: select hold).
- **TV (DVR):** three pages: transport and skips; number keys, last channel, guide; colour keys, subtitles, channel
  up/down, drive select, delete, record. Pad: return, arrows, OK.
- **Steam:** HOT PC launch Steam / slideshow / shut down, slideshow transport and info; page 2 the slideshow's
  display-element toggles. Pad: vote up/down, previous/next.
- **PlayStation:** 7ch Stereo and mute (urem's had no keys; see below).
- **Every activity:** the encoder sets the volume (step 1/3/5 by spin speed, as urem), pad key 7 (encoder push)
  toggles mute.
- **Pad page** (the first page of Apple TV, TV and Steam; not PlayStation, which has no pad keys): on-screen keys
  doing the pad's job while the pad hardware isn't ready. The pad's 2x3 shape in rows 2-3, columns 2-4 (red cross =
  Escape, up, green tick = Enter / left, down, right), Vol+, Vol- and mute down the right-hand column. Each activity
  writes these keys out like any other page, next to its `pad` spec (the physical pad's bindings), so keep the two
  in step by hand: a key the pad leaves unbound (Steam's cross and tick) is left off. Every key fires on tap (touch
  keys have no press event; long kept, e.g. Apple TV's menu/select hold; the arrows repeat as the pad's do), and
  Vol+/Vol- move 1 step per tap and repeat while held (`VOLUME_REPEAT` in `layout/keys.mjs`: 500 ms, then every
  100 ms).

Look: each key has a grey rounded border on black; pressed, the square fills mid grey. Icons are button-face
built-ins with urem's colours, scales and offsets, times `ICON_SCALE` (`layout/look.mjs`, 0.9 for now; the glyphs
come out ~50-83 px, so it may go up after a look on the hardware).

## What couldn't be ported

| urem feature | In urem2 | What ControlBlock would need |
|---|---|---|
| **Display off / idle dim** (off with no activity, dim after 8 s idle, first touch only wakes) | Not ported: the screen stays lit. | The backlight is locked on (switch-on glitch, `docs/backlight.md`); then a standalone idle policy. Backlogged in `docs/backlight.md` Open items. |
| **Re-selecting the current activity re-syncs** (urem re-ran every power-on and the input) | Long press on Home's activity keys (switch, then re-send power-on for its devices and the input, then power-off for every other device), and on the power key (everything off again). Tap only switches; reselecting the current activity only glides. | Possibly a `resync` option on `switchActivity`. |
| **Global pad bindings** (urem's volume encoder and mute button worked everywhere, even on Home) | The volume bindings are repeated in every activity; with no activity selected the pad does nothing (the receiver is off then anyway). | Root-level (global) bindings. |
| **Encoder push = mute** (urem's 7th button) | Bound as `key7` tap in every activity, but **inert**: the pad's pin map has 6 keys and no encoder push. | A 7th key in the macro pad's pin map (`wip/wip-macro-pad.md`). |
| **Press shrink** (urem shrank the image while pressed) | Grey fill instead (the root `pressed` look). | Nothing needed; a different look. |
| **Replies from irtx / the Apple TV daemon** (urem polled `ping` until connected) | No replies (`udpPacket` is one way): the daemon itself now waits for the connection. | UDP replies in ops or scripts (`udp.send` that can wait for an answer). |
| **Yamaha toggles** (mute, 7ch Stereo: read the status, set the opposite) | Lua scripts (`http.get` the status, string search, set). No JSON library in the sandbox. | Works; scripts from a layout not yet run on the hardware. |
| **Encoder speed steps** | Lua script on the encoder's `turn` (`delta`, `interval`); a coalesced event (|delta| > 1) sends one request with the step times |delta|, at most 10. | Works; not yet run on the hardware (the pad isn't built). |
| **Clock, Moonraker printer status, page navigation keys** | Not ported, by request (swipe replaces the pager). | — |
| **PlayStation activity with no keys** | One page with the 7ch Stereo and mute keys (urem had them globally). ControlBlock needs at least one page. | — |
| **The DVR as a device** (urem's `pana` had no power control) | Dropped: TV lists Yamaha and LG only. | — |
| **`yama` host name** | Used as in urem (`http://yama:80/...`). lwIP may not add the DHCP domain to a bare name: if it doesn't resolve, use `yama.lan` in `targets.real`. | To check on the hardware. |

ControlBlock gained one feature for this port: `switchActivity` now runs the switch's own lists before the rest of
the list it's in, so a key can be `[switchActivity, follow-up]` (Netflix: switch to Apple TV, then open the app).

## Not yet checked on the hardware

Everything above runs in the tests, but not yet on the device: the look and icon size, the device order and the IR
gap (`IR_GAP_MS`, 120 ms after each IR power packet, so the blaster never has three in flight), the Lua toggles and
volume script, the long-press resync, and the real target at home (`yama` resolving, the daemon on the NAS). The
checklist is step 17 of `wip/complete-urem2-layout.md`.
