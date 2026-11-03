// T1 seats (desync overhaul round 2, wire v6): the pure tests, included by gameplay_test.cpp after Check() / F() are defined.
//   * the units of libs/sync/seat_intent.h (EnterTaskWatch, SeatBook, PuppetSeatReplay, LocalEnterFlow, SeatClientModel),
//   * the server's reservation rules (apps/server/vehicles.h VehicleStore::Reserve / Enter / Exit / Destroy / Tick and the
//     SeatNotices they queue),
//   * the SeatNotice codec and the EnterIntent argument,
//   * the scripted three-client scenario (tests/t1_seats_scenario.h) against the real VehicleStore behind a simulated relay with
//     40 ms of latency each way, and a "legacy" client (no notices: warp when the list names the player, teleport-in on F) that the
//     same predicates must REJECT (the fails-on-old proof of the scenario's assertions).
// The same scenario runs against the real server over ENet in netcode_test (tests/t1_seats_net_tests.h).
#pragma once

#include "../apps/server/vehicles.h"
#include "protocol/wire.h"
#include "t1_seats_scenario.h"

#include <memory>
#include <queue>

namespace t1 {

using namespace t1sim;
using t1scn::Fm;
namespace srv = gage::server;

constexpr int kPureChecks = 56;   // asserted by gameplay_test's total

// ---- the simulated relay ---------------------------------------------------------------------------------------------

struct PureWorld
{
    srv::VehicleStore store;
    std::vector<std::unique_ptr<SimClient>> cl;
    std::vector<proto::Kinematics> pos;
    double now = 0, latMs = 40, nextStep = 0, nextTick = 0;
    uint32_t tick = 1;
    struct Ev { double at; uint64_t seq; std::function<void()> fn; };
    struct Later { bool operator()(const Ev& a, const Ev& b) const { return a.at != b.at ? a.at > b.at : a.seq > b.seq; } };
    std::priority_queue<Ev, std::vector<Ev>, Later> q;
    uint64_t seq = 0;
    bool legacy = false;   // clients ignore SeatNotices and teleport (the pre-T1 behaviour)

    explicit PureWorld(int n, const sync::SeatReplayCfg& rc = t1scn::ScenarioReplayCfg(), const sync::LocalEnterCfg& lc = t1scn::ScenarioLocalCfg())
    {
        srv::VehicleStore::Rules r;
        r.emptyDespawnMs = 0;
        store.Configure(r, 16);
        for (int i = 0; i < n; ++i)
        {
            store.AddReceiver((uint16_t)i);
            cl.emplace_back(new SimClient((uint16_t)i, rc, lc));
            proto::Kinematics k;
            k.x = 100.0f + (float)i;
            k.y = 100.0f;
            k.z = 30.0f;
            pos.push_back(k);
            const int idx = i;
            cl.back()->send = [this, idx](const proto::VehicleCmd& c) { At(now + latMs, [this, idx, c] { Handle(idx, c); }); };
        }
    }
    double Now() const { return now; }
    SimClient& C(int i) { return *cl[i]; }
    uint16_t Id(int i) { return (uint16_t)i; }
    void At(double t, std::function<void()> fn) { q.push(Ev{t, seq++, std::move(fn)}); }
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
        const srv::VehicleStore::Vehicle* v = store.Get(veh);
        return v && s >= 0 && s < (int)proto::kVehSeats ? v->s.seats[s] : proto::kNoPlayer;
    }
    bool ServerHasVeh(uint16_t veh) { return store.Get(veh) != nullptr; }

    // The server's RunVehicleCmd for the ops the scenario uses (apps/server/server.cpp), then the answer and the notices.
    void Handle(int idx, const proto::VehicleCmd& m)
    {
        using Code = proto::VehicleResultCode;
        const uint64_t nowMs = (uint64_t)now;
        const uint16_t id = (uint16_t)idx;
        uint16_t vehId = m.vehId;
        Code code = Code::BadArg;
        switch (m.op)
        {
        case proto::VehicleOp::Spawn:
        {
            proto::Kinematics at = pos[idx];
            at.y += 4.0f;
            uint16_t nid = 0;
            code = store.Create(m.arg, at, proto::QuatFromHeading(0.0f), 0, 0, id, 0, false, nowMs, tick, &nid);
            vehId = nid;
            break;
        }
        case proto::VehicleOp::Despawn:
        {
            const srv::VehicleStore::Vehicle* v = store.Get(m.vehId);
            if (!v) code = Code::NoVehicle;
            else if (v->spawner != id) code = Code::NotAllowed;
            else { store.Destroy(m.vehId, nowMs, tick, nullptr); code = Code::Ok; }
            break;
        }
        case proto::VehicleOp::Enter: code = store.Enter(id, m.vehId, m.arg, pos[idx], nowMs, tick); break;
        case proto::VehicleOp::Exit: code = store.Exit(id, m.vehId, nowMs, tick, true); break;
        case proto::VehicleOp::EnterIntent:
            code = (m.arg & ~proto::kIntentArgMask) ? Code::BadArg
                   : store.Reserve(id, m.vehId, m.arg & 0xFF, (m.arg & proto::kIntentRun) != 0, pos[idx], nowMs, tick);
            break;
        case proto::VehicleOp::CancelIntent: code = store.CancelIntent(id, m.vehId); break;
        default: break;
        }
        proto::VehicleResult res;
        res.op = m.op;
        res.code = code;
        res.vehId = vehId;
        At(now + latMs, [this, idx, res] { C(idx).OnResult(res); });
        Flush();
    }
    void Flush()
    {
        for (const proto::SeatNotice& n : store.TakeNotices())
            for (size_t i = 0; i < cl.size(); ++i)
                At(now + latMs, [this, i, n] {
                    if (!legacy) C((int)i).OnNotice(n, now);
                });
    }
    void ServerTick()
    {
        std::vector<uint16_t> gone;
        store.Tick((uint64_t)now, tick, &gone);
        Flush();
        // the VehicleWorld of this tick: every existing vehicle's seat list, and the removal of the ones that are gone
        std::vector<std::pair<int, std::array<uint16_t, proto::kVehSeats>>> alive;
        for (int v = 0; v < (int)proto::kMaxVehicles; ++v)
            if (const srv::VehicleStore::Vehicle* sv = store.Get((uint16_t)v))
            {
                std::array<uint16_t, proto::kVehSeats> a;
                for (size_t s = 0; s < proto::kVehSeats; ++s) a[s] = sv->s.seats[s];
                alive.push_back({v, a});
            }
        for (size_t i = 0; i < cl.size(); ++i)
            At(now + latMs, [this, i, alive] {
                SimClient& c = C((int)i);
                std::set<int> have;
                for (const auto& p : alive)
                {
                    c.SetSeats(p.first, p.second.data());
                    have.insert(p.first);
                }
                std::vector<int> drop;
                for (const auto& kv : c.seats)
                    if (!have.count(kv.first)) drop.push_back(kv.first);
                for (int v : drop) c.VehicleGone(v);
            });
        ++tick;
    }
    void Run(double ms)
    {
        const double end = now + ms;
        while (now < end)
        {
            now += 4.0;
            while (!q.empty() && q.top().at <= now)
            {
                Ev e = q.top();
                q.pop();
                e.fn();
            }
            if (now >= nextTick) { nextTick = now + 33.0; ServerTick(); }
            if (now >= nextStep)
            {
                nextStep = now + 16.0;
                for (auto& c : cl) c->Step(now);
            }
        }
    }
    template <class P>
    bool Until(P pred, double ms)
    {
        const double end = now + ms;
        if (pred()) return true;
        while (now < end)
        {
            Run(8.0);
            if (pred()) return true;
        }
        return pred();
    }
};

