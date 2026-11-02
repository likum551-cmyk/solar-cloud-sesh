// interest_test: the Replicator (interest management, send rates, bandwidth budget, protocol v3
// delta frames) driven directly, no sockets: a simulated world feeds it player states, a
// WorldDecoder per receiver decodes what it emits, and every claim below is checked against the
// simulation's ground truth. Build target: interest_test (selftest/, never shipped).
//
//   relevance     enter / leave radii, hysteresis, speed-aware radius, no pop-in at 60 m/s
//   override      SetAlwaysRelevant for one receiver, for everybody, for late joiners, and its undoing
//   rates         near 20 Hz, mid 10 Hz, far 5 Hz, idle 1 Hz, wake-up on the first move (Base: the legacy 20 Hz config
//                 that these tests were written against; the shipped defaults since sync plan W7 are 30 Hz, 30/15/10)
//   rates 30 Hz   the shipped defaults (tickHz 30, 30/15/10, budget 36000): send counts per distance, a 32-player crowd's bytes
//   budget       the per-client byte budget is the binding limit, fair across subjects, scales with the setting
//   coarse        the 1 Hz out-of-range channel: rates, precision, hand-over to and from full detail
//   crowd         600 players, 15% packet loss: every decoded entry is exact, no frame refused,
//                 every packet <= the cap, relevance matches the geometry
//
// Exits non-zero on any failed check or a wrong check count.
#include "config.h"
#include "protocol/protocol.h"
#include "protocol/quant.h"
#include "protocol/worldcodec.h"
#include "replication.h"
#include "sync/sampleclock.h"
#include "tickclock.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace gage;
using server::Metrics;
using server::Replicator;
using server::ReplConfig;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got = "")
{
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", got.c_str());
}

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    double uni() { return (double)(next() >> 11) / 9007199254740992.0; }
    double uni(double lo, double hi) { return lo + (hi - lo) * uni(); }
    uint32_t below(uint32_t n) { return (uint32_t)(next() % n); }
};

static std::string Str(double v, const char* fmt = "%.1f")
{
    char b[64];
    std::snprintf(b, sizeof(b), fmt, v);
    return b;
}
static std::string Str(uint64_t v) { return std::to_string(v); }

// What one simulated client has seen, as a real client would build it from the packets.
struct Client
{
    struct First { uint32_t tick = 0; double dist = 0; uint64_t entriesBefore = 0; };
    proto::WorldDecoder dec;
    std::map<uint16_t, proto::WorldEntry> known;      // newest full-detail entry; erased by `left`
    std::map<uint16_t, uint64_t> entries;             // WorldState entries received per subject
    std::map<uint16_t, uint64_t> leftCount;           // frames that listed the subject in `left`
    std::map<uint16_t, First> first;                  // the first entry of each subject
    std::map<uint16_t, proto::CoarseEntry> coarse;    // newest coarse blip
    std::map<uint16_t, uint64_t> coarseCount;
    std::map<uint16_t, uint32_t> coarseLastTick;
    uint64_t worldBytes = 0;
    uint64_t worldPackets = 0;
};

// The idle dead-band as the test states it (an independent statement of the rule in replication.h, not the server's
// code): per axis 2 cm, 2 velocity steps, 36 heading units, the aim 3 dm, everything else on foot equal.
static bool WithinBand(const proto::QState& a, const proto::QState& b)
{
    auto ad = [](int p, int q) { return p > q ? p - q : q - p; };
    if (ad(a.k.x, b.k.x) > 2 || ad(a.k.y, b.k.y) > 2 || ad(a.k.z, b.k.z) > 2) return false;
    if (ad(a.k.vx, b.k.vx) > 2 || ad(a.k.vy, b.k.vy) > 2 || ad(a.k.vz, b.k.vz) > 2) return false;
    int dh = ad(a.k.heading, b.k.heading);
    if (dh > 32768) dh = 65536 - dh;
    if (dh > 36) return false;
    if (ad(a.p.ax, b.p.ax) > 3 || ad(a.p.ay, b.p.ay) > 3 || ad(a.p.az, b.p.az) > 3) return false;
    proto::QPed pa = a.p, pb = b.p;
    pa.ax = pa.ay = pa.az = pb.ax = pb.ay = pb.az = 0;
    return pa == pb;
}

// The simulated world around one Replicator.
struct Sim
{
    ReplConfig cfg;
    Replicator rep;
    Metrics m;
    std::vector<std::unique_ptr<Client>> cl;
    std::vector<proto::Kinematics> truth;
    std::vector<proto::PedState> pedTruth; // v3: the on-foot state of the same sample
    std::vector<bool> present;
    std::vector<std::array<proto::WorldAck, 8>> ackHist;
    uint32_t tick = 0;
    uint64_t nowUs = 1000000;
    Rng rng{42};
    int downLossPct = 0;       // % of server -> client packets lost
    uint32_t ackLag = 1;       // ticks between a frame arriving and the server learning of it (<= 7)
    int dropWorldNext = 0;     // drop the next N WorldState packets to receiver `dropTarget`
    uint16_t dropTarget = 0;
    // mismatches: decoded entries further from the truth than the dead-band allows (always a bug). bandOnly: entries that
    // differ from it but sit inside the band, which is the server replicating the reference of a standing player.
    uint64_t rejected = 0, mismatches = 0, bandOnly = 0, coarseMismatches = 0, dropped = 0, worldPk = 0, coarsePk = 0;
    size_t maxWorldBytes = 0, maxCoarseBytes = 0;
    double maxEntryDist = 0;

    explicit Sim(const ReplConfig& c) : cfg(c)
    {
        rep.Configure(cfg);
        cfg = rep.config(); // what the Replicator actually uses (clamped)
        cl.resize(cfg.maxPlayers);
        truth.resize(cfg.maxPlayers);
        pedTruth.resize(cfg.maxPlayers);
        present.assign(cfg.maxPlayers, false);
        ackHist.resize(cfg.maxPlayers);
    }

