// The pure pieces of the remote melee swing display and of the animation diagnostics (audit 0e). The swing itself is
// TASK_PLAY_ANIM of a clip whose dictionary and name are INFERRED (docs/CLOUD-REPORT.md 0c); nothing here knows a name.
// What it adds is the machinery that makes a wrong or slow one visible and survivable: the dictionary request that is
// repeated until the dictionary has loaded, the probe that asks IS_ENTITY_PLAYING_ANIM after the task was issued, the
// candidate book that stops trusting a clip the game never played, and the swing that waited for its dictionary and is
// still shown if it loads in time. Pure C++17, script tick only, tested by tests/gameplay_test.cpp.
#pragma once

#include <cstddef>
#include "emote.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

namespace gage::sync {

    // ---- one dictionary's request / load tracking -----------------------------------------------------------------
    //
    // REQUEST_ANIM_DICT must reach the game and be repeated until HAS_ANIM_DICT_LOADED: the old swing path sent it once
    // (and counted it sent even when the native could not be called), so a request the game dropped, or one made before
    // the natives resolved, left the swing waiting for a dictionary nobody was asking for. Tick says when to send.
    class DictStream
    {
    public:
        DictStream(double retryMs = 250.0, double recheckMs = 1000.0) : retryMs_(retryMs), recheckMs_(recheckMs) {}

        // Whether HAS_ANIM_DICT_LOADED is worth asking now: every frame while not loaded, every recheckMs once loaded
        // (to notice a dictionary the game streamed out).
        bool Due(double nowMs) const { return !loaded_ || nowMs - checkedMs_ >= recheckMs_; }

        // Once per Due(): the answer of HAS_ANIM_DICT_LOADED. True = send REQUEST_ANIM_DICT now (the first time, a
        // retry every retryMs while it has not loaded, at once after a failed call or after the dictionary was lost).
        bool Tick(double nowMs, bool loaded)
        {
            checkedMs_ = nowMs;
            if (loaded)
            {
                if (!loaded_)
                {
                    loaded_ = true;
                    ++loads_;
                    lastLoadMs_ = firstReqMs_ >= 0 ? nowMs - firstReqMs_ : 0.0;
                    if (lastLoadMs_ > maxLoadMs_)
                        maxLoadMs_ = lastLoadMs_;
                    firstReqMs_ = -1;
                }
                return false;
            }
            if (loaded_)
            {
                loaded_ = false;
                ++unloads_;                    // streamed out under us: ask again at once
                lastReqMs_ = -1e18;
                firstReqMs_ = -1;
            }
            if (nowMs - lastReqMs_ < retryMs_)
                return false;
            lastReqMs_ = nowMs;
            if (firstReqMs_ < 0)
                firstReqMs_ = nowMs;
            ++requests_;
            return true;
        }
        // REQUEST_ANIM_DICT could not be called (natives not resolved): ask again on the next Tick.
        void NoteRequestFailed()
        {
            lastReqMs_ = -1e18;
            ++failedRequests_;
        }
        // The dictionary was handed back (REMOVE_ANIM_DICT): start over.
        void Reset()
        {
            loaded_ = false;
            lastReqMs_ = -1e18;
            firstReqMs_ = -1;
        }

        bool Loaded() const { return loaded_; }
        uint32_t Requests() const { return requests_; }
        uint32_t FailedRequests() const { return failedRequests_; }
        uint32_t Loads() const { return loads_; }
        uint32_t Unloads() const { return unloads_; }
        double LastLoadMs() const { return lastLoadMs_; }   // first request of the last load to its HAS_ANIM_DICT_LOADED
        double MaxLoadMs() const { return maxLoadMs_; }

    private:
        double retryMs_, recheckMs_;
        bool loaded_ = false;
        double lastReqMs_ = -1e18, firstReqMs_ = -1, checkedMs_ = -1e18, lastLoadMs_ = 0, maxLoadMs_ = 0;
        uint32_t requests_ = 0, failedRequests_ = 0, loads_ = 0, unloads_ = 0;
    };

