# Vehicle sync spec (protocol v5), 2026-11-02

Design agent output (read-only pass over the code; line numbers as of 2026-11-02). INFERRED = not proven in code; verify. Owner rules folded in: F never jacks a player (nearest free passenger seat), an OWNED car can only be driven by its owner. Research input: docs/SYNC-RESEARCH.md.

Order by player impact: A seats / F-entry / ownership, B stutter, C one controlling client per vehicle, D steering, E lights / doors, F drive-by weapon.

## A. Seats, F-entry, ownership

Root cause:
- The link mirrors GTA's own seat (engine.cpp:3278-3283, seat cached 500 ms at :3266); vehicle_link.cpp:288-301 turns any seat change into Enter; a refused Enter sets eject (vehicle_link.cpp:191-196) and the engine runs an animated TaskLeaveVehicle flag 0 (engine.cpp:3330-3336). GTA's auto-shuffle -> SeatTaken -> jack/eject animation.
- The driver seat looked empty because puppet seating retries only every 500 ms (engine.cpp:3223) and is skipped when the peer ped is missing (:3220).
- Server: a same-vehicle shuffle skips every check (vehicles.h:256); no owner rule. Ownership is already `Vehicle::spawner` (vehicles.h:77, set :209 for Spawn and Register). PlayerGone (:283-305) destroys empty cars and orphans occupied ones (spawner=kNoPlayer, orphan=true).

Fix:
1. Prevent the shuffle: existing `SetPedConfigFlag(lp,184,true)` (hook.cpp:4235) whenever lpVeh != 0, re-asserted every 2 s. Flag id 184 (PreventAutoShuffleToDriversSeat) INFERRED: live check by sitting 10 s as passenger beside an empty driver seat.
2. Revert, do not eject: LocalVehicleLink keeps `prevSeat_` when a Linked seat change starts; a refusal of that Enter yields `out.revertSeat=prevSeat_`, stays Linked, no eject; the engine warps back with `SetPedIntoVehicle(lp,lpVeh,seat-1)` and zeroes `g_localSeatAtMs`. First-entry refusals eject with TaskLeaveVehicle flag 16 (teleport out) plus a chat line per code.
3. Puppet seating retry every 100 ms (not 500) for cars within 15 m of the local ped or equal to lpVeh; log "seat target has no ped".
4. F/G entry in a new pure `libs/sync/vehicle_entry.{h,cpp}`, driven by a new `TickVehicleEntry` before TickLocalVehicle: on foot with a networked car within 8 m, DISABLE_CONTROL_ACTION(0,23,1) and (0,47,1) every frame (HoldMenuControls pattern), read IS_DISABLED_CONTROL_JUST_PRESSED(0,23) = F and (0,47) = G (control ids INFERRED). Safety net: GetVehiclePedIsTryingToEnter(lp) on a networked car -> CLEAR_PED_TASKS_IMMEDIATELY and treat as F. Ambient/local cars keep GTA's default entry. Control 75 untouched.
5. `PickEntry(lpPos, cands{netId, seats[8], doorPos[8], maxSeats, locked, ownerId}, me, wantDriver) -> {netId, seat, reason}`: F = seat 0 if free AND not owned by someone else AND not locked for a non-owner, else nearest free seat >= 1 by door distance; G = nearest free seat >= 1. Refusals Full / Locked / Owned. Door positions from bones seat_dside_f, seat_pside_f, seat_dside_r, seat_pside_r, seat_dside_r1... (names INFERRED) via GET_ENTITY_BONE_INDEX_BY_NAME + GET_WORLD_POSITION_OF_ENTITY_BONE (in hook.cpp); fallback quaternion-rotated offsets. maxSeats = GetVehicleMaxPassengers + 1.
6. Execution: door > 1.5 m (and <= 5 m) -> TaskGoStraightToCoord(lp, door, 2.0, 3000) re-picked each frame; <= 1.6 m -> SetPedIntoVehicle (no door anim, no jack); the link then sends Enter. Never TASK_ENTER_VEHICLE (jack flags unverified).
7. Server ownership: `bool owned` on Vehicle, true for Spawn only (Register, server-spawned, orphaned = false). Enter with `seat==0 && owned && spawner!=player` -> new `VehicleResultCode::NotOwner=12`, checked before the shuffle skip and SeatTaken. New `VehicleOp::Release=9` (owner only) sets owned=false. VehicleState gains `ownerId` (u16, kNoPlayer = unowned). Owner disconnect: empty cars destroyed, occupied become orphans with owned=false; timers unchanged (vehicleOrphanSeconds 60, vehicleEmptyDespawnSeconds 600). Client: Ejects(12)=true, "That vehicle belongs to someone else."

