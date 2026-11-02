// Emote arbitration (see emote.h).

#include "emote.h"

namespace gage::sync {

EmoteArbiter::EmoteArbiter(const EmoteConfig& cfg) : cfg_(cfg) {}

void EmoteArbiter::StartAnim(double nowMs, int32_t durationMs, uint32_t flags)
{
    kind_ = EmoteKind::Anim;
    phase_ = Phase::WaitDict;
    durationMs_ = durationMs;
    flags_ = flags;
    sinceMs_ = nowMs;
    notPlayingSinceMs_ = -1;
    stopReq_ = false;
    ++stats_.started;
}

void EmoteArbiter::StartScenario(double nowMs)
{
    kind_ = EmoteKind::Scenario;
    phase_ = Phase::Issue;
    durationMs_ = -1;
    flags_ = 0;
    sinceMs_ = nowMs;
    notPlayingSinceMs_ = -1;
    stopReq_ = false;
    ++stats_.started;
}

void EmoteArbiter::Stop()
{
    if (kind_ != EmoteKind::None)
        stopReq_ = true;
}

void EmoteArbiter::OnNewPed(double nowMs)
{
    issued_ = false;
    if (kind_ == EmoteKind::None)
        return;
    if (!Looping())
    {
        Clear();
        return;
    }
    phase_ = kind_ == EmoteKind::Anim ? Phase::WaitDict : Phase::Issue;
    sinceMs_ = nowMs;
    notPlayingSinceMs_ = -1;
}

void EmoteArbiter::Clear()
{
    kind_ = EmoteKind::None;
    phase_ = Phase::Idle;
    stopReq_ = false;
    issued_ = false;
    notPlayingSinceMs_ = -1;
}

void EmoteArbiter::End(EmoteOut& out)
{
    out.ended = true;
    Clear();
}

EmoteOut EmoteArbiter::Tick(const EmoteIn& in)
{
    EmoteOut out;
    if (kind_ == EmoteKind::None)
        return out;
    if (stopReq_)
    {
        out.clearTasks = issued_;
        ++stats_.stopped;
        End(out);
        return out;
    }
    if (in.dead || in.inVehicle)
    {
        ++stats_.cancelled;
        End(out);
        return out;
    }
    switch (phase_)
    {
    case Phase::Idle:
        break;
    case Phase::WaitDict:
        if (in.dictLoaded)
        {
            out.issueAnim = true;
            phase_ = Phase::Playing;
            issuedAtMs_ = in.nowMs;
            issued_ = true;
            ++stats_.issued;
        }
        else if (in.nowMs - sinceMs_ >= cfg_.dictTimeoutMs)
        {
            out.gaveUp = true;
            ++stats_.dictTimeouts;
            End(out);
            return out;
        }
        else
            out.requestDict = true;
        break;
    case Phase::Issue:
        out.issueScenario = true;
        phase_ = Phase::Playing;
        issuedAtMs_ = in.nowMs;
        issued_ = true;
        ++stats_.issued;
        break;
    case Phase::Playing:
    {
        const double el = in.nowMs - issuedAtMs_;
        if (kind_ == EmoteKind::Anim && durationMs_ > 0 && el >= (double)durationMs_)
        {
            ++stats_.finished;
            End(out);
            return out;
        }
        const double grace = kind_ == EmoteKind::Scenario ? cfg_.scenarioGraceMs : cfg_.startGraceMs;
        if (el >= grace && in.playing == 0)
        {
            if (notPlayingSinceMs_ < 0)
                notPlayingSinceMs_ = in.nowMs;
            else if (in.nowMs - notPlayingSinceMs_ >= cfg_.notPlayingEndMs)
            {
                ++stats_.notPlaying;
                End(out);
                return out;
            }
        }
        else
            notPlayingSinceMs_ = -1;
        if (kind_ == EmoteKind::Anim && durationMs_ == 0 && in.playing < 0 && el >= cfg_.oneShotCapMs)
        {
            ++stats_.finished;
            End(out);
            return out;
        }
        break;
    }
    }
    out.locoPaused = !(kind_ == EmoteKind::Anim && KeepsLoco(flags_));
    out.place = out.locoPaused && in.driftM > cfg_.placeDriftM;
    return out;
}

// ---- animation dictionary lifetime --------------------------------------------------------------

namespace {
    std::string LowerKey(const std::string& s)
    {
        std::string k = s;
        for (char& c : k)
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        return k;
    }
}

void AnimDictRefs::Acquire(const std::string& dict)
{
    if (dict.empty())
        return;
    ++n_[LowerKey(dict)];
}

bool AnimDictRefs::Release(const std::string& dict)
{
    auto it = n_.find(LowerKey(dict));
    if (it == n_.end())
        return false;
    if (--it->second > 0)
        return false;
    n_.erase(it);
    return true;
}

int AnimDictRefs::Count(const std::string& dict) const
{
    auto it = n_.find(LowerKey(dict));
    return it == n_.end() ? 0 : it->second;
}

std::string EmoteDictHold::Sync(AnimDictRefs& refs, const std::string& want)
{
    if (LowerKey(want) == LowerKey(held))
        return std::string();
    std::string remove;
    if (!held.empty() && refs.Release(held))
        remove = held;
    held = want;
    if (!held.empty())
        refs.Acquire(held);
    return remove;
}

}