    // ---- did the swing really play? ---------------------------------------------------------------------------------
    //
    // TASK_PLAY_ANIM reports nothing: a wrong clip name or a dictionary that is not what it was thought to be is a silent
    // no-op. After the task was issued the engine asks IS_ENTITY_PLAYING_ANIM each frame for kWindowMs.
    class SwingProbe
    {
    public:
        enum class Result : uint8_t { Idle, Pending, Shown, NotShown, Unknown };
        static constexpr double kWindowMs = 250.0;

        void Begin(double nowMs, size_t candidate)
        {
            active_ = true;
            startMs_ = nowMs;
            candidate_ = candidate;
            sawNo_ = false;
        }
        bool Active() const { return active_; }
        size_t Candidate() const { return candidate_; }
        // playing: 1 yes, 0 no, -1 cannot be read. Shown on the first yes; after the window NotShown when the ped was seen
        // NOT playing it, Unknown when no read ever answered.
        Result Tick(double nowMs, int playing)
        {
            if (!active_)
                return Result::Idle;
            if (playing == 1)
            {
                active_ = false;
                return Result::Shown;
            }
            if (playing == 0)
                sawNo_ = true;
            if (nowMs - startMs_ >= kWindowMs)
            {
                active_ = false;
                return sawNo_ ? Result::NotShown : Result::Unknown;
            }
            return Result::Pending;
        }
        void Cancel() { active_ = false; }

    private:
        bool active_ = false, sawNo_ = false;
        double startMs_ = 0;
        size_t candidate_ = 0;
    };

    // ---- which candidate clip to use ---------------------------------------------------------------------------------
    //
    // Up to kMax candidate (dictionary, clip) pairs of one swing class. exists is DOES_ANIM_DICT_EXIST (-2 not asked, 1 yes,
    // 0 no, -1 cannot be called). A candidate the ped was seen playing is verified and preferred for good. One whose task
    // was issued twice and never seen playing (and never once seen) is demoted, but only while another usable candidate is
    // left: the last one keeps playing, so a probe that cannot see a secondary task never silences the swing.
    class SwingBook
    {
    public:
        static constexpr size_t kMax = 4;
        void Reset(size_t n) { *this = SwingBook(); n_ = n > kMax ? kMax : n; }
        size_t Size() const { return n_; }
        int Exists(size_t i) const { return i < n_ ? c_[i].exists : 0; }
        void SetExists(size_t i, int e) { if (i < n_) c_[i].exists = e; }
        bool Verified(size_t i) const { return i < n_ && c_[i].shown > 0; }
        bool Demoted(size_t i) const { return i < n_ && c_[i].demoted; }
        uint32_t Shown(size_t i) const { return i < n_ ? c_[i].shown : 0; }
        uint32_t Silent(size_t i) const { return i < n_ ? c_[i].silentTotal : 0; }

        // The candidate to play: a verified one, else the first that exists-or-is-unknown and is not demoted; -1 when no
        // candidate can be used (every dictionary is missing).
        int Pick() const
        {
            for (size_t i = 0; i < n_; ++i)
                if (c_[i].shown > 0 && c_[i].exists != 0)
                    return (int)i;
            for (size_t i = 0; i < n_; ++i)
                if (Usable(i) && !c_[i].demoted)
                    return (int)i;
            for (size_t i = 0; i < n_; ++i)
                if (Usable(i))
                    return (int)i;
            return -1;
        }
        // Returns true when the result demoted the candidate.
        bool OnProbe(size_t i, SwingProbe::Result r)
        {
            if (i >= n_)
                return false;
            Cand& c = c_[i];
            if (r == SwingProbe::Result::Shown)
            {
                ++c.shown;
                c.silent = 0;
                c.demoted = false;
                return false;
            }
            if (r != SwingProbe::Result::NotShown)
                return false;
            ++c.silent;
            ++c.silentTotal;
            if (c.silent >= 2 && c.shown == 0 && !c.demoted && OtherUsable(i))
            {
                c.demoted = true;
                return true;
            }
            return false;
        }

