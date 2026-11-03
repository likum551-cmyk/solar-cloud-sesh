# Solar RP (code name GAGE-MP)

Solar RP is its own GTA V multiplayer platform: launcher (`SolarLauncher.exe`, shown to players as "Solar Launcher"), in-game engine (`gage_mp.dll`), the game server (the live one is SOL; LUNA and TERRA are planned) and master list, and an open protocol (docs/ENGINE-PLAN.md). Domain: solar-rp.com.

Naming (renamed from GAGE-MP on 2026-11-01): every string a player or admin sees says Solar RP / Solar Launcher. The internal identifiers keep the old name on purpose, because renaming them breaks upgrades, the launcher -> engine handoff or the integrity check: the installer AppId, `gage_mp.dll`, `gageinjector.dll`, `gage_loader.dll`, `gage_launch_shim.dll`, `HKCU\Software\GAGE-MP` and its value names, `gage_mp.cfg`, `gage.log`, the `%LOCALAPPDATA%\GAGE-MP` data folder, the single-instance mutex and window class, `gage::` namespaces, CMake target names (the launcher target `GAGE-MP` has OUTPUT_NAME `SolarLauncher`), the `/GAGEUPDATE` installer switch, the `gage-mp://` link scheme and the protocol identifiers. The brand images (`brand/`, launcher-ui `src/brand`, `installer/art`, the .ico) still show the old G until the new art lands; the in-game overlay does not draw them (`kUseBrandArt` in `apps/engine/overlay_internal.h`).

## Layout

```
apps/launcher/    SolarLauncher.exe native host (WebView2) + BRIDGE.md (the JS <-> native contract)
apps/launcher-ui/ the new launcher SPA (replaces the embedded legacy UI when it ships)
apps/engine/      gage_mp.dll (in-game engine)        apps/injector/  gageinjector.dll, gage_loader.dll
apps/ingame-ui/   in-game chat UI                     apps/devtool/   gage_v.exe (CLI inject-chain harness)
apps/server/      gage-server                         apps/bot/       gage-bot
apps/master/      Node master list                    apps/platform/  Node accounts/shop API
libs/             common, net, protocol, netclient, netcommon, sync (include root: "protocol/protocol.h", "common/logger.h", ...)
tests/ installer/ cmake/ third_party/ tools/ (build scripts, update-server)   brand/ (logo)   docs/ (docs/archive: retired code)
build/win, build/server   build output (disposable: delete and reconfigure)
```

## Build (Windows, local toolchain)

Local builds use the portable WinLibs toolchain in `.toolchain/winlibs-gcc13.3` (gcc 13.3, mingw-w64 msvcrt; bundles cmake 3.29 and ninja) and build into `build/win/`:

```bash
export PATH="$PWD/.toolchain/winlibs-gcc13.3/mingw64/bin:$PATH"
cmake -S . -B build/win -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++.exe -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc.exe \
  -DCMAKE_RC_COMPILER=windres.exe
cmake --build build/win --parallel
build/win/selftest/integrity_test.exe "$(cygpath -w "$PWD/build/win/bin")"
build/win/selftest/gage_mp_test.exe
build/win/selftest/game_launch_test.exe
build/win/selftest/legacy_handoff_test.exe  # the one-time RAGE-MP registry clean-up, on a scratch key
build/win/selftest/crash_diag_test.exe   # stack scan, minidump struct, MSVC throw decode, main-thread lookup
build/win/selftest/join_test.exe         # the engine's session adapter against an in-process server, loopback
build/win/selftest/netcode_test.exe      # protocol v1 + server over real ENet on loopback, about 12 s
build/win/selftest/t3_vehvisual_test.exe  # vehicle visual state (v6 tyres / windows / brake lights / steering): wire, store merge, sync module, real server, about 8 s
build/win/selftest/server_test.exe       # UDP query, master announce, bans, admin channel, about 16 s
build/win/selftest/sync_test.exe         # the remote-player smoother (libs/sync) under loss, jitter and teleports
build/win/selftest/locomotion_test.exe   # remote-ped gait controller against a simulated ped
build/win/selftest/vehicle_drive_test.exe  # remote-vehicle interpolator + drive controller
build/win/selftest/gameplay_test.exe     # v4 pure units: vitals/respawn/hit batcher/emotes/vehicle link
build/win/selftest/brand_apparel_test.exe  # branded-tee texture swap on a WARP D3D11 device
build/win/selftest/present_hook_test.exe   # Present vtable hook + watchdog (fake vtable), then World::HookTick on a real dxgi swap chain next to a simulated re-asserting overlay (the RTSS failure); part 2 needs a D3D11 device (hardware or WARP)
# also built: crash_filter_test serverquery_test interest_test quant_test launch_shim_test script_suppress_test
# NEVER run build/win/selftest/*.exe by glob: gage_v.exe there launches GTA.
```

Server only (no Windows client, no WebView2): `cmake -S . -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAGE_SERVER_ONLY=ON` (plus the compiler flags above on Windows), then `cmake --build build/server --parallel`. It builds enet, `gage_protocol`, `gage_netclient`, `gage_server_core`, `gage-server` (`build/server/server/`), `netcode_test` and `server_test` (`build/server/selftest/`). `apps/server/CMakeLists.txt` is the standalone entry point for the same set.

