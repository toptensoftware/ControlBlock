# ControlBlock USB Protocol (v1)

This is the host ↔ device protocol for **mode 1 (host-controlled)**. The PC software uploads images, fonts and key layouts. The device draws them, runs paging and swiping on its own, and reports key presses back to the PC.

## Design summary

- **Transport:** A USB vendor interface with one bulk OUT and one bulk IN endpoint. It is reached through WebUSB in browsers and libusb (`usb` package) in Node.
- **Messages:** Each message is a 12-byte header followed by a payload. Messages run back to back on a byte stream; the protocol does not rely on USB packet boundaries or zero-length packets.
- **Requests and responses:** The host sends requests. Every request gets exactly one response, in order. The host may have several requests in flight (pipelining).
- **Events:** The device sends events (key presses, page changes) without being asked.
- **Resources:** Images and fonts are resources, stored on the device under 64-bit IDs that the host chooses. Using a content hash as the ID gives caching for free: the host asks the device which IDs it already has and uploads only the missing ones.
- **Keys:** The device draws a stock button face for each key, optionally tinted. On top go the host's icon (a small image, about 80–96 px) and an optional text label. The host uploads only the icons, not whole buttons. Changing a label or tint needs no upload.
- **Images:** All images are **rle565** (`.rle565`). Fonts are rle565 `.rfnt`.
- **Paging:** Swiping, the pager and press feedback all run on the device. The host is told about page changes but never drives the animation.

## Byte order and encoding

- All integers are **little-endian**, matching the ESP32-S3 and rle565.
- Colours are RGB565 in a `u16`.
- Strings are UTF-8 with a length prefix; there's no terminating null.
- Payload data has no alignment guarantee. The device copies resource data into its own 4-byte-aligned allocations before handing it to rle565.
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
| VID/PID | TBD. Use Espressif's VID `0x303A` with a test PID during development; apply to Espressif for a PID before release |

The device is designed as a composite device from the start. The vendor interface is the only one in v1. Later modes add HID keyboard and/or USB MIDI interfaces alongside it.

### Control request: reset stream

| bmRequestType | bRequest | wValue | wIndex | wLength |
|---|---|---|---|---|
| `0x41` (vendor, host→device, interface) | `0x10` RESET_STREAM | 0 | interface number | 0 |

This brings the byte stream back to a known state:
- The device discards any partly received message and aborts any upload in progress.
- It flushes queued responses and events.
- Layout and resources are **not** cleared.

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
| 3 | BAD_ARG | Argument out of range (page, key, kind…) |
| 4 | NO_SPACE | Resource store full |
| 5 | BAD_RESOURCE | Data failed rle565 validation |
| 6 | NOT_FOUND | Referenced resource ID doesn't exist |
| 7 | IN_USE | Resource is referenced by a key and can't be deleted |
| 8 | BAD_SEQUENCE | Upload chunk out of order or not started |

## Host → device requests

| Type | Name | Purpose |
|---|---|---|
| `0x00` | PING | Echoes the payload back (for latency and throughput tests) |
| `0x01` | GET_INFO | Device capabilities and geometry |
| `0x02` | RESET | Clear the layout (and optionally resources) |
| `0x03`–`0x04` | — | Reserved for the status bar (clock and status text), not yet defined |
| `0x05` | SET_BACKLIGHT | Turn the backlight on or off |
| `0x10` | PUT_RESOURCE | Upload an image or font, in chunks |
| `0x11` | DELETE_RESOURCES | Remove resources |
| `0x12` | QUERY_RESOURCES | Check which resource IDs the device already has |
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
| u32 | max_payload | Largest payload the device accepts, e.g. 32768 |
| u32 | resource_capacity | Bytes available for resources |
| u32 | resource_used | Bytes currently used |
| u16 | screen_w | 800 |
| u16 | screen_h | 480 |
| u8 | cols | 5 |
| u8 | rows | 3 |
| u8 | max_pages | |
| u8 | page_count | Current number of pages |
| u16 | key_w | Size of a whole key, i.e. the button face (also the image size used with `NO_FASCIA`) |
| u16 | key_h | |
| u16 | icon_w | Size icons are designed for, e.g. 96 or 80 (still being tuned) |
| u16 | icon_h | |
| u8 | current_page | |
| u8 | fw_version_len | Followed by `fw_version` (UTF-8) |
| u8 | serial_len | Followed by `serial` (UTF-8). Lets the host tell several devices apart |

### `0x02` RESET

**Request:**

| Type | Field | Notes |
|---|---|---|
| u8 | flags | bit 0 `KEEP_RESOURCES` |

Clears all pages: page count goes to 1 and every key becomes empty. Without `KEEP_RESOURCES` it also deletes every resource.

### `0x05` SET_BACKLIGHT

**Request:**

| Type | Field |
|---|---|
| u8 | on |

The hardware backlight can only be switched on or off (through the CH422G); it has no dimming.

