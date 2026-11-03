// T1 seats (desync overhaul round 2, wire v6): the CLIENT side of the server-authoritative enter / exit intent.
// Pure C++17, header-only, no game or OS headers, single-threaded (the engine's script tick). The engine reads the natives and
// applies what these units return; tests/t1_seats_tests.h drives them against the real VehicleStore and, in netcode_test,
// the real server.
//
// THE PROTOCOL (docs/api/protocol.md "T1 seats"):
//   * The server owns seat occupancy (VehicleWorld.seats) AND, new in v6, seat RESERVATIONS: VehicleOp::EnterIntent reserves a
//     seat for a player who is walking to it; every relevant client is told with a SeatNotice (Reserved, Seated, Cancelled,
//     Exited). A reservation seats nobody and is never in the seat list.
//   * The player's own client, on F / G, sends EnterIntent, waits for the answer, and only then starts GTA's real
//     TASK_ENTER_VEHICLE for exactly that seat (LocalEnterFlow). It is seated by the game; the link's ordinary Enter then turns the
//     reservation into occupancy. A refused or unanswered intent starts nothing.
//   * Every other client replays the same task on the puppet when SeatNotice::Reserved arrives (PuppetSeatReplay), ONCE per
//     intent, and lets it finish. The seat warp (put the ped into the seat the list names) is only a BOUNDED FALLBACK: it waits
//     kConfirmGraceMs after the server says the player is seated, and never longer than the task's own time limit.
//   * Exit: SeatNotice::Exited plays the normal exit animation at once; a short latch stops a stale seat list (state channel,
//     unordered against the reliable notice) from putting the ped back in.
//
// Units:
//   EnterTaskWatch      the shared question "is a TASK_ENTER_VEHICLE we issued running, done, dead, or stuck": started / ended /
//                       never started / timed out, from the two booleans the engine can read (seated, trying to enter).
//   SeatBook            this client's mirror of the server's reservations (so F never picks a seat somebody is walking to).
//   PuppetSeatReplay    one remote player: Pending -> Running -> Done / Waiting, the warp gate, the exit latch, the counters.
//   LocalEnterFlow      our own player: AwaitIntent -> Running -> Done, with the bounded fallback warp.
//   SeatClientModel     the notice dispatcher: SeatBook + one PuppetSeatReplay per player + the local flow's cancel.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "protocol/protocol.h"

namespace gage::sync {

    constexpr uint16_t kSeatNobody = proto::kNoPlayer;

    inline uint32_t PackIntentArg(int seat, bool run) { return (uint32_t)(seat & 0xFF) | (run ? proto::kIntentRun : 0u); }

    // ---- the task watch ------------------------------------------------------------------------------------------------

    struct EnterWatchCfg
    {
        double maxMs = 4500.0;        // the task's whole allowance (door walk + climb in), then the fallback
        double startWaitMs = 1000.0;  // issued but never seen running (locked door, refused task, native not callable)
        double endedDebounceMs = 400.0;   // seen running, now not, and not seated: the task ended without a seat
    };

    enum class WatchState : uint8_t { Running, Seated, Ended, NeverStarted, TimedOut };

    class EnterTaskWatch
    {
    public:
        void Begin(double nowMs, const EnterWatchCfg& cfg)
        {
            *this = EnterTaskWatch();
            cfg_ = cfg;
            beganMs_ = nowMs;
            active_ = true;
        }
        // seated: the ped sits in the seat it was sent to. trying: GET_VEHICLE_PED_IS_TRYING_TO_ENTER names the car.
        WatchState Update(double nowMs, bool seated, bool trying)
        {
            if (seated) return WatchState::Seated;
            if (trying)
            {
                if (!started_) startedMs_ = nowMs;
                started_ = true;
                notTryingSinceMs_ = -1.0;
            }
            else if (started_ && notTryingSinceMs_ < 0.0)
                notTryingSinceMs_ = nowMs;
            if (nowMs - beganMs_ >= cfg_.maxMs) return WatchState::TimedOut;
            if (!started_ && nowMs - beganMs_ >= cfg_.startWaitMs) return WatchState::NeverStarted;
            if (started_ && notTryingSinceMs_ >= 0.0 && nowMs - notTryingSinceMs_ >= cfg_.endedDebounceMs) return WatchState::Ended;
            return WatchState::Running;
        }
        bool Started() const { return started_; }
        bool Active() const { return active_; }
        double BeganMs() const { return beganMs_; }

