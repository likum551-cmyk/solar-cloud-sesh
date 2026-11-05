// present_hook_test: the per-frame Present hook (apps/engine/present_hook.{h,cpp} + World::HookTick in hook.cpp).
// Never shipped. Exit 0 = pass.
//
// Part 1 runs the module on a fake vtable in a read-only page: ordering, argument and return passthrough, the
// protection round trip, a third party chaining to us, a hooker restoring the slot (watchdog re-take), a chain loop,
// the inline-fallback decision and the once-per-Present dedupe, refusals, GetLastError, two threads.
// Part 2 runs the PRODUCTION path (World::HookTick, findPresent, the real inline hook) on a real dxgi swap chain
// (hardware, else WARP) next to a simulated overlay hooker. The simulator models what gage.log showed with RTSS on
// (2026-11-03: Present's first bytes were an E9 to the overlay, our inline E9 ran for exactly one frame, then
// "no frame in the last 10 s (1 in total)"): it patches Present's code, and on every call restores the original bytes
// (which also removes anybody's patch on top), runs the real function and puts its patch back. It models that observed
// behaviour, not RTSS's internals. The old inline path must die under it (this proves the simulator is hostile; without
// that check the vtable assertions would pass vacuously) and the new path must tick on every Present.
//
// hook.cpp is included as a translation unit to reach installInlineHook / findPresent / g_presentSlot, the same way
// gage_mp_test includes gage_mp.cpp.

#include "../apps/engine/hook.cpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace pht {

namespace ph = gage::presenthook;
using PresentFn = long (WINAPI*)(void*, unsigned, unsigned);

int g_run = 0, g_failed = 0, g_skipped = 0;
void Check(const char* name, bool ok, const char* detail = "") {
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, *detail ? "  got: " : "", detail);
}
void Checkf(const char* name, bool ok, const char* fmt, long long a, long long b = 0) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    Check(name, ok, buf);
}
uint64_t Now() { return GetTickCount64(); }

// ---------------------------------------------------------------------------------------------------------------
// Part 1: the module on a fake vtable
// ---------------------------------------------------------------------------------------------------------------
std::vector<std::string> g_seq;
std::atomic<int> g_origCalls{0}, g_ticks{0}, g_thirdHits{0};
void* g_lastSelf = nullptr;
unsigned g_lastSync = 0, g_lastFlags = 0;
DWORD g_errAtOrig = 0;
bool g_origEntersInline = false;     // the fake original calls OnInlineEntry, like the inline thunk inside the real Present
bool g_recordSeq = true;

long WINAPI FakeOrig(void* self, unsigned s, unsigned f) {
    if (g_recordSeq) g_seq.push_back("orig");
    g_errAtOrig = GetLastError();
    ++g_origCalls;
    g_lastSelf = self; g_lastSync = s; g_lastFlags = f;
    if (g_origEntersInline) ph::OnInlineEntry();
    return 0x1234;
}
void FakeTick() {
    if (g_recordSeq) g_seq.push_back("tick");
    ++g_ticks;
}
std::atomic<int> g_inlineInstalls{0}, g_inlineRemoves{0};
uint8_t* g_inlineArg = nullptr;
bool FakeInstallInline(uint8_t* fn) { ++g_inlineInstalls; g_inlineArg = fn; return true; }
void FakeRemoveInline() { ++g_inlineRemoves; }

// A third party that hooked the slot AFTER us and chains to what it found there.
PresentFn g_thirdOrig = nullptr;
long WINAPI ThirdDetour(void* self, unsigned s, unsigned f) {
    ++g_thirdHits;
    return g_thirdOrig(self, s, f);
}

void ResetCounters() {
    g_seq.clear();
    g_origCalls = 0; g_ticks = 0; g_thirdHits = 0; g_inlineInstalls = 0; g_inlineRemoves = 0;
    g_inlineArg = nullptr; g_lastSelf = nullptr; g_lastSync = g_lastFlags = 0; g_errAtOrig = 0;
    g_origEntersInline = false; g_recordSeq = true;
}

DWORD ProtOf(void* p) { MEMORY_BASIC_INFORMATION m = {}; VirtualQuery(p, &m, sizeof(m)); return m.Protect; }

