#pragma once

// Page compositor core: the platform-independent part of the compositor
// (records, snapshots, the swap rule, work order, drawing in fill gaps and
// the frame-start negotiation). compositor.cpp runs it on the device;
// firmware/test drives it on the host. Spec: docs/page-compositor.md.

#include <stdint.h>
#include "color.h"
#include "port.h"
#include "screen_layout.h"
#include "key_geometry.h"

#define COMP_BUFFERS      3
#define COMP_MAX_PAGES    32        // pages are tracked in 32-bit masks

// Everything no key covers, and keys with no image. One colour for every page.
#define COMP_BACKGROUND   RGB565(0, 0, 0)

// --- Drawing in fill gaps ---
// Keys are drawn straight into the PSRAM page buffer, only in the gap after a
// bounce fill, in chunks of about COMP_CHUNK_BYTES (at least one row), each
// written back to PSRAM straight away so its bus traffic stays in the gap.
// The gap's deadline is measured from the fill's START: the next fill is due
// one fill period later, and the work stops COMP_GAP_MARGIN_US before that.
// A chunk is started only if its worst case still fits before the deadline,
// so a long fill, or a gap reached late, gets less work. Measured on the
// device (spike v2, docs/page-compositor.md, Drawing a key): a ~1 KB chunk takes up
// to ~82 us with its write-back.
#define COMP_CHUNK_BYTES       1024
#define COMP_GAP_MARGIN_US     65
#define COMP_CHUNK_WORST_US    140       // measured up to 131 us under PSRAM contention (stress run 2026-11-12)
// The status bar (status.cpp) uses a gap only if it gets there this soon
// after the fill ends (the compositor uses the deadline instead).
#define COMP_GAP_FRESH_US      50

// With an offer up, don't start a key unless it can be drawn this long before
// the next frame start. Its time is estimated at COMP_GAP_BYTES_EST per fill.
#define COMP_SWAP_GUARD_US     500
#define COMP_GAP_BYTES_EST     3072

#define COMP_FRAME_PERIOD_DEFAULT_US 16400   // until two frame starts have been seen

// PageChanged() flags
#define COMP_FLUSH        0x01      // hold the page back until all its changes are drawn

#define COMP_NO_PAGE      (-1)

// One key: an opaque image at a rect of the page area. Rects don't overlap.
typedef struct {
    KeyPixelRect rect;      // in the page area
    const void  *img;       // opaque rle565 image exactly rect.w x rect.h, or NULL: COMP_BACKGROUND.
                            // An image whose header doesn't fit the rect (opaque_image_fits) is drawn
                            // as COMP_BACKGROUND too.
} comp_key_t;

// A page as the client defines it now: up to COMP_MAX_KEYS keys. Whatever
// no key covers is COMP_BACKGROUND.
typedef struct {
    int        count;
    comp_key_t keys[COMP_MAX_KEYS];
} comp_page_t;

// A fill gap: when the fill that ended started, and the fill period (the next
// fill is due one period after the start)
typedef struct {
    int64_t fill_start_us;
    int32_t period_us;
} comp_gap_t;

typedef struct {
    int16_t         page;   // COMP_NO_PAGE after a reset: last frame's buffer, no page
    const uint16_t *px;     // rows SCREEN_PAGE_W pixels apart
} comp_view_t;

typedef struct {
    uint32_t keys;          // keys drawn
    uint32_t swaps;         // offers taken by the bounce fill
    uint32_t holds;         // frames shown held (a page asked for was missing)
    uint32_t max_key_us;    // slowest key draw (its chunks' time)
    uint32_t busy_us;       // time spent drawing
    uint32_t erases;        // uncovered areas filled (a buffer took another layout)
    uint32_t chunks;        // chunks drawn (keys and erases)
    uint32_t gaps;          // fill gaps that drew at least one chunk
    uint32_t bytes;         // bytes of page buffer drawn
    uint32_t max_chunk_us;  // slowest chunk, its write-back included (vs COMP_CHUNK_WORST_US)
    uint32_t max_gap_end_us;    // latest end of a gap's work, from its fill's start
                                // (the deadline is period - COMP_GAP_MARGIN_US)
    uint32_t late_gaps;     // gaps whose work ended past the deadline (a chunk over its worst case)
} comp_stats_t;

