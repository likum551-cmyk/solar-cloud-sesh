# Sync / bug batch report (2026-11-03), branch `cloud/sync-fixes`

Seven agents (A-G) audited and fixed the problems GI reported after the 0.4.4/0.4.5 batch: players slide, cars do not collide, animations not showing (others / self), melee punches doing no damage / too much damage / not showing, and general bugs. Base: `origin/main` 7c6fb91. Merged here one by one (review, merge, rebuild, tests by name). `main` and `kClientVersion` untouched; protocol still v5 (no wire change).

**Nothing here ran in a game. There was no 0.4.5 live log.** Every item is CODE AUDIT + SIMULATION (the real controllers driven against simulated peds, cars and relays) or compiled-only. Every behaviour change has a pure test that fails on the old logic (mutant proven by each agent). Each agent added per-10 s diagnostics; the "Read the next live log" list at the end says which numbers decide what.

## Final test state (all by explicit name)
| Suite | Result | Suite | Result |
|---|---|---|---|
| sync_test | 107/107 | server_test (native) | 111/111 |
| locomotion_test | 77/77 | netcode_test (native) | 208/208 (95.8 s) |
| vehicle_drive_test | 72/72 | decode_fuzz_test (new) | 172 checks |
| gameplay_test | 403/403 | join_test | 56/56 |
| gage_mp_test | 18/18 | quant / interest | 35/35, 50/50 |
| serverquery / version_gate | 22 / 61 checks | launcher-ui | 143/143 |
Engine DLL and launcher link. Baseline on main: sync 86, locomotion 63, vehicle_drive 53, gameplay 343, server 106, netcode 196.

## A. On-foot sliding (libs/sync/locomotion.*, +14 checks)
- Root causes (all simulated): (1) the anchor lead had a 1.5 m floor, so a stopping puppet ran ~0.9 m past the pose and was dragged back in an idle pose (1.5 m idle slide per stop of a run); (2) move rate was open loop on inferred nominal speeds; (3) a starting puppet was dragged while the start clip ramped; (4) the velocity push could integrate if GET_ENTITY_VELOCITY returns the velocity we set; (5) a dead TASK_GO_TO_ENTITY was noticed only after 2.5 s.
- Fix: closed-loop move rate with a learned per-gait speed scale; settle phase at stops; push only as a last resort (dead band 0.30 m, start grace 1.2 s); contaminated-velocity-read guard; first stall noticed after 0.9 s with doubling fallback.
- Simulated worst skate 25.2% -> 6.4%; run/stop idle slide 5.9 m -> 0.05 m; dead anchor 41% -> 2.8%. Tradeoff: ~1 m position lag for ~1 s at the start of a run (knobs `startGraceMs`, `residualDeadM`).
- Unverified live: SET_PED_MOVE_RATE_OVERRIDE scaling stride, GET_ENTITY_VELOCITY semantics, TASK_GO_TO_ENTITY following the frozen anchor (`loco_mode=straight` reverts).

## B. Vehicle collisions (vehicle_drive.*, vehicle_contact.h, +8 checks)
- No collision-flag bug: collision is never touched on vehicles. A remote copy is solid but kinematic (velocity/angular velocity overwritten every frame), so it acts as a wall and the impulse it takes is erased. W9's contact grace depended on HAS_ENTITY_COLLIDED_WITH_ANYTHING (puppet only, rising edge only) and missed T-bones with a silent / stuck / late flag and every hit on a standing car.
- Fix: third trigger, a velocity "kick" (|commanded - actual| > 3 m/s, any direction, any target speed) with a storm budget that mutes it if the game does not honour velocity writes within a frame. T-bone local-car minimum speed 0.19 -> 1.60 m/s in the planar sim.
- Not fixed: parked/empty cars are puppets for everyone but the last driver (server vehicles.h:86 keeps control to 250 m), so rammed parked cars are immovable (server/design decision); the two screens place a contact differently (render delay + present lead; needs a wire message).
- Natives wished for, not added: collision-state read-back, GET_MODEL_DIMENSIONS, IS_ENTITY_TOUCHING_ENTITY.