Own multiplayer stack (docs/ENGINE-PLAN.md, P1): the C compiler is needed for the vendored ENet (`third_party/enet`, MIT). Build just that part with `cmake --build build/win --target netcode_test gage-server gage-bot`. `gage-server.exe` lands in `build/win/server/` and `gage-bot.exe` in `build/win/tools/`, both outside `bin/` so the installer never picks them up. `gage-server --port N` reads `server.json` (see `apps/server/server.json.example`) and takes console commands `list | kick <id|name> | say <text> | quit`; `gage-bot <host> <port> <name>` walks a circle. The protocol, server and client session sources are portable C++17 (a Linux build of them has not been tried).

- `libs/` is the include root of every target: code says `"common/logger.h"`, `"net/serverfinder.h"`, `"protocol/protocol.h"`, `"netclient/session.h"`, `"sync/smoother.h"`, never `../`. A `.cpp` and its own headers use plain `"x.h"`. Tests that `#include` a whole `.cpp` (`tests/gage_mp_test.cpp`, `game_launch_test.cpp`, `integrity_test.cpp`, `crash_diag_test.cpp`) point at `../apps/<app>/<file>.cpp`.
- Default game port is `gage::proto::kDefaultPort` (24800, UDP) everywhere. The server list comes from the master server (`apps/master/`): `kDefaultListUrl` in `libs/net/serverfinder.h` is `http://127.0.0.1:8080/servers` until a public domain exists; `list_url=` in `gage_mp.cfg` overrides it.
- Per-frame hook: the engine ticks from `IDXGISwapChain::Present` by replacing slot 8 of dxgi's swap chain vtable (`apps/engine/present_hook.{h,cpp}`, called from `World::HookTick`), NOT by patching Present's code: RTSS/Afterburner and other overlays patch and re-assert that code, and they killed our inline hook after one frame (2026-11-03, `gage.log` "no frame in the last 10 s (1 in total)"). A watchdog re-takes a replaced slot and installs the old inline hook only if no frame ever arrives (a per-thread guard keeps the tick once per Present). `present_mode` in `HKCU\Software\GAGE-MP` (unset = auto, `0` = the old inline hook only, `1` = vtable only). `gage.log` names the owner of a foreign hook (`hook: Present vtable slot ... held ...`) and the overlay DLLs loaded.
- Launcher -> engine handoff is `HKCU\Software\GAGE-MP`: `server_host`, `server_port`, `player_name` (written by `DoLaunch` in `apps/launcher/gage_gui.cpp`, read by `Run()` in `apps/engine/gage_mp.cpp`). The launcher no longer touches `HKCU\Software\RAGE-MP`; a one-time clean-up (`apps/launcher/legacy_handoff.cpp`) removes only the values older GAGE-MP versions wrote there, and only when `rage_path` points into this install.
- The UI embedded into `SolarLauncher.exe` is `apps/launcher-ui/dist/index.html` (`GAGE_UI_HTML_IN` in `CMakeLists.txt`; run `npm run build` there with Node 22.12+ first). Its 3D set `dist/assets` is copied post-build to `build/win/bin/launcher-assets/` and served under `https://app.gage/assets/` only for paths in its `manifest.json`. With no platform API (`-DGAGE_API_BASE=` or `api_base=` in `gage_mp.cfg`) the exe opens the UI as `?demo=1`: labelled demo data, real bridge, launch refused with code 13. Still to wire (NATIVE-INTEGRATION.md 3.2-3.4): `launchStage`, `joinTokenNeeded`/`joinToken`, `game`, `deepLink`, code 12. The legacy SPA's recovered build stays in `docs/archive/web-legacy/dist/`.
- `third_party/webview2/x` holds the Microsoft.Web.WebView2 1.0.2903.40 NuGet package (headers only).
- Installer: `BIN="$PWD/build/win/bin" bash installer/build-installer.sh` (Inno Setup 6.7.3 portable in `.toolchain/innosetup-6.7.3`) -> `build/win/installer/SolarRP-Setup-<kClientVersion>.exe` (AppName "Solar RP"; a new install defaults to `{autopf}\Solar RP`, an upgrade keeps the previous folder, removes `GAGE-MP.exe` and the old "GAGE-MP" shortcuts, and creates the "Solar Launcher" shortcuts). It packages the exe, the 3 chain DLLs, `gage_launch_shim.dll`, `ui/` and `launcher-assets/` (the launcher UI's 3D video set). The 0.2.5 setup is 18.5 MB (built 2026-10-31): `launcher-assets/` is ~12 MB of already-compressed video, the exe + 4 DLLs ~7.5 MB, `ui/` tiny. `[InstallDelete]` removes the old `{app}\rage` runtime and `gage_eac.dll` on upgrade.
- SolarLauncher.exe bakes the SHA-256 of all 3 chain DLLs (`gageinjector.dll`, `gage_loader.dll`, `gage_mp.dll`): ship the exe and all 3 DLLs together.
- `docs/archive/assets/` holds the retired RAGE-MP client-asset downloader (not built); its README says how to bring it back.
