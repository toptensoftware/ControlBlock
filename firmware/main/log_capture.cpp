#include "log_capture.h"
#include <stdarg.h>
#include <stdio.h>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "port.h"

static const char *TAG = "log";

static LogRing *s_ring;
static vprintf_like_t s_prev;
static uint32_t s_busy;             // try-lock: 1 while someone uses the ring and s_msg
static char *s_msg;                 // LOG_CAPTURE_MSG_MAX formatting buffer in PSRAM, used under s_busy
alignas(LogRing) static uint8_t s_ringObj[sizeof(LogRing)];

static int capture_vprintf(const char *fmt, va_list args)
{
    if (port::AtomicCompareExchange(&s_busy, 0, 1))
    {
        va_list copy;
        va_copy(copy, args);
        int n = vsnprintf(s_msg, LOG_CAPTURE_MSG_MAX, fmt, copy);
        va_end(copy);
        if (n > 0)
        {
            size_t len = (size_t)n < LOG_CAPTURE_MSG_MAX - 1 ? (size_t)n : LOG_CAPTURE_MSG_MAX - 1;
            s_ring->Append(s_msg, len);
            if ((size_t)n > len)
                s_ring->Append("\n", 1);     // cut: still end the line (the ring drops a half character)
        }
        port::AtomicStore(&s_busy, 0);
    }
    return s_prev(fmt, args);
}

void log_capture_init(void)
{
    if (s_ring)
        return;
    void *buf = heap_caps_malloc(LOG_CAPTURE_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_msg = (char *)heap_caps_malloc(LOG_CAPTURE_MSG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !s_msg)
    {
        heap_caps_free(buf);
        heap_caps_free(s_msg);
        s_msg = nullptr;
        ESP_LOGE(TAG, "no PSRAM for the log ring: logs not captured");
        return;
    }
    s_ring = new (s_ringObj) LogRing(buf, LOG_CAPTURE_RING_SIZE);
    s_prev = esp_log_set_vprintf(capture_vprintf);
}

LogReadResult log_capture_read(bool hasSince, uint32_t since, char *out, size_t cap)
{
    if (!s_ring)
        return LogReadResult{ 0, 0, false };
    while (!port::AtomicCompareExchange(&s_busy, 0, 1))
        vTaskDelay(1);
    LogReadResult r = s_ring->Read(hasSince, since, out, cap);
    port::AtomicStore(&s_busy, 0);
    return r;
}

uint32_t log_capture_next_seq(void)
{
    return s_ring ? s_ring->NextSeq() : 0;
}
