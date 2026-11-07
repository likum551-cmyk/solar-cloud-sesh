// GAGE-MP join: the engine's session with a GAGE server, a thin adapter over
// gage::net::ClientSession (see join.h). One worker thread owns the session:
// it connects, polls, sends what the other threads queued and turns events into
// sink calls. Everything the game thread and the mp.* bridge touch is a mutex-
// guarded queue or an atomic.

#include "join.h"
#include "common/logger.h"
#include "netclient/session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

using namespace gage;

namespace {

    enum class Phase { Idle, Starting, Joined, Failed };

    // All state of the one live session. Heap-allocated and never freed: the DLL
    // can be unloaded with the worker still running at process exit, and a joinable
    // std::thread (or a mutex) destroyed under it would terminate the game.
    struct Shared
    {
        std::mutex mu;
        std::condition_variable cv;

        // Handshake outcome, written by the worker, read by Connect().
        Phase phase = Phase::Idle;
        join::Session result;

        // Outgoing, written by any thread, drained by the worker.
        bool haveState = false;
        proto::Kinematics state;
        proto::PedState statePed;
        int32_t stateSampleMs = -1;        // v5: when the game thread read `state` (set by SendState on its thread)
        bool haveLook = false;             // latest-wins, like the state
        proto::Look look;
        std::deque<std::string> chat;
        // v4
        join::VehicleMailbox vehBox;             // latest-wins PER VEHICLE, sent with the state (v5: every car we control)
        std::deque<proto::VehicleCmd> vehCmds;
        std::deque<proto::Hit> hits;
        bool respawn = false;
        struct Emote { enum Kind { Anim, Scenario, Stop } kind = Stop; proto::Anim anim; std::string name; };
        std::deque<Emote> emotes;
        std::deque<proto::AdminCmd> adminCmds;   // v8
        std::deque<proto::InvOp> invOps;         // v8 (0.6)
        bool haveInvReport = false;              // v8 (0.5.4): the newest unsent InvReport (a newer one replaces it)
        proto::InvReport invReport;

        // Sinks, set before Connect() and copied into the worker at its start.
        join::EventSink events;
        join::PeerSinks peers;

        std::thread* worker = nullptr;

        std::atomic<bool> live{false};   // Welcome received, session not over
        std::atomic<bool> stop{false};   // Disconnect() asked
        std::atomic<uint16_t> myId{0};
        std::atomic<uint32_t> rttMs{0};  // R3 T5: ClientSession::lowRttMs(), refreshed every worker pass
    };

    Shared& Sh()
    {
        static Shared* s = new Shared;
        return *s;
    }

    const size_t kMaxQueuedChat = 32;
    const size_t kMaxQueuedVehCmds = 32;
    const size_t kMaxQueuedHits = 64;
    const size_t kMaxQueuedEmotes = 16;

    const char* WhyName(net::DisconnectReason r)
    {
        switch (r)
        {
        case net::DisconnectReason::Local:            return "local";
        case net::DisconnectReason::Lost:             return "lost";
        case net::DisconnectReason::Kicked:           return "kicked";
        case net::DisconnectReason::Refused:          return "refused";
        case net::DisconnectReason::Shutdown:         return "shutdown";
        case net::DisconnectReason::ConnectFailed:    return "connect failed";
        case net::DisconnectReason::HandshakeTimeout: return "handshake timeout";
        }
        return "unknown";
    }

    void Fire(const join::EventSink& sink, const char* event, const std::string& payload)
    {
        if (sink)
            sink(event, payload.c_str());
    }

    // Publishes a failed handshake to the Connect() that is waiting for it.
    void FailHandshake(const std::string& why, bool refused, const proto::Refuse* refusal = nullptr)
    {
        Shared& S = Sh();
        {
            std::lock_guard<std::mutex> lk(S.mu);
            S.result.connected = false;
            S.result.refused = refused;
            S.result.error = Widen(why);
            S.result.refuseReason = refusal ? refusal->reason : proto::RefuseReason::Protocol;
            S.result.refuseText = refusal ? refusal->text : std::string();
            S.phase = Phase::Failed;
        }
        S.cv.notify_all();
    }