    private:
        EnterWatchCfg cfg_;
        bool active_ = false, started_ = false;
        double beganMs_ = 0, startedMs_ = 0, notTryingSinceMs_ = -1.0;
    };

    // ---- the reservation book ------------------------------------------------------------------------------------------

    class SeatBook
    {
    public:
        // A reservation we never hear the end of is forgotten after the server's own limit plus a margin.
        static constexpr double kStaleMs = (double)proto::kIntentTtlMs + 1500.0;

        void OnReserved(int veh, int seat, uint16_t player, double nowMs)
        {
            DropPlayer(player);                      // one reservation per player
            e_.push_back(Entry{veh, seat, player, nowMs});
        }
        void OnEnded(int veh, int seat, uint16_t player)
        {
            e_.erase(std::remove_if(e_.begin(), e_.end(), [&](const Entry& x) { return x.player == player && x.veh == veh && x.seat == seat; }),
                     e_.end());
        }
        void DropPlayer(uint16_t player)
        {
            e_.erase(std::remove_if(e_.begin(), e_.end(), [&](const Entry& x) { return x.player == player; }), e_.end());
        }
        void DropVehicle(int veh)
        {
            e_.erase(std::remove_if(e_.begin(), e_.end(), [&](const Entry& x) { return x.veh == veh; }), e_.end());
        }
        void Clear() { e_.clear(); }
        // Who walks to (veh, seat) now, kSeatNobody when nobody (or the entry is stale).
        uint16_t Holder(int veh, int seat, double nowMs) const
        {
            for (const Entry& x : e_)
                if (x.veh == veh && x.seat == seat && nowMs - x.sinceMs < kStaleMs) return x.player;
            return kSeatNobody;
        }
        size_t Size() const { return e_.size(); }

    private:
        struct Entry { int veh; int seat; uint16_t player; double sinceMs; };
        std::vector<Entry> e_;
    };

    // ---- one remote player's replay ------------------------------------------------------------------------------------

    struct SeatReplayCfg
    {
        EnterWatchCfg door{4500.0, 1000.0, 400.0};
        EnterWatchCfg mount{3500.0, 1000.0, 400.0};   // a bike or a cycle has no door to open
        double confirmGraceMs = 1500.0;   // the server says seated: the task gets this long to finish before the warp
        double intentWaitMs = 3000.0;     // intent known but the car or the ped is not here yet
        double waitConfirmMs = (double)proto::kIntentTtlMs + 1000.0;   // task over, no word from the server: give up this long after the intent
        double exitLatchMs = 700.0;       // after an Exited notice the seat list may not seat the player again
        double unlistedMs = 2500.0;       // seated by the task, still not in the server's list this long after: put out
    };

    enum class SeatAct : uint8_t { None, IssueEnter, ClearTask, Evict };
    // Why a warp (SetPedIntoVehicle on a puppet) was allowed; the counters tell the story in the 10 s line.
    enum class WarpWhy : uint8_t { None, NoIntent, Confirmed, GraceOver, TaskEnded, TaskTimeout };

    struct SeatReplayIn
    {
        double nowMs = 0;
        bool pedReady = false, vehReady = false;
        bool inSeat = false;       // the ped sits in the intent's seat of the intent's car
        bool tryingToEnter = false;
        bool mount = false;        // the car is a bike or a cycle (GET_VEHICLE_CLASS 8 / 13)
        bool listedSomewhere = false;   // the server's seat list names this player in the intent's car
    };
    struct SeatReplayOut
    {
        SeatAct act = SeatAct::None;
        int veh = -1, seat = -1;
        bool run = false;
        int timeoutMs = 0;
        bool holdsPed = false;     // the task owns the ped: no gait task over it
    };
    struct WarpDecision { bool allow = false; WarpWhy why = WarpWhy::None; };

