// netrig.h - the shared harness of netcode_test and server_test: Check(), the clock helpers,
// and Rig/Probe (an in-process gage::server::Server on an ephemeral loopback port plus real
// ClientSessions, all stepped from the calling thread). Test-only, header-only; each test
// executable includes it once.
#pragma once

#include "netclient/session.h"
#include "protocol/protocol.h"
#include "protocol/wire.h"
#include "server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gage;
using ET = net::Event::Type;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got = "")
{
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", got.c_str());
}

using Clock = std::chrono::steady_clock;
static int64_t MsSince(Clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// A string from raw bytes, so "\xC2\x85"-style escapes cannot swallow a following
// hex digit and the intent of every invalid sequence is explicit.
static std::string Raw(std::initializer_list<int> bytes)
{
    std::string s;
    for (int b : bytes) s.push_back((char)(uint8_t)b);
    return s;
}

static proto::Kinematics Kin(float x, float y, float z, float h = 0, float vx = 0, float vy = 0, float vz = 0)
{
    proto::Kinematics k;
    k.x = x; k.y = y; k.z = z; k.heading = h; k.vx = vx; k.vy = vy; k.vz = vz;
    return k;
}

static std::string Hex(const proto::Bytes& b)
{
    std::string out;
    char buf[4];
    for (uint8_t v : b)
    {
        std::snprintf(buf, sizeof(buf), "%02x ", v);
        out += buf;
    }
    return out;
}

struct Probe;

// An in-process server on an ephemeral loopback port plus the pump that steps it
// and every Probe from the calling thread.
struct Rig
{
    // Declared before `srv` on purpose: ~Server() logs "stopped" through the sink,
    // so the vector it appends to must outlive it.
    std::vector<std::string> logs;
    // Optional: called with every server log line on the server's own thread, inside whatever the server is doing (a test
    // can stall the server at a chosen line). Declared before `srv` for the same reason as `logs`.
    std::function<void(const std::string&)> onLog;
    std::vector<Probe*> probes;
    // Other rigs that are alive while this one is stepped. Every Step() services them too: a rig that is left unserviced
    // for about a second loses its clients to ENet's low rig timeouts (BaseConfig: 400..1000 ms), so a test that runs a
    // second Rig inside the first one's lifetime must list it here (2026-11-02, the StateTests 30 Hz block).
    std::vector<Rig*> companions;
    server::Server srv;

    static server::Config BaseConfig()
    {
        server::Config c;
        c.name = "GAGE Test";
        c.port = 0;                 // ephemeral: parallel runs cannot collide
        c.bindHost = "127.0.0.1";   // loopback only: no firewall prompt, off the network
        c.maxPlayers = 8;
        // Legacy 20 Hz (sync plan W7 moved the shipped defaults to tickHz 30, rates 30/15/10, 36000 B/s): the tests
        // below count frames and entries per second against these numbers, so they keep the numbers they were
        // written for. The shipped defaults are checked by interest_test (Base30) and netcode_test (StateTests).
        c.tickHz = 20;
        c.repl.rateNearHz = 20;
        c.repl.rateMidHz = 10;
        c.repl.rateFarHz = 5;
        c.repl.budgetBytesPerSec = 24000;
        c.helloTimeoutMs = 3000;
        // Keep the Timeout test fast: a silent client is gone within about 1.5 s.
        c.peerTimeoutLimit = 4;
        c.peerTimeoutMinMs = 400;
        c.peerTimeoutMaxMs = 1000;
        // Every probe connects from 127.0.0.1 and several tests keep six or more at once, so
        // the per-address cap is off here (its own test turns it on); no ban file on disk.
        c.maxConnectionsPerIp = 0;
        c.bansFile = "";
        c.inventoryDir = "";   // 0.6: no inventory saves from the rig; the save tests (inventory_net_tests.h) give their own scratch folder
        // The authority rules that count per address or judge invented positions would trip over a rig in
        // which every client is 127.0.0.1 and Place() teleports at will; they are off here and the tests
        // of those rules switch them on (netcode_test AuthorityNetworkTests, server_test J).
        c.connectsPerIpPerMinute = 0;
        c.vehiclesPerIp = 64;
        c.untrustedWindowMs = 0;
        c.registerCooldownMs = 0;
        c.registerSeparationMeters = 0;
        // Protocol v5 combat: the exact-number tests were written against the v4 server (100 health, the client's number
        // applied as it came). They keep that world here; the v5 tests start from server::Config{} (the shipped
        // defaults: 200 health, firearms 72 percent, melee 50, headshots 300) or ShippedCombatConfig() in netcode_test.
        c.playerHealth = 100;
        c.firearmDamagePercent = 100;
        c.meleeDamagePercent = 100;
        c.explosiveDamagePercent = 100;
        c.runOverDamagePercent = 100;
        c.headshotPercent = 100;
        c.respawnArmour = 0;
        return c;
    }

    bool Start(server::Config cfg = BaseConfig())
    {
        cfg.log = [this](const std::string& line) {
            logs.push_back(line);
            if (onLog) onLog(line);
        };
        std::string err;
        const bool ok = srv.Start(cfg, &err);
        if (!ok) std::printf("      server start failed: %s\n", err.c_str());
        return ok;
    }

    void Service();
    void Step();
    bool Until(const std::function<bool()>& pred, int timeoutMs);
    void Pump(int ms) { Until([] { return false; }, ms); }

    bool LogHas(const std::string& needle) const
    {
        for (const std::string& l : logs)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
};

// One client and everything it heard. World, Coarse and (v4) VehicleWorld events are folded into
// state (they arrive up to 20 times a second); every other event is kept in order.
//
// Protocol v2 frames are partial (only the players the server chose to send that tick), so
// the persistent view of the world is `known`: the newest entry per id, erased when the
// server says the id left the relevance range. `last` is the newest frame as it came.
struct Probe
{
    Rig& rig;
    net::ClientSession s;
    // A deque, not a vector: tests keep `const net::Event*` from First() across later Drain()s, and a push_back must not
    // move the events they point at (v8's extra AccountInfo event made bob's vector reallocate under a held PeerAnim).
    std::deque<net::Event> events;
    uint64_t worlds = 0;
    std::vector<uint32_t> worldTicks;     // the server tick of every WorldState received, in arrival order
    uint64_t coarseEvents = 0;
    proto::WorldState last;
    std::map<uint16_t, proto::WorldEntry> known; // newest full-detail entry per player
    std::map<uint16_t, proto::CoarseEntry> coarse; // newest coarse blip per player
    std::map<uint16_t, uint64_t> coarseCount;      // how many blips of each player arrived
    std::set<uint16_t> seenIds;           // every id that ever appeared in a WorldState
    std::set<uint16_t> leftIds;           // every id that ever appeared in a WorldState's `left` list
    uint16_t traceId = 0xFFFF;            // when set, record this player's x from every WorldState entry
    std::vector<float> trace;
    // v4 vehicles: the newest full state per vehicle id this client holds (erased when a frame no
    // longer lists it as present, or lists it as removed), every id ever removed, and frame counts.
    std::map<uint16_t, proto::VehicleState> vehicles;
    std::set<uint16_t> vehiclesRemoved;
    uint64_t vehicleFrames = 0;
    uint64_t vehicleFullStates = 0;
    uint64_t vehicleMotionOnly = 0;       // v5: motion-only entries laid over a held state
    uint8_t shotSeq = 0;                 // the shot counter Fire() last sent (the low 4 bits travel)

    explicit Probe(Rig& r) : rig(r) { rig.probes.push_back(this); }
    ~Probe() { rig.probes.erase(std::remove(rig.probes.begin(), rig.probes.end(), this), rig.probes.end()); }

    bool Connect(const char* name, const std::function<void(net::SessionOptions&)>& tweak = nullptr)
    {
        net::SessionOptions o;
        o.host = "127.0.0.1";
        o.port = rig.srv.port();
        o.name = name;
        o.connectTimeoutMs = 5000;
        o.closeTimeoutMs = 1000;
        if (tweak) tweak(o);
        std::string err;
        const bool ok = s.Connect(o, &err);
        if (!ok) std::printf("      connect failed: %s\n", err.c_str());
        return ok;
    }

    bool Closed() const { return s.state() == net::SessionState::Closed; }
    bool Joined() const { return s.state() == net::SessionState::Joined; }

    // Connect and wait for Welcome or the end of the connection.
    bool Join(const char* name, const std::function<void(net::SessionOptions&)>& tweak = nullptr)
    {
        if (!Connect(name, tweak)) return false;
        rig.Until([this] { return Joined() || Closed(); }, 3000);
        return Joined();
    }

    // Drive until this client's connection is over.
    bool WaitClosed(int ms = 3000)
    {
        return rig.Until([this] { return Closed(); }, ms);
    }

    void Drain()
    {
        net::Event e;
        while (s.NextEvent(e))
        {
            if (e.type == ET::Coarse)
            {
                coarseEvents++;
                for (const proto::CoarseEntry& c : e.coarse.players)
                {
                    coarse[c.id] = c;
                    coarseCount[c.id]++;
                }
                continue;
            }
            if (e.type == ET::VehicleWorld)
            {
                vehicleFrames++;
                for (uint16_t id : e.vehicles.removed)
                {
                    vehiclesRemoved.insert(id);
                    vehicles.erase(id);
                }
                for (auto it = vehicles.begin(); it != vehicles.end();)
                    it = std::binary_search(e.vehicles.present.begin(), e.vehicles.present.end(), it->first) ? std::next(it)
                                                                                                            : vehicles.erase(it);
                for (const proto::VehicleState& v : e.vehicles.vehicles)
                {
                    // v5: a motion-only entry lays its motion over the statics the Probe already holds (the session
                    // does the same when it merges two frames); with none held it is kept as it came.
                    auto held = vehicles.find(v.id);
                    if (v.motionOnly && held != vehicles.end() && !held->second.motionOnly)
                    {
                        proto::VehicleState& s = held->second;
                        s.k = v.k;
                        s.rot = v.rot;
                        s.avx = v.avx;
                        s.avy = v.avy;
                        s.avz = v.avz;
                        s.steer = v.steer;
                        s.flags = v.flags;
                        s.ageMs = v.ageMs;
                        vehicleMotionOnly++;
                        continue;
                    }
                    vehicles[v.id] = v;
                    vehicleFullStates++;
                }
                continue;
            }
            if (e.type != ET::World)
            {
                events.push_back(std::move(e));
                continue;
            }
            worlds++;
            worldTicks.push_back(e.world.tick);
            for (uint16_t id : e.world.left)
            {
                leftIds.insert(id);
                known.erase(id);
            }
            for (const proto::WorldEntry& p : e.world.players)
            {
                seenIds.insert(p.id);
                known[p.id] = p;
                if (p.id == traceId) trace.push_back(p.k.x);
            }
            last = std::move(e.world);
        }
    }

    const net::Event* First(ET t) const
    {
        for (const net::Event& e : events)
            if (e.type == t) return &e;
        return nullptr;
    }
    int Count(ET t) const
    {
        int n = 0;
        for (const net::Event& e : events)
            if (e.type == t) n++;
        return n;
    }
    bool RefusedWith(proto::RefuseReason r) const
    {
        const net::Event* e = First(ET::Refused);
        return e && e->refuse.reason == r;
    }
    bool DisconnectedWith(net::DisconnectReason r) const
    {
        const net::Event* e = First(ET::Disconnected);
        return e && e->disconnect == r;
    }
    bool SawLeft(uint16_t id, proto::LeaveReason r) const
    {
        for (const net::Event& e : events)
            if (e.type == ET::PlayerLeft && e.left.id == id && e.left.reason == r) return true;
        return false;
    }
    bool SawJoined(uint16_t id, const std::string& name) const
    {
        for (const net::Event& e : events)
            if (e.type == ET::PlayerJoined && e.joined.id == id && e.joined.name == name) return true;
        return false;
    }
    // This client's latest full-detail view of another player, or null (never seen, or it left the
    // relevance range since).
    const proto::WorldEntry* Seen(uint16_t id) const
    {
        const auto it = known.find(id);
        return it == known.end() ? nullptr : &it->second;
    }
    // The newest coarse blip of a player, or null.
    const proto::CoarseEntry* Blip(uint16_t id) const
    {
        const auto it = coarse.find(id);
        return it == coarse.end() ? nullptr : &it->second;
    }
    std::vector<std::pair<uint16_t, std::string>> ChatLog() const
    {
        std::vector<std::pair<uint16_t, std::string>> out;
        for (const net::Event& e : events)
            if (e.type == ET::Chat) out.push_back({e.chat.fromId, e.chat.text});
        return out;
    }
    std::string Describe() const
    {
        static const char* names[] = {"Welcome",      "Refused",       "PlayerJoined", "PlayerLeft", "World",
                                      "Chat",         "Disconnected",  "Coarse",       "PeerLook",   "VehicleWorld",
                                      "VehicleResult", "Damage",       "Killed",       "RespawnOk",  "PeerAnim",
                                      "PeerScenario", "PeerAnimStop", "SeatNotice", "VehicleImpact", "LinkInfo",
                                      "AccountInfo",  "AdminResult", "InvState",   "InvUsed"};
        static_assert(sizeof(names) / sizeof(names[0]) == (size_t)ET::InvUsed + 1, "a name per event type");
        std::string out;
        for (const net::Event& e : events) out += std::string(names[(int)e.type]) + " ";
        return out;
    }
    // v4: this client's view of vehicle `id`, or null.
    const proto::VehicleState* Vehicle(uint16_t id) const
    {
        const auto it = vehicles.find(id);
        return it == vehicles.end() ? nullptr : &it->second;
    }
    // v4: every VehicleResult so far, in order.
    std::vector<proto::VehicleResult> Results() const
    {
        std::vector<proto::VehicleResult> out;
        for (const net::Event& e : events)
            if (e.type == ET::VehicleResult) out.push_back(e.vehicleResult);
        return out;
    }
    std::vector<proto::Damage> Damages() const
    {
        std::vector<proto::Damage> out;
        for (const net::Event& e : events)
            if (e.type == ET::Damage) out.push_back(e.damage);
        return out;
    }

    bool SendKin(uint32_t seq, const proto::Kinematics& k)
    {
        proto::PlayerState ps;
        ps.seq = seq;
        ps.k = k;
        return s.SendRaw(proto::kChanState, proto::Encode(ps), false);
    }
    bool SendReliable(const proto::Bytes& b) { return s.SendRaw(proto::kChanReliable, b, true); }
};

void Rig::Service()
{
    srv.Poll(0);
    for (Probe* p : probes)
    {
        p->s.Poll(0);
        p->Drain();
    }
}

void Rig::Step()
{
    Service();
    for (Rig* o : companions) o->Service();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

bool Rig::Until(const std::function<bool()>& pred, int timeoutMs)
{
    const auto t0 = Clock::now();
    if (pred()) return true;
    while (MsSince(t0) < timeoutMs)
    {
        Step();
        if (pred()) return true;
    }
    return pred();
}

// Re-sends `k` every 50 ms from `sender` until `pred` holds (or the budget ends).
static bool SendUntil(Rig& rig, Probe& sender, const proto::Kinematics& k, const std::function<bool()>& pred, int timeoutMs)
{
    const auto t0 = Clock::now();
    auto next = t0;
    while (MsSince(t0) < timeoutMs)
    {
        if (Clock::now() >= next)
        {
            sender.s.SendState(k);
            next += std::chrono::milliseconds(50);
        }
        rig.Step();
        if (pred()) return true;
    }
    return pred();
}

// v4 combat: a shooter holding `weapon` pulls the trigger `shots` times at `k` (0 = only reports the weapon).
// Sends the state with the shot counter advanced until the server holds it, so a Hit sent right after
// names a shot the server has seen. Probe::shotSeq is what that Hit's shotSeq must carry.
static bool Fire(Rig& rig, Probe& p, const proto::Kinematics& k, uint32_t weapon = 0x1B06D571u, int shots = 1)
{
    const net::Event* w = p.First(ET::Welcome);
    const uint16_t id = w ? w->welcome.yourId : proto::kNoPlayer;
    p.shotSeq = (uint8_t)((p.shotSeq + shots) & 15);
    proto::PedState ped;
    ped.weapon = weapon;
    ped.shotSeq = p.shotSeq;
    const auto t0 = Clock::now();
    auto next = t0;
    while (MsSince(t0) < 2000)
    {
        if (Clock::now() >= next)
        {
            p.s.SendState(k, ped);
            next += std::chrono::milliseconds(50);
        }
        rig.Step();
        for (const server::PlayerInfo& i : rig.srv.Players())
            if (i.id == id && i.hasState && i.ped.shotSeq == p.shotSeq && i.ped.weapon == weapon && std::fabs(i.k.x - k.x) < 0.05f &&
                std::fabs(i.k.y - k.y) < 0.05f)
                return true;
    }
    return false;
}

static proto::Bytes ChatBytes(const std::string& text)
{
    proto::ChatSend c;
    c.text = text;
    return proto::Encode(c);
}
