// T1 seats: a client-side simulation for the enter / exit intent (libs/sync/seat_intent.h), shared by gameplay_test (against the
// real VehicleStore and a simulated relay) and netcode_test (against the real server over ENet). Test-only, header-only.
//
// SimClient is ONE machine: it holds the REAL SeatClientModel (+ LocalEnterFlow) and a tiny fake game that mirrors the
// engine's glue step for step:
//   * the game: peds that are on foot, entering (TASK_ENTER_VEHICLE for N ms, then seated if the seat is free) or seated; a car
//     can be marked "mount" (bike), "noStart" (the task never starts: a locked door) or "stuck" (the task starts and never ends);
//   * the engine glue: DrainEvents -> SeatClientModel::OnNotice, TickPeerTierB -> PuppetSeatReplay::Tick, the seat loop in
//     TickVehicles -> AllowsSeatWarp / NoteWarp, the unseat loop, TickVehicleEntry -> LocalEnterFlow and the link's Enter.
// What it proves is the logic those natives-calling functions delegate to, in the order they call it; the natives themselves
// (TASK_ENTER_VEHICLE really animating, GET_VEHICLE_PED_IS_TRYING_TO_ENTER really naming the car) are live-unverified.
#pragma once

#include "protocol/protocol.h"
#include "sync/seat_intent.h"

#include <array>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace t1sim {

namespace sync = gage::sync;
namespace proto = gage::proto;

struct SimPed
{
    enum class St { Foot, Entering, In } st = St::Foot;
    int veh = -1, seat = -1;      // Entering: where the task is aimed; In: where it sits
    double endMs = 0;             // Entering: when the game's task completes
    bool seatedFlag = false;      // the engine's pe.seatedVeh >= 0: set only by the seat loop, cleared by the unseat loop
};

struct WarpRec { double ms; uint16_t player; int veh, seat; sync::WarpWhy why; };
struct IssueRec { double ms; uint16_t player; int veh, seat; int timeoutMs; bool run; };

class SimClient
{
public:
    SimClient(uint16_t id, const sync::SeatReplayCfg& rc, const sync::LocalEnterCfg& lc)
        : me(id), model(rc), local(lc) {}

    uint16_t me;
    sync::SeatClientModel model;
    sync::LocalEnterFlow local;
    std::map<int, std::array<uint16_t, proto::kVehSeats>> seats;   // the last VehicleWorld seat list per vehicle
    std::set<int> mount, noStart, stuck;                           // per-car behaviour of the fake game's enter task
    double taskMs = 800;                                           // how long the game's enter task takes when it works
    std::map<uint16_t, SimPed> peds;                               // other players' puppets
    SimPed self;
    std::function<void(const proto::VehicleCmd&)> send;

    // counters / logs
    std::vector<IssueRec> issues;                  // every puppet TASK_ENTER_VEHICLE
    std::vector<WarpRec> warps;                    // every puppet seat warp
    std::vector<proto::VehicleResult> results;
    std::vector<proto::SeatNotice> notices;
    int evicts = 0, exitAnims = 0, unseats = 0, localIssued = 0, localFallbacks = 0, localRefused = 0, localSends = 0, localCancels = 0;
    uint8_t lastRefusal = 0;
    sync::LocalWhy lastLocalWhy = sync::LocalWhy::None;
    double lastNow = 0;

    int IssuedFor(uint16_t player) const
    {
        int n = 0;
        for (const IssueRec& r : issues) n += r.player == player;
        return n;
    }
    int WarpsFor(uint16_t player) const
    {
        int n = 0;
        for (const WarpRec& r : warps) n += r.player == player;
        return n;
    }
    // (veh << 8 | seat) of the puppet or the self ped, -1 when it is not seated.
    int SeatOf(uint16_t player) const
    {
        const SimPed* p = nullptr;
        if (player == me) p = &self;
        else
        {
            const auto it = peds.find(player);
            if (it != peds.end()) p = &it->second;
        }
        return p && p->st == SimPed::St::In ? (p->veh << 8 | p->seat) : -1;
    }
    // The seat the list gives to `player`, (veh << 8 | seat) or -1.
    int ListedSeat(uint16_t player) const
    {
        for (const auto& kv : seats)
            for (size_t s = 0; s < proto::kVehSeats; ++s)
                if (kv.second[s] == player) return kv.first << 8 | (int)s;
        return -1;
    }
    bool Listed(int veh, int seat, uint16_t player) const
    {
        const auto it = seats.find(veh);
        return it != seats.end() && seat >= 0 && seat < (int)proto::kVehSeats && it->second[seat] == player;
    }

