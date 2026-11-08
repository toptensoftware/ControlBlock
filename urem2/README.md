# urem2

A port of **urem** (`../urem/`: the Raspberry Pi + Stream Deck + macro pad universal remote) to a ControlBlock
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
  keys.mjs               key(), grid(), the transport row, the shared 7ch Stereo and mute keys, the repeat placeholder
  pad.mjs                Macro pad and encoder bindings; the volume bindings every activity gets
  devices.mjs            Devices (power op lists), activity meta (devices, Yamaha input, name), switch and resync
  lua.mjs, scripts.mjs   Lua scripts with a generated prelude of constants (addresses, patterns, step tables)
  home.mjs               Home
  activities/            atv, hdd, steam, playstation
scripts/               Lua: yamaha-toggle.lua (mute and 7ch Stereo), volume.lua (encoder)
icons/                 Netflix, Prime Video and YouTube SVGs (copied from urem)
test-server/           Fake devices for the hardware test (logs what each one received)
atvdaemon/             The Apple TV daemon (Python, pyatv), runs on the NAS
test/                  Test helpers (resolve, slot checks, a wasmoon Lua harness)
```

Every code, path and command comes from `tables/`; every address from `targets.mjs`. The layout and the test server
use the same tables, so they can't disagree.

## Tests

```sh
node --test "urem2/**/*.test.mjs"
```

(A directory argument doesn't work on Node 24.) Covers the tables and encoders, the test server, every op builder,
the Lua scripts (run in wasmoon against the same fake Yamaha the test server uses), every key on Home and in each
activity against urem's map, both targets resolving clean and talking only to their own addresses, and one full bake.

## Baking and uploading

```sh
node cli/cli.js upload urem2/urem2-test.mjs          # against the test server
node cli/cli.js upload urem2/urem2.mjs               # the house
node cli/cli.js bake urem2/urem2-test.mjs -o scratch/urem2-test.bin    # bake only (~134 KB, 56 images)
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
ControlBlock can't read UDP replies. **Not yet tested on the NAS.**

## Key map

Same grid as urem's 15-key Stream Deck (index = row * 5 + column). urem's column 0 navigation keys (Home toggle, pager
arrows, clock) are empty: swipe instead.

- **Home:** PlayStation, Steam, power (everything off), Apple TV, Netflix / Prime Video / YouTube (switch to Apple TV,
  then open the app), 7ch Stereo, TV, DVR list / guide / function (switch to TV, then the IR key). **Long press** an
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

Look: each key has a grey rounded border on black; pressed, the square fills mid grey. Icons are node-buttonImage
built-ins with urem's colours, scales and offsets, times `ICON_SCALE` (`layout/look.mjs`, 0.9 for now; the glyphs
come out ~50-83 px, so it may go up after a look on the hardware).

## What couldn't be ported

| urem feature | In urem2 | What ControlBlock would need |
|---|---|---|
| **Auto-repeat while held** (rewind/forward on Apple TV and Steam, the pad arrows) | Single shot. Each key carries its urem `repeat` setting, which `withRepeat()` in `layout/keys.mjs` drops for now: one place to change once repeat exists. | A `repeat` gesture (fires every period after an initial delay while held), for touch keys and the pad. |
| **Display off / idle dim** (off with no activity, dim after 8 s idle, first touch only wakes) | Not ported: the screen stays lit. | The backlight is locked on (switch-on glitch, `docs/backlight.md`); then a standalone idle policy. Backlogged in `docs/backlight.md` Open items. |
| **Re-selecting the current activity re-syncs** (urem re-ran every power-on and the input) | Long press on Home's activity keys (switch, then re-send power-on and input). Tap only switches; reselecting the current activity only glides. | Possibly a `resync` option on `switchActivity`. |
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
