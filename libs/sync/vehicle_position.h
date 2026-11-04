// vehicle_position.h: the 10 s diagnostic of where a remote car is DRAWN against where its owner really has it (R3 T5, docs/sync-specs/vehicle-position.md).
// Pure C++17, header-only, no game calls, single-threaded (the engine's script tick feeds one Frame per driven copy per frame and prints Format() every 10 s).
//
// The question (GI, live: "p2 hits me on his screen, for me he is about 2-3 meters away just staggering"): how far is the copy on this machine from the owner's
// newest report, extrapolated to the same instant, and WHICH TERM is it? Per frame and copy the engine measures
//   total   |drawn - ideal|   the entity's position against the owner's snapshots sampled at (NearT + link lag): the best estimate of the owner's real position now
//   target  |target - ideal|  what the interpolation time aims at against the ideal: the render delay the lead did not close, the lead's slew, a lead weight below 1
//   track   |drawn - target|  the drive controller's own error: its correction cap, blending, the physics, a grace
// and the times behind them: unled = how far (ms) the interpolation time is behind the ideal time, lead = what PresentLead added.
// Bands: the copy's speed (0-5, 5-15, 15-30, over 30 m/s) and its distance from us (under 15 m, 15-45 m, farther).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gage::sync {

    struct PosErrStat
    {
        static constexpr int kBins = 80;          // 5 cm bins, the last is "3.95 m or more"
        uint32_t n = 0;
        double sum = 0, max = 0;
        uint32_t bins[kBins] = {};
        void Add(double m)
        {
            if (!std::isfinite(m) || m < 0.0) return;
            ++n;
            sum += m;
            if (m > max) max = m;
            int b = static_cast<int>(m / 0.05);
            if (b >= kBins) b = kBins - 1;
            ++bins[b];
        }
        double Mean() const { return n ? sum / n : 0.0; }
        double P95() const
        {
            if (n == 0) return 0.0;
            const double rank = 0.95 * static_cast<double>(n - 1);
            uint32_t seen = 0;
            for (int b = 0; b < kBins; ++b)
            {
                seen += bins[b];
                if (static_cast<double>(seen) > rank) return b == kBins - 1 ? max : (b + 1) * 0.05;
            }
            return max;
        }
    };

    class PositionDiag
    {
    public:
        static constexpr int kSpeedBands = 4, kDistBands = 3;
        static int SpeedBand(double mps) { return !(mps >= 5.0) ? 0 : (mps < 15.0 ? 1 : (mps < 30.0 ? 2 : 3)); }
        static int DistBand(double m) { return !(m >= 15.0) ? 0 : (m < 45.0 ? 1 : 2); }
        static const char* SpeedName(int b) { static const char* const n[] = {"0-5", "5-15", "15-30", "over 30"}; return n[b < 0 ? 0 : (b > 3 ? 3 : b)]; }
        static const char* DistName(int b) { static const char* const n[] = {"under 15 m", "15-45 m", "over 45 m"}; return n[b < 0 ? 0 : (b > 2 ? 2 : b)]; }

        struct Frame
        {
            double speedMps = 0, distM = 0;
            double totalM = 0, targetM = 0, trackM = 0;   // see the header
            double unledMs = 0, leadMs = 0;
            bool snapped = false;
        };
        struct Window
        {
            PosErrStat total[kSpeedBands], target[kSpeedBands], track[kSpeedBands];
            PosErrStat totalByDist[kDistBands];
            double unledSum[kSpeedBands] = {}, leadSum[kSpeedBands] = {};
            uint32_t frames = 0, snaps = 0;
            uint32_t speedFrames[kSpeedBands] = {};
        };

        void Add(const Frame& f)
        {
            if (!std::isfinite(f.totalM) || !std::isfinite(f.speedMps) || !std::isfinite(f.distM)) return;
            const int sb = SpeedBand(f.speedMps), db = DistBand(f.distM);
            w_.total[sb].Add(f.totalM);
            w_.target[sb].Add(f.targetM);
            w_.track[sb].Add(f.trackM);
            w_.totalByDist[db].Add(f.totalM);
            if (std::isfinite(f.unledMs)) w_.unledSum[sb] += f.unledMs;
            if (std::isfinite(f.leadMs)) w_.leadSum[sb] += f.leadMs;
            ++w_.frames;
            ++w_.speedFrames[sb];
            if (f.snapped) ++w_.snaps;
        }
        Window Take()
        {
            const Window w = w_;
            w_ = Window();
            return w;
        }

        // "speed 15-30 m/s: 412 frames, drawn vs owner now mean 0.31 p95 0.70 max 1.20 m (aim 0.12, tracking 0.25), aim behind by 9 ms, lead 188 ms" per band with data,
        // then the same by distance. Empty bands are left out; "no copies within range" when there were none.
        static std::string Format(const Window& w)
        {
            if (w.frames == 0) return "no remote copy within range";
            std::string out;
            char buf[320];
            for (int b = 0; b < kSpeedBands; ++b)
            {
                if (w.total[b].n == 0) continue;
                const double n = static_cast<double>(w.total[b].n);
                std::snprintf(buf, sizeof(buf),
                              "%sspeed %s m/s: %u frames, drawn vs the owner's report now mean %.2f p95 %.2f max %.2f m (aim %.2f, tracking %.2f), aim %.0f ms behind, lead %.0f ms",
                              out.empty() ? "" : "; ", SpeedName(b), w.total[b].n, w.total[b].Mean(), w.total[b].P95(), w.total[b].max, w.target[b].Mean(),
                              w.track[b].Mean(), w.unledSum[b] / n, w.leadSum[b] / n);
                out += buf;
            }
            for (int d = 0; d < kDistBands; ++d)
            {
                if (w.totalByDist[d].n == 0) continue;
                std::snprintf(buf, sizeof(buf), "; %s: mean %.2f p95 %.2f max %.2f m", DistName(d), w.totalByDist[d].Mean(), w.totalByDist[d].P95(),
                              w.totalByDist[d].max);
                out += buf;
            }
            std::snprintf(buf, sizeof(buf), "; drive snaps %u", w.snaps);
            out += buf;
            return out;
        }

    private:
        Window w_;
    };

}   // namespace gage::sync
