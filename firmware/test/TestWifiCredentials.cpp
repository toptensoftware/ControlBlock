// Wi-Fi credentials: the validation rules, the NVS blob format, and
// set/get/clear over a fake storage (docs/wifi.md)

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "wifi_credentials.h"
#include "blob_env.h"

using namespace SimpleLib;

namespace
{
    const uint8_t *B(const char *s) { return (const uint8_t *)s; }

    bool Make(WifiCredentials *c, const char *ssid, const char *pass)
    {
        return wifi_credentials_make(c, B(ssid), (uint32_t)strlen(ssid), B(pass), (uint32_t)strlen(pass));
    }

    struct Fixture
    {
        FakeBlobStorage storage;
        WifiConfig      config;
        int             changes = 0;
        WifiCredentials last = {};

        Fixture() { Start(); }

        void Start()
        {
            config.Init(storage.Host());
            config.SetChangedCallback([](const WifiCredentials &now, void *c) {
                Fixture *f = (Fixture *)c;
                f->changes++;
                f->last = now;
            }, this);
        }

        WifiResult Set(const char *ssid, const char *pass)
        {
            return config.Set(B(ssid), (uint32_t)strlen(ssid), B(pass), (uint32_t)strlen(pass));
        }
    };
}

// --- Rules ---

Fact("Wifi Credentials Valid Lengths")
{
    WifiCredentials c;
    Assert(Make(&c, "Home", "password1"));
    Assert(c.ssid_len == 4 && c.pass_len == 9);
    Assert(!strcmp(c.ssid, "Home") && !strcmp(c.pass, "password1"));      // C strings for the driver

    Assert(Make(&c, "Open network", ""));                                // open
    Assert(c.pass_len == 0 && c.pass[0] == 0);

    char ssid[40], pass[70];
    memset(ssid, 'a', sizeof(ssid));
    memset(pass, 'b', sizeof(pass));
    Assert(wifi_credentials_make(&c, B(ssid), 1, B(pass), WIFI_PASS_MIN));            // shortest
    Assert(wifi_credentials_make(&c, B(ssid), WIFI_SSID_MAX, B(pass), WIFI_PASS_MAX)); // longest
    Assert(c.ssid_len == 32 && c.pass_len == 63);
    Assert(c.ssid[32] == 0 && c.pass[63] == 0);
}

Fact("Wifi Credentials Invalid Lengths Are Rejected")
{
    WifiCredentials c;
    char ssid[40], pass[70];
    memset(ssid, 'a', sizeof(ssid));
    memset(pass, 'b', sizeof(pass));
    Assert(!wifi_credentials_make(&c, B(ssid), WIFI_SSID_MAX + 1, B(pass), 0));       // ssid too long
    for (uint32_t n = 1; n < WIFI_PASS_MIN; n++)
        Assert(!wifi_credentials_make(&c, B(ssid), 4, B(pass), n));                   // 1..7
    Assert(!wifi_credentials_make(&c, B(ssid), 4, B(pass), WIFI_PASS_MAX + 1));       // too long
    Assert(!wifi_credentials_make(&c, B(ssid), 0, B(pass), 8));                       // password without an ssid
    Assert(!wifi_credentials_make(&c, B(ssid), 255, B(pass), 0));
}

Fact("Wifi Credentials Reject NUL Bytes")
{
    WifiCredentials c;
    Assert(!wifi_credentials_make(&c, B("ab\0cd"), 5, B(""), 0));
    Assert(!wifi_credentials_make(&c, B("abc"), 3, B("pass\0word"), 9));
    Assert(!wifi_credentials_make(&c, B("\0"), 1, B(""), 0));
}

Fact("Wifi Credentials Empty Ssid Is Clear")
{
    WifiCredentials c;
    Assert(wifi_credentials_make(&c, B(""), 0, B(""), 0));
    Assert(c.ssid_len == 0 && c.pass_len == 0 && c.ssid[0] == 0 && c.pass[0] == 0);
}

Fact("Wifi Credentials Ssid Is Raw Bytes")
{
    WifiCredentials c;
    Assert(wifi_credentials_make(&c, B("\xFF\xFE\x80"), 3, B(""), 0));           // not UTF-8
    Assert(c.ssid_len == 3 && (uint8_t)c.ssid[0] == 0xFF);
}

// --- Blob ---

