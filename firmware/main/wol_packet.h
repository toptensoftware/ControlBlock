#pragma once

// The wake-on-LAN magic packet: six 0xFF bytes, then the target MAC sixteen times.

#include <stdint.h>
#include <string.h>

#define WOL_PACKET_LEN  102

inline void WolPacket(const uint8_t mac[6], uint8_t out[WOL_PACKET_LEN])
{
    memset(out, 0xFF, 6);
    for (int i = 0; i < 16; i++)
        memcpy(out + 6 + i * 6, mac, 6);
}
