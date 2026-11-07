// world_clock: 0.5.7, the server's time of day and weather on this machine (GI 2026-11-07: "each player has their own night / day").
//
// The server sends proto::WorldClock (55) on join, on every admin change, on every weather phase change of its cycle and every 30 s; the join
// worker hands it to OnClock with the session's round trip; the script tick (Tick) runs the received line forward on the steady clock and holds
// the game on it through the pure holds of libs/sync/world_clock.h:
//   * clock: PAUSE_CLOCK(true) (the game's own clock never runs between our sets), SET_CLOCK_TIME whenever the target second changes, a read-back
//     of GET_CLOCK_HOURS / _MINUTES / _SECONDS every second BEFORE that tick's set (more than 2 s off = a mismatch: counted, paused + set again);
//   * weather: steady = SET_WEATHER_TYPE_NOW_PERSIST(name) (instant, and the game's own weather cycle stays on it), a transition =
//     SET_CURR_WEATHER_STATE(fromHash, toHash, progress) re-issued every 500 ms or 1 % of progress, a GET_CURR_WEATHER_STATE read-back every 2 s.
// The session's end releases both (PAUSE_CLOCK(false), CLEAR_WEATHER_TYPE_PERSIST): the game's own clock and weather run again.
// Switch: world_clock in HKCU\Software\GAGE-MP (default on; 0 = 0.5.6: nothing received is applied, no native is called).
// Log: the switch line per session (ReadSwitch), then `engine: world clock (0.5.7), last 10 s: ...` while anything is held or received.
#pragma once

#include "protocol/protocol.h"

#include <cstdint>

namespace gage::game {
    struct World;
}

namespace gage::game::worldclock {

    // Join worker, at every Welcome: reads the switch and logs it.
    void ReadSwitch();
    // Join worker: one WorldClock from the server (rttMs = the session's smoothed round trip at that moment).
    void OnClock(const proto::WorldClock& m, uint32_t rttMs);
    // Join worker: the session ended; the next Tick releases the clock and the weather.
    void OnEnded();
    // Script tick (the game may be changed): holds the game on the received state, or releases it.
    void Tick(World& w);

}  // namespace gage::game::worldclock
