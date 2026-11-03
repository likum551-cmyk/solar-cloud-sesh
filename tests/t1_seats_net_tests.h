// T1 seats (wire v6): the three-client scenario of tests/t1_seats_scenario.h against the REAL server over ENet, plus the wire rules the
// server enforces for the new ops. Included by netcode_test.cpp after its helpers (Rig / Probe / Check / IdOf / Place / VCmd).
// Each SimClient is fed by a real ClientSession's events (SeatNotice, VehicleResult, VehicleWorld seat lists), so the packets, the
// reliable ordering, the relevance filter and the server's own RunVehicleCmd / FlushSeatNotices are all the shipped code.
#pragma once

#include "t1_seats_scenario.h"

namespace t1net {

using t1sim::SimClient;

struct NetWorld
{
    Rig& rig;
    Probe* pr[3];
    std::vector<std::unique_ptr<SimClient>> cl;
    proto::Kinematics pos[3];
    uint16_t ids[3];
    size_t evSeen[3] = {0, 0, 0};
    Clock::time_point t0 = Clock::now();
    double nextStep = 0, nextSend = 0;
    uint32_t seq = 1;

    NetWorld(Rig& r, Probe& a, Probe& b, Probe& c) : rig(r)
    {
        pr[0] = &a;
        pr[1] = &b;
        pr[2] = &c;
        for (int i = 0; i < 3; ++i)
        {
            ids[i] = IdOf(*pr[i]);
            pos[i] = Kin(100.0f + (float)i, 100.0f, 30.0f);
            cl.emplace_back(new SimClient(ids[i], t1scn::ScenarioReplayCfg(), t1scn::ScenarioLocalCfg()));
            const int idx = i;
            cl.back()->send = [this, idx](const proto::VehicleCmd& cmd) { pr[idx]->s.SendVehicleCmd(cmd); };
        }
    }
    double Now() const { return (double)MsSince(t0); }
    SimClient& C(int i) { return *cl[i]; }
    uint16_t Id(int i) const { return ids[i]; }
    void Cmd(int idx, proto::VehicleOp op, uint16_t veh, uint32_t arg)
    {
        proto::VehicleCmd c;
        c.op = op;
        c.vehId = veh;
        c.arg = arg;
        C(idx).send(c);
    }
    uint16_t ServerSeat(uint16_t veh, int s)
    {
        const server::VehicleStore::Vehicle* v = rig.srv.FindVehicle(veh);
        return v && s >= 0 && s < (int)proto::kVehSeats ? v->s.seats[s] : proto::kNoPlayer;
    }
    bool ServerHasVeh(uint16_t veh) { return rig.srv.FindVehicle(veh) != nullptr; }

