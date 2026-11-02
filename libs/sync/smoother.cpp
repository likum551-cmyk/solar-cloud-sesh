// Remote-player smoothing (see smoother.h for the design and its constants).

#include "smoother.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gage::sync {

namespace {

    constexpr size_t kMinClockSamples   = 16;   // packets measured before the delay adapts
    constexpr double kJitterPercentile  = 0.95;
    constexpr double kDelayFallPerPacket = 0.02; // share of the gap the delay falls per packet
    constexpr double kMaxTangentRatio   = 3.0;   // Hermite tangent <= 3 x chord
    constexpr double kResidualEpsilon   = 1e-4;  // metres / degrees treated as zero

    // Degrees wrapped into (-180, 180].
    double WrapDeg(double d)
    {
        d = std::fmod(d, 360.0);
        if (d > 180.0) d -= 360.0;
        else if (d <= -180.0) d += 360.0;
        return d;
    }

    bool Finite(const proto::Kinematics& k)
    {
        return std::isfinite(k.x) && std::isfinite(k.y) && std::isfinite(k.z) &&
               std::isfinite(k.heading) &&
               std::isfinite(k.vx) && std::isfinite(k.vy) && std::isfinite(k.vz);
    }

    bool Same(const proto::Kinematics& a, const proto::Kinematics& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.heading == b.heading &&
               a.vx == b.vx && a.vy == b.vy && a.vz == b.vz;
    }

    bool SamePed(const proto::PedState& a, const proto::PedState& b)
    {
        // shotSeq too: a standing shooter's only change is the shot counter, and a Repeat would hold
        // the receiver's shots back until the shooter moved.
        return a.move == b.move && a.flags == b.flags && a.jumpSeq == b.jumpSeq && a.weapon == b.weapon &&
               a.shotSeq == b.shotSeq && a.aimX == b.aimX && a.aimY == b.aimY && a.aimZ == b.aimZ;
    }

    double Length(double x, double y, double z) { return std::sqrt(x * x + y * y + z * z); }

    void ClampLength(double& x, double& y, double& z, double maxLen)
    {
        const double len = Length(x, y, z);
        if (len > maxLen && len > 0.0)
        {
            const double s = maxLen / len;
            x *= s; y *= s; z *= s;
        }
    }

    double PositiveOr(double v, double fallback) { return (std::isfinite(v) && v > 0.0) ? v : fallback; }
    double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

    SmootherConfig Sanitize(SmootherConfig c)
    {
        c.tickHz = (c.tickHz >= 1.0 && c.tickHz <= 1000.0) ? c.tickHz : 20.0; // NaN fails the test
        c.minDelayMs = PositiveOr(c.minDelayMs, 75.0);
        c.maxDelayMs = std::max(PositiveOr(c.maxDelayMs, 300.0), c.minDelayMs);
        c.initialDelayMs = Clamp(PositiveOr(c.initialDelayMs, 100.0), c.minDelayMs, c.maxDelayMs);
        c.delaySafetyMs = std::isfinite(c.delaySafetyMs) && c.delaySafetyMs >= 0.0 ? c.delaySafetyMs : 25.0;
        c.extrapolateMaxMs = std::isfinite(c.extrapolateMaxMs) && c.extrapolateMaxMs >= 0.0 ? c.extrapolateMaxMs : 250.0;
        c.maxBridgeMs = PositiveOr(c.maxBridgeMs, 500.0);
        c.snapDistance = (std::isfinite(c.snapDistance) && c.snapDistance > 0.0f) ? c.snapDistance : 8.0f;
        c.forgetAfterMs = PositiveOr(c.forgetAfterMs, 5000.0);
        c.residualTauMs = PositiveOr(c.residualTauMs, 100.0);
        c.slewTauMs = PositiveOr(c.slewTauMs, 500.0);
        c.maxSlew = Clamp(std::isfinite(c.maxSlew) ? c.maxSlew : 0.10, 0.0, 0.5);
        c.resyncMs = PositiveOr(c.resyncMs, 500.0);
        return c;
    }

} // namespace

