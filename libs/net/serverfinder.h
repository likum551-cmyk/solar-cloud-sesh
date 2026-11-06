#pragma once
#include "common/common.h"
#include <functional>
#include <utility>

namespace gage::net {

    // The built-in server list: GET /servers of the GAGE master server (apps/master/),
    // a JSON object keyed by "host:port" (the shape ListServers documents below).
    // Until the master has a public domain this is the master's own default
    // listen address (apps/master/README.md: HOST 127.0.0.1, PORT 8080), i.e. a master
    // running on this PC. Point a launcher at a deployed master with `list_url`
    // in gage_mp.cfg.
    inline constexpr const wchar_t* kDefaultListUrl = L"http://127.0.0.1:8080/servers";

    struct ServerInfo
    {
        std::wstring host;    // "1.2.3.4" or "ge1.nexus-rp.net"
        int          port = 0;
        std::wstring name;    // display label
        std::wstring url;     // the list entry's "url" field (a ws:// connect URL
                              // or the server's website), empty if none supplied
        int          players = -1; // current player count (-1 = unknown, per
                                   // apps/launcher/BRIDGE.md; 0 is a real, empty server)
        int          ping = -1;   // TCP connect time in ms (-1 = not measured)
        int          maxPlayers = -1; // slots (-1 = unknown)
        std::wstring mode;    // game mode label, e.g. "roleplay" ("" = unknown)
        std::wstring lang;    // language code, e.g. "ru" ("" = unknown)
    };

    // Fetch a server list from `endpoint` and pick the best server (the first
    // valid entry). On success returns true and fills `out`. `errorOut`
    // (optional) carries a short reason on failure and is left untouched on
    // success. Pure WinHTTP (no libcurl dependency).
    bool FindServer(const std::wstring& endpoint, ServerInfo& out, std::wstring* errorOut);

    // Fetch a server list from `endpoint` and return every valid entry it
    // contains (at most 2000; the body is capped at 4 MiB and the whole fetch
    // at ~15 s). Accepted JSON shapes:
    //   * a top-level array of server objects;
    //   * an object holding such an array (or an "ip:port"-keyed object) under
    //     "servers", "list", "result" or "data";
    //   * an object keyed by "ip:port" whose values are server objects (the
    //     master server's GET /servers shape);
    //   * a single server object.
    // A server object supplies the host as "ip"/"host"/"address" (or from its
    // "ip:port" key / a "host:port" or ws:// "url"), an optional "port" (number
    // or numeric string, 1..65535; an invalid one drops the entry; absent =>
    // gage::proto::kDefaultPort, 24800), "name"/"label", "url", and "players"
    // (absent/invalid => -1).
    // On success returns true and appends to `out`. `errorOut` (optional) is
    // set on failure only.
    bool ListServers(const std::wstring& endpoint, std::vector<ServerInfo>& out, std::wstring* errorOut);

    // Parse a direct server spec: "host:port", "ip:port", "host",
    // "[v6]:port", optionally prefixed with "ws://" / "wss://". Returns true
    // and fills `out` (host, port, url) when `spec` is a direct host/port;
    // returns false when it should be treated as a JSON-list endpoint instead
    // (i.e. http:// or https:// URLs).
    bool ParseDirect(const std::wstring& spec, ServerInfo& out);

    // Convenience: accept either a direct host:port spec or a JSON-list
    // endpoint. Direct specs (bare host:port, ws://, wss://) are used as-is
    // with no network lookup; http(s):// URLs are fetched. Falls back to a
    // loopback dev server. Always returns a usable ServerInfo (last resort
    // 127.0.0.1:24800).
    ServerInfo ResolveServer(const std::wstring& configured);

    // A game server's round trip, for the server list. Game ports are UDP, so
    // a TCP connect to them is refused or dropped; this races a TCP connect to
    // the game port and to game port + 1 in one pass and returns the first
    // reply in ms (without waiting out the port that stays silent), or -1 if
    // neither answers within `timeoutMs`. A server that really listens on TCP
    // at its game port (a ws:// direct connect) answers there.
    // NOTE: a GAGE server (ENet over UDP, ENGINE-PLAN.md) listens on neither,
    // so until the UDP ping of phase P2 replaces this the list shows no ping
    // for GAGE servers.
    // A reply slowed by a dropped first SYN (anti-DDoS front ends) is asked
    // again, or reported less Windows' 1 s retransmit wait.
    int PingServer(const std::wstring& host, int port, int timeoutMs);

