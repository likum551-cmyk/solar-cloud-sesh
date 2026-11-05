// R5 punches (melee_loop): the swing plan, the ladder and the explicit end of a looped clip (libs/sync/swing.h), against a puppet whose behaviour is a model.
// Included by gameplay_test.cpp after Check() is defined. It mirrors IssuePeerSwing / PeerSwingService (apps/engine/engine.cpp) with the real classes.
//
// The live log (0.4.26, two players): 8 swings issued, 7 SEEN playing at the first frame, 0 still playing at +100 / +250 ms, 4 BLENDED OUT, nothing visible.
// The model "live" below makes exactly that true: any clip that is not looped is taken off the ped 70 ms after the issue. A looped clip (flags 49) is not
// subject to it, so nothing can end it but us: STOP_ANIM_TASK at +480 ms (after the +450 ms read-back), read back, and the puppet's tasks cleared when the
// stop did nothing or could not be read. The OLD order (Upper first) must show the live defect on this model; the new one must show the clip on the ped.
#pragma once

#include "sync/swing.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace r5swing {

using namespace gage::sync;

constexpr int kChecks = 12;   // asserted by gameplay_test's total

struct Model
{
    const char* name = "";
    double blendOutMs = 70.0;     // a clip that is NOT looped is taken off the ped this long after the issue (0 = it plays to its end)
    bool   loopPlays = true;      // a looped clip (flags 49) plays at all
    bool   stopWorks = true;      // STOP_ANIM_TASK ends THAT clip (after stopBlendMs)
    bool   readable = true;       // IS_ENTITY_PLAYING_ANIM can be read
    double clipLenMs = 1000.0;
    double stopBlendMs = 50.0;
};

constexpr double kNever = 1e18;

struct Sim
{
    Model game;
    bool loop;
    SwingLadder ladder;
    SwingSurvey survey;
    SwingProbe probe;
    SwingLoopStop stop;
    SwingDiag diag;
    SwingAttempt att;
    int curPlan = -1;
    double issuedMs = 0, endsAtMs = kNever;
    uint32_t truthVisible = 0, swings = 0, cleared = 0;
    double loopRealMsMax = 0;
    int winner = -2;
    int at450Playing = -2;        // the +450 ms read-back of the last followed swing
    std::vector<int> variants;

    Sim(const Model& m, bool lp, bool surveyOn) : game(m), loop(lp)
    {
        ladder.SetMode(SwingMode::Auto, lp);
        if (surveyOn)
            survey.Start(1, lp);
    }

    bool PlayingAt(double now) const
    {
        const double age = now - issuedMs;
        if (age < 0)
            return false;
        if (att.variant == SwingVariant::Looped)
            return game.loopPlays && now < endsAtMs;
        if (att.variant == SwingVariant::MeleeTask)
            return false;                                    // no anchor ped in this sim
        if (game.blendOutMs > 0 && age >= game.blendOutMs)
            return false;
        const double dur = att.durationMs < 0 ? game.clipLenMs : (double)att.durationMs;
        return age < dur;
    }

    void Settle(bool good)
    {
        if (curPlan >= 0)
            return;
        if (good)
            ladder.OnResult(SwingProbe::Result::Shown, false);
        else if (ladder.OnResult(SwingProbe::Result::NotShown, false))
            ++diag.escalations;
    }
    void SurveyFinish()
    {
        const int w = survey.TakeWinner();
        if (w == -2)
            return;
        winner = w;
        if (w >= 0)
            ladder.PinTo(SurveyPlanAt((size_t)w, loop).variant);
    }

