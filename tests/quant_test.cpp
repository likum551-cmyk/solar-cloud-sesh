// quant_test: protocol v4 quantisation (v2 kinematics + v3 on-foot state + v4 shot / vitals and the vehicle rotation,
// angular velocity and steering), the range check and the delta codec, no sockets, no server.
// Build target: quant_test (selftest/, never shipped).
//
//   position   every centimetre of the box survives int -> float -> int (exhaustive); any float in the box
//              comes back within 0.0055 m per axis (on foot, whole box, map edge)
//   heading, velocity, coarse   error bounds, wrap-around, saturation, NaN
//   range      every field of PlayerState and of an absolute WorldState entry is refused one step outside the
//              quantiser box and accepted on its edge; a delta that leaves the box is refused
//   delta      a long random walk over a lossy, reordering link is reproduced bit for bit; sizes; fallback to absolute
//   acks       AckedAges, WorldDecoder receipts, stale / duplicate / unresolved-baseline frames, leave ids
//   hostile    random and mutated packets never crash a decoder, refusals change nothing
//   vehicle    (v4) smallest-three rotation error over a million rotations, wire validity, heading <-> yaw,
//              angular velocity and steering steps
//
// Exits non-zero on any failed check or a wrong check count.
#include "protocol/protocol.h"
#include "protocol/qwire.h"
#include "protocol/quant.h"
#include "protocol/wire.h"
#include "protocol/worldcodec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace gage;
using namespace gage::proto;

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

static std::string Num(double v, const char* fmt = "%.6f")
{
    char b[64];
    std::snprintf(b, sizeof(b), fmt, v);
    return b;
}
static std::string Num(int64_t v) { return std::to_string(v); }

static QKin Q(int32_t x, int32_t y = 0)
{
    QKin q;
    q.x = x;
    q.y = y;
    return q;
}

// ---- 1. position -----------------------------------------------------------------------------

static void PositionTests()
{
    // Exhaustive lattice: a server that re-quantises a decoded state sends exactly what it received.
    int64_t bad = 0, n = 0;
    for (int32_t v = -kQMaxXY; v <= kQMaxXY; v++)
    {
        QKin k;
        k.x = v;
        k.y = -v;
        const QKin r = Quantize(Dequantize(k));
        n += 2;
        if (r.x != v || r.y != -v) bad++;
    }
    for (int32_t v = kQMinZ; v <= kQMaxZ; v++)
    {
        QKin k;
        k.z = v;
        const QKin r = Quantize(Dequantize(k));
        n++;
        if (r.z != v) bad++;
    }
    const int64_t expectN = 2 * (2 * (int64_t)kQMaxXY + 1) + ((int64_t)kQMaxZ - kQMinZ + 1);
    Check("position: every centimetre of the box survives int -> float -> int (x, y and z, exhaustive)", bad == 0 && n == expectN,
          Num(n) + " values, " + Num(bad) + " changed");

    Rng rng(1);
    auto errOf = [](const Kinematics& in) {
        const Kinematics out = Dequantize(Quantize(in));
        return std::max({std::fabs((double)out.x - in.x), std::fabs((double)out.y - in.y), std::fabs((double)out.z - in.z)});
    };
    double worstFoot = 0, worstBox = 0, worstEdge = 0;
    for (int i = 0; i < 2000000; i++)
    {
        Kinematics k;
        k.x = (float)rng.uni(-8000, 8000);
        k.y = (float)rng.uni(-8000, 8000);
        k.z = (float)rng.uni(-200, 1000);
        worstFoot = std::max(worstFoot, errOf(k));
    }
    for (int i = 0; i < 2000000; i++)
    {
        Kinematics k;
        k.x = (float)rng.uni(-16384, 16384);
        k.y = (float)rng.uni(-16384, 16384);
        k.z = (float)rng.uni(-2048, 3072);
        worstBox = std::max(worstBox, errOf(k));
    }
    for (int i = 0; i < 200000; i++) // the map edge, where a float has the least precision
    {
        Kinematics k;
        k.x = (float)rng.uni(15500, 16384) * (rng.below(2) ? 1.0f : -1.0f);
        k.y = (float)rng.uni(15500, 16384) * (rng.below(2) ? 1.0f : -1.0f);
        k.z = (float)rng.uni(2500, 3072);
        worstEdge = std::max(worstEdge, errOf(k));
    }
    Check("position: on foot (+-8 km, 2M random samples) the error is under 0.0055 m per axis, well under 1 cm",
          worstFoot <= kMaxPosError && worstFoot < 0.01, "worst " + Num(worstFoot) + " m");
    Check("position: over the whole box (2M samples) and at the map edge (200k) the error stays under 0.0055 m per axis",
          worstBox <= kMaxPosError && worstEdge <= kMaxPosError, "box " + Num(worstBox) + " m, edge " + Num(worstEdge) + " m");

    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    Kinematics e;
    e.x = 16384.0f;
    e.y = -16384.0f;
    e.z = 3072.0f;
    const QKin qe = Quantize(e);
    Kinematics f;
    f.x = 1e9f;
    f.y = -1e9f;
    f.z = -1e9f;
    const QKin qf = Quantize(f);
    Kinematics g;
    g.x = inf;
    g.y = -inf;
    g.z = nan;
    g.heading = nan;
    g.vx = nan;
    g.vy = inf;
    const QKin qg = Quantize(g);
    Check("position: the box edges encode exactly, anything beyond saturates, NaN becomes 0 and +-Inf saturates",
          qe.x == kQMaxXY && qe.y == -kQMaxXY && qe.z == kQMaxZ && qf.x == kQMaxXY && qf.y == -kQMaxXY && qf.z == kQMinZ &&
              qg.x == kQMaxXY && qg.y == -kQMaxXY && qg.z == 0 && qg.heading == 0 && qg.vx == 0 && qg.vy == kQMaxVel);
    Kinematics half;
    half.x = 0.0049f;
    half.y = -0.0049f;
    half.z = 12.346f;
    Kinematics up;
    up.x = 0.0051f;
    up.y = -0.0051f;
    up.z = -12.346f;
    const QKin qh = Quantize(half), qu = Quantize(up);
    Check("position: rounding is to the nearest centimetre (0.0049 m -> 0, 0.0051 m -> 1, -0.0051 m -> -1, 12.346 m -> 1235)",
          qh.x == 0 && qh.y == 0 && qh.z == 1235 && qu.x == 1 && qu.y == -1 && qu.z == -1235,
          Num((int64_t)qh.x) + " " + Num((int64_t)qh.y) + " " + Num((int64_t)qh.z) + " / " + Num((int64_t)qu.x) + " " + Num((int64_t)qu.y) +
              " " + Num((int64_t)qu.z));
}

// ---- 2. heading, velocity, coarse -----------------------------------------------------------------