    struct PingTarget
    {
        std::wstring host;
        int          port = 0;
        bool         server = true; // PingServer semantics; false = a TCP connect to this port only
    };
    // (target index, ms or -1) pairs, in completion order
    using PingBatchFn = std::function<void(const std::vector<std::pair<int, int>>&)>;

    // Ping every target at once (up to 128 in flight) on the calling thread,
    // each with its own `timeoutMs` deadline, and hand results to `onBatch` as
    // they arrive (at most every 40 ms, and once at the end), so the whole
    // list takes about one deadline. `cancelled` (may be empty) is polled
    // between waits; once it returns true the call returns promptly and the
    // targets still in flight get no result. PingServer is this
    // engine with one target.
    void PingMany(const std::vector<PingTarget>& targets, int timeoutMs,
                  const std::function<bool()>& cancelled, const PingBatchFn& onBatch);

    // The result of a GAGE UDP server query (protocol/query.h): the live player counts a GAGE
    // server reports and, from the round trip, its ping. All -1 until a valid reply is read.
    struct QueryResult
    {
        int ms = -1;          // round-trip time in ms (-1 = no reply within the deadline)
        int players = -1;     // joined players right now (-1 = unknown)
        int maxPlayers = -1;  // slots (-1 = unknown)
        int protocol = -1;    // the game protocol version the server enforces (-1 = unknown)
        // The newest client release the server was released with (query format v2, "update ping"), packed as
        // major<<16 | minor<<8 | patch (proto::query::ClientVersion::Packed). 0 = unknown: a server that predates
        // v2 (it answers v1), or one with no latest client configured. Never a reason to block anything.
        int latestClient = 0;
    };

    // Send one GAGE UDP query to host:port and wait up to `timeoutMs` for the matching reply
    // (the reply's nonce must echo the request's). The request asks for the v2 reply; a server that
    // predates v2 ignores that and answers v1, which is accepted (latestClient stays 0). Returns true
    // and fills `out` on a valid reply, false on timeout, resolve failure or socket error. `host` may be an IP or a hostname. This
    // is the UDP equivalent of PingServer for GAGE servers (ENet/UDP), which a TCP connect cannot
    // reach; it is independent of the TCP Ping/PingServer/PingMany path above.
    bool QueryServer(const std::wstring& host, int port, int timeoutMs, QueryResult& out);

    struct HttpGetOptions
    {
        unsigned long long maxBytes  = 4ull * 1024ull * 1024ull; // hard cap on the body (declared or actual)
        unsigned long long overallMs = 15000;                    // wall-clock budget for the whole fetch
    };

    // The WinHTTP GET behind every fetch above, for http:// and https:// URLs (WinHTTP
    // validates TLS certificates; redirects are followed except https -> http). The body
    // is streamed to `sink` in chunks of at most 64 KiB; `sink` returns false to stop the
    // transfer. Succeeds only on a 2xx response read to a clean EOF within `opts`: a
    // Content-Length above maxBytes, a body that grows past it, a truncated body, a stall
    // (8 s without data) or an exhausted overallMs all fail. `progress` (may be empty) gets
    // (bytes so far, Content-Length or 0) after each chunk. On failure returns false and sets
    // `errorOut` (optional); success leaves it untouched. Any bytes already handed to `sink`
    // stay with the caller.
    bool HttpGet(const std::wstring& url, const HttpGetOptions& opts,
                 const std::function<bool(const char*, size_t)>& sink,
                 const std::function<void(unsigned long long, unsigned long long)>& progress,
                 std::wstring* errorOut);

    // Fetch the remote client version string from `url` (expects a JSON body
    // with a "version" string field, e.g. {"version":"1.2.3"}, at the top level
    // or one object level down). Returns the version on success, L"" on
    // failure. Pure WinHTTP.
    std::wstring FetchVersion(const std::wstring& url, std::wstring* errorOut);

}
