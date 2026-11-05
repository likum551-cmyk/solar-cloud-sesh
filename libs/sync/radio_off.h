// 0.4.19 radio off (GI, live test: "when sitting in a vehicle, any sort, the radio starts playing: completely rip that
// out. And when holding Q and selecting radio, the single-player slow-mo turns on: eradicate that as well.").
// Pure C++17, single-threaded (the engine's script tick), tested by tests/radio_off_tests.h (gameplay_test).
//
// What the engine issues (natives: apps/engine/hook.h "0.4.19 radio off"), all while no_radio is on (unset / 1):
//   * the globals, once at the first tick and again every globalEveryMs and on every new vehicle:
//     SET_USER_RADIO_CONTROL_ENABLED(false) (no retune, no station choice), SET_MOBILE_RADIO_ENABLED_DURING_GAMEPLAY(false)
//     and SET_AUDIO_FLAG("DisableFlightMusic", true) (the score an aircraft starts in single player);
//   * the vehicle the local player is in, or is entering (GET_VEHICLE_PED_IS_USING; the game starts the radio with the
//     engine while we climb in), driver or passenger, any class: SET_VEH_RADIO_STATION(veh, "OFF"),
//     SET_VEHICLE_RADIO_ENABLED(veh, false) and SET_RADIO_TO_STATION_NAME("OFF") at once when that vehicle changes,
//     again as soon as a read-back says the radio is on (no sooner than readbackGapMs after the last apply), and every
//     reapplyMs while we stay in it;
//   * every vehicle the engine creates (the server's copies, /car): SET_VEH_RADIO_STATION(veh, "OFF") +
//     SET_VEHICLE_RADIO_ENABLED(veh, false) right after CREATE_VEHICLE, so a remote driver's car plays nothing either;
//   * every script tick in or entering a vehicle: DISABLE_CONTROL_ACTION(0, c, true) for the five radio controls
//     below (a per-frame effect). The radio wheel (hold Q / d-pad left) is what slows time in single player: the wheel
//     never opens when its control is disabled, so the slow-mo never starts. Nothing on foot is touched (Q there is
//     cover, a different control), nor the weapon wheel.
// no_radio=0 = the old behaviour: nothing above is issued.
#pragma once

#include <cstdint>

namespace gage::sync {

    // eControlAction ids (GameplayFixesV inc/globals.h, counted from INPUT_NEXT_CAMERA = 0; INPUT_VEH_SELECT_NEXT_WEAPON
    // lands on its known 99 the same way).
    constexpr int kControlVehNextRadio = 81;        // INPUT_VEH_NEXT_RADIO
    constexpr int kControlVehPrevRadio = 82;        // INPUT_VEH_PREV_RADIO
    constexpr int kControlVehNextRadioTrack = 83;   // INPUT_VEH_NEXT_RADIO_TRACK
    constexpr int kControlVehPrevRadioTrack = 84;   // INPUT_VEH_PREV_RADIO_TRACK
    constexpr int kControlVehRadioWheel = 85;       // INPUT_VEH_RADIO_WHEEL (hold Q / d-pad left)
    constexpr int kRadioControls[] = { kControlVehNextRadio, kControlVehPrevRadio, kControlVehNextRadioTrack,
                                       kControlVehPrevRadioTrack, kControlVehRadioWheel };
    constexpr int kRadioControlCount = (int)(sizeof(kRadioControls) / sizeof(kRadioControls[0]));
    constexpr int kHudRadioStations = 16;           // HUD_RADIO_STATIONS (IS_HUD_COMPONENT_ACTIVE id): the radio wheel
    constexpr int kRadioStationOff = 255;           // GET_PLAYER_RADIO_STATION_INDEX: "radio off" (also its failure value)

    // The vehicle the radio rules apply to: the one we sit in, else the one we are getting into.
    inline int32_t RadioTarget(int32_t vehIn, int32_t vehUsing) { return vehIn ? vehIn : vehUsing; }

    // The radio reads on: the vehicle's own state, or (seated) the player's station is a real one.
    inline bool RadioReadsOn(bool vehRadioOn, bool seated, int stationIndex)
    {
        return vehRadioOn || (seated && stationIndex >= 0 && stationIndex != kRadioStationOff);
    }

