// Sync B (collision audit): the 10 s diagnostics of one remote car's contacts, so the next live log says what the
// collision path really does. Pure C++17, header-only, no game calls, single-threaded (the engine's script tick);
// engine.cpp feeds one ContactFrame per driven frame and prints Take() every 10 s.
//
// The questions the numbers answer (docs/sync-specs/vehicles.md, Sync B section):
//   * Does HAS_ENTITY_COLLIDED_WITH_ANYTHING behave as the grace assumes? flagTrue / flagReads is its duty cycle: ~0 on a
//     car that drives on the ground (the assumption), ~1 (it never falls: only ONE rising edge, then no grace by flag ever
//     again), or a flicker (graces every 1.2 s on bumpy ground). flagTrueHard counts the frames the flag was true while the
//     car was overwritten at full weight (no grace in force): the frames a contact was NOT softened.
//   * Is the flag late? preEdgeDevMax is the deviation (|velocity commanded last frame - the car's own|) in the frame BEFORE a
//     rising edge, edgeDevMax in the frame OF the edge. A late flag shows the impulse in the frame before it (pre large,
//     edge small); a same-frame flag shows it in the edge frame.
//   * Does the car honour SET_ENTITY_VELOCITY at all? devMax / devOver1 / devOver3 over ALL frames: ~0.1 m/s when it does;
//     a car that lags the command shows it in every frame (and the kick trigger mutes itself, kickMutes).
//   * Which trigger opens the graces: flag, speed loss, kick?
//   * Do cars interpenetrate? minLocalM is the closest the car's origin came to the local player's vehicle in the window
//     (two ~4.5 x 2 m cars cannot have origins closer than ~2 m without overlapping), localUnder3m / localUnder1_5m count the
//     frames below 3 m / 1.5 m; minPuppetM and puppetUnder3m the same against the nearest OTHER remote car.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gage::sync {

    struct ContactFrame
    {
        bool flagRead = false;       // HAS_ENTITY_COLLIDED_WITH_ANYTHING was asked this frame (a car within 60 m)
        bool flag = false;           // its value
        bool edge = false;           // the rising edge fed to the controller (VehicleDriveInput.contact)
        float devMs = 0.0f;          // VehicleDriveOutput.devMs
        bool graceActive = false;    // VehicleDriveOutput.graceActive
        uint8_t opened = 0;          // VehicleDriveOutput.contactOpened: 0 none, 1 flag, 2 speed loss, 3 kick
        bool overwrote = false;      // the velocity command was written this frame
        float localCarDistM = -1.0f; // origin to the local player's vehicle (negative: unknown / not applicable)
    };

    class ContactWindow
    {
    public:
        struct Window
        {
            uint32_t frames = 0, flagReads = 0, flagTrue = 0, edges = 0, flagTrueHard = 0;
            uint32_t opened[4] = {};                 // [1] by flag, [2] by speed loss, [3] by kick
            uint32_t graceFrames = 0, hardFrames = 0;   // frames a grace was in force / the command was written at full weight
            float devMax = 0.0f, edgeDevMax = 0.0f, preEdgeDevMax = 0.0f;
            uint32_t devOver1 = 0, devOver3 = 0;
            float minLocalM = -1.0f, minPuppetM = -1.0f;   // negative: never measured in the window
            uint32_t localUnder3m = 0, localUnder1_5m = 0, puppetUnder3m = 0;
            // Interesting: a line is worth printing for this car (something touched it, or came close, or a grace opened)
            bool Interesting() const
            {
                return flagTrue || opened[1] || opened[2] || opened[3] || localUnder3m || puppetUnder3m || devOver3;
            }
        };

        void Add(const ContactFrame& f)
        {
            if (!std::isfinite(f.devMs))
                return;
            ++w_.frames;
            if (f.flagRead)
            {
                ++w_.flagReads;
                if (f.flag)
                {
                    ++w_.flagTrue;
                    if (!f.graceActive && f.overwrote)
                        ++w_.flagTrueHard;
                }
            }
            if (f.edge)
            {
                ++w_.edges;
                w_.edgeDevMax = std::max(w_.edgeDevMax, f.devMs);
                w_.preEdgeDevMax = std::max(w_.preEdgeDevMax, prevDev_);
            }
            if (f.opened >= 1 && f.opened <= 3)
                ++w_.opened[f.opened];
            if (f.graceActive)
                ++w_.graceFrames;
            else if (f.overwrote)
                ++w_.hardFrames;
            w_.devMax = std::max(w_.devMax, f.devMs);
            if (f.devMs > 1.0f)
                ++w_.devOver1;
            if (f.devMs > 3.0f)
                ++w_.devOver3;
            prevDev_ = f.devMs;
            if (std::isfinite(f.localCarDistM) && f.localCarDistM >= 0.0f)
            {
                w_.minLocalM = w_.minLocalM < 0.0f ? f.localCarDistM : std::min(w_.minLocalM, f.localCarDistM);
                if (f.localCarDistM < 3.0f)
                    ++w_.localUnder3m;
                if (f.localCarDistM < 1.5f)
                    ++w_.localUnder1_5m;
            }
        }

        // Another remote car's origin this close this frame (the engine's pair pass; only distances under 3 m are reported).
        void AddPuppetNear(float distM)
        {
            if (!std::isfinite(distM) || distM < 0.0f)
                return;
            w_.minPuppetM = w_.minPuppetM < 0.0f ? distM : std::min(w_.minPuppetM, distM);
            if (distM < 3.0f)
                ++w_.puppetUnder3m;
        }

        Window Take()
        {
            const Window w = w_;
            w_ = Window();
            return w;
        }

    private:
        Window w_;
        float prevDev_ = 0.0f;
    };


    // R4 T2 (docs/sync-specs/vehicle-collision-authority.md, section 1): WHAT a copy's collision flag touched. HAS_ENTITY_COLLIDED_WITH_ANYTHING says yes or no; no native in hook.h names
    // the other entity, so the classification is by the evidence we can read: a vehicle we control near the copy, another copy near it, its height above the ground (GET_ENTITY_HEIGHT_ABOVE_GROUND:
    // a car on its wheels reads 0.3-0.9 m at its origin), and how much speed it lost against what the controller commanded (a solid thing stops a car; a kerb, a bump or the road do not).
    // This is a heuristic labelled as such in the log; its job is to say, in numbers, how many of GI's 4-8 flag edges per window were NOT a car.
    enum class TouchKind : uint8_t { OwnCar = 0, OtherCopy, Airborne, Solid, Ground, kCount };
    struct TouchCfg
    {
        double ownNearM = 10.0;       // origin to origin: a vehicle we control this close is the likely toucher (contactNearM of the drive gate)
        double copyNearM = 7.0;       // another copy this close
        double airborneM = 1.6;       // origin height above the ground above this: in the air / on a prop
        double solidLossMs = 3.0;     // lost this much speed against the last command: something solid stopped it
    };
    struct TouchEvidence
    {
        double ownDistM = -1.0;       // nearest vehicle we control (negative: none / unknown)
        double copyDistM = -1.0;      // nearest other remote copy
        double heightM = -1.0;        // GET_ENTITY_HEIGHT_ABOVE_GROUND of the copy (negative: unknown)
        double speedLossMs = 0.0;     // VehicleDriveOutput.devMs: |last command - the car's own velocity|
    };
    inline TouchKind ClassifyTouch(const TouchEvidence& e, const TouchCfg& c = TouchCfg())
    {
        if (e.ownDistM >= 0.0 && e.ownDistM <= c.ownNearM) return TouchKind::OwnCar;
        if (e.copyDistM >= 0.0 && e.copyDistM <= c.copyNearM) return TouchKind::OtherCopy;
        if (e.heightM > c.airborneM) return TouchKind::Airborne;
        if (e.speedLossMs >= c.solidLossMs) return TouchKind::Solid;
        return TouchKind::Ground;
    }
    inline const char* TouchName(TouchKind k)
    {
        static const char* const n[] = {"a car we control", "another copy", "in the air / on a prop", "something solid (speed lost)", "the ground / a kerb (no speed lost)"};
        const int i = static_cast<int>(k);
        return n[i < 0 || i >= static_cast<int>(TouchKind::kCount) ? 4 : i];
    }
    // The 10 s tally of the flag's rising edges by what they touched, with the gate's verdict.
    class TouchTally
    {
    public:
        struct Window
        {
            uint32_t byKind[static_cast<int>(TouchKind::kCount)] = {};
            uint32_t gated = 0;                  // edges that opened no grace because no vehicle we control was near
            double heightMax = 0, lossMax = 0;   // over the edges of the window
        };
        void Add(TouchKind k, bool gated, double heightM, double lossMs)
        {
            ++w_.byKind[static_cast<int>(k)];
            if (gated) ++w_.gated;
            if (std::isfinite(heightM) && heightM > w_.heightMax) w_.heightMax = heightM;
            if (std::isfinite(lossMs) && lossMs > w_.lossMax) w_.lossMax = lossMs;
        }
        Window Take() { const Window w = w_; w_ = Window(); return w; }
    private:
        Window w_;
    };

}   // namespace gage::sync
