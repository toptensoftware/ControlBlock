#include "diag_status.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

using namespace SimpleLib;

void DiagJsonString(const char *s, StringBuilder<char> &sb, int len)
{
    sb.Append('"');
    for (int i = 0; (len < 0 || i < len) && s[i]; i++)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
            case '"':  sb.Append("\\\""); break;
            case '\\': sb.Append("\\\\"); break;
            case '\n': sb.Append("\\n"); break;
            case '\r': sb.Append("\\r"); break;
            case '\t': sb.Append("\\t"); break;
            default:
                if (c < 0x20)
                {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    sb.Append(esc);
                }
                else
                    sb.Append((char)c);
        }
    }
    sb.Append('"');
}

// Writers for one "key":value, with the commas between members
// (StringBuilder::sz() appends a nul, so the builder is never read back here)
namespace
{
    struct Json
    {
        StringBuilder<char> &sb;
        bool first = true;

        void Key(const char *k)
        {
            if (!first)
                sb.Append(',');
            first = false;
            if (k) { sb.Append('"'); sb.Append(k); sb.Append("\":"); }
        }
        void Open(const char *k)       { Key(k); sb.Append('{'); first = true; }
        void Close()                   { sb.Append('}'); first = false; }
        void Raw(const char *k, const char *v) { Key(k); sb.Append(v); }
        void U(const char *k, uint64_t v)
        {
            char t[24];
            snprintf(t, sizeof(t), "%" PRIu64, v);
            Raw(k, t);
        }
        void I(const char *k, int32_t v)
        {
            char t[16];
            snprintf(t, sizeof(t), "%" PRId32, v);
            Raw(k, t);
        }
        void B(const char *k, bool v)  { Raw(k, v ? "true" : "false"); }
        template <size_t N>
        void S(const char *k, const char (&v)[N]) { Key(k); DiagJsonString(v, sb, (int)N); }
        void Str(const char *k, const char *v)    { Key(k); DiagJsonString(v, sb); }
    };
}

void DiagStatusToJson(const DiagStatus &s, StringBuilder<char> &sb)
{
    Json j{ sb };
    j.Open(nullptr);
    j.U("uptimeMs", s.uptimeMs);
    j.Open("app");
        j.S("version", s.appVersion);
        j.S("idf", s.idfVersion);
        j.S("built", s.built);
    j.Close();
    j.S("name", s.name);
    j.Open("heap");
        j.U("internalFree", s.internalFree);
        j.U("internalMinFree", s.internalMinFree);
        j.U("internalLargest", s.internalLargest);
        j.U("psramFree", s.psramFree);
        j.U("psramMinFree", s.psramMinFree);
        j.U("psramLargest", s.psramLargest);
    j.Close();
    j.Open("screen");
        j.U("fps", s.fps);
        j.U("fillMaxUs", s.fillMaxUs);
        j.U("lateFills", s.lateFills);
    j.Close();
    j.Open("compositor");
        j.U("builds", s.compBuilds);
        j.U("buildAvgUs", s.compBuildAvgUs);
    j.Close();
    j.Open("images");
        j.U("count", s.imageCount);
        j.U("bytes", s.imageBytes);
    j.Close();
    j.Open("wifi");
        j.S("state", s.wifiState);
        j.U("reason", s.wifiReason);
        char t[24];
        snprintf(t, sizeof(t), "%u.%u.%u.%u", s.ip[0], s.ip[1], s.ip[2], s.ip[3]);
        j.Str("ip", t);
        j.I("rssi", s.rssi);
        snprintf(t, sizeof(t), "%02x:%02x:%02x:%02x:%02x:%02x", s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);
        j.Str("mac", t);
    j.Close();
    j.Open("ntp");
        j.B("enabled", s.ntpEnabled);
        j.B("synced", s.ntpSynced);
    j.Close();
    j.Open("net");
        j.U("requests", s.netRequests);
        j.U("errors", s.netErrors);
        j.U("dropped", s.netDropped);
    j.Close();
    j.Open("ops");
        j.B("busy", s.opsBusy);
        j.U("scriptRuns", s.scriptRuns);
        j.U("scriptErrors", s.scriptErrors);
        j.U("scriptHeapPeak", s.scriptHeapPeak);
    j.Close();
    j.Open("layout");
        j.B("loaded", s.layoutLoaded);
        j.U("activities", s.activities);
        j.S("activity", s.activity);
        j.S("summary", s.layoutSummary);
    j.Close();
    j.Open("log");
        j.U("nextSeq", s.logNextSeq);
    j.Close();
    j.Close();
}

bool DiagParseSince(const char *query, bool *hasSince, uint32_t *since)
{
    *hasSince = false;
    for (const char *p = query; p && *p; ) {
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= 6 && memcmp(p, "since=", 6) == 0) {
            const char *v = p + 6;
            size_t n = len - 6;
            if (n == 0 || n > 10)
                return false;
            uint64_t val = 0;
            for (size_t i = 0; i < n; i++) {
                if (v[i] < '0' || v[i] > '9')
                    return false;
                val = val * 10 + (uint64_t)(v[i] - '0');
            }
            if (val > 0xFFFFFFFFull)
                return false;
            *hasSince = true;
            *since = (uint32_t)val;
            return true;
        }
        p = end ? end + 1 : nullptr;
    }
    return true;
}
