// Layout apply: a validated blob's Home becomes the key layout, its images
// registered with the store in place (docs/standalone.md "Image IDs").

#include "UnitTesting.h"
#include "Core.h"
#include "layout_apply.h"
#include "layout_env.h"
#include "store_env.h"

using namespace SimpleLib;
using namespace layout_env;

namespace
{
    struct Change { int page; uint32_t flags; };

    struct Fixture
    {
        StoreEnv      env;
        ImageStore    store;
        KeyLayout     layout;
        List<Change>  changes;
        List<uint8_t> blob;
        LayoutSummary summary = {};
        const layoutdata::layoutRoot *root = nullptr;      // the blob, relocated after validating (the summary's offsets still index it)

        Fixture()
        {
            store.Init(env.Host(), 1u << 20, COMP_KEY_SIZE, COMP_KEY_SIZE);
            store.SetListener(&layout);
            layout.Init(&store, Changed, this);
            blob = LoadSample();
            Assert(LayoutValidate(blob.GetBuffer(), (uint32_t)blob.GetCount(), &summary) == LayoutError::Ok);
            root = Relocate(blob);
        }

        // Activity 0 is Watch (2 pages), 1 is Listen (1 page)
        bool Activity(int from, int to)
        {
            return LayoutApplyActivity(root, blob.GetBuffer(), from, to, &store, &layout);
        }

        uint32_t IdOf(const layoutdata::key &k, int slot)
        {
            const uint8_t *p = (const uint8_t *)&k;
            uint32_t ptr;
            memcpy(&ptr, p + LayoutKeyImagePtrOffset(slot), 4);
            return ptr ? ptr - (uint32_t)(uintptr_t)blob.GetBuffer() : 0;
        }

        Set<uint32_t> HomeIds()
        {
            Set<uint32_t> ids;
            for (int k = 0; k < COMP_KEYS; k++)
                for (int s = 0; s < KEY_SLOTS; s++)
                    if (summary.home[k][s].offset)
                        ids.Add(summary.home[k][s].offset);
            return ids;
        }

        Set<uint32_t> ActivityIds(int a)
        {
            Set<uint32_t> ids;
            for (uint32_t p = 0; p < root->activities[a].pages_count; p++)
                for (int k = 0; k < COMP_KEYS; k++)
                    for (int s = 0; s < KEY_SLOTS; s++)
                        if (uint32_t id = IdOf(root->activities[a].pages[p].keys[k], s))
                            ids.Add(id);
            return ids;
        }

        // Home plus activity `a` (-1 none): what the store should hold
        int Expected(int a)
        {
            Set<uint32_t> ids = HomeIds();
            if (a >= 0) {
                Set<uint32_t> act = ActivityIds(a);
                for (auto it = act.Iterate(); it.Next(); )
                    ids.Add(it.Get());
            }
            return ids.GetCount();
        }

        int Stored()
        {
            ImageStoreStats st;
            store.GetStats(&st);
            return (int)st.count;
        }

        comp_key_t PageKey(int page, int key)
        {
            comp_key_t keys[COMP_KEYS];
            Assert(layout.GetPage(page, keys));
            return keys[key];
        }

        static void Changed(int page, uint32_t flags, void *ctx)
        {
            ((Fixture *)ctx)->changes.Add(Change{ page, flags });
        }

        bool Apply() { return LayoutApplyHome(summary, blob.GetBuffer(), &store, &layout); }
        const void *At(uint32_t offset) { return blob.GetBuffer() + offset; }

        comp_key_t Key(int key)
        {
            comp_key_t keys[COMP_KEYS];
            Assert(layout.GetPage(0, keys));
            return keys[key];
        }

        int DistinctImages()
        {
            Set<uint32_t> ids;
            for (int k = 0; k < COMP_KEYS; k++)
                for (int s = 0; s < KEY_SLOTS; s++)
                    if (summary.home[k][s].offset)
                        ids.Add(summary.home[k][s].offset);
            return ids.GetCount();
        }
    };
}

Fact("LayoutApply Shows Home's Keys")
{
    Fixture f;
    Assert(f.Apply());

    const LayoutImage (*im)[KEY_SLOTS] = f.summary.home;
    comp_key_t k0 = f.Key(0);
    Assert(k0.bg == f.At(im[0][KEY_SLOT_BG].offset));
    Assert(k0.fg == f.At(im[0][KEY_SLOT_FG].offset));
    Assert(f.layout.IsActive(0, 0) && f.layout.IsActive(0, 4));
    comp_key_t k4 = f.Key(4);
    Assert(k4.bg == f.At(im[4][KEY_SLOT_BG].offset) && k4.fg == nullptr);

    // Keys with no images are empty
    for (int k : { 3, 5, 14 }) {
        Assert(!f.layout.IsActive(0, k));
        Assert(!f.Key(k).bg && !f.Key(k).fg);
    }
    Assert(f.layout.PageCount() == 1);
}

