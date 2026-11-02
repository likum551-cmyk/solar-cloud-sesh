// On-foot Tier B (sync plan W6, protocol v5 PedState): the decisions around the extra PedState flags and the
// entering / exiting moves. Pure C++17, header-only, no game calls, single-threaded (the engine's script tick).
//
//   RECEIVER  PedStateEdges, one per remote ped: Update(flags, move, vehId) each time the ped's state is applied
//             returns the EDGES the engine acts on once (a reload starts, stealth flips, cover is entered or left,
//             an enter / exit of a vehicle begins). Level flags (swimming) are mirrored.
//   SENDER    DecideVehicleMove: whether the local ped's PedState carries move 5 (entering) or 6 (exiting) and
//             which networked car it names, from what the engine can see (GET_VEHICLE_PED_IS_TRYING_TO_ENTER
//             mapped to the server id, the F / G entry walk, the link's last Exit).
//   EnterWarpDue  when the old behaviour (put the ped straight into the seat) takes over from the enter task.
#pragma once

#include "protocol/protocol.h"

#include <cstdint>

namespace gage::sync {

    struct PedEdgeIn
    {
        uint16_t flags = 0;   // PedState.flags (kPf*)
        uint8_t  move = 0;    // PedState.move (kMove*)
        uint8_t  vehId = 0;   // PedState.vehId (only meaningful for kMoveEnteringVehicle / kMoveExitingVehicle)
    };

    struct PedEdges
    {
        bool    reloadStart = false;    // kPfReloading rose: TASK_RELOAD_WEAPON once
        bool    stealthChanged = false; // kPfStealth flipped: SET_PED_STEALTH_MOVEMENT(stealthOn)
        bool    stealthOn = false;
        bool    coverEnter = false;     // kPfCover rose
        bool    coverExit = false;      // kPfCover fell
        bool    enterStart = false;     // move became kMoveEnteringVehicle, or the car it names changed: TASK_ENTER_VEHICLE(enterVeh)
        uint8_t enterVeh = 0;
        bool    exitStart = false;      // move became kMoveExitingVehicle (or its car changed): TASK_LEAVE_VEHICLE
        uint8_t exitVeh = 0;
        bool    swimming = false;       // level: kPfSwimming
    };

    class PedStateEdges
    {
    public:
        PedEdges Update(const PedEdgeIn& in)
        {
            PedEdges e;
            const bool reloading = (in.flags & proto::kPfReloading) != 0;
            const bool stealth = (in.flags & proto::kPfStealth) != 0;
            const bool cover = (in.flags & proto::kPfCover) != 0;
            e.reloadStart = reloading && !reloading_;
            e.stealthChanged = stealth != stealth_;
            e.stealthOn = stealth;
            e.coverEnter = cover && !cover_;
            e.coverExit = !cover && cover_;
            const bool entering = in.move == proto::kMoveEnteringVehicle;
            const bool exiting = in.move == proto::kMoveExitingVehicle;
            if (entering && (move_ != proto::kMoveEnteringVehicle || vehId_ != in.vehId))
            {
                e.enterStart = true;
                e.enterVeh = in.vehId;
            }
            if (exiting && (move_ != proto::kMoveExitingVehicle || vehId_ != in.vehId))
            {
                e.exitStart = true;
                e.exitVeh = in.vehId;
            }
            e.swimming = (in.flags & proto::kPfSwimming) != 0;
            reloading_ = reloading;
            stealth_ = stealth;
            cover_ = cover;
            move_ = in.move;
            vehId_ = (entering || exiting) ? in.vehId : 0;
            return e;
        }
        // Forget the previous state (a new ped, or the ped was re-tasked by someone else).
        void Reset() { *this = PedStateEdges(); }

    private:
        bool    reloading_ = false, stealth_ = false, cover_ = false;
        uint8_t move_ = proto::kMoveIdle;
        uint8_t vehId_ = 0;
    };

    // ---- sender ----

    struct VehicleMoveIn
    {
        bool   seated = false;       // the ped is in a vehicle: the ordinary in-vehicle PedState (move 4) applies
        int    tryingNetId = -1;     // the networked car GET_VEHICLE_PED_IS_TRYING_TO_ENTER names, -1 = none / not networked
        int    walkNetId = -1;       // the car an F / G entry walk is heading for, -1 = none
        int    exitNetId = -1;       // the networked car the link just sent Exit for, -1 = none
        double sinceExitMs = 1e9;    // time since that Exit
    };

    struct VehicleMoveOut
    {
        bool    use = false;         // false: the ordinary move (idle / walk / run / sprint / in vehicle) stands
        uint8_t move = proto::kMoveIdle;
        uint8_t vehId = 0;
    };

    constexpr double kExitMoveHoldMs = 1500.0;   // move 6 is sent this long after the link's Exit