// ---- units -----------------------------------------------------------------------------------------------------------

inline proto::SeatNotice Notice(proto::SeatNoticeKind k, uint16_t player, uint16_t veh, uint8_t seat, uint8_t detail = 0)
{
    proto::SeatNotice n;
    n.kind = k;
    n.player = player;
    n.vehId = veh;
    n.seat = seat;
    n.detail = detail;
    return n;
}

inline void TestWire()
{
    // the argument
    Check("wire: EnterIntent.arg packs the seat in bits 0..7 and the run flag in bit 8, nothing else",
          sync::PackIntentArg(5, true) == 0x105u && sync::PackIntentArg(0, false) == 0u && (sync::PackIntentArg(7, true) & ~proto::kIntentArgMask) == 0 &&
              proto::kVehicleOpLast == (uint8_t)proto::VehicleOp::CancelIntent);
    // the codec: every kind round-trips; every undefined field combination is refused by Encode and Decode
    bool rt = true;
    for (const proto::SeatNotice& n : {Notice(proto::SeatNoticeKind::Reserved, 7, 12, 3, proto::kSeatNoticeRun), Notice(proto::SeatNoticeKind::Reserved, 1023, 255, 7),
                                       Notice(proto::SeatNoticeKind::Seated, 7, 12, 3), Notice(proto::SeatNoticeKind::Cancelled, 7, 12, 3, proto::kSeatCancelLast),
                                       Notice(proto::SeatNoticeKind::Exited, 0, 0, 0)})
    {
        const proto::Bytes b = proto::Encode(n);
        proto::SeatNotice o;
        rt = rt && b.size() == 8 && proto::Decode(b.data(), b.size(), o) && o.kind == n.kind && o.player == n.player && o.vehId == n.vehId &&
             o.seat == n.seat && o.detail == n.detail;
    }
    Check("wire: SeatNotice is 8 bytes and round-trips for every kind (player 1023, vehicle 255, seat 7 included)", rt);
    int accepted = 0, encoded = 0;
    for (const proto::SeatNotice& n : {Notice((proto::SeatNoticeKind)0, 1, 1, 1), Notice((proto::SeatNoticeKind)5, 1, 1, 1), Notice(proto::SeatNoticeKind::Seated, 1024, 1, 1),
                                       Notice(proto::SeatNoticeKind::Seated, 1, 256, 1), Notice(proto::SeatNoticeKind::Seated, 1, 1, 8),
                                       Notice(proto::SeatNoticeKind::Reserved, 1, 1, 1, 2), Notice(proto::SeatNoticeKind::Cancelled, 1, 1, 1, 5),
                                       Notice(proto::SeatNoticeKind::Seated, 1, 1, 1, 1), Notice(proto::SeatNoticeKind::Exited, 1, 1, 1, 1)})
    {
        if (!proto::Encode(n).empty()) ++encoded;
        // the same bytes built by hand, as a hostile sender would
        gage::wire::Writer w;
        w.u8((uint8_t)proto::Msg::SeatNotice);
        w.u8((uint8_t)n.kind);
        w.u16(n.player);
        w.u16(n.vehId);
        w.u8(n.seat);
        w.u8(n.detail);
        const proto::Bytes b = w.take();
        proto::SeatNotice o;
        if (proto::Decode(b.data(), b.size(), o)) ++accepted;
    }
    Check("wire: Encode and Decode both refuse kind 0 and 5, player 1024, vehicle 256, seat 8 and a detail the kind does not define (9 hostile notices)",
          accepted == 0 && encoded == 0, Fm("decoded %d encoded %d", accepted, encoded));
}

