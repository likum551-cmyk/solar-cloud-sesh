// gage_launch_shim.dll — the BattlEye-free launch helper.
//
// The GAGE-MP launcher starts PlayGTAV.exe (the universal Rockstar/retail
// bootstrapper that every store ships) and injects this DLL into it. PlayGTAV
// establishes the ROS / Social Club session the game needs — so the game's
// "launched by the Rockstar Games Launcher" guard is satisfied legitimately —
// and then goes to launch the BattlEye wrapper, GTA5_BE.exe. This shim
// IAT-hooks PlayGTAV's process-creation calls and, when it sees that spawn,
// redirects it to the real game exe (GTA5.exe) instead. BattlEye's wrapper is
// therefore never in the chain and never arms, while the game still boots in
// PlayGTAV's own ROS session.
//
// This mirrors how RAGE-MP boots GTA V for offline, Story-Mode-derived
// multiplayer (confirmed by reverse-engineering ragemp_v.exe): it rides the
// real launcher for ROS and runs a renamed, BattlEye-free game image. We never
// go online, so no anti-cheat is being defeated for cheating.
//
// The wrapper is matched by SHAPE (GTA*_BE.exe), so Legacy (GTA5_BE.exe) and
// Enhanced (GTA5_Enhanced_BE.exe, also spelled GTAV_Enhanced_BE.exe) are both
// caught — the shim never depends on a single hard-coded edition name.
//
// Config (HKCU\Software\GAGE-MP, written by the launcher before injection):
//   shim_redirect_from   OPTIONAL extra base name to intercept (default ""): the
//                        shim already catches every BattlEye wrapper by shape
//                        (GTA*_BE.exe); set this only to force an off-shape name.
//   shim_redirect_to     full path to launch instead  (the real game exe, the
//                        edition the user chose: GTA5.exe or GTA5_Enhanced.exe)
//   shim_log             "0" to silence the shim's own log
//
// Self-contained on purpose: no gage_common link, so the DLL injected into
// Rockstar's launcher stays tiny and pulls in nothing but kernel32/advapi32.
// IAT hooking (not an inline detour of a system DLL) keeps it safe: no RWX
// pages and no instruction-length decoding inside PlayGTAV.

#include <windows.h>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cwctype>

namespace gage_shim {

// ---------------------------------------------------------------------------
// Pure redirect logic (no Win32 side effects; unit-tested in launch_shim_test)
// ---------------------------------------------------------------------------

inline wchar_t LowerW(wchar_t c) { return (wchar_t)towlower(c); }

// Final path component of `path` (after the last '\' or '/').
inline std::wstring BaseName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

// Case-insensitive wide-string equality (ASCII-fold; exe names are ASCII).
inline bool IEqual(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (LowerW(a[i]) != LowerW(b[i])) return false;
    return true;
}

// Split a command line into its first token (the program) and the remainder.
// Honors a leading quoted path:
//   "\"C:\\a b\\x.exe\" -foo"  -> first = C:\a b\x.exe, rest = -foo
//   "C:\\x.exe -foo"            -> first = C:\x.exe,     rest = -foo
inline void SplitFirstToken(const wchar_t* cmdLine, std::wstring& first, std::wstring& rest) {
    first.clear();
    rest.clear();
    if (!cmdLine) return;
    const wchar_t* p = cmdLine;
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') first.push_back(*p++);
        if (*p == L'"') ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') first.push_back(*p++);
    }
    while (*p == L' ' || *p == L'\t') ++p;
    rest = p;
}

// True when `base` is BattlEye's GTA V launcher wrapper, matched by SHAPE —
// GTA*_BE.exe, case-insensitive. Catches GTA5_BE.exe (Legacy), GTA5_Enhanced_BE.exe
// and the GTAV_Enhanced_BE.exe spelling (Enhanced) alike, so the redirect does not
// depend on a single hard-coded edition name. Mirrors the launcher's IsBattlEyeImage
// (apps/launcher/game_launch.cpp). The real game exe (GTA5.exe / GTA5_Enhanced.exe)
// and PlayGTAV.exe do not end in _BE.exe, so they are never matched here.
inline bool IsBattlEyeWrapperName(const std::wstring& base) {
    const size_t n = base.size();
    if (n <= 10) return false; // shorter than the shortest wrapper, "GTA5_BE.exe"
    if (!(LowerW(base[0]) == L'g' && LowerW(base[1]) == L't' && LowerW(base[2]) == L'a')) return false;
    const wchar_t suffix[] = L"_be.exe"; // 7 chars
    for (size_t i = 0; i < 7; ++i)
        if (LowerW(base[n - 7 + i]) != suffix[i]) return false;
    return true;
}

