// Live test 0.4.2 fixes, W9 (docs/sync-specs/live-0.4.2-fixes.md section W9): the decisions around passengers, seats,
// leaving a moving vehicle, vehicle impacts and bikes. Pure C++17, header-only, no game calls, single-threaded (the
// engine's script tick). engine.cpp only wires them; every rule is exercised by gameplay_test (TestW9).
//
//   SeatRewarpLimiter   item 2: a ped that GTA keeps moving out of the seat the server gave it (the auto-shuffle to
//                       the driver's seat) was warped back every 100 ms; re-warps to the same target back off.
//   EntryPressGuard     item 3: INPUT_ENTER (control 23) is disabled from our handled F / G press until 500 ms after
//                       the ped sat down, and a quick exit after an entry is counted.
//   EngineReassert      item 4: the engine of the car the local ped sits in as a passenger is re-asserted on every
//                       ~1 s while the server says it runs.
//   DecideUnseat        item 5: how a remote ped leaves a vehicle: a normal exit, or a bail-out (flag 4160) from a
//                       moving one, with the vehicle's velocity carried for the exit window (CarryVelocityScale).
//   LocalLeaveFlags     item 5: the flags of the local ped's own forced exit (a vehicle the server removed).
//   DecideVehicleImpact item 6: a puppet hit by the local player's moving vehicle ragdolls.
//   RollTracker         item 7: the 10 s log line of one remote bike (roll error, speed).
#pragma once

