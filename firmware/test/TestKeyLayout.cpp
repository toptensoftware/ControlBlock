// Key layout: four image slots per key, ref counts, all-or-nothing SET_KEYS,
// pressed pairs and replace (docs/image-assets.md)

#include <stdlib.h>
#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "rle565.h"
#include "key_layout.h"
#include "store_env.h"

using namespace SimpleLib;

namespace
{
    struct Change { int page; uint32_t flags; };

    struct Fixture
    {
        StoreEnv     env;
        ImageStore   store;
        KeyLayout    layout;
        List<Change> changes;

        Fixture()
        {
            store.Init(env.Host(), 1u << 20, COMP_KEY_SIZE, COMP_KEY_SIZE);
            store.SetListener(&layout);
            layout.Init(&store, Changed, this);
            // IDs 1-4: faces (2 opaque), 10-15: icons
            Assert(env.Upload(store, 1, env.Face(0)) == ImageResult::Ok);
            Assert(env.Upload(store, 2, env.Face(1, true)) == ImageResult::Ok);
            Assert(env.Upload(store, 3, env.Face(TEST_KEYS_FACE_PRESSED)) == ImageResult::Ok);
            Assert(env.Upload(store, 4, env.Face(2)) == ImageResult::Ok);
            for (int i = 0; i < 6; i++)
                Assert(env.Upload(store, 10 + i, env.Icon(i)) == ImageResult::Ok);
        }

        static void Changed(int page, uint32_t flags, void *ctx)
        {
            ((Fixture *)ctx)->changes.Add(Change{ page, flags });
        }

        const void *Px(uint32_t id) { return store.Find(id)->data->Pixels(); }
        uint32_t Refs(uint32_t id) { return store.Find(id)->key_refs; }

        comp_key_t Key(int page, int key)
        {
            comp_key_t keys[COMP_KEYS];
            Assert(layout.GetPage(page, keys));
            return keys[key];
        }
    };

    KeyDef Def(int page, int key, uint32_t bg, uint32_t fg, uint32_t pbg = 0, uint32_t pfg = 0, uint16_t flags = 0)
    {
        return KeyDef{ (uint8_t)page, (uint8_t)key, flags, { bg, fg, pbg, pfg } };
    }
}

Fact("KeyLayout Starts With One Empty Page")
{
    Fixture f;
    Assert(f.layout.PageCount() == 1);
    comp_key_t keys[COMP_KEYS];
    Assert(f.layout.GetPage(0, keys));
    for (int k = 0; k < COMP_KEYS; k++)
        Assert(!keys[k].bg && !keys[k].fg && !f.layout.IsActive(0, k));
    Assert(!f.layout.GetPage(1, keys));
}

Fact("KeyLayout SetKeys Gives The Compositor The Pixels")
{
    Fixture f;
    KeyDef defs[] = { Def(0, 0, 1, 10), Def(0, 14, 2, 11) };
    Assert(f.layout.SetKeys(defs, 2) == KeyLayoutResult::Ok);

    comp_key_t k0 = f.Key(0, 0), k14 = f.Key(0, 14);
    Assert(k0.bg == f.Px(1) && k0.fg == f.Px(10) && k0.flags == 0);
    Assert(k14.bg == f.Px(2) && k14.fg == f.Px(11) && k14.flags == COMP_KEY_BG_OPAQUE);
    Assert(f.layout.IsActive(0, 0) && !f.layout.IsActive(0, 1));
    Assert(f.Refs(1) == 1 && f.Refs(10) == 1);

    // One flushed change per page
    Assert(f.changes.GetCount() == 1);
    Assert(f.changes[0].page == 0 && f.changes[0].flags == COMP_FLUSH);

    // Or incremental, for the device's own test changes
    KeyDef again = Def(0, 1, 1, 12);
    Assert(f.layout.SetKeys(&again, 1, 0) == KeyLayoutResult::Ok);
    Assert(f.changes.GetCount() == 2 && f.changes[1].flags == 0);
}

