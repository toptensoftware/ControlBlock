// Page compositor core. Spec: docs/page-compositor.md.
//
// The task side (Step) owns the buffers' pixels and records, the snapshot
// and the job. The ISR side (Frame) only touches the offer (one atomic word)
// and the frame info (under m_lock).

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "rle565.h"
#include "page_compositor.h"

// The offer: 0 = none, else the buffer and page packed into one word.
#define OFFER(buf, page)   ((((uint32_t)(page) + 1) << 8) | ((uint32_t)(buf) + 1))
#define OFFER_BUF(o)       ((int)((o) & 0xFF) - 1)
#define OFFER_PAGE(o)      ((int)((o) >> 8) - 1)

// One cell row of COMP_BACKGROUND in internal RAM, memcpy'd to clear
struct BackgroundRow
{
    uint16_t px[COMP_KEY_SIZE];
    BackgroundRow() { for (int i = 0; i < COMP_KEY_SIZE; i++) px[i] = COMP_BACKGROUND; }
};
alignas(4) static const BackgroundRow s_bg_row;

static inline uint32_t page_bit(int page)
{
    return page >= 0 && page < COMP_MAX_PAGES ? 1u << page : 0;
}

static inline bool rec_is(const CompRecord &r, const comp_key_t &k)
{
    return r.known && r.bg == k.bg && r.fg == k.fg;
}

static inline bool rec_eq(const CompRecord &a, const CompRecord &b)
{
    return a.known && b.known && a.bg == b.bg && a.fg == b.fg;
}

