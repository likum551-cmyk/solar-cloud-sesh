// 0.4.25 minimap_scale: GTA's radar smaller, by writing the game's own minimap component table (the FiveM / RAGE:MP route; the rules, the evidence and the
// pattern are in libs/sync/minimap_size.h). Legacy GTA5.exe 1.0.3889 only: Enhanced is a different binary whose frontend.xml reader fills a local
// struct instead of a global table (its dump has no hit for the pattern), so it is refused with a logged reason and the radar stays as the game draws it.
//
// All of it runs on the script tick (engine.cpp TickMinimap). Nothing here calls a game function: it scans the image once for the pattern, checks the
// game's frontend.xml reader against the instructions the layout was read from, reads the table, and writes the 16 geometry bytes of the three entries
// (minimap, minimap_mask, minimap_blur). Every write is read back; a mismatch puts the old bytes back and counts as a failure (three and it gives up).
#pragma once

#include "sync/minimap_size.h"

#include <string>

namespace gage::game::minimapsize {

    // scale: the parsed registry value (0.5..1; 1 = untouched). sourceName: how it was read, for the log only. Safe to call again with a new value.
    void Configure(float scale, const char* sourceName);

    // 0.5.2 ring_map: an absolute placement (sync::RingPlacement: the radar's visible map as a square inside the REV1 ring) instead of the factor.
    // Configure(float) switches back. Safe to call every tick: the same placement is not written again.
    void ConfigurePlacement(const sync::MinimapPlacement& p, const char* sourceName);

    // 0.5.4 ring_map_big: true = once the placement is in the minimap slots, the big radar's slots (bigmap / bigmap_mask / bigmap_blur) get the
    // same geometry (sync::MirroredTrio), so a radar in big mode lands in the ring too. Call before the first Tick; false = slots 0-2 only (0.5.3).
    void SetBigMirror(bool on);

    // True while the table holds the placement (written, read back, not taken back by the game); with SetBigMirror(true) also the big radar's
    // mirror, unless its slots gave up (then the minimap slots alone).
    bool PlacementApplied();

    // Locate (once), apply when the table is populated, re-assert when the game rewrites it. Call every script tick; it paces itself.
    void Tick(double nowMs);

    // The factor the game's table holds now: the configured one while the last write was read back and the table is still ours, 1.0 otherwise.
    // This, not the configured value, is what the overlay's radar rectangle uses.
    float AppliedScale();

    // True once after each write that changed the table (first apply, a re-assert, a restore to the stock sizes): the radar's movie must flip to re-read.
    bool TakeWritten();

    // One phrase for the engine's 10 s minimap line.
    std::string Status();

} // namespace gage::game::minimapsize
