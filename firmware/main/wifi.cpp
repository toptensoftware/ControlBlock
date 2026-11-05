// Wi-Fi, device layer. Spec: docs/wifi.md. Hardware only: the rules live in
// wifi_credentials.cpp and wifi_state.cpp, which the host tests cover.
//
// Everything that touches the driver runs on one task, pinned to the driver's
// core (WIFI_TASK_CORE). The driver's event
// handlers and the protocol task (a credentials change) only post to its
// queue.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"
#include "nvs.h"
#include "heap_log.h"
#include "port.h"
#include "wifi.h"

static const char *TAG = "wifi";

// The same core as the driver's own task (sdkconfig): Wi-Fi's radio interrupt
// runs on the core that started the driver. Core 0 (sdkconfig.defaults).
#if CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1
#define WIFI_TASK_CORE   1
#else
#define WIFI_TASK_CORE   0
#endif
#define WIFI_TASK_PRIO   3
#define WIFI_TASK_STACK  5120
#define WIFI_QUEUE_LEN   8

#define NVS_NAMESPACE    "cb_wifi"
#define NVS_KEY          "cred"

// The machine's reason numbers are esp_wifi's (it can't include the header: host tests)
static_assert(CB_WIFI_REASON_LEAVE == WIFI_REASON_ASSOC_LEAVE, "wifi_state.h reason");
static_assert(CB_WIFI_REASON_MIC_FAILURE == WIFI_REASON_MIC_FAILURE, "wifi_state.h reason");
static_assert(CB_WIFI_REASON_8021X_FAILED == WIFI_REASON_802_1X_AUTH_FAILED, "wifi_state.h reason");
static_assert(CB_WIFI_REASON_AUTH_FAIL == WIFI_REASON_AUTH_FAIL, "wifi_state.h reason");

// ---------------------------------------------------------------------------
// Credentials in NVS: one blob, so a power loss leaves the old or the new
// ---------------------------------------------------------------------------

static WifiConfig s_config;

static bool nvs_load(void *buf, uint32_t cap, uint32_t *len, void *)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;                       // no namespace yet: nothing stored
    size_t n = cap;
    esp_err_t err = nvs_get_blob(h, NVS_KEY, buf, &n);
    nvs_close(h);
    if (err != ESP_OK)
        return false;
    *len = (uint32_t)n;
    return true;
}

static bool nvs_save(const void *blob, uint32_t len, void *)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, blob, len);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "saving the credentials failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static bool nvs_erase(void *)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "erasing the credentials failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

WifiConfig *wifi_config(void)
{
    return &s_config;
}

// ---------------------------------------------------------------------------
// The Wi-Fi task
// ---------------------------------------------------------------------------

enum class Ev : uint8_t { Credentials, Disconnected, GotIp, LostIp };

struct Msg
{
    Ev              ev;
    uint8_t         reason;                 // Disconnected
    uint8_t         ip[4];                  // GotIp
    WifiCredentials creds;                  // Credentials: ssid_len 0 = cleared
};

static QueueHandle_t   s_queue;
static WifiMachine     s_machine;           // the Wi-Fi task only
static WifiCredentials s_creds;             // the Wi-Fi task only: what the driver connects to
static esp_netif_t    *s_netif;
static bool            s_handlers;          // event handlers registered (once, for good)
static int64_t         s_retry_at_us;       // the retry timer: when, if armed
static bool            s_retry_armed;
static wifi_state_cb_t s_state_cb;

// What WIFI_STATUS reads (any task)
static port::SpinLock  s_snap_lock;
static WifiStatus      s_snap;              // state, reason and ip; rssi and mac are read live

void wifi_set_state_cb(wifi_state_cb_t cb)
{
    s_state_cb = cb;
}

void wifi_get_status(WifiStatus *out)
{
    memset(out, 0, sizeof(*out));
    s_snap_lock.Enter();
    *out = s_snap;
    s_snap_lock.Exit();

    if (out->state == (uint8_t)WifiState::NoCredentials)
        return;                             // the driver isn't running: mac stays zero
    esp_wifi_get_mac(WIFI_IF_STA, out->mac);
    if (out->state == (uint8_t)WifiState::Connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            out->rssi = ap.rssi;
    }
}

bool wifi_is_connected(void)
{
    s_snap_lock.Enter();
    bool up = s_snap.state == (uint8_t)WifiState::Connected;
    s_snap_lock.Exit();
    return up;
}

static void post(const Msg &m)
{
    // Handlers can't wait. A full queue means the task is wedged; the next
    // event (or a retry) carries on from there.
    if (!s_queue || xQueueSend(s_queue, &m, 0) != pdTRUE)
        ESP_LOGW(TAG, "event queue full: dropped an event");
}

static void on_wifi_event(void *, esp_event_base_t, int32_t id, void *data)
{
    if (id != WIFI_EVENT_STA_DISCONNECTED)
        return;                             // STA_CONNECTED needs no action: the IP is what counts
    Msg m = {};
    m.ev     = Ev::Disconnected;
    m.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
    post(m);
}

