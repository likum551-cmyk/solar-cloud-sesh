// A minimal plain-HTTP/1.1 client: one POST per connection, `Connection: close`,
// bounded in time and size, no TLS and no redirects. It exists for one job, the
// server's announce to the master list (apps/master/README.md, POST /announce); the
// master is meant to sit behind a plain-HTTP listener the game server can reach
// (no TLS here by design: the announce carries nothing secret and the master
// never trusts its body for the address). C++17, no exceptions; runs on the
// announcer's own thread, never on the tick loop.
//
// Thread-safety: both functions are re-entrant (no shared state besides the
// reference-counted socket-library init), so concurrent calls from different threads
// are fine; one HttpResponse/HttpOptions must not be shared between threads.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace gage::server {

    // A parsed `http://` URL, split the way a request needs it.
    struct HttpUrl
    {
        std::string host; // without brackets for an IPv6 literal
        uint16_t port = 80;
        std::string path = "/"; // always starts with '/'; no query, no fragment
        std::string hostHeader; // "host", "host:port" or "[v6]:port", as sent in Host:
    };

    // Parses `http://host[:port][/path]` into `out` (the scheme is case-insensitive;
    // the port defaults to 80, the path to "/"; hostHeader omits port 80).
    // Returns true on success. Returns false with a one-line reason in *err (when
    // non-null), leaving `out` untouched, for: https (no TLS support, the reason says so),
    // any other scheme, credentials, a query string or fragment, an empty host, a bad
    // or zero port, control characters, whitespace or non-ASCII bytes, and anything over
    // 256 bytes. No network access happens here, so a name is not resolved.
    bool ParseHttpUrl(const std::string& url, HttpUrl& out, std::string* err);

    // What HttpPost read back.
    struct HttpResponse
    {
        int status = 0;
        std::string body;           // de-chunked, at most kHttpMaxBody bytes
        int retryAfterSeconds = -1; // from Retry-After when it is an integer, else -1
    };

    // Time limits and the abort switch of one HttpPost.
    struct HttpOptions
    {
        int connectTimeoutMs = 5000;                // TCP connect (capped at totalTimeoutMs)
        int totalTimeoutMs = 10000;                 // the whole request, resolve excluded
        const std::atomic<bool>* cancel = nullptr;  // set to true to abort within ~100 ms
    };

    // Largest response body HttpPost will accept; a longer one is a failure.
    constexpr size_t kHttpMaxBody = 64 * 1024;

    // POSTs `body` to url.path on url.host:url.port as `contentType` (headers sent:
    // Host, User-Agent: gage-server, Accept: application/json, Content-Type,
    // Content-Length, Connection: close) and reads one response. Any status is a
    // success of the transport: a 404 or 500 returns true with that status in `out`
    // (redirects are not followed; a 3xx is just a status). Bodies framed by
    // Content-Length, by chunked encoding, or by the end of the connection are read;
    // 1xx, 204 and 304 responses carry no body. Returns false with a one-line reason
    // in *err (when non-null) on a transport or framing failure: cannot resolve or
    // connect, send failed, timed out (sending or waiting for the response), the
    // connection closed before a head / inside a chunked body / short of its
    // Content-Length, a malformed head or chunk, a head over 16 KB, a body over
    // kHttpMaxBody, or cancelled via opt.cancel. `out` is only written on success.
    // Blocking: it returns after at most about totalTimeoutMs (plus the name lookup,
    // which cannot be interrupted); call it from a worker thread, never the tick loop.
    bool HttpPost(const HttpUrl& url, const std::string& contentType, const std::string& body,
                  const HttpOptions& opt, HttpResponse& out, std::string* err);

}