    void Swing(double t0)
    {
        ++swings;
        ++diag.seen;
        att = SwingAttempt();
        att.variant = ladder.Current();
        att.durationMs = 400;
        size_t pi = (size_t)-1;
        if (survey.Active())
        {
            const SwingAttempt sa = survey.Next(false, &pi);
            if (pi != (size_t)-1)
                att = sa;
            else
                SurveyFinish();
        }
        // the same adjustment IssuePeerSwing makes
        if (att.variant == SwingVariant::Looped)
        {
            att.durationMs = kSwingClipLength;
            att.stopAfterMs = kLoopStopAfterMs;
        }
        else if (att.variant == SwingVariant::Plain)
            att.durationMs = kSwingClipLength;
        curPlan = pi != (size_t)-1 ? (int)pi : -1;
        ++diag.issued;
        ++diag.variantIssued[(size_t)att.variant];
        if (curPlan >= 0)
            ++diag.surveyed;
        variants.push_back((int)att.variant);
        issuedMs = t0;
        endsAtMs = kNever;
        if (PlayingAt(t0 + 112) && PlayingAt(t0 + 256))
            ++truthVisible;                                  // the sim's truth: on the ped at the +100 / +250 ms read-backs
        probe.Begin(t0, 0, true, att.durationMs, att.stopAfterMs > 0 ? att.stopAfterMs + 60.0 : -1.0);
        if (att.stopAfterMs > 0)
            stop.Arm(t0, att.stopAfterMs);
        else
            stop.Cancel();
        bool probeDone = false;
        for (double now = t0 + 16; now <= t0 + 1400; now += 16)
        {
            if (stop.Armed())
            {
                if (stop.Due(now))
                {
                    ++diag.loopStops;
                    if (game.stopWorks)
                        endsAtMs = now + game.stopBlendMs;
                    stop.Stopped(now);
                }
                else if (stop.Awaiting())
                {
                    const int rd = game.readable ? (PlayingAt(now) ? 1 : 0) : -1;
                    const SwingLoopStop::Outcome lo = stop.Check(now, rd);
                    if (lo == SwingLoopStop::Outcome::Gone)
                        ++diag.loopGone;
                    else if (lo == SwingLoopStop::Outcome::StillPlaying || lo == SwingLoopStop::Outcome::Unread)
                    {
                        ++(lo == SwingLoopStop::Outcome::StillPlaying ? diag.loopStill : diag.loopUnread);
                        endsAtMs = std::min(endsAtMs, now);   // ClearPedTasks
                        ++cleared;
                    }
                }
            }
            if (!probeDone)
            {
                const int playing = game.readable ? (PlayingAt(now) ? 1 : 0) : -1;
                const SwingProbe::Result r = probe.Tick(now, playing, -1.0f, -1.0f);
                if (r == SwingProbe::Result::Shown)
                {
                    ++diag.shown;
                    ++diag.variantShown[(size_t)att.variant];
                }
                else if (r == SwingProbe::Result::Followed)
                {
                    const SwingProbe::Progress pv = probe.Verdict();
                    at450Playing = probe.At(2).playing;
                    if (pv == SwingProbe::Progress::Progressed) { ++diag.progressed; ++diag.variantProgressed[(size_t)att.variant]; }
                    else if (pv == SwingProbe::Progress::BlendedOut) ++diag.blendedOut;
                    if (curPlan >= 0) { survey.OnResult((size_t)curPlan, true, pv, false); curPlan = -1; SurveyFinish(); }
                    else Settle(pv == SwingProbe::Progress::Progressed || pv == SwingProbe::Progress::Unread);
                    probeDone = true;
                }
                else if (r == SwingProbe::Result::NotShown)
                {
                    ++diag.unseen;
                    if (curPlan >= 0) { survey.OnResult((size_t)curPlan, false, SwingProbe::Progress::None, false); curPlan = -1; SurveyFinish(); }
                    else Settle(false);
                    probeDone = true;
                }
                else if (r == SwingProbe::Result::Unknown)
                {
                    ++diag.unreadable;
                    if (curPlan >= 0) { survey.OnResult((size_t)curPlan, false, SwingProbe::Progress::None, true); curPlan = -1; SurveyFinish(); }
                    probeDone = true;
                }
                else if (r == SwingProbe::Result::Idle)
                    probeDone = true;
            }
            if (probeDone && !stop.Armed())
                break;
        }
        if (att.variant == SwingVariant::Looped)
            loopRealMsMax = std::max(loopRealMsMax, std::min(endsAtMs, t0 + 1400.0) - t0);
    }
    void Swings(int n, double spacingMs = 3000.0)
    {
        for (int i = 0; i < n; ++i)
            Swing(1000.0 + i * spacingMs);
    }
    uint32_t Looped() const
    {
        uint32_t n = 0;
        for (int v : variants) n += v == (int)SwingVariant::Looped;
        return n;
    }
};

