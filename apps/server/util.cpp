#include "util.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

namespace gage::server {

    namespace {

        struct Civil
        {
            int y, m, d, hh, mm, ss, ms;
        };

        Civil NowUtc()
        {
            using namespace std::chrono;
            int64_t ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            int64_t days = ms / 86400000;
            int64_t rem = ms % 86400000;
            if (rem < 0) { rem += 86400000; days--; }
            days += 719468;
            const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
            const int64_t doe = days - era * 146097;
            const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            int64_t y = yoe + era * 400;
            const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const int64_t mp = (5 * doy + 2) / 153;
            const int64_t d = doy - (153 * mp + 2) / 5 + 1;
            const int64_t m = mp < 10 ? mp + 3 : mp - 9;
            if (m <= 2) y++;
            return Civil{(int)y, (int)m, (int)d, (int)(rem / 3600000), (int)(rem / 60000 % 60), (int)(rem / 1000 % 60),
                         (int)(rem % 1000)};
        }

    }

    std::string LogTimestamp()
    {
        const Civil c = NowUtc();
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03dZ", c.y, c.m, c.d, c.hh, c.mm, c.ss, c.ms);
        return buf;
    }

    std::string IsoTimestamp()
    {
        const Civil c = NowUtc();
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", c.y, c.m, c.d, c.hh, c.mm, c.ss);
        return buf;
    }

    bool AtomicWriteFile(const std::string& path, const std::string& content, std::string* err)
    {
        namespace fs = std::filesystem;
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        if (path.empty()) return fail("empty path");
        const std::string tmp = path + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return fail("cannot create " + tmp);
        bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size() && std::fflush(f) == 0;
#ifdef _WIN32
        ok = ok && _commit(_fileno(f)) == 0;
#else
        ok = ok && fsync(fileno(f)) == 0;
#endif
        ok = (std::fclose(f) == 0) && ok;
        std::error_code ec;
        if (!ok)
        {
            fs::remove(tmp, ec);
            return fail("cannot write " + tmp + " (disk full or read-only?)");
        }
        // fs::rename replaces an existing target on both platforms (MoveFileEx with
        // REPLACE_EXISTING on Windows, rename(2) elsewhere); std::rename would not on Windows.
        fs::rename(tmp, path, ec);
        if (ec)
        {
            std::error_code ignore;
            fs::remove(tmp, ignore);
            return fail("cannot rename " + tmp + " over " + path + ": " + ec.message());
        }
#ifndef _WIN32
        // Make the rename itself durable: fsync the directory that holds the entry.
        const fs::path dir = fs::path(path).parent_path();
        const int dfd = ::open(dir.empty() ? "." : dir.string().c_str(), O_RDONLY);
        if (dfd >= 0)
        {
            (void)fsync(dfd);
            ::close(dfd);
        }
#endif
        return true;
    }

    bool ReadWholeFile(const std::string& path, std::string& out, std::string* err, bool* missing)
    {
        namespace fs = std::filesystem;
        if (missing) *missing = false;
        // "Missing" is only a path that does not exist. A file that is there but cannot be
        // read (a directory, no permission) is an error: callers treat "missing" as "start
        // from nothing", which must never happen to a file that exists.
        std::error_code ec;
        const fs::file_status st = fs::status(path, ec);
        // not_found is reported WITH an error code on some platforms (MinGW), so test the type first.
        if (st.type() == fs::file_type::not_found)
        {
            if (missing) *missing = true;
            if (err) *err = "no such file";
            return false;
        }
        if (!ec && st.type() == fs::file_type::directory)
        {
            if (err) *err = "is a directory";
            return false;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            if (err) *err = "cannot open (permission denied?)";
            return false;
        }
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (f.bad())
        {
            if (err) *err = "read error";
            return false;
        }
        return true;
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char ch : s)
        {
            const unsigned char c = (unsigned char)ch;
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                    out.push_back(ch);
            }
        }
        return out;
    }

    std::string Trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
        return s.substr(a, b - a);
    }

    bool ParseIpv4(const std::string& s, uint32_t* host)
    {
        uint32_t acc = 0;
        size_t i = 0;
        for (int field = 0; field < 4; field++)
        {
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
            const size_t start = i;
            uint32_t v = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9')
            {
                v = v * 10 + (uint32_t)(s[i] - '0');
                if (v > 255) return false;
                i++;
            }
            if (i - start > 1 && s[start] == '0') return false; // leading zero
            acc = (acc << 8) | v;
            if (field < 3)
            {
                if (i >= s.size() || s[i] != '.') return false;
                i++;
            }
        }
        if (i != s.size()) return false;
        if (host) *host = acc;
        return true;
    }

    double ThreadCpuSeconds()
    {
#ifdef _WIN32
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
        auto ticks = [](const FILETIME& f) {
            return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime);
        };
        return (ticks(kernel) + ticks(user)) * 1e-7; // 100 ns units
#elif defined(CLOCK_THREAD_CPUTIME_ID)
        timespec ts;
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0.0;
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#else
        return 0.0;
#endif
    }

}
