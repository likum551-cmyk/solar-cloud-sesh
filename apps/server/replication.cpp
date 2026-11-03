#include "replication.h"

#include <algorithm>
#include <cmath>

namespace gage::server {

    namespace {

        constexpr uint8_t kRelevant = 1, kForced = 2;
        constexpr float kAccCap = 8.0f;            // a starved pair's priority stops growing here
        constexpr float kDue = 0.999f;             // accumulator value that counts as "due" (float slack)
        constexpr uint8_t kLeaveRepeat = 3;        // frames a "left your range" notice is repeated in
        constexpr size_t kLeavesPerPacket = 16;
        constexpr float kTierLookaheadSec = 0.5f;  // distance classes use distance minus this much closing
        constexpr uint32_t kCoarseRefreshMs = 5000; // an unchanged coarse position is re-sent this often
        constexpr size_t kCoarseMinBytes = 24;     // a coarse packet always has room for a few entries

        inline uint32_t Ctz32(uint32_t v)
        {
#if defined(__GNUC__) || defined(__clang__)
            return (uint32_t)__builtin_ctz(v);
#else
            uint32_t n = 0;
            while (!(v & 1u)) { v >>= 1; n++; }
            return n;
#endif
        }

        inline float Sq(float v) { return v * v; }

        // The idle dead-band (sync plan N4), per axis: 2 cm, 2 velocity steps (1/32 m/s), 36 heading units
        // (360/65536 degrees each), 3 dm of aim.
        constexpr int32_t kBandPosCm = 2, kBandVelSteps = 2, kBandHeadingUnits = 36, kBandAimDm = 3;

        inline int32_t AbsDiff(int32_t a, int32_t b) { return a > b ? a - b : b - a; }

        // True when sample `a` is inside the dead-band of reference `b`.
        inline bool InDeadBand(const proto::QState& a, const proto::QState& b)
        {
            const proto::QKin& p = a.k;
            const proto::QKin& r = b.k;
            if (AbsDiff(p.x, r.x) > kBandPosCm || AbsDiff(p.y, r.y) > kBandPosCm || AbsDiff(p.z, r.z) > kBandPosCm) return false;
            if (AbsDiff(p.vx, r.vx) > kBandVelSteps || AbsDiff(p.vy, r.vy) > kBandVelSteps || AbsDiff(p.vz, r.vz) > kBandVelSteps)
                return false;
            uint32_t dh = (uint32_t)(uint16_t)(p.heading - r.heading); // the short way round
            if (dh > 32768u) dh = 65536u - dh;
            if (dh > (uint32_t)kBandHeadingUnits) return false;
            // Everything else on foot must be equal. Comparing copies with the aim zeroed keeps this right when a
            // field is added to QPed (its operator== is the list of what counts as a change).
            if (AbsDiff(a.p.ax, b.p.ax) > kBandAimDm || AbsDiff(a.p.ay, b.p.ay) > kBandAimDm || AbsDiff(a.p.az, b.p.az) > kBandAimDm)
                return false;
            proto::QPed pa = a.p, pb = b.p;
            pa.ax = pa.ay = pa.az = pb.ax = pb.ay = pb.az = 0;
            return pa == pb;
        }

    }

    Replicator::Replicator() = default;
    Replicator::~Replicator() = default;

