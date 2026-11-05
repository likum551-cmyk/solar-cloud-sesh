// Vehicle body state on the wire (sync plan W6, protocol v5 VehicleState / VehicleUpdate `flags2` and `doors`): what
// a vehicle's CONTROLLING client reads off its car (headlights, high beam, interior light, indicators, roof, which
// doors stand open) and what every other client applies, only when it changes. Pure C++17, header-only, no game
// calls: the engine reads the natives and passes the values in, and calls the natives for the bits DiffBody names.
//
//   sender (the controller, every send):
//     doors  = DoorsFromRatios({GET_VEHICLE_DOOR_ANGLE_RATIO for door 0..5})   // 0 FL, 1 FR, 2 RL, 3 RR, 4 hood, 5 trunk
//     flags2 = Flags2FromState(lightsOn, highBeam, interior, indicatorL, indicatorR, RoofDownFromState(roofState))
//   receiver (per vehicle, per applied state):
//     const BodyDelta d = DiffBody(shown, flags2, doors);   // the bits that differ from what this copy shows
//     ... one native per set bit of d ...
//     shown = BodyShown{ flags2, doors, true };             // after the natives
//
// Applying only on change leaves the local game free to animate (a door swinging shut by itself, a siren's lights)
// and keeps the native count at zero for a quiet car; the FIRST sight of a vehicle applies every known bit once, so
// the copy starts in the controller's state.
//
// Headlights shown on a car = the server's override (VehicleCmd SetLights, kVehLights in `flags`) OR the
// controller's actual state (kVeh2Headlights): HeadlightsShown(). Indicators have no getter in the game's script
// API, so a controller always sends them 0 (INFERRED: a script-set indicator would need its own command); the
// receiver still applies the bits it is given.
#pragma once

#include "protocol/protocol.h"

#include <cmath>
#include <cstdint>

namespace gage::sync {

    constexpr int   kBodyDoors = 6;            // FL, FR, RL, RR, hood, trunk: GTA door ids 0..5 and kDoor* bits 0..5
    constexpr float kDoorOpenRatio = 0.1f;     // GET_VEHICLE_DOOR_ANGLE_RATIO above this = open (exactly 0.1 is shut)

    // Which doors stand open: ratio[i] > kDoorOpenRatio sets kDoor* bit i. NaN and infinities read as shut.
    inline uint8_t DoorsFromRatios(const float (&ratio)[kBodyDoors])
    {
        uint8_t d = 0;
        for (int i = 0; i < kBodyDoors; ++i)
            if (std::isfinite(ratio[i]) && ratio[i] > kDoorOpenRatio)
                d = (uint8_t)(d | (1u << i));
        return d;
    }

    // GET_CONVERTIBLE_ROOF_STATE: 0 up, 1 lowering, 2 down, 3 raising. The bit follows the TARGET: lowering and down
    // are "down", raising and up (and anything else, a car with no roof) are "up".
    inline bool RoofDownFromState(int state) { return state == 1 || state == 2; }

    // brakeLights / handbrake (v6, kVeh2Brake / kVeh2Handbrake): the driver's pedals as sync::ClassifyBrake reads them
    // (libs/sync/vehicle_visual.h); a car nobody drives sends both clear.
    inline uint8_t Flags2FromState(bool headlights, bool highBeam, bool interior, bool indicatorL, bool indicatorR, bool roofDown,
                                   bool brakeLights = false, bool handbrake = false)
    {
        uint8_t f = 0;
        if (headlights) f = (uint8_t)(f | proto::kVeh2Headlights);
        if (highBeam)   f = (uint8_t)(f | proto::kVeh2HighBeam);
        if (interior)   f = (uint8_t)(f | proto::kVeh2Interior);
        if (indicatorL) f = (uint8_t)(f | proto::kVeh2IndicatorL);
        if (indicatorR) f = (uint8_t)(f | proto::kVeh2IndicatorR);
        if (roofDown)   f = (uint8_t)(f | proto::kVeh2RoofDown);
        if (brakeLights) f = (uint8_t)(f | proto::kVeh2Brake);
        if (handbrake)  f = (uint8_t)(f | proto::kVeh2Handbrake);
        return f;
    }

    // Whether this copy's headlights are lit: the server's override or the controller's own state.
    inline bool HeadlightsShown(bool serverOverride, bool controllerActual) { return serverOverride || controllerActual; }

    // 0.4.30: whether GET_VEHICLE_LIGHTS_STATE(car, &lightsOn, &highbeamsOn) says the lamps are lit. The game reports a high beam as
    // lightsOn 0 / highbeamsOn 1 (0.4.29 live log: 0 sends "with the headlights", 44-435 "with the high beam"; FiveM HUDs read the pair
    // the same way), so either bit = lit. highIsLit false (veh_lights_beam_lit=0) = 0.4.29: lightsOn alone. The kVeh2Headlights bit
    // sent, the HUD lamp and the copies' keeper all use this rule.
    inline bool LightsLit(bool lightsOn, bool highbeamsOn, bool highIsLit = true) { return lightsOn || (highIsLit && highbeamsOn); }

    // What a local copy of a vehicle shows of the above; `valid` is false until something was applied.
    struct BodyShown
    {
        uint8_t flags2 = 0;
        uint8_t doors = 0;
        bool    valid = false;
    };

    // The bits to apply (set or clear) to bring a copy from `shown` to the wire state.
    struct BodyDelta
    {
        uint8_t flags2 = 0;
        uint8_t doors = 0;
        bool Any() const { return flags2 != 0 || doors != 0; }
    };

    inline BodyDelta DiffBody(const BodyShown& shown, uint8_t flags2, uint8_t doors)
    {
        BodyDelta d;
        if (!shown.valid)
        {
            d.flags2 = proto::kVeh2Known;
            d.doors = proto::kDoorsKnown;
            return d;
        }
        d.flags2 = (uint8_t)((shown.flags2 ^ flags2) & proto::kVeh2Known);
        d.doors = (uint8_t)((shown.doors ^ doors) & proto::kDoorsKnown);
        return d;
    }

}
