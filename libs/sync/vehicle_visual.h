// Vehicle visual state for the OTHER clients (Desync Overhaul round 2, T3; protocol v6): burst tyres, smashed windows,
// brake lights / handbrake and a steering value that also covers a standing car. Pure C++17, header-only, no game calls:
// the engine reads the natives and passes the values in, executes the commands this module names, and tells it what
// worked (Commit). Everything is testable offline (tests/t3_vehvisual_tests.h).
//
// The chain, per car:
//   controller (ctrl == me)   reads the entity's tyres / windows (VisualMask), puts them in VehicleUpdate.tyres /
//                             tyresRim / windows; ALSO applies what the server adds to its car (a Hit by someone else
//                             that popped a tyre) once, on the rising edge of the server's bit (never a clear: a repair
//                             is the controller's own business and it reports it).
//   server                    merges the controller's report with the Hits (apps/server/vehicles.h Update / HitPart)
//                             and tells everybody in the full entries.
//   copy (somebody else's)    applies the server's mask LEVEL-wise (burst what is missing, fix what is extra) and
//                             reports the pops its OWN game made (the local player shot a tyre of a car it does not
//                             control) as vehicle Hits; a reported pop is kept for pendingMs while the server confirms,
//                             so the round trip is not drawn as pop -> fix -> pop.
//
// Natives are never assumed: a failed read means "no information" (the shown mask is used, nothing is reported), a
// failed command is retried a few times and then the car is left alone for a while (failLimit / failBackoffMs), so an
// unresolved hash (hook.h: constant 0, World:: wrapper returns false) costs a handful of calls, never a stream.
#pragma once