inline void TestStore()
{
    using Code = proto::VehicleResultCode;
    using K = proto::SeatNoticeKind;
    srv::VehicleStore st;
    srv::VehicleStore::Rules r;
    r.emptyDespawnMs = 0;
    st.Configure(r, 8);
    for (uint16_t p = 0; p < 4; ++p) st.AddReceiver(p);
    proto::Kinematics at;
    at.x = 100; at.y = 100; at.z = 30;
    proto::Kinematics near1 = at, far1 = at;
    near1.x += 3;
    far1.x += 80;
    uint16_t v1 = 0, v2 = 0;
    st.Create(0xAAAA, at, proto::QuatFromHeading(0), 0, 0, 3 /*owner*/, 0, false, 1000, 1, &v1);
    st.Create(0xBBBB, at, proto::QuatFromHeading(0), 0, 0, proto::kNoPlayer, 0, true, 1000, 1, &v2);   // adopted: not owned
    uint64_t t = 1000;
    // reserve
    Check("store: Reserve answers Ok, queues exactly one Reserved notice (player, car, seat, run flag) and seats NOBODY",
          st.Reserve(0, v1, 1, true, near1, t, 2) == Code::Ok && st.PendingNotices() == 1 && st.Get(v1)->s.seats[1] == proto::kNoPlayer &&
              st.ReservedBy(v1, 1, t) == 0);
    {
        const std::vector<proto::SeatNotice> n = st.TakeNotices();
        Check("store: that notice is Reserved / player 0 / seat 1 / run", n.size() == 1 && n[0].kind == K::Reserved && n[0].player == 0 && n[0].vehId == v1 &&
                                                                              n[0].seat == 1 && n[0].detail == proto::kSeatNoticeRun);
    }
    Check("store: the same reservation again just renews it (Ok, no second notice)", st.Reserve(0, v1, 1, true, near1, t + 500, 3) == Code::Ok && st.PendingNotices() == 0);
    // conflicts
    Check("store: another player's Reserve AND Enter of the held seat are SeatTaken while the first walks in; a free seat is not affected",
          st.Reserve(1, v1, 1, false, near1, t + 600, 3) == Code::SeatTaken && st.Enter(1, v1, 1, near1, t + 600, 3) == Code::SeatTaken &&
              st.Reserve(1, v1, 2, false, near1, t + 600, 3) == Code::Ok);
    st.TakeNotices();
    // enter converts
    Check("store: the holder's Enter takes the seat, queues Seated (not Cancelled) and the reservation is gone",
          st.Enter(0, v1, 1, near1, t + 700, 4) == Code::Ok && st.Get(v1)->s.seats[1] == 0 && st.ReservedBy(v1, 1, t + 700) == proto::kNoPlayer &&
              [&] {
                  const auto n = st.TakeNotices();
                  return n.size() == 1 && n[0].kind == K::Seated && n[0].player == 0 && n[0].seat == 1;
              }());
    // replace
    Check("store: a new Reserve by the same player cancels the old one (Cancelled/Replaced for seat 2, then Reserved for seat 3) and frees seat 2",
          st.Reserve(1, v1, 3, false, near1, t + 800, 5) == Code::Ok && st.ReservedBy(v1, 2, t + 800) == proto::kNoPlayer &&
              [&] {
                  const auto n = st.TakeNotices();
                  return n.size() == 2 && n[0].kind == K::Cancelled && n[0].seat == 2 && n[0].detail == (uint8_t)proto::SeatCancelReason::Replaced &&
                         n[1].kind == K::Reserved && n[1].seat == 3;
              }());
    // the other refusals
    Check("store: owner rule (driver seat of an owned car: NotOwner, no notice), range (seat 8 BadArg), distance (TooFar), lock, seated-elsewhere (NotAllowed), no car",
          st.Reserve(1, v1, 0, false, near1, t + 900, 6) == Code::NotOwner && st.Reserve(2, v1, 8, false, near1, t + 900, 6) == Code::BadArg &&
              st.Reserve(2, v1, 4, false, far1, t + 900, 6) == Code::TooFar && st.Reserve(0, v1, 4, false, near1, t + 900, 6) == Code::NotAllowed &&
              st.Reserve(2, 200, 1, false, near1, t + 900, 6) == Code::NoVehicle && st.PendingNotices() == 0 &&
              st.Reserve(3, v1, 0, false, near1, t + 900, 6) == Code::Ok /* the owner may reserve the driver seat */);
    st.TakeNotices();
    st.SetFlag(3, v1, proto::kVehLocked, true, 7);
    Check("store: a locked car refuses a stranger's Reserve (Locked) and lets its spawner reserve",
          st.Reserve(2, v1, 4, false, near1, t + 950, 8) == Code::Locked && st.Reserve(3, v1, 0, false, near1, t + 950, 8) == Code::Ok);
    st.SetFlag(3, v1, proto::kVehLocked, false, 9);
    st.CancelIntent(3, v1);
    st.TakeNotices();
    // exit while pending, and the announced Exit
    Check("store: Exit while only a reservation is held answers Ok and queues Cancelled/ByPlayer; CancelIntent with nothing held is Ok and silent",
          st.Exit(1, v1, t + 1000, 10, true) == Code::Ok && st.PendingIntent(1).veh == srv::VehicleStore::kNone &&
              [&] {
                  const auto n = st.TakeNotices();
                  return n.size() == 1 && n[0].kind == K::Cancelled && n[0].detail == (uint8_t)proto::SeatCancelReason::ByPlayer && n[0].seat == 3;
              }() &&
              st.CancelIntent(1, v1) == Code::Ok && st.PendingNotices() == 0);
    Check("store: an announced Exit queues Exited with the seat it left; the same Exit as a death (announce false) queues nothing",
          st.Exit(0, v1, t + 1100, 11, true) == Code::Ok && [&] {
              const auto n = st.TakeNotices();
              return n.size() == 1 && n[0].kind == K::Exited && n[0].player == 0 && n[0].seat == 1;
          }() && st.Enter(0, v1, 2, near1, t + 1200, 12) == Code::Ok && st.Exit(0, v1, t + 1300, 13, false) == Code::Ok && st.PendingNotices() == 0);
    // expiry
    st.Reserve(1, v1, 5, false, near1, t + 2000, 14);
    st.TakeNotices();
    st.Tick(t + 2000 + proto::kIntentTtlMs - 1, 15, nullptr);
    const bool heldBefore = st.ReservedBy(v1, 5, t + 2000 + proto::kIntentTtlMs - 1) == 1 && st.PendingNotices() == 0;
    st.Tick(t + 2000 + proto::kIntentTtlMs, 16, nullptr);
    Check("store: a reservation the player never completes is held for kIntentTtlMs, then dropped by Tick with Cancelled/Expired and the seat is free for others",
          heldBefore && st.PendingIntent(1).veh == srv::VehicleStore::kNone && st.Reserve(2, v1, 5, false, near1, t + 9000, 17) == Code::Ok &&
              [&] {
                  const auto n = st.TakeNotices();
                  return n.size() == 2 && n[0].kind == K::Cancelled && n[0].detail == (uint8_t)proto::SeatCancelReason::Expired;
              }());
    // enter a different seat than reserved
    st.TakeNotices();
    Check("store: Entering another seat than the reserved one cancels the reservation (Replaced) and does not announce Seated",
          st.Enter(2, v1, 6, near1, t + 9100, 18) == Code::Ok && st.ReservedBy(v1, 5, t + 9100) == proto::kNoPlayer &&
              [&] {
                  const auto n = st.TakeNotices();
                  return n.size() == 1 && n[0].kind == K::Cancelled && n[0].detail == (uint8_t)proto::SeatCancelReason::Replaced && n[0].seat == 5;
              }());
    // destroy / disconnect / death
    st.Exit(2, v1, t + 9200, 19, false);
    st.Reserve(1, v2, 1, false, near1, t + 9300, 20);
    st.TakeNotices();
    st.Destroy(v2, t + 9400, 21, nullptr);
    Check("store: destroying the car cancels what was on the way into it (Cancelled/VehicleGone)",
          st.PendingIntent(1).veh == srv::VehicleStore::kNone && [&] {
              const auto n = st.TakeNotices();
              return n.size() == 1 && n[0].kind == K::Cancelled && n[0].detail == (uint8_t)proto::SeatCancelReason::VehicleGone && n[0].vehId == v2;
          }());
    st.Reserve(1, v1, 6, false, near1, t + 9500, 22);
    st.TakeNotices();
    st.CancelIntentOf(1, proto::SeatCancelReason::PlayerGone);
    const auto death = st.TakeNotices();
    st.Reserve(2, v1, 6, false, near1, t + 9600, 23);
    st.TakeNotices();
    st.PlayerGone(2, t + 9700, 24);
    const auto leave = st.TakeNotices();
    Check("store: a death/respawn (CancelIntentOf) and a disconnect (PlayerGone) cancel the walk with Cancelled/PlayerGone and free the seat",
          st.ReservedBy(v1, 6, t + 9700) == proto::kNoPlayer && death.size() == 1 && death[0].kind == K::Cancelled && death[0].player == 1 &&
              death[0].detail == (uint8_t)proto::SeatCancelReason::PlayerGone && leave.size() == 1 && leave[0].kind == K::Cancelled &&
              leave[0].detail == (uint8_t)proto::SeatCancelReason::PlayerGone && leave[0].player == 2);
}

