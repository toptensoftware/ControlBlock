// Page compositor core. Spec: docs/page-compositor.md.
//
// The task side (Step) owns the buffers' pixels and records, the snapshot
// and the job. The ISR side (Frame) only touches the offer (one atomic word)
// and the frame info (under m_lock).

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "rle565.h"
#include "fill_px.h"
#include "opaque_row.h"
#include "page_compositor.h"

// The offer: 0 = none, else the buffer and page packed into one word.
#define OFFER(buf, page)   ((((uint32_t)(page) + 1) << 8) | ((uint32_t)(buf) + 1))
#define OFFER_BUF(o)       ((int)((o) & 0xFF) - 1)
#define OFFER_PAGE(o)      ((int)((o) >> 8) - 1)

// NextKey() results besides a record index
#define NEXT_DONE    (-1)
#define NEXT_ERASE   (-2)

static inline uint32_t page_bit(int page)
{
    return page >= 0 && page < COMP_MAX_PAGES ? 1u << page : 0;
}

static inline bool rect_eq(KeyPixelRect a, KeyPixelRect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// Record `r` shows `k` (same rect and image)
static inline bool rec_is(const CompRecord &r, const comp_key_t &k)
{
    return r.known && rect_eq(r.rect, k.rect) && r.img == k.img;
}

static inline bool rect_in_page(KeyPixelRect r)
{
    return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x + r.w <= SCREEN_PAGE_W && r.y + r.h <= SCREEN_PAGE_H;
}

void PageCompositor::Init(uint16_t *const buffers[COMP_BUFFERS], comp_get_page_t get_page,
                          comp_is_retired_t is_retired, comp_clock_t clock, void *ctx)
{
    m_is_retired = is_retired;
    m_clock      = clock;

    for (int b = 0; b < COMP_BUFFERS; b++) {
        fill_px(buffers[b], SCREEN_PAGE_W * SCREEN_PAGE_H, COMP_BACKGROUND);
        m_buf[b] = Buffer{};
        m_buf[b].px = buffers[b];
        m_px[b] = buffers[b];
    }

    m_get_page = get_page;
    m_ctx      = ctx;
    m_ready    = true;
}

// ===========================================================================
// Frame start (ISR)
// ===========================================================================

IRAM_ATTR int PageCompositor::Frame(int first, int count, int offset, int64_t now_us, comp_view_t out[2])
{
    if (!m_ready)
        return 0;

    Shown next[2];
    int   n = 0;

    m_lock.EnterISR();

    // 1. Take the offer if its page was asked for
    int offer_page = COMP_NO_PAGE, offer_buf = -1;
    uint32_t offer = port::AtomicLoad(&m_offer);
    if (offer) {
        int p = OFFER_PAGE(offer);
        if (p >= first && p < first + count && port::AtomicCompareExchange(&m_offer, offer, 0)) {
            offer_page = p;
            offer_buf  = OFFER_BUF(offer);
            m_st_swaps = m_st_swaps + 1;
        }
    }

    // 2. A buffer for each page asked for that has one
    for (int i = 0; i < count; i++) {
        int page = first + i, buf = -1;
        if (page == offer_page)
            buf = offer_buf;
        else
            for (int j = 0; j < m_frame.shown_count; j++)
                if (m_frame.shown[j].page == page)
                    buf = m_frame.shown[j].buf;
        if (buf >= 0)
            next[n++] = Shown{ (int16_t)page, (int8_t)buf };
    }
    if (n < count)
        m_st_holds = m_st_holds + 1;

    // 3. None: keep what was on screen last frame (after a reset, or boot).
    // 4. Otherwise the new set replaces the old one, which frees every buffer
    //    not in it: the bounce fill has just stopped reading them.
    if (n == 0) {
        n = m_frame.shown_count;
        for (int i = 0; i < n; i++)
            next[i] = m_frame.shown[i];
    } else {
        for (int i = 0; i < n; i++)
            m_frame.shown[i] = next[i];
        m_frame.shown_count = n;
    }

    m_frame.asked_first = first;
    m_frame.asked_count = count;
    m_frame.offset      = offset;
    if (m_frame.time_us)
        m_frame.period_us = (int32_t)(now_us - m_frame.time_us);
    m_frame.time_us = now_us;

    m_lock.ExitISR();

    for (int i = 0; i < n; i++) {
        out[i].page = next[i].page;
        out[i].px   = m_px[next[i].buf];
    }
    return n;
}

// ===========================================================================
// Client side
// ===========================================================================

void PageCompositor::PageChanged(int page, uint32_t flags)
{
    uint32_t bit = page_bit(page);
    if (!bit)
        return;
    if (flags & COMP_FLUSH)
        port::AtomicOr(&m_changed_flush, bit);
    port::AtomicOr(&m_changed, bit);
}

void PageCompositor::RequestReset()
{
    port::AtomicStore(&m_reset_request, 1);
}

uint32_t PageCompositor::RequestSweep()
{
    uint32_t v;
    do
        v = port::AtomicLoad(&m_sweep_request);
    while (!port::AtomicCompareExchange(&m_sweep_request, v, v + 1));
    return v + 1;
}

void PageCompositor::GetStats(comp_stats_t *st)
{
    st->keys       = m_st_keys;
    st->swaps      = m_st_swaps;
    st->holds      = m_st_holds;
    st->max_key_us = m_st_max_key / port::CyclesPerUs();
    st->busy_us    = m_st_busy / port::CyclesPerUs();
    st->erases         = m_st_erases;
    st->chunks         = m_st_chunks;
    st->gaps           = m_st_gaps;
    st->bytes          = m_st_bytes;
    st->max_chunk_us   = m_st_max_chunk_us;
    st->max_gap_end_us = m_st_max_gap_end_us;
    st->late_gaps      = m_st_late_gaps;
    m_st_keys = 0;
    m_st_swaps = 0;
    m_st_holds = 0;
    m_st_max_key = 0;
    m_st_busy = 0;
    m_st_erases = 0;
    m_st_chunks = 0;
    m_st_gaps = 0;
    m_st_bytes = 0;
    m_st_max_chunk_us = 0;
    m_st_max_gap_end_us = 0;
    m_st_late_gaps = 0;
}

// ===========================================================================
// Frame info and the offer (task)
// ===========================================================================

bool PageCompositor::IsShown(int b) const
{
    for (int i = 0; i < m_fi.shown_count; i++)
        if (m_fi.shown[i].buf == b)
            return true;
    return false;
}

int PageCompositor::FrontOf(int page) const
{
    if (page == COMP_NO_PAGE)
        return -1;
    for (int i = 0; i < m_fi.shown_count; i++)
        if (m_fi.shown[i].page == page)
            return m_fi.shown[i].buf;
    return -1;
}

bool PageCompositor::IsAsked(int page) const
{
    return page >= m_fi.asked_first && page < m_fi.asked_first + m_fi.asked_count;
}

// Copy the frame info. It can only be stale in the safe direction: a buffer
// leaves the screen at a frame start, and only comes back on through our
// offer, which Withdraw() and OfferTaken() check for.
void PageCompositor::RefreshFrame()
{
    FrameInfo old = m_fi;
    m_lock.Enter();
    m_fi = m_frame;
    m_lock.Exit();

    // Note buffers that just left the screen, for the back tie-break
    for (int i = 0; i < old.shown_count; i++)
        if (!IsShown(old.shown[i].buf))
            m_buf[old.shown[i].buf].freed = ++m_free_clock;
}

// Put up the back as the offer for `page`. The release store orders the
// key's pixel writes before the bounce fill can see it.
void PageCompositor::PutOffer(int b, int page)
{
    m_offer_mine = OFFER(b, page);
    port::AtomicStore(&m_offer, m_offer_mine);
    Preempt();
}

// Take our offer down. Returns false if the bounce fill took it first: that
// buffer is on screen now, and the frame info is refreshed to show it.
bool PageCompositor::Withdraw()
{
    if (!m_offer_mine)
        return true;
    Preempt();
    uint32_t v = port::AtomicExchange(&m_offer, 0);
    m_offer_mine = 0;
    if (v)
        return true;
    Preempt();
    RefreshFrame();     // the ISR updated m_frame in the same critical section
    return false;
}

// Was our offer taken at a frame start? Only the ISR clears it otherwise.
bool PageCompositor::OfferTaken()
{
    if (!m_offer_mine || port::AtomicLoad(&m_offer) == m_offer_mine)
        return false;
    m_offer_mine = 0;
    Preempt();
    RefreshFrame();
    return true;
}

static int64_t frame_period(int32_t measured_us)
{
    return measured_us > 1000 ? measured_us : COMP_FRAME_PERIOD_DEFAULT_US;
}

// With an offer up, would a key taking `needed_us` run into the next frame
// start (so the offer couldn't be put back up in time)?
bool PageCompositor::FrameStartNear(int64_t now_us, int64_t needed_us) const
{
    if (!m_fi.time_us)
        return false;
    int64_t period = frame_period(m_fi.period_us);
    int64_t since  = (now_us - m_fi.time_us) % period;
    return period - since < needed_us + COMP_SWAP_GUARD_US;
}

// ===========================================================================
// Records and layouts
// ===========================================================================
//
// A buffer's layout is its list of records (rects, no two overlapping); a
// page's is its list of keys. Records are matched to keys by rect.

int PageCompositor::FindRect(int b, KeyPixelRect r) const
{
    const Buffer &B = m_buf[b];
    for (int i = 0; i < B.count; i++)
        if (rect_eq(B.rec[i].rect, r))
            return i;
    return -1;
}

static bool page_has_rect(const comp_page_t &page, KeyPixelRect r)
{
    for (int i = 0; i < page.count; i++)
        if (rect_eq(page.keys[i].rect, r))
            return true;
    return false;
}

// Record `r` (of some buffer) shows the same as buffer `other`'s record at
// its rect, both known
bool PageCompositor::MatchesIn(const CompRecord &r, int other) const
{
    int j = FindRect(other, r.rect);
    return j >= 0 && r.known && m_buf[other].rec[j].known && r.img == m_buf[other].rec[j].img;
}

// The same rects in the same order
bool PageCompositor::SameLayout(int b, const comp_page_t &page) const
{
    const Buffer &B = m_buf[b];
    if (B.count != page.count)
        return false;
    for (int i = 0; i < B.count; i++)
        if (!rect_eq(B.rec[i].rect, page.keys[i].rect))
            return false;
    return true;
}

bool PageCompositor::SameLayoutBufs(int a, int b) const
{
    if (m_buf[a].count != m_buf[b].count)
        return false;
    for (int i = 0; i < m_buf[a].count; i++)
        if (!rect_eq(m_buf[a].rec[i].rect, m_buf[b].rec[i].rect))
            return false;
    return true;
}

// Units of work to make buffer `b` show `page`: keys not shown, rects to
// leave, areas to fill
int PageCompositor::DiffKeys(int b, const comp_page_t &page) const
{
    const Buffer &B = m_buf[b];
    int n = B.erase_count;
    for (int i = 0; i < page.count; i++) {
        int j = FindRect(b, page.keys[i].rect);
        n += j < 0 || !rec_is(B.rec[j], page.keys[i]);
    }
    for (int i = 0; i < B.count; i++)
        n += !page_has_rect(page, B.rec[i].rect);
    return n;
}

// The same for making `b` a copy of buffer `like`
int PageCompositor::DiffRecs(int b, int like) const
{
    const Buffer &B = m_buf[b], &L = m_buf[like];
    int n = B.erase_count + L.erase_count;
    for (int i = 0; i < L.count; i++)
        n += !MatchesIn(L.rec[i], b);
    for (int i = 0; i < B.count; i++)
        n += FindRect(like, B.rec[i].rect) < 0;
    return n;
}

// How many keys free buffer `b` shares with the closest buffer on screen
int PageCompositor::CopyOfShown(int b) const
{
    int most = 0;
    for (int i = 0; i < m_fi.shown_count; i++) {
        int s = m_fi.shown[i].buf, same = 0;
        for (int k = 0; k < m_buf[s].count; k++)
            same += MatchesIn(m_buf[s].rec[k], b);
        if (same > most)
            most = same;
    }
    return most;
}

// The free buffer that differs least from `page` (or, if NULL, from buffer
// `like`). On a tie:
//  - updating a page on screen (`front` >= 0): the one closest to the front,
//    so there are fewer catch-up keys before the first swap
//  - otherwise: the one least like a page on screen, keeping copies of those
//    (they make the next update cheap)
// then the one freed longest ago, so the page just left is kept longest.
// There's always one: at most two buffers are on screen.
int PageCompositor::ChooseFree(const comp_page_t *page, int like, int front) const
{
    int best = -1, best_diff = 0, best_copy = 0;
    for (int b = 0; b < COMP_BUFFERS; b++) {
        if (IsShown(b))
            continue;
        int diff = page ? DiffKeys(b, *page) : DiffRecs(b, like);
        int copy = front >= 0 ? DiffRecs(b, front) : CopyOfShown(b);
        if (best < 0 || diff < best_diff ||
            (diff == best_diff && (copy < best_copy ||
                                   (copy == best_copy && m_buf[b].freed < m_buf[best].freed)))) {
            best      = b;
            best_diff = diff;
            best_copy = copy;
        }
    }
    assert(best >= 0);
    return best;
}

// Area that may hold pixels outside the layout. If the list is full, it
// becomes one rect round everything.
void PageCompositor::AddErase(int b, KeyPixelRect r)
{
    Buffer &B = m_buf[b];
    if (B.erase_count < COMP_MAX_KEYS) {
        B.erase[B.erase_count++] = r;
        return;
    }
    int x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    for (int i = 0; i < B.erase_count; i++) {
        const KeyPixelRect &e = B.erase[i];
        if (e.x < x0) x0 = e.x;
        if (e.y < y0) y0 = e.y;
        if (e.x + e.w > x1) x1 = e.x + e.w;
        if (e.y + e.h > y1) y1 = e.y + e.h;
    }
    B.erase[0]    = KeyPixelRect{ (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
    B.erase_count = 1;
}

// Give buffer `b` `page`'s layout (rects, in its order). Records at rects
// both have keep what they show; new rects are unknown; rects it leaves are
// queued to be filled with background. No pixels change.
void PageCompositor::Relayout(int b, const comp_page_t &page)
{
    if (SameLayout(b, page))
        return;
    Buffer &B = m_buf[b];
    CompRecord rec[COMP_MAX_KEYS];
    bool       used[COMP_MAX_KEYS] = {};
    for (int i = 0; i < page.count; i++) {
        int j = FindRect(b, page.keys[i].rect);
        if (j >= 0) {
            rec[i]  = B.rec[j];
            used[j] = true;
        } else {
            rec[i] = CompRecord{ page.keys[i].rect, nullptr, false };
        }
    }
    for (int j = 0; j < B.count; j++)
        if (!used[j])
            AddErase(b, B.rec[j].rect);
    memcpy(B.rec, rec, page.count * sizeof(rec[0]));
    B.count = page.count;
}

// ===========================================================================
// Drawing
// ===========================================================================

// Roughly how long drawing a key at `r` takes from now
int64_t PageCompositor::KeyTimeUs(KeyPixelRect r) const
{
    int64_t gaps = (int64_t)r.w * r.h * (int64_t)sizeof(uint16_t) / COMP_GAP_BYTES_EST + 1;
    return gaps * (frame_period(m_fi.period_us) / SCREEN_CHUNKS);
}

// Start drawing `key` at record `i` of buffer `b` (which must not be on
// screen or offered). The record is unknown until the key is done.
// DrawChunk() draws it in fill gaps. When it's done, if `offer_page` is a
// page and the swap rule holds, the buffer is offered.
void PageCompositor::BeginKey(int b, int i, const comp_key_t &key, int offer_page)
{
    assert(rect_eq(m_buf[b].rec[i].rect, key.rect));
    m_drawing = b;
    m_buf[b].rec[i] = CompRecord{ key.rect, nullptr, false };

    m_unit.active     = true;
    m_unit.erase      = false;
    m_unit.buf        = b;
    m_unit.rec        = i;
    m_unit.rect       = key.rect;
    m_unit.img        = key.img;
    m_unit.drawable   = key.img && opaque_image_fits(key.img, key.rect.w, key.rect.h);
    m_unit.row        = 0;
    m_unit.offer_page = offer_page;
    m_unit.cycles     = 0;
    // The client gives images the size of their rect (an image that doesn't
    // fit is drawn as background, not past the rect)
    assert(!key.img || get_image_width(key.img) == (uint32_t)key.rect.w);
}

// Start filling buffer `b`'s first erase rect (minus its layout) with background
void PageCompositor::BeginErase(int b, int offer_page)
{
    m_drawing = b;
    m_unit.active     = true;
    m_unit.erase      = true;
    m_unit.buf        = b;
    m_unit.rec        = -1;
    m_unit.rect       = m_buf[b].erase[0];
    m_unit.img        = nullptr;
    m_unit.drawable   = false;
    m_unit.row        = 0;
    m_unit.offer_page = offer_page;
    m_unit.cycles     = 0;
}

// Rows [y, y + rows) of the erase rect into `dst` (its first row, at its
// left edge): background everywhere but the layout's rects
void PageCompositor::EraseRows(uint16_t *dst, int y, int rows) const
{
    const Buffer &B = m_buf[m_unit.buf];
    const int ex0 = m_unit.rect.x, ex1 = m_unit.rect.x + m_unit.rect.w;
    for (int r = 0; r < rows; r++, y++, dst += SCREEN_PAGE_W) {
        // The layout's spans on this row, in x order
        int16_t s0[COMP_MAX_KEYS], s1[COMP_MAX_KEYS];
        int n = 0;
        for (int i = 0; i < B.count; i++) {
            const KeyPixelRect &k = B.rec[i].rect;
            if (y < k.y || y >= k.y + k.h)
                continue;
            int a = k.x > ex0 ? k.x : ex0, e = k.x + k.w < ex1 ? k.x + k.w : ex1;
            if (a >= e)
                continue;
            int j = n++;
            for (; j > 0 && s0[j - 1] > a; j--) {
                s0[j] = s0[j - 1];
                s1[j] = s1[j - 1];
            }
            s0[j] = (int16_t)a;
            s1[j] = (int16_t)e;
        }
        int x = ex0;
        for (int i = 0; i < n; i++) {
            if (s0[i] > x)
                fill_px(dst + (x - ex0), s0[i] - x, COMP_BACKGROUND);
            if (s1[i] > x)
                x = s1[i];
        }
        if (ex1 > x)
            fill_px(dst + (x - ex0), ex1 - x, COMP_BACKGROUND);
    }
}

// The next ~COMP_CHUNK_BYTES of the unit (at least a row) into its page
// buffer, written back to PSRAM at once so its bus traffic stays in the gap.
void PageCompositor::DrawChunk()
{
    uint32_t t0 = port::Cycles();
    const KeyPixelRect r = m_unit.rect;
    int rows = COMP_CHUNK_BYTES / (r.w * (int)sizeof(uint16_t));
    if (rows < 1)
        rows = 1;
    if (rows > r.h - m_unit.row)
        rows = r.h - m_unit.row;

    uint16_t *dst = m_buf[m_unit.buf].px + (r.y + m_unit.row) * SCREEN_PAGE_W + r.x;
    if (m_unit.erase)
        EraseRows(dst, r.y + m_unit.row, rows);
    else if (m_unit.drawable)
        opaque_draw_rows(dst, SCREEN_PAGE_W, m_unit.img, m_unit.row, rows);
    else
        for (int y = 0; y < rows; y++)
            fill_px(dst + y * SCREEN_PAGE_W, r.w, COMP_BACKGROUND);
    Preempt();
    port::CacheWriteback(dst, ((rows - 1) * SCREEN_PAGE_W + r.w) * sizeof(uint16_t));

    m_unit.row += rows;
    m_total_chunks++;
    m_st_chunks = m_st_chunks + 1;
    m_st_bytes  = m_st_bytes + (uint32_t)(rows * r.w * sizeof(uint16_t));
    m_unit.cycles += port::Cycles() - t0;
    if (m_unit.row >= r.h)
        FinishUnit();
}

// The unit is in its page buffer: record it, and offer the buffer if asked
// to and the swap rule holds.
void PageCompositor::FinishUnit()
{
    int b = m_unit.buf;
    Buffer &B = m_buf[b];
    if (m_unit.erase) {
        for (int i = 0; i < B.erase_count; i++) {
            if (rect_eq(B.erase[i], m_unit.rect)) {
                B.erase[i] = B.erase[--B.erase_count];
                break;
            }
        }
        m_total_erases++;
        m_st_erases = m_st_erases + 1;
    } else {
        B.rec[m_unit.rec] = CompRecord{ m_unit.rect, m_unit.img, true };
        m_total_keys++;
        m_st_keys = m_st_keys + 1;
        if (m_unit.cycles > m_st_max_key)
            m_st_max_key = m_unit.cycles;
    }
    m_st_busy = m_st_busy + m_unit.cycles;
    m_unit.active = false;
    m_drawing     = -1;

    int page = m_unit.offer_page;
    if (page != COMP_NO_PAGE && m_job.page == page && m_job.back == b && !m_offer_mine) {
        int front = FrontOf(page);
        if (front >= 0 && SwapOk(b, front))
            PutOffer(b, page);
    }
}

// Stop drawing the unit. A key's record stays unknown; an erase stays queued
// (filling it again is harmless).
void PageCompositor::AbortUnit()
{
    m_unit.active = false;
    m_drawing     = -1;
}

// Draw chunks of the unit until it's done or the gap's deadline: a chunk is
// started only if its worst case ends before it.
bool PageCompositor::GapStep(const comp_gap_t *gap)
{
    if (!gap)
        return false;
    int64_t deadline = gap->fill_start_us + gap->period_us - COMP_GAP_MARGIN_US;
    bool drew = false;
    int64_t t = Clock();
    while (m_unit.active && t + COMP_CHUNK_WORST_US <= deadline) {
        DrawChunk();
        drew = true;
        int64_t end = Clock();
        if ((uint32_t)(end - t) > m_st_max_chunk_us)
            m_st_max_chunk_us = (uint32_t)(end - t);
        t = end;
    }
    if (drew) {
        // Stats: gaps used (a gap may take several Steps), and where the
        // gap's work ended against the deadline
        if (gap->fill_start_us != m_st_gap_start) {
            m_st_gap_start = gap->fill_start_us;
            m_st_gaps = m_st_gaps + 1;
        }
        uint32_t end_us = (uint32_t)(t - gap->fill_start_us);
        if (end_us > m_st_max_gap_end_us)
            m_st_max_gap_end_us = end_us;
        if (t > deadline)
            m_st_late_gaps = m_st_late_gaps + 1;
    }
    return drew;
}

// ===========================================================================
// Jobs
// ===========================================================================

void PageCompositor::StartJob(int page)
{
    uint32_t bit = page_bit(page);
    m_pending &= ~bit;

    // Flush flags set before the snapshot: their change is in it
    bool flush = ((m_pending_flush | port::AtomicAnd(&m_changed_flush, ~bit)) & bit) != 0;
    m_pending_flush &= ~bit;

    m_job.page = page;
    m_job.back = -1;
    Preempt();
    comp_page_t &S = m_job.snap;
    if (!m_get_page(page, &S, m_ctx))
        S.count = 0;
    Preempt();

    // Keys the compositor can't draw safely (the client checks them; this
    // only keeps a bad one from writing outside the buffer)
    if (S.count < 0)
        S.count = 0;
    if (S.count > COMP_MAX_KEYS)
        S.count = COMP_MAX_KEYS;
    int n = 0;
    for (int i = 0; i < S.count; i++) {
        assert(rect_in_page(S.keys[i].rect));
        if (rect_in_page(S.keys[i].rect))
            S.keys[n++] = S.keys[i];
    }
    S.count = n;

    // A flag set while the snapshot was taken may belong to a change that's
    // in it, so honour it too. It stays set, so the next job may flush as
    // well: flushing too often only delays a change, never splits one.
    if (port::AtomicLoad(&m_changed_flush) & bit)
        flush = true;
    m_job.flush = flush;
}

void PageCompositor::EndJob()
{
    Withdraw();
    m_job.page = COMP_NO_PAGE;
    m_job.back = -1;
}

// The page isn't asked for any more. Its records stay accurate, so nothing
// is lost; mark it changed again in case it still has a front somewhere.
void PageCompositor::DropJob()
{
    uint32_t bit = page_bit(m_job.page);
    m_pending |= bit;
    if (m_job.flush)
        m_pending_flush |= bit;
    EndJob();
}

// The next unit for back `b` (which has the snapshot's layout): areas to
// fill first, then catch-up keys (matching neither the front nor the
// snapshot), then progress keys. NEXT_DONE = matches the snapshot everywhere.
int PageCompositor::NextKey(int b, int front, bool flush) const
{
    const Buffer      &B = m_buf[b];
    const comp_page_t &S = m_job.snap;

    if (B.erase_count)
        return NEXT_ERASE;
    if (!flush)
        for (int k = 0; k < B.count; k++)
            if (!rec_is(B.rec[k], S.keys[k]) && !MatchesIn(B.rec[k], front))
                return k;
    for (int k = 0; k < B.count; k++)
        if (!rec_is(B.rec[k], S.keys[k]))
            return k;
    return NEXT_DONE;
}

// The swap rule: every key in the back matches the front or the snapshot,
// with nothing left to fill, and the back differs from the front somewhere.
bool PageCompositor::SwapOk(int b, int front) const
{
    const Buffer &B = m_buf[b];
    if (B.erase_count)
        return false;
    assert(SameLayout(b, m_job.snap));
    bool ahead = !SameLayoutBufs(b, front);
    for (int k = 0; k < B.count; k++) {
        bool same = MatchesIn(B.rec[k], front);
        if (!same && !rec_is(B.rec[k], m_job.snap.keys[k]))
            return false;
        ahead |= !same;
    }
    return ahead;
}

PageCompositor::StepResult PageCompositor::JobStep(int64_t now_us)
{
    int page = m_job.page;

    // Swiped away or reversed past: drop it at this key boundary
    if (!IsAsked(page)) {
        DropJob();
        return StepResult::Run;
    }

    // Offer taken at a frame start: the back is the front now
    if (OfferTaken())
        m_job.back = -1;

    int front = FrontOf(page);
    if (front >= 0 && DiffKeys(front, m_job.snap) == 0) {
        EndJob();       // the snapshot is on screen
        return StepResult::Run;
    }

    if (m_job.back < 0)
        m_job.back = ChooseFree(&m_job.snap, -1, front);
    int  b     = m_job.back;
    bool flush = m_job.flush || front < 0;      // not on screen: always flushed

    // An offered back already has the snapshot's layout, so this never
    // changes a buffer that may go on screen
    assert(!m_offer_mine || SameLayout(b, m_job.snap));
    Relayout(b, m_job.snap);

    int k = NextKey(b, front, flush);
    if (k == NEXT_DONE) {
        // Complete: offer it, and wait for the frame start to take it
        if (!m_offer_mine)
            PutOffer(b, page);
        return StepResult::Wait;
    }

    if (m_offer_mine) {
        // With an offer up, don't start a key that can't be finished before
        // the frame start: the offer would miss it
        KeyPixelRect r = k == NEXT_ERASE ? m_buf[b].erase[0] : m_buf[b].rec[k].rect;
        if (FrameStartNear(now_us, KeyTimeUs(r)))
            return StepResult::Wait;
        // Withdraw before drawing. If it's gone, the back is on screen.
        if (!Withdraw()) {
            m_job.back = -1;
            return StepResult::Run;
        }
    }

    // Incremental: offer the back after the key if the swap rule holds
    Preempt();
    int offer = flush ? COMP_NO_PAGE : page;
    if (k == NEXT_ERASE)
        BeginErase(b, offer);
    else
        BeginKey(b, k, m_job.snap.keys[k], offer);
    return StepResult::Run;
}

// Pick the next job. Returns false if there's nothing to do.
bool PageCompositor::PickJob()
{
    // 1. A page asked for but missing: the drag is waiting for it
    for (int i = 0; i < m_fi.asked_count; i++) {
        int page = m_fi.asked_first + i;
        if (FrontOf(page) < 0) {
            StartJob(page);
            return true;
        }
    }

    // 2. A page in view whose target changed, the one more on screen first
    int order[2] = { m_fi.asked_first, m_fi.asked_first + 1 };
    if (m_fi.asked_count == 2 && m_fi.offset > SCREEN_PAGE_W / 2) {
        order[0] = m_fi.asked_first + 1;
        order[1] = m_fi.asked_first;
    }
    for (int i = 0; i < m_fi.asked_count; i++) {
        int page = order[i];
        if ((m_pending & page_bit(page)) && FrontOf(page) >= 0) {
            StartJob(page);
            return true;
        }
    }
    return false;
}

// Idle work: with one page at rest, bring the free buffer closest to the
// front up to a copy of it, a unit at a time. Images in records stay valid
// (that's the client's contract), so following them is safe.
PageCompositor::StepResult PageCompositor::IdleStep()
{
    if (m_fi.asked_count != 1 || m_fi.shown_count != 1)
        return StepResult::Wait;
    int front = FrontOf(m_fi.asked_first);
    if (front < 0)
        return StepResult::Wait;

    int b = ChooseFree(nullptr, front, -1);
    const Buffer &F = m_buf[front];
    comp_page_t target;
    target.count = F.count;
    for (int k = 0; k < F.count; k++)
        target.keys[k] = comp_key_t{ F.rec[k].rect, F.rec[k].img };
    Relayout(b, target);

    if (m_buf[b].erase_count) {
        BeginErase(b, COMP_NO_PAGE);
        return StepResult::Run;
    }
    for (int k = 0; k < F.count; k++) {
        if (F.rec[k].known && !MatchesIn(F.rec[k], b)) {
            BeginKey(b, k, target.keys[k], COMP_NO_PAGE);
            return StepResult::Run;
        }
    }
    return StepResult::Wait;
}

// Sweep: forget retired images. Records that use one become unknown (an
// unknown key is redrawn before its buffer is offered again; the pixels on
// screen stay as they are). A job whose snapshot uses one is dropped, and a
// key being drawn from one is abandoned: the client no longer uses retired
// images, so the next snapshot won't either.
void PageCompositor::DoSweep()
{
    if (!m_is_retired)
        return;
    auto retired = [this](const void *img) { return img && m_is_retired(img, m_ctx); };

    for (int b = 0; b < COMP_BUFFERS; b++) {
        for (int k = 0; k < m_buf[b].count; k++) {
            CompRecord &r = m_buf[b].rec[k];
            if (!r.known || !retired(r.img))
                continue;
            r.known = false;
            r.img   = nullptr;
            // A page on screen with a key forgotten needs a look, even if
            // the change that retired the image was already taken
            for (int i = 0; i < m_fi.shown_count; i++)
                if (m_fi.shown[i].buf == b)
                    m_pending |= page_bit(m_fi.shown[i].page);
        }
    }

    if (m_job.page != COMP_NO_PAGE) {
        for (int k = 0; k < m_job.snap.count; k++) {
            if (retired(m_job.snap.keys[k].img)) {
                DropJob();
                m_job.snap.count = 0;
                break;
            }
        }
    }

    if (m_unit.active && !m_unit.erase && retired(m_unit.img))
        AbortUnit();
}

// Reload: forget every page and image. The buffers on screen stay on screen,
// belonging to no page, as the fallback until the new page is ready. Their
// layouts stay (they say where pixels may be), with nothing known.
void PageCompositor::DoReset()
{
    // Every reference goes, so any sweep asked for so far is done too
    uint32_t sweep = port::AtomicLoad(&m_sweep_request);

    AbortUnit();
    EndJob();
    m_job.snap.count = 0;
    for (int b = 0; b < COMP_BUFFERS; b++) {
        for (int k = 0; k < m_buf[b].count; k++) {
            m_buf[b].rec[k].known = false;
            m_buf[b].rec[k].img   = nullptr;
        }
    }

    m_lock.Enter();
    for (int i = 0; i < m_frame.shown_count; i++)
        m_frame.shown[i].page = COMP_NO_PAGE;
    m_lock.Exit();

    // Earlier changes don't matter: every record is unknown now
    port::AtomicStore(&m_changed, 0);
    port::AtomicStore(&m_changed_flush, 0);
    m_pending = m_pending_flush = 0;

    RefreshFrame();
    port::AtomicStore(&m_sweep_done, sweep);
    port::AtomicStore(&m_reset_request, 0);
}

PageCompositor::StepResult PageCompositor::Step(int64_t now_us, const comp_gap_t *gap)
{
    RefreshFrame();

    if (port::AtomicLoad(&m_reset_request)) {
        DoReset();
        return StepResult::ResetDone;
    }

    // A sweep: the request is loaded before the images are checked, so every
    // image flagged before it was made is seen
    uint32_t sweep = port::AtomicLoad(&m_sweep_request);
    if (sweep != port::AtomicLoad(&m_sweep_done)) {
        DoSweep();
        port::AtomicStore(&m_sweep_done, sweep);
        return StepResult::Run;
    }

    // A unit being drawn: only fill gaps move it on. Jobs wait for the unit
    // boundary.
    if (m_unit.active) {
        GapStep(gap);
        return m_unit.active ? StepResult::Wait : StepResult::Run;
    }

    // Changed bits: changed before flush, the reverse of the order
    // PageChanged() sets them
    m_pending       |= port::AtomicExchange(&m_changed, 0);
    m_pending_flush |= port::AtomicExchange(&m_changed_flush, 0);

    if (m_job.page != COMP_NO_PAGE)
        return JobStep(now_us);
    if (PickJob())
        return StepResult::Run;
    return IdleStep();
}
