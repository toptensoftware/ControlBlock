#pragma once

// Key layout: the pages of keys (the host's SET_KEYS, the layout blob). Each
// page holds up to COMP_MAX_KEYS keys, each { id, rect in quarter units,
// flags, normal image, pressed image }: one fully opaque image per state, the
// size of the key's rect. Ids are unique across every page. It's the page
// compositor's client: GetPage() gives each key's rect and the image it shows
// (the pressed one while it's held). Platform independent.
// Spec: docs/image-assets.md, Key definitions and references.
//
// Not thread safe: the caller holds the model lock around every call
// (including GetPage, from the compositor's get_page).

#include <stdint.h>
#include "image_store.h"
#include "key_geometry.h"
#include "page_compositor.h"
#include "screen_layout.h"

// KeyDef flags
#define KEYDEF_DISABLED     0x0002      // drawn, but no events and no press feedback
#define KEYDEF_FLAGS        KEYDEF_DISABLED

#define KEY_LAYOUT_MAX_PAGES    SCREEN_MAX_PAGES
#define KEY_LAYOUT_MAX_KEYS     (KEY_LAYOUT_MAX_PAGES * COMP_MAX_KEYS)

// Area icons are designed to fill, centred in a key image (GET_INFO icon_w/h;
// tools/test-icons/gen.js ICON_SIZE). Still being tuned.
#define KEY_ICON_SIZE       80

struct KeyDef
{
    uint16_t id;            // 1..65535, unique across every page
    uint8_t  page;
    KeyRect  rect;          // quarter units from the grid origin
    uint16_t flags;         // KEYDEF_*
    uint32_t normal;        // image IDs: the normal look (0 = none: background)
    uint32_t pressed;       // shown while held (0 = keep the normal one: no press feedback)
};

enum class KeyLayoutResult
{
    Ok,
    BadArg,         // a page outside the range or the page count, or an unknown flag
    NotFound,       // an image ID isn't a live image (and not one the key already holds)
    OutOfGrid,      // a rect is empty or not inside the grid
    Overlap,        // two keys of a page overlap
    TooMany,        // more than COMP_MAX_KEYS on a page
    BadId,          // key id 0
    DuplicateId,    // an id used twice, or by a page outside the range
    BadSize,        // an image's size isn't the key's rect in pixels
};

// A defined key
struct KeyState
{
    uint16_t    id;
    KeyRect     rect;
    uint16_t    flags;
    bool        pressed;            // the finger is on it
    ImageEntry *normal;             // null = none
    ImageEntry *pressed_image;      // null = keeps the normal one
};

// A page's keys changed. `flags` is COMP_FLUSH for host changes, 0 for press
// feedback. Called with the model lock held.
typedef void (*key_layout_page_changed_t)(int page, uint32_t flags, void *ctx);

class KeyLayout : public ImageStoreListener
{
public:
    ~KeyLayout();

    // Starts with one empty page
    void Init(ImageStore *store, key_layout_page_changed_t changed, void *ctx);

    int PageCount() const { return m_page_count; }

    // 1..KEY_LAYOUT_MAX_PAGES. Keys on pages removed are dropped.
    KeyLayoutResult SetPageCount(int count);

    // Pages first..first+pages-1 get exactly the keys in `defs` (each def's
    // page in that range), all or nothing: every entry is checked before
    // anything changes. Pages outside the range are untouched. An image that
    // is no longer live may still be named by the key (same id) that holds it.
    // A key that keeps its id stays pressed. Every page of the range is
    // reported with `change_flags` (host changes appear in one frame; the
    // device's own stress run also uses 0).
    KeyLayoutResult SetKeys(int first, int pages, const KeyDef *defs, int count, uint32_t change_flags = COMP_FLUSH);

    // No keys and one page: no image is referenced afterwards
    void Clear();

    // The finger is on (or off) a key: shows its pressed image, if it has
    // one and isn't disabled. False if there's no such key.
    bool SetPressed(uint16_t id, bool pressed);

    // Defined and not disabled: presses are reported
    bool IsActive(uint16_t id) const;

    // The key with `id` (and its page), or null
    const KeyState *Find(uint16_t id, int *page = nullptr) const;

    // A page's keys (-1 for a page past the count), and their definitions
    // (out: COMP_MAX_KEYS entries)
    int KeyCount(int page) const;
    int GetKeys(int page, KeyDef *out) const;

    // A page's live keys, in place (valid until the layout changes: hold the
    // model lock while reading them), with their count; null and 0 for a page
    // past the count
    const KeyState *PageKeys(int page, int *count) const;

    // comp_get_page_t, minus the lock. An image that isn't the size of its
    // key's rect (replaced by another size) is drawn as background.
    bool GetPage(int page, comp_page_t *out) const;

    // The image a key shows now: pressed (if held, not disabled and it has
    // one), else normal; may be null
    static const ImageEntry *ShownEntry(const KeyState &key);

    // ImageStoreListener: report every page with a key using `entry`
    void OnImageReplaced(ImageEntry *entry) override;

private:
    struct Page
    {
        int      count;
        KeyState keys[COMP_MAX_KEYS];
    };

    KeyState *FindKey(uint16_t id, int *page);
    KeyLayoutResult Check(int first, int pages, const KeyDef *defs, int count);
    KeyLayoutResult CheckImage(uint32_t id, const KeyDef &def, int first, int pages, KeyPixelRect px);
    ImageEntry *Resolve(uint32_t id, uint16_t key_id, int first, int pages);
    void ReleasePage(int page);

    ImageStore           *m_store = nullptr;
    key_layout_page_changed_t m_changed = nullptr;
    void                 *m_ctx = nullptr;
    int                   m_page_count = 1;
    Page                  m_pages[KEY_LAYOUT_MAX_PAGES] = {};
    // SetKeys' scratch (the caller holds the model lock): every id (the defs'
    // then the other pages'; at most one layout's worth once each page's
    // count is checked), and each def's resolved images and held state
    uint16_t              m_ids[KEY_LAYOUT_MAX_KEYS];
    ImageEntry           *m_new[KEY_LAYOUT_MAX_KEYS][2];
    bool                  m_held[KEY_LAYOUT_MAX_KEYS];
};