    void Replicator::Configure(const ReplConfig& in)
    {
        cfg_ = in;
        if (cfg_.maxPlayers < 1) cfg_.maxPlayers = 1;
        if (cfg_.maxPlayers > proto::kMaxPlayers) cfg_.maxPlayers = proto::kMaxPlayers;
        if (cfg_.tickHz < 1) cfg_.tickHz = 1;
        if (cfg_.enterRadius < 1) cfg_.enterRadius = 1;
        if (cfg_.leaveRadius < cfg_.enterRadius) cfg_.leaveRadius = cfg_.enterRadius;
        if (cfg_.maxRadius < cfg_.enterRadius) cfg_.maxRadius = cfg_.enterRadius;
        if (cfg_.cellSize < 64) cfg_.cellSize = 64; // the grid covers the whole quantiser box: 64 m is 512 x 512 cells
        if (cfg_.packetMaxBytes < 64) cfg_.packetMaxBytes = 64;
        if (cfg_.packetMaxBytes > 1400) cfg_.packetMaxBytes = 1400;
        if (cfg_.coarseSharePercent > 90) cfg_.coarseSharePercent = 90;
        auto rate = [&](uint32_t hz) { return (float)std::min<uint32_t>(std::max<uint32_t>(hz, 1), cfg_.tickHz); };
        incNear_ = rate(cfg_.rateNearHz) / (float)cfg_.tickHz;
        incMid_ = rate(cfg_.rateMidHz) / (float)cfg_.tickHz;
        incFar_ = rate(cfg_.rateFarHz) / (float)cfg_.tickHz;
        incIdle_ = rate(cfg_.rateIdleHz) / (float)cfg_.tickHz;
        hysteresis_ = (float)(cfg_.leaveRadius - cfg_.enterRadius);
        perTick_ = (float)cfg_.budgetBytesPerSec / (float)cfg_.tickHz;
        tokenCap_ = std::max((float)cfg_.packetMaxBytes, perTick_ * 2.0f);
        relEveryTicks_ = std::max<uint32_t>(1, (cfg_.relevanceEveryMs * cfg_.tickHz + 500) / 1000);
        idleAfterTicks_ = std::max<uint32_t>(1, (cfg_.idleAfterMs * cfg_.tickHz + 999) / 1000);
        const uint32_t coarseHz = std::min<uint32_t>(std::max<uint32_t>(cfg_.rateCoarseHz, 1), cfg_.tickHz);
        // One pass over the id space takes cycleTicks_ ticks, in pages: at most 4 coarse packets a
        // second per client (a packet costs ~45 B of headers and a send call, so fewer, bigger ones).
        cycleTicks_ = std::max<uint32_t>(1, cfg_.tickHz / coarseHz);
        pageEveryTicks_ = std::max<uint32_t>(1, std::min<uint32_t>(cycleTicks_, cfg_.tickHz / 4));
        const uint32_t pagesPerCycle = std::max<uint32_t>(1, cycleTicks_ / pageEveryTicks_);
        coarseStep_ = std::max<uint32_t>(1, (cfg_.maxPlayers + pagesPerCycle - 1) / pagesPerCycle);
        coarseRefreshTicks_ = std::max<uint32_t>(1, (kCoarseRefreshMs * cfg_.tickHz + 999) / 1000);

        subj_.assign(cfg_.maxPlayers, Subject());
        recv_.assign(cfg_.maxPlayers, Receiver());
        active_.clear();
        gridW_ = std::max(1, (int)std::ceil(2.0f * proto::kQuantMaxXY / (float)cfg_.cellSize));
        cells_.assign((size_t)gridW_ * (size_t)gridW_, std::vector<uint16_t>());
        tick_ = 0;
        step_ = 1;
        received_ = 0;
        enters_ = leaves_ = 0;
    }

    int Replicator::CellCoord(float v) const
    {
        int c = (int)std::floor((v + proto::kQuantMaxXY) / (float)cfg_.cellSize);
        if (c < 0) c = 0;
        if (c >= gridW_) c = gridW_ - 1;
        return c;
    }

    void Replicator::GridRemove(uint16_t id)
    {
        Subject& S = subj_[id];
        if (S.cell < 0) return;
        auto& v = cells_[(size_t)S.cell];
        const uint16_t last = v.back();
        v[S.cellPos] = last;
        subj_[last].cellPos = S.cellPos;
        v.pop_back();
        S.cell = -1;
    }

    void Replicator::GridMove(uint16_t id)
    {
        Subject& S = subj_[id];
        const int cell = CellCoord(S.k.y) * gridW_ + CellCoord(S.k.x);
        if (cell == S.cell) return;
        GridRemove(id);
        auto& v = cells_[(size_t)cell];
        S.cell = cell;
        S.cellPos = (uint32_t)v.size();
        v.push_back(id);
    }

