// Layout apply. Spec: docs/standalone.md.

#include "layout_apply.h"
#include "Core.h"

using namespace SimpleLib;

namespace
{
    // Keys staged for one SetKeys, and the images this staging newly registered
    // (freed again if the keys are not changed after all)
    struct Staged
    {
        List<KeyDef>   defs;
        List<uint32_t> added;
    };

    // Register key `k`'s images (those not live yet) and stage its KeyDef on
    // `page`. False on a refusal (the caller frees `st.added`).
    bool StageKey(int page, const LayoutKey &k, const uint8_t *blob, ImageStore *store, Staged &st)
    {
        const LayoutImage *images[] = { &k.normal, &k.pressed };
        for (const LayoutImage *im : images) {
            if (!im->offset || store->IsLive(im->offset))
                continue;
            if (store->AddExternal(im->offset, blob + im->offset, im->size) != ImageResult::Ok)
                return false;
            st.added.Add(im->offset);
        }
        KeyDef d = {};
        d.id      = k.id;
        d.page    = (uint8_t)page;
        d.rect    = k.rect;
        d.normal  = k.normal.offset;
        d.pressed = k.pressed.offset;
        st.defs.Add(d);
        return true;
    }

    void Unstage(Staged &st, ImageStore *store)
    {
        // Nothing uses these yet (SetKeys is all or nothing), so they go at once
        for (int i = 0; i < st.added.GetCount(); i++)
            store->Free(st.added[i]);
    }

    // Add every image ID of `count` packed keys to `ids`
    void CollectIds(const layoutdata::key *keys, uint32_t count, const uint8_t *blob, Set<uint32_t> &ids)
    {
        for (uint32_t i = 0; i < count; i++) {
            LayoutKey k = LayoutKeyOf(keys[i], blob);
            if (k.normal.offset)
                ids.Add(k.normal.offset);
            if (k.pressed.offset)
                ids.Add(k.pressed.offset);
        }
    }

    void CollectActivityIds(const layoutdata::activity &a, const uint8_t *blob, Set<uint32_t> &ids)
    {
        for (uint32_t p = 0; p < a.pages_count; p++)
            CollectIds(a.pages[p].keys, a.pages[p].keys_count, blob, ids);
    }
}

bool LayoutApplyHome(const LayoutSummary &summary, const uint8_t *blob, ImageStore *store, KeyLayout *keys)
{
    Staged st;
    bool ok = summary.home_count <= COMP_MAX_KEYS;
    for (uint32_t i = 0; ok && i < summary.home_count; i++)
        ok = StageKey(0, summary.home[i], blob, store, st);
    ok = ok && keys->SetKeys(0, 1, st.defs.GetBuffer(), st.defs.GetCount()) == KeyLayoutResult::Ok;
    if (!ok)
        Unstage(st, store);
    return ok;
}

bool LayoutApplyActivity(const layoutdata::layoutRoot *root, const uint8_t *blob, int from, int to,
                         ImageStore *store, KeyLayout *keys)
{
    if (from < -1 || from >= (int)root->activities_count || to < -1 || to >= (int)root->activities_count)
        return false;
    int pages = to < 0 ? 0 : (int)root->activities[to].pages_count;
    if (pages > LAYOUT_MAX_ACTIVITY_PAGES)
        return false;

    // Register the new images first: a refusal leaves everything as it was
    Staged st;
    bool ok = true;
    for (int p = 0; ok && p < pages; p++) {
        const layoutdata::page &pg = root->activities[to].pages[p];
        for (uint32_t i = 0; ok && i < pg.keys_count; i++)
            ok = StageKey(1 + p, LayoutKeyOf(pg.keys[i], blob), blob, store, st);
    }
    // Past this point the key layout calls can't fail: the IDs are live, the
    // ranges checked, and the validated blob's rects, ids (unique across the
    // layout, so none clashes with Home's) and image sizes are good
    ok = ok && keys->SetPageCount(1 + pages) == KeyLayoutResult::Ok;
    ok = ok && keys->SetKeys(1, pages, st.defs.GetBuffer(), st.defs.GetCount()) == KeyLayoutResult::Ok;
    if (!ok) {
        Unstage(st, store);
        return false;
    }

    // Free the old activity's images that Home and the new activity don't use
    if (from >= 0) {
        Set<uint32_t> keep, old;
        CollectIds(root->homePage.keys, root->homePage.keys_count, blob, keep);
        if (to >= 0)
            CollectActivityIds(root->activities[to], blob, keep);
        CollectActivityIds(root->activities[from], blob, old);
        for (auto it = old.Iterate(); it.Next(); )
            if (!keep.Contains(it.Get()))
                store->Free(it.Get());
    }
    return true;
}