    void Add(uint16_t id)
    {
        rep.AddPlayer(id);
        present[id] = true;
        cl[id] = std::make_unique<Client>();
        ackHist[id] = {};
    }
    void Remove(uint16_t id)
    {
        rep.RemovePlayer(id);
        present[id] = false;
    }
    void Put(uint16_t id, float x, float y, float z = 0, float vx = 0, float vy = 0, float vz = 0, float heading = 0)
    {
        proto::Kinematics k;
        k.x = x; k.y = y; k.z = z; k.heading = heading; k.vx = vx; k.vy = vy; k.vz = vz;
        truth[id] = k;
        pedTruth[id] = proto::PedState();
        rep.SetState(id, k, nowUs);
    }
    // Same position as before, a new on-foot state (v3).
    void PutPed(uint16_t id, const proto::PedState& ped)
    {
        pedTruth[id] = ped;
        rep.SetState(id, truth[id], ped, nowUs);
    }
    double Dist(uint16_t a, uint16_t b) const
    {
        const double dx = (double)truth[a].x - truth[b].x, dy = (double)truth[a].y - truth[b].y,
                     dz = (double)truth[a].z - truth[b].z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    void Deliver(uint16_t r, bool coarse, const proto::Bytes& pk)
    {
        if (!coarse && dropWorldNext > 0 && r == dropTarget)
        {
            dropWorldNext--;
            dropped++;
            return;
        }
        if (downLossPct > 0 && (int)rng.below(100) < downLossPct)
        {
            dropped++;
            return;
        }
        Client& C = *cl[r];
        if (coarse)
        {
            proto::CoarseState cs;
            if (!proto::Decode(pk.data(), pk.size(), cs))
            {
                rejected++;
                return;
            }
            coarsePk++;
            maxCoarseBytes = std::max(maxCoarseBytes, pk.size());
            for (const proto::CoarseEntry& e : cs.players)
            {
                float x, y, z;
                proto::DequantizeCoarse(proto::QuantizeCoarse(proto::Quantize(truth[e.id])), x, y, z); // the server's own path
                if (!present[e.id] || e.x != x || e.y != y || e.z != z) coarseMismatches++;
                C.coarse[e.id] = e;
                C.coarseCount[e.id]++;
                C.coarseLastTick[e.id] = tick;
            }
            return;
        }
        proto::WorldState ws;
        if (!C.dec.Decode(pk.data(), pk.size(), ws))
        {
            rejected++;
            return;
        }
        worldPk++;
        C.worldPackets++;
        C.worldBytes += pk.size();
        maxWorldBytes = std::max(maxWorldBytes, pk.size());
        for (uint16_t id : ws.left)
        {
            C.leftCount[id]++;
            C.known.erase(id);
        }
        for (const proto::WorldEntry& e : ws.players)
        {
            if (!present[e.id])
                mismatches++;
            else
            {
                const proto::QState got{proto::Quantize(e.k), proto::QuantizePed(e.ped)};
                const proto::QState want{proto::Quantize(truth[e.id]), proto::QuantizePed(pedTruth[e.id])};
                if (got != want)
                {
                    if (WithinBand(got, want))
                        bandOnly++;
                    else
                        mismatches++;
                }
            }
            if (!C.first.count(e.id)) C.first[e.id] = Client::First{tick, Dist(r, e.id), C.entries[e.id]};
            maxEntryDist = std::max(maxEntryDist, Dist(r, e.id));
            C.known[e.id] = e;
            C.entries[e.id]++;
        }
    }

    // One tick in microseconds: 50000 at the legacy 20 Hz, 33333 at the 30 Hz default (sync plan W7).
    uint64_t TickUs() const { return 1000000u / cfg.tickHz; }
    void StepOnce()
    {
        tick++;
        nowUs += TickUs();
        rep.Tick(tick, nowUs, [this](uint16_t r, bool coarse, const proto::Bytes& pk) { Deliver(r, coarse, pk); }, m);
        for (uint16_t r = 0; r < cfg.maxPlayers; r++)
        {
            if (!present[r]) continue;
            ackHist[r][tick % 8] = cl[r]->dec.ack();
            if (tick > ackLag) rep.SetAck(r, ackHist[r][(tick - ackLag) % 8]);
        }
    }
    void Step(int n = 1)
    {
        for (int i = 0; i < n; i++) StepOnce();
    }
};

// The legacy 20 Hz configuration (tick 20 Hz, rates 20/10/5, budget 24000 B/s) that every test below except the 30 Hz ones
// was written against. Since sync plan W7 the shipped defaults are 30 Hz / 30/15/10 / 36000; Base30 is that.
static ReplConfig Base(uint16_t players)
{
    ReplConfig c;
    c.maxPlayers = players;
    c.tickHz = 20;
    c.rateNearHz = 20;
    c.rateMidHz = 10;
    c.rateFarHz = 5;
    c.budgetBytesPerSec = 24000;
    return c;
}
static ReplConfig Base30(uint16_t players)
{
    ReplConfig c; // ReplConfig's own defaults: tickHz 30, rates 30/15/10, budget 36000
    c.maxPlayers = players;
    return c;
}

// ---- relevance ------------------------------------------------------------------------------------

static void RelevanceTests()
{
    {
        Sim s(Base(3));
        s.Add(0); s.Add(1); s.Add(2);
        s.Put(0, 0, 0); s.Put(1, 300, 0); s.Put(2, 800, 0);
        s.Step(60);
        Client& A = *s.cl[0];
        Check("relevance: a player 300 m away is relevant, one 800 m away is not (both ways)",
              s.rep.Relevant(0, 1) && !s.rep.Relevant(0, 2) && s.rep.Relevant(1, 0) && !s.rep.Relevant(2, 0) &&
                  s.rep.RelevantCount(0) == 1 && s.rep.TotalRelevantPairs() == 2);
        const auto blip = A.coarse.find(2);
        Check("relevance: the near player arrives at full detail, the far one only as a coarse blip within 0.25 m",
              A.known.count(1) && A.known[1].k.x == 300.0f && !A.known.count(2) && blip != A.coarse.end() &&
                  std::fabs(blip->second.x - 800.0f) <= 0.25f && std::fabs(blip->second.z) <= 0.04f && A.coarseCount[1] == 0,
              "known " + Str((uint64_t)A.known.size()) + " blips of 1: " + Str(A.coarseCount[1]));
    }
    {
        Sim s(Base(2));
        s.Add(0); s.Add(1);
        s.Put(0, 0, 0);
        Client& A = *s.cl[0];
        s.Put(1, 420, 0); s.Step(30);
        const bool neverEntered = !s.rep.Relevant(0, 1) && !A.known.count(1);
        s.Put(1, 399, 0); s.Step(10);
        const bool entered = s.rep.Relevant(0, 1) && A.known.count(1);
        s.Put(1, 430, 0); s.Step(30);
        const bool held = s.rep.Relevant(0, 1) && A.known.count(1) && A.leftCount[1] == 0;
        s.Put(1, 451, 0); s.Step(10);
        const bool dropped = !s.rep.Relevant(0, 1) && !A.known.count(1);
        const uint64_t notices = A.leftCount[1];
        s.Put(1, 440, 0); s.Step(30);
        const bool stayedOut = !s.rep.Relevant(0, 1) && !A.known.count(1);
        s.Put(1, 399, 0); s.Step(10);
        const bool back = s.rep.Relevant(0, 1) && A.known.count(1) && A.known[1].k.x == 399.0f;
        Check("relevance: a player at 420 m never enters (enter 400), at 399 enters, holds to 430, leaves past 450, stays out at 440, re-enters at 399",
              neverEntered && entered && held && dropped && stayedOut && back,
              std::string(neverEntered ? "1" : "0") + (entered ? "1" : "0") + (held ? "1" : "0") + (dropped ? "1" : "0") +
                  (stayedOut ? "1" : "0") + (back ? "1" : "0"));
        Check("relevance: the 'left your range' notice goes out in exactly 3 consecutive frames", notices == 3, Str(notices));
    }
    {
        // Hovering at the edge: 395 m and 405 m alternately for 100 ticks. With the 50 m hysteresis nothing flickers;
        // the same run with leave == enter churns (so the first result is the hysteresis, not luck).
        auto churn = [](uint32_t leaveRadius) {
            ReplConfig c = Base(2);
            c.leaveRadius = leaveRadius;
            Sim s(c);
            s.Add(0); s.Add(1);
            s.Put(0, 0, 0);
            s.Put(1, 395, 0);
            s.Step(6);
            const uint64_t enters0 = s.m.relevanceEnters, leaves0 = s.m.relevanceLeaves;
            for (int t = 0; t < 100; t++)
            {
                s.Put(1, t % 2 ? 405.0f : 395.0f, 0);
                s.Step();
            }
            return std::pair<uint64_t, uint64_t>{s.m.relevanceEnters - enters0, s.m.relevanceLeaves - leaves0};
        };
        const auto withHyst = churn(450), without = churn(400);
        Check("relevance: hovering at the edge (395/405 m) causes no enter/leave churn with the 50 m hysteresis",
              withHyst.first == 0 && withHyst.second == 0, Str(withHyst.first) + " enters, " + Str(withHyst.second) + " leaves");
        Check("relevance: the same hovering with leave == enter radius churns (>= 40 leaves), so the hysteresis is what holds it",
              without.second >= 40 && without.first >= 40, Str(without.first) + " enters, " + Str(without.second) + " leaves");
    }
    {
        Sim s(Base(2));
        const Replicator& r = s.rep;
        Check("relevance: enter radius is 400 m at rest, 460 m at a combined 60 m/s, capped at 600 m",
              r.EnterRadius(0, 0) == 400.0f && r.EnterRadius(30, 30) == 460.0f && r.EnterRadius(500, 500) == 600.0f &&
                  r.EnterRadius(60, 0) == r.EnterRadius(0, 60));
        // A subject 450 m away is outside the resting radius but inside the speed-grown one.
        Sim t(Base(3));
        t.Add(0); t.Add(1); t.Add(2);
        t.Put(0, 0, 0);
        t.Put(1, 450, 0, 0, -60, 0, 0);  // closing at 60 m/s
        t.Put(2, 0, 450, 0);             // standing
        t.Step(10);
        Check("relevance: a subject 450 m away is relevant when it is moving at 60 m/s and not when it stands",
              t.rep.Relevant(0, 1) && !t.rep.Relevant(0, 2));
    }
}

// First distance at which `a` is told about `b`, closing head-on from 5 km apart.
struct Approach
{
    bool seen = false;
    double firstDist = 0;
    uint64_t entriesBefore300 = 0;
    double enterRadius = 0;
};
static Approach Closing(float speedA, float speedB, uint32_t lookaheadMs)
{
    ReplConfig c = Base(2);
    c.lookaheadMs = lookaheadMs;
    Sim s(c);
    s.Add(0); s.Add(1);
    Approach out;
    out.enterRadius = s.rep.EnterRadius(speedA, speedB);
    double ax = -2500, bx = 2500;
    bool past300 = false;
    Client& A = *s.cl[0];
    for (int t = 0; t < 3000; t++)
    {
        s.Put(0, (float)ax, 0, 0, speedA, 0, 0);
        s.Put(1, (float)bx, 0, 0, -speedB, 0, 0);
        s.Step();
        const double d = bx - ax;
        if (!past300 && d < 300)
        {
            past300 = true;
            out.entriesBefore300 = A.entries[1];
        }
        ax += speedA / 20.0;
        bx -= speedB / 20.0;
        if (d < 100) break;
    }
    out.seen = A.first.count(1) != 0;
    if (out.seen) out.firstDist = A.first[1].dist;
    return out;
}

static void PopInTests()
{
    struct Case { float a, b; };
    const Case cases[] = {{60, 0}, {0, 60}, {60, 60}, {300, 300}};
    std::string detail;
    bool allOk = true;
    for (const Case& c : cases)
    {
        const Approach r = Closing(c.a, c.b, 1000);
        const double closing = c.a + c.b;
        // The enter test runs every 2nd tick, so a pair can be noticed up to 2 ticks of closing late.
        const double lo = r.enterRadius - closing / 10.0 - 2.0, hi = r.enterRadius + 2.0;
        const bool ok = r.seen && r.firstDist >= lo && r.firstDist <= hi;
        allOk = allOk && ok;
        detail += "[" + Str(c.a, "%.0f") + "+" + Str(c.b, "%.0f") + " m/s: first at " + Str(r.firstDist) + " m, want " + Str(lo, "%.0f") + ".." +
                  Str(hi, "%.0f") + "]";
    }
    Check("pop-in: at 60+0, 0+60, 60+60 and 300+300 m/s the first full-detail entry comes at the speed-grown enter radius (460, 460, 520, 600 m)",
          allOk, detail);
    const Approach early = Closing(60, 0, 1000), late = Closing(60, 0, 0), driver = Closing(0, 60, 1000);
    Check("pop-in: the speed-aware radius is what does it: at 60 m/s the first entry is at >= 440 m, with lookahead off at < 410 m",
          early.seen && late.seen && early.firstDist >= 440 && late.firstDist < 410,
          "lookahead on " + Str(early.firstDist) + " m, off " + Str(late.firstDist) + " m");
    // A driver closing on a standing receiver is moving (5 Hz at this range); a standing player met by a driver is idle (1 Hz).
    Check("pop-in: by the time a 60 m/s driver is 300 m away the client already has >= 10 updates of it (a standing player met at 60 m/s: >= 2)",
          driver.entriesBefore300 >= 10 && early.entriesBefore300 >= 2, Str(driver.entriesBefore300) + " / " + Str(early.entriesBefore300));
}

// ---- the always-relevant override --------------------------------------------------------------------------

static void OverrideTests()
{
    Sim s(Base(8));
    s.Add(0); s.Add(1); s.Add(2);
    Client& R = *s.cl[0];
    Client& Q = *s.cl[2];
    auto place = [&](float sx) {
        s.Put(0, 0, 0);
        s.Put(1, sx, 0, 0, 0, 1, 0);   // moving a little: not idle, so it runs at the rate of its distance class
        s.Put(2, -5000, 0);
    };
    for (int t = 0; t < 40; t++)
    {
        place(5000.0f + 0.05f * t);
        s.Step();
    }
    Check("override: a player 5 km away is not relevant and arrives as a coarse blip only",
          !s.rep.Relevant(0, 1) && !R.known.count(1) && R.coarseCount[1] >= 1 && !s.rep.AlwaysRelevant(1, 0));

    const bool ok = s.rep.SetAlwaysRelevant(1, 0, true);
    Check("override: SetAlwaysRelevant refuses a non-player subject, a non-player receiver and subject == receiver",
          ok && !s.rep.SetAlwaysRelevant(7, 0, true) && !s.rep.SetAlwaysRelevant(1, 7, true) && !s.rep.SetAlwaysRelevant(1, 1, true) &&
              !s.rep.AlwaysRelevant(7, 0) && s.rep.AlwaysRelevant(1, 0) && !s.rep.AlwaysRelevant(1, 2));

    const uint64_t before = R.entries[1];
    for (int t = 0; t < 40; t++)
    {
        place(5002.0f + 0.05f * t);
        s.Step();
    }
    const uint64_t got = R.entries[1] - before;
    Check("override: the forced pair is sent at the far rate (5 Hz: 8..12 entries in 2 s), full detail, and only to its receiver",
          s.rep.Relevant(0, 1) && R.known.count(1) && got >= 8 && got <= 12 && !s.rep.Relevant(2, 1) && !Q.known.count(1),
          Str(got) + " entries");

    for (int t = 0; t < 60; t++)
    {
        place(14000.0f + 0.05f * t);
        s.Step();
    }
    Check("override: a forced player 14 km away is never dropped by the leave radius (no 'left' notice) and still updates",
          s.rep.Relevant(0, 1) && R.leftCount[1] == 0 && R.known.count(1) && R.known[1].k.x >= 14000.0f, Str(R.leftCount[1]));

    // kServerId: everybody, including players who join later.
    const bool all = s.rep.SetAlwaysRelevant(1, proto::kServerId, true);
    s.Add(3);
    for (int t = 0; t < 10; t++)
    {
        place(14003.0f);
        s.Put(3, 0, 8000);
        s.Step();
    }
    Check("override: kServerId makes the player relevant to every receiver, also one who joins afterwards",
          all && s.rep.Relevant(2, 1) && s.rep.Relevant(3, 1) && s.cl[3]->known.count(1) && Q.known.count(1) &&
              s.rep.AlwaysRelevant(1, proto::kServerId) && s.rep.AlwaysRelevant(1, 3));

    // Undo for everybody: the pairs follow the radii again and are dropped (with notices).
    s.rep.SetAlwaysRelevant(1, proto::kServerId, false);
    for (int t = 0; t < 10; t++)
    {
        place(14003.0f);
        s.Put(3, 0, 8000);
        s.Step();
    }
    Check("override: undoing kServerId drops the player for everybody (even the receiver set individually) with 'left' notices",
          !s.rep.Relevant(0, 1) && !s.rep.Relevant(2, 1) && !s.rep.Relevant(3, 1) && R.leftCount[1] == 3 && Q.leftCount[1] == 3 &&
              !R.known.count(1) && !s.rep.AlwaysRelevant(1, 0) && !s.rep.AlwaysRelevant(1, proto::kServerId),
          Str(R.leftCount[1]) + "/" + Str(Q.leftCount[1]));

    // Direct undo for one receiver.
    s.rep.SetAlwaysRelevant(1, 0, true);
    s.Step(6);
    const bool back = s.rep.Relevant(0, 1);
    s.rep.SetAlwaysRelevant(1, 0, false);
    s.Step(6);
    Check("override: SetAlwaysRelevant(.., false) for one receiver sends the pair back to the normal radii",
          back && !s.rep.Relevant(0, 1) && !s.rep.AlwaysRelevant(1, 0));

    // A forced pair of two players standing next to each other is just relevant; removing the subject cleans up.
    s.rep.SetAlwaysRelevant(1, 0, true);
    s.Step(2);
    s.Remove(1);
    const bool purged = !s.rep.Relevant(0, 1) && !s.rep.AlwaysRelevant(1, 0);
    s.Add(1);
    s.Put(1, 14003.0f, 0);
    s.Step(6);
    Check("override: removing the player purges the forced pair, and a reused id starts clean (no override, not relevant)",
          purged && !s.rep.AlwaysRelevant(1, 0) && !s.rep.Relevant(0, 1));
}

// ---- send rates -------------------------------------------------------------------------------------------------

static void RateTests()
{
    Sim s(Base(5));
    for (uint16_t i = 0; i < 5; i++) s.Add(i);
    auto frame = [&](int t) {
        const float y = 0.05f * (float)t;
        s.Put(0, 0, 0);
        s.Put(1, 50, y, 0, 0, 1, 0);   // near
        s.Put(2, 200, y, 0, 0, 1, 0);  // mid
        s.Put(3, 350, y, 0, 0, 1, 0);  // far
        s.Put(4, 60, 0);               // idle: never moves
    };
    int t = 0;
    for (; t < 40; t++)
    {
        frame(t);
        s.Step();
    }
    Client& R = *s.cl[0];
    const uint64_t b1 = R.entries[1], b2 = R.entries[2], b3 = R.entries[3], b4 = R.entries[4];
    for (; t < 140; t++)
    {
        frame(t);
        s.Step();
    }
    const uint64_t n1 = R.entries[1] - b1, n2 = R.entries[2] - b2, n3 = R.entries[3] - b3, n4 = R.entries[4] - b4;
    Check("rates: in 5 s a moving player at 50 m is sent 98..100 times (20 Hz), at 200 m 48..52 (10 Hz), at 350 m 23..27 (5 Hz)",
          n1 >= 98 && n1 <= 100 && n2 >= 48 && n2 <= 52 && n3 >= 23 && n3 <= 27,
          Str(n1) + " / " + Str(n2) + " / " + Str(n3));
    Check("rates: a player standing still at 60 m drops to the idle rate (1 Hz: 4..6 entries in 5 s)", n4 >= 4 && n4 <= 6, Str(n4));

    // The first move of an idle player is sent at once, not at the next idle slot.
    const uint64_t at = R.entries[4];
    s.Put(4, 65, 0);
    s.Step();
    const uint64_t first = R.entries[4] - at;
    s.Step();
    s.Put(4, 70, 0);
    s.Step();
    const uint64_t third = R.entries[4] - at;
    Check("rates: an idle player that starts moving is sent on that very tick, and the next move follows within a tick or two",
          first == 1 && third >= 2, Str(first) + ", " + Str(third));

    // v3: the idle detector compares the whole sample, so a player who stands still and only changes its on-foot
    // state (raises a weapon, swaps it, jumps in place) is sent on that very tick, not at the next 1 Hz idle slot.
    // Six such changes at different phases of the idle cycle. Before each, 12 ticks to fall idle again (after a
    // change the near rate applies for idleAfterMs), then a measured quiet stretch of 8..23 ticks that must carry at
    // most 2 entries (1 Hz): the idle rate is in force when each change lands, so only the detector can explain an
    // entry on that very tick.
    {
        Sim t(Base(2));
        t.Add(0);
        t.Add(1);
        t.Put(0, 0, 0);
        t.Put(1, 60, 0); // stands still for good: only its ped changes from here on
        t.Step(40);
        Client& C = *t.cl[0];
        proto::PedState ped;
        uint64_t immediate = 0, quietMax = 0, wrongPed = 0;
        for (int k = 0; k < 6; k++)
        {
            t.Step(12); // idle again (idleAfterMs 500 = 10 ticks)
            const uint64_t q0 = C.entries[1];
            t.Step(8 + k * 3); // a different phase of the 1 Hz idle cycle each time
            quietMax = std::max<uint64_t>(quietMax, C.entries[1] - q0);
            switch (k % 3)
            {
            case 0: ped.flags ^= proto::kPfAiming; ped.aimX = 12.5f; break;          // flags (+ aim)
            case 1: ped.weapon = 0x1B06D571u + (uint32_t)k; break;                   // weapon only
            case 2: ped.jumpSeq = (uint8_t)((ped.jumpSeq + 1) & 3); break;           // jump counter only
            }
            const uint64_t before = C.entries[1];
            t.PutPed(1, ped);
            t.Step();
            if (C.entries[1] - before == 1) immediate++;
            if (proto::QuantizePed(C.known[1].ped) != proto::QuantizePed(ped)) wrongPed++;
        }
        Check("rates: an idle player whose only change is its on-foot state (aiming flag, weapon, jump counter) is sent on that very tick, 6 of 6, with that state; idle in between (<= 2 entries per quiet stretch)",
              immediate == 6 && wrongPed == 0 && quietMax <= 2 && t.mismatches == 0,
              Str(immediate) + " immediate, quiet max " + Str(quietMax) + ", " + Str(wrongPed) + " wrong ped");
    }

    // Sync plan N4, the idle dead-band: a standing player's sample is never bit-identical for long (the game's velocity
    // reads +-1/32 m/s, a z of one centimetre, a heading of a few units), and a bit-exact idle test turned every such
    // flicker into a wake-up and a 500 ms burst at the near rate. While a sample stays within the band of the sample
    // that was last sent in full (the reference: 2 cm, 2 velocity steps, 36 heading units, 3 dm of aim, the rest of the
    // ped state equal) the idle player keeps its idle rate and the reference is what is replicated (byte-identical).
    {
        Sim t(Base(2));
        t.Add(0);
        t.Add(1);
        Client& C = *t.cl[0];
        t.Put(0, 0, 0);
        t.Put(1, 60, 0, 0, 0, 0, 1.0f / 32.0f);
        t.Step(40); // idle by now
        const uint64_t b0 = C.entries[1];
        std::set<int> vzSeen;
        for (int i = 0; i < 200; i++) // 10 s; vz flips +1/32 <-> -1/32 every 700 ms (14 ticks)
        {
            const float vz = (i / 14) % 2 == 0 ? -1.0f / 32.0f : 1.0f / 32.0f;
            t.Put(1, 60, 0, 0, 0, 0, vz);
            t.Step();
            vzSeen.insert(proto::Quantize(C.known[1].k).vz);
        }
        const uint64_t n = C.entries[1] - b0;
        Check("rates: a standing player whose velocity flickers by +-1/32 m/s every 700 ms stays on the idle rate: <= 25 entries in 10 s (it was ~150), all carrying the same sample",
              n <= 25 && n >= 8 && vzSeen.size() == 1, Str(n) + " entries, " + Str((uint64_t)vzSeen.size()) + " distinct vz");
    }
    {
        // Each case: an idle player, one change that is exactly at the edge of the band (must stay idle: <= 1 entry in the
        // next 8 ticks, only the 1 Hz slot) and one a single step beyond it (must be sent on that very tick and keep the
        // near rate: 7 or more in the next 8). Case 4 is the aim (3 dm / 4 dm) of an aiming player; case 5 swaps the weapon.
        struct Case { const char* what; int kind; bool inBand; };
        const Case cases[] = {
            {"position 2 cm", 0, true},   {"velocity 2 steps", 1, true},  {"heading 36 units", 2, true},  {"aim 3 dm", 3, true},
            {"position 3 cm", 0, false},  {"velocity 3 steps", 1, false}, {"heading 37 units", 2, false}, {"aim 4 dm", 3, false},
            {"weapon", 4, false},
        };
        constexpr float kUnit = 360.0f / 65536.0f;
        std::string quietBad, wakeBad;
        for (const Case& c : cases)
        {
            Sim t(Base(2));
            t.Add(0);
            t.Add(1);
            Client& C = *t.cl[0];
            proto::PedState aiming;
            aiming.flags = proto::kPfAiming;
            aiming.aimX = 12.5f;
            t.Put(0, 0, 0);
            t.Put(1, 60, 0, 0, 0, 0, 0, 90.0f);
            if (c.kind == 3) t.PutPed(1, aiming);
            t.Step(45); // idle
            const uint64_t b = C.entries[1];
            switch (c.kind)
            {
            case 0: t.Put(1, c.inBand ? 60.02f : 60.03f, 0, 0, 0, 0, 0, 90.0f); break;
            case 1: t.Put(1, 60, 0, 0, c.inBand ? 2.0f / 32.0f : 3.0f / 32.0f, 0, 0, 90.0f); break;
            case 2: t.Put(1, 60, 0, 0, 0, 0, 0, 90.0f + (c.inBand ? 36.0f : 37.0f) * kUnit); break;
            case 3: aiming.aimX = c.inBand ? 12.8f : 12.9f; t.PutPed(1, aiming); break;
            case 4: { proto::PedState w; w.weapon = 0x1B06D571u; t.PutPed(1, w); break; }
            }
            t.Step();
            const uint64_t first = C.entries[1] - b;
            t.Step(7);
            const uint64_t eight = C.entries[1] - b;
            if (c.inBand && eight > 1) quietBad += std::string(c.what) + " (" + Str(eight) + " entries) ";
            if (!c.inBand && (first != 1 || eight < 7)) wakeBad += std::string(c.what) + " (" + Str(first) + " at once, " + Str(eight) + " in 8) ";
        }
        Check("rates: changes inside the dead-band (position 2 cm, velocity 2 steps, heading 36 units, aim 3 dm) leave an idle player idle; one step beyond (3 cm, 3 steps, 37 units, 4 dm) or a weapon change is sent on that very tick and wakes it",
              quietBad.empty() && wakeBad.empty(), "stayed awake: " + quietBad + "| did not wake: " + wakeBad);
    }
    {
        // The reference is where the band is measured from, so a creep cannot hide in it: 1 cm every 700 ms is quiet for two
        // steps (1 and 2 cm from the reference) and wakes the player at the third (3 cm).
        Sim t(Base(2));
        t.Add(0);
        t.Add(1);
        Client& C = *t.cl[0];
        t.Put(0, 0, 0);
        t.Put(1, 60, 0);
        t.Step(45);
        const uint64_t b = C.entries[1];
        uint64_t beforeThird = 0;
        for (int i = 1; i <= 50; i++)
        {
            t.Put(1, 60.0f + 0.01f * (float)((i / 14) > 3 ? 3 : (i / 14)), 0);
            t.Step();
            if (i == 41) beforeThird = C.entries[1] - b; // ticks 1..41: still at 1 and 2 cm from the reference
        }
        const uint64_t afterThird = C.entries[1] - b;
        Check("rates: a creep of 1 cm every 700 ms stays quiet for two steps (<= 3 entries by tick 41) and wakes the player at the third, 3 cm from the reference (a burst after tick 42)",
              beforeThird <= 3 && afterThird - beforeThird >= 7, Str(beforeThird) + " by tick 41, " + Str(afterThird) + " by tick 50");
    }
}

// ---- the sync diagnostics counters (sync plan N6 counters) ----------------------------------------------------------

static void CounterTests()
{
    {
        // Three players inside 60 m of each other: 0 stands, 1 moves, 2 stands with a velocity that flickers inside the
        // dead-band every 700 ms and takes one real 1 m step at tick 60. Every relevant pair has exactly one outcome on
        // every tick, "sent" is the WorldState entry count, and the two ways to be deferred add up to the old counter.
        Sim t(Base(3));
        for (uint16_t i = 0; i < 3; i++) t.Add(i);
        uint64_t pairTicks = 0, puts = 0;
        for (int i = 0; i < 100; i++)
        {
            t.Put(0, 0, 0);
            t.Put(1, 50 + 0.05f * (float)i, 0, 0, 1, 0, 0);
            t.Put(2, i >= 60 ? 61.0f : 60.0f, 0, 0, 0, 0, (i / 14) % 2 == 0 ? -1.0f / 32.0f : 1.0f / 32.0f);
            puts += 3;
            t.StepOnce();
            pairTicks += t.rep.TotalRelevantPairs();
        }
        const Metrics& m = t.m;
        const uint64_t outcomes = m.pairSent + m.pairIdleSuppressed + m.pairRateNotDue + m.pairBudgetDeferred + m.pairPacketCapDeferred + m.pairNoState;
        Check("counters: every relevant pair has exactly one outcome per tick (sent, idle-suppressed, rate-not-due, budget-deferred, packet-cap-deferred, no-state); sent equals the WorldState entries; the two deferrals add up to budgetDeferred",
              pairTicks > 0 && outcomes == pairTicks && m.pairSent == m.worldEntries && m.pairSent > 0 && m.pairIdleSuppressed > 0 &&
                  m.pairBudgetDeferred + m.pairPacketCapDeferred == m.budgetDeferred,
              Str(outcomes) + " outcomes for " + Str(pairTicks) + " pair-ticks, sent " + Str(m.pairSent) + " of " + Str(m.worldEntries) + " entries, idle-suppressed " +
                  Str(m.pairIdleSuppressed));
        Check("counters: one real wake-up (the 1 m step) and the flicker's wake-ups the bit-exact rule would have caused counted apart (>= 2, one per 700 ms); every sample counted once; forwarded never exceeds received",
              m.wakes == 1 && m.noiseWakesHeld >= 2 && m.samplesReceived == puts && m.samplesForwarded > 0 && m.samplesForwarded <= m.samplesReceived,
              Str(m.wakes) + " wakes, " + Str(m.noiseWakesHeld) + " held, " + Str(m.samplesReceived) + " received of " + Str(puts) + ", " + Str(m.samplesForwarded) + " forwarded");
    }
    {
        // A sender that goes quiet after one sample: the same sample is sent again on every later tick it is due.
        Sim t(Base(2));
        t.Add(0);
        t.Add(1);
        t.Put(0, 0, 0);
        t.Put(1, 60, 0);
        t.Step(100);
        Check("counters: a sample the sender never replaced is counted as forwarded once and forwarded-twice on every later send (idle refreshes)",
              t.m.samplesForwarded >= 2 && t.m.samplesForwardedTwice >= 8 && t.m.samplesReceived == 2,
              Str(t.m.samplesForwarded) + " forwarded, " + Str(t.m.samplesForwardedTwice) + " twice, " + Str(t.m.samplesReceived) + " received");
    }
}

// ---- a server stall: tick numbers follow the wall clock (sync plan N6c) -------------------------------------------

static void TickJumpTests()
{
    // The history mask of a pair says "sent k ticks ago". When the server stalls and the tick number jumps by 14, the
    // mask has to move by 14 or the frame sent just before the stall is no longer found as a baseline (shifted by one it
    // would name tick-1, which was never acknowledged): every entry after a stall went out in full, and the baseline
    // the client still holds was thrown away.
    Sim t(Base(2));
    t.Add(0);
    t.Add(1);
    for (int i = 0; i < 12; i++)
    {
        t.Put(0, 0.05f * (float)i, 0, 0, 1, 0, 0);
        t.Put(1, 50 + 0.05f * (float)i, 0, 0, 1, 0, 0);
        t.Step();
    }
    const uint64_t d0 = t.m.deltaEntries, a0 = t.m.absoluteEntries;
    for (uint16_t r = 0; r < 2; r++) t.rep.SetAck(r, t.cl[r]->dec.ack()); // both clients hold the newest frame
    t.tick += 13; // the server was away for 14 tick periods
    t.nowUs += 13 * t.TickUs();
    t.Put(0, 0.05f * 12, 0, 0, 1, 0, 0);
    t.Put(1, 50 + 0.05f * 12, 0, 0, 1, 0, 0);
    t.StepOnce();
    const uint64_t dd = t.m.deltaEntries - d0, da = t.m.absoluteEntries - a0;
    Check("stall: after the tick number jumps by 14 the frame sent just before it is still the baseline: both entries are deltas (the history mask moves with the tick number)",
          dd == 2 && da == 0 && t.rejected == 0 && t.mismatches == 0, Str(dd) + " delta, " + Str(da) + " absolute, " + Str(t.rejected) + " refused");

    // The schedule itself (apps/server/tickclock.h): tick numbers follow the wall clock, a tick is stamped with the
    // time of its own slot (what the sample ages are measured against), and lateness / skipped periods are reported.
    {
        server::TickClock c;
        c.Start(1000000, 50000);
        const bool early = c.Due(1049999), onTime = c.Due(1050000);
        const server::TickClock::Slot s1 = c.Advance(1050000); // on time
        const server::TickClock::Slot s2 = c.Advance(1107000); // due at 1100000, 7 ms late
        const server::TickClock::Slot s3 = c.Advance(1700300); // due at 1150000, 550.3 ms late: 11 whole periods missed
        const server::TickClock::Slot s4 = c.Advance(1750000);
        Check("stall: TickClock numbers ticks from 1 at start + one period, stamps each with its own slot (late by 7 ms: the slot, not the clock), and after a 550 ms stall jumps 12 ticks (11 missed) to the slot of the wall time",
              !early && onTime && s1.tick == 1 && s1.nominalUs == 1050000 && s1.lateUs == 0 && s1.missed == 0 && s2.tick == 2 &&
                  s2.nominalUs == 1100000 && s2.lateUs == 7000 && s2.missed == 0 && s3.tick == 14 && s3.nominalUs == 1700000 &&
                  s3.lateUs == 550300 && s3.missed == 11 && c.NextDueUs() == 1800000 && s4.tick == 15 && s4.nominalUs == 1750000 && s4.missed == 0,
              "ticks " + Str((uint64_t)s1.tick) + "," + Str((uint64_t)s2.tick) + "," + Str((uint64_t)s3.tick) + "," + Str((uint64_t)s4.tick) +
                  " nominal " + Str(s3.nominalUs) + " missed " + Str((uint64_t)s3.missed) + " next " + Str(c.NextDueUs()));
    }
}

// ---- the bandwidth budget ---------------------------------------------------------------------------------------

// `n` players circling inside 40 m of the receiver (everybody relevant, everybody due every tick);
// returns bytes sent to receiver 0 over `measure` ticks after a `warm` tick warm-up and fills `perSubject`.
// `hz` 20 runs the legacy configuration (Base), 30 the shipped defaults (Base30; `budget` still overrides its budget).
// The angular speed is per second, so both rates move the players at the same 16-20 m/s.
static uint64_t CircleRun(uint32_t budget, uint32_t packetMax, uint16_t n, std::vector<uint64_t>* perSubject, Sim** keep = nullptr,
                          int warm = 40, int measure = 200, uint16_t hz = 20)
{
    ReplConfig c = hz == 20 ? Base(n) : Base30(n);
    c.budgetBytesPerSec = budget;
    c.packetMaxBytes = packetMax;
    auto s = std::make_unique<Sim>(c);
    for (uint16_t i = 0; i < n; i++) s->Add(i);
    int t = 0;
    auto frame = [&](int tt) {
        for (uint16_t i = 0; i < n; i++)
        {
            const double w0 = 0.02 + 0.0001 * i; // rad per 20 Hz tick: about 16-20 m/s on a 40 m circle
            const double w = hz == 20 ? w0 : w0 * 20.0 / hz;
            const double a = w * tt + i;
            const float speed = (float)(40.0 * w * hz);
            s->Put(i, (float)(40.0 * std::cos(a)), (float)(40.0 * std::sin(a)), 0, (float)(-std::sin(a) * speed),
                   (float)(std::cos(a) * speed), 0, (float)std::fmod(a * 57.29578 + 90.0, 360.0));
        }
    };
    for (; t < warm; t++)
    {
        frame(t);
        s->Step();
    }
    Client& R = *s->cl[0];
    const uint64_t bytes0 = R.worldBytes;
    std::vector<uint64_t> e0(n);
    for (uint16_t i = 1; i < n; i++) e0[i] = R.entries[i];
    for (; t < warm + measure; t++)
    {
        frame(t);
        s->Step();
    }
    if (perSubject)
    {
        perSubject->assign(n, 0);
        for (uint16_t i = 1; i < n; i++) (*perSubject)[i] = R.entries[i] - e0[i];
    }
    const uint64_t bytes = R.worldBytes - bytes0;
    if (keep) *keep = s.release();
    return bytes;
}

static void BudgetTests()
{
    std::vector<uint64_t> per8, per16;
    Sim* keep = nullptr;
    const uint64_t b8 = CircleRun(8000, 1200, 250, &per8, &keep);
    const uint64_t deferred = keep->m.budgetDeferred;
    const double rejected8 = (double)keep->rejected + (double)keep->mismatches;
    delete keep;
    const uint64_t b16 = CircleRun(16000, 1200, 250, &per16, nullptr);
    // 200 ticks = 10 s. The bucket may be full at the start of the window (cap max(1200, 2 x 400)).
    Check("budget: 249 players due every tick against an 8 kB/s budget: the client receives 8 kB/s (68..82 kB in 10 s), not what is due",
          b8 >= 68000 && b8 <= 82400 && deferred > 0 && rejected8 == 0, Str(b8) + " B, deferred " + Str(deferred));
    Check("budget: doubling the budget to 16 kB/s doubles the bytes (1.8x..2.2x), so the budget is the binding limit",
          b16 >= (uint64_t)(1.8 * (double)b8) && b16 <= (uint64_t)(2.2 * (double)b8), Str(b16) + " B vs " + Str(b8) + " B");
    uint64_t lo = ~0ull, hi = 0, sum = 0;
    int counted = 0;
    for (size_t i = 1; i < per8.size(); i++)
    {
        lo = std::min(lo, per8[i]);
        hi = std::max(hi, per8[i]);
        sum += per8[i];
        counted++;
    }
    const double mean = (double)sum / counted;
    Check("budget: scheduling is fair: every one of the 249 players gets 0.6x..1.6x the mean share of updates (nobody starves)",
          counted == 249 && lo >= (uint64_t)(0.6 * mean) && (double)hi <= 1.6 * mean && lo > 0,
          "min " + Str(lo) + " mean " + Str(mean) + " max " + Str(hi));

    // Packet cap: with a huge budget the 1200-byte packet limit binds, and the packets are full.
    Sim* big = nullptr;
    CircleRun(1000000, 1200, 400, nullptr, &big, 10, 20);
    const size_t maxPk = big->maxWorldBytes;
    const bool clean = big->rejected == 0 && big->mismatches == 0;
    const uint64_t pk = big->worldPk;
    delete big;
    Check("budget: with an ample budget no WorldState packet exceeds 1200 bytes and the packets are full (>= 1100: the cap binds), all decode exactly",
          maxPk <= 1200 && maxPk >= 1100 && clean && pk > 0, Str((uint64_t)maxPk) + " B max");
}

// ---- the 30 Hz defaults (sync plan N3, W7) ------------------------------------------------------------------------
//
// Everything above runs the legacy 20 Hz configuration (Base). These run what a server without a server.json uses now:
// tickHz 30, rates 30 / 15 / 10, budget 36000 B/s (ReplConfig's own defaults, server::Config's tickHz).

static void Rates30Tests()
{
    {
        Sim s(Base30(5));
        for (uint16_t i = 0; i < 5; i++) s.Add(i);
        auto frame = [&](int t) {
            const float y = (1.0f / 30.0f) * (float)t; // 1 m/s
            s.Put(0, 0, 0);
            s.Put(1, 50, y, 0, 0, 1, 0);   // near
            s.Put(2, 200, y, 0, 0, 1, 0);  // mid
            s.Put(3, 350, y, 0, 0, 1, 0);  // far
            s.Put(4, 60, 0);               // idle: never moves
        };
        int t = 0;
        for (; t < 60; t++)
        {
            frame(t);
            s.Step();
        }
        Client& R = *s.cl[0];
        const uint64_t b1 = R.entries[1], b2 = R.entries[2], b3 = R.entries[3], b4 = R.entries[4];
        for (; t < 210; t++) // 150 ticks = 5 s
        {
            frame(t);
            s.Step();
        }
        const uint64_t n1 = R.entries[1] - b1, n2 = R.entries[2] - b2, n3 = R.entries[3] - b3, n4 = R.entries[4] - b4;
        Check("rates 30 Hz: with the default config a moving player is sent 148..150 times in 5 s at 50 m (30 Hz), 73..77 at 200 m (15 Hz), "
              "48..52 at 350 m (10 Hz); a standing one at 60 m drops to the 1 Hz idle rate (4..6)",
              s.TickUs() == 33333 && n1 >= 148 && n1 <= 150 && n2 >= 73 && n2 <= 77 && n3 >= 48 && n3 <= 52 && n4 >= 4 && n4 <= 6 &&
                  s.mismatches == 0 && s.rejected == 0,
              Str(n1) + " / " + Str(n2) + " / " + Str(n3) + ", idle " + Str(n4));
    }
    {
        const server::Config sc; // what apps/server/config.h hands Server::Start
        const ReplConfig d;
        Check("rates 30 Hz: the defaults are tickHz 30, rates 30 / 15 / 10 (every 1st / 2nd / 3rd tick), budget 36000 B/s, and the 32-tick baseline "
              "history spans at least one second (kMaxBaseAge >= tickHz)",
              sc.tickHz == 30 && d.tickHz == 30 && d.rateNearHz == 30 && d.rateMidHz == 15 && d.rateFarHz == 10 && d.budgetBytesPerSec == 36000 &&
                  proto::kWorldHistory >= (uint32_t)sc.tickHz && proto::kMaxBaseAge >= sc.tickHz,
              "tick " + Str((uint64_t)sc.tickHz) + ", history " + Str((uint64_t)proto::kWorldHistory) + " ticks = " +
                  Str(1000.0 * proto::kWorldHistory / sc.tickHz, "%.0f") + " ms");
    }
    {
        // 32 players circling within 40 m of the receiver, everybody relevant and due every tick (the worst crowd a 32-slot
        // server can show one client), 10 s after a 2 s warm-up, no loss. The 36000 B/s budget is far above the need, so the
        // rate and the delta coding decide the bytes: that is the number an operator reads.
        std::vector<uint64_t> per30, per20;
        Sim* k30 = nullptr;
        const uint64_t b30 = CircleRun(36000, 1200, 32, &per30, &k30, 60, 300, 30);
        const uint64_t deferred = k30->m.budgetDeferred;
        const uint64_t deltas = k30->m.deltaEntries, entries = k30->m.worldEntries;
        const bool clean = k30->rejected == 0 && k30->mismatches == 0;
        const uint64_t pk30 = k30->worldPk;
        delete k30;
        uint64_t lo = ~0ull;
        for (size_t i = 1; i < per30.size(); i++) lo = std::min(lo, per30[i]);
        Sim* k20 = nullptr;
        const uint64_t b20 = CircleRun(24000, 1200, 32, &per20, &k20, 40, 200, 20);
        delete k20;
        const double kbs30 = (double)b30 / 10.0 / 1000.0, kbs20 = (double)b20 / 10.0 / 1000.0;
        Check("rates 30 Hz: 32 players all within 40 m of one client at 30 Hz cost that client <= 14 kB/s of WorldState payload (10 s), at least 90% of "
              "the entries are deltas, every one of the 31 others is sent ~every tick (>= 290 of 300), nothing deferred by the budget",
              b30 > 0 && kbs30 <= 14.0 && entries > 0 && (double)deltas >= 0.9 * (double)entries && lo >= 290 && deferred == 0 && clean && pk30 >= 300,
              Str(kbs30, "%.2f") + " kB/s (" + Str((uint64_t)b30) + " B), " + Str(entries > 0 ? 100.0 * (double)deltas / (double)entries : 0.0, "%.1f") +
                  "% delta, min " + Str(lo) + " entries, deferred " + Str(deferred) + "; same crowd at the legacy 20 Hz: " + Str(kbs20, "%.2f") + " kB/s");
    }
}

// ---- the coarse channel -----------------------------------------------------------------------------------------

static void CoarseTests()
{
    Sim s(Base(8));
    s.Add(0); s.Add(1); s.Add(2);
    Client& R = *s.cl[0];
    s.Put(0, 0, 0);
    s.Put(2, 0, 3000);
    for (int t = 0; t < 200; t++)
    {
        s.Put(1, 2000.0f + (float)t, 0, 0, 20, 0, 0); // moves one metre per tick: its 0.5 m cell changes every visit
        s.Step();
    }
    const uint64_t moving = R.coarseCount[1], standing = R.coarseCount[2];
    Check("coarse: in 10 s a moving far player is on the coarse channel 9..11 times (1 Hz), a standing one 2..3 times (first sight + 5 s refresh)",
          moving >= 9 && moving <= 11 && standing >= 2 && standing <= 3 && !R.known.count(1) && !R.known.count(2),
          Str(moving) + " / " + Str(standing));
    Check("coarse: every blip is the coarse quantisation of the player's true position at that tick (x, y 0.5 m, z 5/64 m)",
          s.coarseMismatches == 0 && s.rejected == 0 && R.coarse.count(1) && R.coarse.count(2), Str(s.coarseMismatches));

    // Hand-over: the moving player comes within 100 m, coarse stops, full detail starts; it leaves, coarse resumes.
    const uint64_t c0 = R.coarseCount[1];
    for (int t = 0; t < 10; t++)
    {
        s.Put(1, 100, 0);
        s.Step();
    }
    const uint64_t c1 = R.coarseCount[1];
    for (int t = 0; t < 60; t++)
    {
        s.Put(1, 100, 0);
        s.Step();
    }
    const bool silentWhileRelevant = R.coarseCount[1] == c1 && R.known.count(1) && s.rep.Relevant(0, 1);
    for (int t = 0; t < 30; t++)
    {
        s.Put(1, 2000, 0);
        s.Step();
    }
    Check("coarse: a player that becomes relevant leaves the coarse channel, one that stops being relevant is back on it within 1.5 s",
          silentWhileRelevant && !R.known.count(1) && R.coarseCount[1] > c1 && c1 >= c0,
          Str(c0) + " " + Str(c1) + " " + Str(R.coarseCount[1]));

    // A receiver that has not sent a state yet gets no WorldState, only blips; a subject without state is invisible.
    Sim t(Base(4));
    t.Add(0); t.Add(1); t.Add(2);
    t.Put(1, 100, 0);
    t.Put(2, 3000, 0);
    t.Step(60);
    Client& N = *t.cl[0];
    const bool mute = N.dec.framesDecoded() == 0 && N.known.empty() && N.coarse.count(1) && N.coarse.count(2) && !t.rep.Relevant(1, 0);
    t.Put(0, 0, 0);
    t.Step(6);
    Check("coarse: a receiver with no state yet gets only blips (even of a player 100 m away), and is itself invisible; once it has a state it joins in",
          mute && t.rep.Relevant(0, 1) && t.rep.Relevant(1, 0) && N.known.count(1));
}

// ---- lifecycle, loss and configuration ------------------------------------------------------------------------

static void LifecycleTests()
{
    {
        // A lost frame cannot lose a 'left' notice (it is repeated), and cannot corrupt a delta (never acknowledged).
        Sim s(Base(2));
        s.Add(0); s.Add(1);
        s.Put(0, 0, 0);
        s.Put(1, 100, 0);
        s.Step(10);
        s.dropTarget = 0;
        s.dropWorldNext = 1;
        s.Put(1, 460, 0); // leaves: the first notice frame is lost
        s.Step(6);
        Client& A = *s.cl[0];
        Check("lifecycle: when the first 'left' frame is lost the repeats still reach the client (2 of 3 arrive)",
              !s.rep.Relevant(0, 1) && !A.known.count(1) && A.leftCount[1] == 2 && s.dropped == 1, Str(A.leftCount[1]));
    }
    {
        // Id reuse inside the 32-tick history window: the new player must be sent in full, never as a delta of the old.
        Sim s(Base(2));
        s.Add(0); s.Add(1);
        s.Put(0, 0, 0);
        for (int t = 0; t < 12; t++)
        {
            s.Put(1, 100.0f + (float)t, 50, 0, 20, 0, 0);
            s.Step();
        }
        s.Remove(1);
        s.Step(2);
        s.Add(1);
        for (int t = 0; t < 12; t++)
        {
            s.Put(1, -150.0f + (float)t, -60, 3, 20, 0, 0);
            s.Step();
        }
        Client& A = *s.cl[0];
        Check("lifecycle: a reused id is sent in full and decodes exactly (no stale baseline from the previous owner)",
              s.mismatches == 0 && s.rejected == 0 && A.known.count(1) && A.known[1].k.y == -60.0f && A.known[1].k.x < 0, Str(s.mismatches));
    }
    {
        // Configure clamps what Validate() would also refuse.
        ReplConfig c = Base(5000);
        c.enterRadius = 500;
        c.leaveRadius = 300;
        c.maxRadius = 100;
        c.cellSize = 10;
        c.packetMaxBytes = 5;
        c.coarseSharePercent = 99;
        Replicator r;
        r.Configure(c);
        ReplConfig d = Base(0);
        d.packetMaxBytes = 9000;
        Replicator r2;
        r2.Configure(d);
        const ReplConfig& a = r.config();
        Check("config: Configure clamps maxPlayers to 1..1024, leave >= enter, maxRadius >= enter, cell >= 64, packet cap 64..1400, coarse share <= 90",
              a.maxPlayers == 1024 && a.leaveRadius == 500 && a.maxRadius == 500 && a.cellSize == 64 && a.packetMaxBytes == 64 &&
                  a.coarseSharePercent == 90 && r2.config().maxPlayers == 1 && r2.config().packetMaxBytes == 1400);
    }
}

// ---- the crowd --------------------------------------------------------------------------------------------------------

static void CrowdTest()
{
    constexpr uint16_t N = 600;
    ReplConfig c = Base(N);
    Sim s(c);
    s.downLossPct = 15;
    s.ackLag = 2;
    struct Mover { double x, y, heading, speed; };
    std::vector<Mover> mv(N);
    Rng g(7);
    const double cityX[4] = {-1000, 1000, -1000, 1000}, cityY[4] = {-1000, -1000, 1000, 1000};
    for (uint16_t i = 0; i < N; i++)
    {
        s.Add(i);
        if (i < 360)
        {
            const double a = g.uni(0, 6.2832), r = 250.0 * std::sqrt(g.uni());
            mv[i].x = cityX[i % 4] + r * std::cos(a);
            mv[i].y = cityY[i % 4] + r * std::sin(a);
        }
        else
        {
            mv[i].x = g.uni(-3000, 3000);
            mv[i].y = g.uni(-3000, 3000);
        }
        mv[i].heading = g.uni(0, 6.2832);
        mv[i].speed = i % 3 == 0 ? g.uni(10, 30) : g.uni(0, 2); // a third drive, the rest walk or stand
    }
    for (int t = 0; t < 300; t++)
    {
        for (uint16_t i = 0; i < N; i++)
        {
            Mover& m = mv[i];
            m.heading += g.uni(-0.1, 0.1);
            m.x += std::cos(m.heading) * m.speed / 20.0;
            m.y += std::sin(m.heading) * m.speed / 20.0;
            if (std::fabs(m.x) > 3500) { m.heading = 3.14159 - m.heading; m.x = std::clamp(m.x, -3500.0, 3500.0); }
            if (std::fabs(m.y) > 3500) { m.heading = -m.heading; m.y = std::clamp(m.y, -3500.0, 3500.0); }
            s.Put(i, (float)m.x, (float)m.y, (float)(10 + 0.5 * std::sin(0.1 * t + i)), (float)(std::cos(m.heading) * m.speed),
                  (float)(std::sin(m.heading) * m.speed), 0, (float)std::fmod(m.heading * 57.29578 + 720.0, 360.0));
        }
        s.Step();
    }
    const uint64_t entries = s.m.worldEntries, deltas = s.m.deltaEntries, absolutes = s.m.absoluteEntries;
    // W4/N4: an entry may carry the reference of a standing player instead of the newest sample, so "equals the
    // quantised state" became "equals it or sits inside the dead-band of it" (bandOnly counts the second kind: it must
    // happen, the slow walkers creep inside the band, and it must stay rare).
    Check("crowd: 600 players, 15% packet loss, 300 ticks: every decoded entry equals the server's quantised state or sits inside its dead-band (0.01%..2% of them), no frame refused",
          entries > 20000 && s.mismatches == 0 && s.bandOnly > entries / 10000 && s.bandOnly < entries / 50 && s.rejected == 0 &&
              s.dropped > 1000 && s.coarseMismatches == 0 && s.coarsePk > 100,
          Str(entries) + " entries, " + Str(s.mismatches) + " wrong, " + Str(s.bandOnly) + " in-band, " + Str(s.rejected) + " refused, " + Str(s.dropped) + " lost");
    Check("crowd: 90%+ of the entries are deltas against an acknowledged baseline (loss and a 2-tick ack lag included)",
          deltas + absolutes == entries && (double)deltas >= 0.9 * (double)entries, Str(deltas) + " deltas of " + Str(entries));
    Check("crowd: no packet exceeds 1200 bytes (so none is fragmented), WorldState or WorldCoarse",
          s.maxWorldBytes <= 1200 && s.maxCoarseBytes <= 1200 && s.maxWorldBytes > 600, Str((uint64_t)s.maxWorldBytes) + " / " + Str((uint64_t)s.maxCoarseBytes));

    // Geometry: nobody is sent beyond the leave radius, nobody within the resting enter radius is missing.
    int missing = 0, extra = 0;
    uint64_t checkedNear = 0, checkedRel = 0;
    for (uint16_t r = 0; r < N; r++)
        for (uint16_t q = 0; q < N; q++)
        {
            if (r == q) continue;
            const double d = s.Dist(r, q);
            if (d <= 390.0)
            {
                checkedNear++;
                if (!s.rep.Relevant(r, q)) missing++;
            }
            if (s.rep.Relevant(r, q))
            {
                checkedRel++;
                if (d > 650.0 + 0.5) extra++;
            }
        }
    Check("crowd: every pair within 390 m is relevant, no relevant pair is beyond the 650 m cap, no entry was ever sent beyond it",
          checkedNear > 5000 && checkedRel > 5000 && missing == 0 && extra == 0 && s.maxEntryDist <= 650.5,
          Str(checkedNear) + " near pairs (" + Str((uint64_t)missing) + " missing), " + Str(checkedRel) + " relevant (" + Str((uint64_t)extra) +
              " too far), farthest entry " + Str(s.maxEntryDist) + " m");
}

// ---- timeline: the sender's sample clock through the Replicator (protocol v5) -----------------------------------

// One sender walks at 4.5 m/s and sends a sample every 22.2 ms (a 45 Hz client); each arrives 30 ms +- 12 ms later. The
// sample index is the x position (0.1 m per sample), so the client can tell which sample an entry carries and what time
// that sample was really taken. The check is on `ageMs`, the only timeline a remote client gets: the nominal tick time of the
// frame minus ageMs is where the client puts the sample on its own timeline. `useSampleClock` feeds the Replicator the
// sample time SampleClock places (what the v5 server does) or the receipt time (what the server did up to v4).
// Returns the standard deviation of (placed time - true sample time) in ms and the number of entries measured.
struct TimelineResult
{
    double sd = 0;
    int n = 0;
};

static TimelineResult RunTimeline(bool useSampleClock)
{
    struct Ev { uint64_t trueUs, recvUs; uint16_t stamp; int index; };
    Sim s(Base(2));
    s.Add(0);
    s.Add(1);
    s.Put(1, 20.0f, 0.0f); // the observer, 20 m from the sender's path
    const uint64_t t0 = s.nowUs;
    Rng rng(0x7117);
    std::vector<Ev> evs;
    for (int i = 0; i < 400; i++)
    {
        Ev e;
        e.index = i;
        e.trueUs = t0 + (uint64_t)i * 22200ull;
        e.recvUs = e.trueUs + (uint64_t)(30000.0 + rng.uni(-12000.0, 12000.0));
        e.stamp = (uint16_t)((10000ull + (uint64_t)i * 22200ull / 1000ull) & 0xFFFFu); // whole ms on the sender's own clock
        evs.push_back(e);
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.recvUs < b.recvUs; });