// Client: copy page `page`'s keys into `out` under the client's own lock.
// Return false if the page doesn't exist. Called on the compositor task.
typedef bool (*comp_get_page_t)(int page, comp_page_t *out, void *ctx);

// Client: is `img` retired (no longer in the model, to be freed after a
// sweep)? Called on the compositor task during a sweep; it must not take the
// lock get_page takes. See docs/image-assets.md, Compositor references.
typedef bool (*comp_is_retired_t)(const void *img, void *ctx);

// The time now in us (esp_timer on the device, simulated in the tests):
// read before each chunk to keep to the gap's deadline
typedef int64_t (*comp_clock_t)(void *ctx);

// What's drawn at one rect of a buffer: an image, the background (img NULL),
// or unknown (matches nothing)
struct CompRecord
{
    KeyPixelRect rect;
    const void  *img;
    bool         known;
};

class PageCompositor
{
public:
    enum class StepResult
    {
        Run,        // did some work, call again
        Wait,       // nothing to do until the next gap, frame start, change or reset
        ResetDone,  // a requested reset is done: no client image is referenced
    };

    // `buffers`: COMP_BUFFERS page buffers, SCREEN_PAGE_W x SCREEN_PAGE_H.
    // Fills them with COMP_BACKGROUND, with no keys. `is_retired` may be NULL
    // (no sweeps).
    void Init(uint16_t *const buffers[COMP_BUFFERS], comp_get_page_t get_page, comp_is_retired_t is_retired,
              comp_clock_t clock, void *ctx);

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

    // Ask for a whole-layout reload. Step() does it next (a key being drawn
    // is abandoned) and returns ResetDone.
    void RequestReset();

    // Drop every reference to images `is_retired` says are retired: records
    // become unknown, a job whose snapshot uses one is dropped, and a key
    // being drawn from one is abandoned. Done by the next Step(). Flag the
    // images retired first. Returns a generation; Swept() reaches it (compare
    // as int32_t) once it's done.
    uint32_t RequestSweep();
    uint32_t Swept() const { return port::AtomicLoad(&m_sweep_done); }

    // --- Compositor task ---

    // One unit of work. `gap` is the last fill gap given to the compositor
    // (NULL if none): while a key is being drawn, Step() draws chunks of it
    // until the gap's deadline (none if it has passed), and that's the only
    // time page buffers are written. Otherwise it does work that doesn't
    // touch them (snapshots, picking the next key, offers, sweeps, resets).
    StepResult Step(int64_t now_us, const comp_gap_t *gap);

    // True while a key is being drawn: the caller should call Step() with
    // each fill gap. Otherwise it needn't wake for fills.
    bool WantsGaps() const { return m_unit.active; }

    // Counters since the last call (resets them).
    void GetStats(comp_stats_t *stats);

    // --- Test support ---

    // Called at points in Step() where a frame start may land (the ISR can
    // interrupt the task anywhere outside the lock). Tests use it to run
    // Frame() mid-step.
    void SetPreemptHook(void (*hook)(void *ctx), void *ctx) { m_preempt = hook; m_preempt_ctx = ctx; }

    int  RecordCount(int buf) const { return m_buf[buf].count; }
    const CompRecord &GetRecord(int buf, int i) const { return m_buf[buf].rec[i]; }
    int  EraseCount(int buf) const { return m_buf[buf].erase_count; }
    KeyPixelRect GetErase(int buf, int i) const { return m_buf[buf].erase[i]; }
    const uint16_t *GetBuffer(int buf) const { return m_buf[buf].px; }
    int  DrawingBuffer() const { return m_drawing; }    // buffer being drawn into, or -1
    int  JobPage() const { return m_job.page; }
    const comp_page_t *JobSnapshot() const { return &m_job.snap; }
    // The image of the key being drawn (NULL if none, or it's the background)
    const void *DrawingImage() const { return m_unit.active && !m_unit.erase ? m_unit.img : nullptr; }
    uint32_t TotalKeys() const { return m_total_keys; }         // keys finished
    uint32_t TotalErases() const { return m_total_erases; }     // uncovered areas filled
    uint32_t TotalChunks() const { return m_total_chunks; }     // chunks drawn

private:
    struct Buffer
    {
        uint16_t    *px;
        CompRecord   rec[COMP_MAX_KEYS];        // its layout: the rects, and what's drawn at each
        int          count;
        // Rects that may still hold pixels outside the layout (a layout
        // change left them): filled with COMP_BACKGROUND, minus the layout's
        // rects, before the buffer is offered. Outside the layout and these,
        // a buffer is always COMP_BACKGROUND.
        KeyPixelRect erase[COMP_MAX_KEYS];
        int          erase_count;
        uint32_t     freed;                     // m_free_clock when it last left the screen
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

