// Image store: uploads, replace, deferred free and sweeps
// (docs/image-assets.md)

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "rle565.h"
#include "store_env.h"

using namespace SimpleLib;

#define CAPACITY (1u << 20)

namespace
{
    struct Fixture
    {
        StoreEnv   env;
        ImageStore store;

        explicit Fixture(uint32_t capacity = CAPACITY)
        {
            store.Init(env.Host(), capacity, COMP_KEY_SIZE, COMP_KEY_SIZE);
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

    // A valid image of the wrong size (a 24x6 pager pill)
    static BLOBPACK *ui = LoadAsset("ui.bin");
    const void *pill = get_blob(ui, 1);
    Assert(check_image(pill, get_blob_size(ui, 1), nullptr));
    Assert(f.store.PutBegin(9, get_image_packed_size(pill), 0, get_image_packed_size(pill)) == ImageResult::Ok);
    f.store.PutData(pill, get_image_packed_size(pill));
    Assert(f.store.PutEnd() == ImageResult::BadImage);
    Assert(f.env.LiveBlocks() == 0);
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