Tests (fail today):
- server_test.cpp vehicle store: p0 Spawn; p1 Enter seat 1 Ok; p1 Enter seat 0 -> 12 (today shuffle Ok); p1 Enter seat 0 from foot -> 12 (today Ok); p0 Enter seat 0 Ok; after Release p1 Ok; after PlayerGone(p0) the orphan is not owned.
- gameplay_test.cpp TestLink: Linked seat 1, Tick seat 0, OnResult(3,2,9), Tick -> `!eject && revertSeat==1 && Linked() && Seat()==1` (today eject).
- gameplay_test.cpp TestEntry (new): driver taken -> nearest passenger seat, never 0; owned by other -> F and G pick >= 1; empty car F -> seat 0 even if passenger door nearer; locked -> refusal; full -> refusal.

## B. Remote vehicle stutter

Root cause: snapshots stamped `tick*tickMs` (engine.cpp:950); the server copies the driver's newest update (sent every 33 ms, engine.cpp:4000-4042) at receive time, keeps only posMs (vehicles.h:326-338) and emits it at the tick (:539): stamp error 0-33 ms + jitter (INFERRED ~0.8 m at 25 m/s). A repeated sample gets a later stamp -> stall then jump (zero chord clamps the Hermite tangent, vehicle_drive.cpp:297-306). Players have WorldEntry.ageMs (protocol.h:304); vehicles have nothing. The fixed 120 ms delay (engine.cpp:195) is shorter than the 100/200 ms mid/far spacing (vehicles.h:68) -> extrapolation.

Fix: `VehicleState.ageMs` (u8) = min(255, nowMs - posMs), BuildFrame gets nowMs; client stamps `tick*tickMs - ageMs` via pure `sync::SnapshotStampMs` (duplicates become Stale). Per-vehicle delay = max(120, 1.5 x EWMA snapshot spacing + 30) ms.

Tests: server_test Update at 1000, BuildFrame at 1030 -> ageMs == 30. vehicle_drive_test: 30 Hz sender relayed at 20 Hz on Circle(): error < 5 cm with aged stamps, > 20 cm with tick stamps.

## C. One controlling client per vehicle

Root cause: server accepts VehicleUpdate only from seat 0 (vehicles.h:318); `driven` needs a seat-0 occupant (:493); Leave zeroes velocities (:616-620). Client: `iDrive` is seat-based (engine.cpp:3164), the drive controller holds every other car at the server state (:3176-3212), local body/engine health overwritten from the server (:3001-3010). Empty cars cannot be damaged or pushed.

Fix: `ctrl` (u16, kNoPlayer = none) on Vehicle and VehicleState. `VehicleStore::AssignControllers(nowMs, tick, players{id,pos,hasState,trusted})` from BroadcastVehicles. Priority: seat-0 driver; else sticky current controller (initially the last driver) while connected and within 250 m (hysteresis vs 200 m); else nearest trusted player with RelevantTo; else none. Watchdog: no accepted update 1500 ms after assignment while driverless -> skip that player 10 s, reassign. ctrl none -> zero velocities (Leave no longer zeroes them while a controller exists). Update accepted from `ctrl` instead of seat 0. BuildFrame `driven = (seat0 occupied || MovedRecently) && ctrl != receiver`. Client: `iControl = st.ctrl==me` replaces iDrive in ApplyVehicleFields and the drive skip; SendLocalVehicle -> SendControlledVehicles for every spawned car with ctrl==me: 5 Hz still, immediate on change, full rate moving. On a ctrl change: drive.Reset() + interp.Clear(); the new controller starts from its current pose (no snap). Old controller's in-flight update refused NotAllowed. Visual deformation not synced (out of scope).