    void Worker(net::SessionOptions opts, join::EventSink events, join::PeerSinks peers)
    {
        Shared& S = Sh();
        net::ClientSession session;
        std::string err;
        if (!session.Connect(opts, &err))
        {
            GLOGW("join: cannot start the connection to %s:%u: %s", opts.host.c_str(), (unsigned)opts.port, err.c_str());
            FailHandshake("cannot connect: " + err, false);
            return;
        }

        const std::string where = opts.host + ":" + std::to_string(opts.port);
        bool joined = false, refused = false, closing = false;
        proto::Refuse refusal;   // the server's Refuse, kept whole for Session::refuseReason / refuseText
        std::string failure;
        uint32_t stateRejected = 0, vehRejected = 0;

        // Heartbeat and stall check: what this thread did in the last 10 s, and a
        // warning when a game callback (onWorld & co., which take the engine's
        // locks) held it long enough to starve the connection.
        using Clock = std::chrono::steady_clock;
        Clock::time_point beatAt = Clock::now();
        uint32_t beatStates = 0, beatWorlds = 0, beatKeepAlives = 0;
        // The longest gap between two passes of this loop in the beat window: it should be a few ms (the 2 ms
        // poll plus the callbacks). A big one is time this thread did not spend receiving or sending.
        Clock::time_point passAt = Clock::now();
        long long beatLoopGapMaxMs = 0;
        // The latest state that went out and when: the keep-alive below repeats it when the game goes quiet.
        proto::Kinematics lastK;
        proto::PedState lastPed;
        int32_t lastSampleMs = -1;      // the stamp that state went out with (the keep-alive repeats it: the same sample)
        bool haveLast = false;
        Clock::time_point lastStateAt = Clock::now();
        const std::chrono::milliseconds kKeepAliveAfter(100);
        uint64_t beatIn = 0, beatOut = 0, seenSocketErrors = 0; // wire counters at the last beat
        auto timed = [&](const char* what, auto&& call) {
            const Clock::time_point t0 = Clock::now();
            call();
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            if (ms >= 250)
                GLOGW("join: the game held the network thread for %lld ms (%s callback)", ms, what);
        };

        for (bool ended = false; !ended;)
        {
            {
                const Clock::time_point now = Clock::now();
                const long long gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - passAt).count();
                if (gapMs > beatLoopGapMaxMs) beatLoopGapMaxMs = gapMs;
                passAt = now;
            }
            if (joined)
                Sh().rttMs.store(session.lowRttMs(), std::memory_order_relaxed);
            if (joined && Clock::now() - beatAt >= std::chrono::seconds(10))
            {
                // Datagrams both ways tell a dead path (in == 0) from a socket that cannot send.
                const net::ClientSession::Counters c = session.counters();
                const std::string note = peers.beatNote ? peers.beatNote() : std::string();
                // Sync pipeline review: ENet's packet throttle deletes unreliable packets (our state channel) when it is below its scale; the
                // server configures deceleration 0 so it must read full. loss is ENet's smoothed loss of RELIABLE packets, stale / rejected the
                // world frames the decoder dropped (reordered or late / unresolvable) over the whole session.
                const net::ClientSession::LinkInfo li = session.link();
                GLOGI("join: network thread alive, %u states sent (%u of them keep-alives) and %u world updates in the last 10 s; "
                      "udp %llu in / %llu out, rtt %u ms, socket errors %llu, loop gap max %lld ms%s%s; "
                      "enet throttle %u/%u (deceleration %u), reliable loss %.1f%%, rtt deviation %u ms, world frames dropped by the decoder: %llu stale %llu rejected",
                      (unsigned)beatStates, (unsigned)beatKeepAlives, (unsigned)beatWorlds,
                      (unsigned long long)(c.wireDatagramsIn - beatIn), (unsigned long long)(c.wireDatagramsOut - beatOut),
                      (unsigned)session.rttMs(), (unsigned long long)c.socketErrors, beatLoopGapMaxMs,
                      note.empty() ? "" : "; ", note.c_str(),
                      (unsigned)li.throttle, (unsigned)li.throttleScale, (unsigned)li.throttleDeceleration, li.lossPercent, (unsigned)li.rttVarMs,
                      (unsigned long long)c.worldStale, (unsigned long long)c.worldRejected);
                beatIn = c.wireDatagramsIn;
                beatOut = c.wireDatagramsOut;
                beatAt = Clock::now();
                beatStates = beatWorlds = beatKeepAlives = 0;
                beatLoopGapMaxMs = 0;
            }
            // Outgoing first, so what the game queued goes out in this pass.
            if (joined && !closing)
            {
                bool haveState = false, haveLook = false, respawn = false;
                proto::Kinematics k;
                proto::PedState ped;
                int32_t sampleMs = -1;
                proto::Look look;
                std::vector<join::VehicleMailbox::Item> vehs;
                std::deque<std::string> chat;
                std::deque<proto::VehicleCmd> vehCmds;
                std::deque<proto::Hit> hits;
                std::deque<Shared::Emote> emotes;
                std::deque<proto::AdminCmd> adminCmds;
                std::deque<proto::InvOp> invOps;
                bool haveInvReport = false;
                proto::InvReport invReport;
                {
                    std::lock_guard<std::mutex> lk(S.mu);
                    adminCmds.swap(S.adminCmds);
                    invOps.swap(S.invOps);
                    haveInvReport = S.haveInvReport;
                    if (haveInvReport)
                        invReport = std::move(S.invReport);
                    S.haveInvReport = false;
                    haveState = S.haveState;
                    k = S.state;
                    ped = S.statePed;
                    sampleMs = S.stateSampleMs;
                    S.haveState = false;
                    haveLook = S.haveLook;
                    look = S.look;
                    S.haveLook = false;
                    chat.swap(S.chat);
                    S.vehBox.Take(vehs);
                    vehCmds.swap(S.vehCmds);
                    hits.swap(S.hits);
                    respawn = S.respawn;
                    S.respawn = false;
                    emotes.swap(S.emotes);
                }
                // v8: admin commands before the vehicle commands they may lead to.
                for (const proto::AdminCmd& c : adminCmds)
                    if (!session.SendAdminCmd(c))
                        GLOGW("join: admin command /%s (seq %u) not sent", proto::ToString(c.op), (unsigned)c.seq);
                for (const proto::InvOp& o : invOps)
                    if (!session.SendInvOp(o))
                        GLOGW("join: inventory op seq %u not sent", (unsigned)o.seq);
                if (haveInvReport && !session.SendInvReport(invReport))
                    GLOGW("join: inventory report (rev %u) not sent", (unsigned)invReport.rev);
                // v4, reliable ones first (they are ordered on their own channel anyway).
                for (const proto::VehicleCmd& c : vehCmds)
                    if (!session.SendVehicleCmd(c))
                        GLOGW("join: vehicle command (op %u, vehicle %u) not sent", (unsigned)c.op, (unsigned)c.vehId);
                for (const proto::Hit& h : hits)
                    if (!session.SendHit(h))
                        GLOGW("join: hit (target %u, damage %u) not sent", (unsigned)h.targetId, (unsigned)h.damage);
                if (respawn && !session.SendRespawn())
                    GLOGW("join: respawn request not sent");
                for (const Shared::Emote& m : emotes)
                {
                    const bool ok = m.kind == Shared::Emote::Anim       ? session.SendAnim(m.anim)
                                    : m.kind == Shared::Emote::Scenario ? session.SendScenario(m.name)
                                                                        : session.SendStopAnim();
                    if (!ok)
                        GLOGW("join: emote (kind %d) not sent", (int)m.kind);
                }
                for (const std::string& line : chat)
                    if (!session.SendChat(line))
                        GLOGW("join: chat line not sent (%zu bytes)", line.size());
                // Reliable; the session itself skips a look equal to the last one sent.
                if (haveLook && !session.SendLook(look))
                    GLOGW("join: look not sent (model 0x%08x; the session refused it)", (unsigned)look.model);
                if (haveState)
                {
                    // The frame's own sample time (stamped by SendState on the game thread), not the moment of this
                    // send: the worker's wait and the frame quantisation are what the server's clock estimate removes.
                    if (session.SendState(k, ped, sampleMs))
                    {
                        ++beatStates;
                        lastK = k;
                        lastPed = ped;
                        lastSampleMs = sampleMs;
                        haveLast = true;
                        lastStateAt = Clock::now();
                    }
                    else if ((stateRejected++ % 300) == 0)
                        GLOGW("join: state not sent (%u so far; the session refused the values)", (unsigned)stateRejected);
                }
                else if (haveLast && Clock::now() - lastStateAt >= kKeepAliveAfter)
                {
                    // Keep-alive ack. The receipt for the world frames decoded so far rides on PlayerState and on
                    // nothing else, so a game thread that posts no state (a loading screen, a hitch, the menu)
                    // silences it, and after 32 server ticks the server has no baseline that fits and sends every
                    // entry about the other players in full. Repeat the latest state: the server reads it as the
                    // same sample again (inside the idle dead-band, so it changes nothing it replicates). It carries
                    // the same sample stamp as the state it repeats: it IS that sample, the server's age keeps growing.
                    if (session.SendState(lastK, lastPed, lastSampleMs))
                    {
                        ++beatStates;
                        ++beatKeepAlives;
                    }
                    lastStateAt = Clock::now(); // a refused one is not retried every 2 ms
                }
                // "Call it in the same send tick as SendState" (session.h).
                for (const join::VehicleMailbox::Item& it : vehs)
                    if (!session.SendVehicleState(it.u, it.sampleMs) && (vehRejected++ % 300) == 0)
                        GLOGW("join: vehicle state not sent (%u so far; the session refused the values)", (unsigned)vehRejected);
            }
            if (S.stop.load() && !closing)
            {
                closing = true;
                session.Disconnect(); // bounded by SessionOptions::closeTimeoutMs, then Closed
            }

            // The game thread hands over one state per frame and this loop sends it at the top of the
            // next pass: the wait is also the uplink delay (0 .. the timeout). 2 ms while joined keeps it
            // under a quarter of a 100 fps frame; 10 ms is plenty before that (connecting, closing).
            session.Poll(joined ? 2 : 10);
            {
                const net::ClientSession::Counters c = session.counters();
                if (c.socketErrors > seenSocketErrors)
                {
                    // Logged on the first error and then once per 100, not per poll.
                    if (seenSocketErrors == 0 || c.socketErrors / 100 > seenSocketErrors / 100)
                        GLOGW("join: UDP socket error %d (%llu so far): the connection cannot carry traffic",
                              c.lastSocketError, (unsigned long long)c.socketErrors);
                    seenSocketErrors = c.socketErrors;
                }
            }

            net::Event e;
            while (!ended && session.NextEvent(e))
            {
                switch (e.type)
                {
                case net::Event::Type::Welcome:
                {
                    joined = true;
                    S.myId.store(e.welcome.yourId);
                    // The engine sets up its remote-player smoother for this rate before any
                    // WorldState of the session reaches it (this thread delivers both, in order).
                    if (peers.onWelcome)
                        timed("welcome", [&] { peers.onWelcome(e.welcome.yourId, e.welcome.tickHz, e.welcome.playerHealth); });
                    // The UI hears "connected" before Connect() reports success, so a caller that
                    // returns from Connect() can rely on the event having been delivered.
                    Fire(events, "connected", e.welcome.serverName);
                    {
                        std::lock_guard<std::mutex> lk(S.mu);
                        S.result.connected = true;
                        S.result.refused = false;
                        S.result.myId = e.welcome.yourId;
                        S.result.tickHz = e.welcome.tickHz;
                        S.result.playerHealth = e.welcome.playerHealth;
                        S.result.serverName = e.welcome.serverName;
                        S.result.error.clear();
                        S.phase = Phase::Joined;
                    }
                    S.live.store(true);
                    S.cv.notify_all();
                    GLOGI("join: joined %s as id %u (server '%s', %u Hz)", where.c_str(), (unsigned)e.welcome.yourId,
                          e.welcome.serverName.c_str(), (unsigned)e.welcome.tickHz);
                    break;
                }
                case net::Event::Type::Refused:
                    refused = true;
                    refusal = e.refuse;
                    failure = std::string("the server refused us (") + proto::ToString(e.refuse.reason) + "): " + e.refuse.text;
                    GLOGW("join: %s", failure.c_str());
                    break;
                case net::Event::Type::PlayerJoined:
                    GLOGI("join: player %u '%s' joined", (unsigned)e.joined.id, e.joined.name.c_str());
                    if (peers.onJoined)
                        timed("playerJoined", [&] { peers.onJoined(e.joined.id, e.joined.name); });
                    Fire(events, "playerJoined",
                         "{\"id\":" + std::to_string(e.joined.id) + ",\"name\":\"" + join::JsonEscape(e.joined.name) + "\"}");
                    break;
                case net::Event::Type::PlayerLeft:
                    GLOGI("join: player %u left (%s)", (unsigned)e.left.id, proto::ToString(e.left.reason));
                    if (peers.onLeft)
                        timed("playerLeft", [&] { peers.onLeft(e.left.id); });
                    Fire(events, "playerLeft", "{\"id\":" + std::to_string(e.left.id) + "}");
                    break;
                case net::Event::Type::World:
                    ++beatWorlds;
                    if (peers.onWorld)
                        timed("world", [&] { peers.onWorld(e.world); });
                    break;
                case net::Event::Type::Coarse:
                    if (peers.onCoarse)
                        timed("coarse", [&] { peers.onCoarse(e.coarse); }); // far-player map blips
                    break;
                case net::Event::Type::PeerLook:
                    if (peers.onLook)
                        timed("look", [&] { peers.onLook(e.look); });
                    break;
                case net::Event::Type::Chat:
                    if (peers.onChat)
                        timed("chat", [&] { peers.onChat(e.chat.fromId, e.chat.text); });
                    break;
                case net::Event::Type::VehicleWorld:
                    if (peers.onVehicles)
                        timed("vehicles", [&] { peers.onVehicles(e.vehicles); });
                    break;
                case net::Event::Type::VehicleResult:
                    if (peers.onVehicleResult)
                        timed("vehicleResult", [&] { peers.onVehicleResult(e.vehicleResult); });
                    break;
                case net::Event::Type::Damage:
                    if (peers.onDamage)
                        timed("damage", [&] { peers.onDamage(e.damage); });
                    break;
                case net::Event::Type::Killed:
                    if (peers.onKilled)
                        timed("killed", [&] { peers.onKilled(e.killed); });
                    break;
                case net::Event::Type::RespawnOk:
                    if (peers.onRespawnOk)
                        timed("respawnOk", [&] { peers.onRespawnOk(e.respawn); });
                    break;
                case net::Event::Type::PeerAnim:
                    if (peers.onPeerAnim)
                        timed("peerAnim", [&] { peers.onPeerAnim(e.anim); });
                    break;
                case net::Event::Type::PeerScenario:
                    if (peers.onPeerScenario)
                        timed("peerScenario", [&] { peers.onPeerScenario(e.scenario); });
                    break;
                case net::Event::Type::SeatNotice:
                    if (peers.onSeatNotice)
                        timed("seatNotice", [&] { peers.onSeatNotice(e.seatNotice); });
                    break;
                case net::Event::Type::PeerAnimStop:
                    if (peers.onPeerAnimStop)
                        timed("peerAnimStop", [&] { peers.onPeerAnimStop(e.animStop); });
                    break;
                case net::Event::Type::VehicleImpact:
                    if (peers.onVehicleImpact)
                        timed("vehicleImpact", [&] { peers.onVehicleImpact(e.impact); });
                    break;
                case net::Event::Type::LinkInfo:
                    if (peers.onLinkInfo)
                        timed("linkInfo", [&] { peers.onLinkInfo(e.linkInfo); });
                    break;
                case net::Event::Type::AccountInfo:
                    GLOGI("join: account: %s%s%s", (e.account.flags & proto::kAcctVerified) ? "verified as " : "guest (no verified account)",
                          (e.account.flags & proto::kAcctVerified) ? e.account.username.c_str() : "",
                          (e.account.flags & proto::kAcctAdmin) ? ", admin" : (e.account.flags & proto::kAcctCommands) ? ", admin commands allowed" : "");
                    if (peers.onAccount)
                        timed("account", [&] { peers.onAccount(e.account); });
                    break;
                case net::Event::Type::AdminResult:
                    if (peers.onAdminResult)
                        timed("adminResult", [&] { peers.onAdminResult(e.adminResult); });
                    break;
                case net::Event::Type::InvState:
                    if (peers.onInvState)
                        timed("invState", [&] { peers.onInvState(e.invState); });
                    break;
                case net::Event::Type::InvUsed:
                    if (peers.onInvUsed)
                        timed("invUsed", [&] { peers.onInvUsed(e.invUsed); });
                    break;
                case net::Event::Type::InvSnap:
                    if (peers.onInvSnap)
                        timed("invSnap", [&] { peers.onInvSnap(e.invSnap); });
                    break;
                case net::Event::Type::InvNearby:
                    if (peers.onInvNearby)
                        timed("invNearby", [&] { peers.onInvNearby(e.invNearby); });
                    break;
                case net::Event::Type::WorldClock:
                    if (peers.onWorldClock)
                        timed("worldClock", [&] { peers.onWorldClock(e.worldClock, session.rttMs()); });
                    break;
                case net::Event::Type::Disconnected:
                    ended = true;
                    if (!joined && failure.empty())
                        failure = e.disconnect == net::DisconnectReason::ConnectFailed
                                      ? "no answer from " + where + " (is the server running and its UDP port open?)"
                                  : e.disconnect == net::DisconnectReason::HandshakeTimeout
                                      ? where + " answered but sent no Welcome in time (is it a Solar RP server?)"
                                      : std::string("disconnected (") + WhyName(e.disconnect) + ")";
                    if (joined)
                    {
                        const net::ClientSession::Counters c = session.counters();
                        GLOGI("join: session with %s ended (%s); udp %llu in / %llu out since the last beat, "
                              "socket errors %llu (last %d)",
                              where.c_str(), WhyName(e.disconnect), (unsigned long long)(c.wireDatagramsIn - beatIn),
                              (unsigned long long)(c.wireDatagramsOut - beatOut), (unsigned long long)c.socketErrors,
                              c.lastSocketError);
                    }
                    else
                        GLOGW("join: %s", failure.c_str());
                    if (joined)
                    {
                        S.live.store(false);
                        if (peers.onEnded)
                            peers.onEnded();
                        Fire(events, "disconnected", WhyName(e.disconnect));
                    }
                    break;
                }
            }
        }

