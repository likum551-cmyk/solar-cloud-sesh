// gage::server::Server: the GAGE multiplayer server core. Portable C++17 (ENet
// is the only platform layer for the game traffic) so the same code runs on the
// Windows dev box and the Linux VPS, and so tests can drive it in-process.
//
// Single-threaded and caller-driven: Start(), then call Poll() in a loop (it
// services the network, runs timers and broadcasts WorldState when a tick is
// due), then Stop(). The one background thread is the master announcer (announce.h),
// which never touches server state: it is fed a player count and its log lines are
// drained and logged from Poll().
//
// Policy, in one place:
//  * A connection must send a valid Hello within Config::helloTimeoutMs, else it
//    is refused (HelloTimeout). Anything but Hello first, or a malformed message,
//    is refused (Protocol) and cut off.
//  * On connect, before Hello: an address on the ban list is refused (Banned), then
//    an address that already holds Config::maxConnectionsPerIp live connections is
//    refused (TooManyConnections).
//  * Hello is checked in this order: protocol version, name charset, ban list (by
//    name, and again by address), server full, name taken (case-insensitive). Each
//    failure is a Refuse with a reason.
//  * On accept the newcomer gets Welcome, then PlayerJoined for every player
//    already in; everyone else gets PlayerJoined for the newcomer.
//  * PlayerState: older-or-equal sequence numbers are dropped (stale), out-of-box
//    or malformed ones are dropped, each with a logged reason (rate-limited per
//    player). A bad state packet never disconnects: it is unreliable data.
//  * World replication (replication.h, docs/api/server.md "Interest management"): every tick
//    the server decides, per receiver, which other players it is told about. A player is
//    relevant to a receiver inside the enter radius (400 m, a little more when they move
//    fast) until they pass the leave radius (450 m); relevant players are sent as
//    WorldState on the unreliable state channel at 20/10/5 Hz by distance (idle ones
//    1 Hz) as deltas against what the receiver acknowledged, within a per-client byte
//    budget; everyone else reaches the receiver as ~1 Hz WorldCoarse map blips.
//    A receiver never gets its own entry (it knows its own state) and nobody who has
//    not sent any state yet (no phantom at the origin). SetAlwaysRelevant() overrides it.
//  * ChatSend: Decode enforces the byte cap (an over-cap message is malformed, so
//    the sender is cut off). Beyond the per-player token bucket (chatBurst per
//    chatWindowMs) a message is dropped with a rate-limited server notice to the
//    sender. ValidChat failures (empty, control characters, bad UTF-8) are dropped
//    with a server notice to the sender only. A valid message goes to every joined
//    player, the sender included, so all clients show the same ordered log.
//  * PlayerState (v3) also carries the on-foot state (PedState): Decode refuses an unknown move,
//    reserved flags and aim bytes without the aiming flag (malformed); ValidPedState refuses a
//    driver flag on foot (invalid). Both are dropped and logged like a bad position.
//  * PlayerLook (v3, reliable): a malformed one is refused (Protocol); one failing ValidLook is
//    dropped with a log line; one equal to the newest held look is ignored. Accepted looks are
//    stored and broadcast as PeerLook to everyone else, at most 2 at once and 2 a second per player:
//    over that, the newest waits and is sent when allowed (older waiting ones are dropped). A
//    newcomer gets PeerLook for every stored look right after its PlayerJoined burst.
//  * Leaving: PlayerLeft::Quit when the client said goodbye (kDisconnectQuit),
//    Timeout when it vanished, Kicked when the server cut it off (kick, ban, rule
//    violation).
//  * UDP query (apps/master/PROTOCOL.md): a 24-byte datagram "GAGE 01 01 ..." on the game
//    port is answered with protocol version, players and maxPlayers, rate-limited per
//    source address and overall; ENet never sees it. A request that asks for query format v2 (u16 at 6
//    = 2) also gets Config::latestClient in the same 24 bytes; any other request gets the v1 reply.
//  * Admin is the console (Execute) and, when Config::adminPort is set, the local
//    loopback channel (admin.h). No player is ever an admin.
//  * v4 vehicles (vehicles.h): VehicleCmd (reliable) is checked against the server's own state
//    (caps, occupancy, distance, lock, who may) and ALWAYS answered with a VehicleResult; a malformed
//    one is a protocol violation. VehicleUpdate (state channel) is taken only from the driver the
//    server has in seat 0, newer than the last one, in the box and under vehicleMaxSpeed; anything
//    else is dropped and logged, never fatal. Each tick every player gets its own VehicleWorld.
//  * v4 combat: health and armour are the server's. Hit (reliable) is validated (shooter and target
//    alive and known, target relevant to the shooter and within maxHitDistance, hitsPerSecond, damage
//    clamped to 250) and applied armour first; Damage goes to the victim and the attacker, Killed to
//    everybody. A refused Hit is dropped and logged. Respawn is honoured respawnDelayMs after the death
//    (RespawnOk at the next spawn point); earlier, or while alive, it is ignored and counted.
//    A WorldEntry shows the server's health / armour; its kPfDead is the server's death OR the
//    player's own client saying its ped is dead (display only: hits and respawns use the server's).
//  * v4 emotes: PlayAnim / PlayScenario / StopAnim (reliable; malformed = violation) at most 4 a
//    second per player; a bad name or blend is dropped and logged; from a dead player they are dropped; the
//    animation's flags are reduced to the allow-list and its playback rate clamped. Relayed to everybody
//    else; the current looping animation or scenario is replayed to newcomers after the looks, and cleared by
//    StopAnim, a one-shot animation, or a respawn.
//  * v4 authority rules against modded clients (anticheat.h; docs/api/server.md "Anti-cheat rules"): a Hit
//    must be tied to the shooter's own shot (firearms), to reach (melee) or to a moving vehicle it drives
//    (run-over), is clamped by a per-weapon-class damage table and budget; a position farther from the
//    previous one than speed x time allows opens a short "untrusted" window (no Enter / Register / Spawn, no
//    Hits on others); connections per address per minute, vehicles per address, orphaned vehicles, Register
//    spacing and model allow / deny lists are limited; VehicleResult RateLimited replies are throttled.
#pragma once

