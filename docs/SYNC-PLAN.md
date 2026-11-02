# Sync overhaul plan (protocol v5), 2026-11-02

Why: the first two-player live test on SOL (2026-11-02, client 0.4.1, protocol 4) showed laggy and stuttering cars and peds, no remote animations, a passenger "thrown out" on one screen and seated on the other, no hit reactions, ~2-hit kills, missing tracers, wheels that never steer, no weapon in a drive-by, empty cars that take no damage, unsynced headlights.

Specs (each lists root causes with file:line, fixes and tests that fail today):
- [sync-specs/onfoot.md](sync-specs/onfoot.md): gait, task churn, Reset snaps, velocity noise, sample time, missing anim states.
- [sync-specs/vehicles.md](sync-specs/vehicles.md): seats / F entry / ownership, stutter, controller per car, steering, lights / doors, drive-by.
- [sync-specs/combat.md](sync-specs/combat.md): time-to-kill, hit reactions, shots, melee, kill feed.
- [sync-specs/network.md](sync-specs/network.md): vehicle timeline, sender-time stamps, rates, idle dead-band, one render clock, stalls, counters.
- Research input: [SYNC-RESEARCH.md](SYNC-RESEARCH.md).

Owner rules folded in: F never jacks a player (nearest free passenger seat); an OWNED car is driven only by its owner; Alt+F4 asks without pausing (done); cheat-code box disabled (done).

## Decisions where the specs overlap

- **One sample clock.** onfoot 5b `sampleclock.h` and network N2 `SenderClock` are the same thing: one pure `libs/sync/sampleclock.h`, used by the server for peds and vehicles.
- **Vehicle timing**: network N1 + N5 supersede vehicles B's per-vehicle delay. Keep `ageMs` on VehicleWorld; one render clock (`libs/sync/netclock`) for peds and cars.
- **tickHz 30** becomes the default (network N3) only after the interest_test rate test and the sender-pipeline test pass at 30; 60 Hz is rejected (~14 ms gain for 2x cost).
- **VehicleUpdate** gets both vehicles E (flags2, doors) and network N2 (sampleMs): 33 -> 37 B.
- Melee needs a swing server-side (combat 4): only safe because client and server ship v5 together.

## Protocol v5 (one bump, kVersion 4 -> 5)

| Message | Change | Size |
|---|---|---|
| Welcome | + u8 playerHealth | +1 B |
| PlayerState | + u16 sampleMs | 37 -> 39 B (aiming 43 -> 45) |
| PedState flags | kPfSwimming 1<<13, kPfMelee 1<<14, bit 15 reserved (kPfKnown = (1<<15)-1) | 0 B |
| PedState move | 5 entering, 6 exiting + optional u8 vehId under ext bit kExtVeh = 64 | +1 B only when 5/6 |
| VehicleUpdate | + u16 sampleMs, + u8 flags2, + u8 doors | 33 -> 37 B |
| VehicleWorld full entry | + u8 ageMs, u16 ctrl, u16 ownerId, u8 flags2, u8 doors | +7 B |
| VehicleWorld motion-only entry (hdr 2) | id, hdr, ageMs, 22 B motion, flags; statics only on change, enter and 1/s | 26 B (40 B today) |
| VehicleResultCode | NotOwner = 12 | |
| VehicleOp | Release = 9 (owner only) | |
| Damage | + i16 bone, + u8 flags (headshot, melee, explosion, armour absorbed); also sent to relevant onlookers | 14 -> 17 B |
| Killed | + u8 flags (headshot, melee) | 9 -> 10 B |

Server and client deploy together; the server refuses v4 with the existing version-mismatch path. Update docs/api/protocol.md (sections for each message and section 14).

## Natives (all sourced, none guessed)

Every native the specs marked "hash needed" now has a 3889 hash from the same two sources as the existing kCrossmap3889 rows: YimMenu crossmap.hpp at the natives.hpp index (GTA_VERSION_TARGET 1.73-3889.0), agreeing with YimMenuV2 `enhanced` g_Crossmap by name. Public hashes and signatures from alloc8or natives.json. Not live-checked: `logNewNativeResolution` reports each at first use, like the previous batches.

Already in hook.h and the crossmap (no work): GET_VEHICLE_PED_IS_TRYING_TO_ENTER, TASK_ENTER_VEHICLE, TASK_LEAVE_VEHICLE, IS_DISABLED_CONTROL_JUST_PRESSED, SET_VEHICLE_STEER_BIAS, SET_ENTITY_MAX_HEALTH, TASK_PLAY_ANIM, DOES_ANIM_DICT_EXIST, REQUEST_ANIM_DICT, HAS_ANIM_DICT_LOADED, SET_PED_SHOOTS_AT_COORD, SET_PED_INFINITE_AMMO_CLIP, GET_ENTITY_BONE_INDEX_BY_NAME, GET_WORLD_POSITION_OF_ENTITY_BONE, GET_PED_LAST_DAMAGE_BONE, SET_VEHICLE_LIGHTS, SET_VEHICLE_DOOR_SHUT, SHOOT_SINGLE_BULLET_BETWEEN_COORDS.

