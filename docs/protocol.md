# ControlBlock USB Protocol (v1)

This is the host ↔ device protocol for **mode 1 (host-controlled)**. The PC software uploads images and key layouts. The device draws them, runs paging, swiping and press feedback on its own, and reports key presses back to the PC.

How images are stored, replaced and freed on the device is in [`image-assets.md`](image-assets.md).

## Design summary

- **Transport:** A USB vendor interface with one bulk OUT and one bulk IN endpoint. It is reached through WebUSB in browsers and libusb (`usb` package) in Node.
- **Messages:** Each message is a 12-byte header followed by a payload. Messages run back to back on a byte stream; the protocol does not rely on USB packet boundaries or zero-length packets.
- **Requests and responses:** The host sends requests. Every request gets exactly one response, in order. The host may have several requests in flight (pipelining).
- **Events:** The device sends events (key presses, page changes) without being asked.
- **Images:** Images are stored on the device under 32-bit IDs that the host chooses (`0` = none). Using a content hash (CRC-32, say) as the ID gives caching for free: the host asks the device which IDs it already has and uploads only the missing ones. Uploading to an ID that's already there replaces the image everywhere it's used. All images are **rle565** (`.rle565`) with alpha, `key_w`×`key_h`.
- **Keys:** Each key names four images: a background and a foreground for the normal state, and a pair for the pressed state. The host draws everything, including faces, icons and labels (labels are baked into the images). Images can be shared between keys and kept on the device unused, so switching a key's look or showing a press needs no upload.
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

The device is designed as a composite device from the start. The vendor interface is the only one in v1. Later modes add HID keyboard and/or USB MIDI interfaces alongside it.

### Control request: reset stream

| bmRequestType | bRequest | wValue | wIndex | wLength |
|---|---|---|---|---|
| `0x41` (vendor, host→device, interface) | `0x10` RESET_STREAM | 0 | interface number | 0 |

This brings the byte stream back to a known state:
- The device discards any partly received message and aborts any upload in progress.
- It drops queued events. Responses it has already queued may still arrive, as whole messages. The host ignores any response whose seq it isn't waiting for.
- Layout and images are **not** cleared.

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
| 3 | BAD_ARG | Argument out of range (page, key, ID 0…) |
| 4 | NO_SPACE | Image store full |
| 5 | BAD_IMAGE | Data failed rle565 validation, or isn't `key_w`×`key_h` |
| 6 | NOT_FOUND | Referenced image ID doesn't exist |
| 7 | — | Reserved (was IN_USE; freeing is never refused now) |
| 8 | BAD_SEQUENCE | Upload chunk out of order or not started |

## Host → device requests

| Type | Name | Purpose |
|---|---|---|
| `0x00` | PING | Echoes the payload back (for latency and throughput tests) |
| `0x01` | GET_INFO | Device capabilities and geometry |
| `0x02` | RESET | Clear the layout and every image |
| `0x03`–`0x04` | — | Reserved for the status bar (clock and status text), not yet defined |
| `0x05` | SET_BACKLIGHT | Turn the backlight on or off |
| `0x10` | PUT_IMAGE | Upload or replace an image, in chunks |
| `0x11` | FREE_IMAGES | Free images (deferred while keys use them) |
| `0x12` | QUERY_IMAGES | Check which image IDs the device already has |
| `0x20` | SET_PAGE_COUNT | Set the number of pages |
| `0x21` | SET_KEYS | Define one or more keys (applied together) |
| `0x22` | SET_PAGE | Go to a page |

### `0x00` PING

**Request and response:** Any bytes. The response echoes them after the status field.

### `0x01` GET_INFO

**Request:** No payload.

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u16 | protocol_version | `1` |
| u16 | reserved | |
| u32 | max_payload | Largest payload the device accepts, for every message type (8192 in v1) |
| u32 | image_capacity | Bytes available for images |
| u32 | image_used | Bytes in use, counting memory not yet released (retired or mid-replace images) |
| u32 | image_count | Live images |
| u32 | image_largest_free | Largest single allocation possible now |
| u16 | screen_w | 800 |
| u16 | screen_h | 480 |
| u8 | cols | 5 |
| u8 | rows | 3 |
| u8 | max_pages | |
| u8 | page_count | Current number of pages |
| u16 | key_w | Size of a key, and of every image |
| u16 | key_h | |
| u16 | icon_w | Area icons are designed to fill within a key image: 80 for now (still being tuned) |
| u16 | icon_h | |
| u8 | current_page | |
| u8 | fw_version_len | Followed by `fw_version` (UTF-8) |
| u8 | serial_len | Followed by `serial` (UTF-8). Lets the host tell several devices apart |

### `0x02` RESET

**Request:** No payload.

Clears all pages (the page count goes to 1 and every key becomes empty), abandons any upload in progress, and frees every image.

### `0x05` SET_BACKLIGHT

**Request:**

| Type | Field |
|---|---|
| u8 | on |

The hardware backlight can only be switched on or off (through the CH422G); it has no dimming.

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
- **Finishing:** When `offset + len == total_size`, the device checks the image: the header, a size of exactly `key_w`×`key_h`, and a full bounds-checked decode. Bytes after the image (padding) are allowed. If it passes, the image becomes available. If it fails, it's discarded and the response is `BAD_IMAGE`.
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

