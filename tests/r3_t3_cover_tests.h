// R3 T3 "cover" (desync overhaul round 3): the pure tests, included by gameplay_test.cpp after Check() / F() are defined.
//
// WHAT THEY MODEL (every decision is the real libs/sync/cover.h / pedstate.h code; only the GAME is simulated):
//   Ped      the puppet: a position, a heading, a cover task (running / accepted), the locomotion pipeline closing the error to the pose at
//            `locoSpeed`, and a snap/teleport tally by CAUSE (the pipeline, the cover driver, the game moving the ped onto a cover point).
//   Rules    how the GAME answers TASK_PUT_PED_DIRECTLY_INTO_COVER (all of it a hypothesis, live-unverified, which is why the code must be right
//            for every one of them): a cover point exactly at the coordinates; NO cover point (a car: the task runs and never reads in cover);
//            a point 1.2 m away (the game moves the ped onto it: a snap); a point on the wrong side of the car; a ped that IGNORES tasks (the
//            T2 puppet hypothesis); a slow accept (IS_PED_GOING_INTO_COVER first, IS_PED_IN_COVER 700 ms later).
//   OldDriver the 0d engine code (CoverWindow + DecidePrePlace + the 4 m drift guard), line for line.
//   NewDriver the R3 T3 engine code (CoverPuppet: nudge only within 0.30 m, verified acceptance, pose fallback), line for line.
// "fails on old": each scenario that reproduces a defect asserts the OLD driver shows it AND the NEW driver does not. "mutants": the same
// scenario with one rule of the new window switched off through its config must FAIL the new predicate.
#pragma once

#include "sync/cover.h"
#include "sync/pedstate.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace r3cover {

using namespace gage::sync;

constexpr int kPureChecks = 39;   // asserted by gameplay_test's total

// ---- the sender's pose ---------------------------------------------------------------------------------------------------
struct Frame
{
    float x = 0, y = 0, z = 0, heading = 0, vx = 0, vy = 0;
    bool flag = false, aiming = false, crouch = false;
};
struct Key { double t; float x, y; };
struct Win { double a, b; };
struct Scen
{
    std::vector<Key> keys;
    std::vector<Win> flag, gap, aim;
    double end = 8.0;
    bool crouch = true;           // the sender's low-cover crouch hint (kPfStealth)
    float heading = 0;
    Frame At(double t) const
    {
        Frame f;
        f.heading = heading;
        size_t i = 0;
        while (i + 1 < keys.size() && t > keys[i + 1].t)
            ++i;
        if (i + 1 >= keys.size())
        {
            f.x = keys.back().x; f.y = keys.back().y;
        }
        else
        {
            const Key& a = keys[i];
            const Key& b = keys[i + 1];
            const double u = std::min(1.0, std::max(0.0, (t - a.t) / (b.t - a.t)));
            f.x = (float)(a.x + (b.x - a.x) * u);
            f.y = (float)(a.y + (b.y - a.y) * u);
            if (t >= keys.front().t && t <= keys.back().t)
            {
                f.vx = (float)((b.x - a.x) / (b.t - a.t));
                f.vy = (float)((b.y - a.y) / (b.t - a.t));
            }
        }
        for (const Win& w : flag)
            if (t >= w.a && t < w.b)
                f.flag = true;
        for (const Win& w : gap)
            if (t >= w.a && t < w.b)
                f.flag = false;
        for (const Win& w : aim)
            if (t >= w.a && t < w.b)
                f.aiming = true;
        f.crouch = crouch && f.flag;
        return f;
    }
};

// The sender runs 10 m to a car, is in cover at x = 10 from 1.6 s (still at speed) to 6.0 s, then runs away.
inline Scen RunIn()
{
    Scen s;
    s.keys = { { 0, 0, 0 }, { 1.6, 8.8f, 0 }, { 2.1, 10, 0 }, { 6.0, 10, 0 }, { 7.0, 10, -5.5f } };
    s.flag = { { 1.6, 6.0 } };
    return s;
}

// ---- the game ------------------------------------------------------------------------------------------------------------
enum class Game { PointAtPose, NoPoint, PointOffset, WrongSide, IgnoresTasks, SlowAccept };
struct Rules
{
    Game  game = Game::PointAtPose;
    float offset = 1.2f;          // PointOffset: how far from the coordinates the cover point is
    double acceptMs = 30, goingMs = 20, slowAcceptMs = 700;
    float locoSpeed = 6.0f;       // the pipeline's catch-up speed, m/s
    float lagM = 1.2f;            // the puppet starts this far behind the pose
    bool  callable = true;        // TASK_PUT_PED_DIRECTLY_INTO_COVER can be called
};
enum Cause { kLoco = 0, kDriver = 1, kGame = 2 };
struct Ped
{
    float x = 0, y = 0, heading = 0;
    bool  task = false, inCover = false, going = false;
    double taskMs = 0;
    float tx = 0, ty = 0;
    uint32_t taskCalls = 0, clears = 0, stays = 0;
    uint32_t snaps[3] = {}, tele[3] = {};
    float lastMove[3] = {};
    void Move(float nx, float ny, Cause c)
    {
        const float d = std::sqrt((nx - x) * (nx - x) + (ny - y) * (ny - y));
        if (d > 0.4f) ++snaps[c];
        if (d > 1.0f) ++tele[c];
        lastMove[c] = d;
        x = nx; y = ny;
    }
    void Clear() { ++clears; task = inCover = going = false; }
    bool Put(const Rules& r, float px, float py, double now)
    {
        ++taskCalls;
        if (!r.callable)
            return false;
        if (r.game == Game::IgnoresTasks)
            return true;                       // the call works, the ped does nothing with it
        task = true; inCover = going = false; taskMs = now; tx = px; ty = py;
        return true;
    }
    void Step(const Rules& r, double now)      // the game's answer to the task
    {
        if (!task || inCover)
            return;
        const double dt = now - taskMs;
        switch (r.game)
        {
        case Game::PointAtPose:
            going = dt >= r.goingMs;
            if (dt >= r.acceptMs) { Move(tx, ty, kGame); inCover = true; going = false; }
            break;
        case Game::PointOffset:
            if (dt >= r.acceptMs) { Move(tx, ty + r.offset, kGame); inCover = true; }
            break;
        case Game::WrongSide:
            if (dt >= r.acceptMs) { Move(tx, ty + 0.3f, kGame); heading += 180.0f; inCover = true; }
            break;
        case Game::SlowAccept:
            going = dt >= 50.0;
            if (dt >= r.slowAcceptMs) { inCover = true; going = false; }
            break;
        default:
            break;                              // NoPoint: the task runs, the ped never reads in cover
        }
    }
};

// ---- metrics ------------------------------------------------------------------------------------------------------------
struct Metrics
{
    double errSum = 0, errMax = 0, errMaxInCover = 0, frozenNoCoverMs = 0, uprightMs = 0, inCoverMs = 0, errSumInCover = 0;
    uint32_t errN = 0, errNInCover = 0, coverFlagFrames = 0;
    Ped ped;
    CoverStats st;
    double ErrMean() const { return errN ? errSum / errN : 0.0; }
    double ErrMeanInCover() const { return errNInCover ? errSumInCover / errNInCover : 0.0; }
};

