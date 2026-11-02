#pragma once

// Image store: the rle565 key images the host uploads under 32-bit IDs.
// Uploads, replace in place, deferred free and the sweep handshake with the
// compositor. Platform independent: memory and the compositor come in
// through ImageStoreHost. Spec: docs/image-assets.md.
//
// Not thread safe: the caller serialises every call (the model lock on the
// device). The one exception is ImageStore::IsRetired(), which the
// compositor task calls during a sweep.

#include <stddef.h>
#include <stdint.h>
#include "Core.h"
#include "port.h"

#define IMAGE_ID_NONE   0

// ImageData flags
#define IMAGE_OPAQUE    0x01        // every pixel is opaque (comp_image_is_opaque)
#define IMAGE_RETIRED   0x02        // no key draws it any more; freed after a sweep

// One allocation: this header, then the rle565 data
struct ImageData
{
    uint32_t size;          // bytes of rle565 data
    uint32_t flags;         // IMAGE_*. IMAGE_RETIRED is read by the compositor task (port::Atomic*)
    uint32_t sweep;         // retired: the sweep generation it waits for
    uint32_t reserved;

    const void *Pixels() const { return this + 1; }       // 4-byte aligned if the allocation is
    uint8_t    *Bytes()        { return (uint8_t *)(this + 1); }
    bool        Opaque() const { return (flags & IMAGE_OPAQUE) != 0; }
    static const ImageData *FromPixels(const void *px) { return (const ImageData *)px - 1; }
};
static_assert(sizeof(ImageData) == 16, "ImageData keeps the pixels 16-byte aligned");

// What an ID names
struct ImageEntry
{
    uint32_t   id;
    ImageData *data;        // current pixels
    uint32_t   key_refs;    // key slots using it
    bool       live;        // in the ID map (false = dying: freed while keys use it)
};

enum class ImageResult
{
    Ok,
    BadArg,         // ID 0, size 0, or a chunk past the end
    NoSpace,
    BadImage,       // failed the checks
    BadSequence,    // chunk out of order, or no upload started
};

// Services the store needs from its platform
struct ImageStoreHost
{
    void    *(*alloc)(size_t size, void *ctx);     // 16-byte aligned, PSRAM on the device; NULL if none
    void     (*free)(void *p, void *ctx);
    size_t   (*largest_free)(void *ctx);           // largest allocation possible now
    uint32_t (*request_sweep)(void *ctx);          // comp_request_sweep
    uint32_t (*swept)(void *ctx);                  // comp_swept
    void     *ctx;
};

// Told when an image is replaced in place: every key using `entry` must
// switch to entry->data now. Called before the old pixels are retired.
class ImageStoreListener
{
public:
    virtual void OnImageReplaced(ImageEntry *entry) = 0;
};

struct ImageStoreStats
{
    uint32_t capacity;      // bytes
    uint32_t used;          // bytes allocated, including retired pixels and an upload in progress
    uint32_t count;         // live images
    uint32_t largest_free;
    uint32_t retired;       // allocations waiting for a sweep
};

class ImageStore
{
public:
    ~ImageStore();

    // Images must be `width` x `height`. At most `capacity` bytes are
    // allocated (headers included).
    void Init(const ImageStoreHost &host, uint32_t capacity, uint32_t width, uint32_t height);
    void SetListener(ImageStoreListener *listener) { m_listener = listener; }

    // --- Uploads (PUT_IMAGE) ---
    // A chunk: PutBegin() with its header, PutData() with its `len` bytes
    // (in any number of pieces), then PutEnd(). If PutBegin() fails, skip
    // the data and don't call PutEnd(). Starting at offset 0 abandons any
    // upload in progress. A failed chunk abandons the upload.
    ImageResult PutBegin(uint32_t id, uint32_t total_size, uint32_t offset, uint32_t len);
    void        PutData(const void *data, uint32_t len);
    // Ok if the image needs more chunks, or once it's live (new or replaced)
    ImageResult PutEnd();
    void        AbortUpload();

    // A whole image from memory in one chunk (the device's own images)
    ImageResult Put(uint32_t id, const void *data, uint32_t size);

    // --- IDs ---
    ImageEntry *Find(uint32_t id) const;           // live images only
    bool        IsLive(uint32_t id) const { return Find(id) != nullptr; }

    // FREE_IMAGES: the ID goes at once; the memory once no key uses it and
    // a sweep has run. Unknown IDs are ignored.
    void Free(uint32_t id);

    // Key slots taking and dropping an entry. The last Release() of a dying
    // entry retires its pixels and deletes it.
    void AddRef(ImageEntry *entry) { entry->key_refs++; }
    void Release(ImageEntry *entry);

    // Free retired pixels whose sweep has run. Call often (after every
    // message, and on a timer while HasRetired()).
    void Collect();
    bool HasRetired() const { return m_retired.GetCount() > 0; }

    // RESET: free every image. The compositor must hold no references
    // (comp_reset() has returned) and no key may use any entry.
    void Reset();

    void GetStats(ImageStoreStats *stats) const;

    // comp_is_retired_t: are these pixels (ImageData::Pixels()) retired?
    // Safe on the compositor task.
    static bool IsRetired(const void *pixels, void *ctx = nullptr);

private:
    ImageData *Allocate(uint32_t size);
    void       Deallocate(ImageData *data);
    void       Retire(ImageData *data);
    bool       Check(ImageData *data) const;
    void       Install(uint32_t id, ImageData *data);

    ImageStoreHost      m_host = {};
    ImageStoreListener *m_listener = nullptr;
    uint32_t            m_capacity = 0, m_used = 0;
    uint32_t            m_width = 0, m_height = 0;

    SimpleLib::Map<uint32_t, ImageEntry *> m_live;
    SimpleLib::List<ImageEntry *>          m_dying;
    SimpleLib::List<ImageData *>           m_retired;

    struct
    {
        bool       active;
        uint32_t   id;
        uint32_t   total;
        uint32_t   received;        // bytes written so far
        uint32_t   chunk_end;       // where the current chunk ends
        ImageData *data;
    } m_upload = {};
};
