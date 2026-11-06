// GAGE protocol v8: the messages between the GAGE client engine and the GAGE server
// (docs/api/protocol.md has every byte layout; docs/ENGINE-PLAN.md the history). Transport is
// ENet: channel kChanReliable carries control and chat (reliable, ordered), kChanState carries
// per-tick movement (unreliable, sequenced).
//
// Every packet is `u8 Msg` + body (wire.h). Decode() returns false for a wrong type,
// truncation, trailing bytes, an over-cap string or count, or a value outside its range, so a
// caller never acts on a malformed message. Semantic checks (name charset, world box, chat
// content) are the Valid*() helpers, applied by the receiver that owns the policy (the server).
//
// v2 against v1: positions, headings and velocities travel quantised (quant.h), WorldState is
// interest-managed (only the players relevant to the receiver, at per-player rates, with
// per-receiver deltas against an acknowledged baseline), every WorldEntry carries the age of its
// sample, a new WorldCoarse message carries low-rate positions of everybody else, and PlayerState
// acknowledges the WorldState frames received. Hello keeps its v1 layout forever: it is how a
// server recognises (and cleanly refuses) a client of another version.
//
// v3 against v2: remote players look like players, not mannequins.
//   * PlayerState and every WorldEntry carry a PedState: movement class (idle/walk/run/sprint/
//     vehicle), a kPf* flag set (jumping, aiming, ragdoll, ...), a 2-bit jump counter, the selected
//     weapon hash and, only while aiming, the aim point relative to the player. WorldState
//     delta-codes it like the kinematics (an extension byte of change bits per delta entry).
//   * Appearance travels reliably: PlayerLook (C->S, the sender's model, clothes, props and head
//     blend) and PeerLook (S->C, someone else's). The server validates, rate-limits, stores and
//     replays every stored look to a newcomer right after its PlayerJoined burst.
//   * Server-owned vehicles: VehicleWorld (S->C, state channel) is a full snapshot of every
//     networked vehicle each tick (re-sent, so a dropped packet self-corrects): id, current driver,
//     model, kinematics, fuel and lock/light flags, all server-authoritative. VehicleCmd (C->S,
//     reliable) is a client request about a vehicle (spawn/enter/exit/lock/lights) the server
//     validates and may refuse; the server places a spawn at the requester's own position, so no
//     position is trusted from the client. (Defined in v3; first served in v4.)
//
// v4 against v3: vehicles, gunfire, damage and emotes (the RAGE:MP / GTA Network model).
//   * Vehicles are live: the driver (seat 0, per the SERVER's occupancy) sends VehicleUpdate
//     (C->S, state channel) every send tick; the server validates and copies it into its vehicle.
//     VehicleWorld is per receiver: the complete set of vehicles relevant to you (`present`, so a
//     dropped frame self-corrects) plus full states for the ones due this tick; rotation travels as
//     a smallest-three quaternion. VehicleCmd gains SetColours and Register (adopt a local ambient
//     car); Enter/Exit are authoritative (seat free, 10 m, locked); every command is answered with a
//     VehicleResult.
//   * Server-owned health: WorldEntry carries health + armor; shooters report Hit (C->S), the
//     server validates and applies armor then health, answers Damage (victim, attacker) and
//     broadcasts Killed; a dead player asks Respawn after the delay and gets RespawnOk.
//   * PedState.shotSeq: a 4-bit shot counter so remote clients play one shot per increment.
//   * Emotes: PlayAnim / PlayScenario / StopAnim (C->S) relayed as PeerAnim / PeerScenario /
//     PeerAnimStop; the current looping one is replayed to newcomers like a look.
//
// v7 against v6 (desync overhaul round 3; the byte layouts are in docs/api/protocol.md, sections "R3 T4" and "R3 T5"):
//   * R3 T4 vehicle damage: an optional damage block (flags bit 5) in VehicleUpdate and the full VehicleWorld entry: petrol tank,
//     broken doors, headlights, dent probes. An undamaged car costs nothing in a VehicleUpdate and 3 bytes in a full entry.
//   * R3 T5 link info: Msg::LinkInfo (44), the server's round trip to each vehicle controller, about once a second.
//
// v6 against v5 (desync overhaul round 2; the byte layouts are in docs/api/protocol.md, sections T1, 16 and T4):
//   * T1 seats: VehicleOp EnterIntent (10) / CancelIntent (11) and Msg::SeatNotice (31): the server reserves a seat on an enter
//     intent and tells every client, which replay the game's own enter task on the puppet.
//   * T3 vehicle visual state: VehicleUpdate and the full VehicleWorld entry grow by u8 tyres, u8 tyresRim, u8 windows; flags2 bits
//     6 / 7 are brake lights and handbrake; a vehicle Hit names a part in `bone` ((part << 8) | slot).
//   * T4 collisions: Msg::VehicleImpact (40): the server's resolution of a contact between two controllers' vehicles.
//   * R3 T5 vehicle position: Msg::LinkInfo (44): the server's smoothed round trip of every vehicle controller, so a receiver can draw a
//     nearby car where its owner really is (half the owner's and half our round trip are in no timestamp).
//
// v7 against v6 (desync overhaul round 3; docs/api/protocol.md "R3 T4"):
//   * R3 T4 vehicle damage state: the full VehicleWorld entry grows by a VehicleDamage block (petrol tank health, broken-off doors and
//     headlights, a coarse dent map: 3..11 bytes); VehicleUpdate.flags bit 5 (kVehDamageBlock) announces the same block after `windows`.
//
// v8 against v7 (real accounts, docs/api/protocol.md "v8 accounts"):
//   * Msg::JoinAuth (45, C->S reliable): sent right after Hello, carries the platform join token (empty = none). The server
//     verifies it with the platform (apps/platform/docs/game-server-contract.md) before Welcome; Hello itself is unchanged.
//   * Msg::AccountInfo (46, S->C reliable): after the Welcome burst, the verified account name and the kAcct* flags (admin,
//     admin commands allowed).
//   * Msg::AdminCmd (47, C->S reliable) / Msg::AdminResult (48, S->C reliable): the overlay's admin commands (/heal, /fix,
//     /spawn, /tp, /noclip, /gun, /car) are asked of the server, which allows them only to an admin when it knows roles.
//   * RefuseReason AccountRequired (9), AuthFailed (10), AuthUnavailable (11).
//   * 0.6 inventory (still v8: no v8 client was ever released without it): Msg::InvState (49, S->C), InvOp (50, C->S),
//     InvUsed (51, S->C), all reliable; the bytes inside are libs/sync/inventory.h's codec.
//   * 0.5.4 inventory world (still v8, additive and negotiated): InvSnap (52, S->C), InvNearby (53, S->C), InvReport (54, C->S),
//     all reliable, and InvOp's op may be 19 bytes (a ground pile or trunk side). A 0.5.3 client counts 52/53 as malformed
//     and ignores them; the server sends InvSnap / the 19-byte form only to a client that answered with an InvReport.
//
// v5 against v4: sample times, one controller per vehicle, ownership and combat that kills in a fair time
// (docs/SYNC-PLAN.md; the byte layouts are in docs/api/protocol.md).
//   * Sender-side sample time: PlayerState and VehicleUpdate carry a u16 sampleMs (the sender's own clock,
//     whole milliseconds, wrapping) that the server maps onto its clock (libs/sync/sampleclock.h) so a
//     sample is placed when it was taken, not when it happened to be processed. VehicleWorld entries carry
//     the sample's age (ageMs) the way WorldState entries do.
//   * Vehicles: one controlling client per vehicle (`ctrl`, not necessarily the driver: the nearest player
//     drives an empty car's physics), `ownerId` and the VehicleOp::Release / VehicleResultCode::NotOwner
//     ownership rules, lights and doors (`flags2`, `doors`), and a motion-only VehicleWorld entry (header 2)
//     that leaves out what did not change.
//   * On foot: kPfSwimming and kPfMelee, and the vehicle a ped is entering or exiting (PedState.vehId).
//     0.4.3 (no version bump): kPfVaulting, bit 15 of PedState.flags, the last free one.
//   * Combat: Welcome.playerHealth (the server's maximum), Damage with the bone and flags of the hit and
//     sent to the onlookers too, Killed with flags.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gage::proto {

    // Bump on any incompatible change to a message below. A server refuses a Hello whose
    // version differs (RefuseReason::VersionMismatch), so no two versions talk past each other.
    // v3: PedState in PlayerState/WorldEntry, PlayerLook/PeerLook, VehicleWorld/VehicleCmd.
    // v4: live vehicles (VehicleUpdate, per-receiver VehicleWorld), health/hits/death, shotSeq, emotes.
    // v5: sampleMs, vehicle controller / owner / ageMs / motion-only entries / lights and doors, damage
    //     scaling inputs (Welcome.playerHealth, Damage bone and flags, Killed flags), PedState.vehId.
    // v6: seat intents + SeatNotice, vehicle visual state (tyres, windows, brake lights), VehicleImpact.
    // v7: vehicle damage block (petrol, doors, headlights, dents), LinkInfo.
    // v8: JoinAuth (the platform join token), AccountInfo, AdminCmd / AdminResult, the three account RefuseReasons.
    constexpr uint16_t kVersion = 8;

    constexpr uint8_t kChanReliable = 0; // control, chat, join/leave: reliable ordered
    constexpr uint8_t kChanState    = 1; // PlayerState, WorldState, WorldCoarse: unreliable sequenced
    constexpr uint8_t kChannelCount = 2;

    constexpr uint16_t kDefaultPort = 24800;
    constexpr uint16_t kServerId    = 0xFFFF; // ChatMessage.fromId for server notices; also "all receivers" in the relevance API
    constexpr uint16_t kNoPlayer    = 0xFFFF; // "nobody" in a player-id field (an empty seat, no attacker): the same value
    constexpr uint16_t kMaxPlayers  = 1024;   // ids are 0..kMaxPlayers-1
    constexpr uint16_t kMaxVehicles = 256;    // networked vehicle ids 0..kMaxVehicles-1 (a u8 on the wire in v4)

    constexpr size_t kMaxName       = 24;
    constexpr size_t kMaxChat       = 200;
    constexpr size_t kMaxServerName = 64;
    constexpr size_t kMaxRefuseText = 200;
    // v8: a platform join token ("gmj_..." today; the platform accepts 8..200 characters) and an admin command's argument.
    constexpr size_t kMaxJoinToken  = 256;
    constexpr size_t kMaxAdminArg   = 64;

    // The player-facing text of Refuse(VersionMismatch): what the server sends, and what the engine shows on the
    // loading screen and in chat. They tell the player what to do, not which numbers differ (the server log has
    // the numbers). The launcher says the same two sentences before the game starts (apps/launcher/version_gate.h).
    // Client older than the server: the launcher's updater has the matching release; client newer: the server is
    // behind. Both are well under kMaxRefuseText and the loader's 150-byte status line.
    constexpr const char* kRefuseTextClientOld = "Solar RP was updated. Close the game; Solar Launcher installs the update.";
    constexpr const char* kRefuseTextClientNew = "This server has not been updated yet. Try again in a minute.";

    // Resource transfer (v2, reliable channel): the server advertises a small set of files (the
    // GAGE logo overlay, future content) in a ResourceManifest; the client requests by manifest
    // index the entries its disk cache lacks (keyed by name + crc32, crc32.h) and reassembles the
    // ResourceChunks. The index fits a u8 because a manifest holds at most kMaxResources entries.
    constexpr size_t   kMaxResourceName  = 64;                  // bytes; ValidResourceName charset
    constexpr size_t   kMaxResources     = 16;                  // entries per manifest (index 0..15)
    constexpr uint32_t kMaxResourceBytes = 8u * 1024 * 1024;    // 8 MiB, one resource
    constexpr uint32_t kMaxResourceTotal = 32u * 1024 * 1024;   // 32 MiB, a whole manifest (client memory bound)
    constexpr uint32_t kResourceChunk    = 1024;                // payload bytes per ResourceChunk

    // How many server ticks of per-player history both sides keep for delta baselines (a
    // baseline is "the state I sent you N ticks ago", N in 1..kMaxBaseAge), and how many frames
    // a WorldAck can acknowledge (the newest tick plus the 32 before it). Counted in ticks: 32 ticks are 1.07 s at the
    // 30 Hz default (1.6 s at the legacy 20 Hz), so a client may lose a full second of acks and still get deltas
    // (sync plan W7 asks for at least one second of history; interest_test pins kMaxBaseAge >= tickHz).
    constexpr uint32_t kWorldHistory = 32;
    constexpr uint8_t  kMaxBaseAge   = 31;
    // At most this many "left your relevance range" ids in one WorldState (the u8 count's cap).
    constexpr size_t kMaxLeavePerPacket = 255;

    // ENet disconnect data the client sends on a deliberate quit, so the server
    // can tell PlayerLeft::Quit from a timeout (ENet 1.3 reports both as data 0
    // otherwise). The server uses the rest so a client can tell why it was cut
    // off (0 on either side means "no deliberate disconnect": a timeout).
    constexpr uint32_t kDisconnectQuit     = 1; // client -> server
    constexpr uint32_t kDisconnectKicked   = 2; // server -> client: admin kick
    constexpr uint32_t kDisconnectRefused  = 3; // server -> client: after a Refuse
    constexpr uint32_t kDisconnectShutdown = 4; // server -> client: server stopping

    // The world box ValidKinematics accepts: a generous GTA V map (the playable
    // map is about +-8000 m in x/y, ground -200..~1000 m, with headroom for
    // aircraft). Anything outside is a bug or a cheat, not a position.
    constexpr float kWorldMinXY = -16000.0f;
    constexpr float kWorldMaxXY =  16000.0f;
    constexpr float kWorldMinZ  =  -2000.0f;
    constexpr float kWorldMaxZ  =   3000.0f;
    constexpr float kMaxSpeed   =    600.0f; // m/s per axis (jet territory)

    // The message type byte (first byte of every packet). Values are wire-stable: 1..9 are the
    // v1 set (their bodies changed in v2/v3/v4 where noted in docs/api/protocol.md), 10-13 added in
    // v2, 14-17 in v3, 18-30 in v4.
    enum class Msg : uint8_t
    {
        Hello        = 1,  // C->S reliable: version + name; layout frozen across versions
        Welcome      = 2,  // S->C reliable: your id, tick rate, server name
        Refuse       = 3,  // S->C reliable: reason + text, then the connection ends
        PlayerJoined = 4,  // S->C reliable: a player is in (id, name)
        PlayerLeft   = 5,  // S->C reliable: a player is gone (id, reason)
        PlayerState  = 6,  // C->S state channel: own movement + WorldState acknowledgement
        WorldState   = 7,  // S->C state channel: the players relevant to you (deltas)
        ChatSend     = 8,  // C->S reliable: a chat line
        ChatMessage  = 9,  // S->C reliable: a chat line (or server notice)
        WorldCoarse  = 10, // S->C state channel (v2): low-rate coarse positions of the others
        ResourceManifest = 11, // S->C reliable (v2): the resources to have (name, size, crc32); index = list position
        ResourceRequest  = 12, // C->S reliable (v2): send resource[index] starting at chunk startChunk
        ResourceChunk    = 13, // S->C reliable (v2): one <= kResourceChunk-byte slice of resource[index]
        VehicleWorld     = 14, // S->C state channel (v3; per receiver in v4): the vehicles relevant to you
        VehicleCmd       = 15, // C->S reliable (v3): a client request about a vehicle (spawn/enter/exit/lock/lights/...)
        PlayerLook       = 16, // C->S reliable (v3): the sender's appearance (model, clothes, props, head blend)
        PeerLook         = 17, // S->C reliable (v3): another player's appearance
        VehicleUpdate    = 18, // C->S state channel (v4): the driver's vehicle state
        VehicleResult    = 19, // S->C reliable (v4): the answer to one VehicleCmd
        Hit              = 20, // C->S reliable (v4): "my shot hit X for N" (shooter-reported, server-validated)
        Damage           = 21, // S->C reliable (v4): your (or your victim's) health changed
        Killed           = 22, // S->C reliable (v4): someone died (kill feed), to everybody
        Respawn          = 23, // C->S reliable (v4): a dead player asks to come back
        RespawnOk        = 24, // S->C reliable (v4): you are alive again, here
        PlayAnim         = 25, // C->S reliable (v4): the sender plays an animation
        PlayScenario     = 26, // C->S reliable (v4): the sender starts a scenario
        StopAnim         = 27, // C->S reliable (v4): the sender stops its animation / scenario
        PeerAnim         = 28, // S->C reliable (v4): another player plays an animation
        PeerScenario     = 29, // S->C reliable (v4): another player starts a scenario
        PeerAnimStop     = 30, // S->C reliable (v4): another player stopped its animation / scenario
        SeatNotice       = 31, // S->C reliable (T1 seats, wire v6): a player's enter / exit INTENT on a vehicle seat (see SeatNotice)
        // 32..39 are reserved for further desync-round-2 messages (31 is T1 SeatNotice): the numbers
        // between are not defined messages, a Decode of them fails and PeekType only range-checks.
        VehicleImpact    = 40, // S->C reliable (T4, vehicle collisions): the server's resolution of a contact between two vehicles for one controller
        // 41..43 are reserved for other round-3 messages.
        LinkInfo         = 44, // S->C state channel, about 1 Hz (R3 T5, vehicle position): the round trip the server measures to each vehicle controller
        JoinAuth         = 45, // C->S reliable (v8): right after Hello, the platform join token (empty = none)
        AccountInfo      = 46, // S->C reliable (v8): after the Welcome burst, the account the server verified (name, kAcct* flags)
        AdminCmd         = 47, // C->S reliable (v8): "may I run this admin command?" (the overlay's quick actions and Cars page)
        AdminResult      = 48, // S->C reliable (v8): the answer to one AdminCmd
        InvState         = 49, // S->C reliable (v8, 0.6 inventory): the player's whole inventory + the last op the server applied
        InvOp            = 50, // C->S reliable (v8, 0.6 inventory): one inventory operation (move, split, use, drop, bind, ...)
        InvUsed          = 51, // S->C reliable (v8, 0.6 inventory): an item the player used; the client applies its satiety
        InvSnap          = 52, // S->C reliable (v8, 0.5.4 inventory world): InvState with weapons (codec v2), a revision, a strip list
        InvNearby        = 53, // S->C reliable (v8, 0.5.4): the world containers in reach (a ground pile, a trunk); empty = none
        InvReport        = 54, // C->S reliable (v8, 0.5.4): the ped's weapons, current weapon and nearby vehicle classes
    };
    constexpr uint8_t kMsgLast = (uint8_t)Msg::InvReport;

    // Wire compatibility: values 1..6 are the original v1 set. Banned (7) and
    // TooManyConnections (8) were added without bumping kVersion, because they only
    // ever appear in a Refuse, after which the connection ends anyway. A client
    // built before them rejects such a Refuse as malformed (unknown reason), so it
    // gets no Refused event and no reason text, but the server's disconnect data
    // (kDisconnectRefused) still ends its session as Disconnected(Refused). New
    // clients show the text. A reason added later must go through the same
    // reasoning, or bump kVersion.
    enum class RefuseReason : uint8_t
    {
        VersionMismatch = 1, // Hello carried another protocol version
        BadName         = 2, // name fails ValidName
        NameTaken       = 3, // another joined player has it (case-insensitive)
        ServerFull      = 4, // maxPlayers joined already
        Protocol        = 5, // malformed or out-of-order message
        HelloTimeout    = 6, // no Hello in time
        Banned          = 7, // the name or the address is on the server's ban list
        TooManyConnections = 8, // the per-address connection cap is reached
        // v8 (added with the version bump): the platform account check of JoinAuth.
        AccountRequired = 9,  // the server requires an account (server.json requireAccount) and JoinAuth carried no token
        AuthFailed      = 10, // the platform refused the token (unknown, expired, already used, another server's)
        AuthUnavailable = 11, // the platform could not be asked (down, misconfigured): the server fails closed
    };

    // Why a player left, as told to the others in PlayerLeft.
    enum class LeaveReason : uint8_t
    {
        Quit    = 1, // said goodbye
        Timeout = 2, // vanished
        Kicked  = 3, // the server cut it off (kick, ban, rule violation)
    };

    // Position (m), heading (degrees; [-360, 360] in a ValidKinematics input, [0, 360) after
    // any trip over the wire) and velocity (m/s). On the wire this is a QKin (quant.h).
    struct Kinematics
    {
        float x = 0, y = 0, z = 0;
        float heading = 0;
        float vx = 0, vy = 0, vz = 0;
    };

    // The client's receipt for WorldState frames, carried in every PlayerState: `tick` is the
    // newest WorldState tick fully decoded (0 = none yet) and bit i of `bits` says whether tick
    // `tick - 1 - i` was decoded too. The server only uses a frame as a delta baseline once it
    // is acknowledged here, so a lost frame can never corrupt a later one.
    struct WorldAck { uint32_t tick = 0; uint32_t bits = 0; };

    // ---- on-foot state (v3) ---------------------------------------------------
    // PedState.move: what the player's legs are doing (3 bits on the wire; 7 is refused).
    constexpr uint8_t kMoveIdle            = 0;
    constexpr uint8_t kMoveWalk            = 1;
    constexpr uint8_t kMoveRun             = 2;
    constexpr uint8_t kMoveSprint          = 3;
    constexpr uint8_t kMoveInVehicle       = 4;
    constexpr uint8_t kMoveEnteringVehicle = 5;
    constexpr uint8_t kMoveExitingVehicle  = 6;
    constexpr uint8_t kMoveMax             = kMoveExitingVehicle;

    // PedState.flags (u16). All 16 bits are defined. Bit 15 (kPfVaulting) was reserved until the 0.4.3 fixes and was
    // added WITHOUT a version bump: client and server ship together (updates are mandatory and lockstep), so an
    // old peer that would refuse it cannot meet a new one. A flags bit beyond the 16th needs a new field (v6).
    constexpr uint16_t kPfJumping   = 1u << 0;  // a jump is in progress (the start is jumpSeq)
    constexpr uint16_t kPfRagdoll   = 1u << 1;
    constexpr uint16_t kPfAiming    = 1u << 2;  // the aim point travels (PedState.aim*)
    constexpr uint16_t kPfShooting  = 1u << 3;
    constexpr uint16_t kPfReloading = 1u << 4;
    constexpr uint16_t kPfFalling   = 1u << 5;
    constexpr uint16_t kPfParachute = 1u << 6;  // freefall or parachuting
    constexpr uint16_t kPfClimbing  = 1u << 7;  // IS_PED_CLIMBING (a climb up a wall; a vault is kPfVaulting)
    constexpr uint16_t kPfLadder    = 1u << 8;
    constexpr uint16_t kPfCover     = 1u << 9;
    constexpr uint16_t kPfDead      = 1u << 10; // v4, in a WorldEntry: the server holds the player dead (or its own client says so)
    constexpr uint16_t kPfStealth   = 1u << 11; // crouched / stealth movement
    constexpr uint16_t kPfDriver    = 1u << 12; // in a vehicle's driver seat (only with move >= kMoveInVehicle)
    constexpr uint16_t kPfSwimming  = 1u << 13; // v5: swimming (surface or diving)
    constexpr uint16_t kPfMelee     = 1u << 14; // v5: a melee action (a punch, a swing) is in progress
    constexpr uint16_t kPfVaulting  = 1u << 15; // 0.4.3: IS_PED_VAULTING (a vault or slide over an obstacle; kPfClimbing is the climb)
    constexpr uint16_t kPfKnown     = 0xFFFFu;

    // True for the two moves that name a vehicle (v5): entering (5) and exiting (6).
    constexpr bool MoveNamesVehicle(uint8_t move) { return move == kMoveEnteringVehicle || move == kMoveExitingVehicle; }

    // The aim point is sent relative to the player's position, in decimetres per axis as an i16:
    // +-3276.7 m reach at 10 cm steps (an aim error of under 0.1 degree at 30 m). A longer offset is
    // scaled down along its own direction until its largest axis fits, so the direction survives.
    constexpr int32_t kAimPerMeter = 10;
    constexpr float   kMaxAimMeters = 3276.7f; // per axis after scaling

    // What the player's ped is doing, beyond where it is (Kinematics). Defaults are "standing,
    // nothing happening, weapon unknown", so code that never fills it in still works.
    //   move     kMove* (0..kMoveMax)
    //   flags    kPf* bit set
    //   jumpSeq  a counter the client increments at every jump START; only its low 2 bits travel,
    //            so a plain ++ wraps. A receiver that sees it change plays a jump even when the
    //            packet carrying kPfJumping was lost.
    //   weapon   the selected weapon's GTA hash (any u32; 0 = not reported)
    //   aimX/Y/Z the aim point minus the player's position, metres. Only meaningful (and only
    //            sent) while kPfAiming is set; a decoded state has them 0 otherwise.
    //   shotSeq  (v4) a counter the client increments at every shot fired; only its low 4 bits
    //            travel, so a plain ++ wraps. A receiver plays one shot per increment it sees (up to
    //            15 between two received samples), so a lost packet loses no muzzle flash.
    //   health, armor (v4) SERVER-OWNED: only a WorldEntry carries them (the server's values, never
    //            the client's); a PlayerState does not, and a decoded PlayerState has the defaults.
    //            health is hit points above GTA's death threshold (GTA entity health 100 + health; 0 =
    //            dead), armor is GTA armour; both 0..255 on the wire, the server's maximum is
    //            Config::playerHealth (200 by default since v5, announced in Welcome.playerHealth).
    //   vehId    (v5) the networked vehicle id (0..255) a ped is entering or exiting; travels only while
    //            move is kMoveEnteringVehicle or kMoveExitingVehicle (MoveNamesVehicle), and is 0 for
    //            every other move (a decoded PedState has 0 there whatever was set).
    constexpr uint8_t kDefaultHealth = 100;
    struct PedState
    {
        uint8_t  move = kMoveIdle;
        uint16_t flags = 0;
        uint8_t  jumpSeq = 0;
        uint32_t weapon = 0;
        float    aimX = 0, aimY = 0, aimZ = 0;
        uint8_t  shotSeq = 0;
        uint8_t  health = kDefaultHealth;
        uint8_t  armor = 0;
        uint8_t  vehId = 0;
    };

    // ---- appearance (v3) ------------------------------------------------------
    // A player's look: what GTA's SET_PLAYER_MODEL, SET_PED_COMPONENT_VARIATION (12 components),
    // SET_PED_PROP_INDEX (8 anchors) and SET_PED_HEAD_BLEND_DATA need. ValidLook() bounds it.
    constexpr size_t   kLookComponents  = 12;
    constexpr size_t   kLookProps       = 8;
    constexpr uint16_t kMaxLookDrawable = 1023; // component and prop drawable index
    constexpr uint8_t  kMaxLookTexture  = 63;
    constexpr uint8_t  kMaxLookPalette  = 3;
    constexpr uint8_t  kMaxHeadBlendId  = 63;   // shape/skin parent ids (GTA has 0..45)

    struct LookComponent { uint16_t drawable = 0; uint8_t texture = 0; uint8_t palette = 0; };
    struct LookProp      { int16_t drawable = -1; uint8_t texture = 0; }; // drawable -1 = none (texture then 0)
    // Parent ids for the face shape and skin tone; the mixes are 0..255 for GTA's 0.0..1.0.
    struct HeadBlend
    {
        uint8_t shape1 = 0, shape2 = 0, shape3 = 0;
        uint8_t skin1 = 0, skin2 = 0, skin3 = 0;
        uint8_t shapeMix = 0, skinMix = 0, thirdMix = 0;
    };
    struct Look
    {
        uint32_t      model = 0; // GTA ped model hash; 0 is refused by ValidLook
        LookComponent components[kLookComponents];
        LookProp      props[kLookProps];
        HeadBlend     head;
    };
    bool operator==(const Look& a, const Look& b);
    inline bool operator!=(const Look& a, const Look& b) { return !(a == b); }

    // PlayerLook (C->S, reliable): the sender's own look. PeerLook (S->C, reliable): player `id`'s.
    struct PlayerLook { Look look; };
    struct PeerLook   { uint16_t id = 0; Look look; };

    struct Hello        { uint16_t version = kVersion; std::string name; };
    // `playerHealth` (v5, 1..255) is the server's maximum health (Config::playerHealth): what a player has
    // at join, after a heal or a respawn, in the health units of PedState (GTA entity health is 100 more).
    struct Welcome      { uint16_t yourId = 0; uint16_t tickHz = 0; std::string serverName; uint8_t playerHealth = kDefaultHealth; };
    struct Refuse       { RefuseReason reason = RefuseReason::Protocol; std::string text; };
    struct PlayerJoined { uint16_t id = 0; std::string name; };
    struct PlayerLeft   { uint16_t id = 0; LeaveReason reason = LeaveReason::Quit; };
    // `sampleMs` (v5): when the sender took this sample, in whole milliseconds of the SENDER's own
    // monotonic clock, modulo 65536. Only differences between consecutive samples mean anything; the
    // server maps it onto its own clock (libs/sync/sampleclock.h). Any value is legal.
    struct PlayerState  { uint32_t seq = 0; Kinematics k; WorldAck ack; PedState ped; uint16_t sampleMs = 0; };

    // One player in a WorldState. `k` is the dequantised state (position error <= 1 cm, see
    // quant.h). `ageMs` is how long before this packet was built the server received the sample
    // (0..255 ms, saturating): a client places the sample at tick time minus ageMs. The state is
    // never older than the sender's last PlayerState, and an idle sender's age keeps growing.
    // `ped` is the sender's on-foot state of the same sample (aim dequantised to 0.1 m).
    struct WorldEntry   { uint16_t id = 0; Kinematics k; uint8_t ageMs = 0; PedState ped; };
    // One WorldState frame for one receiver: `players` are the entries (ascending id) the server
    // chose to send this tick, `left` the ids that stopped being relevant (their ped can go;
    // coarse updates follow). The receiver's own id never appears.
    struct WorldState   { uint32_t tick = 0; std::vector<uint16_t> left; std::vector<WorldEntry> players; };
    struct ChatSend     { std::string text; };
    struct ChatMessage  { uint16_t fromId = kServerId; std::string text; };

    // One out-of-range player on the coarse channel: position only, in metres at the coarse
    // precision (x, y 0.5 m; z 0.08 m; quant.h).
    struct CoarseEntry  { uint16_t id = 0; float x = 0, y = 0, z = 0; };
    // A WorldCoarse packet: `players` ascending by id.
    struct CoarseState  { uint32_t tick = 0; std::vector<CoarseEntry> players; };

    // ---- resource transfer (reliable channel) ---------------------------------
    // One advertised resource. `crc32` is Crc32(bytes) (crc32.h); the client keeps a disk cache
    // keyed by name + crc32 and only requests entries it does not already have.
    struct ResourceInfo     { std::string name; uint32_t size = 0; uint32_t crc32 = 0; };
    // The resources the client should have. The 0-based position in `resources` (ascending) is the
    // index used by ResourceRequest and ResourceChunk, so the client must receive the manifest first.
    struct ResourceManifest { std::vector<ResourceInfo> resources; };
    // "Send me resource[index] from chunk startChunk onward" (startChunk supports resuming a transfer).
    struct ResourceRequest  { uint8_t index = 0; uint32_t startChunk = 0; };
    // One 1..kResourceChunk-byte slice of resource[index]; the last chunk is shorter. The client
    // knows the total (ceil(size / kResourceChunk)) from the manifest, so no count travels here.
    struct ResourceChunk    { uint8_t index = 0; uint32_t chunkIndex = 0; std::vector<uint8_t> data; };

    // ---- vehicles (v3, live in v4) ---------------------------------------------
    // GAGE keeps vehicle MOTION driver-authoritative (the client in seat 0, per the server's own
    // occupancy, sends VehicleUpdate and the server copies it into the vehicle, as every GTA
    // multiplayer does), but EXISTENCE, occupancy, lock, lights, fuel and colours are the server's
    // alone, and health can only go down from the driver's side (hits and admin set it otherwise).
    constexpr size_t   kVehSeats = 8;            // seat 0 = driver, 1..7 passengers
    constexpr uint16_t kMaxVehicleHealth = 1000; // body / engine health, GTA's 0..1000 scale
    constexpr float    kMaxAngVel = 127.0f / 16.0f; // rad/s per axis on the wire (saturates beyond)

    // VehicleState.flags (u8). Locked and lights are server-owned (VehicleCmd); engine, siren and
    // horn are the driver's (VehicleUpdate). Bits 5..7 are reserved: Decode refuses them.
    constexpr uint8_t kVehLocked = 1u << 0; // doors locked
    constexpr uint8_t kVehLights = 1u << 1; // headlights on
    constexpr uint8_t kVehEngine = 1u << 2; // engine running
    constexpr uint8_t kVehSiren  = 1u << 3; // siren on
    constexpr uint8_t kVehHorn   = 1u << 4; // horn sounding
    constexpr uint8_t kVehKnown  = (1u << 5) - 1;
    constexpr uint8_t kVehDriverFlags = kVehEngine | kVehSiren | kVehHorn;

    // VehicleState.flags2 / VehicleUpdate.flags2 (v5, u8, CONTROLLER-owned: what the vehicle's controlling client
    // sees on its own car). v6 uses the last two bits (brake lights, handbrake): none is reserved any more.
    constexpr uint8_t kVeh2Headlights  = 1u << 0; // headlights actually on (the controller's game state)
    constexpr uint8_t kVeh2HighBeam    = 1u << 1;
    constexpr uint8_t kVeh2Interior    = 1u << 2; // interior light
    constexpr uint8_t kVeh2IndicatorL  = 1u << 3;
    constexpr uint8_t kVeh2IndicatorR  = 1u << 4;
    constexpr uint8_t kVeh2RoofDown    = 1u << 5; // convertible roof lowered
    constexpr uint8_t kVeh2Brake       = 1u << 6; // v6: brake lights on (the driver's foot brake while rolling forward, or the handbrake)
    constexpr uint8_t kVeh2Handbrake   = 1u << 7; // v6: the driver holds the handbrake (the copy's wheels lock; clear = they roll free)
    constexpr uint8_t kVeh2Known       = 0xFF;
    // VehicleState.doors / VehicleUpdate.doors (v5, u8): which doors stand open. Bits 6..7 are reserved.
    constexpr uint8_t kDoorFrontLeft   = 1u << 0;
    constexpr uint8_t kDoorFrontRight  = 1u << 1;
    constexpr uint8_t kDoorRearLeft    = 1u << 2;
    constexpr uint8_t kDoorRearRight   = 1u << 3;
    constexpr uint8_t kDoorHood        = 1u << 4;
    constexpr uint8_t kDoorTrunk       = 1u << 5;
    constexpr uint8_t kDoorsKnown      = (1u << 6) - 1;

    // v6 visual damage (VehicleState / VehicleUpdate `tyres`, `tyresRim`, `windows`; Hit.bone for a vehicle Hit). Wheel and
    // window numbers are the game's own slots 0..7 (a car: 0/1 front left/right, 4/5 rear, 2/3 the middle pair of a
    // 6-wheeler; a bike: 0 front, 4 rear; windows 0..5 front/rear doors, windscreens): the wire never names a model.
    constexpr size_t kVehWheelSlots  = 8;
    constexpr size_t kVehWindowSlots = 8;
    // What a vehicle Hit reports besides body damage: `Hit.bone` = (part << 8) | slot, -1 (or anything else) = none.
    enum class VehiclePart : uint8_t { None = 0, Tyre = 1, TyreRim = 2, Window = 3 };
    constexpr int16_t VehiclePartCode(VehiclePart part, unsigned slot) { return (int16_t)(((unsigned)part << 8) | (slot & 0xFFu)); }
    // False for a plain bone (-1 or a ped bone id) and for a slot out of range.
    inline bool ParseVehiclePart(int16_t bone, VehiclePart& part, unsigned& slot)
    {
        if (bone < 0) return false;
        const unsigned hi = ((unsigned)bone >> 8) & 0xFFu, lo = (unsigned)bone & 0xFFu;
        if (hi < (unsigned)VehiclePart::Tyre || hi > (unsigned)VehiclePart::Window) return false;
        if (lo >= (hi == (unsigned)VehiclePart::Window ? kVehWindowSlots : kVehWheelSlots)) return false;
        part = (VehiclePart)hi;
        slot = lo;
        return true;
    }

    // ---- R3 T4: vehicle damage state (protocol v7, additive; docs/api/protocol.md "R3 T4") --------------------------------------
    // What decides how a damaged car LOOKS beyond tyres and windows, at a low byte cost: the petrol tank's health (leak / fire stage),
    // which doors were broken off and which headlights are broken, and a coarse dent map (kVehDentProbes fixed probe points around the
    // hull, each the controller's GET_VEHICLE_DEFORMATION_AT_POS depth in 2 mm steps). Body and engine health are the existing
    // VehicleState / VehicleUpdate fields. It is a STATIC block: the full VehicleWorld entry carries it (3 bytes for a clean car, at
    // most 11), a VehicleUpdate carries it only when VehicleUpdate.flags has kVehDamageBlock (on a change, on a hand-over and as a
    // slow refresh), a motion-only entry never.
    //   petrol  petrol tank health / 4 (0..kPetrolFullWire = 250; 251..255 refused)
    //   parts   bit i (0..5) = door i broken off (the kDoor* order); kVehDmgLampL / kVehDmgLampR = that headlight is broken
    //   dent[i] probe i's dent depth, 0 = none, 1..255 = 2 mm steps (max 0.51 m)
    // On the wire: u8 petrol, u8 parts, u8 dentMask (bit i = probe i has a dent), then one u8 (1..255) per set bit, ascending.
    constexpr size_t  kVehDentProbes = 8;
    constexpr uint8_t kPetrolFullWire = 250;
    constexpr uint8_t kVehDmgDoorsMask = 0x3F;
    constexpr uint8_t kVehDmgLampL = 1u << 6;
    constexpr uint8_t kVehDmgLampR = 1u << 7;
    constexpr float   kVehDentStepM = 0.002f;
    constexpr uint8_t kVehDamageBlock = 1u << 5;   // VehicleUpdate.flags only (never a VehicleState flag): a damage block follows `windows`
    struct VehicleDamage
    {
        uint8_t petrol = kPetrolFullWire;
        uint8_t parts = 0;
        uint8_t dent[kVehDentProbes] = {};
        bool operator==(const VehicleDamage& o) const
        {
            if (petrol != o.petrol || parts != o.parts) return false;
            for (size_t i = 0; i < kVehDentProbes; i++)
                if (dent[i] != o.dent[i]) return false;
            return true;
        }
        bool operator!=(const VehicleDamage& o) const { return !(*this == o); }
        bool Any() const { return *this != VehicleDamage(); }
        uint8_t DentMask() const
        {
            uint8_t m = 0;
            for (size_t i = 0; i < kVehDentProbes; i++)
                if (dent[i]) m = (uint8_t)(m | (1u << i));
            return m;
        }
    };
    // Encoded size of the block (3 + one byte per dented probe).
    inline size_t VehicleDamageBytes(const VehicleDamage& d)
    {
        size_t n = 3;
        for (size_t i = 0; i < kVehDentProbes; i++)
            if (d.dent[i]) n++;
        return n;
    }
    inline bool VehicleDamageValid(const VehicleDamage& d) { return d.petrol <= kPetrolFullWire; }

    // A rotation (unit quaternion, GTA's GET_ENTITY_QUATERNION order x, y, z, w). On the wire it is
    // a smallest-three u32 (quant.h): at most 0.28 degrees of rotation error.
    struct Quat { float x = 0, y = 0, z = 0, w = 1; };

    // What a client may ask the server to do with a vehicle (VehicleCmd.op). The server validates
    // every request and answers each with a VehicleResult; nothing is obeyed on trust.
    enum class VehicleOp : uint8_t
    {
        Spawn      = 1, // arg = GTA vehicle model hash (non-zero); the server assigns the id and places it 4 m ahead of the requester
        Despawn    = 2, // remove vehId: its spawner only (occupants are put out)
        Enter      = 3, // become an occupant of vehId (arg = seat 0..7, 0 = driver): seat free, within 10 m, not locked (its spawner and last driver may enter it locked)
        Exit       = 4, // leave vehId (the vehicle you are in); a driver's vehicle parks with its last state
        SetLocked  = 5, // arg != 0 locks vehId's doors, 0 unlocks: the player in its driver's seat, its spawner or (0.4.22) its last driver; from outside within server.json vehicleLockDistance (TooFar), else NotAllowed
        SetLights  = 6, // arg != 0 turns vehId's headlights on, 0 off: its driver or spawner
        SetColours = 7, // v4: arg = primary | secondary << 8 (GTA colour indices): its driver or spawner
        Register   = 8, // v4: adopt a local (ambient) car: arg = model hash, the tail carries where it is; the server
                        // creates a networked vehicle there and seats the requester in `seat`
        Release    = 9, // v5: give up ownership of vehId (its owner only): anybody may drive it from then on
        EnterIntent  = 10, // T1 seats (wire v6): "I am about to get into seat N of vehId" (arg = seat | kIntentRun): the server RESERVES the
                           // seat (same checks as Enter: range, NotOwner, taken or reserved by someone else, distance, lock, dead, not
                           // already seated), answers Ok or the refusal, and tells every relevant client with a SeatNotice::Reserved.
                           // The reservation ends with the player's Enter (SeatNotice::Seated), CancelIntent, Exit, a new intent, death,
                           // disconnect, the vehicle's despawn or kIntentTtlMs (SeatNotice::Cancelled). It never seats anybody.
        CancelIntent = 11, // T1 seats (wire v6): drop my reservation on vehId (Ok also when there is none); Exit while a reservation is
                           // pending is the same
    };
    constexpr uint8_t kVehicleOpLast = (uint8_t)VehicleOp::CancelIntent;
    // EnterIntent.arg: bits 0..7 the wire seat (0..kVehSeats-1), bit 8 kIntentRun (the player ran to the door; the replay walks
    // when clear), every other bit reserved and refused (BadArg).
    constexpr uint32_t kIntentRun = 0x100u;
    constexpr uint32_t kIntentArgMask = 0x1FFu;
    constexpr uint32_t kIntentTtlMs = 6000;  // a reservation the player never completes or cancels is dropped by the server after this

    // One networked vehicle as a client sees it in a VehicleWorld full entry.
    //   id            0..kMaxVehicles-1
    //   model         GTA vehicle model hash
    //   k             position (1 cm) and velocity (1/32 m/s) as for players; k.heading is NOT on the
    //                 wire: Decode fills it with the yaw of `rot` (degrees, GTA heading) for convenience
    //   rot           rotation (smallest-three quaternion on the wire, quant.h)
    //   avx/avy/avz   angular velocity, rad/s, 1/16 rad/s steps, saturating at +-kMaxAngVel
    //   steer         steering, -1 (full left) .. +1 (full right), 1/127 steps (SET_VEHICLE_STEER_BIAS)
    //   flags         kVeh* bit set
    //   bodyHealth, engineHealth  0..kMaxVehicleHealth (GTA scale), server-owned
    //   fuelPermille  0..1000, server-owned (drains while the engine runs and the vehicle moves)
    //   colour1/2     primary / secondary GTA colour index
    //   seats         occupant player id per seat, kNoPlayer when empty; seats[0] is the driver
    //   ageMs         (v5) how long before the frame's tick time the motion (k, rot, avx..avz, steer) was sampled:
    //                 a client places it at tick time minus ageMs (0..255, saturating)
    //   ctrl          (v5) the player whose client controls this vehicle's motion (the one that sends
    //                 VehicleUpdate for it), kNoPlayer = nobody: the vehicle stands still. Usually the driver; an
    //                 empty vehicle keeps its last driver while that player is near, else the nearest player takes it
    //   ownerId       (v5) the player who owns the vehicle (spawned it and has not released it), kNoPlayer = nobody.
    //                 Only the owner may take its driver's seat (VehicleResultCode::NotOwner)
    //   flags2, doors (v5) kVeh2* and kDoor* bit sets, the controller's lights and open doors; v6 adds brake lights and
    //                 the handbrake (kVeh2Brake / kVeh2Handbrake) to flags2
    //   tyres, tyresRim, windows (v6) per-wheel / per-window visual damage, bit i = slot i: `tyres` bit set = the tyre is
    //                 burst (flat), `tyresRim` bit set = completely burst, off the rim (always a subset of `tyres`:
    //                 Decode refuses a rim bit without its tyre bit), `windows` bit set = smashed. The server merges the
    //                 controller's report with the Hits that popped something of a car the shooter does not control
    //                 (VehicleStore::HitPart); full entries only, a change rides the usual 3 fresh frames
    //   dmg           (v7) the VehicleDamage block (petrol tank health, broken doors / headlights, dent probes); full entries only,
    //                 the server merges the controller's report monotonically (damage only goes up, as body / engine health)
    //   motionOnly    (v5) decode only: this entry was a motion-only entry (header 2) carrying id, ageMs, k, rot,
    //                 avx..avz, steer and flags; every other field above is NOT in it and holds a default, and the
    //                 receiver keeps what it already has. Encode writes the motion-only form for a state with this
    //                 set. flags is complete (kVeh* bits), flags2 and doors are not in a motion-only entry
    //                 (nor are tyres, tyresRim and windows, v6)
    struct VehicleState
    {
        uint16_t   id = 0;
        uint32_t   model = 0;
        Kinematics k;
        Quat       rot;
        float      avx = 0, avy = 0, avz = 0;
        float      steer = 0;
        uint8_t    flags = 0;
        uint16_t   bodyHealth = kMaxVehicleHealth;
        uint16_t   engineHealth = kMaxVehicleHealth;
        uint16_t   fuelPermille = 1000;
        uint8_t    colour1 = 0, colour2 = 0;
        uint16_t   seats[kVehSeats] = {kNoPlayer, kNoPlayer, kNoPlayer, kNoPlayer, kNoPlayer, kNoPlayer, kNoPlayer, kNoPlayer};
        uint8_t    ageMs = 0;
        uint16_t   ctrl = kNoPlayer;
        uint16_t   ownerId = kNoPlayer;
        uint8_t    flags2 = 0;
        uint8_t    doors = 0;
        uint8_t    tyres = 0, tyresRim = 0, windows = 0;   // v6
        VehicleDamage dmg;                                  // v7 (R3 T4): petrol tank, broken doors / headlights, dents (full entries only)
        bool       motionOnly = false;
    };

    // A VehicleWorld frame (S->C, state channel), per receiver (v4):
    //   present   ascending: the COMPLETE set of vehicles relevant to you now (within the relevance
    //             radius, or one you sit in). A vehicle you hold that is not listed is out of your
    //             range: stream it out (it still exists). Every frame is complete, so a lost one
    //             self-corrects with the next.
    //   vehicles  ascending, each id also in `present`: the states due this tick (moving ones at
    //             the distance rate, parked ones about once a second, any change or newcomer at once
    //             and for 3 frames). A present vehicle without a state keeps what you have. v5: an entry
    //             is a full state or, for a vehicle that moves and whose static fields (model, health,
    //             fuel, colours, seats, ctrl, owner, flags2, doors, v6 tyres/tyresRim/windows) did not change, a motion-only entry
    //             (VehicleState::motionOnly); a newcomer, a change and one entry a second are full.
    //   removed   ascending, never also present: vehicles that were DESTROYED (despawned), repeated in
    //             3 frames. Apply removals before anything else.
    struct VehicleWorld
    {
        uint32_t tick = 0;
        std::vector<uint16_t>     removed;
        std::vector<uint16_t>     present;
        std::vector<VehicleState> vehicles;
    };

    // A VehicleCmd (C->S, reliable): a client's request about a vehicle. `op` is a VehicleOp, `arg`
    // is op-specific (see VehicleOp), `vehId` names the target (ignored for Spawn and Register, where
    // the server assigns it). Only Register carries a position (x, y, z, rot: where the local car
    // is), colours and a seat; for every other op nothing about placement is trusted from the client.
    struct VehicleCmd
    {
        uint16_t  vehId = 0;
        VehicleOp op = VehicleOp::Spawn;
        uint32_t  arg = 0;
        // Register only (the wire tail): position (1 cm, the player box), rotation, colours, seat 0..7.
        float     x = 0, y = 0, z = 0;
        Quat      rot;
        uint8_t   colour1 = 0, colour2 = 0, seat = 0;
    };

    // VehicleUpdate (C->S, state channel, v4): the controlling client's own vehicle, every send tick.
    // Accepted only from the player the SERVER has as the vehicle's controller (v5: VehicleState::ctrl,
    // normally its driver; v4: the player in seat 0), with a seq newer than the last accepted
    // VehicleUpdate from that player, inside the world box and under Config::vehicleMaxSpeed. The
    // server copies position, velocity, rotation, angular velocity, steering, the driver flags
    // (kVehDriverFlags; the others in `flags` are ignored) and, v5, flags2 and doors; a health can only
    // go down this way. v6: `tyres`, `tyresRim` and `windows` are what the controller's own car shows (the server merges
    // them with the visual-damage Hits it took for this car; a bit the controller does not show is dropped once it
    // had its chance to show it, see VehicleStore::Update). `sampleMs` (v5): the sender's clock when the sample was taken, as in PlayerState.
    struct VehicleUpdate
    {
        uint32_t   seq = 0;
        uint16_t   vehId = 0;
        Kinematics k; // heading ignored (the rotation carries it)
        Quat       rot;
        float      avx = 0, avy = 0, avz = 0;
        float      steer = 0;
        uint8_t    flags = 0;
        uint16_t   bodyHealth = kMaxVehicleHealth;
        uint16_t   engineHealth = kMaxVehicleHealth;
        uint16_t   sampleMs = 0;
        uint8_t    flags2 = 0;
        uint8_t    doors = 0;
        uint8_t    tyres = 0, tyresRim = 0, windows = 0;   // v6
        VehicleDamage dmg;                                  // v7 (R3 T4): meaningful only when hasDmg
        bool       hasDmg = false;                          // v7: the update carries the damage block (flags bit kVehDamageBlock on the wire)
    };

    // The answer to every VehicleCmd, to its sender (reliable): `op` and `vehId` (the assigned id for
    // Spawn / Register when Ok) and why it was refused.
    enum class VehicleResultCode : uint8_t
    {
        Ok           = 0,
        NoVehicle    = 1,  // vehId does not exist
        SeatTaken    = 2,
        TooFar       = 3,  // farther than Config::vehicleEnterDistance from the vehicle (or a Register far from you)
        Locked       = 4,
        NotAllowed   = 5,  // not the spawner / driver, or not your vehicle
        Limit        = 6,  // the per-player or global vehicle cap is reached
        RateLimited  = 7,  // too many commands
        BadArg       = 8,  // model 0, seat out of range, bad position, ...
        Dead         = 9,  // dead players cannot do this
        NotInVehicle = 10, // Exit while not in that vehicle
        NoState      = 11, // the server has no position of yours yet (send a PlayerState first)
        NotOwner     = 12, // v5: the driver's seat of an owned vehicle belongs to its owner (Enter, seat 0)
    };
    constexpr uint8_t kVehicleResultLast = (uint8_t)VehicleResultCode::NotOwner;
    struct VehicleResult
    {
        VehicleOp         op = VehicleOp::Spawn;
        VehicleResultCode code = VehicleResultCode::Ok;
        uint16_t          vehId = 0;
    };

    // T1 seats (wire v6): the server's account of a player's enter / exit INTENT, to every client that has the vehicle in range
    // (and to the actor), reliable and in order with the server's other reliable traffic. VehicleWorld.seats stays the only
    // truth about OCCUPANCY; this carries what the seat list cannot: that somebody is ON THE WAY into a seat (so every screen
    // can play the real enter task instead of a warp), which seat, and when an intent died without a seating.
    //   Reserved   `player` reserved `seat` of `vehId` (detail bit 0 kSeatNoticeRun: it ran to the door). Replaces that player's
    //              older reservation (a Cancelled/Replaced for the old one precedes it).
    //   Seated     the reservation became occupancy: `player` now holds `seat` (the VehicleWorld names it from the same tick on).
    //   Cancelled  the reservation ended without a seating; detail = SeatCancelReason.
    //   Exited     `player` left `seat` by its own Exit (the normal exit animation is due; a death, disconnect or despawn is NOT
    //              announced this way, those only drop the seat from the list).
    // Decode refuses an unknown kind, a player at or above kMaxPlayers, a vehicle at or above kMaxVehicles, a seat at or above
    // kVehSeats, and a detail that is not defined for the kind (Reserved: only bit 0; Cancelled: a SeatCancelReason; others 0).
    enum class SeatNoticeKind : uint8_t { Reserved = 1, Seated = 2, Cancelled = 3, Exited = 4 };
    enum class SeatCancelReason : uint8_t
    {
        ByPlayer    = 0, // CancelIntent / Exit while pending
        Expired     = 1, // kIntentTtlMs passed
        Replaced    = 2, // the same player reserved another seat (or entered another seat / vehicle)
        VehicleGone = 3, // the vehicle was despawned
        PlayerGone  = 4, // the player disconnected, died or was put elsewhere
    };
    constexpr uint8_t kSeatCancelLast = (uint8_t)SeatCancelReason::PlayerGone;
    constexpr uint8_t kSeatNoticeRun = 1;
    struct SeatNotice
    {
        SeatNoticeKind kind = SeatNoticeKind::Reserved;
        uint16_t       player = kNoPlayer;
        uint16_t       vehId = 0;
        uint8_t        seat = 0;
        uint8_t        detail = 0;
    };

    // ---- combat (v4): server-owned health -------------------------------------
    // A shooter's client reports each hit its game registered (Hit); the server validates it
    // (shooter and target alive, target relevant to the shooter, within Config::maxHitDistance, at
    // most 30 hits a second, damage clamped to kMaxHitDamage) and applies it: armour first, then
    // health. A Hit whose target is the sender itself is environmental damage (a fall, fire, a crash):
    // always accepted while alive, without the range checks. A vehicle hit takes `damage` off the
    // body health and half of it off the engine health. v6: a vehicle Hit may also name a part in `bone`
    // (VehiclePartCode: a tyre burst, a tyre off its rim, a smashed window): the shooter's client saw that
    // happen on its copy of a car it does not control, and once the Hit passes the same checks the server
    // sets that bit of the car's visual state for everybody (VehicleStore::HitPart). A bone that is not a part
    // is ignored for vehicles, as before.
    enum class HitKind : uint8_t { Player = 1, Vehicle = 2 };
    constexpr uint16_t kMaxHitDamage = 250;
    struct Hit
    {
        HitKind  kind = HitKind::Player;
        uint16_t targetId = 0;
        uint32_t weapon = 0;   // GTA weapon hash (or the cause: WEAPON_FALL, ...)
        uint16_t damage = 0;   // health units (GTA weapon damage); 0 is refused, above kMaxHitDamage is clamped
        int16_t  bone = -1;    // GTA ped bone id hit (-1 = unknown); carried for the gamemode, not checked
        uint8_t  shotSeq = 0;  // the shooter's shot counter (PedState.shotSeq) of the shot that hit
    };
    // Damage.flags (v5, u8): what kind of damage it was. Bits 4..7 are reserved: Decode refuses them.
    constexpr uint8_t kDmgHeadshot  = 1u << 0; // the Hit named the head bone (SKEL_Head) and got the headshot bonus
    constexpr uint8_t kDmgMelee     = 1u << 1; // a punch, a kick or a melee weapon
    constexpr uint8_t kDmgExplosion = 1u << 2;
    constexpr uint8_t kDmgArmour    = 1u << 3; // armour absorbed (part of) the damage
    constexpr uint8_t kDmgKnown     = (1u << 4) - 1;
    // Killed.flags (v5, u8). Bits 2..7 are reserved: Decode refuses them.
    constexpr uint8_t kKilledHeadshot = 1u << 0;
    constexpr uint8_t kKilledMelee    = 1u << 1;
    constexpr uint8_t kKilledKnown    = (1u << 2) - 1;

    // To the victim, the attacker (once when they are the same) and, v5, every player for whom the
    // victim or the attacker is relevant (so onlookers can play the hit reaction): `damage` is what was
    // applied (after the clamp and the scaling), `health` / `armor` the victim's new values, `killed`
    // true when it died. attackerId is kServerId for an admin change (console `heal` / `kill`).
    // `bone` (v5) is the bone the shooter reported (-1 = unknown), `flags` the kDmg* set.
    struct Damage
    {
        uint16_t victimId = 0;
        uint16_t attackerId = kNoPlayer;
        uint32_t weapon = 0;
        uint16_t damage = 0;
        uint8_t  health = 0;
        uint8_t  armor = 0;
        bool     killed = false;
        int16_t  bone = -1;
        uint8_t  flags = 0;
    };
    // To everybody (kill feed): killerId == victimId for a suicide / environmental death, kServerId
    // for the console's `kill`. `flags` (v5): kKilled*.
    struct Killed { uint16_t victimId = 0; uint16_t killerId = kNoPlayer; uint32_t weapon = 0; uint8_t flags = 0; };
    // A dead player's request to come back (empty body). Refused (silently, counted) before
    // Config::respawnDelaySeconds have passed since the death, or when alive: send it again later.
    struct Respawn {};
    // You are alive again at x, y, z (metres), facing heading (degrees), with this health / armour.
    struct RespawnOk
    {
        float   x = 0, y = 0, z = 0, heading = 0;
        uint8_t health = kDefaultHealth;
        uint8_t armor = 0;
    };

    // ---- emotes (v4) -------------------------------------------------------------
    // TASK_PLAY_ANIM / TASK_START_SCENARIO_IN_PLACE for remote players. Names are 1..kMaxAnimName
    // characters of [A-Za-z0-9_@/.-] (ValidAnimName); Decode refuses a longer string, the server
    // drops a bad charset. The server relays at most 4 a second per player and remembers the current
    // LOOPING animation (durationMs -1) or scenario, which it replays to newcomers after their
    // PlayerJoined burst (like looks); a one-shot animation is relayed, not remembered.
    constexpr size_t kMaxAnimName = 63;
    struct Anim
    {
        std::string dict, name;
        float    blendIn = 8.0f;     // GTA blend-in speed; finite, |v| <= 1000
        float    blendOut = -8.0f;   // GTA blend-out speed; finite, |v| <= 1000
        int32_t  durationMs = -1;    // -1 = loop until stopped; otherwise 0.. (milliseconds)
        uint32_t flags = 0;          // GTA animation flags (1 loop, 16 upper body, 32 secondary, ...)
        uint8_t  playbackRate = 100; // 0..255 = 0.00..2.55 (x100 fixed point)
    };
    struct PlayAnim     { Anim anim; };
    struct PlayScenario { std::string name; };
    struct StopAnim     {};
    struct PeerAnim     { uint16_t id = 0; Anim anim; };
    struct PeerScenario { uint16_t id = 0; std::string name; };
    struct PeerAnimStop { uint16_t id = 0; };

    // ---- T4: vehicle collisions (docs/sync-specs/vehicle-collisions.md, docs/api/protocol.md "T4 vehicle impact") ----
    // The server resolves a contact between two vehicles that DIFFERENT clients simulate (it has both tracks on one time base) and tells
    // each controller its half, once per contact. Reliable; to the vehicle's controller only.
    //   seq        the server's impact counter (a duplicate or older one is ignored)
    //   vehId      the vehicle YOU control that was hit; otherId the vehicle it hit
    //   sampleMs   the contact time on YOUR sample clock (the VehicleUpdate.sampleMs domain: whole ms of your clock, wraps at 65536)
    //   dvx, dvy   the change of your vehicle's velocity (world x, y; m/s); dyaw the change of its yaw rate (rad/s, counter-clockwise)
    //   nx, ny     the contact normal from your vehicle toward the other one (unit)
    //   closing    the closing speed along the normal before the impact (m/s); depth the overlap of the two (m, 0 = they only touched)
    // On the wire dv is an i16 in 1/64 m/s, dyaw an i16 in 1/256 rad/s, n an i8 pair in 1/127, closing a u8 in 0.25 m/s, depth a u8 in cm.
    struct VehicleImpact
    {
        uint32_t seq = 0;
        uint16_t vehId = 0, otherId = 0;
        uint16_t sampleMs = 0;
        float    dvx = 0, dvy = 0, dyaw = 0;
        float    nx = 0, ny = 0;
        float    closing = 0, depth = 0;
    };
    constexpr size_t kVehicleImpactBytes = 21;

    // ---- R3 T5: link info (docs/api/protocol.md "R3 T5 link info") ----
    // The sender's stamp of a vehicle sample carries the sample time + the FASTEST uplink (SampleClock absorbs the constant part), and a
    // frame arrives the fastest downlink after its tick, so a car drawn at the newest data is still half its controller's and half our round
    // trip behind its owner's real position: 2-3 m at 25 m/s on a 100 ms path (R3 T5). The client knows its own round trip; the controller's
    // only the server does. Every ~1 s each client gets the smoothed round trip (ms) of the players that control a vehicle. Unreliable
    // (a lost one is replaced by the next), at most kMaxLinkEntries entries.
    struct LinkEntry { uint16_t id = 0; uint16_t rttMs = 0; };
    constexpr size_t kMaxLinkEntries = 32;
    struct LinkInfo
    {
        std::vector<LinkEntry> e;
    };

    // ---- v8: accounts (docs/api/protocol.md "v8 accounts") ----
    // JoinAuth (C->S reliable) follows Hello at once: the platform join token the launcher handed the engine, "" when there is none
    // (no launcher, demo mode). Printable ASCII without spaces (0x21..0x7E), at most kMaxJoinToken bytes; Decode refuses anything else.
    // The server answers with Welcome or a Refuse only after it has the JoinAuth, so a v8 client always sends it.
    struct JoinAuth { std::string token; };
    bool ValidJoinToken(const std::string& token);

    // AccountInfo (S->C reliable) closes the Welcome burst (after PlayerJoined / PeerLook / PeerAnim of the players already in).
    // `flags` is a kAcct* set (unknown bits are refused by Decode), `username` the verified account name (empty for a guest).
    constexpr uint8_t kAcctVerified = 1u << 0; // the platform verified this player's join token (username, roles are real)
    constexpr uint8_t kAcctAdmin    = 1u << 1; // the verified account holds an admin role (server.json adminRoles)
    constexpr uint8_t kAcctCommands = 1u << 2; // the server lets this player run the admin commands (AdminCmd): an admin, or any
                                               // player while the server knows no roles (requireAccount off, no token)
    constexpr uint8_t kAcctKnown    = kAcctVerified | kAcctAdmin | kAcctCommands;
    struct AccountInfo { uint8_t flags = 0; std::string username; };

    // AdminCmd (C->S reliable): the overlay's admin commands. The client runs most of them itself (they act on its own game);
    // the server decides whether it may and answers every one with an AdminResult (same seq and op). `arg` is the command's
    // argument as typed ("pistol", "adder", "1 2 3"), printable ASCII, at most kMaxAdminArg bytes. Heal is done by the server
    // itself (health is server-owned); the client runs the others after an Ok.
    enum class AdminOp : uint8_t
    {
        Heal   = 1, // /heal
        Fix    = 2, // /fix (repair the vehicle you are in)
        Spawn  = 3, // /spawn (back to the spawn point)
        Tp     = 4, // /tp x y z
        Noclip = 5, // /noclip
        Gun    = 6, // /gun <weapon>
        Car    = 7, // /car <model> (the Cars page)
    };
    constexpr uint8_t kAdminOpLast = (uint8_t)AdminOp::Car;
    struct AdminCmd { uint16_t seq = 0; AdminOp op = AdminOp::Heal; std::string arg; };
    enum class AdminCode : uint8_t
    {
        Ok          = 0, // allowed (Heal: done)
        NotAdmin    = 1, // the server knows your roles and none of them is an admin role
        RateLimited = 2, // too many admin commands
        BadArg      = 3, // the argument is not acceptable
        Failed      = 4, // allowed, but the server could not do it (Heal on a dead player)
    };
    constexpr uint8_t kAdminCodeLast = (uint8_t)AdminCode::Failed;
    struct AdminResult { uint16_t seq = 0; AdminOp op = AdminOp::Heal; AdminCode code = AdminCode::Ok; std::string text; };

    // ---- v8 (0.6 inventory, docs/api/protocol.md "Inventory") ----
    // The bytes inside are libs/sync/inventory.h's: `blob` is sync::inv::Encode(Character) (version-prefixed, the receiver's
    // sync::inv::Decode checks every rule), `op` is sync::inv::EncodeOp (exactly kInvOpBytes). The protocol layer only frames
    // and caps them, so the protocol library does not depend on the item rules.
    // InvState (S->C reliable): sent after AccountInfo, after every InvOp (applied or refused: ackSeq is that op's seq, so the
    // client drops its prediction up to it) and after a console give / clear. A server without inventories never sends it,
    // which the overlay shows as "unavailable".
    constexpr size_t kMaxInvBlob = 1024; // a full rucksack character is about 450 bytes
    constexpr size_t kInvOpBytes = 11;
    struct InvState { uint32_t ackSeq = 0; std::string blob; };
    // InvOp (C->S reliable): `seq` counts up from 1 per connection (the client's prediction keys on it).
    struct InvOp { uint32_t seq = 0; std::string op; };
    // InvUsed (S->C reliable): the server consumed one `item` for this player; `satiety` is what the client adds to its own
    // satiety (0..100; health effects are applied by the server itself and arrive as Vitals).
    struct InvUsed { uint16_t item = 0; uint8_t satiety = 0; };

    // ---- v8, 0.5.4 inventory world (docs/api/protocol.md "0.5.4"; the rules are libs/sync/inventory_world.h) ----
    // The handshake: right after the join's InvState the server sends an InvNearby (empty). A 0.5.3 client ignores message
    // types it does not know; a 0.5.4 client answers with an InvReport, and from then on the server sends that player InvSnap
    // (instead of InvState), InvNearby whenever what is in reach changes, and accepts the 19-byte InvOp form.
    // InvOp's op is kInvOpBytes (11) or kInvOpBytesExt (19: srcBox u32, dstBox u32 appended). The long form is legal exactly when the
    // op's src (byte 1) or dst (byte 4) is kInvWhereWorld (sync::inv::Where::Nearby), so 8 stray bytes after a plain op stay an error.
    constexpr size_t kInvOpBytesExt = 19;
    constexpr uint8_t kInvWhereWorld = 5;
    // InvSnap (S->C reliable): `blob` is sync::inv::Encode (codec v2: weapons, ammo, the hand). `rev` counts the server's
    // changes of this inventory (the client stamps its next InvReport with the last rev it applied). flags bit 0 = this
    // snapshot answers the client's own report (the client then leaves its ped's ammo and current weapon alone). `strip`:
    // weapon hashes the client must take off its ped (gained with no room: they went to the ground).
    constexpr size_t kMaxInvStrip = 16;
    constexpr uint8_t kInvSnapFromReport = 0x01;
    struct InvSnap { uint32_t ackSeq = 0; uint32_t rev = 0; uint8_t flags = 0; std::string blob; std::vector<uint32_t> strip; };
    // InvNearby (S->C reliable): up to kMaxInvNearby boxes in reach, in display order. `blob` is sync::inv::EncodeBox (empty
    // when locked). veh is the server vehicle id (kInvNoVehicle for a ground pile), cls its GTA class (255 = none), piles the
    // number of ground piles in reach (the nearest is the one sent), distCm the distance (0 when seated in the vehicle).
    constexpr size_t kMaxInvNearby = 4;
    constexpr uint16_t kInvNoVehicle = 0xFFFF;
    constexpr uint8_t kInvBoxLocked = 0x01;
    struct InvBox { uint32_t id = 0; uint8_t kind = 0; uint8_t flags = 0; uint16_t veh = kInvNoVehicle; uint32_t model = 0; uint8_t cls = 255;
                    uint8_t piles = 0; uint16_t distCm = 0; std::string blob; };
    struct InvNearby { std::vector<InvBox> boxes; };
    // InvReport (C->S reliable): what the client read off its ped after applying InvSnap `rev`. flags bit 0 = the ped is alive
    // and settled (without it the server reads only the vehicle classes). weapons: catalog weapons the ped has, each with its
    // GTA ammo type and that type's rounds. vehicles: the GTA class of server vehicles near the client (for trunk sizes).
    constexpr size_t kMaxInvReportWeapons = 64;
    constexpr size_t kMaxInvReportVehicles = 8;
    constexpr uint8_t kInvReportPedValid = 0x01;
    struct InvPedWeapon { uint32_t hash = 0; uint32_t ammoType = 0; uint16_t ammo = 0; };
    struct InvVehClass { uint16_t veh = 0; uint8_t cls = 0; };
    struct InvReport { uint32_t rev = 0; uint32_t current = 0; uint8_t flags = 0; std::vector<InvPedWeapon> weapons; std::vector<InvVehClass> vehicles; };

    using Bytes = std::vector<uint8_t>;

    // Encode never validates caps (the receiver's Decode does, so tests and
    // tools can build over-cap packets); an EMPTY result means the message cannot
    // be framed at all and must not be sent: a string over 65535 bytes, a non-finite
    // PlayerState float (an aim offset only counts while kPfAiming is set), a PedState.move above 7
    // (3 bits), a WorldState, CoarseState or VehicleWorld whose ids are not strictly ascending
    // or whose counts exceed the wire's. Encode(PlayerState) saturates positions and velocities
    // at the quantiser box and wraps the heading (quant.h), so an out-of-world value still
    // encodes and is then rejected by ValidKinematics at the receiver. A move of 7 and reserved
    // kPf bits are encoded as given (tests build hostile packets that way); Decode refuses them.
    // Encode(WorldState) writes every entry in full (no baseline); the server's delta frames come
    // from WorldPacketBuilder (worldcodec.h). v4: Encode of a VehicleWorld, VehicleUpdate or Register
    // VehicleCmd refuses a non-finite position, velocity, rotation, angular velocity or steering, a
    // vehicle id at or above kMaxVehicles, a full state whose id is not in `present`, or a removed
    // id that is also present; a zero-length rotation encodes as the identity. Encode(RespawnOk)
    // refuses a non-finite value; Encode of an Anim a non-finite blend.
    Bytes Encode(const Hello& m);
    Bytes Encode(const Welcome& m);
    Bytes Encode(const Refuse& m);
    Bytes Encode(const PlayerJoined& m);
    Bytes Encode(const PlayerLeft& m);
    Bytes Encode(const PlayerState& m);
    Bytes Encode(const WorldState& m);
    Bytes Encode(const ChatSend& m);
    Bytes Encode(const ChatMessage& m);
    Bytes Encode(const CoarseState& m);
    Bytes Encode(const ResourceManifest& m);
    Bytes Encode(const ResourceRequest& m);
    Bytes Encode(const ResourceChunk& m);
    Bytes Encode(const VehicleWorld& m);
    Bytes Encode(const VehicleCmd& m);
    Bytes Encode(const PlayerLook& m);
    Bytes Encode(const PeerLook& m);
    Bytes Encode(const VehicleUpdate& m);
    Bytes Encode(const VehicleResult& m);
    Bytes Encode(const Hit& m);
    Bytes Encode(const Damage& m);
    Bytes Encode(const Killed& m);
    Bytes Encode(const Respawn& m);
    Bytes Encode(const RespawnOk& m);
    Bytes Encode(const PlayAnim& m);
    Bytes Encode(const PlayScenario& m);
    Bytes Encode(const StopAnim& m);
    Bytes Encode(const PeerAnim& m);
    Bytes Encode(const PeerScenario& m);
    Bytes Encode(const PeerAnimStop& m);
    Bytes Encode(const SeatNotice& m);   // T1 seats: empty (refused) for a field Decode would refuse
    // T4: refuses a non-finite value or a vehicle id at or above kMaxVehicles; saturates dv, dyaw, closing and depth at the wire's range.
    Bytes Encode(const VehicleImpact& m);
    // R3 T5: [44][u8 count][count x (u16 id, u16 rttMs)]; empty (refused) for more than kMaxLinkEntries entries.
    Bytes Encode(const LinkInfo& m);
    Bytes Encode(const JoinAuth& m);     // v8
    Bytes Encode(const AccountInfo& m);  // v8
    Bytes Encode(const AdminCmd& m);     // v8
    Bytes Encode(const AdminResult& m);  // v8
    Bytes Encode(const InvState& m);     // v8 inventory: {} when the blob is empty or over kMaxInvBlob
    Bytes Encode(const InvOp& m);        // v8 inventory: {} unless op is exactly kInvOpBytes or kInvOpBytesExt
    Bytes Encode(const InvUsed& m);      // v8 inventory: {} for item 0 or satiety over 100
    Bytes Encode(const InvSnap& m);      // v8 0.5.4: {} for an empty / over-cap blob or more than kMaxInvStrip strips
    Bytes Encode(const InvNearby& m);    // v8 0.5.4: {} for more than kMaxInvNearby boxes or a blob over kMaxInvBlob
    Bytes Encode(const InvReport& m);    // v8 0.5.4: {} for more than kMaxInvReportWeapons / kMaxInvReportVehicles

    // The message type of a packet (false for an empty packet or unknown type).
    bool PeekType(const uint8_t* p, size_t n, Msg& out);

    // Decode: true only for a complete, well-formed message of that type; `out` is untouched
    // on failure. Decode(WorldState) is the STATELESS form: it fails on any entry that needs a
    // baseline. Clients use WorldDecoder (worldcodec.h), which keeps the per-player history.
    bool Decode(const uint8_t* p, size_t n, Hello& out);
    bool Decode(const uint8_t* p, size_t n, Welcome& out);
    bool Decode(const uint8_t* p, size_t n, Refuse& out);
    bool Decode(const uint8_t* p, size_t n, PlayerJoined& out);
    bool Decode(const uint8_t* p, size_t n, PlayerLeft& out);
    bool Decode(const uint8_t* p, size_t n, PlayerState& out);
    bool Decode(const uint8_t* p, size_t n, WorldState& out);
    bool Decode(const uint8_t* p, size_t n, ChatSend& out);
    bool Decode(const uint8_t* p, size_t n, ChatMessage& out);
    bool Decode(const uint8_t* p, size_t n, CoarseState& out);
    bool Decode(const uint8_t* p, size_t n, ResourceManifest& out);
    bool Decode(const uint8_t* p, size_t n, ResourceRequest& out);
    bool Decode(const uint8_t* p, size_t n, ResourceChunk& out);
    // v4: refuses non-ascending or contradicting id lists, reserved header / flag bits, a health or
    // fuel above its maximum, a seat id at or above kMaxPlayers, a position outside the quantiser
    // box, an invalid rotation (quant.h), and -128 in an i8 field (the encoder never writes it).
    // v5 adds: a header above 2, reserved flags2 / doors bits, a ctrl or ownerId that is neither
    // kNoPlayer nor below kMaxPlayers; a header 2 entry decodes with VehicleState::motionOnly set.
    bool Decode(const uint8_t* p, size_t n, VehicleWorld& out);
    // v4: refuses an unknown op, and a Register without its tail or any other op with one.
    bool Decode(const uint8_t* p, size_t n, VehicleCmd& out);
    // PlayerLook / PeerLook decode any wire value (a PeerLook id must be below kMaxPlayers);
    // ValidLook is the range policy.
    bool Decode(const uint8_t* p, size_t n, SeatNotice& out);   // T1 seats
    bool Decode(const uint8_t* p, size_t n, PlayerLook& out);
    bool Decode(const uint8_t* p, size_t n, PeerLook& out);
    bool Decode(const uint8_t* p, size_t n, VehicleUpdate& out);
    bool Decode(const uint8_t* p, size_t n, VehicleResult& out);
    bool Decode(const uint8_t* p, size_t n, Hit& out);
    bool Decode(const uint8_t* p, size_t n, Damage& out);
    bool Decode(const uint8_t* p, size_t n, Killed& out);
    bool Decode(const uint8_t* p, size_t n, Respawn& out);
    bool Decode(const uint8_t* p, size_t n, RespawnOk& out);
    bool Decode(const uint8_t* p, size_t n, PlayAnim& out);
    bool Decode(const uint8_t* p, size_t n, PlayScenario& out);
    bool Decode(const uint8_t* p, size_t n, StopAnim& out);
    bool Decode(const uint8_t* p, size_t n, PeerAnim& out);
    bool Decode(const uint8_t* p, size_t n, PeerScenario& out);
    bool Decode(const uint8_t* p, size_t n, PeerAnimStop& out);
    // T4: exactly kVehicleImpactBytes bytes; refuses a vehicle id at or above kMaxVehicles and -128 in an i8 field.
    bool Decode(const uint8_t* p, size_t n, VehicleImpact& out);
    // R3 T5: exactly 2 + 4 * count bytes; refuses a count over kMaxLinkEntries and a repeated id.
    bool Decode(const uint8_t* p, size_t n, LinkInfo& out);
    bool Decode(const uint8_t* p, size_t n, JoinAuth& out);    // v8: a token failing ValidJoinToken is malformed
    bool Decode(const uint8_t* p, size_t n, AccountInfo& out); // v8: unknown flag bits, a username over kMaxName
    bool Decode(const uint8_t* p, size_t n, AdminCmd& out);    // v8: an unknown op, an arg over kMaxAdminArg or with control bytes
    bool Decode(const uint8_t* p, size_t n, AdminResult& out); // v8: an unknown op or code, a text over kMaxRefuseText
    bool Decode(const uint8_t* p, size_t n, InvState& out);    // v8: an empty blob or one over kMaxInvBlob
    bool Decode(const uint8_t* p, size_t n, InvOp& out);       // v8: seq 0, an op that is not kInvOpBytes or kInvOpBytesExt
    bool Decode(const uint8_t* p, size_t n, InvUsed& out);     // v8: item 0, satiety over 100
    bool Decode(const uint8_t* p, size_t n, InvSnap& out);     // v8 0.5.4: empty / over-cap blob, unknown flags, too many strips
    bool Decode(const uint8_t* p, size_t n, InvNearby& out);   // v8 0.5.4: too many boxes, box id 0, unknown flags, over-cap blob
    bool Decode(const uint8_t* p, size_t n, InvReport& out);   // v8 0.5.4: unknown flags, too many weapons or vehicles, hash 0

    // 1..kMaxName characters from [A-Za-z0-9_.-] and inner spaces. A space must
    // sit between two other characters and never doubles ("a  b" and "a b" would
    // look identical to other players).
    bool ValidName(const std::string& name);
    // Finite, inside the world box above (x/y in [kWorldMinXY, kWorldMaxXY], z in
    // [kWorldMinZ, kWorldMaxZ]), heading within [-360, 360], speed per axis at
    // most kMaxSpeed.
    bool ValidKinematics(const Kinematics& k);
    // A PedState a server accepts: move 0..kMoveMax, kPfDriver only with
    // move >= kMoveInVehicle, and a finite aim offset while kPfAiming is set (the aim is ignored,
    // and never sent, without it). jumpSeq and shotSeq are not checked: only their low bits travel.
    // health and armor are not checked: the server never takes them from a client.
    bool ValidPedState(const PedState& s);
    // A look a server accepts: model != 0; every component drawable <= kMaxLookDrawable, texture
    // <= kMaxLookTexture, palette <= kMaxLookPalette; every prop drawable -1..kMaxLookDrawable
    // with texture <= kMaxLookTexture (0 when the drawable is -1); head blend ids <= kMaxHeadBlendId
    // (mixes are any u8).
    bool ValidLook(const Look& l);
    // 1..kMaxChat bytes of well-formed UTF-8 (no overlongs, surrogates or code
    // points above U+10FFFF) with no control characters (C0, DEL, C1).
    bool ValidChat(const std::string& text);
    // v4: a VehicleUpdate a server accepts: position and velocity finite and in the world box
    // (ValidKinematics, heading ignored), a finite rotation of non-zero length, finite angular
    // velocity and steering in [-1, 1], healths <= kMaxVehicleHealth, v5: no reserved flags2 / doors
    // bit. (Speed caps are policy.)
    bool ValidVehicleUpdate(const VehicleUpdate& u);
    // v4: 1..kMaxAnimName characters of [A-Za-z0-9_@/.-].
    bool ValidAnimName(const std::string& name);
    // v4: dict and name ValidAnimName, blends finite within +-1000, durationMs >= -1.
    bool ValidAnim(const Anim& a);

    // A resource file name: 1..kMaxResourceName characters from [A-Za-z0-9._-], no leading or
    // trailing '.', and no "..", so it holds no path separator and is safe as the leaf of a
    // disk-cache path. Touches no filesystem.
    bool ValidResourceName(const std::string& name);
    // A manifest the client will act on: at most kMaxResources entries, each name ValidResourceName
    // and unique (case-insensitive), each size <= kMaxResourceBytes, the total <= kMaxResourceTotal.
    bool ValidManifest(const ResourceManifest& m);

    // ASCII lower-cased copy, for case-insensitive name uniqueness (names are
    // ASCII-only, see ValidName).
    std::string NameKey(const std::string& name);

    // True when `a` is newer than `b` under u32 wrap-around (serial arithmetic).
    inline bool SeqNewer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

    // Names for logs and error messages ("?" for a value outside the enum).
    const char* ToString(Msg m);
    const char* ToString(RefuseReason r);
    const char* ToString(LeaveReason r);
    const char* ToString(VehicleOp op);
    const char* ToString(VehicleResultCode c);
    const char* ToString(SeatNoticeKind k);
    const char* ToString(SeatCancelReason r);
    const char* ToString(AdminOp op);     // v8: "heal", "fix", ... (the command verb)
    const char* ToString(AdminCode c);    // v8

}