#include "protocol/protocol.h"
#include "sync/vehicle_drive.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gage::sync {

    constexpr int kVisSlots = 8;                       // wheel slots and window slots, 0..7 (protocol.h kVehWheelSlots)
    static_assert(kVisSlots == (int)proto::kVehWheelSlots && kVisSlots == (int)proto::kVehWindowSlots, "slot counts");

    // Per-wheel and per-window state as bit sets (bit i = slot i): tyres = burst, rim = completely burst (a subset of
    // tyres), windows = smashed.
    struct VisualMask
    {
        uint8_t tyres = 0, rim = 0, windows = 0;
        bool operator==(const VisualMask& o) const { return tyres == o.tyres && rim == o.rim && windows == o.windows; }
        bool operator!=(const VisualMask& o) const { return !(*this == o); }
        bool Any() const { return (tyres | rim | windows) != 0; }
    };
    inline VisualMask NormalizeVisual(VisualMask m)
    {
        m.tyres = (uint8_t)(m.tyres | m.rim);          // a tyre off its rim is a burst tyre
        return m;
    }
    inline VisualMask VisOr(const VisualMask& a, const VisualMask& b)
    {
        return VisualMask{ (uint8_t)(a.tyres | b.tyres), (uint8_t)(a.rim | b.rim), (uint8_t)(a.windows | b.windows) };
    }
    inline VisualMask VisMinus(const VisualMask& a, const VisualMask& b)
    {
        return VisualMask{ (uint8_t)(a.tyres & ~b.tyres), (uint8_t)(a.rim & ~b.rim), (uint8_t)(a.windows & ~b.windows) };
    }
    inline int PopCount8(unsigned v)
    {
        int n = 0;
        for (v &= 0xFFu; v; v >>= 1) n += (int)(v & 1u);
        return n;
    }
    inline int VisBits(const VisualMask& m) { return PopCount8(m.tyres) + PopCount8(m.rim) + PopCount8(m.windows); }

    // What to do to the entity, slot masks: burst = SET_VEHICLE_TYRE_BURST(onRim false), rim = (onRim true), fixTyre =
    // SET_VEHICLE_TYRE_FIXED, smash = SMASH_VEHICLE_WINDOW, fixWindow = FIX_VEHICLE_WINDOW.
    struct VisualCmd
    {
        uint8_t burst = 0, rim = 0, fixTyre = 0, smash = 0, fixWindow = 0;
        bool Any() const { return (burst | rim | fixTyre | smash | fixWindow) != 0; }
        int Bits() const { return PopCount8(burst) + PopCount8(rim) + PopCount8(fixTyre) + PopCount8(smash) + PopCount8(fixWindow); }
    };

    struct VisualConfig
    {
        double pendingMs = 2500.0;      // a pop this copy reported waits this long for the server's confirmation
        double readCopyMs = 1500.0;     // a near copy is re-read this often (a level check against the server's mask)
        double readShotMs = 200.0;      // ... and this often while the local player shot lately
        int    failLimit = 3;           // failed command rounds in a row before the car is left alone...
        double failBackoffMs = 5000.0;  // ... for this long
    };

    struct VisualStats
    {
        uint32_t reads = 0;             // Update calls with a readback
        uint32_t divergent = 0;         // readbacks whose state differed from what the copy should show (before the commands)
        uint32_t stuck = 0;             // ... the same difference 3 reads in a row after commands (a native that does nothing)
        uint32_t burstsApplied = 0, rimsApplied = 0, tyresFixed = 0, windowsSmashed = 0, windowsFixed = 0;
        uint32_t reported = 0;          // local pops reported (bits)
        uint32_t confirmed = 0;         // ... that the server then showed
        uint32_t expired = 0;           // ... that the server never showed (rejected: the copy is put right)
        uint32_t cmdFailures = 0;       // command rounds in which nothing could be done
        uint32_t suspends = 0;
    };

    struct VisualIn
    {
        VisualMask server;              // the newest full state's mask
        bool iControl = false;          // the server says WE control this car
        bool readTyres = false;         // `local.tyres` / `local.rim` are a real readback (IS_VEHICLE_TYRE_BURST could be called)
        bool readWindows = false;       // `local.windows` is
        VisualMask local;               // what the entity shows (the groups that were read)
        double nowMs = 0;
    };
    struct VisualOut
    {
        VisualCmd cmd;                  // natives to call
        VisualMask report;              // copy only: new local pops to tell the server (tyres without rim bits are partial bursts)
        bool suspended = false;         // commands are held back (a run of failures)
    };

    class VisualSync
    {
    public:
        explicit VisualSync(const VisualConfig& cfg = VisualConfig()) : cfg_(cfg) {}

        // A new entity, a hand-over, a new session: nothing is known about the entity any more.
        void Reset()
        {
            seen_ = false;
            lastServer_ = prevServer_ = shown_ = pend_ = VisualMask();
            for (double& t : pendUntil_) t = 0.0;
            lastReadMs_ = -1e9;
            failStreak_ = 0;
            suspendUntil_ = 0.0;
            lastDiv_ = VisualMask();
            divRepeat_ = 0;
        }

        // Whether the engine should read the entity and call Update this frame. `isNear` = the car is close enough to
        // care (the periodic re-check and the shot-time scan are for near copies only); `shotRecent` = the local player
        // shot within the last second or so.
        bool Due(double nowMs, const VisualMask& server, bool iControl, bool isNear, bool shotRecent) const
        {
            if (nowMs < suspendUntil_) return false;
            if (!seen_) return true;
            if (NormalizeVisual(server) != lastServer_) return true;
            if (iControl) return prevServer_ != lastServer_;     // a rising bit whose command failed: try again
            if (!isNear) return false;
            if (pend_.Any() && nowMs - lastReadMs_ >= cfg_.readShotMs) return true;     // a report is waiting for its answer
            if (shotRecent && nowMs - lastReadMs_ >= cfg_.readShotMs) return true;
            return nowMs - lastReadMs_ >= cfg_.readCopyMs;
        }

        VisualOut Update(const VisualIn& in)
        {
            VisualOut out;
            const VisualMask server = NormalizeVisual(in.server);
            const double now = in.nowMs;
            const bool first = !seen_;
            seen_ = true;
            lastReadMs_ = now;
            const VisualMask before = lastServer_;           // what the server showed at the previous call
            lastServer_ = server;
            const bool anyRead = in.readTyres || in.readWindows;
            if (anyRead) ++stats_.reads;
            // ---- the pending reports: confirmed by the server, or given up (a given-up bit is put right below, not reported again) ----
            const VisualMask given = ExpirePending(server, now);
            const VisualMask cur = Current(in);
            out.suspended = now < suspendUntil_;
            if (in.iControl)
            {
                // The rising edge of the server's bits (all of them on the first sight): burst what the server added.
                const VisualMask rising = first ? server : VisMinus(server, prevServer_);
                prevServer_ = server;
                if (!out.suspended)
                {
                    out.cmd.rim = (uint8_t)(rising.rim & ~cur.rim);
                    out.cmd.burst = (uint8_t)(rising.tyres & ~server.rim & ~cur.tyres);
                    out.cmd.smash = (uint8_t)(rising.windows & ~cur.windows);
                }
                // A bit the server showed that the entity already shows needs no command; remember nothing else.
                return out;
            }
            // ---- a copy: first the new local pops (a real readback only): not the server's, not already pending, not one just given up.
            // A bit the server showed last time and has cleared now is a repair, not a pop of ours (it is fixed below). A slot that is
            // burst and already pending as a partial burst is a new fact only when it is now off the rim.
            // The first sight of an entity (a new car, a hand-over to us) adopts the server's state: nothing on it is a pop of ours.
            if (anyRead && !out.suspended && !first)
            {
                VisualMask fresh;
                if (in.readTyres)
                {
                    fresh.rim = (uint8_t)(cur.rim & ~server.rim & ~pend_.rim & ~given.rim & ~given.tyres & ~before.rim);
                    fresh.tyres = (uint8_t)(((cur.tyres & ~server.tyres & ~pend_.tyres & ~given.tyres & ~before.tyres) | fresh.rim));
                }
                if (in.readWindows)
                    fresh.windows = (uint8_t)(cur.windows & ~server.windows & ~pend_.windows & ~given.windows & ~before.windows);
                if (fresh.Any())
                {
                    out.report = fresh;
                    pend_ = VisOr(pend_, fresh);
                    SetPendingUntil(fresh, now + cfg_.pendingMs);
                    stats_.reported += (uint32_t)VisBits(fresh);
                }
            }
            // ---- then the level apply of (server + our own pending pops) ----
            prevServer_ = server;
            const VisualMask want = NormalizeVisual(VisOr(server, pend_));
            if (anyRead)
                NoteDivergence(want, cur, in.readTyres, in.readWindows);
            if (!out.suspended)
            {
                out.cmd.rim = (uint8_t)(want.rim & ~cur.rim);
                out.cmd.burst = (uint8_t)(want.tyres & ~want.rim & ~cur.tyres);
                out.cmd.fixTyre = (uint8_t)(cur.tyres & ~want.tyres);
                out.cmd.smash = (uint8_t)(want.windows & ~cur.windows);
                out.cmd.fixWindow = (uint8_t)(cur.windows & ~want.windows);
            }
            return out;
        }

        // The engine's answer after it called the natives for `issued`: `done` = the part that could be called.
        void Commit(const VisualCmd& issued, const VisualCmd& done, double nowMs)
        {
            shown_.tyres = (uint8_t)((shown_.tyres | done.burst | done.rim) & ~done.fixTyre);
            shown_.rim = (uint8_t)((shown_.rim | done.rim) & ~done.fixTyre);
            shown_.windows = (uint8_t)((shown_.windows | done.smash) & ~done.fixWindow);
            stats_.burstsApplied += (uint32_t)PopCount8(done.burst);
            stats_.rimsApplied += (uint32_t)PopCount8(done.rim);
            stats_.tyresFixed += (uint32_t)PopCount8(done.fixTyre);
            stats_.windowsSmashed += (uint32_t)PopCount8(done.smash);
            stats_.windowsFixed += (uint32_t)PopCount8(done.fixWindow);
            // The controller edge: a rising bit whose command failed rises again on the next call.
            prevServer_.tyres = (uint8_t)(prevServer_.tyres & ~((issued.burst & ~done.burst) | (issued.rim & ~done.rim)));
            prevServer_.rim = (uint8_t)(prevServer_.rim & ~(issued.rim & ~done.rim));
            prevServer_.windows = (uint8_t)(prevServer_.windows & ~(issued.smash & ~done.smash));
            if (!issued.Any()) return;
            if (done.Any())
            {
                failStreak_ = 0;
                return;
            }
            ++stats_.cmdFailures;
            if (++failStreak_ >= cfg_.failLimit)
            {
                failStreak_ = 0;
                suspendUntil_ = nowMs + cfg_.failBackoffMs;
                ++stats_.suspends;
            }
        }

        // The controller's report for VehicleUpdate: the readback of each group that could be read, else what was applied.
        VisualMask Reported(bool readTyres, bool readWindows, const VisualMask& local) const
        {
            VisualMask m = shown_;
            if (readTyres) { m.tyres = local.tyres; m.rim = local.rim; }
            if (readWindows) m.windows = local.windows;
            return NormalizeVisual(m);
        }

        const VisualMask& Shown() const { return shown_; }
        const VisualMask& Pending() const { return pend_; }
        const VisualStats& Stats() const { return stats_; }

    private:
        static int Idx(int group, int slot) { return group * kVisSlots + slot; }

        void SetPendingUntil(const VisualMask& m, double until)
        {
            for (int s = 0; s < kVisSlots; ++s)
            {
                if (m.tyres & (1u << s)) pendUntil_[Idx(0, s)] = until;
                if (m.rim & (1u << s)) pendUntil_[Idx(1, s)] = until;
                if (m.windows & (1u << s)) pendUntil_[Idx(2, s)] = until;
            }
        }

        // A pending bit the server now shows is confirmed; one past its deadline is dropped (and may be reported again).
        VisualMask ExpirePending(const VisualMask& server, double now)
        {
            VisualMask given;
            for (int s = 0; s < kVisSlots; ++s)
            {
                const uint8_t b = (uint8_t)(1u << s);
                auto settle = [&](uint8_t& pend, uint8_t srv, uint8_t& gave, int group) {
                    if (!(pend & b)) return;
                    if (srv & b)
                    {
                        pend = (uint8_t)(pend & ~b);
                        ++stats_.confirmed;
                    }
                    else if (now >= pendUntil_[Idx(group, s)])
                    {
                        pend = (uint8_t)(pend & ~b);
                        gave = (uint8_t)(gave | b);
                        ++stats_.expired;
                    }
                };
                settle(pend_.tyres, server.tyres, given.tyres, 0);
                settle(pend_.rim, server.rim, given.rim, 1);
                settle(pend_.windows, server.windows, given.windows, 2);
            }
            pend_.tyres = (uint8_t)(pend_.tyres | pend_.rim);
            return given;
        }

        // Readback vs what the copy should show: a difference is "divergent"; the same difference on 3 reads in a row
        // (commands issued in between) is "stuck": the native is called but does nothing.
        // What the entity shows: each group from the readback when it was read, else from what we applied.
        VisualMask Current(const VisualIn& in) const
        {
            VisualMask m = shown_;
            if (in.readTyres) { m.tyres = in.local.tyres; m.rim = in.local.rim; }
            if (in.readWindows) m.windows = in.local.windows;
            return NormalizeVisual(m);
        }

        void NoteDivergence(const VisualMask& want, const VisualMask& cur, bool tyres, bool windows)
        {
            const VisualMask diff{ (uint8_t)(tyres ? want.tyres ^ cur.tyres : 0), (uint8_t)(tyres ? want.rim ^ cur.rim : 0),
                                   (uint8_t)(windows ? want.windows ^ cur.windows : 0) };
            if (!diff.Any())
            {
                lastDiv_ = VisualMask();
                divRepeat_ = 0;
                return;
            }
            ++stats_.divergent;
            if (diff == lastDiv_)
            {
                if (++divRepeat_ == 2) ++stats_.stuck;
            }
            else
            {
                lastDiv_ = diff;
                divRepeat_ = 0;
            }
        }

        VisualConfig cfg_;
        bool seen_ = false;
        VisualMask lastServer_, prevServer_, shown_, pend_, lastDiv_;
        double pendUntil_[3 * kVisSlots] = {};
        double lastReadMs_ = -1e9;
        int failStreak_ = 0;
        double suspendUntil_ = 0.0;
        int divRepeat_ = 0;
        VisualStats stats_;
    };

    // ---- the orchestration the engine runs per car (a template over the game, so a test drives the SAME code) ------------
    //
    // `Game` is one entity: bool TyreBurst(int slot, bool completely, bool& burst), bool WindowIntact(int slot, bool& intact)
    // (reads: false = the native could not be called), bool BurstTyre(int slot, bool onRim), bool FixTyre(int slot),
    // bool SmashWindow(int slot), bool FixWindow(int slot) (writes: false = not done).

    // Reads the entity's tyres and windows. A group whose first read fails is left alone (its native is missing: the other
    // slots would fail the same way) and reported as not read.
    template <class Game>
    inline void ReadVisual(Game& g, VisualMask& m, bool& tyresOk, bool& windowsOk)
    {
        m = VisualMask();
        tyresOk = windowsOk = false;
        for (int s = 0; s < kVisSlots; ++s)
        {
            bool burst = false;
            if (!g.TyreBurst(s, false, burst))
            {
                if (s == 0) { tyresOk = false; m.tyres = m.rim = 0; break; }
                continue;
            }
            tyresOk = true;
            if (!burst) continue;
            m.tyres = (uint8_t)(m.tyres | (1u << s));
            bool rim = false;
            if (g.TyreBurst(s, true, rim) && rim) m.rim = (uint8_t)(m.rim | (1u << s));
        }
        for (int s = 0; s < kVisSlots; ++s)
        {
            bool intact = true;
            if (!g.WindowIntact(s, intact))
            {
                if (s == 0) { windowsOk = false; m.windows = 0; break; }
                continue;
            }
            windowsOk = true;
            if (!intact) m.windows = (uint8_t)(m.windows | (1u << s));
        }
        m = NormalizeVisual(m);
    }

    // Calls the natives for `cmd`; returns the part that worked.
    template <class Game>
    inline VisualCmd ExecuteVisual(Game& g, const VisualCmd& cmd)
    {
        VisualCmd done;
        for (int s = 0; s < kVisSlots; ++s)
        {
            const uint8_t b = (uint8_t)(1u << s);
            if ((cmd.rim & b) && g.BurstTyre(s, true)) done.rim |= b;
            if ((cmd.burst & b) && g.BurstTyre(s, false)) done.burst |= b;
            if ((cmd.fixTyre & b) && g.FixTyre(s)) done.fixTyre |= b;
            if ((cmd.smash & b) && g.SmashWindow(s)) done.smash |= b;
            if ((cmd.fixWindow & b) && g.FixWindow(s)) done.fixWindow |= b;
        }
        return done;
    }

    // One car, one frame: when due, read, decide, call the natives, tell the module what worked. `ran` says whether it was due.
    template <class Game>
    inline VisualOut StepVisual(VisualSync& vs, Game& g, const VisualMask& server, bool iControl, bool isNear, bool shotRecent,
                                double nowMs, bool* ran = nullptr)
    {
        if (ran) *ran = false;
        if (!vs.Due(nowMs, server, iControl, isNear, shotRecent))
            return VisualOut();
        if (ran) *ran = true;
        VisualIn in;
        in.server = server;
        in.iControl = iControl;
        in.nowMs = nowMs;
        ReadVisual(g, in.local, in.readTyres, in.readWindows);
        const VisualOut out = vs.Update(in);
        const VisualCmd done = out.cmd.Any() ? ExecuteVisual(g, out.cmd) : VisualCmd();
        vs.Commit(out.cmd, done, nowMs);
        return out;
    }

    // The controller's report: what the entity shows now (per group, the readback when it could be read).
    template <class Game>
    inline VisualMask ReadReport(const VisualSync& vs, Game& g)
    {
        VisualMask m;
        bool t = false, w = false;
        ReadVisual(g, m, t, w);
        return vs.Reported(t, w, m);
    }

    // ---- the pops the shooter's copy reports as vehicle Hits ---------------------------------------------------

    // The Hits (bone codes, protocol.h VehiclePartCode) for a report: one per rim slot (the tyre is then off its rim),
    // one per partial burst, one per window; at most `cap`. Returns the number written.
    inline int ReportToParts(const VisualMask& report, int16_t* out, int cap)
    {
        int n = 0;
        for (int s = 0; s < kVisSlots && n < cap; ++s)
        {
            const uint8_t b = (uint8_t)(1u << s);
            if (report.rim & b)
                out[n++] = proto::VehiclePartCode(proto::VehiclePart::TyreRim, (unsigned)s);
            else if (report.tyres & b)
                out[n++] = proto::VehiclePartCode(proto::VehiclePart::Tyre, (unsigned)s);
        }
        for (int s = 0; s < kVisSlots && n < cap; ++s)
            if (report.windows & (1u << s))
                out[n++] = proto::VehiclePartCode(proto::VehiclePart::Window, (unsigned)s);
        return n;
    }

    // ---- sender: steering ----------------------------------------------------------------------------------

    // The yaw rate (rad/s about the car's own up axis, + = counter-clockwise = a left turn) from two successive
    // rotations dtSec apart. Unit-safe: it does not depend on what GET_ENTITY_ROTATION_VELOCITY returns (its unit and
    // frame are unverified, vehicle_drive.h RISK). False for a gap outside 5..300 ms or non-finite input.
    inline bool YawRateFromRotations(const DriveQuat& prev, const DriveQuat& cur, double dtSec, float& yawRate)
    {
        if (!(dtSec >= 0.005 && dtSec <= 0.3) || !vq::Finite(prev) || !vq::Finite(cur)) return false;
        const DriveQuat p = vq::Normalize(prev), c = vq::Normalize(cur);
        const DriveVec rv = vq::Log(vq::Mul(c, vq::Conj(p)));           // the world-frame rotation from p to c
        const DriveVec up = vq::Rotate(c, DriveVec{ 0.0f, 0.0f, 1.0f });
        const double w = ((double)rv.x * up.x + (double)rv.y * up.y + (double)rv.z * up.z) / dtSec;
        if (!std::isfinite(w)) return false;
        yawRate = (float)w;
        return true;
    }

    // Which source a steering value came from (diagnostics).
    enum class SteerSource : uint8_t { Held = 0, Estimate = 1, Axis = 2 };
    struct SteerChoice { float steer = 0.0f; SteerSource source = SteerSource::Held; };

    // The wire steering of the car the local player DRIVES: the kinematic estimate while it moves (EstimateSteer),
    // the player's steering axis (control INPUT_VEH_MOVE_LR, -1 left .. +1 right = the wire's sign) below
    // holdBelowMps, where the estimate is noise and holds its last value: a standing car shows its wheels turned.
    // `axisOk` false (the control native is unavailable, or the car is not driven by the local player) keeps the old
    // behaviour exactly.
    inline SteerChoice ChooseSteer(float vFwd, float yawRate, float wheelbase, float prev, bool axisOk, float axis,
                                   float holdBelowMps = 1.0f)
    {
        SteerChoice c;
        const bool slow = !std::isfinite(vFwd) || std::fabs(vFwd) < holdBelowMps;
        if (slow && axisOk && std::isfinite(axis))
        {
            c.steer = std::max(-1.0f, std::min(1.0f, axis));
            c.source = SteerSource::Axis;
            return c;
        }
        c.steer = EstimateSteer(vFwd, yawRate, wheelbase, prev);
        c.source = slow ? SteerSource::Held : SteerSource::Estimate;
        return c;
    }

    // Running tally of the steering the sender wrote, and of whether the axis agrees with the estimate while the car
    // really moves (a flipped axis sign would show as `disagree` > `agree`): the live check of the INFERRED control id /
    // sign. min / mean / max of the value that went out.
    struct SteerStats
    {
        uint32_t n = 0, axis = 0, held = 0, estimate = 0, agree = 0, disagree = 0;
        float min = 0.0f, max = 0.0f;
        double sum = 0.0;
        void Add(const SteerChoice& c, bool axisOk, float axisValue, float estimateValue, float vFwd)
        {
            if (n == 0) { min = max = c.steer; }
            min = std::min(min, c.steer);
            max = std::max(max, c.steer);
            sum += c.steer;
            ++n;
            if (c.source == SteerSource::Axis) ++axis;
            else if (c.source == SteerSource::Held) ++held;
            else ++estimate;
            if (axisOk && std::isfinite(axisValue) && std::isfinite(vFwd) && std::fabs(vFwd) >= 3.0f &&
                std::fabs(estimateValue) >= 0.2f && std::fabs(axisValue) >= 0.2f)
            {
                if ((axisValue > 0.0f) == (estimateValue > 0.0f)) ++agree;
                else ++disagree;
            }
        }
        double Mean() const { return n ? sum / n : 0.0; }
        void Reset() { *this = SteerStats(); }
    };

    // ---- sender: brake lights and handbrake ---------------------------------------------------------------------

    // The driver's pedals (INPUT_VEH_ACCELERATE / BRAKE / HANDBRAKE, 0..1) and the car's speed along its forward axis
    // (m/s, + forward) as the kVeh2Brake / kVeh2Handbrake bits: the handbrake; the foot brake while rolling forward;
    // the accelerator while rolling backward (that is braking too). Below 0.5 m/s the pedals are throttle / reverse,
    // not braking. `inputOk` false: both bits clear.
    struct BrakeState { bool brakeLights = false, handbrake = false; };
    inline BrakeState ClassifyBrake(bool inputOk, float accel, float brake, float handbrake, float vFwd)
    {
        BrakeState b;
        if (!inputOk || !std::isfinite(accel) || !std::isfinite(brake) || !std::isfinite(handbrake) || !std::isfinite(vFwd)) return b;
        b.handbrake = handbrake > 0.5f;
        b.brakeLights = b.handbrake || (brake > 0.1f && vFwd > 0.5f) || (accel > 0.1f && vFwd < -0.5f);
        return b;
    }

}
