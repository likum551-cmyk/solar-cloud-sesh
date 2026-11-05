// VehicleStore: the server's networked vehicles (protocol v4, v5). Pure logic over the data it is fed (no
// sockets, no clocks of its own), header-only so the server core needs no new translation unit.
//
// What it owns, and the rules (docs/api/protocol.md "VehicleCmd", "VehicleUpdate", "VehicleWorld"):
//  * v5 ownership: a vehicle a player Spawns is OWNED by that player (Vehicle::owned, VehicleState::ownerId):
//    nobody else may take its driver's seat (Enter seat 0 -> NotOwner, checked before the seat shuffle and
//    SeatTaken; a passenger seat is open to anybody). The owner gives that up with Release. A vehicle that was
//    adopted (Register), spawned by the server or orphaned is not owned. When the owner disconnects an empty
//    vehicle is destroyed (as before) and an occupied one becomes an unowned orphan.
//  * v5 controller: exactly one client sends the motion of a vehicle (VehicleState::ctrl). The player in seat 0
//    is the controller; when the driver gets out it stays the controller while it is within ctrlStickyMeters;
//    otherwise AssignControllers hands the vehicle to the nearest trusted player in view (within ctrlNewMeters),
//    so an empty car can be pushed and damaged. A driverless controller that sends nothing for ctrlSilenceMs is
//    skipped for ctrlSkipMs. With nobody to control it, the vehicle stands still.
//  * Existence: Create (Spawn by a player, or Register = adopt a local ambient car) under a global cap
//    (Rules::maxVehicles), a per-player cap (Rules::perPlayer) and a per-address cap (Rules::perIp), each
//    counted separately for spawned and adopted vehicles; a player or address at the adopted cap gives up
//    its oldest EMPTY adopted vehicle first, but only once the new vehicle is certain to fit (a free id
//    exists and the global cap has room), so a refused Create never costs a car. Ids are handed out
//    round-robin over 0..kMaxVehicles-1, so a destroyed id is not reused while its "removed" notice is
//    still owed to a receiver. An empty vehicle is destroyed after Rules::emptyDespawnMs.
//  * Occupancy (the only truth about who sits where): Enter checks the seat range, that the seat is
//    free, the distance (Rules::enterDistance from the player's last server-known position) and the
//    lock (the spawner and, 0.4.22, the LAST DRIVER may still enter a locked vehicle: libs/sync/vehicle_lock.h); a player sitting
//    elsewhere leaves that vehicle first. Exit frees the seat; a driver's vehicle parks with its last state (velocities and
//    steering zeroed, horn off). PlayerGone (disconnect) frees the seat; the player's EMPTY vehicles are
//    destroyed with it and the ones somebody else sits in become orphans (no spawner, but still counted
//    against the address) that go Rules::orphanMs after they empty.
//  * T1 seats (wire v6) INTENTS: Reserve (VehicleOp::EnterIntent) holds a seat for a player who is on the way to it
//    (at most one reservation per player, kIntentTtlMs long, never in the seat list), so a second player's Enter or
//    Reserve of the same seat is refused SeatTaken while the first walks in. Every change (Reserved, Seated when the
//    player's Enter takes the reserved seat, Cancelled with a reason, Exited for an announced Exit) is queued as a
//    proto::SeatNotice for the caller to deliver (TakeNotices); the store has no sockets. Occupancy is still only
//    ever the seat list: a reservation seats nobody.
//  * Motion: Update accepts a VehicleUpdate only from the vehicle's controller (v4: the player in seat 0), finite,
//    in the world box, with a reported velocity under Rules::maxSpeed; it copies the motion, the driver flags
//    and (v5) flags2 and doors; health only goes down. The sample time the caller passes (the sender's
//    own clock on the server's, libs/sync/sampleclock.h) is what BuildFrame's ageMs is measured from. The position is judged by DISPLACEMENT against the server time since the last one
//    (anticheat.h AllowedMove): a jump is taken anyway but reported, so the caller can distrust the driver.
//    Speed-based decisions (fuel, "is it moving") use the displacement, never the reported velocity.
//  * Server-owned fields: lock, lights, colours (driver or spawner), fuel (drains with the seconds driven
//    with the engine on while the displacement says it moves; at 0 the engine is switched off), health (hits).
//  * Replication: BuildFrame makes one receiver's VehicleWorld: the complete set of vehicles within the
//    relevance radius (enter / leave hysteresis) plus the one it sits in, and full states for those
//    due: newcomers and changed vehicles for 3 frames, driven vehicles at the distance rate (except to
//    their own controller), parked ones about once a second; bounded by the packet cap (the presence
//    list always fits; entries are taken changed-first, then nearest-first; the rest stay due). v5: an entry
//    is the full state for a newcomer, for 3 frames after a change and about once a second, and the
//    26-byte motion-only entry otherwise; every entry carries its sample's age.
//    Destroyed ids are owed to every receiver for 3 frames, at most 255 per frame (the wire's u8 count),
//    the rest carried to the next frames.
//
// Thread-safety: none (the server's Poll thread). Errors: none throw; refusals are result codes.
#pragma once

