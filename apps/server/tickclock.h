// TickClock: the server's tick schedule as pure arithmetic over a microsecond clock the caller reads.
// Tick numbers follow WALL time: when the loop sleeps through whole periods (a stall, a debugger, a
// suspended VM) the numbers jump by the periods that were skipped instead of counting only the ticks that
// ran, because clients turn a tick number into a time (tick * tickMs - sampleAge) and a number that fell
// behind the wall clock would put every later sample in the past. A tick is also stamped with the time of
// its OWN slot (the nominal time), which is what sample ages are measured against: a tick that starts
// late by a few milliseconds still describes the world as of its slot, and the client's tick clock is
// built on exactly that.
//
// No clocks, no threads, no allocation: tests drive it with made-up times (tests/interest_test.cpp).
#pragma once

#include <cstdint>

namespace gage::server {

    class TickClock
    {
    public:
        struct Slot
        {
            uint32_t tick = 0;       // the tick number to run (>= 1; 0 means "nothing yet" in a WorldAck)
            uint64_t nominalUs = 0;  // the time of that tick's own slot
            uint64_t lateUs = 0;     // how late the tick starts against the slot that was due (before skipping)
            uint32_t missed = 0;     // whole periods skipped: those ticks never happen
        };

        // The first tick is due one period after `nowUs`. A period of 0 is treated as 1 us.
        void Start(uint64_t nowUs, uint64_t periodUs)
        {
            period_ = periodUs ? periodUs : 1;
            next_ = nowUs + period_;
            tick_ = 1;
        }

        bool Due(uint64_t nowUs) const { return nowUs >= next_; }

        // When the next tick is due (the slot after the last one that ran).
        uint64_t NextDueUs() const { return next_; }

        // Takes the tick that is due at `nowUs` (call only when Due(nowUs)): a loop that is less than one
        // period late runs the slot that was due; one that is later skips ahead to the slot that
        // contains `nowUs`. The next tick is due one period after the slot just taken, so the schedule
        // stays on its grid and never bursts to catch up on skipped ticks.
        Slot Advance(uint64_t nowUs)
        {
            Slot s;
            s.lateUs = nowUs > next_ ? nowUs - next_ : 0;
            const uint64_t skipped = s.lateUs / period_;
            s.missed = (uint32_t)skipped;
            s.tick = tick_ + (uint32_t)skipped;
            s.nominalUs = next_ + skipped * period_;
            tick_ = s.tick + 1;
            next_ = s.nominalUs + period_;
            return s;
        }

    private:
        uint64_t period_ = 50000, next_ = 0;
        uint32_t tick_ = 1; // the number the next tick takes
    };

}