    struct SeatReplayStats
    {
        uint32_t intents = 0, issued = 0, started = 0, finished = 0, timeouts = 0, neverStarted = 0, endedNoSeat = 0, issueFailed = 0,
                 cancelled = 0, exits = 0, latchDenied = 0, evicted = 0, gaveUp = 0, droppedNotReady = 0,
                 warpNoIntent = 0, warpConfirmed = 0, warpGraceOver = 0, warpTaskEnded = 0, warpTaskTimeout = 0,
                 mounts = 0;
        uint32_t Warps() const { return warpNoIntent + warpConfirmed + warpGraceOver + warpTaskEnded + warpTaskTimeout; }
        void Add(const SeatReplayStats& o)
        {
            intents += o.intents; issued += o.issued; started += o.started; finished += o.finished; timeouts += o.timeouts;
            neverStarted += o.neverStarted; endedNoSeat += o.endedNoSeat; issueFailed += o.issueFailed; cancelled += o.cancelled;
            exits += o.exits; latchDenied += o.latchDenied; evicted += o.evicted; gaveUp += o.gaveUp; droppedNotReady += o.droppedNotReady;
            warpNoIntent += o.warpNoIntent; warpConfirmed += o.warpConfirmed; warpGraceOver += o.warpGraceOver;
            warpTaskEnded += o.warpTaskEnded; warpTaskTimeout += o.warpTaskTimeout; mounts += o.mounts;
        }
    };

    class PuppetSeatReplay
    {
    public:
        enum class Phase : uint8_t { Idle, Pending, Running, Waiting, Done };

        explicit PuppetSeatReplay(const SeatReplayCfg& cfg = SeatReplayCfg()) : cfg_(cfg) {}

        // SeatNotice::Reserved for this player. A newer intent replaces an older one (its task is cleared first).
        void OnReserve(int veh, int seat, bool run, double nowMs)
        {
            if (phase_ == Phase::Running)
                clearPending_ = true;
            phase_ = Phase::Pending;
            veh_ = veh;
            seat_ = seat;
            run_ = run;
            intentMs_ = nowMs;
            confirmedMs_ = listedMs_ = -1.0;
            endedWhy_ = WarpWhy::None;
            latchVeh_ = -1;                              // a new walk ends the old exit's latch
            ++stats_.intents;
        }
        // SeatNotice::Seated: the server has the player in the seat. The task (if any) keeps running up to the grace.
        void OnSeated(int veh, int seat, double nowMs)
        {
            if (phase_ == Phase::Idle || phase_ == Phase::Done || veh != veh_ || seat != seat_) return;
            if (confirmedMs_ < 0.0) confirmedMs_ = nowMs;
        }
        // SeatNotice::Cancelled: the walk is over without a seating.
        void OnCancel(int veh, int seat, double /*nowMs*/)
        {
            if (phase_ == Phase::Idle || phase_ == Phase::Done || veh != veh_ || seat != seat_) return;
            if (phase_ == Phase::Running) clearPending_ = true;
            phase_ = Phase::Idle;
            ++stats_.cancelled;
        }
        // SeatNotice::Exited: the player left its seat by its own Exit. Latches the seat list for exitLatchMs.
        void OnExit(int veh, double nowMs)
        {
            latchVeh_ = veh;
            latchUntilMs_ = nowMs + cfg_.exitLatchMs;
            ++stats_.exits;
            if (phase_ == Phase::Running) clearPending_ = true;
            if (phase_ != Phase::Idle && phase_ != Phase::Done) phase_ = Phase::Idle;
        }
        // The engine could not start the task (native not callable): behave as a task that never ran.
        void NoteIssueFailed()
        {
            if (phase_ != Phase::Running) return;
            ++stats_.issueFailed;
            phase_ = Phase::Waiting;
            endedWhy_ = WarpWhy::TaskEnded;
        }
        // The engine warped the ped into a seat (allowed by AllowsSeatWarp).
        void NoteWarp(WarpWhy why)
        {
            switch (why)
            {
            case WarpWhy::NoIntent: ++stats_.warpNoIntent; break;
            case WarpWhy::Confirmed: ++stats_.warpConfirmed; break;
            case WarpWhy::GraceOver: ++stats_.warpGraceOver; break;
            case WarpWhy::TaskEnded: ++stats_.warpTaskEnded; break;
            case WarpWhy::TaskTimeout: ++stats_.warpTaskTimeout; break;
            case WarpWhy::None: break;
            }
            if (phase_ == Phase::Pending || phase_ == Phase::Running || phase_ == Phase::Waiting)
            {
                if (phase_ == Phase::Running) clearPending_ = false;   // the warp itself clears the ped's tasks
                phase_ = Phase::Done;
                doneMs_ = lastNowMs_;
            }
        }

