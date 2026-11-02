// Local admin control channel: a loopback-only TCP line protocol that runs the same
// commands as the server console, so a future web panel (or a script) can drive the
// server without stdin. It is NOT an in-game admin system: players never get admin
// rights from this or from anything else (no names, no roles; the console and this
// channel are the only authority).
//
// Security model, in one place:
//  * disabled unless Config::adminPort is set; the listener binds 127.0.0.1 ONLY
//    (never 0.0.0.0, never configurable), and every accepted peer is checked to be a
//    127.x address as well;
//  * the first line must be `auth <token>` with the token from server.json
//    (adminToken, at least 16 characters; env GAGE_ADMIN_TOKEN fills it when the
//    file has none). Compared in constant time. A wrong or missing token closes the
//    connection; after 10 failed logins within a minute (all sources together) new
//    logins are refused without even looking at the token until the allowance refills
//    (about one failed attempt per 6 s), so the token cannot be brute-forced through
//    this port. The token is never logged or echoed;
//  * the token is a shared secret over an unencrypted local socket: anyone who can
//    read it, or who can already run code as another local user and sniff loopback,
//    is trusted. It is not meant to be exposed beyond the machine (a reverse proxy
//    in front of a web panel must add its own TLS and login);
//  * at most 4 clients at once, lines of at most 1024 bytes, an unauthenticated
//    connection lives 5 s.
//
// Protocol: client sends lines; the server answers every line with exactly one JSON
// line: {"ok":true,"output":"..."} or {"ok":false,"output":"..."} (or "error" for
// protocol-level failures: bad token, auth required, line too long, too many
// clients). Line 1: `auth <token>`, answered {"ok":true,"output":"authenticated"}.
// After that each line is a console command (list, kick, say, ban, banip, unban,
// bans, help, quit).
//
// Single-threaded like the server: Service() is called from Server::Poll(), uses
// non-blocking sockets and never waits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace gage::server {

    // How the channel listens and defends itself; Server fills it from Config.
    struct AdminOptions
    {
        uint16_t port = 0;  // 0 = ephemeral (tests); read the real one back with port()
        std::string token;  // the shared secret; an empty token never authenticates anyone
        size_t maxClients = 4;              // simultaneous connections; the next gets "too many admin connections"
        uint32_t authTimeoutMs = 5000;      // a connection that has not authenticated by then is closed
        double failedLoginBurst = 10;       // failed logins allowed in a burst ...
        double failedLoginPerMinute = 10;   // ... and refilled per minute
    };

    // The listener, its clients and the line protocol.
    //
    // Thread-safety: none; every method must be called from the thread that drives
    // Server::Poll() (the channel never starts a thread). Errors: nothing throws; Start()
    // reports failures through its return value and *err, and a misbehaving client is
    // answered with an error line and closed, never reported to the caller.
    class AdminChannel
    {
    public:
        // Runs one console line for an authenticated client. Returns false for an error
        // result (the reply then carries "ok":false); `output` is the reply text.
        using Exec = std::function<bool(const std::string& line, std::string& output)>;
        // Receives the channel's own log lines ("admin: client authenticated", ...). They
        // never contain the token.
        using LogFn = std::function<void(const std::string&)>;

        AdminChannel();
        ~AdminChannel(); // Stop()s
        AdminChannel(const AdminChannel&) = delete;
        AdminChannel& operator=(const AdminChannel&) = delete;

        // Binds 127.0.0.1:opt.port (never any other address) and starts listening;
        // `opt` is copied and the statistics reset. Returns true when listening. Returns
        // false with *err when it is already running, the socket library is unavailable,
        // or the port cannot be bound (in use, no permission).
        bool Start(const AdminOptions& opt, std::string* err);
        // Closes every client and the listener. Safe to call when not running.
        void Stop();
        // True between a successful Start() and Stop().
        bool running() const;
        // The bound loopback TCP port (the real one when Start() was given 0); 0 when
        // not running.
        uint16_t port() const;

        // One non-blocking pass: accepts up to 8 new connections, reads what clients
        // sent (at most 8 KB per client per call), runs each complete line (`exec` for
        // authenticated clients, the `auth` check otherwise), queues and flushes the
        // JSON-line replies, and closes clients that failed to authenticate, timed out,
        // sent an over-long line or went away. `nowMs` is a monotonic millisecond clock
        // (the server passes net::NowMs()). `exec` and `log` may be empty (then every
        // command fails / nothing is logged). Does nothing when not running. Never waits.
        void Service(uint64_t nowMs, const Exec& exec, const LogFn& log);

        // Cumulative counters since Start().
        struct Stats
        {
            uint64_t accepted = 0;
            uint64_t authOk = 0;
            uint64_t authFailed = 0;
            uint64_t lockedOut = 0;        // logins refused while the failure allowance was empty
            uint64_t rejectedBusy = 0;     // over maxClients
            uint64_t rejectedRemote = 0;   // peer not on 127.x
            uint64_t commands = 0;         // lines run by authenticated clients
        };
        // The live counters (a reference that stays valid for the object's lifetime).
        const Stats& stats() const;
        // How many clients are connected right now.
        size_t clients() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // True when `a` and `b` are equal, in time that depends on their lengths only (no
    // early exit at the first differing byte), for comparing secrets. Total, pure,
    // thread-safe.
    bool ConstantTimeEquals(const std::string& a, const std::string& b);

}
