#include "serverfinder.h"
#include "common/registry.h"
#include "common/logger.h"
#include "common/json.h"
#include "protocol/protocol.h"
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "winhttp")

using namespace gage;

namespace {

    // ---- limits ------------------------------------------------------------
    const DWORD     kMaxBodyBytes  = 4u * 1024u * 1024u; // response body cap
    const size_t    kMaxServers    = 2000;               // list entries kept
    const size_t    kMaxJsonNodes  = 200000;             // parser memory budget
    const ULONGLONG kOverallMs     = 15000;              // wall-clock budget per fetch
    const int       kResolveMs     = 5000;
    const int       kConnectMs     = 5000;
    const int       kSendMs        = 5000;
    const int       kReceiveMs     = 8000;               // also the response-header wait
    const int       kDefaultPort   = gage::proto::kDefaultPort; // GAGE game port (UDP)

    // ---- WinHTTP helpers ---------------------------------------------------

    // Closes the handle on every exit path (no leak when a later step fails).
    struct WinHttpHandle
    {
        HINTERNET h;
        explicit WinHttpHandle(HINTERNET x = nullptr) : h(x) {}
        ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
        WinHttpHandle(const WinHttpHandle&) = delete;
        WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    };

    const wchar_t* WinHttpErrName(DWORD code)
    {
        switch (code)
        {
        case ERROR_WINHTTP_TIMEOUT:           return L" (timed out)";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L" (name not resolved)";
        case ERROR_WINHTTP_CANNOT_CONNECT:    return L" (cannot connect)";
        case ERROR_WINHTTP_CONNECTION_ERROR:  return L" (connection error)";
        case ERROR_WINHTTP_INVALID_URL:       return L" (invalid URL)";
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return L" (unsupported scheme)";
        case 12152:                           return L" (invalid server response)"; // ERROR_WINHTTP_INVALID_SERVER_RESPONSE
        case 12175:                           return L" (TLS failure)"; // ERROR_WINHTTP_SECURE_FAILURE
        default:                              return L"";
        }
    }

    std::wstring ApiError(const wchar_t* what, DWORD code)
    {
        return std::wstring(what) + L": " + std::to_wstring(code) + WinHttpErrName(code);
    }

    // Reads a numeric response header (status code, content length).
    bool QueryNumber(HINTERNET hReq, DWORD info, DWORD& out)
    {
        DWORD v = 0, len = sizeof(v);
        if (!WinHttpQueryHeaders(hReq, info | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &v, &len,
                                 WINHTTP_NO_HEADER_INDEX))
            return false;
        out = v;
        return true;
    }

    // True if the response status is 2xx. `status` receives the real code.
    // (A NULL-buffer WinHttpQueryHeaders sizing call returns FALSE with
    // ERROR_INSUFFICIENT_BUFFER, so ask for the status as a DWORD instead.)
    bool HttpStatusOk(HINTERNET hReq, DWORD& status)
    {
        status = 0;
        if (!QueryNumber(hReq, WINHTTP_QUERY_STATUS_CODE, status))
            return false;
        return status >= 200 && status < 300;
    }

