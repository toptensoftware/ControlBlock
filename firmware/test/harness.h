#pragma once

// Test harness for the page compositor: a World holds a PageCompositor, its
// three buffers, a client page model and a simulated bounce fill and clock.
// Every frame start it checks the compositor's invariants (see CheckFrame),
// and every step that the page buffers change only in fill gaps, before the
// gap's deadline.

#include <stdint.h>
#include "Core.h"
#include "UnitTesting.h"
#include "rle565.h"
#include "blobpack.h"
#include "page_compositor.h"
#include "screen_view.h"
#include "test_keys.h"

#define TEST_MAX_PAGES  8
#define CHUNK_US        615                             // one bounce fill period (20 MHz pixel clock)
#define FRAME_US        (CHUNK_US * SCREEN_CHUNKS)

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
    // Opaque keys: 1U (KEY_CELL square) and 2U (2 x KEY_CELL wide)
    const void *Key1U(int i) const { return get_blob(m_pack, TEST_KEYS_KEY_1U(m_pack->count, i)); }
    const void *Key2U(int i) const { return get_blob(m_pack, TEST_KEYS_KEY_2U(m_pack->count, i)); }
    size_t Size(const void *img) const { return get_image_packed_size(img); }
private:
    TestImages();
    BLOBPACK *m_pack;
};

// Page layouts the World's pages use (rects from key_geometry.h)
enum TestLayout
{
    LAYOUT_1U,          // 15 x 1U, the whole grid
    LAYOUT_2U,          // rows of 2U and 1U keys, the whole grid
    LAYOUT_SPARSE,      // a few keys, odd sizes with no image, holes
    LAYOUT_COUNT,
};

// The rects of `layout` (no images)
void MakeLayout(TestLayout layout, comp_page_t *page);

class World
{
public:
    // `mixed`: page p gets layout p % LAYOUT_COUNT (else every page LAYOUT_1U)
    explicit World(int num_pages, uint32_t seed = 1, bool mixed = false);
    ~World();

    PageCompositor comp;
    Rng            rng;
    int            num_pages;
    bool           mixed;

    // --- Client ---

    // Change the keys in `mask` (bit = index in the page's layout) on `page`
    // to new images (fresh copies, so every version has its own pointer),
    // then PageChanged(page, flags). Keys whose rect no image fits keep
    // their (no) image.
    void ChangeKeys(int page, uint32_t mask, uint32_t flags);

    // Give `page` a new layout, every key a new image, then PageChanged
    void ChangeLayout(int page, TestLayout layout, uint32_t flags);

    // Set key `k`'s image (recorded as a new version). Call PageChanged after.
    void SetKey(int page, int k, const void *img);

    // Whole-layout reload: comp reset, free every old image, new model
    void Reload();

    // Retire every image the model no longer uses and ask for a sweep. Once
    // the sweep is done (checked each frame), the images are checked to be
    // unreferenced, poisoned and freed; the next new images reuse their
    // addresses straight away, so a stale record would compare equal.
    void RetireUnused();
    int  RetiredPending() const { return m_retired.GetCount(); }
    uint32_t images_freed = 0;

    // --- Display ---

    int     scroll = 0;
    int     chunk_cost_us = 65;     // simulated time to draw and write back a chunk
    int     fill_one_us   = 150;    // a bounce fill, one page in view
    int     fill_two_us   = 190;    // ...two pages
    int     gap_delay_us  = 0;      // the task reaches each gap this long after the fill ends
    int64_t frame_start = 0;

    // Check that page buffers change only in fill gaps, by at most the
    // chunks drawn, and only in the buffer being drawn (snapshots every
    // buffer per step: slow, for scenarios only)
    bool check_gap_writes = false;

    // A frame start: ask for the pages at `scroll` and check the result
    void FrameStart();

    // One frame: a frame start, then for each of the SCREEN_CHUNKS bounce
    // fills, steps from the end of the fill (plus gap_delay_us) until the
    // next fill or the compositor waits
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
    uint32_t    max_chunks_per_gap = 0;

    int  BufferIndex(const uint16_t *px) const;
    int  ShownBuffer(int page) const;    // buffer shown for `page` this frame, or -1
    bool ShowsModel(int page) const;     // page shown and every key matches the model
    bool Matches(int buf, int page) const;   // buffer `buf` holds the model of `page`, nothing left to fill
    bool AtRestOn(int page) const;       // view is page `page` at offset 0
    void CheckSettledOn(int page);       // after quiet frames: shows the model, pixels right

    // Buffer `b`'s pixels: every known record's image (or background), the
    // background everywhere outside the records and erase rects. With
    // `strict`, nothing may be left to erase.
    void CheckPixels(int b, bool strict);

    comp_page_t model[TEST_MAX_PAGES] = {};

private:
    struct Version { KeyPixelRect rect; const void *img; };
    struct Seen { KeyPixelRect rect; int version; };
    struct Batch { bool flush; int count; KeyPixelRect rect[COMP_MAX_KEYS]; int version[COMP_MAX_KEYS]; };
    struct Layout { int count; KeyPixelRect rect[COMP_MAX_KEYS]; };
    struct Retired { void *img; uint32_t sweep; };

    static bool GetPage(int page, comp_page_t *out, void *ctx);
    static bool IsRetired(const void *img, void *ctx);
    static int64_t Clock(void *ctx);
    static void PreemptHook(void *ctx);
    void CollectSwept();
    bool InModel(const void *img) const;
    void Release(void *img);            // poison, and keep for reuse

    // One compositor step at `now`; returns the time after it
    int64_t DoStep(int64_t now, const comp_gap_t *gap, PageCompositor::StepResult *result);

    const void *Copy(const void *img);
    const void *NewImage(KeyPixelRect r);   // a fresh copy of a test key that fits `r`, or NULL
    void FreeImages();
    void NewModel();
    void SetLayout(int page, TestLayout layout);
    void CheckFrame();
    int  FindVersion(int page, KeyPixelRect r, const void *img) const;
    int  LastSeen(int page, KeyPixelRect r) const;
    void SetSeen(int page, KeyPixelRect r, int version);
    bool KnownLayout(int page, int b) const;

    uint16_t *m_buffers[COMP_BUFFERS];
    uint16_t *m_shadow[COMP_BUFFERS] = {};                   // check_gap_writes snapshots
    SimpleLib::List<void *> m_images;                        // copies to free
    SimpleLib::List<void *> m_reuse;                         // freed copies, handed out again first
    SimpleLib::List<Retired> m_retired;                      // waiting for a sweep
    bool m_sweep_requested = false;                          // records on screen may be unknown
    SimpleLib::List<Version> m_history[TEST_MAX_PAGES];      // every key version a page has had
    SimpleLib::List<Seen>    m_seen[TEST_MAX_PAGES];         // newest version shown per rect
    SimpleLib::List<Batch>   m_batches[TEST_MAX_PAGES];      // changes not yet fully shown
    SimpleLib::List<Layout>  m_layouts[TEST_MAX_PAGES];      // every layout a page has had
    int  m_prev_shown[2] = { -1, -1 };
    int  m_in_hook = 0;
    int64_t  m_clock_base = 0;                               // simulated clock: the step's start...
    uint32_t m_clock_chunks = 0;                             // ...plus chunk_cost_us per chunk since
};
