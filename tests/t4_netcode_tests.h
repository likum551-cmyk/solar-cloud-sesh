// t4_netcode_tests.h: the vehicle-collision wire message and the server's contact arbitration over REAL ENet, included by tests/netcode_test.cpp
// (one include, one call). The pure logic is pinned by tests/t4_collision_tests.h (vehicle_drive_test); this is the end-to-end proof that the
// pieces meet: two clients drive two cars into each other through the real server, and each is told ITS half of the contact, once, on its own
// clock; a velocity over the cap is clamped instead of dropped.
#pragma once

namespace t4net {

constexpr int kT4NetChecks = 4;   // R3 T5: +1 (LinkInfo through the real server)

static void T4NetworkTests()
{
    // ---- the wire: 21 bytes, byte for byte ----
    {
        proto::VehicleImpact m;
        m.seq = 0x01020304u; m.vehId = 5; m.otherId = 9; m.sampleMs = 0xA1B2;
        m.dvx = -1.0f; m.dvy = 24.5f; m.dyaw = -2.0f; m.nx = 1.0f; m.ny = -1.0f; m.closing = 40.0f; m.depth = 0.25f;
        const proto::Bytes b = proto::Encode(m);
        const proto::Bytes want = {0x28,                    // type 40
                                   0x04, 0x03, 0x02, 0x01,  // seq
                                   0x05, 0x00, 0x09, 0x00,  // vehId, otherId
                                   0xB2, 0xA1,             // sampleMs
                                   0xC0, 0xFF,             // dvx = -1 m/s = -64 (1/64)
                                   0x20, 0x06,             // dvy = 24.5 m/s = 1568
                                   0x00, 0xFE,             // dyaw = -2 rad/s = -512 (1/256)
                                   0x7F, 0x81,             // nx = 1.0 = 127, ny = -1.0 = -127 (0x81)
                                   0xA0,                   // closing 40 m/s = 160 (0.25 m/s)
                                   0x19};                  // depth 0.25 m = 25 cm
        proto::VehicleImpact back;
        const bool dec = proto::Decode(b.data(), b.size(), back);
        proto::Msg t;
        Check("T4 wire: VehicleImpact is message 40, 21 bytes laid out byte for byte (seq, ids, the contact time on the receiver's clock, dv in 1/64 m/s, yaw in 1/256 rad/s, the normal in 1/127, "
              "closing in 0.25 m/s, depth in cm); it decodes back; PeekType knows 40 and not 55 (R3 T5 added 44 LinkInfo, v8 45..51, 0.5.4 52..54); the name is VehicleImpact",
              b == want && b.size() == proto::kVehicleImpactBytes && dec && back.seq == m.seq && back.vehId == 5 && back.otherId == 9 && back.sampleMs == 0xA1B2 &&
                  back.dvx == -1.0f && back.dvy == 24.5f && back.dyaw == -2.0f && back.nx == 1.0f && back.ny == -1.0f && back.closing == 40.0f && back.depth == 0.25f &&
                  proto::PeekType(b.data(), b.size(), t) && t == proto::Msg::VehicleImpact && std::string(proto::ToString(t)) == "VehicleImpact" &&
                  proto::kMsgLast == 54 && []{ const uint8_t x = 55; proto::Msg y; return !proto::PeekType(&x, 1, y); }(),
              Hex(b));
    }

    Rig rig;
    if (!rig.Start()) { Check("T4 net: server starts", false); Check("T4 net: -", false); return; }
    Probe alice(rig), bob(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob")) { Check("T4 net: two players join", false); Check("T4 net: -", false); return; }
    using C = proto::VehicleResultCode;

    // Alice's car at y = -26 facing +y, Bob's at y = +26 facing -y (0.4 m to the side), each driver in its own seat 0.
    const bool placed = Place(rig, alice, Kin(0.0f, -30.0f, 30.0f, 0.0f)) && Place(rig, bob, Kin(0.4f, 30.0f, 30.0f, 180.0f));
    const proto::VehicleResult sa = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
    const proto::VehicleResult sb = VCmd(rig, bob, proto::VehicleOp::Spawn, 0, kAdder);
    const proto::VehicleResult ea = VCmd(rig, alice, proto::VehicleOp::Enter, sa.vehId, 0);
    const proto::VehicleResult eb = VCmd(rig, bob, proto::VehicleOp::Enter, sb.vehId, 0);
    const bool ready = placed && sa.code == C::Ok && sb.code == C::Ok && ea.code == C::Ok && eb.code == C::Ok;
    if (!ready) { Check("T4 net: two cars spawned and entered", false, Code(sa) + " " + Code(sb) + " " + Code(ea) + " " + Code(eb)); Check("T4 net: -", false); return; }
    const server::VehicleStore::Vehicle* va = rig.srv.FindVehicle(sa.vehId);
    const server::VehicleStore::Vehicle* vb = rig.srv.FindVehicle(sb.vehId);
    const float ya0 = va ? va->s.k.y : 0.0f, yb0 = vb ? vb->s.k.y : 0.0f, xb0 = vb ? vb->s.k.x : 0.0f;

    // Drive them at each other at 20 m/s for real: an update every ~30 ms, stamped on each client's own clock (A's is +5000 ms, B's +61000 ms),
    // both stopping where their boxes touch (the controllers' own physics halts a car at the other's copy). The geometric touch is when the
    // centres are 4.6 m apart; the arbiter's boxes (grown 0.3 m each) meet 15 ms earlier.
    constexpr double kClkA = 5000.0, kClkB = 61000.0;
    const double gap0 = std::fabs(yb0 - ya0);
    const double tTouch = (gap0 - 4.6) / 40.0;                 // seconds
    const auto tStart = Clock::now();
    uint32_t seqA = 0, seqB = 0;
    double tc = -1;                                            // the instant the loop passed the touch
    const proto::Quat rotA = proto::QuatFromHeading(0.0f), rotB = proto::QuatFromHeading(180.0f);
    for (;;)
    {
        const double t = (double)MsSince(tStart) / 1000.0;
        if (t > tTouch + 0.45) break;
        const double run = std::min(t, tTouch);
        const bool moving = t < tTouch;
        proto::VehicleUpdate ua, ub;
        ua.seq = ++seqA; ua.vehId = sa.vehId; ua.k = Kin(0.0f, (float)(ya0 + 20.0 * run), 30.0f, 0.0f, 0.0f, moving ? 20.0f : 0.0f);
        ua.rot = rotA; ua.flags = proto::kVehEngine;
        ub.seq = ++seqB; ub.vehId = sb.vehId; ub.k = Kin(xb0, (float)(yb0 - 20.0 * run), 30.0f, 180.0f, 0.0f, moving ? -20.0f : 0.0f);
        ub.rot = rotB; ub.flags = proto::kVehEngine;
        alice.s.SendVehicleState(ua, (int32_t)((uint64_t)(t * 1000.0 + kClkA) & 0xFFFFu));
        bob.s.SendVehicleState(ub, (int32_t)((uint64_t)(t * 1000.0 + kClkB) & 0xFFFFu));
        if (tc < 0 && t >= tTouch) tc = t;
        rig.Pump(30);
    }
    auto impacts = [](const Probe& p) {
        std::vector<proto::VehicleImpact> r;
        for (const net::Event& e : p.events)
            if (e.type == net::Event::Type::VehicleImpact) r.push_back(e.impact);
        return r;
    };
    rig.Until([&] { return !impacts(alice).empty() && !impacts(bob).empty(); }, 1500);
    rig.Pump(300);   // anything more it would send (a second impact for the same contact) has arrived by now
    const std::vector<proto::VehicleImpact> ia = impacts(alice), ib = impacts(bob);
    const bool one = ia.size() == 1 && ib.size() == 1;
    double dvA = 0, dvB = 0, closing = 0, errA = 1e9, errB = 1e9;
    uint32_t seqSame = 0;
    bool ids = false;
    if (one)
    {
        dvA = ia[0].dvy; dvB = ib[0].dvy; closing = ia[0].closing;
        seqSame = ia[0].seq == ib[0].seq ? 1u : 0u;
        ids = ia[0].vehId == sa.vehId && ia[0].otherId == sb.vehId && ib[0].vehId == sb.vehId && ib[0].otherId == sa.vehId;
        // the contact time each was told, on its OWN clock, against the clock reading at the arbiter's box touch (15 ms before the geometric one)
        const double touchMs = (tTouch - 0.015) * 1000.0;
        errA = std::fabs(gage::sync::impact::UnwrapStampMs(ia[0].sampleMs, touchMs + kClkA) - (touchMs + kClkA));
        errB = std::fabs(gage::sync::impact::UnwrapStampMs(ib[0].sampleMs, touchMs + kClkB) - (touchMs + kClkB));
    }
    const server::Stats st = rig.srv.stats();
    Check("T4 net head-on through the real server: each driver gets exactly ONE VehicleImpact for the contact (the same seq), naming its own car and the other, with equal and opposite changes of velocity "
          "(about -24.5 / +24.5 m/s along the road), a closing speed near 40 m/s, and the contact time on its OWN clock within 70 ms (the clocks differ by 56 s); the server counted one",
          one && ids && seqSame == 1 && std::fabs(dvA + 24.5) < 3.0 && std::fabs(dvB - 24.5) < 3.0 && std::fabs(dvA + dvB) < 2.0 && std::fabs(closing - 40.0) < 4.0 && errA < 70.0 &&
              errB < 70.0 && st.vehicleImpacts == 1 && st.vehicleImpactsSent == 2,
          std::to_string(ia.size()) + "/" + std::to_string(ib.size()) + " impacts, dv " + std::to_string(dvA) + " / " + std::to_string(dvB) + ", closing " + std::to_string(closing) + ", clock error " +
              std::to_string(errA) + " / " + std::to_string(errB) + " ms, server " + std::to_string(st.vehicleImpacts) + " impacts " + std::to_string(st.vehicleImpactsSent) + " sent");

    // ---- R3 T5: LinkInfo, the server's measure of each vehicle controller's round trip, reaches every client (about once a second while cars are driven) ----
    {
        auto links = [](const Probe& p) {
            std::vector<proto::LinkInfo> r;
            for (const net::Event& e : p.events)
                if (e.type == net::Event::Type::LinkInfo) r.push_back(e.linkInfo);
            return r;
        };
        rig.Until([&] { return !links(alice).empty() && !links(bob).empty(); }, 2500);
        const std::vector<proto::LinkInfo> la = links(alice), lb = links(bob);
        auto has = [](const proto::LinkInfo& l, uint16_t id) {
            for (const proto::LinkEntry& e : l.e)
                if (e.id == id) return true;
            return false;
        };
        const bool both = !la.empty() && !lb.empty() && has(la.back(), alice.s.myId()) && has(la.back(), bob.s.myId()) && has(lb.back(), alice.s.myId()) && has(lb.back(), bob.s.myId());
        uint32_t maxRtt = 0;
        for (const proto::LinkEntry& e : (la.empty() ? proto::LinkInfo() : la.back()).e) maxRtt = std::max<uint32_t>(maxRtt, e.rttMs);
        Check("R3 T5 net: LinkInfo (message 44) reaches both clients through the real server while two cars are driven, names both controllers, with a loopback round trip under 200 ms; the server counted the sends",
              both && maxRtt < 200 && rig.srv.stats().linkInfoSent >= 2, std::to_string(la.size()) + "/" + std::to_string(lb.size()) + " messages, max rtt " + std::to_string(maxRtt) + " ms, sent " +
                  std::to_string(rig.srv.stats().linkInfoSent));
    }

    // ---- the speed cap: over it is clamped, far over it is still dropped ----
    {
        // Relative to the rig's (default) cap, so a change of the shipped default (150 -> 170 m/s on 2026-11-05) keeps the test meaning the same.
        const float cap = (float)server::Config().vehicleMaxSpeed;
        const server::Stats s0 = rig.srv.stats();
        proto::VehicleUpdate u;
        u.seq = ++seqA; u.vehId = sa.vehId; u.k = Kin(0.0f, ya0 + 40.0f, 30.0f, 0.0f, 0.0f, cap + 10.0f); u.rot = rotA; u.flags = proto::kVehEngine;
        alice.s.SendVehicleState(u, (int32_t)((uint64_t)(MsSince(tStart) + kClkA) & 0xFFFFu));
        rig.Until([&] { return rig.srv.stats().vehicleUpdatesAccepted > s0.vehicleUpdatesAccepted; }, 1500);
        const server::VehicleStore::Vehicle* v = rig.srv.FindVehicle(sa.vehId);
        const bool clamped = v && std::fabs(v->s.k.vy - cap) < 0.05f && std::fabs(v->s.k.y - (ya0 + 40.0f)) < 0.05f;
        proto::VehicleUpdate farUpd = u;
        farUpd.seq = ++seqA;
        farUpd.k = Kin(0.0f, ya0 + 41.0f, 30.0f, 0.0f, 0.0f, cap * 2.0f + 60.0f);   // over twice the cap: refused
        alice.s.SendVehicleState(farUpd, (int32_t)((uint64_t)(MsSince(tStart) + kClkA) & 0xFFFFu));
        rig.Until([&] { return rig.srv.stats().vehicleUpdatesInvalid > s0.vehicleUpdatesInvalid; }, 1500);
        const server::Stats s1 = rig.srv.stats();
        const server::VehicleStore::Vehicle* v2 = rig.srv.FindVehicle(sa.vehId);
        Check("T4 net speed cap: an update 10 m/s over the cap (a hyper car's overshoot) is ACCEPTED with its velocity clamped to the cap and its position taken; over twice the cap is still refused and counted invalid",
              clamped && s1.vehicleUpdatesAccepted == s0.vehicleUpdatesAccepted + 1 && s1.vehicleUpdatesInvalid == s0.vehicleUpdatesInvalid + 1 && v2 && std::fabs(v2->s.k.y - (ya0 + 40.0f)) < 0.05f &&
                  rig.srv.VehicleSpeedClamps() >= 1,
              std::string(clamped ? "clamped" : "not clamped") + ", " + std::to_string(s1.vehicleUpdatesAccepted - s0.vehicleUpdatesAccepted) + " accepted, " +
                  std::to_string(s1.vehicleUpdatesInvalid - s0.vehicleUpdatesInvalid) + " invalid");
    }
}

}   // namespace t4net
