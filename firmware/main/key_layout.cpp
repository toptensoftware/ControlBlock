// Key layout. Spec: docs/image-assets.md, Key definitions and references.

#include <string.h>
#include "key_layout.h"

namespace
{
    bool InRange(int page, int first, int pages)
    {
        return page >= first && page < first + pages;
    }

    KeyLayoutResult ResultOf(KeyGeomResult r)
    {
        switch (r) {
        case KEY_GEOM_OK:           return KeyLayoutResult::Ok;
        case KEY_GEOM_OUT_OF_GRID:  return KeyLayoutResult::OutOfGrid;
        case KEY_GEOM_OVERLAP:      return KeyLayoutResult::Overlap;
        case KEY_GEOM_TOO_MANY:     return KeyLayoutResult::TooMany;
        case KEY_GEOM_BAD_ID:       return KeyLayoutResult::BadId;
        case KEY_GEOM_DUPLICATE_ID: return KeyLayoutResult::DuplicateId;
        }
        return KeyLayoutResult::BadArg;
    }
}

// Release every image without telling anyone (the owner may be half gone)
KeyLayout::~KeyLayout()
{
    if (!m_store)
        return;
    for (int p = 0; p < KEY_LAYOUT_MAX_PAGES; p++)
        ReleasePage(p);
}

void KeyLayout::Init(ImageStore *store, key_layout_page_changed_t changed, void *ctx)
{
    m_store      = store;
    m_changed    = changed;
    m_ctx        = ctx;
    m_page_count = 1;
    for (int p = 0; p < KEY_LAYOUT_MAX_PAGES; p++)
        m_pages[p].count = 0;
}

void KeyLayout::ReleasePage(int page)
{
    Page &pg = m_pages[page];
    for (int i = 0; i < pg.count; i++) {
        if (pg.keys[i].normal)
            m_store->Release(pg.keys[i].normal);
        if (pg.keys[i].pressed_image)
            m_store->Release(pg.keys[i].pressed_image);
    }
    pg.count = 0;
}

KeyState *KeyLayout::FindKey(uint16_t id, int *page)
{
    if (id == KEY_ID_NONE)
        return nullptr;
    for (int p = 0; p < m_page_count; p++) {
        for (int i = 0; i < m_pages[p].count; i++) {
            if (m_pages[p].keys[i].id == id) {
                if (page)
                    *page = p;
                return &m_pages[p].keys[i];
            }
        }
    }
    return nullptr;
}

const KeyState *KeyLayout::Find(uint16_t id, int *page) const
{
    return const_cast<KeyLayout *>(this)->FindKey(id, page);
}

// The entry image `id` names for key `key_id`: a live image, or one the key
// (on a page being replaced) already holds
ImageEntry *KeyLayout::Resolve(uint32_t id, uint16_t key_id, int first, int pages)
{
    if (id == IMAGE_ID_NONE)
        return nullptr;
    if (m_store->IsLive(id))
        return m_store->Find(id);
    int p = -1;
    KeyState *old = FindKey(key_id, &p);
    if (!old || !InRange(p, first, pages))
        return nullptr;
    if (old->normal && old->normal->id == id)
        return old->normal;
    if (old->pressed_image && old->pressed_image->id == id)
        return old->pressed_image;
    return nullptr;
}

KeyLayoutResult KeyLayout::CheckImage(uint32_t id, const KeyDef &def, int first, int pages, KeyPixelRect px)
{
    if (id == IMAGE_ID_NONE)
        return KeyLayoutResult::Ok;
    ImageEntry *e = Resolve(id, def.id, first, pages);
    if (!e)
        return KeyLayoutResult::NotFound;
    if (e->width != px.w || e->height != px.h)
        return KeyLayoutResult::BadSize;
    return KeyLayoutResult::Ok;
}

KeyLayoutResult KeyLayout::Check(int first, int pages, const KeyDef *defs, int count)
{
    if (first < 0 || pages < 0 || first + pages > m_page_count || count < 0 || (count > 0 && !defs))
        return KeyLayoutResult::BadArg;
    for (int i = 0; i < count; i++)
        if (!InRange(defs[i].page, first, pages) || (defs[i].flags & ~KEYDEF_FLAGS))
            return KeyLayoutResult::BadArg;

    // Each page's rects (which also bounds the ids below to one layout's worth)
    for (int p = first; p < first + pages; p++) {
        KeyRect rects[COMP_MAX_KEYS];
        int n = 0;
        for (int i = 0; i < count; i++) {
            if (defs[i].page != p)
                continue;
            if (n == COMP_MAX_KEYS)
                return KeyLayoutResult::TooMany;
            rects[n++] = defs[i].rect;
        }
        KeyLayoutResult r = ResultOf(KeyRectsCheck(rects, n));
        if (r != KeyLayoutResult::Ok)
            return r;
    }

    // Ids: unique among the defs and against the pages outside the range
    int n = 0;
    for (int i = 0; i < count; i++)
        m_ids[n++] = defs[i].id;
    for (int p = 0; p < m_page_count; p++)
        if (!InRange(p, first, pages))
            for (int i = 0; i < m_pages[p].count; i++)
                m_ids[n++] = m_pages[p].keys[i].id;
    KeyLayoutResult r = ResultOf(KeyIdsCheck(m_ids, count, m_ids + count, n - count));
    if (r != KeyLayoutResult::Ok)
        return r;

    // Images: live (or held by the key), the size of the rect
    for (int i = 0; i < count; i++) {
        KeyPixelRect px = KeyRectPixels(defs[i].rect);
        if ((r = CheckImage(defs[i].normal, defs[i], first, pages, px)) != KeyLayoutResult::Ok ||
            (r = CheckImage(defs[i].pressed, defs[i], first, pages, px)) != KeyLayoutResult::Ok)
            return r;
    }
    return KeyLayoutResult::Ok;
}

