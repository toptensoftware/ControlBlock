#include "activity_host.h"

void ActivityApplier::Init(const cfgdata::configRoot *root, const uint8_t *blob, ImageStore *store, KeyLayout *layout,
                           const ActivityScreen &screen)
{
    m_root     = root;
    m_blob     = blob;
    m_store    = store;
    m_layout   = layout;
    m_screen   = screen;
    m_from     = -1;
    m_failures = 0;
    m_host.ctx   = this;
    m_host.apply = Apply;
    m_host.glide = Glide;
}

void ActivityApplier::Apply(int index, void *ctx)
{
    ActivityApplier *a = (ActivityApplier *)ctx;
    if (!a->m_root)
        return;

    a->m_screen.lock(a->m_screen.ctx);
    bool ok = ConfigApplyActivity(a->m_root, a->m_blob, a->m_from, index, a->m_store, a->m_layout);
    int pages = a->m_layout->PageCount();
    a->m_screen.unlock(a->m_screen.ctx);
    if (!ok) {
        a->m_failures++;            // layout, store and page count are as they were
        return;
    }
    a->m_from = index;

    // The count before the glide: both reach the swipe task in order
    a->m_screen.set_page_count(pages, a->m_screen.ctx);
    if (index >= 0)
        a->m_screen.go_to_page(1, true, a->m_screen.ctx);
    const cfgdata::activity *act = index >= 0 ? &a->m_root->activities[index] : nullptr;
    const char *text = "";
    if (act)
        text = (act->displayName && act->displayName[0]) ? act->displayName : act->name;
    a->m_screen.set_status_left(text ? text : "", a->m_screen.ctx);
}

void ActivityApplier::Glide(void *ctx)
{
    ActivityApplier *a = (ActivityApplier *)ctx;
    a->m_screen.go_to_page(1, true, a->m_screen.ctx);
}