SmootherConfig SmootherConfig::ForTickHz(double hz)
{
    SmootherConfig c;
    if (!(hz >= 1.0 && hz <= 1000.0)) hz = 20.0;
    const double tick = 1000.0 / hz;
    c.tickHz = hz;
    c.initialDelayMs = 2.0 * tick;
    c.minDelayMs = 1.5 * tick;
    c.maxDelayMs = std::max(300.0, 6.0 * tick);
    c.delaySafetyMs = 0.5 * tick;
    c.stillExtrapolateMaxM = 0.3f; // W8 item 8: a standing player is not carried past where it stopped
    return c;
}

// ---- PlayerSmoother ---------------------------------------------------------

PlayerSmoother::PlayerSmoother(const SmootherConfig& cfg)
    : cfg_(Sanitize(cfg)), tickMs_(1000.0 / cfg_.tickHz), delayMs_(cfg_.initialDelayMs)
{
    for (double& w : window_) w = 0.0;
    for (double& l : late_) l = 0.0;
    for (double& g : gaps_) g = 0.0;
}

void PlayerSmoother::Append(const Snapshot& s)
{
    if (count_ == kSnapshotRing)
    {
        pedBefore_ = ring_[head_].ped; // the evicted oldest is at or before anything still Buffering
        havePedBefore_ = true;
        head_ = (head_ + 1) % kSnapshotRing;
        --count_;
    }
    ring_[(head_ + count_) % kSnapshotRing] = s;
    ++count_;
}

// a: arrival offset on the packet tick (baseline); late: a plus the age of the
// data the packet brought (a for a repeat), whose excess drives the delay.
void PlayerSmoother::UpdateClock(double a, double late)
{
    if (clock_) return; // the shared clock measures the link (smoother.h item 12)
    window_[wHead_] = a;
    late_[wHead_] = late;
    wHead_ = (wHead_ + 1) % kClockWindow;
    if (wCount_ < kClockWindow) ++wCount_;

    // Valid entries are indices [0, wCount_): wHead_ equals wCount_ until the
    // window first fills, then every slot is valid.
    double base = window_[0];
    for (size_t i = 1; i < wCount_; ++i) base = std::min(base, window_[i]);
    baseMs_ = base;
    if (wCount_ < kMinClockSamples) return;

    double excess[kClockWindow];
    for (size_t i = 0; i < wCount_; ++i) excess[i] = late_[i] - base; // late_ >= window_ >= base
    const size_t idx = static_cast<size_t>(kJitterPercentile * static_cast<double>(wCount_ - 1));
    std::nth_element(excess, excess + idx, excess + wCount_);
    jitterMs_ = excess[idx];

    const double target = Clamp(TypicalGapMs() + jitterMs_ + cfg_.delaySafetyMs, cfg_.minDelayMs, cfg_.maxDelayMs);
    if (target > delayMs_) delayMs_ = target;
    else delayMs_ += (target - delayMs_) * kDelayFallPerPacket;
}

// Median spacing of the last stored snapshots, never below one tick.
double PlayerSmoother::TypicalGapMs() const
{
    if (gCount_ == 0) return tickMs_;
    double g[kGapWindow];
    for (size_t i = 0; i < gCount_; ++i) g[i] = gaps_[i];
    std::nth_element(g, g + gCount_ / 2, g + gCount_);
    return std::max(tickMs_, g[gCount_ / 2]);
}

