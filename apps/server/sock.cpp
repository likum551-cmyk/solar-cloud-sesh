#include "sock.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace gage::server::sock {

    namespace {

#ifdef _WIN32
        using Native = SOCKET;
        inline Native N(Handle h) { return (Native)h; }
        inline int LastErr() { return WSAGetLastError(); }
        inline bool WouldBlockErr(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
        inline bool InProgressErr(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
        inline void CloseNative(Native s) { closesocket(s); }
        std::string ErrText(int e) { return "error " + std::to_string(e); }
#else
        using Native = int;
        inline Native N(Handle h) { return (Native)h; }
        inline int LastErr() { return errno; }
        inline bool WouldBlockErr(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINTR; }
        inline bool InProgressErr(int e) { return e == EINPROGRESS || e == EINTR; }
        inline void CloseNative(Native s) { ::close(s); }
        std::string ErrText(int e) { return std::strerror(e); }
#endif

        uint64_t NowMs()
        {
            using namespace std::chrono;
            return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        // 1 ready, 0 timeout, -1 error.
        int WaitNative(Native s, bool forWrite, int timeoutMs)
        {
            if (timeoutMs < 0) timeoutMs = 0;
#ifdef _WIN32
            fd_set set;
            FD_ZERO(&set);
            FD_SET(s, &set);
            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            // A failed non-blocking connect is reported through the exception set on Windows.
            fd_set ex;
            FD_ZERO(&ex);
            FD_SET(s, &ex);
            const int r = select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, forWrite ? &ex : nullptr, &tv);
            if (r < 0) return -1;
            if (r == 0) return 0;
            return 1;
#else
            pollfd p;
            p.fd = s;
            p.events = forWrite ? POLLOUT : POLLIN;
            p.revents = 0;
            const int r = ::poll(&p, 1, timeoutMs);
            if (r < 0) return errno == EINTR ? 0 : -1;
            return r == 0 ? 0 : 1;
#endif
        }

        void SetNonBlockingNative(Native s)
        {
#ifdef _WIN32
            u_long on = 1;
            ioctlsocket(s, FIONBIO, &on);
#else
            const int fl = fcntl(s, F_GETFL, 0);
            fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
        }

        // 0 when the connect finished OK, else the socket error.
        int ConnectResult(Native s)
        {
            int e = 0;
#ifdef _WIN32
            int len = sizeof(e);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &len) != 0) return LastErr();
#else
            socklen_t len = sizeof(e);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &len) != 0) return LastErr();
