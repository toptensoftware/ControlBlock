// Clock settings: the time zone and NTP server rules, the NVS blob format,
// and set/get over a fake storage (wip/wip-status-bar-clock.md)

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "clock_config.h"
#include "blob_env.h"

using namespace SimpleLib;

namespace
{
    const uint8_t *B(const char *s) { return (const uint8_t *)s; }

    bool Make(ClockSettings *c, const char *tz, const char *server)
    {
        return clock_settings_make(c, B(tz), (uint32_t)strlen(tz), B(server), (uint32_t)strlen(server));
    }

    struct Fixture
    {
        FakeBlobStorage storage;
        ClockConfig     config;
        int             changes = 0;
        ClockSettings   last = {};

        Fixture() { Start(); }

        void Start()
        {
            config.Init(storage.Host());
            config.SetChangedCallback([](const ClockSettings &now, void *c) {
                Fixture *f = (Fixture *)c;
                f->changes++;
                f->last = now;
            }, this);
        }

        SettingResult Set(uint8_t mask, const char *tz, const char *server)
        {
            return config.Set(mask, B(tz), (uint32_t)strlen(tz), B(server), (uint32_t)strlen(server));
        }
    };

    void Preload(FakeBlobStorage &s, const char *tz, const char *server)
    {
        ClockSettings c;
        Assert(Make(&c, tz, server));
        uint8_t blob[CLOCK_BLOB_MAX];
        s.Preload(blob, clock_blob_encode(c, blob));
    }
}

// --- Rules ---

Fact("Clock Settings Empty Means Default")
{
    ClockSettings c;
    Assert(Make(&c, "", ""));
    Assert(!strcmp(c.tz, CLOCK_TZ_DEFAULT) && c.tz_len == strlen(CLOCK_TZ_DEFAULT));
    Assert(!strcmp(c.server, CLOCK_SERVER_DEFAULT) && c.server_len == strlen(CLOCK_SERVER_DEFAULT));
    Assert(!strcmp(CLOCK_TZ_DEFAULT, "UTC0") && !strcmp(CLOCK_SERVER_DEFAULT, "pool.ntp.org"));
}

Fact("Clock Settings Valid Values")
{
    ClockSettings c;
    Assert(Make(&c, "AEST-10AEDT,M10.1.0,M4.1.0/3", "time.example-1.com"));
    Assert(!strcmp(c.tz, "AEST-10AEDT,M10.1.0,M4.1.0/3"));                  // NUL terminated for setenv
    Assert(!strcmp(c.server, "time.example-1.com"));
    Assert(Make(&c, "<+0530>-5:30", "192.168.1.10"));                        // angle-bracket zones, IP literals
}

Fact("Clock Settings Length Limits")
{
    char s[70];
    memset(s, 'a', sizeof(s));
    ClockSettings c;
    Assert(clock_settings_make(&c, B(s), CLOCK_FIELD_MAX, B(s), CLOCK_FIELD_MAX));
    Assert(c.tz_len == 63 && c.server_len == 63 && c.tz[63] == 0 && c.server[63] == 0);
    Assert(!clock_settings_make(&c, B(s), CLOCK_FIELD_MAX + 1, B("x"), 1));
    Assert(!clock_settings_make(&c, B("x"), 1, B(s), CLOCK_FIELD_MAX + 1));
}

Fact("Clock Settings Bad Time Zone Characters")
{
    ClockSettings c;
    Assert(!Make(&c, "UTC 0", ""));                                          // space
    Assert(!Make(&c, "UTC\t0", ""));                                         // control
    Assert(!Make(&c, "UTC\x7f", ""));                                        // DEL
    Assert(!Make(&c, "UTC\xc3\xa9", ""));                                    // non-ASCII
    Assert(!clock_settings_make(&c, B("UT\0C"), 4, B(""), 0));               // NUL
}

