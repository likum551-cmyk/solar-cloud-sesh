// NetClock: the one render clock of a connection (see netclock.h for the design and its constants).

#include "netclock.h"

#include <algorithm>
#include <cmath>

namespace gage::sync {

namespace {

    constexpr double kPercentile = 0.95;

    double PositiveOr(double v, double fallback) { return (std::isfinite(v) && v > 0.0) ? v : fallback; }
    double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

    NetClockConfig Sanitize(NetClockConfig c)
    {
        c.tickHz = (c.tickHz >= 1.0 && c.tickHz <= 1000.0) ? c.tickHz : 20.0; // NaN fails the test
        c.minDelayMs = PositiveOr(c.minDelayMs, 75.0);
        c.maxDelayMs = std::max(PositiveOr(c.maxDelayMs, 300.0), c.minDelayMs);
        c.initialDelayMs = Clamp(PositiveOr(c.initialDelayMs, 100.0), c.minDelayMs, c.maxDelayMs);
        c.safetyMs = std::isfinite(c.safetyMs) && c.safetyMs >= 0.0 ? c.safetyMs : 25.0;
        c.baselineWindowMs = PositiveOr(c.baselineWindowMs, 5000.0);
        c.statWindowMs = PositiveOr(c.statWindowMs, 3000.0);
        c.fallTauMs = PositiveOr(c.fallTauMs, 2500.0);
        c.slewTauMs = PositiveOr(c.slewTauMs, 500.0);
        c.maxSlew = Clamp(std::isfinite(c.maxSlew) ? c.maxSlew : 0.10, 0.0, 0.5);
        c.resyncMs = PositiveOr(c.resyncMs, 500.0);
        c.maxFrameDistanceMs = PositiveOr(c.maxFrameDistanceMs, 10000.0);
        return c;
    }

