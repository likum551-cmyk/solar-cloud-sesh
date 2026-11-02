# GAGE-MP own multiplayer stack — plan

Approved 2026-10-29 (phases P0–P2; P3+ reviewed after the game-hooks research).

## Decision

GAGE-MP is its own GTA V multiplayer platform: launcher + in-game engine
(`gage_mp.dll`) + GAGE server + an open GAGE protocol. RAGE-MP compatibility is
dropped. Evidence (2026-10-29):

- RAGE-MP's transport is a modified RakNet (`RakNet::RakPeer`, `mp::LaunchNetSyncThread`
  in the 1.1 server binary) with Crypto++ RSA-OAEP-SHA1 + AES-CBC; undocumented.
- Server client-packages are served as AES-sized encrypted blobs (`/file/N`); running
  them would mean circumventing the server operators' content protection.
- RAGE-MP 1.1 ships Easy Anti-Cheat; spoofing it is out of bounds.
- Take-Two issued RAGE-MP a cease-and-desist (announced 2026-05-25).
- Loading RAGE-MP's own `multiplayer.dll` (VMProtect) from our launcher faulted in its
  static initialisers (`GetSystemTimeAsFileTime` with a bad pointer, caller in `.text0`),
  while the same DLL works from the official launcher.

Legal risk stays: Take-Two treats FiveM as the only authorised platform. Owner's call.

## MVP done-criteria

Two players on different PCs open GAGE-MP, see a GAGE server in the list, join, see
each other's characters move smoothly, and chat; the server runs on the Hetzner VPS.

## Phases

| Phase | Delivers | Verified by |
|---|---|---|
| P0 | `libs/protocol`, `apps/server/`, ENet vendored; RAGE-MP parts retired (file list approved first) | clean build, selftests green |
| P1 | protocol v1, C++ server (Windows + Linux), headless bot client | `netcode_test` here: handshake, version refusal, join/leave, state sync under loss, chat exactly-once; mutation run |
| P2 | GAGE registry handoff, GAGE master list (Node/Hono on VPS), UDP ping, de-RAGE launcher copy | server-list + ping tests against local server/master |
| P3 | game bridge: per-frame tick, natives by hash for 1.0.3889, ped/pos primitives, version gate, SP systems off | owner's PC: `gage.log` + a visible test ped |
| P4 | engine joins server, sends local state, remote players as interpolated peds | bot walking a circle visible in game; then two PCs |
| P5 | Dear ImGui overlay (D3D11 Present hook): chat, status, player list | owner's PC |
| P6+ | vehicles, weapons/health/anims, streaming, server validation, encryption (libsodium), scripting (QuickJS), optional CEF | per feature |

## Protocol v1 (P1)

ENet over UDP. Channel 0 = reliable ordered (control, chat); channel 1 = unreliable
sequenced (state). Each ENet packet = `u8 type` + little-endian body, strings as
`u16 length` + UTF-8 bytes, every read bounds-checked.

| Type | Dir | Channel | Body |
|---|---|---|---|
| Hello | C→S | 0 | u16 protocol, str name |
| Welcome | S→C | 0 | u16 yourId, u16 tickHz, str serverName |
| Refuse | S→C | 0 | u8 reason, str text (then disconnect) |
| PlayerJoined | S→C | 0 | u16 id, str name |
| PlayerLeft | S→C | 0 | u16 id, u8 reason |
| PlayerState | C→S | 1 | u32 seq, f32 x y z heading vx vy vz |
| WorldState | S→C | 1 | u32 tick, u16 n, n × (u16 id, f32 x y z heading vx vy vz) |
| ChatSend | C→S | 0 | str text |
| ChatMessage | S→C | 0 | u16 fromId (0xFFFF = server), str text |

Server validation: protocol version, name 1–24 chars of a safe charset and unique,
chat ≤ 200 bytes, finite floats inside the map bounds; violations refuse or drop with a
logged reason.
