// WorldState / WorldCoarse codec of protocol v5 (see worldcodec.h for the delta scheme and
// docs/api/protocol.md for the byte layouts). Portable C++17, no Windows headers.
#include "worldcodec.h"

#include "qwire.h"
#include "wire.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gage::proto {

    namespace {

        constexpr uint8_t kPosBit = kChgPos, kHeadBit = kChgHead, kVelBit = kChgVel;
        constexpr uint8_t kBaseMask = 0x1F; // header bits 0..4: baseline age; bits 5..7: kPosBit/kHeadBit/kVelBit << 5
        constexpr uint8_t kExtKnown = kExtMove | kExtFlags | kExtWeapon | kExtAim | kExtShot | kExtVitals | kExtVeh;

        constexpr size_t kMinEntryBytes = 3;  // id gap + header + age (a delta adds its extension byte)
        constexpr size_t kCoarsePayload = 6;  // x, y, z as u16

        uint32_t ZigZag(int32_t v) { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
        size_t VarSigned(int32_t v) { return VarintBytes(ZigZag(v)); }
        int32_t HeadingDelta(uint16_t cur, uint16_t base) { return (int16_t)(uint16_t)(cur - base); }

        QState FromKin(const QKin& k)
        {
            QState s;
            s.k = k;
            return s;
        }

        // Applies the on-foot part of a delta entry (extension byte `ext`, fields in the order
        // move byte, vehicle id (v5), flags, weapon, shot, aim, vitals) to `q`, which holds the baseline. The aim
        // follows the RESULTING aiming flag: without it the aim is 0 and the aim bit must be clear; with it the
        // aim bit carries vari32 deltas against the baseline's aim (0 when the baseline was not
        // aiming), and a missing aim bit keeps the baseline's aim. The vehicle id follows the RESULTING move: a
        // move that names no vehicle has id 0 and the vehicle bit must be clear; entering / exiting keeps the
        // baseline's id unless the vehicle bit carries a new one.
        bool ApplyPedDelta(wire::Reader& r, uint8_t ext, QPed& q)
        {
            if (ext & ~kExtKnown) return false;
            if (ext & kExtMove)
            {
                if (!qwire::SplitMoveByte(r.u8(), q)) return false;
            }
            if (!MoveNamesVehicle(q.move))
            {
                if (ext & kExtVeh) return false; // no vehicle without entering / exiting
                q.vehId = 0;
            }
            else if (ext & kExtVeh)
            {
                q.vehId = r.u8();
            }
            if (ext & kExtFlags) q.flags = r.u16();
            if (ext & kExtWeapon) q.weapon = r.u32();
            if (ext & kExtShot)
            {
                const uint8_t shot = r.u8();
                if (shot & 0xF0u) return false;
                q.shotSeq = shot;
            }
            if (!r.ok()) return false;
            if (!(q.flags & kPfAiming))
            {
                if (ext & kExtAim) return false; // no aim without the aiming flag
                q.ax = q.ay = q.az = 0;
            }
            else if (ext & kExtAim)
            {
                const int64_t nx = (int64_t)q.ax + r.vari32();
                const int64_t ny = (int64_t)q.ay + r.vari32();
                const int64_t nz = (int64_t)q.az + r.vari32();
                if (!r.ok() || nx < -kQMaxAim || nx > kQMaxAim || ny < -kQMaxAim || ny > kQMaxAim || nz < -kQMaxAim ||
                    nz > kQMaxAim)
                    return false;
                q.ax = (int16_t)nx;
                q.ay = (int16_t)ny;
                q.az = (int16_t)nz;
            }
            if ((ext & kExtVitals) && !qwire::GetVitals(r, q)) return false;
            return r.ok() && QPedInRange(q);
        }

        // Reads the entries of a WorldState body. `lookup(id, baseTick)` returns the baseline or
        // null. The stateless Decode and the stateful WorldDecoder differ only in that callback.
        // On success `qs` holds the decoded wire states parallel to out.players.
        template <class Lookup>
        bool ParseWorld(const uint8_t* p, size_t n, Lookup&& lookup, WorldState& out, std::vector<QState>& qs,
                        uint64_t& deltas, uint64_t& absolutes)
        {
            wire::Reader r(p, n);
            if (r.u8() != (uint8_t)Msg::WorldState || !r.ok()) return false;
            WorldState m;
            m.tick = r.u32();
            const uint8_t nLeft = r.u8();
            // Tick 0 is "nothing received" in a WorldAck, so no frame carries it.
            if (!r.ok() || m.tick == 0 || r.remaining() < (size_t)nLeft * 2) return false;
            m.left.reserve(nLeft);
            for (uint8_t i = 0; i < nLeft; i++)
            {
                const uint16_t id = r.u16();
                if (id >= kMaxPlayers) return false;
                m.left.push_back(id);
            }
            const uint32_t count = r.varu32();
            // Refuse a hostile count before reserving anything for it.
            if (!r.ok() || count > kMaxPlayers || r.remaining() < (size_t)count * kMinEntryBytes) return false;
            m.players.reserve(count);
            qs.clear();
            qs.reserve(count);
            int64_t prev = -1;
            uint64_t d = 0, a = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                const uint32_t gap = r.varu32();
                const uint8_t flags = r.u8();
                const uint8_t age = r.u8();
                if (!r.ok()) return false;
                const int64_t id = prev + 1 + (int64_t)gap;
                if (id >= kMaxPlayers) return false;
                prev = id;
                const uint8_t baseAge = flags & kBaseMask;
                const uint8_t bits = (uint8_t)(flags >> 5);
                QState s;
                QKin& q = s.k;
                if (baseAge == 0)
                {
                    if (bits != 0) return false; // an absolute entry has nothing to flag
                    if (!qwire::GetAbsolute(r, q)) return false;
                    if (!qwire::GetPed(r, s.p) || !qwire::GetVitals(r, s.p)) return false;
                    a++;
                }
                else
                {
                    const uint8_t ext = r.u8();
                    if (!r.ok()) return false;
                    const QState* baseState = lookup((uint16_t)id, m.tick - baseAge);
                    if (!baseState) return false;
                    s = *baseState;
                    const QKin* base = &baseState->k;
                    if (bits & kPosBit)
                    {
                        const int64_t nx = (int64_t)base->x + r.vari32();
                        const int64_t ny = (int64_t)base->y + r.vari32();
                        const int64_t nz = (int64_t)base->z + r.vari32();
                        if (!r.ok() || nx < -kQMaxXY || nx > kQMaxXY || ny < -kQMaxXY || ny > kQMaxXY || nz < kQMinZ ||
                            nz > kQMaxZ)
                            return false;
                        q.x = (int32_t)nx;
                        q.y = (int32_t)ny;
                        q.z = (int32_t)nz;
                    }
                    if (bits & kHeadBit)
                    {
                        const int32_t dh = r.vari32();
                        if (!r.ok() || dh < -32768 || dh > 32767) return false;
                        q.heading = (uint16_t)(base->heading + dh);
                    }
                    if (bits & kVelBit)
                    {
                        const int64_t nvx = (int64_t)base->vx + r.vari32();
                        const int64_t nvy = (int64_t)base->vy + r.vari32();
                        const int64_t nvz = (int64_t)base->vz + r.vari32();
                        if (!r.ok() || nvx < -kQMaxVel || nvx > kQMaxVel || nvy < -kQMaxVel || nvy > kQMaxVel ||
                            nvz < -kQMaxVel || nvz > kQMaxVel)
                            return false;
                        q.vx = (int16_t)nvx;
                        q.vy = (int16_t)nvy;
                        q.vz = (int16_t)nvz;
                    }
                    if (!ApplyPedDelta(r, ext, s.p)) return false;
                    d++;
                }
                if (!r.ok() || !QInRange(q)) return false;
                WorldEntry e;
                e.id = (uint16_t)id;
                e.k = Dequantize(q);
                e.ageMs = age;
                e.ped = DequantizePed(s.p);
                m.players.push_back(e);
                qs.push_back(s);
            }
            if (!r.done()) return false;
            out = std::move(m);
            deltas = d;
            absolutes = a;
            return true;
        }

    }

    // ---- sizes and planning ------------------------------------------------------------------

    size_t VarintBytes(uint32_t v)
    {
        size_t n = 1;
        while (v >= 0x80)
        {
            v >>= 7;
            n++;
        }
        return n;
    }

    EntryPlan PlanEntry(const QState& curState, const QState* baseState)
    {
        EntryPlan absolute; // delta=false
        absolute.payloadBytes = qwire::AbsoluteStateBytes(curState);
        if (!baseState) return absolute;
        const QKin& cur = curState.k;
        const QKin* base = &baseState->k;
        EntryPlan p;
        p.delta = true;
        size_t bytes = 1; // the extension byte
        const int32_t dx = cur.x - base->x, dy = cur.y - base->y, dz = cur.z - base->z;
        if (dx | dy | dz)
        {
            p.changeBits |= kPosBit;
            bytes += VarSigned(dx) + VarSigned(dy) + VarSigned(dz);
        }
        const int32_t dh = HeadingDelta(cur.heading, base->heading);
        if (dh)
        {
            p.changeBits |= kHeadBit;
            bytes += VarSigned(dh);
        }
        const int32_t dvx = (int32_t)cur.vx - base->vx, dvy = (int32_t)cur.vy - base->vy, dvz = (int32_t)cur.vz - base->vz;
        if (dvx | dvy | dvz)
        {
            p.changeBits |= kVelBit;
            bytes += VarSigned(dvx) + VarSigned(dvy) + VarSigned(dvz);
        }
        const QPed& cp = curState.p;
        const QPed& bp = baseState->p;
        if (cp.move != bp.move || cp.jumpSeq != bp.jumpSeq)
        {
            p.extBits |= kExtMove;
            bytes += 1;
        }
        // The vehicle id, only for a move that names one, and only when it differs from the baseline's (0 when the
        // baseline's move named none: the QPed invariant).
        if (MoveNamesVehicle(cp.move) && cp.vehId != bp.vehId)
        {
            p.extBits |= kExtVeh;
            bytes += 1;
        }
        if (cp.flags != bp.flags)
        {
            p.extBits |= kExtFlags;
            bytes += 2;
        }
        if (cp.weapon != bp.weapon)
        {
            p.extBits |= kExtWeapon;
            bytes += 4;
        }
        if (cp.shotSeq != bp.shotSeq)
        {
            p.extBits |= kExtShot;
            bytes += 1;
        }
        if (cp.health != bp.health || cp.armor != bp.armor)
        {
            p.extBits |= kExtVitals;
            bytes += qwire::kVitalsBytes;
        }
        // Without the aiming flag the aim is implicitly 0 (QPed invariant), so it never travels.
        const int32_t dax = (int32_t)cp.ax - bp.ax, day = (int32_t)cp.ay - bp.ay, daz = (int32_t)cp.az - bp.az;
        if ((cp.flags & kPfAiming) && (dax | day | daz))
        {
            p.extBits |= kExtAim;
            bytes += VarSigned(dax) + VarSigned(day) + VarSigned(daz);
        }
        if (bytes >= absolute.payloadBytes) return absolute;
        p.payloadBytes = bytes;
        return p;
    }

    EntryPlan PlanEntry(const QKin& cur, const QKin* base)
    {
        const QState c = FromKin(cur);
        if (!base) return PlanEntry(c, nullptr);
        const QState b = FromKin(*base);
        return PlanEntry(c, &b);
    }

    size_t EntryBytes(uint32_t idGap, const EntryPlan& plan)
    {
        return VarintBytes(idGap) + 2 + plan.payloadBytes;
    }

    size_t WorldHeaderBytes(size_t nLeft, size_t nEntries)
    {
        return 1 + 4 + 1 + 2 * nLeft + VarintBytes((uint32_t)nEntries);
    }

    uint32_t AckedAges(const WorldAck& ack, uint32_t tick)
    {
        if (ack.tick == 0 || SeqNewer(ack.tick, tick)) return 0; // nothing yet, or from the future
        const uint32_t d = tick - ack.tick; // age of the newest acknowledged frame
        if (d > kMaxBaseAge) return 0;      // older than the 32-frame window
        const uint64_t m = (1ull << d) | ((uint64_t)ack.bits << (d + 1));
        return (uint32_t)(m & 0xFFFFFFFEull); // ages 1..31; age 0 is the frame being built
    }

    // ---- server side: WorldPacketBuilder --------------------------------------------------------

    void WorldPacketBuilder::Begin(uint32_t tick)
    {
        tick_ = tick;
        left_.clear();
        entries_.clear();
        count_ = 0;
        prevId_ = -1;
    }

    void WorldPacketBuilder::AddLeave(uint16_t id)
    {
        if (left_.size() < kMaxLeavePerPacket && id < kMaxPlayers) left_.push_back(id);
    }

    bool WorldPacketBuilder::AddEntry(uint16_t id, uint8_t ageMs, const QKin& cur, const QKin* base, uint8_t baseAge,
                                      bool* wasDelta)
    {
        const QState c = FromKin(cur);
        if (!base) return AddEntry(id, ageMs, c, nullptr, baseAge, wasDelta);
        const QState b = FromKin(*base);
        return AddEntry(id, ageMs, c, &b, baseAge, wasDelta);
    }

    bool WorldPacketBuilder::AddEntry(uint16_t id, uint8_t ageMs, const QState& curState, const QState* baseState,
                                      uint8_t baseAge, bool* wasDelta)
    {
        if (id >= kMaxPlayers || (int32_t)id <= prevId_) return false;
        const bool canDelta = baseState && baseAge >= 1 && baseAge <= kMaxBaseAge;
        const EntryPlan plan = PlanEntry(curState, canDelta ? baseState : nullptr);
        const uint32_t gap = (uint32_t)((int32_t)id - prevId_ - 1);
        const QKin& cur = curState.k;
        wire::Writer& w = entries_; // written in place: no allocation per entry
        w.varu32(gap);
        w.u8((uint8_t)((plan.delta ? baseAge : 0) | (plan.changeBits << 5)));
        w.u8(ageMs);
        if (!plan.delta)
        {
            qwire::PutAbsolute(w, cur);
            qwire::PutPed(w, curState.p);
            qwire::PutVitals(w, curState.p);
        }
        else
        {
            const QKin* base = &baseState->k;
            w.u8(plan.extBits);
            if (plan.changeBits & kPosBit)
            {
                w.vari32(cur.x - base->x);
                w.vari32(cur.y - base->y);
                w.vari32(cur.z - base->z);
            }
            if (plan.changeBits & kHeadBit) w.vari32(HeadingDelta(cur.heading, base->heading));
            if (plan.changeBits & kVelBit)
            {
                w.vari32((int32_t)cur.vx - base->vx);
                w.vari32((int32_t)cur.vy - base->vy);
                w.vari32((int32_t)cur.vz - base->vz);
            }
            const QPed& cp = curState.p;
            const QPed& bp = baseState->p;
            if (plan.extBits & kExtMove) w.u8(qwire::MoveByte(cp));
            if (plan.extBits & kExtVeh) w.u8(cp.vehId);
            if (plan.extBits & kExtFlags) w.u16(cp.flags);
            if (plan.extBits & kExtWeapon) w.u32(cp.weapon);
            if (plan.extBits & kExtShot) w.u8((uint8_t)(cp.shotSeq & 15u));
            if (plan.extBits & kExtAim)
            {
                w.vari32((int32_t)cp.ax - bp.ax);
                w.vari32((int32_t)cp.ay - bp.ay);
                w.vari32((int32_t)cp.az - bp.az);
            }
            if (plan.extBits & kExtVitals) qwire::PutVitals(w, cp);
        }
        count_++;
        prevId_ = id;
        if (wasDelta) *wasDelta = plan.delta;
        return true;
    }

    size_t WorldPacketBuilder::size() const
    {
        return WorldHeaderBytes(left_.size(), count_) + entries_.size();
    }

    Bytes WorldPacketBuilder::Finish()
    {
        wire::Writer w;
        w.reserve(size());
        w.u8((uint8_t)Msg::WorldState);
        w.u32(tick_);
        w.u8((uint8_t)left_.size());
        for (uint16_t id : left_) w.u16(id);
        w.varu32((uint32_t)count_);
        w.bytes(entries_.data().data(), entries_.size());
        return w.take();
    }

    // ---- client side: WorldDecoder ---------------------------------------------------------------

    struct WorldDecoder::Hist
    {
        uint32_t tick[kWorldHistory] = {};
        QState q[kWorldHistory];
        uint32_t valid = 0; // bit per slot

        const QState* Find(uint32_t t) const
        {
            const uint32_t s = t % kWorldHistory;
            return ((valid >> s) & 1u) && tick[s] == t ? &q[s] : nullptr;
        }
        void Put(uint32_t t, const QState& v)
        {
            const uint32_t s = t % kWorldHistory;
            tick[s] = t;
            q[s] = v;
            valid |= 1u << s;
        }
    };

    WorldDecoder::WorldDecoder() : hist_(kMaxPlayers) {}
    WorldDecoder::~WorldDecoder() = default;

    void WorldDecoder::Reset()
    {
        for (auto& h : hist_) h.reset();
        ackTick_ = ackBits_ = 0;
        haveAck_ = false;
        frames_ = rejected_ = stale_ = deltaEntries_ = absoluteEntries_ = 0;
    }

    bool WorldDecoder::Decode(const uint8_t* p, size_t n, WorldState& out)
    {
        // Peek the tick for the staleness test before doing any work.
        if (n >= 5 && p[0] == (uint8_t)Msg::WorldState)
        {
            const uint32_t tick = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
            if (haveAck_ && !SeqNewer(tick, ackTick_))
            {
                stale_++;
                return false;
            }
        }
        auto lookup = [this](uint16_t id, uint32_t baseTick) -> const QState* {
            const auto& h = hist_[id];
            return h ? h->Find(baseTick) : nullptr;
        };
        WorldState ws;
        std::vector<QState> qs;
        uint64_t d = 0, a = 0;
        if (!ParseWorld(p, n, lookup, ws, qs, d, a))
        {
            rejected_++;
            return false;
        }
        // Commit: forget the players that left range, then remember this frame's states.
        for (uint16_t id : ws.left)
            if (hist_[id]) hist_[id]->valid = 0;
        for (size_t i = 0; i < ws.players.size(); i++)
        {
            auto& h = hist_[ws.players[i].id];
            if (!h) h = std::make_unique<Hist>();
            h->Put(ws.tick, qs[i]);
        }
        // Receipt: the new tick, with the old newest tick and the old bits shifted behind it.
        if (!haveAck_)
        {
            ackTick_ = ws.tick;
            ackBits_ = 0;
            haveAck_ = true;
        }
        else
        {
            const uint32_t shift = ws.tick - ackTick_; // >= 1: the staleness test above passed
            ackBits_ = shift > 32 ? 0u : (uint32_t)(((uint64_t)ackBits_ << shift) | (1ull << (shift - 1)));
            ackTick_ = ws.tick;
        }
        frames_++;
        deltaEntries_ += d;
        absoluteEntries_ += a;
        out = std::move(ws);
        return true;
    }

    // ---- stateless Encode/Decode of WorldState (absolute entries only) ---------------------------------

    Bytes Encode(const WorldState& m)
    {
        if (m.players.size() > kMaxPlayers || m.left.size() > kMaxLeavePerPacket) return {};
        WorldPacketBuilder b;
        b.Begin(m.tick);
        for (uint16_t id : m.left) b.AddLeave(id);
        for (const WorldEntry& e : m.players)
        {
            if (!std::isfinite(e.k.x) || !std::isfinite(e.k.y) || !std::isfinite(e.k.z) || !std::isfinite(e.k.heading) ||
                !std::isfinite(e.k.vx) || !std::isfinite(e.k.vy) || !std::isfinite(e.k.vz))
                return {};
            if (e.ped.move > 7) return {}; // a 3-bit field
            if ((e.ped.flags & kPfAiming) &&
                !(std::isfinite(e.ped.aimX) && std::isfinite(e.ped.aimY) && std::isfinite(e.ped.aimZ)))
                return {};
            const QState s = QuantizeState(e.k, e.ped);
            if (!b.AddEntry(e.id, e.ageMs, s, nullptr, 0)) return {}; // ids not ascending or too large
        }
        return b.Finish();
    }

    bool Decode(const uint8_t* p, size_t n, WorldState& out)
    {
        auto none = [](uint16_t, uint32_t) -> const QState* { return nullptr; };
        std::vector<QState> qs;
        uint64_t d = 0, a = 0;
        return ParseWorld(p, n, none, out, qs, d, a);
    }

    // ---- the coarse channel -----------------------------------------------------------------------------

    size_t CoarseHeaderBytes(size_t nEntries)
    {
        return 1 + 4 + VarintBytes((uint32_t)nEntries);
    }

    Bytes EncodeCoarse(uint32_t tick, const std::vector<CoarseWire>& entries)
    {
        if (entries.size() > kMaxPlayers) return {};
        wire::Writer w;
        w.u8((uint8_t)Msg::WorldCoarse);
        w.u32(tick);
        w.varu32((uint32_t)entries.size());
        int32_t prev = -1;
        for (const CoarseWire& e : entries)
        {
            if (e.id >= kMaxPlayers || (int32_t)e.id <= prev) return {};
            w.varu32((uint32_t)((int32_t)e.id - prev - 1));
            w.u16(e.q.x);
            w.u16(e.q.y);
            w.u16(e.q.z);
            prev = e.id;
        }
        return w.take();
    }

    Bytes Encode(const CoarseState& m)
    {
        std::vector<CoarseWire> v;
        v.reserve(m.players.size());
        for (const CoarseEntry& e : m.players)
        {
            if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z)) return {};
            Kinematics k;
            k.x = e.x;
            k.y = e.y;
            k.z = e.z;
            v.push_back(CoarseWire{e.id, QuantizeCoarse(k)});
        }
        return EncodeCoarse(m.tick, v);
    }

    bool Decode(const uint8_t* p, size_t n, CoarseState& out)
    {
        wire::Reader r(p, n);
        if (r.u8() != (uint8_t)Msg::WorldCoarse || !r.ok()) return false;
        CoarseState m;
        m.tick = r.u32();
        const uint32_t count = r.varu32();
        if (!r.ok() || count > kMaxPlayers || r.remaining() < (size_t)count * (1 + kCoarsePayload)) return false;
        m.players.reserve(count);
        int64_t prev = -1;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t gap = r.varu32();
            QCoarse q;
            q.x = r.u16();
            q.y = r.u16();
            q.z = r.u16();
            if (!r.ok()) return false;
            const int64_t id = prev + 1 + (int64_t)gap;
            if (id >= kMaxPlayers) return false;
            prev = id;
            CoarseEntry e;
            e.id = (uint16_t)id;
            DequantizeCoarse(q, e.x, e.y, e.z);
            m.players.push_back(e);
        }
        if (!r.done()) return false;
        out = std::move(m);
        return true;
    }

}