    const proto::QState* Replicator::FindHist(const Subject& s, uint32_t tick) const
    {
        const uint32_t slot = tick % proto::kWorldHistory;
        return ((s.histValid >> slot) & 1u) && s.histTick[slot] == tick ? &s.histQ[slot] : nullptr;
    }

    void Replicator::RemoveFromRel(Receiver& R, uint16_t s)
    {
        const uint16_t idx = R.pair[s].relIdx;
        const uint16_t last = R.rel.back();
        R.rel[idx] = last;
        R.pair[last].relIdx = idx;
        R.rel.pop_back();
    }

    void Replicator::AddPair(uint16_t r, uint16_t s, bool forced)
    {
        Receiver& R = recv_[r];
        Pair& P = R.pair[s];
        if (P.flags & kRelevant)
        {
            if (forced) P.flags |= kForced;
            return;
        }
        P.flags = (uint8_t)(kRelevant | (forced ? kForced : 0));
        P.mask = 0;
        P.acc = 1.0f; // due at once: a newcomer to the set is sent in full on this very tick
        P.lastSent = 0;
        P.relIdx = (uint16_t)R.rel.size();
        R.rel.push_back(s);
        for (size_t i = 0; i < R.leaves.size(); i++)
            if (R.leaves[i].id == s)
            {
                R.leaves.erase(R.leaves.begin() + (long)i); // a stale "left" must not outlive the re-entry
                break;
            }
        enters_++;
    }

    void Replicator::DropPair(uint16_t r, uint16_t s, bool notify)
    {
        Receiver& R = recv_[r];
        Pair& P = R.pair[s];
        if (!(P.flags & kRelevant)) return;
        RemoveFromRel(R, s);
        P.flags = 0;
        P.mask = 0;
        P.acc = 0;
        P.coarseTick = 0; // the coarse channel takes over: send a blip soon
        if (notify) R.leaves.push_back(PendingLeave{s, kLeaveRepeat});
        leaves_++;
    }

    void Replicator::PurgeSubject(uint16_t id)
    {
        for (uint16_t r : active_)
        {
            if (r == id) continue;
            Receiver& R = recv_[r];
            if (R.pair.empty()) continue;
            if (R.pair[id].flags & kRelevant) RemoveFromRel(R, id);
            R.pair[id] = Pair();
            for (size_t i = 0; i < R.leaves.size(); i++)
                if (R.leaves[i].id == id)
                {
                    R.leaves.erase(R.leaves.begin() + (long)i);
                    break;
                }
        }
    }

    void Replicator::AddPlayer(uint16_t id)
    {
        if (id >= cfg_.maxPlayers || subj_[id].active) return;
        PurgeSubject(id);
        Subject& S = subj_[id];
        S = Subject();
        S.active = true;
        Receiver& R = recv_[id];
        R = Receiver();
        R.active = true;
        R.pair.assign(cfg_.maxPlayers, Pair());
        R.tokens = tokenCap_;
        R.coarseCursor = (uint16_t)((id * 7u) % cfg_.maxPlayers); // stagger the coarse load across ticks
        active_.push_back(id);
        for (uint16_t s : active_)
            if (s != id && subj_[s].forcedAll) AddPair(id, s, true);
    }

    void Replicator::RemovePlayer(uint16_t id)
    {
        if (id >= cfg_.maxPlayers || !subj_[id].active) return;
        PurgeSubject(id);
        GridRemove(id);
        subj_[id] = Subject();
        Receiver& R = recv_[id];
        R = Receiver();
        active_.erase(std::remove(active_.begin(), active_.end(), id), active_.end());
    }

    void Replicator::SetState(uint16_t id, const proto::Kinematics& k, const proto::PedState& ped, uint64_t nowUs)
    {
        if (id >= cfg_.maxPlayers || !subj_[id].active) return;
        Subject& S = subj_[id];
        S.k = k;
        S.q = proto::QuantizeState(k, ped);
        S.recvUs = nowUs;
        S.speed = std::sqrt(Sq(k.vx) + Sq(k.vy) + Sq(k.vz));
        S.seq++;
        received_++;
        if (!S.hasState)
        {
            S.hasState = true;
            S.prevQ = S.refQ = S.q;
            S.refSeq = S.seq;
            S.idleTicks = 0;
            S.exactIdle = 0;
        }
    }

