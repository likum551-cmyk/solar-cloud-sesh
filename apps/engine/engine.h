// GAGE-MP in-game engine: per-frame multiplayer driver.
//
// Installed once (Start) it owns the whole "playable in the world" behavior:
//   - keeps the GAGE session alive (connects in the background, reconnects),
//   - runs the game's own main loop via a per-frame detour (hook.h), so all
//     game calls run on the game's main/render thread,
//   - sends the local player's position to the server (throttled),
//   - spawns / moves / despawns remote players as in-world peds,
//   - pipes chat both directions through the mp.* bridge.
//
// Everything degrades: if a game primitive is unresolved the frame simply does
// less, never crashes, and the game stays a normal story-mode world.
#pragma once
#include "hook.h"
#include "join.h"

namespace gage::engine {

    // Wire the engine up and install the tick hook. Call once after
    // game::Init(world). The session is connected in the background so the
    // game's own load is never blocked. `world` must outlive the engine.
    void Start(game::World* world, const join::Config& cfg);

    // Stop the frame driver, drop remote peds, and close the session.
    void Stop();

    // Send a local chat line to the server (called by the mp.* bridge on
    // "chatMessage" / "command").
    void SendChat(const char* text);

}
