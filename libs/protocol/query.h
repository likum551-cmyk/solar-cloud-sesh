// GAGE UDP query: the 24-byte datagram pair a master server or the launcher sends
// to a game server's UDP port to learn whether a GAGE server is there, its live
// player counts and (from the round trip) its ping. apps/master/PROTOCOL.md is the
// authority for the layout; this file is the portable codec (no sockets, no
// platform headers) so the server, the launcher and tools share one definition.
//
// Request (to the game port), 24 bytes, little-endian:
//   0  magic "GAGE"   4  kind 0x01   5  query version 1   6  u16 zero
//   8  nonce[8]       16 zero[8]
// Reply (to the request's source address and port), 24 bytes:
//   0  magic "GAGE"   4  kind 0x81   5  query version 1   6  u16 game protocol version
//   8  nonce[8] (echoed unchanged)   16 u16 players   18 u16 maxPlayers   20 zero[4]
//
// Receivers answer only an exactly-24-byte datagram with the prefix GAGE 01 01
// (IsRequest); everything else is not a query and is left to the next layer. The
// bytes the spec calls zero (6..7 and 16..23 of a request, 20..23 of a reply) are
// written as zero and ignored on receipt.
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
    // Byte 5 of both: the version of this query format (not the game protocol version).
    constexpr uint8_t kQueryVersion = 1;

    // The probe id: random per probe, opaque to the server, echoed unchanged so the
    // asker can match a reply to its request and measure the round trip.
    using Nonce = std::array<uint8_t, kNonceSize>;
    // One wire datagram, request or reply.
    using Datagram = std::array<uint8_t, kDatagramSize>;

    // What a master or launcher sends.
    struct Request
    {
        Nonce nonce{}; // random per probe, opaque to the server
    };

    // What a game server answers.
    struct Reply
    {
        uint16_t protocol = 0; // gage::proto::kVersion the server enforces in Hello
        Nonce nonce{};         // the request's nonce
        uint16_t players = 0;  // joined players right now
        uint16_t maxPlayers = 0;
    };

    // Serialises a request / reply to its exact wire form: magic, kind, query version,
    // the fields little-endian, every reserved byte zero. Total (never fails), pure.
    Datagram Encode(const Request& r);
    Datagram Encode(const Reply& r);

    // True for a datagram a server must answer: exactly kDatagramSize bytes starting
    // GAGE 01 01. Cheap enough to run on every datagram of the game socket. A null `p`
    // is never a request (false, no read).
    bool IsRequest(const uint8_t* p, size_t n);

    // Request: same test as IsRequest, then the nonce. Reply: exactly kDatagramSize
    // bytes, magic, kind 0x81, query version 1; the fields are taken as sent (a
    // caller that wants players <= maxPlayers checks it itself). Returns false for
    // anything else (wrong length, magic, kind or version); `out` is only written on
    // success. A null `p` is false; otherwise `p` must point at `n` readable bytes.
    bool Decode(const uint8_t* p, size_t n, Request& out);
    bool Decode(const uint8_t* p, size_t n, Reply& out);

}
