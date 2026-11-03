// Animation audit (0e): scripted simulations of the whole animation path with the REAL EmoteArbiter / EmoteDictHold /
// AnimDictRefs and the pure swing pieces of libs/sync/swing.h, driven against a small fake game (a ped with a primary and
// a secondary animation slot and a gait task, dictionaries that load N frames after a request, a ragdoll, a streaming layer
// that can drop a request). Included by gameplay_test.cpp after Check() / F(); the count below is added to its expected
// total. The glue in PeerSim / SwingSim mirrors apps/engine/engine.cpp (TickPeerEmote, PlayPeerSwing) step for step: the
// engine's own functions call natives and cannot run here, so what is proven is the logic they delegate to, in the order
// they call it. Nothing here knows a real GTA clip name: the names are placeholders the fake game accepts.
#pragma once

#include "sync/emote.h"
#include "sync/swing.h"

#include <map>
#include <set>
#include <string>

namespace anim_audit {

using namespace gage::sync;
namespace proto = gage::proto;

constexpr int kAnimAuditChecks = 48;
constexpr double kFrameMs = 1000.0 / 60.0;

// ---- the fake game ---------------------------------------------------------------------------------------------------
struct FakeGame
{
    double now = 0;
    int frame = 0;
    int loadFrames = 3;                 // frames from an effective request to HAS_ANIM_DICT_LOADED
    int dropRequests = 0;               // the game ignores this many REQUEST_ANIM_DICT calls (a dropped request)
    bool requestCallable = true;        // REQUEST_ANIM_DICT can be called (natives resolved)
    std::set<std::string> dicts;        // DOES_ANIM_DICT_EXIST
    std::set<std::string> goodClips;    // "dict/clip" that exist (a wrong clip name is a silent no-op)
    std::map<std::string, int> loadedAt;   // dictionary -> frame it is loaded from
    std::map<std::string, int> removeCalls;
    int requests = 0, effectiveRequests = 0, playCalls = 0, clearNowCalls = 0, softClears = 0;

    // one ped
    struct Slot { bool on = false; std::string key; double endMs = -1; };   // endMs < 0: loops
    Slot primary, secondary;
    bool gaitAlive = true;
    int gaitIssues = 0;

    static std::string Lo(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        return s;
    }
    static std::string Key(const std::string& d, const std::string& c) { return Lo(d) + "/" + c; }
    int Exists(const std::string& d) const { return dicts.count(Lo(d)) ? 1 : 0; }
    bool Loaded(const std::string& d0) const
    {
        const auto it = loadedAt.find(Lo(d0));
        return it != loadedAt.end() && frame >= it->second;
    }
    bool Request(const std::string& d0)
    {
        const std::string d = Lo(d0);
        ++requests;
        if (!requestCallable)
            return false;
        if (dropRequests > 0)
        {
            --dropRequests;
            return true;
        }
        if (!loadedAt.count(d))
        {
            loadedAt[d] = frame + loadFrames;
            ++effectiveRequests;
        }
        return true;
    }
    void Remove(const std::string& d0)
    {
        const std::string d = Lo(d0);
        loadedAt.erase(d);
        ++removeCalls[d];
    }
    // TASK_PLAY_ANIM: a silent no-op when the dictionary is not loaded or the clip does not exist (the real native
    // reports nothing either). Flag 32 goes to the secondary slot, anything else replaces the primary task.
    bool Play(const std::string& d, const std::string& c, int durationMs, int flags)
    {
        ++playCalls;
        if (!Loaded(d) || !goodClips.count(Key(d, c)))
            return true;
        Slot& s = (flags & 32) ? secondary : primary;
        s.on = true;
        s.key = Key(d, c);
        s.endMs = (durationMs > 0) ? now + durationMs : ((flags & 1) ? -1.0 : now + 2000.0);   // a clip is 2 s long
        return true;
    }
    void ClearNow()
    {
        ++clearNowCalls;
        primary.on = secondary.on = false;
        gaitAlive = false;
    }
    void ClearSoft()
    {
        ++softClears;
        primary.on = secondary.on = false;
        gaitAlive = false;
    }
    void Ragdoll() { primary.on = secondary.on = false; gaitAlive = false; }
    // the locomotion controller issues a (primary) gait task: it replaces a primary anim, a secondary one runs on
    void IssueGait()
    {
        ++gaitIssues;
        gaitAlive = true;
        primary.on = false;
    }
    int Playing(const std::string& d, const std::string& c) const
    {
        const std::string k = Key(d, c);
        for (const Slot* s : { &primary, &secondary })
            if (s->on && s->key == k && (s->endMs < 0 || now < s->endMs))
                return 1;
        return 0;
    }
    bool AnyAnim() const
    {
        return (primary.on && (primary.endMs < 0 || now < primary.endMs)) ||
               (secondary.on && (secondary.endMs < 0 || now < secondary.endMs));
    }
    void Step() { ++frame; now += kFrameMs; }
};

// ---- the engine's peer emote glue, mirrored (TickPeerEmote + the controller's reaction to its return) ------------
struct PeerSim
{
    FakeGame& g;
    AnimDictRefs& refs;
    EmoteArbiter emote;
    EmoteDictHold hold;
    proto::Anim anim;
    bool legacyGlue = false;        // the 0.4.5 glue: always CLEAR first, never hand the ped back at an upper-body issue
    bool locoThinksAlive = true;    // the controller believes its gait task runs (it re-issues only after a Reset)
    double lastGaitMs = 0;
    uint32_t confirmed = 0, neverPlayed = 0, reissued = 0;
    int ended = 0;
    EmoteEnd lastEnd = EmoteEnd::None;