    // Ground truth for the slow-mo: the game timer (GET_GAME_TIMER, game time) advanced far less than the wall clock
    // over one script tick. Frames outside 5..100 ms of wall time (a hitch, a load, an alt-tab) and an unreadable timer
    // (negative delta) say nothing.
    constexpr double kSlowFrameRatio = 0.6;
    inline bool IsSlowFrame(double gameDeltaMs, double realDeltaMs)
    {
        if (realDeltaMs < 5.0 || realDeltaMs > 100.0 || gameDeltaMs < 0.0)
            return false;
        return gameDeltaMs < kSlowFrameRatio * realDeltaMs;
    }

    struct RadioOffConfig
    {
        double reapplyMs = 2000.0;       // re-assert on the vehicle we stay in
        double readbackGapMs = 250.0;    // a read-back "on" re-applies, but no more often than this
        double globalEveryMs = 10000.0;  // re-assert the global switches
    };

    enum class RadioApplyWhy : uint8_t { None, NewVehicle, ReadBackOn, Periodic };

    struct RadioOffIn
    {
        double  nowMs = 0;
        bool    enabled = true;      // no_radio (unset / 1 = on)
        int32_t vehIn = 0;           // GET_VEHICLE_PED_IS_IN(local ped, false)
        int32_t vehUsing = 0;        // GET_VEHICLE_PED_IS_USING(local ped)
        bool    radioReadsOn = false;// RadioReadsOn(...) on the current target, read this tick before anything is applied
    };

    struct RadioOffOut
    {
        bool          applyGlobal = false;   // the three global switches now
        int32_t       applyVehicle = 0;      // the per-vehicle "off" on this handle now (0 = none)
        bool          holdControls = false;  // DISABLE_CONTROL_ACTION on kRadioControls this tick
        RadioApplyWhy why = RadioApplyWhy::None;
    };

    class RadioOffGate
    {
    public:
        explicit RadioOffGate(RadioOffConfig cfg = RadioOffConfig()) : cfg_(cfg) {}

        RadioOffOut Tick(const RadioOffIn& in)
        {
            RadioOffOut out;
            if (!in.enabled)
            {
                *this = RadioOffGate(cfg_);   // switched off: forget everything, so switching back on starts fresh
                return out;
            }
            if (!globalDone_ || in.nowMs - globalAtMs_ >= cfg_.globalEveryMs)
                out.applyGlobal = true;
            const int32_t target = RadioTarget(in.vehIn, in.vehUsing);
            if (!target)
            {
                veh_ = 0;                     // on foot: the next vehicle, even the same one again, is new
            }
            else
            {
                out.holdControls = true;
                if (target != veh_)
                {
                    out.why = RadioApplyWhy::NewVehicle;
                    out.applyGlobal = true;   // re-assert the user radio control on every entry too
                }
                else if (in.radioReadsOn && in.nowMs - lastApplyMs_ >= cfg_.readbackGapMs)
                    out.why = RadioApplyWhy::ReadBackOn;
                else if (in.nowMs - lastApplyMs_ >= cfg_.reapplyMs)
                    out.why = RadioApplyWhy::Periodic;
                if (out.why != RadioApplyWhy::None)
                {
                    out.applyVehicle = target;
                    veh_ = target;
                    lastApplyMs_ = in.nowMs;
                }
            }
            if (out.applyGlobal)
            {
                globalDone_ = true;
                globalAtMs_ = in.nowMs;
            }
            return out;
        }

        int32_t Vehicle() const { return veh_; }

    private:
        RadioOffConfig cfg_;
        bool    globalDone_ = false;
        double  globalAtMs_ = 0;
        int32_t veh_ = 0;
        double  lastApplyMs_ = 0;
    };

    inline const char* RadioApplyWhyName(RadioApplyWhy w)
    {
        switch (w)
        {
        case RadioApplyWhy::None: return "none";
        case RadioApplyWhy::NewVehicle: return "new vehicle";
        case RadioApplyWhy::ReadBackOn: return "read-back on";
        case RadioApplyWhy::Periodic: return "periodic";
        }
        return "?";
    }

}  // namespace gage::sync
