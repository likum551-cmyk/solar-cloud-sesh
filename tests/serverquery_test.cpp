// serverquery_test: libs/net/serverquery.cpp (gage::net::QueryServer) against an in-process
// UDP responder on loopback. Proves the real GAGE query round trip:
//   * a correct reply is decoded -> live players/maxPlayers/protocol and a measured ms;
//   * a reply whose nonce does not echo ours is ignored (never mistaken for our answer);
//   * a port with nothing listening times out and returns false with the result untouched;
//   * query format v2 (the "update ping"): the request asks for a v2 reply (u16 at 6 = 2) and is still a
//     24-byte GAGE 01 01 datagram an old server answers; a v2 reply yields latestClient (0.4.4 packed), a v1
//     reply from an old server is accepted with latestClient 0 (unknown); v2 junk is not mistaken for a reply.
// Never shipped (selftest/). No gtest: a CHECK macro counts failures and main exits non-zero
// on any, and refuses to "pass" if no checks ran (an empty run is a failure, per lessons-core).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "net/serverfinder.h"
#include "protocol/query.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace q = gage::proto::query;

static int g_checks = 0;
static int g_fail = 0;
#define CHECK(cond, msg)                                              \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_fail;                                                \
            std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

// Bind a UDP socket to an ephemeral loopback port and report the port chosen.
static SOCKET BindLoopback(uint16_t& portOut)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    sockaddr_in a;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0; // kernel picks a free port
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    sockaddr_in got;
    int len = sizeof(got);
    if (getsockname(s, (sockaddr*)&got, &len) != 0)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    portOut = ntohs(got.sin_port);
    return s;
}

// What the last Respond() saw of the request (the test reads it after join()).
static std::atomic<int> g_reqLen{0};
static std::atomic<int> g_reqLegacyOk{0};   // the OLD server's test: exactly 24 bytes, GAGE 01 01
static std::atomic<int> g_reqReplyVersion{0};

// The responder: wait (up to 2 s) for one query on `s`, then answer it. mode 0 echoes the
// request's nonce (a correct reply) in the v1 format: what a 0.4.2 / 0.4.3 server sends, whatever the request
// asks for; mode 1 flips a nonce byte (a reply the client must reject); mode 2 is a server with the update
// ping: a v2 reply (latest client 0.4.4) to a request that asks for v2, v1 otherwise; mode 3 is mode 2 with a
// flipped nonce byte. Reports players=7, maxPlayers=32, protocol=3 so the client's decode can be checked exactly.
static void Respond(SOCKET s, int mode)
{
    DWORD to = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    uint8_t buf[64];
    sockaddr_in from;
    int fromLen = sizeof(from);
    const int n = recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    g_reqLen = n;
    g_reqLegacyOk = (n == 24 && std::memcmp(buf, "GAGE", 4) == 0 && buf[4] == 0x01 && buf[5] == 1) ? 1 : 0;
    if (n == (int)q::kDatagramSize && q::IsRequest(buf, (size_t)n))
    {
        q::Request req;
        q::Decode(buf, (size_t)n, req);
        g_reqReplyVersion = req.replyVersion;
        q::Reply rep;
        rep.protocol = 3;
        rep.players = 7;
        rep.maxPlayers = 32;
        rep.nonce = req.nonce;
        if (mode >= 2 && req.replyVersion >= 2)
        {
            rep.version = 2;
            rep.latestClient = {0, 4, 4};
        }
        if (mode == 1 || mode == 3)
            rep.nonce[0] = (uint8_t)(rep.nonce[0] ^ 0xFF);
        const q::Datagram dg = q::Encode(rep);
        sendto(s, (const char*)dg.data(), (int)dg.size(), 0, (sockaddr*)&from, fromLen);
    }
    closesocket(s);
}