## C. Network / smoother / clock pipeline (libs/sync/smoother.*, netclock.*, apps/server, +21 sync + 4 server checks)
- The pipeline is numerically sound (no time-base, wrap, reorder, snap or bandwidth defect found). Latency budget: sender frame to shown pose 126 ms near / 160 mid / 193 far (docs/sync-specs/network.md).
- Fixed: (1) the 1 cm wire position grid became a 6% pose-speed ripple at walking speed, which the locomotion controller reads as gait; a velocity-aided denoise (`denoiseGain 0.3` via ForTickHz, inside a noise band) takes walking speed error p95 0.36 -> 0.08 m/s; (2) ENet's packet throttle silently deletes unreliable state packets during congestion; the server now calls `enet_peer_throttle_configure(5000, 2, 0)` (applies to the client's peer too, so a server-only redeploy covers old clients).
- Pipeline conclusion: most of "slide / no collision" is receiver-side (A, B, D), not the network.
- New server `sync 10 s` line and engine `pipe clock` / `pipe id=` lines.

## D. Vehicle motion and occupants (vehicle_drive.*, vehicle_link.*, vehicle_syncd.h, tests/vehsim.h, +11 + 4 checks)
- O1 (high, fuzz-proven): LocalVehicleLink ghost seat: leaving while a seat change / Enter was unanswered left the server thinking the player was still seated, so every client kept warping his puppet back in (and "double F" removed the NEW seating). 641 of 20000 random legal sequences ended bad (3.2%) -> 0.
- V1: the interpolator pinned a car at its oldest snapshot while Buffering, so after a hand-over / first sight it popped delay*speed (2.6 m at 25 m/s, 14 m at 139 m/s); now walks back along the snapshot's motion (0.11 m / 0.59 m).
- V2: every controller change cleared the interpolator, even between two other controllers; hand-over now resets only to/from us.
- O2 (inferred): the puppet's own door walk then teleport; default now follows the sender's pose (`puppet_enter=task` reverts).
- Not changed: seat limiter can keep a repeatedly displaced puppet out of its seat up to 91% of the time (knob `maxGapMs`); server rejects vehicle velocity over 150 m/s (hyper car at 139 m/s leaves ~11 m/s headroom: an overshoot freezes the car on other clients); possible engine auto-start on puppets (`disableAutoStart=0`).

## E. Animations (libs/sync/emote.*, swing.h, tests/anim_audit_tests.h, +48 checks)
- Root cause not provable offline. Most likely for "0 peer swings shown": the swing dictionary/clip names are INFERRED, and TASK_PLAY_ANIM on a bad clip is a silent no-op that the old code never checked.
- Fixed provable defects: REQUEST_ANIM_DICT sent once and never retried; a swing arriving while the dictionary streams is held 250 ms instead of lost; a wrong emote clip name now tells the player; upper-body emote on a puppet cleared the gait without telling the locomotion controller; restart leaving the first task / secondary anim stuck; loop emote cancelled by knockdown re-issued up to twice on puppets.
- No emote UI exists (the player types exact dictionary/clip names); no presets invented.