    // The 95th percentile (index floor(0.95 * (n - 1)) of the sorted values); 0 for none. Reorders `v`.
    double P95(std::vector<double>& v)
    {
        if (v.empty()) return 0.0;
        const size_t idx = static_cast<size_t>(kPercentile * static_cast<double>(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
        return v[idx];
    }

} // namespace

NetClockConfig NetClockConfig::ForTickHz(double hz)
{
    NetClockConfig c;
    if (!(hz >= 1.0 && hz <= 1000.0)) hz = 20.0;
    const double tick = 1000.0 / hz;
    c.tickHz = hz;
    c.initialDelayMs = 2.0 * tick;
    c.minDelayMs = 1.5 * tick;
    c.maxDelayMs = std::max(300.0, 6.0 * tick);
    c.safetyMs = 0.5 * tick;
    return c;
}

NetClock::NetClock(const NetClockConfig& cfg)
    : cfg_(Sanitize(cfg)), tickMs_(1000.0 / cfg_.tickHz), targetMs_(cfg_.initialDelayMs), delayMs_(cfg_.initialDelayMs)
{
    scratch_.reserve(kMaxExcessSamples);
}

void NetClock::Reset()
{
    *this = NetClock(cfg_);
}

int64_t NetClock::Unwrap(uint32_t tick) const
{
    if (!started_) return 0;
    return newestU_ + static_cast<int32_t>(tick - newestTick_);
}

bool NetClock::OnFrame(uint32_t tick, double recvMs)
{
    if (!std::isfinite(recvMs))
    {
        ++stats_.ignored;
        return false;
    }
    if (haveLast_ && tick == lastTick_ && recvMs == lastRecvMs_)
        return true; // the same frame again (one call per entry of it)

    int64_t u = 0;
    if (!started_)
    {
        started_ = true;
        originTick_ = tick;
        newestTick_ = tick;
        newestU_ = 0;
    }
    else
    {
        const int32_t delta = static_cast<int32_t>(tick - newestTick_);
        if (std::fabs(static_cast<double>(delta)) * tickMs_ > cfg_.maxFrameDistanceMs)
        {
            ++stats_.ignored;
            return false;
        }
        u = newestU_ + delta;
        if (delta > 0)
        {
            newestU_ = u;
            newestTick_ = tick;
        }
    }
    lastTick_ = tick;
    lastRecvMs_ = recvMs;
    haveLast_ = true;
    ++stats_.frames;
    if (haveFrameRecv_ && recvMs >= lastFrameRecvMs_) frameGaps_.Add(recvMs - lastFrameRecvMs_);
    lastFrameRecvMs_ = recvMs;
    haveFrameRecv_ = true;
    if (u < static_cast<int64_t>(newestU_)) ++stats_.reorderedFrames;

    const double offset = recvMs - static_cast<double>(u) * tickMs_;
    while (!baseline_.empty() && baseline_.back().v >= offset) baseline_.pop_back();
    baseline_.push_back({recvMs, offset});
    while (baseline_.size() > 1 && baseline_.front().t + cfg_.baselineWindowMs < recvMs) baseline_.pop_front();

    {
        const double base = baseline_.front().v;
        if (haveBase_ && std::fabs(base - lastBase_) > 5.0)
        {
            ++stats_.baselineSteps;
            stepMaxMs_ = std::max(stepMaxMs_, std::fabs(base - lastBase_));
        }
        lastBase_ = base;
        haveBase_ = true;
    }
    excess_.push_back({recvMs, offset - baseline_.front().v});
    while (!excess_.empty() && (excess_.size() > kMaxExcessSamples || excess_.front().t + cfg_.statWindowMs < recvMs))
        excess_.pop_front();

    Retarget(recvMs);
    return true;
}

void NetClock::NoteAge(double ageMs)
{
    if (!std::isfinite(ageMs) || ageMs < 0.0 || ageMs >= 255.0) return;
    ages_.push_back(ageMs);
    if (ages_.size() > kMaxAgeSamples) ages_.pop_front();
}

void NetClock::Retarget(double nowMs)
{
    scratch_.clear();
    for (const Sample& s : excess_) scratch_.push_back(s.v);
    excessP95_ = P95(scratch_);
    scratch_.clear();
    for (double a : ages_) scratch_.push_back(a);
    ageP95_ = P95(scratch_);

    targetMs_ = Clamp(tickMs_ + ageP95_ + excessP95_ + cfg_.safetyMs, cfg_.minDelayMs, cfg_.maxDelayMs);
    const double dt = nowMs > lastRetargetMs_ ? nowMs - lastRetargetMs_ : 0.0;
    lastRetargetMs_ = std::max(lastRetargetMs_, nowMs);
    if (stats_.frames < kMinFrames)
        return; // the first frames only measure: the delay stays at its initial value
    if (targetMs_ > delayMs_)
    {
        delayMs_ = targetMs_; // rises at once
        ++stats_.delayRises;
    }
    else
        delayMs_ += (targetMs_ - delayMs_) * (1.0 - std::exp(-dt / cfg_.fallTauMs)); // falls with wall time
}

double NetClock::RenderT(double nowMs)
{
    if (!started_ || !std::isfinite(nowMs)) return renderT_;
    const double target = nowMs - BaselineMs() - delayMs_; // where the render clock wants to be
    if (!haveRender_)
    {
        haveRender_ = true;
        renderT_ = target;
        lastNowMs_ = nowMs;
        return renderT_;
    }
    if (!(nowMs > lastNowMs_))
        return renderT_; // the same frame (or a clock that stood still / stepped back): never rewinds, asks once
    const double dt = nowMs - lastNowMs_;
    lastNowMs_ = nowMs;
    renderT_ += dt;
    const double err = target - renderT_;
    if (std::fabs(err) > cfg_.resyncMs)
    {
        renderT_ = target;
        ++stats_.resyncs;
    }
    else
    {
        double corr = Clamp(err / cfg_.slewTauMs, -cfg_.maxSlew, cfg_.maxSlew) * dt;
        if (std::fabs(corr) > std::fabs(err)) corr = err;
        renderT_ += corr;
    }
    return renderT_;
}

}
