#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness.h"

using namespace SimpleLib;

#define COPY_HEADER 16      // a copy's capacity, before it (keeps it 16-byte aligned)

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

BLOBPACK *LoadAsset(const char *name)
{
    // Run from firmware/test (mk run) or the repository root
    static const char *dirs[] = {
        "../assets/",
        "firmware/assets/",
        "../../assets/",
    };
    FILE *f = nullptr;
    char path[256];
    for (const char *d : dirs) {
        snprintf(path, sizeof(path), "%s%s", d, name);
        if ((f = fopen(path, "rb")) != nullptr)
            break;
    }
    if (!f) {
        fprintf(stderr, "Can't find firmware/assets/%s (run from firmware/test)\n", name);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    BLOBPACK *pack = (BLOBPACK *)malloc(size);
    if (fread(pack, 1, size, f) != (size_t)size)
        exit(2);
    fclose(f);
    return pack;
}

// ---------------------------------------------------------------------------
// World

World::World(int num_pages_, uint32_t seed) : rng(seed), num_pages(num_pages_)
{
    for (int b = 0; b < COMP_BUFFERS; b++)
        m_buffers[b] = (uint16_t *)malloc(SCREEN_PAGE_W * SCREEN_PAGE_H * sizeof(uint16_t));
    for (int p = 0; p < TEST_MAX_PAGES; p++)
        for (int k = 0; k < COMP_KEYS; k++)
            m_shown_version[p][k] = -1;

    m_cell = (uint16_t *)malloc(COMP_KEY_SIZE * COMP_KEY_SIZE * sizeof(uint16_t));

    comp.Init(m_buffers, m_cell, GetPage, IsRetired, this);
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
    free(m_cell);
}

bool World::GetPage(int page, comp_key_t keys[COMP_KEYS], void *ctx)
{
    World *w = (World *)ctx;
    if (page < 0 || page >= w->num_pages)
        return false;
    memcpy(keys, w->model[page], sizeof(w->model[page]));
    return true;
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
        for (int k = 0; k < COMP_KEYS; k++)
            if (model[p][k].bg == img || model[p][k].fg == img)
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
            for (int k = 0; k < COMP_KEYS; k++) {
                const CompRecord &r = comp.GetRecord(b, k);
                Check(!r.known || (r.bg != img && r.fg != img));
            }
        if (comp.JobPage() != COMP_NO_PAGE)
            for (int k = 0; k < COMP_KEYS; k++)
                Check(comp.JobSnapshot()[k].bg != img && comp.JobSnapshot()[k].fg != img);
        if (const comp_key_t *key = comp.DrawingKey())
            Check(key->bg != img && key->fg != img);
        Check(!InModel(img));
        m_retired.RemoveAt(i);
        Release(img);
    }
}

void World::SetKey(int page, int k, const void *bg, const void *fg, uint32_t flags)
{
    model[page][k] = comp_key_t{ bg, fg, flags };
    m_history[page][k].Add(Version{ bg, fg });
}

void World::NewModel()
{
    TestImages &ti = TestImages::Get();
    for (int p = 0; p < num_pages; p++)
        for (int k = 0; k < COMP_KEYS; k++)
            SetKey(p, k, Copy(ti.Face(p % TEST_KEYS_FACE_PRESSED)), Copy(ti.Icon((p * 5 + k) % ti.IconCount())));
}

void World::ChangeKeys(int page, uint32_t mask, uint32_t flags)
{
    TestImages &ti = TestImages::Get();
    Batch batch = {};
    batch.mask  = mask;
    batch.flush = (flags & COMP_FLUSH) != 0;

    for (int k = 0; k < COMP_KEYS; k++) {
        if (!(mask & (1u << k)))
            continue;
        // Keep the face sometimes, or swap it (normal/pressed, transparent
        // padding or opaque); the icon is new or none. At least one pointer
        // is always new, so versions differ.
        const void *bg  = model[page][k].bg;
        uint32_t    kfl = model[page][k].flags;
        bool new_bg = rng.Chance(50);
        if (new_bg) {
            bool opaque = rng.Chance(50);
            int  face   = rng.Chance(30) ? TEST_KEYS_FACE_PRESSED : page % TEST_KEYS_FACE_PRESSED;
            bg  = Copy(ti.Face(face, opaque));
            kfl = opaque ? COMP_KEY_BG_OPAQUE : 0;
        }
        const void *fg = (new_bg && rng.Chance(15)) ? nullptr : Copy(ti.Icon(rng.Range(0, ti.IconCount() - 1)));
        SetKey(page, k, bg, fg, kfl);
        batch.version[k] = m_history[page][k].GetCount() - 1;
    }
    m_batches[page].Add(batch);

    // "Under the client lock": straight after the change
    comp.PageChanged(page, flags);
}

