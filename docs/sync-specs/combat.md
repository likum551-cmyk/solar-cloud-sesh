# Combat sync spec, 2026-11-02

Design agent output (read-only; line numbers as of 2026-11-02). INFERRED = verify. External numbers from secondary sources (GTAForums game-files threads, GTAMods wiki). Model stays: shooter-reported hits, server-validated, server owns health/armour; reactions are cosmetic and never feed Hit.

## 0. Protocol v5 (combat part)

- Welcome + u8 playerHealth (the effective maximum; today the client assumes Reset(100,0)).
- Damage 14 -> 17 B: + i16 bone, + u8 flags (b0 headshot, b1 melee, b2 explosion, b3 armour absorbed). Today SendVitals goes only to victim and attacker (server.cpp:1299-1303); v5 also to every player with victim or attacker relevant (repl_.Relevant).
- Killed 9 -> 10 B: + u8 flags (b0 headshot, b1 melee).
- Hit unchanged (13 B). No new Msg id, no new PedState bit.

## 1. Time-to-kill and damage scaling

Root cause: playerHealth 100 (config.h:53) = GTA health 200, 100 above the death line (protocol.h:243); Online players have raw 238 (rank 1) to 328 (rank 100+). No damage multiplier: server.cpp:1552-1565 applies the client's number clamped to 250 and the class cap; hitDamageScalePercent scales only the caps (anticheat.h:195-202). The client number is the single-player loss of the puppet's 1000 pool (engine.cpp:2333, combat.cpp:286) without Online's freemode factor (~0.72, secondary source): carbine 4 hits, punch 30. Melee {maxHit 100, 300/s, hitsPerShot 0} (anticheat.h:101).

Defaults (new server.json keys; config.h/.cpp, server.json.example, docs/api/server.md):

| Key | Default | Note |
|---|---|---|
| playerHealth | 200 | raw 300, ~rank 80-100 Online; range 1..255 |
| firearmDamagePercent | 72 | Online freemode factor (secondary source) |
| meleeDamagePercent | 50 | INFERRED: fists 30 -> 15 |
| explosiveDamagePercent | 100 | |
| runOverDamagePercent | 100 | |
| headshotPercent | 300 | full at <= 5 m, linear to 100 at 40 m (sniper class: 300 m); game files say 18x (one-shot), 300% is a tuning choice |
| respawnArmour | 0 | Online vests 50 / 75; gamemodes grant it |

Resulting hits to kill at 200 HP, no armour: carbine 23/hit -> 9 (~1.2 s; today 4); pistol 19 -> 11; fists 15 -> 14 (today 4); carbine headshot at 3 m 69 -> 3.

Fix: header-only apps/server/damage.h `ScaleHit(cfg, eff, claimed, bone, distM)` called at server.cpp:1565 after the maxHit clamp and budget, before HurtPlayer (budget stays in raw client units). Headshot = bone 31086 (hook.h:652), once per shot slot (credit->hits == 0). HitBatcher (combat.cpp:223) keys head vs body. Client: SetEntityMaxHealth(lp, 100 + playerHealth) at entry (hooked, never called for the local ped) and g_vitals.Reset(playerHealth, 0). tests/netrig.h:84 Rig::BaseConfig() pins legacy values (health 100, all percents 100) so ~60 exact-number asserts keep passing.

Test (netcode_test.cpp, Config{} defaults): carbine Hit 32 until Damage.killed -> exactly 9 hits (today 4); fists with a swing, claimed 30 -> 14 punches (today 4); Config{}.playerHealth == 200.

## 2. Hit reactions

Root cause: the victim gets only a number (DrainEvents engine.cpp:3472-3491, SetEntityHealth 3636-3641; lastAttacker/lastWeapon only logged). Puppet bullets carry damage 0 (engine.cpp:2275), so GTA raises no damage event (INFERRED). On the shooter's screen the puppet has PCF 188 (DisableHurt), ragdoll off, non-temporary events blocked (hook.cpp:4125-4130), and DrivePeer re-issues gait + aim every 500 ms (engine.cpp:2599-2631). Third parties never get Damage.