    // Records and layouts
    int  FindRect(int b, KeyPixelRect r) const;
    bool MatchesIn(const CompRecord &r, int other) const;
    bool SameLayout(int b, const comp_page_t &page) const;
    bool SameLayoutBufs(int a, int b) const;
    int  DiffKeys(int b, const comp_page_t &page) const;
    int  DiffRecs(int b, int like) const;
    int  ChooseFree(const comp_page_t *page, int like, int front) const;
    int  CopyOfShown(int b) const;
    void Relayout(int b, const comp_page_t &page);
    void AddErase(int b, KeyPixelRect r);

    // Drawing
    int64_t KeyTimeUs(KeyPixelRect r) const;
    void BeginKey(int b, int i, const comp_key_t &key, int offer_page);
    void BeginErase(int b, int offer_page);
    void DrawChunk();
    void EraseRows(uint16_t *dst, int y, int rows) const;
    void FinishUnit();
    void AbortUnit();
    bool GapStep(const comp_gap_t *gap);

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

    int64_t Clock() { return m_clock(m_ctx); }
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
    volatile uint32_t m_st_erases = 0, m_st_chunks = 0, m_st_gaps = 0, m_st_bytes = 0;
    volatile uint32_t m_st_max_chunk_us = 0, m_st_max_gap_end_us = 0, m_st_late_gaps = 0;
    int64_t           m_st_gap_start = INT64_MIN;   // fill start of the last gap counted (task only)

    // --- Compositor task only ---
    comp_get_page_t   m_get_page = nullptr;
    comp_is_retired_t m_is_retired = nullptr;
    comp_clock_t      m_clock = nullptr;
    void             *m_ctx = nullptr;
    Buffer            m_buf[COMP_BUFFERS] = {};
    FrameInfo         m_fi = {};            // copy of m_frame
    uint32_t          m_free_clock = 0;
    uint32_t          m_pending = 0;        // changed pages not yet snapshotted
    uint32_t          m_pending_flush = 0;
    uint32_t          m_offer_mine = 0;     // the offer we put up, 0 = none
    int               m_drawing = -1;
    uint32_t          m_total_keys = 0;
    uint32_t          m_total_erases = 0;
    uint32_t          m_total_chunks = 0;

    // The unit being drawn into a page buffer in fill gaps: a key (one
    // record's rect), or an erase (the buffer's first erase rect, minus its
    // layout). Its record is unknown until it's done.
    struct
    {
        bool         active = false;
        bool         erase = false;
        int          buf = -1;
        int          rec = -1;              // the key's record
        KeyPixelRect rect = {};
        const void  *img = nullptr;
        bool         drawable = false;      // img fits the rect (else drawn as background)
        int          row = 0;               // rows of the rect done
        int          offer_page = COMP_NO_PAGE;     // offer the buffer for this page when done, if the swap rule holds
        uint32_t     cycles = 0;            // drawing time so far
    } m_unit;

    struct
    {
        int         page = COMP_NO_PAGE;    // COMP_NO_PAGE = no job
        bool        flush = false;          // COMP_FLUSH requested
        int         back = -1;              // -1 = not chosen yet
        comp_page_t snap = {};
    } m_job;

    void (*m_preempt)(void *ctx) = nullptr;
    void  *m_preempt_ctx = nullptr;
};