    // ---- inputs from the network (in the order the server sent them) ----
    void OnNotice(const proto::SeatNotice& n, double now)
    {
        notices.push_back(n);
        if (n.player != me) peds[n.player];
        const sync::NoticeEffect fx = model.OnNotice(n, me, now);
        if (fx.aboutMe && n.kind == proto::SeatNoticeKind::Cancelled)
            local.OnServerCancel(n.vehId, n.seat);
        if (fx.unseatPuppet)
        {
            SimPed& p = peds[n.player];
            if (p.st == SimPed::St::In && p.veh == fx.veh)
            {
                p = SimPed();
                ++exitAnims;
            }
        }
    }
    void OnResult(const proto::VehicleResult& r)
    {
        results.push_back(r);
        if (r.op == proto::VehicleOp::EnterIntent)
            local.OnResult(r.vehId, (uint8_t)r.code);
    }
    // The newest seat list of a vehicle (a VehicleWorld entry); `present` false = the vehicle is gone.
    void SetSeats(int veh, const uint16_t* s)
    {
        std::array<uint16_t, proto::kVehSeats> a;
        for (size_t i = 0; i < proto::kVehSeats; ++i) a[i] = s[i];
        seats[veh] = a;
    }
    void VehicleGone(int veh)
    {
        seats.erase(veh);
        model.OnVehicleGone(veh);
        for (auto& kv : peds)
            if (kv.second.veh == veh && kv.second.st != SimPed::St::Foot)
                kv.second = SimPed();
        if (self.veh == veh && self.st != SimPed::St::Foot)
            self = SimPed();
    }

    // ---- the local player ----
    void PressF(int netId, int seat, bool run, double now) { local.Begin(netId, seat, run, now); }
    // The player gives up the walk (moves away, ragdolls): the next Step aborts the flow.
    void AbortLocal() { abortLocal_ = true; }
    void SelfExit(double now)
    {
        (void)now;
        if (self.st == SimPed::St::In)
        {
            proto::VehicleCmd c;
            c.op = proto::VehicleOp::Exit;
            c.vehId = (uint16_t)self.veh;
            self = SimPed();
            linked_ = false;
            if (send) send(c);
        }
    }

