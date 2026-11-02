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
