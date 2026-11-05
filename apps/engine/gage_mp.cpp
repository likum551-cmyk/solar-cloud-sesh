// GAGE-MP core engine.
//
// The in-game brain: registers the game hook points it would patch, exposes
// the `mp.*` JS bridge surface (mp.invoke / mp.events) that the in-game UI in
// apps/ingame-ui/ consumes, and reads the server chosen by the launcher (HKCU\Software\
// GAGE-MP: server_host, server_port, player_name).

#include "common/common.h"
#include "common/registry.h"
#include "common/logger.h"
#include "join.h"
#include "hook.h"
#include "engine.h"
#include "pack_stream.h"
// #include "loading_screen.h"  // TODO: D3D11 in-game overlay (separate task)

#include <windows.h>
#include <winsock2.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>
#include <string>
#include <cstdint>

using namespace gage;

// Defined in the extern "C" block below; the join event sink calls it.
extern "C" void GageMP_Dispatch(const char* event, const char* payload);

namespace {

    struct HookPoint
    {
        const char* name;
        const char* module;
        const char* symbol;
        bool        applied;
    };

    std::vector<HookPoint> g_hooks;
    std::map<std::string, std::vector<void*>> g_bus;
    std::string g_lastResult = "ok";
    uint32_t    g_eventCounter = 0;

    // Seconds since the game process started (to tell, from the log, whether
    // a patch landed before the game reached what it patches).
    double ProcessAgeSeconds()
    {
        FILETIME created, exited, kernel, user, now;
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            return -1;
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER a, b;
        a.LowPart = created.dwLowDateTime; a.HighPart = created.dwHighDateTime;
        b.LowPart = now.dwLowDateTime;     b.HighPart = now.dwHighDateTime;
        return (double)(b.QuadPart - a.QuadPart) / 1e7;
    }

