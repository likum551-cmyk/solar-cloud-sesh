# Cloud session handoff (2026-11-02, release 0.4.3)

You are continuing work on Solar RP (code name GAGE-MP) in a Claude Code cloud session. The owner is GI (address him as "GI"; terse, verified claims only). Read `CLAUDE.md`, `.claude/rules/lessons.md` and this file first. The local PC session (Windows) did W0-W8; you do the queue below, then GI pulls your branch back to the PC, which builds and releases.

## Output
- Work on branch `cloud/0.4.3` (create it from `main`). One commit per queue item, message `W9: ...`, `W10: ...`, `UI: ...`. Push the branch; do NOT push to `main`, do not open a PR unless GI asks.
- At the end write `docs/CLOUD-REPORT.md`: per item what changed, the tests that prove it (and that they failed on the old logic), test pass lines by name, anything only compiled / unverified live, anything you could not do.

## Queue (in order, one writer at a time)
0. **netcode_test flake (release blocker: deploy-server.sh runs it on the VPS).** On the PC it fails 1 run in 3-4 with exactly two checks: "invalid: an out-of-box position is dropped..." (`inv 0, mal 0, neverFar 1`) and "invalid: bad state packets never disconnect the sender" (`joined 0, players 3`, expected joined and 6). So probe `a` and two other probes had already been disconnected before tests/netcode_test.cpp ~1343: the rig's ENet peer timeouts are 400..1000 ms (tests/netrig.h:101-103), so any step that stops servicing the client sessions for about a second drops them. The raw sends there are already reliable (2026-11-02). Find the step that starves them (the 30 Hz near/mid block ~1259-1295, the stale/wrap blocks with `rig.Pump(1300)`, or any `Until` that does not pump every probe), fix the test harness (not the server's real timeouts), and prove it with 10 consecutive green runs; report the per-run time.
0b. **W11: on-foot locomotion still slides (GI's top complaint, 0.4.3 live test 12:48-12:57).** Delivery is fixed (about 30 samples/s stored), W8 cut churn, but on-foot windows still issue 8-12 TASK_GO_STRAIGHT_TO_COORD per 10 s. The new `engine: loco` line shows why: `first of a gait` 6-8 per 10 s (every walk/run/sprint change re-issues the task, and the gait flips often), `turn` 4-6, `drift` 3-5. Every re-issue restarts the stride; between them the velocity push drags the ped, which reads as sliding. Fix, in this order, with a locomotion_test case per step that fails today:
   1. A gait change never re-issues the task: change speed on the running task with `SET_PED_DESIRED_MOVE_BLEND_RATIO` (already in hook.h) plus the move-rate override; add hysteresis to the gait choice (a gait must hold for ~400 ms, and walk/run/sprint thresholds get a dead zone) so it stops flipping.
   2. Turns without re-issue: evaluate a continuously steered target instead of a fixed far point. Preferred: one invisible, collision-less anchor entity per remote ped, the ped runs `TASK_GO_TO_ENTITY(ped, anchor, -1, 0.1, moveBlend, 0, 0)` once, and the anchor is moved every frame to the predicted position about lead metres ahead along the smoothed path; re-issue only when the task is lost. Keep the current go-straight path as a fallback (`loco_mode=straight`). If an anchor is used: create it with existing natives, never networked, deleted with the puppet; count it in the loco line.
   3. The velocity push only corrects the residual error, low-passed and capped (spec 3c, not done in W8), so the animation's own displacement carries the ped; a hard snap only beyond the existing teleport threshold.
   4. Target after the fix: a straight run or sprint with gait changes and gentle turns issues at most 1 task per 10 s; the loco line shows it.
   New natives allowed (public / 3889, both sources agree): TASK_GO_TO_ENTITY 0x6A071245EB0D1882 / 0xAB3658A740EED98E (Entity entity, Entity target, int duration, float distance, float moveBlendRatio, float slowDownDistance, int flags); GET_PED_DESIRED_MOVE_BLEND_RATIO 0x8517D4A6CA8513ED / 0x864E9BBAA7BFA064 (float, Ped); SET_PED_MAX_MOVE_BLEND_RATIO 0x433083750C5E064A / 0x4190C67D524CD0DC; SET_PED_MIN_MOVE_BLEND_RATIO 0x01A898D26E2333DD / 0x7992D566A90CD49C. Already in hook.h: SET_PED_DESIRED_MOVE_BLEND_RATIO, SET_PED_DESIRED_HEADING, TASK_GO_TO_COORD_WHILE_AIMING_AT_COORD.
   Also GI: "passenger teleports" is still live in 0.4.3: that is W9 items 1-2 (EnterWarpDue, PCF 184 on puppets, re-warp rate limit); add a per-10 s re-warp counter so the next test proves it.
1. **W9: vehicles, passengers, collision part 1-2.** Spec: `docs/sync-specs/live-0.4.2-fixes.md`, section W9 (items 1-8). Section W8 is already done (read its result in the code; do not redo it).
2. **W10: player name tags.** Same spec, section W10.
3. **UI: launcher forced-update popup** (apps/launcher-ui SPA). When the native side reports an update in progress (`apps/launcher/BRIDGE.md`; refusal codes 14 "update required" and 15 "server not updated yet" from `apps/launcher/version_gate.h`), show a modal "Updating Solar RP..." with download/install progress; not dismissable while the update is mandatory (every update is mandatory now); code 15 gets its own copy ("This server has not been updated yet. Try again in a minute."). Match the existing launcher-ui design system (tokens, type, motion); keep the bundle build working (`npm run build` in apps/launcher-ui, Node 22.12+). Visual review happens on the PC afterwards (the reviewer tools are local), so leave a short note of the states to capture.

4. **Update ping from the server (GI 2026-11-02).** Launchers should learn about a new client from the server they already query every 5 s, not only from the 2-minute manifest poll; today only a protocol change is noticed that way, a same-protocol release (like 0.4.3) is not. Design: UDP query format v2 (`libs/protocol/query.h`, fixed 24-byte datagrams today): a v2 request gets a v2 reply that also carries the latest client version (x.y.z as 3 bytes) the server was released with; a v1 request still gets the exact v1 reply (0.4.2/0.4.3 launchers must keep working; their Decode refuses any other length). The server reads the version from `server.json` key `latestClientVersion` (optional; absent = 0.0.0 = unknown), and `tools/deploy/release.sh` sets it in the staged release's config, never rewriting the operator's other keys (or passes it on the command line; pick the least invasive and document it). Launcher (`apps/launcher/gage_gui.cpp`, the status worker and the launch gate): when the server's latest client > kClientVersion, start the update check at once (same rate limit as a protocol-ahead server) and refuse PLAY with code 14 until updated; extend `version_gate.h` + `version_gate_test` for it, and serverquery_test/server_test for v1/v2 replies. Docs: `docs/api/protocol.md` (query section), `docs/UPDATES.md`, `docs/OPS.md`.

## Hard rules
- **Native hashes: never guess.** The lookup tool and its data are NOT in this repo. Use only hashes already in `apps/engine/hook.h` / `kCrossmap3889` or listed in the table below (both sources agreed, looked up on the PC). If you need a native not listed: do not add it; write it in CLOUD-REPORT.md under "natives needed" and implement around it or leave the item partial.
- Convention in hook.h: the constant is the PUBLIC hash; the 3889 hash goes in `kCrossmap3889`. Follow how existing natives are added (grep an existing one, e.g. `SET_PED_CONFIG_FLAG`, and mirror every place it appears).
- Every behaviour change gets a pure unit test that FAILS on the old logic (prove it once against the old logic or a mutant).
- No game runs here. Never run `gage_v.exe`; run selftests only by explicit name (never a glob).
- Natives are called outside `g_peersMtx` (existing engine.cpp rule).
- No new dependencies, no file deletions, no broad refactors, no changes to `tools/deploy/*` except where an item says so. Shell scripts stay LF.
- After any `CMakeLists.txt` edit: `launcher-ui/dist`, `crypt32`, `launcher-assets`, `GAGE_TEST_SERVER` must still appear (8 matches) and the `GAGE_CASEFIX_DIR` block must stay.
- Do not bump `kClientVersion` (the PC release step does). Protocol stays v5 (`kVersion = 5`): no new wire fields in this release.
- Never put secrets, keys or tokens in the repo or logs. Untrusted data stays out of the repo.

## Build and test on Linux (Ubuntu)
Server and portable libs, natively:
```bash
sudo apt-get update && sudo apt-get install -y cmake ninja-build g++
cmake -S . -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAGE_SERVER_ONLY=ON
cmake --build build/server --parallel
build/server/selftest/netcode_test && build/server/selftest/server_test
```
Windows client (engine, launcher, all selftests) by cross-compiling, as the VPS does (`cmake/toolchain-mingw-linux.cmake`):
```bash
sudo apt-get install -y gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev
cmake -S . -B build/win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-linux.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/win --parallel
```
The launcher UI is embedded from `apps/launcher-ui/dist/index.html`: run `npm ci && npm run build` in `apps/launcher-ui` first (Node 22.12+), or the launcher target fails. The Windows selftests (`build/win/selftest/*.exe`) need wine (`sudo dpkg --add-architecture i386 && sudo apt-get install -y wine64 wine32` or the distro `wine`); run by name with `wine build/win/selftest/<name>.exe`. Under wine, `legacy_handoff_test` (registry error 87) and `crash_diag_test` (dbghelp) are known environment failures; `tools/deploy/wine-selftests.list` classifies them. If wine is unavailable, the pure sync tests (sync_test, locomotion_test, vehicle_drive_test, gameplay_test) are the minimum: say in the report what ran where.

## Natives looked up on the PC (public / 3889, both sources agree)
Already in hook.h (use as is): SET_PED_CONFIG_FLAG, SET_PED_CAN_RAGDOLL, SET_VEHICLE_ENGINE_ON, GET_IS_VEHICLE_ENGINE_RUNNING, GET_ENTITY_SPEED, TASK_LEAVE_VEHICLE, SET_PED_TO_RAGDOLL, DISABLE_CONTROL_ACTION, IS_PED_RAGDOLL, SET_ENTITY_VELOCITY, GET_ENTITY_VELOCITY, SET_VEHICLE_FORWARD_SPEED, IS_PED_IN_ANY_VEHICLE, CLEAR_ENTITY_LAST_DAMAGE_ENTITY, HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY, IS_PED_VAULTING, TASK_CLIMB.

New, allowed to add:

| Native | Public | 3889 | Returns / params |
|---|---|---|---|
| HAS_ENTITY_COLLIDED_WITH_ANYTHING | 0x8BAD02F0368D9E14 | 0x14E25ED5E75102C8 | BOOL (Entity) |
| GET_VEHICLE_CLASS | 0x29439776AAA00A62 | 0x5506AFBD6056C05F | int (Vehicle); 8 = motorcycles, 13 = cycles |
| IS_THIS_MODEL_A_BIKE | 0xB50C0B0CEDC6CE84 | 0x78B050AFBA6D1517 | BOOL (Hash model) |
| SET_PED_RAGDOLL_ON_COLLISION | 0xF0A4F1BBF4FA7497 | 0xF9E1F44FD9B4934B | void (Ped, BOOL) |
| SET_VEHICLE_RADIO_ENABLED | 0x3B988190C0AA6C0B | 0x4DC72C4B884A6C59 | void (Vehicle, BOOL) |
| GET_SCREEN_COORD_FROM_WORLD_COORD | 0x34E82F05DF2974F5 | 0x2F8A75C70AF54208 | BOOL (float x, y, z, float* sx, float* sy) |
| GET_FINAL_RENDERED_CAM_COORD | 0xA200EB1EE790F448 | 0x02AF3EA0F67D2329 | Vector3 () |
| HAS_ENTITY_CLEAR_LOS_TO_ENTITY | 0xFCDFF7B72D23A1AC | 0xAD915B5E38F323E5 | BOOL (Entity, Entity, int traceType) |
| GET_PED_BONE_COORDS | 0x17C07FC640E86B4E | 0x83FDC027F0BEA202 | Vector3 (Ped, int bone, float ox, oy, oz) |

Not available in 3889 (do not use): SET_VEHICLE_CURRENT_RPM. TASK_LEAVE_VEHICLE flags (public docs): 0 normal, 16 warp out, 4160 bail out (jump from a moving vehicle).

## Context you will want
- Plan and history: `docs/SYNC-PLAN.md`, `docs/sync-specs/*.md` (onfoot, vehicles, combat, network, w6-review-fixes, live-0.4.2-fixes).
- Public surface docs to update with any change: `docs/api/protocol.md`, `docs/api/server.md`.
- Owner rules already in place: F never jacks a player (nearest free passenger seat); an owned car is driven only by its owner; Alt+F4 shows a non-pausing confirm card; the cheat box is disabled.
- Live server: protocol v5, tickHz 30, rates 30/15/10, playerHealth 200 (VPS `server.json`, ops only; do not change deploy scripts for it).
