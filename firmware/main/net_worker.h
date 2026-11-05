#pragma once

// The net worker, device layer (docs/standalone-ops-plan.md section 2, step A5):
// one task that takes requests from the NetQueue (net_queue.h) and does them,
// so the op task never blocks on the network or on DNS. HTTP goes through one
// persistent esp_http_client (http:// only; the connection is reused while the
// host stays the same), UDP and wake-on-LAN through lwIP sockets. The rules
// (queueing, drops, the response cap, the header splitter) are in net_queue.cpp
// and net_host.h and are host tested; this file is checked on the hardware only.
//
// The task runs on core 0 next to Wi-Fi and lwIP, below the op and protocol
// tasks (net_worker.cpp has the numbers).

#include <stdint.h>
#include "esp_err.h"
#include "net_host.h"

// Create the queue and the task. Call before standalone_init().
esp_err_t net_worker_init(void);

// The NetHost the network executors submit to (valid after net_worker_init)
NetHost *net_worker_host(void);

struct NetWorkerStats
{
    uint32_t ok;            // requests that got an answer (an HTTP error status counts) or were sent
    uint32_t failed;        // connect, DNS, send or protocol failures
    uint32_t timeouts;
    uint32_t dropped;       // fire-and-forget requests dropped to make room in the queue
    uint32_t refused;
    uint32_t late;          // replies that came after their waiter had gone
};
void net_worker_get_stats(NetWorkerStats *out);