        // The seat loop asks, for each seat of `veh` that the server's list gives to this player.
        WarpDecision AllowsSeatWarp(int veh, int seat, double nowMs)
        {
            WarpDecision d;
            if (veh == latchVeh_ && nowMs < latchUntilMs_)
            {
                ++stats_.latchDenied;
                return d;                                    // a stale frame: the exit already happened
            }
            const bool mine = veh == veh_ && seat == seat_;
            if (!mine || phase_ == Phase::Idle || phase_ == Phase::Done)
            {
                d.allow = true;
                d.why = WarpWhy::NoIntent;
                return d;
            }
            if (listedMs_ < 0.0) listedMs_ = nowMs;
            switch (phase_)
            {
            case Phase::Pending:                            // the list won the race with the task: nothing left to animate
                d.allow = true;
                d.why = WarpWhy::Confirmed;
                return d;
            case Phase::Running:
            {
                const double since = nowMs - std::min(listedMs_, confirmedMs_ < 0.0 ? listedMs_ : confirmedMs_);
                if (since >= cfg_.confirmGraceMs)
                {
                    d.allow = true;
                    d.why = WarpWhy::GraceOver;
                }
                return d;
            }
            case Phase::Waiting:
                d.allow = true;
                d.why = endedWhy_ == WarpWhy::None ? WarpWhy::TaskEnded : endedWhy_;
                return d;
            default:
                return d;
            }
        }

