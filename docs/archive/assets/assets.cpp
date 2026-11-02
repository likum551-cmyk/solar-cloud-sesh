#include "assets.h"
#include "../common/logger.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp")
#pragma comment(lib, "bcrypt")

using namespace gage;
using gage::net::AssetOptions;
using gage::net::AssetProgress;
using gage::net::AssetResult;
using gage::net::AssetTestOptions;

namespace {

    // ---- limits ------------------------------------------------------------
    const int       kResolveMs     = 10000;
    const int       kConnectMs     = 10000;
    const int       kSendMs        = 15000;
    const int       kReceiveMs     = 10000;               // inactivity: header wait and every body read
    const int       kProbeMs       = 4000;                // counter probe: each phase and the header wait
    const int       kConfirmMs     = 10000;               // second probe confirming an absent boundary index
    const int       kWorkers       = 16;                  // download connections (WinHTTP's per-server
                                                          // limit defaults to INFINITE: no option needed)
    const int       kSearchWays    = 17;                  // a search round probes up to 16 points at once
    const int       kMaxFiles      = 100000;              // hard cap on /file/N indices
    const int       kMaxRecounts   = 4;                   // counter searches before giving up
    const int       kNoIndex       = INT_MAX;             // "not known" for end / limit indices
    const DWORD     kBackoffMs[]   = { 250, 1000, 3000 }; // retries of a failing file below the end
    const int       kMaxRetries    = 3;
    const int       kMaxHashRetries = 1;                  // a file failing its ETag check: one re-download
    const int       kChunkTries    = 3;                   // failures of one range before giving it up
    const ULONGLONG kChunkMin      = 16ull << 20;         // files this big are split ...
    const ULONGLONG kChunkSize     = 8ull << 20;          // ... into ranges this big
    const ULONGLONG kDiskMargin    = 64ull << 20;         // free space kept on top of each file
    const ULONGLONG kProgressMs    = 250;
    const DWORD     kBuf           = 256u * 1024u;        // body read / file compare buffer
    const DWORD     kDrainBytes    = 64u * 1024u;         // WinHTTP drains up to 1 MB (its default) of an
                                                          // abandoned body to reuse the socket; probes and
                                                          // same-size skips must not pull that much
    const size_t    kMaxValidator  = 512;                 // longer ETag / Last-Modified are not kept
    const size_t    kMaxIndexBytes = 32u * 1024u * 1024u; // files.idx / journal read cap

    // ---- handles -----------------------------------------------------------

    // Closes the handle on every exit path (no leak when a later step fails).
    struct WinHttpHandle
    {
        HINTERNET h;
        explicit WinHttpHandle(HINTERNET x = nullptr) : h(x) {}
        ~WinHttpHandle() { reset(); }
        void reset() { if (h) { WinHttpCloseHandle(h); h = nullptr; } }
        WinHttpHandle(const WinHttpHandle&) = delete;
        WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    };

    struct FileHandle
    {
        HANDLE h;
        explicit FileHandle(HANDLE x = INVALID_HANDLE_VALUE) : h(x) {}
        ~FileHandle() { close(); }
        void close() { if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;
    };

    const char* WinHttpErrName(DWORD code)
    {
        switch (code)
        {
        case ERROR_WINHTTP_TIMEOUT:           return " (timed out)";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return " (name not resolved)";
        case ERROR_WINHTTP_CANNOT_CONNECT:    return " (cannot connect)";
        case ERROR_WINHTTP_CONNECTION_ERROR:  return " (connection error)";
        case 12152:                           return " (invalid server response)"; // ERROR_WINHTTP_INVALID_SERVER_RESPONSE
        case 12175:                           return " (TLS failure)"; // ERROR_WINHTTP_SECURE_FAILURE
        case 12156:                           return " (redirect failed)"; // ERROR_WINHTTP_REDIRECT_FAILED
        default:                              return "";
        }
    }

    // ---- hashing (BCrypt) --------------------------------------------------

