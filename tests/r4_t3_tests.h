// r4_t3_tests.h - desync round 4, T3 (punches: "punch anim not working"). Test-only, header-only. Three parts, each compiled where its helpers exist:
//   R4T3_CLIENT  (gameplay_test.cpp)  the swing display's PROGRESS check, the survey and the ladder against a puppet whose behaviours are switchable
//                (a locomotion task takes the plain upper-body layer off after 70 ms, only some variants play, the melee task needs an anchor, the
//                read-back cannot be read, the anim time is frozen). Every scenario runs under every model. Mirrors PlayPeerSwing / IssuePeerSwing /
//                PeerSwingService (apps/engine/engine.cpp) with the real libs/sync/swing.h classes.
//   R4T3_SERVER  (server_test.cpp)    the server's judgement at the LIVE geometry: a puppet displaced 0..8 m from where its owner really is
//                (0.4.9: pose-ped error mean 0.5-2.1 m, max 6.0-7.8 m) against the 1.4 m fist reach, and the per-swing log line that says why.
//   R4T3_NET     (netcode_test.cpp)   the real server over loopback: a swing at the spot where the friend is NOT is a legitimate miss and the server
//                log says so (nearest id, distance, timing); the same swing at the friend's real position is a hit. About 3 s of wall time.
#pragma once

