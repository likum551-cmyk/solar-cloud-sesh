// R4 T1 (desync round 4): BLEND, a velocity-driven walking controller (docs/sync-specs/onfoot.md "R4 T1").
//
// Included by tests/locomotion_test.cpp after tests/r3_t2_tests.h (it uses that file's Check / F / V / Rng / WrapDeg / kPi / Rng) and
// called once from main as R4T1Tests(). R4T1_TABLE=1 prints the model x scenario tables the report quotes.
//
// WHAT GI SAW (0.4.9 = loco_mode straight + loco_push velocity, live): the straight task DID move the puppet, but 1.3-2x too fast
// (speed from positions 7.7 / 10.7 / 8.9 m/s against a pose of 4.5 / 6.1 / 3.9), the velocity push (animation read + a capped correction)
// peaked at 30.7-33.6 m/s, the pose-ped error reached 6.0-7.8 m (= the persistent-error snap = the teleports GI saw) and 11-40 velocity reads
// per 10 s equalled our own write. Nothing in that log says WHICH behaviour of the game does it, so the ped here is a set of switchable MODELS
// (the 2026-11-04 health lesson): every scenario runs under every model and the tables say which pass.
//
// The models (independent axes of Game):
//   gait source   A  the task gives no gait at all (the ped stands in an idle pose, the IS_PED_* natives read Still, it does not move on its own)
//                 B  the task moves the ped at a gait speed the controller's move rate cannot scale (1.5x / 2x the controller's nominals)
//                 S  the task moves it at the nominal speeds and SET_PED_MOVE_RATE_OVERRIDE scales it (the world round 2 assumed)
//   our velocity  D  SET_ENTITY_VELOCITY is honoured (the ped moves by what we write)
//                 C  it is PARTLY overwritten by the task's own velocity (50%: the ped moves by half of what we write plus half of the task's)
//                 E  it is ignored under a task (a ped nothing but the task moves)
//   extras        mbrContinuous  a fractional move blend ratio gives an in-between gait speed (else it rounds to walk / run / sprint)
//                 mbrLive        SET_PED_DESIRED_MOVE_BLEND_RATIO changes the speed of a RUNNING task (else only an issue does)
//                 readLies       GET_ENTITY_VELOCITY returns the velocity we last wrote
//                 readSpikes     a read returns 6x the real velocity now and then (a physics hiccup)
// The pose feed is the 30 Hz sample interpolator of the earlier tests with the round-4 numbers: 80 ms jitter, 2% loss, a 130 ms render delay.

#include <map>
#include <set>

namespace r4 {

enum class GaitSrc { None, Fixed, Scales };

struct Game {
    GaitSrc src = GaitSrc::Scales;
    double fast = 1.0;            // the true gait speeds = the controller's nominal speeds (1.5 / 4.5 / 7) x fast (Fixed)
    double auth = 1.0;            // D 1.0, C 0.5, E 0.0
    bool mbrContinuous = true;
    bool mbrLive = true;
    bool readLies = false;
    double readSpikeP = 0.0;
    const char* name = "";
};

static Game Cell(GaitSrc s, double fast, double auth, const char* name) {
    Game g; g.src = s; g.fast = fast; g.auth = auth; g.name = name;
    return g;
}
// the 12 cells: gait source x velocity authority
static std::vector<Game> Cells() {
    return {
        Cell(GaitSrc::None, 1.0, 1.0, "A/D"), Cell(GaitSrc::None, 1.0, 0.5, "A/C"), Cell(GaitSrc::None, 1.0, 0.0, "A/E"),
        Cell(GaitSrc::Fixed, 1.5, 1.0, "B1.5/D"), Cell(GaitSrc::Fixed, 1.5, 0.5, "B1.5/C"), Cell(GaitSrc::Fixed, 1.5, 0.0, "B1.5/E"),
        Cell(GaitSrc::Fixed, 2.0, 1.0, "B2/D"), Cell(GaitSrc::Fixed, 2.0, 0.5, "B2/C"), Cell(GaitSrc::Fixed, 2.0, 0.0, "B2/E"),
        Cell(GaitSrc::Scales, 1.0, 1.0, "S/D"), Cell(GaitSrc::Scales, 1.0, 0.5, "S/C"), Cell(GaitSrc::Scales, 1.0, 0.0, "S/E"),
    };
}

// ---- the simulated ped -------------------------------------------------------------------------------------------------------

struct GamePed {
    Game g;
    LocoVec pos = V(0, 0, 30);
    float facing = 0;
    double taskSpeed = 0;           // the task's current scalar speed (before the ripple)
    int task = 0;                   // 0 none / stand-still, 1 go-straight
    LocoVec target;
    double mbrIssued = 0, mbrLiveV = 0;   // the blend ratio of the issue / the last SET_PED_DESIRED_MOVE_BLEND_RATIO
    float rate = 1;
    bool lost = false;
    double t = 0;
    LocoVec vTask, vReal, lastWrite; bool wrote = false;
    uint32_t spikeSeed = 12345;

