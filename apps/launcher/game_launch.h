#pragma once
// Starting GTA V the way the game expects to be started: through the store
// that installed it (Rockstar Games Launcher via PlayGTAV.exe, Steam, Epic),
// never by running GTA5.exe directly: PlayGTAV.exe for the Rockstar Games
// Launcher, steam.exe / a steam:// URL for Steam, the Epic launcher URI for
// Epic, with -nobattleye and a watch for the GTA5_BE.exe stub.

#include "common/common.h"

#include <vector>

namespace gage::launch {

    enum class Store { Rockstar, Steam, Epic };

    // Player-facing name: "the Rockstar Games Launcher", "Steam", "the Epic Games Launcher".
    const wchar_t* StoreName(Store s);

    // Which store installed the game whose exe is `gameExe`. The stores' own
    // records win first (Steam libraries/app manifests, Epic's install list,
    // Rockstar's registry). With no record: a .egstore folder => Epic; a
    // PlayGTAV.exe (the universal Rockstar/retail bootstrapper) => Rockstar,
    // even next to a stray steam_api64.dll; steam_api64.dll alone => Steam;
    // anything else => Rockstar.
    Store DetectStore(const std::wstring& gameExe);

    // Where GTA V is installed, from the stores' own records: Rockstar's
    // registry values and the standard folders (FindGameExe), every Steam
    // library's app manifests (libraryfolders.vdf), Epic's install list. A
    // Legacy GTA5.exe wins over GTA5_Enhanced.exe. "" when none is found.
    std::wstring FindInstalledGame();

    // Ask the store to start the game with BattlEye off (-nobattleye, story
    // mode) where the store passes arguments on. True when the request was
    // handed over; otherwise *err (optional) is a player-facing reason.
    bool StartThroughStore(const std::wstring& gameExe, Store store, std::wstring* err);

    // Start GTA V without BattlEye the way the big MP clients do: launch
    // PlayGTAV.exe (which establishes the ROS / Social Club session the game's
    // launcher guard requires) and inject `shimDll` (gage_launch_shim.dll) into
    // it. The shim redirects PlayGTAV's spawn of the BattlEye wrapper
    // (GTA5_BE.exe) to `gameExe` (GTA5.exe), so the game boots in that ROS
    // session and BattlEye never arms. True when PlayGTAV started and the shim
    // loaded; otherwise *err (optional) is a player-facing reason. The caller
    // then waits for the game and injects the engine exactly as before.
    bool StartViaShim(const std::wstring& gameExe, const std::wstring& shimDll, std::wstring* err);

    // Inject gage_launch_shim.dll into every running Rockstar Games Launcher
    // (Launcher.exe under ...\Rockstar Games\Launcher\) whose pid is not
    // already in `done`, recording each pid it attempts. On Epic (and retail)
    // that launcher -- reached through PlayGTAV but running in its own process
    // tree -- is what spawns GTA5_BE.exe, about 16 s after it starts, so the
    // shim's redirect must live inside it, not only in PlayGTAV. Returns how
    // many it injected this call. Safe to call on every poll tick while
    // waiting for the game to start.
    int InjectShimIntoLaunchers(const std::wstring& shimDll, std::vector<DWORD>& done);

    // The running game (GTA5.exe / GTA5_Enhanced.exe), if any. `battleye` is
    // true when BattlEye's stub (GTA*_BE.exe) is running or is the game's
    // parent: Solar RP must not load into that session.
    struct GameProc {
        DWORD pid = 0;
        std::wstring image;
        std::wstring parentImage;  // what started the game (for the log)
        bool battleye = false;
    };
    GameProc FindGame();

}
