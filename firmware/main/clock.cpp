// The device clock. Device layer only (hardware): see clock.h.

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_blob.h"
#include "status.h"
#include "wifi.h"
#include "clock.h"

static const char *TAG = "clock";

static const NvsBlob      s_nvs = { "cb_clock", "cfg", "the clock settings" };
static ClockConfig        s_config;
static StaticSemaphore_t  s_mutex_buf;
static SemaphoreHandle_t  s_mutex;          // newlib's TZ globals, and s_source
static ClockSource        s_source = ClockSource::None;
static char               s_server[CLOCK_FIELD_MAX + 1];   // for the Wi-Fi task (clock_ntp_server)

static void lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

// The time zone and the server copy
static void apply(const ClockSettings &s)
{
    lock();
    setenv("TZ", s.tz, 1);
    tzset();
    memcpy(s_server, s.server, sizeof(s_server));
    unlock();
    ESP_LOGI(TAG, "time zone %s, ntp server %s", s.tz, s.server);
}

static void on_config_changed(const ClockSettings &now, void *)
{
    apply(now);
    status_clock_changed();
    wifi_ntp_server_changed();              // restarts SNTP if the server differs and Wi-Fi is up
}

void clock_init(void)
{
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    s_config.Init(nvs_blob_storage(&s_nvs));
    s_config.SetChangedCallback(on_config_changed, nullptr);
    apply(s_config.Get());
}

void clock_ntp_server(char *dst)
{
    lock();
    memcpy(dst, s_server, sizeof(s_server));
    unlock();
}

void clock_ntp_synced(const struct timeval *tv)
{
    if (!tv || !clock_time_valid((int64_t)tv->tv_sec * 1000))
        return;
    lock();
    s_source = ClockSource::Ntp;
    unlock();
    ESP_LOGI(TAG, "ntp sync");
    status_clock_changed();
}

ClockConfig *clock_config(void)
{
    return &s_config;
}

int64_t clock_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

bool clock_set_unix_ms(int64_t unix_ms, ClockSource source)
{
    if (!clock_time_valid(unix_ms))
        return false;
    struct timeval tv = {};
    tv.tv_sec  = (time_t)(unix_ms / 1000);
    tv.tv_usec = (suseconds_t)(unix_ms % 1000) * 1000;
    lock();
    settimeofday(&tv, NULL);
    s_source = source;
    unlock();
    ESP_LOGI(TAG, "time set (%s)", source == ClockSource::Ntp ? "ntp" : "host");
    status_clock_changed();
    return true;
}

void clock_get(int64_t *unix_ms, ClockSource *source, bool *known)
{
    lock();
    int64_t now = clock_now_ms();
    ClockSource src = s_source;
    unlock();
    *unix_ms = now;
    *known   = clock_time_valid(now);
    *source  = *known ? src : ClockSource::None;
}

void clock_local_text(char *dst)
{
    dst[0] = 0;
    lock();
    int64_t now = clock_now_ms();
    if (clock_time_valid(now)) {
        time_t t = (time_t)(now / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        clock_format(dst, tm.tm_hour, tm.tm_min);
    }
    unlock();
}
