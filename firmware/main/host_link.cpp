// Protocol task: reads the host's byte stream from USB, runs it through
// Protocol, and sends responses and events back.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "waveshare_rgb_lcd_port.h"
#include "compositor.h"
#include "config_flash.h"
#include "model.h"
#include "protocol.h"
#include "status.h"
#include "status_bar.h"
#include "swipe.h"
#include "usb.h"
#include "host_link.h"

static const char *TAG = "host";

// Core 0 keeps host traffic off the core that draws (docs/page-compositor.md);
// below the TinyUSB task (5)
#define PROTOCOL_TASK_CORE   0
#define PROTOCOL_TASK_PRIO   4
#define PROTOCOL_TASK_STACK  6144

#define IDLE_POLL_MS         20      // while images wait for a sweep

// After a config commit's response: let USB send it before restarting
#define RESTART_DELAY_MS     200

static_assert(SWIPE_CAUSE_USER == PAGE_CAUSE_USER && SWIPE_CAUSE_HOST == PAGE_CAUSE_HOST,
              "swipe causes are PAGE_CHANGED causes");

static_assert(STATUS_TEXT_MAX == STATUS_BAR_TEXT_MAX, "protocol text limit is the status bar's");

static Protocol     s_proto;
static ConfigStore  s_config;
static TaskHandle_t s_task;
static char         s_serial[16];

// --- ProtocolHost ---

static void   ph_write(const void *data, size_t len, void *) { usb_vendor_write(data, len); }
static size_t ph_write_space(void *)                         { return usb_vendor_write_space(); }
static void   ph_flush(void *)                               { usb_vendor_flush(); }
static void   ph_lock(void *)                                { model_lock(); }
static void   ph_unlock(void *)                              { model_unlock(); }
static void   ph_reset_compositor(void *)                    { comp_reset(); }
static void   ph_set_page_count(int count, void *)           { swipe_set_page_count(count); }
static void   ph_set_page(int page, bool animate, void *)    { swipe_go_to_page(page, animate); }
static int    ph_current_page(void *)                        { return swipe_current_page(); }
// The backlight state lives here (GET_INFO reads it): boots on (main.cpp).
// It drives the light and the touch wake gate together.
static volatile bool s_backlight = true;

static void   ph_set_backlight(bool on, void *)
{
    if (on == s_backlight)
        return;
    s_backlight = on;
    display_enable_backlight(on);
    swipe_set_enabled(on);
}
static bool   ph_backlight_on(void *)                        { return s_backlight; }

// A config upload is under way (true) or over (false). Flash erases and
// writes stall the display, so it is dark meanwhile; afterwards it goes back
// to what the host set. A committed upload leaves it dark: a restart follows.
static void   cf_busy(bool busy, void *)
{
    display_enable_backlight(busy ? false : s_backlight);
}
static void   ph_restart(void *)
{
    ESP_LOGI(TAG, "config saved: restarting");
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}
static void   ph_set_status_text(int field, const char *text, void *)
{
    // Safe from this task: the status task does the drawing (internal RAM)
    if (field == STATUS_TEXT_LEFT)
        status_set_left(text);
    else
        status_set_right(text);
}

// --- Wakes (USB and swipe tasks) ---

static void wake(void)
{
    if (s_task)
        xTaskNotifyGive(s_task);
}

static void on_page_changed(int page, int cause)
{
    s_proto.PostPageChanged(page, cause);
    wake();
}

static void on_wake(uint32_t time_ms)
{
    s_proto.PostWake(time_ms);
    wake();
}

static void on_key(int page, int key, int action, uint32_t time_ms)
{
    s_proto.PostKey(page, key, action, time_ms);
    wake();
}

static void protocol_task(void *)
{
    static uint8_t buf[512];
    uint32_t epoch = 0;
    bool     waiting = false;       // images waiting for a sweep

    while (1) {
        ulTaskNotifyTake(pdTRUE, waiting ? pdMS_TO_TICKS(IDLE_POLL_MS) : portMAX_DELAY);

        // Everything that's arrived. Bytes from a new stream epoch come
        // after a RESET_STREAM: reset the parser before them.
        while (1) {
            uint32_t e;
            size_t n = usb_vendor_read(buf, sizeof(buf), &e);
            if (e != epoch) {
                epoch = e;
                s_proto.ResetStream();
            }
            if (!n)
                break;
            s_proto.Receive(buf, n);
        }

        s_proto.SendEvents();
        waiting = s_proto.Idle();
    }
}

esp_err_t host_link_init(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ProtocolHost h = {};
    h.write            = ph_write;
    h.write_space      = ph_write_space;
    h.flush            = ph_flush;
    h.lock             = ph_lock;
    h.unlock           = ph_unlock;
    h.reset_compositor = ph_reset_compositor;
    h.set_page_count   = ph_set_page_count;
    h.set_page         = ph_set_page;
    h.current_page     = ph_current_page;
    h.set_backlight    = ph_set_backlight;
    h.backlight_on     = ph_backlight_on;
    h.set_status_text  = ph_set_status_text;
    h.restart          = ph_restart;
    h.fw_version       = esp_app_get_description()->version;
    h.serial           = s_serial;
    ConfigFlash cf = {};
    config_flash_open(&cf);
    cf.busy = cf_busy;
    s_config.Init(cf);
    s_proto.Init(model_store(), model_layout(), &s_config, h);

    if (xTaskCreatePinnedToCore(protocol_task, "protocol", PROTOCOL_TASK_STACK, nullptr,
                                PROTOCOL_TASK_PRIO, &s_task, PROTOCOL_TASK_CORE) != pdPASS)
        return ESP_FAIL;
    swipe_set_page_changed_cb(on_page_changed);
    swipe_set_key_cb(on_key);
    swipe_set_wake_cb(on_wake);

    usb_callbacks_t cb = {};
    cb.rx           = wake;
    cb.reset_stream = wake;
    esp_err_t err = usb_init(s_serial, &cb);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "ready");
    return err;
}

bool host_link_seen(void)
{
    return s_proto.HostSeen();
}
