#pragma once
// Hyper tune: the Solar RP "hyper" cars (GI 2026-11-02: the Jesko with giga acceleration and a 500 km/h top
// speed). Pure numbers, no game calls: the engine applies them to the vehicle the LOCAL player drives (the owner's
// client is the authority for its car; everyone else sees the result through the normal vehicle sync).
//
//   SET_VEHICLE_CHEAT_POWER_INCREASE(veh, kHyperPower)   every frame (the game resets it each frame)
//   MODIFY_VEHICLE_TOP_SPEED(veh, HyperTopSpeedPercent)  once per vehicle, from GET_VEHICLE_ESTIMATED_MAX_SPEED
//   SET_VEHICLE_MAX_SPEED(veh, kHyperTargetMs)           once per vehicle: the cap the game enforces
//
// The server's anti-cheat limits a VehicleUpdate to vehicleMaxSpeed (default 150 m/s = 540 km/h), which is above the
// target here. T4: an overshoot above the cap (a downhill, a jump, the game's enforcement lagging a frame) is no longer
// DROPPED, which froze the car for every other client for as long as it lasted: up to twice the cap the server clamps the
// replicated velocity to the cap and takes the position (VehicleStore::Rules::maxSpeedSoftFactor); above that it refuses.

#include <cstdint>

namespace gage::sync {

    constexpr float kHyperTargetKmh = 500.0f;
    constexpr float kHyperTargetMs = kHyperTargetKmh / 3.6f;   // 138.9 m/s
    constexpr float kHyperPower = 4.0f;                         // cheat power multiplier (1 = stock)
    constexpr float kHyperMaxPercent = 400.0f;                  // never more than 5x the stock top speed

    // MODIFY_VEHICLE_TOP_SPEED takes a percentage added to the handling's top speed (100 = double). From the game's
    // own estimate of the stock top speed (m/s), the percentage that brings it to `targetMs`. An unknown or broken
    // estimate (<= 1 m/s) gets the safe middle value 100; a car already at or above the target gets 0.
    inline float HyperTopSpeedPercent(float estimatedMaxMs, float targetMs = kHyperTargetMs)
    {
        if (!(estimatedMaxMs > 1.0f)) return 100.0f;
        if (estimatedMaxMs >= targetMs) return 0.0f;
        const float pct = (targetMs / estimatedMaxMs - 1.0f) * 100.0f;
        return pct > kHyperMaxPercent ? kHyperMaxPercent : pct;
    }

} // namespace gage::sync