## F. Melee / combat damage (libs/sync/combat.h, apps/server, tests/melee_sim.h, +8 gameplay + 12 netcode checks)
- Arithmetic (defaults: health 200, melee 50%): fists 40 claimed -> 20 applied (10 punches kill); knife and bat both 20 on the event path (the client claims 40 for every melee weapon, so the server's 60 / 80 ceilings never show); carbine 23 per hit.
- Fixed: (1) **late blows refused with no feedback**: the server's 500 ms hit window versus the client reporting blows up to 900 ms after the press; melee now accepts a swing up to 1500 ms old (`kMeleeShotWindowMs`, guns keep the configured window). This also explains "not showing": the attacker's only hit feedback is the server's Damage. (2) **ghost hit**: a standing damage event from a blow outside the window fired as a hit on the next swing; `MeleeEventGate` clears it when a window opens. Server counts every refusal by reason (`combat: last 10 s` line).
- Left for the owner (tuning, defaults unchanged): fists 20/punch (`meleeDamagePercent`, `kMeleeNominalDamage`); flat 40 claim for all melee weapons; 2 Hits per melee swing allowed.
- Security note: the melee window is wider on the server (1500 ms vs 500 ms); it only affects melee Hits and the claim is still capped by weapon ceiling and the damage budget.

## G. General bug sweep (+1 server, +1 join checks, decode_fuzz_test 172)
- Fixed: admin target of 5 digits was cast to 16 bits (`ban 65536` banned player 0, `kick 65537` kicked player 1); any join refusal made the client give up until restart (NameTaken after a dropped connection is the likely real trigger): four transient refusals now retry every 5 s.
- Fuzz: every protocol Decode and the v1/v2 query against truncation, trailing bytes, bit flips and random data, clean under ASan/UBSan: no decode defect.
- Suspected, not changed: reconnect while dead can stay dead ~30 s; `/tp nan`; a malformed `GAGE_LATEST_CLIENT_VERSION` (trailing whitespace) is fatal; chat accepts bidi overrides; `updater.cpp` ParseVersion accepts parts above 255; CLAUDE.md says `kUseBrandArt` is false but it is `true` (the art is drawn).
- Suspected in F's area: TickLocalMelee reads controls 140-142 with IsDisabledControlJustPressed while they are not disabled; if the native only reports disabled controls, only the melee-action edge counts swings (check the "swing counted ... control press N" line).

## Integration notes
- Conflicts resolved by keeping both sides: engine.cpp (includes, Peer fields, drive loop), vehicle_drive_test.cpp, gameplay_test.cpp (expected sums), vehicles.md, server_test.cpp (expected 111).
- C's new server_test check used locals named `near` / `far`, which are macros in the Windows headers: the mingw build failed; renamed.
- `tools/deploy/wine-selftests.list`: added `decode_fuzz_test` (soft) because build-client.sh fails on an unclassified selftest. `pack_collection_test` (from the R3 WIP commit on main) is also unclassified and still needs a line there.
- Suggested lessons.md lines (not added): a sticky game flag read only inside a window must also be cleared when the window opens; do not name variables `near`/`far` in code built for Windows.

## Read the next live log (gage.log, every 10 s) and what each decides
- `engine: loco slide id=`: skate under ~5%, idle slide < 0.5 m, speed-from-positions within 5% of pose, stop-starts 0, anchor stalls 0, contaminated reads 0; learned gait scale stuck at its clamp (0.55 / 1.6) = the move-rate native does not scale speed.
- `engine: vehicle collision (Sync B)` and the W9 line: flag true ~0% on a ground car; "kick mutes" > 0 = velocity writes are not honoured within a frame; flag-true frames overwritten at full weight = contact the grace did not soften.
- `engine: vehicle drive (D)` / `occupants (D)`: position mean < 0.15 m, heading max < 15 degrees (large heading error = try `veh_angframe=body`); seat displacements "in another seat" > 0 = GTA shuffles despite PCF 184; "no car" > 0 = something ejects puppets.
- `engine: anims (0e)` and `anim first use` lines: "SEEN playing" vs "probably WRONG" per clip; quick test `/e melee@unarmed@streamed_core heavy_punch_a`.
- `engine: melee (0c)` audit fields and the server `combat: last 10 s` line: windows opened near a puppet but no hits = GTA never records the damage event; a nonzero "accepted only through the 1500 ms window" confirms the late-blow fix mattered; large `no_shot` / `melee_reach` / `weapon_not_held` = next problem.
- `engine: pipe clock` / `pipe id=` and server `sync 10 s`: near stored >= ~27/s, p95 gap < 60 ms, enet throttle 32/32 (deceleration 0).
