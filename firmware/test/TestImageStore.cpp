// Image store: uploads, replace, deferred free and sweeps
// (docs/image-assets.md)

#include <stdlib.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "rle565.h"
#include "store_env.h"
#include "key_geometry.h"

using namespace SimpleLib;

#define CAPACITY (1u << 20)

namespace
{
    // The store takes images up to max_w x max_h; require_opaque refuses any
    // with a transparent pixel. The default is the interim 128 policy the
    // model still uses (any opacity).
    struct Fixture
    {
        StoreEnv   env;
        ImageStore store;

        explicit Fixture(uint32_t capacity = CAPACITY, uint32_t max_w = TEST_KEYS_FACE_SIZE,
                         uint32_t max_h = TEST_KEYS_FACE_SIZE, bool require_opaque = false)
        {
            store.Init(env.Host(), capacity, max_w, max_h, require_opaque);
        }

        // The sweep runs, then the store frees what it can
        void SweepAndCollect()
        {
            env.Sweep();
            store.Collect();
        }
    };

    struct Listener : ImageStoreListener
    {
        List<ImageEntry *> replaced;
        void OnImageReplaced(ImageEntry *entry) override { replaced.Add(entry); }
    };

    bool SamePixels(const ImageEntry *e, const void *img)
    {
        return e && e->data->size == get_image_packed_size(img) &&
               memcmp(e->data->Pixels(), img, e->data->size) == 0;
    }

    // A copy of `img` whose header is fine but whose first row's runs point
    // past the end of the data
    List<uint8_t> Corrupt(const void *img)
    {
        uint32_t size = get_image_packed_size(img);
        List<uint8_t> bytes;
        for (uint32_t i = 0; i < size; i++)
            bytes.Add(((const uint8_t *)img)[i]);
        uint32_t *row_offsets = (uint32_t *)(bytes.GetBuffer() + sizeof(IMAGE_RLE));
        row_offsets[0] = size - 1;
        bytes.GetBuffer()[size - 1] = 0xFF;     // a run count that needs more bytes
        return bytes;
    }
}

Fact("ImageStore Upload In One Chunk")
{
    Fixture f;
    const void *img = f.env.Icon(0);
    Assert(f.env.Upload(f.store, 42, img) == ImageResult::Ok);

    ImageEntry *e = f.store.Find(42);
    Assert(e && e->id == 42 && e->live && e->key_refs == 0);
    Assert(SamePixels(e, img));
    Assert(((uintptr_t)e->data->Pixels() & 3) == 0);
    Assert(!e->data->Opaque());
    Assert(!f.store.IsLive(43));

    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 1 && st.capacity == CAPACITY);
    Assert(st.used == sizeof(ImageData) + f.env.Size(img));
}

Fact("ImageStore Upload In Chunks And Pieces")
{
    Fixture f;
    const uint8_t *img = (const uint8_t *)f.env.Face(1);
    uint32_t size = f.env.Size(img);

    // Three chunks, each written in two pieces
    uint32_t cuts[] = { 0, size / 3, size * 2 / 3, size };
    for (int c = 0; c < 3; c++) {
        uint32_t len = cuts[c + 1] - cuts[c];
        Assert(f.store.PutBegin(7, size, cuts[c], len) == ImageResult::Ok);
        f.store.PutData(img + cuts[c], len / 2);
        f.store.PutData(img + cuts[c] + len / 2, len - len / 2);
        Assert(!f.store.IsLive(7) || c == 2);
        Assert(f.store.PutEnd() == ImageResult::Ok);
    }
    Assert(SamePixels(f.store.Find(7), img));
}

Fact("ImageStore Bad Arguments")
{
    Fixture f;
    Assert(f.store.PutBegin(IMAGE_ID_NONE, 100, 0, 100) == ImageResult::BadArg);
    Assert(f.store.PutBegin(1, 0, 0, 0) == ImageResult::BadArg);
    Assert(f.store.PutBegin(1, 100, 0, 101) == ImageResult::BadArg);
    Assert(f.env.LiveBlocks() == 0);
}

