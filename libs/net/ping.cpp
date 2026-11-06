// TCP connect-time ping (Winsock). Kept in its own translation unit so
// winsock2.h can be included before windows.h without MIN/MAX or socket-type
// clashes.
//
// One engine (PingMany) serves every entry point:
//   * Winsock is started once per process (WSAStartup in a function-local
//     static).
//   * Every target runs at once (up to kMaxActive) in ONE select() loop on
//     the calling thread: no thread per ping, and a dead server costs its own
//     deadline only, never a slot others wait for.
//   * Hostnames resolve on a small pool of helper threads; getaddrinfo cannot
//     be cancelled, so a stuck lookup only ever costs its own target. The
//     pool shares its state by shared_ptr: whichever side lets go last frees it.
//   * Each target's deadline covers its DNS lookup and every connect attempt;
//     the measured time is the connect time of the winning attempt only (DNS
//     excluded), taken with QueryPerformanceCounter.
//   * Every resolved address (up to kMaxAddrs) is connected at once on every
//     port, and the first to complete wins, so a dead IPv6 route or a port
//     that silently drops SYNs never holds the answer.
//   * A failed connect is detected through select()'s except set instead of
//     waiting out the whole timeout. (Windows itself retries SYNs to a closed
//     loopback port for ~2 s before reporting the refusal.)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows' fd_set is a counted array, sized here: kMaxActive targets x
// kMaxAddrs addresses x 2 ports sockets at most.
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
#include "net/serverfinder.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

    const int kMaxAddrs = 4;          // resolved addresses tried per port
    const int kMaxPorts = 2;          // a server: game port + resource port
    const int kMaxActive = 128;       // targets in flight at once (<= FD_SETSIZE / 8)
    const int kResolvers = 8;         // DNS helper threads per PingMany call
    const int kSynRetransmitMs = 950; // a connect this slow waited out a SYN retransmit
    const int kInitialRtoMs = 1000;   // Windows' SYN retransmit timer (Get-NetTCPSetting InitialRto, default)
    const int kRetryMs = 400;         // the re-ask's own deadline: a game-server trip is far shorter
    const int kFlushMs = 40;          // results are handed over at most this often
    const int kResolvePollMs = 10;    // loop tick while a lookup is outstanding

    static_assert(kMaxActive * kMaxAddrs * kMaxPorts <= FD_SETSIZE, "fd_set too small");

    // Process-wide Winsock init. Thread-safe (C++11 local static); never
    // cleaned up because other threads may still hold sockets at exit.
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
                return (LONGLONG)1000; // cannot happen on XP+; keep arithmetic sane
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

    LONGLONG QpcAfterMs(LONGLONG from, int ms)
    {
        return from + (LONGLONG)ms * QpcFreq() / 1000;
    }

    // Whole milliseconds between two counter readings, rounded to nearest.
    int ElapsedMs(LONGLONG from, LONGLONG to)
    {
        LONGLONG d = to - from;
        if (d < 0)
            d = 0;
        LONGLONG f = QpcFreq();
        return (int)((d * 1000 + f / 2) / f);
    }

    // Milliseconds left before `deadline` (counter value), rounded up; 0 when expired.
    LONGLONG RemainingMs(LONGLONG deadline)
    {
        LONGLONG left = deadline - QpcNow();
        if (left <= 0)
            return 0;
        LONGLONG f = QpcFreq();
        return (left * 1000 + f - 1) / f;
    }

    int ResolveNow(const std::string& host, struct addrinfo** res)
    {
        struct addrinfo hints;
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        return getaddrinfo(host.c_str(), nullptr, &hints, res);
    }

    bool IsNumericHost(const std::string& host)
    {
        struct in_addr a4;
        struct in6_addr a6;
        return inet_pton(AF_INET, host.c_str(), &a4) == 1 || inet_pton(AF_INET6, host.c_str(), &a6) == 1;
    }

    // ---- DNS pool --------------------------------------------------------
    // Hostname lookups for one PingMany call. Helper threads claim slots in
    // order; the loop takes a slot's result once `state` says done. If the
    // loop ends first (deadline, cancel), it marks the board abandoned and the
    // helpers stop claiming; the last owner frees what is left.
    struct ResolveBoard
    {
        struct Slot
        {
            std::string host;
            struct addrinfo* res = nullptr;
            std::atomic<int> state{0}; // 0 waiting, 1 resolving, 2 done
            int rc = EAI_FAIL;
        };
        std::unique_ptr<Slot[]> slots;
        int count = 0;
        std::atomic<int> next{0};
        std::atomic<bool> abandoned{false};

        ~ResolveBoard()
        {
            for (int i = 0; i < count; ++i)
                if (slots[i].res)
                    freeaddrinfo(slots[i].res);
        }
    };

    void ResolveWorker(std::shared_ptr<ResolveBoard> b)
    {
        for (int i; !b->abandoned.load() && (i = b->next++) < b->count;)
        {
            ResolveBoard::Slot& s = b->slots[i];
            s.state = 1;
            struct addrinfo* res = nullptr;
            s.rc = ResolveNow(s.host, &res);
            s.res = s.rc == 0 ? res : nullptr;
            s.state = 2; // publishes res/rc to the loop (seq_cst)
        }
    }

    // ---- one target --------------------------------------------------------
    struct Attempt { SOCKET s; LONGLONG t0; int port; };

    struct Job
    {
        enum Phase { Idle, Resolving, Connecting, Retry, Done };
        std::string host;
        int ports[kMaxPorts] = {0, 0};
        int nports = 0;
        bool server = false;   // PingServer semantics: resource port + SYN-drop handling
        int slot = -1;         // DNS board slot, or -1 for a numeric host
        Phase phase = Idle;
        LONGLONG deadline = 0;
        struct addrinfo* addrs = nullptr; // owned once taken from the board
        Attempt socks[kMaxAddrs * kMaxPorts];
        int nsocks = 0;
        int first = -1;        // a SYN-retransmit-slow first reply, while re-asking
        int wonPort = 0;
    };

    void CloseAll(Job& j)
    {
        for (int i = 0; i < j.nsocks; ++i)
            closesocket(j.socks[i].s);
        j.nsocks = 0;
    }

    // Starts a non-blocking connect to every address on every port in `ports`.
    // Returns the connect time if one completed at once, else -1 (pending
    // attempts sit in j.socks).
    int Connect(Job& j, const int* ports, int nports)
    {
        int addrs = 0;
        for (struct addrinfo* a = j.addrs; a != nullptr && addrs < kMaxAddrs; a = a->ai_next)
        {
            if ((a->ai_family != AF_INET && a->ai_family != AF_INET6) || a->ai_addrlen > sizeof(sockaddr_storage))
                continue;
            ++addrs;
            for (int p = 0; p < nports; ++p)
            {
                sockaddr_storage addr;
                memcpy(&addr, a->ai_addr, a->ai_addrlen);
                const u_short nport = htons((u_short)ports[p]);
                if (a->ai_family == AF_INET)
                    ((sockaddr_in*)&addr)->sin_port = nport;
                else
                    ((sockaddr_in6*)&addr)->sin6_port = nport;

                SOCKET s = socket(a->ai_family, SOCK_STREAM, IPPROTO_TCP);
                if (s == INVALID_SOCKET)
                    continue;
                u_long nb = 1;
                if (ioctlsocket(s, FIONBIO, &nb) != 0)
                {
                    closesocket(s);
                    continue;
                }
                const LONGLONG t0 = QpcNow();
                if (connect(s, (const sockaddr*)&addr, (int)a->ai_addrlen) == 0)
                {
                    closesocket(s);
                    j.wonPort = ports[p];
                    return ElapsedMs(t0, QpcNow());
                }
                if (WSAGetLastError() == WSAEWOULDBLOCK)
                    j.socks[j.nsocks++] = Attempt{s, t0, ports[p]};
                else
                    closesocket(s);
            }
        }
        return -1;
    }

    // The value a target reports when it stops: a reply, -1, or (while
    // re-asking) the first reply less the retransmit wait.
    int Settle(const Job& j, int reply)
    {
        if (j.phase != Job::Retry)
            return reply;
        // A reply just over a second is Windows' first SYN retransmit, not the
        // route: anti-DDoS front ends drop a client's first SYN on purpose. The
        // re-ask usually gets through on its first SYN and measures the real
        // trip; if it is dropped too, the first connect completed on the
        // retransmitted SYN, so its time is the RTO plus the trip (measured
        // 2026-10-25: 1099..1112 ms against 95..99 ms re-asks).
        if (reply >= 0 && reply < j.first)
            return reply;
        return j.first > kInitialRtoMs ? j.first - kInitialRtoMs : j.first;
    }

}

