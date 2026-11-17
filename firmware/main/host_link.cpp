// Protocol task: reads the host's byte stream from USB, runs it through
// Protocol, and sends responses and events back.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "waveshare_rgb_lcd_port.h"
#include "clock.h"
#include "display_settings.h"
#include "compositor.h"
#include "layout_boot.h"
#include "layout_flash.h"
#include "model.h"
#include "protocol.h"
#include "status.h"
#include "standalone_task.h"
#include "status_bar.h"
#include "swipe.h"
#include "usb.h"
#include "wifi.h"
#include "host_link.h"

static const char *TAG = "host";

// Core 0 keeps host traffic off the core that draws (docs/page-compositor.md);
// below the TinyUSB task (5)
#define PROTOCOL_TASK_CORE   0
#define PROTOCOL_TASK_PRIO   4
#define PROTOCOL_TASK_STACK  6144

#define IDLE_POLL_MS         20      // while images wait for a sweep

// After a layout commit's response: let USB send it before restarting
#define RESTART_DELAY_MS     200

static_assert(SWIPE_CAUSE_USER == PAGE_CAUSE_USER && SWIPE_CAUSE_HOST == PAGE_CAUSE_HOST,
              "swipe causes are PAGE_CHANGED causes");

static_assert(STATUS_TEXT_MAX == STATUS_BAR_TEXT_MAX, "protocol text limit is the status bar's");

static Protocol     s_proto;
static LayoutStore  s_layout_store;
static TaskHandle_t s_task;
static char         s_serial[16];
static LinkCounters s_link;          // the receive path, for the stats line (host_link_stats)

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

// A layout upload is under way (true) or over (false). Flash erases and
// writes stall the display, so it is dark meanwhile; afterwards it goes back
// to what the host set. A committed upload leaves it dark: a restart follows.
static void   cf_busy(bool busy, void *)
{
    display_enable_backlight(busy ? false : s_backlight);
}
static void   ph_wifi_status(WifiStatus *out, void *)          { wifi_get_status(out); }
static void   ph_set_time(int64_t unix_ms, void *)             { clock_set_unix_ms(unix_ms, ClockSource::Host); }
static void   ph_get_time(int64_t *unix_ms, ClockSource *source, bool *known, void *)
{
    clock_get(unix_ms, source, known);
}
static void   ph_restart(void *)
{
    ESP_LOGI(TAG, "layout saved: restarting");
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

// TinyUSB task: a bulk OUT transfer is in the RX FIFO
static void on_rx(uint32_t bytes)
{
    s_link.Transfer(bytes);
    wake();
}

// The Wi-Fi task: the connection changed
static void on_wifi_state(WifiState state, const uint8_t ip[4])
{
    s_proto.PostWifiState(state, ip);
    status_set_wifi(state);             // the status bar's Wi-Fi icon (before wifi_start: boots showing not connected)
    wake();
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

// The switch point for key events: hosted, they go to the host; standalone,
// through the gestures and bindings to the op queue (standalone_task.h).
// Both go by the key's id: the KEY event carries it (and the page touched on),
// and standalone it names the control "touch[id]".
static void on_key(int page, uint16_t id, int action, uint32_t time_ms)
{
    if (s_proto.Hosted()) {
        s_proto.PostKey(page, id, action, time_ms);
        wake();
        return;
    }
    standalone_key(id, action, time_ms);
}

// TinyUSB task: the host went away. The protocol task does the release.
static void on_disconnected(void)
{
    s_proto.PostDisconnect();
    wake();
}

// Protocol task, after the model was rebuilt
static void ph_mode_changed(bool hosted, void *)
{
    standalone_set_hosted(hosted);      // drops held keys and queued ops; release re-arms
    status_set_hosted(hosted);          // hosted: the Wi-Fi icon only (busy/error cleared)
    ESP_LOGI(TAG, "%s", hosted ? "host took control" : "standalone");
}

static void protocol_task(void *)
{
    // One receive transfer per read (the endpoint buffer, CONFIG_TINYUSB_VENDOR_EPSIZE:
    // sdkconfig.defaults)
    static uint8_t buf[CONFIG_TINYUSB_VENDOR_EPSIZE];
    uint32_t epoch = 0;
    bool     waiting = false;       // images waiting for a sweep

    while (1) {
        // Wakes at least once per heartbeat interval, so an idle link still
        // carries a HEARTBEAT (the host's reads never sit idle until they time out)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waiting ? IDLE_POLL_MS : PROTO_HEARTBEAT_MS));
        s_link.Wake();

        // Everything that's arrived. Bytes from a new stream epoch come
        // after a RESET_STREAM: reset the parser before them.
        while (1) {
            uint32_t e, fifo;
            size_t n = usb_vendor_read(buf, sizeof(buf), &e, &fifo);
            if (e != epoch) {
                epoch = e;
                s_proto.ResetStream();
            }
            if (!n)
                break;
            s_link.Read(fifo);
            uint32_t t0 = port::Cycles();
            s_proto.Receive(buf, n);
            s_link.ReceiveCycles(port::Cycles() - t0);
        }

        s_proto.ServiceDisconnect();       // after the bytes that came before it
        s_proto.SendEvents();
        s_proto.SendHeartbeat((uint32_t)(esp_timer_get_time() / 1000));
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
    h.mode_changed     = ph_mode_changed;
    h.restart          = ph_restart;
    h.wifi_status      = ph_wifi_status;
    h.set_time         = ph_set_time;
    h.get_time         = ph_get_time;
    h.fw_version       = esp_app_get_description()->version;
    h.serial           = s_serial;
    LayoutFlash cf = {};
    layout_flash_open(&cf);
    cf.busy = cf_busy;
    s_layout_store.Init(cf);
    s_proto.Init(model_store(), model_layout(), &s_layout_store, wifi_config(), clock_config(), device_name(), display_config(), h);
    s_proto.SetHome(layout_summary(), layout_blob());

    if (xTaskCreatePinnedToCore(protocol_task, "protocol", PROTOCOL_TASK_STACK, nullptr,
                                PROTOCOL_TASK_PRIO, &s_task, PROTOCOL_TASK_CORE) != pdPASS)
        return ESP_FAIL;
    swipe_set_page_changed_cb(on_page_changed);
    if (standalone_init(layout_blob()) != ESP_OK)
        return ESP_FAIL;
    swipe_set_key_cb(on_key);
    swipe_set_tick_cb(standalone_tick);
    swipe_set_wake_cb(on_wake);
    wifi_set_state_cb(on_wifi_state);

    usb_callbacks_t cb = {};
    cb.rx           = on_rx;
    cb.reset_stream = wake;
    cb.disconnected = on_disconnected;
    esp_err_t err = usb_init(s_serial, &cb);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "ready");
    return err;
}

bool host_link_seen(void)
{
    return s_proto.HostSeen();
}

LinkSample host_link_stats(void)
{
    return s_link.Take();
}
