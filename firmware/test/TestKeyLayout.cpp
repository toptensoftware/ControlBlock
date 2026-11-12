// Key layout: per-page key lists { id, quarter-unit rect, flags, normal,
// pressed }, all-or-nothing SetKeys over a range of pages, ref counts, the
// pressed swap and replace (docs/image-assets.md)

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

    // Image IDs: 1-6 opaque 1U keys, 20-21 opaque 2U keys, 40 an opaque 128 face
    const uint32_t FACE_128 = 40;

    struct Fixture
    {
        StoreEnv     env;
        ImageStore   store;
        KeyLayout    layout;
        List<Change> changes;

        Fixture()
        {
            store.Init(env.Host(), 1u << 20, KEY_GRID_W, KEY_GRID_H, true);
            store.SetListener(&layout);
            layout.Init(&store, Changed, this);
            for (int i = 0; i < 6; i++)
                Assert(env.Upload(store, 1 + i, env.Key1U(i)) == ImageResult::Ok);
            for (int i = 0; i < 2; i++)
                Assert(env.Upload(store, 20 + i, env.Key2U(i)) == ImageResult::Ok);
            Assert(env.Upload(store, FACE_128, env.Face(1, true)) == ImageResult::Ok);
        }

        static void Changed(int page, uint32_t flags, void *ctx)
        {
            ((Fixture *)ctx)->changes.Add(Change{ page, flags });
        }

        const void *Px(uint32_t id) { return store.Find(id)->pixels; }
        uint32_t Refs(uint32_t id) { return store.Find(id)->key_refs; }

        // What key `id` shows (its pixels, whether or not they fit its rect)
        const void *Shown(uint16_t id)
        {
            const KeyState *k = layout.Find(id);
            Assert(k != nullptr);
            const ImageEntry *e = KeyLayout::ShownEntry(*k);
            return e ? e->pixels : nullptr;
        }

        comp_page_t Page(int page)
        {
            comp_page_t out;
            Assert(layout.GetPage(page, &out));
            return out;
        }

        KeyLayoutResult Set(int first, int pages, const KeyDef *defs, int count, uint32_t flags = COMP_FLUSH)
        {
            return layout.SetKeys(first, pages, defs, count, flags);
        }
    };

    KeyDef Def(uint16_t id, int page, KeyRect r, uint32_t normal, uint32_t pressed = 0, uint16_t flags = 0)
    {
        KeyDef d = {};
        d.id      = id;
        d.page    = (uint8_t)page;
        d.rect    = r;
        d.flags   = flags;
        d.normal  = normal;
        d.pressed = pressed;
        return d;
    }

    // A 1U key in plain grid slot `slot`, with the plain grid's id
    KeyDef Slot(int page, int slot, uint32_t normal, uint32_t pressed = 0, uint16_t flags = 0)
    {
        return Def(KeySlotId(page, slot), page, KeySlotRect(slot), normal, pressed, flags);
    }

    const KeyRect SPAN_2U = { 4, 4, 8, 4 };      // 2U wide, slots 6 and 7
}

Fact("KeyLayout Starts With One Empty Page")
{
    Fixture f;
    Assert(f.layout.PageCount() == 1);
    Assert(f.Page(0).count == 0);
    comp_page_t page;
    Assert(!f.layout.GetPage(1, &page));
    Assert(!f.layout.Find(1) && !f.layout.IsActive(1));
    Assert(f.layout.KeyCount(0) == 0 && f.layout.KeyCount(1) == -1);
}