        // One call per frame per player with the natives' readings. The result says what to do (issue, clear, evict) and
        // whether the task owns the ped.
        SeatReplayOut Tick(const SeatReplayIn& in)
        {
            SeatReplayOut o;
            lastNowMs_ = in.nowMs;
            if (in.listedSomewhere && listedMs_ < 0.0 && Active())
                listedMs_ = in.nowMs;
            if (clearPending_)
            {
                clearPending_ = false;
                if (in.pedReady && !in.inSeat)
                {
                    o.act = SeatAct::ClearTask;
                    return o;
                }
            }
            switch (phase_)
            {
            case Phase::Idle:
                break;
            case Phase::Pending:
                if (!in.pedReady || !in.vehReady)
                {
                    if (in.nowMs - intentMs_ >= cfg_.intentWaitMs)
                    {
                        phase_ = Phase::Idle;                // the car never came: the seat list will seat it when it can
                        ++stats_.droppedNotReady;
                    }
                    break;
                }
                if (confirmedMs_ >= 0.0)
                {
                    phase_ = Phase::Waiting;                 // seated before we could start: no animation left, the warp is allowed
                    endedWhy_ = WarpWhy::Confirmed;
                    break;
                }
                {
                    mount_ = in.mount;
                    watch_.Begin(in.nowMs, in.mount ? cfg_.mount : cfg_.door);
                    phase_ = Phase::Running;
                    ++stats_.issued;
                    if (in.mount) ++stats_.mounts;
                    o.act = SeatAct::IssueEnter;
                    o.veh = veh_;
                    o.seat = seat_;
                    o.run = run_;
                    o.timeoutMs = (int)(in.mount ? cfg_.mount.maxMs : cfg_.door.maxMs);
                    o.holdsPed = true;
                }
                break;
            case Phase::Running:
            {
                if (!in.pedReady || !in.vehReady)
                {
                    phase_ = Phase::Idle;                    // the ped or the car went away under the task
                    break;
                }
                const bool wasStarted = watch_.Started();
                const WatchState w = watch_.Update(in.nowMs, in.inSeat, in.tryingToEnter);
                if (!wasStarted && watch_.Started()) ++stats_.started;
                switch (w)
                {
                case WatchState::Running:
                    o.holdsPed = true;
                    break;
                case WatchState::Seated:
                    phase_ = Phase::Done;
                    doneMs_ = in.nowMs;
                    ++stats_.finished;
                    break;
                case WatchState::Ended:
                    ++stats_.endedNoSeat;
                    phase_ = Phase::Waiting;
                    endedWhy_ = WarpWhy::TaskEnded;
                    break;
                case WatchState::NeverStarted:
                    ++stats_.neverStarted;
                    phase_ = Phase::Waiting;
                    endedWhy_ = WarpWhy::TaskEnded;
                    break;
                case WatchState::TimedOut:
                    ++stats_.timeouts;
                    phase_ = Phase::Waiting;
                    endedWhy_ = WarpWhy::TaskTimeout;
                    break;
                }
                if (phase_ == Phase::Waiting && confirmedMs_ < 0.0 && listedMs_ < 0.0)
                    o.act = SeatAct::ClearTask;              // over, and the server has not confirmed: the ped follows the pose again
                break;
            }
            case Phase::Waiting:
                if (in.inSeat)
                {
                    phase_ = Phase::Done;
                    doneMs_ = in.nowMs;
                    ++stats_.finished;
                }
                else if (in.nowMs - intentMs_ >= cfg_.waitConfirmMs)
                {
                    phase_ = Phase::Idle;                    // the server never confirmed: the sender gave up
                    ++stats_.gaveUp;
                }
                break;
            case Phase::Done:
                // Seated by the task (or warped). While doneMs_ >= 0 the server's list has not named the player yet; if it never
                // does, the screens disagree and the puppet is put out (Evict). Listed, or out of the car: nothing left to check.
                if (doneMs_ >= 0.0)
                {
                    o.holdsPed = in.inSeat;                  // seated by the task, not yet named by the list: no gait task over a ped in a car
                    if (in.listedSomewhere || !in.inSeat)
                        doneMs_ = -1.0;
                    else if (in.nowMs - doneMs_ >= cfg_.unlistedMs)
                    {
                        phase_ = Phase::Idle;
                        doneMs_ = -1.0;
                        ++stats_.evicted;
                        o.act = SeatAct::Evict;
                    }
                }
                break;
            }
            return o;
        }

        Phase phase() const { return phase_; }
        // True while the engine has to read the natives for this player (an intent in flight, or a seating not yet confirmed
        // by the list); false when there is nothing to watch, so a quiet player costs no native.
        bool NeedsReadings() const { return Active() || clearPending_ || (phase_ == Phase::Done && doneMs_ >= 0.0); }
        int Veh() const { return phase_ == Phase::Idle ? -1 : veh_; }
        int Seat() const { return seat_; }
        bool Active() const { return phase_ == Phase::Pending || phase_ == Phase::Running || phase_ == Phase::Waiting; }
        bool Latched(int veh, double nowMs) const { return veh == latchVeh_ && nowMs < latchUntilMs_; }
        SeatReplayStats TakeStats()
        {
            SeatReplayStats s = stats_;
            stats_ = SeatReplayStats();
            return s;
        }
        const SeatReplayStats& stats() const { return stats_; }

    private:
        SeatReplayCfg cfg_;
        Phase phase_ = Phase::Idle;
        int veh_ = -1, seat_ = -1;
        bool run_ = false, mount_ = false, clearPending_ = false;
        double intentMs_ = 0, confirmedMs_ = -1.0, listedMs_ = -1.0, doneMs_ = -1.0, lastNowMs_ = 0;
        WarpWhy endedWhy_ = WarpWhy::None;
        int latchVeh_ = -1;
        double latchUntilMs_ = 0;
        EnterTaskWatch watch_;
        SeatReplayStats stats_;
    };

    // ---- the local player's own entry ----------------------------------------------------------------------------------

