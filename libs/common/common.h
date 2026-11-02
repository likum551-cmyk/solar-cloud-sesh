#pragma once
// GAGE-MP common definitions.
// Windows native, C++, camelCase, Win32 threading.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace gage {

    // Directory of the module (exe or DLL) this code is linked into, with a
    // trailing backslash. gage_common is a static library, so inside an injected
    // DLL this is the DLL's folder, not the host process's. Returns L".\\" if the
    // module path cannot be determined.
    std::wstring ExeDir();

    // Convert between the two string types we juggle at the Win32 boundary.
    std::string  Narrow(const std::wstring& wide);
    std::wstring Widen(const std::string& narrow);

    // True when running with elevated (admin) integrity.
    bool IsAdmin();

    // True when `path` exists and is a regular file.
    bool FileExists(const std::wstring& path);

    // Find the game exe: the install folders the stores record in the registry
    // (Rockstar / Steam / Epic, Legacy and Enhanced), then the standard paths.
    // Empty string when none of them holds GTA5.exe / GTA5_Enhanced.exe.
    std::wstring FindGameExe();

    // Format an hr / DWORD error into a small human string (no trailing
    // whitespace). Code 0 yields an explicit "no error code" text rather than
    // the system's misleading "completed successfully".
    std::wstring FormatError(DWORD code);

}