    std::wstring TrimW(const std::wstring& s)
    {
        size_t b = s.find_first_not_of(L" \t\r\n");
        if (b == std::wstring::npos)
            return L"";
        size_t e = s.find_last_not_of(L" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    std::string TrimA(const std::string& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    // ---- JSON helpers ------------------------------------------------------

    const JVal* Member(const JVal& o, const char* key)
    {
        if (o.t != JVal::Obj)
            return nullptr;
        for (size_t i = 0; i < o.o.size(); ++i)
            if (o.o[i].first == key)
                return &o.o[i].second;
        return nullptr;
    }

    // First member among `keys` that is a non-empty string (trimmed).
    bool FirstString(const JVal& o, const char* const* keys, std::string& out)
    {
        for (; *keys; ++keys)
        {
            const JVal* v = Member(o, *keys);
            if (!v || v->t != JVal::Str)
                continue;
            std::string t = TrimA(v->s);
            if (t.empty())
                continue;
            out = t;
            return true;
        }
        return false;
    }

    // Parse a whole HTTP body as JSON: optional UTF-8 BOM, exactly one value,
    // nothing but whitespace after it, bounded depth and node count.
    bool ParseBody(const std::string& body, JParser& jp)
    {
        const char* b = body.data();
        size_t n = body.size();
        if (n >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF)
        {
            b += 3;
            n -= 3;
        }
        jp.maxNodes = kMaxJsonNodes;
        if (!jp.parse(b, n))
            return false;
        jp.ws();
        return jp.p == jp.end;
    }

    // ---- host / port parsing -----------------------------------------------

    bool ParsePortText(const std::string& s, int& port)
    {
        if (s.empty() || s.size() > 5)
            return false;
        int v = 0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] < '0' || s[i] > '9')
                return false;
            v = v * 10 + (s[i] - '0');
        }
        if (v < 1 || v > 65535)
            return false;
        port = v;
        return true;
    }

    bool ValidHost(const std::string& h)
    {
        if (h.empty() || h.size() > 253)
            return false;
        for (size_t i = 0; i < h.size(); ++i)
        {
            unsigned char c = (unsigned char)h[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '.' || c == '-' || c == '_' || c == ':' || c == '%';
            if (!ok)
                return false;
        }
        return true;
    }

    // "host", "host:port", "[v6]", "[v6]:port", bare "v6" (several colons, no
    // port), each optionally as "scheme://spec/ignored/path". `hasPort` says
    // whether an explicit, valid (1..65535) port was present.
    bool SplitHostPort(const std::string& specIn, bool allowScheme,
                       std::string& host, int& port, bool& hasPort)
    {
        std::string s = TrimA(specIn);
        size_t sc = s.find("://");
        if (sc != std::string::npos)
        {
            if (!allowScheme)
                return false;
            s = s.substr(sc + 3);
        }
        size_t cut = s.find_first_of("/?#");
        if (cut != std::string::npos)
            s.resize(cut);
        if (s.empty() || s.size() > 260 || s.find('@') != std::string::npos)
            return false;

        hasPort = false;
        if (s[0] == '[')
        {
            size_t close = s.find(']');
            if (close == std::string::npos)
                return false;
            host = s.substr(1, close - 1);
            if (host.find(':') == std::string::npos)
                return false;
            std::string rest = s.substr(close + 1);
            if (!rest.empty())
            {
                if (rest[0] != ':' || !ParsePortText(rest.substr(1), port))
                    return false;
                hasPort = true;
            }
        }
        else
        {
            size_t c1 = s.find(':');
            if (c1 == std::string::npos)
                host = s;
            else if (s.find(':', c1 + 1) == std::string::npos)
            {
                host = s.substr(0, c1);
                if (!ParsePortText(s.substr(c1 + 1), port))
                    return false;
                hasPort = true;
            }
            else
                host = s; // bare IPv6 literal
        }
        return ValidHost(host);
    }

    bool EqualNoCase(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                return false;
        return true;
    }

    enum class Field { Absent, Ok, Bad };

    // A port given as an integral JSON number or a numeric string.
    Field ReadPort(const JVal* v, int& out)
    {
        if (!v || v->t == JVal::Null)
            return Field::Absent;
        if (v->t == JVal::Num)
        {
            double d = v->n;
            if (d >= 1.0 && d <= 65535.0 && d == std::floor(d))
            {
                out = (int)d;
                return Field::Ok;
            }
            return Field::Bad;
        }
        if (v->t == JVal::Str)
        {
            std::string t = TrimA(v->s);
            if (t.empty())
                return Field::Absent;
            return ParsePortText(t, out) ? Field::Ok : Field::Bad;
        }
        return Field::Bad;
    }

    // A count under the first of `keys` that holds one; anything missing or
    // unusable is -1 (unknown).
    int ReadCount(const JVal& o, const char* const* keys)
    {
        for (const char* const* k = keys; *k; ++k)
        {
            const JVal* v = Member(o, *k);
            if (!v)
                continue;
            if (v->t == JVal::Num)
            {
                if (v->n >= 0.0 && v->n <= 10000000.0)
                    return (int)std::floor(v->n + 0.5);
                continue;
            }
            if (v->t == JVal::Str)
            {
                std::string t = TrimA(v->s);
                if (t.empty() || t.size() > 8)
                    continue;
                int n = 0;
                bool ok = true;
                for (size_t i = 0; i < t.size(); ++i)
                {
                    if (t[i] < '0' || t[i] > '9') { ok = false; break; }
                    n = n * 10 + (t[i] - '0');
                }
                if (ok)
                    return n;
            }
        }
        return -1;
    }

    int ReadPlayers(const JVal& o)
    {
        static const char* const keys[] = { "players", "player_count", "current", "numPlayers", nullptr };
        return ReadCount(o, keys);
    }

    // Slots; the GAGE master list calls them "maxplayers".
    int ReadMaxPlayers(const JVal& o)
    {
        static const char* const keys[] = { "maxplayers", "max_players", "maxPlayers", "slots", nullptr };
        return ReadCount(o, keys);
    }

    // A short label (game mode, language code): trimmed, empty when missing
    // or longer than `cap` bytes (a list entry is untrusted input).
    std::wstring ReadLabel(const JVal& o, const char* const* keys, size_t cap)
    {
        std::string s;
        if (!FirstString(o, keys, s))
            return L"";
        s = TrimA(s);
        return s.size() > cap ? L"" : gage::Widen(s);
    }

    // Fill `out` from one server object. `key` is the object's member name
    // when it came out of an "ip:port"-keyed map (the master list).
    bool ParseServerEntry(const JVal& o, const std::string* key, gage::net::ServerInfo& out)
    {
        if (o.t != JVal::Obj)
            return false;

        std::string host;
        int port = 0;
        bool haveHost = false, havePort = false;

        // 1) explicit host fields (may carry an embedded ":port").
        static const char* const hostKeys[] = { "ip", "host", "address", "server", nullptr };
        for (const char* const* k = hostKeys; *k && !haveHost; ++k)
        {
            const JVal* v = Member(o, *k);
            if (!v || v->t != JVal::Str)
                continue;
            std::string h;
            int p = 0;
            bool hp = false;
            if (!SplitHostPort(v->s, false, h, p, hp))
                continue;
            host = h;
            haveHost = true;
            if (hp)
            {
                port = p;
                havePort = true;
            }
        }

        // 2) explicit port field; present-but-invalid drops the entry.
        static const char* const portKeys[] = { "port", "port2", nullptr };
        for (const char* const* k = portKeys; *k; ++k)
        {
            int p = 0;
            Field f = ReadPort(Member(o, *k), p);
            if (f == Field::Absent)
                continue;
            if (f == Field::Bad)
                return false;
            port = p;
            havePort = true;
            break;
        }

        // 3) fall back to the map key, then a ws:// / host:port "url", for
        //    whatever is still missing. They need an explicit port to count.
        auto fallback = [&](const std::string& spec, bool allowScheme)
        {
            std::string h;
            int p = 0;
            bool hp = false;
            if (!SplitHostPort(spec, allowScheme, h, p, hp) || !hp)
                return;
            if (!haveHost)
            {
                host = h;
                haveHost = true;
                if (!havePort)
                {
                    port = p;
                    havePort = true;
                }
            }
            else if (!havePort && EqualNoCase(h, host))
            {
                port = p;
                havePort = true;
            }
        };
        if (key)
            fallback(*key, false);
        std::string urlText;
        const JVal* uv = Member(o, "url");
        if (uv && uv->t == JVal::Str)
        {
            urlText = TrimA(uv->s);
            if (urlText.size() > 2048)
                urlText.clear();
            // http(s):// is the server's website in the master list, not an
            // address to connect to.
            size_t sc = urlText.find("://");
            bool connectable = sc == std::string::npos;
            if (!connectable)
            {
                std::string scheme = urlText.substr(0, sc);
                connectable = EqualNoCase(scheme, "ws") || EqualNoCase(scheme, "wss");
            }
            if (connectable && !urlText.empty())
                fallback(urlText, true);
        }

        if (!haveHost)
            return false;
        if (!havePort)
            port = kDefaultPort;

        out.host = gage::Widen(host);
        out.port = port;

        static const char* const nameKeys[] = { "name", "label", nullptr };
        std::string name;
        if (FirstString(o, nameKeys, name))
            out.name = gage::Widen(name);
        if (out.name.empty())
            out.name = out.host + L":" + std::to_wstring(port);

        out.url = gage::Widen(urlText);
        out.players = ReadPlayers(o);
        out.maxPlayers = ReadMaxPlayers(o);
        static const char* const modeKeys[] = { "gamemode", "mode", nullptr };
        static const char* const langKeys[] = { "lang", "language", nullptr };
        out.mode = ReadLabel(o, modeKeys, 32);
        out.lang = ReadLabel(o, langKeys, 8);
        return !out.host.empty();
    }

    // Gathers valid, de-duplicated servers (capped at kMaxServers).
    struct Collector
    {
        std::vector<gage::net::ServerInfo> servers;
        std::unordered_set<std::string> seen;
        size_t dropped = 0;
        bool full = false;

        // `probe` = "is this object itself a server?" (a failure is not a drop).
        bool add(const JVal& o, const std::string* key, bool probe = false)
        {
            if (full)
                return false;
            gage::net::ServerInfo info;
            if (!ParseServerEntry(o, key, info))
            {
                if (!probe)
                    dropped++;
                return false;
            }
            std::string id = gage::Narrow(info.host);
            std::transform(id.begin(), id.end(), id.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            id += ":" + std::to_string(info.port);
            if (!seen.insert(id).second)
                return false;
            if (servers.size() >= kMaxServers)
            {
                full = true;
                return false;
            }
            servers.push_back(info);
            return true;
        }
    };

    void CollectServers(const JVal& v, Collector& c, int level)
    {
        if (c.full)
            return;
        if (v.t == JVal::Arr)
        {
            for (size_t i = 0; i < v.a.size() && !c.full; ++i)
                if (v.a[i].t == JVal::Obj)
                    c.add(v.a[i], nullptr);
            return;
        }
        if (v.t != JVal::Obj)
            return;

        // A server object itself (single-server body).
        if (c.add(v, nullptr, true))
            return;

        // A wrapper: {"servers":[...]} / {"result":{...}} / ...
        if (level < 3)
        {
            static const char* const wrappers[] = { "servers", "list", "result", "data", nullptr };
            for (const char* const* w = wrappers; *w; ++w)
            {
                const JVal* m = Member(v, *w);
                if (!m || (m->t != JVal::Arr && m->t != JVal::Obj))
                    continue;
                size_t before = c.servers.size();
                CollectServers(*m, c, level + 1);
                if (c.servers.size() > before)
                    return;
            }
        }

        // The GAGE master list: {"1.2.3.4:24800": {...server...}, ...}
        for (size_t i = 0; i < v.o.size() && !c.full; ++i)
            if (v.o[i].second.t == JVal::Obj)
                c.add(v.o[i].second, &v.o[i].first);
    }

    // body -> servers. On failure `err` says why.
    bool ParseServerList(const std::string& body, std::vector<gage::net::ServerInfo>& servers,
                         std::wstring& err)
    {
        JParser jp;
        if (!ParseBody(body, jp))
        {
            err = L"invalid JSON in server list";
            return false;
        }
        Collector c;
        CollectServers(jp.v, c, 0);
        if (c.full)
            GLOGW("server list: more than %zu entries, extras ignored", kMaxServers);
        if (c.dropped)
            GLOGW("server list: %zu invalid entr(ies) skipped", c.dropped);
        if (c.servers.empty())
        {
            err = L"no servers in list";
            return false;
        }
        servers.swap(c.servers);
        return true;
    }

}

namespace gage::net {

    bool ParseDirect(const std::wstring& specIn, ServerInfo& out)
    {
        // Trim surrounding whitespace.
        std::wstring spec = specIn;
        size_t b = spec.find_first_not_of(L" \t\r\n");
        if (b == std::wstring::npos)
            return false;
        size_t e = spec.find_last_not_of(L" \t\r\n");
        spec = spec.substr(b, e - b + 1);

        // Split off any scheme.
        size_t schemeAt = spec.find(L"://");
        std::wstring scheme;
        if (schemeAt != std::wstring::npos)
        {
            scheme = spec.substr(0, schemeAt);
            spec   = spec.substr(schemeAt + 3);
            std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::towlower);
            if (scheme == L"http" || scheme == L"https")
                return false; // JSON-list endpoint, not a direct host:port
        }

        // Drop any trailing path and query: keep "host[:port][/path...]" -> "host[:port]".
        size_t slash = spec.find(L'/');
        if (slash != std::wstring::npos)
            spec = spec.substr(0, slash);

        // IPv6 literal: [::1]:24800
        std::wstring host;
        int port = kDefaultPort;
        if (!spec.empty() && spec[0] == L'[')
        {
            size_t close = spec.find(L']');
            if (close == std::wstring::npos)
                return false;
            host = spec.substr(1, close - 1);
            if (close + 1 < spec.size() && spec[close + 1] == L':')
            {
                size_t pe = spec.find(L'/', close + 1);
                std::wstring ps = spec.substr(close + 2, pe == std::wstring::npos ? std::wstring::npos : pe - (close + 2));
                if (ps.empty())
                    return false;
                int p = (int)std::strtol(gage::Narrow(ps).c_str(), nullptr, 10);
                if (p < 1 || p > 65535)
                    return false;
                port = p;
            }
        }
        else
        {
            size_t colon = spec.find_last_of(L':');
            if (colon != std::wstring::npos)
            {
                std::wstring ps = spec.substr(colon + 1);
                if (ps.empty())
                    return false;
                int p = (int)std::strtol(gage::Narrow(ps).c_str(), nullptr, 10);
                if (p < 1 || p > 65535)
                    return false;
                port = p;
                host = spec.substr(0, colon);
            }
            else
            {
                host = spec;
            }
        }

        if (host.empty())
            return false;

        out.host = host;
        out.port = port;
        if (scheme == L"ws" || scheme == L"wss")
        {
            // Canonical url with the explicit port always present.
            out.url = scheme + L"://" + host + L":" + std::to_wstring(port);
        }
        out.name = host + L":" + std::to_wstring(port);
        return true;
    }

    // ---- shared WinHTTP GET ----------------------------------------------
    // "4 MiB" / "512 B" / "15 s" wording for the limit messages.
    static std::wstring ByteLimitText(unsigned long long n)
    {
        if (n >= 1024ull * 1024ull && n % (1024ull * 1024ull) == 0)
            return std::to_wstring(n / (1024ull * 1024ull)) + L" MiB";
        return std::to_wstring(n) + L" B";
    }

    // Fetch http(s) `endpoint`, streaming the body to `sink`. Succeeds only on a
    // 2xx response whose body was read to a clean EOF within opts.maxBytes and
    // the opts.overallMs wall-clock budget; otherwise returns false and sets
    // `errorOut` (never touched on success).
    bool HttpGet(const std::wstring& endpointIn, const HttpGetOptions& opts,
                 const std::function<bool(const char*, size_t)>& sink,
                 const std::function<void(unsigned long long, unsigned long long)>& progress,
                 std::wstring* errorOut)
    {
        auto fail = [&](const std::wstring& why)
        {
            if (errorOut)
                *errorOut = why;
            return false;
        };

        const ULONGLONG deadline = GetTickCount64() + opts.overallMs;

        std::wstring endpoint = TrimW(endpointIn);
        if (endpoint.empty())
            return fail(L"empty endpoint");
        if (endpoint.size() > 2048)
            return fail(L"endpoint too long");
        if (endpoint.find(L"://") == std::wstring::npos)
            endpoint = L"https://" + endpoint; // bare "host/path" has always meant https

        // Split the URL with WinHTTP itself (scheme case-insensitive, port range checked).
        URL_COMPONENTS uc;
        ZeroMemory(&uc, sizeof(uc));
        uc.dwStructSize      = sizeof(uc);
        uc.dwSchemeLength    = (DWORD)-1;
        uc.dwHostNameLength  = (DWORD)-1;
        uc.dwUrlPathLength   = (DWORD)-1;
        uc.dwExtraInfoLength = (DWORD)-1;
        if (!WinHttpCrackUrl(endpoint.c_str(), (DWORD)endpoint.size(), 0, &uc))
            return fail(ApiError(L"invalid URL", GetLastError()));
        if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)
            return fail(L"unsupported URL scheme (need http or https)");
        if (!uc.lpszHostName || uc.dwHostNameLength == 0)
            return fail(L"invalid URL: no host");
        if (uc.nPort == 0)
            return fail(L"invalid URL: bad port");
        const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
        std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring path;
        if (uc.lpszUrlPath && uc.dwUrlPathLength)
            path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
        if (uc.lpszExtraInfo && uc.dwExtraInfoLength)
        {
            std::wstring extra(uc.lpszExtraInfo, uc.dwExtraInfoLength);
            size_t frag = extra.find(L'#'); // fragments are never sent
            if (frag != std::wstring::npos)
                extra.resize(frag);
            path += extra;
        }
        if (path.empty() || path[0] != L'/')
            path = L"/" + path;

        WinHttpHandle session(WinHttpOpen(L"solar-rp/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.h) // pre-8.1 WinHTTP rejects automatic proxy
            session.h = WinHttpOpen(L"solar-rp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.h)
            return fail(ApiError(L"WinHttpOpen failed", GetLastError()));
        if (!WinHttpSetTimeouts(session.h, kResolveMs, kConnectMs, kSendMs, kReceiveMs))
            return fail(ApiError(L"WinHttpSetTimeouts failed", GetLastError()));

        WinHttpHandle conn(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
        if (!conn.h)
            return fail(ApiError(L"connect failed", GetLastError()));

        WinHttpHandle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
        if (!req.h)
            return fail(ApiError(L"request failed", GetLastError()));

        DWORD respTimeout = (DWORD)kReceiveMs;
        WinHttpSetOption(req.h, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &respTimeout, sizeof(respTimeout));

        const std::wstring hdrs = L"Accept: application/json\r\n";
        if (!WinHttpAddRequestHeaders(req.h, hdrs.c_str(), (DWORD)hdrs.size(), WINHTTP_ADDREQ_FLAG_ADD))
            return fail(ApiError(L"request failed", GetLastError())); // req closes via RAII

        if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(req.h, nullptr))
            return fail(ApiError(L"request failed", GetLastError()));

        DWORD status = 0;
        if (!HttpStatusOk(req.h, status))
        {
            if (status == 0)
                return fail(ApiError(L"no HTTP status", GetLastError()));
            return fail(L"HTTP " + std::to_wstring(status));
        }

        DWORD contentLength = 0;
        const bool haveLength = QueryNumber(req.h, WINHTTP_QUERY_CONTENT_LENGTH, contentLength);
        if (haveLength && (unsigned long long)contentLength > opts.maxBytes)
            return fail(L"response too large (limit " + ByteLimitText(opts.maxBytes) + L")");

        std::vector<char> chunk(64 * 1024);
        unsigned long long total = 0; // total <= opts.maxBytes always
        for (;;)
        {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                return fail(L"timed out (" + std::to_wstring(opts.overallMs / 1000) + L" s limit)");
            // Never let one blocked read outlive the overall budget.
            DWORD wait = (DWORD)std::min<ULONGLONG>(deadline - now, (ULONGLONG)kReceiveMs);
            WinHttpSetOption(req.h, WINHTTP_OPTION_RECEIVE_TIMEOUT, &wait, sizeof(wait));

            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.h, &avail))
                return fail(ApiError(L"read failed", GetLastError()));
            if (avail == 0)
                break; // clean EOF
            if ((unsigned long long)avail > opts.maxBytes - total)
                return fail(L"response too large (limit " + ByteLimitText(opts.maxBytes) + L")");

            DWORD want = std::min<DWORD>(avail, (DWORD)chunk.size());
            DWORD got = 0;
            if (!WinHttpReadData(req.h, chunk.data(), want, &got))
                return fail(ApiError(L"read failed", GetLastError()));
            if (got == 0)
                return fail(L"read failed: connection closed early");
            total += got;
            if (!sink(chunk.data(), got))
                return fail(L"the receiver stopped the transfer");
            if (progress)
                progress(total, haveLength ? contentLength : 0);
        }

        if (haveLength && total != contentLength)
            return fail(L"read failed: truncated body ("
                + std::to_wstring(total) + L" of " + std::to_wstring(contentLength) + L" bytes)");
        return true;
    }

    // Fetch http(s) `endpoint` into `body` (cleared first), at most kMaxBodyBytes
    // within kOverallMs. False plus `errorOut` on any failure.
    static bool FetchBody(const std::wstring& endpoint, std::string& body, std::wstring* errorOut)
    {
        body.clear();
        HttpGetOptions opts;
        opts.maxBytes  = kMaxBodyBytes;
        opts.overallMs = kOverallMs;
        return HttpGet(endpoint, opts,
            [&body](const char* data, size_t n) { body.append(data, n); return true; },
            nullptr, errorOut);
    }

    bool FindServer(const std::wstring& endpoint, ServerInfo& out, std::wstring* errorOut)
    {
        std::string body;
        std::wstring err;
        if (!FetchBody(endpoint, body, &err))
        {
            if (errorOut) *errorOut = err;
            return false;
        }
        std::vector<ServerInfo> servers;
        if (!ParseServerList(body, servers, err))
        {
            if (errorOut)
                *errorOut = (err == L"no servers in list") ? L"no host/port in server list" : err;
            return false;
        }
        out = servers.front();
        GLOGI("server found: %s:%d %s",
            gage::Narrow(out.host).c_str(), out.port,
            gage::Narrow(out.name).c_str());
        return true;
    }

    bool ListServers(const std::wstring& endpoint, std::vector<ServerInfo>& out, std::wstring* errorOut)
    {
        std::string body;
        std::wstring err;
        if (!FetchBody(endpoint, body, &err))
        {
            if (errorOut) *errorOut = err;
            return false;
        }
        std::vector<ServerInfo> servers;
        if (!ParseServerList(body, servers, err))
        {
            if (errorOut) *errorOut = err;
            return false;
        }
        GLOGI("server list: %zu entr(ies) from %s", servers.size(), gage::Narrow(endpoint).c_str());
        out.insert(out.end(), servers.begin(), servers.end());
        return true;
    }

    ServerInfo ResolveServer(const std::wstring& configured)
    {
        std::wstring endpoint = !configured.empty()
            ? configured
            : gage::reg::Get(L"server_endpoint", kDefaultListUrl);

        // 1) Direct host:port / ip:port (and ws://host:port) — no network.
        {
            ServerInfo direct;
            if (ParseDirect(endpoint, direct))
            {
                GLOGI("server direct: %s:%d",
                    gage::Narrow(direct.host).c_str(), direct.port);
                return direct;
            }
        }

        // 2) JSON-list endpoint over HTTP(S).
        ServerInfo info;
        std::wstring err;
        if (FindServer(endpoint, info, &err))
            return info;

        GLOGW("server finder fallback (endpoint=%s): %s",
            gage::Narrow(endpoint).c_str(), gage::Narrow(err).c_str());
        info.host = L"127.0.0.1";
        info.port = kDefaultPort;
        info.name = L"local (fallback)";
        return info;
    }

    // ---- version helpers --------------------------------------------------
    // Compare two dot-separated version strings. Returns <0 if a<b, 0 if
    // equal, >0 if a>b. Non-numeric components (spaces, 'v', letters) are
    // skipped; missing trailing components count as 0 ("1" == "1.0").
    // Components are compared as digit strings (leading zeros dropped, then
    // length, then lexicographic), so arbitrarily long runs cannot overflow.
    static int VerCmp(const std::wstring& a, const std::wstring& b)
    {
        auto next = [](const std::wstring& s, size_t& i)
        {
            while (i < s.size() && !(s[i] >= L'0' && s[i] <= L'9'))
                i++;                       // skip non-digits
            size_t start = i;
            while (i < s.size() && s[i] >= L'0' && s[i] <= L'9')
                i++;                       // one digit run
            size_t z = start;
            while (z < i && s[z] == L'0')
                z++;                       // drop leading zeros ("" == 0)
            return s.substr(z, i - z);
        };
        size_t i = 0, j = 0;
        for (;;)
        {
            std::wstring va = next(a, i);
            std::wstring vb = next(b, j);
            if (va.size() != vb.size())
                return (va.size() > vb.size()) ? 1 : -1;
            int c = va.compare(vb);
            if (c != 0)
                return (c > 0) ? 1 : -1;
            if (i >= a.size() && j >= b.size())
                return 0;
        }
    }

    bool VersionAtLeast(const std::wstring& a, const std::wstring& b)
    {
        return VerCmp(a, b) >= 0;
    }

    // The "version" string at the top level of the document, else one object
    // level down ({"data":{"version":"1.2.3"}}).
    static const JVal* FindVersion(const JVal& root)
    {
        const JVal* v = Member(root, "version");
        if (v && v->t == JVal::Str)
            return v;
        if (root.t == JVal::Obj)
            for (size_t i = 0; i < root.o.size(); ++i)
            {
                const JVal* nested = Member(root.o[i].second, "version");
                if (nested && nested->t == JVal::Str)
                    return nested;
            }
        return nullptr;
    }

    std::wstring FetchVersion(const std::wstring& url, std::wstring* errorOut)
    {
        std::string body;
        std::wstring err;
        if (!FetchBody(url, body, &err))
        {
            if (errorOut) *errorOut = err;
            return L"";
        }
        JParser jp;
        if (!ParseBody(body, jp))
        {
            if (errorOut) *errorOut = L"invalid JSON in version response";
            return L"";
        }
        const JVal* v = FindVersion(jp.v);
        std::string ver = v ? TrimA(v->s) : std::string();
        if (ver.empty() || ver.size() > 64)
        {
            if (errorOut) *errorOut = L"no \"version\" field in response";
            return L"";
        }
        return gage::Widen(ver);
    }

}