#include "announce.h"
#include "banlist.h"
#include "config.h"
#include "metrics.h"
#include "protocol/protocol.h"
#include "replication.h"
#include "sync/vehicle_impact.h"
#include "tickclock.h"
#include "vehicles.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct _ENetHost;
struct _ENetPeer;
struct _ENetEvent;

namespace gage::net {
    class EnetScope;
}

namespace gage::server {

    class AdminChannel;

    // Why a Hit was refused (Stats::hitRefusals, the `combat:` log line). One per refusal, whichever path judged it.
    enum class HitReason : uint8_t
    {
        None,
        Rate,            // over hitsPerSecond
        ShooterDead,
        ShooterNoState,
        ZeroDamage,
        NoTarget,        // no such player / vehicle
        TargetNoState,
        TargetDead,
        Untrusted,       // the shooter is inside the untrusted window
        Range,           // beyond maxHitDistance
        NotRelevant,     // the target is not in the shooter's view
        Environment,     // an environmental weapon aimed at somebody else
        NoShot,          // no shot (swing) of the shooter's with that shotSeq in the window
        WeaponNotHeld,   // the claimed weapon was not in the shooter's states lately
        ShotUsed,        // the shot already carried its hitsPerShot Hits
        MeleeReach,      // a melee Hit beyond reach
        RunOver,         // a run-over rule failed
        NoExplosive,     // no launch of an explosive weapon
        OverBudget,      // the class's damage budget is spent
        DeferFull,       // too many Hits waiting for their shot
        Count
    };
    constexpr size_t kHitReasons = (size_t)HitReason::Count;
    const char* HitReasonName(HitReason r); // a short log key, e.g. "melee_reach"