PlayerSmoother::Val PlayerSmoother::Evaluate(double t, PoseMode* mode) const
{
    const Snapshot& first = At(0);
    const Snapshot& last = At(count_ - 1);
    Val o;

    if (t <= first.t)
    {
        *mode = PoseMode::Buffering;
        o.x = first.k.x; o.y = first.k.y; o.z = first.k.z; o.h = first.k.heading;
        o.ped = havePedBefore_ ? pedBefore_ : first.ped; // nothing stored is at or before t

        return o;
    }

    if (t > last.t)
    {
        const double dt = t - last.t;
        const double use = std::min(dt, cfg_.extrapolateMaxMs) / 1000.0;
        double ex = last.k.vx * use, ey = last.k.vy * use;
        // A snapshot that says the player stands (a default or stale PedState still carrying a running velocity)
        // moves the pose by at most stillExtrapolateMaxM: it must not run on 1.5 m and snap back (W8 item 8).
        if (cfg_.stillExtrapolateMaxM > 0.0f && StandingStill(last.ped))
        {
            const double d = std::sqrt(ex * ex + ey * ey);
            if (d > cfg_.stillExtrapolateMaxM && d > 0.0)
            {
                ex *= cfg_.stillExtrapolateMaxM / d;
                ey *= cfg_.stillExtrapolateMaxM / d;
            }
        }
        o.x = last.k.x + ex;
        o.y = last.k.y + ey;
        o.z = last.k.z + last.k.vz * use;
        o.h = last.k.heading;
        o.ped = last.ped;
        if (dt <= cfg_.extrapolateMaxMs)
        {
            *mode = PoseMode::Extrapolating;
            o.vx = last.k.vx; o.vy = last.k.vy; o.vz = last.k.vz;
        }
        else
        {
            *mode = PoseMode::Holding; // at the capped position, standing
        }
        return o;
    }

    // first.t < t <= last.t: b is the first snapshot at or after t, a the one before.
    size_t j = count_ - 1;
    while (j > 1 && At(j - 1).t >= t) --j;
    const Snapshot& a = At(j - 1);
    const Snapshot& b = At(j);
    const double span = (b.t - a.t) / 1000.0; // > 0: stored placed times are strictly increasing
    const double s = (t - a.t) / (b.t - a.t);

    const double cx = double(b.k.x) - a.k.x, cy = double(b.k.y) - a.k.y, cz = double(b.k.z) - a.k.z;
    if (cfg_.hermite)
    {
        const double chord = Length(cx, cy, cz);
        double m0x = a.k.vx * span, m0y = a.k.vy * span, m0z = a.k.vz * span;
        double m1x = b.k.vx * span, m1y = b.k.vy * span, m1z = b.k.vz * span;
        ClampLength(m0x, m0y, m0z, kMaxTangentRatio * chord);
        ClampLength(m1x, m1y, m1z, kMaxTangentRatio * chord);
        const double s2 = s * s, s3 = s2 * s;
        const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s;
        const double h01 = -2 * s3 + 3 * s2,    h11 = s3 - s2;
        const double d00 = 6 * s2 - 6 * s,      d10 = 3 * s2 - 4 * s + 1;
        const double d01 = -6 * s2 + 6 * s,     d11 = 3 * s2 - 2 * s;
        o.x = h00 * a.k.x + h10 * m0x + h01 * b.k.x + h11 * m1x;
        o.y = h00 * a.k.y + h10 * m0y + h01 * b.k.y + h11 * m1y;
        o.z = h00 * a.k.z + h10 * m0z + h01 * b.k.z + h11 * m1z;
        o.vx = (d00 * a.k.x + d10 * m0x + d01 * b.k.x + d11 * m1x) / span;
        o.vy = (d00 * a.k.y + d10 * m0y + d01 * b.k.y + d11 * m1y) / span;
        o.vz = (d00 * a.k.z + d10 * m0z + d01 * b.k.z + d11 * m1z) / span;
    }
    else
    {
        o.x = a.k.x + cx * s; o.y = a.k.y + cy * s; o.z = a.k.z + cz * s;
        o.vx = cx / span; o.vy = cy / span; o.vz = cz / span;
    }
    o.h = WrapDeg(a.k.heading + WrapDeg(double(b.k.heading) - a.k.heading) * s);
    o.ped = b.t <= t ? b.ped : a.ped; // discrete: the newest snapshot at or before t
    *mode = PoseMode::Interpolating;
    return o;
}

// The curve just moved under the current render time by (before - after). Keep
// the shown pose where it was and let the difference decay instead.
void PlayerSmoother::Absorb(const Val& before, const Val& after)
{
    res_.x += before.x - after.x;
    res_.y += before.y - after.y;
    res_.z += before.z - after.z;
    res_.h = WrapDeg(res_.h + WrapDeg(before.h - after.h));
    if (Length(res_.x, res_.y, res_.z) > cfg_.snapDistance) res_ = Residual(); // would be a slide across the map
}

