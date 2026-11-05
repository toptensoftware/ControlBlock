#pragma once

// The network ops' host (docs/standalone-ops-plan.md sections 1 E and 3). The
// executors (exec_net.cpp) turn an httpGet / httpPost / udpPacket / sendWol op
// into a NetRequest and hand it to a NetHost, which queues it for the net
// worker (a task on the device, a fake in the host tests). The op task never
// blocks on the network and never resolves a name: DNS happens in the worker.
//
// Platform independent. Request strings and bodies point into the resident
// config blob (never copied, never freed), so a request is cheap to queue.

#include <stdint.h>
#include <stdio.h>

#define NET_HTTP_TIMEOUT_MS     10000           // a request that waits this long is given up on
#define NET_HTTP_MAX_RESPONSE   (16 * 1024)     // a longer response is truncated (the status is kept)
#define NET_WOL_PORT            9               // standard wake-on-LAN port
#define NET_ADDR_LEN            16              // "255.255.255.255" and the NUL
#define NET_BROADCAST_DEFAULT   "255.255.255.255"

enum class NetKind : uint8_t
{
    HttpGet,
    HttpPost,
    Udp,
    Wol,
};

struct NetRequest
{
    NetKind         kind;
    bool            wait;               // someone waits for the reply: never dropped to make room
    uint16_t        port;               // Udp, Wol
    uint32_t        generation;         // the op that made it (OpTask::generation): ties a reply to its waiter
    const char     *url;                // HttpGet, HttpPost
    const char     *headers;            // HttpGet, HttpPost: "Name: value" lines joined by CRLF, or null
    const char     *content_type;       // HttpPost
    const uint8_t  *body;               // HttpPost body, Udp payload (may be null when body_len is 0)
    uint32_t        body_len;
    const char     *host;               // Udp host (a name or dotted address); Wol: the op's broadcast, or null to use addr
    char            addr[NET_ADDR_LEN]; // Wol: the broadcast address when the op names none
    uint8_t         mac[6];             // Wol

    // Where to send: Udp's host, or Wol's broadcast. (A pointer into the request itself, so use it on the copy you keep.)
    const char *Host() const { return host ? host : addr; }
};

enum class NetResult : uint8_t
{
    Ok,                                 // `status` is the HTTP status (0 for udp / wol)
    Timeout,
    Failed,                             // connect, DNS or send failed
};

struct NetReply
{
    NetResult   result;
    uint16_t    status;
    uint32_t    body_len;               // already clipped to NET_HTTP_MAX_RESPONSE
    bool        truncated;
    const uint8_t *body;                // the (clipped) body; valid until the next waited request completes. Null for none
};

struct NetHost
{
    void *ctx;
    bool (*connected)(void *ctx);                               // Wi-Fi is up
    bool (*broadcast)(char out[NET_ADDR_LEN], void *ctx);       // the netif's subnet-directed broadcast; false if it has none
    bool (*submit)(const NetRequest &req, void *ctx);           // false: refused (queue full of waited requests)
    // A waited request's answer (scripts): once, false until it is in. Timeouts and failures are replies too.
    bool (*take_reply)(uint32_t generation, NetReply *out, void *ctx);
    // The waiter gave up (takeover): its queued request is dropped, an in-flight one's answer ignored
    void (*abandon)(uint32_t generation, void *ctx);
};

// The response cap: clip *len, say whether anything was cut.
inline bool NetClipResponse(uint32_t *len)
{
    if (*len <= NET_HTTP_MAX_RESPONSE)
        return false;
    *len = NET_HTTP_MAX_RESPONSE;
    return true;
}

// The subnet-directed broadcast for an address and netmask ("192.168.1.255"). False
// when either is missing or the mask leaves no host bits (a /32), so the caller
// falls back to NET_BROADCAST_DEFAULT.
inline bool NetSubnetBroadcast(const uint8_t ip[4], const uint8_t mask[4], char out[NET_ADDR_LEN])
{
    uint8_t b[4];
    bool any_ip = false, host_bits = false;
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(ip[i] | (uint8_t)~mask[i]);
        any_ip    = any_ip || ip[i] != 0;
        host_bits = host_bits || mask[i] != 0xFF;
    }
    if (!any_ip || !host_bits || (mask[0] | mask[1] | mask[2] | mask[3]) == 0)
        return false;
    snprintf(out, NET_ADDR_LEN, "%u.%u.%u.%u", (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3]);
    return true;
}

// Walk a header list ("Name: value" lines joined by CRLF; null or empty is none).
// Each call yields the next well-formed line (name, then value with leading
// spaces skipped) and advances *cursor; a line with no colon is skipped.
struct NetHeader
{
    const char *name;
    uint32_t    name_len;
    const char *value;
    uint32_t    value_len;
};

inline bool NetNextHeader(const char **cursor, NetHeader *h)
{
    const char *p = *cursor;
    while (p && *p) {
        const char *line = p;
        const char *end  = line;
        while (*end && *end != '\r' && *end != '\n')
            end++;
        p = end;
        while (*p == '\r' || *p == '\n')
            p++;

        const char *colon = line;
        while (colon < end && *colon != ':')
            colon++;
        if (colon == end || colon == line)
            continue;                               // no colon (or no name): skip the line

        const char *v = colon + 1;
        while (v < end && *v == ' ')
            v++;
        h->name      = line;
        h->name_len  = (uint32_t)(colon - line);
        h->value     = v;
        h->value_len = (uint32_t)(end - v);
        *cursor = p;
        return true;
    }
    *cursor = p;
    return false;
}
