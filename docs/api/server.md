# GAGE game server reference

This is the reference for everything an operator or a gamemode/server author touches in
`gage-server` (the code is in `apps/server/`, the UDP query codec in `libs/protocol/query.{h,cpp}`).
It describes what the shipped code does, nothing planned. Where a number is a limit, it is the
limit the code enforces.

Contents

1. [Running the server](#1-running-the-server)
2. [server.json](#2-serverjson), including the [replication settings](#replication-settings-interest-management-send-rates-bandwidth)
3. [bans.json](#3-bansjson) and [admins.json](#4-adminsjson-there-is-none)
4. [Console commands](#5-console-commands)
5. [The admin channel](#6-the-admin-channel-loopback-tcp)
6. [Master list announce](#7-master-list-announce)
7. [The UDP query](#8-the-udp-query)
8. [Refuse, leave and disconnect codes](#9-refuse-leave-and-disconnect-codes)
9. [Hardening limits at a glance](#10-hardening-limits-at-a-glance)
10. [Log lines](#11-log-lines)
11. [Embedding `gage::server::Server`](#12-embedding-gageserverserver)
12. [Building and platform notes](#13-building-and-platform-notes)

---

## 1. Running the server

```
gage-server [--config <path>] [--port <1..65535>]
```

| Argument | Meaning |
|---|---|
| `--config <path>` | the server.json to read. When given, a missing or invalid file is a fatal error. |
| `--port <n>` | UDP game port; overrides the file's `port`. |
| `--help`, `-h` | usage, exit 0. |

Without `--config` the server reads `./server.json` if it exists and otherwise runs on the built-in
defaults (it prints `no server.json, using the built-in defaults`). A `server.json` that exists but is
invalid, or cannot be read (a directory, no permission), is a fatal error and never silently falls back to defaults.

Exit codes: `0` clean stop, `1` config, ban-list or bind failure, `2` usage error.

The server stops on `SIGINT`/`SIGTERM`, on `quit` typed at its console and on `quit` sent through the
admin channel. Commands are read from stdin; when stdin reaches EOF (run as a service) the server
keeps serving and simply reads no more commands. Log lines go to stdout.

---

## 2. server.json

A single JSON object. Every key is optional; a missing key keeps its default. Unknown keys, wrong types,
non-integer numbers, out-of-range values and trailing data after the object are errors (so a typo can never leave a
default in place unnoticed). At most 1024 JSON values in the file (the two vehicle model lists may hold 128 each). Relative paths are relative to the
working directory the server is started in.

`apps/server/server.json.example` lists every key at its default; a test asserts it equals the built-in defaults.

| Key | Type | Default | Range / rules |
|---|---|---|---|
| `name` | string | `"GAGE Server"` | 1..64 bytes of well-formed UTF-8 without control characters. When `masterUrl` is set it also must not contain U+2028/2029, U+202A..202E or U+2066..2069 (the master rejects those). Sent to clients in `Welcome`. |
| `port` | integer | `24800` | 1..65535. The UDP game port (the UDP query is answered on the same port). |
| `maxPlayers` | integer | `32` | 1..1024. |
| `tickHz` | integer | `30` | 1..120. WorldState broadcasts per second (30 since sync plan W7; it was 20). The engine sends its own state at 1.5x this. 60 is not worth it: ~14 ms less delay for twice the cost. |
| `masterUrl` | string | `""` (announce off) | Base URL of the master: `http://host[:port][/path]`, at most 256 bytes, **plain http only** (`https://` is rejected: the server has no TLS), no credentials, no query, no fragment. Empty disables the announce. |
| `heartbeatSeconds` | integer | `30` | 5..3600. Announce period. The master's TTL is 90 s by default, so keep it well below that. |
| `gamemode` | string | `"freeroam"` | At most 32 bytes of UTF-8 without control, line-separator or bidi-override characters. Must be non-empty when `masterUrl` is set (the master requires 1..32 bytes). |
| `lang` | string | `"en"` | A language tag: 2-3 ASCII letters, optionally `-` and 2-4 letters/digits (`en`, `pt-BR`, `zh-Hans`); at most 8 bytes. |
| `url` | string | `""` | Optional info page shown in the launcher: empty, or `http://` / `https://` without credentials, printable ASCII, at most 256 characters. |
| `maxConnectionsPerIp` | integer | `4` | 0..1024. Live connections (joined, or still saying Hello) per source IPv4 address; the next one is refused `TooManyConnections`. `0` turns the cap off. |
| `chatBurst` | integer | `5` | 1..100. Chat flood limit per player: bucket size in messages. |
| `chatWindowSeconds` | integer | `5` | 1..3600. The bucket refills evenly over this many seconds. With the defaults a player can send 5 messages at once, then one per second. |
| `bansFile` | string | `"bans.json"` | At most 260 bytes. Path of the ban list. `""` keeps bans in memory only (lost on restart). |
| `adminPort` | integer | `0` (off) | 0..65535. Loopback TCP port of the [admin channel](#6-the-admin-channel-loopback-tcp). `0` disables it. |
| `adminToken` | string | `""` | 16..128 printable ASCII characters, no spaces. **Required when `adminPort` is not 0.** When the file sets none, the server uses the environment variable `GAGE_ADMIN_TOKEN`. |
| `latestClientVersion` | string | absent (= `"0.0.0"`, unknown) | `"x.y.z"`, each part 0..255, or `""`. The newest launcher release this server was released with; the [UDP query](#8-the-udp-query) reports it to v2 askers and a launcher older than it refuses PLAY and updates at once. When the file sets none, the server uses the environment variable `GAGE_LATEST_CLIENT_VERSION` (malformed = the server refuses to start). **Release-managed, not listed in `server.json.example`:** `tools/deploy/release.sh` writes `GAGE_LATEST_CLIENT_VERSION` to `/etc/gage/server.env` rather than this file, because `server.json` is shared by every release and an older server binary refuses a key it does not know, which would break a rollback (docs/OPS.md). Set it only to a client that is already published. |

Examples

```json
{ "name": "My Freeroam", "maxPlayers": 64 }
```

Listed on a master (the master must offer a plain-HTTP listener the game server can reach):

```json
{
  "name": "Los Santos Freeroam",
  "port": 24800,
  "maxPlayers": 64,
  "masterUrl": "http://master.example.org:8080",
  "gamemode": "freeroam",
  "lang": "en",
  "url": "https://example.org/gage"
}
```

With the admin channel (keep the token out of the file if the file is copied around):

```
export GAGE_ADMIN_TOKEN='a-long-random-string-0123456789'
```
```json
{ "adminPort": 24900 }
```

Errors read like `server.json: "heartbeatSeconds" must be a whole number 5..3600` or
`server.json: unknown key "bansFiles" (known: name, port, ...)`.

### Replication settings (interest management, send rates, bandwidth)

**No silent pins (0.4.3).** A value in `server.json` that undercuts another one is valid, so the server starts, but it is
never silent: at start the server logs the effective numbers and, for `rateNearHz` below `tickHz`, a `WARNING:` line.

```
replication (effective): tickHz 30, relevance enter/leave/max 400/450/600 m, lookahead 1000 ms, cell 128 m, relevance every 100 ms, near/mid distance .../... m, rates near/mid/far/idle/coarse 30/15/10/1/1 Hz, idle after ... ms, budget ... B/s per client, world packet max ... B, coarse share ...%
combat (effective): playerHealth 200, damage firearm 72% melee 50% explosive 100% run-over 100% headshot x300%, hit caps x100%, melee per-hit ceiling fists 40 / light 60 / heavy 80 (before the melee percent)
WARNING: rateNearHz (20) is below tickHz (30): players within nearDistance reach each other at 20/s, not 30/s (the world still ticks at 30 Hz); raise rateNearHz to tickHz or remove it from server.json
```

The 0.4.2 live test ran with `{"tickHz": 30, "rateNearHz": 20}` for a whole session and the players near each other got 20
updates a second without anything saying so. The warning is the same sentence as `ConfigWarnings` (`apps/server/config.h`),
and `tools/deploy/deploy-server.sh` prints it from the live `server.json` BEFORE it activates a release (it only reads the
file; it never rewrites it). The shipped defaults (`tickHz` 30, `rateNearHz` 30) warn nothing.

The server does not broadcast the whole world to everybody. Per client it keeps a **relevance set** of the players
near enough to matter, sends those at a rate that depends on distance, and sends everybody else about once a second
at low precision, all inside a **per-client byte budget**. The wire format is `docs/api/protocol.md`; this table is
what an operator can tune. Every key is optional and the defaults suit a 1000-player server; the 17 keys below are
listed with their defaults in `apps/server/server.json.example`.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `relevanceEnterRadius` | `400` | 10..20000 | metres. A player becomes relevant inside this distance (at rest) |
| `relevanceLeaveRadius` | `450` | 10..20000, at least the enter radius | metres. A player stops being relevant beyond this (at rest). The gap is the hysteresis: nobody flickers at the edge |
| `relevanceMaxRadius` | `600` | 10..20000, at least the enter radius | metres. Cap of the speed-aware enter radius |
| `relevanceLookaheadMs` | `1000` | 0..10000 | the enter radius grows by this many milliseconds of the pair's combined speed (`enter = min(max, enterRadius + lookahead * (speedA + speedB))`, the leave radius is that plus the hysteresis), so a fast approach is noticed early. `0` turns the speed-awareness off |
| `relevanceCellSize` | `128` | 64..2048 | metres. Edge of the uniform spatial grid (about a third of the enter radius is a good value) |
| `relevanceEveryMs` | `100` | 0..5000 | how often each client looks for newly relevant players (staggered per client). A pair can be noticed up to this late |
| `rateNearHz` | `30` | 1..120 (clamped to `tickHz`) | updates per second for a moving relevant player closer than `nearDistance` (every tick at the default `tickHz`) |
| `rateMidHz` | `15` | 1..120, at most `rateNearHz` | ... closer than `midDistance` (every 2nd tick) |
| `rateFarHz` | `10` | 1..120, at most `rateMidHz` | ... beyond `midDistance` (every 3rd tick; also the rate of an always-relevant player at any distance) |
| `rateIdleHz` | `1` | 1..120 | a player unchanged for `idleAfterMs` (inside the idle dead-band, see "Idle dead-band") is sent at most this often; its first change beyond the band is sent on that tick |
| `rateCoarseHz` | `1` | 1..120 | how often each non-relevant player appears on the coarse channel |
| `nearDistance` | `120` | 1..20000 | metres. Distance classes use distance minus half a second of the pair's combined speed |
| `midDistance` | `250` | 1..20000, at least `nearDistance` | metres |
| `idleAfterMs` | `500` | 50..60000 | |
| `clientBudgetBytesPerSec` | `36000` | 1000..100000000 | payload bytes per second the server may send one client in `WorldState` and `WorldCoarse` (a token bucket refilled every tick; the burst is at most `worldPacketMaxBytes` or two ticks' worth, whichever is larger). Updates over it wait and are served by accumulated priority, longest-waiting first |
| `worldPacketMaxBytes` | `1200` | 200..1300 | a `WorldState` or `WorldCoarse` packet never exceeds this, so it is never fragmented |
| `coarseSharePercent` | `20` | 1..90 | share of the budget reserved for the coarse channel, so a crowded neighbourhood cannot starve the map blips |

A wrong value is a startup error with the key and its range (`server.json: "rateMidHz" must be a whole number 1..120`),
and so are inconsistent sets (`relevanceLeaveRadius` below the enter radius, rates that increase with distance,
`midDistance` below `nearDistance`).

Measured behaviour (`docs/perf/RESULTS.md`, 15 s windows on one Windows 11 machine, loopback, the load-test
harness; measured with the former defaults of 20 Hz, rates 20 / 10 / 5 and a 24000 B/s budget, which send fewer
updates than the 30 Hz defaults: re-measure before sizing a big server): with those defaults 1000 simulated players run at 15 to 19 ms of tick work in the spread and mixed
scenarios (32 ms with all 1000 in six clusters of 70 to 150 m radius) and miss 1 to 7 ticks in 15 s, against 244 to 329 ms and
234 to 267 missed ticks for the v1 broadcast; a client receives 5 kB/s (spread) to 8 kB/s (mixed) on average
instead of 77 to 89 kB/s.

Operator notes

* **Per-client bandwidth** is the budget, not the demand. With the defaults a client receives at most 36 kB/s
  (288 kbit/s); an average player in a spread-out 1000-player world receives about 5 kB/s. Raise
  `clientBudgetBytesPerSec` for a dense hotspot server if players report lagging remote players, lower it for a
  metered link. The `stats` command shows `budget_deferred_per_s` (updates that waited): zero means the budget is not
  binding.
* **Radii** are about what the client can see and stream. The client's ped streaming range should be at most
  `relevanceEnterRadius`; the extra lookahead hides the approach of fast vehicles. Large radii raise CPU (more pairs),
  not just bandwidth.
* **A gamemode can force relevance** with `Server::SetAlwaysRelevant(subject, receiver, bool)` (section 12): the
  subject is then sent whatever the distance and never leaves, e.g. a race leader, a marker or a quest target.

### Gameplay settings (protocol v4: vehicles, health, respawn; v5: damage scaling)

The server owns vehicle occupancy and player health (`docs/api/protocol.md`, messages 18-30). Every key is optional.

| Key | Type | Default | Rule |
|---|---|---|---|
| `maxVehicles` | whole number | `64` | 1..256. Networked vehicles on the whole server. |
| `vehiclesPerPlayer` | whole number | `3` | 1..64. Vehicles one player may spawn. A player may also adopt (`Register`) up to this many ambient cars. See also `vehiclesPerIp`. |
| `vehicleEmptyDespawnSeconds` | whole number | `600` | 0..86400. An empty vehicle is removed after this long; `0` = never. |
| `vehicleEnterDistance` | whole number (m) | `10` | 1..100. An Enter request farther than this from the vehicle is refused. |
| `vehicleMaxSpeed` | whole number (m/s) | `150` | 10..600. A driver update that REPORTS a velocity faster than this is accepted with the velocity clamped to it (T4: a hyper car's overshoot used to freeze it for everybody else) up to twice the limit, and refused (`BadArg`) above that; a position that moved farther than this speed allows in the server's own elapsed time opens the untrusted window (see `untrustedWindowMs`). Also the speed limit of the movement check for a ped in a vehicle, falling, ragdolling or parachuting. |
| `fuelDrainPerMinute` | whole number | `10` | 0..1000. Fuel lost per minute of driving, in permille of a full tank. |
| `playerHealth` | whole number | `200` | 1..255. Health at join, `heal` and respawn, announced to every client in `Welcome.playerHealth` (the client's ped gets GTA health 100 more than this). v5 default 200 (v4: 100): about what an Online rank 80..100 player has, so a hit takes a believable share. |
| `firearmDamagePercent` | whole number | `72` | 1..1000. v5: percent of the damage a firearm Hit reports (handgun, SMG, rifle, machine gun, shotgun, sniper, unknown weapons) that the victim loses; 72 is Online's freemode factor. See [Damage scaling](#damage-scaling-protocol-v5). |
| `meleeDamagePercent` | whole number | `50` | 1..1000. v5: the same for melee (fists 30 becomes 15) and a firearm used as a club. |
| `explosiveDamagePercent` | whole number | `100` | 1..1000. v5: the same for explosives. |
| `runOverDamagePercent` | whole number | `100` | 1..1000. v5: the same for run-over damage. |
| `headshotPercent` | whole number | `300` | 100..1000. v5: the multiplier of a firearm Hit on the head bone (full up to 5 m, falling linearly to 100 percent at 40 m, sniper rifles 300 m). `100` = no headshot bonus. |
| `respawnArmour` | whole number | `0` | 0..255. v5: the armour a player comes back with at `RespawnOk`. Joining is with no armour. |
| `respawnDelaySeconds` | whole number | `5` | 0..300. A Respawn request sooner than this after death is ignored. |
| `maxHitDistance` | whole number (m) | `1000` | 10..5000. A Hit with the target farther than this from the shooter is dropped. |
| `hitsPerSecond` | whole number | `30` | 1..1000. Hits accepted per shooter per second. Damage per hit is clamped to 250 and further to the weapon class's cap (see [Anti-cheat rules](#anti-cheat-rules-protocol-v4)). |
| `meleeAdjudicate` | `0` or `1` | `1` | R3 T6. `1`: the server decides a melee hit itself (see [Melee judgement](#melee-judgement-r3-t6)): a swing (`shotSeq` step or `kPfMelee` edge with a melee weapon) is judged at its blow time against the stored tracks. `0`: only client-reported Hits (0.4.7 behaviour). |
| `meleeReachPercent` | whole number | `100` | 25..400. Scales the reach of every melee weapon class in the judgement (fists 1.4 m, light 1.7 m, heavy 2.1 m, plus a 0.5 m slack, centre to centre). |
| `spawnPoints` | array | `[[195.2, -933.8, 30.7, 140]]` | 1..16 points `[x, y, z, heading]` inside the world box (x, y within ±16000, z -2000..3000, heading ±360). Respawns pick from these. |
| `hitShotWindowMs` | whole number (ms) | `500` | `0` or 100..5000. A firearm Hit must name a shot, and a melee Hit a swing (`shotSeq` change), the server received within this long. A MELEE Hit may name a swing up to 1500 ms old whatever this is set to when it is smaller (0.4.5: a blow lands up to 900 ms after the button press on the client, so 500 refused late blows with no feedback); a larger value applies to melee too. `0` switches the shot requirement off (the damage caps and the melee reach stay). |
| `hitDamageScalePercent` | whole number | `100` | 10..1000. Scales every per-class damage cap and damage budget (the shipped numbers are estimates: raise it if legitimate players are clamped, lower it to be stricter). |
| `onFootMaxSpeed` | whole number (m/s) | `15` | 5..600. The movement check's speed limit for a ped on foot (sprinting is about 7 m/s). |
| `untrustedWindowMs` | whole number (ms) | `3000` | `0` (movement check off) or 500..60000. How long a player stays "untrusted" after a position jump. |
| `untrustedJoinGraceMs` | whole number (ms) | `15000` | 0..120000. After a player's first state the movement check is off this long (the engine teleports a joiner to its spawn point). |
| `connectsPerIpPerMinute` | whole number | `5` | 0..10000 (`0` = no limit). Connection attempts per source IPv4 address per minute (a token bucket of this size refilled evenly over 60 s), checked before any Hello work; the next is refused `TooManyConnections`. |
| `vehiclesPerIp` | whole number | `6` | 1..256. Vehicles spawned (and, separately, adopted) from one source address, orphans included. |
| `vehicleOrphanSeconds` | whole number | `60` | 1..3600. A vehicle whose spawner left while somebody sat in it is removed this long after it empties. |
| `registerCooldownMs` | whole number (ms) | `2000` | 0..60000. Minimum time between one player's `Register` commands. |
| `registerSeparationMeters` | whole number (m) | `4` | 0..20 (`0` = no such rule). `Register` closer than this to a networked vehicle is refused. |
| `vehicleModelDeny` | array | `[]` | At most 128 models the server never spawns or adopts (`NotAllowed`). Each entry: a whole number (the GTA hash), a `"0x..."` hex string, or a model name (hashed like `GET_HASH_KEY`, case-insensitive). |
| `vehicleModelAllow` | array | `[]` | Same format. When not empty, ONLY these models may be spawned or adopted, and it wins over `vehicleModelDeny`. |

Damage comes off armour first, then health. Emote names (`PlayAnim`, `PlayScenario`) are 1..63 bytes of
`[A-Za-z0-9_@/.-]`, at most 4 emote messages per second per player (`StopAnim` counts). A looping anim or scenario
is replayed to late joiners and cleared on death and respawn. These emote limits are fixed, not settings.

#### Damage scaling (protocol v5)

The damage a shooter's client reports is the single-player game's, without Online's freemode factor: against 100 health
a carbine killed in 4 hits and fists in 4 punches. Since v5 the server scales what it applies to a player
(`apps/server/damage.h`, `ScaleHit`) after the three caps above, which stay in the client's units: the result is
`round(damage x percent / 100)`, at least 1, with the percent of the weapon class (the five keys above). A Hit on the head
bone (31086) with a firearm is multiplied by `headshotPercent` at up to 5 m, linearly down to x1 at 40 m (300 m for the
sniper class); only the first Hit credited to a shot can be a headshot, and nothing but firearms ever is. Environmental
damage (a self-hit) and a vehicle's damage are not scaled. With the defaults and 200 health, no armour: a carbine
(Hit 32 -> 23) takes 9 hits, a pistol (26 -> 19) 11, fists (30 -> 15) 14, a carbine headshot at 3 m (69) 3. To get the
v4 numbers back set `playerHealth` 100, every `*DamagePercent` 100 and `headshotPercent` 100.

`Damage` goes to the victim, the attacker and every other player for whom either of them is in the relevance set, with the
bone and flags (headshot, melee, explosion, armour absorbed); `Killed` carries headshot and melee.

### Melee judgement (R3 T6)

Desync round 3, 0.4.7 live log: about 25 swings, 4 Hits sent, none from the game's damage event, 0 damage ever confirmed. A melee hit no longer
depends on the client reading GTA's damage event off a puppet. The server decides it from what it already holds, with no wire change.

* **A swing** is a state of a player on foot with a melee-class weapon (`WEAPON_UNARMED`, knife, bat, ...) whose `shotSeq` stepped, or whose
  `kPfMelee` bit rose with no step in the last 400 ms. Swings closer than 250 ms to the previous judged one are not judged (the client's own gate
  is 300 ms); at most 4 wait at a time.
* **The blow lands** 220 ms after the swing's sample (the press), by the sender's sample clock; the swing is judged then (or 90 ms later on the
  server's clock when the attacker's sample for that time never came).
* **Who is hit.** Attacker pose at the blow; each other player's pose at the blow minus 100 ms (what the attacker's screen showed: lag
  compensation) from a 48-sample track per player. The nearest living player **on foot**, in the attacker's relevance set, within the weapon
  class's reach (centre to centre: fists and knuckles 1.4 m, light weapons 1.7 m, heavy ones 2.1 m, each + 0.5 m slack, scaled by
  `meleeReachPercent`), less than 1.6 m above or below, and inside a 65 degree facing cone of the attacker's heading (inside 0.9 m facing is
  not required). One target per swing. A dead or seated player is no target; an attacker that is dead, untrusted, ragdolling, falling,
  climbing or in a vehicle at the blow throws no punch.
* **The damage** is the weapon's melee ceiling (`MeleeNominalMax`: fists 40, light 60, heavy 80, scaled by `hitDamageScalePercent`) through
  exactly the path a client-reported Hit takes: the melee damage budget (300 per second), `meleeDamagePercent`, `hitsPerSecond`, `HurtPlayer`
  (Damage to victim, attacker and onlookers with the melee flag, Killed with the melee flag). A client-reported Hit stays an input and is never
  counted twice: a Hit for the swing (`shotSeq`) and target the server already hit is a **confirmation** (no damage); one that arrives first
  pays and the judgement skips that swing; one for another target is the swing's second target; one the judgement found a miss for is accepted on
  the game's word and counted as **contradicting** (its distance and angle are logged).
* **Switches.** `meleeAdjudicate` (0 = the 0.4.7 behaviour: only client-reported Hits) and `meleeReachPercent` (25..400).
* **Cheating surface.** A client can only cause a hit that its own reported position, heading and swing counter could have caused, 1.4 to 2.6 m
  from a player in front of it, at most four a second and within the melee budget. It cannot name a victim.
* **The `melee:` line** (every 10 s while anything happened): `melee: last 10 s: swings seen N (too close to the previous swing N), judged N
  [hit=N no_target=N behind=N target_dead=N target_in_vehicle=N attacker_out=N budget=N claimed_first=N], misses with a player on foot within
  3 m N, damage applied by judged hits N; client-reported melee Hits: confirming a judged hit N, paid before the judgement N, on another target N,
  contradicting the judgement N, for a swing the server holds no judgement of N; adjudication on, reach x100%`. The first 40 hits and near misses
  are also logged one per line with the nearest player's distance and angle off the heading. `combat: last 10 s` gained `defer_dropped` (a Hit
  that waited for its shot and died with its shooter was dropped uncounted before).

### Anti-cheat rules (protocol v4)

A modified client controls everything it sends; the server decides what happens. These rules live in
`apps/server/anticheat.h` (pure logic) and `server.cpp` / `vehicles.h` (where they bite). None of them changes the
wire format. **All per-weapon numbers below are estimates** recalled from the game's weapon data, not measured in
game (`hitDamageScalePercent` moves them all); they err on the generous side because a refused legitimate hit costs a
player a kill, while a loose cap only costs a cheater a little less.

**Hits are bound to real shots.** The server rebuilds each shooter's shots from the changes of `PedState.shotSeq` in
the shooter's own states (stamped with the server's receive time, so the client cannot lie about when) and keeps the
last few weapons the state reported. A `Hit` against another player or a vehicle is accepted only when:

* the shooter is alive and not untrusted (below), the target exists, is alive (a player) and is within
  `maxHitDistance` and in the shooter's relevance set;
* **firearms** (handguns, SMGs, rifles, machine guns, shotguns, snipers, and any weapon the table does not know): the
  `shotSeq` the Hit names was stamped within `hitShotWindowMs`, the claimed weapon was reported in a state within the
  last 500 ms, and that shot has not already been credited with all the Hits its class allows (handgun, SMG, rifle 2;
  machine gun 3; shotgun 8; sniper 4: pellets, the engine's 250-point splits of a long burst);
* **melee** (fists, blades, clubs): v5 needs a **swing**, the client's count of a punch or a swing as a shot (a
  `shotSeq` change within `max(hitShotWindowMs, 1500 ms)`, with the weapon reported lately; 2 Hits per swing), and the target within
  4 m (7 m to a vehicle's centre) of the shooter's last position (v4 needed no swing: anybody within 4 m could claim 300
  damage a second). A firearm Hit with no shot behind it still passes when the weapon is reported and the target is
  that close (a pistol-whip), at melee damage;
  R3 T6: with `meleeAdjudicate` 1 (the default) the server does not wait for such a Hit at all, see
  [Melee judgement](#melee-judgement-r3-t6); a Hit for a swing and target the server already hit is a confirmation (no
  second count);
* **explosives** (grenades, rockets, mines, molotovs, ...): the shooter launched one (a `shotSeq` change while the
  state reported an explosive weapon) within the last 15 s and fewer than 16 Hits were charged to it, because their
  damage lands seconds after the shot;
* **run-over** (`WEAPON_RUN_OVER_BY_CAR` 0xA36D413E, what the engine reports for a car): the shooter drives a vehicle
  (seat 0) that the server measured moving at 3 m/s or more within the last 2 s, and it is within 12 m of the target;
* **environmental weapons** (fall, fire, explosion, drowning, ...) are accepted only as a **self-hit** (`targetId` =
  the sender; weapon 0 counts too), which can only hurt the sender and needs no shot. Aimed at anybody else they are
  refused.

**Where Hits die (0.4.5).** Every refused Hit is counted in exactly one reason slot (`Stats::hitRefusals`, `server::HitReason`), and while Hits flow the
server logs one line every 10 s: `combat: last 10 s: hits accepted N (melee M, melee clipped by a cap/ceiling/budget C, melee accepted only
through the 1500 ms melee window L, clamped K, scaled up to the 1-point floor F), damage applied D, deferred X, refused R [reason=count ...]`. The reasons:
`rate`, `shooter_dead`, `shooter_no_state`, `zero_damage`, `no_target`, `target_no_state`, `target_dead`, `untrusted`, `range`, `not_relevant`, `environment`,
`no_shot`, `weapon_not_held`, `shot_used`, `melee_reach`, `run_over`, `no_explosive`, `over_budget`, `defer_full`. The line is silent in a window with no Hit.

A Hit that fails only because its shot or weapon has not arrived yet (the reliable `Hit` can overtake the unreliable
state that carries the shot) is held up to 300 ms, at most 4 per player, and judged again with every state; after
that it is refused and counted (`hitsUnbound`). A player who dies drops the Hits still waiting.

Damage that passes is capped three times: 250 (the protocol), the class's cap per Hit, and a per-class damage
budget (a token bucket worth 2 seconds of the class's sustained rate, refilled continuously). Over a cap the damage is
clamped, not refused (`hitsClamped`); with the budget empty the Hit is refused (`hitsOverBudget`).

**Melee per-hit ceiling by weapon (0.4.3).** The live test of 0.4.2 reported "a punch while moving one-shots and kills": the
client's puppet of a player is a 1000-health ped, a running punch or a takedown kills it, and the client reported the
whole pool as damage; the melee class cap of 100 let 100 through per Hit (50 at the 50 percent melee scale, two Hits per swing,
so two swings killed 200 health). Whatever the client claims, a melee Hit is now clamped to a ceiling of its weapon BEFORE the
class cap, the budget and the damage scale (`ac::MeleeNominalMax`, `apps/server/anticheat.h`; counted in `hitsClamped`):

| Weapon | Ceiling per Hit (client units) | After the default 50 percent |
|---|---|---|
| fists, knuckle duster | 40 | 20 |
| knife and other light blades, tools (hammer, wrench, flashlight, ...) | 60 | 30 |
| bat, hatchet, machete, battle axe, stone hatchet, crowbar, golf club | 80 | 40 |
| a firearm used as a club, any weapon the table does not know | 40 | 20 |

The ceilings follow `hitDamageScalePercent` (50 gives 20 for fists) and never fall below 1. At the defaults a fist Hit claiming
1000 is applied as 20, so killing 200 health takes 10 punches; a bat takes 5. The ceilings are estimates of a single blow of the
single-player game, not measured; the class cap (100) still applies after them.

| Class | Per Hit | Sustained, per second | Hits per shot |
|---|---|---|---|
| melee | 100 | 300 | 2 per swing (v5; v4: no swing needed) |
| handgun | 160 | 500 | 2 |
| SMG | 80 | 800 | 2 |
| rifle | 120 | 900 | 2 |
| machine gun | 160 | 1500 | 3 |
| shotgun | 250 | 700 | 8 |
| sniper | 250 | 700 | 4 |
| explosive | 250 | 1000 | 16 per launch |
| run-over | 250 | 600 | no shot |
| unknown weapon | 100 | 400 | 2 |

**Movement is judged by displacement.** Every `PlayerState` and driver `VehicleUpdate` position is compared with the
previous accepted one: it may be at most `speed x (server seconds since + 0.25) + 2 m` away, where the speed is
`onFootMaxSpeed` on foot and `vehicleMaxSpeed` for a ped in a vehicle, falling, ragdolling or parachuting. The
velocity the client REPORTS is never used for this. A violation does not drop the state (a legitimate client-side
reset must not desync); it opens the **untrusted window** (`untrustedWindowMs`): the player's `Spawn`, `Enter` and
`Register` are answered `RateLimited` (an engine retries those), and its `Hit`s on other players and vehicles are
refused. Hits ON the player still land, and nothing else about it changes. Each violation is counted
(`movesRejected`) and logged at most once per 10 s per player (`untrusted for 3000 ms: PlayerState moved 26.0 m in 100
ms (on-foot limit 15 m/s)`). The check is off for `untrustedJoinGraceMs` after a player's first state and for 5 s after
`RespawnOk` (the engine teleports a ped there); `untrustedWindowMs` 0 turns the whole check off. The speed and fuel
of a driven vehicle come from the same displacement: a driver who claims 100 m/s without moving is neither "moving"
(the run-over rule) nor burning fuel.

**Reconnecting does not reset anything.** When a player leaves, its EMPTY spawned and adopted vehicles are destroyed
(everyone is told); one somebody else still sits in becomes an orphan: it has no spawner, still counts against the
source address (`vehiclesPerIp`) and is removed `vehicleOrphanSeconds` after it empties. A connection attempt over
`connectsPerIpPerMinute` from one address is refused `TooManyConnections` before any Hello work (the text says
`... (limit 5 per minute)`); there is no loopback exemption, so in-process tests and the load-test harness set the
limit to 0.

**Vehicle commands.** An over-limit `VehicleCmd` (10 a second, burst 10) is refused `RateLimited`. Every
`RateLimited` answer (that one, the spawn rate, the untrusted window, the `Register` cooldown) is sent at most once a
second per player; the others get no answer at all (`vehicleRepliesSuppressed`; a client must time out a command
it hears nothing about, as it must for a lost packet). A player that keeps sending at
more than 20 a second beyond the limit for a while is refused `Protocol` (`vehicleFloodKicks`, log `VehicleCmd
flood`). `Register` is refused `BadArg` closer than `registerSeparationMeters` to a networked vehicle and
`RateLimited` within `registerCooldownMs` of the player's previous one. `Create`-type commands (`Spawn`, `Register`)
check the per-player cap, the per-address cap, the global cap and a free id BEFORE the oldest empty adopted car is
given up to make room, so a refused command never costs a car. A model in `vehicleModelDeny` (or outside a non-empty
`vehicleModelAllow`) is `NotAllowed`.

**Ownership and control (protocol v5).** A vehicle a player `Spawn`s is **owned** by that player: nobody else gets its
driver's seat (`Enter` seat 0 answers `NotOwner`, checked before the seat shuffle, `SeatTaken` and the door checks), the
other seats are open to everybody, and the owner can give the vehicle up with `Release`. `Register`ed cars, orphans and
released vehicles are not owned. Independently, one client at a time **controls** each vehicle (`VehicleState.ctrl`) and is
the only one whose `VehicleUpdate` is taken: the player in seat 0; otherwise the current controller while it stays within
250 m, trusted, in contact (an update at least every 1.5 s, else it is skipped for 10 s); otherwise the nearest trusted
player within 200 m who has the vehicle in view. A vehicle with no eligible controller stands still. The displacement check
runs on receipt times, so a controller that sends a forged `sampleMs` cannot hide a jump. `VehicleWorld` carries the
motion of a driven vehicle as a 26-byte entry, with the statics (health, fuel, seats, ctrl, owner, lights, doors) on a
change and once a second.

**Emotes.** `PlayAnim` and `PlayScenario` from a dead player are dropped (`animsDeadDropped`). `Anim.flags` is
reduced to bits 0..8 (loop, hold last frame, reposition, not interruptable, upper body, secondary slot, reorient,
abort on movement, additive: they only change how the clip plays); the higher `TASK_PLAY_ANIM` bits (collision off,
physics override, ignore gravity, kinematic, hide weapon, dead pose, ragdoll on collision, ...) could pin, float or
un-collide the sender's ped on every other screen and are stripped, for the relay and for the replay to late
joiners (`animsSanitized`). `playbackRate` (x100) is clamped to 10..200. The flag meanings are from the community's
documentation of the game's flags, not verified against its scripts.

Counters for all of this are in `Server::stats()` (section 12); they are not part of the `stats` console command.

### Settings that are not in server.json

These exist on the `Config` struct (see `apps/server/config.h`) for tests and embedding; the executable keeps
their defaults: `bindHost` (all interfaces), `helloTimeoutMs` 5000 (a connection must say Hello within this,
else `HelloTimeout`), ENet peer timeout 32 tries / 5000 ms minimum / 30000 ms maximum, `queryRatePerIp` 20 and
`queryRateTotal` 2000 (UDP query replies per second), `announceBackoffBaseMs` 5000 and `announceBackoffMaxMs` 300000.

---

## 3. bans.json

The ban list is read at startup and rewritten after every change (`ban`, `banip`, `unban`). Writes are atomic:
the new content goes to `<file>.tmp`, is flushed to disk, and is renamed over the old file, so a crash leaves the
old list or the new one, never half of either.

```json
{
  "bans": [
    { "name": "Mallory", "ip": "203.0.113.5", "reason": "griefing", "at": "2026-10-29T08:34:56Z" },
    { "ip": "198.51.100.7", "reason": "spam", "at": "2026-10-29T09:01:12Z" },
    { "name": "Eve", "at": "2026-10-29T09:05:40Z" }
  ]
}
```

| Field | Type | Rules |
|---|---|---|
| `name` | string | A valid player name (1..24 characters from `A-Z a-z 0-9 _ . -` plus single inner spaces). Matched case-insensitively. |
| `ip` | string | A strict dotted IPv4 address (four decimal fields 0..255, no leading zeros). Matched exactly. |
| `reason` | string | Optional. Valid UTF-8 without control characters, at most 200 bytes. Shown to the banned player. |
| `at` | string | Optional, informational (ISO-8601 UTC, at most 40 characters). Filled in when a ban is added. |

* Each entry needs a `name`, an `ip`, or both. An entry with both is what `ban <online player>` writes.
* A connection is banned when its address **or** its name matches any entry. Removing an entry removes all of it.
* Unknown keys (at the top level or in an entry), wrong types, bad names/addresses, trailing data and more than
  10000 entries make the file invalid.
* **A file that exists but cannot be parsed or read stops the server from starting** (`ban list: <file>: <why>`);
  the file is left untouched. A damaged ban list never silently unbans everyone. A file that does not exist is
  an empty list; it is created by the first ban.
* You may edit the file by hand while the server is stopped. Edits made while it runs are not noticed and are
  overwritten by the next ban command.
* When the file cannot be written (disk full, read-only), the ban still holds in memory, the console reply
  carries `WARNING: not saved to <file>: <why>; the ban holds until restart`, and the log gets an `ERROR:` line.

## 4. admins.json: there is none

**There is no in-game admin by name and no `admins.json`.** No player is ever an administrator: not by name, not by
role, not by anything a client sends. The only authorities are the **server console** (stdin of the process) and the
optional **loopback admin channel**, which needs a shared token. Gamemodes and tools that need to moderate a
running server do it through one of those two.

---

## 5. Console commands

The console (stdin) and the admin channel run the same grammar. The command word is case-insensitive; arguments
are not. Leading/trailing blanks are ignored and an empty line does nothing. `<id>` is a player's numeric id (shown
by `list`), `<name>` a player name (case-insensitive).

Each command below shows its effect, its output (what the console prints and what the admin channel returns in
`output`), and its errors (the same text with `"ok":false` on the channel).

### `list`

Lists joined players (connections still saying Hello are not listed).

```
players 2/32
  0  Alice  203.0.113.5  rtt 24 ms  pos 120.5 -40.0 30.2
  3  Bob  198.51.100.7  rtt 61 ms  no state yet
```

Columns: id, name, source address, ENet round-trip time, last accepted position (or `no state yet` until the player
sends a state). Never fails.

### `kick <id|name>`

Cuts a player off: the player gets a server chat notice (`You were kicked from the server.`) and then
`Disconnected(Kicked)`; the others get `PlayerLeft(Kicked)`. The player can rejoin at once.
The whole rest of the line is the target, so names with an inner space work (`kick John Doe`).

* Output: `kicked <target>`
* Errors: `usage: kick <id|name>` (no argument); `no player "<target>"`.

### `say <text>`

Sends a server notice to every joined player (a chat message from the server id, shown by clients as a server
message). It bypasses the chat flood limit.

* Output: `sent`
* Errors: `usage: say <text>`; `not sent: text must be 1..200 bytes of UTF-8 without control characters`.

### `ban <id|name> [reason]`

Bans a player and writes `bans.json`.

* `<id>` or `<name>` of an **online** player: bans that player's **name and address**. The player is refused
  (`Banned`, with the reason) and cut off at once, and so is **everyone else connected from the same address**
  (they share the ban).
* A `<name>` that nobody is using: bans just that name; it is refused at Hello (`Banned`).
* The first word after `ban` is the target and the rest of the line is the reason (up to 200 bytes, no control
  characters). Because of that, to ban an online player whose name contains a space, use the numeric id.
* Output: `banned name Alice and address 203.0.113.5` or `banned name Alice`
  (with ` (WARNING: not saved to <file>: <why>; the ban holds until restart)` appended if the file could not be written).
* Errors: `usage: ban <id|name> [reason]`; `no player with id <n>` (a number nobody holds);
  `"<text>" is not a player name`; `already banned` (everything the entry would ban is already banned);
  `reason must be valid UTF-8 text without control characters, at most 200 bytes`;
  `the ban list is full (10000)`.

### `banip <ip> [reason]`

Bans an IPv4 address and writes `bans.json`. Everyone connected from that address is refused (`Banned`) and cut
off at once, and a new connection from it is refused **at connect, before it says Hello**.

* Output: `banned address 203.0.113.5` (plus the same WARNING as `ban` if not saved).
* Errors: `usage: banip <ipv4 address> [reason]` (no argument or not a strict dotted IPv4);
  `already banned`; the reason and list-full errors of `ban`.

### `unban <name|ip>`

Removes every entry whose name matches (case-insensitive) or whose address equals the argument, whole, and writes
`bans.json`. `unban Alice` removes an entry that banned both Alice and her address; `unban 203.0.113.5` removes only entries with that address.
The whole rest of the line is the target.

* Output: `unbanned <target> (1 entry)` or `unbanned <target> (2 entries)`
* Errors: `usage: unban <name|ip>`; `no ban matches "<target>"`.

### `bans`

```
bans 2
  name Mallory  ip 203.0.113.5  2026-10-29T08:34:56Z  reason: griefing
  ip 198.51.100.7  2026-10-29T09:01:12Z  reason: spam
```

The first line is the entry count. Never fails.

### `stats [json]`

Performance counters for the time since the previous `stats` (or since the server started), so the numbers are
rates over a window you control. Plain form: `stats` then one `  <key> <value>` line per counter; `stats json`: one
JSON object with the same keys. Any other argument is an error (`usage: stats [json]`).

| Keys | What |
|---|---|
| `window_s`, `players`, `tick_hz` | the window length, joined players, configured tick rate |
| `ticks`, `ticks_per_s`, `ticks_missed` | world ticks in the window; `ticks_missed` counts whole tick periods skipped because the loop ran late (the tick numbers jump over them, see "Tick numbers and sample ages") |
| `tick_work_us_avg/p50/p99/max` | microseconds of work per tick (replicate, build, hand to ENet, flush). Compare with the tick period (33 333 at the default 30 Hz, 50 000 at 20 Hz): the headroom |
| `tick_late_us_avg/p50/p95/p99/max` | how late ticks started against their slot |
| `cpu_percent` | CPU time of the server thread over the window (the platform's thread timer is coarse on Windows: use windows of 15 s or more) |
| `out_payload_bytes_per_s`, `out_wire_bytes_per_s`, `out_packets_per_s`, `out_datagrams_per_s` | what the server sent, as payload, as ENet wire bytes (UDP and IP headers not included), as packets handed to ENet and as UDP datagrams |
| `in_payload_bytes_per_s`, `in_packets_per_s`, `in_wire_bytes_per_s`, `in_datagrams_per_s` | the same for what it received |
| `client_out_bytes_per_s_avg/p50/p99/max` | payload bytes per second per client |
| `world_packets_per_s`, `world_entries_per_s`, `world_bytes_*`, `world_entries_*` | `WorldState` packets, player entries, bytes per packet and entries per packet (avg/p50/p99/max) |
| `coarse_packets_per_s`, `coarse_entries_per_s` | the coarse channel |
| `delta_entries_per_s`, `absolute_entries_per_s` | entries sent as deltas and in full |
| `budget_deferred_per_s` | due updates that waited for the per-client byte budget (0: the budget is not binding) |
| `relevance_enters_per_s`, `relevance_leaves_per_s`, `relevant_pairs` | churn of the relevance sets and the number of (client, player) pairs relevant now |
| `pairs_sent_per_s`, `pairs_idle_suppressed_per_s`, `pairs_rate_not_due_per_s`, `pairs_budget_deferred_per_s`, `pairs_packet_cap_deferred_per_s`, `pairs_no_state_per_s` | what became of every relevant (client, player) pair on every tick, exactly one of the six: an entry was sent (equals `world_entries_per_s`); not due because the idle rate held it back; not due at its distance class's rate; due but left out because the client's byte budget ran out; due but left out because of the one-packet size cap (the last two add up to `budget_deferred_per_s`); skipped because the player or the client has sent no state yet. They add up to about `relevant_pairs * ticks_per_s` |
| `wakes_per_s`, `noise_wakes_held_per_s` | idle players whose sample left the dead-band (so all their pairs were due at once), and the wake-ups a bit-exact idle test would have caused for samples that stayed inside it (see "Idle dead-band") |
| `samples_received_per_s`, `samples_forwarded_per_s`, `samples_forwarded_twice_per_s`, `samples_never_forwarded_per_s` | samples the replicator got (every accepted `PlayerState`, plus the server's own re-push of a changed health or armour); samples that went into at least one `WorldState`; samples sent again on a later tick (idle refreshes, a sender slower than the tick); and received minus forwarded: samples a newer one replaced before any tick sent them (by design for idle and far pairs; at near range a 30 Hz sender into a 20 Hz tick loses a third) |
| `pairs_sent_near_per_s`, `pairs_sent_mid_per_s`, `pairs_sent_far_per_s`, `pairs_sent_idle_per_s` | the entries of `pairs_sent_per_s` by the rate that applied to the pair: the distance class (`rateNearHz` / `rateMidHz` / `rateFarHz`) or the idle rate. Each should be about the rate times the number of such pairs; entries moving between the classes are tier changes |
| `enet_peers`, `enet_throttle_min`, `enet_loss_permille_max`, `enet_rtt_ms_avg`, `enet_rtt_ms_max`, `enet_rttvar_ms_max` | ENet's own view of the joined peers (gauges, read once a second): the lowest packet throttle (of 32: below 32 ENet deletes that share of UNRELIABLE packets, i.e. the state channel, by itself; the server configures deceleration 0 on every peer so it stays at 32), the highest smoothed loss of reliable packets (permille), the average and highest smoothed round trip and the highest round-trip deviation |

Percentiles come from log-scale histograms about 9% wide (within 5% of the true value). Everything is cumulative inside
the server and costs a few integer operations per event, so it stays on in production.

### Sync log line (every 10 s)

While somebody is joined the server log carries one line every 10 s with the same numbers as `stats`, compact:
`sync 10 s: 3 players, 30.0 ticks/s of 30 (0 missed, late p95 600 us); world 62.1 pkts/s 140.2 entries/s (near 60.0 mid 40.1 far 20.0 idle 20.1 /s, budget deferred 0.0 /s); samples in 135.0 /s, forwarded 88.0 twice 20.0 never 47.0 /s; enet 3 peers throttle min 32/32 loss max 0.0% rtt avg 41 max 77 ms var max 9 ms`.
A `throttle min` under 32 means ENet is deleting state packets (it should not with the server's deceleration 0: an older server build
or a changed config would show it); `never` is the by-design loss of samples a newer one replaced before a tick sent them.

### Idle dead-band

A standing player's sample is never bit-identical for long: the game's velocity reads +-1/32 m/s, a z of one
centimetre, a heading of a few units. A bit-exact idle test turned every such flicker into a wake-up and a 500 ms burst
at the near rate. Instead the replicator keeps, per player, a **reference**: the last sample that left the dead-band.
A sample is inside the band while, per axis, it is within 2 cm of the reference position, 2 velocity steps (1/32 m/s),
36 heading units (360/65536 degrees each, the short way round) and, for the aim of an aiming player, 3 dm, with every
other part of the on-foot state equal (move, flags, weapon, jump and shot counters, health, armour). Inside the band the
player keeps its idle rate and its `WorldState` entries say the reference, byte for byte (so a client sees the same
sample again, not a new one). Leaving the band wakes the player (its pairs are due on that very tick) and the sample
becomes the new reference, so a creep of 1 cm every 700 ms is quiet for two steps and wakes at the third, and nothing is
ever more than the band away from the truth. The coarse (out of range) blips are not affected. No setting: the band is
fixed (`apps/server/replication.cpp`).

### Tick numbers and sample ages

Tick numbers follow **wall time**: a server that sleeps through whole tick periods (a stall, a debugger, a suspended VM)
continues with the number it would have reached had it kept running (`ticks_missed` counts the skipped ones), and the
per-client history of what was sent moves with the number, so the frame sent just before the stall is still a delta
baseline. Clients place a sample at `tick * tickMs - ageMs`, which only works when the number follows the clock. A tick
is stamped with the time of its own slot, and `ageMs` of an entry is measured against that slot, not against the time
the loop got to it, so a tick that starts 40 ms late does not make every sample look 40 ms older than it is.

### `heal <id|name>`

Sets the player's health back to `playerHealth` (200 by default; armour is unchanged) and sends them `Damage` with the new values.

* Output: `healed <target>`
* Errors: `usage: heal <id|name>`; `no player "<target>"`; `<name> is dead (it comes back with Respawn)`.

### `kill <id|name>`

Kills the player as the server (no attacker); everyone gets `Killed`.

* Output: `killed <target>`
* Errors: `usage: kill <id|name>`; `no player "<target>"`; `<name> is already dead`.

### `armor <id|name> <0..255>`

Sets the player's armour.

* Output: `armor of <target> set to <n>`
* Errors: `usage: armor <id|name> <0..255>`; `no player "<target>"`; `<name> is dead`.

### `vehicles`

Lists the networked vehicles: a `vehicles <count>/<maxVehicles>` header, then one line per vehicle with id, model hash,
position, body/engine health, fuel (permille), `adopted` or `spawned`, `locked`, and its occupied seats as
`seat:playerId`.

### `help`

`commands: list | stats [json] | kick <id|name> | say <text> | ban <id|name> [reason] | banip <ip> [reason] | unban <name|ip> | bans | heal <id|name> | kill <id|name> | armor <id|name> <0..255> | vehicles | quit`

### `quit` (aliases `exit`, `stop`)

Asks the server to stop. The console loop finishes, tells the clients, flushes and exits `0`. No output.
Over the admin channel the reply is `{"ok":true,"output":""}` and the server then stops.

### Unknown commands

`unknown command "<word>" (try help)`.

---

## 6. The admin channel (loopback TCP)

An optional line protocol so a script or a future web panel can run the console commands without stdin.
It is **off unless `adminPort` is set**, and it is not an in-game admin system (see [section 4](#4-adminsjson-there-is-none)).

### Security model

* The listener binds **127.0.0.1 only**: never `0.0.0.0`, and there is no setting to change that. Every accepted
  peer is additionally checked to be a `127.x` address.
* The first line of a connection must be `auth <token>` with the `adminToken`. It is compared in constant time.
  The token is never logged or echoed.
* A wrong or missing token gets an error line and the connection is closed.
* After **10 failed logins within a minute** (all sources together; the allowance refills at 10 per minute, one
  per 6 s) new logins are refused without even looking at the token, so the token cannot be brute-forced through
  this port. (Side effect: someone on the machine who can reach the port can lock out the panel for a few seconds
  by failing logins on purpose.)
* At most **4 clients** at once, lines of at most **1024 bytes**, and a connection that has not authenticated
  within **5 s** is closed.
* The token is a shared secret over an unencrypted local socket. Anyone who can read the token, or sniff loopback
  as another local user, is trusted. Do not expose the port beyond the machine; a reverse proxy in front of a web
  panel must add its own TLS and login.

### Wire format

Plain TCP, UTF-8, one request per line (`\n`, a trailing `\r` is ignored). The server answers **every** line with
exactly **one JSON line**:

```
{"ok":true,"output":"<text>"}
{"ok":false,"output":"<text>"}     a command that failed
{"ok":false,"error":"<text>"}      a protocol-level failure (the connection is then closed)
```

`output` is the console output of the command, JSON-escaped (so `list` contains `\n`).

| Step | Request | Reply |
|---|---|---|
| 1 | `auth <token>` | `{"ok":true,"output":"authenticated"}` |
| 2.. | any [console command](#5-console-commands) | `{"ok":true,"output":"..."}` or `{"ok":false,"output":"<error text>"}` |

Protocol-level errors (all with `"ok":false` and an `"error"` key; the server then sends FIN and closes):

| `error` | When |
|---|---|
| ``authentication required: send `auth <token>` first`` | the first line was not `auth` |
| `bad token` | wrong or empty token |
| `too many failed logins, try again later` | the failure allowance is used up (the token is not even compared) |
| `login timed out` | no successful `auth` within 5 s |
| `line too long` | more than 1024 bytes without a newline, or a longer line |
| `too many admin connections` | 4 clients are already connected (sent to the new connection, which is closed) |

A command is never closed over: `{"ok":false,"output":"no player \"zed\""}` leaves the connection open for the next line.

### Example session

```
$ printf 'auth a-long-random-string-0123456789\nlist\nsay hello from the panel\nkick griefer\n' | nc 127.0.0.1 24900
{"ok":true,"output":"authenticated"}
{"ok":true,"output":"players 1/32\n  0  griefer  203.0.113.5  rtt 24 ms  no state yet"}
{"ok":true,"output":"sent"}
{"ok":true,"output":"kicked griefer"}
```

Node.js:

```js
import net from "node:net";
const s = net.connect(24900, "127.0.0.1");
s.write(`auth ${process.env.GAGE_ADMIN_TOKEN}\nbanip 203.0.113.5 spam\nbans\n`);
s.on("data", (d) => process.stdout.write(d));
setTimeout(() => s.end(), 500);
```

### Log lines of the channel

`admin channel on 127.0.0.1:<port> (loopback only, token required)` at start; `admin: client authenticated`;
`admin: login failed (bad token)`; `admin: login refused, too many failed logins recently`;
`admin: command: <line>` for every command an authenticated client runs (the `auth` line is never logged).

---

## 7. Master list announce

When `masterUrl` is set the server tells a master server (`apps/master/`) that it exists, so launchers can list it.
Nothing else about the server depends on the master being up: a dead, slow or hostile master can never stall a
tick or block a player (the announce runs on its own thread; the server only hands it a player count).

### What is sent

`POST <masterUrl path>/announce` over **plain HTTP/1.1** (no TLS, no redirects, one request per connection,
`Connection: close`). The path is the URL's path plus `/announce`; a URL that already ends in `/announce` is used as
it is: `http://m` and `http://m/` post to `/announce`, `http://m/base` to `/base/announce`.

```
POST /announce HTTP/1.1
Host: master.example.org:8080
User-Agent: gage-server
Accept: application/json
Content-Type: application/json
Content-Length: 150
Connection: close

{"port":24800,"name":"Los Santos Freeroam","gamemode":"freeroam","lang":"en","url":"https://example.org/gage","players":3,"maxPlayers":64,"protocol":2}
```

| Field | Value |
|---|---|
| `port` | the UDP game port actually bound |
| `name`, `gamemode`, `lang` | from server.json |
| `url` | from server.json; **omitted** when empty |
| `players` | joined players right now (updated every tick, so every heartbeat reports the live count) |
| `maxPlayers` | server.json `maxPlayers` |
| `protocol` | the game protocol version (`2`, `kVersion`; `docs/api/protocol.md`) |

The master lists the **TCP source address of the request** plus `port`; the body cannot name another address.
So the server must announce from the address players use (not a LAN address behind NAT). A master with its
UDP check on (`VERIFY_UDP=1`) also sends a [UDP query](#8-the-udp-query) to that address and port and lists the
server only if it answers, so the game port must be open for UDP from the internet.

There is no TLS: the master must offer a plain-HTTP listener reachable from the game server (a loopback or
private-network address, or a separate plain listener next to the public HTTPS one). An `https://` `masterUrl`
is rejected at startup.

### Schedule, retries and backoff

* The first announce goes out immediately at startup, from the announce thread.
* While the master answers `200`, the server announces every `min(heartbeatSeconds, the master's heartbeatSeconds)`.
* Any failure (network error, timeout, non-200 status) waits with exponential backoff: 5 s, 10 s, 20 s, ... up to
  300 s, doubling per consecutive failure, and goes back to the normal interval at the first `200`.
* A `Retry-After` header (the master sets it on 429 and 503) raises the wait to at least that many seconds (capped
  at 3600).
* One announce has 5 s to connect and 10 s in total; the response body is limited to 64 KB.
* Shutdown: `Stop()` aborts a request in flight within about 100 ms (a name lookup that is still blocking cannot be
  interrupted) and waits up to 2 s for the thread. No announce is made after `Stop()` returns.

### What the log says

```
announcing to http://master.example.org:8080 every 30 s
master: listed as 203.0.113.9:24800 (new entry), ttl 90 s, announcing every 30 s
master: announce rejected, HTTP 422 unreachable: <master's message> (the master could not reach UDP port 24800 with the GAGE query: open it in the firewall/NAT); retry in 5 s
master: announce to master.example.org:8080 failed: connect(): <os reason>; retry in 5 s
master: listed as 203.0.113.9:24800, ttl 90 s, announcing every 30 s (recovered)
```

The `listed` line appears when the server is first listed and again after a recovery, not on every heartbeat.
Every non-200 status is logged with the master's `error` code, its `message`, up to three `issues` it reports, a
hint, and the retry delay. Hints by master `error` code:

| Master error | Hint the server logs |
|---|---|
| `validation_failed` (400) | check name, gamemode, lang and url in server.json |
| `private_address` (403) | the master sees a non-public address (LAN, loopback, NAT); announce from a public address, or run the master with `ALLOW_PRIVATE=1` for local tests |
| `too_many_servers` (403) | the master already lists its per-address maximum for this address |
| `unreachable` (422) | the master could not reach the UDP port with the query: open it in the firewall/NAT |
| `protocol_mismatch` (422) | the master expects a different protocol version |
| `rate_limited` (429) | announcing too often for this address; backing off |
| `server_list_full` (503) | the master's list is full |
| `probe_busy` (503) | the master is busy probing other servers |
| `invalid_json`, `payload_too_large`, `unsupported_media_type`, `bad_forwarded_for` | unexpected for this client: a bug here or a misconfigured proxy in front of the master |

`Server::announceCounters()` reports `attempts`, `ok` (HTTP 200) and `failed` for embedding and tests.

---

## 8. The UDP query

The master (and the launcher, for ping and live counts) sends one 24-byte datagram to the **game port** and gets
one 24-byte reply. It is answered by the server before ENet sees the datagram, so a query never creates a peer or
a connection. `apps/master/PROTOCOL.md` is the authority for the layout; `libs/protocol/query.{h,cpp}` is the codec
(no sockets, shared by the server, the launcher and tools).

All integers are little-endian.

Request (to the game port):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `47 41 47 45` ("GAGE") |
| 4 | 1 | kind `0x01` |
| 5 | 1 | query version `1` |
| 6 | 2 | reply version wanted: `0` / `1` (v1 askers write `0`) = the v1 reply, exactly `2` = the v2 reply (query format v2, the "update ping"); anything else gets v1 |
| 8 | 8 | nonce, random per probe, opaque to the server |
| 16 | 8 | zero (padding so the reply is not larger than the request; ignored on receipt) |

Reply (to the request's source address and port):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `47 41 47 45` |
| 4 | 1 | kind `0x81` |
| 5 | 1 | query version `1` |
| 6 | 2 | game protocol version (`2`, what Hello must carry; `kVersion`) |
| 8 | 8 | the request's nonce, unchanged |
| 16 | 2 | players: joined players right now |
| 18 | 2 | maxPlayers |
| 20 | 4 | zero |

Test vectors (nonce `01 02 03 04 05 06 07 08`, 3 players, max 32; the codec test writes protocol 1 into the reply, which
is why the field below reads `01 00`; a live server writes `02 00`), asserted by `server_test`:

```
request 474147450101000001020304050607080000000000000000
reply   474147458101010001020304050607080300200000000000
```

Rules

* **Query format v2 (update ping).** A request whose `u16` at 6 is exactly 2 gets the **v2 reply**, also 24 bytes: byte 5 =
  `2` and bytes 20, 21, 22 = the `latestClientVersion` major, minor, patch (`0.0.0` when none is configured), byte 23 zero.
  Every other request gets the byte-exact v1 reply, so 0.4.2 / 0.4.3 launchers and the master are unaffected. The layouts,
  vectors and compatibility rules are in `docs/api/protocol.md` ("The UDP query (formats v1 and v2)").
* Only a datagram of **exactly 24 bytes** starting `GAGE 01 01` is a query. Anything else (23 or 25 bytes, wrong
  magic, kind or version, a reply sent as a request) is left to ENet and, not being ENet traffic, ignored: no reply,
  no peer, nothing logged.
* The reply is never larger than the request, so the server cannot be used to amplify traffic.
* Replies are rate limited: at most **20 per second per source address** and **2000 per second in total**
  (one second of burst each); the rest are dropped silently. Up to 4096 source addresses are tracked, and when
  that table is full of busy sources new ones are refused rather than the table grown. Counters:
  `Stats::queryAnswered` and `Stats::queryRateLimited`.
* It proves only that something at that address speaks this query. It is a liveness and counts check, not
  authentication.
* Compatibility with ENet: a datagram that starts with `GAGE` but is not a valid query (say 25 bytes) is handed to
  ENet, which reads the first two bytes (big-endian) as flags plus a peer id: `47 41` is peer id 1857 (0x741) with
  the "compressed" flag. ENet drops a datagram addressed to a peer id it does not have, so the server guarantees it
  never has that many peers: `static_assert(kMaxPlayers + kPendingSlots <= 0x740)` (1024 + 32 = 1056 today).

---

## 9. Refuse, leave and disconnect codes

### RefuseReason (`gage::proto::RefuseReason`, in a `Refuse` message)

A refused connection receives `Refuse{reason, text}` and is then disconnected (`kDisconnectRefused`); the client
sees a `Refused` event and then `Disconnected(Refused)`. A player who is already joined can be refused too (a
ban, or a protocol violation): the others then get `PlayerLeft(Kicked)`.

| Value | Name | When | Text (examples) |
|---|---|---|---|
| 1 | `VersionMismatch` | Hello carries a protocol version other than the server's (an older client is refused cleanly: `Hello` keeps its v1 layout) | player-facing, `proto::kRefuseTextClientOld` for an older client: `Solar RP was updated. Close the game; Solar Launcher installs the update.`; `proto::kRefuseTextClientNew` for a newer one: `This server has not been updated yet. Try again in a minute.` The protocol numbers go to the server log (`version mismatch <addr>: server speaks protocol vN, client sent vM`) |
| 2 | `BadName` | name is not 1..24 characters of `A-Z a-z 0-9 _ . -` with single inner spaces | `name must be 1..24 characters of A-Z a-z 0-9 _ . - and single inner spaces` |
| 3 | `NameTaken` | another joined player has the name (case-insensitive) | `name is already in use` |
| 4 | `ServerFull` | `maxPlayers` joined players already | `server is full` |
| 5 | `Protocol` | anything but a valid Hello first, a malformed or out-of-order message, an over-long chat message | `expected Hello first, got ChatSend`, `malformed Hello`, ... |
| 6 | `HelloTimeout` | no Hello within 5 s of connecting | `no Hello within 5000 ms` |
| 7 | `Banned` | the connecting address is on the ban list (checked at connect, before Hello), or the Hello name (or address again) is | `You are banned from this server.` or `You are banned from this server: <reason>` |
| 8 | `TooManyConnections` | `maxConnectionsPerIp` live connections from that address already, or more than `connectsPerIpPerMinute` connection attempts from it in the last minute (both checked at connect, after the ban check; the rate first) | `too many connections from your address (limit 4)`, `too many connection attempts from your address (limit 5 per minute)` |

The order inside Hello is: protocol version, name charset, ban list (name, then address again), server full, name taken.

Values 7 and 8 were added **without bumping the protocol version** (they date from v1; `kVersion` is 2 now): they only ever appear in a
`Refuse`, after which the connection ends. A client built before them cannot decode such a `Refuse`
(unknown reason), so it gets no `Refused` event and no text, but the server's disconnect code still ends its session
as `Disconnected(Refused)`. Clients that know the values show the text.

### LeaveReason (`PlayerLeft.reason`, sent to the other players)

| Value | Name | Meaning |
|---|---|---|
| 1 | `Quit` | the client said goodbye (`kDisconnectQuit`) |
| 2 | `Timeout` | the client vanished (no traffic within the ENet peer timeout) |
| 3 | `Kicked` | the server cut the player off: `kick`, `ban`/`banip`, or a rule violation (a `Refuse` of a joined player) |

### ENet disconnect data (`kDisconnect*`)

The 32-bit data word of the ENet disconnect, so the far side can tell why. `0` means no deliberate disconnect (a
timeout).

| Value | Name | Direction | Meaning |
|---|---|---|---|
| 1 | `kDisconnectQuit` | client to server | the player quit |
| 2 | `kDisconnectKicked` | server to client | admin kick (client: `Disconnected(Kicked)`) |
| 3 | `kDisconnectRefused` | server to client | after a `Refuse` (client: `Disconnected(Refused)`) |
| 4 | `kDisconnectShutdown` | server to client | the server is stopping (client: `Disconnected(Shutdown)`) |

---

## 10. Hardening limits at a glance

| Protection | Default | Setting |
|---|---|---|
| Connections per source address | 4 | `maxConnectionsPerIp` (0 = off). Counts joined and still-saying-Hello connections; refused connections that are closing do not count. ENet additionally drops connects silently beyond `2 x cap + 4` peer slots from one address, so refused-but-lingering connections cannot pin the server. |
| Chat flood | 5 messages burst, then 1 per second | `chatBurst`, `chatWindowSeconds`. A message over the limit is dropped, the sender gets a server notice (`Message not sent: you are sending messages too fast, slow down.`, at most once per second), nobody is disconnected. |
| UDP query replies | 20/s per address, 2000/s total | not configurable in server.json |
| Hello deadline | 5 s | not configurable in server.json |
| Admin channel | loopback only, token, 10 failed logins per minute, 4 clients, 1024-byte lines, 5 s to log in | `adminPort`, `adminToken` |
| Ban list | name and/or address, 10000 entries | `bansFile` |
| Bytes sent per client | 36000 B/s of `WorldState` + `WorldCoarse`, bursts of at most 1200 B (or two ticks' worth) | `clientBudgetBytesPerSec` |
| State packet size | 1200 bytes, so no fragmentation | `worldPacketMaxBytes` |
| Hostile `PlayerState` / `Hello` | refused by a strict decoder (truncation, trailing bytes, values outside the quantiser box, non-canonical varints); a v1 client gets `Refuse(VersionMismatch)` | not configurable |
| Connection attempts per source address | 5 a minute, checked before any Hello work | `connectsPerIpPerMinute` (0 = off). The table of addresses is bounded (4096); when it is full of busy addresses a new one is let through. |
| Forged `Hit`s | a Hit needs a shot the server saw, the weapon in hand, a per-shot hit count, a per-class damage cap and budget; melee and run-over have their own reach rules | `hitShotWindowMs`, `hitDamageScalePercent`, `maxHitDistance`, `hitsPerSecond`; see [Anti-cheat rules](#anti-cheat-rules-protocol-v4) |
| Teleporting / speed hacks | position jump against the elapsed server time opens a 3 s window in which Enter / Register / Spawn and outgoing Hits are refused | `onFootMaxSpeed`, `vehicleMaxSpeed`, `untrustedWindowMs`, `untrustedJoinGraceMs` |
| Vehicles per source address | 6 spawned and 6 adopted, orphans included; a leaver's empty vehicles are destroyed, occupied ones time out 60 s after they empty | `vehiclesPerIp`, `vehicleOrphanSeconds` |
| `VehicleCmd` flood | 10 a second; `RateLimited` answers at most 1 a second, a sustained flood is refused `Protocol` | not configurable |
| `Register` | 2 s between commands, not within 4 m of a networked vehicle | `registerCooldownMs`, `registerSeparationMeters` |
| Vehicle models | any | `vehicleModelDeny`, `vehicleModelAllow` |
| Emotes | `Anim.flags` reduced to bits 0..8, playback rate 0.1x..2x, none while dead | not configurable |

A bad `PlayerState` packet (stale, out of the world box, malformed) is dropped with a rate-limited log line and never
disconnects the player: it is unreliable data.

---

## 11. Log lines

Every line starts with `[YYYY-MM-DD HH:MM:SS.mmmZ]` (UTC). The ones you will grep for:

| Line | Meaning |
|---|---|
| `listening on UDP <port>, name "<name>", max <n> players, <hz> Hz, protocol v2` | started |
| `bans: <n> entries from <file or memory only>` | ban list loaded |
| `replication (effective): tickHz <n>, ...` / `combat (effective): playerHealth <n>, ...` | 0.4.3: the effective replication and damage numbers, logged at start (see [Replication settings](#replication-settings-interest-management-send-rates-bandwidth)) |
| `WARNING: rateNearHz (<n>) is below tickHz (<m>): ...` | 0.4.3: `server.json` pins the near send rate under the tick rate, so players near each other get fewer updates than the world ticks (valid, not fatal) |
| `admin channel on 127.0.0.1:<port> (loopback only, token required)` | admin channel up |
| `announcing to <masterUrl> every <n> s` | announcer started (see [section 7](#7-master-list-announce) for its `master:` lines) |
| `connect <ip>:<port>` | a connection arrived |
| `join id=<id> name=<name> addr=<ip> players=<n>/<max>` | a player joined |
| `leave id=<id> name=<name> reason=<Quit\|Timeout\|Kicked> players=<n>/<max>` | a player left |
| `refuse <Reason> <ip> [(<name>)]: <text>` | a connection was refused |
| `drop id=<id> <name>: <why>` | a packet was dropped (flood, invalid state, ...), rate limited per player and kind. v4 adds `Hit refused: <rule>` (a forged or over-cap Hit, e.g. `no shot of the shooter's with shotSeq 5 in the last 500 ms`), `untrusted for <ms> ms: <jump>` (at most once per 10 s per player); a sustained `VehicleCmd` flood shows as `refuse Protocol <ip> (<name>): VehicleCmd flood: ...` |
| `chat <name>: <text>` / `say: <text>` | chat and server notices |
| `kick id=<id> name=<name>` | a kick |
| `ban name=<name> ip=<ip> reason="<reason>"` / `ban ip=<ip> reason="..."` / `unban <target> (<n> entries)` | ban changes |
| `ERROR: ban list not saved: <why>` | bans.json could not be written; the ban holds in memory |
| `stopped` | clean stop |

---

## 12. Embedding `gage::server::Server`

The server core is a library (`gage_server_core`) with no `main()`, so a test or a tool can run it in-process:

```cpp
gage::server::Config cfg;
cfg.port = 0;                 // let the OS pick; read it back with server.port()
cfg.bindHost = "127.0.0.1";
cfg.bansFile = "";            // memory-only bans
cfg.log = [](const std::string& line) { /* the line already has its timestamp */ };

gage::server::Server server;
std::string err;
if (!server.Start(cfg, &err)) { /* err says why */ }
while (running) server.Poll(10);   // services the network, timers, admin channel, WorldState
std::string out;
server.Execute("say hello", out);  // the same grammar as the console
server.Stop();
```

The relevance API for gamemodes (all in `server.h`; ids are the `Welcome.yourId` values, `proto::kServerId` means
"every player"):

```cpp
// Always send `subject` to `receiver` at full detail, whatever the distance (far rate, never dropped by the leave radius).
server.SetAlwaysRelevant(subject, receiver, true, &err);           // false + err: unknown id, subject == receiver
server.SetAlwaysRelevant(subject, gage::proto::kServerId, true);   // to everybody, late joiners included
server.SetAlwaysRelevant(subject, gage::proto::kServerId, false);  // back to the radii, for every receiver
bool pinned = server.AlwaysRelevant(subject, receiver);
bool seen   = server.IsRelevant(receiver, subject);                // in the receiver's relevance set right now
size_t n    = server.RelevantCount(receiver);                      // players sent to it at full detail
```

An override costs a few bytes of state per pair, its updates count against the receiver's byte budget like any other,
and it disappears with the subject (or the receiver) when that player leaves. `server.metrics()` is the same counter
set `stats` prints; `server.stats()` the cumulative event counters.

The class is single-threaded and caller-driven: call every method, `Execute()` and the accessors included,
from the thread that calls `Poll()`. `tests/server_test.cpp` and `tests/netrig.h` show complete in-process
servers with real ENet clients. Every public class and function has a header comment with its parameters,
return values, thread-safety and errors (`apps/server/*.h`, `libs/protocol/query.h`).

---

## 13. Building and platform notes

Standalone server build (no Windows client, no installer; the entry point for a Linux VPS):

```
cmake -S apps/server -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server --parallel
build/server/server/gage-server --config server.json
build/server/selftest/netcode_test
build/server/selftest/server_test
build/server/selftest/quant_test
build/server/selftest/interest_test
build/server/tools/gage-loadtest --bots 200 --scenario mixed
```

`apps/server/CMakeLists.txt` builds `enet`, `gage_protocol`, `gage_netclient` (the tests need the client session),
`gage_server_core`, `gage-server`, `netcode_test`, `quant_test`, `interest_test`, `server_test` and the load-test
harness `gage-loadtest`, all from `cmake/gage_server.cmake`, which the main build includes too. The main build reaches
exactly the same set with `cmake -S . -B build/server -DGAGE_SERVER_ONLY=ON`. Verified on Windows from an empty build
directory (`cmake -S apps/server -B <scratch> -G Ninja`, 2026-11-01): 38 build steps, zero warnings, `netcode_test`
182/182, `server_test` 80/80, `quant_test` 33/33 and `interest_test` 38/38. It links the C++ runtime statically on Windows only (`-static` is never used on Linux, where a
fully static glibc binary cannot resolve names reliably).

Platform status, honestly: the server, its four test programs and the load-test harness were built and run on Windows 11 (WinLibs gcc 13.3,
mingw-w64) with zero compiler warnings. The Linux build has **not been run here**; the code is written to be
POSIX-clean (a Winsock/POSIX socket shim in `apps/server/sock.cpp`, `poll`, `MSG_NOSIGNAL` plus `SIGPIPE` ignored,
`fsync` of the file and its directory on POSIX, no Windows headers outside `#ifdef _WIN32`), but treat the first Linux
build as untested until `server_test` passes there. One check of `server_test` (the admin channel refuses the
machine's own non-loopback address) needs such an address and reports itself as SKIPPED on a host that has none.