// The 4 m drift guard of DrivePeer (tierBHolds): the pose wins over a big drift.
inline void DriftGuard(Ped& p, const Frame& f)
{
    if (std::sqrt((p.x - f.x) * (p.x - f.x) + (p.y - f.y) * (p.y - f.y)) > 4.0f)
        p.Move(f.x, f.y, kDriver);
}

// ---- the 0d engine code, line for line ----------------------------------------------------------------------------------
struct OldDriver
{
    CoverWindow cp;
    Ped& ped;
    const Rules& rules;
    bool crouchShown = false;                 // the 0.4.7 sender sent no crouch hint: the fallback stood upright
    OldDriver(Ped& p, const Rules& r) : ped(p), rules(r) {}
    bool Tick(const Frame& f, double now)
    {
        const bool flag = f.flag;
        bool inCover = false;
        float drift = 0.0f;
        if (cp.Busy())
        {
            inCover = ped.inCover;
            drift = std::sqrt((ped.x - f.x) * (ped.x - f.x) + (ped.y - f.y) * (ped.y - f.y));
        }
        const CoverWindow::Out cv = cp.Update(flag, inCover, drift, now);
        bool failed = cv.failed;
        if (cv.issue)
        {
            const float dist = std::sqrt((ped.x - f.x) * (ped.x - f.x) + (ped.y - f.y) * (ped.y - f.y));
            const PrePlace pp = DecidePrePlace(dist, CoverHeadingErrDeg(ped.heading, f.heading));
            if (pp.clearTasks) ped.Clear();
            if (pp.mode != PrePlaceMode::Keep) ped.Move(f.x, f.y, kDriver);
            if (pp.setHeading) ped.heading = f.heading;
            const bool called = ped.Put(rules, f.x, f.y, now);
            if (!called) { cp.NoteIssueFailed(); failed = true; }
        }
        if (failed) ped.Clear();
        if (cv.ended) ped.Clear();
        if (cv.suspend) DriftGuard(ped, f);
        return cv.suspend;
    }
};

// ---- the R3 T3 engine code, line for line -------------------------------------------------------------------------------
struct NewDriver
{
    CoverPuppet cp;
    CoverBreaker* br;
    Ped& ped;
    const Rules& rules;
    float issueX = 0, issueY = 0;
    bool crouchShown = false;
    NewDriver(Ped& p, const Rules& r, CoverBreaker* b, CoverCfg cfg = CoverCfg(), CoverSyncMode mode = CoverSyncMode::Task)
        : cp(cfg), br(b), ped(p), rules(r) { cp.SetMode(mode); }
    bool Tick(const Frame& f, double now)
    {
        CoverIn in;
        in.flag = f.flag;
        in.aiming = f.aiming;
        in.poseSpeed = std::sqrt(f.vx * f.vx + f.vy * f.vy);
        in.nowMs = now;
        if (cp.NeedsPed(in.flag))
        {
            in.pedReadable = true;
            in.distM = std::sqrt((ped.x - f.x) * (ped.x - f.x) + (ped.y - f.y) * (ped.y - f.y));
            in.headingErrDeg = CoverHeadingErrDeg(ped.heading, f.heading);
            in.movedM = std::sqrt((ped.x - issueX) * (ped.x - issueX) + (ped.y - issueY) * (ped.y - issueY));
        }
        if (cp.Busy())
        {
            in.pedInCover = ped.inCover;
            in.pedGoingInto = ped.going;
        }
        CoverOut cv = cp.Update(in, br);
        if (cv.issue)
        {
            if (cv.nudge)
            {
                ped.Move(f.x, f.y, kDriver);
                if (in.headingErrDeg > kPrePlaceHeadingDeg) ped.heading = f.heading;
            }
            issueX = ped.x; issueY = ped.y;
            if (!ped.Put(rules, f.x, f.y, now))
            {
                const CoverOut x = cp.NoteIssueFailed(now, br);
                cv.why = x.why;
                cv.suspend = false;
            }
        }
        if (cv.stay) ++ped.stays;
        if (cv.clear) ped.Clear();
        crouchShown = cp.CurrentPhase() == CoverPhase::Pose && f.crouch;
        if (cv.suspend) DriftGuard(ped, f);
        return cv.suspend;
    }
};

template <class Driver>
inline Metrics Run(Driver& d, Ped& ped, const Rules& rules, const Scen& s, double dtMs = 1000.0 / 60.0)
{
    Metrics m;
    const Frame f0 = s.At(0.0);
    ped.x = f0.x - rules.lagM;
    ped.y = f0.y;
    for (double now = dtMs; now <= s.end * 1000.0; now += dtMs)
    {
        const Frame f = s.At(now / 1000.0);
        const bool suspend = d.Tick(f, now);
        ped.Step(rules, now);
        if (!suspend && !ped.task)
        {
            // the locomotion pipeline closes the error at locoSpeed; a huge error is its own snap (not the cover driver's)
            const float dx = f.x - ped.x, dy = f.y - ped.y;
            const float e = std::sqrt(dx * dx + dy * dy);
            const float step = rules.locoSpeed * (float)(dtMs / 1000.0);
            if (e > 6.0f) ped.Move(f.x, f.y, kLoco);
            else if (e > step) ped.Move(ped.x + dx / e * step, ped.y + dy / e * step, kLoco);
            else if (e > 0.0f) ped.Move(f.x, f.y, kLoco);
        }
        const float err = std::sqrt((f.x - ped.x) * (f.x - ped.x) + (f.y - ped.y) * (f.y - ped.y));
        if (f.flag)
        {
            ++m.coverFlagFrames;
            m.errSum += err; ++m.errN;
            m.errMax = std::max(m.errMax, (double)err);
            if (suspend && !ped.inCover) m.frozenNoCoverMs += dtMs;
            if (ped.inCover) { m.inCoverMs += dtMs; m.errSumInCover += err; ++m.errNInCover; m.errMaxInCover = std::max(m.errMaxInCover, (double)err); }
            if (f.crouch && !ped.inCover && !d.crouchShown) m.uprightMs += dtMs;
        }
    }
    m.ped = ped;
    return m;
}

struct Out
{
    Metrics m;
    CoverStats st;
    CoverPhase phase = CoverPhase::Idle;
};
inline Metrics RunOld(const Scen& s, const Rules& r)
{
    Ped ped;
    OldDriver d(ped, r);
    return Run(d, ped, r, s);
}
inline Metrics RunNew(const Scen& s, const Rules& r, CoverBreaker* br, CoverCfg cfg = CoverCfg(), CoverSyncMode mode = CoverSyncMode::Task, CoverStats* stOut = nullptr)
{
    Ped ped;
    NewDriver d(ped, r, br, cfg, mode);
    Metrics m = Run(d, ped, r, s);
    m.st = d.cp.Stats();
    if (stOut) *stOut = d.cp.Stats();
    return m;
}

