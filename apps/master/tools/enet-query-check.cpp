// Verification harness and reference implementation for the GAGE UDP query (apps/master/PROTOCOL.md)
// against the vendored ENet 1.3.18 (third_party/enet). It is NOT part of the CMake build; build
// it by hand (see PROTOCOL.md, "Checking the claims against ENet").
//
// What it proves, on a real ENet host with 1024 peers on 127.0.0.1:
//   A. without an intercept callback ENet receives the query datagram, ignores it, creates no
//      peer, emits no event and sends no reply;
//   B. with the intercept callback below (the reference server-side implementation) the query
//      is answered with the documented 24-byte reply and ENet still sees nothing;
//   C. malformed variants (wrong size, kind, version, zeros) are not answered and are ignored
//      by ENet as well; a flood of queries creates no peers;
//   D. a real ENet client can connect and exchange a reliable packet on the same host while it
//      answers queries, and NO datagram of that ENet session is taken for a query: the callback
//      logs the first bytes of every ENet datagram it saw.
#include <enet/enet.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace {

constexpr size_t  kQuerySize    = 24;
constexpr uint8_t kMagic[4]     = { 'G', 'A', 'G', 'E' };
constexpr uint8_t kKindRequest  = 0x01;
constexpr uint8_t kKindReply    = 0x81;
constexpr uint8_t kQueryVersion = 1;
constexpr uint16_t kProtocol    = 1;   // gage::proto::kVersion
constexpr uint16_t kPlayers     = 3;
constexpr uint16_t kMaxPlayers  = 32;

int g_failures = 0;
#define CHECK(cond, what)                                                          \
    do {                                                                           \
        const bool ok_ = (cond);                                                   \
        std::printf("%s %s\n", ok_ ? "OK  " : "FAIL", what);                       \
        if (!ok_) ++g_failures;                                                    \
    } while (0)

// ---- the reference server-side implementation: an ENetHost intercept callback ----

struct InterceptStats
{
    int answered = 0;                 // queries answered
    int enetDatagrams = 0;            // datagrams handed on to ENet
    int enetDatagramsLookingLikeQuery = 0;
    std::set<std::string> enetHeads;  // first two bytes of every datagram handed to ENet
};
InterceptStats g_stats;

bool IsQuery(const enet_uint8* d, size_t n)
{
    return n == kQuerySize && std::memcmp(d, kMagic, 4) == 0 && d[4] == kKindRequest && d[5] == kQueryVersion;
}

// Return 1 = handled here, ENet never sees the datagram. 0 = not ours, ENet parses it.
int QueryIntercept(ENetHost* host, ENetEvent*)
{
    const enet_uint8* d = host->receivedData;
    const size_t n = host->receivedDataLength;
    if (!IsQuery(d, n))
    {
        ++g_stats.enetDatagrams;
        if (n >= 2)
        {
            char head[8];
            std::snprintf(head, sizeof head, "%02x%02x", d[0], d[1]);
            g_stats.enetHeads.insert(head);
        }
        if (n >= 4 && std::memcmp(d, kMagic, 4) == 0) ++g_stats.enetDatagramsLookingLikeQuery;
        return 0;
    }
    enet_uint8 reply[kQuerySize] = {};
    std::memcpy(reply, kMagic, 4);
    reply[4] = kKindReply;
    reply[5] = kQueryVersion;
    reply[6] = (enet_uint8)(kProtocol & 0xFF);
    reply[7] = (enet_uint8)(kProtocol >> 8);
    std::memcpy(reply + 8, d + 8, 8);                 // echo the nonce
    reply[16] = (enet_uint8)(kPlayers & 0xFF);
    reply[17] = (enet_uint8)(kPlayers >> 8);
    reply[18] = (enet_uint8)(kMaxPlayers & 0xFF);
    reply[19] = (enet_uint8)(kMaxPlayers >> 8);
    ENetBuffer out;
    out.data = reply;
    out.dataLength = sizeof reply;
    enet_socket_send(host->socket, &host->receivedAddress, &out, 1);
    ++g_stats.answered;
    return 1;
}

// ---- helpers ----

void MakeRequest(enet_uint8 (&b)[kQuerySize], const enet_uint8 (&nonce)[8])
{
    std::memset(b, 0, sizeof b);
    std::memcpy(b, kMagic, 4);
    b[4] = kKindRequest;
    b[5] = kQueryVersion;
    std::memcpy(b + 8, nonce, 8);
}

void SendRaw(ENetSocket s, const ENetAddress& to, const void* data, size_t n)
{
    ENetBuffer b;
    b.data = const_cast<void*>(data);
    b.dataLength = n;
    enet_socket_send(s, &to, &b, 1);
}