Fact("ImageStore Chunks Out Of Order Abandon The Upload")
{
    Fixture f;
    const uint8_t *img = (const uint8_t *)f.env.Icon(1);
    uint32_t size = f.env.Size(img);

    // No upload started
    Assert(f.store.PutBegin(5, size, 100, 100) == ImageResult::BadSequence);

    // A gap
    Assert(f.store.PutBegin(5, size, 0, 100) == ImageResult::Ok);
    f.store.PutData(img, 100);
    Assert(f.store.PutEnd() == ImageResult::Ok);
    Assert(f.store.PutBegin(5, size, 200, 100) == ImageResult::BadSequence);
    Assert(f.env.LiveBlocks() == 0);        // abandoned

    // ...so carrying on from where it was fails too
    Assert(f.store.PutBegin(5, size, 100, 100) == ImageResult::BadSequence);

    // A different ID or size mid-upload
    Assert(f.store.PutBegin(5, size, 0, 100) == ImageResult::Ok);
    f.store.PutData(img, 100);
    Assert(f.store.PutEnd() == ImageResult::Ok);
    Assert(f.store.PutBegin(6, size, 100, 100) == ImageResult::BadSequence);
    Assert(!f.store.IsLive(5) && !f.store.IsLive(6));
}

Fact("ImageStore A New Upload Abandons The Old One")
{
    Fixture f;
    const uint8_t *a = (const uint8_t *)f.env.Icon(2);
    Assert(f.store.PutBegin(1, f.env.Size(a), 0, 64) == ImageResult::Ok);
    f.store.PutData(a, 64);
    Assert(f.store.PutEnd() == ImageResult::Ok);

    Assert(f.env.Upload(f.store, 2, f.env.Icon(3)) == ImageResult::Ok);
    Assert(!f.store.IsLive(1) && f.store.IsLive(2));
    Assert(f.env.LiveBlocks() == 1);
}

Fact("ImageStore Rejects Bad Images")
{
    Fixture f;
    List<uint8_t> bad = Corrupt(f.env.Icon(0));
    Assert(check_image_header(bad.GetBuffer(), bad.GetCount()));     // only the runs are wrong
    Assert(f.env.Upload(f.store, 9, bad.GetBuffer()) == ImageResult::BadImage);
    Assert(!f.store.IsLive(9));
    Assert(f.env.LiveBlocks() == 0);

    // Garbage
    static const uint32_t junk[64] = { 0x12345678 };
    Assert(f.store.PutBegin(9, sizeof(junk), 0, sizeof(junk)) == ImageResult::Ok);
    f.store.PutData(junk, sizeof(junk));
    Assert(f.store.PutEnd() == ImageResult::BadImage);

    // A valid image larger than the maximum (a 144 key in a 128 store)
    Assert(get_image_width(f.env.Key1U()) > TEST_KEYS_FACE_SIZE);
    Assert(f.env.Upload(f.store, 9, f.env.Key1U()) == ImageResult::BadImage);
    Assert(f.env.LiveBlocks() == 0);
}

Fact("ImageStore Stats Count The Checks Of Finished Uploads")
{
    // (the stats line's check time: wip/wip-upload-throughput.md; cycles read 0 on the host)
    Fixture f;
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.checks == 0 && st.check_cycles == 0);

    Assert(f.env.Upload(f.store, 1, f.env.Icon(0)) == ImageResult::Ok);
    List<uint8_t> bad = Corrupt(f.env.Icon(0));
    Assert(f.env.Upload(f.store, 2, bad.GetBuffer()) == ImageResult::BadImage);
    Assert(f.store.PutBegin(3, 0, 0, 0) == ImageResult::BadArg);        // refused before any data: no check
    f.store.GetStats(&st);
    Assert(st.checks == 2);
}

