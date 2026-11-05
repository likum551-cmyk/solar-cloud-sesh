// R5 telemetry (on-foot sync): does a puppet MIRROR a two-state flag of its sender (swimming, ragdoll)?
//
// The sender's flag arrives on the wire (proto::kPfSwimming / kPfRagdoll); the engine asks the game what the puppet does
// (IS_PED_SWIMMING / IS_PED_RAGDOLL). This class only counts, it never decides anything:
//   * wire edges      the sender's flag going on / off (what the sender did)
//   * puppet edges    the read-back going on / off (what the game showed)
//   * followed        a wire edge that the puppet's read-back matched within kFollowMs (and the mean / largest delay of the rises)
//   * missed          a wire edge the puppet had NOT matched kFollowMs later (the flag was still the same on the wire)
//   * superseded      a wire edge replaced by the next wire edge before it could be judged (a flicker: neither followed nor missed)
//   * unasked         a puppet RISE while the wire flag had been off for over kFollowMs (the game did it on its own: a puppet the sender
//                     never put in water, or a ragdoll nothing asked for)
//   * unreadable      reads that could not be made (-1)
// Pure: the clock and the read-back come in as arguments. The engine reads only when WantRead() says so (the flag is on, the puppet
// was last seen on, an edge is being judged or a settle window is open; otherwise once per kIdleReadMs, which still catches an
// unasked rise within half a second), so a puppet that is on land and upright costs one native call per half second.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace gage::sync {

class StateMirror
{
public:
    static constexpr double kFollowMs = 1500.0;   // a wire edge not matched this long afterwards is a miss
    static constexpr double kSettleMs = 1000.0;   // reads every frame this long after a wire edge
    static constexpr double kIdleReadMs = 500.0;  // the read rate while nothing is going on

    struct Stats
    {
        uint32_t reads = 0, unreadable = 0;
        uint32_t wireOn = 0, wireOff = 0;
        uint32_t puppetOn = 0, puppetOff = 0;
        uint32_t followedOn = 0, followedOff = 0, missedOn = 0, missedOff = 0, superseded = 0;
        uint32_t unasked = 0;
        double lagOnSumMs = 0, lagOffSumMs = 0;
        double lagOnMaxMs = 0, lagOffMaxMs = 0;   // over the window (TakeWindow resets them)
    };

    // true when this frame should read the puppet's state
    bool WantRead(double nowMs) const
    {
        if (!init_)
            return true;
        if (wire_ || puppet_ == 1 || pending_ != 0 || nowMs < settleUntilMs_)
            return true;
        return nowMs >= nextIdleReadMs_;
    }

    // Every frame, with the wire flag (the pose's flag at the render time) and, when WantRead() was true, the read-back (1 on, 0 off,
    // -1 unreadable); pass puppetRead = -2 on a frame nothing was read.
    void Update(double nowMs, bool wire, int puppetRead)
    {
        if (!init_)
        {
            init_ = true;
            wire_ = wire;
            wireSinceMs_ = nowMs;
            nextIdleReadMs_ = nowMs;
        }
        if (wire != wire_)
        {
            if (pending_ != 0)
                ++cur_.superseded;
            wire_ = wire;
            wireSinceMs_ = nowMs;
            settleUntilMs_ = nowMs + kSettleMs;
            wire ? ++cur_.wireOn : ++cur_.wireOff;
            pending_ = wire ? 1 : -1;
            pendingSinceMs_ = nowMs;
        }
        if (puppetRead == -2)
            return;
        ++cur_.reads;
        nextIdleReadMs_ = nowMs + kIdleReadMs;
        if (puppetRead < 0)
        {
            ++cur_.unreadable;
            return;
        }
        if (puppetRead != puppet_ && puppet_ != -1)
        {
            if (puppetRead == 1)
            {
                ++cur_.puppetOn;
                if (!wire_ && nowMs - wireSinceMs_ > kFollowMs)
                    ++cur_.unasked;
            }
            else
                ++cur_.puppetOff;
        }
        puppet_ = puppetRead;
        if (pending_ != 0)
        {
            const bool wantOn = pending_ == 1;
            const double lag = nowMs - pendingSinceMs_;
            if ((puppet_ == 1) == wantOn)
            {
                if (wantOn)
                {
                    ++cur_.followedOn;
                    cur_.lagOnSumMs += lag;
                    if (lag > cur_.lagOnMaxMs) cur_.lagOnMaxMs = lag;
                }
                else
                {
                    ++cur_.followedOff;
                    cur_.lagOffSumMs += lag;
                    if (lag > cur_.lagOffMaxMs) cur_.lagOffMaxMs = lag;
                }
                pending_ = 0;
            }
            else if (lag > kFollowMs)
            {
                wantOn ? ++cur_.missedOn : ++cur_.missedOff;
                pending_ = 0;
            }
        }
    }

