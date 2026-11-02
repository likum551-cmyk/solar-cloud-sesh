#include "admin.h"

#include "ratelimit.h"
#include "sock.h"
#include "util.h"

#include <vector>

namespace gage::server {

    namespace {

        constexpr size_t kMaxLine = 1024;
        constexpr size_t kMaxOutQueue = 256 * 1024;
        constexpr uint64_t kLingerMs = 1000; // after our last reply, wait this long for the peer to read it and close

        std::string Lower(std::string s)
        {
            for (char& c : s)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            return s;
        }

        std::string Reply(bool ok, const std::string& text, const char* key = "output")
        {
            return std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"" + key + "\":\"" + JsonEscape(text) + "\"}\n";
        }

    }

    bool ConstantTimeEquals(const std::string& a, const std::string& b)
    {
        // Walk the longer length so the loop count does not leak where they differ.
        const size_t n = a.size() > b.size() ? a.size() : b.size();
        unsigned diff = (unsigned)(a.size() ^ b.size());
        for (size_t i = 0; i < n; i++)
        {
            const unsigned char x = i < a.size() ? (unsigned char)a[i] : 0;
            const unsigned char y = i < b.size() ? (unsigned char)b[i] : 0;
            diff |= (unsigned)(x ^ y);
        }
        return diff == 0;
    }

    struct AdminChannel::Impl
    {
        struct Client
        {
            sock::Handle h = sock::kInvalid;
            uint64_t connectedAtMs = 0;
            bool authed = false;
            // 0 = serving; 1 = final reply queued, flushing it; 2 = FIN sent, waiting
            // for the peer to read it and close (input is ignored).
            int fin = 0;
            uint64_t finStartMs = 0;
            std::string in, out;
        };

        AdminOptions opt;
        sock::Handle listener = sock::kInvalid;
        uint16_t boundPort = 0;
        bool sockInit = false;
        TokenBucket failures;
        std::vector<Client> clients;
        Stats stats;

        void Finish(Client& c, uint64_t nowMs, const std::string& line)
        {
            c.out += line;
            c.fin = 1;
            c.finStartMs = nowMs;
        }

        // Pushes queued output; false when the connection is dead.
        bool Flush(Client& c)
        {
            while (!c.out.empty())
            {
                size_t n = 0;
                const sock::Io r = sock::Send(c.h, c.out.data(), c.out.size(), &n);
                if (r == sock::Io::Ok)
                    c.out.erase(0, n);
                else if (r == sock::Io::WouldBlock)
                    break;
                else
                    return false;
            }
            return c.out.size() <= kMaxOutQueue;
        }

        void Drop(Client& c)
        {
            sock::Close(c.h);
            c.h = sock::kInvalid;
        }
    };

    AdminChannel::AdminChannel() : impl_(new Impl) {}

    AdminChannel::~AdminChannel() { Stop(); }

    bool AdminChannel::Start(const AdminOptions& opt, std::string* err)
    {
        if (impl_->listener != sock::kInvalid)
        {
            if (err) *err = "admin channel already running";
            return false;
        }
        if (!sock::Init())
        {
            if (err) *err = "network stack unavailable";
            return false;
        }
        impl_->sockInit = true;
        std::string lerr;
        uint16_t bound = 0;
        const sock::Handle h = sock::ListenLoopback(opt.port, &bound, &lerr);
        if (h == sock::kInvalid)
        {
            sock::Cleanup();
            impl_->sockInit = false;
            if (err) *err = "cannot listen for admin connections: " + lerr;
            return false;
        }
        impl_->opt = opt;
        impl_->listener = h;
        impl_->boundPort = bound;
        impl_->failures.Configure(opt.failedLoginBurst, opt.failedLoginPerMinute / 60.0);
        impl_->stats = Stats();
        return true;
    }

    void AdminChannel::Stop()
    {
        for (Impl::Client& c : impl_->clients) impl_->Drop(c);
        impl_->clients.clear();
        if (impl_->listener != sock::kInvalid)
        {
            sock::Close(impl_->listener);
            impl_->listener = sock::kInvalid;
        }
        if (impl_->sockInit)
        {
            sock::Cleanup();
            impl_->sockInit = false;
        }
        impl_->boundPort = 0;
    }

    bool AdminChannel::running() const { return impl_->listener != sock::kInvalid; }
    uint16_t AdminChannel::port() const { return impl_->boundPort; }
    const AdminChannel::Stats& AdminChannel::stats() const { return impl_->stats; }
    size_t AdminChannel::clients() const { return impl_->clients.size(); }

