// 0.4.22 doors and seats: the pure tests of libs/sync/vehicle_lock.h and of the rear-seat additions to libs/sync/vehicle_entry.h, included by
// gameplay_test.cpp after Check() and F() are defined. The same rules against the REAL server and session are in tests/doors_seats_net_tests.h
// (t3_vehvisual_test).
//
//   A. Seat choice (G / F through PickEntry): a rear seat is picked by the door nearest to the player, the car fills in a known order and then
//      refuses as Full, a two-seater has no phantom rear seat, veh_rear_seats=0 (SeatsOffered) stops at the front passenger, the seat-count
//      natives are combined (ResolveSeatCount), the seat class and the read-back verdict.
//   B. Lock rules: who may enter a locked car (the pick), who holds the key (HoldsKey / MayEnterLocked), who may toggle it and from how far
//      (JudgeLockToggle), which car the L key acts on (PickLockTarget), the key ring, the words and the sounds, the status read-back.
//   C. LockKeeper against a fake car at 60 fps: a lock the game keeps is only read; one the game drops once is restored; one the game keeps
//      dropping is not a stream of calls (stuck, back-off); an unreadable native is never "fixed"; Reset makes the next read due.
//
// Evidence level: SIMULATED. What GET_VEHICLE_DOOR_LOCK_STATUS / SET_VEHICLE_DOORS_LOCKED / the frontend sounds do in the real game is
// live-unverified (docs/api/protocol.md "0.4.22 doors and seats").
#pragma once

#include "sync/vehicle_entry.h"
#include "sync/vehicle_lock.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace doorsseats {

using namespace gage::sync;

constexpr int kPureChecks = 19;   // the number of Check() calls RunPure makes (gameplay_test's total counts it, a skipped section fails)

// A car at (cx, cy) with the fixed door offsets the engine falls back to when a model has no seat bones: x -0.45 / +0.45 by side (even seats
// left), y 0.4 forward minus 0.9 per row. `seatsOffered` is what FillEntryDoors hands PickEntry (the model's seat count, or 2 with veh_rear_seats=0).
inline EntryCandidate FourDoor(int netId, float cx, float cy, int seatsOffered)
{
    EntryCandidate c;
    c.netId = netId;
    c.maxSeats = seatsOffered;
    for (int s = 0; s < kEntrySeats; ++s)
        c.door[s] = { cx + ((s & 1) ? 0.45f : -0.45f), cy + 0.4f - 0.9f * (float)(s / 2), 0.0f };
    return c;
}

inline EntryPick Press(const EntryCandidate& c, float px, float py, bool wantDriver, uint16_t me = 9)
{
    return PickEntry({ px, py, 0.0f }, &c, 1, me, wantDriver);
}

inline std::string PickText(const EntryPick& p)
{
    char b[64];
    std::snprintf(b, sizeof b, "car %d seat %d reason %d", p.netId, p.seat, (int)p.reason);
    return b;
}

// ---- C. the keeper against a fake car -------------------------------------------------------------------------------------------

struct FakeLockCar
{
    int  status = 1;              // what GET_VEHICLE_DOOR_LOCK_STATUS reads (1 unlocked, 2 locked)
    bool ignoreSet = false;       // SET_VEHICLE_DOORS_LOCKED is called and changes nothing (a script owns the doors)
    bool readable = true;         // false: the read native cannot be called (-1)
    int  sets = 0;                // SET_VEHICLE_DOORS_LOCKED calls made by the keeper
};

struct KeeperRun
{
    LockKeepStats stats;
    double restoredAtS = -1.0;    // the first frame after the drop at which the car reads the wanted state again
};

// 60 fps for `seconds`; the game drops the lock (the car reads unlocked) once at `dropAtS` (< 0: never).
inline KeeperRun RunKeeper(FakeLockCar& car, int want, double seconds, double dropAtS)
{
    LockKeeper k;
    KeeperRun r;
    bool dropped = false;
    const double dt = 1000.0 / 60.0;
    for (int f = 0; f <= (int)(seconds * 60.0); ++f)
    {
        const double t = f * dt;
        if (dropAtS >= 0.0 && !dropped && t >= dropAtS * 1000.0)
        {
            car.status = kLockUnlocked;
            dropped = true;
        }
        if (k.Due(t))
        {
            const int status = car.readable ? car.status : -1;
            if (k.Observe(want, status, t) == LockKeeper::Act::Reissue)
            {
                ++car.sets;
                if (!car.ignoreSet) car.status = want;
            }
        }
        if (dropped && r.restoredAtS < 0.0 && t > dropAtS * 1000.0 && car.status == want)
            r.restoredAtS = t / 1000.0;
    }
    r.stats = k.Stats();
    return r;
}

