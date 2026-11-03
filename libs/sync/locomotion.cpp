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
}

void LocomotionController::SoftReset()
{
    // Keep lastTask_ and its target: Update decides whether the running task still serves.
    reevaluate_ = true;
    ++stats_.softResets;
}

void LocomotionController::ResetHard()
{
    Reset();
    started_ = false;
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
        ++stats_.levelChanges;
        ++stats_.levelWhy[static_cast<int>(o.levelWhy)];
        level_ = cur;
    }
    pendingWhy_ = LocoLevelWhy::None;
    return o;
}

void LocomotionController::Degrade(LocoLevel to, LocoLevelWhy why, double ms)
{
    // The engine passes on what another puppet learned (the game is the same game for all of them): a level, for a while.
    if (!cfg_.selfCheck || ms <= 0.0) return;
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
    if (anchorFallbackLeftMs_ > 0.0) anchorFallbackLeftMs_ = anchorFallbackLeftMs_ > dt ? anchorFallbackLeftMs_ - dt : 0.0;
    // T2: the fallback timers of the ladder run on, and the ped's own displacement is measured (position differences net of
    // what we wrote last frame): the one signal that needs no model of what SET_ENTITY_VELOCITY or a task really does.
    sinceRateFailMs_ += dt;
    sincePlacedMs_ += dt;
    if (rateOffLeftMs_ > 0.0) rateOffLeftMs_ = rateOffLeftMs_ > dt ? rateOffLeftMs_ - dt : 0.0;
    if (placedLeftMs_ > 0.0) placedLeftMs_ = placedLeftMs_ > dt ? placedLeftMs_ - dt : 0.0;
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
        if (gait_ == Gait::Still && cand != Gait::Still) sinceMoveStartMs_ = 0;
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
    if (wasJumping_ && !jumping) forceIssue_ = true; // landed: the jump replaced the walk task
    wasJumping_ = jumping;

    if (in.ragdoll && !ragdollActive_) { o.ragdollStart = true; ragdollActive_ = true; }
    else if (!in.ragdoll && ragdollActive_) { o.ragdollEnd = true; ragdollActive_ = false; forceIssue_ = true; }

    // ---- snap ----
    LocoVec ped = in.pedPos;
    const double ex = double(in.pos.x) - ped.x, ey = double(in.pos.y) - ped.y, ez = double(in.pos.z) - ped.z;
    const double dist = std::sqrt(ex * ex + ey * ey + ez * ez);
    const double speed3 = std::sqrt(double(in.vel.x) * in.vel.x + double(in.vel.y) * in.vel.y + double(in.vel.z) * in.vel.z);
    bool snap = !started_ || in.teleported || dist > cfg_.snapBaseM + cfg_.snapPerSpeedS * speed3;
    if (!snap)
    {
        offAcc_ += static_cast<float>((dist - cfg_.offToleranceM) * dtS);
        if (offAcc_ < 0.0f) offAcc_ = 0.0f;
        if (offAcc_ >= cfg_.offLimitMs) { snap = true; ++stats_.persistentSnaps; }
    }
    const bool wasStarted = started_;
    started_ = true;
    if (snap && wasStarted && !in.teleported && cfg_.selfCheck)
    {
        // a snap that is neither the first nor a teleport: the ped was not carried to the pose. Three within 6 s are a stall.
        if (sinceFailSnapMs_ > 6000.0) failSnaps_ = 0;
        ++failSnaps_;
        sinceFailSnapMs_ = 0.0;
        if (failSnaps_ >= cfg_.snapStormN) { snapStorm_ = true; failSnaps_ = 0; }
    }
    if (snap)
    {
        o.snap = true;
        o.snapPos = in.pos;
        ped = in.pos;
        offAcc_ = 0;
        forceIssue_ = true;
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
        const bool nudgeMode = cfg_.push == PushMode::Nudge && !jumping && !ragdollActive_ && !in.swimming;
        const bool residual = !nudgeMode && cfg_.residualMaxMs > 0.0f && in.havePedVel && !jumping && !ragdollActive_ && !in.swimming;
        if (!residual || snap) { corrFx_ = corrFy_ = 0.0f; }
        o.setVelocity = true;
        if (snap)
        {
            o.velocity = in.vel;
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
            const double cap = placed ? cfg_.placedNudgeMaxMs : (moving ? cfg_.nudgeMaxMs : cfg_.nudgeStillMaxMs);
            // the start of a movement: the clip is still getting up to speed, a bounded lag is tolerated (Sync A's grace, as a band)
            if (moving && !placed && sinceMoveStartMs_ < cfg_.startGraceMs && cfg_.nudgeStartGraceM > dead)
            {
                dead = cfg_.nudgeStartGraceM;
                if (he < dead) ++stats_.graceFrames;
            }
            double dx = 0, dy = 0, dzv = 0;
            if (he > dead)
            {
                double mag = (he - dead) * (1.0 - std::exp(-dt / (tau > 1.0 ? tau : 1.0)));
                const double maxStep = cap * dtS;
                if (mag > maxStep) mag = maxStep;
                dx = hx / he * mag;
                dy = hy / he * mag;
            }
            const double dz = double(in.pos.z) - ped.z;
            if (std::fabs(dz) > cfg_.nudgeZBandM)
            {
                double cz = cfg_.zGain * (dz > 0 ? dz - cfg_.nudgeZBandM : dz + cfg_.nudgeZBandM);
                if (cz > cfg_.zCorrMaxMs) cz = cfg_.zCorrMaxMs;
                if (cz < -cfg_.zCorrMaxMs) cz = -cfg_.zCorrMaxMs;
                dzv = cz * dtS;
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
        if (rateAvail && !in.swimming) { o.setMoveRate = true; o.moveRate = lastRate_; }
        return o;
    }
    if (gait_ == Gait::Still)
    {
        mismatchMs_ = 0;
        reevaluate_ = false;      // nothing to keep or re-issue: the stand-still task is the running one
        gaitShown_ = Gait::Still;
        if (lastTask_ != LocoTask::StandStill || forceIssue_)
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
    anchor.x = static_cast<float>(in.pos.x + (double(in.pos.x) - ped.x));
    anchor.y = static_cast<float>(in.pos.y + (double(in.pos.y) - ped.y));
    LocoVec target;
    target.x = static_cast<float>(anchor.x + dirx * leadM);
    target.y = static_cast<float>(anchor.y + diry * leadM);
    target.z = in.pos.z;

    // Animation speed. The override scales the gait animation to the pose speed (rate = speed over the
    // gait's nominal speed, clamped); the task's blend ratio is then the plain gait. Without it the
    // blend ratio itself carries the speed, quantised so it changes rarely.
    float blend = BlendRatio(gait_);
    if (rateAvail && !in.swimming)
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
        if (cfg_.selfCheck && haveSelf_ && !settling && sp >= 1.0 && steadyMs_ >= 500.0 && !jumping && accLp_ < cfg_.rateProbeAccMs2)
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
            if (rateIneffMs_ >= cfg_.ratePinMs)
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
        const bool eligible = gait_ != Gait::Still && !settling && !jumping && !ragdollActive_ && !in.swimming && haveSelf_ &&
                              sp >= cfg_.stuckMinPoseMs && steadyMs_ >= cfg_.stuckSettleMs && sinceMoveStartMs_ >= cfg_.stuckSettleMs;
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
            draggingNow = winTravelM_ >= 3.0 && winNudgedM_ > cfg_.nudgeShareMax * winTravelM_;
            winNudgedM_ = winTravelM_ = winMs_ = 0.0;
        }
    }
    else winMs_ = winNudgedM_ = winTravelM_ = 0.0;

    // A stall: the ped shows no gait at all although a moving one is commanded (the task ended or was
    // cancelled by the game). The only gait-mismatch trigger left; the others were churn.
    mismatchMs_ = (in.pedGait == Gait::Still && !in.swimming) ? mismatchMs_ + dt : 0.0;   // a swimmer shows no land gait

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
    if (!issue && !rateAvail && !rateLadderOff && sinceIssueMs_ >= cfg_.blendReissueMs &&
        std::fabs(blend - lastBlend_) >= cfg_.blendStep - 1e-4f)
    {
        issue = true;
        why = LocoWhy::Blend;
    }
    // A SoftReset() evaluates the rules now (no minReissueMs wait) but only the position rules: turn, drift, arrival.
    const bool soft = reevaluate_;
    reevaluate_ = false;
    if (!issue && (soft || sinceIssueMs_ >= cfg_.minReissueMs))
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
        const bool drift = !anchorMode && driftM > cfg_.driftM;
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
        const bool stormNow = snapStorm_;
        const bool stallNow = !soft && (lostNow || stuckNow || stormNow || (draggingNow && anchorMode)) &&
                              (stormNow || sinceIssueMs_ >= (lastIssueWasStall_ ? cfg_.stallRetryMs : cfg_.stallFirstMs));
        issue = turn || driftNow || arriving || stallNow;
        if (stallNow)
        {
            if (sinceStallEventMs_ < 15000.0) ++stallEvents_;
            else stallEvents_ = 1;
            sinceStallEventMs_ = 0.0;
        }
        snapStorm_ = false;
        if (stallNow && !anchorMode && cfg_.selfCheck && stallEvents_ >= 2 && placedLeftMs_ <= 0.0)
        {
            // The straight task was re-issued after a stall and stalled again: nothing the tasks do moves this ped. Follow the pose
            // rigidly for a while (a slide in an idle pose, but never a drift); tried again after the timer, doubling per failure.
            if (sincePlacedMs_ > 300000.0) placedFailStreak_ = 0;
            double dur = cfg_.placedMs;
            for (int i = 0; i < placedFailStreak_ && dur < cfg_.placedDurMaxMs; ++i) dur *= 2.0;
            if (dur > cfg_.placedDurMaxMs) dur = cfg_.placedDurMaxMs;
            ++placedFailStreak_;
            sincePlacedMs_ = 0.0;
            placedLeftMs_ = dur;
            pendingWhy_ = LocoLevelWhy::StraightStuck;
        }
        if (stallNow)
        {
            lagMs_ = 0.0;
            // what the regression of speed on rate saw while the task was stalled says nothing about the native
            rpW_ = rpX_ = rpY_ = rpXX_ = rpXY_ = 0.0;
            rateIneffMs_ = 0.0;
        }
        if (stallNow && anchorMode)
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
        if ((rateAvail || rateLadderOff) && (std::fabs(blend - lastBlend_) > 1e-4f || std::fabs(blend - blendSet_) > 1e-4f))
        {
            o.setBlend = true;
            o.moveBlendRatio = blend;
            blendSet_ = blend;
        }
    }
    return o;
}

