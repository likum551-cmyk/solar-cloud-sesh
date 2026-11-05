// Remote-ped locomotion (see locomotion.h for the design and its constants).

#include "locomotion.h"

#include <cmath>

namespace gage::sync {

namespace {

    double WrapDeg180(double d)
    {
        d = std::fmod(d, 360.0);
        if (d > 180.0) d -= 360.0;
        else if (d <= -180.0) d += 360.0;
        return d;
    }

    float WrapDeg360(double d)
    {
        d = std::fmod(d, 360.0);
        if (d < 0.0) d += 360.0;
        if (d >= 360.0) d -= 360.0; // fmod of a tiny negative can round to 360
        return static_cast<float>(d);
    }

    bool Finite(const LocoVec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    double HLen(double x, double y) { return std::sqrt(x * x + y * y); }

    float BlendRatio(Gait g) { return static_cast<float>(static_cast<int>(g)); } // 1 walk, 2 run, 3 sprint
    constexpr double kClipTrimMax = 0.25; // 0.4.17 blendGaitClip: the sender's clip only while the blend trim stays within this (writes honoured)

    double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // The speed an unscaled gait animation covers (move rate 1).
    double Nominal(Gait g, const LocoConfig& c)
    {
        return g == Gait::Walk ? c.nominalWalk : g == Gait::Run ? c.nominalRun : c.nominalSprint;
    }

    // Without SET_PED_MOVE_RATE_OVERRIDE: the move blend ratio that shows `speed`. 0..1 is still..walk,
    // 1..2 walk..run, 2..3 run..sprint (the nominal speeds are the integer points), kept within half a
    // step of the gait's own integer so the gait stays unambiguous, quantised to blendStep.
    float QuantisedBlend(double speed, Gait g, const LocoConfig& c)
    {
        double b;
        if (speed < c.nominalWalk) b = speed / c.nominalWalk;
        else if (speed < c.nominalRun) b = 1.0 + (speed - c.nominalWalk) / (c.nominalRun - c.nominalWalk);
        else b = 2.0 + (speed - c.nominalRun) / (c.nominalSprint - c.nominalRun);
        const double base = static_cast<double>(static_cast<int>(g));
        b = Clamp(b, base - 0.5, base + 0.5);
        b = Clamp(b, 0.0, 3.0);
        const double step = c.blendStep > 0.0f ? c.blendStep : 0.25;
        return static_cast<float>(std::floor(b / step + 0.5) * step);
    }

    const double kPi = 3.14159265358979323846;
    const double kDirMinSpeedMs = 0.2; // below this the velocity has no usable direction: use the heading
    const double kHeldAnimMs = 300.0;  // a read that was our own push: the last clean one is held this long, then the model

} // namespace

LocomotionController::LocomotionController(const LocoConfig& cfg) : cfg_(cfg) {}

void LocomotionController::Reset()
{
    // Same ped, something else touched it: re-issue the gait task at once but keep started_, so the next
    // Update does not snap the ped onto the (delayed) pose. A real divergence still snaps (distance and
    // persistent-error rules).
    lastTask_ = LocoTask::None;
    forceIssue_ = true;
    resetForced_ = true;
    reevaluate_ = false;
    offAcc_ = 0;
    mismatchMs_ = 0;
    steadyMs_ = 0;
    ++stats_.resets;
    OpenGrace();   // R5: the task list was replaced by someone else: the new task's start clip is not the writes failing (Blend only)
}

// R5: a task-changing event (Reset() from any caller, the end of a ragdoll, a landing): in Blend the self-check accrues no failure for blendGraceMs
// and its fight statistics start afresh. A chain of events re-arms the window but never extends it past blendGraceMaxMs from its first opening.
void LocomotionController::OpenGrace()
{
    if (cfg_.mode != LocoMode::Blend || !cfg_.selfCheck || cfg_.blendGraceMs <= 0.0f || !started_) return;
    if (graceCooldownMs_ > 0.0)
    {
        ++stats_.graceChainCapped;
        return;
    }
    if (graceLeftMs_ > 0.0 && graceChainMs_ >= cfg_.blendGraceMaxMs)
    {
        ++stats_.graceChainCapped;
        graceCooldownMs_ = cfg_.blendGraceMaxMs;
        return;
    }
    if (graceLeftMs_ <= 0.0) graceChainMs_ = 0.0;
    graceLeftMs_ = cfg_.blendGraceMs;
    graceEnded_ = false;
    sinceGraceMs_ = 0.0;
    ++stats_.graceWindows;
    // what the old task did to the writes is not evidence about the new one
    fightF_ = fightN_ = fightShare_ = 0.0;
    blendFailMs_ = 0.0;
    closeRate_ = 0.0;
    havePrevErr_ = false;
}

void LocomotionController::SoftReset()
{
    // Keep lastTask_ and its target: Update decides whether the running task still serves.
    reevaluate_ = true;
    ++stats_.softResets;
}

void LocomotionController::ResetHard()
{
    started_ = false;                     // (before Reset(): a hard reset snaps onto the pose, there is no error to wait out, so no grace window opens)
    Reset();
    graceLeftMs_ = graceChainMs_ = graceCooldownMs_ = 0.0;   // R5: ... and one that was open is dropped
    graceEnded_ = false;
    haveVf_ = false;
    pauseMs_ = 0;
    gait_ = inferred_ = pendingGait_ = gaitShown_ = Gait::Still;
    stillMs_ = pendingMs_ = 0;
    corrFx_ = corrFy_ = 0.0f;
    settling_ = false;
    pushedLast_ = false;
    animEst_ = LocoVec();
    sinceMoveStartMs_ = 1e9;
    havePrevPed_ = false;
    haveSelf_ = false;
    lagMs_ = 0.0;
    haveVfPrev_ = false;
    accLp_ = 0.0;
    quietActive_ = false;      // R3 T2: a window of the old ped is nobody's to judge
    quietInitial_ = false;
    quietLeftMs_ = 0.0;
    harmlessEvidMs_ = 0.0;
    sinceIssueGaitMs_ = -1.0;
    blendFailMs_ = 0.0;         // R4 T1: a new ped / a hard reset restarts the blend's self-check and its trim
    blendTrim_ = 0.0f;
    corrAlongLp_ = 0.0;
    closeRate_ = 0.0;
    havePrevErr_ = false;
    fightShare_ = 0.0;
    fightF_ = fightN_ = 0.0;
    lastBlendWrite_ = false;
}

void LocomotionController::Pause(double ms) { pauseMs_ = ms > 0.0 ? ms : 0.0; }

Gait LocomotionController::ApplyAuthority(Gait inferred, int moveState)
{
    if (moveState < 0 || moveState > 3) return inferred;
    const int lo = static_cast<int>(inferred);
    const int hi = lo + 1 > 3 ? 3 : lo + 1;
    return static_cast<Gait>(moveState < lo ? lo : (moveState > hi ? hi : moveState));
}

void LocomotionController::ResetForNewPed()
{
    ResetHard();
    // R3 T2: a NEW ped (also the one a repair created) starts at the top of the ladder; what is known about coordinate writes, the
    // number of level changes and the cap stay with the puppet.
    if (!repairRespawn_) anchorFallbackLeftMs_ = 0.0;   // (a repair's respawn keeps the straight start it asked for)
    repairRespawn_ = false;
    rateOffLeftMs_ = placedLeftMs_ = 0.0;
    anchorFailStreak_ = rateFailStreak_ = placedFailStreak_ = 0;
    stallEvents_ = 0;
    lastIssueWasStall_ = false;
    snapStorm_ = false;
    failSnaps_ = 0;
    mismatchMs_ = 0.0;
    noGaitSource_ = false;      // R4 T1: a new ped may have a working gait source
    deadStallStreak_ = 0;
    ragdollActive_ = false;
    haveJumpSeq_ = false;
    lastJumpSeq_ = 0;
    sinceJumpMs_ = 1e9;
    wasJumping_ = false;
}

Gait LocomotionController::InferGait(Gait cur, float speed, const LocoConfig& c)
{
    if (!std::isfinite(speed)) return cur;
    const float up[3] = {c.idleUp, c.walkRunUp, c.runSprintUp};       // leave g upward at up[g]
    const float down[4] = {0.0f, c.idleDown, c.walkRunDown, c.runSprintDown}; // leave g downward below down[g]
    int g = static_cast<int>(cur);
    while (g < 3 && speed >= up[g]) ++g;
    while (g > 0 && speed < down[g]) --g;
    return static_cast<Gait>(g);
}

LocoOutput LocomotionController::Update(const LocoInput& in)
{
    LocoOutput o = UpdateImpl(in);
    pushedLast_ = o.setVelocity;      // Sync A: next frame's read of the ped's velocity may be this push (see animEst_)
    if (o.setVelocity) lastPush_ = o.velocity;
    // T2: the baseline of the ped's own movement next frame: where it stands after what we wrote this frame (a snap or a nudge).
    if (std::isfinite(in.pedPos.x) && std::isfinite(in.pedPos.y) && std::isfinite(in.pedPos.z))
    {
        prevPedAfter_ = o.snap ? o.snapPos : in.pedPos;
        if (o.nudge && !o.snap)
        {
            prevPedAfter_.x += o.nudgeDelta.x;
            prevPedAfter_.y += o.nudgeDelta.y;
            prevPedAfter_.z += o.nudgeDelta.z;
        }
        havePrevPed_ = true;
    }
    // T2: the ladder. The level is derived from the timers the self-checks and the stall rule set; a change is reported once.
    LocoLevel cur = LocoLevel::Full;
    if (cfg_.selfCheck)
    {
        if (placedLeftMs_ > 0.0) cur = LocoLevel::Placed;
        else if (rateOffLeftMs_ > 0.0) cur = LocoLevel::NoRate;
        else if (cfg_.mode == LocoMode::Anchor && anchorFallbackLeftMs_ > 0.0) cur = LocoLevel::Straight;
    }
    o.level = cur;
    if (cur != level_)
    {
        o.levelChanged = true;
        o.levelWhy = static_cast<int>(cur) < static_cast<int>(level_) ? LocoLevelWhy::Recovered : pendingWhy_;
        if (cfg_.mode == LocoMode::Blend && level_ == LocoLevel::Placed && cur != LocoLevel::Placed && cfg_.blendProbationMs > 0.0f)
        {
            // R5: the way back up from the rigid follow is a probe: a short leash and fresh evidence
            probationLeftMs_ = cfg_.blendProbationMs;
            ++stats_.probations;
            fightF_ = fightN_ = fightShare_ = 0.0;
            blendFailMs_ = 0.0;
            closeRate_ = 0.0;
            havePrevErr_ = false;
        }
        ++stats_.levelChanges;
        ++stats_.levelWhy[static_cast<int>(o.levelWhy)];
        level_ = cur;
        if (cfg_.selfCheck)
        {
            ++levelChangesTotal_;                     // R3 T2: the cap counts, the hold starts
            levelHoldLeftMs_ = cfg_.levelHoldMs;
            if (levelChangesTotal_ >= cfg_.maxLevelChanges && !ladderFrozen_)
            {
                ladderFrozen_ = true;                 // the cap: this level stays for the rest of the session (a line says so once)
                capHitPending_ = true;
            }
        }
    }
    pendingWhy_ = LocoLevelWhy::None;
    // R3 T2: coordinate writes this controller asked for (the quiet window and the verdict need to know how long ago), and the
    // outputs the engine logs / acts on.
    {
        const double dtw = in.dtMs < 0.0 ? 0.0 : (in.dtMs > 1000.0 ? 1000.0 : in.dtMs);
        if (o.nudge || o.snap) sinceWriteMs_ = 0.0;
        else sinceWriteMs_ += dtw;
        o.quiet = quietActive_;
        o.writeVerdict = writeVerdict_;
        o.verdictChanged = verdictChangedPending_;
        o.capHit = capHitPending_;
        // (R4 T1: a repair is only ever asked for in the evaluation that a melee swing skips, so none is pending while one plays)
        o.repair = repairPending_;
        verdictChangedPending_ = capHitPending_ = false;
        repairPending_ = LocoRepair::None;
    }
    if (o.blend && o.setVelocity)
    {
        ++stats_.blendFrames;
        if (o.corrMs > stats_.maxCorrMs) stats_.maxCorrMs = o.corrMs;
    }
    lastBlendWrite_ = o.blend && o.setVelocity && !o.snap;
    return o;
}

bool LocomotionController::LadderMayChange(bool deeper)
{
    // R3 T2: the cap and the hold of the ladder. A deeper step of the failure being handled is allowed through the hold (the ladder
    // is monotone downward within one failure); anything else waits for the hold; nothing passes the cap.
    if (!cfg_.selfCheck) return true;
    if (ladderFrozen_) { ++stats_.changesRefused; return false; }
    if (!deeper && levelHoldLeftMs_ > 0.0) { ++stats_.changesRefused; return false; }
    return true;
}

void LocomotionController::Degrade(LocoLevel to, LocoLevelWhy why, double ms)
{
    // The engine passes on what another puppet learned (the game is the same game for all of them): a level, for a while.
    if (!cfg_.selfCheck || ms <= 0.0) return;
    if (level_ != LocoLevel::Full ? static_cast<int>(to) <= static_cast<int>(level_) : false) return;   // R3: already at or below it
    if (!LadderMayChange(true)) return;
    if (to == LocoLevel::Straight && cfg_.mode == LocoMode::Anchor)
    {
        if (anchorFallbackLeftMs_ < ms) anchorFallbackLeftMs_ = ms;
        pendingWhy_ = why;
    }
    else if (to == LocoLevel::NoRate)
    {
        if (rateOffLeftMs_ < ms) rateOffLeftMs_ = ms;
        pendingWhy_ = why;
    }
    else if (to == LocoLevel::Placed && cfg_.mode == LocoMode::Blend)
    {
        // R4 T1: two puppets found that the velocity writes do not carry a ped here: the rigid follow, without finding out again
        // R5: ... for no longer than this controller's own doubling cap for falls nobody explained (the witness imposes 45 s; the cap is 300 s, so nothing changes there)
        const double capMs = cfg_.placedUnexplainedMaxMs > 0.0f ? double(cfg_.placedUnexplainedMaxMs) : double(cfg_.placedDurMaxMs);
        if (ms > capMs) ms = capMs;
        if (placedLeftMs_ < ms) placedLeftMs_ = ms;
        pendingWhy_ = why;
    }
}

LocoConfig ShippedLocoConfig(bool anchor)
{
    LocoConfig c;
    c.mode = anchor ? LocoMode::Anchor : LocoMode::Straight;
    c.push = PushMode::Nudge;
    c.selfCheck = true;
    c.fastDownFrac = 0.6f;
    return c;
}

LocoConfig ShippedLocoConfig(LocoMode mode)
{
    if (mode != LocoMode::Blend)
    {
        LocoConfig c = ShippedLocoConfig(mode == LocoMode::Anchor);
        c.push = mode == LocoMode::Anchor ? PushMode::Nudge : PushMode::Velocity;   // 0.4.8 = anchor + nudge, 0.4.9 = straight + velocity
        return c;
    }
    LocoConfig c;
    c.mode = LocoMode::Blend;
    c.push = PushMode::Velocity;       // (unused: Blend has its own drive)
    c.selfCheck = true;
    c.fastDownFrac = 0.6f;
    // the velocity writes are the mode: nothing to prove harmless, nothing the engine could repair about them
    c.quietTrial = false;
    c.maxRepairs = 0;
    // a moving gait holds at least 400 ms, a change UP no sooner than a change down (no start-window exemption)
    c.gaitDebounceMs = 400.0f;
    c.upDebounceMs = 400.0f;
    c.startWindowMs = 0.0f;
    c.gaitMinHoldMs = 400.0f;
    c.stallFirstMs = 1500.0f;
    // the gait task's target is a FAR point along the pose velocity (the bearing from the ped must stay within a few degrees of the pose's
    // direction for the whole life of a task: a nearer target swings the task's heading sideways as the ped closes on it and, when the game
    // only half-honours the velocity written, drags the ped off the line)
    c.leadS = 8.0f;
    c.leadMinM = 12.0f;
    c.leadMaxM = 40.0f;
    // R5 (locomotion.h "11. R5"): a grace window after a task-changing event, and a rigid follow that lasts seconds, not half a minute
    c.blendGraceMs = 2000.0f;
    c.blendGracePlaceM = 1.0f;
    c.blendGraceMaxMs = 6000.0f;
    c.blendProbationMs = 4000.0f;
    c.blendProbationFailMs = 300.0f;
    c.blendProbationErrScale = 0.6f;
    c.blendAttributeMs = 4000.0f;
    c.blendTrimGateM = 1.0f;
    c.placedExplainedResetMs = 30000.0f;
    c = WithPlacedHold(c, 6000);
    c.maxLevelChanges = 60;
    return c;
}

LocoConfig WithPlacedHold(LocoConfig c, int ms)
{
    if (ms <= 0)
    {
        c.placedMs = 30000.0f;
        c.placedDurMaxMs = 300000.0f;
        c.levelHoldMs = 8000.0f;
        c.placedUnexplainedMs = 0.0f;
        c.placedUnexplainedMaxMs = 0.0f;
        return c;
    }
    c.placedMs = static_cast<float>(ms);
    c.placedDurMaxMs = 4.0f * static_cast<float>(ms);
    c.placedUnexplainedMs = 30000.0f;
    c.placedUnexplainedMaxMs = 300000.0f;
    c.levelHoldMs = std::fmin(8000.0f, 0.6f * static_cast<float>(ms));
    return c;
}

LocoConfig Round4SelfCheck(LocoConfig c)
{
    c.blendGraceMs = 0.0f;
    c.blendProbationMs = 0.0f;
    c.blendTrimGateM = 0.0f;
    c.placedExplainedResetMs = 0.0f;
    c = WithPlacedHold(c, 0);
    c.maxLevelChanges = 12;
    return c;
}

LocoConfig Round2Behaviour(LocoConfig c)
{
    c.quietTrial = false;
    c.lostNeedsLowSpeed = false;
    c.reissueMinMs = 0.0f;
    c.turnBigMinMs = 1e9f;
    c.firstGaitWindowMs = 0.0f;
    c.issueGapMs = 0.0f;
    c.levelHoldMs = 0.0f;
    c.maxLevelChanges = 1 << 20;
    c.maxRepairs = 0;
    c.deadPlacedMs = c.placedMs;
    c.deadPlacedMaxMs = c.placedDurMaxMs;
    return c;
}

LocoOutput LocomotionController::UpdateImpl(const LocoInput& in)
{
    LocoOutput o;
    ++stats_.frames;
    if (!Finite(in.pos) || !Finite(in.vel) || !Finite(in.pedPos) || !std::isfinite(in.heading) ||
        !std::isfinite(in.pedHeading) || !std::isfinite(in.dtMs))
    {
        o.gait = gait_;
        return o; // nothing trustworthy to act on
    }
    const double dt = in.dtMs < 0.0 ? 0.0 : (in.dtMs > 1000.0 ? 1000.0 : in.dtMs);
    const double dtS = dt / 1000.0;
    sinceIssueMs_ += dt;
    sinceJumpMs_ += dt;
    steadyMs_ += dt;
    sinceMoveStartMs_ += dt;
    sinceAnchorStallMs_ += dt;
    gaitHeldMs_ += dt;
    // R4 T1: a melee swing plays: tasks and repairs are held for as long as the engine says so and swingHoldMs after
    if (in.swingActive) swingHoldLeftMs_ = cfg_.swingHoldMs;
    else if (swingHoldLeftMs_ > 0.0) swingHoldLeftMs_ = swingHoldLeftMs_ > dt ? swingHoldLeftMs_ - dt : 0.0;
    const bool swingBlocks = swingHoldLeftMs_ > 0.0;
    const bool blendCfg = cfg_.mode == LocoMode::Blend;
    // R3 T2: a fallback timer that runs out is a level change UP (Recovered): it waits for the hold after the last change and never
    // happens once the cap froze the ladder (the timer then rests at 1 ms: the level stays).
    const bool ladderHolds = cfg_.selfCheck && (ladderFrozen_ || levelHoldLeftMs_ > dt);
    if (cfg_.selfCheck && levelHoldLeftMs_ > 0.0) levelHoldLeftMs_ = levelHoldLeftMs_ > dt ? levelHoldLeftMs_ - dt : 0.0;
    sinceForcedIssueMs_ += dt;
    pulseSinceMs_ += dt;
    auto runDown = [&](double& timer) {
        if (timer <= 0.0) return;
        timer -= dt;
        if (timer <= 0.0) timer = ladderHolds ? 1.0 : 0.0;
    };
    runDown(anchorFallbackLeftMs_);
    // T2: the fallback timers of the ladder run on, and the ped's own displacement is measured (position differences net of
    // what we wrote last frame): the one signal that needs no model of what SET_ENTITY_VELOCITY or a task really does.
    sinceRateFailMs_ += dt;
    sincePlacedMs_ += dt;
    runDown(rateOffLeftMs_);
    runDown(placedLeftMs_);
    const bool placedNow = cfg_.selfCheck && placedLeftMs_ > 0.0;   // (R4 T1: after its timer ran down, so the whole frame agrees on the level)
    if (quietActive_) { quietLeftMs_ -= dt; quietElapsedMs_ += dt; }
    if (quietInitial_)
    {
        quietInitialLeftMs_ -= dt;
        if (quietInitialLeftMs_ <= 0.0 || writeVerdict_ != WriteVerdict::Unknown) quietInitial_ = false;
    }
    if (havePrevPed_ && !in.teleported && dt >= 4.0 && dt <= 250.0)
    {
        const double ivx = (double(in.pedPos.x) - prevPedAfter_.x) / dtS, ivy = (double(in.pedPos.y) - prevPedAfter_.y) / dtS;
        if (HLen(ivx, ivy) < 30.0)
        {
            if (!haveSelf_) { selfV_.x = static_cast<float>(ivx); selfV_.y = static_cast<float>(ivy); haveSelf_ = true; }
            else
            {
                const double k = 1.0 - std::exp(-dt / 120.0);
                selfV_.x = static_cast<float>(selfV_.x + (ivx - selfV_.x) * k);
                selfV_.y = static_cast<float>(selfV_.y + (ivy - selfV_.y) * k);
            }
        }
    }
    else if (dt > 250.0 || in.teleported) haveSelf_ = false;
    // W11 item 2: anchor mode now? (the engine reports no anchor entity or an aim task with noAnchor; a stall dropped it)
    bool anchorMode = cfg_.mode == LocoMode::Anchor && !in.noAnchor && anchorFallbackLeftMs_ <= 0.0 &&
                      !(cfg_.selfCheck && placedLeftMs_ > 0.0);
    // T2: SET_PED_MOVE_RATE_OVERRIDE usable: the engine says it can be called, and the ladder has not found it ineffective.
    const bool rateLadderOff = cfg_.selfCheck && rateOffLeftMs_ > 0.0;   // found ineffective: the blend ratio rides on the running task instead
    const bool rateAvail = in.moveRateAvailable && !rateLadderOff;

    // ---- pause: the ped is busy with something else (leaving a vehicle) ----
    if (pauseMs_ > 0.0)
    {
        pauseMs_ -= dt;
        if (pauseMs_ > 0.0)
        {
            o.gait = gait_;
            o.paused = true;
            return o;
        }
        pauseMs_ = 0.0;
        Reset();            // re-issue the gait task; the ped keeps where it is (a real divergence still snaps)
        haveVf_ = false;    // the velocity filter is stale after the pause
    }

    // R5: the grace window after a task-changing event runs down (not during a Pause: the ped is not being judged then)
    if (graceLeftMs_ > 0.0)
    {
        graceChainMs_ += dt;
        graceLeftMs_ -= dt;
        sinceGraceMs_ = 0.0;
        if (graceLeftMs_ <= 0.0)
        {
            graceLeftMs_ = 0.0;
            graceEnded_ = true;
        }
    }
    else sinceGraceMs_ += dt;
    if (graceCooldownMs_ > 0.0) graceCooldownMs_ = graceCooldownMs_ > dt ? graceCooldownMs_ - dt : 0.0;
    if (probationLeftMs_ > 0.0) probationLeftMs_ = probationLeftMs_ > dt ? probationLeftMs_ - dt : 0.0;

    // ---- pose velocity, low-passed ----
    // The sender's GET_ENTITY_VELOCITY carries the stride ripple; the steer and the lead target use a
    // one-pole low-pass of it (tau velTauMs). Seeded on the first frame, after a teleport and after a
    // hard reset, and a stop (raw speed under idleDown) is not smoothed: the ped must not coast on past
    // the pose. Vertical velocity is passed through (a jump's vz must not lag).
    const double rawSpeed = HLen(in.vel.x, in.vel.y);
    if (!haveVf_ || !started_ || in.teleported || rawSpeed < cfg_.idleDown)
    {
        vf_.x = in.vel.x;
        vf_.y = in.vel.y;
    }
    else
    {
        const double k = 1.0 - std::exp(-dt / static_cast<double>(cfg_.velTauMs > 0 ? cfg_.velTauMs : 1.0f));
        vf_.x = static_cast<float>(vf_.x + (double(in.vel.x) - vf_.x) * k);
        vf_.y = static_cast<float>(vf_.y + (double(in.vel.y) - vf_.y) * k);
    }
    vf_.z = in.vel.z;
    haveVf_ = true;
    // T2: how fast the pose's velocity changes (a start, a stop, a reversal, a turn: not steady motion). The low-passed velocity
    // against its own 600 ms average: a stride ripple (10% at 1.9 Hz) hardly shows, a real acceleration a shows as ~a.
    if (haveVfPrev_ && !in.teleported)
    {
        const double ks = 1.0 - std::exp(-dt / 600.0);
        vfPrev_.x = static_cast<float>(vfPrev_.x + (double(vf_.x) - vfPrev_.x) * ks);
        vfPrev_.y = static_cast<float>(vfPrev_.y + (double(vf_.y) - vfPrev_.y) * ks);
        accLp_ = HLen(double(vf_.x) - vfPrev_.x, double(vf_.y) - vfPrev_.y) / 0.6;
    }
    else
    {
        vfPrev_ = vf_;
        accLp_ = 0.0;
    }
    haveVfPrev_ = true;
    sinceStallEventMs_ += dt;
    sinceFailSnapMs_ += dt;

    // ---- gait: speed first, the sender's gait only as a hint ----
    // inferred_ is the speed's gait with hysteresis. The sender's moveState (0 for swimming, climbing,
    // cover, strafing: PedGait() has no gait for them) may only lift it by one: ApplyAuthority. Going
    // Still needs the speed to stay under idleDown for stillHoldMs (a one-sample dip must not fire
    // StandStill then GoStraight), and a change between moving gaits has to hold for gaitDebounceMs
    // (leaving Still is immediate: a ped sliding in an idle pose is the worse error).
    inferred_ = InferGait(inferred_, static_cast<float>(rawSpeed), cfg_);
    Gait cand = ApplyAuthority(inferred_, in.moveState);
    bool holding = false, settling = false;
    // Sync A: how far the ped is from the pose (the horizontal distance, before any snap): a stop is not over until it is there.
    const double pedErrH = HLen(double(in.pos.x) - in.pedPos.x, double(in.pos.y) - in.pedPos.y);
    if (cand == Gait::Still && gait_ != Gait::Still)
    {
        stillMs_ += dt;
        if (stillMs_ < cfg_.stillHoldMs) { cand = gait_; holding = true; }
        else if (cfg_.settleMaxMs > 0.0f && !in.teleported && !in.swimming && !in.ragdoll && !in.jumping &&
                 pedErrH > (settling_ ? cfg_.settleDoneM : cfg_.settleM) && stillMs_ < cfg_.stillHoldMs + cfg_.settleMaxMs &&
                 pedErrH < cfg_.snapBaseM)
        {
            // The pose has stopped and the ped has not reached it: it walks the rest (the task keeps running, the anchor is
            // on the pose) instead of standing still where it is and being dragged there in an idle pose.
            cand = Gait::Walk;
            settling = true;
            gait_ = Gait::Walk;
            pendingGait_ = Gait::Walk;
            pendingMs_ = 0;
            if (!settling_) { ++stats_.settles; steadyMs_ = 0; if (cfg_.mode == LocoMode::Straight || in.noAnchor) forceIssue_ = true; }
        }
    }
    else stillMs_ = 0;
    settling_ = settling;
    if (!settling && cand != gait_ && cand != Gait::Still && gait_ != Gait::Still)
    {
        // A change DOWN holds gaitDebounceMs; a change UP only upDebounceMs, and not at all in the first startWindowMs of
        // a movement (the first frames of a start read a walking speed while the pose is already running).
        const bool up = static_cast<int>(cand) > static_cast<int>(gait_);
        // (only the SPEED's own gait is trusted that fast: a gait the sender's hint lifted above it holds gaitDebounceMs)
        const bool speedUp = up && static_cast<int>(cand) <= static_cast<int>(inferred_);
        double need = speedUp ? (sinceMoveStartMs_ < cfg_.startWindowMs ? 0.0 : cfg_.upDebounceMs) : cfg_.gaitDebounceMs;
        // T2: a pose that has fallen far under the band of the gait it is shown in (a stop, a reversal) must not keep the faster
        // gait for the whole debounce: the ped ran on ~0.4 s at the smallest stride of the faster gait, past the pose and back.
        if (!up && cfg_.fastDownFrac > 0.0f)
        {
            const double downEdge = gait_ == Gait::Sprint ? cfg_.runSprintDown : gait_ == Gait::Run ? cfg_.walkRunDown : 0.0;
            if (downEdge > 0.0 && rawSpeed < cfg_.fastDownFrac * downEdge && need > cfg_.fastDownMs) need = cfg_.fastDownMs;
        }
        if (cand != pendingGait_) { pendingGait_ = cand; pendingMs_ = 0; }
        pendingMs_ += dt;
        if (pendingMs_ < need) cand = gait_;
    }
    else if (!settling) { pendingGait_ = gait_; pendingMs_ = 0; }
    if (!settling)
    {
        // R4 T1: a moving gait, once shown, holds gaitMinHoldMs (a pose speed that dips across a band edge for a moment must not make the
        // animation flap); leaving Still is never held and neither is a teleport
        if (cfg_.gaitMinHoldMs > 0.0f && cand != gait_ && gait_ != Gait::Still && gaitHeldMs_ < cfg_.gaitMinHoldMs && !in.teleported && started_)
            cand = gait_;
        if (gait_ == Gait::Still && cand != Gait::Still) sinceMoveStartMs_ = 0;
        if (cand != gait_) gaitHeldMs_ = 0.0;
        gait_ = cand;
    }
    if (gait_ != gaitPrev_) { steadyMs_ = 0; gaitPrev_ = gait_; }
    o.gait = gait_;

    // ---- jump / ragdoll edges (v3) ----
    if (in.jumpSeq >= 0)
    {
        if (haveJumpSeq_ && in.jumpSeq != lastJumpSeq_)
        {
            o.jumpStart = true;
            sinceJumpMs_ = 0;
            ++stats_.jumps;
        }
        haveJumpSeq_ = true;
        lastJumpSeq_ = in.jumpSeq;
    }
    const bool jumping = in.jumping || sinceJumpMs_ < cfg_.jumpGraceMs;
    if (wasJumping_ && !jumping) { forceIssue_ = true; OpenGrace(); } // landed: the jump replaced the walk task (R5: its start clip is not the writes failing)
    wasJumping_ = jumping;

    if (in.ragdoll && !ragdollActive_) { o.ragdollStart = true; ragdollActive_ = true; }
    else if (!in.ragdoll && ragdollActive_) { o.ragdollEnd = true; ragdollActive_ = false; forceIssue_ = true; OpenGrace(); }

    // ---- snap ----
    LocoVec ped = in.pedPos;
    const double ex = double(in.pos.x) - ped.x, ey = double(in.pos.y) - ped.y, ez = double(in.pos.z) - ped.z;
    const double dist = std::sqrt(ex * ex + ey * ey + ez * ez);
    const double speed3 = std::sqrt(double(in.vel.x) * in.vel.x + double(in.vel.y) * in.vel.y + double(in.vel.z) * in.vel.z);
    bool snap = !started_ || in.teleported || dist > (blendCfg ? double(cfg_.blendSnapM) : cfg_.snapBaseM + cfg_.snapPerSpeedS * speed3);
    // R5: the grace window closed this frame with the ped still well off the pose: placed ONCE (a coordinate set; the gait task keeps running and
    // the blend goes on). Not a failure snap: it is neither a hard snap nor a count toward a snap storm.
    bool gracePlace = false;
    if (graceEnded_)
    {
        graceEnded_ = false;
        if (blendCfg && started_ && !snap && !jumping && !ragdollActive_ && !in.swimming && pedErrH > double(cfg_.blendGracePlaceM))
        {
            snap = true;
            gracePlace = true;
        }
    }
    bool persistentSnap = false;
    if (!snap && !blendCfg)   // R4 T1: Blend has no persistent-error snap (the capped velocity and the self-check answer a lasting error)
    {
        offAcc_ += static_cast<float>((dist - cfg_.offToleranceM) * dtS);
        if (offAcc_ < 0.0f) offAcc_ = 0.0f;
        if (offAcc_ >= cfg_.offLimitMs) { snap = true; persistentSnap = true; ++stats_.persistentSnaps; }
    }
    const bool wasStarted = started_;
    started_ = true;
    if (snap && (quietActive_ || quietInitial_) && wasStarted && !in.teleported)
    {
        // R3 T2: a quiet window writes no coordinate at all, a snap included, unless the ped is so far off that the window itself is
        // the damage (then it ends without a verdict and the snap goes out).
        // (the window may let the error grow by what a ped that does not follow would lose in it: set when the window opened)
        const double abortErr = std::fmax(double(cfg_.quietAbortErrM), quietAbortErr_);
        if (dist <= abortErr)
        {
            snap = false;
            if (persistentSnap) --stats_.persistentSnaps;
        }
        else
        {
            quietActive_ = false;
            quietInitial_ = false;
            quietLeftMs_ = 0.0;
        }
    }
    if (snap && wasStarted && !in.teleported && cfg_.selfCheck && !gracePlace)
    {
        // a snap that is neither the first nor a teleport: the ped was not carried to the pose. Three within 6 s are a stall.
        if (sinceFailSnapMs_ > 6000.0) failSnaps_ = 0;
        ++failSnaps_;
        sinceFailSnapMs_ = 0.0;
        if (failSnaps_ >= cfg_.snapStormN) { snapStorm_ = writeVerdict_ != WriteVerdict::Harm; failSnaps_ = 0; }   // (with Harm our own writes make the snaps)
    }
    if (snap && wasStarted && !in.teleported && !gracePlace) ++stats_.hardSnaps;   // R4 T1: a snap that was neither the first nor a teleport
    if (gracePlace) ++stats_.gracePlaced;
    if (snap)
    {
        o.snap = true;
        o.snapPos = in.pos;
        ped = in.pos;
        offAcc_ = 0;
        if (!gracePlace) forceIssue_ = true;   // (R5: the placement after a grace window keeps the task that runs)
        vf_.x = in.vel.x;   // a snap restarts the ped at the pose's own velocity
        vf_.y = in.vel.y;
        ++stats_.snaps;
    }

    // ---- heading ----
    if (!ragdollActive_)
    {
        o.setHeading = true;
        if (snap) o.heading = WrapDeg360(in.heading);
        else
        {
            const double k = 1.0 - std::exp(-dt / static_cast<double>(cfg_.headingTauMs > 0 ? cfg_.headingTauMs : 1.0f));
            o.heading = WrapDeg360(in.pedHeading + WrapDeg180(double(in.heading) - in.pedHeading) * k);
        }
    }

    // R4 T1: the Blend's read-backs and self-check (before the drive: this frame's decision may already be Placed)
    if (blendCfg)
    {
        const bool judged = !jumping && !ragdollActive_ && !in.swimming && !placedNow;
        const bool graceNow = graceLeftMs_ > 0.0;   // R5: a task-changing event opened a window: nothing accrues in it
        BlendWatch(in, dt, snap, judged && !graceNow, judged && graceNow);
    }

    // ---- the ped's animation velocity (Sync A) ----
    // GET_ENTITY_VELOCITY is the base of the residual push (the push is that velocity plus a correction). If the game keeps
    // the velocity we set (a read after a push returning the push itself), feeding it back makes the push integrate every
    // frame: the simulated ped ran at 2x the pose speed, 7 m behind it, with a snap every second. A read that equals our last
    // push (to 0.05 m/s) is therefore not used: the base is then a model of the animation (the learned nominal speed x the
    // move rate asked for, along the ped's facing; nothing while no gait is shown).
    bool contaminated = false;
    if (in.havePedVel)
    {
        contaminated = pushedLast_ && HLen(double(in.pedVel.x) - lastPush_.x, double(in.pedVel.y) - lastPush_.y) < 0.05;
        if (contaminated)
        {
            ++stats_.contaminated;
            sinceCleanMs_ += dt;
            if (sinceCleanMs_ >= kHeldAnimMs)
            {
                // the last clean read is stale: the model
                const double h = double(in.pedHeading) * kPi / 180.0;
                const double sp = (gait_ != Gait::Still) ? Nominal(gait_, cfg_) * nScale_[static_cast<int>(gait_)] * lastRate_ : 0.0;
                animEst_.x = static_cast<float>(-std::sin(h) * sp);
                animEst_.y = static_cast<float>(std::cos(h) * sp);
                animEst_.z = 0.0f;
            }   // else: the last clean read is held
        }
        else
        {
            sinceCleanMs_ = 0.0;
            animEst_ = in.pedVel;
        }
    }

    // ---- velocity ----
    // The low-passed pose velocity plus a correction toward the pose. No correction inside the
    // steerDeadbandM dead band (the capsule is never exactly on the pose, steering it would only buzz).
    // While ragdolling the same is set (NaturalMotion owns the pose, the velocity makes the ragdoll
    // follow the sender instead of falling in place) with the total capped at ragdollMaxMs and z left to
    // physics.
    // W11 item 3: with the ped's own velocity (havePedVel) a walking / running / standing ped gets only the RESIDUAL: its
    // own velocity (the animation's displacement) plus the position correction, low-passed and capped at residualMaxMs,
    // and nothing at all while the error is inside the dead band. Jumps, ragdoll and swimming keep the push above.
    {
        // T2: on foot (not jumping, ragdolling or swimming) the position error is closed by a coordinate nudge, not a velocity.
        // R4 T1: in Blend the only coordinate writes are the snap and, at the Placed level, the rigid follow (the nudge branch below)
        const bool nudgeMode = (blendCfg ? placedNow : cfg_.push == PushMode::Nudge) && !jumping && !ragdollActive_ && !in.swimming;
        const bool blendDrive = blendCfg && !placedNow && !jumping && !ragdollActive_ && !in.swimming;
        const bool residual = !blendCfg && !nudgeMode && cfg_.residualMaxMs > 0.0f && in.havePedVel && !jumping && !ragdollActive_ && !in.swimming;
        if (!residual || snap) { corrFx_ = corrFy_ = 0.0f; }
        o.setVelocity = true;
        if (snap)
        {
            o.velocity = in.vel;
        }
        else if (blendDrive)
        {
            BlendDrive(in, ped, dt, o);
        }
        else if (nudgeMode)
        {
            o.setVelocity = false;
            const bool moving = gait_ != Gait::Still;
            const bool placed = cfg_.selfCheck && placedLeftMs_ > 0.0;
            const double hx = double(in.pos.x) - ped.x, hy = double(in.pos.y) - ped.y;
            const double he = HLen(hx, hy);
            double dead = placed ? cfg_.placedDeadM : (moving ? cfg_.nudgeDeadM : cfg_.nudgeStillDeadM);
            const double tau = placed ? cfg_.placedTauMs : cfg_.nudgeTauMs;
            double cap = placed ? cfg_.placedNudgeMaxMs : (moving ? cfg_.nudgeMaxMs : cfg_.nudgeStillMaxMs);
            // R4 T1: the rigid follow of the Blend keeps up with the pose it follows (a sprint is 7.5 m/s; the old fixed 8 m/s lagged it) and
            // never writes more than the pose's own speed + the blend's correction ceiling per second
            if (placed && blendCfg) cap = HLen(vf_.x, vf_.y) + cfg_.blendHardCapMs;
            // the start of a movement: the clip is still getting up to speed, a bounded lag is tolerated (Sync A's grace, as a band)
            if (moving && !placed && sinceMoveStartMs_ < cfg_.startGraceMs && cfg_.nudgeStartGraceM > dead)
            {
                dead = cfg_.nudgeStartGraceM;
                if (he < dead) ++stats_.graceFrames;
            }
            double dx = 0, dy = 0, dzv = 0;
            // R4 T1: the rigid follow of the Blend adds the part of the pose's velocity the ped does not carry itself (the pose velocity less
            // the ped's own displacement net of our writes), so a pose at 7 m/s is not followed 0.4 m late
            double ffx = 0, ffy = 0;
            if (placed && blendCfg && haveSelf_)
            {
                ffx = (double(vf_.x) - selfV_.x) * dtS;
                ffy = (double(vf_.y) - selfV_.y) * dtS;
            }
            // R3 T2: a quiet window writes nothing; with the verdict Harm (every write cancels the walk) the error is closed by one
            // PULSE per pulseMs beyond pulseMinErrM instead of a write every frame (the rigid Placed follow stays per frame: nothing
            // else is left there).
            const bool pulsed = cfg_.selfCheck && writeVerdict_ == WriteVerdict::Harm && !placed;
            if (quietActive_ || quietInitial_)
            {
                // nothing
            }
            else if (pulsed)
            {
                const double dzp = double(in.pos.z) - ped.z;
                if ((he > cfg_.pulseMinErrM || std::fabs(dzp) > cfg_.nudgeZBandM) && pulseSinceMs_ >= cfg_.pulseMs)
                {
                    double mag = he - dead;
                    if (mag > cfg_.pulseMaxM) mag = cfg_.pulseMaxM;
                    if (mag > 0.0 && he > 0.0) { dx = hx / he * mag; dy = hy / he * mag; }
                    if (std::fabs(dzp) > cfg_.nudgeZBandM) dzv = dzp > 0 ? dzp - cfg_.nudgeZBandM : dzp + cfg_.nudgeZBandM;
                    pulseSinceMs_ = 0.0;
                    ++stats_.pulses;
                }
            }
            else
            {
                if (he > dead)
                {
                    double mag = (he - dead) * (1.0 - std::exp(-dt / (tau > 1.0 ? tau : 1.0)));
                    const double maxStep = cap * dtS;
                    if (mag > maxStep) mag = maxStep;
                    dx = hx / he * mag;
                    dy = hy / he * mag;
                }
                if (ffx != 0.0 || ffy != 0.0)
                {
                    dx += ffx;
                    dy += ffy;
                    const double tm = HLen(dx, dy), maxStep2 = cap * dtS;
                    if (tm > maxStep2 && tm > 0.0) { dx *= maxStep2 / tm; dy *= maxStep2 / tm; }
                }
                const double dz = double(in.pos.z) - ped.z;
                if (std::fabs(dz) > cfg_.nudgeZBandM)
                {
                    double cz = cfg_.zGain * (dz > 0 ? dz - cfg_.nudgeZBandM : dz + cfg_.nudgeZBandM);
                    if (cz > cfg_.zCorrMaxMs) cz = cfg_.zCorrMaxMs;
                    if (cz < -cfg_.zCorrMaxMs) cz = -cfg_.zCorrMaxMs;
                    dzv = cz * dtS;
                }
            }
            if (placed && blendCfg)
            {
                // the rigid follow still lets the gait task run: the trim keeps braking / speeding it by what the follow has to close
                const double spdP = HLen(vf_.x, vf_.y);
                if (moving && spdP >= 1.0)
                {
                    // (the part of the pose's velocity the ped does not carry itself, along the track: negative = it runs ahead of the pose)
                    const double ca = haveSelf_ ? ((double(vf_.x) - selfV_.x) * vf_.x + (double(vf_.y) - selfV_.y) * vf_.y) / spdP : 0.0;
                    BlendTrimStep(Clamp(ca, -double(cfg_.blendMaxDiffMs), double(cfg_.blendMaxDiffMs)), dt, 0.0);
                }
            }
            const double nm = HLen(dx, dy);
            if (nm > 1e-5 || dzv != 0.0)
            {
                o.nudge = true;
                o.nudgeDelta.x = static_cast<float>(dx);
                o.nudgeDelta.y = static_cast<float>(dy);
                o.nudgeDelta.z = static_cast<float>(dzv);
                ++stats_.nudgeFrames;
                stats_.nudgedM += nm;
            }
            if (moving && haveSelf_)
            {
                const double travelled = HLen(selfV_.x, selfV_.y) * dtS + nm;   // what the ped covered, nudges included
                stats_.travelledM += travelled;
                if (HLen(vf_.x, vf_.y) >= 1.0 && accLp_ < cfg_.steadyAccMs2 && sinceMoveStartMs_ >= 1200.0)
                {
                    winNudgedM_ += nm;
                    winTravelM_ += travelled;
                }
            }
            if (moving && HLen(vf_.x, vf_.y) >= 1.0 && accLp_ < cfg_.steadyAccMs2 && sinceMoveStartMs_ >= 1200.0) winMs_ += dt;
        }
        else if (residual)
        {
            const double hx = double(in.pos.x) - ped.x, hy = double(in.pos.y) - ped.y;
            const double he = HLen(hx, hy);
            double cx = 0, cy = 0;
            // Sync A: a moving gait is pushed only beyond residualDeadM (the rate loop closes the rest; the ramp starts from
            // the dead band's edge), and not at all in the start grace window (below startGraceMaxM); a standing ped keeps
            // the small steerDeadbandM.
            const bool moving = gait_ != Gait::Still;
            const double dead = moving ? cfg_.residualDeadM : cfg_.stillDeadM;
            const bool grace = moving && sinceMoveStartMs_ < cfg_.startGraceMs && he < cfg_.startGraceMaxM;
            if (grace) ++stats_.graceFrames;
            if (he >= dead && !grace)
            {
                const double ex = he - dead;
                const double gain = (cfg_.corrGainBase + cfg_.corrGainPerM * ex) * (ex / he);
                cx = gain * hx;
                cy = gain * hy;
                const double cl = HLen(cx, cy);
                if (cl > cfg_.residualMaxMs && cl > 0.0) { cx *= cfg_.residualMaxMs / cl; cy *= cfg_.residualMaxMs / cl; }
            }
            const double k = 1.0 - std::exp(-dt / static_cast<double>(cfg_.residualTauMs > 0 ? cfg_.residualTauMs : 1.0f));
            corrFx_ = static_cast<float>(corrFx_ + (cx - corrFx_) * k);
            corrFy_ = static_cast<float>(corrFy_ + (cy - corrFy_) * k);
            const double cf = HLen(corrFx_, corrFy_);
            double cz = 0;
            const double dz = double(in.pos.z) - ped.z;
            if (std::fabs(dz) > cfg_.zDeadbandM)
            {
                cz = cfg_.zGain * (dz > 0 ? dz - cfg_.zDeadbandM : dz + cfg_.zDeadbandM);
                if (cz > cfg_.zCorrMaxMs) cz = cfg_.zCorrMaxMs;
                if (cz < -cfg_.zCorrMaxMs) cz = -cfg_.zCorrMaxMs;
            }
            if (cf < 0.02 && cz == 0.0)
            {
                o.setVelocity = false;                 // inside the dead band: the animation carries the ped
                corrFx_ = corrFy_ = 0.0f;
            }
            else
            {
                o.velocity.x = static_cast<float>(animEst_.x + corrFx_);
                o.velocity.y = static_cast<float>(animEst_.y + corrFy_);
                o.velocity.z = static_cast<float>(animEst_.z + cz);
            }
        }
        else
        {
            const double hx = double(in.pos.x) - ped.x, hy = double(in.pos.y) - ped.y;
            const double he = HLen(hx, hy);
            double cx = 0, cy = 0;
            // Swimming (W8 item 4): the water-tuned gain, cap and vertical band.
            const double gainScale = in.swimming ? cfg_.swimCorrScale : 1.0;
            const double capH = in.swimming ? cfg_.swimCorrMaxHMs : cfg_.corrMaxHMs;
            if (he >= cfg_.steerDeadbandM)
            {
                cx = (cfg_.corrGainBase + cfg_.corrGainPerM * he) * hx * gainScale;
                cy = (cfg_.corrGainBase + cfg_.corrGainPerM * he) * hy * gainScale;
                const double cl = HLen(cx, cy);
                if (cl > capH && cl > 0.0) { cx *= capH / cl; cy *= capH / cl; }
            }
            double vx = vf_.x + cx, vy = vf_.y + cy;
            if (ragdollActive_)
            {
                const double tl = HLen(vx, vy);
                if (tl > cfg_.ragdollMaxMs && tl > 0.0) { vx *= cfg_.ragdollMaxMs / tl; vy *= cfg_.ragdollMaxMs / tl; }
            }
            o.velocity.x = static_cast<float>(vx);
            o.velocity.y = static_cast<float>(vy);
            double vz = in.vel.z;
            const double dz = double(in.pos.z) - ped.z;
            const double zBand = in.swimming ? cfg_.swimZDeadbandM : cfg_.zDeadbandM;
            const double zGain = in.swimming ? cfg_.swimZGain : cfg_.zGain;
            if (!jumping && !ragdollActive_ && std::fabs(dz) > zBand)
            {
                double cz = zGain * (dz > 0 ? dz - zBand : dz + zBand);
                if (cz > cfg_.zCorrMaxMs) cz = cfg_.zCorrMaxMs;
                if (cz < -cfg_.zCorrMaxMs) cz = -cfg_.zCorrMaxMs;
                vz += cz;
            }
            o.velocity.z = static_cast<float>(vz);
        }
        o.velocityZFromPhysics = !residual && (jumping || ragdollActive_);
    }

    // ---- direction, lead and the anchor (W11) ----
    // direction = the low-passed velocity's (the sender's heading when it is hardly moving); sp its speed.
    const double sp = HLen(vf_.x, vf_.y);
    double dirx, diry;
    if (sp >= kDirMinSpeedMs) { dirx = vf_.x / sp; diry = vf_.y / sp; }
    else
    {
        const double r = double(in.heading) * kPi / 180.0; // GTA heading 0 = +y, counter-clockwise
        dirx = -std::sin(r);
        diry = std::cos(r);
    }
    // R4 T1: the pose's turn rate (rad/s, low-passed): a gait task issued in the middle of a turn aims where the turn is going, not where it
    // has got to (an issue 0.3 s into a 90 degree turn aimed 45 degrees short, and a ped half-obeying its velocity walked the old line)
    {
        if (blendCfg && sp >= 1.0 && havePrevDir_ && dt >= 4.0 && !in.teleported)
        {
            const double cr = prevDirX_ * diry - prevDirY_ * dirx, dp = prevDirX_ * dirx + prevDirY_ * diry;
            const double w = std::atan2(cr, dp) / (dt / 1000.0);
            omegaLp_ += (w - omegaLp_) * (1.0 - std::exp(-dt / 150.0));
        }
        else omegaLp_ = 0.0;
        if (blendCfg && sp >= 1.0) { prevDirX_ = dirx; prevDirY_ = diry; havePrevDir_ = true; }
        else havePrevDir_ = false;
    }
    if (anchorMode)
    {
        // Anchor mode: the entity the ped runs to is moved EVERY frame to pose + direction * lead (the pose is already the
        // smoother's predicted position), so a turn bends the path instead of re-issuing a task.
        // Sync A: no floor (a stop was run through) and none at all while settling (the anchor IS the pose).
        const double aLead = settling ? 0.0 : Clamp(sp * cfg_.anchorLeadS, cfg_.anchorLeadMinM, cfg_.anchorLeadMaxM);
        o.moveAnchor = true;
        o.anchorPos.x = static_cast<float>(in.pos.x + dirx * aLead);
        o.anchorPos.y = static_cast<float>(in.pos.y + diry * aLead);
        o.anchorPos.z = in.pos.z;
        ++stats_.anchorFrames;
    }
    o.anchorMode = anchorMode;

    // R3 T2: the gait the ped READS while a moving one is commanded (the share of frames, and the time from a task issue to the first
    // gait read: the live number a re-issue spacing has to be measured against).
    if (gait_ != Gait::Still && !jumping && !ragdollActive_ && !in.swimming && !settling)
    {
        ++stats_.commandedFrames;
        const bool read = in.pedGait != Gait::Still;
        if (read) ++stats_.gaitReadFrames;
        if (sinceIssueGaitMs_ >= 0.0)
        {
            sinceIssueGaitMs_ += dt;
            if (read) { stats_.latencySumMs += sinceIssueGaitMs_; ++stats_.latencyN; sinceIssueGaitMs_ = -1.0; }
            else if (sinceIssueGaitMs_ > 6000.0) sinceIssueGaitMs_ = -1.0;
        }
    }

    // ---- gait task ----
    if (ragdollActive_ || jumping)
    {
        mismatchMs_ = 0;
        reevaluate_ = false;
        return o; // a task now would cancel the ragdoll / jump; forceIssue_ re-issues after
    }
    if (holding)
    {
        // about to stand still: let the running task finish, issue nothing new (the move rate is held: it only lasts while called)
        if (rateAvail && !in.swimming && !blendCfg) { o.setMoveRate = true; o.moveRate = lastRate_; }
        return o;
    }
    if (gait_ == Gait::Still)
    {
        mismatchMs_ = 0;
        reevaluate_ = false;      // nothing to keep or re-issue: the stand-still task is the running one
        gaitShown_ = Gait::Still;
        if (swingBlocks && (lastTask_ != LocoTask::StandStill || forceIssue_))
        {
            o.swingHeld = true;       // R4 T1: held while a melee swing plays (forceIssue_ stays: it goes out afterwards)
            ++stats_.swingHeld;
        }
        else if (lastTask_ != LocoTask::StandStill || forceIssue_)
        {
            o.task = LocoTask::StandStill;
            o.standStillMs = cfg_.standStillMs;
            lastTask_ = LocoTask::StandStill;
            lastTaskGait_ = Gait::Still;
            sinceIssueMs_ = 0;
            forceIssue_ = false;
            ++stats_.standStill;
        }
        return o;
    }

    // target = pose + (pose - ped) + direction * lead, on the pose's height. The direction is the
    // low-passed velocity's (the sender's heading when it is hardly moving), the lead is leadS seconds
    // of travel clamped to leadMinM..leadMaxM: far enough ahead that the ped does not reach it for
    // seconds (GTAN's 1 s lead made the target "reached" after half a second).
    const double leadM = settling ? 0.0 : Clamp(sp * cfg_.leadS, cfg_.leadMinM, cfg_.leadMaxM);
    LocoVec anchor; // the target without its lead: where the pose is, plus the ped's error to it
    // (Blend: the target is on the POSE's line. The ped's error is closed by the velocity; aiming the task past the pose by that error made a
    // transient error at the moment of the issue a lasting sideways aim for the whole life of the task)
    anchor.x = static_cast<float>(in.pos.x + (blendCfg ? 0.0 : double(in.pos.x) - ped.x));
    anchor.y = static_cast<float>(in.pos.y + (blendCfg ? 0.0 : double(in.pos.y) - ped.y));
    LocoVec target;
    double tdx = dirx, tdy = diry;
    if (blendCfg && omegaLp_ != 0.0)
    {
        const double a = Clamp(omegaLp_ * cfg_.blendTurnLeadS, -1.2, 1.2), ca = std::cos(a), sa = std::sin(a);
        tdx = dirx * ca - diry * sa;
        tdy = dirx * sa + diry * ca;
    }
    target.x = static_cast<float>(anchor.x + tdx * leadM);
    target.y = static_cast<float>(anchor.y + tdy * leadM);
    target.z = in.pos.z;

    // Animation speed. The override scales the gait animation to the pose speed (rate = speed over the
    // gait's nominal speed, clamped); the task's blend ratio is then the plain gait. Without it the
    // blend ratio itself carries the speed, quantised so it changes rarely.
    float blend = BlendRatio(gait_);
    if (blendCfg && cfg_.blendGaitClip && gait_ != Gait::Still && !in.swimming && !settling && std::fabs(blendTrim_) <= kClipTrimMax &&
        placedFailStreak_ == 0)   // a puppet whose writes were found not to carry it keeps the speed knob until the streak clears
    {
        // 0.4.17: the sender's clip (1 / 2 / 3), the velocity writes carry the exact speed. Only while the writes are honoured (the trim the
        // corrections built up stays small: live 0.4.15 it was +0.05..0.2); a world whose task fights the writes (the trim grows) keeps the
        // continuous speed knob below, which is what the R4 T1 matrix proved for it.
        blend = BlendRatio(gait_);
        rateIneffMs_ = 0.0;
    }
    else if (blendCfg)
    {
        // R4 T1: the move blend ratio is a continuous speed knob, the nominal speeds its integer points (1 walk .. 3 sprint), trimmed by what
        // the writes had to correct; no move rate override (it would scale the speed a second time)
        double b;
        if (sp < cfg_.nominalWalk) b = sp / cfg_.nominalWalk;
        else if (sp < cfg_.nominalRun) b = 1.0 + (sp - cfg_.nominalWalk) / (cfg_.nominalRun - cfg_.nominalWalk);
        else b = 2.0 + (sp - cfg_.nominalRun) / (cfg_.nominalSprint - cfg_.nominalRun);
        b += blendTrim_;
        blend = static_cast<float>(Clamp(b, 0.5, 3.0));
        rateIneffMs_ = 0.0;
    }
    else if (rateAvail && !in.swimming)
    {
        // Sync A: the closed loop. The speed to show is the pose's plus what closes the along-track error in rateTcS (a ped
        // behind the pose runs faster, one ahead slower; settling: the distance to the pose), over the gait's LEARNED nominal.
        double vdes = sp;
        if (cfg_.rateLoop)
        {
            double e;
            if (settling || sp < kDirMinSpeedMs) e = pedErrH;
            else e = (double(in.pos.x) - ped.x) * dirx + (double(in.pos.y) - ped.y) * diry;
            vdes = sp + Clamp(e, cfg_.rateErrMinM, cfg_.rateErrMaxM) / (cfg_.rateTcS > 0.0f ? cfg_.rateTcS : 0.7f);
            if (vdes < 0.0) vdes = 0.0;
        }
        o.setMoveRate = true;
        o.moveRate = static_cast<float>(Clamp(vdes / (Nominal(gait_, cfg_) * nScale_[static_cast<int>(gait_)]), cfg_.moveRateMin, cfg_.moveRateMax));
        // the learning sample: the ped's own speed over the rate that made it (the rate low-passed like the ped's lag)
        const bool ownSpeed = cfg_.push == PushMode::Nudge;    // T2: the ped's own displacement (net of our nudges), no velocity read
        if (cfg_.rateLoop && (ownSpeed ? haveSelf_ : (in.havePedVel && !contaminated && !pushedLast_)) && !settling && sp >= 1.0 &&
            steadyMs_ >= cfg_.learnSteadyMs)
        {
            const double ps = ownSpeed ? HLen(selfV_.x, selfV_.y) : HLen(in.pedVel.x, in.pedVel.y);
            if (ps > 0.8 && rateEmaNum_ > 0.05)
            {
                const double scale = Clamp(ps / rateEmaNum_ / Nominal(gait_, cfg_), cfg_.learnMinScale, cfg_.learnMaxScale);
                const double k = 1.0 - std::exp(-dt / static_cast<double>(cfg_.learnTauMs > 0 ? cfg_.learnTauMs : 1.0f));
                float& ns = nScale_[static_cast<int>(gait_)];
                ns = static_cast<float>(ns + (scale - ns) * k);
            }
        }
        {
            const double k = 1.0 - std::exp(-dt / 300.0);
            rateEmaNum_ += (double(o.moveRate) - rateEmaNum_) * k;
        }
        lastRate_ = o.moveRate;
        o.rateClamped = o.moveRate <= cfg_.moveRateMin + 0.005f || o.moveRate >= cfg_.moveRateMax - 0.005f;
        // T2 self-check: does the native scale the stride? Regress the ped's own speed (over the gait's nominal, net of our nudges) on
        // the applied rate (low-passed like the ped's lag). A working native gives a slope of about the learned scale (0.55 .. 1.6);
        // an ignored one ~0 however the loop moves the rate. Only a decision when the rate has actually varied.
        if (cfg_.selfCheck && haveSelf_ && !settling && sp >= 1.0 && steadyMs_ >= 500.0 && !jumping && accLp_ < cfg_.rateProbeAccMs2 &&
            writeVerdict_ != WriteVerdict::Harm)   // (with Harm each pulse restarts the walk: own speed against the rate is confounded)
        {
            const double x = rateEmaNum_, y = HLen(selfV_.x, selfV_.y) / Nominal(gait_, cfg_);
            const double wd = std::exp(-dt / static_cast<double>(cfg_.rateProbeTauMs > 1.0f ? cfg_.rateProbeTauMs : 1.0f));
            rpW_ = rpW_ * wd + dt;
            rpX_ = rpX_ * wd + dt * x;
            rpY_ = rpY_ * wd + dt * y;
            rpXX_ = rpXX_ * wd + dt * x * x;
            rpXY_ = rpXY_ * wd + dt * x * y;
            if (rpW_ >= cfg_.rateProbeMinMs)
            {
                const double mx = rpX_ / rpW_, my = rpY_ / rpW_;
                const double var = rpXX_ / rpW_ - mx * mx, cov = rpXY_ / rpW_ - mx * my;
                if (var >= cfg_.rateVarMin)
                {
                    if (cov / var < cfg_.rateSlopeMin) rateIneffMs_ += dt;
                    else rateIneffMs_ = 0.0;
                }
            }
            if (rateIneffMs_ >= cfg_.ratePinMs && LadderMayChange(true))
            {
                if (sinceRateFailMs_ > 300000.0) rateFailStreak_ = 0;
                double dur = cfg_.rateOffMs;
                for (int i = 0; i < rateFailStreak_ && dur < cfg_.rateOffMaxMs; ++i) dur *= 2.0;
                if (dur > cfg_.rateOffMaxMs) dur = cfg_.rateOffMaxMs;
                ++rateFailStreak_;
                sinceRateFailMs_ = 0.0;
                rateOffLeftMs_ = dur;
                pendingWhy_ = LocoLevelWhy::RateIneffective;
                rateIneffMs_ = 0.0;
                rpW_ = rpX_ = rpY_ = rpXX_ = rpXY_ = 0.0;
            }
        }
    }
    else
    {
        blend = QuantisedBlend(sp, gait_, cfg_);
        rateIneffMs_ = 0.0;
    }

    // T2 self-checks of the task itself: the ped's own displacement against the pose's (a go-to that "runs" but carries nothing),
    // and the share of the distance the nudges had to cover (a task that does not follow its target closely).
    bool stuckNow = false, draggingNow = false;
    if (cfg_.selfCheck)
    {
        const bool harmSettling = writeVerdict_ == WriteVerdict::Harm && sinceWriteMs_ < cfg_.harmSettleMs;   // R3: a pulse's effect is running
        const bool eligible = gait_ != Gait::Still && !settling && !jumping && !ragdollActive_ && !in.swimming && haveSelf_ &&
                              sp >= cfg_.stuckMinPoseMs && steadyMs_ >= cfg_.stuckSettleMs && sinceMoveStartMs_ >= cfg_.stuckSettleMs && !harmSettling;
        if (eligible)
        {
            if (HLen(selfV_.x, selfV_.y) < cfg_.stuckRatio * sp) { lagMs_ += dt; healthyMs_ = 0.0; }
            else
            {
                lagMs_ = 0.0;
                healthyMs_ += dt;
                if (healthyMs_ >= 3000.0) { lastIssueWasStall_ = false; stallEvents_ = 0; }   // a stall that stayed cured is not "again"
            }
            stuckNow = lagMs_ >= cfg_.stuckMs;
        }
        else lagMs_ = 0.0;
        if (winMs_ >= cfg_.nudgeShareWindowMs)
        {
            // (with the verdict Harm the writes are single pulses by design: their share of the distance says nothing about the task)
            draggingNow = writeVerdict_ != WriteVerdict::Harm && winTravelM_ >= 3.0 && winNudgedM_ > cfg_.nudgeShareMax * winTravelM_;
            winNudgedM_ = winTravelM_ = winMs_ = 0.0;
        }
    }
    else winMs_ = winNudgedM_ = winTravelM_ = 0.0;
    if (blendCfg) { stuckNow = draggingNow = false; lagMs_ = 0.0; }   // R4 T1: the blend's own self-check (BlendWatch) replaces both
    // R4 T1: a gait read after a stall re-issue ends the dead-gait streak
    if (blendCfg && gait_ != Gait::Still && in.pedGait != Gait::Still) { deadStallStreak_ = 0; noGaitSource_ = false; }

    // A stall: the ped shows no gait at all although a moving one is commanded (the task ended or was
    // cancelled by the game). The only gait-mismatch trigger left; the others were churn.
    mismatchMs_ = (in.pedGait == Gait::Still && !in.swimming &&
                   !(cfg_.selfCheck && writeVerdict_ == WriteVerdict::Harm && sinceWriteMs_ < cfg_.harmSettleMs)) ? mismatchMs_ + dt : 0.0;   // a swimmer shows no land gait
    if (cfg_.selfCheck && cfg_.lostNeedsLowSpeed && !blendCfg && mismatchMs_ > 0.0 && haveSelf_ && sp >= cfg_.stuckMinPoseMs &&
        HLen(selfV_.x, selfV_.y) >= 0.6 * sp)
        mismatchMs_ = 0.0;   // R3 T2: it moves with the pose (its own displacement agrees) although the gait natives read Still: not lost

    // ---- R3 T2: coordinate writes: the passive proof, and the quiet window's verdict ----
    if (cfg_.selfCheck && cfg_.quietTrial)
    {
        const bool movingEval = gait_ != Gait::Still && !settling && !jumping && !ragdollActive_ && !in.swimming;
        const double own = haveSelf_ ? HLen(selfV_.x, selfV_.y) : 0.0;
        if (writeVerdict_ == WriteVerdict::Unknown && !quietActive_)
        {
            // a working game proves the writes harmless for free: a gait read AND the ped's own speed agreeing with the pose while a
            // write went out within the last 250 ms, for harmlessEvidMs of steady motion
            if (movingEval && sp >= 1.5 && accLp_ < cfg_.steadyAccMs2 && sinceWriteMs_ < 250.0 && haveSelf_)
            {
                if (in.pedGait != Gait::Still && own >= cfg_.stuckRatio * sp) harmlessEvidMs_ += dt;
                else harmlessEvidMs_ = 0.0;
                if (harmlessEvidMs_ >= cfg_.harmlessEvidMs) { writeVerdict_ = WriteVerdict::Harmless; verdictChangedPending_ = true; }
            }
        }
        if (quietActive_)
        {
            const bool good = movingEval && (in.pedGait != Gait::Still || (haveSelf_ && sp >= cfg_.stuckMinPoseMs && own >= cfg_.stuckRatio * sp));
            if (good) quietGoodMs_ += dt;
            if (quietGoodMs_ >= cfg_.quietNeedGoodMs)
            {
                // the ped showed a gait the moment the writes stopped: every write cancels the walk
                writeVerdict_ = WriteVerdict::Harm;
                verdictChangedPending_ = true;
                quietActive_ = false;
                quietLeftMs_ = 0.0;
                ++stats_.quietHarm;
                mismatchMs_ = 0.0;
                lagMs_ = 0.0;
                snapStorm_ = false;
                failSnaps_ = 0;
            }
            else if (quietLeftMs_ <= 0.0)
            {
                // nothing, with no write for the whole window: the writes are not the cause, the ladder goes on at once
                writeVerdict_ = WriteVerdict::Harmless;
                verdictChangedPending_ = true;
                quietActive_ = false;
                ++stats_.quietClear;
            }
        }
    }

    // R4 T1: a melee swing animation plays: no task is issued and no stall / turn / drift rule is evaluated (their side effects would repeat on
    // every frame of the swing); forceIssue_ and a pending SoftReset stay, so the task goes out right after
    if (swingBlocks)
    {
        o.swingHeld = true;
        ++stats_.swingHeld;
        return o;
    }

    // Re-issue only when the task would otherwise be wrong: the commanded (debounced) gait changed, the
    // blend ratio moved a step (fallback only), or - at most every minReissueMs - the direction turned
    // by more than turnReissueDeg, the target drifted driftM from where the last one has moved to, the
    // ped is about to reach the last target (within max(arriveMinM, arriveFrac * lead)), or it stalled.
    // W11 item 1: a change between Walk / Run / Sprint is NOT a re-issue (the task stays, setBlend changes its speed);
    // a task of the other kind (a stand-still before, or Straight <-> Anchor) is.
    LocoTask wantTask = anchorMode ? LocoTask::GoToAnchor : LocoTask::GoStraight;
    bool issue = forceIssue_ || lastTask_ != wantTask;
    LocoWhy why = LocoWhy::None;
    if (issue)
        why = forceIssue_ ? (resetForced_ ? LocoWhy::Reset : LocoWhy::Forced) : LocoWhy::Gait;
    if (!issue && gait_ != gaitShown_ && gaitShown_ != Gait::Still)
        ++stats_.gaitKept;
    gaitShown_ = gait_;
    if (!issue && !blendCfg && !rateAvail && !rateLadderOff && sinceIssueMs_ >= cfg_.blendReissueMs &&
        std::fabs(blend - lastBlend_) >= cfg_.blendStep - 1e-4f)
    {
        issue = true;
        why = LocoWhy::Blend;
    }
    // R3 T2: a forced re-issue (snap, Reset, a landing) goes out at least issueGapMs after a moving task's issue: the previous one has
    // not had its chance to show a gait (live 0.4.7: 13 issues per 10 s = 0.77 s apart, no gait ever). Still <-> moving is never held.
    // (while the previous issue is still waiting for its first gait the gap is the whole firstGaitWindowMs: a snap's re-issue must not
    // restart a task that has not had its chance)
    const double forcedGap = sinceIssueGaitMs_ >= 0.0 ? std::fmax(double(cfg_.issueGapMs), double(cfg_.firstGaitWindowMs)) : double(cfg_.issueGapMs);
    if (issue && cfg_.selfCheck && lastTaskGait_ != Gait::Still && stats_.goStraight > 0 && sinceIssueMs_ < forcedGap)
    {
        issue = false;
        why = LocoWhy::None;
        ++stats_.issuesHeld;
    }
    // A SoftReset() evaluates the rules now (no minReissueMs wait) but only the position rules: turn, drift, arrival.
    const bool soft = reevaluate_;
    reevaluate_ = false;
    // R3 T2: the rule-based re-issues are spaced reissueMinMs (not minReissueMs), and only a BIG turn may come earlier.
    const double reissueMin = cfg_.selfCheck ? std::fmax(double(cfg_.minReissueMs), double(cfg_.reissueMinMs)) : double(cfg_.minReissueMs);
    const bool windowOpen = soft || sinceIssueMs_ >= reissueMin;
    bool bigTurn = false;
    if (cfg_.selfCheck && !issue && !windowOpen && !anchorMode && sinceIssueMs_ >= cfg_.turnBigMinMs && forceIssue_ == false)
    {
        const double lastSp0 = HLen(lastTargetVel_.x, lastTargetVel_.y);
        if (sp >= cfg_.idleUp && lastSp0 >= cfg_.idleUp)
        {
            const double cosA0 = (double(vf_.x) * lastTargetVel_.x + double(vf_.y) * lastTargetVel_.y) / (sp * lastSp0);
            bigTurn = cosA0 < std::cos(double(cfg_.turnBigDeg) * kPi / 180.0);
        }
        else if (cfg_.turnBigMinMs > 0.0f) bigTurn = false;
    }
    if (bigTurn && !issue)
    {
        issue = true;
        why = LocoWhy::Turn;
    }
    else if (!issue && windowOpen)
    {
        const double el = sinceIssueMs_ / 1000.0;
        // Drift is the anchor's (a long lead turns a few degrees into metres of target movement; the
        // turn rule below covers direction): where the last anchor would be now at its velocity.
        const double px = lastAnchor_.x + double(lastTargetVel_.x) * el;
        const double py = lastAnchor_.y + double(lastTargetVel_.y) * el;
        // Anchor mode has no fixed target: turn, drift and arrival cannot happen, only a lost task (stall) re-issues.
        // W11: only the LATERAL part counts. The target is a far point on the path: when the sender speeds up or slows down
        // (a Run <-> Sprint change) the anchor moves along the track faster or slower than the last velocity predicted, the
        // target is still ahead on the path, and a re-issue would only restart the stride (drift 4 of 5 re-issues in a
        // Run <-> Sprint test).
        const double lastSp = HLen(lastTargetVel_.x, lastTargetVel_.y);
        double driftM = HLen(anchor.x - px, anchor.y - py);
        if (lastSp >= kDirMinSpeedMs)
            driftM = std::fabs((anchor.x - px) * (-double(lastTargetVel_.y) / lastSp) + (anchor.y - py) * (double(lastTargetVel_.x) / lastSp));
        const bool drift = !anchorMode && !blendCfg && driftM > cfg_.driftM;   // (Blend: the position is not the task's job)
        bool turn = false;
        if (sp >= cfg_.idleUp && lastSp >= cfg_.idleUp)
        {
            const double cosA = (double(vf_.x) * lastTargetVel_.x + double(vf_.y) * lastTargetVel_.y) / (sp * lastSp);
            turn = !anchorMode && cosA < std::cos(double(cfg_.turnReissueDeg) * kPi / 180.0);
        }
        const double arrive = std::fmax(cfg_.arriveMinM, cfg_.arriveFrac * lastLeadM_);
        const bool arriving = !anchorMode && HLen(double(lastTarget_.x) - ped.x, double(lastTarget_.y) - ped.y) < arrive;
        const bool driftNow = drift && (soft || sinceIssueMs_ >= cfg_.driftReissueMs);
        // Sync A: a lost task is noticed stallFirstMs after its issue; only after a stall re-issue does stallRetryMs apply (the first
        // stall used to wait 2.5 s: a puppet whose go-to failed was dragged along in an idle pose for that long, at every start).
        const bool lostNow = mismatchMs_ >= cfg_.stallMs;
        const bool stormNow = snapStorm_ && !blendCfg;
        // R3 T2: the first stall waits firstGaitWindowMs while the issue is still waiting for its first gait (a task that has not had
        // its chance is not lost), and a failure seen while coordinate writes were flowing first gets a QUIET window.
        const double stallFirst = (cfg_.selfCheck && sinceIssueGaitMs_ >= 0.0)
                                      ? std::fmax(double(cfg_.stallFirstMs), double(cfg_.firstGaitWindowMs)) : double(cfg_.stallFirstMs);
        const bool fireNow = !soft && (lostNow || stuckNow || stormNow || (draggingNow && anchorMode)) &&
                             (stormNow || sinceIssueMs_ >= (noGaitSource_ ? double(cfg_.blendDeadRetryMs) : (lastIssueWasStall_ ? cfg_.stallRetryMs : stallFirst)));
        bool quietHold = quietActive_;
        if (fireNow && cfg_.selfCheck && cfg_.quietTrial && writeVerdict_ == WriteVerdict::Unknown && !quietActive_ && placedLeftMs_ <= 0.0 &&
            (lostNow || stuckNow || stormNow))
        {
            if (sinceWriteMs_ < cfg_.quietRecentMs && quietTries_ < 3)
            {
                // the window is as long as the error it may cost allows: ~6 m of a ped that does not follow (a walk and a run get the full
                // quietWindowMs, a sprint ~0.9 s; a gait read needs ~0.3-0.5 s after the last write's effect has run out)
                const double winMs = Clamp(6000.0 / (sp > 0.5 ? sp : 0.5), 700.0, double(cfg_.quietWindowMs));
                quietActive_ = true;
                quietLeftMs_ = winMs;
                quietAbortErr_ = pedErrH + 1.25 * sp * winMs / 1000.0 + 0.5;
                quietElapsedMs_ = quietGoodMs_ = 0.0;
                ++quietTries_;
                ++stats_.quietWindows;
                quietHold = true;
            }
            else if (sinceWriteMs_ >= cfg_.quietRecentMs)
            {
                // no coordinate write for the last quietRecentMs and still no gait: the writes are not what stops it
                writeVerdict_ = WriteVerdict::Harmless;
                verdictChangedPending_ = true;
                ++stats_.quietClear;
            }
        }
        o.quiet = quietHold;
        const bool stallNow = fireNow && !quietHold;
        issue = !quietHold && (turn || driftNow || arriving || stallNow);
        if (stallNow)
        {
            if (sinceStallEventMs_ < 15000.0) ++stallEvents_;
            else stallEvents_ = 1;
            sinceStallEventMs_ = 0.0;
        }
        snapStorm_ = false;
        if (stallNow && blendCfg)
        {
            // R4 T1: a task that never shows a gait: re-issued (spaced), and after blendStallLimit tries the source is called dead. The
            // position is carried by the velocity either way, so there is nothing to repair and no level to give up.
            ++stats_.deadStalls;
            if (++deadStallStreak_ >= cfg_.blendStallLimit) noGaitSource_ = true;
        }
        if (stallNow && !blendCfg && !anchorMode && cfg_.selfCheck && stallEvents_ >= 2 && placedLeftMs_ <= 0.0)
        {
            // The straight task was re-issued after a stall and stalled again: nothing the tasks do moves this ped. R3 T2: before the
            // rigid follow, the ENGINE is asked to repair the ped (clear its tasks and flags; then the lean and the alt creation
            // recipe), one step per failure, up to maxRepairs; the ladder starts again from the top after each (a repair resets it).
            // Only then the pose is followed rigidly (a slide in an idle pose, but never a drift); tried again after the timer, doubling
            // per failure, with the long dead timers once the repairs are used up.
            if (writeVerdict_ != WriteVerdict::Harm && in.repairsDone < cfg_.maxRepairs && LadderMayChange(true))
            {
                repairPending_ = in.repairsDone == 0 ? LocoRepair::ClearTasks : in.repairsDone == 1 ? LocoRepair::RespawnLean : LocoRepair::RespawnAlt;
                repairRespawn_ = repairPending_ != LocoRepair::ClearTasks;
                ++stats_.repairs;
                stallEvents_ = 0;
                lastIssueWasStall_ = false;
                // the repaired ped starts on the straight task (the anchor task has already failed on this puppet: the next try of
                // it is the ordinary recovery timer's); one stall pair later it is known whether the repair worked
                anchorFallbackLeftMs_ = cfg_.anchorFallbackMs;
                anchorFailStreak_ = 0;
                sinceAnchorStallMs_ = 1e9;
                forceIssue_ = true;
            }
            else if (LadderMayChange(true))
            {
                if (sincePlacedMs_ > 300000.0) placedFailStreak_ = 0;
                const bool spent = in.repairsDone >= cfg_.maxRepairs;
                double dur = spent ? cfg_.deadPlacedMs : cfg_.placedMs;
                const double durMax = spent ? cfg_.deadPlacedMaxMs : cfg_.placedDurMaxMs;
                for (int i = 0; i < placedFailStreak_ && dur < durMax; ++i) dur *= 2.0;
                if (dur > durMax) dur = durMax;
                ++placedFailStreak_;
                sincePlacedMs_ = 0.0;
                placedLeftMs_ = dur;
                pendingWhy_ = LocoLevelWhy::StraightStuck;
            }
        }
        if (stallNow)
        {
            lagMs_ = 0.0;
            // what the regression of speed on rate saw while the task was stalled says nothing about the native
            rpW_ = rpX_ = rpY_ = rpXX_ = rpXY_ = 0.0;
            rateIneffMs_ = 0.0;
        }
        if (stallNow && anchorMode && LadderMayChange(true))
        {
            pendingWhy_ = lostNow ? LocoLevelWhy::AnchorLost : (stuckNow || stormNow) ? LocoLevelWhy::AnchorStuck : LocoLevelWhy::AnchorDragging;
            // The anchor task is lost and re-issuing it may not cure it (no navmesh, an interior): the same stall as a
            // straight run's, but fall back to the straight path for a while instead of repeating.
            // Sync A: a go-to-entity that keeps failing (the entity task never runs on this machine / in this place) must not be
            // retried every 20 s for a lifetime: the fallback doubles for each stall in a row.
            if (sinceAnchorStallMs_ > cfg_.anchorStreakResetMs) anchorFailStreak_ = 0;
            double fb = cfg_.anchorFallbackMs;
            for (int i = 0; i < anchorFailStreak_ && fb < cfg_.anchorFallbackMaxMs; ++i) fb *= 2.0;
            if (fb > cfg_.anchorFallbackMaxMs) fb = cfg_.anchorFallbackMaxMs;
            ++anchorFailStreak_;
            sinceAnchorStallMs_ = 0.0;
            anchorFallbackLeftMs_ = fb;
            ++stats_.anchorFallbacks;
            anchorMode = false;
            o.anchorMode = false;
            wantTask = LocoTask::GoStraight;
        }
        why = turn ? LocoWhy::Turn : driftNow ? LocoWhy::Drift : arriving ? LocoWhy::Arrive : stallNow ? LocoWhy::Stall : LocoWhy::None;
    }
    if (soft)
    {
        if (issue && why != LocoWhy::None) ++stats_.softReissued;
        else ++stats_.softKept;
    }
    if (issue)
    {
        o.why = why;
        switch (why)
        {
        case LocoWhy::Gait:   ++stats_.whyGait;   break;
        case LocoWhy::Reset:  ++stats_.whyReset;  break;
        case LocoWhy::Forced: ++stats_.whyForced; break;
        case LocoWhy::Blend:  ++stats_.whyBlend;  break;
        case LocoWhy::Turn:   ++stats_.whyTurn;   break;
        case LocoWhy::Drift:  ++stats_.whyDrift;  break;
        case LocoWhy::Arrive: ++stats_.whyArrive; break;
        case LocoWhy::Stall:  ++stats_.whyStall;  break;
        case LocoWhy::None:   break;
        }
        o.task = wantTask;
        o.target = wantTask == LocoTask::GoToAnchor ? o.anchorPos : target;
        o.moveBlendRatio = blend;
        lastTask_ = wantTask;
        lastIssueWasStall_ = why == LocoWhy::Stall;
        steadyMs_ = 0;
        lastTaskGait_ = gait_;
        lastTarget_ = target;
        lastAnchor_ = anchor;
        lastTargetVel_ = vf_;
        lastLeadM_ = static_cast<float>(leadM);
        lastBlend_ = blend;
        blendSet_ = blend;
        sinceIssueMs_ = 0;
        sinceIssueGaitMs_ = 0.0;     // R3 T2: the latency to the first gait read is measured from here
        if (cfg_.selfCheck && cfg_.quietTrial && !quietInitialDone_ && writeVerdict_ == WriteVerdict::Unknown && gait_ != Gait::Still)
        {
            // the FIRST moving task of the puppet runs without a coordinate write for its first-gait window: if it shows nothing,
            // the writes cannot be why (no quiet window to pay for), and if it does, the writes that follow are what is judged
            quietInitialDone_ = true;
            quietInitial_ = true;
            quietInitialLeftMs_ = double(cfg_.quietInitialMs);
            quietAbortErr_ = pedErrH + 1.25 * sp * double(cfg_.quietInitialMs) / 1000.0 + 0.5;
        }
        mismatchMs_ = 0;
        forceIssue_ = false;
        resetForced_ = false;
        ++stats_.goStraight;
        if (wantTask == LocoTask::GoToAnchor) ++stats_.anchorIssues;
    }
    else
    {
        // W11 item 1: the running task keeps its issued speed; the commanded gait's blend ratio is set on it (every frame
        // it differs, the task may reassert its own). Only with the move rate override: without it the blend ratio is the
        // quantised speed and the Blend rule above re-issues.
        // Held every frame it differs from the issued one (the task may reassert its own), and once more when it is back at
        // the issued one (what was set last stays).
        if (blendCfg)
        {
            // R4 T1: the blend ratio is the continuous speed knob: sent to the running task when it moved a step (cfg_.blendSetStep)
            if (std::fabs(blend - blendSet_) >= cfg_.blendSetStep)
            {
                o.setBlend = true;
                o.moveBlendRatio = blend;
                blendSet_ = blend;
            }
        }
        else if ((rateAvail || rateLadderOff) && (std::fabs(blend - lastBlend_) > 1e-4f || std::fabs(blend - blendSet_) > 1e-4f))
        {
            o.setBlend = true;
            o.moveBlendRatio = blend;
            blendSet_ = blend;
        }
    }
    return o;
}

// R4 T1: what the ped did against the velocity written the frame before, how fast the error closes, the self-check that sends the blend to
// the rigid follow when the writes do not carry the ped, and (BlendDrive) the velocity itself.
void LocomotionController::BlendWatch(const LocoInput& in, double dt, bool snapped, bool active, bool graceHeld)
{
    const double dtS = dt / 1000.0;
    const double errH = HLen(double(in.pos.x) - in.pedPos.x, double(in.pos.y) - in.pedPos.y);
    if (lastBlendWrite_ && !snapped && !in.teleported && havePrevPed_ && dt >= 4.0 && dt <= 250.0)
    {
        // the ped's own displacement over the frame (positions, net of nothing: Blend writes no coordinate except a snap) and the velocity
        // the game reports, each against what was written. Two readings because either can lie (a read that returns our own write; a
        // displacement stopped by a wall): the share of frames either differs by more than blendFightMs is the "fight".
        const double ivx = (double(in.pedPos.x) - prevPedAfter_.x) / dtS, ivy = (double(in.pedPos.y) - prevPedAfter_.y) / dtS;
        const bool fightPos = HLen(ivx - lastPush_.x, ivy - lastPush_.y) > cfg_.blendFightMs;
        const bool fightVel = in.havePedVel && HLen(double(in.pedVel.x) - lastPush_.x, double(in.pedVel.y) - lastPush_.y) > cfg_.blendFightMs;
        ++stats_.fightCmpFrames;
        if (fightPos) ++stats_.fightPosFrames;
        if (fightVel) ++stats_.fightVelFrames;
        // the share over a leaky window (half a second of frames): a ped that does nothing it was told is seen in a dozen frames
        const double dk = std::exp(-dt / 600.0);
        fightF_ = fightF_ * dk + (fightPos || fightVel ? 1.0 : 0.0);
        fightN_ = fightN_ * dk + 1.0;
        fightShare_ = fightN_ >= 8.0 ? fightF_ / fightN_ : 0.0;
    }
    if (havePrevErr_ && !snapped && !in.teleported && dt >= 4.0 && dt <= 250.0)
    {
        const double inst = (prevErrH_ - errH) / dtS;
        closeRate_ += (inst - closeRate_) * (1.0 - std::exp(-dt / 300.0));
    }
    else closeRate_ = 0.0;
    prevErrH_ = snapped ? 0.0 : errH;
    havePrevErr_ = true;
    if (!cfg_.selfCheck) return;
    const bool storm = snapStorm_;
    snapStorm_ = false;                      // (a snap storm in Blend is a tracking failure, not a stalled task)
    // two ways to a failure: a big error that does not close, or a moderate one that does not close while nearly every frame shows the ped
    // doing other than the velocity written (the writes are ignored: no reason to wait for the error to grow)
    const bool probation = probationLeftMs_ > 0.0;   // R5: the first seconds after a way back up from the rigid follow
    const double es = probation ? double(cfg_.blendProbationErrScale) : 1.0;
    const bool slowFail = errH > cfg_.blendFailErrM * es && closeRate_ < cfg_.blendFailCloseMs;
    const bool fightFail = errH > cfg_.blendFightErrM * es && fightShare_ > cfg_.blendFightShare && closeRate_ < 0.3;
    const double budgetMs = probation ? double(cfg_.blendProbationFailMs) : double(cfg_.blendFailMs);
    if (graceHeld && !snapped && (slowFail || fightFail)) ++stats_.graceHeldFrames;   // R5: a frame of a fall that a grace window did not let happen
    if (active && !snapped && (slowFail || fightFail)) blendFailMs_ += dt * (fightFail ? 2.0 : 1.0);
    else blendFailMs_ = blendFailMs_ > 2.0 * dt ? blendFailMs_ - 2.0 * dt : 0.0;
    if (active && (blendFailMs_ >= budgetMs || storm) && LadderMayChange(true))
    {
        // the writes do not carry the ped (or something holds it): the rigid follow, whose per-frame step is capped at the pose's speed +
        // blendHardCapMs, never a bigger push. It is tried again after the timer (doubling per failure in a row).
        // R5: a fall soon after a task-changing event is the event's (the short hold, the probation retry); any other is the writes' own (the long one)
        const bool explained = stats_.graceWindows > 0 && sinceGraceMs_ < cfg_.blendAttributeMs;
        const double streakResetMs = explained && cfg_.placedExplainedResetMs > 0.0f ? double(cfg_.placedExplainedResetMs) : 300000.0;
        if (sincePlacedMs_ > streakResetMs) placedFailStreak_ = 0;
        const bool longHold = !explained && cfg_.placedUnexplainedMs > 0.0f;
        double dur = longHold ? double(cfg_.placedUnexplainedMs) : double(cfg_.placedMs);
        const double durMax = longHold ? double(cfg_.placedUnexplainedMaxMs) : double(cfg_.placedDurMaxMs);
        for (int i = 0; i < placedFailStreak_ && dur < durMax; ++i) dur *= 2.0;
        if (dur > durMax) dur = durMax;
        ++placedFailStreak_;
        sincePlacedMs_ = 0.0;
        placedLeftMs_ = dur;
        pendingWhy_ = fightShare_ >= 0.3 ? LocoLevelWhy::BlendFighting : LocoLevelWhy::BlendNoTrack;
        ++stats_.blendDegrades;
        if (probation) ++stats_.probationFalls;
        if (explained) ++stats_.fallsAfterGrace;   // R5: the failure outlasted the window (or a new one began right after)
        else ++stats_.fallsUnexplained;
        blendFailMs_ = 0.0;
        corrAlongLp_ = 0.0;
        closeRate_ = 0.0;
    }
}

void LocomotionController::BlendDrive(const LocoInput& in, const LocoVec& ped, double dt, LocoOutput& o)
{
    const bool moving = gait_ != Gait::Still;
    const double hx = double(in.pos.x) - ped.x, hy = double(in.pos.y) - ped.y;
    const double he = HLen(hx, hy);
    const double dead = moving ? cfg_.blendDeadM : cfg_.blendStillDeadM;
    o.blend = true;
    const double spd = HLen(vf_.x, vf_.y);
    if (!moving && he < dead)
    {
        o.setVelocity = false;               // a standing ped near its pose: the game's idle owns it
        corrAlongLp_ *= 0.9;
        return;
    }
    // FiveM's blender: velocity = target velocity + clamp((predicted - object position) / blendSmoothTime, maxVelDiffFromTarget)
    double cx = 0, cy = 0;
    const double capC = moving ? cfg_.blendMaxDiffMs : cfg_.blendStillMaxMs;
    if (he >= dead && he > 1e-6)
    {
        const double tS = cfg_.blendSmoothS > 0.01f ? double(cfg_.blendSmoothS) : 0.01;
        double cl = he / tS;
        if (cl >= capC) { cl = capC; ++stats_.satFrames; }
        cx = hx / he * cl;
        cy = hy / he * cl;
    }
    double vx = double(vf_.x) + cx, vy = double(vf_.y) + cy;
    // the ceiling: whatever the base and the correction add up to, never more than blendHardCapMs beyond the pose's own (raw) velocity
    const double ddx = vx - in.vel.x, ddy = vy - in.vel.y, dl = HLen(ddx, ddy);
    if (dl > cfg_.blendHardCapMs && dl > 0.0)
    {
        vx = in.vel.x + ddx * cfg_.blendHardCapMs / dl;
        vy = in.vel.y + ddy * cfg_.blendHardCapMs / dl;
    }
    o.velocity.x = static_cast<float>(vx);
    o.velocity.y = static_cast<float>(vy);
    // vertical: the game's physics owns it (a read-and-rewrite of its own z velocity) unless the pose is far above / below the ped
    double vz = in.vel.z;
    o.velocityZFromPhysics = true;
    const double dz = double(in.pos.z) - ped.z;
    if (std::fabs(dz) > cfg_.zDeadbandM)
    {
        double cz = cfg_.zGain * (dz > 0 ? dz - cfg_.zDeadbandM : dz + cfg_.zDeadbandM);
        if (cz > cfg_.zCorrMaxMs) cz = cfg_.zCorrMaxMs;
        if (cz < -cfg_.zCorrMaxMs) cz = -cfg_.zCorrMaxMs;
        vz += cz;
        o.velocityZFromPhysics = false;
    }
    o.velocity.z = static_cast<float>(vz);
    o.corrMs = static_cast<float>(HLen(vx - in.vel.x, vy - in.vel.y));
    if (moving && spd >= 1.0) BlendTrimStep((cx * vf_.x + cy * vf_.y) / spd, dt, he);
    else corrAlongLp_ *= 0.95;
}

// The trim of the move blend ratio: a task that runs faster than the pose is braked by the writes (the mean correction along the track is
// negative) and its blend ratio falls until the writes need not brake it; one that is slower is the mirror image. `along` is the correction
// along the pose's direction (m/s) this frame: the blend's own, or at the Placed level the error that the rigid follow closes.
void LocomotionController::BlendTrimStep(double along, double dt, double errM)
{
    corrAlongLp_ += (along - corrAlongLp_) * (1.0 - std::exp(-dt / 400.0));
    // R5: a grace window, or a catch-up of a big error, is no evidence about the speed knob (the average still follows, the integral waits)
    const bool holdTrim = cfg_.blendTrimGateM > 0.0f && (graceLeftMs_ > 0.0 || errM >= double(cfg_.blendTrimGateM));
    if (!holdTrim && accLp_ < cfg_.steadyAccMs2 && sinceMoveStartMs_ >= 600.0)
    {
        const double ex = std::fabs(corrAlongLp_) - cfg_.blendTrimDeadMs;
        if (ex > 0.0)
        {
            blendTrim_ = static_cast<float>(blendTrim_ + (corrAlongLp_ < 0 ? -1.0 : 1.0) * cfg_.blendTrimGain * ex * dt / 1000.0);
            if (blendTrim_ < cfg_.blendTrimMin) blendTrim_ = cfg_.blendTrimMin;
            if (blendTrim_ > cfg_.blendTrimMax) blendTrim_ = cfg_.blendTrimMax;
        }
    }
}

void LocoDiag::Observe(const LocoDiagFrame& f)
{
    ++frames_;
    const double hx = double(f.pose.x) - f.pedPos.x, hy = double(f.pose.y) - f.pedPos.y;
    const double err = HLen(hx, hy);
    errSum_ += err;
    if (err > errMax_) errMax_ = err;
    {
        int bin = static_cast<int>(err / 0.05);
        if (bin >= kErrBins) bin = kErrBins - 1;
        ++errHist_[bin];
    }
    if (f.snapped) ++snapFrames_;
    const double dtS = f.dtMs > 0 ? f.dtMs / 1000.0 : 0.0;
    // the ped's speed from its positions: this frame's read against the previous one (the distance it moved in dt)
    double derived = 0;
    const double gapMs = f.nowMs - prevNowMs_;
    const bool had = havePrev_ && gapMs > 0.0 && gapMs <= 250.0;
    if (had)
        derived = HLen(double(f.pedPos.x) - prevPed_.x, double(f.pedPos.y) - prevPed_.y) / (gapMs / 1000.0);
    // R5 telemetry: headings (GTA: 0 = +y, counter-clockwise, so a direction (vx, vy) points at heading atan2(-vx, vy)). A frame that snapped moved the ped
    // by the snap, not by walking: no own-velocity sample from it.
    if (f.haveHeading && f.poseSpeed > 1.0f)
    {
        headErr_.Add(std::fabs(WrapDeg180(double(f.pedHeading) - double(f.poseHeadingDeg))));
        const double pa = std::atan2(-double(f.poseVel.x), double(f.poseVel.y)) * 180.0 / kPi;
        poseVelHead_.Add(std::fabs(WrapDeg180(pa - double(f.pedHeading))));
    }
    if (f.haveHeading && had && !f.snapped && !prevSnapped_ && derived > 1.0)
    {
        const double dvx = (double(f.pedPos.x) - prevPed_.x) / (gapMs / 1000.0), dvy = (double(f.pedPos.y) - prevPed_.y) / (gapMs / 1000.0);
        const double va = std::atan2(-dvx, dvy) * 180.0 / kPi;
        pedVelHead_.Add(std::fabs(WrapDeg180(va - double(f.pedHeading))));
    }
    // R4 T1: what the ped did against the velocity written the frame before (the read-back and the position-derived velocity)
    if (prevPushed_ && !f.snapped && had)
    {
        ++fightCmp_;
        if (f.havePedVel && HLen(double(f.pedVel.x) - prevPush_.x, double(f.pedVel.y) - prevPush_.y) > 1.0) ++fightVel_;
        const double ivx = (double(f.pedPos.x) - prevPed_.x) / (gapMs / 1000.0), ivy = (double(f.pedPos.y) - prevPed_.y) / (gapMs / 1000.0);
        if (HLen(ivx - prevPush_.x, ivy - prevPush_.y) > 1.0) ++fightPos_;
    }
    prevPushed_ = f.pushed && f.blend && !f.snapped;
    prevSnapped_ = f.snapped;
    prevPush_ = f.push;
    prevPed_ = f.pedPos;
    prevNowMs_ = f.nowMs;
    havePrev_ = true;
    if (f.anchorMode)
    {
        const double ad = HLen(double(f.anchor.x) - f.pedPos.x, double(f.anchor.y) - f.pedPos.y);
        anchorSum_ += ad;
        if (ad > anchorMax_) anchorMax_ = ad;
        ++anchorFrames_;
    }
    if (f.pushed && f.blend)
    {
        // the blend's push is measured against the POSE velocity (the base it is built on), not against the ped's own read
        ++blendFrames_;
        const double cm = HLen(double(f.push.x) - f.poseVel.x, double(f.push.y) - f.poseVel.y);
        if (cm > maxCorr_) maxCorr_ = cm;
    }
    if (f.pushed)
    {
        double px = f.push.x, py = f.push.y;
        if (f.havePedVel) { px -= f.pedVel.x; py -= f.pedVel.y; }
        const double pm = HLen(px, py);
        ++pushFrames_;
        pushSum_ += pm;
        if (pm > pushMax_) pushMax_ = pm;
        if (f.gait == Gait::Still) idleSlide_ += pm * dtS;
        else if (f.poseSpeed > 1.0f) pushDist_ += pm * dtS;
    }
    if (f.nudged)
    {
        // T2: a nudge is a displacement the animation did not produce, like a velocity push: it counts toward the skate, and toward
        // the idle slide while the ped is commanded Still.
        const double nm = HLen(f.nudge.x, f.nudge.y);
        ++nudgeFrames_;
        nudgeDist_ += nm;
        if (dtS > 0.0)
        {
            const double nv = nm / dtS;
            nudgeSpeedSum_ += nv;
            if (nv > nudgeSpeedMax_) nudgeSpeedMax_ = nv;
        }
        if (f.gait == Gait::Still) idleSlide_ += nm;
        else if (f.poseSpeed > 1.0f) pushDist_ += nm;
    }
    if (f.rateSet)
    {
        if (rateFrames_ == 0 || f.rate < rateMin_) rateMin_ = f.rate;
        if (rateFrames_ == 0 || f.rate > rateMax_) rateMax_ = f.rate;
        ++rateFrames_;
        rateSum_ += f.rate;
        if (f.rateClamped) ++rateClamped_;
    }
    if (f.poseSpeed > 1.0f && f.selfSpeed >= 0.0f)
    {
        ++selfN_;
        selfSum_ += f.selfSpeed;
    }
    if (f.poseSpeed > 1.0f && had)
    {
        ++moving_;
        totalDist_ += derived * (gapMs / 1000.0);
        derivedSum_ += derived;
        poseSum_ += f.poseSpeed;
        if (derived < 0.3)
        {
            stuckMs_ += gapMs;
            if (stuckMs_ >= 200.0 && !stuckCounted_) { ++stops_; stuckCounted_ = true; }
        }
        else { stuckMs_ = 0; stuckCounted_ = false; }
    }
    else { stuckMs_ = 0; stuckCounted_ = false; }
}

LocoDiagSummary LocoDiag::Summary() const
{
    LocoDiagSummary s;
    s.frames = frames_;
    s.movingFrames = moving_;
    s.stops = stops_;
    s.pushFrames = pushFrames_;
    if (frames_) { s.meanErrM = errSum_ / frames_; s.maxErrM = errMax_; }
    if (pushFrames_) { s.meanPushMs = pushSum_ / pushFrames_; s.maxPushMs = pushMax_; }
    if (totalDist_ > 1e-6) { s.skate = pushDist_ / totalDist_; if (s.skate > 1.0) s.skate = 1.0; }
    s.idleSlideM = idleSlide_;
    if (moving_) { s.derivedSpeedMs = derivedSum_ / moving_; s.poseSpeedMs = poseSum_ / moving_; }
    s.anchorFrames = anchorFrames_;
    if (anchorFrames_) { s.meanAnchorM = anchorSum_ / anchorFrames_; s.maxAnchorM = anchorMax_; }
    s.nudgeFrames = nudgeFrames_;
    s.nudgedM = nudgeDist_;
    if (nudgeFrames_) { s.meanNudgeMs = nudgeSpeedSum_ / nudgeFrames_; s.maxNudgeMs = nudgeSpeedMax_; }
    s.rateFrames = rateFrames_;
    if (rateFrames_) { s.rateMin = rateMin_; s.rateMax = rateMax_; s.rateMean = rateSum_ / rateFrames_; s.rateClampFrac = double(rateClamped_) / rateFrames_; }
    if (selfN_) s.selfSpeedMs = selfSum_ / selfN_;
    s.maxCorrMs = maxCorr_;
    s.blendFrames = blendFrames_;
    s.snapFrames = snapFrames_;
    s.fightVelFrames = fightVel_;
    s.fightPosFrames = fightPos_;
    s.fightCmpFrames = fightCmp_;
    s.headFrames = headErr_.Count();
    s.headErrMeanDeg = headErr_.Mean();
    s.headErrP95Deg = headErr_.Percentile(0.95);
    s.poseVelHeadFrames = poseVelHead_.Count();
    s.poseVelHeadMeanDeg = poseVelHead_.Mean();
    s.poseVelHeadP95Deg = poseVelHead_.Percentile(0.95);
    s.pedVelHeadFrames = pedVelHead_.Count();
    s.pedVelHeadMeanDeg = pedVelHead_.Mean();
    s.pedVelHeadP95Deg = pedVelHead_.Percentile(0.95);
    if (frames_)
    {
        const uint32_t n50 = (frames_ + 1) / 2, n95 = static_cast<uint32_t>(frames_ * 0.95 + 0.999);
        uint32_t acc = 0;
        bool d50 = false, d95 = false;
        for (int i = 0; i < kErrBins && !(d50 && d95); ++i)
        {
            acc += errHist_[i];
            if (!d50 && acc >= n50) { s.errP50M = (i + 1) * 0.05; d50 = true; }
            if (!d95 && acc >= n95) { s.errP95M = (i + 1) * 0.05; d95 = true; }
        }
    }
    return s;
}

void GaitDiag::Observe(const GaitDiagFrame& f)
{
    ++s_.frames;
    // the ped's speed from its positions between two observations (our writes included), a gap over 250 ms is not bridged
    double fromPos = -1.0;
    const double gap = f.nowMs - prevNowMs_;
    if (havePrev_ && gap > 0.0 && gap <= 250.0)
        fromPos = HLen(double(f.pedPos.x) - prevPos_.x, double(f.pedPos.y) - prevPos_.y) / (gap / 1000.0);
    prevPos_ = f.pedPos;
    prevNowMs_ = f.nowMs;
    havePrev_ = true;
    if (f.wrote) ++s_.writeFrames;
    if (f.issued) ++s_.issues;
    if (f.ragdoll) ++s_.ragdollFrames;
    if (f.commanded == Gait::Still) return;
    ++s_.commandedFrames;
    switch (f.read)
    {
    case Gait::Still:  ++s_.readStill;  break;
    case Gait::Walk:   ++s_.readWalk;   break;
    case Gait::Run:    ++s_.readRun;    break;
    case Gait::Sprint: ++s_.readSprint; break;
    }
    if (f.pedStill >= 0) { ++s_.stillAsked; if (f.pedStill) ++s_.stillTrue; }
    poseSum_ += f.poseSpeed;
    if (f.ownSpeed >= 0.0f) { ownSum_ += f.ownSpeed; ++s_.ownN; }
    if (f.entitySpeed >= 0.0f) { entitySum_ += f.entitySpeed; ++s_.entityN; }
    if (fromPos >= 0.0) { posSum_ += fromPos; ++s_.posN; }
    if (f.blendSet) { blendSum_ += f.moveBlend; ++s_.blendFrames; }
}

GaitDiagSummary GaitDiag::Summary() const
{
    GaitDiagSummary s = s_;
    if (s.commandedFrames) s.poseMs = poseSum_ / s.commandedFrames;
    if (s.ownN) s.ownMs = ownSum_ / s.ownN;
    if (s.entityN) s.entityMs = entitySum_ / s.entityN;
    if (s.posN) s.posMs = posSum_ / s.posN;
    if (s.blendFrames) s.blendMean = blendSum_ / s.blendFrames;
    return s;
}

bool LevelWitness::Report(LocoLevelWhy why, int id, double nowMs)
{
    int k = -1;
    if (why == LocoLevelWhy::AnchorLost || why == LocoLevelWhy::AnchorStuck || why == LocoLevelWhy::AnchorDragging) k = 0;
    else if (why == LocoLevelWhy::RateIneffective) k = 1;
    else if (why == LocoLevelWhy::BlendFighting) k = 2;      // R4 T1
    if (k < 0) return false;
    Slot& s = slot_[k];
    if (s.have && s.id != id && nowMs - s.atMs <= kWitnessMs)
    {
        s.untilMs = nowMs + (k == 0 ? kImposeStraightMs : k == 1 ? kImposeNoRateMs : kImposePlacedMs);
        s.have = false;
        ++globals_;
        return true;
    }
    s.have = true;
    s.id = id;
    s.atMs = nowMs;
    return false;
}

double LevelWitness::ActiveMs(LocoLevel level, double nowMs) const
{
    const int k = level == LocoLevel::Straight ? 0 : level == LocoLevel::NoRate ? 1 : level == LocoLevel::Placed ? 2 : -1;
    if (k < 0) return 0.0;
    return slot_[k].untilMs > nowMs ? slot_[k].untilMs - nowMs : 0.0;
}

bool AimGate::ShouldIssue(const LocoVec& aim, const LocoVec& ped, Gait gait, bool haveTask, bool locoIssuedGoStraight,
                          bool airborne)
{
    if (airborne) return false; // nothing is remembered: whatever is pending is decided again on landing
    bool issue = !haveTask || gait != lastGait_ || locoIssuedGoStraight;
    if (!issue)
    {
        const double dx = double(aim.x) - lastAim_.x, dy = double(aim.y) - lastAim_.y, dz = double(aim.z) - lastAim_.z;
        const double ax = double(aim.x) - ped.x, ay = double(aim.y) - ped.y, az = double(aim.z) - ped.z;
        const double tol = std::fmax(double(moveM_), double(distFrac_) * std::sqrt(ax * ax + ay * ay + az * az));
        issue = std::sqrt(dx * dx + dy * dy + dz * dz) > tol;
    }
    if (issue)
    {
        lastAim_ = aim;
        lastGait_ = gait;
    }
    return issue;
}

}
