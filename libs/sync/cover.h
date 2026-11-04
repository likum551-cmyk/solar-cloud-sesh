// libs/sync/cover.h - R3 T3 "cover": the sender's cover classification and the receiver's verified cover window.
// Header-only, portable, no game calls: apps/engine/engine.cpp wires it (ReadLocalPedState / TickPeerCover), tests/r3_t3_cover_tests.h
// proves it against a simulated puppet.
//
// WHY (GI's 0.4.7 live test: "taking cover behind a car is not in sync and buggy"; P2's log is not available):
//   The 0d window (pedstate.h CoverWindow, 0.4.3 .. 0.4.7) put the puppet on the sender's pose with DecidePrePlace (a SET at 1 m or more:
//   a teleport), issued TASK_PUT_PED_DIRECTLY_INTO_COVER with identifier -1 and no cover point, and held the puppet frozen (controller,
//   push and anchor off) for up to 700 ms waiting for IS_PED_IN_COVER. A car is not a cover point the task can find, so (model, not
//   measured) the task is not accepted, the puppet stands frozen where it was for 700 ms, is cleared (ClearPedTasksNow) and then stands
//   UPRIGHT on the pose while the sender crouches; and when a cover point does exist a few decimetres away the game moves the puppet onto
//   it (a snap) and the 1.5 m drift rule re-issues from the pose (another set / snap) every second. There was no sender-side evidence at all.
//
// NOW:
//   SENDER  CoverSense classifies IS_PED_IN_COVER / IS_PED_GOING_INTO_COVER / IS_PED_AIMING_FROM_COVER into None / GoingInto / In /
//           AimingFrom / Leaving, sends kPfCover for In, AimingFrom and a short Leaving hold (a read flicker between aim and cover
//           must not look like leaving cover), a crouch hint as kPfStealth (kPfStealth is documented "crouched / stealth movement") when
//           the head is low, and counts everything per 10 s (the line that tells next time what the game answered).
//   RECEIVER CoverPuppet, a window with VERIFIED steps and a safe fallback:
//           Wait     the sender's flag rose; the controller keeps placing the puppet (nothing is forced). The task is only issued when the
//                    puppet is within issueTolM of the pose and the pose is nearly still.
//           Pending  TASK_PUT_PED_DIRECTLY_INTO_COVER issued ONCE (a pre-placement of at most issueTolM: invisible). Accepted only when
//                    IS_PED_IN_COVER reads true within acceptMs (IS_PED_GOING_INTO_COVER true buys goingExtraMs), the puppet is within
//                    acceptDistM of the pose and (not aiming) faces roughly the sender's way. A puppet the game moved more than snapM is a
//                    SNAP (counted); one left far from the pose or on the wrong side is a refusal.
//           Held     accepted. The controller is suspended ONLY here (and for the bounded Pending); puppet-pose error is measured every
//                    frame; lost cover for lostMs -> TASK_STAY_IN_COVER once, then one re-issue; sliding off the pose for slideMs -> Pose.
//           Pose     the SAFE FALLBACK: no task, no clear loop, no snap. The locomotion pipeline keeps placing the puppet on the sender's
//                    pose and the crouch hint (kPfStealth) crouches it. Sticky for the hold except two bounded re-arms (sender slid, cover
//                    lost, never settled) after minPoseMs, so the window cannot flap. A global CoverBreaker opens after consecutive refusals
//                    (a game / puppet that ignores tasks) and sends every hold straight to Pose for a while.
//   Every transition is counted per puppet (CoverStats) and printed with the player id by the engine.
#pragma once

#include "protocol/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gage::sync {

// ---- HKCU cover_sync ----------------------------------------------------------------------------------------------------
enum class CoverSyncMode : uint8_t { Task, Pose, Off };
inline const char* CoverSyncName(CoverSyncMode m) { return m == CoverSyncMode::Pose ? "pose" : m == CoverSyncMode::Off ? "off" : "task"; }
// "pose": never a cover task, the crouch pose on the sender's position. "off": the cover flag is ignored (the normal walk / stand pipeline).
// Anything else (unset included): "task" with the safe fallback.
template <class WStr>
inline CoverSyncMode ParseCoverSync(const WStr& v)
{
    if (v == L"pose")
        return CoverSyncMode::Pose;
    if (v == L"off")
        return CoverSyncMode::Off;
    return CoverSyncMode::Task;
}