#include "anticheat.h"
#include "protocol/protocol.h"
#include "protocol/quant.h"
#include "sync/vehicle_lock.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gage::server {

    class VehicleStore
    {
    public:
        using Code = proto::VehicleResultCode;
        static constexpr uint16_t kNone = 0xFFFF; // "no vehicle"

        struct Rules
        {
            uint16_t maxVehicles = 64;          // alive at once, 1..kMaxVehicles
            uint16_t perPlayer = 3;             // spawned (and, separately, adopted) per player
            uint16_t perIp = 6;                 // spawned (and, separately, adopted) per source address, orphans included
            uint32_t emptyDespawnMs = 600000;   // 0 = never
            uint32_t orphanMs = 60000;          // an orphan (its spawner left while it was occupied) goes this long after it empties
            float    enterDistance = 10.0f;     // m
            float    maxSpeed = 139.0f;         // m/s, magnitude (the server sets it from Config::vehicleMaxSpeed; this default matches it)
            uint32_t fuelDrainPerMinute = 10;   // permille while the engine runs and the vehicle moves
            // replication
            float    enterRadius = 400.0f, leaveRadius = 450.0f;
            float    nearDist = 120.0f, midDist = 250.0f;
            uint32_t nearEvery = 1, midEvery = 2, farEvery = 4; // ticks between full states of a driven vehicle
            uint32_t refreshTicks = 20;                          // ticks between full states of a parked one (and of the statics of a driven one)
            size_t   packetMaxBytes = 1200;
            // v5 controller assignment (AssignControllers)
            float    ctrlNewMeters = 200.0f;     // a player may BECOME the controller of a driverless vehicle within this distance
            float    ctrlStickyMeters = 250.0f;  // the current controller keeps it out to this distance (hysteresis)
            uint32_t ctrlSilenceMs = 1500;       // a driverless controller silent this long is skipped...
            uint32_t ctrlSkipMs = 10000;         // ...for this long
            uint32_t movedWithinMs = 1000;       // MovedRecently window that makes a driverless vehicle "driven" for the send rate
            // T4 (vehicle collisions): a driverless vehicle is simulated by one client, and every other client sees a kinematic copy that
            // cannot be pushed. So a driven vehicle that is about to hit a driverless one TAKES its control (ClaimFor): the rammer's own
            // physics then resolves the contact natively. The approach test: the driver is faster than claimMinSpeed, heads for the
            // vehicle (closing speed above 1 m/s) and is within claimBaseMeters + claimLookSeconds * speed of it; a claim holds
            // claimHoldMs before it can change again. 0 claimBaseMeters switches the rule off.
            float    claimBaseMeters = 10.0f;
            float    claimLookSeconds = 0.5f;
            float    claimMinSpeed = 3.0f;       // m/s
            uint32_t claimHoldMs = 1500;
            // T4: a reported velocity above maxSpeed but below maxSpeed * maxSpeedSoftFactor is CLAMPED to maxSpeed and accepted (a hyper car's
            // overshoot must not freeze it for everybody else); above that it is BadArg as before. The position is still judged by displacement.
            float    maxSpeedSoftFactor = 2.0f;
            // R3 T4 (vehicle damage): a controller's report that ADDS a smashed window, a broken door / headlight, a dent or a lower petrol tank is
            // taken only when the car shows some damage anywhere (the server's or the report's body health is below cleanHealth, or a Hit
            // already named the bit): a clean car with a window "smashed" in every update was the round-2 live read of a slot that never was one.
            // Defence in depth behind the client's own baselines (libs/sync/vehicle_damage.h); false = trust the report as in v6.
            bool     damageNeedsHealthLoss = true;
            uint16_t cleanHealth = 995;
            // 0.4.22 door locks (server.json vehicleLocks / vehicleLockDistance): false = SetLocked is refused NotAllowed (nothing is ever locked);
            // lockDistance = how close (m, to the vehicle's last server position) a key holder OUTSIDE it must be to lock or unlock it, 0 = any distance.
            bool     locksEnabled = true;
            float    lockDistance = 12.0f;
        };

        // One player as AssignControllers sees them: where they are, whether the server has a position of theirs, and
        // whether the movement check trusts them (not inside an untrusted window).
        struct ControlCandidate
        {
            uint16_t id = proto::kNoPlayer;
            proto::Kinematics pos;
            bool hasState = false;
            bool trusted = true;
        };

        struct Vehicle
        {
            bool exists = false;
            proto::VehicleState s;
            uint16_t spawner = proto::kNoPlayer;
            bool owned = false;        // v5: only `spawner` may take the driver's seat (s.ownerId mirrors it for the wire)
            uint64_t recvMs = 0;       // server time of the last accepted VehicleUpdate (the displacement budget); posMs is the SAMPLE time
            uint64_t ctrlSinceMs = 0;  // when s.ctrl was assigned (or the driver left)
            uint64_t ctrlUpdateMs = 0; // receipt time of the controller's last accepted update since then (0 = none yet)
            struct Skip { uint16_t player = proto::kNoPlayer; uint64_t untilMs = 0; };
            Skip skips[4];             // controllers that went silent, and until when they are not chosen again
            uint8_t nextSkip = 0;
            uint32_t ownerKey = 0;     // the spawner's source address (0 = unknown): the per-address caps count it, also after the spawner left
            bool adopted = false;
            bool orphan = false;       // its spawner left while somebody sat in it
            uint16_t lastDriver = proto::kNoPlayer; // 0.4.22: the player who last took seat 0 (never on the wire); holds the key with the spawner; cleared when it disconnects
            uint64_t createdMs = 0;
            uint64_t emptySinceMs = 0; // valid while nobody sits in it
            uint64_t posMs = 0;        // server time the last accepted sample was TAKEN at (the sender's clock placed on the server's), or the creation time
            bool moved = false;        // fastMs is valid
            uint64_t fastMs = 0;       // server time the displacement last said it moved at least ac::kMovingSpeed
            uint32_t changedTick = 0;  // a server-owned field, the occupancy or a park changed at this tick
            double fuelAcc = 0;        // fractional permille drained so far
            double driveSec = 0;       // seconds driven with the engine on (by displacement) since the last Tick
            uint16_t resBy[proto::kVehSeats] = {proto::kNoPlayer, proto::kNoPlayer, proto::kNoPlayer, proto::kNoPlayer,
                                                proto::kNoPlayer, proto::kNoPlayer, proto::kNoPlayer, proto::kNoPlayer}; // T1: who holds each seat's reservation
            // v6 visual damage: s.tyres / s.tyresRim / s.windows are the merged truth (the controller's report plus the Hits that
            // popped something of this car). visPendMs[group * 8 + slot] (group 0 tyre, 1 rim, 2 window) is when a Hit set a bit
            // the controller has not shown yet, plus 1 (0 = none): see MergeVisual.
            uint64_t visPendMs[3 * proto::kVehWheelSlots] = {};
        };

        // T1 seats: one player's pending reservation (at most one each).
        struct Intent
        {
            uint16_t veh = 0xFFFF;     // kNone = no reservation
            uint8_t  seat = 0;
            bool     run = false;
            uint64_t sinceMs = 0;
        };

        struct Seat { uint16_t veh = kNone; uint8_t seat = 0; };

        void Configure(const Rules& r, uint16_t maxPlayers)
        {
            rules_ = r;
            if (rules_.maxVehicles < 1) rules_.maxVehicles = 1;
            if (rules_.maxVehicles > proto::kMaxVehicles) rules_.maxVehicles = proto::kMaxVehicles;
            if (rules_.leaveRadius < rules_.enterRadius) rules_.leaveRadius = rules_.enterRadius;
            for (uint32_t* e : {&rules_.nearEvery, &rules_.midEvery, &rules_.farEvery, &rules_.refreshTicks})
                if (*e < 1) *e = 1;
            v_.assign(proto::kMaxVehicles, Vehicle());
            occ_.assign(maxPlayers, Seat());
            intent_.assign(maxPlayers, Intent());
            notices_.clear();
            audiences_.clear();
            rx_.assign(maxPlayers, Rx());
            removed_.clear();
            nextId_ = 0;
            count_ = 0;
            lastTick_ = 0;
        }
        const Rules& rules() const { return rules_; }

        // ---- receivers ----
        void AddReceiver(uint16_t player)
        {
            if (player >= rx_.size()) return;
            rx_[player] = Rx();
            rx_[player].active = true;
            rx_[player].v.assign(proto::kMaxVehicles, RxVeh());
            occ_[player] = Seat();
        }
        void RemoveReceiver(uint16_t player)
        {
            if (player >= rx_.size()) return;
            rx_[player] = Rx();
        }

        // ---- queries ----
        size_t Count() const { return count_; }
        const Vehicle* Get(uint16_t id) const { return id < v_.size() && v_[id].exists ? &v_[id] : nullptr; }
        Seat Occupancy(uint16_t player) const { return player < occ_.size() ? occ_[player] : Seat(); }
        // T1: the player's pending reservation (veh == kNone: none).
        Intent PendingIntent(uint16_t player) const { return player < intent_.size() ? intent_[player] : Intent(); }
        // T1: who holds the reservation on (id, seat), kNoPlayer when nobody (a reservation older than kIntentTtlMs counts as gone).
        uint16_t ReservedBy(uint16_t id, uint32_t seat, uint64_t nowMs) const
        {
            if (id >= v_.size() || !v_[id].exists || seat >= proto::kVehSeats) return proto::kNoPlayer;
            const uint16_t p = v_[id].resBy[seat];
            if (p == proto::kNoPlayer || p >= intent_.size()) return proto::kNoPlayer;
            return nowMs >= intent_[p].sinceMs && nowMs - intent_[p].sinceMs >= proto::kIntentTtlMs ? proto::kNoPlayer : p;
        }
        // T1: the SeatNotices queued since the last call (oldest first); the caller delivers them. `audiences` (optional) gets, per notice, the
        // players to tell: the actor plus every receiver that had the vehicle in its relevant set WHEN THE NOTICE WAS QUEUED (so the notice about a
        // vehicle being destroyed still reaches the players who had it, though the destroy has cleared their relevance since).
        std::vector<proto::SeatNotice> TakeNotices(std::vector<std::vector<uint16_t>>* audiences = nullptr)
        {
            std::vector<proto::SeatNotice> out;
            out.swap(notices_);
            if (audiences)
                audiences->swap(audiences_);
            else
                audiences_.clear();
            return out;
        }
        size_t PendingNotices() const { return notices_.size(); }
        // True when vehicle `id` is in `player`'s relevant set (what its client is told about).
        bool RelevantTo(uint16_t player, uint16_t id) const
        {
            return player < rx_.size() && rx_[player].active && id < proto::kMaxVehicles && rx_[player].v[id].relevant;
        }
        size_t SpawnedBy(uint16_t player, bool adopted) const
        {
            size_t n = 0;
            for (const Vehicle& v : v_)
                if (v.exists && v.spawner == player && v.adopted == adopted) n++;
            return n;
        }
        // Vehicles created from source address `ownerKey` (orphans included), spawned or adopted.
        size_t OwnedBy(uint32_t ownerKey, bool adopted) const
        {
            size_t n = 0;
            for (const Vehicle& v : v_)
                if (v.exists && v.ownerKey == ownerKey && v.adopted == adopted) n++;
            return n;
        }
        // The nearest networked vehicle's distance to `pos` (a huge number when there is none).
        float NearestVehicleDistance(const proto::Kinematics& pos) const
        {
            float best = 1e30f;
            for (const Vehicle& v : v_)
                if (v.exists) best = std::min(best, ac::Dist(pos, v.s.k));
            return best;
        }

        // ---- existence ----
        // A new vehicle at `pos` (velocity ignored) rotated `rot`, created by `spawner` from source address
        // `ownerKey` (0 = unknown; no per-address cap then). Limit when a cap is reached.
        Code Create(uint32_t model, const proto::Kinematics& pos, const proto::Quat& rot, uint8_t colour1, uint8_t colour2,
                    uint16_t spawner, uint32_t ownerKey, bool adopted, uint64_t nowMs, uint32_t tick, uint16_t* outId,
                    std::vector<uint16_t>* despawned = nullptr)
        {
            if (model == 0) return Code::BadArg;
            // Does somebody have to make room? The per-player cap and the per-address cap are each served by
            // giving up an EMPTY adopted vehicle (ambient cars are cheap); spawned ones are never taken away.
            const bool playerFull = spawner != proto::kNoPlayer && SpawnedBy(spawner, adopted) >= rules_.perPlayer;
            const bool ipFull = ownerKey != 0 && OwnedBy(ownerKey, adopted) >= rules_.perIp;
            uint16_t evict = kNone;
            if (playerFull || ipFull)
            {
                if (!adopted) return Code::Limit;
                evict = OldestEmptyAdopted(playerFull ? spawner : proto::kNoPlayer, ownerKey);
                if (evict == kNone) return Code::Limit;
            }
            // Everything that can still refuse is checked BEFORE the eviction: the global cap (the evicted
            // vehicle frees one place) and a free id (the evicted id itself cannot be reused yet).
            if (count_ - (evict != kNone ? 1 : 0) >= rules_.maxVehicles) return Code::Limit;
            uint16_t id = kNone;
            for (uint32_t i = 0; i < proto::kMaxVehicles; i++)
            {
                const uint16_t c = (uint16_t)((nextId_ + i) % proto::kMaxVehicles);
                if (!v_[c].exists && !RecentlyRemoved(c, tick) && !NoticeOwed(c))
                {
                    id = c;
                    break;
                }
            }
            if (id == kNone) return Code::Limit;
            if (evict != kNone)
            {
                Destroy(evict, nowMs, tick, nullptr);
                if (despawned) despawned->push_back(evict);
            }
            nextId_ = (uint16_t)((id + 1) % proto::kMaxVehicles);
            NoteTick(tick);
            Vehicle& V = v_[id];
            V = Vehicle();
            V.exists = true;
            V.s.id = id;
            V.s.model = model;
            V.s.k.x = pos.x;
            V.s.k.y = pos.y;
            V.s.k.z = pos.z;
            V.s.rot = rot;
            V.s.k.heading = proto::QuatHeading(rot);
            V.s.colour1 = colour1;
            V.s.colour2 = colour2;
            V.spawner = spawner;
            V.ownerKey = ownerKey;
            V.adopted = adopted;
            V.owned = spawner != proto::kNoPlayer && !adopted; // v5: a player's Spawn is theirs to drive
            V.s.ownerId = V.owned ? spawner : proto::kNoPlayer;
            V.s.ctrl = proto::kNoPlayer;
            V.createdMs = nowMs;
            V.emptySinceMs = nowMs;
            V.posMs = nowMs;
            V.recvMs = nowMs;
            V.changedTick = tick;
            count_++;
            if (outId) *outId = id;
            return Code::Ok;
        }

        // Removes vehicle `id`: its occupants are put out (listed in `ejected`) and every receiver is
        // told (removed, 3 frames).
        Code Destroy(uint16_t id, uint64_t /*nowMs*/, uint32_t tick, std::vector<uint16_t>* ejected)
        {
            if (id >= v_.size() || !v_[id].exists) return Code::NoVehicle;
            Vehicle& V = v_[id];
            for (size_t s = 0; s < proto::kVehSeats; s++)
            {
                const uint16_t p = V.s.seats[s];
                if (p == proto::kNoPlayer) continue;
                if (p < occ_.size()) occ_[p] = Seat();
                if (ejected) ejected->push_back(p);
            }
            for (size_t s = 0; s < proto::kVehSeats; s++)   // T1: whoever was on the way in is not any more
                if (V.resBy[s] != proto::kNoPlayer)
                    DropIntent(V.resBy[s], proto::SeatCancelReason::VehicleGone);
            V = Vehicle();
            count_--;
            removed_.push_back(Removed{id, tick});
            for (Rx& R : rx_)
                if (R.active)
                {
                    R.v[id] = RxVeh();
                    R.owed.push_back(Owed{id, 0});
                }
            return Code::Ok;
        }

        // ---- occupancy ----
        Code Enter(uint16_t player, uint16_t id, uint32_t seat, const proto::Kinematics& playerPos, uint64_t nowMs, uint32_t tick)
        {
            if (player >= occ_.size()) return Code::NotAllowed;
            if (id >= v_.size() || !v_[id].exists) return Code::NoVehicle;
            if (seat >= proto::kVehSeats) return Code::BadArg;
            Vehicle& V = v_[id];
            // v5: the driver's seat of an owned vehicle is its owner's. First of all the checks: GTA's own seat
            // shuffle (a passenger sliding over to an empty driver's seat) and the occupied seat are refused with
            // this answer too, so the client knows why and does not retry.
            if (seat == 0 && V.owned && V.spawner != player) return Code::NotOwner;
            if (V.s.seats[seat] == player) return Code::Ok; // already there
            if (V.s.seats[seat] != proto::kNoPlayer) return Code::SeatTaken;
            {
                // T1: a seat somebody else is walking to is theirs until they sit, cancel or time out.
                const uint16_t held = ReservedBy(id, seat, nowMs);
                if (held != proto::kNoPlayer && held != player) return Code::SeatTaken;
            }
            const Seat cur = occ_[player];
            if (cur.veh != id) // a seat shuffle inside the same vehicle skips the door checks
            {
                if (Dist2(playerPos, V.s.k) > rules_.enterDistance * rules_.enterDistance) return Code::TooFar;
                if ((V.s.flags & proto::kVehLocked) && !sync::MayEnterLocked(V.spawner == player, V.lastDriver == player)) return Code::Locked;
            }
            NoteTick(tick);
            if (cur.veh != kNone) Leave(player, nowMs, tick);
            ConsumeIntent(player, id, (uint8_t)seat);   // T1: the walk ends here: Seated for the seat it was for, else Cancelled
            V.s.seats[seat] = player;
            occ_[player] = Seat{id, (uint8_t)seat};
            V.changedTick = tick;
            if (seat == 0)
            {
                V.lastDriver = player;                  // 0.4.22: the last driver holds the key (SetLocked, entering a locked vehicle)
                SetController(V, player, nowMs, tick);  // the driver controls; the displacement budget starts now
            }
            return Code::Ok;
        }

        // v5: gives up the ownership of vehicle `id` (its owner only): anybody may drive it from then on. NotAllowed
        // for anybody else and for a vehicle that is not owned (nothing to release).
        Code Release(uint16_t player, uint16_t id, uint32_t tick)
        {
            Vehicle* V = Mutable(id);
            if (!V) return Code::NoVehicle;
            if (!V->owned || V->spawner != player) return Code::NotAllowed;
            V->owned = false;
            V->s.ownerId = proto::kNoPlayer;
            V->changedTick = tick;
            return Code::Ok;
        }

        // Leaves vehicle `id` (the one the player sits in). T1: `announce` (a player's own Exit command, not a death or a
        // respawn) queues SeatNotice::Exited so every screen plays the normal exit animation; an Exit while the player only
        // holds a reservation on `id` cancels it (Ok).
        Code Exit(uint16_t player, uint16_t id, uint64_t nowMs, uint32_t tick, bool announce = false)
        {
            if (player >= occ_.size()) return Code::NotAllowed;
            if (id >= v_.size() || !v_[id].exists) return Code::NoVehicle;
            if (occ_[player].veh != id)
            {
                if (intent_[player].veh == id)
                {
                    DropIntent(player, proto::SeatCancelReason::ByPlayer);
                    return Code::Ok;
                }
                return Code::NotInVehicle;
            }
            const uint8_t seat = occ_[player].seat;
            Leave(player, nowMs, tick);
            if (announce) Notice(proto::SeatNoticeKind::Exited, player, id, seat, 0);
            return Code::Ok;
        }

        // T1 seats: `player` reserves `seat` of vehicle `id` (VehicleOp::EnterIntent). The checks are Enter's, plus: a player
        // who sits somewhere already is NotAllowed (an intent is a walk from outside), and a seat somebody else holds (seated or
        // reserved) is SeatTaken. Ok when the reservation stands (a repeat of the same one just renews it).
        Code Reserve(uint16_t player, uint16_t id, uint32_t seat, bool run, const proto::Kinematics& playerPos, uint64_t nowMs, uint32_t tick)
        {
            if (player >= occ_.size()) return Code::NotAllowed;
            if (id >= v_.size() || !v_[id].exists) return Code::NoVehicle;
            if (seat >= proto::kVehSeats) return Code::BadArg;
            Vehicle& V = v_[id];
            if (seat == 0 && V.owned && V.spawner != player) return Code::NotOwner;
            if (occ_[player].veh != kNone) return Code::NotAllowed;
            if (V.s.seats[seat] != proto::kNoPlayer) return Code::SeatTaken;
            const uint16_t held = ReservedBy(id, seat, nowMs);
            if (held != proto::kNoPlayer && held != player) return Code::SeatTaken;
            if (Dist2(playerPos, V.s.k) > rules_.enterDistance * rules_.enterDistance) return Code::TooFar;
            if ((V.s.flags & proto::kVehLocked) && !sync::MayEnterLocked(V.spawner == player, V.lastDriver == player)) return Code::Locked;
            NoteTick(tick);
            Intent& I = intent_[player];
            if (I.veh == id && I.seat == seat)
            {
                I.sinceMs = nowMs;            // the same walk, renewed: nobody needs telling again
                I.run = run;
                return Code::Ok;
            }
            if (I.veh != kNone) DropIntent(player, proto::SeatCancelReason::Replaced);
            I.veh = id;
            I.seat = (uint8_t)seat;
            I.run = run;
            I.sinceMs = nowMs;
            V.resBy[seat] = player;
            Notice(proto::SeatNoticeKind::Reserved, player, id, (uint8_t)seat, run ? proto::kSeatNoticeRun : 0);
            return Code::Ok;
        }

        // T1 seats: drops `player`'s reservation on `id` (VehicleOp::CancelIntent). Ok also when there is none.
        Code CancelIntent(uint16_t player, uint16_t id)
        {
            if (player >= occ_.size()) return Code::NotAllowed;
            if (id >= v_.size() || !v_[id].exists) return Code::NoVehicle;
            if (intent_[player].veh == id) DropIntent(player, proto::SeatCancelReason::ByPlayer);
            return Code::Ok;
        }

        // T1 seats: the player died, respawned or was put elsewhere: whatever it was walking to is over.
        void CancelIntentOf(uint16_t player, proto::SeatCancelReason why)
        {
            if (player < intent_.size() && intent_[player].veh != kNone) DropIntent(player, why);
        }

        // A player left the server: out of its seat. Its EMPTY vehicles are destroyed (ids to `destroyed`),
        // so a reconnect neither leaves cars behind nor starts over with fresh caps; the ones somebody else
        // still sits in lose their spawner and become orphans (counted in `orphaned`; they stay counted
        // against the address and go Rules::orphanMs after they empty).
        void PlayerGone(uint16_t player, uint64_t nowMs, uint32_t tick, std::vector<uint16_t>* destroyed = nullptr,
                        size_t* orphaned = nullptr)
        {
            if (player >= occ_.size()) return;
            if (intent_[player].veh != kNone) DropIntent(player, proto::SeatCancelReason::PlayerGone);
            if (occ_[player].veh != kNone) Leave(player, nowMs, tick);
            for (Vehicle& V : v_)
            {
                if (!V.exists) continue;
                if (V.lastDriver == player) V.lastDriver = proto::kNoPlayer;   // 0.4.22: a later player with this id must not inherit the key
                // v5: a vehicle the departed player controlled has no controller until AssignControllers finds one.
                if (V.s.ctrl == player) ClearController(V, tick);
                if (V.spawner != player) continue;
                if (Empty(V))
                {
                    const uint16_t id = V.s.id;
                    Destroy(id, nowMs, tick, nullptr);
                    if (destroyed) destroyed->push_back(id);
                }
                else
                {
                    V.spawner = proto::kNoPlayer;
                    V.orphan = true;
                    V.owned = false; // v5: an orphan belongs to nobody
                    V.s.ownerId = proto::kNoPlayer;
                    V.changedTick = tick;
                    if (orphaned) ++*orphaned;
                }
            }
            RemoveReceiver(player);
        }

        // ---- v5: the controlling client ----
        // Re-decides who controls each vehicle. Call once per tick before BuildFrame, with every joined player.
        // Seat 0 decides first (the driver controls). Otherwise a controller that is still a candidate with a
        // position, trusted and within Rules::ctrlStickyMeters keeps it, unless it was silent for ctrlSilenceMs
        // (then it is skipped for ctrlSkipMs); else the NEAREST candidate that has a position, is trusted, has the
        // vehicle in view (RelevantTo, from the last BuildFrame), is not skipped and is within ctrlNewMeters
        // takes over; else nobody does and the vehicle stands still (velocities zeroed).
        void AssignControllers(uint64_t nowMs, uint32_t tick, const std::vector<ControlCandidate>& players)
        {
            NoteTick(tick);
            for (Vehicle& V : v_)
            {
                if (!V.exists) continue;
                const uint16_t driver = V.s.seats[0];
                if (driver != proto::kNoPlayer)
                {
                    if (V.s.ctrl != driver) SetController(V, driver, nowMs, tick);
                    continue;
                }
                const float sticky2 = rules_.ctrlStickyMeters * rules_.ctrlStickyMeters;
                const float fresh2 = rules_.ctrlNewMeters * rules_.ctrlNewMeters;
                // T4: a driven vehicle about to hit this one takes its control, so the hit is resolved by ONE physics instance
                if (rules_.claimBaseMeters > 0.0f && (V.s.ctrl == proto::kNoPlayer || nowMs >= V.ctrlSinceMs + rules_.claimHoldMs))
                {
                    const uint16_t toucher = FindToucher(V, players);
                    if (toucher != proto::kNoPlayer && toucher != V.s.ctrl)
                    {
                        SetController(V, toucher, nowMs, tick);
                        ++claims_;
                        continue;
                    }
                }
                if (V.s.ctrl != proto::kNoPlayer)
                {
                    const ControlCandidate* c = FindCandidate(players, V.s.ctrl);
                    const uint64_t lastAct = std::max(V.ctrlSinceMs, V.ctrlUpdateMs);
                    if (nowMs >= lastAct && nowMs - lastAct >= rules_.ctrlSilenceMs)
                    {
                        AddSkip(V, V.s.ctrl, nowMs + rules_.ctrlSkipMs);
                        c = nullptr; // silent: look for somebody else
                    }
                    if (c && c->hasState && c->trusted && Dist2(c->pos, V.s.k) <= sticky2) continue; // keeps it
                }
                const ControlCandidate* best = nullptr;
                float bestD2 = 0;
                for (const ControlCandidate& c : players)
                {
                    if (!c.hasState || !c.trusted || c.id == V.s.ctrl || Skipped(V, c.id, nowMs) || !RelevantTo(c.id, V.s.id)) continue;
                    const float d2 = Dist2(c.pos, V.s.k);
                    if (d2 > fresh2) continue;
                    if (!best || d2 < bestD2 || (d2 == bestD2 && c.id < best->id))
                    {
                        best = &c;
                        bestD2 = d2;
                    }
                }
                if (best)
                    SetController(V, best->id, nowMs, tick);
                else if (V.s.ctrl != proto::kNoPlayer)
                    ClearController(V, tick);
            }
        }

        // T4 counters (the server's 10 s contact line)
        uint64_t Claims() const { return claims_; }
        uint64_t SpeedClamps() const { return speedClamps_; }

        // ---- the controller's motion ----
        // Ok when taken; NoVehicle, NotAllowed (not the vehicle's controller: v4 required seat 0), BadArg (invalid,
        // or a REPORTED velocity over the speed cap: the replicated velocity must be sane). The position is judged
        // by displacement: when it is farther from the previous one than a vehicle can get in the server time since
        // (see ac::AllowedMove) `*jumped` is set, and the update is taken anyway (a legitimate client-side reset
        // must not desync); the caller decides what that costs the controller. `nowMs` is the server's receive
        // time; `sampleMs` (default: nowMs) is when the sender took the sample on the server's clock (SampleClock),
        // clamped to nowMs: BuildFrame measures the entry's ageMs from it.
        static constexpr uint64_t kSampleAtReceipt = UINT64_MAX;
        Code Update(uint16_t player, const proto::VehicleUpdate& u, uint64_t nowMs, bool* jumped = nullptr,
                    uint64_t sampleMs = kSampleAtReceipt)
        {
            if (jumped) *jumped = false;
            if (u.vehId >= v_.size() || !v_[u.vehId].exists) return Code::NoVehicle;
            Vehicle& V = v_[u.vehId];
            if (V.s.ctrl != player) return Code::NotAllowed;
            if (!proto::ValidVehicleUpdate(u)) return Code::BadArg;
            const double speed2 = (double)u.k.vx * u.k.vx + (double)u.k.vy * u.k.vy + (double)u.k.vz * u.k.vz;
            const double cap = (double)rules_.maxSpeed;
            double velScale = 1.0;
            if (speed2 > cap * cap)
            {
                // T4: over the cap. A hard overshoot (a cheat or a broken read) is still refused; a small one is clamped, not dropped
                // (a dropped update froze the car for everybody else for as long as the overshoot lasted).
                const double soft = cap * (double)rules_.maxSpeedSoftFactor;
                if (!(rules_.maxSpeedSoftFactor > 1.0f) || speed2 > soft * soft) return Code::BadArg;
                velScale = cap / std::sqrt(speed2);
                ++speedClamps_;
            }
            const double dtSec = nowMs > V.recvMs ? (double)(nowMs - V.recvMs) / 1000.0 : 0.0;
            const float moved = ac::Dist(u.k, V.s.k);
            const bool jump = moved > ac::AllowedMove(rules_.maxSpeed, dtSec);
            if (jumped) *jumped = jump;
            V.recvMs = nowMs;
            V.ctrlUpdateMs = nowMs;
            V.posMs = std::min(sampleMs, nowMs);
            V.s.k.x = u.k.x;
            V.s.k.y = u.k.y;
            V.s.k.z = u.k.z;
            V.s.k.vx = (float)(u.k.vx * velScale);
            V.s.k.vy = (float)(u.k.vy * velScale);
            V.s.k.vz = (float)(u.k.vz * velScale);
            V.s.rot = u.rot;
            V.s.k.heading = proto::QuatHeading(u.rot);
            V.s.avx = u.avx;
            V.s.avy = u.avy;
            V.s.avz = u.avz;
            V.s.steer = u.steer;
            uint8_t flags = (uint8_t)((V.s.flags & ~proto::kVehDriverFlags) | (u.flags & proto::kVehDriverFlags));
            if (V.s.fuelPermille == 0) flags &= (uint8_t)~proto::kVehEngine; // no fuel, no engine
            V.s.flags = flags;
            // Health only goes down from the controller's side (a crash); never repaired by a client. What the
            // motion-only entry leaves out (health, lights, doors) and changes is sent in full for a few frames.
            const uint16_t body = std::min(V.s.bodyHealth, u.bodyHealth), engine = std::min(V.s.engineHealth, u.engineHealth);
            // R3 T4: a car whose body health is still (nearly) full cannot have gained a smashed window or a broken part since the last report.
            const bool looksClean = rules_.damageNeedsHealthLoss && std::min(body, u.bodyHealth) >= rules_.cleanHealth;
            uint8_t reportWindows = u.windows;
            if (looksClean && (reportWindows & ~V.s.windows))
            {
                reportWindows = (uint8_t)(reportWindows & V.s.windows);
                ++windowRejects_;
            }
            const bool visChanged = MergeVisual(V, u.tyres, u.tyresRim, reportWindows, nowMs); // v6
            const bool dmgChanged = u.hasDmg && MergeDamage(V, u.dmg, looksClean); // v7
            if (body != V.s.bodyHealth || engine != V.s.engineHealth || u.flags2 != V.s.flags2 || u.doors != V.s.doors || visChanged || dmgChanged)
                V.changedTick = lastTick_ + 1; // told in full from the next frame on
            V.s.bodyHealth = body;
            V.s.engineHealth = engine;
            V.s.flags2 = u.flags2; // v5: the controller's lights and doors
            V.s.doors = u.doors;
            // How fast it really moved: displacement over the server time since the last update (the gap
            // is bounded below so a burst of late packets does not read as a speed, and above so one lost
            // stretch is not billed as driving). A jump counts for nothing.
            if (!jump)
            {
                const double speed = moved / std::max(dtSec, 0.01);
                if (speed >= ac::kMovingSpeed)
                {
                    V.moved = true;
                    V.fastMs = nowMs;
                }
                if ((flags & proto::kVehEngine) && V.s.fuelPermille > 0 && speed >= 1.0) V.driveSec += std::min(dtSec, 0.25);
            }
            return Code::Ok;
        }
        // R3 T4 counters: window bits and damage-block rises refused because the car still looked clean (see Rules::damageNeedsHealthLoss).
        uint64_t windowRejects() const { return windowRejects_; }
        uint64_t damageRejects() const { return damageRejects_; }
        // The vehicle's measured movement: true when its displacement said it moved at least ac::kMovingSpeed
        // within the last `withinMs` of `nowMs`.
        bool MovedRecently(uint16_t id, uint64_t nowMs, uint64_t withinMs) const
        {
            const Vehicle* V = Get(id);
            // A `nowMs` before fastMs is a tick's nominal slot time asking about an update received after the slot
            // (the loop ran late): that is "just now".
            return V && V->moved && (nowMs < V->fastMs || nowMs - V->fastMs <= withinMs);
        }

        // ---- server-owned fields (the driver or the spawner) ----
        Code SetFlag(uint16_t player, uint16_t id, uint8_t flag, bool on, uint32_t tick)
        {
            Vehicle* V = Mutable(id);
            if (!V) return Code::NoVehicle;
            if (V->s.seats[0] != player && V->spawner != player) return Code::NotAllowed;
            const uint8_t f = on ? (uint8_t)(V->s.flags | flag) : (uint8_t)(V->s.flags & ~flag);
            if (f != V->s.flags) V->changedTick = tick;
            V->s.flags = f;
            return Code::Ok;
        }
        // 0.4.22: VehicleOp::SetLocked. sync::JudgeLockToggle is the rule (libs/sync/vehicle_lock.h): the player in seat 0, the spawner or the
        // last driver; a player OUTSIDE the vehicle must also be within Rules::lockDistance of it by `playerPos` (the last position the server has;
        // `havePos` false = it has none: NoState). Rules::locksEnabled false refuses everything with NotAllowed. Idempotent: locking a locked
        // vehicle is Ok and changes nothing (no fresh frames). `before` (optional) gets the flag as it was.
        Code SetLocked(uint16_t player, uint16_t id, bool on, const proto::Kinematics& playerPos, bool havePos, uint32_t tick, bool* before = nullptr)
        {
            Vehicle* V = Mutable(id);
            if (!V) return Code::NoVehicle;
            if (before) *before = (V->s.flags & proto::kVehLocked) != 0;
            if (!rules_.locksEnabled) return Code::NotAllowed;
            sync::LockActor a;
            a.inThisVehicle = player < occ_.size() && occ_[player].veh == id;
            a.driverSeat = V->s.seats[0] == player;
            a.spawner = V->spawner == player;
            a.lastDriver = V->lastDriver == player;
            if (!a.inThisVehicle)
            {
                if (!havePos) return Code::NoState;
                a.distM = std::sqrt(Dist2(playerPos, V->s.k));
            }
            const float reach = rules_.lockDistance > 0.0f ? rules_.lockDistance : 3.0e38f;
            switch (sync::JudgeLockToggle(a, reach))
            {
            case sync::LockVerdict::Ok: break;
            case sync::LockVerdict::NotAllowed: return Code::NotAllowed;
            case sync::LockVerdict::TooFar: return Code::TooFar;
            }
            const uint8_t f = on ? (uint8_t)(V->s.flags | proto::kVehLocked) : (uint8_t)(V->s.flags & ~proto::kVehLocked);
            if (f != V->s.flags) V->changedTick = tick;
            V->s.flags = f;
            return Code::Ok;
        }
        Code SetColours(uint16_t player, uint16_t id, uint8_t c1, uint8_t c2, uint32_t tick)
        {
            Vehicle* V = Mutable(id);
            if (!V) return Code::NoVehicle;
            if (V->s.seats[0] != player && V->spawner != player) return Code::NotAllowed;
            V->s.colour1 = c1;
            V->s.colour2 = c2;
            V->changedTick = tick;
            return Code::Ok;
        }
        // v6: a validated vehicle Hit that names a part (protocol.h VehiclePartCode): a tyre burst, a tyre off its rim or
        // a smashed window of a car whose shooter does not necessarily control it. The bit is set for everybody at once
        // (told in full for a few frames) and kept as unconfirmed until the controller shows it in a VehicleUpdate or
        // kVisAckMs pass with it not showing it (MergeVisual). Idempotent. True when the visual state changed.
        bool HitPart(uint16_t id, proto::VehiclePart part, unsigned slot, uint64_t nowMs, uint32_t tick)
        {
            Vehicle* V = Mutable(id);
            if (!V || slot >= proto::kVehWheelSlots) return false;
            const uint8_t b = (uint8_t)(1u << slot);
            const uint64_t stamp = nowMs + 1;
            bool changed = false;
            switch (part)
            {
            case proto::VehiclePart::Tyre:
                if (!(V->s.tyres & b))
                {
                    V->s.tyres |= b;
                    V->visPendMs[slot] = stamp;
                    changed = true;
                }
                break;
            case proto::VehiclePart::TyreRim:
                if (!(V->s.tyresRim & b))
                {
                    if (!(V->s.tyres & b)) V->visPendMs[slot] = stamp;
                    V->s.tyres |= b;
                    V->s.tyresRim |= b;
                    V->visPendMs[proto::kVehWheelSlots + slot] = stamp;
                    changed = true;
                }
                break;
            case proto::VehiclePart::Window:
                if (!(V->s.windows & b))
                {
                    V->s.windows |= b;
                    V->visPendMs[2 * proto::kVehWheelSlots + slot] = stamp;
                    changed = true;
                }
                break;
            default:
                break;
            }
            if (changed)
            {
                NoteTick(tick);
                V->changedTick = tick;
            }
            return changed;
        }

        // R3 T4: forget every damage-block field of V (a repair: the future admin / gamemode path). Body and engine health are the caller's.
        bool RepairDamage(uint16_t id, uint32_t tick)
        {
            Vehicle* V = Mutable(id);
            if (!V) return false;
            V->s.dmg = proto::VehicleDamage();
            V->changedTick = tick;
            NoteTick(tick);
            return true;
        }

        // A validated hit: `damage` off the body health, half of it off the engine health.
        void Damage(uint16_t id, uint16_t damage, uint32_t tick)
        {
            Vehicle* V = Mutable(id);
            if (!V) return;
            V->s.bodyHealth = (uint16_t)(V->s.bodyHealth > damage ? V->s.bodyHealth - damage : 0);
            const uint16_t half = (uint16_t)(damage / 2);
            V->s.engineHealth = (uint16_t)(V->s.engineHealth > half ? V->s.engineHealth - half : 0);
            V->changedTick = tick;
        }

        // ---- time ----
        // The empty-despawn and orphan timers, the fuel drain (from the seconds driven that Update measured by
        // displacement since the last Tick), and the end of old "removed" notices. Ids of vehicles destroyed by
        // a timer go to `despawned`.
        void Tick(uint64_t nowMs, uint32_t tick, std::vector<uint16_t>* despawned)
        {
            NoteTick(tick);
            for (uint16_t p = 0; p < intent_.size(); p++)   // T1: a walk that never ended
                if (intent_[p].veh != kNone && nowMs >= intent_[p].sinceMs && nowMs - intent_[p].sinceMs >= proto::kIntentTtlMs)
                    DropIntent(p, proto::SeatCancelReason::Expired);
            for (Vehicle& V : v_)
            {
                if (!V.exists) continue;
                if (Empty(V))
                {
                    // An orphan always has a timer, whatever emptyDespawnMs says (0 = never for ordinary ones).
                    const uint64_t limit = V.orphan ? (rules_.emptyDespawnMs ? std::min<uint64_t>(rules_.emptyDespawnMs, rules_.orphanMs)
                                                                             : (uint64_t)rules_.orphanMs)
                                                    : (uint64_t)rules_.emptyDespawnMs;
                    if (limit > 0 && nowMs - V.emptySinceMs >= limit)
                    {
                        const uint16_t id = V.s.id;
                        Destroy(id, nowMs, tick, nullptr);
                        if (despawned) despawned->push_back(id);
                        continue;
                    }
                }
                if (V.driveSec > 0)
                {
                    if (rules_.fuelDrainPerMinute > 0 && V.s.fuelPermille > 0 && (V.s.flags & proto::kVehEngine))
                    {
                        V.fuelAcc += (double)rules_.fuelDrainPerMinute / 60.0 * V.driveSec;
                        const uint16_t whole = (uint16_t)std::min<double>(V.fuelAcc, (double)V.s.fuelPermille);
                        if (whole > 0)
                        {
                            V.fuelAcc -= whole;
                            V.s.fuelPermille = (uint16_t)(V.s.fuelPermille - whole);
                            if (V.s.fuelPermille == 0)
                            {
                                V.s.flags &= (uint8_t)~proto::kVehEngine;
                                V.changedTick = tick;
                            }
                        }
                    }
                    V.driveSec = 0;
                }
            }
            removed_.erase(std::remove_if(removed_.begin(), removed_.end(),
                                          [&](const Removed& r) { return tick - r.tick >= kRemovedRepeat; }),
                           removed_.end());
        }

        // Builds receiver `r`'s VehicleWorld for `tick`, whose nominal time is `nowMs` (the entries' ageMs is
        // nowMs minus the time their motion was sampled). `hasPos` false: the receiver has sent no state
        // yet (only a vehicle it sits in is relevant). Returns false when no frame is due (nothing is
        // relevant, nothing was destroyed lately, and no keep-alive is due).
        bool BuildFrame(uint16_t r, bool hasPos, const proto::Kinematics& pos, uint32_t tick, uint64_t nowMs,
                        proto::VehicleWorld& out)
        {
            out = proto::VehicleWorld();
            out.tick = tick;
            NoteTick(tick);
            if (r >= rx_.size() || !rx_[r].active) return false;
            Rx& R = rx_[r];
            const uint16_t mine = occ_[r].veh;
            cands_.clear();
            const float enter2 = rules_.enterRadius * rules_.enterRadius, leave2 = rules_.leaveRadius * rules_.leaveRadius;
            for (uint16_t id = 0; id < proto::kMaxVehicles; id++)
            {
                const Vehicle& V = v_[id];
                RxVeh& RV = R.v[id];
                if (!V.exists)
                {
                    RV = RxVeh();
                    continue;
                }
                float d2 = 0;
                bool rel;
                if (id == mine) rel = true;
                else if (!hasPos) rel = false;
                else
                {
                    d2 = Dist2(pos, V.s.k);
                    rel = d2 <= (RV.relevant ? leave2 : enter2);
                }
                if (!rel)
                {
                    RV = RxVeh();
                    continue;
                }
                if (!RV.relevant)
                {
                    RV.relevant = true;
                    RV.enteredTick = tick;
                    RV.lastFull = 0;
                    RV.lastStatic = 0;
                }
                out.present.push_back(id);
                const bool fresh = tick - RV.enteredTick < kFreshFrames || tick - V.changedTick < kFreshFrames;
                // Moving (somebody drives it, or it was pushed lately) and not this receiver's own: at the distance rate.
                const bool driven = (V.s.seats[0] != proto::kNoPlayer || MovedRecently(id, nowMs, rules_.movedWithinMs)) && V.s.ctrl != r;
                const float d = std::sqrt(d2);
                const uint32_t every = !driven ? rules_.refreshTicks
                                       : d < rules_.nearDist ? rules_.nearEvery
                                       : d < rules_.midDist  ? rules_.midEvery
                                                             : rules_.farEvery;
                if (fresh || RV.lastFull == 0 || tick - RV.lastFull >= every)
                {
                    // The statics (model, health, fuel, colours, seats, ctrl, owner, flags2, doors) ride a newcomer, a
                    // change (fresh) and one entry in refreshTicks; in between the 26-byte motion-only entry will do.
                    const bool full = fresh || RV.lastStatic == 0 || tick - RV.lastStatic >= rules_.refreshTicks;
                    cands_.push_back(Cand{id, fresh ? 0 : 1, d2, full});
                }
            }
            // Destroyed ids owed to this receiver: each rides kRemovedRepeat frames, at most kMaxRemovedPerFrame
            // in one (the wire's u8 count). The least-sent go first, so a mass destruction is told to
            // everybody before any id is repeated; what does not fit stays owed for the next frame.
            R.owed.erase(std::remove_if(R.owed.begin(), R.owed.end(),
                                        [&](const Owed& o) { return std::binary_search(out.present.begin(), out.present.end(), o.id); }),
                         R.owed.end());
            std::stable_sort(R.owed.begin(), R.owed.end(), [](const Owed& a, const Owed& b) { return a.sent < b.sent; });
            const size_t nOwed = std::min<size_t>(R.owed.size(), kMaxRemovedPerFrame);
            for (size_t i = 0; i < nOwed; i++)
            {
                out.removed.push_back(R.owed[i].id);
                R.owed[i].sent++;
            }
            R.owed.erase(std::remove_if(R.owed.begin(), R.owed.end(), [](const Owed& o) { return o.sent >= kRemovedRepeat; }),
                         R.owed.end());
            std::sort(out.removed.begin(), out.removed.end());
            out.removed.erase(std::unique(out.removed.begin(), out.removed.end()), out.removed.end());

            const bool nonEmpty = !out.present.empty() || !out.removed.empty();
            if (nonEmpty) R.lastNonEmpty = tick;
            // An empty set is said in 3 frames, then about once a second while any vehicle exists (a
            // server without vehicles sends nothing).
            const bool keepAlive = (R.lastNonEmpty != 0 && tick - R.lastNonEmpty < kFreshFrames) ||
                                   (count_ > 0 && (tick + r) % rules_.refreshTicks == 0);
            if (!nonEmpty && !keepAlive) return false;

            // Full states by priority (changed / new first, then nearest) within the packet cap; the
            // presence list itself always goes out.
            size_t bytes = 1 + 4 + 1 + out.removed.size() + 2 + 2 * out.present.size();
            const size_t cap = std::max(rules_.packetMaxBytes, bytes);
            std::sort(cands_.begin(), cands_.end(), [](const Cand& a, const Cand& b) {
                if (a.prio != b.prio) return a.prio < b.prio;
                return a.d2 < b.d2;
            });
            for (const Cand& c : cands_)
            {
                const Vehicle& V = v_[c.id];
                const size_t sz = c.full ? FullBytes(V.s) : kMotionOnlyBytes;
                if (bytes + sz > cap) continue; // a smaller one may still fit; this one stays due
                bytes += sz;
                out.vehicles.push_back(V.s);
                proto::VehicleState& e = out.vehicles.back();
                e.motionOnly = !c.full;
                if (V.s.ctrl == proto::kNoPlayer) e.flags2 &= (uint8_t)~proto::kVeh2Brake; // nobody brakes a car nobody controls
                e.ageMs = (uint8_t)(nowMs > V.posMs ? std::min<uint64_t>(nowMs - V.posMs, 255) : 0);
                R.v[c.id].lastFull = tick;
                if (c.full) R.v[c.id].lastStatic = tick;
            }
            std::sort(out.vehicles.begin(), out.vehicles.end(),
                      [](const proto::VehicleState& a, const proto::VehicleState& b) { return a.id < b.id; });
            return true;
        }

        // Encoded size of an entry after its id and header byte: a full one (46 bytes in v6, which added the tyre, rim and window masks;
        // v7 adds the damage block, 3..11 bytes: 49 for a clean car; + 2 per occupant), or the motion-only one (24).
        static constexpr size_t kMotionOnlyBytes = 24;
        static size_t FullBytes(const proto::VehicleState& s)
        {
            size_t n = 46 + proto::VehicleDamageBytes(s.dmg);
            for (uint16_t p : s.seats)
                if (p != proto::kNoPlayer) n += 2;
            return n;
        }

    private:
        static constexpr uint32_t kFreshFrames = 3;   // a newcomer / change is sent in this many frames
        static constexpr uint64_t kVisAckMs = 3000;   // a Hit's visual bit waits this long for the controller to show it
        uint64_t windowRejects_ = 0, damageRejects_ = 0;   // R3 T4
        static constexpr uint32_t kRemovedRepeat = 3; // a "destroyed" notice rides this many frames
        static constexpr size_t kMaxRemovedPerFrame = 255; // the wire's u8 nRemoved

        // lastFull: the tick of the last entry of any kind sent to this receiver; lastStatic: of the last FULL one.
        struct RxVeh { bool relevant = false; uint32_t enteredTick = 0; uint32_t lastFull = 0; uint32_t lastStatic = 0; };
        struct Owed { uint16_t id; uint32_t sent; }; // a destroyed id and the frames it has ridden so far
        struct Rx { bool active = false; std::vector<RxVeh> v; uint32_t lastNonEmpty = 0; std::vector<Owed> owed; };
        struct Removed { uint16_t id; uint32_t tick; };
        struct Cand { uint16_t id; int prio; float d2; bool full; };

        void NoteTick(uint32_t tick)
        {
            if (tick > lastTick_) lastTick_ = tick;
        }
        // The driver of the nearest driven vehicle that is about to hit the driverless V (see Rules::claimBaseMeters), or kNoPlayer.
        // The driver must be a trusted candidate with a position and have V in view.
        uint16_t FindToucher(const Vehicle& V, const std::vector<ControlCandidate>& players) const
        {
            uint16_t best = proto::kNoPlayer;
            float bestD2 = 0;
            for (const Vehicle& W : v_)
            {
                if (!W.exists || W.s.id == V.s.id) continue;
                const uint16_t driver = W.s.seats[0];
                if (driver == proto::kNoPlayer || W.s.ctrl != driver) continue;
                const float vx = W.s.k.vx, vy = W.s.k.vy;
                const float speed = std::sqrt(vx * vx + vy * vy);
                if (speed < rules_.claimMinSpeed) continue;
                const float dx = V.s.k.x - W.s.k.x, dy = V.s.k.y - W.s.k.y, dz = V.s.k.z - W.s.k.z;
                const float d2 = dx * dx + dy * dy;
                const float reach = rules_.claimBaseMeters + rules_.claimLookSeconds * speed;
                if (d2 > reach * reach || std::fabs(dz) > 6.0f) continue;
                const float d = std::sqrt(d2);
                const float closing = d > 0.1f ? (vx * dx + vy * dy) / d : speed;   // speed of the driver toward V
                if (closing < 1.0f) continue;
                const ControlCandidate* c = FindCandidate(players, driver);
                if (!c || !c->hasState || !c->trusted || !RelevantTo(driver, V.s.id)) continue;
                if (best == proto::kNoPlayer || d2 < bestD2) { best = driver; bestD2 = d2; }
            }
            return best;
        }
        static const ControlCandidate* FindCandidate(const std::vector<ControlCandidate>& players, uint16_t id)
        {
            for (const ControlCandidate& c : players)
                if (c.id == id) return &c;
            return nullptr;
        }
        static bool Skipped(const Vehicle& V, uint16_t player, uint64_t nowMs)
        {
            for (const Vehicle::Skip& s : V.skips)
                if (s.player == player && nowMs < s.untilMs) return true;
            return false;
        }
        static void AddSkip(Vehicle& V, uint16_t player, uint64_t untilMs)
        {
            for (Vehicle::Skip& s : V.skips)
                if (s.player == player)
                {
                    s.untilMs = untilMs;
                    return;
                }
            V.skips[V.nextSkip] = Vehicle::Skip{player, untilMs};
            V.nextSkip = (uint8_t)((V.nextSkip + 1) % 4);
        }
        // `player` now controls V: its updates are accepted from here on and its displacement budget starts now.
        static void SetController(Vehicle& V, uint16_t player, uint64_t nowMs, uint32_t tick)
        {
            V.s.ctrl = player;
            V.ctrlSinceMs = nowMs;
            V.ctrlUpdateMs = 0;
            V.recvMs = nowMs;
            V.changedTick = tick;
        }
        // v6: the controller's visual report (tyre burst, off-rim and smashed-window masks) merged into V's truth.
        // A bit the controller shows is truth (and acks a Hit's pending bit); a bit it does not show is dropped, except
        // a Hit's bit that is still inside kVisAckMs (the controller has not had its chance to show it yet). So a
        // repair (the controller reports the bit clear) clears it, a pop by somebody else survives until the controller
        // shows it, and a controller that cannot show it (native missing, the pop rejected there) does not keep a
        // phantom for ever. With no controller nothing calls this: the truth stays what the last report and Hits said.
        // True when the merged state changed.
        static bool MergeVisual(Vehicle& V, uint8_t reportTyres, uint8_t reportRim, uint8_t reportWindows, uint64_t nowMs)
        {
            const uint8_t rep[3] = { (uint8_t)(reportTyres | reportRim), reportRim, reportWindows };
            uint8_t next[3];
            for (size_t g = 0; g < 3; g++)
            {
                uint8_t n = rep[g];
                for (size_t slot = 0; slot < proto::kVehWheelSlots; slot++)
                {
                    uint64_t& t = V.visPendMs[g * proto::kVehWheelSlots + slot];
                    if (!t) continue;
                    const uint8_t b = (uint8_t)(1u << slot);
                    if (rep[g] & b)
                        t = 0;                                  // the controller shows it: confirmed
                    else if (nowMs + 1 >= t && nowMs + 1 - t >= kVisAckMs)
                        t = 0;                                  // it never showed it: dropped
                    else
                        n |= b;                                 // not yet
                }
                next[g] = n;
            }
            next[0] |= next[1];
            const bool changed = next[0] != V.s.tyres || next[1] != V.s.tyresRim || next[2] != V.s.windows;
            V.s.tyres = next[0];
            V.s.tyresRim = next[1];
            V.s.windows = next[2];
            return changed;
        }
        // v7: the controller's damage block merged into V's truth. Damage only goes UP, like body and engine health (a crash cannot be
        // undone by a client; a repair path would call RepairDamage): the petrol tank takes the lower value, broken parts are OR-ed, a
        // dent takes the deeper value. A rise on a car that still looks clean is refused (and counted). True when V's block changed.
        bool MergeDamage(Vehicle& V, const proto::VehicleDamage& d, bool looksClean)
        {
            proto::VehicleDamage next = V.s.dmg;
            bool refused = false;
            if (d.petrol < next.petrol)
            {
                if (looksClean) refused = true;
                else next.petrol = d.petrol;
            }
            const uint8_t addParts = (uint8_t)(d.parts & ~next.parts);
            if (addParts)
            {
                if (looksClean) refused = true;
                else next.parts = (uint8_t)(next.parts | d.parts);
            }
            for (size_t i = 0; i < proto::kVehDentProbes; i++)
                if (d.dent[i] > next.dent[i])
                {
                    if (looksClean) refused = true;
                    else next.dent[i] = d.dent[i];
                }
            if (refused) ++damageRejects_;
            const bool changed = next != V.s.dmg;
            V.s.dmg = next;
            return changed;
        }
        // Nobody controls V: it stands still.
        static void ClearController(Vehicle& V, uint32_t tick)
        {
            V.s.ctrl = proto::kNoPlayer;
            V.s.k.vx = V.s.k.vy = V.s.k.vz = 0;
            V.s.avx = V.s.avy = V.s.avz = 0;
            V.s.steer = 0;
            V.s.flags &= (uint8_t)~proto::kVehHorn;
            V.changedTick = tick;
        }

        static float Dist2(const proto::Kinematics& a, const proto::Kinematics& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }
        static bool Empty(const Vehicle& V)
        {
            for (uint16_t p : V.s.seats)
                if (p != proto::kNoPlayer) return false;
            return true;
        }
        Vehicle* Mutable(uint16_t id) { return id < v_.size() && v_[id].exists ? &v_[id] : nullptr; }
        bool RecentlyRemoved(uint16_t id, uint32_t tick) const
        {
            for (const Removed& r : removed_)
                if (r.id == id && tick - r.tick < kRemovedRepeat) return true;
            return false;
        }
        // True while some receiver is still owed the "destroyed" notice of `id` (the id must not be reused yet).
        bool NoticeOwed(uint16_t id) const
        {
            for (const Rx& R : rx_)
                if (R.active)
                    for (const Owed& o : R.owed)
                        if (o.id == id) return true;
            return false;
        }
        // The oldest EMPTY adopted vehicle of `spawner` (when it is a player) or else of source address `ownerKey`.
        uint16_t OldestEmptyAdopted(uint16_t spawner, uint32_t ownerKey) const
        {
            uint16_t best = kNone;
            for (const Vehicle& V : v_)
            {
                if (!V.exists || !V.adopted || !Empty(V)) continue;
                if (spawner != proto::kNoPlayer ? V.spawner != spawner : V.ownerKey != ownerKey) continue;
                if (best == kNone || V.createdMs < v_[best].createdMs) best = V.s.id;
            }
            return best;
        }
        // T1 seats helpers. Notice queues one SeatNotice; DropIntent ends a reservation with Cancelled(why);
        // ConsumeIntent is Enter's: the reservation for exactly this seat becomes Seated, any other one is Cancelled(Replaced).
        void Notice(proto::SeatNoticeKind kind, uint16_t player, uint16_t veh, uint8_t seat, uint8_t detail)
        {
            proto::SeatNotice n;
            n.kind = kind;
            n.player = player;
            n.vehId = veh;
            n.seat = seat;
            n.detail = detail;
            notices_.push_back(n);
            std::vector<uint16_t> to;
            for (uint16_t p = 0; p < rx_.size(); p++)
                if (p == player || (rx_[p].active && veh < rx_[p].v.size() && rx_[p].v[veh].relevant)) to.push_back(p);
            audiences_.push_back(std::move(to));
        }
        void ReleaseIntent(uint16_t player)
        {
            Intent& I = intent_[player];
            if (I.veh < v_.size() && v_[I.veh].resBy[I.seat] == player) v_[I.veh].resBy[I.seat] = proto::kNoPlayer;
            I = Intent();
        }
        void DropIntent(uint16_t player, proto::SeatCancelReason why)
        {
            const Intent I = intent_[player];
            if (I.veh == kNone) return;
            ReleaseIntent(player);
            Notice(proto::SeatNoticeKind::Cancelled, player, I.veh, I.seat, (uint8_t)why);
        }
        void ConsumeIntent(uint16_t player, uint16_t id, uint8_t seat)
        {
            const Intent I = intent_[player];
            if (I.veh == kNone) return;
            if (I.veh == id && I.seat == seat)
            {
                ReleaseIntent(player);
                Notice(proto::SeatNoticeKind::Seated, player, id, seat, 0);
            }
            else
                DropIntent(player, proto::SeatCancelReason::Replaced);
        }

        // Out of its seat; a driver's vehicle parks.
        void Leave(uint16_t player, uint64_t nowMs, uint32_t tick)
        {
            const Seat cur = occ_[player];
            occ_[player] = Seat();
            if (cur.veh >= v_.size() || !v_[cur.veh].exists) return;
            Vehicle& V = v_[cur.veh];
            if (V.s.seats[cur.seat] == player) V.s.seats[cur.seat] = proto::kNoPlayer;
            if (cur.seat == 0)
            {
                V.s.flags &= (uint8_t)~proto::kVehHorn;
                if (V.s.ctrl == player)
                {
                    // v5: the driver stays the controller of the car it got out of (its client keeps simulating it:
                    // it rolls on, brakes, gets pushed), so the motion is kept; the silence watchdog starts now.
                    V.ctrlSinceMs = nowMs;
                    V.ctrlUpdateMs = 0;
                }
                else
                {
                    ClearController(V, tick);
                }
            }
            if (Empty(V)) V.emptySinceMs = nowMs;
            V.changedTick = tick;
        }

        Rules rules_;
        std::vector<Vehicle> v_;  // by id, kMaxVehicles slots
        std::vector<Seat> occ_;   // by player id
        std::vector<Intent> intent_;           // T1: by player id
        std::vector<proto::SeatNotice> notices_;   // T1: queued, oldest first
        std::vector<std::vector<uint16_t>> audiences_;   // T1: the players to tell, per queued notice
        std::vector<Rx> rx_;      // by player id
        std::vector<Removed> removed_;
        std::vector<Cand> cands_;
        uint16_t nextId_ = 0;
        size_t count_ = 0;
        uint64_t claims_ = 0, speedClamps_ = 0; // T4 counters
        uint32_t lastTick_ = 0; // the newest tick any call carried: a change made between ticks is told from the next one on
    };

    // Armour first, then health (protocol v4). Returns the damage applied (armour plus health taken);
    // `health` reaching 0 is death.
    inline uint16_t ApplyDamage(uint8_t& health, uint8_t& armor, uint16_t damage)
    {
        const uint16_t fromArmor = std::min<uint16_t>(armor, damage);
        armor = (uint8_t)(armor - fromArmor);
        const uint16_t fromHealth = std::min<uint16_t>(health, (uint16_t)(damage - fromArmor));
        health = (uint8_t)(health - fromHealth);
        return (uint16_t)(fromArmor + fromHealth);
    }

}