inline void TestAudience()
{
    // Who is told: the actor plus every receiver that had the car in range when the notice was queued - also for the car's own destruction.
    srv::VehicleStore st;
    srv::VehicleStore::Rules r;
    r.emptyDespawnMs = 0;
    st.Configure(r, 8);
    for (uint16_t p = 0; p < 4; ++p) st.AddReceiver(p);
    proto::Kinematics at;
    at.x = 100; at.y = 100; at.z = 30;
    uint16_t v = 0;
    st.Create(0xAAAA, at, proto::QuatFromHeading(0), 0, 0, 3, 0, false, 1000, 1, &v);
    proto::VehicleWorld fw;
    st.BuildFrame(1, true, at, 5, 5000, fw);       // receiver 1 has the car in range; 2 and 3 never built a frame
    st.Reserve(0, v, 1, false, at, 5000, 5);
    st.Destroy(v, 5100, 6, nullptr);
    std::vector<std::vector<uint16_t>> aud;
    const std::vector<proto::SeatNotice> n = st.TakeNotices(&aud);
    const std::vector<uint16_t> want = {0, 1};
    Check("store: a notice goes to the actor and to the receivers that had the car in range when it was queued - the car's destruction included (2 and 3 are not told)",
          n.size() == 2 && aud.size() == 2 && n[0].kind == proto::SeatNoticeKind::Reserved && n[1].kind == proto::SeatNoticeKind::Cancelled &&
              n[1].detail == (uint8_t)proto::SeatCancelReason::VehicleGone && aud[0] == want && aud[1] == want,
          aud.size() == 2 ? Fm("audiences %zu / %zu", aud[0].size(), aud[1].size()) : "");
}

