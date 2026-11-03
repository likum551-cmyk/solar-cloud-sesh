// t4_server_tests.h: the server-side rules of the vehicle-collision work (T4), included by tests/server_test.cpp (one include, one call): the claim of
// a driverless vehicle by a driver about to hit it, and the speed cap that clamps a small overshoot instead of dropping the car. The arbiter itself is
// pinned by tests/t4_collision_tests.h; the whole path over real ENet by tests/t4_netcode_tests.h.
#pragma once

#include <cstdarg>

namespace t4srv {

static std::string T4F(const char* fmt, ...)
{
    char buf[500];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

constexpr int kT4ServerChecks = 2;

static void T4ServerTests()
{
    using Code = proto::VehicleResultCode;
    const proto::Quat rot = proto::QuatFromHeading(0.0f);

    // ---- the claim ----
    {
        // E is parked at the origin; its last driver p0 stands 20 m away (inside the 250 m the controller keeps it from), so today p0 stays its controller and
        // every other player sees E as a copy nobody can push. p1 drives D toward E at 15 m/s.
        auto build = [&](server::VehicleStore& vs, float dy, float dvy, uint16_t& e, uint16_t& d) {
            vs.Configure(V5Rules(), 8);
            for (uint16_t p = 0; p < 4; p++) vs.AddReceiver(p);
            vs.Create(1, At(0, 0), rot, 0, 0, 0, 7, false, 0, 1, &e);               // p0's Spawn
            vs.Enter(0, e, 0, At(0, 0), 100, 2);                                       // p0 drives it ...
            vs.Update(0, UnitDrive(e, 0, 0, 0), 150);
            vs.Exit(0, e, 200, 3);                                                     // ... and walks away: the last driver keeps the control
            vs.Create(1, At(0, dy), rot, 0, 0, 1, 7, false, 0, 4, &d);                 // p1's Spawn D, `dy` m from E
            vs.Enter(1, d, 0, At(0, dy), 300, 5);
            vs.Update(1, UnitDrive(d, 0, dy, dvy), 350);
            proto::VehicleWorld w;
            vs.BuildFrame(1, true, At(0, dy), 6, 400, w);                              // E is in p1's view (RelevantTo)
            vs.BuildFrame(0, true, At(20, 0), 6, 400, w);
            vs.Update(0, UnitDrive(e, 0, 0, 0), 1900);                                 // the parked car's controller is alive (proof of life every 200 ms): it is not "silent"
        };
        auto assign = [&](server::VehicleStore& vs, uint16_t dId, bool trusted1, uint64_t now, uint32_t tick) {
            std::vector<server::VehicleStore::ControlCandidate> ps = {Cand(0, 20, 0), Cand(1, 0, -1000, trusted1), Cand(2, 500, 500)};
            ps[1].pos = vs.Get(dId)->s.k;       // p1 sits in D
            vs.AssignControllers(now, tick, ps);
        };
        // approaching at 15 m/s from 12 m: claimed
        server::VehicleStore a;
        uint16_t ea = 0, da = 0;
        build(a, -12.0f, 15.0f, ea, da);
        const uint16_t before = a.Get(ea)->s.ctrl;
        assign(a, da, true, 2000, 7);
        const bool claimed = before == 0 && a.Get(ea)->s.ctrl == 1 && a.Claims() == 1;
        const Code oldCtrl = a.Update(0, UnitDrive(ea, 0, 0, 0), 2010);
        bool jumped = false;
        const Code newCtrl = a.Update(1, UnitDrive(ea, 0, 0, 3), 2020, &jumped);
        // not claimed: slow, driving away, too far for its speed, an untrusted driver
        server::VehicleStore slow, away, farCar, bad;
        uint16_t e1, d1, e2, d2, e3, d3, e4, d4;
        build(slow, -12.0f, 2.0f, e1, d1);  assign(slow, d1, true, 2000, 7);
        build(away, -12.0f, -15.0f, e2, d2); assign(away, d2, true, 2000, 7);
        build(farCar, -40.0f, 15.0f, e3, d3);  assign(farCar, d3, true, 2000, 7);
        build(bad, -12.0f, 15.0f, e4, d4);  assign(bad, d4, false, 2000, 7);
        const bool noneClaimed = slow.Get(e1)->s.ctrl == 0 && away.Get(e2)->s.ctrl == 0 && farCar.Get(e3)->s.ctrl == 0 && bad.Get(e4)->s.ctrl == 0 &&
                                 slow.Claims() + away.Claims() + farCar.Claims() + bad.Claims() == 0;
        // a claim holds: a second approacher inside claimHoldMs does not take it, after the hold it may (here p2 drives F at it)
        uint16_t f = 0;
        a.Create(1, At(0, 8), rot, 0, 0, 2, 7, false, 0, 8, &f);
        a.Enter(2, f, 0, At(0, 8), 2100, 9);
        a.Update(2, UnitDrive(f, 0, 8, -15), 2150);
        proto::VehicleWorld w;
        a.BuildFrame(2, true, At(0, 8), 10, 2200, w);
        std::vector<server::VehicleStore::ControlCandidate> ps2 = {Cand(0, 20, 0), Cand(1, 0, -12), Cand(2, 0, 8)};
        ps2[1].pos = a.Get(da)->s.k;
        ps2[2].pos = a.Get(f)->s.k;
        a.AssignControllers(2300, 11, ps2);
        const bool held = a.Get(ea)->s.ctrl == 1;
        a.Update(2, UnitDrive(f, 0, 8, -15), 3600);
        a.Update(1, UnitDrive(da, 0, -12, 15), 3600);
        a.Update(1, UnitDrive(ea, 0, 0, 0), 3650);                                     // the claimant keeps sending (a claimed car must not look silent)
        ps2[1].pos = a.Get(da)->s.k;
        ps2[2].pos = a.Get(f)->s.k;
        a.AssignControllers(3700, 12, ps2);
        const bool released = a.Get(ea)->s.ctrl == 2;
        Check("T4 claim: a driver heading for a driverless vehicle within claimBase + 0.5 s of travel (here 12 m at 15 m/s) takes its control from the last driver -- the old controller's updates are then "
              "NotAllowed, the claimant's Ok; not for a slow (2 m/s) driver, one driving away, one 40 m out, or an untrusted one; a claim holds 1.5 s against another approacher and then yields to the closer one",
              claimed && oldCtrl == Code::NotAllowed && newCtrl == Code::Ok && noneClaimed && held && released,
              T4F("claimed %d (ctrl %u, claims %llu), old %s, new %s; none claimed %d (ctrl slow %u away %u far %u untrusted %u); held %d, released %d", int(claimed), a.Get(ea)->s.ctrl,
                (unsigned long long)a.Claims(), proto::ToString(oldCtrl), proto::ToString(newCtrl), int(noneClaimed), slow.Get(e1)->s.ctrl, away.Get(e2)->s.ctrl, farCar.Get(e3)->s.ctrl,
                bad.Get(e4)->s.ctrl, int(held), int(released)));
    }

    // ---- the speed cap ----
    {
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        vs.AddReceiver(0);
        uint16_t v = 0;
        vs.Create(1, At(0, 0), rot, 0, 0, 0, 7, false, 0, 1, &v);
        vs.Enter(0, v, 0, At(0, 0), 100, 2);
        auto drive = [&](float vy, float y, uint64_t now) {
            proto::VehicleUpdate u = UnitDrive(v, 0, y, vy);
            return vs.Update(0, u, now);
        };
        const Code ok = drive(140.0f, 3.0f, 200);                    // under the cap: untouched
        const float vOk = vs.Get(v)->s.k.vy;
        const Code over = drive(160.0f, 6.0f, 300);                  // a hyper car's overshoot: clamped, accepted, position taken
        const float vOver = vs.Get(v)->s.k.vy, yOver = vs.Get(v)->s.k.y;
        const uint64_t clamps = vs.SpeedClamps();
        const Code way = drive(301.0f, 9.0f, 400);                   // more than twice the cap: refused, nothing taken
        const float yWay = vs.Get(v)->s.k.y;
        server::VehicleStore::Rules old = V5Rules();
        old.maxSpeedSoftFactor = 1.0f;                               // the v5 behaviour: any overshoot is dropped
        server::VehicleStore vo;
        vo.Configure(old, 8);
        vo.AddReceiver(0);
        uint16_t vo1 = 0;
        vo.Create(1, At(0, 0), rot, 0, 0, 0, 7, false, 0, 1, &vo1);
        vo.Enter(0, vo1, 0, At(0, 0), 100, 2);
        const Code oldOver = vo.Update(0, UnitDrive(vo1, 0, 3, 160.0f), 200);
        Check("T4 speed cap: a 140 m/s update is untouched; 160 m/s (over the 150 m/s cap) is accepted with the velocity clamped to 150 and the position taken (counted); 301 m/s is still BadArg and "
              "changes nothing; with the soft factor 1 (the v5 rule) the 160 m/s update is dropped",
              ok == Code::Ok && vOk == 140.0f && over == Code::Ok && std::fabs(vOver - 150.0f) < 1e-3f && std::fabs(yOver - 6.0f) < 1e-3f && clamps == 1 && way == Code::BadArg &&
                  std::fabs(yWay - 6.0f) < 1e-3f && oldOver == Code::BadArg && vo.SpeedClamps() == 0,
              T4F("140 -> %s %.1f; 160 -> %s %.2f at y %.1f (clamps %llu); 301 -> %s; v5 rule: %s", proto::ToString(ok), vOk, proto::ToString(over), vOver, yOver, (unsigned long long)clamps,
                proto::ToString(way), proto::ToString(oldOver)));
    }
}

}   // namespace t4srv
