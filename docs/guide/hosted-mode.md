# Hosted mode

In hosted mode an app on your PC drives the ControlBlock: the app decides what every key
shows, the device reports touches, and the app acts on them. Nothing is stored on the device: when the app lets
go, the device goes back to its own [standalone](./standalone-mode.md) Home.

You write the app with the **host library** in [`host/`](../../host/README.md), a JavaScript library for Node.js
and for browsers (WebUSB). It speaks the device's USB protocol, renders key images and can bake and upload a
standalone layout. Apps in other languages can speak the protocol directly (see
[Other languages](#other-languages)).

This guide assumes the device is flashed and connected: see [Getting started](./getting-started.md).

## Installing

The library isn't on the npm registry; it's installed from GitHub, from the same `cli` branch the CLI runs from
(each release's library and CLI, built by the release workflow). In your app's folder:

```sh
npm install github:toptensoftware/ControlBlock#cli            # the latest release
npm install github:toptensoftware/ControlBlock#cli-v0.1.0      # or pin one release
```

That needs Node.js 24 or later and git (npm fetches the package, and one of its dependencies, button-face, from
GitHub). It fetches native packages too (`usb`, `sharp`, and the Skia canvas button-face draws with). Then import
it by name:

```js
import { ControlBlock } from "@toptensoftware/controlblock";
```

In Node that is everything (`host/index.js`); a browser bundler that honours the `browser` export condition gets
the device API only (`host/browser.js`). In a clone of the repo (after `npm install` in the repo root),
[`examples/`](../../examples/README.md) and the CLI import the files by path instead (`./host/index.js`,
`./host/browser.js`), and so do the examples in this guide: in your own app, import
`@toptensoftware/controlblock` where they say `./host/index.js`.

| Entry | What it has |
|---|---|
| `host/index.js` (Node; the package's default) | The device API, the key image helpers and the layout bake |
| `host/browser.js` (the package's `browser` entry; also works in Node) | The device API only: `ControlBlock`, `ControlBlockError`, the protocol constants (`Msg`, `Status`, `KeyAction`, `PageCause`, `WifiState`, `ClockSource`, `USB_VID`, `USB_PID`, ...) and `crc32`. It imports nothing Node-only |

The image helpers and the bake need Node (they use sharp, the file system and button-face's Node renderer).

## A first app

Save this as `hello-host.mjs` in the repo root and run `node hello-host.mjs`:

```js
// hello-host.mjs
import { ControlBlock, KeyAction, keySlot, keyPixelSize, keyImage, svgLayer, buttonLayer, FACE_MARGIN } from "./host/index.js";

const cb = await ControlBlock.open();           // the first ControlBlock on USB
const info = cb.info;                           // the device's grid, limits and state
console.log(`ControlBlock ${info.serial}, firmware ${info.firmwareVersion}`);

// A rounded key face filling width x height
const faceSvg = (width, height, color) => `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}">
    <rect x="1" y="1" width="${width - 2}" height="${height - 2}" rx="14" fill="${color}"/></svg>`;

// One key picture: the face, then an icon and a label (a button-face definition), composed over black at the
// key's size. `size` is the key's size in grid units, { w, h }.
async function picture(size, color, icon, label)
{
    const { width, height } = keyPixelSize(size, info);
    const fw = width - 2 * FACE_MARGIN, fh = height - 2 * FACE_MARGIN;
    const foreground = await buttonLayer({ layers: [
        { type: "svg", src: icon, color: "white", translate: { y: -7 } },
        { type: "text", text: label, font: "bold 11px sans-serif", valign: "bottom", padding: [9, 4] },
    ] }, fw, fh);
    return keyImage([await svgLayer(faceSvg(fw, fh, color), fw, fh), foreground], width, height);
}

// A key's two pictures: { normal, pressed }
const looks = async (size, icon, label) => ({
    normal: await picture(size, "#2a6f97", icon, label),
    pressed: await picture(size, "#61a5c2", icon, label),
});

const ONE = { w: 1, h: 1 }, WIDE = { w: 2, h: 1 };
const play = await looks(ONE, "#play", "Play");
const stop = await looks(ONE, "#stop", "Stop");
const tv = await looks(WIDE, "#apple-tv", "Apple TV");
const home = await looks(ONE, "#home", "Back");

await cb.takeControl();                         // the device boots standalone: take the screen first
await cb.syncImages([play, stop, tv, home].flatMap(k => [k.normal, k.pressed]));    // uploads what's missing

await cb.setPageCount(2);
await cb.setKeys([
    { id: 1, page: 0, ...keySlot(0), ...play },             // slot 0: top left
    { id: 2, page: 0, ...keySlot(1), ...stop },
    { id: 3, page: 0, x: 0, y: 1, ...WIDE, ...tv },         // a 2-wide key on the second row
    { id: 4, page: 1, ...keySlot(0), ...home },             // page 2
]);
await cb.setStatusText({ left: "Hello", right: "{clock}" });

cb.on("key", e => {
    if (e.action !== KeyAction.RELEASE)
        return;
    console.log(`key ${e.id} tapped`);
    if (e.id === 4)
        cb.setPage(0, { animate: true }).catch(err => console.error(err.message));
});
cb.on("pageChanged", e => console.log(`page ${e.page}`));
cb.on("error", err => console.error("device error:", err.message));

process.on("SIGINT", async () => {                      // Ctrl+C: give the screen back
    await cb.releaseControl().catch(() => { });
    await cb.close();
    process.exit(0);
});
```

The rest of this guide explains each step. [`examples/demo.js`](../../examples/demo.js) is a bigger app: two
full pages, half-height keys, a key that toggles between two pictures, a clock key replaced in place every
second, an image freed while in use, and the backlight.

## Connecting

### From Node

`ControlBlock.open()` finds the device on USB (by its ID `303a:4020`), opens it and reads its info:

```js
const cb = await ControlBlock.open();                               // the first one found
const cb = await ControlBlock.open({ serial: "..." });              // one in particular (info.serial)
const cb = await ControlBlock.open({ setTime: true });              // also set its clock from this PC's
```

It throws `No ControlBlock found` if there's none, and an error if the device speaks another protocol version
(the library and the firmware must come from the same version of the repo).

`cb.close()` closes the connection (and fires `close`). On an idle connection it can take up to a second (it
waits for the device's next heartbeat).

### From a browser

Chrome and Edge reach the device over WebUSB. The page must be served from `https://` or `http://localhost`, and
the first connection needs a click (the browser asks which device to allow):

```html
<button id="connect">Connect</button>
<script type="module">
import { ControlBlock, USB_VID, USB_PID } from "./host/browser.js";

document.getElementById("connect").onclick = async () => {
    const device = await navigator.usb.requestDevice({ filters: [{ vendorId: USB_VID, productId: USB_PID }] });
    const cb = new ControlBlock(device);
    await cb.connect();                         // the same object as from open(): cb.info, cb.takeControl()...
    console.log(cb.info);
};
</script>
```

Later visits can skip the prompt with `navigator.usb.getDevices()`, which lists the devices already allowed.

The browser entry has no image helpers: they need Node for now. A browser app can render its key images with
Node ahead of time and fetch them (the bytes in a `keyImage` result's `data` and its `id`), and upload them with
`putImage` or `syncImages`.

### One app at a time

Only one program can have the device open: your app, the CLI or a browser tab (on Windows WinUSB is exclusive).
While your app runs, the CLI can't reach the device, and vice versa. To upload a layout or change Wi-Fi from your
app, use the library's calls on the connection you have (below), not the CLI.

## Modes: taking and giving back control

The device **boots standalone**, running its own layout. It refuses every request that changes the screen with a
`NOT_HOSTED` error until you take control:

```js
await cb.takeControl();
```

Taking control clears the screen: every key and image (Home's too) and both status texts, leaving one empty page.
Then `key` and `pageChanged` events start. The device stops running its layout while you have control (see
[When a host takes control](./standalone-mode.md#when-a-host-takes-control)).

`cb.info.hosted` says which mode the device is in.

**Giving it back.** `await cb.releaseControl()` returns the device to standalone: it clears your keys and images,
rebuilds its Home, and turns the backlight on. It does the same by itself when the **USB connection goes away**:
the cable is unplugged, or the PC suspends the port (sleep).

**Closing your app doesn't release control.** The device can't tell that an app closed its connection, so it
stays hosted, showing your last keys, until another app takes or releases control, or the cable is unplugged. To
leave the device on its Home, call `releaseControl()` before `close()`, as the first app does on Ctrl+C.

That also means a restarted app can find the device still hosted, with its keys and images. `takeControl()`
clears them anyway; an app that wants to reuse them can check `info.hosted` and skip it (`syncImages` then
uploads only what's missing).

| Works in | Calls |
|---|---|
| Hosted mode only | `reset`, `putImage`, `freeImages`, `syncImages`, `setPageCount`, `setKeys`, `removeKeys`, `setPage`, `setStatusText` |
| Both modes | `ping`, `getInfo`, `queryImages`, `setBacklight`, `uploadLayout`, Wi-Fi, clock and name calls, `takeControl`, `releaseControl` |

`reset()` clears the screen like `takeControl()` but stays hosted.

## The grid and key sizes

The screen is a 40 px status bar over a grid of 5 x 3 cells of 144 px (720 x 432), with black around and between
whatever keys you place. Swiping left and right moves between pages.

Keys are placed and sized in **grid units**: 1 unit (1U) is one cell, and positions and sizes go in steps of a
quarter (0.25U, 36 px). So a key can be 1U, 2U wide, 2 x 2, half height, a quarter cell, or 1.5U wide off the cell
lines. Keys must stay inside the grid and must not overlap (touching is fine), and a page holds up to 32.

| Key | Grid units (`w` x `h`) | Picture size (px) |
|---|---|---|
| A cell (1U) | 1 x 1 | 144 x 144 |
| Two cells wide (2U) | 2 x 1 | 288 x 144 |
| Two by two | 2 x 2 | 288 x 288 |
| Half height | 1 x 0.5 | 144 x 72 |
| Quarter (half by half) | 0.5 x 0.5 | 72 x 72 |

Don't hard-code these: `cb.info` has them, and the helpers read it.

| `info` field | What |
|---|---|
| `cols`, `rows` | The grid in cells (5, 3) |
| `keyWidth`, `keyHeight` | A cell in pixels (144) |
| `quarter` | 0.25U in pixels (36) |
| `gridX`, `gridY`, `gridWidth`, `gridHeight` | The grid on the screen, in pixels |
| `maxKeys` | Keys per page (32) |
| `maxPages`, `pageCount`, `currentPage` | Pages |
| `iconWidth`, `iconHeight` | The area an icon should fill in a 1U key (80) |
| `maxImageWidth`, `maxImageHeight` | The largest image (the grid) |
| `imageCapacity`, `imageUsed`, `imageCount`, `imageLargestFree` | Image memory |
| `serial`, `firmwareVersion`, `protocolVersion` | The device |
| `hosted`, `backlight`, `wifiCredentials`, `timeSet` | State flags |
| `layoutCapacity`, `layoutSize`, `layoutCrc` | The stored standalone layout |
| `layoutBlobVersion` | The layout blob version the firmware loads (3); `undefined` from firmware that doesn't report it |

`info` is read when you connect. `await cb.getInfo()` reads it again (for the image memory figures, say).

- `keySlot(i)` is plain grid slot `i` (row by row, 0 top left, 5 the start of the second row) as
  `{ x, y, w: 1, h: 1 }`.
- `keyPixelSize({ w, h }, info)` is the picture size a key of that size needs: `{ width, height }`.

Touches near a key's edge (12 px) and on empty areas only swipe, so there's a narrow swipe-only strip between any
two keys ([`docs/interactions.md`](../interactions.md)).

## Key images

Each key shows one picture normally and, optionally, another while pressed. The device draws them as they are:
**each picture is exactly the key's size and fully opaque**. The device has no faces, fonts or icons of its own:
your app composes the whole picture, including the black margin around the face. The usual 1U look is a 128 px
face centred in the 144 px cell (`FACE_MARGIN`, 8 px of black each side), with an icon about `info.iconWidth`
across and a label.

### Making images (Node)

A **layer** is `{ rgba, width, height, x, y }`: straight-alpha RGBA pixels, placed at `x`, `y` in the key, or
centred if those are missing. The helpers make layers and turn them into an image:

| Helper | Does |
|---|---|
| `buttonLayer(def, width, height)` | Renders a [button-face](https://github.com/toptensoftware/button-face#readme) definition (`{ virtualSize?, layers: [fill, svg, text...] }`) as a layer |
| `svgLayer(svg, width, height)` | Renders an SVG string, stretched to the size, as a layer |
| `keyImage(layers, width, height)` | Composes the layers over black and encodes the result: `{ id, data, width, height }`, ready to upload |
| `renderButtonImage(def, width, height)` | A button-face definition as raw RGBA |
| `composeOverBlack(layers, width, height)` | The composed RGBA, without encoding |
| `encodeImage(rgba, width, height)` | Encodes opaque RGBA to the device's format (rle565) |
| `imageId(data)` | An encoded image's content ID (CRC-32) |

button-face definitions can use its built-in icons (`#play`, `#home`, ...), any of Google's Material Symbols
(`@lightbulb`; downloaded once, needs internet), your own SVG files, and text. Its README describes the format;
the [standalone guide's icon list](./standalone-mode.md#icons) names the built-in icons.

Any other picture works too, as long as it ends up as RGBA layers: make a layer from your own pixels and pass it
to `keyImage`.

### Uploading and IDs

Images live on the device under 32-bit IDs you choose (0 means none). `keyImage` uses a CRC of the image as its
ID, so the same picture always has the same ID, in every run and every app.

```js
await cb.syncImages([a.normal, a.pressed, b.normal]);   // uploads only those the device lacks; returns how many
await cb.putImage(id, data);                            // uploads (or replaces) one, whatever the device has
const [hasIt] = await cb.queryImages([id]);             // true if the device has it
await cb.freeImages([id]);                              // forgets it
```

- **Replacing in place.** Uploading to an ID the device already has replaces that image everywhere it's shown, in
  one frame. Use a fixed ID of your own for a picture you update (a clock, a level meter), and content IDs for the
  rest:

  ```js
  const CLOCK_ID = 0xC10C0001;
  const clockPicture = async () => (await picture(ONE, "#2a6f97", "#info", new Date().toLocaleTimeString())).data;
  await cb.putImage(CLOCK_ID, await clockPicture());
  await cb.setKeys([{ id: 8, page: 0, ...keySlot(4), normal: CLOCK_ID }]);
  setInterval(async () => cb.putImage(CLOCK_ID, await clockPicture()).catch(() => { }), 1000);   // same ID, new pixels
  ```

  A replacement of another size is accepted, but its keys show black until you set them again with a picture of
  their size.
- **Freeing.** A freed ID is gone at once (new keys can't use it), but keys already showing it keep it until they
  change; its memory is released then.
- **Memory.** Images take memory on the device (`info.imageCapacity` bytes in all). An upload that doesn't fit
  fails with `NO_SPACE`. Free what you no longer use.
- **Checks.** The device refuses an image that isn't valid, isn't fully opaque, or is bigger than the grid
  (`BAD_IMAGE`).
- Images and keys are kept only while the device is powered and you're in control: never in flash.

## Keys

`setKeys` adds, replaces or removes keys, by **id**. Continuing the first app:

```js
const HALF = { w: 1, h: 0.5 };
const volUp = await looks(HALF, "#volume-up", "Vol +");             // pictures 144 x 72
const volDown = await looks(HALF, "#volume-down", "Vol -");
await cb.syncImages([volUp.normal, volUp.pressed, volDown.normal, volDown.pressed]);

await cb.setKeys([
    { id: 1, page: 0, x: 0, y: 0, normal: play.normal, pressed: play.pressed },   // w and h default to 1
    { id: 5, page: 0, x: 4, y: 2, ...HALF, normal: volUp.normal },                // half height, no press look
    { id: 6, page: 0, x: 4, y: 2.5, ...HALF, ...volDown },                       // { normal, pressed }
    { id: 7, page: 1, ...keySlot(14), normal: stop.normal, disabled: true },     // shown, but sends no events
    { id: 2, remove: true },
]);
await cb.removeKeys([3]);
```

| Field | What |
|---|---|
| `id` | 1 to 65535, unique across all pages. Key events report it |
| `page` | The page, from 0 (below `info.pageCount`) |
| `x`, `y` | The top-left corner in grid units from the grid's top left, in steps of 0.25 |
| `w`, `h` | The size in grid units, in steps of 0.25 (default 1) |
| `normal` | The picture: an image ID, or a `keyImage` result. Missing or 0: the key is black |
| `pressed` | The picture while held. Missing or 0: no press feedback |
| `disabled` | Draw the key, but send no events and show no press feedback |
| `remove: true` | Remove the key with this id (only `id` is needed) |

- **New id:** adds a key. **Existing id:** replaces that key (its place, size, pictures and flags). Keys a call
  doesn't name stay as they are.
- **A key stays on its page.** To move one to another page, remove it first (in an earlier call), then define it
  again.
- **All or nothing.** The device checks the result of the whole call first and changes nothing if anything is
  wrong: overlapping keys, a key outside the grid, too many on a page, an id used twice, an existing id on another
  page (`BAD_ARG`); an image it doesn't have (`NOT_FOUND`); an image that isn't the key's size (`BAD_IMAGE`). A
  value that can't be sent at all (id 0, `x: 0.3`) throws before anything is sent.
- Several keys can move in one call (two keys swapping places), since the overlap check is on the result. The
  change appears on screen in one frame.
- To change only a key's picture, send the key again with its new images, or replace the image itself in place
  (above).

**Groups.** Neighbouring keys can share one picture, such as a Volume - / + pair under one "Volume" label: render
the whole picture, then give each key its part of it. `clipLayer(layer, width, height)` takes the part of a layer
that falls inside one key (set the layer's `x`, `y` relative to that key first). Each key still presses on its own.

## Pages

```js
await cb.setPageCount(3);                       // 1 to info.maxPages
await cb.setPage(2);                            // jump there
await cb.setPage(0, { animate: true });         // glide there, as if swiped
```

The user swipes between pages, and the status bar shows a pill for each page, the current one highlighted.
Shrinking the page count removes the keys on the pages that go (their ids become free); if the current page goes,
the device moves to the last one left.

## Events

`cb.on(name, handler)` and `cb.off(name, handler)`:

| Event | Fields | When |
|---|---|---|
| `key` | `id`, `page`, `action`, `lost`, `timeMs` | A key was pressed, released or cancelled (hosted only) |
| `pageChanged` | `page`, `cause` | The page changed and settled: `cause` is `PageCause.USER` (swiped) or `PageCause.HOST` (`setPage`) (hosted only) |
| `wake` | `lost`, `timeMs` | The screen was touched while the backlight is off (see [Backlight](#backlight)) |
| `wifiState` | `state`, `stateName`, `lost`, `ip` | The Wi-Fi connection changed |
| `heartbeat` | `timeMs` | About once a second; ignore it |
| `error` | the error | The connection failed (the device was unplugged, say). `close` follows |
| `close` | | The connection closed |

**Key actions** (`KeyAction`):

- `PRESS`: the finger is down on the key. It comes once the touch can no longer become a swipe: at lift for a quick
  tap, or after a short window (about 150 ms). A touch that turns into a swipe sends no key events.
- `RELEASE`: the finger lifted on the key. Run click-style actions here.
- `CANCEL`: the finger dragged off the key after `PRESS`. Don't run release actions.

Every `PRESS` is followed by exactly one `RELEASE` or `CANCEL`. `timeMs` is the device's clock in milliseconds,
for timing holds. `lost` means the device dropped events because your app wasn't reading: treat every key you
think is held as released.

**Long press, double press and repeat** are up to your app; the library has no helpers for them yet. Measure from
`PRESS` (it already comes after the swipe window). A long press with a timer:

```js
const LONG_MS = 500;
const timers = new Map();                       // key id -> long-press timer

cb.on("key", e => {
    if (e.action === KeyAction.PRESS) {
        timers.set(e.id, setTimeout(() => { timers.delete(e.id); onLong(e.id); }, LONG_MS));
        return;
    }
    const timer = timers.get(e.id);             // RELEASE or CANCEL
    clearTimeout(timer);
    timers.delete(e.id);
    if (timer && e.action === KeyAction.RELEASE)
        onTap(e.id);                            // released before the long press fired
});
```

## The status bar

```js
await cb.setStatusText({ left: "Living room", right: "{clock}" });
await cb.setStatusText({ left: "" });          // clears the left text; the right one stays
```

- Two texts, left and right aligned. Only the ones you pass change.
- `{clock}` shows the device's clock (`2:05`), kept current; it's blank until the device knows the time (see
  [Wi-Fi, clock and name](#wi-fi-clock-and-name)).
- Each text is at most 47 bytes of UTF-8, and the font has ASCII characters only for now. A text that's too long
  throws before anything is sent; it's never cut.
- Taking control clears both texts. Releasing control puts back the device's own (the clock on the right).
- The page pills and the Wi-Fi icon are the device's; your app doesn't set them.

The bar's design is in [`docs/status-bar.md`](../status-bar.md).

## Backlight

`await cb.setBacklight(false)` turns the screen off, and `setBacklight(true)` on again. While it's off, a touch
doesn't press keys or swipe: the device sends a `wake` event instead, and it's up to your app to turn the light
back on. The device never turns it on itself, except when control is released.

```js
cb.on("wake", () => cb.setBacklight(true).catch(() => { }));
```

**For now the backlight stays on.** Switching it on can corrupt the device's memory (a hardware problem still
being investigated), so the firmware keeps it lit and `setBacklight(false)` leaves the screen visibly on.
Everything else behaves as if it were off: `info.backlight` reports `false`, and touches send `wake` instead of
key events until you turn it on again. See [`docs/backlight.md`](../backlight.md).

## Wi-Fi, clock and name

These work in either mode and are the same settings the CLI changes
([Getting started](./getting-started.md#wi-fi)).

**Wi-Fi** ([`docs/wifi.md`](../wifi.md)):

| Call | Does |
|---|---|
| `setWifi(ssid, password)` | Stores the network and connects in the background. No password: an open network. SSID up to 32 bytes, password 8 to 63 |
| `clearWifi()` | Forgets the network and turns Wi-Fi off |
| `getWifi()` | `{ ssid, password, ssidBytes }`: the password in clear text |
| `wifiStatus()` | `{ state, stateName, reason, rssi, ip, mac }`; `ip` is null unless connected |
| `waitWifiConnected({ timeoutMs, intervalMs, onStatus })` | Polls `wifiStatus()` until connected (`true`), or no network is stored or time runs out (`false`) |

`state` is a `WifiState`: `NO_CREDENTIALS`, `CONNECTING`, `CONNECTED` or `RETRYING`. The `wifiState` event tells
you when it changes, without polling.

**Clock:** with Wi-Fi the device sets its clock by NTP. Without it, the board has no battery-backed clock: set it
after each power-up.

| Call | Does |
|---|---|
| `setTime(time)` | Sets the clock (a `Date` or milliseconds; default now). `ControlBlock.open({ setTime: true })` does it on connect |
| `getTime()` | `{ known, source, sourceName, time, timeZone, ntpServer }`; `time` is a `Date`, or null while unknown; `source` is a `ClockSource` (`NONE`, `HOST`, `NTP`) |
| `setClock({ timeZone, ntpServer })` | Stores a POSIX TZ string (such as `AEST-10AEDT,M10.1.0,M4.1.0/3`) and/or the NTP server; `""` puts one back to its default (`UTC0`, `pool.ntp.org`) |

`info.timeSet` says whether the device knows the time.

**Name:** `setName(name)` stores the device's network name (`<name>.local`, and its DHCP host name): lowercase
letters, digits and `-`, up to 32; `""` puts back the default (`controlblock-` and part of its MAC address).
`getName()` returns `{ name, isDefault }`.

Values the device would refuse (a long SSID, a time before 2025, a bad time zone or name) throw before anything
is sent.

## Requests and errors

Every call sends its request at once and returns a promise for its own answer, so calls can overlap: there's no
need to wait for one before sending the next, and answers come back in order.

```js
await Promise.all([cb.setKeys(page0Keys), cb.setStatusText({ left: "Ready" })]);
```

A request the device refuses throws a `ControlBlockError`, whose message names both (`SET_KEYS failed:
BAD_ARG`). Its `status` is a `Status` value and its `type` a `Msg` value:

```js
import { ControlBlockError, Status } from "./host/index.js";

try {
    await cb.setKeys(keys);
} catch (err) {
    if (err instanceof ControlBlockError && err.status === Status.NOT_HOSTED)
        await cb.takeControl();             // someone released control: take it again
    else
        throw err;
}
```

| Status | Means |
|---|---|
| `NOT_HOSTED` | A screen request while the device is standalone: `takeControl()` first |
| `BAD_ARG` | A value out of range: a key rect, an id, a page, a status text... |
| `NOT_FOUND` | A key names an image the device doesn't have |
| `BAD_IMAGE` | An image that isn't valid, fully opaque, within the grid, or the key's size |
| `NO_SPACE` | No room for an image (or for a layout) |
| `BAD_LENGTH`, `UNKNOWN_TYPE` | A malformed request (a library and firmware mismatch) |
| `BAD_SEQUENCE`, `BAD_CRC`, `IO_ERROR` | A layout upload went wrong (or Wi-Fi settings couldn't be stored) |

Other errors:

- **Before sending:** values that can't be encoded (id 0, a position not in quarter steps, a status text over 47
  bytes) throw a `RangeError` or `TypeError` without sending anything.
- **Timeouts:** a request with no answer within 5 seconds fails with `<request> timed out`; the library then
  resets the stream, and every other request waiting fails too. `new ControlBlock(device, { timeoutMs })` changes
  the time.
- **Disconnects:** if the device goes away, the `error` and `close` events fire and waiting requests fail with
  `Connection closed`; later calls fail with `Not connected`.

## Uploading a layout from code

The library also bakes and uploads [standalone layouts](./standalone-mode.md), as the CLI does. From a layout
file:

```js
import { bakeFile, uploadBlob, formatEntries } from "./host/index.js";

const { blob, warnings } = await bakeFile("my-layout.json");        // throws a BakeError listing every problem
if (warnings.length)
    console.warn(formatEntries(warnings));
await uploadBlob(blob, { log: console.log });                       // send, wait for the restart, check it
```

- `bakeFile(file)` reads a `.json` layout, or a `.js`/`.mjs` module's default export, with file paths relative to
  the layout's folder, and resolves `{ blob, tree, warnings }`. A `BakeError` has the problems in `errors` (each
  `{ path, message }`).
- A layout built in code bakes with `bake(layout, await createNodeHost(dir), createRenderer())`, where `dir` is
  the folder its file paths are relative to.
- `uploadBlob(blob, { serial, force, log })` opens the device itself, so **close your own connection first**
  (one app at a time). It skips the upload if the device already has this exact layout (`force: true` sends it
  anyway).

With a connection already open, upload on it:

```js
let cb = await ControlBlock.open();
// ...
const result = await cb.uploadLayout(blob, { onProgress: (sent, total) => console.log(`${sent} of ${total}`) });
if (result.uploaded)
    cb = await cb.reconnect();                  // the device restarts; this waits for it and opens it again
```

- `uploadLayout` resolves `{ uploaded, size, crc }`; `uploaded` is false (and nothing is sent) if the device
  already has this layout, unless `{ force: true }`.
- `uploadLayout` (and so `uploadBlob`) refuses a blob of another version than the firmware loads
  (`info.layoutBlobVersion`) before sending anything, even with `force`: the device would refuse it at boot and
  show an empty Home. Update the firmware or re-bake with matching tools. Firmware that doesn't report the version
  isn't checked.
- When the upload is done, the device **restarts**: the connection closes, and `reconnect()` waits for the same
  device to come back and resolves to a new `ControlBlock`. It comes back standalone, running the new layout:
  take control again if you want the screen.
- If an upload fails part way (`NO_SPACE`, `BAD_CRC`, `IO_ERROR`), the device has no layout until the next good
  upload.
- In a browser, `reconnect()` needs an `opener` (a function that finds the device again, say with
  `navigator.usb.getDevices()`, and resolves to a connected `ControlBlock`): its default uses Node's USB.

## Other languages

Any language with USB access can drive the device: it's a vendor-class USB interface with one bulk endpoint in
each direction, and needs no driver on Windows (WinUSB). [`docs/protocol.md`](../protocol.md) is the full
specification: the message format, every request and event, and a typical session. Read its USB notes before
writing a read loop: on Windows a bulk read that times out can leave the pipe deaf, so reads must never time out
(see [HEARTBEAT](../protocol.md#0x83-heartbeat)), and writes that are a multiple of 64 bytes must end with a
zero-length packet ([Zero-length packets](../protocol.md#zero-length-packets)).

Key images are in the rle565 format, made with the [rle565](https://github.com/toptensoftware/rle565) encoder
([`docs/image-assets.md`](../image-assets.md)).

## Limitations

- **The backlight stays on** for now (see [Backlight](#backlight)).
- **No long or double press helpers:** your app times them from key events.
- **Image helpers and the bake are Node only**; the browser entry has the device API only.
- **Not on the npm registry:** install it from GitHub (`github:toptensoftware/ControlBlock#cli`), which needs git.
- **One app at a time** can use the device.
- **Nothing is kept across a power cycle:** keys and images are in the device's memory only; your app sends them
  again when it connects.
- Status texts are ASCII only, up to 47 bytes.