    private:
        struct Cand { int exists = -2; uint32_t shown = 0, silent = 0, silentTotal = 0; bool demoted = false; };
        bool Usable(size_t i) const { return c_[i].exists != 0; }
        bool OtherUsable(size_t i) const
        {
            for (size_t j = 0; j < n_; ++j)
                if (j != i && Usable(j) && !c_[j].demoted)
                    return true;
            return false;
        }
        size_t n_ = 0;
        Cand c_[kMax];
    };

    // ---- a swing that waited for its dictionary ----------------------------------------------------------------------
    //
    // A swing whose dictionary had not loaded was lost. It is kept for maxAgeMs and played the moment the dictionary is
    // ready: a punch shown 100 ms late beats none (a swing lasts 400 ms; older than maxAgeMs it would be a ghost).
    class PendingSwing
    {
    public:
        explicit PendingSwing(double maxAgeMs = 250.0) : maxAgeMs_(maxAgeMs) {}
        void Arm(double nowMs) { armed_ = true; armedMs_ = nowMs; }
        bool Armed() const { return armed_; }
        // True = play it now (armed, ready, not too old). Too old: dropped, counted in Expired().
        bool Take(double nowMs, bool ready)
        {
            if (!armed_)
                return false;
            if (nowMs - armedMs_ > maxAgeMs_)
            {
                armed_ = false;
                ++expired_;
                return false;
            }
            if (!ready)
                return false;
            armed_ = false;
            return true;
        }
        void Clear() { armed_ = false; }
        uint32_t Expired() const { return expired_; }
    private:
        double maxAgeMs_;
        bool armed_ = false;
        double armedMs_ = 0;
        uint32_t expired_ = 0;
    };

    // ---- the melee swing ladder (R3 T6) -----------------------------------------------------------------------------------
    //
    // Live 0.4.7: P2's swings reached GI's screen as "peer swings shown 2, 1, 4" and GI saw no punch: the counter counted TASK_PLAY_ANIM
    // calls, not animations. 0e added the IS_ENTITY_PLAYING_ANIM probe; this adds what to DO when the probe says no. The ways a
    // TASK_PLAY_ANIM swing can fail to appear on a puppet are all plausible and none is knowable offline: a wrong clip name (SwingBook
    // already walks the candidates), an upper-body secondary task the puppet's task tree ignores, a locomotion task issued 13 times in
    // 10 s that replaces or clears it, the puppet being busy. So the way the swing is issued is a LADDER of variants the engine climbs by
    // MEASURED effect, strictly upwards (no oscillation), pinning the first variant that is seen playing:
    //
    //   Upper      TASK_PLAY_ANIM flags 16|32 (upper body + secondary): the legs and the gait task keep running  (the 0c behaviour)
    //   UpperHard  16|32|8: also not interruptable, so a task issued meanwhile does not end it
    //   FullBody   8: the whole body plays the clip (the gait task is replaced for the swing; the locomotion controller re-issues after)
    //   MeleeTask  TASK_PUT_PED_DIRECTLY_INTO_MELEE against the puppet's own invisible invincible anchor ped (the blows can land on nobody
    //              but it), with the AI melee damage modifier at 0 for the window and a breaker that drops the variant for the rest of the
    //              session at the first sign of harm to the local player. The clip does not matter to it; the read-back is
    //              IS_PED_IN_MELEE_COMBAT.
    //
    // melee_anim = auto (default) | play (the three animation variants only) | melee_task (the last rung only) | off (no swing is shown).
    enum class SwingMode : uint8_t { Auto, Play, MeleeTask, Off };
    enum class SwingVariant : uint8_t { Upper, UpperHard, FullBody, MeleeTask, kCount };