    // Skip the Rockstar logo movie: the game looks its file up by the string
    // "platform:/movies/rockstar_logos"; pointed at a file that does not
    // exist, the movie is skipped. Same-length overwrite of the exact string
    // only (no code is patched), so a build without it is left untouched.
    // Technique from grasmanek94/SkipIntro (MIT), also the fallback of
    // SkipIntroStoryMode. Only readable, committed pages of the game image
    // are scanned; each page's protection is restored after the write.
    // Returns how many copies were patched.
    int SkipLogoMovie(HMODULE game)
    {
        // The key is assembled at run time, so no copy of it sits in any
        // image (it would match itself when a module scans its own image).
        char kFind[32];
        memcpy(kFind, "platform:/movies/rockstar_logo#", 32);
        kFind[30] = 's';                                                  // 31 chars + NUL
        static const char kRepl[] = "./nonexistingfilenonexistingfil";   // same length
        static_assert(sizeof(kFind) == sizeof(kRepl), "same length");
        const size_t n = sizeof(kFind); // the NUL too: an exact, whole string

        auto* dos = (const IMAGE_DOS_HEADER*)game;
        auto* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)game + dos->e_lfanew);
        BYTE* base = (BYTE*)game;
        BYTE* end = base + nt->OptionalHeader.SizeOfImage;

        int found = 0, patched = 0;
        MEMORY_BASIC_INFORMATION mbi;
        for (BYTE* p = base; p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi);
             p = (BYTE*)mbi.BaseAddress + mbi.RegionSize)
        {
            const DWORD prot = mbi.Protect & 0xFF;
            const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD)
                && prot != PAGE_NOACCESS && prot != PAGE_EXECUTE;
            if (!readable)
                continue;
            BYTE* r = (BYTE*)mbi.BaseAddress;
            BYTE* rEnd = std::min(r + mbi.RegionSize, end);
            for (BYTE* s = r; s + n <= rEnd; ++s)
            {
                s = (BYTE*)memchr(s, kFind[0], (size_t)(rEnd - n + 1 - s));
                if (!s)
                    break;
                if (memcmp(s, kFind, n) != 0)
                    continue;
                ++found;
                DWORD old = 0, tmp = 0;
                const bool exec = prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE
                    || prot == PAGE_EXECUTE_WRITECOPY;
                if (!VirtualProtect(s, n, exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old))
                {
                    GLOGW("gage_mp: logo string at +0x%llx not writable (%lu)",
                        (unsigned long long)(s - base), GetLastError());
                    continue;
                }
                memcpy(s, kRepl, n);
                VirtualProtect(s, n, old, &tmp);
                ++patched;
            }
        }
        GLOGI("gage_mp: skip logo movie: %d found, %d patched, %.1f s after game start",
            found, patched, ProcessAgeSeconds());
        return patched;
    }

    void Run()
    {
        GLOGI("gage_mp: engine attached (pid %lu, %.1f s after game start)",
            (unsigned long)GetCurrentProcessId(), ProcessAgeSeconds());

        // GTA V calls WSACleanup during early init, which tears down the Winsock
        // subsystem and kills any socket created before it. Winsock is reference-
        // counted: each WSAStartup increments, each WSACleanup decrements, and the
        // subsystem only shuts down when the count reaches zero. Padding the count
        // with extra WSAStartup calls keeps our ENet socket alive through the
        // game's cleanup cycle. 16 is generous — the game typically does 1–3 cleanups.
        {
            WSADATA wsaData;
            int padded = 0;
            for (int i = 0; i < 16; ++i)
                if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0)
                    ++padded;
            GLOGI("gage_mp: Winsock ref-count padded +%d (survives game WSACleanup)", padded);
        }

        if (reg::Get(L"skip_intro") != L"0")
            SkipLogoMovie(GetModuleHandleW(nullptr));

        // Resolve the game world (the game module + every primitive) once. The
        // World outlives the engine and the tick hook (static storage).
        static game::World world;
        // hooks=0: inject and connect but patch nothing in the game (with
        // skip_intro=0 as well), to tell our patches apart from the injection itself.
        // hooks=2: probe Present (dummy device) but do not detour it.
        const std::wstring hooks = reg::Get(L"hooks");
        world.passive = hooks == L"0";
        world.noDetour = hooks == L"2";
        // present_mode (engine-only, like pack_*): unset = Present vtable slot with the inline fallback,
        // 0 = the old inline hook only, 1 = vtable only. Support switch for a machine where the default misbehaves.
        const std::wstring presentMode = reg::Get(L"present_mode");
        world.presentMode = presentMode == L"0" ? 0 : presentMode == L"1" ? 1 : -1;
        if (world.presentMode != -1)
            GLOGW("gage_mp: present_mode=%d (%s)", world.presentMode,
                  world.presentMode == 0 ? "inline Present hook only" : "Present vtable hook only, no fallback");
        if (world.passive)
            GLOGW("gage_mp: PASSIVE MODE (hooks=0): no Present detour, no per-frame driver");
        else if (world.noDetour)
            GLOGW("gage_mp: PROBE-ONLY MODE (hooks=2): Present resolved, not detoured");
        const std::wstring spawn = reg::Get(L"spawn");
        world.spawnMode = spawn == L"0" ? 0 : spawn == L"1" ? 1 : -1;
        GLOGI("gage_mp: spawning %s", world.spawnMode == 0 ? "OFF (spawn=0)"
                                      : world.spawnMode == 1 ? "ON everywhere (spawn=1)"
                                                             : "on inside GTA's script tick (spawn unset)");
        // Boot straight to Story Mode (opt-in, off by default): GAGE is built on
        // Story Mode, but GTA Legacy stops at the landing page and never spawns a
        // ped, so the loader watchdog gives up. skip_landing=1 patches the
        // landing-page check (see tryPatchLandingPage in hook.cpp). A code patch
        // not yet verified live on build 3889, so it stays opt-in until a real
        // launch confirms it.
        world.skipLanding = reg::Get(L"skip_landing") == L"1";
        GLOGI("gage_mp: landing-page skip %s", world.skipLanding
                  ? "ON (skip_landing=1): boot straight to Story Mode"
                  : "off (set skip_landing=1 to boot straight to Story Mode)");
        // Enhanced dump: on an unported/Arxan-encrypted build (e.g. Enhanced) the
        // engine can't resolve its signatures; enh_dump writes the DECRYPTED game
        // image to %LOCALAPPDATA%\GAGE-MP\dump for offline signature derivation.
        // -1 unset = automatic (unported builds only), 0 = never, 1 = always.
        {
            const std::wstring d = reg::Get(L"enh_dump");
            world.enhDump = (d == L"1") ? 1 : (d == L"0") ? 0 : -1;
        }
        // Network-session recon (opt-in, off by default): with net_session=1 a
        // background thread scans the decrypted image for the FiveM/YimMenu
        // session-entry signatures and logs which resolve uniquely on this build,
        // before any session hook is wired. Recon only -- no hooks, no game call
        // (see reconNetSession / netSessionReconThread in hook.cpp).
        world.netSession = (reg::Get(L"net_session") == L"1") ? 1 : 0;
        GLOGI("gage_mp: network-session recon %s", world.netSession
                  ? "ON (net_session=1): logging session signatures, no hooks yet"
                  : "off");
        // Network-session HOST port (opt-in, off by default): with net_host=1 the
        // FiveM-style local session-entry stubs are installed once the network code
        // decrypts (reversible; see netHostInstall / netUnpatchAll in hook.cpp). The
        // host-call bring-up is live-gated -- this leaves everything restorable.
        world.netHost = (reg::Get(L"net_host") == L"1") ? 1 : 0;
        GLOGI("gage_mp: network-session host %s", world.netHost
                  ? "ON (net_host=1): ROS guard first; dry run (no hostGame) until G1"
                  : "off");
        // Network baseline (G0, off unless net_probe=1): read-only. Logs each distinct hostname /
        // destination the game resolves or connects to (import-table hooks, restored on Unhook) and
        // samples NETWORK_IS_* after the entry (see net_guard.h, netProbeTick in hook.cpp). Opt-in:
        // whether Arxan tolerates the IAT pointers is unproven, so ordinary play never carries them.
        world.netProbe = (reg::Get(L"net_probe") == L"1") ? 1 : 0;
        GLOGI("gage_mp: network baseline probe %s", world.netProbe ? "ON (net_probe=1, read-only)"
                                                                   : "off (net_probe=0)");
        // Story-character hijack guard (on unless kill_sp_hijack=0): after the entry, terminate
        // player_controller / player_timetable_scene every tick so main cannot swap the player
        // back to Michael/Franklin/Trevor.
        world.killSpHijack = (reg::Get(L"kill_sp_hijack") == L"0") ? 0 : 1;
        GLOGI("gage_mp: story-character hijack guard %s", world.killSpHijack ? "ON" : "off (kill_sp_hijack=0)");
        // FiveM JustNoScript (on unless no_sp_scripts=0): no stock script thread executes, so the
        // singleplayer tree (startup -> main -> player_controller) never runs.
        world.noSpScripts = (reg::Get(L"no_sp_scripts") == L"0") ? 0 : 1;
        GLOGI("gage_mp: stock scripts %s", world.noSpScripts ? "OFF (no_sp_scripts=1, FiveM JustNoScript)"
                                                             : "on (no_sp_scripts=0)");
        world.noSpKnownRva = (reg::Get(L"nosp_known_rva") == L"0") ? 0 : 1;
        GLOGI("gage_mp: nosp_known_rva=%d (0.4.21: an ambiguous GtaThread::Tick signature %s)", world.noSpKnownRva,
              world.noSpKnownRva ? "uses the hit at the RVA proven live for this build" : "is refused, as before");
        // Before game::Init (which arms skip_landing): the DLC vehicle array is sized when the Story session starts.
        // Undecided (gameconfig not parsed or the code still packed): retry off-thread every 100 ms for up to 2 min;
        // pack_stream's Tick ends the retry once the player is in the world.
        if (!game::content::PrepareVehicleStore()) {
            HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                for (int i = 0; i < 1200; ++i) {
                    Sleep(100);
                    if (game::content::PrepareVehicleStore()) return 0;
                }
                GLOGW("gage_mp: DLC vehicle store still undecided after 2 min; giving up the raise");
                return 0;
            }, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            else GLOGW("gage_mp: could not start the DLC vehicle store retry (error %lu)", GetLastError());
        }
        game::Init(world);
        GLOGI("gage_mp: game world ready=%d image@0x%llx size=%u resolved=0x%x %s",
            (int)world.ready, (unsigned long long)(uintptr_t)world.module,
            world.imageSize, world.Resolved(),
            world.err.empty() ? "" : ("(unresolved: " + world.err + ")").c_str());

        // Reflect the real primitives in the mp.* bridge's hook list.
        g_hooks.clear();
        struct PDef { const char* name; uint32_t bit; };
        const PDef pdefs[] = {
            {"MainLoopDetour",    1u},  // bit0: D3D11 Present hook target
            {"NativeInvocation",  2u},  // bit1: native registration table
        };
        const uint32_t mask = world.Resolved();
        for (auto& d : pdefs)
        {
            const bool ok = (mask & d.bit) != 0;
            g_hooks.push_back({d.name, "GTA5.exe", "pattern-resolved", ok});
            GLOGI("gage_mp:   %-16s %s", d.name, ok ? "resolved" : "unresolved (degrades)");
        }

        std::wstring host = reg::Get(L"server_host");
        std::wstring port = reg::Get(L"server_port");
        std::wstring playerName = reg::Get(L"player_name");
        GLOGI("gage_mp: target server %s:%s player=%s",
            host.empty() ? "(none)" : Narrow(host).c_str(),
            port.empty() ? "0" : Narrow(port).c_str(),
            playerName.empty() ? "(default)" : Narrow(playerName).c_str());

        join::Config cfg;
        cfg.host   = host;
        const long portNum = wcstol(port.c_str(), nullptr, 10);
        cfg.port   = (portNum >= 1 && portNum <= 65535) ? (int)portNum : (int)proto::kDefaultPort;
        cfg.player = playerName.empty() ? L"player" : playerName;

        // Forward session events ("connected", "disconnected", "playerJoined",
        // "playerLeft") to the mp.* bridge (the JS layer subscribes via
        // GageMP_EventAdd; unknown events are dropped there).
        join::SetEventSink([](const char* event, const char* payload) {
            GageMP_Dispatch(event, payload);
        });

        // Start the in-game engine: install the main-loop detour, then drive the
        // GAGE session (connect + peer peds) in the background, throttled per frame.
        engine::Start(&world, cfg);
        GLOGI("gage_mp: engine started (tick hook %s)",
            world.Hooked() ? "installed" : "PENDING (main_loop unresolved)");

        if (host.empty())
            GLOGI("gage_mp: no server configured; world stays singleplayer (companion demo still runs)");

        GLOGI("gage_mp: mp.* bridge ready (invoke / events / hooks)");
    }

}