Fact("KeyLayout GetPage Gives The Compositor Rects And Shown Images")
{
    Fixture f;
    KeyDef defs[] = {
        Slot(0, 0, 1),
        Def(500, 0, SPAN_2U, 20),
        Def(7, 0, KeyRect{ 16, 8, 2, 2 }, 0),       // no image: background
    };
    Assert(f.Set(0, 1, defs, 3) == KeyLayoutResult::Ok);

    comp_page_t p = f.Page(0);
    Assert(p.count == 3);
    for (int i = 0; i < 3; i++) {
        KeyPixelRect want = KeyRectPixels(defs[i].rect);
        KeyPixelRect r = p.keys[i].rect;
        Assert(r.x == want.x && r.y == want.y && r.w == want.w && r.h == want.h);
    }
    Assert(p.keys[0].img == f.Px(1));
    Assert(p.keys[1].img == f.Px(20) && p.keys[1].rect.w == 2 * KEY_CELL);
    Assert(p.keys[2].img == nullptr);
    Assert(f.layout.IsActive(1) && f.layout.IsActive(500) && f.layout.IsActive(7));

    // One flushed change for the page
    Assert(f.changes.GetCount() == 1);
    Assert(f.changes[0].page == 0 && f.changes[0].flags == COMP_FLUSH);

    // Or incremental, for the device's own test changes
    Assert(f.Set(0, 1, defs, 1, 0) == KeyLayoutResult::Ok);
    Assert(f.changes.GetCount() == 2 && f.changes[1].flags == 0);
    Assert(f.Page(0).count == 1);
}

Fact("KeyLayout SetKeys Replaces Exactly The Pages In Its Range")
{
    Fixture f;
    Assert(f.layout.SetPageCount(3) == KeyLayoutResult::Ok);
    KeyDef all[] = { Slot(0, 0, 1), Slot(1, 0, 2), Slot(1, 1, 3), Slot(2, 4, 4) };
    Assert(f.Set(0, 3, all, 4) == KeyLayoutResult::Ok);
    Assert(f.changes.GetCount() == 3);
    f.changes.Clear();

    // Page 1 gets one key; pages 0 and 2 are untouched
    KeyDef one = Slot(1, 2, 5);
    Assert(f.Set(1, 1, &one, 1) == KeyLayoutResult::Ok);
    Assert(f.layout.KeyCount(0) == 1 && f.layout.KeyCount(1) == 1 && f.layout.KeyCount(2) == 1);
    Assert(!f.layout.Find(KeySlotId(1, 0)) && f.layout.Find(KeySlotId(1, 2)));
    Assert(f.Refs(2) == 0 && f.Refs(3) == 0 && f.Refs(5) == 1);
    Assert(f.changes.GetCount() == 1 && f.changes[0].page == 1);

    // No keys empties the range, every page of it reported
    f.changes.Clear();
    Assert(f.Set(1, 2, nullptr, 0) == KeyLayoutResult::Ok);
    Assert(f.layout.KeyCount(1) == 0 && f.layout.KeyCount(2) == 0 && f.layout.KeyCount(0) == 1);
    Assert(f.Refs(4) == 0 && f.Refs(5) == 0 && f.Refs(1) == 1);
    Assert(f.changes.GetCount() == 2 && f.changes[0].page == 1 && f.changes[1].page == 2);
}

