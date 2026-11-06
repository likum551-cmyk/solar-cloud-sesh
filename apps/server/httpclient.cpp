#include "httpclient.h"

#include "sock.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace gage::server {

    namespace {

        uint64_t NowMs()
        {
            using namespace std::chrono;
            return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        std::string Lower(std::string s)
        {
            for (char& c : s)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            return s;
        }

        bool StartsWithNoCase(const std::string& s, const char* prefix)
        {
            const size_t n = std::strlen(prefix);
            return s.size() >= n && Lower(s.substr(0, n)) == prefix;
        }

        // Parsed response head.
        struct Head
        {
            int status = 0;
            long contentLength = -1;
            bool chunked = false;
            int retryAfter = -1;
        };

        // Reads the status line and the headers of `head` (text before the blank line).
        bool ParseHead(const std::string& head, Head& out)
        {
            size_t eol = head.find("\r\n");
            const std::string status = head.substr(0, eol);
            // "HTTP/1.1 200 OK"
            if (status.size() < 12 || status.compare(0, 5, "HTTP/") != 0 || status[6] != '.' || status[8] != ' ')
                return false;
            if (status[5] != '1') return false;
            const char* d = status.c_str() + 9;
            if (d[0] < '0' || d[0] > '9' || d[1] < '0' || d[1] > '9' || d[2] < '0' || d[2] > '9') return false;
            out.status = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
            if (out.status < 100) return false;

            size_t pos = eol == std::string::npos ? head.size() : eol + 2;
            while (pos < head.size())
            {
                size_t e = head.find("\r\n", pos);
                if (e == std::string::npos) e = head.size();
                const std::string line = head.substr(pos, e - pos);
                pos = e + 2;
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                const std::string name = Lower(line.substr(0, colon));
                size_t vs = colon + 1;
                while (vs < line.size() && (line[vs] == ' ' || line[vs] == '\t')) vs++;
                const std::string value = line.substr(vs);
                if (name == "content-length")
                {
                    if (value.empty() || value.size() > 9) return false;
                    long v = 0;
                    for (char c : value)
                    {
                        if (c < '0' || c > '9') return false;
                        v = v * 10 + (c - '0');
                    }
                    out.contentLength = v;
                }
                else if (name == "transfer-encoding")
                {
                    if (Lower(value).find("chunked") != std::string::npos) out.chunked = true;
                }
                else if (name == "retry-after")
                {
                    bool digits = !value.empty() && value.size() <= 7;
                    long v = 0;
                    for (char c : value)
                    {
                        if (c < '0' || c > '9') { digits = false; break; }
                        v = v * 10 + (c - '0');
                    }
                    if (digits) out.retryAfter = (int)v;
                }
            }
            return true;
        }

        enum class Chunks { Need, Done, Bad };

        // Decodes a chunked body from `in` (everything after the blank line).
        Chunks DecodeChunked(const std::string& in, std::string& body)
        {
            body.clear();
            size_t pos = 0;
            for (;;)
            {
                const size_t eol = in.find("\r\n", pos);
                if (eol == std::string::npos) return in.size() - pos > 24 ? Chunks::Bad : Chunks::Need;
                size_t size = 0;
                size_t i = pos;
                int digits = 0;
                for (; i < eol && in[i] != ';'; i++)
                {
                    const char c = in[i];
                    int v;
                    if (c >= '0' && c <= '9') v = c - '0';
                    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                    else return Chunks::Bad;
                    size = size * 16 + (size_t)v;
                    if (++digits > 8) return Chunks::Bad;
                }
                if (digits == 0) return Chunks::Bad;
                pos = eol + 2;
                if (size == 0)
                {
                    // Optional trailers, then the empty line.
                    return in.find("\r\n", pos) == std::string::npos ? Chunks::Need : Chunks::Done;
                }
                if (body.size() + size > kHttpMaxBody) return Chunks::Bad;
                if (in.size() - pos < size + 2) return Chunks::Need;
                body.append(in, pos, size);
                pos += size;
                if (in.compare(pos, 2, "\r\n") != 0) return Chunks::Bad;
                pos += 2;
            }
        }

    }

    bool ParseHttpUrl(const std::string& url, HttpUrl& out, std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        if (url.empty()) return fail("empty URL");
        if (url.size() > 256) return fail("URL longer than 256 bytes");
        for (char ch : url)
        {
            const unsigned char c = (unsigned char)ch;
            if (c <= 0x20 || c >= 0x7F) return fail("URL must be printable ASCII without spaces (use punycode for names)");
        }
        if (StartsWithNoCase(url, "https://"))
            return fail("https is not supported (the server speaks plain HTTP only); use an http:// address of the master");
        if (!StartsWithNoCase(url, "http://")) return fail("URL must start with http://");
        const std::string rest = url.substr(7);
        if (rest.find('?') != std::string::npos || rest.find('#') != std::string::npos)
            return fail("URL must not have a query or a fragment");
        const size_t slash = rest.find('/');
        const std::string authority = rest.substr(0, slash);
        std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
        if (authority.empty()) return fail("URL has no host");
        if (authority.find('@') != std::string::npos) return fail("URL must not contain credentials");

        std::string host, portText;
        bool v6 = false;
        if (authority[0] == '[')
        {
            const size_t close = authority.find(']');
            if (close == std::string::npos) return fail("unterminated [ in URL host");
            host = authority.substr(1, close - 1);
            v6 = true;
            const std::string after = authority.substr(close + 1);
            if (!after.empty())
            {
                if (after[0] != ':') return fail("junk after the ] of the URL host");
                portText = after.substr(1);
                if (portText.empty()) return fail("empty port in URL");
            }
            for (char c : host)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.'))
                    return fail("bad IPv6 literal in URL");
        }
        else
        {
            const size_t colon = authority.rfind(':');
            if (colon != std::string::npos)
            {
                host = authority.substr(0, colon);
                portText = authority.substr(colon + 1);
                if (portText.empty()) return fail("empty port in URL");
            }
            else
                host = authority;
            for (char c : host)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.' ||
                      c == '_'))
                    return fail("URL host has an invalid character");
        }
        if (host.empty()) return fail("URL has no host");

        uint32_t port = 80;
        if (!portText.empty())
        {
            port = 0;
            for (char c : portText)
            {
                if (c < '0' || c > '9') return fail("URL port must be a number");
                port = port * 10 + (uint32_t)(c - '0');
                if (port > 65535) return fail("URL port must be 1..65535");
            }
            if (port == 0) return fail("URL port must be 1..65535");
        }

        out.host = host;
        out.port = (uint16_t)port;
        out.path = path;
        out.hostHeader = (v6 ? "[" + host + "]" : host) + (port != 80 ? ":" + std::to_string(port) : std::string());
        return true;
    }

    bool HttpPost(const HttpUrl& url, const std::string& contentType, const std::string& body, const HttpOptions& opt,
                  HttpResponse& out, std::string* err)
    {
        auto failMsg = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        // Extra headers first: a line break in a value would let it inject headers (or a second request).
        std::string extra;
        for (const auto& h : opt.headers)
        {
            if (h.first.empty() || h.first.size() > 64) return failMsg("bad extra header name");
            for (char c : h.first)
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                    return failMsg("bad extra header name");
            for (char ch : h.second)
            {
                const unsigned char c = (unsigned char)ch;
                if ((c < 0x20 && c != '\t') || c == 0x7F) return failMsg("extra header " + h.first + " has a control character");
            }
            extra += h.first + ": " + h.second + "\r\n";
        }
        if (!sock::Init()) return failMsg("network stack unavailable");
        struct Cleanup
        {
            ~Cleanup() { sock::Cleanup(); }
        } cleanup;

        const uint64_t start = NowMs();
        const uint64_t deadline = start + (uint64_t)(opt.totalTimeoutMs < 1 ? 1 : opt.totalTimeoutMs);
        auto remaining = [&]() -> int {
            const uint64_t now = NowMs();
            return now >= deadline ? 0 : (int)(deadline - now);
        };
        auto cancelled = [&]() { return opt.cancel && opt.cancel->load(); };

        std::string cerr;
        const int connectMs = opt.connectTimeoutMs < opt.totalTimeoutMs ? opt.connectTimeoutMs : opt.totalTimeoutMs;
        const sock::Handle h = sock::ConnectTcp(url.host, url.port, connectMs, opt.cancel, &cerr);
        if (h == sock::kInvalid) return failMsg(cerr);
        struct Closer
        {
            sock::Handle h;
            ~Closer() { sock::Close(h); }
        } closer{h};

        std::string req = "POST " + url.path + " HTTP/1.1\r\nHost: " + url.hostHeader +
                          "\r\nUser-Agent: gage-server\r\nAccept: application/json\r\nContent-Type: " + contentType +
                          "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body;

        size_t sent = 0;
        while (sent < req.size())
        {
            if (cancelled()) return failMsg("cancelled");
            if (remaining() == 0) return failMsg("timed out sending the request");
            size_t n = 0;
            const sock::Io r = sock::Send(h, req.data() + sent, req.size() - sent, &n);
            if (r == sock::Io::Ok)
                sent += n;
            else if (r == sock::Io::WouldBlock)
            {
                const int slice = remaining() < 100 ? remaining() : 100;
                if (sock::Wait(h, true, slice) < 0) return failMsg("send failed");
            }
            else
                return failMsg("send failed (connection closed by the master)");
        }

        std::string raw;
        Head head;
        bool haveHead = false;
        size_t bodyAt = 0;
        constexpr size_t kMaxHead = 16 * 1024;
        char buf[4096];
        for (;;)
        {
            // Complete already? (decided from what is in `raw` before reading more)
            if (haveHead)
            {
                if (head.chunked)
                {
                    std::string decoded;
                    const Chunks c = DecodeChunked(raw.substr(bodyAt), decoded);
                    if (c == Chunks::Bad) return failMsg("malformed chunked response");
                    if (c == Chunks::Done)
                    {
                        out.status = head.status;
                        out.body = std::move(decoded);
                        out.retryAfterSeconds = head.retryAfter;
                        return true;
                    }
                }
                else if (head.contentLength >= 0 && raw.size() - bodyAt >= (size_t)head.contentLength)
                {
                    out.status = head.status;
                    out.body = raw.substr(bodyAt, (size_t)head.contentLength);
                    out.retryAfterSeconds = head.retryAfter;
                    return true;
                }
            }

            if (cancelled()) return failMsg("cancelled");
            if (remaining() == 0) return failMsg("timed out waiting for the response");
            const int slice = remaining() < 100 ? remaining() : 100;
            const int w = sock::Wait(h, false, slice);
            if (w < 0) return failMsg("receive failed");
            if (w == 0) continue;
            size_t n = 0;
            const sock::Io r = sock::Recv(h, buf, sizeof(buf), &n);
            if (r == sock::Io::WouldBlock) continue;
            if (r == sock::Io::Error) return failMsg("receive failed");
            if (r == sock::Io::Closed)
            {
                if (!haveHead) return failMsg("connection closed before a response head");
                if (head.chunked) return failMsg("connection closed inside a chunked response");
                if (head.contentLength >= 0) return failMsg("response body truncated");
                // Neither length nor chunks: the body runs to EOF.
                out.status = head.status;
                out.body = raw.substr(bodyAt);
                out.retryAfterSeconds = head.retryAfter;
                return true;
            }
            raw.append(buf, n);
            if (!haveHead)
            {
                const size_t end = raw.find("\r\n\r\n");
                if (end == std::string::npos)
                {
                    if (raw.size() > kMaxHead) return failMsg("response head too large");
                }
                else
                {
                    if (!ParseHead(raw.substr(0, end), head)) return failMsg("malformed HTTP response head");
                    if (head.contentLength > (long)kHttpMaxBody) return failMsg("response body too large");
                    haveHead = true;
                    bodyAt = end + 4;
                    if (head.status < 200 || head.status == 204 || head.status == 304)
                    {
                        out.status = head.status;
                        out.body.clear();
                        out.retryAfterSeconds = head.retryAfter;
                        return true;
                    }
                }
            }
            if (haveHead && raw.size() - bodyAt > kHttpMaxBody + 64) return failMsg("response body too large");
        }
    }

}