Tests: server_test: p0 drives and exits, p1 nearer -> after AssignControllers p1's Update Ok, p0's NotAllowed (today p1 NotAllowed); controller disconnects -> next nearest.

## D. Steering

Root cause: engine ignores the drive controller's steering output (engine.cpp:3207; vehicle_drive.cpp:371 emits it); sender never sets `u.steer` (engine.cpp:3396-3430); steer is on the wire as i8 (protocol.cpp:104,118). hook.h: SetVehicleSteerBias is -1 right..+1 left, opposite to the wire: pass `-steer`.

Fix: sender pure `sync::EstimateSteer(vFwd, yawRateBody, wheelbase, lock=0.6)` = atan2(L*w, v)/lock clamped +-1, hold below 1 m/s; inputs already read (rotation velocity, velocity, quaternion); wheelbase from wheel_lf / wheel_lr bones cached per model; lock 0.6 INFERRED. Receiver: SetVehicleSteerBias(h, -o.steering) after the drive update, every frame (whether it turns a puppet's wheels visibly: INFERRED, verify live). Memory getter (RAGECOOP-style offset) later only: no handle-to-pointer helper in hook.cpp yet. Standstill wheel turn not covered.

Test: vehicle_drive_test: kinematic bicycle at 15 m/s with known delta -> EstimateSteer within 0.1, left negative, holds below 1 m/s.

## E. Lights, doors, roof

Root cause: only kVehLights (a server command) replicates via SET_VEHICLE_LIGHTS (engine.cpp:2979); the driver's real headlights are never read; only engine, siren, horn are sent (engine.cpp:3418-3420).

Fix: `flags2` (u8, controller-owned): headlights actual, high beam, interior light, indicator L, indicator R, roof down (bits 6-7 reserved). `doors` (u8): open FL, FR, RL, RR, hood, trunk. Natives present: SET_VEHICLE_LIGHTS, SET_VEHICLE_DOOR_SHUT. Hash needed (3889 crossmap): GET_VEHICLE_LIGHTS_STATE, SET_VEHICLE_FULLBEAM, SET_VEHICLE_INTERIORLIGHT, SET_VEHICLE_INDICATOR_LIGHTS, GET_VEHICLE_DOOR_ANGLE_RATIO, SET_VEHICLE_DOOR_OPEN, GET_CONVERTIBLE_ROOF_STATE, RAISE_CONVERTIBLE_ROOF, LOWER_CONVERTIBLE_ROOF. Indicators have no getter: script-owned SetFlag-style command (INFERRED). Radio skipped. Lights shown = server override OR driver's actual state, else forced off. Adding a native: hook.h constant, hook.cpp kCrossmap3889 row, GAGE_NEW_NATIVE entry (~hook.cpp:5200), World wrapper.

Tests: netcode_test round trip of flags2 + doors in VehicleWorld and VehicleUpdate; server_test Update copies flags2/doors from the controller only.

## F. Drive-by weapon

Root cause: the weapon hash arrives and ShowPeerWeapon calls SET_CURRENT_PED_WEAPON (engine.cpp:2135-2149), but a seated puppet returns early (engine.cpp:2503-2507): no aim task, so GTA holds no weapon object; only zero-damage bullets (:2274). No drive-by native in hook.cpp.

Fix: seated branch, kPfAiming + gun -> re-issue every 500 ms like on foot (engine.cpp:2599): first try existing TaskAimGunAtCoord(ped, pose+aim, -1) (drive-by pose INFERRED); else hash needed TASK_DRIVE_BY + SET_DRIVEBY_TASK_TARGET (or TASK_VEHICLE_AIM_AT_COORD for turrets). Clear with blending ClearPedTasks, never IMMEDIATELY on a seated ped. Pure `DecideVehicleAim(seated, flags, isGun, taskActive, ageMs) -> {None, Issue, Clear}`.

Test: gameplay_test: seated+aiming+gun -> Issue; not aiming with task active -> Clear; unarmed -> None; re-issue after 500 ms.

## v5 wire (vehicle part)

| Message | Change |
|---|---|
| VehicleWorld full entry | + ageMs u8, ctrl u16, ownerId u16, flags2 u8, doors u8 (+7 B: ~43 + 2/occupant; ~25 entries per 1200 B packet instead of ~29) |
| VehicleUpdate | + flags2 u8, doors u8 (33 -> 35 B) |
| VehicleResultCode | NotOwner = 12 (kVehicleResultLast = 12) |
| VehicleOp | Release = 9 (kVehicleOpLast = 9) |

Docs: docs/api/protocol.md section 14.

## Sync D: offline audit of remote vehicle motion and occupants (2026-11-03)

Method: `tests/vehsim.h` runs the real `VehicleInterpolator`, `VehicleDriveController`, `PresentLead`, `NetClock` and `ClassDelay` behind a simulated relay (30 Hz sender, 30/15/10 Hz tiers, 20-80 ms jitter, 3-5 % loss, the wire quantisation of `protocol/quant.h`) against a simplified physical car (tyre grip that turns a hard corner into a drift, drag, ramp and ballistic flight, gravity, ground). It is NOT the game: the puppet obeys SET_ENTITY_VELOCITY / ANGULAR_VELOCITY at once and the physics are guesses. Numbers below are simulation, not live.

What the simulation found fine: straight, lane change, hard brake, brake in a bend, reverse, hairpin, jump, parked, crawl (0.15..3 m/s), at 20..240 fps and all three tiers stay within about 0.1-0.4 m / 6-15 degrees of the instant the sample claims, one snap (the spawn), no pops. A 500 km/h car is limited by timing: 1 ms of render-clock noise is 14 cm (the 10 Hz tier's delay change costs up to 2.5 m; that is the shared clock's, not the controller's).

What it found wrong, and the change:
1. Buffering pinned the car at its oldest snapshot while the snapshot's velocity said "moving": the target stood still for a whole render delay and then moved on. After any hand-over, first sight, teleport or bridge reset (a hole over 500 ms) the car popped 2.5 m ahead at 25 m/s (14 m at 139 m/s) and was snapped there. `VehicleInterpolator::Sample` now walks the oldest snapshot BACK along its velocity / angular velocity (`VehicleInterpConfig::bufferBackMaxMs`, 300 ms; 0 restores the pin): 0.11 m / 0.59 m.
2. A controller hand-over emptied the snapshot buffer and snapped the drive controller even between two OTHER controllers (the driver left a moving car, someone else is nearest). `sync::DecideHandover` (vehicle_syncd.h): only a hand-over to or from US clears and snaps; otherwise the buffer and the controller stay.
3. Door walk (INFERRED, not provable offline): the sender's F / G entry is a run to the door and a warp into the seat; the W6 puppet walked to the door with its own TASK_ENTER_VEHICLE (blend 1.0, own path) and was warped from wherever that had got to. The default is now to keep following the sender's pose and warp when the seat list names the player (`puppet_enter=task` in HKCU restores the W6 walk, read at Welcome).
4. `LocalVehicleLink` (occupant fuzz, 20000 random legal sequences: 641 ended with a ghost or a lost seat, now 0): (a) leaving while a SEAT CHANGE was unanswered left the old seat on the server if the change was refused (the ghost occupant: every other client warped the puppet back into the car); (b) leaving and re-entering the same car within a round trip: the first Enter's late Ok owed an Exit for the whole vehicle and removed the new seating. The Exit for an unanswered Enter is now owed at the moment we leave; the late answer owes nothing.

Diagnostics added (10 s, `engine: vehicle drive (D)` and `engine: occupants (D)`): position / heading / sideways error mean and max against the sender's sample at the instant it claims, drive frames by interpolator mode (interpolating / extrapolating / holding / buffering), snaps, persistent and under-floor snaps, blends, graces; door walks followed vs tasked, a warped puppet found out of its seat (another seat / another car / outside / unknown), exits by flag (0 / 4160 / the flag-16 fallback / pops), controller hand-overs kept vs snapped.

Not changed, reported: the present-time lead (W9) trades a constant 110 ms lag for a prediction error that peaks at the onset of a manoeuvre (hairpin: up to 1 m / 17 degrees, versus 0.2 m / 6 degrees without it, but then about 1.3 m behind the sender at 12 m/s).
