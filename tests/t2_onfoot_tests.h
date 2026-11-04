// T2 (desync overhaul round 2): remote-ped locomotion against MODELS OF THE GAME THAT ARE UNKNOWN.
//
// Included by tests/locomotion_test.cpp (after TestSyncA, before main; it uses that file's Check / F / V / Rng / WrapDeg / kPi /
// SlideScenarios) and called once from main as T2OnFootTests(). Nothing here talks to a game: the ped is a model, and the point of
// the file is that there are MANY models. Every number in docs/sync-specs/onfoot.md "T2" comes from the tables this prints
// (T2_TABLE=1 prints the full matrix; the default run prints one summary line per design).
//
// The game's behaviour that nobody has seen live (T0 sec 9), as independent axes of the simulated ped:
//   follow  Honour    TASK_GO_TO_ENTITY follows a frozen invisible anchor that is moved every frame
//           Sluggish  it follows at 60% of the commanded stride (slows near the target, detours around nothing)
//           Dead      the anchor task never moves the ped (it stands, reads Still)
//           NoneMove  no task moves the ped at all (straight or anchor)
//   vel     Replace   SET_ENTITY_VELOCITY replaces the animation's displacement for the frame
//           Add       it adds to it
//           Ignore    it has no effect on a ped under a task
//   rate    SET_PED_MOVE_RATE_OVERRIDE scales the stride, or is ignored
//   turn    Arc       the ped keeps its speed while it turns toward the target (RealPed's old behaviour)
//           Pivot     it turns on the spot when the target is more than 90 degrees off, and slows in between (GTA's turn clips)
//   nominal the true stride speeds against the controller's 1.5 / 4.5 / 7 guess (x1, x0.85, x1.15)
// A design = a LocoConfig (+ whether the engine can provide the anchor entity). A design is judged on ALL models: the worst model
// is what decides, because which model is true is exactly what is not known.

#include <map>
#include "sync/oneshot.h"

namespace t2 {

enum class Follow { Honour, Sluggish, Dead, NoneMove };
enum class VelSem { Replace, Add, Ignore };

struct Model {
    Follow follow = Follow::Honour;
    VelSem vel = VelSem::Replace;
    bool rateScales = true;
    bool pivot = false;
    bool persistRead = false;   // GET_ENTITY_VELOCITY returns the velocity we set (Replace only)
    double nominal = 1.0;
    // R3 T2: causes of "the task runs and the ped shows no gait, own speed 0.00" (tests/r3_t2_tests.h)
    double writeResetMs = 0;    // every coordinate write (nudge or snap) cancels the walk for this long: no motion, no gait read
    double taskLatencyMs = 0;   // a task (re)issue needs this long before the ped moves / reads a gait; a re-issue restarts the wait
    int fixAtRepair = -1;       // >= 0: the follow behaviour is Honour once the engine has done this many LocoRepair steps (a flag was the cause)
    bool gaitBlind = false;     // the IS_PED_WALKING / RUNNING / SPRINTING natives read Still whatever the ped does (wrong hashes): it moves fine
    std::string Name() const {
        static const char* fn[] = {"honour", "sluggish", "dead", "nonemove"};
        static const char* vn[] = {"replace", "add", "ignore"};
        return F("follow=%s vel=%s rate=%s turn=%s nom=%.2f%s", fn[int(follow)], vn[int(vel)], rateScales ? "scales" : "ignored",
                 pivot ? "pivot" : "arc", nominal, persistRead ? " persist" : "");
    }
};

struct Design {
    const char* name;
    LocoConfig cfg;
};

struct Ped {
    LocoVec pos = V(0, 0, 30);
    float facing = 0;
    double speed = 0;
    int task = 0;                 // 0 none / stand-still, 1 go-straight coordinate, 2 go-to-entity
    LocoVec target, anchor;
    float blend = 0, rate = 1;
    bool lost = false;
    double nomWalk = 1.5, nomRun = 4.5, nomSprint = 7.0;
    double accelTauMs = 250, decelTauMs = 180, ripple = 0.10, rippleHz = 1.9, turnDps = 300;
    Model m;
    LocoVec lastSet; bool hasLastSet = false;
    LocoVec animVel;
    double t = 0;
    double holdUntilT = -1, issuedT = -1e9;   // R3: write-reset hold, time of the last task issue