    // Counters for the admin console and the tests.
    struct Stats
    {
        uint64_t ticks = 0;            // WorldState broadcasts
        uint64_t joins = 0;
        uint64_t leaves = 0;
        uint64_t refused = 0;          // Hello refusals and protocol cut-offs
        uint64_t refusedBanned = 0;    // of which: ban list
        uint64_t refusedIpCap = 0;     // of which: per-address connection cap
        uint64_t statesAccepted = 0;
        uint64_t statesStale = 0;      // seq not newer than the last accepted
        uint64_t statesInvalid = 0;    // decoded, but outside the world box / limits
        uint64_t statesMalformed = 0;  // would not decode (truncated, non-finite...)
        uint64_t statesSimDropped = 0; // dropped by the loss-simulation knob
        uint64_t chatDelivered = 0;    // ChatMessage broadcasts (one per message)
        uint64_t chatRejected = 0;     // ValidChat failures
        uint64_t chatFlooded = 0;      // dropped by the per-player flood limit
        uint64_t looksBroadcast = 0;   // PeerLook broadcasts (one per accepted look change)
        uint64_t looksReplayed = 0;    // stored looks sent to a newcomer after its PlayerJoined burst
        uint64_t looksInvalid = 0;     // PlayerLook that failed ValidLook (dropped, not fatal)
        uint64_t looksUnchanged = 0;   // PlayerLook equal to the newest held look (nothing to send)
        uint64_t looksDeferred = 0;    // PlayerLook over the rate limit, held back for later
        uint64_t looksCoalesced = 0;   // held-back looks replaced by a newer one (never sent)
        uint64_t queryAnswered = 0;    // UDP queries replied to
        uint64_t queryRateLimited = 0; // UDP queries dropped by the reply rate limit
        // ---- v4 ----
        uint64_t vehicleCmds = 0;            // VehicleCmds answered (any result)
        uint64_t vehicleCmdsRefused = 0;     // of which not Ok
        uint64_t vehiclesCreated = 0;        // Spawn + Register
        uint64_t vehiclesDestroyed = 0;      // Despawn, empty timer, adopt cap
        uint64_t vehicleUpdatesAccepted = 0;
        uint64_t vehicleUpdatesRejected = 0; // not the server's driver of that vehicle, dead, no such vehicle
        uint64_t vehicleUpdatesInvalid = 0;  // malformed, out of the box, over vehicleMaxSpeed
        uint64_t vehicleUpdatesStale = 0;    // seq not newer than the last accepted
        uint64_t vehicleFrames = 0;          // VehicleWorld packets sent
        uint64_t vehiclePartHits = 0;        // v6: vehicle Hits that set a tyre / window bit (VehicleStore::HitPart)
        uint64_t vehicleImpacts = 0;         // T4: contacts between two controllers' vehicles the arbiter resolved
        uint64_t vehicleImpactsSent = 0;     // ... VehicleImpact messages sent (one per controller)
        uint64_t hitsAccepted = 0;
        uint64_t hitsRejected = 0;           // failed validation (dead, unknown, range, relevance, rate, 0 damage)
        uint64_t kills = 0;
        uint64_t respawns = 0;
        uint64_t respawnsRefused = 0;        // too early, or not dead
        uint64_t animsRelayed = 0;           // PeerAnim / PeerScenario / PeerAnimStop broadcasts
        uint64_t animsReplayed = 0;          // stored loops / scenarios sent to newcomers
        uint64_t animsInvalid = 0;           // failed ValidAnim / ValidAnimName (dropped)
        uint64_t animsRateLimited = 0;       // over 4 a second (dropped)
        // ---- v4 authority rules (anticheat.h, docs/api/server.md "Anti-cheat rules") ----
        uint64_t refusedConnectRate = 0;     // connection attempts over connectsPerIpPerMinute (also in `refused`)
        uint64_t hitsDeferred = 0;           // Hits held back (up to 300 ms) for the shot or weapon state they name
        uint64_t hitsUnbound = 0;            // refused: no matching shot / weapon not held / shot used up / out of melee reach / run-over rules
        uint64_t hitsClamped = 0;            // accepted with less damage than claimed (class cap or damage-rate budget)
        uint64_t hitsOverBudget = 0;         // refused: the weapon class's damage budget per second is spent
        uint64_t hitsUntrusted = 0;          // refused: the shooter is inside the untrusted window
        // ---- 0.4.5 melee audit: why a Hit died (every refusal counts in exactly one slot) and what the accepted ones were worth ----
        uint64_t hitRefusals[kHitReasons] = {};  // by HitReason
        uint64_t hitsMelee = 0;              // accepted as the melee class (fists, blades, a gun used as a club)
        uint64_t hitsMeleeClipped = 0;       // ... of which less than claimed: the class cap, the weapon ceiling or the budget
        uint64_t hitsMeleeLate = 0;          // ... of which named a swing older than hitShotWindowMs (the melee window, kMeleeShotWindowMs)
        uint64_t hitsFloored = 0;            // accepted and scaled below 1 point, applied as the 1-point floor
        uint64_t hitsDamageApplied = 0;      // sum of the damage taken off players by accepted Hits (armour and health)
        uint64_t movesRejected = 0;          // positions beyond the displacement budget (each opens the untrusted window)
        uint64_t untrustedRefusals = 0;      // Enter / Register / Spawn refused inside the window
        uint64_t vehicleRepliesSuppressed = 0; // RateLimited VehicleResults not sent (at most 1 a second per player)
        uint64_t vehicleFloodKicks = 0;      // players cut off for a sustained VehicleCmd flood
        uint64_t seatIntents = 0, seatIntentsRefused = 0, seatSeated = 0, seatCancelled = 0, seatExpired = 0, seatExits = 0, seatNoticesSent = 0; // T1 seats (wire v6)
        uint64_t registersRefused = 0;       // Register: too soon after the last one, or too close to a networked vehicle
        uint64_t modelsDenied = 0;           // Spawn / Register of a model outside the allow / deny lists
        uint64_t vehiclesOrphaned = 0;       // occupied vehicles whose spawner left
        uint64_t animsDeadDropped = 0;       // PlayAnim / PlayScenario from a dead player
        uint64_t animsSanitized = 0;         // PlayAnim relayed with flags stripped or the rate clamped
    };