static void on_ip_event(void *, esp_event_base_t, int32_t id, void *data)
{
    Msg m = {};
    if (id == IP_EVENT_STA_GOT_IP) {
        m.ev = Ev::GotIp;
        // lwIP keeps the address in network order: the first octet is the first byte
        memcpy(m.ip, &((ip_event_got_ip_t *)data)->ip_info.ip.addr, 4);
    } else {
        m.ev = Ev::LostIp;
    }
    post(m);
}

// Driver bring-up and teardown, in one place so a clear followed by a set works
static void driver_up(void)
{
    if (!s_handlers) {
        ESP_ERROR_CHECK(esp_netif_init());
        // Nothing else creates the default event loop
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(err);
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_event, nullptr));
        s_handlers = true;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char host[32];
    snprintf(host, sizeof(host), "controlblock-%02x%02x%02x", mac[3], mac[4], mac[5]);
    esp_netif_set_hostname(s_netif, host);

    log_heap(TAG, "before the Wi-Fi driver starts");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // One copy of the credentials (ours), and no flash write on every set_config
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // The board is USB powered: modem sleep would only add tens of ms to each request
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "driver up as %s", host);
    log_heap(TAG, "with the Wi-Fi driver up");
}

static void driver_down(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = nullptr;
    }
    ESP_LOGI(TAG, "driver down");
    log_heap(TAG, "with the Wi-Fi driver down");
}

static void driver_connect(void)
{
    wifi_config_t cfg = {};
    memcpy(cfg.sta.ssid, s_creds.ssid, s_creds.ssid_len);      // zero padded; 32 bytes fill the field
    memcpy(cfg.sta.password, s_creds.pass, s_creds.pass_len);
    // A WPA2/WPA3 mixed AP works, and a downgrade to WEP/WPA is refused
    cfg.sta.threshold.authmode = s_creds.pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK)
        err = esp_wifi_connect();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
    memset(&cfg, 0, sizeof(cfg));           // the password doesn't linger on the stack
    ESP_LOGI(TAG, "connecting to \"%s\" (%u byte password)", s_creds.ssid, (unsigned)s_creds.pass_len);
}

// ---- Link diagnostics ----
// Gateway, mask, RSSI and a short ping of the gateway, to tell a weak or slow
// link from a problem further up. Logged once per connect; with WIFI_DIAG
// (main/CMakeLists.txt, -DCB_WIFI_DIAG=ON) also every WIFI_DIAG_PERIOD_MS.

#ifndef WIFI_DIAG
#define WIFI_DIAG 0
#endif
#define WIFI_DIAG_PERIOD_MS  5000
#define GATEWAY_PINGS_AT_CONNECT 4

// 1 while a ping session runs: one at a time, so overlapping sessions don't skew
// the times. Set by the Wi-Fi task, cleared on the esp_ping task.
static uint32_t s_ping_busy;

// Profile types are esp_ping's: SEQNO is 16 bits, TTL 8, the rest 32
static void on_ping_success(esp_ping_handle_t h, void *)
{
    uint16_t seq = 0;
    uint8_t  ttl = 0;
    uint32_t ms  = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    ESP_LOGI(TAG, "gateway ping #%u: %u ms (ttl %u)", (unsigned)seq, (unsigned)ms, (unsigned)ttl);
}

static void on_ping_timeout(esp_ping_handle_t h, void *)
{
    uint16_t seq = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    ESP_LOGW(TAG, "gateway ping #%u: timed out", (unsigned)seq);
}

static void on_ping_end(esp_ping_handle_t h, void *)
{
    uint32_t sent = 0, received = 0, total_ms = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(h, ESP_PING_PROF_DURATION, &total_ms, sizeof(total_ms));
    ESP_LOGI(TAG, "gateway ping: %u sent, %u received, %u ms in all",
             (unsigned)sent, (unsigned)received, (unsigned)total_ms);
    esp_ping_delete_session(h);
    port::AtomicStore(&s_ping_busy, 0);
}

// Log the link and ping the gateway `pings` times (0: just the link line)
static void log_link(int pings)
{
    esp_netif_ip_info_t info;
    wifi_ap_record_t    ap;
    if (!s_netif || esp_netif_get_ip_info(s_netif, &info) != ESP_OK)
        return;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        ESP_LOGI(TAG, "link: gateway " IPSTR ", mask " IPSTR ", rssi %d dBm, channel %u",
                 IP2STR(&info.gw), IP2STR(&info.netmask), (int)ap.rssi, (unsigned)ap.primary);
    else
        ESP_LOGW(TAG, "link: gateway " IPSTR ", mask " IPSTR ", no AP info", IP2STR(&info.gw), IP2STR(&info.netmask));
    if (pings <= 0 || info.gw.addr == 0)
        return;
    if (!port::AtomicCompareExchange(&s_ping_busy, 0, 1))
        return;                             // the last session is still running: skip this one

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count       = (uint32_t)pings;
    cfg.interval_ms = 500;
    cfg.timeout_ms  = 2000;
    ip_addr_set_ip4_u32(&cfg.target_addr, info.gw.addr);
    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = on_ping_success;
    cbs.on_ping_timeout = on_ping_timeout;
    cbs.on_ping_end     = on_ping_end;
    esp_ping_handle_t h = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK || esp_ping_start(h) != ESP_OK) {
        ESP_LOGW(TAG, "couldn't start the gateway ping");
        if (h)
            esp_ping_delete_session(h);
        port::AtomicStore(&s_ping_busy, 0);
    }
}