// ---- the sender classifier ---------------------------------------------------------------------------------------------
struct SenderRun
{
    uint32_t sent = 0, sentCrouch = 0;
    bool goingSent = false;
    CoverSenderStats stats;
};

inline void RunPure()
{
    // ===================================================================== SENDER (CoverSense)
    {
        // going-into 0.8 s (the ped runs to the car), in cover 3 s with the head low, aiming from cover 1 s, a 80 ms read flicker, leaving.
        CoverSense cs;
        SenderRun r;
        double t = 0;
        bool noneBeforeIn = true;
        bool holdsFlicker = true, dropsAfterHold = false;
        auto step = [&](bool in, bool going, bool aim, float head, bool* sentOut) {
            CoverReads rd;
            rd.inCover = in; rd.goingInto = going; rd.aimingFrom = aim; rd.headAboveM = head; rd.x = 10; rd.y = 0; rd.z = 30; rd.heading = 90;
            const CoverSenderOut o = cs.Update(rd, t);
            if (sentOut) *sentOut = o.sendCover;
            if (o.sendCover) ++r.sent;
            if (o.sendCrouch) ++r.sentCrouch;
            t += 33.0;
            return o;
        };
        for (int i = 0; i < 24; ++i) { bool s = false; step(false, true, false, -99.0f, &s); noneBeforeIn = noneBeforeIn && !s; }
        r.goingSent = !noneBeforeIn;
        for (int i = 0; i < 90; ++i) step(true, false, false, 0.25f, nullptr);               // 3 s in cover, head low
        for (int i = 0; i < 30; ++i) step(true, false, true, 0.25f, nullptr);                // 1 s aiming from cover (IS_PED_IN_COVER stays true)
        for (int i = 0; i < 2; ++i) { bool s = false; step(false, false, false, -99.0f, &s); holdsFlicker = holdsFlicker && s; }   // 66 ms with no read
        for (int i = 0; i < 20; ++i) step(true, false, false, 0.25f, nullptr);
        int leftAfter = 0;
        bool s2 = false;
        for (int i = 0; i < 12; ++i) { step(false, false, false, -99.0f, &s2); if (s2) ++leftAfter; else { dropsAfterHold = true; } }
        const CoverSenderStats w = cs.Take();
        Check("R3 T3 sender: going-into sends NOTHING (the ped is still running to the car), cover reads send kPfCover, a 66 ms read gap between aiming and cover keeps the flag up",
              !r.goingSent && w.goingReads == 24 && w.inReads == 90 + 30 + 20 && w.aimingReads == 30 && holdsFlicker && w.rises == 1 && w.flickers == 0 && w.sentCover >= 140,
              F("goingSent %d going %u in %u aiming %u holdsFlicker %d rises %u sent %u", (int)r.goingSent, w.goingReads, w.inReads, w.aimingReads, (int)holdsFlicker, w.rises, w.sentCover));
        Check("R3 T3 sender: after the last cover read the flag falls within 150 ms (leaving is not held longer), the hold is counted once with its length",
              dropsAfterHold && leftAfter >= 3 && leftAfter <= 5 && w.falls == 1 && w.holdMsMax > 4000.0 && w.holdMsMax < 5000.0,
              F("leftAfter %d falls %u holdMax %.0f", leftAfter, w.falls, w.holdMsMax));
        Check("R3 T3 sender: a low head in cover sends the crouch hint (kPfStealth) on every cover sample, a standing head does not; counters: head min/max, going episode followed by cover",
              r.sentCrouch >= 140 && w.headMin > 0.24f && w.headMax < 0.26f && w.goingEpisodes == 1 && w.goingThenIn == 1 && w.sentLeavingHold >= 3,
              F("crouch %u head %.2f..%.2f episodes %u thenIn %u leaving %u", r.sentCrouch, (double)w.headMin, (double)w.headMax, w.goingEpisodes, w.goingThenIn, w.sentLeavingHold));
        // crouch hysteresis + standing cover + flicker count
        CoverSense c2;
        double t2 = 0;
        auto rd2 = [&](bool in, float head) {
            CoverReads rd;
            rd.inCover = in; rd.headAboveM = head; rd.x = 1; rd.y = 1;
            const CoverSenderOut o = c2.Update(rd, t2);
            t2 += 33.0;
            return o;
        };
        const bool stand = !rd2(true, 0.70f).sendCrouch;                // a standing cover (a wall): no crouch
        const bool low = rd2(true, 0.30f).sendCrouch;                   // head drops: crouch
        const bool hyst = rd2(true, 0.50f).sendCrouch;                  // between 0.40 and 0.55: stays crouched
        const bool up = !rd2(true, 0.60f).sendCrouch;                   // above 0.55: stands
        for (int i = 0; i < 8; ++i) rd2(false, -99.0f);                 // out of cover > 150 ms
        for (int i = 0; i < 3; ++i) rd2(true, 0.30f);                   // back within 600 ms: a flicker
        const CoverSenderStats w2 = c2.Take();
        Check("R3 T3 sender: crouch hint has hysteresis (0.40 / 0.55 m), a standing cover sends none, and a cover re-entered within 600 ms of leaving is counted as a flicker",
              stand && low && hyst && up && w2.rises == 2 && w2.falls == 1 && w2.flickers == 1, F("stand %d low %d hyst %d up %d rises %u falls %u flickers %u", (int)stand, (int)low, (int)hyst, (int)up, w2.rises, w2.falls, w2.flickers));
        // the sender line carries the id and the evidence, and is silent when no read of any kind happened
        const std::string line = CoverSenderLine(7, w);
        CoverSense quiet;
        CoverReads none;
        quiet.Update(none, 0);
        const std::string silent = CoverSenderLine(7, quiet.Take());
        Check("R3 T3 sender: the 10 s line names the player id and every read (in-cover / going-into / aiming-from true counts, sent, crouch, flickers, head, speed); silent with no cover read",
              line.find("id=7") != std::string::npos && line.find("IS_PED_IN_COVER true 140") != std::string::npos && line.find("IS_PED_GOING_INTO_COVER true 24") != std::string::npos &&
                  line.find("IS_PED_AIMING_FROM_COVER true 30") != std::string::npos && line.find("head above") != std::string::npos && silent.empty(),
              line.substr(0, 260));
        // sliding in cover is measured (the sender moves along the car), a z span and a heading swing too
        CoverSense sl;
        for (int i = 0; i < 60; ++i)
        {
            CoverReads rd;
            rd.inCover = true; rd.x = 10.0f; rd.y = (float)(i * 0.033 * 1.4); rd.z = 30.0f + (i > 30 ? 0.2f : 0.0f); rd.heading = 90.0f + i * 0.5f; rd.headAboveM = 0.3f;
            sl.Update(rd, i * 33.0);
        }
        const CoverSenderStats ws = sl.Take();
        Check("R3 T3 sender: the pose in cover is measured: sliding speed (1.4 m/s along the car), z span and heading swing",
              ws.speedMax > 1.2f && ws.speedMax < 1.6f && ws.slideSamples > 40 && ws.zSpanMax > 0.19f && ws.headingSwingMax > 25.0f,
              F("speed %.2f slide %u z %.2f swing %.0f", (double)ws.speedMax, ws.slideSamples, (double)ws.zSpanMax, (double)ws.headingSwingMax));
    }

    // ===================================================================== RECEIVER (CoverPuppet) vs the 0d code
    CoverBreaker brA;
    {   // S1: a cover point exactly where the sender is
        Rules r;
        const Scen s = RunIn();
        const Metrics nw = RunNew(s, r, &brA);
        Check("R3 T3 receiver S1 (a cover point exists at the pose): the puppet waits until it is within 0.30 m, ONE task, accepted by IS_PED_IN_COVER, held without churn, zero snaps and teleports from the driver, "
              "error in cover under 0.35 m, one clear when the sender leaves",
              nw.ped.taskCalls == 1 && nw.st.accepted == 1 && nw.st.fallbacks() == 0 && nw.ped.snaps[kDriver] == 0 && nw.ped.tele[kDriver] == 0 && nw.ped.snaps[kGame] == 0 &&
                  nw.errMaxInCover < 0.35 && nw.ped.clears == 1 && nw.inCoverMs > 3000.0 && nw.st.ended == 1,
              F("calls %u accepted %u fb %u driver snaps %u tele %u game snaps %u errInCover %.2f clears %u inCover %.0f ms", nw.ped.taskCalls, nw.st.accepted, nw.st.fallbacks(), nw.ped.snaps[kDriver],
                nw.ped.tele[kDriver], nw.ped.snaps[kGame], nw.errMaxInCover, nw.ped.clears, nw.inCoverMs));
        const Metrics od = RunOld(s, r);
        Check("R3 T3 receiver S1 on the OLD code: the pre-placement SET the lagging puppet onto the pose (a driver snap), then the task was accepted: the old window works when a cover point exists, the new one does it without the snap",
              od.ped.snaps[kDriver] >= 1 && od.ped.taskCalls == 1 && od.inCoverMs > 3000.0, F("old driver snaps %u calls %u inCover %.0f", od.ped.snaps[kDriver], od.ped.taskCalls, od.inCoverMs));
    }
    {   // S2: NO cover point (a car), a lagging puppet (the live log: pose 5.6 m/s, the ped's own speed 0.5)
        CoverBreaker br;
        Rules r;
        r.game = Game::NoPoint;
        r.locoSpeed = 2.5f;
        const Scen s = RunIn();
        const Metrics od = RunOld(s, r);
        const Metrics nw = RunNew(s, r, &br);
        Check("R3 T3 receiver S2 on the OLD code reproduces GI's car: puppet teleported onto the pose (driver teleports >= 1, its tasks cleared), the cover task runs and is never accepted, the puppet stands FROZEN "
              "for 700 ms with no cover, then upright on the pose while the sender crouches",
              od.ped.tele[kDriver] >= 1 && od.st.accepted == 0 && od.frozenNoCoverMs >= 650.0 && od.uprightMs > 3000.0 && od.ped.taskCalls == 2,
              F("old driver tele %u frozen %.0f ms upright %.0f ms calls %u", od.ped.tele[kDriver], od.frozenNoCoverMs, od.uprightMs, od.ped.taskCalls));
        Check("R3 T3 receiver S2 on the NEW code: never forced (no driver snap or teleport), at most 2 task calls in the hold, frozen with no cover for at most one 400 ms window per call, falls back to the crouch pose "
              "(upright for the wait and the 400 ms window only: under 1.5 s against 4.4 s on the old code), one clear per task",
              nw.ped.snaps[kDriver] == 0 && nw.ped.tele[kDriver] == 0 && nw.ped.taskCalls <= 2 && nw.frozenNoCoverMs <= 420.0 * nw.ped.taskCalls && nw.st.accepted == 0 &&
                  nw.st.fb[(int)CoverWhy::NotAccepted] == 1 && nw.uprightMs < 1500.0 && nw.uprightMs < od.uprightMs / 2.0 && nw.ped.clears == nw.ped.taskCalls && nw.ped.snaps[kGame] == 0,
              F("driver snaps %u tele %u calls %u frozen %.0f upright %.0f clears %u notAccepted %u", nw.ped.snaps[kDriver], nw.ped.tele[kDriver], nw.ped.taskCalls, nw.frozenNoCoverMs, nw.uprightMs,
                nw.ped.clears, nw.st.fb[(int)CoverWhy::NotAccepted]));
        // the same car, the puppet keeps up (fast pipeline): still a refusal, still no snap, and the breaker has now seen one refusal
        CoverBreaker br2;
        Rules rf = r;
        rf.locoSpeed = 6.0f;
        const Metrics nf = RunNew(s, rf, &br2);
        const Metrics of = RunOld(s, rf);
        Check("R3 T3 receiver S2 with a fast pipeline: the old code nudges (SET) the puppet and freezes it 700 ms; the new code issues one task in place and releases it after 400 ms (frozen <= 420 ms)",
              of.ped.snaps[kDriver] >= 1 && of.frozenNoCoverMs >= 650.0 && nf.ped.snaps[kDriver] == 0 && nf.ped.taskCalls == 1 && nf.frozenNoCoverMs <= 420.0 && nf.uprightMs < 1500.0,
              F("old snaps %u frozen %.0f; new snaps %u calls %u frozen %.0f upright %.0f", of.ped.snaps[kDriver], of.frozenNoCoverMs, nf.ped.snaps[kDriver], nf.ped.taskCalls, nf.frozenNoCoverMs, nf.uprightMs));
    }
    {   // S3: a cover point 1.2 m from the coordinates: the game moves the ped onto it
        Rules r;
        r.game = Game::PointOffset;
        const Scen s = RunIn();
        CoverBreaker br;
        const Metrics od = RunOld(s, r);
        const Metrics nw = RunNew(s, r, &br);
        Check("R3 T3 receiver S3 on the OLD code: the game snaps the puppet 1.2 m onto its cover point, the window accepts it, and the puppet stands 1.2 m from the sender for the whole cover (error in cover mean > 1.0 m)",
              od.ped.snaps[kGame] >= 1 && od.st.accepted == 0 && od.ErrMeanInCover() > 1.0 && od.errMaxInCover > 1.1, F("old game snaps %u errInCover mean %.2f max %.2f", od.ped.snaps[kGame], od.ErrMeanInCover(), od.errMaxInCover));
        Check("R3 T3 receiver S3 on the NEW code: the snap is DETECTED (moved 1.2 m > 0.45), the window refuses the position (left far from the pose), clears once and falls back: one game snap in the whole hold, mean error under 0.35 m, "
              "and the breaker opens at once (a snap counts double) so the next hold is not tried",
              nw.ped.snaps[kGame] == 1 && nw.st.snaps == 1 && nw.st.accepted == 0 && nw.st.fb[(int)CoverWhy::Snapped] == 1 && nw.ped.taskCalls == 1 && nw.ped.clears == 1 && nw.ErrMean() < 0.35 && !br.Allow(5000.0),
              F("game snaps %u counted %u accepted %u snapped %u calls %u clears %u errMean %.2f breaker allows %d", nw.ped.snaps[kGame], nw.st.snaps, nw.st.accepted, nw.st.fb[(int)CoverWhy::Snapped],
                nw.ped.taskCalls, nw.ped.clears, nw.ErrMean(), (int)br.Allow(5000.0)));
        // mutant: snap and distance checks off -> accepts the displaced puppet (the test bites)
        CoverCfg mut;
        mut.acceptDistM = 1e9f;
        mut.snapM = 1e9f;
        CoverBreaker bm;
        const Metrics mm = RunNew(s, r, &bm, mut);
        Check("R3 T3 mutant (snap / distance acceptance off): the displaced puppet is ACCEPTED and stands 1.2 m off, so the S3 predicate (no acceptance, error under 0.35 m) fails",
              mm.st.accepted >= 1 && mm.st.fb[(int)CoverWhy::Snapped] == 0 && mm.ErrMeanInCover() > 1.0 && !(mm.st.accepted == 0 && mm.ped.snaps[kGame] == 1), F("mutant accepted %u errInCover %.2f", mm.st.accepted, mm.ErrMeanInCover()));
    }
    {   // S4: the cover point is on the other side of the car (the puppet faces the wrong way)
        Rules r;
        r.game = Game::WrongSide;
        const Scen s = RunIn();
        CoverBreaker br;
        const Metrics od = RunOld(s, r);
        const Metrics nw = RunNew(s, r, &br);
        Check("R3 T3 receiver S4 (cover point on the wrong side, the puppet turned 180 deg): the old code accepts it, the new one refuses it (wrong side), clears once and falls back to the pose",
              od.st.accepted == 0 && od.inCoverMs > 2000.0 && nw.st.fb[(int)CoverWhy::WrongSide] == 1 && nw.inCoverMs < 100.0 && nw.ped.clears == 1,
              F("old inCover %.0f ms; new wrongSide %u inCover %.0f clears %u", od.inCoverMs, nw.st.fb[(int)CoverWhy::WrongSide], nw.inCoverMs, nw.ped.clears));
    }
    {   // S5: the puppet ignores tasks (the T2 hypothesis): the call works, nothing happens
        Rules r;
        r.game = Game::IgnoresTasks;
        const Scen s = RunIn();
        CoverBreaker br;
        const Metrics od = RunOld(s, r);
        const Metrics nw = RunNew(s, r, &br);
        Check("R3 T3 receiver S5 (the puppet ignores tasks): the old code suspends the controller for 700 ms and issues twice; the new one waits 400 ms, falls back ONCE, and then stays on the pose for the rest of the hold "
              "with no further task and no oscillation (3.4 s on the fallback)",
              od.ped.taskCalls == 2 && od.frozenNoCoverMs >= 650.0 && nw.ped.taskCalls == 1 && nw.ped.clears == 1 && nw.st.fb[(int)CoverWhy::NotAccepted] == 1 && nw.st.poseMs > 3000.0 && nw.frozenNoCoverMs <= 420.0 &&
                  nw.st.rearms == 0,
              F("old calls %u frozen %.0f; new calls %u clears %u notAccepted %u poseMs %.0f frozen %.0f rearms %u", od.ped.taskCalls, od.frozenNoCoverMs, nw.ped.taskCalls, nw.ped.clears,
                nw.st.fb[(int)CoverWhy::NotAccepted], nw.st.poseMs, nw.frozenNoCoverMs, nw.st.rearms));
    }
    {   // S6: the sender slides along the car
        Rules r;
        Scen s = RunIn();
        s.keys = { { 0, 0, 0 }, { 1.6, 8.8f, 0 }, { 2.1, 10, 0 }, { 3.0, 10, 0 }, { 5.0, 10, 3.0f }, { 7.0, 10, 3.0f }, { 8.0, 10, -2.0f } };
        s.flag = { { 1.6, 7.0 } };
        s.end = 8.5;
        CoverBreaker br;
        const Metrics od = RunOld(s, r);
        const Metrics nw = RunNew(s, r, &br);
        Check("R3 T3 receiver S6 (the sender slides 3 m along the car): the old window holds the puppet still until the 1.5 m / 600 ms rule fires and then SETS it (driver teleports >= 1); the new one releases it to the pose after 0.6 m / 300 ms "
              "(slid), the pipeline follows with no driver snap, and the task is re-issued once the sender has stopped (2 calls, accepted twice)",
              od.ped.tele[kDriver] >= 1 && nw.ped.snaps[kDriver] == 0 && nw.ped.tele[kDriver] == 0 && nw.st.fb[(int)CoverWhy::Slid] == 1 && nw.ped.taskCalls == 2 && nw.st.accepted == 2 && nw.st.rearms == 1,
              F("old tele %u; new driver snaps %u tele %u slid %u calls %u accepted %u rearms %u", od.ped.tele[kDriver], nw.ped.snaps[kDriver], nw.ped.tele[kDriver], nw.st.fb[(int)CoverWhy::Slid], nw.ped.taskCalls,
                nw.st.accepted, nw.st.rearms));
        // mutant: no dwell / no issue bound -> the window flaps (re-issues over and over while the sender shuffles back and forth)
        Scen z = s;
        z.keys = { { 0, 0, 0 }, { 1.6, 8.8f, 0 }, { 2.1, 10, 0 } };
        for (int i = 0; i < 12; ++i)
        {
            z.keys.push_back({ 2.4 + i * 0.5, 10, (i % 2) ? 0.0f : 1.0f });
            z.keys.push_back({ 2.6 + i * 0.5, 10, (i % 2) ? 0.0f : 1.0f });
        }
        z.flag = { { 1.6, 9.0 } };
        z.end = 9.5;
        CoverBreaker b1, b2;
        const Metrics shuffle = RunNew(z, r, &b1);
        CoverCfg mut;
        mut.maxIssues = 1000; mut.minPoseMs = 0; mut.reissueGapMs = 0;
        const Metrics flap = RunNew(z, r, &b2, mut);
        Check("R3 T3 receiver S6 shuffle (sender slides 1 m back and forth every 0.5 s): the new window issues at most twice per hold; with the issue bound and the dwell removed (mutant) it flaps with many more calls",
              shuffle.ped.taskCalls <= 2 && shuffle.ped.snaps[kDriver] == 0 && flap.ped.taskCalls > 4, F("new calls %u mutant calls %u", shuffle.ped.taskCalls, flap.ped.taskCalls));
    }
    {   // S7: slow accept: IS_PED_GOING_INTO_COVER at 50 ms, IS_PED_IN_COVER at 700 ms
        Rules r;
        r.game = Game::SlowAccept;
        const Scen s = RunIn();
        CoverBreaker br;
        const Metrics nw = RunNew(s, r, &br);
        CoverCfg mut;
        mut.goingExtraMs = 0.0;
        CoverBreaker bm;
        const Metrics mm = RunNew(s, r, &bm, mut);
        Check("R3 T3 receiver S7 (the game takes 700 ms to enter cover but reads IS_PED_GOING_INTO_COVER from 50 ms): the new window accepts it (going-into extends the wait), one task, no fallback; without that rule (mutant) "
              "it refuses a task that was working",
              nw.st.accepted == 1 && nw.ped.taskCalls == 1 && nw.st.fallbacks() == 0 && nw.st.goingSeen == 1 && mm.st.accepted == 0 && mm.st.fb[(int)CoverWhy::NotAccepted] >= 1,
              F("new accepted %u going %u fb %u; mutant accepted %u notAccepted %u", nw.st.accepted, nw.st.goingSeen, nw.st.fallbacks(), mm.st.accepted, mm.st.fb[(int)CoverWhy::NotAccepted]));
    }
    {   // S8: the sender aims from cover and stops, three times
        Rules r;
        Scen s = RunIn();
        s.aim = { { 3.0, 3.5 }, { 4.0, 4.5 }, { 5.0, 5.5 } };
        CoverBreaker br;
        const Metrics od = RunOld(s, r);
        const Metrics nw = RunNew(s, r, &br);
        Check("R3 T3 receiver S8 (aim, stop, aim, stop from cover): the cover task is dropped ONCE when the sender first aims (the aim task + crouch pose show it), and stays on the pose to the end of the hold: 1 call, 1 clear, no flap",
              nw.ped.taskCalls == 1 && nw.ped.clears == 1 && nw.st.fb[(int)CoverWhy::Aiming] == 1 && nw.st.rearms == 0 && nw.ped.snaps[kDriver] == 0 && od.ped.taskCalls == 1,
              F("new calls %u clears %u aiming %u rearms %u", nw.ped.taskCalls, nw.ped.clears, nw.st.fb[(int)CoverWhy::Aiming], nw.st.rearms));
    }
    {   // S9: the sender's flag flickers (100 ms) and drops (400 ms)
        Rules r;
        Scen s = RunIn();
        s.gap = { { 3.0, 3.1 } };
        CoverBreaker br;
        const Metrics od = RunOld(s, r);
        const Metrics nw = RunNew(s, r, &br);
        Check("R3 T3 receiver S9 (a 100 ms gap in the sender's flag while held): the old window ends and re-issues (2 calls, 2 clears), the new one bridges it (grace 150 ms): 1 call, 1 clear at the real end, bridged counted",
              od.ped.taskCalls == 2 && od.ped.clears >= 2 && nw.ped.taskCalls == 1 && nw.ped.clears == 1 && nw.st.bridged == 1, F("old calls %u clears %u; new calls %u clears %u bridged %u", od.ped.taskCalls, od.ped.clears,
                nw.ped.taskCalls, nw.ped.clears, nw.st.bridged));
        CoverCfg mut;
        mut.endGraceMs = 0.0;
        CoverBreaker bm;
        const Metrics mm = RunNew(s, r, &bm, mut);
        Check("R3 T3 mutant (no end grace): the 100 ms gap ends the hold and the task is issued again (2 calls), so the S9 predicate fails", mm.ped.taskCalls == 2 && mm.st.bridged == 0, F("mutant calls %u bridged %u", mm.ped.taskCalls, mm.st.bridged));
        Scen g2 = RunIn();
        g2.gap = { { 4.5, 4.9 } };
        CoverBreaker b3;
        const Metrics real = RunNew(g2, r, &b3);
        Check("R3 T3 receiver S9b (a real 400 ms drop): the hold ends (one clear), the flag returns and the puppet, still on the cover point, is put in again without a snap (2 calls, accepted twice)",
              real.ped.taskCalls == 2 && real.st.accepted == 2 && real.ped.clears == 2 && real.ped.snaps[kDriver] == 0 && real.st.bridged == 0,
              F("calls %u accepted %u clears %u driver snaps %u", real.ped.taskCalls, real.st.accepted, real.ped.clears, real.ped.snaps[kDriver]));
    }

    // ===================================================================== TOLERANCE, BREAKER, MODES, UNCALLABLE NATIVE
    {   // tolerance: a puppet 3 m behind is NEVER placed; the window waits and then gives up to the pose (no teleport)
        Rules r;
        r.lagM = 2.5f;
        r.locoSpeed = 0.5f;                       // the pipeline cannot catch up within the wait
        Scen s = RunIn();
        s.keys = { { 0, 0, 0 }, { 0.5, 3, 0 }, { 6.0, 3, 0 } };
        s.flag = { { 0.6, 3.0 } };
        s.end = 3.5;
        CoverBreaker br;
        const Metrics od = RunOld(s, r);
        const Metrics nw = RunNew(s, r, &br);
        Check("R3 T3 receiver tolerance: a puppet 5 m from the pose is teleported by the OLD pre-placement (driver teleport) and NEVER touched by the new window: it waits 900 ms, then the pose fallback (never settled), no task call, no driver move",
              od.ped.tele[kDriver] >= 1 && nw.ped.tele[kDriver] == 0 && nw.ped.snaps[kDriver] == 0 && nw.ped.taskCalls == 0 && nw.st.fb[(int)CoverWhy::NoTolerance] == 1 && nw.st.issued == 0,
              F("old tele %u; new tele %u calls %u noTol %u", od.ped.tele[kDriver], nw.ped.tele[kDriver], nw.ped.taskCalls, nw.st.fb[(int)CoverWhy::NoTolerance]));
        CoverCfg mut;
        mut.issueTolM = 1e9f;
        CoverBreaker bm;
        const Metrics mm = RunNew(s, r, &bm, mut);
        Check("R3 T3 mutant (tolerance gate off): the window pre-places the 3 m puppet itself (driver teleport), so the tolerance predicate fails", mm.ped.tele[kDriver] >= 1 && mm.ped.taskCalls >= 1, F("mutant tele %u calls %u", mm.ped.tele[kDriver], mm.ped.taskCalls));
    }
    {   // breaker
        CoverBreaker br;                                  // 2 refusals open it for 30 s
        Rules r;
        r.game = Game::NoPoint;
        const Scen s = RunIn();
        const Metrics a = RunNew(s, r, &br);
        const bool afterOne = br.Allow(7000.0);
        Scen later = s;                                   // the same scenario shifted: a second hold 8 s later in the same machine would need a driver run, so use the breaker directly
        (void)later;
        const Metrics b = RunNew(s, r, &br);              // a fresh puppet, the shared breaker
        // each run restarts the clock at 0: the breaker was opened at ~2.5 s of the second run
        Check("R3 T3 breaker: two consecutive refused holds open the shared breaker (the second hold's task is the last one); the puppet itself still falls back to the pose each time",
              a.st.fb[(int)CoverWhy::NotAccepted] == 1 && afterOne && b.st.fb[(int)CoverWhy::NotAccepted] == 1 && br.Opens() == 1,
              F("first refusals %u allowed-after-one %d second refusals %u opens %u", a.st.fb[(int)CoverWhy::NotAccepted], (int)afterOne, b.st.fb[(int)CoverWhy::NotAccepted], br.Opens()));
        CoverBreaker bb;
        bb.Report(false, 1000.0);
        const bool one = bb.Allow(1001.0);
        bb.Report(false, 2000.0);
        const bool open = !bb.Allow(2001.0) && !bb.Allow(31999.0);
        const bool probe = bb.Allow(32001.0);
        bb.Report(false, 32500.0);                        // the probe is refused: re-opened at once, twice as long
        const bool reopened = !bb.Allow(33000.0) && !bb.Allow(92000.0) && bb.Allow(92600.0);
        bb.Report(true, 93000.0);
        bb.Report(false, 93100.0);
        const bool closed = bb.Allow(93200.0);            // one success reset the count
        Check("R3 T3 breaker: closed after one refusal, open for 30 s after two, a probe after the pause, a refused probe re-opens it for twice as long, one success closes it",
              one && open && probe && reopened && closed, F("%d %d %d %d %d", (int)one, (int)open, (int)probe, (int)reopened, (int)closed));
        // an open breaker sends a new hold straight to the pose: no task at all
        CoverBreaker ob;
        ob.Report(false, 0);
        ob.Report(false, 0);
        Rules r2;
        const Metrics o = RunNew(RunIn(), r2, &ob);
        Check("R3 T3 breaker: while it is open a rising flag goes straight to the crouch pose (reason: tasks refused lately), zero task calls, zero clears, the pipeline keeps the puppet on the pose",
              o.ped.taskCalls == 0 && o.ped.clears == 0 && o.st.fb[(int)CoverWhy::BreakerOpen] == 1 && o.st.issued == 0 && o.frozenNoCoverMs == 0.0, F("calls %u clears %u breaker fb %u", o.ped.taskCalls, o.ped.clears, o.st.fb[(int)CoverWhy::BreakerOpen]));
    }
    {   // modes
        Rules r;
        const Scen s = RunIn();
        CoverBreaker b1, b2;
        const Metrics pose = RunNew(s, r, &b1, CoverCfg(), CoverSyncMode::Pose);
        const Metrics off = RunNew(s, r, &b2, CoverCfg(), CoverSyncMode::Off);
        Check("R3 T3 registry cover_sync=pose: never a cover task, never a clear, never a suspend, the fallback phase for the whole hold (crouch hint shown, no upright frames)",
              pose.ped.taskCalls == 0 && pose.ped.clears == 0 && pose.frozenNoCoverMs == 0.0 && pose.st.fb[(int)CoverWhy::ModePose] == 1 && pose.st.poseMs > 3500.0 && pose.uprightMs < 100.0,
              F("calls %u clears %u poseMs %.0f upright %.0f", pose.ped.taskCalls, pose.ped.clears, pose.st.poseMs, pose.uprightMs));
        Check("R3 T3 registry cover_sync=off: the flag is ignored (counted), no task, no clear, no suspend, the window never leaves Idle",
              off.ped.taskCalls == 0 && off.ped.clears == 0 && off.frozenNoCoverMs == 0.0 && off.st.ignoredHolds == 1 && off.st.fallbacks() == 0 && off.st.poseMs == 0.0,
              F("calls %u ignored %u", off.ped.taskCalls, off.st.ignoredHolds));
        Check("R3 T3 registry parse: pose / off are honoured, anything else (unset, 'task', garbage) is the default task mode with the safe fallback; names round-trip",
              ParseCoverSync(std::wstring(L"pose")) == CoverSyncMode::Pose && ParseCoverSync(std::wstring(L"off")) == CoverSyncMode::Off && ParseCoverSync(std::wstring(L"task")) == CoverSyncMode::Task &&
                  ParseCoverSync(std::wstring(L"")) == CoverSyncMode::Task && ParseCoverSync(std::wstring(L"OFF!")) == CoverSyncMode::Task && std::string(CoverSyncName(CoverSyncMode::Pose)) == "pose",
              "");
    }
    {   // the native cannot be called (unresolved / hash 0): straight to the pose, nothing to clear, breaker counts it
        Rules r;
        r.callable = false;
        CoverBreaker br;
        const Metrics nw = RunNew(RunIn(), r, &br);
        Check("R3 T3 uncallable native: TASK_PUT_PED_DIRECTLY_INTO_COVER not callable -> the pose fallback at once (reason native not callable), no clear, no frozen frame, issued count corrected to 0, the pipeline keeps the puppet on the pose",
              nw.st.fb[(int)CoverWhy::CannotCall] == 1 && nw.ped.clears == 0 && nw.frozenNoCoverMs < 20.0 && nw.st.issued == 0 && nw.ped.snaps[kDriver] == 0 && nw.uprightMs < 1500.0,
              F("cannotCall %u clears %u frozen %.0f issued %u", nw.st.fb[(int)CoverWhy::CannotCall], nw.ped.clears, nw.frozenNoCoverMs, nw.st.issued));
    }

    // ===================================================================== DIAGNOSTICS
    {
        Rules r;
        r.game = Game::NoPoint;
        const Scen s = RunIn();
        CoverBreaker br;
        Ped ped;
        NewDriver d(ped, r, &br);
        const Metrics m = Run(d, ped, r, s);
        (void)m;
        const CoverStats w = d.cp.TakeWindow();
        const std::string line = CoverPuppetLine(2, "P2", d.cp.Mode(), d.cp.CurrentPhase(), d.cp.LastWhy(), w, d.cp.Stats());
        Check("R3 T3 diagnostics: the per-puppet line carries the player id, the mode, flag frames, issued, accepted, fallbacks by reason, snaps, teleports, time in cover / on the pose and the cover error; "
              "its numbers equal the machine's counters",
              line.find("id=2 (P2)") != std::string::npos && line.find("cover_sync=task") != std::string::npos && line.find("issued 1") != std::string::npos && line.find("task not accepted 1") != std::string::npos &&
                  line.find("accepted (IS_PED_IN_COVER) 0") != std::string::npos && line.find("snaps) 0") != std::string::npos && line.find("fell back to the crouch pose") != std::string::npos &&
                  w.issued == 1 && w.fallbacks() == 1 && w.flagFrames > 200 && w.poseMs > 3000.0,
              line.substr(0, 300));
        const CoverStats w2 = d.cp.TakeWindow();
        const std::string quiet = CoverPuppetLine(2, "P2", d.cp.Mode(), d.cp.CurrentPhase(), d.cp.LastWhy(), w2, d.cp.Stats());
        Check("R3 T3 diagnostics: the window is taken (a second take is empty and the line is silent), the session totals are kept", quiet.empty() && w2.issued == 0 && d.cp.Stats().issued == 1);
        // a held, accepted hold: acceptance time and cover error are reported
        Rules r1;
        CoverBreaker b1;
        Ped p1;
        NewDriver d1(p1, r1, &b1);
        Run(d1, p1, r1, RunIn());
        const CoverStats w1 = d1.cp.TakeWindow();
        const std::string l1 = CoverPuppetLine(3, "GI", d1.cp.Mode(), d1.cp.CurrentPhase(), d1.cp.LastWhy(), w1, d1.cp.Stats());
        Check("R3 T3 diagnostics: an accepted hold reports IS_PED_GOING_INTO_COVER seen, the accept delay (about 30 ms here), the time in cover and the error in cover",
              w1.accepted == 1 && w1.goingSeen <= 1 && w1.acceptMsMax > 20.0 && w1.acceptMsMax < 80.0 && w1.heldMs > 3000.0 && w1.errFrames > 100 && w1.errMax < 0.35f && l1.find("id=3 (GI)") != std::string::npos &&
                  l1.find("accepted (IS_PED_IN_COVER) 1") != std::string::npos && l1.find("now: idle") != std::string::npos,
              l1.substr(0, 320));
    }

    // ===================================================================== MACHINE UNITS
    {
        // the exact sequence: rise, wait, issue within tolerance, pending, accept, held, flag falls -> grace -> ended once
        CoverPuppet cp;
        CoverBreaker br;
        CoverIn in;
        in.flag = true; in.pedReadable = true; in.distM = 0.2f; in.poseSpeed = 0.1f;
        in.nowMs = 1000;
        const CoverOut a = cp.Update(in, &br);
        in.nowMs = 1016; in.pedInCover = false;
        const CoverOut b = cp.Update(in, &br);
        in.nowMs = 1050; in.pedInCover = true; in.pedGoingInto = true; in.movedM = 0.05f; in.distM = 0.1f;
        const CoverOut c = cp.Update(in, &br);
        const CoverPhase held = cp.CurrentPhase();
        in.nowMs = 2000;
        const CoverOut d2 = cp.Update(in, &br);
        in.flag = false; in.nowMs = 2016;
        const CoverOut e1 = cp.Update(in, &br);
        in.nowMs = 2200;
        const CoverOut e2 = cp.Update(in, &br);
        in.nowMs = 2216;
        const CoverOut e3 = cp.Update(in, &br);
        Check("R3 T3 machine: rise within tolerance issues at once with a nudge (0.2 m > 0.04), Pending suspends, IS_PED_IN_COVER accepts, Held suspends without issuing, the flag falling is held 150 ms and then ENDS once with one clear",
              a.issue && a.nudge && a.suspend && !b.issue && b.suspend && c.accepted && held == CoverPhase::Held && d2.suspend && !d2.issue && e1.suspend && !e1.clear && e2.ended && e2.clear && !e2.suspend && !e3.clear &&
                  cp.CurrentPhase() == CoverPhase::Idle,
              F("a %d%d%d b %d%d c %d held %d d2 %d%d e1 %d%d e2 %d%d%d e3 %d", a.issue, a.nudge, a.suspend, b.issue, b.suspend, c.accepted, (int)held, d2.suspend, d2.issue, e1.suspend, e1.clear, e2.ended, e2.clear,
                e2.suspend, e3.clear));
        // lost cover while the sender is in it: STAY once, then (only after the 1.5 s re-issue gap) ONE re-issue that is verified like the first, then the pose
        auto lostRun = [&](double dropAtMs, bool& stayed, bool& reissued, CoverWhy& why, uint32_t& issued) {
            CoverPuppet lp;
            CoverIn li;
            li.flag = true; li.pedReadable = true; li.distM = 0.05f; li.poseSpeed = 0;
            li.nowMs = 100;
            lp.Update(li, nullptr);
            stayed = reissued = false;
            why = CoverWhy::None;
            int stays = 0;
            for (double t = 130; t < 12000 && why == CoverWhy::None; t += 16.0)
            {
                li.nowMs = t;
                li.pedInCover = t < dropAtMs && t >= 130;
                const CoverOut o = lp.Update(li, nullptr);
                if (o.stay) { stayed = true; ++stays; }
                if (o.issue && t > 200.0) reissued = true;
                if (o.why != CoverWhy::None) why = o.why;
            }
            issued = lp.Stats().issued;
            return stays;
        };
        bool st1, ri1, st2, ri2;
        CoverWhy lw1, lw2;
        uint32_t is1, is2;
        const int stays1 = lostRun(800.0, st1, ri1, lw1, is1);        // dropped early: no re-issue yet (gap 1.5 s), STAY once, then the pose (lost cover)
        const int stays2 = lostRun(2500.0, st2, ri2, lw2, is2);       // dropped late: STAY once, one re-issue, which the game does not accept -> the pose (not accepted)
        Check("R3 T3 machine: a puppet that leaves cover while the sender is in it gets TASK_STAY_IN_COVER once; early -> the pose (lost cover), late -> at most one more issue, verified like the first, then the pose: bounded at 2 issues",
              st1 && stays1 == 1 && !ri1 && lw1 == CoverWhy::Lost && is1 == 1 && st2 && stays2 == 1 && ri2 && lw2 == CoverWhy::NotAccepted && is2 == 2,
              F("early: stay %d/%d reissue %d why %s issued %u; late: stay %d/%d reissue %d why %s issued %u", (int)st1, stays1, (int)ri1, CoverWhyName(lw1), is1, (int)st2, stays2, (int)ri2, CoverWhyName(lw2), is2));
        // no pre-placement beyond 0.30 m: 0.31 m waits, 0.29 m issues; a sprinting pose waits
        CoverPuppet w1, w2, w3;
        CoverIn x;
        x.flag = true; x.pedReadable = true; x.nowMs = 10;
        x.distM = 0.31f; const CoverOut o1 = w1.Update(x, nullptr);
        x.distM = 0.29f; const CoverOut o2 = w2.Update(x, nullptr);
        x.distM = 0.02f; x.poseSpeed = 5.0f; const CoverOut o3 = w3.Update(x, nullptr);
        x.poseSpeed = 0.0f; x.distM = 0.02f;
        CoverPuppet w4;
        const CoverOut o4 = w4.Update(x, nullptr);
        Check("R3 T3 machine: the tolerance is exactly 0.30 m (0.31 waits, 0.29 issues), a pose still running at 5 m/s waits, a puppet already on the pose issues with no nudge",
              !o1.issue && o1.nudge == false && w1.CurrentPhase() == CoverPhase::Wait && o2.issue && o2.nudge && !o3.issue && w3.CurrentPhase() == CoverPhase::Wait && o4.issue && !o4.nudge,
              F("%d %d %d %d", o1.issue, o2.issue, o3.issue, o4.issue));
        // the wait ends in the pose after waitMs when the puppet cannot be read
        CoverPuppet un;
        CoverIn ui;
        ui.flag = true; ui.pedReadable = false; ui.nowMs = 0;
        un.Update(ui, nullptr);
        ui.nowMs = 950;
        const CoverOut uo = un.Update(ui, nullptr);
        Check("R3 T3 machine: an unreadable puppet never gets a task; after the wait the window falls back to the pose (reason: ped unreadable), no clear", uo.why == CoverWhy::Unreadable && !uo.clear && !uo.issue && un.Stats().issued == 0, "");
    }
}

}  // namespace r3cover