void Pump(ENetHost* host, int ms)
{
    ENetEvent ev;
    const enet_uint32 until = enet_time_get() + (enet_uint32)ms;
    while (enet_time_get() < until)
    {
        // Events other than NONE are unexpected in the raw-datagram tests; the caller checks counters.
        if (enet_host_service(host, &ev, 10) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE)
            enet_packet_destroy(ev.packet);
    }
}

// Receive one datagram on the raw client socket within `ms` (0 = none arrived).
int RecvRaw(ENetSocket s, enet_uint8* buf, size_t cap, int ms)
{
    enet_uint32 cond = ENET_SOCKET_WAIT_RECEIVE;
    if (enet_socket_wait(s, &cond, (enet_uint32)ms) != 0 || !(cond & ENET_SOCKET_WAIT_RECEIVE)) return 0;
    ENetAddress from;
    ENetBuffer b;
    b.data = buf;
    b.dataLength = cap;
    const int n = enet_socket_receive(s, &from, &b, 1);
    return n > 0 ? n : 0;
}

size_t LivePeers(ENetHost* h)
{
    size_t live = 0;
    for (size_t i = 0; i < h->peerCount; ++i)
        if (h->peers[i].state != ENET_PEER_STATE_DISCONNECTED) ++live;
    return live;
}

}  // namespace

