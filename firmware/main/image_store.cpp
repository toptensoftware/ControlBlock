// Image store. Spec: docs/image-assets.md.

#include <assert.h>
#include <string.h>
#include "rle565.h"
#include "image_store.h"

using namespace SimpleLib;

// Sweep generations wrap; compare them as a signed difference
static inline bool sweep_done(uint32_t swept, uint32_t gen)
{
    return (int32_t)(swept - gen) >= 0;
}

ImageStore::~ImageStore()
{
    Reset();
}

void ImageStore::Init(const ImageStoreHost &host, uint32_t capacity, uint32_t width, uint32_t height)
{
    m_host     = host;
    m_capacity = capacity;
    m_width    = width;
    m_height   = height;
}

// ===========================================================================
// Memory
// ===========================================================================

ImageData *ImageStore::Allocate(uint32_t size)
{
    uint64_t bytes = (uint64_t)sizeof(ImageData) + size;
    if (m_used + bytes > m_capacity)
        return nullptr;
    ImageData *d = (ImageData *)m_host.alloc((size_t)bytes, m_host.ctx);
    if (!d)
        return nullptr;
    memset(d, 0, sizeof(*d));
    d->size = size;
    m_used += (uint32_t)bytes;
    return d;
}

void ImageStore::Deallocate(ImageData *data)
{
    m_used -= (uint32_t)(sizeof(ImageData) + data->size);
    m_host.free(data, m_host.ctx);
}

// No key draws `data` any more. It's freed once the compositor has swept:
// flag it first, then ask, so the sweep sees the flag.
void ImageStore::Retire(ImageData *data)
{
    port::AtomicOr(&data->flags, IMAGE_RETIRED);
    data->sweep = m_host.request_sweep(m_host.ctx);
    m_retired.Add(data);
}

bool ImageStore::IsRetired(const void *pixels, void *)
{
    const ImageData *d = ImageData::FromPixels(pixels);
    return (port::AtomicLoad(&d->flags) & IMAGE_RETIRED) != 0;
}

void ImageStore::Collect()
{
    if (!m_retired.GetCount())
        return;
    uint32_t swept = m_host.swept(m_host.ctx);
    for (int i = m_retired.GetCount() - 1; i >= 0; i--) {
        ImageData *d = m_retired[i];
        if (sweep_done(swept, d->sweep)) {
            m_retired.RemoveAt(i);
            Deallocate(d);
        }
    }
}

// ===========================================================================
// Uploads
// ===========================================================================

void ImageStore::AbortUpload()
{
    if (m_upload.data)
        Deallocate(m_upload.data);
    m_upload = {};
}

ImageResult ImageStore::PutBegin(uint32_t id, uint32_t total_size, uint32_t offset, uint32_t len)
{
    if (id == IMAGE_ID_NONE || total_size == 0 || (uint64_t)offset + len > total_size)
        return ImageResult::BadArg;

    if (offset == 0) {
        AbortUpload();
        ImageData *d = Allocate(total_size);
        if (!d)
            return ImageResult::NoSpace;
        m_upload.active = true;
        m_upload.id     = id;
        m_upload.total  = total_size;
        m_upload.data   = d;
    } else if (!m_upload.active || m_upload.id != id || m_upload.total != total_size ||
               m_upload.received != offset) {
        AbortUpload();
        return ImageResult::BadSequence;
    }

    m_upload.chunk_end = offset + len;
    return ImageResult::Ok;
}

void ImageStore::PutData(const void *data, uint32_t len)
{
    if (!m_upload.active)
        return;
    assert(m_upload.received + len <= m_upload.chunk_end);
    if (m_upload.received + len > m_upload.chunk_end)
        len = m_upload.chunk_end - m_upload.received;
    memcpy(m_upload.data->Bytes() + m_upload.received, data, len);
    m_upload.received += len;
}

ImageResult ImageStore::PutEnd()
{
    if (!m_upload.active)
        return ImageResult::Ok;         // abandoned meanwhile (Free)
    if (m_upload.received < m_upload.total)
        return ImageResult::Ok;         // more chunks to come

    ImageData *d  = m_upload.data;
    uint32_t   id = m_upload.id;
    m_upload = {};
    if (!Check(d)) {
        Deallocate(d);
        return ImageResult::BadImage;
    }
    Install(id, d);
    return ImageResult::Ok;
}

ImageResult ImageStore::Put(uint32_t id, const void *data, uint32_t size)
{
    ImageResult r = PutBegin(id, size, 0, size);
    if (r != ImageResult::Ok)
        return r;
    PutData(data, size);
    return PutEnd();
}

// The whole image, bounds checked: right size, every row's runs valid.
// Bytes after the image (padding) are allowed. Reads the data only, so it
// adds no PSRAM writes.
bool ImageStore::Check(ImageData *d) const
{
    const void *px = d->Pixels();
    bool opaque = false;
    if (!check_image_header(px, d->size) ||
        get_image_width(px) != m_width || get_image_height(px) != m_height ||
        !check_image(px, d->size, &opaque))
        return false;
    if (opaque)
        d->flags |= IMAGE_OPAQUE;
    return true;
}

// A checked image becomes `id`'s pixels: a new entry, or a replace
void ImageStore::Install(uint32_t id, ImageData *data)
{
    ImageEntry *e = Find(id);
    if (!e) {
        e = new ImageEntry{ id, data, 0, true };
        m_live.Add(id, e);
        return;
    }

    // Replace: keys switch to the new pixels before the old ones retire
    ImageData *old = e->data;
    e->data = data;
    if (m_listener)
        m_listener->OnImageReplaced(e);
    Retire(old);
}

// ===========================================================================
// IDs and references
// ===========================================================================

ImageEntry *ImageStore::Find(uint32_t id) const
{
    ImageEntry *e = nullptr;
    return m_live.TryGetValue(id, e) ? e : nullptr;
}

void ImageStore::Free(uint32_t id)
{
    if (m_upload.active && m_upload.id == id)
        AbortUpload();

    ImageEntry *e = Find(id);
    if (!e)
        return;
    m_live.Remove(id);
    e->live = false;
    if (e->key_refs == 0) {
        Retire(e->data);
        delete e;
    } else {
        m_dying.Add(e);
    }
}

void ImageStore::Release(ImageEntry *e)
{
    assert(e->key_refs > 0);
    if (--e->key_refs || e->live)
        return;
    m_dying.Remove(e);
    Retire(e->data);
    delete e;
}

void ImageStore::Reset()
{
    AbortUpload();
    for (auto it = m_live.Iterate(); it.Next(); ) {
        Deallocate(it.GetValue()->data);
        delete it.GetValue();
    }
    m_live.Clear();
    for (int i = 0; i < m_dying.GetCount(); i++) {
        Deallocate(m_dying[i]->data);
        delete m_dying[i];
    }
    m_dying.Clear();
    for (int i = 0; i < m_retired.GetCount(); i++)
        Deallocate(m_retired[i]);
    m_retired.Clear();
}

void ImageStore::GetStats(ImageStoreStats *st) const
{
    st->capacity     = m_capacity;
    st->used         = m_used;
    st->count        = (uint32_t)m_live.GetCount();
    st->retired      = (uint32_t)m_retired.GetCount();
    size_t largest   = m_host.largest_free ? m_host.largest_free(m_host.ctx) : 0;
    uint32_t room    = m_capacity - m_used;
    st->largest_free = largest < room ? (uint32_t)largest : room;
}