Fact("KeyLayout SetKeys Is All Or Nothing")
{
    Fixture f;
    Assert(f.layout.SetPageCount(2) == KeyLayoutResult::Ok);
    KeyDef ok[] = { Slot(0, 0, 1), Slot(1, 0, 2) };
    Assert(f.Set(0, 2, ok, 2) == KeyLayoutResult::Ok);
    f.changes.Clear();

    // Each refusal has a good key (image 3) on page 0 before the bad one
    KeyDef good = Slot(0, 1, 3);
    struct Case { KeyDef bad; int first, pages; KeyLayoutResult want; };
    Case cases[] = {
        { Slot(1, 4, 4),                                  0, 1, KeyLayoutResult::BadArg },        // page outside the range
        { Slot(2, 4, 4),                                  0, 3, KeyLayoutResult::BadArg },        // range past the page count
        { Slot(0, 4, 4, 0, 0x0100),                       0, 1, KeyLayoutResult::BadArg },        // unknown flag
        { Def(90, 0, KeyRect{ 18, 0, 4, 4 }, 0),          0, 1, KeyLayoutResult::OutOfGrid },
        { Def(90, 0, KeyRect{ 0, 0, 0, 4 }, 0),           0, 1, KeyLayoutResult::OutOfGrid },     // empty
        { Def(90, 0, KeyRect{ 6, 2, 4, 4 }, 0),           0, 1, KeyLayoutResult::Overlap },       // over slot 1
        { Def(0, 0, KeySlotRect(4), 4),                   0, 1, KeyLayoutResult::BadId },
        { Def(KeySlotId(0, 1), 0, KeySlotRect(4), 4),     0, 1, KeyLayoutResult::DuplicateId },   // the good key's
        { Def(KeySlotId(1, 0), 0, KeySlotRect(4), 4),     0, 1, KeyLayoutResult::DuplicateId },   // page 1's key
        { Slot(0, 4, 99),                                 0, 1, KeyLayoutResult::NotFound },
        { Slot(0, 4, 4, 99),                              0, 1, KeyLayoutResult::NotFound },      // pressed
        { Slot(0, 4, FACE_128),                           0, 1, KeyLayoutResult::BadSize },       // 128 in a 1U rect
        { Slot(0, 4, 4, 20),                              0, 1, KeyLayoutResult::BadSize },       // 2U pressed image
        { Def(90, 0, SPAN_2U, 4),                         0, 1, KeyLayoutResult::BadSize },       // 1U in a 2U rect
    };
    for (const Case &c : cases) {
        KeyDef defs[] = { good, c.bad };
        Assert(f.Set(c.first, c.pages, defs, 2) == c.want);
    }

    // More than COMP_MAX_KEYS on a page
    KeyDef many[COMP_MAX_KEYS + 1];
    for (int i = 0; i <= COMP_MAX_KEYS; i++)
        many[i] = Def((uint16_t)(100 + i), 0, KeyRect{ (uint8_t)(i % 20), (uint8_t)(i / 20 * 2), 1, 2 }, 0);
    Assert(f.Set(0, 1, many, COMP_MAX_KEYS + 1) == KeyLayoutResult::TooMany);
    Assert(f.Set(0, 1, many, COMP_MAX_KEYS) == KeyLayoutResult::Ok);
    Assert(f.Set(0, 1, ok, 1) == KeyLayoutResult::Ok);
    f.changes.Clear();

    // A bad first page leaves a good second page alone too
    KeyDef two[] = { Slot(0, 0, FACE_128), Slot(1, 3, 3) };
    Assert(f.Set(0, 2, two, 2) == KeyLayoutResult::BadSize);

    // Nothing changed
    Assert(f.layout.KeyCount(0) == 1 && f.layout.KeyCount(1) == 1);
    Assert(f.Shown(KeySlotId(0, 0)) == f.Px(1) && f.Shown(KeySlotId(1, 0)) == f.Px(2));
    Assert(f.Refs(1) == 1 && f.Refs(2) == 1 && f.Refs(3) == 0 && f.Refs(4) == 0);
    Assert(f.changes.GetCount() == 0);
}

Fact("KeyLayout Ids Are Unique Across Pages But A Replaced Page May Reuse Its Own")
{
    Fixture f;
    Assert(f.layout.SetPageCount(2) == KeyLayoutResult::Ok);
    KeyDef a[] = { Def(10, 0, KeySlotRect(0), 1), Def(11, 1, KeySlotRect(0), 2) };
    Assert(f.Set(0, 2, a, 2) == KeyLayoutResult::Ok);

    // Page 0 again with id 10 moved elsewhere: fine, the old page 0 goes
    KeyDef moved = Def(10, 0, KeySlotRect(9), 3);
    Assert(f.Set(0, 1, &moved, 1) == KeyLayoutResult::Ok);
    int page = -1;
    const KeyState *k = f.layout.Find(10, &page);
    Assert(k && page == 0 && k->rect.x == KeySlotRect(9).x && k->rect.y == KeySlotRect(9).y);

    // Id 11 moving from page 1 to page 0 needs both pages in the range
    KeyDef take = Def(11, 0, KeySlotRect(1), 3);
    Assert(f.Set(0, 1, &take, 1) == KeyLayoutResult::DuplicateId);
    KeyDef both[] = { moved, take };
    Assert(f.Set(0, 2, both, 2) == KeyLayoutResult::Ok);
    Assert(f.layout.Find(11, &page) && page == 0 && f.layout.KeyCount(1) == 0);
}

