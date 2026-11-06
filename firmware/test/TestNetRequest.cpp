// The small pure pieces of the network ops: the wake-on-LAN magic packet, the
// header list splitter, the url's host, and the response cap.

#include <string.h>
#include "UnitTesting.h"
#include "Core.h"
#include "net_host.h"
#include "wol_packet.h"

using namespace SimpleLib;

Fact("Wol Packet Is Six FF Then The Mac Sixteen Times")
{
    const uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01 };
    uint8_t pkt[WOL_PACKET_LEN];
    memset(pkt, 0x55, sizeof(pkt));
    WolPacket(mac, pkt);

    Assert(WOL_PACKET_LEN == 102);
    for (int i = 0; i < 6; i++)
        Assert(pkt[i] == 0xFF);
    for (int rep = 0; rep < 16; rep++)
        Assert(memcmp(pkt + 6 + rep * 6, mac, 6) == 0);
}

Fact("Net Subnet Broadcast Is The Address With The Host Bits Set")
{
    char out[NET_ADDR_LEN];
    const uint8_t ip[4] = { 192, 168, 1, 37 };

    const uint8_t m24[4] = { 255, 255, 255, 0 };
    Assert(NetSubnetBroadcast(ip, m24, out));
    Assert(strcmp(out, "192.168.1.255") == 0);

    const uint8_t m20[4] = { 255, 255, 240, 0 };
    const uint8_t ip20[4] = { 10, 1, 17, 200 };
    Assert(NetSubnetBroadcast(ip20, m20, out));
    Assert(strcmp(out, "10.1.31.255") == 0);

    // A /32 (no host bits) or a missing address or mask has no subnet broadcast
    const uint8_t m32[4] = { 255, 255, 255, 255 };
    const uint8_t none[4] = {};
    Assert(!NetSubnetBroadcast(ip, m32, out));
    Assert(!NetSubnetBroadcast(none, m24, out));
    Assert(!NetSubnetBroadcast(ip, none, out));
}

Fact("Net Header Splitter Walks Name And Value Pairs")
{
    const char *list = "Content-Type: text/plain\r\nX-Key:abc\r\nA:  spaced ";
    const char *cur  = list;
    NetHeader h;

    Assert(NetNextHeader(&cur, &h));
    Assert(h.name_len == 12 && memcmp(h.name, "Content-Type", 12) == 0);
    Assert(h.value_len == 10 && memcmp(h.value, "text/plain", 10) == 0);

    Assert(NetNextHeader(&cur, &h));                    // no space after the colon
    Assert(h.name_len == 5 && memcmp(h.name, "X-Key", 5) == 0);
    Assert(h.value_len == 3 && memcmp(h.value, "abc", 3) == 0);

    Assert(NetNextHeader(&cur, &h));                    // leading spaces skipped, trailing kept as given
    Assert(h.name_len == 1 && h.name[0] == 'A');
    Assert(h.value_len == 7 && memcmp(h.value, "spaced ", 7) == 0);

    Assert(!NetNextHeader(&cur, &h));                   // done
}

Fact("Net Header Splitter Handles Null Empty And Malformed Lists")
{
    NetHeader h;
    const char *cur = nullptr;
    Assert(!NetNextHeader(&cur, &h));                   // null list: no headers

    const char *empty = "";
    cur = empty;
    Assert(!NetNextHeader(&cur, &h));

    // A line without a colon is skipped; the next good one is found
    const char *odd = "garbage\r\nOk: 1";
    cur = odd;
    Assert(NetNextHeader(&cur, &h));
    Assert(h.name_len == 2 && memcmp(h.name, "Ok", 2) == 0);
    Assert(h.value_len == 1 && h.value[0] == '1');
    Assert(!NetNextHeader(&cur, &h));
}

Fact("Net Url Host Is The Authority After The Scheme")
{
    const char *host;
    uint32_t len;

    Assert(NetUrlHost("http://10.1.1.126:8000/get?x=1", &host, &len));     // the port is kept (Host: header form)
    Assert(len == 15 && memcmp(host, "10.1.1.126:8000", 15) == 0);

    Assert(NetUrlHost("http://braddev/", &host, &len));
    Assert(len == 7 && memcmp(host, "braddev", 7) == 0);

    Assert(NetUrlHost("http://braddev", &host, &len));                     // no path
    Assert(len == 7 && memcmp(host, "braddev", 7) == 0);

    Assert(NetUrlHost("http://braddev?q=1", &host, &len));                 // query straight after the host
    Assert(len == 7 && memcmp(host, "braddev", 7) == 0);

    Assert(!NetUrlHost("http:///path", &host, &len));                      // empty host
    Assert(!NetUrlHost("braddev/path", &host, &len));                      // no scheme
    Assert(!NetUrlHost(nullptr, &host, &len));
}

Fact("Net Response Is Clipped To The Cap And Says So")
{
    uint32_t len = 100;
    Assert(!NetClipResponse(&len));
    Assert(len == 100);

    len = NET_HTTP_MAX_RESPONSE;
    Assert(!NetClipResponse(&len));
    Assert(len == NET_HTTP_MAX_RESPONSE);

    len = NET_HTTP_MAX_RESPONSE + 1;
    Assert(NetClipResponse(&len));
    Assert(len == NET_HTTP_MAX_RESPONSE);

    Assert(NET_HTTP_MAX_RESPONSE == 16 * 1024);
    Assert(NET_HTTP_TIMEOUT_MS == 10000);
}
