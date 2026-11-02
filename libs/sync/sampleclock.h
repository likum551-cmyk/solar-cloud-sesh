// SampleClock: puts the sender's own sample time (protocol v5 PlayerState.sampleMs / VehicleUpdate.sampleMs, a u16
// of whole milliseconds on the SENDER's clock, wrapping every 65.536 s) on the server's clock.
//
// Why: the server used to place a sample at the moment it processed the packet. That moment carries the whole
// uplink jitter, the worker's wait and the frame quantisation of the sender (+-10 ms is 6 cm at 6 m/s and 30 cm
// at 30 m/s), and it turns the age a client sees (WorldEntry.ageMs, VehicleState.ageMs) into a saw-tooth. The
// sender's clock has none of that: its samples are evenly spaced. What it lacks is a shared epoch, so the
// offset between the two clocks is estimated from the traffic itself.
//
// How (no clock exchange, no round trip): each sample unwraps its u16 against the newest one by the SIGNED delta
// (an out-of-order older sample comes out a little smaller, a wrap comes out a little larger), and the
// difference  recv - sender  is the offset plus that packet's network delay. The delay is never negative, so the
// smallest offset over a short window is the offset plus the fastest path: the sample time is
//     placed = sender + min(recv - sender over the window)
// which is the sender's even spacing shifted by one constant (a constant shifts every sample of a player
// alike, which no client can see). Edge rules:
//  * The first kWarmupSamples samples are placed at receipt: one lucky packet must not set the offset.
//  * Every placed time lies in [recv - kMaxBackMs, recv]. A sender whose clock stands still (the offset only
//    grows) is held at recv - kMaxBackMs and one that races (the offset shrinks) at recv, so a liar can make
//    its own samples look a little older but never newer than they arrive, and never absurdly old.
//  * A silence longer than kGapMs re-anchors (the sender may have restarted its clock or been suspended): the
//    estimator starts again and the next samples are placed at receipt.
//  * The window is kWindowMs of server time (the newest sample is always in it), so a path that got slower is
//    followed within that time, and a clock drifting 1 ms/s costs under 2 ms.
//
// Pure C++17, header-only, no clocks, no threads: the caller passes the receipt time (microseconds on any
// monotonic clock, the server's NowUs()) and gets a time on that same clock back. One instance per sender
// and per stream (PlayerState and VehicleUpdate run on separate sender timers). Tested by tests/sync_test.cpp
// (SampleClock) and tests/interest_test.cpp (TimelineTests: through the Replicator).
#pragma once

#include <cstdint>
#include <deque>

namespace gage::sync {

    class SampleClock
    {
    public:
        static constexpr int      kWarmupSamples = 8;
        static constexpr uint64_t kMaxBackMs     = 300;
        static constexpr uint64_t kGapMs         = 30000;
        static constexpr uint64_t kWindowMs      = 2000;

        // Where the sample stamped `sampleMs` (the sender's clock, whole ms, wraps at 65536) belongs on the
        // receiver's clock, given it was received at `recvUs`. Call once per received sample, in arrival order.
        uint64_t Place(uint16_t sampleMs, uint64_t recvUs)
        {
            if (started_ && recvUs > lastRecvUs_ && recvUs - lastRecvUs_ > kGapMs * 1000ull) Reset();
            if (!started_)
            {
                started_ = true;
                newestStamp_ = sampleMs;
                newestMs_ = 0; // an arbitrary origin: only differences of unwrapped times matter
            }
            // Signed delta against the newest sample: the u16 difference reinterpreted as i16.
            const int32_t delta = (int16_t)(uint16_t)(sampleMs - newestStamp_);
            const int64_t unwrappedMs = newestMs_ + delta;
            if (delta > 0)
            {
                newestMs_ = unwrappedMs;
                newestStamp_ = sampleMs;
            }
            if (recvUs > lastRecvUs_) lastRecvUs_ = recvUs;

            // offset = recv - sender, in us. Keep the window's running minimum (a monotonic deque: the front
            // is the smallest offset, entries behind it are larger and newer).
            const int64_t offUs = (int64_t)recvUs - unwrappedMs * 1000;
            while (!win_.empty() && win_.back().off >= offUs) win_.pop_back();
            win_.push_back({recvUs, offUs});
            while (win_.size() > 1 && win_.front().recvUs + kWindowMs * 1000ull < recvUs) win_.pop_front();

            if (seen_ < kWarmupSamples)
            {
                seen_++;
                return recvUs;
            }
            int64_t placed = unwrappedMs * 1000 + win_.front().off;
            const int64_t hi = (int64_t)recvUs;
            const int64_t lo = hi > (int64_t)(kMaxBackMs * 1000) ? hi - (int64_t)(kMaxBackMs * 1000) : 0;
            if (placed > hi) placed = hi;
            if (placed < lo) placed = lo;
            return (uint64_t)placed;
        }

        // Forget everything (a reconnect, or the sender's clock restarted).
        void Reset()
        {
            started_ = false;
            seen_ = 0;
            win_.clear();
            lastRecvUs_ = 0;
            newestMs_ = 0;
            newestStamp_ = 0;
        }

    private:
        struct Entry { uint64_t recvUs; int64_t off; };
        bool     started_ = false;
        int      seen_ = 0;
        uint64_t lastRecvUs_ = 0;
        int64_t  newestMs_ = 0;
        uint16_t newestStamp_ = 0;
        std::deque<Entry> win_;
    };

}