    // Entering wins over the walk to the door, which wins over exiting; a seated ped sends none of them; an id
    // outside 0..kMaxVehicles-1 (not a networked car) is ignored.
    inline VehicleMoveOut DecideVehicleMove(const VehicleMoveIn& in, double exitHoldMs = kExitMoveHoldMs)
    {
        VehicleMoveOut o;
        if (in.seated)
            return o;
        auto valid = [](int id) { return id >= 0 && id < (int)proto::kMaxVehicles; };
        if (valid(in.tryingNetId))
        {
            o.use = true;
            o.move = proto::kMoveEnteringVehicle;
            o.vehId = (uint8_t)in.tryingNetId;
        }
        else if (valid(in.walkNetId))
        {
            o.use = true;
            o.move = proto::kMoveEnteringVehicle;
            o.vehId = (uint8_t)in.walkNetId;
        }
        else if (valid(in.exitNetId) && in.sinceExitMs >= 0.0 && in.sinceExitMs < exitHoldMs)
        {
            o.use = true;
            o.move = proto::kMoveExitingVehicle;
            o.vehId = (uint8_t)in.exitNetId;
        }
        return o;
    }

    // ---- receiver: the enter task's fallback ----

    constexpr double kEnterTaskMaxMs = 2500.0;   // TASK_ENTER_VEHICLE gets this long before the seat warp takes over

    // The server says the peer sits in the car (move >= in-vehicle) but its local copy does not yet. With no enter
    // task issued (sinceEnterIssuedMs < 0: the car was not here to walk to, or the entering move was missed) the
    // warp is due at once, as before W6; with one, once it has had kEnterTaskMaxMs.
    inline bool EnterWarpDue(bool serverSeated, bool locallySeated, double sinceEnterIssuedMs, double maxMs = kEnterTaskMaxMs)
    {
        if (!serverSeated || locallySeated)
            return false;
        return sinceEnterIssuedMs < 0.0 || sinceEnterIssuedMs >= maxMs;
    }

    // ====================================================================================================
    // Live test 0.4.2 fixes, W8 (docs/sync-specs/live-0.4.2-fixes.md items 4, 5 and 8)
    // ====================================================================================================

    // ---- receiver: entering and leaving water (item 4) ----

    // The edge of the sender's kPfSwimming. The first Update is a baseline (no edge: a peer first seen swimming has
    // nothing to transition from). The engine does a SoftReset of the locomotion controller on either edge, so the
    // gait task is re-evaluated the moment the ped's medium changes and kept if it still serves.
    class WaterEdge
    {
    public:
        struct Out { bool entered = false, left = false; };
        Out Update(bool swimming)
        {
            Out o;
            if (have_)
            {
                o.entered = swimming && !in_;
                o.left = !swimming && in_;
            }
            have_ = true;
            in_ = swimming;
            return o;
        }
        bool InWater() const { return in_; }
        void Reset() { *this = WaterEdge(); }

    private:
        bool have_ = false, in_ = false;
    };

    // ---- receiver: vault / climb (item 5) ----

    // The sender's kPfClimbing / kPfVaulting (not while it ragdolls) as a window around ONE TASK_CLIMB:
    //   rising edge        -> issueClimb (once), phase Pending: the engine suspends position correction and the
    //                         velocity push (suspend) so nothing fights the climb;
    //   puppet climbing    -> phase Active (the engine passes IS_PED_CLIMBING / IS_PED_VAULTING of the puppet);
    //   not climbing within startTimeoutMs of the edge (or the task could not be issued: NoteIssueFailed)
    //                      -> phase Fallback: fallbackPlace, the old behaviour (collision off, ped placed on the pose)
    //                         until the flag falls;
    //   flag falls         -> phase Settle for settleMs (still suspended), then reset = true once: the climb task
    //                         replaced the gait task, so the controller must re-issue it (Reset(), not SoftReset()).
    // A flag that rises again during the settle is the same climb (no second TASK_CLIMB).
    class ClimbWindow
    {
    public:
        struct Out
        {
            bool issueClimb = false;     // rising edge: TASK_CLIMB once
            bool suspend = false;        // no position correction, no velocity push, no gait task this frame
            bool fallbackPlace = false;  // the task did not start: place the ped on the pose with collision off
            bool reset = false;          // the window just ended: the controller re-issues its gait task
        };
        enum class Phase : uint8_t { Idle, Pending, Active, Fallback, Settle };

        explicit ClimbWindow(double startTimeoutMs = 300.0, double settleMs = 250.0)
            : startTimeoutMs_(startTimeoutMs), settleMs_(settleMs) {}