// ---------------------------------------------------------------------------
// Sizes and opacity: any size up to the maximum, opaque only if required
// ---------------------------------------------------------------------------

Fact("ImageStore Accepts Any Size Up To The Maximum")
{
    Fixture f(CAPACITY, KEY_GRID_W, KEY_GRID_H, true);
    Assert(f.env.Upload(f.store, 1, f.env.Key1U(0)) == ImageResult::Ok);
    Assert(f.env.Upload(f.store, 2, f.env.Key2U(0)) == ImageResult::Ok);
    Assert(f.env.Upload(f.store, 3, f.env.Face(0, true)) == ImageResult::Ok);     // 128, opaque

    // A small image (a 24x6 pager pill) in a store with no opacity rule
    Fixture g;
    static BLOBPACK *ui = LoadAsset("ui.bin");
    const void *pill = get_blob(ui, 1);
    Assert(check_image(pill, get_blob_size(ui, 1), nullptr));
    Assert(g.env.Upload(g.store, 9, pill) == ImageResult::Ok);
}

Fact("ImageStore Refuses Images Wider Or Taller Than The Maximum")
{
    Fixture narrow(CAPACITY, KEY_CELL, KEY_CELL, true);
    Assert(narrow.env.Upload(narrow.store, 1, narrow.env.Key1U(0)) == ImageResult::Ok);     // exactly the maximum
    Assert(narrow.env.Upload(narrow.store, 2, narrow.env.Key2U(0)) == ImageResult::BadImage);
    Assert(!narrow.store.IsLive(2));

    Fixture low(CAPACITY, 2 * KEY_CELL, KEY_CELL - 1, true);
    Assert(low.env.Upload(low.store, 1, low.env.Key1U(0)) == ImageResult::BadImage);
    Assert(low.env.Upload(low.store, 2, low.env.Key2U(0)) == ImageResult::BadImage);
    Assert(low.env.LiveBlocks() == 0);

    // External images too
    Fixture ext(CAPACITY, KEY_CELL, KEY_CELL, true);
    uint32_t size = get_image_packed_size(ext.env.Key2U(0));
    void *copy = malloc(size);
    memcpy(copy, ext.env.Key2U(0), size);
    Assert(ext.store.AddExternal(1, copy, size) == ImageResult::BadImage);
    Assert(!ext.store.IsLive(1));
    free(copy);
}

Fact("ImageStore Refuses Transparent Images When Opaque Is Required")
{
    Fixture f(CAPACITY, KEY_GRID_W, KEY_GRID_H, true);
    Assert(f.env.Upload(f.store, 1, f.env.Face(0, false)) == ImageResult::BadImage);
    Assert(f.env.Upload(f.store, 2, f.env.Icon(0)) == ImageResult::BadImage);
    Assert(!f.store.IsLive(1) && !f.store.IsLive(2));
    Assert(f.env.LiveBlocks() == 0);

    // A refused replace keeps the old image
    Assert(f.env.Upload(f.store, 3, f.env.Key1U(0)) == ImageResult::Ok);
    Assert(f.env.Upload(f.store, 3, f.env.Icon(1)) == ImageResult::BadImage);
    Assert(f.store.Find(3)->opaque && f.store.Find(3)->width == KEY_CELL);

    // External images too
    uint32_t size = get_image_packed_size(f.env.Icon(0));
    void *copy = malloc(size);
    memcpy(copy, f.env.Icon(0), size);
    Assert(f.store.AddExternal(4, copy, size) == ImageResult::BadImage);
    Assert(!f.store.IsLive(4));
    free(copy);
}