    void Replicator::SetAck(uint16_t id, const proto::WorldAck& ack)
    {
        if (id >= cfg_.maxPlayers || !subj_[id].active) return;
        recv_[id].ack = ack;
    }

    bool Replicator::SetAlwaysRelevant(uint16_t subject, uint16_t receiver, bool on)
    {
        if (subject >= cfg_.maxPlayers || !subj_[subject].active) return false;
        if (receiver == proto::kServerId)
        {
            subj_[subject].forcedAll = on;
            for (uint16_t r : active_)
            {
                if (r == subject) continue;
                if (on)
                    AddPair(r, subject, true);
                else
                    recv_[r].pair[subject].flags &= (uint8_t)~kForced;
            }
            return true;
        }
        if (receiver >= cfg_.maxPlayers || !subj_[receiver].active || receiver == subject) return false;
        if (on)
            AddPair(receiver, subject, true);
        else
            recv_[receiver].pair[subject].flags &= (uint8_t)~kForced;
        return true;
    }

    bool Replicator::AlwaysRelevant(uint16_t subject, uint16_t receiver) const
    {
        if (subject >= cfg_.maxPlayers || !subj_[subject].active) return false;
        if (receiver == proto::kServerId) return subj_[subject].forcedAll;
        if (receiver >= cfg_.maxPlayers || !subj_[receiver].active || receiver == subject) return false;
        return (recv_[receiver].pair[subject].flags & kForced) != 0 || subj_[subject].forcedAll;
    }

    bool Replicator::Relevant(uint16_t receiver, uint16_t subject) const
    {
        if (receiver >= cfg_.maxPlayers || subject >= cfg_.maxPlayers || !recv_[receiver].active) return false;
        return (recv_[receiver].pair[subject].flags & kRelevant) != 0;
    }

    size_t Replicator::RelevantCount(uint16_t receiver) const
    {
        return receiver < cfg_.maxPlayers ? recv_[receiver].rel.size() : 0;
    }

    size_t Replicator::TotalRelevantPairs() const
    {
        size_t n = 0;
        for (uint16_t r : active_) n += recv_[r].rel.size();
        return n;
    }

    float Replicator::EnterRadius(float speedA, float speedB) const
    {
        const float grown = (float)cfg_.enterRadius + (float)cfg_.lookaheadMs * 0.001f * (speedA + speedB);
        return std::min((float)cfg_.maxRadius, grown);
    }

    // ---- the tick ---------------------------------------------------------------------------------------

    void Replicator::Tick(uint32_t tick, uint64_t nowUs, const Sink& sink, Metrics& m)
    {
        // Ticks since the previous call: 1 normally, more after a server stall (tick numbers follow wall time).
        step_ = tick_ != 0 && tick > tick_ ? tick - tick_ : 1;
        tick_ = tick;
        nowUs_ = nowUs;
        m.samplesReceived += received_;
        received_ = 0;

        // 1. Everyone's state for this tick: history, idle count, sample age, grid cell.
        for (uint16_t id : active_)
        {
            Subject& S = subj_[id];
            if (!S.hasState) continue;
            // A player that leaves the dead-band after standing still is due at once for everybody: the idle
            // rate must not delay the first update of a start-off by up to a second. A sample that stays inside
            // the band of the reference is not activity (see SetState): it neither wakes the player nor changes
            // what is replicated.
            const bool inBand = InDeadBand(S.q, S.refQ);
            S.woke = !inBand && S.idleTicks >= idleAfterTicks_;
            if (S.woke)
                m.wakes++;
            else if (inBand && S.q != S.prevQ && S.exactIdle >= idleAfterTicks_)
                m.noiseWakesHeld++; // a bit-exact idle test would have woken this player here
            S.exactIdle = S.q == S.prevQ ? S.exactIdle + step_ : 0;
            S.prevQ = S.q;
            if (inBand)
                S.idleTicks += step_;
            else
            {
                S.refQ = S.q;
                S.refSeq = S.seq;
                S.idleTicks = 0;
            }
            const uint32_t slot = tick % proto::kWorldHistory;
            S.histTick[slot] = tick;
            S.histQ[slot] = S.refQ;
            S.histValid |= 1u << slot;
            const uint64_t ageUs = nowUs > S.recvUs ? nowUs - S.recvUs : 0;
            S.ageMs = (uint8_t)std::min<uint64_t>(255, ageUs / 1000);
            GridMove(id);
        }

        // 2. Each receiver: relevance, priorities, the world frame, the coarse page.
        for (size_t i = 0; i < active_.size(); i++)
            ProcessReceiver(active_[i], tick, sink, m);

        m.relevanceEnters += enters_;
        m.relevanceLeaves += leaves_;
        enters_ = leaves_ = 0;
        m.relevantPairs = TotalRelevantPairs();
    }