    bool Sha256(const std::string& data, unsigned char out[32])
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            return false;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))
            && BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0))
            && BCRYPT_SUCCESS(BCryptFinishHash(hash, out, 32, 0));
        if (hash)
            BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }

    // One MD5 provider for the process (provider handles are thread-safe;
    // opening one per file would cost more than hashing a small file).
    BCRYPT_ALG_HANDLE Md5Provider()
    {
        static std::once_flag once;
        static BCRYPT_ALG_HANDLE alg = nullptr;
        std::call_once(once, []()
        {
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0)))
            {
                alg = nullptr;
                GLOGW("assets: MD5 provider unavailable; file hashes are not recorded");
            }
        });
        return alg;
    }

    // Incremental MD5; hex() is "" when the provider failed.
    struct Md5
    {
        BCRYPT_HASH_HANDLE h = nullptr;
        bool ok = false;
        Md5()
        {
            BCRYPT_ALG_HANDLE alg = Md5Provider();
            ok = alg && BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0));
        }
        ~Md5() { if (h) BCryptDestroyHash(h); }
        Md5(const Md5&) = delete;
        Md5& operator=(const Md5&) = delete;
        void add(const void* p, size_t n)
        {
            if (ok && n)
                ok = BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0));
        }
        std::string hex()
        {
            unsigned char d[16];
            if (!ok || !BCRYPT_SUCCESS(BCryptFinishHash(h, d, 16, 0)))
                return "";
            ok = false;
            static const char digits[] = "0123456789abcdef";
            std::string s;
            for (unsigned char b : d)
            {
                s += digits[b >> 4];
                s += digits[b & 15];
            }
            return s;
        }
    };

    // MD5 of a whole file, "" on any error.
    std::string FileMd5(const std::wstring& path)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return "";
        Md5 md5;
        std::vector<char> buf(kBuf);
        bool ok = true;
        for (;;)
        {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), kBuf, &got, nullptr))
            {
                ok = false;
                break;
            }
            if (got == 0)
                break;
            md5.add(buf.data(), got);
        }
        CloseHandle(f);
        return ok ? md5.hex() : "";
    }

    // ---- small file helpers ------------------------------------------------

    bool FileSizeOf(const std::wstring& path, unsigned long long& size)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)
            || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return false;
        size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        return true;
    }

    bool IsDir(const std::wstring& path)
    {
        DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }

    // Create `dir` (ends in a backslash) and every missing parent.
    bool MakeDirs(const std::wstring& dir)
    {
        for (size_t i = 1; i < dir.size(); ++i)
        {
            if (dir[i] != L'\\' || dir[i - 1] == L':' || dir[i - 1] == L'\\')
                continue; // skip "C:\" and the "\\" of a UNC prefix
            std::wstring part = dir.substr(0, i);
            if (!IsDir(part) && !CreateDirectoryW(part.c_str(), nullptr)
                && GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
        return IsDir(dir);
    }

    bool ReadWhole(const std::wstring& path, std::string& out, size_t cap)
    {
        out.clear();
        FileHandle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (f.h == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(f.h, &sz) || sz.QuadPart < 0 || (unsigned long long)sz.QuadPart > cap)
            return false;
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!out.empty() && (!ReadFile(f.h, &out[0], (DWORD)out.size(), &got, nullptr) || got != out.size()))
            return false;
        return true;
    }

    // Rename over `to`. A file just closed can be held for a moment by an
    // antivirus scan, so sharing / access errors get a few short retries.
    bool MoveReplace(const std::wstring& from, const std::wstring& to)
    {
        for (int attempt = 0; ; ++attempt)
        {
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            DWORD e = GetLastError();
            if (attempt >= 5 || (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION))
            {
                GLOGW("assets: rename %s failed: %lu", Narrow(to).c_str(), e);
                return false;
            }
            Sleep(100);
        }
    }

    // Write `data` to `path` via "<path>.tmp" + rename, so readers see the old
    // or the new content, never a mix.
    bool WriteAtomic(const std::wstring& path, const std::string& data)
    {
        const std::wstring tmp = path + L".tmp";
        {
            FileHandle f(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr));
            if (f.h == INVALID_HANDLE_VALUE)
                return false;
            DWORD put = 0;
            bool ok = (data.empty() || (WriteFile(f.h, data.data(), (DWORD)data.size(), &put, nullptr)
                                        && put == data.size()))
                && FlushFileBuffers(f.h);
            f.close();
            if (!ok)
            {
                DeleteFileW(tmp.c_str());
                return false;
            }
        }
        if (!MoveReplace(tmp, path))
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    // Byte-for-byte comparison of two files.
    bool SameBytes(const std::wstring& a, const std::wstring& b)
    {
        unsigned long long sa = 0, sb = 0;
        if (!FileSizeOf(a, sa) || !FileSizeOf(b, sb) || sa != sb)
            return false;
        FileHandle fa(CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        FileHandle fb(CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        if (fa.h == INVALID_HANDLE_VALUE || fb.h == INVALID_HANDLE_VALUE)
            return false;
        std::vector<char> ba(kBuf), bb(kBuf);
        for (;;)
        {
            DWORD ga = 0, gb = 0;
            if (!ReadFile(fa.h, ba.data(), kBuf, &ga, nullptr) || !ReadFile(fb.h, bb.data(), kBuf, &gb, nullptr))
                return false;
            if (ga != gb || memcmp(ba.data(), bb.data(), ga) != 0)
                return false;
            if (ga == 0)
                return true;
        }
    }

    bool ParseDigits(const std::string& s, unsigned long long& out)
    {
        if (s.empty() || s.size() > 19)
            return false;
        unsigned long long v = 0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] < '0' || s[i] > '9')
                return false;
            v = v * 10 + (unsigned long long)(s[i] - '0');
        }
        out = v;
        return true;
    }

    // ---- ETag schemes ------------------------------------------------------

    enum HashMode { HashNone = 0, HashMd5 = 1, HashSizeMtime = 2 };

    // The opaque part of a strong ETag ("" for a weak or missing one).
    std::string StripEtag(const std::string& etag)
    {
        if (etag.size() >= 2 && (etag[0] == 'W' || etag[0] == 'w') && etag[1] == '/')
            return "";
        std::string s = etag;
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        return s;
    }

    bool IsHex(const std::string& s)
    {
        if (s.empty())
            return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        return true;
    }

    bool EqualHex(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size() || a.empty())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                return false;
        return true;
    }

    bool IsMd5Tag(const std::string& tag) { return tag.size() == 32 && IsHex(tag); }

    // nginx's "<hex size>-<hex mtime>".
    bool ParseSizeMtime(const std::string& tag, unsigned long long& size)
    {
        size_t dash = tag.find('-');
        if (dash == std::string::npos || dash == 0 || dash > 16 || tag.find('-', dash + 1) != std::string::npos)
            return false;
        std::string a = tag.substr(0, dash), b = tag.substr(dash + 1);
        if (!IsHex(a) || !IsHex(b))
            return false;
        size = std::strtoull(a.c_str(), nullptr, 16);
        return true;
    }

    // ---- cache index -------------------------------------------------------

    struct Entry
    {
        unsigned long long size = 0;
        std::string etag;    // as the server sent it, quotes and W/ included
        std::string lastMod;
        std::string md5;     // our own MD5 of the blob ("" = not recorded)
    };
    using Index = std::map<int, Entry>;

    // A validator kept only when it fits on one files.idx line.
    std::string CleanValidator(const std::wstring& v)
    {
        std::string s = Narrow(v);
        if (s.size() > kMaxValidator || s.find_first_of("\t\r\n") != std::string::npos)
            return "";
        return s;
    }

    std::string IndexLine(int n, const Entry& e)
    {
        return std::to_string(n) + "\t" + std::to_string(e.size) + "\t" + e.etag + "\t" + e.lastMod
            + "\t" + e.md5 + "\n";
    }

    // Apply "N<TAB>size<TAB>etag<TAB>last-modified[<TAB>md5]" lines to `idx`
    // (later lines win). Malformed lines are skipped (that file is just
    // re-downloaded); with `wholeLines` a last line without its newline (a
    // torn journal append) is skipped too.
    void ApplyIndexText(const std::string& text, bool wholeLines, Index& idx)
    {
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos)
            {
                if (wholeLines)
                    break;
                eol = text.size();
            }
            std::string line = text.substr(pos, eol - pos);
            pos = eol + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::vector<std::string> f;
            size_t s = 0;
            for (;;)
            {
                size_t t = line.find('\t', s);
                f.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
                if (t == std::string::npos)
                    break;
                s = t + 1;
            }
            unsigned long long n = 0, size = 0;
            if ((f.size() != 4 && f.size() != 5) || !ParseDigits(f[0], n) || n >= (unsigned long long)kMaxFiles
                || !ParseDigits(f[1], size))
                continue;
            Entry e;
            e.size = size;
            e.etag = f[2];
            e.lastMod = f[3];
            if (f.size() == 5 && IsMd5Tag(f[4]))
                e.md5 = f[4];
            idx[(int)n] = e;
        }
    }

    // files.idx, then the journal of a run that never got to rewrite it.
    Index LoadIndex(const std::wstring& idxPath, const std::wstring& journalPath, size_t& journalled)
    {
        Index idx;
        std::string text;
        if (ReadWhole(idxPath, text, kMaxIndexBytes))
            ApplyIndexText(text, false, idx);
        journalled = 0;
        if (ReadWhole(journalPath, text, kMaxIndexBytes))
        {
            Index j;
            ApplyIndexText(text, true, j);
            journalled = j.size();
            for (const auto& kv : j)
                idx[kv.first] = kv.second;
        }
        return idx;
    }

    bool SaveIndex(const std::wstring& path, const Index& idx)
    {
        std::string text;
        for (const auto& kv : idx)
            text += IndexLine(kv.first, kv.second);
        return WriteAtomic(path, text);
    }

    // The `complete` marker's file count; false when missing or unreadable.
    bool ReadComplete(const std::wstring& path, int& count)
    {
        std::string text;
        if (!ReadWhole(path, text, 64))
            return false;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        unsigned long long n = 0;
        if (!ParseDigits(text, n) || n > (unsigned long long)kMaxFiles)
            return false;
        count = (int)n;
        return true;
    }

    // ---- HTTP helpers ------------------------------------------------------

    struct Target
    {
        std::wstring  host;
        INTERNET_PORT port = 0;
        bool          secure = false;
        std::wstring  path;   // path, plus the query when kept
    };

    bool CrackTarget(const std::wstring& url, Target& t, bool keepQuery)
    {
        if (url.empty() || url.size() > 8192)
            return false;
        URL_COMPONENTS uc;
        ZeroMemory(&uc, sizeof(uc));
        uc.dwStructSize      = sizeof(uc);
        uc.dwSchemeLength    = (DWORD)-1;
        uc.dwHostNameLength  = (DWORD)-1;
        uc.dwUrlPathLength   = (DWORD)-1;
        uc.dwExtraInfoLength = (DWORD)-1;
        if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)
            || (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)
            || !uc.lpszHostName || uc.dwHostNameLength == 0 || uc.nPort == 0)
            return false;
        t.host.assign(uc.lpszHostName, uc.dwHostNameLength);
        t.port = uc.nPort;
        t.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
        t.path.clear();
        if (uc.lpszUrlPath && uc.dwUrlPathLength)
            t.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
        if (keepQuery && uc.lpszExtraInfo && uc.dwExtraInfoLength)
        {
            std::wstring extra(uc.lpszExtraInfo, uc.dwExtraInfoLength);
            size_t frag = extra.find(L'#'); // fragments are never sent
            if (frag != std::wstring::npos)
                extra.resize(frag);
            t.path += extra;
        }
        if (t.path.empty() || t.path[0] != L'/')
            t.path = L"/" + t.path;
        return true;
    }

    std::wstring QueryText(HINTERNET hReq, DWORD info)
    {
        DWORD len = 0;
        WinHttpQueryHeaders(hReq, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                            &len, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0)
            return L"";
        std::wstring s(len / sizeof(wchar_t) + 1, L'\0');
        DWORD cap = (DWORD)(s.size() * sizeof(wchar_t));
        if (!WinHttpQueryHeaders(hReq, info, WINHTTP_HEADER_NAME_BY_INDEX, &s[0], &cap,
                                 WINHTTP_NO_HEADER_INDEX))
            return L"";
        s.resize(cap / sizeof(wchar_t));
        return s;
    }

    // The URL the response really came from (after every redirect hop).
    std::wstring QueryUrl(HINTERNET hReq)
    {
        DWORD len = 0;
        WinHttpQueryOption(hReq, WINHTTP_OPTION_URL, nullptr, &len);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0)
            return L"";
        std::wstring s(len / sizeof(wchar_t) + 1, L'\0');
        DWORD cap = (DWORD)(s.size() * sizeof(wchar_t));
        if (!WinHttpQueryOption(hReq, WINHTTP_OPTION_URL, &s[0], &cap))
            return L"";
        s.resize(wcslen(s.c_str()));
        return s;
    }

    bool IsHtml(HINTERNET hReq)
    {
        std::wstring type = QueryText(hReq, WINHTTP_QUERY_CONTENT_TYPE);
        std::transform(type.begin(), type.end(), type.begin(), ::towlower);
        return type.compare(0, 9, L"text/html") == 0;
    }

    // "bytes a-b/total"
    bool ParseContentRange(const std::wstring& v, unsigned long long& a, unsigned long long& b,
                           unsigned long long& total)
    {
        std::string s = Narrow(v);
        if (s.compare(0, 6, "bytes ") != 0)
            return false;
        size_t dash = s.find('-', 6), slash = s.find('/', 6);
        if (dash == std::string::npos || slash == std::string::npos || dash > slash)
            return false;
        return ParseDigits(s.substr(6, dash - 6), a) && ParseDigits(s.substr(dash + 1, slash - dash - 1), b)
            && ParseDigits(s.substr(slash + 1), total);
    }

    // Open, send and wait for the headers of a GET. False when no status came
    // back (`err` says why). `phaseMs` > 0 overrides every session timeout
    // (the counter's short probes); `waitMs` is the header wait.
    bool SendGet(HINTERNET conn, const std::wstring& path, bool secure, const std::wstring& hdrs,
                 int waitMs, int phaseMs, WinHttpHandle& req, DWORD& status, DWORD& err)
    {
        status = 0;
        err = 0;
        req.h = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (!req.h)
        {
            err = GetLastError();
            return false;
        }
        if (phaseMs > 0)
            WinHttpSetTimeouts(req.h, phaseMs, phaseMs, phaseMs, phaseMs);
        DWORD wait = (DWORD)waitMs;
        WinHttpSetOption(req.h, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &wait, sizeof(wait));
        // Added headers survive WinHTTP's automatic redirects.
        if (!hdrs.empty()
            && !WinHttpAddRequestHeaders(req.h, hdrs.c_str(), (DWORD)hdrs.size(), WINHTTP_ADDREQ_FLAG_ADD))
        {
            err = GetLastError();
            return false;
        }
        if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(req.h, nullptr))
        {
            err = GetLastError();
            return false;
        }
        DWORD len = sizeof(status);
        if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX))
        {
            status = 0;
            err = GetLastError();
            return false;
        }
        return true;
    }

    HINTERNET OpenSession(int resolveMs, int connectMs, int sendMs, int receiveMs)
    {
        HINTERNET s = WinHttpOpen(L"gage-mp/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) // pre-8.1 WinHTTP rejects automatic proxy
            s = WinHttpOpen(L"gage-mp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s)
            return nullptr;
        if (!WinHttpSetTimeouts(s, resolveMs, connectMs, sendMs, receiveMs))
        {
            WinHttpCloseHandle(s);
            return nullptr;
        }
        DWORD drain = kDrainBytes;
        WinHttpSetOption(s, WINHTTP_OPTION_MAX_RESPONSE_DRAIN_SIZE, &drain, sizeof(drain));
        return s;
    }

    // ---- one run -----------------------------------------------------------

    // Why a file did not arrive.
    struct Fault
    {
        DWORD status = 0;      // HTTP status, 0 = no response
        DWORD err = 0;         // WinHTTP / file error
        bool  html = false;    // a text/html page instead of the file
        bool  broken = false;  // the body broke off (the file is there)
        bool  badHash = false; // the body does not match its ETag
    };

    enum class Res
    {
        Downloaded, // written under its final name (index 0: left in .tmp for the caller)
        Cached,     // 304, same bytes, or same size with no validators
        NonPresent, // 404 / html / 5xx / other status / no response: absent or failing
        Transient,  // present, but the body broke off, failed its check, or changed mid-download
        Fatal,      // local problem (disk space, file system)
    };

    struct FileRes
    {
        Res                res = Res::NonPresent;
        Fault              why;
        Entry              entry;      // what the cache records for this index
        unsigned long long bytes = 0;  // body bytes received for it (ranges included)
        std::wstring       fatal;      // player-facing text for Res::Fatal
    };

    struct Done
    {
        Entry              entry;
        bool               cached = false;
        unsigned long long bytes = 0;
    };

    struct RetryItem
    {
        int       index = 0;
        int       attempt = 0;   // retries already made
        ULONGLONG due = 0;
        Fault     why;
    };

    // A file whose body is coming in: its announced size (0 = not announced)
    // and the bytes received for it on every connection. Registered in
    // Run::flights for as long as FetchFile works on it (progress reports).
    struct Flight
    {
        unsigned long long               length = 0;
        std::atomic<unsigned long long>  got{0};
    };

    // A file being fetched in ranges. The owner streams from offset 0; helpers
    // claim chunks from the end. All fields but the atomics are guarded by
    // Run::m.
    struct BigFile
    {
        std::shared_ptr<Flight> flight; // the helpers' bytes count toward it too
        enum State : char { Free, Owner, Helper, Done };
        enum Range : int { Unknown, Yes, No };

        int                index = 0;
        std::wstring       tmp;
        Target             target;      // the file's final URL (redirects resolved)
        unsigned long long length = 0, chunk = 0;
        int                chunks = 0;
        std::string        etag, lastMod; // the owner's version: If-Range and the 206 check
        std::vector<char>  state;
        std::vector<int>   tries;
        std::atomic<int>   range{Unknown};
        std::atomic<bool>  failed{false};
        bool               probing = false;    // one helper is finding out whether ranges work
        bool               ownerActive = true; // the owner is still streaming
        int                helpers = 0;        // helper requests in flight
        unsigned long long helperBytes = 0;
    };

    struct Run
    {
        HINTERNET    session = nullptr;      // downloads
        HINTERNET    probeSession = nullptr; // the counter's short probes
        Target       base;                   // first hop; base.path = prefix, no trailing slash
        std::wstring label;                  // "host:port" for player-facing errors
        std::wstring dir, fileDir;           // end in a backslash
        std::wstring journalPath;
        Index        old;                    // files.idx (+ journal) as found; read-only once workers run
        const std::function<void(const AssetProgress&)>* onProgress = nullptr;
        ULONGLONG    startTick = 0;
        int          workers = kWorkers;
        bool         chunking = true;
        unsigned long long chunkMin = kChunkMin, chunkSize = kChunkSize;
        int          maxFiles = kMaxFiles;   // test prefix runs: indices below this only
        int          hash = HashNone;        // the ETag scheme found on /file/0; fixed before workers start

        std::atomic<bool> failed{false};
        std::atomic<bool> finished{false};
        std::atomic<int>  total{-1};
        std::atomic<unsigned long long> liveBytes{0};
        std::atomic<int>  liveFiles{0};
        std::atomic<int>  liveCached{0};
        std::atomic<int>  probes{0};
        std::atomic<ULONGLONG> lastReport{0};
        std::atomic<ULONGLONG> countMs{0};

        // ---- scheduler, guarded by m ----
        std::mutex              m;
        std::condition_variable cv;
        int nextIndex = 1;
        int endE = kNoIndex;       // the counter's verified total; only it ends the list
        int suspect = kNoIndex;    // lowest non-present index while the end is unknown
        int recountFrom = -1;      // a file answered at/after the published end: count again
        int inFlight = 0;          // file fetches running
        std::deque<RetryItem> retries;
        std::map<int, RetryItem> parked;  // non-present while the end was unknown
        std::map<int, RetryItem> dropped; // non-present at/after the end (re-queued if it moves up)
        std::map<int, Done> done;
        std::vector<std::shared_ptr<BigFile>> big;
        std::wstring error;

        std::mutex reportLock;  // serialises onProgress
        std::mutex flightLock;  // guards flights
        std::map<int, std::shared_ptr<Flight>> flights;
        std::mutex journalLock;
        HANDLE     journal = INVALID_HANDLE_VALUE;

        bool Stopping() const { return failed.load() || finished.load(); }
    };

    void Report(Run& run)
    {
        if (!run.onProgress || !*run.onProgress)
            return;
        if (GetTickCount64() - run.lastReport.load() < kProgressMs)
            return;
        std::lock_guard<std::mutex> g(run.reportLock);
        // Read the clock under the lock: a tick taken before another thread's
        // report would underflow against its newer lastReport.
        ULONGLONG now = GetTickCount64();
        if (now - run.lastReport.load() < kProgressMs) // another thread just reported
            return;
        run.lastReport.store(now);
        AssetProgress p;
        p.files = run.liveFiles.load();
        p.total = run.total.load();
        p.fromCache = run.liveCached.load();
        p.bytes = run.liveBytes.load();
        double secs = (double)(now - run.startTick) / 1000.0;
        p.mbps = secs > 0 ? (double)p.bytes * 8.0 / 1e6 / secs : 0;
        {
            std::lock_guard<std::mutex> f(run.flightLock);
            p.active = (int)run.flights.size();
            unsigned long long bigLeft = 0;
            for (const auto& kv : run.flights)
            {
                const Flight& fl = *kv.second;
                if (!fl.length)
                    continue;
                // a range fetched twice (a retry) can count past the size
                const unsigned long long got = std::min(fl.got.load(), fl.length);
                p.activeLeft += fl.length - got;
                if (fl.length - got > bigLeft)
                {
                    bigLeft = fl.length - got;
                    p.bigIndex = kv.first;
                    p.bigSize = fl.length;
                    p.bigDone = got;
                }
            }
        }
        (*run.onProgress)(p);
    }

    // Record the first run-ending error. Caller holds run.m.
    void FailLocked(Run& run, const std::wstring& why)
    {
        if (!run.failed.load())
        {
            run.error = why;
            run.failed = true;
            GLOGW("assets: %s", Narrow(why).c_str());
        }
        run.cv.notify_all();
    }

    std::wstring NoSpaceText(unsigned long long need)
    {
        return L"Not enough disk space (need " + std::to_wstring((need + kDiskMargin) / (1024 * 1024) + 1)
            + L" MB free)";
    }

    // Player-facing text for a file that kept failing.
    std::wstring FailText(const Run& run, int index, const Fault& f)
    {
        const std::wstring file = L" (file " + std::to_wstring(index) + L")";
        if (f.badHash)
            return L"A server file failed its check" + file;
        if (f.broken)
            return L"Download interrupted" + file;
        if (f.status)
            return L"Server files unavailable: HTTP " + std::to_wstring(f.status) + file;
        if (index == 0)
            return L"Could not reach the server's file host (" + run.label + L")";
        return L"Server stopped answering" + file;
    }

    void AppendJournal(Run& run, int index, const Entry& e)
    {
        std::lock_guard<std::mutex> g(run.journalLock);
        if (run.journal == INVALID_HANDLE_VALUE)
            run.journal = CreateFileW(run.journalPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (run.journal == INVALID_HANDLE_VALUE)
        {
            GLOGW("assets: cannot open the journal: %lu", GetLastError());
            return;
        }
        std::string line = IndexLine(index, e);
        DWORD put = 0;
        if (!WriteFile(run.journal, line.data(), (DWORD)line.size(), &put, nullptr) || put != line.size())
            GLOGW("assets: journal write failed: %lu", GetLastError());
    }

    void CloseJournal(Run& run)
    {
        std::lock_guard<std::mutex> g(run.journalLock);
        if (run.journal != INVALID_HANDLE_VALUE)
        {
            CloseHandle(run.journal);
            run.journal = INVALID_HANDLE_VALUE;
        }
    }

    // The cached entry for `index`, only when its blob is on disk at the
    // recorded size (anything else is re-downloaded without validators).
    const Entry* Usable(const Run& run, int index)
    {
        auto it = run.old.find(index);
        if (it == run.old.end())
            return nullptr;
        unsigned long long size = 0;
        if (!FileSizeOf(run.fileDir + std::to_wstring(index), size) || size != it->second.size)
            return nullptr;
        return &it->second;
    }

    // Does a downloaded body (its MD5 and length) agree with its ETag under
    // this run's scheme? Unknown shapes pass: only a scheme seen to hold on
    // /file/0 is enforced.
    bool EtagAgrees(const Run& run, const std::string& etag, const std::string& md5, unsigned long long length)
    {
        const std::string tag = StripEtag(etag);
        if (tag.empty())
            return true;
        if (run.hash == HashMd5 && IsMd5Tag(tag))
            return EqualHex(tag, md5); // the body must hash to its ETag
        unsigned long long size = 0;
        if (run.hash == HashSizeMtime && ParseSizeMtime(tag, size))
            return size == length;
        return true;
    }

    // Before keeping a cached blob (304 / same size): with MD5 ETags its
    // recorded MD5 (computed now if an older version did not record one) must
    // equal the ETag. Fills `e.md5` when it computed it.
    bool KeepAgrees(const Run& run, Entry& e, const std::wstring& blob)
    {
        const std::string tag = StripEtag(e.etag);
        if (run.hash == HashMd5 && IsMd5Tag(tag))
        {
            if (e.md5.empty())
                e.md5 = FileMd5(blob);
            return EqualHex(tag, e.md5);
        }
        unsigned long long size = 0;
        if (run.hash == HashSizeMtime && ParseSizeMtime(tag, size))
            return size == e.size;
        return true;
    }

    bool AllDone(const BigFile& bf)
    {
        for (char s : bf.state)
            if (s != BigFile::Done)
                return false;
        return true;
    }

    // Claim the last free chunk of `bf` for a helper. Caller holds run.m.
    // While range support is unknown only one helper (the probe) may go.
    int ClaimChunk(BigFile& bf)
    {
        if (bf.failed.load() || bf.range.load() == BigFile::No)
            return -1;
        if (bf.range.load() == BigFile::Unknown && bf.probing)
            return -1;
        for (int c = bf.chunks - 1; c >= 0; --c)
            if (bf.state[c] == BigFile::Free)
            {
                bf.state[c] = BigFile::Helper;
                bf.helpers++;
                if (bf.range.load() == BigFile::Unknown)
                    bf.probing = true;
                return c;
            }
        return -1;
    }

    // Help the big file with the most free chunks. Caller holds run.m.
    int ClaimAny(Run& run, std::shared_ptr<BigFile>& out)
    {
        std::shared_ptr<BigFile> best;
        int bestFree = 0;
        for (auto& bf : run.big)
        {
            if (bf->failed.load() || bf->range.load() == BigFile::No
                || (bf->range.load() == BigFile::Unknown && bf->probing))
                continue;
            int n = 0;
            for (char s : bf->state)
                if (s == BigFile::Free)
                    n++;
            if (n > bestFree)
            {
                bestFree = n;
                best = bf;
            }
        }
        if (!best)
            return -1;
        out = best;
        return ClaimChunk(*best);
    }

    // One ranged request for chunk `c` of `bf`, written at its offset through
    // this helper's own handle.
    void FetchChunk(Run& run, BigFile& bf, int c)
    {
        const unsigned long long s = (unsigned long long)c * bf.chunk;
        const unsigned long long e = std::min(bf.length, s + bf.chunk) - 1;
        enum { Ok, Fail, NoRanges, Abort } outcome = Fail;
        unsigned long long got = 0;
        {
            WinHttpHandle conn(WinHttpConnect(run.session, bf.target.host.c_str(), bf.target.port, 0));
            WinHttpHandle req;
            DWORD status = 0, err = 0;
            const std::wstring hdrs = L"Range: bytes=" + std::to_wstring(s) + L"-" + std::to_wstring(e)
                + L"\r\nIf-Range: " + Widen(bf.etag.empty() ? bf.lastMod : bf.etag) + L"\r\n";
            if (!conn.h || !SendGet(conn.h, bf.target.path, bf.target.secure, hdrs, kReceiveMs, 0, req, status, err))
                outcome = Fail;
            else if (status == 200)
                outcome = NoRanges; // the server ignores Range (or If-Range says the file changed)
            else if (status != 206)
                outcome = Fail;
            else
            {
                unsigned long long a = 0, b = 0, total = 0;
                const bool rangeOk = ParseContentRange(QueryText(req.h, WINHTTP_QUERY_CONTENT_RANGE), a, b, total)
                    && a == s && b == e && total == bf.length;
                const bool sameVersion = bf.etag.empty()
                    ? CleanValidator(QueryText(req.h, WINHTTP_QUERY_LAST_MODIFIED)) == bf.lastMod
                    : CleanValidator(QueryText(req.h, WINHTTP_QUERY_ETAG)) == bf.etag;
                const bool valid = rangeOk && sameVersion;
                if (!valid)
                    outcome = NoRanges; // another version or a wrong range: never mix bytes
                else
                {
                    {
                        std::lock_guard<std::mutex> g(run.m);
                        if (bf.range.load() == BigFile::Unknown)
                        {
                            bf.range = BigFile::Yes;
                            GLOGI("assets: file %d: ranges supported, splitting %llu bytes", bf.index, bf.length);
                        }
                        bf.probing = false;
                        outcome = (bf.failed.load() || bf.state[c] != BigFile::Helper) ? Abort : Ok;
                        run.cv.notify_all();
                    }
                    if (outcome == Ok)
                    {
                        FileHandle out(CreateFileW(bf.tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                        LARGE_INTEGER at;
                        at.QuadPart = (LONGLONG)s;
                        if (out.h == INVALID_HANDLE_VALUE || !SetFilePointerEx(out.h, at, nullptr, FILE_BEGIN))
                            outcome = Fail;
                        std::vector<char> buf(kBuf);
                        const unsigned long long want = e - s + 1;
                        while (outcome == Ok && got < want)
                        {
                            if (bf.failed.load() || bf.range.load() == BigFile::No)
                            {
                                outcome = Abort;
                                break;
                            }
                            DWORD avail = 0, n = 0, put = 0;
                            if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0)
                            {
                                outcome = Fail;
                                break;
                            }
                            DWORD ask = (DWORD)std::min<unsigned long long>(std::min(avail, kBuf), want - got);
                            if (!WinHttpReadData(req.h, buf.data(), ask, &n) || n == 0
                                || !WriteFile(out.h, buf.data(), n, &put, nullptr) || put != n)
                            {
                                outcome = Fail;
                                break;
                            }
                            got += n;
                            run.liveBytes += n;
                            if (bf.flight)
                                bf.flight->got += n;
                            Report(run);
                        }
                    }
                }
            }
        }

        std::lock_guard<std::mutex> g(run.m);
        bf.helpers--;
        bf.helperBytes += got;
        switch (outcome)
        {
        case Ok:
            if (bf.state[c] == BigFile::Helper)
                bf.state[c] = BigFile::Done;
            break;
        case Abort:
            if (bf.state[c] == BigFile::Helper)
                bf.state[c] = BigFile::Free;
            break;
        case Fail:
            if (bf.range.load() == BigFile::Unknown)
                bf.probing = false;
            if (bf.state[c] == BigFile::Helper)
            {
                bf.state[c] = BigFile::Free;
                if (++bf.tries[c] >= kChunkTries)
                {
                    GLOGW("assets: file %d: range %d failed %d times", bf.index, c, kChunkTries);
                    if (bf.ownerActive)
                        bf.range = BigFile::No; // the owner's stream covers it
                    else
                        bf.failed = true;
                }
            }
            break;
        case NoRanges:
            bf.probing = false;
            if (bf.ownerActive)
            {
                // No (usable) ranges: the owner streams the whole file.
                if (bf.range.load() != BigFile::No)
                    GLOGI("assets: file %d: no usable ranges, single stream", bf.index);
                bf.range = BigFile::No;
                for (char& st : bf.state)
                    if (st == BigFile::Helper)
                        st = BigFile::Free;
            }
            else if (!AllDone(bf))
            {
                GLOGW("assets: file %d changed during download", bf.index);
                bf.failed = true; // the owner stopped; retry the whole file
            }
            break;
        }
        run.cv.notify_all();
    }

    // The owner, after its stream ended: fetch the chunks still free, wait for
    // helpers, and take the file off the help list. True when every byte is in.
    bool FinishBig(Run& run, const std::shared_ptr<BigFile>& bf)
    {
        std::unique_lock<std::mutex> lk(run.m);
        for (;;)
        {
            if (bf->failed.load() || AllDone(*bf))
            {
                if (bf->helpers == 0)
                    break;
            }
            else if (!bf->ownerActive && bf->range.load() == BigFile::No)
            {
                bf->failed = true; // nobody can fetch the rest
                continue;
            }
            else
            {
                int c = ClaimChunk(*bf);
                if (c >= 0)
                {
                    lk.unlock();
                    FetchChunk(run, *bf, c);
                    lk.lock();
                    continue;
                }
            }
            run.cv.wait_for(lk, std::chrono::milliseconds(50));
        }
        run.big.erase(std::remove(run.big.begin(), run.big.end(), bf), run.big.end());
        run.cv.notify_all();
        return !bf->failed.load();
    }

    // GET /file/<index>: revalidate a cached copy, else stream the body (split
    // across helpers when big and the server allows ranges), check it against
    // its ETag, flush, rename and journal. `isZero`: no same-size shortcut, no
    // ranges, no ETag check (the scheme is decided from it afterwards), and the
    // body is compared with the cached copy and left in .tmp for the caller.
    // `fresh`: ignore the cache (a kept copy failed its check).
    FileRes FetchFile(Run& run, HINTERNET conn, int index, bool isZero, bool fresh)
    {
        FileRes r;
        const Entry* cached = fresh ? nullptr : Usable(run, index);
        const std::wstring final = run.fileDir + std::to_wstring(index);
        const std::wstring tmp = final + L".tmp";
        auto nonPresent = [&]() { r.res = Res::NonPresent; return r; };
        auto transient = [&](DWORD err) { r.res = Res::Transient; r.why.broken = true; r.why.err = err; return r; };
        auto fatal = [&](const std::wstring& why) { r.res = Res::Fatal; r.fatal = why; return r; };

        std::wstring hdrs;
        if (cached && !cached->etag.empty())
            hdrs += L"If-None-Match: " + Widen(cached->etag) + L"\r\n";
        if (cached && !cached->lastMod.empty())
            hdrs += L"If-Modified-Since: " + Widen(cached->lastMod) + L"\r\n";
        WinHttpHandle req;
        DWORD status = 0;
        if (!SendGet(conn, run.base.path + L"/file/" + std::to_wstring(index), run.base.secure, hdrs,
                     kReceiveMs, 0, req, status, r.why.err))
            return nonPresent();
        r.why.status = status;

        if (status == 304 && cached)
        {
            r.res = Res::Cached;
            r.entry = *cached;
            std::string etag = CleanValidator(QueryText(req.h, WINHTTP_QUERY_ETAG));
            if (!etag.empty())
                r.entry.etag = etag;
            if (!KeepAgrees(run, r.entry, final))
            {
                GLOGW("assets: file %d: cached copy does not match its ETag; downloading again", index);
                req.reset();
                return FetchFile(run, conn, index, isZero, true);
            }
            if (r.entry.etag != cached->etag || r.entry.md5 != cached->md5)
                AppendJournal(run, index, r.entry);
            return r;
        }
        if (status == 404)
            return nonPresent();
        if (status >= 500) // gateways answer 5xx past the end (502 text/html seen live)
            return nonPresent();
        if (status < 200 || status >= 300)
            return nonPresent();
        if (IsHtml(req.h)) // a catch-all web server answers any path with its page
        {
            r.why.html = true;
            return nonPresent();
        }

        r.entry.etag = CleanValidator(QueryText(req.h, WINHTTP_QUERY_ETAG));
        r.entry.lastMod = CleanValidator(QueryText(req.h, WINHTTP_QUERY_LAST_MODIFIED));
        unsigned long long length = 0;
        const bool haveLength = ParseDigits(Narrow(QueryText(req.h, WINHTTP_QUERY_CONTENT_LENGTH)), length);

        // A cached copy recorded without validators. With MD5 ETags it is kept
        // when its MD5 equals the ETag (and adopts the validators). Otherwise,
        // best effort for servers that send none (raw RAGE:MP): a copy of the
        // announced size is taken as unchanged and the body is never read
        // (closing the request drops the connection). A same-size change slips
        // through there; /file/0 never takes this path.
        if (!isZero && cached && cached->etag.empty() && cached->lastMod.empty()
            && haveLength && length == cached->size)
        {
            const std::string tag = StripEtag(r.entry.etag);
            if (run.hash == HashMd5 && IsMd5Tag(tag))
            {
                Entry keep = *cached;
                if (keep.md5.empty())
                    keep.md5 = FileMd5(final);
                if (EqualHex(keep.md5, tag))
                {
                    keep.etag = r.entry.etag;
                    keep.lastMod = r.entry.lastMod;
                    AppendJournal(run, index, keep);
                    r.res = Res::Cached;
                    r.entry = keep;
                    return r;
                }
                // differs: download it below
            }
            else
            {
                r.res = Res::Cached;
                r.entry = *cached;
                return r;
            }
        }

        // From here the body comes in: the progress reports see this file
        // (its size, its bytes) until FetchFile returns, whichever way.
        auto flight = std::make_shared<Flight>();
        flight->length = haveLength ? length : 0;
        struct FlightScope
        {
            Run& run;
            int  index;
            ~FlightScope()
            {
                std::lock_guard<std::mutex> f(run.flightLock);
                run.flights.erase(index);
            }
        } flightScope{ run, index };
        {
            std::lock_guard<std::mutex> f(run.flightLock);
            run.flights[index] = flight;
        }

        ULARGE_INTEGER freeBytes;
        if (GetDiskFreeSpaceExW(run.dir.c_str(), &freeBytes, nullptr, nullptr)
            && freeBytes.QuadPart < (haveLength ? length : 0) + kDiskMargin)
            return fatal(NoSpaceText(haveLength ? length : 0));

        FileHandle out(CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (out.h == INVALID_HANDLE_VALUE)
        {
            GLOGW("assets: cannot create %s: %lu", Narrow(tmp).c_str(), GetLastError());
            return fatal(L"Could not save server files to disk");
        }
        // Preallocate: a full disk shows up now, not halfway through.
        if (haveLength && length > 0)
        {
            LARGE_INTEGER end, zero;
            end.QuadPart = (LONGLONG)length;
            zero.QuadPart = 0;
            if (!SetFilePointerEx(out.h, end, nullptr, FILE_BEGIN) || !SetEndOfFile(out.h)
                || !SetFilePointerEx(out.h, zero, nullptr, FILE_BEGIN))
            {
                DWORD e = GetLastError();
                out.close();
                DeleteFileW(tmp.c_str());
                return fatal(e == ERROR_DISK_FULL ? NoSpaceText(length) : L"Could not save server files to disk");
            }
        }

        std::shared_ptr<BigFile> bf;
        if (!isZero && run.chunking && haveLength && length >= run.chunkMin
            && (!r.entry.etag.empty() || !r.entry.lastMod.empty()))
        {
            // Without a validator a ranged reply could be another version, so
            // such files are never split.
            Target t;
            if (CrackTarget(QueryUrl(req.h), t, true))
            {
                bf = std::make_shared<BigFile>();
                bf->flight = flight;
                bf->index = index;
                bf->tmp = tmp;
                bf->target = t;
                bf->length = length;
                bf->chunk = run.chunkSize;
                bf->chunks = (int)((length + run.chunkSize - 1) / run.chunkSize);
                bf->etag = r.entry.etag;
                bf->lastMod = r.entry.lastMod;
                bf->state.assign(bf->chunks, BigFile::Free);
                bf->tries.assign(bf->chunks, 0);
                bf->state[0] = BigFile::Owner;
                std::lock_guard<std::mutex> g(run.m);
                run.big.push_back(bf);
                run.cv.notify_all();
            }
        }

        // ---- the owner's stream ----
        std::vector<char> buf(kBuf);
        Md5 md5; // the whole body, when it arrives in order on this stream
        unsigned long long pos = 0;
        int cur = 0;            // the owner's chunk
        bool broken = false, stopped = false;
        DWORD brokenErr = 0;
        std::wstring localError;
        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.h, &avail))
            {
                broken = true;
                brokenErr = GetLastError();
                break;
            }
            if (avail == 0)
                break; // clean EOF
            DWORD want = std::min(avail, kBuf);
            unsigned long long edge = 0;
            if (bf)
            {
                edge = std::min(bf->length, (unsigned long long)(cur + 1) * bf->chunk);
                if (edge > pos && edge - pos < want)
                    want = (DWORD)(edge - pos);
            }
            DWORD got = 0, put = 0;
            if (!WinHttpReadData(req.h, buf.data(), want, &got))
            {
                broken = true;
                brokenErr = GetLastError();
                break;
            }
            if (got == 0)
            {
                broken = true;
                break;
            }
            if (!WriteFile(out.h, buf.data(), got, &put, nullptr) || put != got)
            {
                DWORD e = GetLastError();
                localError = e == ERROR_DISK_FULL ? NoSpaceText(length) : L"Could not save server files to disk";
                break;
            }
            if (!bf)
                md5.add(buf.data(), got);
            pos += got;
            run.liveBytes += got;
            flight->got += got;
            Report(run);
            if (bf && pos == edge)
            {
                std::lock_guard<std::mutex> g(run.m);
                bf->state[cur] = BigFile::Done;
                if (pos < bf->length)
                {
                    const int next = cur + 1;
                    if (bf->failed.load())
                        stopped = true;
                    else if (bf->state[next] == BigFile::Free)
                    {
                        bf->state[next] = BigFile::Owner;
                        cur = next;
                    }
                    else if (bf->range.load() == BigFile::Yes)
                        stopped = true; // helpers (206 confirmed) hold the rest
                    else
                        cur = next;     // range support unknown: keep streaming (same bytes)
                }
                run.cv.notify_all();
                if (stopped)
                    break;
            }
        }
        if (!broken && !stopped && localError.empty() && haveLength && pos != length)
            broken = true; // truncated
        if (stopped)
            req.reset(); // helpers confirmed 206: drop the rest of this stream

        if (bf)
        {
            {
                std::lock_guard<std::mutex> g(run.m);
                bf->ownerActive = false;
                if (!localError.empty())
                    bf->failed = true;
                else if (broken)
                {
                    if (bf->range.load() == BigFile::Yes)
                    {
                        if (bf->state[cur] != BigFile::Done)
                            bf->state[cur] = BigFile::Free; // helpers fetch it as a range
                    }
                    else
                        bf->failed = true;
                }
                run.cv.notify_all();
            }
            const bool complete = FinishBig(run, bf);
            r.bytes = pos + bf->helperBytes;
            if (!localError.empty() || !complete)
            {
                out.close();
                DeleteFileW(tmp.c_str());
                if (!localError.empty())
                    return fatal(localError);
                return transient(brokenErr);
            }
        }
        else
        {
            r.bytes = pos;
            if (!localError.empty() || broken)
            {
                out.close();
                DeleteFileW(tmp.c_str());
                if (!localError.empty())
                    return fatal(localError);
                return transient(brokenErr);
            }
        }

        // Every byte is in: make it durable before anything trusts it.
        if (!FlushFileBuffers(out.h))
        {
            GLOGW("assets: flush %s failed: %lu", Narrow(tmp).c_str(), GetLastError());
            out.close();
            DeleteFileW(tmp.c_str());
            return fatal(L"Could not save server files to disk");
        }
        out.close();
        r.entry.size = haveLength ? length : pos;
        r.entry.md5 = bf ? FileMd5(tmp) : md5.hex(); // ranges arrive out of order: hash the file

        if (!isZero && !EtagAgrees(run, r.entry.etag, r.entry.md5, r.entry.size))
        {
            GLOGW("assets: file %d failed its ETag check (etag %s, md5 %s, %llu bytes)", index,
                r.entry.etag.c_str(), r.entry.md5.c_str(), r.entry.size);
            DeleteFileW(tmp.c_str());
            r.res = Res::Transient;
            r.why.badHash = true;
            return r;
        }
        if (isZero)
        {
            if (SameBytes(tmp, final))
            {
                DeleteFileW(tmp.c_str());
                r.res = Res::Cached; // the cached copy stands; the new validators describe it
            }
            else
                r.res = Res::Downloaded; // the caller renames it
            return r;
        }
        if (!MoveReplace(tmp, final))
        {
            DeleteFileW(tmp.c_str());
            return fatal(L"Could not save server files to disk");
        }
        AppendJournal(run, index, r.entry);
        r.res = Res::Downloaded;
        return r;
    }

    // ---- end-of-list bookkeeping (caller holds run.m) ----

    enum class Verdict { Wait, PastEnd, MidList };

    // What a non-present answer at `index` means. Only the counter's verified
    // end decides; a worker never ends the list on its own.
    Verdict ClassifyAbsent(const Run& run, int index)
    {
        if (run.endE == kNoIndex)
            return Verdict::Wait;
        return index >= run.endE ? Verdict::PastEnd : Verdict::MidList;
    }

    // Schedule the next try of a failing file below the end, or fail the run.
    void Retry(Run& run, RetryItem it)
    {
        if (it.attempt >= (it.why.badHash ? kMaxHashRetries : kMaxRetries))
        {
            FailLocked(run, FailText(run, it.index, it.why));
            return;
        }
        GLOGW("assets: file %d: HTTP %lu, error %lu%s%s%s; retry %d in %lu ms", it.index, it.why.status,
            it.why.err, WinHttpErrName(it.why.err), it.why.broken ? ", body broke off" : "",
            it.why.badHash ? ", failed its ETag check" : "", it.attempt + 1, kBackoffMs[it.attempt]);
        it.due = GetTickCount64() + kBackoffMs[it.attempt];
        it.attempt++;
        run.retries.push_back(it);
    }

    // A file answered at/after the published end: the count is wrong.
    void Contradict(Run& run, int index)
    {
        if (run.endE != kNoIndex)
        {
            GLOGW("assets: file %d exists past the counted end %d; counting again", index, run.endE);
            run.endE = kNoIndex;
            run.total = -1;
        }
        run.recountFrom = std::max(run.recountFrom, index);
        run.cv.notify_all();
    }

    void Publish(Run& run, int E)
    {
        run.endE = E;
        run.total = E;
        run.suspect = kNoIndex;
        if (!run.countMs.load())
            run.countMs = GetTickCount64() - run.startTick;
        for (auto& kv : run.parked)
        {
            if (kv.first < E)
                Retry(run, kv.second);
            else
                run.dropped[kv.first] = kv.second;
        }
        run.parked.clear();
        for (auto it = run.dropped.begin(); it != run.dropped.end();)
        {
            if (it->first < E) // the end moved up past an index once thought absent
            {
                Retry(run, it->second);
                it = run.dropped.erase(it);
            }
            else
                ++it;
        }
        run.cv.notify_all();
    }

    void Settle(Run& run, int index, int attempt, const FileRes& r)
    {
        switch (r.res)
        {
        case Res::Downloaded:
        case Res::Cached:
        {
            Done d;
            d.entry = r.entry;
            d.cached = r.res == Res::Cached;
            d.bytes = r.bytes;
            run.done[index] = d;
            run.liveFiles++;
            if (d.cached)
                run.liveCached++;
            if (run.endE != kNoIndex && index >= run.endE)
                Contradict(run, index);
            break;
        }
        case Res::NonPresent:
        {
            RetryItem it;
            it.index = index;
            it.attempt = attempt;
            it.why = r.why;
            switch (ClassifyAbsent(run, index))
            {
            case Verdict::Wait:
                run.parked[index] = it;
                run.suspect = std::min(run.suspect, index);
                break;
            case Verdict::PastEnd:
                run.dropped[index] = it;
                break;
            case Verdict::MidList:
                Retry(run, it);
                break;
            }
            break;
        }
        case Res::Transient:
        {
            if (run.endE != kNoIndex && index >= run.endE)
                Contradict(run, index); // it answered 2xx, so it exists
            RetryItem it;
            it.index = index;
            it.attempt = attempt;
            it.why = r.why;
            Retry(run, it);
            break;
        }
        case Res::Fatal:
            FailLocked(run, r.fatal);
            break;
        }
        run.cv.notify_all();
    }

    void Worker(Run* run)
    {
        WinHttpHandle conn(WinHttpConnect(run->session, run->base.host.c_str(), run->base.port, 0));
        std::unique_lock<std::mutex> lk(run->m);
        if (!conn.h)
        {
            GLOGW("assets: WinHttpConnect failed: %lu", GetLastError());
            FailLocked(*run, L"Network setup failed");
            return;
        }
        auto fetch = [&](int index, int attempt)
        {
            run->inFlight++;
            lk.unlock();
            FileRes r = FetchFile(*run, conn.h, index, false, false);
            lk.lock();
            run->inFlight--;
            Settle(*run, index, attempt, r);
        };
        for (;;)
        {
            if (run->failed.load())
                return;
            // 1) a retry that is due (a file below the end that is failing)
            const ULONGLONG now = GetTickCount64();
            auto due = std::find_if(run->retries.begin(), run->retries.end(),
                [&](const RetryItem& it) { return it.due <= now; });
            if (due != run->retries.end())
            {
                RetryItem it = *due;
                run->retries.erase(due);
                if (!it.why.broken && !it.why.badHash && run->endE != kNoIndex && it.index >= run->endE)
                {
                    run->dropped[it.index] = it; // the end moved below it meanwhile
                    continue;
                }
                fetch(it.index, it.attempt);
                continue;
            }
            // 2) the next index (never past the end, nor past a non-present
            //    index while the end is still being counted)
            const int limit = std::min(run->endE != kNoIndex ? run->endE : run->suspect, run->maxFiles);
            if (run->nextIndex < limit)
            {
                fetch(run->nextIndex++, 0);
                continue;
            }
            // 3) nothing new to start: help a big file with a range. New files
            //    come first: they keep every connection busy with fewer
            //    requests; ranges matter when few files are left (the tail).
            std::shared_ptr<BigFile> bf;
            int c = ClaimAny(*run, bf);
            if (c >= 0)
            {
                lk.unlock();
                FetchChunk(*run, *bf, c);
                lk.lock();
                continue;
            }
            // 4) all done once the end is verified and nothing can bring new work
            if (run->endE != kNoIndex && run->recountFrom < 0 && run->retries.empty()
                && run->parked.empty() && run->inFlight == 0 && run->big.empty())
                return;
            run->cv.wait_for(lk, std::chrono::milliseconds(50));
        }
    }

    // ---- the counter ----

    // One 1-byte probe. Present = 2xx and not text/html; anything else (404,
    // html, 5xx, other status, no response) is absent.
    bool Probe(Run& run, int index, int waitMs)
    {
        run.probes++;
        WinHttpHandle conn(WinHttpConnect(run.probeSession, run.base.host.c_str(), run.base.port, 0));
        if (!conn.h)
            return false;
        WinHttpHandle req;
        DWORD status = 0, err = 0;
        if (!SendGet(conn.h, run.base.path + L"/file/" + std::to_wstring(index), run.base.secure,
                     L"Range: bytes=0-0\r\n", waitMs, waitMs, req, status, err))
            return false;
        return status >= 200 && status < 300 && !IsHtml(req.h);
    }

    std::vector<char> ProbeMany(Run& run, const std::vector<int>& pts, int waitMs)
    {
        std::vector<char> got(pts.size(), 0);
        std::vector<std::thread> threads;
        for (size_t i = 0; i < pts.size(); ++i)
            threads.emplace_back([&run, &pts, &got, i, waitMs]() { got[i] = Probe(run, pts[i], waitMs) ? 1 : 0; });
        for (auto& t : threads)
            t.join();
        return got;
    }

    // Probe lo+1, lo+2, lo+4, ... and kMaxFiles at once. Returns the first
    // absent point and moves `lo` to the last present point below it; -1 when
    // even index kMaxFiles answers (too many files).
    int Gallop(Run& run, int& lo, std::map<int, bool>& known)
    {
        if (lo >= kMaxFiles)
            return -1;
        std::vector<int> pts;
        for (long long step = 1; lo + step < kMaxFiles; step *= 2)
            pts.push_back(lo + (int)step);
        pts.push_back(kMaxFiles);
        std::vector<char> got = ProbeMany(run, pts, kProbeMs);
        for (size_t i = 0; i < pts.size(); ++i)
            known[pts[i]] = got[i] != 0;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            if (!got[i])
                return pts[i];
            lo = pts[i];
        }
        return -1;
    }

    // Shrink (lo present, hi absent) to adjacent indices, ~17 ways per round.
    void Bisect(Run& run, int& lo, int& hi, std::map<int, bool>& known)
    {
        while (hi - lo > 1 && !run.Stopping())
        {
            const int step = std::max(1, (hi - lo + kSearchWays - 1) / kSearchWays);
            std::vector<int> pts;
            for (int x = lo + step; x < hi; x += step)
                pts.push_back(x);
            std::vector<char> got = ProbeMany(run, pts, kProbeMs);
            for (size_t i = 0; i < pts.size(); ++i)
                known[pts[i]] = got[i] != 0;
            for (size_t i = 0; i < pts.size(); ++i)
            {
                if (!got[i])
                {
                    hi = pts[i];
                    break;
                }
                lo = pts[i];
            }
        }
    }

    // Re-check a boundary E: E-1 present, E, E+1, E+2 absent, each absent
    // confirmed by a second, longer probe. 0 = verified; > 0 = an index at or
    // after E answered (search above it); -1 = E-1 did not answer.
    int Verify(Run& run, int E, std::map<int, bool>& known)
    {
        const std::vector<int> pts = { E - 1, E, E + 1, E + 2 };
        std::vector<char> got = ProbeMany(run, pts, kProbeMs);
        for (size_t i = 0; i < pts.size(); ++i)
            known[pts[i]] = got[i] != 0;
        int above = 0;
        for (size_t i = 1; i < pts.size(); ++i)
            if (got[i])
                above = pts[i];
        if (above)
            return above;
        if (!got[0])
            return -1;
        Sleep(kBackoffMs[0]);
        const std::vector<int> again = { E, E + 1, E + 2 };
        got = ProbeMany(run, again, kConfirmMs);
        for (size_t i = 0; i < again.size(); ++i)
            if (got[i])
            {
                known[again[i]] = true;
                above = again[i];
            }
        return above;
    }

    // Find the verified file count, starting from `lo` (known present).
    bool FindEnd(Run& run, int lo, int& E, std::wstring& err)
    {
        std::map<int, bool> known;
        known[0] = true; // /file/0 was just downloaded
        known[lo] = true;
        int hi = Gallop(run, lo, known);
        for (int round = 0; round < kMaxRecounts && !run.Stopping(); ++round)
        {
            if (hi < 0)
            {
                err = L"Server lists too many files (over " + std::to_wstring(kMaxFiles) + L")";
                return false;
            }
            Bisect(run, lo, hi, known);
            if (run.Stopping())
                break;
            const int v = Verify(run, hi, known);
            if (v == 0)
            {
                E = hi;
                return true;
            }
            GLOGW("assets: count boundary %d did not verify (%d); searching again", hi, v);
            if (v > 0)
            {
                lo = v;
                hi = Gallop(run, lo, known);
                continue;
            }
            // E-1 stopped answering: the boundary is lower.
            hi = hi - 1;
            lo = -1;
            for (const auto& kv : known)
                if (kv.second && kv.first < hi)
                    lo = kv.first; // the map is ascending: the last match is the largest
            if (lo < 0 || hi <= lo)
                break;
        }
        err = L"Could not count the server's files";
        return false;
    }

    void Counter(Run* run)
    {
        int lo = 0;
        for (int round = 0; ; ++round)
        {
            int E = 0;
            std::wstring err;
            const bool ok = round < kMaxRecounts && FindEnd(*run, lo, E, err);
            std::unique_lock<std::mutex> lk(run->m);
            if (run->Stopping())
                return;
            if (!ok)
            {
                FailLocked(*run, err.empty() ? L"Could not count the server's files" : err);
                return;
            }
            // A worker may already hold a file at/after E (it ran ahead).
            int beyond = run->recountFrom >= E ? run->recountFrom : -1;
            for (auto it = run->done.lower_bound(E); it != run->done.end(); ++it)
                beyond = std::max(beyond, it->first);
            run->recountFrom = -1;
            if (beyond >= 0)
            {
                GLOGW("assets: file %d exists past the counted end %d; counting again", beyond, E);
                lo = beyond;
                continue;
            }
            GLOGI("assets: %s has %d file(s) (counted in %.1f s, %d probe(s))", Narrow(run->label).c_str(),
                E, (double)(GetTickCount64() - run->startTick) / 1000.0, run->probes.load());
            Publish(*run, E);
            run->cv.wait(lk, [&]() { return run->Stopping() || run->recountFrom >= 0; });
            if (run->Stopping())
                return;
            lo = run->recountFrom;
            run->recountFrom = -1;
        }
    }

    // ---- cache checks ------------------------------------------------------

    // Fast-path guard: every file below `count` is on disk at its recorded size.
    bool CacheIntact(const Run& run, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            auto it = run.old.find(i);
            unsigned long long size = 0;
            if (it == run.old.end() || !FileSizeOf(run.fileDir + std::to_wstring(i), size)
                || size != it->second.size)
            {
                GLOGW("assets: cached file %d is missing or the wrong size; checking the server", i);
                return false;
            }
        }
        return true;
    }

    // "Verify files": re-hash every cached blob against its recorded MD5 and
    // delete the ones that differ (the run then fetches them again). Blobs
    // recorded before MD5s were kept get one recorded now. Returns the number
    // deleted.
    int VerifyLocal(Run& run)
    {
        int bad = 0, checked = 0;
        for (auto& kv : run.old)
        {
            const std::wstring blob = run.fileDir + std::to_wstring(kv.first);
            unsigned long long size = 0;
            if (!FileSizeOf(blob, size))
                continue;
            std::string md5 = FileMd5(blob);
            if (md5.empty())
                continue;
            checked++;
            if (size != kv.second.size || (!kv.second.md5.empty() && !EqualHex(md5, kv.second.md5)))
            {
                GLOGW("assets: verify: file %d is damaged (md5 %s, recorded %s); fetching it again",
                    kv.first, md5.c_str(), kv.second.md5.c_str());
                if (DeleteFileW(blob.c_str()))
                    bad++;
                continue;
            }
            kv.second.md5 = md5;
        }
        GLOGI("assets: verify: %d cached file(s) checked, %d damaged", checked, bad);
        return bad;
    }

    // Delete file\<N> for N >= count and any leftover *.tmp, inside fileDir only.
    void RemoveStale(const std::wstring& fileDir, int count)
    {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((fileDir + L"*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE)
            return;
        int removed = 0;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            std::string name = Narrow(fd.cFileName);
            bool tmp = name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0;
            unsigned long long n = 0;
            if (!ParseDigits(tmp ? name.substr(0, name.size() - 4) : name, n))
                continue; // not ours
            if (tmp || n >= (unsigned long long)count)
            {
                if (DeleteFileW((fileDir + fd.cFileName).c_str()))
                    removed++;
                else
                    GLOGW("assets: could not remove %s: %lu", name.c_str(), GetLastError());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        if (removed)
            GLOGI("assets: removed %d stale file(s)", removed);
    }

    // Decide the run's ETag scheme from /file/0 (its body is `md5` hex).
    int DetectHash(const Entry& e0, int index0Label)
    {
        const std::string tag = StripEtag(e0.etag);
        unsigned long long size = 0;
        if (IsMd5Tag(tag))
        {
            if (EqualHex(tag, e0.md5))
            {
                GLOGI("assets: ETags are MD5 hashes; every file is checked");
                return HashMd5;
            }
            GLOGI("assets: file %d's 32-hex ETag is not its MD5; hash checks off", index0Label);
            return HashNone;
        }
        if (ParseSizeMtime(tag, size))
        {
            if (size == e0.size)
            {
                GLOGI("assets: nginx size-mtime ETags; every file's size is checked");
                return HashSizeMtime;
            }
            GLOGI("assets: file %d's ETag does not name its size; checks off", index0Label);
        }
        return HashNone;
    }

    AssetResult RunDownload(const std::wstring& baseUrl, const std::wstring& label,
                            const std::wstring& dirIn, bool verifyLocal,
                            const std::function<void(const AssetProgress&)>& onProgress,
                            const AssetTestOptions* test)
    {
        AssetResult res;
        auto fail = [&](const std::wstring& why)
        {
            res = AssetResult();
            res.error = why;
            GLOGW("assets: %s: %s", Narrow(baseUrl).c_str(), Narrow(why).c_str());
            return res;
        };

        Run run;
        run.startTick = GetTickCount64();
        run.lastReport = run.startTick;
        run.onProgress = &onProgress;
        run.label = label;
        if (test)
        {
            if (test->workers > 0)
                run.workers = test->workers;
            run.chunking = !test->noChunks;
            if (test->chunkMinBytes)
                run.chunkMin = test->chunkMinBytes;
            if (test->chunkBytes)
                run.chunkSize = test->chunkBytes;
            if (test->maxFiles > 0)
                run.maxFiles = std::min(test->maxFiles, kMaxFiles);
        }

        if (baseUrl.size() > 2048 || !CrackTarget(baseUrl, run.base, false))
            return fail(L"Invalid server address");
        while (!run.base.path.empty() && run.base.path.back() == L'/')
            run.base.path.pop_back();
        if (run.label.empty())
            run.label = run.base.host + L":" + std::to_wstring(run.base.port);

        run.dir = dirIn;
        std::replace(run.dir.begin(), run.dir.end(), L'/', L'\\');
        if (run.dir.empty())
            return fail(L"Could not create the server files folder");
        if (run.dir.back() != L'\\')
            run.dir += L'\\';
        run.fileDir = run.dir + L"file\\";
        if (!MakeDirs(run.fileDir))
            return fail(L"Could not create the server files folder");
        const std::wstring idxPath = run.dir + L"files.idx";
        const std::wstring completePath = run.dir + L"complete";
        run.journalPath = run.dir + L"files.idx.journal";
        size_t journalled = 0;
        run.old = LoadIndex(idxPath, run.journalPath, journalled);

        GLOGI("assets: %s -> %s (%zu cached entr(ies), %zu from the journal)", Narrow(baseUrl).c_str(),
            Narrow(run.dir).c_str(), run.old.size(), journalled);

        int verifyBad = 0;
        if (verifyLocal)
            verifyBad = VerifyLocal(run);

        WinHttpHandle session(OpenSession(kResolveMs, kConnectMs, kSendMs, kReceiveMs));
        WinHttpHandle probeSession(OpenSession(kProbeMs, kProbeMs, kProbeMs, kProbeMs));
        if (!session.h || !probeSession.h)
        {
            GLOGW("assets: WinHTTP setup failed: %lu", GetLastError());
            return fail(L"Network setup failed");
        }
        run.session = session.h;
        run.probeSession = probeSession.h;

        auto fillStats = [&]()
        {
            if (test && test->stats)
            {
                test->stats->total = run.total.load();
                test->stats->probes = run.probes.load();
                test->stats->countSeconds = (double)run.countMs.load() / 1000.0;
                test->stats->hashMode = run.hash;
                test->stats->verifyBad = verifyBad;
            }
        };

        // ---- test: the counter alone ----
        if (test && test->countOnly)
        {
            if (!Probe(run, 0, kConfirmMs))
            {
                res.ok = true;
                fillStats();
                return res;
            }
            int E = 0;
            std::wstring err;
            if (!FindEnd(run, 0, E, err))
            {
                fillStats();
                return fail(err);
            }
            run.total = E;
            run.countMs = GetTickCount64() - run.startTick;
            fillStats();
            res.ok = true;
            res.files = E;
            return res;
        }

        const bool only = test && test->onlyIndex >= 0;
        if (only)
        {
            // ---- test: one index, no counter, no ETag scheme ----
            if (test->onlyIndex >= kMaxFiles)
                return fail(L"Invalid file index");
            if (!DeleteFileW(completePath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                return fail(L"Could not save server files to disk");
            run.nextIndex = test->onlyIndex;
            run.endE = test->onlyIndex + 1;
            run.total = 1;
        }
        else
        {
            // ---- /file/0: the fast path ----
            WinHttpHandle conn0(WinHttpConnect(session.h, run.base.host.c_str(), run.base.port, 0));
            if (!conn0.h)
                return fail(L"Network setup failed");
            const std::wstring final0 = run.fileDir + L"0";
            const std::wstring tmp0 = final0 + L".tmp";
            FileRes r0 = FetchFile(run, conn0.h, 0, true, false);
            auto noAssets = [](const FileRes& r) { return r.res == Res::NonPresent && (r.why.status == 404 || r.why.html); };
            if ((r0.res == Res::NonPresent && !noAssets(r0)) || r0.res == Res::Transient)
            {
                GLOGW("assets: file 0: HTTP %lu, error %lu%s; retrying", r0.why.status, r0.why.err,
                    WinHttpErrName(r0.why.err));
                Sleep(kBackoffMs[0]);
                r0 = FetchFile(run, conn0.h, 0, true, false);
            }
            if (noAssets(r0))
            {
                // No assets. Keep the blobs (a CDN hiccup must not wipe a big
                // cache; they are revalidated next time) but drop the marker.
                if (!DeleteFileW(completePath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                    return fail(L"Could not save server files to disk");
                GLOGI("assets: %s has no files", Narrow(run.label).c_str());
                res.ok = true;
                return res;
            }
            if (r0.res == Res::NonPresent || r0.res == Res::Transient)
                return fail(FailText(run, 0, r0.why));
            if (r0.res == Res::Fatal)
                return fail(r0.fatal);

            int completeCount = 0;
            if (r0.res == Res::Cached && ReadComplete(completePath, completeCount) && CacheIntact(run, completeCount))
            {
                res.ok = true;
                res.upToDate = true;
                res.files = completeCount;
                res.fromCache = completeCount;
                res.bytes = r0.bytes;
                fillStats();
                GLOGI("assets: %s up to date (%d file(s))", Narrow(run.label).c_str(), completeCount);
                return res;
            }

            // The ETag scheme, from file 0 (a 304 kept the local copy: hash that).
            if (r0.entry.md5.empty())
                r0.entry.md5 = FileMd5(r0.res == Res::Downloaded ? tmp0 : final0);
            run.hash = DetectHash(r0.entry, 0);

            // From here on the cache is being changed: the marker goes first, so
            // an interrupted run can never pass for a complete one.
            if (!DeleteFileW(completePath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            {
                DeleteFileW(tmp0.c_str());
                return fail(L"Could not save server files to disk");
            }
            if (r0.res == Res::Downloaded)
            {
                if (!MoveReplace(tmp0, final0))
                {
                    DeleteFileW(tmp0.c_str());
                    return fail(L"Could not save server files to disk");
                }
            }
            AppendJournal(run, 0, r0.entry);
            std::lock_guard<std::mutex> g(run.m);
            Settle(run, 0, 0, r0);
        }

        // ---- the counter and the workers ----
        std::thread counter;
        if (!only)
            counter = std::thread(Counter, &run);
        std::vector<std::thread> workers;
        for (int i = 0; i < run.workers; ++i)
            workers.emplace_back(Worker, &run);
        for (auto& t : workers)
            t.join();
        {
            std::lock_guard<std::mutex> g(run.m);
            run.finished = true;
            run.cv.notify_all();
        }
        if (counter.joinable())
            counter.join();
        CloseJournal(run);
        fillStats();

        // The new index: what was cached, overlaid with every file this run
        // wrote or verified (so the index always describes the blobs on disk).
        Index idx = run.old;
        for (const auto& kv : run.done)
            idx[kv.first] = kv.second.entry;

        if (run.failed.load() || run.endE == kNoIndex)
        {
            if (SaveIndex(idxPath, idx))
                DeleteFileW(run.journalPath.c_str());
            else
                GLOGW("assets: could not write files.idx: %lu", GetLastError());
            return fail(run.error.empty() ? L"Download failed" : run.error);
        }

        // Success: exactly the files below the end.
        const int first = only ? test->onlyIndex : 0;
        const int count = std::min(run.endE, run.maxFiles);
        Index finalIdx;
        for (int i = first; i < count; ++i)
        {
            auto it = run.done.find(i);
            if (it == run.done.end()) // cannot happen: every index below the end finished or failed
            {
                SaveIndex(idxPath, idx);
                return fail(L"Download failed (file " + std::to_wstring(i) + L")");
            }
            finalIdx[i] = it->second.entry;
            if (it->second.cached)
                res.fromCache++;
            else
                res.downloaded++;
            res.bytes += it->second.bytes;
        }
        const bool fullRun = !only && run.maxFiles >= kMaxFiles;
        if (fullRun)
            RemoveStale(run.fileDir, count);
        if (!SaveIndex(idxPath, fullRun ? finalIdx : idx))
            return fail(L"Could not save server files to disk");
        DeleteFileW(run.journalPath.c_str());
        if (fullRun && !WriteAtomic(completePath, std::to_string(count)))
            return fail(L"Could not save server files to disk");

        res.ok = true;
        res.files = count - first;
        GLOGI("assets: %s: %d file(s), %d downloaded, %d from cache, %llu bytes in %.1f s",
            Narrow(run.label).c_str(), res.files, res.downloaded, res.fromCache, res.bytes,
            (double)(GetTickCount64() - run.startTick) / 1000.0);
        return res;
    }

}

namespace gage::net {

    std::wstring ServerAssetId(const std::wstring& host, int port)
    {
        std::wstring key = host + L":" + std::to_wstring(port);
        if (!key.empty())
        {
            int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, key.c_str(), (int)key.size(),
                                  nullptr, 0, nullptr, nullptr, 0);
            if (n > 0)
            {
                std::wstring low((size_t)n, L'\0');
                if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, key.c_str(), (int)key.size(),
                                  &low[0], n, nullptr, nullptr, 0) == n)
                    key = low;
            }
        }
        unsigned char digest[32];
        if (!Sha256(Narrow(key), digest))
        {
            GLOGW("assets: SHA-256 provider failed");
            return L"";
        }
        static const wchar_t hex[] = L"0123456789abcdef";
        std::wstring id;
        for (int i = 0; i < 16; ++i)
        {
            id += hex[digest[i] >> 4];
            id += hex[digest[i] & 15];
        }
        return id;
    }

    AssetResult DownloadServerAssets(const std::wstring& host, int port, const std::wstring& dir,
                                     const AssetOptions& options,
                                     const std::function<void(const AssetProgress&)>& onProgress)
    {
        if (host.empty() || port < 1 || port > 65534
            || host.find_first_of(L" \t\r\n/?#@") != std::wstring::npos)
        {
            AssetResult res;
            res.error = L"Invalid server address";
            return res;
        }
        // An IPv6 literal needs brackets in a URL.
        std::wstring h = (host.find(L':') != std::wstring::npos && host[0] != L'[') ? L"[" + host + L"]" : host;
        std::wstring hostPort = h + L":" + std::to_wstring(port + 1);
        return RunDownload(L"http://" + hostPort, hostPort, dir, options.verifyLocal, onProgress, nullptr);
    }

    AssetResult DownloadServerAssets(const std::wstring& host, int port, const std::wstring& dir,
                                     const std::function<void(const AssetProgress&)>& onProgress)
    {
        return DownloadServerAssets(host, port, dir, AssetOptions(), onProgress);
    }

    AssetResult DownloadAssetsFrom(const std::wstring& baseUrl, const std::wstring& dir,
                                   const std::function<void(const AssetProgress&)>& onProgress)
    {
        return RunDownload(baseUrl, L"", dir, false, onProgress, nullptr);
    }

    AssetResult DownloadAssetsTest(const std::wstring& baseUrl, const std::wstring& dir,
                                   const AssetTestOptions& options,
                                   const std::function<void(const AssetProgress&)>& onProgress)
    {
        return RunDownload(baseUrl, L"", dir, options.verifyLocal, onProgress, &options);
    }

}
