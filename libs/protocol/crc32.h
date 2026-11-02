// CRC-32 (IEEE 802.3): the zlib / PNG checksum, reflected, polynomial 0xEDB88320. Header-only and
// portable (no tables, no Windows headers) so the server, the client engine and the tools share one
// definition. Used to key the resource disk cache and to verify a reassembled resource (protocol.h).
#pragma once

#include <cstddef>
#include <cstdint>

namespace gage::proto {

    // CRC-32 of `n` bytes at `data`, continuing from a previous result (`crc`, 0 to start), so a
    // large resource can be summed chunk by chunk. The check value of the nine ASCII bytes
    // "123456789" is 0xCBF43926.
    inline uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0)
    {
        crc = ~crc;
        for (size_t i = 0; i < n; i++)
        {
            crc ^= data[i];
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

}
