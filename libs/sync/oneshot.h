// One-shot / state animations of a remote on-foot puppet: did the game SHOW what we asked for?  (T2, desync round 2.)
//
// There is no per-ped animation id on the wire: a puppet's animations are synthesised from the sender's PedState edges (a jump
// step, a reload flag, stealth, the water, a ragdoll) by asking the game for a task / state. GI reported "animations missing /
// not in place on other players' screens" and nothing in the log says WHICH of them the game really plays on a puppet. This is
// the bookkeeping that does: the engine ARMS a kind when it asks for it, and every frame the kind is armed it passes the game's
// own readback (IS_PED_JUMPING, IS_PED_RELOADING, GET_PED_STEALTH_MOVEMENT, IS_PED_SWIMMING, IS_PED_RAGDOLL) to Observe(). A
// readback that matches inside the kind's window is SHOWN; one that never does is MISSED (the task was dropped, a bad
// precondition); a readback that cannot be taken is UNKNOWN (never counted as missed).
//
//   audit.Arm(OneShot::Jump, now);                       // TASK_JUMP was issued
//   ... every frame: if (audit.Armed(OneShot::Jump)) ev = audit.Observe(OneShot::Jump, IsPedJumping ? 1 : 0, now);
//
// Pure C++17, single-threaded (the script tick), one instance per remote ped.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gage::sync {

    enum class OneShot : uint8_t { Jump, Reload, Stealth, Swim, Ragdoll, kCount };
    enum class OneShotEvent : uint8_t { None, Shown, Missed, Unknown };

    inline const char* OneShotName(OneShot k)
    {
        switch (k)
        {
        case OneShot::Jump:    return "jump";
        case OneShot::Reload:  return "reload";
        case OneShot::Stealth: return "stealth";
        case OneShot::Swim:    return "swim";
        case OneShot::Ragdoll: return "ragdoll";
        case OneShot::kCount:  break;
        }
        return "?";
    }

    // How long after the request the game may take to show it (blend-in, the sender's stream delay is already past: these are
    // local reaction times): a jump is airborne within ~0.3 s, a reload starts at once, stealth is a flag, a ped placed in water
    // starts swimming within a second or two, a ragdoll is immediate.
    inline double OneShotWindowMs(OneShot k)
    {
        switch (k)
        {
        case OneShot::Jump:    return 700.0;
        case OneShot::Reload:  return 900.0;
        case OneShot::Stealth: return 800.0;
        case OneShot::Swim:    return 2500.0;
        case OneShot::Ragdoll: return 500.0;
        case OneShot::kCount:  break;
        }
        return 1000.0;
    }

    struct OneShotStats
    {
        uint32_t issued = 0, shown = 0, missed = 0, unknown = 0;
        double showMsSum = 0, showMsMax = 0;   // request -> first matching readback
    };

    class OneShotAudit
    {
    public:
        // The request was made now. `expect` is the readback value that means "shown" (true for a jump or a reload; stealth: the
        // state asked for, which may be false). A kind already armed is replaced (its request counts as issued; the earlier one is
        // judged as missed if it never showed).
        // Returns the verdict on the request it replaced (None when nothing was armed).
        OneShotEvent Arm(OneShot k, double nowMs, bool expect = true)
        {
            Slot& s = slot_[(size_t)k];
            OneShotEvent replaced = OneShotEvent::None;
            if (s.armed)
                replaced = Judge(k, s);
            s.armed = true;
            s.expect = expect;
            s.atMs = nowMs;
            s.readable = false;
            ++stats_[(size_t)k].issued;
            return replaced;
        }
        bool Armed(OneShot k) const { return slot_[(size_t)k].armed; }
        // playing: 1 / 0 = the readback, -1 = could not be read. Returns what happened this call.
        OneShotEvent Observe(OneShot k, int playing, double nowMs)
        {
            Slot& s = slot_[(size_t)k];
            if (!s.armed)
                return OneShotEvent::None;
            if (playing >= 0)
                s.readable = true;
            if (playing >= 0 && (playing != 0) == s.expect)
            {
                const double ms = nowMs - s.atMs;
                lastShowMs_ = ms;
                OneShotStats& st = stats_[(size_t)k];
                ++st.shown;
                st.showMsSum += ms;
                if (ms > st.showMsMax) st.showMsMax = ms;
                s.armed = false;
                return OneShotEvent::Shown;
            }
            if (nowMs - s.atMs >= OneShotWindowMs(k))
                return Judge(k, s);
            return OneShotEvent::None;
        }
        // The ped was replaced / the kind no longer applies: forget the request without judging it.
        void Cancel(OneShot k) { slot_[(size_t)k].armed = false; }
        void CancelAll() { for (Slot& s : slot_) s.armed = false; }
        const OneShotStats& Stats(OneShot k) const { return stats_[(size_t)k]; }
        double LastShowMs() const { return lastShowMs_; }   // request -> readback of the last Shown event

    private:
        struct Slot { bool armed = false; bool expect = true; bool readable = false; double atMs = 0; };
        // The window ran out (or the request was replaced) without a matching readback: missed, or unknown when no readback
        // could be taken at all.
        OneShotEvent Judge(OneShot k, Slot& s)
        {
            OneShotStats& st = stats_[(size_t)k];
            s.armed = false;
            if (!s.readable)
            {
                ++st.unknown;
                return OneShotEvent::Unknown;
            }
            ++st.missed;
            return OneShotEvent::Missed;
        }
        Slot slot_[(size_t)OneShot::kCount];
        OneShotStats stats_[(size_t)OneShot::kCount];
        double lastShowMs_ = 0;
    };

}