    // One frame (the engine's script tick), in the engine's order.
    void Step(double now)
    {
        lastNow = now;
        // the game: tasks run to their end
        for (auto* p : AllPeds())
        {
            if (p->st == SimPed::St::Entering && noStart.count(p->veh))
            {
                *p = SimPed();                        // a locked door: the game refuses the task at once
                continue;
            }
            if (p->st == SimPed::St::Entering && now >= p->endMs && !stuck.count(p->veh))
            {
                if (SeatFree(p->veh, p->seat, p)) { p->st = SimPed::St::In; }
                else *p = SimPed();                   // the seat was taken: the task fails
            }
        }
        // our own entry (TickVehicleEntry) and the link's Enter
        if (local.phase() != sync::LocalEnterFlow::Phase::Idle)
        {
            sync::LocalEnterIn in;
            in.nowMs = now;
            in.seatedNow = self.st == SimPed::St::In;
            in.tryingToEnter = self.st == SimPed::St::Entering && !noStart.count(self.veh);
            in.mount = mount.count(local.NetId()) != 0;
            in.abort = abortLocal_;
            abortLocal_ = false;
            ApplyLocal(local.Tick(in), now);
        }
        if (self.st == SimPed::St::In && !linked_)
        {
            linked_ = true;
            proto::VehicleCmd c;
            c.op = proto::VehicleOp::Enter;
            c.vehId = (uint16_t)self.veh;
            c.arg = (uint32_t)self.seat;
            if (send) send(c);
        }
        // TickPeerTierB: one Tick per puppet
        for (auto& kv : peds)
        {
            const uint16_t pid = kv.first;
            SimPed& ped = kv.second;
            sync::PuppetSeatReplay& rp = model.Puppet(pid);
            sync::SeatReplayIn in;
            in.nowMs = now;
            in.pedReady = true;
            const int veh = rp.Veh();
            in.vehReady = veh >= 0 && seats.count(veh) != 0;
            if (rp.NeedsReadings() && veh >= 0)
            {
                in.inSeat = ped.st == SimPed::St::In && ped.veh == veh && ped.seat == rp.Seat();
                in.tryingToEnter = ped.st == SimPed::St::Entering && ped.veh == veh && !noStart.count(veh);
                in.mount = mount.count(veh) != 0;
                in.listedSomewhere = false;
                const auto it = seats.find(veh);
                if (it != seats.end())
                    for (size_t s = 0; s < proto::kVehSeats; ++s)
                        in.listedSomewhere = in.listedSomewhere || it->second[s] == pid;
            }
            const sync::SeatReplayOut out = rp.Tick(in);
            switch (out.act)
            {
            case sync::SeatAct::IssueEnter:
                ped.st = SimPed::St::Entering;
                ped.veh = out.veh;
                ped.seat = out.seat;
                ped.endMs = now + taskMs;
                issues.push_back(IssueRec{now, pid, out.veh, out.seat, out.timeoutMs, out.run});
                break;
            case sync::SeatAct::ClearTask:
                if (ped.st == SimPed::St::Entering) ped = SimPed();
                break;
            case sync::SeatAct::Evict:
                if (ped.st == SimPed::St::In) ped = SimPed();
                ++evicts;
                break;
            case sync::SeatAct::None:
                break;
            }
        }
        // TickVehicles' seat loop: warp whoever the list names and is not in the seat, if the replay allows it
        for (const auto& vk : seats)
        {
            const int veh = vk.first;
            for (size_t s = 0; s < proto::kVehSeats; ++s)
            {
                const uint16_t pid = vk.second[s];
                if (pid == proto::kNoPlayer || pid == me) continue;
                SimPed& ped = peds[pid];
                if (ped.st == SimPed::St::In && ped.veh == veh && ped.seat == (int)s)
                {
                    ped.seatedFlag = true;           // the task seated it: the loop only records it
                    continue;
                }
                const sync::WarpDecision wd = model.Puppet(pid).AllowsSeatWarp(veh, (int)s, now);
                if (!wd.allow) continue;
                ped.st = SimPed::St::In;
                ped.veh = veh;
                ped.seat = (int)s;
                ped.seatedFlag = true;
                model.Puppet(pid).NoteWarp(wd.why);
                warps.push_back(WarpRec{now, pid, veh, (int)s, wd.why});
            }
        }
        // the unseat loop: seated here (by the loop) but the list no longer has it
        for (auto& kv : peds)
        {
            SimPed& ped = kv.second;
            if (ped.seatedFlag && !Listed(ped.veh, ped.seat, kv.first))
            {
                ped = SimPed();
                ++unseats;
            }
        }
        // our own ped follows the list too (the server is the truth about where we sit)
    }

private:
    std::vector<SimPed*> AllPeds()
    {
        std::vector<SimPed*> v;
        v.push_back(&self);
        for (auto& kv : peds) v.push_back(&kv.second);
        return v;
    }
    bool SeatFree(int veh, int seat, const SimPed* except)
    {
        if (self.st == SimPed::St::In && self.veh == veh && self.seat == seat && &self != except) return false;
        for (const auto& kv : peds)
            if (&kv.second != except && kv.second.st == SimPed::St::In && kv.second.veh == veh && kv.second.seat == seat) return false;
        return true;
    }
    void ApplyLocal(const sync::LocalEnterOut& out, double now)
    {
        if (out.why != sync::LocalWhy::None) lastLocalWhy = out.why;
        if (out.why == sync::LocalWhy::Refused)
        {
            ++localRefused;
            lastRefusal = out.refusal;
        }
        for (const sync::LocalAct a : {out.act, out.also})
        {
            proto::VehicleCmd c;
            switch (a)
            {
            case sync::LocalAct::None: break;
            case sync::LocalAct::SendIntent:
                c.op = proto::VehicleOp::EnterIntent;
                c.vehId = (uint16_t)local.NetId();
                c.arg = sync::PackIntentArg(local.Seat(), local.Run());
                ++localSends;
                if (send) send(c);
                break;
            case sync::LocalAct::SendCancel:
                c.op = proto::VehicleOp::CancelIntent;
                c.vehId = (uint16_t)local.NetId();
                ++localCancels;
                if (send) send(c);
                break;
            case sync::LocalAct::IssueEnter:
                self.st = SimPed::St::Entering;
                self.veh = local.NetId();
                self.seat = local.Seat();
                self.endMs = now + taskMs;
                ++localIssued;
                lastLocalTimeoutMs = out.timeoutMs;
                break;
            case sync::LocalAct::ClearTask:
                if (self.st == SimPed::St::Entering) self = SimPed();
                break;
            case sync::LocalAct::FallbackWarp:
                self.st = SimPed::St::In;
                self.veh = local.NetId();
                self.seat = local.Seat();
                ++localFallbacks;
                lastFallbackMs = now;
                break;
            }
        }
    }

    bool abortLocal_ = false, linked_ = false;

public:
    int lastLocalTimeoutMs = 0;
    double lastFallbackMs = -1;
};

}   // namespace t1sim
