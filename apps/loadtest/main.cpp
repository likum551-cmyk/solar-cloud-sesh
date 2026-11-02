// gage-loadtest: drives N headless GAGE clients against a GAGE server and reports what
// the server and the clients measured, once a second and as a summary over a window.
//
//   gage-loadtest --bots N [--scenario spread|hotspot|mixed] [--seconds S] [--warmup W]
//                 [--ramp JOINS_PER_S] [--threads T] [--seed K] [--poll-ms MS]
//                 [--chat-interval SECONDS] [--server-config server.json] [--csv out.csv]
//                 [--host H --port P]
//
// Two modes:
//   * embedded (default): the server runs inside this process on its own thread
//     (gage::server::Server on 127.0.0.1, an ephemeral port), so its tick time, CPU and
//     byte counters are read straight from Server::metrics(). This is the benchmark mode.
//     --server-config loads a server.json first; the harness then forces the loopback bind,
//     an ephemeral port, no per-address cap, no per-address connect rate, no movement check,
//     no ban file and maxPlayers >= N.
//   * external (--host/--port): the bots join a server you started yourself (it needs
//     "maxConnectionsPerIp": 0, "connectsPerIpPerMinute": 0, "untrustedWindowMs": 0 and
//     "maxPlayers" >= N). Only the client-side numbers exist;
//     read the server's side with its `stats` console command.
//
// Bots are real ClientSessions (one ENet host, one UDP socket each), spread over T worker
// threads; each sends its state at 20 Hz. Roles: walkers (1-5 m/s), drivers (30-60 m/s),
// idlers. Scenarios place them spread over an 8 x 8 km square, in 6 hotspots, or half each.
// Loopback only in embedded mode; external mode connects to whatever --host says, so point it
// at a server you own.
//
// Limits (also in docs/perf/BASELINE.md): one process, so the bots, the server thread and the
// OS share the machine's cores (on this class of box 1000 bots need ~3 worker threads; past
// that the clients, not the server, become the bottleneck and the client-side receive rate says
// so); kMaxPlayers is 1024 per server; Windows opens one socket per bot (no hard limit hit at
// 1000); the harness never touches the network outside loopback unless --host says so.
//
// Exit codes: 0 finished, 1 could not start (config, bind, connect), 2 usage, 3 finished but a client
// rejected or failed to decode a world frame (a protocol bug: the run is not valid).
#include "netclient/session.h"
#include "server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

using namespace gage;

namespace {

    using Clock = std::chrono::steady_clock;
    const Clock::time_point g_t0 = Clock::now();
    volatile std::sig_atomic_t g_stop = 0;

    void OnSignal(int) { g_stop = 1; }

    double Now() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

