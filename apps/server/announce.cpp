#include "announce.h"

#include "util.h"

// The launcher's header-only JSON reader (libs/common/json.h), as config.cpp uses it.
#include "common/json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace gage::server {

    namespace {

        constexpr size_t kMaxQueuedLog = 200;
        constexpr uint32_t kMaxRetryAfterMs = 3600u * 1000u;
        constexpr int kStopWaitMs = 2000;

        const JVal* Member(const JVal& obj, const char* key)
        {
            if (obj.t != JVal::Obj) return nullptr;
            for (const auto& kv : obj.o)
                if (kv.first == key) return &kv.second;
            return nullptr;
        }

        std::string Str(const JVal& obj, const char* key)
        {
            const JVal* v = Member(obj, key);
            return v && v->t == JVal::Str ? v->s : std::string();
        }

        // One-line, length-bounded copy of master-supplied text for the log.
        std::string Clean(const std::string& s, size_t max = 160)
        {
            std::string out;
            for (char ch : s)
            {
                const unsigned char c = (unsigned char)ch;
                out.push_back(c < 0x20 || c == 0x7F ? ' ' : ch);
                if (out.size() >= max)
                {
                    out += "...";
                    break;
                }
            }
            return out;
        }

        std::string Seconds(uint32_t ms)
        {
            if (ms % 1000 == 0) return std::to_string(ms / 1000) + " s";
            return std::to_string(ms) + " ms";
        }

        // What an operator should do about a master rejection, by its `error` code
        // (apps/master/README.md, POST /announce).
        std::string Hint(const std::string& error, const AnnounceSettings& s)
        {
            if (error == "validation_failed")
                return "the master rejected the announce fields: check name, gamemode, lang and url in server.json";
            if (error == "private_address")
                return "the master sees this server's address as non-public (LAN, loopback or NAT): announce from a "
                       "public address, or run the master with ALLOW_PRIVATE=1 for local tests";
            if (error == "too_many_servers")
                return "the master already lists its per-address maximum of servers for this address";
            if (error == "unreachable")
                return "the master could not reach UDP port " + std::to_string(s.gamePort) +
                       " with the GAGE query: open it in the firewall/NAT";
            if (error == "protocol_mismatch")
                return "the master expects a different protocol version than v" + std::to_string(s.protocol);
            if (error == "rate_limited") return "announcing too often for this address; backing off";
            if (error == "server_list_full") return "the master's list is full";
            if (error == "probe_busy") return "the master is busy probing other servers";
            if (error == "payload_too_large" || error == "unsupported_media_type" || error == "invalid_json" ||
                error == "bad_forwarded_for")
                return "unexpected for this client: a bug here or a misconfigured proxy in front of the master";
            return "";
        }

    }

    std::string AnnouncePath(const HttpUrl& master)
    {
        std::string p = master.path;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (p == "/") return "/announce";
        const std::string tail = "/announce";
        if (p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0) return p;
        return p + tail;
    }

    std::string BuildAnnounceJson(const AnnounceSettings& s, uint16_t players)
    {
        std::string j = "{\"port\":" + std::to_string(s.gamePort) + ",\"name\":\"" + JsonEscape(s.name) +
                        "\",\"gamemode\":\"" + JsonEscape(s.gamemode) + "\",\"lang\":\"" + JsonEscape(s.lang) + "\"";
        if (!s.url.empty()) j += ",\"url\":\"" + JsonEscape(s.url) + "\"";
        j += ",\"players\":" + std::to_string(players) + ",\"maxPlayers\":" + std::to_string(s.maxPlayers) +
             ",\"protocol\":" + std::to_string(s.protocol) + "}";
        return j;
    }

    struct Announcer::State
    {
        AnnounceSettings settings;
        std::string path;

        std::mutex m;
        std::condition_variable cv;
        std::atomic<bool> cancel{false};
        bool done = false; // under m
        std::vector<std::string> log; // under m
        std::atomic<uint16_t> players{0};
        std::atomic<uint64_t> attempts{0}, ok{0}, failed{0};
        std::thread th;

        void Push(const std::string& line)
        {
            std::lock_guard<std::mutex> lock(m);
            if (log.size() >= kMaxQueuedLog) log.erase(log.begin());
            log.push_back(line);
        }

        // Sleeps up to `ms`; false when cancelled meanwhile.
        bool Sleep(uint32_t ms)
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return cancel.load(); });
            return !cancel.load();
        }

        uint32_t Backoff(uint32_t failures) const
        {
            uint64_t d = settings.backoffBaseMs ? settings.backoffBaseMs : 1;
            for (uint32_t i = 1; i < failures && d < settings.backoffMaxMs; i++) d *= 2;
            return (uint32_t)std::min<uint64_t>(d, settings.backoffMaxMs);
        }

        void Run()
        {
            uint32_t failures = 0;
            uint32_t intervalMs = settings.intervalMs;
            bool listed = false;
            HttpOptions opt;
            opt.connectTimeoutMs = settings.connectTimeoutMs;
            opt.totalTimeoutMs = settings.requestTimeoutMs;
            opt.cancel = &cancel;

            while (!cancel.load())
            {
                const std::string body = BuildAnnounceJson(settings, players.load());
                HttpResponse resp;
                std::string err;
                attempts++;
                const bool got = HttpPost(settings.master, "application/json", body, opt, resp, &err);
                if (cancel.load()) break;

                uint32_t nextMs;
                if (!got)
                {
                    failed++;
                    failures++;
                    listed = false;
                    nextMs = Backoff(failures);
                    Push("master: announce to " + settings.master.hostHeader + " failed: " + Clean(err) + "; retry in " +
                         Seconds(nextMs));
                }
                else if (resp.status == 200)
                {
                    ok++;
                    JParser p;
                    p.maxNodes = 64;
                    std::string key;
                    bool created = false;
                    double ttl = 0, hb = 0;
                    if (p.parse(resp.body.data(), resp.body.size()) && p.v.t == JVal::Obj)
                    {
                        key = Str(p.v, "key");
                        if (const JVal* c = Member(p.v, "created")) created = c->t == JVal::Bool && c->b;
                        if (const JVal* t = Member(p.v, "ttlSeconds")) ttl = t->t == JVal::Num ? t->n : 0;
                        if (const JVal* h = Member(p.v, "heartbeatSeconds")) hb = h->t == JVal::Num ? h->n : 0;
                    }
                    // Never slower than the master asks; never faster than we are configured to.
                    intervalMs = settings.intervalMs;
                    if (hb >= 1 && hb < 100000)
                        intervalMs = std::min<uint32_t>(intervalMs, (uint32_t)(hb * 1000.0));
                    if (!listed)
                        Push("master: listed" + (key.empty() ? std::string() : " as " + Clean(key, 80)) +
                             (created ? " (new entry)" : "") + (ttl > 0 ? ", ttl " + std::to_string((long)ttl) + " s" : "") +
                             ", announcing every " + Seconds(intervalMs) + (failures ? " (recovered)" : ""));
                    listed = true;
                    failures = 0;
                    nextMs = intervalMs;
                }
                else
                {
                    failed++;
                    failures++;
                    listed = false;
                    JParser p;
                    p.maxNodes = 128;
                    std::string error, message, issues;
                    if (p.parse(resp.body.data(), resp.body.size()) && p.v.t == JVal::Obj)
                    {
                        error = Clean(Str(p.v, "error"), 48);
                        message = Clean(Str(p.v, "message"));
                        if (const JVal* is = Member(p.v, "issues"))
                            if (is->t == JVal::Arr)
                                for (size_t i = 0; i < is->a.size() && i < 3; i++)
                                    issues += (issues.empty() ? "" : "; ") + Clean(Str(is->a[i], "path"), 40) + " " +
                                              Clean(Str(is->a[i], "message"), 80);
                    }
                    nextMs = Backoff(failures);
                    if (resp.retryAfterSeconds >= 0)
                        nextMs = std::max<uint32_t>(
                            nextMs, (uint32_t)std::min<uint64_t>((uint64_t)resp.retryAfterSeconds * 1000u, kMaxRetryAfterMs));
                    std::string line = "master: announce rejected, HTTP " + std::to_string(resp.status);
                    if (!error.empty()) line += " " + error;
                    if (!message.empty()) line += ": " + message;
                    if (!issues.empty()) line += " [" + issues + "]";
                    const std::string hint = Hint(error, settings);
                    if (!hint.empty()) line += " (" + hint + ")";
                    line += "; retry in " + Seconds(nextMs);
                    Push(line);
                }
                if (!Sleep(nextMs)) break;
            }
            std::lock_guard<std::mutex> lock(m);
            done = true;
            cv.notify_all();
        }
    };

    Announcer::Announcer() = default;

    Announcer::~Announcer() { Stop(); }

    bool Announcer::Start(const AnnounceSettings& settings, std::string* err)
    {
        if (st_)
        {
            if (err) *err = "announcer already running";
            return false;
        }
        auto st = std::make_shared<State>();
        st->settings = settings;
        st->path = AnnouncePath(settings.master);
        st->settings.master.path = st->path; // HttpPost sends url.path
        st->players = 0;
        std::shared_ptr<State> keep = st;
        st->th = std::thread([keep] { keep->Run(); });
        st_ = st;
        return true;
    }

    void Announcer::SetPlayers(uint16_t players)
    {
        if (st_) st_->players.store(players);
    }

    std::vector<std::string> Announcer::TakeLog()
    {
        std::vector<std::string> out;
        if (!st_) return out;
        std::lock_guard<std::mutex> lock(st_->m);
        out.swap(st_->log);
        return out;
    }

    Announcer::Counters Announcer::counters() const
    {
        Counters c;
        if (st_)
        {
            c.attempts = st_->attempts.load();
            c.ok = st_->ok.load();
            c.failed = st_->failed.load();
        }
        return c;
    }

    void Announcer::Stop()
    {
        if (!st_) return;
        std::shared_ptr<State> st = std::move(st_);
        st_.reset();
        {
            std::lock_guard<std::mutex> lock(st->m);
            st->cancel.store(true);
        }
        st->cv.notify_all();
        bool finished;
        {
            std::unique_lock<std::mutex> lock(st->m);
            finished = st->cv.wait_for(lock, std::chrono::milliseconds(kStopWaitMs), [&st] { return st->done; });
        }
        if (finished)
            st->th.join();
        else
            st->th.detach(); // still inside a blocking resolve; it ends on its own, holding only `st`
    }

}