void LocoDiag::Observe(const LocoDiagFrame& f)
{
    ++frames_;
    const double hx = double(f.pose.x) - f.pedPos.x, hy = double(f.pose.y) - f.pedPos.y;
    const double err = HLen(hx, hy);
    errSum_ += err;
    if (err > errMax_) errMax_ = err;
    const double dtS = f.dtMs > 0 ? f.dtMs / 1000.0 : 0.0;
    // the ped's speed from its positions: this frame's read against the previous one (the distance it moved in dt)
    double derived = 0;
    const double gapMs = f.nowMs - prevNowMs_;
    const bool had = havePrev_ && gapMs > 0.0 && gapMs <= 250.0;
    if (had)
        derived = HLen(double(f.pedPos.x) - prevPed_.x, double(f.pedPos.y) - prevPed_.y) / (gapMs / 1000.0);
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
    return s;
}

bool LevelWitness::Report(LocoLevelWhy why, int id, double nowMs)
{
    int k = -1;
    if (why == LocoLevelWhy::AnchorLost || why == LocoLevelWhy::AnchorStuck || why == LocoLevelWhy::AnchorDragging) k = 0;
    else if (why == LocoLevelWhy::RateIneffective) k = 1;
    if (k < 0) return false;
    Slot& s = slot_[k];
    if (s.have && s.id != id && nowMs - s.atMs <= kWitnessMs)
    {
        s.untilMs = nowMs + (k == 0 ? kImposeStraightMs : kImposeNoRateMs);
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
    const int k = level == LocoLevel::Straight ? 0 : level == LocoLevel::NoRate ? 1 : -1;
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
