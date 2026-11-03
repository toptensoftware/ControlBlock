# Config file format (user level)

The file a user writes (by hand now, from a web front end later) to define a standalone-mode device: Home, activities, pages, buttons, devices and their actions. The bake pipeline turns it into the binpack blob that is uploaded to the device. This document specifies the **input** of that pipeline. The packed blob's structs, the loader, the op queue and the runtime are in [`standalone.md`](standalone.md); that document stays the source of truth for them.

**Status:** the proposals below are confirmed and the bake tool in [`tools/bake/`](../tools/bake/) implements this format (`npm test`; `node cli.js config.json -o out.bin`, and `--upload` sends it to the device: [`config-upload-plan.md`](config-upload-plan.md)). A sample is `tools/bake/examples/upload-test.json`. Where the tool differs from, or settles, something in this document:

- `image` layers accept `.svg` paths and settings objects only (no `.png` yet).
- A built-in icon name (`#name`) that node-buttonImage doesn't have is an error. The built-in names are kebab-case (`#play`, `#apple-tv`, `#volume-mute`), not Material names.
- A `hidKey` chord is modifiers plus **one** key (the packed op has `mode`, `modifiers`, `key`); `hold` is accepted under `press`/`release` only, and in a hook or device list it's an error.
- Not checked yet (deliberately): device limits (pages, images, blob size, op list length) and the "icon larger than the icon area" warning.
- The default pressed look is the normal face with a 35% black overlay, unless `pressed` sets its own `color` or `face`.
- The packed structs are in `tools/bake/src/schema.js`; `--header types.h` writes the C declarations.

## Goals

