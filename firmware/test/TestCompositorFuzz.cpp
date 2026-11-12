// Randomised compositor runs: swipes, reversals, key changes (incremental
// and flushed), reloads and frame starts landing mid-step, with World
// checking the invariants on every frame start. Each run ends by settling on
// a page and checking it shows the model.

#include <stdio.h>
#include <stdlib.h>
#include "UnitTesting.h"
#include "Core.h"
#include "harness.h"

using namespace SimpleLib;

#define W SCREEN_PAGE_W

namespace
{
    struct FuzzOptions
    {
        int  pages;
        int  ops;
        int  preempt_percent;           // frame starts mid-step
        int  preempt_client_percent;    // client changes mid-step
        bool reloads;
        bool sweeps = false;            // retire unused images and sweep them
        bool layouts = false;           // pages with different layouts, and layout changes
    };

    void Fuzz(uint32_t seed, const FuzzOptions &o)
    {
        World w(o.pages, seed, o.layouts);
        w.preempt_percent        = o.preempt_percent;
        w.preempt_client_percent = o.preempt_client_percent;
        Rng &r = w.rng;
        int max_scroll = (o.pages - 1) * W;

        for (int op = 0; op < o.ops; op++) {
            int dice = r.Range(0, 99);
            if (dice < 45) {
                // Chunks of varying cost, and sometimes gaps reached late
                w.chunk_cost_us = r.Range(30, 250);
                w.gap_delay_us  = r.Chance(20) ? r.Range(0, 400) : 0;
                w.RunFrame();
            } else if (dice < 47 && o.layouts) {
                w.ChangeLayout(r.Range(0, o.pages - 1), (TestLayout)r.Range(0, LAYOUT_COUNT - 1),
                               r.Chance(50) ? COMP_FLUSH : 0);
            } else if (dice < 70) {
                // Scroll: drag a bit, snap to a page, or jump
                int kind = r.Range(0, 9);
                if (kind < 6)
                    w.scroll += r.Range(-120, 120);
                else if (kind < 9)
                    w.scroll = r.Range(0, o.pages - 1) * W;
                else
                    w.scroll = r.Range(-300, max_scroll + 300);
                if (w.scroll < -300) w.scroll = -300;
                if (w.scroll > max_scroll + 300) w.scroll = max_scroll + 300;
            } else if (dice < 95) {
                int page = r.Range(0, o.pages - 1);
                uint32_t mask = r.Chance(30) ? 0xFFFFFFFFu : r.Next();
                if (!mask)
                    mask = 1u << r.Range(0, w.model[page].count - 1);
                w.ChangeKeys(page, mask, r.Chance(35) ? COMP_FLUSH : 0);
                if (o.sweeps && r.Chance(30))
                    w.RetireUnused();
            } else if (dice < 97 && o.reloads) {
                w.Reload();
            } else {
                w.RunFrames(r.Range(1, 6));
            }
        }

        // Quiet: settle on a page and check it shows the model
        w.preempt_percent = w.preempt_client_percent = 0;
        int page = r.Range(0, o.pages - 1);
        w.scroll = page * W;
        w.chunk_cost_us = 65;
        w.gap_delay_us  = 0;
        w.RunFrames(40);
        w.CheckSettledOn(page);
        if (o.sweeps)
            Check(w.images_freed > 0);
    }

    // Seeds per fuzz test: a quick default, or FUZZ_SEEDS=<n> for a soak run
    int SeedCount(int quick)
    {
        const char *env = getenv("FUZZ_SEEDS");
        return env && atoi(env) > 0 ? atoi(env) : quick;
    }

    void FuzzSeeds(int quick, const FuzzOptions &o)
    {
        int count = SeedCount(quick);
        for (uint32_t seed = 1; seed <= (uint32_t)count; seed++) {
            try {
                Fuzz(seed * 7919, o);
            } catch (const AssertionFailed &) {
                fprintf(stderr, "  (fuzz seed %u)\n", seed * 7919);
                throw;
            }
        }
    }
}

Fact("Compositor Fuzz Swipes And Changes")
{
    FuzzSeeds(4, FuzzOptions{ 3, 2000, 0, 0, false });
}

Fact("Compositor Fuzz With Reloads")
{
    FuzzSeeds(4, FuzzOptions{ 4, 2000, 0, 0, true, false, true });
}

Fact("Compositor Fuzz Frame Starts Mid Step")
{
    FuzzSeeds(4, FuzzOptions{ 3, 2000, 10, 0, true, false, true });
}

Fact("Compositor Fuzz Client Changes Mid Step")
{
    FuzzSeeds(4, FuzzOptions{ 3, 2000, 10, 5, true, false, true });
}

Fact("Compositor Fuzz One Page")
{
    FuzzSeeds(4, FuzzOptions{ 1, 1500, 10, 5, true, false, true });
}

Fact("Compositor Fuzz Sweeps")
{
    FuzzSeeds(4, FuzzOptions{ 3, 2000, 0, 0, false, true });
}

Fact("Compositor Fuzz Sweeps With Preemption And Reloads")
{
    FuzzSeeds(4, FuzzOptions{ 3, 2000, 10, 5, true, true, true });
}

Fact("Compositor Fuzz Layouts")
{
    FuzzSeeds(4, FuzzOptions{ 3, 2000, 0, 0, false, false, true });
}
