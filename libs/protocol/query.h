// GAGE UDP query: the 24-byte datagram pair a master server or the launcher sends
// to a game server's UDP port to learn whether a GAGE server is there, its live
// player counts and (from the round trip) its ping. apps/master/PROTOCOL.md is the
// authority for the layout; this file is the portable codec (no sockets, no
// platform headers) so the server, the launcher and tools share one definition.
//
// Request (to the game port), 24 bytes, little-endian:
//   0  magic "GAGE"   4  kind 0x01   5  query version 1   6  u16 reply version wanted (0/1: v1, 2: v2)
//   8  nonce[8]       16 zero[8]
// Reply v1 (to the request's source address and port), 24 bytes:
//   0  magic "GAGE"   4  kind 0x81   5  query version 1   6  u16 game protocol version
//   8  nonce[8] (echoed unchanged)   16 u16 players   18 u16 maxPlayers   20 zero[4]
// Reply v2 (only to a request that asked for it), also 24 bytes: the same, except
//   5  query version 2   20 u8 latest client major   21 minor   22 patch   23 zero
//
// Query format v2 (2026-11-02, "update ping"): the server also reports the newest
// client version it was released with, so a launcher learns of a same-protocol
// release from the query it already sends every 5 s. The REQUEST stays byte 5 = 1 and
// asks for v2 in the u16 at 6, which every older server ignores ("zero, ignored on
// receipt" since v1): a v2-asking launcher therefore gets a v1 reply from a 0.4.2/0.4.3
// server (one round trip, no fallback) and a v1 asker (0.4.2/0.4.3 launcher, the master)
// gets exactly the v1 reply from a new server, whose length and layout their Decode
// still accepts. A v2 reply is never sent unasked.
//
// Receivers answer only an exactly-24-byte datagram with the prefix GAGE 01 01
// (IsRequest); everything else is not a query and is left to the next layer. The
// bytes the spec calls zero (16..23 of a request, 20..23 of a v1 reply, 23 of a v2
// reply) are written as zero and ignored on receipt. A request asks for v2 only when
// its u16 at 6 is exactly 2; any other value (0, 1, 3, garbage) gets the v1 reply.
//
// Thread-safety: every function here is pure (no state, no I/O), so all of them may
// be called from any thread at once. Errors: nothing throws (the codebase builds with
// -fno-exceptions); the decoders return false and leave their output untouched.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gage::proto::query {

    // Size of a request and of a reply, in bytes. A reply is never larger than the
    // request that asked for it, so the server cannot be used to amplify traffic.
    constexpr size_t kDatagramSize = 24;
    // Size of the opaque probe id the server echoes back.
    constexpr size_t kNonceSize = 8;
    // The four ASCII bytes "GAGE" that open both datagrams (bytes 0..3).
    inline constexpr uint8_t kMagic[4] = {'G', 'A', 'G', 'E'};
    // Byte 4 of a request.
    constexpr uint8_t kKindRequest = 0x01;
    // Byte 4 of a reply.
    constexpr uint8_t kKindReply = 0x81;
    // Byte 5 of a request, and of a v1 reply: the version of this query format (not the game protocol version).
    constexpr uint8_t kQueryVersion = 1;
    // Byte 5 of a v2 reply, and the value of the request's u16 at 6 that asks for one.
    constexpr uint8_t kQueryVersion2 = 2;

    // A client release x.y.z as the three bytes a v2 reply carries (each 0..255). 0.0.0 means
    // "unknown": a server that has no latest client configured, or a v1 reply, which has none.
    struct ClientVersion
    {
        uint8_t major = 0, minor = 0, patch = 0;
        // major<<16 | minor<<8 | patch: orders like the version, 0 = unknown.
        constexpr uint32_t Packed() const { return ((uint32_t)major << 16) | ((uint32_t)minor << 8) | patch; }
        constexpr bool Known() const { return Packed() != 0; }
        constexpr bool operator==(const ClientVersion& o) const { return Packed() == o.Packed(); }
    };

    // Strict "x.y.z": three decimal numbers of 1..3 digits (no sign, no spaces, no leading text) each 0..255,
    // exactly two dots, and `n` bytes long (no NUL needed). On success writes `out`; false (out untouched) for
    // anything else, including the empty string.
    inline bool ParseClientVersion(const char* s, size_t n, ClientVersion& out)
    {
        if (!s) return false;
        unsigned part[3] = {0, 0, 0};
        size_t idx = 0, digits = 0;
        for (size_t i = 0; i < n; i++)
        {
            const char c = s[i];
            if (c >= '0' && c <= '9')
            {
                if (++digits > 3) return false;
                part[idx] = part[idx] * 10 + (unsigned)(c - '0');
                if (part[idx] > 255) return false;
            }
            else if (c == '.' && digits > 0 && idx < 2)
            {
                idx++;
                digits = 0;
            }
            else
                return false;
        }
        if (idx != 2 || digits == 0) return false;
        out.major = (uint8_t)part[0];
        out.minor = (uint8_t)part[1];
        out.patch = (uint8_t)part[2];
        return true;
    }

    // The probe id: random per probe, opaque to the server, echoed unchanged so the
    // asker can match a reply to its request and measure the round trip.
    using Nonce = std::array<uint8_t, kNonceSize>;
    // One wire datagram, request or reply.
    using Datagram = std::array<uint8_t, kDatagramSize>;

    // What a master or launcher sends.
    struct Request
    {
        Nonce nonce{}; // random per probe, opaque to the server
        // The reply format the asker understands: 1 (default; the u16 at 6 is written 0, the exact v1 request) or
        // 2 (written 2). Encode writes 2 only for a value >= 2; Decode sets 2 only for a u16 at 6 of exactly 2.
        uint8_t replyVersion = kQueryVersion;
    };

    // What a game server answers.
    struct Reply
    {
        uint16_t protocol = 0; // gage::proto::kVersion the server enforces in Hello
        Nonce nonce{};         // the request's nonce
        uint16_t players = 0;  // joined players right now
        uint16_t maxPlayers = 0;
        // The wire format: 1 (default, the exact v1 datagram) or 2. Encode writes v2 only for a value >= 2;
        // Decode sets 1 or 2 from byte 5.
        uint8_t version = kQueryVersion;
        // v2 only: the newest client the server was released with (0.0.0 = unknown). Encode writes it for v2 and
        // ignores it for v1; Decode leaves it 0.0.0 for a v1 reply.
        ClientVersion latestClient{};
    };

    // Serialises a request / reply to its exact wire form: magic, kind, query version,
    // the fields little-endian, every reserved byte zero. Total (never fails), pure.
    // A default Request / Reply (v1) encodes byte for byte as before the v2 format existed.
    Datagram Encode(const Request& r);
    Datagram Encode(const Reply& r);

    // True for a datagram a server must answer: exactly kDatagramSize bytes starting
    // GAGE 01 01. Cheap enough to run on every datagram of the game socket. A null `p`
    // is never a request (false, no read).
    bool IsRequest(const uint8_t* p, size_t n);

    // Request: same test as IsRequest, then the nonce and the reply version asked for. Reply:
    // exactly kDatagramSize bytes, magic, kind 0x81, query version 1 or 2; the fields are
    // taken as sent (a caller that wants players <= maxPlayers checks it itself). Returns
    // false for anything else (wrong length, magic, kind or version); `out` is only written
    // on success. A null `p` is false; otherwise `p` must point at `n` readable bytes.
    bool Decode(const uint8_t* p, size_t n, Request& out);
    bool Decode(const uint8_t* p, size_t n, Reply& out);

}