    // One joined player, as Players() reports it.
    struct PlayerInfo
    {
        uint16_t id = 0;
        std::string name;
        std::string address; // dotted IPv4
        bool hasState = false; // false until the player has sent a PlayerState
        proto::Kinematics k;   // the last accepted state (meaningful only when hasState)
        proto::PedState ped;   // its on-foot state (v3), same sample, as the others see it (v4: server health / armour)
        uint32_t rttMs = 0;    // ENet's smoothed round-trip estimate
        uint8_t health = 0;    // v4: the server's values
        uint8_t armor = 0;
        bool dead = false;
        uint16_t vehicle = VehicleStore::kNone; // the vehicle it sits in (VehicleStore::kNone = on foot)
        uint8_t seat = 0;
        bool untrusted = false;  // inside the untrusted window after a position jump (Config::untrustedWindowMs)
    };

    // What Execute() made of a console line.
    //   Ok     the command ran
    //   Error  the command was refused or malformed; `output` says why
    //   Quit   `quit` (or `exit`/`stop`): Server::quitRequested() is now true; the owner
    //          of the Poll loop decides to stop
    enum class CommandResult { Ok, Error, Quit };

    // The server. Own it on the stack or heap, Start() it, then drive it from ONE thread.
    //
    // Thread-safety: not thread-safe. Every method (including the accessors, Execute()
    // and the admin calls) must be called from the thread that calls Poll(); the class
    // takes no locks. The only other thread is the master announcer, which is internal
    // and never touches server state. Lines go to Config::log on the Poll thread only.
    // Errors: nothing throws (the build uses -fno-exceptions); failures are bool returns
    // with a reason string, and a malformed or hostile packet is dropped or answered with
    // a Refuse, never an error to the caller.
    class Server
    {
    public:
        Server();
        ~Server(); // Stop()s
        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        // Validates `cfg` and starts serving: loads the ban list, binds the UDP port
        // (and, when the admin channel is on, its loopback TCP port), installs the query
        // responder and starts the announcer when masterUrl is set. `cfg` is copied.
        // Returns true when running. Returns false with *err (when non-null) for an
        // invalid config (see Validate()), a server that is already running, a bindHost
        // that is not an IPv4 address, a game or admin port that cannot be bound, or a
        // ban list file that exists but cannot be read or parsed (the file is not touched);
        // nothing stays open after a failure.
        bool Start(const Config& cfg, std::string* err = nullptr);
        // Services the network for at most `timeoutMs` (it returns early on
        // traffic), runs timers (hello timeouts, the closing sweep), the announcer's log
        // drain, the admin channel and the WorldState broadcast when a tick is due. Call
        // it in a loop; with timeoutMs 0 it never waits. Does nothing when not running.
        void Poll(uint32_t timeoutMs);
        // Tells every client the server is going down (a chat notice, then a
        // kDisconnectShutdown), flushes (bounded to about half a second), stops the
        // announcer and the admin channel and closes the sockets. Safe to call twice and
        // when never started.
        void Stop();