        S.live.store(false);
        if (!joined)
            FailHandshake(failure, refused, refused ? &refusal : nullptr);
    }

    bool IsAsciiNameChar(unsigned char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '-';
    }

}

namespace gage::join {

    void SetEventSink(EventSink sink)
    {
        Shared& S = Sh();
        std::lock_guard<std::mutex> lk(S.mu);
        S.events = std::move(sink);
    }

    void SetPeerSinks(PeerSinks sinks)
    {
        Shared& S = Sh();
        std::lock_guard<std::mutex> lk(S.mu);
        S.peers = std::move(sinks);
    }

    bool IsConnected() { return Sh().live.load(); }

    uint16_t MyId() { return Sh().myId.load(); }
    uint32_t RttMs() { return Sh().rttMs.load(std::memory_order_relaxed); }

    bool VehicleMailbox::Put(const proto::VehicleUpdate& u, int32_t sampleMs)
    {
        if (u.vehId >= proto::kMaxVehicles)
            return false;
        Item& it = items_[u.vehId];
        it.u = u;
        it.sampleMs = sampleMs;
        return true;
    }

    void VehicleMailbox::Take(std::vector<Item>& out)
    {
        out.clear();
        out.reserve(items_.size());
        for (auto& kv : items_)                 // std::map: ascending vehicle id
            out.push_back(std::move(kv.second));
        items_.clear();
    }

