#pragma once

// Page compositor core: the platform-independent part of the compositor
// (records, snapshots, the swap rule, work order and the frame-start
// negotiation). compositor.cpp runs it on the device; firmware/test drives
// it on the host. Spec: docs/page-compositor.md.

#include <stdint.h>
#include "color.h"
#include "port.h"
#include "screen_layout.h"

#define COMP_KEYS         15        // 5 x 3
#define COMP_COLS         5
#define COMP_ROWS         3
#define COMP_KEY_SIZE     128       // cell width and height
#define COMP_BUFFERS      3
#define COMP_MAX_PAGES    32        // pages are tracked in 32-bit masks

// Cell origins in the page area: a grid with the same gap between cells
// across and down, centred in the page. The gap is the largest that leaves
// about half a gap above and below, so it follows the page area's height:
//   800x420 (with the pager): 12 px gap, pitch 140, cells from x = 56, y = 6
//   800x440 (no pager):       18 px gap, pitch 146, cells from x = 44, y = 10
// The margin at each side (half the space between pages) is wider. The
// visible gap between faces adds their padding. Reported to the host in
// GET_INFO.
#define COMP_KEY_GAP      ((SCREEN_PAGE_H - COMP_ROWS * COMP_KEY_SIZE) / COMP_ROWS)
#define COMP_KEY_PITCH    (COMP_KEY_SIZE + COMP_KEY_GAP)
#define COMP_GRID_X0      ((SCREEN_PAGE_W - (COMP_COLS * COMP_KEY_PITCH - COMP_KEY_GAP)) / 2)
#define COMP_GRID_Y0      ((SCREEN_PAGE_H - (COMP_ROWS * COMP_KEY_PITCH - COMP_KEY_GAP)) / 2)
#define COMP_KEY_X(i)     (COMP_GRID_X0 + ((i) % COMP_COLS) * COMP_KEY_PITCH)
#define COMP_KEY_Y(i)     (COMP_GRID_Y0 + ((i) / COMP_COLS) * COMP_KEY_PITCH)

// Gaps between cells and cleared cell rows. One colour for every page.
#define COMP_BACKGROUND   RGB565(0, 0, 0)

// Keys are rendered into a cell in internal RAM, then copied to the page
// buffer in slices of whole rows, one slice in the gap after each bounce
// fill, so PSRAM writes don't compete with the fill's reads. Measured on the
// device (copy + write-back after each fill; see docs/page-compositor.md):
// 8 KB per gap keeps every fill in budget with one page in view, 4 KB with
// two (one of them read cold); 16 KB doesn't.
#define COMP_SLICE_ROWS_ONE_PAGE   32       // 8 KB of key rows
#define COMP_SLICE_ROWS_TWO_PAGES  12       // 3 KB (16 was late with opaque faces at 440 lines)
// A gap is only used if the copy starts this soon after the fill ends: an
// 8 KB copy + write-back takes ~325 us of the ~420 us gap.
#define COMP_GAP_FRESH_US          50

// With an offer up, don't start a key unless it can be finished (rendered
// and all its slices copied) this long before the next frame start.
#define COMP_SWAP_GUARD_US         500
#define COMP_RENDER_EST_US         800      // rendering a key into the cell

#define COMP_FRAME_PERIOD_DEFAULT_US 16400   // until two frame starts have been seen

// PageChanged() flags
#define COMP_FLUSH        0x01      // hold the page back until all its changes are drawn

#define COMP_NO_PAGE      (-1)

// comp_key_t flags
#define COMP_KEY_BG_OPAQUE 0x01     // bg covers every pixel of the cell (see comp_image_is_opaque)

typedef struct {
    const void *bg;     // rle565 image, 128x128, or NULL
    const void *fg;     // rle565 image, 128x128, or NULL
    uint32_t    flags;  // COMP_KEY_*. Set COMP_KEY_BG_OPAQUE only for a bg that passes
                        // comp_image_is_opaque(): the cell isn't cleared before it's drawn
} comp_key_t;

typedef struct {
    int16_t         page;   // COMP_NO_PAGE after a reset: last frame's buffer, no page
    const uint16_t *px;     // rows SCREEN_PAGE_W pixels apart
} comp_view_t;

typedef struct {
    uint32_t keys;          // keys drawn
    uint32_t swaps;         // offers taken by the bounce fill
    uint32_t holds;         // frames shown held (a page asked for was missing)
    uint32_t max_key_us;    // slowest key draw
    uint32_t busy_us;       // time spent drawing keys
} comp_stats_t;

// Client: copy page `page`'s 15 keys into `keys` under the client's own
// lock. Return false if the page doesn't exist. Called on the compositor task.
typedef bool (*comp_get_page_t)(int page, comp_key_t keys[COMP_KEYS], void *ctx);

// Client: is `img` retired (no longer in the model, to be freed after a
// sweep)? Called on the compositor task during a sweep; it must not take the
// lock get_page takes. See docs/image-assets.md, Compositor references.
typedef bool (*comp_is_retired_t)(const void *img, void *ctx);