    PeerSim(FakeGame& game, AnimDictRefs& r, EmoteConfig cfg = PeerEmoteConfig()) : g(game), refs(r), emote(cfg) {}

    void SyncDict(bool hasPed = true)
    {
        const std::string rm = hold.Sync(refs, hasPed && emote.Active() && emote.Kind() == EmoteKind::Anim ? anim.dict : std::string());
        if (!rm.empty())
            g.Remove(rm);
    }
    void Start(const proto::Anim& a)
    {
        anim = a;
        emote.StartAnim(g.now, a.durationMs, a.flags);
        SyncDict();
    }
    void Stop() { emote.Stop(); }

    // One frame: the emote tick, then the controller (it runs only when no emote holds the ped).
    bool Frame(bool dead = false, bool inVehicle = false)
    {
        SyncDict();
        bool holds = false;
        if (emote.Active())
        {
            EmoteIn in;
            in.nowMs = g.now;
            if (emote.WaitingForDict())
                in.dictLoaded = g.Loaded(anim.dict);
            if (emote.Issued())
                in.playing = g.Playing(anim.dict, anim.name);
            in.dead = dead;
            in.inVehicle = inVehicle;
            const EmoteOut o = emote.Tick(in);
            if (o.requestDict)
                g.Request(anim.dict);
            if (o.issueAnim)
            {
                if (legacyGlue || o.clearFirst)
                    g.ClearNow();
                g.Play(anim.dict, anim.name, EmoteArbiter::GtaDuration(anim.durationMs),
                       (int)EmoteArbiter::GtaFlags(anim.durationMs, anim.flags));
                if (!legacyGlue && o.resetLoco)
                    locoThinksAlive = false;
            }
            if (o.clearTasks)
                g.ClearSoft();
            if (o.confirmed)
                ++confirmed;
            if (o.neverPlayed)
                ++neverPlayed;
            if (o.reissued)
                ++reissued;
            if (o.ended)
            {
                ++ended;
                lastEnd = o.endReason;
                SyncDict();
                locoThinksAlive = false;         // LocoReset(kSiteEmoteEnd)
            }
            else
                holds = o.locoPaused;
        }
        if (!holds)
        {
            // the controller: re-issue after a Reset, and its own periodic re-issue (1 s: the target is reached)
            if (!locoThinksAlive || g.now - lastGaitMs >= 1000.0)
            {
                g.IssueGait();
                locoThinksAlive = true;
                lastGaitMs = g.now;
            }
        }
        g.Step();
        return holds;
    }
    void Run(int frames)
    {
        for (int i = 0; i < frames; ++i)
            Frame();
    }
};

static proto::Anim MakeAnim(const char* dict, const char* name, int32_t durationMs, uint32_t flags)
{
    proto::Anim a;
    a.dict = dict;
    a.name = name;
    a.durationMs = durationMs;
    a.flags = flags;
    return a;
}

// ---- the melee swing display, mirrored (PlayPeerSwing / SwingClipFor / the probe) ---------------------------------------
struct SwingSim
{
    FakeGame& g;
    AnimDictRefs& refs;
    const char* dict[2] = { "swing_a", "swing_b" };
    const char* clip[2] = { "punch_a", "punch_b" };
    SwingBook book;
    DictStream stream[2];
    bool held[2] = { false, false };
    SwingProbe probe;
    PendingSwing pending;
    bool legacy = false;            // 0.4.5: one request ever, no pending swing, no probe
    bool legacyRequested[2] = { false, false };
    AnimKindStats st;

