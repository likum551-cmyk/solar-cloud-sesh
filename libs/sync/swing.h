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
