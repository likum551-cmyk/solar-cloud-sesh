// Master-list announce: a background thread that POSTs this server's state to the
// master (apps/master/README.md, POST /announce) every heartbeat, over plain HTTP.
//
// Thread model: the worker owns all the slow work (DNS, connect, request, waits).
// The tick loop only ever calls SetPlayers() (an atomic store) and TakeLog() (a
// short mutex), so a dead or slow master can never stall a tick. The worker does not
// log by itself: it queues lines and the owner logs them from its own thread
// (the log sink is not required to be thread-safe).
//
// Schedule: the first announce goes out at once; then every min(heartbeat, the
// heartbeatSeconds the master reports) while it answers 200. Any failure (network
// error, timeout, or a non-200 status) backs off exponentially from backoffBaseMs to
// backoffMaxMs, honouring a Retry-After the master sets (429, 503), and goes back to
// the normal interval at the first 200. Every non-200 status is logged with the
// master's error code and message and a hint (the README's table); a 200 is logged
// when the server first gets listed and when it recovers.
//
// Stop() wakes the worker and waits for it for up to two seconds. A worker still
// stuck in a blocking DNS lookup after that is detached; it holds only its own
// shared state, touches no log sink, and exits when the lookup returns.
#pragma once

#include "httpclient.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gage::server {

    // Everything the announcer needs; Server fills it from Config at Start().
    struct AnnounceSettings
    {
        HttpUrl master;      // parsed masterUrl
        uint16_t gamePort = 0;   // the UDP game port to announce (the master probes it with the query)
        std::string name;        // server name, 1..64 bytes
        std::string gamemode;    // 1..32 bytes
        std::string lang;        // language tag, at most 8 bytes
        std::string url;         // optional info page; omitted from the JSON when empty
        uint16_t protocol = 0;   // gage::proto::kVersion
        uint16_t maxPlayers = 0;
        uint32_t intervalMs = 30000;     // announce period while the master answers 200
        uint32_t backoffBaseMs = 5000;   // first retry delay after a failure; doubles each time
        uint32_t backoffMaxMs = 300000;  // cap on the retry delay
        int connectTimeoutMs = 5000;     // TCP connect budget of one announce
        int requestTimeoutMs = 10000;    // whole-request budget of one announce
    };

    // The HTTP request path to POST to for `master`: its path plus "/announce" (a URL
    // that already ends in "/announce" is used as it is; a trailing slash is ignored).
    // "http://m" -> "/announce", "http://m/base/" -> "/base/announce",
    // "http://m/base/announce" -> "/base/announce". Pure, total.
    std::string AnnouncePath(const HttpUrl& master);

    // The JSON body of one announce: port, name, gamemode, lang, [url], players,
    // maxPlayers, protocol (the master's schema), with every string JSON-escaped.
    // `players` is the live count to report. Pure, total, thread-safe.
    std::string BuildAnnounceJson(const AnnounceSettings& s, uint16_t players);

    // The background heartbeat to the master list.
    //
    // Thread-safety: Start(), Stop(), running(), SetPlayers(), TakeLog() and counters()
    // are meant to be called from ONE owner thread (the server's Poll thread); the worker
    // thread is internal. SetPlayers() and counters() are lock-free and TakeLog() takes a
    // short mutex, so none of them can wait on the network. Errors: nothing throws.
    class Announcer
    {
    public:
        Announcer();
        ~Announcer(); // Stop()s
        Announcer(const Announcer&) = delete;
        Announcer& operator=(const Announcer&) = delete;

        // Starts the worker thread, which announces at once and then on the schedule
        // above. `settings` is copied; the master path is replaced by AnnouncePath().
        // Returns true when started. Returns false with *err ("announcer already
        // running") when called twice without Stop() in between. The announcer can be
        // started again after Stop().
        bool Start(const AnnounceSettings& settings, std::string* err = nullptr);
        // Cancels the worker (an in-flight request is aborted within about 100 ms) and
        // waits for it for up to two seconds, then returns; see the header comment for the
        // detach rule. Safe to call when not running and from the destructor. After it
        // returns no further request is started.
        void Stop();
        // True between a successful Start() and Stop().
        bool running() const { return st_ != nullptr; }

        // Sets the player count the NEXT announce reports (an atomic store; call it
        // every tick). Ignored when not running.
        void SetPlayers(uint16_t players);
        // Returns and clears the log lines the worker produced since the last call,
        // oldest first (at most the newest 200 are kept between calls). The owner logs
        // them from its own thread. Empty when not running. Lines look like
        //   master: listed as 203.0.113.9:24800 (new entry), ttl 90 s, announcing every 30 s
        //   master: announce rejected, HTTP 422 unreachable: <message> (<hint>); retry in 5 s
        //   master: announce to master.example.org:8080 failed: <reason>; retry in 5 s
        std::vector<std::string> TakeLog();

        // Running totals, for tests and the console.
        struct Counters
        {
            uint64_t attempts = 0; // requests started
            uint64_t ok = 0;       // HTTP 200
            uint64_t failed = 0;   // transport failures and non-200 statuses
        };
        // A snapshot of the totals (all zero when not running).
        Counters counters() const;

    private:
        struct State;
        std::shared_ptr<State> st_;
    };

}