    inline int SwingVariantFlags(SwingVariant v)
    {
        return v == SwingVariant::Upper ? (16 | 32) : v == SwingVariant::UpperHard ? (16 | 32 | 8) : v == SwingVariant::FullBody ? 8 : 0;
    }
    inline const char* SwingVariantName(SwingVariant v)
    {
        return v == SwingVariant::Upper ? "upper-body (16|32)" : v == SwingVariant::UpperHard ? "upper-body, not interruptable (16|32|8)"
             : v == SwingVariant::FullBody ? "full body (8)" : "melee task (TASK_PUT_PED_DIRECTLY_INTO_MELEE)";
    }
    inline const char* SwingModeName(SwingMode m)
    {
        return m == SwingMode::Auto ? "auto" : m == SwingMode::Play ? "play" : m == SwingMode::MeleeTask ? "melee_task" : "off";
    }

    class SwingLadder
    {
    public:
        // escalateAfter: consecutive swings not seen playing (and not excused) that move the ladder up one rung. repinAfter: the same
        // for a variant that WAS seen playing before (a streak of misses after a success is a different cause, so it takes longer).
        explicit SwingLadder(uint32_t escalateAfter = 2, uint32_t repinAfter = 6) : escalateAfter_(escalateAfter), repinAfter_(repinAfter) {}

        void SetMode(SwingMode m)
        {
            mode_ = m;
            cur_ = m == SwingMode::MeleeTask ? SwingVariant::MeleeTask : SwingVariant::Upper;
            pinned_ = false;
            run_ = 0;
            meleeTripped_ = false;
        }
        SwingMode Mode() const { return mode_; }
        bool Enabled() const { return mode_ != SwingMode::Off; }
        SwingVariant Current() const { return cur_; }
        bool Pinned() const { return pinned_; }
        bool MeleeTripped() const { return meleeTripped_; }
        uint32_t Escalations() const { return escalations_; }
        uint32_t Exhausted() const { return exhausted_; }

        // The probe's verdict for a swing played with Current(). `excused`: the miss was not the variant's fault (the ped was busy,
        // ragdolling, a reaction owned it). Returns true when this result moved the ladder up one rung.
        bool OnResult(SwingProbe::Result r, bool excused)
        {
            if (mode_ == SwingMode::Off)
                return false;
            if (r == SwingProbe::Result::Shown)
            {
                pinned_ = true;
                run_ = 0;
                return false;
            }
            if (r != SwingProbe::Result::NotShown || excused)
                return false;                                   // unreadable or excused: no evidence either way
            ++run_;
            if (run_ < (pinned_ ? repinAfter_ : escalateAfter_))
                return false;
            run_ = 0;
            return Escalate();
        }
        // No candidate clip dictionary exists in this game (every DOES_ANIM_DICT_EXIST said no): no animation variant can play, and with
        // nothing issued the read-back could never climb the ladder. In auto the swing goes to the melee task at once (it needs no clip);
        // play / off / a tripped breaker stay as they are. True when the ladder moved.
        bool OnNoClips()
        {
            if (mode_ != SwingMode::Auto || meleeTripped_ || cur_ == SwingVariant::MeleeTask)
                return false;
            cur_ = SwingVariant::MeleeTask;
            pinned_ = false;
            run_ = 0;
            ++escalations_;
            return true;
        }
        // The melee-task rung did harm (the local player's health moved while a puppet was in melee): never again this session.
        void TripMeleeTask()
        {
            meleeTripped_ = true;
            if (cur_ == SwingVariant::MeleeTask)
            {
                cur_ = SwingVariant::Upper;                    // back to the animations (the only backward move, and a forced one)
                pinned_ = false;
                run_ = 0;
                if (mode_ == SwingMode::MeleeTask)
                    mode_ = SwingMode::Play;
            }
        }