int main()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::printf("serverquery_test: WSAStartup failed\n");
        return 1;
    }

    // 1) A correct reply is decoded into the live counts and a measured round trip.
    {
        uint16_t port = 0;
        SOCKET srv = BindLoopback(port);
        CHECK(srv != INVALID_SOCKET && port != 0, "bind responder (correct)");
        std::thread th(Respond, srv, 0);
        gage::net::QueryResult out;
        const bool ok = gage::net::QueryServer(L"127.0.0.1", port, 2000, out);
        th.join();
        CHECK(ok, "QueryServer returns true on a correct reply");
        CHECK(out.players == 7, "players decoded (7)");
        CHECK(out.maxPlayers == 32, "maxPlayers decoded (32)");
        CHECK(out.protocol == 3, "protocol decoded (3)");
        CHECK(out.ms >= 0, "round trip measured (ms >= 0)");
        // An old server (mode 0 sends the v1 format whatever it is asked) answered the v2-asking request: the
        // request was still one an old server accepts, and the v1 reply leaves latestClient unknown (0).
        CHECK(g_reqLen == 24 && g_reqLegacyOk == 1, "the request is exactly 24 bytes GAGE 01 01 (an old server still answers it)");
        CHECK(g_reqReplyVersion == 2, "the request asks for the v2 reply");
        CHECK(out.latestClient == 0, "a v1 reply from an old server leaves latestClient 0 (unknown, never blocks anything)");
        std::printf("  correct reply: ok=%d players=%d max=%d proto=%d ms=%d\n",
                    (int)ok, out.players, out.maxPlayers, out.protocol, out.ms);
    }

    // 2) A reply whose nonce does not echo ours must be ignored -> timeout, result untouched.
    {
        uint16_t port = 0;
        SOCKET srv = BindLoopback(port);
        CHECK(srv != INVALID_SOCKET && port != 0, "bind responder (wrong nonce)");
        std::thread th(Respond, srv, 1);
        gage::net::QueryResult out;
        const bool ok = gage::net::QueryServer(L"127.0.0.1", port, 500, out);
        th.join();
        CHECK(!ok, "QueryServer rejects a wrong-nonce reply (returns false)");
        CHECK(out.players == -1 && out.maxPlayers == -1 && out.ms == -1,
              "result left untouched when no matching reply arrives");
        std::printf("  wrong nonce: ok=%d (expected 0)\n", (int)ok);
    }

    // 2b) A server with the update ping answers the v2 request with a v2 reply: latest client 0.4.4 (packed
    //     0x000404), and every other field decodes as before.
    {
        uint16_t port = 0;
        SOCKET srv = BindLoopback(port);
        CHECK(srv != INVALID_SOCKET && port != 0, "bind responder (v2)");
        std::thread th(Respond, srv, 2);
        gage::net::QueryResult out;
        const bool ok = gage::net::QueryServer(L"127.0.0.1", port, 2000, out);
        th.join();
        CHECK(ok, "QueryServer returns true on a v2 reply");
        CHECK(out.latestClient == 0x000404, "latestClient decoded from a v2 reply (0.4.4 -> 0x000404)");
        CHECK(out.players == 7 && out.maxPlayers == 32 && out.protocol == 3 && out.ms >= 0, "the other fields of a v2 reply decode as before");
        CHECK(out.latestClient > 0x000403 && out.latestClient < 0x000500, "packed latestClient orders like the version (0.4.3 < 0.4.4 < 0.5.0)");
        std::printf("  v2 reply: ok=%d latest=0x%06X players=%d proto=%d\n", (int)ok, out.latestClient, out.players, out.protocol);
    }

    // 2c) A v2 reply with a flipped nonce is rejected like a v1 one (no latestClient from a stray datagram).
    {
        uint16_t port = 0;
        SOCKET srv = BindLoopback(port);
        CHECK(srv != INVALID_SOCKET && port != 0, "bind responder (v2, wrong nonce)");
        std::thread th(Respond, srv, 3);
        gage::net::QueryResult out;
        const bool ok = gage::net::QueryServer(L"127.0.0.1", port, 500, out);
        th.join();
        CHECK(!ok && out.latestClient == 0 && out.protocol == -1, "a v2 reply with the wrong nonce is ignored, result untouched");
    }

    // 3) A port with nothing listening times out (or the kernel's port-unreachable ends it early).
    {
        uint16_t deadPort = 0;
        SOCKET probe = BindLoopback(deadPort); // take a free port, then free it so it is closed
        CHECK(probe != INVALID_SOCKET && deadPort != 0, "reserve a dead port");
        closesocket(probe);
        gage::net::QueryResult out;
        const bool ok = gage::net::QueryServer(L"127.0.0.1", deadPort, 500, out);
        CHECK(!ok, "QueryServer returns false on a dead port");
        std::printf("  dead port: ok=%d (expected 0)\n", (int)ok);
    }

    // An empty run is not a pass (lessons-core: loop/absence checks must not pass vacuously).
    CHECK(g_checks >= 20, "enough checks ran");

    WSACleanup();
    std::printf("serverquery_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
