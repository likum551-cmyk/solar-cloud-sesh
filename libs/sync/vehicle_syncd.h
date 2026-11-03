// Sync D: remote VEHICLE motion and OCCUPANT decisions found wanting by the offline audit (tests/vehsim.h drives the real
// controllers through a simulated relay and a simplified physical car; tests/vehicle_drive_test.cpp section 11 and
// tests/gameplay_test.cpp TestSyncD hold the proofs). Pure C++17, header-only, no game calls, single-threaded.
// engine.cpp only wires them.
//
//   DecideHandover      a vehicle's controlling client changed: what the OBSERVER does with its snapshot buffer and its drive
//                       controller. Until Sync D both were reset on EVERY hand-over (interp.Clear + drive.Reset): the car was
//                       shown at the new controller's first snapshot, a whole render delay ahead of where it was drawn, and
//                       snapped there (2.5 m at 25 m/s, 14 m at 139 m/s in the simulation).
//   EnterWalkMode       how a puppet shows the sender's walk to a car door (kMoveEnteringVehicle). The sender's F / G entry
//                       is a RUN to the door (TASK_GO_STRAIGHT_TO_COORD at blend 2.0) and then a warp into the seat: no door
//                       animation ever happens on the real player. The W6 receiver started its own TASK_ENTER_VEHICLE walk
//                       (blend 1.0, its own path, a door animation) and, when the seat list named the player, warped the
//                       puppet from wherever that walk had got to: a visible teleport of up to half the door distance.
//                       Follow: the puppet keeps following the sender's pose (the ordinary locomotion controller) and the
//                       seat warp, due the moment the seat list names the player, moves it a few tens of cm.
//   LateralErrorM       the sideways part of a car's position error, for the 10 s drive-error line.
//   DriveErrWindow      the 10 s drive-error statistics of one remote car (position, heading, lateral, snaps, modes).
//   SeatDisplacement    where a puppet that should sit in seat S of car V is found instead (shuffled to another seat,
//                       in another car, outside): the diagnosis the W9 re-warp counters cannot give.
#pragma once

