#include "query.h"

#include <cstring>

namespace gage::proto::query {

    namespace {

        void PutHeader(Datagram& d, uint8_t kind)
        {
            d.fill(0);
            std::memcpy(d.data(), kMagic, sizeof(kMagic));
            d[4] = kind;
            d[5] = kQueryVersion;
        }

        bool HasHeader(const uint8_t* p, size_t n, uint8_t kind)
        {
            return p && n == kDatagramSize && std::memcmp(p, kMagic, sizeof(kMagic)) == 0 && p[4] == kind &&
                   p[5] == kQueryVersion;
        }

        void PutU16(Datagram& d, size_t at, uint16_t v)
        {
            d[at] = (uint8_t)(v & 0xFF);
            d[at + 1] = (uint8_t)(v >> 8);
        }

        uint16_t GetU16(const uint8_t* p, size_t at) { return (uint16_t)(p[at] | (p[at + 1] << 8)); }

    }

    Datagram Encode(const Request& r)
    {
        Datagram d;
        PutHeader(d, kKindRequest);
        std::memcpy(d.data() + 8, r.nonce.data(), kNonceSize);
        return d;
    }

    Datagram Encode(const Reply& r)
    {
        Datagram d;
        PutHeader(d, kKindReply);
        PutU16(d, 6, r.protocol);
        std::memcpy(d.data() + 8, r.nonce.data(), kNonceSize);
        PutU16(d, 16, r.players);
        PutU16(d, 18, r.maxPlayers);
        return d;
    }

    bool IsRequest(const uint8_t* p, size_t n) { return HasHeader(p, n, kKindRequest); }

    bool Decode(const uint8_t* p, size_t n, Request& out)
    {
        if (!HasHeader(p, n, kKindRequest)) return false;
        std::memcpy(out.nonce.data(), p + 8, kNonceSize);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, Reply& out)
    {
        if (!HasHeader(p, n, kKindReply)) return false;
        Reply r;
        r.protocol = GetU16(p, 6);
        std::memcpy(r.nonce.data(), p + 8, kNonceSize);
        r.players = GetU16(p, 16);
        r.maxPlayers = GetU16(p, 18);
        out = r;
        return true;
    }

}
