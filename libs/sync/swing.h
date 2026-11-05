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
    //
    // R4 T3. "Playing" at the first frame is NOT "visible": live 0.4.8 read 6/6 playing and GI still saw no punch. The probe
    // therefore FOLLOWS the swing after the first sighting (Begin with follow = true): the read-back is sampled again at +100,
    // +250 and +450 ms from the issue (the first frame at or after each), together with the anim's current / total time when
    // the two natives exist (GET_ENTITY_ANIM_CURRENT_TIME / _TOTAL_TIME: NOT in hook.h yet, the samples then carry -1 = unknown).
    // A clip that is playing at the first frame and gone by +100 / +250 was blended out or replaced (BlendedOut); 1,0,1 is
    // Flicker; a current time that does not advance is Frozen; playing at both must-play samples is Progressed. The +450 sample
    // is informational (the task should be over by then when the swing has a duration: "tail still playing" = it holds or loops).
    // Active() covers the WHOLE window including the follow, so the locomotion controller can stay away from the ped (Task 1's
    // LocoInput::swingActive reads it): from the issue until the swing is expected to be over (the duration + 60 ms, 1100 ms for a
    // whole-clip swing, or the holdMs the engine gives the melee task) and never before the last progress sample is in. Without follow the
    // probe is the 0e one (Active() false at the first sighting).
    class SwingProbe
    {
    public:
        enum class Result : uint8_t { Idle, Pending, Shown, NotShown, Unknown, Following, Followed };
        enum class Progress : uint8_t { None, Progressed, BlendedOut, Flicker, Frozen, Unread };
        static constexpr double kWindowMs = 250.0;
        static constexpr size_t kSamples = 3;
        static constexpr double kSampleAtMs[kSamples] = { 100.0, 250.0, 450.0 };
        static constexpr double kFollowMaxMs = 700.0;      // the follow gives up this long after the issue (a stalled frame loop)

        struct Sample
        {
            bool taken = false;
            double atMs = 0;       // since the issue
            int playing = -1;      // 1 yes, 0 no, -1 cannot be read
            float cur = -1, total = -1;   // seconds, -1 unknown
        };

        static constexpr double kWholeClipHoldMs = 1100.0;   // a whole-clip swing (duration -1) is assumed over by then
        void Begin(double nowMs, size_t candidate, bool follow = false, int durationMs = 400, double holdMs = -1.0)
        {
            holdMs_ = holdMs >= 0.0 ? holdMs : durationMs < 0 ? kWholeClipHoldMs : (double)durationMs + 60.0;
            verdictGiven_ = false;
            active_ = true;
            startMs_ = nowMs;
            candidate_ = candidate;
            sawNo_ = false;
            follow_ = follow;
            shown_ = false;
            durationMs_ = durationMs;
            progress_ = Progress::None;
            first_ = Sample();
            for (Sample& s : samples_)
                s = Sample();
        }
        bool Active() const { return active_; }
        size_t Candidate() const { return candidate_; }
        // playing: 1 yes, 0 no, -1 cannot be read. Shown on the first yes; after the window NotShown when the ped was seen
        // NOT playing it, Unknown when no read ever answered. With follow, the Ticks after Shown answer Following until the
        // last sample is in (or kFollowMaxMs), then Followed once (Verdict() says how the swing went).
        Result Tick(double nowMs, int playing, float curTime = -1.0f, float totalTime = -1.0f)
        {
            if (!active_)
                return Result::Idle;
            const double since = nowMs - startMs_;
            if (follow_)
                for (size_t i = 0; i < kSamples; ++i)
                    if (!samples_[i].taken && since >= kSampleAtMs[i])
                    {
                        samples_[i].taken = true;
                        samples_[i].atMs = since;
                        samples_[i].playing = playing;
                        samples_[i].cur = curTime;
                        samples_[i].total = totalTime;
                        break;                                  // one sample per Tick: a stalled frame does not fake three reads
                    }
            if (!shown_)
            {
                if (playing == 1)
                {
                    shown_ = true;
                    first_.taken = true;
                    first_.atMs = since;
                    first_.playing = 1;
                    first_.cur = curTime;
                    first_.total = totalTime;
                    if (!follow_)
                        active_ = false;
                    return Result::Shown;
                }
                if (playing == 0)
                    sawNo_ = true;
                if (since >= kWindowMs)
                {
                    active_ = false;
                    return sawNo_ ? Result::NotShown : Result::Unknown;
                }
                return Result::Pending;
            }
            if (!verdictGiven_ && (samples_[kSamples - 1].taken || since >= kFollowMaxMs))
            {
                verdictGiven_ = true;
                progress_ = JudgeProgress();
                if (since >= holdMs_)
                    active_ = false;
                return Result::Followed;
            }
            if (verdictGiven_ && since >= holdMs_)
                active_ = false;                                // the swing is over: Active() ends, Following once more, Idle after
            return Result::Following;
        }
        void Cancel() { active_ = false; }

        // How the followed swing went (valid after Followed).
        Progress Verdict() const { return progress_; }
        const Sample& First() const { return first_; }
        const Sample& At(size_t i) const { return samples_[i < kSamples ? i : kSamples - 1]; }
        int DurationMs() const { return durationMs_; }
        // The +450 ms sample reads playing although a timed swing should be over: the clip holds or loops.
        bool TailStillPlaying() const { return samples_[2].taken && samples_[2].playing == 1 && durationMs_ > 0 && durationMs_ < 450; }
        // The clip is known to be longer than the time it is given (total time from the native): it is cut short.
        bool ClipCutShort() const
        {
            const float total = first_.total > 0 ? first_.total : samples_[0].total;
            return total > 0 && durationMs_ > 0 && (double)durationMs_ < 800.0 * (double)total;   // under 80 % of the clip's length
        }
        static const char* ProgressName(Progress p)
        {
            return p == Progress::Progressed ? "progressed" : p == Progress::BlendedOut ? "BLENDED OUT" : p == Progress::Flicker ? "FLICKER"
                 : p == Progress::Frozen ? "FROZEN (anim time not advancing)" : p == Progress::Unread ? "unreadable" : "-";
        }

    private:
        // The two must-play samples (+100, +250) decide. A 0 followed by a later 1 is a flicker, a 0 that stays is a blend-out;
        // reads that never answered are Unread; times that do not advance are Frozen.
        Progress JudgeProgress() const
        {
            bool anyRead = false;
            for (const Sample& s : samples_)
                anyRead = anyRead || (s.taken && s.playing >= 0);
            if (!anyRead)
                return Progress::Unread;
            for (size_t i = 0; i < 2; ++i)
                if (samples_[i].taken && samples_[i].playing == 0)
                {
                    for (size_t j = i + 1; j < kSamples; ++j)
                        if (samples_[j].taken && samples_[j].playing == 1)
                            return Progress::Flicker;
                    return Progress::BlendedOut;
                }
            // times: from the first sighting and the playing samples; the anim must move at about real time (>= a quarter of it)
            double t0 = -1, c0 = -1, t1 = -1, c1 = -1;
            const Sample* all[4] = { &first_, &samples_[0], &samples_[1], &samples_[2] };
            for (const Sample* s : all)
                if (s->taken && s->playing == 1 && s->cur >= 0.0f)
                {
                    if (t0 < 0) { t0 = s->atMs; c0 = s->cur; }
                    t1 = s->atMs;
                    c1 = s->cur;
                }
            if (t0 >= 0 && t1 - t0 >= 80.0 && (c1 - c0) < 0.25 * (t1 - t0) / 1000.0)
                return Progress::Frozen;
            return Progress::Progressed;
        }
        bool active_ = false, sawNo_ = false, follow_ = false, shown_ = false, verdictGiven_ = false;
        double startMs_ = 0, holdMs_ = 460.0;
        size_t candidate_ = 0;
        int durationMs_ = 400;
        Progress progress_ = Progress::None;
        Sample first_;
        Sample samples_[kSamples];
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
    //
    // R5 (live 0.4.26: 8 swings issued, 7 SEEN playing at the first frame, 0 still playing at +100 / +250 ms, 4 BLENDED OUT, and GI saw no punch): the
    // two variants the new plan puts FIRST (melee_loop, default on; 0 = the round-4 plan and ladder, byte for byte):
    //   Looped     flags 49 = 1 (loop) | 16 | 32: the clip LOOPS, so nothing can blend it out before we end it; the engine ends it itself with
    //              STOP_ANIM_TASK at the swing's end (SwingAttempt::stopAfterMs, ~480 ms; SwingLoopStop schedules and checks it)
    //   Plain      flags 0: the clip as the game plays it by default (a task that replaces the gait task for its length, like FullBody)
    // They are appended AFTER MeleeTask: the round-4 numbering (and every array indexed by the variant) is unchanged.
    enum class SwingVariant : uint8_t { Upper, UpperHard, FullBody, MeleeTask, Looped, Plain, kCount };

    inline int SwingVariantFlags(SwingVariant v)
    {
        return v == SwingVariant::Upper ? (16 | 32) : v == SwingVariant::UpperHard ? (16 | 32 | 8) : v == SwingVariant::FullBody ? 8
             : v == SwingVariant::Looped ? (1 | 16 | 32) : 0;
    }
    inline const char* SwingVariantName(SwingVariant v)
    {
        return v == SwingVariant::Upper ? "upper-body (16|32)" : v == SwingVariant::UpperHard ? "upper-body, not interruptable (16|32|8)"
             : v == SwingVariant::FullBody ? "full body (8)" : v == SwingVariant::Looped ? "looped upper-body (1|16|32), stopped by us"
             : v == SwingVariant::Plain ? "plain (flags 0)" : "melee task (TASK_PUT_PED_DIRECTLY_INTO_MELEE)";
    }
    // A variant whose clip replaces the puppet's gait task (the controller issues it again when the swing is over).
    inline bool SwingVariantReplacesGait(SwingVariant v) { return v == SwingVariant::FullBody || v == SwingVariant::Plain; }
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

        // loop (R5, melee_loop): the animation rungs start with the looped clip and the plain one (Looped, Plain, then the round-4 Upper, UpperHard, FullBody)
        void SetMode(SwingMode m, bool loop = false)
        {
            mode_ = m;
            loop_ = loop;
            cur_ = m == SwingMode::MeleeTask ? SwingVariant::MeleeTask : FirstAnimation();
            pinned_ = false;
            run_ = 0;
            meleeTripped_ = false;
        }
        SwingMode Mode() const { return mode_; }
        bool Loop() const { return loop_; }
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
        // R4 T3: the survey (SwingSurvey) found the variant whose swing PROGRESSED: the ladder starts there, pinned. A tripped breaker
        // refuses the melee rung; play mode refuses it as well.
        bool PinTo(SwingVariant v)
        {
            if (mode_ == SwingMode::Off)
                return false;
            if (v == SwingVariant::MeleeTask && (mode_ != SwingMode::Auto || meleeTripped_))
                return false;
            cur_ = v;
            pinned_ = true;
            run_ = 0;
            return true;
        }
        // The melee-task rung did harm (the local player's health moved while a puppet was in melee): never again this session.
        void TripMeleeTask()
        {
            meleeTripped_ = true;
            if (cur_ == SwingVariant::MeleeTask)
            {
                cur_ = FirstAnimation();                       // back to the animations (the only backward move, and a forced one)
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
            case SwingVariant::Looped:    next = SwingVariant::Plain; break;
            case SwingVariant::Plain:     next = SwingVariant::Upper; break;
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
        SwingVariant FirstAnimation() const { return loop_ ? SwingVariant::Looped : SwingVariant::Upper; }
        uint32_t escalateAfter_, repinAfter_;
        SwingMode mode_ = SwingMode::Auto;
        SwingVariant cur_ = SwingVariant::Upper;
        bool pinned_ = false, meleeTripped_ = false, loop_ = false;
        uint32_t run_ = 0, escalations_ = 0, exhausted_ = 0;
    };

    // ---- the explicit end of a looped swing (R5, melee_loop) ----------------------------------------------------------------------
    //
    // A looped clip (flags 1) never ends by itself, so the engine ends it: STOP_ANIM_TASK `afterMs` after the issue, then the read-back
    // (IS_ENTITY_PLAYING_ANIM) says whether the clip is gone. Pure: the clock and the read-back come in as arguments.
    //   Gone          the clip is not playing any more: done
    //   StillPlaying  still playing kCheckAfterMs after the stop (the native did nothing on this build): the engine clears the puppet's tasks instead
    //   Unread        the read-back could not be called: the engine clears the tasks too (a loop it cannot see must not run for ever)
    class SwingLoopStop
    {
    public:
        enum class Outcome : uint8_t { Idle, Pending, Gone, StillPlaying, Unread };
        static constexpr double kCheckAfterMs = 80.0;     // the blend-out of a stopped clip takes a few frames
        void Arm(double nowMs, int afterMs)
        {
            armed_ = afterMs > 0;
            stopAtMs_ = nowMs + (afterMs > 0 ? afterMs : 0);
            stoppedAtMs_ = -1.0;
        }
        void Cancel() { armed_ = false; stoppedAtMs_ = -1.0; }
        bool Armed() const { return armed_; }
        // the stop was called and the read-back is awaited
        bool Awaiting() const { return armed_ && stoppedAtMs_ >= 0.0; }
        // the first call at or after the stop time (the engine calls STOP_ANIM_TASK and says so with Stopped())
        bool Due(double nowMs) const { return armed_ && stoppedAtMs_ < 0.0 && nowMs >= stopAtMs_; }
        void Stopped(double nowMs) { stoppedAtMs_ = nowMs; }
        // playing: 1 yes, 0 no, -1 unreadable. Gone / StillPlaying / Unread disarm; Pending = ask again on the next frame.
        Outcome Check(double nowMs, int playing)
        {
            if (!Awaiting())
                return Outcome::Idle;
            if (playing == 0)
            {
                armed_ = false;
                return Outcome::Gone;
            }
            if (nowMs - stoppedAtMs_ < kCheckAfterMs)
                return Outcome::Pending;
            armed_ = false;
            return playing == 1 ? Outcome::StillPlaying : Outcome::Unread;
        }

    private:
        bool armed_ = false;
        double stopAtMs_ = 0, stoppedAtMs_ = -1.0;
    };

    // ---- the swing survey (R4 T3) -----------------------------------------------------------------------------------------
    //
    // The R3 ladder pins the FIRST variant the read-back sees playing, so a live test only ever exercised one (live 0.4.8: "upper-body
    // 16|32" pinned after 6/6, and GI still saw no punch). Whether a playing swing is VISIBLE cannot be decided offline: the survey makes
    // one test decide it. For the first swings of a session (one cycle of the plan by default, melee_survey=N cycles, 0 = off) each swing
    // is issued as the next entry of a fixed plan, whatever was seen before, and every attempt is followed by the progress check
    // (SwingProbe, +100 / +250 / +450 ms). The log has one line per swing naming the attempt and its read-backs; the overlay names it
    // while it plays so GI can say which attempts he saw. Afterwards the ladder is pinned to the attempt whose swings PROGRESSED.
    //
    // 0.4.30: TASK_PLAY_ANIM's 8th argument is the clip's START PHASE (0..1; ox_lib / the R* header: startPhase), not a playback rate. 1.0
    // started every clip at its END: a one-shot read "playing" for one frame and was gone the next (0.4.12-0.4.29 traces: 1 -> 0 at frame 2,
    // "phase never read"), a looped one wrapped round and was the only variant that showed. anim_phase_fix=0 = 1.0 again.
    constexpr float kAnimStartPhase = 0.0f;
    constexpr float kAnimStartPhase0429 = 1.0f;
    inline float AnimStartPhase(bool phaseFix) { return phaseFix ? kAnimStartPhase : kAnimStartPhase0429; }
    struct SwingAttempt
    {
        SwingVariant variant = SwingVariant::Upper;
        int durationMs = 400;          // TASK_PLAY_ANIM duration: kSwingClipLength = the clip's own length
        int stopAfterMs = 0;           // R5: > 0 = the clip loops and the engine ends it (STOP_ANIM_TASK) this long after the issue (SwingLoopStop)
    };
    constexpr int kSwingClipLength = -1;
    constexpr int kLoopStopAfterMs = 480;        // after the +450 ms read-back, so the progress check still sees the loop playing
    constexpr size_t kSurveyPlan = 5;            // the round-4 plan
    constexpr size_t kSurveyPlanLoop = 7;        // R5 (melee_loop): the looped and the plain clip first
    constexpr size_t kSurveyPlanMax = 7;
    inline size_t SurveyPlanSize(bool loop) { return loop ? kSurveyPlanLoop : kSurveyPlan; }
    inline SwingAttempt SurveyPlanAt(size_t i, bool loop)
    {
        if (loop)
        {
            // R5: the looped clip cannot be blended out before we end it, the plain one is what the game plays by default; the round-4 attempts follow
            static const SwingAttempt kLoopPlan[kSurveyPlanLoop] = { { SwingVariant::Looped, kSwingClipLength, kLoopStopAfterMs },
                                                                     { SwingVariant::Plain, kSwingClipLength, 0 },
                                                                     { SwingVariant::Upper, 400, 0 },
                                                                     { SwingVariant::Upper, kSwingClipLength, 0 },
                                                                     { SwingVariant::UpperHard, 400, 0 },
                                                                     { SwingVariant::FullBody, kSwingClipLength, 0 },
                                                                     { SwingVariant::MeleeTask, 400, 0 } };
            return kLoopPlan[i % kSurveyPlanLoop];
        }
        // Order = preference on a tie: the shipped behaviour first, then the same flags for the whole clip (a 400 ms cut of a longer punch
        // shows the wind-up only), the not-interruptable upper body, the whole body, and the melee task last (it can only be taken with an anchor).
        static const SwingAttempt kPlan[kSurveyPlan] = { { SwingVariant::Upper, 400, 0 },
                                                         { SwingVariant::Upper, kSwingClipLength, 0 },
                                                         { SwingVariant::UpperHard, 400, 0 },
                                                         { SwingVariant::FullBody, kSwingClipLength, 0 },
                                                         { SwingVariant::MeleeTask, 400, 0 } };
        return kPlan[i % kSurveyPlan];
    }
    inline SwingAttempt SurveyPlanAt(size_t i) { return SurveyPlanAt(i, false); }

    class SwingSurvey
    {
    public:
        // cycles: how many passes over the plan (0 = no survey).
        // loop (R5, melee_loop): the plan of SurveyPlanAt(i, true), 7 entries, the looped clip first.
        void Start(uint32_t cycles, bool loop = false) { *this = SwingSurvey(); cycles_ = cycles; loop_ = loop; }
        bool Loop() const { return loop_; }
        size_t PlanSize() const { return SurveyPlanSize(loop_); }
        SwingAttempt PlanAt(size_t i) const { return SurveyPlanAt(i, loop_); }
        bool Active() const { return cycles_ > 0 && (step_ < (size_t)cycles_ * PlanSize() || nRequeue_ > 0); }
        bool Done() const { return cycles_ > 0 && step_ >= (size_t)cycles_ * PlanSize() && nRequeue_ == 0; }
        uint32_t Cycles() const { return cycles_; }
        size_t Step() const { return step_; }
        size_t Planned() const { return (size_t)cycles_ * PlanSize(); }
        // The attempt for the next swing. The melee-task entry is skipped (and counted as a step) when it cannot be taken: that is decided
        // here and right after each issue, so a trailing unavailable entry never keeps the survey open. Out: the plan index of the attempt,
        // left unchanged when nothing was left to issue (only skipped entries remained: the caller plays an ordinary swing).
        SwingAttempt Next(bool meleeTaskOk, size_t* planIdx = nullptr)
        {
            meleeOk_ = meleeTaskOk;
            SkipUnavailable();
            if (step_ >= Planned() && nRequeue_ > 0)
            {
                // after the plan: an attempt that gave no evidence (excused: the swing was overtaken by the next one, the ped ragdolled, the read-back was unreadable) comes back
                const size_t idx = requeue_[0];
                for (size_t i = 1; i < nRequeue_; ++i)
                    requeue_[i - 1] = requeue_[i];
                --nRequeue_;
                ++outstanding_;
                ++e_[idx].issued;
                if (planIdx)
                    *planIdx = idx;
                return PlanAt(idx);
            }
            if (step_ >= Planned())
                return PlanAt(0);
            const size_t idx = step_ % PlanSize();
            ++step_;
            ++outstanding_;
            ++e_[idx].issued;
            if (planIdx)
                *planIdx = idx;
            SkipUnavailable();
            return PlanAt(idx);
        }
        // The followed result of an attempt (Followed with progress p, or NotShown / Unknown at the first sight: seen = false).
        // excused: no evidence either way (the ped was busy, the swing was overtaken, the read-back could not be read): the attempt is not counted
        // and goes back to the queue, at most kMaxRequeues times in all so a read-back that never answers cannot keep the survey open for good.
        void OnResult(size_t planIdx, bool seen, SwingProbe::Progress p, bool excused)
        {
            if (planIdx >= PlanSize())
                return;
            if (outstanding_)
                --outstanding_;
            Entry& e = e_[planIdx];
            if (excused)
            {
                ++e.excused;
                if (e.issued)
                    --e.issued;
                if (requeues_ < kMaxRequeues && nRequeue_ < kMaxRequeues)
                {
                    ++requeues_;
                    requeue_[nRequeue_++] = (uint8_t)planIdx;
                }
                return;
            }
            if (!seen)
                ++e.unseen;
            else if (p == SwingProbe::Progress::Progressed)
                ++e.progressed;
            else if (p == SwingProbe::Progress::Unread)
                ++e.unread;
            else
                ++e.failed;                                   // blended out / flicker / frozen
        }
        uint32_t Issued(size_t i) const { return i < PlanSize() ? e_[i].issued : 0; }
        uint32_t Progressed(size_t i) const { return i < PlanSize() ? e_[i].progressed : 0; }
        uint32_t Failed(size_t i) const { return i < PlanSize() ? e_[i].failed : 0; }
        uint32_t Unseen(size_t i) const { return i < PlanSize() ? e_[i].unseen : 0; }
        uint32_t Outstanding() const { return outstanding_; }
        uint32_t Skipped() const { return skipped_; }
        // Once, when the plan is done and every attempt reported: the winner's plan index, or -1 (nothing progressed). An
        // animation that progressed beats the melee task (it needs an anchor and carries the harm risk); on a tie the earlier entry.
        int TakeWinner()
        {
            if (!Done() || outstanding_ || taken_)
                return -2;
            taken_ = true;
            int best = -1;
            double bestScore = 0.0;
            for (size_t i = 0; i < PlanSize(); ++i)
            {
                if (PlanAt(i).variant == SwingVariant::MeleeTask || !e_[i].issued)
                    continue;
                const double score = (double)e_[i].progressed / (double)e_[i].issued;
                if (score > bestScore + 1e-9)
                {
                    bestScore = score;
                    best = (int)i;
                }
            }
            if (best < 0 && e_[PlanSize() - 1].issued && e_[PlanSize() - 1].progressed)
                best = (int)(PlanSize() - 1);
            winner_ = best;
            return best;
        }
        int Winner() const { return winner_; }

    private:
        void SkipUnavailable()
        {
            while (step_ < Planned() && PlanAt(step_ % PlanSize()).variant == SwingVariant::MeleeTask && !meleeOk_)
            {
                ++skipped_;
                ++step_;
            }
        }
        struct Entry { uint32_t issued = 0, progressed = 0, failed = 0, unseen = 0, unread = 0, excused = 0; };
        static constexpr uint32_t kMaxRequeues = 4;
        bool meleeOk_ = false, loop_ = false;
        uint8_t requeue_[kMaxRequeues] = {};
        size_t nRequeue_ = 0;
        uint32_t requeues_ = 0;
        uint32_t cycles_ = 0;
        size_t step_ = 0;
        uint32_t outstanding_ = 0, skipped_ = 0;
        bool taken_ = false;
        int winner_ = -1;
        Entry e_[kSurveyPlanMax];
    };

    inline std::string SurveyAttemptName(const SwingAttempt& a)
    {
        char b[96];
        if (a.variant == SwingVariant::MeleeTask)
            std::snprintf(b, sizeof(b), "%s", SwingVariantName(a.variant));
        else
            std::snprintf(b, sizeof(b), "%s, %s", SwingVariantName(a.variant), a.durationMs < 0 ? "the clip's whole length" : (std::to_string(a.durationMs) + " ms").c_str());
        return b;
    }

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
        // R4 T3: the progress check after the first sighting (+100 / +250 / +450 ms read-backs): of the swings SEEN playing at the first
        // frame, how many were still playing at +100 and +250 (progressed), were gone by then (blended out), flickered, had an anim time that
        // did not advance (frozen), or could not be read again; `tail` = still playing at +450 although a timed swing should be over.
        uint32_t progressed = 0, blendedOut = 0, flicker = 0, frozen = 0, progressUnread = 0, tail = 0, cutShort = 0;
        uint32_t variantProgressed[(size_t)SwingVariant::kCount] = {};
        uint32_t surveyed = 0;                                  // swings issued by the survey plan
        // R5 (melee_loop): the looped swing's explicit end (STOP_ANIM_TASK), what the read-back said after it, and the anchorless melee task
        uint32_t loopStops = 0, loopGone = 0, loopStill = 0, loopUnread = 0, loopAbandoned = 0;
        uint32_t anchorlessSpawned = 0, anchorlessFailed = 0, anchorlessRemoved = 0;
    };

    inline std::string SwingDiagLine(const SwingDiag& n, const SwingDiag& o, const SwingLadder& ladder)
    {
        char buf[1500];
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
        std::string out = buf;
        char b2[700];
        std::snprintf(b2, sizeof(b2),
                      "; PROGRESS (R4 T3, read-back again at +100 / +250 / +450 ms): of the swings seen at the first frame, still playing at +100 and +250 %u "
                      "[upper %u, upper-hard %u, full-body %u, melee-task %u], BLENDED OUT %u, flicker %u, anim time frozen %u, not readable again %u; "
                      "+450 ms still playing (a timed swing should be over) %u; clip known to be longer than its duration %u; survey swings %u",
                      d(n.progressed, o.progressed), d(n.variantProgressed[0], o.variantProgressed[0]), d(n.variantProgressed[1], o.variantProgressed[1]),
                      d(n.variantProgressed[2], o.variantProgressed[2]), d(n.variantProgressed[3], o.variantProgressed[3]), d(n.blendedOut, o.blendedOut),
                      d(n.flicker, o.flicker), d(n.frozen, o.frozen), d(n.progressUnread, o.progressUnread), d(n.tail, o.tail), d(n.cutShort, o.cutShort),
                      d(n.surveyed, o.surveyed));
        out += b2;
        char b3[700];
        std::snprintf(b3, sizeof(b3),
                      "; LOOP (R5): looped swings issued/seen/progressed %u/%u/%u, plain %u/%u/%u; the explicit end: STOP_ANIM_TASK called %u, clip GONE after it %u, STILL PLAYING after it %u "
                      "(tasks cleared instead), unreadable %u (tasks cleared), abandoned in flight %u; melee task without an anchor ped: anchor spawned %u, spawn failed %u, anchors removed %u",
                      d(n.variantIssued[4], o.variantIssued[4]), d(n.variantShown[4], o.variantShown[4]), d(n.variantProgressed[4], o.variantProgressed[4]),
                      d(n.variantIssued[5], o.variantIssued[5]), d(n.variantShown[5], o.variantShown[5]), d(n.variantProgressed[5], o.variantProgressed[5]),
                      d(n.loopStops, o.loopStops), d(n.loopGone, o.loopGone), d(n.loopStill, o.loopStill), d(n.loopUnread, o.loopUnread), d(n.loopAbandoned, o.loopAbandoned),
                      d(n.anchorlessSpawned, o.anchorlessSpawned), d(n.anchorlessFailed, o.anchorlessFailed), d(n.anchorlessRemoved, o.anchorlessRemoved));
        return out + b3;
    }

    // ---- the per-swing line (R4 T3) ------------------------------------------------------------------------------------------
    // One line per followed swing, in the log: the attempt (variant, flags, duration), the clip, when it started, the read-backs at the first
    // frame and at +100 / +250 / +450 ms (with the anim's current time when the native exists), whether a locomotion task was issued
    // meanwhile, and the verdict. The next live log then says WHICH swing was blended out, when, and by what.
    struct SwingLineIn
    {
        int peerId = 0;
        uint32_t index = 0;                  // the swing's number this session (the survey step when surveyed)
        int surveyStep = -1, surveySteps = 0;   // -1 = not a survey swing
        SwingVariant variant = SwingVariant::Upper;
        int flags = 0, durationMs = 400;
        int stopAfterMs = 0;                 // R5: > 0 = looped, ended by us this long after the issue
        const char* dict = "";
        const char* clip = "";
        double startMs = 0;                  // the issue time (the engine's clock)
        bool retried = false;
        uint32_t locoTasksMeanwhile = 0;     // locomotion tasks issued on the ped between the issue and the end of the follow
        bool busy = false;                   // ragdoll / reaction / seat at the end
        SwingProbe::Result firstResult = SwingProbe::Result::Idle;   // Shown / NotShown / Unknown
        SwingProbe::Progress progress = SwingProbe::Progress::None;
        bool tailPlaying = false, cutShort = false;
        SwingProbe::Sample first;
        SwingProbe::Sample at[SwingProbe::kSamples];
    };
    inline std::string SwingLine(const SwingLineIn& in)
    {
        const auto rd = [](const SwingProbe::Sample& s, char* out, size_t n) {
            if (!s.taken)
                std::snprintf(out, n, "-");
            else if (s.cur >= 0.0f)
                std::snprintf(out, n, "%s (anim %.3f s)", s.playing == 1 ? "playing" : s.playing == 0 ? "NOT playing" : "unreadable", (double)s.cur);
            else
                std::snprintf(out, n, "%s", s.playing == 1 ? "playing" : s.playing == 0 ? "NOT playing" : "unreadable");
        };
        char f0[48], f1[48], f2[48], f3[48], buf[900];
        rd(in.first, f0, sizeof f0);
        rd(in.at[0], f1, sizeof f1);
        rd(in.at[1], f2, sizeof f2);
        rd(in.at[2], f3, sizeof f3);
        char head[96];
        if (in.surveyStep >= 0)
            std::snprintf(head, sizeof head, "survey %d of %d", in.surveyStep + 1, in.surveySteps);
        else
            std::snprintf(head, sizeof head, "swing");
        std::snprintf(buf, sizeof(buf),
                      "engine: melee swing #%u (%s) on peer id=%d: %s, flags %d, duration %s%s, %s / %s (INFERRED names), started at %.0f ms%s; read-back: "
                      "first sighting %s%s, +100 ms %s, +250 ms %s, +450 ms %s; a locomotion task was issued meanwhile %s (%u); ped busy %s; first sighting: %s; "
                      "verdict: %s%s%s",
                      in.index, head, in.peerId, SwingVariantName(in.variant), in.flags,
                      in.durationMs < 0 ? "the clip's whole length" : (std::to_string(in.durationMs) + " ms").c_str(),
                      in.stopAfterMs > 0 ? (std::string(" (looped, ended by us at +") + std::to_string(in.stopAfterMs) + " ms)").c_str() : "", in.dict, in.clip, in.startMs,
                      in.retried ? " (re-issued once)" : "", f0,
                      in.first.taken ? (std::string(" at +") + std::to_string((int)in.first.atMs) + " ms").c_str() : "", f1, f2, f3,
                      in.locoTasksMeanwhile ? "YES" : "no", in.locoTasksMeanwhile, in.busy ? "yes" : "no",
                      in.firstResult == SwingProbe::Result::Shown ? "seen playing" : in.firstResult == SwingProbe::Result::NotShown ? "NOT seen playing" : "unreadable",
                      SwingProbe::ProgressName(in.progress), in.tailPlaying ? ", still playing at +450 ms (holds or loops)" : "",
                      in.cutShort ? ", the clip is longer than its duration (cut short)" : "");
        return buf;
    }

    // ---- the line for OUR swing (R4 T3) ----------------------------------------------------------------------------------------
    // GI swung 11 times in 0.4.9 and the line said "windows opened on a puppet 0": nothing said why. One line per counted swing (the first
    // 30, then one per 10 s): how many peers are known and drawn, the nearest drawn puppet and how far the PUPPET is from where its owner's
    // pose says it should be (the displacement that sends a punch to the wrong place), and whether the swing opened a hit window on it.
    struct OwnSwingIn
    {
        uint32_t index = 0;
        uint32_t peers = 0, spawned = 0, hittable = 0;   // known peers / with a ped / with a hit pool (a window can open on them)
        int nearestId = -1;
        float nearestM = -1;                  // our ped to the nearest drawn puppet (-1: none)
        float poseErrM = -1;                  // that puppet to its owner's current pose (-1: unknown)
        float poseDistM = -1;                 // our ped to that owner's pose, i.e. where the friend really is (-1: unknown)
        double sinceLastMs = -1;              // since our previous counted swing
    };
    inline std::string OwnSwingLine(const OwnSwingIn& in)
    {
        char buf[600];
        if (in.nearestId < 0)
            std::snprintf(buf, sizeof(buf),
                          "engine: melee: our swing #%u: NO puppet to hit (peers known %u, drawn %u, hittable %u): nothing can land, the server judges the swing on its own tracks",
                          in.index, in.peers, in.spawned, in.hittable);
        else
            std::snprintf(buf, sizeof(buf),
                          "engine: melee: our swing #%u: peers known %u, drawn %u, hittable %u (a hit window opens on each); nearest puppet id=%d is %.1f m from us, and %.1f m from "
                          "where its owner's pose says it is (the owner is really %.1f m from us)%s",
                          in.index, in.peers, in.spawned, in.hittable, in.nearestId, (double)in.nearestM, (double)in.poseErrM, (double)in.poseDistM,
                          in.poseErrM > 1.5f ? " -- THE PUPPET IS DISPLACED: a punch aimed at it lands where the owner is not" : "");
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
