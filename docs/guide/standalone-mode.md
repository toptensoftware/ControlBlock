# Standalone mode

In standalone mode the ControlBlock runs on its own: no app on your PC needs to be running. You describe its keys
and what they do in a **layout file**, the CLI **bakes** it into a blob, and you **upload** the blob to the device,
which keeps it in flash and loads it at every boot.

A key can type keystrokes over USB (the device is also a USB keyboard), send HTTP requests and UDP packets, wake a
PC with wake-on-LAN, or run a small Lua script. Keys are grouped into **activities** ("Watch TV", "Music"), each
with its own pages and the equipment it needs switched on.

This guide assumes the device is flashed and connected, the tools are installed, and (for network actions) Wi-Fi
is set up: see [Getting started](./getting-started.md). `controlblock` below stands for however you run the CLI
(`npx github:toptensoftware/ControlBlock#cli`, an installed `controlblock`, or `node cli/cli.js` in a clone of the
repo; see [Installing the tools](./getting-started.md#installing-the-tools)). The examples in `examples/` and the
test server need a clone of the repo; run those commands from its root.

The full reference for the layout file (every property, every op, the Lua API, the validation rules) is
[`docs/layout-format.md`](../layout-format.md). This guide explains how the pieces fit and walks through
examples; it links there for the details.

## Concepts

**Home and pages.** The screen shows one page of keys at a time, a 5x3 grid of 144 px cells, and you swipe left
and right between pages. **Home** is always the first page. It's where the device starts after a boot.

**Activities.** An activity is a set of pages for one thing you do: watching TV, listening to music, working in
an app. A Home key **selects** an activity: its pages appear to the right of Home and the screen glides to the
first of them. Swipe back to Home to pick another. Only one activity is selected at a time, and none is selected
after a boot.

**Devices.** A device is a piece of equipment the layout controls (a TV, a receiver, a PC) with two lists of
actions: how to turn it on and how to turn it off. Each activity lists the devices it uses. When you switch
activities, the ControlBlock turns on what the new activity needs and turns off what's no longer needed, and
leaves alone a device both activities use (no TV power cycle between "Watch TV" and "Play games").

**Ops and the queue.** Everything a key does is a list of **ops** (operations): `hidKey`, `httpGet`, `httpPost`,
`udpPacket`, `sendWol`, `delay`, `switchActivity` and `script`. Pressing a key puts its op list on the device's
single **op queue**, which runs lists one after another, in order. A `delay` holds the whole queue, so later key
presses wait behind it.

**Gestures.** A key can do different things on `press`, `release`, `tap` and `long` (long press), and can repeat
while held.

**Key ids.** Every key has a number, its **id**, unique across the whole layout. The bake gives each key one
automatically; you only need to choose ids yourself in special cases (see [Key ids](#key-ids)).

## Tutorial: your first layout

### 1. Write the layout

Save this as `my-layout.json` in any folder, and run the commands below from that folder:

```json
{
  "version": 1,
  "defaults": {
    "color": "#22262e",
    "pressed": { "color": "#3a86ff" }
  },
  "home": {
    "keys": [
      { "icon": "#play-pause", "label": "Play", "action": { "op": "hidKey", "keys": "space" } },
      { "label": "Copy", "action": { "op": "hidKey", "keys": "ctrl+c" } },
      { "label": "Paste", "action": { "op": "hidKey", "keys": "ctrl+v" } },
      null,
      { "icon": "#settings", "label": "Tasks", "color": "#8338ec",
        "action": { "op": "hidKey", "keys": "ctrl+shift+esc" } }
    ]
  }
}
```

- `version` is the layout file format (always `1` for now), and `home` holds Home's keys. Those two are all a
  layout needs.
- `home.keys` fills the grid's slots in order, left to right and top to bottom: the first five entries are the
  top row. `null` leaves a slot empty (black, and it only swipes).
- `defaults` gives every key a dark grey face that turns blue while pressed. The last key overrides the colour.
- `icon` names one of the [built-in icons](#icons) (they start with `#`), and `label` is the text under it.
- `action` is what a tap does: here, the `hidKey` op types a key or a chord on the PC the device is plugged into.

### 2. Preview it

```sh
controlblock render my-layout.json
```

This draws each page as the device would show it, as PNG files in `my-layout-pages/` (here just
`00-home.png`). Add `--pressed` to see the pressed look. No device is needed.

### 3. Bake and upload it

```sh
controlblock upload my-layout.json
```

`upload` bakes the layout and sends it to the device, which saves it, restarts and comes back with your Home
page. The CLI shows the progress, waits for the restart and checks the device kept the layout:

```
device <serial> (firmware <version>)
  ...                     (progress, every 10%)
saved 32756 bytes; waiting for the device to restart...
device restarted and verified: 32756 bytes, crc 0x<crc>
```

Open a text editor on the PC and tap **Copy** and **Paste**: the ControlBlock types them like a keyboard.

To bake without uploading (to check for errors, or to keep the blob), use `controlblock bake my-layout.json`,
which writes `my-layout.bin`; `upload` also takes a `.bin`. Uploading a layout the device already has sends
nothing (add `--force` to send it anyway). See [`cli/README.md`](../../cli/README.md) for every option.

### 4. Change it and repeat

Edit the file, `render` to check, `upload` to try it. The bake checks the whole layout before anything is sent and
lists every problem with its place in the file (see [Bake errors](#bake-errors)), so a broken layout never reaches
the device.

The rest of this guide builds up a larger layout: two activities, devices, network actions and a script. The
complete file is shown in [A complete example](#a-complete-example).

## Layout files: JSON or JavaScript

A layout is either a `.json` file or a JavaScript module (`.mjs` or `.js`) whose default export is the layout
object. They describe the same thing; the bake reads both.

JSON is fine for small layouts. A module helps as soon as you repeat yourself: comments, constants for addresses,
and small functions that build keys and ops:

```js
// my-layout.mjs
const AVR = "http://192.168.1.51";
const type = keys => ({ op: "hidKey", keys });
const avr = path => ({ op: "httpGet", url: `${AVR}${path}` });

export default {
    version: 1,
    home: {
        keys: [
            { label: "Copy", action: type("ctrl+c") },
            { label: "Power", action: avr("/power?on=1") },
        ],
    },
};
```

The functions run at bake time; what they return must be plain data (no functions left in the layout). Imports
work as usual, so a big layout can be split into modules; [urem2](../../urem2/README.md) is a real layout built
that way.

Some rules that apply to both forms (details in [File basics](../layout-format.md#file-basics)):

- File paths in a layout (SVG icons, Lua scripts) are relative to the **layout file's folder**, not the folder you
  run the CLI from.
- An unknown property is an error, so a typo like `lable` can't pass silently. A property whose name starts with
  `_` or `$` is ignored: use it for notes.

## Placing keys

The grid is 5 cells wide and 3 high. Positions and sizes are in **grid units** (1 unit = one cell), in steps of a
quarter.

- **By slot:** entry `i` of a page's `keys` goes in slot `i` (0 is top left, 4 top right, 5 starts the second
  row), one cell in size. This is all most pages need.
- **Bigger keys:** `w` and `h` make a key wider or taller. A key without `x`/`y` still starts at its slot, so the
  slots it covers need `null` entries: `[{ "w": 2, ... }, null, ...]` is a 2-wide key over slots 0 and 1.
- **Anywhere:** `x` and `y` place a key freely (give both), so a page can have half-height keys, half-width keys
  or a small key off the cell lines: `{ "x": 0, "y": 1.5, "h": 0.5, ... }`.
- Keys must stay inside the grid and must not overlap. Up to 32 keys fit on a page.
- **Groups:** a key with its own `keys` list is a group: one picture split into several keys that each press on
  their own, such as a Volume - / + pair under one "Volume" label. The group sets the face and the shared label;
  its keys set their own icons and actions, placed relative to the group.

```js
{ w: 2, label: "Volume", labelVAlign: "bottom", color: "#2a9d8f", keys: [
    { icon: "#volume-down", iconOffset: { x: 0, y: -7 }, action: volume("-") },
    { x: 1, icon: "#volume-up", iconOffset: { x: 0, y: -7 }, action: volume("") },
] },
null,           // slot 3 is covered by the Volume group
```

Between any two keys, and around every key's edge, there's a narrow strip that only swipes, so you can start a
swipe anywhere without pressing a key by mistake.

[`examples/key-shapes.mjs`](../../examples/key-shapes.mjs) shows every shape on one page (a 2-wide key, a 2x2 key,
a tall key, a group, half keys and an off-grid key); render it to see the result. The rules are in
[Keys](../layout-format.md#keys).

### Key ids

The bake numbers keys for you: a key at a whole-cell position gets its slot's id, counting through the layout's
pages (Home's slots are ids 1 to 15, the first activity page's 16 to 30, and so on), and a key off the cell lines
gets the next free number after those. You rarely see ids, but they show up in bake errors and the device log.

Give a key an explicit `"id"` (1 to 65535) when its automatic id would clash with another key's, or when you want
an off-grid key's id to stay the same as you add pages. A [host app](./hosted-mode.md) refers to keys by id too.

## The look

Each key is drawn at bake time into a picture of exactly its size: a **face** (the key's rounded shape and
colour, with a black margin around it) and a **foreground** (an icon and a label). The device just shows the
pictures, normal and pressed.

| Property | What it does |
|---|---|
| `color` | The face colour (with a gentle top-to-bottom shade) |
| `face` | The face's shape (`"rounded"`, `"square"`, `"none"`) or its own settings (`top`, `bottom`, `border`, `radius`, `padding`) |
| `icon`, `iconColor`, `iconScale`, `iconOffset` | The icon, its colour, size and position |
| `label`, `labelColor`, `font`, `labelAlign`, `labelVAlign` | The text and how it's set |
| `pressed` | The changes for the pressed picture, such as `{ "color": "#3a86ff" }`. Without it, the pressed key is the normal one darkened |
| `image` | Your own layer definitions, replacing the generated face or foreground (see [Material Symbols](#material-symbols)) |

The full list, with defaults, is in [Look properties](../layout-format.md#look-properties).

**Defaults and inheritance.** Look properties set in `defaults` apply to every key below them: the root
`defaults` to the whole layout, `home.defaults` or an activity's `defaults` to its keys, a page's `defaults` to
that page. The nearest one wins, objects such as `pressed` merge, and `null` clears an inherited value. Actions
and labels are never inherited. See [Inheritance and defaults](../layout-format.md#inheritance-and-defaults).

**Flat faces.** A plain `color` has a slight shade, which can show visible bands on dark colours on the device's
screen. For a perfectly flat face, give the same top and bottom: `"face": { "top": "#22262e", "bottom": "#22262e" }`.
(The examples' `flatten()` helper in [`examples/test-common.mjs`](../../examples/test-common.mjs) does this for a
whole layout.)

### Icons

- **Built-in icons:** `"icon": "#name"`, one of button-face's set: `add`, `apple-tv`, `apps`, `arrow-down`,
  `arrow-left`, `arrow-right`, `arrow-up`, `cancel`, `cancel-circle`, `check`, `circle`, `cross`, `delete`,
  `diamond`, `forward`, `function`, `guide`, `hdd`, `heart`, `home`, `info`, `list`, `next-track`, `ok`, `option`,
  `pause`, `play`, `play-pause`, `playstation`, `power`, `previous-track`, `refresh`, `rewind`, `settings`,
  `square`, `star`, `steam`, `stop`, `triangle`, `volume-down`, `volume-mute`, `volume-up`. An unknown name is a
  bake error.
- **Your own SVG:** `"icon": "icons/logo.svg"`, a path relative to the layout file. The colour `#848484` in the SVG
  is replaced by `iconColor` (white by default), so draw recolourable parts in that grey.

### Material Symbols

Any of Google's [Material Symbols](https://fonts.google.com/icons) can be used by name with an `@` prefix, but
**only inside an `image` definition**, not in `icon` (there, `@lightbulb` would be read as a file name). An `image`
foreground replaces the generated one, label included, so put the label in the definition too, and give the
pressed picture the same foreground:

```js
const bulb = {
    layers: [
        { type: "svg", src: "@lightbulb", color: "white", translate: { y: -7 } },
        { type: "text", text: "Lights", font: "bold 11px sans-serif", valign: "bottom", padding: [9, 4] },
    ],
};

{ image: { normalFg: bulb, pressedFg: bulb }, action: { ... } }
```

The bake downloads each symbol once (it needs internet access the first time) and bakes it into the layout. The
layer format (`svg`, `text` and `fill` layers, sizes, colours, the `@name?style=...` options) is described in
[button-face's README](https://github.com/toptensoftware/button-face#readme).

## Gestures

A key's actions are keyed by gesture:

| Gesture | Fires |
|---|---|
| `press` | when the key goes down |
| `release` | when the finger lifts, and also if the touch turns into a swipe |
| `tap` | on lift, for a press shorter than the long-press time |
| `long` | once the key has been held for the long-press time |
| `action` | another name for `tap`, for the common case (use one or the other) |

A key goes down as soon as it's clear the touch isn't a swipe: when the finger lifts or after about 150 ms,
whichever is first. If a key has no `long`, its `tap` doesn't wait for the lift.

The long-press time is 500 ms unless you change it for the whole layout (`settings.longPressMs`) or for a key
(`longPressMs`, which inherits through `defaults`). There's no double press.

**Holding a key down on the PC.** `hidKey` with mode `hold` presses the chord on `press` and lets it go on
`release`, so it stays down for as long as your finger does:

```json
{ "label": "Shift", "press": { "op": "hidKey", "keys": "shift", "mode": "hold" },
  "release": { "op": "hidKey", "keys": "shift", "mode": "hold" } }
```

Because `release` also fires when a press becomes a swipe, a held key can't get stuck.

### Auto repeat

`repeat` makes a held key fire again and again, like a keyboard key:

```js
{ icon: "#volume-up", repeat: 150, action: ... }                            // every 150 ms while held
{ label: "Up", repeat: { initialDelay: 500, period: 100 }, action: ... }    // wait 500 ms, then every 100 ms
```

While the key is held, its `press` and `tap` fire at once, again after the initial delay, then every period; one
`release` fires when it lifts. A repeat is skipped while earlier ops are still waiting in the queue, so a slow
action never piles up and runs on after you let go. A repeating key can't also have a `long` gesture. Each
repeat passes `repeat=<n>` to scripts, so a script can take bigger steps the longer the key is held (see
[Scripts](#scripts)). The rules are in [Auto repeat](../layout-format.md#auto-repeat).

## Actions

### Op lists

A gesture's value is an **op list**: one op, an array of ops run in order, or `null`. To reuse a list, name it in
the root `actions` and refer to it with `use`:

```js
actions: {
    mute: { op: "script", file: "scripts/mute.lua" },
},
...
{ icon: "#volume-mute", label: "Mute", action: { use: "mute" } },
```

The table of ops and all their properties is in [Ops](../layout-format.md#ops). In short:

| Op | Does |
|---|---|
| `hidKey` | Types a key or chord on the PC: `"keys": "ctrl+shift+esc"`. Modes `tap` (default), `hold`, `down`, `up` |
| `httpGet`, `httpPost` | Sends an HTTP request (optional `headers`; `httpPost` has `body` and `contentType`). The reply is ignored |
| `udpPacket` | Sends a UDP packet: `host`, `port`, `data` (text, or `{ "hex": "DEADBEEF" }` for bytes) |
| `sendWol` | Sends a wake-on-LAN packet to a `mac` address |
| `delay` | Waits `ms` milliseconds (holding the queue) |
| `switchActivity` | Selects an activity by name, or `null` for none (see [Activities and devices](#activities-and-devices)) |
| `script` | Runs Lua (see [Scripts](#scripts)) |

### Keyboard keys

`hidKey` takes a chord: any modifiers (`ctrl`, `shift`, `alt`, `win`/`cmd`/`gui`, and right-hand variants such
as `rctrl` and `altgr`) plus at most one key: letters, digits, `f1` to `f24`, `enter`, `esc`, `tab`, `space`,
`backspace`, `delete`, the arrows (`up`, `down`, `left`, `right`), `home`, `end`, `pageup`, `pagedown` and the
punctuation keys (`minus`, `plus` for the `=` key, `comma`, `period`, `slash`, ...). The full list is in
[`host/src/bake/hid.js`](../../host/src/bake/hid.js). Media keys (volume, play/pause) aren't supported yet.

To type a sequence, list several `hidKey` ops, or use a script's `hid.key()`.

### Network actions

Network ops need Wi-Fi ([Getting started](./getting-started.md#wi-fi)). A worked example: a TV woken by
wake-on-LAN and switched off by a UDP command, a receiver with an HTTP interface, and a smart light that takes a
UDP packet:

```js
const TV = "192.168.1.50";
const AVR = "http://192.168.1.51";
const avr = path => ({ op: "httpGet", url: `${AVR}${path}` });

// Wake the TV, then give it 5 s to start before anything else runs
const tvOn = [{ op: "sendWol", mac: "AA:BB:CC:DD:EE:FF" }, { op: "delay", ms: 5000 }];
const tvOff = { op: "udpPacket", host: TV, port: 9761, data: "POWER OFF" };

// Keys
{ icon: "#power", label: "Receiver", action: avr("/power?on=1") },
{ label: "HDMI 1", action: { op: "httpPost", url: `${AVR}/input`, body: "src=hdmi1" } },
{ image: { normalFg: bulb, pressedFg: bulb },
  action: { op: "udpPacket", host: "192.168.1.60", port: 4210, data: { hex: "A1FF00" } } },
```

Things to know:

- **HTTP only**, not HTTPS. An `https://` URL is a bake error.
- Requests are **fire and forget**: the op doesn't wait for the reply, and the queue moves straight on. To use a
  reply, or to wait for one, use a script.
- Requests go out one at a time from their own small queue, which holds 8. If more pile up (a burst of taps on a
  slow server), the oldest unsent ones are dropped.
- A host can be an IP address or a name the device can resolve. Back-to-back HTTP requests to the same
  server reuse one connection.
- When a network request fails (Wi-Fi down, no connection, a timeout, or an HTTP error status), an error icon shows
  in the status bar for a few seconds, and the device log says what happened.

## Activities and devices

An activity has a name (its key in `activities`), pages, and optionally a `displayName` for the status bar, the
`devices` it uses, and hooks. Devices are defined once at the root, each with `turnOn` and `turnOff` op lists:

```js
devices: {
    tv: {
        turnOn: [{ op: "sendWol", mac: "AA:BB:CC:DD:EE:FF" }, { op: "delay", ms: 5000 }],
        turnOff: { op: "udpPacket", host: TV, port: 9761, data: "POWER OFF" },
    },
    receiver: { turnOn: avr("/power?on=1"), turnOff: avr("/power?on=0") },
},

activities: {
    Watch: {
        displayName: "Watch TV",
        devices: ["tv", "receiver"],
        hooks: { didActivate: { op: "httpPost", url: `${AVR}/input`, body: "src=hdmi1" } },
        pages: [ { keys: [ ... ] }, { keys: [ ... ] } ],
    },
    Listen: {
        displayName: "Music",
        devices: ["receiver"],
        hooks: { didActivate: { op: "httpPost", url: `${AVR}/input`, body: "src=net" } },
        pages: [ { keys: [ ... ] } ],
    },
},
```

A Home key selects an activity with `switchActivity`; there's no special kind of key for it:

```js
{ icon: "#play", label: "Watch TV", action: { op: "switchActivity", activity: "Watch" } },
{ icon: "#power", label: "All off", color: "#bc4749", action: { op: "switchActivity", activity: null } },
```

**What a switch does.** The pages change and the screen glides to the activity's first page at once, while the
switch's ops run through the queue in this order:

1. the old activity's `willDeactivate`, then the new one's `willActivate` hooks;
2. `turnOn` for each device the new activity needs that isn't on (in its `devices` order);
3. `turnOff` for each device the old activity used and the new one doesn't;
4. the new activity's `didActivate`, then the old one's `didDeactivate`.

From "Watch" to "Listen" above, the TV turns off and the receiver, used by both, is left alone. Switching to
`null` (no activity) turns off every device that's on and leaves only Home. Selecting the activity that's
already selected just glides back to its first page.

Things to know:

- These ops run **before** the rest of the list that switched, so in `[{ op: "switchActivity", activity: "Watch" },
  { op: "hidKey", keys: "f11" }]` the key is typed after the TV's `turnOn`, including its 5 s `delay`. Use a
  `delay` in `turnOn` to wait for slow equipment to warm up; touches wait in the queue meanwhile.
- The device doesn't know whether equipment is really on: it remembers what it last turned on or off. If they
  disagree (someone used the TV's own remote), selecting the activity again, or "All off" and then the activity,
  puts them back in step.
- Hooks and device lists can't contain `switchActivity`.
- All devices are assumed off after a boot, and no activity is selected.

The rules are in [Home and activities](../layout-format.md#home-and-activities) and
[Devices](../layout-format.md#devices).

## Scripts

Use a `script` op when an op list isn't enough: to read a reply and act on it, to make a decision, or to vary an
action with how long a key is held. Scripts are Lua 5.4, either inline (`"lua": "..."`) or in a file (`"file":
"scripts/mute.lua"`, relative to the layout file). The bake checks each script for syntax errors.

A mute toggle that asks the receiver for its state and sends the opposite:

```lua
-- scripts/mute.lua
local body, status = http.get("http://192.168.1.51/mute")
if not body then
    log("mute: no reply", status)
    return
end
local muted = body:find("mute=1") ~= nil
http.get{ url = "http://192.168.1.51/mute?set=" .. (muted and "0" or "1"), reply = false }
```

A volume step that grows while the key is held, using the `param` global that auto repeat sets to `repeat=<n>`
(inline in a `.mjs` layout, so the address comes from a constant):

```js
const volume = sign => ({ op: "script", lua: `
    local n = tonumber(param:match("repeat=(%d+)")) or 0
    local step = n < 10 and 1 or 3
    http.get{ url = "${AVR}/volume?step=${sign}" .. step, reply = false }` });

{ icon: "#volume-up", repeat: 150, action: volume("") },
```

The API mirrors the ops: `hid.key()`, `http.get()` (which can wait for the reply), `http.post()`, `udp.send()`,
`wol.send()`, `delay()`, `switchActivity()`, plus `log()` and `millis()`. Calls return `nil, err` on failure
rather than raising errors. The full list is in [Script API](../layout-format.md#script-api).

Limits to keep in mind:

- One script runs at a time, as one op in the queue: while it waits (for a reply, or in `delay()`), the ops after
  it wait too.
- A script that computes for 100 ms without waiting is stopped, and any run is stopped after 30 s. Memory is
  capped at 256 KB, and a waited reply is cut at 16 KB.
- No file, OS or module access (`io`, `os`, `require`), no HTTPS. Each script has its own globals, kept until the
  device is taken over or released.
- `log()` and errors go to the device log (see [Logs](#logs)).

[`examples/script-test.mjs`](../../examples/script-test.mjs) has many small scripts, and urem2's
[`scripts/`](../../urem2/scripts/) has two real ones.

## The status bar

The bar across the top shows:

- on the left, the selected activity's `displayName` (or its name), and nothing on Home with no activity
  selected. Keep it short: the bar shows up to 47 bytes, and its font has plain ASCII characters only;
- on the right, the clock, once the device knows the time ([Getting started](./getting-started.md#clock-and-time-zone));
- a pill for each page, the current one highlighted;
- small icons beside the clock: Wi-Fi, an hourglass while ops are running or requests are pending, and an
  error mark for a few seconds after a network request fails.

The layout can't put other text in the bar.

## When a host takes control

A [host app](./hosted-mode.md) can take over the screen at any time, even in the middle of a switch. The device
then stops running the layout: queued ops and scripts are cancelled, held keyboard keys are released, and the host
draws its own keys. Your layout stays stored.

When the host releases control, or the PC unplugs or sleeps the USB port, the device goes back to standalone
mode and shows Home, with no activity selected. (An app that just closes its connection leaves the device showing
its keys until the next app takes or releases control.) It still remembers which devices it turned on.

Uploading a layout with the CLI doesn't need (or take) control: you can upload while the layout runs.

## A complete example

The pieces above, put together: Home with two activity keys and "All off", a "Watch" activity of two pages
(media keys, a mute script, a Volume group with auto repeat, and a Material Symbol light key), and a "Music"
activity sharing the receiver. Save it as `guide-example.mjs`, and the mute script as `scripts/mute.lua` beside
it. The addresses and MAC are placeholders: change them to your equipment's.

```js
// guide-example.mjs
const TV = "192.168.1.50";
const AVR = "http://192.168.1.51";

const type = keys => ({ op: "hidKey", keys });
const avr = path => ({ op: "httpGet", url: `${AVR}${path}` });
const go = (icon, label, activity) => ({ icon, label, action: { op: "switchActivity", activity } });

// A Material Symbol needs an `image` definition (not `icon`); give the pressed look the same foreground
const bulb = {
    layers: [
        { type: "svg", src: "@lightbulb", color: "white", translate: { y: -7 } },
        { type: "text", text: "Lights", font: "bold 11px sans-serif", valign: "bottom", padding: [9, 4] },
    ],
};

// A volume step that grows the longer the key is held (param is "repeat=<n>")
const volume = sign => ({ op: "script", lua: `
    local n = tonumber(param:match("repeat=(%d+)")) or 0
    local step = n < 10 and 1 or 3
    http.get{ url = "${AVR}/volume?step=${sign}" .. step, reply = false }` });

export default {
    version: 1,
    defaults: { color: "#22262e", pressed: { color: "#3a86ff" } },

    devices: {
        tv: {
            turnOn: [{ op: "sendWol", mac: "AA:BB:CC:DD:EE:FF" }, { op: "delay", ms: 5000 }],
            turnOff: { op: "udpPacket", host: TV, port: 9761, data: "POWER OFF" },
        },
        receiver: { turnOn: avr("/power?on=1"), turnOff: avr("/power?on=0") },
    },

    actions: {
        mute: { op: "script", file: "scripts/mute.lua" },
    },

    home: {
        keys: [
            go("#play", "Watch TV", "Watch"),
            go("#list", "Music", "Listen"),
            null,
            null,
            { icon: "#power", label: "All off", color: "#bc4749",
              action: { op: "switchActivity", activity: null } },
        ],
    },

    activities: {
        Watch: {
            displayName: "Watch TV",
            devices: ["tv", "receiver"],
            hooks: { didActivate: { op: "httpPost", url: `${AVR}/input`, body: "src=hdmi1" } },
            pages: [
                { keys: [
                    { icon: "#play-pause", label: "Play", action: type("space") },
                    { icon: "#volume-mute", label: "Mute", action: { use: "mute" } },
                    { w: 2, label: "Volume", labelVAlign: "bottom", color: "#2a9d8f", keys: [
                        { icon: "#volume-down", iconOffset: { x: 0, y: -7 }, repeat: 150, action: volume("-") },
                        { x: 1, icon: "#volume-up", iconOffset: { x: 0, y: -7 }, repeat: 150, action: volume("") },
                    ] },
                    null,           // slot 3 is covered by the Volume group
                    { icon: "#arrow-left", label: "Back",
                      tap: type("esc"),
                      long: { op: "switchActivity", activity: null } },
                ] },
                { keys: [
                    { image: { normalFg: bulb, pressedFg: bulb },
                      action: { op: "udpPacket", host: "192.168.1.60", port: 4210, data: { hex: "A1FF00" } } },
                ] },
            ],
        },
        Listen: {
            displayName: "Music",
            devices: ["receiver"],
            hooks: { didActivate: { op: "httpPost", url: `${AVR}/input`, body: "src=net" } },
            pages: [
                { keys: [
                    { icon: "#play-pause", label: "Play", action: avr("/play") },
                    { icon: "#volume-mute", label: "Mute", action: { use: "mute" } },
                    { icon: "#power", label: "Off", color: "#bc4749",
                      action: { op: "switchActivity", activity: null } },
                ] },
            ],
        },
    },
};
```

`controlblock render guide-example.mjs` draws its four pages (Home, Watch's two, Music's one).

## More examples

| Layout | Shows |
|---|---|
| [`examples/upload-test.json`](../../examples/upload-test.json) | A few keyboard keys on Home, including a held key |
| [`examples/key-shapes.mjs`](../../examples/key-shapes.mjs) | Every key shape: spans, a group, half keys, an off-grid key with its own id |
| [`examples/net-test.mjs`](../../examples/net-test.mjs) | Every network op: GET with headers, form and JSON POST, UDP by address, name and hex, wake-on-LAN |
| [`examples/activity-test.mjs`](../../examples/activity-test.mjs) | Activities, devices and all four hooks, with long delays and many devices |
| [`examples/script-test.mjs`](../../examples/script-test.mjs) | Scripts: waited replies, timeouts, typing, switching from a script, auto repeat |
| [`urem2/`](../../urem2/README.md) | A complete home theatre remote: a TV, receiver, Apple TV, DVR and PC, built from modules |

The net, activity and script test layouts talk to a test server you run on your PC (see [The test server](#the-test-server)).

## Troubleshooting

### Bake errors

The bake lists every problem at once, each with its path in the layout, and writes nothing:

```
2 errors in the layout:
home.keys[0].lable: unknown property
home.keys[1].action.url: url must start with http:// (TLS is not supported yet)
```

A common one is a wide key without a `null` after it, so the next key lands on a slot the wide key already covers:

```
activities.Watch.pages[0].keys[3]: overlaps activities.Watch.pages[0].keys[2]
```

Warnings (an action nothing uses, a device no activity lists) are printed too but don't stop the bake.

### Home is empty

The device shows an empty Home (no keys) when it has no usable layout:

- **Nothing uploaded yet.** Upload one.
- **After a firmware update.** When an update changes the baked layout format, the device can't read a layout
  baked for the old firmware and ignores it. Bake and upload the layout again with the CLI release that matches
  the firmware (`npx github:toptensoftware/ControlBlock#cli-v<version>`; `controlblock info` shows the firmware's
  version). The CLI refuses to upload a blob the firmware can't load.
- **The layout is too big for the device.** The bake doesn't yet check everything the device does (for example
  the total number of devices or a very large layout), so a layout can bake fine and still be refused when the
  device loads it. Make it smaller (fewer pages or activities, fewer distinct key pictures).

The device log says which it was: `layout rejected (unsupported version): empty Home` for an old layout, `(over a
limit)` for one that's too big.

### Logs

- **Over USB:** the device logs to the UART port (see [Getting started](./getting-started.md#the-two-usb-ports)).
  With ESP-IDF installed, `idf.py -p COM4 monitor` in `firmware/` shows it; any serial terminal at 115200 baud
  works too. Script `log()` lines appear here, as do failed ops (`op failed: ...`) and each network request's
  result.
- **Over Wi-Fi:** the diagnostics page at `http://<name>.local/` shows the recent log, and
  `http://<name>.local/log` returns it as text ([`docs/diagnostics.md`](../diagnostics.md)).

### The test server

The layouts in `examples/` send their requests to a small test server
([`examples/test-server/test-server.mjs`](../../examples/test-server/test-server.mjs)) that prints everything it
receives, which is handy for checking network actions without real equipment:

```sh
node examples/test-server/test-server.mjs
```

It listens for HTTP on port 8000 and UDP on ports 9 and 9999; allow it through your PC's firewall. Tell the
example layouts where it is when you bake or upload them: `CB_TEST_SERVER` is your PC's IP address and
`CB_TEST_SERVER_NAME` a host name the device can resolve to it:

```sh
CB_TEST_SERVER=192.168.1.20 CB_TEST_SERVER_NAME=mypc node cli/cli.js upload examples/net-test.mjs
```

(In PowerShell, set `$env:CB_TEST_SERVER = "192.168.1.20"` first.) See [`examples/README.md`](../../examples/README.md).

### Other problems

- **A key does nothing on the network:** check Wi-Fi (`controlblock wifi status`) and the log; a failed
  request shows the error icon in the status bar.
- **Taps are slow after a switch:** a `delay` in a hook or `turnOn` holds the queue; touches run after it.
- **The CLI can't open the device:** another app has it open (see
  [Getting started](./getting-started.md#connecting-to-your-pc)).

## Limitations

- **HTTP only:** no HTTPS, for ops and scripts.
- **No double press.** Gestures are press, release, tap, long and auto repeat.
- **No media keys:** `hidKey` is a standard keyboard (no volume or play/pause keys).
- **The bake doesn't check every device limit**, so an oversized layout can bake and then be refused at boot
  (see [Home is empty](#home-is-empty)).
- **The status bar text** is the activity name only, ASCII, up to 47 bytes.
- **The backlight stays on.** Turning it off is disabled for now ([`docs/backlight.md`](../backlight.md)).
- **No clock after power-off without Wi-Fi:** the board has no battery-backed clock.
- **The selected activity isn't remembered** across a reboot: the device always starts on Home with nothing
  selected and assumes every device is off.
- **A macro pad** (physical keys and a rotary encoder, bound with an activity's `bindings` and `pad` map) is
  planned but not available yet. Its bindings bake and upload, but never fire, and the bake doesn't check their
  names. See [Bindings](../layout-format.md#bindings).
- **Layouts are written by hand** for now; a web editor is planned.