// ---- mp.* JS bridge + engine surface (C ABI for the CEF/V8 layer) --------
extern "C" {

    const char* GageMP_Version()
    {
        return "Solar RP 1.0.0";
    }

    // mp.invoke(name, payload) -> result string.
    const char* GageMP_Invoke(const char* name, const char* payload)
    {
        if (!name)
            return "error:badarg";
        std::string key = name;
        const char* p = payload ? payload : "";
        if (key == "chatMessage" || key == "command")
        {
            GLOGI("mp.invoke %-18s %s", name, p);
            engine::SendChat(p);                 // over the wire to the server
            g_lastResult = std::string("ok:") + p;
        }
        else if (key == "focus" || key == "setTypingInChatState")
        {
            GLOGI("mp.invoke %s", name);
            g_lastResult = "ok";
        }
        else
        {
            GLOGI("mp.invoke %-18s (unknown, accepted)", name);
            g_lastResult = "ok:" + key;
        }
        return g_lastResult.c_str();
    }

    // mp.events.add(event, sink) -> handle id.
    uint32_t GageMP_EventAdd(const char* event, void* sink)
    {
        if (!event || !sink)
            return 0;
        g_bus[std::string(event)].push_back(sink);
        return ++g_eventCounter;
    }

    void GageMP_EventRemove(const char* event, uint32_t id)
    {
        auto it = g_bus.find(std::string(event));
        if (it != g_bus.end() && id >= 1 && id <= it->second.size())
            it->second.erase(it->second.begin() + (long)(id - 1));
    }

    // Engine -> JS event dispatch.
    void GageMP_Dispatch(const char* event, const char* payload)
    {
        if (!event)
            return;
        auto it = g_bus.find(std::string(event));
        if (it == g_bus.end())
            return;
        for (void* s : it->second)
            GLOGI("mp.event %s -> sink %p payload=%s", event, s, payload ? payload : "");
    }

    int GageMP_HooksCount() { return (int)g_hooks.size(); }
    const char* GageMP_HookName(uint32_t i) { return (i < g_hooks.size()) ? g_hooks[i].name : ""; }
    int GageMP_HookApply(uint32_t i)
    {
        if (i >= g_hooks.size())
            return -1;
        g_hooks[i].applied = true;
        GLOGI("hook applied: %s (%s!%s)", g_hooks[i].name, g_hooks[i].module, g_hooks[i].symbol);
        return 1;
    }

}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        HANDLE t = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)Run, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
