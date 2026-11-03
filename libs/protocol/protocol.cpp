// GAGE protocol v5 codec and validators (the WorldState / WorldCoarse codec is worldcodec.cpp).
// Portable C++17, no Windows headers: the same file builds into the server, the client engine
// and the tools.
#include "protocol.h"

#include "qwire.h"
#include "wire.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gage::proto {

    namespace {

        // Decode helpers: every Decode starts with the type byte and ends with
        // done() (all bytes consumed), and only touches `out` on success.
        bool Begin(wire::Reader& r, Msg expected)
        {
            return r.u8() == (uint8_t)expected && r.ok();
        }

        bool Finite(float v) { return std::isfinite(v); }

        bool AllFinite(const Kinematics& k)
        {
            return Finite(k.x) && Finite(k.y) && Finite(k.z) && Finite(k.heading) && Finite(k.vx) && Finite(k.vy) &&
                   Finite(k.vz);
        }

        // A PedState Encode can frame: move fits 3 bits, and a finite aim while aiming.
        bool PedEncodable(const PedState& s)
        {
            if (s.move > 7) return false;
            if ((s.flags & kPfAiming) && !(Finite(s.aimX) && Finite(s.aimY) && Finite(s.aimZ))) return false;
            return true;
        }

        // Look body: u32 model, 12 x (u16 drawable, u8 texture, u8 palette), 8 x (i16 drawable,
        // u8 texture), 9 x u8 head blend. 85 bytes.
        void PutLook(wire::Writer& w, const Look& l)
        {
            w.u32(l.model);
            for (const LookComponent& c : l.components)
            {
                w.u16(c.drawable);
                w.u8(c.texture);
                w.u8(c.palette);
            }
            for (const LookProp& p : l.props)
            {
                w.i16(p.drawable);
                w.u8(p.texture);
            }
            const HeadBlend& h = l.head;
            const uint8_t hb[9] = {h.shape1, h.shape2, h.shape3, h.skin1, h.skin2, h.skin3, h.shapeMix, h.skinMix, h.thirdMix};
            w.bytes(hb, sizeof(hb));
        }

        void GetLook(wire::Reader& r, Look& l)
        {
            l.model = r.u32();
            for (LookComponent& c : l.components)
            {
                c.drawable = r.u16();
                c.texture = r.u8();
                c.palette = r.u8();
            }
            for (LookProp& p : l.props)
            {
                p.drawable = r.i16();
                p.texture = r.u8();
            }
            HeadBlend& h = l.head;
            uint8_t* const hb[9] = {&h.shape1, &h.shape2, &h.shape3, &h.skin1, &h.skin2, &h.skin3, &h.shapeMix, &h.skinMix, &h.thirdMix};
            for (uint8_t* b : hb) *b = r.u8();
        }

        // ---- v4 vehicle bodies ----

        bool QuatFinite(const Quat& q) { return Finite(q.x) && Finite(q.y) && Finite(q.z) && Finite(q.w); }

        // The moving part of a vehicle, shared by VehicleWorld full entries and VehicleUpdate (22 bytes):
        //   u64 position (cm, 22/22/20 bits), i16 vx, vy, vz (1/32 m/s), u32 rotation (smallest three),
        //   i8 angular velocity x, y, z (1/16 rad/s), i8 steering (1/127)
        bool MotionEncodable(const Kinematics& k, const Quat& rot, float avx, float avy, float avz, float steer)
        {
            return Finite(k.x) && Finite(k.y) && Finite(k.z) && Finite(k.vx) && Finite(k.vy) && Finite(k.vz) &&
                   QuatFinite(rot) && Finite(avx) && Finite(avy) && Finite(avz) && Finite(steer);
        }

        void PutMotion(wire::Writer& w, const Kinematics& k, const Quat& rot, float avx, float avy, float avz, float steer)
        {
            const QKin q = Quantize(k);
            qwire::PutPos(w, q);
            w.i16(q.vx);
            w.i16(q.vy);
            w.i16(q.vz);
            w.u32(QuantizeQuat(rot));
            w.u8((uint8_t)QuantizeAngVel(avx));
            w.u8((uint8_t)QuantizeAngVel(avy));
            w.u8((uint8_t)QuantizeAngVel(avz));
            w.u8((uint8_t)QuantizeSteer(steer));
        }

        // False when the reader failed or a value is illegal (outside the quantiser box, an invalid
        // rotation, -128 in an i8). k.heading becomes the rotation's yaw.
        bool GetMotion(wire::Reader& r, Kinematics& k, Quat& rot, float& avx, float& avy, float& avz, float& steer)
        {
            QKin q;
            qwire::GetPos(r, q);
            q.vx = r.i16();
            q.vy = r.i16();
            q.vz = r.i16();
            const uint32_t rq = r.u32();
            const int8_t a[3] = {(int8_t)r.u8(), (int8_t)r.u8(), (int8_t)r.u8()};
            const int8_t st = (int8_t)r.u8();
            if (!r.ok() || !QInRange(q) || !QuatWireValid(rq)) return false;
            if (a[0] == -128 || a[1] == -128 || a[2] == -128 || st == -128) return false;
            k = Dequantize(q);
            rot = DequantizeQuat(rq);
            k.heading = QuatHeading(rot);
            avx = DequantizeAngVel(a[0]);
            avy = DequantizeAngVel(a[1]);
            avz = DequantizeAngVel(a[2]);
            steer = DequantizeSteer(st);
            return true;
        }

        // A full VehicleWorld entry after its id and header byte: u32 model, the motion (22), u8 flags,
        // u16 body, u16 engine, u16 fuel, u8 colour1, u8 colour2, (v5) u8 ageMs, u16 ctrl, u16 ownerId,
        // u8 flags2, u8 doors, (v6) u8 tyres, u8 tyresRim, u8 windows, u8 seat mask, u16 player id per set bit (seat
        // order). 46 bytes + 2 per occupant.
        bool VisualValid(uint8_t tyres, uint8_t rim) { return (rim & ~tyres) == 0; } // a rim bit needs its tyre bit

        void PutVehicleFull(wire::Writer& w, const VehicleState& v)
        {
            w.u32(v.model);
            PutMotion(w, v.k, v.rot, v.avx, v.avy, v.avz, v.steer);
            w.u8(v.flags);
            w.u16(v.bodyHealth);
            w.u16(v.engineHealth);
            w.u16(v.fuelPermille);
            w.u8(v.colour1);
            w.u8(v.colour2);
            w.u8(v.ageMs);
            w.u16(v.ctrl);
            w.u16(v.ownerId);
            w.u8(v.flags2);
            w.u8(v.doors);
            w.u8(v.tyres);
            w.u8(v.tyresRim);
            w.u8(v.windows);
            uint8_t mask = 0;
            for (size_t i = 0; i < kVehSeats; i++)
                if (v.seats[i] != kNoPlayer) mask |= (uint8_t)(1u << i);
            w.u8(mask);
            for (size_t i = 0; i < kVehSeats; i++)
                if (v.seats[i] != kNoPlayer) w.u16(v.seats[i]);
        }

        bool GetVehicleFull(wire::Reader& r, VehicleState& v)
        {
            v.model = r.u32();
            if (!GetMotion(r, v.k, v.rot, v.avx, v.avy, v.avz, v.steer)) return false;
            v.flags = r.u8();
            v.bodyHealth = r.u16();
            v.engineHealth = r.u16();
            v.fuelPermille = r.u16();
            v.colour1 = r.u8();
            v.colour2 = r.u8();
            v.ageMs = r.u8();
            v.ctrl = r.u16();
            v.ownerId = r.u16();
            v.flags2 = r.u8();
            v.doors = r.u8();
            v.tyres = r.u8();
            v.tyresRim = r.u8();
            v.windows = r.u8();
            const uint8_t mask = r.u8();
            for (size_t i = 0; i < kVehSeats; i++)
            {
                v.seats[i] = kNoPlayer;
                if (!(mask & (1u << i))) continue;
                v.seats[i] = r.u16();
                if (v.seats[i] >= kMaxPlayers) return false;
            }
            if (!r.ok()) return false;
            if (v.flags & ~kVehKnown) return false;
            if (v.flags2 & ~kVeh2Known || v.doors & ~kDoorsKnown || !VisualValid(v.tyres, v.tyresRim)) return false;
            if ((v.ctrl != kNoPlayer && v.ctrl >= kMaxPlayers) || (v.ownerId != kNoPlayer && v.ownerId >= kMaxPlayers)) return false;
            if (v.bodyHealth > kMaxVehicleHealth || v.engineHealth > kMaxVehicleHealth || v.fuelPermille > 1000) return false;
            return true;
        }

        // A motion-only VehicleWorld entry (v5, header 2) after its id and header byte: u8 ageMs, the motion
        // (22), u8 flags. 24 bytes; the entry is 26 with its id and header.
        void PutVehicleMotionOnly(wire::Writer& w, const VehicleState& v)
        {
            w.u8(v.ageMs);
            PutMotion(w, v.k, v.rot, v.avx, v.avy, v.avz, v.steer);
            w.u8(v.flags);
        }

        bool GetVehicleMotionOnly(wire::Reader& r, VehicleState& v)
        {
            v.ageMs = r.u8();
            if (!GetMotion(r, v.k, v.rot, v.avx, v.avy, v.avz, v.steer)) return false;
            v.flags = r.u8();
            if (!r.ok() || (v.flags & ~kVehKnown)) return false;
            v.motionOnly = true;
            return true;
        }

        // ---- v4 animation body: str dict, str name, f32 blendIn, f32 blendOut, i32 durationMs, u32 flags, u8 rate ----
        bool PutAnim(wire::Writer& w, const Anim& a)
        {
            if (!Finite(a.blendIn) || !Finite(a.blendOut)) return false;
            if (!w.str(a.dict) || !w.str(a.name)) return false;
            w.f32(a.blendIn);
            w.f32(a.blendOut);
            w.u32((uint32_t)a.durationMs);
            w.u32(a.flags);
            w.u8(a.playbackRate);
            return true;
        }

        bool GetAnim(wire::Reader& r, Anim& a)
        {
            a.dict = r.str(kMaxAnimName);
            a.name = r.str(kMaxAnimName);
            a.blendIn = r.f32();
            a.blendOut = r.f32();
            a.durationMs = (int32_t)r.u32();
            a.flags = r.u32();
            a.playbackRate = r.u8();
            return r.ok() && Finite(a.blendIn) && Finite(a.blendOut);
        }

        bool PlayerIdOrNone(uint16_t id) { return id < kMaxPlayers || id == kNoPlayer; }

        bool KnownRefuse(uint8_t v)
        {
            return v >= (uint8_t)RefuseReason::VersionMismatch && v <= (uint8_t)RefuseReason::TooManyConnections;
        }
        bool KnownLeave(uint8_t v) { return v >= (uint8_t)LeaveReason::Quit && v <= (uint8_t)LeaveReason::Kicked; }

        // Length of the UTF-8 sequence starting at s[i], or 0 when it is not
        // well-formed (bad lead byte, truncated, bad continuation, overlong,
        // surrogate, above U+10FFFF). `cp` receives the code point.
        size_t Utf8Seq(const std::string& s, size_t i, uint32_t& cp)
        {
            const uint8_t b0 = (uint8_t)s[i];
            size_t len;
            uint32_t min;
            if (b0 < 0x80) { cp = b0; return 1; }
            if (b0 >= 0xC2 && b0 <= 0xDF) { len = 2; cp = b0 & 0x1F; min = 0x80; }
            else if (b0 >= 0xE0 && b0 <= 0xEF) { len = 3; cp = b0 & 0x0F; min = 0x800; }
            else if (b0 >= 0xF0 && b0 <= 0xF4) { len = 4; cp = b0 & 0x07; min = 0x10000; }
            else return 0; // continuation byte, C0/C1 (always overlong), F5..FF
            if (s.size() - i < len) return 0;
            for (size_t k = 1; k < len; k++)
            {
                const uint8_t b = (uint8_t)s[i + k];
                if ((b & 0xC0) != 0x80) return 0;
                cp = (cp << 6) | (b & 0x3F);
            }
            if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
            return len;
        }

    }

    // ---- encode ----------------------------------------------------------------

    Bytes Encode(const Hello& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::Hello);
        w.u16(m.version);
        if (!w.str(m.name)) return {};
        return w.take();
    }

    Bytes Encode(const Welcome& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::Welcome);
        w.u16(m.yourId);
        w.u16(m.tickHz);
        if (!w.str(m.serverName)) return {};
        w.u8(m.playerHealth);
        return w.take();
    }

    Bytes Encode(const Refuse& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::Refuse);
        w.u8((uint8_t)m.reason);
        if (!w.str(m.text)) return {};
        return w.take();
    }

    Bytes Encode(const PlayerJoined& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::PlayerJoined);
        w.u16(m.id);
        if (!w.str(m.name)) return {};
        return w.take();
    }

    Bytes Encode(const PlayerLeft& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::PlayerLeft);
        w.u16(m.id);
        w.u8((uint8_t)m.reason);
        return w.take();
    }

    Bytes Encode(const PlayerState& m)
    {
        if (!AllFinite(m.k) || !PedEncodable(m.ped)) return {}; // a NaN/Inf has no quantised form
        wire::Writer w;
        w.u8((uint8_t)Msg::PlayerState);
        w.u32(m.seq);
        w.u32(m.ack.tick);
        w.u32(m.ack.bits);
        w.u16(m.sampleMs);
        qwire::PutAbsolute(w, Quantize(m.k));
        qwire::PutPed(w, QuantizePed(m.ped));
        return w.take();
    }

    Bytes Encode(const ChatSend& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::ChatSend);
        if (!w.str(m.text)) return {};
        return w.take();
    }

    Bytes Encode(const ChatMessage& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::ChatMessage);
        w.u16(m.fromId);
        if (!w.str(m.text)) return {};
        return w.take();
    }

    Bytes Encode(const ResourceManifest& m)
    {
        if (m.resources.size() > 0xFF) return {}; // the u8 count cannot frame more
        wire::Writer w;
        w.u8((uint8_t)Msg::ResourceManifest);
        w.u8((uint8_t)m.resources.size());
        for (const ResourceInfo& r : m.resources)
        {
            if (!w.str(r.name)) return {};
            w.u32(r.size);
            w.u32(r.crc32);
        }
        return w.take();
    }

    Bytes Encode(const ResourceRequest& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::ResourceRequest);
        w.u8(m.index);
        w.u32(m.startChunk);
        return w.take();
    }

    Bytes Encode(const ResourceChunk& m)
    {
        if (m.data.size() > 0xFFFF) return {}; // the u16 length cannot frame more
        wire::Writer w;
        w.u8((uint8_t)Msg::ResourceChunk);
        w.u8(m.index);
        w.u32(m.chunkIndex);
        w.u16((uint16_t)m.data.size());
        w.bytes(m.data.data(), m.data.size());
        return w.take();
    }

    Bytes Encode(const VehicleWorld& m)
    {
        if (m.removed.size() > 0xFF || m.present.size() > kMaxVehicles) return {}; // the u8 / u16 counts
        for (size_t i = 0; i < m.removed.size(); i++)
            if (m.removed[i] >= kMaxVehicles || (i > 0 && !(m.removed[i] > m.removed[i - 1]))) return {};
        for (size_t i = 0; i < m.present.size(); i++)
        {
            if (m.present[i] >= kMaxVehicles || (i > 0 && !(m.present[i] > m.present[i - 1]))) return {};
            if (std::binary_search(m.removed.begin(), m.removed.end(), m.present[i])) return {}; // destroyed AND present
        }
        for (const VehicleState& v : m.vehicles)
            if (!MotionEncodable(v.k, v.rot, v.avx, v.avy, v.avz, v.steer) || (!v.motionOnly && !VisualValid(v.tyres, v.tyresRim))) return {};
        wire::Writer w;
        w.u8((uint8_t)Msg::VehicleWorld);
        w.u32(m.tick);
        w.u8((uint8_t)m.removed.size());
        for (uint16_t id : m.removed) w.u8((uint8_t)id);
        w.u16((uint16_t)m.present.size());
        size_t j = 0; // next full state; both lists ascend
        for (uint16_t id : m.present)
        {
            const bool listed = j < m.vehicles.size() && m.vehicles[j].id == id;
            w.u8((uint8_t)id);
            // Header: 0 presence only, 1 full state, 2 motion-only state (v5).
            w.u8(!listed ? 0 : m.vehicles[j].motionOnly ? 2 : 1);
            if (listed)
            {
                if (m.vehicles[j].motionOnly)
                    PutVehicleMotionOnly(w, m.vehicles[j]);
                else
                    PutVehicleFull(w, m.vehicles[j]);
                j++;
            }
        }
        if (j != m.vehicles.size()) return {}; // a full state whose id is not present (or out of order)
        return w.take();
    }

    Bytes Encode(const VehicleCmd& m)
    {
        const bool reg = m.op == VehicleOp::Register;
        if (reg && !(Finite(m.x) && Finite(m.y) && Finite(m.z) && QuatFinite(m.rot))) return {};
        wire::Writer w;
        w.u8((uint8_t)Msg::VehicleCmd);
        w.u16(m.vehId);
        w.u8((uint8_t)m.op);
        w.u32(m.arg);
        if (reg)
        {
            Kinematics k;
            k.x = m.x;
            k.y = m.y;
            k.z = m.z;
            qwire::PutPos(w, Quantize(k));
            w.u32(QuantizeQuat(m.rot));
            w.u8(m.colour1);
            w.u8(m.colour2);
            w.u8(m.seat);
        }
        return w.take();
    }

    Bytes Encode(const VehicleUpdate& m)
    {
        if (m.vehId >= kMaxVehicles || !MotionEncodable(m.k, m.rot, m.avx, m.avy, m.avz, m.steer) || !VisualValid(m.tyres, m.tyresRim)) return {};
        wire::Writer w;
        w.u8((uint8_t)Msg::VehicleUpdate);
        w.u32(m.seq);
        w.u8((uint8_t)m.vehId);
        PutMotion(w, m.k, m.rot, m.avx, m.avy, m.avz, m.steer);
        w.u8(m.flags);
        w.u16(m.bodyHealth);
        w.u16(m.engineHealth);
        w.u16(m.sampleMs);
        w.u8(m.flags2);
        w.u8(m.doors);
        w.u8(m.tyres);
        w.u8(m.tyresRim);
        w.u8(m.windows);
        return w.take();
    }

    Bytes Encode(const VehicleResult& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::VehicleResult);
        w.u8((uint8_t)m.op);
        w.u8((uint8_t)m.code);
        w.u16(m.vehId);
        return w.take();
    }

    Bytes Encode(const Hit& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::Hit);
        w.u8((uint8_t)m.kind);
        w.u16(m.targetId);
        w.u32(m.weapon);
        w.u16(m.damage);
        w.i16(m.bone);
        w.u8(m.shotSeq);
        return w.take();
    }

    Bytes Encode(const Damage& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::Damage);
        w.u16(m.victimId);
        w.u16(m.attackerId);
        w.u32(m.weapon);
        w.u16(m.damage);
        w.u8(m.health);
        w.u8(m.armor);
        w.u8(m.killed ? 1 : 0);
        w.i16(m.bone);
        w.u8(m.flags);
        return w.take();
    }

    namespace
    {
        int16_t SatI16(double v)
        {
            const double r = std::floor(v + 0.5);
            return (int16_t)(r > 32767.0 ? 32767 : (r < -32767.0 ? -32767 : r));
        }
        int8_t SatI8Unit(double v)
        {
            const double r = std::floor(v * 127.0 + 0.5);
            return (int8_t)(r > 127.0 ? 127 : (r < -127.0 ? -127 : r));
        }
        uint8_t SatU8(double v)
        {
            const double r = std::floor(v + 0.5);
            return (uint8_t)(r > 255.0 ? 255 : (r < 0.0 ? 0 : r));
        }
    }

    Bytes Encode(const VehicleImpact& m)
    {
        if (m.vehId >= kMaxVehicles || m.otherId >= kMaxVehicles || !Finite(m.dvx) || !Finite(m.dvy) || !Finite(m.dyaw) || !Finite(m.nx) ||
            !Finite(m.ny) || !Finite(m.closing) || !Finite(m.depth))
            return {};
        wire::Writer w;
        w.u8((uint8_t)Msg::VehicleImpact);
        w.u32(m.seq);
        w.u16(m.vehId);
        w.u16(m.otherId);
        w.u16(m.sampleMs);
        w.i16(SatI16((double)m.dvx * 64.0));
        w.i16(SatI16((double)m.dvy * 64.0));
        w.i16(SatI16((double)m.dyaw * 256.0));
        w.u8((uint8_t)SatI8Unit(m.nx));
        w.u8((uint8_t)SatI8Unit(m.ny));
        w.u8(SatU8((double)m.closing * 4.0));
        w.u8(SatU8((double)m.depth * 100.0));
        return w.take();
    }

    Bytes Encode(const Killed& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::Killed);
        w.u16(m.victimId);
        w.u16(m.killerId);
        w.u32(m.weapon);
        w.u8(m.flags);
        return w.take();
    }

    Bytes Encode(const Respawn&)
    {
        return Bytes{(uint8_t)Msg::Respawn};
    }

    Bytes Encode(const RespawnOk& m)
    {
        if (!(Finite(m.x) && Finite(m.y) && Finite(m.z) && Finite(m.heading))) return {};
        wire::Writer w;
        w.u8((uint8_t)Msg::RespawnOk);
        w.f32(m.x);
        w.f32(m.y);
        w.f32(m.z);
        w.f32(m.heading);
        w.u8(m.health);
        w.u8(m.armor);
        return w.take();
    }

    Bytes Encode(const PlayAnim& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::PlayAnim);
        if (!PutAnim(w, m.anim)) return {};
        return w.take();
    }

    Bytes Encode(const PlayScenario& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::PlayScenario);
        if (!w.str(m.name)) return {};
        return w.take();
    }

    Bytes Encode(const StopAnim&)
    {
        return Bytes{(uint8_t)Msg::StopAnim};
    }

    Bytes Encode(const PeerAnim& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::PeerAnim);
        w.u16(m.id);
        if (!PutAnim(w, m.anim)) return {};
        return w.take();
    }

    Bytes Encode(const PeerScenario& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::PeerScenario);
        w.u16(m.id);
        if (!w.str(m.name)) return {};
        return w.take();
    }

    Bytes Encode(const PeerAnimStop& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::PeerAnimStop);
        w.u16(m.id);
        return w.take();
    }

    namespace {
        // The one place that says which SeatNotice fields are valid (Encode refuses, Decode refuses).
        bool SeatNoticeValid(const SeatNotice& m)
        {
            const uint8_t kind = (uint8_t)m.kind;
            if (kind < (uint8_t)SeatNoticeKind::Reserved || kind > (uint8_t)SeatNoticeKind::Exited) return false;
            if (m.player >= kMaxPlayers || m.vehId >= kMaxVehicles || m.seat >= kVehSeats) return false;
            switch (m.kind)
            {
            case SeatNoticeKind::Reserved:  return (m.detail & ~kSeatNoticeRun) == 0;
            case SeatNoticeKind::Cancelled: return m.detail <= kSeatCancelLast;
            case SeatNoticeKind::Seated:
            case SeatNoticeKind::Exited:    return m.detail == 0;
            }
            return false;
        }
    }

    Bytes Encode(const SeatNotice& m)
    {
        if (!SeatNoticeValid(m)) return {};
        wire::Writer w;
        w.u8((uint8_t)Msg::SeatNotice);
        w.u8((uint8_t)m.kind);
        w.u16(m.player);
        w.u16(m.vehId);
        w.u8(m.seat);
        w.u8(m.detail);
        return w.take();
    }

    Bytes Encode(const PlayerLook& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::PlayerLook);
        PutLook(w, m.look);
        return w.take();
    }

    Bytes Encode(const PeerLook& m)
    {
        wire::Writer w;
        w.u8((uint8_t)Msg::PeerLook);
        w.u16(m.id);
        PutLook(w, m.look);
        return w.take();
    }

    // ---- decode ----------------------------------------------------------------

    bool PeekType(const uint8_t* p, size_t n, Msg& out)
    {
        if (!p || n < 1 || p[0] < (uint8_t)Msg::Hello || p[0] > kMsgLast)
            return false;
        out = (Msg)p[0];
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, Hello& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::Hello)) return false;
        Hello m;
        m.version = r.u16();
        m.name = r.str(kMaxName);
        if (!r.done()) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, Welcome& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::Welcome)) return false;
        Welcome m;
        m.yourId = r.u16();
        m.tickHz = r.u16();
        m.serverName = r.str(kMaxServerName);
        m.playerHealth = r.u8();
        if (!r.done() || m.playerHealth == 0) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, Refuse& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::Refuse)) return false;
        Refuse m;
        const uint8_t reason = r.u8();
        m.text = r.str(kMaxRefuseText);
        if (!r.done() || !KnownRefuse(reason)) return false;
        m.reason = (RefuseReason)reason;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PlayerJoined& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PlayerJoined)) return false;
        PlayerJoined m;
        m.id = r.u16();
        m.name = r.str(kMaxName);
        if (!r.done()) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PlayerLeft& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PlayerLeft)) return false;
        PlayerLeft m;
        m.id = r.u16();
        const uint8_t reason = r.u8();
        if (!r.done() || !KnownLeave(reason)) return false;
        m.reason = (LeaveReason)reason;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PlayerState& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PlayerState)) return false;
        PlayerState m;
        m.seq = r.u32();
        m.ack.tick = r.u32();
        m.ack.bits = r.u32();
        m.sampleMs = r.u16();
        QKin q;
        const bool inRange = qwire::GetAbsolute(r, q);
        QPed qp;
        const bool pedOk = qwire::GetPed(r, qp); // the aim is only read when the aiming flag says it is there
        if (!r.done() || !inRange || !pedOk) return false;
        m.k = Dequantize(q);
        m.ped = DequantizePed(qp);
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, ChatSend& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::ChatSend)) return false;
        ChatSend m;
        m.text = r.str(kMaxChat);
        if (!r.done()) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, ChatMessage& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::ChatMessage)) return false;
        ChatMessage m;
        m.fromId = r.u16();
        m.text = r.str(kMaxChat);
        if (!r.done()) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, ResourceManifest& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::ResourceManifest)) return false;
        const uint8_t count = r.u8();
        if (!r.ok() || count > kMaxResources) return false;
        ResourceManifest m;
        m.resources.reserve(count);
        for (uint8_t i = 0; i < count; i++)
        {
            ResourceInfo info;
            info.name = r.str(kMaxResourceName);
            info.size = r.u32();
            info.crc32 = r.u32();
            if (!r.ok() || info.size > kMaxResourceBytes) return false;
            m.resources.push_back(std::move(info));
        }
        if (!r.done()) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, ResourceRequest& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::ResourceRequest)) return false;
        ResourceRequest m;
        m.index = r.u8();
        m.startChunk = r.u32();
        if (!r.done()) return false;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, ResourceChunk& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::ResourceChunk)) return false;
        ResourceChunk m;
        m.index = r.u8();
        m.chunkIndex = r.u32();
        const uint16_t len = r.u16();
        if (!r.ok() || len == 0 || len > kResourceChunk) return false;
        m.data.resize(len);
        if (!r.bytes(m.data.data(), len) || !r.done()) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, VehicleWorld& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::VehicleWorld)) return false;
        VehicleWorld m;
        m.tick = r.u32();
        const uint8_t removedCount = r.u8();
        if (!r.ok() || r.remaining() < removedCount) return false;
        m.removed.reserve(removedCount);
        for (uint8_t i = 0; i < removedCount; i++)
        {
            const uint16_t id = r.u8();
            if (i > 0 && !(id > m.removed.back())) return false; // strictly ascending
            m.removed.push_back(id);
        }
        const uint16_t count = r.u16();
        // Refuse a hostile count before reserving anything for it (an entry is at least 2 bytes).
        if (!r.ok() || count > kMaxVehicles || r.remaining() < (size_t)count * 2) return false;
        m.present.reserve(count);
        for (uint16_t i = 0; i < count; i++)
        {
            const uint16_t id = r.u8();
            const uint8_t hdr = r.u8();
            if (!r.ok() || hdr > 2) return false;                               // 0 presence, 1 full, 2 motion-only (v5)
            if (i > 0 && !(id > m.present.back())) return false;               // strictly ascending
            if (std::binary_search(m.removed.begin(), m.removed.end(), id)) return false; // destroyed AND present
            m.present.push_back(id);
            if (hdr != 0)
            {
                VehicleState v;
                v.id = id;
                if (!(hdr == 1 ? GetVehicleFull(r, v) : GetVehicleMotionOnly(r, v))) return false;
                m.vehicles.push_back(v);
            }
        }
        if (!r.done()) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, VehicleCmd& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::VehicleCmd)) return false;
        VehicleCmd m;
        m.vehId = r.u16();
        const uint8_t op = r.u8();
        m.arg = r.u32();
        if (!r.ok() || op < (uint8_t)VehicleOp::Spawn || op > kVehicleOpLast) return false;
        m.op = (VehicleOp)op;
        if (m.op == VehicleOp::Register)
        {
            QKin q;
            qwire::GetPos(r, q);
            const uint32_t rq = r.u32();
            m.colour1 = r.u8();
            m.colour2 = r.u8();
            m.seat = r.u8();
            if (!r.ok() || !QInRange(q) || !QuatWireValid(rq)) return false;
            const Kinematics k = Dequantize(q);
            m.x = k.x;
            m.y = k.y;
            m.z = k.z;
            m.rot = DequantizeQuat(rq);
        }
        if (!r.done()) return false; // a Register without its tail, or another op with one
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, VehicleUpdate& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::VehicleUpdate)) return false;
        VehicleUpdate m;
        m.seq = r.u32();
        m.vehId = r.u8();
        if (!GetMotion(r, m.k, m.rot, m.avx, m.avy, m.avz, m.steer)) return false;
        m.flags = r.u8();
        m.bodyHealth = r.u16();
        m.engineHealth = r.u16();
        m.sampleMs = r.u16();
        m.flags2 = r.u8();
        m.doors = r.u8();
        m.tyres = r.u8();
        m.tyresRim = r.u8();
        m.windows = r.u8();
        if (!r.done() || (m.flags & ~kVehKnown) || (m.flags2 & ~kVeh2Known) || (m.doors & ~kDoorsKnown) || !VisualValid(m.tyres, m.tyresRim) ||
            m.bodyHealth > kMaxVehicleHealth || m.engineHealth > kMaxVehicleHealth)
            return false;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, VehicleResult& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::VehicleResult)) return false;
        const uint8_t op = r.u8();
        const uint8_t code = r.u8();
        VehicleResult m;
        m.vehId = r.u16();
        if (!r.done() || op < (uint8_t)VehicleOp::Spawn || op > kVehicleOpLast || code > kVehicleResultLast) return false;
        m.op = (VehicleOp)op;
        m.code = (VehicleResultCode)code;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, Hit& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::Hit)) return false;
        const uint8_t kind = r.u8();
        Hit m;
        m.targetId = r.u16();
        m.weapon = r.u32();
        m.damage = r.u16();
        m.bone = r.i16();
        m.shotSeq = r.u8();
        if (!r.done() || kind < (uint8_t)HitKind::Player || kind > (uint8_t)HitKind::Vehicle) return false;
        m.kind = (HitKind)kind;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, Damage& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::Damage)) return false;
        Damage m;
        m.victimId = r.u16();
        m.attackerId = r.u16();
        m.weapon = r.u32();
        m.damage = r.u16();
        m.health = r.u8();
        m.armor = r.u8();
        const uint8_t killed = r.u8();
        m.bone = r.i16();
        m.flags = r.u8();
        if (!r.done() || killed > 1 || (m.flags & ~kDmgKnown) || m.victimId >= kMaxPlayers || !PlayerIdOrNone(m.attackerId))
            return false;
        m.killed = killed == 1;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, VehicleImpact& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::VehicleImpact)) return false;
        VehicleImpact m;
        m.seq = r.u32();
        m.vehId = r.u16();
        m.otherId = r.u16();
        m.sampleMs = r.u16();
        const int16_t dvx = r.i16(), dvy = r.i16(), dyaw = r.i16();
        const int8_t nx = (int8_t)r.u8(), ny = (int8_t)r.u8();
        const uint8_t closing = r.u8(), depth = r.u8();
        if (!r.done() || m.vehId >= kMaxVehicles || m.otherId >= kMaxVehicles || nx == -128 || ny == -128 || dvx == -32768 || dvy == -32768 ||
            dyaw == -32768)
            return false;
        m.dvx = (float)dvx / 64.0f;
        m.dvy = (float)dvy / 64.0f;
        m.dyaw = (float)dyaw / 256.0f;
        m.nx = (float)nx / 127.0f;
        m.ny = (float)ny / 127.0f;
        m.closing = (float)closing / 4.0f;
        m.depth = (float)depth / 100.0f;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, Killed& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::Killed)) return false;
        Killed m;
        m.victimId = r.u16();
        m.killerId = r.u16();
        m.weapon = r.u32();
        m.flags = r.u8();
        if (!r.done() || (m.flags & ~kKilledKnown) || m.victimId >= kMaxPlayers || !PlayerIdOrNone(m.killerId)) return false;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, Respawn& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::Respawn) || !r.done()) return false;
        out = Respawn();
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, RespawnOk& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::RespawnOk)) return false;
        RespawnOk m;
        m.x = r.f32();
        m.y = r.f32();
        m.z = r.f32();
        m.heading = r.f32();
        m.health = r.u8();
        m.armor = r.u8();
        if (!r.done() || !(Finite(m.x) && Finite(m.y) && Finite(m.z) && Finite(m.heading))) return false;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PlayAnim& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PlayAnim)) return false;
        PlayAnim m;
        if (!GetAnim(r, m.anim) || !r.done()) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PlayScenario& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PlayScenario)) return false;
        PlayScenario m;
        m.name = r.str(kMaxAnimName);
        if (!r.done()) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, StopAnim& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::StopAnim) || !r.done()) return false;
        out = StopAnim();
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PeerAnim& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PeerAnim)) return false;
        PeerAnim m;
        m.id = r.u16();
        if (!GetAnim(r, m.anim) || !r.done() || m.id >= kMaxPlayers) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PeerScenario& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PeerScenario)) return false;
        PeerScenario m;
        m.id = r.u16();
        m.name = r.str(kMaxAnimName);
        if (!r.done() || m.id >= kMaxPlayers) return false;
        out = std::move(m);
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PeerAnimStop& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PeerAnimStop)) return false;
        PeerAnimStop m;
        m.id = r.u16();
        if (!r.done() || m.id >= kMaxPlayers) return false;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, SeatNotice& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::SeatNotice)) return false;
        SeatNotice m;
        m.kind = (SeatNoticeKind)r.u8();
        m.player = r.u16();
        m.vehId = r.u16();
        m.seat = r.u8();
        m.detail = r.u8();
        if (!r.done() || !SeatNoticeValid(m)) return false;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PlayerLook& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PlayerLook)) return false;
        PlayerLook m;
        GetLook(r, m.look);
        if (!r.done()) return false;
        out = m;
        return true;
    }

    bool Decode(const uint8_t* p, size_t n, PeerLook& out)
    {
        wire::Reader r(p, n);
        if (!Begin(r, Msg::PeerLook)) return false;
        PeerLook m;
        m.id = r.u16();
        GetLook(r, m.look);
        if (!r.done() || m.id >= kMaxPlayers) return false;
        out = m;
        return true;
    }

    bool operator==(const Look& a, const Look& b)
    {
        if (a.model != b.model) return false;
        for (size_t i = 0; i < kLookComponents; i++)
        {
            const LookComponent &x = a.components[i], &y = b.components[i];
            if (x.drawable != y.drawable || x.texture != y.texture || x.palette != y.palette) return false;
        }
        for (size_t i = 0; i < kLookProps; i++)
            if (a.props[i].drawable != b.props[i].drawable || a.props[i].texture != b.props[i].texture) return false;
        const HeadBlend &h = a.head, &g = b.head;
        return h.shape1 == g.shape1 && h.shape2 == g.shape2 && h.shape3 == g.shape3 && h.skin1 == g.skin1 &&
               h.skin2 == g.skin2 && h.skin3 == g.skin3 && h.shapeMix == g.shapeMix && h.skinMix == g.skinMix &&
               h.thirdMix == g.thirdMix;
    }

    // ---- validators ------------------------------------------------------------

    bool ValidName(const std::string& name)
    {
        if (name.empty() || name.size() > kMaxName) return false;
        for (size_t i = 0; i < name.size(); i++)
        {
            const char c = name[i];
            if (c == ' ')
            {
                if (i == 0 || i + 1 == name.size() || name[i - 1] == ' ') return false;
                continue;
            }
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
            if (!ok) return false;
        }
        return true;
    }

    bool ValidKinematics(const Kinematics& k)
    {
        const float all[] = { k.x, k.y, k.z, k.heading, k.vx, k.vy, k.vz };
        for (float v : all)
            if (!Finite(v)) return false;
        if (k.x < kWorldMinXY || k.x > kWorldMaxXY) return false;
        if (k.y < kWorldMinXY || k.y > kWorldMaxXY) return false;
        if (k.z < kWorldMinZ || k.z > kWorldMaxZ) return false;
        if (k.heading < -360.0f || k.heading > 360.0f) return false;
        if (std::fabs(k.vx) > kMaxSpeed || std::fabs(k.vy) > kMaxSpeed || std::fabs(k.vz) > kMaxSpeed)
            return false;
        return true;
    }

    bool ValidPedState(const PedState& s)
    {
        if (s.move > kMoveMax || (s.flags & ~kPfKnown) != 0) return false;
        if ((s.flags & kPfDriver) && s.move < kMoveInVehicle) return false; // a driver is in (or getting in/out of) a vehicle
        if ((s.flags & kPfAiming) && !(Finite(s.aimX) && Finite(s.aimY) && Finite(s.aimZ))) return false;
        return true;
    }

    bool ValidLook(const Look& l)
    {
        if (l.model == 0) return false;
        for (const LookComponent& c : l.components)
            if (c.drawable > kMaxLookDrawable || c.texture > kMaxLookTexture || c.palette > kMaxLookPalette) return false;
        for (const LookProp& p : l.props)
        {
            if (p.drawable < -1 || p.drawable > (int)kMaxLookDrawable || p.texture > kMaxLookTexture) return false;
            if (p.drawable == -1 && p.texture != 0) return false; // "no prop" has one spelling
        }
        const HeadBlend& h = l.head;
        for (uint8_t id : {h.shape1, h.shape2, h.shape3, h.skin1, h.skin2, h.skin3})
            if (id > kMaxHeadBlendId) return false;
        return true;
    }

    bool ValidChat(const std::string& text)
    {
        if (text.empty() || text.size() > kMaxChat) return false;
        size_t i = 0;
        while (i < text.size())
        {
            uint32_t cp = 0;
            const size_t len = Utf8Seq(text, i, cp);
            if (len == 0) return false;
            if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) return false;
            i += len;
        }
        return true;
    }

    bool ValidResourceName(const std::string& name)
    {
        if (name.empty() || name.size() > kMaxResourceName) return false;
        if (name.front() == '.' || name.back() == '.') return false; // no hidden names, no trailing dot
        for (size_t i = 0; i < name.size(); i++)
        {
            const char c = name[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if (!ok) return false;                            // excludes '/' and '\\', so no traversal
            if (c == '.' && name[i + 1] == '.') return false; // no ".."; back() != '.' so i+1 is in range
        }
        return true;
    }

    bool ValidManifest(const ResourceManifest& m)
    {
        if (m.resources.size() > kMaxResources) return false;
        uint64_t total = 0;
        for (size_t i = 0; i < m.resources.size(); i++)
        {
            const ResourceInfo& r = m.resources[i];
            if (!ValidResourceName(r.name) || r.size > kMaxResourceBytes) return false;
            const std::string key = NameKey(r.name);
            for (size_t j = 0; j < i; j++)
                if (NameKey(m.resources[j].name) == key) return false; // duplicate name (case-insensitive)
            total += r.size;
        }
        return total <= kMaxResourceTotal;
    }

    bool ValidVehicleUpdate(const VehicleUpdate& u)
    {
        Kinematics k = u.k;
        k.heading = 0; // not part of a vehicle's wire form
        if (!ValidKinematics(k)) return false;
        if (!QuatFinite(u.rot)) return false;
        const double len2 = (double)u.rot.x * u.rot.x + (double)u.rot.y * u.rot.y + (double)u.rot.z * u.rot.z + (double)u.rot.w * u.rot.w;
        if (!(len2 > 1e-6)) return false;
        if (!(Finite(u.avx) && Finite(u.avy) && Finite(u.avz) && Finite(u.steer))) return false;
        if (u.steer < -1.0f || u.steer > 1.0f) return false;
        return u.bodyHealth <= kMaxVehicleHealth && u.engineHealth <= kMaxVehicleHealth && (u.flags & ~kVehKnown) == 0 &&
               (u.flags2 & ~kVeh2Known) == 0 && (u.doors & ~kDoorsKnown) == 0 && VisualValid(u.tyres, u.tyresRim);
    }

    bool ValidAnimName(const std::string& name)
    {
        if (name.empty() || name.size() > kMaxAnimName) return false;
        for (char c : name)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                            c == '@' || c == '/' || c == '.' || c == '-';
            if (!ok) return false;
        }
        return true;
    }

    bool ValidAnim(const Anim& a)
    {
        if (!ValidAnimName(a.dict) || !ValidAnimName(a.name)) return false;
        if (!Finite(a.blendIn) || !Finite(a.blendOut) || std::fabs(a.blendIn) > 1000.0f || std::fabs(a.blendOut) > 1000.0f)
            return false;
        return a.durationMs >= -1;
    }

    std::string NameKey(const std::string& name)
    {
        std::string key = name;
        for (char& c : key)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return key;
    }

    // ---- names -----------------------------------------------------------------

    const char* ToString(Msg m)
    {
        switch (m)
        {
        case Msg::Hello:        return "Hello";
        case Msg::Welcome:      return "Welcome";
        case Msg::Refuse:       return "Refuse";
        case Msg::PlayerJoined: return "PlayerJoined";
        case Msg::PlayerLeft:   return "PlayerLeft";
        case Msg::PlayerState:  return "PlayerState";
        case Msg::WorldState:   return "WorldState";
        case Msg::ChatSend:     return "ChatSend";
        case Msg::ChatMessage:  return "ChatMessage";
        case Msg::WorldCoarse:  return "WorldCoarse";
        case Msg::ResourceManifest: return "ResourceManifest";
        case Msg::ResourceRequest:  return "ResourceRequest";
        case Msg::ResourceChunk:    return "ResourceChunk";
        case Msg::VehicleWorld:     return "VehicleWorld";
        case Msg::VehicleCmd:       return "VehicleCmd";
        case Msg::PlayerLook:       return "PlayerLook";
        case Msg::PeerLook:         return "PeerLook";
        case Msg::VehicleUpdate:    return "VehicleUpdate";
        case Msg::VehicleResult:    return "VehicleResult";
        case Msg::Hit:              return "Hit";
        case Msg::Damage:           return "Damage";
        case Msg::Killed:           return "Killed";
        case Msg::Respawn:          return "Respawn";
        case Msg::RespawnOk:        return "RespawnOk";
        case Msg::PlayAnim:         return "PlayAnim";
        case Msg::PlayScenario:     return "PlayScenario";
        case Msg::StopAnim:         return "StopAnim";
        case Msg::PeerAnim:         return "PeerAnim";
        case Msg::PeerScenario:     return "PeerScenario";
        case Msg::PeerAnimStop:     return "PeerAnimStop";
        case Msg::SeatNotice:       return "SeatNotice";
        case Msg::VehicleImpact:    return "VehicleImpact";
        }
        return "?";
    }

    const char* ToString(RefuseReason r)
    {
        switch (r)
        {
        case RefuseReason::VersionMismatch: return "VersionMismatch";
        case RefuseReason::BadName:         return "BadName";
        case RefuseReason::NameTaken:       return "NameTaken";
        case RefuseReason::ServerFull:      return "ServerFull";
        case RefuseReason::Protocol:        return "Protocol";
        case RefuseReason::HelloTimeout:    return "HelloTimeout";
        case RefuseReason::Banned:          return "Banned";
        case RefuseReason::TooManyConnections: return "TooManyConnections";
        }
        return "?";
    }

    const char* ToString(LeaveReason r)
    {
        switch (r)
        {
        case LeaveReason::Quit:    return "Quit";
        case LeaveReason::Timeout: return "Timeout";
        case LeaveReason::Kicked:  return "Kicked";
        }
        return "?";
    }

    const char* ToString(VehicleOp op)
    {
        switch (op)
        {
        case VehicleOp::Spawn:      return "Spawn";
        case VehicleOp::Despawn:    return "Despawn";
        case VehicleOp::Enter:      return "Enter";
        case VehicleOp::Exit:       return "Exit";
        case VehicleOp::SetLocked:  return "SetLocked";
        case VehicleOp::SetLights:  return "SetLights";
        case VehicleOp::SetColours: return "SetColours";
        case VehicleOp::Register:   return "Register";
        case VehicleOp::Release:    return "Release";
        case VehicleOp::EnterIntent:  return "EnterIntent";
        case VehicleOp::CancelIntent: return "CancelIntent";
        }
        return "?";
    }

    const char* ToString(SeatNoticeKind k)
    {
        switch (k)
        {
        case SeatNoticeKind::Reserved:  return "Reserved";
        case SeatNoticeKind::Seated:    return "Seated";
        case SeatNoticeKind::Cancelled: return "Cancelled";
        case SeatNoticeKind::Exited:    return "Exited";
        }
        return "?";
    }

    const char* ToString(SeatCancelReason r)
    {
        switch (r)
        {
        case SeatCancelReason::ByPlayer:    return "ByPlayer";
        case SeatCancelReason::Expired:     return "Expired";
        case SeatCancelReason::Replaced:    return "Replaced";
        case SeatCancelReason::VehicleGone: return "VehicleGone";
        case SeatCancelReason::PlayerGone:  return "PlayerGone";
        }
        return "?";
    }

    const char* ToString(VehicleResultCode c)
    {
        switch (c)
        {
        case VehicleResultCode::Ok:           return "Ok";
        case VehicleResultCode::NoVehicle:    return "NoVehicle";
        case VehicleResultCode::SeatTaken:    return "SeatTaken";
        case VehicleResultCode::TooFar:       return "TooFar";
        case VehicleResultCode::Locked:       return "Locked";
        case VehicleResultCode::NotAllowed:   return "NotAllowed";
        case VehicleResultCode::Limit:        return "Limit";
        case VehicleResultCode::RateLimited:  return "RateLimited";
        case VehicleResultCode::BadArg:       return "BadArg";
        case VehicleResultCode::Dead:         return "Dead";
        case VehicleResultCode::NotInVehicle: return "NotInVehicle";
        case VehicleResultCode::NoState:      return "NoState";
        case VehicleResultCode::NotOwner:     return "NotOwner";
        }
        return "?";
    }

}