Fact("Wifi Blob Layout And Round Trip")
{
    WifiCredentials c, d;
    Assert(Make(&c, "Home", "password1"));
    uint8_t blob[WIFI_BLOB_MAX];
    uint32_t n = wifi_blob_encode(c, blob);
    Assert(n == 4 + 4 + 9);
    Assert(blob[0] == 1 && blob[1] == 4 && blob[2] == 9 && blob[3] == 0);
    Assert(!memcmp(blob + 4, "Home", 4) && !memcmp(blob + 8, "password1", 9));

    Assert(wifi_blob_decode(blob, n, &d));
    Assert(wifi_credentials_equal(c, d));

    Assert(Make(&c, "Open", ""));                                           // open network
    n = wifi_blob_encode(c, blob);
    Assert(n == 4 + 4);
    Assert(wifi_blob_decode(blob, n, &d) && wifi_credentials_equal(c, d));
}

Fact("Wifi Blob Corrupt Decodes As None")
{
    WifiCredentials c, d;
    Assert(Make(&c, "Home", "password1"));
    uint8_t blob[WIFI_BLOB_MAX];
    uint32_t n = wifi_blob_encode(c, blob);

    Assert(!wifi_blob_decode(blob, 0, &d));
    Assert(!wifi_blob_decode(blob, 3, &d));                                 // no whole header
    Assert(!wifi_blob_decode(blob, n - 1, &d));                             // truncated
    uint8_t bad[WIFI_BLOB_MAX];

    memcpy(bad, blob, n); bad[0] = 2;
    Assert(!wifi_blob_decode(bad, n, &d));                                  // version
    memcpy(bad, blob, n); bad[0] = 0;
    Assert(!wifi_blob_decode(bad, n, &d));
    memcpy(bad, blob, n); bad[1] = 0;
    Assert(!wifi_blob_decode(bad, n, &d));                                  // empty ssid is never stored
    memcpy(bad, blob, n); bad[1] = 33;
    Assert(!wifi_blob_decode(bad, n, &d));                                  // ssid too long
    memcpy(bad, blob, n); bad[2] = 7;
    Assert(!wifi_blob_decode(bad, n, &d));                                  // password too short (and length mismatch)
    memcpy(bad, blob, n); bad[1] = 200; bad[2] = 200;
    Assert(!wifi_blob_decode(bad, n, &d));                                  // lengths past the end
    memcpy(bad, blob, n); bad[5] = 0;
    Assert(!wifi_blob_decode(bad, n, &d));                                  // NUL in the ssid
    memcpy(bad, blob, n); bad[3] = 1;
    Assert(!wifi_blob_decode(bad, n, &d));                                  // reserved byte must be 0
    Assert(!wifi_blob_decode(blob, n + 1, &d));                             // trailing bytes
}

// --- Set, get, clear ---

Fact("Wifi Config Starts Empty")
{
    Fixture f;
    Assert(!f.config.Stored());
    Assert(f.config.Get().ssid_len == 0 && f.config.Get().pass_len == 0);
    Assert(f.storage.saves == 0 && f.storage.erases == 0);
    Assert(f.changes == 0);
}

Fact("Wifi Config Set Stores And Get Returns")
{
    Fixture f;
    Assert(f.Set("Home", "password1") == WifiResult::Ok);
    Assert(f.config.Stored());
    Assert(!strcmp(f.config.Get().ssid, "Home") && !strcmp(f.config.Get().pass, "password1"));
    Assert(f.storage.saves == 1 && f.storage.present);
    Assert(f.changes == 1 && !strcmp(f.last.ssid, "Home"));
}

Fact("Wifi Config Survives A Restart")
{
    Fixture f;
    f.Set("Home", "password1");
    WifiConfig again;
    again.Init(f.storage.Host());
    Assert(again.Stored() && !strcmp(again.Get().ssid, "Home") && !strcmp(again.Get().pass, "password1"));
}

Fact("Wifi Config Same Credentials Change Nothing")
{
    Fixture f;
    f.Set("Home", "password1");
    Assert(f.Set("Home", "password1") == WifiResult::Ok);
    Assert(f.storage.saves == 1);                                           // no flash write
    Assert(f.changes == 1);                                                 // no reconnect

    Assert(f.Set("Home", "password2") == WifiResult::Ok);                   // a real change
    Assert(f.storage.saves == 2 && f.changes == 2 && !strcmp(f.last.pass, "password2"));
    Assert(f.Set("Home2", "password2") == WifiResult::Ok);
    Assert(f.storage.saves == 3 && f.changes == 3);
}