    struct LocalEnterCfg
    {
        EnterWatchCfg door{4500.0, 1000.0, 400.0};
        EnterWatchCfg mount{3500.0, 1000.0, 400.0};
        double answerWaitMs = 2000.0;    // the server's answer to the intent
    };
    enum class LocalAct : uint8_t
    {
        None,
        SendIntent,    // send VehicleOp::EnterIntent (netId, seat, run) -- emitted once, by Begin's first Tick
        IssueEnter,    // the server said Ok: TASK_ENTER_VEHICLE for the reserved seat
        SendCancel,    // send VehicleOp::CancelIntent
        ClearTask,     // clear the ped's tasks (a cancel after the task started)
        FallbackWarp,  // the task failed or ran out of time: clear it and SET_PED_INTO_VEHICLE (the bounded teleport-in)
    };
    enum class LocalWhy : uint8_t { None, Refused, NoAnswer, Aborted, ServerCancelled, NeverStarted, EndedNoSeat, TimedOut };
    struct LocalEnterIn
    {
        double nowMs = 0;
        bool seatedNow = false;      // GET_VEHICLE_PED_IS_IN is non-zero: the game seated us
        bool tryingToEnter = false;
        bool mount = false;
        bool abort = false;          // ragdoll, falling, dead, the car went away
    };
    struct LocalEnterOut
    {
        LocalAct act = LocalAct::None;
        LocalAct also = LocalAct::None;   // a second action (SendCancel with ClearTask)
        LocalWhy why = LocalWhy::None;
        uint8_t refusal = 0;              // the result code when why == Refused
        int timeoutMs = 0;
    };
    struct LocalEnterStats
    {
        uint32_t begun = 0, sent = 0, accepted = 0, refused = 0, noAnswer = 0, issued = 0, started = 0, finished = 0, aborted = 0,
                 serverCancelled = 0, neverStarted = 0, endedNoSeat = 0, timedOut = 0, fallbackWarps = 0;
    };

    class LocalEnterFlow
    {
    public:
        enum class Phase : uint8_t { Idle, Send, AwaitAnswer, Issue, Running, Done };

        explicit LocalEnterFlow(const LocalEnterCfg& cfg = LocalEnterCfg()) : cfg_(cfg) {}

        // F / G picked (netId, seat). The first Tick returns SendIntent.
        void Begin(int netId, int seat, bool run, double nowMs)
        {
            *this = LocalEnterFlow(cfg_, stats_);
            phase_ = Phase::Send;
            netId_ = netId;
            seat_ = seat;
            run_ = run;
            beganMs_ = nowMs;
            ++stats_.begun;
        }
        // The server's answer to our EnterIntent for this car.
        void OnResult(int netId, uint8_t code)
        {
            if (phase_ != Phase::AwaitAnswer || netId != netId_) return;
            if (code == 0)
            {
                phase_ = Phase::Issue;
                ++stats_.accepted;
            }
            else
            {
                refusedCode_ = code;
                refusedPending_ = true;
                ++stats_.refused;
            }
        }
        // A SeatNotice::Cancelled about us (expired, replaced, the car went): the walk is void.
        void OnServerCancel(int netId, int seat)
        {
            if (netId != netId_ || seat != seat_) return;
            if (phase_ == Phase::Running || phase_ == Phase::Issue || phase_ == Phase::AwaitAnswer)
            {
                serverCancelPending_ = true;
                ++stats_.serverCancelled;
            }
        }
        // Drop everything without telling the server (the session ended).
        void Reset() { *this = LocalEnterFlow(cfg_, stats_); }

