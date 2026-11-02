// GAGE-MP launcher.
//
// The stage-1 entry point (a CLI harness for the inject chain). It resolves a
// server, persists the selection + install dir to the registry, then drives the
// injection chain: injects gageinjector.dll into the game (or into itself for
// --selftest), which loads gage_loader.dll, which loads the engine.
//
// Usage:
//   gage_v [--selftest] [--dump] [--target <name>] [--game <path>]
//          [--server <host:port|ip:port|ws://host:port>]
//          [--endpoint <url>] [--name <character>] [--version]
//
//   --server 1.2.3.4:24800     connect straight to a server (no list fetch)
//   --endpoint https://...     fetch a JSON server list and pick the best

#include "common/common.h"
#include "common/inject.h"
#include "common/registry.h"
#include "common/config.h"
#include "common/logger.h"
#include "net/serverfinder.h"

#include <windows.h>
#include <vector>
#include <string>

using namespace gage;

static const char* kVersion = "Solar RP 1.0.0 launcher (gage_v dev harness)";

static void PrintBanner()
{
    printf("\n  %s\n", kVersion);
    printf("  chain: gage_v -> gageinjector -> gage_loader -> gage_mp\n\n");
}

static bool ParseArgs(const std::vector<std::string>& a,
                      std::wstring& target, std::wstring& game,
                      std::wstring& endpoint, std::wstring& name,
                      bool& selftest, bool& dump)
{
    for (size_t i = 0; i < a.size(); ++i)
    {
        std::string v = (i + 1 < a.size()) ? a[i + 1] : "";
        if (a[i] == "--selftest")              selftest = true;
        else if (a[i] == "--dump")             dump     = true;
        else if (a[i] == "--target")           target   = Widen(v);
        else if (a[i] == "--game")             game     = Widen(v);
        else if (a[i] == "--endpoint")         endpoint = Widen(v);
        else if (a[i] == "--name")             name     = Widen(v);
        else if (a[i] == "--server")
        {
            // Direct host:port / ip:port / ws://host:port. Takes precedence
            // over --endpoint when both are given (last one wins, like flags).
            endpoint = Widen(v);
        }
        else if (a[i] == "--version")
        {
            printf("%s\n", kVersion);
            exit(0);
        }
        else if (a[i] == "-h" || a[i] == "--help")
        {
            PrintBanner();
            printf("  --selftest      inject the whole chain into this process and verify it\n"
                   "  --dump          resolve the server, print the plan, exit (no injection)\n"
                   "  --target NAME   game image name to inject (default GTA5.exe)\n"
                   "  --game PATH     full path to the game executable\n"
                   "  --server SPEC   connect straight to a server (no server-list fetch).\n"
                   "                  Accepts a hostname or an IP, with or without a port,\n"
                   "                  e.g. play.example.org:24800  |  192.168.1.5:24800\n"
                   "                  |  192.168.1.5 (port 24800)  |  ws://host:port\n"
                   "                  |  [v6]:port. ws/wss also accepted; /path is ignored.\n"
                   "  --endpoint URL  fetch a JSON server list (http/https) and pick the best\n"
                   "  --name CH       in-game player / character name (persisted)\n"
                   "  --version       print version\n");
            exit(0);
        }
        else
        {
            printf("  (unknown arg: %s)\n", a[i].c_str());
        }
    }
    return true;
}

// Poll until the engine module is loaded in our own process (selftest sync).
static bool WaitForEngine(DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs)
    {
        if (GetModuleHandleW(L"gage_mp.dll") != nullptr)
            return true;
        Sleep(50);
    }
    return GetModuleHandleW(L"gage_mp.dll") != nullptr;
}

