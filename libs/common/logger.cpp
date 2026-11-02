#include "logger.h"
#include <cstdarg>
#include <cstring>

namespace gage::log {

    // One process-wide lock guards the file handle and the "where to log"
    // state. SRWLOCK_INIT is constant-initialised, so it is usable from the
    // very first call even before static constructors have run (injected DLLs).
    static SRWLOCK      g_lock    = SRWLOCK_INIT;
    static FILE*        g_fp      = nullptr;
    static std::wstring g_file;          // requested path; empty = <ExeDir>gage.log
    static ULONGLONG    g_retryAt = 0;   // after a failed open, don't retry before this tick

    static const ULONGLONG kRotateBytes = 1024ull * 1024ull;
    static const ULONGLONG kRetryMs     = 5000;
    static const int       kMaxLine     = 2048; // whole line incl. prefix and "\n"

    // Move a log larger than 1 MiB to "<path>.1" (replacing the previous one).
    // Fails harmlessly if another process still holds the log open; we then
    // simply keep appending.
    static void rotateIfLarge(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
            return;
        ULONGLONG size = ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        if (size <= kRotateBytes)
            return;
        MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    static FILE* openAt(const std::wstring& path)
    {
        rotateIfLarge(path);
        return _wfopen(path.c_str(), L"a"); // text mode: "\n" becomes CRLF on disk
    }

    // Open g_file, else %LOCALAPPDATA%\GAGE-MP\gage.log. Never falls back to
    // stdout: the launcher is a GUI-subsystem exe with no console. On failure
    // g_fp stays null and the next attempt is delayed so a dead path does not
    // cost a filesystem hit per log line. Caller holds g_lock.
    static void openLocked()
    {
        if (g_file.empty())
            g_file = gage::ExeDir() + L"gage.log";

        g_fp = openAt(g_file);
        if (!g_fp)
        {
            wchar_t base[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                std::wstring dir(base, n);
                while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
                    dir.pop_back();
                dir += L"\\GAGE-MP";
                CreateDirectoryW(dir.c_str(), nullptr); // already-exists is fine
                g_fp = openAt(dir + L"\\gage.log");
            }
        }
        g_retryAt = g_fp ? 0 : GetTickCount64() + kRetryMs;
    }

    // Caller holds g_lock. True when g_fp is usable.
    static bool ensureLocked()
    {
        if (g_fp)
            return true;
        if (g_retryAt != 0 && GetTickCount64() < g_retryAt)
            return false;
        openLocked();
        return g_fp != nullptr;
    }

    void init(const std::wstring& path)
    {
        AcquireSRWLockExclusive(&g_lock);
        if (g_fp)
        {
            std::fclose(g_fp);
            g_fp = nullptr;
        }
        g_file = path;
        g_retryAt = 0;
        openLocked();
        ReleaseSRWLockExclusive(&g_lock);
    }

    // Length to keep so that cutting s[0..n) never leaves half of a UTF-8
    // multi-byte sequence at the end.
    static int utf8SafeLen(const char* s, int n)
    {
        if (n <= 0)
            return 0;
        int start = n - 1;
        while (start > 0 && n - 1 - start < 3 && ((unsigned char)s[start] & 0xC0) == 0x80)
            --start;
        unsigned char lead = (unsigned char)s[start];
        int need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
        return (need > n - start) ? start : n;
    }

    static void vwrite(const char* tag, const char* fmt, va_list ap)
    {
        // Build "[hh:mm:ss.mmm] TAG body\n" in one fixed buffer, clamped.
        char line[kMaxLine];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int len = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag);
        if (len < 0)
            len = 0;
        if (len > 64) // tags are short literals; defensive clamp
            len = 64;

        // vsnprintf returns the length the output WOULD have had; the buffer
        // holds at most cap-1 characters of it plus the NUL.
        const int cap = kMaxLine - len - 1;
        int n = vsnprintf(line + len, (size_t)cap, fmt, ap);
        if (n < 0)
            n = 0;
        if (n >= cap) // truncated: keep room for "..." and cut on a character boundary
        {
            n = utf8SafeLen(line + len, cap - 4);
            memcpy(line + len + n, "...", 3);
            n += 3;
        }
        line[len + n] = '\n';
        const int total = len + n + 1;
        line[total] = '\0';

        AcquireSRWLockExclusive(&g_lock);
        if (ensureLocked())
        {
            std::fwrite(line, 1, (size_t)total, g_fp);
            std::fflush(g_fp);
        }
        ReleaseSRWLockExclusive(&g_lock);

        // Mirror to the console for headless / self-test runs, but only when
        // this process really has a stdout (the GUI exe and injected game
        // processes often do not). Outside the lock: a stalled console must not
        // block other threads' logging.
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out != nullptr && out != INVALID_HANDLE_VALUE)
        {
            std::fwrite(line, 1, (size_t)total, stdout);
            std::fflush(stdout);
        }
    }

    void write(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        vwrite("...", fmt, ap);
        va_end(ap);
    }
    void info(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        vwrite("INFO ", fmt, ap);
        va_end(ap);
    }
    void warn(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        vwrite("WARN ", fmt, ap);
        va_end(ap);
    }
    void error(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        vwrite("ERROR", fmt, ap);
        va_end(ap);
    }

}