void World::Reload()
{
    // Done at the next key boundary: a key being copied needs its gaps first
    comp.RequestReset();
    bool done = false;
    for (int i = 0; i < 100 && !done; i++)
        done = comp.Step(frame_start, true) == PageCompositor::StepResult::ResetDone;
    Check(done);

    // No client image may be referenced now
    for (int b = 0; b < COMP_BUFFERS; b++)
        for (int k = 0; k < COMP_KEYS; k++)
            Check(!comp.GetRecord(b, k).known);
    Check(comp.JobPage() == COMP_NO_PAGE);

    FreeImages();
    for (int p = 0; p < TEST_MAX_PAGES; p++) {
        m_batches[p].Clear();
        for (int k = 0; k < COMP_KEYS; k++) {
            m_history[p][k].Clear();
            m_shown_version[p][k] = -1;
        }
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

    int64_t now = frame_start;
    for (int c = 0; c < SCREEN_CHUNKS; c++) {
        // The fill for chunk c has just finished. The gap is only usable if
        // the compositor gets to it promptly (it may still be rendering).
        int64_t fill_end = frame_start + (int64_t)c * CHUNK_US;
        if (now < fill_end)
            now = fill_end;
        PageCompositor::StepResult r;
        now = DoStep(now, now - fill_end < COMP_GAP_FRESH_US, &r);

        for (int i = 0; i < 100 && r != PageCompositor::StepResult::Wait && now < fill_end + CHUNK_US; i++)
            now = DoStep(now, false, &r);
    }
    CollectSwept();
}

int64_t World::DoStep(int64_t now, bool gap, PageCompositor::StepResult *result)
{
    const size_t bytes = SCREEN_PAGE_W * SCREEN_PAGE_H * sizeof(uint16_t);
    int drawing  = comp.DrawingBuffer();
    int max_rows = view_count == 2 ? COMP_SLICE_ROWS_TWO_PAGES : COMP_SLICE_ROWS_ONE_PAGE;
    if (check_gap_writes) {
        for (int b = 0; b < COMP_BUFFERS; b++) {
            if (!m_shadow[b])
                m_shadow[b] = (uint16_t *)malloc(bytes);
            memcpy(m_shadow[b], m_buffers[b], bytes);
        }
    }

    uint32_t renders = comp.TotalRenders(), slices = comp.TotalSlices();
    *result = comp.Step(now, gap);
    now += 20;
    if (comp.TotalRenders() > renders)
        now += key_cost_us;
    if (comp.TotalSlices() > slices)
        now += copy_cost_us;

    if (check_gap_writes) {
        // Page buffers change only in a gap, in the buffer being drawn, by
        // at most one slice of key rows
        for (int b = 0; b < COMP_BUFFERS; b++) {
            if (memcmp(m_buffers[b], m_shadow[b], bytes) == 0)
                continue;
            int changed = 0;
            for (size_t i = 0; i < bytes / sizeof(uint16_t); i++)
                changed += m_buffers[b][i] != m_shadow[b][i];
            {
                Check(gap);
                Check(b == drawing);
                Check(changed <= max_rows * COMP_KEY_SIZE);
            }
        }
    }
    return now;
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
        uint32_t mask = w->rng.Next() & 0x7FFF;
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

bool World::ShowsModel(int page) const
{
    int b = ShownBuffer(page);
    if (b < 0)
        return false;
    for (int k = 0; k < COMP_KEYS; k++) {
        const CompRecord &r = comp.GetRecord(b, k);
        if (!r.known || r.bg != model[page][k].bg || r.fg != model[page][k].fg)
            return false;
    }
    return true;
}

bool World::AtRestOn(int page) const
{
    int b = ShownBuffer(page);
    return b >= 0 && view.px[0] == comp.GetBuffer(b) && view.offset == 0 && view.px[1] == nullptr;
}

int World::FindVersion(int page, int k, const CompRecord &r) const
{
    const List<Version> &h = m_history[page][k];
    for (int v = h.GetCount() - 1; v >= 0; v--)
        if (h[v].bg == r.bg && h[v].fg == r.fg)
            return v;
    return -1;
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
        int page = views[i].page;
        if (page == COMP_NO_PAGE)
            continue;           // last frame's buffer after a reload

        // Every key shows a version the client set, never older than one
        // shown before. A sweep may have made a record unknown (its images
        // were retired); its pixels are still what was drawn, but which
        // version they are can't be told.
        int  now_version[COMP_KEYS];
        bool known[COMP_KEYS];
        for (int k = 0; k < COMP_KEYS; k++) {
            const CompRecord &r = comp.GetRecord(shown[i], k);
            known[k] = r.known;
            if (!r.known) {
                Check(m_sweep_requested);       // a frame start can land mid-sweep
                continue;
            }
            int v = FindVersion(page, k, r);
            Check(v >= 0);
            Check(v >= m_shown_version[page][k]);
            m_shown_version[page][k] = v;
            now_version[k] = v;
        }

        // A flushed change appears all at once
        List<Batch> &batches = m_batches[page];
        for (int j = batches.GetCount() - 1; j >= 0; j--) {
            const Batch &bt = batches[j];
            int total = 0, shown_new = 0;
            for (int k = 0; k < COMP_KEYS; k++) {
                if (!(bt.mask & (1u << k)) || !known[k])
                    continue;
                total++;
                shown_new += now_version[k] >= bt.version[k];
            }
            if (shown_new > 0 && shown_new < total) {
                Check(!bt.flush);
                partial_frames++;
            }
            if (total > 0 && shown_new == total)
                batches.RemoveAt(j);
        }

        // A buffer coming on screen has exactly the pixels its records say
        if (shown[i] != m_prev_shown[0] && shown[i] != m_prev_shown[1])
            CheckPixels(shown[i]);
    }

    m_prev_shown[0] = shown[0];
    m_prev_shown[1] = shown[1];
}

// Each key cell matches a reference render of its record: background, then
// bg, then fg.
void World::CheckPixels(int b)
{
    static uint16_t ref[COMP_KEY_SIZE * COMP_KEY_SIZE];
    const uint16_t *px = comp.GetBuffer(b);

    for (int k = 0; k < COMP_KEYS; k++) {
        const CompRecord &r = comp.GetRecord(b, k);
        if (!r.known)
            continue;
        for (int i = 0; i < COMP_KEY_SIZE * COMP_KEY_SIZE; i++)
            ref[i] = COMP_BACKGROUND;
        IMAGE_BUFFER rb;
        init_image_buffer(&rb, COMP_KEY_SIZE, COMP_KEY_SIZE, ref, COMP_KEY_SIZE);
        if (r.bg)
            draw_image(&rb, r.bg, 0, 0, 0, 0, COMP_KEY_SIZE, COMP_KEY_SIZE, nullptr, 255, nullptr);
        if (r.fg)
            draw_image(&rb, r.fg, 0, 0, 0, 0, COMP_KEY_SIZE, COMP_KEY_SIZE, nullptr, 255, nullptr);

        const uint16_t *cell = px + COMP_KEY_Y(k) * SCREEN_PAGE_W + COMP_KEY_X(k);
        for (int y = 0; y < COMP_KEY_SIZE; y++)
            Check(memcmp(cell + y * SCREEN_PAGE_W, ref + y * COMP_KEY_SIZE, COMP_KEY_SIZE * sizeof(uint16_t)) == 0);
    }
}

void World::CheckSettledOn(int page)
{
    Check(AtRestOn(page));
    Check(ShowsModel(page));
    CheckPixels(ShownBuffer(page));

    // The gaps between cells are never drawn
    static bool in_cell[SCREEN_PAGE_H][SCREEN_PAGE_W];
    memset(in_cell, 0, sizeof(in_cell));
    for (int k = 0; k < COMP_KEYS; k++)
        for (int y = 0; y < COMP_KEY_SIZE; y++)
            for (int x = 0; x < COMP_KEY_SIZE; x++)
                in_cell[COMP_KEY_Y(k) + y][COMP_KEY_X(k) + x] = true;

    for (int b = 0; b < COMP_BUFFERS; b++) {
        const uint16_t *px = comp.GetBuffer(b);
        for (int y = 0; y < SCREEN_PAGE_H; y++)
            for (int x = 0; x < SCREEN_PAGE_W; x++)
                if (!in_cell[y][x])
                    Check(px[y * SCREEN_PAGE_W + x] == COMP_BACKGROUND);
    }
}
