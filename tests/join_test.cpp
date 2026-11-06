// join_test — the engine's session adapter (apps/engine/join.cpp) against a real
// gage::server::Server over ENet on 127.0.0.1: connect, refusal, peer events,
// world positions, chat in both directions, state upstream, kick, reconnect.
// The server runs on its own thread (join::Connect blocks its caller, as the
// engine's connect thread does); a second player ("Bob") is a plain
// net::ClientSession polled from the test thread. Build target: join_test
// (selftest/, never shipped). Exit 0 = pass.

#include "../apps/engine/join.h"
#include "netclient/session.h"
#include "server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace gage;
using Clock = std::chrono::steady_clock;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got = "") {
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", got.c_str());
}

static long MsSince(Clock::time_point t0) {
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// ---- the server, on its own thread -------------------------------------------
class ServerRunner {
public:
    server::Server srv;

    bool Start(const std::function<void(server::Config&)>& tweak = nullptr) {
        server::Config c;
        c.name = "GAGE Join Test";
        c.port = 0;                // ephemeral
        c.bindHost = "127.0.0.1";  // loopback only
        c.maxPlayers = 8;
        c.tickHz = 20;
        c.helloTimeoutMs = 3000;
        c.peerTimeoutLimit = 4;
        c.peerTimeoutMinMs = 400;
        c.peerTimeoutMaxMs = 1000;
        // Every client here is 127.0.0.1 and the run opens far more than five connections in a minute, so the
        // per-address connect rate (default 5 a minute, apps/server/anticheat.h ConnectLimiter) is off for this
        // in-process server; production keeps the limit and has no loopback exemption.
        c.connectsPerIpPerMinute = 0;
        c.log = [](const std::string&) {}; // the test reads state, not the log
        if (tweak) tweak(c);
        std::string err;
        if (!srv.Start(c, &err)) {
            std::printf("      server start failed: %s\n", err.c_str());
            return false;
        }
        th_ = std::thread([this] { Loop(); });
        return true;
    }
    ~ServerRunner() {
        stop_ = true;
        if (th_.joinable()) th_.join();
    }
    uint16_t port() const { return srv.port(); }
    void Post(std::function<void()> f) {
        std::lock_guard<std::mutex> lk(mu_);
        jobs_.push_back(std::move(f));
    }
    std::vector<server::PlayerInfo> Players() {
        std::lock_guard<std::mutex> lk(mu_);
        return players_;
    }
    // WorldState entries the server has sent so far, as deltas against an acknowledged baseline and in full
    // (all receivers; snapshots taken on the server thread after every Poll).
    uint64_t deltaEntries() const { return deltaEntries_.load(); }
    uint64_t absoluteEntries() const { return absoluteEntries_.load(); }

private:
    void Loop() {
        while (!stop_) {
            srv.Poll(5);
            deltaEntries_.store(srv.metrics().deltaEntries);
            absoluteEntries_.store(srv.metrics().absoluteEntries);
            std::deque<std::function<void()>> todo;
            {
                std::lock_guard<std::mutex> lk(mu_);
                todo.swap(jobs_);
            }
            for (auto& j : todo) j();
            std::lock_guard<std::mutex> lk(mu_);
            players_ = srv.Players();
        }
        srv.Stop();
    }
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> deltaEntries_{0}, absoluteEntries_{0};
    std::mutex mu_;
    std::deque<std::function<void()>> jobs_;
    std::vector<server::PlayerInfo> players_;
};

// ---- what the adapter's sinks heard (worker thread -> test thread) ------------
struct Heard {
    std::mutex mu;
    std::vector<std::pair<uint16_t, std::string>> joined;
    std::vector<uint16_t> left;
    std::map<uint16_t, proto::Kinematics> positions; // latest per player
    int worlds = 0;
    bool sawSelf = false;
    uint16_t selfId = 0xFFFF;
    std::vector<std::pair<uint16_t, std::string>> chats;
    int ended = 0;
    std::vector<std::pair<std::string, std::string>> events;
    // PeerSinks::onWelcome (sync plan W6: with the server's playerHealth)
    int welcomes = 0;
    uint16_t welcomeId = 0xFFFF, welcomeTickHz = 0;
    int welcomeHealth = -1;
    // v8: PeerSinks::onAccount / onAdminResult
    int accounts = 0;
    uint8_t accountFlags = 0;
    std::vector<proto::AdminResult> adminResults;

    void Clear() {
        std::lock_guard<std::mutex> lk(mu);
        joined.clear(); left.clear(); positions.clear(); worlds = 0; sawSelf = false;
        chats.clear(); ended = 0; events.clear();
        welcomes = 0; welcomeId = 0xFFFF; welcomeTickHz = 0; welcomeHealth = -1;
        accounts = 0; accountFlags = 0; adminResults.clear();
    }
    bool HasEvent(const std::string& name, const std::string& payloadPart) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : events)
            if (e.first == name && e.second.find(payloadPart) != std::string::npos) return true;
        return false;
    }
};
static Heard g_heard;