#endif
            return e;
        }

    }

    bool Init()
    {
#ifdef _WIN32
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
#else
        return true;
#endif
    }

    void Cleanup()
    {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void Close(Handle h)
    {
        if (h != kInvalid) CloseNative(N(h));
    }

    void ShutdownSend(Handle h)
    {
        if (h == kInvalid) return;
#ifdef _WIN32
        shutdown(N(h), SD_SEND);
#else
        ::shutdown(N(h), SHUT_WR);
#endif
    }

    bool SetNonBlocking(Handle h, bool on)
    {
#ifdef _WIN32
        u_long v = on ? 1 : 0;
        return ioctlsocket(N(h), FIONBIO, &v) == 0;
#else
        const int fl = fcntl(N(h), F_GETFL, 0);
        if (fl < 0) return false;
        return fcntl(N(h), F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0;
#endif
    }

    Io Recv(Handle h, void* buf, size_t cap, size_t* got)
    {
        if (got) *got = 0;
        if (cap == 0) return Io::Ok;
#ifdef _WIN32
        const int r = recv(N(h), (char*)buf, (int)(cap > 0x40000000 ? 0x40000000 : cap), 0);
#else
        const ssize_t r = ::recv(N(h), buf, cap, 0);
#endif
        if (r > 0)
        {
            if (got) *got = (size_t)r;
            return Io::Ok;
        }
        if (r == 0) return Io::Closed;
        return WouldBlockErr(LastErr()) ? Io::WouldBlock : Io::Error;
    }

    Io Send(Handle h, const void* buf, size_t n, size_t* sent)
    {
        if (sent) *sent = 0;
        if (n == 0) return Io::Ok;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
#ifdef _WIN32
        const int r = send(N(h), (const char*)buf, (int)(n > 0x40000000 ? 0x40000000 : n), flags);
#else
        const ssize_t r = ::send(N(h), buf, n, flags);
#endif
        if (r >= 0)
        {
            if (sent) *sent = (size_t)r;
            return Io::Ok;
        }
        return WouldBlockErr(LastErr()) ? Io::WouldBlock : Io::Error;
    }

    int Wait(Handle h, bool forWrite, int timeoutMs) { return WaitNative(N(h), forWrite, timeoutMs); }

    Handle ConnectTcp(const std::string& host, uint16_t port, int timeoutMs, const std::atomic<bool>* cancel,
                      std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return kInvalid;
        };
        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* res = nullptr;
        char portText[8];
        std::snprintf(portText, sizeof(portText), "%u", (unsigned)port);
        const int gai = getaddrinfo(host.c_str(), portText, &hints, &res);
        if (gai != 0 || !res) return fail("cannot resolve " + host);

        const uint64_t deadline = NowMs() + (uint64_t)(timeoutMs < 0 ? 0 : timeoutMs);
        std::string lastErr = "no address to connect to";
        Handle result = kInvalid;
        for (addrinfo* ai = res; ai && result == kInvalid; ai = ai->ai_next)
        {
            const Native s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
#ifdef _WIN32
            if (s == INVALID_SOCKET) { lastErr = "socket(): " + ErrText(LastErr()); continue; }
#else
            if (s < 0) { lastErr = "socket(): " + ErrText(LastErr()); continue; }
#endif
            SetNonBlockingNative(s);
            int rc = ::connect(s, ai->ai_addr, (int)ai->ai_addrlen);
            bool connected = rc == 0;
            if (!connected && !InProgressErr(LastErr()))
            {
                lastErr = "connect(): " + ErrText(LastErr());
                CloseNative(s);
                continue;
            }
            while (!connected)
            {
                if (cancel && cancel->load())
                {
                    CloseNative(s);
                    freeaddrinfo(res);
                    return fail("cancelled");
                }
                const uint64_t now = NowMs();
                if (now >= deadline)
                {
                    lastErr = "connect timed out";
                    break;
                }
                const int slice = (int)(deadline - now < 100 ? deadline - now : 100);
                const int w = WaitNative(s, true, slice);
                if (w < 0)
                {
                    lastErr = "wait failed";
                    break;
                }
                if (w == 1)
                {
                    const int e = ConnectResult(s);
                    if (e == 0)
                        connected = true;
                    else
                    {
                        lastErr = "connect(): " + ErrText(e);
                        break;
                    }
                }
            }
            if (connected)
                result = (Handle)s;
            else
                CloseNative(s);
        }
        freeaddrinfo(res);
        if (result == kInvalid) return fail(lastErr);
        return result;
    }

    Handle ListenLoopback(uint16_t port, uint16_t* bound, std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return kInvalid;
        };
        const Native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
        if (s == INVALID_SOCKET) return fail("socket(): " + ErrText(LastErr()));
#else
        if (s < 0) return fail("socket(): " + ErrText(LastErr()));
        // Quick restart after a stop; Windows' SO_REUSEADDR would allow port hijacking instead.
        const int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
        sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0)
        {
            const std::string why = "bind 127.0.0.1:" + std::to_string(port) + ": " + ErrText(LastErr());
            CloseNative(s);
            return fail(why);
        }
        if (::listen(s, 8) != 0)
        {
            const std::string why = "listen(): " + ErrText(LastErr());
            CloseNative(s);
            return fail(why);
        }
        SetNonBlockingNative(s);
        if (bound)
        {
            sockaddr_in b;
#ifdef _WIN32
            int len = sizeof(b);
#else
            socklen_t len = sizeof(b);
#endif
            *bound = (getsockname(s, (sockaddr*)&b, &len) == 0) ? ntohs(b.sin_port) : port;
        }
        return (Handle)s;
    }

    bool LocalAddress(Handle h, std::string* ip, uint16_t* port)
    {
        sockaddr_in b;
#ifdef _WIN32
        int len = sizeof(b);
#else
        socklen_t len = sizeof(b);
#endif
        if (getsockname(N(h), (sockaddr*)&b, &len) != 0 || b.sin_family != AF_INET) return false;
        const uint32_t host = ntohl(b.sin_addr.s_addr);
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (host >> 24) & 255, (host >> 16) & 255, (host >> 8) & 255,
                      host & 255);
        if (ip) *ip = buf;
        if (port) *port = ntohs(b.sin_port);
        return true;
    }

    Handle Accept(Handle listener, uint32_t* peerHost)
    {
        sockaddr_storage ss;
#ifdef _WIN32
        int len = sizeof(ss);
#else
        socklen_t len = sizeof(ss);
#endif
        const Native c = ::accept(N(listener), (sockaddr*)&ss, &len);
#ifdef _WIN32
        if (c == INVALID_SOCKET) return kInvalid;
#else
        if (c < 0) return kInvalid;
#endif
        SetNonBlockingNative(c);
        if (peerHost)
        {
            *peerHost = 0;
            if (ss.ss_family == AF_INET) *peerHost = ntohl(((sockaddr_in*)&ss)->sin_addr.s_addr);
        }
        return (Handle)c;
    }

    bool OutboundIpv4(std::string* ip)
    {
        const Native s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
        if (s == INVALID_SOCKET) return false;
#else
        if (s < 0) return false;
#endif
        sockaddr_in to;
        std::memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(9);
        to.sin_addr.s_addr = htonl(0xC0000201u); // 192.0.2.1 (TEST-NET-1), never routed
        bool ok = ::connect(s, (sockaddr*)&to, sizeof(to)) == 0 && LocalAddress((Handle)s, ip, nullptr);
        CloseNative(s);
        return ok;
    }

}
