// The diagnostics snapshot from the device's getters. Hardware only.

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "clock.h"
#include "layout_boot.h"
#include "log_capture.h"
#include "model.h"
#include "net_worker.h"
#include "port.h"
#include "standalone_task.h"
#include "wifi.h"
#include "diag_collect.h"

// The last second's screen and compositor stats (main's loop writes, any task reads)
static port::SpinLock s_lock;
static uint32_t s_fps, s_fill_max_us, s_late_total, s_keys, s_key_avg_us, s_last_frames;

void diag_sample_second(screen_stats_t *st, comp_stats_t *cs)
{
    screen_get_stats(st);
    comp_get_stats(cs);
    s_lock.Enter();
    s_fps          = st->frames - s_last_frames;
    s_last_frames  = st->frames;
    s_fill_max_us  = st->max_fill_us;
    s_late_total  += st->overruns;
    s_keys         = cs->keys;
    s_key_avg_us   = cs->keys ? cs->busy_us / cs->keys : 0;
    s_lock.Exit();
}

static const char *wifi_state_name(uint8_t state)
{
    switch ((WifiState)state) {
    case WifiState::NoCredentials: return "none";
    case WifiState::Connecting:    return "connecting";
    case WifiState::Connected:     return "connected";
    case WifiState::Retrying:      return "retrying";
    }
    return "?";
}

template <size_t N>
static void copy_text(char (&dst)[N], const char *src)
{
    snprintf(dst, N, "%s", src ? src : "");
}

static void heap(uint32_t caps, uint32_t *free_, uint32_t *min_free, uint32_t *largest)
{
    *free_    = (uint32_t)heap_caps_get_free_size(caps);
    *min_free = (uint32_t)heap_caps_get_minimum_free_size(caps);
    *largest  = (uint32_t)heap_caps_get_largest_free_block(caps);
}

void diag_collect(DiagStatus *s)
{
    memset(s, 0, sizeof(*s));
    s->uptimeMs = (uint64_t)(esp_timer_get_time() / 1000);

    const esp_app_desc_t *app = esp_app_get_description();
    copy_text(s->appVersion, app->version);
    copy_text(s->idfVersion, app->idf_ver);
    snprintf(s->built, sizeof(s->built), "%s %s", app->date, app->time);
    copy_text(s->name, device_name()->Get());      // read without a lock: changes rarely, a torn read is cosmetic

    heap(MALLOC_CAP_INTERNAL, &s->internalFree, &s->internalMinFree, &s->internalLargest);
    heap(MALLOC_CAP_SPIRAM, &s->psramFree, &s->psramMinFree, &s->psramLargest);

    s_lock.Enter();
    s->fps            = s_fps;
    s->fillMaxUs      = s_fill_max_us;
    s->lateFills      = s_late_total;
    s->compBuilds     = s_keys;
    s->compBuildAvgUs = s_key_avg_us;
    s_lock.Exit();

    ImageStoreStats is;
    model_lock();
    model_store()->GetStats(&is);
    model_unlock();
    s->imageCount = is.count;
    s->imageBytes = is.used;

    WifiStatus ws = {};
    wifi_get_status(&ws);
    copy_text(s->wifiState, wifi_state_name(ws.state));
    s->wifiReason = ws.reason;
    memcpy(s->ip, ws.ip, sizeof(s->ip));
    s->rssi = ws.rssi;
    memcpy(s->mac, ws.mac, sizeof(s->mac));

    int64_t now;
    ClockSource source;
    bool known;
    clock_get(&now, &source, &known);
    s->ntpEnabled = wifi_sntp_on();
    s->ntpSynced  = source == ClockSource::Ntp;

    NetWorkerStats ns;
    net_worker_get_stats(&ns);
    s->netRequests = ns.ok + ns.failed + ns.timeouts;
    s->netErrors   = ns.failed + ns.timeouts;
    s->netDropped  = ns.dropped + ns.refused;

    StandaloneDiag sd;
    standalone_get_diag(&sd);
    s->opsBusy        = sd.busy;
    s->scriptRuns     = sd.script_runs;
    s->scriptErrors   = sd.script_errors;
    s->scriptHeapPeak = sd.script_heap_peak;

    const LayoutSummary *ls = layout_summary();
    s->layoutLoaded = ls != nullptr;
    if (ls) {
        s->activities = ls->activity_count;
        copy_text(s->activity, standalone_activity_name(sd.activity));
        snprintf(s->layoutSummary, sizeof(s->layoutSummary), "version %lu, %lu bytes, %lu devices",
                 (unsigned long)ls->version, (unsigned long)layout_blob_size(), (unsigned long)ls->device_count);
    }

    s->logNextSeq = log_capture_next_seq();
}
