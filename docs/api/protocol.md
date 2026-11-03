# GAGE game protocol v5

The wire protocol between the GAGE client engine and a GAGE server: every message, every byte, the quantisation
formulas, the limits and the refusal codes. It describes what the shipped code does. The code is
`libs/protocol/` (`protocol.h` the messages, `quant.h` the quantisation, `worldcodec.h` the delta and coarse
codecs, `wire.h` the primitives); the server side is `apps/server/replication.cpp` (players),
`apps/server/vehicles.h` (vehicles) and `apps/server/server.cpp` (combat, emotes), the client side
`libs/netclient/session.cpp`. `docs/api/server.md` covers running the server; this file is what a client, a bot
or a tool needs to talk to one.

Contents

1. [Transport and versioning](#1-transport-and-versioning)
2. [Primitives](#2-primitives)
3. [Connection flow](#3-connection-flow)
4. [Messages](#4-messages)
5. [Quantisation](#5-quantisation)
6. [WorldState: relevance, rates and deltas](#6-worldstate-relevance-rates-and-deltas)
7. [WorldCoarse](#7-worldcoarse)
8. [Limits](#8-limits)
9. [Refuse, leave and disconnect codes](#9-refuse-leave-and-disconnect-codes)
10. [Strict decoding](#10-strict-decoding)
11. [Test vectors](#11-test-vectors)
12. [Changes from v1](#12-changes-from-v1)
13. [Changes from v2](#13-changes-from-v2)
14. [Changes from v3](#14-changes-from-v3)
15. [Changes from v4](#15-changes-from-v4)

---

## 1. Transport and versioning

* Transport: [ENet](http://enet.bespin.org/) over UDP, default port 24800 (`kDefaultPort`). The server answers the
  24-byte UDP query on the same port (`docs/api/server.md` section 8); everything else is ENet.
* Two ENet channels: **0 `kChanReliable`** (reliable, ordered) carries every control message and chat; **1
  `kChanState`** (unreliable, sequenced, sent with `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT`) carries `PlayerState`,
  `VehicleUpdate`, `WorldState`, `WorldCoarse` and `VehicleWorld`. An unreliable packet larger than the MTU is fragmented by ENet and
  lost whole when any fragment is lost, which is why the server caps its state packets (below).
* Every ENet packet is one message: `u8 type` followed by the body. All integers are little-endian.
* `kVersion = 5`. A server refuses a `Hello` whose version differs with `Refuse(VersionMismatch)` and closes the
  connection, so clients and servers of different versions never exchange game data (a v4 client is told
  `Solar RP was updated. Close the game; Solar Launcher installs the update.`, a v6 client `This server has not
  been updated yet. Try again in a minute.`: player-facing sentences, `proto::kRefuseTextClientOld` / `kRefuseTextClientNew`; the
  numbers are in the server log). `Hello` and `Refuse` keep their v1 layouts forever for exactly this
  reason: they are the two messages a client of another version must be able to parse. Client and server ship
  together.
* A `WorldState`, `WorldCoarse` or `VehicleWorld` packet never exceeds `worldPacketMaxBytes` (default 1200, so it
  fits one 1400-byte-MTU datagram without ENet fragmentation; a `VehicleWorld` presence list alone is at most 521
  bytes).

### The UDP query (formats v1 and v2)

Not part of the ENet protocol: a separate 24-byte datagram pair on the game port, answered before ENet sees the
datagram (`docs/api/server.md` section 8 for the server rules, `apps/master/PROTOCOL.md` for the master's use, codec
`libs/protocol/query.{h,cpp}`). The **query format version** is its own number and has nothing to do with `kVersion`
(the game protocol stays 5). All integers are little-endian; offsets are bytes from the start of the datagram.

Request, 24 bytes (v1 and v2 differ only in the `u16` at offset 6):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `47 41 47 45` ("GAGE") |
| 4 | 1 | kind `0x01` |
| 5 | 1 | query version `1` (always, in both formats) |
| 6 | 2 | **reply version wanted**: `0000` (what v1 askers write) or `0100` = v1 reply; `0200` = **v2 reply**. Exactly `2` asks for v2; every other value (3, `FFFF`, `0201`...) gets the v1 reply |
| 8 | 8 | nonce, random per probe |
| 16 | 8 | zero (ignored on receipt) |

Reply v1, 24 bytes (what a v1 request gets, and what every server before the update ping sends):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic |
| 4 | 1 | kind `0x81` |
| 5 | 1 | query version `1` |
| 6 | 2 | game protocol version (`kVersion`) |
| 8 | 8 | the request's nonce |
| 16 | 2 | players |
| 18 | 2 | maxPlayers |
| 20 | 4 | zero (ignored on receipt) |

Reply v2, 24 bytes (only to a request that asked for it): the v1 layout except

| Offset | Size | Field |
|---|---|---|
| 5 | 1 | query version `2` |
| 20 | 1 | **latest client, major** |
| 21 | 1 | latest client, minor |
| 22 | 1 | latest client, patch |
| 23 | 1 | zero (ignored on receipt) |

"Latest client" is the newest launcher release `x.y.z` this server was released with (each part 0..255); `0.0.0`
means unknown (the operator configured none). A launcher that sees a latest client newer than its own refuses PLAY
(`launchResult` code 14) and starts its update check at once, so a release that keeps the game protocol (like 0.4.3)
is noticed within one 5-second query instead of the 2-minute manifest poll (`docs/UPDATES.md`). The server takes the
value from `server.json` `latestClientVersion` or the environment variable `GAGE_LATEST_CLIENT_VERSION`
(`docs/api/server.md`; `tools/deploy/release.sh` sets the variable).

Test vectors (nonce `01 02 03 04 05 06 07 08`, protocol 1, 3 players, max 32, latest client 0.4.4), asserted by `server_test`:

```
request v1  474147450101000001020304050607080000000000000000
request v2  474147450101020001020304050607080000000000000000
reply v1    474147458101010001020304050607080300200000000000
reply v2    474147458102010001020304050607080300200000040400
```

Compatibility rules (why the request keeps query version 1)

* **Old askers, new server.** A 0.4.2 / 0.4.3 launcher and the master send the v1 request. The server answers it with
  the exact v1 reply, 24 bytes, query version 1: their `Decode` refuses any other length or version, so a v2 reply
  is never sent unasked.
* **New asker, old server.** A v2 request is a valid v1 request to a server that predates v2 (it checks `GAGE 01 01`
  and ignores offsets 6, 7 and 16..23), so it answers v1 in the same round trip. The asker accepts both formats and
  reads "latest client unknown" from a v1 reply; no retry, no fallback delay. (A request with query version `2`
  in byte 5 would be ignored by an old server: it is not a query to it, so the format is not signalled there.)
* **Framing.** Unchanged: only an exactly-24-byte datagram starting `GAGE 01 01` is a query. A v2 request with a
  trailing byte (25), one byte short (23), or with `02` in byte 5 is not a query: no reply, nothing logged, handed to
  ENet which drops it. Garbage in the `u16` at offset 6 is not an error; it just selects the v1 reply.
* **Same size.** The v2 reply is exactly as long as the request, so the amplification argument is unchanged.

## 2. Primitives

| Name | Encoding |
|---|---|
| `u8`, `u16`, `u32`, `u64` | unsigned, little-endian |
| `i16` | two's complement, little-endian |
| `varu32` | LEB128: 7 bits per byte, low group first, bit 7 set when more bytes follow; 1..5 bytes. Only the shortest form is accepted (a trailing `00` group after a continuation byte is refused), at most 32 bits |
| `vari32` | zigzag (`(v << 1) ^ (v >> 31)`: 0, -1, 1, -2, 2 ... become 0, 1, 2, 3, 4 ...) then `varu32` |
| `str` | `u16` byte length, then that many bytes of UTF-8 (each message states its cap) |

## 3. Connection flow

```
client                                   server
  | ---- ENet connect ------------------> |
  | ---- Hello{version, name} ----------> |   reliable; must arrive within 5 s (HelloTimeout)
  | <--- Welcome{yourId, tickHz, name, playerHealth} |   reliable (playerHealth: v5), or
  | <--- Refuse{reason, text} ----------- |   reliable, then the connection is closed
  | <--- PlayerJoined x (players here) -- |   reliable: the roster, one message per player
  | <--- PeerLook x (players with a look) |   reliable: right after the roster burst (v3)
  | ---- PlayerLook --------------------> |   reliable: own appearance, on change (v3)
  | ---- PlayerState (1.5x tickHz) ----> |   unreliable: own movement + on-foot state + WorldState receipt
  | <--- WorldState (up to tickHz) ------ |   unreliable: the relevant players, deltas
  | <--- WorldCoarse (up to 4 per s) ---- |   unreliable: everybody else, low rate, low precision
  | <--> ChatSend / ChatMessage --------- |   reliable
  | <--- PlayerJoined / PlayerLeft ------ |   reliable: the roster changes
  | <--- PeerLook ----------------------- |   reliable: someone else's appearance changed (v3)
  | <--- PeerAnim / PeerScenario x ------ |   reliable: after the PeerLook burst, every running loop / scenario (v4)
  | <--- VehicleWorld (up to tickHz) ---- |   unreliable: the vehicles relevant to you, per receiver (v4)
  | ---- VehicleCmd --------------------> |   reliable: spawn, enter, exit, lock, ... (v4)
  | <--- VehicleResult ------------------ |   reliable: the answer to every VehicleCmd (v4)
  | ---- VehicleUpdate (controller) ----> |   unreliable: the controlled vehicle, every send tick (v4: driver only; v5: the server's pick)
  | ---- Hit ---------------------------> |   reliable: a shot that hit, validated by the server (v4)
  | <--- Damage / Killed ---------------- |   reliable: health is the server's (v4); v5: also to everyone who sees the victim or the attacker
  | ---- Respawn / <--- RespawnOk ------- |   reliable: after the respawn delay (v4)
  | <--> PlayAnim ... / PeerAnim ... ---- |   reliable: emotes, relayed (v4)
```

The roster (`PlayerJoined`, `PlayerLeft`) is always complete and reliable; `WorldState` only carries the players
that are relevant to this client right now (section 6). The server never sends a client its own entry, nor its own
look. A `PeerLook` always arrives after the `PlayerJoined` of its id (one reliable, ordered channel); the newest one
per id is that player's look until its `PlayerLeft`.

## 4. Messages

`type` values are wire-stable (`enum class Msg`).

### 1 `Hello` (client to server, reliable)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type `01` |
| 1 | 2 | `u16` protocol version (`kVersion`) |
| 3 | 2 + n | `str` name, 1..24 bytes (`A-Z a-z 0-9 _ . -` and single inner spaces) |

### 2 `Welcome` (server to client, reliable)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type `02` |
| 1 | 2 | `u16` yourId (0..1023) |
| 3 | 2 | `u16` tickHz, the server's world tick rate (1..120) |
| 5 | 2 + n | `str` server name, at most 64 bytes |
| 7 + n | 1 | v5: `u8` playerHealth, the server's maximum health (`playerHealth` in server.json, 1..255, default 200); 0 is refused. A player joins, heals and respawns with this much, and a client sizes its health bar and its ped to it (GTA ped health = 100 + playerHealth) |

### 3 `Refuse` (server to client, reliable)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type `03` |
| 1 | 1 | `u8` reason (section 9) |
| 2 | 2 + n | `str` text, at most 200 bytes |

### 4 `PlayerJoined` (server to client, reliable)

type `04`, `u16` id, `str` name (at most 24 bytes).

### 5 `PlayerLeft` (server to client, reliable)

type `05`, `u16` id, `u8` reason (section 9). After it the id may be reused by a later player.

### 6 `PlayerState` (client to server, state channel): 39 bytes, 45 while aiming (v5; one more while entering or exiting a vehicle)

The ack fields are the only way a client tells the server what it has decoded, and the server builds its delta
`WorldState` frames against the newest tick acknowledged within the last 32. A client that has nothing new to say
(a loading screen, a long hitch) therefore keeps sending its **latest state again** at least every 100 ms (the engine's
join worker does); the server reads it as the same sample, and without the receipt every entry would go out in full
from 1.07 s on (at the default 30 Hz; 1.6 s at 20 Hz).

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type `06` |
| 1 | 4 | `u32` seq: newer than the previous one (serial arithmetic); an older or equal seq is dropped |
| 5 | 4 | `u32` ack tick: the newest `WorldState` tick this client fully decoded (0 = none yet) |
| 9 | 4 | `u32` ack bits: bit i set = the `WorldState` of tick `ackTick - 1 - i` was also decoded |
| 13 | 2 | v5: `u16` sampleMs, the sender's millisecond clock (any monotonic clock, modulo 65536) at the moment this state was sampled |
| 15 | 16 | the kinematics (below) |
| 31 | 8 to 15 | the ped block (below) |

`sampleMs` (v5) lets the server place the sample on its own timeline instead of at the packet's arrival: it unwraps the
16-bit stamp against the newest one (signed difference, so a wrap is invisible), tracks the smallest `arrival - stamp`
offset over the last 2 s (the packets that met no queueing), and puts the sample at `stamp + offset`, never earlier than 300
ms before the arrival and never after it; the first 8 samples of a connection, and the first after a gap of more than 30 s,
are placed at their arrival (`libs/sync/sampleclock.h`, one clock per message stream). The `age` of the sample in a
`WorldState` entry is measured from that placed time, so jitter in the network no longer shows up as jitter in a
puppet's motion. A client that sends a constant or random stamp only makes its own samples look old or young (within
the 300 ms clamp); it cannot affect anyone else's.

The **kinematics** (16 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `u64` position: `x` in bits 0..21, `y` in bits 22..43, `z` in bits 44..63; each a two's-complement signed integer in centimetres (22, 22 and 20 bits) |
| 8 | 2 | `u16` heading, 65536 steps per turn |
| 10 | 2 | `i16` vx, 1/32 m/s |
| 12 | 2 | `i16` vy |
| 14 | 2 | `i16` vz |

The **ped block** (v3; v4: 8 bytes, 14 while aiming; v5: 9 / 15 with a vehicle) is what the player's ped is doing (`PedState`):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | move byte: bits 0..2 = `move` (`kMove*`, 0..6), bits 3..4 = `jumpSeq`, bits 5..7 must be 0 |
| 1 | 2 | `u16` flags, a `kPf*` bit set; all 16 bits are defined (0.4.3: bit 15 is `kPfVaulting`; v5: bit 15 must be 0; v4: bits 13..15) |
| 3 | 4 | `u32` weapon: the selected weapon's GTA hash (0 = not reported) |
| 7 | 1 | v4: shot byte: bits 0..3 = `shotSeq`, bits 4..7 must be 0 |
| 8 | 1 | v5: only when `move` is 5 (entering) or 6 (exiting): `u8` vehId, the vehicle being entered or left (below 256); the byte is mandatory for those two moves and illegal after any other |
| 8 or 9 | 6 | only when flag `kPfAiming` is set: `i16` ax, ay, az, the aim point minus the player's position in decimetres (section 5), each within +-32767 |

The kinematics followed by the ped block (24 bytes, 30 while aiming; v5: 25 / 31 entering or exiting) is what
`PlayerState` carries. A `WorldState` entry adds the **vitals** (v4): `u8` health and `u8` armour, the **server's**
values (health 0..255, `playerHealth` by default 200 in v5 (100 in v4); GTA ped health is 100 more than that; armour
0..255). The three together are the **absolute state** (26 bytes, 32 while aiming; v5: 27 / 33 with a vehicle move),
the body of an undeltaed `WorldState` entry. In a `WorldState` entry `kPfDead` is set when the server has the player
dead **or** the player's own client reports its ped dead (display only: hits and respawns use the server's state).

`shotSeq` (v4) is a 4-bit counter the client increments at every shot it fires (`++` wraps); a receiver that sees it
change plays a shot even when the packet with `kPfShooting` was lost, and a `Hit` names the shot it came from. The
server uses the same counter to bind Hits to real shots (section 4, `Hit`): a client must send the state that carries
a shot's new `shotSeq` (with the weapon in `PedState.weapon`) no later than the `Hit` that names it; one that arrives
a little behind it is held for up to 300 ms. After 16 shots the counter wraps, so a sustained burst cannot be told
from a longer one by the counter alone.

| `move` | Name | | `kPf` bit | Name | Meaning |
|---|---|---|---|---|---|
| 0 | `kMoveIdle` | | 0 | `kPfJumping` | a jump is in progress |
| 1 | `kMoveWalk` | | 1 | `kPfRagdoll` | |
| 2 | `kMoveRun` | | 2 | `kPfAiming` | the aim point travels |
| 3 | `kMoveSprint` | | 3 | `kPfShooting` | |
| 4 | `kMoveInVehicle` | | 4 | `kPfReloading` | |
| 5 | `kMoveEnteringVehicle` | | 5 | `kPfFalling` | |
| 6 | `kMoveExitingVehicle` | | 6 | `kPfParachute` | freefall or parachuting |
| | | | 7 | `kPfClimbing` | `IS_PED_CLIMBING`: a climb up a wall or ledge (a vault over an obstacle is `kPfVaulting`) |
| | | | 8 | `kPfLadder` | |
| | | | 9 | `kPfCover` | |
| | | | 10 | `kPfDead` | |
| | | | 11 | `kPfStealth` | crouched / stealth movement |
| | | | 12 | `kPfDriver` | in a driver seat (only with `move` 4..6) |
| | | | 13 | `kPfSwimming` | v5: swimming (surface or diving) |
| | | | 14 | `kPfMelee` | v5: a melee action (a punch, a swing) is in progress |
| | | | 15 | `kPfVaulting` | 0.4.3: `IS_PED_VAULTING`, a vault or slide over an obstacle |

**`kPfVaulting` (0.4.3 fixes, no version bump).** Bit 15 was the last reserved flag bit. It was added to protocol v5 without
changing `kVersion`: client and server ship together (updates are mandatory and lockstep), so an old peer that would refuse
the bit cannot meet a new one. Every bit of the `u16` is now known (`kPfKnown` = `0xFFFF`); a further flag needs a new field
(protocol v6). The receiver treats `kPfClimbing` and `kPfVaulting` alike: one `TASK_CLIMB` on the rising edge, no position
correction or velocity push until the flag has fallen and settled (`sync::ClimbWindow`, `libs/sync/pedstate.h`).

`jumpSeq` is a counter the client increments at every jump start; only its low 2 bits travel, so a plain `++` wraps.
A receiver that sees it change plays a jump even when the packet that carried `kPfJumping` was lost. An aim offset
given without `kPfAiming` is not sent (the block stays 8 bytes) and decodes as 0.

The server additionally drops a `PlayerState` outside the world box or speed limit (`ValidKinematics`: x, y within
+-16000 m, z within -2000..3000 m, speed at most 600 m/s per axis), or whose ped state is inconsistent
(`ValidPedState`: `kPfDriver` with `move` below 4). Such a packet is dropped and logged (counted in `statesInvalid`),
never fatal. A malformed one (a reserved bit in the move byte, `move` 7, a truncated aim, a move 5 or 6 without its `vehId` byte or a
`vehId` byte after any other move) fails `Decode` and is dropped and logged as malformed, also never fatal.

### 7 `WorldState` (server to client, state channel)

The players relevant to this client at one server tick, as deltas where possible.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type `07` |
| 1 | 4 | `u32` tick, at least 1 (server ticks are numbered from 1; 0 means "nothing received" in an ack) |
| 5 | 1 | `u8` nLeft |
| 6 | 2 x nLeft | `u16` ids that stopped being relevant (each below 1024) |
| ... | 1 to 2 | `varu32` count of entries (at most 1024) |
| ... | | `count` entries, ascending by id |

One entry:

| Size | Field |
|---|---|
| 1 to 2 | `varu32` idGap: the id is `previousId + 1 + idGap` (`previousId` starts at -1); the id must be below 1024 |
| 1 | `u8` flags: bits 0..4 = baseAge (0 = absolute, 1..31 = delta), bits 5..7 = change bits (bit 5 position, bit 6 heading, bit 7 velocity) |
| 1 | `u8` age: milliseconds between the server receiving this sample and building this packet, saturating at 255 |
| 26 or 32 | baseAge 0: the absolute state (kinematics + ped block + vitals, section 4.6; v5: one byte more, the `vehId`, for a move 5 or 6). The change bits must be 0; there is no extension byte |
| 1 | baseAge 1..31: `u8` extension byte (v3): bit 0 move byte, bit 1 flags, bit 2 weapon, bit 3 aim, (v4) bit 4 shot byte, bit 5 vitals, (v5) bit 6 vehicle id; bit 7 must be 0 |
| var | baseAge 1..31: only the flagged fields, in this order. Position: `vari32` dx, dy, dz in cm. Heading: `vari32` dh in 1/65536 turn, wrapping, within -32768..32767. Velocity: `vari32` dvx, dvy, dvz in 1/32 m/s. Move byte: `u8`, the new value (same rules as the ped block). Vehicle id (v5): `u8`, the new value; legal only when the RESULTING move is 5 or 6 and written only when it differs from the baseline's (0 when the baseline's move named no vehicle), so entering a vehicle costs one byte once. Flags: `u16`, the new value. Weapon: `u32`, the new value. Shot byte: `u8`, the new value. Aim: `vari32` dax, day, daz in dm against the baseline's aim. Vitals: `u8` health, `u8` armour, the new values. Fields not flagged are copied from the baseline |

The aim follows the **resulting** flags: when the entry's flags (new or copied) lack `kPfAiming`, the aim is 0 and an
aim bit makes the frame invalid; when they have it, the aim bit carries deltas against the baseline's aim (0 if the
baseline was not aiming) and a clear aim bit keeps the baseline's aim. Every resulting aim axis must stay within
+-32767. So stopping to aim costs only the flags (3 bytes), and a ped that never aims never sends an aim.

A delta entry with nothing changed is 4 bytes (id gap, header, age, extension `00`): the player has not moved or
changed at all since the baseline. An absolute entry is 29 bytes (35 while aiming, one more when the id gap needs two
varint bytes); a delta whose payload (extension byte plus fields) would cost as much as the absolute payload (26 or 32
bytes) or more is sent absolute instead (so a pure-kinematics teleport of 24 bytes is now a delta). Measured on the
quant test's walking and driving paths, a delta entry averages about 11.8 bytes for a walker and 13.6 for a driver
(the shot byte and the vitals cost nothing while they do not change).

### 8 `ChatSend` (client to server, reliable)

type `08`, `str` text, 1..200 bytes of well-formed UTF-8 without control characters.

### 9 `ChatMessage` (server to client, reliable)

type `09`, `u16` fromId (`0xFFFF` = `kServerId`, a server notice), `str` text.

### 10 `WorldCoarse` (server to client, state channel)

See section 7. type `0A`, `u32` tick, `varu32` count, then `count` entries of `varu32` idGap + `u16` x + `u16` y + `u16` z.

### 11-13 `ResourceManifest`, `ResourceRequest`, `ResourceChunk` (reliable)

The server-script resource download (v2 numbers, layouts in `libs/protocol/protocol.h`): manifest `u8` count then
`str` name + `u32` size + `u32` crc32 each; request `u8` index + `u32` startChunk; chunk `u8` index + `u32` chunkIndex +
`u16` length + at most 1024 bytes. The current client ignores 11 and 13; the current server does not serve them and
answers a 12 with `Refuse(Protocol)` (unexpected message).

### 14 `VehicleWorld` (server to client, state channel)

v4: per receiver, every world tick while it has anything to say. The vehicles relevant to this client (within the
player relevance radii, `relevanceEnterRadius` / `relevanceLeaveRadius`, plus the one it sits in), as a complete
**presence list** with states for those due.

| Size | Field |
|---|---|
| 1 | type `0E` |
| 4 | `u32` tick |
| 1 | `u8` nRemoved |
| nRemoved | `u8` ids of vehicles that were DESTROYED (despawned), strictly ascending; each is repeated in 3 frames |
| 2 | `u16` count, at most 256 |
| | `count` entries, strictly ascending by id: `u8` id, `u8` header (0 presence only, 1 a full state follows, v5: 2 a motion-only state follows; 3 and above are refused), then the state when the header says so |

A **full state** (v6: 46 bytes, plus 2 per occupant; v5: 43; v4: 36):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `u32` model, the GTA vehicle model hash |
| 4 | 22 | the **motion**: `u64` position (as the kinematics, cm 22/22/20 bits), `i16` vx, vy, vz (1/32 m/s), `u32` rotation (smallest three, section 5), `i8` angular velocity x, y, z (1/16 rad/s), `i8` steering (-1..1 in 1/127 steps, as the game reports it) |
| 26 | 1 | `u8` flags: bit 0 `kVehLocked`, 1 `kVehLights`, 2 `kVehEngine`, 3 `kVehSiren`, 4 `kVehHorn`; bits 5..7 must be 0 |
| 27 | 2 | `u16` body health, 0..1000 |
| 29 | 2 | `u16` engine health, 0..1000 |
| 31 | 2 | `u16` fuel permille, 0..1000 |
| 33 | 2 | `u8` primary colour, `u8` secondary colour (GTA colour indices) |
| 35 | 1 | v5: `u8` ageMs, how old the motion is: the frame's tick time minus the time the controller sampled it, 0..255 (saturating; never negative) |
| 36 | 2 | v5: `u16` ctrl, the player whose client controls the vehicle right now (the only one whose `VehicleUpdate` the server takes), `0xFFFF` = nobody (then the vehicle stands still) |
| 38 | 2 | v5: `u16` ownerId, the player who owns the vehicle (spawned it and has not released it), `0xFFFF` = nobody; either is below 1024 or `0xFFFF` |
| 40 | 1 | v5: `u8` flags2: bit 0 `kVeh2Headlights`, 1 `kVeh2HighBeam`, 2 `kVeh2Interior`, 3 `kVeh2IndicatorL`, 4 `kVeh2IndicatorR`, 5 `kVeh2RoofDown`; v6: 6 `kVeh2Brake`, 7 `kVeh2Handbrake` (no bit is reserved any more) |
| 41 | 1 | v5: `u8` doors, the open doors: bit 0 front left, 1 front right, 2 rear left, 3 rear right, 4 hood, 5 trunk (`kDoor*`); bits 6..7 must be 0 |
| 42 | 1 | v6: `u8` tyres, the burst tyres: bit i = wheel slot i (0..7) is burst (section 16) |
| 43 | 1 | v6: `u8` tyresRim, the tyres completely burst (off the rim); must be a subset of `tyres` |
| 44 | 1 | v6: `u8` windows, the smashed windows: bit i = window slot i |
| 45 | 1 | `u8` seat mask: bit i = seat i is occupied (seat 0 is the driver) |
| 46 | 2 x n | `u16` player id per set bit, in seat order (each below 1024) |

A **motion-only state** (v5, header 2): `u8` ageMs, the 22-byte motion, `u8` flags (the complete `kVeh*` set, as above):
24 bytes after the id and header (26 with them; a full entry is 45 or more). It leaves out the model, health, fuel,
colours, seats, ctrl, owner, flags2, doors and (v6) tyres, tyresRim and windows: the receiver keeps what it holds for them. The server sends a full entry
for a vehicle that is new to the client, for 3 frames after any of those fields changed, and about once a second
(`refreshTicks`, 20 ticks) otherwise, and the motion-only entry in between, so a driven car costs about a third less.
A client that has no full state yet for a motion-only entry (its first one was lost) waits for the next full one.

How a client applies a frame: first every `removed` id is deleted (the vehicle is gone); then any vehicle it holds
that is not in the presence list is streamed out (it still exists, it is only out of range); then every full state
replaces what it had and every motion-only state replaces the motion and the flags of what it had. A present vehicle
without a state keeps its last one. Every frame is complete, so a lost one is repaired by the next. A vehicle's
`heading` (in the decoded `Kinematics`) is the rotation's yaw. A client places a motion at `tickTime - ageMs`, the
same way as the `age` of a `WorldState` entry.

When states are sent: a vehicle new to this client, or one whose server-owned fields, occupants, controller, owner or
parking changed, in each of the next 3 frames; a driven vehicle (somebody in seat 0, or one that moved in the last
second, other than the vehicle's own controller) at the distance rate of section 6 (`rateNearHz` within `nearDistance`,
`rateMidHz` within `midDistance`, `rateFarHz` beyond); a parked one, and the vehicle its own controller drives (the
controller is the source of that motion), about once a second. States are chosen changed-first, then nearest-first,
within `worldPacketMaxBytes`; the rest stay due. With nothing present, the empty set is said in 3 frames and then about
once a second while any vehicle exists; a server without vehicles sends no `VehicleWorld` at all.

**Who controls a vehicle** (v5; the controller sends its `VehicleUpdate`s, section 18). Decided once per world tick for
every vehicle: the player in seat 0 if there is one. Otherwise the current controller keeps it while it is still on the
server, has sent a position, is not in an untrusted window and is within 250 m of the vehicle, unless it has been silent
(no accepted update) for 1.5 s, in which case it is skipped for 10 s. Otherwise the nearest player who has a position, is
trusted, has the vehicle in view (the vehicle is in its `VehicleWorld` presence list), is not skipped and is within 200 m
takes over. With nobody eligible the vehicle has no controller and stands still (velocities, angular velocity, steering
and horn cleared). A driver who gets out stays the controller until it is 250 m away or silent.

### 15 `VehicleCmd` (client to server, reliable)

type `0F`, `u16` vehId, `u8` op, `u32` arg; op 8 (`Register`) adds a tail. Every `VehicleCmd` is answered with one
`VehicleResult`; a malformed one (v5: an op of 0 or over 9) is `Refuse(Protocol)`.

| op | Name | arg | Who / rule |
|---|---|---|---|
| 1 | `Spawn` | model hash (not 0) | needs a server-known position (`NoState`); placed 4 m ahead of the requester, facing its heading; caps below |
| 2 | `Despawn` | | its spawner only (occupants are put out) |
| 3 | `Enter` | seat 0..7 (0 = driver) | seat free, vehicle within `vehicleEnterDistance` (10 m) of the requester's last position, not locked (its spawner may still enter); a player sitting elsewhere leaves that seat first; a seat change inside the same vehicle skips the distance and lock checks. v5: the driver's seat (0) of an **owned** vehicle is its owner's alone: anybody else gets `NotOwner`, checked before everything else about the seat (so GTA's seat shuffle into an empty driver's seat, and a seat somebody sits in, are `NotOwner` too, and the client knows why and does not retry); the other seats are open to everybody |
| 4 | `Exit` | | the vehicle the requester sits in; a driver's vehicle **parks** (velocities, steering and horn cleared) |
| 5 | `SetLocked` | non-zero = locked | its driver or spawner |
| 6 | `SetLights` | non-zero = on | its driver or spawner |
| 7 | `SetColours` | primary \| secondary << 8 | its driver or spawner |
| 8 | `Register` | model hash | adopt a local (ambient) car: the tail is `u64` position (cm), `u32` rotation, `u8` colour1, `u8` colour2, `u8` seat (16 bytes; 23-byte message). Within `vehicleEnterDistance` of the requester; the server creates the vehicle there (adopted, the requester its spawner) and seats the requester. An adopted vehicle is not owned |
| 9 | `Release` | | v5: its owner gives the vehicle up; from then on anybody may take the driver's seat. `NotAllowed` for anybody else and for a vehicle that is not owned |

**Ownership** (v5): a vehicle is owned by the player who `Spawn`ed it (`VehicleState.ownerId`); `Register`ed (adopted) vehicles,
vehicles whose owner left the server while somebody sat in them (orphans) and vehicles after a `Release` are not owned
(`ownerId` `0xFFFF`). Ownership is only about the driver's seat; `SetLocked`, `SetLights`, `SetColours` and `Despawn`
keep their rules.

Caps: at most `vehiclesPerPlayer` (3) spawned and, separately, 3 adopted per player, and `maxVehicles` (64, at most
256) in all (`Limit`). A player at its adopted cap gives up its oldest **empty** adopted vehicle instead. Spawn and
Register share a bucket of 3 at once, then 1 a second; all `VehicleCmd`s together 10 a second (`RateLimited`). A
vehicle nobody sits in is destroyed after `vehicleEmptyDespawnSeconds` (600; 0 = never). A player who leaves the
server is put out of its seat and its EMPTY vehicles are destroyed at once; one somebody else still sits in becomes an
**orphan** (no spawner, still counted against the source address, `vehiclesPerIp` 6) that is destroyed
`vehicleOrphanSeconds` (60) after it empties. Dead players cannot spawn, register or enter (`Dead`).

Server-side rules that change no layout (docs/api/server.md "Anti-cheat rules"): a player whose position just jumped
farther than it could move (`untrustedWindowMs`, 3 s) gets `RateLimited` for `Spawn`, `Enter` and `Register`; `Register`
is `BadArg` within `registerSeparationMeters` (4 m) of a networked vehicle and `RateLimited` within
`registerCooldownMs` (2 s) of the previous one; a model on the operator's deny list (or outside its allow list) is
`NotAllowed`; a `Spawn` or `Register` refused for any reason never costs the player an existing vehicle. **Every
`RateLimited` result is sent at most once a second per player; the other commands in that second get no
`VehicleResult` at all**, so a client must time out a command it hears nothing about. Sustained flooding is
`Refuse(Protocol)`.

### 16 `PlayerLook` (client to server, reliable): 86 bytes

The sender's appearance: type `10`, then the **look** (85 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `u32` model, the GTA ped model hash (not 0) |
| 4 | 12 x 4 | components 0..11: `u16` drawable (0..1023), `u8` texture (0..63), `u8` palette (0..3) |
| 52 | 8 x 3 | props 0..7: `i16` drawable (-1 = none, else 0..1023), `u8` texture (0..63; 0 when the drawable is -1) |
| 76 | 9 | head blend, `u8` each: shape1, shape2, shape3, skin1, skin2, skin3 (parent ids 0..63), shapeMix, skinMix, thirdMix (0..255 for GTA's 0.0..1.0) |

The fields map onto `SET_PLAYER_MODEL`, `SET_PED_COMPONENT_VARIATION`, `SET_PED_PROP_INDEX` and
`SET_PED_HEAD_BLEND_DATA`. A look outside these ranges fails `ValidLook`: the server drops it and logs `PlayerLook
rejected` (counted in `looksInvalid`), the connection stays. A truncated or over-long `PlayerLook` is malformed
(`Refuse(Protocol)`).

The client sends its look whenever it changes; `Session::SendLook` skips a look equal to the last one sent on this
connection. The server keeps the newest valid look per player and publishes it as `PeerLook` to everybody else at most
2 per second per player (a token bucket, burst 2). A look that arrives over the rate waits (`looksDeferred`); a newer
one replaces the waiting one (`looksCoalesced`), so the newest look always wins and goes out as soon as a token is back
(checked every world tick). A look equal to the one already published (or waiting) is not re-sent (`looksUnchanged`).

### 17 `PeerLook` (server to client, reliable): 88 bytes

type `11`, `u16` id (below 1024), then the look (85 bytes, as above). Sent to every other client when a player's
published look changes, and to a newly accepted client once for every player that has a look, right after its
`PlayerJoined` burst (`looksReplayed`). The sender never receives its own. The client checks `ValidLook` and drops (counts
as malformed) a look that fails it; a `PeerLook` before the client's own `Welcome` is ignored.

### 18 `VehicleUpdate` (client to server, state channel): 40 bytes (v6; 37 in v5)

The controlled vehicle, every send tick: type `12`, `u32` seq, `u8` vehId, the 22-byte motion, `u8` flags, `u16`
body health, `u16` engine health, (v5) `u16` sampleMs, `u8` flags2, `u8` doors, (v6) `u8` tyres, `u8` tyresRim, `u8` windows. `sampleMs` is the sender's millisecond
clock when the motion was sampled, exactly as in `PlayerState` (section 6; its own clock on the server, one per
connection and message stream); `flags2` and `doors` are the `kVeh2*` and `kDoor*` sets of section 14 (`doors` bits 6..7
refused); the three v6 masks are what the controller's own car shows (section 16: a rim bit without its tyre bit is refused). Taken only from the vehicle's **controller** (v4: the player the server has in seat 0 of `vehId`; v5: the
player named in `VehicleState.ctrl`, which is the driver, or for a driverless vehicle the nearby player the server
picked, section 14), with a seq newer
than the last accepted `VehicleUpdate` from that player, inside the world box (`ValidVehicleUpdate`: finite, the
`PlayerState` box, a rotation of non-zero length, steering within +-1) and at most `vehicleMaxSpeed` (150 m/s), from a living player. The
server copies position, velocity, rotation, angular velocity, steering and the driver flags (`kVehEngine`, `kVehSiren`,
`kVehHorn`; lock and lights in `flags` are ignored, they are server-owned), and (v5) `flags2` and `doors`, and (v6) the visual masks, merged with the
Hits (section 16); a health can only go down this way (a crash). Anything else is dropped and counted (`vehicleUpdatesRejected` / `Invalid` / `Stale`), never fatal. With fuel 0
the engine stays off. The displacement check (`untrustedWindowMs`) runs on the server's receipt times, not on `sampleMs`, so a
forged stamp cannot hide a jump.

### 19 `VehicleResult` (server to client, reliable): 5 bytes

type `13`, `u8` op (as sent), `u8` code, `u16` vehId (the new id for an `Ok` Spawn or Register).

| code | Name | | code | Name |
|---|---|---|---|---|
| 0 | `Ok` | | 6 | `Limit` |
| 1 | `NoVehicle` | | 7 | `RateLimited` |
| 2 | `SeatTaken` | | 8 | `BadArg` (model 0, seat over 7, a bad position) |
| 3 | `TooFar` | | 9 | `Dead` |
| 4 | `Locked` | | 10 | `NotInVehicle` |
| 5 | `NotAllowed` (not the driver / spawner) | | 11 | `NoState` (send a `PlayerState` first) |
| | | | 12 | `NotOwner` (v5: the driver's seat of somebody else's vehicle) |

### 20 `Hit` (client to server, reliable): 13 bytes

The shooter's claim that its shot hit: type `14`, `u8` kind (1 player, 2 vehicle), `u16` targetId, `u32` weapon hash
(or the cause), `u16` damage (GTA health units), `i16` bone (-1 unknown; carried, not checked), `u8` shotSeq (the
shooter's `PedState.shotSeq` of that shot). The server owns health and checks: the shooter is alive and has a
position, the target exists, is alive and has a position, is within `maxHitDistance` (1000 m) and is in the shooter's
relevance set (a player it is being sent, or a vehicle in its `VehicleWorld`), the shooter is under `hitsPerSecond`
(30, a bucket of the same burst), damage is not 0. Damage is clamped to 250. A refused `Hit` is dropped, counted
(`hitsRejected`) and nobody is told. A hit on oneself (targetId = own id) is **environmental** damage (a fall, a fire,
a crash): no distance or relevance check, the same rate and clamp, and it can only hurt the sender.

Hits against others are also **bound to real shots** (server rules, no layout change; the numbers are in
docs/api/server.md "Anti-cheat rules"): a firearm Hit needs a change of the shooter's `shotSeq` that the server saw in a
`PlayerState` within the last 500 ms (`hitShotWindowMs`), the Hit's `weapon` reported in a state in the last 500 ms,
and no more Hits on that shot than the weapon class allows (2 for handguns, SMGs and rifles, 3 for machine guns, 4
for snipers, 8 for shotguns); a melee Hit (v5) needs a **swing** (a change of `shotSeq` under the melee weapon, the
same rule as a shot, so the client counts a punch or a swing as a shot), at most 2 Hits per swing, the weapon reported
and the target within 4 m (7 m to a vehicle; v4 needed no swing, so anybody within 4 m could claim 300 damage a second); a
firearm used as a club (a Hit with no shot of its own) needs the weapon reported and the same reach; a
grenade or rocket needs a launch (a `shotSeq` change under an explosive weapon) in the last 15 s; a run-over
(`WEAPON_RUN_OVER_BY_CAR`) needs the shooter to be the driver of a vehicle that is moving and within 12 m of the
target; an environmental cause (fall, fire, explosion, drowning, ...) is valid only as a self-hit. Damage is also
capped per weapon class (clamped, not refused) and by a per-class damage budget per second. A Hit that fails only for a
shot or weapon that has not arrived yet is held up to 300 ms. A shooter whose position just jumped (the untrusted
window) has its Hits on others refused; Hits on it still land.

A player hit takes the damage from armour first, then from health. A vehicle hit takes it off the body health and
half of it off the engine health (seen by everybody in `VehicleWorld`). v6: a vehicle Hit may also name a tyre or window in
`bone` (section 16).

**Damage scaling** (v5, `apps/server/damage.h`): the number the shooter's client reports is the single-player game's
damage, without Online's freemode factor, so the server scales what it applies to a player (after the clamp to 250, the
class cap and the damage budget, which stay in the client's units): firearms (handgun, SMG, rifle, machine gun, shotgun,
sniper, and a weapon the server does not know) by `firearmDamagePercent` (72), melee by `meleeDamagePercent` (50),
explosives by `explosiveDamagePercent` (100), run-over by `runOverDamagePercent` (100); the result is rounded to the nearest
point, at least 1. Environmental damage (a self-hit) is never scaled. A firearm Hit whose `bone` is the head bone (31086,
`SKEL_Head`) additionally multiplies by `headshotPercent` (300) at up to 5 m, falling linearly to x1 at 40 m (300 m for
the sniper class); only the first Hit credited to a shot can be a headshot (a shotgun's pellets are not all heads), and
fists, blades, explosives and run-overs never are. With `playerHealth` 200 that makes a carbine (32) take 9 hits, a pistol
(26) 11, fists (30) 14 and a carbine headshot at 3 m (69) 3 hits. A vehicle takes the unscaled number. Every percent at
100 (and `headshotPercent` 100) is the v4 behaviour.

### 21 `Damage` (server to client, reliable): 17 bytes

To the victim, to the attacker (once when they are the same) and (v5) to every other player for whom the victim or the
attacker is in the relevance set (section 6) when the hit did damage or killed (a console `heal` or `armor` goes to the
victim alone): type `15`, `u16` victimId, `u16` attackerId (`0xFFFF` = the server: console `heal`, `kill`, `armor`),
`u32` weapon, `u16` damage (what was taken: armour plus health), `u8` health and `u8` armour (the victim's new values),
`u8` killed (0 or 1), (v5) `i16` bone (the bone the shooter reported, -1 unknown) and `u8` flags: bit 0 `kDmgHeadshot` (the
head-bone bonus was given), 1 `kDmgMelee` (the hit was judged as melee, a gun used as a club included), 2
`kDmgExplosion`, 3 `kDmgArmour` (armour took part of it); bits 4..7 are refused. A third player uses the message for hit
reactions and the kill feed, and ignores its health fields (its own health comes in no `Damage` unless it is the victim).

### 22 `Killed` (server to client, reliable): 10 bytes

To everybody (a kill feed): type `16`, `u16` victimId, `u16` killerId (the victim's own id for environmental death,
`0xFFFF` for the console's `kill`), `u32` weapon, (v5) `u8` flags: bit 0 `kKilledHeadshot`, 1 `kKilledMelee` (those of
the killing blow's `Damage` flags); bits 2..7 are refused. A dead player is put out of its vehicle and its looping animation
is stopped (`PeerAnimStop`).

### 23 `Respawn` (client to server, reliable): 1 byte

type `17`, no body. Honoured only while dead and `respawnDelaySeconds` (5) after the death; otherwise ignored and
counted (`respawnsRefused`): send it again later.

### 24 `RespawnOk` (server to client, reliable): 19 bytes

type `18`, `f32` x, y, z (metres), `f32` heading (degrees), `u8` health (`playerHealth`, v5 default 200), `u8` armour (`respawnArmour`, 0). The
spawn points come from `spawnPoints` in server.json, used in turn (default: Legion Square, 195.2, -933.8, 30.7, 140).

### 25-27 `PlayAnim`, `PlayScenario`, `StopAnim` (client to server, reliable)

Emotes, for `TASK_PLAY_ANIM` / `TASK_START_SCENARIO_IN_PLACE` on the other clients. The **anim** body: `str` dict,
`str` name (each 1..63 bytes of `A-Z a-z 0-9 _ @ / . -`, `ValidAnimName`), `f32` blendIn, `f32` blendOut (finite,
within +-1000; defaults 8 / -8), `i32` durationMs (-1 = loop until stopped, else 0 or more), `u32` GTA animation flags,
`u8` playbackRate (x100: 100 = normal speed).

* `PlayAnim` type `19` + anim. `PlayScenario` type `1A` + `str` name. `StopAnim` type `1B`, no body.
* A string over 63 bytes, a truncated or trailing body is malformed (`Refuse(Protocol)`). A name outside the charset,
  a blend out of range or a duration below -1 is dropped and counted (`animsInvalid`); the connection stays.
* At most 4 at once, then 4 a second per player for the three together (`animsRateLimited`). A dead player's
  `PlayAnim` and `PlayScenario` are dropped (`animsDeadDropped`).
* The server reduces `flags` to bits 0..8 (the ones that only change how the clip plays: loop, hold last frame,
  reposition, not interruptable, upper body only, secondary slot, reorient, abort on movement, additive) and clamps
  `playbackRate` to 10..200, in the relay and in the replay (`animsSanitized`). The wire format is unchanged: the
  decoder still carries all 32 bits; a client that wants a flag above bit 8 cannot have it.
* The server relays each to everybody else. It keeps the player's current **loop** (durationMs -1) or **scenario**
  and replays it to a newcomer right after the `PeerLook` burst (`animsReplayed`); a one-shot animation, `StopAnim`,
  a death and a respawn clear it.

### 28-30 `PeerAnim`, `PeerScenario`, `PeerAnimStop` (server to client, reliable)

`PeerAnim` type `1C`, `u16` id, anim. `PeerScenario` type `1D`, `u16` id, `str` name. `PeerAnimStop` type `1E`, `u16`
id. The id is below 1024 and always follows its `PlayerJoined`; the client drops (counts as malformed) a `PeerAnim` /
`PeerScenario` whose names fail `ValidAnim` / `ValidAnimName`.

## 5. Quantisation

Positions, headings and velocities travel as integers. The floats on both ends are the integers divided back.

| Field | Wire | Encode | Decode | Error |
|---|---|---|---|---|
| position x, y | signed, 22 bits | `floor(m * 100 + 0.5)` | `q / 100` | 0.5 cm of rounding plus the float's own precision (0.05 cm at the map edge): at most **0.55 cm per axis** over the whole box, under 0.52 cm on foot (the worst of 2M random samples was 0.5005 cm) |
| position z | signed, 20 bits | same | same | same |
| heading | `u16` | `floor(fmod(deg, 360) * 65536 / 360 + 0.5) mod 65536` | `q * 360 / 65536` | at most 0.00275 degrees (plus a float ulp: 0.0028 degrees) |
| velocity | `i16` | `floor(mps * 32 + 0.5)` | `q / 32` | at most 1/64 m/s per axis |
| coarse x, y | `u16` | `floor((m + 16384) / 0.5 + 0.5)` | `q * 0.5 - 16384` | at most 0.25 m |
| coarse z | `u16` | `floor((m + 2048) / (5/64) + 0.5)` | `q * 5/64 - 2048` | at most 0.04 m |
| aim offset (v3) | `i16` per axis | `floor(m * 10 + 0.5)` after scaling (below) | `q / 10` | at most 5 cm per axis within +-3276.7 m (under 0.1 degree at 30 m; unchanged in v4, enough for hit display) |
| vehicle rotation (v4) | `u32` smallest three | below | below | at most **0.28 degrees** (analytic worst case; 0.23 measured over a million random rotations) |
| angular velocity (v4) | `i8` per axis | `floor(rad/s * 16 + 0.5)`, saturating at +-127 | `q / 16` | at most 1/32 rad/s within +-7.9 rad/s |
| steering (v4) | `i8` | `floor(s * 127 + 0.5)`, saturating at +-127 | `q / 127` | at most 1/254 |

Rounding is to nearest, half up, in double precision, so every platform produces the same integer.

The **quantiser box** is the range a field can take on the wire (anything outside is refused by `Decode`):

| Field | Range |
|---|---|
| x, y | -16384 m .. +16384 m (+-1 638 400 cm) |
| z | -2048 m .. +3072 m (-204 800 .. +307 200 cm) |
| velocity per axis | +-1000 m/s (+-32 000) |
| heading | any `u16`; encoding wraps into [0, 360) |

It is larger than the world box the server's policy accepts (section 4, `PlayerState`), so a position just outside the
world still round-trips and is refused by policy, not by the codec. The encoder saturates to the box (a NaN field encodes
as 0, +-Inf saturates; `Encode(PlayerState)` refuses a non-finite state outright). The top cell of the coarse channel
saturates: within the last half step below +16384 m (x, y) or +3072 m (z) the coarse error is up to one step.

The **aim offset** is quantised only while `kPfAiming` is set (otherwise it is 0). An offset whose largest axis exceeds
3276.7 m is first scaled down along its own direction until that axis is exactly 32767 dm, so the aim direction
survives a far target. A NaN axis is refused by `Encode(PlayerState)`.

The **rotation** (v4) is a unit quaternion in "smallest three" form. Of its four components (x, y, z, w) one is
dropped and rebuilt as `sqrt(1 - a^2 - b^2 - c^2)` (made non-negative first: q and -q are the same rotation); the
other three lie within +-1/sqrt2 and travel as 10 bits each, `code = floor(c * sqrt2 * 511 + 0.5) + 511` (0..1022;
1023 is refused, and so are three codes whose squares sum past 1). Layout: bits 30..31 the dropped component's index
(0 x, 1 y, 2 z, 3 w), bits 20..29, 10..19, 0..9 the other three in x, y, z, w order. The encoder tries all four drop
choices and keeps the valid code that decodes closest (the largest component first, which wins a tie), so decoding
and re-encoding gives the same rotation: the server's relay adds no second rounding. The identity is `0xDFF7FDFF`,
exact. A zero-length or non-finite rotation encodes as the identity (`Encode` refuses a non-finite one outright).

Proven in `tests/quant_test.cpp`: every centimetre of the box survives integer to float and back unchanged
(exhaustive), the error bounds above hold over millions of random samples, and the range check refuses every field one
step past its edge.

## 6. WorldState: relevance, rates and deltas

### What is sent

The server does not broadcast the whole world. Per client it keeps a **relevance set** of the other players and sends
those at full detail:

* A player **enters** the set when it comes within `enter = min(maxRadius, enterRadius + lookahead * (speedA + speedB))`
  metres of the client (defaults: 400 m, +1 s of the pair's combined speed, capped at 600 m) and **leaves** it beyond
  `enter + (leaveRadius - enterRadius)` (+50 m of hysteresis, so nobody flickers at the edge). A fast approach is
  noticed early: at a combined 60 m/s the radius is 460 m, and the enter test runs every 100 ms, so a 60 m/s
  approach is first sent at about 455 m (`tests/interest_test.cpp` measures 455 m for 60+0 and 0+60, 518 m for 60+60 and
  590 m for 300+300 m/s).
* A gamemode can pin players into a client's set whatever the distance (`Server::SetAlwaysRelevant`); they are sent at
  the far rate and never leave.
* A player that stops being relevant is listed in the `left` ids of the next three `WorldState` packets (the repeat covers
  a lost packet), and from then on arrives as a coarse blip (section 7). A client treats `left` as "stop expecting
  detail updates for this id"; the roster (`PlayerJoined` / `PlayerLeft`) alone says who is online.
* A client that has not sent a `PlayerState` yet is sent coarse blips only; a player that has not sent one yet is not
  sent to anybody.

### How often

Each relevant pair accumulates priority at the rate of its distance class and is sent when it reaches 1:

| Class | Rule (defaults) | Rate |
|---|---|---|
| near | distance - 0.5 s x combined speed < 120 m | `rateNearHz` (default 30 Hz = `tickHz`; the defaults were 20 / 10 / 5 Hz before sync plan W7) |
| mid | below 250 m | 15 Hz (every 2nd tick) |
| far | beyond | 10 Hz (every 3rd tick) |
| idle | the player's quantised state (kinematics and ped block) has not changed for 500 ms | at most 1 Hz; the first change after idling, a ped-only one included (drawing a weapon, starting to aim), is sent on that very tick |

A per-client **byte budget** (default 36 000 bytes/s, a token bucket refilled every tick) bounds what the server sends
one client; updates over it wait and are served by accumulated priority, longest-waiting first, so every player's turn
comes. A packet is at most `worldPacketMaxBytes`. `docs/api/server.md` lists every setting.

### Delta baselines under loss

The `WorldState` channel is unreliable, so a delta must never depend on a frame the client may not have. The scheme:

* Both sides keep, per player id, the last 32 ticks of the state they sent / decoded for that id. An entry exists in
  that history for tick T only if the `WorldState` of tick T carried that player.
* Every `PlayerState` carries the receipt (`ack tick`, `ack bits`). The server may use "player P as sent in tick
  T - k" as the baseline of P in tick T only when it sent P in that frame **and** the receipt lists that frame. The entry
  names the baseline by its age k (1..31) in its flags. With no usable baseline the entry is absolute (baseAge 0).
* So a baseline exists bit for bit on both sides, and a lost frame is simply never acknowledged and never used. A client
  that cannot resolve a baseline (a bug, a hostile server) refuses the whole frame and does not acknowledge it.
* A `left` id wipes that player's history on the client; the server resets the pair, so a re-entering player (or a
  reused id) is sent absolute first.
* Frames carry increasing ticks; the decoder refuses a tick that is not newer than the newest it accepted (stale or
  duplicate) without any change of state.

### Sample age

`age` lets a client place the sample on the server's timeline: the sample was taken at `tickTime - age`. The state is
never older than the sender's last `PlayerState`; an idle sender's age keeps growing (up to 255). Since v5 the
sample's time is the sender's own `PlayerState.sampleMs` placed on the server's clock (section 4.6), not the packet's
arrival, so network jitter no longer moves the sample; `tests/interest_test.cpp` measures the placement error of a
jittery link at 0.34 ms (standard deviation) against 6.4 ms when placing at arrival. The ages are measured against the
tick's nominal time (its slot), not the clock of a loop that got round to it late.

## 7. WorldCoarse

Every player that is not in the client's relevance set is sent at about 1 Hz (`rateCoarseHz`) as three `u16`
(section 5), in rolling pages of at most 4 packets per second. An entry is sent when the 0.5 m coarse cell changed
since the last one or 5 s have passed. The coarse channel has its own share of the byte budget (20% by default); under
a very large crowd the pages come slower rather than starving the detail channel. A coarse entry never repeats the
information of a relevant player. Ids ascend; the client's own id never appears.

## 8. Limits

| Limit | Value | Where enforced |
|---|---|---|
| players | 1024 (`kMaxPlayers`), ids 0..1023 | Welcome, WorldState, WorldCoarse decoders |
| name | 1..24 bytes | Hello, PlayerJoined |
| server name | at most 64 bytes | Welcome |
| refuse text | at most 200 bytes | Refuse |
| chat | 1..200 bytes UTF-8, no control characters | ChatSend, ChatMessage |
| WorldState entries | at most 1024; at least 3 bytes each (checked against the bytes left before anything is reserved) | WorldState decoder |
| `PeerLook` publications | 2 per second per player, burst 2; the newest waiting look wins | server |
| look values | model not 0; drawable 0..1023 (prop -1..1023); texture 0..63; palette 0..3; head parent ids 0..63 | `ValidLook`, server and client |
| ped state | `move` 0..6, flags bits 0..14 (v5; v4: 0..12), `kPfDriver` only with `move` 4..6, aim +-32767 dm and only with `kPfAiming`, `vehId` byte exactly with `move` 5 and 6 | `Decode` (wire), `ValidPedState` (server policy) |
| `left` ids per packet | at most 255 on the wire; the server sends at most 16 | WorldState decoder, server |
| baseline age | 1..31 ticks (history of 32) | WorldState decoder |
| WorldState / WorldCoarse / VehicleWorld packet | at most `worldPacketMaxBytes`, default 1200, setting 200..1300 | server |
| vehicles | ids 0..255 (`kMaxVehicles` 256, a `u8` on the wire); `maxVehicles` 64 alive (1..256), 3 spawned + 3 adopted per player, 8 seats | VehicleWorld decoder, server |
| VehicleCmd | 10 a second per player; Spawn + Register 3 at once then 1 a second | server (`RateLimited`) |
| VehicleUpdate | the vehicle's controller (v5; v4: the server's driver), newer seq, world box, `vehicleMaxSpeed` 150 m/s, health 0..1000 and only down | server |
| Hit | 30 a second per shooter (`hitsPerSecond`), damage 1..250 (clamped, then per weapon class, then scaled by the `*DamagePercent` keys), `maxHitDistance` 1000 m, target relevant, a real shot (v5: a swing too, for melee) / weapon in hand / reach (see `Hit`) | server |
| connection attempts | 5 a minute per source address (`connectsPerIpPerMinute`), refused `TooManyConnections` before Hello | server |
| Respawn | `respawnDelaySeconds` 5 after the death | server |
| emotes | names 1..63 of `A-Z a-z 0-9 _ @ / . -`, blends within +-1000, duration -1 or more; 4 at once then 4 a second | decoders (length), `ValidAnim`, server |
| per-client send budget | `clientBudgetBytesPerSec`, default 36 000, setting 1000..100 000 000 | server |
| quantiser box | section 5 | every decoder |

## 9. Refuse, leave and disconnect codes

`RefuseReason` (the `u8` in `Refuse`):

| Value | Name | Meaning |
|---|---|---|
| 1 | `VersionMismatch` | Hello carried a protocol version other than the server's (player-facing text, `kRefuseTextClientOld` for an older client, `kRefuseTextClientNew` for a newer one) |
| 2 | `BadName` | the name fails the charset or length rule |
| 3 | `NameTaken` | another joined player has it (case-insensitive) |
| 4 | `ServerFull` | `maxPlayers` joined already |
| 5 | `Protocol` | anything but a valid Hello first, a malformed or out-of-order message |
| 6 | `HelloTimeout` | no Hello within 5 s |
| 7 | `Banned` | the name or address is on the ban list |
| 8 | `TooManyConnections` | the per-address connection cap is reached |

`LeaveReason` (the `u8` in `PlayerLeft`): 1 `Quit` (said goodbye), 2 `Timeout` (vanished), 3 `Kicked` (the server cut
it off: kick, ban, rule violation).

ENet disconnect data: `0` timeout (no deliberate disconnect), 1 `kDisconnectQuit` (client to server), 2
`kDisconnectKicked`, 3 `kDisconnectRefused`, 4 `kDisconnectShutdown` (server to client).

Values 7 and 8 were added without bumping the version because they only ever appear in a `Refuse`; a client that
does not know them cannot decode that `Refuse` but still ends its session through the disconnect data.
`docs/api/server.md` section 9 has the full table of when each is sent.

## 10. Strict decoding

`Decode` returns false, and leaves its output untouched, for: a wrong type byte; truncation; trailing bytes; a string
or count over its cap; an id at or above 1024; a value outside the quantiser box (checked after every delta is
applied, not just in absolute entries); a non-canonical or over-long varint; an absolute entry with change bits
set; a delta whose baseline the decoder does not hold; a WorldState tick of 0; an unknown refuse or leave reason;
(v3) a reserved bit in a move byte, ped flags or extension byte, a `move` of 7, an aim without `kPfAiming` (absolute: a
non-zero aim cannot be expressed; delta: an aim bit), an aim axis beyond +-32767 after the delta, a `PeerLook` id at or
above 1024; (v4) a reserved bit in the shot byte or a `VehicleWorld` entry header, a vehicle id listed twice or both
present and removed, ids not strictly ascending, a rotation code of 1023 or an imaginary dropped component, -128 in an
angular velocity or steering byte, unknown vehicle flags, a health or fuel over 1000, a seat occupant id at or above
1024, an unknown `VehicleCmd` op (0 or over 8), a `Register` without its tail or another op with one, an unknown
`VehicleResult` op or code, a `Hit` kind other than 1 or 2, a `Damage` killed byte over 1, a victim id at or above
1024, a non-finite `RespawnOk`, an animation or scenario name over 63 bytes; (v5) a `Welcome` with `playerHealth` 0, a
move-5 or move-6 ped block without its `vehId` byte or a `vehId` byte after any other move (in a `PlayerState`, an
absolute entry or after a delta's move byte), the vehicle-id extension bit on a delta whose resulting move names no
vehicle, a `VehicleWorld` entry header above 2, a `ctrl` or `ownerId` that is neither `0xFFFF` nor below
1024, reserved `doors` bits (in an entry or a `VehicleUpdate`; v6: `flags2` has no reserved bit left), (v6) a `tyresRim` bit without its `tyres` bit, unknown flags or a steering of -128 in a
motion-only entry, a `VehicleCmd` op over 9, a `VehicleResult` code over 12, reserved `Damage.flags` bits (4..7) and
reserved `Killed.flags` bits (2..7).
`WorldDecoder` additionally refuses (and counts separately) a frame whose tick is not newer than the newest accepted.
A refusal changes no decoder state: the receipt, the histories and the counters other than the refusal counter stay as
they were. `Encode` of a `PlayerState`, `WorldState` or `CoarseState` returns an empty result for a non-finite float
(an aim axis counts only while `kPfAiming` is set), a `move` above 7, ids that are not strictly ascending, or counts the
wire cannot express. A decoded packet re-encodes to the same bytes
(`tests/quant_test.cpp` fuzzes 500 000 packets for this).

## 11. Test vectors

All asserted byte for byte by `tests/netcode_test.cpp` and `tests/quant_test.cpp`.

The state used below: x = 1.00 m, y = -1.00 m, z = 2.50 m, heading 90 degrees, velocity (1, -1, 0.5) m/s, i.e.
x = 100 cm, y = -100 cm, z = 250 cm packed as `x | y << 22 | z << 44` into `0x000FAFFFE7000064`, heading `0x4000`,
velocity 32, -32, 16.

```
state16   64 00 00 E7 FF AF 0F 00  00 40  20 00 E0 FF 10 00
ped8      00  00 00  00 00 00 00  00                      idle ped: move byte, flags, weapon, shot byte

Hello "Al", version 5
          01 05 00 02 00 41 6C

Welcome yourId 0x0102, tickHz 0x0304, server name "S", playerHealth 200 (9 bytes)
          02 02 01 04 03 01 00 53 C8

PlayerState seq 0x01020304, ack tick 0x0A0B0C0D, ack bits 0x80000001, sampleMs 0x1234, idle ped (39 bytes)
          06 04 03 02 01  0D 0C 0B 0A  01 00 00 80  34 12  <state16>  <ped8>

The same, running (2), jumpSeq 1, flags aiming | jumping, weapon 0xA2719263, shotSeq 11, aim (1.0, -0.5, 2.0) m (45 bytes)
          06 04 03 02 01  0D 0C 0B 0A  01 00 00 80  34 12  <state16>  0A  05 00  63 92 71 A2  0B  0A 00 FB FF 14 00
          move byte 2 | 1 << 3 = 0x0A, flags 0x0005, weapon, shot byte 11, aim (10, -5, 20) dm
          (with the jumping flag only, the aim is not sent: ... 0A 01 00 63 92 71 A2 0B, 39 bytes)
          (entering vehicle 200, move 5, no weapon and no shot, adds the vehicle id after the shot byte: ... 05 00 00 00 00 00 00 00 C8, 40 bytes)

WorldState tick 0x01020304, left {7}, one absolute entry: id 5, age 12, idle ped, health 100, armour 0 (38 bytes)
          07 04 03 02 01  01 07 00  01  05 00 0C  <state16>  <ped8>  64 00
          type, tick, nLeft, left[0], count, idGap, flags, age, state, ped, vitals

WorldState tick 6, one delta entry: id 3 moved +55 cm x, +6 cm y against tick 5 (age 1) (14 bytes)
          07 06 00 00 00  00  01  03 21 00 00  6E 0C 00
          type, tick, nLeft 0, count 1, idGap 3, flags 0x21 = age 1 | position bit, age 0, extension 00, dx, dy, dz zigzag

WorldState tick 9, one delta entry: id 0, heading +16384, vx +32 against tick 7 (age 2), age 7 (17 bytes)
          07 09 00 00 00  00  01  00 C2 07 00  80 80 02  40 00 00
          flags 0xC2 = age 2 | heading bit | velocity bit, extension 00, dh zigzag 32768 = 80 80 02, dvx dvy dvz = 40 00 00

WorldState tick 6, one delta entry: id 3 stands still and starts aiming at (+1.0, 0, -0.5) m (16 bytes)
          07 06 00 00 00  00  01  03 01 00 0A  04 00  14 00 09
          flags 0x01 = age 1 (no kinematics bits), age 0, extension 0x0A = flags | aim, flags 0x0004,
          aim deltas +10, 0, -5 dm zigzag (the weapon, unchanged, is not sent)

PeerLook id 5 (88 bytes): model 0x705E61F2, component i = (0x100 + i, i, i % 4), prop 0 none, prop i = (200 + i, i),
head blend 21 0 45 3 4 5 128 255 0
          11 05 00  F2 61 5E 70  00 01 00 00  01 01 01 01  ...  0B 01 0B 03  FF FF 00  C9 00 01  ...  CF 00 07
          15 00 2D 03 04 05 80 FF 00
          (PlayerLook is the same without the id: type 10, 86 bytes)

WorldCoarse tick 7, one entry: id 3 at (100.0, 0.0, 30.0)
          0A 07 00 00 00  01  03  C8 80  00 80  E6 67
          x = (100 + 16384) / 0.5 = 32968 = 0x80C8, y = 32768 = 0x8000, z = (30 + 2048) / (5/64) = 26598 = 0x67E6

The vehicle below: id 5, model 0x0A0B0C0D, position (1, 2, 3) m = 100 | 200 << 22 | 300 << 44 cm =
0x0012C00032000064, velocity (5, 6, 7) m/s = 160, 192, 224, identity rotation 0xDFF7FDFF, angular velocity
(0.5, -1, 0) rad/s = 8, -16, 0, steering -1 = -127, locked | lights, body 900, engine 800, fuel 700, colours 12 / 34,
(v5) age 12 ms, controller player 3, owner player 5, flags2 headlights | interior | right indicator = 0x15, doors front
left | trunk = 0x21, (v6) tyres slots 0 + 4 burst = 0x11, rim slot 4 = 0x10, windows 4 + 5 smashed = 0x30, player 3 driving and player 5 in seat 2. Vehicle 11 is a motion-only entry with the same motion,
age 30 ms and flags `kVehEngine`.

VehicleWorld tick 0x01020304, removed {7}, present {5 with a full state, 9 without, 11 motion-only} (89 bytes)
          0E 04 03 02 01  01 07  03 00
          05 01  0D 0C 0B 0A  64 00 00 32 00 C0 12 00  A0 00 C0 00 E0 00  FF FD F7 DF  08 F0 00  81
                 03  84 03  20 03  BC 02  0C 22  0C  03 00  05 00  15 21  11 10 30  05  03 00 05 00
          09 00
          0B 02  1E  64 00 00 32 00 C0 12 00  A0 00 C0 00 E0 00  FF FD F7 DF  08 F0 00  81  04
          type, tick, nRemoved, removed, count; id, header 1, model, position, velocity, rotation, angular velocity,
          steering, flags, body, engine, fuel, colours, ageMs, ctrl, ownerId, flags2, doors, tyres, tyresRim, windows, seat mask 0b101,
          occupants; id, header 0; id, header 2, ageMs, the motion, flags

VehicleCmd SetLocked vehicle 5 (8 bytes)
          0F 05 00 05 01 00 00 00
VehicleCmd Register model 0xB779A091 at (1, 2, 3) m, identity, colours 4 / 5, seat 1 (23 bytes)
          0F 00 00 08 91 A0 79 B7  64 00 00 32 00 C0 12 00  FF FD F7 DF  04 05 01

VehicleUpdate seq 0x11223344, vehicle 5, the motion above but angular velocity (0, 0, 0.25), steering +1, engine | horn,
body 999, engine 1000, sampleMs 0x5566, flags2 headlights | left indicator = 0x09, doors front right | hood = 0x12,
(v6) tyres slots 1 + 2 = 0x06, rim slot 1 = 0x02, window 0 = 0x01 (40 bytes)
          12 44 33 22 11 05  64 00 00 32 00 C0 12 00  A0 00 C0 00 E0 00  FF FD F7 DF  00 00 04  7F  14  E7 03  E8 03  66 55  09 12  06 02 01

Hit player 2, WEAPON_PISTOL 0x1B06D571, 34 damage, bone 31086, shot 9 (13 bytes)
          14 01 02 00 71 D5 06 1B 22 00 6E 79 09
Damage victim 2, attacker 1, pistol, 34 taken, health 66, armour 0, alive, bone 31086, flags headshot | armour (17 bytes)
          15 02 00 01 00 71 D5 06 1B 22 00 42 00 00  6E 79  09
Killed victim 2, killer 0xFFFF (the console), weapon 0, flags headshot | melee (10 bytes)
          16 02 00 FF FF 00 00 00 00  03
PeerAnimStop id 4
          1E 04 00
```

## 12. Changes from v1

| | v1 | v2 |
|---|---|---|
| `kVersion` | 1 | 2; a v1 client gets `Refuse(VersionMismatch)` |
| position | 3 x `f32` | 1 cm integers, 22/22/20 bits in a `u64` |
| heading, velocity | `f32` each | `u16` (1/65536 turn), 3 x `i16` (1/32 m/s) |
| `PlayerState` | 33 bytes | 29 bytes, carries the `WorldState` receipt |
| `WorldState` | every player, every tick | the relevant players, per-pair rates, per-client byte budget, deltas against an acknowledged baseline, `left` ids, a sample age per entry |
| `WorldCoarse` | none | new (type 10): everyone else at about 1 Hz |
| `Hello`, `Refuse` | | unchanged |

## 13. Changes from v2

| | v2 | v3 |
|---|---|---|
| `kVersion` | 2 | 3; a v2 client gets `Refuse(VersionMismatch)`, a v2 server refuses a v3 client |
| `PlayerState` | 29 bytes | 36 bytes, 42 while aiming: + ped block (move, jumpSeq, flags, weapon, aim) |
| absolute `WorldState` entry | 19 bytes (16-byte payload) | 26 bytes (23-byte payload), 32 while aiming |
| delta `WorldState` entry | header, age, kinematics deltas | + extension byte after the age, then changed ped fields after the kinematics deltas (+1 byte per delta entry; the unchanged entry is 4 bytes, was 3) |
| idle detection | kinematics only | kinematics and ped block: a ped-only change ends idling on that tick |
| `VehicleWorld` (14), `VehicleCmd` (15) | staged | part of the protocol (server support pending) |
| `PlayerLook` (16), `PeerLook` (17) | none | new: appearance, validated, at most 2 publications per second per player, replayed to late joiners |
| `Hello`, `Refuse` | | unchanged |

## 14. Changes from v3

| | v3 | v4 |
|---|---|---|
| `kVersion` | 3 | 4; a v3 client gets `Refuse(VersionMismatch)`, a v3 server refuses a v4 client. Client and server ship together (the VPS server must be redeployed with the client) |
| ped block | 7 bytes (13 aiming) | 8 bytes (14 aiming): + shot byte (`shotSeq`, 4 bits) after the weapon |
| `PlayerState` | 36 bytes (42 aiming) | 37 bytes (43 aiming) |
| absolute `WorldState` entry | 26 bytes (23-byte payload), 32 aiming | 29 bytes (26-byte payload: + shot byte, + `u8` health, `u8` armour), 35 aiming |
| delta `WorldState` entry | extension bits 0..3 | + bit 4 shot byte, bit 5 vitals (2 bytes); a pure-kinematics teleport (24 bytes) is now a delta, not absolute |
| health and armour | the client's own | the server's: `Hit` (validated, clamped to 250, armour first) -> `Damage` (victim and attacker), `Killed` (everybody), `Respawn` -> `RespawnOk` after `respawnDelaySeconds`; console `heal`, `kill`, `armor`. `kPfDead` in a `WorldEntry` = server-dead or client-reported dead |
| `VehicleWorld` (14) | all-to-all, `u16` ids, `ownerId`, 7 x `f32`, fuel, 2 flags; never sent | per receiver, relevance-filtered presence list + due full states (`u8` ids; quantised position and velocity, smallest-three rotation, angular velocity, steering, 5 flags, body / engine health, fuel, colours, 8 seat occupants; 36 + 2 per occupant bytes); sent |
| `VehicleCmd` (15) | 6 ops, refused by the server | + `SetColours` (7), `Register` (8, adopt a local car, 16-byte tail); every command answered with `VehicleResult`; server-authoritative occupancy, caps, despawn |
| new messages | | 18 `VehicleUpdate` (driver only), 19 `VehicleResult`, 20 `Hit`, 21 `Damage`, 22 `Killed`, 23 `Respawn`, 24 `RespawnOk`, 25 `PlayAnim`, 26 `PlayScenario`, 27 `StopAnim`, 28 `PeerAnim`, 29 `PeerScenario`, 30 `PeerAnimStop` |
| aim precision | 0.1 m per axis | unchanged (already under 0.1 degree at 30 m) |
| authority rules (server policy, no layout change) | `Hit` checked for range and relevance only | `Hit` bound to a shot the server saw (`shotSeq`), the weapon in hand, per-shot hit count, per-class damage caps and budgets, melee / run-over / explosive reach; position jumps open a 3 s untrusted window (Enter / Register / Spawn / outgoing Hits refused); leavers' vehicles destroyed or orphaned; `RateLimited` results sent at most once a second; per-address connect rate and vehicle cap; `Anim.flags` reduced to bits 0..8. See docs/api/server.md "Anti-cheat rules" |
| `Hello`, `Refuse` | | unchanged |

## 15. Changes from v4

Client and server ship together: a v4 client gets `Refuse(VersionMismatch)` from a v5 server and the other way round
(the VPS server must be redeployed with the client). Every row is in the codec and the server (`libs/protocol`,
`libs/sync/sampleclock.h`, `apps/server`); the client engine adopts the new fields in the next step (sync plan W6).

| | v4 | v5 |
|---|---|---|
| `kVersion` | 4 | 5 |
| `Welcome` | 7 + n bytes | + `u8 playerHealth` (the server's maximum health; the `playerHealth` default is 200, was 100) |
| `PlayerState` | 37 bytes (43 aiming) | + `u16 sampleMs` after the ack bits: 39 (45); the server places the sample on its clock (`SampleClock`) and ages `WorldState` entries from it |
| ped block | flags bits 0..12 | + `kPfSwimming` (13), `kPfMelee` (14); bit 15 reserved (it became `kPfVaulting` in the 0.4.3 fixes, still v5). A move 5 (entering) or 6 (exiting) carries a `u8 vehId` after the shot byte (`PlayerState` 40 / 46 bytes, absolute entry 27 / 33) |
| delta `WorldState` entry | extension bits 0..5 | + bit 6 vehicle id (`u8`, right after the move byte; only when the resulting move is 5 or 6 and the id differs from the baseline's) |
| `VehicleWorld` (14) entry | header 0 / 1; full state 36 bytes + 2 per occupant | header 2 = motion-only entry (`u8 ageMs`, motion, `u8 flags`: 26 bytes with id and header); a full state is 43 bytes + 2 per occupant: + `u8 ageMs`, `u16 ctrl`, `u16 ownerId`, `u8 flags2`, `u8 doors`. Statics ride a newcomer, a change (3 frames) and one entry a second |
| `VehicleCmd` (15) | ops 1..8 | + `Release` (9). `Enter` into seat 0 of an owned vehicle: `NotOwner` for everybody but the owner |
| `VehicleResult` (19) | codes 0..11 | + `NotOwner` (12) |
| `VehicleUpdate` (18) | 33 bytes, the seat-0 driver only | 37 bytes: + `u16 sampleMs`, `u8 flags2`, `u8 doors`; accepted from the vehicle's controller (`VehicleState.ctrl`: the driver, else the nearest trusted player in view), which the server assigns every tick |
| `Damage` (21) | 14 bytes, to the victim and the attacker | 17 bytes: + `i16 bone`, `u8 flags` (headshot, melee, explosion, armour absorbed); also to every player for whom the victim or the attacker is relevant |
| `Killed` (22) | 9 bytes | 10 bytes: + `u8 flags` (headshot, melee) |
| `Hit` (20) | 13 bytes | unchanged; the server now scales the damage it applies (`firearmDamagePercent` 72, `meleeDamagePercent` 50, `explosiveDamagePercent` 100, `runOverDamagePercent` 100, `headshotPercent` 300 on the head bone) and a melee Hit needs a swing (a `shotSeq` step, 2 Hits per swing, 4 m) |
| `RespawnOk` (24) | health 100, armour 0 | health `playerHealth` (200), armour `respawnArmour` (0) |
| health | 100 by default (GTA ped health 200) | 200 by default (GTA ped health 300); a carbine kills in 9 hits instead of 4 |
| `Hello`, `Refuse` | | unchanged |

<!-- ===== T1 seats (desync overhaul round 2) ===== begin: append-only section, owned by the T1 task -->

## T1 seats: enter / exit intents (wire v6, additive)

Why: the seat list (`VehicleWorld.seats`) says who SITS where, but nothing said that a player is ON THE WAY to a seat, so every other
screen could only warp the puppet into the seat the moment the list named it ("sometimes the player is teleported in, sometimes other
players never see the enter animation at all"). v6 makes the intent authoritative on the server and replays it on every client with the
game's own enter task; the seat warp stays only as a bounded fallback. `VehicleWorld`, `VehicleState`, `VehicleUpdate` and `PedState`
are NOT touched by this section. Client and server must ship together, like every wire change (the orchestrator does the single
`kVersion` bump; this section is written against v6).

### New ops of `VehicleCmd` (15), new range `1..11` (`kVehicleOpLast = CancelIntent`)

| op | value | `arg` | meaning |
|---|---|---|---|
| `EnterIntent` | 10 | bits 0..7 the wire seat (0..7), bit 8 `kIntentRun` (the player runs to the door; clear = walks); every other bit must be 0 (`BadArg`) | reserve seat `arg & 0xFF` of `vehId` for the sender. Same gates as `Enter` (dead, no state, untrusted window, `NoVehicle`, `BadArg` seat, `NotOwner` driver seat of an owned car, `SeatTaken` when the seat is occupied OR reserved by somebody else, `TooFar` over `vehicleEnterDistance`, `Locked`), plus `NotAllowed` when the sender already sits in a vehicle. Ok seats NOBODY: the reservation lives `kIntentTtlMs` (6000 ms) |
| `CancelIntent` | 11 | 0 | drop the sender's reservation on `vehId`; Ok also when there is none (idempotent); `NoVehicle` when the car does not exist |

A player has at most one reservation: a new `EnterIntent` for another seat cancels the old one (`Cancelled/Replaced`), the same seat
renews it silently. `Exit` while only a reservation is held answers Ok and cancels it (`Cancelled/ByPlayer`). `Enter` by the holder
turns it into occupancy; an `Enter` of another seat cancels it (`Cancelled/Replaced`). An `Enter` of a seat somebody else has reserved is
`SeatTaken`. The answer to both ops is the usual `VehicleResult` (19) with `op` = 10 / 11 (the link of `libs/sync/vehicle_link.h` never
sees these ops).

### 31 `SeatNotice` (server to client, reliable): 8 bytes

```
u8 type=31 | u8 kind | u16 player | u16 vehId | u8 seat | u8 detail
```

Sent to every client that has the vehicle in its relevant set (`VehicleStore::RelevantTo`) and to the player concerned, in order with
the other reliable traffic (a `VehicleResult` to the actor first). Never relayed back: a client that sends one is refused
(`Refuse(Protocol)`).

| kind | value | `detail` | meaning |
|---|---|---|---|
| `Reserved` | 1 | bit 0 `kSeatNoticeRun` | `player` reserved `seat` of `vehId`: replay the entry (the real enter task toward that seat) |
| `Seated` | 2 | 0 | the reservation became occupancy: `player` holds `seat` (the next `VehicleWorld` lists it) |
| `Cancelled` | 3 | `SeatCancelReason` 0 ByPlayer, 1 Expired, 2 Replaced, 3 VehicleGone, 4 PlayerGone | the reservation ended without a seating |
| `Exited` | 4 | 0 | `player` left `seat` by its own `Exit`: play the normal exit animation now. NOT sent for a death, a respawn, a disconnect or a despawn (those only drop the seat from the list) |

Strict decoding: unknown kind, `player >= kMaxPlayers` (1024), `vehId >= kMaxVehicles` (256), `seat >= 8`, or a `detail` the kind does
not define (Reserved: only bit 0; Cancelled: 0..4; Seated / Exited: 0) refuse the message; `Encode` returns an empty packet for the same
fields. Extra or missing bytes refuse it. Test: `tests/decode_fuzz_test.cpp` (four `SeatNotice` cases, two new `VehicleCmd` cases),
`tests/gameplay_test.cpp` `TestWire`, `tests/netcode_test.cpp` (`VehicleCmd` ops 1..11, `PeekType` 1..31).

### What the clients do with it (libs/sync/seat_intent.h, apps/engine/engine.cpp)

* The player's own client, on F / G: `EnterIntent`, wait for the Ok, then GTA's `TASK_ENTER_VEHICLE` for exactly that seat (flags 1, no
  jack flag); the link's `Enter` follows when the game has seated the ped. A refusal starts nothing; no answer within 2 s cancels. The
  teleport-in (`SET_PED_INTO_VEHICLE`) only happens when the task never starts (1 s), stops without a seat (0.4 s after) or runs out of
  time (4.5 s door, 3.5 s bike / cycle), or for a locked car (its owner's entry).
* Every other client: `Reserved` starts the same task on the puppet ONCE; the seat warp waits `confirmGraceMs` (1.5 s) after `Seated` / the
  list naming the player and is never allowed while the task still runs inside that grace; `Cancelled` clears the task; `Exited` runs the
  normal exit animation at once and refuses a stale seat list for 0.7 s.
* `HKCU\Software\GAGE-MP` value `seat_intent=0` restores the pre-v6 behaviour on the client for A/B testing (the server keeps sending).

Server diagnostics: `seats: last 10 s: ...` in the server log while intents flow. Client: `engine: seats (T1), last 10 s: ...` and
`engine: seats: ...` lines in `gage.log`.

<!-- ===== T1 seats ===== end -->

<!-- ===== T3 (Desync Overhaul round 2): vehicle visual state, protocol v6. Append-only section; the orchestrator bumps kVersion. ===== -->

## 16. T3: vehicle visual state (burst tyres, windows, brake lights, steering) for the other clients (protocol v6)

**Why.** A burst tyre, a smashed window, a lit brake light and a steering wheel turned were only ever seen by the machine whose
game produced them: the vehicle messages carried no such state, and a vehicle `Hit` only lowered the body and engine health. This
section is the wire, the server's merge and the client rules; the client logic is `libs/sync/vehicle_visual.h`, the tests are
`tests/t3_vehvisual_test.cpp`. Client and server ship together (the orchestrator raises `kVersion` and `kClientVersion` once).

**Wire (all in the codec, `libs/protocol`).**

| Message | v5 | v6 |
|---|---|---|
| `VehicleUpdate` (18) | 37 bytes | 40: + `u8 tyres`, `u8 tyresRim`, `u8 windows` after `doors` |
| `VehicleWorld` (14) full entry | 43 bytes + 2 per occupant | 46: + the same three bytes after `doors`. A motion-only entry (header 2) does not carry them |
| `flags2` | bits 6..7 reserved (refused) | bit 6 `kVeh2Brake` (brake lights on), bit 7 `kVeh2Handbrake`; every bit is defined |
| `Hit` (20) | `bone` carried, not checked | unchanged layout; for `kind` = vehicle a `bone` of `(part << 8) \| slot` names a tyre / window (below) |

`tyres`, `tyresRim` and `windows` are slot bit sets (bit i = slot i, 0..7): `tyres` = the tyre is burst (flat), `tyresRim` = it is
completely burst, off the rim (always a subset of `tyres`: both decoders and both encoders refuse a rim bit without its tyre bit,
`ValidVehicleUpdate` too), `windows` = the window is smashed. Slots are the game's own numbers (a car: 0 / 1 the front left / right wheel, 4 / 5
the rear pair, 2 / 3 the middle pair of a 6-wheeler; a bike: 0 front, 4 rear; windows 0..7); the wire never names a model, so a bike
and a truck need no special case. Wheels the game numbers above 7 (the second middle pair of some trucks) are not carried.

**Brake lights and handbrake** are the controller's pedals as its client reads them (`sync::ClassifyBrake`: the handbrake; the foot
brake while rolling forward; the accelerator while rolling backward; below 0.5 m/s a pedal is throttle / reverse, not braking). They
ride `flags2`, so a change is a full entry for 3 frames like the other body bits. A car nobody controls is sent with `kVeh2Brake`
clear (`BuildFrame`).

**Steering** was already on the wire (`steer`, `i8`, -1 left .. +1 right, in the motion of both messages). v6 changes how the sender
fills it: the yaw rate now comes from two successive rotations (unit-independent) and, for the car the local player drives, the
steering axis replaces the estimate below 1 m/s, so a standing car shows turned wheels (`sync::ChooseSteer`). The wire and the
receiver's `SET_VEHICLE_STEER_BIAS` write are unchanged.

**Server rules** (`apps/server/vehicles.h`).
1. *The controller reports.* `VehicleStore::Update` takes the three masks from the vehicle's controller. A bit the controller shows is
   truth; a bit it does not show is cleared (a repair).
2. *A Hit names a part.* `HitPart` runs after a vehicle `Hit` passed every existing check (alive, in range, in the shooter's
   `VehicleWorld`, 30 per second, bound to a real shot of the shooter's with the weapon reported; see "Hit"): `bone` = `(part << 8) | slot`,
   part 1 = a tyre burst, 2 = a tyre off its rim (sets the tyre bit too), 3 = a window smashed, slot below 8. Any other `bone` (-1,
   a ped bone id) is ignored for a vehicle, exactly as before. The bit is set for every receiver at once (full entries for 3 frames)
   and is kept **unconfirmed**: the controller's next reports that show it confirm it (it then follows the controller like any bit);
   if the controller never shows it, it is dropped `3000 ms` after the Hit (`kVisAckMs`), so a controller that cannot show a pop
   (native missing, the pop rejected on its machine) does not leave a phantom for ever. A car with **no controller** gets no reports, so
   nothing expires and nothing clears: its bits stay. The Hit's `damage` must be non-zero as always (the engine sends 1); it is
   applied to the body health like any vehicle damage.
3. *Stream-in, hand-over.* The masks are static fields: a newcomer gets them in its first full entry, a change is told for 3 frames, and
   one entry a second repeats them. A new controller reports what its car shows, which is what the server told it.

**Client rules** (`libs/sync/vehicle_visual.h`, engine glue in `TickVehicleVisual`, `SendControlledVehicles`, `ApplyVehicleBody`).
- *Controller* (`ctrl` is me): reads its car's tyres and windows every 250 ms and sends them in `VehicleUpdate`; when the server's mask
  gains a bit (a Hit by somebody else) it bursts / smashes it on its own car **once** (the rising edge, all bits on the first sight),
  never clears one (a repair is its own business, reported by the next update).
- *Copy* (somebody else's car): applies the server's mask level-wise (burst what is missing, fix what is extra), on every change of the
  mask and every 1.5 s for copies within 150 m. While the local player has shot lately (1.5 s) it scans the copy every 200 ms: a burst
  its own game produced that the server's mask lacks (and that the server did not just clear) is reported once as a vehicle `Hit`
  (`weapon` = the weapon in hand, `shotSeq` = the shot counter, `damage` 1, `bone` = the part code) and kept for 2.5 s as pending, so the
  round trip is not drawn as pop, fix, pop; confirmed by the server's mask, or put right after 2.5 s (the server refused it: no shot
  behind it, out of range, over the rate).
- *Natives.* Every native needed is one of the nine in hook.h whose hash is still 0 (UNRESOLVED: lookup needed). A wrapper with hash 0
  returns false without calling anything and logs once; a failed read is "no information" (nothing is reported, the controller reports
  what it applied), a failed command round is retried 3 times and then the car is left alone for 5 s. Adding the hashes switches the
  feature on with no other change.

**Wheel spin.** Nothing is sent for spin: wheels roll from ground contact because the copy's body gets real velocity. What can stop
that is a brake the game itself applies to an empty or task-less car, so the copy's handbrake is **released explicitly** on the first
sight and mirrored afterwards (`kVeh2Handbrake`), and the brake lights follow the controller's pedals. A burnout (wheels spinning on a
standing car) and a locked wheel at speed need a wheel-speed native and a way to write wheel rotation, neither of which exists in
hook.h; they are not carried.

**Not carried (and why).** Body deformation (a per-vertex damage map: dozens of samples of `GET_VEHICLE_DEFORMATION_AT_POS` per car, not
cheap), broken-off doors, broken lights and dirt: no cheap bit set, no native checked. Tyre and window repair by a gamemode or an
admin command is not a thing yet: the server's repair path (`fix`) does not touch visual state; a controller that repairs its car
reports the clear and everybody follows.

**Diagnostics.** The engine prints `engine: vehicle visual (T3), last 10 s: ...` with every 10 s of vehicles lines (see the report of
the task for the fields); the server counts `vehiclePartHits`.

<!-- T4-BEGIN: vehicle collisions (desync overhaul round 2). Append-only section; the orchestrator does the single kVersion 5 -> 6 bump. -->
## 16. T4: vehicle collisions (`VehicleImpact`, message 40)

Wire change for protocol v6 (the orchestrator bumps `kVersion` once, in lockstep with the client version): ONE new message and no change to any existing layout.
Design, numbers and the live test: docs/sync-specs/vehicle-collisions.md.

### 40 `VehicleImpact` (server to client, reliable): 21 bytes

The server keeps the last few `VehicleUpdate` samples of every vehicle on one time base. For two vehicles with DIFFERENT controllers it finds the first
instant their boxes touch (boxes by mass class, grown 0.3 m each because each controller's physics stops its car at the other's copy), solves one planar
two-body impulse there (restitution 0.25, friction 0.3, masses 250 / 1500 / 7000 kg by model) and sends each controller its half, once per contact. Pairs with
the same controller are never arbitrated. No message is sent for a pass, a graze, a closing speed under 1.2 m/s or an untrusted controller.

| offset | size | field | |
|---|---|---|---|
| 0 | 1 | type | 40 |
| 1 | 4 | `seq` | `u32`, the server's impact counter; both controllers of one contact get the same value |
| 5 | 2 | `vehId` | `u16` (< 256): the vehicle YOU control that was hit |
| 7 | 2 | `otherId` | `u16` (< 256): the vehicle it hit |
| 9 | 2 | `sampleMs` | `u16`: the contact time on YOUR OWN sample clock (the `VehicleUpdate.sampleMs` domain: whole ms of your clock, wraps at 65536). Unwrap against your current clock reading (+-32 s) |
| 11 | 2 | `dvx` | `i16`, 1/64 m/s: the change of your vehicle's world-x velocity |
| 13 | 2 | `dvy` | `i16`, 1/64 m/s |
| 15 | 2 | `dyaw` | `i16`, 1/256 rad/s: the change of its yaw rate (counter-clockwise) |
| 17 | 1 | `nx` | `i8`, 1/127: the contact normal from your vehicle toward the other one |
| 18 | 1 | `ny` | `i8`, 1/127 |
| 19 | 1 | `closing` | `u8`, 0.25 m/s: the closing speed along the normal before the impact |
| 20 | 1 | `depth` | `u8`, cm: the overlap of the two TRUE boxes within 120 ms after the touch (0: they only came within the margin) |

Strict decoding: exactly 21 bytes; `vehId` and `otherId` below 256; an `i8` of -128 and an `i16` of -32768 are refused (the encoder never writes them). `Encode`
refuses a non-finite value or an id of 256 and saturates `dv` at +-511.98 m/s, `dyaw` at +-127.99 rad/s. The server never sends a change above 30 m/s.

Client rules (engine, `libs/sync/vehicle_impact.h`):
* The game has already bounced the car off the kinematic copy, so the message is a CORRECTION target: the engine writes `authoritative - what the game's own physics did`
  (the velocity jumps it measured around the contact time) onto the car's velocity and yaw rate. A message whose contact the local physics did not feel is applied only if
  the server's true boxes overlapped (`depth >= 0.10 m`) and our copy of the other car is within 7 m; a message older than 1.5 s, a duplicate `seq` and a change under 0.6 m/s are ignored.
* A copy that takes an impulse inside a frame (the drive controller's contact trigger) has its own response adopted as its trajectory
  (`VehicleInterpolator::ReplaceFrom`): the stream, which still shows the pre-impact motion for the whole latency, is refused until it shows the impact (or 300 ms).
* `impact_sync=0` (HKCU\Software\GAGE-MP, read at Welcome) ignores the message and keeps the W9 contact grace as the only handling.

Server policy changes that need no wire change (T4):
* `VehicleStore::Rules::claimBaseMeters` / `claimLookSeconds` / `claimMinSpeed` / `claimHoldMs`: a driven vehicle about to hit a driverless one (closing speed above 1 m/s, within
  10 m + 0.5 s of travel) takes the driverless vehicle's control (`VehicleState.ctrl` changes), so the rammer's own physics resolves the hit.
* `Rules::maxSpeedSoftFactor` (2.0): a `VehicleUpdate` whose velocity is over `vehicleMaxSpeed` (150) but under twice it is accepted with the velocity clamped to the cap (it used to be
  refused as `BadArg`, which froze a hyper car for everybody else); above twice the cap it is refused as before.
<!-- T4-END -->
