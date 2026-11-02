// webview2_loader.cpp — in-tree WebView2 loader for the mingw-w64 build.
//
// Implements the two loader entry points GAGE-MP uses, with the signatures
// WebView2.h declares:
//   CreateCoreWebView2EnvironmentWithOptions
//   GetAvailableCoreWebView2BrowserVersionString
//
// Why not WebView2LoaderStatic.lib: it is MSVC-built and expects the MSVC CRT
// (TLS-based thread-safe statics, /GS cookies, MSVC operator new variants).
// Linking it into a mingw static-CRT image needed hand-written CRT shims, and
// those crashed (the loader read _Init_thread_epoch through the TLS block,
// but the shim had put it in .bss). This file replaces the .lib and the shims.
//
// Resolution order:
//   1. WebView2Loader.dll next to SolarLauncher.exe, if someone ships it there —
//      Microsoft's supported loader, forwarded to unchanged.
//   2. Built-in: locate the runtime the same way the official loader does and
//      call its EmbeddedBrowserWebView.dll export directly (the approach used
//      by webview/webview, Wails go-webview2 and OpenWebView2Loader):
//        - browserExecutableFolder argument, else WEBVIEW2_BROWSER_EXECUTABLE_FOLDER
//          => fixed-version runtime in that folder;
//        - otherwise the Evergreen runtime registered under
//          SOFTWARE\Microsoft\EdgeUpdate\ClientState\{channel}\EBWebView
//          (HKLM then HKCU, 32-bit registry view), stable first, then
//          beta/dev/canary (reversed when WEBVIEW2_RELEASE_CHANNEL_PREFERENCE=1).
//      EmbeddedBrowserWebView.dll lives at <runtime>\EBWebView\<arch>\.
//
// The runtime module is never unloaded: environment creation completes
// asynchronously and the environment keeps using it for the process lifetime.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>

#include <cstring>
#include <cwchar>
#include <string>

#include <WebView2.h>

#include "common/common.h"
#include "common/logger.h"

using namespace gage;

namespace {

    typedef HRESULT(STDMETHODCALLTYPE* CreateInternalFn)(
        bool checkRunningInstance, int runtimeType, PCWSTR userDataFolder, IUnknown* options,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);
    typedef HRESULT(STDMETHODCALLTYPE* CreateEnvFn)(
        PCWSTR browserExecutableFolder, PCWSTR userDataFolder, ICoreWebView2EnvironmentOptions* options,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);
    typedef HRESULT(STDMETHODCALLTYPE* VersionFn)(PCWSTR browserExecutableFolder, LPWSTR* versionInfo);

    const int kRuntimeInstalled = 0;
    const int kRuntimeFixed = 1;

#if defined(__aarch64__) || defined(_M_ARM64)
    const wchar_t* kArchDir = L"arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const wchar_t* kArchDir = L"x64";
#else
    const wchar_t* kArchDir = L"x86";
#endif

    struct Channel { const wchar_t* guid; const wchar_t* suffix; };
    const Channel kChannels[] = {
        { L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", L"" },         // stable (Evergreen runtime)
        { L"{2CD8A007-E189-409D-A2C8-9AF4EF3C72AA}", L" beta" },
        { L"{0D50BFEC-CD6A-4F9A-964C-C7416E3ACB10}", L" dev" },
        { L"{65C35B14-6C1D-4122-AC46-7148CC9D6497}", L" canary" },
    };
    const int kChannelCount = (int)(sizeof(kChannels) / sizeof(kChannels[0]));

    struct Client {
        std::wstring dll;      // full path of EmbeddedBrowserWebView.dll
        std::wstring version;  // e.g. 155.0.4283.62 (+ channel suffix); empty for fixed folders
        int runtimeType = kRuntimeInstalled;
    };

    bool IsFile(const std::wstring& p) {
        DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring TrimSlashes(std::wstring s) {
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        return s;
    }

    std::wstring ClientDllIn(const std::wstring& runtimeDir) {
        return TrimSlashes(runtimeDir) + L"\\EBWebView\\" + kArchDir + L"\\EmbeddedBrowserWebView.dll";
    }

    std::wstring EnvVar(const wchar_t* name) {
        DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
        if (n == 0) return L"";
        std::wstring v(n, L'\0');
        DWORD got = GetEnvironmentVariableW(name, &v[0], n);
        if (got == 0 || got >= n) return L"";
        v.resize(got);
        return v;
    }

    // REG_SZ value under HKxx\<sub> in the 32-bit view (EdgeUpdate is a 32-bit
    // product; a 64-bit process must ask for WOW6432Node explicitly).
    bool RegString(HKEY root, const std::wstring& sub, const wchar_t* value, std::wstring& out) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS)
            return false;
        DWORD cb = 0;
        LONG rc = RegGetValueW(k, nullptr, value, RRF_RT_REG_SZ, nullptr, nullptr, &cb);
        bool ok = false;
        if (rc == ERROR_SUCCESS && cb >= sizeof(wchar_t)) {
            std::wstring buf(cb / sizeof(wchar_t) + 1, L'\0');
            cb = (DWORD)(buf.size() * sizeof(wchar_t));
            if (RegGetValueW(k, nullptr, value, RRF_RT_REG_SZ, nullptr, &buf[0], &cb) == ERROR_SUCCESS) {
                buf.resize(wcslen(buf.c_str()));
                out = buf;
                ok = !out.empty();
            }
        }
        RegCloseKey(k);
        return ok;
    }

    // "155.0.4283.62" -> 155; rejects "0.0.0.0" (EdgeUpdate's "uninstalled").
    bool UsableVersion(const std::wstring& v) {
        unsigned long major = wcstoul(v.c_str(), nullptr, 10);
        return major >= 86;
    }