Fact("Clock Settings Bad Server Characters")
{
    ClockSettings c;
    Assert(!Make(&c, "", "pool ntp.org"));
    Assert(!Make(&c, "", "pool_ntp.org"));
    Assert(!Make(&c, "", "http://pool.ntp.org"));
    Assert(!Make(&c, "", "pool.ntp.org:123"));
    Assert(!Make(&c, "", "p\xc3\xa9.org"));
}

Fact("Clock Blob Round Trip")
{
    ClockSettings c, d;
    Assert(Make(&c, "EST5EDT,M3.2.0,M11.1.0", "time.nist.gov"));
    uint8_t blob[CLOCK_BLOB_MAX];
    uint32_t n = clock_blob_encode(c, blob);
    Assert(n == CLOCK_BLOB_HEADER + c.tz_len + c.server_len && n <= CLOCK_BLOB_MAX);
    Assert(blob[0] == CLOCK_BLOB_VERSION && blob[1] == c.tz_len && blob[2] == c.server_len && blob[3] == 0);
    Assert(clock_blob_decode(blob, n, &d));
    Assert(clock_settings_equal(c, d));
}

Fact("Clock Blob Decode Rejects Bad Blobs")
{
    ClockSettings c, d;
    Assert(Make(&c, "UTC0", "pool.ntp.org"));
    uint8_t blob[CLOCK_BLOB_MAX + 1];
    uint32_t n = clock_blob_encode(c, blob);

    Assert(!clock_blob_decode(blob, CLOCK_BLOB_HEADER - 1, &d));             // short header
    Assert(!clock_blob_decode(blob, n - 1, &d));                             // short body
    blob[n] = 'x';
    Assert(!clock_blob_decode(blob, n + 1, &d));                             // leftovers

    uint8_t bad[CLOCK_BLOB_MAX];
    memcpy(bad, blob, n);
    bad[0] = CLOCK_BLOB_VERSION + 1;
    Assert(!clock_blob_decode(bad, n, &d));                                  // version
    memcpy(bad, blob, n);
    bad[3] = 1;
    Assert(!clock_blob_decode(bad, n, &d));                                  // reserved
    memcpy(bad, blob, n);
    bad[CLOCK_BLOB_HEADER] = ' ';
    Assert(!clock_blob_decode(bad, n, &d));                                  // bad tz character

    static const uint8_t empty_tz[] = { CLOCK_BLOB_VERSION, 0, 1, 0, 'a' };
    Assert(!clock_blob_decode(empty_tz, sizeof(empty_tz), &d));              // stored values are never empty
}

// --- Config over storage ---

Fact("Clock Config Defaults With Nothing Stored")
{
    Fixture f;
    Assert(!strcmp(f.config.Get().tz, "UTC0") && !strcmp(f.config.Get().server, "pool.ntp.org"));
    Assert(f.storage.loads == 1 && f.storage.saves == 0 && f.storage.erases == 0);
}

Fact("Clock Config Loads Stored Settings")
{
    Fixture f;
    Preload(f.storage, "AEST-10AEDT,M10.1.0,M4.1.0/3", "au.pool.ntp.org");
    f.Start();
    Assert(!strcmp(f.config.Get().tz, "AEST-10AEDT,M10.1.0,M4.1.0/3"));
    Assert(!strcmp(f.config.Get().server, "au.pool.ntp.org"));
    Assert(f.changes == 0);                                                  // not told at Init
}

Fact("Clock Config Corrupt Stored Blob Is Erased And Ignored")
{
    FakeBlobStorage storage;
    static const uint8_t junk[] = { 9, 9, 9, 9, 9 };
    storage.Preload(junk, sizeof(junk));
    ClockConfig config;
    config.Init(storage.Host());
    Assert(!strcmp(config.Get().tz, "UTC0") && !strcmp(config.Get().server, "pool.ntp.org"));
    Assert(storage.erases == 1 && !storage.present);
}

Fact("Clock Config Unreadable Storage Is Treated As None")
{
    FakeBlobStorage storage;
    Preload(storage, "EST5", "time.nist.gov");
    storage.fail_load = true;
    ClockConfig config;
    config.Init(storage.Host());
    Assert(!strcmp(config.Get().tz, "UTC0"));
    Assert(storage.erases == 0);                                             // a read error doesn't destroy anything
}