inline void TestReplay()
{
    using sync::SeatAct;
    using sync::WarpWhy;
    const sync::SeatReplayCfg cfg;
    auto in = [](double now, bool veh = true, bool inSeat = false, bool trying = false) {
        sync::SeatReplayIn i;
        i.nowMs = now;
        i.pedReady = true;
        i.vehReady = veh;
        i.inSeat = inSeat;
        i.tryingToEnter = trying;
        return i;
    };
    // normal: issued once, the task owns the ped, seated by the game, zero warps
    {
        sync::PuppetSeatReplay r(cfg);
        r.OnReserve(5, 2, true, 1000);
        const sync::SeatReplayOut a = r.Tick(in(1016));
        const sync::SeatReplayOut b = r.Tick(in(1032, true, false, true));
        const sync::SeatReplayOut c = r.Tick(in(2000, true, false, true));
        const sync::SeatReplayOut d = r.Tick(in(2500, true, true, false));
        Check("replay: Reserved -> ONE IssueEnter (seat 2 of car 5, run, door limit, ped held), no second issue, finished by the game (no warp)",
              a.act == SeatAct::IssueEnter && a.veh == 5 && a.seat == 2 && a.run && a.timeoutMs == (int)cfg.door.maxMs && a.holdsPed && b.act == SeatAct::None &&
                  b.holdsPed && c.act == SeatAct::None && c.holdsPed && d.act == SeatAct::None && !d.holdsPed && r.phase() == sync::PuppetSeatReplay::Phase::Done &&
                  r.stats().issued == 1 && r.stats().started == 1 && r.stats().finished == 1 && r.stats().Warps() == 0);
    }
    // the seat warp waits for the grace after the server's word, and never past it
    {
        sync::PuppetSeatReplay r(cfg);
        r.OnReserve(5, 2, false, 1000);
        r.Tick(in(1016));
        r.Tick(in(1100, true, false, true));
        r.OnSeated(5, 2, 1500);
        const sync::WarpDecision d0 = r.AllowsSeatWarp(5, 2, 1500);
        r.Tick(in(2000, true, false, true));
        const sync::WarpDecision d1 = r.AllowsSeatWarp(5, 2, 1500 + cfg.confirmGraceMs - 1);
        const sync::WarpDecision d2 = r.AllowsSeatWarp(5, 2, 1500 + cfg.confirmGraceMs);
        Check("replay: the list names the player while the task still runs: NO warp until confirmGraceMs after the server's Seated, then one (GraceOver)",
              !d0.allow && !d1.allow && d2.allow && d2.why == WarpWhy::GraceOver);
        r.NoteWarp(d2.why);
        Check("replay: that warp is counted (GraceOver) and ends the intent (Done)", r.stats().warpGraceOver == 1 && r.phase() == sync::PuppetSeatReplay::Phase::Done);
    }
    // a task that ends without a seat: after the debounce the warp is allowed once the list names the player
    {
        sync::PuppetSeatReplay r(cfg);
        r.OnReserve(5, 2, false, 1000);
        r.Tick(in(1016));
        r.Tick(in(1100, true, false, true));
        r.Tick(in(1300, true, false, false));
        const sync::SeatReplayOut mid = r.Tick(in(1300 + cfg.door.endedDebounceMs - 1, true, false, false));
        const sync::SeatReplayOut end = r.Tick(in(1300 + cfg.door.endedDebounceMs, true, false, false));
        r.OnSeated(5, 2, 1800);
        const sync::WarpDecision d = r.AllowsSeatWarp(5, 2, 1810);
        Check("replay: a task that was running and stopped without a seat is given endedDebounceMs, then the ped is released to the locomotion (ClearTask) and the warp is allowed (TaskEnded) once the list names it",
              mid.holdsPed && !end.holdsPed && end.act == SeatAct::ClearTask && r.stats().endedNoSeat == 1 && d.allow && d.why == WarpWhy::TaskEnded);
    }
    // never started / timed out
    {
        sync::PuppetSeatReplay r(cfg);
        r.OnReserve(5, 2, false, 1000);
        r.Tick(in(1016));
        const sync::SeatReplayOut early = r.Tick(in(1016 + cfg.door.startWaitMs - 1));
        r.Tick(in(1016 + cfg.door.startWaitMs));
        sync::PuppetSeatReplay s(cfg);
        s.OnReserve(5, 2, false, 1000);
        s.Tick(in(1016));
        s.Tick(in(1100, true, false, true));
        const sync::SeatReplayOut live = s.Tick(in(1016 + cfg.door.maxMs - 1, true, false, true));
        s.Tick(in(1016 + cfg.door.maxMs, true, false, true));
        Check("replay: never started -> NeverStarted at startWaitMs (not before); started but never finishing -> TimedOut at the door limit (not before)",
              early.holdsPed && r.stats().neverStarted == 1 && live.holdsPed && s.stats().timeouts == 1 && s.stats().started == 1);
    }
    // cancel clears the task exactly once
    {
        sync::PuppetSeatReplay r(cfg);
        r.OnReserve(5, 2, false, 1000);
        r.Tick(in(1016));
        r.Tick(in(1100, true, false, true));
        r.OnCancel(5, 2, 1200);
        const sync::SeatReplayOut a = r.Tick(in(1216, true, false, true));
        const sync::SeatReplayOut b = r.Tick(in(1232));
        Check("replay: Cancelled mid-task -> exactly one ClearTask, then nothing (Idle); a Cancelled for another seat changes nothing",
              a.act == SeatAct::ClearTask && b.act == SeatAct::None && r.phase() == sync::PuppetSeatReplay::Phase::Idle && r.stats().cancelled == 1 &&
                  [&] {
                      sync::PuppetSeatReplay q(cfg);
                      q.OnReserve(5, 2, false, 1000);
                      q.OnCancel(5, 3, 1100);
                      return q.phase() == sync::PuppetSeatReplay::Phase::Pending;
                  }());
    }
    // a newer intent replaces a running one
    {
        sync::PuppetSeatReplay r(cfg);
        r.OnReserve(5, 2, false, 1000);
        r.Tick(in(1016));
        r.Tick(in(1100, true, false, true));
        r.OnReserve(5, 3, false, 1200);
        const sync::SeatReplayOut a = r.Tick(in(1216, true, false, true));
        const sync::SeatReplayOut b = r.Tick(in(1232));
        Check("replay: a new Reserved while a task runs clears the old task first, then issues the new seat (2 issues in all)",
              a.act == SeatAct::ClearTask && b.act == SeatAct::IssueEnter && b.seat == 3 && r.stats().issued == 2 && r.stats().intents == 2);
    }
    // exit latch
    {
        sync::PuppetSeatReplay r(cfg);
        r.OnExit(5, 1000);
        const sync::WarpDecision a = r.AllowsSeatWarp(5, 2, 1000 + cfg.exitLatchMs - 1);
        const sync::WarpDecision b = r.AllowsSeatWarp(5, 2, 1000 + cfg.exitLatchMs);
        const sync::WarpDecision other = r.AllowsSeatWarp(6, 2, 1100);
        r.OnExit(5, 3000);
        r.OnReserve(5, 2, false, 3100);
        const sync::WarpDecision afterNew = r.AllowsSeatWarp(5, 3, 3200);
        Check("replay: after Exited a stale seat list may not re-seat the ped for exitLatchMs (that car only), then it may; a new Reserved ends the latch",
              !a.allow && b.allow && other.allow && afterNew.allow && r.stats().latchDenied == 1 && r.stats().exits == 2);
    }
    // the car never arrives; no intent at all; unlisted seating
    {
        sync::PuppetSeatReplay r(cfg);
        r.OnReserve(5, 2, false, 1000);
        r.Tick(in(1100, false));
        r.Tick(in(1000 + cfg.intentWaitMs, false));
        const sync::WarpDecision d = r.AllowsSeatWarp(5, 2, 5000);
        sync::PuppetSeatReplay n(cfg);
        const sync::WarpDecision nd = n.AllowsSeatWarp(5, 2, 100);
        Check("replay: an intent whose car never appears is dropped after intentWaitMs (the list then seats the ped), and a player with no intent at all is warped as before",
              r.stats().droppedNotReady == 1 && d.allow && nd.allow && nd.why == WarpWhy::NoIntent);
        sync::PuppetSeatReplay u(cfg);
        u.OnReserve(5, 2, false, 1000);
        u.Tick(in(1016));
        u.Tick(in(1100, true, false, true));
        u.Tick(in(1800, true, true, false));
        sync::SeatReplayIn li = in(1800 + cfg.unlistedMs - 1, true, true, false);
        const sync::SeatReplayOut e0 = u.Tick(li);
        const sync::SeatReplayOut e1 = u.Tick(in(1800 + cfg.unlistedMs, true, true, false));
        sync::PuppetSeatReplay l(cfg);
        l.OnReserve(5, 2, false, 1000);
        l.Tick(in(1016));
        l.Tick(in(1100, true, false, true));
        l.Tick(in(1800, true, true, false));
        sync::SeatReplayIn listed = in(2000, true, true, false);
        listed.listedSomewhere = true;
        l.Tick(listed);
        const sync::SeatReplayOut l1 = l.Tick(in(1800 + cfg.unlistedMs + 100, true, true, false));
        Check("replay: seated by the task but never named by the server's list -> Evict after unlistedMs (not before); named in time -> never evicted",
              e0.act == SeatAct::None && e1.act == SeatAct::Evict && u.stats().evicted == 1 && l1.act == SeatAct::None && l.stats().evicted == 0);
    }
    // a bike uses the mount limits
    {
        sync::PuppetSeatReplay r(cfg);
        r.OnReserve(5, 1, false, 1000);
        sync::SeatReplayIn i = in(1016);
        i.mount = true;
        const sync::SeatReplayOut a = r.Tick(i);
        Check("replay: a bike or cycle (class 8 / 13) is issued with the mount time limit and counted",
              a.act == SeatAct::IssueEnter && a.timeoutMs == (int)cfg.mount.maxMs && r.stats().mounts == 1);
    }
}