Keys on pages that are removed are cleared. If the current page no longer exists, the device moves to the last remaining page.

### `0x21` SET_KEYS

**Request:**

| Type | Field |
|---|---|
| u16 | count |
| KeyDef[count] | keys |

Each `KeyDef`:

| Type | Field | Notes |
|---|---|---|
| u8 | page | |
| u8 | key | 0..`cols*rows−1`, numbered across each row, left to right, top to bottom |
| u16 | flags | See below |
| u32 | bg_id | Normal background. `0` = none |
| u32 | fg_id | Normal foreground. `0` = none |
| u32 | pressed_bg_id | Background while pressed |
| u32 | pressed_fg_id | Foreground while pressed. If both pressed IDs are `0`, the normal pair is shown while pressed (no feedback) |

**Flags:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `EMPTY` | Key is blank and inactive. The image IDs must be `0` |
| 1 | `DISABLED` | Draw the key but send no events and show no press feedback |

**How keys are drawn:**
- The background, then the foreground, each `key_w`×`key_h` at the key's origin, with alpha. Both come from the pressed pair while the key is held.
- **Typical use:** the background is a button face (shared by many keys, with a pressed version) and the foreground is the icon with its label baked in. Icons are designed to fill about `icon_w`×`icon_h`, centred, with transparent padding. rle565 skips transparent rows and runs, so the padding costs next to nothing.
- **Full custom art:** put it all in the background and leave the foreground `0`.

**Applying the update:**
- The whole message is applied in one go: the device validates every entry first. If any entry is invalid, nothing changes and the response is `BAD_ARG` or `NOT_FOUND`.
- Every image it references must already be uploaded.
- Only the affected page buffers are redrawn, and the new content appears on screen in one frame, so a half-applied update is never shown.

### `0x22` SET_PAGE

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | page | |
| u8 | flags | bit 0 `ANIMATE`: glide there as if the user had swiped |

## Device → host events

Events have the `EVENT` flag set, and `seq` is the device's event counter.

**If the host isn't reading:**
- The device queues up to 64 events. It sends an event only when there's room for it in the USB transmit buffer, so a host that isn't reading never holds up the device.
- When the queue overflows, it drops the oldest events and sets bit 0 of `lost` on the next event it sends.
- In that case the host should treat key state as unknown, and treat any key it believes is held as released.

| Type | Name |
|---|---|
| `0x80` | KEY |
| `0x81` | PAGE_CHANGED |

### `0x80` KEY

| Type | Field | Notes |
|---|---|---|
| u8 | page | |
| u8 | key | |
| u8 | action | `0` down, `1` up, `2` cancel |
| u8 | lost | Event-overflow flag (see above) |
| u32 | time_ms | Device milliseconds since boot. For working out hold times |

**What each action means:**
- **Down** is sent as soon as a finger lands on a key.
- **Cancel** replaces **up** if the touch turns into a swipe or slides off the key.

**Not sent yet:** the firmware has no key router yet (touch only drives swiping), so no KEY events are sent. PAGE_CHANGED works.

**Host guidance:**
- Trigger normal actions on **up**.
- Use **down** only for things like hold-to-repeat, or for actions where a cancelled press does no harm.
- **Long press and double press are detected by the host, not the device.** The host uses `time_ms` from the down/up events; the `host/` library should provide helpers for this.

### `0x81` PAGE_CHANGED

| Type | Field | Notes |
|---|---|---|
| u8 | page | New current page |
| u8 | cause | `0` the user swiped or tapped the pager, `1` the host sent SET_PAGE |

This is sent once the page settles, not while a swipe is in progress.

## Typical session

1. Open the device and send RESET_STREAM (control request).
2. Send `GET_INFO`: check `protocol_version` and read the geometry.
3. Render the faces and icons (with labels), encode them with the rle565 encoder and compute their IDs (CRC-32). Send `QUERY_IMAGES`, then `PUT_IMAGE` only for the missing ones, pipelined.
4. Send `SET_PAGE_COUNT`, then one `SET_KEYS` with every key.
5. Handle `KEY` events. Later changes are `SET_KEYS` with the affected keys (plus `PUT_IMAGE` for any new images), or a `PUT_IMAGE` that replaces an image in place. `FREE_IMAGES` images that are no longer wanted.

## Device state lifetime

- Layout and images are kept in PSRAM while the device is powered. A host app that restarts can reconnect, query what the device has, and avoid uploading it again.
- Nothing is saved to flash in v1.
- Modes 2 and 3 will need layout and images saved to flash. They'll reuse the same image and KeyDef formats.
- The device can't tell when the host app closes, because there's no signal for it. The host can explicitly `RESET` before it exits if it wants the screen cleared.

## Open questions

- Status bar and pager: Not designed yet. Message types `0x03`–`0x04` are reserved for them.
- Icon size: Somewhere from 80 to 96 px, to be decided by trying it on the hardware. The device reports the sizes in `GET_INFO`, so hosts shouldn't hard-code them.