Fact("KeyLayout Ref Counts Follow Every Image")
{
    Fixture f;
    Assert(f.layout.SetPageCount(3) == KeyLayoutResult::Ok);

    // Image 1 on many keys and pages, and as both images of one key
    List<KeyDef> defs;
    for (int p = 0; p < 3; p++)
        for (int k = 0; k < 5; k++)
            defs.Add(Slot(p, k, 1, 2 + k % 2));
    defs.Add(Slot(2, 14, 1, 1));
    Assert(f.Set(0, 3, defs.GetBuffer(), defs.GetCount()) == KeyLayoutResult::Ok);
    Assert(f.Refs(1) == 17 && f.Refs(2) == 9 && f.Refs(3) == 6);

    // Redefining page 0 with the same images keeps them; others drop
    KeyDef same = Slot(0, 0, 1, 4);
    Assert(f.Set(0, 1, &same, 1) == KeyLayoutResult::Ok);
    Assert(f.Refs(1) == 13 && f.Refs(2) == 6 && f.Refs(3) == 4 && f.Refs(4) == 1);

    // Emptying pages releases them
    Assert(f.Set(2, 1, nullptr, 0) == KeyLayoutResult::Ok);
    Assert(f.Refs(1) == 6);
}

Fact("KeyLayout Freed Image Shows Until Its Last Key Goes")
{
    Fixture f;
    KeyDef defs[] = { Slot(0, 0, 1), Slot(0, 1, 2, 1) };
    Assert(f.Set(0, 1, defs, 2) == KeyLayoutResult::Ok);
    const void *px = f.Px(1);

    f.store.Free(1);
    Assert(!f.store.IsLive(1));
    Assert(f.Page(0).keys[0].img == px);                     // still shown
    KeyDef again[] = { Slot(0, 0, 1), Slot(0, 1, 2, 1), Slot(0, 2, 1) };
    Assert(f.Set(0, 1, again, 3) == KeyLayoutResult::NotFound);     // can't be used anew

    // Keys that already hold it may keep it when their page is set again
    Assert(f.Set(0, 1, defs, 2) == KeyLayoutResult::Ok);
    Assert(f.Shown(KeySlotId(0, 0)) == px);
    Assert(f.layout.SetPressed(KeySlotId(0, 1), true));
    Assert(f.Shown(KeySlotId(0, 1)) == px);

    // Key 1 keeps it as its pressed image while key 0 lets it go
    KeyDef keep[] = { Slot(0, 0, 3), Slot(0, 1, 2, 1) };
    Assert(f.Set(0, 1, keep, 2) == KeyLayoutResult::Ok);
    Assert(!ImageStore::IsRetired(px));
    KeyDef rest[] = { Slot(0, 0, 3), Slot(0, 1, 2) };
    Assert(f.Set(0, 1, rest, 2) == KeyLayoutResult::Ok);
    Assert(ImageStore::IsRetired(px));                       // waiting for the compositor's sweep
    f.env.Sweep();
    f.store.Collect();
    Assert(!f.env.IsAllocated(px));
}

Fact("KeyLayout Pressed Image Shows While Held")
{
    Fixture f;
    KeyDef defs[] = {
        Slot(0, 0, 1, 2),                                    // pressed: another image
        Slot(0, 1, 1),                                       // pressed 0: keeps normal, no feedback
        Slot(0, 2, 1, 2, KEYDEF_DISABLED),
        Def(300, 0, SPAN_2U, 20, 21),                        // a span presses as one key
    };
    Assert(f.Set(0, 1, defs, 4) == KeyLayoutResult::Ok);
    f.changes.Clear();

    Assert(f.layout.SetPressed(KeySlotId(0, 1), true));
    Assert(f.layout.SetPressed(KeySlotId(0, 2), true));
    Assert(f.changes.GetCount() == 0);                       // nothing to show
    Assert(f.layout.SetPressed(KeySlotId(0, 0), true));
    Assert(f.layout.SetPressed(300, true));
    Assert(f.changes.GetCount() == 2);
    for (int i = 0; i < f.changes.GetCount(); i++)
        Assert(f.changes[i].page == 0 && f.changes[i].flags == 0);   // incremental, not flushed

    comp_page_t p = f.Page(0);
    Assert(p.keys[0].img == f.Px(2) && p.keys[1].img == f.Px(1) && p.keys[2].img == f.Px(1));
    Assert(p.keys[3].img == f.Px(21));
    Assert(!f.layout.IsActive(KeySlotId(0, 2)) && f.layout.IsActive(KeySlotId(0, 1)));

    // Redefining a held key (same id) keeps it held
    KeyDef redo[] = { Slot(0, 0, 3, 4), defs[3] };
    Assert(f.Set(0, 1, redo, 2) == KeyLayoutResult::Ok);
    Assert(f.Shown(KeySlotId(0, 0)) == f.Px(4) && f.Shown(300) == f.Px(21));

    Assert(f.layout.SetPressed(KeySlotId(0, 0), false));
    Assert(f.Shown(KeySlotId(0, 0)) == f.Px(3));
    Assert(!f.layout.SetPressed(KeySlotId(0, 1), true));     // gone with the redefinition
    Assert(!f.layout.SetPressed(999, true));
}