PushResult PlayerSmoother::Push(uint32_t tick, const proto::Kinematics& in, double nowMs, double ageMs,
                                const proto::PedState& ped)
{
    if (!Finite(in) || !std::isfinite(nowMs) || !std::isfinite(ageMs) || ageMs < 0.0)
    {
        ++stats_.invalid;
        return PushResult::Invalid;
    }

    int64_t utick = 0;
    if (started_)
    {
        if (!proto::SeqNewer(tick, newestTick_))
        {
            ++stats_.stale;
            return PushResult::Stale;
        }
        utick = newestUtick_ + static_cast<int32_t>(tick - newestTick_);
    }
    if (!started_) originTick_ = tick;
    started_ = true;
    newestTick_ = tick;
    newestUtick_ = utick;

    // When the server built the packet, on the shared clock's timeline when there is one (the same instant a
    // vehicle frame of that tick is stamped with), else on this player's own (counted from its first tick).
    const double packetMs = clock_ ? clock_->TimelineMs(tick) : static_cast<double>(utick) * tickMs_;
    const double placedMs = packetMs - ageMs;                      // when the server got the sample
    const double offset = nowMs - packetMs;
    lastRecvMs_ = nowMs;

    proto::Kinematics k = in;
    k.heading = static_cast<float>(WrapDeg(in.heading));

    Snapshot s;
    s.t = placedMs;
    s.recvMs = nowMs;
    s.tick = tick;
    s.k = k;
    s.ped = ped;

    // Every fresh packet measures the link, repeat or not (on the packet tick);
    // only new data adds its age to the lateness the delay has to cover.
    if (count_ == 0)
    {
        UpdateClock(offset, offset + ageMs);
        Append(s);
        ++stats_.accepted;
        return PushResult::Accepted;
    }

    const Snapshot newest = At(count_ - 1); // copy: Append may recycle its slot
    // A ped-only change (a standing player who starts aiming) is new information.
    if ((Same(newest.k, k) && SamePed(newest.ped, ped)) || !(placedMs > newest.t))
    {
        UpdateClock(offset, offset);
        ++stats_.repeats;
        return PushResult::Repeat;
    }
    UpdateClock(offset, offset + ageMs);

    // Where the newest snapshot said this player would be by now.
    const double ahead = std::min(placedMs - newest.t, cfg_.extrapolateMaxMs) / 1000.0;
    const double ex = k.x - (newest.k.x + newest.k.vx * ahead);
    const double ey = k.y - (newest.k.y + newest.k.vy * ahead);
    const double ez = k.z - (newest.k.z + newest.k.vz * ahead);
    if (Length(ex, ey, ez) > cfg_.snapDistance)
    {
        pedBefore_ = newest.ped;
        havePedBefore_ = true;
        count_ = 0;
        head_ = 0;
        res_ = Residual();
        teleportPending_ = true;
        Append(s);
        ++stats_.accepted;
        ++stats_.teleports;
        return PushResult::Teleported;
    }

    Val before, after;
    PoseMode mode = PoseMode::Buffering;
    if (haveClock_) before = Evaluate(renderT_, &mode);
    if (placedMs - newest.t > cfg_.maxBridgeMs)
    {
        count_ = 0; // too long a gap to interpolate across: the old data is history
        head_ = 0;
        pedBefore_ = newest.ped;
        havePedBefore_ = true;
        ++stats_.bridgeResets;
    }
    else
    {
        gaps_[gHead_] = placedMs - newest.t;
        gHead_ = (gHead_ + 1) % kGapWindow;
        if (gCount_ < kGapWindow) ++gCount_;
    }
    Append(s);
    if (haveClock_)
    {
        after = Evaluate(renderT_, &mode);
        Absorb(before, after);
    }
    ++stats_.accepted;
    return PushResult::Accepted;
}