inline void TestLocalFlow()
{
    using sync::LocalAct;
    using sync::LocalWhy;
    const sync::LocalEnterCfg cfg;
    auto in = [](double now, bool seated = false, bool trying = false, bool abort = false, bool mount = false) {
        sync::LocalEnterIn i;
        i.nowMs = now;
        i.seatedNow = seated;
        i.tryingToEnter = trying;
        i.abort = abort;
        i.mount = mount;
        return i;
    };
    {
        sync::LocalEnterFlow f(cfg);
        f.Begin(9, 1, true, 1000);
        const sync::LocalEnterOut a = f.Tick(in(1000));
        const sync::LocalEnterOut wait = f.Tick(in(1016));
        f.OnResult(9, 0);
        const sync::LocalEnterOut go = f.Tick(in(1100));
        const sync::LocalEnterOut run = f.Tick(in(1200, false, true));
        const sync::LocalEnterOut done = f.Tick(in(2000, true, false));
        Check("local: F -> SendIntent once; nothing starts until the server says Ok; then ONE IssueEnter (door limit); seated by the game -> done, no warp",
              a.act == LocalAct::SendIntent && wait.act == LocalAct::None && go.act == LocalAct::IssueEnter && go.timeoutMs == (int)cfg.door.maxMs &&
                  run.act == LocalAct::None && done.act == LocalAct::None && f.phase() == sync::LocalEnterFlow::Phase::Done && f.stats().fallbackWarps == 0 &&
                  f.stats().issued == 1 && f.stats().finished == 1);
    }
    {
        sync::LocalEnterFlow f(cfg);
        f.Begin(9, 1, true, 1000);
        f.Tick(in(1000));
        f.OnResult(9, 2);
        const sync::LocalEnterOut r = f.Tick(in(1100));
        const sync::LocalEnterOut after = f.Tick(in(1200));
        Check("local: a refusal (SeatTaken 2) starts nothing and reports the code; an Ok for a different car is ignored",
              r.why == LocalWhy::Refused && r.refusal == 2 && after.act == LocalAct::None && f.stats().issued == 0 && [&] {
                  sync::LocalEnterFlow g(cfg);
                  g.Begin(9, 1, true, 0);
                  g.Tick(in(0));
                  g.OnResult(8, 0);
                  return g.Tick(in(100)).act == LocalAct::None && g.phase() == sync::LocalEnterFlow::Phase::AwaitAnswer;
              }());
    }
    {
        sync::LocalEnterFlow f(cfg);
        f.Begin(9, 1, true, 1000);
        f.Tick(in(1000));
        const sync::LocalEnterOut early = f.Tick(in(1000 + cfg.answerWaitMs - 1));
        const sync::LocalEnterOut late = f.Tick(in(1000 + cfg.answerWaitMs));
        Check("local: no answer within answerWaitMs -> the walk is dropped with a CancelIntent (nothing is started on a reservation we cannot confirm)",
              early.act == LocalAct::None && late.act == LocalAct::SendCancel && late.why == LocalWhy::NoAnswer && f.stats().issued == 0);
    }
    {
        sync::LocalEnterFlow f(cfg);
        f.Begin(9, 1, true, 1000);
        f.Tick(in(1000));
        f.OnResult(9, 0);
        f.Tick(in(1100));
        sync::LocalEnterOut o;
        for (double t = 1116; t < 1100 + cfg.door.maxMs - 2; t += 16)
        {
            o = f.Tick(in(t, false, true));
            if (o.act != LocalAct::None) break;
        }
        const bool quiet = o.act == LocalAct::None;
        const sync::LocalEnterOut fb = f.Tick(in(1100 + cfg.door.maxMs, false, true));
        Check("local: a task that keeps running is left alone until its limit; then ONE FallbackWarp (TimedOut) - the only teleport-in",
              quiet && fb.act == LocalAct::FallbackWarp && fb.why == LocalWhy::TimedOut && f.stats().fallbackWarps == 1 && f.stats().timedOut == 1);
    }
    {
        sync::LocalEnterFlow f(cfg);
        f.Begin(9, 1, true, 1000);
        f.Tick(in(1000));
        f.OnResult(9, 0);
        f.Tick(in(1100));
        const sync::LocalEnterOut early = f.Tick(in(1100 + cfg.door.startWaitMs - 1));
        const sync::LocalEnterOut fb = f.Tick(in(1100 + cfg.door.startWaitMs));
        sync::LocalEnterFlow g(cfg);
        g.Begin(9, 1, true, 1000);
        g.Tick(in(1000));
        g.OnResult(9, 0);
        g.Tick(in(1100));
        g.Tick(in(1200, false, true));
        g.Tick(in(1300, false, false));
        const sync::LocalEnterOut fb2 = g.Tick(in(1300 + cfg.door.endedDebounceMs, false, false));
        Check("local: a task that never starts (locked door) falls back at startWaitMs; one that starts and stops without a seat falls back after the debounce",
              early.act == LocalAct::None && fb.act == LocalAct::FallbackWarp && fb.why == LocalWhy::NeverStarted && fb2.act == LocalAct::FallbackWarp &&
                  fb2.why == LocalWhy::EndedNoSeat);
    }
    {
        sync::LocalEnterFlow f(cfg);
        f.Begin(9, 1, true, 1000);
        f.Tick(in(1000));
        f.OnResult(9, 0);
        f.Tick(in(1100));
        f.Tick(in(1200, false, true));
        const sync::LocalEnterOut ab = f.Tick(in(1300, false, true, true));
        sync::LocalEnterFlow g(cfg);
        g.Begin(9, 1, true, 1000);
        g.Tick(in(1000));
        g.OnResult(9, 0);
        g.Tick(in(1100));
        g.Tick(in(1200, false, true));
        g.OnServerCancel(9, 1);
        const sync::LocalEnterOut sc = g.Tick(in(1300, false, true));
        Check("local: the player giving up, or the server cancelling the reservation (expired, replaced, car gone), clears the task and cancels - no warp",
              ab.act == LocalAct::ClearTask && ab.also == LocalAct::SendCancel && ab.why == LocalWhy::Aborted && sc.act == LocalAct::ClearTask &&
                  sc.why == LocalWhy::ServerCancelled && f.stats().fallbackWarps == 0 && g.stats().fallbackWarps == 0);
    }
    {
        sync::LocalEnterFlow f(cfg);
        f.Begin(9, 1, true, 1000);
        f.Tick(in(1000));
        f.OnResult(9, 0);
        const sync::LocalEnterOut go = f.Tick(in(1100, false, false, false, true));
        Check("local: a bike is issued with the mount time limit", go.act == LocalAct::IssueEnter && go.timeoutMs == (int)cfg.mount.maxMs);
    }
}