To add (hook.h constant = public hash, kCrossmap3889 row, GAGE_NEW_NATIVE entry, World wrapper):

| Native | Namespace | Public hash | 3889 hash | Returns / params |
|---|---|---|---|---|
| IS_PED_RELOADING | PED | 0x24B100C68C645951 | 0xC722DDBD6C3E86D7 | BOOL (Ped ped) |
| TASK_RELOAD_WEAPON | TASK | 0x62D2916F56B9CD2D | 0x550B03C7904C63CD | void (Ped ped, BOOL drawWeapon) |
| GET_PED_STEALTH_MOVEMENT | PED | 0x7C2AC9CA66575FBF | 0xC2BF1F6F84E31EB2 | BOOL (Ped ped) |
| SET_PED_STEALTH_MOVEMENT | PED | 0x88CBB5CEB96B7BD2 | 0xF9358C41CC69C616 | void (Ped ped, BOOL p1, const char* action) |
| IS_PED_SWIMMING | PED | 0x9DE327631295B4C2 | 0x2CFBD7757B4D922F | BOOL (Ped ped) |
| IS_PED_IN_COVER | PED | 0x60DFD0691A170B88 | 0xD6179D448A06A77F | BOOL (Ped ped, BOOL exceptUseWeapon) |
| TASK_PUT_PED_DIRECTLY_INTO_COVER | TASK | 0x4172393E6BE1FECE | 0x994B4C04761C0AA0 | void (Ped, float x, float y, float z, int time, BOOL allowPeekingAndFiring, float blendInDuration, BOOL forceInitialFacingDirection, BOOL forceFaceLeft, int identifier, BOOL doEntry) |
| IS_PED_PERFORMING_MELEE_ACTION | PED | 0xDCCA191DF9980FD7 | 0xB73833BDAAE31047 | BOOL (Ped ped) |
| SET_PED_MOVE_RATE_OVERRIDE | PED | 0x085BF80FA50A39D1 | 0xB27B08E34AC92345 | void (Ped ped, float value) |
| GET_VEHICLE_LIGHTS_STATE | VEHICLE | 0xB91B4C20085BD12F | 0x9FFEA38DBCE391EC | BOOL (Vehicle, BOOL* lightsOn, BOOL* highbeamsOn) |
| SET_VEHICLE_FULLBEAM | VEHICLE | 0x8B7FD87F0DDB421E | 0x2F12C305B28C6C59 | void (Vehicle, BOOL toggle) |
| SET_VEHICLE_INTERIORLIGHT | VEHICLE | 0xBC2042F090AF6AD3 | 0xEC6A9F16D3F15978 | void (Vehicle, BOOL toggle) |
| SET_VEHICLE_INDICATOR_LIGHTS | VEHICLE | 0xB5D45264751B7DF0 | 0x78004905636369DF | void (Vehicle, int turnSignal, BOOL toggle) |
| GET_VEHICLE_DOOR_ANGLE_RATIO | VEHICLE | 0xFE3F9C29F7B32BD5 | 0x7BFB76C576628F3D | float (Vehicle, int doorId) |
| SET_VEHICLE_DOOR_OPEN | VEHICLE | 0x7C65DAC73C35C862 | 0xBFE60A5CC0C835D8 | void (Vehicle, int doorId, BOOL loose, BOOL openInstantly) |
| GET_CONVERTIBLE_ROOF_STATE | VEHICLE | 0xF8C397922FC03F41 | 0x54DA32C15F7A6ABA | int (Vehicle) |
| RAISE_CONVERTIBLE_ROOF | VEHICLE | 0x8F5FB35D7E88FC70 | 0xD6B15DF382A594C7 | void (Vehicle, BOOL instantlyRaise) |
| LOWER_CONVERTIBLE_ROOF | VEHICLE | 0xDED51F703D0FA83D | 0x232B023FE4D977E2 | void (Vehicle, BOOL instantlyLower) |
| TASK_DRIVE_BY | TASK | 0x2F8AF0E82773A171 | 0xD83588CA24829967 | void (Ped driverPed, Ped targetPed, Vehicle targetVehicle, float x, float y, float z, float distanceToShoot, int pedAccuracy, BOOL pushUnderneathDrivingTaskIfDriving, Hash firingPattern) |
| SET_DRIVEBY_TASK_TARGET | TASK | 0xE5B302114D8162EE | 0xF38F1E299109383A | void (Ped shootingPed, Ped targetPed, Vehicle targetVehicle, float x, float y, float z) |
| TASK_VEHICLE_AIM_AT_COORD | TASK | 0x447C1E9EF844BC0F | 0xE61AF8D27706A774 | void (Ped, float x, float y, float z) |
| CLEAR_DRIVEBY_TASK_UNDERNEATH_DRIVING_TASK | TASK | 0xC35B5CDB2824CF69 | 0x827783BAD5CBE95D | void (Ped) |
| GET_IS_TASK_ACTIVE | TASK | 0xB0760331C7AA4155 | 0x12623527E5B8AB7C | BOOL (Ped, int taskType) |
| START_PARTICLE_FX_NON_LOOPED_ON_ENTITY | GRAPHICS | 0x0D53A3B8DA0809D2 | 0xDD19F75405614D3C | BOOL (const char* effectName, Entity, float offX, offY, offZ, rotX, rotY, rotZ, float scale, BOOL axisX, axisY, axisZ) |
| USE_PARTICLE_FX_ASSET | GRAPHICS | 0x6C38AF3693A69A91 | 0xD03F4780B97A39AE | void (const char* name) |
| REQUEST_NAMED_PTFX_ASSET | STREAMING | 0xB80D8756B4668AB6 | 0xEBEE7DC21AB44EC9 | void (const char* fxName) |
| HAS_NAMED_PTFX_ASSET_LOADED | STREAMING | 0x8702416E512EC454 | 0x939D49C9FAA8139A | BOOL (const char* fxName) |

