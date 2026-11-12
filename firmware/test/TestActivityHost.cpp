// The page swap behind switchActivity (activity_host): a real Standalone, store and
// key layout over the sample layout (Watch: displayName "Watch TV", 2 pages; Listen: no
// displayName, 1 page), with a recording screen. The device layer's lock and screen
// calls are not tested here (no hardware): see wip/wip-activity-device-management.md.

#include "UnitTesting.h"
#include "Core.h"
#include "activity_host.h"
#include "layout_env.h"
#include "store_env.h"
#include "touch_router.h"

using namespace SimpleLib;
using namespace layout_env;

namespace
{
    // The sample's Home keys (by id: the control "touch[id]") that switch activity
    const uint16_t WATCH = 3, NONE = 10;

    struct Rig
    {
        StoreEnv      env;
        ImageStore    store;
        KeyLayout     layout;
        List<uint8_t> bytes = LoadSample();
        const layoutdata::layoutRoot *root = nullptr;
        LayoutSummary summary = {};
        ActivityApplier applier;
        Standalone    sa;
        char          calls[16][48];            // "lock", "count 3", "page 1", "status X", "unlock"
        int           n = 0;

        static void Log(void *c, const char *s, int v = -1)
        {
            Rig *r = (Rig *)c;
            if (r->n < 16)
                v < 0 ? snprintf(r->calls[r->n], 48, "%s", s) : snprintf(r->calls[r->n], 48, "%s %d", s, v);
            r->n++;
        }

        Rig()
        {
            store.Init(env.Host(), 1u << 20, KEY_GRID_W, KEY_GRID_H, true);     // as the device: opaque only
            store.SetListener(&layout);
            layout.Init(&store, [](int, uint32_t, void *) {}, nullptr);
            Assert(LayoutValidate(bytes.GetBuffer(), (uint32_t)bytes.GetCount(), &summary) == LayoutError::Ok);
            root = Relocate(bytes);
            Assert(LayoutApplyHome(summary, bytes.GetBuffer(), &store, &layout));

            ActivityScreen s = {};
            s.ctx = this;
            s.lock   = [](void *c) { Log(c, "lock"); };
            s.unlock = [](void *c) { Log(c, "unlock"); };
            s.set_page_count = [](int count, void *c) { Log(c, "count", count); };
            s.go_to_page = [](int page, bool animate, void *c) { Assert(animate); Log(c, "page", page); };
            s.set_status_left = [](const char *t, void *c)
            {
                Rig *r = (Rig *)c;
                if (r->n < 16)
                    snprintf(r->calls[r->n], 48, "status %s", t);
                r->n++;
            };
            applier.Init(root, bytes.GetBuffer(), &store, &layout, s);

            StandaloneHost h = {};
            h.ctx    = this;
            h.now_ms = [](void *) { return (uint32_t)1000; };
            h.activity = applier.Host();
            sa.Init(h, root);
        }

        void Tap(uint16_t id)
        {
            sa.Key(id, TOUCH_KEY_PRESS, 1000);
            sa.Key(id, TOUCH_KEY_RELEASE, 1000);
            for (int i = 0; i < 1000 && sa.Poll(); i++) {}
        }

        bool Is(int i, const char *s) const { return i < n && i < 16 && strcmp(calls[i], s) == 0; }
    };
}

Fact("ActivityHost Select Swaps The Layout Then Sets Count, Glides And Names It")
{
    Rig r;
    Assert(r.layout.PageCount() == 1);
    r.Tap(WATCH);                                       // Home's Watch key
    Assert(r.n == 5);
    Assert(r.Is(0, "lock") && r.Is(1, "unlock"));       // the layout swap is the only thing under the model lock
    Assert(r.Is(2, "count 3") && r.Is(3, "page 1"));    // the count before the glide: Home + 2 pages
    Assert(r.Is(4, "status Watch TV"));                 // displayName
    Assert(r.layout.PageCount() == 3);
    // Watch's keys, with the blob's ids, on pages 1 and 2
    int page = -1;
    Assert(r.layout.Find(16, &page) && page == 1);
    Assert(r.layout.Find(31, &page) && page == 2);
}

Fact("ActivityHost Status Text Is The Name When There Is No Display Name")
{
    Rig r;
    layoutdata::switchActivityOp *op = nullptr;
    for (uint32_t i = 0; i < r.root->homePage.bindings_count; i++) {
        layoutdata::binding &b = r.root->homePage.bindings[i];
        if (!op && b.ops_count && b.ops[0]->op == OP_SWITCH_ACTIVITY)
            op = (layoutdata::switchActivityOp *)b.ops[0];      // the first: the Watch key
    }
    Assert(op);
    op->index = 1;                                      // the Watch key now selects Listen
    r.Tap(WATCH);
    Assert(r.n == 5 && r.Is(2, "count 2") && r.Is(4, "status Listen"));
}

Fact("ActivityHost None Collapses To Home With Empty Text And No Glide")
{
    Rig r;
    r.Tap(WATCH);
    r.n = 0;
    r.Tap(NONE);                                        // Home's none key
    Assert(r.n == 4);
    Assert(r.Is(0, "lock") && r.Is(1, "unlock") && r.Is(2, "count 1") && r.Is(3, "status "));
    Assert(r.layout.PageCount() == 1);
}

Fact("ActivityHost Same Activity Only Glides")
{
    Rig r;
    r.Tap(WATCH);
    r.n = 0;
    r.Tap(WATCH);
    Assert(r.n == 1 && r.Is(0, "page 1"));
    Assert(r.layout.PageCount() == 3);
}

Fact("ActivityHost Frees The Old Activity's Images On A Swap")
{
    Rig r;
    ImageStoreStats home;
    r.store.GetStats(&home);
    r.Tap(WATCH);
    r.Tap(NONE);
    ImageStoreStats after;
    r.store.GetStats(&after);
    Assert(after.count == home.count);                  // back to Home's images only
}

Fact("ActivityHost Reset After The Store Was Reset Frees Nothing Twice")
{
    Rig r;
    r.Tap(WATCH);
    // A hosted session reset the model; release rebuilt Home
    r.layout.Clear();
    r.store.Reset();
    Assert(LayoutApplyHome(r.summary, r.bytes.GetBuffer(), &r.store, &r.layout));
    r.sa.SetHosted(true);
    r.sa.SetHosted(false);
    r.applier.Reset();
    Assert(r.sa.Activity() == -1);
    r.n = 0;
    r.Tap(WATCH);
    Assert(r.n == 5 && r.Is(4, "status Watch TV") && r.layout.PageCount() == 3);
    Assert(r.applier.Failures() == 0);
}

Fact("ActivityHost Nothing Is Called While Hosted")
{
    Rig r;
    r.sa.SetHosted(true);
    r.Tap(WATCH);
    Assert(r.n == 0 && r.sa.Activity() == -1);
    r.sa.SetHosted(false);
    r.Tap(WATCH);
    Assert(r.n == 5);
}
