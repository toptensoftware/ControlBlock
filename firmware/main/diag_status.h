#pragma once

// The diagnostics status snapshot (GET /status) and its JSON. Pure and host
// tested; diag_collect.cpp fills it on the device. Plain values only: strings
// are fixed arrays (read up to their size even without a terminator).

#include <stdint.h>
#include "Core.h"

struct DiagStatus
{
    uint64_t uptimeMs;
    char     appVersion[32];
    char     idfVersion[32];
    char     built[32];             // build date and time
    char     name[33];              // device name (mDNS/DHCP hostname)

    uint32_t internalFree, internalMinFree, internalLargest;
    uint32_t psramFree, psramMinFree, psramLargest;

    uint32_t fps, fillMaxUs, lateFills;    // the last second; late fills since boot
    // keys drawn in the last second and their average time
    uint32_t compBuilds, compBuildAvgUs;
    uint32_t imageCount, imageBytes;

    char     wifiState[16];
    uint32_t wifiReason;
    uint8_t  ip[4];
    int32_t  rssi;
    uint8_t  mac[6];

    bool     ntpEnabled, ntpSynced;

    uint32_t netRequests, netErrors, netDropped;

    bool     opsBusy;
    uint32_t scriptRuns, scriptErrors, scriptHeapPeak;

    bool     layoutLoaded;
    uint32_t activities;
    char     activity[32];          // selected activity's name, "" for none
    char     layoutSummary[64];

    uint32_t logNextSeq;
};

// A JSON string literal (quotes, escapes), up to len characters or the first nul
void DiagJsonString(const char *s, SimpleLib::StringBuilder<char> &sb, int len = -1);

// The whole snapshot as one JSON object (no whitespace)
void DiagStatusToJson(const DiagStatus &s, SimpleLib::StringBuilder<char> &sb);

// The `since` of GET /log?since=N from the query string (after '?', may be
// null): hasSince false when absent. False if the value isn't a decimal u32.
bool DiagParseSince(const char *query, bool *hasSince, uint32_t *since);