        // True between a successful Start() and Stop().
        bool running() const { return host_ != nullptr; }
        // The bound UDP port (the real one when Config::port was 0); 0 before Start().
        uint16_t port() const { return port_; }
        // The admin channel's bound loopback TCP port, 0 when it is off.
        uint16_t adminPort() const;
        // The configuration the server was started with.
        const Config& config() const { return cfg_; }
        // Live counters; the reference stays valid for the object's lifetime.
        const Stats& stats() const { return stats_; }
        // Performance counters (tick time, bytes, packets, CPU...): cumulative since
        // Start(); a window is the difference of two copies (metrics.h). Read on the Poll
        // thread like everything else. The `stats` console command reports them.
        const Metrics& metrics() const { return metrics_; }
        // Clears the peak (max) values of the metrics so a measurement window starts clean.
        void ResetMetricsPeaks() { metrics_.ResetPeaks(); }
        // The joined players, ordered by id (connections still saying Hello are not
        // listed).
        std::vector<PlayerInfo> Players() const;
        // The ban list (read-only; mutate it through Ban/BanIp/Unban). Pointers taken from
        // it are invalidated by those calls.
        const BanList& bans() const { return bans_; }
        // Counters of the master announcer (all zero when masterUrl is empty).
        Announcer::Counters announceCounters() const { return announcer_.counters(); }
        // True once a `quit` came in through any admin path (console or channel).
        bool quitRequested() const { return quitRequested_; }

        // ---- relevance (gamemode API) ----
        // Makes player `subject` always relevant to `receiver`: it is replicated to that
        // receiver whatever the distance (at the far rate, never dropped by the leave radius).
        // `receiver` may be proto::kServerId, meaning every player, including those who join
        // later. `on == false` returns the pair to the normal radii (the receiver may then get
        // a leave notice and the coarse channel takes over); with `receiver == kServerId` it
        // clears the override of EVERY receiver, also ones set individually. Both ids must be joined players
        // (a subject that leaves takes its overrides with it); subject == receiver is refused.
        // Returns true when applied; false with *err (when non-null) otherwise. Costs a few
        // bytes of server state per pair, and the always-relevant players' updates count
        // against each receiver's byte budget like any other.
        bool SetAlwaysRelevant(uint16_t subject, uint16_t receiver, bool on, std::string* err = nullptr);
        // True when the override is on for that pair (receiver may be kServerId: "for all").
        bool AlwaysRelevant(uint16_t subject, uint16_t receiver) const;
        // True when `subject` is in `receiver`'s relevance set right now (replicated at full
        // detail, not just as a coarse blip). False for ids that are not joined players.
        bool IsRelevant(uint16_t receiver, uint16_t subject) const;
        // How many players `receiver` is being sent at full detail right now.
        size_t RelevantCount(uint16_t receiver) const;

