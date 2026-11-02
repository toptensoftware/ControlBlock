#pragma once

// Test harness for the page compositor: a World holds a PageCompositor, its
// three buffers, a client page model and a simulated bounce fill and clock.
// Every frame start it checks the compositor's invariants (see CheckFrame).

#include <stdint.h>
#include "Core.h"
#include "UnitTesting.h"
#include "rle565.h"
#include "blobpack.h"
#include "page_compositor.h"
#include "screen_view.h"
#include "test_keys.h"

#define TEST_MAX_PAGES  8
#define FRAME_US        16400
#define CHUNK_US        (FRAME_US / SCREEN_CHUNKS)      // one bounce fill

// Thrown (via Check) when an invariant fails, so a failure inside the
// preempt hook unwinds out of the compositor to the test.
#define Check(x) SimpleLib::_Assert(x, #x, __FILE__, __LINE__)

// Small deterministic random number generator
class Rng
{
public:
    explicit Rng(uint32_t seed) : m_s(seed ? seed : 1) {}
    uint32_t Next() { m_s ^= m_s << 13; m_s ^= m_s >> 17; m_s ^= m_s << 5; return m_s; }
    int  Range(int lo, int hi) { return lo + (int)(Next() % (uint32_t)(hi - lo + 1)); }   // inclusive
    bool Chance(int percent) { return (int)(Next() % 100) < percent; }
private:
    uint32_t m_s;
};

// Read firmware/assets/<name> (a blobpack) into memory; exits if missing
BLOBPACK *LoadAsset(const char *name);

// firmware/assets/test_keys.bin (layout in test_keys.h)
class TestImages
{
public:
    static TestImages &Get();
    int IconCount() const { return TEST_KEYS_ICON_COUNT(m_pack->count); }
    const void *Face(int i, bool opaque = false) const { return get_blob(m_pack, TEST_KEYS_FACE(i, opaque)); }
    const void *Icon(int i) const { return get_blob(m_pack, TEST_KEYS_FIRST_ICON + i); }
    size_t Size(const void *img) const { return get_image_packed_size(img); }
private:
    TestImages();
    BLOBPACK *m_pack;
};

class World
{
public:
    explicit World(int num_pages, uint32_t seed = 1);
    ~World();

    PageCompositor comp;
    Rng            rng;
    int            num_pages;

    // --- Client ---

    // Change the keys in `mask` on `page` to new images (fresh copies, so
    // every version has its own pointers), then PageChanged(page, flags).
    void ChangeKeys(int page, uint32_t mask, uint32_t flags);

    // Set one key's images (recorded as a new version). Call PageChanged after.
    void SetKey(int page, int k, const void *bg, const void *fg, uint32_t flags = 0);

    // Whole-layout reload: comp reset, free every old image, new model
    void Reload();

    // --- Display ---

    int     scroll = 0;
    int     key_cost_us  = 600;     // simulated time to render a key into the cell
    int     copy_cost_us = 300;     // simulated time to copy a slice to a page buffer
    int64_t frame_start = 0;

    // Check that page buffers change only in fill gaps, by at most a slice,
    // and only in the buffer being drawn (snapshots every buffer per step:
    // slow, for scenarios only)
    bool check_gap_writes = false;

    // A frame start: ask for the pages at `scroll` and check the result
    void FrameStart();

    // One frame: a frame start, then for each of the SCREEN_CHUNKS bounce
    // fills a gap step (if the compositor isn't busy, as on the device) and
    // other steps until the next fill or it waits
    void RunFrame();
    void RunFrames(int n) { for (int i = 0; i < n; i++) RunFrame(); }

    // While the compositor is drawing, run a frame start at its preempt
    // points with this probability (and client changes with client_percent)
    int preempt_percent = 0;
    int preempt_client_percent = 0;

    // --- What's on screen ---

    comp_view_t views[2] = {};
    int         view_count = 0;
    FrameView   view = {};
    PagesInView pages = {};
    uint32_t    held_frames = 0;
    uint32_t    partial_frames = 0;     // frames showing a page with keys from two versions of a batch

    int  BufferIndex(const uint16_t *px) const;
    int  ShownBuffer(int page) const;    // buffer shown for `page` this frame, or -1
    bool ShowsModel(int page) const;     // page shown and every key matches the model
    bool AtRestOn(int page) const;       // view is page `page` at offset 0
    void CheckSettledOn(int page);       // after quiet frames: shows the model, pixels right

    comp_key_t model[TEST_MAX_PAGES][COMP_KEYS] = {};

private:
    struct Version { const void *bg, *fg; };
    struct Batch { uint32_t mask; bool flush; int version[COMP_KEYS]; };

    static bool GetPage(int page, comp_key_t keys[COMP_KEYS], void *ctx);
    static void PreemptHook(void *ctx);

    // One compositor step at `now`; returns the time after it
    int64_t DoStep(int64_t now, bool gap, PageCompositor::StepResult *result);

    const void *Copy(const void *img);
    void FreeImages();
    void NewModel();
    void CheckFrame();
    void CheckPixels(int b);
    int  FindVersion(int page, int k, const CompRecord &r) const;

    uint16_t *m_buffers[COMP_BUFFERS];
    uint16_t *m_cell;                                        // the compositor's render cell
    uint16_t *m_shadow[COMP_BUFFERS] = {};                   // check_gap_writes snapshots
    SimpleLib::List<void *> m_images;                        // copies to free
    SimpleLib::List<Version> m_history[TEST_MAX_PAGES][COMP_KEYS];
    int  m_shown_version[TEST_MAX_PAGES][COMP_KEYS];
    SimpleLib::List<Batch> m_batches[TEST_MAX_PAGES];       // changes not yet fully shown
    int  m_prev_shown[2] = { -1, -1 };
    int  m_in_hook = 0;
};