static void publish(void)
{
    s_snap_lock.Enter();
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state  = (uint8_t)s_machine.GetState();
    s_snap.reason = s_machine.Reason();
    memcpy(s_snap.ip, s_machine.Ip(), 4);
    s_snap_lock.Exit();
}

static void apply(const WifiAction &a)
{
    if (a.cancel_retry)
        s_retry_armed = false;
    if (a.retry_ms >= 0) {
        s_retry_at_us = esp_timer_get_time() + (int64_t)a.retry_ms * 1000;
        s_retry_armed = true;
        ESP_LOGI(TAG, "retrying in %d ms (disconnect reason %u)", (int)a.retry_ms, (unsigned)s_machine.Reason());
    }
    if (a.stop)
        driver_down();
    if (a.disconnect)
        esp_wifi_disconnect();
    if (a.start)
        driver_up();
    if (a.connect)
        driver_connect();

    publish();
    if (a.changed) {
        ESP_LOGI(TAG, "state %d", (int)s_machine.GetState());
        if (s_machine.GetState() == WifiState::Connected) {
            const uint8_t *ip = s_machine.Ip();
            ESP_LOGI(TAG, "got IP %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            log_heap(TAG, "with Wi-Fi connected");
            log_link(GATEWAY_PINGS_AT_CONNECT);
        }
        if (s_state_cb)
            s_state_cb(s_machine.GetState(), s_machine.Ip());
    }
}

static void handle(const Msg &m)
{
    switch (m.ev) {
    case Ev::Credentials:
        s_creds = m.creds;
        apply(s_machine.OnCredentials(s_creds.ssid_len != 0));
        break;
    case Ev::Disconnected:
        apply(s_machine.OnDisconnected(m.reason));
        break;
    case Ev::GotIp:
        apply(s_machine.OnGotIp(m.ip));
        break;
    case Ev::LostIp:
        apply(s_machine.OnLostIp());
        break;
    }
}

static void wifi_task(void *)
{
#if WIFI_DIAG
    int64_t diag_at_us = 0;
#endif
    while (1) {
        TickType_t wait = portMAX_DELAY;
        if (s_retry_armed) {
            int64_t left_us = s_retry_at_us - esp_timer_get_time();
            wait = left_us > 0 ? pdMS_TO_TICKS((left_us + 999) / 1000) : 0;
        }
#if WIFI_DIAG
        if (s_machine.GetState() == WifiState::Connected && wait > pdMS_TO_TICKS(WIFI_DIAG_PERIOD_MS))
            wait = pdMS_TO_TICKS(WIFI_DIAG_PERIOD_MS);
#endif

        Msg m;
        if (xQueueReceive(s_queue, &m, wait) == pdTRUE) {
            handle(m);
        } else if (s_retry_armed && esp_timer_get_time() >= s_retry_at_us) {
            s_retry_armed = false;
            apply(s_machine.OnRetryTimer());
        }
#if WIFI_DIAG
        if (s_machine.GetState() == WifiState::Connected && esp_timer_get_time() >= diag_at_us) {
            diag_at_us = esp_timer_get_time() + (int64_t)WIFI_DIAG_PERIOD_MS * 1000;
            log_link(1);
        }
#endif
    }
}

// A set or clear from the protocol task: hand it to the Wi-Fi task
static void on_credentials_changed(const WifiCredentials &now, void *)
{
    Msg m = {};
    m.ev    = Ev::Credentials;
    m.creds = now;
    post(m);
}

void wifi_init(void)
{
    // The queue exists from here on: a host can set credentials before wifi_start()
    s_queue = xQueueCreate(WIFI_QUEUE_LEN, sizeof(Msg));
    if (!s_queue)
        ESP_LOGE(TAG, "no memory for the event queue");
    WifiStorage st = {};
    st.load  = nvs_load;
    st.save  = nvs_save;
    st.erase = nvs_erase;
    s_config.Init(st);
    s_config.SetChangedCallback(on_credentials_changed, nullptr);
    if (s_config.Stored())
        ESP_LOGI(TAG, "credentials stored for \"%s\"", s_config.Get().ssid);
}

esp_err_t wifi_start(void)
{
    if (!s_queue)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(wifi_task, "wifi", WIFI_TASK_STACK, nullptr, WIFI_TASK_PRIO,
                                nullptr, WIFI_TASK_CORE) != pdPASS)
        return ESP_FAIL;

    // Stored credentials: connect on boot. With none, Wi-Fi isn't started at all.
    if (s_config.Stored()) {
        Msg m = {};
        m.ev    = Ev::Credentials;
        m.creds = s_config.Get();
        post(m);
    }
    return ESP_OK;
}
