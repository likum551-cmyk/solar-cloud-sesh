// GAGE UDP server query client (Winsock). The UDP counterpart of ping.cpp:
// PingServer races a TCP connect, which a GAGE server (ENet over UDP) never
// answers, so this sends the real 24-byte GAGE query (protocol/query.h) to the
// game port, measures the round trip, and reads the live player counts the
// server replies with. Kept in its own translation unit so winsock2.h precedes
// windows.h, exactly as ping.cpp documents.
//
// One probe, one socket, the calling thread: resolve host (UDP), connect() the
// datagram socket to the peer so the kernel drops replies from anyone else,
// send one request with a random nonce, then select() for the matching reply
// until the deadline. A stray datagram on the socket (connect() makes that
// unlikely, but not impossible) is skipped and the wait continues within the
// same deadline. No retries: a lost query reads as "no reply", which the caller
// polls again on its own cadence.
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
#include <cstring>
#include <string>

namespace {

    // Process-wide Winsock init, shared semantics with ping.cpp (its own static
    // lives in that TU; a second WSAStartup here is reference-counted by Winsock
    // and equally never cleaned up, since sockets may outlive any one call).
    bool EnsureWinsock()
    {
        static const bool ok = []
        {
            WSADATA wsa;
            return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        }();
        return ok;
    }

    LONGLONG QpcFreq()
    {
        static const LONGLONG f = []
        {
            LARGE_INTEGER v;
            if (!QueryPerformanceFrequency(&v) || v.QuadPart <= 0)
                return (LONGLONG)1000;
            return (LONGLONG)v.QuadPart;
        }();
        return f;
    }

    LONGLONG QpcNow()
    {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    int ElapsedMs(LONGLONG from, LONGLONG to)
    {
        LONGLONG d = to - from;
        if (d < 0)
            d = 0;
        LONGLONG f = QpcFreq();
        return (int)((d * 1000 + f / 2) / f);
    }

    // Milliseconds left before `deadline` (a counter value), rounded up; 0 when
    // expired. Mirrors ping.cpp::RemainingMs.
    LONGLONG RemainingMs(LONGLONG deadline)
    {
        LONGLONG left = deadline - QpcNow();
        if (left <= 0)
            return 0;
        LONGLONG f = QpcFreq();
        return (left * 1000 + f - 1) / f;
    }

    // A nonce need not be cryptographic: it only has to distinguish this probe's
    // reply from a stray datagram and from a previous probe's late reply on a
    // reused source port. splitmix64 over a seed mixing the clock, the thread id
    // and a process-wide counter gives 8 well-spread bytes with no dependency
    // (bcrypt, rand_s) that the launcher/tools targets would have to link.
    void FillNonce(gage::proto::query::Nonce& n)
    {
        static std::atomic<uint64_t> counter{0};
        uint64_t x = (uint64_t)QpcNow();
        x ^= (uint64_t)GetCurrentThreadId() << 32;
        x ^= counter.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ull;
        // splitmix64 finaliser
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x = x ^ (x >> 31);
        for (size_t i = 0; i < n.size(); ++i)
            n[i] = (uint8_t)(x >> (i * 8));
    }

}

namespace gage::net {

    bool QueryServer(const std::wstring& host, int port, int timeoutMs, QueryResult& out)
    {
        out = QueryResult{};
        if (port < 1 || port > 65535)
            return false;
        if (timeoutMs < 1)
            timeoutMs = 1;
        if (!EnsureWinsock())
            return false;

        const std::string h = gage::Narrow(host);
        if (h.empty())
            return false;

        struct addrinfo hints;
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        char portStr[16];
        _snprintf_s(portStr, sizeof(portStr), _TRUNCATE, "%d", port);

        struct addrinfo* res = nullptr;
        if (getaddrinfo(h.c_str(), portStr, &hints, &res) != 0 || res == nullptr)
            return false;

        using namespace gage::proto;
        query::Request req;
        req.replyVersion = query::kQueryVersion2; // older servers ignore this and answer v1
        FillNonce(req.nonce);
        const query::Datagram dg = query::Encode(req);

        bool ok = false;
        for (struct addrinfo* a = res; a != nullptr && !ok; a = a->ai_next)
        {
            if (a->ai_family != AF_INET && a->ai_family != AF_INET6)
                continue;
            SOCKET s = socket(a->ai_family, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET)
                continue;
            // connect() a datagram socket: fixes the peer so recv() returns only
            // datagrams from this server (an unrelated host's spoofed reply to our
            // ephemeral port is dropped by the kernel) and lets us send()/recv().
            if (connect(s, a->ai_addr, (int)a->ai_addrlen) != 0)
            {
                closesocket(s);
                continue;
            }

            const LONGLONG t0 = QpcNow();
            if (send(s, (const char*)dg.data(), (int)dg.size(), 0) != (int)dg.size())
            {
                closesocket(s);
                continue;
            }

            const LONGLONG deadline = t0 + (LONGLONG)timeoutMs * QpcFreq() / 1000;
            for (;;)
            {
                const LONGLONG left = RemainingMs(deadline);
                if (left <= 0)
                    break;
                fd_set rf;
                FD_ZERO(&rf);
                FD_SET(s, &rf);
                struct timeval tv;
                tv.tv_sec = (long)(left / 1000);
                tv.tv_usec = (long)(left % 1000) * 1000;
                const int rc = select(0, &rf, nullptr, nullptr, &tv);
                if (rc <= 0)
                    break; // timeout (0) or socket error (SOCKET_ERROR)
                uint8_t buf[64];
                const int n = recv(s, (char*)buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                query::Reply rep;
                if (query::Decode(buf, (size_t)n, rep) && rep.nonce == req.nonce)
                {
                    out.ms = ElapsedMs(t0, QpcNow());
                    out.players = (int)rep.players;
                    out.maxPlayers = (int)rep.maxPlayers;
                    out.protocol = (int)rep.protocol;
                    out.latestClient = (int)rep.latestClient.Packed();
                    ok = true;
                    break;
                }
                // a datagram that is not our reply: keep waiting within the deadline
            }
            closesocket(s);
        }

        freeaddrinfo(res);
        return ok;
    }

}
