#pragma once

// Key layout: the pages of keys the host defines (SET_KEYS), each key naming
// four images by ID. It's the page compositor's client: GetPage() gives the
// compositor each key's bg/fg pixels, the pressed pair while a key is held.
// Platform independent. Spec: docs/image-assets.md, Key definitions.
//
// Not thread safe: the caller holds the model lock around every call
// (including GetPage, from the compositor's get_page).

#include <stdint.h>
#include "image_store.h"
#include "page_compositor.h"
#include "screen_layout.h"

#define KEY_SLOTS           4
#define KEY_SLOT_BG         0
#define KEY_SLOT_FG         1
#define KEY_SLOT_PRESSED_BG 2
#define KEY_SLOT_PRESSED_FG 3

// KeyDef flags (protocol.md, SET_KEYS)
#define KEYDEF_EMPTY        0x0001      // blank and inactive; every ID must be 0
#define KEYDEF_DISABLED     0x0002      // drawn, but no events and no press feedback

#define KEY_LAYOUT_MAX_PAGES    SCREEN_MAX_PAGES

// Area icons are designed to fill, centred in a key image (GET_INFO icon_w/h;
// tools/test-icons/gen.js ICON_SIZE). Still being tuned.
#define KEY_ICON_SIZE       80

struct KeyDef
{
    uint8_t  page;
    uint8_t  key;
    uint16_t flags;
    uint32_t ids[KEY_SLOTS];        // bg, fg, pressed bg, pressed fg; 0 = none
};

enum class KeyLayoutResult
{
    Ok,
    BadArg,         // page, key or flags out of range
    NotFound,       // an ID isn't a live image
};

// A page's keys changed. `flags` is COMP_FLUSH for host changes, 0 for press
// feedback. Called with the model lock held.
typedef void (*key_layout_page_changed_t)(int page, uint32_t flags, void *ctx);

class KeyLayout : public ImageStoreListener
{
public:
    ~KeyLayout();

    // Starts with one page of empty keys
    void Init(ImageStore *store, key_layout_page_changed_t changed, void *ctx);

    int PageCount() const { return m_page_count; }

    // 1..KEY_LAYOUT_MAX_PAGES. Keys on pages removed are cleared.
    KeyLayoutResult SetPageCount(int count);

    // Define keys, all or nothing: every entry is checked before any changes.
    // Pages changed are reported with `change_flags` (host changes appear
    // in one frame; the device's own stress run also uses 0).
    KeyLayoutResult SetKeys(const KeyDef *defs, int count, uint32_t change_flags = COMP_FLUSH);

    // Every key empty and one page: no image is referenced afterwards
    void Clear();

    // The finger is on (or off) a key: shows its pressed pair, if it has
    // one and isn't disabled. False if there's no such key.
    bool SetPressed(int page, int key, bool pressed);

    // Defined, not empty and not disabled: presses are reported to the host
    bool IsActive(int page, int key) const;

    // comp_get_page_t, minus the lock
    bool GetPage(int page, comp_key_t keys[COMP_KEYS]) const;

    // ImageStoreListener: switch every key using `entry` to its new pixels
    void OnImageReplaced(ImageEntry *entry) override;

    // --- Test support ---
    ImageEntry *Slot(int page, int key, int slot) const { return m_keys[page][key].slot[slot]; }

private:
    struct Key
    {
        uint16_t    flags;
        bool        pressed;
        ImageEntry *slot[KEY_SLOTS];
        comp_key_t  comp;           // what the compositor draws
    };

    bool ValidKey(int page, int key) const;
    void Rebuild(Key &key);
    void Assign(Key &key, uint16_t flags, ImageEntry *const slots[KEY_SLOTS]);
    void ClearPage(int page);

    ImageStore           *m_store = nullptr;
    key_layout_page_changed_t m_changed = nullptr;
    void                 *m_ctx = nullptr;
    int                   m_page_count = 1;
    Key                   m_keys[KEY_LAYOUT_MAX_PAGES][COMP_KEYS] = {};
};