static double CircDiff(double a, double b)
{
    double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

static void OtherFieldTests()
{
    Rng rng(2);
    double worstH = 0;
    bool inRange = true;
    for (int i = 0; i < 1000000; i++)
    {
        Kinematics k;
        k.heading = (float)rng.uni(-360, 360);
        const Kinematics o = Dequantize(Quantize(k));
        worstH = std::max(worstH, CircDiff(o.heading, k.heading));
        if (!(o.heading >= 0.0f && o.heading < 360.0f)) inRange = false;
    }
    Check("heading: error under 0.0028 degrees around the full circle, always in [0, 360)",
          worstH <= kMaxHeadingError && worstH < 0.0028 && inRange, "worst " + Num(worstH) + " deg");
    Kinematics k360, kneg, kbig, kwrap;
    k360.heading = 360.0f;
    kneg.heading = -90.0f;
    kbig.heading = 720.5f;
    kwrap.heading = 359.999f;
    Check("heading: 360 wraps to 0, -90 to 270 (49152), 720.5 to 0.5 (91), 359.999 to 0 (rounds up and wraps)",
          Quantize(k360).heading == 0 && Quantize(kneg).heading == 49152 && Quantize(kbig).heading == 91 && Quantize(kwrap).heading == 0);

    double worstV = 0;
    for (int i = 0; i < 1000000; i++)
    {
        Kinematics k;
        k.vx = (float)rng.uni(-600, 600);
        k.vy = (float)rng.uni(-600, 600);
        k.vz = (float)rng.uni(-600, 600);
        const Kinematics o = Dequantize(Quantize(k));
        worstV = std::max({worstV, (double)std::fabs(o.vx - k.vx), (double)std::fabs(o.vy - k.vy), (double)std::fabs(o.vz - k.vz)});
    }
    int64_t badV = 0;
    for (int v = -kQMaxVel; v <= kQMaxVel; v++)
    {
        QKin a;
        a.vx = (int16_t)v;
        a.vy = (int16_t)-v;
        a.vz = (int16_t)v;
        const QKin r = Quantize(Dequantize(a));
        if (r.vx != v || r.vy != -v || r.vz != v) badV++;
    }
    Kinematics fast;
    fast.vx = 5000.0f;
    fast.vy = -5000.0f;
    Check("velocity: error under 1/64 m/s per axis over +-600 m/s; every velocity step round-trips; beyond +-1000 m/s saturates",
          worstV <= kMaxVelError && badV == 0 && Quantize(fast).vx == kQMaxVel && Quantize(fast).vy == -kQMaxVel,
          "worst " + Num(worstV) + " m/s, " + Num(badV) + " lattice mismatches");

    double worstXY = 0, worstZ = 0;
    int64_t badC = 0;
    for (int i = 0; i < 1000000; i++)
    {
        Kinematics k; // anywhere below the saturating top cell: the whole world box and then some
        k.x = (float)rng.uni(-16384, 16383.75);
        k.y = (float)rng.uni(-16384, 16383.75);
        k.z = (float)rng.uni(-2048, 3071.96);
        const QCoarse c = QuantizeCoarse(k);
        float x, y, z;
        DequantizeCoarse(c, x, y, z);
        worstXY = std::max({worstXY, (double)std::fabs(x - k.x), (double)std::fabs(y - k.y)});
        worstZ = std::max(worstZ, (double)std::fabs(z - k.z));
        Kinematics back;
        back.x = x;
        back.y = y;
        back.z = z;
        if (QuantizeCoarse(back) != c) badC++; // the cell centre quantises to its own cell
    }
    Kinematics edge;
    edge.x = 1e9f;
    edge.y = -1e9f;
    edge.z = 1e9f;
    const QCoarse ce = QuantizeCoarse(edge);
    Kinematics top; // the very top of the box lands in the saturating cell: error up to one step
    top.x = 16384.0f;
    top.y = 16384.0f;
    top.z = 3072.0f;
    float tx, ty, tz;
    DequantizeCoarse(QuantizeCoarse(top), tx, ty, tz);
    Check("coarse: x and y within 0.25 m, z within 0.04 m, cells round-trip (1M samples); out-of-box values saturate; the top cell is off by at most one step",
          worstXY <= kMaxCoarseXYError && worstZ <= kMaxCoarseZError && badC == 0 && ce.x == 65535 && ce.y == 0 && ce.z == 65535 &&
              std::fabs(tx - 16384.0f) <= 0.5 + 1e-6 && std::fabs(ty - 16384.0f) <= 0.5 + 1e-6 && std::fabs(tz - 3072.0f) <= 5.0 / 64.0 + 1e-6,
          "xy " + Num(worstXY, "%.4f") + " m, z " + Num(worstZ, "%.4f") + " m, " + Num(badC) + " cell mismatches");
}

// ---- 3. the range check -----------------------------------------------------------------------------

// v3: both carry the ped block after the 16-byte kinematics (v4: 8 bytes with the shot byte; the
// WorldState entry then carries the 2 vitals bytes). `pedBytes` replaces the ped block verbatim
// (so tests can put illegal values on the wire); empty means the default ped.
static Bytes RawPlayerState(const QKin& q, const Bytes& pedBytes = {})
{
    wire::Writer w;
    w.u8((uint8_t)Msg::PlayerState);
    w.u32(1);
    w.u32(0);
    w.u32(0);
    w.u16(0); // v5: sampleMs
    qwire::PutAbsolute(w, q);
    if (pedBytes.empty())
        qwire::PutPed(w, QPed());
    else
        w.bytes(pedBytes.data(), pedBytes.size());
    return w.take();
}

// A WorldState with one absolute entry, built by hand because Encode() saturates into the box.
static Bytes RawWorld(const QKin& q, const Bytes& pedBytes = {})
{
    wire::Writer w;
    w.u8((uint8_t)Msg::WorldState);
    w.u32(1);
    w.u8(0);      // no leave ids
    w.varu32(1);  // one entry
    w.varu32(0);  // id gap 0
    w.u8(0);      // flags: absolute
    w.u8(0);      // age
    qwire::PutAbsolute(w, q);
    if (pedBytes.empty())
        qwire::PutPed(w, QPed());
    else
        w.bytes(pedBytes.data(), pedBytes.size());
    qwire::PutVitals(w, QPed()); // v4: health 100, armour 0
    return w.take();
}

// A ped block written by hand: move byte, u16 flags, u32 weapon, the shot byte (v4), the vehicle id byte
// when `vehByte` is not negative (v5: move 5 and 6 carry one), then `aim` i16 values (0, 3 or any count).
static Bytes RawPed(uint8_t moveByte, uint16_t flags, uint32_t weapon, const std::vector<int16_t>& aim, uint8_t shotByte = 0,
                    int vehByte = -1)
{
    wire::Writer w;
    w.u8(moveByte);
    w.u16(flags);
    w.u32(weapon);
    w.u8(shotByte);
    if (vehByte >= 0) w.u8((uint8_t)vehByte);
    for (int16_t a : aim) w.i16(a);
    return w.take();
}

static void RangeTests()
{
    struct Field
    {
        const char* name;
        void (*set)(QKin&, int32_t);
        int32_t lo, hi;
    };
    const Field fields[] = {
        {"x", [](QKin& q, int32_t v) { q.x = v; }, -kQMaxXY, kQMaxXY},
        {"y", [](QKin& q, int32_t v) { q.y = v; }, -kQMaxXY, kQMaxXY},
        {"z", [](QKin& q, int32_t v) { q.z = v; }, kQMinZ, kQMaxZ},
        {"vx", [](QKin& q, int32_t v) { q.vx = (int16_t)v; }, -kQMaxVel, kQMaxVel},
        {"vy", [](QKin& q, int32_t v) { q.vy = (int16_t)v; }, -kQMaxVel, kQMaxVel},
        {"vz", [](QKin& q, int32_t v) { q.vz = (int16_t)v; }, -kQMaxVel, kQMaxVel},
    };
    std::string wrong;
    int cases = 0;
    for (const Field& f : fields)
        for (int side = 0; side < 2; side++)
        {
            const int32_t edge = side ? f.hi : f.lo;
            const int32_t past = side ? f.hi + 1 : f.lo - 1;
            QKin in, out;
            f.set(in, edge);
            f.set(out, past);
            const Bytes inP = RawPlayerState(in), outP = RawPlayerState(out), inW = RawWorld(in), outW = RawWorld(out);
            PlayerState ps;
            WorldState ws;
            const bool edgeP = Decode(inP.data(), inP.size(), ps), edgeW = Decode(inW.data(), inW.size(), ws);
            const bool pastP = Decode(outP.data(), outP.size(), ps), pastW = Decode(outW.data(), outW.size(), ws);
            cases += 4;
            const std::string tag = std::string(f.name) + (side ? "+" : "-");
            if (!edgeP || !edgeW || !QInRange(in)) wrong += "[edge " + tag + "]";
            if (pastP || pastW || QInRange(out)) wrong += "[past " + tag + "]";
        }
    Check("range: every field of PlayerState and of an absolute WorldState entry is accepted on the box edge and refused one step past it (48 decodes)",
          cases == 48 && wrong.empty(), wrong);

    // The 22-bit position field is wider than the box: values the field holds but the box excludes are refused.
    QKin wide;
    wide.x = 2000000; // 20 km: inside +-2^21 cm, outside +-16384 m
    const Bytes wp = RawPlayerState(wide);
    PlayerState ps;
    Check("range: a 20 km position fits the 22-bit field but is outside the box, so it is refused", !Decode(wp.data(), wp.size(), ps));

    // A delta whose result leaves the box is refused (the builder is trusting; a hostile server could do this).
    WorldDecoder dec;
    WorldPacketBuilder b;
    const QKin base = Q(kQMaxXY - 5);
    b.Begin(1);
    b.AddEntry(0, 0, base, nullptr, 0);
    const Bytes first = b.Finish();
    WorldState out;
    const bool firstOk = dec.Decode(first.data(), first.size(), out);
    b.Begin(2);
    b.AddEntry(0, 0, Q(kQMaxXY + 20), &base, 1);
    const Bytes pk = b.Finish();
    const bool refused = !dec.Decode(pk.data(), pk.size(), out);
    Check("range: a delta that lands outside the box is refused, counted as rejected, and not acknowledged",
          firstOk && refused && dec.framesRejected() == 1 && dec.ack().tick == 1);

    // v3 ped block, in PlayerState and in an absolute WorldState entry: the last legal value of every
    // field decodes, one past it is refused, and the aim travels exactly when the aiming flag says so.
    {
        const uint32_t w = 0xDEADBEEFu;
        struct Case { const char* name; Bytes ped; bool legal; };
        const std::vector<Case> cases = {
            {"v5 move 6 (exiting vehicle) with its vehId byte", RawPed(6, 0, w, {}, 0, 9), true},
            {"v5 move 6 without its vehId byte", RawPed(6, 0, w, {}), false},
            {"v5 move 5 (entering), vehId 255", RawPed(5, 0, w, {}, 0, 255), true},
            {"v5 move 5, aiming: the vehId byte comes before the aim", RawPed(5, kPfAiming, w, {1, 2, 3}, 0, 4), true},
            {"v5 vehId byte after move 2", RawPed(2, 0, w, {}, 0, 1), false},
            {"move 7", RawPed(7, 0, w, {}), false},
            {"jumpSeq 3", RawPed(3u << 3, 0, w, {}), true},
            {"move-byte bit 5", RawPed(1u << 5, 0, w, {}), false},
            {"move-byte bit 7", RawPed(0x80, 0, w, {}), false},
            {"every known flag but aiming", RawPed(4, (uint16_t)(kPfKnown & ~kPfAiming), w, {}), true},
            {"v5 flag bit 13 (swimming)", RawPed(0, 1u << 13, w, {}), true},
            {"v5 flag bit 14 (melee)", RawPed(0, 1u << 14, w, {}), true},
            {"0.4.3 flag bit 15 (vaulting, no version bump: every bit of the u16 is known)", RawPed(0, 0x8000, w, {}), true},
            {"aiming, aim +-32767", RawPed(0, kPfAiming, w, {32767, -32767, 0}), true},
            {"aiming, aim -32768", RawPed(0, kPfAiming, w, {-32768, 0, 0}), false},
            {"aim bytes without the aiming flag", RawPed(0, 0, w, {1, 2, 3}), false},
            {"aiming flag without aim bytes", RawPed(0, kPfAiming, w, {}), false},
            {"v4 shotSeq 15", RawPed(0, 0, w, {}, 15), true},
            {"v4 shot byte high nibble", RawPed(0, 0, w, {}, 0x10), false},
        };
        std::string bad;
        for (const Case& c : cases)
        {
            const Bytes p = RawPlayerState(Q(100), c.ped), ww = RawWorld(Q(100), c.ped);
            PlayerState ps;
            WorldState ws;
            const bool okP = Decode(p.data(), p.size(), ps), okW = Decode(ww.data(), ww.size(), ws);
            if (okP != c.legal || okW != c.legal) bad += std::string("[") + c.name + "]";
            if (c.legal && okP && Encode(ps) != p) bad += std::string("[re-encode ") + c.name + "]";
        }
        Check("range: ped block: move 0..6 (v5: 5 and 6 with their vehId byte, and only they), jumpSeq 0..3, known flags (v5: 13 swimming and 14 melee; 0.4.3: 15 vaulting), aim +-32767 and shotSeq 0..15 decode; move 7, reserved bits (the shot byte's high nibble too), a missing or surplus vehId byte, aim -32768 and aim bytes that disagree with the aiming flag are refused (38 decodes)",
              cases.size() == 19 && bad.empty(), bad);
    }
}

// ---- 4. the delta codec -------------------------------------------------------------------------------

// Server-side model of one receiver, built only from the public codec API: per subject a 32-tick history of what was
// sent, baselines chosen from AckedAges, and a lossy, reordering link to a real WorldDecoder.
static void DeltaTests()
{
    Rng rng(3);
    constexpr int kSubjects = 40;
    constexpr uint32_t kTicks = 2500;
    struct Subject
    {
        QState cur;               // v3: kinematics AND on-foot state
        int kind = 0;             // 0 idle, 1 walker, 2 driver, 3 chaotic
        double heading = 0;       // radians
        uint32_t histTick[32] = {};
        QState hist[32];
        uint32_t histValid = 0;
    };
    std::vector<Subject> subj(kSubjects);
    for (int i = 0; i < kSubjects; i++)
    {
        subj[i].kind = i % 4;
        subj[i].cur.k.x = (int32_t)rng.uni(-1500000, 1500000);
        subj[i].cur.k.y = (int32_t)rng.uni(-1500000, 1500000);
        subj[i].cur.k.z = (int32_t)rng.uni(-1000, 50000);
        subj[i].heading = rng.uni(0, 6.2832);
    }
    auto headingOf = [](double rad) { return (uint16_t)std::lround(std::fmod(rad + 62.8318, 6.28318) * 65536.0 / 6.28318); };
    WorldPacketBuilder builder;
    WorldDecoder dec;
    WorldAck ackHist[4];
    struct Held { uint32_t tick; Bytes pkt; std::vector<std::pair<int, QState>> content; };
    std::vector<Held> held;
    int64_t entries = 0, deltas = 0, mismatches = 0, delivered = 0, lost = 0, reordered = 0;
    auto deliver = [&](const Bytes& p, const std::vector<std::pair<int, QState>>& content) {
        WorldState out;
        if (!dec.Decode(p.data(), p.size(), out)) return;
        delivered++;
        if (out.players.size() != content.size()) mismatches++;
        for (size_t j = 0; j < out.players.size() && j < content.size(); j++)
            if (out.players[j].id != content[j].first * 7 || QuantizeState(out.players[j].k, out.players[j].ped) != content[j].second) mismatches++;
    };
    for (uint32_t tick = 1; tick <= kTicks; tick++)
    {
        for (int i = 0; i < kSubjects; i++)
        {
            Subject& s = subj[i];
            QKin& q = s.cur.k;
            if (s.kind == 1 || s.kind == 2) // walker 2-4 m/s, driver 30-60 m/s, both with smooth turns
            {
                s.heading += rng.uni(-0.08, 0.08) * (s.kind == 1 ? 1.0 : 0.4);
                const double sp = s.kind == 1 ? 3.0 + std::sin(tick * 0.01 + i) : 45.0 + 15.0 * std::sin(tick * 0.02 + i);
                q.x += (int32_t)std::lround(std::cos(s.heading) * sp / 20.0 * 100.0);
                q.y += (int32_t)std::lround(std::sin(s.heading) * sp / 20.0 * 100.0);
                q.heading = headingOf(s.heading);
                q.vx = (int16_t)std::lround(std::cos(s.heading) * sp * 32);
                q.vy = (int16_t)std::lround(std::sin(s.heading) * sp * 32);
            }
            else if (s.kind == 3) // chaotic: teleports, wraps and extremes
            {
                if (rng.below(8) == 0)
                {
                    q.x = (int32_t)rng.uni(-kQMaxXY, kQMaxXY);
                    q.y = (int32_t)rng.uni(-kQMaxXY, kQMaxXY);
                    q.z = (int32_t)rng.uni(kQMinZ, kQMaxZ);
                }
                q.heading = (uint16_t)rng.below(65536);
                q.vx = (int16_t)rng.uni(-kQMaxVel, kQMaxVel);
                q.vy = (int16_t)rng.uni(-kQMaxVel, kQMaxVel);
                q.vz = (int16_t)rng.uni(-kQMaxVel, kQMaxVel);
            }
            q.x = std::clamp(q.x, -kQMaxXY, kQMaxXY);
            q.y = std::clamp(q.y, -kQMaxXY, kQMaxXY);

            // v3 on-foot events for everyone but the idle subjects: gait changes, flags toggling, jumps
            // (the 2-bit counter wraps), weapon swaps, and an aim that wanders while the aiming flag is on
            // and is 0 otherwise (the QPed invariant every encoder keeps).
            if (s.kind != 0)
            {
                QPed& pd = s.cur.p;
                if (rng.below(10) == 0) pd.move = (uint8_t)rng.below(kMoveMax + 1);
                // v5: moves 5 and 6 name the vehicle (the QPed invariant: vehId is 0 for every other move).
                if (pd.move == kMoveEnteringVehicle || pd.move == kMoveExitingVehicle)
                {
                    if (rng.below(8) == 0) pd.vehId = (uint8_t)rng.below(256);
                }
                else
                    pd.vehId = 0;
                if (rng.below(12) == 0) pd.flags ^= (uint16_t)(1u << rng.below(15)); // v5: bits 0..14
                if (rng.below(25) == 0)
                {
                    pd.jumpSeq = (uint8_t)((pd.jumpSeq + 1) & 3);
                    pd.flags |= kPfJumping;
                }
                if (rng.below(40) == 0) pd.weapon = (uint32_t)rng.next();
                if (pd.flags & kPfAiming)
                {
                    auto wander = [&](int16_t v) {
                        const int32_t step = rng.below(20) == 0 ? (int32_t)rng.uni(-30000, 30000) : (int32_t)rng.uni(-40, 40);
                        return (int16_t)std::clamp<int32_t>(v + step, -kQMaxAim, kQMaxAim);
                    };
                    pd.ax = wander(pd.ax);
                    pd.ay = wander(pd.ay);
                    pd.az = wander(pd.az);
                }
                else
                    pd.ax = pd.ay = pd.az = 0;
            }
        }

        // The ack the server holds now: what the client had two ticks ago.
        const WorldAck ack = tick > 2 ? ackHist[(tick - 2) % 4] : WorldAck{};
        const uint32_t acked = AckedAges(ack, tick);
        builder.Begin(tick);
        std::vector<std::pair<int, QState>> sent;
        for (int i = 0; i < kSubjects; i++)
        {
            if (rng.below(4) == 0) continue; // not every subject is in every frame
            Subject& s = subj[i];
            const QState* base = nullptr;
            uint8_t age = 0;
            for (uint32_t k = 1; k <= kMaxBaseAge && !base; k++)
            {
                if (!((acked >> k) & 1u)) continue;
                const uint32_t slot = (tick - k) % 32;
                if (((s.histValid >> slot) & 1u) && s.histTick[slot] == tick - k)
                {
                    base = &s.hist[slot];
                    age = (uint8_t)k;
                }
            }
            bool wasDelta = false;
            builder.AddEntry((uint16_t)(i * 7), (uint8_t)rng.below(256), s.cur, base, age, &wasDelta);
            sent.push_back({i, s.cur});
            entries++;
            if (wasDelta) deltas++;
        }
        for (const auto& e : sent)
        {
            Subject& s = subj[e.first];
            const uint32_t slot = tick % 32;
            s.histTick[slot] = tick;
            s.hist[slot] = e.second;
            s.histValid |= 1u << slot;
        }
        const Bytes pkt = builder.Finish();

        // The link: 20% lost, 5% held back one tick (arrives after the next frame, so it is stale), the rest on time.
        const uint32_t roll = rng.below(100);
        if (roll < 20)
            lost++;
        else if (roll < 25)
        {
            held.push_back(Held{tick, pkt, sent});
            reordered++;
        }
        else
            deliver(pkt, sent);
        for (size_t h = 0; h < held.size();)
        {
            if (held[h].tick < tick)
            {
                deliver(held[h].pkt, held[h].content); // stale by now unless the newer frame was lost
                held.erase(held.begin() + (long)h);
            }
            else
                h++;
        }
        ackHist[tick % 4] = dec.ack();
    }
    Check("delta: 2500 ticks of walkers, drivers, teleports and extremes (with gait, flag, jump, weapon and aim changes) over a link losing 20% and reordering 5% are reproduced bit for bit",
          mismatches == 0 && dec.framesRejected() == 0 && entries > 50000 && delivered > 1500 && lost > 300 && reordered > 50 &&
              dec.framesStale() > 0 && deltas > entries / 2,
          Num(entries) + " entries, " + Num(deltas) + " deltas, " + Num(mismatches) + " wrong, " + Num((int64_t)dec.framesRejected()) +
              " refused, " + Num((int64_t)dec.framesStale()) + " stale, " + Num(delivered) + " delivered, " + Num(lost) + " lost");

    // Sizes of one entry (id gap 0): smooth walker and driver deltas against the previous tick, and the absolute form.
    auto avgSize = [&](int kind, bool useDelta) {
        Rng r(77);
        QKin q;
        q.x = 100000;
        q.y = 100000;
        q.z = 3000;
        double h = 1.0;
        int64_t total = 0;
        for (int i = 0; i < 2000; i++)
        {
            const QKin before = q;
            h += r.uni(-0.08, 0.08) * (kind == 1 ? 1.0 : 0.4);
            const double sp = kind == 1 ? 3.0 + std::sin(i * 0.01) : 45.0 + 15.0 * std::sin(i * 0.02);
            q.x += (int32_t)std::lround(std::cos(h) * sp / 20.0 * 100.0);
            q.y += (int32_t)std::lround(std::sin(h) * sp / 20.0 * 100.0);
            q.heading = headingOf(h);
            q.vx = (int16_t)std::lround(std::cos(h) * sp * 32);
            q.vy = (int16_t)std::lround(std::sin(h) * sp * 32);
            total += (int64_t)EntryBytes(0, PlanEntry(q, useDelta ? &before : nullptr));
        }
        return (double)total / 2000.0;
    };
    const double walker = avgSize(1, true), driver = avgSize(2, true), absolute = avgSize(1, false);
    // v3 (deliberate): an absolute entry grew by the 7-byte ped block (19 -> 26) and every delta by its
    // extension byte (v2 bounds 13 / 15 -> 14 / 16). The walker's ped part is unchanged tick to tick,
    // as it is for anyone walking steadily, so the extension byte is all it costs.
    // v4 (deliberate): the absolute entry grows by the shot byte and the 2 vitals bytes (26 -> 29); a
    // delta pays for them only when they change, so the delta bounds stay.
    Check("delta: an absolute entry is 29 bytes (gap, header, age, 16 + 8 ped + 2 vitals); a smooth walker's delta averages under 14, a driver's under 16",
          std::fabs(absolute - 29.0) < 1e-9 && walker < 14.0 && driver < 16.0,
          "walker " + Num(walker, "%.2f") + " B, driver " + Num(driver, "%.2f") + " B, absolute " + Num(absolute, "%.2f") + " B");

    const QKin same = Q(5);
    const EntryPlan unchanged = PlanEntry(same, &same);
    QKin far = same;
    far.x = -1600000; // 4-byte varints: a 24-byte kinematic delta (with the extension byte)
    far.y = 1600000;
    far.z = 200000;
    far.vx = 30000;
    far.vy = -30000;
    far.vz = 29000;
    far.heading = 30000;
    const EntryPlan teleport = PlanEntry(far, &same);
    // v4 (deliberate): the absolute form is 26 bytes (16 + 8 + 2 vitals), 32 while aiming, so that pure
    // kinematic teleport (24 B) is now sent as a delta; a teleport that also changes the weapon and the
    // vitals (24 + 4 + 2 = 30 B as a delta) falls back to the absolute form.
    QState sameS, farS;
    sameS.k = same;
    farS.k = far;
    farS.p.weapon = 0xA2719263u;
    farS.p.health = 40;
    const EntryPlan teleportArmed = PlanEntry(farS, &sameS);
    // v3 (deliberate): an unchanged state is the extension byte alone (4 bytes framed, v2 had 3).
    Check("delta: an unchanged state costs only the extension byte (4 bytes with its framing); a kinematic teleport is a 24-byte delta, one that also changes weapon and vitals falls back to the 26-byte absolute form; no base is absolute",
          unchanged.delta && unchanged.payloadBytes == 1 && unchanged.extBits == 0 && EntryBytes(0, unchanged) == 4 && teleport.delta &&
              teleport.payloadBytes == 24 && !teleportArmed.delta && teleportArmed.payloadBytes == 26 && !PlanEntry(same, nullptr).delta,
          "teleport " + std::string(teleport.delta ? "delta " : "absolute ") + Num((int64_t)teleport.payloadBytes) + " B, armed " +
              (teleportArmed.delta ? "delta " : "absolute ") + Num((int64_t)teleportArmed.payloadBytes) + " B");
}

// ---- 4b. the on-foot state (v3) -------------------------------------------------------------------------

// One absolute frame of `base` at tick 1, then `cur` as a delta against it at tick 2, through a fresh decoder.
// Returns the decoded tick-2 entry as a QState; `ok` false when either frame was refused.
static QState PedRoundTrip(const QState& cur, const QState& base, bool& ok, EntryPlan* plan = nullptr, bool* wasDelta = nullptr)
{
    WorldDecoder dec;
    WorldPacketBuilder b;
    b.Begin(1);
    b.AddEntry(4, 0, base, nullptr, 0);
    const Bytes f1 = b.Finish();
    b.Begin(2);
    b.AddEntry(4, 0, cur, &base, 1, wasDelta);
    const Bytes f2 = b.Finish();
    if (plan) *plan = PlanEntry(cur, &base);
    WorldState o1, o2;
    ok = dec.Decode(f1.data(), f1.size(), o1) && dec.Decode(f2.data(), f2.size(), o2) && o2.players.size() == 1;
    return ok ? QuantizeState(o2.players[0].k, o2.players[0].ped) : QState();
}

static void PedTests()
{
    QState base;
    base.k = Q(1000, -2000);
    base.p.move = kMoveWalk;
    base.p.flags = kPfAiming | kPfStealth;
    base.p.weapon = 0x1B06D571u;
    base.p.ax = 120;
    base.p.ay = -40;
    base.p.az = 7;

    // Each on-foot field alone: exactly its extension bit, no kinematics bit, and a bit-exact decode.
    struct Mut { const char* name; void (*apply)(QPed&); uint8_t ext; };
    const Mut muts[] = {
        {"move", [](QPed& p) { p.move = kMoveSprint; }, kExtMove},
        {"jumpSeq", [](QPed& p) { p.jumpSeq = 2; }, kExtMove},
        {"flags", [](QPed& p) { p.flags |= kPfShooting | kPfReloading; }, kExtFlags},
        {"weapon", [](QPed& p) { p.weapon = 0xA2719263u; }, kExtWeapon},
        {"aim", [](QPed& p) { p.ax = -32767; p.az = 32767; }, kExtAim},
        {"v4 shotSeq", [](QPed& p) { p.shotSeq = 9; }, kExtShot},
        {"v4 health", [](QPed& p) { p.health = 37; }, kExtVitals},
        {"v4 armour", [](QPed& p) { p.armor = 200; }, kExtVitals},
        {"nothing", [](QPed&) {}, 0},
    };
    std::string bad;
    for (const Mut& m : muts)
    {
        QState cur = base;
        m.apply(cur.p);
        bool ok = false, wasDelta = false;
        EntryPlan plan;
        const QState got = PedRoundTrip(cur, base, ok, &plan, &wasDelta);
        if (!ok || got != cur || !wasDelta || plan.extBits != m.ext || plan.changeBits != 0) bad += std::string("[") + m.name + "]";
    }
    // Absolute entries carry the whole ped block (v4: 8 bytes, 14 while aiming) and the 2 vitals bytes,
    // and decode bit for bit.
    {
        bool ok = false;
        WorldDecoder dec;
        WorldPacketBuilder b;
        b.Begin(3);
        QState idle;
        idle.k = Q(5);
        b.AddEntry(1, 0, idle, nullptr, 0);
        b.AddEntry(2, 0, base, nullptr, 0);
        const Bytes f = b.Finish();
        WorldState o;
        ok = dec.Decode(f.data(), f.size(), o) && o.players.size() == 2 && QuantizeState(o.players[0].k, o.players[0].ped) == idle &&
             QuantizeState(o.players[1].k, o.players[1].ped) == base;
        // header 1 + 4 + 1 + count 1; entries: (1 + 1 + 1 + 26) + (1 + 1 + 1 + 32)
        if (!ok || f.size() != 7 + 29 + 35) bad += "[absolute " + Num((int64_t)f.size()) + " B]";
    }
    Check("ped: every on-foot field (v4: shotSeq, health, armour too) round-trips; a change of one sets exactly its extension bit (move and jumpSeq share one); absolute entries are 26 / 32 bytes of state (idle / aiming)",
          bad.empty(), bad);

    // The aim travels only with the aiming flag.
    {
        std::string why;
        // Flag cleared: only the flags bit, the aim becomes 0 on both sides.
        QState lowered = base;
        lowered.p.flags = kPfStealth;
        lowered.p.ax = lowered.p.ay = lowered.p.az = 0;
        bool ok1 = false;
        EntryPlan p1;
        const QState g1 = PedRoundTrip(lowered, base, ok1, &p1);
        if (!ok1 || g1 != lowered || p1.extBits != kExtFlags) why += "[lowered]";
        // Flag raised: flags + aim against the baseline's implicit 0.
        QState idle;
        idle.k = base.k;
        QState raised = idle;
        raised.p.flags = kPfAiming;
        raised.p.ax = 300;
        raised.p.ay = -300;
        bool ok2 = false;
        EntryPlan p2;
        const QState g2 = PedRoundTrip(raised, idle, ok2, &p2);
        if (!ok2 || g2 != raised || p2.extBits != (kExtFlags | kExtAim)) why += "[raised]";
        // PedState -> wire: an aim without the flag is dropped, not sent; with it, it is scaled along its direction
        // into +-3276.7 m and rounded to decimetres; a NaN aim refuses to encode only while aiming.
        PedState notAiming;
        notAiming.aimX = 50.0f;
        const QPed qn = QuantizePed(notAiming);
        PedState far;
        far.flags = kPfAiming;
        far.aimX = 5000.0f;
        far.aimY = 0.0f;
        far.aimZ = -2500.0f;
        const QPed qf = QuantizePed(far);
        PlayerState nanAim, nanIgnored;
        nanAim.ped.flags = kPfAiming;
        nanAim.ped.aimY = std::numeric_limits<float>::quiet_NaN();
        nanIgnored.ped.aimY = std::numeric_limits<float>::quiet_NaN();
        if (qn.ax != 0 || qf.ax != 32767 || qf.ay != 0 || std::abs(qf.az + 16384) > 1) why += "[quantise " + Num((int64_t)qf.az) + "]";
        if (!Encode(nanAim).empty() || Encode(nanIgnored).size() != 39) why += "[nan]"; // v5: 39 bytes with sampleMs
        // Hostile deltas: an aim bit when the resulting state is not aiming, and an unknown extension bit.
        auto handDelta = [&](uint8_t ext, const std::function<void(wire::Writer&)>& body) {
            WorldDecoder dec;
            WorldPacketBuilder b;
            b.Begin(1);
            b.AddEntry(4, 0, idle, nullptr, 0);
            const Bytes f1 = b.Finish();
            wire::Writer w;
            w.u8((uint8_t)Msg::WorldState);
            w.u32(2);
            w.u8(0);
            w.varu32(1);
            w.varu32(4);
            w.u8(1); // age 1, no kinematics bits
            w.u8(0);
            w.u8(ext);
            body(w);
            const Bytes f2 = w.take();
            WorldState o;
            return dec.Decode(f1.data(), f1.size(), o) && dec.Decode(f2.data(), f2.size(), o);
        };
        const bool aimNoFlag = handDelta(kExtAim, [](wire::Writer& w) { w.vari32(5); w.vari32(0); w.vari32(0); });
        const bool unknownExt = handDelta(0x80, [](wire::Writer&) {}); // v4: 0x10 / 0x20 are shot / vitals, v5: 0x40 is the vehicle id
        const bool control = handDelta(kExtFlags | kExtAim, [](wire::Writer& w) { w.u16(kPfAiming); w.vari32(5); w.vari32(0); w.vari32(0); });
        if (aimNoFlag || unknownExt || !control) why += "[hostile]";
        Check("ped: the aim travels only with the aiming flag (dropped when it is off, delta against 0 when it comes on), is scaled along its direction into +-3276.7 m; aim bits without the flag and unknown extension bits are refused",
              why.empty(), why);
    }

    // (the vehicle id of moves 5 and 6 is tested in VehIdTests below)

    // jumpSeq: a 2-bit counter. 0,1,2,3,0 as successive deltas (the wrap is a change like any other), a PedState
    // counter past 3 keeps its low 2 bits, and a receiver that loses the frame of a jump still sees the counter move.
    {
        WorldDecoder dec;
        WorldPacketBuilder b;
        QState s;
        s.k = Q(7);
        std::vector<uint8_t> seen;
        QState prev = s;
        b.Begin(1);
        b.AddEntry(9, 0, s, nullptr, 0);
        Bytes f = b.Finish();
        WorldState o;
        bool ok = dec.Decode(f.data(), f.size(), o);
        for (uint32_t t = 2; t <= 5; t++)
        {
            s.p.jumpSeq = (uint8_t)((s.p.jumpSeq + 1) & 3);
            b.Begin(t);
            b.AddEntry(9, 0, s, &prev, 1);
            f = b.Finish();
            ok = ok && dec.Decode(f.data(), f.size(), o) && o.players.size() == 1;
            if (ok) seen.push_back(o.players[0].ped.jumpSeq);
            prev = s;
        }
        // Tick 6 (jumpSeq 1) is lost; tick 7 (still 1, against tick 5's 0) arrives: the receiver sees 0 -> 1.
        QState jumped = s;
        jumped.p.jumpSeq = 1;
        b.Begin(7);
        b.AddEntry(9, 0, jumped, &prev, 2);
        f = b.Finish();
        const bool afterLoss = dec.Decode(f.data(), f.size(), o) && o.players.size() == 1 && o.players[0].ped.jumpSeq == 1;
        PedState wrapped;
        wrapped.jumpSeq = 255; // a uint8_t counter the engine just ++'d past 3 many times
        const bool masked = QuantizePed(wrapped).jumpSeq == 3;
        Check("ped: jumpSeq is a 2-bit counter: 1, 2, 3, 0 decode in order (the wrap included), a counter of 255 travels as 3, and a jump whose frame was lost still shows up as a changed counter",
              ok && seen == std::vector<uint8_t>({1, 2, 3, 0}) && afterLoss && masked,
              std::to_string(seen.size()) + " decoded");
    }
}

// ---- 4c. the vehicle id of entering / exiting (v5) -------------------------------------------------------

// Moves 5 (entering) and 6 (exiting) name the vehicle. In a delta entry that is extension bit kExtVeh (64): one u8
// after the move byte, written only when the id differs from the baseline's (0 when the baseline's move was not 5 or
// 6); any other resulting move has vehId 0 and never carries the bit.
static void VehIdTests()
{
    auto st = [](uint8_t move, uint8_t veh) {
        QState s;
        s.k = Q(500, -500);
        s.p.move = move;
        s.p.vehId = veh;
        return s;
    };
    WorldDecoder dec;
    WorldPacketBuilder b;
    WorldState o;
    QState prev = st(kMoveWalk, 0);
    b.Begin(1);
    b.AddEntry(3, 0, prev, nullptr, 0);
    Bytes f = b.Finish();
    bool ok = dec.Decode(f.data(), f.size(), o);
    struct Step { uint8_t move, veh, ext; size_t bytes; };
    // Packet: type, tick, no leave ids, count, id gap, header, age = 10 bytes, then the extension byte and its fields.
    const Step steps[] = {
        {kMoveEnteringVehicle, 7, (uint8_t)(kExtMove | kExtVeh), 13},   // walk -> entering vehicle 7: move byte, vehicle byte
        {kMoveExitingVehicle, 7, kExtMove, 12},                         // same vehicle: the baseline's id is kept
        {kMoveEnteringVehicle, 200, (uint8_t)(kExtMove | kExtVeh), 13}, // another vehicle
        {kMoveEnteringVehicle, 201, kExtVeh, 12},                       // same move, another vehicle: the vehicle byte alone
        {kMoveIdle, 0, kExtMove, 12},                                   // back to idle: vehId 0 implied, no vehicle byte
        {kMoveEnteringVehicle, 0, kExtMove, 12},                        // vehicle 0 after idle: the implied 0 already matches
    };
    std::string bad;
    uint32_t tick = 2;
    int n = 0;
    for (const Step& s : steps)
    {
        const QState cur = st(s.move, s.veh);
        const EntryPlan plan = PlanEntry(cur, &prev);
        b.Begin(tick++);
        b.AddEntry(3, 0, cur, &prev, 1);
        f = b.Finish();
        ok = ok && dec.Decode(f.data(), f.size(), o) && o.players.size() == 1;
        const QState got = ok ? QuantizeState(o.players[0].k, o.players[0].ped) : QState();
        const uint8_t ext = f.size() > 10 ? f[10] : 0xFF;
        if (!plan.delta || plan.extBits != s.ext || f.size() != s.bytes || ext != s.ext || got != cur || got.p.vehId != s.veh)
            bad += "[step " + Num((int64_t)n) + ": ext " + Num((int64_t)ext) + ", " + Num((int64_t)f.size()) + " B]";
        prev = cur;
        n++;
    }
    Check("vehicle id: a delta into or between entering / exiting costs the vehicle byte only when the id changes (extension bit 64, after the move byte); leaving the move clears it for free; every step decodes bit for bit",
          ok && bad.empty() && kExtVeh == 64, bad);

    // Hostile deltas: the bit is only legal when the resulting move is 5 or 6.
    auto hand = [&](uint8_t baseMove, uint8_t ext, const std::function<void(wire::Writer&)>& body) {
        WorldDecoder d2;
        WorldPacketBuilder b2;
        b2.Begin(1);
        b2.AddEntry(3, 0, st(baseMove, baseMove == kMoveEnteringVehicle ? 4 : 0), nullptr, 0);
        const Bytes f1 = b2.Finish();
        wire::Writer w;
        w.u8((uint8_t)Msg::WorldState);
        w.u32(2);
        w.u8(0);
        w.varu32(1);
        w.varu32(3); // id gap: player 3
        w.u8(1);     // baseline age 1, no kinematics bits
        w.u8(0);     // sample age
        w.u8(ext);
        body(w);
        const Bytes f2 = w.take();
        WorldState o2;
        return d2.Decode(f1.data(), f1.size(), o2) && d2.Decode(f2.data(), f2.size(), o2);
    };
    const bool vehicleWithoutMove = hand(kMoveWalk, kExtVeh, [](wire::Writer& w) { w.u8(9); });
    const bool vehicleBackToIdle = hand(kMoveEnteringVehicle, kExtMove | kExtVeh, [](wire::Writer& w) { w.u8(kMoveIdle); w.u8(9); });
    const bool enteringWithVehicle = hand(kMoveWalk, kExtMove | kExtVeh, [](wire::Writer& w) { w.u8(kMoveEnteringVehicle); w.u8(9); });
    const bool vehicleOfBaseline = hand(kMoveEnteringVehicle, kExtVeh, [](wire::Writer& w) { w.u8(77); });
    const bool enteringKeepsImplied = hand(kMoveWalk, kExtMove, [](wire::Writer& w) { w.u8(kMoveEnteringVehicle); });
    Check("vehicle id: a delta with the vehicle bit whose resulting move is not 5 or 6 is refused; with move 5 or 6 (set by the delta or the baseline) it decodes",
          !vehicleWithoutMove && !vehicleBackToIdle && enteringWithVehicle && vehicleOfBaseline && enteringKeepsImplied,
          std::string(vehicleWithoutMove ? "[walk+veh]" : "") + (vehicleBackToIdle ? "[idle+veh]" : "") +
              (enteringWithVehicle ? "" : "[entering+veh refused]") + (vehicleOfBaseline ? "" : "[baseline 5 + veh refused]") +
              (enteringKeepsImplied ? "" : "[entering without veh refused]"));
}

// ---- 5. acknowledgements and baselines ---------------------------------------------------------------

static Bytes AbsFrame(uint32_t tick, const std::vector<std::pair<uint16_t, QKin>>& es, const std::vector<uint16_t>& left = {})
{
    WorldPacketBuilder b;
    b.Begin(tick);
    for (uint16_t id : left) b.AddLeave(id);
    for (const auto& e : es) b.AddEntry(e.first, 0, e.second, nullptr, 0);
    return b.Finish();
}

static Bytes DeltaFrame(uint32_t tick, uint16_t id, const QKin& cur, const QKin& base, uint8_t age)
{
    WorldPacketBuilder b;
    b.Begin(tick);
    b.AddEntry(id, 0, cur, &base, age);
    return b.Finish();
}

static bool Take(WorldDecoder& d, const Bytes& b, WorldState& out)
{
    return d.Decode(b.data(), b.size(), out);
}

static void AckTests()
{
    // Bit i of an ack's `bits` is tick ack.tick - 1 - i; AckedAges re-expresses that as ages relative to `tick`.
    WorldAck a;
    a.tick = 10;
    a.bits = 0b101; // ticks 9 and 7 (and 10 itself)
    const uint32_t at12 = AckedAges(a, 12), at10 = AckedAges(a, 10);
    WorldAck none, future, old;
    future.tick = 20;
    old.tick = 10;
    old.bits = 0xFFFFFFFFu;
    Check("ack: AckedAges maps (ack tick, bits) to the usable baseline ages; nothing, the future and a stale ack give none",
          at12 == ((1u << 2) | (1u << 3) | (1u << 5)) && at10 == ((1u << 1) | (1u << 3)) && AckedAges(none, 5) == 0 &&
              AckedAges(future, 12) == 0 && AckedAges(old, 42) == 0 && AckedAges(old, 41) == 0x80000000u && AckedAges(old, 40) != 0 &&
              (AckedAges(old, 11) & 1u) == 0,
          Num((int64_t)at12) + " " + Num((int64_t)at10));

    WorldDecoder dec;
    WorldState out;
    bool ok = true;
    for (uint32_t t : {1u, 2u, 4u, 5u}) // tick 3 never arrives
        ok = ok && Take(dec, AbsFrame(t, {{3, Q((int32_t)t)}}), out);
    Check("ack: after frames 1, 2, 4 and 5 the receipt is tick 5 with bits for 4, 2 and 1 but not 3 (0b1101)",
          ok && dec.ack().tick == 5 && dec.ack().bits == 0b1101, "tick " + Num((int64_t)dec.ack().tick) + " bits " + Num((int64_t)dec.ack().bits));

    const bool dupRefused = !Take(dec, AbsFrame(5, {{3, Q(99)}}), out);
    const bool oldRefused = !Take(dec, AbsFrame(4, {{3, Q(98)}}), out);
    Check("ack: a duplicate and an older frame are refused as stale and change nothing (receipt, history)",
          dupRefused && oldRefused && dec.framesStale() == 2 && dec.framesRejected() == 0 && dec.ack().tick == 5 && dec.ack().bits == 0b1101);

    // Frame 6: a delta against tick 5 (age 1), which carried player 3. Frame 7: against tick 3 (age 4), which never arrived.
    WorldState d6;
    const bool goodOk = Take(dec, DeltaFrame(6, 3, Q(60, 6), Q(5), 1), d6);
    const WorldAck before = dec.ack();
    const bool missingRefused = !Take(dec, DeltaFrame(7, 3, Q(70), Q(3), 4), out);
    Check("ack: a delta against a received frame decodes exactly; against a frame that never arrived it is refused and not acknowledged",
          goodOk && d6.players.size() == 1 && Quantize(d6.players[0].k) == Q(60, 6) && missingRefused && dec.framesRejected() == 1 &&
              dec.ack().tick == before.tick && dec.ack().bits == before.bits);

    // The refusal changed nothing: a later delta on the history still decodes; `left` wipes a player's history.
    WorldState d8;
    const bool laterOk = Take(dec, DeltaFrame(8, 3, Q(80), Q(60, 6), 2), d8) && Quantize(d8.players[0].k) == Q(80);
    WorldState d9;
    const bool leaveOk = Take(dec, AbsFrame(9, {}, {3}), d9) && d9.left.size() == 1 && d9.left[0] == 3 && d9.players.empty();
    const bool afterLeave = !Take(dec, DeltaFrame(10, 3, Q(81), Q(80), 2), out); // tick 8 existed but id 3 left at 9
    WorldState d11;
    const bool reenter = Take(dec, AbsFrame(11, {{3, Q(82)}}), d11) && Quantize(d11.players[0].k) == Q(82);
    Check("ack: the decoder survives a refused frame, a leave id wipes that player's baselines, an absolute entry re-adds the player",
          laterOk && leaveOk && afterLeave && reenter,
          std::string(laterOk ? "1" : "0") + (leaveOk ? "1" : "0") + (afterLeave ? "1" : "0") + (reenter ? "1" : "0"));

    // Tick 0 is "nothing received" in an ack, so no frame carries it.
    WorldDecoder fresh;
    const Bytes zero = AbsFrame(0, {{1, Q(1)}});
    WorldState z;
    Check("ack: a frame with tick 0 is refused by both decoders (0 in an ack means nothing received)",
          !Take(fresh, zero, out) && fresh.ack().tick == 0 && !Decode(zero.data(), zero.size(), z) && fresh.framesRejected() == 1);

    // The builder refuses a repeated, descending or too-large id; the decoder refuses a gap that walks past kMaxPlayers.
    WorldPacketBuilder b;
    b.Begin(1);
    const bool a1 = b.AddEntry(5, 0, Q(1), nullptr, 0), a2 = b.AddEntry(5, 0, Q(1), nullptr, 0), a3 = b.AddEntry(3, 0, Q(1), nullptr, 0),
               a4 = b.AddEntry(kMaxPlayers, 0, Q(1), nullptr, 0);
    wire::Writer w;
    w.u8((uint8_t)Msg::WorldState);
    w.u32(1);
    w.u8(0);
    w.varu32(1);
    w.varu32(kMaxPlayers); // gap puts the id at kMaxPlayers
    w.u8(0);
    w.u8(0);
    qwire::PutAbsolute(w, Q(1));
    qwire::PutPed(w, QPed()); // a complete v3 entry: the id is the only fault
    const Bytes walkPast = w.take();
    Check("ack: the builder refuses a repeated, descending or too-large id (entry count stays 1); the decoder refuses an id gap walking past kMaxPlayers",
          a1 && !a2 && !a3 && !a4 && b.entryCount() == 1 && !Decode(walkPast.data(), walkPast.size(), z));
}

// The delta wire form written out byte by byte (docs/api/protocol.md quotes these vectors): a codec that is
// symmetrically wrong would still round-trip.
static void GoldenDeltaTests()
{
    // Tick 6, player 3 moved from (0.05 m, 0) to (0.60 m, 0.06 m) against its entry in tick 5 (age 1):
    // flags = age 1 | position bit (1 << 5) = 0x21; age 0; v3 extension byte 0x00 (on-foot state unchanged);
    // dx = +55 cm, dy = +6 cm, dz = 0 as zigzag varints 0x6E 0x0C 0x00.
    const Bytes want = {0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x21, 0x00, 0x00, 0x6E, 0x0C, 0x00};
    const Bytes got = DeltaFrame(6, 3, Q(60, 6), Q(5), 1);
    // The same player turning 90 degrees at the same spot (heading +16384 steps, velocity +32 on vx):
    // flags = age 2 | heading bit (2 << 5) | velocity bit (4 << 5) = 0xC2; age 7; extension 0x00; heading delta
    // 16384 -> zigzag 32768 = 80 80 02, velocity deltas +32, 0, 0 -> 0x40 0x00 0x00.
    QKin base = Q(100, 200), cur = base;
    cur.heading = 16384;
    cur.vx = 32;
    const Bytes want2 = {0x07, 0x09, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xC2, 0x07, 0x00, 0x80, 0x80, 0x02, 0x40, 0x00, 0x00};
    WorldPacketBuilder b;
    b.Begin(9);
    b.AddEntry(0, 7, cur, &base, 2);
    const Bytes got2 = b.Finish();
    // v3 on-foot delta: player 3 stands still and raises a weapon (aiming flag on, aim point +1.0 m x, -0.5 m z):
    // header 0x01 (age 1, no kinematics bits), age 0, extension 0x0A = flags bit | aim bit, flags u16 04 00,
    // aim deltas against the baseline's 0 in decimetres: +10, 0, -5 -> zigzag 0x14 0x00 0x09.
    QState pBase;
    pBase.k = Q(5);
    pBase.p.weapon = 0x1B06D571u;
    QState pCur = pBase;
    pCur.p.flags = kPfAiming;
    pCur.p.ax = 10;
    pCur.p.az = -5;
    const Bytes want3 = {0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x01, 0x00, 0x0A, 0x04, 0x00, 0x14, 0x00, 0x09};
    WorldPacketBuilder b3;
    b3.Begin(6);
    b3.AddEntry(3, 0, pCur, &pBase, 1);
    const Bytes got3 = b3.Finish();
    auto hex = [](const Bytes& v) {
        std::string s;
        char t[4];
        for (uint8_t x : v)
        {
            std::snprintf(t, sizeof(t), "%02x ", x);
            s += t;
        }
        return s;
    };
    Check("delta: the wire bytes are exactly the documented vectors (a position delta: 14 bytes; heading and velocity deltas: 17 bytes; aim on: 16 bytes)",
          got == want && got2 == want2 && got3 == want3, hex(got) + "| " + hex(got2) + "| " + hex(got3));
}

// ---- 6. hostile input -------------------------------------------------------------------------------------

static void FuzzTests()
{
    Rng rng(6);
    WorldState out;
    // A valid history to attack: players 1..20 at tick 1.
    std::vector<std::pair<uint16_t, QKin>> start;
    for (uint16_t i = 1; i <= 20; i++) start.push_back({i, Q(i * 1000, i)});
    const Bytes seed = AbsFrame(1, start);
    std::vector<Bytes> corpus;
    corpus.push_back(AbsFrame(2, start, {1, 2}));
    {
        WorldPacketBuilder b;
        b.Begin(2);
        b.AddLeave(9);
        b.AddEntry(1, 5, Q(1001, 5), &start[0].second, 1);
        b.AddEntry(2, 6, Q(2000, 9), &start[1].second, 1);
        b.AddEntry(9, 7, Q(9000, 9), &start[8].second, 1);
        b.AddEntry(300, 7, Q(5), nullptr, 0);
        corpus.push_back(b.Finish());
    }
    {
        // v3: on-foot deltas (move, flags, weapon, aim) and an absolute entry that is aiming.
        WorldPacketBuilder b;
        b.Begin(2);
        QState b1, b2, c1, c2, abs;
        b1.k = start[0].second;
        b2.k = start[3].second;
        c1 = b1;
        c1.p.move = kMoveRun;
        c1.p.jumpSeq = 1;
        c1.p.flags = kPfJumping | kPfAiming;
        c1.p.ax = -300;
        c1.p.ay = 25;
        c2 = b2;
        c2.p.weapon = 0x1B06D571u;
        c2.p.flags = kPfStealth;
        abs.k = Q(77);
        abs.p.flags = kPfAiming | kPfShooting;
        abs.p.ax = 12;
        abs.p.az = -32767;
        b.AddEntry(1, 1, c1, &b1, 1);
        b.AddEntry(4, 2, c2, &b2, 1);
        b.AddEntry(500, 3, abs, nullptr, 0);
        corpus.push_back(b.Finish());
    }
    int64_t accepted = 0, refused = 0, bad = 0;
    for (int i = 0; i < 300000; i++)
    {
        Bytes p;
        const uint32_t mode = rng.below(4);
        if (mode == 0)
        {
            const size_t n = rng.below(80);
            for (size_t k = 0; k < n; k++) p.push_back((uint8_t)rng.below(256));
            if (!p.empty() && rng.below(2)) p[0] = (uint8_t)Msg::WorldState;
        }
        else
        {
            p = corpus[rng.below((uint32_t)corpus.size())];
            const uint32_t flips = 1 + rng.below(4);
            for (uint32_t f = 0; f < flips && !p.empty(); f++) p[rng.below((uint32_t)p.size())] = (uint8_t)rng.below(256);
            if (mode == 2 && !p.empty()) p.resize(rng.below((uint32_t)p.size() + 1));
            if (mode == 3) p.push_back((uint8_t)rng.below(256));
        }
        // Two decoders with the same history; one sees the hostile packet, then both see a probe delta (tick 2 on tick 1).
        WorldDecoder victim, pristine;
        Take(victim, seed, out);
        Take(pristine, seed, out);
        const WorldAck ackBefore = victim.ack();
        WorldState got;
        if (victim.Decode(p.data(), p.size(), got))
        {
            accepted++;
            int prev = -1;
            bool wellFormed = got.players.size() <= kMaxPlayers && got.left.size() <= kMaxLeavePerPacket && got.tick != 0;
            for (const WorldEntry& e : got.players)
            {
                if ((int)e.id <= prev || e.id >= kMaxPlayers || !QInRange(Quantize(e.k)) || !QPedInRange(QuantizePed(e.ped)))
                    wellFormed = false;
                prev = e.id;
            }
            for (uint16_t id : got.left)
                if (id >= kMaxPlayers) wellFormed = false;
            if (!wellFormed) bad++;
        }
        else
        {
            refused++;
            if (victim.ack().tick != ackBefore.tick || victim.ack().bits != ackBefore.bits) bad++;
            const Bytes probe = DeltaFrame(2, 1, Q(1234), Q(1000, 1), 1);
            WorldState a, b;
            const bool pa = Take(victim, probe, a), pb = Take(pristine, probe, b);
            if (!pa || !pb || a.players.size() != 1 || b.players.size() != 1 || Quantize(a.players[0].k) != Q(1234) ||
                Quantize(b.players[0].k) != Q(1234))
                bad++;
        }
    }
    Check("fuzz: 300k random and mutated WorldState packets: no crash, every accepted frame is well formed, every refusal leaves the decoder as it was",
          bad == 0 && accepted > 1000 && refused > 100000, Num(accepted) + " accepted, " + Num(refused) + " refused, " + Num(bad) + " violations");

    // The other decoders that take network input: PlayerState from random bytes, WorldCoarse from mutated valid packets.
    std::vector<CoarseWire> cw;
    for (uint16_t i = 0; i < 6; i++) cw.push_back(CoarseWire{(uint16_t)(i * 3), QCoarse{(uint16_t)(1000 * i), (uint16_t)(2000 * i), (uint16_t)(3000 * i)}});
    const Bytes coarseSeed = EncodeCoarse(9, cw);
    int64_t badOther = 0, acceptedPs = 0, acceptedCs = 0;
    for (int i = 0; i < 200000; i++)
    {
        Bytes p;
        const bool coarse = rng.below(2) == 0;
        if (coarse && rng.below(2))
        {
            p = coarseSeed;
            const uint32_t flips = 1 + rng.below(3);
            for (uint32_t f = 0; f < flips; f++) p[rng.below((uint32_t)p.size())] = (uint8_t)rng.below(256);
        }
        else
        {
            const size_t n = rng.below(60);
            for (size_t k = 0; k < n; k++) p.push_back((uint8_t)rng.below(256));
            if (!p.empty()) p[0] = (uint8_t)(coarse ? Msg::WorldCoarse : Msg::PlayerState);
            if (!coarse && p.size() > 32)
            {
                // v3: 31 bytes of PlayerState (v5: with sampleMs), then a ped block: half the time random bytes
                // (v4: 8 or 14; v5: 9 or 15 with a vehId byte), half the time a plausible one (legal move and flags,
                // aim when aiming, now and then -32768; v4: a shot byte, now and then with a reserved bit;
                // v5: the vehId byte of move 5 and 6).
                p.resize(31);
                if (rng.below(2))
                {
                    const size_t m = (rng.below(2) ? 8 : 14) + rng.below(2);
                    for (size_t k = 0; k < m; k++) p.push_back((uint8_t)rng.below(256));
                }
                else
                {
                    const uint16_t flags = (uint16_t)rng.below(kPfKnown + 1);
                    std::vector<int16_t> aim;
                    if (flags & kPfAiming)
                        for (int a = 0; a < 3; a++) aim.push_back(rng.below(50) == 0 ? (int16_t)-32768 : (int16_t)(int32_t)rng.below(65536));
                    const uint8_t shot = (uint8_t)(rng.below(50) == 0 ? rng.below(256) : rng.below(16));
                    const uint32_t move = rng.below(7);
                    const int veh = (move == kMoveEnteringVehicle || move == kMoveExitingVehicle) && rng.below(40) != 0 ? (int)rng.below(256) : -1;
                    const Bytes ped = RawPed((uint8_t)(move | (rng.below(4) << 3)), flags, (uint32_t)rng.next(), aim, shot, veh);
                    p.insert(p.end(), ped.begin(), ped.end());
                }
            }
        }
        PlayerState ps;
        CoarseState cs;
        if (Decode(p.data(), p.size(), ps))
        {
            acceptedPs++;
            if (!QInRange(Quantize(ps.k)) || !QPedInRange(QuantizePed(ps.ped))) badOther++;
            if (Encode(ps) != p) badOther++; // accepted packets are canonical: re-encoding gives the same bytes
        }
        if (Decode(p.data(), p.size(), cs))
        {
            acceptedCs++;
            int prev = -1;
            for (const CoarseEntry& e : cs.players)
            {
                if ((int)e.id <= prev || e.id >= kMaxPlayers) badOther++;
                prev = e.id;
            }
            if (Encode(cs) != p) badOther++;
        }
    }
    Check("fuzz: 200k random PlayerState and mutated WorldCoarse packets: accepted ones are in range, ascending and canonical (re-encode to the same bytes)",
          badOther == 0 && acceptedPs > 100 && acceptedCs > 1000,
          Num(acceptedPs) + " PlayerState, " + Num(acceptedCs) + " WorldCoarse accepted, " + Num(badOther) + " violations");
}

// ---- 7. vehicle rotation, angular velocity, steering (v4) -----------------------------------------------

// Rotation angle between two quaternions, degrees (normalised in double; q and -q are the same rotation).
// 4 atan2(|a - b|, |a + b|) instead of 2 acos(dot): acos loses all precision near 1.
static double QuatAngleDeg(const Quat& qa, const Quat& qb)
{
    double a[4] = {qa.x, qa.y, qa.z, qa.w}, b[4] = {qb.x, qb.y, qb.z, qb.w};
    double na = 0, nb = 0, dot = 0;
    for (int i = 0; i < 4; i++)
    {
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    na = std::sqrt(na);
    nb = std::sqrt(nb);
    for (int i = 0; i < 4; i++)
    {
        a[i] /= na;
        b[i] /= nb;
        dot += a[i] * b[i];
    }
    double d = 0, s = 0;
    for (int i = 0; i < 4; i++)
    {
        const double bi = dot < 0 ? -b[i] : b[i];
        d += (a[i] - bi) * (a[i] - bi);
        s += (a[i] + bi) * (a[i] + bi);
    }
    return 4.0 * std::atan2(std::sqrt(d), std::sqrt(s)) * 180.0 / 3.14159265358979323846;
}

static Quat Unit(double x, double y, double z, double w)
{
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    Quat q;
    q.x = (float)(x / n);
    q.y = (float)(y / n);
    q.z = (float)(z / n);
    q.w = (float)(w / n);
    return q;
}

static void VehicleQuantTests()
{
    // A million uniformly random rotations (Shoemake), plus the hard cases: all four components equal
    // (the analytic worst case), near-ties for the largest, each axis alone, and negated copies.
    Rng rng(0x51A11E57);
    std::vector<Quat> qs;
    qs.reserve(1000000 + 64);
    const double tau = 6.283185307179586;
    for (int i = 0; i < 1000000; i++)
    {
        const double u1 = rng.uni(), u2 = rng.uni(), u3 = rng.uni();
        qs.push_back(Unit(std::sqrt(1 - u1) * std::sin(tau * u2), std::sqrt(1 - u1) * std::cos(tau * u2),
                          std::sqrt(u1) * std::sin(tau * u3), std::sqrt(u1) * std::cos(tau * u3)));
    }
    qs.push_back(Unit(0.5, 0.5, 0.5, 0.5));
    qs.push_back(Unit(-0.5, 0.5, -0.5, 0.5));
    qs.push_back(Unit(0.5, 0.5, 0.5, 0.50001));
    qs.push_back(Unit(0.70710678, 0.70710678, 0, 0));
    qs.push_back(Unit(0.7071, 0.70711, 0.0001, 0));
    for (int axis = 0; axis < 4; axis++)
        qs.push_back(Unit(axis == 0, axis == 1, axis == 2, axis == 3));
    double worst = 0, relayWorst = 0;
    int64_t invalid = 0, signDiffers = 0, reencodeWorse = 0, sameCode = 0;
    for (const Quat& q : qs)
    {
        const uint32_t v = QuantizeQuat(q);
        if (!QuatWireValid(v)) invalid++;
        const Quat back = DequantizeQuat(v);
        worst = std::max(worst, QuatAngleDeg(q, back));
        Quat neg;
        neg.x = -q.x;
        neg.y = -q.y;
        neg.z = -q.z;
        neg.w = -q.w;
        if (QuantizeQuat(neg) != v && QuatAngleDeg(DequantizeQuat(QuantizeQuat(neg)), q) > kMaxQuatErrorDeg) signDiffers++;
        // A relay (the server decodes the driver's rotation and encodes it again for everybody else) adds no
        // second rounding: the re-encoded rotation is the decoded one (to float precision; a handful of
        // rotations lie on the grid of two drop choices at once and may change code, never rotation).
        const uint32_t v2 = QuantizeQuat(back);
        if (QuatAngleDeg(DequantizeQuat(v2), q) > kMaxQuatErrorDeg) reencodeWorse++;
        relayWorst = std::max(relayWorst, QuatAngleDeg(DequantizeQuat(v2), back));
        if (v2 == v) sameCode++;
    }
    Check("vehicle: smallest-three rotation error stays under 0.28 degrees over 1M random rotations and the worst cases; every code is wire-valid; q and -q agree; a decode-encode relay adds under 0.001 degrees (no second rounding)",
          worst <= kMaxQuatErrorDeg && worst > 0.01 && invalid == 0 && signDiffers == 0 && reencodeWorse == 0 && relayWorst < 0.001 &&
              sameCode >= (int64_t)qs.size() - 1000,
          "worst " + Num(worst, "%.4f") + " deg, " + Num(invalid) + " invalid, " + Num(signDiffers) + " sign, relay " +
              Num(relayWorst, "%.6f") + " deg, " + Num((int64_t)qs.size() - sameCode) + " codes changed");

    // The identity is exact; 1023 in any slot and a dropped component that would be imaginary are refused.
    {
        const Quat id = DequantizeQuat(QuantizeQuat(Quat()));
        const uint32_t idCode = (3u << 30) | (511u << 20) | (511u << 10) | 511u;
        Quat zero;
        zero.w = 0;
        Quat nan;
        nan.x = std::numeric_limits<float>::quiet_NaN();
        bool bad1023 = false;
        for (int slot = 0; slot < 3; slot++)
            if (QuatWireValid((idCode & ~(1023u << (10 * slot))) | (1023u << (10 * slot)))) bad1023 = true;
        const uint32_t imaginary = (3u << 30) | (1022u << 20) | (1022u << 10) | 1022u; // three components of 1/sqrt2
        const uint32_t edge = (3u << 30) | (1022u << 20) | (1022u << 10) | 511u;      // exactly 1: w rebuilt as 0
        Check("vehicle: the identity round-trips exactly; a zero-length or NaN rotation encodes as the identity; code 1023 and an imaginary dropped component are refused, a dropped component of exactly 0 is accepted",
              QuantizeQuat(Quat()) == idCode && id.x == 0 && id.y == 0 && id.z == 0 && id.w == 1 && QuantizeQuat(zero) == idCode &&
                  QuantizeQuat(nan) == idCode && !bad1023 && !QuatWireValid(imaginary) && QuatWireValid(edge));
    }

    // Heading <-> pure yaw, and the yaw of a quantised rotation.
    {
        double worstH = 0, worstQ = 0;
        for (int i = 0; i < 36000; i++)
        {
            const float h = (float)(i * 0.01);
            const double back = QuatHeading(QuatFromHeading(h));
            double d = std::fabs(back - h);
            if (d > 180) d = 360 - d;
            worstH = std::max(worstH, d);
            const double viaWire = QuatHeading(DequantizeQuat(QuantizeQuat(QuatFromHeading(h))));
            double dq = std::fabs(viaWire - h);
            if (dq > 180) dq = 360 - dq;
            worstQ = std::max(worstQ, dq);
        }
        const float h90 = QuatHeading(QuatFromHeading(90.0f)), hneg = QuatHeading(QuatFromHeading(-90.0f));
        Check("vehicle: heading -> yaw quaternion -> heading within 0.001 degrees around the circle, within 0.28 through the wire; -90 reads as 270",
              worstH < 0.001 && worstQ <= kMaxQuatErrorDeg && std::fabs(h90 - 90.0f) < 0.001f && std::fabs(hneg - 270.0f) < 0.001f,
              "direct " + Num(worstH, "%.5f") + ", wire " + Num(worstQ, "%.4f") + " deg");
    }

    // Angular velocity 1/16 rad/s and steering 1/127, both saturating at +-127 (never -128).
    {
        double worstAv = 0, worstSt = 0;
        for (int i = -7900; i <= 7900; i++)
        {
            const float av = (float)(i / 1000.0);
            worstAv = std::max(worstAv, (double)std::fabs(DequantizeAngVel(QuantizeAngVel(av)) - av));
        }
        for (int i = -1000; i <= 1000; i++)
        {
            const float st = (float)(i / 1000.0);
            worstSt = std::max(worstSt, (double)std::fabs(DequantizeSteer(QuantizeSteer(st)) - st));
        }
        Check("vehicle: angular velocity within 1/32 rad/s and steering within 1/254 over their ranges; both saturate at +-127, never -128",
              worstAv <= 1.0 / 32 + 1e-6 && worstSt <= 1.0 / 254 + 1e-6 && QuantizeAngVel(1000.0f) == 127 && QuantizeAngVel(-1000.0f) == -127 &&
                  QuantizeSteer(-5.0f) == -127 && QuantizeSteer(5.0f) == 127 && DequantizeSteer(127) == 1.0f,
              "angvel " + Num(worstAv, "%.5f") + ", steer " + Num(worstSt, "%.5f"));
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("-- position\n");
    PositionTests();
    std::printf("-- heading, velocity, coarse\n");
    OtherFieldTests();
    std::printf("-- range check\n");
    RangeTests();
    std::printf("-- delta codec\n");
    DeltaTests();
    std::printf("-- on-foot state (v3)\n");
    PedTests();
    VehIdTests();
    std::printf("-- acknowledgements\n");
    AckTests();
    GoldenDeltaTests();
    std::printf("-- hostile input\n");
    FuzzTests();
    std::printf("-- vehicle rotation (v4)\n");
    VehicleQuantTests();

    const int expected = 35; // v2: 25; v3 adds the ped range check and three on-foot checks; v4 adds four vehicle checks; v5 adds two vehicle-id checks
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected)
    {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
