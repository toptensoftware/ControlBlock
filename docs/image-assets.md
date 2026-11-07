# Image Assets

How the host uploads, references, replaces and frees key images on the device. Message byte layouts are in [`protocol.md`](protocol.md); this doc covers what they mean and how the firmware implements them, between the protocol and the [page compositor](page-compositor.md).

Status: **implemented and host tested**, not yet run on the hardware.

| Piece | Where |
|---|---|
| Image store | `firmware/main/image_store.cpp/.h` (tests: `TestImageStore.cpp`) |
| Key layout (four slots per key, the compositor's client) | `key_layout.cpp/.h` (`TestKeyLayout.cpp`) |
| Sweep | `PageCompositor::RequestSweep()` (`TestCompositor.cpp`, fuzzed in `TestCompositorFuzz.cpp`) |
| Messages | `protocol.cpp/.h` (`TestProtocol.cpp`) |
| Device wiring | `model.cpp` (store + key layout + lock), `host_link.cpp` (protocol task), `usb.cpp` |
| Host side | `host/src/images.js` (encode, IDs), `host/src/ControlBlock.js` |

## Summary

- **The host names images.** Each image is uploaded under a 32-bit ID the host chooses. The firmware treats it as opaque; a CRC or hash of the encoded file makes a good ID. **ID 0 means "no image".**
- **Keys reference images by ID.** A key definition names four images: a **background** and a **foreground** for the **normal** state, and the same pair for the **pressed** state. The device switches to the pressed pair on its own while a key is held, so press feedback needs no USB traffic.
- **The host draws everything.** Faces, icons and labels are all host images (labels are baked in). The firmware has no stock face, tint or font.
- **Images live on their own.** An image can sit on the device unused (the "off" version of a toggle, say), and one image can be used by any number of keys and slots. Changing what a key shows is just a key definition naming other IDs.
- **Replace in place.** Uploading to an ID that's already live replaces its pixels, and every key using it updates.
- **rle565 in PSRAM.** Images are uploaded already encoded (`.rle565`, with alpha), checked once on arrival, and kept in PSRAM in 4-byte-aligned allocations.
- **Free is deferred, never refused.** Freeing an image removes its ID at once. Its memory is released only when no key uses it **and** the compositor no longer references it.
- **Reset** clears every key definition and every image.
- **Everything goes over the vendor bulk interface** described in `protocol.md`.

## Terms

| Term | Meaning |
|---|---|
| **Image ID** | `u32` chosen by the host. `0` = null. Unique among *live* images. |
| **Entry** | What an ID names (an `ImageEntry`): its current pixels and how many key slots use it. |
| **Pixels** | One rle565 allocation (an `ImageData`). An entry has one at a time; replacing an image gives the entry new pixels and retires the old ones. |
| **Slot** | One of a key's four image references: normal `bg`/`fg`, pressed `bg`/`fg`. |
| **Live** | In the ID map: keys can reference it, uploads replace it and `QUERY_IMAGES` reports it. |
| **Key refs** | How many slots (on any key, any page) point at an entry. |
| **Dying** | Freed by the host but key refs > 0. Not in the ID map: new key definitions can't use it, but keys that already do keep showing it. |
| **Retired** | Pixels no key will draw again: those of a dying entry whose key refs reached 0, or the old pixels of a replaced image. They're waiting for the compositor to drop its references. |
| **Sweep** | The compositor dropping its references to retired pixels (see [Compositor references](#compositor-references)). |

## Image IDs

- **Any `u32` except 0.** The device never interprets an ID or checks it against the data.
- **Content IDs:** the CRC-32 of the encoded `.rle565` file, or the first 4 bytes of its SHA-256, with 0 mapped to 1. The same image then has the same ID across sessions and apps, so a host that reconnects can ask which images the device already has (`QUERY_IMAGES`) and upload only the missing ones. With content IDs, a replace never happens unless two images collide.
- **Slot IDs:** the host can also use fixed IDs ("key 3's icon") and replace them. Changing a picture is then one upload with no `SET_KEYS`.
- **Collisions are the host's problem.** With hashed IDs, two different images could get the same ID; uploading the second replaces the first, wherever it's used. The chance with *n* images is about *n*²/2³³, so ~0.01% at 1,000 images. The host library should check its own images for duplicate IDs before it uploads.
- **Freeing then re-uploading an ID** while the old entry is still dying gives a new, separate entry. The old one keeps showing on the keys that use it until they're redefined.

## Image format

- **rle565** with alpha, exactly **`key_w`×`key_h`** (128x128, from `GET_INFO`). This is what the compositor draws (`comp_key_t`, see [page-compositor.md](page-compositor.md#keys)).
- **Smaller artwork is padded, not shrunk.** An 80–96 px icon is encoded centred in a 128x128 image with transparent edges. rle565 only stores rows from `firstRow` to `firstRow + rowCount` and skips transparent runs, so the padding costs next to nothing. `icon_w`×`icon_h` in `GET_INFO` is the area icons are designed to fill.
- **No size limit** beyond the store's capacity. The host keeps images sensible.
- **Checked on arrival**, in every build (the data comes from outside):
  1. `check_image_header()`, and the width and height must equal the key size.
  2. `check_image()`: every row's runs, bounds checked (no out-of-range runs, rows or palette indexes). It also reports whether every pixel is opaque; if so the pixels are flagged, and a key drawn with them as `bg` gets `COMP_KEY_BG_OPAQUE`.
- `check_image()` was added to rle565 for this. It walks the image the way `decode_image_argb8888` does, sharing its code, but writes no pixels. Decoding into a buffer would have meant a 64 KB write to PSRAM per upload. `comp_image_is_opaque()` uses it too.
- Bytes after the image (padding to 4, say) are allowed.
- Data that fails is discarded and the upload returns `BAD_IMAGE`. On a replace, the old pixels stay.

## Storage

```cpp
struct ImageData                // one allocation: this header, then the pixels
{
    uint32_t size;              // bytes of rle565 data
    uint8_t  flags;             // IMAGE_OPAQUE, IMAGE_RETIRED
    uint32_t sweep;             // sweep generation it waits for (retired only)
    // rle565 data follows, 4-byte aligned; comp_key_t points here
};

struct ImageEntry
{
    uint32_t   id;
    ImageData *data;            // current pixels
    uint16_t   key_refs;        // slots using it
    uint8_t    state;           // Live or Dying
};
```

- **Pixels** are allocated with `heap_caps_aligned_alloc(4, …, MALLOC_CAP_SPIRAM)` on the first chunk of an upload. rle565 needs 4-byte alignment and USB payloads have none, so the data is always copied in. The header sits just before the data, so the compositor's sweep callback can get from a data pointer to its flags.
- **Entries** are kept in a SimpleLib `Map<uint32_t, ImageEntry *>` for live images and a `List<ImageEntry *>` for dying ones. Retired pixels go in a `List<ImageData *>`. Entries are small (~16 B), so whether they go in internal RAM or PSRAM is up to the allocator setup.
- **Capacity:** PSRAM free after init, less a reserve for everything else (`IMAGE_RESERVE`, e.g. 512 KB). That's ~5 MB with three page buffers, or ~250–1,000 icons at 5–20 KB each. Faces shared across keys cost one image, not one per key. `GET_INFO` reports capacity, bytes used (including retired pixels and a replace in progress, since that memory isn't back yet), the image count and the largest free block.
- **Fragmentation:** images are all similar sizes, so the PSRAM heap should cope. If `NO_SPACE` comes back while the free total says there's room, the largest free block shows why, and `RESET` gets the memory back.
- **Not saved to flash** in v1, the same as the rest of the hosted keys (only the standalone layout is saved: [`protocol.md`](protocol.md#layout-upload)). Modes 2 and 3 will want images in flash; the IDs and format here are meant to carry over.

## Key definitions and references

The page model (the compositor's client, under the model lock) holds per key:
- its four `ImageEntry *` slots,
- whether it's pressed (set by the touch router: PRESS to RELEASE/CANCEL, see interactions.md),
- and the `comp_key_t` the compositor sees, derived from these: the normal or pressed pair's current `data` pointers, plus `COMP_KEY_BG_OPAQUE` if that `bg` is opaque.

**Pressed pair:**
- **Both pressed IDs 0** means "no press feedback": the normal pair is shown while pressed. A key that should blank while pressed uses a fully transparent image.
- Press and release just change which pair goes into `comp_key_t`, followed by `comp_page_changed()` (incremental). Press highlighting is a model change on the device, and the compositor treats it like any other. The un-highlight on a press that turns into a drag is delayed, as described in [page-compositor.md](page-compositor.md#swipe-start).

**Applying `SET_KEYS`:**
- **All or nothing.** Every ID it names must be live (or 0), otherwise nothing changes and the response is `NOT_FOUND`. This is checked before anything changes.
- **Ref counting** happens under the model lock while keys are applied. Each new reference is counted first and each old one released afterwards, so a key that keeps the same image never takes its count to 0 in between.
- **When the last ref goes:** a dying entry's pixels are retired and the entry deleted. A live entry is left alone; unused images stay until the host frees them.
- `comp_page_changed(page, COMP_FLUSH)` is called for each changed page before the model lock is released, so the whole message appears in one frame.

## Replacing

An upload to a live ID builds new pixels alongside the old ones. The old pixels stay on screen and in use while the upload is in progress. When the new pixels pass their checks, under the model lock:
1. The entry's `data` is pointed at the new pixels.
2. Every key with a slot on this entry has its `comp_key_t` rebuilt, and each page affected gets `comp_page_changed(page, COMP_FLUSH)`. A scan of the whole model is pages × 15 × 4 compares, which is microseconds.
3. The old pixels are retired.

- **Memory:** a replace needs room for both copies until it finishes. If there isn't room, the response is `NO_SPACE` and the old image stays.
- **Retries:** resending the same upload replaces it again with identical pixels, which is harmless but costs a redraw of every key using it. The host library skips images `QUERY_IMAGES` says are already present, unless it means to replace them.

## Freeing

`FREE_IMAGES` with a list of IDs. For each one:

| The ID is | What happens |
|---|---|
| Live, key refs 0 | Removed from the ID map; its pixels are retired and the entry deleted. |
| Live, key refs > 0 | Removed from the ID map and becomes **dying**. Keys using it keep showing it; when the last of them is redefined, its pixels are retired. |
| Not live (unknown, already freed) | Ignored. Freeing is idempotent. |
| Being uploaded | The upload is abandoned and its new pixels released, since they were never visible. A live entry being replaced is then freed as above. |

The response is always `OK`. Nothing is ever refused as in use.

**Retired pixels still can't be freed straight away**, because the compositor may still hold their pointers (next section). The memory goes back to the heap after the next sweep.

## Compositor references

The compositor references pixels while they're in the **current snapshot** (it may read them at any moment, without a lock) or in any buffer's **record** (it compares the pointer). Records count even though they're never followed: a freed address reused for new pixels would compare equal, and the key wouldn't be redrawn ([page-compositor.md, Image pointers](page-compositor.md#image-pointers)). Records can hold a pointer indefinitely, because idle work keeps copies of old pages.

So once pixels are retired, the store needs the compositor to let go before freeing them. It does this with a **sweep request**, a new compositor call. It's like `comp_reset()`, but non-blocking and partial:

```c
// Drop every reference to pixels for which `retired(img, ctx)` returns
// true. Done on the compositor task at the next key boundary. Returns a
// generation; comp_swept() reaches it once the sweep is done. Any task.
uint32_t comp_request_sweep(void);
uint32_t comp_swept(void);

// Given to comp_init(). Called on the compositor task during a sweep; it
// must not take the client's model lock.
typedef bool (*comp_is_retired_t)(const void *img, void *ctx);
```

At the next key boundary (once any key being copied is finished), the compositor:
1. **Records:** marks unknown every record whose `bg` or `fg` is retired. That's 3 buffers × 15 keys × 2 pointers, so 90 calls. An unknown key is redrawn the next time that buffer is used for that page. The pixels in the buffer aren't touched, so whatever is on screen stays as it is.
2. **Snapshot:** if the current job's snapshot names retired pixels, the job is dropped, as for a page that's no longer asked for. Retired pixels are no longer in the model, so the next snapshot of that page won't name them. The page is then rebuilt from records that are still accurate.
3. Publishes the generation (`comp_swept()`), with a release store.

**The store side:** when pixels retire, they're tagged with the generation from `comp_request_sweep()`. The compositor serves every request made so far in one sweep. The protocol task then frees every retired allocation whose generation is ≤ `comp_swept()`. It checks after every message, and every 20 ms while anything is retired. A reset (`comp_reset()`) also serves every sweep requested before it. `comp_is_retired` reads `IMAGE_RETIRED` from the header in front of the data. That flag is set before the sweep is requested and the request is a release, so no lock is needed.

**Cost:** a sweep is microseconds of compares, plus at worst one dropped job and a few redrawn keys. It happens only when the host frees or replaces an image that was drawn. A host that replaces images many times a second (a live meter, say) pays a redraw for each replace, which it would pay anyway.

**`comp_reset()`** already drops every reference, so `RESET` frees everything straight after it, with no sweep needed.

Rejected alternatives:

| Option | Why not |
|---|---|
| The compositor counts references itself (retain/release callbacks on every record and snapshot change) | Exact, but it adds callbacks to the compositor's hot path and touches the store from the compositor task on every key. Pixels would also live on until a record happened to be overwritten, which could be never for a page buffer that sits idle. |
| Never reuse an address until `RESET` (quarantine freed memory) | No compositor change, but it leaks memory for the whole session, which defeats the point of freeing. |

## Uploading

Images are the only large traffic: ~5–20 KB per image, so 100 KB–1 MB for a full set of keys. At USB full speed that's ~1 MB/s at best.

**Flow** (`PUT_IMAGE`, see [protocol.md](protocol.md#0x10-put_image)):
- **Start** (`offset = 0`): allocates `total_size` (`NO_SPACE` if it can't). Starting a new upload abandons any upload already in progress.
- **Chunks** must arrive in order. An icon usually fits in one message, since `max_payload` is 32 KB.
- **Finish** (`offset + len == total_size`): the pixels are checked (see [Image format](#image-format)). If they pass, they become the ID's pixels: a new live entry, or a [replace](#replacing). The response is `OK`. If they fail, they're discarded and the response is `BAD_IMAGE`.
- **Until it finishes**, a new image isn't live and a replaced one still shows its old pixels.
- **`RESET_STREAM`** and **`RESET`** abandon any upload in progress.

**Streaming.** `PUT_IMAGE` data isn't buffered. After the 12-byte prefix, the protocol copies it from the USB read buffer (512 B at a time) straight into the image's own allocation.

**PSRAM writes during an upload.** The bounce fill rule is that nothing may write PSRAM heavily while fills run ([CLAUDE.md](../CLAUDE.md#bounce-fill-rules)). An upload writes at most ~1 MB/s, about 500 B per fill period, far less than the compositor's 3–8 KB per gap. The check reads PSRAM too, in bursts of one image. This should be safe, but it needs measuring:
- **Done:** chunks are copied straight into PSRAM by the protocol task (core 0). `RUN_STRESS` has two "image replaces" cases (at rest and mid-swipe): two icons replaced per tick (~1 MB/s), with every key in view using the icon, so the compositor redraws while the uploads write.
- **To do:** run it on the hardware.
- **If fills go late:** stage chunks in a small internal-RAM buffer and copy them into PSRAM in fill gaps, the way `CopySlice` does.

## Reset

`RESET`:
1. Takes the model lock and clears every key on every page. The page count goes to 1.
2. Calls `comp_reset()`, which blocks until no key is being drawn and no pixels are referenced.
3. Abandons any upload in progress and frees every image: live, dying and retired.

The old keys stay on screen until the new page 0 has been built (blank until the host defines keys), as described in [page-compositor.md, Reload](page-compositor.md#reload).

## Concurrency

| Who | Does | Locking |
|---|---|---|
| Protocol task (core 0) | Uploads, replaces, `FREE_IMAGES`, `SET_KEYS`, `RESET`, freeing swept pixels | Model lock for ref counts, key slots, entry state and the ID map |
| Swipe task (touch router) | PRESS and RELEASE/CANCEL: picks a key's pair | Model lock |
| Compositor task (core 1) | Reads pixels from its snapshot; sweeps | None for reading; `comp_is_retired` reads one header flag (see above) |
| Bounce fill | Never sees images | |

The store is **platform independent** (`image_store.cpp/.h`, with the allocator and atomics behind `port.h`), so it can be tested on the host.

## Testing

Host tests in `firmware/test/`, written first:
- **Store:** uploads (single and chunked, out of order, abandoned, bad data, wrong size, `NO_SPACE`), replace (success, bad data keeps the old pixels, `NO_SPACE` keeps the old pixels, freed mid-replace), query, free in every state, re-uploading a dying ID, reset, and the stats.
- **References:** `SET_KEYS` is all or nothing. Counts stay right across replace, swaps, the same image in several slots and on many keys, and pressed pairs. A dying entry retires exactly when its last slot goes. A replace rebuilds exactly the keys that use the ID and flags exactly their pages.
- **Press:** the pressed pair is shown while pressed, and both pressed IDs 0 falls back to the normal pair.
- **Compositor sweep:** in the `World` harness, the fuzzer adds random uploads, replaces, frees and key changes. A test allocator poisons freed memory and reuses addresses straight away. The invariants: the compositor never reads freed pixels, and no record or snapshot holds a freed pointer once a sweep has completed.
- **Not automatable:** the USB transport and the PSRAM impact of uploads. These need the device and the `RUN_STRESS` upload case.
