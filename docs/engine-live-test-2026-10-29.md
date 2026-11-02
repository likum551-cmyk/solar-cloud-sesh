# First live engine test, 2026-10-29

The first run of `gage_mp.dll` in a real GTA V, on a tester's PC, joining the
owner's local `gage-server` through a playit UDP tunnel. The owner confirms the
failure is on our side (the tester's PC and game are fine).

## Setup

- Installer `GAGE-MP-Setup-0.1.31.exe`, built with `-DGAGE_TEST_SERVER=domination-there.tun.ply.gg:17565`
  (demo mode: no platform API, so PLAY joins the test server).
- Game: Steam, GTA V Legacy (app 271590), `F:\SteamLibrary\steamapps\common\Grand Theft Auto V\GTA5.exe`,
  started with `steam.exe -applaunch 271590 -nobattleye`.
- Server: `gage-server` (protocol v2) on the owner's PC, UDP 24800, behind playit.

## What worked

| Step | Evidence in gage.log |
|---|---|
| Store launch | `launch: game GTA5.exe pid 8156, parent Launcher.exe (started through its store)` |
| Integrity + inject | `integrity: 3 file(s) match the build`, `launch: inject ok in 0 ms` |
| Loader, main-thread load | `trampoline done=1 ... status=0x00000000 crashed=0`, `gage_mp.dll -> 00007ffb4fa90000` |
| Engine attach | `gage_mp: engine attached (pid 8156, 0.4 s after game start)` |
| Present hook | `hook: D3D11 Present detoured ... (patched 5 bytes)`, `engine: tick hook installed` |
| Join over the internet | `join: joined domination-there.tun.ply.gg:17565 as id 1 (server 'GAGE Test', 20 Hz)` at 19:49:55.459, about 150 ms after `joining` |

## What failed

1. **`hook: native registration table not found (native invocation disabled)`** at 19:49:55.010.
   `gage_mp: ... resolved=0x1 (unresolved: native_table)`, `NativeInvocation unresolved (degrades)`.
   With no natives the engine cannot read the local player or spawn remote peds, so nothing
   multiplayer can appear in game even with a stable session.
2. **The game stops responding** shortly after the engine's patches (logo-movie patch at
   19:49:54.991, MainLoopDetour, Present detour at 19:49:55.302).
   - `join: session with ... ended (lost)` at 19:50:07.658, 12.2 s after the join. The session runs on
     its own worker thread (`apps/engine/join.cpp`), so "lost" (ENet peer timeout) suggests the
     whole process stalled, not only the render thread. Unverified.
   - `game pid 8156 exited after 98 s, exit code 0xCFFFFFFF` (the tester may have closed it).
   - The log has no engine line after 19:49:55.302 other than the join and the loss, so the
     freeze point is not pinned down.

## Open questions for whoever takes the engine

- Does the freeze happen with the engine's patches disabled one by one (logo patch,
  MainLoopDetour, Present detour)? There is no config switch for that today; every patch is
  unconditional.
- Why does the native table lookup fail on 1.0.3889 (`hook.cpp`)? And was the game already
  past its loading stage when it ran (0.4 s after start)?
- Should the engine wait for the game world to load before it joins? It joined 0.6 s after
  game start, during the boot sequence.
- Does a crash file exist next to gage.log on the tester's PC (crash handler was installed)?
  Server console lines for the session (the `leave` reason) were not collected yet.
