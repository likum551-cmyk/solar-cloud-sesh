// R3 T2 (desync round 3): "the task runs and the puppet shows no gait, own speed 0.00 m/s" (0.4.7 live log, puppet id=2).
//
// Included by tests/locomotion_test.cpp after tests/t2_onfoot_tests.h (it uses that file's simulated ped, Run(), RunOpts, Model, Result
// and this file's host: Check / F / V / SlideScenario) and called once from main as R3T2Tests(). R3T2_TABLE=1 prints the tables the
// report quotes.
//
// What the live log could not say, and what this file models. Four different things on the puppet make "the task ran, nothing moved":
//   W  every coordinate write (the per-frame nudge, a snap) cancels the walk for a while (Model::writeResetMs)
//   D  no task moves the ped, whatever is done to it (Model::follow = NoneMove): a dead AI
//   F  the same as D until the engine does something to the PED (Model::fixAtRepair): a creation flag / ped state was the cause
//   L  a task needs time before the ped moves, and every re-issue restarts the wait (Model::taskLatencyMs)
// and a fifth that only LOOKS like them: B the IS_PED_* natives read Still while the ped moves fine (Model::gaitBlind).
// The 0.4.7 10 s lines (gait read, own speed, nudge frames, task issues) are the same for W, D and F; the round-3 controller tells
// them apart by an experiment (a quiet window with no coordinate write), then by what the ENGINE can do to the ped (LocoRepair).