inline float CoverHeadingErrDeg(float a, float b)
{
    float d = std::fmod(std::fabs(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

// ================================================================== SENDER =================================================
enum class CoverSenderState : uint8_t { None, GoingInto, In, AimingFrom, Leaving };
inline const char* CoverSenderStateName(CoverSenderState s)
{
    switch (s)
    {
    case CoverSenderState::GoingInto:  return "going-into";
    case CoverSenderState::In:         return "in-cover";
    case CoverSenderState::AimingFrom: return "aiming-from-cover";
    case CoverSenderState::Leaving:    return "leaving";
    default:                           return "none";
    }
}

struct CoverReads
{
    bool  inCover = false;       // IS_PED_IN_COVER(ped, false)
    bool  goingInto = false;     // IS_PED_GOING_INTO_COVER
    bool  aimingFrom = false;    // IS_PED_AIMING_FROM_COVER
    float headAboveM = -99.0f;   // SKEL_Head z minus the ped's z; < -50 = not read
    float x = 0, y = 0, z = 0, heading = 0;
};
struct CoverSenderOut
{
    CoverSenderState state = CoverSenderState::None;
    bool sendCover = false;      // kPfCover
    bool sendCrouch = false;     // kPfStealth (crouched in cover)
};

// What the sender's cover reads looked like in one report window (the sender's own 10 s line).
struct CoverSenderStats
{
    uint32_t samples = 0, inReads = 0, goingReads = 0, aimingReads = 0, goingNoIn = 0;   // goingNoIn: going-into true, no in-cover / aiming read
    uint32_t sentCover = 0, sentLeavingHold = 0, sentCrouch = 0, aimNoIn = 0;            // aimNoIn: aiming-from true with IS_PED_IN_COVER false
    uint32_t rises = 0, falls = 0, flickers = 0;                                         // flicker: a rise within flickerMs of a fall
    uint32_t goingEpisodes = 0, goingThenIn = 0;                                         // a going-into episode, and the ones followed by cover within 3 s
    uint32_t headReads = 0, slideSamples = 0;
    double   holdMsSum = 0, holdMsMax = 0;
    float    headMin = 99.0f, headMax = -99.0f, speedMax = 0, zSpanMax = 0, headingSwingMax = 0;
};

struct CoverSenseCfg
{
    double holdMs = 150.0;        // kPfCover stays up this long after the last read of cover (a flicker is not leaving)
    double flickerMs = 600.0;
    float  crouchBelowM = 0.40f;  // head this close to the ped's root = crouched (hysteresis up to standAboveM)
    float  standAboveM = 0.55f;
    float  slideSpeedMs = 0.5f;
};

class CoverSense
{
public:
    using Cfg = CoverSenseCfg;
    explicit CoverSense(Cfg c = Cfg()) : cfg_(c) {}

    CoverSenderOut Update(const CoverReads& r, double nowMs)
    {
        CoverSenderOut o;
        CoverSenderStats& w = win_;
        ++w.samples;
        if (r.inCover) ++w.inReads;
        if (r.goingInto) ++w.goingReads;
        if (r.aimingFrom) ++w.aimingReads;
        if (r.goingInto && !r.inCover && !r.aimingFrom) ++w.goingNoIn;
        if (r.aimingFrom && !r.inCover) ++w.aimNoIn;
        const bool raw = r.inCover || r.aimingFrom;
        // going-into episodes: opened by the first read, "followed by cover" when cover reads within 3 s of the last going-into read
        if (r.goingInto)
        {
            if (!goingOpen_)
            {
                goingOpen_ = true;
                ++w.goingEpisodes;
                goingCounted_ = false;
            }
            lastGoingMs_ = nowMs;
        }
        if (raw && goingOpen_ && !goingCounted_ && nowMs - lastGoingMs_ <= 3000.0)
        {
            ++w.goingThenIn;
            goingCounted_ = true;
        }
        if (goingOpen_ && nowMs - lastGoingMs_ > 3000.0)
            goingOpen_ = false;

        bool sent = false;
        if (raw)
        {
            lastRawMs_ = nowMs;
            sent = true;
            o.state = r.aimingFrom ? CoverSenderState::AimingFrom : CoverSenderState::In;
            if (!up_)
            {
                up_ = true;
                ++w.rises;
                if (lastFallMs_ > 0.0 && nowMs - lastFallMs_ <= cfg_.flickerMs)
                    ++w.flickers;
                holdStartMs_ = nowMs;
                zMin_ = zMax_ = r.z;
                headingFirst_ = r.heading;
                lastX_ = r.x; lastY_ = r.y; lastMs_ = nowMs;
            }
            // what the pose does in cover: sliding speed, vertical span, how far the heading swings
            if (lastMs_ > 0.0 && nowMs > lastMs_ && nowMs - lastMs_ < 500.0)
            {
                const float sp = std::sqrt((r.x - lastX_) * (r.x - lastX_) + (r.y - lastY_) * (r.y - lastY_)) / (float)((nowMs - lastMs_) / 1000.0);
                if (sp < 20.0f)
                {
                    w.speedMax = std::max(w.speedMax, sp);
                    if (sp > cfg_.slideSpeedMs) ++w.slideSamples;
                }
            }
            lastX_ = r.x; lastY_ = r.y; lastMs_ = nowMs;
            zMin_ = std::min(zMin_, r.z);
            zMax_ = std::max(zMax_, r.z);
            w.zSpanMax = std::max(w.zSpanMax, zMax_ - zMin_);
            w.headingSwingMax = std::max(w.headingSwingMax, CoverHeadingErrDeg(headingFirst_, r.heading));
            if (r.headAboveM > -50.0f)
            {
                ++w.headReads;
                w.headMin = std::min(w.headMin, r.headAboveM);
                w.headMax = std::max(w.headMax, r.headAboveM);
                if (r.headAboveM < cfg_.crouchBelowM) crouched_ = true;
                else if (r.headAboveM > cfg_.standAboveM) crouched_ = false;
            }
        }
        else if (up_ && nowMs - lastRawMs_ <= cfg_.holdMs)
        {
            sent = true;                                   // a read gap: the flag stays up
            o.state = CoverSenderState::Leaving;
            ++w.sentLeavingHold;
        }
        else
        {
            if (up_)
            {
                up_ = false;
                ++w.falls;
                lastFallMs_ = nowMs;
                const double held = lastRawMs_ - holdStartMs_;
                w.holdMsSum += held;
                w.holdMsMax = std::max(w.holdMsMax, held);
                crouched_ = false;
            }
            o.state = r.goingInto ? CoverSenderState::GoingInto : CoverSenderState::None;
        }
        o.sendCover = sent;
        o.sendCrouch = sent && crouched_;
        if (o.sendCover) ++w.sentCover;
        if (o.sendCrouch) ++w.sentCrouch;
        total_.samples += 1;
        if (r.inCover) ++total_.inReads;
        if (r.goingInto) ++total_.goingReads;
        if (r.aimingFrom) ++total_.aimingReads;
        if (sent) ++total_.sentCover;
        return o;
    }
    // The window since the last Take (the 10 s line), then cleared.
    CoverSenderStats Take()
    {
        CoverSenderStats w = win_;
        win_ = CoverSenderStats();
        return w;
    }
    const CoverSenderStats& Total() const { return total_; }
    bool Up() const { return up_; }
    void Reset() { *this = CoverSense(cfg_); }

private:
    Cfg cfg_;
    CoverSenderStats win_, total_;
    bool up_ = false, crouched_ = false, goingOpen_ = false, goingCounted_ = false;
    double lastRawMs_ = 0, lastFallMs_ = 0, holdStartMs_ = 0, lastGoingMs_ = 0, lastMs_ = 0;
    float zMin_ = 0, zMax_ = 0, headingFirst_ = 0, lastX_ = 0, lastY_ = 0;
};

// The sender's 10 s line (the engine prefixes nothing: this is the whole text). Empty when no cover read of any kind happened.
inline std::string CoverSenderLine(int myId, const CoverSenderStats& w)
{
    if (!w.inReads && !w.goingReads && !w.aimingReads && !w.sentCover && !w.rises)
        return std::string();
    char b[1100];
    std::snprintf(b, sizeof(b),
                  "engine: cover sender (R3 T3) id=%d, last 10 s: %u state reads; IS_PED_IN_COVER true %u, IS_PED_GOING_INTO_COVER true %u (%u with no in-cover "
                  "read), IS_PED_AIMING_FROM_COVER true %u (%u with IS_PED_IN_COVER false); kPfCover sent on %u samples (%u of them held through a read gap), "
                  "crouch hint (kPfStealth) on %u; holds %u (rises %u, falls %u, flickers %u: a rise within 600 ms of a fall), in cover mean %.1f max %.1f s; "
                  "head above the ped's root min %.2f max %.2f m (%u reads); speed while in cover max %.2f m/s (%u sliding samples over 0.5), z span max "
                  "%.2f m, heading swing max %.0f deg; going-into episodes %u (cover followed within 3 s: %u)",
                  myId, w.samples, w.inReads, w.goingReads, w.goingNoIn, w.aimingReads, w.aimNoIn, w.sentCover, w.sentLeavingHold, w.sentCrouch, w.rises,
                  w.rises, w.falls, w.flickers, w.falls ? w.holdMsSum / w.falls / 1000.0 : 0.0, w.holdMsMax / 1000.0, w.headReads ? (double)w.headMin : 0.0,
                  w.headReads ? (double)w.headMax : 0.0, w.headReads, (double)w.speedMax, w.slideSamples, (double)w.zSpanMax, (double)w.headingSwingMax,
                  w.goingEpisodes, w.goingThenIn);
    return b;
}

// ================================================================= RECEIVER ================================================
enum class CoverPhase : uint8_t { Idle, Wait, Pending, Held, Pose };
enum class CoverWhy : uint8_t
{
    None, ModePose, Unreadable, NoTolerance, NotAccepted, WrongSide, Snapped, FarAfter, Lost, Slid, Aiming, BreakerOpen, CannotCall, kCount
};
inline const char* CoverWhyName(CoverWhy w)
{
    switch (w)
    {
    case CoverWhy::ModePose:    return "cover_sync=pose";
    case CoverWhy::Unreadable:  return "ped unreadable";
    case CoverWhy::NoTolerance: return "never settled on the pose";
    case CoverWhy::NotAccepted: return "task not accepted";
    case CoverWhy::WrongSide:   return "wrong side";
    case CoverWhy::Snapped:     return "game moved it too far";
    case CoverWhy::FarAfter:    return "left far from the pose";
    case CoverWhy::Lost:        return "lost cover";
    case CoverWhy::Slid:        return "sender slid";
    case CoverWhy::Aiming:      return "sender aiming";
    case CoverWhy::BreakerOpen: return "tasks refused lately";
    case CoverWhy::CannotCall:  return "native not callable";
    default:                    return "none";
    }
}

struct CoverCfg
{
    float  issueTolM = 0.30f;       // the puppet must be within this of the pose to be issued the task (pre-placement up to here is invisible)
    float  nudgeAboveM = 0.04f;     // ... and is put exactly on the pose when further than this
    float  stillSpeedMs = 0.8f;     // the pose moves slower than this when the task goes out
    double waitMs = 900.0;          // how long a rising flag waits for tolerance before the pose fallback
    double acceptMs = 400.0;        // IS_PED_IN_COVER must read true within this
    double goingExtraMs = 600.0;    // ... plus this when IS_PED_GOING_INTO_COVER reads true
    float  acceptDistM = 0.70f;     // accepted only this close to the pose
    float  snapM = 0.45f;           // the game moved the puppet more than this when it took the task = a snap
    float  teleportM = 2.0f;        // ... more than this = a teleport
    float  wrongSideDeg = 80.0f;    // the cover faces the other way than the sender (not checked while aiming)
    double lostMs = 350.0;          // out of cover for this long while the sender is in it
    float  slideM = 0.60f;          // the sender moved off the cover point by this much ...
    double slideMs = 300.0;         // ... for this long
    int    maxIssues = 2;           // TASK_PUT_PED_DIRECTLY_INTO_COVER calls per hold
    double reissueGapMs = 1500.0;
    double minPoseMs = 1200.0;      // dwell in the fallback before a re-arm
    double endGraceMs = 150.0;      // a flag that falls and returns inside this does not end a held window
    bool   aimPose = true;          // the sender aiming from cover is shown by the pose pipeline (aim task + crouch)
};

struct CoverIn
{
    bool   flag = false;            // the sender's kPfCover (alive, on foot, not ragdolling)
    bool   aiming = false;          // the sender aims with a weapon
    float  poseSpeed = 0;           // horizontal m/s of the pose
    bool   pedReadable = false;
    float  distM = 0;               // puppet to pose, 3-D
    float  headingErrDeg = 0;
    float  movedM = 0;              // how far the puppet moved since the task was issued (Pending / Held)
    bool   pedInCover = false, pedGoingInto = false;
    double nowMs = 0;
};
struct CoverOut
{
    bool     suspend = false;       // no gait task, no velocity push, no anchor move, no nudge this frame
    bool     nudge = false;         // put the puppet exactly on the pose first (<= issueTolM away: invisible)
    bool     issue = false;         // TASK_PUT_PED_DIRECTLY_INTO_COVER now
    bool     stay = false;          // TASK_STAY_IN_COVER now
    bool     clear = false;         // ClearPedTasksNow + LocoReset, once: the task is over and the controller takes the ped back
    bool     ended = false;         // a hold with a task ended (the flag fell)
    bool     accepted = false;      // this frame IS_PED_IN_COVER was confirmed
    CoverWhy why = CoverWhy::None;  // set on the frame the window fell back to the pose
};

// Counters of one puppet (cumulative; the engine prints deltas per 10 s). The maxima are the window's own: TakeWindow clears them.
struct CoverStats
{
    uint32_t holds = 0, flagFrames = 0, ignoredHolds = 0;
    uint32_t issued = 0, nudged = 0, goingSeen = 0, accepted = 0, stays = 0, rearms = 0, ended = 0, bridged = 0;
    uint32_t snaps = 0, teleports = 0;
    uint32_t fb[(int)CoverWhy::kCount] = {};
    uint32_t errFrames = 0;
    double   goingMsSum = 0, acceptMsSum = 0;
    double   heldMs = 0, poseMs = 0, waitMs = 0;
    double   errSum = 0;
    // window maxima
    double   acceptMsMax = 0;
    float    errMax = 0, snapMax = 0;
    uint32_t fallbacks() const { uint32_t n = 0; for (int i = 1; i < (int)CoverWhy::kCount; ++i) n += fb[i]; return n; }
    void Minus(const CoverStats& o)
    {
        holds -= o.holds; flagFrames -= o.flagFrames; ignoredHolds -= o.ignoredHolds; issued -= o.issued; nudged -= o.nudged;
        goingSeen -= o.goingSeen; accepted -= o.accepted; stays -= o.stays; rearms -= o.rearms; ended -= o.ended; bridged -= o.bridged;
        snaps -= o.snaps; teleports -= o.teleports; errFrames -= o.errFrames;
        for (int i = 0; i < (int)CoverWhy::kCount; ++i) fb[i] -= o.fb[i];
        goingMsSum -= o.goingMsSum; acceptMsSum -= o.acceptMsSum; heldMs -= o.heldMs; poseMs -= o.poseMs; waitMs -= o.waitMs; errSum -= o.errSum;
    }
};

// Opens after `threshold` consecutive refusals (the game / the puppet ignores or mangles cover tasks) and sends every new hold straight to
// the pose fallback for holdMs; the first hold after that is a probe (one more refusal re-opens it, longer, up to maxHoldMs). One success closes it.
class CoverBreaker
{
public:
    explicit CoverBreaker(int threshold = 2, double holdMs = 30000.0, double maxHoldMs = 240000.0)
        : threshold_(threshold), holdMs_(holdMs), maxHoldMs_(maxHoldMs), curHold_(holdMs) {}
    bool Allow(double nowMs) const { return nowMs >= openUntil_; }
    void Report(bool ok, double nowMs)
    {
        if (ok)
        {
            fails_ = 0;
            curHold_ = holdMs_;
            return;
        }
        if (++fails_ >= threshold_)
        {
            openUntil_ = nowMs + curHold_;
            ++opens_;
            curHold_ = std::min(curHold_ * 2.0, maxHoldMs_);
            fails_ = threshold_ - 1;                      // the probe after the pause re-opens it at once if it fails
        }
    }
    uint32_t Opens() const { return opens_; }
    double OpenUntil() const { return openUntil_; }
    void Reset() { *this = CoverBreaker(threshold_, holdMs_, maxHoldMs_); }
private:
    int threshold_;
    double holdMs_, maxHoldMs_, curHold_;
    int fails_ = 0;
    double openUntil_ = 0;
    uint32_t opens_ = 0;
};

class CoverPuppet
{
public:
    explicit CoverPuppet(CoverCfg c = CoverCfg()) : cfg_(c) {}
    void SetMode(CoverSyncMode m) { mode_ = m; }
    CoverSyncMode Mode() const { return mode_; }
    const CoverCfg& Cfg() const { return cfg_; }
    CoverPhase CurrentPhase() const { return phase_; }
    CoverWhy LastWhy() const { return lastWhy_; }
    // A task could be on the ped (the readbacks are asked while true).
    bool Busy() const { return phase_ == CoverPhase::Pending || phase_ == CoverPhase::Held; }
    // The puppet's position / heading are needed this frame.
    bool NeedsPed(bool flag) const { return mode_ == CoverSyncMode::Task && (flag || phase_ != CoverPhase::Idle); }
    // A fresh ped: nothing is held, no task is on it. The counters stay (they are the player's, not the ped's).
    void Reset()
    {
        phase_ = CoverPhase::Idle;
        holdOpen_ = false;
        why_ = CoverWhy::None;
    }
    const CoverStats& Stats() const { return st_; }
    // The window since the last call as deltas (+ the window maxima), then the baseline moves on.
    CoverStats TakeWindow()
    {
        CoverStats d = st_;
        d.Minus(base_);
        base_ = st_;
        st_.acceptMsMax = 0; st_.errMax = 0; st_.snapMax = 0;
        base_.acceptMsMax = 0; base_.errMax = 0; base_.snapMax = 0;
        return d;
    }

    CoverOut Update(const CoverIn& in, CoverBreaker* br)
    {
        CoverOut o;
        const double now = in.nowMs;
        const double dt = lastMs_ > 0.0 ? std::min(std::max(now - lastMs_, 0.0), 250.0) : 0.0;
        lastMs_ = now;
        if (in.flag)
            ++st_.flagFrames;
        switch (phase_)
        {
        case CoverPhase::Idle:
            if (!in.flag)
            {
                holdOpen_ = false;
                break;
            }
            if (holdOpen_)
                break;                                           // cover_sync=off: this hold is being ignored
            holdOpen_ = true;
            ++st_.holds;
            issues_ = 0;
            stayTried_ = false;
            fellSince_ = -1.0;
            if (mode_ == CoverSyncMode::Off)
            {
                ++st_.ignoredHolds;
                break;
            }
            if (mode_ == CoverSyncMode::Pose)
            {
                ToPose(CoverWhy::ModePose, o, now, false);
                break;
            }
            if (br && !br->Allow(now))
            {
                ToPose(CoverWhy::BreakerOpen, o, now, false);
                break;
            }
            phase_ = CoverPhase::Wait;
            waitSince_ = now;
            StepWait(in, o, now);
            break;
        case CoverPhase::Wait:
            st_.waitMs += dt;
            StepWait(in, o, now);
            break;
        case CoverPhase::Pending:
            StepPending(in, o, br, now);
            break;
        case CoverPhase::Held:
            st_.heldMs += dt;
            StepHeld(in, o, br, now);
            break;
        case CoverPhase::Pose:
            st_.poseMs += dt;
            StepPose(in, o, br, now);
            break;
        }
        return o;
    }

    // TASK_PUT_PED_DIRECTLY_INTO_COVER could not be called at all (native unresolved): straight to the pose, nothing to clear.
    CoverOut NoteIssueFailed(double nowMs, CoverBreaker* br)
    {
        CoverOut o;
        if (phase_ == CoverPhase::Pending)
        {
            --st_.issued;
            if (br) br->Report(false, nowMs);
            ToPose(CoverWhy::CannotCall, o, nowMs, false);
        }
        return o;
    }

private:
    void Issue(const CoverIn& in, CoverOut& o, double now)
    {
        o.issue = true;
        o.suspend = true;
        o.nudge = in.pedReadable && in.distM > cfg_.nudgeAboveM;
        if (o.nudge) ++st_.nudged;
        ++issues_;
        ++st_.issued;
        phase_ = CoverPhase::Pending;
        issueMs_ = now;
        goingMs_ = -1.0;
        lostSince_ = slideSince_ = -1.0;
    }
    void ToPose(CoverWhy why, CoverOut& o, double now, bool clear)
    {
        phase_ = CoverPhase::Pose;
        why_ = why;
        lastWhy_ = why;
        poseSince_ = now;
        o.why = why;
        o.clear = clear;
        o.suspend = false;
        ++st_.fb[(int)why];
        rearmable_ = why == CoverWhy::Slid || why == CoverWhy::Lost || why == CoverWhy::NoTolerance || why == CoverWhy::Unreadable;
    }
    void EndHold(CoverOut& o, bool hadTask)
    {
        phase_ = CoverPhase::Idle;
        holdOpen_ = false;
        if (hadTask)
        {
            o.ended = true;
            o.clear = true;
            ++st_.ended;
        }
    }
    bool CanIssue(const CoverIn& in) const
    {
        return in.pedReadable && in.distM <= cfg_.issueTolM && in.poseSpeed <= cfg_.stillSpeedMs;
    }
    void StepWait(const CoverIn& in, CoverOut& o, double now)
    {
        if (!in.flag)
        {
            phase_ = CoverPhase::Idle;
            holdOpen_ = false;
            return;
        }
        if (in.aiming && cfg_.aimPose)
        {
            ToPose(CoverWhy::Aiming, o, now, false);
            return;
        }
        if (CanIssue(in))
        {
            Issue(in, o, now);
            return;
        }
        if (now - waitSince_ >= cfg_.waitMs)
            ToPose(in.pedReadable ? CoverWhy::NoTolerance : CoverWhy::Unreadable, o, now, false);
    }
    void StepPending(const CoverIn& in, CoverOut& o, CoverBreaker* br, double now)
    {
        if (!in.flag)
        {
            EndHold(o, true);
            return;
        }
        o.suspend = true;
        if (in.aiming && cfg_.aimPose)
        {
            ToPose(CoverWhy::Aiming, o, now, true);
            return;
        }
        if (in.pedGoingInto && goingMs_ < 0.0)
        {
            goingMs_ = now - issueMs_;
            ++st_.goingSeen;
            st_.goingMsSum += goingMs_;
        }
        if (in.pedInCover)
        {
            const bool snapped = in.movedM > cfg_.snapM;
            if (snapped)
            {
                ++st_.snaps;
                st_.snapMax = std::max(st_.snapMax, in.movedM);
                if (in.movedM >= cfg_.teleportM)
                    ++st_.teleports;
            }
            const bool tooFar = !in.pedReadable || in.distM > cfg_.acceptDistM;
            const bool wrongSide = !in.aiming && in.pedReadable && in.headingErrDeg > cfg_.wrongSideDeg;
            if (tooFar || wrongSide)
            {
                if (br)
                {
                    br->Report(false, now);
                    if (snapped)
                        br->Report(false, now);                  // a snap is visible: it counts double, one is enough to open the breaker
                }
                ToPose(wrongSide && !tooFar ? CoverWhy::WrongSide : snapped ? CoverWhy::Snapped : CoverWhy::FarAfter, o, now, true);
                return;
            }
            phase_ = CoverPhase::Held;
            heldSince_ = now;
            lostSince_ = slideSince_ = fellSince_ = -1.0;
            ++st_.accepted;
            const double ms = now - issueMs_;
            st_.acceptMsSum += ms;
            st_.acceptMsMax = std::max(st_.acceptMsMax, ms);
            o.accepted = true;
            o.suspend = true;
            if (br) br->Report(!snapped, now);
            return;
        }
        const double window = cfg_.acceptMs + (goingMs_ >= 0.0 ? cfg_.goingExtraMs : 0.0);
        if (now - issueMs_ >= window)
        {
            if (br) br->Report(false, now);
            ToPose(CoverWhy::NotAccepted, o, now, true);
        }
    }
    void StepHeld(const CoverIn& in, CoverOut& o, CoverBreaker* br, double now)
    {
        if (!in.flag)
        {
            if (fellSince_ < 0.0)
                fellSince_ = now;
            if (now - fellSince_ >= cfg_.endGraceMs)
            {
                EndHold(o, true);
                return;
            }
            o.suspend = true;
            return;
        }
        if (fellSince_ >= 0.0)
        {
            ++st_.bridged;
            fellSince_ = -1.0;
        }
        o.suspend = true;
        if (in.aiming && cfg_.aimPose)
        {
            ToPose(CoverWhy::Aiming, o, now, true);
            return;
        }
        if (in.pedReadable)
        {
            ++st_.errFrames;
            st_.errSum += in.distM;
            st_.errMax = std::max(st_.errMax, in.distM);
            if (in.distM > cfg_.slideM)
            {
                if (slideSince_ < 0.0)
                    slideSince_ = now;
                if (now - slideSince_ >= cfg_.slideMs)
                {
                    ToPose(CoverWhy::Slid, o, now, true);
                    return;
                }
            }
            else
                slideSince_ = -1.0;
        }
        if (in.pedInCover)
        {
            lostSince_ = -1.0;
            return;
        }
        if (lostSince_ < 0.0)
            lostSince_ = now;
        if (now - lostSince_ < cfg_.lostMs)
            return;
        if (!stayTried_)
        {
            stayTried_ = true;
            o.stay = true;
            ++st_.stays;
            lostSince_ = now;                                    // judged again after another lostMs
        }
        else if (issues_ < cfg_.maxIssues && CanIssue(in) && now - issueMs_ >= cfg_.reissueGapMs)
        {
            Issue(in, o, now);
            ++st_.rearms;
        }
        else
            ToPose(CoverWhy::Lost, o, now, true);
        (void)br;
    }
    void StepPose(const CoverIn& in, CoverOut& o, CoverBreaker* br, double now)
    {
        if (!in.flag)
        {
            phase_ = CoverPhase::Idle;
            holdOpen_ = false;
            return;
        }
        if (mode_ != CoverSyncMode::Task || !rearmable_ || issues_ >= cfg_.maxIssues || now - poseSince_ < cfg_.minPoseMs)
            return;
        if (in.aiming || !CanIssue(in) || (br && !br->Allow(now)) || now - issueMs_ < cfg_.reissueGapMs)
            return;
        Issue(in, o, now);
        ++st_.rearms;
    }

    CoverCfg cfg_;
    CoverSyncMode mode_ = CoverSyncMode::Task;
    CoverPhase phase_ = CoverPhase::Idle;
    CoverWhy why_ = CoverWhy::None, lastWhy_ = CoverWhy::None;
    CoverStats st_, base_;
    bool holdOpen_ = false, stayTried_ = false, rearmable_ = false;
    int issues_ = 0;
    double lastMs_ = 0, waitSince_ = 0, issueMs_ = -1e9, goingMs_ = -1.0, heldSince_ = 0, lostSince_ = -1.0, slideSince_ = -1.0, fellSince_ = -1.0,
           poseSince_ = 0;
};

// The per-puppet 10 s line. `d` is TakeWindow(). Empty when the sender's cover flag was never up and nothing was counted.
inline std::string CoverPuppetLine(int id, const std::string& name, CoverSyncMode mode, CoverPhase phase, CoverWhy lastWhy, const CoverStats& d,
                                   const CoverStats& total)
{
    if (!d.flagFrames && !d.holds && !d.issued)
        return std::string();
    std::string why;
    for (int i = 1; i < (int)CoverWhy::kCount; ++i)
        if (d.fb[i])
        {
            char t[96];
            std::snprintf(t, sizeof(t), "%s%s %u", why.empty() ? "" : ", ", CoverWhyName((CoverWhy)i), d.fb[i]);
            why += t;
        }
    if (why.empty())
        why = "none";
    static const char* kPhase[] = { "idle", "waiting for the puppet to settle on the pose", "task pending", "in cover (verified)", "crouch pose fallback" };
    char b[1500];
    std::snprintf(b, sizeof(b),
                  "engine: cover puppet (R3 T3) id=%d (%s), cover_sync=%s, last 10 s: the sender's cover flag on %u pose frames in %u holds (%u ignored by "
                  "cover_sync=off); TASK_PUT_PED_DIRECTLY_INTO_COVER issued %u (puppet put exactly on the pose first %u, never more than %.2f m), "
                  "IS_PED_GOING_INTO_COVER seen %u (mean %.0f ms after the task), accepted (IS_PED_IN_COVER) %u (mean %.0f max %.0f ms), TASK_STAY_IN_COVER "
                  "%u, re-issues %u; fell back to the crouch pose on the sender's position %u [%s]; game moved the puppet more than %.2f m (snaps) %u "
                  "(max %.2f m), teleports %u; held cover through a flag gap %u, ended %u; time in cover %.1f s, on the pose fallback %.1f s, waiting %.1f s; "
                  "puppet-to-pose error while in cover mean %.2f max %.2f m (%u frames); now: %s%s%s (session: %u holds, %u accepted, %u fallbacks)",
                  id, name.c_str(), CoverSyncName(mode), d.flagFrames, d.holds, d.ignoredHolds, d.issued, d.nudged, (double)CoverCfg().issueTolM, d.goingSeen,
                  d.goingSeen ? d.goingMsSum / d.goingSeen : 0.0, d.accepted, d.accepted ? d.acceptMsSum / d.accepted : 0.0, d.acceptMsMax, d.stays, d.rearms,
                  d.fallbacks(), why.c_str(), (double)CoverCfg().snapM, d.snaps, (double)d.snapMax, d.teleports, d.bridged, d.ended, d.heldMs / 1000.0,
                  d.poseMs / 1000.0, d.waitMs / 1000.0, d.errFrames ? d.errSum / d.errFrames : 0.0, (double)d.errMax, d.errFrames,
                  kPhase[(int)phase], phase == CoverPhase::Pose ? " because " : "", phase == CoverPhase::Pose ? CoverWhyName(lastWhy) : "", total.holds,
                  total.accepted, total.fallbacks());
    return b;
}

}  // namespace gage::sync