Fix: pure `HitReact::Decide(damage, maxHp, flags, sinceLastMs, state) -> None | Flinch | Knockdown(ms)` in combat.h (INFERRED tunables): flinch at >= 3% max HP, >= 400 ms apart; knockdown at >= 20% or explosion / heavy melee, ragdoll ms = clamp(300 + 8*dmg, 0, 1500); 1.5 s immunity after a knockdown; never dead / in vehicle / climbing. Natives (all in hook.cpp): flinch = ShootBulletBetween damage 1, owner = attacker puppet, muzzle -> victim spine (GetEntityBoneIndexByName + GetWorldPositionOfEntityBone; real hit event, blood, impact INFERRED); knockdown = SetPedToRagdoll + SetEntityVelocity away from the attacker's render-time pose; anim fallback TaskPlayAnim flags 16|32 with dict names checked by AnimDictExists (names INFERRED, log which exist). Victim's own ped reacts at engine.cpp:3472 (DamageSlot combat.h:267 gains lastBone, lastFlags). Puppets: HitReactArbiter (like EmoteArbiter): 600 ms window where DrivePeer returns before gait/aim re-issue (like emoteHolds engine.cpp:2534), ragdoll allowed and PCF 188 cleared only then. VitalsTracker: a puppetDamage loss (combat.cpp:119-127) re-baselined forever today: schedule applyServer at + puppetGraceMs if no Damage arrived, so a cosmetic 1-HP bullet never becomes a self-reported death. Registry hit_react = off|bullet|ragdoll|both (like peer_hits, engine.cpp:729).

Test (gameplay_test.cpp): 25 at max 200 -> Flinch; 60 -> Knockdown 780 ms, a second hit within 1.5 s -> Flinch only; Tick(Obs(puppetDamage, health 199)) without Damage for 1.6 s -> apply && setHealth == 200 (today never).

## 3. Shots, tracers, muzzle flash, impacts

Root causes (which one hurts live is unknown): (1) IsGunWeapon caches false forever when IsWeaponValid / WeaponDamage are uncallable (engine.cpp:2190-2199); (2) shots capped at 3 per frame, the rest discarded (engine.cpp:2244, 2251), mid/far relevance delivers bursts; (3) without the aiming flag the bullet goes 50 m along the heading (engine.cpp:2267-2272); (4) script bullets are not weapon fires: no muzzle flash, shells, partial tracers (INFERRED). Already fixed 2026-11-02: smoother repeats dropped shot-counter-only changes.

Fix: (1) WeaponShowCache in combat.h caching only callable answers; (2) ShotDebt: carry up to 15 shots, fire 3 per frame; (4) registry peer_shoot = bullet|ped: `ped` = PedShootAtCoord (SET_PED_SHOOTS_AT_COORD, hook.cpp:760) + SetPedInfiniteAmmoClip so the weapon object really fires (a REAL shot: only when the line passes no local-player or peer capsule, else the damage-0 bullet); off by default until eyeballed live. Research alternative: RAGECOOP muzzle particle per weapon class at the muzzle bone (docs/SYNC-RESEARCH.md section 5).

Test: ShotDebt shotSeq jump of 9 -> 3,3,3 over three frames (today 3 total); WeaponShowCache uncallable then callable -> true.

## 4. Melee

Root cause: IS_PED_SHOOTING never fires for a punch (engine.cpp:3989): shotSeq does not move, receivers see no swing. Server melee needs only "weapon held within 4 m" (server.cpp:1506-1514), hitsPerShot 0: anyone within 4 m can claim 300 damage/s without swinging. Puppets hurt-proof (PCF 188).

