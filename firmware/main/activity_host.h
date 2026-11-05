#pragma once

// The page swap a switchActivity does (Standalone's ActivityHost): the layout
// (ConfigApplyActivity under the model lock), then the screen's page count, a
// glide to page 1 and the status bar's left text. Platform independent and
// tested with the real store and layout and a recording screen; the device
// layer (standalone_task.cpp) supplies the lock and the screen calls.
//
// Remembers which activity's pages the layout holds (`from` for the next swap).
// Reset() when the store was reset (hosted takeover / release): the layout then
// holds no activity.
//
// Not thread safe: called under the Standalone owner's lock, so the lock order
// is Standalone's mutex, then the model lock (taken here, around the layout
// swap only; the screen calls are made outside it).

#include "config_apply.h"
#include "standalone.h"

struct ActivityScreen
{
    void *ctx;
    void (*lock)(void *ctx);                            // the model lock
    void (*unlock)(void *ctx);
    void (*set_page_count)(int count, void *ctx);       // the swipe task's page count (pager pills)
    void (*go_to_page)(int page, bool animate, void *ctx);
    void (*set_status_left)(const char *text, void *ctx);   // the status bar cuts it to its width
};

class ActivityApplier
{
public:
    // `blob` is the relocated blob `root` was taken from; `store`/`layout` the model's
    void Init(const cfgdata::configRoot *root, const uint8_t *blob, ImageStore *store, KeyLayout *layout,
              const ActivityScreen &screen);

    // The store was reset: the layout holds no activity's pages
    void Reset() { m_from = -1; }

    // Hand to StandaloneHost::activity
    const ActivityHost *Host() const { return &m_host; }

    // Swaps the layout refused (an image the store would not take): the screen was left alone
    uint32_t Failures() const { return m_failures; }

private:
    static void Apply(int index, void *ctx);
    static void Glide(void *ctx);

    const cfgdata::configRoot *m_root = nullptr;
    const uint8_t  *m_blob = nullptr;
    ImageStore     *m_store = nullptr;
    KeyLayout      *m_layout = nullptr;
    ActivityScreen  m_screen = {};
    ActivityHost    m_host = {};
    int             m_from = -1;
    uint32_t        m_failures = 0;
};
