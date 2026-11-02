// The local player's link to a server vehicle (protocol v4 VehicleCmd / VehicleResult): which
// command to send when the player gets into, changes seat in or leaves a vehicle, and what to do
// with the server's answer. Pure C++17, single-threaded (the engine's script tick), tested by
// tests/gameplay_test.cpp. The engine reads the natives (which vehicle, which seat, which server id
// that handle is bound to) and sends what Tick returns.
//
//   On foot -> in vehicle X:  X bound to a server id -> Enter(id, seat); X local only (an ambient car,
//                             one from /car) -> Register(seat) (adopt it; the engine fills the tail).
//   Seat change in X:         Enter(id, new seat) (the server skips the distance and lock checks).
//   Leaving X:                Exit(id), also when the player goes straight into another car (/car,
//                             SET_PED_INTO_VEHICLE: no frame on foot between): Exit(old id) goes out on
//                             one Tick and the new car's command on the next. Left while the answer is
//                             still out: the command becomes an "orphan" and its Ok, when it comes, is
//                             answered with an Exit (a Register's Ok also binds that car to the new id).
//   Answers:                  matched on op AND vehicle id (a Register has no id yet: on op alone, the
//                             oldest orphan first, the server answers in order). Ok -> linked (seat 0 =
//                             the server's driver: send VehicleUpdate). An Ok that matches nothing we
//                             are in or wait for means the server seated us in a car we are not in: Exit.
//                             RateLimited / NoState -> retried after retryMs, at most maxTries.
//                             An Enter refused for the seat itself (taken, locked, too far, not
//                             allowed, dead, no such vehicle) on the FIRST entry -> eject: the server
//                             says we are not in it, so the engine takes the player out. The same
//                             refusal of a SEAT CHANGE inside a car we are linked in (GTA shuffled us
//                             to a seat, or we moved) is a revert: the link stays Linked in the old
//                             seat, out.revertSeat names it and the engine puts the ped back there
//                             (no animation, no eject: the server never moved us). Seat observations
//                             that differ from the linked seat are ignored for revertHoldMs after a
//                             revert, so a warp that has not happened yet is not asked again.
//                             Any other refusal -> stay unlinked (the car stays local; others see the
//                             placement fallback).
//                             No answer within pendingTimeoutMs -> retried like RateLimited (a late
//                             answer is still taken).
//   One answer is applied per Tick and one command goes out per Tick.
// Ops and codes are the wire values (proto::VehicleOp, proto::VehicleResultCode).
#pragma once

#include <cstdint>

namespace gage::sync {

    enum class VehLinkCmd : uint8_t { None, Enter, Register, Exit };

    struct VehLinkConfig
    {
        double retryMs = 1500.0;
        double pendingTimeoutMs = 5000.0;
        int    maxTries = 3;
        double revertHoldMs = 600.0;   // after a revert: how long a differing seat reading is not acted on
    };

    struct VehLinkIn
    {
        double  nowMs = 0;
        int32_t veh = 0;     // the vehicle the local player sits in (0 = on foot)
        int     seat = -1;   // its wire seat 0..7 (0 driver); -1 unknown / not representable
        int     netId = -1;  // the server id `veh` is bound to; -1 none
        bool    dead = false;
    };

    struct VehLinkOut
    {
        VehLinkCmd cmd = VehLinkCmd::None;
        int        netId = -1;  // Enter / Exit target
        int        seat = 0;    // Enter / Register seat
        int32_t    veh = 0;     // the local vehicle the command is about
        bool       eject = false;
        int        revertSeat = -1; // >= 0: a seat change was refused, put the ped back in this wire seat (no eject)
        int32_t    bindVeh = 0; // a Register's Ok: local vehicle bindVeh is server vehicle bindNetId
        int        bindNetId = -1;
        // The answer this Tick applied to the car we are in (for the log): op 0 = none.
        uint8_t    resultOp = 0, resultCode = 0;
    };

    struct VehLinkStats { uint32_t enters = 0, registers = 0, exits = 0, oks = 0, refused = 0, retries = 0, timeouts = 0, ejects = 0, stale = 0, reverts = 0; };

    // A server vehicle's local entity was spawned for a model; the id was reused for another model
    // (destroyed and created again): the entity is replaced unless it is our own adopted car or we
    // sit in it.
    inline bool NetVehicleModelStale(uint32_t stateModel, uint32_t spawnedModel, bool adopted, bool occupiedByUs)
    {
        return !adopted && !occupiedByUs && stateModel != 0 && spawnedModel != 0 && stateModel != spawnedModel;
    }

    class LocalVehicleLink
    {
    public:
        explicit LocalVehicleLink(const VehLinkConfig& cfg = VehLinkConfig());
        // Queued; applied by the next Tick.
        void OnResult(uint8_t op, uint8_t code, uint16_t vehId);
        VehLinkOut Tick(const VehLinkIn& in);
        // The engine could not send the command Tick just returned (a Register whose position or
        // rotation did not read): no answer is coming, so the link goes back to retrying.
        void OnNotSent(double nowMs);
        void Reset();   // a new session: nothing is linked

        bool Linked() const { return state_ == State::Linked; }
        bool Driver() const { return state_ == State::Linked && seat_ == 0; }
        int NetId() const { return state_ == State::Linked ? netId_ : -1; }
        int Seat() const { return seat_; }
        int32_t Vehicle() const { return veh_; }
        const char* StateName() const;
        const VehLinkStats& Stats() const { return stats_; }

        // The chat line for an Enter that was refused for the seat itself (Ejects(code)); "" for any other code.
        static const char* RefusalText(uint8_t code);
        static bool Retryable(uint8_t code) { return code == 7 /* RateLimited */ || code == 11 /* NoState */; }
        static bool Ejects(uint8_t code)
        {
            return code == 1 /* NoVehicle */ || code == 2 /* SeatTaken */ || code == 3 /* TooFar */ ||
                   code == 4 /* Locked */ || code == 5 /* NotAllowed */ || code == 9 /* Dead */ ||
                   code == 12 /* NotOwner (protocol v5): the driver seat of somebody else's car */;
        }

    private:
        enum class State : uint8_t { OnFoot, Pending, Linked, Unlinked };
        struct Res { uint8_t op, code; uint16_t vehId; };
        struct Orph { uint8_t op; int netId; int32_t veh; double sinceMs; };   // a command whose car we left unanswered
        struct Owed { int netId; int32_t veh; };                               // an Exit still to send
        void Start(const VehLinkIn& in, VehLinkOut& out);
        void Apply(const Res& r, const VehLinkIn& in, VehLinkOut& out);
        void LeaveCurrent(double nowMs);
        void Owe(int netId, int32_t veh);
        void Orphan(uint8_t op, int netId, int32_t veh, double nowMs);
        int FindOrphan(uint8_t op, uint16_t vehId) const;

        VehLinkConfig cfg_;
        State state_ = State::OnFoot;
        int32_t veh_ = 0;
        int netId_ = -1, seat_ = 0, tries_ = 0;
        int prevSeat_ = -1;         // the seat we were linked in when the current seat change began (-1 = a first entry)
        double revertUntilMs_ = 0;  // a differing seat reading is ignored until then (after a revert)
        uint8_t pendingOp_ = 0;
        double pendingSinceMs_ = 0, retryAtMs_ = 0;
        bool noRetry_ = false;
        bool awaiting_ = false;     // the newest command for veh_ has no answer yet
        Res res_[8] = {};
        int nres_ = 0;
        Orph orph_[4] = {};
        int norph_ = 0;
        Owed owed_[4] = {};
        int nowed_ = 0;
        VehLinkStats stats_;
    };

}