bool PlayerSmoother::Sample(double nowMs, Pose& out)
{
    if (count_ == 0 || !std::isfinite(nowMs) || Expired(nowMs)) return false;

    if (clock_ && !clock_->Ready()) return false; // the shared clock has seen no frame yet

    // Shared clock (item 12): the time to show is the connection's render clock minus this player's class extra
    // (whole ticks its snapshots are spaced beyond one). The clock did the slewing; the extra slews itself.
    double target = 0.0;
    if (clock_)
    {
        // W8 item 2: the class (whole ticks of gap) is held with hysteresis (ClassHold), so a stream whose gaps
        // alternate or hover at a class boundary does not make the render time slew back and forth.
        const double extra = extra_.UpdateGap(TypicalGapMs(), tickMs_, nowMs, cfg_.maxSlew);
        stats_.extraChanges = extra_.ClassChanges();
        target = clock_->RenderT(nowMs) - extra;
        delayMs_ = clock_->DelayMs() + extra;
        jitterMs_ = clock_->LatenessMs();
    }
    else
        target = nowMs - baseMs_ - delayMs_; // where the render clock wants to be
    double dt = 0.0;
    if (!haveClock_)
    {
        haveClock_ = true;
        renderT_ = target;
        lastSampleMs_ = nowMs;
        if (clock_) seenResyncs_ = clock_->Resyncs();
    }
    else if (clock_)
    {
        if (nowMs > lastSampleMs_) // a clock that stands still or steps back never rewinds the pose
        {
            dt = nowMs - lastSampleMs_;
            lastSampleMs_ = nowMs;
        }
        if (clock_->Resyncs() != seenResyncs_)
        {
            // The shared clock jumped (a hard resync): show the new time at once, absorbing the difference.
            seenResyncs_ = clock_->Resyncs();
            PoseMode m = PoseMode::Buffering;
            const Val before = Evaluate(renderT_, &m);
            renderT_ = target;
            Absorb(before, Evaluate(renderT_, &m));
            ++stats_.resyncs;
        }
        else if (target > renderT_)
            renderT_ = target; // monotone: the clock and the extra both move slowly, a smaller target waits
    }
    else
    {
        if (nowMs > lastSampleMs_) // a clock that stands still or steps back never rewinds the pose
        {
            dt = nowMs - lastSampleMs_;
            lastSampleMs_ = nowMs;
        }
        renderT_ += dt;
        const double err = target - renderT_;
        if (std::fabs(err) > cfg_.resyncMs)
        {
            PoseMode m = PoseMode::Buffering;
            const Val before = Evaluate(renderT_, &m);
            renderT_ = target;
            Absorb(before, Evaluate(renderT_, &m));
            ++stats_.resyncs;
        }
        else
        {
            double corr = Clamp(err / cfg_.slewTauMs, -cfg_.maxSlew, cfg_.maxSlew) * dt;
            if (std::fabs(corr) > std::fabs(err)) corr = err;
            renderT_ += corr;
        }
    }

    const double decay = std::exp(-dt / cfg_.residualTauMs);
    res_.x *= decay; res_.y *= decay; res_.z *= decay; res_.h *= decay;
    if (Length(res_.x, res_.y, res_.z) < kResidualEpsilon && std::fabs(res_.h) < kResidualEpsilon)
        res_ = Residual();

    PoseMode mode = PoseMode::Buffering;
    const Val v = Evaluate(renderT_, &mode);
    out.k.x = static_cast<float>(v.x + res_.x);
    out.k.y = static_cast<float>(v.y + res_.y);
    out.k.z = static_cast<float>(v.z + res_.z);
    out.k.heading = static_cast<float>(WrapDeg(v.h + res_.h));
    out.k.vx = static_cast<float>(v.vx);
    out.k.vy = static_cast<float>(v.vy);
    out.k.vz = static_cast<float>(v.vz);
    out.mode = mode;
    out.ped = v.ped;
    out.renderTick = clock_ ? clock_->TickAt(renderT_) : static_cast<double>(originTick_) + renderT_ / tickMs_;
    out.teleported = teleportPending_;
    teleportPending_ = false;
    return true;
}

// ---- RemoteSync -------------------------------------------------------------

namespace {