        // ---- vehicles and combat (v4) ----
        // Every networked vehicle, ascending by id (the server's own state).
        std::vector<proto::VehicleState> Vehicles() const;
        // Vehicle `id` (spawner, adopted, ...), or null. The pointer is valid until the next Poll().
        const VehicleStore::Vehicle* FindVehicle(uint16_t id) const { return vehicles_.Get(id); }
        // T4: VehicleUpdates whose velocity was over vehicleMaxSpeed (but under twice it) and was clamped, and driverless vehicles a driver took the control of.
        uint64_t VehicleSpeedClamps() const { return vehicles_.SpeedClamps(); }
        uint64_t VehicleClaims() const { return vehicles_.Claims(); }
        // Console `heal`: full health (Config::playerHealth) for a live player; `kill`: health and
        // armour 0, dead, Killed (killer kServerId); `armor`: sets the armour. Each tells the player
        // with a Damage (attacker kServerId). False with *err for an unknown target or a dead one
        // (heal, armor, kill).
        bool Heal(const std::string& target, std::string* err = nullptr);
        bool KillPlayer(const std::string& target, std::string* err = nullptr);
        bool SetArmor(const std::string& target, uint8_t armor, std::string* err = nullptr);

        // ---- admin ----
        // These are the programmatic form of the console commands; Execute() calls them.
        //
        // Cuts a player off (PlayerLeft::Kicked to the others, Disconnected(Kicked) to
        // the player). `target` is a numeric id or a name (case-insensitive). Returns
        // true when someone was kicked; false with *err ("no player \"<target>\"")
        // otherwise.
        bool Kick(const std::string& target, std::string* err = nullptr);
        // Server notice to everyone (ChatMessage from kServerId), bypassing the flood
        // limit. Returns false, sending nothing, if `text` fails ValidChat (empty, over
        // 200 bytes, control characters, invalid UTF-8).
        bool Say(const std::string& text);
        // Bans an online player (numeric id or name: the name AND the address it
        // connected from) or, for a name nobody is using, just that name; the player is
        // refused and cut off at once, with `reason` as the Refuse text. So is anyone else
        // connected from the same address. The ban is saved to bans.json at once. Returns
        // true when added; *msg (when non-null) gets a human-readable result ("banned
        // name Alice and address 203.0.113.5"), with a WARNING appended when bans.json
        // could not be written (the ban still holds in memory). Returns false with *msg
        // set to the reason for: an empty target, a numeric id nobody holds, something
        // that is not a valid player name, an invalid reason, a full list, or a target
        // that is already banned.
        bool Ban(const std::string& target, const std::string& reason, std::string* msg);
        // Bans an IPv4 address (and cuts off everyone connected from it). Same result
        // reporting as Ban(); fails for an address that is not a strict dotted IPv4, an
        // invalid reason, a full list or an address that is already banned.
        bool BanIp(const std::string& ip, const std::string& reason, std::string* msg);
        // Lifts every ban entry matching a name (case-insensitive) or an address, saved at
        // once. Returns true when at least one entry was removed (*msg: "unbanned <target>
        // (N entries)"); false with *msg set when nothing matched or `target` is empty.
        bool Unban(const std::string& target, std::string* msg);
        // Runs one console line (the same grammar the stdin console and the admin channel
        // use): list | kick <id|name> | say <text> | ban <id|name> [reason] | banip <ip>
        // [reason] | unban <name|ip> | bans | heal <id|name> | kill <id|name> | armor <id|name> <0..255>
        // | vehicles | help | quit. The command word is
        // case-insensitive; an empty line is Ok with empty output. The human-readable
        // result (or the reason for an Error) goes to `output`. See docs/api/server.md for
        // every command's exact output.
        CommandResult Execute(const std::string& line, std::string& output);

