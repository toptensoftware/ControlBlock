// Config apply. Spec: docs/standalone.md.

#include "config_apply.h"

bool ConfigApplyHome(const ConfigSummary &config, const uint8_t *blob, ImageStore *store, KeyLayout *layout)
{
    KeyDef   defs[COMP_KEYS] = {};
    uint32_t added[COMP_KEYS * KEY_SLOTS];
    int      added_count = 0;
    bool     ok = true;

    for (int k = 0; k < COMP_KEYS && ok; k++) {
        KeyDef &def = defs[k];
        def.page = 0;
        def.key  = (uint8_t)k;
        bool any = false;
        for (int s = 0; s < KEY_SLOTS && ok; s++) {
            const ConfigImage &im = config.home[k][s];
            if (!im.offset)
                continue;
            any = true;
            bool known = store->IsLive(im.offset);
            ok = store->AddExternal(im.offset, blob + im.offset, im.size) == ImageResult::Ok;
            if (ok && !known)
                added[added_count++] = im.offset;
            def.ids[s] = im.offset;
        }
        def.flags = any ? 0 : KEYDEF_EMPTY;
    }

    ok = ok && layout->SetKeys(defs, COMP_KEYS) == LayoutResult::Ok;
    if (!ok) {
        // Nothing uses these yet (SetKeys is all or nothing), so they go at once
        for (int i = 0; i < added_count; i++)
            store->Free(added[i]);
    }
    return ok;
}