Fact("KeyLayout Lookup By Id")
{
    Fixture f;
    Assert(f.layout.SetPageCount(3) == KeyLayoutResult::Ok);
    KeyDef defs[] = { Def(1000, 0, SPAN_2U, 20, 21, KEYDEF_DISABLED), Def(2, 2, KeySlotRect(14), 5) };
    Assert(f.Set(0, 3, defs, 2) == KeyLayoutResult::Ok);

    int page = -1;
    const KeyState *k = f.layout.Find(1000, &page);
    Assert(k && page == 0 && k->id == 1000 && k->flags == KEYDEF_DISABLED);
    Assert(k->rect.x == 4 && k->rect.y == 4 && k->rect.w == 8 && k->rect.h == 4);
    Assert(k->normal == f.store.Find(20) && k->pressed_image == f.store.Find(21));
    Assert(f.layout.Find(2, &page) && page == 2);
    Assert(!f.layout.Find(KEY_ID_NONE) && !f.layout.Find(3));

    // GetKeys gives the definitions back
    KeyDef out[COMP_MAX_KEYS];
    Assert(f.layout.GetKeys(0, out) == 1);
    Assert(out[0].id == 1000 && out[0].page == 0 && out[0].normal == 20 && out[0].pressed == 21);
    Assert(out[0].flags == KEYDEF_DISABLED && out[0].rect.w == 8);
    Assert(f.layout.GetKeys(1, out) == 0 && f.layout.GetKeys(3, out) == -1);

    // PageKeys gives the live keys without copying (the touch hit test)
    int count = -2;
    const KeyState *keys = f.layout.PageKeys(0, &count);
    Assert(keys && count == 1 && keys[0].id == 1000 && keys[0].rect.w == 8);
    keys = f.layout.PageKeys(2, &count);
    Assert(keys && count == 1 && keys[0].id == 2 && keys[0].rect.x == 16 && keys[0].rect.y == 8);
    Assert(f.layout.PageKeys(1, &count) && count == 0);
    Assert(!f.layout.PageKeys(3, &count) && count == 0);
    Assert(!f.layout.PageKeys(-1, &count) && count == 0);
}

Fact("KeyLayout Replace Updates Exactly The Pages Using It")
{
    Fixture f;
    Assert(f.layout.SetPageCount(3) == KeyLayoutResult::Ok);
    KeyDef defs[] = { Slot(0, 0, 1), Slot(2, 5, 3, 1), Slot(1, 0, 2) };
    Assert(f.Set(0, 3, defs, 3) == KeyLayoutResult::Ok);
    f.changes.Clear();
    const void *old = f.Px(1);

    Assert(f.env.Upload(f.store, 1, f.env.Key1U(5)) == ImageResult::Ok);
    const void *now = f.Px(1);
    Assert(now != old);
    Assert(f.Page(0).keys[0].img == now);
    Assert(f.layout.SetPressed(KeySlotId(2, 5), true));
    Assert(f.Shown(KeySlotId(2, 5)) == now);

    // Pages 0 and 2 flushed, page 1 untouched
    Assert(f.changes.GetCount() >= 2);
    Assert(f.changes[0].page == 0 && f.changes[0].flags == COMP_FLUSH);
    Assert(f.changes[1].page == 2 && f.changes[1].flags == COMP_FLUSH);
    for (int i = 0; i < f.changes.GetCount(); i++)
        Assert(f.changes[i].page != 1);

    Assert(ImageStore::IsRetired(old));
    Assert(f.Refs(1) == 2);
}

