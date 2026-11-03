// Key layout. Spec: docs/image-assets.md, Key definitions and references.

#include <string.h>
#include "key_layout.h"

// Release every image without telling anyone (the owner may be half gone)
KeyLayout::~KeyLayout()
{
    if (!m_store)
        return;
    for (int p = 0; p < LAYOUT_MAX_PAGES; p++)
        ClearPage(p);
}

void KeyLayout::Init(ImageStore *store, layout_page_changed_t changed, void *ctx)
{
    m_store      = store;
    m_changed    = changed;
    m_ctx        = ctx;
    m_page_count = 1;
    for (int p = 0; p < LAYOUT_MAX_PAGES; p++)
        for (int k = 0; k < COMP_KEYS; k++)
            m_keys[p][k].flags = KEYDEF_EMPTY;
}

bool KeyLayout::ValidKey(int page, int key) const
{
    return page >= 0 && page < m_page_count && key >= 0 && key < COMP_KEYS;
}

// What the compositor draws: the pressed pair while held (if the key has
// one and isn't disabled), else the normal pair
void KeyLayout::Rebuild(Key &key)
{
    bool pressed = key.pressed && !(key.flags & KEYDEF_DISABLED) &&
                   (key.slot[KEY_SLOT_PRESSED_BG] || key.slot[KEY_SLOT_PRESSED_FG]);
    ImageEntry *bg = key.slot[pressed ? KEY_SLOT_PRESSED_BG : KEY_SLOT_BG];
    ImageEntry *fg = key.slot[pressed ? KEY_SLOT_PRESSED_FG : KEY_SLOT_FG];
    key.comp.bg    = bg ? bg->pixels : nullptr;
    key.comp.fg    = fg ? fg->pixels : nullptr;
    key.comp.flags = bg && bg->opaque ? COMP_KEY_BG_OPAQUE : 0;
}

// New references first, then the old ones go, so an image the key keeps
// never drops to no references in between
void KeyLayout::Assign(Key &key, uint16_t flags, ImageEntry *const slots[KEY_SLOTS])
{
    ImageEntry *old[KEY_SLOTS];
    memcpy(old, key.slot, sizeof(old));
    for (int s = 0; s < KEY_SLOTS; s++)
        if (slots[s])
            m_store->AddRef(slots[s]);
    for (int s = 0; s < KEY_SLOTS; s++) {
        key.slot[s] = slots[s];
        if (old[s])
            m_store->Release(old[s]);
    }
    key.flags = flags;
    Rebuild(key);
}

void KeyLayout::ClearPage(int page)
{
    static ImageEntry *const none[KEY_SLOTS] = {};
    for (int k = 0; k < COMP_KEYS; k++) {
        Assign(m_keys[page][k], KEYDEF_EMPTY, none);
        m_keys[page][k].pressed = false;
    }
}

LayoutResult KeyLayout::SetPageCount(int count)
{
    if (count < 1 || count > LAYOUT_MAX_PAGES)
        return LayoutResult::BadArg;
    for (int p = count; p < m_page_count; p++)
        ClearPage(p);
    m_page_count = count;
    return LayoutResult::Ok;
}

LayoutResult KeyLayout::SetKeys(const KeyDef *defs, int count, uint32_t change_flags)
{
    // Check everything first
    for (int i = 0; i < count; i++) {
        const KeyDef &d = defs[i];
        if (!ValidKey(d.page, d.key))
            return LayoutResult::BadArg;
        for (int s = 0; s < KEY_SLOTS; s++) {
            if (d.ids[s] == IMAGE_ID_NONE)
                continue;
            if (d.flags & KEYDEF_EMPTY)
                return LayoutResult::BadArg;
            if (!m_store->IsLive(d.ids[s]))
                return LayoutResult::NotFound;
        }
    }

    uint32_t changed = 0;
    for (int i = 0; i < count; i++) {
        const KeyDef &d = defs[i];
        ImageEntry *slots[KEY_SLOTS];
        for (int s = 0; s < KEY_SLOTS; s++)
            slots[s] = d.ids[s] ? m_store->Find(d.ids[s]) : nullptr;
        Assign(m_keys[d.page][d.key], d.flags, slots);
        changed |= 1u << d.page;
    }

    for (int p = 0; p < m_page_count; p++)
        if (changed & (1u << p))
            m_changed(p, change_flags, m_ctx);
    return LayoutResult::Ok;
}

void KeyLayout::Clear()
{
    int pages = m_page_count;
    for (int p = 0; p < pages; p++)
        ClearPage(p);
    m_page_count = 1;
    if (m_changed)
        m_changed(0, COMP_FLUSH, m_ctx);
}

bool KeyLayout::SetPressed(int page, int key, bool pressed)
{
    if (!ValidKey(page, key))
        return false;
    Key &k = m_keys[page][key];
    if (k.pressed == pressed)
        return true;
    const void *bg = k.comp.bg, *fg = k.comp.fg;
    k.pressed = pressed;
    Rebuild(k);
    if (k.comp.bg != bg || k.comp.fg != fg)
        m_changed(page, 0, m_ctx);
    return true;
}

bool KeyLayout::IsActive(int page, int key) const
{
    return ValidKey(page, key) && !(m_keys[page][key].flags & (KEYDEF_EMPTY | KEYDEF_DISABLED));
}

bool KeyLayout::GetPage(int page, comp_key_t keys[COMP_KEYS]) const
{
    if (page < 0 || page >= m_page_count)
        return false;
    for (int k = 0; k < COMP_KEYS; k++)
        keys[k] = m_keys[page][k].comp;
    return true;
}

void KeyLayout::OnImageReplaced(ImageEntry *entry)
{
    for (int p = 0; p < m_page_count; p++) {
        bool changed = false;
        for (int k = 0; k < COMP_KEYS; k++) {
            Key &key = m_keys[p][k];
            for (int s = 0; s < KEY_SLOTS; s++) {
                if (key.slot[s] == entry) {
                    Rebuild(key);
                    changed = true;
                    break;
                }
            }
        }
        if (changed)
            m_changed(p, COMP_FLUSH, m_ctx);
    }
}