Fact("ImageStore Entries Expose The Image Size")
{
    Fixture f(CAPACITY, KEY_GRID_W, KEY_GRID_H, true);
    Listener l;
    f.store.SetListener(&l);
    Assert(f.env.Upload(f.store, 1, f.env.Key1U(1)) == ImageResult::Ok);
    ImageEntry *e = f.store.Find(1);
    Assert(e->width == KEY_CELL && e->height == KEY_CELL && e->opaque);

    // A replace may change the size; the listener sees the new one
    Assert(f.env.Upload(f.store, 1, f.env.Key2U(1)) == ImageResult::Ok);
    Assert(f.store.Find(1) == e && l.replaced.GetCount() == 1);
    Assert(e->width == 2 * KEY_CELL && e->height == KEY_CELL);

    // External
    uint32_t size = get_image_packed_size(f.env.Key2U(2));
    void *copy = malloc(size);
    memcpy(copy, f.env.Key2U(2), size);
    Assert(f.store.AddExternal(2, copy, size) == ImageResult::Ok);
    ImageEntry *x = f.store.Find(2);
    Assert(x->external && x->width == 2 * KEY_CELL && x->height == KEY_CELL && x->opaque);
    f.store.Reset();
    free(copy);
}

Fact("ImageStore Accepts Padding After The Image")
{
    Fixture f;
    const void *img = f.env.Icon(0);
    uint32_t size = f.env.Size(img);
    List<uint8_t> padded;
    for (uint32_t i = 0; i < size; i++)
        padded.Add(((const uint8_t *)img)[i]);
    padded.Add(0);
    padded.Add(0);
    Assert(f.store.Put(1, padded.GetBuffer(), (uint32_t)padded.GetCount()) == ImageResult::Ok);
    Assert(get_image_packed_size(f.store.Find(1)->data->Pixels()) == size);
}

Fact("ImageStore Flags Opaque Images")
{
    Fixture f;
    Assert(f.env.Upload(f.store, 1, f.env.Face(0, true)) == ImageResult::Ok);
    Assert(f.env.Upload(f.store, 2, f.env.Face(0, false)) == ImageResult::Ok);
    Assert(f.store.Find(1)->data->Opaque());
    Assert(!f.store.Find(2)->data->Opaque());
}

Fact("ImageStore No Space")
{
    // Over capacity
    Fixture small(100);
    uint32_t size = small.env.Size(small.env.Face(0));
    Assert(size > 100);
    Assert(small.store.PutBegin(1, size, 0, 100) == ImageResult::NoSpace);

    // The allocator has nothing
    Fixture g;
    g.env.fail_alloc = true;
    Assert(g.env.Upload(g.store, 1, g.env.Icon(0)) == ImageResult::NoSpace);
    Assert(!g.store.IsLive(1));
}

Fact("ImageStore Replace Switches Keys Then Retires The Old Pixels")
{
    Fixture f;
    Listener l;
    f.store.SetListener(&l);
    Assert(f.env.Upload(f.store, 3, f.env.Icon(0)) == ImageResult::Ok);
    ImageEntry *e = f.store.Find(3);
    ImageData *old = e->data;
    const void *old_px = old->Pixels();
    f.store.AddRef(e);

    Assert(f.env.Upload(f.store, 3, f.env.Icon(1)) == ImageResult::Ok);
    Assert(f.store.Find(3) == e);           // same entry, new pixels
    Assert(SamePixels(e, f.env.Icon(1)));
    Assert(l.replaced.GetCount() == 1 && l.replaced[0] == e);
    Assert(e->key_refs == 1);

    // The old pixels wait for a sweep
    Assert(ImageStore::IsRetired(old_px));
    Assert(!ImageStore::IsRetired(e->data->Pixels()));
    Assert(f.store.HasRetired());
    f.store.Collect();
    Assert(f.env.IsAllocated(old_px));      // not swept yet
    f.SweepAndCollect();
    Assert(!f.env.IsAllocated(old_px));
    Assert(!f.store.HasRetired());
}

