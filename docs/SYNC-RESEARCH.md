# Open-source GTA V coop sync: how others solved it (research, 2026-11-02)

Input to `docs/SYNC-PLAN.md`. Read from GitHub by a research agent (WebFetch; nothing downloaded or run). Line numbers are approximate (the fetch tool strips line structure): re-open the cited methods before porting. Offsets and byte patterns in these projects are for older Legacy builds: re-scan on 3889, never reuse.

## Licences (from each repo's own LICENSE)

| Project | Licence | State |
|---|---|---|
| RAGECOOP/RAGECOOP-V | MIT (c) 2022 RAGECOOP | last push 2026-02, release 1.5.4.7 (2023); ScriptHookV + SHVDN, Legacy |
| GTANetworkDev/platform (GTAN) | MIT (c) 2017 GTA Network | deprecated, code ~2017-18, Legacy |
| Guad/GTACoop | GPL-3.0 | old; not read |
| cakeMP/cakeMP | GPL-3.0 | archived 2022 (C++ ASI + ENet) |
| ScriptHookVDotNet | zlib | paraphrase only |
| RAGE-MP, alt:V, GTA:MP | closed | nothing usable |
| FiveM | Take-Two CPL | docs only, never code |

MIT code may be ported with attribution (keep the copyright line in the file that carries it). GPL code is not copied into our tree.

## 1. Vehicle steering and interpolation

- **Steering (RAGECOOP):** sender reads the steering angle; receiver writes it only when it changed, as a raw float write at a pattern-scanned offset in CVehicle (`Util/Util.cs` CustomSteeringAngle; pattern `74 0A F3 0F 11 B3 1C 09 00 00 EB 25`, offset = `*(int*)(match+6) + 8`). Throttle, brake and brake lights applied too. GTAN lerps `SteeringAngle` toward the sent value scaled by latency.
- **Wheel spin:** nobody syncs it; wheels roll from ground contact because the body gets real velocity.
- **Interpolation:** no snapshot buffer in either. Dead reckoning + physics correction: predicted = pos + vel * (travel time + time since packet); error < 0.03 m ignore, < 10 m add to velocity, > 10 m snap. Rotation: slerp 0.5 when flipped / large error, else angular velocity + 0.2 * error. GTAN: latency-scaled velocity force `1.10 + sqrt(lat/2500) + speed/250`.
- **Rate:** RAGECOOP 30 ms per entity, unreliable sequenced, server distance filter.
- **Unoccupied vehicles (GTAN):** server assigns a syncer client within ~100 units, releases beyond 130; sent every 400 ms only when moved.

## 2. Drive-by

- RAGECOOP: aim sent only while aiming; receiver `TASK_DRIVE_BY(ped,0,0,x,y,z,1,100,1,SingleShot)` once, `SET_DRIVEBY_TASK_TARGET` every tick, clear on stop; turrets `TASK_VEHICLE_AIM_AT_COORD`; weapon shown via `CREATE_WEAPON_OBJECT` / `GIVE_WEAPON_OBJECT_TO_PED`.
- GTAN: same tasks, infinite clip, fire with `SET_PED_SHOOTS_AT_COORD` or `SHOOT_SINGLE_BULLET_BETWEEN_COORDS` from the hand bone; `CLEAR_DRIVEBY_TASK_UNDERNEATH_DRIVING_TASK` 200 ms after the last shot; sender detects drive-by from ped subtasks 200/190.

## 3. Enter / exit / authority

- RAGECOOP ped state: 4 seated, 5 entering (plays `Task.EnterVehicle`), 6 exiting. Re-seat with SetIntoVehicle only when in the wrong vehicle/seat AND not being jacked (`IsBeingJacked` guard avoids fighting the jack anim).
- **Authority = whoever sits in the driver seat.** The new driver claims ownership (reliable `OwnerChanged`); receivers drop packets for a vehicle they own; passengers never claim.
- GTAN: interpolation runs only when the responsible ped is the remote driver; server picks a syncer for unoccupied vehicles (see 1).
- RedAge (our old RAGE-MP gamemode, reference only): custom F entry, nearest free seat by door bone, never jacks, warps in within 1-2 m of the door; server seat caps per vehicle class.

## 4. On-foot

- Tasks, not raw animations: walk `GoStraightTo`, run `RunTo`, sprint `TASK_GO_STRAIGHT_TO_COORD` + sprint multiplier, all with `SET_PED_DESIRED_MOVE_BLEND_RATIO`; idle `StandStill(2000)`.
- Position fix by velocity: `vel += 5 * dist * (predicted - pos)`, heading lerp 0.5; snap threshold GTAN 15 + 5 * speed.
- Jump `Task.Jump`, vault `Task.Climb`, reload `Task.ReloadWeapon`, cover `TASK_STAY_IN_COVER`, ladders `TASK_PLAY_ANIM("laddersbase")`.
- Ragdoll: RAGECOOP sends head + foot positions while ragdolling and applies 20x-error impulses to those bones; GTAN `SET_PED_TO_RAGDOLL(ped,50000,60000,0,1,1,1)`, get-up `ball_get_up`.

## 5. Shooting effects

- RAGECOOP: shooter sends weapon, start, end per bullet; receivers `SHOOT_SINGLE_BULLET_BETWEEN_COORDS` (via SHVDN World.ShootBullet, INFERRED); start clamped to 10 m from the impact; muzzle flash particle by weapon class (`muz_assault_rifle`, `muz_pistol_silencer`, ...) at a per-weapon muzzle bone.
- GTAN: `SET_PED_SHOOTS_AT_COORD(ped,end,true)` with `SET_PED_SHOOT_RATE 100`, so the engine plays its own flash and tracer.

## 6. Damage

- Both are victim-authoritative (INFERRED): the shot is replayed on the victim's machine, the victim's own game applies the hit, each owner streams its own health. Puppets are invincible; health copied from the owner.
- GTAN victim detects hits with `HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY` + `GET_PED_LAST_DAMAGE_BONE`; melee by capsule raycast + `ApplyDamage(25)`.
- RAGECOOP disables hurt/explosion reactions on puppets (no flinch). Neither sets player damage modifiers: GTA Online-style TTK must come from our own server config.

## 7. Animations

- GTAN only: owner sends dict, name, flag; receiver `TASK_PLAY_ANIM(..., 8, 10, -1, flag, -8, ...)`; flag 32 = upper body (locomotion keeps running), 1 = loop; stop at 95% progress. Scenarios `TASK_START_SCENARIO_IN_PLACE`.
- RAGECOOP: cover idle/blind-fire, reload, ladders, facial anims only.

Sources: github.com/RAGECOOP/RAGECOOP-V, github.com/GTANetworkDev/platform, github.com/Guad/GTACoop, github.com/cakeMP/cakeMP, github.com/crosire/scripthookvdotnet.
