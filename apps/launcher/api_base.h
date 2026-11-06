#pragma once
// The platform API base the launcher hands its UI (settings.apiBase): pure checks, header-only, so a selftest can pin them
// (tests/version_gate_test.cpp). gage_gui.cpp GetApiBase() picks the value (gage_mp.cfg api_base=, else the build's
// GAGE_API_BASE) and refuses one these checks refuse.

#include <cwctype>
#include <cwchar>
#include <string>

namespace gage::apibase {

    // An https:// origin, or http:// on 127.0.0.1 / localhost for development: a host of [A-Za-z0-9.-] with an optional
    // :port (1..65535), then an optional path prefix (https://solar-rp.com/api: the SPA calls base + "/v1/..."). The path
    // is at most 64 characters of "/"-separated segments of [A-Za-z0-9._~-]; an empty, "." or ".." segment, a query, a
    // fragment, a backslash or anything else is refused. No trailing "/" (the caller strips it).
    inline bool Valid(const std::wstring& base)
    {
        std::wstring host;
        bool loopbackOnly = false;
        if (base.size() > 8 && _wcsnicmp(base.c_str(), L"https://", 8) == 0) host = base.substr(8);
        else if (base.size() > 7 && _wcsnicmp(base.c_str(), L"http://", 7) == 0) { host = base.substr(7); loopbackOnly = true; }
        else return false;
        const size_t slash = host.find(L'/');
        if (slash != std::wstring::npos)
        {
            const std::wstring path = host.substr(slash);   // "/api" or "/a/b"
            host.resize(slash);
            if (path.size() > 64) return false;
            size_t i = 0;
            while (i < path.size())
            {
                if (path[i] != L'/') return false;
                const size_t end = path.find(L'/', i + 1);
                const std::wstring seg = path.substr(i + 1, (end == std::wstring::npos ? path.size() : end) - i - 1);
                if (seg.empty() || seg == L"." || seg == L"..") return false;
                for (wchar_t c : seg)
                    if (!((c < 128 && iswalnum(c)) || c == L'.' || c == L'_' || c == L'~' || c == L'-')) return false;
                i = end == std::wstring::npos ? path.size() : end;
            }
        }
        if (host.empty() || host.size() > 253) return false;
        for (wchar_t c : host)
            if (!((c < 128 && iswalnum(c)) || c == L'.' || c == L'-' || c == L':')) return false;
        const size_t colon = host.find(L':');
        const std::wstring name = host.substr(0, colon);
        if (name.empty()) return false;
        if (colon != std::wstring::npos)
        {
            const std::wstring port = host.substr(colon + 1);
            if (port.empty() || port.size() > 5 || port.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
            const long p = wcstol(port.c_str(), nullptr, 10);
            if (p < 1 || p > 65535) return false;
        }
        return !loopbackOnly || _wcsicmp(name.c_str(), L"127.0.0.1") == 0 || _wcsicmp(name.c_str(), L"localhost") == 0;
    }

    // `api_base=off` / `none` in gage_mp.cfg (or GAGE_API_BASE=OFF at configure time): no API on purpose, the demo UI.
    inline bool IsOff(const std::wstring& v)
    {
        return _wcsicmp(v.c_str(), L"off") == 0 || _wcsicmp(v.c_str(), L"none") == 0;
    }

}
