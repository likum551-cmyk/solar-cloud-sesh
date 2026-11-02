// The absolute (baseline-free) wire form of a QKin and (v3) of a QPed, shared by PlayerState and
// by the "absolute" entries of WorldState. The QKin part: 16 bytes, little-endian.
//
//   u64  position: x (22 bits signed, cm) | y (22 bits signed, cm) << 22 | z (20 bits signed, cm) << 44
//   u16  heading   (65536 steps per turn)
//   i16  vx, vy, vz (1/32 m/s)
//
// Internal to libs/protocol (protocol.cpp, worldcodec.cpp); not part of the public API.
#pragma once

#include "quant.h"
#include "wire.h"

namespace gage::proto::qwire {

    constexpr size_t kAbsoluteBytes = 16;

    inline int32_t SignExtend(uint32_t v, int bits)
    {
        const uint32_t m = 1u << (bits - 1);
        return (int32_t)((v ^ m) - m);
    }

    inline void PutAbsolute(wire::Writer& w, const QKin& q)
    {
        const uint64_t px = (uint64_t)((uint32_t)q.x & 0x3FFFFFu);
        const uint64_t py = (uint64_t)((uint32_t)q.y & 0x3FFFFFu);
        const uint64_t pz = (uint64_t)((uint32_t)q.z & 0xFFFFFu);
        w.u64(px | (py << 22) | (pz << 44));
        w.u16(q.heading);
        w.i16(q.vx);
        w.i16(q.vy);
        w.i16(q.vz);
    }

    // False when the reader failed or the value is outside the quantiser box (quant.h).
    inline bool GetAbsolute(wire::Reader& r, QKin& q)
    {
        const uint64_t v = r.u64();
        q.x = SignExtend((uint32_t)(v & 0x3FFFFFu), kBitsXY);
        q.y = SignExtend((uint32_t)((v >> 22) & 0x3FFFFFu), kBitsXY);
        q.z = SignExtend((uint32_t)((v >> 44) & 0xFFFFFu), kBitsZ);
        q.heading = r.u16();
        q.vx = r.i16();
        q.vy = r.i16();
        q.vz = r.i16();
        return r.ok() && QInRange(q);
    }

    // The absolute wire form of a QPed (v3, v4, v5), after the 16-byte kinematics in PlayerState and in
    // absolute WorldState entries: 8 bytes, 9 while entering / exiting a vehicle, 6 more while aiming.
    //
    //   u8   move (bits 0..2) | jumpSeq << 3 (bits 3..4); bits 5..7 are 0
    //   u16  flags (kPf*)
    //   u32  weapon hash
    //   u8   shotSeq (bits 0..3); bits 4..7 are 0          (v4)
    //   u8   vehId: present only when move is 5 (entering) or 6 (exiting)   (v5)
    //   i16  aimX, aimY, aimZ in decimetres: present only when flags has kPfAiming
    //
    // A WorldState entry (never a PlayerState) follows it with the server-owned vitals (v4):
    //   u8   health, u8 armor
    constexpr size_t kPedBytes = 8;
    constexpr size_t kAimBytes = 6;
    constexpr size_t kVitalsBytes = 2;

    inline size_t PedBytes(const QPed& p)
    {
        return kPedBytes + (MoveNamesVehicle(p.move) ? 1 : 0) + ((p.flags & kPfAiming) ? kAimBytes : 0);
    }
    inline uint8_t MoveByte(const QPed& p) { return (uint8_t)((p.move & 7u) | ((p.jumpSeq & 3u) << 3)); }

    inline void PutPed(wire::Writer& w, const QPed& p)
    {
        w.u8(MoveByte(p));
        w.u16(p.flags);
        w.u32(p.weapon);
        w.u8((uint8_t)(p.shotSeq & 15u));
        if (MoveNamesVehicle(p.move)) w.u8(p.vehId);
        if (p.flags & kPfAiming)
        {
            w.i16(p.ax);
            w.i16(p.ay);
            w.i16(p.az);
        }
    }

    inline void PutVitals(wire::Writer& w, const QPed& p)
    {
        w.u8(p.health);
        w.u8(p.armor);
    }

    // False when the reader failed.
    inline bool GetVitals(wire::Reader& r, QPed& p)
    {
        p.health = r.u8();
        p.armor = r.u8();
        return r.ok();
    }

    // The vehicle position: the same u64 packing as QKin (x 22, y 22, z 20 bits of centimetres).
    inline void PutPos(wire::Writer& w, const QKin& q)
    {
        const uint64_t px = (uint64_t)((uint32_t)q.x & 0x3FFFFFu);
        const uint64_t py = (uint64_t)((uint32_t)q.y & 0x3FFFFFu);
        const uint64_t pz = (uint64_t)((uint32_t)q.z & 0xFFFFFu);
        w.u64(px | (py << 22) | (pz << 44));
    }
    inline void GetPos(wire::Reader& r, QKin& q)
    {
        const uint64_t v = r.u64();
        q.x = SignExtend((uint32_t)(v & 0x3FFFFFu), kBitsXY);
        q.y = SignExtend((uint32_t)((v >> 22) & 0x3FFFFFu), kBitsXY);
        q.z = SignExtend((uint32_t)((v >> 44) & 0xFFFFFu), kBitsZ);
    }

    // Splits a move byte; false when a reserved bit (5..7) is set.
    inline bool SplitMoveByte(uint8_t b, QPed& p)
    {
        if (b & 0xE0u) return false;
        p.move = (uint8_t)(b & 7u);
        p.jumpSeq = (uint8_t)((b >> 3) & 3u);
        return true;
    }

    // False when the reader failed or a value is illegal (QPedInRange, reserved move-byte bits).
    inline bool GetPed(wire::Reader& r, QPed& p)
    {
        p = QPed();
        const bool moveOk = SplitMoveByte(r.u8(), p);
        p.flags = r.u16();
        p.weapon = r.u32();
        const uint8_t shot = r.u8();
        if (shot & 0xF0u) return false; // reserved bits of the shot byte
        p.shotSeq = shot;
        if (moveOk && MoveNamesVehicle(p.move)) p.vehId = r.u8();
        if (p.flags & kPfAiming)
        {
            p.ax = r.i16();
            p.ay = r.i16();
            p.az = r.i16();
        }
        return r.ok() && moveOk && QPedInRange(p);
    }

    // The whole absolute WorldState sample (kinematics, ped, vitals): 26 bytes, 32 while aiming.
    inline size_t AbsoluteStateBytes(const QState& s) { return kAbsoluteBytes + PedBytes(s.p) + kVitalsBytes; }

}
