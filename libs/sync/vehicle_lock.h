// Vehicle door locks (the L key) and the rear-seat bookkeeping of the G key. Pure C++17, header-only, no game or OS headers, tested by
// tests/doors_seats_tests.h (gameplay_test) and tests/doors_seats_net_tests.h (the real server and session). 0.4.22.
//
// NO PROTOCOL CHANGE. The lock is VehicleState.flags bit 0 (proto::kVehLocked, server-owned, already in every full and motion-only
// entry), it is toggled with VehicleOp::SetLocked (arg 1 locks, 0 unlocks) and refused with the existing VehicleResultCode values
// (Locked, NotAllowed, TooFar, NoVehicle, Dead, NoState, RateLimited); the seat claims are the existing Enter / EnterIntent with a wire seat
// 0..7. This header only holds the RULES both sides apply, so the server and the client cannot disagree about them by accident.
//
// WHO HOLDS THE KEY (the server's rule, VehicleStore::SetLocked / Enter / Reserve):
//   * the player in the driver's seat of the vehicle (seat 0) may lock and unlock it, wherever the rest of the car is;
//   * the SPAWNER (the player whose Spawn or Register created it) and the LAST DRIVER (the player who last held seat 0, a server-side
//     field that is never on the wire) hold the key: they may lock and unlock it from anywhere inside it, or from outside within the
//     lock distance (server.json vehicleLockDistance, default 12 m), and they may enter it while it is locked;
//   * a passenger without the key, and everybody else, gets NotAllowed; a key holder outside the lock distance gets TooFar.
//   A locked vehicle refuses every other player's Enter / EnterIntent with Locked (a seat change inside the same car is never refused:
//   occupants can sit anywhere and always get out).
//
// THE CLIENT (apps/engine/engine.cpp, TickDoorsSeats): L picks the car (PickLockTarget), asks the server (SetLocked, toggling what
// the replicated flag says), and shows the answer. The client never decides the lock on its own: the flag it sees comes back
// in the VehicleWorld, and every copy gets SET_VEHICLE_DOORS_LOCKED(2 locked / 1 unlocked) from it. LockKeeper reads the lock status
// back with GET_VEHICLE_DOOR_LOCK_STATUS and puts back a lock the game dropped.
#pragma once

#include <cmath>
#include <cstdint>

#include "protocol/protocol.h"

namespace gage::sync {

    // ---- the server's rule --------------------------------------------------------------------------------------------------

    enum class LockVerdict : uint8_t { Ok, NotAllowed, TooFar };

    // The key: the spawner or the last driver.
    constexpr bool HoldsKey(bool spawner, bool lastDriver) { return spawner || lastDriver; }

    // A locked vehicle admits a player who holds the key (Enter / EnterIntent for a player who is not in it already).
    constexpr bool MayEnterLocked(bool spawner, bool lastDriver) { return HoldsKey(spawner, lastDriver); }

    struct LockActor
    {
        bool  inThisVehicle = false;   // the player sits in some seat of the vehicle (the server's occupancy)
        bool  driverSeat = false;      // ... in seat 0
        bool  spawner = false;
        bool  lastDriver = false;
        float distM = 0;               // from the player's last server-known position to the vehicle (ignored while inThisVehicle)
    };

    // SetLocked: the driver or a key holder; a player outside the car must also be within `reachM` of it.
    inline LockVerdict JudgeLockToggle(const LockActor& a, float reachM)
    {
        if (!(a.driverSeat || HoldsKey(a.spawner, a.lastDriver)))
            return LockVerdict::NotAllowed;
        if (!a.inThisVehicle && !(a.distM <= reachM))      // written so that a NaN distance is TooFar, never Ok
            return LockVerdict::TooFar;
        return LockVerdict::Ok;
    }

    // ---- the client's pick ---------------------------------------------------------------------------------------------------

    // One networked vehicle in reach of the local player, as the engine read it.
    struct LockCand
    {
        int   netId = -1;
        float distM = 0;               // from the player to the vehicle's origin
        bool  locked = false;          // the replicated flag
        bool  keyed = false;           // we believe we hold its key (we own it, or we drove it: KeyRing)
        bool  inside = false;          // the local ped sits in it
    };

    enum class LockWhy : uint8_t { Ok, NoCar };

    struct LockPick
    {
        int     netId = -1;
        bool    wantLocked = false;    // the state to ask for: the opposite of what the flag says now
        bool    keyed = false;
        bool    inside = false;
        LockWhy why = LockWhy::NoCar;
    };

    // The car we sit in wins; otherwise the nearest car we believe we hold the key of; otherwise the nearest car (the server judges, and
    // answers NotAllowed to a stranger). Ties go to the lower index (the engine's order is the server id's).
    inline LockPick PickLockTarget(const LockCand* cands, int count)
    {
        LockPick out;
        if (!cands || count <= 0)
            return out;
        int best = -1;
        for (int i = 0; i < count; ++i)
        {
            const LockCand& c = cands[i];
            if (c.inside) { best = i; break; }
            if (best < 0) { best = i; continue; }
            const LockCand& b = cands[best];
            const bool better = (c.keyed && !b.keyed) || (c.keyed == b.keyed && c.distM < b.distM);
            if (better) best = i;
        }
        const LockCand& w = cands[best];
        out.netId = w.netId;
        out.wantLocked = !w.locked;
        out.keyed = w.keyed;
        out.inside = w.inside;
        out.why = LockWhy::Ok;
        return out;
    }

