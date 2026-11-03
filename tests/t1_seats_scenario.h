// T1 seats: THE scripted multi-client scenario, written once and run twice: against the real VehicleStore behind a simulated relay
// (gameplay_test, tests/t1_seats_tests.h) and against the REAL server over ENet (netcode_test, tests/t1_seats_net_tests.h).
// Three players: A (0), B (1) and C (2). C owns V1 and V3 (V3 is a bike), A owns V2. Every client is a t1sim::SimClient (the real
// SeatClientModel / LocalEnterFlow / PuppetSeatReplay plus a fake game, tests/t1_seats_sim.h). The World type supplies:
//   double Now()                 the clock the clients are stepped with (simulated or real, ms)
//   void   Run(double ms)        advance time, stepping the clients and moving packets
//   bool   Until(pred, ms)       Run until pred() or the budget
//   SimClient& C(int i)          client i
//   uint16_t Id(int i)           its player id
//   void   Cmd(i, op, veh, arg)  a VehicleCmd from client i (async)
//   uint16_t ServerSeat(veh, s)  the SERVER's occupant of a seat (kNoPlayer when empty or no such vehicle)
//   bool   ServerHasVeh(veh)     the server still has the vehicle
// Every absence check ("no warp", "no task") sits next to the presence check that proves the path ran.
#pragma once

#include "t1_seats_sim.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace t1scn {

using t1sim::SimClient;
using t1sim::SimPed;
namespace proto = gage::proto;
namespace sync = gage::sync;

constexpr uint32_t kModel = 0xB779A091u;
constexpr int kScenarioChecks = 16;

inline std::string Fm(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

inline sync::SeatReplayCfg ScenarioReplayCfg()
{
    sync::SeatReplayCfg c;
    c.door = sync::EnterWatchCfg{2500.0, 1000.0, 400.0};
    c.mount = sync::EnterWatchCfg{2200.0, 1000.0, 400.0};
    return c;
}
inline sync::LocalEnterCfg ScenarioLocalCfg()
{
    sync::LocalEnterCfg c;
    c.door = sync::EnterWatchCfg{2000.0, 1000.0, 400.0};
    c.mount = sync::EnterWatchCfg{1800.0, 1000.0, 400.0};
    return c;
}

template <class W>
int TotalWarps(W& w)
{
    int n = 0;
    for (int i = 0; i < 3; ++i) n += (int)w.C(i).warps.size();
    return n;
}
template <class W>
int TotalEvicts(W& w)
{
    int n = 0;
    for (int i = 0; i < 3; ++i) n += w.C(i).evicts;
    return n;
}
template <class W>
size_t TotalNotices(W& w)
{
    size_t n = 0;
    for (int i = 0; i < 3; ++i) n += w.C(i).notices.size();
    return n;
}
template <class W>
bool NoPuppetActive(W& w)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (i != j)
            {
                const sync::PuppetSeatReplay* p = w.C(i).model.FindPuppet(w.Id(j));
                if (p && p->Active()) return false;
            }
    return true;
}
// Every client's seat list == the server's, for the seats of `veh`.
template <class W>
bool ListsAgree(W& w, int veh)
{
    for (int i = 0; i < 3; ++i)
        for (int s = 0; s < (int)proto::kVehSeats; ++s)
        {
            const uint16_t srv = w.ServerSeat((uint16_t)veh, s);
            const bool listed = srv != proto::kNoPlayer && w.C(i).Listed(veh, s, srv);
            const bool listedWrong = srv == proto::kNoPlayer && w.C(i).seats.count(veh) && w.C(i).seats[veh][s] != proto::kNoPlayer;
            if ((srv != proto::kNoPlayer && !listed) || listedWrong) return false;
        }
    return true;
}
// For every player and every client: the ped shown for that player sits exactly where the server seated it (or on foot).
template <class W>
bool ScreensAgree(W& w, std::string* why)
{
    for (int pl = 0; pl < 3; ++pl)
    {
        int want = -1;
        for (int veh = 0; veh < (int)proto::kMaxVehicles && want < 0; ++veh)
            for (int s = 0; s < (int)proto::kVehSeats; ++s)
                if (w.ServerSeat((uint16_t)veh, s) == w.Id(pl)) { want = veh << 8 | s; break; }
        for (int c = 0; c < 3; ++c)
        {
            const int got = w.C(c).SeatOf(w.Id(pl));
            if (got != want)
            {
                if (why) *why = Fm("client %d shows player %d at %d/%d, the server has %d/%d", c, pl, got >> 8, got & 255, want >> 8, want & 255);
                return false;
            }
        }
    }
    return true;
}

