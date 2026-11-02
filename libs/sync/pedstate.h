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

    // The server lists the peer's seat in the car but its local copy is not inside yet. The warp (put the ped
    // straight into the seat) is due:
    //   * at once when no enter task runs (sinceEnterIssuedMs < 0: the car was not here to walk to, or the entering
    //     move was missed), as before W6;
    //   * W9 item 1, at once when the sender's latest move is no longer kMoveEnteringVehicle (senderStillEntering
    //     false): the real passenger was put into the seat by F (a warp, by design) in 0.3 s, so the door walk
    //     on the other screen has nothing left to show. Before this the warp waited out kEnterTaskMaxMs (2.5 s) of
    //     door walk while the real player already sat, the "passenger teleports";
    //   * else once the walk has had kEnterTaskMaxMs (the sender is still walking in: a slow entry).
    // senderStillEntering defaults to true (the pre-W9 rule) for callers that do not know the sender's move.
    inline bool EnterWarpDue(bool serverSeated, bool locallySeated, double sinceEnterIssuedMs, bool senderStillEntering = true,
                             double maxMs = kEnterTaskMaxMs)
    {
        if (!serverSeated || locallySeated)
            return false;
        if (sinceEnterIssuedMs < 0.0 || !senderStillEntering)
            return true;
        return sinceEnterIssuedMs >= maxMs;
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

    // ---- receiver: 0d, the pose a vault / climb / cover task starts from ----

    // TASK_CLIMB probes the obstacle in front of the ped it is given to, and TASK_PUT_PED_DIRECTLY_INTO_COVER looks for a cover
    // point at the coordinates it is given. Both are issued on the sender's rising edge, when the puppet is wherever its gait
    // task and the ~100 ms render delay left it: 5 of 8 climbs of the 0.4.3 live test never started (300 ms timeout, the puppet
    // was placed with its collision off, no animation). So the puppet is first put on the pose of the edge (position and
    // heading), the task goes out the same frame.
    //   Keep   close enough (< kPrePlaceKeepM): the position is left, the heading is still corrected.
    //   Blend  under kPrePlaceSetM off: the position is nudged onto the pose with the puppet's tasks and momentum kept (no
    //          visible restart; a time blend cannot wait for the task, which must go out the same frame).
    //   Set    kPrePlaceSetM or more off, or the ped's position is unreadable: placed outright and its tasks cleared first.
    constexpr float kPrePlaceKeepM = 0.12f;
    constexpr float kPrePlaceSetM = 1.0f;
    constexpr float kPrePlaceHeadingDeg = 8.0f;
    enum class PrePlaceMode : uint8_t { Keep, Blend, Set };
    struct PrePlace
    {
        PrePlaceMode mode = PrePlaceMode::Keep;
        bool setHeading = false;    // SET_ENTITY_HEADING to the pose's heading
        bool clearTasks = false;    // the old task (a go-to-entity on the anchor, ...) is dropped before the placement
    };
    // distM: horizontal-plus-vertical distance of the ped from the pose (< 0 = not readable). headingErrDeg: |ped heading - pose
    // heading| folded to 0..180.
    inline PrePlace DecidePrePlace(float distM, float headingErrDeg)
    {
        PrePlace p;
        if (distM < 0.0f || distM >= kPrePlaceSetM)
        {
            p.mode = PrePlaceMode::Set;
            p.clearTasks = true;
            p.setHeading = true;
        }
        else if (distM >= kPrePlaceKeepM)
        {
            p.mode = PrePlaceMode::Blend;
            p.setHeading = true;
        }
        else
            p.setHeading = headingErrDeg > kPrePlaceHeadingDeg;
        return p;
    }

    // Started-versus-fallback accounting of the 10 s sync line: a window that issued a task, the ones that started (the puppet
    // really climbs / is in cover), the ones that fell back to the placement, and the ones that ended.
    struct TaskWindowTally
    {
        uint32_t issued = 0, started = 0, fellBack = 0, ended = 0;
    };

    // One ClimbWindow::Update, counted: `before` and `after` are CurrentPhase() around it, `issued` is Out::issueClimb. A task
    // that could not be called (NoteIssueFailed moves Pending to Fallback) is counted by the caller's second call with the
    // phases before and after that.
    inline void CountClimb(TaskWindowTally& t, ClimbWindow::Phase before, ClimbWindow::Phase after, bool issued)
    {
        using P = ClimbWindow::Phase;
        if (issued)
            ++t.issued;
        if (before == P::Pending && after == P::Active)
            ++t.started;
        if (before != P::Fallback && after == P::Fallback)
            ++t.fellBack;
        if (before == P::Settle && after == P::Idle)
            ++t.ended;
    }

    // ---- receiver: cover (0d) ----

    // The sender's kPfCover (alive, on foot, not ragdolling) as a window around TASK_PUT_PED_DIRECTLY_INTO_COVER. The 0.4.3 code
    // set its "cover hold" as soon as the native was CALLED and kept the gait controller, the velocity push and the anchor off
    // the ped for as long as the flag stayed up, whether or not the puppet ever got into cover: with no cover point at the
    // coordinates (a car is not one) the puppet then stood frozen wherever it was, up to 4 m from the sender, no animation.
    //   Idle     -> flag rises: issue (the engine puts the puppet on the pose first, DecidePrePlace), phase Pending, suspend.
    //   Pending  -> pedInCover: phase Held (started). Not within startTimeoutMs: one more issue (the pose moved on), then
    //               Fallback (failed): the engine clears the task and hands the ped back to the controller, which keeps it on the
    //               sender's pose (standing, placed) until the flag falls.
    //   Held     -> suspend (no gait task, no velocity push, no anchor move). The puppet leaving cover by itself (pedInCover
    //               false for lostMs) or drifting off the sender (> driftReissueM for driftMs, or > driftPlaceM at once) issues
    //               again, at most every reissueGapMs.
    //   flag falls (Pending / Held) -> ended: the engine clears the task and Resets the controller; Fallback -> Idle quietly.
    class CoverWindow
    {
    public:
        struct Out
        {
            bool issue = false;      // TASK_PUT_PED_DIRECTLY_INTO_COVER now (put the puppet on the pose first)
            bool suspend = false;    // no gait task, no velocity push, no anchor move this frame
            bool failed = false;     // the task never started: clear it, Reset the controller (once)
            bool ended = false;      // the sender left cover while the task was held: clear it, Reset the controller (once)
        };
        enum class Phase : uint8_t { Idle, Pending, Held, Fallback };

        explicit CoverWindow(double startTimeoutMs = 350.0, double lostMs = 400.0, double driftMs = 600.0,
                             double reissueGapMs = 1000.0, float driftReissueM = 1.5f, float driftPlaceM = 4.0f)
            : startTimeoutMs_(startTimeoutMs), lostMs_(lostMs), driftMs_(driftMs), reissueGapMs_(reissueGapMs),
              driftReissueM_(driftReissueM), driftPlaceM_(driftPlaceM) {}

        // flag: the sender is in cover and the puppet may take it. pedInCover: IS_PED_IN_COVER of the puppet (only asked while
        // Busy()). driftM: horizontal distance of the puppet from the sender's pose.
        Out Update(bool flag, bool pedInCover, float driftM, double nowMs)
        {
            Out o;
            switch (phase_)
            {
            case Phase::Idle:
                if (flag)
                {
                    issuesThisHold_ = 0;
                    Issue(o, nowMs, Phase::Pending);
                }
                break;
            case Phase::Pending:
                if (!flag)
                {
                    o.ended = true;
                    phase_ = Phase::Idle;
                    ++tally_.ended;
                    break;
                }
                o.suspend = true;
                if (pedInCover)
                {
                    phase_ = Phase::Held;
                    lostSince_ = -1.0;
                    driftSince_ = -1.0;
                    ++tally_.started;
                }
                else if (nowMs - issueMs_ >= startTimeoutMs_)
                {
                    if (issuesThisHold_ < 2)
                        Issue(o, nowMs, Phase::Pending);
                    else
                    {
                        phase_ = Phase::Fallback;
                        o.failed = true;
                        o.suspend = false;
                        ++tally_.fellBack;
                    }
                }
                break;
            case Phase::Held:
                if (!flag)
                {
                    o.ended = true;
                    phase_ = Phase::Idle;
                    ++tally_.ended;
                    break;
                }
                o.suspend = true;
                if (pedInCover)
                    lostSince_ = -1.0;
                else if (lostSince_ < 0.0)
                    lostSince_ = nowMs;
                if (driftM > driftReissueM_)
                {
                    if (driftSince_ < 0.0)
                        driftSince_ = nowMs;
                }
                else
                    driftSince_ = -1.0;
                {
                    const bool lost = lostSince_ >= 0.0 && nowMs - lostSince_ >= lostMs_;
                    const bool drifted = (driftSince_ >= 0.0 && nowMs - driftSince_ >= driftMs_) || driftM > driftPlaceM_;
                    if ((lost || drifted) && (nowMs - issueMs_ >= reissueGapMs_ || driftM > driftPlaceM_))
                    {
                        issuesThisHold_ = 0;
                        Issue(o, nowMs, Phase::Pending);
                    }
                }
                break;
            case Phase::Fallback:
                if (!flag)
                    phase_ = Phase::Idle;              // the task never ran: nothing to clear, the controller owns the ped
                break;
            }
            return o;
        }
        // The native could not be called at all: straight to the fallback (the controller keeps the ped on the pose).
        void NoteIssueFailed()
        {
            if (phase_ == Phase::Pending)
            {
                phase_ = Phase::Fallback;
                ++tally_.fellBack;
            }
        }
        Phase CurrentPhase() const { return phase_; }
        bool Busy() const { return phase_ == Phase::Pending || phase_ == Phase::Held; }
        const TaskWindowTally& Tally() const { return tally_; }
        // A new ped, or the engine took the ped over (death, seat): forget the window; the tally lives on.
        void Reset()
        {
            phase_ = Phase::Idle;
            issuesThisHold_ = 0;
            lostSince_ = driftSince_ = -1.0;
        }

    private:
        void Issue(Out& o, double nowMs, Phase next)
        {
            o.issue = true;
            o.suspend = true;
            phase_ = next;
            issueMs_ = nowMs;
            ++issuesThisHold_;
            ++tally_.issued;
        }

        double startTimeoutMs_, lostMs_, driftMs_, reissueGapMs_;
        float driftReissueM_, driftPlaceM_;
        Phase phase_ = Phase::Idle;
        double issueMs_ = 0, lostSince_ = -1.0, driftSince_ = -1.0;
        int issuesThisHold_ = 0;
        TaskWindowTally tally_;
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