    double ProcessCpuSeconds()
    {
#ifdef _WIN32
        FILETIME c, e, k, u;
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
        auto t = [](const FILETIME& f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime); };
        return (t(k) + t(u)) * 1e-7;
#else
        rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
        return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
#endif
    }

    // xorshift64*: one per bot, so a run is reproducible from --seed whatever the thread timing.
    struct Rng
    {
        uint64_t s = 88172645463325252ull;
        explicit Rng(uint64_t seed = 1) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) { if (!s) s = 1; for (int i = 0; i < 4; i++) next(); }
        uint64_t next()
        {
            s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
            return s * 2685821657736338717ull;
        }
        double uni() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
        double uni(double lo, double hi) { return lo + (hi - lo) * uni(); }
    };

    constexpr double kPi = 3.14159265358979323846;
    constexpr double kBoxHalf = 3900.0; // spread scenario: +-3.9 km, inside the +-16 km world box

    struct Hotspot { double x, y, radius, weight; };
    // Six busy places (Legion Square, the airport, ...). Weights are the share of the hotspot crowd.
    const Hotspot kHotspots[] = {
        {195, -934, 90, 0.30},    {-1037, -2737, 150, 0.25}, {1700, 3200, 110, 0.20},
        {-1600, 200, 80, 0.10},   {2500, -1200, 100, 0.10},  {-200, 3600, 70, 0.05},
    };

    enum class Role { Walker, Driver, Idler };
    enum class Scenario { Spread, Hotspot, Mixed };

    const char* RoleName(Role r) { return r == Role::Walker ? "walker" : r == Role::Driver ? "driver" : "idler"; }

    double WrapDeg(double h)
    {
        h = std::fmod(h, 360.0);
        return h < 0 ? h + 360.0 : h;
    }
    // Signed shortest turn from a to b, degrees in (-180, 180].
    double DeltaDeg(double from, double to)
    {
        double d = std::fmod(to - from, 360.0);
        if (d > 180.0) d -= 360.0;
        if (d <= -180.0) d += 360.0;
        return d;
    }

    struct BotSnap
    {
        uint64_t wireBytesIn = 0, wireDatagramsIn = 0, worldPackets = 0, worldBytes = 0, worldEntries = 0;
        uint64_t deltaEntries = 0, absoluteEntries = 0, rejected = 0, stale = 0, malformed = 0, coarseEntries = 0;
        uint32_t rttMs = 0;
        bool joined = false, lost = false, refused = false;
    };

    struct Bot
    {
        int index = 0;
        Role role = Role::Walker;
        int hotspot = -1;       // -1: spread over the box
        Rng rng;
        net::ClientSession session;
        std::atomic<bool> go{false};

        // movement (GTA heading: 0 faces +Y, grows towards -X)
        double x = 0, y = 0, z = 30, heading = 0, speed = 0;
        double targetHeading = 0, targetSpeed = 0, nextTurn = 0;
        double cx = 0, cy = 0, confine = 0; // confinement circle; confine 0 = the whole box
        double lastStep = 0;

        // session bookkeeping (worker thread only)
        bool started = false, joined = false, lost = false, refused = false;
        double nextSend = 0, nextChat = 0;
        uint32_t chatsSent = 0;
        uint64_t chatsHeard = 0;
        std::string name;

        explicit Bot(uint64_t seed) : rng(seed) {}
    };

    struct Options
    {
        int bots = 50;
        Scenario scenario = Scenario::Mixed;
        double seconds = 20, warmup = 5;
        int ramp = 100;
        int threads = 0;
        uint64_t seed = 1;
        double pollMs = 4;
        double chatInterval = 120;
        std::string serverConfig, csv, host;
        int port = 0;
        bool external = false;
    };

    // ------------------------------------------------------------------ placement

    void Place(Bot& b, Scenario sc, Rng& rng)
    {
        bool inHotspot = sc == Scenario::Hotspot || (sc == Scenario::Mixed && (b.index % 2 == 0));
        if (inHotspot)
        {
            double r = rng.uni(), acc = 0;
            int h = 0;
            for (int i = 0; i < 6; i++)
            {
                acc += kHotspots[i].weight;
                if (r <= acc || i == 5) { h = i; break; }
            }
            b.hotspot = h;
            const Hotspot& hs = kHotspots[h];
            const double a = rng.uni(0, 2 * kPi), d = hs.radius * std::sqrt(rng.uni());
            b.x = hs.x + d * std::cos(a);
            b.y = hs.y + d * std::sin(a);
            b.cx = hs.x;
            b.cy = hs.y;
            // Hotspot crowds: half idle, the rest walk; a few circle in cars around the place.
            const double role = rng.uni();
            b.role = role < 0.35 ? Role::Idler : role < 0.85 ? Role::Walker : Role::Driver;
            b.confine = b.role == Role::Driver ? hs.radius * 2.5 + 150 : hs.radius;
        }
        else
        {
            b.hotspot = -1;
            b.x = rng.uni(-kBoxHalf, kBoxHalf);
            b.y = rng.uni(-kBoxHalf, kBoxHalf);
            b.cx = b.cy = 0;
            b.confine = 0;
            const double role = rng.uni();
            b.role = role < 0.15 ? Role::Idler : role < 0.75 ? Role::Walker : Role::Driver;
        }
        b.z = 30.0 + rng.uni(-2, 2);
        b.heading = rng.uni(0, 360);
        b.targetHeading = b.heading;
        b.speed = 0;
        b.targetSpeed = 0;
        b.nextTurn = 0;
    }

    // Advances one bot by dt seconds at time `now`.
    void Step(Bot& b, double now, double dt)
    {
        if (b.role == Role::Idler)
        {
            b.speed = 0;
            return;
        }
        const bool driver = b.role == Role::Driver;
        if (now >= b.nextTurn)
        {
            b.targetHeading = WrapDeg(b.heading + b.rng.uni(driver ? -60 : -100, driver ? 60 : 100));
            b.nextTurn = now + (driver ? b.rng.uni(4, 12) : b.rng.uni(2, 8));
            if (driver)
                b.targetSpeed = b.rng.uni(30, 60);
            else
            {
                const double r = b.rng.uni();
                b.targetSpeed = r < 0.05 ? 0.0 : r < 0.65 ? 1.4 : r < 0.90 ? 2.4 : 5.5; // stop, walk, jog, sprint
            }
        }
        // Steer back towards the middle when leaving the area this bot lives in.
        const double ax = b.confine > 0 ? b.cx : 0.0, ay = b.confine > 0 ? b.cy : 0.0;
        const double dx = b.x - ax, dy = b.y - ay;
        const double limit = b.confine > 0 ? b.confine : kBoxHalf;
        if (std::sqrt(dx * dx + dy * dy) > limit * (b.confine > 0 ? 1.0 : 1.2) ||
            (b.confine == 0 && (std::fabs(b.x) > kBoxHalf || std::fabs(b.y) > kBoxHalf)))
        {
            b.targetHeading = WrapDeg(std::atan2(-(ax - b.x), (ay - b.y)) * 180.0 / kPi + b.rng.uni(-20, 20));
            b.nextTurn = now + 3;
        }
        const double turnRate = driver ? 28.0 : 140.0, accel = driver ? 6.0 : 3.5; // deg/s, m/s^2
        const double dh = DeltaDeg(b.heading, b.targetHeading);
        b.heading = WrapDeg(b.heading + std::max(-turnRate * dt, std::min(turnRate * dt, dh)));
        const double ds = b.targetSpeed - b.speed;
        b.speed += std::max(-accel * dt, std::min(accel * dt, ds));
        // Real controllers are not perfectly steady: a few percent of speed noise.
        const double v = std::max(0.0, b.speed * (1.0 + b.rng.uni(-0.03, 0.03)));
        const double rad = b.heading * kPi / 180.0;
        b.x += -std::sin(rad) * v * dt;
        b.y += std::cos(rad) * v * dt;
        b.x = std::max(-15000.0, std::min(15000.0, b.x));
        b.y = std::max(-15000.0, std::min(15000.0, b.y));
    }

    proto::Kinematics KinOf(const Bot& b)
    {
        proto::Kinematics k;
        k.x = (float)b.x;
        k.y = (float)b.y;
        k.z = (float)b.z;
        k.heading = (float)b.heading;
        const double rad = b.heading * kPi / 180.0;
        const double v = b.role == Role::Idler ? 0.0 : b.speed;
        k.vx = (float)(-std::sin(rad) * v);
        k.vy = (float)(std::cos(rad) * v);
        k.vz = 0;
        return k;
    }

    // ------------------------------------------------------------------ workers

    struct Worker
    {
        std::vector<Bot*> bots;
        std::mutex mu;
        std::vector<BotSnap> snap; // published copy, parallel to `bots`
        std::thread th;
    };

    void RunWorker(Worker* w, const Options* opt, uint16_t port, std::atomic<bool>* stop)
    {
        double lastPublish = -1;
        const double period = 1.0 / 20.0;
        while (!stop->load())
        {
            const double now = Now();
            for (Bot* b : w->bots)
            {
                if (b->lost || b->refused) continue;
                if (!b->started)
                {
                    if (!b->go.load()) continue;
                    net::SessionOptions so;
                    so.host = opt->external ? opt->host : "127.0.0.1";
                    so.port = port;
                    so.name = b->name;
                    so.connectTimeoutMs = 20000;
                    std::string err;
                    b->started = true;
                    if (!b->session.Connect(so, &err))
                    {
                        std::fprintf(stderr, "bot %d: cannot connect: %s\n", b->index, err.c_str());
                        b->lost = true;
                        continue;
                    }
                }
                b->session.Poll(0);
                net::Event e;
                while (b->session.NextEvent(e))
                {
                    switch (e.type)
                    {
                    case net::Event::Type::Welcome:
                        b->joined = true;
                        b->nextSend = now + b->rng.uni(0, period);
                        b->lastStep = b->nextSend;
                        b->nextChat = now + b->rng.uni(1.0, std::max(2.0, opt->chatInterval));
                        break;
                    case net::Event::Type::Refused:
                        b->refused = true;
                        break;
                    case net::Event::Type::Chat:
                        b->chatsHeard++;
                        break;
                    case net::Event::Type::Disconnected:
                        b->lost = true;
                        break;
                    default:
                        break;
                    }
                }
                if (b->joined && !b->lost)
                {
                    if (now >= b->nextSend)
                    {
                        const double dt = std::min(0.25, now - b->lastStep);
                        Step(*b, now, dt);
                        b->lastStep = now;
                        b->session.SendState(KinOf(*b));
                        b->nextSend += period;
                        if (b->nextSend < now - 0.5) b->nextSend = now + period; // we fell far behind: do not burst
                    }
                    if (opt->chatInterval > 0 && now >= b->nextChat)
                    {
                        b->session.SendChat("bot " + std::to_string(b->index) + " says hello #" + std::to_string(++b->chatsSent));
                        b->nextChat = now + b->rng.uni(0.5, 1.5) * opt->chatInterval;
                    }
                }
            }
            if (now - lastPublish >= 0.25)
            {
                lastPublish = now;
                std::lock_guard<std::mutex> lk(w->mu);
                w->snap.resize(w->bots.size());
                for (size_t i = 0; i < w->bots.size(); i++)
                {
                    const Bot& b = *w->bots[i];
                    BotSnap& s = w->snap[i];
                    if (b.started)
                    {
                        const auto c = b.session.counters();
                        s.wireBytesIn = c.wireBytesIn;
                        s.wireDatagramsIn = c.wireDatagramsIn;
                        s.worldPackets = c.worldPackets;
                        s.worldBytes = c.worldBytes;
                        s.worldEntries = c.worldEntries;
                        s.deltaEntries = c.worldDeltaEntries;
                        s.absoluteEntries = c.worldAbsoluteEntries;
                        s.rejected = c.worldRejected;
                        s.stale = c.worldStale;
                        s.malformed = b.session.malformed();
                        s.coarseEntries = c.coarseEntries;
                        s.rttMs = b.session.rttMs();
                    }
                    s.joined = b.joined && !b.lost;
                    s.lost = b.lost;
                    s.refused = b.refused;
                }
            }
            // The loop period bounds how often each bot's ENet host is serviced.
            const double spent = Now() - now;
            const double wait = opt->pollMs / 1000.0 - spent;
            if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        }
        // Sessions are destroyed with their bots (a graceful quit each, no waiting).
    }

    // ------------------------------------------------------------------ embedded server

    struct ServerHost
    {
        server::Config cfg;
        server::Server srv;
        std::thread th;
        std::atomic<bool> stop{false}, resetPeaks{false}, started{false}, failed{false};
        std::mutex mu;
        server::Metrics published; // copy for the main thread
        uint64_t publishSeq = 0;
        uint16_t port = 0;
        std::string error;
        std::vector<std::string> notable; // log lines worth showing (errors, refusals, drops)
        uint64_t logLines = 0;
    };

    void RunServer(ServerHost* h)
    {
        h->cfg.log = [h](const std::string& line) {
            h->logLines++;
            if (h->notable.size() < 40 &&
                (line.find("ERROR") != std::string::npos || line.find("refuse") != std::string::npos ||
                 line.find("drop ") != std::string::npos || line.find("leave ") != std::string::npos))
                h->notable.push_back(line);
        };
        std::string err;
        if (!h->srv.Start(h->cfg, &err))
        {
            h->error = err;
            h->failed = true;
            h->started = true;
            return;
        }
        h->port = h->srv.port();
        h->started = true;
        double lastPub = 0;
        while (!h->stop.load())
        {
            h->srv.Poll(1);
            if (h->resetPeaks.exchange(false)) h->srv.ResetMetricsPeaks();
            const double now = Now();
            if (now - lastPub >= 0.1)
            {
                lastPub = now;
                std::lock_guard<std::mutex> lk(h->mu);
                h->published = h->srv.metrics();
                h->publishSeq++;
            }
        }
        {
            std::lock_guard<std::mutex> lk(h->mu);
            h->published = h->srv.metrics();
            h->publishSeq++;
        }
        h->srv.Stop();
    }

    // ------------------------------------------------------------------ reporting

    struct ClientTotals
    {
        std::vector<BotSnap> bots; // one per bot, in bot order
        int joined = 0, lost = 0, refused = 0;
    };

    ClientTotals CollectClients(std::vector<std::unique_ptr<Worker>>& workers, int nBots,
                                const std::vector<std::unique_ptr<Bot>>& bots)
    {
        ClientTotals t;
        t.bots.resize(nBots);
        for (auto& w : workers)
        {
            std::lock_guard<std::mutex> lk(w->mu);
            for (size_t i = 0; i < w->snap.size() && i < w->bots.size(); i++)
                t.bots[w->bots[i]->index] = w->snap[i];
        }
        for (const BotSnap& s : t.bots)
        {
            if (s.joined) t.joined++;
            if (s.lost) t.lost++;
            if (s.refused) t.refused++;
        }
        (void)bots;
        return t;
    }

    double Pct(std::vector<double> v, double q)
    {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        size_t i = (size_t)(q * (double)v.size());
        if (i >= v.size()) i = v.size() - 1;
        return v[i];
    }

    int Usage(const char* why)
    {
        if (why) std::fprintf(stderr, "gage-loadtest: %s\n", why);
        std::fprintf(stderr,
                     "usage: gage-loadtest --bots N [--scenario spread|hotspot|mixed] [--seconds S] [--warmup W]\n"
                     "                     [--ramp JOINS_PER_S] [--threads T] [--seed K] [--poll-ms MS]\n"
                     "                     [--chat-interval SECONDS] [--server-config server.json] [--csv out.csv]\n"
                     "                     [--host H --port P]   (external server; client-side numbers only)\n");
        return 2;
    }

    bool ParseNum(const char* s, double& out)
    {
        char* end = nullptr;
        out = std::strtod(s, &end);
        return end != s && *end == '\0' && std::isfinite(out);
    }

}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Options opt;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { Usage(nullptr); return 0; }
        if (i + 1 >= argc) return Usage(("missing value for " + a).c_str());
        const char* v = argv[++i];
        double n = 0;
        if (a == "--bots") { if (!ParseNum(v, n) || n < 1 || n > 4000) return Usage("--bots must be 1..4000"); opt.bots = (int)n; }
        else if (a == "--seconds") { if (!ParseNum(v, n) || n < 1) return Usage("--seconds must be >= 1"); opt.seconds = n; }
        else if (a == "--warmup") { if (!ParseNum(v, n) || n < 0) return Usage("--warmup must be >= 0"); opt.warmup = n; }
        else if (a == "--ramp") { if (!ParseNum(v, n) || n < 1) return Usage("--ramp must be >= 1"); opt.ramp = (int)n; }
        else if (a == "--threads") { if (!ParseNum(v, n) || n < 1 || n > 32) return Usage("--threads must be 1..32"); opt.threads = (int)n; }
        else if (a == "--seed") { if (!ParseNum(v, n) || n < 0) return Usage("--seed must be >= 0"); opt.seed = (uint64_t)n; }
        else if (a == "--poll-ms") { if (!ParseNum(v, n) || n < 0.5 || n > 100) return Usage("--poll-ms must be 0.5..100"); opt.pollMs = n; }
        else if (a == "--chat-interval") { if (!ParseNum(v, n) || n < 0) return Usage("--chat-interval must be >= 0 (0 = no chat)"); opt.chatInterval = n; }
        else if (a == "--scenario")
        {
            if (!std::strcmp(v, "spread")) opt.scenario = Scenario::Spread;
            else if (!std::strcmp(v, "hotspot")) opt.scenario = Scenario::Hotspot;
            else if (!std::strcmp(v, "mixed")) opt.scenario = Scenario::Mixed;
            else return Usage("--scenario must be spread, hotspot or mixed");
        }
        else if (a == "--server-config") opt.serverConfig = v;
        else if (a == "--csv") opt.csv = v;
        else if (a == "--host") { opt.host = v; opt.external = true; }
        else if (a == "--port") { if (!ParseNum(v, n) || n < 1 || n > 65535) return Usage("--port must be 1..65535"); opt.port = (int)n; }
        else return Usage(("unknown argument " + a).c_str());
    }
    if (opt.external && opt.port == 0) return Usage("--host needs --port");
    if (!opt.external && opt.port != 0) return Usage("--port needs --host (embedded mode picks its own port)");
    if (opt.threads == 0) opt.threads = opt.bots >= 600 ? 4 : opt.bots >= 150 ? 3 : 2;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    const char* scName = opt.scenario == Scenario::Spread ? "spread" : opt.scenario == Scenario::Hotspot ? "hotspot" : "mixed";

    // ---- the server ----
    ServerHost host;
    uint16_t port = (uint16_t)opt.port;
    if (!opt.external)
    {
        if (!opt.serverConfig.empty())
        {
            std::string err;
            if (!server::LoadConfigFile(opt.serverConfig, host.cfg, &err))
            {
                std::fprintf(stderr, "gage-loadtest: %s\n", err.c_str());
                return 1;
            }
        }
        host.cfg.port = 0;
        host.cfg.bindHost = "127.0.0.1";
        host.cfg.maxConnectionsPerIp = 0;
        // Every bot is 127.0.0.1 and the bots' drivers cover 30-60 m/s while reporting an on-foot ped: the per-address
        // connect rate and the movement check (anti-cheat rules, docs/api/server.md) would refuse or flag them.
        host.cfg.connectsPerIpPerMinute = 0;
        host.cfg.untrustedWindowMs = 0;
        host.cfg.bansFile = "";
        host.cfg.masterUrl = "";
        host.cfg.adminPort = 0;
        host.cfg.adminEphemeral = false;
        if (opt.bots > proto::kMaxPlayers)
        {
            std::fprintf(stderr, "gage-loadtest: a server holds at most %u players (kMaxPlayers); use --bots <= %u\n",
                         (unsigned)proto::kMaxPlayers, (unsigned)proto::kMaxPlayers);
            return 2;
        }
        host.cfg.maxPlayers = (uint16_t)std::max<int>(host.cfg.maxPlayers, opt.bots);
        host.th = std::thread(RunServer, &host);
        while (!host.started.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (host.failed.load())
        {
            host.th.join();
            std::fprintf(stderr, "gage-loadtest: server did not start: %s\n", host.error.c_str());
            return 1;
        }
        port = host.port;
    }

    // ---- the bots ----
    std::vector<std::unique_ptr<Bot>> bots;
    Rng placeRng(opt.seed * 7919 + 13);
    for (int i = 0; i < opt.bots; i++)
    {
        auto b = std::make_unique<Bot>(opt.seed * 1000003ull + (uint64_t)i * 7919ull + 1);
        b->index = i;
        b->name = "Bot" + std::to_string(i);
        Place(*b, opt.scenario, placeRng);
        bots.push_back(std::move(b));
    }
    int nWalk = 0, nDrive = 0, nIdle = 0, nHot = 0;
    for (auto& b : bots)
    {
        (b->role == Role::Walker ? nWalk : b->role == Role::Driver ? nDrive : nIdle)++;
        if (b->hotspot >= 0) nHot++;
    }
    std::vector<std::unique_ptr<Worker>> workers;
    for (int t = 0; t < opt.threads; t++) workers.push_back(std::make_unique<Worker>());
    for (int i = 0; i < opt.bots; i++) workers[(size_t)i % workers.size()]->bots.push_back(bots[i].get());
    std::atomic<bool> stopWorkers{false};

    std::printf("gage-loadtest: %d bots (%d walkers, %d drivers, %d idlers; %d in hotspots), scenario %s, %s, %d worker threads, seed %llu\n",
                opt.bots, nWalk, nDrive, nIdle, nHot, scName, opt.external ? "external server" : "embedded server on 127.0.0.1",
                opt.threads, (unsigned long long)opt.seed);
    if (!opt.external)
        std::printf("server: max %u players, %u Hz, port %u\n", (unsigned)host.cfg.maxPlayers, (unsigned)host.cfg.tickHz, (unsigned)port);

    const double cpuStart = ProcessCpuSeconds();
    const double wallStart = Now();
    for (auto& w : workers) w->th = std::thread(RunWorker, w.get(), &opt, port, &stopWorkers);

    // ---- ramp: join opt.ramp bots per second ----
    {
        int activated = 0;
        const double rampStart = Now();
        const double deadline = rampStart + (double)opt.bots / (double)opt.ramp + 45.0;
        while (!g_stop)
        {
            const double t = Now() - rampStart;
            const int want = std::min(opt.bots, (int)(t * opt.ramp) + 1);
            while (activated < want) bots[activated++]->go.store(true);
            const ClientTotals ct = CollectClients(workers, opt.bots, bots);
            if (activated == opt.bots && ct.joined + ct.refused + ct.lost >= opt.bots) break;
            if (Now() > deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const ClientTotals ct = CollectClients(workers, opt.bots, bots);
        std::printf("ramp: %d of %d joined in %.1f s (refused %d, lost %d)\n", ct.joined, opt.bots, Now() - rampStart, ct.refused, ct.lost);
    }

    // ---- warm-up, then the measured window ----
    {
        const double until = Now() + opt.warmup;
        while (!g_stop && Now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto serverSnapshot = [&](server::Metrics& out) {
        // A copy published after this call, so it reflects "now" rather than up to 100 ms ago.
        uint64_t seq;
        {
            std::lock_guard<std::mutex> lk(host.mu);
            seq = host.publishSeq;
        }
        for (int i = 0; i < 400; i++)
        {
            {
                std::lock_guard<std::mutex> lk(host.mu);
                if (host.publishSeq > seq + 1)
                {
                    out = host.published;
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::lock_guard<std::mutex> lk(host.mu);
        out = host.published;
    };

    server::Metrics m0, mPrev;
    if (!opt.external)
    {
        host.resetPeaks.store(true);
        serverSnapshot(m0);
        mPrev = m0;
    }
    const ClientTotals c0 = CollectClients(workers, opt.bots, bots);
    ClientTotals cPrev = c0;
    const double winStart = Now();
    const double cpuWinStart = ProcessCpuSeconds();

    std::FILE* csv = nullptr;
    if (!opt.csv.empty())
    {
        csv = std::fopen(opt.csv.c_str(), "w");
        if (!csv) std::fprintf(stderr, "gage-loadtest: cannot write %s\n", opt.csv.c_str());
        else std::fprintf(csv, "t_s,joined,tick_work_avg_us,tick_work_p99_us,tick_missed,cpu_pct,out_payload_Bps,out_wire_Bps,out_datagrams_ps,in_packets_ps,world_pkt_bytes_avg,client_in_Bps_avg\n");
    }
    std::printf("%6s %6s %9s %9s %7s %6s %11s %11s %10s %10s %11s %13s\n", "t(s)", "joined", "tick_avg", "tick_p99", "missed",
                "cpu%", "out_pay_B/s", "out_wire_B/s", "dgram/s", "in_pkt/s", "worldB_avg", "cli_in_B/s/cl");

    const int secs = (int)opt.seconds;
    for (int s = 1; s <= secs && !g_stop; s++)
    {
        const double target = winStart + s;
        while (!g_stop && Now() < target) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const ClientTotals c1 = CollectClients(workers, opt.bots, bots);
        double clientInBps = 0;
        int counted = 0;
        for (int i = 0; i < opt.bots; i++)
            if (c1.bots[i].joined && cPrev.bots[i].joined)
            {
                clientInBps += (double)(c1.bots[i].wireBytesIn - cPrev.bots[i].wireBytesIn);
                counted++;
            }
        clientInBps = counted ? clientInBps / counted : 0;
        if (!opt.external)
        {
            server::Metrics m1;
            serverSnapshot(m1);
            const double dt = std::max(1e-6, m1.wallSeconds - mPrev.wallSeconds);
            const double cpu = 100.0 * (m1.cpuSeconds - mPrev.cpuSeconds) / dt;
            const double pay = (double)(m1.payloadBytesOut - mPrev.payloadBytesOut) / dt;
            const double wire = (double)(m1.wireBytesOut - mPrev.wireBytesOut) / dt;
            const double dg = (double)(m1.wireDatagramsOut - mPrev.wireDatagramsOut) / dt;
            const double pin = (double)(m1.packetsIn - mPrev.packetsIn) / dt;
            std::printf("%6d %6d %9.0f %9.0f %7llu %6.1f %11.0f %11.0f %10.0f %10.0f %11.0f %13.0f\n", s, c1.joined,
                        m1.tickWorkUs.Mean(&mPrev.tickWorkUs), m1.tickWorkUs.Quantile(0.99, &mPrev.tickWorkUs),
                        (unsigned long long)(m1.ticksMissed - mPrev.ticksMissed), cpu, pay, wire, dg, pin,
                        m1.worldBytes.Mean(&mPrev.worldBytes), clientInBps);
            if (csv)
                std::fprintf(csv, "%d,%d,%.0f,%.0f,%llu,%.1f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n", s, c1.joined,
                             m1.tickWorkUs.Mean(&mPrev.tickWorkUs), m1.tickWorkUs.Quantile(0.99, &mPrev.tickWorkUs),
                             (unsigned long long)(m1.ticksMissed - mPrev.ticksMissed), cpu, pay, wire, dg, pin,
                             m1.worldBytes.Mean(&mPrev.worldBytes), clientInBps);
            mPrev = m1;
        }
        else
            std::printf("%6d %6d  (external server: no server-side numbers)  client in %.0f B/s per client\n", s, c1.joined, clientInBps);
        cPrev = c1;
    }
    if (csv) std::fclose(csv);

    // ---- summary over the window ----
    server::Metrics m1;
    if (!opt.external) serverSnapshot(m1);
    const ClientTotals c1 = CollectClients(workers, opt.bots, bots);
    const double winSecs = Now() - winStart;
    const double procCpu = (ProcessCpuSeconds() - cpuWinStart) / winSecs; // cores, server + clients + harness

    std::vector<double> cliBps, cliWorldPs, cliRtt, cliEntriesPer, cliDgramPs;
    uint64_t sumWireIn = 0, sumDelta = 0, sumAbs = 0, sumRejected = 0, sumStale = 0, sumMalformed = 0, sumCoarse = 0;
    int clientsMeasured = 0;
    for (int i = 0; i < opt.bots; i++)
    {
        const BotSnap& a = c0.bots[i];
        const BotSnap& b = c1.bots[i];
        if (!a.joined || !b.joined) continue; // only bots that were in for the whole window
        clientsMeasured++;
        cliBps.push_back((double)(b.wireBytesIn - a.wireBytesIn) / winSecs);
        cliDgramPs.push_back((double)(b.wireDatagramsIn - a.wireDatagramsIn) / winSecs);
        cliWorldPs.push_back((double)(b.worldPackets - a.worldPackets) / winSecs);
        const uint64_t wp = b.worldPackets - a.worldPackets;
        cliEntriesPer.push_back(wp ? (double)(b.worldEntries - a.worldEntries) / (double)wp : 0.0);
        cliRtt.push_back((double)b.rttMs);
        sumWireIn += b.wireBytesIn - a.wireBytesIn;
        sumDelta += b.deltaEntries - a.deltaEntries;
        sumAbs += b.absoluteEntries - a.absoluteEntries;
        sumRejected += b.rejected - a.rejected;
        sumStale += b.stale - a.stale;
        sumMalformed += b.malformed - a.malformed;
        sumCoarse += b.coarseEntries - a.coarseEntries;
    }
    auto avg = [](const std::vector<double>& v) {
        double s = 0;
        for (double x : v) s += x;
        return v.empty() ? 0.0 : s / (double)v.size();
    };

    std::printf("\n=== summary: %d bots, %s, window %.1f s (after %.0f s warm-up) ===\n", opt.bots, scName, winSecs, opt.warmup);
    std::printf("bots: joined at window start %d, at end %d, lost %d, refused %d\n", c0.joined, c1.joined, c1.lost, c1.refused);
    if (!opt.external)
    {
        const double dt = std::max(1e-6, m1.wallSeconds - m0.wallSeconds);
        const double expectTicks = dt * host.cfg.tickHz;
        std::printf("server tick work (us)   avg %.0f  p50 %.0f  p99 %.0f  max %llu   (%llu ticks, %.1f%% of %.0f expected)\n",
                    m1.tickWorkUs.Mean(&m0.tickWorkUs), m1.tickWorkUs.Quantile(0.5, &m0.tickWorkUs),
                    m1.tickWorkUs.Quantile(0.99, &m0.tickWorkUs), (unsigned long long)m1.tickWorkUs.max(),
                    (unsigned long long)(m1.ticks - m0.ticks), 100.0 * (double)(m1.ticks - m0.ticks) / expectTicks, expectTicks);
        std::printf("server tick lateness us avg %.0f  p99 %.0f  max %llu\n", m1.tickLateUs.Mean(&m0.tickLateUs),
                    m1.tickLateUs.Quantile(0.99, &m0.tickLateUs), (unsigned long long)m1.tickLateUs.max());
        std::printf("server missed ticks     %llu (%.2f per s)\n", (unsigned long long)(m1.ticksMissed - m0.ticksMissed),
                    (double)(m1.ticksMissed - m0.ticksMissed) / dt);
        std::printf("server cpu              %.1f %% of one core (the Poll thread)\n", 100.0 * (m1.cpuSeconds - m0.cpuSeconds) / dt);
        std::printf("server out              payload %.0f B/s, wire %.0f B/s (ENet headers incl.), %.0f datagrams/s, %.0f app packets/s\n",
                    (double)(m1.payloadBytesOut - m0.payloadBytesOut) / dt, (double)(m1.wireBytesOut - m0.wireBytesOut) / dt,
                    (double)(m1.wireDatagramsOut - m0.wireDatagramsOut) / dt, (double)(m1.packetsOut - m0.packetsOut) / dt);
        std::printf("server in               payload %.0f B/s, %.0f packets/s, %.0f datagrams/s\n",
                    (double)(m1.payloadBytesIn - m0.payloadBytesIn) / dt, (double)(m1.packetsIn - m0.packetsIn) / dt,
                    (double)(m1.wireDatagramsIn - m0.wireDatagramsIn) / dt);
        std::printf("server out per client   avg %.0f  p50 %.0f  p99 %.0f  max %llu B/s (payload)\n",
                    m1.clientBytesPerSec.Mean(&m0.clientBytesPerSec), m1.clientBytesPerSec.Quantile(0.5, &m0.clientBytesPerSec),
                    m1.clientBytesPerSec.Quantile(0.99, &m0.clientBytesPerSec), (unsigned long long)m1.clientBytesPerSec.max());
        std::printf("WorldState packet       avg %.0f  p50 %.0f  p99 %.0f  max %llu B;  entries avg %.1f  p99 %.0f  max %llu  (%llu packets, %.0f/s)\n",
                    m1.worldBytes.Mean(&m0.worldBytes), m1.worldBytes.Quantile(0.5, &m0.worldBytes),
                    m1.worldBytes.Quantile(0.99, &m0.worldBytes), (unsigned long long)m1.worldBytes.max(),
                    m1.worldEntryCount.Mean(&m0.worldEntryCount), m1.worldEntryCount.Quantile(0.99, &m0.worldEntryCount),
                    (unsigned long long)m1.worldEntryCount.max(), (unsigned long long)(m1.worldPackets - m0.worldPackets),
                    (double)(m1.worldPackets - m0.worldPackets) / dt);
        std::printf("coarse channel          %.0f packets/s, %.0f entries/s, %.0f payload B/s\n",
                    (double)(m1.coarsePackets - m0.coarsePackets) / dt, (double)(m1.coarseEntries - m0.coarseEntries) / dt,
                    (double)(m1.coarsePayloadBytes - m0.coarsePayloadBytes) / dt);
        std::printf("entries                 delta %.0f/s, absolute %.0f/s; budget deferred %.0f/s; relevance enters %.0f/s leaves %.0f/s; relevant pairs now %llu\n",
                    (double)(m1.deltaEntries - m0.deltaEntries) / dt, (double)(m1.absoluteEntries - m0.absoluteEntries) / dt,
                    (double)(m1.budgetDeferred - m0.budgetDeferred) / dt, (double)(m1.relevanceEnters - m0.relevanceEnters) / dt,
                    (double)(m1.relevanceLeaves - m0.relevanceLeaves) / dt, (unsigned long long)m1.relevantPairs);
    }
    std::printf("client in (wire)        avg %.0f  p50 %.0f  p99 %.0f B/s per client; %.0f datagrams/s avg; total %.0f B/s over %d clients\n",
                avg(cliBps), Pct(cliBps, 0.5), Pct(cliBps, 0.99), avg(cliDgramPs), (double)sumWireIn / winSecs, clientsMeasured);
    std::printf("client world packets    avg %.1f/s per client (server tick rate %u), min %.1f; entries per packet avg %.1f; rtt avg %.0f ms\n",
                avg(cliWorldPs), opt.external ? 0u : (unsigned)host.cfg.tickHz, cliWorldPs.empty() ? 0.0 : *std::min_element(cliWorldPs.begin(), cliWorldPs.end()),
                avg(cliEntriesPer), avg(cliRtt));
    std::printf("client decode           world entries delta %llu, absolute %llu (%.1f%% delta); coarse entries %.1f/s per client; "
                "REJECTED frames %llu, malformed %llu, stale %llu\n",
                (unsigned long long)sumDelta, (unsigned long long)sumAbs,
                sumDelta + sumAbs ? 100.0 * (double)sumDelta / (double)(sumDelta + sumAbs) : 0.0,
                clientsMeasured ? (double)sumCoarse / winSecs / (double)clientsMeasured : 0.0, (unsigned long long)sumRejected,
                (unsigned long long)sumMalformed, (unsigned long long)sumStale);
    std::printf("process cpu             %.2f cores (server thread + %d client threads + harness), total run %.0f s, %.1f cpu-s\n", procCpu,
                opt.threads, Now() - wallStart, ProcessCpuSeconds() - cpuStart);

    // One machine-readable line per run, for the tables in docs/perf.
    if (!opt.external)
    {
        const double dt = std::max(1e-6, m1.wallSeconds - m0.wallSeconds);
        std::printf("RESULT bots=%d scenario=%s joined=%d lost=%d tick_avg_us=%.0f tick_p99_us=%.0f tick_max_us=%llu missed=%llu cpu_pct=%.1f "
                    "out_payload_Bps=%.0f out_wire_Bps=%.0f dgram_ps=%.0f app_pkt_ps=%.0f cl_out_avg=%.0f cl_out_p99=%.0f "
                    "world_avg_B=%.0f world_p50_B=%.0f world_p99_B=%.0f world_max_B=%llu entries_avg=%.1f entries_max=%llu "
                    "cli_in_avg_Bps=%.0f cli_in_p99_Bps=%.0f cli_world_ps=%.1f in_pkt_ps=%.0f coarse_B_ps=%.0f "
                    "cli_rejected=%llu cli_malformed=%llu delta_pct=%.1f relevant_pairs=%llu\n",
                    opt.bots, scName, c1.joined, c1.lost, m1.tickWorkUs.Mean(&m0.tickWorkUs), m1.tickWorkUs.Quantile(0.99, &m0.tickWorkUs),
                    (unsigned long long)m1.tickWorkUs.max(), (unsigned long long)(m1.ticksMissed - m0.ticksMissed),
                    100.0 * (m1.cpuSeconds - m0.cpuSeconds) / dt, (double)(m1.payloadBytesOut - m0.payloadBytesOut) / dt,
                    (double)(m1.wireBytesOut - m0.wireBytesOut) / dt, (double)(m1.wireDatagramsOut - m0.wireDatagramsOut) / dt,
                    (double)(m1.packetsOut - m0.packetsOut) / dt, m1.clientBytesPerSec.Mean(&m0.clientBytesPerSec),
                    m1.clientBytesPerSec.Quantile(0.99, &m0.clientBytesPerSec), m1.worldBytes.Mean(&m0.worldBytes),
                    m1.worldBytes.Quantile(0.5, &m0.worldBytes), m1.worldBytes.Quantile(0.99, &m0.worldBytes),
                    (unsigned long long)m1.worldBytes.max(), m1.worldEntryCount.Mean(&m0.worldEntryCount),
                    (unsigned long long)m1.worldEntryCount.max(), avg(cliBps), Pct(cliBps, 0.99), avg(cliWorldPs),
                    (double)(m1.packetsIn - m0.packetsIn) / dt, (double)(m1.coarsePayloadBytes - m0.coarsePayloadBytes) / dt,
                    (unsigned long long)sumRejected, (unsigned long long)sumMalformed,
                    sumDelta + sumAbs ? 100.0 * (double)sumDelta / (double)(sumDelta + sumAbs) : 0.0,
                    (unsigned long long)m1.relevantPairs);
    }

    // ---- teardown: workers first (their sessions say goodbye), then the server ----
    stopWorkers.store(true);
    for (auto& w : workers) w->th.join();
    bots.clear();
    if (!opt.external)
    {
        host.stop.store(true);
        host.th.join();
        std::printf("server log: %llu lines", (unsigned long long)host.logLines);
        int shown = 0;
        for (const std::string& l : host.notable)
        {
            if (shown++ == 0) std::printf("; first notable ones:\n");
            if (shown <= 8) std::printf("  %s\n", l.c_str());
        }
        if (shown == 0) std::printf("\n");
    }
    if (sumRejected || sumMalformed)
    {
        std::printf("FAIL: clients rejected %llu world frames and found %llu malformed packets (no loss is simulated, so both must be 0)\n",
                    (unsigned long long)sumRejected, (unsigned long long)sumMalformed);
        return 3;
    }
    return 0;
}