        LocalEnterOut Tick(const LocalEnterIn& in)
        {
            LocalEnterOut o;
            switch (phase_)
            {
            case Phase::Idle:
            case Phase::Done:
                return o;
            case Phase::Send:
                if (in.seatedNow)
                {
                    phase_ = Phase::Done;
                    ++stats_.finished;
                    return o;
                }
                if (in.abort)
                {
                    phase_ = Phase::Idle;
                    ++stats_.aborted;
                    o.why = LocalWhy::Aborted;
                    return o;
                }
                phase_ = Phase::AwaitAnswer;
                sentMs_ = in.nowMs;
                ++stats_.sent;
                o.act = LocalAct::SendIntent;
                return o;
            case Phase::AwaitAnswer:
                if (refusedPending_)
                {
                    phase_ = Phase::Idle;
                    o.why = LocalWhy::Refused;
                    o.refusal = refusedCode_;
                    return o;
                }
                if (in.seatedNow)                            // the game seated us before the answer (it should not): the link's Enter takes over
                {
                    phase_ = Phase::Done;
                    ++stats_.finished;
                    return o;
                }
                if (in.abort || serverCancelPending_)
                {
                    const bool sc = serverCancelPending_ && !in.abort;
                    phase_ = Phase::Idle;
                    serverCancelPending_ = false;
                    if (in.abort) ++stats_.aborted;
                    o.act = LocalAct::SendCancel;
                    o.why = sc ? LocalWhy::ServerCancelled : LocalWhy::Aborted;
                    return o;
                }
                if (in.nowMs - sentMs_ >= cfg_.answerWaitMs)
                {
                    phase_ = Phase::Idle;
                    ++stats_.noAnswer;
                    o.act = LocalAct::SendCancel;
                    o.why = LocalWhy::NoAnswer;
                }
                return o;
            case Phase::Issue:
                if (in.seatedNow)
                {
                    phase_ = Phase::Done;
                    ++stats_.finished;
                    return o;
                }
                if (in.abort || serverCancelPending_)
                {
                    phase_ = Phase::Idle;
                    const bool sc = serverCancelPending_;
                    serverCancelPending_ = false;
                    if (in.abort) ++stats_.aborted;
                    o.act = LocalAct::SendCancel;
                    o.why = sc && !in.abort ? LocalWhy::ServerCancelled : LocalWhy::Aborted;
                    return o;
                }
                watch_.Begin(in.nowMs, in.mount ? cfg_.mount : cfg_.door);
                phase_ = Phase::Running;
                ++stats_.issued;
                o.act = LocalAct::IssueEnter;
                o.timeoutMs = (int)(in.mount ? cfg_.mount.maxMs : cfg_.door.maxMs);
                return o;
            case Phase::Running:
            {
                const bool was = watch_.Started();
                const WatchState w = watch_.Update(in.nowMs, in.seatedNow, in.tryingToEnter);
                if (!was && watch_.Started()) ++stats_.started;
                if (w == WatchState::Seated)
                {
                    phase_ = Phase::Done;
                    ++stats_.finished;                       // seated by the game's own task: no warp
                    return o;
                }
                if (in.abort || serverCancelPending_)
                {
                    const bool sc = serverCancelPending_;
                    serverCancelPending_ = false;
                    phase_ = Phase::Idle;
                    if (in.abort) ++stats_.aborted;
                    o.act = LocalAct::ClearTask;
                    o.also = LocalAct::SendCancel;
                    o.why = sc && !in.abort ? LocalWhy::ServerCancelled : LocalWhy::Aborted;
                    return o;
                }
                if (w == WatchState::Running)
                    return o;
                phase_ = Phase::Done;
                o.act = LocalAct::FallbackWarp;
                ++stats_.fallbackWarps;
                if (w == WatchState::NeverStarted) { ++stats_.neverStarted; o.why = LocalWhy::NeverStarted; }
                else if (w == WatchState::Ended) { ++stats_.endedNoSeat; o.why = LocalWhy::EndedNoSeat; }
                else { ++stats_.timedOut; o.why = LocalWhy::TimedOut; }
                return o;
            }
            }
            return o;
        }

        Phase phase() const { return phase_; }
        bool Active() const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
        bool Tasked() const { return phase_ == Phase::Running; }
        int NetId() const { return netId_; }
        int Seat() const { return seat_; }
        bool Run() const { return run_; }
        const LocalEnterStats& stats() const { return stats_; }
        LocalEnterStats TakeStats()
        {
            LocalEnterStats s = stats_;
            stats_ = LocalEnterStats();
            return s;
        }

    private:
        LocalEnterFlow(const LocalEnterCfg& cfg, const LocalEnterStats& st) : cfg_(cfg), stats_(st) {}
        LocalEnterCfg cfg_;
        Phase phase_ = Phase::Idle;
        int netId_ = -1, seat_ = -1;
        bool run_ = false, refusedPending_ = false, serverCancelPending_ = false;
        uint8_t refusedCode_ = 0;
        double beganMs_ = 0, sentMs_ = 0;
        EnterTaskWatch watch_;
        LocalEnterStats stats_;
    };

