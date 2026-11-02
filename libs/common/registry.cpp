#include "registry.h"
#include "common.h"
#include "logger.h"
#include <windows.h>
#include <cwchar>
#include <vector>

namespace gage::reg {

    // All GAGE-MP settings live under HKCU\Software\GAGE-MP.
    static const wchar_t* kRoot = L"Software\\GAGE-MP";

    // Sanity cap on a stored string (bytes). Paths top out near 64 KiB.
    static const DWORD kMaxValueBytes = 1u << 20;

    std::wstring Get(const std::wstring& key, const std::wstring& def)
    {
        // REG_EXPAND_SZ values come back with %VARS% expanded; RegGetValueW also
        // guarantees a terminator on string types, so we never trust the stored one.
        const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;

        // Size query first.
        DWORD bytes = 0;
        LONG r = RegGetValueW(HKEY_CURRENT_USER, kRoot, key.c_str(), flags,
                              nullptr, nullptr, &bytes);
        if (r != ERROR_SUCCESS)
            return def;

        // Then read. The value can grow between the two calls (and an expanded
        // REG_EXPAND_SZ can be longer than its raw size), so on ERROR_MORE_DATA
        // retry with the size the API reports back.
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            if (bytes < sizeof(wchar_t) || bytes > kMaxValueBytes)
                return def;

            std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
            DWORD got = (DWORD)((buf.size() - 1) * sizeof(wchar_t)); // keep a spare slot
            r = RegGetValueW(HKEY_CURRENT_USER, kRoot, key.c_str(), flags,
                             nullptr, buf.data(), &got);
            if (r == ERROR_MORE_DATA)
            {
                bytes = (got > bytes) ? got : bytes * 2 + 64;
                continue;
            }
            if (r != ERROR_SUCCESS)
                return def;

            buf.back() = L'\0'; // the string ends at the first NUL, whatever was stored
            return std::wstring(buf.data(), wcslen(buf.data()));
        }
        return def;
    }

    bool Set(const std::wstring& key, const std::wstring& value)
    {
        HKEY k = nullptr;
        LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, kRoot, 0, nullptr, 0, KEY_WRITE,
                                 nullptr, &k, nullptr);
        if (r != ERROR_SUCCESS)
        {
            GLOGE("reg: cannot open HKCU\\%s for write (error %lu)",
                  Narrow(kRoot).c_str(), (unsigned long)r);
            return false;
        }

        r = RegSetValueExW(k, key.c_str(), 0, REG_SZ, (const BYTE*)value.c_str(),
            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        LONG rc = RegCloseKey(k);
        if (r != ERROR_SUCCESS)
        {
            GLOGE("reg: cannot set '%s' (error %lu)", Narrow(key).c_str(), (unsigned long)r);
            return false;
        }
        if (rc != ERROR_SUCCESS)
        {
            GLOGE("reg: closing key after setting '%s' failed (error %lu)",
                  Narrow(key).c_str(), (unsigned long)rc);
            return false;
        }
        return true;
    }

    bool Has(const std::wstring& key)
    {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRoot, 0, KEY_READ, &k) != ERROR_SUCCESS)
            return false;
        LONG r = RegQueryValueExW(k, key.c_str(), nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(k);
        return r == ERROR_SUCCESS;
    }

    bool Clear(const std::wstring& key)
    {
        HKEY k = nullptr;
        LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, kRoot, 0, KEY_WRITE, &k);
        if (r == ERROR_FILE_NOT_FOUND)
            return true; // no key, so the value is already absent
        if (r != ERROR_SUCCESS)
        {
            GLOGE("reg: cannot open HKCU\\%s to clear '%s' (error %lu)",
                  Narrow(kRoot).c_str(), Narrow(key).c_str(), (unsigned long)r);
            return false;
        }
        r = RegDeleteValueW(k, key.c_str());
        RegCloseKey(k);
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND)
        {
            GLOGE("reg: cannot clear '%s' (error %lu)", Narrow(key).c_str(), (unsigned long)r);
            return false;
        }
        return true;
    }

}
