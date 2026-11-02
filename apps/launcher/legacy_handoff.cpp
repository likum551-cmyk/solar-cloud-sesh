// legacy_handoff.cpp — see legacy_handoff.h.

#include "legacy_handoff.h"
#include "common/logger.h"
#include "common/registry.h"

#include <vector>

namespace gage::legacy {

    namespace {

        // What the old launcher's DoLaunch wrote, value by value. Nothing outside
        // this list is ever deleted.
        const wchar_t* const kWritten[] = {
            L"launch.ip", L"launch.port", L"player_name", L"game_v_path", L"rage_path", L"lang",
            L"launch.password", L"launch.data0", L"launch.type", L"launch.cefPort", L"type", L"rage-type",
            nullptr,
        };

        bool IsAbsolute(const std::wstring& p) {
            if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/') &&
                ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')))
                return true;
            return p.size() >= 3 && p[0] == L'\\' && p[1] == L'\\'; // UNC
        }

        // Absolute path with "." / ".." resolved, backslashes only and no trailing
        // backslash (a root keeps its own). Pure string work: nothing touches the disk.
        bool Normalize(const std::wstring& in, std::wstring& out) {
            if (!IsAbsolute(in)) return false;
            std::vector<wchar_t> buf(in.size() + 260);
            DWORD n = GetFullPathNameW(in.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
            if (n >= buf.size()) {
                buf.resize((size_t)n + 1);
                n = GetFullPathNameW(in.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
            }
            if (n == 0 || n >= buf.size()) return false;
            out.assign(buf.data(), n);
            while (out.size() > 3 && out.back() == L'\\') out.pop_back();
            return true;
        }

        // A string value of the open key: 1 = read, 0 = absent or not a string,
        // -1 = error (*err holds the registry error code).
        int ReadString(HKEY k, const wchar_t* name, std::wstring& out, LONG* err) {
            const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
            DWORD bytes = 0;
            LONG r = RegGetValueW(k, nullptr, name, flags, nullptr, nullptr, &bytes);
            if (r == ERROR_FILE_NOT_FOUND || r == ERROR_UNSUPPORTED_TYPE) return 0;
            if (r != ERROR_SUCCESS) { *err = r; return -1; }
            std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
            DWORD got = (DWORD)((buf.size() - 1) * sizeof(wchar_t));
            r = RegGetValueW(k, nullptr, name, flags, nullptr, buf.data(), &got);
            if (r == ERROR_FILE_NOT_FOUND || r == ERROR_UNSUPPORTED_TYPE) return 0;
            if (r != ERROR_SUCCESS) { *err = r; return -1; }
            buf.back() = L'\0';
            out.assign(buf.data(), wcslen(buf.data()));
            return 1;
        }

        Result Fail(const std::wstring& what, LONG code) {
            Result r;
            r.outcome = Outcome::Error;
            r.detail = what + L" (error " + std::to_wstring((long)code) + L")";
            return r;
        }

    }

    const wchar_t* const* WrittenValueNames() { return kWritten; }

    bool IsInside(const std::wstring& path, const std::wstring& dir) {
        std::wstring p, d;
        if (!Normalize(path, p) || !Normalize(dir, d)) return false;
        if (d.size() <= 3) return false; // a drive root "contains" the whole drive
        if (p.size() < d.size()) return false;
        if (CompareStringOrdinal(p.c_str(), (int)d.size(), d.c_str(), (int)d.size(), TRUE) != CSTR_EQUAL)
            return false;
        return p.size() == d.size() || p[d.size()] == L'\\';
    }

    Result MigrateHandoffKey(const wchar_t* subKey, const std::wstring& installDir) {
        Result res;
        HKEY k = nullptr;
        LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ | KEY_WRITE, &k);
        if (rc == ERROR_FILE_NOT_FOUND) {
            res.outcome = Outcome::NoKey;
            res.detail = L"no such key";
            return res;
        }
        if (rc != ERROR_SUCCESS) return Fail(L"cannot open the key", rc);

        std::wstring ragePath;
        LONG readErr = ERROR_SUCCESS;
        int have = ReadString(k, L"rage_path", ragePath, &readErr);
        if (have < 0) {
            RegCloseKey(k);
            return Fail(L"cannot read rage_path", readErr);
        }
        // The one test of ownership: our old launcher always set rage_path to the
        // "rage" folder inside its own install. Anything else is somebody else's key.
        if (have == 0 || !IsInside(ragePath, installDir)) {
            RegCloseKey(k);
            res.outcome = Outcome::NotOurs;
            res.detail = have == 0 ? std::wstring(L"no rage_path value, left alone")
                                   : L"rage_path '" + ragePath + L"' is outside '" + installDir + L"', left alone";
            return res;
        }

        LONG firstErr = ERROR_SUCCESS;
        std::wstring firstErrName;
        for (const wchar_t* const* n = kWritten; *n; ++n) {
            LONG r = RegDeleteValueW(k, *n);
            if (r == ERROR_SUCCESS) res.deleted++;
            else if (r != ERROR_FILE_NOT_FOUND && firstErr == ERROR_SUCCESS) {
                firstErr = r;
                firstErrName = *n;
            }
        }

        DWORD subkeys = 0, values = 0;
        LONG qr = RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                                   nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(k);
        if (firstErr != ERROR_SUCCESS) {
            Result e = Fail(L"cannot delete value '" + firstErrName + L"'", firstErr);
            e.deleted = res.deleted;
            return e;
        }

        res.outcome = Outcome::Cleaned;
        res.detail = L"removed " + std::to_wstring(res.deleted) + L" value(s) written by older launchers";
        // A key that holds nothing now was created by us alone: remove it too.
        if (qr == ERROR_SUCCESS && subkeys == 0 && values == 0) {
            LONG dr = RegDeleteKeyW(HKEY_CURRENT_USER, subKey);
            res.keyRemoved = dr == ERROR_SUCCESS;
            res.detail += res.keyRemoved ? L", key now empty and removed"
                                         : L", key empty but could not be removed (error " + std::to_wstring((long)dr) + L")";
        } else {
            res.detail += L", key kept (other values remain)";
        }
        return res;
    }

    void RunOnce(const std::wstring& installDir) {
        if (reg::Has(kDoneFlag)) return;
        Result r = MigrateHandoffKey(kLegacyHandoffKey, installDir);
        const std::string detail = Narrow(r.detail);
        switch (r.outcome) {
            case Outcome::NoKey:
                GLOGI("legacy handoff: HKCU\\%s: %s", Narrow(kLegacyHandoffKey).c_str(), detail.c_str());
                break;
            case Outcome::NotOurs:
                GLOGI("legacy handoff: HKCU\\%s is not from this install: %s",
                      Narrow(kLegacyHandoffKey).c_str(), detail.c_str());
                break;
            case Outcome::Cleaned:
                GLOGI("legacy handoff: HKCU\\%s cleaned: %s", Narrow(kLegacyHandoffKey).c_str(), detail.c_str());
                break;
            case Outcome::Error:
                GLOGW("legacy handoff: HKCU\\%s: %s; will retry on the next start",
                      Narrow(kLegacyHandoffKey).c_str(), detail.c_str());
                return; // not marked done
        }
        if (!reg::Set(kDoneFlag, L"1")) GLOGW("legacy handoff: could not record %s", Narrow(kDoneFlag).c_str());
    }

}