#ifdef R4T3_CLIENT
#include "sync/swing.h"
#endif

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace r4t3 {

static std::string TF(const char* fmt, ...)
{
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

#ifdef R4T3_CLIENT
// ======================================================================================================================================
// the swing display: progress check, survey, ladder
// ======================================================================================================================================
constexpr int kClientChecks = 17;

// What GTA does to a swing on a puppet. Each flag is one hypothesis the engine must survive.
struct Model
{
    const char* name = "";
    // A locomotion task issued on the ped 70 ms after the swing takes a plain upper-body layer (Upper) off. UpperHard (flag 8), FullBody (8, the
    // whole body is the swing) and the melee task are not interrupted by it. (The live hypothesis: "playing" at the first frame, gone before
    // anything is drawn.)
    bool locoTakesPlainUpper = false;
    bool upperPlays = true;          // flags 16|32 play at all (IS_ENTITY_PLAYING_ANIM true)
    bool fullBodyPlays = true;
    bool meleeTaskPlays = true;
    bool anchor = false;             // the puppet has an anchor ped: the melee-task rung can be issued
    bool allBlended = false;         // EVERY variant is taken off the ped after 70 ms
    int readback = 1;                // 1 normal, -1 the read-back native cannot be called
    bool animTimes = false;          // GET_ENTITY_ANIM_CURRENT_TIME / TOTAL_TIME exist
    bool frozenTime = false;         // ... and the current time does not move
    double clipLenMs = 1000.0;       // the clip's real length
};

struct FollowSim
{
    Model game;
    gage::sync::SwingLadder ladder;
    gage::sync::SwingSurvey survey;
    gage::sync::SwingProbe probe;
    gage::sync::SwingDiag diag;
    bool follow = true;
    int durationMs = 400;            // g_swingDurationMs
    std::vector<int> variants;       // the variant of every issued swing
    std::vector<int> planIdx;        // ... and its survey plan index (-1)
    std::vector<long> durations;     // ... and its duration
    uint32_t truthVisible = 0;       // swings that were really still on the ped at +100 and +250 ms (the sim knows; the engine does not)
    int surveyWinner = -2;
    size_t finishedAt = 0;           // the number of swings issued when the survey's winner was taken
    uint32_t perSwingLines = 0;
    gage::sync::SwingAttempt att;
    int curPlan = -1;
    double issuedMs = 0;
    uint32_t locoTasks = 0, locoMark = 0;

    // Is the swing issued at issuedMs and shaped by `att` on the ped at `now`?
    bool Playing(double now) const
    {
        using gage::sync::SwingVariant;
        const double age = now - issuedMs;
        if (age < 0) return false;
        double dur = att.durationMs < 0 ? game.clipLenMs : (double)att.durationMs;
        if (att.variant == SwingVariant::MeleeTask)
            return game.meleeTaskPlays && age < 550.0;
        if (att.variant == SwingVariant::FullBody ? !game.fullBodyPlays : !game.upperPlays)
            return false;
        if (game.allBlended && age >= 70.0)
            return false;
        if (att.variant == SwingVariant::Upper && game.locoTakesPlainUpper && age >= 70.0)
            return false;
        return age < dur;
    }

    void Settle(bool good, bool busy)
    {
        using namespace gage::sync;
        if (curPlan >= 0) return;
        if (good) { ladder.OnResult(SwingProbe::Result::Shown, false); return; }
        if (ladder.OnResult(SwingProbe::Result::NotShown, busy)) ++diag.escalations;
    }
    void SurveyFinish()
    {
        using namespace gage::sync;
        const int w = survey.TakeWinner();
        if (w == -2) return;
        surveyWinner = w;
        finishedAt = variants.size();
        if (w >= 0)
        {
            const SwingAttempt win = SurveyPlanAt((size_t)w);
            ladder.PinTo(win.variant);
            durationMs = win.durationMs;
        }
    }

    // One swing seen at t0: IssuePeerSwing, then PeerSwingService every 16 ms frame until the probe is over.
    void Swing(double t0)
    {
        using namespace gage::sync;
        ++diag.seen;
        att.variant = ladder.Current();
        att.durationMs = durationMs;
        size_t pi = (size_t)-1;
        if (survey.Active() && (ladder.Mode() == SwingMode::Auto || ladder.Mode() == SwingMode::Play))
        {
            const bool melOk = ladder.Mode() == SwingMode::Auto && !ladder.MeleeTripped() && game.anchor;
            const SwingAttempt sa = survey.Next(melOk, &pi);
            if (pi != (size_t)-1) att = sa;
        }
        curPlan = pi != (size_t)-1 ? (int)pi : -1;
        ++diag.issued;
        ++diag.variantIssued[(size_t)att.variant];
        if (curPlan >= 0) ++diag.surveyed;
        variants.push_back((int)att.variant);
        planIdx.push_back(curPlan);
        durations.push_back(att.durationMs);
        issuedMs = t0;
        if (Playing(t0 + 112) && Playing(t0 + 256)) ++truthVisible;   // the sim's truth: still on the ped at the +100 / +250 ms read-backs
        locoMark = locoTasks;
        probe.Begin(t0, 0, follow, att.durationMs);
        bool loco = false;
        for (double now = t0 + 16; now <= t0 + 1200; now += 16)
        {
            if (!loco && game.locoTakesPlainUpper && now >= t0 + 70) { loco = true; ++locoTasks; }
            const int playing = game.readback < 0 ? -1 : Playing(now) ? 1 : 0;
            float cur = -1, total = -1;
            if (game.animTimes && playing == 1)
            {
                total = (float)(game.clipLenMs / 1000.0);
                cur = game.frozenTime ? 0.1f : (float)((now - t0) / 1000.0);
            }
            const SwingProbe::Result r = probe.Tick(now, playing, cur, total);
            if (r == SwingProbe::Result::Pending || r == SwingProbe::Result::Following)
                continue;
            if (r == SwingProbe::Result::Shown)
            {
                ++diag.shown;
                ++diag.variantShown[(size_t)att.variant];
                if (follow) continue;
                Settle(true, false);
                break;
            }
            if (r == SwingProbe::Result::Followed)
            {
                const SwingProbe::Progress pv = probe.Verdict();
                if (pv == SwingProbe::Progress::Progressed) { ++diag.progressed; ++diag.variantProgressed[(size_t)att.variant]; }
                else if (pv == SwingProbe::Progress::BlendedOut) ++diag.blendedOut;
                else if (pv == SwingProbe::Progress::Flicker) ++diag.flicker;
                else if (pv == SwingProbe::Progress::Frozen) ++diag.frozen;
                else ++diag.progressUnread;
                if (probe.TailStillPlaying()) ++diag.tail;
                if (probe.ClipCutShort()) ++diag.cutShort;
                ++perSwingLines;
                if (curPlan >= 0) { survey.OnResult((size_t)curPlan, true, pv, false); curPlan = -1; SurveyFinish(); }
                else Settle(pv == SwingProbe::Progress::Progressed || pv == SwingProbe::Progress::Unread, false);
                break;
            }
            if (r == SwingProbe::Result::NotShown)
            {
                ++diag.unseen;
                ++perSwingLines;
                if (curPlan >= 0) { survey.OnResult((size_t)curPlan, false, SwingProbe::Progress::None, false); curPlan = -1; SurveyFinish(); }
                else Settle(false, false);
                break;
            }
            if (r == SwingProbe::Result::Unknown)
            {
                ++diag.unreadable;
                if (curPlan >= 0) { survey.OnResult((size_t)curPlan, false, SwingProbe::Progress::None, true); curPlan = -1; SurveyFinish(); }
                break;
            }
            break;
        }
    }
    void Swings(int n, double start = 0)
    {
        for (int i = 0; i < n; ++i)
            Swing(start + i * 1500.0);   // 1.5 s apart: the previous swing's follow is long over
    }
    int DistinctVariants(size_t firstN) const
    {
        bool seen[4] = {false, false, false, false};
        for (size_t i = 0; i < variants.size() && i < firstN; ++i)
            seen[variants[i] & 3] = true;
        return (int)seen[0] + (int)seen[1] + (int)seen[2] + (int)seen[3];
    }
    int DistinctAttempts(size_t firstN) const
    {
        std::vector<long> a;
        for (size_t i = 0; i < variants.size() && i < firstN; ++i)
        {
            const long key = (long)variants[i] * 100000L + durations[i];
            bool dup = false;
            for (long k : a) dup = dup || k == key;
            if (!dup) a.push_back(key);
        }
        return (int)a.size();
    }
};

static std::vector<Model> Models()
{
    std::vector<Model> m;
    Model a; a.name = "A: a locomotion task takes the plain upper-body layer off after 70 ms"; a.locoTakesPlainUpper = true; m.push_back(a);
    Model b; b.name = "B: everything plays and stays"; m.push_back(b);
    Model d; d.name = "D: every variant is gone by +100 ms"; d.allBlended = true; m.push_back(d);
    Model g; g.name = "G: only the melee task plays (anchor present)"; g.upperPlays = false; g.fullBodyPlays = false; g.anchor = true; m.push_back(g);
    Model u; u.name = "U: the read-back cannot be called"; u.readback = -1; m.push_back(u);
    return m;
}

static void ClientTests()
{
    using namespace gage::sync;
    using PG = SwingProbe::Progress;
    // ---- 1. the defect, with the OLD logic: playing at the first frame is pinned as seen although nothing stays on the ped ----
    {
        Model a = Models()[0];
        FollowSim old;
        old.game = a;
        old.follow = false;                         // R3: the probe stops at the first sighting
        old.survey.Start(0);
        old.Swings(10);
        Check("R4 T3 control (the OLD probe, no follow): model A (a locomotion task takes the plain upper-body layer off after 70 ms): 10 swings are counted 'SEEN PLAYING 10/10', the ladder pins "
              "the plain upper-body variant, and the sim's truth is that NONE of them was still on the ped at +100 / +250 ms: this is how 6/6 read-backs and no punch on screen fit together",
              old.diag.shown == 10 && old.ladder.Current() == SwingVariant::Upper && old.ladder.Pinned() && old.diag.escalations == 0 && old.truthVisible == 0 && old.diag.progressed == 0,
              TF("shown %u pinned %d variant %d escalations %u truth-visible %u", old.diag.shown, (int)old.ladder.Pinned(), (int)old.ladder.Current(), old.diag.escalations, old.truthVisible));
    }
    // ---- 2. the same model with the progress check: the first two are blended out, the ladder moves to the variant that survives ----
    {
        Model a = Models()[0];
        FollowSim s;
        s.game = a;
        s.survey.Start(0);
        s.Swings(10);
        Check("R4 T3 progress: the same model with the follow: the first swings are counted BLENDED OUT (not 'seen'), the ladder moves up to the not-interruptable variant by itself, pins it, and the "
              "rest PROGRESS (truth: still on the ped at +100 / +250 ms)",
              s.diag.blendedOut >= 2 && s.ladder.Current() == SwingVariant::UpperHard && s.ladder.Pinned() && s.diag.progressed >= 6 && s.truthVisible == s.diag.progressed &&
                  s.diag.variantProgressed[(size_t)SwingVariant::Upper] == 0 && s.diag.escalations == 1,
              TF("blended %u progressed %u (truth %u) variant %d pinned %d escalations %u", s.diag.blendedOut, s.diag.progressed, s.truthVisible, (int)s.ladder.Current(),
                 (int)s.ladder.Pinned(), s.diag.escalations));
    }
    // ---- 3. the survey: several variants in one session, the winner pinned (model A) ----
    {
        Model a = Models()[0];
        FollowSim s;
        s.game = a;
        s.survey.Start(1);
        s.Swings(12);
        bool lateSame = true;
        for (size_t i = 5; i < s.variants.size(); ++i)
            lateSame = lateSame && s.variants[i] == (int)SwingVariant::UpperHard;
        Check("R4 T3 survey (model A): ONE session exercises four attempts in its first four swings (upper-body 400 ms, upper-body whole clip, not-interruptable, full body: three variants; the melee-task entry is "
              "skipped without an anchor), the swing log has one line each, the winner is the not-interruptable upper body (plan entry 2), the ladder is pinned there, and every later swing uses it and PROGRESSES",
              s.DistinctAttempts(4) == 4 && s.DistinctVariants(4) == 3 && s.surveyWinner == 2 && s.ladder.Current() == SwingVariant::UpperHard && s.ladder.Pinned() && lateSame && s.survey.Skipped() == 1 &&
                  s.diag.surveyed == 4 && s.perSwingLines == 12 && s.diag.variantProgressed[(size_t)SwingVariant::UpperHard] >= 7 && s.diag.variantProgressed[(size_t)SwingVariant::Upper] == 0,
              TF("distinct attempts %d winner %d variant %d skipped %u surveyed %u lines %u upper-hard progressed %u", s.DistinctAttempts(4), s.surveyWinner, (int)s.ladder.Current(),
                 s.survey.Skipped(), s.diag.surveyed, s.perSwingLines, s.diag.variantProgressed[(size_t)SwingVariant::UpperHard]));
    }
    // ---- 4. everything plays: the shipped behaviour (plain upper-body, 400 ms) stays the winner on a tie ----
    {
        Model b = Models()[1];
        FollowSim s;
        s.game = b;
        s.survey.Start(1);
        s.Swings(8);
        Check("R4 T3 survey (model B, everything plays): all four animation attempts progress, the earlier plan entry wins the tie: the shipped upper-body 400 ms swing stays, and the duration is not changed",
              s.surveyWinner == 0 && s.ladder.Current() == SwingVariant::Upper && s.durationMs == 400 && s.survey.Progressed(0) == 1 && s.survey.Progressed(1) == 1 &&
                  s.survey.Progressed(2) == 1 && s.survey.Progressed(3) == 1 && s.diag.progressed == 8,
              TF("winner %d variant %d duration %d progressed %u/%u/%u/%u", s.surveyWinner, (int)s.ladder.Current(), s.durationMs, s.survey.Progressed(0), s.survey.Progressed(1),
                 s.survey.Progressed(2), s.survey.Progressed(3)));
    }
    // ---- 5. nothing stays / only the melee task plays / nothing can be read ----
    {
        FollowSim d;
        d.game = Models()[2];
        d.survey.Start(1);
        d.Swings(6);
        FollowSim g;
        g.game = Models()[3];
        g.survey.Start(1);
        g.Swings(8);
        FollowSim g2;
        g2.game = Models()[3];
        g2.game.anchor = false;                     // the 0.4.9 default (loco_mode straight): no anchor
        g2.survey.Start(1);
        g2.Swings(4);
        FollowSim u;
        u.game = Models()[4];
        u.survey.Start(1);
        u.Swings(8);
        Check("R4 T3 survey: nothing stays on the ped (model D): no winner, every swing counted BLENDED OUT, the ladder is not pinned on a variant that was never seen to progress",
              d.surveyWinner == -1 && d.diag.blendedOut >= 4 && d.diag.progressed == 0 && !d.ladder.Pinned(),
              TF("winner %d blended %u progressed %u pinned %d", d.surveyWinner, d.diag.blendedOut, d.diag.progressed, (int)d.ladder.Pinned()));
        Check("R4 T3 survey: only the melee task plays and there is an anchor (model G): the melee-task entry is the winner and the ladder is pinned on it; without an anchor (loco_mode straight) the entry is "
              "skipped, nothing wins, and the animations keep their rung",
              g.surveyWinner == 4 && g.ladder.Current() == SwingVariant::MeleeTask && g.ladder.Pinned() && g2.surveyWinner == -1 && g2.survey.Skipped() == 1 && g2.ladder.Current() == SwingVariant::Upper &&
                  !g2.ladder.Pinned(),
              TF("anchor: winner %d variant %d; no anchor: winner %d skipped %u variant %d", g.surveyWinner, (int)g.ladder.Current(), g2.surveyWinner, g2.survey.Skipped(), (int)g2.ladder.Current()));
        Check("R4 T3 survey: a read-back that cannot be called is no evidence: nothing progresses, nothing is blamed, no winner, the ladder does not move",
              u.diag.unreadable >= 4 && u.diag.progressed == 0 && u.diag.blendedOut == 0 && u.ladder.Current() == SwingVariant::Upper && u.surveyWinner == -1 && u.diag.escalations == 0,
              TF("unreadable %u progressed %u blended %u variant %d", u.diag.unreadable, u.diag.progressed, u.diag.blendedOut, (int)u.ladder.Current()));
    }
    // ---- 6. the survey's own bookkeeping: an excused attempt is no evidence and comes back; the winner waits for every attempt ----
    {
        SwingSurvey sv;
        sv.Start(1);
        size_t pi = 0;
        std::vector<size_t> idx;
        for (int i = 0; i < 4; ++i)
        {
            sv.Next(false, &pi);                    // no anchor: the 5th (melee task) entry is skipped right after the 4th issue
            idx.push_back(pi);
        }
        const bool plannedDone = sv.Done();
        const int early = sv.TakeWinner();          // attempts are still outstanding
        sv.OnResult(idx[0], true, PG::BlendedOut, false);
        sv.OnResult(idx[1], true, PG::Progressed, true);    // excused (the next swing overtook it): not counted, queued again
        sv.OnResult(idx[2], true, PG::Progressed, false);
        sv.OnResult(idx[3], true, PG::Progressed, false);
        const bool openForRequeue = sv.Active() && !sv.Done();
        const int whileQueued = sv.TakeWinner();    // nothing outstanding, but the excused attempt is still to be played
        size_t again = 99;
        sv.Next(false, &again);                     // the excused attempt comes back first
        sv.OnResult(again, true, PG::Progressed, false);
        const int win = sv.TakeWinner();
        const int second = sv.TakeWinner();
        Check("R4 T3 survey bookkeeping: the winner waits for every attempt to report (-2 before) AND for an excused attempt to be played again, an excused result is not counted as a try, the best animation wins "
              "(a tie goes to the earlier plan entry: entry 1 here, which only progressed on its second go) and the winner is handed out once",
              plannedDone && early == -2 && openForRequeue && whileQueued == -2 && again == idx[1] && win == 1 && second == -2 && sv.Outstanding() == 0 && sv.Progressed(1) == 1 && sv.Issued(1) == 1,
              TF("planned %d early %d open %d queued %d again %zu win %d second %d prog(1) %u issued(1) %u", (int)plannedDone, early, (int)openForRequeue, whileQueued, again, win, second,
                 sv.Progressed(1), sv.Issued(1)));
    }
    // ---- 7. the progress verdicts of the probe ----
    {
        struct Row { int p[4]; PG want; const char* name; };
        // p[0] = first sighting (always 1 here), p[1..3] = +100, +250, +450
        const Row rows[] = {
            { { 1, 1, 1, 0 }, PG::Progressed, "playing at +100 and +250, over by +450 (a 400 ms swing)" },
            { { 1, 1, 1, 1 }, PG::Progressed, "playing throughout" },
            { { 1, 0, 0, 0 }, PG::BlendedOut, "gone by +100" },
            { { 1, 1, 0, 0 }, PG::BlendedOut, "gone by +250" },
            { { 1, 0, 1, 1 }, PG::Flicker, "lost at +100, back at +250" },
            { { 1, -1, -1, -1 }, PG::Unread, "never readable again" },
        };
        bool all = true;
        std::string bad;
        for (const Row& r : rows)
        {
            SwingProbe pr;
            pr.Begin(0, 0, true, 400);
            pr.Tick(16, 1);                         // first sighting
            SwingProbe::Result last = SwingProbe::Result::Pending;
            const double at[3] = { 100, 250, 450 };
            for (int k = 0; k < 3; ++k)
                last = pr.Tick(at[k] + 3, r.p[k + 1]);
            const bool ok = last == SwingProbe::Result::Followed && pr.Verdict() == r.want;
            if (!ok) { all = false; bad += std::string(r.name) + "; "; }
        }
        Check("R4 T3 progress verdicts: playing at +100 and +250 progresses; gone by +100 or +250 is BLENDED OUT; lost and back is a flicker; reads that never answer again are unreadable",
              all, bad);
    }
    // ---- 8. the anim time, when the natives exist: advancing progresses, a frozen clip is caught, the cut-short clip is named ----
    {
        const auto run = [](float cur0, float cur1, float cur2, float total, int durationMs) {
            SwingProbe pr;
            pr.Begin(0, 0, true, durationMs);
            pr.Tick(16, 1, cur0, total);
            pr.Tick(103, 1, cur1, total);
            pr.Tick(253, 1, cur2, total);
            pr.Tick(453, durationMs < 450 ? 0 : 1, cur2, total);
            return pr;
        };
        const SwingProbe adv = run(0.016f, 0.103f, 0.253f, 1.2f, 400);
        const SwingProbe frozen = run(0.016f, 0.016f, 0.016f, 1.2f, 400);
        const SwingProbe unknown = run(-1.f, -1.f, -1.f, -1.f, 400);
        const SwingProbe shortClip = run(0.016f, 0.103f, 0.253f, 0.45f, 400);
        const SwingProbe longClip = run(0.016f, 0.103f, 0.253f, 1.2f, -1);
        Check("R4 T3 anim time (pure; the natives are not in hook.h, the wrapper returns unknown): a time that advances progresses, a time that does not is FROZEN, unknown times (-1) are plausible, and a clip "
              "known to be 1.2 s long played for 400 ms is flagged CUT SHORT (a 0.45 s clip and a whole-clip duration are not)",
              adv.Verdict() == PG::Progressed && frozen.Verdict() == PG::Frozen && unknown.Verdict() == PG::Progressed && adv.ClipCutShort() && !shortClip.ClipCutShort() &&
                  !longClip.ClipCutShort() && !unknown.ClipCutShort(),
              TF("adv %d frozen %d unknown %d cut %d/%d/%d/%d", (int)adv.Verdict(), (int)frozen.Verdict(), (int)unknown.Verdict(), (int)adv.ClipCutShort(), (int)shortClip.ClipCutShort(),
                 (int)longClip.ClipCutShort(), (int)unknown.ClipCutShort()));
        SwingProbe tail;
        tail.Begin(0, 0, true, 400);
        tail.Tick(16, 1); tail.Tick(103, 1); tail.Tick(253, 1); tail.Tick(453, 1);
        SwingProbe tail2;
        tail2.Begin(0, 0, true, 400);
        tail2.Tick(16, 1); tail2.Tick(103, 1); tail2.Tick(253, 1); tail2.Tick(453, 0);
        Check("R4 T3 progress: a timed 400 ms swing still playing at +450 ms is reported as holding or looping (tail), one that is over is not",
              tail.TailStillPlaying() && !tail2.TailStillPlaying());
    }
    // ---- 9. SwingProbe::Active() covers the whole window (the contract with the locomotion controller) ----
    {
        SwingProbe pr;
        pr.Begin(1000, 0, true, 400);
        bool allActive = true;
        double endMs = -1;
        SwingProbe::Result r = SwingProbe::Result::Pending;
        for (double now = 1016; now <= 2200; now += 16)
        {
            r = pr.Tick(now, 1);
            if (r == SwingProbe::Result::Followed) { endMs = now - 1000; break; }
            allActive = allActive && pr.Active();
        }
        SwingProbe legacy;
        legacy.Begin(0, 0);                         // no follow: the 0e probe
        legacy.Tick(16, 1);
        SwingProbe stalled;
        stalled.Begin(0, 0, true, 400);
        stalled.Tick(16, 1);
        const SwingProbe::Result sr = stalled.Tick(5000, 1);   // the frame loop stalled for 5 s
        SwingProbe canceled;
        canceled.Begin(0, 0, true, 400);
        canceled.Tick(16, 1);
        canceled.Cancel();
        // a whole-clip swing (duration -1) and the melee task hold Active() until the swing is expected to be over, past the last sample
        SwingProbe whole, mel;
        whole.Begin(0, 0, true, -1);
        mel.Begin(0, 0, true, 400, 600.0);
        bool wholeAt800 = false, wholeEnds = false, melAt550 = false, melEnds = false;
        for (double now = 16; now <= 1300; now += 16)
        {
            whole.Tick(now, 1);
            mel.Tick(now, 1);
            if (now >= 800 && now < 816) wholeAt800 = whole.Active();
            if (now >= 1104 && now < 1120) wholeEnds = !whole.Active();
            if (now >= 544 && now < 560) melAt550 = mel.Active();
            if (now >= 608 && now < 624) melEnds = !mel.Active();
        }
        Check("R4 T3 Active(): true from the issue through the first sighting and every progress sample (>= 450 ms), false from the verdict on for a 400 ms swing; a whole-clip swing holds it until 1100 ms and the "
              "melee task until its own end (600 ms); without the follow it ends at the first sighting as before; a stalled frame loop ends it (<= 700 ms rule), and Cancel() clears it, so the locomotion "
              "controller can never be held off for good",
              allActive && endMs >= 450 && endMs <= 480 && !pr.Active() && !legacy.Active() && sr == SwingProbe::Result::Followed && !stalled.Active() && !canceled.Active() &&
                  wholeAt800 && wholeEnds && melAt550 && melEnds,
              TF("allActive %d end %.0f legacy %d stalled %d whole %d/%d melee %d/%d", (int)allActive, endMs, (int)legacy.Active(), (int)stalled.Active(), (int)wholeAt800, (int)wholeEnds,
                 (int)melAt550, (int)melEnds));
    }
    // ---- 10. the per-swing line ----
    {
        SwingLineIn li;
        li.peerId = 3; li.index = 4; li.surveyStep = 1; li.surveySteps = 5;
        li.variant = SwingVariant::Upper; li.flags = 48; li.durationMs = -1;
        li.dict = "melee@unarmed@streamed_core"; li.clip = "heavy_punch_a"; li.startMs = 123456;
        li.locoTasksMeanwhile = 2; li.firstResult = SwingProbe::Result::Shown; li.progress = PG::BlendedOut;
        li.first.taken = true; li.first.atMs = 16; li.first.playing = 1;
        li.at[0].taken = true; li.at[0].atMs = 103; li.at[0].playing = 0;
        li.at[1].taken = true; li.at[1].atMs = 253; li.at[1].playing = 0;
        li.at[2].taken = true; li.at[2].atMs = 453; li.at[2].playing = 0;
        const std::string line = SwingLine(li);
        Check("R4 T3 per-swing line: names the swing, the survey step, the variant and flags, the duration, the clip (INFERRED), the start time, the read-back at the first sighting and at +100 / +250 / +450 ms, "
              "'a locomotion task was issued meanwhile YES (2)' and the verdict BLENDED OUT",
              line.find("melee swing #4 (survey 2 of 5) on peer id=3") != std::string::npos && line.find("upper-body (16|32)") != std::string::npos && line.find("flags 48") != std::string::npos &&
                  line.find("the clip's whole length") != std::string::npos && line.find("melee@unarmed@streamed_core / heavy_punch_a (INFERRED names)") != std::string::npos &&
                  line.find("started at 123456 ms") != std::string::npos && line.find("first sighting playing at +16 ms") != std::string::npos &&
                  line.find("+100 ms NOT playing") != std::string::npos && line.find("+250 ms NOT playing") != std::string::npos && line.find("+450 ms NOT playing") != std::string::npos &&
                  line.find("a locomotion task was issued meanwhile YES (2)") != std::string::npos && line.find("verdict: BLENDED OUT") != std::string::npos,
              line);
        OwnSwingIn none;
        none.index = 3; none.peers = 1; none.spawned = 0;
        OwnSwingIn disp;
        disp.index = 4; disp.peers = 1; disp.spawned = 1; disp.hittable = 1; disp.nearestId = 2; disp.nearestM = 2.0f; disp.poseErrM = 6.4f; disp.poseDistM = 8.1f;
        OwnSwingIn okp = disp;
        okp.poseErrM = 0.4f; okp.poseDistM = 2.2f;
        const std::string l0 = OwnSwingLine(none), l1 = OwnSwingLine(disp), l2 = OwnSwingLine(okp);
        Check("R4 T3 our-swing line: says there was NO puppet to hit (peers known / drawn / hittable), or the nearest puppet's distance and how far it is from where its owner really is, and names a "
              "displaced puppet (> 1.5 m) as the reason a punch aimed at it lands where the owner is not",
              l0.find("NO puppet to hit (peers known 1, drawn 0, hittable 0)") != std::string::npos && l1.find("nearest puppet id=2 is 2.0 m from us, and 6.4 m from where its owner's pose says it is") != std::string::npos &&
                  l1.find("the owner is really 8.1 m from us") != std::string::npos && l1.find("THE PUPPET IS DISPLACED") != std::string::npos && l2.find("THE PUPPET IS DISPLACED") == std::string::npos,
              l0 + " | " + l1);
    }
    // ---- 11. the 10 s line carries the progress counters; PinTo's rules ----
    {
        FollowSim s;
        s.game = Models()[0];
        s.survey.Start(0);
        s.Swings(6);
        const std::string line = SwingDiagLine(s.diag, SwingDiag(), s.ladder);
        Check("R4 T3 10 s line: carries the PROGRESS block (still playing at +100 / +250 by variant, BLENDED OUT, flicker, frozen, +450 tail, cut short, survey swings)",
              line.find("PROGRESS (R4 T3") != std::string::npos && line.find("BLENDED OUT 2") != std::string::npos && line.find("survey swings 0") != std::string::npos &&
                  line.find("upper-hard 4") != std::string::npos,
              line);
        SwingLadder a, p, off, tripped;
        p.SetMode(SwingMode::Play);
        off.SetMode(SwingMode::Off);
        tripped.TripMeleeTask();
        Check("R4 T3 ladder.PinTo: pins an animation variant in auto and play; the melee rung only in auto with the breaker closed; never when off",
              a.PinTo(SwingVariant::FullBody) && a.Current() == SwingVariant::FullBody && a.Pinned() && a.PinTo(SwingVariant::MeleeTask) && !p.PinTo(SwingVariant::MeleeTask) &&
                  p.PinTo(SwingVariant::UpperHard) && !tripped.PinTo(SwingVariant::MeleeTask) && !off.PinTo(SwingVariant::Upper));
    }
    // ---- 12. every scenario under every model: nothing oscillates after the survey, nothing is counted twice ----
    {
        bool stable = true, counted = true;
        std::string bad;
        for (const Model& m : Models())
        {
            FollowSim s;
            s.game = m;
            s.survey.Start(1);
            s.Swings(12);
            for (size_t i = s.finishedAt + 1; i < s.variants.size(); ++i)
                stable = stable && s.variants[i] >= s.variants[i - 1];
            counted = counted && s.diag.issued == 12 && s.survey.Outstanding() == 0 &&
                      s.diag.progressed + s.diag.blendedOut + s.diag.flicker + s.diag.frozen + s.diag.progressUnread + s.diag.unseen + s.diag.unreadable <= 12;
            if (!stable || !counted) bad += std::string(m.name) + "; ";
        }
        Check("R4 T3 every model: after the survey the variant never goes back down (no oscillation; a pinned winner stays) and every issued swing is accounted once (outcomes sum to at most the swings, nothing outstanding)",
              stable && counted, bad);
    }
}
#endif  // R4T3_CLIENT

#ifdef R4T3_SERVER
// ======================================================================================================================================
// the server's judgement at the live geometry
// ======================================================================================================================================
constexpr int kServerChecks = 7;

static void ServerTests()
{
    namespace mel = gage::server::melee;
    using mel::Outcome;
    // The attacker stands at the origin facing +y (heading 0). The puppet on the attacker's screen is 1.0 m in front of him: that is where he
    // punches. The player it stands for is really `err` metres from the puppet, in direction `dirDeg` (0 = further away, 90 = to his side,
    // 180 = back toward the attacker). The server only knows the real position.
    const mel::Track atk = r3t6::Stand(0, 0, 0, 0, 2000);
    const auto verdictFor = [&](double err, double dirDeg) {
        const double r = dirDeg * 3.14159265358979 / 180.0;
        const float x = (float)(err * std::sin(r)), y = (float)(1.0 + err * std::cos(r));
        const mel::Track real = r3t6::Stand(x, y, 180, 0, 2000);
        return r3t6::Judge(atk, {{7, &real}});
    };
    {
        const mel::Verdict v0 = verdictFor(0.0, 0.0);
        Check("R4 T3 server, the 0.4.8 geometry (the puppet is where its owner is, 1.0 m in front): the swing is judged a HIT on that player",
              v0.outcome == Outcome::Hit && v0.target == 7 && std::fabs(v0.distM - 1.0f) < 0.05f, TF("%s %.2f m", mel::OutcomeName(v0.outcome), v0.distM));
    }
    {
        // the largest error that still hits, by bisection, in the three directions the live error can take
        const auto limit = [&](double dirDeg) {
            double lo = 0.0, hi = 9.0;
            for (int i = 0; i < 40; ++i)
            {
                const double mid = 0.5 * (lo + hi);
                if (verdictFor(mid, dirDeg).outcome == Outcome::Hit) lo = mid; else hi = mid;
            }
            return lo;
        };
        const double away = limit(0.0), side = limit(90.0), diag = limit(45.0);
        Check("R4 T3 server, how much puppet error a punch survives: a puppet displaced further AWAY from the attacker than 0.9 m, or sideways than about 1.6 m, is a legitimate miss (reach 1.4 m + 0.5 m slack, "
              "centre to centre, 65 degree cone): the 0.4.9 pose-ped error (mean 0.5-2.1 m) alone costs a large share of the punches",
              std::fabs(away - 0.9) < 0.05 && side > 1.4 && side < 1.8 && diag > away && diag < side + 0.2, TF("away %.2f side %.2f diagonal %.2f m", away, side, diag));
    }
    {
        // the live error range 6..8 m, every direction in 15 degree steps: never a hit, always a miss that names the player and the distance
        bool noHit = true, namesId = true;
        double nearestSeen = 1e9;
        for (double err = 6.0; err <= 8.0; err += 1.0)
            for (double dir = 0.0; dir < 360.0; dir += 15.0)
            {
                const mel::Verdict v = verdictFor(err, dir);
                noHit = noHit && v.outcome != Outcome::Hit;
                // players closer than 0.9 m would hit/behind regardless of the facing, so the sweep stays outside: at 6+ m none can be
                namesId = namesId && (v.nearestId == 7 || v.distM < 0);
                if (v.distM >= 0 && v.distM < nearestSeen) nearestSeen = v.distM;
            }
        Check("R4 T3 server, the live error (6-8 m, any direction): the swing is a MISS every time (a punch aimed at the displaced puppet hits nobody), and the verdict names the one player it measured (id and "
              "distance) so the log can say 'the nearest player stands N m away'; the hypothesis 'the displaced puppet made GI punch a spot where the friend was not' is consistent with 0 blows in 0.4.9",
              noHit && namesId && nearestSeen > 1.9, TF("noHit %d namesId %d nearest %.2f", (int)noHit, (int)namesId, nearestSeen));
    }
    {
        // the attacker's own pose: the blow time is press + 220 ms; a runner moving at 5 m/s is judged where he IS at the blow, not at the press
        mel::Track runner;
        for (double ms = 0; ms <= 1000; ms += 33.0)   // 31 samples: the track keeps the newest 48
        {
            mel::Sample s;
            s.ms = ms; s.x = 0; s.y = (float)(5.0 * ms / 1000.0); s.z = 30; s.heading = 0;
            runner.Push(s);
        }
        const mel::Track victim = r3t6::Stand(0, 3.0f, 180, 0, 1000);   // standing 3.0 m ahead of the origin; the runner is at y = 0.9 at 180 ms (2.1 m short), 2.0 at 400 ms (1.0 m)
        const mel::Verdict atPress = mel::Adjudicate(runner, 180.0, gage::server::ac::Joaat("WEAPON_UNARMED"), std::vector<mel::Candidate>{{7, &victim, false}}.data(), 1);
        const mel::Verdict atBlow = mel::Adjudicate(runner, 180.0 + mel::kStrikeDelayMs, gage::server::ac::Joaat("WEAPON_UNARMED"), std::vector<mel::Candidate>{{7, &victim, false}}.data(), 1);
        Check("R4 T3 server, where the attacker is judged: his pose AT THE BLOW (press + 220 ms by his own sample clock), not at the press: a runner who is 2.1 m short of a standing player at the press is "
              "1.0 m from him at the blow (the verdict's attacker gap is 0 inside a dense run)",
              atPress.outcome != Outcome::Hit && atBlow.outcome == Outcome::Hit && atBlow.attackerGapMs >= 0 && atBlow.attackerGapMs < 20.0,
              TF("press %s %.2f m, blow %s %.2f m, gap %.0f ms", mel::OutcomeName(atPress.outcome), atPress.distM, mel::OutcomeName(atBlow.outcome), atBlow.distM, atBlow.attackerGapMs));
    }
    {
        // the log line says why, for each outcome
        const mel::Track far7 = r3t6::Stand(0, 7.0f, 180, 0, 2000), behind = r3t6::Stand(0, -1.0f, 0, 0, 2000);
        const mel::Verdict miss = r3t6::Judge(atk, {{7, &far7}});
        const mel::Verdict back = r3t6::Judge(atk, {{7, &behind}});
        const mel::Verdict nobody = r3t6::Judge(atk, {});
        const mel::Verdict hit = verdictFor(0.0, 0.0);
        mel::SwingLogCtx lc;
        lc.name = "GI"; lc.seq = 3; lc.weapon = gage::server::ac::Joaat("WEAPON_UNARMED"); lc.pressMs = 780.0; lc.nowMs = 1010.0; lc.others = 1;
        const std::string lMiss = mel::FormatSwingLine(lc, miss), lBack = mel::FormatSwingLine(lc, back), lHit = mel::FormatSwingLine(lc, hit);
        mel::SwingLogCtx lcN = lc;
        lcN.others = 3; lcN.noState = 1; lcN.beyondPrefilter = 1; lcN.notRelevant = 1; lcN.byTimer = true;
        const std::string lNobody = mel::FormatSwingLine(lcN, nobody);
        Check("R4 T3 server per-swing line: a miss names the nearest player's id, how far it stands and how far beyond the reach, the timing (how late it was judged, how far a sample each pose came from) and "
              "how many other players there are; Behind names the angle and the cone; nobody names why (no state / beyond 8 m / not relevant); a hit keeps the R3 wording",
              lMiss.find("GI swing (shotSeq 3, weapon 0x") != std::string::npos && lMiss.find("judged no_target") != std::string::npos &&
                  lMiss.find("id 7 is the nearest and stands 7.00 m away, 5.10 m beyond the reach") != std::string::npos && lMiss.find("timing: judged 10 ms after the blow time") != std::string::npos &&
                  lMiss.find("by the attacker's samples") != std::string::npos && lMiss.find("1 other players") != std::string::npos &&
                  lBack.find("judged behind") != std::string::npos && lBack.find("id 7 at 1.00 m is 180.0 degrees off the heading, outside the facing cone") != std::string::npos &&
                  lNobody.find("no living player on foot with a pose") != std::string::npos && lNobody.find("no state 1, beyond 8 m 1, not relevant 1") != std::string::npos &&
                  lNobody.find("by the clock") != std::string::npos &&
                  lHit.find("judged hit on id 7: nearest living player on foot 1.00 m (reach 1.90 m), 0.0 degrees off the heading") != std::string::npos,
              lMiss + " | " + lNobody);
    }
    {
        // the time window: a pending swing is judged by the clock when the attacker's samples have not reached the blow
        mel::SwingBook b;
        b.Observe(1000.0, 1, false, true, 3, gage::server::ac::Joaat("WEAPON_UNARMED"));
        mel::SwingBook::Pending p1, p2;
        const bool early = b.Take(1100.0, 1100.0, p1);              // neither the samples nor the clock are at 1220 yet
        const bool byClock = b.Take(1315.0, 1100.0, p1);            // the clock passed 1220 + 90 with the newest sample at 1100
        mel::SwingBook b2;
        b2.Observe(1000.0, 1, false, true, 3, gage::server::ac::Joaat("WEAPON_UNARMED"));
        const bool bySamples = b2.Take(1230.0, 1225.0, p2);
        mel::Track t = r3t6::Stand(0, 0, 0, 0, 1000);
        Check("R4 T3 server, the time window: a swing is judged when the attacker's samples reach press + 220 ms, or 90 ms of the server's clock later when they do not (flagged byTimer, printed as 'by the clock'); "
              "Track::GapMs says how far the nearest sample is (0 in a dense run, large across a stall)",
              !early && byClock && p1.byTimer && bySamples && !p2.byTimer && t.GapMs(500.0) < 20.0 && t.GapMs(5000.0) > 3900.0 && mel::Track().GapMs(1.0) < 0.0,
              TF("early %d byClock %d/%d bySamples %d/%d gap %.0f/%.0f", (int)early, (int)byClock, (int)p1.byTimer, (int)bySamples, (int)p2.byTimer, t.GapMs(500.0), t.GapMs(5000.0)));
    }
    {
        // a vertical separation: another floor is named
        const mel::Track upstairs = [&] { mel::Track t; for (double ms = 0; ms <= 2000; ms += 33.0) { mel::Sample s; s.ms = ms; s.x = 0; s.y = 1.0f; s.z = 34.0f; s.heading = 180; t.Push(s); } return t; }();
        const mel::Verdict v = r3t6::Judge(atk, {{7, &upstairs}});
        mel::SwingLogCtx lc;
        lc.name = "GI"; lc.others = 1; lc.nearestAnyId = 7; lc.nearestAnyM = 1.0f;
        const std::string line = mel::FormatSwingLine(lc, v);
        Check("R4 T3 server per-swing line: a player 4 m above is not a candidate (another floor): the line says so ('another floor 1') and still names the nearest player by position",
              v.outcome == Outcome::NoTarget && v.verticalOut == 1 && line.find("another floor 1") != std::string::npos && line.find("nearest by position id 7 at 1.00 m") != std::string::npos,
              line);
    }
}
#endif  // R4T3_SERVER

#ifdef R4T3_NET
// ======================================================================================================================================
// the real server: a punch at the spot where the friend is not
// ======================================================================================================================================
constexpr int kNetChecks = 4;

static void NetTests()
{
    using gage::server::melee::Outcome;
    const proto::Kinematics atkAt = Kin(0, 0, 30, 0);
    Rig rig;
    if (!rig.Start(r3t6::AdjConfig(true))) { Check("R4 T3 net: server starts", false); return; }
    Probe alice(rig), bob(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob")) { Check("R4 T3 net: two players join", false); return; }
    const uint16_t aId = IdOf(alice), bId = IdOf(bob);
    // Bob is REALLY 7 m in front of Alice. On Alice's screen his puppet stands 1 m in front of her (a 6 m displacement, the 0.4.9 maximum): she punches there.
    const bool placed = Place(rig, alice, atkAt) && Place(rig, bob, Kin(0, 7.0f, 30, 180));
    const bool rel = rig.Until([&] { return rig.srv.IsRelevant(aId, bId); }, 3000);
    if (!placed || !rel) { Check("R4 T3 net: players placed and relevant", false); return; }
    const server::Stats& st = rig.srv.stats();
    r3t6::SendFor(rig, alice, atkAt, 0, 400);
    r3t6::SendFor(rig, alice, atkAt, 1, 700, 350);
    Check("R4 T3 net: a swing at the spot where the friend is NOT (he is really 7 m away) is a legitimate miss: no Damage, judged no_target",
          r3t6::MeleeDamages(bob, aId) == 0 && st.meleeOutcome[(size_t)Outcome::NoTarget] == 1 && st.meleeSwingsSeen == 1,
          TF("damages %d no_target %llu seen %llu", r3t6::MeleeDamages(bob, aId), (unsigned long long)st.meleeOutcome[(size_t)Outcome::NoTarget], (unsigned long long)st.meleeSwingsSeen));
    std::string missLine;
    for (const std::string& l : rig.logs)
        if (l.find("judged no_target") != std::string::npos) missLine = l;
    Check("R4 T3 net: ... and the server log has ONE line for that swing that says why: the attacker's name, 'id <Bob> is the nearest and stands 7.0x m away', the timing and the player count",
          missLine.find("Alice swing (shotSeq 1") != std::string::npos && missLine.find("id " + std::to_string(bId) + " is the nearest and stands 7.") != std::string::npos &&
              missLine.find("timing: judged") != std::string::npos && missLine.find("1 other players") != std::string::npos,
          missLine);
    // the same swing with Bob where Alice punches (error 0): a hit, and its line
    Place(rig, bob, Kin(0, 1.0f, 30, 180));
    r3t6::SendFor(rig, alice, atkAt, 2, 700, 350);
    std::string hitLine;
    for (const std::string& l : rig.logs)
        if (l.find("judged hit on id") != std::string::npos) hitLine = l;
    Check("R4 T3 net: with the friend where the puppet is (error 0) the same swing hurts him once and the line says 'judged hit on id <Bob>'",
          r3t6::MeleeDamages(bob, aId) == 1 && st.meleeOutcome[(size_t)Outcome::Hit] == 1 && hitLine.find("judged hit on id " + std::to_string(bId)) != std::string::npos,
          TF("damages %d hit %llu line: %s", r3t6::MeleeDamages(bob, aId), (unsigned long long)st.meleeOutcome[(size_t)Outcome::Hit], hitLine.c_str()));
    Check("R4 T3 net: every judged swing (a miss that is NOT within 3 m included) is logged: two swings, two judgement lines",
          [&] { int n = 0; for (const std::string& l : rig.logs) if (l.find("melee: Alice swing (shotSeq") != std::string::npos) n++; return n == 2; }());
}
#endif  // R4T3_NET

}  // namespace r4t3