    sync::SampleClock clock;
    size_t next = 0;
    uint64_t seen = 0;
    std::vector<double> err;
    for (int step = 0; step < 150; step++)
    {
        const uint64_t upTo = s.nowUs + s.TickUs(); // the tick StepOnce is about to run
        while (next < evs.size() && evs[next].recvUs <= upTo)
        {
            const Ev& e = evs[next++];
            proto::Kinematics k;
            k.x = (float)e.index * 0.1f;
            k.vx = 4.5f;
            const uint64_t at = useSampleClock ? clock.Place(e.stamp, e.recvUs) : e.recvUs;
            s.truth[0] = k;
            s.rep.SetState(0, k, at);
        }
        s.StepOnce();
        Client& C = *s.cl[1];
        const uint64_t cnt = C.entries[0];
        if (cnt == seen) continue;
        seen = cnt;
        if (step < 10) continue; // the clock anchors on the first samples
        const auto it = C.known.find(0);
        if (it == C.known.end()) continue;
        const int idx = (int)std::lround(it->second.k.x / 0.1f);
        const double trueUs = (double)t0 + (double)idx * 22200.0;
        const double placedUs = (double)s.nowUs - (double)it->second.ageMs * 1000.0;
        err.push_back((placedUs - trueUs) / 1000.0);
    }
    TimelineResult r;
    r.n = (int)err.size();
    if (err.empty()) return r;
    double mean = 0;
    for (double e : err) mean += e;
    mean /= (double)err.size();
    double var = 0;
    for (double e : err) var += (e - mean) * (e - mean);
    r.sd = std::sqrt(var / (double)err.size());
    return r;
}