    void Replicator::FindEnters(uint16_t r)
    {
        const Subject& me = subj_[r];
        Receiver& R = recv_[r];
        const float reach = (float)cfg_.maxRadius;
        const int x0 = CellCoord(me.k.x - reach), x1 = CellCoord(me.k.x + reach);
        const int y0 = CellCoord(me.k.y - reach), y1 = CellCoord(me.k.y + reach);
        for (int cy = y0; cy <= y1; cy++)
            for (int cx = x0; cx <= x1; cx++)
            {
                const auto& cell = cells_[(size_t)cy * (size_t)gridW_ + (size_t)cx];
                for (uint16_t s : cell)
                {
                    if (s == r) continue;
                    const Subject& S = subj_[s];
                    if (!S.hasState || (R.pair[s].flags & kRelevant)) continue;
                    const float enter = EnterRadius(me.speed, S.speed);
                    const float d2 = Sq(S.k.x - me.k.x) + Sq(S.k.y - me.k.y) + Sq(S.k.z - me.k.z);
                    if (d2 <= enter * enter) AddPair(r, s, false);
                }
            }
    }

    void Replicator::ProcessReceiver(uint16_t r, uint32_t tick, const Sink& sink, Metrics& m)
    {
        Receiver& R = recv_[r];
        const Subject& me = subj_[r];
        R.tokens = std::min(R.tokens + perTick_, tokenCap_);

        // Newly relevant players (staggered by id so the grid queries spread over the ticks).
        if (me.hasState && (tick + r) % relEveryTicks_ == 0) FindEnters(r);

        // Every relevant pair: shift its history mask, apply the leave radius, accumulate priority.
        cands_.clear();
        for (size_t i = R.rel.size(); i-- > 0;)
        {
            const uint16_t s = R.rel[i];
            Pair& P = R.pair[s];
            const Subject& S = subj_[s];
            // "Sent k ticks ago" moves by the ticks that passed: more than one after a server stall, or the
            // frame sent just before it would no longer be found as a baseline (and every entry after a stall
            // went out in full). Past 32 ticks no baseline survives anyway.
            P.mask = step_ >= 32 ? 0u : P.mask << step_;
            if (!S.hasState || !me.hasState)
            {
                m.pairNoState++; // nothing to measure or send yet
                continue;
            }
            const float d2 = Sq(S.k.x - me.k.x) + Sq(S.k.y - me.k.y) + Sq(S.k.z - me.k.z);
            const float vsum = me.speed + S.speed;
            if (!(P.flags & kForced))
            {
                const float leave = EnterRadius(me.speed, S.speed) + hysteresis_;
                if (d2 > leave * leave)
                {
                    DropPair(r, s, true);
                    continue;
                }
            }
            const float dist = std::sqrt(d2);
            const float dEff = std::max(0.0f, dist - kTierLookaheadSec * vsum);
            float inc = dEff < (float)cfg_.nearDist ? incNear_ : dEff < (float)cfg_.midDist ? incMid_ : incFar_;
            uint8_t tier = dEff < (float)cfg_.nearDist ? 0 : dEff < (float)cfg_.midDist ? 1 : 2;
            const bool idle = S.idleTicks >= idleAfterTicks_;
            const bool idleHeld = idle && incIdle_ < inc; // the idle rate is slower than the distance class's
            if (idle) inc = std::min(inc, incIdle_);
            if (idleHeld) tier = 3;
            P.acc = std::min(P.acc + inc, kAccCap);
            if (S.woke && P.acc < 1.0f) P.acc = 1.0f;
            if (P.acc >= kDue)
                cands_.push_back(Cand{s, P.acc, d2, nullptr, 0, proto::EntryPlan(), 0, P.lastSent, tier});
            else if (idleHeld)
                m.pairIdleSuppressed++;
            else
                m.pairRateNotDue++;
        }

        // The coarse page first (a few times a second, staggered by id): its share of the budget
        // is reserved, so a crowded neighbourhood cannot starve the map blips.
        if ((tick + r) % pageEveryTicks_ == 0)
        {
            const size_t coarseCap = std::max<size_t>(
                kCoarseMinBytes, (size_t)(perTick_ * (float)pageEveryTicks_ * (float)cfg_.coarseSharePercent / 100.0f));
            const size_t tokensNow = R.tokens > 0 ? (size_t)R.tokens : 0;
            const size_t coarseUsed =
                BuildCoarse(r, tick, std::min<size_t>({coarseCap, tokensNow, (size_t)cfg_.packetMaxBytes}), sink, m);
            R.tokens -= (float)coarseUsed;
        }

        const size_t nLeave = std::min(R.leaves.size(), kLeavesPerPacket);
        if (cands_.empty() && nLeave == 0) return;

        // Sizes: each candidate against the newest acknowledged baseline it has.
        const uint32_t acked = proto::AckedAges(R.ack, tick);
        const size_t gapEst = cfg_.maxPlayers > 128 ? 2 : 1; // varint bytes of an id gap, worst case
        size_t sum = 0;
        for (Cand& c : cands_)
        {
            const Pair& P = R.pair[c.id];
            const Subject& S = subj_[c.id];
            const uint32_t usable = P.mask & acked;
            if (usable)
            {
                const uint32_t k = Ctz32(usable);
                if (const proto::QState* b = FindHist(S, tick - k))
                {
                    c.base = b;
                    c.baseAge = (uint8_t)k;
                }
            }
            c.plan = proto::PlanEntry(S.refQ, c.base);
            c.bytes = gapEst + 2 + c.plan.payloadBytes;
            sum += c.bytes;
        }

        const size_t avail = R.tokens > 0 ? (size_t)R.tokens : 0;
        const size_t cap = std::min<size_t>(cfg_.packetMaxBytes, avail);
        const size_t header = proto::WorldHeaderBytes(nLeave, 1023); // count varint at its widest
        picked_.clear();
        if (header + sum <= cap)
            picked_ = cands_;
        else
        {
            // Over budget: highest accumulated priority first. Pairs starved long enough all sit at the
            // accumulator cap, so among equals the one served longest ago goes first (round robin: with
            // a distance tie-break the farthest pairs would starve for good); nearest breaks the rest.
            std::sort(cands_.begin(), cands_.end(), [](const Cand& a, const Cand& b) {
                if (a.acc != b.acc) return a.acc > b.acc;
                if (a.lastSent != b.lastSent) return a.lastSent < b.lastSent;
                return a.d2 < b.d2;
            });
            size_t used = header;
            for (const Cand& c : cands_)
            {
                if (used + c.bytes > cap) continue; // a smaller one further down may still fit
                used += c.bytes;
                picked_.push_back(c);
            }
            const uint64_t deferred = cands_.size() - picked_.size();
            m.budgetDeferred += deferred;
            // What was binding: the receiver's byte budget when it holds less than a whole packet, else the
            // packet size cap.
            (avail < cfg_.packetMaxBytes ? m.pairBudgetDeferred : m.pairPacketCapDeferred) += deferred;
        }
        if (picked_.empty() && (nLeave == 0 || header > cap)) return;

        std::sort(picked_.begin(), picked_.end(), [](const Cand& a, const Cand& b) { return a.id < b.id; });
        builder_.Begin(tick);
        for (size_t i = 0; i < nLeave; i++) builder_.AddLeave(R.leaves[i].id);
        uint64_t deltas = 0;
        for (const Cand& c : picked_)
        {
            const Subject& S = subj_[c.id];
            bool wasDelta = false;
            builder_.AddEntry(c.id, S.ageMs, S.refQ, c.base, c.baseAge, &wasDelta);
            if (subj_[c.id].fwdTick != tick)
            {
                // The first time this tick that the subject goes out: a new sample is "forwarded", the same one
                // again (an idle refresh, a sender slower than the tick) is "forwarded twice".
                Subject& W = subj_[c.id];
                if (W.fwdSeq == W.refSeq)
                    m.samplesForwardedTwice++;
                else
                    m.samplesForwarded++;
                W.fwdSeq = W.refSeq;
                W.fwdTick = tick;
            }
            m.pairSentTier[c.tier < 4 ? c.tier : 3]++;
            Pair& P = R.pair[c.id];
            P.mask |= 1u;
            P.lastSent = tick;
            // Keep at most half an update of surplus: a pair served on time stays on its rate, a pair
            // that waited does not get a burst of catch-up sends after it.
            P.acc = std::max(0.0f, std::min(P.acc - 1.0f, 0.5f));
            if (wasDelta) deltas++;
        }
        // Leave notices are repeated for a few frames (they ride an unreliable channel).
        for (size_t i = 0; i < nLeave; i++) R.leaves[i].remain--;
        R.leaves.erase(std::remove_if(R.leaves.begin(), R.leaves.end(), [](const PendingLeave& l) { return l.remain == 0; }),
                       R.leaves.end());

        const proto::Bytes bytes = builder_.Finish();
        R.tokens -= (float)bytes.size();
        m.worldPackets++;
        m.worldEntries += picked_.size();
        m.pairSent += picked_.size();
        m.worldPayloadBytes += bytes.size();
        m.worldBytes.Add(bytes.size());
        m.worldEntryCount.Add(picked_.size());
        m.deltaEntries += deltas;
        m.absoluteEntries += picked_.size() - deltas;
        sink(r, false, bytes);
    }