static void InstallSinks() {
    join::PeerSinks s;
    s.onWelcome = [](uint16_t myId, uint16_t tickHz, uint8_t playerHealth) {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        ++g_heard.welcomes;
        g_heard.welcomeId = myId;
        g_heard.welcomeTickHz = tickHz;
        g_heard.welcomeHealth = playerHealth;
    };
    s.onJoined = [](uint16_t id, const std::string& name) {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        g_heard.joined.push_back({id, name});
    };
    s.onLeft = [](uint16_t id) {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        g_heard.left.push_back(id);
    };
    s.onWorld = [](const proto::WorldState& w) {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        g_heard.worlds++;
        for (auto& e : w.players) {
            if (e.id == g_heard.selfId) g_heard.sawSelf = true;
            g_heard.positions[e.id] = e.k;
        }
    };
    s.onChat = [](uint16_t from, const std::string& text) {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        g_heard.chats.push_back({from, text});
    };
    s.onEnded = [] {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        g_heard.ended++;
    };
    s.onAccount = [](const proto::AccountInfo& a) {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        g_heard.accounts++;
        g_heard.accountFlags = a.flags;
    };
    s.onAdminResult = [](const proto::AdminResult& r) {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        g_heard.adminResults.push_back(r);
    };
    join::SetPeerSinks(std::move(s));
    join::SetEventSink([](const char* ev, const char* payload) {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        g_heard.events.push_back({ev, payload});
    });
}

// ---- Bob: a second player on the test thread ---------------------------------
struct Bob {
    net::ClientSession s;
    std::vector<net::Event> events;
    uint16_t id = 0;

