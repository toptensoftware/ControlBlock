#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "heap_log.h"
#include "net_queue.h"
#include "net_worker.h"
#include "wifi.h"
#include "wol_packet.h"

static const char *TAG = "net";

// Core 0 with Wi-Fi and lwIP, below the op task (3) and protocol task (4) so a
// slow request never delays a key press. The stack is internal RAM: esp_http_client
// and getaddrinfo need about 6 KB between them, so 8 KB leaves headroom (measure
// with uxTaskGetStackHighWaterMark on the hardware and trim).
#define NET_TASK_CORE   0
#define NET_TASK_PRIO   2
#define NET_TASK_STACK  8192
#define NET_IDLE_MS     1000            // wake this often to expire timed-out requests when idle
#define HTTP_BUF_SIZE   1024            // esp_http_client's receive and transmit buffers
#define HEADER_NAME_MAX  48
#define HEADER_VALUE_MAX 256

namespace
{
    struct Outcome
    {
        NetResult result;
        uint16_t  status;
        uint32_t  body_len;
    };

    NetQueue                 s_queue;
    SemaphoreHandle_t        s_lock;            // guards s_queue and s_stats (the op task submits, the worker takes)
    TaskHandle_t             s_task;
    NetHost                  s_host;
    NetWorkerStats           s_stats;
    esp_http_client_handle_t s_client;          // persistent: reused while the host stays the same
    uint32_t                 s_body_len;        // bytes of response body seen by the current request
    uint8_t                 *s_resp;            // PSRAM: the body of the waited request in flight / last done (a script's http.get)
    bool                     s_capture;         // the current request is waited on: keep its body

    uint32_t now_ms()
    {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

    // ---- The NetHost the executors use (called from the op task) ----

    bool host_connected(void *)
    {
        return wifi_is_connected();
    }

    bool host_broadcast(char out[NET_ADDR_LEN], void *)
    {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info;
        if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK)
            return false;
        // lwIP keeps addresses in network order: the bytes in memory are a.b.c.d
        return NetSubnetBroadcast((const uint8_t *)&info.ip.addr, (const uint8_t *)&info.netmask.addr, out);
    }