Fact("LayoutApply Registers Each Image Once Under Its Offset")
{
    Fixture f;
    Assert(f.Apply());
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == (uint32_t)f.DistinctImages());
    Assert(st.used == 0 && f.env.LiveBlocks() == 0);          // nothing copied

    for (int k = 0; k < COMP_KEYS; k++) {
        for (int s = 0; s < KEY_SLOTS; s++) {
            uint32_t id = f.summary.home[k][s].offset;
            if (!id)
                continue;
            ImageEntry *e = f.store.Find(id);
            Assert(e && e->external && e->pixels == f.At(id));
        }
    }
    // The face shared by keys 0..2 is one entry held by all of them (normal and pressed)
    uint32_t face = f.summary.home[0][KEY_SLOT_BG].offset;
    Assert(f.store.Find(face)->key_refs == 3);
}

Fact("LayoutApply Pressed Keys Show The Pressed Pair")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.layout.SetPressed(0, 0, true));
    Assert(f.Key(0).bg == f.At(f.summary.home[0][KEY_SLOT_PRESSED_BG].offset));
    Assert(f.layout.SetPressed(0, 0, false));
    Assert(f.Key(0).bg == f.At(f.summary.home[0][KEY_SLOT_BG].offset));
}

Fact("LayoutApply Reports Page 0 Changed With A Flush")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.changes.GetCount() == 1 && f.changes[0].page == 0 && f.changes[0].flags == COMP_FLUSH);
}

Fact("LayoutApply Failure Leaves The Store And Layout Empty")
{
    Fixture f;
    // An image that fails the store check, after earlier images went in
    f.summary.home[7][KEY_SLOT_BG] = f.summary.home[0][KEY_SLOT_BG];
    f.summary.home[7][KEY_SLOT_FG] = LayoutImage{ 700, 64 };
    Assert(!f.Apply());

    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 0);
    Assert(f.changes.GetCount() == 0);
    for (int k = 0; k < COMP_KEYS; k++)
        Assert(!f.layout.IsActive(0, k) && !f.Key(k).bg);
}

Fact("LayoutApply Can Rebuild After A Reset")
{
    Fixture f;
    Assert(f.Apply());
    f.layout.Clear();
    f.store.Reset();
    f.changes.Clear();
    Assert(f.Apply());
    Assert(f.layout.IsActive(0, 0));
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == (uint32_t)f.DistinctImages());
}

Fact("LayoutApply Copes With An Empty Home")
{
    Fixture f;
    f.summary = {};
    Assert(f.Apply());
    for (int k = 0; k < COMP_KEYS; k++)
        Assert(!f.layout.IsActive(0, k));
}

// --- Activity pages (strip pages 1..n) ---

Fact("LayoutApplyActivity Shows The Activity's Pages After Home")
{
    Fixture f;
    Assert(f.Apply());
    f.changes.Clear();
    Assert(f.Activity(-1, 0));                                  // Watch: 2 pages
    Assert(f.layout.PageCount() == 3);
    for (int p = 0; p < 2; p++) {
        const layoutdata::key &src = f.root->activities[0].pages[p].keys[0];
        comp_key_t k = f.PageKey(1 + p, 0);
        Assert(k.bg == f.At(f.IdOf(src, KEY_SLOT_BG)) && k.fg == f.At(f.IdOf(src, KEY_SLOT_FG)));
        Assert(f.layout.IsActive(1 + p, 0));
        for (int key = 1; key < COMP_KEYS; key++)
            Assert(!f.layout.IsActive(1 + p, key));
    }
    // Home is untouched, and no change was reported for it
    Assert(f.Key(0).bg == f.At(f.summary.home[0][KEY_SLOT_BG].offset));
    for (int i = 0; i < f.changes.GetCount(); i++)
        Assert(f.changes[i].page != 0);
    Assert(f.Stored() == f.Expected(0));
}

Fact("LayoutApplyActivity Shares Images With Home Without Registering Twice")
{
    Fixture f;
    Assert(f.Apply());
    Set<uint32_t> home = f.HomeIds();
    Set<uint32_t> listen = f.ActivityIds(1);
    int shared = 0;
    for (auto it = listen.Iterate(); it.Next(); )
        shared += home.Contains(it.Get());
    Assert(shared > 0);                                         // the sample really shares one
    Assert(f.Activity(-1, 1));
    Assert(f.Stored() == f.Expected(1));
}