    double AnimSpeed(double b) const {
        if (b <= 0) return 0;
        if (b <= 1) return nomWalk * b;
        if (b <= 2) return nomWalk + (nomRun - nomWalk) * (b - 1);
        return nomRun + (nomSprint - nomRun) * (std::min(b, 3.0) - 2);
    }
    Gait GameGait() const {
        if (m.gaitBlind) return Gait::Still;
        const double s = std::hypot(animVel.x, animVel.y);
        return s < 0.5 ? Gait::Still : s < 2.65 ? Gait::Walk : s < 5.75 ? Gait::Run : Gait::Sprint;
    }
    LocoVec Readback() const { return m.persistRead && hasLastSet ? lastSet : animVel; }
    void Anim(double dtMs) {
        double vd = 0;
        const bool held = t < holdUntilT || (m.taskLatencyMs > 0 && task != 0 && (t - issuedT) * 1000.0 < m.taskLatencyMs);
        const double arriveR = task == 2 ? 0.1 : 0.25;
        const LocoVec& tg = task == 2 ? anchor : target;
        const double dx = double(tg.x) - pos.x, dy = double(tg.y) - pos.y, d = std::hypot(dx, dy);
        if (task != 0 && !lost) {
            if (d < arriveR) { if (task == 1) lost = true; }
            else {
                vd = AnimSpeed(blend) * (m.rateScales ? rate : 1.0);
                if (task == 2 && m.follow == Follow::Dead) vd = 0;
                if (task == 2 && m.follow == Follow::Sluggish) vd *= 0.6;
                if (m.follow == Follow::NoneMove) vd = 0;
                if (held) vd = 0;
            }
            const double want = std::atan2(-dx, dy) * 180.0 / kPi;
            const double diff = WrapDeg(want - facing);
            const double maxTurn = turnDps * dtMs / 1000.0;
            facing = float(WrapDeg(facing + std::max(-maxTurn, std::min(maxTurn, diff))));
            if (m.pivot && vd > 0) {
                const double err = std::fabs(WrapDeg(want - facing));
                vd *= err >= 90.0 ? 0.0 : std::cos(err * kPi / 180.0);
            }
        }
        const double k = 1.0 - std::exp(-dtMs / (vd > speed ? accelTauMs : decelTauMs));
        speed += (vd - speed) * k;
        if (held) speed = 0;
        if (speed < 0.02) speed = 0;
        t += dtMs / 1000.0;
        const double rip = 1.0 + ripple * std::min(1.0, speed / 2.0) * std::sin(2 * kPi * rippleHz * t);
        const double a = double(facing) * kPi / 180.0;
        animVel = V(-std::sin(a) * speed * rip, std::cos(a) * speed * rip, 0);
    }
};

struct Result {
    LocoStats st;
    LocoDiagSummary d;
    double p95Err = 0, maxErr = 0, tasksPer10s = 0, speedRatio = 0;
    uint32_t snaps = 0;
    LocoLevel finalLevel = LocoLevel::Full;
    double firstLevelChangeMs = -1, lastLevelChangeMs = -1;
    std::vector<std::pair<LocoLevel, LocoLevelWhy>> changes;
    std::vector<double> changeAtMs;       // R3: when each level change happened
    std::vector<std::pair<double, LocoRepair>> repairs;   // R3: LocoRepair requests (time, step)
    struct Issue { double atMs; LocoWhy why; };
    std::vector<Issue> issues;            // R3: every gait task issue
    double firstGaitMs = -1;              // R3: first time (after 500 ms) the ped read a gait while the pose moved > 1 m/s
    double steadyGaitFrac = 0;            // R3: share of the frames after 12 s (pose > 1 m/s) the ped read a gait
    uint32_t verdictChanges = 0; WriteVerdict verdict = WriteVerdict::Unknown;
    uint32_t capHits = 0;
};

struct RunOpts {
    double loss = 0.02, jitterMs = 10.0, delayMs = 130.0, fps = 60.0;
    uint64_t seed = 7;
    bool anchorEntity = true;     // the engine can create the anchor (false: noAnchor every frame)
    int repairsDone0 = 0;         // R3: LocoRepair steps the engine is taken to have done already (LocoConfig::maxRepairs = none will be asked)
    double warmupMs = 2000;       // errors before this are not counted (the first snap, the start)
    double traceFromMs = -1, traceToMs = -1;
};

// The engine's DrivePeer glue, as far as the controller's outputs go: this is the code that has to be a faithful mirror of
// apps/engine/engine.cpp for the numbers to mean anything (the same order: anchor, snap, tasks, rate, nudge/velocity).
static Result Run(const SlideScenario& sc, const Model& m, const LocoConfig& cfg, const RunOpts& op) {
    Rng rng(op.seed);
    LocomotionController lc(cfg);
    LocoDiag diag;
    Ped ped;
    ped.m = m;
    ped.nomWalk = 1.5 * m.nominal; ped.nomRun = 4.5 * m.nominal; ped.nomSprint = 7.0 * m.nominal;
    struct Smp { double t, arr; LocoVec pos, vel; float heading; bool tele; };
    std::vector<Smp> smp;
    double tx = 0, ty = 0, yOff = 0, nextSmp = 0, tele = 0, tSub = 0;
    Result res;
    std::vector<double> errs;
    double speedRatioSum = 0; int speedRatioN = 0;
    bool teleDone = false, first = true;
    int repairs = op.repairsDone0;
    double gaitFramesLate = 0, framesLate = 0;
    const double dtBase = 1000.0 / op.fps;
    double now = 0, lastNow = 0;
    LocoLevel lastLevel = LocoLevel::Full;
    while (now < sc.durMs) {
        const double dt = first ? dtBase : now - lastNow;
        while (tSub < now) {
            const double a = sc.dirDeg(tSub / 1000.0) * kPi / 180.0, sp = sc.speed(tSub / 1000.0);
            tx += std::cos(a) * sp * 0.001; ty += std::sin(a) * sp * 0.001;
            if (!teleDone && sc.teleportAtMs >= 0 && tSub >= sc.teleportAtMs) { yOff += 30.0; teleDone = true; tele = 1; }
            tSub += 1.0;
            if (tSub >= nextSmp) {
                Smp s;
                s.t = tSub;
                s.arr = tSub + rng.Uniform() * op.jitterMs + 5.0;
                const double rip = 1.0 + 0.10 * std::min(1.0, sp / 2.0) * std::sin(2 * kPi * 1.9 * tSub / 1000.0 + 1.0);
                s.pos = V(tx, ty + yOff, 30.0);
                s.vel = V(std::cos(a) * sp * rip, std::sin(a) * sp * rip, 0);
                s.heading = float(WrapDeg(std::atan2(-std::cos(a), std::sin(a)) * 180.0 / kPi));
                s.tele = tele > 0; tele = 0;
                if (rng.Uniform() >= op.loss || s.tele) smp.push_back(s);
                nextSmp += 1000.0 / 30.0 + (rng.Uniform() - 0.5) * 6.0;
            }
        }
        const double renderT = now - op.delayMs;
        LocoVec ppos, pvel; float phead = 0; bool ptele = false;
        {
            int a = -1;
            for (int i = int(smp.size()) - 1; i >= 0; --i) if (smp[i].arr <= now) { a = i; break; }
            if (a < 0) { now += dtBase; lastNow = now - dtBase; first = false; continue; }
            int lo = -1;
            for (int i = a; i >= 0; --i) if (smp[i].t <= renderT) { lo = i; break; }
            if (lo < 0) lo = 0;
            const Smp& A = smp[lo];
            if (lo + 1 <= a && smp[lo + 1].t > A.t && !smp[lo + 1].tele) {
                const Smp& B = smp[lo + 1];
                const double f = std::max(0.0, std::min(1.0, (renderT - A.t) / (B.t - A.t)));
                ppos = V(A.pos.x + (B.pos.x - A.pos.x) * f, A.pos.y + (B.pos.y - A.pos.y) * f, A.pos.z);
                pvel = V(A.vel.x + (B.vel.x - A.vel.x) * f, A.vel.y + (B.vel.y - A.vel.y) * f, 0);
                phead = float(WrapDeg(A.heading + WrapDeg(B.heading - A.heading) * f));
            } else {
                const double ex = std::min(250.0, std::max(0.0, renderT - A.t)) / 1000.0;
                ppos = V(A.pos.x + A.vel.x * ex, A.pos.y + A.vel.y * ex, A.pos.z);
                pvel = A.vel; phead = A.heading;
            }
            ptele = A.tele && (renderT - A.t) < dtBase * 1.5 && renderT >= A.t;
        }
        const double poseSp = std::hypot(pvel.x, pvel.y);
        LocoInput in;
        in.pos = ppos; in.vel = pvel; in.heading = phead; in.teleported = ptele;
        ped.pos.z = 30.0f;
        in.pedPos = ped.pos; in.pedHeading = ped.facing; in.pedGait = ped.GameGait();
        in.dtMs = dt;
        in.moveState = int8_t(poseSp < 0.5 ? 0 : poseSp < 3.1 ? 1 : poseSp < 6.1 ? 2 : 3);
        const LocoVec rb = ped.Readback();
        in.pedVel = rb; in.havePedVel = true;
        in.noAnchor = !op.anchorEntity;
        in.repairsDone = repairs;
        const LocoOutput o = lc.Update(in);
        if (o.verdictChanged) ++res.verdictChanges;
        if (o.capHit) ++res.capHits;
        if (o.repair != LocoRepair::None) {
            res.repairs.push_back({now, o.repair});
            ++repairs;
            if (m.fixAtRepair >= 0 && repairs >= m.fixAtRepair) ped.m.follow = Follow::Honour;
            if (o.repair == LocoRepair::ClearTasks) { ped.task = 0; lc.Reset(); }
            else { ped.task = 0; ped.speed = 0; ped.lost = false; lc.ResetForNewPed(); }
        }
        if (o.levelChanged) {
            res.changes.push_back({o.level, o.levelWhy});
            res.changeAtMs.push_back(now);
            if (res.firstLevelChangeMs < 0) res.firstLevelChangeMs = now;
            res.lastLevelChangeMs = now;
        }
        lastLevel = o.level;
        // ---- apply as DrivePeer does ----
        if (o.snap) { ped.pos = o.snapPos; ped.facing = o.heading; ped.speed = 0; ped.lost = false; }
        if (o.nudge && !o.snap) { ped.pos.x += o.nudgeDelta.x; ped.pos.y += o.nudgeDelta.y; }
        if ((o.snap || o.nudge) && m.writeResetMs > 0) ped.holdUntilT = ped.t + m.writeResetMs / 1000.0;
        if (o.task == LocoTask::GoStraight || o.task == LocoTask::GoToAnchor) { ped.issuedT = ped.t; res.issues.push_back({now, o.why}); }
        if (o.setHeading && !o.snap && o.gait == Gait::Still && ped.task == 0) ped.facing = o.heading;
        if (o.moveAnchor) ped.anchor = o.anchorPos;
        if (o.task == LocoTask::GoStraight) { ped.task = 1; ped.target = o.target; ped.blend = o.moveBlendRatio; ped.lost = false; }
        else if (o.task == LocoTask::GoToAnchor) { ped.task = 2; ped.blend = o.moveBlendRatio; ped.lost = false; }
        else if (o.task == LocoTask::StandStill) { ped.task = 0; }
        if (o.setBlend) ped.blend = o.moveBlendRatio;
        ped.rate = o.setMoveRate ? o.moveRate : 1.0f;
        ped.Anim(dt);
        if (poseSp > 1.0) {
            if (res.firstGaitMs < 0 && now >= 500 && ped.GameGait() != Gait::Still) res.firstGaitMs = now;
            if (now >= 12000) { ++framesLate; if (ped.GameGait() != Gait::Still) ++gaitFramesLate; }
        }
        LocoVec v = ped.animVel;
        if (o.setVelocity) {
            if (m.vel == VelSem::Add) { v.x += o.velocity.x; v.y += o.velocity.y; }
            else if (m.vel == VelSem::Replace) { v.x = o.velocity.x; v.y = o.velocity.y; }
            ped.lastSet = o.velocity; ped.hasLastSet = true;
        } else ped.hasLastSet = false;
        LocoDiagFrame df;
        df.dtMs = dt; df.nowMs = now; df.pose = ppos; df.pedPos = in.pedPos; df.poseSpeed = float(poseSp); df.gait = o.gait;
        df.pushed = o.setVelocity; df.push = o.velocity;
        // an ignored velocity moved nothing: it is no skate
        if (m.vel == VelSem::Ignore) df.pushed = false;
        df.pedVel = rb; df.havePedVel = true;
        df.anchorMode = o.anchorMode; df.anchor = o.anchorPos;
        df.nudged = o.nudge && !o.snap; df.nudge = o.nudgeDelta;
        df.rateSet = o.setMoveRate; df.rate = o.moveRate; df.rateClamped = o.rateClamped; df.selfSpeed = lc.SelfSpeed();
        diag.Observe(df);
        if (op.traceFromMs >= 0 && now >= op.traceFromMs && now < op.traceToMs)
            std::printf("    t=%6.0f pose (%.2f,%.2f) sp %.2f ped (%.2f,%.2f) speed %.2f err %.2f gait %d task %d why %d rate %.2f blend %.1f nudge %.3f lvl %d self %.2f verdict %d quiet %d\n", now,
                        ppos.x, ppos.y, poseSp, in.pedPos.x, in.pedPos.y, std::hypot(v.x, v.y), std::hypot(double(ppos.x) - in.pedPos.x, double(ppos.y) - in.pedPos.y),
                        int(o.gait), int(o.task), int(o.why), ped.rate, ped.blend, std::hypot(o.nudgeDelta.x, o.nudgeDelta.y), int(o.level), lc.SelfSpeed(), int(lc.Verdict()), int(o.quiet));
        ped.pos.x = float(ped.pos.x + v.x * dt / 1000.0);
        ped.pos.y = float(ped.pos.y + v.y * dt / 1000.0);
        if (now >= op.warmupMs) {
            errs.push_back(std::hypot(double(ppos.x) - in.pedPos.x, double(ppos.y) - in.pedPos.y));
            if (poseSp > 1.0) { speedRatioSum += std::hypot(v.x, v.y) / poseSp; ++speedRatioN; }
        }
        first = false;
        lastNow = now;
        now += dtBase * (0.8 + 0.4 * rng.Uniform());
    }
    res.st = lc.Stats();
    res.d = diag.Summary();
    std::sort(errs.begin(), errs.end());
    if (!errs.empty()) { res.p95Err = errs[size_t(errs.size() * 0.95)]; res.maxErr = errs.back(); }
    res.tasksPer10s = double(res.st.goStraight + res.st.standStill) * 10000.0 / sc.durMs;
    res.speedRatio = speedRatioN ? speedRatioSum / speedRatioN : 0;
    res.snaps = res.st.snaps;
    res.finalLevel = lastLevel;
    res.verdict = lc.Verdict();
    res.steadyGaitFrac = framesLate > 0 ? gaitFramesLate / framesLate : 0;
    return res;
}

static std::vector<Model> Models(bool full) {
    std::vector<Model> v;
    const Follow fl[] = {Follow::Honour, Follow::Sluggish, Follow::Dead, Follow::NoneMove};
    const VelSem vs[] = {VelSem::Replace, VelSem::Add, VelSem::Ignore};
    for (Follow f : fl)
        for (VelSem s : vs)
            for (int r = 0; r < 2; ++r)
                for (int p = 0; p < 2; ++p) {
                    if (f == Follow::NoneMove && (s != VelSem::Replace)) continue;   // nothing moves it: the velocity axis adds nothing new
                    Model m; m.follow = f; m.vel = s; m.rateScales = r == 0; m.pivot = p == 1;
                    v.push_back(m);
                }
    if (full) {
        std::vector<Model> w;
        for (Model m : v)
            for (double nom : {1.0, 0.85, 1.15}) { m.nominal = nom; w.push_back(m); }
        Model pm; pm.persistRead = true; w.push_back(pm);
        return w;
    }
    return v;
}

// The scenarios. SlideScenarios()'s "reversal" flips the direction at FULL speed (its speed dip is centred on t = 4k, the flip on
// t = 4k + 2: a velocity step of 9 m/s no player can make; it is kept as the stress case "instant flip"). The realistic ones:
// a stop, a turn on the spot and a start again.
static SlideScenario Reversal(const char* name, double v, double turnEveryS) {
    SlideScenario sc;
    sc.name = name;
    sc.speed = [v, turnEveryS](double t) {
        const double u = std::fmod(t + turnEveryS / 2, turnEveryS) - turnEveryS / 2;   // time from the nearest turn, -T/2 .. T/2
        const double lo = std::min(0.3, v);
        if (t < turnEveryS / 2) return v;
        if (u > -0.45 && u <= 0) return v - (v - lo) * (u + 0.45) / 0.45;
        if (u > 0 && u <= 0.15) return lo;
        if (u > 0.15 && u <= 0.75) return lo + (v - lo) * (u - 0.15) / 0.6;
        return v;
    };
    sc.dirDeg = [turnEveryS](double t) { return int(std::floor(t / turnEveryS)) % 2 ? 180.0 : 0.0; };
    return sc;
}
static SlideScenario Zigzag(const char* name, double v) {
    SlideScenario sc;
    sc.name = name;
    sc.speed = [v](double) { return v; };
    // a 70 degree change of direction every 1.5 s, made over 0.3 s (a player's smooth cut), alternating
    sc.dirDeg = [](double t) {
        const double k = std::floor(t / 1.5), u = t - k * 1.5;
        const double a0 = std::fmod(k, 2.0) < 1.0 ? -35.0 : 35.0, a1 = -a0;
        return u < 0.3 ? a0 + (a1 - a0) * u / 0.3 : a1;
    };
    return sc;
}
static std::vector<SlideScenario> Scenarios() {
    std::vector<SlideScenario> v = SlideScenarios();
    for (SlideScenario& sc : v)
        if (std::string(sc.name).find("reversal") != std::string::npos) sc.name = "stress: instant 180 flip at full speed";
    v.push_back(Reversal("reversal: stop, turn, go (run 4.5)", 4.5, 4.0));
    v.push_back(Reversal("reversal: stop, turn, go (walk 1.5)", 1.5, 4.0));
    v.push_back(Zigzag("zigzag: 70 deg cuts every 1.5 s (run 4.5)", 4.5));
    return v;
}

// The score counts everything but the teleport (a snap by design) and the instant flip (a stress case, scored on its own).
static bool Scored(const SlideScenario& sc) {
    const std::string n = sc.name;
    return n.find("teleport") == std::string::npos && n.find("stress:") == std::string::npos;
}

struct Score {
    double worstSkate = 0, worstP95 = 0, worstIdle = 0, sumSkate = 0, sumP95 = 0;
    std::string worstSkateAt, worstP95At;
    int n = 0;
};

static Score ScoreDesign(const LocoConfig& cfg, const std::vector<Model>& models, bool anchorEntity, bool print, const char* title) {
    Score s;
    const std::vector<SlideScenario> scs = Scenarios();
    RunOpts op; op.anchorEntity = anchorEntity;
    for (const Model& m : models)
        for (const SlideScenario& sc : scs) {
            if (!Scored(sc)) continue;
            const Result r = Run(sc, m, cfg, op);
            const double sk = r.d.skate;
            s.sumSkate += sk; s.sumP95 += r.p95Err; ++s.n;
            if (sk > s.worstSkate) { s.worstSkate = sk; s.worstSkateAt = m.Name() + " / " + sc.name; }
            if (r.p95Err > s.worstP95) { s.worstP95 = r.p95Err; s.worstP95At = m.Name() + " / " + sc.name; }
            s.worstIdle = std::max(s.worstIdle, r.d.idleSlideM);
        }
    if (print)
        std::printf("  %-34s %3d runs: skate worst %5.1f%% mean %4.1f%% | p95 err worst %5.2f m mean %4.2f m | idle slide worst %5.2f m\n", title, s.n,
                    s.worstSkate * 100, s.sumSkate / s.n * 100, s.worstP95, s.sumP95 / s.n, s.worstIdle);
    return s;
}

// ---- the designs ----------------------------------------------------------------------------------------------------------

// R3 T2: the shipped config with every round-3 rule switched off = the round-2 (0.4.7) controller, for before / after tables.
static LocoConfig R2Cfg(bool anchor) { return Round2Behaviour(ShippedLocoConfig(anchor)); }

// HEAD (4e2ef63) as shipped: anchor, velocity residual push, closed-loop rate, no self-checks.
static LocoConfig HeadCfg(LocoMode mode) { LocoConfig c; c.mode = mode; return c; }

// One line per model: mean skate / mean p95 error / worst p95 over the scored scenarios, for each design.
static void ExploreByModel(const std::vector<std::pair<const char*, LocoConfig>>& designs, const std::vector<Model>& models) {
    const std::vector<SlideScenario> scs = Scenarios();
    std::printf("  %-52s", "model");
    for (const auto& d : designs) std::printf(" | %-22s", d.first);
    std::printf("\n");
    for (const Model& m : models) {
        std::printf("  %-52s", m.Name().c_str());
        for (const auto& d : designs) {
            RunOpts op;
            double sk = 0, p95 = 0, wp = 0; int n = 0;
            for (const SlideScenario& sc : scs) {
                if (!Scored(sc)) continue;
                const Result r = Run(sc, m, d.second, op);
                sk += r.d.skate; p95 += r.p95Err; wp = std::max(wp, r.p95Err); ++n;
            }
            std::printf(" | %4.1f%% %5.2f %5.2f    ", sk / n * 100, p95 / n, wp);
        }
        std::printf("\n");
    }
}

static void ExploreByScenario(const LocoConfig& cfg, const Model& m) {
    RunOpts op;
    std::printf("    %-52s %6s %6s %6s %6s %6s %6s %5s %s\n", "scenario", "tasks", "skate", "idle m", "p95err", "maxerr", "spd/ps", "snaps", "level");
    for (const SlideScenario& sc : Scenarios()) {
        const Result r = Run(sc, m, cfg, op);
        std::printf("    %-52s %6.1f %6.3f %6.2f %6.2f %6.2f %6.2f %5u %d (%zu changes", sc.name, r.tasksPer10s, r.d.skate, r.d.idleSlideM, r.p95Err, r.maxErr,
                    r.speedRatio, r.snaps, int(r.finalLevel), r.changes.size());
        for (const auto& c : r.changes) std::printf(" ->%d/why%d", int(c.first), int(c.second));
        std::printf("; first at %.0f ms)\n", r.firstLevelChangeMs);
    }
}

// Classes: "good" = the game works as the code assumed (honour, rate scales); "rate" = honour but the rate native is ignored;
// "slow" = the anchor task follows at 60%; "dead" = it never moves the ped. Mean skate / mean p95 / false degradations per class.
static void SweepRow(const char* label, const LocoConfig& cfg, const std::vector<Model>& models) {
    const std::vector<SlideScenario> scs = Scenarios();
    RunOpts op;
    struct Cls { double sk = 0, p95 = 0; int n = 0, changed = 0, runs = 0; } c[4];
    for (const Model& m : models) {
        if (m.follow == Follow::NoneMove) continue;
        const int k = m.follow == Follow::Dead ? 3 : m.follow == Follow::Sluggish ? 2 : (m.rateScales ? 0 : 1);
        double sk = 0, p95 = 0; int n = 0;
        for (const SlideScenario& sc : scs) {
            if (!Scored(sc)) continue;
            const Result r = Run(sc, m, cfg, op);
            sk += r.d.skate; p95 += r.p95Err; ++n; ++c[k].runs;
            if (!r.changes.empty()) ++c[k].changed;
        }
        c[k].sk += sk / n; c[k].p95 += p95 / n; ++c[k].n;
    }
    std::printf("  %-34s", label);
    const char* nm[4] = {"good", "rate-ign", "slow", "dead"};
    for (int k = 0; k < 4; ++k)
        std::printf(" | %-8s %5.1f%% %4.2f (%2d/%2d chg)", nm[k], c[k].sk / c[k].n * 100, c[k].p95 / c[k].n, c[k].changed, c[k].runs);
    std::printf("\n");
}

static void FalseChanges() {
    RunOpts op;
    for (const Model& m : Models(true)) {
        if (m.follow != Follow::Honour || !m.rateScales) continue;
        for (const SlideScenario& sc : Scenarios()) {
            if (!Scored(sc)) continue;
            const Result r = Run(sc, m, ShippedLocoConfig(true), op);
            if (!r.changes.empty()) {
                std::printf("  false change: %s / %s:", m.Name().c_str(), sc.name);
                for (const auto& c : r.changes) std::printf(" ->%d/why%d", int(c.first), int(c.second));
                std::printf(" at %.0f ms\n", r.firstLevelChangeMs);
            }
        }
    }
}

static void Explore() {
    if (std::getenv("T2_FALSE")) { FalseChanges(); return; }
    if (std::getenv("T2_SWEEP")) {
        std::vector<Model> ms;
        for (const Model& m : Models(false)) if (m.follow != Follow::NoneMove) ms.push_back(m);
        SweepRow("HEAD anchor", HeadCfg(LocoMode::Anchor), ms);
        SweepRow("HEAD straight", HeadCfg(LocoMode::Straight), ms);
        {
            LocoConfig c = ShippedLocoConfig(true); c.selfCheck = false;
            SweepRow("T2 anchor nudge, no ladder", c, ms);
        }
        SweepRow("T2 anchor (shipped: nudge + ladder)", ShippedLocoConfig(true), ms);
        SweepRow("T2 straight (shipped)", ShippedLocoConfig(false), ms);
        return;
    }
    if (std::getenv("T2_BYMODEL")) {
        std::vector<std::pair<const char*, LocoConfig>> ds;
        ds.push_back({"HEAD anchor", HeadCfg(LocoMode::Anchor)});
        ds.push_back({"HEAD straight", HeadCfg(LocoMode::Straight)});
        ds.push_back({"T2 anchor", ShippedLocoConfig(true)});
        ds.push_back({"T2 straight", ShippedLocoConfig(false)});
        ExploreByModel(ds, Models(false));
        return;
    }
    if (std::getenv("T2_SC")) {
        Model m;
        const char* e = std::getenv("T2_SC");
        m.follow = Follow(e[0] - '0'); m.vel = VelSem(e[1] - '0'); m.rateScales = e[2] == '1'; m.pivot = e[3] == '1';
        const bool ship = std::getenv("T2_SHIP") != nullptr;
        if (std::getenv("T2_TRACE")) {
            RunOpts op; op.traceFromMs = std::atof(std::getenv("T2_TRACE")); op.traceToMs = op.traceFromMs + (std::getenv("T2_LEN") ? std::atof(std::getenv("T2_LEN")) : 3000);
            const std::vector<SlideScenario> scs = Scenarios();
            Run(scs[std::getenv("T2_N") ? std::atoi(std::getenv("T2_N")) : 11], m, ship ? ShippedLocoConfig(true) : HeadCfg(LocoMode::Anchor), op);
            return;
        }
        ExploreByScenario(ship ? ShippedLocoConfig(true) : HeadCfg(LocoMode::Anchor), m);
        return;
    }
    const bool full = std::getenv("T2_FULL") != nullptr;
    const std::vector<Model> models = Models(full);
    std::printf("  T2 model matrix: %zu models x scored scenarios\n", models.size());
    ScoreDesign(HeadCfg(LocoMode::Anchor), models, true, true, "HEAD anchor (velocity push)");
    ScoreDesign(HeadCfg(LocoMode::Straight), models, true, true, "HEAD straight (loco_mode=straight)");
    ScoreDesign(R2Cfg(true), models, true, true, "round-2 anchor (nudge + ladder, 0.4.7)");
    ScoreDesign(ShippedLocoConfig(true), models, true, true, "T2 shipped anchor (nudge + ladder)");
    ScoreDesign(ShippedLocoConfig(false), models, true, true, "T2 shipped straight (nudge + ladder)");
    {
        LocoConfig c = ShippedLocoConfig(true); c.selfCheck = false;
        ScoreDesign(c, models, true, true, "T2 anchor, nudge, NO ladder");
    }
}


// ---- the checks ------------------------------------------------------------------------------------------------------------

struct Agg {
    double worstP95 = 0, meanSkate = 0, worstSkate = 0, meanP95 = 0;
    int runs = 0, changed = 0, changes = 0;
    std::string worstAt;
};

// Every scored scenario (or those whose name contains `only`) against the models, for one config.
static Agg Collect(const LocoConfig& cfg, const std::vector<Model>& models, const char* only = nullptr) {
    Agg a;
    RunOpts op;
    for (const Model& m : models)
        for (const SlideScenario& sc : Scenarios()) {
            if (!Scored(sc)) continue;
            if (only && std::string(sc.name).find(only) == std::string::npos) continue;
            const Result r = Run(sc, m, cfg, op);
            ++a.runs;
            a.meanSkate += r.d.skate;
            a.meanP95 += r.p95Err;
            a.worstSkate = std::max(a.worstSkate, r.d.skate);
            if (r.p95Err > a.worstP95) { a.worstP95 = r.p95Err; a.worstAt = m.Name() + " / " + sc.name; }
            if (!r.changes.empty()) { ++a.changed; a.changes += int(r.changes.size()); }
        }
    if (a.runs) { a.meanSkate /= a.runs; a.meanP95 /= a.runs; }
    return a;
}

static std::vector<Model> Select(const std::function<bool(const Model&)>& pred, bool full = false) {
    std::vector<Model> v;
    for (const Model& m : Models(full)) if (pred(m)) v.push_back(m);
    return v;
}

static SlideScenario ByName(const char* frag) {
    for (const SlideScenario& sc : Scenarios()) if (std::string(sc.name).find(frag) != std::string::npos) return sc;
    return Scenarios()[0];
}

// A one-frame unit input like the older tests': a run at 4.5 m/s along +x, the ped `behind` m behind the pose.
static LocoInput Unit(double behind, double dtMs = 1000.0 / 60) {
    LocoInput in;
    in.vel = V(4.5, 0, 0); in.heading = 270;
    in.pos = V(100, 0, 30); in.pedPos = V(100 - behind, 0, 30);
    in.pedHeading = 270;
    in.pedGait = Gait::Run;
    in.dtMs = dtMs;
    return in;
}

static void TestMatrix() {
    // The 0.4.6 default (anchor + velocity push) and the shipped T2 design, on every model of the game that is not "the game
    // works exactly as the 0.4.6 code assumed". Bound = what a player sees as "not where it is" (1 m).
    const std::vector<Model> unknown = Select([](const Model& m) {
        return m.follow != Follow::NoneMove && (m.vel != VelSem::Replace || !m.rateScales || m.follow != Follow::Honour);
    });
    const Agg head = Collect(HeadCfg(LocoMode::Anchor), unknown);
    const Agg ship = Collect(ShippedLocoConfig(true), unknown);
    Check("T2 matrix: on 30 models where the game does NOT behave as the 0.4.6 code assumed (velocity ignored / added, stride not scaled, go-to slow or dead), the shipped design keeps the mean error p95 under 0.7 m",
          ship.runs >= 300 && ship.meanP95 <= 0.7, F("%d runs; shipped mean p95 %.2f m (worst %.2f at %s)", ship.runs, ship.meanP95, ship.worstP95, ship.worstAt.c_str()));
    Check("T2 matrix: ... the 0.4.6 default does not (mean p95 over 1.1 m, so the bound above is binding)",
          head.runs == ship.runs && head.meanP95 >= 1.1, F("0.4.6 mean p95 %.2f m (worst %.2f at %s)", head.meanP95, head.worstP95, head.worstAt.c_str()));
    // The model the 0.4.6 code was written for: the shipped design must not be worse there.
    const std::vector<Model> good = Select([](const Model& m) { return m.follow == Follow::Honour && m.rateScales && m.vel == VelSem::Replace; }, true);
    const Agg gh = Collect(HeadCfg(LocoMode::Anchor), good), gs = Collect(ShippedLocoConfig(true), good);
    Check("T2 matrix: where the game works as assumed (6 variants incl. stride x0.85 / x1.15), the shipped design is at least as good as 0.4.6 (error p95 mean no worse, skate within 1.5 points) and under 3% skate",
          gs.meanP95 <= gh.meanP95 + 0.01 && gs.meanSkate <= gh.meanSkate + 0.015 && gs.meanSkate <= 0.03,
          F("%d runs; shipped p95 %.2f skate %.1f%% | 0.4.6 p95 %.2f skate %.1f%%", gs.runs, gs.meanP95, gs.meanSkate * 100, gh.meanP95, gh.meanSkate * 100));
    // A straight-mode user (loco_mode=straight) gets the nudge and the ladder too.
    const Agg st = Collect(ShippedLocoConfig(false), unknown), sth = Collect(HeadCfg(LocoMode::Straight), unknown);
    Check("T2 matrix: loco_mode=straight with the nudge: mean error p95 on the 30 unknown models under 0.7 m (round 2: 0.6, the quiet first task of round 3 costs the rest; 0.4.6 straight: over 1.1 m)",
          st.meanP95 <= 0.7 && sth.meanP95 >= 1.1, F("shipped straight %.2f m, 0.4.6 straight %.2f m", st.meanP95, sth.meanP95));
}

static void TestNudge() {
    // The nudge is a coordinate write, bounded per frame, silent inside the dead band, and nothing but a coordinate (no velocity).
    LocoConfig cfg = ShippedLocoConfig(false);
    double maxStep = 0; bool anyVel = false; size_t nudges = 0; double sumX = 0;
    {
        LocomotionController lc(cfg);
        for (int i = 0; i < 180; ++i) lc.Update(Unit(0));                      // 3 s on the pose: past the start
        for (int i = 0; i < 90; ++i) {
            const LocoOutput o = lc.Update(Unit(1.6));                         // the ped stays 1.6 m behind (it is not moved here)
            if (o.nudge) { ++nudges; maxStep = std::max(maxStep, std::hypot(double(o.nudgeDelta.x), double(o.nudgeDelta.y))); sumX += o.nudgeDelta.x; }
            if (o.setVelocity) anyVel = true;
        }
    }
    Check("T2 nudge: a ped 1.6 m behind a running pose is nudged forward every frame, never by more than nudgeMaxMs x dt (3 m/s -> 50 mm at 60 fps) and without a velocity write",
          nudges >= 80 && maxStep <= 3.0 / 60.0 + 1e-4 && maxStep >= 0.04 && !anyVel && sumX > 0, F("%zu nudge frames, max step %.4f m, velocity writes %d, total x %.2f m", nudges, maxStep, int(anyVel), sumX));
    size_t inBand = 0;
    {
        LocomotionController lc(cfg);
        for (int i = 0; i < 180; ++i) lc.Update(Unit(0));
        for (int i = 0; i < 90; ++i) if (lc.Update(Unit(0.10)).nudge) ++inBand;
    }
    Check("T2 nudge: 0.10 m behind (inside the 0.12 m dead band) nothing is written, 0.5 m behind it is (the dead band is the only thing that stays the stride's)",
          inBand == 0, F("%zu nudge frames at 0.10 m", inBand));
    size_t at05 = 0;
    {
        LocomotionController lc(cfg);
        for (int i = 0; i < 180; ++i) lc.Update(Unit(0));
        for (int i = 0; i < 90; ++i) if (lc.Update(Unit(0.5)).nudge) ++at05;
    }
    Check("T2 nudge: ... and 0.5 m behind it nudges (presence for the absence above)", at05 >= 80, F("%zu nudge frames at 0.5 m", at05));
    // legacy config: no nudge at all, the velocity push stays
    {
        LocomotionController lc;
        size_t n = 0, v = 0;
        for (int i = 0; i < 180; ++i) lc.Update(Unit(0));
        for (int i = 0; i < 90; ++i) { LocoInput in = Unit(1.0); in.pedVel = V(4.5, 0, 0); in.havePedVel = true; const LocoOutput o = lc.Update(in); n += o.nudge; v += o.setVelocity; }
        Check("T2 nudge: the legacy LocoConfig (push = Velocity) never nudges and still pushes velocity (the 77 older checks are about that path)", n == 0 && v > 0, F("%zu nudges, %zu velocity pushes", n, v));
    }
}

static void TestSelfSpeed() {
    // The ped's own speed is its displacement net of what we wrote: a ped that only moves because we nudge it has an own speed of
    // ~0 (it is "stuck"), a ped that walks 3 m/s on its own and is nudged on top still reads 3 m/s.
    auto run = [](double ownSpeed, bool nudgeable) {
        LocoConfig cfg = ShippedLocoConfig(false);
        LocomotionController lc(cfg);
        LocoVec ped = V(100 - 1.5, 0, 30);
        float last = -1;
        double t = 0;
        for (int i = 0; i < 240; ++i, t += 1000.0 / 60) {
            LocoInput in = Unit(0);
            in.pos = V(100 + 4.5 * t / 1000.0, 0, 30);
            in.pedPos = ped;
            const LocoOutput o = lc.Update(in);
            if (o.snap) ped = o.snapPos;
            if (nudgeable && o.nudge) { ped.x += o.nudgeDelta.x; ped.y += o.nudgeDelta.y; }
            ped.x += float(ownSpeed / 60.0);
            last = lc.SelfSpeed();
        }
        return last;
    };
    const float stuck = run(0.0, true), walking = run(3.0, true);
    Check("T2 self speed: the ped's own speed is measured net of our nudges: a ped that never moves by itself reads ~0 m/s however hard it is nudged, one that walks 3 m/s reads ~3 m/s",
          stuck >= 0.0f && stuck < 0.3f && std::fabs(walking - 3.0f) < 0.3f, F("never moving %.2f m/s, walking 3 m/s %.2f m/s", stuck, walking));
}

static void TestLadder() {
    // The degradation ladder, one failure at a time. "Detected" = a levelChanged with the right reason inside the time, and the
    // level it lands on; the error afterwards is the sim's p95 over the rest of the run.
    const SlideScenario run = ByName("run 4.5 straight");
    RunOpts op;
    auto first = [&](const Model& m, const LocoConfig& cfg, const SlideScenario& sc) { return Run(sc, m, cfg, op); };
    {
        Model m; m.follow = Follow::Dead;
        const Result r = first(m, ShippedLocoConfig(true), run);
        const bool why = !r.changes.empty() && (r.changes[0].second == LocoLevelWhy::AnchorLost || r.changes[0].second == LocoLevelWhy::AnchorStuck);
        Check("T2 ladder: a go-to-entity task that never moves the ped is found within 3 s (AnchorLost / AnchorStuck), the controller walks straight from then on and the error p95 stays under 0.3 m",
              why && r.firstLevelChangeMs > 0 && r.firstLevelChangeMs <= 3000 && r.changes.size() == 1 && r.finalLevel == LocoLevel::Straight && r.p95Err <= 0.3,
              F("level change at %.0f ms (%zu changes), final level %d, p95 %.2f m", r.firstLevelChangeMs, r.changes.size(), int(r.finalLevel), r.p95Err));
        const Result h = first(m, HeadCfg(LocoMode::Anchor), run);
        Check("T2 ladder: ... 0.4.6 reports nothing (no level exists) and drifts 1+ m with a velocity that is not honoured",
              h.changes.empty() && h.finalLevel == LocoLevel::Full, F("0.4.6: %zu changes, p95 %.2f m", h.changes.size(), h.p95Err));
    }
    {
        Model m; m.follow = Follow::Sluggish; m.rateScales = false;
        const Result r = first(m, ShippedLocoConfig(true), run);
        bool drag = false;
        for (const auto& c : r.changes) drag = drag || c.second == LocoLevelWhy::AnchorDragging;
        Check("T2 ladder: a go-to-entity task that follows at 60% of the speed is found by its nudge share (AnchorDragging, over 10% of the distance) within 6 s and replaced by the straight task",
              drag && r.firstLevelChangeMs > 0 && r.firstLevelChangeMs <= 6000, F("first level change at %.0f ms, %zu changes, final %d", r.firstLevelChangeMs, r.changes.size(), int(r.finalLevel)));
    }
    {
        // (a steady run never varies the rate, so nothing could be learned from it and nothing is wrong: the gait-change walk does)
        Model m; m.rateScales = false;
        const SlideScenario gc = ByName("gait changes");
        const Result r = first(m, ShippedLocoConfig(true), gc);
        bool rate = false; for (const auto& c : r.changes) rate = rate || c.second == LocoLevelWhy::RateIneffective;
        const Result n = first(m, [] { LocoConfig c = ShippedLocoConfig(true); c.selfCheck = false; return c; }(), gc);
        Check("T2 ladder: SET_PED_MOVE_RATE_OVERRIDE ignored by the game: found by the regression of the ped's own speed on the rate within 8 s (RateIneffective), the speed then rides on the blend ratio WITHOUT re-issuing the task (<= 1 task per 10 s)",
              rate && r.firstLevelChangeMs > 0 && r.firstLevelChangeMs <= 8000 && r.finalLevel == LocoLevel::NoRate && r.tasksPer10s <= 1.0,
              F("level change at %.0f ms, final %d, %.1f tasks/10 s, p95 %.2f m (no ladder: p95 %.2f m)", r.firstLevelChangeMs, int(r.finalLevel), r.tasksPer10s, r.p95Err, n.p95Err));
    }
    {
        Model m; m.follow = Follow::NoneMove;
        RunOpts norep = op; norep.repairsDone0 = ShippedLocoConfig(true).maxRepairs;   // R3: the engine has no repair left to try (the round-3 repairs: tests/r3_t2_tests.h)
        const Result r = Run(run, m, ShippedLocoConfig(true), norep);
        const Result h = first(m, HeadCfg(LocoMode::Anchor), run);
        Check("T2 ladder: when no task moves the ped at all (and no repair is left) it follows the pose rigidly within 7 s (Placed: StraightStuck) and the run's error p95 is under 3.2 m (round 2: 1.5; the 1.55 s quiet start of the first task and the 1.5 s first-gait window cost the difference; 0.4.6: over 3.5 m)",
              r.finalLevel == LocoLevel::Placed && r.lastLevelChangeMs > 0 && r.lastLevelChangeMs <= 7000 && r.p95Err <= 3.2 && h.p95Err >= 3.5,
              F("final level %d, Placed at %.0f ms, p95 %.2f m; 0.4.6 p95 %.2f m", int(r.finalLevel), r.lastLevelChangeMs, r.p95Err, h.p95Err));
    }
    {
        // No false alarm: the game works (follow honoured, stride scaled), whatever it does with velocities and however far the real
        // stride is from the guessed nominals: not one level change in 4 gaits x 3 nominals x 12 scenarios.
        const std::vector<Model> good = Select([](const Model& m) { return m.follow == Follow::Honour && m.rateScales; }, true);
        const Agg a = Collect(ShippedLocoConfig(true), good);
        Check("T2 ladder: NO level change in a working game (stride x1 / x0.85 / x1.15, velocity replace / add / ignore, arc and pivot turns, all 12 scenarios incl. reversals, zigzag, stop-and-go)",
              a.runs >= 200 && a.changed == 0, F("%d runs, %d with a level change", a.runs, a.changed));
    }
}

static void TestDegradeAndWitness() {
    LevelWitness w;
    const double now = 100000;
    const bool one = w.Report(LocoLevelWhy::AnchorLost, 1, now);
    const bool same = w.Report(LocoLevelWhy::AnchorStuck, 1, now + 1000);
    const double alone = w.ActiveMs(LocoLevel::Straight, now + 2000);
    const bool two = w.Report(LocoLevelWhy::AnchorDragging, 2, now + 5000);
    const double active = w.ActiveMs(LocoLevel::Straight, now + 6000), expired = w.ActiveMs(LocoLevel::Straight, now + 5000 + LevelWitness::kImposeStraightMs + 1);
    const bool late = w.Report(LocoLevelWhy::AnchorLost, 3, now + 5000 + LevelWitness::kWitnessMs * 3);   // a witness older than the window does not count
    const bool placed = w.Report(LocoLevelWhy::StraightStuck, 4, now + 7000) || w.Report(LocoLevelWhy::StraightStuck, 5, now + 7500);
    Check("T2 witness: one puppet (even twice) imposes nothing; a second DIFFERENT puppet within 60 s imposes the straight task on all for 45 s; it expires; a stale witness does not count; Placed is never shared",
          !one && !same && alone == 0.0 && two && std::fabs(active - (LevelWitness::kImposeStraightMs - 1000)) < 1e-6 && expired == 0.0 && !late && !placed,
          F("one %d same %d alone %.0f two %d active %.0f expired %.0f late %d placed %d", int(one), int(same), alone, int(two), active, expired, int(late), int(placed)));
    // Degrade(): the controller runs the straight task from the next frame and reports why.
    LocoConfig cfg = ShippedLocoConfig(true);
    LocomotionController a(cfg), b(cfg);
    b.Degrade(LocoLevel::Straight, LocoLevelWhy::Shared, 30000);
    bool aAnchor = false, bAnchor = false, bChanged = false; LocoLevelWhy why = LocoLevelWhy::None; LocoTask bTask = LocoTask::None, aTask = LocoTask::None;
    for (int i = 0; i < 120; ++i) {
        LocoInput in = Unit(0);
        const LocoOutput oa = a.Update(in), ob = b.Update(in);
        aAnchor = aAnchor || oa.anchorMode; bAnchor = bAnchor || ob.anchorMode;
        if (oa.task == LocoTask::GoToAnchor) aTask = oa.task;
        if (ob.task == LocoTask::GoStraight) bTask = ob.task;
        if (ob.levelChanged) { bChanged = true; why = ob.levelWhy; }
    }
    Check("T2 witness: a controller that was told to degrade (Degrade(Straight, Shared)) never enters anchor mode, issues the straight task and reports level 1 'Shared' once; an untouched twin runs the anchor task",
          aAnchor && aTask == LocoTask::GoToAnchor && !bAnchor && bTask == LocoTask::GoStraight && bChanged && why == LocoLevelWhy::Shared && b.Level() == LocoLevel::Straight && a.Level() == LocoLevel::Full,
          F("twin anchor %d, degraded anchor %d task %d changed %d why %d level %d", int(aAnchor), int(bAnchor), int(bTask), int(bChanged), int(why), int(b.Level())));
}

static void TestReversalAndDownShift() {
    // Realistic reversal (stop, turn on the spot, go): the error of a puppet that arcs round (the Arc model) and one that pivots.
    const SlideScenario rev = ByName("stop, turn, go (run");
    const std::vector<Model> two = Select([](const Model& m) { return m.follow == Follow::Honour && m.rateScales && m.vel == VelSem::Replace; });
    double hs = 0, ss = 0, hp = 0, sp = 0; RunOpts op;
    for (const Model& m : two) {
        const Result h = Run(rev, m, HeadCfg(LocoMode::Anchor), op), s = Run(rev, m, ShippedLocoConfig(true), op);
        hs += h.d.skate; ss += s.d.skate; hp = std::max(hp, h.p95Err); sp = std::max(sp, s.p95Err);
    }
    Check("T2 reversal: stop-turn-go at a run (arc and pivot ped): the shipped error p95 is at most 0.7 m (0.4.6: over 0.9 m) and the skate no worse than 0.4.6",
          sp <= 0.7 && hp >= 0.9 && ss <= hs + 0.02, F("shipped p95 %.2f skate %.1f%% | 0.4.6 p95 %.2f skate %.1f%%", sp, ss / two.size() * 100, hp, hs / two.size() * 100));
    // The gait drops fast when the pose has fallen far under the gait's band.
    auto dropAt = [](const LocoConfig& cfg) {
        LocomotionController lc(cfg);
        for (int i = 0; i < 180; ++i) lc.Update(Unit(0));
        double t = 0, at = -1;
        for (int i = 0; i < 90; ++i, t += 1000.0 / 60) {
            LocoInput in = Unit(0);
            in.vel = V(0.9, 0, 0);                 // 0.9 m/s: under 60% of the run band's down edge (2.1 -> 1.26), over the idle edge
            in.pedGait = Gait::Run;
            const LocoOutput o = lc.Update(in);
            if (o.gait == Gait::Walk && at < 0) at = t;
        }
        return at;
    };
    const double fast = dropAt(ShippedLocoConfig(false)), slow = dropAt(HeadCfg(LocoMode::Straight));
    Check("T2 reversal: a pose that falls to 0.9 m/s drops a Run to a Walk within 150 ms (0.4.6: after the 400 ms debounce)", fast >= 0 && fast <= 150 && slow >= 380, F("shipped %.0f ms, 0.4.6 %.0f ms", fast, slow));
}

static void TestDiagAndAudit() {
    // LocoDiag counts a nudge as skate (it is a displacement the animation did not make) and reports the rate.
    LocoDiag carried, nudged;
    const double dt = 1000.0 / 60;
    for (int i = 0; i < 120; ++i) {
        LocoDiagFrame f;
        f.dtMs = dt; f.nowMs = i * dt;
        f.pose = V(4.0 * i * dt / 1000.0 + 0.2, 0, 0); f.pedPos = V(4.0 * i * dt / 1000.0, 0, 0);
        f.poseSpeed = 4.0f; f.gait = Gait::Run; f.rateSet = true; f.rate = (i % 2) ? 1.6f : 0.9f; f.rateClamped = (i % 2) != 0; f.selfSpeed = 3.9f;
        carried.Observe(f);
        f.nudged = true; f.nudge = V(0.04, 0, 0);                  // 2.4 m/s of nudge on a 4 m/s run: 0.04 / (4 x dt 0.0167 = 0.067) = 60% of the distance
        nudged.Observe(f);
    }
    const LocoDiagSummary a = carried.Summary(), b = nudged.Summary();
    Check("T2 diag: LocoDiag counts nudges as skate (0 without, ~0.6 with 40 mm per frame on a 4 m/s run), their speed (2.4 m/s), and the move rate applied (min 0.9 max 1.6, half the frames at a limit)",
          a.skate < 0.01 && b.skate > 0.5 && b.skate < 0.7 && std::fabs(b.meanNudgeMs - 2.4) < 0.1 && b.nudgeFrames == 120 && std::fabs(b.rateMin - 0.9) < 1e-3 && std::fabs(b.rateMax - 1.6) < 1e-3 &&
              std::fabs(b.rateClampFrac - 0.5) < 0.01 && std::fabs(b.selfSpeedMs - 3.9) < 0.01,
          F("skate %.3f / %.3f, nudge mean %.2f m/s, rate %.2f..%.2f clamp %.2f, own speed %.2f", a.skate, b.skate, b.meanNudgeMs, b.rateMin, b.rateMax, b.rateClampFrac, b.selfSpeedMs));

    // OneShotAudit: shown, missed, unreadable, a replaced request, the expected state false (stealth off), a cancelled one.
    OneShotAudit au;
    au.Arm(OneShot::Jump, 1000);
    OneShotEvent e1 = au.Observe(OneShot::Jump, 0, 1100), e2 = au.Observe(OneShot::Jump, 1, 1300);
    au.Arm(OneShot::Reload, 2000);
    OneShotEvent e3 = au.Observe(OneShot::Reload, 0, 2500), e4 = au.Observe(OneShot::Reload, 0, 2950);
    au.Arm(OneShot::Swim, 3000);
    OneShotEvent e5 = au.Observe(OneShot::Swim, -1, 3500), e6 = au.Observe(OneShot::Swim, -1, 5600);
    au.Arm(OneShot::Stealth, 6000, false);
    OneShotEvent e7 = au.Observe(OneShot::Stealth, 1, 6200), e8 = au.Observe(OneShot::Stealth, 0, 6300);
    au.Arm(OneShot::Ragdoll, 7000);
    const OneShotEvent rep = au.Arm(OneShot::Ragdoll, 7100);      // replaces an unanswered request: judged missed (the readback was never taken)
    au.Cancel(OneShot::Ragdoll);
    const OneShotEvent e9 = au.Observe(OneShot::Ragdoll, 1, 7200);
    const OneShotStats &j = au.Stats(OneShot::Jump), &r = au.Stats(OneShot::Reload), &w = au.Stats(OneShot::Swim), &s = au.Stats(OneShot::Stealth), &g = au.Stats(OneShot::Ragdoll);
    Check("T2 audit: a jump is SHOWN when the readback says so inside its window (300 ms), a reload whose readback never says yes is MISSED at 900 ms, an unreadable swim is UNKNOWN (never missed), stealth off is shown by a false readback, a replaced request is judged, a cancelled one is ignored",
          e1 == OneShotEvent::None && e2 == OneShotEvent::Shown && j.shown == 1 && std::fabs(j.showMsSum - 300) < 1e-6 &&
              e3 == OneShotEvent::None && e4 == OneShotEvent::Missed && r.missed == 1 && r.shown == 0 &&
              e5 == OneShotEvent::None && e6 == OneShotEvent::Unknown && w.unknown == 1 && w.missed == 0 &&
              e7 == OneShotEvent::None && e8 == OneShotEvent::Shown && s.shown == 1 && rep == OneShotEvent::Unknown && g.unknown == 1 && g.issued == 2 && e9 == OneShotEvent::None,
          F("%d %d %d %d %d %d %d %d rep %d %d", int(e1), int(e2), int(e3), int(e4), int(e5), int(e6), int(e7), int(e8), int(rep), int(e9)));
}

} // namespace t2

static void T2OnFootTests() {
    std::printf("-- T2: on-foot locomotion against many models of the game\n");
    if (std::getenv("T2_EXPLORE")) { t2::Explore(); return; }
    t2::TestMatrix();
    t2::TestNudge();
    t2::TestSelfSpeed();
    t2::TestLadder();
    t2::TestDegradeAndWitness();
    t2::TestReversalAndDownShift();
    t2::TestDiagAndAudit();
}
