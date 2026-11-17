#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "opaque_row.h"

using namespace SimpleLib;

#define COPY_HEADER 16      // a copy's capacity, before it (keeps it 16-byte aligned)

static_assert(COMP_BACKGROUND == 0, "CheckPixels clears its reference with memset");

static inline bool RectEq(KeyPixelRect a, KeyPixelRect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// ---------------------------------------------------------------------------
// Test images

TestImages &TestImages::Get()
{
    static TestImages s_images;
    return s_images;
}

TestImages::TestImages() : m_pack(LoadAsset("test_keys.bin"))
{
}

// Read the first of dirs[i] + name that exists (`where` names it in the error); exits if none
static void *LoadFirst(const char *const *dirs, int ndirs, const char *where, const char *name, size_t *size_out)
{
    FILE *f = nullptr;
    char path[256];
    for (int i = 0; i < ndirs && !f; i++) {
        snprintf(path, sizeof(path), "%s%s", dirs[i], name);
        f = fopen(path, "rb");
    }
    if (!f) {
        fprintf(stderr, "Can't find %s%s (run from firmware/test)\n", where, name);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc(size ? size : 1);
    if (fread(data, 1, size, f) != (size_t)size)
        exit(2);
    fclose(f);
    if (size_out)
        *size_out = (size_t)size;
    return data;
}

BLOBPACK *LoadAsset(const char *name)
{
    // Run from firmware/test (mk run) or the repository root
    static const char *dirs[] = { "../assets/", "firmware/assets/", "../../assets/" };
    return (BLOBPACK *)LoadFirst(dirs, 3, "firmware/assets/", name, nullptr);
}

void *LoadTestData(const char *name, size_t *size)
{
    static const char *dirs[] = { "data/", "firmware/test/data/", "../test/data/" };
    return LoadFirst(dirs, 3, "firmware/test/data/", name, size);
}

// ---------------------------------------------------------------------------
// Layouts

void MakeLayout(TestLayout layout, comp_page_t *page)
{
    static const KeyRect sparse[] = {
        { 0, 0, 4, 4 },         // 1U
        { 8, 0, 8, 4 },         // 2U
        { 4, 4, 2, 4 },         // 0.5U x 1U: no image fits
        { 12, 4, 4, 4 },        // 1U
        { 0, 8, 8, 4 },         // 2U
        { 9, 9, 3, 2 },         // 0.75U x 0.5U
        { 19, 11, 1, 1 },       // 0.25U
    };
    static const int rows_2u[3][4] = { { 2, 1, 1, 1 }, { 1, 2, 2, 0 }, { 2, 2, 1, 0 } };

    KeyRect rects[COMP_MAX_KEYS];
    int n = 0;
    switch (layout) {
    case LAYOUT_1U:
        for (int r = 0; r < KEY_GRID_ROWS; r++)
            for (int c = 0; c < KEY_GRID_COLS; c++)
                rects[n++] = KeyRect{ (uint8_t)(c * 4), (uint8_t)(r * 4), 4, 4 };
        break;
    case LAYOUT_2U:
        for (int r = 0; r < 3; r++) {
            int x = 0;
            for (int i = 0; i < 4 && rows_2u[r][i]; i++) {
                rects[n++] = KeyRect{ (uint8_t)x, (uint8_t)(r * 4), (uint8_t)(rows_2u[r][i] * 4), 4 };
                x += rows_2u[r][i] * 4;
            }
        }
        break;
    default:
        for (const KeyRect &r : sparse)
            rects[n++] = r;
        break;
    }
    Check(KeyRectsCheck(rects, n) == KEY_GEOM_OK);
    page->count = n;
    for (int i = 0; i < n; i++)
        page->keys[i] = comp_key_t{ KeyRectPixels(rects[i]), nullptr };
}

// ---------------------------------------------------------------------------
// World

World::World(int num_pages_, uint32_t seed, bool mixed_) : rng(seed), num_pages(num_pages_), mixed(mixed_)
{
    for (int b = 0; b < COMP_BUFFERS; b++)
        m_buffers[b] = (uint16_t *)malloc(SCREEN_PAGE_W * SCREEN_PAGE_H * sizeof(uint16_t));

    comp.Init(m_buffers, GetPage, IsRetired, Clock, this);
    comp.SetPreemptHook(PreemptHook, this);
    NewModel();
}

World::~World()
{
    FreeImages();
    for (int i = 0; i < m_reuse.GetCount(); i++)
        free((uint8_t *)m_reuse[i] - COPY_HEADER);
    for (int b = 0; b < COMP_BUFFERS; b++) {
        free(m_buffers[b]);
        free(m_shadow[b]);
    }
}

bool World::GetPage(int page, comp_page_t *out, void *ctx)
{
    World *w = (World *)ctx;
    if (page < 0 || page >= w->num_pages)
        return false;
    *out = w->model[page];
    return true;
}

// The step's start, plus chunk_cost_us for every chunk drawn since
int64_t World::Clock(void *ctx)
{
    World *w = (World *)ctx;
    return w->m_clock_base + (int64_t)(w->comp.TotalChunks() - w->m_clock_chunks) * w->chunk_cost_us;
}

// A copy's capacity is kept just before it, so released copies can be reused
static uint32_t &CopyCapacity(void *img) { return *(uint32_t *)((uint8_t *)img - COPY_HEADER); }

// A private copy, so every key version has its own pointer. Released copies
// are reused first (newest first), as a real allocator might.
const void *World::Copy(const void *img)
{
    uint32_t size = get_image_packed_size(img);
    void *p = nullptr;
    for (int i = m_reuse.GetCount() - 1; i >= 0 && !p; i--) {
        if (CopyCapacity(m_reuse[i]) >= size) {
            p = m_reuse[i];
            m_reuse.RemoveAt(i);
        }
    }
    if (!p) {
        uint8_t *block = (uint8_t *)malloc(size + COPY_HEADER);
        p = block + COPY_HEADER;
        CopyCapacity(p) = size;
    }
    memcpy(p, img, size);
    m_images.Add(p);
    return p;
}

const void *World::NewImage(KeyPixelRect r)
{
    TestImages &ti = TestImages::Get();
    if (r.w == KEY_CELL && r.h == KEY_CELL)
        return Copy(ti.Key1U(rng.Range(0, TEST_KEYS_KEYS_1U - 1)));
    if (r.w == 2 * KEY_CELL && r.h == KEY_CELL)
        return Copy(ti.Key2U(rng.Range(0, TEST_KEYS_KEYS_2U - 1)));
    return nullptr;
}

// Poison then free: a compositor that still drew from these would show it
void World::FreeImages()
{
    for (int i = 0; i < m_images.GetCount(); i++) {
        memset(m_images[i], 0xDD, CopyCapacity(m_images[i]));
        free((uint8_t *)m_images[i] - COPY_HEADER);
    }
    m_images.Clear();
    m_retired.Clear();
}

void World::Release(void *img)
{
    memset(img, 0xDD, CopyCapacity(img));
    m_images.Remove(img);
    m_reuse.Add(img);
    images_freed++;
}

bool World::InModel(const void *img) const
{
    for (int p = 0; p < num_pages; p++)
        for (int k = 0; k < model[p].count; k++)
            if (model[p].keys[k].img == img)
                return true;
    return false;
}

bool World::IsRetired(const void *img, void *ctx)
{
    World *w = (World *)ctx;
    for (int i = 0; i < w->m_retired.GetCount(); i++)
        if (w->m_retired[i].img == img)
            return true;
    return false;
}

void World::RetireUnused()
{
    int first = m_retired.GetCount();
    for (int i = 0; i < m_images.GetCount(); i++)
        if (!InModel(m_images[i]) && !IsRetired(m_images[i], this))
            m_retired.Add(Retired{ m_images[i], 0 });
    if (m_retired.GetCount() == first)
        return;

    // Flag first, then request: the sweep must see the flags
    m_sweep_requested = true;
    uint32_t gen = comp.RequestSweep();
    for (int i = first; i < m_retired.GetCount(); i++)
        m_retired.GetBuffer()[i].sweep = gen;      // operator[] returns a copy
}

// Free retired images whose sweep is done, checking nothing references them
void World::CollectSwept()
{
    uint32_t done = comp.Swept();
    for (int i = m_retired.GetCount() - 1; i >= 0; i--) {
        if ((int32_t)(done - m_retired[i].sweep) < 0)
            continue;
        void *img = m_retired[i].img;
        for (int b = 0; b < COMP_BUFFERS; b++)
            for (int k = 0; k < comp.RecordCount(b); k++) {
                const CompRecord &r = comp.GetRecord(b, k);
                Check(!r.known || r.img != img);
            }
        if (comp.JobPage() != COMP_NO_PAGE)
            for (int k = 0; k < comp.JobSnapshot()->count; k++)
                Check(comp.JobSnapshot()->keys[k].img != img);
        Check(comp.DrawingImage() != img);
        Check(!InModel(img));
        m_retired.RemoveAt(i);
        Release(img);
    }
}

void World::SetKey(int page, int k, const void *img)
{
    model[page].keys[k].img = img;
    m_history[page].Add(Version{ model[page].keys[k].rect, img });
}

void World::SetLayout(int page, TestLayout layout)
{
    MakeLayout(layout, &model[page]);
    Layout l = {};
    l.count = model[page].count;
    for (int k = 0; k < l.count; k++) {
        l.rect[k] = model[page].keys[k].rect;
        SetKey(page, k, NewImage(l.rect[k]));
    }
    m_layouts[page].Add(l);
}

void World::NewModel()
{
    for (int p = 0; p < num_pages; p++)
        SetLayout(p, mixed ? (TestLayout)(p % LAYOUT_COUNT) : LAYOUT_1U);
}

void World::ChangeKeys(int page, uint32_t mask, uint32_t flags)
{
    Batch batch = {};
    batch.flush = (flags & COMP_FLUSH) != 0;

    for (int k = 0; k < model[page].count; k++) {
        if (!(mask & (1u << k)))
            continue;
        const void *img = NewImage(model[page].keys[k].rect);
        if (!img)
            continue;       // no image fits: it stays as it is
        SetKey(page, k, img);
        batch.rect[batch.count]      = model[page].keys[k].rect;
        batch.version[batch.count++] = m_history[page].GetCount() - 1;
    }
    if (batch.count)
        m_batches[page].Add(batch);

    // "Under the client lock": straight after the change
    comp.PageChanged(page, flags);
}

void World::ChangeLayout(int page, TestLayout layout, uint32_t flags)
{
    SetLayout(page, layout);
    Batch batch = {};
    batch.flush = (flags & COMP_FLUSH) != 0;
    int first = m_history[page].GetCount() - model[page].count;
    for (int k = 0; k < model[page].count; k++) {
        batch.rect[batch.count]      = model[page].keys[k].rect;
        batch.version[batch.count++] = first + k;
    }
    m_batches[page].Add(batch);
    comp.PageChanged(page, flags);
}

void World::Reload()
{
    // Done straight away: a key being drawn is abandoned
    comp.RequestReset();
    bool done = false;
    for (int i = 0; i < 10 && !done; i++)
        done = comp.Step(frame_start, nullptr) == PageCompositor::StepResult::ResetDone;
    Check(done);

    // No client image may be referenced now
    for (int b = 0; b < COMP_BUFFERS; b++)
        for (int k = 0; k < comp.RecordCount(b); k++)
            Check(!comp.GetRecord(b, k).known);
    Check(comp.JobPage() == COMP_NO_PAGE);
    Check(comp.DrawingBuffer() < 0);

    FreeImages();
    for (int p = 0; p < TEST_MAX_PAGES; p++) {
        m_batches[p].Clear();
        m_history[p].Clear();
        m_seen[p].Clear();
        m_layouts[p].Clear();
    }
    NewModel();
    for (int p = 0; p < num_pages; p++)
        comp.PageChanged(p, 0);
}

// ---------------------------------------------------------------------------
// Frames

void World::FrameStart()
{
    pages      = screen_pages_in_view(scroll, num_pages);
    view_count = comp.Frame(pages.first, pages.count, pages.count == 2 ? pages.offset : 0, frame_start, views);
    view       = screen_resolve_view(pages, views, view_count);
    CheckFrame();
}

void World::RunFrame()
{
    frame_start += FRAME_US;
    FrameStart();

    // Gaps follow this frame's fills (a frame start landing mid-step moves
    // frame_start, but not the fills already under way)
    const int64_t fs0 = frame_start;
    int64_t now = fs0;
    for (int c = 0; c < SCREEN_CHUNKS; c++) {
        // The fill for chunk c has just finished; the task gets there when
        // it's done with what it was doing
        int64_t fill_start = fs0 + (int64_t)c * CHUNK_US;
        int64_t arrive = fill_start + (view_count == 2 ? fill_two_us : fill_one_us) + gap_delay_us;
        if (now < arrive)
            now = arrive;
        comp_gap_t gap = { fill_start, CHUNK_US };
        uint32_t chunks = comp.TotalChunks();

        PageCompositor::StepResult r;
        for (int i = 0; i < 200 && now < fill_start + CHUNK_US; i++) {
            now = DoStep(now, &gap, &r);
            if (r == PageCompositor::StepResult::Wait)
                break;
        }
        if (comp.TotalChunks() - chunks > max_chunks_per_gap)
            max_chunks_per_gap = comp.TotalChunks() - chunks;
    }
    CollectSwept();
}

int64_t World::DoStep(int64_t now, const comp_gap_t *gap, PageCompositor::StepResult *result)
{
    const size_t bytes = SCREEN_PAGE_W * SCREEN_PAGE_H * sizeof(uint16_t);
    int drawing = comp.DrawingBuffer();
    if (check_gap_writes) {
        for (int b = 0; b < COMP_BUFFERS; b++) {
            if (!m_shadow[b])
                m_shadow[b] = (uint16_t *)malloc(bytes);
            memcpy(m_shadow[b], m_buffers[b], bytes);
        }
    }

    m_clock_base   = now;
    m_clock_chunks = comp.TotalChunks();
    *result = comp.Step(now, gap);
    uint32_t chunks = comp.TotalChunks() - m_clock_chunks;
    int64_t after = Clock(this) + 20;

    // Chunks only in a gap, the last started while its worst case still
    // fitted before the deadline (the chunks run back to back from `now`)
    if (chunks) {
        Check(gap != nullptr);
        int64_t deadline = gap->fill_start_us + gap->period_us - COMP_GAP_MARGIN_US;
        Check(now + (int64_t)(chunks - 1) * chunk_cost_us + COMP_CHUNK_WORST_US <= deadline);
    }

    if (check_gap_writes) {
        // Page buffers change only by the chunks drawn, in the buffer being
        // drawn (a chunk is at most a row, or COMP_CHUNK_BYTES)
        for (int b = 0; b < COMP_BUFFERS; b++) {
            if (memcmp(m_buffers[b], m_shadow[b], bytes) == 0)
                continue;
            uint32_t changed = 0;
            for (size_t i = 0; i < bytes / sizeof(uint16_t); i++)
                changed += m_buffers[b][i] != m_shadow[b][i];
            Check(chunks > 0);
            Check(b == drawing);
            Check(changed <= chunks * SCREEN_PAGE_W);
        }
    }
    return after;
}

void World::PreemptHook(void *ctx)
{
    World *w = (World *)ctx;
    if (w->m_in_hook)
        return;
    w->m_in_hook++;

    // Whatever is being drawn into must not be on screen
    int drawing = w->comp.DrawingBuffer();
    if (drawing >= 0)
        for (int i = 0; i < w->view_count; i++)
            Check(w->BufferIndex(w->views[i].px) != drawing);

    if (w->preempt_client_percent && w->rng.Chance(w->preempt_client_percent)) {
        int page = w->rng.Range(0, w->num_pages - 1);
        uint32_t mask = w->rng.Next();
        w->ChangeKeys(page, mask ? mask : 1, w->rng.Chance(50) ? COMP_FLUSH : 0);
    }
    if (w->preempt_percent && w->rng.Chance(w->preempt_percent)) {
        w->frame_start += FRAME_US;
        w->FrameStart();
    }
    w->m_in_hook--;
}

// ---------------------------------------------------------------------------
// Checks

int World::BufferIndex(const uint16_t *px) const
{
    for (int b = 0; b < COMP_BUFFERS; b++)
        if (comp.GetBuffer(b) == px)
            return b;
    return -1;
}

int World::ShownBuffer(int page) const
{
    for (int i = 0; i < view_count; i++)
        if (views[i].page == page)
            return BufferIndex(views[i].px);
    return -1;
}

bool World::Matches(int b, int page) const
{
    const comp_page_t &m = model[page];
    if (comp.RecordCount(b) != m.count || comp.EraseCount(b))
        return false;
    for (int k = 0; k < m.count; k++) {
        const CompRecord &r = comp.GetRecord(b, k);
        if (!r.known || !RectEq(r.rect, m.keys[k].rect) || r.img != m.keys[k].img)
            return false;
    }
    return true;
}

bool World::ShowsModel(int page) const
{
    int b = ShownBuffer(page);
    return b >= 0 && Matches(b, page);
}

bool World::AtRestOn(int page) const
{
    int b = ShownBuffer(page);
    return b >= 0 && view.px[0] == comp.GetBuffer(b) && view.offset == 0 && view.px[1] == nullptr;
}

int World::FindVersion(int page, KeyPixelRect r, const void *img) const
{
    const List<Version> &h = m_history[page];
    for (int v = h.GetCount() - 1; v >= 0; v--)
        if (RectEq(h[v].rect, r) && h[v].img == img)
            return v;
    return -1;
}

int World::LastSeen(int page, KeyPixelRect r) const
{
    for (int i = 0; i < m_seen[page].GetCount(); i++)
        if (RectEq(m_seen[page][i].rect, r))
            return m_seen[page][i].version;
    return -1;
}

void World::SetSeen(int page, KeyPixelRect r, int version)
{
    for (int i = 0; i < m_seen[page].GetCount(); i++) {
        if (RectEq(m_seen[page][i].rect, r)) {
            m_seen[page].GetBuffer()[i].version = version;
            return;
        }
    }
    m_seen[page].Add(Seen{ r, version });
}

// Buffer `b`'s rects are a layout `page` has had (never a mix of two)
bool World::KnownLayout(int page, int b) const
{
    for (int i = 0; i < m_layouts[page].GetCount(); i++) {
        const Layout &l = m_layouts[page][i];
        bool same = l.count == comp.RecordCount(b);
        for (int k = 0; same && k < l.count; k++)
            same = RectEq(l.rect[k], comp.GetRecord(b, k).rect);
        if (same)
            return true;
    }
    return false;
}

// Every frame start
void World::CheckFrame()
{
    Check(view_count >= 0 && view_count <= 2);

    int shown[2] = { -1, -1 };
    for (int i = 0; i < view_count; i++) {
        shown[i] = BufferIndex(views[i].px);
        Check(shown[i] >= 0);
        // Never show a buffer that's being drawn into
        Check(shown[i] != comp.DrawingBuffer());
    }
    if (view_count == 2) {
        Check(shown[0] != shown[1]);
        Check(views[0].page < views[1].page || views[0].page == COMP_NO_PAGE || views[1].page == COMP_NO_PAGE);
    }
    if (view_count < pages.count)
        held_frames++;

    // Pages not asked for come back only when none of the asked ones is
    // ready: last frame's buffers, held at rest
    int asked = 0;
    for (int i = 0; i < view_count; i++)
        asked += views[i].page >= pages.first && views[i].page < pages.first + pages.count;
    Check(asked == 0 || asked == view_count);

    for (int i = 0; i < view_count; i++) {
        int page = views[i].page, b = shown[i];
        if (page == COMP_NO_PAGE)
            continue;           // last frame's buffer after a reload

        // A layout the page has had, with nothing left to fill
        Check(KnownLayout(page, b));
        Check(comp.EraseCount(b) == 0);

        // Every key shows a version the client set, never older than one
        // shown before at its rect. A sweep may have made a record unknown
        // (its images were retired); its pixels are still what was drawn,
        // but which version they are can't be told. Nor can it for a key with
        // no image (the layouts' odd rects get one each time a layout is set):
        // its pixels are checked, its version isn't.
        int  now_version[COMP_MAX_KEYS];
        bool known[COMP_MAX_KEYS];
        for (int k = 0; k < comp.RecordCount(b); k++) {
            const CompRecord &r = comp.GetRecord(b, k);
            known[k] = r.known && r.img;
            if (!r.known) {
                Check(m_sweep_requested);       // a frame start can land mid-sweep
                continue;
            }
            if (!r.img)
                continue;
            int v = FindVersion(page, r.rect, r.img);
            Check(v >= 0);
            Check(v >= LastSeen(page, r.rect));
            SetSeen(page, r.rect, v);
            now_version[k] = v;
        }

        // A flushed change appears all at once
        List<Batch> &batches = m_batches[page];
        for (int j = batches.GetCount() - 1; j >= 0; j--) {
            const Batch &bt = batches.GetBuffer()[j];
            int total = 0, shown_new = 0;
            for (int e = 0; e < bt.count; e++) {
                int k = 0;
                while (k < comp.RecordCount(b) && !RectEq(comp.GetRecord(b, k).rect, bt.rect[e]))
                    k++;
                if (k == comp.RecordCount(b) || !known[k])
                    continue;
                total++;
                shown_new += now_version[k] >= bt.version[e];
            }
            if (shown_new > 0 && shown_new < total) {
                Check(!bt.flush);
                partial_frames++;
            }
            if (total > 0 && shown_new == total)
                batches.RemoveAt(j);
        }

        // A buffer coming on screen has exactly the pixels its records say
        if (b != m_prev_shown[0] && b != m_prev_shown[1])
            CheckPixels(b, true);
    }

    m_prev_shown[0] = shown[0];
    m_prev_shown[1] = shown[1];
}

// The oracle: opaque images blitted at their rects over the background
void World::CheckPixels(int b, bool strict)
{
    static uint16_t ref[SCREEN_PAGE_W * SCREEN_PAGE_H];
    const uint16_t *px = comp.GetBuffer(b);
    IMAGE_BUFFER rb;
    init_image_buffer(&rb, SCREEN_PAGE_W, SCREEN_PAGE_H, ref, SCREEN_PAGE_W);

    auto copy_actual = [&](KeyPixelRect r) {
        for (int y = r.y; y < r.y + r.h; y++)
            memcpy(ref + y * SCREEN_PAGE_W + r.x, px + y * SCREEN_PAGE_W + r.x, r.w * sizeof(uint16_t));
    };

    memset(ref, 0, sizeof(ref));
    // Don't care: areas still to fill, and records not known (a key being
    // drawn, or forgotten by a sweep or reset)
    if (strict)
        Check(comp.EraseCount(b) == 0);
    for (int i = 0; i < comp.EraseCount(b); i++)
        copy_actual(comp.GetErase(b, i));
    for (int k = 0; k < comp.RecordCount(b); k++)
        if (!comp.GetRecord(b, k).known)
            copy_actual(comp.GetRecord(b, k).rect);
    // Known records (they may lie under an erase rect)
    for (int k = 0; k < comp.RecordCount(b); k++) {
        const CompRecord &r = comp.GetRecord(b, k);
        if (!r.known)
            continue;
        fill_rect(&rb, r.rect.x, r.rect.y, r.rect.w, r.rect.h, COMP_BACKGROUND);
        if (r.img && opaque_image_fits(r.img, r.rect.w, r.rect.h))     // else background
            draw_image(&rb, r.img, r.rect.x, r.rect.y, 0, 0, r.rect.w, r.rect.h, nullptr, 255, nullptr);
    }
    Check(memcmp(px, ref, sizeof(ref)) == 0);
}

void World::CheckSettledOn(int page)
{
    Check(AtRestOn(page));
    Check(ShowsModel(page));
    CheckPixels(ShownBuffer(page), true);

    // Every buffer: what its records say, background outside them
    for (int b = 0; b < COMP_BUFFERS; b++)
        CheckPixels(b, false);
}