// Grow [*y0, *y1) by the cell rows a 128x128 rle565 image draws (rle565
// stores only rows firstRow to firstRow + rowCount). Start with *y0 == *y1
// for an empty span.
void comp_add_image_rows(const void *img, int *y0, int *y1);

// Clear rows [y0, y1) of the key cell at `cell` (rows `stride` pixels apart)
// to COMP_BACKGROUND.
void comp_clear_rows(uint16_t *cell, int stride, int y0, int y1);

// True if the rle565 image (`size` bytes) is 128x128 and every pixel is fully
// opaque, so it can be a bg with COMP_KEY_BG_OPAQUE. Decodes the whole image:
// call it once when the image is loaded, not per draw.
bool comp_image_is_opaque(const void *img, size_t size);

// What's drawn in one key cell of a buffer
struct CompRecord
{
    const void *bg, *fg;
    uint8_t     flags;      // the key's COMP_KEY_* flags, so idle work can redraw it
    uint8_t     y0, y1;     // rows drawn, [y0, y1) within the cell; y0 == y1 = none
    bool        known;      // false = unknown, matches nothing
};

class PageCompositor
{
public:
    enum class StepResult
    {
        Run,        // did some work, call again
        Wait,       // nothing to do until the next frame start, change or reset
        ResetDone,  // a requested reset is done: no client image is referenced
    };

    // `buffers`: COMP_BUFFERS page buffers, SCREEN_PAGE_W x SCREEN_PAGE_H.
    // `cell`: COMP_KEY_SIZE x COMP_KEY_SIZE pixels to render keys into
    // (internal RAM on the device). Fills the buffers with COMP_BACKGROUND
    // and marks every record unknown. `is_retired` may be NULL (no sweeps).
    void Init(uint16_t *const buffers[COMP_BUFFERS], uint16_t *cell, comp_get_page_t get_page,
              comp_is_retired_t is_retired, void *ctx);

    // --- Bounce fill, at frame start (ISR) ---

    // Ask for `count` (0-2) pages starting at `first`. `offset` is how far
    // the first page is scrolled off the left edge; it only decides which
    // page to update first when both change. `now_us` is the frame start time.
    // Fills `out` in page order and returns how many buffers to show (0-2):
    //  - every page asked for: show them at the latched scroll
    //  - fewer, or pages not asked for: show out[0] at rest (the hold)
    //  - none: show COMP_BACKGROUND
    int Frame(int first, int count, int offset, int64_t now_us, comp_view_t out[2]);

    // --- Client tasks ---

    // Page `page`'s target has changed. With COMP_FLUSH, call it while
    // still holding the lock the change was made under, so the snapshot
    // that picks the change up also sees the flag.
    void PageChanged(int page, uint32_t flags);

    // Ask for a whole-layout reload. Step() does it at the next key
    // boundary (once a key being copied is finished) and returns ResetDone.
    void RequestReset();

    // Drop every reference to images `is_retired` says are retired: records
    // become unknown, and a job whose snapshot uses one is dropped. Done by
    // Step() at the next key boundary. Flag the images retired first. Returns
    // a generation; Swept() reaches it (compare as int32_t) once it's done.
    uint32_t RequestSweep();
    uint32_t Swept() const { return port::AtomicLoad(&m_sweep_done); }

    // --- Compositor task ---

    // One unit of work: render a key into the cell, or (with `gap`) copy one
    // slice of it to its page buffer, or other work that doesn't touch the
    // page buffers. Pass `gap` only straight after a bounce fill (within
    // COMP_GAP_FRESH_US); it's the only time page buffers are written.
    StepResult Step(int64_t now_us, bool gap);

    // True while a key is being copied: the caller should call Step() with
    // `gap` after each bounce fill. Otherwise it needn't wake for fills.
    bool WantsGaps() const { return m_key.active; }

    // Counters since the last call (resets them).
    void GetStats(comp_stats_t *stats);

    // --- Test support ---

    // Called at points in Step() where a frame start may land (the ISR can
    // interrupt the task anywhere outside the lock). Tests use it to run
    // Frame() mid-step.
    void SetPreemptHook(void (*hook)(void *ctx), void *ctx) { m_preempt = hook; m_preempt_ctx = ctx; }

    const CompRecord &GetRecord(int buf, int key) const { return m_buf[buf].rec[key]; }
    const uint16_t *GetBuffer(int buf) const { return m_buf[buf].px; }
    int  DrawingBuffer() const { return m_drawing; }    // buffer being drawn into, or -1
    int  JobPage() const { return m_job.page; }
    const comp_key_t *JobSnapshot() const { return m_job.snap; }
    const comp_key_t *DrawingKey() const { return m_key.active ? &m_key.content : nullptr; }
    uint32_t TotalKeys() const { return m_total_keys; }         // keys finished
    uint32_t TotalRenders() const { return m_total_renders; }   // keys rendered into the cell
    uint32_t TotalSlices() const { return m_total_slices; }     // slices copied

private:
    struct Buffer
    {
        uint16_t  *px;
        CompRecord rec[COMP_KEYS];
        uint32_t   freed;       // m_free_clock when it last left the screen
    };

