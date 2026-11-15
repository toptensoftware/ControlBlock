# ControlBlock USB Protocol (v2)

This is the host ↔ device protocol for **mode 1 (host-controlled)**. The device boots into standalone mode ([`standalone.md`](standalone.md)); a host sends [`TAKE_CONTROL`](#0x06-take_control) to enter mode 1 and [`RELEASE_CONTROL`](#0x07-release_control) to give the screen back. The PC software uploads images and sets the keys. The device draws them, runs paging, swiping and press feedback on its own, and reports key presses back to the PC.

How images are stored, replaced and freed on the device is in [`image-assets.md`](image-assets.md). Backlight on/off and the wake-on-touch event are in [`backlight.md`](backlight.md). Uploading the standalone layout to flash is [below](#layout-upload); how it's stored is in [`standalone.md`](standalone.md).

## Design summary

- **Transport:** A USB vendor interface with one bulk OUT and one bulk IN endpoint. It is reached through WebUSB in browsers and libusb (`usb` package) in Node.
- **Messages:** Each message is a 12-byte header followed by a payload. Messages run back to back on a byte stream; the protocol does not rely on USB packet boundaries. A host write whose length is a whole number of 64-byte packets **must be followed by a zero-length packet** (see [Zero-length packets](#zero-length-packets)).
- **Requests and responses:** The host sends requests. Every request gets exactly one response, in order. The host may have several requests in flight (pipelining).
- **Events:** The device sends events (key presses, page changes) without being asked.
- **Images:** Images are stored on the device under 32-bit IDs that the host chooses (`0` = none). Using a content hash (CRC-32, say) as the ID gives caching for free: the host asks the device which IDs it already has and uploads only the missing ones. Uploading to an ID that's already there replaces the image everywhere it's used. All images are **rle565** (`.rle565`), **fully opaque**, any size up to `max_image_w`×`max_image_h`.
- **Keys:** Keys are rectangles on a grid of 1U cells (`key_w`×`key_h`, 144 px) packed edge to edge, placed and sized in **quarter units** (0.25U, `quarter` px), so a key can be 1U, 1.5U, 2U wide, 2U tall and so on. Each key has an **id** (u16, unique across all pages) and names two images, one per state (normal and pressed), each exactly the key's size. The host draws everything, including the face, icon, label and the spacing around the face, composed over black (the device draws no background, blends nothing and has no stock look). Images can be shared between keys and kept on the device unused, so switching a key's look or showing a press needs no upload.
- **Paging:** Swiping, the pager and press feedback all run on the device. The host is told about page changes but never drives the animation.

## Byte order and encoding

- All integers are **little-endian**, matching the ESP32-S3 and rle565.
- Colours are RGB565 in a `u16`.
- Strings are UTF-8 with a length prefix; there's no terminating null.
- Payload data has no alignment guarantee. The device copies image data into its own 4-byte-aligned allocations before handing it to rle565.
- **Payloads can grow:** Later protocol versions may add fields at the end of any payload.
  - Readers must ignore trailing bytes they don't understand.
  - Fields missing from the end take their default value.

## USB layer

| Item | Value |
|---|---|
| Interface class | `0xFF` vendor, subclass `0x00`, protocol `0x00` |
| Endpoints | Bulk OUT `0x01` and bulk IN `0x81`, 64-byte max packet (full speed) |
| Windows | MS OS 2.0 descriptors so WinUSB binds automatically (no driver install) |
| Browser | WebUSB BOS descriptor |
| VID/PID | `0x303A`/`0x4020` for development (Espressif's VID, TinyUSB's vendor-only PID); apply to Espressif for a PID before release |
| Device class | `0xEF`/`0x02`/`0x01` (composite, IAD), so later interfaces need no change here |

The device is a composite device. The vendor interface (interface 0) carries this protocol; standalone mode adds a HID keyboard interface (interface 1, EP 0x82 IN) alongside it, which is not part of this protocol. USB MIDI may be added later.

### Zero-length packets

The host **must** end every bulk OUT transfer whose length is a multiple of 64 (the max packet size) with a zero-length packet (ZLP), the usual USB way of marking the end of a transfer. With WebUSB or the `usb` package that is a second `transferOut` with an empty buffer, straight after the first. Other lengths end in a short packet and need nothing.

Why: for speed, the device receives in 4096-byte transfers (`CONFIG_TINYUSB_VENDOR_EPSIZE`; 512 until 2026-11-12), and a transfer completes only when it is full or a short packet ends it. Without the ZLP, the bytes after the last transfer boundary of such a write wait on the device until more data arrives: the request is never answered and the host times out. (Found with a 4352-byte `LAYOUT_DATA`.) The host library does this in `ControlBlock._write`, and its test `FakeDevice` holds back data the same way the device does.

### Upload speed: hold reads during bulk writes

While a bulk IN read is pending and the device has nothing to send, the host controller keeps polling the IN endpoint, and those NAKed polls take most of each 1 ms full-speed frame: bulk OUT then moves only ~4 packets per frame (~250 KB/s). With no read pending it moves ~14 per frame (~845 KB/s with the device's 4 KB transfers). So a host that uploads a lot should **not submit its next read while bulk-data writes are queued** (`PUT_IMAGE`, `LAYOUT_DATA`: 16-byte replies), for a short while at most (the library holds it up to 20 ms, `READ_HOLD_MS` in `ControlBlock.js`). The replies wait in the device's 2 KB transmit FIFO meanwhile; the device drops one it can't send within 1 s, so the hold must stay short. **Never hold reads while a request with a possibly big reply is queued:** big replies fill the FIFO, the device blocks writing and stops reading, and everything stalls until the hold ends (pipelined 8 KB pings fell from 702 to 259 KB/s when every request held reads). Holding a read back is not the same as cancelling one: never let a read time out (see HEARTBEAT). Measured 2026-11-12 (`examples/trace.js`).

### Control request: reset stream

| bmRequestType | bRequest | wValue | wIndex | wLength |
|---|---|---|---|---|
| `0x41` (vendor, host→device, interface) | `0x10` RESET_STREAM | 0 | interface number | 0 |

This brings the byte stream back to a known state:
- The device discards any partly received message and aborts any upload in progress.
- It drops queued events. Responses it has already queued may still arrive, as whole messages. The host ignores any response whose seq it isn't waiting for.
- Keys and images are **not** cleared.

The host library sends this every time it opens the device, then sends `GET_INFO`. It also sends it whenever it gets out of sync, such as after a timeout or a malformed message.

Codes `0x01` and `0x02` are left free for the WebUSB and MS OS 2.0 vendor codes. These are the codes TinyUSB's examples use.

## Message header (12 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u16 | magic | `0x4243` (`"CB"` as bytes `43 42`) |
| 2 | u8 | type | Message type (see tables below) |
| 3 | u8 | flags | bit 0 `RESPONSE`, bit 1 `EVENT`; other bits are 0 |
| 4 | u16 | seq | **Request:** chosen by the host. **Response:** copies the request's seq. **Event:** the device's own counter |
| 6 | u16 | reserved | 0 |
| 8 | u32 | length | Payload length in bytes, not counting the header |

If the device sees a bad magic or a length over `max_payload`, it treats the stream as corrupt. It drops everything it receives until the host sends RESET_STREAM.

### Responses

A response has the same `type` as its request, with the `RESPONSE` flag set. Its payload always starts with:

| Type | Field |
|---|---|
| u16 | status |
| u16 | reserved |

Any data specific to that response follows these two fields.

| Status | Name | Meaning |
|---|---|---|
| 0 | OK | |
| 1 | UNKNOWN_TYPE | Message type not supported |
| 2 | BAD_LENGTH | Payload too short or inconsistent |
| 3 | BAD_ARG | Argument out of range (page, key rect, key id, ID 0…) |
| 4 | NO_SPACE | Image store full |
| 5 | BAD_IMAGE | `PUT_IMAGE`: the data failed rle565 validation, is larger than `max_image_w`×`max_image_h`, or isn't fully opaque. `SET_KEYS`: an image isn't the size of its key |
| 6 | NOT_FOUND | Referenced image ID doesn't exist |
| 7 | — | Reserved (was IN_USE; freeing is never refused now) |
| 8 | BAD_SEQUENCE | Upload chunk out of order or not started, or a layout commit before all the data arrived |
| 9 | BAD_CRC | Layout upload: the data doesn't match the CRC-32 given at `LAYOUT_BEGIN` |
| 10 | IO_ERROR | Layout upload: a flash erase, write or read-back failed. `WIFI_SET`: the credentials couldn't be stored |
| 11 | NOT_HOSTED | A request that edits the screen while the device is standalone: send [`TAKE_CONTROL`](#0x06-take_control) first |

## Host → device requests

| Type | Name | Purpose |
|---|---|---|
| `0x00` | PING | Echoes the payload back (for latency and throughput tests) |
| `0x01` | GET_INFO | Device capabilities and geometry |
| `0x02` | RESET | Clear the keys and every image |
| `0x03` | SET_STATUS_TEXT | Set the status bar's left or right text |
| `0x04` | — | Reserved, not yet defined |
| `0x05` | SET_BACKLIGHT | Turn the backlight on or off |
| `0x06` | TAKE_CONTROL | Enter host-controlled mode (clears the keys like `RESET`) |
| `0x07` | RELEASE_CONTROL | Hand the screen back to the device's standalone Home |
| `0x10` | PUT_IMAGE | Upload or replace an image, in chunks |
| `0x11` | FREE_IMAGES | Free images (deferred while keys use them) |
| `0x12` | QUERY_IMAGES | Check which image IDs the device already has |
| `0x20` | SET_PAGE_COUNT | Set the number of pages |
| `0x21` | SET_KEYS | Define one or more keys (applied together) |
| `0x22` | SET_PAGE | Go to a page |
| `0x30` | LAYOUT_BEGIN | Start uploading the standalone layout (erases the stored one) |
| `0x31` | LAYOUT_DATA | One chunk of the layout |
| `0x32` | LAYOUT_COMMIT | Finish: verify, save and restart the device |
| `0x40` | WIFI_SET | Store Wi-Fi credentials and connect; an empty ssid clears |
| `0x41` | WIFI_GET | Read the stored Wi-Fi credentials |
| `0x42` | WIFI_STATUS | Wi-Fi connection state, IP, signal |
| `0x50` | TIME_SET | Set the device clock (UTC) |
| `0x51` | TIME_GET | Read the device clock, time zone and NTP server |
| `0x52` | CLOCK_SET | Set the time zone and/or the NTP server |
| `0x60` | NAME_SET | Set the device name (mDNS/DHCP hostname) |
| `0x61` | NAME_GET | Read the device name |

### `0x00` PING

**Request and response:** Any bytes. The response echoes them after the status field.

### `0x01` GET_INFO

**Request:** No payload.

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u16 | protocol_version | `2`. A host for another version should stop here: v2 changed `SET_KEYS`, the `KEY` event, the images and `key_w`/`key_h` (see [Version history](#version-history)) |
| u16 | flags | Bit 0: backlight is on (see [`backlight.md`](backlight.md)). Bit 1: Wi-Fi credentials are stored (see [`wifi.md`](wifi.md)). Bit 2: hosted (a host has taken control; clear = standalone). Bit 3: the clock is set (see [Clock](#clock)). Other bits 0 |
| u32 | max_payload | Largest payload the device accepts, for every message type (8192) |
| u32 | image_capacity | Bytes available for images |
| u32 | image_used | Bytes in use, counting memory not yet released (retired or mid-replace images) |
| u32 | image_count | Live images |
| u32 | image_largest_free | Largest single allocation possible now |
| u16 | screen_w | 800 |
| u16 | screen_h | 480 |
| u8 | cols | Grid width in 1U cells: 5 |
| u8 | rows | Grid height in 1U cells: 3 |
| u8 | max_pages | |
| u8 | page_count | Current number of pages |
| u16 | key_w | 1U: the size of a grid cell, 144. A 1U key's images are `key_w`×`key_h` |
| u16 | key_h | 144 |
| u16 | icon_w | Area icons are designed to fill within a 1U key: 80 for now (still being tuned) |
| u16 | icon_h | |
| u8 | current_page | |
| u8 | fw_version_len | Followed by `fw_version` (UTF-8) |
| u8 | serial_len | Followed by `serial` (UTF-8). Lets the host tell several devices apart |
| u32 | layout_capacity | Largest layout the device can store, in bytes |
| u32 | layout_size | Size of the stored layout; `0` if there is none (or an upload is in progress) |
| u32 | layout_crc | CRC-32 of the stored layout; `0` if there is none |
| u16 | grid_x | The grid's top-left corner on the screen, in pixels: 40 |
| u16 | grid_y | 44 (the 40 px status bar, then 4) |
| u16 | grid_w | `cols × key_w`: 720 |
| u16 | grid_h | `rows × key_h`: 432 |
| u16 | quarter | The quarter unit (0.25U) in pixels: 36. Key rects are counted in these |
| u16 | max_image_w | Largest image accepted: the grid, 720 |
| u16 | max_image_h | 432 |
| u8 | max_keys | Keys per page: 32 |
| u16 | layout_blob_version | The standalone layout blob version this firmware loads: 3. A blob of another version is refused at boot (empty Home), so hosts check it before uploading. Appended within v2: older v2 firmware doesn't send it (treat as unknown) |

`layout_size` and `layout_crc` let a host skip an upload when the device already has the same layout. They describe what was committed; the CRC is not re-checked against the flash for each `GET_INFO`.

### `0x02` RESET

**Request:** No payload. Needs control: `NOT_HOSTED` while standalone.

Clears all pages (the page count goes to 1 and every key becomes empty), abandons any upload in progress, frees every image, and clears both status bar texts.

### `0x03` SET_STATUS_TEXT

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | field | `0` left text, `1` right text |
| u8 | len | Bytes of text, 0..47 |
| u8[len] | text | UTF-8, no NUL |

Sets one of the two texts in the [status bar](status-bar.md) (left aligned and right aligned). The host decides what they say. To change both, send two requests (they may be pipelined).

- **Clock:** every `{clock}` in either text is replaced by the device's local time, `h:mm` on a 12 hour clock without am/pm (`2:05`, `12:30`), kept current each minute. It is blank until the clock is set ([Clock](#clock)). Only exactly `{clock}` is replaced; other braces are shown as they are. The 47-byte limit applies to the text as sent.

- **Empty text** (`len = 0`) clears the text.
- **Setting the current text** changes nothing on screen.
- **Fixed buffers:** each text is held in a 48-byte buffer on the device, so the limit is 47 bytes (not characters).
- **Rejected texts:** the text is not cut. The request fails and the bar keeps what it had. `BAD_LENGTH` if `len` runs past the payload; `BAD_ARG` for a `field` other than 0 or 1, more than 47 bytes, or text that isn't well-formed UTF-8 (bad or truncated sequences, overlongs, surrogates, code points above U+10FFFF, or a NUL).
- **Characters:** the current font covers ASCII 32–126 only, so send ASCII for now.
- **Icons:** the status icons, just left of the right text ([status-bar.md](status-bar.md#icons)) are firmware owned: no request sets or hides them. While hosted only the Wi-Fi icon shows.
- **Lifetime:** the texts stay until changed, or until `RESET` or `TAKE_CONTROL` (which clear both). Releasing control (or a disconnect) clears the left text and sets the right one to `{clock}`, the standalone default. They're drawn by the device's status task, not at once: the new text appears within a frame or two of the response.

### `0x05` SET_BACKLIGHT

**Request:**

| Type | Field |
|---|---|
| u8 | on |

| Value | Meaning |
|---|---|
| `0` | Backlight off |
| non-zero | Backlight on |

The hardware backlight can only be switched on or off (through the CH422G); it has no dimming. Full behaviour is in [`backlight.md`](backlight.md).

- **Touch while off:** the device ignores touches for its own purposes (no key events, no swiping, no pressed look) and sends a [`WAKE`](#0x82-wake) event on each touch-down instead. It never turns the backlight on itself: the host does, with `SET_BACKLIGHT`.
- **Turning off mid-touch:** a key being held gets a `CANCEL`, and a swipe settles. The finger still down is swallowed and sends no `WAKE`.
- **Turning on mid-touch:** a touch that went down while dark stays swallowed until it lifts, so the wake touch never presses what appears under it.
- **Content keeps updating** while dark (`SET_KEYS`, `PUT_IMAGE`, `SET_STATUS_TEXT`, `SET_PAGE`).
- **Both modes:** unlike the screen requests, `SET_BACKLIGHT` works while standalone too. It is device state, not keys, and a setup tool may want the screen dark.
- **Lifetime:** boots on. `SET_BACKLIGHT` changes it; `RESET`, `RESET_STREAM` and taking control leave it alone. Releasing control (the message, a USB unmount or a suspend) **turns it on**, and so does a disconnect while standalone: nobody is left to answer a `WAKE`, and the standalone Home needs the screen.
- **Reading the state:** `GET_INFO` `flags` bit 0.
- Setting the current state does nothing. A missing `on` byte is `BAD_LENGTH`.

### `0x06` TAKE_CONTROL

**Request:** No payload.

Switches the device from standalone to hosted mode ([`standalone.md`](standalone.md), "Runtime modes"). The device **boots standalone**, showing its own Home, and refuses every request that edits the screen until the host takes control:

| Request or event | Standalone | Hosted |
|---|---|---|
| `RESET`, `SET_STATUS_TEXT`, `PUT_IMAGE`, `FREE_IMAGES`, `SET_PAGE_COUNT`, `SET_KEYS`, `SET_PAGE` | **`NOT_HOSTED`** (a refused `PUT_IMAGE`'s data is skipped, so the stream stays in step) | accepted |
| `PING`, `GET_INFO`, `QUERY_IMAGES`, `SET_BACKLIGHT`, `LAYOUT_*`, `WIFI_*`, `TIME_SET`, `TIME_GET`, `CLOCK_SET`, `NAME_*`, `TAKE_CONTROL`, `RELEASE_CONTROL` | accepted | accepted |
| `KEY` and `PAGE_CHANGED` events | none | sent |

There is no implicit takeover: a host that forgets `TAKE_CONTROL` gets `NOT_HOSTED` rather than half-editing Home.

- **Effect:** exactly `RESET`: the screen is cleared (one empty page, no keys), every image is freed (Home's included), an image upload in progress is abandoned, and both status bar texts are cleared. The backlight and the Wi-Fi credentials are left alone. The device's own layout stays in its flash and memory.
- **Already hosted:** the reset happens again and the device stays hosted.
- Standalone actions don't run while hosted ([`standalone.md`](standalone.md)).
- **Reading the mode:** `GET_INFO` `flags` bit 2.

### `0x07` RELEASE_CONTROL

**Request:** No payload.

Hands the screen back. The device goes to standalone: the keys, images and status texts are cleared (the right text then shows `{clock}`, the standalone default) and **Home is rebuilt** from the stored layout (an empty Home if there is none, or if it can't be shown), the screen goes to page 0, the backlight is turned **on**, and the `KEY` / `PAGE_CHANGED` stream stops (ones still queued are dropped; `WAKE`, `HEARTBEAT` and `WIFI_STATE` events are unaffected). The response is sent after that. Releasing while standalone does nothing and answers `OK`.

The device also releases by itself when the host goes away: a **USB unmount** (unplug) or a **USB suspend**, with the same result. The stream state is reset too (a partial message and any layout upload are dropped), exactly as for `RESET_STREAM`. A host that hangs without disconnecting is not detected yet (a heartbeat timeout may be added).

After a disconnect the next host must `TAKE_CONTROL` again; it can read `GET_INFO` `flags` bit 2 to see the mode.

### `0x10` PUT_IMAGE

**Request:**

| Type | Field | Notes |
|---|---|---|
| u32 | id | Chosen by the host; not `0` |
| u32 | total_size | Size of the whole `.rle565` file |
| u32 | offset | Where this chunk goes in the file |
| … | data | Chunk bytes, up to `max_payload − 12` |

**Uploading an image:**
- **Order:** Chunks must arrive in order. The first chunk has `offset = 0`, and each later chunk starts where the previous one ended.
- **Starting:** At `offset = 0` the device allocates `total_size` bytes and returns `NO_SPACE` if it can't.
- **Finishing:** When `offset + len == total_size`, the device checks the image: the header, a size from 1×1 up to `max_image_w`×`max_image_h`, a full bounds-checked decode, and that **every pixel is fully opaque** (no alpha below 255). Bytes after the image (padding) are allowed. If it passes, the image becomes available. If it fails, it's discarded and the response is `BAD_IMAGE`.
- **Size:** any size is accepted here; it's checked against the key's rect when a key names the image ([`SET_KEYS`](#0x21-set_keys)). Replacing an image with one of another size is allowed, but its keys then show black until they are set again with images of their size.
- **Uploads in progress:** Only one upload can be in progress at a time. Starting a new one (`offset = 0`) abandons the old one. Any other message type may be interleaved between chunks.
- **ID already exists:** The new image **replaces** it. Every key using the ID shows the new image, in one frame per page. The old image stays until the new one has passed its checks, so a failed replace changes nothing. A replace needs room for both copies while it's in progress.

**Recommended ID:** CRC-32 (or the first 4 bytes of the SHA-256) of the encoded file, with 0 mapped to 1. The same image then always has the same ID, across sessions and across apps. The host should check its own images for colliding IDs, because a collision replaces an image.

### `0x11` FREE_IMAGES

**Request:**

| Type | Field |
|---|---|
| u16 | count |
| u32[count] | ids |

Always `OK`. Each ID is gone at once: `QUERY_IMAGES` no longer reports it and new key definitions can't use it. Keys that already use it keep showing it, and its memory is released once no key uses it. Unknown IDs are ignored. See [image-assets.md, Freeing](image-assets.md#freeing).

### `0x12` QUERY_IMAGES

**Request:**

| Type | Field |
|---|---|
| u16 | count |
| u32[count] | ids |

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u16 | count | |
| u8[ceil(count/8)] | present | Bitmap. Bit `i` (LSB first) is set if `ids[i]` is available (fully uploaded and not freed) |

### `0x20` SET_PAGE_COUNT

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | count | 1..`max_pages` |

Keys on pages that are removed are cleared (their ids become free). If the current page no longer exists, the device moves to the last remaining page.

### `0x21` SET_KEYS

**Request:**

| Type | Field |
|---|---|
| u16 | count |
| KeyDef[count] | keys |

Each `KeyDef` (20 bytes):

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u16 | id | The key's id, 1..65535, unique across all pages. Chosen by the host; `KEY` events report it |
| 2 | u8 | page | |
| 3 | u8 | x | Left edge, in quarter units from the grid's left: 0..`cols × 4 − 1` (0..19) |
| 4 | u8 | y | Top edge, in quarter units from the grid's top: 0..`rows × 4 − 1` (0..11) |
| 5 | u8 | w | Width in quarter units: 4 = 1U, 6 = 1.5U, 8 = 2U |
| 6 | u8 | h | Height in quarter units |
| 7 | u8 | reserved | 0 |
| 8 | u16 | flags | See below |
| 10 | u16 | reserved | 0 |
| 12 | u32 | normal | Image shown normally. `0` = none (the key's rect is black) |
| 16 | u32 | pressed | Image shown while the key is held. `0` = the normal image stays (no press feedback) |

**Flags:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `REMOVE` | Remove the key with this `id`. The other fields are ignored; an id with no key is ignored too |
| 1 | `DISABLED` | Draw the key but send no events and show no press feedback |

Other bits must be 0 (`BAD_ARG`).

**Geometry:** in pixels a key's rect is `x × quarter`, `y × quarter` from (`grid_x`, `grid_y`), `w × quarter` by `h × quarter`. A plain 5×3 grid of 1U keys has slot `i` (row-major) at `x = i % 5 × 4`, `y = i / 5 × 4`, `w = h = 4`. Keys may be any size in quarter steps (wide, tall, or a group of small keys) as long as each page's keys lie inside the grid and don't overlap (keys that only touch are fine). Areas no key covers are black.

**Images:**
- Each image is exactly the key's size in pixels (`w × quarter` by `h × quarter`), fully opaque, and is copied to the screen as it is: no blending, no background. The host composes the face, icon, label and the black margin around the face (the 1U cell is edge to edge with its neighbours, so the spacing between keys is part of the images).
- **Typical 1U key:** a 128×128 face centred in the 144×144 image (8 px of black each side), the icon filling about `icon_w`×`icon_h` of it, the label baked in. The pressed image is the same with the pressed face.
- **Groups:** two keys side by side (say `−` and `+`) can carry one picture across both (a shared "Volume" label): each key's image is its part of the picture. Each key presses on its own.

**Editing keys (by id):**
- Each `KeyDef` defines the key with its `id`: a new id adds a key; an id that already exists replaces that key (its rect, flags and images). A key stays on its page: naming an existing id with another page is `BAD_ARG` (remove it first, in an earlier message, then define it on the new page). `REMOVE` removes a key.
- Keys the message doesn't name are left alone. Several keys of a page can move in one message (keys trade places, say): the overlap check is made on the result.
- A key that is replaced while held stays held (it shows the new pressed image).

**Applying the update:**
- The whole message is applied in one go: the device checks the result of every entry first. If anything is wrong, nothing changes:

  | Status | Cause |
  |---|---|
  | `BAD_LENGTH` | The payload is shorter than `count` KeyDefs |
  | `BAD_ARG` | An unknown flag; id 0; the same id twice in the message; an existing id on another page; a page past `page_count`; a rect that is empty or not inside the grid; two keys of a page overlapping; more than `max_keys` keys on a page |
  | `NOT_FOUND` | An image ID that isn't uploaded (or was freed: a key may keep naming an image it already shows after it is freed, but a new key can't take it) |
  | `BAD_IMAGE` | An image that isn't the key's size |

- Only the affected page buffers are redrawn, and the new content appears on screen in one frame, so a half-applied update is never shown.

### `0x22` SET_PAGE

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | page | |
| u8 | flags | bit 0 `ANIMATE`: glide there as if the user had swiped |

## Layout upload

Saves the standalone-mode layout (a BPAK blob from the bake in `host/src/bake/`, see [`standalone.md`](standalone.md)) in the device's `layout` flash partition. The device doesn't look inside the blob here: it checks the size and a CRC-32 and stores the bytes. The CRC-32 is the usual zlib one (polynomial `0xEDB88320`, initial and final XOR `0xFFFFFFFF`; the CRC of `"123456789"` is `0xCBF43926`). (These messages and the `GET_INFO` `layout_*` fields kept their opcodes and wire format when the standalone "config" was renamed "layout"; only the names changed, so the protocol version is unchanged.)

```
LAYOUT_BEGIN(size, crc)  →  LAYOUT_DATA(0, …) … LAYOUT_DATA(n, …)  →  LAYOUT_COMMIT  →  device restarts
```

### `0x30` LAYOUT_BEGIN

**Request:**

| Type | Field | Notes |
|---|---|---|
| u32 | total_size | Size of the blob in bytes, at least 1 and at most `layout_capacity` |
| u32 | crc32 | CRC-32 of the whole blob |

- Starts a new upload, abandoning one in progress.
- **The stored layout is destroyed** as soon as the upload starts (the response is `OK`), even if the upload later fails: the device then has no layout and boots to an empty Home.
- `BAD_LENGTH` if the payload is shorter than 8 bytes. `BAD_ARG` if `total_size` is 0, `NO_SPACE` if it is more than `layout_capacity`; in both cases nothing changes. `IO_ERROR` if the flash can't be erased.
- **The screen goes dark** (backlight off) until the upload ends. Flash erases and writes stall the display, so it is blanked rather than glitching. A failed upload, `RESET_STREAM` or a new `BEGIN` that fails restores the backlight to the host's setting ([`SET_BACKLIGHT`](#0x05-set_backlight)).

### `0x31` LAYOUT_DATA

**Request:**

| Type | Field | Notes |
|---|---|---|
| u32 | offset | Where this chunk goes in the blob |
| … | data | Chunk bytes, at least 1, up to `max_payload − 4` |

- **Order:** the first chunk has `offset = 0` and each later chunk starts where the previous one ended (like [`PUT_IMAGE`](#0x10-put_image)). Chunks may be pipelined; other message types may be interleaved.
- **Errors end the upload:** `BAD_SEQUENCE` (no upload in progress, or the offset isn't the next one), `BAD_ARG` (the chunk runs past `total_size`) or `IO_ERROR`. The stored layout stays destroyed and every later `LAYOUT_DATA` gets `BAD_SEQUENCE` until a new `LAYOUT_BEGIN`. `BAD_LENGTH` (no data) changes nothing.
- A chunk's response arrives once it is in flash. A chunk can take tens of milliseconds (erases), so a host shouldn't have unlimited chunks in flight with a fixed timeout; the `host/` library keeps a small window.

### `0x32` LAYOUT_COMMIT

**Request:** No payload.

- `BAD_SEQUENCE` if no upload is in progress, or if fewer than `total_size` bytes have arrived (the upload carries on).
- Otherwise the device checks the CRC-32 of the bytes it received (`BAD_CRC` if wrong), re-reads the flash and checks it again (`IO_ERROR` if wrong), then writes the layout's header. Any failure ends the upload with the layout destroyed, as above.
- On `OK` the layout is saved and **the device restarts** right after sending the response. The host sees its USB connection drop and should wait for the device to come back (about a second or two), then read `layout_size` / `layout_crc` from `GET_INFO` to confirm.
- Bytes sent after `LAYOUT_COMMIT` in the same stream are ignored.

**Other messages and the upload:** `RESET_STREAM` abandons the upload (the layout stays destroyed). `RESET` doesn't touch it. Everything else works as usual during an upload, but the screen is dark.

## Wi-Fi

The host sets the Wi-Fi credentials; the device keeps them in NVS and connects in the background. Design, storage and the connection state machine: [`wifi.md`](wifi.md). These messages work in every mode, and `RESET`, `RESET_STREAM`, taking control and releasing leave the credentials and the connection alone.

### `0x40` WIFI_SET

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | ssid_len | 0..32 |
| u8 | pass_len | 0, or 8..63 |
| u8[ssid_len] | ssid | Raw bytes (an SSID need not be UTF-8) |
| u8[pass_len] | pass | Raw bytes. `pass_len` 0 = an open network |

- `BAD_LENGTH` if the payload is shorter than the two lengths say (checked first). Trailing bytes are ignored.
- `BAD_ARG` if `ssid_len` is over 32, `pass_len` is 1..7 or over 63, `ssid_len` is 0 with a non-zero `pass_len`, or either string contains a NUL byte. Nothing changes.
- `ssid_len` 0 (with `pass_len` 0) **clears** the credentials: they are erased, the device disconnects and Wi-Fi stops.
- `IO_ERROR` if the NVS write fails. The previously stored credentials are still in place.
- Setting exactly the credentials already stored does nothing (no flash write, no reconnect) and responds `OK`.
- On `OK` the credentials are stored. The connection attempt starts after the response and does not hold it up: poll `WIFI_STATUS` or watch for `WIFI_STATE` events. A 64-character hex pre-shared key is not accepted.

### `0x41` WIFI_GET

**Request:** No payload (any payload is ignored).

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u8 | ssid_len | 0 if none are stored |
| u8 | pass_len | |
| u8[ssid_len] | ssid | |
| u8[pass_len] | pass | In clear text (see [`wifi.md`](wifi.md#security)) |

### `0x42` WIFI_STATUS

**Request:** No payload (any payload is ignored).

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u8 | state | See below |
| u8 | reason | The last disconnect reason (ESP-IDF `wifi_err_reason_t`), 0 if none. For diagnostics only |
| i8 | rssi | dBm while connected, else 0 |
| u8 | reserved | 0 |
| u8[4] | ip | `a.b.c.d` in that order while `CONNECTED`, else 0 |
| u8[6] | mac | The station MAC. All zeros in `NO_CREDENTIALS` (the driver isn't running then) |

| State | Name | Meaning |
|---|---|---|
| 0 | `NO_CREDENTIALS` | Nothing stored; Wi-Fi is not running |
| 1 | `CONNECTING` | The first attempt: associating or waiting for an IP |
| 2 | `CONNECTED` | Has an IP |
| 3 | `RETRYING` | An attempt failed or the link dropped. It stays `RETRYING` across the retries (1 s, doubling to 30 s; at once to 30 s after an authentication failure) until it connects. After an authentication failure it stays `RETRYING` but stops retrying (the reason says why) until new credentials are set or the device reboots |

## Clock

The device keeps a clock for the status bar's `{clock}` ([`SET_STATUS_TEXT`](#0x03-set_status_text)). It is set by NTP while Wi-Fi is connected (resynced hourly), or by a host with `TIME_SET`; whichever came last wins. The board has no battery-backed clock: the time survives a restart (such as after a layout upload) but not a power-off, after which it is unknown until set again. The time zone and the NTP server are stored in NVS. These messages work in every mode (no `TAKE_CONTROL` needed), and `RESET`, `RESET_STREAM`, taking control and releasing leave the clock and its settings alone.

**Known:** the clock counts as set when it reads 2025-01-01 or later (it starts at 1970 at power-up). `GET_INFO` `flags` bit 3 and `TIME_GET`'s `known` report it.

### `0x50` TIME_SET

**Request:**

| Type | Field | Notes |
|---|---|---|
| i64 | unix_ms | UTC milliseconds since 1970-01-01 |

- `BAD_LENGTH` if the payload is shorter than 8 bytes. Trailing bytes are ignored.
- `BAD_ARG` for a time before 2025-01-01 00:00 UTC. The clock is left alone.
- On `OK` the clock is set and its source becomes "host". The status bar shows the new time within a frame or two.

### `0x51` TIME_GET

**Request:** No payload (any payload is ignored).

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u8 | known | `1` if the clock is set (see above) |
| u8 | source | Who set it last: `0` none (or not known), `1` host, `2` NTP. Kept in RAM only: after a restart it is `0` even if the time is still known |
| i64 | unix_ms | The device clock now, UTC milliseconds (meaningless while `known` is 0) |
| u8 | tz_len | Followed by `tz`: the POSIX TZ string in use (default `UTC0`) |
| u8 | server_len | Followed by `server`: the NTP server in use (default `pool.ntp.org`) |

### `0x52` CLOCK_SET

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | mask | Bit 0: time zone follows. Bit 1: NTP server follows. Other bits 0 |
| u8 | tz_len | Only if bit 0: 0..63 |
| u8[tz_len] | tz | A POSIX TZ string, e.g. `AEST-10AEDT,M10.1.0,M4.1.0/3` (Sydney) or `EST5EDT,M3.2.0,M11.1.0`. Empty = the default `UTC0` |
| u8 | server_len | Only if bit 1: 0..63 |
| u8[server_len] | server | A host name or IPv4 address. Empty = the default `pool.ntp.org` |

- Fields not in `mask` are left as they are.
- `BAD_LENGTH` if the payload runs out before a field `mask` says follows. Trailing bytes are ignored.
- `BAD_ARG` for an unknown mask bit, a time zone that isn't printable ASCII without spaces (0x21-0x7E), a server with characters other than `A-Z a-z 0-9 . -`, or either over 63 bytes. All or nothing: nothing changes. The time zone isn't checked as POSIX; the C runtime treats one it can't parse as UTC.
- `IO_ERROR` if the NVS write fails. The previous settings stay.
- Setting what is already in use does nothing (no flash write) and responds `OK`.
- On `OK` a new time zone applies at once (the clock text is redrawn, DST by the zone's rules), and a new server restarts NTP at once if Wi-Fi is connected (otherwise it is used from the next connect).

## Device name

The device's name on the network: its mDNS name (`<name>.local`) and DHCP hostname. Stored in NVS; the default (nothing stored) is `controlblock-` and the last three bytes of the Wi-Fi MAC in hex, e.g. `controlblock-a1b2c3`. These messages work in every mode (no `TAKE_CONTROL` needed), and `RESET`, `RESET_STREAM`, taking control and releasing leave the name alone.

### `0x60` NAME_SET

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | len | 0..32 |
| u8[len] | name | A hostname label: `a-z 0-9 -` (`A-Z` are lowercased), not starting or ending with `-`. Empty = back to the default |

- `BAD_LENGTH` unless the payload is exactly `1 + len` bytes.
- `BAD_ARG` for a name that breaks the rules above or is over 32 bytes. Nothing changes.
- `IO_ERROR` if the NVS write fails. The previous name stays.
- Setting the name in use does nothing (no flash write) and responds `OK`. Setting the default name explicitly stores it (it no longer follows the MAC).

### `0x61` NAME_GET

**Request:** No payload (any payload is ignored).

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u8 | is_default | `1` if no name is stored (the MAC-based default is in use) |
| u8 | len | Followed by `name`: the name in use (lowercase) |

## Device → host events

Events have the `EVENT` flag set, and `seq` is the device's event counter.

`KEY` and `PAGE_CHANGED` exist **only while hosted** ([`TAKE_CONTROL`](#0x06-take_control)); standalone, the device handles keys itself and sends neither. `WAKE`, `HEARTBEAT` and `WIFI_STATE` are sent in both modes.

**If the host isn't reading:**
- The device queues up to 64 events. It sends an event only when there's room for it in the USB transmit buffer, so a host that isn't reading never holds up the device.
- When the queue overflows, it drops the oldest events and sets bit 0 of `lost` on the next event it sends.
- In that case the host should treat key state as unknown, and treat any key it believes is held as released.

| Type | Name |
|---|---|
| `0x80` | KEY |
| `0x81` | PAGE_CHANGED |
| `0x82` | WAKE |
| `0x83` | HEARTBEAT |
| `0x84` | WIFI_STATE |

### `0x80` KEY

| Type | Field | Notes |
|---|---|---|
| u16 | id | The key's id (from `SET_KEYS`) |
| u8 | page | The page it was touched on |
| u8 | action | `0` press, `1` release, `2` cancel |
| u8 | lost | Event-overflow flag (see above) |
| u8[3] | reserved | 0 |
| u32 | time_ms | Device milliseconds since boot. For working out hold times |

A touch belongs to the key whose rect it lands in; near a key's edge (12 px) and in areas no key covers, a touch can only swipe ([`interactions.md`](interactions.md)).

**What each action means:**
See [`interactions.md`](interactions.md) for exactly when these fire.
- **Press** is sent when the short swipe window after touch-down ends with the finger still on the key (or at lift, for a quick tap). A touch that becomes a swipe sends no key events.
- **Release** is sent when the finger lifts while still on the key.
- **Cancel** is sent instead of release if the finger drags out of the key after press. Don't trigger release actions.
- Every press is followed by exactly one release or cancel.

**Host guidance:**
- Press is safe to act on: it only fires once the touch can no longer become a swipe. Use **release** for actions that should only run on a completed click, and ignore **cancel**.
- **Long press and double press are detected by the host, not the device.** The host uses `time_ms` from the press/release events (press is delayed by the swipe window, so measure from press, not touch-down); the `host/` library should provide helpers for this.

### `0x81` PAGE_CHANGED

| Type | Field | Notes |
|---|---|---|
| u8 | page | New current page |
| u8 | cause | `0` the user swiped or tapped the pager, `1` the host sent SET_PAGE |

This is sent once the page settles, not while a swipe is in progress.

### `0x82` WAKE

| Type | Field | Notes |
|---|---|---|
| u8 | lost | Event-overflow flag (see above) |
| u8[3] | reserved | 0 |
| u32 | time_ms | Device milliseconds since boot |

The screen was touched while the backlight is off (see [`SET_BACKLIGHT`](#0x05-set_backlight) and [`backlight.md`](backlight.md)).
- One `WAKE` per touch-down of the first finger. Extra fingers send nothing.
- No `KEY` or `PAGE_CHANGED` events come from that touch: it is swallowed until the finger lifts.
- The device stays dark until the host sends `SET_BACKLIGHT`. A host that doesn't want to wake can ignore the event.
- Queued events (such as the `CANCEL` for a key held when the light went off) arrive before it.

### `0x83` HEARTBEAT

| Type | Field | Notes |
|---|---|---|
| u32 | time_ms | Device milliseconds since boot |

Sent about once a second (`PROTO_HEARTBEAT_MS`), whether or not anything else is happening. **It keeps the host's idle bulk IN read from ever timing out.**

- **Why:** on Windows (WinUSB, `usb` 3.x) a bulk IN read that times out is cancelled, and a cancelled read leaves the pipe deaf: the device still receives requests, but nothing it sends reaches the host until the device is reopened. Neither `clearHalt` nor `RESET_STREAM` recovers it. A host whose reads time out every few hundred milliseconds hits this within seconds of idling.
- **Host rule:** set the read timeout well past the interval (the `host/` library uses 5 s), so a read always completes first. The heartbeat is also why `close()` on an idle connection can take up to a second: a pending read can't be cancelled, so it waits for the next heartbeat. A host that doesn't need this (WebUSB, which can abort reads) may ignore the event.
- **Not queued:** it is written straight out, not through the event queue, so it can never overflow it or set a `lost` flag. It shares the events' `seq` counter.
- **Only into free room:** it is sent only while at least half of the transmit FIFO is free (`PROTO_HEARTBEAT_ROOM`, 1024 of 2048 bytes). With no app reading, the unread ones stop at half full, so responses and key events are never crowded out; they are drained and ignored when a host connects.
- Older firmware doesn't send it. A host that sets a long read timeout needs this firmware.

### `0x84` WIFI_STATE

| Type | Field | Notes |
|---|---|---|
| u8 | state | As in `WIFI_STATUS` |
| u8 | lost | Event-overflow flag (see above) |
| u8[2] | reserved | 0 |
| u8[4] | ip | As in `WIFI_STATUS` |

Sent when the state or the address changes (not on every retry), so a host can show "connected" without polling. A host that doesn't care ignores it.

## Typical session

Uploading a standalone layout instead: `RESET_STREAM`, `GET_INFO` (stop if `layout_blob_version` is reported and differs from the blob's root `version`; skip the rest if `layout_size` and `layout_crc` already match), `LAYOUT_BEGIN`, pipelined `LAYOUT_DATA`, `LAYOUT_COMMIT`, then wait for the device to restart.

And for a host-controlled session:

1. Open the device and send RESET_STREAM (control request).
2. Send `GET_INFO`: check `protocol_version` and read the geometry. Send `TAKE_CONTROL` (the device boots standalone and refuses the screen requests below until then). When finished, send `RELEASE_CONTROL`; closing or unplugging has the same effect.
3. Lay out the keys (ids and quarter-unit rects), render each key's normal and pressed images at its size over black (face, icon, label), encode them with the rle565 encoder and compute their IDs (CRC-32). Send `QUERY_IMAGES`, then `PUT_IMAGE` only for the missing ones, pipelined.
4. Send `SET_PAGE_COUNT`, then one `SET_KEYS` with every key. Set the status bar texts with `SET_STATUS_TEXT`, and again whenever they change.
5. Handle `KEY` events (by key id). To sleep the screen, send `SET_BACKLIGHT` off; on a `WAKE` event, send it on. Later changes are `SET_KEYS` with the affected keys (plus `PUT_IMAGE` for any new images), or a `PUT_IMAGE` that replaces an image in place. `FREE_IMAGES` images that are no longer wanted.

## Device state lifetime

- Keys and images are kept in PSRAM while the device is powered. A host app that restarts can reconnect, query what the device has, and avoid uploading it again.
- Keys and images are never saved to flash. Only the standalone layout is ([layout upload](#layout-upload)); the standalone mode that runs it is in [`standalone.md`](standalone.md).
- The device can't tell when the host app closes (it only sees USB unmount or suspend, which release control). A host that wants the device back on its Home before it exits sends `RELEASE_CONTROL`; `RESET` only clears the screen and keeps the host in control.

## Version history

- **v2** (2026-11): keys are rects in quarter units on a grid of 144 px cells (`key_w`/`key_h` went from 128 to 144; `grid_*`, `quarter`, `max_image_*` and `max_keys` appended to `GET_INFO`), each with a u16 id and one fully opaque image per state, the size of the key (was four 128×128 images with alpha: background and foreground, normal and pressed). `SET_KEYS` edits keys by id (`REMOVE` replaced `EMPTY`); `KEY` events carry the id (12 bytes, was 8: page and slot); `PUT_IMAGE` refuses images that aren't opaque. A v1 host gets `BAD_ARG`/`BAD_LENGTH` from `SET_KEYS` and should check `protocol_version` first. Later (2026-11-15, still v2, since payloads can grow): `layout_blob_version` appended to `GET_INFO`.
- **v1:** the first version.

## Open questions

- Status bar: the left and right texts are host-settable (`SET_STATUS_TEXT`), and the device has a clock (`{clock}`, [Clock](#clock)). Still open: other status bar content (the pager is pills in the status bar; there is no pager bar). Message type `0x04` is reserved for them.
- Icon size: Somewhere from 80 to 96 px, to be decided by trying it on the hardware. The device reports the sizes in `GET_INFO`, so hosts shouldn't hard-code them.
- Deflate: images are ~3.5x the bytes of v1's shared layers. Compressing uploads (an optional flag on `PUT_IMAGE`) may follow if hosted updates feel slow.