Fix: sender IsDisabledControlJustPressed(0, 140/141/142) (light / heavy / alternate melee; wrapper exists) gated on a melee-class weapon and on foot -> ++g_shotSeq (control ids INFERRED). Receiver: melee weapon + shotSeq step -> swing via TaskPlayAnim flags 16|32 ~400 ms (clip names INFERRED, AnimDictExists). Server: Melee in NeedsShot (anticheat.h:205), Melee hitsPerShot 2, keep kMeleeRange 4.0. Damage via meleeDamagePercent + section 2 knockdown. Optional IS_PED_PERFORMING_MELEE_ACTION (hash needed).

Test (netcode_test.cpp): flip 3152-3164: fists Hit without Fire() refused; with a swing accepted once; a third Hit on the same swing refused (today the first is accepted).

## 5. Death, ragdoll, kill feed

Root cause: feed line has no weapon / headshot (engine.cpp:3501-3507) though Killed carries the weapon; killedAtMs stamped at receipt (engine.cpp:989) so the puppet dies ahead of its interpolated pose; UpdatePeerDeath (2282-2308) has no direction.

Fix: Killed + flags; feed "A killed B with Carbine Rifle (headshot)" from a WeaponLabel(hash) joaat table in libs/sync/gtaids.h; on a killed Damage apply the section 2 impulse before SetEntityHealth(0); delay the death display by the peer's pose delay.

Test: WeaponLabel(Joaat("WEAPON_CARBINERIFLE")) == "Carbine Rifle"; Encode(Killed).size() == 10; Damage round trip 17 B with bone + flags (today 9 / 14).

## Sequencing

Section 1, then section 0 protocol with 2 and 5, then 3, then 4. Update the gameplay_test.cpp:996-997 check-count sum and docs/api/protocol.md.

## T6 health (desync round 2, symptom 5: "health sticks at 50 instead of 100")