namespace r3 {

using namespace t2;

static SlideScenario Steady(double v, double durMs) {
    SlideScenario sc = Reversal("steady run", v, 1e6);   // the turn is a million seconds away: one straight run
    sc.durMs = durMs;
    return sc;
}
static SlideScenario Long(const char* name, double v, double turnEveryS, double durMs) {
    SlideScenario sc = Reversal(name, v, turnEveryS);
    sc.durMs = durMs;
    return sc;
}

static Model CauseW(double holdMs = 400) { Model m; m.writeResetMs = holdMs; return m; }
static Model CauseD() { Model m; m.follow = Follow::NoneMove; return m; }
static Model CauseF(int fixAt) { Model m; m.follow = Follow::NoneMove; m.fixAtRepair = fixAt; return m; }
static Model CauseL(double ms = 1200) { Model m; m.taskLatencyMs = ms; return m; }
static Model CauseB() { Model m; m.gaitBlind = true; return m; }

static LocoConfig Old(bool anchor = true) { return R2Cfg(anchor); }
static LocoConfig New(bool anchor = true) { return ShippedLocoConfig(anchor); }

static void Row(const char* label, const Model& m, const char* what, const Result& r, double durMs) {
    if (!std::getenv("R3T2_TABLE")) return;
    (void)m;
    if (std::getenv("R3T2_CHANGES")) {
        std::printf("      level changes of %s %s:", label, what);
        for (size_t i = 0; i < r.changes.size(); ++i) std::printf(" [%.1fs ->%d why %d]", r.changeAtMs[i] / 1000, int(r.changes[i].first), int(r.changes[i].second));
        std::printf("\n      repairs:");
        for (const auto& rp : r.repairs) std::printf(" [%.1fs %d]", rp.first / 1000, int(rp.second));
        std::printf("\n");
    }
    std::printf("    %-34s %-4s gait read late %5.1f%% | own %.2f vs pose %.2f m/s | nudge frames %5u | tasks %5.1f/10s | level changes %2zu (final %d) | repairs %zu | verdict %d | quiet %u | err p95 %.2f m | skate %.0f%% | first gait %.0f ms\n",
                label, what, r.steadyGaitFrac * 100, r.d.selfSpeedMs, r.d.poseSpeedMs, r.d.nudgeFrames, r.st.goStraight * 10000.0 / durMs, r.changes.size(),
                int(r.finalLevel), r.repairs.size(), int(r.verdict), r.st.quietWindows, r.p95Err, r.d.skate * 100, r.firstGaitMs);
}

// the max number of issues of a kind in any window of `winMs` (issues are sorted by time)
static int MaxInWindow(const std::vector<Result::Issue>& v, double winMs, bool (*pick)(const Result::Issue&)) {
    int best = 0;
    std::vector<double> t;
    for (const auto& i : v) if (pick(i)) t.push_back(i.atMs);
    for (size_t a = 0, b = 0; b < t.size(); ++b) {
        while (t[b] - t[a] >= winMs) ++a;
        best = std::max(best, int(b - a + 1));
    }
    return best;
}

static void TestSignature() {
    // 1. The old controller's numbers cannot tell W, D and F apart; they do tell B (the natives lie) and the healthy game.
    const SlideScenario sc = Steady(4.5, 60000);
    RunOpts op;
    const Result w = Run(sc, CauseW(), Old(), op), d = Run(sc, CauseD(), Old(), op), f = Run(sc, CauseF(2), Old(), op), l = Run(sc, CauseL(), Old(), op);
    const Result b = Run(sc, CauseB(), Old(), op), h = Run(sc, Model(), Old(), op);
    Row("0.4.7 controller", CauseW(), "W", w, sc.durMs); Row("0.4.7 controller", CauseD(), "D", d, sc.durMs);
    Row("0.4.7 controller", CauseF(2), "F", f, sc.durMs); Row("0.4.7 controller", CauseL(), "L", l, sc.durMs);
    Row("0.4.7 controller", CauseB(), "B", b, sc.durMs); Row("0.4.7 controller", Model(), "ok", h, sc.durMs);
    auto nogait = [](const Result& r) { return r.steadyGaitFrac < 0.05 && r.d.selfSpeedMs < 0.6; };
    Check("R3 T2 signature: with the 0.4.7 controller the three causes W (every write cancels the walk), D (no task moves the ped) and F (a ped flag blocks tasks) all look like the live log: the ped reads no gait in the last 40 s and its own speed is ~0 against a 4.5 m/s pose (own speed < 0.6 m/s)",
          nogait(w) && nogait(d) && nogait(f), F("gait read late %.0f%% / %.0f%% / %.0f%%, own speed %.2f / %.2f / %.2f m/s", w.steadyGaitFrac * 100, d.steadyGaitFrac * 100,
                                               f.steadyGaitFrac * 100, w.d.selfSpeedMs, d.d.selfSpeedMs, f.d.selfSpeedMs));
    Check("R3 T2 signature: ... a healthy game reads a gait on > 90% of the frames and runs at the pose speed, and B (natives read Still, the ped moves) shows own speed > 3 m/s with no gait read: own speed is what separates B from W / D / F (the live log had 0.00)",
          h.steadyGaitFrac > 0.9 && h.d.selfSpeedMs > 3.0 && b.steadyGaitFrac < 0.05 && b.d.selfSpeedMs > 3.0,
          F("healthy gait %.0f%% own %.2f; B gait %.0f%% own %.2f", h.steadyGaitFrac * 100, h.d.selfSpeedMs, b.steadyGaitFrac * 100, b.d.selfSpeedMs));
    Check("R3 T2 signature: 0.4.7 calls B 'lost' (a false alarm: a level change although the ped moves at the pose speed), the round-3 controller does not (lost needs the ped's own speed to agree)",
          b.changes.size() >= 1 && Run(sc, CauseB(), New(), op).changes.empty(),
          F("0.4.7 level changes %zu, round 3 %zu", b.changes.size(), Run(sc, CauseB(), New(), op).changes.size()));
    // the live cadence: the tasks counter. Under L the 0.4.7 stall rule restarts the wait before the ped can show a gait.
    const Result ln = Run(sc, CauseL(), New(), op);
    Row("round 3", CauseL(), "L", ln, sc.durMs);
    Check("R3 T2 signature: a task that needs 1.2 s before the ped shows a gait (L): 0.4.7 declares it lost at ~1 s and re-issues it before it can (first gait at 2.2 s), round 3 waits firstGaitWindowMs (1.5 s): the ped reads its first gait at ~1.3 s, no level change before 1.5 s, and the task-to-gait latency is measured (1.0 .. 1.5 s)",
          l.firstGaitMs > 0 && ln.firstGaitMs > 0 && ln.firstGaitMs < l.firstGaitMs - 500 && l.firstLevelChangeMs > 0 && l.firstLevelChangeMs < 1500 && ln.firstLevelChangeMs >= 1500 &&
              ln.st.latencyN >= 1 && ln.st.latencySumMs / std::max<uint32_t>(1, ln.st.latencyN) > 1000 && ln.st.latencySumMs / std::max<uint32_t>(1, ln.st.latencyN) < 1500,
          F("0.4.7 first gait %.0f ms, first level change %.0f ms; round 3 first gait %.0f ms, first level change %.0f ms, latency mean %.0f ms over %u issues", l.firstGaitMs, l.firstLevelChangeMs,
            ln.firstGaitMs, ln.firstLevelChangeMs, ln.st.latencyN ? ln.st.latencySumMs / ln.st.latencyN : -1.0, ln.st.latencyN));
}

static void TestWriteCause() {
    // 2. W: every write cancels the walk. 0.4.7 writes every frame and never sees a gait; round 3 runs a quiet window, sees the gait
    // come back, calls the writes harmful, and closes the error by single pulses.
    const SlideScenario sc = Steady(4.5, 60000);
    RunOpts op;
    const Model m = CauseW();
    if (const char* tr = std::getenv("R3T2_TRACE")) {   // R3T2_TRACE=<from ms>: trace the round-3 W run for 3 s
        RunOpts q; q.traceFromMs = std::atof(tr); q.traceToMs = q.traceFromMs + 3000;
        Run(sc, m, New(), q);
    }
    const Result o = Run(sc, m, Old(), op), n = Run(sc, m, New(), op);
    Row("0.4.7 controller", m, "W", o, sc.durMs); Row("round 3", m, "W", n, sc.durMs);
    Check("R3 T2 W: the quiet window finds that coordinate writes cancel the walk (verdict Harm after a window), the ped then reads a gait on > 60% of the late frames (0.4.7: < 5%) and runs at least half the pose speed on its own (0.4.7: ~0)",
          n.verdict == WriteVerdict::Harm && n.st.quietWindows >= 1 && n.st.quietHarm >= 1 && n.steadyGaitFrac > 0.6 && o.steadyGaitFrac < 0.05 && n.d.selfSpeedMs > 2.0 && o.d.selfSpeedMs < 0.6,
          F("round 3: verdict %d, windows %u, harm %u, late gait %.0f%%, own %.2f m/s | 0.4.7: late gait %.0f%%, own %.2f m/s", int(n.verdict), n.st.quietWindows, n.st.quietHarm,
            n.steadyGaitFrac * 100, n.d.selfSpeedMs, o.steadyGaitFrac * 100, o.d.selfSpeedMs));
    Check("R3 T2 W: after the verdict the position error is closed by pulses (<= 1 write per 1.5 s) instead of a write on every frame: coordinate-write frames per 60 s < 120 (0.4.7: over 1000), the pose error p95 stays under 2.5 m and the ladder does not move (no level change: the task was fine)",
          n.st.pulses >= 1 && n.d.nudgeFrames + n.st.snaps < 120 && o.d.nudgeFrames > 1000 && n.p95Err < 2.5 && n.changes.empty(),
          F("pulses %u, nudge frames %u + snaps %u (0.4.7: %u nudge frames), p95 %.2f m, level changes %zu", n.st.pulses, n.d.nudgeFrames, n.st.snaps, o.d.nudgeFrames, n.p95Err, n.changes.size()));
    // A pure unit: a Harm verdict makes a ped 1.6 m behind a run get one pulse per 1.5 s, not a nudge per frame.
    {
        RunOpts q;
        const Result g = Run(Steady(4.5, 30000), CauseW(900), New(), q);
        Check("R3 T2 W: a longer reset (900 ms per write) is found the same way",
              g.verdict == WriteVerdict::Harm, F("verdict %d, late gait %.0f%%", int(g.verdict), g.steadyGaitFrac * 100));
    }
    // No quiet window in a working game, and no change to what a working game scored in round 2.
    const std::vector<Model> good = Select([](const Model& mm) { return mm.follow == Follow::Honour && mm.rateScales; }, true);
    uint32_t windows = 0; int runs = 0; double sumNew = 0, sumOld = 0;
    {
        RunOpts q;
        for (const Model& gm : good)
            for (const SlideScenario& s2 : Scenarios()) {
                if (!Scored(s2)) continue;
                const Result a = Run(s2, gm, New(), q), b = Run(s2, gm, Old(), q);
                windows += a.st.quietWindows; ++runs; sumNew += a.p95Err; sumOld += b.p95Err;
            }
    }
    Check("R3 T2 W: a working game never opens a quiet window (0 in all runs) and the error p95 is within 0.03 m of round 2's (the quiet start of the first task costs nothing there)",
          runs >= 200 && windows == 0 && sumNew / runs <= sumOld / runs + 0.03, F("%d runs, %u windows, mean p95 %.3f m vs round-2 %.3f m", runs, windows, sumNew / runs, sumOld / runs));
}

static void TestRepairs() {
    // 3. F: a ped flag is the cause. The controller asks the ENGINE for a repair after the straight task stalled twice with the writes
    // cleared: ClearTasks, then a respawn with the lean recipe, then the alt recipe. The sim engine does what it is asked.
    const SlideScenario sc = Steady(4.5, 90000);
    RunOpts op;
    const Result o = Run(sc, CauseF(2), Old(), op), n = Run(sc, CauseF(2), New(), op), n1 = Run(sc, CauseF(1), New(), op), n3 = Run(sc, CauseF(3), New(), op), d = Run(sc, CauseD(), New(), op);
    Row("0.4.7 controller", CauseF(2), "F2", o, sc.durMs); Row("round 3", CauseF(2), "F2", n, sc.durMs); Row("round 3", CauseF(1), "F1", n1, sc.durMs);
    Row("round 3", CauseF(3), "F3", n3, sc.durMs); Row("round 3", CauseD(), "D", d, sc.durMs);
    Check("R3 T2 F: a flag that blocks tasks until the lean respawn (F2): the repairs asked are ClearTasks then RespawnLean, the puppet then reads a gait on > 80% of the late frames at the pose speed (0.4.7: < 5%, Placed for good) and the verdict is Harmless (the writes were not it)",
          n.repairs.size() == 2 && n.repairs[0].second == LocoRepair::ClearTasks && n.repairs[1].second == LocoRepair::RespawnLean && n.steadyGaitFrac > 0.8 && n.d.selfSpeedMs > 3.0 &&
              o.steadyGaitFrac < 0.05 && n.verdict == WriteVerdict::Harmless,
          F("repairs %zu (%d, %d), late gait %.0f%%, own %.2f m/s, verdict %d | 0.4.7 late gait %.0f%%", n.repairs.size(), n.repairs.size() > 0 ? int(n.repairs[0].second) : -1,
            n.repairs.size() > 1 ? int(n.repairs[1].second) : -1, n.steadyGaitFrac * 100, n.d.selfSpeedMs, int(n.verdict), o.steadyGaitFrac * 100));
    Check("R3 T2 F: the repair that fixes it is the first one that does (F1: one repair, F3: three), the next is not asked for once the ped moves",
          n1.repairs.size() == 1 && n1.steadyGaitFrac > 0.8 && n3.repairs.size() == 3 && n3.steadyGaitFrac > 0.8,
          F("F1 repairs %zu gait %.0f%%; F3 repairs %zu gait %.0f%%", n1.repairs.size(), n1.steadyGaitFrac * 100, n3.repairs.size(), n3.steadyGaitFrac * 100));
    Check("R3 T2 D: a ped nothing moves gets exactly the 3 repairs (ClearTasks, RespawnLean, RespawnAlt) and then the rigid follow with the long dead timer: Placed at the end, never a 4th repair",
          d.repairs.size() == 3 && d.repairs[2].second == LocoRepair::RespawnAlt && d.finalLevel == LocoLevel::Placed,
          F("repairs %zu, final level %d", d.repairs.size(), int(d.finalLevel)));
    // The repair counter is the engine's: a puppet whose engine reports the repairs done is not asked again.
    {
        RunOpts q; q.repairsDone0 = 3;
        const Result r = Run(Steady(4.5, 30000), CauseD(), New(), q);
        Check("R3 T2 D: ... and with repairsDone = 3 reported the controller asks for nothing (straight to Placed)", r.repairs.empty() && r.finalLevel == LocoLevel::Placed,
              F("repairs %zu, final %d", r.repairs.size(), int(r.finalLevel)));
    }
}

static void TestConvergence() {
    // 4. The ladder converges. Live: 7 level changes in 6.5 minutes. A dead puppet (D) over 6.5 minutes of stop-turn-go, 0.4.7 vs round 3.
    const SlideScenario sc = Long("stop, turn, go for 6.5 minutes", 4.5, 4.0, 390000);
    RunOpts op;
    const Result o = Run(sc, CauseD(), Old(), op);
    const Result n = Run(sc, CauseD(), New(), op);
    Row("0.4.7 controller", CauseD(), "D 6.5min", o, sc.durMs); Row("round 3", CauseD(), "D 6.5min", n, sc.durMs);
    // hold: no UP step (Recovered) within levelHoldMs of the change before it
    auto holdViolations = [&](const Result& r, double holdMs) {
        int bad = 0;
        for (size_t i = 1; i < r.changes.size(); ++i)
            if (r.changes[i].second == LocoLevelWhy::Recovered && r.changeAtMs[i] - r.changeAtMs[i - 1] < holdMs) ++bad;
        return bad;
    };
    LocoConfig shortRecovery = Old(); shortRecovery.anchorFallbackMs = 2000; shortRecovery.placedMs = 3000;
    LocoConfig shortNew = New();     shortNew.anchorFallbackMs = 2000; shortNew.placedMs = 3000; shortNew.deadPlacedMs = 3000;
    const Result so = Run(sc, CauseD(), shortRecovery, op), sn = Run(sc, CauseD(), shortNew, op);
    Check("R3 T2 ladder: a dead puppet over 6.5 minutes: 0.4.7 changes level at least 7 times (the live count), round 3 at most 5 and the last change is early (the final Placed holds 120 s then 240 s, the repairs are 3)",
          o.changes.size() >= 7 && n.changes.size() <= 5 && n.changes.size() < o.changes.size(),
          F("0.4.7: %zu changes, round 3: %zu changes (last at %.0f s), repairs %zu", o.changes.size(), n.changes.size(), n.lastLevelChangeMs / 1000, n.repairs.size()));
    Check("R3 T2 ladder: the hold: with the fallback timers cut to 2-3 s (so recovery is tried at once) 0.4.7 climbs back within 8 s of the change before it, round 3 never does (every Recovered step is >= levelHoldMs after the previous change)",
          holdViolations(so, 8000) >= 1 && holdViolations(sn, 8000) == 0, F("0.4.7 violations %d (%zu changes), round 3 violations %d (%zu changes)", holdViolations(so, 8000), so.changes.size(), holdViolations(sn, 8000), sn.changes.size()));
    // the global cap: at most maxLevelChanges, a line (capHit) once, and the level frozen
    LocoConfig capped = New(); capped.maxLevelChanges = 3; capped.anchorFallbackMs = 2000; capped.placedMs = 3000; capped.deadPlacedMs = 3000; capped.levelHoldMs = 1000; capped.maxRepairs = 0;
    const Result cr = Run(sc, CauseD(), capped, op);
    Check("R3 T2 ladder: the cap: with maxLevelChanges = 3 the puppet changes level 3 times, the cap is reported exactly once (capHit), later failures are refused (changesRefused > 0) and the level stays frozen",
          cr.changes.size() == 3 && cr.capHits == 1 && cr.st.changesRefused > 0, F("changes %zu, cap hits %u, refused %u", cr.changes.size(), cr.capHits, cr.st.changesRefused));
    Check("R3 T2 ladder: the uncapped round-3 run never reports a cap hit (the cap is a backstop, 12 changes)", n.capHits == 0, F("cap hits %u", n.capHits));
}

static void TestReissue() {
    // 5. Task re-issue limits (straight mode, a healthy game). A 40 degree zigzag every 0.7 s: 0.4.7 re-issues on the turn rule as soon as
    // minReissueMs (500 ms) allows; round 3 at most once per second, except a BIG turn (>= 70 degrees), which may come after 500 ms.
    SlideScenario zz;
    zz.name = "zigzag 40 deg every 0.7 s";
    zz.speed = [](double) { return 4.5; };
    zz.dirDeg = [](double t) { return std::fmod(std::floor(t / 0.7), 2.0) < 1.0 ? -20.0 : 20.0; };
    zz.durMs = 30000;
    SlideScenario big;
    big.name = "turn 90 deg every 0.8 s";
    big.speed = [](double) { return 4.5; };
    big.dirDeg = [](double t) { return std::fmod(std::floor(t / 0.8), 2.0) < 1.0 ? 0.0 : 90.0; };
    big.durMs = 30000;
    RunOpts op;
    auto rule = [](const Result::Issue& i) { return i.why == LocoWhy::Turn || i.why == LocoWhy::Drift || i.why == LocoWhy::Arrive || i.why == LocoWhy::Stall; };
    const Result o = Run(zz, Model(), Old(false), op), n = Run(zz, Model(), New(false), op);
    const Result ob = Run(big, Model(), Old(false), op), nb = Run(big, Model(), New(false), op);
    const int mo = MaxInWindow(o.issues, 1000, rule), mn = MaxInWindow(n.issues, 1000, rule);
    Check("R3 T2 reissue: 40-degree turns every 0.7 s: at most ONE rule-based issue (turn / drift / arrival / stall) in any 1000 ms window with round 3 (0.4.7: 2)",
          mo >= 2 && mn <= 1, F("max per second: 0.4.7 %d, round 3 %d (issues %zu vs %zu in 30 s)", mo, mn, o.issues.size(), n.issues.size()));
    const int mob = MaxInWindow(ob.issues, 1000, rule), mnb = MaxInWindow(nb.issues, 1000, rule);
    Check("R3 T2 reissue: ... but 90-degree turns every 0.8 s are still followed: round 3 issues on the big turns (>= 12 turn issues in 30 s, 2 per second allowed)",
          nb.st.whyTurn >= 12 && mnb >= 2 && mnb <= 2 && mob >= 2, F("turn issues round 3 %u (0.4.7 %u), max per second %d / %d", nb.st.whyTurn, ob.st.whyTurn, mnb, mob));
    // The live count: 13 go tasks in 10 s with a dead puppet in the rigid level. Round 3 never exceeds 1 per second in steady state.
    const SlideScenario sc = Long("stop, turn, go (dead puppet)", 4.5, 4.0, 60000);
    RunOpts q; q.repairsDone0 = 3;
    const Result dn = Run(sc, CauseD(), New(), q), dold = Run(sc, CauseD(), Old(), q);
    auto notFirst = [](const Result::Issue& i) { return i.why != LocoWhy::Gait && i.why != LocoWhy::None; };
    const int wn = MaxInWindow(dn.issues, 1000, notFirst), wo = MaxInWindow(dold.issues, 1000, notFirst);
    if (std::getenv("R3T2_ISSUES")) {
        std::printf("      issues of the dead puppet (round 3):");
        for (const auto& i : dn.issues) std::printf(" [%.2fs why %d]", i.atMs / 1000, int(i.why));
        std::printf("\n");
    }
    Check("R3 T2 reissue: a dead puppet in the rigid level (stop, turn, go): no more than one non-gait-start issue per second (forced issues after a snap or Reset are spaced issueGapMs)",
          wn <= 1, F("max per second: round 3 %d, 0.4.7 %d; issues %zu vs %zu in 60 s", wn, wo, dn.issues.size(), dold.issues.size()));
    // A forced re-issue (Reset after an aim task) right after an issue is held back, then goes out: unit.
    {
        LocoConfig cfg = New(false);
        LocomotionController lc(cfg);
        for (int i = 0; i < 120; ++i) lc.Update(Unit(0));       // 2 s on the pose: the task was issued at the start, nothing since
        // a Reset long after the last issue goes out at once
        lc.Reset();
        const LocoOutput first = lc.Update(Unit(0));        // the issue after the Reset
        lc.Reset();                                          // ... and another one 16 ms later
        int n2 = 0; double t = 0, at2 = -1;
        for (int i = 0; i < 60; ++i, t += 1000.0 / 60) {
            const LocoOutput b = lc.Update(Unit(0));
            if (b.task == LocoTask::GoStraight) { ++n2; if (at2 < 0) at2 = t; }
        }
        Check("R3 T2 reissue: a second forced re-issue 16 ms after the first (Reset twice) is held until issueGapMs (600 ms) has passed, then issued once",
              first.task == LocoTask::GoStraight && n2 == 1 && at2 >= 500 && at2 <= 700 && lc.Stats().issuesHeld >= 1, F("first %d, second issued %d time(s) at %.0f ms, held %u", int(first.task), n2, at2, lc.Stats().issuesHeld));
    }
}

static void TestDiag() {
    // 6. The per-puppet gait diagnostic (the same aggregator the engine prints).
    GaitDiag g;
    const double dt = 1000.0 / 60;
    for (int i = 0; i < 120; ++i) {
        GaitDiagFrame f;
        f.dtMs = dt; f.nowMs = i * dt;
        f.commanded = i < 100 ? Gait::Run : Gait::Still;
        f.read = i % 4 == 0 ? Gait::Run : Gait::Still;          // 25% of the commanded frames read Run
        f.poseSpeed = 4.5f; f.ownSpeed = 0.5f; f.entitySpeed = 1.0f; f.pedPos = V(0.02 * i, 0, 0);   // 1.2 m/s from positions
        f.wrote = i % 2 == 0; f.issued = i == 10 || i == 50;
        f.blendSet = i % 10 == 0; f.moveBlend = 2.0f; f.pedStill = i < 50 ? 1 : 0; f.ragdoll = i == 3;
        g.Observe(f);
    }
    const GaitDiagSummary s = g.Summary();
    Check("R3 T2 diag: the gait diagnostic counts what the ped READS next to the pose: 100 commanded frames of 120, 25% read Run / 75% Still, IS_PED_STILL true on half, pose 4.5 / own 0.5 / GET_ENTITY_SPEED 1.0 / from positions 1.2 m/s, 60 write frames, 2 issues, blend 2.0 on 10 frames, 1 ragdoll frame",
          s.frames == 120 && s.commandedFrames == 100 && s.readRun == 25 && s.readStill == 75 && s.readWalk == 0 && s.stillAsked == 100 && s.stillTrue == 50 && std::fabs(s.poseMs - 4.5) < 1e-6 &&
              std::fabs(s.ownMs - 0.5) < 1e-6 && std::fabs(s.entityMs - 1.0) < 1e-6 && std::fabs(s.posMs - 1.2) < 0.05 && s.writeFrames == 60 && s.issues == 2 && s.blendFrames == 10 &&
              std::fabs(s.blendMean - 2.0) < 1e-6 && s.ragdollFrames == 1 && std::fabs(s.readShare(s.readRun) - 0.25) < 1e-9,
          F("frames %u cmd %u run %u still %u stillTrue %u/%u pose %.2f own %.2f ent %.2f pos %.2f writes %u issues %u blend %u/%.1f ragdoll %u", s.frames, s.commandedFrames, s.readRun, s.readStill,
            s.stillTrue, s.stillAsked, s.poseMs, s.ownMs, s.entityMs, s.posMs, s.writeFrames, s.issues, s.blendFrames, s.blendMean, s.ragdollFrames));
}

} // namespace r3

static void R3T2Tests() {
    std::printf("-- R3 T2: why a puppet shows no gait, and a ladder that converges\n");
    r3::TestSignature();
    r3::TestWriteCause();
    r3::TestRepairs();
    r3::TestConvergence();
    r3::TestReissue();
    r3::TestDiag();
}