    private:
        struct Conn;
        struct Impl;

        // ENet's raw-datagram hook (host->intercept): answers the UDP query. A plain
        // function pointer carries no context, so Poll() publishes `this` for the
        // duration of the service call (thread-local) and this finds the server again.
        static int QueryIntercept(_ENetHost* host, _ENetEvent* event);
        int AnswerQuery(_ENetHost* host);

        void Log(const std::string& msg);
        void LogDrop(Conn& c, int kind, const std::string& msg);
        void Tick(uint64_t nowUs);
        void BroadcastWorld();
        void SampleMetrics(uint64_t nowUs);
        void HandleConnect(_ENetPeer* peer);
        void HandleDisconnect(_ENetPeer* peer, uint32_t data);
        void HandleReceive(Conn& c, uint8_t channel, const uint8_t* data, size_t len);
        void OnHello(Conn& c, const uint8_t* data, size_t len);
        void OnState(Conn& c, const uint8_t* data, size_t len);
        void OnChat(Conn& c, const uint8_t* data, size_t len);
        void OnLook(Conn& c, const uint8_t* data, size_t len);
        void PublishLook(Conn& c, const proto::Look& look);
        void FlushPendingLooks();
        // ---- v4 ----
        void OnVehicleUpdate(Conn& c, const uint8_t* data, size_t len);
        void OnVehicleCmd(Conn& c, const uint8_t* data, size_t len);
        proto::VehicleResultCode RunVehicleCmd(Conn& c, const proto::VehicleCmd& m, uint16_t& vehId);
        void FlushSeatNotices();   // T1 seats: delivers VehicleStore's queued SeatNotices
        void OnHit(Conn& c, const uint8_t* data, size_t len);
        // The authority rules of anticheat.h. ApplyHit judges a Hit and, when it passes, applies it: Accept
        // (applied), Defer (the shot or weapon state it names may still be on its way: kept up to 300 ms) or
        // Reject (`why` says). `final` turns a Defer into a Reject.
        enum class HitVerdict { Accept, Defer, Reject };
        HitReason hitReason_ = HitReason::None;   // set with `why` when ApplyHit (or OnHit) refuses: counted by whoever reports the refusal
        Stats hitLogMark_;                        // stats_ as of the previous 10 s `combat:` line
        uint64_t nextHitLogUs_ = 0;
        void LogHitSummary(uint64_t nowUs);
        HitVerdict ApplyHit(Conn& c, const proto::Hit& h, uint64_t nowMs, bool final, std::string& why);
        void RetryDeferredHits(Conn& c, uint64_t nowMs);
        void CheckMove(Conn& c, const proto::PlayerState& s, uint64_t nowMs);
        void MarkUntrusted(Conn& c, uint64_t nowMs, const std::string& why);
        bool Untrusted(const Conn& c, uint64_t nowMs) const;
        bool ModelAllowed(uint32_t model) const;
        void OnRespawn(Conn& c, const uint8_t* data, size_t len);
        void OnAnim(Conn& c, proto::Msg type, const uint8_t* data, size_t len);
        void ClearAnim(Conn& c, bool tellOthers);
        // Applies `damage` (already clamped and scaled, apps/server/damage.h) to `victim` from `attacker`, tells the
        // victim, the attacker and (v5) everyone for whom either is relevant, broadcasts a death. `bone` is the Hit's
        // bone (-1 unknown), `flags` the proto::kDmg* bits the hit earned (the armour bit is added here).
        uint16_t HurtPlayer(Conn& victim, uint16_t attacker, uint32_t weapon, uint16_t damage, int16_t bone = -1, uint8_t flags = 0);
        void SendVitals(Conn& victim, uint16_t attacker, uint32_t weapon, uint16_t damage, bool killed, int16_t bone = -1,
                        uint8_t flags = 0);
        // Re-publishes the player's state to the replicator with the server's health / armour / death.
        void PushShown(Conn& c);
        proto::PedState Shown(const Conn& c) const;
        void BroadcastVehicles(uint64_t nowMs);
        void BroadcastImpacts(uint64_t nowMs);   // T4
        void VehicleDestroyed(uint16_t id);
        void Accept(Conn& c, const std::string& name);
        void Refuse(Conn& c, proto::RefuseReason reason, const std::string& text);
        void RefuseBanned(Conn& c, const BanEntry& ban);
        // Refuses (Banned) every live connection from `ip`, joined players included.
        void RefuseAddress(const std::string& ip, const BanEntry& ban);
        void RemovePlayer(Conn& c, proto::LeaveReason why);
        void BeginClose(Conn& c, uint32_t disconnectData);
        void SendTo(Conn& c, uint8_t channel, const proto::Bytes& bytes, bool reliable);
        void SendToAll(const proto::Bytes& bytes, const Conn* except);
        void Notice(Conn& c, const std::string& text);
        uint16_t AllocId();
        Conn* FindPlayer(const std::string& target);
        size_t LiveConnectionsFrom(const std::string& ip, const Conn* except) const;