// Decide whether a CreateProcess* call for (appName, cmdLine) should be
// redirected to `toPath` (the real game exe). It is redirected when the spawned
// image is a BattlEye wrapper by shape (IsBattlEyeWrapperName — every edition),
// or when `fromBase` is a non-empty explicit override that the base name equals.
// On a match: outApp = toPath, outCmd = "\"toPath\"" followed by the original
// arguments, and returns true. Otherwise returns false and leaves the outputs
// untouched.
//
// lpApplicationName wins when set; otherwise argv[0] from the command line names
// the exe. Only the final path component is matched, so it fires however PlayGTAV
// (or the Rockstar Games Launcher) spells the path.
inline bool ComputeRedirect(const wchar_t* appName, const wchar_t* cmdLine,
                            const std::wstring& fromBase, const std::wstring& toPath,
                            std::wstring& outApp, std::wstring& outCmd) {
    if (toPath.empty()) return false;

    std::wstring cmdFirst, cmdRest;
    SplitFirstToken(cmdLine, cmdFirst, cmdRest);

    const std::wstring exe = (appName && appName[0]) ? std::wstring(appName) : cmdFirst;
    if (exe.empty()) return false;
    const std::wstring base = BaseName(exe);
    if (!(IsBattlEyeWrapperName(base) || (!fromBase.empty() && IEqual(base, fromBase)))) return false;

    outApp = toPath;
    outCmd = L"\"" + toPath + L"\"";
    if (!cmdRest.empty()) {
        outCmd += L" ";
        outCmd += cmdRest;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shim runtime: module dir, logging, config, IAT hooks
// ---------------------------------------------------------------------------

static HMODULE     g_self = nullptr;   // this DLL (null in the unit test)
static std::wstring g_fromBase;        // base name to intercept
static std::wstring g_toPath;          // full path to launch instead
static bool        g_logOn = true;
static bool        g_configLoaded = false;

// Directory (trailing backslash) of a module: the DLL's own folder for the
// shim's log, or the host exe's folder (hModule == nullptr) for the game-path
// fallback.
inline std::wstring ModuleDir(HMODULE hModule) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(hModule, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring(L".\\") : p.substr(0, s + 1);
}

// Minimal append logger: one CreateFile/WriteFile/CloseHandle, no CRT globals
// and nothing that needs the loader lock, so it is safe from DllMain and inside
// another process. Writes <this DLL's folder>\gage_shim.log.
inline void ShimLog(const char* fmt, ...) {
    if (!g_logOn) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\r';
    line[n++] = '\n';
    std::wstring path = ModuleDir(g_self) + L"gage_shim.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, line, (DWORD)n, &wr, nullptr);
    CloseHandle(h);
}

inline std::wstring RegGet(const wchar_t* name, const std::wstring& def) {
    wchar_t buf[1024];
    DWORD bytes = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\GAGE-MP", name,
                     RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS)
        return def;
    return std::wstring(buf);
}

// Read config lazily on the first intercepted call (never from DllMain: reading
// the registry under the loader lock is best avoided). The fallback target is
// GTA5.exe next to the host exe (PlayGTAV lives in the game folder).
inline void LoadConfig() {
    if (g_configLoaded) return;
    g_configLoaded = true;
    // Default empty: the shim catches every BattlEye wrapper by shape (GTA*_BE.exe),
    // so no edition name need be configured. An explicit value only adds an off-shape name.
    g_fromBase = RegGet(L"shim_redirect_from", L"");
    g_toPath   = RegGet(L"shim_redirect_to",   ModuleDir(nullptr) + L"GTA5.exe");
    g_logOn    = RegGet(L"shim_log", L"1") != L"0";
}

// Saved originals (resolved by HookIAT, or by GetProcAddress as a fallback).
typedef BOOL (WINAPI *CreateProcessW_t)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                        BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *CreateProcessAsUserW_t)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                              BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *CreateProcessInternalW_t)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                                BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION, PHANDLE);

static CreateProcessW_t         g_realCPW  = nullptr;
static CreateProcessAsUserW_t   g_realCPAU = nullptr;
static CreateProcessInternalW_t g_realCPI  = nullptr;

static BOOL WINAPI Hook_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                       BOOL inh, DWORD fl, LPVOID env, LPCWSTR cwd,
                                       LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    LoadConfig();
    std::wstring a, c;
    ShimLog("CreateProcessW: app='%ls' cmd='%.220ls'", app ? app : L"", cmd ? cmd : L"");
    if (ComputeRedirect(app, cmd, g_fromBase, g_toPath, a, c)) {
        ShimLog("redirect CreateProcessW: '%ls' -> '%ls'", app ? app : (cmd ? cmd : L"(null)"), a.c_str());
        std::vector<wchar_t> buf(c.begin(), c.end());
        buf.push_back(L'\0');
        return g_realCPW(a.c_str(), buf.data(), pa, ta, inh, fl, env, cwd, si, pi);
    }
    return g_realCPW(app, cmd, pa, ta, inh, fl, env, cwd, si, pi);
}