inline void RunPure()
{
    std::printf("-- 0.4.22 doors and seats (libs/sync/vehicle_lock.h, vehicle_entry.h): seat choice, lock rules, the lock keeper\n");

    // ---------------------------------------------------------------- A. seat choice
    {
        // Standing on the left of a four-door whose driver's seat and front passenger's seat are taken: the rear LEFT door is the nearest, on the
        // right the rear RIGHT one (wire seats 2 and 3).
        EntryCandidate c = FourDoor(1, 0, 0, 4);
        c.seats[0] = 5;
        c.seats[1] = 6;
        const EntryPick left = Press(c, -2.0f, -0.5f, false), right = Press(c, 2.0f, -0.5f, false);
        Check("rear seats: G beside a four-door whose front seats are taken picks the rear door on the player's side (left: wire seat 2, right: wire seat 3)",
              left.reason == EntryReason::Ok && left.seat == 2 && right.reason == EntryReason::Ok && right.seat == 3,
              F("left %s, right %s", PickText(left).c_str(), PickText(right).c_str()));
    }
    {
        // Three presses from the right-hand side with the driver's seat taken fill the car front passenger, rear right, rear left, then it is Full.
        EntryCandidate c = FourDoor(1, 0, 0, 4);
        c.seats[0] = 5;
        int order[4] = { -9, -9, -9, -9 };
        EntryReason last = EntryReason::Ok;
        for (int i = 0; i < 4; ++i)
        {
            const EntryPick p = Press(c, 2.0f, 0.0f, false);
            order[i] = p.reason == EntryReason::Ok ? p.seat : -1;
            last = p.reason;
            if (p.reason == EntryReason::Ok) c.seats[p.seat] = (uint16_t)(20 + i);
        }
        Check("rear seats: four G presses from one side fill the free seats nearest-door first (front passenger 1, rear right 3, rear left 2) and the fourth is refused Full",
              order[0] == 1 && order[1] == 3 && order[2] == 2 && order[3] == -1 && last == EntryReason::Full,
              F("seats %d %d %d %d, last reason %d", order[0], order[1], order[2], order[3], (int)last));
    }
    {
        // veh_rear_seats=0: the engine offers SeatsOffered(seats, false) = 2, so a front passenger is picked, then the car is Full (never a rear seat);
        // with the switch on the same car offers its rear seats.
        EntryCandidate off = FourDoor(1, 0, 0, SeatsOffered(4, false)), on = FourDoor(1, 0, 0, SeatsOffered(4, true));
        off.seats[0] = on.seats[0] = 5;
        const EntryPick offFront = Press(off, -2.0f, 0.0f, false);
        off.seats[1] = on.seats[1] = 6;
        const EntryPick offFull = Press(off, -2.0f, -0.5f, false), onRear = Press(on, -2.0f, -0.5f, false);
        Check("rear seats: veh_rear_seats=0 (SeatsOffered 2) picks the front passenger and then refuses Full where the same car with the switch on picks the rear seat",
              offFront.reason == EntryReason::Ok && offFront.seat == 1 && offFull.reason == EntryReason::Full && onRear.reason == EntryReason::Ok && onRear.seat == 2,
              F("off: %s then %s; on: %s", PickText(offFront).c_str(), PickText(offFull).c_str(), PickText(onRear).c_str()));
    }
    Check("rear seats: SeatsOffered keeps every seat with the switch on and stops at 2 (driver + front passenger) with it off; a one- or two-seater is unchanged",
          SeatsOffered(4, true) == 4 && SeatsOffered(4, false) == 2 && SeatsOffered(2, false) == 2 && SeatsOffered(1, false) == 1 && SeatsOffered(8, false) == 2 &&
              SeatsOffered(8, true) == 8 && SeatsOffered(2, true) == 2,
          F("%d %d %d %d %d %d %d", SeatsOffered(4, true), SeatsOffered(4, false), SeatsOffered(2, false), SeatsOffered(1, false), SeatsOffered(8, false),
            SeatsOffered(8, true), SeatsOffered(2, true)));
    {
        // GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS excludes the driver, GET_VEHICLE_MODEL_NUMBER_OF_SEATS includes him; -1 = not callable.
        const SeatCount a = ResolveSeatCount(3, 4), b = ResolveSeatCount(1, -1), c = ResolveSeatCount(-1, 4), d = ResolveSeatCount(-1, -1),
                        e = ResolveSeatCount(1, 4), f = ResolveSeatCount(7, 9), g = ResolveSeatCount(0, 1), h = ResolveSeatCount(3, 2);
        Check("rear seats: ResolveSeatCount reads the vehicle's count (passengers + 1) or the model's, whichever answers; both answering takes the larger and flags the "
              "disagreement; neither answering is the old 2; clamped to 1..8",
              a.seats == 4 && !a.disagree && a.fromVehicle && a.fromModel && b.seats == 2 && b.fromVehicle && !b.fromModel && c.seats == 4 && !c.fromVehicle &&
                  c.fromModel && d.seats == 2 && !d.fromVehicle && !d.fromModel && e.seats == 4 && e.disagree && f.seats == 8 && g.seats == 1 && h.seats == 4 && h.disagree,
              F("%d %d %d %d %d %d %d %d", a.seats, b.seats, c.seats, d.seats, e.seats, f.seats, g.seats, h.seats));
    }
    {
        // A two-seater has doors for 8 seats in the helper (as the engine's fallback offsets do): the seats beyond the model's do not exist.
        EntryCandidate c = FourDoor(1, 0, 0, 2);
        c.seats[0] = 5;
        c.seats[1] = 6;
        const EntryPick g = Press(c, 2.0f, -0.5f, false);
        c.seats[1] = kEntryNobody;
        const EntryPick f = Press(c, 2.0f, -0.5f, true);
        Check("rear seats: a two-seater has no rear seat (G with both seats taken is Full, F with the front free is seat 1 with the driver's seat taken)",
              g.reason == EntryReason::Full && f.reason == EntryReason::Ok && f.seat == 1, F("G %s, F %s", PickText(g).c_str(), PickText(f).c_str()));
    }
    Check("rear seats: the seat class (driver, front passenger, rear 2-3, further back 4-7) and the read-back verdict (match, differs, not seated)",
          ClassifySeat(0) == SeatClass::Driver && ClassifySeat(-1) == SeatClass::Driver && ClassifySeat(1) == SeatClass::FrontPassenger && ClassifySeat(2) == SeatClass::Rear &&
              ClassifySeat(3) == SeatClass::Rear && ClassifySeat(4) == SeatClass::FurtherBack && ClassifySeat(7) == SeatClass::FurtherBack &&
              JudgeSeatReadback(3, 3) == SeatReadback::Match && JudgeSeatReadback(3, 2) == SeatReadback::Differs && JudgeSeatReadback(3, -1) == SeatReadback::NotSeated &&
              JudgeSeatReadback(0, 0) == SeatReadback::Match);

    // ---------------------------------------------------------------- B. lock rules
    {
        EntryCandidate stranger = FourDoor(1, 0, 0, 4);
        stranger.locked = true;
        stranger.ownerId = 7;
        EntryCandidate keyed = stranger;
        keyed.keyHolder = true;
        keyed.ownerId = kEntryNobody;                           // a key holder who is not the owner drove an unowned car (the owner's car has its driver's seat to the owner)
        EntryCandidate mine = stranger;
        mine.ownerId = 9;                                       // me
        EntryCandidate nearLocked = FourDoor(2, 0, 0, 4), farOpen = FourDoor(3, 6, 0, 4);
        nearLocked.locked = true;
        nearLocked.ownerId = 7;
        const EntryCandidate two[] = { nearLocked, farOpen };
        const EntryPick fS = Press(stranger, 2.0f, 0.0f, true), gS = Press(stranger, 2.0f, -0.5f, false), fK = Press(keyed, 2.0f, 0.0f, true),
                        gK = Press(keyed, 2.0f, -0.5f, false), fM = Press(mine, 2.0f, 0.0f, true),
                        skip = PickEntry({ 2.0f, 0.0f, 0.0f }, two, 2, 9, false);
        Check("locks: a locked car refuses F and G (rear seats too) with Locked for a player without the key, lets in its owner and a key holder, and a nearer locked car is "
              "skipped for a farther open one",
              fS.reason == EntryReason::Locked && gS.reason == EntryReason::Locked && fK.reason == EntryReason::Ok && fK.seat == 0 && gK.reason == EntryReason::Ok &&
                  gK.seat >= 1 && fM.reason == EntryReason::Ok && fM.seat == 0 && skip.reason == EntryReason::Ok && skip.netId == 3,
              F("stranger F %d G %d, keyed F %s G %s, owner F %s, two cars %s", (int)fS.reason, (int)gS.reason, PickText(fK).c_str(), PickText(gK).c_str(),
                PickText(fM).c_str(), PickText(skip).c_str()));
    }
    Check("locks: the key is the spawner or the last driver, nobody else; a locked vehicle admits exactly them (MayEnterLocked == HoldsKey)",
          !HoldsKey(false, false) && HoldsKey(true, false) && HoldsKey(false, true) && HoldsKey(true, true) && !MayEnterLocked(false, false) &&
              MayEnterLocked(true, false) && MayEnterLocked(false, true) && MayEnterLocked(true, true));
    {
        const float reach = 12.0f;
        LockActor driver;                                       // seat 0, inside
        driver.inThisVehicle = true;
        driver.driverSeat = true;
        LockActor passenger;                                    // inside without the key
        passenger.inThisVehicle = true;
        LockActor passengerKey = passenger;                     // inside, spawner
        passengerKey.spawner = true;
        LockActor spawnerNear;                                  // outside, 8 m
        spawnerNear.spawner = true;
        spawnerNear.distM = 8.0f;
        LockActor spawnerFar = spawnerNear;                     // outside, 40 m
        spawnerFar.distM = 40.0f;
        LockActor last = spawnerNear;                           // outside, last driver only
        last.spawner = false;
        last.lastDriver = true;
        LockActor stranger;                                     // outside, no key, 1 m
        stranger.distM = 1.0f;
        LockActor edge = spawnerNear;
        edge.distM = reach;
        LockActor nan = spawnerNear;
        nan.distM = std::numeric_limits<float>::quiet_NaN();
        LockActor driverFarAway = driver;                       // inside: the distance is not looked at
        driverFarAway.distM = 500.0f;
        const LockVerdict vd = JudgeLockToggle(driver, reach), vp = JudgeLockToggle(passenger, reach), vpk = JudgeLockToggle(passengerKey, reach),
                          vsn = JudgeLockToggle(spawnerNear, reach), vsf = JudgeLockToggle(spawnerFar, reach), vl = JudgeLockToggle(last, reach),
                          vx = JudgeLockToggle(stranger, reach), ve = JudgeLockToggle(edge, reach), vn = JudgeLockToggle(nan, reach),
                          vda = JudgeLockToggle(driverFarAway, reach), vinf = JudgeLockToggle(spawnerFar, 3.0e38f);
        Check("locks: the driver (inside) and a key holder inside or within reach may toggle; a passenger without the key and a stranger may not (even next to it); a key holder "
              "beyond the reach is TooFar, exactly at it is Ok, a NaN distance is TooFar, and an unlimited reach lets a distant key holder through",
              vd == LockVerdict::Ok && vp == LockVerdict::NotAllowed && vpk == LockVerdict::Ok && vsn == LockVerdict::Ok && vsf == LockVerdict::TooFar &&
                  vl == LockVerdict::Ok && vx == LockVerdict::NotAllowed && ve == LockVerdict::Ok && vn == LockVerdict::TooFar && vda == LockVerdict::Ok && vinf == LockVerdict::Ok,
              F("driver %d passenger %d inside-spawner %d near %d far %d last %d stranger %d edge %d nan %d inside-far %d unlimited %d", (int)vd, (int)vp, (int)vpk, (int)vsn,
                (int)vsf, (int)vl, (int)vx, (int)ve, (int)vn, (int)vda, (int)vinf));
    }
    {
        LockCand a, b, in1;
        a.netId = 10; a.distM = 2.0f;                            // near, no key, unlocked
        b.netId = 11; b.distM = 6.0f; b.keyed = true; b.locked = true;   // farther, key, locked
        in1.netId = 12; in1.distM = 0.0f; in1.inside = true;
        const LockCand two[] = { a, b }, withInside[] = { b, in1 }, insideFirst[] = { in1, b }, ties[] = { a, a }, unkeyed[] = { a, in1 };
        LockCand c = a;
        c.netId = 13;
        c.distM = 1.0f;
        const LockCand nearest[] = { a, c };
        const LockPick none = PickLockTarget(nullptr, 0), keyedWins = PickLockTarget(two, 2), insideWins = PickLockTarget(withInside, 2),
                       insideWins2 = PickLockTarget(insideFirst, 2), tie = PickLockTarget(ties, 2), nearestWins = PickLockTarget(nearest, 2),
                       oneOpen = PickLockTarget(&a, 1), oneLocked = PickLockTarget(&b, 1), unkeyedInside = PickLockTarget(unkeyed, 2);
        Check("locks: L acts on the car we sit in, else the nearest car we hold the key of, else the nearest car; it asks for the OPPOSITE of the flag (locked -> unlock); no car is NoCar",
              none.why == LockWhy::NoCar && none.netId == -1 && keyedWins.netId == 11 && keyedWins.keyed && insideWins.netId == 12 && insideWins.inside &&
                  insideWins2.netId == 12 && nearestWins.netId == 13 && tie.netId == 10 && oneOpen.wantLocked && !oneLocked.wantLocked && unkeyedInside.netId == 12,
              F("none %d, keyed %d, inside %d/%d, nearest %d, tie %d, open->lock %d, locked->lock %d", none.netId, keyedWins.netId, insideWins.netId, insideWins2.netId,
                nearestWins.netId, tie.netId, (int)oneOpen.wantLocked, (int)oneLocked.wantLocked));
    }
    {
        KeyRing r;
        for (int i = 1; i <= 8; ++i) r.Note(i);
        const bool full = r.Count() == 8 && r.Has(1) && r.Has(8);
        r.Note(9);                                              // the ninth drops the oldest
        const bool dropped = r.Count() == 8 && !r.Has(1) && r.Has(2) && r.Has(9);
        r.Note(5);                                              // again: moves to the front, no growth
        const bool again = r.Count() == 8;
        r.Forget(5);
        const bool forgot = r.Count() == 7 && !r.Has(5) && r.Has(9) && r.Has(2);
        r.Note(-1);
        r.Forget(77);
        const bool ignored = r.Count() == 7 && !r.Has(-1);
        r.Clear();
        Check("locks: the key ring keeps the last 8 cars (the oldest drops), a repeat does not grow it, Forget removes one, a negative id is ignored, Clear empties it",
              full && dropped && again && forgot && ignored && r.Count() == 0 && !r.Has(9));
    }
    {
        const std::string lk = LockResultText(0, true), ul = LockResultText(0, false), gen = LockResultText(200, true);
        bool distinct = true, nonEmpty = true;
        const uint8_t codes[] = { 1, 3, 5, 7, 9, 11 };
        for (const uint8_t a : codes)
        {
            const std::string ta = LockResultText(a, true);
            nonEmpty = nonEmpty && !ta.empty();
            distinct = distinct && ta != gen && ta != lk && ta != ul;
            for (const uint8_t b : codes)
                if (a != b) distinct = distinct && ta != LockResultText(b, true);
        }
        Check("locks: the chat line says \"Vehicle locked.\" / \"Vehicle unlocked.\" for an Ok, each refusal (NoVehicle, TooFar, NotAllowed, RateLimited, Dead, NoState) has its own "
              "words, and an unknown code gets the generic refusal",
              lk == "Vehicle locked." && ul == "Vehicle unlocked." && gen == "The server refused that." && distinct && nonEmpty,
              F("%s | %s | %s", lk.c_str(), ul.c_str(), gen.c_str()));
    }
    {
        const LockSound a = LockSoundFor(0, true), b = LockSoundFor(0, false), e1 = LockSoundFor(5, true), e2 = LockSoundFor(3, false);
        const bool named = a.name && a.set && b.name && b.set && e1.name && e2.name;
        Check("locks: the frontend sound differs between locking and unlocking, and every refusal plays the same error sound",
              named && std::string(a.name) != std::string(b.name) && std::string(e1.name) == std::string(e2.name) && std::string(e1.name) != std::string(a.name) &&
                  std::string(e1.name) != std::string(b.name),
              F("lock %s, unlock %s, error %s", a.name, b.name, e1.name));
    }
    Check("locks: the read-back wants 2 for a locked flag and 1 for an unlocked one; locked matches only 2 (a trapped 4 does not), unlocked matches 1 and 0 (a car the game never touched) but not 2 or 4",
          LockStateFor(true) == 2 && LockStateFor(false) == 1 && LockStatusMatches(2, 2) && !LockStatusMatches(2, 1) && !LockStatusMatches(2, 0) &&
              !LockStatusMatches(2, 4) && LockStatusMatches(1, 1) && LockStatusMatches(1, 0) && !LockStatusMatches(1, 2) && !LockStatusMatches(1, 4));

    // ---------------------------------------------------------------- C. the keeper
    {
        FakeLockCar car;
        car.status = kLockLocked;
        const KeeperRun r = RunKeeper(car, kLockLocked, 30.0, -1.0);
        Check("lock keeper: a car that keeps its lock is only read (about once a second for 30 s), nothing re-issued",
              r.stats.reads >= 29 && r.stats.reads <= 31 && r.stats.match == r.stats.reads && r.stats.mismatch == 0 && r.stats.reissued == 0 && car.sets == 0,
              F("%u reads, %u mismatches, %d calls", r.stats.reads, r.stats.mismatch, car.sets));
    }
    {
        FakeLockCar car;
        car.status = kLockLocked;
        const KeeperRun r = RunKeeper(car, kLockLocked, 30.0, 5.0);
        Check("lock keeper: a lock the game drops at 5 s is read as unlocked and put back once within 1.5 s, and the next read counts it restored",
              car.sets == 1 && r.stats.reissued == 1 && r.stats.restored == 1 && r.restoredAtS > 5.0 && r.restoredAtS <= 6.5 && car.status == kLockLocked,
              F("%d calls, restored at %.2f s, %u restored", car.sets, r.restoredAtS, r.stats.restored));
    }
    {
        FakeLockCar car;
        car.status = kLockUnlocked;
        car.ignoreSet = true;
        const KeeperRun r = RunKeeper(car, kLockLocked, 40.0, -1.0);
        Check("lock keeper: a car whose doors a script owns (the call changes nothing) is not a stream of calls: 3 tries, then left alone for 10 s, over 40 s at most 12 calls and "
              "at least 2 stuck verdicts",
              car.sets >= 3 && car.sets <= 12 && r.stats.stuck >= 2 && r.stats.mismatch > (uint32_t)car.sets && r.stats.restored == 0,
              F("%d calls, %u stuck, %u mismatches", car.sets, r.stats.stuck, r.stats.mismatch));
    }
    {
        FakeLockCar car;
        car.readable = false;
        const KeeperRun r = RunKeeper(car, kLockLocked, 10.0, -1.0);
        LockKeeper k;
        k.Observe(kLockLocked, kLockLocked, 100.0);
        const bool dueBefore = k.Due(600.0);
        k.Reset();
        const bool dueAfter = k.Due(600.0);
        Check("lock keeper: an uncallable read is counted and never acted on (no re-issue), and Reset (a new flag) makes the next read due at once",
              r.stats.readFails >= 9 && r.stats.reads == 0 && r.stats.reissued == 0 && car.sets == 0 && !dueBefore && dueAfter,
              F("%u fails, %u reads, %d calls, due before %d after %d", r.stats.readFails, r.stats.reads, car.sets, (int)dueBefore, (int)dueAfter));
    }
}

}   // namespace doorsseats