    bool host_take_reply(uint32_t generation, NetReply *out, void *)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool ok = s_queue.TakeReply(generation, out);
        xSemaphoreGive(s_lock);
        return ok;
    }

    void host_abandon(uint32_t generation, void *)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_queue.Abandon(generation);
        xSemaphoreGive(s_lock);
    }

    bool host_submit(const NetRequest &req, void *)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool ok = s_queue.Submit(req);
        xSemaphoreGive(s_lock);
        if (ok)
            xTaskNotifyGive(s_task);
        return ok;
    }

    // ---- HTTP ----

    esp_err_t on_http_event(esp_http_client_event_t *e)
    {
        // Ops discard the response (only its size matters); a waited request keeps up to the cap
        if (e->event_id == HTTP_EVENT_ON_DATA) {
            if (s_capture && s_resp && s_body_len < NET_HTTP_MAX_RESPONSE) {
                uint32_t room = NET_HTTP_MAX_RESPONSE - s_body_len;
                memcpy(s_resp + s_body_len, e->data, (uint32_t)e->data_len < room ? (uint32_t)e->data_len : room);
            }
            s_body_len += (uint32_t)e->data_len;
        }
        return ESP_OK;
    }

    void drop_client()
    {
        if (s_client) {
            esp_http_client_cleanup(s_client);
            s_client = nullptr;
        }
    }

    // The request's headers (and the post's Content-Type) onto the client, replacing the last request's
    void set_headers(const NetRequest &r)
    {
        esp_http_client_delete_all_headers(s_client);
        if (r.kind == NetKind::HttpPost && r.content_type)
            esp_http_client_set_header(s_client, "Content-Type", r.content_type);

        const char *cursor = r.headers;
        NetHeader h;
        while (NetNextHeader(&cursor, &h)) {
            char name[HEADER_NAME_MAX], value[HEADER_VALUE_MAX];
            if (h.name_len >= sizeof(name) || h.value_len >= sizeof(value)) {
                ESP_LOGW(TAG, "header too long, skipped: %.*s", (int)h.name_len, h.name);
                continue;
            }
            memcpy(name, h.name, h.name_len);
            name[h.name_len] = 0;
            memcpy(value, h.value, h.value_len);
            value[h.value_len] = 0;
            esp_http_client_set_header(s_client, name, value);
        }
    }

    Outcome do_http(const NetRequest &r)
    {
        Outcome o = { NetResult::Failed, 0, 0 };
        // Plain http only (the bake and the blob validator refuse anything else; a last check here)
        if (!r.url || strncmp(r.url, "http://", 7) != 0) {
            ESP_LOGW(TAG, "not an http:// url");
            return o;
        }

        if (!s_client) {
            esp_http_client_config_t cfg = {};
            cfg.url               = r.url;
            cfg.timeout_ms        = NET_HTTP_TIMEOUT_MS;
            cfg.event_handler     = on_http_event;
            cfg.buffer_size       = HTTP_BUF_SIZE;
            cfg.buffer_size_tx    = HTTP_BUF_SIZE;
            cfg.keep_alive_enable = true;
            s_client = esp_http_client_init(&cfg);
            if (!s_client)
                return o;
        } else if (esp_http_client_set_url(s_client, r.url) != ESP_OK) {
            drop_client();
            return o;
        }

        bool post = r.kind == NetKind::HttpPost;
        esp_http_client_set_method(s_client, post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
        set_headers(r);
        // The post field persists between requests: always set it (cleared for a get)
        esp_http_client_set_post_field(s_client, post ? (const char *)r.body : nullptr, post ? (int)r.body_len : 0);

        s_body_len = 0;
        s_capture  = r.wait;
        esp_err_t err = esp_http_client_perform(s_client);
        if (err == ESP_OK) {
            o.result   = NetResult::Ok;
            o.status   = (uint16_t)esp_http_client_get_status_code(s_client);
            o.body_len = s_body_len;
            return o;
        }

        if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_HTTP_READ_TIMEOUT ||
            err == ESP_ERR_HTTP_CONNECTING || err == ESP_ERR_HTTP_WRITE_DATA)
            o.result = NetResult::Timeout;
        ESP_LOGW(TAG, "http %s %s: %s", post ? "POST" : "GET", r.url, esp_err_to_name(err));
        drop_client();                          // start clean next time
        return o;
    }

    // ---- UDP and wake-on-LAN ----

    // One datagram to host:port. The name is resolved here, in the worker (never in the op task).
    NetResult send_datagram(const char *host, uint16_t port, const uint8_t *data, uint32_t len, bool broadcast)
    {
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
        struct addrinfo hints = {};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo *res = nullptr;
        if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
            ESP_LOGW(TAG, "can't resolve %s", host);
            return NetResult::Failed;
        }

        NetResult out = NetResult::Failed;
        int fd = socket(res->ai_family, res->ai_socktype, 0);
        if (fd >= 0) {
            if (broadcast) {
                int on = 1;
                setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
            }
            if (sendto(fd, data, len, 0, res->ai_addr, res->ai_addrlen) == (int)len)
                out = NetResult::Ok;
            else
                ESP_LOGW(TAG, "udp send to %s:%u failed", host, (unsigned)port);
            close(fd);
        }
        freeaddrinfo(res);
        return out;
    }

    Outcome do_request(const NetRequest &r)
    {
        Outcome o = { NetResult::Failed, 0, 0 };
        switch (r.kind) {
        case NetKind::HttpGet:
        case NetKind::HttpPost:
            return do_http(r);
        case NetKind::Udp:
            o.result = send_datagram(r.host, r.port, r.body ? r.body : (const uint8_t *)"", r.body_len, false);
            break;
        case NetKind::Wol: {
            uint8_t packet[WOL_PACKET_LEN];
            WolPacket(r.mac, packet);
            o.result = send_datagram(r.Host(), r.port, packet, sizeof(packet), true);
            break;
        }
        }
        return o;
    }

    // ---- The worker ----

    void worker_task(void *)
    {
        while (1) {
            // Wake on a submit, or now and then to expire a request that timed out
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NET_IDLE_MS));

            while (1) {
                NetRequest req;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_queue.Expire(now_ms());
                bool have = s_queue.Next(now_ms(), &req);
                xSemaphoreGive(s_lock);
                if (!have)
                    break;

                Outcome o = do_request(req);

                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_queue.Complete(req.generation, o.result, o.status, o.body_len, req.wait ? s_resp : nullptr);
                if (o.result == NetResult::Ok)
                    s_stats.ok++;
                else if (o.result == NetResult::Timeout)
                    s_stats.timeouts++;
                else
                    s_stats.failed++;
                xSemaphoreGive(s_lock);

                ESP_LOGD(TAG, "kind %d: result %d, status %u, %lu body bytes", (int)req.kind, (int)o.result,
                         (unsigned)o.status, (unsigned long)o.body_len);
            }
        }
    }
}

esp_err_t net_worker_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;
    s_queue.Init();
    // A waited request's body (scripts): PSRAM, 16 KB. Without it replies carry no body.
    s_resp = (uint8_t *)heap_caps_malloc(NET_HTTP_MAX_RESPONSE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_resp)
        ESP_LOGW(TAG, "no memory for http.get replies");
    s_host = {};
    s_host.connected = host_connected;
    s_host.broadcast = host_broadcast;
    s_host.submit    = host_submit;
    s_host.take_reply = host_take_reply;
    s_host.abandon   = host_abandon;
    log_heap(TAG, "before the net worker");
    if (xTaskCreatePinnedToCore(worker_task, "net", NET_TASK_STACK, nullptr, NET_TASK_PRIO, &s_task, NET_TASK_CORE) != pdPASS)
        return ESP_FAIL;
    ESP_LOGI(TAG, "net worker started (core %d, priority %d, %d byte stack)", NET_TASK_CORE, NET_TASK_PRIO, NET_TASK_STACK);
    log_heap(TAG, "with the net worker running");
    return ESP_OK;
}

NetHost *net_worker_host(void)
{
    return &s_host;
}

void net_worker_get_stats(NetWorkerStats *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out         = s_stats;
    out->dropped = s_queue.Dropped();
    out->refused = s_queue.Refused();
    out->late    = s_queue.Late();
    xSemaphoreGive(s_lock);
}
