// The local player's vehicle link (see vehicle_link.h).

#include "vehicle_link.h"

namespace gage::sync {

namespace {
    constexpr uint8_t kOpEnter = 3, kOpExit = 4, kOpRegister = 8;   // proto::VehicleOp
}

LocalVehicleLink::LocalVehicleLink(const VehLinkConfig& cfg) : cfg_(cfg) {}

const char* LocalVehicleLink::RefusalText(uint8_t code)
{
    switch (code)
    {
    case 1:  return "That vehicle no longer exists.";                 // NoVehicle
    case 2:  return "That seat is taken.";                            // SeatTaken
    case 3:  return "You are too far from that vehicle.";             // TooFar
    case 4:  return "That vehicle is locked.";                        // Locked
    case 5:  return "You are not allowed to use that seat.";          // NotAllowed
    case 9:  return "You cannot do that while dead.";                 // Dead
    case 12: return "That vehicle belongs to someone else.";          // NotOwner (protocol v5)
    }
    return "";
}

void LocalVehicleLink::Reset()
{
    state_ = State::OnFoot;
    veh_ = 0;
    netId_ = -1;
    seat_ = 0;
    tries_ = 0;
    prevSeat_ = -1;
    revertUntilMs_ = 0;
    pendingOp_ = 0;
    noRetry_ = false;
    awaiting_ = false;
    nres_ = 0;
    norph_ = 0;
    nowed_ = 0;
}

void LocalVehicleLink::OnResult(uint8_t op, uint8_t code, uint16_t vehId)
{
    if (nres_ < (int)(sizeof(res_) / sizeof(res_[0])))
        res_[nres_++] = { op, code, vehId };
}

void LocalVehicleLink::OnNotSent(double nowMs)
{
    if (state_ != State::Pending || !awaiting_)
        return;
    awaiting_ = false;                          // nothing went out: no answer will come
    state_ = State::Unlinked;
    noRetry_ = false;
    retryAtMs_ = nowMs + cfg_.retryMs;          // tries_ stays counted: a read that keeps failing gives up
}

const char* LocalVehicleLink::StateName() const
{
    switch (state_)
    {
    case State::OnFoot:   return "on foot";
    case State::Pending:  return "waiting for the server";
    case State::Linked:   return "linked";
    case State::Unlinked: return "unlinked";
    }
    return "?";
}

void LocalVehicleLink::Owe(int netId, int32_t veh)
{
    if (netId < 0)
        return;
    for (int i = 0; i < nowed_; ++i)
        if (owed_[i].netId == netId)
            return;                             // already owed
    if (nowed_ < (int)(sizeof(owed_) / sizeof(owed_[0])))
        owed_[nowed_++] = { netId, veh };
}

void LocalVehicleLink::Orphan(uint8_t op, int netId, int32_t veh, double nowMs)
{
    if (norph_ == (int)(sizeof(orph_) / sizeof(orph_[0])))
    {
        for (int i = 1; i < norph_; ++i)        // the oldest is the least likely to still be answered
            orph_[i - 1] = orph_[i];
        --norph_;
    }
    orph_[norph_++] = { op, netId, veh, nowMs };
}

int LocalVehicleLink::FindOrphan(uint8_t op, uint16_t vehId) const
{
    for (int i = 0; i < norph_; ++i)        // oldest first: the server answers in the order it was asked
        if (orph_[i].op == op && (op == kOpRegister || orph_[i].netId == (int)vehId))
            return i;
    return -1;
}

// The vehicle we sat in is no longer ours: a command still unanswered becomes an orphan (its Ok will
// be answered with an Exit), a linked seat is owed an Exit.
void LocalVehicleLink::LeaveCurrent(double nowMs)
{
    if (awaiting_)
        Orphan(pendingOp_, netId_, veh_, nowMs);
    else if (state_ == State::Linked && netId_ >= 0)
        Owe(netId_, veh_);
    awaiting_ = false;
    state_ = State::OnFoot;
    veh_ = 0;
    netId_ = -1;
    tries_ = 0;
    prevSeat_ = -1;                             // a seat change in the car we left is moot
    revertUntilMs_ = 0;
    noRetry_ = false;
}

void LocalVehicleLink::Start(const VehLinkIn& in, VehLinkOut& out)
{
    if (in.seat < 0 || in.seat > 7)
    {
        state_ = State::Unlinked;              // a seat the wire cannot carry: stays local
        noRetry_ = true;
        return;
    }
    if (in.dead)
    {
        state_ = State::Unlinked;              // the server refuses the dead: try again later
        noRetry_ = false;
        retryAtMs_ = in.nowMs + cfg_.retryMs;
        return;
    }
    seat_ = in.seat;
    if (in.netId >= 0)
    {
        netId_ = in.netId;
        out.cmd = VehLinkCmd::Enter;
        out.netId = netId_;
        pendingOp_ = kOpEnter;
        ++stats_.enters;
    }
    else
    {
        netId_ = -1;
        out.cmd = VehLinkCmd::Register;
        pendingOp_ = kOpRegister;
        ++stats_.registers;
    }
    out.seat = seat_;
    out.veh = veh_;
    state_ = State::Pending;
    awaiting_ = true;
    pendingSinceMs_ = in.nowMs;
    ++tries_;
}

// One answer (the oldest queued; one per Tick keeps every Tick's bind and Exit unambiguous).
void LocalVehicleLink::Apply(const Res& r, const VehLinkIn& in, VehLinkOut& out)
{
    if (r.op != kOpEnter && r.op != kOpRegister)
    {
        ++stats_.stale;                         // an Exit's answer: nothing to do about it
        return;
    }
    // 1. An older command whose car we left before the answer: if the server seated us, take us out.
    const int oi = FindOrphan(r.op, r.vehId);
    if (oi >= 0)
    {
        const Orph o = orph_[oi];
        for (int i = oi + 1; i < norph_; ++i)
            orph_[i - 1] = orph_[i];
        --norph_;
        ++stats_.stale;
        if (r.code == 0)
        {
            if (r.op == kOpRegister)
            {
                out.bindVeh = o.veh;            // that car now IS server vehicle r.vehId
                out.bindNetId = r.vehId;
            }
            Owe(r.vehId, o.veh);
        }
        return;
    }
    // 2. The answer to the command in flight: same op AND the same vehicle (a Register has no id yet).
    const bool inFlight = (state_ == State::Pending || (state_ == State::Unlinked && awaiting_)) && veh_ != 0;
    if (inFlight && r.op == pendingOp_ && (r.op == kOpRegister || (int)r.vehId == netId_))
    {
        awaiting_ = false;
        out.resultOp = r.op;
        out.resultCode = r.code;
        if (r.code == 0)
        {
            ++stats_.oks;
            if (r.op == kOpRegister)
            {
                netId_ = r.vehId;
                out.bindVeh = veh_;
                out.bindNetId = r.vehId;
            }
            state_ = State::Linked;
            tries_ = 0;
            prevSeat_ = -1;                     // a granted seat change is done
        }
        else
        {
            ++stats_.refused;
            if (r.op == kOpEnter && Ejects(r.code) && prevSeat_ >= 0)
            {
                // A seat change inside a car we are linked in, refused for the seat itself: the server
                // still has us where we were. Stay linked there and tell the engine to put the ped back
                // (nothing is ejected: it never left the car, and a leave animation would only show the
                // other players a passenger thrown out).
                ++stats_.reverts;
                out.revertSeat = prevSeat_;
                seat_ = prevSeat_;
                prevSeat_ = -1;
                state_ = State::Linked;
                tries_ = 0;
                noRetry_ = false;
                revertUntilMs_ = in.nowMs + cfg_.revertHoldMs;
                return;
            }
            if (r.op == kOpEnter && Ejects(r.code))
            {
                out.eject = true;
                ++stats_.ejects;
                noRetry_ = true;
            }
            else
                noRetry_ = !Retryable(r.code);
            state_ = State::Unlinked;
            retryAtMs_ = in.nowMs + cfg_.retryMs;
        }
        return;
    }
    // 3. Anything else is stale. An Ok for a vehicle we are not linked to means the server has us
    // seated in it: say we left.
    ++stats_.stale;
    if (r.code == 0 && !(state_ == State::Linked && netId_ == (int)r.vehId))
        Owe(r.vehId, 0);
}

VehLinkOut LocalVehicleLink::Tick(const VehLinkIn& in)
{
    VehLinkOut out;

    for (int i = 0; i < norph_;)                // an orphan nobody answered in time: forget it
    {
        if (in.nowMs - orph_[i].sinceMs >= cfg_.pendingTimeoutMs)
        {
            for (int j = i + 1; j < norph_; ++j)
                orph_[j - 1] = orph_[j];
            --norph_;
        }
        else
            ++i;
    }

    // The server's answers first: they decide what the observation below means.
    if (nres_ > 0)
    {
        const Res r = res_[0];
        for (int i = 1; i < nres_; ++i)
            res_[i - 1] = res_[i];
        --nres_;
        Apply(r, in, out);
    }

    // The owed Exit goes out on the first Tick that has no command of its own.
    auto flushOwed = [&]() {
        if (out.cmd == VehLinkCmd::None && nowed_ > 0)
        {
            out.cmd = VehLinkCmd::Exit;
            out.netId = owed_[0].netId;
            out.veh = owed_[0].veh;
            for (int i = 1; i < nowed_; ++i)
                owed_[i - 1] = owed_[i];
            --nowed_;
            ++stats_.exits;
        }
    };

    if (in.veh == 0)
    {
        if (veh_ != 0)
            LeaveCurrent(in.nowMs);
        flushOwed();
        return out;
    }

    if (veh_ != 0 && in.veh != veh_)
        LeaveCurrent(in.nowMs);                 // another car with no frame on foot between (SET_PED_INTO_VEHICLE)
    if (state_ == State::OnFoot)
    {
        if (nowed_ > 0)
        {
            flushOwed();                        // the old seat is given up before the new one is asked for
            return out;
        }
        veh_ = in.veh;                          // a new vehicle
        tries_ = 0;
        prevSeat_ = -1;                         // a first entry: a refusal ejects
        noRetry_ = false;
        Start(in, out);
        return out;
    }

    if (state_ == State::Pending && in.nowMs - pendingSinceMs_ >= cfg_.pendingTimeoutMs)
    {
        ++stats_.timeouts;
        state_ = State::Unlinked;               // awaiting_ stays: a late answer is still taken
        noRetry_ = false;
        retryAtMs_ = in.nowMs;
    }
    if (state_ == State::Unlinked && !noRetry_ && in.nowMs >= retryAtMs_ && tries_ < cfg_.maxTries)
    {
        ++stats_.retries;
        Start(in, out);
        return out;
    }
    if (state_ == State::Linked && in.seat >= 0 && in.seat <= 7 && in.seat != seat_ && netId_ >= 0 &&
        out.revertSeat < 0 && in.nowMs >= revertUntilMs_)
    {
        prevSeat_ = seat_;                      // kept: a refusal of this Enter puts us back here
        seat_ = in.seat;                        // a seat change inside the same vehicle
        out.cmd = VehLinkCmd::Enter;
        out.netId = netId_;
        out.seat = seat_;
        out.veh = veh_;
        pendingOp_ = kOpEnter;
        state_ = State::Pending;
        awaiting_ = true;
        pendingSinceMs_ = in.nowMs;
        tries_ = 1;
        ++stats_.enters;
    }
    flushOwed();
    return out;
}

}
