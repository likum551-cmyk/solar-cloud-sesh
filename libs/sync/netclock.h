// NetClock: the ONE render clock of a connection (sync plan W7, network spec N5). Remote peds (RemoteSync) and
// remote vehicles (VehicleInterpolator) both read the time they draw from it, so a car and the player beside it
// show the same instant of the server's timeline.
//
// Why one: until v5 the peds had a clock of their own per player (baseline = minimum of that player's last 100
// arrivals, delay = tick gap + p95 lateness + 25 ms, falling 2% per PACKET) and the vehicles another (a global
// decaying maximum of the arrival offset, a fixed 120 ms delay, no slew). On a jittered link the two disagreed by
// up to ~60 ms, so a passenger walked inside the car it sat beside, and a 1 Hz idle pair needed 50 s to lower a
// delay a burst had raised.
//
// How (everything is in milliseconds; "the timeline" is tick * tickMs, the server's own frame time, counted from
// the first frame this clock saw):
//  * Every frame the server sends (WorldState and VehicleWorld alike) is one observation: OnFrame(tick, recvMs).
//    offset = recvMs - tick * tickMs. The BASELINE is the minimum offset of the last baselineWindowMs (5 s) of
//    WALL time: the least-delayed frame. (The old smoother's 100-packet window was 5 s at 20 Hz and 0.4 s of a
//    busy 250 Hz stream; wall time does not care how many packets there are.)
//  * A frame's EXCESS is its offset above the baseline at that time: the network's jitter. The AGE of a sample
//    (WorldEntry::ageMs, VehicleState::ageMs: how long before its frame the server got it) is noted separately
//    with NoteAge, and only for samples that were new information (a repeat re-sent with a growing age asks for
//    no delay). Both are kept for statWindowMs (3 s) of wall time.
//  * DELAY = tick + p95(age) + p95(excess) + 0.5 tick, clamped to [minDelayMs, maxDelayMs]. Why: to interpolate you
//    need the sample AFTER the render time to have arrived: one frame spacing (the tick), plus how long after its
//    frame time that sample was taken (age), plus how late the frame came (excess), plus half a tick of margin.
//    A stream the server relays slower than every tick (mid and far players, cars in the far tier) needs more:
//    that is the CLASS EXTRA, (gap - tick), added by the consumer (ClassExtraMs, ClassDelay), not here.
//  * The delay RISES AT ONCE (the next frame that sees a larger p95) and FALLS WITH WALL TIME: every frame moves
//    it toward the target by 1 - exp(-dt / fallTauMs) of the way (2.5 s time constant), where dt is the time since
//    the previous frame. It is a rate per second, not per packet: a connection that only hears 1 Hz frames lowers
//    its delay as fast as one that hears 60 Hz.
//  * RenderT(nowMs) is the render clock: the timeline time to show now. It advances with the local clock and is
//    steered toward (nowMs - baseline - delay) at no more than +-10% speed (500 ms time constant), so estimate
//    changes never jump or reverse what is drawn; only an error over resyncMs (500 ms) resyncs hard (Resyncs()).
//    It is idempotent for one nowMs (every ped and every car of a frame asks and gets the same answer) and never
//    runs backwards. Consumers draw RenderT(now) - classExtra.
//  * Ticks are unwrapped (signed 32-bit distance from the newest) and a frame more than maxFrameDistanceMs of
//    ticks away from the newest is ignored (Stats().ignored): a restarted server is a new session, and a new
//    session is a new clock (Reset(), done by the engine's OnWelcome).
//
// Pure C++17: no OS headers, no clocks, no threads (the caller passes the local time of every observation, one
// monotonic clock, any epoch), no exceptions. Single-threaded: the engine calls it under g_peersMtx. Tested by
// tests/sync_test.cpp (NetClock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace gage::sync {

    // A windowed picture of how irregularly something arrives (pipeline diagnostics, sync pipeline review 2026-11-03):
    // the gaps (ms) between consecutive arrivals, kept as a count, a sum, a maximum and a 4 ms histogram (the last
    // bin is "252 ms or more"), so a mean, a p95 and a maximum over a window come out without storing the gaps.
    // The owner takes the window (Take) every few seconds and the numbers restart. Pure, no clock.
    struct ArrivalWindow
    {
        static constexpr int kBins = 64;
        static constexpr double kBinMs = 4.0;
        uint32_t n = 0;
        double sumMs = 0, maxMs = 0;
        uint32_t bins[kBins] = {};

        void Add(double gapMs)
        {
            if (!(gapMs >= 0.0) || gapMs > 1e7) return;
            ++n;
            sumMs += gapMs;
            if (gapMs > maxMs) maxMs = gapMs;
            int b = static_cast<int>(gapMs / kBinMs);
            if (b >= kBins) b = kBins - 1;
            ++bins[b];
        }
        double MeanMs() const { return n ? sumMs / static_cast<double>(n) : 0.0; }
        // The p-th percentile (0..1), to the bin: the upper edge of the bin holding that rank (the true maximum for the
        // overflow bin). 0 for an empty window.
        double PercentileMs(double p) const
        {
            if (n == 0) return 0.0;
            const double rank = p * static_cast<double>(n - 1);
            uint32_t seen = 0;
            for (int b = 0; b < kBins; ++b)
            {
                seen += bins[b];
                if (static_cast<double>(seen) > rank)
                    return b == kBins - 1 ? maxMs : (b + 1) * kBinMs;
            }
            return maxMs;
        }
        void Reset() { *this = ArrivalWindow(); }
    };

    struct NetClockConfig
    {
        double tickHz = 20.0;            // Welcome::tickHz

        double initialDelayMs = 100.0;   // 2 ticks: until kMinFrames frames are measured
        double minDelayMs = 75.0;        // 1.5 ticks
        double maxDelayMs = 300.0;       // at least 6 ticks
        double safetyMs = 25.0;          // half a tick

        double baselineWindowMs = 5000.0;// wall time the baseline (minimum offset) looks back
        double statWindowMs = 3000.0;    // wall time the excess and age percentiles look back
        double fallTauMs = 2500.0;       // the delay moves 1 - exp(-dt / this) of the way down per dt of wall time
        double slewTauMs = 500.0;        // render clock steering time constant
        double maxSlew = 0.10;           // +-10% render clock speed
        double resyncMs = 500.0;         // render clock error that resyncs hard
        double maxFrameDistanceMs = 10000.0; // a frame this far (in ticks) from the newest is ignored

        // The tick-derived values for a server rate: initial 2 ticks, min 1.5, max max(300 ms, 6 ticks), safety
        // half a tick. hz outside [1, 1000] (including 0 and NaN) falls back to 20.
        static NetClockConfig ForTickHz(double hz);
    };

    struct NetClockStats
    {
        uint32_t frames = 0;    // frames observed
        uint32_t ignored = 0;   // refused: non-finite, or too far from the newest tick
        uint32_t resyncs = 0;   // hard render clock resyncs
        uint32_t baselineSteps = 0;   // frames after which the baseline (the clock offset estimate) moved by more than 5 ms
        uint32_t delayRises = 0;      // times the delay rose at once (a jitter burst the estimate had not covered)
        uint32_t reorderedFrames = 0; // frames whose tick was older than the newest seen (jitter, counted as such)
    };

    class NetClock
    {
    public:
        static constexpr size_t kMinFrames = 16;      // frames measured before the delay adapts
        static constexpr size_t kMaxExcessSamples = 512;
        static constexpr size_t kMaxAgeSamples = 256;

        explicit NetClock(const NetClockConfig& cfg = NetClockConfig());

        // One frame of the server's timeline arrived (a WorldState or a VehicleWorld): its tick, received at
        // recvMs. Returns false when it was ignored. The same (tick, recvMs) twice in a row counts once, so a
        // caller that feeds it once per ENTRY of a frame is harmless. Reordered frames count (they are jitter).
        bool OnFrame(uint32_t tick, double recvMs);

        // The age (ms, 0..254) of a sample that was NEW information in a frame. 255 (saturated) and non-finite
        // values are ignored.
        void NoteAge(double ageMs);

        // True once a frame was observed: RenderT and the timeline functions mean something.
        bool Ready() const { return started_; }

        // The timeline time to show at nowMs (see above). Call it with the frame's time for every consumer.
        double RenderT(double nowMs);

        // The unwrapped tick (0 at the first frame this clock saw), the timeline time of a tick, and back: the
        // fractional ABSOLUTE tick (not wrapped) a timeline time stands for.
        int64_t Unwrap(uint32_t tick) const;
        double TimelineMs(uint32_t tick) const { return static_cast<double>(Unwrap(tick)) * tickMs_; }
        double TickAt(double timelineMs) const { return static_cast<double>(originTick_) + timelineMs / tickMs_; }
        double TickMs() const { return tickMs_; }

        double DelayMs() const { return delayMs_; }          // the shared part of the delay (not the class extra)
        double TargetDelayMs() const { return targetMs_; }   // what the delay is heading for
        double ExcessP95Ms() const { return excessP95_; }
        double AgeP95Ms() const { return ageP95_; }
        double LatenessMs() const { return excessP95_ + ageP95_; }
        double BaselineMs() const { return baseline_.empty() ? 0.0 : baseline_.front().v; }
        uint32_t Resyncs() const { return stats_.resyncs; }
        const NetClockStats& Stats() const { return stats_; }
        const NetClockConfig& Config() const { return cfg_; }

        // Pipeline diagnostics, taken by the engine's 10 s report. TakeFrameGaps: the arrival gaps (local receive time)
        // of the frames this clock heard since the last take (WorldStates and VehicleWorlds together: the link's own
        // rhythm). TakeBaselineDriftMs: how far the baseline (recv - tick * tickMs, the clock offset estimate) is from
        // where it was at the last take, and TakeBaselineStepMaxMs the largest single step it made since.
        ArrivalWindow TakeFrameGaps() { ArrivalWindow w = frameGaps_; frameGaps_.Reset(); return w; }
        double TakeBaselineDriftMs()
        {
            const double now = BaselineMs();
            const double d = haveBaseMark_ ? now - baseMark_ : 0.0;
            baseMark_ = now;
            haveBaseMark_ = true;
            return d;
        }
        double TakeBaselineStepMaxMs() { const double m = stepMaxMs_; stepMaxMs_ = 0.0; return m; }

        // Forget everything: a new session (its ticks restart, its timeline has nothing to do with the old one).
        void Reset();

    private:
        struct Sample { double t; double v; };
        void Retarget(double nowMs);

        NetClockConfig cfg_;
        double tickMs_;

        bool started_ = false;
        uint32_t originTick_ = 0, newestTick_ = 0;
        int64_t newestU_ = 0;
        uint32_t lastTick_ = 0;
        double lastRecvMs_ = 0;
        bool haveLast_ = false;

        std::deque<Sample> baseline_;  // monotonic deque of (recvMs, offset): the front is the minimum
        std::deque<Sample> excess_;    // (recvMs, offset above the baseline)
        std::deque<double> ages_;
        std::vector<double> scratch_;
        double excessP95_ = 0, ageP95_ = 0, targetMs_;
        double delayMs_;
        double lastRetargetMs_ = 0;

        bool haveRender_ = false;
        double renderT_ = 0, lastNowMs_ = 0;
        NetClockStats stats_;
        ArrivalWindow frameGaps_;
        double lastFrameRecvMs_ = 0, lastBase_ = 0, stepMaxMs_ = 0, baseMark_ = 0;
        bool haveFrameRecv_ = false, haveBase_ = false, haveBaseMark_ = false;
    };

    // ---- the per-class extra delay -------------------------------------------------------------------------

    // A stream the server relays every k-th tick (a mid-range ped at half the tick rate, a far car at a third)
    // needs (k - 1) ticks more delay than the shared clock gives a per-tick stream. `gapMs` is the observed spacing
    // of that stream's samples: the extra is the whole number of ticks it rounds to, minus one (0 for anything
    // within 1.5 ticks: the 22 / 44 ms alternation of a 45 Hz sender into a 30 Hz tick is the same class).
    inline double ClassExtraMs(double gapMs, double tickMs)
    {
        if (!(gapMs > 0.0) || !(tickMs > 0.0)) return 0.0;
        const double k = static_cast<double>(static_cast<long>(gapMs / tickMs + 0.5));
        return k > 1.0 ? (k - 1.0) * tickMs : 0.0;
    }

    // Hysteresis for the class number (live test 0.4.2, W8 item 2). ClassExtraMs rounds the typical gap to whole
    // ticks, so a stream whose gaps alternate (a 20 Hz sender into a 30 Hz tick: 33 / 33 / 67 ms, or 33 / 67) or
    // hover at 1.5 ticks flips between two classes whenever the median of the gap window wobbles; every flip made
    // ClassDelay slew the render time by 10% for a third of a second, a speed ripple of that player. The held class
    // changes only when the gap stays well outside the held class's band for a while:
    //   * up (more delay) when gap / tick >= class + 0.75 for kUpConfirmMs (1.5 s) in a row: starving is worse;
    //   * down (less delay) when gap / tick <= class - 0.8 for kDownConfirmMs (8 s) in a row;
    //   * anything in between, a reading that comes back inside the band, or one that changes the wanted class,
    //     restarts the wait. So 1 / 2 / 1 / 2 flapping settles on the larger class at most once and stays; a
    //     real tier change (a player walks from the near radius into the mid one) still arrives in 1.5 s.
    // The first reading sets the class at once (nothing to be steady against). Pure, no clock of its own.
    class ClassHold
    {
    public:
        static constexpr double kUpBand = 0.75, kDownBand = 0.8;
        static constexpr double kUpConfirmMs = 1500.0, kDownConfirmMs = 8000.0;

        // The held class's extra (ms) after seeing a gap of gapMs at nowMs. An unusable gap leaves the class alone.
        double Target(double gapMs, double tickMs, double nowMs)
        {
            if (!(gapMs > 0.0) || !(tickMs > 0.0))
                return Extra(tickMs);
            const double raw = gapMs / tickMs;
            const int want = Round(raw);
            if (!have_)
            {
                have_ = true;
                held_ = want;
                cand_ = 0;
                return Extra(tickMs);
            }
            const double dev = raw - static_cast<double>(held_);
            const bool outside = want != held_ && (dev >= kUpBand || dev <= -kDownBand);
            if (!outside)
            {
                cand_ = 0;
                return Extra(tickMs);
            }
            if (want != cand_)
            {
                cand_ = want;
                sinceMs_ = nowMs;
            }
            if (nowMs - sinceMs_ >= (want > held_ ? kUpConfirmMs : kDownConfirmMs))
            {
                held_ = want;
                cand_ = 0;
                ++changes_;
            }
            return Extra(tickMs);
        }
        int Held() const { return have_ ? held_ : 1; }
        uint32_t Changes() const { return changes_; }   // class changes after the first reading
        void Reset() { *this = ClassHold(); }

    private:
        static int Round(double raw)
        {
            const long k = static_cast<long>(raw + 0.5);
            return k < 1 ? 1 : (k > 1000 ? 1000 : static_cast<int>(k));
        }
        double Extra(double tickMs) const { return have_ && held_ > 1 && tickMs > 0.0 ? (held_ - 1) * tickMs : 0.0; }

        bool have_ = false;
        int held_ = 1, cand_ = 0;
        double sinceMs_ = 0.0;
        uint32_t changes_ = 0;
    };

    // The extra a consumer is currently using, steered toward its target at no more than maxSlew (10%) of the
    // elapsed time so a class change (a player walking out of the near radius) shifts the render time smoothly
    // instead of popping it. Idempotent for one nowMs. UpdateGap takes the typical gap itself and applies the
    // ClassHold hysteresis; Update takes a ready target (tests, callers with their own rule).
    class ClassDelay
    {
    public:
        double UpdateGap(double gapMs, double tickMs, double nowMs, double maxSlew)
        {
            return Update(hold_.Target(gapMs, tickMs, nowMs), nowMs, maxSlew);
        }
        uint32_t ClassChanges() const { return hold_.Changes(); }
        int HeldClass() const { return hold_.Held(); }

        double Update(double targetMs, double nowMs, double maxSlew)
        {
            if (!have_)
            {
                have_ = true;
                value_ = targetMs;
                lastMs_ = nowMs;
                return value_;
            }
            const double dt = nowMs > lastMs_ ? nowMs - lastMs_ : 0.0;
            if (dt > 0.0) lastMs_ = nowMs;
            const double step = maxSlew * dt;
            const double d = targetMs - value_;
            value_ += d > step ? step : (d < -step ? -step : d);
            return value_;
        }
        double Value() const { return value_; }
        void Reset() { have_ = false; value_ = 0.0; lastMs_ = 0.0; hold_.Reset(); }

    private:
        bool have_ = false;
        double value_ = 0.0, lastMs_ = 0.0;
        ClassHold hold_;
    };

}