    SwingSim(FakeGame& game, AnimDictRefs& r, bool legacyMode = false) : g(game), refs(r), legacy(legacyMode) { book.Reset(2); }

    // SwingClipFor: the candidate to use, its dictionary requested.
    int Clip(bool warm)
    {
        for (size_t i = 0; i < 2; ++i)
            if (book.Exists(i) < 0)
                book.SetExists(i, g.Exists(dict[i]));
        const int pick = book.Pick();
        if (pick < 0)
            return -1;
        const size_t i = (size_t)pick;
        if (!held[i])
        {
            held[i] = true;
            refs.Acquire(dict[i]);
            if (legacy)
            {
                g.Request(dict[i]);                       // once, counted sent whatever it returned
                legacyRequested[i] = true;
            }
        }
        if (!legacy && stream[i].Due(g.now))
        {
            if (stream[i].Tick(g.now, g.Loaded(dict[i])) && !g.Request(dict[i]))
                stream[i].NoteRequestFailed();
        }
        (void)warm;
        return pick;
    }
    bool Ready(int c) { return g.Loaded(dict[c]); }
    void Issue(int c)
    {
        g.Play(dict[c], clip[c], 400, 16 | 32);
        ++st.issued;
        if (!legacy)
            probe.Begin(g.now, (size_t)c);
    }
    // One swing step from the sender (PlayPeerSwing).
    void Swing()
    {
        ++st.requested;
        const int c = Clip(false);
        if (c < 0)
        {
            ++st.dictMissing;
            return;
        }
        if (!Ready(c))
        {
            if (!legacy)
                pending.Arm(g.now);
            return;
        }
        Issue(c);
    }
    // Per frame, after the swing step: the warm-up request, the late swing, the probe.
    void Frame(bool meleeWeaponHeld = true)
    {
        int c = -1;
        if (meleeWeaponHeld)
            c = Clip(true);
        if (!legacy && pending.Armed() && c >= 0 && pending.Take(g.now, Ready(c)))
        {
            ++st.late;
            Issue(c);
        }
        if (!legacy && probe.Active())
        {
            const size_t pc = probe.Candidate();
            const SwingProbe::Result r = probe.Tick(g.now, g.Playing(dict[pc], clip[pc]));
            if (r == SwingProbe::Result::Shown)
                ++st.shown;
            else if (r == SwingProbe::Result::NotShown)
                ++st.unseen;
            if (r != SwingProbe::Result::Pending && r != SwingProbe::Result::Idle)
                book.OnProbe(pc, r);
        }
        g.Step();
    }
};

inline void TestAnimAudit()
{
    // ===== 1. dictionary load delays: 0, 3 and 30 frames =====
    for (int delay : { 0, 3, 30 })
    {
        FakeGame g;
        g.dicts = { "d1" };
        g.goodClips = { "d1/c1" };
        g.loadFrames = delay;
        AnimDictRefs refs;
        PeerSim p(g, refs);
        p.Start(MakeAnim("d1", "c1", -1, 0));      // a full-body loop
        p.Run(90);
        Check(F("sim delay %d: a loop is issued once the dictionary has loaded and the ped shows it", delay).c_str(),
              g.playCalls == 1 && g.Playing("d1", "c1") == 1 && p.confirmed == 1 && !p.emote.Stats().notPlaying,
              F("play calls %d, playing %d, confirmed %u", g.playCalls, g.Playing("d1", "c1"), p.confirmed));
        Check(F("sim delay %d: the controller stays off the ped for the whole emote (no gait task over a full-body loop)", delay).c_str(),
              g.gaitIssues == 0, F("%d gait issues", g.gaitIssues));
        p.Stop();
        p.Run(3);
        Check(F("sim delay %d: Stop clears the anim, frees the dictionary once and the gait is issued again", delay).c_str(),
              !g.AnyAnim() && g.removeCalls["d1"] == 1 && g.gaitAlive && refs.Tracked() == 0,
              F("anim %d removes %d gait %d refs %zu", g.AnyAnim(), g.removeCalls["d1"], g.gaitAlive, refs.Tracked()));
    }

    // ===== 2. upper-body emote while the controller walks the ped =====
    {
        FakeGame g;
        g.dicts = { "d1" };
        g.goodClips = { "d1/c1" };
        AnimDictRefs refs;
        PeerSim p(g, refs);
        p.Start(MakeAnim("d1", "c1", -1, 1 | 16 | 32));
        p.Run(200);
        Check("sim upper body: the loop plays over the gait (secondary slot) and the gait task is never cleared for it",
              g.Playing("d1", "c1") == 1 && g.clearNowCalls == 0 && g.gaitAlive,
              F("playing %d, clears %d, gait %d, gait issues %d", g.Playing("d1", "c1"), g.clearNowCalls, g.gaitAlive, g.gaitIssues));
        Check("sim upper body: the controller's own gait re-issues (a primary task) do not cancel the secondary anim",
              g.gaitIssues >= 2 && g.Playing("d1", "c1") == 1, F("gait issues %d", g.gaitIssues));

        FakeGame g2;
        g2.dicts = { "d1" };
        g2.goodClips = { "d1/c1" };
        AnimDictRefs refs2;
        PeerSim old(g2, refs2);
        old.legacyGlue = true;
        old.Start(MakeAnim("d1", "c1", -1, 1 | 16 | 32));
        old.Run(20);
        Check("sim upper body (0.4.5 glue, kept as the failing baseline): the issue cleared the gait and the controller did not know",
              !g2.gaitAlive && old.locoThinksAlive, F("gait alive %d, controller thinks %d", g2.gaitAlive, old.locoThinksAlive));
    }

    // ===== 3. restart: a full-body loop replaced by an upper-body one =====
    {
        FakeGame g;
        g.dicts = { "d1", "d2" };
        g.goodClips = { "d1/c1", "d2/c2" };
        AnimDictRefs refs;
        PeerSim p(g, refs);
        p.Start(MakeAnim("d1", "c1", -1, 1));
        p.Run(40);
        p.Start(MakeAnim("d2", "c2", -1, 1 | 16 | 32));
        p.Run(10);
        const bool gaitSoon = g.gaitAlive;             // within 10 frames, not at the controller's own 1 s re-issue
        const bool oldGoneSoon = g.Playing("d1", "c1") == 0;   // ... and the old full-body task too
        p.Run(50);
        Check("sim restart: the earlier full-body loop is gone when the upper-body one starts (its task was cleared)",
              oldGoneSoon && g.Playing("d1", "c1") == 0 && g.Playing("d2", "c2") == 1, F("old %d new %d (old gone after 10 frames: %d)", g.Playing("d1", "c1"), g.Playing("d2", "c2"), oldGoneSoon));
        Check("sim restart: the first dictionary was freed once, the second is held, the restart is counted",
              g.removeCalls["d1"] == 1 && refs.Count("d2") == 1 && p.emote.Stats().restarted == 1,
              F("removes %d, refs %d, restarted %u", g.removeCalls["d1"], refs.Count("d2"), p.emote.Stats().restarted));
        Check("sim restart: the controller was handed the ped back at once after the gait was cleared under an upper-body anim",
              gaitSoon && g.gaitAlive, F("gait after 10 frames %d", gaitSoon));
    }

    // ===== 4. restart whose dictionary never loads =====
    {
        FakeGame g;
        g.dicts = { "d1", "slow" };
        g.goodClips = { "d1/c1", "slow/c2" };
        AnimDictRefs refs;
        PeerSim p(g, refs);
        p.Start(MakeAnim("d1", "c1", -1, 1 | 16 | 32));
        p.Run(30);
        g.loadFrames = 100000;                      // the second dictionary never streams
        p.Start(MakeAnim("slow", "c2", -1, 1 | 16 | 32));
        p.Run(240);                                  // 4 s: past the 3 s give-up
        Check("sim restart + dictionary timeout: the emote is given up and the old emote's secondary task does not stay on the ped",
              p.lastEnd == EmoteEnd::DictTimeout && !g.AnyAnim(), F("end %d, anim %d", (int)p.lastEnd, g.AnyAnim()));
    }

    // ===== 5. a loop the game cancelled under us (a knockdown) =====
    {
        FakeGame g;
        g.dicts = { "d1" };
        g.goodClips = { "d1/c1" };
        AnimDictRefs refs;
        PeerSim p(g, refs);
        p.Start(MakeAnim("d1", "c1", -1, 1));
        p.Run(60);
        g.Ragdoll();                                // cancelled by the hit reaction
        p.Run(60);                                  // 1 s: > 400 ms not playing
        Check("sim knockdown: a loop that was seen playing and was cancelled comes back (re-issued)",
              g.Playing("d1", "c1") == 1 && p.reissued == 1 && p.emote.Active(), F("playing %d reissued %u", g.Playing("d1", "c1"), p.reissued));
        g.Ragdoll();
        p.Run(60);
        g.Ragdoll();
        p.Run(60);
        Check("sim knockdown: the re-issue is capped (twice), then the emote ends as not playing and the ped is the controller's",
              !p.emote.Active() && p.reissued == 2 && p.lastEnd == EmoteEnd::NotPlaying && g.gaitAlive,
              F("active %d reissued %u end %d", p.emote.Active(), p.reissued, (int)p.lastEnd));
        // our own player never re-issues: leaving the emote is its choice
        FakeGame g2;
        g2.dicts = { "d1" };
        g2.goodClips = { "d1/c1" };
        AnimDictRefs refs2;
        PeerSim self(g2, refs2, EmoteConfig());
        self.Start(MakeAnim("d1", "c1", -1, 1));
        self.Run(60);
        g2.Ragdoll();
        self.Run(60);
        Check("sim own emote: cancelled by the player's own move it ends (no re-issue)",
              !self.emote.Active() && self.reissued == 0 && self.lastEnd == EmoteEnd::NotPlaying);
    }

    // ===== 6. a bad clip name =====
    {
        FakeGame g;
        g.dicts = { "d1" };                          // the dictionary exists, the clip does not
        AnimDictRefs refs;
        PeerSim p(g, refs);
        p.Start(MakeAnim("d1", "no_such_clip", -1, 1));
        p.Run(120);
        Check("sim bad clip: ended as never played (the engine tells the player), not re-issued, dictionary freed",
              !p.emote.Active() && p.neverPlayed == 1 && p.reissued == 0 && p.confirmed == 0 && g.removeCalls["d1"] == 1,
              F("never %u reissued %u confirmed %u removes %d", p.neverPlayed, p.reissued, p.confirmed, g.removeCalls["d1"]));
        FakeGame g2;
        g2.dicts = { "d1" };
        g2.goodClips = { "d1/c1" };
        AnimDictRefs refs2;
        PeerSim q(g2, refs2);
        q.Start(MakeAnim("d1", "c1", 0, 0));          // a one-shot of 2 s
        q.Run(240);
        Check("sim one-shot: seen playing, ends when the clip does (not 'never played'), the gait comes back",
              !q.emote.Active() && q.confirmed == 1 && q.neverPlayed == 0 && g2.gaitAlive,
              F("confirmed %u never %u end %d", q.confirmed, q.neverPlayed, (int)q.lastEnd));
        Check("sim one-shot: the confirmation arrived within 100 ms of the issue (the figure for the 10 s line)",
              q.emote.Stats().confirmMsMax > 0 && q.emote.Stats().confirmMsMax <= 100.0, F("%.1f ms", q.emote.Stats().confirmMsMax));
    }

    // ===== 7. hit reaction on a one-shot, a seat, a respawn, a disconnect =====
    {
        FakeGame g;
        g.dicts = { "d1" };
        g.goodClips = { "d1/c1" };
        AnimDictRefs refs;
        PeerSim p(g, refs);
        p.Start(MakeAnim("d1", "c1", 0, 0));
        p.Run(30);
        g.Ragdoll();
        p.Run(90);
        Check("sim one-shot knocked down: it ends as not playing and is not replayed (only loops come back)",
              !p.emote.Active() && p.reissued == 0 && g.Playing("d1", "c1") == 0);

        FakeGame g2;
        g2.dicts = { "d1" };
        g2.goodClips = { "d1/c1" };
        AnimDictRefs refs2;
        PeerSim s(g2, refs2);
        s.Start(MakeAnim("d1", "c1", -1, 1));
        s.Run(30);
        s.Frame(false, true);                          // the sender got into a car
        Check("sim seat: the emote ends without a clear and the dictionary is released",
              !s.emote.Active() && s.lastEnd == EmoteEnd::Vehicle && g2.softClears == 0 && refs2.Tracked() == 0 &&
                  g2.removeCalls["d1"] == 1);

        FakeGame g3;
        g3.dicts = { "d1" };
        g3.goodClips = { "d1/c1" };
        AnimDictRefs refs3;
        PeerSim r(g3, refs3);
        r.Start(MakeAnim("d1", "c1", -1, 1));
        r.Run(40);
        // the ped is recreated (a respawn): the old ped's anim is gone, the loop streams and plays again on the new one
        g3.Ragdoll();
        g3.primary.on = false;
        r.hold.Sync(r.refs, std::string());              // SyncEmoteDict(pe.emoteDict, false, ""): released with the old ped
        g3.Remove("d1");
        r.emote.OnNewPed(g3.now);
        r.Run(60);
        Check("sim respawn of the ped: a loop re-streams and plays again, the dictionary is held again",
              g3.Playing("d1", "c1") == 1 && refs3.Count("d1") == 1);
        r.SyncDict(false);                               // the peer disconnects: its ped is deleted
        r.emote.Clear();
        r.SyncDict(false);
        Check("sim disconnect: nothing holds the dictionary any more", refs3.Tracked() == 0 && g3.removeCalls["d1"] == 2);
    }

    // ===== 8. two peers on one dictionary =====
    {
        FakeGame g;
        g.dicts = { "d1" };
        g.goodClips = { "d1/c1" };
        AnimDictRefs refs;
        PeerSim a(g, refs), b(g, refs);
        a.Start(MakeAnim("d1", "c1", -1, 1));
        b.Start(MakeAnim("D1", "c1", -1, 1));
        a.Run(40);
        a.Stop();
        a.Run(3);
        const bool stillHeld = g.removeCalls["d1"] == 0 && refs.Count("d1") == 1;
        b.Stop();
        b.Run(3);
        Check("sim two peers: the dictionary is freed when the last one ends, not before",
              stillHeld && g.removeCalls["d1"] == 1 && refs.Tracked() == 0);
    }

    // ===== 9. the swing display: a dropped request, a slow dictionary, a wrong clip =====
    {
        // the 0.4.5 path with a dropped first request: the dictionary is never asked for again
        FakeGame g;
        g.dicts = { "swing_a", "swing_b" };
        g.goodClips = { "swing_a/punch_a", "swing_b/punch_b" };
        g.dropRequests = 1;
        AnimDictRefs refs;
        SwingSim old(g, refs, true);
        int shownOld = 0;
        for (int i = 0; i < 600; ++i)
        {
            old.Frame();
            if (i % 40 == 20)
            {
                old.Swing();
                if (g.Playing("swing_a", "punch_a"))
                    ++shownOld;
            }
        }
        FakeGame g2;
        g2.dicts = { "swing_a", "swing_b" };
        g2.goodClips = { "swing_a/punch_a", "swing_b/punch_b" };
        g2.dropRequests = 1;
        AnimDictRefs refs2;
        SwingSim nu(g2, refs2);
        int shownNew = 0;
        for (int i = 0; i < 600; ++i)
        {
            nu.Frame();
            if (i % 40 == 20)
            {
                nu.Swing();
                if (g2.Playing("swing_a", "punch_a"))
                    ++shownNew;
            }
        }
        Check("sim swing, request dropped by the game (0.4.5 path kept as the baseline): no swing is ever shown",
              shownOld == 0 && g.effectiveRequests == 0, F("shown %d, effective requests %d", shownOld, g.effectiveRequests));
        Check("sim swing, request dropped: the repeated request gets the dictionary and the swings show",
              shownNew >= 12 && g2.effectiveRequests == 1, F("shown %d of 15, requests %d", shownNew, g2.requests));
    }
    {
        // a request before the natives resolved is retried, not counted as sent
        FakeGame g;
        g.dicts = { "swing_a", "swing_b" };
        g.goodClips = { "swing_a/punch_a" };
        g.requestCallable = false;
        AnimDictRefs refs;
        SwingSim s(g, refs);
        for (int i = 0; i < 30; ++i)
            s.Frame();
        g.requestCallable = true;
        for (int i = 0; i < 30; ++i)
            s.Frame();
        Check("sim swing, REQUEST_ANIM_DICT uncallable at first: asked again every frame until it goes through, then loaded",
              g.effectiveRequests == 1 && g.Loaded("swing_a") && s.stream[0].FailedRequests() >= 20,
              F("effective %d failed %u", g.effectiveRequests, s.stream[0].FailedRequests()));
    }
    for (int delay : { 0, 3, 30 })
    {
        // the first swing comes while the dictionary streams: shown late if it loads within 250 ms, else dropped
        FakeGame g;
        g.dicts = { "swing_a", "swing_b" };
        g.goodClips = { "swing_a/punch_a" };
        g.loadFrames = delay;
        AnimDictRefs refs;
        SwingSim s(g, refs);
        s.Swing();                                    // the very first frame: nothing warmed up
        for (int i = 0; i < 40; ++i)
            s.Frame();
        const bool shownNow = s.st.shown == 1;
        const bool expectShown = delay * kFrameMs <= 250.0;
        const unsigned expectLate = delay == 0 ? 0u : 1u;
        Check(F("sim swing, dictionary %d frames away: the first swing is %s", delay, expectShown ? "played (late when it had to wait) and seen" : "dropped (too old)").c_str(),
              expectShown ? (shownNow && s.st.issued == 1 && s.st.late == expectLate)
                          : (s.st.issued == 0 && s.pending.Expired() == 1),
              F("shown %u late %u issued %u expired %u", s.st.shown, s.st.late, s.st.issued, s.pending.Expired()));
    }
    {
        // a wrong clip in the first dictionary: never seen playing twice -> the second candidate takes over
        FakeGame g;
        g.dicts = { "swing_a", "swing_b" };
        g.goodClips = { "swing_b/punch_b" };          // swing_a exists but its clip is wrong
        AnimDictRefs refs;
        SwingSim s(g, refs);
        for (int i = 0; i < 20; ++i)
            s.Frame();
        for (int sw = 0; sw < 5; ++sw)
        {
            s.Swing();
            for (int i = 0; i < 40; ++i)
                s.Frame();
        }
        Check("sim swing, wrong clip in the first dictionary: the probe sees it never played, the book moves to the next candidate",
              s.st.unseen >= 2 && s.book.Demoted(0) && s.st.shown >= 2 && s.book.Verified(1),
              F("unseen %u shown %u demoted0 %d", s.st.unseen, s.st.shown, s.book.Demoted(0)));
    }
    {
        // every candidate wrong: the last one is never demoted (the swing keeps being issued)
        FakeGame g;
        g.dicts = { "swing_a", "swing_b" };
        AnimDictRefs refs;
        SwingSim s(g, refs);
        for (int i = 0; i < 20; ++i)
            s.Frame();
        for (int sw = 0; sw < 8; ++sw)
        {
            s.Swing();
            for (int i = 0; i < 40; ++i)
                s.Frame();
        }
        Check("sim swing, every clip wrong: one candidate demoted, the last keeps being tried, nothing is ever 'shown'",
              s.book.Demoted(0) && !s.book.Demoted(1) && s.st.shown == 0 && s.st.issued == 8 && s.book.Pick() == 1,
              F("demoted %d %d issued %u", s.book.Demoted(0), s.book.Demoted(1), s.st.issued));
    }
    {
        // a dictionary that does not exist is skipped without a request; none exists -> the swing counts as missing
        FakeGame g;
        AnimDictRefs refs;
        SwingSim s(g, refs);
        s.Swing();
        s.Frame();
        Check("sim swing, no candidate dictionary exists in the game: no request, the swing is counted as missing",
              s.st.dictMissing == 1 && g.requests == 0 && s.book.Pick() == -1);
    }
    {
        // the game streams a loaded dictionary out: the request comes again by itself
        FakeGame g;
        g.dicts = { "swing_a", "swing_b" };
        g.goodClips = { "swing_a/punch_a" };
        AnimDictRefs refs;
        SwingSim s(g, refs);
        for (int i = 0; i < 20; ++i)
            s.Frame();
        g.loadedAt.erase("swing_a");
        for (int i = 0; i < 120; ++i)
            s.Frame();
        Check("sim swing, a loaded dictionary streamed out by the game: requested again and loaded again",
              g.Loaded("swing_a") && s.stream[0].Unloads() == 1 && s.stream[0].Loads() == 2,
              F("loads %u unloads %u", s.stream[0].Loads(), s.stream[0].Unloads()));
    }

    // ===== 10. the pure pieces on their own =====
    {
        DictStream d;
        const bool first = d.Tick(0, false);
        const bool again = d.Tick(100, false);
        const bool retry = d.Tick(260, false);
        Check("DictStream: requests at once, waits 250 ms, repeats while the dictionary has not loaded", first && !again && retry && d.Requests() == 2);
        d.NoteRequestFailed();
        Check("DictStream: a call that failed is retried on the next tick", d.Tick(270, false) && d.FailedRequests() == 1);
        d.Tick(500, true);
        Check("DictStream: loaded is recorded with the time since the first request; a loaded one is rechecked once a second",
              d.Loaded() && d.Loads() == 1 && d.LastLoadMs() > 0 && !d.Due(600) && d.Due(1600));
        DictStream slow(1500.0, 1000.0);                // a long retry gap: only an 'at once' rule can answer inside it
        slow.Tick(0, false);
        slow.Tick(50, true);
        const bool atOnce = slow.Tick(1100, false);     // the recheck finds it streamed out
        Check("DictStream: a dictionary the game streamed out is requested again at once (not after the retry gap)",
              atOnce && slow.Unloads() == 1 && !slow.Loaded() && d.Due(1700));
    }
    {
        SwingProbe p;
        p.Begin(0, 0);
        const SwingProbe::Result r0 = p.Tick(16, 0);
        const SwingProbe::Result r1 = p.Tick(32, 1);
        SwingProbe q;
        q.Begin(0, 0);
        q.Tick(16, 0);
        const SwingProbe::Result rq = q.Tick(260, 0);
        SwingProbe u;
        u.Begin(0, 0);
        u.Tick(16, -1);
        const SwingProbe::Result ru = u.Tick(260, -1);
        Check("SwingProbe: Shown on the first sighting, NotShown after 250 ms of 'no', Unknown when the read never answered",
              r0 == SwingProbe::Result::Pending && r1 == SwingProbe::Result::Shown && rq == SwingProbe::Result::NotShown &&
                  ru == SwingProbe::Result::Unknown && !p.Active() && !q.Active());
    }
    {
        SwingBook b;
        b.Reset(2);
        b.SetExists(0, 1);
        b.SetExists(1, 1);
        b.OnProbe(0, SwingProbe::Result::NotShown);
        const bool d1 = b.OnProbe(0, SwingProbe::Result::NotShown);
        const int afterDemote = b.Pick();
        b.OnProbe(1, SwingProbe::Result::NotShown);
        const bool d2 = b.OnProbe(1, SwingProbe::Result::NotShown);
        Check("SwingBook: twice never seen demotes a candidate, but never the last usable one",
              d1 && afterDemote == 1 && !d2 && !b.Demoted(1) && b.Pick() == 1);
        SwingBook v;
        v.Reset(2);
        v.SetExists(0, 1);
        v.SetExists(1, 1);
        v.OnProbe(1, SwingProbe::Result::Shown);
        v.OnProbe(0, SwingProbe::Result::NotShown);
        v.OnProbe(0, SwingProbe::Result::NotShown);
        SwingBook m;
        m.Reset(2);
        m.SetExists(0, 0);
        m.SetExists(1, 1);
        SwingBook w;
        w.Reset(2);
        w.SetExists(0, 1);
        w.SetExists(1, 1);
        w.OnProbe(1, SwingProbe::Result::Shown);
        w.OnProbe(0, SwingProbe::Result::NotShown);      // not demoted yet (once)
        Check("SwingBook: a verified candidate is preferred for good, even over an earlier one not demoted yet; a missing dictionary is skipped",
              v.Pick() == 1 && v.Verified(1) && w.Pick() == 1 && !w.Demoted(0) && m.Pick() == 1);
    }
    {
        PendingSwing p;
        p.Arm(0);
        const bool notReady = p.Take(50, false);
        const bool ready = p.Take(100, true);
        const bool twice = p.Take(110, true);
        p.Arm(1000);
        const bool tooOld = p.Take(1300, true);
        Check("PendingSwing: played once when the dictionary is ready in time, never twice, dropped when older than 250 ms",
              !notReady && ready && !twice && !tooOld && p.Expired() == 1);
    }
    {
        AnimDiag now, last;
        now.Of(AnimKind::PeerSwing).requested = 12;
        now.Of(AnimKind::PeerSwing).issued = 10;
        now.Of(AnimKind::PeerSwing).shown = 9;
        now.Of(AnimKind::PeerSwing).unseen = 1;
        now.Of(AnimKind::PeerSwing).dictReady = 2;
        now.Of(AnimKind::PeerSwing).dictReadyMsSum = 200;
        now.Of(AnimKind::PeerSwing).dictReadyMsMax = 150;
        now.Of(AnimKind::LocalEmote).ended[(size_t)EmoteEnd::Stop] = 1;
        const std::string line = AnimDiagLine(now, last);
        last = now;
        Check("AnimDiagLine: one bracket per kind with activity, key=value fields, the idle kinds left out",
              line.find("[swing-peer req=12 dict_ready=2 avg_ms=100 max_ms=150") != std::string::npos &&
                  line.find("issued=10 failed=0 shown=9 unseen=1") != std::string::npos &&
                  line.find("[emote-ours req=0") != std::string::npos && line.find("stop=1") != std::string::npos &&
                  line.find("emote-peer") == std::string::npos,
              line);
        Check("AnimDiagLine: a quiet interval reads 'idle'", AnimDiagLine(now, last) == "idle");
        FirstUse f(2);
        Check("FirstUse: true once per key, bounded", f.Note("a") && !f.Note("a") && f.Note("b") && !f.Note("c") && f.Size() == 2);
    }
}

}  // namespace anim_audit
