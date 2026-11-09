// The device name (mDNS/DHCP hostname): rules, default, the NVS blob and
// set/get over a fake storage (wip/wip-wifi-diagnostics.md step 5)

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "device_name.h"
#include "blob_env.h"

using namespace SimpleLib;

namespace
{
    const uint8_t MAC[6] = { 0x10, 0x20, 0x30, 0xa1, 0xb2, 0xc3 };

    bool Make(char *out, const char *s)
    {
        return device_name_make(out, (const uint8_t *)s, (uint32_t)strlen(s));
    }

    struct Fixture
    {
        FakeBlobStorage storage;
        DeviceName      name;
        int             changes = 0;

        void Start()
        {
            name.Init(storage.Host(), MAC);
            name.SetChangedCallback([](const char *, void *c) { ((Fixture *)c)->changes++; }, this);
        }

        SettingResult Set(const char *s)
        {
            return name.Set((const uint8_t *)s, (uint32_t)strlen(s));
        }
    };
}

// --- Rules ---

Fact("Device Name Accepts A Hostname Label")
{
    char n[DEVICE_NAME_MAX + 1];
    Assert(Make(n, "a"));
    Assert(strcmp(n, "a") == 0);
    Assert(Make(n, "living-room-2"));
    Assert(strcmp(n, "living-room-2") == 0);
    Assert(Make(n, "abcdefghijklmnopqrstuvwxyz012345"));     // 32
}

Fact("Device Name Lowercases")
{
    char n[DEVICE_NAME_MAX + 1];
    Assert(Make(n, "Den-TV"));
    Assert(strcmp(n, "den-tv") == 0);
}

Fact("Device Name Rejects Bad Names")
{
    char n[DEVICE_NAME_MAX + 1];
    Assert(!Make(n, ""));
    Assert(!Make(n, "abcdefghijklmnopqrstuvwxyz0123456"));   // 33
    Assert(!Make(n, "-abc"));
    Assert(!Make(n, "abc-"));
    Assert(!Make(n, "a b"));
    Assert(!Make(n, "a.b"));
    Assert(!Make(n, "a_b"));
    Assert(!device_name_make(n, (const uint8_t *)"a\0b", 3));
}

Fact("Device Name Default From The MAC")
{
    char n[DEVICE_NAME_MAX + 1];
    device_name_default(MAC, n);
    Assert(strcmp(n, "controlblock-a1b2c3") == 0);
}

// --- Blob ---

Fact("Device Name Blob Round Trip")
{
    uint8_t blob[DEVICE_NAME_BLOB_MAX];
    uint32_t len = device_name_blob_encode("den", blob);
    Assert(len == DEVICE_NAME_BLOB_HEADER + 3);
    char n[DEVICE_NAME_MAX + 1];
    Assert(device_name_blob_decode(blob, len, n));
    Assert(strcmp(n, "den") == 0);
}

Fact("Device Name Blob Decode Rejects Bad Blobs")
{
    uint8_t blob[DEVICE_NAME_BLOB_MAX];
    uint32_t len = device_name_blob_encode("den", blob);
    char n[DEVICE_NAME_MAX + 1];
    Assert(!device_name_blob_decode(blob, len - 1, n));         // short
    Assert(!device_name_blob_decode(blob, 1, n));
    blob[0] = 99;
    Assert(!device_name_blob_decode(blob, len, n));             // version
    len = device_name_blob_encode("Den", blob);                 // (encode doesn't check)
    Assert(!device_name_blob_decode(blob, len, n));             // not a valid stored name
}

// --- DeviceName ---

Fact("Device Name Defaults With Nothing Stored")
{
    Fixture f;
    f.Start();
    Assert(strcmp(f.name.Get(), "controlblock-a1b2c3") == 0);
    Assert(f.name.IsDefault());
}

Fact("Device Name Loads The Stored Name")
{
    Fixture f;
    uint8_t blob[DEVICE_NAME_BLOB_MAX];
    f.storage.Preload(blob, device_name_blob_encode("den", blob));
    f.Start();
    Assert(strcmp(f.name.Get(), "den") == 0);
    Assert(!f.name.IsDefault());
}

Fact("Device Name Bad Stored Blob Is Erased")
{
    Fixture f;
    uint8_t blob[4] = { 99, 1, 'a', 0 };
    f.storage.Preload(blob, 3);
    f.Start();
    Assert(f.name.IsDefault());
    Assert(f.storage.erases == 1);
    Assert(!f.storage.present);
}

Fact("Device Name Set Saves And Tells")
{
    Fixture f;
    f.Start();
    Assert(f.Set("Den") == SettingResult::Ok);
    Assert(strcmp(f.name.Get(), "den") == 0);
    Assert(f.changes == 1);
    Assert(f.storage.present);
    char n[DEVICE_NAME_MAX + 1];
    Assert(device_name_blob_decode(f.storage.blob, f.storage.blob_len, n));
    Assert(strcmp(n, "den") == 0);

    Assert(f.Set("den") == SettingResult::Ok);                 // unchanged: no write
    Assert(f.storage.saves == 1);
    Assert(f.changes == 1);
}

Fact("Device Name Set Bad Name Changes Nothing")
{
    Fixture f;
    f.Start();
    Assert(f.Set("-x") == SettingResult::BadArg);
    Assert(f.name.IsDefault());
    Assert(f.storage.saves == 0);
    Assert(f.changes == 0);
}

Fact("Device Name Set Empty Goes Back To The Default")
{
    Fixture f;
    f.Start();
    Assert(f.Set("den") == SettingResult::Ok);
    Assert(f.Set("") == SettingResult::Ok);
    Assert(f.name.IsDefault());
    Assert(strcmp(f.name.Get(), "controlblock-a1b2c3") == 0);
    Assert(!f.storage.present);
    Assert(f.changes == 2);

    Assert(f.Set("") == SettingResult::Ok);                    // already the default: nothing
    Assert(f.storage.erases == 1);
    Assert(f.changes == 2);
}

Fact("Device Name Set The Default Name Explicitly Is Stored")
{
    Fixture f;
    f.Start();
    Assert(f.Set("controlblock-a1b2c3") == SettingResult::Ok);
    Assert(!f.name.IsDefault());                                // pinned: no longer follows the MAC
    Assert(f.storage.present);
}

Fact("Device Name Save Failure Keeps The Old Name")
{
    Fixture f;
    f.Start();
    f.storage.fail_save = true;
    Assert(f.Set("den") == SettingResult::IoError);
    Assert(f.name.IsDefault());
    Assert(f.changes == 0);
}