    // forget the state (a new ped, a respawn): the counters stay
    void Rebase() { init_ = false; pending_ = 0; puppet_ = -1; }

    const Stats& Total() const { return cur_; }
    // what happened since the last call; the largest delays are per window
    Stats TakeWindow()
    {
        Stats d;
        d.reads = cur_.reads - last_.reads;
        d.unreadable = cur_.unreadable - last_.unreadable;
        d.wireOn = cur_.wireOn - last_.wireOn;
        d.wireOff = cur_.wireOff - last_.wireOff;
        d.puppetOn = cur_.puppetOn - last_.puppetOn;
        d.puppetOff = cur_.puppetOff - last_.puppetOff;
        d.followedOn = cur_.followedOn - last_.followedOn;
        d.followedOff = cur_.followedOff - last_.followedOff;
        d.missedOn = cur_.missedOn - last_.missedOn;
        d.missedOff = cur_.missedOff - last_.missedOff;
        d.superseded = cur_.superseded - last_.superseded;
        d.unasked = cur_.unasked - last_.unasked;
        d.lagOnSumMs = cur_.lagOnSumMs - last_.lagOnSumMs;
        d.lagOffSumMs = cur_.lagOffSumMs - last_.lagOffSumMs;
        d.lagOnMaxMs = cur_.lagOnMaxMs;
        d.lagOffMaxMs = cur_.lagOffMaxMs;
        last_ = cur_;
        cur_.lagOnMaxMs = cur_.lagOffMaxMs = 0;
        last_.lagOnMaxMs = last_.lagOffMaxMs = 0;
        return d;
    }

private:
    Stats cur_, last_;
    bool init_ = false;
    bool wire_ = false;
    int  puppet_ = -1;              // -1 not read yet
    int  pending_ = 0;              // +1 a wire rise awaits the puppet, -1 a wire fall, 0 none
    double wireSinceMs_ = 0, pendingSinceMs_ = 0, settleUntilMs_ = 0, nextIdleReadMs_ = 0;
};

// "engine: edges id=3 (name): last 10 s swim: sender ON 2 / OFF 2 ..." Empty when the window had no edge and no unasked rise (nothing to say).
inline std::string StateMirrorLine(const char* label, const StateMirror::Stats& w)
{
    if (!w.wireOn && !w.wireOff && !w.puppetOn && !w.puppetOff && !w.unasked)
        return std::string();
    char b[420];
    std::snprintf(b, sizeof(b),
                  "%s: sender on %u / off %u, puppet on %u / off %u (read-back), matched within %.1f s: on %u of %u (avg %.0f max %.0f ms), off %u of %u (avg %.0f max %.0f ms), "
                  "missed on %u off %u, replaced by the next edge %u, puppet on by itself %u, %u reads (%u unreadable)",
                  label, (unsigned)w.wireOn, (unsigned)w.wireOff, (unsigned)w.puppetOn, (unsigned)w.puppetOff, StateMirror::kFollowMs / 1000.0,
                  (unsigned)w.followedOn, (unsigned)w.wireOn, w.followedOn ? w.lagOnSumMs / w.followedOn : 0.0, w.lagOnMaxMs,
                  (unsigned)w.followedOff, (unsigned)w.wireOff, w.followedOff ? w.lagOffSumMs / w.followedOff : 0.0, w.lagOffMaxMs,
                  (unsigned)w.missedOn, (unsigned)w.missedOff, (unsigned)w.superseded, (unsigned)w.unasked, (unsigned)w.reads, (unsigned)w.unreadable);
    return b;
}

}  // namespace gage::sync