    // One pass of the loop: the server and the sessions (Rig::Step), the clients' inputs from their sessions' events, the states the server
    // needs from us to keep us in range (every 50 ms), and the clients' frames (every 16 ms).
    void Pump()
    {
        rig.Step();
        const double now = Now();
        if (now >= nextSend)
        {
            nextSend = now + 50.0;
            for (int i = 0; i < 3; ++i) pr[i]->SendKin(seq, pos[i]);
            ++seq;
        }
        for (int i = 0; i < 3; ++i)
        {
            Probe& p = *pr[i];
            while (evSeen[i] < p.events.size())
            {
                const net::Event& e = p.events[evSeen[i]++];
                if (e.type == ET::SeatNotice) C(i).OnNotice(e.seatNotice, now);
                else if (e.type == ET::VehicleResult) C(i).OnResult(e.vehicleResult);
            }
        }
        if (now >= nextStep)
        {
            nextStep = now + 16.0;
            for (int i = 0; i < 3; ++i)
            {
                SimClient& c = C(i);
                const Probe& p = *pr[i];
                for (const auto& kv : p.vehicles)
                {
                    uint16_t seats[proto::kVehSeats];
                    for (size_t s = 0; s < proto::kVehSeats; ++s) seats[s] = kv.second.seats[s];
                    c.SetSeats(kv.first, seats);
                }
                std::vector<int> drop;
                for (const auto& kv : c.seats)
                    if (!p.vehicles.count((uint16_t)kv.first)) drop.push_back(kv.first);
                for (int v : drop) c.VehicleGone(v);
                c.Step(now);
            }
        }
    }
    void Run(double ms)
    {
        const double end = Now() + ms;
        while (Now() < end) Pump();
    }
    template <class P>
    bool Until(P pred, double ms)
    {
        const double end = Now() + ms;
        if (pred()) return true;
        while (Now() < end)
        {
            Pump();
            if (pred()) return true;
        }
        return pred();
    }
};

constexpr int kNetChecks = t1scn::kScenarioChecks + 4;

static void SeatNetworkTests()
{
    Rig rig;
    if (!rig.Start()) { Check("seats net: server starts", false); return; }
    Probe a(rig), b(rig), c(rig), d(rig);
    if (!a.Join("SeatA") || !b.Join("SeatB") || !c.Join("SeatC") || !d.Join("SeatD")) { Check("seats net: four players join", false); return; }
    NetWorld w(rig, a, b, c);
    for (int i = 0; i < 3; ++i) Place(rig, *w.pr[i], w.pos[i]);
    t1scn::RunSeatScenario(w, "net", [](const char* n, bool ok, const std::string& g) { Check(n, ok, g); });

    // ---- the server's own accounting of that run ----
    const server::Stats& st = rig.srv.stats();
    Check("seats net: the server's counters match the script (seated from a reservation 5, cancelled 3, announced exits 2, intents refused 2: NotOwner and SeatTaken)",
          st.seatSeated == 5 && st.seatCancelled == 3 && st.seatExits == 2 && st.seatIntentsRefused == 2,
          t1scn::Fm("seated %llu cancelled %llu exits %llu refused %llu reserved %llu notices sent %llu", (unsigned long long)st.seatSeated,
                    (unsigned long long)st.seatCancelled, (unsigned long long)st.seatExits, (unsigned long long)st.seatIntentsRefused,
                    (unsigned long long)st.seatIntents, (unsigned long long)st.seatNoticesSent));
    Check("seats net: the script made 8 reservations and 18 notices (8 Reserved, 5 Seated, 3 Cancelled, 2 Exited); each of the three clients received all 18, the server sent 54",
          st.seatIntents == 8 && a.Count(ET::SeatNotice) == 18 && b.Count(ET::SeatNotice) == 18 && c.Count(ET::SeatNotice) == 18 && st.seatNoticesSent == 54,
          t1scn::Fm("reserved %llu notices A %d B %d C %d sent %llu", (unsigned long long)st.seatIntents, a.Count(ET::SeatNotice), b.Count(ET::SeatNotice),
                    c.Count(ET::SeatNotice), (unsigned long long)st.seatNoticesSent));

    // ---- the wire rules for the new ops ----
    const uint16_t aId = IdOf(a);
    Place(rig, d, Kin(103.0f, 100.0f, 30.0f));   // D stands on foot beside the fresh car (the script's three players all sit by now)
    // a fresh car for the checks that must not disturb the script
    proto::VehicleResult sp = VCmd(rig, a, proto::VehicleOp::Spawn, 0, t1scn::kModel);
    if (sp.code == proto::VehicleResultCode::RateLimited)
    {
        rig.Pump(1100);
        sp = VCmd(rig, a, proto::VehicleOp::Spawn, 0, t1scn::kModel);
    }
    const uint16_t v = sp.vehId;
    const proto::VehicleResult reserved = VCmd(rig, d, proto::VehicleOp::EnterIntent, v, 0x200u | 1u);   // reserved bit 9 in the argument
    const proto::VehicleResult badSeat = VCmd(rig, d, proto::VehicleOp::EnterIntent, v, 8u);
    const proto::VehicleResult noCar = VCmd(rig, d, proto::VehicleOp::EnterIntent, 250, 1u);
    const proto::VehicleResult fine = VCmd(rig, d, proto::VehicleOp::EnterIntent, v, 1u | proto::kIntentRun);
    const proto::VehicleResult cancel = VCmd(rig, d, proto::VehicleOp::CancelIntent, v, 0);
    const proto::VehicleResult cancelAgain = VCmd(rig, d, proto::VehicleOp::CancelIntent, v, 0);
    Check("seats net: EnterIntent with a reserved argument bit or seat 8 is BadArg, on a missing car NoVehicle, a good one Ok; CancelIntent is Ok and idempotent",
          sp.code == proto::VehicleResultCode::Ok && reserved.code == proto::VehicleResultCode::BadArg && badSeat.code == proto::VehicleResultCode::BadArg &&
              noCar.code == proto::VehicleResultCode::NoVehicle && fine.code == proto::VehicleResultCode::Ok &&
              cancel.code == proto::VehicleResultCode::Ok && cancelAgain.code == proto::VehicleResultCode::Ok,
          t1scn::Fm("%s %s %s %s %s %s", proto::ToString(sp.code), proto::ToString(reserved.code), proto::ToString(badSeat.code), proto::ToString(noCar.code),
                    proto::ToString(fine.code), proto::ToString(cancel.code)));
    (void)aId;

    // a client may not send a server-to-client message: a SeatNotice from a client is a protocol violation
    bool refused = false;
    {
        proto::SeatNotice n;
        n.kind = proto::SeatNoticeKind::Seated;
        n.player = 0;
        n.vehId = 0;
        n.seat = 1;
        d.SendReliable(proto::Encode(n));
        refused = rig.Until([&] { return d.RefusedWith(proto::RefuseReason::Protocol) || d.Closed(); }, 2000);
    }
    Check("seats net: a SeatNotice sent BY a client is refused as a protocol violation (it is server-to-client only)", refused);
}

}   // namespace t1net