void PageCompositor::Init(uint16_t *const buffers[COMP_BUFFERS], uint16_t *cell, comp_get_page_t get_page, void *ctx)
{
    m_cell = cell;

    const size_t pixels = SCREEN_PAGE_W * SCREEN_PAGE_H;
    const uint32_t c2 = COMP_BACKGROUND | ((uint32_t)COMP_BACKGROUND << 16);

    for (int b = 0; b < COMP_BUFFERS; b++) {
        uint32_t *p = (uint32_t *)buffers[b];
        for (size_t i = 0; i < pixels / 2; i++)
            p[i] = c2;
        m_buf[b].px = buffers[b];
        memset(m_buf[b].rec, 0, sizeof(m_buf[b].rec));
        m_buf[b].freed = 0;
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

void PageCompositor::GetStats(comp_stats_t *st)
{
    st->keys       = m_st_keys;
    st->swaps      = m_st_swaps;
    st->holds      = m_st_holds;
    st->max_key_us = m_st_max_key / port::CyclesPerUs();
    st->busy_us    = m_st_busy / port::CyclesPerUs();
    m_st_keys = 0;
    m_st_swaps = 0;
    m_st_holds = 0;
    m_st_max_key = 0;
    m_st_busy = 0;
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
// Records and drawing
// ===========================================================================

int PageCompositor::DiffKeys(int b, const comp_key_t *keys) const
{
    int n = 0;
    for (int k = 0; k < COMP_KEYS; k++)
        n += !rec_is(m_buf[b].rec[k], keys[k]);
    return n;
}

int PageCompositor::DiffRecs(int b, int like) const
{
    int n = 0;
    for (int k = 0; k < COMP_KEYS; k++)
        n += !rec_eq(m_buf[b].rec[k], m_buf[like].rec[k]);
    return n;
}

// How many keys free buffer `b` shares with the closest buffer on screen
int PageCompositor::CopyOfShown(int b) const
{
    int most = 0;
    for (int i = 0; i < m_fi.shown_count; i++) {
        int same = COMP_KEYS - DiffRecs(b, m_fi.shown[i].buf);
        if (same > most)
            most = same;
    }
    return most;
}

// The free buffer that differs least from `keys` (or, if NULL, from buffer
// `like`). On a tie:
//  - updating a page on screen (`front` >= 0): the one closest to the front,
//    so there are fewer catch-up keys before the first swap
//  - otherwise: the one least like a page on screen, keeping copies of those
//    (they make the next update cheap)
// then the one freed longest ago, so the page just left is kept longest.
// There's always one: at most two buffers are on screen.
int PageCompositor::ChooseFree(const comp_key_t *keys, int like, int front) const
{
    int best = -1, best_diff = 0, best_copy = 0;
    for (int b = 0; b < COMP_BUFFERS; b++) {
        if (IsShown(b))
            continue;
        int diff = keys ? DiffKeys(b, keys) : DiffRecs(b, like);
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

void comp_add_image_rows(const void *img, int *y0, int *y1)
{
    if (!img)
        return;
    const IMAGE_RLE *h = (const IMAGE_RLE *)img;
    assert(h->base.signature == IMAGE_SIGNATURE && h->base.format == IMAGE_FORMAT_RLE);
    assert(h->base.width == COMP_KEY_SIZE && h->base.height == COMP_KEY_SIZE);

    int a = (int)h->firstRow, b = (int)(h->firstRow + h->rowCount);
    if (b > COMP_KEY_SIZE)
        b = COMP_KEY_SIZE;
    if (a >= b)
        return;
    if (*y0 >= *y1) {
        *y0 = a;
        *y1 = b;
    } else {
        if (a < *y0) *y0 = a;
        if (b > *y1) *y1 = b;
    }
}

void comp_clear_rows(uint16_t *cell, int stride, int y0, int y1)
{
    for (int y = y0; y < y1; y++)
        memcpy(cell + y * stride, s_bg_row.px, sizeof(s_bg_row.px));
}

bool comp_image_is_opaque(const void *img, size_t size)
{
    if (!check_image_header(img, size) ||
        get_image_width(img) != COMP_KEY_SIZE || get_image_height(img) != COMP_KEY_SIZE)
        return false;

    uint32_t *px = (uint32_t *)malloc(COMP_KEY_SIZE * COMP_KEY_SIZE * sizeof(uint32_t));
    if (!px)
        return false;
    bool opaque = decode_image_argb8888(img, size, NULL, px, 0, false);
    for (int i = 0; opaque && i < COMP_KEY_SIZE * COMP_KEY_SIZE; i++)
        opaque = (px[i] >> 24) == 0xFF;
    free(px);
    return opaque;
}

// Rows of a key slice copied per fill gap: smaller with two pages in view,
// when the fill reads one of them cold and has less time to spare
int PageCompositor::SliceRows() const
{
    return m_fi.shown_count >= 2 ? COMP_SLICE_ROWS_TWO_PAGES : COMP_SLICE_ROWS_ONE_PAGE;
}

// Rows for drawing `key` into cell `k` of buffer `b`:
//  - [new0, new1): the rows the new images draw (the record's new span)
//  - [copy0, copy1): the rows to copy to the page buffer: the old span (to
//    erase it) and the new one, or the whole cell for an opaque bg
void PageCompositor::KeyRows(int b, int k, const comp_key_t &key, int *new0, int *new1, int *copy0, int *copy1) const
{
    *new0 = *new1 = 0;
    comp_add_image_rows(key.bg, new0, new1);
    comp_add_image_rows(key.fg, new0, new1);

    bool opaque = key.bg && (key.flags & COMP_KEY_BG_OPAQUE);
    assert(!opaque || (*new0 == 0 && *new1 == COMP_KEY_SIZE));

    const CompRecord &r = m_buf[b].rec[k];
    *copy0 = r.known ? r.y0 : 0;
    *copy1 = r.known ? r.y1 : COMP_KEY_SIZE;
    if (opaque) {
        *copy0 = 0;
        *copy1 = COMP_KEY_SIZE;
    } else if (*copy0 >= *copy1) {
        *copy0 = *new0;
        *copy1 = *new1;
    } else if (*new0 < *new1) {
        if (*new0 < *copy0) *copy0 = *new0;
        if (*new1 > *copy1) *copy1 = *new1;
    }
}

// Roughly how long drawing `key` takes from now: the render, then a gap for
// each slice (the render usually costs one gap too)
int64_t PageCompositor::KeyTimeUs(int b, int k, const comp_key_t &key) const
{
    int n0, n1, c0, c1;
    KeyRows(b, k, key, &n0, &n1, &c0, &c1);
    int rows   = c1 > c0 ? c1 - c0 : 0;
    int slices = (rows + SliceRows() - 1) / SliceRows();
    int64_t chunk = frame_period(m_fi.period_us) / SCREEN_CHUNKS;
    return COMP_RENDER_EST_US + (slices + 1) * chunk;
}

// Start drawing `key` into cell `k` of buffer `b` (which must not be on
// screen or offered): render it into the cell in internal RAM. CopySlice()
// then copies it to the page buffer in fill gaps. When it's done, if
// `offer_page` is a page and the swap rule holds, the buffer is offered.
void PageCompositor::BeginKey(int b, int k, const comp_key_t &key, int offer_page)
{
    uint32_t t0 = port::Cycles();
    m_drawing = b;

    m_key.active     = true;
    m_key.buf        = b;
    m_key.key        = k;
    m_key.content    = key;
    m_key.offer_page = offer_page;
    KeyRows(b, k, key, &m_key.y0, &m_key.y1, &m_key.next, &m_key.end);

    // Background under the rows to copy (an opaque bg covers them), then
    // bg and fg with alpha
    if (!(key.bg && (key.flags & COMP_KEY_BG_OPAQUE)))
        comp_clear_rows(m_cell, COMP_KEY_SIZE, m_key.next, m_key.end);
    IMAGE_BUFFER view;
    init_image_buffer(&view, COMP_KEY_SIZE, COMP_KEY_SIZE, m_cell, COMP_KEY_SIZE);
    if (key.bg)
        draw_image(&view, key.bg, 0, 0, 0, 0, COMP_KEY_SIZE, COMP_KEY_SIZE, NULL, 255, NULL);
    Preempt();
    if (key.fg)
        draw_image(&view, key.fg, 0, 0, 0, 0, COMP_KEY_SIZE, COMP_KEY_SIZE, NULL, 255, NULL);

    m_total_renders++;
    m_key.cycles = port::Cycles() - t0;
    if (m_key.next >= m_key.end)
        FinishKey();            // nothing to copy
}

// Copy the next slice of the key from the cell to its page buffer, and
// write it back to PSRAM, so all its bus traffic is in this fill gap.
void PageCompositor::CopySlice()
{
    uint32_t t0 = port::Cycles();
    int rows = m_key.end - m_key.next;
    if (rows > SliceRows())
        rows = SliceRows();

    uint16_t *dst = m_buf[m_key.buf].px + (COMP_KEY_Y(m_key.key) + m_key.next) * SCREEN_PAGE_W + COMP_KEY_X(m_key.key);
    const uint16_t *src = m_cell + m_key.next * COMP_KEY_SIZE;
    for (int y = 0; y < rows; y++)
        memcpy(dst + y * SCREEN_PAGE_W, src + y * COMP_KEY_SIZE, COMP_KEY_SIZE * sizeof(uint16_t));
    Preempt();
    port::CacheWriteback(dst, ((rows - 1) * SCREEN_PAGE_W + COMP_KEY_SIZE) * sizeof(uint16_t));

    m_key.next += rows;
    m_total_slices++;
    m_key.cycles += port::Cycles() - t0;
    if (m_key.next >= m_key.end)
        FinishKey();
}

// The key is in its page buffer: record it, and offer the buffer if asked
// to and the swap rule holds.
void PageCompositor::FinishKey()
{
    int b = m_key.buf;
    m_buf[b].rec[m_key.key] = CompRecord{ m_key.content.bg, m_key.content.fg, (uint8_t)m_key.content.flags,
                                          (uint8_t)m_key.y0, (uint8_t)m_key.y1, true };
    m_key.active = false;
    m_drawing    = -1;
    m_total_keys++;

    m_st_keys = m_st_keys + 1;
    m_st_busy = m_st_busy + m_key.cycles;
    if (m_key.cycles > m_st_max_key)
        m_st_max_key = m_key.cycles;

    int page = m_key.offer_page;
    if (page != COMP_NO_PAGE && m_job.page == page && m_job.back == b && !m_offer_mine) {
        int front = FrontOf(page);
        if (front >= 0 && SwapOk(b, front))
            PutOffer(b, page);
    }
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
    if (!m_get_page(page, m_job.snap, m_ctx))
        memset(m_job.snap, 0, sizeof(m_job.snap));
    Preempt();

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

// The next key to draw into back `b`: catch-up keys (matching neither the
// front nor the snapshot) first, then progress keys. -1 = matches the
// snapshot everywhere.
int PageCompositor::NextKey(int b, int front, bool flush) const
{
    const CompRecord *rb = m_buf[b].rec;
    const comp_key_t *S  = m_job.snap;

    if (!flush)
        for (int k = 0; k < COMP_KEYS; k++)
            if (!rec_is(rb[k], S[k]) && !rec_eq(rb[k], m_buf[front].rec[k]))
                return k;
    for (int k = 0; k < COMP_KEYS; k++)
        if (!rec_is(rb[k], S[k]))
            return k;
    return -1;
}

// The swap rule: every key in the back matches the front or the snapshot,
// and at least one differs from the front.
bool PageCompositor::SwapOk(int b, int front) const
{
    bool ahead = false;
    for (int k = 0; k < COMP_KEYS; k++) {
        const CompRecord &rb = m_buf[b].rec[k];
        bool same = rec_eq(rb, m_buf[front].rec[k]);
        if (!same && !rec_is(rb, m_job.snap[k]))
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
        m_job.back = ChooseFree(m_job.snap, -1, front);
    int  b     = m_job.back;
    bool flush = m_job.flush || front < 0;      // not on screen: always flushed

    int k = NextKey(b, front, flush);
    if (k < 0) {
        // Complete: offer it, and wait for the frame start to take it
        if (!m_offer_mine)
            PutOffer(b, page);
        return StepResult::Wait;
    }

    if (m_offer_mine) {
        // With an offer up, don't start a key that can't be finished before
        // the frame start: the offer would miss it
        if (FrameStartNear(now_us, KeyTimeUs(b, k, m_job.snap[k])))
            return StepResult::Wait;
        // Withdraw before drawing. If it's gone, the back is on screen.
        if (!Withdraw()) {
            m_job.back = -1;
            return StepResult::Run;
        }
    }

    // Incremental: offer the back after the key if the swap rule holds
    Preempt();
    BeginKey(b, k, m_job.snap[k], flush ? COMP_NO_PAGE : page);
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
// front up to a copy of it, a key at a time. Images in records stay valid
// (that's the client's contract), so following them is safe.
PageCompositor::StepResult PageCompositor::IdleStep()
{
    if (m_fi.asked_count != 1 || m_fi.shown_count != 1)
        return StepResult::Wait;
    int front = FrontOf(m_fi.asked_first);
    if (front < 0)
        return StepResult::Wait;

    int b = ChooseFree(nullptr, front, -1);
    const CompRecord *rf = m_buf[front].rec;
    for (int k = 0; k < COMP_KEYS; k++) {
        if (rf[k].known && !rec_eq(m_buf[b].rec[k], rf[k])) {
            comp_key_t key = { rf[k].bg, rf[k].fg, rf[k].flags };
            BeginKey(b, k, key, COMP_NO_PAGE);
            return StepResult::Run;
        }
    }
    return StepResult::Wait;
}

// Reload: forget every page and image. The buffers on screen stay on screen,
// belonging to no page, as the fallback until the new page is ready.
void PageCompositor::DoReset()
{
    EndJob();
    memset(m_job.snap, 0, sizeof(m_job.snap));
    for (int b = 0; b < COMP_BUFFERS; b++)
        memset(m_buf[b].rec, 0, sizeof(m_buf[b].rec));     // known = false

    m_lock.Enter();
    for (int i = 0; i < m_frame.shown_count; i++)
        m_frame.shown[i].page = COMP_NO_PAGE;
    m_lock.Exit();

    // Earlier changes don't matter: every record is unknown now
    port::AtomicStore(&m_changed, 0);
    port::AtomicStore(&m_changed_flush, 0);
    m_pending = m_pending_flush = 0;

    RefreshFrame();
    port::AtomicStore(&m_reset_request, 0);
}

PageCompositor::StepResult PageCompositor::Step(int64_t now_us, bool gap)
{
    RefreshFrame();

    // A key being copied: only fill gaps move it on. Everything else
    // (jobs, resets) waits for the key boundary.
    if (m_key.active) {
        if (!gap)
            return StepResult::Wait;
        CopySlice();
        return StepResult::Run;
    }

    if (port::AtomicLoad(&m_reset_request)) {
        DoReset();
        return StepResult::ResetDone;
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