    // The sample stamp of "now" on the calling thread: whole milliseconds of the steady clock. The session keeps
    // the low 16 bits (the wire's modulo 65536); a stamp is only ever compared with the stamps of the same stream.
    static int32_t StampNowMs()
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return (int32_t)(ms & 0xFFFF);
    }

    bool SendState(const proto::Kinematics& k, const proto::PedState& ped, int32_t sampleMs)
    {
        Shared& S = Sh();
        if (!S.live.load() || !proto::ValidKinematics(k) || !proto::ValidPedState(ped))
            return false;
        if (sampleMs < 0)
            sampleMs = StampNowMs();           // stamped here, on the game thread, not when the worker gets to it
        std::lock_guard<std::mutex> lk(S.mu);
        S.state = k;
        S.statePed = ped;
        S.stateSampleMs = sampleMs;
        S.haveState = true;
        return true;
    }

    bool SendLook(const proto::Look& look)
    {
        Shared& S = Sh();
        if (!S.live.load() || !proto::ValidLook(look))
            return false;
        std::lock_guard<std::mutex> lk(S.mu);
        S.look = look;
        S.haveLook = true;
        return true;
    }

    bool SendChat(const std::string& text)
    {
        Shared& S = Sh();
        if (!S.live.load() || !proto::ValidChat(text))
            return false;
        std::lock_guard<std::mutex> lk(S.mu);
        if (S.chat.size() >= kMaxQueuedChat)
            return false;
        S.chat.push_back(text);
        return true;
    }

    bool SendVehicleState(const proto::VehicleUpdate& u, int32_t sampleMs)
    {
        Shared& S = Sh();
        if (!S.live.load() || u.vehId >= proto::kMaxVehicles || !proto::ValidVehicleUpdate(u))
            return false;
        if (sampleMs < 0)
            sampleMs = StampNowMs();
        std::lock_guard<std::mutex> lk(S.mu);
        return S.vehBox.Put(u, sampleMs);
    }

    bool SendVehicleCmd(const proto::VehicleCmd& c)
    {
        Shared& S = Sh();
        if (!S.live.load() || (uint8_t)c.op < (uint8_t)proto::VehicleOp::Spawn || (uint8_t)c.op > proto::kVehicleOpLast)
            return false;
        std::lock_guard<std::mutex> lk(S.mu);
        if (S.vehCmds.size() >= kMaxQueuedVehCmds)
            return false;
        S.vehCmds.push_back(c);
        return true;
    }

    bool SendAdminCmd(const proto::AdminCmd& c)
    {
        Shared& S = Sh();
        if (!S.live.load() || proto::Encode(c).empty())
            return false;
        std::lock_guard<std::mutex> lk(S.mu);
        if (S.adminCmds.size() >= 16)
            return false;
        S.adminCmds.push_back(c);
        return true;
    }

    bool SendInvOp(const proto::InvOp& o)
    {
        Shared& S = Sh();
        if (!S.live.load() || proto::Encode(o).empty())
            return false;
        std::lock_guard<std::mutex> lk(S.mu);
        if (S.invOps.size() >= 64)
            return false;
        S.invOps.push_back(o);
        return true;
    }

    bool SendInvReport(const proto::InvReport& r)
    {
        Shared& S = Sh();
        if (!S.live.load() || proto::Encode(r).empty())
            return false;
        std::lock_guard<std::mutex> lk(S.mu);
        S.invReport = r;
        S.haveInvReport = true;
        return true;
    }

    bool SendHit(const proto::Hit& h)
    {
        Shared& S = Sh();
        if (!S.live.load() || h.damage == 0 || (h.kind != proto::HitKind::Player && h.kind != proto::HitKind::Vehicle))
            return false;
        std::lock_guard<std::mutex> lk(S.mu);
        if (S.hits.size() >= kMaxQueuedHits)
            return false;
        S.hits.push_back(h);
        return true;
    }

    bool SendRespawn()
    {
        Shared& S = Sh();
        if (!S.live.load())
            return false;
        std::lock_guard<std::mutex> lk(S.mu);
        S.respawn = true;
        return true;
    }

    namespace {
        bool QueueEmote(Shared::Emote&& m)
        {
            Shared& S = Sh();
            std::lock_guard<std::mutex> lk(S.mu);
            if (S.emotes.size() >= kMaxQueuedEmotes)
                return false;
            S.emotes.push_back(std::move(m));
            return true;
        }
    }

    bool SendAnim(const proto::Anim& a)
    {
        if (!Sh().live.load() || !proto::ValidAnim(a))
            return false;
        Shared::Emote m;
        m.kind = Shared::Emote::Anim;
        m.anim = a;
        return QueueEmote(std::move(m));
    }

    bool SendScenario(const std::string& name)
    {
        if (!Sh().live.load() || !proto::ValidAnimName(name))
            return false;
        Shared::Emote m;
        m.kind = Shared::Emote::Scenario;
        m.name = name;
        return QueueEmote(std::move(m));
    }

    bool SendStopAnim()
    {
        if (!Sh().live.load())
            return false;
        Shared::Emote m;
        m.kind = Shared::Emote::Stop;
        return QueueEmote(std::move(m));
    }

    std::string SanitizeName(const std::wstring& wanted)
    {
        const std::string utf8 = Narrow(wanted);
        std::string out;
        for (unsigned char c : utf8)
        {
            if ((c & 0xC0) == 0x80)
                continue; // continuation byte: its lead byte already stood for the whole character
            if (IsAsciiNameChar(c))
                out.push_back((char)c);
            else if (c == ' ' && !out.empty() && out.back() != ' ')
                out.push_back(' ');
            else if (c != ' ')
                out.push_back('_');
        }
        if (out.size() > proto::kMaxName)
            out.resize(proto::kMaxName);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        if (out.empty() || !proto::ValidName(out))
            return "player";
        return out;
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char u[8];
                    snprintf(u, sizeof(u), "\\u%04x", (unsigned)c);
                    out += u;
                }
                else
                    out.push_back((char)c);
            }
        }
        return out;
    }

    void Disconnect()
    {
        Shared& S = Sh();
        std::thread* t = nullptr;
        {
            std::lock_guard<std::mutex> lk(S.mu);
            t = S.worker;
            S.worker = nullptr;
        }
        if (!t)
            return;
        S.stop.store(true);
        if (t->joinable())
            t->join();
        delete t;
        S.live.store(false);
    }

    bool Connect(const Config& cfg, int timeoutMs, Session& session)
    {
        Shared& S = Sh();
        Disconnect(); // replaces any session, live or already over (joins its worker)
        session = Session();

        if (cfg.host.empty())
        {
            session.error = L"no server host";
            return false;
        }
        if (cfg.port < 1 || cfg.port > 65535)
        {
            session.error = L"bad server port " + std::to_wstring(cfg.port);
            return false;
        }
        if (timeoutMs < 500)
            timeoutMs = 500;
        if (timeoutMs > 60000)
            timeoutMs = 60000;

        net::SessionOptions o;
        o.host = Narrow(cfg.host);
        o.port = (uint16_t)cfg.port;
        o.name = SanitizeName(cfg.player);
        o.connectTimeoutMs = (uint32_t)timeoutMs;
        o.joinToken = cfg.joinToken;
        if (Narrow(cfg.player) != o.name)
            GLOGW("join: player name '%s' is not allowed by the protocol, joining as '%s'",
                  Narrow(cfg.player).c_str(), o.name.c_str());
        GLOGI("join: joining %s:%d as '%s' (protocol %u, %s)", o.host.c_str(), cfg.port, o.name.c_str(),
              (unsigned)proto::kVersion,
              cfg.joinToken.empty() ? "no join token: a guest unless the server requires an account"
              : proto::ValidJoinToken(cfg.joinToken) ? "with a join token"
                                                     : "the join token is malformed and is not sent");

        EventSink events;
        PeerSinks peers;
        {
            std::lock_guard<std::mutex> lk(S.mu);
            S.phase = Phase::Starting;
            S.result = Session();
            S.haveState = false;
            S.haveLook = false;
            S.chat.clear();
            S.vehBox.Clear();
            S.vehCmds.clear();
            S.hits.clear();
            S.respawn = false;
            S.emotes.clear();
            S.adminCmds.clear();
            S.invOps.clear();
            S.haveInvReport = false;
            S.stop.store(false);
            S.live.store(false);
            S.myId.store(0);
            S.rttMs.store(0);
            events = S.events;
            peers = S.peers;
            S.worker = new std::thread(Worker, o, events, peers);
        }

        // The session has its own deadline (connectTimeoutMs); the extra wait is
        // only a guard so this call can never hang on a worker that did not report.
        std::unique_lock<std::mutex> lk(S.mu);
        const bool reported = S.cv.wait_for(lk, std::chrono::milliseconds(timeoutMs + 3000),
                                            [&] { return S.phase != Phase::Starting; });
        if (reported && S.phase == Phase::Joined)
        {
            session = S.result;
            return true;
        }
        const Session failed = S.result;
        lk.unlock();

        Disconnect(); // the worker ends on its own after a failed handshake; this joins it
        session = failed;
        if (!reported)
            session.error = L"no answer within " + std::to_wstring(timeoutMs) + L" ms";
        return false;
    }

}