- **Write what you mean, not what the device runs.** The file has names, inheritance, inline actions and SVG buttons. The pipeline's stage 1 turns that into the device's indices, binding lists and four images per key.
- **Plain data.** JSON, or a JS module that default-exports the same object (comments and shared constants are convenient there). No functions in the tree: everything must survive `JSON.stringify`, so a web front end can store and edit it, and the bake can run in a browser.
- **Fail early, with a path.** Every error names the JSON path (`activities[2].pages[0].keys[4].tap[1].url`) and the reason. Nothing reaches the device that the loader would reject.
- **Small by default.** A key with nothing but an icon and an action is a valid key. Repeated definitions cost one render and one stored image (see [Inheritance](#inheritance-and-defaults)).

## File basics

| | |
|---|---|
| Format | JSON (`.json`) or a JS module (`.js`/`.mjs`) whose default export is the object |
| Encoding | UTF-8; JSON is strict (no comments, no trailing commas); the JS form allows both |
| Paths | `svg`, `file` and similar are resolved relative to the **config file's directory**, not the working directory. A `#name` prefix on an `svg` value means a built-in node-buttonImage icon |
| Unknown properties | **Error** (typos like `longPres` must not pass silently). A property starting with `_` or `$` is ignored, for notes and the web front end's own data |
| Names | Activity, device and action names are case-sensitive strings, unique within their kind, and may not be empty |
| Order | Arrays keep their order; it matters for pages, keys and `bindings` (every matching binding is enqueued in config order) |

## Overview

```jsonc
{
  "version": 1,
  "defaults": { ... },          // inherited by every button (see Inheritance)
  "settings": { ... },          // device-wide constants
  "devices": { "tv": { ... }, "receiver": { ... } },
  "actions": { "mute": [ ... ] },   // named, reusable op lists (expanded host side)
  "home": { "keys": [ ... 15 ... ], "defaults": { ... } },
  "activities": {
    "Watch TV": {
      "displayName": "TV",
      "devices": ["tv", "receiver"],
      "defaults": { ... },
      "hooks": { "willActivate": [ ... ] },
      "pages": [ { "keys": [ ... ] } ],
      "bindings": [ ... ]       // macro pad, encoder, anything that isn't a touch key
    }
  }
}
```

Only `version` and `home` are required. A file with just those is a valid (if boring) device.

## Root

| Property | Type | Description |
|---|---|---|
| `version` | integer | Config **file** format version. Currently `1`. A different value is an error. (Not the packed blob's version, which the pipeline sets.) |
| `defaults` | [`ButtonDefaults`](#button-definition) | Root of the inheritance chain |
| `settings` | object | See [Settings](#settings) |
| `devices` | map name → [Device](#devices) | External equipment the activities power on and off |
| `actions` | map name → [OpList](#op-lists) | Named op lists, referenced with `{ "use": "name" }` |
| `home` | [Home](#home-and-activities) | The Home page. Required |
| `activities` | map name → [Activity](#home-and-activities) | An object (not an array) so the name is the key. Order is preserved and is the order the packed `activities[]` array uses |

### Settings

| Property | Default | Description |
|---|---|---|
| `longPressMs` | firmware constant (500, tuned on hardware) | The long-press time for every control. A button's own `longPressMs` overrides it |

More settings (HTTP timeout, Lua limits) are firmware constants today and become settings only if there's a reason to tune them per config. The Wi-Fi credentials are **not** in this file: they're set from the CLI and kept in NVS ([`standalone.md`](standalone.md#wi-fi)), so the file is safe to share and commit.

## Home and activities

```jsonc
"home": {
  "defaults": { ... },                 // optional; between root and key in the chain
  "keys": [ Button | null, ... ]       // up to 15, row-major; null = empty slot
}
"activities": {
  "<name>": {
    "displayName": "string",           // optional; the status bar shows this instead of the name
    "devices": ["tv"],                 // device names this activity uses
    "defaults": { ... },
    "hooks": {                         // each an OpList; all optional
      "willActivate": [], "didActivate": [], "willDeactivate": [], "didDeactivate": []
    },
    "pages": [ { "defaults": { ... }, "keys": [ ... ] }, ... ],
    "bindings": [ Binding, ... ]
  }
}
```

- **Keys are row-major** over the 5x3 grid: index 0 is the top-left key, 4 the top-right, 5 the second row's first. A `keys` array shorter than 15 is padded with empty slots; longer is an error. `null` is an empty slot (no images, and no bindings).
- A **page** is an object (not a bare array) so it can carry `defaults`.
- An activity needs at least one page. The number of pages is bounded by the firmware's limit (checked, see [Validation](#validation-and-limits)).
- **Home** has only keys. It has no hooks, devices, pages or bindings: Home's touch keys are the only thing it binds, and the macro pad and encoder always use the **current activity's** bindings, even while Home is in view ([`standalone.md`](standalone.md#triggers-and-bindings)).
- A Home key that selects an activity is an ordinary button whose action is `{ "op": "switchActivity", "activity": "Watch TV" }`; there is no special key type.
- `devices` entries must exist in the root `devices`; the order is irrelevant (the packed form stores indices).

## Devices

A device is external equipment with two op lists:

```jsonc
"devices": {
  "tv":       { "turnOn": [ { "op": "sendWol", "mac": "AA:BB:CC:DD:EE:FF" } ],
                "turnOff": { "op": "httpPost", "url": "http://tv.local/off" } },
  "receiver": { "turnOn": [ ... ], "turnOff": [ ... ] }
}
```

Both lists are optional (a missing list is empty). The firmware decides when to run them from the activities' `devices` lists; the switching order and the on/off tracking are in [`standalone.md`](standalone.md#devices-and-activity-switching).

## Button definition

A button has three parts: how it **looks**, what its **gestures do**, and (rarely) per-button timing.

```jsonc
{
  "icon": "#play",          // look: see below
  "label": "Play",
  "color": "#3a86ff",
  "pressed": { "color": "#1d4fa3" },

  "tap":   { "op": "httpPost", "url": "http://x/play" },     // gestures: see "Gestures"
  "long":  { "use": "stopAll" },
  "longPressMs": 800
}
```

### Look properties *(proposed)*

A key is drawn from **two layers** (the device draws them in order and swaps the pair when pressed; [`image-assets.md`](image-assets.md)):

- **Background ("face"):** the key's shape and fill. Opaque or with transparent padding at the edges.
- **Foreground:** the icon and label on a transparent background.

The look properties describe both layers; the pipeline renders four images per key: `normalBg`, `normalFg`, `pressedBg`, `pressedFg`.

| Property | Type | Layer | Description |
|---|---|---|---|
| `face` | `"rounded"` \| `"square"` \| `"none"` \| object | bg | The face shape. `"none"` is a transparent face (the icon sits on black). The object form gives the face's own settings (`top`, `bottom`, `border`, `radius`, `padding`), the demo's rounded gradient being the default |
| `color` | CSS color | bg | The face fill. Shorthand for `face: { top: c, bottom: darker(c) }` |
| `icon` | string | fg | An SVG file path, or `#name` for a built-in icon. Omit for a label-only key |
| `iconColor` | CSS color | fg | Replaces `#848484` in the SVG (`svgForeColor`). Default white |
| `iconScale` | number | fg | `svgScale` |
| `iconOffset` | `{x, y}` | fg | `svgTranslate`, in virtual pixels |
| `iconText` | string | fg | `svgText`: replaces `$(x)` in the SVG |
| `label` | string | fg | The text, drawn over the icon. `\n` for multiple lines |
| `labelColor` | CSS color | fg | `textColor`. Default white |
| `labelAlign`, `labelVAlign` | as node-buttonImage | fg | `textAlign`, `textVAlign`. `labelVAlign` defaults to `"bottom"` with an icon and `"center"` without |
| `font` | CSS font string | fg | e.g. `"bold 11px sans-serif"`. Defaults depend on whether there's an icon |
| `labelPadding`, `labelOffset`, `labelLineSpacing` | as node-buttonImage | fg | `textPadding`, `textTranslate`, `textLineSpacing` |
| `pressed` | object | both | Look properties **merged over** the above for the pressed pair. Omitted: a built-in pressed treatment *(open: darken the face, or shift the icon)* |
| `image` | object | either | Escape hatch: `{ normalBg, normalFg, pressedBg, pressedFg }`, each a raw node-buttonImage settings object (`svgFile`, `svgCode`, `text`, ...), an `.svg`/`.png` path, or `null` for none. Overrides the generated layer outright |

The sizes are virtual pixels (node-buttonImage's `virtualSize`, default 72) and scale to the rendered key size (128x128 cells, with the icon area about 80–96 px; the pipeline takes these from the same firmware constants that GET_INFO reports, never from this file).

Rules:

- A key with **no look properties and no inherited ones** has no images at all (all four absent: they pack as length 0 with a null pointer, which the compositor draws as nothing). That is valid.
- `pressed` only needs the differences. A property merged in `pressed` may be `null` to clear an inherited value.
- Colors are CSS color strings. Gradients and alpha are the face object's job.
- The pipeline renders each distinct resolved image once. Two buttons that resolve to the same `normalBg` (say, every key inheriting the root `color`) share one encoded Buffer and the blob stores one copy ([`standalone.md`](standalone.md#build-pipeline-and-sources)).

### Gestures

A button's actions are op lists keyed by the gesture that triggers them. These are exactly the events the device's `key_gesture` module emits for a touch key ([`standalone.md`](standalone.md#key-gestures)):

| Property | Fires | Notes |
|---|---|---|
| `press` | immediately on touch-down | |
| `release` | on lift, **and** when a press becomes a swipe | So a `hold`-mode HID key can never get stuck |
| `tap` | on lift, if shorter than the long-press time and `long` didn't fire | With no `long` set, fires on press (no added latency) |
| `long` | at the long-press time while still held | |
| `action` | alias for `tap` | The common case: `"action": [...]` reads better than `"tap"` for a plain button. Setting both is an error |
| `longPressMs` | (timing) | Per-button override of `settings.longPressMs` |

Each gesture's value is an [OpList](#op-lists). Double press is not supported yet.

## Op lists

An **op list** is the simplest possible sequence of ops, run in order on the device's global queue. Anything more complex goes in a `script` op ([`standalone.md`](standalone.md#actions)).

An op list is written as:

- one op object: `{ "op": "delay", "ms": 200 }`
- an array of op objects: `[ { ... }, { ... } ]`
- a reference to a named action: `{ "use": "mute" }`, anywhere an op is allowed (so it can sit in an array next to other ops)
- `null` or `[]`: nothing

`use` is expanded in stage 1 and recursively (an action may use other actions). A cycle is an error, naming the chain. The packed blob has everything inlined; the device has no `callAction`. Named actions take no arguments.

### Ops

Every op has an `op` code. Properties beyond those listed are errors.

| `op` | Properties | Behaviour |
|---|---|---|
| `delay` | `ms` (int, 1..60000) | Pauses the queue |
| `switchActivity` | `activity` (name) | Selects the activity (switching ops, page swap and glide). Name resolved to an index |
| `hidKey` | `keys` (string), `mode` (`"tap"` \| `"hold"` \| `"down"` \| `"up"`, default `"tap"`) | USB HID keyboard. `keys` is a chord: `"a"`, `"ctrl+c"`, `"ctrl+shift+esc"`, `"f5"`, `"enter"`. Names are checked and turned into usage codes in stage 1. `hold` is key down on `press`, key up on `release` (it only makes sense under `press`/`release`; elsewhere it's an error). *(Consumer/media keys: name the set when HID is built.)* |
| `httpGet` | `url`, `headers`? | Fire and forget at the op level (the response is discarded). A script is needed to use the reply |
| `httpPost` | `url`, `body`? (string), `contentType`? (default `application/x-www-form-urlencoded`), `headers`? | |
| `udpPacket` | `host`, `port` (1..65535), `data` | `data` is a string, or `{ "hex": "DEADBEEF" }` for bytes |
| `sendWol` | `mac` (`AA:BB:CC:DD:EE:FF`, `-` also accepted), `broadcast`? | Wake-on-LAN magic packet |
| `script` | exactly one of `lua` (source string) or `file` (path to a `.lua`) | See [Scripts](#scripts) |

`headers` is an object of string values. URLs must be `http://` (TLS isn't planned; `https://` is an error until it is). The planned set mirrors `standalone.md`; adding an op means adding it there and here together.

### Scripts

```jsonc
{ "op": "script", "lua": "http.post('http://avr/vol', 'delta=' .. (tonumber(param) // 10))" }
{ "op": "script", "file": "scripts/mute.lua" }
```

- `file` is read at bake time and its **source** goes into the blob (the device stores Lua source, not bytecode).
- The script receives the trigger's `param` string as a global. The API is `http.get`/`http.post`, `udp`, `hid`, `delay`, `switchActivity` ([`standalone.md`](standalone.md#actions); the surface is still being decided).
- Stage 1 compiles each script with a Lua 5.4 compiler and fails on a syntax error, naming the path and the Lua error line. Identical sources are stored once.
- Only `script` ops use `param`. Other ops ignore it.

## Bindings

A button's gestures cover touch keys. Everything else (the 6-key macro pad, the rotary encoder, and later whatever else the second MCU reports) is bound with a `bindings` list on the **activity**:

```jsonc
"bindings": [
  { "control": "macro1", "event": "tap",  "do": { "use": "mute" } },
  { "control": "macro1", "event": "long", "do": [ { "op": "switchActivity", "activity": "Listen" } ] },
  { "control": "volEnc", "event": "turnLeft",  "do": { "op": "script", "file": "scripts/vol.lua" } },
  { "control": "volEnc", "event": "turnRight", "do": { "op": "script", "file": "scripts/vol.lua" } }
]
```

| Property | Description |
|---|---|
| `control` | The control's name as the device reports it. Free-form (the device matches exact strings): `macro1`..`macro6`, `volEnc` *(proposed names; the serial format from the second MCU is undecided)* |
| `event` | `press`, `release`, `tap`, `long` (the device synthesizes `tap`/`long` from raw `press`/`release`), or the control's own events (`turnLeft`, `turnRight`: undecided) |
| `do` | An [OpList](#op-lists) |

- Matching is exact on both strings. **No wildcards**; the authoring layer expands any patterns, and this file has none yet.
- A `control` that looks like a touch key (`touch[...]`) is an error here: touch keys are defined by the buttons themselves, and the pipeline generates their bindings.
- Several bindings can match one trigger; all are enqueued in file order.
- Home has no `bindings`. With no activity selected, the pad and encoder do nothing.
- The encoder's velocity arrives as the trigger's `param` string for a script to read.

## Inheritance and defaults

Look properties and `longPressMs` inherit down a fixed chain *(proposed; not yet confirmed in `standalone.md`)*:

```
root.defaults  →  (home | activity).defaults  →  page.defaults  →  button
```

(Home's chain is `root.defaults → home.defaults → button`.)

- **Merge rule:** property by property, nearest wins. Object-valued properties (`pressed`, `face`, `image`, `iconOffset`) merge **recursively**, so `"pressed": { "color": ... }` on a key keeps an inherited `pressed.iconColor`. An array or scalar replaces. An explicit `null` clears an inherited value (so a key can opt out of an inherited `icon`).
- **Only look properties and `longPressMs` inherit.** Gestures (`tap`, `long`, `press`, ...) and `label` do not: inheriting an action or a label across buttons is almost never intended. (A default `label` can be set explicitly where wanted.)
- `defaults` takes the same look properties as a button, minus gestures and `label`.
- **Empty slots** (`null`) never inherit anything.
- A button that has nothing of its own *and* nothing inherited is the all-absent key above; one with inherited backgrounds but no icon gets the inherited face only.

## What the pipeline produces

Stage 1 of the pipeline ([`standalone.md`](standalone.md#build-pipeline-and-sources)) maps this file onto the packed schema:

| In the file | In the blob |
|---|---|
| `home.keys[i]` | `home.keys[i]`: four image pointers + `longPressMs` |
| `home.keys[i]` gestures | `home.bindings[]`: `{ control: "touch[c,r]", event, ops }`, with `c = i % 5`, `r = floor(i / 5)` |
| `activities` map | `activities[]` in file order; `switchActivity` names → indices |
| `pages[p].keys[i]` gestures | the activity's `bindings[]`, with `control: "touch[5*(p+1) + i%5, floor(i/5)]"`: **the column index runs across the page strip** and counts Home as page 0, so the activity's first page starts at column 5 |
| `activity.devices` | `devices: int[]` (indices into the root `devices` order) |
| `activity.bindings[]` | appended to the activity's `bindings[]` after the generated touch ones |
| `devices.<name>` | `devices[]` in file order |
| `use` references | expanded inline |
| `script` ops | Lua source strings (deduplicated), already compile-checked |
| `hidKey.keys` | HID usage codes + modifier bits |
| look properties | four rle565 Buffers per key (one shared Buffer per distinct encoding) |

The generated binding order is: touch bindings in page, then key, then gesture order (`press`, `release`, `tap`, `long`), followed by the activity's own `bindings`. A button's `action` becomes `tap`.

Stage 1 is pure and synchronous and its output (a plain object tree) is the unit-test surface. Stage 2 renders the images and stage 3 packs.

## Validation and limits

Stage 1 collects **all** errors it can and reports them together, each as `path: message`. Warnings don't stop the build.

Errors:

- Unknown property, wrong type, out-of-range number; unsupported `version`.
- Duplicate or empty names; `devices` entry, `switchActivity` target or `use` that doesn't exist; a `use` cycle.
- More than 15 keys on a page; an activity with no pages; more pages than the firmware allows.
- `hold` outside `press`/`release`; both `action` and `tap`; `hidKey` with an unknown key name; bad MAC, port or URL scheme.
- A `script` with both or neither of `lua` and `file`; a missing file; a Lua syntax error.
- A missing icon/SVG file, or a render that fails.
- A `control` of the form `touch[...]` in `bindings`.
- Resolved config exceeds a device limit: images or pages (the PSRAM image store and the three page buffers are the real limit), blob size (the partition), op list length. Limits are read from the firmware's published constants (the same source as GET_INFO), so they stay in one place.

Warnings:

- A `long` op list on a button whose `tap` is absent (legal, just unusual).
- A device listed by no activity; an action that nothing uses.
- Two buttons that resolve to identical images *and* actions (a likely copy-paste), at most once per page.
- An `icon` larger than the icon area will be clipped.

## Example

```jsonc
{
  "version": 1,
  "settings": { "longPressMs": 600 },

  "defaults": {
    "color": "#22262e",
    "iconColor": "white",
    "pressed": { "color": "#3a86ff" }
  },

  "devices": {
    "tv":       { "turnOn": { "op": "sendWol", "mac": "AA:BB:CC:DD:EE:FF" },
                  "turnOff": { "op": "httpPost", "url": "http://tv.local/api/power", "body": "state=off" } },
    "receiver": { "turnOn":  { "op": "httpPost", "url": "http://avr.local/power", "body": "on=1" },
                  "turnOff": { "op": "httpPost", "url": "http://avr.local/power", "body": "on=0" } }
  },

  "actions": {
    "mute": { "op": "script", "file": "scripts/mute.lua" },
    "back": [ { "op": "hidKey", "keys": "esc" } ]
  },

  "home": {
    "keys": [
      { "icon": "#apple-tv", "label": "Watch TV", "color": "#8338ec",
        "action": { "op": "switchActivity", "activity": "Watch TV" } },
      { "icon": "#list", "label": "Listen",
        "action": { "op": "switchActivity", "activity": "Listen" } }
    ]
  },

  "activities": {
    "Watch TV": {
      "displayName": "TV",
      "devices": ["tv", "receiver"],
      "hooks": { "didActivate": { "op": "httpPost", "url": "http://avr.local/input", "body": "src=hdmi1" } },
      "pages": [
        { "keys": [
            { "icon": "#play",  "label": "Play",  "action": { "op": "hidKey", "keys": "space" } },
            { "icon": "#next-track",   "label": "Next",  "action": { "op": "hidKey", "keys": "ctrl+right" } },
            null, null,
            { "icon": "#volume-mute",  "label": "Mute",  "action": { "use": "mute" } },
            { "icon": "#arrow-left",  "action": { "use": "back" },
              "long": { "op": "switchActivity", "activity": "Listen" } }
        ] }
      ],
      "bindings": [
        { "control": "macro1", "event": "tap", "do": { "use": "mute" } },
        { "control": "volEnc", "event": "turnLeft",  "do": { "op": "script", "file": "scripts/vol.lua" } },
        { "control": "volEnc", "event": "turnRight", "do": { "op": "script", "file": "scripts/vol.lua" } }
      ]
    },
    "Listen": {
      "devices": ["receiver"],
      "pages": [ { "keys": [ { "icon": "#play", "action": { "op": "hidKey", "keys": "space" } } ] } ]
    }
  }
}
```

In "Watch TV"'s first page, `Mute` is at index 4 (column 4, row 0), so on the page strip (Home is page 0) its generated binding is `touch[9,0]` / `tap`. `back` is at index 5 (column 0, row 1), giving `touch[5,1]` / `tap` and `touch[5,1]` / `long`.

## Testing

Unit tested like `tools/bake/` (Node's test runner; stage 1 needs no device, no `sharp`):

- Valid configs: the example above, a minimal one (`version` + empty `home`), and a maximal one (15 keys on every page).
- Resolution: the defaults chain (each level overrides, recursive merge, `null` clears, gestures and labels don't inherit); name → index; `use` expansion (nested, cycle error); touch control names across the page strip (Home columns 0..4, page 1 from 5); `action` → `tap`; generated binding order.
- Validation: each error and warning above, with the exact path and message; several errors reported at once.
- Scripts: syntax error is reported; `file` and `lua` forms; identical sources deduplicated.
- Images: through an injected fake renderer, the same resolved look gives the same Buffer object (and so one stored copy); different looks give different ones; an absent look gives none.
- Round trip: the resolved tree packed with binpack and read back by a (test) BPAK loader gives the expected structs.

The browser run of the pipeline (`sharp`, rle565 encoder, Lua checker) is untested; see "Browser support" in [`standalone.md`](standalone.md#still-open).

## Open points

- **Look properties** (above) are a proposal and need to be checked against what node-buttonImage and the demo's face SVG can produce at 128x128; in particular the exact split into background and foreground, and the default pressed look.
- **The defaults chain** (root → activity/home → page → key) is proposed and needs a decision.
- **Control and event names** for the pad and encoder wait on the second MCU's serial format.
- **HID key names** (including consumer/media keys) are decided when HID is built.
- **Script API** is still being decided; the `param` global and the module names here follow `standalone.md` but are not final.
- **Named actions with arguments** are out of scope (a script reads `param` instead).
- **Wildcard or templated bindings** (e.g. one binding for `macro1..6`) are the authoring layer's job and are not in this format.
- **Multiple files:** `$include` for splitting a big config isn't specified. Reusable pieces are `actions`, `defaults` and, in the JS form, ordinary imports.
- **Web front end:** the editor probably wants stable ids on buttons and pages to keep diffs sensible; `_`-prefixed properties are reserved for it.
- **`CLAUDE.md` (a rules file, so only with your OK):** link this document from the Repository Layout's `docs/` list.
