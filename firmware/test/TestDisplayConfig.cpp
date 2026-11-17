// The display timing setting (DISPLAY_SET): the stored blob, defaults, set
// and the range checks, over a fake storage (wip/wip-frame-rate-experiment.md, phase 3)

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "display_config.h"
#include "blob_env.h"

using namespace SimpleLib;

namespace
{
    struct Fixture
    {
        FakeBlobStorage storage;
        DisplayConfig   config;
        int             changes = 0;
        DisplayTimingSetting last = {};

        void Start(uint32_t build_mhz = 20)
        {
            config.Init(storage.Host(), DisplayTimingLimitsFor(build_mhz));
            config.SetChangedCallback([](const DisplayTimingSetting &t, void *c) {
                ((Fixture *)c)->changes++;
                ((Fixture *)c)->last = t;
            }, this);
        }

        void Preload(uint8_t mhz, uint16_t lines)
        {
            uint8_t blob[DISPLAY_CONFIG_BLOB_SIZE];
            DisplayTimingSetting t = { mhz, lines };
            storage.Preload(blob, display_config_blob_encode(t, blob));
        }
    };
}

// --- Blob ---

Fact("Display Config Blob Round Trip")
{
    uint8_t blob[DISPLAY_CONFIG_BLOB_SIZE];
    DisplayTimingSetting t = { 16, 750 };
    uint32_t len = display_config_blob_encode(t, blob);
    Assert(len == DISPLAY_CONFIG_BLOB_SIZE);
    Assert(blob[0] == DISPLAY_CONFIG_BLOB_VERSION && blob[1] == 16 && blob[2] == (750 & 0xFF) && blob[3] == 750 >> 8);
    DisplayTimingSetting out = {};
    Assert(display_config_blob_decode(blob, len, &out));
    Assert(out.mhz == 16 && out.lines == 750);
}

Fact("Display Config Blob Decode Rejects Bad Blobs")
{
    uint8_t blob[DISPLAY_CONFIG_BLOB_SIZE + 1] = {};
    DisplayTimingSetting t = { 20, 750 };
    uint32_t len = display_config_blob_encode(t, blob);
    DisplayTimingSetting out;
    Assert(!display_config_blob_decode(blob, len - 1, &out));          // short
    Assert(!display_config_blob_decode(blob, len + 1, &out));          // long
    blob[0] = 99;
    Assert(!display_config_blob_decode(blob, len, &out));              // version
}

// --- DisplayConfig ---

Fact("Display Config Defaults With Nothing Stored")
{
    Fixture f;
    f.Start();
    Assert(f.config.Get().mhz == 20 && f.config.Get().lines == 500);
    Assert(f.config.IsDefault());
    Assert(f.config.Limits().min_mhz == 16 && f.config.Limits().max_lines == 1024);
}

Fact("Display Config Loads The Stored Timing")
{
    Fixture f;
    f.Preload(16, 750);
    f.Start();
    Assert(f.config.Get().mhz == 16 && f.config.Get().lines == 750);
    Assert(!f.config.IsDefault());
    Assert(f.changes == 0);                                             // not told at Init
}

Fact("Display Config Bad Or Out Of Range Stored Blob Is Erased")
{
    Fixture f;
    uint8_t bad[3] = { 99, 20, 0 };
    f.storage.Preload(bad, 3);
    f.Start();
    Assert(f.config.IsDefault());
    Assert(f.storage.erases == 1 && !f.storage.present);

    // Stored by a build with a faster clock: this build can't go above its own
    Fixture g;
    g.Preload(25, 750);
    g.Start(20);
    Assert(g.config.IsDefault() && g.config.Get().mhz == 20 && g.config.Get().lines == 500);
    Assert(g.storage.erases == 1 && !g.storage.present);

    Fixture h;
    h.Preload(20, 1100);
    h.Start();
    Assert(h.config.IsDefault() && !h.storage.present);
}

Fact("Display Config Set Saves And Tells")
{
    Fixture f;
    f.Start();
    Assert(f.config.Set(20, 750) == SettingResult::Ok);
    Assert(f.config.Get().mhz == 20 && f.config.Get().lines == 750);
    Assert(!f.config.IsDefault());
    Assert(f.changes == 1 && f.last.mhz == 20 && f.last.lines == 750);
    DisplayTimingSetting stored;
    Assert(f.storage.present && display_config_blob_decode(f.storage.blob, f.storage.blob_len, &stored));
    Assert(stored.mhz == 20 && stored.lines == 750);

    Assert(f.config.Set(20, 750) == SettingResult::Ok);                 // unchanged: no write
    Assert(f.storage.saves == 1 && f.changes == 1);

    Assert(f.config.Set(16, 1024) == SettingResult::Ok);
    Assert(f.changes == 2 && f.last.mhz == 16 && f.last.lines == 1024);
}

Fact("Display Config Zero Is The Default Per Field")
{
    Fixture f;
    f.Start();
    Assert(f.config.Set(16, 750) == SettingResult::Ok);
    Assert(f.config.Set(0, 750) == SettingResult::Ok);                  // the build's clock
    Assert(f.config.Get().mhz == 20 && f.config.Get().lines == 750);
    Assert(f.config.Set(16, 0) == SettingResult::Ok);                   // the build's lines
    Assert(f.config.Get().mhz == 16 && f.config.Get().lines == 500);

    // Both: the defaults, which aren't stored
    Assert(f.config.Set(0, 0) == SettingResult::Ok);
    Assert(f.config.IsDefault() && f.config.Get().mhz == 20 && f.config.Get().lines == 500);
    Assert(!f.storage.present);
    Assert(f.changes == 4);

    Assert(f.config.Set(0, 0) == SettingResult::Ok);                    // already: nothing
    Assert(f.changes == 4);

    // The defaults spelled out are the defaults too
    Assert(f.config.Set(16, 600) == SettingResult::Ok);
    Assert(f.config.Set(20, 500) == SettingResult::Ok);
    Assert(f.config.IsDefault() && !f.storage.present);
}

Fact("Display Config Set Out Of Range Changes Nothing")
{
    Fixture f;
    f.Start();
    Assert(f.config.Set(15, 500) == SettingResult::BadArg);             // below the panel's floor
    Assert(f.config.Set(21, 500) == SettingResult::BadArg);             // above the build's clock
    Assert(f.config.Set(20, 499) == SettingResult::BadArg);
    Assert(f.config.Set(20, 1025) == SettingResult::BadArg);            // the LCD_CAM's 10 bits
    Assert(f.config.Set(255, 65535) == SettingResult::BadArg);
    Assert(f.config.IsDefault());
    Assert(f.storage.saves == 0 && f.storage.erases == 0 && f.changes == 0);
}

Fact("Display Config Save Failure Keeps The Old Timing")
{
    Fixture f;
    f.Start();
    f.storage.fail_save = true;
    Assert(f.config.Set(16, 750) == SettingResult::IoError);
    Assert(f.config.IsDefault() && f.changes == 0);
}
