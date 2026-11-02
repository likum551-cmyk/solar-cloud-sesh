// F / G vehicle entry and the seated drive-by decision (see vehicle_entry.h).

#include "vehicle_entry.h"

#include <cmath>

namespace gage::sync {

namespace {

    float Distance(const EntryVec& a, const EntryVec& b)
    {
        const double dx = (double)a.x - b.x, dy = (double)a.y - b.y, dz = (double)a.z - b.z;
        return (float)std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // What one key does at one car.
    struct CarChoice
    {
        EntryReason reason = EntryReason::Full;
        int         seat = -1;
        float       nearestDoorM = 1e30f;   // nearest door of any seat the model has (ranks the cars)
    };

    CarChoice Evaluate(const EntryVec& pos, const EntryCandidate& c, uint16_t me, bool wantDriver)
    {
        CarChoice r;
        int n = c.maxSeats;
        if (n > kEntrySeats) n = kEntrySeats;
        if (n < 0) n = 0;
        for (int s = 0; s < n; ++s)
        {
            const float d = Distance(pos, c.door[s]);
            if (d < r.nearestDoorM) r.nearestDoorM = d;
        }
        const bool ownedByOther = c.ownerId != kEntryNobody && c.ownerId != me;
        if (c.locked && c.ownerId != me)
        {
            r.reason = EntryReason::Locked;     // the server lets only the spawner into a locked car
            return r;
        }
        const bool driverFree = n > 0 && c.seats[0] == kEntryNobody;
        if (wantDriver && driverFree && !ownedByOther)
        {
            r.reason = EntryReason::Ok;
            r.seat = 0;
            return r;
        }
        float best = 1e30f;
        for (int s = 1; s < n; ++s)
        {
            if (c.seats[s] != kEntryNobody)
                continue;                       // taken: never jacked
            const float d = Distance(pos, c.door[s]);
            if (d < best)                       // strict: the lower seat wins a tie
            {
                best = d;
                r.seat = s;
            }
        }
        if (r.seat >= 0)
            r.reason = EntryReason::Ok;
        else if (wantDriver && driverFree)
            r.reason = EntryReason::Owned;      // only the driver's seat is left and it is somebody else's
        else
            r.reason = EntryReason::Full;
        return r;
    }

}

EntryPick PickEntry(const EntryVec& pos, const EntryCandidate* cands, int count, uint16_t me, bool wantDriver)
{
    EntryPick out;
    if (!cands || count <= 0)
        return out;                             // NoCar
    int okCar = -1, refCar = -1;
    CarChoice okChoice, refChoice;
    for (int i = 0; i < count; ++i)
    {
        const CarChoice ch = Evaluate(pos, cands[i], me, wantDriver);
        if (ch.reason == EntryReason::Ok)
        {
            if (okCar < 0 || ch.nearestDoorM < okChoice.nearestDoorM)
            {
                okCar = i;
                okChoice = ch;
            }
        }
        else if (refCar < 0 || ch.nearestDoorM < refChoice.nearestDoorM)
        {
            refCar = i;
            refChoice = ch;
        }
    }
    if (okCar >= 0)
    {
        const EntryCandidate& c = cands[okCar];
        out.netId = c.netId;
        out.seat = okChoice.seat;
        out.reason = EntryReason::Ok;
        out.door = c.door[okChoice.seat];
        out.doorDistM = Distance(pos, out.door);
        return out;
    }
    out.reason = refChoice.reason;              // the nearest car's reason
    return out;
}

const char* EntryRefusalText(EntryReason reason)
{
    switch (reason)
    {
    case EntryReason::Full:   return "That vehicle is full.";
    case EntryReason::Locked: return "That vehicle is locked.";
    case EntryReason::Owned:  return "That vehicle belongs to someone else.";
    case EntryReason::Ok:
    case EntryReason::NoCar:  break;
    }
    return "";
}

VehAim DecideVehicleAim(bool seated, uint16_t flags, bool isGun, bool taskActive, double ageMs)
{
    if (!seated)
        return VehAim::None;                    // the on-foot path owns the ped
    const bool aiming = (flags & proto::kPfAiming) != 0 && (flags & proto::kPfRagdoll) == 0;
    if (aiming && isGun)
    {
        if (!taskActive)
            return VehAim::Issue;
        return ageMs >= kVehAimReissueMs ? VehAim::Issue : VehAim::None;
    }
    return taskActive ? VehAim::Clear : VehAim::None;
}

}