Fact("KeyLayout SetKeys Is All Or Nothing")
{
    Fixture f;
    Assert(f.layout.SetPageCount(2) == KeyLayoutResult::Ok);
    KeyDef ok = Def(0, 0, 1, 10);
    Assert(f.layout.SetKeys(&ok, 1) == KeyLayoutResult::Ok);
    f.changes.Clear();

    KeyDef missing[] = { Def(0, 0, 4, 11), Def(1, 3, 1, 99) };
    Assert(f.layout.SetKeys(missing, 2) == KeyLayoutResult::NotFound);
    KeyDef bad_page[] = { Def(0, 0, 4, 11), Def(2, 0, 1, 10) };
    Assert(f.layout.SetKeys(bad_page, 2) == KeyLayoutResult::BadArg);
    KeyDef bad_key[] = { Def(0, 0, 4, 11), Def(0, COMP_KEYS, 1, 10) };
    Assert(f.layout.SetKeys(bad_key, 2) == KeyLayoutResult::BadArg);
    KeyDef empty_with_ids = Def(0, 1, 1, 0, 0, 0, KEYDEF_EMPTY);
    Assert(f.layout.SetKeys(&empty_with_ids, 1) == KeyLayoutResult::BadArg);

    // Nothing changed
    Assert(f.Key(0, 0).bg == f.Px(1) && f.Key(0, 0).fg == f.Px(10));
    Assert(f.Refs(1) == 1 && f.Refs(4) == 0 && f.Refs(11) == 0);
    Assert(f.changes.GetCount() == 0);
}

Fact("KeyLayout Ref Counts Follow Every Slot")
{
    Fixture f;
    Assert(f.layout.SetPageCount(3) == KeyLayoutResult::Ok);

    // Face 1 on many keys and pages, and in both slots of one key
    List<KeyDef> defs;
    for (int p = 0; p < 3; p++)
        for (int k = 0; k < 5; k++)
            defs.Add(Def(p, k, 1, 10 + k, 3, 10 + k));
    defs.Add(Def(2, 14, 1, 1));
    Assert(f.layout.SetKeys(defs.GetBuffer(), defs.GetCount()) == KeyLayoutResult::Ok);
    Assert(f.Refs(1) == 17 && f.Refs(3) == 15 && f.Refs(10) == 6);

    // Redefining a key with the same image keeps it; others drop
    KeyDef same = Def(0, 0, 1, 11);
    Assert(f.layout.SetKeys(&same, 1) == KeyLayoutResult::Ok);
    Assert(f.Refs(1) == 17 && f.Refs(3) == 14 && f.Refs(10) == 4 && f.Refs(11) == 7);

    // Two entries for one key: the last wins
    KeyDef twice[] = { Def(0, 1, 4, 12), Def(0, 1, 2, 0) };
    Assert(f.layout.SetKeys(twice, 2) == KeyLayoutResult::Ok);
    Assert(f.Key(0, 1).bg == f.Px(2) && !f.Key(0, 1).fg);
    Assert(f.Refs(4) == 0 && f.Refs(12) == 6 && f.Refs(2) == 1);
    Assert(f.Refs(1) == 16 && f.Refs(11) == 5);

    // Emptying keys releases them
    KeyDef empty = Def(2, 14, 0, 0, 0, 0, KEYDEF_EMPTY);
    Assert(f.layout.SetKeys(&empty, 1) == KeyLayoutResult::Ok);
    Assert(f.Refs(1) == 14);
}