KeyLayoutResult KeyLayout::SetKeys(int first, int pages, const KeyDef *defs, int count, uint32_t change_flags)
{
    KeyLayoutResult r = Check(first, pages, defs, count);
    if (r != KeyLayoutResult::Ok)
        return r;

    // New references first (resolved while the old keys still hold theirs),
    // then the old ones go, so an image a key keeps never drops to no
    // references in between
    for (int i = 0; i < count; i++) {
        const KeyDef &d = defs[i];
        m_new[i][0] = Resolve(d.normal, d.id, first, pages);
        m_new[i][1] = Resolve(d.pressed, d.id, first, pages);
        for (int s = 0; s < 2; s++)
            if (m_new[i][s])
                m_store->AddRef(m_new[i][s]);
        const KeyState *old = FindKey(d.id, nullptr);
        m_held[i] = old && old->pressed;        // (only pages in the range can have it: ids checked)
    }
    for (int p = first; p < first + pages; p++)
        ReleasePage(p);
    for (int i = 0; i < count; i++) {
        const KeyDef &d = defs[i];
        Page &pg = m_pages[d.page];
        pg.keys[pg.count++] = KeyState{ d.id, d.rect, d.flags, m_held[i], m_new[i][0], m_new[i][1] };
    }

    for (int p = first; p < first + pages; p++)
        m_changed(p, change_flags, m_ctx);
    return KeyLayoutResult::Ok;
}

KeyLayoutResult KeyLayout::SetPageCount(int count)
{
    if (count < 1 || count > KEY_LAYOUT_MAX_PAGES)
        return KeyLayoutResult::BadArg;
    for (int p = count; p < m_page_count; p++)
        ReleasePage(p);
    m_page_count = count;
    return KeyLayoutResult::Ok;
}

void KeyLayout::Clear()
{
    for (int p = 0; p < KEY_LAYOUT_MAX_PAGES; p++)
        ReleasePage(p);
    m_page_count = 1;
    if (m_changed)
        m_changed(0, COMP_FLUSH, m_ctx);
}

const ImageEntry *KeyLayout::ShownEntry(const KeyState &key)
{
    if (key.pressed && key.pressed_image && !(key.flags & KEYDEF_DISABLED))
        return key.pressed_image;
    return key.normal;
}

bool KeyLayout::SetPressed(uint16_t id, bool pressed)
{
    int page = -1;
    KeyState *k = FindKey(id, &page);
    if (!k)
        return false;
    if (k->pressed == pressed)
        return true;
    const ImageEntry *before = ShownEntry(*k);
    k->pressed = pressed;
    if (ShownEntry(*k) != before)
        m_changed(page, 0, m_ctx);
    return true;
}

bool KeyLayout::IsActive(uint16_t id) const
{
    const KeyState *k = Find(id);
    return k && !(k->flags & KEYDEF_DISABLED);
}

int KeyLayout::KeyCount(int page) const
{
    return page >= 0 && page < m_page_count ? m_pages[page].count : -1;
}

const KeyState *KeyLayout::PageKeys(int page, int *count) const
{
    int n = KeyCount(page);
    *count = n < 0 ? 0 : n;
    return n < 0 ? nullptr : m_pages[page].keys;
}

int KeyLayout::GetKeys(int page, KeyDef *out) const
{
    int n = KeyCount(page);
    for (int i = 0; i < n; i++) {
        const KeyState &k = m_pages[page].keys[i];
        KeyDef d = {};
        d.id      = k.id;
        d.page    = (uint8_t)page;
        d.rect    = k.rect;
        d.flags   = k.flags;
        d.normal  = k.normal ? k.normal->id : IMAGE_ID_NONE;
        d.pressed = k.pressed_image ? k.pressed_image->id : IMAGE_ID_NONE;
        out[i] = d;
    }
    return n;
}

bool KeyLayout::GetPage(int page, comp_page_t *out) const
{
    int n = KeyCount(page);
    if (n < 0)
        return false;
    out->count = n;
    for (int i = 0; i < n; i++) {
        const KeyState &k = m_pages[page].keys[i];
        KeyPixelRect px = KeyRectPixels(k.rect);
        const ImageEntry *e = ShownEntry(k);
        bool fits = e && e->width == px.w && e->height == px.h;
        out->keys[i] = comp_key_t{ px, fits ? e->pixels : nullptr };
    }
    return true;
}

void KeyLayout::OnImageReplaced(ImageEntry *entry)
{
    for (int p = 0; p < m_page_count; p++) {
        bool uses = false;
        for (int i = 0; i < m_pages[p].count && !uses; i++)
            uses = m_pages[p].keys[i].normal == entry || m_pages[p].keys[i].pressed_image == entry;
        if (uses)
            m_changed(p, COMP_FLUSH, m_ctx);
    }
}
