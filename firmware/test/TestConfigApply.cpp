// Config apply: a validated blob's Home becomes the key layout, its images
// registered with the store in place (docs/standalone.md "Image IDs").

#include "UnitTesting.h"
#include "Core.h"
#include "config_apply.h"
#include "config_env.h"
#include "store_env.h"

using namespace SimpleLib;
using namespace config_env;

namespace
{
    struct Change { int page; uint32_t flags; };

    struct Fixture
    {
        StoreEnv      env;
        ImageStore    store;
        KeyLayout     layout;
        List<Change>  changes;
        List<uint8_t> blob;
        ConfigSummary summary = {};

        Fixture()
        {
            store.Init(env.Host(), 1u << 20, COMP_KEY_SIZE, COMP_KEY_SIZE);
            store.SetListener(&layout);
            layout.Init(&store, Changed, this);
            blob = LoadSample();
            Assert(ConfigValidate(blob.GetBuffer(), (uint32_t)blob.GetCount(), &summary) == ConfigError::Ok);
        }

        static void Changed(int page, uint32_t flags, void *ctx)
        {
            ((Fixture *)ctx)->changes.Add(Change{ page, flags });
        }

        bool Apply() { return ConfigApplyHome(summary, blob.GetBuffer(), &store, &layout); }
        const void *At(uint32_t offset) { return blob.GetBuffer() + offset; }

        comp_key_t Key(int key)
        {
            comp_key_t keys[COMP_KEYS];
            Assert(layout.GetPage(0, keys));
            return keys[key];
        }

        int DistinctImages()
        {
            Set<uint32_t> ids;
            for (int k = 0; k < COMP_KEYS; k++)
                for (int s = 0; s < KEY_SLOTS; s++)
                    if (summary.home[k][s].offset)
                        ids.Add(summary.home[k][s].offset);
            return ids.GetCount();
        }
    };
}

Fact("ConfigApply Shows Home's Keys")
{
    Fixture f;
    Assert(f.Apply());

    const ConfigImage (*im)[KEY_SLOTS] = f.summary.home;
    comp_key_t k0 = f.Key(0);
    Assert(k0.bg == f.At(im[0][KEY_SLOT_BG].offset));
    Assert(k0.fg == f.At(im[0][KEY_SLOT_FG].offset));
    Assert(f.layout.IsActive(0, 0) && f.layout.IsActive(0, 4));
    comp_key_t k4 = f.Key(4);
    Assert(k4.bg == f.At(im[4][KEY_SLOT_BG].offset) && k4.fg == nullptr);

    // Keys with no images are empty
    for (int k : { 3, 5, 14 }) {
        Assert(!f.layout.IsActive(0, k));
        Assert(!f.Key(k).bg && !f.Key(k).fg);
    }
    Assert(f.layout.PageCount() == 1);
}

Fact("ConfigApply Registers Each Image Once Under Its Offset")
{
    Fixture f;
    Assert(f.Apply());
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == (uint32_t)f.DistinctImages());
    Assert(st.used == 0 && f.env.LiveBlocks() == 0);          // nothing copied

    for (int k = 0; k < COMP_KEYS; k++) {
        for (int s = 0; s < KEY_SLOTS; s++) {
            uint32_t id = f.summary.home[k][s].offset;
            if (!id)
                continue;
            ImageEntry *e = f.store.Find(id);
            Assert(e && e->external && e->pixels == f.At(id));
        }
    }
    // The face shared by keys 0..2 is one entry held by all of them (normal and pressed)
    uint32_t face = f.summary.home[0][KEY_SLOT_BG].offset;
    Assert(f.store.Find(face)->key_refs == 3);
}

Fact("ConfigApply Pressed Keys Show The Pressed Pair")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.layout.SetPressed(0, 0, true));
    Assert(f.Key(0).bg == f.At(f.summary.home[0][KEY_SLOT_PRESSED_BG].offset));
    Assert(f.layout.SetPressed(0, 0, false));
    Assert(f.Key(0).bg == f.At(f.summary.home[0][KEY_SLOT_BG].offset));
}

Fact("ConfigApply Reports Page 0 Changed With A Flush")
{
    Fixture f;
    Assert(f.Apply());
    Assert(f.changes.GetCount() == 1 && f.changes[0].page == 0 && f.changes[0].flags == COMP_FLUSH);
}

Fact("ConfigApply Failure Leaves The Store And Layout Empty")
{
    Fixture f;
    // An image that fails the store check, after earlier images went in
    f.summary.home[7][KEY_SLOT_BG] = f.summary.home[0][KEY_SLOT_BG];
    f.summary.home[7][KEY_SLOT_FG] = ConfigImage{ 700, 64 };
    Assert(!f.Apply());

    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == 0);
    Assert(f.changes.GetCount() == 0);
    for (int k = 0; k < COMP_KEYS; k++)
        Assert(!f.layout.IsActive(0, k) && !f.Key(k).bg);
}

Fact("ConfigApply Can Rebuild After A Reset")
{
    Fixture f;
    Assert(f.Apply());
    f.layout.Clear();
    f.store.Reset();
    f.changes.Clear();
    Assert(f.Apply());
    Assert(f.layout.IsActive(0, 0));
    ImageStoreStats st;
    f.store.GetStats(&st);
    Assert(st.count == (uint32_t)f.DistinctImages());
}

Fact("ConfigApply Copes With An Empty Home")
{
    Fixture f;
    f.summary = {};
    Assert(f.Apply());
    for (int k = 0; k < COMP_KEYS; k++)
        Assert(!f.layout.IsActive(0, k));
}