#include "protocol/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gage::sync {

    // ---- item 2: re-warp rate limit ----

    // A WARP of a ped that already sits in a vehicle (here or in another) is a re-warp: the visible "passenger
    // teleports". The first warp into a seat is never limited; a re-warp to the SAME vehicle and seat as the last
    // warp is allowed only after a gap that doubles with every consecutive one (minGapMs, 2x, 4x ... maxGapMs) and
    // starts over after calmMs without one. A re-warp to a DIFFERENT target is a real seat change (the server moved
    // the player) and is allowed at once. With the engine's 100 ms retry a thrashing ped now gets about 5 re-warps
    // in 10 s instead of 100.
    struct SeatRewarpConfig
    {
        double minGapMs = 400.0, maxGapMs = 4000.0, calmMs = 5000.0;
    };

    class SeatRewarpLimiter
    {
    public:
        explicit SeatRewarpLimiter(const SeatRewarpConfig& cfg = SeatRewarpConfig()) : cfg_(cfg) {}

        // True: warp now (and the warp is recorded). False: not yet (counted in Suppressed()).
        bool Allow(double nowMs, int vehId, int seat, bool wasSeated)
        {
            if (!have_ || vehId != veh_ || seat != seat_)
            {
                Record(nowMs, vehId, seat, 0);
                return true;
            }
            if (!wasSeated)
            {
                Record(nowMs, vehId, seat, streak_);   // not seated now: the first warp of a visit, never held back
                return true;
            }
            const double quiet = nowMs - lastMs_;
            if (quiet >= cfg_.calmMs)
            {
                Record(nowMs, vehId, seat, 0);
                return true;
            }
            const double gap = std::min(cfg_.maxGapMs, cfg_.minGapMs * std::pow(2.0, (double)std::min(streak_, 16)));
            if (quiet >= gap)
            {
                Record(nowMs, vehId, seat, streak_ + 1);
                return true;
            }
            ++suppressed_;
            return false;
        }
        uint32_t Suppressed() const { return suppressed_; }
        void Reset() { *this = SeatRewarpLimiter(cfg_); }

    private:
        void Record(double nowMs, int vehId, int seat, int streak)
        {
            have_ = true;
            veh_ = vehId;
            seat_ = seat;
            lastMs_ = nowMs;
            streak_ = streak;
        }
        SeatRewarpConfig cfg_;
        bool have_ = false;
        int veh_ = -1, seat_ = -1, streak_ = 0;
        double lastMs_ = 0;
        uint32_t suppressed_ = 0;
    };

    // ---- item 3: F / G press guard ----

    constexpr double kEntryAfterSeatMs = 500.0;    // control 23 stays disabled this long after the ped sat down
    constexpr double kQuickExitMs = 3000.0;        // an exit this soon after an entry is counted

    class EntryPressGuard
    {
    public:
        void OnPress(double nowMs)                 // our F / G handler took a press (the control was read disabled)
        {
            ++presses_;
            walking_ = true;
            seated_ = false;
            pressMs_ = nowMs;
        }
        void OnSeated(double nowMs)                // we put the ped into the seat (or the walk ended with it seated)
        {
            walking_ = false;
            seated_ = true;
            seatedMs_ = nowMs;
        }
        void OnCancel() { walking_ = false; }      // the walk was dropped without seating
        // INPUT_ENTER (23) is disabled this frame: from the handled press until the ped sat, then 500 ms more.
        bool DisableEnter(double nowMs) const
        {
            return walking_ || (seated_ && nowMs - seatedMs_ >= 0.0 && nowMs - seatedMs_ < kEntryAfterSeatMs);
        }
        // The ped left the vehicle (or the link saw an Exit). True when that is a quick exit after our entry.
        bool NoteExit(double nowMs)
        {
            const bool quick = seated_ && nowMs - seatedMs_ >= 0.0 && nowMs - seatedMs_ < kQuickExitMs;
            if (quick)
                ++quickExits_;
            seated_ = false;
            return quick;
        }
        uint32_t Presses() const { return presses_; }
        uint32_t QuickExits() const { return quickExits_; }
        void Reset() { *this = EntryPressGuard(); }

    private:
        bool walking_ = false, seated_ = false;
        double pressMs_ = 0, seatedMs_ = 0;
        uint32_t presses_ = 0, quickExits_ = 0;
    };

    // ---- item 4: passenger engine ----

    constexpr double kEngineReassertMs = 1000.0;   // while the server says the engine runs
    constexpr double kEngineRetryMs = 250.0;       // ... sooner when the car reports it is not running

    // The local ped sits in a car somebody else drives. The engine flag is applied by ApplyVehicleFields only when the
    // server's flag CHANGES, and the server clears it at 0 fuel and on a driver change, so the passenger could sit in a
    // car whose engine had stopped on this machine. Due() says when SET_VEHICLE_ENGINE_ON(true) is called again.
    class EngineReassert
    {
    public:
        // active: the local ped is a passenger in this car (not the controller). serverOn: the server's kVehEngine.
        // running: GET_IS_VEHICLE_ENGINE_RUNNING (only read when the caller has a fresh reading, else pass true).
        bool Due(double nowMs, bool active, bool serverOn, bool running)
        {
            if (!active || !serverOn)
            {
                started_ = false;
                return false;
            }
            const double gap = running ? kEngineReassertMs : kEngineRetryMs;
            if (!started_ || nowMs - lastMs_ >= gap)
            {
                started_ = true;
                lastMs_ = nowMs;
                ++asserts_;
                return true;
            }
            return false;
        }
        uint32_t Asserts() const { return asserts_; }
        void Reset() { started_ = false; }

    private:
        bool started_ = false;
        double lastMs_ = 0;
        uint32_t asserts_ = 0;
    };

    // ---- item 5: leaving a moving vehicle ----

    constexpr int kLeaveNormal = 0, kLeaveWarpOut = 16, kLeaveBailOut = 4160;   // TASK_LEAVE_VEHICLE flags (public docs)
    constexpr float kBailSpeedMps = 3.0f;          // a vehicle faster than this is bailed out of
    constexpr double kLeavePauseMs = 1500.0;       // the locomotion pause of a normal exit (the animation)
    constexpr double kBailPauseMs = 600.0;         // ... of a bail-out (the ragdoll edge of the sender takes over)
    constexpr double kCarryVelocityMs = 300.0;     // the vehicle's velocity is given to the ped this long after it left

    enum class UnseatMode : uint8_t { Clear, Leave, Bail };

    struct UnseatIn
    {
        bool haveVehicle = false;       // the vehicle is still alive here and the ped is inside it
        float vehSpeedMps = 0.0f;       // GET_ENTITY_SPEED of the vehicle (non-finite reads as 0)
        uint16_t senderFlags = 0;       // the sender's newest PedState.flags (its first on-foot sample)
    };

    struct UnseatPlan
    {
        UnseatMode mode = UnseatMode::Clear;
        int leaveFlags = kLeaveNormal;  // TASK_LEAVE_VEHICLE flags to try first
        int fallbackFlags = -1;         // tried when that call fails (-1: clear the ped's tasks instead)
        bool carryVelocity = false;     // give the ped the vehicle's velocity for kCarryVelocityMs
        double pauseMs = 0.0;           // PedLocomotion::Pause
    };

    // No vehicle (or the ped is not inside it): the old pop (ClearPedTasksImmediately). A vehicle moving faster than
    // kBailSpeedMps, or a sender whose first on-foot sample is ragdolling, falling or jumping (it jumped out): a
    // bail-out (4160; the warp flag 16 when the call fails), the vehicle's velocity carried, and no locomotion pause
    // when the sender is already ragdolling (the controller's ragdoll edge takes the ped at once and holds it until the
    // sender's ragdoll ends, with no position correction over it). Otherwise the normal climb-out (flag 0).
    inline UnseatPlan DecideUnseat(const UnseatIn& in)
    {
        UnseatPlan p;
        if (!in.haveVehicle)
            return p;
        const float speed = std::isfinite(in.vehSpeedMps) && in.vehSpeedMps > 0.0f ? in.vehSpeedMps : 0.0f;
        const uint16_t jumped = (uint16_t)(proto::kPfRagdoll | proto::kPfFalling | proto::kPfJumping);
        if (speed > kBailSpeedMps || (in.senderFlags & jumped) != 0)
        {
            p.mode = UnseatMode::Bail;
            p.leaveFlags = kLeaveBailOut;
            p.fallbackFlags = kLeaveWarpOut;
            p.carryVelocity = true;
            p.pauseMs = (in.senderFlags & proto::kPfRagdoll) ? 0.0 : kBailPauseMs;
            return p;
        }
        p.mode = UnseatMode::Leave;
        p.leaveFlags = kLeaveNormal;
        p.pauseMs = kLeavePauseMs;
        return p;
    }

    // The share of the vehicle's velocity the ped still gets `sinceMs` after it left: 1 at the start, linear to 0 at
    // kCarryVelocityMs (and 0 before the start or for non-finite input).
    inline float CarryVelocityScale(double sinceMs)
    {
        if (!std::isfinite(sinceMs) || sinceMs < 0.0 || sinceMs >= kCarryVelocityMs)
            return 0.0f;
        return (float)(1.0 - sinceMs / kCarryVelocityMs);
    }

    // The local ped's own forced exit (the server removed the vehicle with us inside): a bail-out from a moving one on
    // the first try, the warp out otherwise and on every retry (a bail-out the game refused must not keep the ped in a
    // car that is about to be deleted). F in a vehicle is GTA's own INPUT_VEH_EXIT (75), untouched.
    inline int LocalLeaveFlags(float vehSpeedMps, bool retry)
    {
        if (!retry && std::isfinite(vehSpeedMps) && vehSpeedMps > kBailSpeedMps)
            return kLeaveBailOut;
        return kLeaveWarpOut;
    }

    // ---- item 6: a puppet hit by the local player's vehicle ----

    constexpr float kImpactMinSpeedMps = 5.0f;     // the local vehicle must be at least this fast
    constexpr float kImpactReachM = 6.0f;          // ... and this close (origin to ped) for the damage query to be asked
    constexpr double kImpactRepeatMs = 1500.0;     // one impact per puppet in this time

    struct ImpactIn
    {
        float vehSpeedMps = 0.0f;       // the local player's vehicle
        bool damagedByVehicle = false;  // HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(puppet, our vehicle)
        bool puppetSeated = false;      // a seated puppet rides along
        bool puppetDead = false;
        double sinceLastImpactMs = 1e9; // since this puppet's last shown impact
    };

    struct ImpactOut
    {
        bool ragdoll = false;
        int ragdollMs = 0;              // how long the puppet ragdolls (the position correction is suspended that long)
        float velocityScale = 0.0f;     // the share of the vehicle's velocity the puppet is thrown with
    };

    inline ImpactOut DecideVehicleImpact(const ImpactIn& in)
    {
        ImpactOut o;
        if (!in.damagedByVehicle || in.puppetSeated || in.puppetDead || in.sinceLastImpactMs < kImpactRepeatMs)
            return o;
        if (!std::isfinite(in.vehSpeedMps) || in.vehSpeedMps < kImpactMinSpeedMps)
            return o;
        o.ragdoll = true;
        o.ragdollMs = (int)std::min(2500.0, std::max(800.0, 600.0 + 80.0 * (double)in.vehSpeedMps));
        o.velocityScale = 0.5f;
        return o;
    }

    // ---- item 7: one remote bike's 10 s line ----

    // Per frame the roll error (|local roll - target roll|, degrees) and the speed; Take() returns the window and clears it.
    class RollTracker
    {
    public:
        struct Window
        {
            uint32_t frames = 0;
            float maxErrDeg = 0.0f, meanErrDeg = 0.0f, maxSpeedMs = 0.0f;
        };
        void Add(double rollErrDeg, double speedMs)
        {
            if (!std::isfinite(rollErrDeg) || !std::isfinite(speedMs))
                return;
            const double e = std::fabs(rollErrDeg);
            ++frames_;
            sum_ += e;
            maxErr_ = std::max(maxErr_, e);
            maxSpeed_ = std::max(maxSpeed_, std::fabs(speedMs));
        }
        Window Take()
        {
            Window w;
            w.frames = frames_;
            w.maxErrDeg = (float)maxErr_;
            w.meanErrDeg = frames_ ? (float)(sum_ / frames_) : 0.0f;
            w.maxSpeedMs = (float)maxSpeed_;
            frames_ = 0;
            sum_ = maxErr_ = maxSpeed_ = 0.0;
            return w;
        }

    private:
        uint32_t frames_ = 0;
        double sum_ = 0.0, maxErr_ = 0.0, maxSpeed_ = 0.0;
    };

    // GET_VEHICLE_CLASS: 8 = motorcycles, 13 = cycles.
    inline bool IsBikeClass(int vehicleClass) { return vehicleClass == 8 || vehicleClass == 13; }

}   // namespace gage::sync
