#include "UnitTesting.h"
#include "Core.h"
#include "diag_status.h"
#include <string.h>

using namespace SimpleLib;

// The /status JSON

static String ToJson(const DiagStatus &s)
{
    StringBuilder<char> sb;
    DiagStatusToJson(s, sb);
    return sb.ToString();
}

Fact("DiagStatus Empty Struct Gives Zeros And Empty Strings")
{
    DiagStatus s = {};
    String j = ToJson(s);
    const char *expect =
        "{\"uptimeMs\":0,\"app\":{\"version\":\"\",\"idf\":\"\",\"built\":\"\"},"
        "\"name\":\"\","
        "\"heap\":{\"internalFree\":0,\"internalMinFree\":0,\"internalLargest\":0,\"psramFree\":0,\"psramMinFree\":0,\"psramLargest\":0},"
        "\"screen\":{\"fps\":0,\"fillMaxUs\":0,\"lateFills\":0},"
        "\"compositor\":{\"builds\":0,\"buildAvgUs\":0},"
        "\"images\":{\"count\":0,\"bytes\":0},"
        "\"wifi\":{\"state\":\"\",\"reason\":0,\"ip\":\"0.0.0.0\",\"rssi\":0,\"mac\":\"00:00:00:00:00:00\"},"
        "\"ntp\":{\"enabled\":false,\"synced\":false},"
        "\"net\":{\"requests\":0,\"errors\":0,\"dropped\":0},"
        "\"ops\":{\"busy\":false,\"scriptRuns\":0,\"scriptErrors\":0,\"scriptHeapPeak\":0},"
        "\"layout\":{\"loaded\":false,\"activities\":0,\"activity\":\"\",\"summary\":\"\"},"
        "\"log\":{\"nextSeq\":0}}";
    Assert(strcmp(j.sz(), expect) == 0);
}

Fact("DiagStatus Filled Struct")
{
    DiagStatus s = {};
    s.uptimeMs = 123456789012ull;
    strcpy(s.appVersion, "1.2");
    strcpy(s.idfVersion, "v6.0");
    strcpy(s.built, "Nov  9 2026 10:00:00");
    strcpy(s.name, "controlblock-a1b2c3");
    s.internalFree = 100; s.internalMinFree = 90; s.internalLargest = 64;
    s.psramFree = 5000000; s.psramMinFree = 4000000; s.psramLargest = 3000000;
    s.fps = 49; s.fillMaxUs = 330; s.lateFills = 2;
    s.compBuilds = 7; s.compBuildAvgUs = 900;
    s.imageCount = 12; s.imageBytes = 34567;
    strcpy(s.wifiState, "connected");
    s.wifiReason = 8;
    s.ip[0] = 192; s.ip[1] = 168; s.ip[2] = 1; s.ip[3] = 20;
    s.rssi = -45;
    for (int i = 0; i < 6; i++) s.mac[i] = (uint8_t)(0xA0 + i);
    s.ntpEnabled = true; s.ntpSynced = true;
    s.netRequests = 5; s.netErrors = 1; s.netDropped = 4;
    s.opsBusy = true; s.scriptRuns = 3; s.scriptErrors = 1; s.scriptHeapPeak = 81920;
    s.layoutLoaded = true; s.activities = 4;
    strcpy(s.activity, "tv");
    strcpy(s.layoutSummary, "4 activities");
    s.logNextSeq = 4000000000u;
    String j = ToJson(s);
    const char *expect =
        "{\"uptimeMs\":123456789012,\"app\":{\"version\":\"1.2\",\"idf\":\"v6.0\",\"built\":\"Nov  9 2026 10:00:00\"},"
        "\"name\":\"controlblock-a1b2c3\","
        "\"heap\":{\"internalFree\":100,\"internalMinFree\":90,\"internalLargest\":64,\"psramFree\":5000000,\"psramMinFree\":4000000,\"psramLargest\":3000000},"
        "\"screen\":{\"fps\":49,\"fillMaxUs\":330,\"lateFills\":2},"
        "\"compositor\":{\"builds\":7,\"buildAvgUs\":900},"
        "\"images\":{\"count\":12,\"bytes\":34567},"
        "\"wifi\":{\"state\":\"connected\",\"reason\":8,\"ip\":\"192.168.1.20\",\"rssi\":-45,\"mac\":\"a0:a1:a2:a3:a4:a5\"},"
        "\"ntp\":{\"enabled\":true,\"synced\":true},"
        "\"net\":{\"requests\":5,\"errors\":1,\"dropped\":4},"
        "\"ops\":{\"busy\":true,\"scriptRuns\":3,\"scriptErrors\":1,\"scriptHeapPeak\":81920},"
        "\"layout\":{\"loaded\":true,\"activities\":4,\"activity\":\"tv\",\"summary\":\"4 activities\"},"
        "\"log\":{\"nextSeq\":4000000000}}";
    Assert(strcmp(j.sz(), expect) == 0);
}

Fact("DiagStatus Escapes Strings")
{
    StringBuilder<char> sb;
    DiagJsonString("a\"b\\c\nd\te\x01" "f\x7f", sb);
    Assert(strcmp(sb.ToString().sz(), "\"a\\\"b\\\\c\\nd\\te\\u0001f\x7f\"") == 0);
}

Fact("DiagStatus Unterminated Field Stops At Its Size")
{
    DiagStatus s = {};
    memset(s.activity, 'x', sizeof(s.activity));     // no terminator: must not read past
    String j = ToJson(s);
    Assert(strstr(j.sz(), "\"activity\":\"xxxxxxxx") != nullptr);
    Assert(strstr(j.sz(), "\"summary\":\"\"") != nullptr);
}

// GET /log?since=N: the query string after '?'

Fact("DiagParseSince No Query Or No Key Reads All")
{
    bool has = true; uint32_t since = 7;
    Assert(DiagParseSince(nullptr, &has, &since)); Assert(!has);
    has = true;
    Assert(DiagParseSince("", &has, &since)); Assert(!has);
    has = true;
    Assert(DiagParseSince("x=1&sincex=4", &has, &since)); Assert(!has);
}

Fact("DiagParseSince Reads The Number")
{
    bool has = false; uint32_t since = 0;
    Assert(DiagParseSince("since=42", &has, &since)); Assert(has); Assert(since == 42);
    Assert(DiagParseSince("a=b&since=4294967295&c", &has, &since)); Assert(has); Assert(since == 4294967295u);
    Assert(DiagParseSince("since=0", &has, &since)); Assert(since == 0);
}

Fact("DiagParseSince Refuses Bad Numbers")
{
    bool has; uint32_t since;
    Assert(!DiagParseSince("since=", &has, &since));
    Assert(!DiagParseSince("since=-1", &has, &since));
    Assert(!DiagParseSince("since=12x", &has, &since));
    Assert(!DiagParseSince("since=4294967296", &has, &since));
    Assert(!DiagParseSince("since=99999999999999999999", &has, &since));
}