    bool Join(uint16_t port, const char* name) {
        net::SessionOptions o;
        o.host = "127.0.0.1";
        o.port = port;
        o.name = name;
        o.connectTimeoutMs = 5000;
        o.closeTimeoutMs = 1000;
        std::string err;
        if (!s.Connect(o, &err)) return false;
        const auto t0 = Clock::now();
        while (MsSince(t0) < 3000 && s.state() != net::SessionState::Joined && s.state() != net::SessionState::Closed)
            Pump(5);
        id = s.myId();
        return s.state() == net::SessionState::Joined;
    }
    void Pump(int ms) {
        s.Poll(0);
        net::Event e;
        while (s.NextEvent(e)) events.push_back(e);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    bool HeardChat(uint16_t from, const std::string& text) const {
        for (auto& e : events)
            if (e.type == net::Event::Type::Chat && e.chat.fromId == from && e.chat.text == text) return true;
        return false;
    }
    bool HeardPlayerAt(uint16_t who, float x) const {
        for (auto& e : events)
            if (e.type == net::Event::Type::World)
                for (auto& p : e.world.players)
                    if (p.id == who && std::fabs(p.k.x - x) < 0.01f) return true;
        return false;
    }
};

static bool Until(Bob* bob, const std::function<bool()>& pred, int timeoutMs) {
    const auto t0 = Clock::now();
    while (MsSince(t0) < timeoutMs) {
        if (pred()) return true;
        if (bob) bob->Pump(5); else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

static bool ServerHasPlayer(ServerRunner& sr, const std::string& name) {
    for (auto& p : sr.Players())
        if (p.name == name) return true;
    return false;
}

int main() {
    // ---- pure helpers -------------------------------------------------------
    Check("SanitizeName: a valid name is unchanged", join::SanitizeName(L"Alice_01") == "Alice_01");
    Check("SanitizeName: bad characters become '_'", join::SanitizeName(L"user@name!") == "user_name_",
          join::SanitizeName(L"user@name!"));
    Check("SanitizeName: spaces are trimmed and not doubled", join::SanitizeName(L"  Ana   Maria ") == "Ana Maria",
          join::SanitizeName(L"  Ana   Maria "));
    Check("SanitizeName: one '_' per non-ASCII character", join::SanitizeName(L"გიო") == "___",
          join::SanitizeName(L"გიო"));
    Check("SanitizeName: cut to 24 characters", join::SanitizeName(std::wstring(40, L'x')) == std::string(24, 'x'));
    Check("SanitizeName: nothing left -> player", join::SanitizeName(L"") == "player" && join::SanitizeName(L"   ") == "player");
    Check("SanitizeName: every result passes ValidName",
          proto::ValidName(join::SanitizeName(L"a b\t\"c\" é --..")));
    {
        using R = proto::RefuseReason;
        bool finalOk = true, retryOk = true;
        for (R r : {R::VersionMismatch, R::BadName, R::Protocol, R::Banned})
            finalOk = finalOk && join::RefusalIsFinal(r);
        for (R r : {R::NameTaken, R::ServerFull, R::HelloTimeout, R::TooManyConnections})
            retryOk = retryOk && !join::RefusalIsFinal(r);
        Check("RefusalIsFinal: a version mismatch, a bad name, a protocol error and a ban end the engine's connecting; a taken name (our own "
              "dropped session's ghost), a full server, a missed Hello and the connect rate are retried",
              finalOk && retryOk && join::kRefusedRetryMs >= 1000, std::string(finalOk ? "" : "final ") + (retryOk ? "" : "retry "));
    }
    Check("JsonEscape: quote, backslash, newline and control characters",
          join::JsonEscape(std::string("a\"b\\c\n\x01")) == "a\\\"b\\\\c\\n\\u0001", join::JsonEscape(std::string("a\"b\\c\n\x01")));
    Check("JsonEscape: UTF-8 passes through", join::JsonEscape("\xE1\x83\x92") == "\xE1\x83\x92");

    // ---- the vehicle mailbox (sync plan W6: one latest-wins slot PER controlled vehicle) --------------------
    {
        join::VehicleMailbox mb;
        proto::VehicleUpdate u;
        u.vehId = 1; u.k.x = 1.0f;
        const bool p1 = mb.Put(u, 100);
        u.vehId = 2; u.k.x = 2.0f;
        const bool p2 = mb.Put(u, 101);
        u.vehId = 1; u.k.x = 3.0f;
        const bool p3 = mb.Put(u, 102);                        // newer for vehicle 1: replaces its slot, not vehicle 2's
        std::vector<join::VehicleMailbox::Item> out;
        mb.Take(out);
        const bool shape = out.size() == 2 && out[0].u.vehId == 1 && out[0].u.k.x == 3.0f && out[0].sampleMs == 102 &&
                           out[1].u.vehId == 2 && out[1].u.k.x == 2.0f && out[1].sampleMs == 101;
        std::vector<join::VehicleMailbox::Item> again;
        mb.Take(again);
        Check("vehicle mailbox: two vehicles keep one slot each (the newer of vehicle 1 wins), ascending id, each with its own sample time; Take clears",
              p1 && p2 && p3 && shape && again.empty() && mb.Pending() == 0, "out " + std::to_string(out.size()));
        u.vehId = proto::kMaxVehicles;
        const bool tooBig = mb.Put(u, 1);
        u.vehId = proto::kMaxVehicles - 1;
        const bool last = mb.Put(u, 1);
        const size_t pending = mb.Pending();
        mb.Clear();
        Check("vehicle mailbox: an id past kMaxVehicles is refused, the last valid one taken; Clear drops what waits",
              !tooBig && last && pending == 1 && mb.Pending() == 0);
    }

    // ---- nothing is listening ----------------------------------------------
    InstallSinks();
    {
        join::Config c;
        c.host = L"127.0.0.1";
        c.port = 9; // discard port: nobody answers an ENet handshake
        join::Session s;
        const auto t0 = Clock::now();
        const bool ok = join::Connect(c, 600, s);
        Check("dead server: Connect fails", !ok && !s.connected);
        Check("dead server: the reason names the host and is not a refusal", !s.refused && !s.error.empty(),
              Narrow(s.error));
        Check("dead server: it returns within the deadline plus a margin", MsSince(t0) < 600 + 2500, std::to_string(MsSince(t0)));
        Check("dead server: no session is left running", !join::IsConnected());
    }
    {
        join::Config c;
        join::Session s;
        Check("no host: Connect fails at once with a reason", !join::Connect(c, 600, s) && !s.error.empty(), Narrow(s.error));
        c.host = L"127.0.0.1";
        c.port = 70000;
        Check("bad port: Connect fails at once with a reason", !join::Connect(c, 600, s) && !s.error.empty(), Narrow(s.error));
    }

    // ---- a live server -------------------------------------------------------
    ServerRunner sr;
    if (!sr.Start()) {
        Check("server starts", false);
        return 1;
    }
    join::Config cfg;
    cfg.host = L"127.0.0.1";
    cfg.port = sr.port();
    cfg.player = L"Alice";

    // Bob first, so the adapter learns about him from PlayerJoined on its Welcome.
    Bob bob;
    Check("Bob joins the server", bob.Join(sr.port(), "Bob"));

    join::Session sess;
    g_heard.Clear();
    const bool joined = join::Connect(cfg, 5000, sess);
    Check("Connect: joins", joined && sess.connected, Narrow(sess.error));
    Check("Connect: Welcome fields come through",
          sess.serverName == "GAGE Join Test" && sess.tickHz == 20 && sess.myId != bob.id && !sess.refused && sess.refuseText.empty(),
          sess.serverName + " id=" + std::to_string(sess.myId));
    Check("IsConnected and MyId agree with the Welcome", join::IsConnected() && join::MyId() == sess.myId);
    {
        // sync plan W6: the server's playerHealth (this server runs the Config default, 200, not the client's 100)
        std::lock_guard<std::mutex> lk(g_heard.mu);
        Check("Welcome.playerHealth reaches onWelcome and the Session result (server default 200, not the client's 100)",
              g_heard.welcomes == 1 && g_heard.welcomeId == sess.myId && g_heard.welcomeTickHz == 20 && g_heard.welcomeHealth == 200 &&
                  sess.playerHealth == 200,
              "welcomes " + std::to_string(g_heard.welcomes) + " health " + std::to_string(g_heard.welcomeHealth) + "/" +
                  std::to_string(sess.playerHealth));
    }
    g_heard.selfId = sess.myId;
    Check("the server lists the adapter's player by name", Until(&bob, [&] { return ServerHasPlayer(sr, "Alice"); }, 2000));
    Check("event 'connected' carries the server name", g_heard.HasEvent("connected", "GAGE Join Test"));
    Check("Bob (already in) is announced to the adapter with his name",
          Until(&bob, [&] {
              std::lock_guard<std::mutex> lk(g_heard.mu);
              for (auto& j : g_heard.joined) if (j.first == bob.id && j.second == "Bob") return true;
              return false;
          }, 2000));
    Check("event 'playerJoined' carries id and name as JSON",
          g_heard.HasEvent("playerJoined", "\"id\":" + std::to_string(bob.id) + ",\"name\":\"Bob\""));

    // Chat, both ways. The server echoes a line to its sender too.
    Check("SendChat accepts a normal line", join::SendChat("hello from the engine"));
    Check("Bob hears the adapter's chat, attributed to its id",
          Until(&bob, [&] { return bob.HeardChat(sess.myId, "hello from the engine"); }, 2000));
    Check("the adapter hears its own line echoed",
          Until(&bob, [&] {
              std::lock_guard<std::mutex> lk(g_heard.mu);
              for (auto& c : g_heard.chats) if (c.first == sess.myId && c.second == "hello from the engine") return true;
              return false;
          }, 2000));
    bob.s.SendChat("hi Alice");
    Check("the adapter hears Bob's chat, attributed to Bob",
          Until(&bob, [&] {
              std::lock_guard<std::mutex> lk(g_heard.mu);
              for (auto& c : g_heard.chats) if (c.first == bob.id && c.second == "hi Alice") return true;
              return false;
          }, 2000));
    Check("SendChat refuses an empty line", !join::SendChat(""));
    Check("SendChat refuses a line over the 200-byte cap", !join::SendChat(std::string(201, 'x')));
    Check("SendChat refuses control characters", !join::SendChat("a\x01" "b"));

    // Positions, both ways.
    proto::Kinematics mine;
    mine.x = 100.0f; mine.y = 200.0f; mine.z = 30.0f; mine.heading = 90.0f; mine.vx = 1.5f;
    proto::Kinematics bobs;
    bobs.x = -50.0f; bobs.y = 60.0f; bobs.z = 12.0f;
    proto::Kinematics bad = mine;
    bad.x = NAN;
    Check("SendState refuses a non-finite position", !join::SendState(bad));
    proto::Kinematics outside = mine;
    outside.x = 99999.0f;
    Check("SendState refuses a position outside the world box", !join::SendState(outside));
    // Re-sent every 50 ms: state is unreliable and the server drops anything before the first accepted one.
    bool bobSawMe = false, aliceSawBob = false;
    const auto t0 = Clock::now();
    auto next = t0;
    while (MsSince(t0) < 4000 && !(bobSawMe && aliceSawBob)) {
        if (Clock::now() >= next) {
            join::SendState(mine);
            bob.s.SendState(bobs);
            next += std::chrono::milliseconds(50);
        }
        bob.Pump(5);
        bobSawMe = bob.HeardPlayerAt(sess.myId, 100.0f);
        std::lock_guard<std::mutex> lk(g_heard.mu);
        auto it = g_heard.positions.find(bob.id);
        aliceSawBob = it != g_heard.positions.end() && std::fabs(it->second.x - (-50.0f)) < 0.01f && std::fabs(it->second.y - 60.0f) < 0.01f;
    }
    Check("the server hands Bob the adapter's position", bobSawMe);
    Check("the adapter's onWorld gets Bob's position", aliceSawBob);
    {
        std::lock_guard<std::mutex> lk(g_heard.mu);
        Check("the adapter's own entry never comes back in a WorldState", g_heard.worlds > 0 && !g_heard.sawSelf,
              "worlds=" + std::to_string(g_heard.worlds));
    }

    // Keep-alive ack (sync plan N6b). The receipt for the world frames the adapter has decoded rides on its PlayerState, so
    // a game thread that posts nothing (a loading screen, a hitch, a menu) silences it, and after 32 ticks (1.6 s) the
    // server has no baseline that fits and sends everything about the other players in full. The worker therefore
    // repeats the latest state at 10 Hz while the game is quiet. Here the adapter posts nothing from now on and Bob keeps
    // walking; between 2.4 s and 3.8 s of that (well past the 1.6 s) Bob's entries to the adapter must still be deltas.
    {
        bobs.vx = 1.0f;
        const auto tq = Clock::now();
        auto nextBob = tq;
        bool marked = false;
        uint64_t d0 = 0, a0 = 0;
        while (MsSince(tq) < 3800) {
            if (Clock::now() >= nextBob) {
                bobs.x += 0.05f;
                bob.s.SendState(bobs);
                nextBob += std::chrono::milliseconds(50);
            }
            bob.Pump(5);
            if (!marked && MsSince(tq) >= 2400) {
                d0 = sr.deltaEntries();
                a0 = sr.absoluteEntries();
                marked = true;
            }
        }
        const uint64_t dWin = sr.deltaEntries() - d0, aWin = sr.absoluteEntries() - a0;
        Check("keep-alive: a game thread that posts no state for 3.8 s does not lose the delta baselines: in the last 1.4 s the entries are deltas (6+) and at most 2 are sent in full (it was all in full)",
              marked && dWin >= 6 && aWin <= 2, std::to_string(dWin) + " delta, " + std::to_string(aWin) + " in full");
    }

    // The sample time (sync plan W6, protocol v5 PlayerState.sampleMs). SendState's third argument is the instant the
    // game thread read the state; the server places the sample on its clock by it, and Bob's WorldEntry.ageMs for
    // the adapter's player is how long before the frame the server holds that sample. Black box: with the default
    // (the stamp of the call) a sender that moves every 50 ms shows ages well under 150 ms; with a stamp that never
    // advances (1000) the server cannot tell the samples apart in time, clamps them 300 ms back, and the age
    // saturates at 255. Both only differ if the argument really travels through the mailbox to the wire.
    {
        auto agesFrom = [&](size_t from, size_t keep) {
            std::vector<int> ages;
            for (size_t i = from; i < bob.events.size(); ++i)
                if (bob.events[i].type == net::Event::Type::World)
                    for (auto& p : bob.events[i].world.players)
                        if (p.id == sess.myId) ages.push_back(p.ageMs);
            if (ages.size() > keep) ages.erase(ages.begin(), ages.end() - (long)keep);
            return ages;
        };
        auto drive = [&](int ms, int32_t stamp) {
            const auto tq = Clock::now();
            auto nextTick = tq;
            while (MsSince(tq) < ms) {
                if (Clock::now() >= nextTick) {
                    mine.x += 0.05f;
                    mine.vx = 1.0f;
                    join::SendState(mine, proto::PedState(), stamp);
                    bobs.x += 0.05f;
                    bob.s.SendState(bobs);
                    nextTick += std::chrono::milliseconds(50);
                }
                bob.Pump(5);
            }
        };
        const size_t markNow = bob.events.size();
        drive(1800, -1);
        const std::vector<int> nowAges = agesFrom(markNow, 12);
        int maxNow = 0;
        for (int a : nowAges) maxNow = std::max(maxNow, a);
        const size_t markFrozen = bob.events.size();
        drive(2600, 1000);
        const std::vector<int> frozenAges = agesFrom(markFrozen, 12);
        int minFrozen = 255;
        for (int a : frozenAges) minFrozen = std::min(minFrozen, a);
        Check("sample time: the default stamp (the call) gives ages under 150 ms (12 last entries; 71 measured, loaded CI hosts jitter)",
              nowAges.size() == 12 && maxNow < 150, "n " + std::to_string(nowAges.size()) + " max " + std::to_string(maxNow));
        Check("sample time: an explicit, never-advancing stamp reaches the server (the age saturates, 200+ ms in all 12 last entries)",
              frozenAges.size() == 12 && minFrozen >= 200, "n " + std::to_string(frozenAges.size()) + " min " + std::to_string(minFrozen));
    }

    // Bob leaves.
    bob.s.Disconnect();
    Check("Bob's departure reaches the adapter (onLeft and 'playerLeft')",
          Until(&bob, [&] {
              std::lock_guard<std::mutex> lk(g_heard.mu);
              for (uint16_t id : g_heard.left) if (id == bob.id) return true;
              return false;
          }, 3000) && g_heard.HasEvent("playerLeft", "\"id\":" + std::to_string(bob.id)));

    // Kicked by the server: the session ends, the sinks are told, nothing is left running.
    g_heard.Clear();
    sr.Post([&] { sr.srv.Kick("Alice"); });
    Check("a kick ends the session: IsConnected false", Until(nullptr, [] { return !join::IsConnected(); }, 3000));
    Check("a kick calls onEnded once and fires 'disconnected' with the reason",
          Until(nullptr, [] { std::lock_guard<std::mutex> lk(g_heard.mu); return g_heard.ended == 1; }, 1000) &&
              g_heard.HasEvent("disconnected", "kicked"));
    Check("SendChat after the session ended is refused", !join::SendChat("anyone there?"));

    // Reconnect over a finished session (Connect joins the old worker first).
    g_heard.Clear();
    join::Session again;
    Check("Connect again after the session ended", join::Connect(cfg, 5000, again) && again.connected, Narrow(again.error));
    {
        // 500 lines in a tight loop against a 10 ms worker poll: the outgoing queue (32 lines)
        // fills, the rest are refused instead of growing without bound. This session is
        // throwaway, so a server-side flood limit acting on it cannot disturb a later check.
        int accepted = 0;
        for (int i = 0; i < 500; ++i)
            if (join::SendChat("burst " + std::to_string(i))) ++accepted;
        Check("a burst of 500 chat lines hits the outgoing queue cap: some taken, some refused",
              accepted >= 1 && accepted < 500, "accepted " + std::to_string(accepted));
    }
    join::Disconnect();
    Check("Disconnect: not connected, and a second Disconnect is harmless", !join::IsConnected() && (join::Disconnect(), true));
    Check("Disconnect: the server saw a goodbye and dropped the player",
          Until(nullptr, [&] { return !ServerHasPlayer(sr, "Alice"); }, 3000));

    // A refusal: the name is taken.
    {
        Bob holder;
        Check("setup: another player holds the name 'Alice'", holder.Join(sr.port(), "Alice"));
        join::Session refused;
        const bool ok = join::Connect(cfg, 5000, refused);
        Check("refused name: Connect fails and says so", !ok && refused.refused, Narrow(refused.error));
        Check("refused name: the reason carries the server's wording", Narrow(refused.error).find("NameTaken") != std::string::npos,
              Narrow(refused.error));
        // The engine shows a VersionMismatch to the player from these two fields (engine.cpp OnJoinRefused), so a
        // refusal must hand over the server's reason and sentence verbatim, and only a refusal does.
        Check("refused name: the Session carries the Refuse reason and the server's text verbatim",
              refused.refuseReason == proto::RefuseReason::NameTaken && !refused.refuseText.empty() &&
                  Narrow(refused.error).find(refused.refuseText) != std::string::npos,
              refused.refuseText);
        Check("refused name: no session is left running", !join::IsConnected());
    }

    // A name the protocol would refuse is cleaned up, not refused.
    {
        join::Config c = cfg;
        c.player = L"გიო";
        join::Session s;
        Check("a non-ASCII player name joins as '___'", join::Connect(c, 5000, s) && Until(nullptr, [&] { return ServerHasPlayer(sr, "___"); }, 2000),
              Narrow(s.error));
        join::Disconnect();
    }

    // v8: the adapter forwards Config::joinToken in JoinAuth, surfaces AccountInfo, and round-trips an admin command.
    {
        g_heard.Clear();
        join::Config c = cfg;
        c.player = L"Tokio";
        c.joinToken = "gjt_token-from-the-launcher";
        join::Session s;
        const bool in = join::Connect(c, 5000, s);
        const bool acct = Until(nullptr, [] { std::lock_guard<std::mutex> lk(g_heard.mu); return g_heard.accounts == 1; }, 2000);
        proto::AdminCmd heal;
        heal.seq = 77;
        heal.op = proto::AdminOp::Heal;
        const bool sent = join::SendAdminCmd(heal);
        const bool answered = Until(nullptr, [] { std::lock_guard<std::mutex> lk(g_heard.mu); return !g_heard.adminResults.empty(); }, 2000);
        std::lock_guard<std::mutex> lk(g_heard.mu);
        Check("v8: a token on a server without a platform joins as a guest; onAccount says 'admin commands allowed, not verified', and "
              "SendAdminCmd(/heal) is answered Ok through onAdminResult with its seq",
              in && acct && (g_heard.accountFlags & proto::kAcctCommands) && !(g_heard.accountFlags & proto::kAcctVerified) && sent &&
                  answered && g_heard.adminResults[0].seq == 77 && g_heard.adminResults[0].code == proto::AdminCode::Ok,
              Narrow(s.error) + " flags " + std::to_string(g_heard.accountFlags));
    }
    join::Disconnect();
    {
        // requireAccount against a platform that does not answer (a closed loopback port): with a token the server fails
        // closed (AuthUnavailable), without one it asks for an account (AccountRequired); the engine treats both as final.
        ServerRunner strict;
        const bool up = strict.Start([](server::Config& c) {
            c.platformUrl = "http://127.0.0.1:1";
            c.platformServerId = "sol";
            c.platformServerSecret = "0123456789abcdefXYZ";
            c.requireAccount = true;
            c.platformTimeoutMs = 500;
        });
        join::Config c = cfg;
        c.port = strict.port();
        c.player = L"Strict";
        c.joinToken = "gjt_whatever";
        join::Session withTok, noTok;
        const bool a = up && !join::Connect(c, 8000, withTok);
        c.joinToken.clear();
        const bool b = up && !join::Connect(c, 5000, noTok);
        Check("v8: requireAccount refuses a token the platform cannot check (AuthUnavailable) and a join without one (AccountRequired), "
              "both final for the engine, with the server's sentence",
              a && b && withTok.refused && withTok.refuseReason == proto::RefuseReason::AuthUnavailable && noTok.refused &&
                  noTok.refuseReason == proto::RefuseReason::AccountRequired && join::RefusalIsFinal(withTok.refuseReason) &&
                  join::RefusalIsFinal(noTok.refuseReason) && noTok.refuseText.find("Solar RP account") != std::string::npos,
              Narrow(withTok.error) + " | " + Narrow(noTok.error));
    }

    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    const int expected = 49 + 2 + 1 + 2 + 1 + 1 + 2;   // + vehicle mailbox 2 + playerHealth 1 + sample time 2 (sync plan W6) + refusal reason/text 1 + RefusalIsFinal 1 + v8 accounts 2
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