Fact("ImageStore Failed Replace Keeps The Old Image")
{
    Fixture f;
    Listener l;
    f.store.SetListener(&l);
    Assert(f.env.Upload(f.store, 3, f.env.Icon(0)) == ImageResult::Ok);

    List<uint8_t> bad = Corrupt(f.env.Icon(1));
    Assert(f.env.Upload(f.store, 3, bad.GetBuffer()) == ImageResult::BadImage);
    Assert(SamePixels(f.store.Find(3), f.env.Icon(0)));

    f.env.fail_alloc = true;
    Assert(f.env.Upload(f.store, 3, f.env.Icon(1)) == ImageResult::NoSpace);
    Assert(SamePixels(f.store.Find(3), f.env.Icon(0)));
    Assert(l.replaced.GetCount() == 0);
    Assert(!f.store.HasRetired());
}

Fact("ImageStore Free Unused Image")
{
    Fixture f;
    Assert(f.env.Upload(f.store, 8, f.env.Icon(4)) == ImageResult::Ok);
    const void *px = f.store.Find(8)->data->Pixels();
    f.store.Free(8);
    Assert(!f.store.IsLive(8));
    Assert(ImageStore::IsRetired(px));
    Assert(f.env.IsAllocated(px));          // the compositor may still hold it
    f.SweepAndCollect();
    Assert(!f.env.IsAllocated(px));

    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 0 && st.used == 0 && st.retired == 0);
}

Fact("ImageStore Free In Use Image Waits For Its Last Key")
{
    Fixture f;
    Assert(f.env.Upload(f.store, 8, f.env.Icon(4)) == ImageResult::Ok);
    ImageEntry *e = f.store.Find(8);
    f.store.AddRef(e);
    f.store.AddRef(e);

    f.store.Free(8);
    Assert(!f.store.IsLive(8));             // gone from the ID map
    Assert(!e->live);
    Assert(!ImageStore::IsRetired(e->data->Pixels()));
    f.SweepAndCollect();
    Assert(SamePixels(e, f.env.Icon(4)));   // still drawn by its keys

    const void *px = e->data->Pixels();
    f.store.Release(e);
    Assert(!ImageStore::IsRetired(px));
    f.store.Release(e);                     // the last key: entry deleted, pixels retired
    Assert(ImageStore::IsRetired(px));
    f.SweepAndCollect();
    Assert(!f.env.IsAllocated(px));
    Assert(f.env.LiveBlocks() == 0);
}

Fact("ImageStore Releasing A Live Image Keeps It")
{
    Fixture f;
    Assert(f.env.Upload(f.store, 8, f.env.Icon(4)) == ImageResult::Ok);
    ImageEntry *e = f.store.Find(8);
    f.store.AddRef(e);
    f.store.Release(e);
    Assert(f.store.IsLive(8));
    Assert(!f.store.HasRetired());
}

Fact("ImageStore Free Is Idempotent")
{
    Fixture f;
    f.store.Free(123);
    Assert(f.env.Upload(f.store, 8, f.env.Icon(4)) == ImageResult::Ok);
    f.store.Free(8);
    f.store.Free(8);
    f.SweepAndCollect();
    Assert(f.env.LiveBlocks() == 0);
}

Fact("ImageStore Free During Upload Abandons It")
{
    Fixture f;
    const uint8_t *img = (const uint8_t *)f.env.Icon(5);
    Assert(f.store.PutBegin(4, f.env.Size(img), 0, 64) == ImageResult::Ok);
    f.store.PutData(img, 64);
    Assert(f.store.PutEnd() == ImageResult::Ok);
    f.store.Free(4);
    Assert(f.env.LiveBlocks() == 0);
    Assert(f.store.PutBegin(4, f.env.Size(img), 64, 64) == ImageResult::BadSequence);
}

