// Layout apply: a validated blob's Home and activity pages become the key
// layout (the blob's key ids and rects), their images registered with the
// store in place (docs/standalone.md "Image IDs").

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

    // The sample's ids: Watch's pages hold one key each (16, 31), Listen's page two (46, 47)
    const uint16_t WATCH_KEYS[2] = { 16, 31 };
    const uint16_t LISTEN_KEYS[2] = { 46, 47 };

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
            store.Init(env.Host(), 1u << 20, KEY_GRID_W, KEY_GRID_H, true);     // as the device: opaque only
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

        static void AddIds(const LayoutKey &k, Set<uint32_t> &ids)
        {
            if (k.normal.offset)
                ids.Add(k.normal.offset);
            if (k.pressed.offset)
                ids.Add(k.pressed.offset);
        }

        Set<uint32_t> HomeIds()
        {
            Set<uint32_t> ids;
            for (uint32_t i = 0; i < summary.home_count; i++)
                AddIds(summary.home[i], ids);
            return ids;
        }

        // A key of the activity's page, as a LayoutKey
        LayoutKey ActivityKey(int a, int page, int i)
        {
            return LayoutKeyOf(root->activities[a].pages[page].keys[i], blob.GetBuffer());
        }

        Set<uint32_t> ActivityIds(int a)
        {
            Set<uint32_t> ids;
            for (uint32_t p = 0; p < root->activities[a].pages_count; p++)
                for (uint32_t i = 0; i < root->activities[a].pages[p].keys_count; i++)
                    AddIds(ActivityKey(a, (int)p, (int)i), ids);
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

        // What the key with `id` shows now (null: no such key, or no image)
        const void *Shown(uint16_t id)
        {
            const KeyState *k = layout.Find(id);
            const ImageEntry *e = k ? KeyLayout::ShownEntry(*k) : nullptr;
            return e ? e->pixels : nullptr;
        }

        bool Active(uint16_t id) { return layout.IsActive(id); }

        // The key with `id` is on `page`
        bool On(uint16_t id, int page)
        {
            int p = -1;
            return layout.Find(id, &p) && p == page;
        }

        static void Changed(int page, uint32_t flags, void *ctx)
        {
            ((Fixture *)ctx)->changes.Add(Change{ page, flags });
        }

        bool Apply() { return LayoutApplyHome(summary, blob.GetBuffer(), &store, &layout); }
        const void *At(uint32_t offset) { return blob.GetBuffer() + offset; }

        int DistinctImages() { return HomeIds().GetCount(); }
    };

    bool SameRect(KeyRect a, KeyRect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
}

Fact("LayoutApply Shows Home's Keys With Their Ids And Rects")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.layout.PageCount() == 1);
    Assert(f.layout.KeyCount(0) == (int)f.summary.home_count && f.summary.home_count == 13);

    for (uint32_t i = 0; i < f.summary.home_count; i++) {
        const LayoutKey &src = f.summary.home[i];
        int page = -1;
        const KeyState *k = f.layout.Find(src.id, &page);
        Assert(k && page == 0 && SameRect(k->rect, src.rect));
        Assert(f.Active(src.id));
        Assert(f.Shown(src.id) == f.At(src.normal.offset));
    }

    // Every key is drawn: its image is its rect's size (spans and 0.5U keys too)
    comp_page_t page;
    Assert(f.layout.GetPage(0, &page));
    Assert(page.count == (int)f.summary.home_count);
    for (int i = 0; i < page.count; i++) {
        KeyPixelRect px = KeyRectPixels(f.summary.home[i].rect);
        Assert(page.keys[i].rect.x == px.x && page.keys[i].rect.w == px.w && page.keys[i].rect.h == px.h);
        Assert(page.keys[i].img == f.At(f.summary.home[i].normal.offset));
    }

    // Empty slots have no key
    for (uint16_t id : { 4, 6, 9, 12 })
        Assert(!f.layout.Find(id));
}

Fact("LayoutApply Registers Each Image Once Under Its Offset")
{
    Fixture f;
    Assert(f.Apply());
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == (uint32_t)f.DistinctImages());
    Assert(st.used == 0 && f.env.LiveBlocks() == 0);          // nothing copied

    for (uint32_t i = 0; i < f.summary.home_count; i++) {
        for (const LayoutImage &im : { f.summary.home[i].normal, f.summary.home[i].pressed }) {
            ImageEntry *e = f.store.Find(im.offset);
            Assert(e && e->external && e->pixels == f.At(im.offset));
        }
    }
    // The pressed image keys 5 and 10 share is one entry held by both
    Assert(f.summary.home[4].pressed.offset == f.summary.home[7].pressed.offset);
    Assert(f.store.Find(f.summary.home[4].pressed.offset)->key_refs == 2);
}

Fact("LayoutApply Pressed Keys Show Their Pressed Image")
{
    Fixture f;
    Assert(f.Apply());
    for (uint32_t i : { 0u, 7u, 9u }) {                     // 1U, the 2U span, a 0.5U key
        const LayoutKey &k = f.summary.home[i];
        Assert(f.layout.SetPressed(k.id, true));
        Assert(f.Shown(k.id) == f.At(k.pressed.offset));
        Assert(f.layout.SetPressed(k.id, false));
        Assert(f.Shown(k.id) == f.At(k.normal.offset));
    }
}

Fact("LayoutApply Reports Page 0 Changed With A Flush")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.changes.GetCount() == 1 && f.changes[0].page == 0 && f.changes[0].flags == COMP_FLUSH);
}

