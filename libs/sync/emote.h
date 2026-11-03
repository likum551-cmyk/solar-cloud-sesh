// Emote arbitration for one ped (protocol v4 PlayAnim / PlayScenario / StopAnim): when to stream
// the animation dictionary, when to issue TASK_PLAY_ANIM / TASK_START_SCENARIO_IN_PLACE, when the
// emote is over, and whether the locomotion controller (locomotion.h, which re-tasks the ped with
// gait tasks) may run meanwhile. Pure C++17, single-threaded (the engine's script tick), tested by
// tests/gameplay_test.cpp. One arbiter per remote ped, and one for the local player.
//
//   arbiter.StartAnim(now, durationMs, flags) / StartScenario(now) / Stop()   on the events
//   every tick:   EmoteIn in; in.dictLoaded = (only while WaitingForDict()) HAS_ANIM_DICT_LOADED;
//                 in.playing = (only while Issued()) IS_ENTITY_PLAYING_ANIM / IS_PED_USING_ANY_SCENARIO;
//                 const EmoteOut o = arbiter.Tick(in);
//                 o.requestDict -> REQUEST_ANIM_DICT;  o.issueAnim -> TASK_PLAY_ANIM(GtaDuration, GtaFlags)
//                 o.issueScenario -> TASK_START_SCENARIO_IN_PLACE;  o.clearTasks -> CLEAR_PED_TASKS
//                 o.ended -> the locomotion controller takes the ped back (Reset)
//                 o.locoPaused -> issue no gait / aim task this tick;  o.place -> set the ped onto its pose
//
// Rules:
//  * A dictionary that has not loaded after dictTimeoutMs is given up (gaveUp + ended).
//  * A timed animation (durationMs > 0) ends after its duration. A one-shot (0) and a loop (-1) or a
//    scenario end when the ped is seen NOT playing it for notPlayingEndMs after a start grace (blend
//    in, a scenario's enter animation): a bad clip name, a task the game dropped. A one-shot whose
//    playing state cannot be read ends at oneShotCapMs.
//  * Death or a vehicle seat ends the emote without a clear (the game already replaced the task).
//  * The gait is paused while an emote runs unless its flags keep the legs free (16 upper body or
//    32 secondary task, GTA's AF_UPPERBODY / AF_SECONDARY). A paused ped still follows its pose by
//    placement when it drifts beyond placeDriftM (the sender walked away mid-emote).
//  * OnNewPed (the ped was recreated): a loop or scenario is issued again, a one-shot is dropped.
//  * 0e (animation audit): an emote that was never SEEN playing when it ends for "not playing" is a bad clip name
//    (neverPlayed: the engine tells the player and logs it); a LOOP that was seen playing and then stops (a knockdown, a
//    task the game replaced) is issued again up to cfg.reissueMax times (peers only: our own player leaving the emote is
//    the player's choice); an issue clears the ped's tasks first, except for an upper-body / secondary anim on a ped that
//    had no emote before (its gait task keeps running: clearFirst false), and when the gait was cleared while the
//    controller keeps running (resetLoco) the engine hands the ped back to it; a restart whose dictionary never loads
//    clears the task the previous emote left on the ped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace gage::sync {

    enum class EmoteKind : uint8_t { None, Anim, Scenario };

    // Why an emote ended (EmoteOut::endReason), for the engine's per-10-s animation line.
    enum class EmoteEnd : uint8_t { None, Duration, Stop, Dead, Vehicle, NotPlaying, DictTimeout, OneShotCap };

    struct EmoteConfig
    {
        double dictTimeoutMs = 3000.0;
        double startGraceMs = 600.0;      // an anim's blend-in
        double scenarioGraceMs = 3000.0;  // a scenario's enter animation
        double notPlayingEndMs = 400.0;
        double oneShotCapMs = 30000.0;
        float  placeDriftM = 2.5f;
        int    reissueMax = 0;            // a seen-playing loop that stops is issued again this many times (0 = ends)
    };
    // The config of a remote puppet's arbiter: a loop the game cancelled under it (a knockdown) comes back.
    inline EmoteConfig PeerEmoteConfig()
    {
        EmoteConfig c;
        c.reissueMax = 2;
        return c;
    }

    struct EmoteIn
    {
        double nowMs = 0;
        bool   dictLoaded = false;  // read only while WaitingForDict()
        int    playing = -1;        // 1 playing, 0 not, -1 unknown (read only while Issued())
        bool   dead = false;
        bool   inVehicle = false;
        float  driftM = 0;          // distance between the ped and its pose (0 for the local player)
    };

    struct EmoteOut
    {
        bool requestDict = false;
        bool issueAnim = false;
        bool issueScenario = false;
        bool clearTasks = false;
        bool ended = false;
        bool gaveUp = false;
        bool locoPaused = false;
        bool place = false;
        // 0e
        bool clearFirst = false;      // with issueAnim / issueScenario: CLEAR_PED_TASKS_IMMEDIATELY before the task
        bool replacedTask = false;    // with issueAnim: an earlier emote's task may still be on the ped (a restart or a re-issue)
        bool resetLoco = false;       // the gait task was cleared but the controller keeps running: Reset it
        bool confirmed = false;       // this tick the ped was first SEEN playing it (confirmMs after the issue)
        double confirmMs = 0;
        double waitedMs = 0;          // with issueAnim: how long the dictionary took (start or re-issue to loaded)
        bool neverPlayed = false;     // ended as "not playing" without ever having been seen playing: a bad clip name
        bool reissued = false;        // a seen-playing loop stopped: it is issued again (requestDict / issue follow)
        EmoteEnd endReason = EmoteEnd::None;   // with ended
    };

    struct EmoteStats
    {
        uint32_t started = 0, issued = 0, finished = 0, stopped = 0, notPlaying = 0, dictTimeouts = 0, cancelled = 0;
        // 0e: seen playing, ended without ever playing, loops issued again, starts that replaced a running emote,
        // and the time from the issue to the first sighting (sum / max, ms)
        uint32_t confirmed = 0, neverPlayed = 0, reissued = 0, restarted = 0;
        double confirmMsSum = 0, confirmMsMax = 0;
    };

    class EmoteArbiter
    {
    public:
        explicit EmoteArbiter(const EmoteConfig& cfg = EmoteConfig());

        // durationMs and flags as on the wire (proto::Anim): -1 loops until stopped.
        // Both return true when they replaced an emote that was running (its task may still be on the ped).
        bool StartAnim(double nowMs, int32_t durationMs, uint32_t flags);
        bool StartScenario(double nowMs);
        void Stop();                 // applied on the next Tick
        void OnNewPed(double nowMs);
        void Clear();                // forget everything (the ped is gone), no outputs
        EmoteOut Tick(const EmoteIn& in);

        EmoteKind Kind() const { return kind_; }
        bool Active() const { return kind_ != EmoteKind::None; }
        bool WaitingForDict() const { return kind_ == EmoteKind::Anim && phase_ == Phase::WaitDict; }
        bool Issued() const { return phase_ == Phase::Playing; }
        bool Looping() const { return kind_ == EmoteKind::Scenario || (kind_ == EmoteKind::Anim && durationMs_ < 0); }
        const EmoteStats& Stats() const { return stats_; }

        static bool KeepsLoco(uint32_t flags) { return (flags & (16u | 32u)) != 0; }
        // TASK_PLAY_ANIM's duration and flags for a wire anim: -1 loops (flag 1 added), 0 plays the
        // clip once at its own length (GTA duration -1 without the loop flag), > 0 as given.
        static int32_t GtaDuration(int32_t wireDurationMs) { return wireDurationMs > 0 ? wireDurationMs : -1; }
        static uint32_t GtaFlags(int32_t wireDurationMs, uint32_t flags) { return wireDurationMs < 0 ? (flags | 1u) : flags; }

    private:
        enum class Phase : uint8_t { Idle, WaitDict, Issue, Playing };
        void End(EmoteOut& out, EmoteEnd why);

        EmoteConfig cfg_;
        EmoteKind kind_ = EmoteKind::None;
        Phase phase_ = Phase::Idle;
        int32_t durationMs_ = -1;
        uint32_t flags_ = 0;
        double sinceMs_ = 0, issuedAtMs_ = 0, notPlayingSinceMs_ = -1;
        bool stopReq_ = false, issued_ = false, seen_ = false;
        int reissues_ = 0;
        EmoteStats stats_;
    };

    // ---- animation dictionary lifetime ------------------------------------------------------------
    //
    // REQUEST_ANIM_DICT / REMOVE_ANIM_DICT work per dictionary NAME, while up to 32 remote peds and
    // the local player may each play a clip of the same one. Every owner (one EmoteArbiter) holds one
    // reference on the dictionary of the anim it streams or plays; the game's copy is removed only when
    // the last owner lets go, so a dictionary is neither leaked (every emote ever played staying in
    // memory) nor pulled from under a ped still playing it. Names are case-insensitive.
    class AnimDictRefs
    {
    public:
        void Acquire(const std::string& dict);
        // One reference less; true when it was the last one (the caller calls REMOVE_ANIM_DICT).
        // A name nobody holds is ignored (false): the count never goes negative.
        bool Release(const std::string& dict);
        int Count(const std::string& dict) const;
        size_t Tracked() const { return n_.size(); }
    private:
        std::map<std::string, int> n_;
    };

    // One owner's reference. Call Sync after anything that may have changed what its arbiter plays: a
    // StartAnim, the arbiter ending (a duration, Stop, a death, a seat, a dictionary timeout), a
    // Clear, the ped despawning (want "").
    struct EmoteDictHold
    {
        std::string held;   // the dictionary this owner holds a reference on ("" = none)
        // want: the dictionary of the anim the arbiter streams or plays right now ("" = none: idle, a
        // scenario, ended). Returns the dictionary the caller must REMOVE now ("" = none).
        std::string Sync(AnimDictRefs& refs, const std::string& want);
    };

}
