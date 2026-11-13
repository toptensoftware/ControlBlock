# Layout file format (user level)

The file a user writes (by hand now, from a web front end later) to define a standalone-mode device: Home, activities, pages, buttons, devices and their actions. The bake pipeline turns it into the binpack blob that is uploaded to the device. This document specifies the **input** of that pipeline. The packed blob's structs, the loader, the op queue and the runtime are in [`standalone.md`](standalone.md); that document stays the source of truth for them.

**Status:** the proposals below are confirmed and the bake code in the host library ([`host/src/bake/`](../host/src/bake/)) implements this format. Bake a layout with `node cli/cli.js bake layout.json -o out.bin` from the repo root; `node cli/cli.js upload layout.json` sends it to the device ([`cli/README.md`](../cli/README.md), [`protocol.md`](protocol.md#layout-upload)). A sample is `examples/upload-test.json`. Where the tool differs from, or settles, something in this document:

- `image` layers accept `.svg` paths and settings objects only (no `.png` yet).
- A built-in icon name (`#name`) that node-buttonImage doesn't have is an error. The built-in names are kebab-case (`#play`, `#apple-tv`, `#volume-mute`), not Material names.
- A `hidKey` chord is modifiers plus **one** key (the packed op has `mode`, `modifiers`, `key`); `hold` is accepted under `press`/`release` only, and in a hook or device list it's an error.
- Not checked yet (deliberately): device limits (pages, images, blob size, op list length) and the "icon larger than the icon area" warning.
- The default pressed look is the normal face with a 35% black overlay, unless `pressed` sets its own `color` or `face`.
- Keys are rects on the 5x3 grid with ids, spans and groups (see [Keys](#keys)); since 2026-11-12 (blob version 2) each key state is baked into one opaque image. A layout baked before that is refused by the device (empty Home) until it's baked and uploaded again.
- Auto repeat (see [Auto repeat](#auto-repeat)) came with blob version 3 (2026-11-13): a layout baked before it is refused the same way.
- The packed structs are in `host/src/bake/schema.js`; `node tools/codegen/gen-types.js` writes their C declarations (`firmware/main/layout_types.gen.h`).

## Goals

- **Write what you mean, not what the device runs.** The file has names, inheritance, inline actions and SVG buttons. The pipeline's stage 1 turns that into the device's indices, key rects and ids, binding lists and two images per key.
- **Plain data.** JSON, or a JS module that default-exports the same object (comments and shared constants are convenient there). No functions in the tree: everything must survive `JSON.stringify`, so a web front end can store and edit it, and the bake can run in a browser.
- **Fail early, with a path.** Every error names the JSON path (`activities[2].pages[0].keys[4].tap[1].url`) and the reason. Nothing reaches the device that the loader would reject.
- **Small by default.** A key with nothing but an icon and an action is a valid key. Repeated definitions cost one render and one stored image (see [Inheritance](#inheritance-and-defaults)).

## File basics

| | |
|---|---|
| Format | JSON (`.json`) or a JS module (`.js`/`.mjs`) whose default export is the object |
| Encoding | UTF-8; JSON is strict (no comments, no trailing commas); the JS form allows both |
| Paths | `svg`, `file` and similar are resolved relative to the **layout file's directory**, not the working directory. A `#name` prefix on an `svg` value means a built-in node-buttonImage icon |
| Unknown properties | **Error** (typos like `longPres` must not pass silently). A property starting with `_` or `$` is ignored, for notes and the web front end's own data |
| Names | Activity, device and action names are case-sensitive strings, unique within their kind, and may not be empty |
| Order | Arrays keep their order; it matters for pages, keys and `bindings` (every matching binding is enqueued in layout order) |

## Overview

```jsonc
{
  "version": 1,
  "defaults": { ... },          // inherited by every button (see Inheritance)
  "settings": { ... },          // device-wide constants
  "devices": { "tv": { ... }, "receiver": { ... } },
  "actions": { "mute": [ ... ] },   // named, reusable op lists (expanded host side)
  "home": { "keys": [ ... ], "defaults": { ... } },
  "activities": {
    "Watch TV": {
      "displayName": "TV",
      "devices": ["tv", "receiver"],
      "defaults": { ... },
      "hooks": { "willActivate": [ ... ] },
      "pages": [ { "keys": [ ... ] } ],
      "bindings": [ ... ],      // macro pad, encoder, anything that isn't a touch key
      "pad": { "key3": { "repeat": 100 } }   // macro pad keys' timing (see Auto repeat)
    }
  }
}
```

Only `version` and `home` are required. A file with just those is a valid (if boring) device.

## Root

| Property | Type | Description |
|---|---|---|
| `version` | integer | Layout **file** format version. Currently `1`. A different value is an error. (Not the packed blob's version, which the pipeline sets.) |
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

More settings (HTTP timeout, Lua limits) are firmware constants today and become settings only if there's a reason to tune them per layout. The Wi-Fi credentials are **not** in this file: they're set from the CLI and kept in NVS ([`standalone.md`](standalone.md#wi-fi)), so the file is safe to share and commit.

## Home and activities

```jsonc
"home": {
  "defaults": { ... },                 // optional; between root and key in the chain
  "keys": [ Button | Group | null, ... ]   // see Keys; null = empty slot
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
    "bindings": [ Binding, ... ],
    "pad": { "key1": { "repeat": ..., "longPressMs": ... }, ... }   // optional; see Auto repeat
  }
}
```

- **Keys** are listed in a page's `keys` array: by default entry `i` is a 1U key in slot `i` of the 5x3 grid (row-major), and a key can also be placed and sized freely. See [Keys](#keys).
- A **page** is an object (not a bare array) so it can carry `defaults`.
- An activity needs at least one page. The number of pages is bounded by the firmware's limit (checked, see [Validation](#validation-and-limits)).
- **Home** has only keys. It has no hooks, devices, pages or bindings: Home's touch keys are the only thing it binds, and the macro pad and encoder always use the **current activity's** bindings, even while Home is in view ([`standalone.md`](standalone.md#triggers-and-bindings)).
- A Home key that selects an activity is an ordinary button whose action is `{ "op": "switchActivity", "activity": "Watch TV" }`; there is no special key type.
- `devices` entries must exist in the root `devices`. The packed form stores indices, and the **order matters at run time**: on a switch, `turnOn` lists run in the new activity's `devices` order and `turnOff` lists in the old activity's `devices` order.
- Hooks (and device `turnOn`/`turnOff`) may not contain `switchActivity`. Delays between ops are the author's job: put a `delay` op in the list (a `delay` holds the whole queue, so touches queue behind a TV warm-up). A switch's hooks and device lists run straight after its `switchActivity` op, before the rest of the list that switched, so a `delay` in a hook or `turnOn` also holds back the ops after `switchActivity` in that list.

## Keys

The page area holds a grid of 5x3 **cells**, 1U each (144x144 px on the device, packed edge to edge). A key is a rect on that grid: by default one cell, but it can be wider, taller, smaller, or sit off the whole-unit lines. Positions and sizes are in **grid units** (1U = one cell), in steps of **0.25U** (36 px).

| Property | Type | Description |
|---|---|---|
| `x`, `y` | number (multiple of 0.25, ≥ 0) | The key's top-left corner, from the grid's top-left. Give both or neither |
| `w`, `h` | number (multiple of 0.25, > 0) | Width and height. Default 1 |
| `id` | integer 1..65535 | The key's id (optional, see below) |

- **Placement:** entry `i` of a page's `keys` array goes in **slot `i`** (row-major: 0 top-left, 4 top-right, 5 the second row's first), 1U, unless it gives `x` and `y`. Without `x`/`y` a bigger key starts at its slot, so `[{ "w": 2 }, null, ...]` is a 2U key over slots 0 and 1 (entry 1 is `null` because that slot is taken). Entries past index 14 have no slot and need `x`/`y`. `null` is no key.
- **Rects** must lie inside the 5x3 grid and must not overlap (keys that only touch are fine). Any part of the grid no key covers is black and only swipes.
- **Up to 32 keys a page** (a group's keys each count).
- **Ids** are unique across the **whole layout** (Home and every activity's pages). They name the key in its bindings (`touch[id]`, generated) and in the device's key events. By default:
  - a key whose top-left corner is on a whole unit gets that slot's **plain grid id**, `g × 15 + slot + 1`, where `g` numbers the pages across the whole layout (Home 0, then each activity's pages in file order). So a plain 5x3 page keeps the same ids as before keys could move;
  - any other key (corner off the whole-unit lines) gets the lowest free id above `pages × 15`, in layout order. That id changes if pages are added: name an `id` for a stable one.
  - Two keys with the same id (named or default) are an error at the later one: give one of them another `id`.
- **Groups:** an entry with a `keys` array of its own is a **group**: one picture split into several keys, each pressing alone (Volume `-`/`+` with one "Volume" label across both). The group has `x`, `y` (or its slot), `w`, `h` and look properties: its face, and its own `icon`/`label`, are drawn once across the whole group. Each of its keys has its own `id`, gestures, `longPressMs`, `repeat`, `icon` and `label`, at `x`/`y` (default 0 each) and `w`/`h` (default 1) **relative to the group**. Rules:
  - The keys inherit the chain plus the group's look, but not its `icon`, `iconText`, `label` or `image`.
  - They can't set `face`, `color`, `image` or `pressed.face`/`pressed.color` (an error: set it on the group).
  - A group has no gestures and no `id` (unknown property), and groups don't nest.
  - Its keys must lie inside the group and not overlap each other. If they don't cover it all, a warning (that part shows black).
  - Pressing a key shows the group's pressed face (by default the overlay) and the key's pressed icon/label on that key only.
- **The look scales to the key:** the face fills the key's rect less an 8 px black margin each side (128x128 on a 1U key), and the icon and label keep their 1U size in virtual pixels (text doesn't stretch on a wide key; the icon follows the face's height). See [Look properties](#look-properties-proposed).

`examples/key-shapes.mjs` (Home only; each key types a different HID key, so a text editor shows which one fired):

```
+-----------+-----+-----+-----+
|   Wide    | 1U  |  -  Vol  +|     row 0 (Volume: one picture, two keys)
+-----+-----+-----+--+--+-----+
|  A  |           |L |R |     |     row 1 (A over B: half height; L, R: half width)
|  B  |    Big    |  |  |     |
+-----+           +--+--+Tall |
| Esc |           | (q) |     |     row 2 (q: 0.5U square, 0.25U in from the cell's corner)
+-----+-----------+-----+-----+
```

```jsonc
"keys": [
  { "w": 2, "label": "Wide (w)", "tap": { "op": "hidKey", "keys": "w" } },     // slot 0, 2U: id 1
  null,                                                                       // slot 1 is taken
  { "label": "1U (1)", "tap": { "op": "hidKey", "keys": "1" } },               // slot 2: id 3
  { "w": 2, "label": "Volume", "labelVAlign": "bottom", "color": "#2a9d8f",    // a group over slots 3 and 4
    "keys": [
      { "icon": "#volume-down", "iconOffset": { "x": 0, "y": -7 }, "tap": { "op": "hidKey", "keys": "minus" } },          // id 4
      { "x": 1, "icon": "#volume-up", "iconOffset": { "x": 0, "y": -7 }, "tap": { "op": "hidKey", "keys": "plus" } }     // id 5
    ] },
  { "x": 0, "y": 1, "h": 0.5, "label": "A", "tap": { "op": "hidKey", "keys": "a" } },                     // id 6
  { "x": 0, "y": 1.5, "h": 0.5, "label": "B", "tap": { "op": "hidKey", "keys": "b" } },                   // off the unit lines: id 16
  { "x": 1, "y": 1, "w": 2, "h": 2, "label": "Big (g)", "color": "#8338ec", "tap": { "op": "hidKey", "keys": "g" } },  // id 7
  { "x": 3, "y": 1, "w": 0.5, "label": "L", "tap": { "op": "hidKey", "keys": "left" } },                 // id 9
  { "x": 3.5, "y": 1, "w": 0.5, "label": "R", "tap": { "op": "hidKey", "keys": "right" } },              // id 17
  { "x": 4, "y": 1, "h": 2, "label": "Tall (t)", "color": "#e76f51", "tap": { "op": "hidKey", "keys": "t" } },        // id 10
  { "x": 0, "y": 2, "label": "Esc", "tap": { "op": "hidKey", "keys": "esc" } },                          // id 11
  { "id": 500, "x": 3.25, "y": 2.25, "w": 0.5, "h": 0.5, "label": "q", "tap": { "op": "hidKey", "keys": "q" } }       // named id
]
```

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

A key's look has **two layers**:

- **Background ("face"):** the key's shape and fill, inside an 8 px black margin.
- **Foreground:** the icon and label.

The look properties describe both layers. The pipeline renders them and composes them over black into **one opaque image per state**, exactly the key's size (144x144 for 1U, 288x144 for 2U): `normal` and `pressed`. The device shows the pressed one while the key is held ([`image-assets.md`](image-assets.md)).

| Property | Type | Layer | Description |
|---|---|---|---|
| `face` | `"rounded"` \| `"square"` \| `"none"` \| object | bg | The face shape. `"none"` is no face (the icon sits on black). The object form gives the face's own settings (`top`, `bottom`, `border`, `radius`, `padding`), the demo's rounded gradient being the default |
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
| `pressed` | object | both | Look properties **merged over** the above for the pressed image. Omitted: the normal face with a 35% black overlay |
| `image` | object | either | Escape hatch: `{ normalBg, normalFg, pressedBg, pressedFg }`, each a raw node-buttonImage settings object (`svgFile`, `svgCode`, `text`, ...), an `.svg` path, or `null` for none. Overrides the generated layer outright (the layers are still composed into one image per state) |

The sizes are virtual pixels (node-buttonImage's `virtualSize`: 72 across a 128 px face, the face of a 1U key, with the icon area about 80 px). On other key sizes the virtual size scales with the face (72 per 128 px each way), so text, padding and offsets keep their 1U size; the icon's size follows the face's height. The pixel sizes come from the same firmware constants that GET_INFO reports (`host/src/bake/consts.js`, checked against `key_geometry.h`), never from this file.

Rules:

- A key with **no look properties and no inherited ones** has no images at all (both absent: they pack as length 0 with a null pointer, and the device draws the key black). That is valid: it's still a touchable key.
- `pressed` only needs the differences. A property merged in `pressed` may be `null` to clear an inherited value.
- Colors are CSS color strings. Gradients and alpha are the face object's job.
- The pipeline renders each distinct layer once and each distinct image once. Two keys of the same size that resolve to the same look share one encoded Buffer and the blob stores one copy ([`standalone.md`](standalone.md#build-pipeline-and-sources)). Since faces are baked into every image, keys that differ only in their icon or label no longer share anything.

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
| `repeat` | (timing) | Auto repeat while held: see [Auto repeat](#auto-repeat) |

Each gesture's value is an [OpList](#op-lists). Double press is not supported yet.

### Auto repeat

A held key can repeat its action, on touch keys (`repeat` on a button or a group's key) and macro pad keys (an activity's `pad` map):

```jsonc
{ "icon": "#forward", "repeat": 100, "tap": { "op": "udpPacket", ... } }       // first repeat at 100 ms, then every 100 ms
{ "label": "Up", "repeat": { "initialDelay": 500, "period": 100 }, "tap": [ ... ] }

"activities": { "Watch TV": {
  "bindings": [ { "control": "key3", "event": "press", "do": { "op": "script", "file": "scripts/vol.lua" } } ],
  "pad": { "key3": { "repeat": { "initialDelay": 500, "period": 100 } }, "key4": { "longPressMs": 800 } }
} }
```

| Form | Meaning |
|---|---|
| `repeat: <ms>` | The period, and the initial delay too (as urem) |
| `repeat: { initialDelay?, period }` | `initialDelay` defaults to `period` |

- Both are integers, 40..10000 ms (40 is about two frames of the device's tick).
- While held, the key fires `press` and `tap` at the press, again after the initial delay, then every period. Lifting it fires one `release`. Bind the action to `tap` (or `action`) or `press`; both repeat.
- Each repeat's `param` is `repeat=<n>`: 0 at the press, then 1, 2, ... counting the periods held, so a script can take bigger steps the longer the key is held (`tonumber(param:match("repeat=(%d+)"))`). `release` has no param.
- A repeat is skipped while earlier ops are still waiting in the queue, so a slow action doesn't pile up and run on after the key is lifted; `n` still counts time, not repeats.
- On a touch key the first repeat waits for the swipe window (150 ms) even if the delay is shorter, so a swipe that starts on the key runs its action once. Pad keys repeat exactly on time.
- **Not with long press:** a repeating key can't have a `long` gesture (or, on a pad key, a `long` binding) or its own `longPressMs` (errors); an inherited `longPressMs` is ignored. A `hidKey` op with mode `hold` under the `press` of a repeating key is an error too (each repeat would press the key again; the PC repeats a held key itself): use mode `tap`.
- `repeat` is a behaviour, not a look property: it doesn't inherit through `defaults` (an error there), and a group can't have one (set it on the group's keys).
- **`pad`** (on an activity): a map from macro pad key names `key1`..`key32` to `{ repeat?, longPressMs? }` (same forms and ranges as on a button). It gives that activity's bindings for the key their timing. A `pad` entry for a key the activity doesn't bind is a warning. Encoders have no timing.
- Repeats run only in standalone mode; a host gets plain press and release.

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
| `switchActivity` | `activity` (name, or `null`) | Selects the activity (switching ops, page swap and glide). Its switching ops (hooks, device `turnOn`/`turnOff`) run straight after it, before the rest of its list, so ops after it run once the devices are on. Name resolved to an index. `null` (the property is still required) selects **no activity**: turns off every device that is on, collapses the strip to Home and clears the status text (packed as index `0xFFFFFFFF`; the string `"none"` is an ordinary name lookup). Not allowed in an activity hook or a device's `turnOn`/`turnOff` list (error) |
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
- The script receives the trigger's `param` string as a global (empty for most triggers; `repeat=<n>` on an [auto repeat](#auto-repeat) key's `press` and `tap`).
- Stage 1 compiles each script with a Lua 5.4 compiler and fails on a syntax error, naming the path and the Lua error line. Identical sources are stored once. A source is at most **16 KB** (16384 bytes; the device refuses a bigger one too).
- Only `script` ops use `param`. Other ops ignore it.
- Lua 5.4 with 64-bit integers and doubles. Available: base (without `dofile`, `loadfile`, `load`, `require`), `string`, `table`, `math`, `utf8`. No `io`, `os`, `package`, `debug` or `coroutine`. Each script has its own globals, which last until the next takeover or release (not shared between scripts).

#### Script API

The API mirrors the ops, so a script can do anything an op can. Every call takes its arguments positionally or as one table of named fields (`http.get{ url = "http://x/" }`). A failure returns `nil, err`; calls don't raise. Calls run in script order.

| Call | Returns | Notes |
|---|---|---|
| `delay(ms)` | `true` | 1..60000. Waits |
| `switchActivity(name)` | `true`, or `nil, err` | `nil` selects no activity. Waits while the switch's ops (hooks, device `turnOn`/`turnOff`) run, then returns, so code after it runs with the devices on; the waiting time doesn't count towards the 30 s limit. `nil, err` if the queue is full or the name is unknown, and `nil, "switchActivity: the switch runs a script"` (nothing changes) if any of the switch's ops is a `script` (only one script runs at a time) |
| `hid.key(keys [, mode])` | `true` | `keys` is a chord as in `hidKey`; `mode` is `"tap"` (default, waits until the key is up and settled), `"down"` or `"up"`. `"hold"` is refused: it only means something under `press`/`release` |
| `http.get(url [, headers])` | `body, status` | Waits for the reply (10 s timeout; `nil, "timeout"`). Bodies over 16 KB are truncated (status still returned) |
| `http.get{ url=, headers=, reply=false }` | `true` | Fire and forget, like the `httpGet` op |
| `http.post(url, body [, contentType [, headers]])` | `true` | Fire and forget. `contentType` defaults to `application/x-www-form-urlencoded`. Body up to 4096 bytes |
| `udp.send(host, port, data)` | `true` | `data` is a string or `{ hex = "de ad be ef" }` (spaces allowed), 1..4096 bytes |
| `wol.send(mac [, broadcast])` | `true` | `mac` as in `sendWol` |
| `log(...)`, `print(...)` | | Joined with tabs, written to the device log |
| `millis()` | integer | Milliseconds since boot |

`headers` is a table of `{ Name = "value" }`. URLs must be `http://`. Fire-and-forget calls don't wait, so a script that only posts runs straight through.

Limits: a script that runs 100 ms without waiting is stopped, a run is cancelled after 30 s in all (not counting time waiting in `switchActivity`), and the Lua heap is capped at 256 KB. A stopped or failed script is logged and the queue carries on.

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
- A pad key's long-press time and auto repeat are set in the activity's `pad` map ([Auto repeat](#auto-repeat)), not on the binding.

## Inheritance and defaults

Look properties and `longPressMs` inherit down a fixed chain *(proposed; not yet confirmed in `standalone.md`)*:

```
root.defaults  →  (home | activity).defaults  →  page.defaults  →  button
```

(Home's chain is `root.defaults → home.defaults → button`.)

- **Merge rule:** property by property, nearest wins. Object-valued properties (`pressed`, `face`, `image`, `iconOffset`) merge **recursively**, so `"pressed": { "color": ... }` on a key keeps an inherited `pressed.iconColor`. An array or scalar replaces. An explicit `null` clears an inherited value (so a key can opt out of an inherited `icon`).
- **Only look properties and `longPressMs` inherit.** Gestures (`tap`, `long`, `press`, ...), `repeat` and `label` do not: inheriting an action or a label across buttons is almost never intended. (A default `label` can be set explicitly where wanted.)
- `defaults` takes the same look properties as a button, minus gestures and `label`.
- **Empty slots** (`null`) never inherit anything.
- **Groups** sit between the page and their keys in the chain (see [Keys](#keys)).
- A button that has nothing of its own *and* nothing inherited is the all-absent key above; one with inherited backgrounds but no icon gets the inherited face only.

## What the pipeline produces

Stage 1 of the pipeline ([`standalone.md`](standalone.md#build-pipeline-and-sources)) maps this file onto the packed schema:

| In the file | In the blob |
|---|---|
| `home.keys` | `home.keys[]`: per key `{ id, rect: [x, y, w, h] (quarter units), normal, pressed (rle565, or absent), longPressMs, repeatDelayMs, repeatMs }` (0 = default long press / no repeat; a repeating key has `longPressMs` 0); a group becomes its keys; `null` entries are left out |
| `home.keys[i]` gestures | `home.bindings[]`: `{ control: "touch[id]", event, ops }` |
| `activities` map | `activities[]` in file order; `switchActivity` names → indices |
| `pages[p].keys` | `pages[p].keys[]`, as for Home |
| `pages[p].keys[i]` gestures | the activity's `bindings[]`, with `control: "touch[id]"`: ids are unique across the layout, so the name alone says which key |
| `activity.devices` | `devices: int[]` (indices into the root `devices` order) |
| `activity.bindings[]` | appended to the activity's `bindings[]` after the generated touch ones |
| `activity.pad` | the activity's `controls[]`: `{ control: "keyN", longPressMs, repeatDelayMs, repeatMs }`, sorted by key number |
| `devices.<name>` | `devices[]` in file order |
| `use` references | expanded inline |
| `script` ops | Lua source strings (deduplicated), already compile-checked |
| `hidKey.keys` | HID usage codes + modifier bits |
| look properties | two opaque rle565 Buffers per key, the key's size (one shared Buffer per distinct encoding) |

The generated binding order is: touch bindings in page, then key (layout order, a group's keys in place), then gesture order (`press`, `release`, `tap`, `long`), followed by the activity's own `bindings`. A button's `action` becomes `tap`.

Stage 1 is pure and synchronous and its output (a plain object tree) is the unit-test surface. Stage 2 renders the images and stage 3 packs.

## Validation and limits

Stage 1 collects **all** errors it can and reports them together, each as `path: message`. Warnings don't stop the build.

Errors:

- Unknown property, wrong type, out-of-range number; unsupported `version`.
- Duplicate or empty names; `devices` entry, `switchActivity` target or `use` that doesn't exist; a `use` cycle.
- More than 32 keys on a page; an activity with no pages; more pages than the firmware allows.
- A key or group outside the 5x3 grid, overlapping another, or with a position or size that isn't a multiple of 0.25; `x` without `y` (or the reverse); an entry past index 14 without `x`/`y`; a group's key outside its group or overlapping another; `face`/`color`/`image` on a group's key; a duplicate `id` (named or default), or no id left.
- `switchActivity` in an activity hook or a device `turnOn`/`turnOff` list (also through `use`).
- `hold` outside `press`/`release`; both `action` and `tap`; `hidKey` with an unknown key name; bad MAC, port or URL scheme.
- A `script` with both or neither of `lua` and `file`; a missing file; a Lua syntax error; a source over 16 KB.
- A missing icon/SVG file, or a render that fails.
- A `control` of the form `touch[...]` in `bindings`.
- Auto repeat: a `repeat` of the wrong shape or outside 40..10000 ms; `repeat` with a `long` gesture (or a `long` binding of a repeating pad key), or with the key's own `longPressMs`; a `hidKey` `hold` under the `press` of a repeating key or pad key; `repeat` in `defaults` or on a group; a `pad` name other than `key1`..`key32`.
- Resolved layout exceeds a device limit: images or pages (the PSRAM image store and the three page buffers are the real limit), blob size (the partition), op list length. Limits are read from the firmware's published constants (the same source as GET_INFO), so they stay in one place.

Warnings:

- A `long` op list on a button whose `tap` is absent (legal, just unusual).
- A device listed by no activity; an action that nothing uses.
- Two buttons that resolve to identical images *and* actions (a likely copy-paste), at most once per page.
- A group whose keys don't cover it (that part shows black).
- A `pad` entry for a key the activity has no binding for.
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

Home's keys get ids 1 and 2 (slots 0 and 1 of page 0). "Watch TV"'s first page is page 1 of the layout, so `Mute` at index 4 (slot 4) gets id 1 × 15 + 4 + 1 = 20, and its generated binding is `touch[20]` / `tap`. `back` is at index 5 (slot 5): id 21, giving `touch[21]` / `tap` and `touch[21]` / `long`. Listen's page is page 2, so its key is id 31.

## Testing

Unit tested in `host/test/bake/` (`npm test` in `host/`; Node's test runner; stage 1 needs no device, no `sharp`):

- Valid layouts: the example above, a minimal one (`version` + empty `home`), and a maximal one (15 keys on every page).
- Resolution: the defaults chain (each level overrides, recursive merge, `null` clears, gestures and labels don't inherit); name → index; `use` expansion (nested, cycle error); `action` → `tap`; generated binding order.
- Keys (`keys.test.js`, with the example in `fixtures.js` `exampleKeys()`): slot placement, `x`/`y`/`w`/`h`, groups, rect and overlap errors, default and named ids across the layout, `touch[id]` names. `render-real.test.js` renders 1U, span, fractional and group keys for real (opaque, the right sizes, no seam inside a group, each group key pressing alone) and checks the constants against the firmware headers.
- Validation: each error and warning above, with the exact path and message; several errors reported at once.
- Scripts: syntax error is reported; `file` and `lua` forms; identical sources deduplicated.
- Images: through an injected fake renderer, the same resolved look gives the same Buffer object (and so one stored copy); different looks give different ones; an absent look gives none.
- Round trip: the resolved tree packed with binpack and read back by a (test) BPAK loader gives the expected structs.

The browser run of the pipeline (`sharp`, rle565 encoder, Lua checker) is untested; see "Browser support" in [`standalone.md`](standalone.md#still-open).

## Open points

- **Look properties** (above) are a proposal and need to be checked against what node-buttonImage and the demo's face SVG can produce, especially on spans, fractional keys and groups.
- **The defaults chain** (root → activity/home → page → key) is proposed and needs a decision.
- **Face shade bands:** a plain `color` gets a gentle top-to-bottom shade, which bands visibly in RGB565 on dark
  faces (seen on the hardware, 2026-11-07; flat faces look clean). Options: make a plain `color` flat by default
  (a `face` with `top`/`bottom` still gives a gradient), or dither the face when it is encoded.
- **Control and event names** for the pad and encoder wait on the second MCU's serial format.
- **HID key names** (including consumer/media keys) are decided when HID is built.
- **Named actions with arguments** are out of scope (a script reads `param` instead).
- **Wildcard or templated bindings** (e.g. one binding for `macro1..6`) are the authoring layer's job and are not in this format.
- **Multiple files:** `$include` for splitting a big layout isn't specified. Reusable pieces are `actions`, `defaults` and, in the JS form, ordinary imports.
- **Web front end:** the editor probably wants stable ids on buttons and pages to keep diffs sensible; `_`-prefixed properties are reserved for it.
- **`CLAUDE.md` (a rules file, so only with your OK):** link this document from the Repository Layout's `docs/` list.
