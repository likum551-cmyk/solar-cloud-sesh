// R5 (on-foot round 5): the Blend self-check must not fall to the rigid follow because a TASK CHANGED (docs/sync-specs/onfoot.md "R5"; the
// controller side is libs/sync/locomotion.h "11. R5").
//
// Included by tests/locomotion_test.cpp after tests/r4_t1_blend_tests.h (it reuses that file's game models, scenario runner and checks) and called
// once from main as R5Tests(). R5_TABLE=1 prints the model x scenario tables and the ablation the report quotes (R5_CELLS=1 adds one line per run).
//
// WHAT GI'S LIVE LOG SHOWED (0.4.26, two players, Legacy 3889, 17:36-17:43): three falls to the rigid follow in seven minutes, each held 30 s.
// Two of them came right after a task-changing Reset() (the end of a vault, of an aim, of a hit reaction, the car exit): the new task's start
// clip does not honour velocity writes for a second or so, the error sits at 0.7-1.2 m, over 75 % of the frames fight the writes and BlendWatch
// calls that a failure of the writes. The simulated ped for it (r4::Event): at the event its task is gone and for stunMs it moves at stunK x the
// pose's velocity whatever is written. stunMs / stunK are MY model of "about a second, 0.7-1.2 m", not measurements of the game.
//
// What is asserted, and against what:
//   * the OLD controller (Round4SelfCheck) falls to the rigid follow in these worlds, the NEW one (the shipped Blend config) does not;
//   * the grace window, the place-once, the chain cap, the explained / unexplained hold, the probation, the level cap, the trim gate and the Degrade
//     cap each have a test that FAILS when that one rule is removed (the mutants were run by hand; the report names them).

#include <map>
#include <set>