static void TimelineTests()
{
    const TimelineResult placed = RunTimeline(true);
    const TimelineResult receipt = RunTimeline(false);
    Check("timeline: with the sender's sample clock the ageMs of an entry places the sample within 1.5 ms (stddev) of when it was taken; "
          "placing it at receipt time (up to v4) is about 7 ms off",
          placed.n >= 100 && receipt.n >= 100 && placed.sd <= 1.5 && receipt.sd > 5.0,
          Str((uint64_t)placed.n) + " entries, sample clock " + Str(placed.sd, "%.2f") + " ms, receipt " + Str(receipt.sd, "%.2f") + " ms");
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("-- relevance\n");
    RelevanceTests();
    std::printf("-- pop-in\n");
    PopInTests();
    std::printf("-- always-relevant override\n");
    OverrideTests();
    std::printf("-- send rates\n");
    RateTests();
    std::printf("-- tick numbers across a stall\n");
    TickJumpTests();
    std::printf("-- sync diagnostics counters\n");
    CounterTests();
    std::printf("-- sample timeline (v5)\n");
    TimelineTests();
    std::printf("-- bandwidth budget\n");
    BudgetTests();
    std::printf("-- 30 Hz defaults\n");
    Rates30Tests();
    std::printf("-- coarse channel\n");
    CoarseTests();
    std::printf("-- lifecycle and configuration\n");
    LifecycleTests();
    std::printf("-- crowd\n");
    CrowdTest();

    const int expected = 50; // v2: 37; v3 adds the on-foot idle-detector check (38); W4: 3 dead-band + 2 stall + 3 counter checks; W5: +1 timeline; W7: +3 (30 Hz rates, defaults, 32-player crowd)
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected)
    {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