int main(int argc, char** argv)
{
    if (enet_initialize() != 0)
    {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 2;
    }

    // `enet-query-check --serve <seconds>`: run only the reference responder on an ephemeral
    // loopback port for that long (prints "PORT <n>"), so another implementation, such as the
    // master's Node probe, can be pointed at it.
    if (argc == 3 && std::strcmp(argv[1], "--serve") == 0)
    {
        ENetAddress a;
        enet_address_set_host_ip(&a, "127.0.0.1");
        a.port = 0;
        ENetHost* h = enet_host_create(&a, 1024, 2, 0, 0);
        if (!h) return 2;
        h->intercept = QueryIntercept;
        ENetAddress bound;
        enet_socket_get_address(h->socket, &bound);
        std::printf("PORT %u\n", (unsigned)bound.port);
        std::fflush(stdout);
        Pump(h, std::atoi(argv[2]) * 1000);
        enet_host_destroy(h);
        enet_deinitialize();
        return 0;
    }

    ENetAddress bind;
    enet_address_set_host_ip(&bind, "127.0.0.1");
    bind.port = 0;
    ENetHost* server = enet_host_create(&bind, 1024, 2, 0, 0);   // peerCount 1024 = gage::proto::kMaxPlayers
    if (!server)
    {
        std::fprintf(stderr, "enet_host_create failed\n");
        return 2;
    }
    ENetAddress serverAddr;
    enet_socket_get_address(server->socket, &serverAddr);
    enet_address_set_host_ip(&serverAddr, "127.0.0.1");
    std::printf("ENet %d.%d.%d server, %zu peers, UDP port %u\n", ENET_VERSION_MAJOR, ENET_VERSION_MINOR,
        ENET_VERSION_PATCH, server->peerCount, (unsigned)serverAddr.port);

    ENetSocket raw = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    enet_socket_set_option(raw, ENET_SOCKOPT_NONBLOCK, 1);

    const enet_uint8 nonce[8] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18 };
    enet_uint8 request[kQuerySize];
    MakeRequest(request, nonce);
    enet_uint8 got[256];

    // A. no intercept: ENet ignores the datagram.
    std::printf("-- A: no intercept callback\n");
    SendRaw(raw, serverAddr, request, sizeof request);
    Pump(server, 150);
    CHECK(server->totalReceivedPackets == 1, "ENet received the datagram");
    CHECK(LivePeers(server) == 0 && server->connectedPeers == 0, "ENet created no peer for it");
    CHECK(RecvRaw(raw, got, sizeof got, 150) == 0, "ENet sent no reply");

    // B. with the intercept: answered, ENet untouched.
    std::printf("-- B: intercept callback installed\n");
    server->intercept = QueryIntercept;
    const enet_uint32 receivedBefore = server->totalReceivedPackets;
    SendRaw(raw, serverAddr, request, sizeof request);
    Pump(server, 150);
    const int n = RecvRaw(raw, got, sizeof got, 500);
    CHECK(n == (int)kQuerySize, "reply is exactly 24 bytes (same size as the request: no amplification)");
    CHECK(n == (int)kQuerySize && std::memcmp(got, kMagic, 4) == 0 && got[4] == kKindReply && got[5] == kQueryVersion, "reply magic, kind 0x81, query version 1");
    CHECK(n == (int)kQuerySize && std::memcmp(got + 8, nonce, 8) == 0, "reply echoes the nonce");
    CHECK(n == (int)kQuerySize && got[6] == (kProtocol & 0xFF) && got[7] == (kProtocol >> 8), "reply carries protocol 1 (u16 LE)");
    CHECK(n == (int)kQuerySize && got[16] == kPlayers && got[17] == 0 && got[18] == kMaxPlayers && got[19] == 0, "reply carries players 3 / maxPlayers 32 (u16 LE)");
    CHECK(server->totalReceivedPackets == receivedBefore + 1 && g_stats.enetDatagrams == 0, "the query never reached ENet's parser");
    CHECK(LivePeers(server) == 0, "still no peer");

    // C. malformed variants and a flood.
    std::printf("-- C: malformed queries and a flood\n");
    enet_uint8 bad[kQuerySize + 1];
    const int answeredBefore = g_stats.answered;
    auto variant = [&](size_t len, int mutateIndex, enet_uint8 mutateValue) {
        std::memcpy(bad, request, kQuerySize);
        bad[kQuerySize] = 0;
        if (mutateIndex >= 0) bad[mutateIndex] = mutateValue;
        SendRaw(raw, serverAddr, bad, len);
    };
    variant(kQuerySize - 1, -1, 0);   // too short
    variant(kQuerySize + 1, -1, 0);   // too long
    variant(kQuerySize, 4, kKindReply); // a reply sent as a request
    variant(kQuerySize, 5, 2);        // unknown query version
    variant(kQuerySize, 0, 0);        // bad magic
    enet_uint8 zeros[kQuerySize] = {};
    SendRaw(raw, serverAddr, zeros, sizeof zeros);
    Pump(server, 200);
    CHECK(g_stats.answered == answeredBefore, "none of the malformed variants was answered");
    CHECK(RecvRaw(raw, got, sizeof got, 100) == 0, "no reply datagram for them");
    CHECK(LivePeers(server) == 0 && server->connectedPeers == 0, "ENet created no peer for them");

    const int floodBefore = g_stats.answered;
    for (int i = 0; i < 1000; ++i) SendRaw(raw, serverAddr, request, sizeof request);
    Pump(server, 500);
    CHECK(g_stats.answered > floodBefore, "a flood of queries is answered (the C++ side must rate-limit it, PROTOCOL.md)");
    CHECK(LivePeers(server) == 0 && server->connectedPeers == 0, "a flood of queries creates no peers");
    while (RecvRaw(raw, got, sizeof got, 20) > 0) {}

    // D. a real ENet session next to queries.
    std::printf("-- D: a real ENet client on the same host, queries in between\n");
    g_stats = InterceptStats{};
    ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
    ENetPeer* toServer = enet_host_connect(client, &serverAddr, 2, 0);
    bool clientUp = false, serverSawConnect = false, gotPayload = false, sentPayload = false;
    const enet_uint32 deadline = enet_time_get() + 4000;
    int queriesDuringSession = 0;
    while (enet_time_get() < deadline && !gotPayload)
    {
        ENetEvent ev;
        while (enet_host_service(client, &ev, 0) > 0)
        {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) clientUp = true;
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
        }
        while (enet_host_service(server, &ev, 5) > 0)
        {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) serverSawConnect = true;
            if (ev.type == ENET_EVENT_TYPE_RECEIVE)
            {
                gotPayload = ev.packet->dataLength == 5 && std::memcmp(ev.packet->data, "hello", 5) == 0;
                enet_packet_destroy(ev.packet);
            }
        }
        if (clientUp && !sentPayload)
        {
            ENetPacket* p = enet_packet_create("hello", 5, ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(toServer, 0, p);
            sentPayload = true;
        }
        if (clientUp && queriesDuringSession < 20)
        {
            SendRaw(raw, serverAddr, request, sizeof request);
            ++queriesDuringSession;
        }
    }
    CHECK(clientUp && serverSawConnect, "ENet handshake completed with the intercept installed");
    CHECK(gotPayload, "a reliable ENet packet was delivered to the server");
    CHECK(g_stats.enetDatagrams > 0, "the callback saw the ENet session's datagrams");
    CHECK(g_stats.enetDatagramsLookingLikeQuery == 0, "no ENet datagram started with the query magic");
    std::string heads;
    for (const std::string& h : g_stats.enetHeads) heads += h + " ";
    std::printf("     first two bytes seen on ENet datagrams (peerID|flags|session, big-endian): %s\n", heads.c_str());
    CHECK(g_stats.enetHeads.count("4741") == 0, "none of them is 4741 (the query magic read as an ENet header)");
    CHECK(g_stats.answered >= 1, "queries sent during the session were answered");

    enet_peer_disconnect_now(toServer, 0);
    enet_host_flush(client);
    enet_host_destroy(client);
    enet_host_destroy(server);
    enet_socket_destroy(raw);
    enet_deinitialize();

    std::printf(g_failures == 0 ? "ALL CHECKS PASSED\n" : "%d CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