Fact("ImageStore Re-upload Of A Dying ID Is A New Image")
{
    Fixture f;
    Assert(f.env.Upload(f.store, 8, f.env.Icon(4)) == ImageResult::Ok);
    ImageEntry *old = f.store.Find(8);
    f.store.AddRef(old);
    f.store.Free(8);

    Listener l;
    f.store.SetListener(&l);
    Assert(f.env.Upload(f.store, 8, f.env.Icon(5)) == ImageResult::Ok);
    ImageEntry *now = f.store.Find(8);
    Assert(now && now != old);
    Assert(l.replaced.GetCount() == 0);     // not a replace
    Assert(SamePixels(old, f.env.Icon(4)) && SamePixels(now, f.env.Icon(5)));
    f.store.Release(old);
    f.SweepAndCollect();
    Assert(SamePixels(f.store.Find(8), f.env.Icon(5)));
}

Fact("ImageStore Collect Waits For Each Image's Own Sweep")
{
    Fixture f;
    Assert(f.env.Upload(f.store, 1, f.env.Icon(0)) == ImageResult::Ok);
    Assert(f.env.Upload(f.store, 2, f.env.Icon(1)) == ImageResult::Ok);
    const void *p1 = f.store.Find(1)->data->Pixels();
    const void *p2 = f.store.Find(2)->data->Pixels();

    f.store.Free(1);
    f.env.Sweep();                          // covers image 1 only
    f.store.Free(2);
    f.store.Collect();
    Assert(!f.env.IsAllocated(p1));
    Assert(f.env.IsAllocated(p2));
    f.SweepAndCollect();
    Assert(!f.env.IsAllocated(p2));
}

Fact("ImageStore Reset Frees Everything At Once")
{
    Fixture f;
    Assert(f.env.Upload(f.store, 1, f.env.Icon(0)) == ImageResult::Ok);
    Assert(f.env.Upload(f.store, 2, f.env.Icon(1)) == ImageResult::Ok);
    Assert(f.env.Upload(f.store, 3, f.env.Icon(2)) == ImageResult::Ok);
    f.store.AddRef(f.store.Find(2));
    f.store.Free(2);                        // dying
    f.store.Free(3);                        // retired
    Assert(f.store.PutBegin(4, 1000, 0, 10) == ImageResult::Ok);    // mid-upload

    f.store.Reset();
    Assert(f.env.LiveBlocks() == 0);
    Assert(!f.store.IsLive(1) && !f.store.HasRetired());
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 0 && st.used == 0);
    Assert(f.env.Upload(f.store, 1, f.env.Icon(0)) == ImageResult::Ok);
}

// ---------------------------------------------------------------------------
// External images: pointers into the layout blob (docs/standalone.md)
// ---------------------------------------------------------------------------

namespace
{
    // An image in memory the store does not own, with 0xFF bytes where an
    // owned image would have its ImageData header (so a header read would see
    // IMAGE_RETIRED set)
    struct Blob
    {
        uint8_t *block;
        const void *px;
        uint32_t size;

        explicit Blob(const void *img)
        {
            size  = get_image_packed_size(img);
            block = (uint8_t *)malloc(16 + size + 16);
            memset(block, 0xFF, 16 + size + 16);
            memcpy(block + 16, img, size);
            px = block + 16;
        }
        ~Blob() { free(block); }
    };
}

Fact("ImageStore AddExternal Uses The Pointer Without Copying")
{
    Fixture f;
    Blob blob(f.env.Icon(0));
    Assert(f.store.AddExternal(1000, blob.px, blob.size) == ImageResult::Ok);

    ImageEntry *e = f.store.Find(1000);
    Assert(e && e->id == 1000 && e->live && e->external && e->key_refs == 0);
    Assert(e->pixels == blob.px && e->data == nullptr && !e->opaque);
    Assert(f.store.IsLive(1000));
    Assert(f.env.LiveBlocks() == 0);

    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 1 && st.used == 0 && st.retired == 0);
}

Fact("ImageStore AddExternal Records Whether The Image Is Opaque")
{
    Fixture f;
    Blob opaque(f.env.Face(1, true)), clear(f.env.Face(0));
    Assert(f.store.AddExternal(1, opaque.px, opaque.size) == ImageResult::Ok);
    Assert(f.store.AddExternal(2, clear.px, clear.size) == ImageResult::Ok);
    Assert(f.store.Find(1)->opaque && !f.store.Find(2)->opaque);
}

