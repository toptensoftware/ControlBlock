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

    // Register the images of `pages` pages (laid out from `first_page`) and build
    // their KeyDefs. `src(page, key, slot)` gives a key's image (offset 0 = none),
    // `page` counting from 0 within this staging. Images already live are not
    // re-registered. Returns false on a refusal (the caller frees `st.added`).
    template <class Src>
    bool Stage(int first_page, int pages, const Src &src, const uint8_t *blob, ImageStore *store, Staged &st)
    {
        for (int p = 0; p < pages; p++) {
            for (int k = 0; k < COMP_KEYS; k++) {
                KeyDef def = {};
                def.page = (uint8_t)(first_page + p);
                def.key  = (uint8_t)k;
                bool any = false;
                for (int s = 0; s < KEY_SLOTS; s++) {
                    LayoutImage im = src(p, k, s);
                    if (!im.offset)
                        continue;
                    any = true;
                    bool known = store->IsLive(im.offset);
                    if (!known) {
                        if (store->AddExternal(im.offset, blob + im.offset, im.size) != ImageResult::Ok)
                            return false;
                        st.added.Add(im.offset);
                    }
                    def.ids[s] = im.offset;
                }
                def.flags = any ? 0 : KEYDEF_EMPTY;
                st.defs.Add(def);
            }
        }
        return true;
    }

    void Unstage(Staged &st, ImageStore *store)
    {
        // Nothing uses these yet (SetKeys is all or nothing), so they go at once
        for (int i = 0; i < st.added.GetCount(); i++)
            store->Free(st.added[i]);
    }

    // A packed key's image in `slot`, by its offset in the blob (relocated blob:
    // the pointer minus the blob's address)
    LayoutImage PackedImage(const layoutdata::key &key, int slot, const uint8_t *blob)
    {
        const uint8_t *k = (const uint8_t *)&key;
        uint32_t count, ptr;
        memcpy(&count, k + LayoutKeyImageOffset(slot), 4);
        memcpy(&ptr, k + LayoutKeyImagePtrOffset(slot), 4);
        if (!count || !ptr)
            return {};
        return LayoutImage{ ptr - (uint32_t)(uintptr_t)blob, count };
    }

    // Add every image ID of `keys` (COMP_KEYS of them) to `ids`
    void CollectIds(const layoutdata::key *keys, const uint8_t *blob, Set<uint32_t> &ids)
    {
        for (int k = 0; k < COMP_KEYS; k++)
            for (int s = 0; s < KEY_SLOTS; s++) {
                uint32_t id = PackedImage(keys[k], s, blob).offset;
                if (id)
                    ids.Add(id);
            }
    }

    void CollectActivityIds(const layoutdata::activity &a, const uint8_t *blob, Set<uint32_t> &ids)
    {
        for (uint32_t p = 0; p < a.pages_count; p++)
            CollectIds(a.pages[p].keys, blob, ids);
    }
}

bool LayoutApplyHome(const LayoutSummary &summary, const uint8_t *blob, ImageStore *store, KeyLayout *keys)
{
    Staged st;
    bool ok = Stage(0, 1, [&](int, int k, int s) { return summary.home[k][s]; }, blob, store, st);
    ok = ok && keys->SetKeys(st.defs.GetBuffer(), st.defs.GetCount()) == KeyLayoutResult::Ok;
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
    if (pages > 0) {
        const layoutdata::activity &a = root->activities[to];
        ok = Stage(1, pages, [&](int p, int k, int s) { return PackedImage(a.pages[p].keys[k], s, blob); }, blob, store, st);
    }
    // Past this point the key layout calls can't fail: the IDs are live and the ranges checked
    ok = ok && keys->SetPageCount(1 + pages) == KeyLayoutResult::Ok;
    ok = ok && (pages == 0 || keys->SetKeys(st.defs.GetBuffer(), st.defs.GetCount()) == KeyLayoutResult::Ok);
    if (!ok) {
        Unstage(st, store);
        return false;
    }

    // Free the old activity's images that Home and the new activity don't use
    if (from >= 0) {
        Set<uint32_t> keep, old;
        CollectIds(root->homePage.keys, blob, keep);
        if (to >= 0)
            CollectActivityIds(root->activities[to], blob, keep);
        CollectActivityIds(root->activities[from], blob, old);
        for (auto it = old.Iterate(); it.Next(); )
            if (!keep.Contains(it.Get()))
                store->Free(it.Get());
    }
    return true;
}
