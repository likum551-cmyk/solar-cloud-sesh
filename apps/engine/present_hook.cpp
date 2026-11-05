// present_hook.cpp: see present_hook.h for the why. Portable pieces only (windows.h, the logger): the inline
// fallback's machinery stays in hook.cpp and comes in through Callbacks, so this file links into a test on its own.
#include "present_hook.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "common/logger.h"

namespace gage::presenthook {

namespace {

using PresentFn = long (WINAPI*)(void* self, unsigned syncInterval, unsigned flags);

std::mutex          g_mu;                 // Install / Uninstall / Poll (never taken on the Present path)
std::atomic<bool>   g_installed{false};
void**              g_slot = nullptr;
void*               g_inlineTarget = nullptr;
Callbacks           g_cb;
Options             g_opt;
std::atomic<PresentFn> g_orig{nullptr};      // what PresentDetour calls (the slot's previous value)
std::atomic<PresentFn> g_pristine{nullptr};  // the slot's value at the first take: breaks a chain loop
std::atomic<uint64_t> g_ticks{0}, g_loops{0}, g_lastTick{0};
uint64_t            g_windowStart = 0;       // quiet time is measured from the later of this and the last tick
int                 g_retakes = 0;
bool                g_inlineOn = false;
bool                g_inlineTried = false;
uint64_t            g_lastStallLog = 0;
uint64_t            g_intactLoggedFor = 0;   // the lastTick whose "our slot went quiet" line was written
uint64_t            g_lastRetakeMs = 0;      // when Poll last took the slot back (0 = never)
void* volatile      g_sc = nullptr;          // IDXGISwapChain* of the latest Present
DWORD               g_tls = TLS_OUT_OF_INDEXES;  // per-thread "inside PresentDetour" flag; never freed (one DWORD)
HANDLE              g_thread = nullptr;
HANDLE              g_stop = nullptr;

long WINAPI PresentDetour(void* self, unsigned syncInterval, unsigned flags);

// The tick for this Present, unless this thread is already inside PresentDetour (the inline fallback's thunk runs
// inside the original, after PresentDetour has ticked).
bool enterFrame() {
    const DWORD le = GetLastError();
    const bool outer = TlsGetValue(g_tls) == nullptr;
    if (outer) TlsSetValue(g_tls, (LPVOID)1);
    SetLastError(le);
    return outer;
}
void leaveFrame() {
    const DWORD le = GetLastError();
    TlsSetValue(g_tls, nullptr);
    SetLastError(le);
}

void runTick() {
    g_lastTick.store(GetTickCount64());
    g_ticks.fetch_add(1);
    if (g_cb.tick) g_cb.tick();
}

long WINAPI PresentDetour(void* self, unsigned syncInterval, unsigned flags) {
    g_sc = self;
    const bool outer = enterFrame();
    PresentFn next;
    if (outer) {
        // After Uninstall a hooker that chained to us may still call this function: it ticks nothing and
        // forwards to the real Present.
        if (g_installed.load()) {
            runTick();
            next = g_orig.load();
        } else {
            next = g_pristine.load();
        }
    } else {
        // A hooker that chained to us sits between us and the original and we were re-entered: calling g_orig again
        // would recurse for ever. Go straight to the function the slot held when we first took it.
        g_loops.fetch_add(1);
        next = g_pristine.load();
    }
    const long hr = next(self, syncInterval, flags);
    if (outer) leaveFrame();
    return hr;
}

// Replace *slot, returning the previous value. The vtable lives in the image's read-only data: flip the page to
// writable for the store and put the protection back.
bool writeSlot(void** slot, void* value, void** prev) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        GLOGW("presenthook: VirtualProtect on the Present slot %p failed (%lu)", (void*)slot, GetLastError());
        return false;
    }
    void* was = InterlockedExchangePointer((PVOID volatile*)slot, value);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    if (prev) *prev = was;
    return true;
}

bool readBytes(const void* p, void* out, size_t n) {
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

HMODULE moduleOf(const void* p) {
    HMODULE m = nullptr;
    if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR)p, &m))
        return nullptr;
    return m;
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