    // One page of the coarse channel for receiver `r`: the next few ids after its cursor that are
    // players, not relevant to it, and whose coarse position changed (or is due a refresh).
    size_t Replicator::BuildCoarse(uint16_t r, uint32_t tick, size_t maxBytes, const Sink& sink, Metrics& m)
    {
        Receiver& R = recv_[r];
        if (maxBytes < kCoarseMinBytes) return 0;
        page_.clear();
        const size_t headerBytes = proto::CoarseHeaderBytes(cfg_.maxPlayers); // widest count
        const size_t perEntry = (cfg_.maxPlayers > 128 ? 2 : 1) + 6;
        size_t bytes = headerBytes;
        uint32_t cursor = R.coarseCursor;
        uint32_t examined = 0;
        while (examined < coarseStep_)
        {
            const uint16_t s = (uint16_t)cursor;
            if (s != r && subj_[s].active && subj_[s].hasState && !(R.pair[s].flags & kRelevant))
            {
                Pair& P = R.pair[s];
                const proto::QCoarse c = proto::QuantizeCoarse(subj_[s].q.k);
                const bool due = P.coarseTick == 0 || c != P.lastCoarse || tick - P.coarseTick >= coarseRefreshTicks_;
                if (due)
                {
                    if (bytes + perEntry > maxBytes) break; // resume here next tick: nobody is skipped
                    bytes += perEntry;
                    page_.push_back(proto::CoarseWire{s, c});
                    P.lastCoarse = c;
                    P.coarseTick = tick;
                }
            }
            cursor = (cursor + 1) % cfg_.maxPlayers;
            examined++;
        }
        R.coarseCursor = (uint16_t)cursor;
        if (page_.empty()) return 0;
        std::sort(page_.begin(), page_.end(), [](const proto::CoarseWire& a, const proto::CoarseWire& b) { return a.id < b.id; });
        const proto::Bytes pkt = proto::EncodeCoarse(tick, page_);
        if (pkt.empty()) return 0;
        m.coarsePackets++;
        m.coarseEntries += page_.size();
        m.coarsePayloadBytes += pkt.size();
        sink(r, true, pkt);
        return pkt.size();
    }

}