Fact("KeyLayout Freed Image Shows Until Its Last Key Goes")
{
    Fixture f;
    KeyDef defs[] = { Def(0, 0, 1, 10), Def(0, 1, 4, 10) };
    Assert(f.layout.SetKeys(defs, 2) == KeyLayoutResult::Ok);
    const void *px = f.Px(10);

    f.store.Free(10);
    Assert(!f.store.IsLive(10));
    Assert(f.Key(0, 0).fg == px && f.Key(0, 1).fg == px);   // still shown
    KeyDef again = Def(0, 2, 1, 10);
    Assert(f.layout.SetKeys(&again, 1) == KeyLayoutResult::NotFound);   // can't be used anew

    KeyDef k0 = Def(0, 0, 1, 11);
    Assert(f.layout.SetKeys(&k0, 1) == KeyLayoutResult::Ok);
    Assert(!ImageStore::IsRetired(px));
    KeyDef k1 = Def(0, 1, 4, 0);
    Assert(f.layout.SetKeys(&k1, 1) == KeyLayoutResult::Ok);
    Assert(ImageStore::IsRetired(px));          // waiting for the compositor's sweep
    f.env.Sweep();
    f.store.Collect();
    Assert(!f.env.IsAllocated(px));
}

Fact("KeyLayout Pressed Pair Shows While Held")
{
    Fixture f;
    KeyDef defs[] = {
        Def(0, 0, 1, 10, 3, 11),        // pressed: a different face and icon
        Def(0, 1, 1, 10),               // no pressed pair: no feedback
        Def(0, 2, 1, 10, 3, 0),         // pressed face only, no pressed icon
        Def(0, 3, 1, 10, 3, 11, KEYDEF_DISABLED),
    };
    Assert(f.layout.SetKeys(defs, 4) == KeyLayoutResult::Ok);
    f.changes.Clear();

    for (int k = 0; k < 4; k++)
        Assert(f.layout.SetPressed(0, k, true));
    Assert(f.Key(0, 0).bg == f.Px(3) && f.Key(0, 0).fg == f.Px(11));
    Assert(f.Key(0, 1).bg == f.Px(1) && f.Key(0, 1).fg == f.Px(10));
    Assert(f.Key(0, 2).bg == f.Px(3) && f.Key(0, 2).fg == nullptr);
    Assert(f.Key(0, 3).bg == f.Px(1) && f.Key(0, 3).fg == f.Px(10));
    Assert(!f.layout.IsActive(0, 3));

    // Press feedback is incremental, not flushed
    Assert(f.changes.GetCount() >= 1);
    for (int i = 0; i < f.changes.GetCount(); i++)
        Assert(f.changes[i].flags == 0);

    // Redefining a held key keeps it held
    KeyDef redo = Def(0, 0, 4, 12, 2, 13);
    Assert(f.layout.SetKeys(&redo, 1) == KeyLayoutResult::Ok);
    Assert(f.Key(0, 0).bg == f.Px(2) && f.Key(0, 0).flags == COMP_KEY_BG_OPAQUE);

    Assert(f.layout.SetPressed(0, 0, false));
    Assert(f.Key(0, 0).bg == f.Px(4) && f.Key(0, 0).fg == f.Px(12) && f.Key(0, 0).flags == 0);
    Assert(!f.layout.SetPressed(1, 0, true));
    Assert(!f.layout.SetPressed(0, COMP_KEYS, true));
}

Fact("KeyLayout Replace Updates Exactly The Keys Using It")
{
    Fixture f;
    Assert(f.layout.SetPageCount(3) == KeyLayoutResult::Ok);
    KeyDef defs[] = { Def(0, 0, 1, 10), Def(2, 5, 4, 11, 3, 10), Def(1, 0, 1, 12) };
    Assert(f.layout.SetKeys(defs, 3) == KeyLayoutResult::Ok);
    f.changes.Clear();
    const void *old = f.Px(10);

    Assert(f.env.Upload(f.store, 10, f.env.Icon(5)) == ImageResult::Ok);
    const void *now = f.Px(10);
    Assert(now != old);
    Assert(f.Key(0, 0).fg == now);
    Assert(f.layout.SetPressed(2, 5, true));
    Assert(f.Key(2, 5).fg == now);

    // Pages 0 and 2 flushed, page 1 untouched
    Assert(f.changes.GetCount() >= 2);
    Assert(f.changes[0].page == 0 && f.changes[0].flags == COMP_FLUSH);
    Assert(f.changes[1].page == 2 && f.changes[1].flags == COMP_FLUSH);
    for (int i = 0; i < f.changes.GetCount(); i++)
        Assert(f.changes[i].page != 1);

    // The keys moved off the old pixels before they retired
    Assert(ImageStore::IsRetired(old));
    Assert(f.Refs(10) == 2);
}