    // ---- the notice dispatcher -----------------------------------------------------------------------------------------

    struct NoticeEffect
    {
        bool unseatPuppet = false;     // an Exited notice: run the normal exit animation for `player` (if it is seated in `veh`)
        uint16_t player = kSeatNobody;
        int veh = -1;
        bool aboutMe = false;          // the notice concerns the local player (cancel its flow / nothing for the puppet table)
    };

    class SeatClientModel
    {
    public:
        explicit SeatClientModel(const SeatReplayCfg& cfg = SeatReplayCfg()) : cfg_(cfg) {}

        // One SeatNotice, in the order the server sent it. `me` is the local player's id.
        NoticeEffect OnNotice(const proto::SeatNotice& n, uint16_t me, double nowMs)
        {
            NoticeEffect fx;
            fx.player = n.player;
            fx.veh = n.vehId;
            fx.aboutMe = n.player == me;
            switch (n.kind)
            {
            case proto::SeatNoticeKind::Reserved:
                book_.OnReserved(n.vehId, n.seat, n.player, nowMs);
                if (!fx.aboutMe) Puppet(n.player).OnReserve(n.vehId, n.seat, (n.detail & proto::kSeatNoticeRun) != 0, nowMs);
                break;
            case proto::SeatNoticeKind::Seated:
                book_.OnEnded(n.vehId, n.seat, n.player);
                if (!fx.aboutMe) Puppet(n.player).OnSeated(n.vehId, n.seat, nowMs);
                break;
            case proto::SeatNoticeKind::Cancelled:
                book_.OnEnded(n.vehId, n.seat, n.player);
                if (!fx.aboutMe) Puppet(n.player).OnCancel(n.vehId, n.seat, nowMs);
                break;
            case proto::SeatNoticeKind::Exited:
                book_.OnEnded(n.vehId, n.seat, n.player);
                if (!fx.aboutMe)
                {
                    Puppet(n.player).OnExit(n.vehId, nowMs);
                    fx.unseatPuppet = true;
                }
                break;
            }
            return fx;
        }

        PuppetSeatReplay& Puppet(uint16_t player)
        {
            auto it = puppets_.find(player);
            if (it == puppets_.end())
                it = puppets_.emplace(player, PuppetSeatReplay(cfg_)).first;
            return it->second;
        }
        const PuppetSeatReplay* FindPuppet(uint16_t player) const
        {
            const auto it = puppets_.find(player);
            return it == puppets_.end() ? nullptr : &it->second;
        }
        PuppetSeatReplay* Find(uint16_t player)
        {
            const auto it = puppets_.find(player);
            return it == puppets_.end() ? nullptr : &it->second;
        }
        SeatBook& Book() { return book_; }
        const SeatBook& Book() const { return book_; }
        // A vehicle was removed: nothing walks to it any more.
        void OnVehicleGone(int veh)
        {
            book_.DropVehicle(veh);
            for (auto& kv : puppets_)
                if (kv.second.Veh() == veh)
                    kv.second.OnCancel(veh, kv.second.Seat(), 0.0);
        }
        void DropPlayer(uint16_t player)
        {
            book_.DropPlayer(player);
            const auto it = puppets_.find(player);
            if (it != puppets_.end())
            {
                retired_.Add(it->second.TakeStats());
                puppets_.erase(it);
            }
        }
        void Reset()
        {
            book_.Clear();
            puppets_.clear();
        }
        // The sum of every puppet's counters since the last call (the 10 s line).
        SeatReplayStats TakeStats()
        {
            SeatReplayStats s = retired_;
            retired_ = SeatReplayStats();
            for (auto& kv : puppets_)
                s.Add(kv.second.TakeStats());
            return s;
        }
        size_t Puppets() const { return puppets_.size(); }

    private:
        SeatReplayCfg cfg_;
        SeatBook book_;
        std::map<uint16_t, PuppetSeatReplay> puppets_;
        SeatReplayStats retired_;
    };

}   // namespace gage::sync
