// t3_vehvisual_test: vehicle visual state for the other clients (Desync Overhaul round 2, T3; protocol v6).
// Burst tyres (and tyres off their rim), smashed windows, brake lights / handbrake, and steering that also covers a
// standing car: the wire, the server's merge, the client's pure sync module (libs/sync/vehicle_visual.h) driven against
// a fake car, and the whole chain through the REAL in-process server over ENet (controller A, observer B, shooter C).
// Build target: t3_vehvisual_test (selftest/, never shipped). Pure computation plus loopback sockets, about 8 s.
//
// What is NOT proven here (live-unverified, docs/api/protocol.md "T3"): that the natives do what their names say on the
// game (their hashes are still 0: UNRESOLVED, lookup needed), that SET_VEHICLE_STEER_BIAS turns a puppet's wheels, and that
// a copy with the handbrake released rolls free. The fake car obeys the commands it is given; what is proven is the logic
// and the wire around the natives, and that a native that cannot be called changes nothing.
//
//   1. Wire: the new bytes of VehicleUpdate / VehicleWorld, refusals (a rim bit without its tyre bit, truncation, trailing
//      byte, flipped bits), the vehicle-Hit part codes, flags2 bits 6 and 7, the server's size accounting.
//   2. VehicleStore: the controller's report merged with Hits (HitPart / MergeVisual), the ack and the 3 s drop, a car with
//      no controller keeps its bits, full entries carry the state and motion-only ones do not, hand-over and stream-in.
//   3. VisualSync against a fake car: copy level apply, no pop-fix-pop flicker, a local pop reported and confirmed or
//      expired, controller rising-edge apply that never clears, idempotence, and the UNRESOLVED-native path (inert, bounded,
//      neutral reports, and the same code works once the natives answer).
//   4. Steering and pedals: yaw rate from rotations, the kinematic estimate end to end through the wire (sign), a standing
//      car, a flipped axis shows in the stats; brake / handbrake classification.
//   5. The real server: A reports a burst tyre -> B's copy has it; C pops a tyre and a window of A's car -> A and B get them,
//      A applies and reports them, a pop A never shows is dropped, a late joiner gets the state in the full entry, a Hit
//      with no shot is refused.
#include "netrig.h"

#include "sync/vehicle_body.h"
#include "sync/vehicle_drive.h"
#include "sync/vehicle_visual.h"
#include "vehicles.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace gage::sync;