namespace r5 {

using r4::Event;
using r4::Game;
using r4::GaitSrc;
using r4::Opt;
using r4::Res;
using r4::Scn;

static LocoConfig CfgOld() { return Round4SelfCheck(r4::CfgBlend()); }
static LocoConfig CfgNew() { return r4::CfgBlend(); }
// ablation halves: only the grace window (the round-4 hold) / only the hold (no grace window)
static LocoConfig CfgGraceOnly() {
    LocoConfig c = CfgOld();
    const LocoConfig n = CfgNew();
    c.blendGraceMs = n.blendGraceMs; c.blendGracePlaceM = n.blendGracePlaceM; c.blendGraceMaxMs = n.blendGraceMaxMs;
    return c;
}
static LocoConfig CfgHoldOnly() {
    LocoConfig c = CfgNew();
    c.blendGraceMs = 0.0f;
    return c;
}

static Scn RunScn(double durMs = 20000) {
    Scn s{"run 4.5 straight", [](double) { return 4.5; }, [](double) { return 0.0; }, durMs};
    return s;
}
// the owner leaves a car (stands still), then runs: the pose is still while the puppet is paused
static Scn ExitScn() {
    Scn s{"leave a car, then run 4.5", [](double t) { return t < 5.5 ? 0.0 : 4.5; }, [](double) { return 0.0; }, 18000};
    return s;
}

struct Case { const char* name; Scn scn; Opt op; };

static std::vector<Case> Cases() {
    std::vector<Case> v;
    { Case c{"vault ends (climb-end Reset)", RunScn(), Opt()}; c.op.events = {Event{7000, 0, 1600, 0.75}}; v.push_back(c); }
    { Case c{"aim ends twice", RunScn(), Opt()}; c.op.events = {Event{5000, 0, 1200, 0.8}, Event{12000, 0, 1200, 0.8}}; v.push_back(c); }
    { Case c{"hit reaction ends", RunScn(), Opt()}; c.op.events = {Event{6000, 0, 1000, 0.6}}; v.push_back(c); }
    { Case c{"car exit (pause 1 s, then Reset)", ExitScn(), Opt()}; c.op.events = {Event{4500, 1000, 1500, 0.6}}; v.push_back(c); }
    return v;
}
// the worlds where the writes carry the ped (A/C and B2/C are the R4 worlds in which they are found NOT to: they fall at ~1.3 s with no event at all)
static std::vector<Game> StunWorlds() { return r4::Pick({"A/D", "B1.5/D", "B1.5/C", "B2/D", "S/D", "S/C"}); }

struct Tally {
    int runs = 0, fell = 0, placedRuns = 0;
    double msPlaced = 0, worstErr = 0;
    uint32_t degrades = 0, gracePlaced = 0, graceWindows = 0, hard = 0;
};
static Tally SweepCase(const Case& c, const LocoConfig& cfg) {
    Tally t;
    for (const Game& g : StunWorlds()) {
        const Res r = r4::Run(c.scn, g, cfg, c.op);
        ++t.runs;
        if (r.st.blendDegrades > 0) ++t.fell;
        if (r.firstPlacedMs >= 0) ++t.placedRuns;
        t.msPlaced += r.msPlaced;
        t.worstErr = std::max(t.worstErr, r.errMaxAfterEvent);
        t.degrades += r.st.blendDegrades;
        t.gracePlaced += r.st.gracePlaced;
        t.graceWindows += r.st.graceWindows;
        t.hard += r.st.hardSnaps;
    }
    return t;
}

static void Table() {
    std::printf("R5 ablation: runs that fell to the rigid follow / runs, mean ms at level 3, worst error after the event, grace places (6 models x each case)\n");
    struct V { const char* n; LocoConfig c; };
    const std::vector<V> vs = {{"old (round 4)", CfgOld()}, {"grace only", CfgGraceOnly()}, {"hold only", CfgHoldOnly()}, {"new (shipped)", CfgNew()}};
    for (const Case& c : Cases()) {
        std::printf("  %s\n", c.name);
        for (const V& v : vs) {
            const Tally t = SweepCase(c, v.c);
            std::printf("    %-14s fell %d/%d  ms at level 3 %6.0f  worst err %.2f  grace places %u  hard snaps %u\n", v.n, t.fell, t.runs, t.msPlaced / t.runs, t.worstErr, t.gracePlaced, t.hard);
            if (std::getenv("R5_CELLS"))
                for (const Game& g : StunWorlds()) {
                    const Res r = r4::Run(c.scn, g, v.c, c.op);
                    if (r.st.blendDegrades > 0 || r.st.gracePlaced > 0)
                        std::printf("        %-7s first level 3 at %6.0f ms, degrades %u, grace places %u, held frames %u, explained falls %u, unexplained %u, probation falls %u, err after %.2f, level changes %zu\n", g.name,
                                    r.firstPlacedMs, r.st.blendDegrades, r.st.gracePlaced, r.st.graceHeldFrames, r.st.fallsAfterGrace, r.st.fallsUnexplained, r.st.probationFalls, r.errMaxAfterEvent, r.levelChanges);
                }
        }
    }
}

// ---- the tests ---------------------------------------------------------------------------------------------------------------

static Res One(const char* world, const Scn& sc, const LocoConfig& cfg, const Opt& op) { return r4::Run(sc, r4::Pick({world})[0], cfg, op); }

static void Show(const char* what, const Res& r) {
    if (!std::getenv("R5_TABLE")) return;
    std::printf("    [%s] first level 3 at %.0f ms, ms at level 3 %.0f, level changes %zu, degrades %u (explained %u, unexplained %u, probation %u), grace windows %u places %u capped %u held %u, "
                "p95 %.2f max %.2f after-event max %.2f, hard %u, final level %d\n", what, r.firstPlacedMs, r.msPlaced, r.levelChanges, r.st.blendDegrades, r.st.fallsAfterGrace,
                r.st.fallsUnexplained, r.st.probationFalls, r.st.graceWindows, r.st.gracePlaced, r.st.graceChainCapped, r.st.graceHeldFrames, r.p95, r.maxErr, r.errMaxAfterEvent,
                r.st.hardSnaps, int(r.finalLevel));
}

static void TestEvents() {
    // 1. A task-changing event must not send the puppet to the rigid follow. The OLD controller (Round4SelfCheck) does, the shipped one does not, in every world where the
    // writes carry the ped.
    const int minOld[4] = {6, 2, 1, 6};
    const std::vector<Case> cs = Cases();
    for (size_t i = 0; i < cs.size(); ++i) {
        const Case& c = cs[i];
        const Tally o = SweepCase(c, CfgOld()), n = SweepCase(c, CfgNew());
        const std::string nm = std::string("R5 events: ") + c.name + ": the round-4 self-check falls to the rigid follow in at least " + std::to_string(minOld[i]) +
                               " of 6 worlds, the shipped one in none (no hard snap, the error after the event stays under 3 m)";
        Check(nm.c_str(), o.fell >= minOld[i] && n.fell == 0 && n.degrades == 0 && n.hard == 0 && n.worstErr < 3.0,
              F("old fell %d/%d (%.0f ms at level 3 on average), new fell %d/%d, new degrades %u, hard snaps %u, worst error after the event %.2f (old %.2f)", o.fell, o.runs, o.msPlaced / o.runs, n.fell,
                n.runs, n.degrades, n.hard, n.worstErr, o.worstErr));
    }
    // the ablation: the grace window is what keeps the puppet out of the rigid follow (the hold alone only shortens the stay, and only of a fall the window explains)
    const Tally oV = SweepCase(cs[0], CfgOld()), gV = SweepCase(cs[0], CfgGraceOnly()), hV = SweepCase(cs[0], CfgHoldOnly()), nV = SweepCase(cs[0], CfgNew());
    Check("R5 events ablation (vault, 6 worlds): the hold without the grace window falls as often as round 4, the grace window without the new hold falls at most once, both together never",
          hV.fell == oV.fell && gV.fell <= 1 && nV.fell == 0, F("old %d, grace only %d, hold only %d, both %d of %d", oV.fell, gV.fell, hV.fell, nV.fell, nV.runs));
}

static void TestPlaceOnce() {
    // 2. A stun that outlasts the window: the ped is placed ONCE on the pose, the blend goes on (no fall, no re-issue of the task by the placement).
    Opt big; big.events = {Event{7000, 0, 2600, 0.5}};
    Opt small; small.events = {Event{7000, 0, 1200, 0.95}};
    const Scn sc = RunScn();
    int placedOk = 0, runs = 0; uint32_t smallPlaced = 0, falls = 0, hard = 0; double worst = 0;
    for (const Game& g : r4::Pick({"S/D", "B1.5/D", "A/D"})) {
        const Res a = r4::Run(sc, g, CfgNew(), big), b = r4::Run(sc, g, CfgNew(), small);
        ++runs;
        if (a.st.gracePlaced == 1) ++placedOk;
        falls += a.st.blendDegrades + b.st.blendDegrades;
        hard += a.st.hardSnaps + b.st.hardSnaps;
        smallPlaced += b.st.gracePlaced;
        worst = std::max(worst, a.errMaxAfterEvent);
        Show(g.name, a);
    }
    Check("R5 place once: a ped still more than 1.0 m from the pose when the 2 s window closes (a 2.6 s stun at half speed) is placed exactly once per run, the blend goes on: no fall, no hard snap, "
          "the error after the event stays under 5 m (the stun alone drifts 4.5 m in 2 s)", placedOk == runs && falls == 0 && hard == 0 && worst < 5.0, F("placed once in %d of %d, falls %u, hard snaps %u, worst error %.2f", placedOk, runs, falls, hard, worst));
    Check("R5 place once: a ped within 1.0 m of the pose when the window closes (a 1.2 s stun at 95 % speed) is not touched", smallPlaced == 0, F("grace places %u", smallPlaced));
}

static void TestStorm() {
    // 3. A storm of Resets (one every 0.5 s for 10 s while the ped ignores the writes) re-arms the window but never extends it past 6 s from its first opening: the failure is judged.
    Opt op; op.events.clear();
    for (int i = 0; i < 21; ++i) op.events.push_back(Event{5000.0 + 500.0 * i, 0, 600, 0.5});
    const Scn sc = RunScn(26000);
    const Res a = One("S/D", sc, CfgNew(), op);
    LocoConfig nocap = CfgNew(); nocap.blendGraceMaxMs = 1e9f;
    const Res b = One("S/D", sc, nocap, op);
    Show("storm, shipped", a); Show("storm, no chain cap", b);
    Check("R5 reset storm: 21 Resets in 10 s with the writes ignored: the window chain is capped (counter > 0) and the fall to the rigid follow comes by 15 s; with the cap removed it does not come before the storm is over",
          a.st.graceChainCapped > 0 && a.firstPlacedMs > 0 && a.firstPlacedMs < 15000 && (b.firstPlacedMs < 0 || b.firstPlacedMs > 15500),
          F("shipped: capped %u, first level 3 at %.0f ms; no cap: first level 3 at %.0f ms", a.st.graceChainCapped, a.firstPlacedMs, b.firstPlacedMs));
}

static void TestHold() {
    // 4. The hold: a fall the event explains stays 6 s at the rigid follow (and the blend is tried again); a fall nothing explains keeps the round-4 30 s.
    Opt ev; ev.events = {Event{5000, 0, 9000, 0.6}};
    const Scn s40 = RunScn(40000);
    const Res n = One("S/D", s40, CfgNew(), ev), o = One("S/D", s40, CfgOld(), ev);
    Show("explained, new", n); Show("explained, old", o);
    Check("R5 hold: a 9 s stun after an event (the writes ignored 9 s): the shipped controller falls once (explained by the event), is at the rigid follow 6 s (5-7 s counting the frames up to the retry), "
          "tries the blend again and ends at level 0; the round-4 controller sits there 25 s or more", n.st.fallsAfterGrace == 1 && n.st.fallsUnexplained == 0 && n.msPlaced > 5000 && n.msPlaced < 7000 &&
              n.finalLevel == LocoLevel::Full && o.msPlaced >= 25000,
          F("new: explained %u unexplained %u, %.0f ms at level 3, final level %d; old: %.0f ms", n.st.fallsAfterGrace, n.st.fallsUnexplained, n.msPlaced, int(n.finalLevel), o.msPlaced));
    // a fall with no event at all (the writes are ignored: world E) keeps the long hold
    const Scn s45 = RunScn(45000);
    const Res en = One("B1.5/E", s45, CfgNew(), Opt()), eo = One("B1.5/E", s45, CfgOld(), Opt());
    Show("unexplained, new", en); Show("unexplained, old", eo);
    Check("R5 hold: a fall with no event before it (the velocity writes ignored: world E) is the writes own: unexplained, and the stay is the round-4 30 s (the way back up not before 29 s, "
          "the time at level 3 within 0.5 s of the round-4 controllers)", en.st.fallsUnexplained >= 1 && en.st.fallsAfterGrace == 0 && en.changeAtMs.size() >= 2 && en.changeAtMs[1] - en.changeAtMs[0] >= 29000 &&
              std::fabs(en.msPlaced - eo.msPlaced) < 500.0,
          F("new: unexplained %u explained %u, level 3 from %.0f to %.0f ms, %.0f ms at level 3; old %.0f ms", en.st.fallsUnexplained, en.st.fallsAfterGrace, en.changeAtMs.empty() ? -1.0 : en.changeAtMs[0],
            en.changeAtMs.size() < 2 ? -1.0 : en.changeAtMs[1], en.msPlaced, eo.msPlaced));
    // a stun that never ends: the first fall is the event's, the retry on probation falls again, and that one is the writes own: long hold
    Opt forever; forever.events = {Event{5000, 0, 70000, 0.6}};
    const Res f = One("S/D", RunScn(70000), CfgNew(), forever);
    Show("never recovers", f);
    Check("R5 hold: when the writes stay ignored after an event, the retry fails on probation and the second fall is judged the writes own: the ped ends at the rigid follow with 3 level changes "
          "(down, up, down), one fall explained, one unexplained, one on probation", f.levelChanges == 3 && f.finalLevel == LocoLevel::Placed && f.st.fallsAfterGrace == 1 && f.st.fallsUnexplained == 1 && f.st.probationFalls == 1,
          F("level changes %zu, final level %d, explained %u unexplained %u probation falls %u", f.levelChanges, int(f.finalLevel), f.st.fallsAfterGrace, f.st.fallsUnexplained, f.st.probationFalls));
}

static void TestProbation() {
    // 5. The way back up is a probe: in a world that keeps ignoring the writes the probe costs a short excursion with the probation, a long one without.
    LocoConfig on = CfgNew(), off = CfgNew();
    on.placedUnexplainedMs = off.placedUnexplainedMs = 0.0f;   // every fall on the short timer, so there are retries to measure
    off.blendProbationMs = 0.0f;
    const Scn sc = RunScn(45000);
    const Res a = One("B1.5/E", sc, on, Opt()), b = One("B1.5/E", sc, off, Opt());
    Show("probation on", a); Show("probation off", b);
    Check("R5 probation: in world E on the 6 s timer (45 s, about 3 retries) the largest pose-ped error is smaller with the probation than without it, and the time spent at the rigid follow is no less than 80 % "
          "of the unprobated runs (the probation does not give the failure up)", a.maxErr < b.maxErr - 0.1 && a.msPlaced > 0.8 * b.msPlaced && a.st.probationFalls >= 1,
          F("with: max %.2f p95 %.2f, %.0f ms at level 3, %u probation falls, %zu level changes; without: max %.2f p95 %.2f, %.0f ms at level 3, %zu level changes", a.maxErr, a.p95, a.msPlaced, a.st.probationFalls,
            a.levelChanges, b.maxErr, b.p95, b.msPlaced, b.levelChanges));
}

static void TestLevelCap() {
    // 6. The ladder's level cap FREEZES it in whatever level it is at: with 12 changes per session (round 4) a puppet that is hit, aims, vaults all evening ends frozen at the rigid follow.
    Opt op; op.events.clear();
    for (int i = 0; i < 22; ++i) op.events.push_back(Event{5000.0 + 40000.0 * i, 0, 4000, 0.5});
    const Scn sc = RunScn(890000);
    LocoConfig capped = CfgNew(); capped.maxLevelChanges = 12;
    const Res a = One("S/D", sc, CfgNew(), op), b = One("S/D", sc, capped, op);
    Show("cap 60", a); Show("cap 12", b);
    Check("R5 level cap: 22 events of a 4 s stun (a fall and a recovery each): the shipped cap lets every event fall and recover (>= 20 falls, ends at level 0, every fall on the first-fall hold of 6 s: under 160 s at level 3 in all), the old cap of 12 freezes the ladder "
          "(fewer than 8 falls)", a.st.blendDegrades >= 20 && a.finalLevel == LocoLevel::Full && a.msPlaced < 160000 && b.st.blendDegrades < 8,
          F("cap 60: %u falls, %zu level changes, final level %d; cap 12: %u falls, %zu level changes, final level %d", a.st.blendDegrades, a.levelChanges, int(a.finalLevel), b.st.blendDegrades, b.levelChanges, int(b.finalLevel)));
}

static void TestDegradeCap() {
    // 7. What another puppet found (LevelWitness) is imposed for its time, not cut by the explained-fall cap.
    LocomotionController lc(CfgNew());
    lc.Degrade(LocoLevel::Placed, LocoLevelWhy::Shared, 45000.0);
    const float imposed = lc.PlacedLeftMs();
    LocomotionController lc2(CfgNew());
    lc2.Degrade(LocoLevel::Placed, LocoLevelWhy::Shared, 1e7);
    Check("R5 witness: Degrade(Placed, 45 s) keeps the 45 s the witness imposes (the explained-fall cap of 24 s does not apply to it), and nothing is imposed longer than the 300 s cap",
          imposed > 44000.0f && imposed <= 45000.0f && lc2.PlacedLeftMs() <= 300000.0f && lc2.PlacedLeftMs() > 299000.0f, F("45 s imposed -> %.0f ms, 1e7 ms imposed -> %.0f ms", imposed, lc2.PlacedLeftMs()));
}

static bool Near(double a, double b) { return std::fabs(a - b) < 0.5; }

static void TestConfig() {
    const LocoConfig n = ShippedLocoConfig(LocoMode::Blend), o = Round4SelfCheck(n);
    Check("R5 config: ShippedLocoConfig(Blend): grace 2000 ms, place 1.0 m, chain 6000 ms, probation 4000 ms (300 ms budget, errors x 0.6), attribute window 4000 ms, explained hold 6000 / 24000 ms, "
          "unexplained hold 30000 / 300000 ms, level hold 3600 ms, cap 60 level changes, trim gate 1.0 m, streak reset 30 s",
          n.blendGraceMs == 2000.0f && n.blendGracePlaceM == 1.0f && n.blendGraceMaxMs == 6000.0f && n.blendProbationMs == 4000.0f && n.blendProbationFailMs == 300.0f && n.blendProbationErrScale == 0.6f &&
              n.blendAttributeMs == 4000.0f && n.placedMs == 6000.0f && n.placedDurMaxMs == 24000.0f && n.placedUnexplainedMs == 30000.0f && n.placedUnexplainedMaxMs == 300000.0f && Near(n.levelHoldMs, 3600.0) &&
              n.maxLevelChanges == 60 && n.blendTrimGateM == 1.0f && n.placedExplainedResetMs == 30000.0f,
          F("grace %.0f place %.1f chain %.0f probation %.0f/%.0f/%.1f attribute %.0f hold %.0f/%.0f unexplained %.0f/%.0f level hold %.0f cap %d gate %.1f", n.blendGraceMs, n.blendGracePlaceM, n.blendGraceMaxMs,
            n.blendProbationMs, n.blendProbationFailMs, n.blendProbationErrScale, n.blendAttributeMs, n.placedMs, n.placedDurMaxMs, n.placedUnexplainedMs, n.placedUnexplainedMaxMs, n.levelHoldMs,
            n.maxLevelChanges, n.blendTrimGateM));
    const LocoConfig h = WithPlacedHold(n, 9000);
    Check("R5 config: Round4SelfCheck puts back grace 0, probation 0, Placed 30000 / 300000 ms for every fall, level hold 8000 ms, cap 12, no trim gate; WithPlacedHold(9000) = 9000 / 36000 ms, level hold 5400 ms",
          o.blendGraceMs == 0.0f && o.blendProbationMs == 0.0f && o.placedMs == 30000.0f && o.placedDurMaxMs == 300000.0f && o.placedUnexplainedMs == 0.0f && o.levelHoldMs == 8000.0f &&
              o.maxLevelChanges == 12 && o.blendTrimGateM == 0.0f && o.placedExplainedResetMs == 0.0f && h.placedMs == 9000.0f && h.placedDurMaxMs == 36000.0f && Near(h.levelHoldMs, 5400.0),
          F("old: grace %.0f placed %.0f/%.0f unexplained %.0f hold %.0f cap %d; 9000: %.0f/%.0f hold %.0f", o.blendGraceMs, o.placedMs, o.placedDurMaxMs, o.placedUnexplainedMs, o.levelHoldMs, o.maxLevelChanges,
            h.placedMs, h.placedDurMaxMs, h.levelHoldMs));
}

// a steady walker: the pose runs +x at 4.5 m/s and the ped stands exactly on it (heading -90 in GTA's convention)
struct Walker {
    LocomotionController lc;
    double x = 0;
    explicit Walker(const LocoConfig& c) : lc(c) {}
    LocoOutput Step(double dtMs, bool ragdoll = false, bool jumping = false) {
        x += 4.5 * dtMs / 1000.0;
        LocoInput in;
        in.pos = V(x, 0, 30); in.vel = V(4.5, 0, 0); in.heading = -90.0f;
        in.pedPos = V(x, 0, 30); in.pedHeading = -90.0f; in.pedGait = Gait::Run; in.dtMs = dtMs; in.moveState = 2;
        in.pedVel = V(4.5, 0, 0); in.havePedVel = true; in.noAnchor = true; in.ragdoll = ragdoll; in.jumping = jumping;
        return lc.Update(in);
    }
    void Run(double ms, bool ragdoll = false, bool jumping = false) { for (double t = 0; t < ms; t += 16.0) Step(16.0, ragdoll, jumping); }
};

static void TestTriggers() {
    // 8. What opens a window: Reset() from any caller, the end of a ragdoll, a landing, a Pause's end. What does not: a hard reset (a new ped), any mode but Blend, a controller without the self-check.
    Walker w(CfgNew());
    w.Run(3000);
    const uint32_t g0 = w.lc.Stats().graceWindows;
    w.Run(700, true);                       // ragdolling
    const uint32_t gr0 = w.lc.Stats().graceWindows;
    w.Run(100);                             // ... and up again
    const uint32_t gRag = w.lc.Stats().graceWindows;
    w.Run(3000);
    w.Run(400, false, true);                // a jump
    w.Run(1500);                            // ... and the landing
    const uint32_t gJump = w.lc.Stats().graceWindows;
    w.Run(3000);
    w.lc.Pause(500.0);
    w.Run(900);                             // the pause runs out: Reset()
    const uint32_t gPause = w.lc.Stats().graceWindows;
    const float left = w.lc.GraceLeftMs();
    w.Run(3000);
    w.lc.Reset();
    const uint32_t gReset = w.lc.Stats().graceWindows;
    Check("R5 triggers: no window while walking; the end of a ragdoll opens one, a landing one, a Pause's end one, a Reset() one (each exactly one), and a window is open (> 0 ms left) just after the pause",
          g0 == 0 && gr0 == 0 && gRag == 1 && gJump == 2 && gPause == 3 && gReset == 4 && left > 0.0f,
          F("windows: start %u, during the ragdoll %u, after it %u, after the jump %u, after the pause %u (%.0f ms left), after Reset %u", g0, gr0, gRag, gJump, gPause, left, gReset));
    Walker hard(CfgNew());
    hard.Run(2000);
    hard.lc.ResetHard();
    hard.lc.ResetForNewPed();
    hard.Run(500);
    Walker str(ShippedLocoConfig(LocoMode::Straight));
    str.Run(2000);
    str.lc.Reset();
    str.Run(500);
    Walker r4w(CfgOld());
    r4w.Run(2000);
    r4w.lc.Reset();
    r4w.Run(500);
    Check("R5 triggers: a hard reset / a new ped, a Reset() in Straight mode and a Reset() of the round-4 Blend open no window",
          hard.lc.Stats().graceWindows == 0 && str.lc.Stats().graceWindows == 0 && r4w.lc.Stats().graceWindows == 0 && hard.lc.GraceLeftMs() == 0.0f,
          F("hard %u, straight %u, round-4 %u", hard.lc.Stats().graceWindows, str.lc.Stats().graceWindows, r4w.lc.Stats().graceWindows));
}

static void TestHeadingStats() {
    // 9. Telemetry: the puppet's heading against the pose's, and against the way the pose and the puppet itself move.
    LocoDiag d;
    double x = 0, now = 0;
    for (int i = 0; i < 240; ++i) {
        LocoDiagFrame f;
        f.dtMs = 16.0; now += 16.0; f.nowMs = now;
        x += 4.0 * 0.016;
        f.pose = V(x + 1.0, 0, 30); f.pedPos = V(x, 0, 30); f.poseSpeed = 4.0f; f.poseVel = V(4.0, 0, 0);
        f.haveHeading = true;
        f.poseHeadingDeg = -90.0f;            // the sender faces +x ...
        f.pedHeading = -70.0f;                // ... the puppet is 20 degrees off ...
        f.gait = Gait::Run;
        d.Observe(f);                         // ... and walks along +x (heading -90) at 4 m/s
    }
    const LocoDiagSummary s = d.Summary();
    Check("R5 heading telemetry: a puppet 20 degrees off the sender's heading and walking along +x while facing -70: heading error mean and p95 20 (+-1.5), pose velocity vs the puppet's heading 20 (+-1.5), the "
          "puppet's own velocity vs its heading 20 (+-1.5), 240 / 240 / 239 frames",
          s.headFrames == 240 && s.poseVelHeadFrames == 240 && s.pedVelHeadFrames >= 238 && std::fabs(s.headErrMeanDeg - 20.0) < 1.5 && std::fabs(s.headErrP95Deg - 20.0) < 1.5 &&
              std::fabs(s.poseVelHeadMeanDeg - 20.0) < 1.5 && std::fabs(s.pedVelHeadMeanDeg - 20.0) < 1.5 && std::fabs(s.pedVelHeadP95Deg - 20.0) < 1.5,
          F("frames %u/%u/%u, heading error mean %.1f p95 %.1f, pose velocity %.1f/%.1f, own velocity %.1f/%.1f", s.headFrames, s.poseVelHeadFrames, s.pedVelHeadFrames, s.headErrMeanDeg, s.headErrP95Deg,
            s.poseVelHeadMeanDeg, s.poseVelHeadP95Deg, s.pedVelHeadMeanDeg, s.pedVelHeadP95Deg));
    LocoDiag e;
    for (int i = 0; i < 50; ++i) { LocoDiagFrame f; f.dtMs = 16; f.nowMs = 16.0 * i; f.pose = V(0, 0, 30); f.pedPos = V(0, 0, 30); f.poseSpeed = 4.0f; f.poseVel = V(4, 0, 0); e.Observe(f); }
    Check("R5 heading telemetry: frames without a heading read count nothing", e.Summary().headFrames == 0 && e.Summary().poseVelHeadFrames == 0 && e.Summary().pedVelHeadFrames == 0,
          F("%u/%u/%u", e.Summary().headFrames, e.Summary().poseVelHeadFrames, e.Summary().pedVelHeadFrames));
    // 0.4.30: a sender backpedalling (moving -y while facing +y, heading 0) reads 180 deg off-axis on every frame; the forward walk above reads 0;
    // ragdoll / jump / swim / aim frames are not heading frames (the 0.4.29 '128 deg' window was a ragdoll)
    LocoDiag bp;
    double y = 0, t = 0;
    for (int i = 0; i < 120; ++i) {
        LocoDiagFrame f;
        f.dtMs = 16.0; t += 16.0; f.nowMs = t;
        y -= 1.5 * 0.016;
        f.pose = V(0, y, 30); f.pedPos = V(0, y, 30); f.poseSpeed = 1.5f; f.poseVel = V(0, -1.5, 0);
        f.haveHeading = HeadingTelemetryFrame(false, false, false, false);
        f.poseHeadingDeg = 0.0f; f.pedHeading = 0.0f; f.gait = Gait::Walk;
        bp.Observe(f);
    }
    const LocoDiagSummary b = bp.Summary();
    const bool gate = HeadingTelemetryFrame(false, false, false, false) && !HeadingTelemetryFrame(true, false, false, false) &&
                      !HeadingTelemetryFrame(false, true, false, false) && !HeadingTelemetryFrame(false, false, true, false) && !HeadingTelemetryFrame(false, false, false, true);
    Check("0.4.30 heading telemetry: a backpedalling sender reads 180 deg off-axis on 120 of 120 frames (all over 60), the forward walk 0 (none over 60); "
          "ragdoll, jump, swim and aim frames are not heading frames",
          b.senderOffFrames == 120 && b.senderOffOver60 == 120 && std::fabs(b.senderOffMeanDeg - 180.0) < 0.5 && s.senderOffFrames == 240 && s.senderOffOver60 == 0 &&
              s.senderOffMeanDeg < 0.5 && gate,
          F("backpedal %u/%u mean %.1f, forward %u/%u mean %.1f, gate %d", b.senderOffOver60, b.senderOffFrames, b.senderOffMeanDeg, s.senderOffOver60, s.senderOffFrames,
            s.senderOffMeanDeg, (int)gate));
}

static void R5Tests() {
    std::printf("-- R5: the Blend self-check against task-changing events\n");
    if (std::getenv("R5_TABLE")) Table();
    TestEvents();
    TestPlaceOnce();
    TestStorm();
    TestHold();
    TestProbation();
    TestLevelCap();
    TestDegradeCap();
    TestConfig();
    TestTriggers();
    TestHeadingStats();
}

} // namespace r5