    private:
        bool Escalate()
        {
            SwingVariant next = cur_;
            switch (cur_)
            {
            case SwingVariant::Upper:     next = SwingVariant::UpperHard; break;
            case SwingVariant::UpperHard: next = SwingVariant::FullBody; break;
            case SwingVariant::FullBody:  next = SwingVariant::MeleeTask; break;
            case SwingVariant::MeleeTask: case SwingVariant::kCount: break;
            }
            if (next == SwingVariant::MeleeTask && (mode_ != SwingMode::Auto || meleeTripped_))
                next = cur_;                                   // play: the animations only; a tripped breaker closes the last rung
            if (next == cur_)
            {
                ++exhausted_;                                  // nothing left to try: the last rung keeps playing
                return false;
            }
            cur_ = next;
            pinned_ = false;
            ++escalations_;
            return true;
        }
        uint32_t escalateAfter_, repinAfter_;
        SwingMode mode_ = SwingMode::Auto;
        SwingVariant cur_ = SwingVariant::Upper;
        bool pinned_ = false, meleeTripped_ = false;
        uint32_t run_ = 0, escalations_ = 0, exhausted_ = 0;
    };

    // The one bounded re-issue: a locomotion task was issued on the ped since the swing, the clip is READ as not playing (0, not unreadable),
    // the swing is young (a punch shown late is better than none, but not after 150 ms) and the retry was not spent. The melee-task rung
    // is never retried (a second task would only restart the puppet's fight).
    constexpr double kSwingRetryWindowMs = 150.0;
    inline bool SwingRetryDue(bool viaMeleeTask, bool retried, int playing, double sinceIssueMs, uint32_t locoNow, uint32_t locoAtIssue)
    {
        return !viaMeleeTask && !retried && playing == 0 && sinceIssueMs <= kSwingRetryWindowMs && locoNow > locoAtIssue;
    }
    // Why a swing the read-back did not see was not seen: the ped was busy (ragdoll, a reaction, a seat: not the swing's fault), a
    // locomotion task was issued meanwhile (or the one retry was spent on one), or nothing explains it (a wrong clip, or a task the native
    // cannot see).
    enum class SwingMiss : uint8_t { Busy, Overwritten, Plain };
    inline SwingMiss ClassifySwingMiss(bool busy, bool retried, uint32_t locoNow, uint32_t locoAtIssue)
    {
        return busy ? SwingMiss::Busy : (retried || locoNow > locoAtIssue) ? SwingMiss::Overwritten : SwingMiss::Plain;
    }

    // Why a swing seen on a puppet did not end as an animation on screen, and what happened to the ones that did. Every counter is
    // the EFFECT (a read-back after the command) except `issued`, which is the command.
    struct SwingDiag
    {
        uint32_t seen = 0;                                      // swing steps on puppets (a shotSeq step or a kPfMelee edge, melee weapon)
        uint32_t notIssuedOwned = 0, notIssuedRagdoll = 0, notIssuedDictMissing = 0, notIssuedDictLate = 0, notIssuedFailed = 0, notIssuedOff = 0;
        uint32_t issued = 0;                                    // TASK_PLAY_ANIM / the melee task was called and accepted
        uint32_t shown = 0;                                     // ... and the read-back SAW it playing
        uint32_t unseen = 0;                                    // ... and the read-back never did within the window ...
        uint32_t unseenOverwritten = 0;                         // ... of which a locomotion task was issued on the ped meanwhile
        uint32_t unseenBusy = 0;                                // ... of which the ped was ragdolling / in a reaction / seated
        uint32_t unseenPlain = 0;                               // ... of which nothing explains it (wrong clip, or a task the native cannot see)
        uint32_t unreadable = 0;                                // the read-back native could not be called
        uint32_t retried = 0, retriedShown = 0;                 // re-issued once after a locomotion task replaced it / and then seen
        uint32_t variantIssued[(size_t)SwingVariant::kCount] = {};
        uint32_t variantShown[(size_t)SwingVariant::kCount] = {};
        uint32_t escalations = 0, meleeTaskNoTarget = 0, meleeTaskHarm = 0;
    };