DWORD WINAPI watchdogMain(LPVOID) {
    for (;;) {
        const DWORD ms = (DWORD)std::max<uint64_t>(g_opt.pollMs, 50);
        HANDLE stop = g_stop;
        if (!stop || WaitForSingleObject(stop, ms) != WAIT_TIMEOUT) return 0;
        Poll(GetTickCount64());
    }
}

} // namespace

Options& DefaultOptions() {
    static Options o;
    return o;
}

bool Install(void** slot, void* inlineTarget, const Callbacks& cb) { return Install(slot, inlineTarget, cb, DefaultOptions()); }

bool Install(void** slot, void* inlineTarget, const Callbacks& cb, const Options& opt) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_installed.load() || !slot || !cb.tick) return false;
    void* cur = *(void* volatile*)slot;
    if (!cur) { GLOGW("presenthook: the Present slot %p is empty; not hooked", (void*)slot); return false; }
    if (g_tls == TLS_OUT_OF_INDEXES) {
        g_tls = TlsAlloc();
        if (g_tls == TLS_OUT_OF_INDEXES) { GLOGW("presenthook: TlsAlloc failed (%lu)", GetLastError()); return false; }
    }
    g_cb = cb;
    g_opt = opt;
    g_slot = slot;
    g_inlineTarget = inlineTarget;
    g_orig.store((PresentFn)cur);
    g_pristine.store((PresentFn)cur);
    g_ticks = 0; g_loops = 0; g_lastTick = 0; g_retakes = 0;
    g_inlineOn = false; g_inlineTried = false; g_lastStallLog = 0; g_intactLoggedFor = 0; g_lastRetakeMs = 0;
    g_windowStart = GetTickCount64();

    const EntryInfo entry = DescribeEntry(cur);
    const std::string overlays = LoadedOverlayModules();
    void* prev = nullptr;
    if (!writeSlot(slot, (void*)&PresentDetour, &prev)) { g_slot = nullptr; return false; }
    if (prev != cur) {
        // The slot changed between the read and the store (another hooker, same instant): chain to what it held.
        g_orig.store((PresentFn)prev);
        g_pristine.store((PresentFn)prev);
        GLOGW("presenthook: the Present slot changed while taking it (%p -> %p); chaining to the newer value", cur, prev);
        cur = prev;
    }
    g_installed.store(true);
    GLOGI("hook: Present vtable slot %p taken; it held %s%s", (void*)slot, entry.text.c_str(),
          entry.foreign ? "  [that entry is inline-hooked by another module; we chain through it, untouched]" : "");
    if (!overlays.empty())
        GLOGI("hook: overlay/capture modules loaded: %s", overlays.c_str());

    if (opt.watchdog) {
        g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_stop) g_thread = CreateThread(nullptr, 0, watchdogMain, nullptr, 0, nullptr);
        if (!g_thread) GLOGW("presenthook: watchdog thread not started (%lu); no re-take or fallback", GetLastError());
    }
    return true;
}

void Uninstall() {
    HANDLE th = nullptr, ev = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_installed.load()) return;
        g_installed.store(false);       // Poll bails out from here on
        th = g_thread; ev = g_stop;
        g_thread = nullptr; g_stop = nullptr;
    }
    if (ev) SetEvent(ev);
    if (th) {
        // Short wait: Unhook can run under the loader lock, where a thread that is still exiting cannot finish.
        WaitForSingleObject(th, 1000);
        CloseHandle(th);
    }
    if (ev) CloseHandle(ev);

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inlineOn && g_cb.removeInline) g_cb.removeInline();
    g_inlineOn = false;
    if (g_slot) {
        void* now = *(void* volatile*)g_slot;
        if (now == (void*)&PresentDetour) {
            writeSlot(g_slot, (void*)g_orig.load(), nullptr);
        } else {
            GLOGW("presenthook: the Present slot holds %p, not our detour; leaving it (a hooker chained to us)", now);
        }
    }
    g_slot = nullptr;
}

bool Active() { return g_installed.load(); }
bool InlineActive() { return g_installed.load() && g_inlineOn; }
void* SwapChain() { return g_sc; }
void** SwapChainStorage() { return (void**)&g_sc; }