    struct Shown
    {
        int16_t page;           // COMP_NO_PAGE after a reset
        int8_t  buf;
    };

    struct FrameInfo
    {
        Shown   shown[2];       // the on-screen set, in page order
        int     shown_count;
        int     asked_first;    // pages asked for at the last frame start
        int     asked_count;
        int     offset;         // first asked page's scroll offset (when 2 asked)
        int64_t time_us;        // last frame start
        int32_t period_us;      // time between the last two frame starts
    };

    // Frame info
    void RefreshFrame();
    bool IsShown(int b) const;
    int  FrontOf(int page) const;
    bool IsAsked(int page) const;

    // The offer
    void PutOffer(int b, int page);
    bool Withdraw();
    bool OfferTaken();
    bool FrameStartNear(int64_t now_us, int64_t needed_us) const;

    // Records and drawing
    int  DiffKeys(int b, const comp_key_t *keys) const;
    int  DiffRecs(int b, int like) const;
    int  ChooseFree(const comp_key_t *keys, int like, int front) const;
    int  CopyOfShown(int b) const;
    int  SliceRows() const;
    void KeyRows(int b, int k, const comp_key_t &key, int *new0, int *new1, int *copy0, int *copy1) const;
    int64_t KeyTimeUs(int b, int k, const comp_key_t &key) const;
    void BeginKey(int b, int k, const comp_key_t &key, int offer_page);
    void CopySlice();
    void FinishKey();

    // Jobs
    void       StartJob(int page);
    void       EndJob();
    void       DropJob();
    int        NextKey(int b, int front, bool flush) const;
    bool       SwapOk(int b, int front) const;
    StepResult JobStep(int64_t now_us);
    bool       PickJob();
    StepResult IdleStep();
    void       DoReset();
    void       DoSweep();

    void Preempt() { if (m_preempt) m_preempt(m_preempt_ctx); }

    // --- Shared with the ISR and client tasks ---
    port::SpinLock m_lock;
    FrameInfo      m_frame = {};            // under m_lock
    uint32_t       m_offer = 0;             // 0, or OFFER(buf, page). port::Atomic* only
    uint32_t       m_changed = 0;           // pages changed (atomic)
    uint32_t       m_changed_flush = 0;     // ...with COMP_FLUSH (atomic)
    uint32_t       m_reset_request = 0;     // atomic
    uint32_t       m_sweep_request = 0;     // atomic: sweeps asked for
    uint32_t       m_sweep_done = 0;        // atomic: the request count at the last sweep
    const uint16_t *m_px[COMP_BUFFERS] = {};
    bool           m_ready = false;

    // Stats (the ISR counts swaps and holds)
    volatile uint32_t m_st_keys = 0, m_st_swaps = 0, m_st_holds = 0, m_st_max_key = 0, m_st_busy = 0;

    // --- Compositor task only ---
    comp_get_page_t m_get_page = nullptr;
    comp_is_retired_t m_is_retired = nullptr;
    void           *m_ctx = nullptr;
    Buffer          m_buf[COMP_BUFFERS] = {};
    FrameInfo       m_fi = {};              // copy of m_frame
    uint32_t        m_free_clock = 0;
    uint32_t        m_pending = 0;          // changed pages not yet snapshotted
    uint32_t        m_pending_flush = 0;
    uint32_t        m_offer_mine = 0;       // the offer we put up, 0 = none
    int             m_drawing = -1;
    uint32_t        m_total_keys = 0;
    uint32_t        m_total_renders = 0;
    uint32_t        m_total_slices = 0;
    uint16_t       *m_cell = nullptr;       // COMP_KEY_SIZE^2, keys are rendered here

    // The key being copied from the cell to a page buffer
    struct
    {
        bool       active = false;
        int        buf = -1, key = -1;
        comp_key_t content = {};
        int        y0 = 0, y1 = 0;          // rows the new content draws (the record's span)
        int        next = 0, end = 0;       // rows still to copy: [next, end)
        int        offer_page = COMP_NO_PAGE;   // offer the buffer for this page when done, if the swap rule holds
        uint32_t   cycles = 0;              // render + copy time so far
    } m_key;

    struct
    {
        int        page = COMP_NO_PAGE;     // COMP_NO_PAGE = no job
        bool       flush = false;           // COMP_FLUSH requested
        int        back = -1;               // -1 = not chosen yet
        comp_key_t snap[COMP_KEYS] = {};
    } m_job;

    void (*m_preempt)(void *ctx) = nullptr;
    void  *m_preempt_ctx = nullptr;
};