template <class W>
uint16_t SpawnFor(W& w, int idx)
{
    const size_t before = w.C(idx).results.size();
    w.Cmd(idx, proto::VehicleOp::Spawn, 0, kModel);
    w.Until([&] { return w.C(idx).results.size() > before; }, 3000);
    const auto& r = w.C(idx).results;
    return r.size() > before && r.back().code == proto::VehicleResultCode::Ok ? r.back().vehId : (uint16_t)0xFFFF;
}

// `tag` prefixes every check name ("pure" / "net"). Runs kScenarioChecks checks.
template <class W>
void RunSeatScenario(W& w, const char* tag, void (*check)(const char*, bool, const std::string&))
{
    const uint16_t A = w.Id(0), B = w.Id(1), Cc = w.Id(2);
    auto chk = [&](const std::string& n, bool ok, const std::string& got) { check((std::string(tag) + ": " + n).c_str(), ok, got); };
    SimClient &a = w.C(0), &b = w.C(1), &c = w.C(2);
    // B's game plays the enter animation more slowly (1400 ms against 800): when the server says "seated" B's replay of A is still running,
    // which is exactly the case the seat warp must NOT cut short (it waits confirmGraceMs). With a grace of 0 the first enter already fails.
    b.taskMs = 1400;

    // ---- setup: C owns V1 (a car) and V3 (a bike), A owns V2 ----
    const uint16_t V1 = SpawnFor(w, 2), V2 = SpawnFor(w, 0), V3 = SpawnFor(w, 2);
    const bool spawned = V1 != 0xFFFF && V2 != 0xFFFF && V3 != 0xFFFF;
    w.Until([&] {
        for (int i = 0; i < 3; ++i)
            if (!w.C(i).seats.count(V1) || !w.C(i).seats.count(V2) || !w.C(i).seats.count(V3)) return false;
        return true;
    }, 3000);
    for (int i = 0; i < 3; ++i) w.C(i).mount.insert(V3);
    chk("setup: three vehicles spawned and listed on every client", spawned && ListsAgree(w, V1) && ListsAgree(w, V2),
        Fm("V1 %u V2 %u V3 %u", (unsigned)V1, (unsigned)V2, (unsigned)V3));

    // ---- owner rule: a driver seat of somebody else's car is refused, nothing starts, nobody is told ----
    {
        const size_t n0 = TotalNotices(w);
        a.PressF(V1, 0, true, w.Now());
        w.Until([&] { return a.localRefused >= 1; }, 2000);
        w.Run(300);
        chk("owner rule: A's intent on the DRIVER seat of C's car is refused NotOwner (12) by the server; no task starts anywhere and no notice goes out",
            a.localRefused == 1 && a.lastRefusal == 12 && a.localSends == 1 && a.localIssued == 0 && b.issues.empty() && c.issues.empty() &&
                TotalNotices(w) == n0,
            Fm("refused %d code %d sends %d issued %d notices %zu->%zu", a.localRefused, a.lastRefusal, a.localSends, a.localIssued, n0, TotalNotices(w)));
    }

    // ---- S1: A walks into seat 1 of V1 (door entry) ----
    {
        a.PressF(V1, 1, true, w.Now());
        const bool seated = w.Until([&] { return w.ServerSeat(V1, 1) == A; }, 6000);
        w.Run(800);
        std::string why;
        chk("enter: one task on A's screen, exactly one on each observer's, A seated by the game and ZERO warps anywhere",
            seated && a.localIssued == 1 && a.localFallbacks == 0 && b.IssuedFor(A) == 1 && c.IssuedFor(A) == 1 && TotalWarps(w) == 0 && TotalEvicts(w) == 0,
            Fm("seated %d localIssued %d fallbacks %d B tasks %d C tasks %d warps %d evicts %d", seated, a.localIssued, a.localFallbacks, b.IssuedFor(A), c.IssuedFor(A), TotalWarps(w), TotalEvicts(w)));
        chk("enter: server seat == every client's seat list == the ped every screen shows (V1 seat 1 = A)",
            w.ServerSeat(V1, 1) == A && ListsAgree(w, V1) && ScreensAgree(w, &why), why);
        bool order = false, runFlag = false;
        for (size_t i = 0; i + 1 < b.notices.size(); ++i)
            if (b.notices[i].kind == proto::SeatNoticeKind::Reserved && b.notices[i].player == A && b.notices[i].seat == 1 &&
                b.notices[i + 1].kind == proto::SeatNoticeKind::Seated && b.notices[i + 1].player == A)
                order = true;
        runFlag = !b.issues.empty() && b.issues[0].run && b.issues[0].seat == 1 && b.issues[0].veh == V1 &&
                  b.issues[0].timeoutMs == (int)ScenarioReplayCfg().door.maxMs;
        chk("enter: B heard Reserved then Seated for A (seat 1), and replayed it with A's run flag, the car's seat and the door time limit",
            order && runFlag, Fm("order %d run %d issues %zu", order, runFlag, b.issues.size()));
    }

    // ---- S2: B reserves seat 2; C reaches for the same seat while B is mid-enter ----
    {
        const size_t bIssued = a.IssuedFor(B);
        b.PressF(V1, 2, false, w.Now());
        const bool bookSeen = w.Until([&] { return c.model.Book().Holder(V1, 2, w.Now()) == B; }, 2000);
        c.PressF(V1, 2, false, w.Now());
        w.Until([&] { return c.localRefused >= 1; }, 2000);
        const bool bSeated = w.Until([&] { return w.ServerSeat(V1, 2) == B; }, 6000);
        w.Run(800);
        std::string why;
        chk("conflict: C's book shows B walking to seat 2; C's own intent on that seat is refused SeatTaken (2) and C starts nothing",
            bookSeen && c.localRefused == 1 && c.lastRefusal == 2 && c.localIssued == 0 && b.localIssued == 1,
            Fm("book %d refused %d code %d C issued %d B issued %d", bookSeen, c.localRefused, c.lastRefusal, c.localIssued, b.localIssued));
        chk("conflict: B ends in seat 2 on every screen, one task per observer, no warp (server seat == lists == peds)",
            bSeated && w.ServerSeat(V1, 2) == B && ListsAgree(w, V1) && ScreensAgree(w, &why) && a.IssuedFor(B) == bIssued + 1 && c.IssuedFor(B) == 1 &&
                TotalWarps(w) == 0,
            why.empty() ? Fm("A tasks for B %d C tasks for B %d warps %d", a.IssuedFor(B), c.IssuedFor(B), TotalWarps(w)) : why);
    }

    // ---- S3: C starts for seat 3 and gives up mid-enter, twice: by the walk's own cancel, then by an Exit command ----
    {
        c.PressF(V1, 3, false, w.Now());
        const bool started = w.Until([&] { return a.IssuedFor(Cc) == 1 && b.IssuedFor(Cc) == 1 && c.localIssued == 1; }, 2000);
        w.Run(250);
        c.AbortLocal();
        w.Until([&] { return NoPuppetActive(w) && w.ServerSeat(V1, 3) == proto::kNoPlayer; }, 1500);
        w.Run(400);
        std::string why;
        chk("cancel mid-enter: the tasks had started (1 each), CancelIntent frees seat 3 and both observers clear C's task: C is on foot on every screen, no warp",
            started && w.ServerSeat(V1, 3) == proto::kNoPlayer && a.SeatOf(Cc) == -1 && b.SeatOf(Cc) == -1 && c.SeatOf(Cc) == -1 &&
                a.peds[Cc].st == SimPed::St::Foot && b.peds[Cc].st == SimPed::St::Foot && NoPuppetActive(w) && ScreensAgree(w, &why) && TotalWarps(w) == 0,
            why);
        // the same through an Exit command while only the reservation is pending
        c.PressF(V1, 3, false, w.Now());
        const bool started2 = w.Until([&] { return a.IssuedFor(Cc) == 2 && b.IssuedFor(Cc) == 2 && c.localIssued == 2; }, 2000);
        w.Run(250);
        const size_t before = c.results.size();
        w.Cmd(2, proto::VehicleOp::Exit, V1, 0);
        w.Until([&] { return c.results.size() > before && NoPuppetActive(w) && c.local.phase() != sync::LocalEnterFlow::Phase::Running; }, 2000);
        w.Run(400);
        bool exitOk = false;
        for (size_t i = before; i < c.results.size(); ++i)
            exitOk = exitOk || (c.results[i].op == proto::VehicleOp::Exit && c.results[i].code == proto::VehicleResultCode::Ok);
        chk("exit during enter: Exit while the seat is only reserved answers Ok, cancels the reservation, clears C's task on all three screens, seat 3 stays free, no warp",
            started2 && exitOk && w.ServerSeat(V1, 3) == proto::kNoPlayer && a.SeatOf(Cc) == -1 && b.SeatOf(Cc) == -1 && c.SeatOf(Cc) == -1 &&
                NoPuppetActive(w) && ScreensAgree(w, nullptr) && TotalWarps(w) == 0,
            Fm("started %d exitOk %d warps %d", started2, exitOk, TotalWarps(w)));
    }

    // ---- S4: A's V2 is despawned while C is on the way into it ----
    {
        const int bBefore = b.IssuedFor(Cc), aBefore = a.IssuedFor(Cc);
        c.PressF(V2, 1, false, w.Now());
        const bool started = w.Until([&] { return a.IssuedFor(Cc) == aBefore + 1 && b.IssuedFor(Cc) == bBefore + 1 && c.localIssued == 3; }, 2000);
        w.Run(200);
        w.Cmd(0, proto::VehicleOp::Despawn, V2, 0);
        w.Until([&] { return !w.ServerHasVeh(V2) && NoPuppetActive(w) && !b.seats.count(V2); }, 2000);
        w.Run(400);
        std::string why;
        chk("despawn mid-enter: C's task had started on every screen, the car's removal cancels it (VehicleGone): nobody is seated, nobody is warped, the car is gone everywhere",
            started && !w.ServerHasVeh(V2) && !a.seats.count(V2) && !b.seats.count(V2) && !c.seats.count(V2) && a.SeatOf(Cc) == -1 &&
                b.SeatOf(Cc) == -1 && c.SeatOf(Cc) == -1 && NoPuppetActive(w) && ScreensAgree(w, &why) && TotalWarps(w) == 0,
            why);
    }

    // ---- S5: a bike's pillion seat ----
    {
        c.PressF(V3, 1, false, w.Now());
        const bool seated = w.Until([&] { return w.ServerSeat(V3, 1) == Cc; }, 6000);
        w.Run(800);
        std::string why;
        const bool mountLimit = !a.issues.empty() && a.issues.back().veh == V3 && a.issues.back().timeoutMs == (int)ScenarioReplayCfg().mount.maxMs &&
                                c.lastLocalTimeoutMs == (int)ScenarioLocalCfg().mount.maxMs;
        chk("bike pillion: C reaches seat 1 of the bike by the game's task on every screen (mount time limits on A's replay and on C), server seat == screens, no warp",
            seated && mountLimit && ListsAgree(w, V3) && ScreensAgree(w, &why) && TotalWarps(w) == 0,
            why.empty() ? Fm("seated %d mountLimit %d", seated, mountLimit) : why);
    }

    // ---- S6: exit: the normal exit animation is replayed once on every observer ----
    {
        const int exitsA = a.exitAnims, exitsB = b.exitAnims;
        c.SelfExit(w.Now());
        w.Until([&] { return w.ServerSeat(V3, 1) == proto::kNoPlayer; }, 2000);
        w.Run(400);
        std::string why;
        chk("exit: Exited reaches both observers: one exit animation each, C on foot everywhere, the seat is free on the server, no warp",
            a.exitAnims == exitsA + 1 && b.exitAnims == exitsB + 1 && w.ServerSeat(V3, 1) == proto::kNoPlayer && ScreensAgree(w, &why) && TotalWarps(w) == 0,
            why.empty() ? Fm("exit anims A %d B %d", a.exitAnims - exitsA, b.exitAnims - exitsB) : why);
    }

    // ---- S7: timeout fallback on one screen: B's game never starts the task for A (a locked door) ----
    {
        // A leaves seat 1 first (so the seat is free again), then enters it with B's task refusing to start.
        a.SelfExit(w.Now());
        w.Until([&] { return w.ServerSeat(V1, 1) == proto::kNoPlayer; }, 2000);
        w.Run(300);
        b.noStart.insert(V1);
        const int bTasks = b.IssuedFor(A), cTasks = c.IssuedFor(A);
        const double pressMs = w.Now();
        a.PressF(V1, 1, true, pressMs);
        const bool seated = w.Until([&] { return w.ServerSeat(V1, 1) == A; }, 6000);
        w.Until([&] { return b.WarpsFor(A) >= 1; }, 4000);
        w.Run(600);
        std::string why;
        const bool oneWarp = b.WarpsFor(A) == 1 && b.warps.back().why == sync::WarpWhy::TaskEnded;
        const double issueMs = b.issues.empty() ? 0.0 : b.issues.back().ms;
        const bool notEarly = oneWarp && b.warps.back().ms - issueMs >= ScenarioReplayCfg().door.startWaitMs;
        chk("timeout fallback: B's task for A is issued once and never starts; exactly ONE warp follows on B, not before the start timeout; C (task worked) gets none",
            seated && b.IssuedFor(A) == bTasks + 1 && oneWarp && notEarly && c.IssuedFor(A) == cTasks + 1 && c.WarpsFor(A) == 0 && a.WarpsFor(A) == 0,
            Fm("seated %d B tasks %d warps %d notEarly %d (warp at +%.0f ms of the issue) phase %d neverStarted %u ped %d", seated, b.IssuedFor(A) - bTasks, b.WarpsFor(A), notEarly,
               b.warps.empty() ? -1.0 : b.warps.back().ms - issueMs, (int)b.model.Puppet(A).phase(), b.model.Puppet(A).stats().neverStarted, (int)b.peds[A].st));
        chk("timeout fallback: afterwards server seat == lists == every screen, A in seat 1 everywhere",
            w.ServerSeat(V1, 1) == A && ListsAgree(w, V1) && ScreensAgree(w, &why), why);
        b.noStart.erase(V1);
    }

    // ---- S8: our own task never ends: the bounded fallback seats C (the teleport-in only at the timeout) ----
    {
        c.stuck.insert(V1);
        const int fb = c.localFallbacks;
        const double pressMs = w.Now();
        c.PressF(V1, 3, false, pressMs);
        const bool seated = w.Until([&] { return w.ServerSeat(V1, 3) == Cc; }, 8000);
        w.Run(600);
        const bool bounded = c.localFallbacks == fb + 1 && c.lastFallbackMs - pressMs >= ScenarioLocalCfg().door.maxMs - 1.0;
        std::string why;
        chk("local fallback: a task that never ends is left alone until its limit, then ONE bounded warp seats C; the server and every screen agree",
            seated && bounded && c.lastLocalWhy == sync::LocalWhy::TimedOut && w.ServerSeat(V1, 3) == Cc && ListsAgree(w, V1) && ScreensAgree(w, &why),
            why.empty() ? Fm("seated %d fallbacks %d at +%.0f ms why %d", seated, c.localFallbacks - fb, c.lastFallbackMs - pressMs, (int)c.lastLocalWhy) : why);
        c.stuck.erase(V1);
    }

    // ---- the whole run ----
    {
        std::string why;
        chk("whole run: V1 seats 1..3 = A, B, C on the server, on every client's list and on every screen; the only warp in the run is S7's",
            w.ServerSeat(V1, 1) == A && w.ServerSeat(V1, 2) == B && w.ServerSeat(V1, 3) == Cc && ListsAgree(w, V1) && ScreensAgree(w, &why) &&
                TotalWarps(w) == 1 && TotalEvicts(w) == 0,
            why.empty() ? Fm("warps %d evicts %d", TotalWarps(w), TotalEvicts(w)) : why);
    }
}

}   // namespace t1scn