Still INFERRED, not hashes: control ids 23/47 (enter), 140/141/142 (melee); ped config flag 184; seat bone names; anim dictionary and clip names (only names checked with DOES_ANIM_DICT_EXIST at runtime, failures logged); particle names. Each is logged at first use so one live session confirms or refutes it.

## Implementation order (one writer at a time; the tree is not a git repo)

Each step: a Sonnet agent, edits only its listed areas, adds the spec's failing-today tests first, builds with the WinLibs toolchain, runs the named selftests (never by glob), reports diff summary + test output. No game or launcher run.

| Step | Scope | Wire | Tests |
|---|---|---|---|
| W0 | 27 natives above: hook.h, kCrossmap3889, GAGE_NEW_NATIVE, World wrappers | no | gage_mp_test, build |
| W1 | On-foot 1-4 and 7 (libs/sync locomotion + engine wiring), Tier A anim states, Poll(2) | no | locomotion_test, sync_test, gameplay_test |
| W2 | Vehicles client: A1-A6 (shuffle flag, revert-not-eject, 100 ms puppet seating, vehicle_entry F/G), D steering, F drive-by | no | gameplay_test, vehicle_drive_test |
| W3 | Combat client: WeaponShowCache, ShotDebt, HitReact::Decide + arbiter + puppet grace, melee sender shotSeq + receiver swing | no | gameplay_test |
| W4 | Server/network without wire: N4 idle dead-band, N6a lock scope, N6b keep-alive ack, N6c wall-time tick, counters | no | interest_test, server_test, netcode_test |
| W5 | Protocol v5 codec + server: every row of the v5 table, sampleclock, ownership/NotOwner/Release, ctrl assignment, vehicle ageMs + motion-only, damage scaling (damage.h, playerHealth 200), melee NeedsShot, onlooker Damage | yes | quant_test, netcode_test, server_test, interest_test, join_test |
| W6 | Engine on v5: sampleMs send, iControl + SendControlledVehicles, flags2/doors read+apply, NotOwner chat, playerHealth max health, Damage bone/flags -> hit reactions, kill feed labels, entering/exiting moves, Tier B flags | yes | gage_mp_test, gameplay_test, join_test |
| W7 | One render clock (netclock) for peds + vehicles, tickHz 30 + rates 30/15/10, budget 36000 | no (server config) | sync_test, interest_test, vehicle_drive_test, netcode_test |

Then: kClientVersion bump, installer with a new versioned name, server redeploy with v5, live test by GI.

## Targets (asserted in tests where a harness exists)

Peds: p95 placement error <= 2 ms; speed ripple <= 3%; p95 position error <= 0.10 m; frozen <= 100 ms (150 at 10% loss, 300 at 30%); loco p95 distance <= 0.5 m; 0 persistent snaps per 60 s; GoStraight <= 0.4/s straight. Vehicles: aged stamps error < 5 cm on the circle; Repeat for identical motion. Clock: ped vs vehicle renderT within 5 ms. Rates at 30 Hz: 148..150 / 73..77 / 48..52 entries per 5 s near/mid/far. TTK at defaults: carbine 9 hits, fists 14.

## Live checks for GI after the build

Passenger seat stays seated on both screens; F beside an occupied car seats you as passenger; a non-owner cannot take the driver seat of an owned car; wheels turn on a remote car; headlights and doors match; drive-by shows the weapon; remote walk/run/jump/climb/swim animate; hits flinch and knock down; carbine needs ~9 body hits; tracers on every shot; empty cars can be pushed and damaged.
