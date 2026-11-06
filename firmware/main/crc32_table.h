#pragma once

// CRC-32 as zlib's crc32(): polynomial 0xEDB88320, initial and final XOR
// 0xFFFFFFFF. Chain pieces by passing the previous result:
//     crc = crc32(0, a, na); crc = crc32(crc, b, nb);
// Pure, so the host tests and the device share it. The host library's
// crc32() (host/src/protocol.js) must agree.

#include <stddef.h>
#include <stdint.h>

namespace crc32_detail
{
    struct Table
    {
        uint32_t v[256];
        constexpr Table() : v()
        {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++)
                    c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                v[i] = c;
            }
        }
    };
    inline constexpr Table table;
}

inline uint32_t crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--)
        crc = crc32_detail::table.v[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
