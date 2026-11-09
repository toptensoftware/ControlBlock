// Diagnostics HTTP server, device layer. See diag_http.h. Hardware only: the
// JSON and the query parsing are pure and host tested (diag_status.cpp).

#include <stdio.h>
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "Core.h"
#include "diag_collect.h"
#include "diag_status.h"
#include "log_capture.h"
#include "diag_http.h"

static const char *TAG = "http";

// Low priority on core 0, below the protocol and op tasks. Two sockets (plus
// the listener) keep lwIP's 10 within reach of the net worker's.
#define DIAG_HTTP_CORE      0
#define DIAG_HTTP_PRIO      2
#define DIAG_HTTP_STACK     5120
#define DIAG_HTTP_SOCKETS   2
#define DIAG_LOG_CHUNK      1024

static httpd_handle_t s_server;

static const char PAGE[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>ControlBlock</title>"
    "<style>body{font:13px monospace;margin:8px;background:#111;color:#ddd}"
    "pre{white-space:pre-wrap;margin:0}#s{border-bottom:1px solid #444;padding-bottom:8px}</style>"
    "<pre id=s></pre><pre id=l></pre><script>"
    "let since=null;"
    "async function st(){try{let r=await fetch('/status');"
    "s.textContent=JSON.stringify(await r.json(),null,1)}catch(e){}}"
    "async function lg(){try{let r=await fetch(since===null?'/log':'/log?since='+since);"
    "let t=await r.text();since=r.headers.get('X-Log-Next');"
    "if(r.headers.get('X-Log-Dropped')&&l.textContent)t='[lines dropped]\n'+t;"
    "if(t){let b=innerHeight+scrollY>=document.body.scrollHeight-4;l.textContent+=t;"
    "if(b)scrollTo(0,document.body.scrollHeight)}}catch(e){}}"
    "st();lg();setInterval(st,5000);setInterval(lg,2000)</script>";

static esp_err_t get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t get_status(httpd_req_t *req)
{
    DiagStatus st;
    diag_collect(&st);
    SimpleLib::StringBuilder<char> sb;
    DiagStatusToJson(st, sb);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, sb.sz(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_log(httpd_req_t *req)
{
    char query[64] = "";
    bool hasSince = false;
    uint32_t since = 0;
    if (httpd_req_get_url_query_len(req) > 0)
        httpd_req_get_url_query_str(req, query, sizeof(query));
    if (!DiagParseSince(query, &hasSince, &since))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "since must be a number");

    // The ring's worth, in PSRAM for the length of the request
    char *buf = (char *)heap_caps_malloc(LOG_CAPTURE_RING_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    LogReadResult r = log_capture_read(hasSince, since, buf, LOG_CAPTURE_RING_SIZE);

    char next[12];
    snprintf(next, sizeof(next), "%lu", (unsigned long)r.next);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Log-Next", next);
    if (r.dropped)
        httpd_resp_set_hdr(req, "X-Log-Dropped", "1");

    esp_err_t err = ESP_OK;
    for (size_t at = 0; at < r.len && err == ESP_OK; at += DIAG_LOG_CHUNK) {
        size_t n = r.len - at < DIAG_LOG_CHUNK ? r.len - at : DIAG_LOG_CHUNK;
        err = httpd_resp_send_chunk(req, buf + at, n);
    }
    if (err == ESP_OK)
        err = httpd_resp_send_chunk(req, nullptr, 0);
    heap_caps_free(buf);
    return err;
}

void diag_http_start(void)
{
    if (s_server)
        return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = DIAG_HTTP_PORT;
    cfg.core_id          = DIAG_HTTP_CORE;
    cfg.task_priority    = DIAG_HTTP_PRIO;
    cfg.stack_size       = DIAG_HTTP_STACK;
    cfg.max_open_sockets = DIAG_HTTP_SOCKETS;
    cfg.max_uri_handlers = 3;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        s_server = nullptr;
        return;
    }
    static const httpd_uri_t uris[] = {
        { "/",       HTTP_GET, get_root,   nullptr },
        { "/status", HTTP_GET, get_status, nullptr },
        { "/log",    HTTP_GET, get_log,    nullptr },
    };
    for (const httpd_uri_t &u : uris)
        httpd_register_uri_handler(s_server, &u);
    ESP_LOGI(TAG, "diagnostics on port %d", DIAG_HTTP_PORT);
}

void diag_http_stop(void)
{
    if (!s_server)
        return;
    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(TAG, "diagnostics stopped");
}