    bool FindInChannel(const Channel& ch, Client& out) {
        const HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
        for (HKEY root : roots) {
            // Primary: ClientState\{guid}\EBWebView = <...>\Application\<version>
            std::wstring dir;
            if (RegString(root, std::wstring(L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\") + ch.guid,
                          L"EBWebView", dir)) {
                dir = TrimSlashes(dir);
                size_t cut = dir.find_last_of(L"\\/");
                std::wstring ver = cut == std::wstring::npos ? L"" : dir.substr(cut + 1);
                std::wstring dll = ClientDllIn(dir);
                if (UsableVersion(ver) && IsFile(dll)) {
                    out.dll = dll;
                    out.version = ver + ch.suffix;
                    out.runtimeType = kRuntimeInstalled;
                    return true;
                }
            }
            // Fallback: Clients\{guid} location + pv
            std::wstring loc, pv;
            std::wstring clients = std::wstring(L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\") + ch.guid;
            if (RegString(root, clients, L"location", loc) && RegString(root, clients, L"pv", pv) &&
                UsableVersion(pv)) {
                std::wstring dll = ClientDllIn(TrimSlashes(loc) + L"\\" + pv);
                if (IsFile(dll)) {
                    out.dll = dll;
                    out.version = pv + ch.suffix;
                    out.runtimeType = kRuntimeInstalled;
                    return true;
                }
            }
        }
        return false;
    }

    bool FindInstalled(Client& out) {
        bool reverse = EnvVar(L"WEBVIEW2_RELEASE_CHANNEL_PREFERENCE") == L"1";
        for (int i = 0; i < kChannelCount; ++i) {
            const Channel& ch = kChannels[reverse ? kChannelCount - 1 - i : i];
            if (FindInChannel(ch, out)) return true;
        }
        return false;
    }

    bool FindFixed(std::wstring dir, Client& out) {
        bool absolute = dir.size() >= 2 &&
                        (dir[1] == L':' || (dir[0] == L'\\' && dir[1] == L'\\'));
        if (!absolute) dir = ExeDir() + dir;  // relative => relative to the exe, like the official loader
        std::wstring dll = ClientDllIn(dir);
        if (!IsFile(dll)) return false;
        out.dll = dll;
        out.version.clear();
        out.runtimeType = kRuntimeFixed;
        return true;
    }

    HRESULT FindClient(PCWSTR browserExecutableFolder, Client& out) {
        std::wstring fixed = (browserExecutableFolder && *browserExecutableFolder)
                                 ? std::wstring(browserExecutableFolder)
                                 : EnvVar(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER");
        bool ok = fixed.empty() ? FindInstalled(out) : FindFixed(fixed, out);
        return ok ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    // Microsoft's WebView2Loader.dll shipped next to the exe (optional).
    HMODULE OfficialLoader() {
        static HMODULE mod = nullptr;
        static bool tried = false;
        if (!tried) {
            tried = true;
            std::wstring p = ExeDir() + L"WebView2Loader.dll";
            if (IsFile(p)) {
                mod = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (mod) GLOGI("webview2: using %s", Narrow(p).c_str());
                else GLOGW("webview2: %s present but failed to load (err=%lu); using built-in loader",
                           Narrow(p).c_str(), GetLastError());
            }
        }
        return mod;
    }

    template <typename Fn>
    Fn OfficialExport(const char* name) {
        HMODULE m = OfficialLoader();
        if (!m) return nullptr;
        return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    }

}  // namespace

STDAPI CreateCoreWebView2EnvironmentWithOptions(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder, ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler) {
    if (!environmentCreatedHandler) return E_POINTER;

    if (CreateEnvFn official = OfficialExport<CreateEnvFn>("CreateCoreWebView2EnvironmentWithOptions"))
        return official(browserExecutableFolder, userDataFolder, environmentOptions, environmentCreatedHandler);

    Client c;
    HRESULT hr = FindClient(browserExecutableFolder, c);
    if (FAILED(hr)) {
        GLOGE("webview2: no usable WebView2 runtime found (hr=0x%08lX)", (unsigned long)hr);
        return hr;
    }
    GLOGI("webview2: runtime %s (%s)", c.version.empty() ? "fixed" : Narrow(c.version).c_str(),
          Narrow(c.dll).c_str());

    HMODULE mod = LoadLibraryExW(c.dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        DWORD e = GetLastError();
        GLOGE("webview2: LoadLibrary failed err=%lu", e);
        return HRESULT_FROM_WIN32(e);
    }
    CreateInternalFn create = reinterpret_cast<CreateInternalFn>(
        reinterpret_cast<void*>(GetProcAddress(mod, "CreateWebViewEnvironmentWithOptionsInternal")));
    if (!create) {
        GLOGE("webview2: CreateWebViewEnvironmentWithOptionsInternal export missing");
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    return create(true, c.runtimeType, userDataFolder, environmentOptions, environmentCreatedHandler);
}

STDAPI GetAvailableCoreWebView2BrowserVersionString(PCWSTR browserExecutableFolder, LPWSTR* versionInfo) {
    if (!versionInfo) return E_POINTER;
    *versionInfo = nullptr;

    if (VersionFn official = OfficialExport<VersionFn>("GetAvailableCoreWebView2BrowserVersionString"))
        return official(browserExecutableFolder, versionInfo);

    Client c;
    HRESULT hr = FindClient(browserExecutableFolder, c);
    if (FAILED(hr)) return hr;
    std::wstring v = c.version.empty() ? std::wstring(L"fixed") : c.version;
    size_t bytes = (v.size() + 1) * sizeof(wchar_t);
    LPWSTR p = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!p) return E_OUTOFMEMORY;
    memcpy(p, v.c_str(), bytes);
    *versionInfo = p;
    return S_OK;
}