Trace (all units checked first-hand): server `Config::playerHealth` (hit points above GTA's death line; default 200) -> `Welcome.playerHealth` u8 (1..255, no clamp) -> client `g_playerHealth` -> ped max `PedMaxHealth` = 100 + it (300) via `ApplyServerMaxHealth` -> `VitalsTracker` (writes 100 + min(server health, max - 100)) -> `Damage.health` u8 / `damage` u16 back from the server -> HUD `HudHealthFraction(hp, max)` = (hp - 100) / (max - 100). Nothing in the wire or the HUD formula is wrong; the sticking value is a REAL loss on the server.

Root cause (reproduced through the real server, tests/t6_health_tests.h with -DT6_BASELINE on the base commit): a NEW local ped (the default-ped guard's SET_PLAYER_MODEL, TickEntryGuard) starts at GTA's default health 200 / max 200 and the guard raises the max to 300 in the same script tick, so the tracker's next tick sees "health 300 -> 200, max 300 unchanged": an unexplained loss of 100, reported as an environmental self-hit that the server accepts (a self-hit has no range or weapon rule). Server 200 -> 100, the ack puts GTA 200 on max 300 = 50 %, the server has no regeneration and the client's is off: the bar stays at 50 % until a console `heal` or a death. With playerHealth 100 (max 200 = the default) the same swap is harmless, which is why it started with the 2026-11-02 07:04Z switch to 200. 0e (b3dcf3e) fixed only an OBSERVED max change.

Fix (libs/sync/combat.*): `VitalsObs::ped` (a changed handle = a new ped: baseline and the server's value, never a loss) + `NotePedReplaced()` from the guard; an ABSOLUTE check (a living ped steady for 1 s off the server's value with nothing pending and no puppet in play is written again; 3 tries per mismatch, then the tracker stops fighting GTA and counts `reconcileGaveUp`); a failed `GET_ENTITY_MAX_HEALTH` read (0) no longer substitutes 200 (the last good max stands in, the HUD falls back to it); `ApplyMaxHealth` reads `SET_ENTITY_MAX_HEALTH` back and counts it; `OnWelcome` stores playerHealth before it raises the vitals reset.

Diagnostics (engine log, `gage.log`): `engine: health: Welcome.playerHealth N -> ...` at join; `engine: health: SET_ENTITY_MAX_HEALTH(ped, wish) [session start|ped guard swap|respawn] read back X` (WARN with "GTA did not keep the wish" when X != wish; the first 16 calls and any disagreement, 1 per 10 s); `engine: health: ped guard swap: new ped ... health H / max M`; `engine: health: self-hit of D with no fall and no puppet cause ...` (WARN, D >= 50, 1 per 2 s); `engine: health: SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER(0.0) -> ok|FAILED` once; and every 10 s `engine: health: ped H / max M, server health S of playerHealth P (ped max should be 100+P), bar B%; totals: max changes, gains undone, largest self-hit, ped swaps, reconciles (gave up), max reads unreadable, max writes (failed, read-back differs, last wish / read)`. Healthy: `ped 300 / max 300, server health 200 of playerHealth 200 ... bar 100%`, `largest self-hit` 0 or a real fall, `read-back differs 0`, `reconciles 0`. `reconciles` counting up with `gave up 1` and a bar stuck at 50 % means GTA itself refuses health above 200 on the player (the hypothesis the absolute check cannot fix): look at the `read back` and `ped health` figures.

## R3 T1 health (desync round 3: "health stuck at 50 %", measured live in 0.4.7)

Live (GI's `gage.log`, 0.4.7, playerHealth 200): `SET_ENTITY_MAX_HEALTH(ped 514, 300) [session start] read back 300; ped health 200`, then for 10+ minutes `ped 200 / max 300, server health 195 of playerHealth 200, bar 50%; reconciles 12 (gave up 4); max writes 1 (failed 0, read-back differs 0)`. So GTA clamps the PLAYER ped's HEALTH somewhere other than the entity maximum `SET_ENTITY_MAX_HEALTH` sets; the read-back of the maximum proved nothing about the health, and round 2's harness modelled GTA accepting health above 200 once the max was raised.

Design (libs/sync/combat.h `HealthScale`, `RunHealthProbe`, `ChooseHealthScale`, `HealthController`; wired in engine.cpp `ApplyServerMaxHealth`, `TickVitals`, `DrawHud`): the engine MEASURES what the ped holds and runs in one of two modes, whichever way GTA behaves.
- Probe (`HealthController::Begin`, at session start, after a ped swap, after a respawn, and when the tracker asks): `SET_ENTITY_MAX_HEALTH(100+ph)`, `SET_ENTITY_HEALTH(100+ph)`, read; `SET_PED_MAX_HEALTH(100+ph)`, `SET_ENTITY_HEALTH(100+ph)`, read. The health left after the last write minus 100 is the CEILING; the behaviour is `Holds` (the entity maximum is enough), `FollowsPedMax` (only the ped maximum lifts the cap) or `Capped` (stays below the wish whatever the maxima read). The tracker waits one tick, the next tick re-reads (a clamp a frame later is caught and the ceiling lowered), and a second read one second later is logged.
- DIRECT (ceiling >= playerHealth): the ped's health is 100 + the server's number, as in 0.4.7.
- SCALED (ceiling < playerHealth): the server's number stays the truth; the ped carries `100 + max(1, round(hp * ceiling / ph))` (never above the ceiling, never on GTA's death line for a living player, 100 + ceiling is its maximum); the HUD bar is the server's number minus the self-damage in flight; environmental damage in GTA units is scaled back by ph / ceiling before it is reported as a self-hit (armour is not scaled); a server-side death kills the ped (`kill`); a GTA-side lethal fall reports the whole server health. A mismatch the reconciler cannot fix (3 writes) asks for ONE new probe (3 per ped) so a clamp that appears later changes the mode instead of fighting it.
- Why not a lower `playerHealth` on the VPS: that is a server config change that also changes every player's time-to-kill and the damage scale for a limit of GTA's client; the SCALED mode keeps the server's 200 (or 255) as the truth and works for any server. Why not `SET_PED_MAX_HEALTH` alone: it is part of the probe sequence and is enough if it is the thing that lifts the cap (`FollowsPedMax`), but the live log shows a cap the maximum natives did not move; nothing guaranteed a second native would; the probe reads the real health so it needs no such guarantee.
- Registry (`HKCU\Software\GAGE-MP`, read at Welcome): `health_mode` = `direct` (the 0.4.7 behaviour: no scaling, the roll-back), `scaled` (always scaled; an unmeasurable ceiling is taken as 100), anything else / unset = `auto`.

Diagnostics (`gage.log`): `engine: health: probe [session start|ped guard swap|respawn|ped handle not probed|reconciliation gave up] ped N, playerHealth P -> wish max W, health_mode=M: start: hp H max X; SET_ENTITY_MAX_HEALTH(W) ok -> hp H max X; SET_ENTITY_HEALTH(W) ok -> ...; SET_PED_MAX_HEALTH(W) ok -> ...; SET_ENTITY_HEALTH(W) ok -> ...; => <behaviour>; ceiling C; mode SCALED|DIRECT`, then `probe +1 frame: ...` and `probe +1 s: ...`; every 10 s `engine: health: mode SCALED|DIRECT (health_mode=...), measured ceiling C (behaviour), ped H / max M (GTA wants H' / M'), server health S of playerHealth P, bar B%; read-backs: probe hp .. max ..; probes N (+1 frame took back N, +1 s differed N); totals: ... reconciles N (gave up N, re-probes N) ...`. Tests: `tests/t6_health_tests.h` (SimPed with five switchable GTA models, every scenario under each) and `tests/r3_t1_health_tests.h`.

## R3 T6 melee (desync round 3: "punch animations aint there and hits do no damage")

Live (0.4.7, GI's log, evidence level "measured live"): about 25 swings counted, 4 Hits sent (self-hits included), 0 melee hits by the damage event, 1 by the pool
drop, "Damage confirmed by the server on our Hits 0 (0 damage)" on every line; P2's swings on GI's screen: "peer swings shown 2, 1, 4" and no animation (that counter
counted TASK_PLAY_ANIM calls). Server log not available.

Where a melee Hit can die (from the code; none proven live): (1) client: the swing is counted (shotSeq step) but the hit is detected only by HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY on
the puppet (PED_TYPE_MISSION, PCF 188 DisableHurt, no ragdoll, pool 1000) inside a 900 ms window, or by its pool dropping; neither fired for ~25 swings, and nothing in the log
says how many swings had a puppet within reach (`meleeNearSwings` was not in the excerpt); (2) HitBatcher 50 ms hold, SendHit needs Joined; (3) server: Rate, shooter dead / no
state, zero damage, target missing / no state / dead, untrusted shooter, range, relevance, a swing within 1500 ms with that shotSeq (`no_shot`), weapon held 500 ms
(`weapon_not_held`), 2 Hits per swing (`shot_used`), 4 m reach (`melee_reach`), melee budget (`over_budget`), 4 deferred Hits (`defer_full`); a deferred Hit that died with its
shooter was uncounted (now `defer_dropped`). After ScaleHit a fist is 40 x 50 % = 20 at the shipped defaults. Not provable offline which of these ate the 4 Hits.

Fix: the server judges the swing itself (apps/server/melee.h, docs/api/server.md "Melee judgement"), no wire change; client-reported Hits stay as confirmations. Animation: the swing
is read back (IS_ENTITY_PLAYING_ANIM; IS_PED_IN_MELEE_COMBAT for the melee-task rung) and counted as SEEN, with the reason when not (a locomotion task issued meanwhile, a busy ped,
nothing explains it); a ladder (libs/sync/swing.h SwingLadder: upper-body 16|32, + not interruptable 8, full body 8, the melee task against the puppet's own invisible invincible
anchor ped with the AI melee modifier at 0 and a harm breaker) climbs only on measured misses and pins the first variant seen playing. HKCU `melee_anim` = auto (default) | play |
melee_task | off. Logs: `engine: melee swings (R3 T6)` (effect counters), `engine: melee server (R3 T6)` (server-confirmed melee damage), server `melee:`.
