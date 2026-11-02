// How the local player gets into a networked vehicle (the F and G keys), and what a remote player
// seated in a vehicle shows while aiming a gun. Pure C++17, no game or OS headers, single-threaded
// (the engine's script tick), tested by tests/gameplay_test.cpp.
//
// OWNER RULES (the project owner's, binding):
//   * Pressing F next to a car whose driver seat is taken by a player seats you in the nearest FREE
//     passenger seat. It never jacks (never takes a seat somebody holds, never pulls anyone out).
//   * An OWNED car can only be driven by its owner: for anybody else F and G both pick a passenger
//     seat, and a car with no passenger seat left is refused as Owned (not Full).
//
// PickEntry
//   The engine hands it the player's position and every networked car in reach (EntryCandidate: who
//   sits in which wire seat, where one stands to use each seat, how many seats the model has, locked,
//   owner). It returns the car, the wire seat and why not when there is none.
//     F (wantDriver)  seat 0 when it is free AND the car is not owned by someone else; otherwise the
//                     nearest free seat >= 1 by door distance. Seat 0 wins over a nearer passenger door.
//     G               the nearest free seat >= 1 by door distance, never seat 0.
//     A locked car refuses F and G for anybody but its owner (the server lets the spawner in), with
//     Locked. A car with nothing for the key refuses with Full (Owned: F, the only free seat is the
//     driver's and the car is owned by someone else).
//   With several cars in reach the one with the nearest door is used; a car that cannot take the key
//   is skipped for a farther one that can. When none can, the reason is the nearest car's.
//   Seats at or above maxSeats do not exist. "Free" is the server's seat list (EntryCandidate::seats).
//
// DecideVehicleAim
//   One decision per frame for a SEATED remote puppet: show a drive-by aim (a gun raised through the
//   window), keep it pointed, or take it down. The engine owns the natives (TASK_DRIVE_BY once, then
//   SET_DRIVEBY_TASK_TARGET every kVehAimReissueMs; blending clears only, never
//   CLEAR_PED_TASKS_IMMEDIATELY on a seated ped, which pulls it out of the car).
#pragma once

#include <cstdint>

#include "protocol/protocol.h"

namespace gage::sync {

    constexpr int kEntrySeats = (int)proto::kVehSeats;            // wire seat 0 = driver, 1..7 passengers
    constexpr uint16_t kEntryNobody = proto::kNoPlayer;           // "nobody" in a seat or owner field

    struct EntryVec { float x = 0, y = 0, z = 0; };

    // One networked car in reach, as the engine read it.
    struct EntryCandidate
    {
        int      netId = -1;                                      // the server's vehicle id
        uint16_t seats[kEntrySeats] = { kEntryNobody, kEntryNobody, kEntryNobody, kEntryNobody,
                                        kEntryNobody, kEntryNobody, kEntryNobody, kEntryNobody };
                                                                  // occupant player id per wire seat, kEntryNobody = free
        EntryVec door[kEntrySeats];                               // world position to stand at to use each seat
        int      maxSeats = 0;                                    // wire seats the model has (passengers + the driver)
        bool     locked = false;                                  // doors locked (VehicleState.flags kVehLocked)
        uint16_t ownerId = kEntryNobody;                          // the player who owns it; kEntryNobody = unowned
    };

    enum class EntryReason : uint8_t { Ok, NoCar, Full, Locked, Owned };

    struct EntryPick
    {
        int         netId = -1;                                   // the car (-1 unless Ok)
        int         seat = -1;                                    // its wire seat (-1 unless Ok)
        EntryReason reason = EntryReason::NoCar;
        EntryVec    door;                                         // where to stand for `seat` (only with Ok)
        float       doorDistM = 0;                                // from `pos` to `door` (only with Ok)
    };

    // `cands` may be null with count 0 (NoCar). `me` is the local player's id.
    EntryPick PickEntry(const EntryVec& pos, const EntryCandidate* cands, int count, uint16_t me, bool wantDriver);

    // The chat line for a refusal ("" for Ok and NoCar).
    const char* EntryRefusalText(EntryReason reason);

    // ---- seated puppet drive-by --------------------------------------------------------------

    enum class VehAim : uint8_t { None, Issue, Clear };

    // A running drive-by task is pointed at the new aim again every 500 ms (every issue restarts the
    // aim blend, so not every frame), the same cadence as the on-foot aim.
    constexpr double kVehAimReissueMs = 500.0;

    // seated    the puppet sits in a synced vehicle here
    // flags     its PedState flags (proto::kPf*): kPfAiming and not kPfRagdoll count as aiming
    // isGun     the weapon in hand fires bullets (not unarmed, not thrown / explosive)
    // taskActive the engine issued a drive-by (or its fallback aim) that is still running
    // ageMs     since that task was last issued (only read when taskActive)
    // Issue: start it (no task yet) or point it again (>= kVehAimReissueMs). Clear: take it down
    // (stopped aiming, put the gun away, ragdolling). None: nothing to do; also always None when
    // not seated (the on-foot path owns the ped).
    VehAim DecideVehicleAim(bool seated, uint16_t flags, bool isGun, bool taskActive, double ageMs);

}