        Out Update(bool flag, bool pedClimbing, double nowMs)
        {
            Out o;
            switch (phase_)
            {
            case Phase::Idle:
                if (flag)
                {
                    phase_ = Phase::Pending;
                    startMs_ = nowMs;
                    o.issueClimb = true;
                    o.suspend = true;
                }
                break;
            case Phase::Pending:
            case Phase::Active:
                if (!flag)
                {
                    phase_ = Phase::Settle;
                    settleUntilMs_ = nowMs + settleMs_;
                    o.suspend = true;
                    break;
                }
                if (pedClimbing)
                    phase_ = Phase::Active;
                else if (phase_ == Phase::Pending && nowMs - startMs_ >= startTimeoutMs_)
                    phase_ = Phase::Fallback;
                o.suspend = true;
                o.fallbackPlace = phase_ == Phase::Fallback;
                break;
            case Phase::Fallback:
                if (!flag)
                {
                    phase_ = Phase::Settle;
                    settleUntilMs_ = nowMs + settleMs_;
                    o.suspend = true;
                    break;
                }
                o.suspend = true;
                o.fallbackPlace = true;
                break;
            case Phase::Settle:
                if (flag)
                {
                    phase_ = pedClimbing ? Phase::Active : Phase::Pending;   // the same climb, flickering flag
                    if (phase_ == Phase::Pending)
                        startMs_ = nowMs;
                    o.suspend = true;
                    break;
                }
                if (nowMs >= settleUntilMs_)
                {
                    phase_ = Phase::Idle;
                    o.reset = true;
                    break;
                }
                o.suspend = true;
                break;
            }
            return o;
        }
        // TASK_CLIMB could not be called: go straight to the placement fallback.
        void NoteIssueFailed()
        {
            if (phase_ == Phase::Pending)
                phase_ = Phase::Fallback;
        }
        Phase CurrentPhase() const { return phase_; }
        bool Busy() const { return phase_ != Phase::Idle; }
        void Reset() { phase_ = Phase::Idle; }

    private:
        double startTimeoutMs_, settleMs_;
        Phase phase_ = Phase::Idle;
        double startMs_ = 0, settleUntilMs_ = 0;
    };

    // ---- receiver: when a puppet aims (item 8) ----

    // The aim task is wanted while the sender's kPfAiming is set, it is not ragdolling and it holds a weapon: weapon 0
    // is "none / not reported" (the alt-tab fallback used to send exactly that), and an aim task without a weapon
    // would be stale the moment the weapon is taken away.
    inline bool AimWanted(uint16_t flags, uint32_t weapon)
    {
        return (flags & proto::kPfAiming) != 0 && (flags & proto::kPfRagdoll) == 0 && weapon != 0;
    }

    // ---- sender: the PedState to send when no script tick ran (item 8) ----

    // GTA stops its script tick while the window is unfocused, Present keeps running and the render-thread fallback
    // keeps sending states. It used to send a DEFAULT PedState (weapon 0, idle, no flags): the others saw the player
    // drop the weapon and the vehicle state vanish. FallbackPedState is the LAST REAL state with what cannot still be
    // true after a stop cleared: move idle (unless seated), the transient flags (shooting, jumping, falling, melee,
    // reloading, climbing, vaulting, ladder, aiming) and the aim point. Kept: weapon, shot and jump counters, the
    // vehicle seat (move 4, driver), dead, stealth, cover, swimming, ragdoll, parachute. The caller zeroes the
    // velocity. Without a last real state (none was ever read) the default state.
    inline proto::PedState FallbackPedState(const proto::PedState& last, bool haveLast)
    {
        if (!haveLast)
            return proto::PedState();
        constexpr uint16_t kKeep = proto::kPfDead | proto::kPfDriver | proto::kPfStealth | proto::kPfCover | proto::kPfSwimming |
                                   proto::kPfRagdoll | proto::kPfParachute;
        proto::PedState s = last;
        s.flags = (uint16_t)(last.flags & kKeep);
        s.aimX = s.aimY = s.aimZ = 0.0f;
        if (s.move != proto::kMoveInVehicle)
        {
            s.move = proto::kMoveIdle;
            s.vehId = 0;
            s.flags = (uint16_t)(s.flags & ~proto::kPfDriver);   // a driver is in a vehicle (ValidPedState)
        }
        return s;
    }

    // Per-site tally of locomotion resets, for the 10 s sync line (item 3 (a)). The engine maps its call sites onto
    // the indexes.
    template <size_t N>
    class SiteTally
    {
    public:
        void Add(size_t site)
        {
            if (site < N)
            {
                ++n_[site];
                ++total_;
            }
        }
        uint32_t Count(size_t site) const { return site < N ? n_[site] : 0; }
        uint32_t Total() const { return total_; }
        static constexpr size_t Size() { return N; }

    private:
        uint32_t n_[N] = {};
        uint32_t total_ = 0;
    };

}
