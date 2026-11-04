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
        static constexpr double kNearFullM = 40.0;   // PresentLead's NearLeadConfig().fullWithinM
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
            // R4 T2: the same two sums for the frames inside the full-lead range only (kNearFullM): the all-distance averages mix in the 40-120 m ring where the lead
            // fades to zero on purpose (a car over 30 m/s spends most of its frames there: 'lead 104 ms against 176' is that arithmetic, not a cap)
            double unledNearSum[kSpeedBands] = {}, leadNearSum[kSpeedBands] = {};
            uint32_t nearFrames[kSpeedBands] = {};
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
            if (f.distM <= kNearFullM && std::isfinite(f.unledMs) && std::isfinite(f.leadMs))
            {
                w_.unledNearSum[sb] += f.unledMs;
                w_.leadNearSum[sb] += f.leadMs;
                ++w_.nearFrames[sb];
            }
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
                if (w.nearFrames[b] > 0)
                {
                    std::snprintf(buf, sizeof(buf), " [within 40 m: %u frames, aim %.0f ms behind, lead %.0f ms]", w.nearFrames[b], w.unledNearSum[b] / w.nearFrames[b],
                                  w.leadNearSum[b] / w.nearFrames[b]);
                    out += buf;
                }
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


    // R4 T2 (docs/sync-specs/vehicle-collision-authority.md): the divergence check after EVERY contact, not only after an adopted response (T4's check compared 0 times in
    // every window of GI's log). A watch starts on a copy when a contact opens its grace (collision flag, speed loss, kick, proximity), when the server's impact for one of
    // OUR cars is applied (the other car's copy is the one to watch), when its response is adopted, or when a world touch was ignored by the contact gate; the copy's
    // distance to its owner's report is then read at +0.5 / +1 / +2 s. totalM = |entity - the owner's report at now + link lag| (what the player sees against where the owner is);
    // trackM = |entity - the drive target| (the controller's own error). Pure, single-threaded (the script tick), no allocation.
    class ContactWatch
    {
    public:
        enum Cause : uint8_t { kNone = 0, kFlag = 1, kSpeedLoss = 2, kKick = 3, kProximity = 4, kServerImpact = 5, kAdoption = 6, kWorld = 7, kCauses = 8 };
        static constexpr int kChecks = 3;
        static constexpr double kCheckMs[kChecks] = {500.0, 1000.0, 2000.0};
        static constexpr double kLateMs = 300.0;      // a check read later than this after it was due is skipped (a hitch, a copy out of range)
        static constexpr int kSlots = 8;
        struct Check { uint32_t n = 0, over1 = 0, missed = 0; double sum = 0, max = 0, trackSum = 0, trackMax = 0; };
        struct Window
        {
            uint32_t opened[kCauses] = {};
            uint32_t merged = 0, dropped = 0;
            Check check[kChecks];
        };
        static const char* CauseName(int c)
        {
            static const char* const n[] = {"?", "flag", "speed loss", "kick", "proximity", "server impact", "adoption", "world touch (ignored)"};
            return n[c < 0 || c >= kCauses ? 0 : c];
        }

        // Start a watch on copy `id`. A watch already running on it absorbs the event (counted as merged); false then. A full table drops it.
        bool Open(uint16_t id, uint8_t cause, double nowMs)
        {
            if (!std::isfinite(nowMs) || cause == kNone || cause >= kCauses) return false;
            for (Slot& sl : slot_)
                if (sl.used && sl.id == id && nowMs - sl.t0 < kCheckMs[kChecks - 1] + kLateMs) { ++w_.merged; return false; }
            for (Slot& sl : slot_)
            {
                if (sl.used) continue;
                sl.used = true; sl.id = id; sl.cause = cause; sl.t0 = nowMs; sl.done = 0;
                ++w_.opened[cause];
                return true;
            }
            ++w_.dropped;
            return false;
        }
        bool Active(uint16_t id) const { for (const Slot& sl : slot_) if (sl.used && sl.id == id) return true; return false; }
        // One frame of copy `id`: completes the checks that are due. Returns the number completed (0..3).
        int Sample(uint16_t id, double nowMs, double totalM, double trackM)
        {
            int completed = 0;
            for (Slot& sl : slot_)
            {
                if (!sl.used || sl.id != id) continue;
                const double age = nowMs - sl.t0;
                for (int k = 0; k < kChecks; ++k)
                {
                    if ((sl.done & (1u << k)) || age < kCheckMs[k]) continue;
                    sl.done |= 1u << k;
                    Check& c = w_.check[k];
                    if (age - kCheckMs[k] > kLateMs || !std::isfinite(totalM) || !std::isfinite(trackM)) { ++c.missed; continue; }
                    ++c.n; ++completed;
                    c.sum += totalM; c.trackSum += trackM;
                    if (totalM > c.max) c.max = totalM;
                    if (trackM > c.trackMax) c.trackMax = trackM;
                    if (totalM > 1.0) ++c.over1;
                }
                if (sl.done == (1u << kChecks) - 1u || age > kCheckMs[kChecks - 1] + kLateMs) sl.used = false;
            }
            return completed;
        }
        void Drop(uint16_t id) { for (Slot& sl : slot_) if (sl.used && sl.id == id) sl.used = false; }
        Window Take()
        {
            const Window w = w_;
            w_ = Window();
            return w;
        }
        // "watched 5 (flag 2, proximity 1, server impact 2, merged 1): +0.5 s 5 compared mean 0.42 max 0.90 m (over 1 m 0; vs the drive target mean 0.30 max 0.60), +1 s ..., +2 s ..."
        static std::string Format(const Window& w)
        {
            uint32_t total = 0;
            for (int c = 1; c < kCauses; ++c) total += w.opened[c];
            if (total == 0 && w.merged == 0 && w.dropped == 0) return "no contact watched";
            std::string out;
            char buf[260];
            std::snprintf(buf, sizeof(buf), "watched %u", total);
            out += buf;
            const char* sep = " (";
            for (int c = 1; c < kCauses; ++c)
            {
                if (w.opened[c] == 0) continue;
                std::snprintf(buf, sizeof(buf), "%s%s %u", sep, CauseName(c), w.opened[c]);
                out += buf;
                sep = ", ";
            }
            if (w.merged) { std::snprintf(buf, sizeof(buf), "%smerged into a running watch %u", sep, w.merged); out += buf; sep = ", "; }
            if (w.dropped) { std::snprintf(buf, sizeof(buf), "%swatch table full %u", sep, w.dropped); out += buf; sep = ", "; }
            if (sep[0] == ',') out += ")";
            static const char* const nm[kChecks] = {"+0.5 s", "+1 s", "+2 s"};
            for (int k = 0; k < kChecks; ++k)
            {
                const Check& c = w.check[k];
                if (c.n == 0)
                {
                    std::snprintf(buf, sizeof(buf), "; %s: 0 compared%s", nm[k], c.missed ? " (missed: no frame in time)" : "");
                    out += buf;
                    continue;
                }
                std::snprintf(buf, sizeof(buf), "; %s: %u compared, copy vs the owner's report mean %.2f max %.2f m (over 1 m: %u), vs the drive target mean %.2f max %.2f m", nm[k], c.n,
                              c.sum / c.n, c.max, c.over1, c.trackSum / c.n, c.trackMax);
                out += buf;
            }
            return out;
        }

    private:
        struct Slot { bool used = false; uint16_t id = 0; uint8_t cause = 0; double t0 = 0; uint32_t done = 0; };
        Slot slot_[kSlots];
        Window w_;
    };

}   // namespace gage::sync