    void AdminChannel::Service(uint64_t nowMs, const Exec& exec, const LogFn& log)
    {
        Impl& d = *impl_;
        if (d.listener == sock::kInvalid) return;

        // New connections.
        for (int i = 0; i < 8; i++)
        {
            uint32_t peer = 0;
            const sock::Handle h = sock::Accept(d.listener, &peer);
            if (h == sock::kInvalid) break;
            d.stats.accepted++;
            if ((peer & 0xFF000000u) != 0x7F000000u)
            {
                // Cannot happen with a 127.0.0.1 listener; kept as a second wall.
                d.stats.rejectedRemote++;
                sock::Close(h);
                if (log) log("admin: refused a non-loopback peer");
                continue;
            }
            if (d.clients.size() >= d.opt.maxClients)
            {
                d.stats.rejectedBusy++;
                size_t n = 0;
                const std::string msg = Reply(false, "too many admin connections", "error");
                sock::Send(h, msg.data(), msg.size(), &n);
                sock::Close(h);
                continue;
            }
            Impl::Client c;
            c.h = h;
            c.connectedAtMs = nowMs;
            d.clients.push_back(std::move(c));
        }

        for (Impl::Client& c : d.clients)
        {
            if (c.h == sock::kInvalid) continue;

            if (!c.authed && c.fin == 0 && nowMs - c.connectedAtMs >= d.opt.authTimeoutMs)
                d.Finish(c, nowMs, Reply(false, "login timed out", "error"));

            // Read what is there (bounded per call so one client cannot hog the tick).
            char buf[1024];
            for (int i = 0; i < 8 && c.h != sock::kInvalid; i++)
            {
                size_t n = 0;
                const sock::Io r = sock::Recv(c.h, buf, sizeof(buf), &n);
                if (r == sock::Io::WouldBlock) break;
                if (r != sock::Io::Ok)
                {
                    d.Drop(c);
                    break;
                }
                if (c.fin != 0) continue; // draining after the final reply; input is ignored
                c.in.append(buf, n);
                if (c.in.find('\n') == std::string::npos && c.in.size() > kMaxLine)
                {
                    c.in.clear();
                    d.Finish(c, nowMs, Reply(false, "line too long", "error"));
                }
            }
            if (c.h == sock::kInvalid) continue;

            // Whole lines.
            size_t nl;
            while (c.fin == 0 && (nl = c.in.find('\n')) != std::string::npos)
            {
                std::string line = Trim(c.in.substr(0, nl));
                c.in.erase(0, nl + 1);
                if (line.size() > kMaxLine)
                {
                    d.Finish(c, nowMs, Reply(false, "line too long", "error"));
                    break;
                }
                if (line.empty()) continue;

                if (!c.authed)
                {
                    const size_t sp = line.find(' ');
                    const std::string cmd = Lower(line.substr(0, sp));
                    const std::string given = sp == std::string::npos ? std::string() : Trim(line.substr(sp + 1));
                    if (cmd != "auth")
                    {
                        d.Finish(c, nowMs, Reply(false, "authentication required: send `auth <token>` first", "error"));
                        break;
                    }
                    if (!d.failures.Has(nowMs))
                    {
                        // Out of failure allowance: do not even look at the token.
                        d.stats.lockedOut++;
                        if (log) log("admin: login refused, too many failed logins recently");
                        d.Finish(c, nowMs, Reply(false, "too many failed logins, try again later", "error"));
                        break;
                    }
                    if (!d.opt.token.empty() && ConstantTimeEquals(given, d.opt.token))
                    {
                        c.authed = true;
                        d.stats.authOk++;
                        if (log) log("admin: client authenticated");
                        c.out += Reply(true, "authenticated");
                    }
                    else
                    {
                        d.failures.Take(nowMs);
                        d.stats.authFailed++;
                        if (log) log("admin: login failed (bad token)");
                        d.Finish(c, nowMs, Reply(false, "bad token", "error"));
                        break;
                    }
                    continue;
                }

                d.stats.commands++;
                if (log) log("admin: command: " + line);
                std::string output;
                const bool ok = exec ? exec(line, output) : false;
                c.out += Reply(ok, output);
            }

            if (c.h == sock::kInvalid) continue;
            if (!d.Flush(c))
            {
                d.Drop(c);
                continue;
            }
            if (c.fin == 1)
            {
                if (c.out.empty())
                {
                    sock::ShutdownSend(c.h);
                    c.fin = 2;
                    c.finStartMs = nowMs;
                }
                else if (nowMs - c.finStartMs >= kLingerMs * 3)
                    d.Drop(c); // the peer never reads
            }
            else if (c.fin == 2 && nowMs - c.finStartMs >= kLingerMs)
                d.Drop(c);
        }

        std::vector<Impl::Client> alive;
        for (Impl::Client& c : d.clients)
            if (c.h != sock::kInvalid) alive.push_back(std::move(c));
        d.clients.swap(alive);
    }

}