Fact("Wifi Config Open Network")
{
    Fixture f;
    Assert(f.Set("Cafe", "") == WifiResult::Ok);
    Assert(f.config.Stored() && f.config.Get().pass_len == 0);
    Assert(f.Set("Cafe", "") == WifiResult::Ok && f.storage.saves == 1);
}

Fact("Wifi Config Clear Erases")
{
    Fixture f;
    f.Set("Home", "password1");
    Assert(f.Set("", "") == WifiResult::Ok);
    Assert(!f.config.Stored() && f.config.Get().ssid_len == 0);
    Assert(f.storage.erases == 1 && !f.storage.present);
    Assert(f.changes == 2 && f.last.ssid_len == 0);

    // Clearing nothing is no change
    Assert(f.Set("", "") == WifiResult::Ok);
    Assert(f.storage.erases == 1 && f.changes == 2);

    // And a clear followed by a set works
    Assert(f.Set("Home", "password1") == WifiResult::Ok);
    Assert(f.config.Stored() && f.changes == 3);
}

Fact("Wifi Config Bad Arguments Change Nothing")
{
    Fixture f;
    f.Set("Home", "password1");
    Assert(f.Set("Home", "short") == WifiResult::BadArg);
    Assert(f.Set("", "password1") == WifiResult::BadArg);
    Assert(f.config.Set(B("a\0b"), 3, B(""), 0) == WifiResult::BadArg);
    Assert(!strcmp(f.config.Get().ssid, "Home") && !strcmp(f.config.Get().pass, "password1"));
    Assert(f.storage.saves == 1 && f.storage.erases == 0 && f.changes == 1);
}

Fact("Wifi Config Failed Save Keeps The Old Credentials")
{
    Fixture f;
    f.Set("Home", "password1");
    f.storage.fail_save = true;
    Assert(f.Set("Other", "password2") == WifiResult::IoError);
    Assert(!strcmp(f.config.Get().ssid, "Home"));
    Assert(f.changes == 1);                                                 // no notification
    Assert(!strcmp(f.last.ssid, "Home"));

    f.storage.fail_save = false;
    Assert(f.Set("Other", "password2") == WifiResult::Ok);                  // and it can be retried
    Assert(!strcmp(f.config.Get().ssid, "Other") && f.changes == 2);
}

Fact("Wifi Config Failed Erase Keeps The Old Credentials")
{
    Fixture f;
    f.Set("Home", "password1");
    f.storage.fail_erase = true;
    Assert(f.Set("", "") == WifiResult::IoError);
    Assert(f.config.Stored() && !strcmp(f.config.Get().ssid, "Home") && f.changes == 1);
}

Fact("Wifi Config Corrupt Stored Blob Is Erased And Ignored")
{
    FakeBlobStorage storage;
    static const uint8_t junk[] = { 9, 9, 9, 9, 9 };
    storage.Preload(junk, sizeof(junk));
    WifiConfig config;
    config.Init(storage.Host());                                            // not a boot failure
    Assert(!config.Stored());
    Assert(storage.erases == 1 && !storage.present);
}

Fact("Wifi Config Unreadable Storage Is Treated As None")
{
    FakeBlobStorage storage;
    WifiCredentials c;
    Make(&c, "Home", "password1");
    uint8_t blob[WIFI_BLOB_MAX];
    storage.Preload(blob, wifi_blob_encode(c, blob));
    storage.fail_load = true;
    WifiConfig config;
    config.Init(storage.Host());
    Assert(!config.Stored());
    Assert(storage.erases == 0);                                            // a read error doesn't destroy anything
}

Fact("Wifi Config Without Storage Reports An Error")
{
    WifiConfig config;                                                      // never Init'd
    Assert(!config.Stored());
    Assert(config.Set(B("Home"), 4, B("password1"), 9) == WifiResult::IoError);
    Assert(!config.Stored());
}

Fact("Wifi Config Bad Args Are Reported Before Storage Errors")
{
    Fixture f;
    f.storage.fail_save = true;
    Assert(f.Set("Home", "x") == WifiResult::BadArg);
    Assert(f.storage.saves == 0);
}
