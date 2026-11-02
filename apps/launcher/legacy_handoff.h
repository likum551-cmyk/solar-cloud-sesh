#pragma once
// One-time clean-up of the join-handoff values older GAGE-MP launchers wrote
// into HKCU\Software\RAGE-MP for the RAGE-MP engine they used to load. GAGE-MP
// no longer touches that key. The values are removed only when the key
// demonstrably came from this install: a person who really uses RAGE-MP owns
// the key, and it is left exactly as it is.

#include "common/common.h"

namespace gage::legacy {

    // The key the old launcher wrote, relative to HKCU.
    inline constexpr const wchar_t* kLegacyHandoffKey = L"Software\\RAGE-MP";

    // GAGE-MP's own flag (HKCU\Software\GAGE-MP) that the check has run.
    inline constexpr const wchar_t* kDoneFlag = L"legacy_handoff_done";

    enum class Outcome
    {
        NoKey,    // the key does not exist: nothing to do
        NotOurs,  // the key exists but does not point into this install: left alone
        Cleaned,  // the values this launcher wrote were removed
        Error,    // a registry call failed; nothing is marked done, the next start retries
    };

    struct Result
    {
        Outcome      outcome = Outcome::NoKey;
        int          deleted = 0;        // values removed
        bool         keyRemoved = false; // the key held nothing else and was removed too
        std::wstring detail;             // one line for the log
    };

    // The values GAGE-MP wrote (and nothing else is ever deleted).
    // Returned as a null-terminated list.
    const wchar_t* const* WrittenValueNames();

    // `rage_path` of HKCU\<subKey> is inside `installDir` (absolute, with or
    // without a trailing backslash): delete WrittenValueNames() from it, and the
    // key too when that leaves it empty. Otherwise leave it alone. `subKey` is a
    // parameter so the test can run on a scratch key.
    Result MigrateHandoffKey(const wchar_t* subKey, const std::wstring& installDir);

    // The launcher's start-up call: runs MigrateHandoffKey(kLegacyHandoffKey,
    // installDir) once (kDoneFlag), and logs the outcome.
    void RunOnce(const std::wstring& installDir);

    // `path` equals `dir` or lies below it (case-insensitive, either slash,
    // trailing slashes ignored; "C:\GAGE-MP2" is not inside "C:\GAGE-MP").
    // False when either is empty or relative, or `dir` is a drive root.
    bool IsInside(const std::wstring& path, const std::wstring& dir);

}