void OnInlineEntry() {
    if (g_tls == TLS_OUT_OF_INDEXES) { runTick(); return; }   // not installed through us: nothing to dedupe against
    const DWORD le = GetLastError();
    const bool inside = TlsGetValue(g_tls) != nullptr;
    SetLastError(le);
    if (!inside && g_installed.load()) runTick();
}

Stats GetStats() {
    Stats s;
    s.ticks = g_ticks.load();
    s.chainLoops = g_loops.load();
    s.lastTickMs = g_lastTick.load();
    s.inlineInstalled = InlineActive();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s.retakes = (uint64_t)g_retakes;
        s.slotIsOurs = g_installed.load() && g_slot && *(void* volatile*)g_slot == (void*)&PresentDetour;
    }
    return s;
}

PollResult Poll(uint64_t nowMs) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_installed.load() || !g_slot) return PollResult::NotInstalled;

    const uint64_t last = g_lastTick.load();
    const uint64_t ref = std::max<uint64_t>(last, g_windowStart);
    const uint64_t limit = last ? g_opt.stalledMs : g_opt.neverTickedMs;
    if (nowMs < ref + limit) return PollResult::Healthy;

    void* cur = *(void* volatile*)g_slot;
    const bool ours = cur == (void*)&PresentDetour;
    // Our slot carried frames, then they stopped: the game stopped presenting (a GPU hang, a load, minimised) far
    // more often than its swap chain got re-wrapped. Wait intactStalledMs before patching code; one line per quiet period.
    // Frames before a re-take do not count: they came through the slot before someone replaced it, not through the slot as it is now.
    if (ours && last != 0 && last >= g_lastRetakeMs && !g_inlineOn && !g_inlineTried && nowMs < ref + g_opt.intactStalledMs) {
        if (g_intactLoggedFor != last) {
            g_intactLoggedFor = last;
            GLOGW("presenthook: no Present for %llu ms after %llu frames through OUR slot: the game stopped presenting "
                  "(a GPU hang, a load, minimised); the inline fallback waits until %llu ms of quiet",
                  (unsigned long long)(nowMs - ref), (unsigned long long)g_ticks.load(),
                  (unsigned long long)g_opt.intactStalledMs);
        }
        return PollResult::Stalled;
    }
    const EntryInfo curInfo = DescribeEntry(cur);
    GLOGW("presenthook: no Present reached us for %llu ms (%llu ticks so far); slot holds %s %s",
          (unsigned long long)(nowMs - ref), (unsigned long long)g_ticks.load(),
          ours ? "OUR detour" : "a DIFFERENT pointer:", ours ? "" : curInfo.text.c_str());

    if (!ours && g_retakes < g_opt.maxRetakes) {
        ++g_retakes;
        g_orig.store((PresentFn)cur);       // chain to whoever replaced us
        void* prev = nullptr;
        if (writeSlot(g_slot, (void*)&PresentDetour, &prev)) {
            g_windowStart = nowMs;
            g_lastRetakeMs = nowMs;
            g_intactLoggedFor = 0;
            GLOGW("presenthook: took the Present slot back (%d of %d), chaining to %s; overlays: %s", g_retakes,
                  g_opt.maxRetakes, curInfo.text.c_str(), LoadedOverlayModules().c_str());
            return PollResult::Retaken;
        }
    }

    if (!g_inlineOn && !g_inlineTried && g_cb.installInline && g_inlineTarget) {
        g_inlineTried = true;
        GLOGW("presenthook: falling back to the inline hook on Present at %p (the game's swap chain does not "
              "reach the vtable slot, or the slot is contested); overlays: %s",
              g_inlineTarget, LoadedOverlayModules().c_str());
        g_inlineOn = g_cb.installInline((uint8_t*)g_inlineTarget);
        g_windowStart = nowMs;
        return g_inlineOn ? PollResult::InlineInstalled : PollResult::Stalled;
    }

    if (nowMs - g_lastStallLog >= 30000 || g_lastStallLog == 0) {
        g_lastStallLog = nowMs;
        GLOGW("presenthook: still no frame; nothing left to try (retakes %d, inline fallback %s)", g_retakes,
              g_inlineOn ? "installed" : (g_inlineTried ? "failed" : "unavailable"));
    }
    g_windowStart = nowMs;                  // re-arm: one verdict per quiet period, not one per poll
    return PollResult::Stalled;
}

