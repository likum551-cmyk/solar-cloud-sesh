// sync_test: the remote-player smoother (libs/sync/smoother.h), driven by
// scripted network conditions against known ground truth. Build target:
// sync_test (selftest/, never shipped). Pure computation: no sockets, no clock,
// every run is reproducible (seeded generator, simulated time).
//
// Ground truth is a scripted walk on a circle (6 m/s, radius 15 m, so heading
// crosses +-180 every lap). A "server" stamps tick i at i*tickMs; each snapshot
// is delivered after latency +- jitter, or dropped, and the render loop pushes
// what has arrived (stamped with the frame time, as the engine would) and samples
// once per frame. Two things are measured on every shown frame:
//   error   = 3D distance (m) between the pose and the truth at the server time the
//             pose itself claims to show (Pose::renderTick): the reconstruction
//             error of interpolation, extrapolation and the continuity residual;
//   latency = how far behind "now" that time is, measured against the fastest
//             packet of the whole run (the clock model: must stay near the delay).
//
//   1. Steady stream at 60 and 144 fps, a 30 Hz server, linear mode: continuous,
//      bounded per-frame step, no stall, small error.
//   2. 10% and 30% packet loss, then jitter and loss together: error bounded.
//   3. Jitter +-40 ms (which also reorders packets): error bounded, delay adapts.
//   4. A 10 Hz client behind a 20 Hz server (repeated states): not stored, delay
//      follows the real spacing, error bounded.
//   4b. A wrong extrapolation (player stops during a loss burst) is absorbed, not popped.
//   5. Reordered, duplicate and wrapping ticks; a stale packet changes nothing.
//   6. Heading wrap 179 -> -179 (and the mirror, and 350 -> 10) goes the short way.
//   7. Extrapolation capped at 250 ms, then holds, never pops back.
//   8. Teleport (500 m, 9 m) snaps; 7 m does not and is interpolated, not shown at once
//      (the default snapDistance is 8 m, sized for on-foot players).
//   9. RemoteSync: two players from WorldStates, own id / bad ids / NaN ignored.
//  10. A silent player is forgotten after 5 s; a standing one is not; ids reusable.
//  10b. Sample age (WorldEntry::ageMs, protocol v2): a 21 Hz client behind the 20 Hz
//      tick, placed at tick*tickMs - ageMs, tracks the truth where the age-blind
//      placement is off by v*age; an idle player re-sent with growing ages (and a
//      re-sent sample placed before the newest) is a Repeat and holds still;
//      Remove for a WorldState.left id drops the player.
//  10c. PedState (protocol v3): discrete, the snapshot at or before the render time; a
//      ped-only change of a standing player is stored, not a Repeat; a jump whose
//      snapshot the render time skips survives as jumpSeq; RemoteSync passes it through.
//  11. Config sanitising.
//  12. Sync plan W7, the one render clock (libs/sync/netclock.h): NetClock alone (steady, rises at once, falls with
//      wall time not per packet, a new session re-baselines, tick wrap / reorder / refusals, the class extra); one
//      +-30 ms jittered stream into the ped (RemoteSync) and the vehicle path: both read the same render time
//      (|difference| <= 5 ms after 5 s), while the pre-W7 split (a per-player ped clock against a decaying-maximum
//      offset with a fixed 120 ms delay) is measured in the same harness and does not.
//  13. Sender pipeline (onfoot spec item 8): a 100 fps frame-quantised 45 Hz sender, worker wait U[0,2] ms, uplink
//      jitter, a 30 Hz server placing samples with SampleClock, downlink jitter, 2% loss; truth = sample time.
//  14. Sync pipeline review (2026-11-03): (a) the velocity-aided position denoise (smoother.h item 13): the wire's 1 cm
//      position grid was a 6% speed ripple at walking speed; real motion (stop, turn, start) is not smeared; (b) the
//      diagnostics counters (arrival windows, repeats split, baseline steps); (c) END TO END with the real
//      server Replicator and WorldDecoder: sender, uplink, SampleClock, tiers 30/15/10 Hz, idle gate, delta frames,
//      downlink, RemoteSync, 60 fps render, against ground truth, in the profiles of the review (jitter 20..80 ms,
//      loss 2..10%, clock drift, stop/start, tier change, sender hitch, bursts).
//

// Every error check also asserts how many frames it measured, so an empty loop
// cannot pass; the run asserts the total number of checks.

#include "protocol/quant.h"
#include "protocol/worldcodec.h"
#include "sync/netclock.h"
#include "sync/sampleclock.h"
#include "sync/smoother.h"
#include "sync/vehicle_drive.h"

// The REAL server replication (tiers, idle gate, delta codec, quantiser) drives the end-to-end pipeline tests (section 14):
// compiled into this test as it is, no copy of its rules.
#include "../apps/server/replication.cpp"
#include "../apps/server/vehicles.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <queue>
#include <string>
#include <utility>
#include <vector>

using namespace gage;
using namespace gage::sync;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got = "") {
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", got.c_str());
}

