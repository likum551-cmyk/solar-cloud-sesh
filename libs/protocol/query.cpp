#include "query.h"

#include <cstring>

namespace gage::proto::query {

    namespace {

        void PutHeader(Datagram& d, uint8_t kind, uint8_t version)
        {
            d.fill(0);
            std::memcpy(d.data(), kMagic, sizeof(kMagic));
            d[4] = kind;
            d[5] = version;
        }

        // Length, magic and kind; byte 5 (the query version) is checked by the caller.
        bool HasPrefix(const uint8_t* p, size_t n, uint8_t kind)
        {
            return p && n == kDatagramSize && std::memcmp(p, kMagic, sizeof(kMagic)) == 0 && p[4] == kind;
        }

        bool HasHeader(const uint8_t* p, size_t n, uint8_t kind)
        {
            return HasPrefix(p, n, kind) && p[5] == kQueryVersion;
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
        PutHeader(d, kKindRequest, kQueryVersion);
        if (r.replyVersion >= kQueryVersion2) PutU16(d, 6, kQueryVersion2);
        std::memcpy(d.data() + 8, r.nonce.data(), kNonceSize);
        return d;
    }

    Datagram Encode(const Reply& r)
    {
        Datagram d;
        const bool v2 = r.version >= kQueryVersion2;
        PutHeader(d, kKindReply, v2 ? kQueryVersion2 : kQueryVersion);
        PutU16(d, 6, r.protocol);
        std::memcpy(d.data() + 8, r.nonce.data(), kNonceSize);
        PutU16(d, 16, r.players);
        PutU16(d, 18, r.maxPlayers);
        if (v2)
        {
            d[20] = r.latestClient.major;
            d[21] = r.latestClient.minor;
            d[22] = r.latestClient.patch;
        }
        return d;
    }

    bool IsRequest(const uint8_t* p, size_t n) { return HasHeader(p, n, kKindRequest); }

    bool Decode(const uint8_t* p, size_t n, Request& out)
    {
        if (!HasHeader(p, n, kKindRequest)) return false;
        std::memcpy(out.nonce.data(), p + 8, kNonceSize);
        // Only an exact 2 asks for v2; 0, 1, 3 and garbage (a stray byte in what v1 called "zero") get the v1 reply.
        out.replyVersion = GetU16(p, 6) == kQueryVersion2 ? kQueryVersion2 : kQueryVersion;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, Reply& out)
    {
        if (!HasPrefix(p, n, kKindReply) || (p[5] != kQueryVersion && p[5] != kQueryVersion2)) return false;
        Reply r;
        r.version = p[5];
        if (r.version == kQueryVersion2)
        {
            r.latestClient.major = p[20];
            r.latestClient.minor = p[21];
            r.latestClient.patch = p[22];
        }
        r.protocol = GetU16(p, 6);
        std::memcpy(r.nonce.data(), p + 8, kNonceSize);
        r.players = GetU16(p, 16);
        r.maxPlayers = GetU16(p, 18);
        out = r;
        return true;
    }

}