inline Model Live() { Model m; m.name = "live (a clip that is not looped is gone 70 ms after the issue)"; return m; }
inline Model Healthy() { Model m; m.name = "healthy (clips play to their end)"; m.blendOutMs = 0; return m; }

inline void RunPure()
{
    // 1. the plan: the looped clip first when melee_loop is on, the round-4 plan byte for byte when it is off
    {
        const SwingVariant want[7] = { SwingVariant::Looped, SwingVariant::Plain, SwingVariant::Upper, SwingVariant::Upper, SwingVariant::UpperHard, SwingVariant::FullBody, SwingVariant::MeleeTask };
        const int wantDur[7] = { -1, -1, 400, -1, 400, -1, 400 };
        bool newOk = SurveyPlanSize(true) == 7;
        for (size_t i = 0; i < 7; ++i)
        {
            const SwingAttempt a = SurveyPlanAt(i, true);
            newOk = newOk && a.variant == want[i] && a.durationMs == wantDur[i] && a.stopAfterMs == (i == 0 ? kLoopStopAfterMs : 0);
        }
        const SwingVariant oldWant[5] = { SwingVariant::Upper, SwingVariant::Upper, SwingVariant::UpperHard, SwingVariant::FullBody, SwingVariant::MeleeTask };
        const int oldDur[5] = { 400, -1, 400, -1, 400 };
        bool oldOk = SurveyPlanSize(false) == 5;
        for (size_t i = 0; i < 5; ++i)
        {
            const SwingAttempt a = SurveyPlanAt(i, false);
            const SwingAttempt b = SurveyPlanAt(i);
            oldOk = oldOk && a.variant == oldWant[i] && a.durationMs == oldDur[i] && a.stopAfterMs == 0 && b.variant == a.variant && b.durationMs == a.durationMs;
        }
        Check("R5 plan: melee_loop on = Looped (clip's length, stopped by us at +480 ms), Plain, Upper 400, Upper whole, UpperHard 400, FullBody whole, melee task; off = the round-4 five, "
              "unchanged; the stop sits after the +450 ms read-back; flags Looped 49, Plain 0, Upper 48 unchanged",
              newOk && oldOk && kLoopStopAfterMs > (int)SwingProbe::kSampleAtMs[2] && SwingVariantFlags(SwingVariant::Looped) == 49 && SwingVariantFlags(SwingVariant::Plain) == 0 &&
              SwingVariantFlags(SwingVariant::Upper) == 48 && SwingVariantFlags(SwingVariant::UpperHard) == 56 && SwingVariantFlags(SwingVariant::FullBody) == 8,
              std::string("new ") + (newOk ? "ok" : "WRONG") + ", old " + (oldOk ? "ok" : "WRONG"));
    }
    // 2. the ladder: Looped -> Plain -> Upper -> UpperHard -> FullBody -> MeleeTask, two misses each; the old ladder starts at Upper
    {
        SwingLadder l;
        l.SetMode(SwingMode::Auto, true);
        const bool start = l.Current() == SwingVariant::Looped && l.Loop();
        SwingVariant seen[6];
        seen[0] = l.Current();
        int n = 1;
        for (int i = 0; i < 12 && n < 6; ++i)
            if (l.OnResult(SwingProbe::Result::NotShown, false))
                seen[n++] = l.Current();
        const bool chain = n == 6 && seen[1] == SwingVariant::Plain && seen[2] == SwingVariant::Upper && seen[3] == SwingVariant::UpperHard && seen[4] == SwingVariant::FullBody &&
                           seen[5] == SwingVariant::MeleeTask && l.Escalations() == 5;
        SwingLadder o;
        o.SetMode(SwingMode::Auto);
        const bool oldStart = o.Current() == SwingVariant::Upper && !o.Loop();
        o.OnResult(SwingProbe::Result::NotShown, false);
        o.OnResult(SwingProbe::Result::NotShown, false);
        const bool oldNext = o.Current() == SwingVariant::UpperHard;
        Check("R5 ladder: melee_loop on starts at Looped and climbs Plain, Upper, UpperHard, FullBody, MeleeTask (2 misses a rung, 5 escalations); off starts at Upper and climbs to UpperHard as before",
              start && chain && oldStart && oldNext, std::string("start ") + (start ? "ok" : "WRONG") + ", chain " + (chain ? "ok" : "WRONG") + ", old start " + (oldStart ? "ok" : "WRONG") +
              ", old next " + (oldNext ? "ok" : "WRONG"));
    }
    // 3. play mode and the breaker with the loop: no melee rung in play; a tripped breaker goes back to the FIRST animation (Looped)
    {
        SwingLadder p;
        p.SetMode(SwingMode::Play, true);
        for (int i = 0; i < 20; ++i)
            p.OnResult(SwingProbe::Result::NotShown, false);
        const bool playEnd = p.Current() == SwingVariant::FullBody && p.Exhausted() > 0;
        SwingLadder a;
        a.SetMode(SwingMode::MeleeTask, true);
        const bool meleeOnly = a.Current() == SwingVariant::MeleeTask;
        a.TripMeleeTask();
        const bool back = a.Current() == SwingVariant::Looped && a.MeleeTripped();
        SwingLadder b;
        b.SetMode(SwingMode::Auto, true);
        b.PinTo(SwingVariant::Plain);
        const bool pin = b.Current() == SwingVariant::Plain && b.Pinned();
        Check("R5 ladder: play mode stops at FullBody (no melee rung), melee_task mode starts at the melee rung and a tripped breaker falls back to Looped, a survey pin to Plain sticks",
              playEnd && meleeOnly && back && pin, std::string("play ") + (playEnd ? "ok" : "WRONG") + ", melee-only " + (meleeOnly ? "ok" : "WRONG") + ", trip " + (back ? "ok" : "WRONG") +
              ", pin " + (pin ? "ok" : "WRONG"));
    }
    // 4. SwingLoopStop: due at the armed time, the read-back after it, the three outcomes
    {
        SwingLoopStop s;
        s.Arm(1000.0, 480);
        const bool dueEdge = !s.Due(1479.0) && s.Due(1480.0) && s.Armed() && !s.Awaiting() && s.Check(1500.0, 1) == SwingLoopStop::Outcome::Idle;
        s.Stopped(1480.0);
        const bool pending = s.Awaiting() && !s.Due(1600.0) && s.Check(1500.0, 1) == SwingLoopStop::Outcome::Pending && s.Armed();
        const bool gone = s.Check(1516.0, 0) == SwingLoopStop::Outcome::Gone && !s.Armed();
        SwingLoopStop t;
        t.Arm(0.0, 480);
        t.Stopped(480.0);
        const bool still = t.Check(500.0, 1) == SwingLoopStop::Outcome::Pending && t.Check(560.0, 1) == SwingLoopStop::Outcome::StillPlaying && !t.Armed();
        SwingLoopStop u;
        u.Arm(0.0, 480);
        u.Stopped(480.0);
        const bool unread = u.Check(500.0, -1) == SwingLoopStop::Outcome::Pending && u.Check(570.0, -1) == SwingLoopStop::Outcome::Unread && !u.Armed();
        SwingLoopStop v;
        v.Arm(0.0, 0);
        const bool none = !v.Armed();
        v.Arm(0.0, 480);
        v.Cancel();
        Check("R5 loop stop: Due exactly at the armed time, Pending while the clip still shows inside 80 ms of the stop, Gone when it is off, StillPlaying / Unread after 80 ms, a zero time arms nothing, Cancel disarms",
              dueEdge && pending && gone && still && unread && none && !v.Armed(), std::string("due ") + (dueEdge ? "ok" : "WRONG") + ", pending " + (pending ? "ok" : "WRONG") + ", gone " + (gone ? "ok" : "WRONG") +
              ", still " + (still ? "ok" : "WRONG") + ", unread " + (unread ? "ok" : "WRONG") + ", none " + (none ? "ok" : "WRONG"));
    }
    // 5. the live model, survey on, melee_loop on: the looped clip wins and stays visible; it is ended by us once per looped swing, within ~600 ms
    std::string diagLine;
    {
        Sim s(Live(), true, true);
        s.Swings(6);
        const bool surveyDone = s.winner == 0 && s.ladder.Current() == SwingVariant::Looped && s.ladder.Pinned();
        const uint32_t afterSurvey = s.diag.loopStops;
        s.Swings(2);                                          // two ordinary swings with the pinned ladder
        diagLine = SwingDiagLine(s.diag, SwingDiag(), s.ladder);
        Check("R5 loop on the live model: the survey's winner is the LOOPED clip (plan 0) and the ladder pins to it; it is on the ped at +100 / +250 / +450 ms; STOP_ANIM_TASK is called once per looped swing "
              "(1 in the survey, 3 with the two pinned swings), every one read back GONE, the clip really ended by 600 ms",
              surveyDone && afterSurvey == 1 && s.diag.loopStops == 3 && s.diag.loopGone == 3 && s.diag.loopStill == 0 && s.diag.loopUnread == 0 && s.Looped() == 3 && s.truthVisible == 3 &&
              s.at450Playing == 1 && s.loopRealMsMax >= 480.0 && s.loopRealMsMax <= 600.0 && s.cleared == 0,
              "winner " + std::to_string(s.winner) + " pinned " + (s.ladder.Pinned() ? "yes" : "NO") + " stops " + std::to_string(s.diag.loopStops) + " gone " + std::to_string(s.diag.loopGone) +
              " looped " + std::to_string(s.Looped()) + " visible " + std::to_string(s.truthVisible) + "/" + std::to_string(s.swings) + " +450 read " + std::to_string(s.at450Playing) +
              " real max " + std::to_string((int)s.loopRealMsMax) + " ms");
    }
    // 6. the control: the same model with melee_loop off. Every swing of the round-4 plan is BLENDED OUT, nothing wins, nothing is visible (the 0.4.26 log)
    {
        Sim s(Live(), false, true);
        s.Swings(4);                                          // the 5-entry plan minus the melee rung (no anchor in this sim)
        Check("R5 control (melee_loop off) on the live model: all 4 plan swings are seen at the first frame and BLENDED OUT, none progressed, no winner, none visible, no STOP_ANIM_TASK",
              s.winner == -1 && s.diag.progressed == 0 && s.diag.blendedOut == 4 && s.diag.shown == 4 && s.truthVisible == 0 && s.diag.loopStops == 0 && s.Looped() == 0,
              "winner " + std::to_string(s.winner) + " progressed " + std::to_string(s.diag.progressed) + " blended out " + std::to_string(s.diag.blendedOut) + " visible " +
              std::to_string(s.truthVisible) + " stops " + std::to_string(s.diag.loopStops));
    }
    // 7. a stop that does nothing (the native has no effect on this build): the read-back says STILL PLAYING, the tasks are cleared, the loop does not run on
    {
        Model m = Live();
        m.stopWorks = false;
        Sim s(m, true, false);
        s.Swings(1);
        Check("R5 loop on a build where STOP_ANIM_TASK does nothing: STILL PLAYING after 80 ms, the puppet's tasks cleared (1), the loop ended by 600 ms instead of running on",
              s.diag.loopStops == 1 && s.diag.loopStill == 1 && s.diag.loopGone == 0 && s.cleared == 1 && s.endsAtMs < kNever / 2 && s.loopRealMsMax >= 480.0 && s.loopRealMsMax <= 600.0,
              "stops " + std::to_string(s.diag.loopStops) + " still " + std::to_string(s.diag.loopStill) + " cleared " + std::to_string(s.cleared) + " real " + std::to_string((int)s.loopRealMsMax));
    }
    // 8. a read-back that cannot be read: the loop is cleared too (a loop we cannot see must not run on)
    {
        Model m = Live();
        m.stopWorks = false;
        m.readable = false;
        Sim s(m, true, false);
        s.Swings(1);
        Check("R5 loop with an unreadable read-back (and a stop that does nothing): Unread after 80 ms, the tasks cleared, the loop ended by 600 ms; the swing itself is reported unreadable, not seen",
              s.diag.loopStops == 1 && s.diag.loopUnread == 1 && s.diag.loopStill == 0 && s.cleared == 1 && s.loopRealMsMax <= 600.0 && s.diag.unreadable == 1 && s.diag.shown == 0,
              "stops " + std::to_string(s.diag.loopStops) + " unread " + std::to_string(s.diag.loopUnread) + " cleared " + std::to_string(s.cleared) + " real " + std::to_string((int)s.loopRealMsMax) +
              " unreadable swings " + std::to_string(s.diag.unreadable));
    }
    // 9. no survey (melee_survey=0), the ladder alone, the live model: the loop shows 6 of 6 swings with no escalation; the old order shows 0 of 6
    {
        Sim n(Live(), true, false);
        n.Swings(6, 2000.0);
        Sim o(Live(), false, false);
        o.Swings(6, 2000.0);
        Check("R5 ladder alone on the live model: melee_loop on shows 6 of 6 swings on the ped with no escalation; the old order shows 0 of 6 and climbs the ladder",
              n.truthVisible == 6 && n.diag.escalations == 0 && n.ladder.Current() == SwingVariant::Looped && o.truthVisible == 0 && o.diag.escalations >= 2,
              "on: visible " + std::to_string(n.truthVisible) + " escalations " + std::to_string(n.diag.escalations) + "; off: visible " + std::to_string(o.truthVisible) + " escalations " +
              std::to_string(o.diag.escalations));
    }
    // 10. no regression on a healthy game: both orders show every swing, neither escalates, and the loop is still ended by us
    {
        Sim n(Healthy(), true, false);
        n.Swings(6, 2000.0);
        Sim o(Healthy(), false, false);
        o.Swings(6, 2000.0);
        Check("R5 healthy game (clips play to their end): melee_loop on and off both show 6 of 6 swings with no escalation; the looped clip is still ended by us (6 stops, 6 gone)",
              n.truthVisible == 6 && o.truthVisible == 6 && n.diag.escalations == 0 && o.diag.escalations == 0 && n.diag.loopStops == 6 && n.diag.loopGone == 6 && o.diag.loopStops == 0,
              "on: visible " + std::to_string(n.truthVisible) + " stops " + std::to_string(n.diag.loopStops) + " gone " + std::to_string(n.diag.loopGone) + "; off: visible " +
              std::to_string(o.truthVisible));
    }
    // 11. the log: the per-swing line names the loop and the stop, the 10 s line has the LOOP (R5) segment with the counts of check 5
    {
        SwingLineIn in;
        in.peerId = 2;
        in.index = 7;
        in.variant = SwingVariant::Looped;
        in.flags = SwingVariantFlags(SwingVariant::Looped);
        in.durationMs = kSwingClipLength;
        in.stopAfterMs = kLoopStopAfterMs;
        in.dict = "melee@unarmed@streamed_core";
        in.clip = "ground_attack_on_spot";
        const std::string a = SwingLine(in);
        SwingLineIn in2 = in;
        in2.variant = SwingVariant::Upper;
        in2.flags = 48;
        in2.durationMs = 400;
        in2.stopAfterMs = 0;
        const std::string b = SwingLine(in2);
        const bool lineOk = a.find("flags 49, duration the clip's whole length (looped, ended by us at +480 ms),") != std::string::npos && b.find("looped") == std::string::npos &&
                            b.find("flags 48, duration 400 ms,") != std::string::npos;
        const bool diagOk = diagLine.find("LOOP (R5): looped swings issued/seen/progressed 3/3/3") != std::string::npos &&
                            diagLine.find("STOP_ANIM_TASK called 3, clip GONE after it 3, STILL PLAYING after it 0") != std::string::npos;
        Check("R5 log: the per-swing line says 'flags 49, duration the clip's whole length (looped, ended by us at +480 ms)' (and nothing about a loop for a timed swing); the 10 s line has the LOOP (R5) "
              "segment: looped 3/3/3, STOP_ANIM_TASK called 3, GONE 3, STILL PLAYING 0",
              lineOk && diagOk, std::string("line ") + (lineOk ? "ok" : "WRONG: " + a.substr(0, 260)) + ", diag " + (diagOk ? "ok" : "WRONG"));
    }
    {
        // 0.4.30: TASK_PLAY_ANIM's 8th argument is the START PHASE: every swing / emote starts at 0 (anim_phase_fix=0: 1.0, 0.4.29)
        Check("0.4.30 anim: TASK_PLAY_ANIM starts a clip at phase 0 (not 1.0, its end); anim_phase_fix=0 restores 1.0",
              AnimStartPhase(true) == 0.0f && AnimStartPhase(false) == 1.0f && kAnimStartPhase == 0.0f,
              std::to_string(AnimStartPhase(true)) + " / " + std::to_string(AnimStartPhase(false)));
    }
}

}  // namespace r5swing