    inline std::string SwingDiagLine(const SwingDiag& n, const SwingDiag& o, const SwingLadder& ladder)
    {
        char buf[900];
        const auto d = [&](uint32_t a, uint32_t b) { return a - b; };
        std::snprintf(buf, sizeof(buf),
                      "swing steps seen on puppets %u; not issued: ped owned %u, ragdoll %u, dictionary missing %u, dictionary still streaming after "
                      "250 ms %u, call failed %u, melee_anim=off %u; issued %u, SEEN PLAYING %u (read-back IS_ENTITY_PLAYING_ANIM / "
                      "IS_PED_IN_MELEE_COMBAT), not seen %u [a locomotion task was issued meanwhile %u, ped busy %u, no explanation %u], read-back "
                      "unreadable %u; retried after a locomotion task %u (seen %u); by variant issued/seen: upper %u/%u, upper-hard %u/%u, "
                      "full-body %u/%u, melee-task %u/%u; melee_anim=%s, now %s (%s), escalations %u, melee task: no anchor %u, local harm %u%s",
                      d(n.seen, o.seen), d(n.notIssuedOwned, o.notIssuedOwned), d(n.notIssuedRagdoll, o.notIssuedRagdoll),
                      d(n.notIssuedDictMissing, o.notIssuedDictMissing), d(n.notIssuedDictLate, o.notIssuedDictLate),
                      d(n.notIssuedFailed, o.notIssuedFailed), d(n.notIssuedOff, o.notIssuedOff), d(n.issued, o.issued), d(n.shown, o.shown),
                      d(n.unseen, o.unseen), d(n.unseenOverwritten, o.unseenOverwritten), d(n.unseenBusy, o.unseenBusy),
                      d(n.unseenPlain, o.unseenPlain), d(n.unreadable, o.unreadable), d(n.retried, o.retried), d(n.retriedShown, o.retriedShown),
                      d(n.variantIssued[0], o.variantIssued[0]), d(n.variantShown[0], o.variantShown[0]),
                      d(n.variantIssued[1], o.variantIssued[1]), d(n.variantShown[1], o.variantShown[1]),
                      d(n.variantIssued[2], o.variantIssued[2]), d(n.variantShown[2], o.variantShown[2]),
                      d(n.variantIssued[3], o.variantIssued[3]), d(n.variantShown[3], o.variantShown[3]),
                      SwingModeName(ladder.Mode()), SwingVariantName(ladder.Current()), ladder.Pinned() ? "seen playing, pinned" : "still being tried", n.escalations,
                      n.meleeTaskNoTarget, n.meleeTaskHarm, ladder.MeleeTripped() ? " (MELEE TASK DISABLED for this session)" : "");
        return buf;
    }

    // ---- the 10 s animation line --------------------------------------------------------------------------------------

    enum class AnimKind : uint8_t { LocalEmote, PeerEmote, PeerSwing, kCount };

    struct AnimKindStats
    {
        uint32_t requested = 0;            // an animation was asked for (a start, a swing)
        uint32_t dictReady = 0;            // its dictionary was loaded ...
        double   dictReadyMsSum = 0, dictReadyMsMax = 0;   // ... after this long (request to loaded)
        uint32_t dictTimeout = 0, dictMissing = 0;         // never loaded in 3 s / DOES_ANIM_DICT_EXIST said no
        uint32_t issued = 0, failed = 0;   // TASK_PLAY_ANIM called / the call returned false
        uint32_t shown = 0, unseen = 0;    // IS_ENTITY_PLAYING_ANIM true after the issue / never true
        uint32_t reissued = 0, restarted = 0, owned = 0, late = 0;   // loop issued again, replaced, swing not shown (ped owned), swing played late
        uint32_t ended[8] = {};            // by EmoteEnd (None unused)
        uint32_t cancelled = 0;            // an emote the game ended under us that was not a stop
    };

    struct AnimDiag
    {
        AnimKindStats k[(size_t)AnimKind::kCount];
        AnimKindStats& Of(AnimKind a) { return k[(size_t)a]; }
        const AnimKindStats& Of(AnimKind a) const { return k[(size_t)a]; }
    };

    inline const char* AnimKindName(AnimKind a)
    {
        return a == AnimKind::LocalEmote ? "emote-ours" : a == AnimKind::PeerEmote ? "emote-peer" : "swing-peer";
    }