    // The smoother's clock settings as the connection's NetClock takes them.
    NetClockConfig ClockConfigOf(const SmootherConfig& c)
    {
        NetClockConfig n;
        n.tickHz = c.tickHz;
        n.initialDelayMs = c.initialDelayMs;
        n.minDelayMs = c.minDelayMs;
        n.maxDelayMs = c.maxDelayMs;
        n.safetyMs = c.delaySafetyMs;
        n.slewTauMs = c.slewTauMs;
        n.maxSlew = c.maxSlew;
        n.resyncMs = c.resyncMs;
        return n;
    }

}

RemoteSync::RemoteSync(const SmootherConfig& cfg) : cfg_(cfg), clock_(ClockConfigOf(cfg)) {}

RemoteSync::RemoteSync(const RemoteSync& o)
    : cfg_(o.cfg_), clock_(o.clock_), players_(o.players_), localId_(o.localId_), hasLocalId_(o.hasLocalId_)
{
    Rebind();
}

RemoteSync::RemoteSync(RemoteSync&& o)
    : cfg_(o.cfg_), clock_(std::move(o.clock_)), players_(std::move(o.players_)), localId_(o.localId_),
      hasLocalId_(o.hasLocalId_)
{
    Rebind();
}

RemoteSync& RemoteSync::operator=(const RemoteSync& o)
{
    if (this == &o) return *this;
    cfg_ = o.cfg_;
    clock_ = o.clock_;
    players_ = o.players_;
    localId_ = o.localId_;
    hasLocalId_ = o.hasLocalId_;
    Rebind();
    return *this;
}

RemoteSync& RemoteSync::operator=(RemoteSync&& o)
{
    if (this == &o) return *this;
    cfg_ = o.cfg_;
    clock_ = std::move(o.clock_);
    players_ = std::move(o.players_);
    localId_ = o.localId_;
    hasLocalId_ = o.hasLocalId_;
    Rebind();
    return *this;
}

void RemoteSync::Rebind()
{
    for (auto& kv : players_) kv.second.SetClock(cfg_.sharedClock ? &clock_ : nullptr);
}

PushResult RemoteSync::OnPlayerState(uint16_t id, uint32_t tick, const proto::Kinematics& k, double nowMs,
                                     double ageMs, const proto::PedState& ped)
{
    if (id >= proto::kMaxPlayers || (hasLocalId_ && id == localId_)) return PushResult::Invalid;
    if (cfg_.sharedClock) clock_.OnFrame(tick, nowMs); // once per frame: the entries of one frame share (tick, nowMs)
    auto it = players_.find(id);
    if (it == players_.end())
    {
        it = players_.try_emplace(id, cfg_).first;
        if (cfg_.sharedClock) it->second.SetClock(&clock_);
    }
    const PushResult r = it->second.Push(tick, k, nowMs, ageMs, ped);
    if (!it->second.HasData()) players_.erase(it); // first packet was invalid: nothing to track
    // Only a sample that was new information asks the shared clock for delay (a repeat re-sent with a growing
    // age is not waited for).
    if (cfg_.sharedClock && (r == PushResult::Accepted || r == PushResult::Teleported)) clock_.NoteAge(ageMs);
    return r;
}

size_t RemoteSync::OnWorldState(const proto::WorldState& ws, double nowMs)
{
    size_t stored = 0;
    for (const proto::WorldEntry& e : ws.players)
    {
        const PushResult r = OnPlayerState(e.id, ws.tick, e.k, nowMs, static_cast<double>(e.ageMs), e.ped);
        if (r == PushResult::Accepted || r == PushResult::Teleported) ++stored;
    }
    return stored;
}

bool RemoteSync::Sample(uint16_t id, double nowMs, Pose& out)
{
    auto it = players_.find(id);
    return it != players_.end() && it->second.Sample(nowMs, out);
}

size_t RemoteSync::Prune(double nowMs, std::vector<uint16_t>* forgotten)
{
    size_t n = 0;
    for (auto it = players_.begin(); it != players_.end();)
    {
        if (!it->second.HasData() || it->second.Expired(nowMs))
        {
            if (forgotten) forgotten->push_back(it->first);
            it = players_.erase(it);
            ++n;
        }
        else ++it;
    }
    return n;
}

const PlayerSmoother* RemoteSync::Find(uint16_t id) const
{
    auto it = players_.find(id);
    return it == players_.end() ? nullptr : &it->second;
}

}