    // The vehicles the local player holds the key of by what happened on this client: the ones it sat in the driver's seat of (the server's
    // lastDriver) and the ones it owns come from the state's ownerId. The last kCap, oldest dropped. It only ORDERS the L pick between two
    // cars in reach; the server's answer is the rule.
    class KeyRing
    {
    public:
        static constexpr int kCap = 8;
        void Note(int netId)
        {
            if (netId < 0) return;
            int at = -1;
            for (int i = 0; i < n_; ++i)
                if (ids_[i] == netId) { at = i; break; }
            if (at == 0) return;
            if (at < 0) at = n_ < kCap ? n_++ : kCap - 1;
            for (int i = at; i > 0; --i) ids_[i] = ids_[i - 1];
            ids_[0] = netId;
        }
        bool Has(int netId) const
        {
            for (int i = 0; i < n_; ++i)
                if (ids_[i] == netId) return true;
            return false;
        }
        void Forget(int netId)
        {
            for (int i = 0; i < n_; ++i)
                if (ids_[i] == netId)
                {
                    for (int j = i + 1; j < n_; ++j) ids_[j - 1] = ids_[j];
                    --n_;
                    return;
                }
        }
        void Clear() { n_ = 0; }
        int Count() const { return n_; }
    private:
        int ids_[kCap] = {};
        int n_ = 0;
    };

    // ---- the words ------------------------------------------------------------------------------------------------------------

    // The chat / HUD line for the server's answer to a SetLocked (code = proto::VehicleResultCode). `wantLocked` is what we asked for.
    inline const char* LockResultText(uint8_t code, bool wantLocked)
    {
        switch (code)
        {
        case 0:  return wantLocked ? "Vehicle locked." : "Vehicle unlocked.";
        case 1:  return "That vehicle no longer exists.";                // NoVehicle
        case 3:  return "You are too far from that vehicle.";            // TooFar
        case 5:  return "You do not have the key to that vehicle.";      // NotAllowed
        case 7:  return "Slow down, try again in a moment.";             // RateLimited
        case 9:  return "You cannot do that while dead.";                // Dead
        case 11: return "The server has no position of yours yet.";      // NoState
        }
        return "The server refused that.";
    }

    // PLAY_SOUND_FRONTEND names (the frontend soundset needs no audio bank request). From the community sound lists, not from a crossmap:
    // live-unverified; a name the game does not know is silent, not an error.
    struct LockSound { const char* name; const char* set; };
    inline LockSound LockSoundFor(uint8_t code, bool wantLocked)
    {
        if (code != 0) return { "ERROR", "HUD_FRONTEND_DEFAULT_SOUNDSET" };
        return wantLocked ? LockSound{ "SELECT", "HUD_FRONTEND_DEFAULT_SOUNDSET" } : LockSound{ "BACK", "HUD_FRONTEND_DEFAULT_SOUNDSET" };
    }

    // ---- the read-back --------------------------------------------------------------------------------------------------------

    // eVehicleLockState values this client sets: 1 unlocked, 2 locked (SET_VEHICLE_DOORS_LOCKED). A car the game never touched can read 0
    // (NONE), which is unlocked. 2 is the state in which the player inside can still get out (4 = LOCKED_PLAYER_INSIDE would trap him).
    constexpr int kLockUnlocked = 1, kLockLocked = 2;
    constexpr int LockStateFor(bool locked) { return locked ? kLockLocked : kLockUnlocked; }
    constexpr bool LockStatusMatches(int want, int status)
    {
        return want == kLockLocked ? status == kLockLocked : (status == kLockUnlocked || status == 0);
    }

    struct LockKeepStats
    {
        uint32_t reads = 0, readFails = 0, match = 0, mismatch = 0, reissued = 0, stuck = 0, restored = 0;
    };

    // One per copy: reads GET_VEHICLE_DOOR_LOCK_STATUS about once a second (Due) and, when it differs from what the state says, asks for the
    // lock to be set again; after kMaxReissues in a row that changed nothing the car is let alone for kBackoffMs (a script or the game owns its
    // doors: counted as stuck, never a stream of calls).
    class LockKeeper
    {
    public:
        enum class Act : uint8_t { None, Reissue };
        static constexpr double kReadEveryMs = 1000.0;
        static constexpr double kBackoffMs = 10000.0;
        static constexpr int kMaxReissues = 3;

        bool Due(double nowMs) const { return nowMs >= nextReadMs_; }

        // `want` kLockUnlocked / kLockLocked; `status` the native's answer, -1 = it could not be called.
        Act Observe(int want, int status, double nowMs)
        {
            nextReadMs_ = nowMs + kReadEveryMs;
            if (status < 0)
            {
                ++stats_.readFails;
                return Act::None;
            }
            ++stats_.reads;
            if (LockStatusMatches(want, status))
            {
                ++stats_.match;
                if (reissuedRun_ > 0) ++stats_.restored;
                reissuedRun_ = 0;
                return Act::None;
            }
            ++stats_.mismatch;
            if (nowMs < backoffUntilMs_)
                return Act::None;
            if (reissuedRun_ >= kMaxReissues)
            {
                ++stats_.stuck;
                backoffUntilMs_ = nowMs + kBackoffMs;
                reissuedRun_ = 0;
                return Act::None;
            }
            ++reissuedRun_;
            ++stats_.reissued;
            return Act::Reissue;
        }
        // The state changed (a new lock or unlock): the next read is due at once and a back-off is over.
        void Reset()
        {
            nextReadMs_ = 0;
            backoffUntilMs_ = 0;
            reissuedRun_ = 0;
        }
        const LockKeepStats& Stats() const { return stats_; }
    private:
        double nextReadMs_ = 0, backoffUntilMs_ = 0;
        int reissuedRun_ = 0;
        LockKeepStats stats_;
    };

}
