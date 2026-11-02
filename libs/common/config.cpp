#include "config.h"
#include "common.h"
#include "logger.h"
#include <windows.h>
#include <io.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <map>

namespace gage::cfg {

    using Map = std::map<std::wstring, std::wstring>;

    // One process-wide lock serialises every read and every read-modify-write of
    // the file (UI thread and worker threads both call in). SRWLOCK_INIT is
    // constant-initialised, so there is no static-constructor ordering to worry
    // about, unlike a static std::mutex.
    static SRWLOCK g_lock = SRWLOCK_INIT;

    struct SharedGuard
    {
        SharedGuard()  { AcquireSRWLockShared(&g_lock); }
        ~SharedGuard() { ReleaseSRWLockShared(&g_lock); }
        SharedGuard(const SharedGuard&) = delete;
        SharedGuard& operator=(const SharedGuard&) = delete;
    };
    struct ExclusiveGuard
    {
        ExclusiveGuard()  { AcquireSRWLockExclusive(&g_lock); }
        ~ExclusiveGuard() { ReleaseSRWLockExclusive(&g_lock); }
        ExclusiveGuard(const ExclusiveGuard&) = delete;
        ExclusiveGuard& operator=(const ExclusiveGuard&) = delete;
    };

    static const size_t kMaxFileBytes = 4u << 20; // refuse to slurp anything bigger
    static const long   kMaxListCount = 4096;

    std::wstring Path()
    {
        return ExeDir() + L"gage_mp.cfg";
    }

    // ---- parsing -----------------------------------------------------------

    static std::string Trim(const std::string& s, bool left, bool right)
    {
        size_t b = 0, e = s.size();
        if (left)
            while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r'))
                ++b;
        if (right)
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
                --e;
        return s.substr(b, e - b);
    }

    // Parse every key=value pair under [gage]. The file is UTF-8; keys are
    // ASCII identifiers in practice, so Widen at the boundary.
    static void Parse(const std::string& data, Map& m)
    {
        size_t pos = 0;
        // A UTF-8 BOM (Notepad adds one) would otherwise glue itself to the
        // first line.
        static const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
        if (data.size() >= 3 && std::memcmp(data.data(), kBom, 3) == 0)
            pos = 3;

        while (pos < data.size())
        {
            size_t end = data.find('\n', pos);
            std::string line = (end == std::string::npos)
                ? data.substr(pos) : data.substr(pos, end - pos);
            pos = (end == std::string::npos) ? data.size() : end + 1;

            for (char& c : line) // a NUL would truncate the string at Widen()
                if (c == '\0')
                    c = ' ';

            line = Trim(line, true, true);
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue; // blank / comment
            if (line[0] == '[')
                continue; // the single section we emit; everything is [gage]

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string k = Trim(line.substr(0, eq), true, true);
            if (k.empty())
                continue;
            std::string v = Trim(line.substr(eq + 1), true, true);
            m[Widen(k)] = Widen(v);
        }
    }

    // Load the file into `m`. A missing file is fine (first run, empty map).
    // Any other failure returns false so a writer refuses to overwrite a file it
    // could not read, instead of silently dropping every key in it.
    static bool ReadAll(Map& m)
    {
        m.clear();
        const std::wstring path = Path();

        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f)
        {
            int e = errno;
            if (e == ENOENT)
                return true;
            GLOGE("cfg: cannot open %s for reading (errno %d)", Narrow(path).c_str(), e);
            return false;
        }