inline void TestBookAndModel()
{
    sync::SeatBook b;
    b.OnReserved(3, 2, 7, 1000);
    const bool held = b.Holder(3, 2, 1100) == 7;
    b.OnReserved(3, 4, 7, 1200);       // one reservation per player
    const bool replaced = b.Holder(3, 2, 1300) == sync::kSeatNobody && b.Holder(3, 4, 1300) == 7;
    const bool stale = b.Holder(3, 4, 1200 + sync::SeatBook::kStaleMs) == sync::kSeatNobody;
    b.OnReserved(4, 1, 8, 1400);
    b.DropVehicle(4);
    Check("book: a reservation is the holder's until it ends; one per player; forgotten when stale; dropped with its car",
          held && replaced && stale && b.Holder(4, 1, 1500) == sync::kSeatNobody);
    sync::SeatClientModel m;
    const proto::SeatNotice mine = Notice(proto::SeatNoticeKind::Reserved, 1, 9, 2, 0);
    const sync::NoticeEffect e0 = m.OnNotice(mine, 1, 1000);
    const sync::NoticeEffect e1 = m.OnNotice(Notice(proto::SeatNoticeKind::Reserved, 5, 9, 3, proto::kSeatNoticeRun), 1, 1010);
    m.OnNotice(Notice(proto::SeatNoticeKind::Exited, 5, 9, 3), 1, 1020);
    const sync::NoticeEffect e2 = m.OnNotice(Notice(proto::SeatNoticeKind::Exited, 5, 9, 3), 1, 1020);
    Check("model: a notice about ourselves reaches the book but creates no puppet; another player's Reserved creates its replay; Exited asks for the exit animation and latches",
          e0.aboutMe && !m.FindPuppet(1) && !e1.aboutMe && m.FindPuppet(5) && m.Book().Holder(9, 2, 1100) == 1 && e2.unseatPuppet && e2.player == 5 &&
              m.Puppet(5).Latched(9, 1100));
}

