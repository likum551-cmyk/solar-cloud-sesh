// The knobs of the world replication layer (interest management, send rates, bandwidth budget).
// A plain value type shared by Config (server.json keys) and Replicator. Portable C++17.
// docs/api/server.md lists every key with its range; Validate() (config.cpp) enforces them.
#pragma once

#include <cstdint>

namespace gage::server {

    struct ReplConfig
    {
        // ---- filled in by Server::Start from the rest of the Config ----
        uint16_t maxPlayers = 32; // ids are 0..maxPlayers-1
        uint16_t tickHz = 30;     // world ticks per second (Server::Start overwrites it with Config::tickHz)

        // ---- interest management (server.json: relevance*) ----
        // A player becomes relevant to a receiver inside `enterRadius` metres and stops being
        // relevant beyond `leaveRadius` (the gap is the hysteresis: a player hovering at the
        // edge does not flicker in and out). Both grow with speed, so a fast approach is
        // noticed early: enter = min(maxRadius, enterRadius + lookaheadMs/1000 * (speedA +
        // speedB)), leave = enter + (leaveRadius - enterRadius).
        uint32_t enterRadius = 400;
        uint32_t leaveRadius = 450;
        uint32_t maxRadius = 600;     // cap of the speed-aware enter radius
        uint32_t lookaheadMs = 1000;  // how far ahead (at the pair's combined speed) the radius reaches
        uint32_t cellSize = 128;      // spatial grid cell edge, metres (tuned: about a third of enterRadius)
        uint32_t relevanceEveryMs = 100; // how often each receiver looks for newly relevant players

        // ---- send rates (server.json: rate*Hz), clamped to tickHz ----
        // A relevant player is sent at rateNearHz inside nearDist metres, rateMidHz inside
        // midDist, rateFarHz beyond; the distance is reduced by half a second of the pair's
        // combined speed, so fast-closing pairs get the higher rate. A player that has not
        // moved for idleAfterMs is sent at most at rateIdleHz. Players outside relevance range
        // reach the receiver at rateCoarseHz on the coarse channel.
        uint32_t nearDist = 120;
        uint32_t midDist = 250;
        // Sync plan W7: 30 / 15 / 10 (it was 20 / 10 / 5) with the 30 Hz tick, so a near player is sent every tick,
        // a mid one every 2nd and a far one every 3rd; the vehicle tiers (Server::Start) follow tickHz the same way.
        uint32_t rateNearHz = 30;
        uint32_t rateMidHz = 15;
        uint32_t rateFarHz = 10;
        uint32_t rateIdleHz = 1;
        uint32_t rateCoarseHz = 1;
        uint32_t idleAfterMs = 500;

        // ---- bandwidth (server.json: clientBudgetBytesPerSec, worldPacketMaxBytes) ----
        // Payload bytes per second the server may send one client in WorldState + WorldCoarse
        // (a token bucket refilled every tick). Due updates beyond it wait and are served by
        // accumulated priority, so every player's turn comes (fair scheduling). A world packet
        // never exceeds worldPacketMaxBytes (below the ENet MTU, so it is never fragmented).
        // 36000 (was 24000, sync plan W7): the 30 Hz rates cost 1.5x, and a crowd in which every delta falls back to an
        // absolute entry (a lost baseline) needs ~27 kB/s, so the budget must not be the limit there.
        uint32_t budgetBytesPerSec = 36000;
        uint32_t packetMaxBytes = 1200;
        uint32_t coarseSharePercent = 20; // of the per-tick budget, reserved for the coarse channel
    };

}
