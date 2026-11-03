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

}   // namespace gage::sync