// A vtable-shaped page of read-only pointers, slot 8 = `original` (dxgi's vtable is read-only image data).
void** MakeRoVtable(void* original) {
    void** page = (void**)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < 16; ++i) page[i] = (void*)(uintptr_t)(0x1000 + i);
    page[8] = original;
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_READONLY, &old);
    return page;
}
// What another hooker does to the slot.
void ForceSlot(void** slot, void* value) {
    DWORD old = 0;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *(void* volatile*)slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
}
long GameCall(void** slot, void* self, unsigned s, unsigned f) {   // the game's `swapChain->Present(s, f)`
    return ((PresentFn)*(void* volatile*)slot)(self, s, f);
}

ph::Options TestOptions() {
    ph::Options o;
    o.watchdog = false;
    o.neverTickedMs = 1000;
    o.stalledMs = 500;
    o.intactStalledMs = 3000;
    o.maxRetakes = 2;
    return o;
}
ph::Callbacks TestCallbacks(bool withInline) {
    ph::Callbacks cb;
    cb.tick = FakeTick;
    if (withInline) { cb.installInline = FakeInstallInline; cb.removeInline = FakeRemoveInline; }
    return cb;
}

void PartOne() {
    std::printf("-- part 1: the module on a fake vtable\n");

    // refusals
    {
        void** vt = MakeRoVtable((void*)&FakeOrig);
        Check("null slot refused", !ph::Install(nullptr, nullptr, TestCallbacks(false), TestOptions()));
        ph::Callbacks noTick;
        Check("missing tick refused", !ph::Install(&vt[8], nullptr, noTick, TestOptions()));
        void** empty = MakeRoVtable(nullptr);
        Check("empty slot refused", !ph::Install(&empty[8], nullptr, TestCallbacks(false), TestOptions()));
        Check("nothing active after refusals", !ph::Active());
        Check("Poll without an install", ph::Poll(Now()) == ph::PollResult::NotInstalled);
    }

    // install, call, ordering, passthrough, protection round trip, uninstall
    {
        ResetCounters();
        void** vt = MakeRoVtable((void*)&FakeOrig);
        void** slot = &vt[8];
        Check("install", ph::Install(slot, (void*)&FakeOrig, TestCallbacks(false), TestOptions()));
        Check("slot now holds a different pointer", *slot != (void*)&FakeOrig);
        Check("slot page is read-only again after the store", ProtOf(slot) == PAGE_READONLY);
        Check("other vtable entries untouched", vt[7] == (void*)(uintptr_t)0x1007 && vt[9] == (void*)(uintptr_t)0x1009);
        Check("second install refused", !ph::Install(slot, nullptr, TestCallbacks(false), TestOptions()));
        SetLastError(1234);
        const long hr = GameCall(slot, (void*)0xABCD, 1, 0x20);
        Checkf("return value passes through", hr == 0x1234, "0x%llx", hr);
        Checkf("tick ran once", g_ticks == 1, "%lld", g_ticks.load());
        Checkf("original ran once", g_origCalls == 1, "%lld", g_origCalls.load());
        Check("tick runs before the original", g_seq.size() == 2 && g_seq[0] == "tick" && g_seq[1] == "orig");
        Check("arguments pass through", g_lastSelf == (void*)0xABCD && g_lastSync == 1 && g_lastFlags == 0x20);
        Check("swap chain captured", ph::SwapChain() == (void*)0xABCD);
        Checkf("GetLastError survives the detour", g_errAtOrig == 1234, "%lld", (long long)g_errAtOrig);
        ph::Stats st = ph::GetStats();
        Check("stats: one tick, slot ours", st.ticks == 1 && st.slotIsOurs && st.lastTickMs != 0 && !st.inlineInstalled);
        Check("healthy right after a frame", ph::Poll(Now()) == ph::PollResult::Healthy);
        ph::Uninstall();
        Check("uninstall puts the original back", *slot == (void*)&FakeOrig);
        Check("slot page read-only after uninstall", ProtOf(slot) == PAGE_READONLY);
        Check("not active after uninstall", !ph::Active());
        GameCall(slot, (void*)1, 0, 0);
        Checkf("no tick after uninstall", g_ticks == 1, "%lld", g_ticks.load());
    }

    // a third party hooks after us and chains to us; then a quiet period: re-take; the chain loop must not recurse
    {
        ResetCounters();
        void** vt = MakeRoVtable((void*)&FakeOrig);
        void** slot = &vt[8];
        ph::Install(slot, nullptr, TestCallbacks(false), TestOptions());
        g_thirdOrig = (PresentFn)*slot;                 // our detour
        ForceSlot(slot, (void*)&ThirdDetour);
        GameCall(slot, (void*)1, 0, 0);
        Check("chained third party: tick once, third once, original once",
              g_ticks == 1 && g_thirdHits == 1 && g_origCalls == 1);
        Check("chained and healthy: no re-take while frames arrive", ph::Poll(Now()) == ph::PollResult::Healthy);
        // frames stop for longer than stalledMs: the slot is not ours, so Poll takes it back (it cannot know the
        // third party chains to us). Now the slot -> us -> third -> us again: the second entry must be recognised.
        Check("quiet + foreign slot: re-taken", ph::Poll(Now() + 600) == ph::PollResult::Retaken);
        Check("slot ours again", ph::GetStats().slotIsOurs && ph::GetStats().retakes == 1);
        ResetCounters();
        const long hr = GameCall(slot, (void*)1, 0, 0);     // would recurse for ever without the re-entry guard
        Checkf("chain loop terminates and returns the real result", hr == 0x1234, "0x%llx", hr);
        Check("chain loop: one tick, third once, original once", g_ticks == 1 && g_thirdHits == 1 && g_origCalls == 1);
        Check("chain loop counted", ph::GetStats().chainLoops == 1);
        ph::Uninstall();
        // after Uninstall a stale chain through our detour must tick nothing and still reach the real function
        ResetCounters();
        const long hr2 = ((PresentFn)&ThirdDetour)((void*)1, 0, 0);
        Check("stale chain after uninstall: no tick, original reached", g_ticks == 0 && g_origCalls >= 1 && hr2 == 0x1234);
    }

    // someone restores the slot to the original and does not chain: frames stop, the watchdog takes it back
    {
        ResetCounters();
        void** vt = MakeRoVtable((void*)&FakeOrig);
        void** slot = &vt[8];
        ph::Install(slot, nullptr, TestCallbacks(false), TestOptions());
        GameCall(slot, (void*)1, 0, 0);
        ForceSlot(slot, (void*)&FakeOrig);
        GameCall(slot, (void*)1, 0, 0);
        GameCall(slot, (void*)1, 0, 0);
        Checkf("restored slot: ticks stop (the failure being guarded)", g_ticks == 1, "%lld", g_ticks.load());
        Check("not yet quiet long enough: healthy", ph::Poll(Now() + 100) == ph::PollResult::Healthy);
        Check("quiet long enough: re-taken", ph::Poll(Now() + 600) == ph::PollResult::Retaken);
        GameCall(slot, (void*)1, 0, 0);
        Checkf("ticks resume after the re-take", g_ticks == 2, "%lld", g_ticks.load());
        ph::Uninstall();
        Check("uninstall restores", *slot == (void*)&FakeOrig);
    }

    // re-takes are bounded; then the inline fallback; it goes in once, and Uninstall removes it
    {
        ResetCounters();
        void** vt = MakeRoVtable((void*)&FakeOrig);
        void** slot = &vt[8];
        ph::Install(slot, (void*)0x5000, TestCallbacks(true), TestOptions());   // maxRetakes = 2
        GameCall(slot, (void*)1, 0, 0);
        uint64_t t = Now();
        ForceSlot(slot, (void*)&FakeOrig);
        t += 600;
        Check("retake 1", ph::Poll(t) == ph::PollResult::Retaken);
        ForceSlot(slot, (void*)&FakeOrig);
        t += 600;
        Check("retake 2", ph::Poll(t) == ph::PollResult::Retaken);
        ForceSlot(slot, (void*)&FakeOrig);
        t += 600;
        Check("retakes exhausted: inline fallback", ph::Poll(t) == ph::PollResult::InlineInstalled);
        Check("fallback got the inline target", g_inlineInstalls == 1 && g_inlineArg == (uint8_t*)0x5000);
        Check("fallback reported active", ph::InlineActive());
        t += 600;
        Check("nothing left to try: stalled, no second install", ph::Poll(t) == ph::PollResult::Stalled && g_inlineInstalls == 1);
        ph::Uninstall();
        Check("uninstall removed the inline hook", g_inlineRemoves == 1 && !ph::InlineActive());
    }

    // the game's chain never reaches the slot: no frame at all -> inline fallback, and the tick runs once per Present
    {
        ResetCounters();
        void** vt = MakeRoVtable((void*)&FakeOrig);
        void** slot = &vt[8];
        ph::Install(slot, (void*)0x6000, TestCallbacks(true), TestOptions());   // neverTickedMs = 1000
        FakeOrig((void*)1, 0, 0);                       // the game bypasses the vtable
        const uint64_t t0 = Now();
        Check("short of the never-ticked limit: healthy", ph::Poll(t0 + 500) == ph::PollResult::Healthy);
        Check("never ticked: inline fallback", ph::Poll(t0 + 1100) == ph::PollResult::InlineInstalled);
        Check("installed exactly once", g_inlineInstalls == 1 && g_inlineArg == (uint8_t*)0x6000);
        Check("second poll does not install again", ph::Poll(t0 + 2300) != ph::PollResult::InlineInstalled && g_inlineInstalls == 1);
        // the dedupe: the fake original enters OnInlineEntry like the inline thunk inside the real Present does
        g_origEntersInline = true;
        ResetCounters(); g_origEntersInline = true;
        GameCall(slot, (void*)1, 0, 0);
        Checkf("both hooks live, one Present through the vtable: ONE tick", g_ticks == 1, "%lld", g_ticks.load());
        FakeOrig((void*)1, 0, 0);                       // a Present that bypasses the vtable: only the inline entry sees it
        Checkf("a Present that bypasses the vtable: the inline entry ticks", g_ticks == 2, "%lld", g_ticks.load());
        ph::Uninstall();
        ResetCounters();
        ph::OnInlineEntry();
        Checkf("inline entry after uninstall ticks nothing", g_ticks == 0, "%lld", g_ticks.load());
    }

    // 0.4.30: frames came through OUR slot, then the game stopped presenting (a friend's Enhanced GPU hang before
    // ERR_GFX_STATE): no code patch for intactStalledMs (3000 here), then the inline fallback as before
    {
        ResetCounters();
        void** vt = MakeRoVtable((void*)&FakeOrig);
        void** slot = &vt[8];
        ph::Install(slot, (void*)0x7000, TestCallbacks(true), TestOptions());
        for (int i = 0; i < 5; ++i) GameCall(slot, (void*)1, 0, 0);
        const uint64_t t0 = Now();
        Check("intact slot, quiet past stalledMs: stalled, NO inline hook", ph::Poll(t0 + 600) == ph::PollResult::Stalled && g_inlineInstalls == 0);
        Check("intact slot, quiet 2.9 s: still no inline hook", ph::Poll(t0 + 2900) == ph::PollResult::Stalled && g_inlineInstalls == 0);
        Check("intact slot, quiet past intactStalledMs: inline fallback", ph::Poll(t0 + 3100) == ph::PollResult::InlineInstalled && g_inlineInstalls == 1);
        ph::Uninstall();
        // frames resume inside the window: healthy again, and the window starts over from the newest frame
        ResetCounters();
        vt = MakeRoVtable((void*)&FakeOrig);
        slot = &vt[8];
        ph::Install(slot, (void*)0x7000, TestCallbacks(true), TestOptions());
        GameCall(slot, (void*)1, 0, 0);
        Check("hang then recovery: stalled first", ph::Poll(Now() + 600) == ph::PollResult::Stalled);
        GameCall(slot, (void*)1, 0, 0);
        const uint64_t f2 = Now();
        Check("hang then recovery: healthy after a frame", ph::Poll(f2) == ph::PollResult::Healthy && g_inlineInstalls == 0);
        // the window restarts at the newest frame: quiet 0.6 s after it is not yet 3 s, so no inline hook; 3.1 s is
        Check("recovery: the 3 s window counts from the newest frame (0.6 s: stalled, no hook)",
              ph::Poll(f2 + 600) == ph::PollResult::Stalled && g_inlineInstalls == 0);
        Check("recovery: 3.1 s after the newest frame: inline fallback", ph::Poll(f2 + 3100) == ph::PollResult::InlineInstalled && g_inlineInstalls == 1);
        ph::Uninstall();

        // after a re-take the frames from before it do not count: silence then gets stalledMs (0.5 s), not the 3 s window
        ResetCounters();
        vt = MakeRoVtable((void*)&FakeOrig);
        slot = &vt[8];
        ph::Install(slot, (void*)0x7000, TestCallbacks(true), TestOptions());
        GameCall(slot, (void*)1, 0, 0);
        uint64_t t = Now();
        ForceSlot(slot, (void*)&FakeOrig);
        t += 600;
        Check("re-take after frames", ph::Poll(t) == ph::PollResult::Retaken);
        t += 600;
        Check("re-take then silence: stalledMs applies, the inline fallback goes in (no 3 s wait)",
              ph::Poll(t) == ph::PollResult::InlineInstalled && g_inlineInstalls == 1);
        ph::Uninstall();
    }

    // two threads presenting at once: the per-thread guard keeps every Present's tick
    {
        ResetCounters();
        g_recordSeq = false;
        void** vt = MakeRoVtable((void*)&FakeOrig);
        void** slot = &vt[8];
        ph::Install(slot, nullptr, TestCallbacks(false), TestOptions());
        auto worker = [&]() { for (int i = 0; i < 2000; ++i) GameCall(slot, (void*)1, 0, 0); };
        std::thread a(worker), b(worker);
        a.join(); b.join();
        Checkf("2 threads x 2000 Presents: every one ticked", g_ticks == 4000, "%lld", g_ticks.load());
        Checkf("... and reached the original", g_origCalls == 4000, "%lld", g_origCalls.load());
        Check("no chain loop counted", ph::GetStats().chainLoops == 0);
        ph::Uninstall();
        g_recordSeq = true;
    }

    // diagnostics
    {
        // an E9 / FF25 / mov-rax-jmp-rax stub in an executable page that lands inside kernel32
        uint8_t* page = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        uint8_t* target = (uint8_t*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep");
        Check("plain function: not foreign", !ph::DescribeEntry((const void*)&FakeOrig).foreign);
        Check("DescribeAddress names this exe", ph::DescribeAddress((const void*)&FakeOrig).find("present_hook_test.exe+0x") != std::string::npos,
              ph::DescribeAddress((const void*)&FakeOrig).c_str());
        // FF 25 rel32=0 + abs target
        page[0] = 0xFF; page[1] = 0x25; *(int32_t*)(page + 2) = 0; *(uint64_t*)(page + 6) = (uint64_t)(uintptr_t)target;
        // (GetProcAddress may resolve a kernel32 export to kernelbase: compare against whatever module owns the target)
        const std::string targetName = ph::DescribeAddress(target);
        ph::EntryInfo e = ph::DescribeEntry(page);
        Check("FF25 stub out of unbacked memory is foreign and names the target's module",
              e.foreign && e.text.find(targetName) != std::string::npos, e.text.c_str());
        Check("jump out of unbacked memory is described as such", e.text.find("unbacked memory") != std::string::npos, e.text.c_str());
        // E9 rel32 to a second stub, then mov rax,imm64 ; jmp rax to kernel32
        uint8_t* page2 = page + 256;
        page2[0] = 0x48; page2[1] = 0xB8; *(uint64_t*)(page2 + 2) = (uint64_t)(uintptr_t)target; page2[10] = 0xFF; page2[11] = 0xE0;
        page[512] = 0xE9; *(int32_t*)(page + 513) = (int32_t)((intptr_t)page2 - (intptr_t)(page + 512 + 5));
        e = ph::DescribeEntry(page + 512);
        Check("E9 -> mov rax/jmp rax chain is followed to the target's module",
              e.foreign && e.text.find(targetName) != std::string::npos && e.text.find(" -> jmp -> ") != std::string::npos, e.text.c_str());
        VirtualFree(page, 0, MEM_RELEASE);
        const std::string ov = ph::LoadedOverlayModules();
        std::printf("   overlay/capture modules in this test process: %s\n", ov.empty() ? "(none)" : ov.c_str());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Part 2: the production path on a real dxgi swap chain, next to a simulated overlay hooker
// ---------------------------------------------------------------------------------------------------------------
std::atomic<int> g_realTicks{0};
void RealTick() { ++g_realTicks; }

void WriteCode(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old = 0;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

struct Sim {                       // the overlay hooker
    uint8_t* fn = nullptr;
    uint8_t pristine[5] = {};
    uint8_t patch[5] = {};
    uint8_t* stub = nullptr;
    std::atomic<int> hits{0};
    bool on = false;
};
Sim g_sim;

long WINAPI SimDetour(void* self, unsigned s, unsigned f) {
    ++g_sim.hits;
    WriteCode(g_sim.fn, g_sim.pristine, 5);                 // restore the original bytes: whoever patched on top is gone
    const long hr = ((PresentFn)g_sim.fn)(self, s, f);      // the real function
    WriteCode(g_sim.fn, g_sim.patch, 5);                    // re-assert its own patch
    return hr;
}
bool SimInstall(uint8_t* fn) {
    g_sim.fn = fn;
    std::memcpy(g_sim.pristine, fn, 5);
    g_sim.stub = (uint8_t*)allocNear(32, fn);               // a near stub: FF 25 00000000 <abs64>
    if (!g_sim.stub) return false;
    g_sim.stub[0] = 0xFF; g_sim.stub[1] = 0x25; *(int32_t*)(g_sim.stub + 2) = 0;
    *(uint64_t*)(g_sim.stub + 6) = (uint64_t)(uintptr_t)&SimDetour;
    const intptr_t rel = (intptr_t)g_sim.stub - (intptr_t)(fn + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) return false;
    g_sim.patch[0] = 0xE9; *(int32_t*)(g_sim.patch + 1) = (int32_t)rel;
    g_sim.hits = 0;
    WriteCode(fn, g_sim.patch, 5);
    g_sim.on = true;
    return true;
}
void SimRemove() {
    if (!g_sim.on) return;
    WriteCode(g_sim.fn, g_sim.pristine, 5);
    VirtualFree(g_sim.stub, 0, MEM_RELEASE);
    g_sim.stub = nullptr;
    g_sim.fn = nullptr;
    g_sim.on = false;
}

struct Chain {                     // the "game's" swap chain
    HWND hwnd = nullptr;
    void* sc = nullptr; void* dev = nullptr; void* ctx = nullptr;
    void** vtable = nullptr;
    bool driverIsWarp = false;
};
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

bool MakeChain(Chain& c) {
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) return false;
    using Create = long(__stdcall*)(void*, int, HMODULE, UINT, const int*, UINT, UINT, const void*, void**, void**, int*, void**);
    auto create = (Create)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    if (!create) return false;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PresentHookTestWnd";
    RegisterClassExW(&wc);
    c.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!c.hwnd) return false;
    uint8_t scd[128] = {};
    *(uint32_t*)(scd + 0) = 100; *(uint32_t*)(scd + 4) = 100; *(uint32_t*)(scd + 8) = 60; *(uint32_t*)(scd + 12) = 1;
    *(uint32_t*)(scd + 16) = 28; *(uint32_t*)(scd + 28) = 1; *(uint32_t*)(scd + 36) = 0x20; *(uint32_t*)(scd + 40) = 1;
    *(uint64_t*)(scd + 48) = (uint64_t)(uintptr_t)c.hwnd; *(uint32_t*)(scd + 56) = 1;
    for (int driver : {1, 5}) {                              // hardware first, like the game; WARP where there is no GPU
        long hr = create(nullptr, driver, nullptr, 0, nullptr, 0, 7, scd, &c.sc, &c.dev, nullptr, &c.ctx);
        if (hr >= 0 && c.sc) { c.driverIsWarp = driver == 5; c.vtable = *(void***)c.sc; return true; }
    }
    return false;
}
void DropChain(Chain& c) {
    if (c.sc) ((void(__stdcall*)(void*))(*(void***)c.sc)[2])(c.sc);
    if (c.dev) ((void(__stdcall*)(void*))(*(void***)c.dev)[2])(c.dev);
    if (c.ctx) ((void(__stdcall*)(void*))(*(void***)c.ctx)[2])(c.ctx);
    if (c.hwnd) DestroyWindow(c.hwnd);
    c = Chain{};
}
long Present(Chain& c) { return ((PresentFn)c.vtable[8])(c.sc, 0, 0); }    // exactly what the game does
int PresentN(Chain& c, int n) {                                            // returns how many returned a success code
    int ok = 0;
    for (int i = 0; i < n; ++i) if (Present(c) >= 0) ++ok;
    return ok;
}

void PartTwo() {
    std::printf("-- part 2: the production path on a real dxgi swap chain next to a simulated overlay hooker\n");
    Chain c;
    if (!MakeChain(c)) {
        ++g_skipped;
        std::printf("SKIP  no D3D11 device (hardware or WARP) on this machine: part 2 not run\n");
        return;
    }
    std::printf("   swap chain %p, vtable %p, Present %p (%s)\n", c.sc, (void*)c.vtable, c.vtable[8], c.driverIsWarp ? "WARP" : "hardware");
    uint8_t* fn = (uint8_t*)c.vtable[8];
    uint8_t snapshot[16];
    std::memcpy(snapshot, fn, 16);
    auto bodyIntact = [&]() { return std::memcmp(fn, snapshot, 16) == 0; };
    // ReShade or the like loaded here would already have changed the first bytes; the scenarios assume a clean start
    const bool cleanStart = fn[0] != 0xE9 && !(fn[0] == 0xFF && fn[1] == 0x25);
    if (!cleanStart) std::printf("   NOTE: Present already starts with a jump (%02x %02x): another overlay is loaded in this process\n", fn[0], fn[1]);

    // the premise of the vtable hook: findPresent's dummy chain shares the game's chain vtable
    uint8_t* found = findPresent();
    Check("findPresent resolves this chain's Present", found == fn);
    Check("findPresent records this chain's vtable slot (shared class vtable)", g_presentSlot == &c.vtable[8]);
    g_presentAddr = found;

    ph::DefaultOptions().watchdog = false;
    ph::DefaultOptions().neverTickedMs = 1000;
    ph::DefaultOptions().stalledMs = 500;
    gage::game::World w;

    // baseline
    g_realTicks = 0;
    const int baseOk = PresentN(c, 3);
    Checkf("baseline: Present succeeds", baseOk == 3, "%lld of 3", baseOk);
    Check("baseline: nothing ticks, nothing patched", g_realTicks == 0 && bodyIntact());

    // the simulated overlay alone (RTSS up, we are not)
    if (!SimInstall(fn)) { Check("simulator installed", false); DropChain(c); return; }
    Checkf("overlay alone: Presents succeed", PresentN(c, 4) == 4, "%lld of 4", 4);
    Checkf("overlay alone: it saw every Present and re-asserted its patch", g_sim.hits == 4 && fn[0] == 0xE9, "%lld hits", g_sim.hits.load());

    // --- the OLD path (present_mode=0) under the overlay: must die after one frame, as in gage.log ---------------
    {
        g_realTicks = 0; g_sim.hits = 0;
        w.presentMode = 0;
        Check("inline-only: hook installed (copies the overlay's E9)", w.HookTick(RealTick));
        const int ok = PresentN(c, 10);
        Checkf("inline-only: Presents succeed", ok == 10, "%lld of 10", ok);
        Checkf("inline-only under the overlay: ticked ONCE then never again (the 2026-11-03 failure)", g_realTicks == 1,
               "%lld ticks of 10", g_realTicks.load());
        Check("inline-only: the overlay still saw every Present", g_sim.hits == 10);
        w.Unhook();
    }
    // --- the NEW default under the same overlay -------------------------------------------------------------------
    {
        g_realTicks = 0; g_sim.hits = 0;
        w.presentMode = -1;
        Check("vtable: hook installed", w.HookTick(RealTick));
        Check("vtable: Hooked() reports it", w.Hooked());
        Check("vtable: slot swapped, Present's code untouched (still the overlay's E9)", c.vtable[8] != (void*)fn && fn[0] == 0xE9);
        const int ok = PresentN(c, 50);
        Checkf("vtable: Presents succeed", ok == 50, "%lld of 50", ok);
        Checkf("vtable under the overlay: ticked on EVERY Present", g_realTicks == 50, "%lld ticks of 50", g_realTicks.load());
        Check("vtable: the overlay still saw every Present (we chain through it)", g_sim.hits == 50);
        Check("vtable: swap chain handle is the game's", gage::game::GetSwapChain() == c.sc);
        Check("vtable: watchdog sees a healthy hook", ph::Poll(Now()) == ph::PollResult::Healthy);
        w.Unhook();
        Check("unhook: slot restored", c.vtable[8] == (void*)fn);
        g_realTicks = 0;
        PresentN(c, 3);
        Check("unhook: no more ticks, overlay still chained", g_realTicks == 0 && g_sim.hits >= 3);
        Check("Hooked() false after unhook", !w.Hooked());
    }
    // --- another party replaces the slot with the original and does not chain: watchdog re-take ---------------------
    {
        g_realTicks = 0;
        w.presentMode = -1;
        w.HookTick(RealTick);
        PresentN(c, 3);
        ForceSlot(&c.vtable[8], (void*)fn);
        PresentN(c, 3);
        Checkf("slot restored by someone else: ticks stop (the failure being guarded)", g_realTicks == 3, "%lld", g_realTicks.load());
        Check("watchdog re-takes after the quiet period", ph::Poll(Now() + 600) == ph::PollResult::Retaken);
        PresentN(c, 3);
        Checkf("ticks resume after the re-take", g_realTicks == 6, "%lld", g_realTicks.load());
        w.Unhook();
        Check("slot restored after unhook", c.vtable[8] == (void*)fn);
    }
    SimRemove();
    Check("overlay removed: Present's bytes are the original again", bodyIntact());

    // --- the inline fallback on the real function: the game's swap chain bypasses the vtable ---------------------
    {
        g_realTicks = 0;
        w.presentMode = -1;
        w.HookTick(RealTick);
        for (int i = 0; i < 5; ++i) ((PresentFn)fn)(c.sc, 0, 0);              // calls Present's code directly
        Checkf("bypass: no tick through the vtable hook", g_realTicks == 0, "%lld", g_realTicks.load());
        Check("bypass: the watchdog installs the inline fallback", ph::Poll(Now() + 1100) == ph::PollResult::InlineInstalled);
        Check("bypass: the real inline hook is live", g_hook.active && ph::InlineActive());
        for (int i = 0; i < 5; ++i) ((PresentFn)fn)(c.sc, 0, 0);
        Checkf("bypass: the inline entry ticks each Present", g_realTicks == 5, "%lld", g_realTicks.load());
        const int before = g_realTicks;
        PresentN(c, 5);                                                        // through the vtable: both hooks, ONE tick each
        Checkf("both hooks live: ONE tick per Present through the vtable", g_realTicks - before == 5, "%lld of 5", g_realTicks - before);
        Check("swap chain handle is the game's", gage::game::GetSwapChain() == c.sc);
        w.Unhook();
        Check("unhook: inline fallback removed, slot restored", !g_hook.active && c.vtable[8] == (void*)fn);
        Check("Present's bytes are the original again", bodyIntact());
    }
    {
        w.presentMode = 1;
        const bool ok = w.HookTick(RealTick);
        Check("present_mode=1 (vtable only) installs", ok && ph::Active());
        Check("present_mode=1 has no inline hook", !g_hook.active);
        w.Unhook();
        Check("present_mode=1 unhook restores the slot", c.vtable[8] == (void*)fn);
    }
    DropChain(c);
}

} // namespace pht

int main() {
    pht::PartOne();
    pht::PartTwo();
    std::printf("\n%d/%d passed%s\n", pht::g_run - pht::g_failed, pht::g_run, pht::g_skipped ? "  (part 2 SKIPPED: no D3D11)" : "");
    return pht::g_failed == 0 ? 0 : 1;
}