    // The activity since `last` (counters subtract, the maxima are the totals').
    inline AnimKindStats AnimDelta(const AnimKindStats& now, const AnimKindStats& last)
    {
        AnimKindStats d = now;
        d.requested -= last.requested;
        d.dictReady -= last.dictReady;
        d.dictReadyMsSum -= last.dictReadyMsSum;
        d.dictTimeout -= last.dictTimeout;
        d.dictMissing -= last.dictMissing;
        d.issued -= last.issued;
        d.failed -= last.failed;
        d.shown -= last.shown;
        d.unseen -= last.unseen;
        d.reissued -= last.reissued;
        d.restarted -= last.restarted;
        d.owned -= last.owned;
        d.late -= last.late;
        d.cancelled -= last.cancelled;
        for (size_t i = 0; i < 8; ++i)
            d.ended[i] -= last.ended[i];
        return d;
    }

    inline bool AnimActive(const AnimKindStats& d)
    {
        if (d.requested || d.dictReady || d.dictTimeout || d.dictMissing || d.issued || d.failed || d.shown || d.unseen ||
            d.reissued || d.restarted || d.owned || d.late || d.cancelled)
            return true;
        for (size_t i = 0; i < 8; ++i)
            if (d.ended[i])
                return true;
        return false;
    }

    // One bracket per kind that did anything: [kind req=N dict_ready=N avg_ms=A max_ms=M dict_timeout=N dict_missing=N
    // issued=N failed=N shown=N unseen=N reissued=N restarted=N late=N owned=N ended(dur=N stop=N dead=N veh=N notplay=N
    // dict=N cap=N)]. "idle" when no kind did.
    inline std::string AnimDiagLine(const AnimDiag& now, const AnimDiag& last)
    {
        std::string out;
        char buf[480];
        for (size_t i = 0; i < (size_t)AnimKind::kCount; ++i)
        {
            const AnimKindStats d = AnimDelta(now.k[i], last.k[i]);
            if (!AnimActive(d))
                continue;
            const double avg = d.dictReady ? d.dictReadyMsSum / d.dictReady : 0.0;
            std::snprintf(buf, sizeof(buf),
                          "%s[%s req=%u dict_ready=%u avg_ms=%.0f max_ms=%.0f dict_timeout=%u dict_missing=%u issued=%u "
                          "failed=%u shown=%u unseen=%u reissued=%u restarted=%u late=%u owned=%u cancelled=%u "
                          "ended(dur=%u stop=%u dead=%u veh=%u notplay=%u dict=%u cap=%u)]",
                          out.empty() ? "" : " ", AnimKindName((AnimKind)i), d.requested, d.dictReady, avg,
                          d.dictReadyMsMax, d.dictTimeout, d.dictMissing, d.issued, d.failed, d.shown, d.unseen,
                          d.reissued, d.restarted, d.late, d.owned, d.cancelled, d.ended[(size_t)EmoteEnd::Duration],
                          d.ended[(size_t)EmoteEnd::Stop], d.ended[(size_t)EmoteEnd::Dead], d.ended[(size_t)EmoteEnd::Vehicle],
                          d.ended[(size_t)EmoteEnd::NotPlaying], d.ended[(size_t)EmoteEnd::DictTimeout],
                          d.ended[(size_t)EmoteEnd::OneShotCap]);
            out += buf;
        }
        return out.empty() ? std::string("idle") : out;
    }

    // ---- "first use of this animation" logging -------------------------------------------------------------------------
    // True the first time a key is noted (capped: past the cap nothing new is logged, the line count stays bounded).
    class FirstUse
    {
    public:
        explicit FirstUse(size_t cap = 48) : cap_(cap) {}
        bool Note(const std::string& key)
        {
            if (seen_.size() >= cap_ || seen_.count(key))
                return false;
            seen_.insert(key);
            return true;
        }
        size_t Size() const { return seen_.size(); }
    private:
        size_t cap_;
        std::set<std::string> seen_;
    };

}