Fact("LayoutApply Key With No Images Is A Key Drawn As Background")
{
    Fixture f;
    f.summary.home[0].normal = LayoutImage{};
    f.summary.home[0].pressed = LayoutImage{};
    Assert(f.Apply());
    uint16_t id = f.summary.home[0].id;
    Assert(f.Active(id) && !f.Shown(id));
    comp_page_t page;
    Assert(f.layout.GetPage(0, &page));
    Assert(page.keys[0].img == nullptr);
}

Fact("LayoutApply Failure Leaves The Store And Layout Empty")
{
    Fixture f;
    // An image that fails the store check, after earlier images went in
    f.summary.home[7].pressed = LayoutImage{ 700, 64 };
    Assert(!f.Apply());

    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 0);
    Assert(f.changes.GetCount() == 0);
    Assert(f.layout.KeyCount(0) == 0);
    for (uint32_t i = 0; i < f.summary.home_count; i++)
        Assert(!f.Active(f.summary.home[i].id) && !f.Shown(f.summary.home[i].id));
}

Fact("LayoutApply Failure On A Key The Layout Refuses Leaves Everything Empty")
{
    // Two keys overlapping (a summary that didn't come from LayoutValidate): the key
    // layout refuses it after the images went in
    Fixture f;
    f.summary.home[1].rect = f.summary.home[0].rect;
    Assert(!f.Apply());
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 0 && f.layout.KeyCount(0) == 0);
}

Fact("LayoutApply Can Rebuild After A Reset")
{
    Fixture f;
    Assert(f.Apply());
    f.layout.Clear();
    f.store.Reset();
    f.changes.Clear();
    Assert(f.Apply());
    Assert(f.Active(1));
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == (uint32_t)f.DistinctImages());
}

Fact("LayoutApply Copes With An Empty Home")
{
    Fixture f;
    f.summary = {};
    Assert(f.Apply());
    Assert(f.layout.KeyCount(0) == 0);
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
        LayoutKey src = f.ActivityKey(0, p, 0);
        Assert(src.id == WATCH_KEYS[p]);
        Assert(f.On(src.id, 1 + p) && f.Active(src.id));
        Assert(SameRect(f.layout.Find(src.id)->rect, src.rect));
        Assert(f.Shown(src.id) == f.At(src.normal.offset));
        Assert(f.layout.KeyCount(1 + p) == 1);
    }
    // Home is untouched, and no change was reported for it
    Assert(f.Shown(1) == f.At(f.summary.home[0].normal.offset));
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
    Assert(f.layout.KeyCount(2) == -1);
    Assert(!f.layout.Find(WATCH_KEYS[0]) && !f.layout.Find(WATCH_KEYS[1]));
    for (uint16_t id : LISTEN_KEYS)
        Assert(f.On(id, 1));
    Assert(f.Shown(LISTEN_KEYS[0]) == f.At(f.ActivityKey(1, 0, 0).normal.offset));
    Assert(f.Stored() == f.Expected(1));

    Assert(f.Activity(1, 0) && f.layout.PageCount() == 3);
    Assert(f.On(WATCH_KEYS[1], 2) && !f.layout.Find(LISTEN_KEYS[0]));
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
    Assert(f.Activity(-1, 1));
    Set<uint32_t> home = f.HomeIds();
    Set<uint32_t> listen = f.ActivityIds(1);
    Assert(f.Activity(1, -1));
    // Everything Home uses is still live and still on its keys
    for (auto it = home.Iterate(); it.Next(); )
        Assert(f.store.IsLive(it.Get()));
    for (auto it = listen.Iterate(); it.Next(); )
        Assert(f.store.IsLive(it.Get()) == home.Contains(it.Get()));
    for (uint32_t i = 0; i < f.summary.home_count; i++)
        Assert(f.Shown(f.summary.home[i].id) == f.At(f.summary.home[i].normal.offset));
}

Fact("LayoutApplyActivity Refusal Leaves Store Layout And Page Count As They Were")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.Activity(-1, 1));                                  // Listen is showing
    int stored = f.Stored();
    const void *before = f.Shown(LISTEN_KEYS[0]);
    f.changes.Clear();

    // Watch's second page gets an image the store refuses, after earlier images went in
    layoutdata::key &bad = f.root->activities[0].pages[1].keys[0];
    bad.normal = f.blob.GetBuffer() + 700;                      // not an image
    Assert(!f.Activity(1, 0));

    Assert(f.layout.PageCount() == 2);
    Assert(f.Stored() == stored);
    Assert(before && f.Shown(LISTEN_KEYS[0]) == before);
    Assert(!f.layout.Find(WATCH_KEYS[0]));
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
    List<uint32_t> ids;
    for (int i = 0; i < TEST_KEYS_KEYS_1U; i++) {
        ids.Add(0xDE000100u + (uint32_t)i);
        Assert(f.env.Upload(f.store, ids[ids.GetCount() - 1], f.env.Key1U(i)) == ImageResult::Ok);
    }
    for (int i = 0; i < TEST_KEYS_KEYS_2U; i++) {
        ids.Add(0xDE000200u + (uint32_t)i);
        Assert(f.env.Upload(f.store, ids[ids.GetCount() - 1], f.env.Key2U(i)) == ImageResult::Ok);
    }
    for (int i = 0; i < ids.GetCount(); i++)
        Assert(f.store.IsLive(ids[i]));
}