namespace gage::net {

    void PingMany(const std::vector<PingTarget>& targets, int timeoutMs,
                  const std::function<bool()>& cancelled, const PingBatchFn& onBatch)
    {
        const int n = (int)targets.size();
        if (n == 0)
            return;
        if (timeoutMs < 1)
            timeoutMs = 1;
        std::vector<std::pair<int, int>> out;
        if (!EnsureWinsock())
        {
            for (int i = 0; i < n; ++i)
                out.emplace_back(i, -1);
            onBatch(out);
            return;
        }

        std::vector<Job> jobs(n);
        auto board = std::make_shared<ResolveBoard>();
        int lookups = 0;
        for (int i = 0; i < n; ++i)
        {
            Job& j = jobs[i];
            const PingTarget& t = targets[i];
            j.host = gage::Narrow(t.host);
            j.server = t.server;
            j.ports[0] = t.port;
            j.nports = 1;
            if (t.server && t.port < 65535)
                j.ports[j.nports++] = t.port + 1;
            if (!j.host.empty() && !IsNumericHost(j.host))
                j.slot = lookups++;
        }
        if (lookups > 0)
        {
            board->slots.reset(new ResolveBoard::Slot[lookups]);
            board->count = lookups;
            for (int i = 0; i < n; ++i)
                if (jobs[i].slot >= 0)
                    board->slots[jobs[i].slot].host = jobs[i].host;
            const int helpers = lookups < kResolvers ? lookups : kResolvers;
            for (int h = 0; h < helpers; ++h)
                std::thread(ResolveWorker, board).detach();
        }

        int nextJob = 0, active = 0, done = 0;
        LONGLONG lastFlush = QpcNow();

        auto finish = [&](int i, int ms) {
            Job& j = jobs[i];
            CloseAll(j);
            if (j.addrs)
                freeaddrinfo(j.addrs);
            j.addrs = nullptr;
            j.phase = Job::Done;
            --active;
            ++done;
            out.emplace_back(i, ms);
        };

        // A connect completed: report it, or re-ask once if it waited out a
        // dropped first SYN.
        auto replied = [&](int i, int ms, int port) {
            Job& j = jobs[i];
            CloseAll(j);
            if (j.server && j.phase == Job::Connecting && ms >= kSynRetransmitMs)
            {
                j.phase = Job::Retry;
                j.first = ms;
                j.deadline = QpcAfterMs(QpcNow(), kRetryMs);
                const int again = Connect(j, &port, 1);
                if (again >= 0)
                    finish(i, Settle(j, again));
                else if (j.nsocks == 0)
                    finish(i, Settle(j, -1));
                return;
            }
            finish(i, Settle(j, ms));
        };

        auto startConnect = [&](int i) {
            Job& j = jobs[i];
            if (!j.addrs)
            {
                finish(i, -1);
                return;
            }
            j.phase = Job::Connecting;
            const int ms = Connect(j, j.ports, j.nports);
            if (ms >= 0)
                replied(i, ms, j.wonPort);
            else if (j.nsocks == 0)
                finish(i, -1);
        };

        while (done < n)
        {
            if (cancelled && cancelled())
                break;

            // admit new targets
            while (active < kMaxActive && nextJob < n)
            {
                const int i = nextJob++;
                Job& j = jobs[i];
                ++active;
                j.deadline = QpcAfterMs(QpcNow(), timeoutMs);
                const PingTarget& t = targets[i];
                if (j.host.empty() || t.port < 1 || t.port > 65535)
                {
                    finish(i, -1);
                    continue;
                }
                if (j.slot < 0)
                {
                    struct addrinfo* res = nullptr;
                    j.addrs = ResolveNow(j.host, &res) == 0 ? res : nullptr;
                    startConnect(i);
                }
                else
                    j.phase = Job::Resolving;
            }

            // collect finished lookups
            bool resolving = false;
            for (int i = 0; i < nextJob; ++i)
            {
                Job& j = jobs[i];
                if (j.phase != Job::Resolving)
                    continue;
                ResolveBoard::Slot& s = board->slots[j.slot];
                if (s.state.load() == 2)
                {
                    j.addrs = s.res; // ownership moves to the job
                    s.res = nullptr;
                    startConnect(i);
                }
                else
                    resolving = true;
            }

            // expire deadlines; gather sockets and the nearest deadline
            fd_set wf, ef;
            FD_ZERO(&wf);
            FD_ZERO(&ef);
            int nfds = 0;
            LONGLONG waitMs = kFlushMs;
            for (int i = 0; i < nextJob; ++i)
            {
                Job& j = jobs[i];
                if (j.phase == Job::Done || j.phase == Job::Idle)
                    continue;
                const LONGLONG left = RemainingMs(j.deadline);
                if (left <= 0)
                {
                    finish(i, Settle(j, -1));
                    continue;
                }
                if (left < waitMs)
                    waitMs = left;
                for (int k = 0; k < j.nsocks; ++k)
                {
                    FD_SET(j.socks[k].s, &wf);
                    FD_SET(j.socks[k].s, &ef); // a refused connect shows up here, not in wf
                    ++nfds;
                }
            }
            if (resolving && waitMs > kResolvePollMs)
                waitMs = kResolvePollMs;

            if (done < n && nfds == 0)
                Sleep((DWORD)waitMs); // select() refuses empty sets on Windows
            else if (done < n)
            {
                struct timeval tv;
                tv.tv_sec = (long)(waitMs / 1000);
                tv.tv_usec = (long)(waitMs % 1000) * 1000;
                const int rc = select(0, nullptr, &wf, &ef, &tv);
                if (rc == SOCKET_ERROR)
                {
                    // cannot wait on these sockets: fail what is in flight rather than spin
                    for (int i = 0; i < nextJob; ++i)
                        if (jobs[i].phase == Job::Connecting || jobs[i].phase == Job::Retry)
                            finish(i, Settle(jobs[i], -1));
                }
                else if (rc > 0)
                {
                    const LONGLONG tNow = QpcNow();
                    for (int i = 0; i < nextJob; ++i)
                    {
                        Job& j = jobs[i];
                        if (j.phase != Job::Connecting && j.phase != Job::Retry)
                            continue;
                        for (int k = 0; k < j.nsocks;)
                        {
                            Attempt& a = j.socks[k];
                            bool failed = FD_ISSET(a.s, &ef) != 0, ok = false;
                            if (!failed && FD_ISSET(a.s, &wf))
                            {
                                int soErr = 0;
                                int len = sizeof(soErr);
                                ok = getsockopt(a.s, SOL_SOCKET, SO_ERROR, (char*)&soErr, &len) == 0 && soErr == 0;
                                failed = !ok;
                            }
                            if (ok)
                            {
                                replied(i, ElapsedMs(a.t0, tNow), a.port);
                                break; // the job's sockets are gone (or replaced by the re-ask)
                            }
                            if (failed)
                            {
                                closesocket(a.s);
                                j.socks[k] = j.socks[--j.nsocks];
                            }
                            else
                                ++k;
                        }
                        // every attempt refused: no reply is coming
                        if ((j.phase == Job::Connecting || j.phase == Job::Retry) && j.nsocks == 0)
                            finish(i, Settle(j, -1));
                    }
                }
            }

            if (!out.empty() && (done == n || RemainingMs(QpcAfterMs(lastFlush, kFlushMs)) == 0))
            {
                onBatch(out);
                out.clear();
                lastFlush = QpcNow();
            }
        }

        // cancelled: release what is still open (no results for those)
        for (Job& j : jobs)
        {
            CloseAll(j);
            if (j.addrs)
                freeaddrinfo(j.addrs);
            j.addrs = nullptr;
        }
        board->abandoned = true;
        if (!out.empty())
            onBatch(out);
    }

    static int PingOne(const PingTarget& t, int timeoutMs)
    {
        int result = -1;
        PingMany({t}, timeoutMs, nullptr, [&](const std::vector<std::pair<int, int>>& r) {
            if (!r.empty())
                result = r.back().second;
        });
        return result;
    }

    int PingServer(const std::wstring& host, int port, int timeoutMs)
    {
        return PingOne(PingTarget{host, port, true}, timeoutMs);
    }

}