int main(int argc, char** argv)
{
    PrintBanner();

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);

    std::wstring target, game, endpoint, name;
    bool selftest = false, dump = false;
    ParseArgs(args, target, game, endpoint, name, selftest, dump);

    std::wstring dir = ExeDir();

    GLOGI("launcher: install dir  = %s", Narrow(dir).c_str());
    GLOGI("launcher: admin        = %d", IsAdmin() ? 1 : 0);
    if (!IsAdmin())
        GLOGW("launcher: not elevated; anti-cheat / admin integrity may require UAC");

    // ---- 1. Server finder -------------------------------------------------
    net::ServerInfo server = net::ResolveServer(endpoint);
    GLOGI("launcher: server       = %s:%d %s",
        Narrow(server.host).c_str(), server.port, Narrow(server.name).c_str());

    // ---- 1b. Game exe -----------------------------------------------------
    // --game flag, else a previously saved game_path, else a standard install
    // location. Remember whatever we land on so the next run (and the GUI)
    // starts pre-filled.
    if (game.empty())
        game = reg::Get(L"game_path");
    if (game.empty())
        game = FindGameExe();
    if (!game.empty() && FileExists(game))
    {
        GLOGI("launcher: game         = %s", Narrow(game).c_str());
        reg::Set(L"game_path", game);
        cfg::Set(L"game_path", game);
    }
    else if (game.empty())
        GLOGW("launcher: no game exe found; passing --game <GTA5.exe> or set gage_mp.cfg / HKCU\\Software\\GAGE-MP\\game_path");

    // ---- 1c. Player / character name --------------------------------------
    // --name flag, else a previously saved name. Remember it so the injected
    // engine and the GUI start with the same character.
    if (name.empty())
        name = reg::Get(L"player_name");
    if (!name.empty())
    {
        GLOGI("launcher: player name  = %s", Narrow(name).c_str());
        reg::Set(L"player_name", name);
        cfg::Set(L"player_name", name);
    }

    // Persist selection + install dir for the injected chain and the settings file.
    reg::Set(L"install_dir", dir);
    reg::Set(L"server_host", server.host);
    reg::Set(L"server_port", std::to_wstring(server.port));
    reg::Set(L"server_name", server.name);
    cfg::Set(L"server_host", server.host);
    cfg::Set(L"server_port", std::to_wstring(server.port));
    cfg::Set(L"server_name", server.name);

    if (dump)
    {
        GLOGI("launcher: --dump: plan ready (target=%s, selftest=%d)",
            Narrow(target.empty() ? std::wstring(L"GTA5.exe") : target).c_str(), selftest ? 1 : 0);
        printf("  plan: install=%s\n         server=%s:%d\n         target=%s\n",
            Narrow(dir).c_str(), Narrow(server.host).c_str(), server.port,
            Narrow(target.empty() ? std::wstring(L"GTA5.exe") : target).c_str());
        return 0;
    }

    // ---- 2. Resolve the injection target ----------------------------------
    std::wstring injectorPath = dir + L"gageinjector.dll";

    HANDLE hTarget;
    DWORD   targetPid = GetCurrentProcessId();
    bool    inProcess = false;

    if (selftest)
    {
        inProcess = true;
        targetPid = GetCurrentProcessId();
        GLOGI("launcher: selftest: injecting chain into this process (pid %lu)",
            (unsigned long)targetPid);
    }
    else
    {
        if (target.empty())
            target = L"GTA5.exe";
        GLOGI("launcher: target = %s%s", Narrow(target).c_str(),
            game.empty() ? "" : Narrow(game).c_str());
        hTarget = inject::GetOrOpenProcess(target, game);
        if (!hTarget)
        {
            GLOGE("launcher: could not open/launch target: %s",
                Narrow(FormatError(GetLastError())).c_str());
            return 2;
        }
        targetPid = GetProcessId(hTarget);
    }

    // Tell the injector where to deliver the loader.
    reg::Set(L"inject_target", std::to_wstring(targetPid));

    // ---- 3. Inject the stage-2 injector -----------------------------------
    if (!FileExists(injectorPath))
    {
        GLOGE("launcher: missing %s", Narrow(injectorPath).c_str());
        return 3;
    }

    bool ok = false;
    if (inProcess)
    {
        // Self-test: load the injector into ourselves via the real injector path
        // (CreateRemoteThread/LoadLibraryW) so the full chain is exercised.
        ok = inject::InjectDll(GetCurrentProcess(), injectorPath, 30000);
    }
    else
    {
        HANDLE h = (targetPid == GetCurrentProcessId())
            ? GetCurrentProcess()
            : inject::GetOrOpenProcess(target, game);
        if (h)
        {
            DWORD err = 0;
            ok = inject::InjectDll(h, injectorPath, 30000, &err);
            if (h != GetCurrentProcess())
                CloseHandle(h);
            GLOGI("launcher: InjectDll(gageinjector) ok=%d err=0x%08X", (int)ok, (unsigned)err);
        }
    }

    if (!ok)
    {
        GLOGE("launcher: injector injection FAILED");
        return 4;
    }

    // ---- 4. Verify the chain (selftest) -----------------------------------
    if (inProcess)
    {
        bool engine = WaitForEngine(8000);
        GLOGI("launcher: engine loaded = %d", engine ? 1 : 0);

        printf("\n  selftest: %s\n", engine ? "PASS — full chain loaded (injector -> loader -> engine)"
                                            : "engine not detected; see gage.log");
        return engine ? 0 : 5;
    }

    GLOGI("launcher: injection dispatched into pid %lu — done", (unsigned long)targetPid);
    printf("\n  injected gageinjector.dll into pid %lu (game). chain continues in-process there.\n",
        (unsigned long)targetPid);
    return 0;
}