static std::string F(const char* fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// ===========================================================================
// 1. Wire
// ===========================================================================

static proto::VehicleUpdate SampleUpdate()
{
    proto::VehicleUpdate u;
    u.seq = 7;
    u.vehId = 3;
    u.k = Kin(10.0f, 20.0f, 30.0f, 0.0f, 1.0f, 2.0f, 0.0f);
    u.rot = proto::QuatFromHeading(0.0f);
    u.flags = proto::kVehEngine;
    u.sampleMs = 100;
    u.flags2 = proto::kVeh2Headlights | proto::kVeh2Brake | proto::kVeh2Handbrake;
    u.doors = proto::kDoorHood;
    u.tyres = 0x35;
    u.tyresRim = 0x14;
    u.windows = 0xA1;
    return u;
}

static proto::VehicleState SampleFull(uint16_t id)
{
    proto::VehicleState v;
    v.id = id;
    v.model = 0xB779A091u;
    v.k = Kin(1.0f, 2.0f, 3.0f);
    v.flags = proto::kVehEngine;
    v.seats[0] = 2;
    v.ctrl = 2;
    v.flags2 = proto::kVeh2Brake;
    v.tyres = 0x81;
    v.tyresRim = 0x01;
    v.windows = 0x06;
    return v;
}

static void TestWire()
{
    std::printf("-- 1. wire\n");
    const proto::VehicleUpdate up = SampleUpdate();
    const proto::Bytes ub = proto::Encode(up);
    proto::VehicleUpdate back;
    const bool rt = !ub.empty() && proto::Decode(ub.data(), ub.size(), back) && back.tyres == 0x35 && back.tyresRim == 0x14 &&
                    back.windows == 0xA1 && back.flags2 == up.flags2 && back.doors == up.doors && proto::Encode(back) == ub;
    Check("wire: VehicleUpdate carries tyres, tyresRim and windows after doors (the last three bytes) and re-encodes byte for byte",
          rt && ub.size() == 40 && ub[37] == 0x35 && ub[38] == 0x14 && ub[39] == 0xA1, F("%zu bytes", ub.size()));

    proto::VehicleWorld w;
    w.tick = 9;
    w.present = {4, 5};
    w.vehicles = {SampleFull(4)};
    proto::VehicleState mo;
    mo.id = 5;
    mo.motionOnly = true;
    mo.k = Kin(1.0f, 2.0f, 3.0f);
    w.vehicles.push_back(mo);
    const proto::Bytes wb = proto::Encode(w);
    proto::VehicleWorld wback;
    const bool wrt = !wb.empty() && proto::Decode(wb.data(), wb.size(), wback) && wback.vehicles.size() == 2 && wback.vehicles[0].tyres == 0x81 &&
                     wback.vehicles[0].tyresRim == 0x01 && wback.vehicles[0].windows == 0x06 && wback.vehicles[0].flags2 == proto::kVeh2Brake &&
                     wback.vehicles[1].motionOnly && wback.vehicles[1].tyres == 0 && wback.vehicles[1].windows == 0 && proto::Encode(wback) == wb;
    Check("wire: a full VehicleWorld entry carries the three masks (flags2 brake bit too); a motion-only entry carries none and decodes with them 0",
          wrt, F("%zu bytes", wb.size()));

    // Refusals: a rim bit without its tyre bit, in the encoder and the decoder, for both messages.
    proto::VehicleUpdate badUp = up;
    badUp.tyres = 0x00;
    badUp.tyresRim = 0x01;
    proto::VehicleWorld badW = w;
    badW.vehicles[0].tyres = 0x00;
    badW.vehicles[0].tyresRim = 0x04;
    proto::Bytes ub2 = ub;
    ub2[37] = 0x10;                                   // tyres 0x10, rim 0x14: rim not a subset
    proto::Bytes wb2 = wb;
    size_t tyOff = 0;                                 // the tyres byte of the full entry: find it by the known pattern 0x81 0x01 0x06
    for (size_t i = 0; i + 2 < wb2.size(); ++i)
        if (wb2[i] == 0x81 && wb2[i + 1] == 0x01 && wb2[i + 2] == 0x06) { tyOff = i; break; }
    wb2[tyOff] = 0x00;                                // tyres 0, rim 1
    proto::VehicleUpdate tmpU;
    proto::VehicleWorld tmpW;
    Check("wire: a rim bit without its tyre bit is refused by both encoders and both decoders (and ValidVehicleUpdate)",
          tyOff != 0 && proto::Encode(badUp).empty() && proto::Encode(badW).empty() && !proto::Decode(ub2.data(), ub2.size(), tmpU) &&
              !proto::Decode(wb2.data(), wb2.size(), tmpW) && !proto::ValidVehicleUpdate(badUp) && proto::ValidVehicleUpdate(up));

    // Truncation at every length and one trailing byte (the generic fuzz checks do the same for the old fields).
    int trunc = 0, trail = 0;
    for (size_t n = 0; n < ub.size(); ++n)
        if (proto::Decode(ub.data(), n, tmpU)) ++trunc;
    for (size_t n = 0; n < wb.size(); ++n)
        if (proto::Decode(wb.data(), n, tmpW)) ++trunc;
    proto::Bytes ubl = ub, wbl = wb;
    ubl.push_back(0);
    wbl.push_back(0);
    if (proto::Decode(ubl.data(), ubl.size(), tmpU)) ++trail;
    if (proto::Decode(wbl.data(), wbl.size(), tmpW)) ++trail;
    Check("wire: every truncation of the v6 VehicleUpdate / VehicleWorld and one trailing byte are refused", trunc == 0 && trail == 0,
          F("%d truncated and %d trailing accepted", trunc, trail));

    // Every single-bit flip of the three new bytes of the update: accepted only when rim stays a subset of tyres, and then it
    // re-encodes to exactly the flipped bytes (canonical).
    int flips = 0, wrongAccept = 0, accepted = 0;
    for (size_t byte = 37; byte < 40; ++byte)
        for (int bit = 0; bit < 8; ++bit)
        {
            proto::Bytes m = ub;
            m[byte] = (uint8_t)(m[byte] ^ (1u << bit));
            ++flips;
            proto::VehicleUpdate r;
            const bool ok = proto::Decode(m.data(), m.size(), r);
            const bool legal = (m[38] & ~m[37]) == 0;
            if (ok != legal) ++wrongAccept;
            if (ok)
            {
                ++accepted;
                if (proto::Encode(r) != m) ++wrongAccept;
            }
        }
    Check("wire: each of the 24 single-bit flips of the new update bytes is accepted exactly when rim is still a subset of tyres, and then re-encodes canonically",
          flips == 24 && wrongAccept == 0 && accepted > 0 && accepted < 24, F("%d accepted, %d wrong", accepted, wrongAccept));

    // flags2: bits 6 and 7 are brake lights and handbrake now, no bit is reserved; the helper builds them.
    proto::VehicleUpdate f2 = up;
    f2.flags2 = 0xFF;
    const proto::Bytes f2b = proto::Encode(f2);
    proto::VehicleUpdate f2back;
    const uint8_t built = Flags2FromState(true, false, false, false, false, false, true, true);
    Check("wire: flags2 0xFF is legal (kVeh2Known 0xFF); Flags2FromState sets the brake and handbrake bits and, by default, leaves them clear",
          proto::kVeh2Known == 0xFF && proto::Decode(f2b.data(), f2b.size(), f2back) && f2back.flags2 == 0xFF &&
              built == (proto::kVeh2Headlights | proto::kVeh2Brake | proto::kVeh2Handbrake) &&
              Flags2FromState(true, true, true, true, true, true) == 0x3F);

    // The vehicle-Hit part codes.
    using proto::VehiclePart;
    VehiclePart part = VehiclePart::None;
    unsigned slot = 99;
    const bool t5 = proto::ParseVehiclePart(proto::VehiclePartCode(VehiclePart::Tyre, 5), part, slot) && part == VehiclePart::Tyre && slot == 5;
    const bool r0 = proto::ParseVehiclePart(proto::VehiclePartCode(VehiclePart::TyreRim, 0), part, slot) && part == VehiclePart::TyreRim && slot == 0;
    const bool w7 = proto::ParseVehiclePart(proto::VehiclePartCode(VehiclePart::Window, 7), part, slot) && part == VehiclePart::Window && slot == 7;
    VehiclePart junk = VehiclePart::None;
    unsigned js = 0;
    const bool refused = !proto::ParseVehiclePart(-1, junk, js) && !proto::ParseVehiclePart(31086, junk, js) /* SKEL_Head */ &&
                         !proto::ParseVehiclePart(proto::VehiclePartCode(VehiclePart::Tyre, 8), junk, js) &&
                         !proto::ParseVehiclePart(proto::VehiclePartCode(VehiclePart::None, 1), junk, js) &&
                         !proto::ParseVehiclePart((int16_t)0x0400, junk, js) && !proto::ParseVehiclePart(0, junk, js);
    int16_t codes[24];
    VisualMask rep;
    rep.tyres = 0x31;
    rep.rim = 0x10;
    rep.windows = 0x82;
    const int nCodes = ReportToParts(rep, codes, 24);
    Check("wire: Hit.bone part codes (part << 8 | slot) parse; -1, a ped bone id, slot 8 and part 0 / 4 are not parts; a report becomes one code per rim slot, partial burst and window",
          t5 && r0 && w7 && refused && nCodes == 5 && codes[0] == proto::VehiclePartCode(VehiclePart::Tyre, 0) &&
              codes[1] == proto::VehiclePartCode(VehiclePart::TyreRim, 4) && codes[2] == proto::VehiclePartCode(VehiclePart::Tyre, 5) &&
              codes[3] == proto::VehiclePartCode(VehiclePart::Window, 1) && codes[4] == proto::VehiclePartCode(VehiclePart::Window, 7),
          F("%d codes", nCodes));
}

// ===========================================================================
// 2. VehicleStore
// ===========================================================================

static server::VehicleStore::Rules StoreRules()
{
    server::VehicleStore::Rules r;
    r.maxVehicles = 64;
    r.perPlayer = 3;
    r.perIp = 6;
    r.emptyDespawnMs = 600000;
    r.damageNeedsHealthLoss = false;       // round 2 trusted the controller's report; the R3 T4 rule (a clean car cannot gain a window) has its own tests (tests/r3_t4_tests.h)
    return r;
}

static proto::Kinematics At(float x, float y)
{
    proto::Kinematics k;
    k.x = x;
    k.y = y;
    k.z = 30.0f;
    return k;
}

static proto::VehicleUpdate Drive(uint16_t veh, float x, float y, float vy, uint8_t tyres = 0, uint8_t rim = 0, uint8_t windows = 0)
{
    proto::VehicleUpdate u;
    u.vehId = veh;
    u.k.x = x;
    u.k.y = y;
    u.k.z = 30.0f;
    u.k.vy = vy;
    u.rot = proto::QuatFromHeading(0.0f);
    u.flags = proto::kVehEngine;
    u.tyres = tyres;
    u.tyresRim = rim;
    u.windows = windows;
    return u;
}

struct StoreRig
{
    server::VehicleStore vs;
    uint16_t veh = 0;
    uint32_t tick = 1;
    StoreRig()
    {
        vs.Configure(StoreRules(), 8);
        for (uint16_t p = 0; p < 4; p++) vs.AddReceiver(p);
        vs.Create(1, At(0, 0), proto::QuatFromHeading(0.0f), 0, 0, proto::kNoPlayer, 0, true, 0, tick++, &veh);
        vs.Enter(0, veh, 0, At(0, 0), 500, tick++);          // player 0 drives: controller
    }
    // The newest frame for `receiver`; entries are returned as the receiver would hold them.
    bool Frame(uint16_t receiver, uint64_t nowMs, proto::VehicleWorld& w) { return vs.BuildFrame(receiver, true, At(10, 0), tick++, nowMs, w); }
    const proto::VehicleState* Entry(const proto::VehicleWorld& w) const { return w.vehicles.empty() ? nullptr : &w.vehicles[0]; }
};

static void TestStore()
{
    std::printf("-- 2. VehicleStore\n");
    using proto::VehiclePart;
    {
        // The controller's report reaches an observer; a clear report clears it (a repair); a full entry carries it.
        StoreRig R;
        const server::VehicleStore::Code c1 = R.vs.Update(0, Drive(R.veh, 0, 1, 10, 0x05, 0x04, 0x20), 1000);
        proto::VehicleWorld w;
        R.Frame(1, 1030, w);
        const proto::VehicleState* e = R.Entry(w);
        const bool seen = e && !e->motionOnly && e->tyres == 0x05 && e->tyresRim == 0x04 && e->windows == 0x20;
        R.vs.Update(0, Drive(R.veh, 0, 2, 10, 0x01, 0x00, 0x20), 1100); // slot 2 repaired
        R.Frame(1, 1130, w);
        e = R.Entry(w);
        const bool repaired = e && e->tyres == 0x01 && e->tyresRim == 0x00 && e->windows == 0x20;
        Check("store: the controller's tyre / rim / window report goes to an observer in the full entry the change triggers, and a later report without a bit clears it",
              c1 == server::VehicleStore::Code::Ok && seen && repaired);
    }
    {
        // Entries between changes are motion-only; the change is told in full for 3 frames and then again with the 1/s statics.
        StoreRig R;
        R.vs.Update(0, Drive(R.veh, 0, 1, 10), 1000);
        proto::VehicleWorld w;
        int fulls = 0, motion = 0, fullsWithBits = 0;
        for (int i = 0; i < 6; i++) { R.Frame(1, 1030 + i * 50, w); R.vs.Update(0, Drive(R.veh, 0, 1.0f + i, 10), 1050 + i * 50); }
        R.vs.Update(0, Drive(R.veh, 0, 8, 10, 0x08, 0x08, 0), 1400); // a tyre pops
        for (int i = 0; i < 8; i++)
        {
            R.Frame(1, 1430 + i * 50, w);
            R.vs.Update(0, Drive(R.veh, 0, 9.0f + i, 10, 0x08, 0x08, 0), 1450 + i * 50);
            if (const proto::VehicleState* e = R.Entry(w))
            {
                if (e->motionOnly) ++motion;
                else { ++fulls; if (e->tyres == 0x08) ++fullsWithBits; }
            }
        }
        Check("store: a tyre pop makes the next 3 frames full entries (all carrying it), then motion-only entries (which carry no masks, the receiver keeps what it has)",
              fulls >= 3 && fulls <= 4 && fullsWithBits == fulls && motion >= 4, F("%d full, %d motion-only", fulls, motion));
    }
    {
        // HitPart: sets the bit at once for everybody, idempotent, rim implies tyre, refuses a bad slot, windows.
        StoreRig R;
        const bool a = R.vs.HitPart(R.veh, VehiclePart::Tyre, 2, 1000, R.tick++);
        const bool again = R.vs.HitPart(R.veh, VehiclePart::Tyre, 2, 1010, R.tick++);
        const bool rim = R.vs.HitPart(R.veh, VehiclePart::TyreRim, 6, 1020, R.tick++);
        const bool win = R.vs.HitPart(R.veh, VehiclePart::Window, 3, 1030, R.tick++);
        const bool badSlot = R.vs.HitPart(R.veh, VehiclePart::Tyre, 8, 1040, R.tick++);
        const bool noCar = R.vs.HitPart(9, VehiclePart::Tyre, 0, 1040, R.tick++);
        const bool nonePart = R.vs.HitPart(R.veh, VehiclePart::None, 1, 1040, R.tick++);
        const server::VehicleStore::Vehicle* V = R.vs.Get(R.veh);
        proto::VehicleWorld w;
        R.Frame(2, 1050, w);
        const proto::VehicleState* e = R.Entry(w);
        Check("store: HitPart sets a tyre / rim (rim implies the tyre) / window bit at once for every receiver, is idempotent and refuses a bad slot, a missing car and part None",
              a && !again && rim && win && !badSlot && !noCar && !nonePart && V->s.tyres == 0x44 && V->s.tyresRim == 0x40 && V->s.windows == 0x08 &&
                  e && !e->motionOnly && e->tyres == 0x44 && e->tyresRim == 0x40 && e->windows == 0x08);
    }
    {
        // The controller shows a Hit's bit (ack): it stays, also past the 3 s, and a later clear (repair) removes it.
        StoreRig R;
        R.vs.Update(0, Drive(R.veh, 0, 1, 10), 1000);
        R.vs.HitPart(R.veh, VehiclePart::Tyre, 1, 1100, R.tick++);
        R.vs.Update(0, Drive(R.veh, 0, 2, 10), 1133);                  // the controller has not shown it yet: kept
        const bool kept = R.vs.Get(R.veh)->s.tyres == 0x02;
        R.vs.Update(0, Drive(R.veh, 0, 3, 10, 0x02), 1300);            // now it shows it
        R.vs.Update(0, Drive(R.veh, 0, 4, 10, 0x02), 9000);            // long after the 3 s: still there
        const bool acked = R.vs.Get(R.veh)->s.tyres == 0x02;
        R.vs.Update(0, Drive(R.veh, 0, 5, 10), 9033);                  // repaired by the controller: gone at once (it was acked)
        const bool cleared = R.vs.Get(R.veh)->s.tyres == 0;
        Check("store: a Hit's bit survives the controller's reports until it shows it (ack), stays after, and the controller's later clear removes it at once",
              kept && acked && cleared);
    }
    {
        // A bit the controller never shows is dropped 3 s after the Hit, not before.
        StoreRig R;
        R.vs.Update(0, Drive(R.veh, 0, 1, 10), 1000);
        R.vs.HitPart(R.veh, VehiclePart::TyreRim, 0, 2000, R.tick++);
        R.vs.Update(0, Drive(R.veh, 0, 2, 10), 4900);                  // 2.9 s
        const bool before = R.vs.Get(R.veh)->s.tyres == 0x01 && R.vs.Get(R.veh)->s.tyresRim == 0x01;
        R.vs.Update(0, Drive(R.veh, 0, 3, 10), 5001);                  // 3.0 s
        const bool dropped = R.vs.Get(R.veh)->s.tyres == 0 && R.vs.Get(R.veh)->s.tyresRim == 0;
        Check("store: a Hit's bit the controller never shows is kept for 2.9 s and dropped at 3.0 s (a phantom does not live for ever)", before && dropped);
    }
    {
        // With no controller nobody can show or clear: the bits stay (and the controller's brake lights do not).
        StoreRig R;
        R.vs.Update(0, Drive(R.veh, 0, 1, 0, 0x01), 1000);
        proto::VehicleUpdate braking = Drive(R.veh, 0, 1, 0, 0x01);
        braking.flags2 = proto::kVeh2Brake | proto::kVeh2Handbrake;
        R.vs.Update(0, braking, 1033);
        R.vs.Exit(0, R.veh, 1100, R.tick++);                           // the driver stays the controller (it is near)
        R.vs.HitPart(R.veh, VehiclePart::Tyre, 4, 1200, R.tick++);
        std::vector<server::VehicleStore::ControlCandidate> none;      // nobody near: AssignControllers clears the controller
        R.vs.AssignControllers(60000, R.tick++, none);
        R.vs.Tick(60001, R.tick++, nullptr);
        const bool noCtrl = R.vs.Get(R.veh)->s.ctrl == proto::kNoPlayer;
        proto::VehicleWorld w;
        R.Frame(1, 60100, w);
        const proto::VehicleState* e = R.Entry(w);
        Check("store: a car nobody controls keeps its tyre bits for ever (no report can clear them) and shows no brake lights; the handbrake bit stays",
              noCtrl && e && e->tyres == 0x11 && (e->flags2 & proto::kVeh2Brake) == 0 && (e->flags2 & proto::kVeh2Handbrake) != 0);
    }
    {
        // Hand-over: the new controller's reports are truth from its first update; bits it shows stay, others are the old rules.
        StoreRig R;
        R.vs.Update(0, Drive(R.veh, 0, 1, 10, 0x03, 0x01, 0x01), 1000);
        R.vs.Exit(0, R.veh, 1100, R.tick++);
        std::vector<server::VehicleStore::ControlCandidate> ps;
        server::VehicleStore::ControlCandidate c;
        c.id = 1;
        c.pos = At(30, 0);
        c.hasState = true;
        ps.push_back(c);
        proto::VehicleWorld w;
        R.vs.BuildFrame(1, true, At(30, 0), R.tick++, 1150, w);        // makes the car relevant to player 1
        R.vs.Enter(1, R.veh, 0, At(1, 0), 1200, R.tick++);              // player 1 takes the wheel
        R.vs.Update(1, Drive(R.veh, 0, 2, 10, 0x03, 0x01, 0x01), 1233); // its copy showed what the server told it
        const server::VehicleStore::Vehicle* V = R.vs.Get(R.veh);
        Check("store: after a hand-over the new controller's report of the same state keeps it (tyres 0x03, rim 0x01, window 0x01); the old controller's Update is refused",
              V->s.ctrl == 1 && V->s.tyres == 0x03 && V->s.tyresRim == 0x01 && V->s.windows == 0x01 &&
                  R.vs.Update(0, Drive(R.veh, 0, 3, 10), 1300) == server::VehicleStore::Code::NotAllowed && R.vs.Get(R.veh)->s.tyres == 0x03);
    }
    {
        // Size accounting: FullBytes equals what the encoder writes for a full entry; the packet cap uses it.
        StoreRig R;
        R.vs.Update(0, Drive(R.veh, 0, 1, 10, 0x0F, 0x03, 0x10), 1000);
        proto::VehicleWorld w;
        R.Frame(1, 1030, w);
        proto::VehicleWorld one;
        one.present = w.present;
        one.vehicles = w.vehicles;
        const size_t entry = proto::Encode(one).size() - (1 + 4 + 1 + 2) - 2;       // minus the frame header and the id + header byte
        Check("store: FullBytes (the packet-cap accounting) equals the encoded size of a full entry with one occupant (v6: 46 + 2; v7 adds the 3-byte damage block of a clean car: 49 + 2)",
              server::VehicleStore::FullBytes(w.vehicles[0]) == entry && entry == 51, F("%zu vs %zu", server::VehicleStore::FullBytes(w.vehicles[0]), entry));
    }
}

// ===========================================================================
// 3. VisualSync against a fake car
// ===========================================================================

// One entity. `tyreNatives` / `windowNatives` model a hash that is 0 (UNRESOLVED) or unmapped: the wrapper then returns false for
// every call, without touching the car. `ineffective` models a native that is called, reports success and does nothing.
struct FakeCar
{
    VisualMask m;
    bool tyreNatives = true, windowNatives = true, ineffective = false;
    int readCalls = 0, writeCalls = 0;
    int burstCalls = 0, rimCalls = 0, fixCalls = 0, smashCalls = 0, fixWinCalls = 0;
    bool TyreBurst(int s, bool completely, bool& burst)
    {
        ++readCalls;
        if (!tyreNatives) return false;
        burst = completely ? (m.rim >> s) & 1 : (m.tyres >> s) & 1;
        return true;
    }
    bool WindowIntact(int s, bool& intact)
    {
        ++readCalls;
        if (!windowNatives) return false;
        intact = !((m.windows >> s) & 1);
        return true;
    }
    bool BurstTyre(int s, bool onRim)
    {
        ++writeCalls;
        if (!tyreNatives) return false;
        onRim ? ++rimCalls : ++burstCalls;
        if (!ineffective)
        {
            m.tyres = (uint8_t)(m.tyres | (1u << s));
            if (onRim) m.rim = (uint8_t)(m.rim | (1u << s));
        }
        return true;
    }
    bool FixTyre(int s)
    {
        ++writeCalls;
        if (!tyreNatives) return false;
        ++fixCalls;
        if (!ineffective)
        {
            m.tyres = (uint8_t)(m.tyres & ~(1u << s));
            m.rim = (uint8_t)(m.rim & ~(1u << s));
        }
        return true;
    }
    bool SmashWindow(int s)
    {
        ++writeCalls;
        if (!windowNatives) return false;
        ++smashCalls;
        if (!ineffective) m.windows = (uint8_t)(m.windows | (1u << s));
        return true;
    }
    bool FixWindow(int s)
    {
        ++writeCalls;
        if (!windowNatives) return false;
        ++fixWinCalls;
        if (!ineffective) m.windows = (uint8_t)(m.windows & ~(1u << s));
        return true;
    }
};

static VisualMask VM(unsigned tyres, unsigned rim = 0, unsigned windows = 0)
{
    VisualMask m;
    m.tyres = (uint8_t)tyres;
    m.rim = (uint8_t)rim;
    m.windows = (uint8_t)windows;
    return m;
}

static void TestVisualSync()
{
    std::printf("-- 3. VisualSync\n");
    {
        // A copy comes up with the server's state (first sight), applies it once, and a re-check changes nothing.
        VisualSync vs;
        FakeCar car;
        const VisualMask server = VM(0x13, 0x10, 0x05);
        const VisualOut o = StepVisual(vs, car, server, false, true, false, 1000.0);
        const VisualCmd c1 = o.cmd;
        const bool shown = car.m == server;
        const int writes1 = car.writeCalls;
        // Idempotent: the same server state at every later check (even at the periodic re-check) issues nothing.
        int issuedLater = 0;
        for (double t = 1100; t < 9000; t += 100)
        {
            const VisualOut oo = StepVisual(vs, car, server, false, true, false, t);
            issuedLater += oo.cmd.Bits();
        }
        Check("visual copy: the first sight applies the server's tyres (partial and off the rim) and windows, exactly once; later checks of the same state issue nothing",
              shown && c1.burst == 0x03 && c1.rim == 0x10 && c1.smash == 0x05 && writes1 == 5 && issuedLater == 0 && car.writeCalls == 5,
              F("burst %02x rim %02x smash %02x, %d writes, %d later", c1.burst, c1.rim, c1.smash, writes1, issuedLater));
    }
    {
        // Level semantics: a repair (server clears a bit) fixes the copy's tyre / window; a bit the copy shows that the server does not is put right.
        VisualSync vs;
        FakeCar car;
        StepVisual(vs, car, VM(0x03, 0x01, 0x02), false, true, false, 1000.0);
        StepVisual(vs, car, VM(0x01, 0x01, 0x00), false, true, false, 1100.0);     // slot 1 and window 1 repaired
        const bool fixed = car.m == VM(0x01, 0x01, 0x00) && car.fixCalls == 1 && car.fixWinCalls == 1;
        car.m.tyres |= 0x20;                                                        // the game popped slot 5 by itself (a bump on this machine)
        VisualSync vs2;
        FakeCar copy;
        StepVisual(vs2, copy, VM(0), false, true, false, 1000.0);
        copy.m.tyres |= 0x20;
        StepVisual(vs2, copy, VM(0), false, true, false, 2600.0);                    // the periodic check finds it: a local pop, reported...
        const VisualOut keep = StepVisual(vs2, copy, VM(0), false, true, false, 4200.0);   // ...kept pending for 2.5 s (not fixed at 1.6 s)...
        const VisualOut mid = StepVisual(vs2, copy, VM(0), false, true, false, 5700.0);   // ...then put right: the server never showed it
        Check("visual copy: a bit the server clears is fixed on the copy (tyre and window), level not edge; a pop the server never confirms is put right after the pending time",
              fixed && keep.cmd.fixTyre == 0 && (copy.m.tyres & 0x20) == 0 && mid.cmd.fixTyre == 0x20, F("fixed %d (car %02x/%02x/%02x fix %d/%d) keep %02x mid %02x copy tyres %02x", (int)fixed, car.m.tyres, car.m.rim, car.m.windows, car.fixCalls, car.fixWinCalls, keep.cmd.fixTyre, mid.cmd.fixTyre, copy.m.tyres));
    }
    {
        // The shooter's copy pops a tyre locally: reported once; while the server has not answered the copy is NOT fixed (no flicker);
        // when the server confirms it is counted; one the server never shows is fixed after pendingMs and may be reported again.
        VisualSync vs;
        FakeCar car;
        StepVisual(vs, car, VM(0), false, true, false, 1000.0);                       // first sight, clean car
        car.m.tyres |= 0x04;                                                           // our bullet bursts slot 2 on the copy
        VisualOut o = StepVisual(vs, car, VM(0), false, true, true, 1200.0);           // shot-time scan (200 ms)
        const bool reported = o.report.tyres == 0x04 && o.report.rim == 0 && !o.cmd.Any();
        int fixesWhilePending = 0, reportsWhilePending = 0;
        for (double t = 1400; t < 3600; t += 200)                                      // 2.2 s with the server still saying "intact"
        {
            const VisualOut p = StepVisual(vs, car, VM(0), false, true, true, t);
            fixesWhilePending += p.cmd.fixTyre != 0;
            reportsWhilePending += p.report.Any();
        }
        const bool stillPopped = (car.m.tyres & 0x04) != 0;
        const VisualOut gone = StepVisual(vs, car, VM(0), false, true, true, 3800.0);   // 2.6 s after the report: expired
        const bool fixedAfter = gone.cmd.fixTyre == 0x04 && (car.m.tyres & 0x04) == 0 && !gone.report.Any();
        const VisualStats s = vs.Stats();
        Check("visual copy: a local pop is reported once (not again, not fixed) while the server's answer is awaited for 2.5 s, then put right when it never came",
              reported && fixesWhilePending == 0 && reportsWhilePending == 0 && stillPopped && fixedAfter && s.reported == 1 && s.expired == 1 && s.confirmed == 0,
              F("fixes %d reports %d reported %u expired %u", fixesWhilePending, reportsWhilePending, s.reported, s.expired));

        // Confirmed: the server shows the bit after a round trip; the copy keeps it, nothing is re-reported, confirmation counted.
        VisualSync vc;
        FakeCar cc;
        StepVisual(vc, cc, VM(0), false, true, false, 1000.0);
        cc.m.tyres |= 0x04;
        cc.m.rim |= 0x04;
        const VisualOut r1 = StepVisual(vc, cc, VM(0), false, true, true, 1200.0);
        StepVisual(vc, cc, VM(0x04, 0x04), false, true, true, 1300.0);                 // the server's full entry arrives
        int more = 0;
        for (double t = 1500; t < 8000; t += 100)
        {
            const VisualOut p = StepVisual(vc, cc, VM(0x04, 0x04), false, true, false, t);
            more += p.report.Any() + (p.cmd.Any() ? 1 : 0);
        }
        Check("visual copy: a tyre popped off its rim is reported as a rim bit, confirmed by the server's entry, then kept without commands or reports",
              r1.report.rim == 0x04 && r1.report.tyres == 0x04 && more == 0 && cc.m == VM(0x04, 0x04) && vc.Stats().confirmed >= 1 && vc.Stats().expired == 0,
              F("confirmed %u more %d", vc.Stats().confirmed, more));
    }
    {
        // A burst becomes a rim burst while pending: the escalation is a new fact and is reported.
        VisualSync vs;
        FakeCar car;
        StepVisual(vs, car, VM(0), false, true, false, 1000.0);
        car.m.tyres |= 0x01;
        const VisualOut a = StepVisual(vs, car, VM(0), false, true, true, 1200.0);
        car.m.rim |= 0x01;
        const VisualOut b = StepVisual(vs, car, VM(0), false, true, true, 1400.0);
        Check("visual copy: a pending partial burst that goes off its rim is reported again as a rim bit (only the rim)",
              a.report.tyres == 0x01 && a.report.rim == 0 && b.report.rim == 0x01 && !b.cmd.Any());
    }
    {
        // The controller: applies a bit the server ADDS (the Hit of someone else) once; never clears; hand-over resets.
        VisualSync vs;
        FakeCar car;
        StepVisual(vs, car, VM(0), true, true, false, 1000.0);
        const bool quiet = !car.m.Any() && car.writeCalls == 0;
        const VisualOut add = StepVisual(vs, car, VM(0x20, 0x20, 0x08), true, true, false, 1100.0);   // C popped slot 5 and smashed window 3
        const bool applied = car.m == VM(0x20, 0x20, 0x08) && add.cmd.rim == 0x20 && add.cmd.smash == 0x08 && add.cmd.burst == 0;
        const int writes = car.writeCalls;
        // The controller repairs its own car; the server (not yet told) still shows the bits: nothing is re-applied.
        car.m = VisualMask();
        int reapplied = 0;
        for (double t = 1200; t < 6000; t += 100)
            reapplied += StepVisual(vs, car, VM(0x20, 0x20, 0x08), true, true, false, t).cmd.Bits();
        const bool noFight = !car.m.Any() && reapplied == 0 && car.writeCalls == writes;
        // A later Hit adds another bit (window 7): only THAT bit is applied - the tyre the controller repaired is not burst again (edge, not level).
        const VisualOut other = StepVisual(vs, car, VM(0x20, 0x20, 0x88), true, true, false, 6000.0);
        const bool onlyNew = other.cmd.smash == 0x80 && other.cmd.rim == 0 && other.cmd.burst == 0 && car.m == VM(0, 0, 0x80);
        // The server clears them (after the controller's report): the controller does not touch its car for a clear.
        StepVisual(vs, car, VM(0), true, true, false, 6100.0);
        // A fresh Hit on the same slot later is a new rising edge and applies again.
        const VisualOut again = StepVisual(vs, car, VM(0x20, 0x20, 0), true, true, false, 6200.0);
        Check("visual controller: applies what the server adds once (rim, partial, window), never fights the controller's own repair (a later Hit applies only its own bit), never clears, and a new Hit on the same slot applies again",
              quiet && applied && noFight && onlyNew && again.cmd.rim == 0x20 && car.m == VM(0x20, 0x20, 0x80), F("reapplied %d, tyres %02x rim %02x windows %02x", reapplied, car.m.tyres, car.m.rim, car.m.windows));
        // Hand-over to a copy: Reset() then the first sight is level (the copy is made to match the server).
        vs.Reset();
        car.m = VM(0x01);
        StepVisual(vs, car, VM(0x20, 0x20, 0), false, true, false, 6300.0);
        Check("visual: after Reset (a hand-over, a new entity) the first call is a first sight again - as a copy the car is made to match the server's mask",
              car.m == VM(0x20, 0x20, 0), F("tyres %02x rim %02x", car.m.tyres, car.m.rim));
    }
    {
        // Bikes: wheel slots 0 (front) and 4 (rear) only; a car's middle slots are untouched; slot numbers are never model-specific.
        VisualSync vs;
        FakeCar car;
        StepVisual(vs, car, VM(0x11, 0x10), false, true, false, 1000.0);
        Check("visual copy: the slot masks are model-agnostic - slots 0 and 4 (a bike's wheels) apply and nothing else is touched",
              car.m == VM(0x11, 0x10) && car.burstCalls == 1 && car.rimCalls == 1);
    }
    {
        // The controller's report: the readback per group; a group that cannot be read reports what was applied.
        VisualSync vs;
        FakeCar car;
        StepVisual(vs, car, VM(0x02), true, true, false, 1000.0);
        car.m.windows = 0x40;                                                          // smashed by a bullet on the controller's machine
        const VisualMask rep = ReadReport(vs, car);
        FakeCar blind;
        blind.tyreNatives = false;
        blind.windowNatives = false;
        VisualSync vb;
        StepVisual(vb, blind, VM(0x02), true, true, false, 1000.0);                    // cannot apply either
        const VisualMask repBlind = ReadReport(vb, blind);
        Check("visual controller: the report is the readback (tyre 1 from the server's add, window 6 from the bullet); with unreadable natives it is the neutral applied state, never 'everything intact' overwriting a bit",
              rep == VM(0x02, 0, 0x40) && !repBlind.Any());
    }
    {
        // UNRESOLVED natives (hash 0): every wrapper returns false. Inert: the car is never touched, no report, bounded attempts,
        // a neutral controller report, and the server's bits are not lost.
        VisualSync vs;
        FakeCar car;
        car.tyreNatives = car.windowNatives = false;
        int ran = 0, reports = 0;
        const VisualMask server = VM(0x13, 0x10, 0x05);
        for (int i = 0; i < 1000; ++i)                                                 // 100 s at 10 Hz
        {
            bool did = false;
            const VisualOut o = StepVisual(vs, car, server, false, true, i % 3 == 0, 1000.0 + i * 100.0, &did);
            ran += did;
            reports += o.report.Any();
        }
        Check("UNRESOLVED natives (copy): the car is never changed, nothing is reported, and the attempts are bounded (3 rounds, then 5 s of silence, ~20 retries in 100 s) - not a stream of calls",
              !car.m.Any() && reports == 0 && car.writeCalls <= 3 * 5 * 24 && car.writeCalls < 800 && vs.Stats().suspends >= 15 && ran < 400,
              F("%d write calls, %d read calls, %u suspends, %d rounds", car.writeCalls, car.readCalls, vs.Stats().suspends, ran));
        VisualSync vc;
        FakeCar cc;
        cc.tyreNatives = cc.windowNatives = false;
        for (int i = 0; i < 300; ++i)
            StepVisual(vc, cc, server, true, true, false, 1000.0 + i * 100.0);
        Check("UNRESOLVED natives (controller): nothing happens either, bounded, and the report is neutral",
              !cc.m.Any() && cc.writeCalls < 300 && !ReadReport(vc, cc).Any());
        // The same code, the natives now answering (the hashes are added): it works with no other change.
        car.tyreNatives = car.windowNatives = true;
        double t = 1000.0 + 1000 * 100.0 + 6000.0;
        for (int i = 0; i < 5; ++i)
            StepVisual(vs, car, server, false, true, false, t + i * 100.0);
        Check("UNRESOLVED -> resolved: once the same car's natives answer, the next check after the back-off applies the whole server state with no other change",
              car.m == server, F("tyres %02x rim %02x windows %02x", car.m.tyres, car.m.rim, car.m.windows));
        // A native that is called, succeeds and does nothing: the divergence is counted and flagged as stuck (live diagnostic).
        VisualSync vi;
        FakeCar ineff;
        ineff.ineffective = true;
        for (int i = 0; i < 40; ++i)
            StepVisual(vi, ineff, VM(0x01), false, true, true, 1000.0 + i * 200.0);
        Check("visual diagnostics: a native that succeeds but changes nothing shows as divergent reads that are 'stuck' (the same difference on consecutive reads)",
              vi.Stats().divergent >= 10 && vi.Stats().stuck >= 1, F("%u divergent, %u stuck", vi.Stats().divergent, vi.Stats().stuck));
    }
}

// ===========================================================================
// 4. Steering, brake lights
// ===========================================================================

static void TestSteerAndBrake()
{
    std::printf("-- 4. steering and pedals\n");
    {
        // Yaw rate from rotations: unit-safe (independent of GET_ENTITY_ROTATION_VELOCITY), sign + = left, banked car, bad gaps refused.
        const double yawRate = 0.5;                                                    // rad/s, a left turn
        DriveQuat q0 = vq::FromHeadingDeg(30.0);
        const double dt = 1.0 / 60.0;
        DriveQuat q1 = vq::Mul(vq::Exp(DriveVec{ 0.0f, 0.0f, (float)(yawRate * dt) }), q0);
        float w = 0;
        const bool ok = YawRateFromRotations(q0, q1, dt, w);
        // A car banked 20 degrees about its forward axis turns about ITS up axis: the world-frame rotation vector projected on it.
        const DriveQuat roll = vq::Exp(DriveVec{ 0.0f, (float)(20.0 * 3.14159265 / 180.0), 0.0f });
        const DriveQuat b0 = vq::Mul(q0, roll);
        const DriveVec up = vq::Rotate(b0, DriveVec{ 0, 0, 1 });
        const DriveQuat b1 = vq::Mul(vq::Exp(DriveVec{ up.x * (float)(yawRate * dt), up.y * (float)(yawRate * dt), up.z * (float)(yawRate * dt) }), b0);
        float wb = 0;
        const bool okb = YawRateFromRotations(b0, b1, dt, wb);
        float junk = 0;
        Check("steer: the yaw rate comes from two rotations (0.5 rad/s left -> +0.5, also for a banked car about its own up axis); a gap under 5 ms or over 300 ms and non-finite input are refused",
              ok && std::fabs(w - 0.5f) < 0.01f && okb && std::fabs(wb - 0.5f) < 0.01f && !YawRateFromRotations(q0, q1, 0.001, junk) &&
                  !YawRateFromRotations(q0, q1, 0.5, junk) && !YawRateFromRotations(q0, DriveQuat{ NAN, 0, 0, 1 }, dt, junk),
              F("%.4f / %.4f", w, wb));
    }
    {
        // The whole sender -> wire -> observer chain for a left turn at 15 m/s: the wire says left (negative), the observer's bias is the
        // negation (positive = left), and it survives quantisation (1/127). A kinematic bicycle with a known wheel angle drives the test.
        const float L = 2.8f, delta = 0.3f, speed = 15.0f;                              // 0.3 rad to the left (counter-clockwise)
        const double yawRate = speed * std::tan(delta) / L;
        DriveQuat q = vq::FromHeadingDeg(0.0);
        float prevSteer = 0.0f;
        float est = 0;
        const double dt = 1.0 / 60.0;
        for (int i = 0; i < 60; ++i)
        {
            const DriveQuat q2 = vq::Mul(vq::Exp(DriveVec{ 0.0f, 0.0f, (float)(yawRate * dt) }), q);
            float yr = 0;
            if (!YawRateFromRotations(q, q2, dt, yr)) yr = 0;
            const DriveVec fwd = vq::Rotate(q2, DriveVec{ 0.0f, 1.0f, 0.0f });
            const float vFwd = speed;                                                   // along the body's forward axis by construction
            est = ChooseSteer(vFwd, yr, L, prevSteer, false, 0.0f).steer;
            prevSteer = est;
            q = q2;
            (void)fwd;
        }
        proto::VehicleUpdate u = SampleUpdate();
        u.steer = est;
        const proto::Bytes b = proto::Encode(u);
        proto::VehicleUpdate back;
        proto::Decode(b.data(), b.size(), back);
        // The observer: the value goes through the interpolator and its drive output is what SET_VEHICLE_STEER_BIAS gets, negated.
        VehicleInterpolator ip;
        VehicleSnapshot s0, s1;
        s0.tMs = 0; s1.tMs = 50;
        s0.steering = s1.steering = back.steer;
        s0.rot = s1.rot = DriveQuat{ 0, 0, 0, 1 };
        ip.Push(s0);
        ip.Push(s1);
        VehicleSample out;
        ip.Sample(25.0, out);
        const float bias = -out.steering;
        Check("steer: a 0.3 rad left turn at 15 m/s is sent as about -0.5 (lock 0.6 rad), survives the wire, and the observer's SET_VEHICLE_STEER_BIAS value is the positive negation",
              std::fabs(est - (-0.5f)) < 0.03f && back.steer < 0 && std::fabs(back.steer - est) < 0.01f && bias > 0.4f && bias < 0.6f,
              F("est %.3f wire %.3f bias %.3f", est, back.steer, bias));
    }
    {
        // A standing car: the estimate holds its last value (the old behaviour: the wheels stay where they were or straight); with the player's axis
        // the wheels show the steering; the axis is used only below 1 m/s, only when it can be read; a car that moves uses the estimate.
        const SteerChoice held = ChooseSteer(0.0f, 0.0f, 2.8f, 0.0f, false, 0.0f);
        const SteerChoice heldLast = ChooseSteer(0.3f, 0.0f, 2.8f, -0.4f, false, 0.0f);
        const SteerChoice axisL = ChooseSteer(0.0f, 0.0f, 2.8f, 0.0f, true, -1.0f);
        const SteerChoice axisHalf = ChooseSteer(0.5f, 0.0f, 2.8f, 0.0f, true, 0.5f);
        const SteerChoice moving = ChooseSteer(15.0f, 1.2f, 2.8f, 0.0f, true, 1.0f);        // axis says hard right, physics says left: physics wins
        const SteerChoice clampHi = ChooseSteer(0.0f, 0.0f, 2.8f, 0.0f, true, 3.0f);
        const SteerChoice nan = ChooseSteer(0.0f, 0.0f, 2.8f, 0.0f, true, NAN);
        Check("steer: a standing car holds its estimate without the axis (as before) and shows the player's steering with it (left = -1 on the wire, clamped, NaN ignored); a moving car always uses the estimate",
              held.steer == 0.0f && held.source == SteerSource::Held && heldLast.steer == -0.4f && heldLast.source == SteerSource::Held &&
                  axisL.steer == -1.0f && axisL.source == SteerSource::Axis && axisHalf.steer == 0.5f && axisHalf.source == SteerSource::Axis &&
                  moving.source == SteerSource::Estimate && moving.steer < -0.3f && clampHi.steer == 1.0f && nan.source == SteerSource::Held,
              F("moving %.3f", moving.steer));
    }
    {
        // The stats: min / mean / max of what was written, and a flipped axis sign shows as disagreement while the car really turns.
        SteerStats good, flipped;
        for (int i = 0; i < 200; ++i)
        {
            const float v = 15.0f, yr = 0.8f * (i % 2 ? 1.0f : -1.0f);
            const float est = EstimateSteer(v, yr, 2.8f, 0.0f);
            const SteerChoice c = ChooseSteer(v, yr, 2.8f, 0.0f, true, est > 0 ? 0.8f : -0.8f);
            good.Add(c, true, est > 0 ? 0.8f : -0.8f, est, v);
            flipped.Add(c, true, est > 0 ? -0.8f : 0.8f, est, v);
        }
        SteerStats standing;
        standing.Add(ChooseSteer(0.0f, 0.0f, 2.8f, 0.0f, true, -0.7f), true, -0.7f, 0.0f, 0.0f);
        Check("steer stats: min / mean / max of the value written; the axis agrees with the estimate in a good run (200 / 0) and disagrees in a flipped-sign run (0 / 200); a standing sample counts as axis",
              good.n == 200 && good.agree == 200 && good.disagree == 0 && flipped.disagree == 200 && flipped.agree == 0 && good.min < -0.2f && good.max > 0.2f &&
                  std::fabs(good.Mean()) < 0.05 && standing.axis == 1 && standing.min == -0.7f,
              F("good %u/%u flipped %u/%u", good.agree, good.disagree, flipped.agree, flipped.disagree));
    }
    {
        const BrakeState rollBrake = ClassifyBrake(true, 0.0f, 0.8f, 0.0f, 10.0f);
        const BrakeState hand = ClassifyBrake(true, 0.0f, 0.0f, 1.0f, 10.0f);
        const BrakeState reversePedal = ClassifyBrake(true, 0.0f, 0.8f, 0.0f, 0.1f);            // S at a standstill = reverse, not braking
        const BrakeState reversing = ClassifyBrake(true, 0.9f, 0.0f, 0.0f, -4.0f);              // W while rolling backwards = braking
        const BrakeState gas = ClassifyBrake(true, 1.0f, 0.0f, 0.0f, 10.0f);
        const BrakeState blind = ClassifyBrake(false, 0.0f, 1.0f, 1.0f, 10.0f);
        const BrakeState nan = ClassifyBrake(true, NAN, 1.0f, 1.0f, 10.0f);
        Check("brake: foot brake while rolling forward and the accelerator while rolling backward light the brake lights, the handbrake sets both bits, a standstill pedal is not braking, no input = clear",
              rollBrake.brakeLights && !rollBrake.handbrake && hand.brakeLights && hand.handbrake && !reversePedal.brakeLights &&
                  reversing.brakeLights && !gas.brakeLights && !blind.brakeLights && !blind.handbrake && !nan.brakeLights && !nan.handbrake);
        // The bits reach the observer through DiffBody (flags2 delta) like the other body bits: the first sight applies both, a change only the changed one.
        BodyShown shown;
        const BodyDelta first = DiffBody(shown, 0, 0);
        shown = BodyShown{ proto::kVeh2Brake, 0, true };
        const BodyDelta release = DiffBody(shown, proto::kVeh2Handbrake, 0);
        Check("brake: on the observer the first sight applies brake lights AND handbrake (so a copy's handbrake is released), a change applies exactly the bits that changed",
              (first.flags2 & proto::kVeh2Brake) && (first.flags2 & proto::kVeh2Handbrake) && release.flags2 == (proto::kVeh2Brake | proto::kVeh2Handbrake));
    }
}

// ===========================================================================
// 5. The real server
// ===========================================================================

static const uint32_t kAdder = 0xB779A091u;
static const uint32_t kPistol = 0x1B06D571u;

static proto::VehicleResult VCmd(Rig& rig, Probe& p, proto::VehicleOp op, uint16_t veh, uint32_t arg)
{
    proto::VehicleCmd c;
    c.op = op;
    c.vehId = veh;
    c.arg = arg;
    const size_t before = p.Results().size();
    proto::VehicleResult none;
    none.code = (proto::VehicleResultCode)255;
    if (!p.s.SendVehicleCmd(c)) return none;
    rig.Until([&] { return p.Results().size() > before; }, 2000);
    const std::vector<proto::VehicleResult> r = p.Results();
    return r.size() > before ? r.back() : none;
}

static bool PlaceAt(Rig& rig, Probe& p, const proto::Kinematics& k)
{
    const net::Event* w = p.First(ET::Welcome);
    const uint16_t id = w ? w->welcome.yourId : proto::kNoPlayer;
    return SendUntil(rig, p, k, [&] {
        for (const server::PlayerInfo& i : rig.srv.Players())
            if (i.id == id && i.hasState && std::fabs(i.k.x - k.x) < 0.05f && std::fabs(i.k.y - k.y) < 0.05f) return true;
        return false;
    }, 2000);
}

static proto::Hit PartHit(const Probe& shooter, uint16_t veh, proto::VehiclePart part, unsigned slot, uint8_t shotSeq)
{
    (void)shooter;
    proto::Hit h;
    h.kind = proto::HitKind::Vehicle;
    h.targetId = veh;
    h.weapon = kPistol;
    h.damage = 1;
    h.bone = proto::VehiclePartCode(part, slot);
    h.shotSeq = shotSeq;
    return h;
}

// A's client in miniature: the controller sends its car's state at 30 Hz with whatever its (fake) car shows, and runs the
// controller side of VisualSync on the server's mask exactly as the engine does.
struct ControllerClient
{
    Rig& rig;
    Probe& p;
    uint16_t veh;
    FakeCar car;
    VisualSync sync;
    uint32_t seq = 100;
    double y = 104.0;
    bool report = true;                       // false: the client does not show what it applies (a controller that cannot)
    ControllerClient(Rig& r, Probe& pr, uint16_t v) : rig(r), p(pr), veh(v) {}
    void Send(double nowMs)
    {
        const proto::VehicleState* st = p.Vehicle(veh);
        if (st)
            StepVisual(sync, car, VM(st->tyres, st->tyresRim, st->windows), true, true, false, nowMs);
        proto::VehicleUpdate u = Drive(veh, 100.0f, (float)y, 0.0f);
        const VisualMask m = report ? ReadReport(sync, car) : VisualMask();
        u.tyres = m.tyres;
        u.tyresRim = m.rim;
        u.windows = m.windows;
        u.seq = ++seq;
        p.s.SendVehicleState(u);
    }
};

static uint16_t IdOf(const Probe& p)
{
    const net::Event* w = p.First(ET::Welcome);
    return w ? w->welcome.yourId : proto::kNoPlayer;
}

static void TestRealServer()
{
    std::printf("-- 5. the real server (controller A, observer B, shooter C, late joiner D)\n");
    Rig rig;
    server::Config cfg = Rig::BaseConfig();
    cfg.maxHitDistance = 30;
    if (!rig.Start(cfg)) { Check("t3 real server: starts", false); return; }
    Probe alice(rig), bob(rig), carol(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob") || !carol.Join("Carol")) { Check("t3 real server: three players join", false); return; }
    const bool placed = PlaceAt(rig, alice, Kin(100, 100, 30)) && PlaceAt(rig, bob, Kin(103, 100, 30)) && PlaceAt(rig, carol, Kin(106, 100, 30));
    const proto::VehicleResult spawn = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
    const uint16_t vid = spawn.vehId;
    const bool seen = rig.Until([&] { return alice.Vehicle(vid) && bob.Vehicle(vid) && carol.Vehicle(vid); }, 2000);
    const proto::VehicleResult enter = VCmd(rig, alice, proto::VehicleOp::Enter, vid, 0);
    Check("t3 real server: three players join and are placed, A spawns a car and sits in the driver's seat (controller), B and C see it",
          placed && spawn.code == proto::VehicleResultCode::Ok && seen && enter.code == proto::VehicleResultCode::Ok &&
              rig.srv.FindVehicle(vid) && rig.srv.FindVehicle(vid)->s.ctrl == IdOf(alice));

    ControllerClient A(rig, alice, vid);
    const auto t0 = Clock::now();
    auto clock = [&] { return (double)MsSince(t0); };
    // A sends its car at ~30 Hz while the rig runs, until `pred` holds or `ms` pass.
    auto pumpA = [&](const std::function<bool()>& pred, int ms) {
        const auto s = Clock::now();
        auto next = s;
        while (MsSince(s) < ms)
        {
            if (Clock::now() >= next)
            {
                A.Send(clock());
                next += std::chrono::milliseconds(33);
            }
            rig.Step();
            if (pred && pred()) return true;
        }
        return pred ? pred() : false;
    };
    auto view = [&](Probe& p, uint8_t tyres, uint8_t rim, uint8_t windows) {
        const proto::VehicleState* v = p.Vehicle(vid);
        return v && v->tyres == tyres && v->tyresRim == rim && v->windows == windows;
    };

    // ---- 1. the controller's own burst tyre reaches the others ----
    A.car.m = VM(0x01);                                   // slot 0 burst on A's machine (a collision, a spike strip)
    const bool bSees = pumpA([&] { return view(bob, 0x01, 0, 0) && view(carol, 0x01, 0, 0); }, 3000);
    Check("t3 real server: A's burst tyre (slot 0, from A's own car) is in B's and C's copy within a second or two - the controller -> server -> observer chain",
          bSees && rig.srv.FindVehicle(vid)->s.tyres == 0x01);

    // ---- 2. C pops a tyre off its rim and smashes a window of A's car (a car C does not control) ----
    const uint64_t hitsBefore = rig.srv.stats().hitsAccepted, partsBefore = rig.srv.stats().vehiclePartHits;
    Fire(rig, carol, Kin(106, 100, 30));
    carol.s.SendHit(PartHit(carol, vid, proto::VehiclePart::TyreRim, 5, carol.shotSeq));
    Fire(rig, carol, Kin(106, 100, 30));
    carol.s.SendHit(PartHit(carol, vid, proto::VehiclePart::Window, 2, carol.shotSeq));
    const bool everyone = pumpA([&] { return view(alice, 0x21, 0x20, 0x04) && view(bob, 0x21, 0x20, 0x04) && view(carol, 0x21, 0x20, 0x04); }, 4000);
    const server::Stats s2 = rig.srv.stats();
    Check("t3 real server: C's two vehicle Hits (a tyre off its rim at slot 5, window 2) set the bits for everybody: A (the controller), B and C all hold tyres 0x21 / rim 0x20 / windows 0x04; both Hits were accepted and counted as part hits",
          everyone && s2.hitsAccepted == hitsBefore + 2 && s2.vehiclePartHits == partsBefore + 2,
          F("hits %llu parts %llu", (unsigned long long)(s2.hitsAccepted - hitsBefore), (unsigned long long)(s2.vehiclePartHits - partsBefore)));

    // ---- 3. A applies what C did to A's car, once, and reports it - the bits then outlive the 3 s ack window ----
    pumpA(nullptr, 3600);
    Check("t3 real server: A's client (controller side of VisualSync) burst slot 5 off its rim and smashed window 2 on its own car exactly once, and its reports keep the bits for B past the 3 s ack window",
          A.car.m == VM(0x21, 0x20, 0x04) && A.car.rimCalls == 1 && A.car.smashCalls == 1 && A.car.burstCalls == 0 && view(bob, 0x21, 0x20, 0x04) &&
              view(carol, 0x21, 0x20, 0x04) && rig.srv.FindVehicle(vid)->s.tyres == 0x21,
          F("rim calls %d smash calls %d burst calls %d, server tyres %02x", A.car.rimCalls, A.car.smashCalls, A.car.burstCalls, rig.srv.FindVehicle(vid)->s.tyres));

    // ---- 4. a pop A's client cannot show is dropped after 3 s, one it shows stays ----
    A.car.windowNatives = false;                          // A's window natives stop answering (UNRESOLVED / unmapped on that build)
    Fire(rig, carol, Kin(106, 100, 30));
    carol.s.SendHit(PartHit(carol, vid, proto::VehiclePart::Window, 6, carol.shotSeq));
    const bool shownFirst = pumpA([&] { return view(bob, 0x21, 0x20, 0x44); }, 1500);            // window 6 is there for B...
    const bool aHasIt = view(alice, 0x21, 0x20, 0x44);
    const bool dropped = pumpA([&] { return view(bob, 0x21, 0x20, 0x04) && view(carol, 0x21, 0x20, 0x04); }, 4500);   // ...until A had its chance
    Check("t3 real server: a window popped on a car whose controller cannot show it is visible to B first (presence), then dropped about 3 s after the Hit, while the tyre bits A shows stay",
          shownFirst && aHasIt && dropped && A.car.m.windows == 0x04,
          F("window 6 shown first %d, dropped %d", (int)shownFirst, (int)dropped));
    A.car.windowNatives = true;

    // ---- 5. a Hit with no shot behind it is refused, and sets nothing ----
    const server::Stats s3 = rig.srv.stats();
    carol.s.SendHit(PartHit(carol, vid, proto::VehiclePart::Tyre, 1, (uint8_t)((carol.shotSeq + 7) & 15)));
    rig.Until([&] { return rig.srv.stats().hitsRejected > s3.hitsRejected; }, 2000);
    pumpA(nullptr, 300);
    const server::Stats s4 = rig.srv.stats();
    Check("t3 real server: a vehicle Hit with a shotSeq the shooter never fired is refused (counted) and no tyre bit appears; a Hit with bone -1 only does body damage",
          s4.hitsRejected == s3.hitsRejected + 1 && s4.vehiclePartHits == s3.vehiclePartHits && view(bob, 0x21, 0x20, 0x04));
    const uint16_t bodyBefore = rig.srv.FindVehicle(vid)->s.bodyHealth;
    Fire(rig, carol, Kin(106, 100, 30));
    proto::Hit plain = PartHit(carol, vid, proto::VehiclePart::Tyre, 1, carol.shotSeq);
    plain.bone = -1;
    plain.damage = 50;
    carol.s.SendHit(plain);
    pumpA([&] { return rig.srv.FindVehicle(vid)->s.bodyHealth < bodyBefore; }, 2000);
    Check("t3 real server: a vehicle Hit with a plain bone (-1) still only lowers the body health and sets no part",
          rig.srv.FindVehicle(vid)->s.bodyHealth == bodyBefore - 50 && rig.srv.FindVehicle(vid)->s.tyres == 0x21 && rig.srv.stats().vehiclePartHits == s3.vehiclePartHits);

    // ---- 6. stream-in: a player who arrives later gets the state in the full entry ----
    Probe dave(rig);
    const bool joined = dave.Join("Dave");
    const bool daveAt = joined && PlaceAt(rig, dave, Kin(110, 100, 30));
    const bool daveSees = pumpA([&] { return view(dave, 0x21, 0x20, 0x04); }, 3000);
    Check("t3 real server: a late joiner D, placed in range, gets tyres 0x21 / rim 0x20 / windows 0x04 in the car's first full entry (stream-in carries the state)",
          joined && daveAt && daveSees);
}

// ===========================================================================

#include "r3_t4_tests.h"   // R3 T4: vehicle damage state, the sanity layer, the v7 wire (one include, one call below)

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto t0 = Clock::now();
    TestWire();
    TestStore();
    TestVisualSync();
    TestSteerAndBrake();
    TestRealServer();
    const int r2Checks = g_run;
    RunR3T4Tests();
    // A skipped section (an early return) must fail, not pass vacuously: 7 wire + 8 store + 13 visual + 6 steer / brake + 8 real-server
    // (round 2), then the R3 T4 checks (kR3T4Checks, tests/r3_t4_tests.h).
    const int expected = 7 + 8 + 13 + 6 + 8 + kR3T4Checks;
    (void)r2Checks;
    std::printf("%d/%d passed  (%lld ms)\n", g_run - g_failed, g_run, (long long)MsSince(t0));
    if (g_run != expected)
    {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