Fact("KeyLayout Image Replaced With Another Size Draws Background")
{
    Fixture f;
    KeyDef def = Slot(0, 0, 1);
    Assert(f.Set(0, 1, &def, 1) == KeyLayoutResult::Ok);
    Assert(f.env.Upload(f.store, 1, f.env.Key2U(0)) == ImageResult::Ok);     // 2U now
    comp_page_t p = f.Page(0);
    Assert(p.count == 1 && p.keys[0].img == nullptr);
    Assert(f.Shown(KeySlotId(0, 0)) == f.Px(1));
}

Fact("KeyLayout SetPageCount Clears Removed Pages")
{
    Fixture f;
    Assert(f.layout.SetPageCount(0) == KeyLayoutResult::BadArg);
    Assert(f.layout.SetPageCount(KEY_LAYOUT_MAX_PAGES + 1) == KeyLayoutResult::BadArg);
    Assert(f.layout.SetPageCount(KEY_LAYOUT_MAX_PAGES) == KeyLayoutResult::Ok);

    const int last = KEY_LAYOUT_MAX_PAGES - 1;
    KeyDef a = Slot(0, 0, 1), b = Slot(last, 0, 2);
    Assert(f.Set(0, 1, &a, 1) == KeyLayoutResult::Ok);
    Assert(f.Set(last, 1, &b, 1) == KeyLayoutResult::Ok);
    Assert(f.layout.SetPageCount(2) == KeyLayoutResult::Ok);
    Assert(f.Refs(1) == 1 && f.Refs(2) == 0);
    Assert(!f.layout.Find(KeySlotId(last, 0)));

    // Growing again brings back empty pages, and the removed ids are free
    Assert(f.layout.SetPageCount(KEY_LAYOUT_MAX_PAGES) == KeyLayoutResult::Ok);
    Assert(f.layout.KeyCount(last) == 0);
    KeyDef reuse = Def(KeySlotId(last, 0), 1, KeySlotRect(0), 2);
    Assert(f.Set(1, 1, &reuse, 1) == KeyLayoutResult::Ok);
}

Fact("KeyLayout Clear Releases Everything")
{
    Fixture f;
    Assert(f.layout.SetPageCount(3) == KeyLayoutResult::Ok);
    KeyDef defs[] = { Slot(0, 0, 1, 2), Def(77, 2, SPAN_2U, 20) };
    Assert(f.Set(0, 3, defs, 2) == KeyLayoutResult::Ok);
    f.layout.Clear();
    Assert(f.layout.PageCount() == 1 && f.layout.KeyCount(0) == 0);
    Assert(f.Refs(1) == 0 && f.Refs(2) == 0 && f.Refs(20) == 0);
    Assert(!f.layout.Find(KeySlotId(0, 0)) && !f.layout.Find(77));

    // Then the store can be reset with nothing referenced
    f.store.Reset();
    Assert(f.env.LiveBlocks() == 0);
}

Fact("KeyLayout Draws External Images")
{
    Fixture f;
    const void *key = f.env.Key1U(7);
    uint32_t size = f.env.Size(key);
    uint8_t *copy = (uint8_t *)malloc(size);
    memcpy(copy, key, size);
    Assert(f.store.AddExternal(1000, copy, size) == ImageResult::Ok);

    KeyDef def = Slot(0, 2, 1000, 1000);
    Assert(f.Set(0, 1, &def, 1) == KeyLayoutResult::Ok);
    Assert(f.Page(0).keys[0].img == copy);
    Assert(f.layout.IsActive(KeySlotId(0, 2)));
    Assert(f.store.Find(1000)->key_refs == 2);

    // Clearing the layout releases it; nothing is retired and nothing is freed
    f.layout.Clear();
    Assert(f.store.Find(1000)->key_refs == 0);
    f.store.Free(1000);
    Assert(!f.store.HasRetired() && f.env.sweeps_requested == 0);
    Assert(get_image_width(copy) == KEY_CELL);
    free(copy);
}
