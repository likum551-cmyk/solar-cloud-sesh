#include "common.h"
#include <windows.h>

namespace gage {

    std::wstring ExeDir()
    {
        // Resolve the module that contains *this function* (the exe, or the
        // injected DLL gage_common was linked into), not the host process exe:
        // GetModuleFileNameW(nullptr) would return GTA5.exe inside a DLL.
        HMODULE mod = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ExeDir), &mod) || !mod)
            return L".\\";

        // Grow the buffer until the path fits (long-path installs exceed MAX_PATH).
        std::wstring path;
        for (DWORD cap = MAX_PATH; cap <= 65536; cap *= 2)
        {
            path.assign(cap, L'\0');
            DWORD n = GetModuleFileNameW(mod, &path[0], cap);
            if (n == 0)
                return L".\\";
            if (n < cap) // n == cap means truncated
            {
                path.resize(n);
                size_t slash = path.find_last_of(L"\\/");
                if (slash == std::wstring::npos)
                    return L".\\";
                return path.substr(0, slash + 1);
            }
        }
        return L".\\";
    }

    std::string Narrow(const std::wstring& wide)
    {
        if (wide.empty())
            return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0)
            return {};
        std::string out((size_t)n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring Widen(const std::string& narrow)
    {
        if (narrow.empty())
            return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0);
        if (n <= 0)
            return {};
        std::wstring out((size_t)n - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, out.data(), n);
        return out;
    }

    bool IsAdmin()
    {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
            return false;
        TOKEN_ELEVATION elev{};
        DWORD sz = 0;
        BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &sz);
        CloseHandle(tok);
        return ok && elev.TokenIsElevated;
    }

    bool FileExists(const std::wstring& path)
    {
        DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    // The game exe in `dir` (GTA5.exe first), or "".
    static std::wstring GameExeIn(std::wstring dir)
    {
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
            dir.pop_back();
        if (dir.empty())
            return L"";
        for (const wchar_t* exe : { L"\\GTA5.exe", L"\\GTA5_Enhanced.exe" })
            if (FileExists(dir + exe))
                return dir + exe;
        return L"";
    }

    // Find GTA V: first where the stores record it (the Rockstar, Steam and Epic
    // install keys), then the standard install folders.
    std::wstring FindGameExe()
    {
        struct { HKEY root; const wchar_t* key; const wchar_t* value; } regs[] =
        {
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Rockstar Games\\Grand Theft Auto V", L"InstallFolder" },
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Rockstar Games\\GTAV", L"InstallFolderSteam" },
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Rockstar Games\\GTAV Enhanced", L"InstallFolder" },
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Rockstar Games\\GTAV Enhanced", L"InstallFolderSteam" },
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Rockstar Games\\Grand Theft Auto V", L"InstallFolderEpic" },
        };
        for (const auto& r : regs)
        {
            wchar_t buf[MAX_PATH * 2] = L"";
            DWORD bytes = sizeof(buf);
            if (RegGetValueW(r.root, r.key, r.value, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS)
                continue;
            std::wstring dir = buf;
            std::wstring exe = GameExeIn(dir);
            // the Steam value has been seen to carry a stray "GTAV" after the folder
            if (exe.empty() && dir.size() > 4 && _wcsicmp(dir.c_str() + dir.size() - 4, L"GTAV") == 0)
                exe = GameExeIn(dir.substr(0, dir.size() - 4));
            if (!exe.empty())
                return exe;
        }

        const wchar_t* envs[2] = { L"ProgramFiles", L"ProgramFiles(x86)" };
        const wchar_t* subs[] =
        {
            L"Rockstar Games\\Grand Theft Auto V\\GTA5.exe",
            L"Rockstar Games\\Grand Theft Auto V\\GTA5_Enhanced.exe",
            L"Steam\\steamapps\\common\\Grand Theft Auto V\\GTA5.exe",
            L"Epic Games\\Grand Theft Auto V\\GTA5.exe",
        };
        for (int e = 0; e < 2; ++e)
        {
            wchar_t base[MAX_PATH] = L"";
            if (GetEnvironmentVariableW(envs[e], base, MAX_PATH) == 0)
                continue;
            for (int s = 0; s < (int)(sizeof(subs) / sizeof(subs[0])); ++s)
            {
                std::wstring p = std::wstring(base) + L"\\" + subs[s];
                if (FileExists(p))
                    return p;
            }
        }
        return L"";
    }

    std::wstring FormatError(DWORD code)
    {
        // FormatMessage turns 0 into "The operation completed successfully.",
        // which reads as a success when a caller reports a failure with a stale
        // GetLastError() of 0.
        if (code == 0)
            return L"no error code reported (0)";

        wchar_t buf[512];
        DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
            nullptr, code, 0, buf, 512, nullptr);
        if (n == 0)
        {
            swprintf(buf, 512, L"0x%08X", (unsigned)code);
            return buf;
        }
        // MAX_WIDTH_MASK leaves a trailing space in addition to any CR/LF.
        while (n > 0 && (buf[n - 1] == L'\n' || buf[n - 1] == L'\r'
                         || buf[n - 1] == L' ' || buf[n - 1] == L'\t'))
            buf[--n] = L'\0';
        return std::wstring(buf, n);
    }

}