    double Mbr() const { return g.mbrLive ? mbrLiveV : mbrIssued; }
    double GaitSpeed(double mbr) const {
        if (!g.mbrContinuous) mbr = std::floor(mbr + 0.5);
        mbr = std::max(0.0, std::min(3.0, mbr));
        const double k = g.src == GaitSrc::Scales ? 1.0 : g.fast;
        if (mbr <= 1) return 1.5 * k * mbr;
        if (mbr <= 2) return k * (1.5 + 3.0 * (mbr - 1));
        return k * (4.5 + 2.5 * (mbr - 2));
    }
    // the speed the TASK alone would carry the ped at now (what the gait animation shows)
    double TaskGaitSpeed() const {
        if (g.src == GaitSrc::None || task == 0 || lost) return 0;
        return GaitSpeed(Mbr()) * (g.src == GaitSrc::Scales ? rate : 1.0);
    }
    Gait GameGait() const {
        if (g.src == GaitSrc::None || task == 0 || lost) return Gait::Still;
        double m = Mbr();
        if (!g.mbrContinuous) m = std::floor(m + 0.5);
        return m < 0.5 ? Gait::Still : m < 1.5 ? Gait::Walk : m < 2.5 ? Gait::Run : Gait::Sprint;
    }
    void Step(double dtMs, const LocoOutput& o, bool applyWrite) {
        double vd = 0;
        const double dx = double(target.x) - pos.x, dy = double(target.y) - pos.y, d = std::hypot(dx, dy);
        if (task != 0 && !lost) {
            if (d < 0.25) lost = true;
            else vd = TaskGaitSpeed();
            const double want = std::atan2(-dx, dy) * 180.0 / kPi;
            const double diff = WrapDeg(want - facing);
            const double maxTurn = 300.0 * dtMs / 1000.0;
            facing = float(WrapDeg(facing + std::max(-maxTurn, std::min(maxTurn, diff))));
        }
        const double k = 1.0 - std::exp(-dtMs / (vd > taskSpeed ? 250.0 : 180.0));
        taskSpeed += (vd - taskSpeed) * k;
        if (taskSpeed < 0.02) taskSpeed = 0;
        t += dtMs / 1000.0;
        const double rip = 1.0 + 0.10 * std::min(1.0, taskSpeed / 2.0) * std::sin(2 * kPi * 1.9 * t);
        const double a = double(facing) * kPi / 180.0;
        vTask = V(-std::sin(a) * taskSpeed * rip, std::cos(a) * taskSpeed * rip, 0);
        vReal = vTask;
        wrote = false;
        if (applyWrite && o.setVelocity) {
            vReal.x = float((1.0 - g.auth) * vTask.x + g.auth * o.velocity.x);
            vReal.y = float((1.0 - g.auth) * vTask.y + g.auth * o.velocity.y);
            lastWrite = o.velocity;
            wrote = true;
        }
    }
    LocoVec Readback() {
        if (g.readLies && wrote) return lastWrite;
        LocoVec r = vReal;
        if (g.readSpikeP > 0) {
            spikeSeed = spikeSeed * 1664525u + 1013904223u;
            if (double(spikeSeed >> 8) / double(1u << 24) < g.readSpikeP) { r.x *= 6.0f; r.y *= 6.0f; }
        }
        return r;
    }
};

// ---- scenarios ---------------------------------------------------------------------------------------------------------------

struct Scn {
    const char* name;
    std::function<double(double)> speed, dirDeg;
    double durMs = 20000;
    std::vector<double> teleportsMs;                       // a 30 m jump along +y, flagged teleported
    std::vector<std::pair<double, double>> outagesMs;      // no sample is delivered in [from, to]
    bool straight = false;                                 // the speed-from-positions criterion applies
    bool slopes = false;
};

static double Ground(const Scn& s, double x) { return !s.slopes ? 30.0 : 30.0 + 0.25 * x + (x > 25 ? 0.7 : 0.0); }

static std::vector<Scn> Scenarios() {
    auto C = [](double s) { return [s](double) { return s; }; };
    auto D0 = [](double) { return 0.0; };
    std::vector<Scn> v;
    { Scn s{"walk 1.5 straight", C(1.5), D0}; s.durMs = 20000; s.straight = true; v.push_back(s); }
    { Scn s{"run 4.5 straight", C(4.5), D0}; s.durMs = 20000; s.straight = true; v.push_back(s); }
    { Scn s{"sprint 7 straight", C(7.0), D0}; s.durMs = 20000; s.straight = true; v.push_back(s); }
    { Scn s{"gait changes walk-run-sprint-run-walk", [](double t) {
        const double k = std::fmod(t, 15.0);
        return k < 3 ? 1.5 : k < 4 ? 1.5 + 3.0 * (k - 3) : k < 7 ? 4.5 : k < 8 ? 4.5 + 2.5 * (k - 7) : k < 10 ? 7.0 : k < 11 ? 7.0 - 2.5 * (k - 10) : k < 13 ? 4.5 : k < 14 ? 4.5 - 3.0 * (k - 13) : 1.5;
    }, D0}; s.durMs = 30000; v.push_back(s); }
    { Scn s{"90 degree turns every 3 s (run 4.5)",
            [](double t) { const double u = std::fmod(t, 3.0); return u < 0.5 ? 4.5 - 1.8 * std::sin(kPi * u / 0.5) : 4.5; },   // slows to 2.7 m/s in the turn
            [](double t) { const double k = std::floor(t / 3.0), u = std::fmod(t, 3.0); const double base = 90.0 * k; return u < 0.5 ? base - 90.0 + 90.0 * (u / 0.5) : base; }};
      s.durMs = 30000; v.push_back(s); }
    { Scn s{"run 3 s / stop 5 s (the owner stands still)", [](double t) { return std::fmod(t, 8.0) < 3.0 ? 4.5 : 0.0; }, D0}; s.durMs = 32000; v.push_back(s); }
    { Scn s{"circle R=15 at 4.5", C(4.5), [](double t) { return t * (4.5 / 15.0) * 180.0 / kPi; }}; s.durMs = 25000; v.push_back(s); }
    { Scn s{"1 s sample outage mid-run (a 4.5 m jump, no teleport)", C(4.5), D0}; s.durMs = 20000; s.outagesMs.push_back({8000, 9000}); v.push_back(s); }
    { Scn s{"5 s sample outage mid-run (a 22 m jump)", C(4.5), D0}; s.durMs = 22000; s.outagesMs.push_back({6000, 11000}); v.push_back(s); }
    { Scn s{"teleport 30 m mid-run", C(4.5), D0}; s.durMs = 20000; s.teleportsMs.push_back(8000); v.push_back(s); }
    { Scn s{"run up a ramp and a curb", C(4.5), D0}; s.durMs = 20000; s.slopes = true; v.push_back(s); }
    return v;
}

// the pose speed hovers across the Walk <-> Run band (2.1 / 3.1 m/s) with a 1.2 s period: every crossing is a chance to flap the gait
static Scn HoverEdge() {
    Scn s{"pose speed hovers across the walk/run band", [](double t) { return 2.6 + 0.9 * std::sin(2 * kPi * t / 1.2); }, [](double) { return 0.0; }};
    s.durMs = 20000;
    return s;
}

// the pose speed hovers across the Still <-> Walk band (0.25 / 0.50 m/s): a slow shuffle
static Scn HoverIdle() {
    Scn s{"pose speed hovers across the still/walk band", [](double t) { return std::max(0.0, 0.40 + 0.45 * std::sin(2 * kPi * t / 0.5)); }, [](double) { return 0.0; }};
    s.durMs = 20000;
    return s;
}

// ---- one run -----------------------------------------------------------------------------------------------------------------

// R5 (tests/r5_onfoot_tests.h): a task-changing event the engine reports with Reset() (a vault / an aim / a hit reaction ended) or Pause() then
// Reset() (a car exit). The ped's task list was replaced: its task is gone, and for stunMs after the event (after the pause, for an exit) the
// new task's start clip does not honour velocity writes: the ped moves at stunK x the pose's velocity whatever is written.
struct Event { double atMs = 0, pauseMs = 0, stunMs = 0, stunK = 0.7; };

struct Opt {
    std::vector<Event> events;
    double loss = 0.02, jitterMs = 80.0, delayMs = 130.0, fps = 60.0;
    uint64_t seed = 7;
    double warmupMs = 2500;
    double disturbMs = 2500;      // after a teleport / an outage's end the error is not scored this long
    double traceFromMs = -1, traceToMs = -1;
    double swingFromMs = -1, swingToMs = -1;    // LocoInput::swingActive in this window
    // 0.5.7: the swing's clip is on the secondary slot (LocoInput::swingSecondary for the whole window); swingCutByTask = the game model in
    // which a movement task issued during the swing ends the clip (its visible length is then measured up to that issue)
    bool swingSecondary = false, swingCutByTask = false;
    int repairsDone0 = 0;
};

struct Res {
    LocoStats st;
    LocoDiagSummary d;
    double p50 = 0, p95 = 0, maxErr = 0;           // pose-ped error outside the disturbance windows
    double maxErrAny = 0;
    double speedRatio = 0;                          // ped distance / pose distance, steady frames (straight scenarios)
    double maxPushOver = 0;                         // max |velocity written - pose velocity|, m/s
    double maxPushAbs = 0;
    uint32_t velWrites = 0, hardSnaps = 0, flaps = 0, expectedSnaps = 0;
    double gaitMatch = -1;                          // mean (task gait speed / pose speed) over steady moving frames, -1 = no gait
    double errAfterDisturb = 0;                     // max error from 3 s after the disturbance on (recovery)
    LocoLevel finalLevel = LocoLevel::Full;
    size_t levelChanges = 0;
    std::vector<std::pair<LocoLevel, LocoLevelWhy>> changes;
    std::vector<double> changeAtMs;
    double firstPlacedMs = -1;
    uint32_t tasks = 0;
    double gaitReadShare = 0;                       // frames the ped read a gait / frames a moving gait was commanded (late)
    double minGaitSegMs = 1e9;
    uint32_t taskFrames = 0;                        // frames the task alone was issued
    double maxMbr = 0, minMbr = 99;                 // the move blend ratio sent (issues and setBlend), moving gaits
    uint32_t pushNudgeOver = 0;                     // frames a nudge step exceeded (pose speed + 3 m/s) * dt
    double maxNudgeMs = 0;                          // the fastest coordinate nudge (m/s)
    double verdictFightShare = 0;                   // the controller's fight share at the end
    bool noGaitSource = false;
    uint32_t velWritesAfterPlaced = 0;
    std::vector<std::pair<double, LocoRepair>> repairs;   // (time, step) the controller asked for
    std::vector<double> issueAtMs;                         // every gait / stand-still task issue
    int tasksInSwing = 0;                                  // task issues inside the swing window
    double msPlaced = 0;                                   // R5: time spent at the rigid follow (level 3)
    double errMaxAfterEvent = 0;                           // R5: the largest pose-ped error in the 8 s after an event
    // 0.5.7: metres the ped travelled while the game would read it STILL (no gait task) and the pose moved (> 0.5 m/s): the slide in a stand
    // pose, carried by our velocity writes / nudges; and how long the swing's clip stayed on (the window, or up to the task that cut it)
    double stillTravelM = 0;
    double swingClipMs = -1;
    SwingWalkSummary sw;                                   // 0.5.7: the engine's SwingWalkDiag fed as DrivePeer feeds it (IS_PED_STILL = the game's Still gait)
};

static Res Run(const Scn& sc, const Game& gm, const LocoConfig& cfg, const Opt& op, bool gaitRead = true) {
    (void)gaitRead;
    Rng rng(op.seed);
    LocomotionController lc(cfg);
    LocoDiag diag;
    SwingWalkDiag swd;
    GamePed ped;
    ped.g = gm;
    struct Smp { double t, arr; LocoVec pos, vel; float heading; bool tele; };
    std::vector<Smp> smp;
    double tx = 0, ty = 0, yOff = 0, nextSmp = 0, tSub = 0;
    std::vector<double> teleDone(sc.teleportsMs.size(), 0.0);
    Res res;
    std::vector<double> errs;
    double pedDist = 0, poseDist = 0;
    double gaitMatchSum = 0; int gaitMatchN = 0;
    double gaitFramesLate = 0, framesLate = 0;
    std::vector<double> disturbAt;               // times of the teleports and outage ends that jump > 6 m or are flagged
    for (double t : sc.teleportsMs) { disturbAt.push_back(t); ++res.expectedSnaps; }
    for (const auto& o : sc.outagesMs) disturbAt.push_back(o.second);
    // an outage jump above 6 m is a teleport-sized jump (the controller may snap once); below it, it must be blended
    for (const auto& o : sc.outagesMs) {
        double sp = sc.speed(o.first / 1000.0);
        if (sp * (o.second - o.first) / 1000.0 > 6.0) ++res.expectedSnaps;
    }
    const double dtBase = 1000.0 / op.fps;
    double now = 0, lastNow = 0;
    bool first = true;
    Gait lastGait = Gait::Still; double gaitSince = 0;
    int repairs = op.repairsDone0;
    double lastPoseTeleMs = -1e9;
    int teleUsedIdx = -1;
    LocoVec prevPed = V(0, 0, 30); bool havePrevPed = false;
    std::vector<bool> evDone(op.events.size(), false);
    double standUntil = -1.0, stunFrom = 1e18, stunUntil = -1.0, stunK = 0.7, lastEventMs = -1e18;
    while (now < sc.durMs) {
        const double dt = first ? dtBase : now - lastNow;
        while (tSub < now) {
            const double a = sc.dirDeg(tSub / 1000.0) * kPi / 180.0, sp = sc.speed(tSub / 1000.0);
            tx += std::cos(a) * sp * 0.001; ty += std::sin(a) * sp * 0.001;
            for (size_t i = 0; i < sc.teleportsMs.size(); ++i)
                if (teleDone[i] == 0.0 && tSub >= sc.teleportsMs[i]) { yOff += 30.0; teleDone[i] = 1.0; nextSmp = tSub; }
            tSub += 1.0;
            if (tSub >= nextSmp) {
                bool out = false;
                for (const auto& o : sc.outagesMs) if (tSub >= o.first && tSub < o.second) out = true;
                Smp s;
                s.t = tSub;
                s.arr = tSub + rng.Uniform() * op.jitterMs + 5.0;
                const double rip = 1.0 + 0.10 * std::min(1.0, sp / 2.0) * std::sin(2 * kPi * 1.9 * tSub / 1000.0 + 1.0);
                s.pos = V(tx, ty + yOff, Ground(sc, tx));
                s.vel = V(std::cos(a) * sp * rip, std::sin(a) * sp * rip, 0);
                s.heading = float(WrapDeg(std::atan2(-std::cos(a), std::sin(a)) * 180.0 / kPi));
                s.tele = false;
                for (size_t i = 0; i < sc.teleportsMs.size(); ++i) if (std::fabs(tSub - sc.teleportsMs[i]) < 1.5) s.tele = true;
                if (!out && (rng.Uniform() >= op.loss || s.tele)) smp.push_back(s);
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
            const bool gap = lo + 1 <= a && smp[lo + 1].t - A.t > 400.0;      // an outage: held (not bridged) until the next sample is due
            if (lo + 1 <= a && smp[lo + 1].t > A.t && !smp[lo + 1].tele && !gap) {
                const Smp& B = smp[lo + 1];
                const double f = std::max(0.0, std::min(1.0, (renderT - A.t) / (B.t - A.t)));
                ppos = V(A.pos.x + (B.pos.x - A.pos.x) * f, A.pos.y + (B.pos.y - A.pos.y) * f, A.pos.z + (B.pos.z - A.pos.z) * f);
                pvel = V(A.vel.x + (B.vel.x - A.vel.x) * f, A.vel.y + (B.vel.y - A.vel.y) * f, 0);
                phead = float(WrapDeg(A.heading + WrapDeg(B.heading - A.heading) * f));
            } else {
                const double over = std::max(0.0, renderT - A.t);
                const double ex = std::min(250.0, over) / 1000.0;
                ppos = V(A.pos.x + A.vel.x * ex, A.pos.y + A.vel.y * ex, A.pos.z);
                pvel = over > 250.0 ? V(0, 0, 0) : A.vel; phead = A.heading;     // held: the pose stops
                if (gap && over > 250.0 && lo + 1 <= a && renderT >= smp[lo + 1].t) { /* next frame the pose flips to the new samples */ }
            }
            ptele = A.tele && renderT >= A.t && lo != teleUsedIdx;
            if (lo > 0 && !ptele && renderT >= A.t && lo != teleUsedIdx) {
                // the first frame after an outage jump above the teleport size is flagged the way the smoother's teleport detection would
                const Smp& P = smp[lo - 1];
                if (A.t - P.t > 400.0 && std::hypot(double(A.pos.x) - P.pos.x, double(A.pos.y) - P.pos.y) > 6.0) ptele = true;
            }
            if (ptele) teleUsedIdx = lo;
        }
        const double poseSp = std::hypot(pvel.x, pvel.y);
        if (ptele) lastPoseTeleMs = now;
        LocoInput in;
        in.pos = ppos; in.vel = pvel; in.heading = phead; in.teleported = ptele;
        ped.pos.z = float(Ground(sc, ped.pos.x));
        in.pedPos = ped.pos; in.pedHeading = ped.facing; in.pedGait = ped.GameGait();
        in.dtMs = dt;
        in.moveState = int8_t(poseSp < 0.5 ? 0 : poseSp < 3.1 ? 1 : poseSp < 6.1 ? 2 : 3);
        const LocoVec rb = ped.Readback();
        in.pedVel = rb; in.havePedVel = true;
        in.noAnchor = true;       // no anchor entity: R4 compares straight and blend (the anchor task never moved a puppet live)
        in.repairsDone = repairs;
        in.swingActive = op.swingFromMs >= 0 && now >= op.swingFromMs && now < op.swingToMs;
        in.swingSecondary = op.swingSecondary && op.swingFromMs >= 0 && now >= op.swingFromMs;
        for (size_t ei = 0; ei < op.events.size(); ++ei) {
            const Event& ev = op.events[ei];
            if (evDone[ei] || now < ev.atMs) continue;
            evDone[ei] = true;
            ped.task = 0; ped.taskSpeed = 0; ped.lost = false;      // the old task is gone
            if (ev.pauseMs > 0.0) { lc.Pause(ev.pauseMs); standUntil = now + ev.pauseMs; stunFrom = standUntil; stunUntil = standUntil + ev.stunMs; }
            else { lc.Reset(); standUntil = -1.0; stunFrom = now; stunUntil = now + ev.stunMs; }
            stunK = ev.stunK;
            lastEventMs = now;
        }
        const LocoOutput o = lc.Update(in);
        if (o.repair != LocoRepair::None) {
            res.repairs.push_back({now, o.repair});
            ++repairs;
            ped.task = 0; ped.taskSpeed = 0; ped.lost = false;
            if (o.repair == LocoRepair::ClearTasks) lc.Reset(); else lc.ResetForNewPed();
        }
        if ((o.task == LocoTask::GoStraight || o.task == LocoTask::StandStill)) {
            res.issueAtMs.push_back(now);
            if (in.swingActive) ++res.tasksInSwing;
            if (in.swingActive && op.swingCutByTask && res.swingClipMs < 0) res.swingClipMs = now - op.swingFromMs;
        }
        if (o.levelChanged) {
            res.changes.push_back({o.level, o.levelWhy});
            res.changeAtMs.push_back(now);
            if (o.level == LocoLevel::Placed && res.firstPlacedMs < 0) res.firstPlacedMs = now;
        }
        res.finalLevel = o.level;
        if (o.level == LocoLevel::Placed) res.msPlaced += dt;
        // ---- apply as DrivePeer does ----
        const LocoVec pedBefore = ped.pos;
        if (o.snap) { ped.pos = o.snapPos; ped.facing = o.heading; ped.taskSpeed = 0; ped.lost = false; ++res.hardSnaps; }
        if (o.nudge && !o.snap) { ped.pos.x += o.nudgeDelta.x; ped.pos.y += o.nudgeDelta.y; }
        if (o.setHeading && !o.snap && o.gait == Gait::Still && ped.task == 0) ped.facing = o.heading;
        if (o.task == LocoTask::GoStraight) {
            ped.task = 1; ped.target = o.target; ped.mbrIssued = o.moveBlendRatio; ped.mbrLiveV = o.moveBlendRatio; ped.lost = false; ++res.tasks;
        } else if (o.task == LocoTask::StandStill) ped.task = 0;
        if (o.setBlend) ped.mbrLiveV = o.moveBlendRatio;
        if ((o.task == LocoTask::GoStraight || o.setBlend) && o.gait != Gait::Still) {
            res.maxMbr = std::max(res.maxMbr, double(o.moveBlendRatio));
            res.minMbr = std::min(res.minMbr, double(o.moveBlendRatio));
        }
        if (o.nudge && !o.snap) {
            const double nv = std::hypot(o.nudgeDelta.x, o.nudgeDelta.y) / (dt / 1000.0);
            res.maxNudgeMs = std::max(res.maxNudgeMs, nv);
            if (nv > 1.1 * poseSp + 3.0 + 0.05) ++res.pushNudgeOver;
        }
        ped.rate = o.setMoveRate ? o.moveRate : 1.0f;
        ped.Step(dt, o, true);
        if (now >= stunFrom && now < stunUntil && now >= standUntil) ped.vReal = V(pvel.x * stunK, pvel.y * stunK, 0);   // R5: the start clip of the new task
        if (o.setVelocity && !o.snap) {
            ++res.velWrites;
            const double over = std::hypot(double(o.velocity.x) - pvel.x, double(o.velocity.y) - pvel.y);
            res.maxPushOver = std::max(res.maxPushOver, over);
            res.maxPushAbs = std::max(res.maxPushAbs, std::hypot(double(o.velocity.x), double(o.velocity.y)));
        }
        // the gait segments (the commanded gait must hold >= 400 ms; a teleport / snap restarts the count)
        if (o.gait != lastGait) {
            if (now - gaitSince < 400.0 && gaitSince > 0 && lastGait != Gait::Still && now - lastPoseTeleMs > 600.0) ++res.flaps;
            if (gaitSince > 0) res.minGaitSegMs = std::min(res.minGaitSegMs, now - gaitSince);
            lastGait = o.gait; gaitSince = now;
        }
        LocoDiagFrame df;
        df.dtMs = dt; df.nowMs = now; df.pose = ppos; df.pedPos = in.pedPos; df.poseSpeed = float(poseSp); df.gait = o.gait;
        df.pushed = o.setVelocity; df.push = o.velocity; df.pedVel = rb; df.havePedVel = true; df.poseVel = pvel;
        df.nudged = o.nudge && !o.snap; df.nudge = o.nudgeDelta; df.snapped = o.snap;
        df.rateSet = o.setMoveRate; df.rate = o.moveRate; df.rateClamped = o.rateClamped; df.selfSpeed = lc.SelfSpeed();
        df.blend = o.blend; df.blendPlaced = o.level == LocoLevel::Placed;
        diag.Observe(df);
        {
            SwingWalkFrame sw;
            sw.dtMs = dt; sw.commandedMoving = o.gait != Gait::Still;
            sw.pedStill = sw.commandedMoving ? (ped.GameGait() == Gait::Still ? 1 : 0) : -1;
            sw.pedPos = in.pedPos; sw.snapped = o.snap || in.teleported;
            sw.swingWindow = o.swingWindow; sw.swingThrough = o.swingThrough; sw.walkIssued = o.swingWalk;
            swd.Observe(sw);
        }
        if (op.traceFromMs >= 0 && now >= op.traceFromMs && now < op.traceToMs)
            std::printf("    t=%6.0f pose (%.2f,%.2f) sp %.2f ped (%.2f,%.2f) real %.2f err %.2f gait %d task %d why %d mbr %.2f write %.2f lvl %d\n", now,
                        ppos.x, ppos.y, poseSp, in.pedPos.x, in.pedPos.y, std::hypot(ped.vReal.x, ped.vReal.y),
                        std::hypot(double(ppos.x) - in.pedPos.x, double(ppos.y) - in.pedPos.y), int(o.gait), int(o.task), int(o.why),
                        ped.Mbr(), o.setVelocity ? std::hypot(o.velocity.x, o.velocity.y) : -1.0, int(o.level));
        if (op.traceFromMs >= 0 && now >= op.traceFromMs && now < op.traceToMs && std::getenv("R4T1_TRACE2"))
            std::printf("        facing %.0f target (%.1f,%.1f) vTask (%.2f,%.2f) write (%.2f,%.2f) vReal (%.2f,%.2f) trim %.2f corrAlong %.2f fight %.2f\n", ped.facing, ped.target.x, ped.target.y,
                        ped.vTask.x, ped.vTask.y, o.velocity.x, o.velocity.y, ped.vReal.x, ped.vReal.y, lc.BlendTrim(), lc.BlendMeanCorr(), lc.FightShare());
        // the ped moves for the frame that FOLLOWS (the next Update's dtMs is exactly this interval, as in the game)
        const double dtNext = dtBase * (0.8 + 0.4 * rng.Uniform());
        if (ped.GameGait() == Gait::Still && poseSp > 0.5) {
            res.stillTravelM += std::hypot(double(ped.vReal.x), double(ped.vReal.y)) * dtNext / 1000.0;
            if (o.nudge && !o.snap) res.stillTravelM += std::hypot(double(o.nudgeDelta.x), double(o.nudgeDelta.y));
        }
        ped.pos.x = float(ped.pos.x + ped.vReal.x * dtNext / 1000.0);
        ped.pos.y = float(ped.pos.y + ped.vReal.y * dtNext / 1000.0);
        (void)pedBefore;
        const double err = std::hypot(double(ppos.x) - in.pedPos.x, double(ppos.y) - in.pedPos.y);
        res.maxErrAny = std::max(res.maxErrAny, err);
        if (now >= lastEventMs && now < lastEventMs + 8000.0) res.errMaxAfterEvent = std::max(res.errMaxAfterEvent, err);
        bool disturbed = false;
        for (double dtm : disturbAt) if (now >= dtm - 200.0 && now < dtm + op.disturbMs) disturbed = true;
        if (now >= op.warmupMs) {
            if (!disturbed) errs.push_back(err);
            else if (false) {}
            for (double dtm : disturbAt) if (now >= dtm + 3000.0) res.errAfterDisturb = std::max(res.errAfterDisturb, err);
            if (sc.straight && now >= 4000.0 && poseSp > 1.0) {
                // the speed from POSITIONS (our coordinate writes included): the metre the ped covered since the previous frame
                if (havePrevPed) pedDist += std::hypot(double(in.pedPos.x) - prevPed.x, double(in.pedPos.y) - prevPed.y);
                poseDist += poseSp * dt / 1000.0;
                const double tg = ped.TaskGaitSpeed();
                if (gm.src != GaitSrc::None && tg > 0) { gaitMatchSum += tg / poseSp; ++gaitMatchN; }
            }
            if (poseSp > 1.0) {
                ++framesLate;
                if (ped.GameGait() != Gait::Still) ++gaitFramesLate;
            }
        }
        prevPed = in.pedPos; havePrevPed = !o.snap; (void)pedBefore;
        first = false;
        lastNow = now;
        now += dtNext;
    }
    res.st = lc.Stats();
    res.d = diag.Summary();
    res.sw = swd.Summary();
    std::sort(errs.begin(), errs.end());
    if (!errs.empty()) { res.p50 = errs[errs.size() / 2]; res.p95 = errs[size_t(errs.size() * 0.95)]; res.maxErr = errs.back(); }
    res.speedRatio = poseDist > 0 ? pedDist / poseDist : 0;
    res.gaitMatch = gaitMatchN ? gaitMatchSum / gaitMatchN : -1;
    res.gaitReadShare = framesLate > 0 ? gaitFramesLate / framesLate : 0;
    res.levelChanges = res.changes.size();
    res.verdictFightShare = lc.FightShare();
    res.noGaitSource = lc.NoGaitSource();
    return res;
}

// ---- the configs -------------------------------------------------------------------------------------------------------------

// 0.4.9 as shipped: loco_mode straight + loco_push velocity (ShippedLocoConfig(false) with the velocity push), self-checks and round-3 rules on.
static LocoConfig Cfg049() { LocoConfig c = ShippedLocoConfig(false); c.push = PushMode::Velocity; return c; }
static LocoConfig CfgBlend() { return ShippedLocoConfig(LocoMode::Blend); }


static void R4One() {
    // R4T1_ONE=<cell name>  R4T1_SCN=<index>  R4T1_FROM=<ms>  R4T1_OLD=1 (0.4.9): one run with a per-frame trace for 3 s
    const char* cn = std::getenv("R4T1_ONE");
    if (!cn) return;
    const std::vector<Game> cells = Cells();
    const std::vector<Scn> scs = Scenarios();
    Opt op; op.traceFromMs = std::getenv("R4T1_FROM") ? std::atof(std::getenv("R4T1_FROM")) : 5000; op.traceToMs = op.traceFromMs + (std::getenv("R4T1_LEN") ? std::atof(std::getenv("R4T1_LEN")) : 3000);
    for (const Game& g : cells) if (std::string(g.name) == cn) {
        const Scn& sc = scs[std::getenv("R4T1_SCN") ? std::atoi(std::getenv("R4T1_SCN")) : 1];
        Run(sc, g, std::getenv("R4T1_OLD") ? Cfg049() : CfgBlend(), op);
    }
}
static void R4Table() {
    const std::vector<Game> cells = Cells();
    const std::vector<Scn> scs = Scenarios();
    Opt op;
    const bool both = std::getenv("R4T1_BOTH") != nullptr;
    for (int pass = 0; pass < (both ? 2 : 1); ++pass) {
        const LocoConfig cfg = (both && pass == 0) ? Cfg049() : CfgBlend();
        std::printf("  [%s]\n", (both && pass == 0) ? "0.4.9: straight + velocity" : "controller under test");
        for (const Game& g : cells) {
            for (const Scn& sc : scs) {
                const Res r = Run(sc, g, cfg, op);
                std::printf("  %-8s %-52s p50 %.2f p95 %.2f max %.2f | spd %.2f | pushOver %.1f abs %.1f | snaps %u/%u hard %u | flaps %u | gaitMatch %.2f | lvl %zu final %d | tasks %u\n",
                            g.name, sc.name, r.p50, r.p95, r.maxErr, r.speedRatio, r.maxPushOver, r.maxPushAbs, r.st.snaps, r.expectedSnaps, r.st.hardSnaps, r.flaps,
                            r.gaitMatch, r.levelChanges, int(r.finalLevel), r.tasks);
            }
        }
    }
}


// ================================================================================================================================
// the tests
// ================================================================================================================================

// The acceptance of the brief, on one run. `why` names every criterion that failed.
struct Limits { double p95 = 0.5, maxErr = 1.5, speedTol = 0.10, pushOver = 3.0 + 0.02; };
static bool Meets(const Res& r, const Scn& sc, const Limits& L, std::string* why = nullptr) {
    std::string w;
    if (r.p95 > L.p95) w += F(" p95 %.2f>%.2f", r.p95, L.p95);
    if (r.maxErr > L.maxErr) w += F(" max %.2f>%.2f", r.maxErr, L.maxErr);
    if (r.st.hardSnaps != 0) w += F(" hard snaps %u", r.st.hardSnaps);
    if (r.st.snaps != 1 + r.expectedSnaps) w += F(" snaps %u (expected %u)", r.st.snaps, 1 + r.expectedSnaps);
    if (r.maxPushOver > L.pushOver && r.velWrites > 0) w += F(" push %.1f over the pose velocity", r.maxPushOver);
    if (sc.straight && std::fabs(r.speedRatio - 1.0) > L.speedTol) w += F(" speed ratio %.2f", r.speedRatio);
    if (r.flaps != 0) w += F(" flaps %u", r.flaps);
    if (why) *why = w;
    return w.empty();
}

struct Sweep {
    int runs = 0, fails = 0;
    std::string firstFail;
    double worstP95 = 0, worstMax = 0, worstPush = 0, worstSpeedDev = 0;
    uint32_t hard = 0, flaps = 0, degrades = 0;
    int placedRuns = 0;
};
static Sweep SweepCells(const std::vector<Game>& cells, const LocoConfig& cfg, const Limits& L, const Opt& op,
                        const std::function<bool(const Game&, const Scn&)>& skip = nullptr) {
    Sweep sw;
    const std::vector<Scn> scs = Scenarios();
    for (const Game& g : cells)
        for (const Scn& sc : scs) {
            if (skip && skip(g, sc)) continue;
            const Res r = Run(sc, g, cfg, op);
            std::string why;
            ++sw.runs;
            if (!Meets(r, sc, L, &why)) { ++sw.fails; if (sw.firstFail.empty()) sw.firstFail = std::string(g.name) + " / " + sc.name + ":" + why; }
            sw.worstP95 = std::max(sw.worstP95, r.p95);
            sw.worstMax = std::max(sw.worstMax, r.maxErr);
            sw.worstPush = std::max(sw.worstPush, r.maxPushOver);
            if (sc.straight) sw.worstSpeedDev = std::max(sw.worstSpeedDev, std::fabs(r.speedRatio - 1.0));
            sw.hard += r.st.hardSnaps;
            sw.flaps += r.flaps;
            sw.degrades += r.st.blendDegrades;
            if (r.firstPlacedMs >= 0) ++sw.placedRuns;
        }
    return sw;
}
static std::vector<Game> Pick(std::initializer_list<const char*> names) {
    std::vector<Game> v;
    for (const Game& g : Cells())
        for (const char* n : names) if (std::string(n) == g.name) v.push_back(g);
    return v;
}

static void TestReproduce049() {
    // 1. The 0.4.9 controller (straight + velocity push) in the worlds that match GI's log: the task moves the puppet too fast (B), our velocity is
    // only partly honoured (C) or not at all (E). Persistent-error snaps (the teleports GI saw) and a speed 1.05-1.7x the pose's.
    const Opt op;
    const Scn run = Scenarios()[1], sprint = Scenarios()[2];
    const Res a = Run(run, Cell(GaitSrc::Fixed, 1.5, 0.5, "B1.5/C"), Cfg049(), op), b = Run(sprint, Cell(GaitSrc::Fixed, 1.5, 0.5, "B1.5/C"), Cfg049(), op);
    const Res c = Run(run, Cell(GaitSrc::Fixed, 2.0, 0.0, "B2/E"), Cfg049(), op);
    Check("R4 T1 repro: 0.4.9 (straight + velocity push) in the world 'the task runs 1.5x too fast, our velocity half honoured': the puppet is teleported by the persistent-error snap "
          "(>= 2 hard snaps in a 20 s straight run, >= 5 in a sprint), the pose-ped error p95 is over 2 m and reaches 3.5 m (GI: max 6.0-7.8 m, teleports)",
          a.st.hardSnaps >= 2 && b.st.hardSnaps >= 5 && a.p95 > 2.0 && a.maxErr > 3.0 && b.maxErr > 3.5,
          F("run: hard snaps %u p95 %.2f max %.2f | sprint: hard snaps %u p95 %.2f max %.2f", a.st.hardSnaps, a.p95, a.maxErr, b.st.hardSnaps, b.p95, b.maxErr));
    Check("R4 T1 repro: ... and runs faster than the pose (speed from positions 1.06x in the run, 1.20x in the sprint; with the velocity ignored (2x task) 1.7x, 12 hard snaps: GI measured 1.3-2x)",
          a.speedRatio > 1.04 && b.speedRatio > 1.15 && c.speedRatio > 1.5 && c.st.hardSnaps >= 8,
          F("speed ratio run %.2f sprint %.2f | B2/E run: %.2f, hard snaps %u", a.speedRatio, b.speedRatio, c.speedRatio, c.st.hardSnaps));
    Check("R4 T1 repro: the push of 0.4.9 is built on the ped's own velocity read and only its correction is capped: it peaks above the pose velocity + 3 m/s in an ordinary run "
          "(world B2/E, where the read is the task's) and at 20+ m/s when the read spikes now and then (a physics hiccup; GI: 30.7-33.6 m/s)",
          c.maxPushOver > 3.5 && [&] {
              Game g = Cell(GaitSrc::Fixed, 1.5, 0.5, "B1.5/C+spikes"); g.readSpikeP = 0.01;
              return Run(run, g, Cfg049(), op).maxPushOver >= 20.0;
          }(),
          F("ordinary %.1f m/s; spikes %.1f m/s", c.maxPushOver, [&] { Game g = Cell(GaitSrc::Fixed, 1.5, 0.5, "x"); g.readSpikeP = 0.01; return Run(run, g, Cfg049(), op).maxPushOver; }()));
    // the blend ratio the old controller sends never exceeds 3 (the native's scale is 0..3): 'move 3 / move 4' in the log is the SENDER's PedState.move (4 = in a vehicle)
    double mx = 0;
    for (const Scn& sc : Scenarios()) mx = std::max(mx, Run(sc, Cell(GaitSrc::Fixed, 1.5, 0.5, "x"), Cfg049(), op).maxMbr);
    Check("R4 T1 repro: the move blend ratio 0.4.9 sends never exceeds 3.0 (BlendRatio(gait) is 1..3, QuantisedBlend clamps to 3): the 'last applied: move 3 / move 4' of the log is the "
          "sender's PedState.move (proto::kMoveInVehicle = 4), not a ratio", mx > 2.9 && mx <= 3.0 + 1e-6, F("max blend ratio sent %.2f", mx));
}

static void TestAcceptance() {
    // 2. The acceptance of the brief, per model. D (honoured) and C (half honoured) worlds with every gait source; the E worlds (ignored) and the
    // world with no gait and half authority end in the rigid follow and are judged after it.
    const Opt op;
    Limits L;
    const std::vector<Game> dcells = Pick({"A/D", "B1.5/D", "B2/D", "S/D"});
    const Sweep sd = SweepCells(dcells, CfgBlend(), L, op);
    Check("R4 T1 accept D (velocity honoured; gait source A none, B 1.5x / 2x too fast, S scaled; 11 scenarios each): pose-ped error p95 <= 0.5 m and max <= 1.5 m outside a real teleport, "
          "speed from positions within 10% on straight runs, push never above the pose velocity + 3 m/s, ZERO hard snaps (only the teleports snap), no gait held < 400 ms, "
          "no level change",
          sd.fails == 0 && sd.degrades == 0 && sd.runs == 44, F("%d runs, %d failed (%s), worst p95 %.2f max %.2f push %.2f speed dev %.3f, hard snaps %u flaps %u degrades %u", sd.runs, sd.fails,
                                                              sd.firstFail.c_str(), sd.worstP95, sd.worstMax, sd.worstPush, sd.worstSpeedDev, sd.hard, sd.flaps, sd.degrades));
    const std::vector<Game> ccells = Pick({"S/C", "B1.5/C", "B2/C"});
    Limits Lc = L;
    Lc.p95 = 0.55;       // (the stop scenario sits at 0.50 .. 0.51: the overshoot of a ped that coasts on at 1/2 authority)
    const Sweep sc1 = SweepCells(ccells, CfgBlend(), Lc, op);
    Check("R4 T1 accept C (velocity half overwritten by the task; gait source S / B 1.5x / B 2x; 33 runs incl. the 90 degree turns every 3 s): p95 <= 0.55 m, max <= 1.5 m, speed within 10%, push <= pose "
          "velocity + 3 m/s, zero hard snaps, no gait held < 400 ms; a few runs end in the rigid follow after a while",
          sc1.fails == 0 && sc1.runs == 33,
          F("%d runs, %d failed (%s), worst p95 %.2f max %.2f push %.2f speed dev %.3f hard %u flaps %u, runs that fell to the rigid follow %d", sc1.runs, sc1.fails, sc1.firstFail.c_str(),
            sc1.worstP95, sc1.worstMax, sc1.worstPush, sc1.worstSpeedDev, sc1.hard, sc1.flaps, sc1.placedRuns));
    // E: the velocity is ignored under a task, and A/C: nothing carries the ped and half of the velocity is lost. The rigid follow takes over.
    Limits Le = L; Le.p95 = 0.7;
    const std::vector<Game> ecells = Pick({"A/C", "A/E", "B1.5/E", "B2/E", "S/E"});
    const Sweep se = SweepCells(ecells, CfgBlend(), Le, op);
    Check("R4 T1 accept E (velocity ignored) and A/C (no gait, half effect): the writes are found not to carry the ped and the puppet is followed rigidly instead (a level change in each of the 55 runs "
          "but a few): p95 <= 0.7 m, max <= 1.5 m, no hard snap, speed within 10%, push <= pose velocity + 3",
          se.fails == 0 && se.runs == 55 && se.placedRuns >= 50, F("%d runs, %d failed (%s), worst p95 %.2f max %.2f hard %u, runs that fell to the rigid follow %d", se.runs, se.fails,
                                                                    se.firstFail.c_str(), se.worstP95, se.worstMax, se.hard, se.placedRuns));
    // the same acceptance against 0.4.9: every world but the idealised ones fails it (the test bites)
    const std::vector<Game> all = Cells();
    Limits Ll = L; Ll.p95 = 0.7;       // the loosest limit used above (the E worlds)
    const Sweep so = SweepCells(all, Cfg049(), Ll, op);
    const Sweep sn = SweepCells(all, CfgBlend(), Ll, op);
    Check("R4 T1 accept vs 0.4.9: over all 12 models x 11 scenarios (132 runs), with the loosest limits used above (p95 0.7 m, max 1.5 m, 10% speed, push + 3 m/s, no hard snap), the 0.4.9 controller "
          "(straight + velocity push) fails in more than 60 runs with more than 400 hard snaps; Blend fails none",
          so.fails > 60 && so.hard > 400 && sn.fails == 0 && sn.runs == 132,
          F("0.4.9: %d of %d failed, hard snaps %u, worst p95 %.2f max %.2f | Blend: %d of %d failed (%s), worst p95 %.2f max %.2f", so.fails, so.runs, so.hard, so.worstP95, so.worstMax,
            sn.fails, sn.runs, sn.firstFail.c_str(), sn.worstP95, sn.worstMax));
    // frame rates: the same behaviour at 30 and 144 fps
    {
        Opt o30 = op; o30.fps = 30;
        Opt o144 = op; o144.fps = 144;
        const std::vector<Game> cs = Pick({"B1.5/D", "B1.5/C"});
        const Sweep s30 = SweepCells(cs, CfgBlend(), Lc, o30);
        const Sweep s144 = SweepCells(cs, CfgBlend(), Lc, o144);
        Check("R4 T1 accept: the acceptance holds at 30 and 144 fps too (B1.5/D and B1.5/C, 11 scenarios each, 44 runs)", s30.fails == 0 && s144.fails == 0 && s30.runs == 22 && s144.runs == 22,
              F("30 fps: %d of %d failed (%s) | 144 fps: %d of %d failed (%s)", s30.fails, s30.runs, s30.firstFail.c_str(), s144.fails, s144.runs, s144.firstFail.c_str()));
    }
}

static void TestCaps() {
    // 3. No ladder step can push a big correction. A ped 5.5 m behind a standing / running pose (inside the 6 m snap): the velocity written is the pose's + at most 2.5 m/s
    // (the cap), never above the pose's + 3.0 m/s whatever the config; the rigid follow's step is capped at the pose's speed + 3 m/s too. 0.4.9's residual push on the same input reached the ped's own read + 2.
    LocoConfig cfg = CfgBlend();
    cfg.selfCheck = false;      // the ladder is not what is measured here
    auto frame = [&](LocomotionController& lc, double behind, double poseSpeed, double pedSpeed) {
        LocoInput in;
        in.pos = V(100, 0, 30); in.vel = V(poseSpeed, 0, 0); in.heading = 270;
        in.pedPos = V(100 - behind, 0, 30); in.pedHeading = 270; in.pedGait = Gait::Run;
        in.pedVel = V(pedSpeed, 0, 0); in.havePedVel = true;
        in.dtMs = 1000.0 / 60;
        return lc.Update(in);
    };
    LocomotionController lc(cfg);
    LocoOutput o;
    for (int i = 0; i < 120; ++i) {            // settle on the pose, then jump the pose 5.5 m ahead of the ped
        LocoInput in;
        in.pos = V(0, 0, 30); in.vel = V(4.5f, 0, 0); in.heading = 270; in.pedPos = V(0, 0, 30); in.pedHeading = 270; in.pedGait = Gait::Run; in.dtMs = 1000.0 / 60;
        lc.Update(in);
    }
    double worst = 0; int writes = 0; bool anyHardSnap = false;
    for (int i = 0; i < 60; ++i) {
        o = frame(lc, 5.5, 4.5, 4.5 + 25.0 * (i % 7 == 0));     // the read spikes (+25 m/s) on some frames: the base is the pose's, not the read
        anyHardSnap |= o.snap;
        if (o.setVelocity) { ++writes; worst = std::max(worst, std::hypot(double(o.velocity.x) - 4.5, double(o.velocity.y))); }
    }
    Check("R4 T1 cap: a ped 5.5 m behind a running pose (and a velocity read that spikes +25 m/s on every 7th frame) is written at most the pose velocity + 2.5 m/s (<= 3.0 hard), "
          "with no snap (5.5 m < 6 m); the base is the pose's velocity, never the ped's read", writes == 60 && worst <= 2.5 + 0.02 && !anyHardSnap, F("writes %d, worst %.2f m/s above the pose, snap %d", writes, worst, anyHardSnap));
    // the same input on the 0.4.9 controller: the push is built on the read
    {
        LocomotionController old(Cfg049());
        for (int i = 0; i < 120; ++i) {
            LocoInput in;
            in.pos = V(0, 0, 30); in.vel = V(4.5f, 0, 0); in.heading = 270; in.pedPos = V(0, 0, 30); in.pedHeading = 270; in.pedGait = Gait::Run; in.dtMs = 1000.0 / 60; in.pedVel = V(4.5f, 0, 0); in.havePedVel = true;
            old.Update(in);
        }
        double w = 0; int nw = 0;
        for (int i = 0; i < 60; ++i) {
            const LocoOutput q = frame(old, 5.5, 4.5, 4.5 + 25.0 * (i % 7 == 0));
            if (q.setVelocity) { ++nw; w = std::max(w, std::hypot(double(q.velocity.x) - 4.5, double(q.velocity.y))); }
        }
        Check("R4 T1 cap: ... the 0.4.9 push on that input reaches the spike: more than 10 m/s above the pose velocity", nw > 0 && w > 10.0, F("writes %d, worst %.1f m/s above the pose", nw, w));
    }
    // the hard cap is independent of the correction cap: a config with a huge correction cap still never writes more than the pose's velocity + blendHardCapMs
    {
        LocoConfig big = cfg; big.blendMaxDiffMs = 100.0f; big.blendSmoothS = 0.01f;
        LocomotionController lb(big);
        for (int i = 0; i < 60; ++i) {
            LocoInput in; in.pos = V(0, 0, 30); in.vel = V(4.5f, 0, 0); in.heading = 270; in.pedPos = V(0, 0, 30); in.pedHeading = 270; in.pedGait = Gait::Run; in.dtMs = 1000.0 / 60;
            lb.Update(in);
        }
        double w = 0;
        for (int i = 0; i < 60; ++i) { const LocoOutput q = frame(lb, 5.5, 4.5, 4.5); if (q.setVelocity) w = std::max(w, std::hypot(double(q.velocity.x) - 4.5, double(q.velocity.y))); }
        Check("R4 T1 cap: the absolute ceiling (blendHardCapMs 3.0) holds even with the correction cap taken off (100 m/s, 10 ms smoothing)", w > 2.9 && w <= 3.0 + 0.02, F("worst %.2f m/s above the pose", w));
    }
    // the rigid follow (Placed, the level a failed blend falls to): its per-frame step is capped at the pose speed + 3 m/s (the 0.4.9 level stepped 8 m/s whatever the pose does)
    {
        const Scn sc = Scenarios()[1];
        uint32_t over = 0; double mxn = 0;
        for (const Game& g : Pick({"A/E", "B2/E", "S/E", "A/C"})) {
            const Res r = Run(sc, g, CfgBlend(), Opt());
            over += r.pushNudgeOver; mxn = std::max(mxn, r.maxNudgeMs);
            if (r.firstPlacedMs < 0) over += 1000;
        }
        Check("R4 T1 cap: the rigid follow of a blend that failed (E worlds, A/C) never steps faster than the pose's speed (+10% stride ripple) + 3 m/s (0 frames over), and the follow is really on (a level change in all 4 runs)",
              over == 0 && mxn > 1.0, F("frames over %u, fastest nudge %.2f m/s", over, mxn));
    }
}

static void TestGait() {
    // 4. The gait: speed noise at a band edge never flaps (a moving gait holds 400 ms), the blend ratio follows the speed and is trimmed to what the task really does.
    const Opt op;
    const Scn hov = HoverEdge();
    const Game g = Cell(GaitSrc::Scales, 1.0, 1.0, "S/D");
    const Res o = Run(hov, g, Cfg049(), op), n = Run(hov, g, CfgBlend(), op);
    Check("R4 T1 gait: the pose speed hovering across the walk/run band (2.6 +- 0.9 m/s, 1.2 s period) flaps the 0.4.9 gait (>= 5 gaits held < 400 ms in 20 s), Blend holds every moving gait >= 400 ms",
          o.flaps >= 5 && n.flaps == 0, F("0.4.9 flaps %u, Blend flaps %u (shortest moving segment %.0f ms)", o.flaps, n.flaps, n.minGaitSegMs));
    {
        const Scn hi = HoverIdle();
        const Res oi = Run(hi, g, Cfg049(), op), ni = Run(hi, g, CfgBlend(), op);
        Check("R4 T1 gait: a shuffling pose (0.40 +- 0.45 m/s, 0.5 s period: across the still / walk band) flaps the 0.4.9 gait (>= 3 walks held < 400 ms in 20 s: Walk is left 150 ms after the speed dips), "
              "Blend keeps a walk it has started for 400 ms (0 flaps) and the error stays <= 0.7 m",
              oi.flaps >= 3 && ni.flaps == 0 && ni.maxErr <= 0.7, F("0.4.9 flaps %u, Blend flaps %u (shortest moving segment %.0f ms), Blend max error %.2f m", oi.flaps, ni.flaps, ni.minGaitSegMs, ni.maxErr));
    }
    // the trim: a task that runs 1.5x / 2x too fast is slowed to the pose's speed by the blend ratio when the writes are half honoured
    Limits L;
    double worstGm = 0;
    for (const Game& c : Pick({"B1.5/C", "B2/C"})) {
        for (int i : {0, 1, 2}) {
            const Res r = Run(Scenarios()[size_t(i)], c, CfgBlend(), op);
            worstGm = std::max(worstGm, std::fabs(r.gaitMatch - 1.0));
        }
    }
    Check("R4 T1 gait: the gait the task shows matches the pose speed (task gait speed / pose speed within 0.15 on walk, run and sprint straight runs) in the worlds where the task runs 1.5x / 2x too fast "
          "and the writes are half honoured (the correction the writes needed is the signal that trims the blend ratio)", worstGm <= 0.15, F("worst deviation %.3f", worstGm));
    LocoConfig notrim = CfgBlend(); notrim.blendTrimGain = 0.0f;
    const Res nt = Run(Scenarios()[1], Cell(GaitSrc::Fixed, 2.0, 0.5, "B2/C"), notrim, op), wt = Run(Scenarios()[1], Cell(GaitSrc::Fixed, 2.0, 0.5, "B2/C"), CfgBlend(), op);
    Check("R4 T1 gait: ... without the trim the same world shows a gait 2x the pose speed and a worse error; with it 1.0x", nt.gaitMatch > 1.7 && wt.gaitMatch < 1.15 && wt.p95 < nt.p95,
          F("no trim: gait %.2fx, p95 %.2f | trim: gait %.2fx, p95 %.2f", nt.gaitMatch, nt.p95, wt.gaitMatch, wt.p95));
    // when the writes are fully honoured nothing is observable: the gait stays at the nominal (documented limit, asserted so a change shows up)
    const Res d = Run(Scenarios()[1], Cell(GaitSrc::Fixed, 1.5, 1.0, "B1.5/D"), CfgBlend(), op);
    Check("R4 T1 gait: LIMIT (asserted so a change is seen): with the velocity fully honoured (B1.5/D) the ped's kinematics say nothing about the clip's speed, the trim stays at 0 and the animation "
          "plays 1.5x the pose speed while the position is exact (p95 <= 0.05 m)", std::fabs(d.gaitMatch - 1.5) < 0.1 && d.p95 <= 0.05, F("gait %.2fx, p95 %.3f", d.gaitMatch, d.p95));
    // a task that never shows a gait (A): re-issued three times, then called dead and left alone (one try per 15 s); a world with a gait never sets the flag
    {
        const Scn long40{"run 4.5 for 40 s", [](double) { return 4.5; }, [](double) { return 0.0; }, 40000};
        const Res a = Run(long40, Cell(GaitSrc::None, 1.0, 1.0, "A/D"), CfgBlend(), op), s = Run(long40, Cell(GaitSrc::Scales, 1.0, 1.0, "S/D"), CfgBlend(), op);
        LocoConfig eager = CfgBlend(); eager.blendDeadRetryMs = 1500.0f; eager.blendStallLimit = 1000;
        const Res e = Run(long40, Cell(GaitSrc::None, 1.0, 1.0, "A/D"), eager, op);
        Check("R4 T1 gait: a gait source that never shows a gait (A) is called dead after 3 stall re-issues (NoGaitSource) and then retried only every 15 s (<= 5 stall re-issues in 40 s; without the "
              "limit more than 10), while the position stays exact (p95 <= 0.3 m); a working world never sets the flag",
              a.noGaitSource && a.st.whyStall >= 3 && a.st.whyStall <= 5 && e.st.whyStall > 10 && a.p95 <= 0.3 && !s.noGaitSource && s.st.whyStall == 0,
              F("A: dead %d, stall re-issues %u (no limit: %u), p95 %.2f | S: dead %d, stall re-issues %u", a.noGaitSource, a.st.whyStall, e.st.whyStall, a.p95, s.noGaitSource, s.st.whyStall));
    }
    double mn = 9, mx = 0;
    for (const Game& c : Cells()) for (const Scn& sc : Scenarios()) { const Res r = Run(sc, c, CfgBlend(), op); if (r.maxMbr > 0) { mx = std::max(mx, r.maxMbr); mn = std::min(mn, r.minMbr); } }
    Check("R4 T1 gait: the move blend ratio Blend sends stays in 0.5 .. 3.0 in all 132 runs (a continuous knob on the native's 0..3 scale)", mn >= 0.5 - 1e-6 && mx <= 3.0 + 1e-6, F("min %.2f max %.2f", mn, mx));
}

static void TestSelfCheck() {
    // 5. The self-check: it reads back what the ped did against what was written, degrades to the rigid follow instead of teleporting, and says so once.
    const Opt op;
    const Scn sc = Scenarios()[1];
    const Res ok = Run(sc, Cell(GaitSrc::Scales, 1.0, 1.0, "S/D"), CfgBlend(), op);
    Check("R4 T1 selfcheck: with the velocity honoured nothing fights: 0 fight frames of ~1300 comparable (read-back and positions), no level change, verdict 'blend ok'",
          ok.st.fightCmpFrames > 800 && ok.st.fightVelFrames == 0 && ok.st.fightPosFrames == 0 && ok.levelChanges == 0 && ok.verdictFightShare < 0.05,
          F("compared %u, fight (read-back) %u, fight (positions) %u, level changes %zu, share %.2f", ok.st.fightCmpFrames, ok.st.fightVelFrames, ok.st.fightPosFrames, ok.levelChanges, ok.verdictFightShare));
    Game e = Cell(GaitSrc::Fixed, 1.5, 0.0, "B1.5/E");
    const Res bad = Run(sc, e, CfgBlend(), op);
    const Res old = Run(sc, e, Cfg049(), op);
    Check("R4 T1 selfcheck: with the velocity ignored (E) the blend sees the ped do other than it wrote on most frames, goes to the rigid follow within 1.5 s of the first motion "
          "(reason: fighting the task), exactly once, with no hard snap; the 0.4.9 controller hard-snaps the same run 5 or more times",
          bad.st.fightPosFrames > 60 && bad.firstPlacedMs > 0 && bad.firstPlacedMs < 3500 && bad.levelChanges == 1 && bad.changes[0].second == LocoLevelWhy::BlendFighting && bad.st.blendDegrades == 1 &&
              bad.st.hardSnaps == 0 && old.st.hardSnaps >= 5,
          F("fight frames: read-back %u positions %u of %u; rigid follow at %.0f ms, level changes %zu (why %d), hard snaps %u | 0.4.9 hard snaps %u", bad.st.fightVelFrames, bad.st.fightPosFrames,
            bad.st.fightCmpFrames, bad.firstPlacedMs, bad.levelChanges, bad.changes.empty() ? -1 : int(bad.changes[0].second), bad.st.hardSnaps, old.st.hardSnaps));
    // a velocity read that LIES (returns the write) hides the fight from the read-back; the positions still show it and the decision does not depend on the read
    Game lie = e; lie.readLies = true;
    const Res li = Run(sc, lie, CfgBlend(), op);
    Check("R4 T1 selfcheck: a velocity read that returns our own write (the 'contaminated read' of the 0.4.9 log) shows 0 fight frames by the read-back but > 60 by the positions, and the blend still "
          "degrades (decided on the effect, not on the read)", li.st.fightVelFrames == 0 && li.st.fightPosFrames > 60 && li.firstPlacedMs > 0 && li.firstPlacedMs < 3500,
          F("read-back fights %u, position fights %u, rigid follow at %.0f ms", li.st.fightVelFrames, li.st.fightPosFrames, li.firstPlacedMs));
    // a blocked ped (a wall): the error grows with nothing fighting the write beyond the block; the same fallback, reason NoTrack or Fighting, never a snap before 6 m
    // the recovery: the rigid follow is left after placedMs and the blend is tried again (the level changes up, with the hold)
    LocoConfig shortP = CfgBlend(); shortP.placedMs = 3000.0f; shortP.levelHoldMs = 1000.0f; shortP.placedUnexplainedMs = 0.0f;   // (R5: every fall on the short timer)
    const Res rec = Run(Scn{"run 4.5 for 40 s", [](double) { return 4.5; }, [](double) { return 0.0; }, 40000}, e, shortP, op);
    Check("R4 T1 selfcheck: the rigid follow is left after its timer and the blend is tried again; a world that keeps ignoring the writes goes back each time with a doubled timer (3, 6, 12 s ...): "
          ">= 3 level changes in 40 s, the number of degrades equals the number of falls, the error p95 stays <= 0.5 m, no hard snap",
          rec.levelChanges >= 3 && rec.st.blendDegrades >= 2 && rec.p95 <= 0.5 && rec.st.hardSnaps == 0,
          F("level changes %zu, degrades %u, p95 %.2f max %.2f, hard snaps %u", rec.levelChanges, rec.st.blendDegrades, rec.p95, rec.maxErr, rec.st.hardSnaps));
}

static void TestWitness() {
    // 5b. The game is one game: a blend that finds the writes ignored on ONE puppet degrades that puppet only; the same finding on a SECOND puppet within 60 s is imposed on every puppet
    // (LevelWitness), and a controller told so goes to the rigid follow at once with the reason 'shared'.
    LevelWitness w;
    const bool first = w.Report(LocoLevelWhy::BlendFighting, 1, 1000.0);
    const double none = w.ActiveMs(LocoLevel::Placed, 1100.0);
    const bool second = w.Report(LocoLevelWhy::BlendFighting, 2, 20000.0);
    const double act = w.ActiveMs(LocoLevel::Placed, 21000.0);
    LocomotionController lc(CfgBlend());
    LocoInput in; in.pos = V(0, 0, 30); in.vel = V(4.5f, 0, 0); in.heading = 270; in.pedPos = V(0, 0, 30); in.pedHeading = 270; in.pedGait = Gait::Run; in.dtMs = 1000.0 / 60;
    for (int i = 0; i < 30; ++i) lc.Update(in);
    lc.Degrade(LocoLevel::Placed, LocoLevelWhy::Shared, act);
    const LocoOutput o = lc.Update(in);
    Check("R4 T1 witness: one puppet's finding 'the writes are ignored' is not imposed on the others (no active level after the first report), the second puppet's within 60 s is (Placed for 45 s), "
          "and a Blend controller told so is at the rigid follow on its next update with the reason 'shared'",
          !first && none == 0.0 && second && act > 40000.0 && act <= 45000.0 && o.levelChanged && o.level == LocoLevel::Placed && o.levelWhy == LocoLevelWhy::Shared,
          F("first %d, active after one %.0f, second %d, active %.0f ms, level %d changed %d why %d", first, none, second, act, int(o.level), o.levelChanged, int(o.levelWhy)));
}

static void TestTarget() {
    // 5c. Where the gait task aims. Blend: at a point on the POSE's line, whatever the ped's error is at the moment of the issue (the ped's error is closed by the velocity; a target
    // placed past the pose by that error made a transient error a sideways aim for the whole life of the task). 0.4.9: pose + the ped's error + the lead.
    auto issueTarget = [](const LocoConfig& cfg, double lateral) {
        LocomotionController lc(cfg);
        LocoInput in;
        in.vel = V(4.5f, 0, 0); in.heading = 270; in.pedHeading = 270; in.pedGait = Gait::Run; in.dtMs = 1000.0 / 60; in.noAnchor = true;
        double t = 0;
        for (int i = 0; i < 240; ++i, t += 1.0 / 60) { in.pos = V(float(4.5 * t), 0, 30); in.pedPos = V(float(4.5 * t - 0.3), 0, 30); lc.Update(in); }
        in.pedPos = V(float(4.5 * t - 0.3), float(lateral), 30);          // the ped is `lateral` m to the side of the pose's line when the task is re-issued
        in.pos = V(float(4.5 * t), 0, 30);
        lc.Reset();
        LocoOutput o;
        for (int i = 0; i < 60 && o.task != LocoTask::GoStraight; ++i) { t += 1.0 / 60; in.pos = V(float(4.5 * t), 0, 30); in.pedPos = V(float(4.5 * t - 0.3), float(lateral), 30); o = lc.Update(in); }
        return o;
    };
    const LocoOutput b = issueTarget(CfgBlend(), 1.5), o = issueTarget(Cfg049(), 1.5);
    Check("R4 T1 target: the Blend gait task aims at the pose's line (target y = 0 within 0.05 m with the ped 1.5 m to the side, 12..40 m ahead); 0.4.9 aimed past the pose by the ped's error (y = -1.5)",
          b.task == LocoTask::GoStraight && std::fabs(b.target.y) < 0.05 && std::hypot(b.target.x - 4.5 * 4.0, 0) > 10.0 && o.task == LocoTask::GoStraight && std::fabs(o.target.y + 1.5) < 0.1,
          F("Blend target (%.1f, %.2f), 0.4.9 target (%.1f, %.2f)", b.target.x, b.target.y, o.target.x, o.target.y));
}

static void TestSwing() {
    // 6. Task 3's interface: LocoInput::swingActive holds every task issue and repair, in every mode; the velocity drive goes on; the held issue goes out afterwards.
    const Opt base;
    for (int m = 0; m < 3; ++m) {
        const LocoMode mode = m == 0 ? LocoMode::Blend : m == 1 ? LocoMode::Straight : LocoMode::Anchor;
        const char* nm = m == 0 ? "Blend" : m == 1 ? "Straight" : "Anchor";
        LocomotionController lc(ShippedLocoConfig(mode));
        auto in = [&](double t, bool swing) {
            LocoInput x;
            x.pos = V(float(4.5 * t), 0, 30); x.vel = V(4.5f, 0, 0); x.heading = 270;
            x.pedPos = V(float(4.5 * t - 0.3), 0, 30); x.pedHeading = 270; x.pedGait = Gait::Run;
            x.dtMs = 1000.0 / 60; x.swingActive = swing; x.noAnchor = true; x.pedVel = V(4.5f, 0, 0); x.havePedVel = true;
            return x;
        };
        double t = 0;
        int issuesBefore = 0, issuesSwing = 0, issuesAfter = 0, heldFrames = 0, pushSwing = 0, resetsDone = 0;
        for (int i = 0; i < 180; ++i, t += 1.0 / 60) { if (lc.Update(in(t, false)).task != LocoTask::None) ++issuesBefore; }
        // 1 s of swing: someone resets the task list (the swing replaced it) several times while it plays
        for (int i = 0; i < 60; ++i, t += 1.0 / 60) {
            if (i % 15 == 0) { lc.Reset(); ++resetsDone; }
            const LocoOutput o = lc.Update(in(t, true));
            if (o.task != LocoTask::None || o.repair != LocoRepair::None) ++issuesSwing;
            if (o.swingHeld) ++heldFrames;
            if (o.setVelocity && !o.snap) ++pushSwing;
        }
        int sinceSwing = 0;
        bool issuedAt = false;
        for (int i = 0; i < 120; ++i, t += 1.0 / 60) {
            const LocoOutput o = lc.Update(in(t, false));
            if (o.task != LocoTask::None) { ++issuesAfter; if (!issuedAt) { issuedAt = true; sinceSwing = i; } }
        }
        const bool pushOk = (mode == LocoMode::Blend) ? pushSwing > 30 : true;
        Check(F("R4 T1 swing (%s): swingActive for 1 s with the task list reset 4 times: 0 task issues and 0 repairs during it, the held issues are reported (swingHeld on >= 30 frames), the first issue "
                "goes out after the hold (swingHoldMs 700) and exactly once%s", nm, mode == LocoMode::Blend ? ", the velocity drive keeps writing (>= 30 frames)" : "").c_str(),
              issuesBefore >= 1 && issuesSwing == 0 && heldFrames >= 30 && issuesAfter >= 1 && issuesAfter <= 2 && sinceSwing >= 30 && sinceSwing <= 100 && pushOk,
              F("issues before %d, during %d, held frames %d, after %d (first %d frames after the swing), velocity writes during the swing %d", issuesBefore, issuesSwing, heldFrames, issuesAfter, sinceSwing, pushSwing));
        (void)resetsDone;
    }
    // the stand-still task is a task too: a stopped pose with the task list reset during a swing issues nothing until the swing is over
    {
        LocomotionController lc(ShippedLocoConfig(LocoMode::Blend));
        LocoInput in; in.pos = V(10, 0, 30); in.pedPos = V(10, 0, 30); in.heading = 270; in.pedHeading = 270; in.dtMs = 1000.0 / 60; in.noAnchor = true;
        for (int i = 0; i < 120; ++i) lc.Update(in);
        lc.Reset();
        int during = 0, after = 0;
        for (int i = 0; i < 40; ++i) { in.swingActive = true; if (lc.Update(in).task == LocoTask::StandStill) ++during; }
        for (int i = 0; i < 100; ++i) { in.swingActive = false; if (lc.Update(in).task == LocoTask::StandStill) ++after; }
        Check("R4 T1 swing: ... and so is TASK_STAND_STILL: a stopped pose whose task list was reset during a swing issues it only after the swing (0 during, 1 after)", during == 0 && after == 1,
              F("during %d, after %d", during, after));
    }
    // a repair is held too: the dead puppet (D) in straight mode asks for ClearTasks after two stalls; a swing over that moment postpones it
    {
        Opt q; q.warmupMs = 0;
        LocoConfig c = ShippedLocoConfig(LocoMode::Straight);
        c.quietTrial = false;
        const Game dead = Cell(GaitSrc::None, 1.0, 0.0, "A/E");
        const Scn sc = Scenarios()[1];
        const Res free = Run(sc, dead, c, q);
        const bool have = !free.repairs.empty();
        const double t1 = have ? free.repairs[0].first : 0;
        Opt s = q; s.swingFromMs = std::max(0.0, t1 - 1500.0); s.swingToMs = t1 + 1500.0;
        const Res held = Run(sc, dead, c, s);
        const double first = held.repairs.empty() ? -1 : held.repairs[0].first;
        Check("R4 T1 swing: a repair (ClearPedTasks) the controller would ask for at t is held while a swing plays over it: it comes after the swing and its hold (>= swing end + 700 ms), no task issue inside the swing",
              have && first >= s.swingToMs + 600.0 && held.tasksInSwing == 0 && free.tasksInSwing == 0 && held.st.swingHeld > 10,
              F("repair free-running at %.0f ms; with the swing %.0f..%.0f ms at %.0f ms; tasks in swing %d; held frames %u", t1, s.swingFromMs, s.swingToMs, first, held.tasksInSwing, held.st.swingHeld));
    }
}

// 0.5.7 (live 0.5.5, 08:00:51: an upper-body punch (flags 16|32, the whole 1.8 s clip) by a STANDING attacker who then walked away: the
// puppet kept its stand-still task through the swing + its 700 ms hold, IS_PED_STILL read true on 179 of 179 asks while our velocity writes
// carried it 1.7 m = the slide GI saw). An upper-body / secondary clip does not replace the gait task (SwingVariantReplacesGait), so with
// swingWalkThrough the gait task may follow the pose through such a swing; FullBody / Plain / the melee task stay held (TestSwing).
static Scn PunchThenGo(double sp, double goAtMs) {
    Scn s{sp < 3.0 ? "stand, punch (upper body), walk 1.5 away" : "stand, punch (upper body), run 4.5 away",
          [sp, goAtMs](double t) { return t * 1000.0 < goAtMs ? 0.0 : sp; }, [](double) { return 0.0; }};
    s.durMs = 11000;
    return s;
}
static void TestSwingWalk() {
    const double swingAt = 4000.0, swingEnd = swingAt + 1100.0;       // kWholeClipHoldMs: the probe holds a whole-clip swing 1100 ms
    LocoConfig oldC = CfgBlend(); oldC.swingWalkThrough = false;
    const LocoConfig newC = CfgBlend();
    struct Row { std::string cell, scn; Res o, n, nCut; };
    std::vector<Row> rows;
    bool reproOk = true, fixOk = true, errOk = true, clipOk = true;
    double oldD = 0, newMax = 0;
    std::string why;
    for (const Game& g : Cells()) {
        if (g.src == GaitSrc::None) continue;                          // no gait source: the ped reads still whatever is issued (both configs)
        for (double sp : {1.5, 4.5}) {
            const Scn sc = PunchThenGo(sp, swingAt + 250.0);
            Opt op; op.swingFromMs = swingAt; op.swingToMs = swingEnd; op.swingSecondary = true;
            Row r; r.cell = g.name; r.scn = sc.name;
            r.o = Run(sc, g, oldC, op);
            r.n = Run(sc, g, newC, op);
            Opt cut = op; cut.swingCutByTask = true;
            r.nCut = Run(sc, g, newC, cut);
            // the old code must show the slide where the writes are honoured (D: at least the walk's 1.5 m/s over most of the 1.55 s held)
            if (g.auth >= 1.0) { oldD = std::max(oldD, r.o.stillTravelM); if (r.o.stillTravelM < 0.8 * sp) { reproOk = false; why += F(" old %s %s %.2f m;", g.name, sc.name, r.o.stillTravelM); } }
            newMax = std::max(newMax, r.n.stillTravelM);
            if (r.n.stillTravelM > 0.25 * sp * 0.5 + 0.05) { fixOk = false; why += F(" new %s %s %.2f m;", g.name, sc.name, r.n.stillTravelM); }
            if (r.n.p95 > r.o.p95 + 0.10 || r.n.maxErrAny > r.o.maxErrAny + 0.3) { errOk = false; why += F(" err %s %s p95 %.2f->%.2f max %.2f->%.2f;", g.name, sc.name, r.o.p95, r.n.p95, r.o.maxErrAny, r.n.maxErrAny); }
            if (r.n.tasksInSwing > 2) { clipOk = false; why += F(" %s %s %d tasks in the swing;", g.name, sc.name, r.n.tasksInSwing); }
            rows.push_back(r);
        }
    }
    if (std::getenv("R4T1_SWINGWALK"))
        for (const Row& r : rows)
            std::printf("  %-7s %-42s still-travel old %.2f new %.2f m | p95 %.2f/%.2f max %.2f/%.2f | tasks in swing %d/%d | clip shown (task cuts it) %.0f ms | walks %u\n",
                        r.cell.c_str(), r.scn.c_str(), r.o.stillTravelM, r.n.stillTravelM, r.o.p95, r.n.p95, r.o.maxErrAny, r.n.maxErrAny, r.o.tasksInSwing, r.n.tasksInSwing,
                        r.nCut.swingClipMs, r.n.st.swingWalks);
    Check("R4 T1 swing walk (0.5.7): a standing attacker punches (upper-body clip, swingActive 1.1 s + the 700 ms hold) and walks / runs away 250 ms later: the 0.5.6 hold leaves the "
          "stand-still task on and the writes carry a still ped (>= 0.8 x speed metres in every D cell); swingWalkThrough lets the gait task follow (<= 0.125 x speed + 0.05 m in every B / S cell), "
          "never worse in p95 (+0.10) / max (+0.3), at most 2 task issues inside the swing",
          reproOk && fixOk && errOk && clipOk && newC.swingWalkThrough && !rows.empty(),
          F("old D worst %.2f m, new worst %.2f m, rows %zu;%s", oldD, newMax, rows.size(), why.c_str()));
    // the 10 s line's read-backs (SwingWalkDiag, fed per frame as DrivePeer feeds it) tell the two apart in the same runs: 0.5.6's slide is booked
    // under "held swing", 0.5.7's walk-through frames move a ped that reads walking, and the walk-through issues are counted
    {
        bool ok = true;
        std::string w;
        for (const Row& r : rows) {
            const bool honoured = r.cell.find("/D") != std::string::npos;
            const double sp = r.scn.find("walk") != std::string::npos ? 1.5 : 4.5;
            const bool oldOk = !honoured || (r.o.sw.slideM[SwingWalkSummary::Held] >= 0.8 * sp && r.o.sw.slideM[SwingWalkSummary::Through] == 0.0 && r.o.sw.walkIssued == 0);
            const bool newOk = r.n.sw.slideM[SwingWalkSummary::Held] + r.n.sw.slideM[SwingWalkSummary::Through] <= 0.125 * sp + 0.05 && r.n.sw.walkIssued >= 1 &&
                               r.n.sw.askedFrames[SwingWalkSummary::Through] >= 30;
            if (!oldOk || !newOk) {
                ok = false;
                w += F(" %s %s: old held %.2f through %.2f issued %u; new held %.2f through %.2f (asks %u) issued %u;", r.cell.c_str(), r.scn.c_str(), r.o.sw.slideM[0], r.o.sw.slideM[1],
                       r.o.sw.walkIssued, r.n.sw.slideM[0], r.n.sw.slideM[1], r.n.sw.askedFrames[1], r.n.sw.walkIssued);
            }
        }
        const SwingWalkSummary& a = rows.empty() ? SwingWalkSummary() : rows[0].o.sw;
        Check("R4 T1 swing walk (0.5.7) diag: the 'on-foot swing walk (0.5.7)' read-backs book 0.5.6's slide under 'in a held swing' (>= 0.8 x speed m in the D cells, nothing under walk-through), "
              "and 0.5.7's walk-through swing as asked frames that barely move a still-reading ped (<= 0.125 x speed + 0.05 m) with >= 1 walk-through issue",
              ok && !rows.empty(), F("rows %zu, first row old: held %.2f m (%u of %u asks);%s", rows.size(), a.slideM[0], a.slideFrames[0], a.askedFrames[0], w.c_str()));
    }
    // a punch that stays put issues nothing in the swing (the clip is never cut by us), in both configs
    {
        Scn st{"stand and punch", [](double) { return 0.0; }, [](double) { return 0.0; }};
        st.durMs = 9000;
        Opt op; op.swingFromMs = swingAt; op.swingToMs = swingEnd; op.swingSecondary = true;
        int worst = 0;
        for (const Game& g : Cells()) worst = std::max(worst, Run(st, g, newC, op).tasksInSwing);
        Check("R4 T1 swing walk (0.5.7): an attacker who punches standing still gets 0 task issues inside the swing in all 12 cells (walk-through only acts when the pose moves)", worst == 0,
              F("most task issues inside the swing %d", worst));
    }
    // the ablation over the whole model x scenario matrix: the same upper-body swing (8.0 .. 9.1 s) in every scenario of every cell, 0.5.6 vs 0.5.7
    {
        // A swing at 8 s lands on the scenarios' events (the 1 s outage, the teleport); one at 13 s is clear of them. The bar for a run is the
        // better of 0.5.6 under the same swing and the controller with no swing at all (the walk-through hands the frames back to the normal
        // controller, so where the 0.5.6 hold happened to help (it kept a run task going through a 1 s outage) the no-swing run is the reference).
        int worse = 0, runs = 0, holdHelped = 0;
        double oldTravel = 0, newTravel = 0, worstP95 = 0;
        std::string w;
        const bool show = std::getenv("R4T1_SWINGWALK") != nullptr;
        for (const Game& g : Cells())
            for (const Scn& sc : Scenarios()) {
                const Res ns = Run(sc, g, newC, Opt());
                for (double at : {8000.0, 13000.0}) {
                    Opt op; op.swingFromMs = at; op.swingToMs = at + 1100.0; op.swingSecondary = true;
                    const Res o = Run(sc, g, oldC, op), n = Run(sc, g, newC, op);
                    ++runs;
                    if (g.src != GaitSrc::None) { oldTravel += o.stillTravelM; newTravel += n.stillTravelM; }   // A reads still whatever is issued
                    worstP95 = std::max(worstP95, n.p95 - o.p95);
                    const double maxBar = std::max(o.maxErrAny, ns.maxErrAny) + 0.3;
                    if (n.maxErrAny > o.maxErrAny + 0.3 && n.maxErrAny <= maxBar) ++holdHelped;
                    const bool bad = n.p95 > o.p95 + 0.10 || n.maxErrAny > maxBar || n.flaps > std::max(o.flaps, ns.flaps) || n.st.hardSnaps > std::max(o.st.hardSnaps, ns.st.hardSnaps);
                    if (bad) { ++worse; w += F(" %s / %s @%.0f;", g.name, sc.name, at); }
                    if (bad || show)
                        std::printf("    %s %-7s %-56s @%5.0f: p95 %.2f->%.2f max %.2f->%.2f (no swing %.2f) flaps %u->%u snaps %u->%u still-travel %.2f->%.2f walks %u\n", bad ? "WORSE" : "abl  ",
                                    g.name, sc.name, at, o.p95, n.p95, o.maxErrAny, n.maxErrAny, ns.maxErrAny, o.flaps, n.flaps, o.st.hardSnaps, n.st.hardSnaps, o.stillTravelM, n.stillTravelM, n.st.swingWalks);
                }
            }
        Check("R4 T1 swing walk (0.5.7) ablation: an upper-body swing at 8 s (on the events) and at 13 s in every scenario x every cell (264 runs): 0.5.7 is never worse than 0.5.6 in p95 (+0.10) "
              "nor than the better of 0.5.6 / no swing in max (+0.3), flaps and snaps; a still-reading ped (cells with a gait source) moves less in total",
              runs == 264 && worse == 0 && newTravel < oldTravel,
              F("runs %d, worse %d, max above 0.5.6 but within the no-swing run %d, still-travel total (B / S) %.1f -> %.1f m, worst p95 delta %+.2f;%s", runs, worse, holdHelped, oldTravel, newTravel,
                worstP95, w.c_str()));
    }
}

static void TestDiag() {
    // 7. The summary the 10 s line prints: error percentiles, the largest correction on top of the pose velocity, snaps, read-back vs write fights.
    LocoDiag d;
    const double dt = 1000.0 / 60;
    for (int i = 0; i < 200; ++i) {
        LocoDiagFrame f;
        f.dtMs = dt; f.nowMs = i * dt;
        f.pose = V(10.0 + i * 0.075, 0, 30);
        const double err = i < 60 ? 0.10 : (i < 188 ? 0.30 : 2.0);        // p50 sits in the 0.30 band, p95 in the 2.0 one (the last 12 of 200 = 6%)
        f.pedPos = V(f.pose.x - err, 0, 30);
        f.poseSpeed = 4.5f; f.poseVel = V(4.5f, 0, 0); f.gait = Gait::Run;
        f.pushed = true; f.blend = true; f.push = V(4.5f + (i == 50 ? 2.4f : 0.5f), 0, 0);
        f.havePedVel = true; f.pedVel = V(i % 4 == 0 ? 7.0f : 5.0f, 0, 0);           // the read-back differs from the last write by > 1 m/s on every 4th frame
        f.snapped = i == 10;
        d.Observe(f);
    }
    const LocoDiagSummary s = d.Summary();
    Check("R4 T1 diag: LocoDiag reports the error percentiles (p50 ~0.30, p95 ~2.0, max 2.0 m), the largest correction on top of the POSE velocity (2.4 m/s), snaps (1), blend frames (200) and the "
          "frames whose read-back differed from the write the frame before by > 1 m/s (50 of 198 comparable)",
          std::fabs(s.errP50M - 0.30) <= 0.06 && std::fabs(s.errP95M - 2.0) <= 0.1 && std::fabs(s.maxErrM - 2.0) < 0.01 && std::fabs(s.maxCorrMs - 2.4) < 0.01 && s.snapFrames == 1 && s.blendFrames == 200 &&
              s.fightVelFrames >= 45 && s.fightVelFrames <= 52 && s.fightCmpFrames >= 195,
          F("p50 %.2f p95 %.2f max %.2f corr %.2f snaps %u blend %u fight(read-back) %u of %u", s.errP50M, s.errP95M, s.maxErrM, s.maxCorrMs, s.snapFrames, s.blendFrames, s.fightVelFrames, s.fightCmpFrames));
}

static void TestConfig() {
    // 8. The three modes of the shipped config: anchor = the 0.4.8 pair, straight = the 0.4.9 pair, blend = the round-4 default.
    const LocoConfig a = ShippedLocoConfig(LocoMode::Anchor), s = ShippedLocoConfig(LocoMode::Straight), b = ShippedLocoConfig(LocoMode::Blend);
    Check("R4 T1 config: ShippedLocoConfig(mode): Anchor = anchor + nudge (0.4.8), Straight = straight + velocity push (0.4.9), Blend = blend with the self-checks on, the quiet window and the repairs off, "
          "a 400 ms minimum hold of a moving gait, a far (12..40 m) target on the pose's line",
          a.mode == LocoMode::Anchor && a.push == PushMode::Nudge && s.mode == LocoMode::Straight && s.push == PushMode::Velocity && b.mode == LocoMode::Blend && b.selfCheck && !b.quietTrial &&
              b.maxRepairs == 0 && b.gaitMinHoldMs == 400.0f && b.leadMinM >= 12.0f && b.leadMaxM >= 40.0f && b.blendHardCapMs == 3.0f && b.blendMaxDiffMs < b.blendHardCapMs && b.blendSnapM == 6.0f &&
              s.gaitMinHoldMs == 0.0f && a.gaitMinHoldMs == 0.0f,
          F("anchor %d/%d straight %d/%d blend %d selfCheck %d quiet %d repairs %d hold %.0f lead %.0f..%.0f", int(a.mode), int(a.push), int(s.mode), int(s.push), int(b.mode), b.selfCheck, b.quietTrial,
            b.maxRepairs, b.gaitMinHoldMs, b.leadMinM, b.leadMaxM));
}

static void TestSenderClip() {
    // 9. 0.4.17 (live 0.4.15: a sender at 2.8-3.3 m/s showed WALKING on 38-86 % of frames, sprinting never): the blend ratio is the sender's gait.
    //    3 s at 3.0 m/s with the sender running (moveState 2), then 3 s at 5.6 m/s sprinting (3); every blend ratio the controller hands the game
    //    (issue or set) after the 400 ms gait hold of each phase must be 2.00, then 3.00. The 0.4.15 knob sends ~1.5 and ~2.4 (+ trim).
    auto run = [](bool clip, double& mean1, double& mean2, int& bad, int& seen) {
        LocoConfig c = ShippedLocoConfig(LocoMode::Blend);
        c.blendGaitClip = clip;
        LocomotionController lc(c);
        const double dt = 1000.0 / 60;
        double x = 0, sum1 = 0, sum2 = 0;
        int n1 = 0, n2 = 0;
        bad = 0; seen = 0;
        for (int i = 0; i < 360; ++i) {
            const bool second = i >= 180;
            const double sp = second ? 5.6 : 3.0;
            x += sp * dt / 1000.0;
            LocoInput in;
            in.pos = V(float(x), 0, 30); in.vel = V(float(sp), 0, 0); in.heading = 270;
            in.pedPos = V(float(x), 0, 30); in.pedHeading = 270; in.pedGait = second ? Gait::Sprint : Gait::Run;   // writes honoured: the ped is on the pose
            in.pedVel = V(float(sp), 0, 0); in.havePedVel = true;
            in.dtMs = dt; in.noAnchor = true; in.moveState = second ? 3 : 2;
            const LocoOutput o = lc.Update(in);
            const int phaseFrame = second ? i - 180 : i;
            if ((o.task != LocoTask::None || o.setBlend) && o.moveBlendRatio > 0 && (!second || phaseFrame >= 30)) {
                ++seen;
                const double want = second ? 3.0 : 2.0;
                if (std::fabs(o.moveBlendRatio - want) > 0.01) ++bad;
                if (second) { sum2 += o.moveBlendRatio; ++n2; } else { sum1 += o.moveBlendRatio; ++n1; }
            }
        }
        mean1 = n1 ? sum1 / n1 : -1; mean2 = n2 ? sum2 / n2 : -1;
    };
    double a1, a2, b1, b2;
    int badNew, seenNew, badOld, seenOld;
    run(true, a1, a2, badNew, seenNew);
    run(false, b1, b2, badOld, seenOld);
    Check("R4 T1 sender clip (0.4.17): with blendGaitClip (shipped default) every blend ratio handed to the game is the sender's gait (2.00 running at 3.0 m/s, "
          "3.00 sprinting at 5.6 m/s); with it off (0.4.15) the speed knob sends something else",
          ShippedLocoConfig(LocoMode::Blend).blendGaitClip && seenNew >= 2 && badNew == 0 && a1 > 1.99 && a2 > 2.99 && seenOld >= 2 && badOld > 0 && b1 < 1.9,
          F("new: %d of %d off, means %.2f / %.2f; old: %d of %d off, means %.2f / %.2f", badNew, seenNew, a1, a2, badOld, seenOld, b1, b2));
}

} // namespace r4

static void R4T1Tests() {
    std::printf("-- R4 T1: Blend, a velocity-driven walking controller, against switchable models of the game\n");
    r4::TestReproduce049();
    r4::TestAcceptance();
    r4::TestCaps();
    r4::TestGait();
    r4::TestSelfCheck();
    r4::TestWitness();
    r4::TestTarget();
    r4::TestSwing();
    r4::TestSwingWalk();
    r4::TestDiag();
    r4::TestConfig();
    r4::TestSenderClip();
}