#include "sync/vehicle_drive.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gage::sync {

    // ---- controller hand-over (observer side) ----

    struct HandoverAction
    {
        bool clearInterp = false;   // empty the snapshot buffer (the old controller's samples do not continue)
        bool resetDrive = false;    // the next drive frame snaps onto its sample (a new source of motion)
    };

    // oldCtrl / newCtrl: VehicleState.ctrl before and after (0xFFFF = nobody); me: our player id.
    // We became the controller, or stopped being it: the buffer holds nothing of the new source (we send, nobody pushes)
    // or only stale samples, and our own physics drove the copy: clear and snap, as before. Between two OTHER controllers
    // (the driver left a moving car, another player is nearest; the new driver sat in a moving car): it is the same car on
    // the same road, only a different machine reports it. The buffer stays (Push's own rules clear it across a hole over
    // maxBridgeMs or a jump over the teleport limit), the controller's own snap rules (distance, persistent error) judge
    // the first new samples, and the car's correction pulls it over a small step in a third of a second instead of a
    // pop. The new controller's samples can be up to a render delay behind the old controller's (its copy was drawn that
    // far in the past): at 25 m/s a step of about 2.7 m, absorbed by the correction; the simulation shows the old
    // behaviour popping the car 2.5 m forward, snapping, and braking it there.
    inline HandoverAction DecideHandover(uint16_t oldCtrl, uint16_t newCtrl, uint16_t me)
    {
        HandoverAction a;
        if (oldCtrl == newCtrl)
            return a;
        if (oldCtrl == me || newCtrl == me)
        {
            a.clearInterp = true;
            a.resetDrive = true;
        }
        return a;
    }

    // ---- the puppet's door walk ----

    enum class EnterWalkMode : uint8_t { Task, Follow };

    // Whether the puppet gets its own TASK_ENTER_VEHICLE when the sender's move turns kMoveEnteringVehicle.
    inline bool PuppetUsesEnterTask(EnterWalkMode mode) { return mode == EnterWalkMode::Task; }

    // ---- diagnostics ----

    // The sideways error (m, absolute) of `local` against `target`, perpendicular to the target's horizontal heading.
    // 0 for a target pointing straight up or down (no horizon) or non-finite input.
    inline double LateralErrorM(const DriveVec& targetPos, const DriveQuat& targetRot, const DriveVec& localPos)
    {
        const DriveVec f = vq::Rotate(targetRot, DriveVec{0.0f, 1.0f, 0.0f});
        const double fl = std::sqrt((double)f.x * f.x + (double)f.y * f.y);
        if (!(fl > 1e-6))
            return 0.0;
        const double dx = (double)localPos.x - targetPos.x, dy = (double)localPos.y - targetPos.y;
        const double lat = (dx * f.y - dy * f.x) / fl;
        return std::isfinite(lat) ? std::fabs(lat) : 0.0;
    }

    // One remote car's drive error over a window (the 10 s line). Add() once per drive frame with what the controller
    // reported (VehicleDriveOutput.posErrorM / rotErrorDeg: the error the frame STARTED with, before its commands).
    class DriveErrWindow
    {
    public:
        struct Window
        {
            uint32_t frames = 0, buffering = 0, interpolating = 0, extrapolating = 0, holding = 0, snaps = 0;
            float posMeanM = 0, posMaxM = 0, rotMeanDeg = 0, rotMaxDeg = 0, latMeanM = 0, latMaxM = 0;
        };
        void Add(double posErrM, double rotErrDeg, double latErrM, VehMode mode, bool snap)
        {
            if (!std::isfinite(posErrM) || !std::isfinite(rotErrDeg) || !std::isfinite(latErrM))
                return;
            ++w_.frames;
            posSum_ += posErrM;
            rotSum_ += rotErrDeg;
            latSum_ += latErrM;
            posMax_ = std::max(posMax_, posErrM);
            rotMax_ = std::max(rotMax_, rotErrDeg);
            latMax_ = std::max(latMax_, latErrM);
            if (snap) ++w_.snaps;
            switch (mode)
            {
            case VehMode::Buffering: ++w_.buffering; break;
            case VehMode::Interpolating: ++w_.interpolating; break;
            case VehMode::Extrapolating: ++w_.extrapolating; break;
            case VehMode::Holding: ++w_.holding; break;
            }
        }
        Window Take()
        {
            Window w = w_;
            if (w.frames)
            {
                w.posMeanM = (float)(posSum_ / w.frames);
                w.rotMeanDeg = (float)(rotSum_ / w.frames);
                w.latMeanM = (float)(latSum_ / w.frames);
            }
            w.posMaxM = (float)posMax_;
            w.rotMaxDeg = (float)rotMax_;
            w.latMaxM = (float)latMax_;
            *this = DriveErrWindow();
            return w;
        }

    private:
        Window w_;
        double posSum_ = 0, rotSum_ = 0, latSum_ = 0, posMax_ = 0, rotMax_ = 0, latMax_ = 0;
    };

    // Where a puppet that should sit in (vehicle, seat) is found when the engine's "already there" check fails.
    enum class SeatDisplacement : uint8_t
    {
        NotSeatedBefore,   // it was never warped there (the first warp, or a fresh ped): not a displacement
        OtherSeat,         // inside the same car, in another seat (GTA's auto-shuffle)
        OtherVehicle,      // inside another vehicle
        Outside,           // not in any vehicle (ragdolled out, a task pulled it out, the car was replaced)
        Unknown            // the natives could not say
    };

    // wasWarpedHere: the engine's record says the ped was warped into this very seat. inSameVehicle / inAnyVehicle: the
    // game's answers now (GET_VEHICLE_PED_IS_IN == that car; IS_PED_IN_ANY_VEHICLE). readable: both natives answered.
    inline SeatDisplacement ClassifyDisplacement(bool wasWarpedHere, bool readable, bool inSameVehicle, bool inAnyVehicle)
    {
        if (!wasWarpedHere)
            return SeatDisplacement::NotSeatedBefore;
        if (!readable)
            return SeatDisplacement::Unknown;
        if (inSameVehicle)
            return SeatDisplacement::OtherSeat;
        if (inAnyVehicle)
            return SeatDisplacement::OtherVehicle;
        return SeatDisplacement::Outside;
    }

}   // namespace gage::sync
