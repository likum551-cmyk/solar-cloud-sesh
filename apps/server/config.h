// Server configuration: the knobs of gage::server::Server and the server.json
// loader behind them. Portable C++17.
//
// Thread-safety: Config is a plain value type (copy it freely; nothing here is shared
// or locked), Validate() and LoadConfigFile() are re-entrant. The one global they read
// is the environment variable GAGE_ADMIN_TOKEN. Errors: nothing throws; the two
// functions return false with a reason in *err. docs/api/server.md is the user-facing
// reference for every key.
#pragma once

#include "protocol/protocol.h"
#include "protocol/query.h"
#include "replconfig.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gage::server {

    // Where a player comes back after dying (RespawnOk): metres and a GTA heading in degrees.
    struct SpawnPoint { float x = 0, y = 0, z = 0, heading = 0; };

    // Every setting of one server. A default-constructed Config is a valid, runnable
    // configuration (the same values server.json.example shows). Fields in the first
    // groups are server.json keys; the last group is for tests and tuning only.
    struct Config
    {
        // ---- server.json keys: the game ----
        std::string name = "GAGE Server"; // shown in Welcome; 1..kMaxServerName bytes
        uint16_t port = proto::kDefaultPort; // UDP; 0 = let the OS pick (tests, see Server::port())
        uint16_t maxPlayers = 32;            // 1..kMaxPlayers
        // WorldState broadcasts per second, 1..120. 30 since sync plan W7 (it was 20): the delay floor of a remote
        // player falls from 107+j ms to 71+j ms, for 1.5x the replication bandwidth (interest_test's 32 players
        // all within 40 m of one client, WorldState payload: 8.5 -> 12.5 kB/s, see rateNearHz / budget in
        // replconfig.h). 60 Hz would gain ~14 ms more
        // for twice the cost: rejected. The engine sends at 1.5x this (45 Hz).
        uint16_t tickHz = 30;
        // The newest client release this server was released with, reported in the v2 UDP query reply ("update
        // ping": a launcher older than this is told to update at once). server.json "latestClientVersion"
        // "x.y.z" (each part 0..255; "" or "0.0.0" = unknown, nothing is announced), else the environment
        // variable GAGE_LATEST_CLIENT_VERSION (tools/deploy/release.sh sets it, docs/OPS.md). Not in
        // server.json.example on purpose: it belongs to a release, not to the operator.
        proto::query::ClientVersion latestClient;

        // ---- server.json keys: world replication (interest management, send rates, budget) ----
        // The knobs of replconfig.h under these server.json names: relevanceEnterRadius,
        // relevanceLeaveRadius, relevanceMaxRadius, relevanceLookaheadMs, relevanceCellSize,
        // relevanceEveryMs, rateNearHz, rateMidHz, rateFarHz, rateIdleHz, rateCoarseHz,
        // nearDistance, midDistance, idleAfterMs, clientBudgetBytesPerSec, worldPacketMaxBytes,
        // coarseSharePercent. repl.maxPlayers and repl.tickHz are ignored here: the server
        // takes them from maxPlayers and tickHz above.
        ReplConfig repl;

        // ---- server.json keys: vehicles (protocol v4; apps/server/vehicles.h) ----
        uint16_t maxVehicles = 64;               // networked vehicles alive at once, 1..256 (kMaxVehicles)
        uint16_t vehiclesPerPlayer = 3;          // spawned (and, separately, adopted) per player, 1..64
        uint32_t vehicleEmptyDespawnMs = 600000; // server.json vehicleEmptyDespawnSeconds 0..86400 (0 = never)
        uint32_t vehicleEnterDistance = 10;      // m, 1..100: Enter / Register range from the player
        uint32_t vehicleMaxSpeed = 150;          // m/s, 10..600: a VehicleUpdate above it is clamped to it up to twice it (T4: a hyper car's overshoot), refused above that
        uint32_t fuelDrainPerMinute = 10;        // permille per minute while driven with the engine on, 0..1000
        // R3 T5: the contact arbiter places a controller's samples at their TRUE time, not the SampleClock's (sample time + the fastest uplink): each
        // sample is moved back by half the controller's round trip, at most this many ms (0..500, 0 = off; server.json contactLagCapMs).
        uint32_t contactLagCapMs = 150;

        // ---- server.json keys: combat (protocol v4; damage scaling v5, apps/server/damage.h) ----
        // playerHealth: the maximum a player has at join, after `heal` and after a respawn, announced in
        // Welcome.playerHealth. 200 (v5; v4: 100) is GTA health 300, about what an Online rank 80..100 player has,
        // so a hit takes a believable share of it. The percents scale the damage a shooter's Hit reports into the
        // damage the server applies (ScaleHit): firearms 72 (Online's freemode factor), melee 50, explosives and
        // run-over 100 (falls, fire and the like are never scaled). headshotPercent is the multiplier of a hit on
        // the head bone with a firearm: full up to 5 m, falling linearly to 100 percent at 40 m (a sniper rifle:
        // 300 m). respawnArmour is the armour a player comes back with.
        uint16_t playerHealth = 200;             // hit points a player (re)spawns with, 1..255
        uint32_t firearmDamagePercent = 72;      // 1..1000
        uint32_t meleeDamagePercent = 50;        // 1..1000
        uint32_t explosiveDamagePercent = 100;   // 1..1000
        uint32_t runOverDamagePercent = 100;     // 1..1000
        uint32_t headshotPercent = 300;          // 100..1000
        uint16_t respawnArmour = 0;              // 0..255
        uint32_t respawnDelayMs = 5000;          // server.json respawnDelaySeconds 0..300
        uint32_t maxHitDistance = 1000;          // m, 10..5000: shooter to target
        uint32_t hitsPerSecond = 30;             // per shooter, 1..1000 (a token bucket, burst the same)
        // server.json spawnPoints: [[x, y, z, heading], ...], 1..16 points inside the world box, used in
        // turn. The default is the engine's own spawn, Legion Square.
        std::vector<SpawnPoint> spawnPoints{SpawnPoint{195.2f, -933.8f, 30.7f, 140.0f}};

        // ---- server.json keys: authority rules against modded clients (protocol v4) ----
        // apps/server/anticheat.h is the code, docs/api/server.md "Anti-cheat rules" the description.
        // hitShotWindowMs: a firearm Hit must match a shot of the shooter's own (a change of its PedState.shotSeq
        // the server received) at most this long ago; 0 turns that requirement off (the damage caps stay).
        uint32_t hitShotWindowMs = 500;          // 0 or 100..5000
        // Scales the per-weapon-class damage caps (their stock values are estimates): 100 = as shipped.
        uint32_t hitDamageScalePercent = 100;    // 10..1000
        // A position farther from the previous one than speed * time + slack opens the "untrusted" window:
        // for untrustedWindowMs the player's Enter / Register / Spawn and its Hits on others are refused.
        // onFootMaxSpeed applies on foot, vehicleMaxSpeed in a vehicle (or ragdolling, falling, parachuting).
        // untrustedWindowMs 0 switches the whole movement check off; untrustedJoinGraceMs is the time after a
        // player's first state in which it is not judged (the engine's own spawn teleport).
        uint32_t onFootMaxSpeed = 15;            // m/s, 5..600
        uint32_t untrustedWindowMs = 3000;       // 0 (off) or 500..60000
        uint32_t untrustedJoinGraceMs = 15000;   // 0..120000
        // At most this many connection attempts per source address per minute (checked before Hello); 0 = no limit.
        uint32_t connectsPerIpPerMinute = 5;     // 0..10000
        // Vehicles: spawned (and, separately, adopted) per source address, orphans included; how long a vehicle
        // whose spawner left stays once it is empty; Register: the pause between two of a player's, and the
        // smallest distance to a networked vehicle (0 = no such rule).
        uint16_t vehiclesPerIp = 6;              // 1..256
        uint32_t vehicleOrphanMs = 60000;        // server.json vehicleOrphanSeconds 1..3600
        uint32_t registerCooldownMs = 2000;      // 0..60000
        uint32_t registerSeparationMeters = 4;   // 0..20
        // Model hashes (server.json: numbers, "0x..." strings, or model names hashed with joaat) a Spawn /
        // Register may not use (deny) or may only use (allow; wins over deny; empty = no restriction), at most 128 each.
        std::vector<uint32_t> vehicleModelDeny;
        std::vector<uint32_t> vehicleModelAllow;

        // ---- server.json keys: master list announce (apps/master/README.md) ----
        // masterUrl: base URL of the master, plain http only (no TLS support), e.g.
        // "http://master.example.org:8080". The server POSTs <masterUrl>/announce every
        // heartbeatSeconds from a background thread. Empty (the default) disables it.
        std::string masterUrl;
        uint32_t heartbeatSeconds = 30;   // 5..3600; the master's TTL is 90 s by default
        // What the launcher shows. The master requires gamemode 1..32 bytes and lang a
        // language tag of at most 8 bytes ("en", "pt-BR", "zh-Hans"); url is optional
        // (empty, or http(s) without credentials, at most 256 characters).
        std::string gamemode = "freeroam";
        std::string lang = "en";
        std::string url;

        // ---- server.json keys: hardening ----
        // Live connections (joined or still saying Hello) allowed per source IPv4
        // address; the next one is refused with TooManyConnections. 0 = no cap.
        uint32_t maxConnectionsPerIp = 4;
        // Chat flood limit, per player: a token bucket of chatBurst messages refilled
        // evenly over chatWindowMs (default 5 messages per 5 s, so a sustained 1/s);
        // beyond it messages are dropped and the sender gets a server notice.
        // server.json takes chatWindowSeconds (1..3600).
        uint32_t chatBurst = 5;           // 1..100
        uint32_t chatWindowMs = 5000;     // 100..3 600 000
        // Ban list file (see banlist.h). Empty = bans live in memory only. Relative
        // paths are relative to the working directory, like server.json itself.
        std::string bansFile = "bans.json";

        // ---- server.json keys: local admin control channel (admin.h) ----
        // adminPort: loopback-only TCP port for the admin line protocol; 0 (default) =
        // disabled. adminToken: shared secret, 16..128 printable ASCII characters
        // without spaces, required whenever the channel is on. There is no in-game
        // admin by player name: the console and this channel are the only admin.
        // When adminPort is set and the file has no adminToken, LoadConfigFile takes
        // it from the environment variable GAGE_ADMIN_TOKEN.
        uint16_t adminPort = 0;
        std::string adminToken;

        // ---- not in server.json ----
        std::string bindHost;           // empty = all interfaces; else an IPv4 literal
        uint32_t helloTimeoutMs = 5000; // a connection must say Hello within this
        // ENet peer timeout (limit, minimum ms, maximum ms): how long a silent
        // client lives before PlayerLeft::Timeout. ENet's own defaults.
        uint32_t peerTimeoutLimit = 32;
        uint32_t peerTimeoutMinMs = 5000;
        uint32_t peerTimeoutMaxMs = 30000;

        // UDP query replies (apps/master/PROTOCOL.md): at most this many per second per
        // source address and in total; the rest are silently dropped.
        uint32_t queryRatePerIp = 20;
        uint32_t queryRateTotal = 2000;

        // Announce timing beyond heartbeatSeconds: 0 announceIntervalMs = use
        // heartbeatSeconds; failures back off from announceBackoffBaseMs, doubling up
        // to announceBackoffMaxMs. Tests shrink these.
        uint32_t announceIntervalMs = 0;
        uint32_t announceBackoffBaseMs = 5000;
        uint32_t announceBackoffMaxMs = 300000;

        // Test knob: with adminPort 0, listen on an ephemeral loopback port instead of
        // staying disabled (read the port back with Server::adminPort()).
        bool adminEphemeral = false;

        // Test knob: drop this % of incoming PlayerState packets, reproducibly
        // from dropSeed. Leave 0 in production.
        uint32_t dropStatePercent = 0;
        uint32_t dropSeed = 1;

        // Log line sink (the line already carries its timestamp, no newline).
        // Empty: stdout. Called on the thread that drives Server::Poll only.
        std::function<void(const std::string&)> log;

        // True when the admin channel will be started: adminPort is set, or the
        // adminEphemeral test knob is on.
        bool adminEnabled() const { return adminPort != 0 || adminEphemeral; }
    };

    // Checks every field against its range and the master list's schema rules (name
    // 1..64 bytes of UTF-8 without control characters; maxPlayers 1..1024; tickHz
    // 1..120; heartbeatSeconds 5..3600; masterUrl a parseable plain-http URL, then
    // gamemode non-empty and name/gamemode free of line-separator and bidi-override
    // characters; gamemode at most 32 bytes; lang a tag of at most 8 bytes; url empty or
    // http(s) without credentials, at most 256 characters; maxConnectionsPerIp 0..1024;
    // chatBurst 1..100; chatWindowMs 100..3600000; query rates 1..1000000; the replication
    // knobs (radii 10..20000 with enter <= leave and enter <= max, rates 1..120 with
    // near >= mid >= far, distances 1..20000 with near <= mid, cell 64..2048 m, budget
    // 1000..100000000 B/s, world packet 200..1300 B, coarse share 1..90 %); announce
    // backoff 0 < base <= max; adminToken 16..128 printable ASCII without spaces when the
    // admin channel is enabled; v4: maxVehicles 1..256, vehiclesPerPlayer 1..64, vehicleEmptyDespawn
    // 0..86400 s, vehicleEnterDistance 1..100, vehicleMaxSpeed 10..600, fuelDrainPerMinute 0..1000,
    // playerHealth 1..255, firearm / melee / explosive / runOver DamagePercent 1..1000, headshotPercent 100..1000,
    // respawnArmour 0..255, respawnDelay 0..300 s, maxHitDistance 10..5000, hitsPerSecond 1..1000,
    // 1..16 spawnPoints inside the world box with a heading within +-360; anti-cheat: hitShotWindowMs 0 or
    // 100..5000, hitDamageScalePercent 10..1000, onFootMaxSpeed 5..600, untrustedWindowMs 0 or 500..60000,
    // untrustedJoinGraceMs 0..120000, connectsPerIpPerMinute 0..10000, vehiclesPerIp 1..256, vehicleOrphanMs
    // 1000..3600000, registerCooldownMs 0..60000, registerSeparationMeters 0..20, at most 128 non-zero model
    // hashes in each of the allow and deny lists). Returns true when `cfg` can run a server. Returns false
    // with *err (when non-null) naming the first bad field and the allowed range.
    // Server::Start() calls this itself.
    bool Validate(const Config& cfg, std::string* err);

    // Findings in a VALID config that the operator should see (the server runs, so these never fail Validate):
    // one line each, in plain words, naming the values and the effect. W8 item 1 (live test 0.4.2): a server.json
    // that pinned rateNearHz 20 under tickHz 30 silently capped what players received at 20/s for the whole
    // test. Today: rateNearHz < tickHz. Server::Start logs each as "WARNING: ...", deploy-server.sh prints the same
    // text from server.json before it activates a release.
    std::vector<std::string> ConfigWarnings(const Config& cfg);

    // The effective settings as log lines (what Server::Start prints): every replication knob, then the combat
    // numbers (playerHealth and every damage percentage), so no value is ever pinned without a trace in the log.
    std::string DescribeRepl(const Config& cfg);
    std::string DescribeCombat(const Config& cfg);
    // "latest client advertised in the UDP query: x.y.z" or "... none (unknown)": one log line at start.
    std::string DescribeLatestClient(const Config& cfg);

    // Reads server.json into `cfg` (keys it lacks keep their value; the file must be a
    // single JSON object of at most 1024 values). Unknown keys, wrong types, non-integer
    // or out-of-range numbers, trailing data and a failing Validate() are all errors, so
    // a typo cannot silently leave a default in place. When the admin channel is on
    // (adminPort != 0) and the file sets no adminToken, the token is taken from the
    // environment variable GAGE_ADMIN_TOKEN. Likewise latestClientVersion falls back to the environment variable
    // GAGE_LATEST_CLIENT_VERSION when the file sets none; a malformed value in either place is an error. Returns true and replaces `cfg` only when
    // everything is valid; otherwise returns false, leaves `cfg` untouched and puts
    // "<path>: <reason>" in *err. *missing (when non-null) is true ONLY when the file does
    // not exist, so callers can choose to fall back to defaults; a file that exists but
    // cannot be read (a directory, no permission) is an error with *missing false.
    bool LoadConfigFile(const std::string& path, Config& cfg, std::string* err, bool* missing = nullptr);

}