Fact("Clock Config Set Time Zone Only")
{
    Fixture f;
    Assert(f.Set(CLOCK_SET_TZ, "EST5EDT", "bad server!") == SettingResult::Ok);   // an unmasked field isn't looked at
    Assert(!strcmp(f.config.Get().tz, "EST5EDT") && !strcmp(f.config.Get().server, "pool.ntp.org"));
    Assert(f.storage.saves == 1 && f.changes == 1 && !strcmp(f.last.tz, "EST5EDT"));

    f.Start();                                                               // survives a restart
    Assert(!strcmp(f.config.Get().tz, "EST5EDT") && !strcmp(f.config.Get().server, "pool.ntp.org"));
}

Fact("Clock Config Set Server Only")
{
    Fixture f;
    f.Set(CLOCK_SET_TZ, "EST5EDT", "");
    Assert(f.Set(CLOCK_SET_SERVER, "", "time.nist.gov") == SettingResult::Ok);
    Assert(!strcmp(f.config.Get().tz, "EST5EDT") && !strcmp(f.config.Get().server, "time.nist.gov"));
    Assert(f.storage.saves == 2 && f.changes == 2);
}

Fact("Clock Config Set Both And Back To Defaults")
{
    Fixture f;
    Assert(f.Set(CLOCK_SET_TZ | CLOCK_SET_SERVER, "EST5", "time.nist.gov") == SettingResult::Ok);
    Assert(f.Set(CLOCK_SET_TZ | CLOCK_SET_SERVER, "", "") == SettingResult::Ok);    // empty = default
    Assert(!strcmp(f.config.Get().tz, "UTC0") && !strcmp(f.config.Get().server, "pool.ntp.org"));
    Assert(f.changes == 2 && !strcmp(f.last.server, "pool.ntp.org"));
}

Fact("Clock Config Unchanged Set Writes Nothing")
{
    Fixture f;
    Assert(f.Set(CLOCK_SET_TZ | CLOCK_SET_SERVER, "", "") == SettingResult::Ok);    // already the defaults
    Assert(f.Set(CLOCK_SET_TZ, "UTC0", "") == SettingResult::Ok);
    Assert(f.Set(0, "", "") == SettingResult::Ok);
    Assert(f.storage.saves == 0 && f.changes == 0);

    f.Set(CLOCK_SET_TZ, "EST5", "");
    Assert(f.Set(CLOCK_SET_TZ, "EST5", "") == SettingResult::Ok);
    Assert(f.storage.saves == 1 && f.changes == 1);
}

Fact("Clock Config Bad Values Change Nothing")
{
    Fixture f;
    Assert(f.Set(CLOCK_SET_TZ, "UTC 0", "") == SettingResult::BadArg);
    Assert(f.Set(CLOCK_SET_TZ | CLOCK_SET_SERVER, "EST5", "bad server") == SettingResult::BadArg);   // all or nothing
    Assert(!strcmp(f.config.Get().tz, "UTC0"));
    Assert(f.storage.saves == 0 && f.changes == 0);
}

Fact("Clock Config Save Failure Keeps The Old Settings")
{
    Fixture f;
    f.Set(CLOCK_SET_TZ, "EST5", "");
    f.storage.fail_save = true;
    Assert(f.Set(CLOCK_SET_TZ, "CET-1", "") == SettingResult::IoError);
    Assert(!strcmp(f.config.Get().tz, "EST5") && f.changes == 1);
}

Fact("Clock Config Without Storage Reports An Error")
{
    ClockConfig config;                                                      // never Init'd
    Assert(!strcmp(config.Get().tz, "UTC0") && !strcmp(config.Get().server, "pool.ntp.org"));
    Assert(config.Set(CLOCK_SET_TZ, B("EST5"), 4, B(""), 0) == SettingResult::IoError);
    Assert(!strcmp(config.Get().tz, "UTC0"));
}
