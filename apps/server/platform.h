// The game server's client for the platform's INTERNAL API (protocol v8, real accounts):
// POST /internal/game/verify (a joining player's single-use join token -> account id, username, roles, ban) and
// POST /internal/game/session-end (the player left). The contract is apps/platform/docs/game-server-contract.md:
// every call carries X-Gage-Server-Id and X-Gage-Server-Secret, verify answers every verdict with HTTP 200 and the
// body decides, anything else (a non-200, a transport failure, a body that does not parse) means "could not tell"
// and the caller fails closed.
//
// One worker thread owns every HTTP call (never the tick loop): the server queues Verify / SessionEnd jobs and drains
// finished verifies and log lines from Poll(). A verify is sent ONCE (the token is consumed by the first verify that
// reaches the platform, so a retry after a timeout cannot help); a session-end is idempotent and is retried up to three
// times on a transport failure, 5xx or 429. Neither the token nor the secret ever appears in a log line.
//
// Header-only on purpose: the server core's source list lives in cmake/gage_server.cmake, which this change does not
// touch; server.cpp is the only translation unit that includes it (tests include it for ParseVerifyAnswer).
#pragma once

#include "common/json.h"
#include "httpclient.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gage::server {

    // What one verify came to.
    struct VerifyResult
    {
        enum class Outcome
        {
            Ok,          // the token was valid (and is now consumed): the account fields are set
            Rejected,    // the platform refused the token: `reason` is invalid_token / expired / used (or another code it sent)
            Banned,      // the account is banned: banReason, banExpiresAt ("" = permanent)
            Unavailable, // could not tell: transport failure, a non-200, a body that is not a verdict (`reason` says which)
        };
        uint64_t serial = 0;   // the connection it was asked for (Server::Conn::serial)
        Outcome outcome = Outcome::Unavailable;
        std::string reason;
        std::string accountId, username, sessionId;
        std::vector<std::string> roles;
        std::string banReason, banExpiresAt;
        // 0.5.9 characters: the account's character slots (1 + the `character_slot` entitlements, at most 3). 0 = the platform did not say
        // (an older platform): the server plays it as 1 (sync::chr::SlotsFor).
        int characterSlots = 0;
    };

    inline const char* ToString(VerifyResult::Outcome o)
    {
        switch (o)
        {
        case VerifyResult::Outcome::Ok:          return "ok";
        case VerifyResult::Outcome::Rejected:    return "rejected";
        case VerifyResult::Outcome::Banned:      return "banned";
        case VerifyResult::Outcome::Unavailable: return "unavailable";
        }
        return "?";
    }

    namespace platform_detail {

        inline const JVal* Field(const JVal& o, const char* name)
        {
            if (o.t != JVal::Obj) return nullptr;
            for (const auto& kv : o.o)
                if (kv.first == name) return &kv.second;
            return nullptr;
        }

        inline std::string StrField(const JVal& o, const char* name)
        {
            const JVal* v = Field(o, name);
            return v && v->t == JVal::Str ? v->s : std::string();
        }

        // Printable ASCII only, cut at `cap` bytes: platform text may end up in a log line or a Refuse text.
        inline std::string Clean(const std::string& s, size_t cap)
        {
            std::string out;
            for (char ch : s)
            {
                if (out.size() >= cap) break;
                const unsigned char c = (unsigned char)ch;
                out.push_back(c >= 0x20 && c < 0x7F ? ch : '?');
            }
            return out;
        }

        // The platform's error envelope: {"error":{"code":"..."}} (or a top-level "code"); "" when neither.
        inline std::string ErrorCode(const std::string& body)
        {
            JParser p;
            p.maxNodes = 512;
            if (!p.parse(body.data(), body.size()) || p.v.t != JVal::Obj) return std::string();
            if (const JVal* e = Field(p.v, "error"))
            {
                if (e->t == JVal::Obj) return Clean(StrField(*e, "code"), 48);
                if (e->t == JVal::Str) return Clean(e->s, 48);
            }
            return Clean(StrField(p.v, "code"), 48);
        }

    }

    // Pure: what an answer of POST /internal/game/verify means (status + body as received). Exposed for the tests.
    inline VerifyResult ParseVerifyAnswer(int status, const std::string& body)
    {
        using namespace platform_detail;
        VerifyResult r;
        if (status != 200)
        {
            const std::string code = ErrorCode(body);
            r.reason = "HTTP " + std::to_string(status) + (code.empty() ? std::string() : " " + code);
            return r;
        }
        JParser p;
        p.maxNodes = 512;
        if (!p.parse(body.data(), body.size()) || p.v.t != JVal::Obj)
        {
            r.reason = "verify answer is not a JSON object";
            return r;
        }
        const JVal* ok = Field(p.v, "ok");
        if (!ok || ok->t != JVal::Bool)
        {
            r.reason = "verify answer has no boolean \"ok\"";
            return r;
        }
        if (ok->b)
        {
            r.accountId = StrField(p.v, "accountId");
            r.username = StrField(p.v, "username");
            r.sessionId = StrField(p.v, "sessionId");
            const JVal* roles = Field(p.v, "roles");
            bool rolesOk = roles && roles->t == JVal::Arr && roles->a.size() <= 16;
            if (rolesOk)
                for (const JVal& e : roles->a)
                {
                    if (e.t != JVal::Str || e.s.empty() || e.s.size() > 32) { rolesOk = false; break; }
                    r.roles.push_back(e.s);
                }
            if (r.accountId.empty() || r.accountId.size() > 64 || r.username.empty() || r.username.size() > 64 ||
                r.sessionId.size() > 64 || !rolesOk)
            {
                r = VerifyResult();
                r.reason = "verify answer ok:true lacks accountId, username or a roles array";
                return r;
            }
            r.accountId = Clean(r.accountId, 64);
            r.sessionId = Clean(r.sessionId, 64);
            // Optional (0.5.9): a whole number 1..3; anything else (absent, not a number, out of range) is 0 = "not said".
            if (const JVal* slots = Field(p.v, "characterSlots"))
                if (slots->t == JVal::Num && slots->n >= 1 && slots->n <= 3 && slots->n == (double)(int)slots->n)
                    r.characterSlots = (int)slots->n;
            r.outcome = VerifyResult::Outcome::Ok;
            return r;
        }
        r.reason = Clean(StrField(p.v, "reason"), 48);
        if (r.reason == "banned")
        {
            r.outcome = VerifyResult::Outcome::Banned;
            r.accountId = Clean(StrField(p.v, "accountId"), 64);
            r.username = StrField(p.v, "username");
            if (const JVal* ban = Field(p.v, "ban"))
            {
                r.banReason = Clean(StrField(*ban, "reason"), 120);
                r.banExpiresAt = Clean(StrField(*ban, "expiresAt"), 40);
            }
            return r;
        }
        if (r.reason.empty()) r.reason = "unknown";
        r.outcome = VerifyResult::Outcome::Rejected;
        return r;
    }

    class PlatformClient
    {
    public:
        PlatformClient() = default;
        ~PlatformClient() { Stop(0); }
        PlatformClient(const PlatformClient&) = delete;
        PlatformClient& operator=(const PlatformClient&) = delete;

        // `baseUrl` is the internal listener's origin (Config::platformUrl, already validated). False with *err when
        // it does not parse (Validate catches that first).
        bool Start(const std::string& baseUrl, const std::string& serverId, const std::string& secret, uint32_t timeoutMs,
                   std::string* err)
        {
            Stop(0);
            HttpUrl base;
            if (!ParseHttpUrl(baseUrl, base, err)) return false;
            verifyUrl_ = base;
            verifyUrl_.path = "/internal/game/verify";
            endUrl_ = base;
            endUrl_.path = "/internal/game/session-end";
            headers_ = {{"X-Gage-Server-Id", serverId}, {"X-Gage-Server-Secret", secret}};
            timeoutMs_ = timeoutMs;
            stop_ = false;
            cancel_ = false;
            drainUntilMs_ = 0;
            th_ = std::thread([this] { Run(); });
            return true;
        }

        bool running() const { return th_.joinable(); }

        void Verify(uint64_t serial, const std::string& token)
        {
            Job j;
            j.verify = true;
            j.serial = serial;
            j.token = token;
            Push(std::move(j));
        }

        // reason: quit | timeout | kicked | shutdown | crash. The session is named by its id when there is one (an
        // account id would also close a NEWER session of the same account, opened by the join that replaced this one).
        void SessionEnd(const std::string& sessionId, const std::string& accountId, const char* reason)
        {
            if (sessionId.empty() && accountId.empty()) return;
            Job j;
            j.sessionId = sessionId;
            j.accountId = sessionId.empty() ? accountId : std::string();
            j.reason = reason;
            Push(std::move(j));
        }

        // Finished verifies and the worker's log lines since the last call (the server's Poll thread).
        void Drain(std::vector<VerifyResult>& results, std::vector<std::string>& logs)
        {
            std::lock_guard<std::mutex> lock(m_);
            for (auto& r : done_) results.push_back(std::move(r));
            done_.clear();
            for (auto& l : logs_) logs.push_back(std::move(l));
            logs_.clear();
        }

        // Ends the worker. Queued session-ends are still sent for at most `drainMs` (verifies are dropped: nobody waits
        // for them any more); then whatever is in flight is cancelled. Blocks for about drainMs at most.
        void Stop(uint32_t drainMs)
        {
            if (!th_.joinable()) return;
            {
                std::lock_guard<std::mutex> lock(m_);
                stop_ = true;
                drainUntilMs_ = NowMs() + drainMs;
            }
            cv_.notify_all();
            std::thread watchdog([this, drainMs] {
                std::unique_lock<std::mutex> lock(m_);
                if (!cv_.wait_for(lock, std::chrono::milliseconds(drainMs + 50), [this] { return exited_; })) cancel_ = true;
            });
            th_.join();
            {
                std::lock_guard<std::mutex> lock(m_);
                exited_ = true;
            }
            cv_.notify_all();
            watchdog.join();
            std::lock_guard<std::mutex> lock(m_);
            jobs_.clear();
            exited_ = false;
        }

    private:
        struct Job
        {
            bool verify = false;
            uint64_t serial = 0;
            std::string token;
            std::string sessionId, accountId, reason;
            int attempts = 0;
            uint64_t notBeforeMs = 0;
        };

        static uint64_t NowMs()
        {
            using namespace std::chrono;
            return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        void Push(Job&& j)
        {
            {
                std::lock_guard<std::mutex> lock(m_);
                if (!th_.joinable() || stop_) return;
                jobs_.push_back(std::move(j));
            }
            cv_.notify_all();
        }

        void Log(const std::string& line)
        {
            std::lock_guard<std::mutex> lock(m_);
            if (logs_.size() < 256) logs_.push_back(line);
        }

        bool Post(const HttpUrl& url, const std::string& body, HttpResponse& resp, std::string& err)
        {
            HttpOptions o;
            o.connectTimeoutMs = (int)timeoutMs_;
            o.totalTimeoutMs = (int)timeoutMs_;
            o.cancel = &cancel_;
            o.headers = headers_;
            return HttpPost(url, "application/json", body, o, resp, &err);
        }

        void Run()
        {
            for (;;)
            {
                Job j;
                {
                    std::unique_lock<std::mutex> lock(m_);
                    // The first job that is due (a session-end waiting for its retry is skipped until then; while
                    // shutting down every job is due).
                    auto due = [this] {
                        const uint64_t now = NowMs();
                        for (auto i = jobs_.begin(); i != jobs_.end(); ++i)
                            if (stop_ || i->notBeforeMs <= now) return i;
                        return jobs_.end();
                    };
                    cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return stop_ || due() != jobs_.end(); });
                    if (stop_)
                    {
                        // Shutting down: session-ends only, until the drain deadline.
                        for (auto i = jobs_.begin(); i != jobs_.end();)
                            i = i->verify ? jobs_.erase(i) : std::next(i);
                        if (jobs_.empty() || NowMs() >= drainUntilMs_) return;
                    }
                    const auto it = due();
                    if (it == jobs_.end()) continue;
                    j = std::move(*it);
                    jobs_.erase(it);
                }
                if (j.verify) DoVerify(j);
                else DoSessionEnd(j);
            }
        }

        void DoVerify(const Job& j)
        {
            HttpResponse resp;
            std::string err;
            VerifyResult r;
            const uint64_t t0 = NowMs();
            if (!Post(verifyUrl_, "{\"token\":\"" + JsonEscape(j.token) + "\"}", resp, err))
                r.reason = "platform unreachable (" + platform_detail::Clean(err, 120) + ")";
            else
                r = ParseVerifyAnswer(resp.status, resp.body);
            r.serial = j.serial;
            if (r.outcome == VerifyResult::Outcome::Unavailable)
                Log("platform: verify FAILED for connection #" + std::to_string(j.serial) + " after " + std::to_string(NowMs() - t0) +
                    " ms: " + r.reason + (resp.status == 401 || resp.status == 403 ? " (configuration error: check platformServerId, "
                                                                                     "the secret and the platform's IP allowlist)"
                                                                                   : ""));
            std::lock_guard<std::mutex> lock(m_);
            done_.push_back(std::move(r));
        }

        void DoSessionEnd(Job j)
        {
            std::string body = "{";
            if (!j.sessionId.empty()) body += "\"sessionId\":\"" + JsonEscape(j.sessionId) + "\",";
            if (!j.accountId.empty()) body += "\"accountId\":\"" + JsonEscape(j.accountId) + "\",";
            body += "\"reason\":\"" + JsonEscape(j.reason) + "\"}";
            HttpResponse resp;
            std::string err;
            const bool sent = Post(endUrl_, body, resp, err);
            const std::string who = j.sessionId.empty() ? "account " + j.accountId : "session " + j.sessionId;
            if (sent && resp.status == 200)
            {
                Log("platform: session-end " + who + " (" + j.reason + "): " + platform_detail::Clean(resp.body, 60));
                return;
            }
            const bool transient = !sent || resp.status == 429 || resp.status >= 500;
            const std::string why = sent ? "HTTP " + std::to_string(resp.status) + " " + platform_detail::ErrorCode(resp.body)
                                         : platform_detail::Clean(err, 120);
            if (transient && ++j.attempts < 3)
            {
                Log("platform: session-end " + who + " failed (" + why + "); retrying");
                j.notBeforeMs = NowMs() + (j.attempts == 1 ? 500u : 1500u);
                std::lock_guard<std::mutex> lock(m_);
                if (!stop_ || NowMs() < drainUntilMs_) jobs_.push_back(std::move(j));
                return;
            }
            Log("platform: session-end " + who + " (" + j.reason + ") FAILED: " + why +
                " (the platform closes it after GAME_SESSION_MAX_HOURS)");
        }

        HttpUrl verifyUrl_, endUrl_;
        std::vector<std::pair<std::string, std::string>> headers_;
        uint32_t timeoutMs_ = 2500;
        std::thread th_;
        std::mutex m_;
        std::condition_variable cv_;
        std::atomic<bool> cancel_{false};
        bool stop_ = false;
        bool exited_ = false;
        uint64_t drainUntilMs_ = 0;
        std::deque<Job> jobs_;
        std::vector<VerifyResult> done_;
        std::vector<std::string> logs_;
    };

}