Fact("ImageStore AddExternal Checks Its Arguments")
{
    Fixture f;
    Blob blob(f.env.Icon(0));
    Assert(f.store.AddExternal(IMAGE_ID_NONE, blob.px, blob.size) == ImageResult::BadArg);
    Assert(f.store.AddExternal(1, nullptr, blob.size) == ImageResult::BadArg);
    Assert(f.store.AddExternal(1, blob.px, 0) == ImageResult::BadArg);
    Assert(f.store.AddExternal(1, (const uint8_t *)blob.px + 2, blob.size - 2) == ImageResult::BadArg);   // not 4-aligned
    Assert(f.store.AddExternal(1, blob.px, blob.size - 8) == ImageResult::BadImage);
    Assert(f.store.AddExternal(1, blob.px, 4) == ImageResult::BadImage);

    // Garbage
    Blob j(f.env.Icon(0));
    for (int i = 0; i < 64; i++)
        ((uint8_t *)j.px)[i] = (uint8_t)(i * 7 + 3);
    Assert(f.store.AddExternal(1, j.px, j.size) == ImageResult::BadImage);

    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 0 && !f.store.IsLive(1));
}

Fact("ImageStore AddExternal Of The Same Image Again Is Fine")
{
    Fixture f;
    Blob a(f.env.Icon(0)), b(f.env.Icon(0));
    Assert(f.store.AddExternal(7, a.px, a.size) == ImageResult::Ok);
    ImageEntry *e = f.store.Find(7);
    f.store.AddRef(e);
    Assert(f.store.AddExternal(7, a.px, a.size) == ImageResult::Ok);       // the loader dedupes by ID
    Assert(f.store.Find(7) == e && e->key_refs == 1);
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 1);

    // A different image under the same ID is an error, and so is an uploaded ID
    Assert(f.store.AddExternal(7, b.px, b.size) == ImageResult::BadArg);
    Assert(f.env.Upload(f.store, 8, f.env.Icon(1)) == ImageResult::Ok);
    Assert(f.store.AddExternal(8, b.px, b.size) == ImageResult::BadArg);
    f.store.Release(e);
}

Fact("ImageStore External Images Are Never Retired")
{
    Fixture f;
    Blob blob(f.env.Icon(2));
    Assert(f.store.AddExternal(5, blob.px, blob.size) == ImageResult::Ok);
    // The bytes before the pixels say "retired", but those are not a header
    Assert(ImageStore::IsRetired(blob.px));
    Assert(!f.store.PixelsRetired(blob.px));

    // Owned images still are, once retired
    Assert(f.env.Upload(f.store, 6, f.env.Icon(3)) == ImageResult::Ok);
    const void *owned = f.store.Find(6)->pixels;
    Assert(!f.store.PixelsRetired(owned));
    f.store.Free(6);
    Assert(f.store.PixelsRetired(owned));
    Assert(!f.store.PixelsRetired(blob.px));
}

Fact("ImageStore Freeing An External Image Frees Nothing")
{
    Fixture f;
    Blob blob(f.env.Icon(0));
    Assert(f.store.AddExternal(5, blob.px, blob.size) == ImageResult::Ok);
    f.store.Free(5);
    Assert(!f.store.IsLive(5) && !f.store.HasRetired());
    Assert(f.env.sweeps_requested == 0 && f.env.LiveBlocks() == 0);

    // Used by a key when freed: dies on the last release
    Assert(f.store.AddExternal(5, blob.px, blob.size) == ImageResult::Ok);
    ImageEntry *e = f.store.Find(5);
    f.store.AddRef(e);
    f.store.Free(5);
    f.store.Release(e);
    Assert(!f.store.HasRetired() && f.env.sweeps_requested == 0);
    Assert(get_image_width(blob.px) == TEST_KEYS_FACE_SIZE);          // the memory is untouched
}

