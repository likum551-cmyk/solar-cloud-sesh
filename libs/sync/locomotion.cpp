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
    if (anchorFallbackLeftMs_ > 0.0) anchorFallbackLeftMs_ = anchorFallbackLeftMs_ > dt ? anchorFallbackLeftMs_ - dt : 0.0;
    // W11 item 2: anchor mode now? (the engine reports no anchor entity or an aim task with noAnchor; a stall dropped it)
    bool anchorMode = cfg_.mode == LocoMode::Anchor && !in.noAnchor && anchorFallbackLeftMs_ <= 0.0;

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

    // ---- gait: speed first, the sender's gait only as a hint ----
    // inferred_ is the speed's gait with hysteresis. The sender's moveState (0 for swimming, climbing,
    // cover, strafing: PedGait() has no gait for them) may only lift it by one: ApplyAuthority. Going
    // Still needs the speed to stay under idleDown for stillHoldMs (a one-sample dip must not fire
    // StandStill then GoStraight), and a change between moving gaits has to hold for gaitDebounceMs
    // (leaving Still is immediate: a ped sliding in an idle pose is the worse error).
    inferred_ = InferGait(inferred_, static_cast<float>(rawSpeed), cfg_);
    Gait cand = ApplyAuthority(inferred_, in.moveState);
    bool holding = false;
    if (cand == Gait::Still && gait_ != Gait::Still)
    {
        stillMs_ += dt;
        if (stillMs_ < cfg_.stillHoldMs) { cand = gait_; holding = true; }
    }
    else stillMs_ = 0;
    if (cand != gait_ && cand != Gait::Still && gait_ != Gait::Still)
    {
        if (cand != pendingGait_) { pendingGait_ = cand; pendingMs_ = 0; }
        pendingMs_ += dt;
        if (pendingMs_ < cfg_.gaitDebounceMs) cand = gait_;
    }
    else { pendingGait_ = gait_; pendingMs_ = 0; }
    gait_ = cand;
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
    started_ = true;
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
        const bool residual = cfg_.residualMaxMs > 0.0f && in.havePedVel && !jumping && !ragdollActive_ && !in.swimming;
        if (!residual || snap) { corrFx_ = corrFy_ = 0.0f; }
        o.setVelocity = true;
        if (snap)
        {
            o.velocity = in.vel;
        }
        else if (residual)
        {
            const double hx = double(in.pos.x) - ped.x, hy = double(in.pos.y) - ped.y;
            const double he = HLen(hx, hy);
            double cx = 0, cy = 0;
            if (he >= cfg_.steerDeadbandM)
            {
                cx = (cfg_.corrGainBase + cfg_.corrGainPerM * he) * hx;
                cy = (cfg_.corrGainBase + cfg_.corrGainPerM * he) * hy;
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
                o.velocity.x = static_cast<float>(in.pedVel.x + corrFx_);
                o.velocity.y = static_cast<float>(in.pedVel.y + corrFy_);
                o.velocity.z = static_cast<float>(in.pedVel.z + cz);
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
        const double aLead = Clamp(sp * cfg_.anchorLeadS, cfg_.anchorLeadMinM, cfg_.anchorLeadMaxM);
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
        return o; // about to stand still: let the running task finish, issue nothing new
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
    const double leadM = Clamp(sp * cfg_.leadS, cfg_.leadMinM, cfg_.leadMaxM);
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
    if (in.moveRateAvailable && !in.swimming)
    {
        o.setMoveRate = true;
        o.moveRate = static_cast<float>(Clamp(sp / Nominal(gait_, cfg_), cfg_.moveRateMin, cfg_.moveRateMax));
    }
    else
        blend = QuantisedBlend(sp, gait_, cfg_);

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
    if (!issue && !in.moveRateAvailable && sinceIssueMs_ >= cfg_.blendReissueMs &&
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
        const bool stallNow = !soft && mismatchMs_ >= cfg_.stallMs && sinceIssueMs_ >= cfg_.stallRetryMs;
        issue = turn || driftNow || arriving || stallNow;
        if (stallNow && anchorMode)
        {
            // The anchor task is lost and re-issuing it may not cure it (no navmesh, an interior): the same stall as a
            // straight run's, but fall back to the straight path for a while instead of repeating.
            anchorFallbackLeftMs_ = cfg_.anchorFallbackMs;
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
        if (in.moveRateAvailable && (std::fabs(blend - lastBlend_) > 1e-4f || std::fabs(blend - blendSet_) > 1e-4f))
        {
            o.setBlend = true;
            o.moveBlendRatio = blend;
            blendSet_ = blend;
        }
    }
    return o;
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
