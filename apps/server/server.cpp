#include "server.h"

#include "admin.h"
#include "damage.h"
#include "netcommon/netcommon.h"
#include "protocol/query.h"
#include "ratelimit.h"
#include "sync/sampleclock.h"
#include "util.h"

#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gage::server {

    namespace {

        // Spare ENet slots beyond maxPlayers for connections that have not said
        // Hello yet, so ServerFull can be answered instead of the connection being
        // silently ignored by ENet.
        constexpr size_t kPendingSlots = 32;
        // The UDP query shares the game socket, so ENet must never mistake it for its
        // own traffic: the query magic read as an ENet header is peer id 1857, which
        // is only ignored (never a peer) while the host has at most 1856 peers
        // (apps/master/PROTOCOL.md, "Why ENet cannot mistake this for its own traffic").
        // The host is created with maxPlayers + kPendingSlots peers.
        static_assert(proto::kMaxPlayers <= 0x740, "kMaxPlayers must stay <= 0x740 for the UDP query");
        static_assert(proto::kMaxPlayers + kPendingSlots <= 0x740,
                      "ENet peerCount (maxPlayers + kPendingSlots) must stay <= 0x740 for the UDP query");
        // A connection we asked to close that never completes is reset after this.
        constexpr uint64_t kCloseGraceMs = 3000;
        // One log line per drop kind per player per this long (the rest is counted); the untrusted-window
        // line is rarer (at most one per 10 s per player).
        constexpr uint64_t kDropLogEveryMs = 2000;
        constexpr uint64_t kDropLogEveryUntrustedMs = 10000;
        // At most one "you are flooding" notice per player per this long.
        constexpr uint64_t kFloodNoticeEveryMs = 1000;

        enum DropKind
        {
            kDropStale = 0,
            kDropInvalid = 1,
            kDropMalformed = 2,
            kDropChat = 3,
            kDropFlood = 4,
            kDropLook = 5,
            kDropVehicle = 6,
            kDropHit = 7,
            kDropAnim = 8,
            kDropUntrusted = 9,
            kDropKinds = 10
        };

        // v4 per-player rate limits (token buckets: burst, per second). VehicleCmd of any kind; Spawn and
        // Register on top of that; PlayAnim / PlayScenario / StopAnim together.
        constexpr double kVehCmdBurst = 10.0, kVehCmdPerSec = 10.0;
        constexpr double kSpawnBurst = 3.0, kSpawnPerSec = 1.0;
        constexpr double kAnimBurst = 4.0, kAnimPerSec = 4.0;
        // A VehicleCmd over its rate limit is answered RateLimited at most this often per player ...
        constexpr uint64_t kRateLimitedReplyEveryMs = 1000;
        // ... and every one over the limit also draws on this bucket (burst, per second): a client that keeps
        // sending commands at more than kVehFloodPerSec over its limit for a while is cut off (Protocol).
        constexpr double kVehFloodBurst = 200.0, kVehFloodPerSec = 20.0;

        // PlayerLook rate limit per player: a burst of 2, then 2 a second. Over it the newest look
        // waits (replacing an older waiting one) and is broadcast once a token is back.
        constexpr double kLookBurst = 2.0;
        constexpr double kLookPerSec = 2.0;

        uint64_t NowUs()
        {
            using namespace std::chrono;
            return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
        }

        std::string AddressOf(const ENetPeer* peer)
        {
            char buf[64];
            if (enet_address_get_host_ip(&peer->address, buf, sizeof(buf)) != 0)
                return "?";
            return buf;
        }

        bool AllDigits(const std::string& s)
        {
            if (s.empty() || s.size() > 5) return false;
            for (char c : s)
                if (c < '0' || c > '9') return false;
            return true;
        }

        // The server whose Poll()/service call is running on this thread (see QueryIntercept).
        thread_local Server* t_servicing = nullptr;

        struct Servicing
        {
            Server* previous;
            explicit Servicing(Server* s) : previous(t_servicing) { t_servicing = s; }
            ~Servicing() { t_servicing = previous; }
        };

    }

    struct Server::Conn
    {
        enum class St { AwaitHello, Joined, Closing };

        _ENetPeer* peer = nullptr;
        St st = St::AwaitHello;
        uint64_t connectedAtMs = 0;
        uint64_t closingAtMs = 0;
        std::string addr;

        // valid once Joined
        uint16_t id = 0;
        std::string name;
        std::string key;
        bool hasState = false;
        uint32_t lastSeq = 0;
        proto::Kinematics k;
        proto::PedState ped;     // as the client sent it (v4: the shown one is Server::Shown)
        uint64_t stateRecvUs = 0; // when `k` / `ped` were SAMPLED on the server's clock (v5: the sender's PlayerState.sampleMs placed by `stateClock`)
        uint32_t ownerKey = 0;    // the source address (ENet's host value): the per-address vehicle caps count it
        // v5: the sender's 16-bit millisecond stamps placed on the server's clock (libs/sync/sampleclock.h), one per
        // stream: PlayerState (feeds Replicator::SetState, so a WorldState age is the sample's, not the packet's) and
        // VehicleUpdate (the sample time of the vehicle's motion).
        sync::SampleClock stateClock;
        sync::SampleClock vehClock;

        // v4 combat: the server's own vitals. `deathMs` is valid while dead.
        uint8_t health = proto::kDefaultHealth;
        uint8_t armor = 0;
        bool dead = false;
        uint64_t deathMs = 0;
        TokenBucket hitRate;
        // v4 authority rules (anticheat.h)
        ac::ShotLog shots;                   // the shooter's shots, rebuilt from PedState.shotSeq as the states arrive
        ac::WeaponHistory weapons;           // the weapons its states reported lately
        TokenBucket dmgBudget[ac::kWeaponKinds]; // damage per second by weapon class
        struct DeferredHit
        {
            proto::Hit h;
            uint64_t atMs = 0;
        };
        std::vector<DeferredHit> deferred;   // Hits waiting for the shot / weapon state they name
        uint64_t posMs = 0;                  // server time `k` was accepted (the displacement budget runs from it)
        uint64_t graceUntilMs = 0;           // no displacement judgement before this (join, respawn)
        uint64_t untrustedUntilMs = 0;       // inside the untrusted window until this
        // v4 vehicles
        TokenBucket vehCmdRate;
        TokenBucket vehFlood;                // drawn on by every VehicleCmd over vehCmdRate
        TokenBucket spawnRate;
        bool hasVehSeq = false;
        uint32_t vehSeq = 0;
        bool hasRateLimitedReply = false;
        uint64_t rateLimitedReplyAtMs = 0;
        bool hasRegistered = false;
        uint64_t lastRegisterMs = 0;
        // v4 emotes: 0 none, 1 a looping animation (`anim`), 2 a scenario (`scenario`); what a newcomer
        // is shown.
        TokenBucket animRate;
        uint8_t animKind = 0;
        proto::Anim anim;
        std::string scenario;

        // Appearance: `look` is what the others were told (valid when hasLook). A look that
        // arrives over the rate limit waits in `pendingLook` (a newer one replaces it) and goes out
        // when the bucket allows, so the last look always wins and nobody sees more than
        // kLookPerSec updates a second from one player.
        bool hasLook = false;
        proto::Look look;
        bool lookPending = false;
        proto::Look pendingLook;
        TokenBucket lookRate;

        // payload bytes sent to this player since secStartUs (the per-client bandwidth metric)
        uint64_t bytesOutSec = 0;
        uint64_t secStartUs = 0;

        TokenBucket chat;
        bool floodNoticed = false;
        uint64_t floodNoticeAtMs = 0;

        uint64_t dropLogAtMs[kDropKinds] = {};
        bool dropLogged[kDropKinds] = {};
        uint32_t dropSuppressed[kDropKinds] = {};
    };

    struct Server::Impl
    {
        net::DropSim dropState;
        SourceLimiter queryLimiter;
        ac::ConnectLimiter connectLimiter; // connection attempts per source address per minute
    };

    Server::Server() : impl_(new Impl), admin_(new AdminChannel) {}

    Server::~Server() { Stop(); }

    void Server::Log(const std::string& msg)
    {
        const std::string line = "[" + LogTimestamp() + "] " + msg;
        if (cfg_.log)
        {
            cfg_.log(line);
            return;
        }
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    void Server::LogDrop(Conn& c, int kind, const std::string& msg)
    {
        const uint64_t now = net::NowMs();
        const uint64_t every = kind == kDropUntrusted ? kDropLogEveryUntrustedMs : kDropLogEveryMs;
        if (c.dropLogged[kind] && now - c.dropLogAtMs[kind] < every)
        {
            c.dropSuppressed[kind]++;
            return;
        }
        std::string line = "drop id=" + std::to_string(c.id) + " " + c.name + ": " + msg;
        if (c.dropSuppressed[kind])
            line += " (+" + std::to_string(c.dropSuppressed[kind]) + " similar suppressed)";
        Log(line);
        c.dropLogged[kind] = true;
        c.dropLogAtMs[kind] = now;
        c.dropSuppressed[kind] = 0;
    }

    const char* HitReasonName(HitReason r)
    {
        switch (r)
        {
        case HitReason::None:           return "other";
        case HitReason::Rate:           return "rate";
        case HitReason::ShooterDead:    return "shooter_dead";
        case HitReason::ShooterNoState: return "shooter_no_state";
        case HitReason::ZeroDamage:     return "zero_damage";
        case HitReason::NoTarget:       return "no_target";
        case HitReason::TargetNoState:  return "target_no_state";
        case HitReason::TargetDead:     return "target_dead";
        case HitReason::Untrusted:      return "untrusted";
        case HitReason::Range:          return "range";
        case HitReason::NotRelevant:    return "not_relevant";
        case HitReason::Environment:    return "environment";
        case HitReason::NoShot:         return "no_shot";
        case HitReason::WeaponNotHeld:  return "weapon_not_held";
        case HitReason::ShotUsed:       return "shot_used";
        case HitReason::MeleeReach:     return "melee_reach";
        case HitReason::RunOver:        return "run_over";
        case HitReason::NoExplosive:    return "no_explosive";
        case HitReason::OverBudget:     return "over_budget";
        case HitReason::DeferFull:      return "defer_full";
        case HitReason::Count:          break;
        }
        return "?";
    }

    // 0.4.5 melee audit: one line every 10 s while Hits flowed, so the next live log says where they died or were cut. Silent
    // when no Hit came in the window. Key=value, every refusal reason listed with its count.
    void Server::LogHitSummary(uint64_t nowUs)
    {
        nextHitLogUs_ = nowUs + 10000000;
        const Stats& n = stats_;
        const Stats& o = hitLogMark_;
        const uint64_t acc = n.hitsAccepted - o.hitsAccepted, rej = n.hitsRejected - o.hitsRejected, def = n.hitsDeferred - o.hitsDeferred;
        if (acc + rej + def != 0)
        {
            std::string line = "combat: last 10 s: hits accepted " + std::to_string(acc) + " (melee " + std::to_string(n.hitsMelee - o.hitsMelee) +
                               ", melee clipped by a cap/ceiling/budget " + std::to_string(n.hitsMeleeClipped - o.hitsMeleeClipped) +
                               ", melee accepted only through the " + std::to_string(ac::kMeleeShotWindowMs) + " ms melee window " +
                               std::to_string(n.hitsMeleeLate - o.hitsMeleeLate) + ", clamped " + std::to_string(n.hitsClamped - o.hitsClamped) +
                               ", scaled up to the 1-point floor " + std::to_string(n.hitsFloored - o.hitsFloored) + "), damage applied " +
                               std::to_string(n.hitsDamageApplied - o.hitsDamageApplied) + ", deferred " + std::to_string(def) + ", refused " +
                               std::to_string(rej) + " [";
            bool first = true;
            for (size_t i = 0; i < kHitReasons; i++)
            {
                const uint64_t d = n.hitRefusals[i] - o.hitRefusals[i];
                if (!d) continue;
                line += std::string(first ? "" : " ") + HitReasonName((HitReason)i) + "=" + std::to_string(d);
                first = false;
            }
            line += first ? "none]" : "]";
            Log(line);
        }
        hitLogMark_ = stats_;
    }

    uint16_t Server::adminPort() const { return admin_ && admin_->running() ? admin_->port() : 0; }

    bool Server::Start(const Config& cfg, std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        if (host_) return fail("already running");
        std::string why;
        if (!Validate(cfg, &why)) return fail(why);

        cfg_ = cfg;
        if (!bans_.Load(cfg_.bansFile, &why)) return fail("ban list: " + why);

        enet_ = std::make_unique<net::EnetScope>();
        if (!enet_->ok()) return fail("ENet failed to initialise");

        ENetAddress addr;
        addr.host = ENET_HOST_ANY;
        addr.port = cfg_.port;
        if (!cfg_.bindHost.empty() && enet_address_set_host_ip(&addr, cfg_.bindHost.c_str()) != 0)
            return fail("bindHost is not an IPv4 address: " + cfg_.bindHost);

        host_ = enet_host_create(&addr, (size_t)cfg_.maxPlayers + kPendingSlots, proto::kChannelCount, 0, 0);
        if (!host_)
            return fail("cannot bind UDP port " + std::to_string(cfg_.port) + " (already in use?)");
        port_ = host_->address.port;
        host_->intercept = &Server::QueryIntercept;
        // Backstop under the per-address cap: ENet itself ignores (silently) a CONNECT
        // from an address that already holds this many peer slots, Closing ones
        // included, so one address cannot pin every slot with refused-but-lingering
        // connections. Our own cap (below it) answers with a reason first.
        if (cfg_.maxConnectionsPerIp > 0)
            host_->duplicatePeers = (size_t)cfg_.maxConnectionsPerIp * 2 + 4;

        if (cfg_.adminEnabled())
        {
            AdminOptions ao;
            ao.port = cfg_.adminPort;
            ao.token = cfg_.adminToken;
            std::string aerr;
            if (!admin_->Start(ao, &aerr))
            {
                enet_host_destroy(host_);
                host_ = nullptr;
                return fail(aerr);
            }
        }

        impl_->dropState.Configure(cfg_.dropStatePercent, cfg_.dropSeed);
        impl_->queryLimiter.Configure(cfg_.queryRatePerIp, cfg_.queryRateTotal);
        impl_->connectLimiter.Configure(cfg_.connectsPerIpPerMinute);
        stats_ = Stats();
        quitRequested_ = false;
        tick_ = 0;
        nextId_ = 0;
        {
            ReplConfig rc = cfg_.repl;
            rc.maxPlayers = cfg_.maxPlayers;
            rc.tickHz = cfg_.tickHz;
            repl_.Configure(rc);

            VehicleStore::Rules vr;
            vr.maxVehicles = cfg_.maxVehicles;
            vr.perPlayer = cfg_.vehiclesPerPlayer;
            vr.perIp = cfg_.vehiclesPerIp;
            vr.orphanMs = cfg_.vehicleOrphanMs;
            vr.emptyDespawnMs = cfg_.vehicleEmptyDespawnMs;
            vr.enterDistance = (float)cfg_.vehicleEnterDistance;
            vr.maxSpeed = (float)cfg_.vehicleMaxSpeed;
            vr.fuelDrainPerMinute = cfg_.fuelDrainPerMinute;
            vr.enterRadius = (float)cfg_.repl.enterRadius;
            vr.leaveRadius = (float)cfg_.repl.leaveRadius;
            vr.nearDist = (float)cfg_.repl.nearDist;
            vr.midDist = (float)cfg_.repl.midDist;
            auto every = [&](uint32_t hz) { return hz == 0 ? (uint32_t)cfg_.tickHz : std::max<uint32_t>(1, cfg_.tickHz / hz); };
            vr.nearEvery = every(cfg_.repl.rateNearHz);
            vr.midEvery = every(cfg_.repl.rateMidHz);
            vr.farEvery = every(cfg_.repl.rateFarHz);
            vr.refreshTicks = cfg_.tickHz;
            vr.packetMaxBytes = cfg_.repl.packetMaxBytes;
            vehicles_.Configure(vr, cfg_.maxPlayers);
            nextSpawnPoint_ = 0;
        }
        startUs_ = NowUs();
        tickClock_.Start(startUs_, 1000000u / cfg_.tickHz);
        tickNominalUs_ = startUs_;
        metrics_ = Metrics();
        statsMark_ = Metrics();
        logMark_ = Metrics();
        nextSyncLogUs_ = NowUs() + 10000000;
        statsMarkUs_ = startUs_;
        nextSecondUs_ = startUs_ + 1000000;
        nextHitLogUs_ = startUs_ + 10000000;
        cpuBase_ = ThreadCpuSeconds();
        lastWireSentData_ = host_->totalSentData;
        lastWireSentPackets_ = host_->totalSentPackets;
        lastWireRecvData_ = host_->totalReceivedData;
        lastWireRecvPackets_ = host_->totalReceivedPackets;

        Log("listening on UDP " + std::to_string(port_) + ", name \"" + cfg_.name + "\", max " +
            std::to_string(cfg_.maxPlayers) + " players, " + std::to_string(cfg_.tickHz) + " Hz, protocol v" +
            std::to_string(proto::kVersion));
        Log("replication: relevant within " + std::to_string(cfg_.repl.enterRadius) + " m (dropped beyond " +
            std::to_string(cfg_.repl.leaveRadius) + " m), " + std::to_string(cfg_.repl.rateNearHz) + "/" +
            std::to_string(cfg_.repl.rateMidHz) + "/" + std::to_string(cfg_.repl.rateFarHz) + " Hz near/mid/far, " +
            std::to_string(cfg_.repl.budgetBytesPerSec) + " B/s budget per client");
        // W8 item 1: no silent pins. The whole effective replication config and every combat number are in the log, and a
        // value that undercuts another (rateNearHz under tickHz) is a WARNING naming both.
        Log(DescribeRepl(cfg_));
        Log(DescribeCombat(cfg_));
        Log(DescribeLatestClient(cfg_));
        for (const std::string& w : ConfigWarnings(cfg_)) Log("WARNING: " + w);
        Log("authority: hits tied to shots within " +
            (cfg_.hitShotWindowMs ? std::to_string(cfg_.hitShotWindowMs) + " ms (melee swings within " +
                                        std::to_string(ac::ShotWindowFor(ac::WeaponKind::Melee, cfg_.hitShotWindowMs)) + " ms)"
                                  : std::string("(shot binding OFF)")) +
            ", damage caps x" + std::to_string(cfg_.hitDamageScalePercent) + "%, movement check " +
            (cfg_.untrustedWindowMs ? std::to_string(cfg_.untrustedWindowMs) + " ms window" : std::string("OFF")) + ", " +
            (cfg_.connectsPerIpPerMinute ? std::to_string(cfg_.connectsPerIpPerMinute) + " connects/min per address"
                                         : std::string("no connect rate limit")) +
            ", " + std::to_string(cfg_.vehiclesPerIp) + " vehicles per address");
        Log("bans: " + std::to_string(bans_.entries().size()) + " entries from " +
            (cfg_.bansFile.empty() ? std::string("memory only") : cfg_.bansFile));
        if (admin_->running())
            Log("admin channel on 127.0.0.1:" + std::to_string(admin_->port()) + " (loopback only, token required)");

        if (!cfg_.masterUrl.empty())
        {
            AnnounceSettings as;
            std::string perr;
            ParseHttpUrl(cfg_.masterUrl, as.master, &perr); // Validate() accepted it
            as.gamePort = port_;
            as.name = cfg_.name;
            as.gamemode = cfg_.gamemode;
            as.lang = cfg_.lang;
            as.url = cfg_.url;
            as.protocol = proto::kVersion;
            as.maxPlayers = cfg_.maxPlayers;
            as.intervalMs = cfg_.announceIntervalMs ? cfg_.announceIntervalMs : cfg_.heartbeatSeconds * 1000u;
            as.backoffBaseMs = cfg_.announceBackoffBaseMs;
            as.backoffMaxMs = cfg_.announceBackoffMaxMs;
            std::string aerr;
            if (announcer_.Start(as, &aerr))
                Log("announcing to " + cfg_.masterUrl + " every " + std::to_string(as.intervalMs / 1000) + " s");
            else
                Log("ERROR: master announce not started: " + aerr);
        }
        return true;
    }

    int Server::QueryIntercept(_ENetHost* host, _ENetEvent*)
    {
        Server* s = t_servicing;
        if (!s || s->host_ != host) return 0; // not ours: let ENet parse it
        return s->AnswerQuery(host);
    }

    int Server::AnswerQuery(_ENetHost* host)
    {
        // 1 = handled here, ENet never sees the datagram; 0 = not a query, ENet parses it.
        if (!proto::query::IsRequest(host->receivedData, host->receivedDataLength)) return 0;
        proto::query::Request req;
        if (!proto::query::Decode(host->receivedData, host->receivedDataLength, req)) return 0;
        if (!impl_->queryLimiter.Allow(host->receivedAddress.host, net::NowMs()))
        {
            stats_.queryRateLimited++; // silently dropped: the limit is the whole point
            return 1;
        }
        proto::query::Reply rep;
        rep.protocol = proto::kVersion;
        rep.nonce = req.nonce;
        rep.players = (uint16_t)players_.size();
        rep.maxPlayers = cfg_.maxPlayers;
        // The v2 reply (same 24 bytes) only to a request that asked for it; everyone else gets the exact v1 reply
        // that 0.4.2 / 0.4.3 launchers and the master decode.
        if (req.replyVersion >= proto::query::kQueryVersion2)
        {
            rep.version = proto::query::kQueryVersion2;
            rep.latestClient = cfg_.latestClient;
        }
        const proto::query::Datagram out = proto::query::Encode(rep);
        ENetBuffer buf;
        buf.data = const_cast<uint8_t*>(out.data());
        buf.dataLength = out.size();
        enet_socket_send(host->socket, &host->receivedAddress, &buf, 1);
        stats_.queryAnswered++;
        return 1;
    }

    void Server::Poll(uint32_t timeoutMs)
    {
        if (!host_) return;
        const Servicing servicing(this);
        const uint64_t now = NowUs();
        const uint64_t nextTickUs = tickClock_.NextDueUs();
        const uint64_t untilTickMs = nextTickUs > now ? (nextTickUs - now + 999) / 1000 : 0;
        const uint32_t wait = (uint32_t)std::min<uint64_t>(timeoutMs, untilTickMs);

        ENetEvent ev;
        int r = enet_host_service(host_, &ev, wait);
        while (r > 0 && host_)
        {
            switch (ev.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                HandleConnect(ev.peer);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
            {
                auto it = conns_.find(ev.peer);
                if (it != conns_.end())
                {
                    metrics_.packetsIn++;
                    metrics_.payloadBytesIn += ev.packet->dataLength;
                    HandleReceive(*it->second, ev.channelID, ev.packet->data, ev.packet->dataLength);
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                HandleDisconnect(ev.peer, ev.data);
                break;
            case ENET_EVENT_TYPE_NONE:
                break;
            }
            r = enet_host_service(host_, &ev, 0);
        }
        if (r < 0)
            Log("ENet service error");
        Tick(NowUs());
    }

    void Server::Tick(uint64_t nowUs)
    {
        const uint64_t nowMs = net::NowMs();

        // Timers over connections that are not (or no longer) playing.
        std::vector<Conn*> helloLate, stuck;
        for (auto& kv : conns_)
        {
            Conn& c = *kv.second;
            if (c.st == Conn::St::AwaitHello && nowMs - c.connectedAtMs >= cfg_.helloTimeoutMs)
                helloLate.push_back(&c);
            else if (c.st == Conn::St::Closing && nowMs - c.closingAtMs >= kCloseGraceMs)
                stuck.push_back(&c);
        }
        for (Conn* c : helloLate)
            Refuse(*c, proto::RefuseReason::HelloTimeout, "no Hello within " + std::to_string(cfg_.helloTimeoutMs) + " ms");
        for (Conn* c : stuck)
        {
            // The client never completed the close handshake; free the slot.
            ENetPeer* peer = c->peer;
            enet_peer_reset(peer);
            peer->data = nullptr;
            conns_.erase(peer);
        }

        // The announcer lives on its own thread: feed it the player count and log what it
        // queued, here, so the log sink only ever sees this thread.
        if (announcer_.running())
        {
            announcer_.SetPlayers((uint16_t)players_.size());
            for (const std::string& line : announcer_.TakeLog()) Log(line);
        }
        if (admin_->running())
        {
            admin_->Service(
                nowMs,
                [this](const std::string& line, std::string& out) {
                    const CommandResult r = Execute(line, out);
                    return r != CommandResult::Error;
                },
                [this](const std::string& line) { Log(line); });
        }

        if (tickClock_.Due(nowUs))
        {
            // Tick numbers follow wall time (see tickclock.h): after a stall the number jumps by the periods the
            // loop slept through, those ticks never happen, and clients keep placing samples at the right time.
            // The tick describes the world as of its own slot, whatever the time the loop got to it.
            const TickClock::Slot slot = tickClock_.Advance(nowUs);
            metrics_.tickLateUs.Add(slot.lateUs);
            metrics_.ticksMissed += slot.missed;
            const uint64_t workStart = NowUs();
            tick_ = slot.tick; // world ticks are numbered from 1: 0 means "nothing yet" in a WorldAck
            tickNominalUs_ = slot.nominalUs;
            FlushPendingLooks();
            for (auto& kv : players_) // Hits waiting for a shot that never came are judged now
                if (!kv.second->deferred.empty()) RetryDeferredHits(*kv.second, nowMs);
            BroadcastWorld();
            BroadcastVehicles(nowMs);
            stats_.ticks++;
            metrics_.ticks++;
            if (host_) enet_host_flush(host_);
            metrics_.tickWorkUs.Add(NowUs() - workStart);
        }
        if (host_) enet_host_flush(host_);
        SampleMetrics(nowUs);
    }

    void Server::SampleMetrics(uint64_t nowUs)
    {
        if (!host_) return;
        // ENet's counters are 32-bit and wrap; reading them every Poll keeps each delta far
        // below 4 GB.
        metrics_.wireBytesOut += (uint32_t)(host_->totalSentData - lastWireSentData_);
        metrics_.wireDatagramsOut += (uint32_t)(host_->totalSentPackets - lastWireSentPackets_);
        metrics_.wireBytesIn += (uint32_t)(host_->totalReceivedData - lastWireRecvData_);
        metrics_.wireDatagramsIn += (uint32_t)(host_->totalReceivedPackets - lastWireRecvPackets_);
        lastWireSentData_ = host_->totalSentData;
        lastWireSentPackets_ = host_->totalSentPackets;
        lastWireRecvData_ = host_->totalReceivedData;
        lastWireRecvPackets_ = host_->totalReceivedPackets;
        metrics_.playersJoined = players_.size();
        metrics_.cpuSeconds = ThreadCpuSeconds() - cpuBase_;
        metrics_.wallSeconds = (double)(nowUs - startUs_) * 1e-6;

        // Once a second: one bandwidth sample per player that was here for the whole second.
        if (nowUs >= nextSecondUs_)
        {
            for (auto& kv : players_)
            {
                Conn& c = *kv.second;
                if (nowUs - c.secStartUs >= 950000)
                {
                    metrics_.clientBytesPerSec.Add((uint64_t)((double)c.bytesOutSec * 1e6 / (double)(nowUs - c.secStartUs)));
                    c.bytesOutSec = 0;
                    c.secStartUs = nowUs;
                }
            }
            nextSecondUs_ = nowUs + 1000000;

            // ENet's view of the joined peers (gauges): the state channel's silent throttle, loss and round trips.
            uint64_t peers = 0, throttleMin = ENET_PEER_PACKET_THROTTLE_SCALE, lossMax = 0, rttMax = 0, rttSum = 0, varMax = 0;
            for (auto& kv : players_)
            {
                const ENetPeer* p = kv.second->peer;
                if (!p) continue;
                peers++;
                throttleMin = std::min<uint64_t>(throttleMin, p->packetThrottle);
                lossMax = std::max<uint64_t>(lossMax, (uint64_t)p->packetLoss * 1000 / ENET_PEER_PACKET_LOSS_SCALE);
                rttMax = std::max<uint64_t>(rttMax, p->roundTripTime);
                rttSum += p->roundTripTime;
                varMax = std::max<uint64_t>(varMax, p->roundTripTimeVariance);
            }
            metrics_.enetPeers = peers;
            metrics_.enetThrottleMin = throttleMin;
            metrics_.enetLossPermilleMax = lossMax;
            metrics_.enetRttMsMax = rttMax;
            metrics_.enetRttMsAvg = peers ? rttSum / peers : 0;
            metrics_.enetRttVarMsMax = varMax;
        }

        // The replication health line, every 10 s while somebody is joined (the same numbers as `stats`, compact).
        if (nowUs >= nextSyncLogUs_)
        {
            const double secs = metrics_.wallSeconds - logMark_.wallSeconds;
            if (!players_.empty() && secs > 0.5)
                Log(FormatSyncLine(metrics_, logMark_, secs, cfg_.tickHz, players_.size()));
            logMark_ = metrics_;
            nextSyncLogUs_ = nowUs + 10000000;
        }
        if (nowUs >= nextHitLogUs_) LogHitSummary(nowUs);
    }

    void Server::BroadcastWorld()
    {
        // The Replicator decides what each receiver is told and builds the packets; this only
        // hands them to ENet. The unreliable state channel carries both packet kinds.
        repl_.Tick(
            tick_, tickNominalUs_,
            [this](uint16_t receiver, bool, const proto::Bytes& packet) {
                const auto it = players_.find(receiver);
                if (it != players_.end()) SendTo(*it->second, proto::kChanState, packet, false);
            },
            metrics_);
    }

    bool Server::SetAlwaysRelevant(uint16_t subject, uint16_t receiver, bool on, std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        if (!players_.count(subject)) return fail("subject " + std::to_string(subject) + " is not a joined player");
        if (receiver != proto::kServerId && !players_.count(receiver))
            return fail("receiver " + std::to_string(receiver) + " is not a joined player (use kServerId for everyone)");
        if (receiver == subject) return fail("a player is never told about itself: subject == receiver");
        return repl_.SetAlwaysRelevant(subject, receiver, on);
    }

    bool Server::AlwaysRelevant(uint16_t subject, uint16_t receiver) const
    {
        return players_.count(subject) && (receiver == proto::kServerId || players_.count(receiver)) &&
               repl_.AlwaysRelevant(subject, receiver);
    }

    bool Server::IsRelevant(uint16_t receiver, uint16_t subject) const
    {
        return players_.count(receiver) && players_.count(subject) && repl_.Relevant(receiver, subject);
    }

    size_t Server::RelevantCount(uint16_t receiver) const
    {
        return players_.count(receiver) ? repl_.RelevantCount(receiver) : 0;
    }

    size_t Server::LiveConnectionsFrom(const std::string& ip, const Conn* except) const
    {
        size_t n = 0;
        for (const auto& kv : conns_)
        {
            const Conn& c = *kv.second;
            if (&c != except && c.st != Conn::St::Closing && c.addr == ip) n++;
        }
        return n;
    }

    void Server::HandleConnect(_ENetPeer* peer)
    {
        auto owned = std::make_unique<Conn>();
        Conn& c = *owned;
        c.peer = peer;
        c.connectedAtMs = net::NowMs();
        c.addr = AddressOf(peer);
        c.ownerKey = peer->address.host;
        peer->data = &c;
        enet_peer_timeout(peer, cfg_.peerTimeoutLimit, cfg_.peerTimeoutMinMs, cfg_.peerTimeoutMaxMs);
        // ENet's packet throttle deletes a share of UNRELIABLE packets (this server's whole state channel, both ways) each
        // time a round trip is much slower than the last 5 s's quietest (2/32 per such acknowledgement) and wins it back
        // only as fast as quick ones come: a few seconds of congestion take it to a third of full and cost the state channel
        // about a fifth of its packets over the next 10 s (tests/server_test.cpp models it on ENet's own function), at the
        // moment the link is worst, with no counter anywhere (sync pipeline review 2026-11-03). Deceleration 0 keeps it at
        // full. The command is applied by the client's peer too (it throttles what it sends us), so an older client is
        // covered as well.
        enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_ACCELERATION, 0);
        Log("connect " + c.addr + ":" + std::to_string(peer->address.port));
        conns_[peer] = std::move(owned);

        // Before Hello: a banned address never gets as far as saying who it is, and one
        // address cannot hold more than its share of connections.
        if (const BanEntry* ban = bans_.FindIp(c.addr))
        {
            RefuseBanned(c, *ban);
            return;
        }
        // The connect RATE comes next, before any Hello work: a banned address costs nothing here, and one
        // that reconnects over and over (to reset per-connection caps, or just to hammer the server) is told
        // to slow down: a bucket of the limit refilled evenly over a minute.
        if (!impl_->connectLimiter.Allow(c.ownerKey, c.connectedAtMs))
        {
            stats_.refusedConnectRate++;
            Refuse(c, proto::RefuseReason::TooManyConnections,
                   "too many connection attempts from your address (limit " + std::to_string(cfg_.connectsPerIpPerMinute) +
                       " per minute)");
            return;
        }
        if (cfg_.maxConnectionsPerIp > 0 && LiveConnectionsFrom(c.addr, &c) >= cfg_.maxConnectionsPerIp)
        {
            stats_.refusedIpCap++;
            Refuse(c, proto::RefuseReason::TooManyConnections,
                   "too many connections from your address (limit " + std::to_string(cfg_.maxConnectionsPerIp) + ")");
            return;
        }
    }

    void Server::HandleDisconnect(_ENetPeer* peer, uint32_t data)
    {
        auto it = conns_.find(peer);
        if (it == conns_.end()) return;
        Conn& c = *it->second;
        if (c.st == Conn::St::Joined)
            RemovePlayer(c, data == proto::kDisconnectQuit ? proto::LeaveReason::Quit : proto::LeaveReason::Timeout);
        else if (c.st == Conn::St::AwaitHello)
            Log("disconnect " + c.addr + " before Hello");
        peer->data = nullptr;
        conns_.erase(it);
    }

    void Server::HandleReceive(Conn& c, uint8_t channel, const uint8_t* data, size_t len)
    {
        if (c.st == Conn::St::Closing) return; // already on the way out

        proto::Msg type;
        if (!proto::PeekType(data, len, type))
        {
            Refuse(c, proto::RefuseReason::Protocol, "empty or unknown message type");
            return;
        }

        if (c.st == Conn::St::AwaitHello)
        {
            if (type != proto::Msg::Hello || channel != proto::kChanReliable)
            {
                Refuse(c, proto::RefuseReason::Protocol,
                       std::string("expected Hello first, got ") + proto::ToString(type));
                return;
            }
            OnHello(c, data, len);
            return;
        }

        // Joined: PlayerState and VehicleUpdate (state channel); ChatSend, PlayerLook, VehicleCmd,
        // Hit, Respawn, PlayAnim, PlayScenario and StopAnim (reliable) are the client messages; a
        // message on the wrong channel is as wrong as a wrong type.
        const bool rel = channel == proto::kChanReliable;
        if (type == proto::Msg::PlayerState && channel == proto::kChanState)
            OnState(c, data, len);
        else if (type == proto::Msg::VehicleUpdate && channel == proto::kChanState)
            OnVehicleUpdate(c, data, len);
        else if (type == proto::Msg::ChatSend && rel)
            OnChat(c, data, len);
        else if (type == proto::Msg::PlayerLook && rel)
            OnLook(c, data, len);
        else if (type == proto::Msg::VehicleCmd && rel)
            OnVehicleCmd(c, data, len);
        else if (type == proto::Msg::Hit && rel)
            OnHit(c, data, len);
        else if (type == proto::Msg::Respawn && rel)
            OnRespawn(c, data, len);
        else if ((type == proto::Msg::PlayAnim || type == proto::Msg::PlayScenario || type == proto::Msg::StopAnim) && rel)
            OnAnim(c, type, data, len);
        else
            Refuse(c, proto::RefuseReason::Protocol,
                   std::string("unexpected ") + proto::ToString(type) + " on channel " + std::to_string(channel));
    }

    void Server::OnHello(Conn& c, const uint8_t* data, size_t len)
    {
        proto::Hello h;
        if (!proto::Decode(data, len, h))
        {
            Refuse(c, proto::RefuseReason::Protocol, "malformed Hello");
            return;
        }
        if (h.version != proto::kVersion)
        {
            // The text is what the player reads (the engine shows it on the loading screen and in chat), so it
            // says what to DO, not which numbers differ; the numbers go to the server log. The reason code is
            // unchanged (VersionMismatch), so tools and tests that key on it keep working.
            Log("version mismatch " + c.addr + ": server speaks protocol v" + std::to_string(proto::kVersion) +
                ", client sent v" + std::to_string(h.version));
            Refuse(c, proto::RefuseReason::VersionMismatch,
                   h.version < proto::kVersion ? proto::kRefuseTextClientOld : proto::kRefuseTextClientNew);
            return;
        }
        if (!proto::ValidName(h.name))
        {
            Refuse(c, proto::RefuseReason::BadName,
                   "name must be 1.." + std::to_string(proto::kMaxName) +
                       " characters of A-Z a-z 0-9 _ . - and single inner spaces");
            return;
        }
        // By name, and by address again (a ban added while the connection sat waiting
        // for its Hello is caught here as well as by RefuseAddress).
        if (const BanEntry* ban = bans_.Match(h.name, c.addr))
        {
            RefuseBanned(c, *ban);
            return;
        }
        if (players_.size() >= cfg_.maxPlayers)
        {
            Refuse(c, proto::RefuseReason::ServerFull, "server is full");
            return;
        }
        if (nameKeys_.count(proto::NameKey(h.name)))
        {
            Refuse(c, proto::RefuseReason::NameTaken, "name is already in use");
            return;
        }
        Accept(c, h.name);
    }

    void Server::Accept(Conn& c, const std::string& name)
    {
        c.id = AllocId();
        c.name = name;
        c.key = proto::NameKey(name);
        c.st = Conn::St::Joined;
        c.secStartUs = NowUs();
        c.chat.Configure((double)cfg_.chatBurst, (double)cfg_.chatBurst / ((double)cfg_.chatWindowMs / 1000.0));
        c.lookRate.Configure(kLookBurst, kLookPerSec);
        c.health = (uint8_t)cfg_.playerHealth;
        c.hitRate.Configure((double)cfg_.hitsPerSecond, (double)cfg_.hitsPerSecond);
        c.vehCmdRate.Configure(kVehCmdBurst, kVehCmdPerSec);
        c.vehFlood.Configure(kVehFloodBurst, kVehFloodPerSec);
        c.spawnRate.Configure(kSpawnBurst, kSpawnPerSec);
        c.animRate.Configure(kAnimBurst, kAnimPerSec);
        for (size_t k = 0; k < ac::kWeaponKinds; k++) // a burst of ac::kDamageBudgetSec seconds of the class's rate
        {
            const ac::WeaponRule r = ac::RuleForKind((ac::WeaponKind)k, cfg_.hitDamageScalePercent);
            c.dmgBudget[k].Configure((double)r.maxPerSec * ac::kDamageBudgetSec, (double)r.maxPerSec);
        }

        proto::Welcome w;
        w.yourId = c.id;
        w.tickHz = cfg_.tickHz;
        w.serverName = cfg_.name;
        w.playerHealth = (uint8_t)cfg_.playerHealth; // v5: the effective maximum, so the client sizes its health bar and its ped to it
        SendTo(c, proto::kChanReliable, proto::Encode(w), true);

        // Who is already here, then what they look like (reliable and ordered: every PeerLook
        // arrives after the PlayerJoined of its id), then announce the newcomer to them.
        for (const auto& kv : players_)
        {
            proto::PlayerJoined pj;
            pj.id = kv.second->id;
            pj.name = kv.second->name;
            SendTo(c, proto::kChanReliable, proto::Encode(pj), true);
        }
        for (const auto& kv : players_)
        {
            if (!kv.second->hasLook) continue;
            proto::PeerLook pl;
            pl.id = kv.second->id;
            pl.look = kv.second->look;
            SendTo(c, proto::kChanReliable, proto::Encode(pl), true);
            stats_.looksReplayed++;
        }
        // ...then what they are playing (a loop or a scenario; one-shots are long over).
        for (const auto& kv : players_)
        {
            const Conn& o = *kv.second;
            if (o.animKind == 1)
            {
                proto::PeerAnim pa;
                pa.id = o.id;
                pa.anim = o.anim;
                SendTo(c, proto::kChanReliable, proto::Encode(pa), true);
            }
            else if (o.animKind == 2)
            {
                proto::PeerScenario ps;
                ps.id = o.id;
                ps.name = o.scenario;
                SendTo(c, proto::kChanReliable, proto::Encode(ps), true);
            }
            else
                continue;
            stats_.animsReplayed++;
        }
        proto::PlayerJoined self;
        self.id = c.id;
        self.name = c.name;
        SendToAll(proto::Encode(self), &c);

        players_[c.id] = &c;
        nameKeys_[c.key] = c.id;
        repl_.AddPlayer(c.id);
        vehicles_.AddReceiver(c.id);
        stats_.joins++;
        Log("join id=" + std::to_string(c.id) + " name=" + c.name + " addr=" + c.addr + " players=" +
            std::to_string(players_.size()) + "/" + std::to_string(cfg_.maxPlayers));
    }

    void Server::OnState(Conn& c, const uint8_t* data, size_t len)
    {
        if (impl_->dropState.Drop())
        {
            stats_.statesSimDropped++;
            return;
        }
        proto::PlayerState s;
        if (!proto::Decode(data, len, s))
        {
            stats_.statesMalformed++;
            LogDrop(c, kDropMalformed, "malformed PlayerState (truncated, trailing bytes or a non-finite float)");
            return;
        }
        if (c.hasState && !proto::SeqNewer(s.seq, c.lastSeq))
        {
            stats_.statesStale++;
            LogDrop(c, kDropStale, "stale PlayerState seq " + std::to_string(s.seq) + " (last accepted " +
                                       std::to_string(c.lastSeq) + ")");
            return;
        }
        if (!proto::ValidKinematics(s.k))
        {
            stats_.statesInvalid++;
            LogDrop(c, kDropInvalid, "PlayerState outside the world box or speed limit (x=" + std::to_string(s.k.x) +
                                         " y=" + std::to_string(s.k.y) + " z=" + std::to_string(s.k.z) + ")");
            return;
        }
        // Decode already refused an unknown move, reserved flags and aim bytes without the aiming
        // flag; this is the cross-field policy (a driver must be in a vehicle).
        if (!proto::ValidPedState(s.ped))
        {
            stats_.statesInvalid++;
            LogDrop(c, kDropInvalid, "PlayerState with an inconsistent on-foot state (move=" + std::to_string(s.ped.move) +
                                         " flags=" + std::to_string(s.ped.flags) + ")");
            return;
        }
        // The authority rules see the state before it replaces the old one: the displacement is judged against
        // the previous position, and the shot counter and weapon feed the Hit checks.
        const uint64_t nowMs = net::NowMs();
        if (!c.hasState)
            c.graceUntilMs = nowMs + cfg_.untrustedJoinGraceMs; // the first state is the baseline, the engine's own spawn teleport follows
        else
            CheckMove(c, s, nowMs);
        c.shots.OnState(s.ped.shotSeq, s.ped.weapon, nowMs);
        c.weapons.Note(s.ped.weapon, nowMs);
        c.posMs = nowMs;
        c.lastSeq = s.seq;
        c.hasState = true;
        c.k = s.k;
        c.ped = s.ped;
        // v5: the sample time is the sender's own stamp placed on the server's clock, not the packet's arrival.
        c.stateRecvUs = c.stateClock.Place(s.sampleMs, NowUs());
        repl_.SetState(c.id, s.k, Shown(c), c.stateRecvUs);
        repl_.SetAck(c.id, s.ack);
        stats_.statesAccepted++;
        RetryDeferredHits(c, nowMs); // a Hit that got here before the state it names
    }

    // ---- v4: authority rules (anticheat.h) ----------------------------------------------------------

    bool Server::Untrusted(const Conn& c, uint64_t nowMs) const
    {
        return cfg_.untrustedWindowMs != 0 && c.untrustedUntilMs > nowMs;
    }

    void Server::MarkUntrusted(Conn& c, uint64_t nowMs, const std::string& why)
    {
        stats_.movesRejected++;
        c.untrustedUntilMs = std::max<uint64_t>(c.untrustedUntilMs, nowMs + cfg_.untrustedWindowMs);
        LogDrop(c, kDropUntrusted, "untrusted for " + std::to_string(cfg_.untrustedWindowMs) + " ms: " + why);
    }

    // Judges a new position against the previous accepted one by SERVER time (the reported velocity is a
    // claim, not a measurement): too far for the time that passed is a jump. The state is still taken (a
    // legitimate reset must not desync the player), but the player is distrusted for a while.
    void Server::CheckMove(Conn& c, const proto::PlayerState& s, uint64_t nowMs)
    {
        if (cfg_.untrustedWindowMs == 0 || nowMs < c.graceUntilMs) return;
        const bool fast = ac::FastPed(s.ped) || ac::FastPed(c.ped);
        const float limit = fast ? (float)cfg_.vehicleMaxSpeed : (float)cfg_.onFootMaxSpeed;
        const double dtSec = nowMs > c.posMs ? (double)(nowMs - c.posMs) / 1000.0 : 0.0;
        const float moved = ac::Dist(s.k, c.k);
        if (moved <= ac::AllowedMove(limit, dtSec)) return;
        char buf[160];
        std::snprintf(buf, sizeof buf, "PlayerState moved %.1f m in %.0f ms (%s limit %.0f m/s)", (double)moved,
                      dtSec * 1000.0, fast ? "vehicle" : "on-foot", (double)limit);
        MarkUntrusted(c, nowMs, buf);
    }

    bool Server::ModelAllowed(uint32_t model) const
    {
        const auto in = [&](const std::vector<uint32_t>& l) { return std::find(l.begin(), l.end(), model) != l.end(); };
        if (!cfg_.vehicleModelAllow.empty()) return in(cfg_.vehicleModelAllow); // the allow list wins
        return !in(cfg_.vehicleModelDeny);
    }

    void Server::Notice(Conn& c, const std::string& text)
    {
        proto::ChatMessage notice;
        notice.fromId = proto::kServerId;
        notice.text = text;
        SendTo(c, proto::kChanReliable, proto::Encode(notice), true);
    }

    void Server::OnChat(Conn& c, const uint8_t* data, size_t len)
    {
        proto::ChatSend m;
        if (!proto::Decode(data, len, m))
        {
            Refuse(c, proto::RefuseReason::Protocol, "malformed ChatSend (over " + std::to_string(proto::kMaxChat) +
                                                         " bytes or truncated)");
            return;
        }
        // Flood limit first, so invalid text cannot be used to spam notices either.
        const uint64_t nowMs = net::NowMs();
        if (!c.chat.Take(nowMs))
        {
            stats_.chatFlooded++;
            LogDrop(c, kDropFlood, "chat flood: over " + std::to_string(cfg_.chatBurst) + " messages per " +
                                       std::to_string(cfg_.chatWindowMs) + " ms, message dropped");
            if (!c.floodNoticed || nowMs - c.floodNoticeAtMs >= kFloodNoticeEveryMs)
            {
                c.floodNoticed = true;
                c.floodNoticeAtMs = nowMs;
                Notice(c, "Message not sent: you are sending messages too fast, slow down.");
            }
            return;
        }
        if (!proto::ValidChat(m.text))
        {
            stats_.chatRejected++;
            LogDrop(c, kDropChat, "chat rejected (empty, control characters or invalid UTF-8)");
            Notice(c, "Message not sent: it was empty or contained control or invalid characters.");
            return;
        }
        proto::ChatMessage out;
        out.fromId = c.id;
        out.text = m.text;
        SendToAll(proto::Encode(out), nullptr);
        stats_.chatDelivered++;
        Log("chat " + c.name + ": " + m.text);
    }

    void Server::OnLook(Conn& c, const uint8_t* data, size_t len)
    {
        proto::PlayerLook m;
        if (!proto::Decode(data, len, m))
        {
            // A reliable message that does not frame is a broken or hostile client, like ChatSend.
            Refuse(c, proto::RefuseReason::Protocol, "malformed PlayerLook (truncated or trailing bytes)");
            return;
        }
        if (!proto::ValidLook(m.look))
        {
            stats_.looksInvalid++;
            LogDrop(c, kDropLook, "PlayerLook rejected (model 0 or a component, prop or head blend value out of range)");
            return;
        }
        // Back to what the others already see while a newer one waits: the waiting one is moot.
        if (c.lookPending && c.hasLook && m.look == c.look)
        {
            c.lookPending = false;
            stats_.looksCoalesced++;
            return;
        }
        // Compare with the newest look the server holds for this player (the waiting one first).
        const proto::Look& newest = c.lookPending ? c.pendingLook : c.look;
        if ((c.hasLook || c.lookPending) && newest == m.look)
        {
            stats_.looksUnchanged++;
            return;
        }
        if (c.lookRate.Take(net::NowMs()))
        {
            c.lookPending = false; // superseded: this one is newer
            PublishLook(c, m.look);
            return;
        }
        if (c.lookPending) stats_.looksCoalesced++; // the waiting one is replaced and never sent
        c.lookPending = true;
        c.pendingLook = m.look;
        stats_.looksDeferred++;
        LogDrop(c, kDropLook, "PlayerLook over " + std::to_string((int)kLookPerSec) + "/s: held back, the newest is sent when allowed");
    }

    void Server::PublishLook(Conn& c, const proto::Look& look)
    {
        c.look = look;
        c.hasLook = true;
        proto::PeerLook out;
        out.id = c.id;
        out.look = look;
        SendToAll(proto::Encode(out), &c);
        stats_.looksBroadcast++;
    }

    void Server::FlushPendingLooks()
    {
        const uint64_t nowMs = net::NowMs();
        for (auto& kv : players_)
        {
            Conn& c = *kv.second;
            if (!c.lookPending || !c.lookRate.Take(nowMs)) continue;
            c.lookPending = false;
            PublishLook(c, c.pendingLook);
        }
    }

    // ---- v4: vehicles -------------------------------------------------------------------------------

    proto::PedState Server::Shown(const Conn& c) const
    {
        proto::PedState p = c.ped; // the client's own on-foot state...
        p.health = c.health;       // ...with the server's vitals
        p.armor = c.armor;
        if (c.dead) p.flags |= proto::kPfDead;
        return p;
    }

    void Server::PushShown(Conn& c)
    {
        if (c.hasState) repl_.SetState(c.id, c.k, Shown(c), c.stateRecvUs);
    }

    void Server::VehicleDestroyed(uint16_t id)
    {
        stats_.vehiclesDestroyed++;
        Log("vehicle " + std::to_string(id) + " removed");
    }

    void Server::BroadcastVehicles(uint64_t nowMs)
    {
        std::vector<uint16_t> gone;
        vehicles_.Tick(nowMs, tick_, &gone);
        for (uint16_t id : gone) VehicleDestroyed(id);
        // v5: who controls each vehicle (the driver, else the nearest trusted player who has it in view), decided
        // once per tick before the frames are built, so every receiver is told the same ctrl.
        controlCandidates_.clear();
        for (const auto& kv : players_)
        {
            const Conn& c = *kv.second;
            VehicleStore::ControlCandidate cand;
            cand.id = c.id;
            cand.pos = c.k;
            cand.hasState = c.hasState;
            cand.trusted = !Untrusted(c, nowMs) && !c.dead;
            controlCandidates_.push_back(cand);
        }
        vehicles_.AssignControllers(nowMs, tick_, controlCandidates_);
        // The entries' ageMs is measured against the tick's NOMINAL time (its slot), not the clock of a loop that got
        // round to it late (the same rule as the WorldState entries').
        const uint64_t nominalMs = tickNominalUs_ / 1000;
        proto::VehicleWorld frame;
        for (auto& kv : players_)
        {
            Conn& c = *kv.second;
            if (!vehicles_.BuildFrame(c.id, c.hasState, c.k, tick_, nominalMs, frame)) continue;
            const proto::Bytes bytes = proto::Encode(frame);
            if (bytes.empty()) continue; // cannot happen with the store's own data; never send a broken frame
            SendTo(c, proto::kChanState, bytes, false);
            stats_.vehicleFrames++;
        }
    }

    std::vector<proto::VehicleState> Server::Vehicles() const
    {
        std::vector<proto::VehicleState> out;
        for (uint16_t id = 0; id < proto::kMaxVehicles; id++)
            if (const VehicleStore::Vehicle* v = vehicles_.Get(id)) out.push_back(v->s);
        return out;
    }

    void Server::OnVehicleUpdate(Conn& c, const uint8_t* data, size_t len)
    {
        proto::VehicleUpdate u;
        if (!proto::Decode(data, len, u))
        {
            stats_.vehicleUpdatesInvalid++;
            LogDrop(c, kDropVehicle, "malformed VehicleUpdate (truncated, trailing bytes, reserved bits or a bad rotation)");
            return;
        }
        if (c.hasVehSeq && !proto::SeqNewer(u.seq, c.vehSeq))
        {
            stats_.vehicleUpdatesStale++;
            return;
        }
        if (c.dead)
        {
            stats_.vehicleUpdatesRejected++;
            LogDrop(c, kDropVehicle, "VehicleUpdate from a dead player");
            return;
        }
        const uint64_t nowMs = net::NowMs();
        bool jumped = false;
        // v5: the motion's sample time is the sender's stamp placed on the server's clock; the store measures the
        // entry's ageMs from it and keeps the displacement budget on the receipt time `nowMs`.
        const uint64_t sampleMs = c.vehClock.Place(u.sampleMs, NowUs()) / 1000;
        const proto::VehicleResultCode r = vehicles_.Update(c.id, u, nowMs, &jumped, sampleMs);
        if (r == proto::VehicleResultCode::BadArg)
        {
            stats_.vehicleUpdatesInvalid++;
            LogDrop(c, kDropVehicle, "VehicleUpdate for vehicle " + std::to_string(u.vehId) +
                                         " outside the world box or over " + std::to_string(cfg_.vehicleMaxSpeed) + " m/s");
            return;
        }
        if (r != proto::VehicleResultCode::Ok)
        {
            stats_.vehicleUpdatesRejected++;
            LogDrop(c, kDropVehicle, "VehicleUpdate for vehicle " + std::to_string(u.vehId) + " refused (" + proto::ToString(r) +
                                         "): only the vehicle's controller (its driver, or the nearby player the server picked) may move it");
            return;
        }
        c.vehSeq = u.seq;
        c.hasVehSeq = true;
        stats_.vehicleUpdatesAccepted++;
        // The vehicle's position is judged by displacement like the player's (it was taken either way).
        if (jumped && cfg_.untrustedWindowMs != 0 && nowMs >= c.graceUntilMs)
            MarkUntrusted(c, nowMs, "VehicleUpdate for vehicle " + std::to_string(u.vehId) +
                                        " moved farther than a vehicle can in the time since its last update");
    }

    void Server::OnVehicleCmd(Conn& c, const uint8_t* data, size_t len)
    {
        proto::VehicleCmd m;
        if (!proto::Decode(data, len, m))
        {
            Refuse(c, proto::RefuseReason::Protocol, "malformed VehicleCmd (unknown op, truncated, or a Register tail that does not fit)");
            return;
        }
        using Code = proto::VehicleResultCode;
        const uint64_t nowMs = net::NowMs();
        uint16_t vehId = m.vehId;
        Code code;
        if (!c.vehCmdRate.Take(nowMs))
        {
            code = Code::RateLimited;
            // Every command over the limit also draws on the flood bucket: a client that keeps sending far
            // above its limit (a broken retry loop, or a flood) is cut off, like a chat flood.
            if (!c.vehFlood.Take(nowMs))
            {
                stats_.vehicleFloodKicks++;
                Refuse(c, proto::RefuseReason::Protocol,
                       "VehicleCmd flood: commands kept arriving over the limit of " + std::to_string((int)kVehCmdPerSec) + " per second");
                return;
            }
        }
        else
            code = RunVehicleCmd(c, m, vehId);
        stats_.vehicleCmds++;
        if (code != Code::Ok)
        {
            stats_.vehicleCmdsRefused++;
            LogDrop(c, kDropVehicle, std::string("VehicleCmd ") + proto::ToString(m.op) + " vehicle " + std::to_string(m.vehId) +
                                         " refused: " + proto::ToString(code));
        }
        // A RateLimited answer is itself traffic: at most one a second per player, the rest is counted.
        if (code == Code::RateLimited)
        {
            if (c.hasRateLimitedReply && nowMs - c.rateLimitedReplyAtMs < kRateLimitedReplyEveryMs)
            {
                stats_.vehicleRepliesSuppressed++;
                return;
            }
            c.hasRateLimitedReply = true;
            c.rateLimitedReplyAtMs = nowMs;
        }
        proto::VehicleResult res;
        res.op = m.op;
        res.code = code;
        res.vehId = vehId;
        SendTo(c, proto::kChanReliable, proto::Encode(res), true);
    }

    proto::VehicleResultCode Server::RunVehicleCmd(Conn& c, const proto::VehicleCmd& m, uint16_t& vehId)
    {
        using Code = proto::VehicleResultCode;
        const uint64_t nowMs = net::NowMs(); // the command rate limit was taken by OnVehicleCmd
        switch (m.op)
        {
        case proto::VehicleOp::Spawn:
        case proto::VehicleOp::Register:
        {
            const bool reg = m.op == proto::VehicleOp::Register;
            if (c.dead) return Code::Dead;
            if (!c.hasState) return Code::NoState;
            if (m.arg == 0 || (reg && m.seat >= proto::kVehSeats)) return Code::BadArg;
            if (!ModelAllowed(m.arg))
            {
                stats_.modelsDenied++;
                return Code::NotAllowed;
            }
            proto::Kinematics at;
            proto::Quat rot;
            if (reg)
            {
                at.x = m.x;
                at.y = m.y;
                at.z = m.z;
                rot = m.rot;
                const float dx = at.x - c.k.x, dy = at.y - c.k.y, dz = at.z - c.k.z;
                if (dx * dx + dy * dy + dz * dz > (float)cfg_.vehicleEnterDistance * (float)cfg_.vehicleEnterDistance)
                    return Code::TooFar;
            }
            else
            {
                // 4 m ahead of the requester (GTA heading: forward is (-sin h, cos h)), facing the same way.
                const double h = (double)c.k.heading * 3.14159265358979323846 / 180.0;
                at.x = (float)(c.k.x - 4.0 * std::sin(h));
                at.y = (float)(c.k.y + 4.0 * std::cos(h));
                at.z = c.k.z;
                rot = proto::QuatFromHeading(c.k.heading);
            }
            if (!proto::ValidKinematics(at)) return Code::BadArg;
            // Register is a client's claim that a car stands there: not twice in a row, and not on top of a
            // car that is networked already (which is how one car would be adopted over and over).
            if (reg && cfg_.registerSeparationMeters > 0 &&
                vehicles_.NearestVehicleDistance(at) < (float)cfg_.registerSeparationMeters)
            {
                stats_.registersRefused++;
                return Code::BadArg;
            }
            if (reg && cfg_.registerCooldownMs > 0 && c.hasRegistered && nowMs - c.lastRegisterMs < cfg_.registerCooldownMs)
            {
                stats_.registersRefused++;
                return Code::RateLimited;
            }
            if (Untrusted(c, nowMs))
            {
                stats_.untrustedRefusals++;
                return Code::RateLimited;
            }
            if (!c.spawnRate.Take(nowMs)) return Code::RateLimited;
            std::vector<uint16_t> gave;
            uint16_t id = 0;
            const Code r = vehicles_.Create(m.arg, at, rot, reg ? m.colour1 : 0, reg ? m.colour2 : 0, c.id, c.ownerKey, reg, nowMs,
                                            tick_, &id, &gave);
            for (uint16_t g : gave) VehicleDestroyed(g);
            if (r != Code::Ok) return r;
            if (reg)
            {
                c.hasRegistered = true;
                c.lastRegisterMs = nowMs;
            }
            stats_.vehiclesCreated++;
            vehId = id;
            char model[16];
            std::snprintf(model, sizeof model, "0x%08X", (unsigned)m.arg);
            Log(std::string(reg ? "vehicle register" : "vehicle spawn") + " id=" + std::to_string(id) + " model=" + model + " by " +
                c.name);
            if (reg) return vehicles_.Enter(c.id, id, m.seat, at, nowMs, tick_); // at the car it just registered
            return Code::Ok;
        }
        case proto::VehicleOp::Despawn:
        {
            const VehicleStore::Vehicle* v = vehicles_.Get(m.vehId);
            if (!v) return Code::NoVehicle;
            if (v->spawner != c.id) return Code::NotAllowed;
            vehicles_.Destroy(m.vehId, nowMs, tick_, nullptr);
            VehicleDestroyed(m.vehId);
            return Code::Ok;
        }
        case proto::VehicleOp::Enter:
            if (c.dead) return Code::Dead;
            if (!c.hasState) return Code::NoState;
            if (Untrusted(c, nowMs))
            {
                stats_.untrustedRefusals++;
                return Code::RateLimited;
            }
            return vehicles_.Enter(c.id, m.vehId, m.arg, c.k, nowMs, tick_);
        case proto::VehicleOp::Exit:
            return vehicles_.Exit(c.id, m.vehId, nowMs, tick_);
        case proto::VehicleOp::Release:
            return vehicles_.Release(c.id, m.vehId, tick_); // v5: the owner gives the driver's seat to anybody
        case proto::VehicleOp::SetLocked:
            return vehicles_.SetFlag(c.id, m.vehId, proto::kVehLocked, m.arg != 0, tick_);
        case proto::VehicleOp::SetLights:
            return vehicles_.SetFlag(c.id, m.vehId, proto::kVehLights, m.arg != 0, tick_);
        case proto::VehicleOp::SetColours:
            return vehicles_.SetColours(c.id, m.vehId, (uint8_t)(m.arg & 0xFF), (uint8_t)((m.arg >> 8) & 0xFF), tick_);
        }
        return Code::BadArg;
    }

    // ---- v4: combat ---------------------------------------------------------------------------------

    void Server::SendVitals(Conn& victim, uint16_t attacker, uint32_t weapon, uint16_t damage, bool killed, int16_t bone, uint8_t flags)
    {
        proto::Damage d;
        d.victimId = victim.id;
        d.attackerId = attacker;
        d.weapon = weapon;
        d.damage = damage;
        d.health = victim.health;
        d.armor = victim.armor;
        d.killed = killed;
        d.bone = bone;
        d.flags = (uint8_t)(flags & proto::kDmgKnown);
        const proto::Bytes b = proto::Encode(d);
        SendTo(victim, proto::kChanReliable, b, true);
        if (attacker != victim.id)
        {
            const auto it = players_.find(attacker);
            if (it != players_.end()) SendTo(*it->second, proto::kChanReliable, b, true);
        }
        // v5: everyone who can see the victim or the attacker is told as well (hit reactions, blood, the kill feed's
        // headshot mark). A heal or an armour grant (no damage) concerns the victim alone: its health rides PedState.
        if (damage == 0 && !killed) return;
        for (const auto& kv : players_)
        {
            Conn& o = *kv.second;
            if (o.id == victim.id || o.id == attacker) continue;
            const bool sees = repl_.Relevant(o.id, victim.id) || (attacker != proto::kServerId && players_.count(attacker) && repl_.Relevant(o.id, attacker));
            if (sees) SendTo(o, proto::kChanReliable, b, true);
        }
    }

    uint16_t Server::HurtPlayer(Conn& victim, uint16_t attacker, uint32_t weapon, uint16_t damage, int16_t bone, uint8_t flags)
    {
        const uint8_t armourBefore = victim.armor;
        const uint16_t applied = ApplyDamage(victim.health, victim.armor, damage);
        const bool killed = victim.health == 0;
        if (victim.armor < armourBefore) flags |= proto::kDmgArmour; // armour took (part of) the hit
        if (killed)
        {
            victim.dead = true;
            victim.deathMs = net::NowMs();
            stats_.kills++;
            // A dead player sits nowhere (a driver's vehicle parks), plays nothing and shoots nobody (a Hit
            // that was still waiting for its shot dies with it).
            const VehicleStore::Seat seat = vehicles_.Occupancy(victim.id);
            if (seat.veh != VehicleStore::kNone) vehicles_.Exit(victim.id, seat.veh, victim.deathMs, tick_);
            ClearAnim(victim, true);
            victim.deferred.clear();
        }
        SendVitals(victim, attacker, weapon, applied, killed, bone, flags);
        if (killed)
        {
            proto::Killed k;
            k.victimId = victim.id;
            k.killerId = attacker;
            k.weapon = weapon;
            k.flags = (uint8_t)(((flags & proto::kDmgHeadshot) ? proto::kKilledHeadshot : 0) | ((flags & proto::kDmgMelee) ? proto::kKilledMelee : 0));
            SendToAll(proto::Encode(k), nullptr);
            char w[16];
            std::snprintf(w, sizeof w, "0x%08X", (unsigned)weapon);
            const auto it = players_.find(attacker);
            Log("killed id=" + std::to_string(victim.id) + " " + victim.name + " by " +
                (attacker == victim.id ? std::string("self") : it != players_.end() ? it->second->name : std::string("server")) +
                " weapon=" + w);
        }
        PushShown(victim);
        return applied;
    }

    void Server::OnHit(Conn& c, const uint8_t* data, size_t len)
    {
        proto::Hit h;
        if (!proto::Decode(data, len, h))
        {
            Refuse(c, proto::RefuseReason::Protocol, "malformed Hit");
            return;
        }
        auto reject = [&](const std::string& why) {
            stats_.hitsRejected++;
            stats_.hitRefusals[(size_t)hitReason_]++;
            LogDrop(c, kDropHit, "Hit refused: " + why);
        };
        const uint64_t nowMs = net::NowMs();
        hitReason_ = HitReason::Rate;
        if (!c.hitRate.Take(nowMs)) return reject("over " + std::to_string(cfg_.hitsPerSecond) + " hits per second");
        hitReason_ = HitReason::ShooterDead;
        if (c.dead) return reject("the shooter is dead");
        hitReason_ = HitReason::ShooterNoState;
        if (!c.hasState) return reject("the shooter has sent no state yet");
        hitReason_ = HitReason::ZeroDamage;
        if (h.damage == 0) return reject("zero damage");
        std::string why;
        switch (ApplyHit(c, h, nowMs, false, why))
        {
        case HitVerdict::Accept:
            return;
        case HitVerdict::Defer:
            // Its shot (or weapon) may be in a state still on its way: the reliable Hit can overtake the
            // unreliable state in the same datagram. Held for the next states, a few at most.
            hitReason_ = HitReason::DeferFull;
            if (c.deferred.size() >= ac::kMaxDeferredHits) return reject("too many Hits waiting for their shot");
            c.deferred.push_back(Conn::DeferredHit{h, nowMs});
            stats_.hitsDeferred++;
            return;
        case HitVerdict::Reject:
            return reject(why);
        }
    }

    // The rules a Hit has to pass (docs/api/server.md "Anti-cheat rules"), then its effect. Order: the target
    // must exist and live; a self-hit (target = shooter) is environmental damage and only ever hurts the
    // sender; anything else needs a trusted shooter, in range and in view, and a weapon rule it satisfies:
    //   firearm   a shot of the shooter's own (PedState.shotSeq seen by the server within hitShotWindowMs)
    //             with the claimed weapon reported lately, not credited more times than the class allows;
    //   melee     the weapon reported lately and the target within reach (also for a firearm used as a club);
    //   run-over  the shooter drives a vehicle that moved lately and is near the target;
    //   explosive a recent launch of an explosive weapon (a grenade's damage lands seconds after the throw).
    // The damage is then clamped to the class's per-hit cap and the player's per-class damage budget.
    Server::HitVerdict Server::ApplyHit(Conn& c, const proto::Hit& h, uint64_t nowMs, bool final, std::string& why)
    {
        why.clear();
        hitReason_ = HitReason::None;
        Conn* tp = nullptr;
        const VehicleStore::Vehicle* tv = nullptr;
        proto::Kinematics tpos;
        if (h.kind == proto::HitKind::Player)
        {
            const auto it = players_.find(h.targetId);
            if (it == players_.end())
            {
                hitReason_ = HitReason::NoTarget;
                why = "no player " + std::to_string(h.targetId);
                return HitVerdict::Reject;
            }
            tp = it->second;
            if (!tp->hasState)
            {
                hitReason_ = HitReason::TargetNoState;
                why = "the target has sent no state yet";
                return HitVerdict::Reject;
            }
            if (tp->dead)
            {
                hitReason_ = HitReason::TargetDead;
                why = "the target is already dead";
                return HitVerdict::Reject;
            }
            tpos = tp->k;
        }
        else
        {
            tv = vehicles_.Get(h.targetId);
            if (!tv)
            {
                hitReason_ = HitReason::NoTarget;
                why = "no vehicle " + std::to_string(h.targetId);
                return HitVerdict::Reject;
            }
            tpos = tv->s.k;
        }

        ac::WeaponKind eff = ac::WeaponKind::Environment; // the class whose caps apply
        ac::ShotLog::Slot* credit = nullptr;              // the shot or launch this Hit is charged to
        if (tp != &c) // a self-hit is environmental damage: no trust, range or weapon rules
        {
            if (Untrusted(c, nowMs))
            {
                stats_.hitsUntrusted++;
                hitReason_ = HitReason::Untrusted;
                why = "the shooter is untrusted after a position jump";
                return HitVerdict::Reject;
            }
            const float maxD = (float)cfg_.maxHitDistance;
            if (ac::Dist(c.k, tpos) > maxD)
            {
                hitReason_ = HitReason::Range;
                why = std::string(tv ? "vehicle" : "target") + " beyond " + std::to_string(cfg_.maxHitDistance) + " m";
                return HitVerdict::Reject;
            }
            if (tp ? !repl_.Relevant(c.id, tp->id) : !vehicles_.RelevantTo(c.id, h.targetId))
            {
                hitReason_ = HitReason::NotRelevant;
                why = std::string("the ") + (tv ? "vehicle" : "target") + " is not in the shooter's view (not relevant)";
                return HitVerdict::Reject;
            }

            const ac::WeaponKind kind = ac::KindOf(h.weapon);
            if (kind == ac::WeaponKind::Environment)
            {
                stats_.hitsUnbound++;
                hitReason_ = HitReason::Environment;
                why = "an environmental weapon (fall, fire, ...) cannot hurt anyone else";
                return HitVerdict::Reject;
            }
            bool bound = false, wait = false;
            std::string first; // the first reason a path failed
            HitReason firstWhy = HitReason::NoShot;
            const auto note = [&](const std::string& r, HitReason why2) {
                if (first.empty())
                {
                    first = r;
                    firstWhy = why2;
                }
            };
            char wbuf[16];
            std::snprintf(wbuf, sizeof wbuf, "0x%08X", (unsigned)h.weapon);
            if (kind == ac::WeaponKind::RunOver)
            {
                const VehicleStore::Seat occ = vehicles_.Occupancy(c.id);
                const VehicleStore::Vehicle* dv = occ.veh != VehicleStore::kNone && occ.seat == 0 ? vehicles_.Get(occ.veh) : nullptr;
                if (!dv)
                    note("a run-over needs the shooter to drive a vehicle", HitReason::RunOver);
                else if (ac::Dist(dv->s.k, tpos) > ac::kRunOverRange)
                    note("the shooter's vehicle is farther than " + std::to_string((int)ac::kRunOverRange) + " m from the target", HitReason::RunOver);
                else if (!vehicles_.MovedRecently(occ.veh, nowMs, ac::kRunOverMovedMs))
                    note("the shooter's vehicle has not been moving", HitReason::RunOver);
                else
                {
                    bound = true;
                    eff = ac::WeaponKind::RunOver;
                }
            }
            else
            {
                const bool held = c.weapons.Held(h.weapon, nowMs, ac::kWeaponHeldMs);
                if (ac::NeedsShot(kind))
                {
                    if (cfg_.hitShotWindowMs == 0)
                    {
                        bound = true; // shot binding switched off by the operator: the damage caps still apply
                        eff = kind;
                    }
                    else if (ac::ShotLog::Slot* s = c.shots.Find(h.shotSeq, nowMs, ac::ShotWindowFor(kind, cfg_.hitShotWindowMs)))
                    {
                        if (!held)
                        {
                            note(std::string("weapon ") + wbuf + " was not reported in the shooter's states lately", HitReason::WeaponNotHeld);
                            wait = true;
                        }
                        else if (s->hits >= ac::RuleOf(kind).hitsPerShot)
                            note("that shot has hit " + std::to_string((int)s->hits) + " times already", HitReason::ShotUsed);
                        else
                        {
                            bound = true;
                            eff = kind;
                            credit = s;
                        }
                    }
                    else
                    {
                        note("no shot of the shooter's with shotSeq " + std::to_string((int)h.shotSeq) + " in the last " +
                             std::to_string(ac::ShotWindowFor(kind, cfg_.hitShotWindowMs)) + " ms", HitReason::NoShot);
                        wait = c.shots.Ahead(h.shotSeq) || !held; // its state (or the weapon change) may still be coming
                    }
                }
                const float reach = tv ? ac::kMeleeRangeVehicle : ac::kMeleeRange;
                if (bound && kind == ac::WeaponKind::Melee && ac::Dist(c.k, tpos) > reach)
                {
                    // v5: a melee Hit needs a swing (the shot slot above) AND the target within reach.
                    bound = false;
                    credit = nullptr;
                    note("a melee hit from " + std::to_string((int)ac::Dist(c.k, tpos)) + " m (reach " + std::to_string((int)reach) + " m)", HitReason::MeleeReach);
                }
                else if (!bound && kind != ac::WeaponKind::Melee && ac::NeedsShot(kind) && held && ac::Dist(c.k, tpos) <= reach)
                {
                    bound = true; // a gun used as a club: no shot of its own, but held and within reach
                    eff = ac::WeaponKind::Melee;
                    credit = nullptr;
                }
                if (!bound)
                {
                    if (ac::ShotLog::Slot* ex = c.shots.FindLaunch(nowMs, ac::kExplosiveHeldMs))
                    {
                        bound = true; // damage that lands seconds after a grenade or rocket left the shooter's hand
                        eff = ac::WeaponKind::Explosive;
                        credit = ex;
                    }
                    else if (kind == ac::WeaponKind::Explosive)
                    {
                        note("no launch of an explosive weapon in the last " + std::to_string(ac::kExplosiveHeldMs / 1000) + " s", HitReason::NoExplosive);
                        wait = true;
                    }
                }
            }
            if (!bound)
            {
                if (wait && !final) return HitVerdict::Defer;
                stats_.hitsUnbound++;
                hitReason_ = firstWhy;
                why = first.empty() ? std::string("no matching shot") : first;
                return HitVerdict::Reject;
            }
        }

        // Damage: the protocol cap, the class's per-hit cap, then what is left of the class's budget.
        const ac::WeaponRule rule = ac::RuleForKind(eff, cfg_.hitDamageScalePercent);
        const uint16_t claimed = std::min<uint16_t>(h.damage, proto::kMaxHitDamage);
        uint16_t damage = std::min<uint16_t>(claimed, rule.maxHit);
        // W8: a melee Hit is also held to its WEAPON's ceiling (fists 40, light 60, heavy 80, a gun used as a club 40), before
        // the budget and before ScaleHit, whatever the client reports (the class cap alone was 100).
        if (eff == ac::WeaponKind::Melee) damage = std::min<uint16_t>(damage, ac::MeleeNominalMax(h.weapon, cfg_.hitDamageScalePercent));
        TokenBucket& budget = c.dmgBudget[(size_t)eff];
        const double avail = budget.Available(nowMs);
        if (avail < 1.0)
        {
            stats_.hitsOverBudget++;
            hitReason_ = HitReason::OverBudget;
            why = std::string("over the damage budget of ") + rule.label + " weapons (" + std::to_string(rule.maxPerSec) + " per second)";
            return HitVerdict::Reject;
        }
        if ((double)damage > avail) damage = (uint16_t)avail;
        budget.Take(nowMs, (double)damage);
        if (damage < claimed) stats_.hitsClamped++;
        // v5: the number the victim loses is the clamped, budgeted one scaled by the operator's percents
        // (damage.h). Only the first Hit credited to a shot can score a headshot (a shotgun's pellets are not all
        // heads). A Hit with no shot slot to credit (binding switched off by hitShotWindowMs 0, a gun used as a
        // club, an explosion) is judged on its bone alone; the melee and explosive classes cannot headshot anyway.
        const bool boneCounts = !credit || credit->hits == 0;
        const HitScale scaled = ScaleHit(cfg_, eff, damage, boneCounts ? h.bone : (int16_t)-1,
                                         tp == &c ? 0.0f : ac::Dist(c.k, tpos));
        if (credit && eff == ac::WeaponKind::Melee && nowMs - credit->atMs > cfg_.hitShotWindowMs) stats_.hitsMeleeLate++;   // only the melee window let it in
        if (credit) credit->hits++;
        stats_.hitsAccepted++;
        if (eff == ac::WeaponKind::Melee)
        {
            stats_.hitsMelee++;
            if (damage < h.damage) stats_.hitsMeleeClipped++;
        }
        if (scaled.floored) stats_.hitsFloored++;
        if (tp)
            stats_.hitsDamageApplied += HurtPlayer(*tp, c.id, h.weapon, scaled.damage, h.bone, DamageFlags(scaled.headshot, eff, false));
        else
        {
            vehicles_.Damage(h.targetId, damage, tick_); // a vehicle's body health is not a player's: the clamped number, unscaled
            // v6: the Hit names a part (a tyre the shooter saw burst on its copy, a smashed window): set it for everybody. A bone
            // that is not a part (-1, a ped bone id) is ignored for a vehicle, as before.
            proto::VehiclePart part;
            unsigned slot;
            if (proto::ParseVehiclePart(h.bone, part, slot) && vehicles_.HitPart(h.targetId, part, slot, nowMs, tick_))
                stats_.vehiclePartHits++;
        }
        return HitVerdict::Accept;
    }

    // Re-judges the Hits that waited for a state: every accepted state of the shooter, and every world tick
    // (a Hit that has waited ac::kShotDeferMs is judged for the last time).
    void Server::RetryDeferredHits(Conn& c, uint64_t nowMs)
    {
        if (c.deferred.empty()) return;
        std::vector<Conn::DeferredHit> list;
        list.swap(c.deferred);
        for (const Conn::DeferredHit& d : list)
        {
            if (c.dead) continue; // died while waiting: nothing to apply
            std::string why;
            const bool final = nowMs - d.atMs >= ac::kShotDeferMs;
            switch (ApplyHit(c, d.h, nowMs, final, why))
            {
            case HitVerdict::Accept:
                break;
            case HitVerdict::Defer:
                c.deferred.push_back(d);
                break;
            case HitVerdict::Reject:
                stats_.hitsRejected++;
                stats_.hitRefusals[(size_t)hitReason_]++;
                LogDrop(c, kDropHit, "Hit refused: " + why);
                break;
            }
        }
    }

    void Server::OnRespawn(Conn& c, const uint8_t* data, size_t len)
    {
        proto::Respawn r;
        if (!proto::Decode(data, len, r))
        {
            Refuse(c, proto::RefuseReason::Protocol, "malformed Respawn");
            return;
        }
        const uint64_t nowMs = net::NowMs();
        if (!c.dead || nowMs - c.deathMs < cfg_.respawnDelayMs)
        {
            stats_.respawnsRefused++;
            LogDrop(c, kDropHit, c.dead ? "Respawn before the " + std::to_string(cfg_.respawnDelayMs) + " ms delay: ignored"
                                        : std::string("Respawn while alive: ignored"));
            return;
        }
        const SpawnPoint& sp = cfg_.spawnPoints[nextSpawnPoint_++ % cfg_.spawnPoints.size()];
        c.dead = false;
        c.health = (uint8_t)cfg_.playerHealth;
        c.armor = (uint8_t)cfg_.respawnArmour; // v5: Config::respawnArmour (0 by default)
        const VehicleStore::Seat seat = vehicles_.Occupancy(c.id);
        if (seat.veh != VehicleStore::kNone) vehicles_.Exit(c.id, seat.veh, nowMs, tick_);
        ClearAnim(c, true);
        c.k = proto::Kinematics();
        c.k.x = sp.x;
        c.k.y = sp.y;
        c.k.z = sp.z;
        c.k.heading = sp.heading;
        // The server moved the player: the next states may still come from the old place for a moment, and
        // the one at the spawn point is expected. Neither is a jump.
        c.posMs = nowMs;
        c.graceUntilMs = nowMs + ac::kRespawnGraceMs;
        PushShown(c);
        proto::RespawnOk ok;
        ok.x = sp.x;
        ok.y = sp.y;
        ok.z = sp.z;
        ok.heading = sp.heading;
        ok.health = c.health;
        ok.armor = c.armor;
        SendTo(c, proto::kChanReliable, proto::Encode(ok), true);
        stats_.respawns++;
        Log("respawn id=" + std::to_string(c.id) + " " + c.name);
    }

    // ---- v4: emotes ---------------------------------------------------------------------------------

    void Server::ClearAnim(Conn& c, bool tellOthers)
    {
        if (c.animKind == 0) return;
        c.animKind = 0;
        if (!tellOthers) return;
        proto::PeerAnimStop s;
        s.id = c.id;
        SendToAll(proto::Encode(s), &c);
        stats_.animsRelayed++;
    }

    void Server::OnAnim(Conn& c, proto::Msg type, const uint8_t* data, size_t len)
    {
        proto::PlayAnim pa;
        proto::PlayScenario ps;
        proto::StopAnim st;
        const bool ok = type == proto::Msg::PlayAnim       ? proto::Decode(data, len, pa)
                        : type == proto::Msg::PlayScenario ? proto::Decode(data, len, ps)
                                                           : proto::Decode(data, len, st);
        if (!ok)
        {
            Refuse(c, proto::RefuseReason::Protocol, std::string("malformed ") + proto::ToString(type) + " (a name over " +
                                                         std::to_string(proto::kMaxAnimName) + " bytes, truncated or trailing bytes)");
            return;
        }
        if (!c.animRate.Take(net::NowMs()))
        {
            stats_.animsRateLimited++;
            LogDrop(c, kDropAnim, std::string(proto::ToString(type)) + " over 4 per second: dropped");
            return;
        }
        // A dead player plays nothing (HurtPlayer cleared what it was playing); StopAnim stays harmless.
        if (c.dead && type != proto::Msg::StopAnim)
        {
            stats_.animsDeadDropped++;
            LogDrop(c, kDropAnim, std::string(proto::ToString(type)) + " from a dead player: dropped");
            return;
        }
        if (type == proto::Msg::PlayAnim)
        {
            if (!proto::ValidAnim(pa.anim))
            {
                stats_.animsInvalid++;
                LogDrop(c, kDropAnim, "PlayAnim rejected (a name outside [A-Za-z0-9_@/.-], a blend beyond +-1000, or a duration below -1)");
                return;
            }
            // What the others get is the allow-listed subset of the flags (no collision-off, no physics
            // override, no kill pose, ...) and a sane playback rate; a loop replayed to newcomers is the same.
            if (ac::SanitizeAnim(pa.anim)) stats_.animsSanitized++;
            proto::PeerAnim out;
            out.id = c.id;
            out.anim = pa.anim;
            SendToAll(proto::Encode(out), &c);
            stats_.animsRelayed++;
            // A loop is what a newcomer must see; a one-shot replaces whatever played before.
            c.animKind = pa.anim.durationMs == -1 ? 1 : 0;
            if (c.animKind == 1) c.anim = pa.anim;
            return;
        }
        if (type == proto::Msg::PlayScenario)
        {
            if (!proto::ValidAnimName(ps.name))
            {
                stats_.animsInvalid++;
                LogDrop(c, kDropAnim, "PlayScenario rejected (a name outside [A-Za-z0-9_@/.-])");
                return;
            }
            proto::PeerScenario out;
            out.id = c.id;
            out.name = ps.name;
            SendToAll(proto::Encode(out), &c);
            stats_.animsRelayed++;
            c.animKind = 2;
            c.scenario = ps.name;
            return;
        }
        c.animKind = 1; // so ClearAnim always relays the stop (a one-shot may be running on the others' screens)
        ClearAnim(c, true);
    }

    // ---- v4: admin --------------------------------------------------------------------------------

    bool Server::Heal(const std::string& target, std::string* err)
    {
        Conn* c = FindPlayer(target);
        if (!c)
        {
            if (err) *err = "no player \"" + target + "\"";
            return false;
        }
        if (c->dead)
        {
            if (err) *err = c->name + " is dead (it comes back with Respawn)";
            return false;
        }
        c->health = (uint8_t)cfg_.playerHealth;
        SendVitals(*c, proto::kServerId, 0, 0, false);
        PushShown(*c);
        Log("heal id=" + std::to_string(c->id) + " " + c->name);
        enet_host_flush(host_);
        return true;
    }

    bool Server::KillPlayer(const std::string& target, std::string* err)
    {
        Conn* c = FindPlayer(target);
        if (!c)
        {
            if (err) *err = "no player \"" + target + "\"";
            return false;
        }
        if (c->dead)
        {
            if (err) *err = c->name + " is already dead";
            return false;
        }
        HurtPlayer(*c, proto::kServerId, 0, (uint16_t)(c->health + c->armor));
        enet_host_flush(host_);
        return true;
    }

    bool Server::SetArmor(const std::string& target, uint8_t armor, std::string* err)
    {
        Conn* c = FindPlayer(target);
        if (!c)
        {
            if (err) *err = "no player \"" + target + "\"";
            return false;
        }
        if (c->dead)
        {
            if (err) *err = c->name + " is dead";
            return false;
        }
        c->armor = armor;
        SendVitals(*c, proto::kServerId, 0, 0, false);
        PushShown(*c);
        Log("armor id=" + std::to_string(c->id) + " " + c->name + " = " + std::to_string(armor));
        enet_host_flush(host_);
        return true;
    }

    void Server::Refuse(Conn& c, proto::RefuseReason reason, const std::string& text)
    {
        stats_.refused++;
        Log(std::string("refuse ") + proto::ToString(reason) + " " + c.addr +
            (c.st == Conn::St::Joined ? " (" + c.name + ")" : std::string()) + ": " + text);
        if (c.st == Conn::St::Joined)
            RemovePlayer(c, proto::LeaveReason::Kicked);
        proto::Refuse r;
        r.reason = reason;
        r.text = text.substr(0, proto::kMaxRefuseText);
        SendTo(c, proto::kChanReliable, proto::Encode(r), true);
        BeginClose(c, proto::kDisconnectRefused);
    }

    void Server::RefuseBanned(Conn& c, const BanEntry& ban)
    {
        stats_.refusedBanned++;
        Refuse(c, proto::RefuseReason::Banned,
               ban.reason.empty() ? std::string("You are banned from this server.")
                                  : "You are banned from this server: " + ban.reason);
    }

    void Server::RefuseAddress(const std::string& ip, const BanEntry& ban)
    {
        std::vector<Conn*> hit;
        for (auto& kv : conns_)
            if (kv.second->st != Conn::St::Closing && kv.second->addr == ip) hit.push_back(kv.second.get());
        for (Conn* c : hit) RefuseBanned(*c, ban);
        if (host_ && !hit.empty()) enet_host_flush(host_);
    }

    void Server::BeginClose(Conn& c, uint32_t disconnectData)
    {
        c.st = Conn::St::Closing;
        c.closingAtMs = net::NowMs();
        // "Later": whatever is queued (the Refuse, the kick notice) goes out and is
        // acknowledged first. A plain disconnect would discard the queue.
        enet_peer_disconnect_later(c.peer, disconnectData);
    }

    void Server::RemovePlayer(Conn& c, proto::LeaveReason why)
    {
        if (players_.erase(c.id) == 0) return;
        repl_.RemovePlayer(c.id);
        // Its empty vehicles go with it (a reconnect does not start over with fresh caps, and nothing is
        // left parked for ever); the ones somebody else sits in become orphans that go once they empty.
        std::vector<uint16_t> gone;
        size_t orphaned = 0;
        vehicles_.PlayerGone(c.id, net::NowMs(), tick_, &gone, &orphaned);
        for (uint16_t id : gone) VehicleDestroyed(id);
        stats_.vehiclesOrphaned += orphaned;
        if (orphaned) Log("vehicles orphaned by the leaving of id=" + std::to_string(c.id) + ": " + std::to_string(orphaned));
        nameKeys_.erase(c.key);
        stats_.leaves++;
        proto::PlayerLeft pl;
        pl.id = c.id;
        pl.reason = why;
        SendToAll(proto::Encode(pl), &c);
        Log("leave id=" + std::to_string(c.id) + " name=" + c.name + " reason=" + proto::ToString(why) +
            " players=" + std::to_string(players_.size()) + "/" + std::to_string(cfg_.maxPlayers));
    }

    void Server::SendTo(Conn& c, uint8_t channel, const proto::Bytes& bytes, bool reliable)
    {
        if (bytes.empty() || !c.peer) return;
        // Unreliable WorldState can exceed one MTU with many players; without this
        // flag ENet would fragment it RELIABLY and block the state channel.
        const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
        ENetPacket* pkt = enet_packet_create(bytes.data(), bytes.size(), flags);
        if (!pkt) return;
        if (enet_peer_send(c.peer, channel, pkt) < 0)
        {
            enet_packet_destroy(pkt); // enet_peer_send only takes ownership on success
            return;
        }
        metrics_.packetsOut++;
        metrics_.payloadBytesOut += bytes.size();
        c.bytesOutSec += bytes.size();
    }

    void Server::SendToAll(const proto::Bytes& bytes, const Conn* except)
    {
        for (const auto& kv : players_)
            if (kv.second != except)
                SendTo(*kv.second, proto::kChanReliable, bytes, true);
    }

    uint16_t Server::AllocId()
    {
        // Round-robin rather than lowest-free, so an id is not handed to a new
        // player the instant its previous owner leaves (stale WorldState entries
        // in flight would show up on the wrong person). Callers checked there is room.
        for (uint16_t i = 0; i < cfg_.maxPlayers; i++)
        {
            const uint16_t id = (uint16_t)((nextId_ + i) % cfg_.maxPlayers);
            if (!players_.count(id))
            {
                nextId_ = (uint16_t)((id + 1) % cfg_.maxPlayers);
                return id;
            }
        }
        return 0; // unreachable: players_.size() < maxPlayers
    }

    std::vector<PlayerInfo> Server::Players() const
    {
        std::vector<PlayerInfo> out;
        for (const auto& kv : players_)
        {
            const Conn& c = *kv.second;
            PlayerInfo p;
            p.id = c.id;
            p.name = c.name;
            p.address = c.addr;
            p.hasState = c.hasState;
            p.k = c.k;
            p.ped = Shown(c);
            p.rttMs = c.peer ? c.peer->roundTripTime : 0;
            p.health = c.health;
            p.armor = c.armor;
            p.dead = c.dead;
            const VehicleStore::Seat seat = vehicles_.Occupancy(c.id);
            p.vehicle = seat.veh;
            p.seat = seat.seat;
            p.untrusted = Untrusted(c, net::NowMs());
            out.push_back(p);
        }
        return out;
    }

    Server::Conn* Server::FindPlayer(const std::string& target)
    {
        if (AllDigits(target))
        {
            // An id wider than the wire's u16 names nobody: `(uint16_t)` used to wrap 65536 onto player 0 (a kick or a
            // ban of the wrong player). strtoul saturates on overflow, so the range test is exact on every platform.
            const unsigned long id = std::strtoul(target.c_str(), nullptr, 10);
            if (id >= proto::kMaxPlayers) return nullptr;
            auto it = players_.find((uint16_t)id);
            return it == players_.end() ? nullptr : it->second;
        }
        auto it = nameKeys_.find(proto::NameKey(target));
        if (it == nameKeys_.end()) return nullptr;
        auto p = players_.find(it->second);
        return p == players_.end() ? nullptr : p->second;
    }

    bool Server::Kick(const std::string& target, std::string* err)
    {
        Conn* c = FindPlayer(target);
        if (!c)
        {
            if (err) *err = "no player \"" + target + "\"";
            return false;
        }
        Notice(*c, "You were kicked from the server.");
        Log("kick id=" + std::to_string(c->id) + " name=" + c->name);
        RemovePlayer(*c, proto::LeaveReason::Kicked);
        BeginClose(*c, proto::kDisconnectKicked);
        enet_host_flush(host_);
        return true;
    }

    bool Server::Say(const std::string& text)
    {
        if (!proto::ValidChat(text)) return false;
        proto::ChatMessage m;
        m.fromId = proto::kServerId;
        m.text = text;
        SendToAll(proto::Encode(m), nullptr);
        stats_.chatDelivered++;
        Log("say: " + text);
        enet_host_flush(host_);
        return true;
    }

    bool Server::Ban(const std::string& targetRaw, const std::string& reasonRaw, std::string* msg)
    {
        auto fail = [&](const std::string& why) {
            if (msg) *msg = why;
            return false;
        };
        const std::string target = Trim(targetRaw);
        if (target.empty()) return fail("usage: ban <id|name> [reason]");
        BanEntry e;
        e.reason = Trim(reasonRaw);
        Conn* online = FindPlayer(target);
        if (online)
        {
            e.name = online->name;
            e.ip = online->addr;
        }
        else if (AllDigits(target))
            return fail("no player with id " + target);
        else if (proto::ValidName(target))
            e.name = target;
        else
            return fail("\"" + target + "\" is not a player name");

        std::string err, saveErr;
        if (!bans_.Add(e, &err, &saveErr))
            return fail(err);
        Log("ban " + (e.name.empty() ? std::string() : "name=" + e.name + " ") +
            (e.ip.empty() ? std::string() : "ip=" + e.ip + " ") + "reason=\"" + e.reason + "\"");
        if (!saveErr.empty()) Log("ERROR: ban list not saved: " + saveErr);
        const BanEntry* stored = bans_.Match(e.name, e.ip);
        if (!e.ip.empty() && stored) RefuseAddress(e.ip, *stored);

        if (msg)
        {
            *msg = "banned " + (e.name.empty() ? std::string() : "name " + e.name) +
                   (!e.name.empty() && !e.ip.empty() ? " and " : "") + (e.ip.empty() ? std::string() : "address " + e.ip);
            if (!saveErr.empty()) *msg += " (WARNING: not saved to " + bans_.path() + ": " + saveErr + "; the ban holds until restart)";
        }
        return true;
    }

    bool Server::BanIp(const std::string& ipRaw, const std::string& reasonRaw, std::string* msg)
    {
        auto fail = [&](const std::string& why) {
            if (msg) *msg = why;
            return false;
        };
        const std::string ip = Trim(ipRaw);
        if (!ParseIpv4(ip)) return fail("usage: banip <ipv4 address> [reason]");
        BanEntry e;
        e.ip = ip;
        e.reason = Trim(reasonRaw);
        std::string err, saveErr;
        if (!bans_.Add(e, &err, &saveErr)) return fail(err);
        Log("ban ip=" + ip + " reason=\"" + e.reason + "\"");
        if (!saveErr.empty()) Log("ERROR: ban list not saved: " + saveErr);
        if (const BanEntry* stored = bans_.FindIp(ip)) RefuseAddress(ip, *stored);
        if (msg)
        {
            *msg = "banned address " + ip;
            if (!saveErr.empty()) *msg += " (WARNING: not saved to " + bans_.path() + ": " + saveErr + "; the ban holds until restart)";
        }
        return true;
    }

    bool Server::Unban(const std::string& targetRaw, std::string* msg)
    {
        const std::string target = Trim(targetRaw);
        if (target.empty())
        {
            if (msg) *msg = "usage: unban <name|ip>";
            return false;
        }
        std::string saveErr;
        const size_t n = bans_.Remove(target, &saveErr);
        if (n == 0)
        {
            if (msg) *msg = "no ban matches \"" + target + "\"";
            return false;
        }
        Log("unban " + target + " (" + std::to_string(n) + (n == 1 ? " entry)" : " entries)"));
        if (!saveErr.empty()) Log("ERROR: ban list not saved: " + saveErr);
        if (msg)
        {
            *msg = "unbanned " + target + " (" + std::to_string(n) + (n == 1 ? " entry)" : " entries)");
            if (!saveErr.empty()) *msg += " (WARNING: not saved to " + bans_.path() + ": " + saveErr + ")";
        }
        return true;
    }

    CommandResult Server::Execute(const std::string& rawLine, std::string& output)
    {
        output.clear();
        const std::string line = Trim(rawLine);
        if (line.empty()) return CommandResult::Ok;
        const size_t sp = line.find(' ');
        std::string cmd = line.substr(0, sp);
        for (char& ch : cmd)
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        const std::string arg = sp == std::string::npos ? std::string() : Trim(line.substr(sp + 1));
        // "<first word> <rest>" for the commands that take a reason.
        const size_t sp2 = arg.find(' ');
        const std::string arg1 = arg.substr(0, sp2);
        const std::string argRest = sp2 == std::string::npos ? std::string() : Trim(arg.substr(sp2 + 1));

        if (cmd == "list")
        {
            output = "players " + std::to_string(players_.size()) + "/" + std::to_string(cfg_.maxPlayers);
            for (const PlayerInfo& p : Players())
            {
                char pos[96] = "no state yet";
                if (p.hasState)
                    std::snprintf(pos, sizeof(pos), "pos %.1f %.1f %.1f", p.k.x, p.k.y, p.k.z);
                output += "\n  " + std::to_string(p.id) + "  " + p.name + "  " + p.address + "  rtt " +
                          std::to_string(p.rttMs) + " ms  " + pos + "  hp " + std::to_string(p.health) + " armor " +
                          std::to_string(p.armor) + (p.dead ? " DEAD" : "") + (p.untrusted ? " UNTRUSTED" : "") +
                          (p.vehicle != VehicleStore::kNone
                               ? "  in vehicle " + std::to_string(p.vehicle) + " seat " + std::to_string(p.seat)
                               : std::string());
            }
            return CommandResult::Ok;
        }
        if (cmd == "kick")
        {
            if (arg.empty())
            {
                output = "usage: kick <id|name>";
                return CommandResult::Error;
            }
            std::string why;
            if (!Kick(arg, &why))
            {
                output = why;
                return CommandResult::Error;
            }
            output = "kicked " + arg;
            return CommandResult::Ok;
        }
        if (cmd == "say")
        {
            if (arg.empty())
            {
                output = "usage: say <text>";
                return CommandResult::Error;
            }
            if (!Say(arg))
            {
                output = "not sent: text must be 1.." + std::to_string(proto::kMaxChat) +
                         " bytes of UTF-8 without control characters";
                return CommandResult::Error;
            }
            output = "sent";
            return CommandResult::Ok;
        }
        if (cmd == "heal" || cmd == "kill")
        {
            if (arg.empty())
            {
                output = "usage: " + cmd + " <id|name>";
                return CommandResult::Error;
            }
            std::string why;
            if (!(cmd == "heal" ? Heal(arg, &why) : KillPlayer(arg, &why)))
            {
                output = why;
                return CommandResult::Error;
            }
            output = (cmd == "heal" ? "healed " : "killed ") + arg;
            return CommandResult::Ok;
        }
        if (cmd == "armor")
        {
            if (arg1.empty() || !AllDigits(argRest) || std::strtoul(argRest.c_str(), nullptr, 10) > 255)
            {
                output = "usage: armor <id|name> <0..255>";
                return CommandResult::Error;
            }
            std::string why;
            if (!SetArmor(arg1, (uint8_t)std::strtoul(argRest.c_str(), nullptr, 10), &why))
            {
                output = why;
                return CommandResult::Error;
            }
            output = "armor of " + arg1 + " set to " + argRest;
            return CommandResult::Ok;
        }
        if (cmd == "vehicles")
        {
            const std::vector<proto::VehicleState> all = Vehicles();
            output = "vehicles " + std::to_string(all.size()) + "/" + std::to_string(cfg_.maxVehicles);
            for (const proto::VehicleState& v : all)
            {
                const VehicleStore::Vehicle* sv = vehicles_.Get(v.id);
                char line[200];
                std::snprintf(line, sizeof line, "\n  %u  model 0x%08X  pos %.1f %.1f %.1f  body %u engine %u fuel %u  %s%s",
                              (unsigned)v.id, (unsigned)v.model, v.k.x, v.k.y, v.k.z, (unsigned)v.bodyHealth,
                              (unsigned)v.engineHealth, (unsigned)v.fuelPermille, sv && sv->adopted ? "adopted" : "spawned",
                              (v.flags & proto::kVehLocked) ? " locked" : (sv && sv->orphan ? " orphan" : ""));
                output += line;
                std::string seats;
                for (size_t i = 0; i < proto::kVehSeats; i++)
                    if (v.seats[i] != proto::kNoPlayer)
                        seats += " " + std::to_string(i) + ":" + std::to_string(v.seats[i]);
                if (!seats.empty()) output += "  seats" + seats;
            }
            return CommandResult::Ok;
        }
        if (cmd == "ban")
            return Ban(arg1, argRest, &output) ? CommandResult::Ok : CommandResult::Error;
        if (cmd == "banip")
            return BanIp(arg1, argRest, &output) ? CommandResult::Ok : CommandResult::Error;
        if (cmd == "unban")
            return Unban(arg, &output) ? CommandResult::Ok : CommandResult::Error;
        if (cmd == "bans")
        {
            output = "bans " + std::to_string(bans_.entries().size());
            for (const BanEntry& b : bans_.entries())
            {
                output += "\n  ";
                if (!b.name.empty()) output += "name " + b.name + "  ";
                if (!b.ip.empty()) output += "ip " + b.ip + "  ";
                if (!b.at.empty()) output += b.at + "  ";
                if (!b.reason.empty()) output += "reason: " + b.reason;
            }
            return CommandResult::Ok;
        }
        if (cmd == "stats")
        {
            if (!arg.empty() && arg != "json")
            {
                output = "usage: stats [json]";
                return CommandResult::Error;
            }
            // The report covers the time since the previous `stats` (or since start).
            const double secs = metrics_.wallSeconds - statsMark_.wallSeconds;
            output = arg.empty() ? FormatMetrics(metrics_, statsMark_, secs, cfg_.tickHz, players_.size())
                                 : FormatMetricsJson(metrics_, statsMark_, secs, cfg_.tickHz, players_.size());
            statsMark_ = metrics_;
            return CommandResult::Ok;
        }
        if (cmd == "help")
        {
            output = "commands: list | stats [json] | kick <id|name> | say <text> | ban <id|name> [reason] | "
                     "banip <ip> [reason] | unban <name|ip> | bans | heal <id|name> | kill <id|name> | "
                     "armor <id|name> <0..255> | vehicles | quit";
            return CommandResult::Ok;
        }
        if (cmd == "quit" || cmd == "exit" || cmd == "stop")
        {
            quitRequested_ = true;
            return CommandResult::Quit;
        }
        output = "unknown command \"" + cmd + "\" (try help)";
        return CommandResult::Error;
    }

    void Server::Stop()
    {
        if (!host_) return;
        // Off the network first: no more announces, no more admin commands, no more queries.
        announcer_.Stop();
        admin_->Stop();
        host_->intercept = nullptr;

        if (!players_.empty())
        {
            proto::ChatMessage m;
            m.fromId = proto::kServerId;
            m.text = "Server is shutting down.";
            SendToAll(proto::Encode(m), nullptr);
        }
        // Nobody is "leaving" any more: forget the roster so the disconnects that
        // follow do not broadcast PlayerLeft to people who are going too.
        players_.clear();
        nameKeys_.clear();
        for (auto& kv : conns_)
        {
            kv.second->st = Conn::St::Closing;
            kv.second->closingAtMs = net::NowMs();
            if (kv.second->peer->state == ENET_PEER_STATE_CONNECTED)
                enet_peer_disconnect_later(kv.second->peer, proto::kDisconnectShutdown);
        }
        enet_host_flush(host_);

        // Give the goodbyes a moment to be sent and acknowledged.
        const uint64_t deadline = net::NowMs() + 500;
        ENetEvent ev;
        while (host_->connectedPeers > 0 && net::NowMs() < deadline)
        {
            if (enet_host_service(host_, &ev, 20) > 0)
            {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE)
                    enet_packet_destroy(ev.packet);
                else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
                {
                    ev.peer->data = nullptr;
                    conns_.erase(ev.peer);
                }
            }
        }
        conns_.clear();
        enet_host_destroy(host_);
        host_ = nullptr;
        Log("stopped");
    }

}