// ---- diagnostics -------------------------------------------------------------------------------------------------
std::string DescribeAddress(const void* p) {
    char buf[320];
    HMODULE m = moduleOf(p);
    if (m) {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* base = path;
        if (n) for (const wchar_t* c = path; *c; ++c) if (*c == L'\\' || *c == L'/') base = c + 1;
        std::snprintf(buf, sizeof(buf), "%s+0x%llx", n ? narrow(base).c_str() : "?module?",
                      (unsigned long long)((const uint8_t*)p - (const uint8_t*)m));
    } else {
        std::snprintf(buf, sizeof(buf), "unbacked memory %p", p);
    }
    return buf;
}

EntryInfo DescribeEntry(const void* fn) {
    EntryInfo info;
    info.text = DescribeAddress(fn);
    const HMODULE home = moduleOf(fn);
    const uint8_t* p = (const uint8_t*)fn;
    for (int hop = 0; hop < 3 && p; ++hop) {
        uint8_t b[16] = {};
        if (!readBytes(p, b, sizeof(b))) break;
        const uint8_t* next = nullptr;
        if (b[0] == 0xE9) {
            int32_t d; std::memcpy(&d, b + 1, 4);
            next = p + 5 + d;
        } else if (b[0] == 0xEB) {
            next = p + 2 + (int8_t)b[1];
        } else if (b[0] == 0xFF && b[1] == 0x25) {          // jmp qword ptr [rip+disp32]
            int32_t d; std::memcpy(&d, b + 2, 4);
            uint64_t tgt = 0;
            if (!readBytes(p + 6 + d, &tgt, 8)) break;
            next = (const uint8_t*)(uintptr_t)tgt;
        } else if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) {   // mov rax, imm64; jmp rax
            uint64_t tgt; std::memcpy(&tgt, b + 2, 8);
            next = (const uint8_t*)(uintptr_t)tgt;
        } else if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) {  // mov r11, imm64; jmp r11
            uint64_t tgt; std::memcpy(&tgt, b + 2, 8);
            next = (const uint8_t*)(uintptr_t)tgt;
        } else {
            break;
        }
        info.text += " -> jmp -> " + DescribeAddress(next);
        if (moduleOf(next) != home) info.foreign = true;
        p = next;
    }
    return info;
}

std::string LoadedOverlayModules() {
    static const wchar_t* const kNames[] = {
        L"RTSSHooks64.dll",            // RivaTuner Statistics Server (MSI Afterburner's overlay)
        L"GameOverlayRenderer64.dll",  // Steam overlay
        L"DiscordHook64.dll",          // Discord overlay
        L"nvspcap64.dll",              // NVIDIA ShadowPlay / GeForce Experience overlay
        L"graphics-hook64.dll",        // OBS game capture
        L"EOSOVH-Win64-Shipping.dll",  // Epic Online Services overlay
        L"ReShade64.dll", L"SpecialK64.dll", L"fraps64.dll",
    };
    std::string out;
    for (const wchar_t* n : kNames) {
        if (GetModuleHandleW(n)) { if (!out.empty()) out += ", "; out += narrow(n); }
    }
    // dxgi/d3d11 loaded from outside the system folder: a proxy DLL next to the game (ReShade, SpecialK, ENB, ...).
    wchar_t sys[MAX_PATH] = {}, sysWow[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    GetSystemWow64DirectoryW(sysWow, MAX_PATH);
    for (const wchar_t* n : {L"dxgi.dll", L"d3d11.dll"}) {
        HMODULE m = GetModuleHandleW(n);
        wchar_t path[MAX_PATH] = {};
        if (!m || !GetModuleFileNameW(m, path, MAX_PATH)) continue;
        const bool system = (*sys && _wcsnicmp(path, sys, wcslen(sys)) == 0) ||
                            (*sysWow && _wcsnicmp(path, sysWow, wcslen(sysWow)) == 0);
        if (!system) { if (!out.empty()) out += ", "; out += narrow(n) + " (proxy: " + narrow(path) + ")"; }
    }
    return out;
}

} // namespace gage::presenthook