// The legacy client: no notices, warp when the list names the player, teleport-in on F. The scenario's key predicates must REJECT it.
inline void TestLegacyIsRejected()
{
    PureWorld w(3);
    w.legacy = true;
    const uint16_t V1 = t1scn::SpawnFor(w, 2);
    w.Until([&] { return w.C(0).seats.count(V1) && w.C(1).seats.count(V1) && w.C(2).seats.count(V1); }, 2000);
    // the old F: the player is put into the seat at once and the link says Enter
    w.C(0).self.st = SimPed::St::In;
    w.C(0).self.veh = V1;
    w.C(0).self.seat = 1;
    const double t0 = w.Now();
    w.Cmd(0, proto::VehicleOp::Enter, V1, 1);
    w.Until([&] { return w.ServerSeat(V1, 1) == 0; }, 2000);
    w.Run(600);
    const bool sameFinal = w.C(1).SeatOf(0) == (V1 << 8 | 1) && w.ServerSeat(V1, 1) == 0;
    const bool predicate = w.C(1).IssuedFor(0) == 1 && w.C(2).IssuedFor(0) == 1 && t1scn::TotalWarps(w) == 0;
    Check("fails-on-old: a legacy client ends in the right seat but with NO task on any observer and one warp per observer - S1's 'one task each, zero warps' predicate rejects it",
          sameFinal && !predicate && w.C(1).IssuedFor(0) == 0 && w.C(2).IssuedFor(0) == 0 && t1scn::TotalWarps(w) == 2,
          Fm("tasks B %d C %d warps %d (t0 %.0f)", w.C(1).IssuedFor(0), w.C(2).IssuedFor(0), t1scn::TotalWarps(w), t0));
}

// A stale VehicleWorld (state channel) overtaking the reliable Exited must not put the ped back in.
inline void TestStaleListAfterExit()
{
    sync::SeatReplayCfg rc = t1scn::ScenarioReplayCfg();
    SimClient c(0, rc, t1scn::ScenarioLocalCfg());
    uint16_t seats[proto::kVehSeats];
    for (auto& s : seats) s = proto::kNoPlayer;
    seats[1] = 1;
    c.SetSeats(4, seats);
    c.peds[1];
    c.peds[1].st = SimPed::St::In;
    c.peds[1].veh = 4;
    c.peds[1].seat = 1;
    c.peds[1].seatedFlag = true;
    double t = 1000;
    c.Step(t);
    c.OnNotice(Notice(proto::SeatNoticeKind::Exited, 1, 4, 1), t);
    // the stale frame still names player 1 in seat 1
    for (t = 1016; t < 1016 + 600; t += 16) c.Step(t);
    const bool held = c.exitAnims == 1 && c.peds[1].st == SimPed::St::Foot && c.warps.empty() && c.model.Puppet(1).stats().latchDenied > 0;
    for (t = 1016 + 600; t < 1016 + 900; t += 16) c.Step(t);
    Check("exit latch: the stale list naming the player right after Exited is ignored (denials counted, ped stays on foot); once the latch ends a list that still names it re-seats it (bounded)",
          held && c.warps.size() == 1 && c.warps[0].why == sync::WarpWhy::NoIntent);
}

inline void RunPure()
{
    std::printf("-- T1 seats: wire\n");
    TestWire();
    std::printf("-- T1 seats: the server's reservations\n");
    TestStore();
    TestAudience();
    std::printf("-- T1 seats: the puppet replay, the local flow, the book\n");
    TestReplay();
    TestLocalFlow();
    TestBookAndModel();
    std::printf("-- T1 seats: three clients and the real VehicleStore behind a 40 ms relay\n");
    {
        PureWorld w(3);
        t1scn::RunSeatScenario(w, "pure", [](const char* n, bool ok, const std::string& g) { Check(n, ok, g); });
    }
    TestLegacyIsRejected();
    TestStaleListAfterExit();
}

}   // namespace t1