### `0x10` PUT_RESOURCE

**Request:**

| Type | Field | Notes |
|---|---|---|
| u64 | id | Chosen by the host; `0` is reserved |
| u8 | kind | `1` image (`.rle565`), `2` font (`.rfnt`) |
| u8[3] | reserved | |
| u32 | total_size | Size of the whole resource |
| u32 | offset | Where this chunk goes in the resource |
| … | data | Chunk bytes, up to `max_payload − 24` |

**Uploading a resource:**
- **Order:** Chunks must arrive in order. The first chunk has `offset = 0`, and each later chunk starts where the previous one ended.
- **Starting:** At `offset = 0` the device allocates `total_size` bytes and returns `NO_SPACE` if it can't.
- **Finishing:** When `offset + len == total_size`, the device validates the data and the resource becomes available. Images are checked with `check_image_header`, then the bounds-checked decoder in a debug build. Data that fails is discarded and the response is `BAD_RESOURCE`.
- **Uploads in progress:** Only one upload can be in progress at a time. Starting a new one (`offset = 0`) abandons the old one. Any other message type may be interleaved between chunks.
- **ID already exists:** If the ID is already complete on the device, the device returns `OK` and ignores the data. Uploads are therefore safe to retry.

**Recommended ID:** The first 8 bytes of the SHA-256 of the encoded file. The same image then always has the same ID, across sessions and across apps.

### `0x11` DELETE_RESOURCES

**Request:**

| Type | Field |
|---|---|
| u16 | count |
| u64[count] | ids |

The device deletes whatever it can. It returns `IN_USE` if any of the IDs is still used by a key; those resources are kept, and the others are still deleted.

### `0x12` QUERY_RESOURCES

**Request:**

| Type | Field |
|---|---|
| u16 | count |
| u64[count] | ids |

**Response** (after the status field):

| Type | Field | Notes |
|---|---|---|
| u16 | count | |
| u8[ceil(count/8)] | present | Bitmap. Bit `i` (LSB first) is set if `ids[i]` is fully uploaded |

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
| u16 | tint_color | RGB565, used if `TINT` is set |
| u64 | image_id | `0` = no image |
| u64 | font_id | `0` = the font built into the firmware |
| u16 | label_color | RGB565 |
| u8 | label_align | `0` bottom, `1` top, `2` centre |
| u8 | label_len | Followed by `label` (UTF-8). `0` = no label |

**Flags:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `EMPTY` | Key is blank and inactive. All other fields are ignored |
| 1 | `TINT` | Recolour the stock button face to `tint_color` |
| 2 | `NO_FEEDBACK` | Don't show the pressed version of the face |
| 3 | `DISABLED` | Draw the key but send no events when it's pressed |
| 4 | `NO_FASCIA` | Don't draw a stock face. The image fills the whole key (`key_w`×`key_h`) |

**How keys are drawn:**
- Layers, bottom to top:
  1. The stock button face, which is built into the firmware.
  2. The icon, centred in the face and clipped to it, with alpha.
  3. The label.
- **The stock face:**
  - It is an rle565 image with normal and pressed versions. The device shows the pressed version as press feedback.
  - Tinting uses rle565's palette recolouring.
- **Icon size:** Icons should be about `icon_w`×`icon_h`. Smaller icons are centred; larger ones are clipped.
- **Full custom art:** Set `NO_FASCIA` and supply a full `key_w`×`key_h` image instead. This costs a bigger upload.

**Applying the update:**
- The whole message is applied in one go: the device validates every entry first. If any entry is invalid, nothing changes and the response is `BAD_ARG` or `NOT_FOUND`.
- Every image and font it references must already be uploaded.
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
- The device queues up to 64 events.
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
3. Encode the icons with the rle565 encoder and compute their IDs (SHA-256 prefix). Send `QUERY_RESOURCES`, then `PUT_RESOURCE` only for the missing ones, pipelined.
4. Send `SET_PAGE_COUNT`, then one `SET_KEYS` with every key.
5. Handle `KEY` events. Later changes are just `SET_KEYS` with the affected keys, plus `PUT_RESOURCE` for any new images.

## Device state lifetime

- Layout and resources are kept in PSRAM while the device is powered. A host app that restarts can reconnect, query what the device has, and avoid uploading it again.
- Nothing is saved to flash in v1.
- Modes 2 and 3 will need layout and resources saved to flash. They'll reuse the same resource and KeyDef formats.
- The device can't tell when the host app closes, because there's no signal for it. The host can explicitly `RESET` before it exits if it wants the screen cleared.

## Open questions

- Status bar and pager: Not designed yet. Message types `0x03`–`0x04` are reserved for them.
- Icon and button-face size: Somewhere from 80 to 96 px, to be decided by trying it on the hardware. The device reports the sizes in `GET_INFO`, so hosts shouldn't hard-code them.
- Images larger than `max_payload` already work through chunking, so no change is needed there.