        std::string data;
        char buf[8192];
        bool ok = true;
        for (;;)
        {
            size_t n = std::fread(buf, 1, sizeof(buf), f);
            if (n > 0)
            {
                if (data.size() + n > kMaxFileBytes)
                {
                    GLOGE("cfg: %s is larger than %lu bytes, refusing to load it",
                          Narrow(path).c_str(), (unsigned long)kMaxFileBytes);
                    ok = false;
                    break;
                }
                data.append(buf, n);
            }
            if (n < sizeof(buf))
            {
                if (std::ferror(f))
                {
                    GLOGE("cfg: read error on %s (errno %d)", Narrow(path).c_str(), errno);
                    ok = false;
                }
                break;
            }
        }
        std::fclose(f);
        if (ok)
            Parse(data, m);
        return ok;
    }

    // ---- writing -----------------------------------------------------------

    // Write the whole map to <path>.<pid>.tmp, flush it to disk, then swap it
    // over the real file in one MoveFileEx. A crash or a concurrent reader sees
    // either the old file or the new one, never a half-written one. (The pid in
    // the name keeps two processes from truncating each other's temp file.)
    static bool WriteAll(const Map& m)
    {
        const std::wstring path = Path();
        const std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";

        std::string out = "[gage]\n";
        for (const auto& kv : m)
        {
            out += Narrow(kv.first);
            out += '=';
            out += Narrow(kv.second);
            out += '\n';
        }

        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f)
        {
            GLOGE("cfg: cannot create %s (errno %d)", Narrow(tmp).c_str(), errno);
            return false;
        }
        bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
        if (ok)
            ok = std::fflush(f) == 0 && _commit(_fileno(f)) == 0;
        int werr = errno;
        if (std::fclose(f) != 0)
        {
            ok = false;
            werr = errno;
        }
        if (!ok)
        {
            GLOGE("cfg: failed writing %s (errno %d)", Narrow(tmp).c_str(), werr);
            DeleteFileW(tmp.c_str());
            return false;
        }

        // Another process (an AV scan, a second GAGE tool reading the cfg) can
        // hold the destination open for a moment, which makes the replace fail
        // with a sharing error; retry briefly.
        DWORD err = 0;
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            if (MoveFileExW(tmp.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            err = GetLastError();
            if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION
                && err != ERROR_LOCK_VIOLATION)
                break;
            Sleep(15u * (unsigned)(attempt + 1));
        }
        GLOGE("cfg: cannot replace %s (error %lu)", Narrow(path).c_str(), (unsigned long)err);
        DeleteFileW(tmp.c_str());
        return false;
    }

    // ---- validation --------------------------------------------------------

    // Printable, length-limited rendering of caller data for the log (a key with
    // CR/LF in it must not forge log lines).
    static std::string Printable(const std::wstring& s)
    {
        std::wstring t = s.substr(0, 64);
        for (wchar_t& c : t)
            if (c < 0x20)
                c = L'?';
        return Narrow(t);
    }

    // Keys must survive a write/parse round trip: Parse() splits at the first
    // '=', trims whitespace and skips lines starting with '[', ';' or '#'.
    static bool ValidKey(const std::wstring& k)
    {
        if (k.empty())
            return false;
        for (wchar_t c : k)
            if (c == L'=' || c == L'\r' || c == L'\n' || c == L'\0')
                return false;
        const wchar_t first = k.front(), last = k.back();
        if (first == L'[' || first == L';' || first == L'#')
            return false;
        if (first == L' ' || first == L'\t' || last == L' ' || last == L'\t')
            return false;
        return true;
    }

    // No escaping (existing files hold raw C:\ paths); line breaks and NUL are
    // simply not allowed to reach the file, so a value can never start a new
    // "key=value" line.
    static std::wstring SanitizeValue(std::wstring v)
    {
        for (wchar_t& c : v)
            if (c == L'\r' || c == L'\n' || c == L'\0')
                c = L' ';
        return v;
    }

    // ---- public API --------------------------------------------------------

    std::wstring Get(const std::wstring& key, const std::wstring& def)
    {
        SharedGuard lock;
        Map m;
        if (!ReadAll(m))
            return def;
        auto it = m.find(key);
        return (it == m.end()) ? def : it->second;
    }

    bool SetMany(const std::vector<std::pair<std::wstring, std::wstring>>& kv)
    {
        for (const auto& p : kv)
        {
            if (!ValidKey(p.first))
            {
                GLOGE("cfg: rejected invalid key '%s'", Printable(p.first).c_str());
                return false;
            }
        }
        if (kv.empty())
            return true;

        ExclusiveGuard lock;
        Map m;
        if (!ReadAll(m))
            return false;
        for (const auto& p : kv)
            m[p.first] = SanitizeValue(p.second);
        return WriteAll(m);
    }

    bool Set(const std::wstring& key, const std::wstring& value)
    {
        std::vector<std::pair<std::wstring, std::wstring>> kv;
        kv.emplace_back(key, value);
        return SetMany(kv);
    }

    static int ReadCount(const Map& m, const std::wstring& key)
    {
        auto it = m.find(key + L".count");
        if (it == m.end())
            return 0;
        long n = std::wcstol(it->second.c_str(), nullptr, 10);
        if (n < 0)
            return 0;
        return (n > kMaxListCount) ? (int)kMaxListCount : (int)n;
    }

    std::vector<std::wstring> GetList(const std::wstring& key)
    {
        SharedGuard lock;
        Map m;
        std::vector<std::wstring> out;
        if (!ReadAll(m))
            return out;
        int n = ReadCount(m, key);
        for (int i = 0; i < n; ++i)
        {
            auto v = m.find(key + L"_" + std::to_wstring(i));
            if (v == m.end())
                break; // stop at the first gap so the list is a clean prefix
            out.push_back(v->second);
        }
        return out;
    }

    // Remove "<key>.count" and every "<key>_<digits>" entry by scanning the
    // sorted map from the prefix, so cost is bounded by what is actually in the
    // file and not by a (possibly hostile) stored count.
    static void EraseList(Map& m, const std::wstring& key)
    {
        const std::wstring prefix = key + L"_";
        auto it = m.lower_bound(prefix);
        while (it != m.end() && it->first.compare(0, prefix.size(), prefix) == 0)
        {
            bool digits = it->first.size() > prefix.size();
            for (size_t i = prefix.size(); digits && i < it->first.size(); ++i)
                digits = it->first[i] >= L'0' && it->first[i] <= L'9';
            it = digits ? m.erase(it) : std::next(it);
        }
        m.erase(key + L".count");
    }

    bool SetList(const std::wstring& key, const std::vector<std::wstring>& values)
    {
        if (!ValidKey(key))
        {
            GLOGE("cfg: rejected invalid list key '%s'", Printable(key).c_str());
            return false;
        }
        size_t count = values.size();
        if (count > (size_t)kMaxListCount)
        {
            GLOGW("cfg: list '%s' has %lu entries, keeping the first %ld",
                  Printable(key).c_str(), (unsigned long)count, kMaxListCount);
            count = (size_t)kMaxListCount;
        }

        ExclusiveGuard lock;
        Map m;
        if (!ReadAll(m))
            return false;
        EraseList(m, key);
        for (size_t i = 0; i < count; ++i)
            m[key + L"_" + std::to_wstring(i)] = SanitizeValue(values[i]);
        m[key + L".count"] = std::to_wstring(count);
        return WriteAll(m);
    }

}