        std::unique_ptr<net::EnetScope> enet_;
        std::unique_ptr<Impl> impl_;
        std::unique_ptr<AdminChannel> admin_;
        Announcer announcer_;
        BanList bans_;
        _ENetHost* host_ = nullptr;
        Config cfg_;
        Stats stats_;
        uint16_t port_ = 0;
        uint32_t tick_ = 0;
        uint16_t nextId_ = 0;
        TickClock tickClock_;         // the schedule: tick numbers follow wall time
        uint64_t tickNominalUs_ = 0;  // the running tick's own slot time: what sample ages are measured against
        bool quitRequested_ = false;
        Replicator repl_;
        VehicleStore vehicles_;
        std::vector<VehicleStore::ControlCandidate> controlCandidates_; // scratch of BroadcastVehicles (one allocation, reused)
        sync::impact::ContactArbiter contact_;                           // T4: one resolution per contact between two controllers' vehicles
        std::vector<sync::impact::ContactArbiter::Live> contactLive_;    // scratch
        std::vector<sync::impact::ImpactOut> contactOut_;                // scratch
        uint32_t impactSeq_ = 0;
        uint64_t impactLogMs_ = 0, impactLogCount_ = 0, impactLogClaims_ = 0;
        uint32_t nextSpawnPoint_ = 0;
        Metrics metrics_;
        Metrics statsMark_;           // metrics_ as of the previous `stats` report
        Metrics logMark_;             // metrics_ as of the previous 10 s sync log line
        uint64_t nextSyncLogUs_ = 0;  // when that line is next due
        uint64_t statsMarkUs_ = 0;
        uint64_t startUs_ = 0;        // Start() time on the NowUs() clock
        uint64_t nextSecondUs_ = 0;   // next once-a-second sampling (per-client bytes)
        double cpuBase_ = 0;          // ThreadCpuSeconds() at Start()
        uint32_t lastWireSentData_ = 0, lastWireSentPackets_ = 0, lastWireRecvData_ = 0, lastWireRecvPackets_ = 0;
        std::unordered_map<_ENetPeer*, std::unique_ptr<Conn>> conns_;
        std::map<uint16_t, Conn*> players_;                // joined players by id
        std::unordered_map<std::string, uint16_t> nameKeys_; // NameKey -> id
    };

}