Fact("KeyLayout SetPageCount Clears Removed Pages")
{
    Fixture f;
    Assert(f.layout.SetPageCount(0) == KeyLayoutResult::BadArg);
    Assert(f.layout.SetPageCount(KEY_LAYOUT_MAX_PAGES + 1) == KeyLayoutResult::BadArg);
    Assert(f.layout.SetPageCount(KEY_LAYOUT_MAX_PAGES) == KeyLayoutResult::Ok);

    KeyDef defs[] = { Def(0, 0, 1, 10), Def(KEY_LAYOUT_MAX_PAGES - 1, 0, 1, 11) };
    Assert(f.layout.SetKeys(defs, 2) == KeyLayoutResult::Ok);
    Assert(f.layout.SetPageCount(2) == KeyLayoutResult::Ok);
    Assert(f.Refs(1) == 1 && f.Refs(11) == 0);

    // Growing again brings back empty pages
    Assert(f.layout.SetPageCount(KEY_LAYOUT_MAX_PAGES) == KeyLayoutResult::Ok);
    Assert(!f.Key(KEY_LAYOUT_MAX_PAGES - 1, 0).bg);
}

Fact("KeyLayout Clear Releases Everything")
{
    Fixture f;
    Assert(f.layout.SetPageCount(3) == KeyLayoutResult::Ok);
    KeyDef defs[] = { Def(0, 0, 1, 10, 3, 11), Def(2, 4, 2, 12) };
    Assert(f.layout.SetKeys(defs, 2) == KeyLayoutResult::Ok);
    f.layout.Clear();
    Assert(f.layout.PageCount() == 1);
    Assert(f.Refs(1) == 0 && f.Refs(3) == 0 && f.Refs(12) == 0);
    Assert(!f.Key(0, 0).bg && !f.Key(0, 0).fg);

    // Then the store can be reset with nothing referenced
    f.store.Reset();
    Assert(f.env.LiveBlocks() == 0);
}

Fact("KeyLayout Draws External Images")
{
    Fixture f;
    const void *face = f.env.Face(1, true);                 // opaque
    const void *icon = f.env.Icon(0);
    uint32_t face_size = f.env.Size(face), icon_size = f.env.Size(icon);
    uint8_t *face_copy = (uint8_t *)malloc(face_size);
    uint8_t *icon_copy = (uint8_t *)malloc(icon_size);
    memcpy(face_copy, face, face_size);
    memcpy(icon_copy, icon, icon_size);
    Assert(f.store.AddExternal(1000, face_copy, face_size) == ImageResult::Ok);
    Assert(f.store.AddExternal(1004, icon_copy, icon_size) == ImageResult::Ok);

    KeyDef def = Def(0, 2, 1000, 1004, 1000, 1004);
    Assert(f.layout.SetKeys(&def, 1) == KeyLayoutResult::Ok);
    comp_key_t k = f.Key(0, 2);
    Assert(k.bg == face_copy && k.fg == icon_copy && k.flags == COMP_KEY_BG_OPAQUE);
    Assert(f.layout.IsActive(0, 2));
    Assert(f.store.Find(1000)->key_refs == 2);

    // Pressing swaps to the pressed pair, here the same external images
    Assert(f.layout.SetPressed(0, 2, true));
    Assert(f.Key(0, 2).bg == face_copy);

    // Clearing the layout releases them; nothing is retired and nothing is freed
    f.layout.Clear();
    Assert(f.store.Find(1000)->key_refs == 0);
    f.store.Free(1000);
    f.store.Free(1004);
    Assert(!f.store.HasRetired() && f.env.sweeps_requested == 0);
    Assert(get_image_width(face_copy) == COMP_KEY_SIZE);
    free(face_copy);
    free(icon_copy);
}