Fact("ImageStore Reset Drops External Images Without Touching Them")
{
    Fixture f;
    Blob blob(f.env.Icon(0));
    Assert(f.store.AddExternal(5, blob.px, blob.size) == ImageResult::Ok);
    Assert(f.env.Upload(f.store, 6, f.env.Icon(1)) == ImageResult::Ok);
    f.store.Reset();
    Assert(!f.store.IsLive(5) && !f.store.IsLive(6));
    Assert(f.env.LiveBlocks() == 0 && get_image_width(blob.px) == TEST_KEYS_FACE_SIZE);
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 0 && st.used == 0);

    // The same ID can be used again
    Assert(f.store.AddExternal(5, blob.px, blob.size) == ImageResult::Ok);
}

Fact("ImageStore Uploading Over An External ID Replaces It")
{
    Fixture f;
    Blob blob(f.env.Icon(0));
    Assert(f.store.AddExternal(5, blob.px, blob.size) == ImageResult::Ok);
    ImageEntry *e = f.store.Find(5);
    Listener l;
    f.store.SetListener(&l);

    Assert(f.env.Upload(f.store, 5, f.env.Icon(1)) == ImageResult::Ok);
    Assert(f.store.Find(5) == e && l.replaced.GetCount() == 1);
    Assert(!e->external && e->data && e->pixels == e->data->Pixels());
    Assert(SamePixels(e, f.env.Icon(1)));
    Assert(!f.store.HasRetired());              // the old pixels were not ours to retire
    Assert(!f.store.PixelsRetired(e->pixels));

    // And again: now the old pixels are ours
    Assert(f.env.Upload(f.store, 5, f.env.Icon(2)) == ImageResult::Ok);
    Assert(f.store.HasRetired());
}

// RUN_STRESS (main.cpp load_demo_images) uploads the opaque keys with their blob size; the 128
// faces and icons are the host tests' small and transparent images
Fact("ImageStore Accepts Every Test Key Image At Its Blob Size")
{
    static BLOBPACK *pack = LoadAsset("test_keys.bin");
    int first_key = TEST_KEYS_FIRST_KEY(pack->count);
    Assert(first_key > TEST_KEYS_FIRST_ICON);

    // The 128 faces and icons, in a store that takes transparent images
    StoreEnv env;
    ImageStore store;
    store.Init(env.Host(), 1u << 20, TEST_KEYS_FACE_SIZE, TEST_KEYS_FACE_SIZE, false);
    for (int b = 0; b < first_key; b++) {
        Assert(get_image_width(get_blob(pack, b)) == TEST_KEYS_FACE_SIZE);
        Assert(store.Put(0xDE000000u + b, get_blob(pack, b), get_blob_size(pack, b)) == ImageResult::Ok);
    }

    // The opaque keys, under the opaque policy: 1U then 2U
    StoreEnv env2;
    ImageStore opaque;
    opaque.Init(env2.Host(), 1u << 20, KEY_GRID_W, KEY_GRID_H, true);
    for (int i = 0; i < TEST_KEYS_KEYS; i++) {
        int b = first_key + i;
        uint32_t w = i < TEST_KEYS_KEYS_1U ? KEY_CELL : 2 * KEY_CELL;
        Assert(store.Put(0xDE100000u + b, get_blob(pack, b), get_blob_size(pack, b)) == ImageResult::BadImage);
        Assert(opaque.Put(0xDE000000u + b, get_blob(pack, b), get_blob_size(pack, b)) == ImageResult::Ok);
        ImageEntry *e = opaque.Find(0xDE000000u + b);
        Assert(e->opaque && e->width == w && e->height == KEY_CELL);
    }
    Assert(TEST_KEYS_KEY_2U(pack->count, TEST_KEYS_KEYS_2U - 1) == (int)pack->count - 2);
}