static BOOL WINAPI Hook_CreateProcessAsUserW(HANDLE tok, LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
                                             LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD fl, LPVOID env, LPCWSTR cwd,
                                             LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    LoadConfig();
    std::wstring a, c;
    ShimLog("CreateProcessAsUserW: app='%ls' cmd='%.220ls'", app ? app : L"", cmd ? cmd : L"");
    if (ComputeRedirect(app, cmd, g_fromBase, g_toPath, a, c)) {
        ShimLog("redirect CreateProcessAsUserW: '%ls' -> '%ls'", app ? app : (cmd ? cmd : L"(null)"), a.c_str());
        std::vector<wchar_t> buf(c.begin(), c.end());
        buf.push_back(L'\0');
        return g_realCPAU(tok, a.c_str(), buf.data(), pa, ta, inh, fl, env, cwd, si, pi);
    }
    return g_realCPAU(tok, app, cmd, pa, ta, inh, fl, env, cwd, si, pi);
}

static BOOL WINAPI Hook_CreateProcessInternalW(HANDLE tok, LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
                                               LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD fl, LPVOID env, LPCWSTR cwd,
                                               LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi, PHANDLE rtok) {
    LoadConfig();
    std::wstring a, c;
    ShimLog("CreateProcessInternalW: app='%ls' cmd='%.220ls'", app ? app : L"", cmd ? cmd : L"");
    if (ComputeRedirect(app, cmd, g_fromBase, g_toPath, a, c)) {
        ShimLog("redirect CreateProcessInternalW: '%ls' -> '%ls'", app ? app : (cmd ? cmd : L"(null)"), a.c_str());
        std::vector<wchar_t> buf(c.begin(), c.end());
        buf.push_back(L'\0');
        return g_realCPI(tok, a.c_str(), buf.data(), pa, ta, inh, fl, env, cwd, si, pi, rtok);
    }
    return g_realCPI(tok, app, cmd, pa, ta, inh, fl, env, cwd, si, pi, rtok);
}

// Patch every IAT entry in `host` that imports `func` (from any module) to
// point at `newFn`; returns the first original pointer replaced, or nullptr if
// the import is absent. An ordinary IAT walk — no code is modified, only the
// import pointer, so it is safe to do from DllMain.
inline void* HookIAT(HMODULE host, const char* func, void* newFn) {
    BYTE* base = reinterpret_cast<BYTE*>(host);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;

    void* original = nullptr;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         imp->Name; ++imp) {
        DWORD namesRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + namesRva);
        auto iat   = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;  // imported by ordinal
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (lstrcmpA(reinterpret_cast<const char*>(byName->Name), func) != 0) continue;
            DWORD old = 0;
            if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                if (!original) original = reinterpret_cast<void*>(static_cast<ULONG_PTR>(iat->u1.Function));
                iat->u1.Function = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(newFn));
                DWORD tmp = 0;
                VirtualProtect(&iat->u1.Function, sizeof(void*), old, &tmp);
            }
        }
    }
    return original;
}

// Hook the process-creation imports present in `host`'s IAT. Returns how many
// were hooked. Any variant not imported by name still gets its real pointer
// resolved via GetProcAddress, so a thunk reached by an unexpected path can
// still forward correctly.
inline int InstallProcessCreationHooks(HMODULE host) {
    int n = 0;
    if (void* o = HookIAT(host, "CreateProcessW", reinterpret_cast<void*>(&Hook_CreateProcessW))) {
        g_realCPW = reinterpret_cast<CreateProcessW_t>(o);
        ++n;
    }
    if (void* o = HookIAT(host, "CreateProcessAsUserW", reinterpret_cast<void*>(&Hook_CreateProcessAsUserW))) {
        g_realCPAU = reinterpret_cast<CreateProcessAsUserW_t>(o);
        ++n;
    }
    if (void* o = HookIAT(host, "CreateProcessInternalW", reinterpret_cast<void*>(&Hook_CreateProcessInternalW))) {
        g_realCPI = reinterpret_cast<CreateProcessInternalW_t>(o);
        ++n;
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        if (!g_realCPW)  g_realCPW  = reinterpret_cast<CreateProcessW_t>(GetProcAddress(k32, "CreateProcessW"));
        if (!g_realCPAU) g_realCPAU = reinterpret_cast<CreateProcessAsUserW_t>(GetProcAddress(k32, "CreateProcessAsUserW"));
    }
    return n;
}

}  // namespace gage_shim

#ifndef GAGE_SHIM_TEST
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gage_shim::g_self = inst;
        DisableThreadLibraryCalls(inst);
        int n = gage_shim::InstallProcessCreationHooks(GetModuleHandleW(nullptr));
        gage_shim::ShimLog("gage_launch_shim attached to pid %lu; hooked %d process-creation API(s)",
                           (unsigned long)GetCurrentProcessId(), n);
    }
    return TRUE;
}
#endif