static std::string F(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// ---- helpers ----------------------------------------------------------------

static const double kPi = 3.14159265358979323846;

static double WrapDeg(double d) {
    d = std::fmod(d, 360.0);
    if (d > 180.0) d -= 360.0;
    else if (d <= -180.0) d += 360.0;
    return d;
}

static double Dist(const proto::Kinematics& a, const proto::Kinematics& b) {
    const double dx = double(a.x) - b.x, dy = double(a.y) - b.y, dz = double(a.z) - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Rng { // splitmix64: portable, so the scripted losses are the same everywhere
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double Uniform() { return double(Next() >> 11) / double(1ull << 53); } // [0, 1)
};

// Ground truth: constant-speed walk on a circle. Heading is the travel
// direction in the game's convention (0 = +y, counter-clockwise positive).
struct Walk {
    double cx = 0, cy = 0, z = 30, radius = 15, speed = 6, phase = 0;
    proto::Kinematics At(double tMs) const {
        const double th = phase + speed / radius * tMs / 1000.0;
        const double vx = -speed * std::sin(th), vy = speed * std::cos(th);
        proto::Kinematics k;
        k.x = float(cx + radius * std::cos(th));
        k.y = float(cy + radius * std::sin(th));
        k.z = float(z);
        k.vx = float(vx); k.vy = float(vy); k.vz = 0;
        k.heading = float(WrapDeg(std::atan2(-vx, vy) * 180.0 / kPi));
        return k;
    }
};

struct Net {
    double latencyMs = 60, jitterMs = 0, loss = 0;
    uint64_t seed = 1;
};

struct Arrival {
    double at;
    uint32_t tick;
    proto::Kinematics k;
};

// Every tick of `w` from 0 to durationMs, delivered per `net`, in arrival order.
// `everyNth` > 1 models a client that sends slower than the tick rate: the server
// re-sends its last state on the ticks between (bit for bit; protocol v2 would add a
// larger ageMs, which these streams leave at 0: TestSampleAge covers the age).
static std::vector<Arrival> MakeStream(const Walk& w, const Net& net, double tickMs, double durationMs, int everyNth = 1) {
    Rng rng(net.seed);
    std::vector<Arrival> v;
    const int n = int(durationMs / tickMs) + 2;
    for (int i = 0; i < n; ++i) {
        if (net.loss > 0 && rng.Uniform() < net.loss) continue;
        const double j = net.jitterMs > 0 ? (rng.Uniform() * 2 - 1) * net.jitterMs : 0;
        Arrival a;
        a.at = i * tickMs + net.latencyMs + j;
        a.tick = uint32_t(i);
        a.k = w.At((i - i % everyNth) * tickMs);
        v.push_back(a);
    }
    std::stable_sort(v.begin(), v.end(), [](const Arrival& a, const Arrival& b) { return a.at < b.at; });
    return v;
}

struct Metrics {
    size_t frames = 0, shown = 0, dropouts = 0, errN = 0, extrap = 0, hold = 0;
    double maxErr = 0, sumSq = 0, maxHdg = 0;
    double maxStep = 0, minStep = 1e9;
    double minDelay = 1e9, maxDelay = 0;
    double minLat = 1e9, maxLat = -1e9; // how far behind the fastest packets the pose is, ms
    bool badHeading = false; // an output heading outside (-180, 180]
    double Rms() const { return errN ? std::sqrt(sumSq / double(errN)) : 0; }
};

// Pushes arrivals at frame time (as the engine does after its network poll),
// samples once per frame, and measures: error from errFromMs, steps from stepFromMs.
// The fastest a packet is stamped after its tick's server time, over the whole stream:
// the network's minimum latency plus the wait for the next frame (the engine stamps
// at its poll). The truth the smoother is judged against is that far behind "now".
static double MinStampOffset(const std::vector<Arrival>& stream, double tickMs, double fps, double durationMs) {
    const double frameMs = 1000.0 / fps;
    double best = 1e18;
    for (const Arrival& a : stream) {
        if (a.at > durationMs) continue;
        best = std::min(best, std::ceil(a.at / frameMs - 1e-9) * frameMs - double(a.tick) * tickMs);
    }
    return best;
}

static Metrics RunWalk(PlayerSmoother& sm, const Walk& w, const Net& net, double tickMs, double fps,
                       double durationMs, double errFromMs, double stepFromMs) {
    const std::vector<Arrival> stream = MakeStream(w, net, tickMs, durationMs);
    Metrics m;
    size_t next = 0;
    bool everShown = false, havePrev = false;
    Pose prev;
    const double refOffset = MinStampOffset(stream, tickMs, fps, durationMs);
    for (long f = 0;; ++f) {
        const double now = double(f) * (1000.0 / fps);
        if (now > durationMs) break;
        while (next < stream.size() && stream[next].at <= now) {
            sm.Push(stream[next].tick, stream[next].k, now);
            ++next;
        }
        ++m.frames;
        Pose p;
        if (!sm.Sample(now, p)) {
            if (everShown) ++m.dropouts;
            havePrev = false;
            continue;
        }
        everShown = true;
        ++m.shown;
        if (p.k.heading <= -180.0f || p.k.heading > 180.0f) m.badHeading = true;
        if (p.mode == PoseMode::Extrapolating) ++m.extrap;
        if (p.mode == PoseMode::Holding) ++m.hold;
        m.minDelay = std::min(m.minDelay, sm.DelayMs());
        m.maxDelay = std::max(m.maxDelay, sm.DelayMs());
        if (now >= errFromMs) {
            const double shownMs = p.renderTick * tickMs; // the server time this pose claims to show
            const proto::Kinematics t = w.At(shownMs);
            const double lat = now - refOffset - shownMs;
            m.minLat = std::min(m.minLat, lat);
            m.maxLat = std::max(m.maxLat, lat);
            const double e = Dist(p.k, t);
            m.maxErr = std::max(m.maxErr, e);
            m.sumSq += e * e;
            ++m.errN;
            m.maxHdg = std::max(m.maxHdg, std::fabs(WrapDeg(double(p.k.heading) - t.heading)));
        }
        if (now >= stepFromMs && havePrev) {
            const double s = Dist(p.k, prev.k);
            m.maxStep = std::max(m.maxStep, s);
            m.minStep = std::min(m.minStep, s);
        }
        prev = p;
        havePrev = true;
    }
    return m;
}

static void Print(const char* label, const Metrics& m, const SmootherStats& st, double delayNow) {
    std::printf("  [%s] frames=%zu err max=%.4f m rms=%.4f m  hdg max=%.2f deg  step max=%.4f min=%.4f m  "
                "delay %.0f..%.0f (end %.0f) ms  latency %.0f..%.0f ms  extrap=%zu hold=%zu  "
                "accepted=%u stale=%u repeats=%u teleports=%u resyncs=%u\n",
                label, m.frames, m.maxErr, m.Rms(), m.maxHdg, m.maxStep, m.minStep, m.minDelay, m.maxDelay,
                delayNow, m.minLat, m.maxLat, m.extrap, m.hold, st.accepted, st.stale, st.repeats, st.teleports, st.resyncs);
}

static proto::Kinematics K(double x, double y = 0, double z = 0, double heading = 0,
                           double vx = 0, double vy = 0, double vz = 0) {
    proto::Kinematics k;
    k.x = float(x); k.y = float(y); k.z = float(z); k.heading = float(heading);
    k.vx = float(vx); k.vy = float(vy); k.vz = float(vz);
    return k;
}

struct Ev {
    double at;
    uint32_t tick;
    proto::Kinematics k;
};

// Frames from fromMs to toMs at `fps`; events due by a frame are pushed (stamped with
// the frame time) before it is sampled. onFrame(now, ok, pose); onPush(index, result).
template <class OnFrame, class OnPush>
static void RunFrames(PlayerSmoother& sm, const std::vector<Ev>& evs, double fromMs, double toMs, double fps,
                      OnFrame onFrame, OnPush onPush) {
    size_t next = 0;
    for (long f = 0;; ++f) {
        const double now = fromMs + double(f) * (1000.0 / fps);
        if (now > toMs) break;
        while (next < evs.size() && evs[next].at <= now) {
            onPush(next, sm.Push(evs[next].tick, evs[next].k, now));
            ++next;
        }
        Pose p;
        const bool ok = sm.Sample(now, p);
        onFrame(now, ok, p);
    }
}

// ---- 1 + 2 + 3 + 4: the walk under network conditions -----------------------

static void TestSteady(double fps, double tickHz, bool hermite, const char* label) {
    SmootherConfig cfg = SmootherConfig::ForTickHz(tickHz);
    cfg.hermite = hermite;
    PlayerSmoother sm(cfg);
    const double tickMs = 1000.0 / tickHz;
    const Metrics m = RunWalk(sm, Walk(), Net(), tickMs, fps, 30000, 8000, 2000);
    Print(label, m, sm.Stats(), sm.DelayMs());
    const double stepRef = 6.0 / fps; // v * dt
    Check((std::string(label) + ": shown on every frame once data exists, no teleports").c_str(),
          m.dropouts == 0 && m.shown > 0 && sm.Stats().teleports == 0 && sm.Stats().resyncs == 0 && !m.badHeading,
          F("dropouts=%zu teleports=%u resyncs=%u", m.dropouts, sm.Stats().teleports, sm.Stats().resyncs));
    Check((std::string(label) + ": per-frame step within 0.85..1.15 x v*dt (continuous, no stall)").c_str(),
          m.maxStep <= 1.15 * stepRef && m.minStep >= 0.85 * stepRef,
          F("step %.4f..%.4f, v*dt=%.4f", m.minStep, m.maxStep, stepRef));
    // Hermite is exact on the circle at this tick rate; what is left is the render
    // clock still settling while the delay adapts (a few mm at most). The latency
    // (pose age against the fastest packet) stays between the delay bounds.
    Check((std::string(label) + ": error vs truth < 1 cm, heading < 0.5 deg, latency inside the delay band").c_str(),
          m.errN > 1000 && m.maxErr < 0.01 && m.maxHdg < 0.5 &&
          m.minLat >= cfg.minDelayMs - 5 && m.maxLat <= cfg.initialDelayMs + 5,
          F("n=%zu max %.4f m rms %.4f m, hdg %.2f, latency %.1f..%.1f", m.errN, m.maxErr, m.Rms(), m.maxHdg, m.minLat, m.maxLat));
}

// Latency between (min delay - 5) and (max delay + 5), the band the clock model must hold.
static bool LatencyInBand(const Metrics& m) { return m.minLat >= 70 && m.maxLat <= 305; }

// The bounds below were set from the printed runs (seeded, so they repeat exactly)
// with roughly 2-3x headroom, and each is explained by physics: with the heading
// held while extrapolating, the heading error is at most the turn rate (22.9 deg/s
// on this circle) times the extrapolated time; the position error is 0.5 * a * t^2
// on the circle (a = 2.4 m/s^2) while extrapolating, plus the residual still
// decaying after the data comes back.
static void TestLossAndJitter() {
    const double tickMs = 50;
    {
        PlayerSmoother sm;
        Net net; net.loss = 0.10; net.seed = 10;
        const Metrics m = RunWalk(sm, Walk(), net, tickMs, 60, 60000, 8000, 2000);
        Print("loss 10% @60fps", m, sm.Stats(), sm.DelayMs());
        Check("loss 10%: error vs truth bounded (max < 4 cm, rms < 3 mm, hdg < 4 deg)",
              m.errN > 3000 && m.maxErr < 0.04 && m.Rms() < 0.003 && m.maxHdg < 4.0 && m.dropouts == 0 && !m.badHeading &&
              LatencyInBand(m),
              F("n=%zu max %.4f rms %.4f hdg %.2f dropouts %zu latency %.0f..%.0f", m.errN, m.maxErr, m.Rms(), m.maxHdg, m.dropouts, m.minLat, m.maxLat));
        Check("loss 10%: per-frame step bounded (< 1.15 x v*dt), no teleport",
              m.maxStep < 1.15 * 6.0 / 60 && sm.Stats().teleports == 0, F("max step %.4f vs %.4f", m.maxStep, 1.15 * 6.0 / 60));
    }
    {
        PlayerSmoother sm;
        Net net; net.loss = 0.30; net.seed = 30;
        const Metrics m = RunWalk(sm, Walk(), net, tickMs, 60, 60000, 8000, 2000);
        Print("loss 30% @60fps", m, sm.Stats(), sm.DelayMs());
        Check("loss 30%: error vs truth bounded (max < 10 cm, rms < 1 cm, hdg < 8 deg)",
              m.errN > 3000 && m.maxErr < 0.10 && m.Rms() < 0.01 && m.maxHdg < 8.0 && m.dropouts == 0 && !m.badHeading &&
              LatencyInBand(m),
              F("n=%zu max %.4f rms %.4f hdg %.2f dropouts %zu latency %.0f..%.0f", m.errN, m.maxErr, m.Rms(), m.maxHdg, m.dropouts, m.minLat, m.maxLat));
        Check("loss 30%: per-frame step bounded (< 1.3 x v*dt), no teleport",
              m.maxStep < 1.3 * 6.0 / 60 && sm.Stats().teleports == 0, F("max step %.4f vs %.4f", m.maxStep, 1.3 * 6.0 / 60));
    }
    {
        PlayerSmoother sm;
        Net net; net.jitterMs = 40; net.seed = 7;
        const Metrics m = RunWalk(sm, Walk(), net, tickMs, 60, 60000, 8000, 2000);
        Print("jitter +-40 ms @60fps", m, sm.Stats(), sm.DelayMs());
        Check("jitter +-40 ms: the stream really reordered packets (stale drops > 20)",
              sm.Stats().stale > 20, F("stale=%u accepted=%u", sm.Stats().stale, sm.Stats().accepted));
        Check("jitter +-40 ms: error vs truth bounded (max < 2 cm, rms < 1 cm, hdg < 1 deg)",
              m.errN > 3000 && m.maxErr < 0.02 && m.Rms() < 0.01 && m.maxHdg < 1.0 && m.dropouts == 0 && !m.badHeading &&
              LatencyInBand(m),
              F("n=%zu max %.4f rms %.4f hdg %.2f dropouts %zu latency %.0f..%.0f", m.errN, m.maxErr, m.Rms(), m.maxHdg, m.dropouts, m.minLat, m.maxLat));
        Check("jitter +-40 ms: delay adapted up inside [75, 300] ms (late packets ~67 ms past the fastest -> ~140 ms)",
              sm.DelayMs() > 120 && sm.DelayMs() <= 300 && m.minDelay >= 75 - 1e-9 && sm.JitterMs() > 50,
              F("delay %.1f ms, p95 jitter %.1f ms", sm.DelayMs(), sm.JitterMs()));
        Check("jitter +-40 ms: per-frame step bounded (< 1.15 x v*dt)",
              m.maxStep < 1.15 * 6.0 / 60, F("max step %.4f vs %.4f", m.maxStep, 1.15 * 6.0 / 60));
    }
    {
        // The bad link: jitter and loss together, on a 144 fps frame grid that does not line up with the ticks.
        PlayerSmoother sm;
        Net net; net.jitterMs = 40; net.loss = 0.10; net.seed = 11;
        const Metrics m = RunWalk(sm, Walk(), net, tickMs, 144, 60000, 8000, 2000);
        Print("jitter +-40 ms + loss 10% @144fps", m, sm.Stats(), sm.DelayMs());
        Check("jitter +-40 ms + loss 10% @144fps: error vs truth bounded (max < 3 cm, rms < 3 mm, hdg < 4 deg)",
              m.errN > 7000 && m.maxErr < 0.03 && m.Rms() < 0.003 && m.maxHdg < 4.0 && m.dropouts == 0 && !m.badHeading &&
              LatencyInBand(m),
              F("n=%zu max %.4f rms %.4f hdg %.2f dropouts %zu latency %.0f..%.0f", m.errN, m.maxErr, m.Rms(), m.maxHdg, m.dropouts, m.minLat, m.maxLat));
        Check("jitter +-40 ms + loss 10% @144fps: per-frame step bounded (< 1.3 x v*dt), no teleport",
              m.maxStep < 1.3 * 6.0 / 144 && sm.Stats().teleports == 0, F("max step %.4f vs %.4f", m.maxStep, 1.3 * 6.0 / 144));
    }
}

static void TestSlowClient() {
    // A 10 Hz client on the 20 Hz server: every second tick repeats the state.
    PlayerSmoother sm;
    const Walk w;
    const std::vector<Arrival> stream = MakeStream(w, Net(), 50, 40000, 2);
    const double refOffset = MinStampOffset(stream, 50, 60, 40000);
    size_t next = 0, extrap = 0, shown = 0, errN = 0;
    double maxErr = 0, maxLat = -1e9;
    for (long f = 0;; ++f) {
        const double now = double(f) * (1000.0 / 60);
        if (now > 40000) break;
        while (next < stream.size() && stream[next].at <= now) { sm.Push(stream[next].tick, stream[next].k, now); ++next; }
        Pose p;
        if (!sm.Sample(now, p)) continue;
        ++shown;
        if (p.mode == PoseMode::Extrapolating) ++extrap;
        if (now >= 10000) {
            maxErr = std::max(maxErr, Dist(p.k, w.At(p.renderTick * 50.0)));
            maxLat = std::max(maxLat, now - refOffset - p.renderTick * 50.0);
            ++errN;
        }
    }
    const SmootherStats& st = sm.Stats();
    std::printf("  [10 Hz client on 20 Hz ticks] err max=%.4f m  delay %.0f ms  max latency %.0f ms  accepted=%u repeats=%u extrap=%zu/%zu\n",
                maxErr, sm.DelayMs(), maxLat, st.accepted, st.repeats, extrap, shown);
    Check("slow client: repeated states are not stored (repeats ~ accepted, not zero)",
          st.repeats > 350 && st.repeats <= st.accepted + 2 && st.repeats + 2 >= st.accepted,
          F("accepted=%u repeats=%u", st.accepted, st.repeats));
    Check("slow client: delay follows the real 100 ms spacing (>= 125 ms) and stays out of extrapolation (< 3% of frames)",
          sm.DelayMs() >= 125 && double(extrap) < 0.03 * double(shown), F("delay %.1f ms, extrap %zu/%zu", sm.DelayMs(), extrap, shown));
    Check("slow client: error vs truth < 1 cm (Hermite across the 100 ms spacing)",
          errN > 1500 && maxErr < 0.01, F("n=%zu max %.4f m", errN, maxErr));
}

// ---- 4b: a wrong prediction is absorbed, not popped ---------------------------

static void TestContinuity() {
    // A player running at 10 m/s stops dead at x = 20 m while 6 packets are lost.
    // Extrapolation carries the pose 2.5 m (the cap) past the stop; when the first
    // "stopped" snapshot arrives, the curve says x = 20, 2 m back. Shown at once
    // that is a 2 m pop in one frame; the continuity residual spreads it out.
    PlayerSmoother sm;
    std::vector<Ev> evs;
    for (int i = 0; i < 40; ++i) evs.push_back({i * 50.0, uint32_t(i), K(i * 0.5, 0, 0, 90, 10, 0, 0)});
    for (int i = 46; i < 90; ++i) evs.push_back({i * 50.0, uint32_t(i), K(20.0, 0, 0, 90, 0, 0, 0)});
    double maxStep = 0, maxX = 0, overshoot = 0, lastX = 0;
    size_t frames = 0;
    bool havePrev = false;
    Pose prev;
    RunFrames(sm, evs, 0, 4000, 60, [&](double now, bool ok, const Pose& p) {
        if (!ok) { havePrev = false; return; }
        if (now >= 1500) {
            ++frames;
            if (havePrev) maxStep = std::max(maxStep, Dist(p.k, prev.k));
            maxX = std::max(maxX, double(p.k.x));
            lastX = p.k.x;
        }
        prev = p;
        havePrev = true;
    }, [](size_t, PushResult) {});
    overshoot = maxX - 20.0;
    std::printf("  [continuity] max step %.4f m (running step %.4f), overshoot past the stop %.3f m, final x %.4f, stored=%u repeats=%u\n",
                maxStep, 10.0 / 60, overshoot, lastX, sm.Stats().accepted, sm.Stats().repeats);
    Check("extrapolation overshoots the stop (so the prediction really was wrong by ~2 m), capped at 2.5 m",
          frames > 100 && overshoot > 1.5 && overshoot <= 2.5 + 0.01, F("overshoot %.3f m over %zu frames", overshoot, frames));
    Check("...and the correction is absorbed: no step over 0.7 m (a 2 m pop would be one step), ends exactly at the stop",
          maxStep < 0.7 && std::fabs(lastX - 20.0) < 0.005, F("max step %.4f m, final x %.4f", maxStep, lastX));
}

// ---- 5: ordering ------------------------------------------------------------

static void TestOrdering() {
    {
        PlayerSmoother sm;
        auto at = [](uint32_t tick) { return K(tick * 0.5, 0, 0, 90, 10, 0, 0); };
        const PushResult a = sm.Push(100, at(100), 0), b = sm.Push(101, at(101), 50), c = sm.Push(102, at(102), 100);
        Check("ticks 100, 101, 102 in order are accepted",
              a == PushResult::Accepted && b == PushResult::Accepted && c == PushResult::Accepted);
        Pose before, after;
        sm.Sample(120, before);
        const PushResult dup1 = sm.Push(101, at(101), 120);     // duplicate of an older tick
        const PushResult dup2 = sm.Push(102, at(102), 120);     // duplicate of the newest tick
        const PushResult old = sm.Push(99, at(99), 120);        // reordered-late
        const PushResult wild = sm.Push(100, K(5000, 5000, 5000, 10), 120); // stale AND far away
        sm.Sample(120, after);
        Check("duplicate and reordered ticks are dropped as Stale",
              dup1 == PushResult::Stale && dup2 == PushResult::Stale && old == PushResult::Stale && wild == PushResult::Stale,
              F("%d %d %d %d", int(dup1), int(dup2), int(old), int(wild)));
        Check("a stale packet changes nothing: no teleport, pose identical, counts right",
              sm.Stats().teleports == 0 && before.k.x == after.k.x && before.k.y == after.k.y &&
              before.k.z == after.k.z && sm.Stats().accepted == 3 && sm.Stats().stale == 4,
              F("accepted=%u stale=%u teleports=%u dx=%g", sm.Stats().accepted, sm.Stats().stale,
                sm.Stats().teleports, double(after.k.x) - before.k.x));
        Check("the next newer tick is accepted again", sm.Push(103, at(103), 150) == PushResult::Accepted);
    }
    {
        // u32 wrap: serial arithmetic keeps ordering across 0xFFFFFFFF -> 0.
        PlayerSmoother sm;
        auto at = [](int i) { return K(i * 0.5, 0, 0, 0, 10, 0, 0); };
        const PushResult r0 = sm.Push(0xFFFFFFFEu, at(0), 0), r1 = sm.Push(0xFFFFFFFFu, at(1), 50);
        const PushResult r2 = sm.Push(0u, at(2), 100), r3 = sm.Push(1u, at(3), 150);
        Check("ticks across the u32 wrap are accepted in order",
              r0 == PushResult::Accepted && r1 == PushResult::Accepted && r2 == PushResult::Accepted && r3 == PushResult::Accepted);
        const PushResult back = sm.Push(0xFFFFFFFFu, at(1), 160);
        const PushResult half = sm.Push(1u + 0x80000000u, at(9), 160); // exactly half the range ahead: not newer
        Check("after the wrap, a pre-wrap tick and a half-range jump are stale",
              back == PushResult::Stale && half == PushResult::Stale, F("%d %d", int(back), int(half)));
        Check("tick 2 after the wrap is accepted", sm.Push(2u, at(4), 200) == PushResult::Accepted);
    }
    {
        // A moving player whose ticks wrap mid-run: the unwrapped timeline stays continuous.
        PlayerSmoother sm;
        std::vector<Ev> evs;
        for (int i = 0; i < 80; ++i) evs.push_back({i * 50.0 + 30, uint32_t(0xFFFFFFF0u + uint32_t(i)), K(i * 0.5, 0, 0, 0, 10, 0, 0)});
        double maxStep = 0, minStep = 1e9;
        bool havePrev = false;
        Pose prev;
        RunFrames(sm, evs, 0, 3900, 60, [&](double now, bool ok, const Pose& p) {
            if (!ok) { havePrev = false; return; }
            if (now >= 1500 && havePrev) {
                const double s = Dist(p.k, prev.k);
                maxStep = std::max(maxStep, s);
                minStep = std::min(minStep, s);
            }
            prev = p;
            havePrev = true;
        }, [](size_t, PushResult) {});
        Check("a run across the tick wrap moves continuously (step 0.85..1.15 x v*dt, no teleport)",
              maxStep <= 1.15 * 10.0 / 60 && minStep >= 0.85 * 10.0 / 60 && sm.Stats().teleports == 0 && sm.Stats().stale == 0,
              F("step %.4f..%.4f vs %.4f, teleports=%u", minStep, maxStep, 10.0 / 60, sm.Stats().teleports));
    }
}

// ---- 6: heading -------------------------------------------------------------

struct HeadingResult {
    size_t interp = 0;
    double minAbs = 1e9, maxAbs = 0, maxFrameStep = 0;
    bool badRange = false;
};

static HeadingResult RunHeading(double h0, double h1, double h2) {
    PlayerSmoother sm;
    std::vector<Ev> evs = {{0, 0, K(0, 0, 0, h0)}, {50, 1, K(0, 0, 0, h1)}, {100, 2, K(0, 0, 0, h2)}};
    HeadingResult r;
    bool havePrev = false;
    double prev = 0;
    RunFrames(sm, evs, 0, 400, 60, [&](double, bool ok, const Pose& p) {
        if (!ok) return;
        const double h = p.k.heading;
        if (h <= -180.0 || h > 180.0) r.badRange = true;
        if (p.mode != PoseMode::Interpolating) { havePrev = false; return; }
        ++r.interp;
        r.minAbs = std::min(r.minAbs, std::fabs(h));
        r.maxAbs = std::max(r.maxAbs, std::fabs(h));
        if (havePrev) r.maxFrameStep = std::max(r.maxFrameStep, std::fabs(WrapDeg(h - prev)));
        prev = h;
        havePrev = true;
    }, [](size_t, PushResult) {});
    return r;
}

static void TestHeading() {
    const HeadingResult a = RunHeading(179, -179, -177);
    Check("heading 179 -> -179 -> -177 goes the short way (never near 0, small steps)",
          a.interp >= 4 && a.minAbs >= 176.9 && a.maxFrameStep < 2.0 && !a.badRange,
          F("interp frames %zu, |h| min %.2f max %.2f, max step %.2f deg", a.interp, a.minAbs, a.maxAbs, a.maxFrameStep));
    const HeadingResult b = RunHeading(-179, 179, 177);
    Check("heading -179 -> 179 -> 177 goes the short way the other direction",
          b.interp >= 4 && b.minAbs >= 176.9 && b.maxFrameStep < 2.0 && !b.badRange,
          F("interp frames %zu, |h| min %.2f max %.2f, max step %.2f deg", b.interp, b.minAbs, b.maxAbs, b.maxFrameStep));
    const HeadingResult c = RunHeading(350, 10, 30);
    Check("heading 350 -> 10 -> 30 (0..360 input) passes through 0 and reports (-180, 180]",
          c.interp >= 4 && c.maxAbs <= 30.1 && c.maxFrameStep < 8.0 && !c.badRange,
          F("interp frames %zu, |h| max %.2f, max step %.2f deg", c.interp, c.maxAbs, c.maxFrameStep));
}

// ---- 7: extrapolation -------------------------------------------------------

static void TestExtrapolation() {
    PlayerSmoother sm;
    std::vector<Ev> evs;
    for (int i = 0; i < 40; ++i) evs.push_back({i * 50.0, uint32_t(i), K(i * 0.5, 0, 0, 90, 10, 0, 0)});
    const double lastX = 39 * 0.5; // the last snapshot; nothing is sent after it
    double maxD = 0, prevD = -1, dAt3500 = 0, dAtEnd = 0;
    bool monotone = true, modesInOrder = true, holdMoving = false;
    int phase = 0; // 0 interpolating, 1 extrapolating, 2 holding
    size_t extrapFrames = 0, holdFrames = 0;
    RunFrames(sm, evs, 0, 6500, 60, [&](double now, bool ok, const Pose& p) {
        if (!ok) return;
        const int ph = p.mode == PoseMode::Holding ? 2 : p.mode == PoseMode::Extrapolating ? 1 : 0;
        if (ph < phase) modesInOrder = false;
        phase = std::max(phase, ph);
        if (ph == 1) ++extrapFrames;
        if (ph == 2) { ++holdFrames; if (p.k.vx != 0 || p.k.vy != 0 || p.k.vz != 0) holdMoving = true; }
        if (now < 2300) return; // the buffer still has the last snapshots ahead of the render time
        const double d = double(p.k.x) - lastX;
        maxD = std::max(maxD, d);
        if (prevD >= 0 && d < prevD - 1e-3) monotone = false;
        prevD = d;
        if (now >= 3500 && now < 3520) dAt3500 = d;
        dAtEnd = d;
    }, [](size_t, PushResult) {});
    std::printf("  [extrapolation] max displacement %.4f m (cap = 10 m/s * 0.25 s = 2.5 m), at 3.5 s %.4f, at end %.4f; extrap frames %zu, hold frames %zu\n",
                maxD, dAt3500, dAtEnd, extrapFrames, holdFrames);
    Check("extrapolation never exceeds the 250 ms cap (<= 2.5 m past the last snapshot)",
          maxD <= 2.5 + 0.01, F("max displacement %.4f m", maxD));
    Check("extrapolation does reach the cap (about 2.5 m), so the check above is not vacuous",
          maxD >= 2.5 - 0.05 && extrapFrames >= 8, F("max displacement %.4f m, extrap frames %zu", maxD, extrapFrames));
    Check("after the cap the pose holds: same position 1.5 s and 4 s later, velocity 0, modes in order, never pops back",
          holdFrames > 100 && std::fabs(dAtEnd - dAt3500) < 1e-3 && std::fabs(dAtEnd - 2.5) < 0.01 && !holdMoving && modesInOrder && monotone,
          F("hold frames %zu, d3.5=%.4f dEnd=%.4f holdMoving=%d inOrder=%d monotone=%d", holdFrames, dAt3500, dAtEnd,
            int(holdMoving), int(modesInOrder), int(monotone)));
}

// ---- 8: teleport ------------------------------------------------------------

struct JumpResult {
    PushResult push = PushResult::Stale;
    bool flagOnFirst = false, flagLater = false;
    // maxStepAfter: steps from the 4th frame after the jump's push on (the snap frame
    // itself is excluded); maxStepSince: every step from the push frame on.
    double distToNewAtFirst = 1e9, maxStepAfter = 0, maxStepSince = 0, finalErr = 0;
    size_t finalN = 0; // frames behind finalErr (an empty loop must not pass)
};

// A player running along +x at 10 m/s at 20 Hz; at tick 60 (3 s) its y jumps by `jump`.
static JumpResult RunJump(double jump) {
    PlayerSmoother sm;
    std::vector<Ev> evs;
    auto pos = [&](int i) { return K(i * 0.5, i >= 60 ? jump : 0, 0, 90, 10, 0, 0); };
    for (int i = 0; i <= 100; ++i) evs.push_back({i * 50.0 + 30, uint32_t(i), pos(i)});
    JumpResult r;
    bool pushedJump = false, havePrev = false;
    int framesSince = -1;
    Pose prev;
    RunFrames(sm, evs, 0, 5000, 60, [&](double now, bool ok, const Pose& p) {
        if (!ok) return;
        if (pushedJump) {
            ++framesSince;
            if (framesSince == 0) {
                r.flagOnFirst = p.teleported;
                r.distToNewAtFirst = Dist(p.k, pos(60));
            } else if (p.teleported) {
                r.flagLater = true;
            }
            if (havePrev) {
                const double s = Dist(p.k, prev.k);
                r.maxStepSince = std::max(r.maxStepSince, s);
                if (framesSince >= 3) r.maxStepAfter = std::max(r.maxStepAfter, s);
            }
        }
        if (now >= 4500) {
            const double tr = p.renderTick * 50.0;
            r.finalErr = std::max(r.finalErr, Dist(p.k, K(tr * 0.01, jump, 0, 90, 10, 0, 0)));
            ++r.finalN;
        }
        prev = p;
        havePrev = true;
    }, [&](size_t i, PushResult res) {
        if (i == 60) { r.push = res; pushedJump = true; }
    });
    return r;
}

static void TestTeleport() {
    const double stepRef = 10.0 / 60;
    const JumpResult far = RunJump(500);
    std::printf("  [teleport 500 m] push=%d flagFirst=%d dist to new at first frame=%.4f m, max step after=%.4f (v*dt=%.4f)\n",
                int(far.push), int(far.flagOnFirst), far.distToNewAtFirst, far.maxStepAfter, stepRef);
    Check("a 500 m jump is a teleport: reported, history dropped, shown at once (flag on the first frame only)",
          far.push == PushResult::Teleported && far.flagOnFirst && !far.flagLater && far.distToNewAtFirst < 0.5,
          F("push=%d flag=%d later=%d dist=%.3f", int(far.push), int(far.flagOnFirst), int(far.flagLater), far.distToNewAtFirst));
    Check("after the snap nothing slides: every later step <= 1.2 x v*dt, tracking resumes (< 5 cm)",
          far.finalN > 20 && far.maxStepAfter <= 1.2 * stepRef && far.finalErr < 0.05,
          F("max step %.4f, final err %.4f over %zu frames", far.maxStepAfter, far.finalErr, far.finalN));

    const JumpResult over = RunJump(9);
    Check("a 9 m jump (above the 8 m threshold) snaps",
          over.push == PushResult::Teleported && over.flagOnFirst && over.distToNewAtFirst < 0.5,
          F("push=%d dist=%.3f", int(over.push), over.distToNewAtFirst));

    const JumpResult under = RunJump(7);
    std::printf("  [jump 7 m] push=%d max step since push=%.4f m, final err=%.4f\n",
                int(under.push), under.maxStepSince, under.finalErr);
    Check("a 7 m jump (below the 8 m threshold) is not a teleport: no flag, history kept",
          under.push == PushResult::Accepted && !under.flagOnFirst && !under.flagLater,
          F("push=%d flag=%d later=%d", int(under.push), int(under.flagOnFirst), int(under.flagLater)));
    Check("...it is interpolated across the 50 ms snapshot spacing (several frames, no single step over 0.7 x the jump) and tracking resumes (< 5 cm)",
          under.finalN > 20 && under.maxStepSince > 0.2 * 7 && under.maxStepSince < 0.7 * 7 && under.finalErr < 0.05,
          F("max step since push %.3f m (jump 7), final err %.4f over %zu frames", under.maxStepSince, under.finalErr, under.finalN));
}

// ---- 9 + 10: RemoteSync -----------------------------------------------------

static proto::WorldState World(uint32_t tick, double tMs, const Walk& a, const Walk& b) {
    proto::WorldState ws;
    ws.tick = tick;
    proto::WorldEntry e;
    e.id = 3; e.k = a.At(tMs); ws.players.push_back(e);
    e.id = 8; e.k = b.At(tMs); ws.players.push_back(e);
    return ws;
}

static void TestRemoteSync() {
    Walk a, b;
    b.cx = 200; b.cy = -80; b.radius = 9; b.speed = 4; b.phase = 2.0;
    RemoteSync rs;
    rs.SetLocalId(9);
    Rng rng(99);
    double maxErrA = 0, maxErrB = 0;
    size_t n = 0;
    bool badIds = true;
    uint32_t nextTick = 0;
    double nextAt = 60;
    for (long f = 0;; ++f) {
        const double now = double(f) * (1000.0 / 60);
        if (now > 30000) break;
        // the server's tick i is due at i*50 and arrives 60 ms later; 10% of whole packets are lost
        while (nextAt <= now) {
            if (rng.Uniform() >= 0.10) {
                proto::WorldState ws = World(nextTick, nextTick * 50.0, a, b);
                proto::WorldEntry own, huge, nan;
                own.id = 9; own.k = K(1, 1, 1);          // our own id: skipped
                huge.id = 4000; huge.k = K(2, 2, 2);     // >= kMaxPlayers: skipped
                nan.id = 77; nan.k = K(0); nan.k.x = NAN; // non-finite: refused
                ws.players.push_back(own); ws.players.push_back(huge); ws.players.push_back(nan);
                rs.OnWorldState(ws, now);
            }
            ++nextTick;
            nextAt += 50;
        }
        Pose p;
        if (rs.Sample(3, now, p) && now >= 8000) { maxErrA = std::max(maxErrA, Dist(p.k, a.At(p.renderTick * 50.0))); ++n; }
        if (rs.Sample(8, now, p) && now >= 8000) maxErrB = std::max(maxErrB, Dist(p.k, b.At(p.renderTick * 50.0)));
        if (rs.Sample(9, now, p) || rs.Sample(4000, now, p) || rs.Sample(77, now, p)) badIds = false;
    }
    std::printf("  [RemoteSync 2 players, 10%% loss] err max a=%.4f m b=%.4f m, count=%zu\n", maxErrA, maxErrB, rs.Count());
    Check("RemoteSync: two players track their own paths from WorldStates (each < 4 cm under 10% loss)",
          n > 1000 && maxErrA < 0.04 && maxErrB < 0.04, F("a %.4f b %.4f samples %zu", maxErrA, maxErrB, n));
    Check("RemoteSync: own id, id >= kMaxPlayers and a NaN entry are never tracked (count is exactly 2)",
          badIds && rs.Count() == 2 && rs.Find(77) == nullptr && rs.Find(9) == nullptr, F("count=%zu", rs.Count()));
}

static void TestForget() {
    RemoteSync rs;
    auto stateOf = [](int id, double x) { return K(x + id * 100.0, 0, 0, 0, 5, 0, 0); };
    double lastRecv5 = 0;
    bool still6Ok = true, still7Ok = true;
    size_t prunedEarly = 0;
    std::vector<uint16_t> gone;
    Pose p;
    uint32_t tick = 0;
    double nextAt = 0;
    int checkedAlive = -1, aliveSample = -1, afterSample = -1, goneBefore = -1;
    for (long f = 0;; ++f) {
        const double now = double(f) * (1000.0 / 60);
        if (now > 12000) break;
        while (nextAt <= now) {
            const double tMs = tick * 50.0;
            if (tMs < 2000) { rs.OnPlayerState(5, tick, stateOf(5, tMs * 0.005), now); lastRecv5 = now; }
            rs.OnPlayerState(6, tick, stateOf(6, tMs * 0.005), now);
            rs.OnPlayerState(7, tick, K(700, 50, 10, 45), now); // standing: identical state every tick
            ++tick;
            nextAt += 50;
        }
        if (now >= 6000 && now < 6016) { // 5 s of silence is not reached yet (last packet ~2 s)
            prunedEarly = rs.Prune(now, &gone);
            checkedAlive = int(rs.Sample(5, now, p));
        }
        if (now >= lastRecv5 + 4900 && aliveSample < 0) aliveSample = int(rs.Sample(5, now, p)) + 10 * int(rs.Count());
        if (now >= lastRecv5 + 5100 && afterSample < 0) {
            afterSample = int(rs.Sample(5, now, p)); // expired: false even before Prune
            goneBefore = int(rs.Count());
            rs.Prune(now, &gone);
        }
        if (now >= 3000 && !rs.Sample(6, now, p)) still6Ok = false;
        if (now >= 3000 && !rs.Sample(7, now, p)) still7Ok = false;
    }
    const bool sevenHolds = rs.Sample(7, 12000, p) && p.mode == PoseMode::Holding && std::fabs(p.k.x - 700) < 1e-3 &&
                            std::fabs(p.k.y - 50) < 1e-3 && std::fabs(p.k.heading - 45) < 1e-3;
    std::printf("  [forget] last packet of player 5 at %.0f ms; alive@+4.9s sample+10*count=%d, @+5.1s sample=%d count=%d, pruned=%zu\n",
                lastRecv5, aliveSample, afterSample, goneBefore, gone.size());
    Check("a player silent for 4.9 s is still shown (3 tracked); nothing is pruned early",
          aliveSample == 31 && prunedEarly == 0 && checkedAlive == 1, F("aliveSample=%d prunedEarly=%zu alive@6s=%d", aliveSample, prunedEarly, checkedAlive));
    Check("a player silent for 5.1 s is forgotten: Sample false, Prune reports exactly that id, count drops to 2",
          afterSample == 0 && goneBefore == 3 && gone.size() == 1 && gone[0] == 5 && rs.Count() == 2,
          F("sample=%d countBefore=%d gone=%zu count=%zu", afterSample, goneBefore, gone.size(), rs.Count()));
    Check("players still sending are never forgotten; a standing player (identical state every tick) stays and holds its pose",
          still6Ok && still7Ok && sevenHolds, F("6ok=%d 7ok=%d 7holds=%d", int(still6Ok), int(still7Ok), int(sevenHolds)));

    // The id comes back (a new session, ticks restart at 3): it is a fresh player, not "stale".
    const PushResult back = rs.OnPlayerState(5, 3, stateOf(5, 1), 12100);
    const bool shown = rs.Sample(5, 12116, p);
    rs.Remove(5);
    Check("a forgotten id can return with a lower tick (fresh state), and Remove drops it at once",
          back == PushResult::Accepted && shown && rs.Count() == 2 && !rs.Sample(5, 12200, p),
          F("push=%d shown=%d count=%zu", int(back), int(shown), rs.Count()));
}

// ---- 10b: sample age (protocol v2 WorldEntry::ageMs) --------------------------

struct AgedRun {
    size_t errN = 0, shown = 0, extrap = 0;
    double maxErr = 0, minLat = 1e9, maxLat = -1e9, delay = 0;
    uint32_t accepted = 0, repeats = 0;
    bool agesIrregular = false; // the stream really had varying ages (min < 5, max > 40)
};

// A client sending at 21 Hz (every 47.6 ms, from 3 ms) walking +x at a constant
// 6 m/s, behind a 20 Hz server that sends it every tick (near tier) with the age of
// the newest sample it has: floor(tick time - receive time), so the ages saw-tooth
// over 0..47 ms and one sample in 21 is never sent. The truth is on the server's
// receive timeline (uplink 0). Delivered 60 ms after the tick, stamped at the 60 fps
// frame, through RemoteSync::OnWorldState. useAge false zeroes every ageMs: the
// placement of an age-blind smoother. Error from 4 s on.
static AgedRun RunAged(bool useAge) {
    const double tickMs = 50, period = 1000.0 / 21, frameMs = 1000.0 / 60, endMs = 20000;
    const uint32_t tick0 = 5000;
    auto truth = [](double t) { return K(6.0 * t / 1000.0, 5, 30, -90, 6, 0, 0); };
    struct Pkt { double at; uint32_t tick; double sampleMs; uint8_t age; };
    std::vector<Pkt> pkts;
    int minAge = 1000, maxAge = -1;
    for (int n = 0; n * tickMs <= endMs; ++n) {
        const double tt = n * tickMs;
        if (tt < 3) continue; // no sample yet
        const double s = 3 + std::floor((tt - 3) / period) * period; // newest sample by the tick
        const int age = std::min(255, int(std::floor(tt - s)));
        minAge = std::min(minAge, age);
        maxAge = std::max(maxAge, age);
        pkts.push_back({tt + 60, tick0 + uint32_t(n), s, uint8_t(age)});
    }
    double refOffset = 1e18; // the fastest packet's stamp past its tick (the clock baseline)
    for (const Pkt& p : pkts) refOffset = std::min(refOffset, std::ceil(p.at / frameMs - 1e-9) * frameMs - (p.tick - tick0) * tickMs);

    AgedRun r;
    r.agesIrregular = minAge < 5 && maxAge > 40;
    RemoteSync rs;
    size_t next = 0;
    for (long f = 0;; ++f) {
        const double now = double(f) * frameMs;
        if (now > endMs) break;
        while (next < pkts.size() && pkts[next].at <= now) {
            proto::WorldState ws;
            ws.tick = pkts[next].tick;
            proto::WorldEntry e;
            e.id = 12;
            e.k = truth(pkts[next].sampleMs);
            e.ageMs = useAge ? pkts[next].age : uint8_t(0);
            ws.players.push_back(e);
            rs.OnWorldState(ws, now);
            ++next;
        }
        Pose p;
        if (!rs.Sample(12, now, p)) continue;
        ++r.shown;
        if (now < 4000) continue;
        if (p.mode == PoseMode::Extrapolating) ++r.extrap;
        const double shownMs = (p.renderTick - tick0) * tickMs;
        r.maxErr = std::max(r.maxErr, Dist(p.k, truth(shownMs)));
        r.minLat = std::min(r.minLat, now - refOffset - shownMs);
        r.maxLat = std::max(r.maxLat, now - refOffset - shownMs);
        ++r.errN;
    }
    if (const PlayerSmoother* ps = rs.Find(12)) {
        r.delay = ps->DelayMs();
        r.accepted = ps->Stats().accepted;
        r.repeats = ps->Stats().repeats;
    }
    return r;
}

static void TestSampleAge() {
    const AgedRun aged = RunAged(true), blind = RunAged(false);
    std::printf("  [sample age, 21 Hz client] placed: err max=%.4f m extrap=%zu/%zu delay %.0f ms latency %.0f..%.0f  |  "
                "age-blind: err max=%.4f m  accepted=%u repeats=%u\n",
                aged.maxErr, aged.extrap, aged.errN, aged.delay, aged.minLat, aged.maxLat, blind.maxErr, aged.accepted, aged.repeats);
    // v * the largest age = 6 m/s * 47 ms = 0.28 m: what ignoring the age costs here.
    Check("sample age: the stream has saw-tooth ages and the age-blind placement is off by v*age (max err > 15 cm)",
          aged.agesIrregular && blind.errN > 900 && blind.maxErr > 0.15, F("irregular=%d n=%zu max %.4f m", int(aged.agesIrregular), blind.errN, blind.maxErr));
    // Placed at tick*tickMs - ageMs the error is the age's whole-ms rounding (< 1 ms * 6 m/s = 6 mm).
    Check("sample age: placed at tick*tickMs - ageMs the pose tracks the truth (max err < 1.5 cm, < 1/10 of age-blind), "
          "latency in band, extrapolating < 3% of frames",
          aged.errN > 900 && aged.maxErr < 0.015 && aged.maxErr < 0.1 * blind.maxErr && aged.minLat >= 70 && aged.maxLat <= 305 &&
              double(aged.extrap) < 0.03 * double(aged.errN),
          F("n=%zu max %.4f m (blind %.4f), latency %.0f..%.0f, extrap %zu", aged.errN, aged.maxErr, blind.maxErr, aged.minLat, aged.maxLat, aged.extrap));
}

// A player walks +x at 6 m/s (one fresh sample per tick, age 10 ms) for 2 s, stops at
// tick 40 and goes quiet. The server re-sends that last sample every tick while it is
// not yet idle (ages 60, 110, ... saturating at 255), then at 1 Hz. At tick 44 the
// entry carries a state 4 mm off with age 250: a sample placed at 1950 ms, before the
// newest stored one (1990 ms), so not new information whatever its bits say.
static void TestIdleAndLeft() {
    RemoteSync rs;
    const uint16_t id = 21;
    const double stopX = 6.0 * 1990 / 1000.0; // where the walk is at the stop sample's time
    std::vector<std::pair<uint32_t, proto::WorldEntry>> sends;
    for (uint32_t n = 0; n <= 200; ++n) {
        proto::WorldEntry e;
        e.id = id;
        const double sampleMs = (n <= 40 ? n : 40) * 50.0 - 10.0; // received 10 ms before its tick
        e.ageMs = uint8_t(std::min(255.0, n * 50.0 - sampleMs));
        const bool moving = n < 40;
        e.k = K(moving ? 6.0 * sampleMs / 1000.0 : stopX, 5, 30, -90, moving ? 6 : 0, 0, 0); // n = 40: the stop, placed at 1990 ms
        if (n == 44) { e.k.x += 0.004f; e.ageMs = 250; }  // re-sent, placed at 1950 ms
        const bool idle = n > 50; // unchanged for 500 ms: 1 Hz
        if (idle && n % 20 != 0) continue;
        sends.push_back({n, e});
    }
    size_t next = 0, idleRepeats = 0, idleSends = 0;
    bool allRepeat = true, alwaysShown = true;
    double holdFrom = 0, holdMaxMove = 0, lastX = 0;
    PoseMode lastMode = PoseMode::Buffering;
    Pose p;
    for (long f = 0;; ++f) {
        const double now = double(f) * (1000.0 / 60);
        if (now > 10100) break; // the last send (tick 200) arrives at 10060
        while (next < sends.size() && sends[next].first * 50.0 + 60 <= now) {
            const uint32_t n = sends[next].first;
            proto::WorldState ws;
            ws.tick = 300 + n;
            ws.players.push_back(sends[next].second);
            const size_t stored = rs.OnWorldState(ws, now);
            if (n > 40) {
                ++idleSends;
                if (stored == 0) ++idleRepeats; else allRepeat = false;
            }
            ++next;
        }
        if (!rs.Sample(id, now, p)) { if (now > 200) alwaysShown = false; continue; }
        if (now >= 7000) {
            if (holdFrom == 0) { holdFrom = now; lastX = p.k.x; }
            holdMaxMove = std::max(holdMaxMove, std::fabs(double(p.k.x) - lastX));
            lastMode = p.mode;
        }
    }
    const PlayerSmoother* ps = rs.Find(id);
    const uint32_t accepted = ps ? ps->Stats().accepted : 0, repeats = ps ? ps->Stats().repeats : 0;
    const double delay = ps ? ps->DelayMs() : -1;
    std::printf("  [idle re-sends] idle sends %zu, repeats %zu, accepted=%u repeats=%u, held x=%.4f (stop %.4f) moved %.5f m, delay %.0f ms\n",
                idleSends, idleRepeats, accepted, repeats, lastX, stopX, holdMaxMove, delay);
    Check("idle re-sends with growing ageMs are Repeats, and so is a re-sent sample placed before the newest (4 mm off): exactly 41 stored",
          idleSends == 18 && idleRepeats == idleSends && allRepeat && accepted == 41 && repeats == 18,
          F("idle sends %zu repeats %zu accepted=%u repeats=%u", idleSends, idleRepeats, accepted, repeats));
    Check("...the idle pose holds still at the stop (Holding, < 1 mm over 3 s), stays shown at 1 Hz, idle ages do not inflate the delay (<= 150 ms)",
          alwaysShown && lastMode == PoseMode::Holding && holdMaxMove < 1e-3 && std::fabs(lastX - stopX) < 0.005 && delay > 0 && delay <= 150,
          F("shown=%d mode=%d moved %.5f x=%.4f delay %.0f", int(alwaysShown), int(lastMode), holdMaxMove, lastX, delay));

    // WorldState.left: the caller removes the id; the ped goes at once, and the id comes
    // back fresh (a lower tick is not "stale" for a new entry).
    proto::WorldState ws;
    ws.tick = 600;
    ws.left.push_back(id);
    for (uint16_t gone : ws.left) rs.Remove(gone);
    const bool goneNow = !rs.Sample(id, 10100, p) && rs.Find(id) == nullptr && rs.Count() == 0;
    proto::WorldEntry back;
    back.id = id; back.k = K(1, 2, 3); back.ageMs = 20;
    ws.left.clear();
    ws.tick = 400; // older than the last tick the removed entry saw
    ws.players.push_back(back);
    const size_t stored = rs.OnWorldState(ws, 10120);
    Check("Remove for a WorldState.left id drops the player at once (Sample false, Find null, Count 0); the id returns fresh",
          goneNow && stored == 1 && rs.Count() == 1 && rs.Sample(id, 10140, p),
          F("gone=%d stored=%zu count=%zu", int(goneNow), stored, rs.Count()));
}

// ---- 10c: ped state pass-through (protocol v3 WorldEntry::ped) ----------------

static proto::PedState Ped(uint8_t move, uint16_t flags = 0, uint8_t jumpSeq = 0, uint32_t weapon = 0) {
    proto::PedState s;
    s.move = move; s.flags = flags; s.jumpSeq = jumpSeq; s.weapon = weapon;
    return s;
}

// The tick whose snapshot is the newest at or before the pose's render time (snapshots
// placed at tick * 50 ms, age 0, origin tick 0); -1 when the render time is too close to
// a boundary to tell.
static long ShownTick(const Pose& p) {
    const double r = p.renderTick;
    if (std::fabs(r - std::round(r)) < 1e-6) return -1;
    return long(std::floor(r));
}

static void TestPedState() {
    // A: a walker switches gait and weapon at tick 40. Every interpolated frame shows the
    // PedState of the snapshot at or before its render time: never interpolated, never early.
    {
        PlayerSmoother sm(SmootherConfig::ForTickHz(20));
        std::vector<Ev> evs;
        std::vector<proto::PedState> peds;
        for (uint32_t n = 0; n <= 80; ++n) {
            evs.push_back({n * 50.0 + 40.0, n, K(1.5 * n * 0.05, 0, 30, 90, 1.5)});
            peds.push_back(n < 40 ? Ped(proto::kMoveWalk, 0, 0, 111) : Ped(proto::kMoveRun, 0, 0, 222));
        }
        size_t frames = 0, before = 0, after = 0, wrong = 0;
        size_t next = 0;
        for (long f = 0;; ++f) {
            const double now = f * (1000.0 / 60.0);
            if (now > 4500) break;
            while (next < evs.size() && evs[next].at <= now) {
                sm.Push(evs[next].tick, evs[next].k, now, 0.0, peds[next]);
                ++next;
            }
            Pose p;
            if (!sm.Sample(now, p) || p.mode != PoseMode::Interpolating) continue;
            const long t = ShownTick(p);
            if (t < 0 || t > 80) continue;
            ++frames;
            const proto::PedState& want = peds[size_t(t)];
            if (t < 40) ++before; else ++after;
            if (p.ped.move != want.move || p.ped.weapon != want.weapon) ++wrong;
        }
        Check("PedState is discrete and follows the render time: the snapshot at or before it, walk/111 then run/222 (>= 60 frames each side, 0 wrong)",
              before >= 60 && after >= 60 && wrong == 0,
              F("frames %zu (before %zu, after %zu) wrong %zu", frames, before, after, wrong));
    }

    // B: a standing player who starts aiming: equal kinematics, new PedState. That snapshot is
    // stored (not a Repeat), the identical ones after it are Repeats, and the pose shows the aim.
    {
        PlayerSmoother sm(SmootherConfig::ForTickHz(20));
        PushResult r30 = PushResult::Invalid;
        size_t laterRepeats = 0, frames = 0, aimShownEarly = 0;
        Pose last;
        proto::PedState aim = Ped(proto::kMoveIdle, proto::kPfAiming, 0, 0x1B06D571u);
        aim.aimX = 1.0f; aim.aimY = 2.0f; aim.aimZ = 0.5f;
        for (uint32_t n = 0; n <= 60; ++n) {
            const double now = n * 50.0 + 40.0;
            const PushResult r = sm.Push(n, K(5, 5, 30), now, 0.0, n < 30 ? Ped(proto::kMoveIdle) : aim);
            if (n == 30) r30 = r;
            if (n > 30 && r == PushResult::Repeat) ++laterRepeats;
            Pose p;
            if (sm.Sample(now, p)) {
                ++frames;
                if (p.renderTick < 29.9 && (p.ped.flags & proto::kPfAiming)) ++aimShownEarly;
                last = p;
            }
        }
        Check("a ped-only change of a standing player is stored (Accepted, not Repeat), identical ones after it are Repeats, the pose shows its aim",
              r30 == PushResult::Accepted && laterRepeats == 30 && frames == 61 && aimShownEarly == 0 &&
                  last.ped.flags == proto::kPfAiming && last.ped.weapon == 0x1B06D571u && last.ped.aimX == 1.0f &&
                  last.ped.aimY == 2.0f && last.ped.aimZ == 0.5f,
              F("tick 30 -> %d, later repeats %zu, frames %zu, early %zu, last flags 0x%x weapon 0x%x aim %.1f %.1f %.1f",
                int(r30), laterRepeats, frames, aimShownEarly, unsigned(last.ped.flags), unsigned(last.ped.weapon),
                last.ped.aimX, last.ped.aimY, last.ped.aimZ));
    }

    // B2: a standing, aiming shooter whose only change is the shot counter (PedState.shotSeq).
    // Each new count is new information: stored, not a Repeat, so the pose reaches the receiver's
    // PlayPeerShots while the shooter stands still (it used to wait for the shooter to move).
    {
        PlayerSmoother sm(SmootherConfig::ForTickHz(20));
        proto::PedState aim = Ped(proto::kMoveIdle, proto::kPfAiming, 0, 0x1B06D571u);
        aim.aimX = 1.0f; aim.aimY = 2.0f; aim.aimZ = 0.5f;
        size_t shotAccepted = 0, sameRepeats = 0;
        uint8_t lastShownSeq = 0;
        for (uint32_t n = 0; n <= 40; ++n) {
            const double now = n * 50.0 + 40.0;
            proto::PedState ps = aim;
            ps.shotSeq = (uint8_t)((n >= 20 && n < 30) ? (n - 19) : (n >= 30 ? 10 : 0));   // 1..10 over ticks 20..29
            const PushResult r = sm.Push(n, K(5, 5, 30), now, 0.0, ps);
            if (n >= 20 && n < 30 && r == PushResult::Accepted) ++shotAccepted;
            if (n > 30 && r == PushResult::Repeat) ++sameRepeats;
            Pose p;
            if (sm.Sample(now, p))
                lastShownSeq = p.ped.shotSeq;
        }
        Check("a standing shooter's shot-counter-only change is stored (not a Repeat); identical ones after it are Repeats; the pose shows the last count",
              shotAccepted == 10 && sameRepeats == 10 && lastShownSeq == 10,
              F("shots accepted %zu of 10, later repeats %zu, last shown shotSeq %u", shotAccepted, sameRepeats,
                unsigned(lastShownSeq)));
    }

    // C: a jump whose only kPfJumping snapshot the render time skips (3 fps: ~6.7 ticks per
    // frame). jumpSeq counts, so every frame after it still shows the jump happened. The ped
    // block does not change the timing, so a first pass finds a tick in 15..35 no frame shows
    // and the second pass puts the jump there.
    {
        struct JumpRun { size_t before = 0, after = 0, wrong = 0, showedJump = 0; std::vector<long> shown; };
        auto run = [](long jumpTick) {
            JumpRun jr;
            PlayerSmoother sm(SmootherConfig::ForTickHz(20));
            std::vector<Ev> evs;
            std::vector<proto::PedState> peds;
            for (uint32_t n = 0; n <= 60; ++n) {
                evs.push_back({n * 50.0 + 40.0, n, K(4.0 * n * 0.05, 0, 30, 90, 4.0)});
                peds.push_back(Ped(proto::kMoveRun, long(n) == jumpTick ? proto::kPfJumping : 0,
                                   jumpTick >= 0 && long(n) >= jumpTick ? 1 : 0));
            }
            size_t next = 0;
            for (long f = 0;; ++f) {
                const double now = f * (1000.0 / 3.0);
                if (now > 3000) break;
                while (next < evs.size() && evs[next].at <= now) {
                    sm.Push(evs[next].tick, evs[next].k, now, 0.0, peds[next]);
                    ++next;
                }
                Pose p;
                if (!sm.Sample(now, p) || p.mode != PoseMode::Interpolating) continue;
                const long t = ShownTick(p);
                if (t < 0) continue;
                jr.shown.push_back(t);
                if (t == jumpTick || (p.ped.flags & proto::kPfJumping)) ++jr.showedJump;
                if (t < jumpTick) { ++jr.before; if (p.ped.jumpSeq != 0) ++jr.wrong; }
                else              { ++jr.after;  if (p.ped.jumpSeq != 1) ++jr.wrong; }
            }
            return jr;
        };
        const JumpRun probe = run(-1);
        long jumpTick = -1;
        for (long t = 15; t <= 35 && jumpTick < 0; ++t)
            if (std::find(probe.shown.begin(), probe.shown.end(), t) == probe.shown.end())
                jumpTick = t;
        const JumpRun jr = run(jumpTick);
        Check("a skipped jump snapshot is not lost: jumpSeq 0 before, 1 on every frame after (the kPfJumping tick itself never shown)",
              jumpTick > 0 && jr.before >= 2 && jr.after >= 2 && jr.wrong == 0 && jr.showedJump == 0,
              F("jump at tick %ld: before %zu after %zu wrong %zu shown-jump %zu", jumpTick, jr.before, jr.after,
                jr.wrong, jr.showedJump));
    }

    // D: RemoteSync hands WorldEntry::ped through; past the newest snapshot (extrapolating,
    // then holding) the pose shows the newest PedState.
    {
        RemoteSync rs;
        const uint16_t id = 9;
        for (uint32_t n = 0; n <= 10; ++n) {
            proto::WorldState ws;
            ws.tick = n;
            proto::WorldEntry e;
            e.id = id;
            e.k = K(n * 0.3, 0, 30, 0, 6);
            e.ped = n < 10 ? Ped(proto::kMoveSprint, 0, 0, 5) : Ped(proto::kMoveSprint, proto::kPfShooting, 2, 77);
            ws.players.push_back(e);
            rs.OnWorldState(ws, n * 50.0 + 40.0);
        }
        Pose pe, ph;
        const bool okE = rs.Sample(id, 10 * 50.0 + 40.0 + 150.0, pe); // past the newest by ~50 ms
        const bool okH = rs.Sample(id, 10 * 50.0 + 40.0 + 1500.0, ph); // past the extrapolation cap
        Check("RemoteSync::OnWorldState carries WorldEntry::ped; extrapolating and holding show the newest (sprint, shooting, seq 2, weapon 77)",
              okE && okH && pe.mode == PoseMode::Extrapolating && ph.mode == PoseMode::Holding &&
                  pe.ped.flags == proto::kPfShooting && pe.ped.weapon == 77 && pe.ped.jumpSeq == 2 &&
                  ph.ped.flags == proto::kPfShooting && ph.ped.weapon == 77 && ph.ped.move == proto::kMoveSprint,
              F("modes %d/%d flags 0x%x/0x%x weapon %u/%u seq %u", int(pe.mode), int(ph.mode), unsigned(pe.ped.flags),
                unsigned(ph.ped.flags), unsigned(pe.ped.weapon), unsigned(ph.ped.weapon), unsigned(pe.ped.jumpSeq)));
    }
}

// ---- 11: config -------------------------------------------------------------

static void TestConfig() {
    const SmootherConfig d, c20 = SmootherConfig::ForTickHz(20);
    Check("ForTickHz(20) equals the documented defaults (100 / 75 / 300 / 25 ms, snap 8 m)",
          c20.tickHz == d.tickHz && c20.initialDelayMs == 100 && c20.minDelayMs == 75 && c20.maxDelayMs == 300 &&
          c20.delaySafetyMs == 25 && d.initialDelayMs == 100 && d.minDelayMs == 75 && d.maxDelayMs == 300 &&
          d.snapDistance == 8.0f && c20.snapDistance == 8.0f);
    const SmootherConfig c60 = SmootherConfig::ForTickHz(60), c4 = SmootherConfig::ForTickHz(4), cbad = SmootherConfig::ForTickHz(0);
    Check("ForTickHz scales initial/min with the tick (60 Hz: 33.3/25 ms; 4 Hz: 500/375 ms, max 1500) and falls back to 20 on 0 / NaN",
          std::fabs(c60.initialDelayMs - 100.0 / 3) < 1e-9 && std::fabs(c60.minDelayMs - 25) < 1e-9 && c60.maxDelayMs == 300 &&
          c4.initialDelayMs == 500 && c4.minDelayMs == 375 && c4.maxDelayMs == 1500 && cbad.tickHz == 20 &&
          SmootherConfig::ForTickHz(NAN).tickHz == 20,
          F("60Hz initial %.2f min %.2f, 4Hz initial %.0f min %.0f max %.0f", c60.initialDelayMs, c60.minDelayMs, c4.initialDelayMs, c4.minDelayMs, c4.maxDelayMs));
    SmootherConfig bad;
    bad.tickHz = NAN; bad.minDelayMs = -5; bad.maxDelayMs = 0; bad.initialDelayMs = 1e9; bad.maxSlew = NAN; bad.snapDistance = -1;
    PlayerSmoother sm(bad);
    sm.Push(0, K(0), 0);
    Pose p;
    Check("a nonsense config is sanitised and the smoother still works",
          sm.Sample(0, p) && sm.DelayMs() >= 75 && sm.DelayMs() <= 300, F("delay %.1f", sm.DelayMs()));

    // The fallback for an unusable snapDistance is the default, not a second constant:
    // with -1 a 9 m jump snaps and a 7 m jump does not (the default is 8 m).
    SmootherConfig noSnap;
    noSnap.snapDistance = -1;
    PlayerSmoother over(noSnap), under(noSnap);
    over.Push(0, K(0), 0);
    under.Push(0, K(0), 0);
    const PushResult r9 = over.Push(1, K(9), 50);
    const PushResult r7 = under.Push(1, K(7), 50);
    Check("an unusable snapDistance falls back to the 8 m default: a 9 m jump snaps, a 7 m jump does not",
          r9 == PushResult::Teleported && r7 == PushResult::Accepted, F("9 m -> %d, 7 m -> %d", int(r9), int(r7)));
}

// ---- 12. SampleClock: the sender's u16 sample time on the server's clock (protocol v5) -----------------

struct ClockEv { uint64_t recvUs; uint16_t stamp; double trueMs; };

// `n` samples every `periodMs` from a sender whose clock reads `senderStartMs + t` when the server's reads t, each
// delayed by baseMs + U[-jitterMs, +jitterMs]. Returned in ARRIVAL order (jitter wider than the period reorders
// neighbours). The server clock starts at kServerEpochUs so that "receive time - true time" is a large number.
static const uint64_t kServerEpochUs = 5000000000ull;
static std::vector<ClockEv> MakeClockEvents(Rng& rng, int n, double periodMs, double baseMs, double jitterMs, uint32_t senderStartMs) {
    std::vector<ClockEv> evs;
    for (int i = 0; i < n; i++) {
        const double t = i * periodMs;
        const double delay = baseMs + (rng.Uniform() * 2.0 - 1.0) * jitterMs;
        ClockEv e;
        e.trueMs = t;
        e.stamp = uint16_t((uint64_t(senderStartMs) + uint64_t(t)) & 0xFFFFu); // integer ms on the sender, wraps every 65.536 s
        e.recvUs = kServerEpochUs + uint64_t((t + delay) * 1000.0);
        evs.push_back(e);
    }
    std::stable_sort(evs.begin(), evs.end(), [](const ClockEv& a, const ClockEv& b) { return a.recvUs < b.recvUs; });
    return evs;
}

// stddev of (placed - true) in ms over the samples from index `from` on.
static double PlacementStddev(const std::vector<ClockEv>& evs, size_t from, SampleClock& clock, double* worstFromMedian = nullptr,
                              bool naive = false) {
    std::vector<double> err;
    for (size_t i = 0; i < evs.size(); i++) {
        const uint64_t placed = clock.Place(evs[i].stamp, evs[i].recvUs);
        if (i < from) continue;
        const uint64_t at = naive ? evs[i].recvUs : placed;
        err.push_back((double(at) - double(kServerEpochUs) - evs[i].trueMs * 1000.0) / 1000.0);
    }
    double mean = 0;
    for (double e : err) mean += e;
    mean /= double(err.size());
    double var = 0, worst = 0;
    for (double e : err) {
        var += (e - mean) * (e - mean);
        worst = std::max(worst, std::fabs(e - mean));
    }
    if (worstFromMedian) *worstFromMedian = worst;
    return std::sqrt(var / double(err.size()));
}

static void TestSampleClock() {
    // 22.2 ms samples (a 45 Hz sender), receipts jittered +-12 ms around 30 ms: the placed sample time is within 1.5 ms (stddev)
    // of the truth; placing at receipt time (what the server did before v5) is off by about 7 ms (uniform +-12 ms).
    {
        Rng rng(0x5A3C);
        const std::vector<ClockEv> evs = MakeClockEvents(rng, 1800, 22.2, 30.0, 12.0, 10000);
        SampleClock clock;
        double worst = 0, worstNaive = 0;
        const double sd = PlacementStddev(evs, 20, clock, &worst);
        SampleClock unused;
        const double naive = PlacementStddev(evs, 20, unused, &worstNaive, true);
        Check("SampleClock: 22.2 ms samples with +-12 ms receipt jitter are placed within 1.5 ms (stddev), against about 7 ms for placing at receipt",
              evs.size() == 1800 && sd <= 1.5 && naive > 5.0, F("placed %.2f ms (worst %.2f), at receipt %.2f ms", sd, worst, naive));
    }

    // The u16 sample time wraps every 65.536 s: a stream that crosses the wrap is unwrapped by the signed delta and placed
    // as well as one that does not; 3 minutes cross it twice.
    {
        Rng rng(0xBEEF);
        const std::vector<ClockEv> evs = MakeClockEvents(rng, 8100, 22.2, 30.0, 12.0, 65536 - 700);
        SampleClock clock;
        double worst = 0;
        const double sd = PlacementStddev(evs, 20, clock, &worst);
        bool wrapped = false;
        for (size_t i = 1; i < evs.size(); i++) wrapped = wrapped || evs[i].stamp < evs[i - 1].stamp;
        Check("SampleClock: a stream that wraps the u16 sample time twice (3 minutes) keeps its 1.5 ms placement; no sample is ever off by a wrap",
              wrapped && sd <= 1.5 && worst < 40.0, F("%.2f ms stddev, worst %.2f ms from the mean", sd, worst));
    }

    // Edges: the first 8 samples are placed at receipt; a gap over 30 s re-anchors (a sender clock that jumped is followed at once);
    // a sender that freezes or races its clock is clamped to [receipt - 300 ms, receipt].
    {
        Rng rng(0x1234);
        SampleClock clock;
        std::string bad;
        const std::vector<ClockEv> evs = MakeClockEvents(rng, 200, 22.2, 30.0, 12.0, 4000);
        for (size_t i = 0; i < 8; i++)
            if (clock.Place(evs[i].stamp, evs[i].recvUs) != evs[i].recvUs) bad += "[warm-up " + std::to_string(i) + "]";
        bool inside = true;
        for (size_t i = 8; i < evs.size(); i++) {
            const uint64_t p = clock.Place(evs[i].stamp, evs[i].recvUs);
            if (p > evs[i].recvUs || p + 300000 < evs[i].recvUs) inside = false;
        }
        if (!inside) bad += "[outside the clamp]";
        // 40 s of silence, then the sender's clock is 20 s ahead: re-anchored, then tracked.
        const uint64_t resumeUs = evs.back().recvUs + 40000000ull;
        std::vector<ClockEv> after = MakeClockEvents(rng, 200, 22.2, 30.0, 12.0, 4000 + 20000 + 4444 + 40000);
        for (ClockEv& e : after) e.recvUs += resumeUs - kServerEpochUs;
        const uint64_t first = clock.Place(after[0].stamp, after[0].recvUs);
        if (first != after[0].recvUs) bad += "[no re-anchor after 30 s]";
        std::vector<double> err;
        for (size_t i = 1; i < after.size(); i++) {
            const uint64_t p = clock.Place(after[i].stamp, after[i].recvUs);
            if (i >= 30) err.push_back((double(p) - double(resumeUs) - after[i].trueMs * 1000.0) / 1000.0); // the fastest path's delay, about 17 ms
        }
        double lo = 1e9, hi = -1e9;
        for (double e : err) { lo = std::min(lo, e); hi = std::max(hi, e); }
        if (err.empty() || hi - lo > 4.0) bad += F("[tracking after re-anchor %.1f..%.1f]", lo, hi);
        // A frozen sender clock: the offset only grows, so the placement falls behind and is held at receipt - 300 ms.
        SampleClock frozen;
        uint64_t last = 0, recv = 0;
        for (int i = 0; i < 400; i++) {
            recv = kServerEpochUs + uint64_t(i) * 22200ull;
            last = frozen.Place(777, recv);
        }
        if (last != recv - 300000) bad += F("[frozen: %lld]", (long long)(int64_t(last) - int64_t(recv)));
        // A sender whose clock races at 3x: placed samples never lie in the future of their receipt.
        SampleClock fast;
        bool never = true;
        for (int i = 0; i < 400; i++) {
            recv = kServerEpochUs + uint64_t(i) * 22200ull;
            const uint64_t p = fast.Place(uint16_t((uint64_t(i) * 66600ull / 1000ull) & 0xFFFF), recv);
            if (p > recv || p + 300000 < recv) never = false;
        }
        if (!never) bad += "[racing clock outside the clamp]";
        Check("SampleClock: the first 8 samples are placed at receipt, everything is within [receipt - 300 ms, receipt], a 30 s gap re-anchors, "
              "a frozen sender clock is held at receipt - 300 ms and a racing one never lands in the future",
              bad.empty(), bad);
    }
}

// ---- 12: the one render clock (sync plan W7, libs/sync/netclock.h) -----------------------------------------

static const double kTick30 = 1000.0 / 30;

// Tick `first + i` of a 30 Hz server arrives at base + i * tick + extra(i): a constant offset (so the baseline is
// `base`) plus whatever extra(i) adds (that is the frame's excess). Frames are fed in arrival order.
template <class Extra>
static void FeedTicks(NetClock& clk, uint32_t first, long from, long to, double base, Extra extra) {
    for (long i = from; i < to; ++i) clk.OnFrame(first + uint32_t(i), base + double(i) * kTick30 + extra(i));
}

static void TestNetClock() {
    const NetClockConfig cfg = NetClockConfig::ForTickHz(30);
    const auto calm = [](long) { return 0.0; };

    // 1. A perfect 30 Hz stream read at 60 fps for 20 s (the delay starts at 2 ticks and falls to 1.5 with a 2.5 s time
    // constant, so the render clock is judged from 16 s on).
    {
        NetClock clk(cfg);
        double maxErr = 0;
        size_t n = 0;
        uint32_t tick = 100;
        double nextAt = 5040;
        for (long f = 0; f < 1200; ++f) {
            const double now = 5000 + double(f) * (1000.0 / 60);
            while (nextAt <= now) { clk.OnFrame(tick++, nextAt); nextAt += kTick30; }
            if (!clk.Ready()) continue;
            const double t = clk.RenderT(now);
            if (now >= 16000) { maxErr = std::max(maxErr, std::fabs(t - (now - 5040 - 50.0))); ++n; }
        }
        Check("NetClock: a perfect 30 Hz stream settles at 1.5 ticks (50 ms), the baseline is its constant offset and the render clock "
              "shows now - baseline - delay within 1 ms without a resync",
              n >= 230 && std::fabs(clk.DelayMs() - 50.0) < 0.5 && std::fabs(clk.BaselineMs() - 5040.0) < 1e-6 && maxErr < 1.0 &&
                  clk.Resyncs() == 0,
              F("n=%zu delay %.2f baseline %.3f max err %.3f ms resyncs %u", n, clk.DelayMs(), clk.BaselineMs(), maxErr, clk.Resyncs()));
    }

    // 2. The delay rises at once: after 8 calm seconds, 14 frames that are 90 ms late. The 95th percentile of the
    // excess (3 s of frames, 5% of them is 5) reaches 90 with the sixth late frame, and the delay follows it that frame.
    {
        NetClock clk(cfg);
        FeedTicks(clk, 1000, 0, 240, 1040, calm);
        const double before = clk.DelayMs();
        double prev = before, top = 0;
        bool monotone = true;
        int firstHigh = -1;
        for (long i = 240; i < 254; ++i) {
            FeedTicks(clk, 1000, i, i + 1, 1040, [](long) { return 90.0; });
            const double d = clk.DelayMs();
            if (d < prev - 0.5) monotone = false; // (the floor-seeking tail of the calm fall may still shave a few hundredths)
            prev = d;
            top = std::max(top, d);
            if (firstHigh < 0 && d >= 135.0) firstHigh = int(i - 240);
        }
        Check("NetClock: the delay rises at once: 50 ms when calm, 140 ms (tick + 90 + half a tick) from the sixth 90 ms-late frame, never down while it rises",
              before < 51.0 && firstHigh >= 4 && firstHigh <= 6 && monotone && top > 139.0 && top < 141.0,
              F("calm %.2f ms, >=135 ms after %d late frames, top %.2f ms, monotone %d", before, firstHigh, top, int(monotone)));
    }

    // 3. The delay falls with wall time, not per packet: a 4 s burst (every other frame 80 ms late) raises it to ~130 ms;
    // then the link is calm and the frames come at 30 Hz or at 1 Hz. 10 s of wall time later both are back near the
    // 50 ms floor and within 3 ms of each other. A per-packet fall (the pre-W7 smoother: 2% of the gap per packet) would
    // leave the 1 Hz one at 130 - 80 * 0.98^10 = 115 ms.
    {
        auto after = [&](double hz, double* peak) {
            NetClock clk(cfg);
            FeedTicks(clk, 2000, 0, 120, 1040, calm);
            FeedTicks(clk, 2000, 120, 240, 1040, [](long i) { return (i & 1) ? 80.0 : 0.0; });
            if (peak) *peak = clk.DelayMs();
            const long step = long(30.0 / hz + 0.5);
            for (long i = 240; i <= 540; i += step) FeedTicks(clk, 2000, i, i + 1, 1040, calm);
            return clk.DelayMs();
        };
        double peak = 0;
        const double d30 = after(30, &peak), d1 = after(1, nullptr);
        const double perPacket = 50.0 + (peak - 50.0) * std::pow(0.98, 10.0);
        Check("NetClock: the delay falls with wall time, not per packet: after a burst (peak ~130 ms) and 10 s calm both 30 Hz and 1 Hz frames "
              "are back below 60 ms and within 3 ms of each other (a per-packet 2% fall would still be at 115 ms at 1 Hz)",
              peak > 125.0 && d30 < 60.0 && d1 < 60.0 && std::fabs(d30 - d1) < 3.0 && perPacket > 110.0,
              F("peak %.1f ms, +10 s: 30 Hz %.1f ms, 1 Hz %.1f ms (per-packet rule would give %.1f)", peak, d30, d1, perPacket));
    }

    // 4. A new session re-baselines. Session 1 runs at tick ~4e9 (a long-lived server); a restarted server counts from 1
    // and its frames are refused by the old clock (so the engine resets it in OnWelcome); after Reset() the clock is
    // empty (not Ready) and 5 s of session 2 later shows the tick of the new timeline, not the old one.
    {
        NetClock clk(cfg);
        FeedTicks(clk, 4000000000u, 0, 300, 100000, calm);
        const bool ready1 = clk.Ready();
        const bool refused = !clk.OnFrame(5, 110000) && clk.Stats().ignored == 1;
        clk.Reset();
        const bool empty = !clk.Ready() && clk.Stats().frames == 0 && clk.Resyncs() == 0;
        double worst = 0;
        size_t n = 0;
        uint32_t tick = 1;
        double nextAt = 110060;
        for (long f = 0; f < 540; ++f) { // 9 s at 60 fps from local time 110000
            const double now = 110000 + double(f) * (1000.0 / 60);
            while (nextAt <= now) { clk.OnFrame(tick++, nextAt); nextAt += kTick30; }
            if (!clk.Ready()) continue;
            const double t = clk.RenderT(now);
            if (now >= 116000) {
                const double serverTick = (now - 110060) / kTick30 + 1.0; // the tick the server is at (frames arrive 60 ms after their tick)
                worst = std::max(worst, std::fabs(clk.TickAt(t) - (serverTick - clk.DelayMs() / kTick30)));
                ++n;
            }
        }
        RemoteSync rs;
        proto::Kinematics k;
        rs.OnPlayerState(2, 77, k, 1000.0);
        const bool filled = rs.Clock().Ready() && rs.Count() == 1;
        rs.Clear();
        Check("NetClock: a restarted server's low ticks are refused by the old clock; Reset() (and RemoteSync::Clear) empty it, and the new "
              "session's render clock shows its own timeline (within 0.05 tick = 1.7 ms of the server tick minus the delay)",
              ready1 && refused && empty && n > 150 && worst < 0.05 && filled && !rs.Clock().Ready() && rs.Count() == 0,
              F("ready %d refused %d empty %d n=%zu worst %.4f tick", int(ready1), int(refused), int(empty), n, worst));
    }

    // 5. Ticks wrap (the clock unwraps by the signed distance), a reordered frame counts as jitter and does not move the timeline,
    // the same frame fed once per entry counts once, a frame a day away or a NaN time is refused, a saturated age is ignored.
    {
        NetClock clk(cfg);
        const uint32_t first = 0xFFFFFFF0u;
        FeedTicks(clk, first, 0, 40, 1000, calm);
        bool unwrapped = true;
        for (long i = 0; i < 40; ++i) unwrapped = unwrapped && clk.Unwrap(first + uint32_t(i)) == i;
        const double lastAt = 1000 + 39 * kTick30;
        const uint32_t f0 = clk.Stats().frames;
        const bool dup = clk.OnFrame(first + 39, lastAt) && clk.Stats().frames == f0;
        const bool late = clk.OnFrame(first + 38, lastAt + 5) && clk.Stats().frames == f0 + 1 && clk.Unwrap(first + 39) == 39;
        const bool far = !clk.OnFrame(first + 100000, lastAt + 6) && !clk.OnFrame(first + 40, NAN) && clk.Stats().ignored == 2;
        clk.NoteAge(255.0); clk.NoteAge(-1.0); clk.NoteAge(NAN);
        const double ageNone = clk.AgeP95Ms();
        clk.NoteAge(12.0);
        FeedTicks(clk, first, 40, 41, 1000, calm);
        Check("NetClock: ticks unwrap across 2^32, a reordered frame is accepted without moving the timeline, one frame fed per entry counts once, "
              "a frame 55 min away and a NaN time are refused, a saturated or negative age is ignored",
              unwrapped && dup && late && far && ageNone == 0.0 && std::fabs(clk.AgeP95Ms() - 12.0) < 1e-9 && clk.Ready(),
              F("unwrapped %d dup %d late %d far %d age %.1f", int(unwrapped), int(dup), int(late), int(far), clk.AgeP95Ms()));
    }

    // 6. The class extra: whole ticks beyond one, 0 within 1.5 ticks (the 22 / 44 ms alternation of a 45 Hz sender into a 30 Hz tick is
    // one class), and a slewed ClassDelay: at once the first time, then 10% of the elapsed time, idempotent for one time.
    {
        const double t = kTick30;
        const bool classes = ClassExtraMs(t, t) == 0.0 && ClassExtraMs(22.2, t) == 0.0 && ClassExtraMs(44.4, t) == 0.0 &&
                             std::fabs(ClassExtraMs(66.7, t) - t) < 1e-9 && std::fabs(ClassExtraMs(111.0, t) - 2 * t) < 1e-9 &&
                             ClassExtraMs(0.0, t) == 0.0 && ClassExtraMs(NAN, t) == 0.0 && ClassExtraMs(50.0, 0.0) == 0.0;
        ClassDelay cd;
        const double a = cd.Update(33.3, 1000, 0.10);
        const double b = cd.Update(0.0, 1100, 0.10);
        const double b2 = cd.Update(0.0, 1100, 0.10);
        const double c = cd.Update(0.0, 1500, 0.10);
        cd.Reset();
        Check("class extra: ClassExtraMs is (round(gap / tick) - 1) ticks, 0 up to 1.5 ticks; ClassDelay starts at its target, moves 10% of the elapsed time "
              "toward it (33.3 -> 23.3 in 100 ms), asks idempotently and arrives",
              classes && a == 33.3 && std::fabs(b - 23.3) < 1e-9 && b2 == b && c == 0.0 && cd.Value() == 0.0,
              F("classes %d a %.1f b %.2f b2 %.2f c %.2f", int(classes), a, b, b2, c));
    }
}


// ---- W8 item 2 and 8 (docs/sync-specs/live-0.4.2-fixes.md): class-extra hysteresis, standing-still settle -------------

// ClassHold alone: the first reading sets the class at once; a stream hovering at a boundary does not move it; a
// sustained change does (up after 1.5 s, down after 8 s), a short outlier does not.
static void TestClassHold() {
    const double t = kTick30;
    ClassHold h;
    const double first = h.Target(2.0 * t, t, 0);                       // first reading: class 2 at once
    // boundary jitter: raw 1.45 / 1.55 alternate for 20 s at 60 readings per s; the class never leaves 2
    bool stable = true;
    for (int i = 0; i < 1200; ++i) {
        const double raw = (i & 1) ? 1.55 : 1.45;
        const double e = h.Target(raw * t, t, 100.0 + i * 16.7);
        if (h.Held() != 2 || std::fabs(e - t) > 1e-9) stable = false;
    }
    const uint32_t afterBoundary = h.Changes();
    // outlier: raw 3.0 for 600 ms then back to 2: no change
    double now = 21000;
    for (int i = 0; i < 36; ++i, now += 16.7) h.Target(3.0 * t, t, now);
    for (int i = 0; i < 60; ++i, now += 16.7) h.Target(2.0 * t, t, now);
    const bool outlierIgnored = h.Held() == 2 && h.Changes() == afterBoundary;
    // sustained 3.0: not yet at 1.4 s, yes at 1.6 s
    double upAt = -1;
    const double start = now;
    for (int i = 0; i < 200; ++i, now += 16.7) {
        h.Target(3.0 * t, t, now);
        if (upAt < 0 && h.Held() == 3) upAt = now - start;
    }
    // sustained 1.0: not before 8 s, yes after
    double downAt = -1;
    const double start2 = now;
    for (int i = 0; i < 700; ++i, now += 16.7) {
        h.Target(1.0 * t, t, now);
        if (downAt < 0 && h.Held() == 1) downAt = now - start2;
    }
    Check("class hold: the first reading sets the class at once; boundary jitter (raw 1.45 / 1.55) and a 600 ms outlier change nothing; a sustained rise takes 1.5 s, a sustained fall 8 s",
          std::fabs(first - t) < 1e-9 && stable && afterBoundary == 0 && outlierIgnored && upAt >= 1480 && upAt <= 1560 && downAt >= 7980 && downAt <= 8060,
          F("first %.1f stable %d changes %u outlier ignored %d up after %.0f ms, down after %.0f ms", first, int(stable), afterBoundary, int(outlierIgnored), upAt, downAt));
}

// A player whose snapshots reach the client on a random ~55% of the server's ticks (a 20 Hz-pattern stream with jitter, the
// shape of a mid-tier player): the typical gap hovers around 1.8 ticks and the old class (round(gap / tick) - 1 ticks of
// extra, taken from the median of the last 16 gaps) flips between 1 and 2 ticks every few seconds; each flip slews the
// render time at 10% for ~0.3 s = a speed ripple. The extra of the shown player must change at most once per minute once
// settled (episodes of movement of ExtraMs after 10 s, over 120 s).
static void TestClassHysteresis() {
    const double tickMs = kTick30, frameMs = 1000.0 / 60, endMs = 130000, latency = 60, period = 1000.0 / 45;
    SmootherConfig sc = SmootherConfig::ForTickHz(30);
    sc.sharedClock = true;
    RemoteSync rs(sc);
    Walk w;
    Rng rng(0xFACE5);
    struct Pkt { double at; uint32_t tick; double sampleMs; uint8_t age; };
    std::vector<Pkt> pkts;
    std::vector<int> gaps;           // the oracle: gaps in ticks, the raw class from their upper median
    int lastTick = -1;
    int oracleClass = -1;
    long oracleFlips = 0;
    for (int i = 0; i * tickMs <= endMs; ++i) {
        if (i > 0 && rng.Uniform() >= 0.55) continue;
        const double tt = i * tickMs;
        const double s = std::floor((tt - 3) / period) * period;
        const double j = (rng.Uniform() * 2 - 1) * 12.0;
        pkts.push_back({tt + latency + j, uint32_t(1000 + i), s, uint8_t(std::min(255.0, std::floor(tt - s)))});
        if (lastTick >= 0) {
            gaps.push_back(i - lastTick);
            if (gaps.size() > 16) gaps.erase(gaps.begin());
            std::vector<int> g = gaps;
            std::sort(g.begin(), g.end());
            const int cls = g[g.size() / 2];
            if (tt > 10000) {
                if (oracleClass >= 0 && cls != oracleClass) ++oracleFlips;
                oracleClass = cls;
            }
        }
        lastTick = i;
    }
    std::stable_sort(pkts.begin(), pkts.end(), [](const Pkt& a, const Pkt& b) { return a.at < b.at; });

    size_t next = 0, frames = 0;
    double prevExtra = 0, lastMoveMs = -1e9, maxExtra = 0;
    long episodes = 0;
    for (long f = 0;; ++f) {
        const double now = double(f) * frameMs;
        if (now > endMs) break;
        while (next < pkts.size() && pkts[next].at <= now) {
            const Pkt& p = pkts[next++];
            rs.OnPlayerState(1, p.tick, w.At(p.sampleMs), now, p.age);
        }
        Pose pose;
        if (!rs.Sample(1, now, pose)) continue;
        const PlayerSmoother* ps = rs.Find(1);
        if (!ps) continue;
        const double extra = ps->ExtraMs();
        if (now >= 10000) {
            ++frames;
            maxExtra = std::max(maxExtra, extra);
            if (std::fabs(extra - prevExtra) > 1e-6) {
                if (now - lastMoveMs > 400.0) ++episodes;
                lastMoveMs = now;
            }
        }
        prevExtra = extra;
    }
    std::printf("  [class extra, ~55%% of 30 Hz ticks, 120 s] raw class (median of 16 gaps) flips %ld times; shown extra moved in %ld episodes (max extra %.1f ms)\n",
                oracleFlips, episodes, maxExtra);
    Check("class extra hysteresis: a jittered 20 Hz-pattern stream whose raw class flips >= 10 times in 120 s changes the shown extra at most twice (once per minute)",
          frames > 6500 && oracleFlips >= 10 && episodes <= 2,
          F("frames %zu raw flips %ld episodes %ld", frames, oracleFlips, episodes));
}

// Receiver settle (item 8): the last snapshot of a stream that ends says the player stands (PedState idle, no moving flag)
// although its kinematics still carry a running velocity (a default PedState from an alt-tabbed sender; a stale one): the
// pose moves at most stillExtrapolateMaxM (0.3 m) past it instead of running on 1.5 m and snapping back. A snapshot that
// says run keeps the old behaviour (the 250 ms extrapolation).
static void TestStillSettle() {
    struct R { double maxD = 0; size_t frames = 0, extrap = 0; };
    const auto run = [](const proto::PedState& lastPed, const SmootherConfig& cfg) {
        PlayerSmoother sm(cfg);
        R r;
        const double lastX = 19 * 0.5;
        size_t next = 0;
        for (long f = 0;; ++f) {
            const double now = double(f) * (1000.0 / 60);
            if (now > 4000) break;
            while (next < 20 && next * 50.0 <= now) {
                sm.Push(uint32_t(next), K(double(next) * 0.5, 0, 0, 90, 10, 0, 0), now, 0.0, next == 19 ? lastPed : Ped(proto::kMoveRun));
                ++next;
            }
            Pose p;
            if (!sm.Sample(now, p)) continue;
            ++r.frames;
            if (p.mode == PoseMode::Extrapolating) ++r.extrap;
            if (now >= 2300) r.maxD = std::max(r.maxD, double(p.k.x) - lastX);
        }
        return r;
    };
    const SmootherConfig cfg = SmootherConfig::ForTickHz(20);
    const R still = run(Ped(proto::kMoveIdle), cfg), running = run(Ped(proto::kMoveRun), cfg);
    SmootherConfig off = cfg;
    off.stillExtrapolateMaxM = 0.0f;
    const R stillOff = run(Ped(proto::kMoveIdle), off);
    std::printf("  [standing-still settle] idle last snapshot: max overshoot %.3f m (cap off %.3f m), running last snapshot %.3f m\n", still.maxD, stillOff.maxD, running.maxD);
    Check("settle: a last snapshot that stands (idle, running velocity) overshoots by <= 0.3 m, a running one keeps its 250 ms extrapolation (~2.5 m), and with the cap off the idle one runs on 2.5 m",
          still.frames > 200 && still.extrap > 5 && still.maxD > 0.25 && still.maxD <= 0.31 && running.maxD > 2.4 && stillOff.maxD > 2.4,
          F("idle %.3f m (%zu extrapolating frames), cap off %.3f m, running %.3f m", still.maxD, still.extrap, stillOff.maxD, running.maxD));
}

// A ped (RemoteSync) and a car (VehicleInterpolator) on one jittered stream: both are delivered every `every`-th
// tick (1 = near tier, 2 = mid tier) of a 30 Hz server, 60 ms plus +-jitter after their tick, stamped at the 60 fps
// frame. `shared` true: the production path (one NetClock; the car at RenderT - its class extra, stamped on the
// clock's timeline minus its age). false: the pre-W7 pair, reproduced from engine.cpp as it was: the ped's own
// per-player clock against the car's decaying-maximum offset (0.5 ms/s) minus a fixed 120 ms. Returns the largest
// and mean |ped render time - car render time| (server timeline, ms) over the frames from 5 s to 20 s.
struct ClockCmp { size_t n = 0, extrap = 0; double maxDiff = 0, meanAbs = 0, pedDelay = 0, sum = 0; };

static ClockCmp RunPedVsCar(bool shared, int every, double jitterMs, uint64_t seed) {
    const double tickMs = kTick30, frameMs = 1000.0 / 60, endMs = 20000, latency = 60, period = 1000.0 / 45;
    SmootherConfig sc = SmootherConfig::ForTickHz(30);
    sc.sharedClock = shared;
    RemoteSync rs(sc);
    VehicleInterpolator interp;
    ClassDelay carExtra;
    Walk w;
    struct Pkt { double at; uint32_t tick; double sampleMs; uint8_t age; };
    Rng rng(seed);
    std::vector<Pkt> pkts;
    for (int i = 0; i * tickMs <= endMs; ++i) {
        if (i % every != 0) continue;
        const double tt = i * tickMs;
        const double s = std::floor((tt - 3) / period) * period; // the newest sample of a 45 Hz sender the server holds (3 ms uplink)
        const double j = (rng.Uniform() * 2 - 1) * jitterMs;
        pkts.push_back({tt + latency + j, uint32_t(1000 + i), s, uint8_t(std::min(255.0, std::floor(tt - s)))});
    }
    std::stable_sort(pkts.begin(), pkts.end(), [](const Pkt& a, const Pkt& b) { return a.at < b.at; });

    bool oldHave = false;
    double oldOffset = 0, oldAt = 0; // the pre-W7 car clock
    ClockCmp r;
    size_t next = 0;
    for (long f = 0;; ++f) {
        const double now = double(f) * frameMs;
        if (now > endMs) break;
        while (next < pkts.size() && pkts[next].at <= now) {
            const Pkt& p = pkts[next++];
            rs.OnPlayerState(1, p.tick, w.At(p.sampleMs), now, p.age);
            VehicleSnapshot vs;
            vs.pos = {float(20.0 * p.sampleMs / 1000.0), 0, 0};
            vs.vel = {20, 0, 0};
            if (shared) {
                rs.OnFrame(p.tick, now);
                vs.tMs = SnapshotStampMs(rs.Clock().Unwrap(p.tick), tickMs, p.age);
                const VehPush pr = interp.Push(vs);
                if (pr == VehPush::Accepted || pr == VehPush::Teleported) rs.Clock().NoteAge(p.age);
            } else {
                vs.tMs = double(p.tick) * tickMs; // the pre-W7 stamp: the frame's tick time, no age
                interp.Push(vs);
                const double sample = double(p.tick) * tickMs - now;
                if (!oldHave) oldOffset = sample;
                else {
                    const double decayed = oldOffset - (now - oldAt) * 0.0005;
                    oldOffset = sample > decayed ? sample : decayed;
                }
                oldHave = true;
                oldAt = now;
            }
        }
        Pose pose;
        if (!rs.Sample(1, now, pose)) continue;
        double carAbs = 0;
        VehicleSample vsm;
        bool carOk = false;
        if (shared) {
            NetClock& clk = rs.Clock();
            const double extra = carExtra.Update(ClassExtraMs(interp.TypicalGapMs(), tickMs), now, clk.Config().maxSlew);
            const double t = clk.RenderT(now) - extra;
            carOk = interp.Sample(t, vsm);
            carAbs = clk.TickAt(t) * tickMs;
        } else {
            carAbs = now + oldOffset - 120.0;
            carOk = interp.Sample(carAbs, vsm);
        }
        if (now < 5000 || !carOk) continue;
        const double diff = std::fabs(pose.renderTick * tickMs - carAbs);
        r.maxDiff = std::max(r.maxDiff, diff);
        r.sum += diff;
        ++r.n;
        if (pose.mode == PoseMode::Extrapolating || pose.mode == PoseMode::Holding) ++r.extrap;
    }
    r.meanAbs = r.n ? r.sum / double(r.n) : 0;
    if (const PlayerSmoother* ps = rs.Find(1)) r.pedDelay = ps->DelayMs();
    return r;
}

static void TestOneClock() {
    const ClockCmp near1 = RunPedVsCar(true, 1, 30.0, 7), mid = RunPedVsCar(true, 2, 30.0, 7);
    const ClockCmp oldNear = RunPedVsCar(false, 1, 30.0, 7), oldMid = RunPedVsCar(false, 2, 30.0, 7);
    std::printf("  [one clock, +-30 ms jitter] shared near: max %.2f mean %.2f ms (ped delay %.0f ms, extrap %zu/%zu)  mid: max %.2f mean %.2f ms "
                "(delay %.0f)  |  pre-W7 split near: max %.2f mean %.2f ms (delay %.0f)  mid: max %.2f mean %.2f ms\n",
                near1.maxDiff, near1.meanAbs, near1.pedDelay, near1.extrap, near1.n, mid.maxDiff, mid.meanAbs, mid.pedDelay, oldNear.maxDiff,
                oldNear.meanAbs, oldNear.pedDelay, oldMid.maxDiff, oldMid.meanAbs);
    Check("one render clock: a +-30 ms jittered stream into the ped and the car (near tier, 30 Hz): |ped renderT - car renderT| <= 5 ms from 5 s to 20 s",
          near1.n > 850 && near1.maxDiff <= 5.0, F("n=%zu max %.2f mean %.2f ms", near1.n, near1.maxDiff, near1.meanAbs));
    Check("one render clock: the same for the mid tier (every 2nd tick, both with one tick of class extra): <= 5 ms",
          mid.n > 850 && mid.maxDiff <= 5.0 && mid.pedDelay > near1.pedDelay + 20.0,
          F("n=%zu max %.2f mean %.2f ms, ped delay %.0f vs near %.0f", mid.n, mid.maxDiff, mid.meanAbs, mid.pedDelay, near1.pedDelay));
    Check("one render clock: the pre-W7 pair (per-player ped clock, car on a decaying-maximum offset - 120 ms) fails the same harness: "
          "> 5 ms apart near and mid",
          oldNear.n > 850 && oldNear.maxDiff > 5.0 && oldMid.n > 850 && oldMid.maxDiff > 5.0,
          F("near max %.2f mean %.2f ms, mid max %.2f mean %.2f ms", oldNear.maxDiff, oldNear.meanAbs, oldMid.maxDiff, oldMid.meanAbs));
}

// ---- 13: the sender pipeline (onfoot spec item 8) -----------------------------------------------------------

struct PipeResult {
    size_t entries = 0, frames = 0, lost = 0, extrap = 0;
    double placeP95 = 0, placeMax = 0, ripple = 0, posP95 = 0, posMax = 0, frozenMs = 0, offsetK = 0, delayEnd = 0;
};

static double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[size_t(p * double(v.size() - 1))];
}

// The whole path of a sample, simulated end to end against the 6 m/s circle (ground truth = the sample's creation time):
//  * sender: a 100 fps game loop (frame times jittered +-1.5 ms) that sends when its 22.2 ms beat (1.5x the 30 Hz tick = 45 Hz,
//    scheduled as engine.cpp does) is due, so every send lands on a frame; the sample time is the frame time, truncated to a
//    whole ms in a u16; the join worker's Poll(2) adds U[0,2] ms, the uplink 20 ms + U[0,upJitter];
//  * server (30 Hz): SampleClock places each sample (or, `receiptPlacement`, v4 style at receipt); at every tick the newest
//    sample goes out quantised (1 cm, 1/32 m/s) with ageMs = floor(tick time - placed time), tick time nominal;
//  * downlink: 20 ms + U[0,downJitter], lossPct of the frames never arrive; the client (100 fps, own phase) stamps what
//    arrived at its frame and samples RemoteSync (the shared clock).
// Measured from 6 s after the first frame: p95 |placement error| (tick*tickMs - ageMs against the sample time, minus its median),
// speed ripple (stddev / mean of the per-frame step), p95 and max position error (shown pose vs the truth at the sample time the
// pose claims), the longest frozen run (steps < 1 cm at 6 m/s).
static PipeResult RunSenderPipeline(double lossPct, uint64_t seed, double upJitterMs, double downJitterMs, bool receiptPlacement) {
    const double tickMs = kTick30, durMs = 40000, frameMs = 10.0, interval = tickMs / 1.5;
    const double serverShift = 54321.7; // the server clock = the sender's + this + the path
    Rng rng(seed);
    Walk w;
    struct Smp { double t; uint16_t stamp; double recvMs; proto::Kinematics k; };
    std::vector<Smp> smps;
    {
        double nextSend = 0;
        for (long f = 0; double(f) * frameMs <= durMs; ++f) {
            const double t = 3.7 + double(f) * frameMs + (rng.Uniform() - 0.5) * 3.0;
            if (t < nextSend) continue;
            const double due = nextSend + interval;
            nextSend = due > t ? due : t + interval;
            Smp s;
            s.t = t;
            s.stamp = uint16_t(uint64_t(t) & 0xFFFFu);
            s.recvMs = serverShift + t + rng.Uniform() * 2.0 + 20.0 + rng.Uniform() * upJitterMs;
            s.k = w.At(t);
            smps.push_back(s);
        }
    }
    std::stable_sort(smps.begin(), smps.end(), [](const Smp& a, const Smp& b) { return a.recvMs < b.recvMs; });

    struct Out { double at; uint32_t tick; proto::Kinematics k; uint8_t age; double sampleT; };
    std::vector<Out> outs;
    PipeResult r;
    {
        SampleClock sc;
        size_t next = 0;
        bool have = false;
        Smp newest{};
        uint64_t placedUs = 0;
        for (uint32_t i = uint32_t(std::ceil(serverShift / tickMs)) + 2;; ++i) {
            const double T = double(i) * tickMs;
            if (T > serverShift + durMs) break;
            while (next < smps.size() && smps[next].recvMs <= T) {
                const uint64_t recvUs = uint64_t(smps[next].recvMs * 1000.0);
                placedUs = receiptPlacement ? recvUs : sc.Place(smps[next].stamp, recvUs);
                newest = smps[next++];
                have = true;
            }
            if (!have) continue;
            const uint64_t nowUs = uint64_t(T * 1000.0);
            const uint64_t ageUs = nowUs > placedUs ? nowUs - placedUs : 0;
            if (lossPct > 0 && rng.Uniform() * 100.0 < lossPct) { ++r.lost; continue; }
            outs.push_back({T + 20.0 + rng.Uniform() * downJitterMs, i, proto::Dequantize(proto::Quantize(newest.k)),
                            uint8_t(std::min<uint64_t>(255, ageUs / 1000)), newest.t});
        }
    }
    std::stable_sort(outs.begin(), outs.end(), [](const Out& a, const Out& b) { return a.at < b.at; });
    r.entries = outs.size();
    if (outs.empty()) return r;

    // The constant the pipeline adds (server clock - sender clock + the fastest path): the median placement.
    std::vector<double> place;
    for (const Out& o : outs) place.push_back((double(o.tick) * tickMs - double(o.age)) - o.sampleT);
    r.offsetK = Percentile(place, 0.5);
    std::vector<double> placeErr;
    for (double p : place) placeErr.push_back(std::fabs(p - r.offsetK));
    r.placeP95 = Percentile(placeErr, 0.95);
    r.placeMax = Percentile(placeErr, 1.0);

    RemoteSync rs(SmootherConfig::ForTickHz(30));
    const double t0 = outs.front().at, tEnd = outs.back().at, warm = t0 + 6000;
    size_t next = 0;
    Pose prev;
    bool havePrev = false;
    double frozenRun = 0;
    std::vector<double> posErr, steps;
    for (long f = 0;; ++f) {
        const double now = t0 + 2.3 + double(f) * frameMs;
        if (now > tEnd) break;
        while (next < outs.size() && outs[next].at <= now) {
            rs.OnPlayerState(1, outs[next].tick, outs[next].k, now, outs[next].age);
            ++next;
        }
        Pose p;
        if (!rs.Sample(1, now, p)) { havePrev = false; continue; }
        if (now >= warm) {
            ++r.frames;
            if (p.mode != PoseMode::Interpolating) ++r.extrap;
            posErr.push_back(Dist(p.k, w.At(p.renderTick * tickMs - r.offsetK)));
            if (havePrev) {
                const double step = Dist(p.k, prev.k);
                steps.push_back(step);
                frozenRun = step < 0.01 ? frozenRun + frameMs : 0.0;
                r.frozenMs = std::max(r.frozenMs, frozenRun);
            }
        }
        prev = p;
        havePrev = true;
    }
    if (!steps.empty()) {
        double mean = 0, var = 0;
        for (double s : steps) mean += s;
        mean /= double(steps.size());
        for (double s : steps) var += (s - mean) * (s - mean);
        r.ripple = mean > 0 ? std::sqrt(var / double(steps.size())) / mean : 0;
    }
    r.posP95 = Percentile(posErr, 0.95);
    r.posMax = Percentile(posErr, 1.0);
    if (const PlayerSmoother* ps = rs.Find(1)) r.delayEnd = ps->DelayMs();
    return r;
}

static void TestSenderPipeline() {
    // The primary run (seed 11, 8 ms uplink and 10 ms downlink jitter, 2% loss) and a sweep of 12 seeds around it: the
    // spec bounds are asserted on the primary run AND on every seed where the measurement meets them; the speed ripple
    // sits at the 3% bound by construction (1 cm / 1 ms resolution of the wire, see below), so it also has a guard.
    const PipeResult a = RunSenderPipeline(2.0, 11, 8.0, 10.0, false);
    const PipeResult receipt = RunSenderPipeline(2.0, 11, 8.0, 10.0, true);
    double worstPlace = 0, worstRipple = 0, minRipple = 1e9, worstPos = 0, worstFrozen = 0, sumRipple = 0;
    int ripplesOver3 = 0;
    bool framesOk = true;
    for (uint64_t sd = 1; sd <= 12; ++sd) {
        const PipeResult q = RunSenderPipeline(2.0, sd, 8.0, 10.0, false);
        framesOk = framesOk && q.frames > 3000;
        worstPlace = std::max(worstPlace, q.placeP95);
        worstRipple = std::max(worstRipple, q.ripple);
        minRipple = std::min(minRipple, q.ripple);
        sumRipple += q.ripple;
        ripplesOver3 += q.ripple > 0.03 ? 1 : 0;
        worstPos = std::max(worstPos, q.posP95);
        worstFrozen = std::max(worstFrozen, q.frozenMs);
    }
    const PipeResult l10 = RunSenderPipeline(10.0, 11, 8.0, 15.0, false), l30 = RunSenderPipeline(30.0, 11, 8.0, 15.0, false);
    std::printf("  [sender pipeline, 100 fps sender 45 Hz -> 30 Hz server, 2%% loss] placement p95 %.2f ms (max %.2f, the first 8 samples are placed at receipt)  "
                "speed ripple %.2f%%  position p95 %.4f m (max %.4f)  max frozen %.0f ms  frames %zu (not interpolating %zu)  delay %.0f ms  "
                "entries %zu lost %zu\n", a.placeP95, a.placeMax, a.ripple * 100.0, a.posP95, a.posMax, a.frozenMs, a.frames, a.extrap,
                a.delayEnd, a.entries, a.lost);
    std::printf("  [sender pipeline, 12 seeds] placement p95 worst %.2f ms, ripple %.2f%%..%.2f%% (mean %.2f%%, %d of 12 above 3%%), position p95 worst "
                "%.4f m, frozen worst %.0f ms\n", worstPlace, minRipple * 100.0, worstRipple * 100.0, sumRipple / 12.0 * 100.0, ripplesOver3, worstPos,
                worstFrozen);
    std::printf("  [sender pipeline, v4 placement at receipt] placement p95 %.2f ms, ripple %.2f%%, position p95 %.4f m  |  stress: 10%% loss +-15 ms "
                "frozen %.0f ms ripple %.2f%% position p95 %.4f m; 30%% loss frozen %.0f ms ripple %.2f%% position p95 %.4f m\n",
                receipt.placeP95, receipt.ripple * 100.0, receipt.posP95, l10.frozenMs, l10.ripple * 100.0, l10.posP95, l30.frozenMs,
                l30.ripple * 100.0, l30.posP95);
    Check("sender pipeline: p95 placement error <= 2 ms on every one of 12 seeds (v4 placement at receipt: more than twice that)",
          framesOk && a.placeP95 <= 2.0 && worstPlace <= 2.0 && receipt.placeP95 > 2.0 * a.placeP95,
          F("primary %.2f ms, worst %.2f ms, at receipt %.2f ms", a.placeP95, worstPlace, receipt.placeP95));
    // The ripple was a measured floor of 2.9..3.1% (the 1 cm position grid of the wire: +-0.3 m/s of chord noise per 33 ms tick); the velocity-aided
    // denoise (smoother.h item 13, sync pipeline review 2026-11-03) takes it to 0.66..0.71% over the 12 seeds. The bound is now 1.2% (guard 1.5%), so the
    // old reconstruction (3.0%) fails it; the v4 placement at receipt (3.9%) is still several times worse.
    Check("sender pipeline: speed ripple <= 1.2% on the primary run (0.66..0.71% over 12 seeds with the denoise, 2.9..3.1% before it; 3.9% with v4 placement at receipt)",
          framesOk && a.ripple <= 0.012 && worstRipple <= 0.015 && receipt.ripple > 3.0 * a.ripple,
          F("primary %.2f%%, seeds %.2f..%.2f%%, at receipt %.2f%%", a.ripple * 100.0, minRipple * 100.0, worstRipple * 100.0, receipt.ripple * 100.0));
    Check("sender pipeline: p95 position error <= 0.10 m on the primary run and every seed",
          framesOk && a.posP95 <= 0.10 && worstPos <= 0.10, F("primary %.4f m, worst %.4f m", a.posP95, worstPos));
    Check("sender pipeline: max frozen interval <= 100 ms on the primary run and every seed",
          framesOk && a.frozenMs <= 100.0 && worstFrozen <= 100.0, F("primary %.0f ms, worst %.0f ms", a.frozenMs, worstFrozen));
    Check("sender pipeline loss stress: 10% loss with +-15 ms downlink jitter freezes a ped for <= 150 ms, 30% loss for <= 300 ms",
          l10.frames > 3000 && l30.frames > 3000 && l10.frozenMs <= 150.0 && l30.frozenMs <= 300.0,
          F("10%%: %.0f ms, 30%%: %.0f ms", l10.frozenMs, l30.frozenMs));
}

// ---- 14a: the velocity-aided position denoise (smoother.h item 13) ---------------------------------------------------

// A server-side stream through the wire's quantiser into a RemoteSync (the engine's configuration; `gain` overrides
// SmootherConfig::denoiseGain, 0 = the old behaviour): a path `truth(tMs)` is sampled at every `every`-th tick of a 30 Hz
// server, quantised (1 cm, 1/32 m/s), placed at its exact time (age 16 ms at the tick) and read at 60 fps. Measured from
// `fromMs`: the pose speed error against the path's speed, the speed ripple (sd of the per-frame step over its mean: only
// meaningful on a constant-speed path) and the position error against the path at the time the pose claims to show.
struct DenoiseRun {
    double velP95 = 0, ripple = 0, posMax = 0, posP95 = 0;
    size_t frames = 0;
    SmootherStats stats;
};

template <class Truth>
static DenoiseRun RunDenoise(Truth truth, double gain, int every, double durMs, double fromMs = 6000) {
    const double tickMs = kTick30, frame = 1000.0 / 60.0;
    SmootherConfig sc = SmootherConfig::ForTickHz(30);
    sc.denoiseGain = float(gain);
    RemoteSync rs(sc);
    std::vector<double> ve, steps, pe;
    Pose prev;
    bool have = false;
    uint32_t tick = 1000;
    double nextTick = 0;
    DenoiseRun r;
    for (long f = 0; double(f) * frame < durMs; ++f) {
        const double now = double(f) * frame;
        while (nextTick <= now) {
            if (tick % uint32_t(every) == 0) {
                const double age = 16.0, ts = nextTick - age; // the sample time on the server's timeline
                const proto::Kinematics k = proto::Dequantize(proto::Quantize(truth(ts)));
                rs.OnPlayerState(1, tick, k, nextTick + 25.0, age);
            }
            ++tick;
            nextTick += tickMs;
        }
        Pose p;
        if (!rs.Sample(1, now, p)) continue;
        if (now >= fromMs) {
            const proto::Kinematics t = truth((p.renderTick - 1000.0) * tickMs);
            pe.push_back(Dist(p.k, t));
            ve.push_back(std::fabs(std::sqrt(double(p.k.vx) * p.k.vx + double(p.k.vy) * p.k.vy) - std::sqrt(double(t.vx) * t.vx + double(t.vy) * t.vy)));
            if (have) steps.push_back(Dist(p.k, prev.k));
            ++r.frames;
        }
        prev = p;
        have = true;
    }
    if (!steps.empty()) {
        double mean = 0, var = 0;
        for (double s : steps) mean += s;
        mean /= double(steps.size());
        for (double s : steps) var += (s - mean) * (s - mean);
        r.ripple = mean > 0 ? std::sqrt(var / double(steps.size())) / mean : 0;
    }
    r.velP95 = Percentile(ve, 0.95);
    r.posP95 = Percentile(pe, 0.95);
    r.posMax = Percentile(pe, 1.0);
    if (const PlayerSmoother* ps = rs.Find(1)) r.stats = ps->Stats();
    return r;
}

static void TestDenoise() {
    const double kDur = 30000;
    const auto walk = [](double speed) { Walk w; w.speed = speed; return [w](double t) { return w.At(t); }; };
    const DenoiseRun w18on = RunDenoise(walk(1.8), 0.3, 1, kDur), w18off = RunDenoise(walk(1.8), 0.0, 1, kDur);
    const DenoiseRun w60on = RunDenoise(walk(6.0), 0.3, 1, kDur), w60off = RunDenoise(walk(6.0), 0.0, 1, kDur);
    const DenoiseRun mid18on = RunDenoise(walk(1.8), 0.3, 2, kDur), mid18off = RunDenoise(walk(1.8), 0.0, 2, kDur);
    std::printf("  [denoise, wire-quantised stream, 30 Hz near tier] walking 1.8 m/s: speed ripple %.2f%% -> %.2f%%, pose speed error p95 %.3f -> %.3f m/s, "
                "position p95 %.4f -> %.4f m  |  6 m/s: ripple %.2f%% -> %.2f%%, speed error p95 %.3f -> %.3f  |  mid tier (15 Hz) 1.8 m/s: ripple %.2f%% -> %.2f%%, "
                "speed error p95 %.3f -> %.3f (denoised %u of %u stored)\n",
                w18off.ripple * 100, w18on.ripple * 100, w18off.velP95, w18on.velP95, w18off.posP95, w18on.posP95, w60off.ripple * 100, w60on.ripple * 100,
                w60off.velP95, w60on.velP95, mid18off.ripple * 100, mid18on.ripple * 100, mid18off.velP95, mid18on.velP95, mid18on.stats.denoised, mid18on.stats.accepted);
    Check("denoise: a ped walking at 1.8 m/s into the 30 Hz near tier moves at a steady pose speed (ripple <= 2.5%, p95 speed error <= 0.08 m/s); without it the 1 cm "
          "position grid showed as a > 5% ripple and 0.2 m/s errors",
          w18on.frames > 1000 && w18on.ripple <= 0.025 && w18on.velP95 <= 0.08 && w18off.ripple >= 0.05 && w18off.velP95 >= 0.20,
          F("on %.2f%% / %.3f, off %.2f%% / %.3f", w18on.ripple * 100, w18on.velP95, w18off.ripple * 100, w18off.velP95));
    Check("denoise: the same at 6 m/s (circle) and in the mid tier (every 2nd tick): the pose speed error p95 drops to under half of the raw wire's",
          w60on.velP95 <= 0.5 * w60off.velP95 && w60on.ripple <= 0.01 && mid18on.velP95 <= 0.5 * mid18off.velP95 && mid18on.ripple <= 0.5 * mid18off.ripple &&
              w60on.frames > 1000 && mid18on.frames > 1000,
          F("6 m/s %.3f vs %.3f m/s, ripple %.2f%%; mid %.3f vs %.3f", w60on.velP95, w60off.velP95, w60on.ripple * 100, mid18on.velP95, mid18off.velP95));

    // Real motion is not smeared. Paths (m, seconds): a 4.5 m/s run that stops dead, a 90 degree turn at 4.5 m/s, a start from
    // standing to 4.5 m/s over 0.3 s, repeated every 4 s. The pose error against the path must not exceed the raw wire's by
    // more than 1.5 cm, and stay within 5 cm of the path (the interpolation's own error at the corners is in both).
    const auto cycle = [](double t) {
        const double cyc = std::floor(t / 8000.0), c = std::fmod(t / 1000.0, 8.0); // 0..2 run +x, 2..4 stand, 4..6 accelerate +y over 0.3 s then run, 6..8 stand
        proto::Kinematics k;
        double x = 0, y = 0, vx = 0, vy = 0;
        if (c < 2) { x = 4.5 * c; vx = 4.5; }
        else if (c < 4) { x = 9.0; }
        else if (c < 6) {
            const double a = c - 4, ramp = std::min(a, 0.3);
            x = 9.0;
            y = a < 0.3 ? 4.5 * a * a / (2 * 0.3) : 4.5 * (a - 0.15);
            vy = 4.5 * ramp / 0.3;
        }
        else { x = 9.0; y = 4.5 * (2 - 0.15); }
        k.x = float(x + cyc * 9.0); k.y = float(y + cyc * 4.5 * 1.85); k.z = 30; k.vx = float(vx); k.vy = float(vy); k.vz = 0; // each cycle ends where the next one starts
        k.heading = float(vx != 0 ? -90.0 : 0.0);
        return k;
    };
    const DenoiseRun cyOn = RunDenoise(cycle, 0.3, 1, 40000), cyOff = RunDenoise(cycle, 0.0, 1, 40000);
    std::printf("  [denoise, stop / turn / start every 8 s] position error max %.4f m (raw wire %.4f), p95 %.4f (raw %.4f), speed error p95 %.3f (raw %.3f)\n",
                cyOn.posMax, cyOff.posMax, cyOn.posP95, cyOff.posP95, cyOn.velP95, cyOff.velP95);
    Check("denoise: real motion is not smeared: a run that stops dead, a start from standing and a 90 degree turn (every 8 s) show a pose error within 1.5 cm of the raw wire's and under 5 cm",
          cyOn.frames > 1500 && cyOn.posMax <= cyOff.posMax + 0.015 && cyOn.posMax <= 0.05 && cyOn.velP95 <= cyOff.velP95 + 0.02,
          F("max %.4f vs raw %.4f, speed p95 %.3f vs %.3f", cyOn.posMax, cyOff.posMax, cyOn.velP95, cyOff.velP95));

    // A sender whose velocity says 0 while it moves at 0.4 m/s (a velocity native that fails or lags): the prediction is 1.3 cm per
    // sample off, which is inside the noise band, so the filter is on but its pull is bounded by the band: the position stays within
    // 3 cm of the path (the raw wire: under 1 cm).
    const auto blind = [](double t) {
        proto::Kinematics k;
        k.x = float(0.4 * t / 1000.0); k.y = 0; k.z = 30; k.vx = 0; k.vy = 0; k.vz = 0; k.heading = -90;
        return k;
    };
    const DenoiseRun blOn = RunDenoise(blind, 0.3, 1, 20000), blOff = RunDenoise(blind, 0.0, 1, 20000);
    Check("denoise: a sender that reports velocity 0 while walking at 0.4 m/s is not pulled back more than 3 cm (the noise band bounds the filter), a faster one (1.8 m/s, 6 cm per sample) not at all",
          blOn.frames > 800 && blOn.posMax <= 0.03 && blOff.posMax <= 0.015 &&
              RunDenoise([](double t) { proto::Kinematics k; k.x = float(1.8 * t / 1000.0); k.z = 30; return k; }, 0.3, 1, 20000).posMax <= 0.02,
          F("0.4 m/s: on %.4f m, off %.4f m", blOn.posMax, blOff.posMax));

    // The repeat test compares the RAW samples. A ped that runs and then stands: the standing samples are bit-identical, so after the
    // first of them every further one is a Repeat (not stored), whatever the filter did to the stored copy of the last moving one.
    {
        SmootherConfig sc = SmootherConfig::ForTickHz(30);
        PlayerSmoother sm(sc);
        double now = 0;
        uint32_t tick = 1000;
        for (int i = 0; i < 60; ++i, ++tick, now += kTick30) {
            const proto::Kinematics k = proto::Dequantize(proto::Quantize(K(1.0 + 0.15 * i, 0, 30, -90, 4.5)));
            sm.Push(tick, k, now + 25, 16.0);
        }
        const proto::Kinematics stand = proto::Dequantize(proto::Quantize(K(1.0 + 0.15 * 59 + 0.05, 0, 30, -90, 0)));
        const uint32_t acc0 = sm.Stats().accepted, rep0 = sm.Stats().repeats;
        for (int i = 0; i < 90; ++i, ++tick, now += kTick30) sm.Push(tick, stand, now + 25, 16.0 + i * 33.3 > 254 ? 254.0 : 16.0 + i * 33.3);
        const uint32_t acc = sm.Stats().accepted - acc0, rep = sm.Stats().repeats - rep0;
        Check("denoise: the repeat detection compares the raw samples: 90 identical standing samples after a run store 1 and repeat 89 (comparing with the denoised stored copy kept re-storing them)",
              acc == 1 && rep == 89 && sm.Stats().denoised > 0, F("accepted %u repeats %u (denoised %u)", acc, rep, sm.Stats().denoised));
    }

    {
        SmootherConfig base;
        const SmootherConfig tuned = SmootherConfig::ForTickHz(30);
        SmootherConfig wild;
        wild.denoiseGain = 7.0f;
        wild.denoiseBandM = -1.0f;
        wild.denoiseMaxGapMs = std::nanf("");
        PlayerSmoother a(wild), b(base);
        Check("denoise config: off in the struct default (every older standalone smoother), 0.3 in ForTickHz (the engine); a wild gain / band / gap is sanitised and never crashes",
              base.denoiseGain == 0.0f && tuned.denoiseGain > 0.29f && tuned.denoiseGain < 0.31f &&
                  a.Push(1, K(0, 0, 0), 0, 0) == PushResult::Accepted && a.Push(2, K(0.01, 0, 0), 33, 0) == PushResult::Accepted &&
                  b.Push(1, K(0, 0, 0), 0, 0) == PushResult::Accepted && b.Stats().denoised == 0,
              F("base %.2f tuned %.2f", base.denoiseGain, tuned.denoiseGain));
    }
}

// ---- 14b: the pipeline diagnostics counters -----------------------------------------------------------------------------

static void TestPipelineDiagnostics() {
    // ArrivalWindow: mean / p95 / max of arrival gaps, restarting on Take.
    {
        ArrivalWindow w;
        for (double g : {33.0, 34.0, 33.0, 33.0, 66.0, 33.0, 34.0, 33.0, 33.0, 300.0}) w.Add(g);
        w.Add(-1.0); // refused
        w.Add(std::nan(""));
        const double p50 = w.PercentileMs(0.5), p95 = w.PercentileMs(0.95);
        ArrivalWindow cleared = w;
        cleared.Reset();
        Check("pipeline counters: ArrivalWindow keeps count, mean, max and a 4 ms histogram (p50 / p95 to the bin), refuses negative and NaN gaps, and Reset empties it",
              w.n == 10 && std::fabs(w.MeanMs() - 63.2) < 0.01 && w.maxMs == 300.0 && p50 >= 33.0 && p50 <= 40.0 && p95 >= 66.0 && p95 <= 300.0 && cleared.n == 0 &&
                  cleared.PercentileMs(0.95) == 0.0,
              F("n %u mean %.1f max %.0f p50 %.0f p95 %.0f", w.n, w.MeanMs(), w.maxMs, p50, p95));
    }
    // PlayerSmoother: the arrival window of STORED snapshots, the repeat split, the longest extrapolation, taken per window.
    {
        SmootherConfig sc = SmootherConfig::ForTickHz(30);
        RemoteSync rs(sc);
        double now = 0;
        uint32_t tick = 100;
        for (int i = 0; i < 40; ++i, ++tick, now += 30 + (i % 3) * 3) { // arrivals 30, 33, 36 ms apart
            rs.OnFrame(tick, now);
            rs.OnPlayerState(2, tick, K(0.15 * i, 0, 30, 0, 4.5), now, 10.0);
        }
        // identical state again (idle refresh): Repeat, identical
        rs.OnFrame(tick, now);
        const PushResult same = rs.OnPlayerState(2, tick++, K(0.15 * 39, 0, 30, 0, 4.5), now, 10.0);
        // a different state whose placed time is earlier than the newest stored: Repeat, earlier (the sender's timeline went back)
        rs.OnFrame(tick, now += 33);
        const PushResult back = rs.OnPlayerState(2, tick++, K(0.15 * 40, 0, 30, 0, 4.5), now, 120.0);
        // a late duplicate tick: Stale
        const PushResult stale = rs.OnPlayerState(2, tick - 3, K(1, 0, 30), now, 10.0);
        ArrivalWindow w;
        double longest = 0;
        const bool have = rs.TakeDiagnostics(2, w, longest);
        const SmootherStats st = rs.Find(2)->Stats();
        ArrivalWindow w2;
        double longest2 = 0;
        rs.TakeDiagnostics(2, w2, longest2);
        Check("pipeline counters: a player's stored snapshots arrive as an arrival window (39 gaps of 30..36 ms), the repeats split into identical / earlier-placed, stale ones are counted, "
              "and taking the window restarts it",
              have && w.n == 39 && w.MeanMs() > 31.0 && w.MeanMs() < 34.0 && w.maxMs <= 36.5 && same == PushResult::Repeat && back == PushResult::Repeat &&
                  stale == PushResult::Stale && st.repeatsIdentical == 1 && st.repeatsEarlier == 1 && st.repeats == 2 && st.stale == 1 && w2.n == 0,
              F("n %u mean %.1f max %.1f; identical %u earlier %u stale %u; second window %u", w.n, w.MeanMs(), w.maxMs, st.repeatsIdentical, st.repeatsEarlier, st.stale, w2.n));
        (void)longest; (void)longest2;
    }
    // Longest extrapolation run: a stream that stops for 200 ms is extrapolated for about that long and the number is reported once.
    {
        RemoteSync rs(SmootherConfig::ForTickHz(30));
        double now = 0;
        uint32_t tick = 100;
        for (int i = 0; i < 90; ++i, ++tick, now += kTick30) {
            rs.OnFrame(tick, now);
            rs.OnPlayerState(2, tick, K(0.15 * i, 0, 30, 0, 4.5), now, 10.0);
            Pose p;
            rs.Sample(2, now, p);
        }
        // the stream stops: sample at 60 fps for 220 ms
        for (double t = now; t < now + 220; t += 1000.0 / 60) {
            Pose p;
            rs.Sample(2, t, p);
        }
        ArrivalWindow w;
        double longest = 0;
        rs.TakeDiagnostics(2, w, longest);
        ArrivalWindow w2;
        double longest2 = -1;
        rs.TakeDiagnostics(2, w2, longest2);
        Check("pipeline counters: the longest run of extrapolating frames is measured (a stream that stops for 220 ms: 60..230 ms) and the window restarts",
              longest > 60.0 && longest <= 230.0 && longest2 == 0.0, F("longest %.0f ms, after the take %.0f", longest, longest2));
    }
    // NetClock: baseline steps, delay rises, reordered frames, the frame arrival window and the baseline drift.
    {
        NetClock clk(NetClockConfig::ForTickHz(30));
        double base = 5000;
        uint32_t tick = 1;
        auto feed = [&](int n, double extra) {
            for (int i = 0; i < n; ++i, ++tick) clk.OnFrame(tick, base + double(tick) * kTick30 + extra);
        };
        feed(300, 0);                  // 10 s calm
        const double d0 = clk.TakeBaselineDriftMs();
        ArrivalWindow calmWin = clk.TakeFrameGaps();
        const uint32_t stepsCalm = clk.Stats().baselineSteps, risesCalm = clk.Stats().delayRises;
        feed(90, 70.0);                // the link gets 70 ms slower: the frames are late, the delay rises at once; the baseline only follows after 5 s
        const uint32_t risesBurst = clk.Stats().delayRises;
        feed(210, 70.0);               // 7 s more: the old baseline leaves the 5 s window, a step of 70 ms
        const double drift = clk.TakeBaselineDriftMs();
        const double stepMax = clk.TakeBaselineStepMaxMs();
        const uint32_t steps = clk.Stats().baselineSteps;
        const uint32_t re0 = clk.Stats().reorderedFrames;
        clk.OnFrame(tick - 5, base + double(tick) * kTick30 + 70.0); // an old frame arrives late
        Check("pipeline counters: NetClock counts delay rises (a link that gets 70 ms slower), baseline steps over 5 ms (one of 70 ms once the old baseline leaves its 5 s window) with the "
              "drift since the last take, the frame arrival window (30 Hz: mean 33.3 ms) and reordered frames",
              stepsCalm == 0 && risesCalm == 0 && risesBurst >= 1 && steps >= 1 && stepMax > 60.0 && stepMax < 80.0 && std::fabs(drift - 70.0) < 5.0 && d0 == 0.0 &&
                  std::fabs(calmWin.MeanMs() - kTick30) < 0.5 && calmWin.n >= 298 && clk.Stats().reorderedFrames == re0 + 1,
              F("steps %u (calm %u) rises %u (calm %u) step max %.1f drift %.1f frames mean %.1f reordered %u", steps, stepsCalm, risesBurst, risesCalm, stepMax, drift,
                calmWin.MeanMs(), clk.Stats().reorderedFrames));
    }
}

// ---- 14c: end to end through the real server replication ---------------------------------------------------------------

// Ground-truth paths on the SENDER's timeline (ms): an interface so each scenario brings its own motion.
struct E2ePath {
    virtual ~E2ePath() {}
    virtual proto::Kinematics At(double tMs) const = 0;
};
struct E2eCircle : E2ePath { // the 6 m/s walk circle of the other tests, its centre `cx` metres from the receiver
    Walk w;
    explicit E2eCircle(double cx, double speed = 6.0) { w.cx = cx; w.speed = speed; }
    proto::Kinematics At(double t) const override { return w.At(t); }
};
struct E2eShuttle : E2ePath { // out and back along +x between x0 and x1
    double x0, x1, speed;
    E2eShuttle(double a, double b, double s) : x0(a), x1(b), speed(s) {}
    proto::Kinematics At(double t) const override {
        const double len = x1 - x0, period = 2 * len / speed * 1000.0;
        double ph = std::fmod(t, period);
        if (ph < 0) ph += period;
        const bool fwd = ph < period / 2;
        proto::Kinematics k;
        k.x = float(x0 + (fwd ? ph : period - ph) / 1000.0 * speed); k.y = 0; k.z = 30;
        k.vx = float(fwd ? speed : -speed); k.vy = 0; k.vz = 0; k.heading = fwd ? -90.0f : 90.0f;
        return k;
    }
};
struct E2eStopGo : E2ePath { // walk 1.8 m/s 3 s, stand 3 s, run 4.5 m/s 4 s, stand 3 s, sprint 7 m/s 4 s, stand 3 s (20 s cycle), 0.4 s ramps, folded into 55 m
    double cx;
    std::vector<double> X;
    double L = 55.0; // the path folds here (one cycle's distance: the direction reverses while standing, never at speed)
    explicit E2eStopGo(double c) : cx(c) {
        X.assign(100001, 0.0);
        for (size_t i = 1; i < X.size(); ++i) X[i] = X[i - 1] + Speed(double(i - 1) * 0.001) * 0.001;
        L = X[20000];
    }
    static double Speed(double ts) {
        const double c = std::fmod(ts, 20.0);
        auto ramp = [](double t, double t0, double t1, double v) {
            if (t < t0 || t >= t1 + 0.4) return 0.0;
            if (t < t0 + 0.4) return v * (t - t0) / 0.4;
            if (t < t1) return v;
            return v * (1 - (t - t1) / 0.4);
        };
        return ramp(c, 0, 3, 1.8) + ramp(c, 6, 10, 4.5) + ramp(c, 13, 17, 7.0);
    }
    proto::Kinematics At(double t) const override {
        if (t < 0) t = 0;
        size_t i = size_t(t);
        if (i + 1 >= X.size()) i = X.size() - 2;
        const double x = X[i] + (X[i + 1] - X[i]) * (t - double(i)), m = std::fmod(x, 2 * L);
        const bool back = m > L;
        proto::Kinematics k;
        k.x = float(cx + (back ? 2 * L - m : m)); k.y = 0; k.z = 30;
        const double sp = Speed(t / 1000.0);
        k.vx = float(back ? -sp : sp); k.vy = 0; k.vz = 0; k.heading = back ? 90.0f : -90.0f;
        return k;
    }
};

static proto::PedState E2ePedFor(const proto::Kinematics& k) {
    proto::PedState p;
    const double s = std::sqrt(double(k.vx) * k.vx + double(k.vy) * k.vy);
    p.move = s < 0.3 ? proto::kMoveIdle : s < 3.0 ? proto::kMoveWalk : s < 6.0 ? proto::kMoveRun : proto::kMoveSprint;
    return p;
}

struct E2eProfile {
    double tickHz = 30, senderFps = 60;
    double upLatMs = 25, upJitMs = 8, upLoss = 0;
    double downLatMs = 25, downJitMs = 10, downLoss = 0.02;
    double tickLateMs = 0.6;               // the server loop starts a tick U[0, this] ms late
    double clientDriftPpm = 0;             // the client's clock runs fast by this
    double durMs = 40000;
    uint64_t seed = 11;
    double hitchAtMs = -1, hitchLenMs = 0; // the sender produces no frames for hitchLenMs from hitchAtMs
    double burstEveryMs = 0, burstMs = 0, burstExtraMs = 0; // downlink delay bursts (a bad Wi-Fi)
    double senderOffsetMs = 1234.5;        // the sender's clock = the global one + this (its u16 stamp wraps every 65.536 s)
    float denoiseGain = -1.0f;             // >= 0: override SmootherConfig::denoiseGain (0 = the pre-review behaviour)
};

struct E2eResult {
    size_t frames = 0, extrap = 0, hold = 0, buffering = 0, stalls = 0, entries = 0;
    double errP50 = 0, errP95 = 0, errMax = 0, velP95 = 0, latMed = 0, latSd = 0, latP95 = 0, delayMed = 0, delayMax = 0;
    double gapMean = 0, gapP95 = 0, gapMax = 0, longestExtrapMs = 0, stepMaxRel = 0, entriesPerS = 0;
    uint32_t snaps = 0, resyncs = 0, bridges = 0, repeats = 0, stale = 0, denoised = 0;
    uint64_t tierSent[4] = {0, 0, 0, 0};
};

// Sender (a game loop of `senderFps` sending at 1.5 x the tick as engine.cpp does) -> uplink (worker wait, latency, jitter,
// loss) -> server (SampleClock placement, the real Replicator: tiers by distance, idle gate, quantiser, delta frames and acks)
// -> downlink (latency, jitter, loss, bursts) -> WorldDecoder -> RemoteSync -> 60 fps render. Scored from 6 s: the pose against the
// path at a constant latency (the median over the run: the pipeline's fixed latency), the sd of that latency over the run (the time
// base's steadiness), the pose speed error, modes, the largest step, stalls (steps under 15% of the path's while it moves) and the
// arrival rhythm of the stored entries.
static E2eResult RunE2e(const E2ePath& path, const E2eProfile& P) {
    Rng rng(P.seed);
    const double tickMs = 1000.0 / P.tickHz;
    const uint64_t tickUs = 1000000ull / uint64_t(P.tickHz);
    const double vOff = 54321.7, cOff = 777.3; // the server's and the client's clocks against the global one
    struct E { double t; uint64_t ord; int kind; std::vector<uint8_t> bytes; proto::Kinematics k; proto::PedState ped; uint16_t stamp; double sT; };
    auto cmp = [](const E& a, const E& b) { return a.t != b.t ? a.t > b.t : a.ord > b.ord; };
    std::priority_queue<E, std::vector<E>, decltype(cmp)> q(cmp);
    uint64_t ord = 0;
    auto push = [&](E e) { e.ord = ord++; q.push(std::move(e)); };
    {
        double nextSend = 0;
        const double fr = 1000.0 / P.senderFps, interval = tickMs / 1.5;
        for (long f = 0; double(f) * fr <= P.durMs; ++f) {
            const double t = 3.7 + double(f) * fr + (rng.Uniform() - 0.5) * 2.0;
            if (P.hitchAtMs >= 0 && t >= P.hitchAtMs && t < P.hitchAtMs + P.hitchLenMs) continue;
            if (t < nextSend) continue;
            const double due = nextSend + interval;
            nextSend = due > t ? due : t + interval;
            E e{};
            e.kind = 0;
            e.sT = t; e.k = path.At(t); e.ped = E2ePedFor(e.k);
            e.stamp = uint16_t(uint64_t(t + P.senderOffsetMs) & 0xFFFFu);
            if (P.upLoss > 0 && rng.Uniform() < P.upLoss) continue;
            e.t = t + rng.Uniform() * 2.0 + P.upLatMs + rng.Uniform() * P.upJitMs;
            push(e);
        }
    }
    for (uint64_t i = uint64_t(std::ceil(vOff * 1000.0 / double(tickUs))) + 2;; ++i) {
        const double T = double(i * tickUs) / 1000.0 - vOff;
        if (T > P.durMs + 500) break;
        if (T < 0) continue;
        E e{};
        e.kind = 1; e.t = T + rng.Uniform() * P.tickLateMs; e.sT = double(i);
        push(e);
    }
    const double rf = 1000.0 / 60.0;
    for (long f = 0; double(f) * rf <= P.durMs; ++f) { E e{}; e.kind = 3; e.t = 2.3 + double(f) * rf; push(e); }

    server::Replicator rep;
    server::ReplConfig cfg;
    cfg.maxPlayers = 8;
    cfg.tickHz = uint16_t(P.tickHz);
    rep.Configure(cfg);
    rep.AddPlayer(0);
    rep.AddPlayer(1); // the receiver, at the origin
    rep.SetState(1, proto::Kinematics(), 0);
    server::Metrics metrics;
    SampleClock sc;
    proto::WorldDecoder dec;
    SmootherConfig smc = SmootherConfig::ForTickHz(P.tickHz);
    if (P.denoiseGain >= 0.0f) smc.denoiseGain = P.denoiseGain;
    RemoteSync rs(smc);
    rs.SetLocalId(1);

    struct AckRec { double t; proto::WorldAck a; };
    std::vector<AckRec> ackHist;
    struct Fr { double T; Pose p; double delay; };
    std::vector<Fr> frames;
    std::vector<double> gaps;
    E2eResult R;
    double lastStoredAt = -1;
    auto clientNow = [&](double T) { return T * (1.0 + P.clientDriftPpm * 1e-6) + cOff; };
    while (!q.empty()) {
        E e = q.top();
        q.pop();
        const double T = e.t;
        if (e.kind == 0) {
            const uint64_t recvUs = uint64_t((T + vOff) * 1000.0);
            rep.SetState(0, e.k, e.ped, sc.Place(e.stamp, recvUs));
            const double want = T - P.upLatMs - 11.0; // the receiver's own 45 Hz PlayerState carries its newest receipt
            proto::WorldAck a;
            for (size_t i = ackHist.size(); i-- > 0;)
                if (ackHist[i].t <= want) { a = ackHist[i].a; break; }
            rep.SetAck(1, a);
        } else if (e.kind == 1) {
            const uint32_t tick = uint32_t(e.sT);
            std::vector<std::vector<uint8_t>> out;
            rep.Tick(tick, uint64_t(tick) * tickUs, [&](uint16_t r, bool coarse, const proto::Bytes& pk) { if (r == 1 && !coarse) out.push_back(pk); }, metrics);
            for (auto& pk : out) {
                if (rng.Uniform() < P.downLoss) continue;
                double extra = 0;
                if (P.burstEveryMs > 0 && std::fmod(T, P.burstEveryMs) < P.burstMs) extra = P.burstExtraMs * rng.Uniform();
                E d{};
                d.kind = 2; d.t = T + P.downLatMs + rng.Uniform() * P.downJitMs + extra; d.bytes = pk;
                push(d);
            }
        } else if (e.kind == 2) {
            const double now = clientNow(T);
            proto::WorldState ws;
            if (!dec.Decode(e.bytes.data(), e.bytes.size(), ws)) continue; // stale or reordered: ENet's sequenced channel drops these too
            ackHist.push_back({T, dec.ack()});
            rs.OnFrame(ws.tick, now);
            for (const proto::WorldEntry& we : ws.players) {
                if (we.id == 1) continue;
                const PushResult r = rs.OnPlayerState(we.id, ws.tick, we.k, now, double(we.ageMs), we.ped);
                R.entries++;
                if (r == PushResult::Accepted || r == PushResult::Teleported) {
                    if (lastStoredAt >= 0) gaps.push_back(now - lastStoredAt);
                    lastStoredAt = now;
                }
            }
        } else {
            const double now = clientNow(T);
            Pose p;
            if (!rs.Sample(0, now, p)) continue;
            const PlayerSmoother* ps = rs.Find(0);
            frames.push_back({T, p, ps ? ps->DelayMs() : 0.0});
        }
    }

    const double warm = 6000;
    std::vector<double> taus, errs, vels, delayV;
    size_t nf = 0;
    for (const Fr& f : frames) {
        if (f.T < warm) continue;
        if ((nf++ & 3) != 0) continue; // every 4th frame is enough for the latency estimate
        const double sp = std::sqrt(double(f.p.k.vx) * f.p.k.vx + double(f.p.k.vy) * f.p.k.vy);
        double best = 1e9, bt = -1;
        for (double tau = 20; tau <= 600; tau += 2.0) {
            const double d = Dist(f.p.k, path.At(f.T - tau));
            if (d < best) { best = d; bt = tau; }
        }
        const proto::Kinematics tn = path.At(f.T - bt);
        if (std::sqrt(double(tn.vx) * tn.vx + double(tn.vy) * tn.vy) > 1.0 && sp > 1.0) taus.push_back(bt);
    }
    R.latMed = Percentile(taus, 0.5);
    R.latP95 = Percentile(taus, 0.95);
    {
        double m = 0, v = 0;
        for (double t : taus) m += t;
        m /= double(std::max<size_t>(1, taus.size()));
        for (double t : taus) v += (t - m) * (t - m);
        R.latSd = std::sqrt(v / double(std::max<size_t>(1, taus.size())));
    }
    Pose pp;
    bool have = false;
    double extrapRun = 0;
    for (const Fr& f : frames) {
        if (f.T < warm) continue;
        ++R.frames;
        if (f.p.mode == PoseMode::Extrapolating) { ++R.extrap; extrapRun += rf; R.longestExtrapMs = std::max(R.longestExtrapMs, extrapRun); } else extrapRun = 0;
        if (f.p.mode == PoseMode::Holding) ++R.hold;
        if (f.p.mode == PoseMode::Buffering) ++R.buffering;
        const proto::Kinematics t = path.At(f.T - R.latMed);
        errs.push_back(Dist(f.p.k, t));
        vels.push_back(std::hypot(double(f.p.k.vx) - t.vx, double(f.p.k.vy) - t.vy));
        delayV.push_back(f.delay);
        const double tsp = std::sqrt(double(t.vx) * t.vx + double(t.vy) * t.vy);
        if (have && tsp > 1.5) {
            const double step = Dist(f.p.k, pp.k), expect = tsp * rf / 1000.0;
            if (step < 0.15 * expect) ++R.stalls;
            R.stepMaxRel = std::max(R.stepMaxRel, step / expect);
        }
        pp = f.p;
        have = true;
    }
    R.errP50 = Percentile(errs, 0.5); R.errP95 = Percentile(errs, 0.95); R.errMax = Percentile(errs, 1.0); R.velP95 = Percentile(vels, 0.95);
    R.delayMed = Percentile(delayV, 0.5); R.delayMax = Percentile(delayV, 1.0);
    double gm = 0;
    for (double g : gaps) gm += g;
    R.gapMean = gm / double(std::max<size_t>(1, gaps.size()));
    R.gapP95 = Percentile(gaps, 0.95); R.gapMax = Percentile(gaps, 1.0);
    if (const PlayerSmoother* ps = rs.Find(0)) {
        const SmootherStats& st = ps->Stats();
        R.snaps = st.teleports; R.resyncs = st.resyncs; R.bridges = st.bridgeResets; R.repeats = st.repeats; R.stale = st.stale; R.denoised = st.denoised;
    }
    R.entriesPerS = double(R.entries) / (P.durMs / 1000.0);
    for (int i = 0; i < 4; ++i) R.tierSent[i] = metrics.pairSentTier[i];
    return R;
}

static void PrintE2e(const char* name, const E2eResult& r) {
    std::printf("  [pipeline %-34s] err p50 %.3f p95 %.3f max %.2f m, speed err p95 %.2f m/s | latency %.0f ms (sd %.1f, p95 %.0f), delay %.0f..%.0f | stored gap mean %.0f p95 %.0f max %.0f ms | "
                "extrap %.1f%% (longest %.0f ms) hold %.1f%% | stalls %zu, step max %.2fx | snaps %u resyncs %u bridges %u repeats %u stale %u, %.1f entries/s\n",
                name, r.errP50, r.errP95, r.errMax, r.velP95, r.latMed, r.latSd, r.latP95, r.delayMed, r.delayMax, r.gapMean, r.gapP95, r.gapMax,
                100.0 * double(r.extrap) / double(std::max<size_t>(1, r.frames)), r.longestExtrapMs, 100.0 * double(r.hold) / double(std::max<size_t>(1, r.frames)), r.stalls,
                r.stepMaxRel, r.snaps, r.resyncs, r.bridges, r.repeats, r.stale, r.entriesPerS);
}

static void TestEndToEnd() {
    const E2eProfile base;
    // Tiers by distance from the receiver at the origin: near < 120 m (30 Hz), mid < 250 m (15 Hz), far (10 Hz).
    const E2eCircle nearC(50), midC(180), farC(350);
    const E2eResult n0 = RunE2e(nearC, base), m0 = RunE2e(midC, base), f0 = RunE2e(farC, base);
    PrintE2e("near 6 m/s circle, 2% loss", n0);
    PrintE2e("mid", m0);
    PrintE2e("far", f0);
    Check("e2e steady (25 ms links, 8 / 10 ms jitter, 2% loss): the near, mid and far tier each track the path within 3 cm p95 at a steady latency (sd <= 2 ms; 127 / 160 / 193 ms behind), "
          "no snap, no stall, and the stored entries arrive at the tier's rate (about 30 / 15 / 10 per second)",
          n0.frames > 1800 && m0.frames > 1800 && f0.frames > 1800 && n0.errP95 <= 0.03 && m0.errP95 <= 0.03 && f0.errP95 <= 0.03 && n0.latSd <= 2.0 && m0.latSd <= 2.0 && f0.latSd <= 2.0 &&
              n0.snaps + m0.snaps + f0.snaps == 0 && n0.stalls + m0.stalls + f0.stalls == 0 && n0.entriesPerS > 27.0 && n0.entriesPerS < 31.0 && m0.entriesPerS > 13.5 && m0.entriesPerS < 15.5 &&
              f0.entriesPerS > 9.0 && f0.entriesPerS < 10.5 && n0.latMed > 110 && n0.latMed < 145 && f0.latMed > n0.latMed + 40,
          F("err p95 %.3f/%.3f/%.3f, latency %.0f/%.0f/%.0f sd %.1f/%.1f/%.1f, entries/s %.1f/%.1f/%.1f", n0.errP95, m0.errP95, f0.errP95, n0.latMed, m0.latMed, f0.latMed, n0.latSd, m0.latSd, f0.latSd,
            n0.entriesPerS, m0.entriesPerS, f0.entriesPerS));
    {
        // The server's per-tier counts (Metrics::pairSentTier): near : mid : far = 30 : 15 : 10 entries per second over the run, idle 0 (the sender never stands).
        const double perS[3] = {double(n0.tierSent[0]) / 40.0, double(m0.tierSent[1]) / 40.0, double(f0.tierSent[2]) / 40.0};
        Check("e2e server counters: Metrics::pairSentTier counts the entries by the rate that applied: 30 / 15 / 10 per second for a near, mid and far subject (within 8%), nothing in the other distance classes, and only the 1 Hz idle refresh of the standing receiver (the sender's own view of it) in the idle class",
              std::fabs(perS[0] - 30.0) < 2.4 && std::fabs(perS[1] - 15.0) < 1.2 && std::fabs(perS[2] - 10.0) < 0.8 && n0.tierSent[1] + n0.tierSent[2] == 0 &&
                  m0.tierSent[0] + m0.tierSent[2] == 0 && f0.tierSent[0] + f0.tierSent[1] == 0 && n0.tierSent[3] <= 45,
              F("near %.1f mid %.1f far %.1f /s; stray near-run %llu mid-run %llu far-run %llu", perS[0], perS[1], perS[2],
                (unsigned long long)(n0.tierSent[1] + n0.tierSent[2]), (unsigned long long)(m0.tierSent[0] + m0.tierSent[2]),
                (unsigned long long)(f0.tierSent[0] + f0.tierSent[1])));
    }

    // The same with and without the denoise: the speed ripple of a walking ped, end to end.
    {
        E2eProfile p = base;
        p.downLoss = 0.0; // a clean link isolates the reconstruction
        const E2eCircle walkC(50, 1.8);
        const E2eResult on = RunE2e(walkC, p);
        p.denoiseGain = 0.0f;
        const E2eResult off = RunE2e(walkC, p);
        PrintE2e("near walking 1.8 m/s, denoise on", on);
        PrintE2e("near walking 1.8 m/s, denoise OFF (old)", off);
        Check("e2e walking pace: through the real server (1 cm wire, 45 Hz sender into the 30 Hz tick) a ped walking at 1.8 m/s is shown with a pose speed error p95 <= 0.10 m/s "
              "(the old reconstruction: >= 0.25) and no position error beyond 1.5 cm p95",
              on.frames > 1800 && on.velP95 <= 0.10 && off.velP95 >= 0.25 && on.errP95 <= 0.015, F("on %.3f m/s, off %.3f m/s, position p95 %.4f", on.velP95, off.velP95, on.errP95));
    }

    // Hostile links: 80 ms downlink jitter with 5% loss, 5% uplink loss, 10% downlink loss.
    {
        E2eProfile j = base;
        j.downJitMs = 80; j.upJitMs = 20; j.downLoss = 0.05;
        const E2eResult nj = RunE2e(nearC, j), mj = RunE2e(midC, j), fj = RunE2e(farC, j);
        PrintE2e("near, 80 ms down jitter, 5% loss", nj);
        PrintE2e("mid", mj);
        PrintE2e("far", fj);
        E2eProfile l = base;
        l.downLoss = 0.10;
        const E2eResult nl = RunE2e(nearC, l), ml = RunE2e(midC, l);
        E2eProfile u = base;
        u.upLoss = 0.05;
        const E2eResult nu = RunE2e(nearC, u), mu = RunE2e(midC, u);
        PrintE2e("near, 10% down loss", nl);
        PrintE2e("mid, 10% down loss", ml);
        PrintE2e("near, 5% up loss", nu);
        PrintE2e("mid, 5% up loss", mu);
        Check("e2e hostile (80 ms downlink jitter + 5% loss): no snap, no stall, p95 position error <= 12 cm in every tier (the delay adapts: median delay 150..250 ms), nothing frozen",
              nj.frames > 1800 && mj.frames > 1800 && fj.frames > 1800 && nj.snaps + mj.snaps + fj.snaps == 0 && nj.stalls + mj.stalls + fj.stalls == 0 && nj.errP95 <= 0.12 && mj.errP95 <= 0.12 &&
                  fj.errP95 <= 0.12 && nj.longestExtrapMs <= 250.0 && nj.latMed > 150 && nj.latMed < 260,
              F("err p95 %.3f/%.3f/%.3f, latency %.0f/%.0f/%.0f, longest extrapolation %.0f ms", nj.errP95, mj.errP95, fj.errP95, nj.latMed, mj.latMed, fj.latMed, nj.longestExtrapMs));
        Check("e2e loss: 10% downlink loss and 5% uplink loss: p95 position error <= 6 cm, no snap, no stall, the longest extrapolation within the 250 ms cap",
              nl.snaps + ml.snaps + nu.snaps + mu.snaps == 0 && nl.stalls + ml.stalls + nu.stalls + mu.stalls == 0 && nl.errP95 <= 0.06 && ml.errP95 <= 0.06 && nu.errP95 <= 0.06 && mu.errP95 <= 0.08 &&
                  nl.longestExtrapMs <= 250.0 && ml.longestExtrapMs <= 250.0,
              F("down10 near %.3f mid %.3f; up5 near %.3f mid %.3f; longest %.0f/%.0f ms", nl.errP95, ml.errP95, nu.errP95, mu.errP95, nl.longestExtrapMs, ml.longestExtrapMs));
    }

    // Stop and start (walk, stand, run, stand, sprint, stand; the idle gate takes the standing sender to 1 Hz and wakes it for the start).
    {
        E2eProfile p = base;
        p.downJitMs = 20;
        const E2eStopGo sn(30), sm(150), sf(300);
        const E2eResult a = RunE2e(sn, p), b = RunE2e(sm, p), c = RunE2e(sf, p);
        PrintE2e("near stop/go", a);
        PrintE2e("mid stop/go", b);
        PrintE2e("far stop/go", c);
        Check("e2e stop / start: standing for 3 s (1 Hz idle refresh) and starting again (walk, run, sprint) at every tier: p95 position error <= 2 cm, the latency does not move (sd <= 2 ms: "
              "the idle gate, the bridge reset and the class extra do not shift the time base), no snap, at most one stalled frame",
              a.frames > 1800 && a.errP95 <= 0.02 && b.errP95 <= 0.02 && c.errP95 <= 0.02 && a.latSd <= 2.0 && b.latSd <= 2.0 && c.latSd <= 2.5 && a.snaps + b.snaps + c.snaps == 0 &&
                  a.stalls <= 1 && b.stalls <= 1 && c.stalls <= 1 && a.bridges >= 3,
              F("err p95 %.3f/%.3f/%.3f, latency sd %.1f/%.1f/%.1f, stalls %zu/%zu/%zu, bridges %u", a.errP95, b.errP95, c.errP95, a.latSd, b.latSd, c.latSd, a.stalls, b.stalls, c.stalls, a.bridges));
    }

    // Tier changes: a ped shuttling across the near/mid boundary (95..150 m) and the mid/far one (220..280 m) at 5 m/s. Today the class extra
    // moves the render time by one tick (33 ms) each time the tier changes (up after 1.5 s, down after 8 s, then slewed at 10%): the latency
    // of the near/mid shuttle swings by 33 ms. That is the design (a mid ped must be drawn one tick later than a near one), so the bound
    // here is what it costs (p95 position error 20 cm at 5 m/s = 33 ms), not zero: it must not snap, stall or freeze.
    {
        const E2eShuttle s1(95, 150, 5), s2(220, 280, 5);
        const E2eResult a = RunE2e(s1, base), b = RunE2e(s2, base);
        PrintE2e("tier crossing near<->mid, 5 m/s", a);
        PrintE2e("tier crossing mid<->far, 5 m/s", b);
        Check("e2e tier change: crossing the near/mid and the mid/far boundary back and forth shifts the latency by one class extra (<= 40 ms, p95 position error <= 22 cm) without a snap, a resync, a stall or a frozen ped",
              a.frames > 1800 && b.frames > 1800 && a.snaps + b.snaps == 0 && a.resyncs + b.resyncs == 0 && a.stalls <= 3 && b.stalls <= 3 && a.errP95 <= 0.22 && b.errP95 <= 0.22 && a.latSd <= 40.0 &&
                  b.latSd <= 40.0 && a.longestExtrapMs <= 250.0,
              F("near/mid: err p95 %.3f latency sd %.1f stalls %zu; mid/far: err p95 %.3f sd %.1f stalls %zu", a.errP95, a.latSd, a.stalls, b.errP95, b.latSd, b.stalls));
    }

    // Time-base robustness: the client's clock 300 ppm fast / slow, the sender's u16 stamp wrapping inside the run, a server loop that starts
    // its ticks up to 8 ms late, a 30 fps sender and a 144 fps sender.
    {
        E2eProfile pf = base, ps = base, pw = base, pl = base, p30 = base, p144 = base;
        pf.clientDriftPpm = 300; ps.clientDriftPpm = -300; pw.senderOffsetMs = 62000; pl.tickLateMs = 8; p30.senderFps = 30; p144.senderFps = 144;
        const E2eResult a = RunE2e(nearC, pf), b = RunE2e(nearC, ps), c = RunE2e(nearC, pw), d = RunE2e(nearC, pl), e = RunE2e(nearC, p30), g = RunE2e(nearC, p144);
        PrintE2e("near, client clock +300 ppm", a);
        PrintE2e("near, client clock -300 ppm", b);
        PrintE2e("near, sender stamp wraps (u16) in the run", c);
        PrintE2e("near, server ticks 0..8 ms late", d);
        PrintE2e("near, sender at 30 fps", e);
        PrintE2e("near, sender at 144 fps", g);
        Check("e2e time base: client clock drift of +-300 ppm, the sender's 16-bit stamp wrapping inside the run, a server loop up to 8 ms late and 30 / 144 fps senders all keep the p95 "
              "position error <= 3 cm at a steady latency (sd <= 2 ms), no snap, no resync",
              a.errP95 <= 0.03 && b.errP95 <= 0.03 && c.errP95 <= 0.03 && d.errP95 <= 0.03 && e.errP95 <= 0.03 && g.errP95 <= 0.03 && std::max({a.latSd, b.latSd, c.latSd, d.latSd, e.latSd, g.latSd}) <= 2.0 &&
                  a.snaps + b.snaps + c.snaps + d.snaps + e.snaps + g.snaps == 0 && a.resyncs + b.resyncs + c.resyncs + d.resyncs + e.resyncs + g.resyncs == 0 && a.frames > 1800,
              F("err p95 %.3f/%.3f/%.3f/%.3f/%.3f/%.3f latency sd %.1f/%.1f/%.1f/%.1f/%.1f/%.1f", a.errP95, b.errP95, c.errP95, d.errP95, e.errP95, g.errP95, a.latSd, b.latSd, c.latSd, d.latSd, e.latSd, g.latSd));
    }

    // A sender that freezes (a game hitch, an alt-tab) for 400 ms and Wi-Fi style downlink bursts of up to 150 ms every 4 s.
    {
        E2eProfile h = base;
        h.hitchAtMs = 15000; h.hitchLenMs = 400;
        E2eProfile w = base;
        w.burstEveryMs = 4000; w.burstMs = 300; w.burstExtraMs = 150;
        const E2eResult a = RunE2e(nearC, h), b = RunE2e(nearC, w), c = RunE2e(midC, w);
        PrintE2e("near, sender hitch 400 ms at 15 s", a);
        PrintE2e("near, 150 ms bursts every 4 s", b);
        PrintE2e("mid, 150 ms bursts every 4 s", c);
        Check("e2e hitch and bursts: a 400 ms sender hitch is bridged without a snap (extrapolation <= 250 ms, the ped catches up: p95 error <= 2 cm over the run), 150 ms downlink bursts keep the p95 "
              "position error <= 25 cm with no snap, no stall and no frozen ped",
              a.snaps == 0 && a.longestExtrapMs <= 250.0 && a.errP95 <= 0.02 && b.snaps + c.snaps == 0 && b.stalls + c.stalls == 0 && b.errP95 <= 0.25 && c.errP95 <= 0.25 && b.longestExtrapMs <= 250.0,
              F("hitch: err p95 %.3f max %.2f; bursts near %.3f mid %.3f, longest %.0f ms", a.errP95, a.errMax, b.errP95, c.errP95, b.longestExtrapMs));
    }
}

// A car on a circle (radius 60 m, `speed` m/s) through the real VehicleStore (controller's VehicleUpdate at 45 Hz with SampleClock placement,
// BuildFrame's tiers and ageMs, the VehicleWorld codec) into the engine's receive path (SnapshotStampMs on the shared NetClock, the interpolator,
// the car's class extra), drawn at 60 fps: the vehicle half of the end-to-end pipeline. Same scoring as RunE2e: the car against the path at a
// constant latency, the sd of that latency, the largest per-frame step against the expected one.
struct E2eCarResult { size_t frames = 0; double errP95 = 0, errMax = 0, latMed = 0, latSd = 0, stepMin = 1e9, stepMax = 0, gapMean = 0; uint32_t stale = 0, repeats = 0; };

static E2eCarResult RunCarE2e(double cx, double speed, double downJitMs, double downLoss, double upJitMs, double upLoss, uint64_t seed) {
    Rng rng(seed);
    const double tickMs = kTick30, durMs = 40000, vOff = 54321.7, cOff = 777.3, rf = 1000.0 / 60.0, r = 60.0, sOff = 1234.5;
    const uint64_t tickUs = 1000000ull / 30;
    struct E { double t; uint64_t o; int kind; double sT; uint16_t stamp; proto::VehicleUpdate u; std::vector<uint8_t> b; };
    auto cmp = [](const E& a, const E& b) { return a.t != b.t ? a.t > b.t : a.o > b.o; };
    std::priority_queue<E, std::vector<E>, decltype(cmp)> q(cmp);
    uint64_t ord = 0;
    auto push = [&](E e) { e.o = ord++; q.push(std::move(e)); };
    auto at = [&](double t, proto::Kinematics& k, proto::Quat& qu, double& w) {
        const double om = speed / r, th = om * t / 1000.0;
        k.x = float(cx + r * std::cos(th)); k.y = float(r * std::sin(th)); k.z = 30;
        k.vx = float(-speed * std::sin(th)); k.vy = float(speed * std::cos(th)); k.vz = 0;
        qu.x = 0; qu.y = 0; qu.z = float(std::sin(th / 2)); qu.w = float(std::cos(th / 2));
        w = om;
    };
    {
        double nextSend = 0;
        uint32_t seq = 0;
        for (long f = 0; double(f) * (1000.0 / 60) <= durMs; ++f) {
            const double t = 3.7 + double(f) * (1000.0 / 60) + (rng.Uniform() - 0.5) * 2.0;
            if (t < nextSend) continue;
            const double due = nextSend + tickMs / 1.5;
            nextSend = due > t ? due : t + tickMs / 1.5;
            E e{};
            e.kind = 0; e.sT = t; e.stamp = uint16_t(uint64_t(t + sOff) & 0xFFFFu);
            double w;
            at(t, e.u.k, e.u.rot, w);
            e.u.avz = float(w); e.u.vehId = 0; e.u.seq = ++seq; e.u.flags = proto::kVehEngine;
            if (upLoss > 0 && rng.Uniform() < upLoss) continue;
            e.t = t + rng.Uniform() * 2.0 + 25.0 + rng.Uniform() * upJitMs;
            push(e);
        }
    }
    for (uint64_t i = uint64_t(std::ceil(vOff * 1000.0 / double(tickUs))) + 2;; ++i) {
        const double T = double(i * tickUs) / 1000.0 - vOff;
        if (T > durMs + 500) break;
        if (T < 0) continue;
        E e{};
        e.kind = 1; e.t = T + rng.Uniform() * 0.6; e.sT = double(i);
        push(e);
    }
    for (long f = 0; double(f) * rf <= durMs; ++f) { E e{}; e.kind = 3; e.t = 2.3 + double(f) * rf; push(e); }

    server::VehicleStore vs;
    server::VehicleStore::Rules rules;
    rules.nearEvery = 1; rules.midEvery = 2; rules.farEvery = 3; rules.refreshTicks = 30;
    vs.Configure(rules, 8);
    vs.AddReceiver(0);
    vs.AddReceiver(1);
    uint16_t vid = 0;
    proto::Kinematics pos0;
    pos0.x = float(cx + r); pos0.z = 30;
    vs.Create(0x1234, pos0, proto::Quat(), 0, 0, 0, 0, false, 0, 1, &vid);
    vs.Enter(0, vid, 0, pos0, 0, 1);
    SampleClock sc;
    NetClock clock(NetClockConfig::ForTickHz(30));
    VehicleInterpolator interp;
    ClassDelay extra;
    const proto::Kinematics rpos; // the receiver stands at the origin
    struct Fr { double T, x, y; };
    std::vector<Fr> frames;
    std::vector<double> gaps;
    double lastStored = -1;
    E2eCarResult R;
    while (!q.empty()) {
        E e = q.top();
        q.pop();
        const double T = e.t;
        if (e.kind == 0) {
            const uint64_t recvUs = uint64_t((T + vOff) * 1000.0);
            bool jumped = false;
            vs.Update(0, e.u, recvUs / 1000, &jumped, sc.Place(e.stamp, recvUs) / 1000);
        } else if (e.kind == 1) {
            const uint32_t tick = uint32_t(e.sT);
            const uint64_t nowMs = uint64_t(tick) * tickUs / 1000;
            std::vector<uint16_t> gone;
            vs.Tick(nowMs, tick, &gone);
            std::vector<server::VehicleStore::ControlCandidate> cc(2);
            cc[0].id = 0; cc[0].pos = pos0; cc[0].hasState = true; cc[0].trusted = true;
            cc[1].id = 1; cc[1].pos = rpos; cc[1].hasState = true; cc[1].trusted = true;
            vs.AssignControllers(nowMs, tick, cc);
            proto::VehicleWorld fr;
            if (vs.BuildFrame(1, true, rpos, tick, nowMs, fr)) {
                const proto::Bytes b = proto::Encode(fr);
                if (!b.empty() && rng.Uniform() >= downLoss) {
                    E d{};
                    d.kind = 2; d.t = T + 25.0 + rng.Uniform() * downJitMs; d.b = b;
                    push(d);
                }
            }
        } else if (e.kind == 2) {
            proto::VehicleWorld vw;
            if (!proto::Decode(e.b.data(), e.b.size(), vw)) continue;
            const double now = T + cOff;
            clock.OnFrame(vw.tick, now);
            const int64_t ft = clock.Unwrap(vw.tick);
            for (const proto::VehicleState& st : vw.vehicles) {
                if (st.ctrl == 1) continue;
                VehicleSnapshot s;
                s.tMs = SnapshotStampMs(ft, clock.TickMs(), st.ageMs);
                s.pos = {st.k.x, st.k.y, st.k.z}; s.vel = {st.k.vx, st.k.vy, st.k.vz};
                s.rot = {st.rot.x, st.rot.y, st.rot.z, st.rot.w}; s.angVel = {st.avx, st.avy, st.avz};
                s.hasAngVel = true; s.steering = st.steer;
                const VehPush pr = interp.Push(s);
                if (pr == VehPush::Accepted || pr == VehPush::Teleported) {
                    clock.NoteAge(st.ageMs);
                    if (lastStored >= 0) gaps.push_back(now - lastStored);
                    lastStored = now;
                }
            }
        } else {
            const double now = T + cOff;
            if (!clock.Ready()) continue;
            const double ex = extra.UpdateGap(interp.TypicalGapMs(), clock.TickMs(), now, clock.Config().maxSlew);
            VehicleSample out;
            if (interp.Sample(clock.RenderT(now) - ex, out)) frames.push_back({T, out.pos.x, out.pos.y});
        }
    }
    std::vector<double> taus, errs, steps;
    size_t nf = 0;
    for (const Fr& f : frames) {
        if (f.T < 6000 || (nf++ & 3) != 0) continue;
        double best = 1e9, bt = 0;
        for (double tau = 20; tau <= 600; tau += 2.0) {
            proto::Kinematics k; proto::Quat qq; double w;
            at(f.T - tau, k, qq, w);
            const double d = std::hypot(f.x - k.x, f.y - k.y);
            if (d < best) { best = d; bt = tau; }
        }
        taus.push_back(bt);
    }
    R.latMed = Percentile(taus, 0.5);
    {
        double m = 0, v = 0;
        for (double t : taus) m += t;
        m /= double(std::max<size_t>(1, taus.size()));
        for (double t : taus) v += (t - m) * (t - m);
        R.latSd = std::sqrt(v / double(std::max<size_t>(1, taus.size())));
    }
    const Fr* pv = nullptr;
    for (const Fr& f : frames) {
        if (f.T < 6000) continue;
        proto::Kinematics k; proto::Quat qq; double w;
        at(f.T - R.latMed, k, qq, w);
        errs.push_back(std::hypot(f.x - k.x, f.y - k.y));
        if (pv) steps.push_back(std::hypot(f.x - pv->x, f.y - pv->y) / (speed * rf / 1000.0));
        pv = &f;
        ++R.frames;
    }
    R.errP95 = Percentile(errs, 0.95); R.errMax = Percentile(errs, 1.0);
    R.stepMin = Percentile(steps, 0.0); R.stepMax = Percentile(steps, 1.0);
    double gm = 0;
    for (double g : gaps) gm += g;
    R.gapMean = gm / double(std::max<size_t>(1, gaps.size()));
    R.stale = interp.Stats().stale; R.repeats = interp.Stats().repeats;
    return R;
}

static void TestEndToEndVehicles() {
    // Receiver at the origin, the car on a circle of 60 m around (cx, 0): near < 120 m, mid < 250 m, far beyond.
    const E2eCarResult n = RunCarE2e(50, 20, 10, 0.02, 8, 0, 3), m = RunCarE2e(180, 20, 10, 0.02, 8, 0, 3), f = RunCarE2e(350, 20, 10, 0.02, 8, 0, 3);
    const E2eCarResult fast = RunCarE2e(50, 35, 10, 0.02, 8, 0, 3), hostile = RunCarE2e(50, 20, 60, 0.05, 20, 0, 3), hostileMid = RunCarE2e(180, 20, 60, 0.05, 20, 0, 3);
    const E2eCarResult up = RunCarE2e(50, 20, 10, 0.02, 8, 0.05, 3);
    auto line = [](const char* name, const E2eCarResult& r) {
        std::printf("  [car pipeline %-30s] err p95 %.3f max %.2f m | latency %.0f ms (sd %.1f) | step %.2f..%.2fx | stored gap mean %.0f ms | stale %u repeats %u\n", name, r.errP95, r.errMax, r.latMed,
                    r.latSd, r.stepMin, r.stepMax, r.gapMean, r.stale, r.repeats);
    };
    line("near 20 m/s circle", n); line("mid", m); line("far", f); line("near 35 m/s", fast); line("near, 80 ms-ish jitter, 5% loss", hostile); line("mid, same", hostileMid); line("near, 5% uplink loss", up);
    Check("e2e vehicles: a car at 20 m/s through the real VehicleStore (45 Hz controller, SampleClock, tiers 30 / 15 / 10 Hz, ageMs, the VehicleWorld codec) is shown within 6 cm (p95: about 2.5 ms of time-base noise at 20 m/s) of its path at a steady latency "
          "(sd <= 2 ms: 127 / 160 / 193 ms behind), every frame within 0.9..1.1 of the expected step; 35 m/s within 10 cm",
          n.frames > 1800 && m.frames > 1800 && f.frames > 1800 && n.errP95 <= 0.06 && m.errP95 <= 0.06 && f.errP95 <= 0.06 && std::max({n.latSd, m.latSd, f.latSd}) <= 2.0 &&
              std::min({n.stepMin, m.stepMin, f.stepMin}) >= 0.9 && std::max({n.stepMax, m.stepMax, f.stepMax}) <= 1.1 && fast.errP95 <= 0.10 && n.latMed > 110 && n.latMed < 145 && f.latMed > n.latMed + 40,
          F("err p95 %.3f/%.3f/%.3f fast %.3f, latency %.0f/%.0f/%.0f sd %.1f/%.1f/%.1f, step %.2f..%.2f", n.errP95, m.errP95, f.errP95, fast.errP95, n.latMed, m.latMed, f.latMed, n.latSd, m.latSd, f.latSd,
            std::min({n.stepMin, m.stepMin, f.stepMin}), std::max({n.stepMax, m.stepMax, f.stepMax})));
    Check("e2e vehicles hostile: 60 ms downlink jitter with 5% loss and 20 ms uplink jitter (near and mid), and 5% uplink loss: p95 position error <= 25 cm (a 20 m/s car, delay adapting), steps within 0.7..1.3, no frozen car",
          hostile.frames > 1800 && hostile.errP95 <= 0.25 && hostileMid.errP95 <= 0.25 && up.errP95 <= 0.25 && std::min({hostile.stepMin, hostileMid.stepMin, up.stepMin}) >= 0.7 &&
              std::max({hostile.stepMax, hostileMid.stepMax, up.stepMax}) <= 1.3,
          F("err p95 %.3f/%.3f/%.3f, steps %.2f..%.2f", hostile.errP95, hostileMid.errP95, up.errP95, std::min({hostile.stepMin, hostileMid.stepMin, up.stepMin}),
            std::max({hostile.stepMax, hostileMid.stepMax, up.stepMax})));
}

int main() {
    std::printf("-- steady streams\n");
    TestSteady(60, 20, true, "steady 20 Hz @60fps");
    TestSteady(144, 20, true, "steady 20 Hz @144fps");
    TestSteady(60, 30, true, "steady 30 Hz server @60fps");
    TestSteady(60, 20, false, "steady 20 Hz @60fps linear");
    std::printf("-- loss and jitter\n");
    TestLossAndJitter();
    std::printf("-- slow client\n");
    TestSlowClient();
    std::printf("-- continuity\n");
    TestContinuity();
    std::printf("-- ordering\n");
    TestOrdering();
    std::printf("-- heading\n");
    TestHeading();
    std::printf("-- extrapolation\n");
    TestExtrapolation();
    std::printf("-- teleport\n");
    TestTeleport();
    std::printf("-- RemoteSync\n");
    TestRemoteSync();
    TestForget();
    std::printf("-- sample age\n");
    TestSampleAge();
    TestIdleAndLeft();
    std::printf("-- ped state\n");
    TestPedState();
    std::printf("-- sample clock (v5)\n");
    TestSampleClock();
    std::printf("-- one render clock (W7)\n");
    TestNetClock();
    TestOneClock();
    std::printf("-- sender pipeline\n");
    TestSenderPipeline();
    std::printf("-- config\n");
    TestConfig();
    std::printf("-- W8: class extra hysteresis, standing-still settle\n");
    TestClassHold();
    TestClassHysteresis();
    TestStillSettle();
    std::printf("-- sync pipeline review: position denoise, diagnostics, end to end through the real replication\n");
    TestDenoise();
    TestPipelineDiagnostics();
    TestEndToEnd();
    TestEndToEndVehicles();

    // steady 4x3 + loss/jitter 10 + slow client 3 + continuity 2 + ordering 8 +
    // heading 3 + extrapolation 3 + teleport 5 + RemoteSync 2 + forget 4 +
    // sample age 2 + idle/left 3 + ped state 5 + sample clock 3 (v5) + NetClock 6 + one clock 3 + sender pipeline 5 (W7) + config 4
    // + W8: class hold 1 + class extra hysteresis 1 + standing-still settle 1
    // + sync pipeline review: denoise 6, diagnostics 4, end to end 9, vehicles end to end 2
    const int expected = 12 + 10 + 3 + 2 + 8 + 3 + 3 + 5 + 2 + 4 + 2 + 3 + 5 + 3 + 6 + 3 + 5 + 4 + 3 + 6 + 4 + 9 + 2;
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