Fact("LayoutApplyActivity Shrinks And Grows The Page Count Without Leaking Keys")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.Activity(-1, 0) && f.layout.PageCount() == 3);
    Assert(f.Activity(0, 1));                                   // Listen: 1 page
    Assert(f.layout.PageCount() == 2);
    comp_key_t keys[COMP_KEYS];
    Assert(!f.layout.GetPage(2, keys));
    const layoutdata::key &src = f.root->activities[1].pages[0].keys[0];
    Assert(f.PageKey(1, 0).bg == f.At(f.IdOf(src, KEY_SLOT_BG)));
    Assert(f.Stored() == f.Expected(1));

    Assert(f.Activity(1, 0) && f.layout.PageCount() == 3);
    Assert(f.layout.IsActive(2, 0));
    Assert(f.Stored() == f.Expected(0));
}

Fact("LayoutApplyActivity None Collapses To Home And Frees The Activity's Images")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.Activity(-1, 0));
    Assert(f.Activity(0, -1));
    Assert(f.layout.PageCount() == 1);
    Assert(f.Stored() == f.Expected(-1));
    Assert(f.Stored() == f.DistinctImages());
    Assert(f.Activity(-1, -1));                                 // nothing to do, still fine
    Assert(f.layout.PageCount() == 1);
}

Fact("LayoutApplyActivity Switching Back And Forth Does Not Leak Entries")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.Activity(-1, 0));
    int from = 0;
    for (int i = 0; i < 6; i++) {
        int to = from == 0 ? 1 : 0;
        Assert(f.Activity(from, to));
        Assert(f.Stored() == f.Expected(to));
        from = to;
    }
    Assert(f.Activity(from, from));                             // the same again: no change in entries
    Assert(f.Stored() == f.Expected(from));
}

Fact("LayoutApplyActivity Frees Only Unshared Images, Keys In Use Keep Theirs")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.Activity(-1, 0));
    Set<uint32_t> home = f.HomeIds();
    Set<uint32_t> watch = f.ActivityIds(0);
    Assert(f.Activity(0, -1));
    // Everything Home uses is still live and still on its keys
    for (auto it = home.Iterate(); it.Next(); )
        Assert(f.store.IsLive(it.Get()));
    for (auto it = watch.Iterate(); it.Next(); )
        Assert(f.store.IsLive(it.Get()) == home.Contains(it.Get()));
    Assert(f.Key(0).bg == f.At(f.summary.home[0][KEY_SLOT_BG].offset));
}

Fact("LayoutApplyActivity Refusal Leaves Store Layout And Page Count As They Were")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.Activity(-1, 1));                                  // Listen is showing
    int stored = f.Stored();
    comp_key_t before = f.PageKey(1, 0);
    f.changes.Clear();

    // Watch's second page gets an image the store refuses, after earlier images went in
    layoutdata::key &bad = const_cast<layoutdata::key &>(f.root->activities[0].pages[1].keys[0]);
    uint8_t *p = (uint8_t *)&bad;
    uint32_t junk = (uint32_t)(uintptr_t)f.blob.GetBuffer() + 700;      // not an image
    memcpy(p + LayoutKeyImagePtrOffset(KEY_SLOT_BG), &junk, 4);
    Assert(!f.Activity(1, 0));

    Assert(f.layout.PageCount() == 2);
    Assert(f.Stored() == stored);
    Assert(f.PageKey(1, 0).bg == before.bg);
    Assert(f.changes.GetCount() == 0);
    Assert(f.Activity(1, 1));                                   // and Listen still applies cleanly
}

Fact("LayoutApplyActivity Refuses An Index Outside The Layout")
{
    Fixture f;
    Assert(f.Apply());
    Assert(!f.Activity(-1, (int)f.root->activities_count));
    Assert(!f.Activity(-2, 0));
    Assert(!f.Activity((int)f.root->activities_count, 0));
    Assert(f.layout.PageCount() == 1);
    Assert(f.Stored() == f.DistinctImages());
}

// The RUN_STRESS setup on a device with a stored layout: Home's images are external, then the host
// (or the stress run) uploads more under its own IDs. Every upload must stay live and usable.
Fact("LayoutApply Uploads After Home Stay Live")
{
    Fixture f;
    Assert(f.Apply());
    const TestImages &ti = TestImages::Get();
    List<uint32_t> ids;
    for (int i = 0; i < TEST_KEYS_FACES; i++) {
        ids.Add(0xDE000100u + (uint32_t)i);
        Assert(f.env.Upload(f.store, ids[ids.GetCount() - 1], ti.Face(i, false)) == ImageResult::Ok);
        ids.Add(0xDE000110u + (uint32_t)i);
        Assert(f.env.Upload(f.store, ids[ids.GetCount() - 1], ti.Face(i, true)) == ImageResult::Ok);
    }
    for (int i = 0; i < ti.IconCount(); i++) {
        ids.Add(0xDE000200u + (uint32_t)i);
        Assert(f.env.Upload(f.store, ids[ids.GetCount() - 1], ti.Icon(i)) == ImageResult::Ok);
    }
    for (int i = 0; i < ids.GetCount(); i++)
        Assert(f.store.IsLive(ids[i]));
}
