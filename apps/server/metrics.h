// Server performance counters: what `stats` prints and what the load-test harness
// (apps/loadtest) samples. Everything here is cumulative and monotonic (a window is the
// difference of two snapshots), updated only on the Poll() thread with a handful of
// integer operations per event, so it is cheap enough to stay on in production.
//
// Thread-safety: none. Metrics is a plain value; Server::metrics() hands out a const
// reference that must be read (or copied) on the thread that calls Poll(), like every
// other Server accessor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gage::server {

    // A cumulative log-scale histogram of unsigned samples (microseconds, bytes,
    // entry counts): 8 buckets per octave (about 9% wide) from 1 to 2^32, plus an
    // exact count, sum and maximum. Two snapshots of the same histogram give any
    // window's count, mean and percentiles (Quantile with `since`). The maximum is a
    // peak since the last ResetMax(), not windowable.
    class LogHistogram
    {
    public:
        static constexpr int kPerOctave = 8;
        static constexpr int kOctaves = 32;
        static constexpr int kBuckets = 1 + kPerOctave * kOctaves;

        // Adds one sample. Bucket 0 holds 0; bucket 1 + 8*oct + sub holds
        // [(8+sub) * 2^(oct-3), (9+sub) * 2^(oct-3)).
        void Add(uint64_t v)
        {
            counts_[Index(v)]++;
            count_++;
            sum_ += v;
            if (v > max_) max_ = v;
        }
        uint64_t count() const { return count_; }
        uint64_t sum() const { return sum_; }
        uint64_t max() const { return max_; }
        void ResetMax() { max_ = 0; }

        // Mean of the samples added since `older` (a copy of this histogram taken
        // earlier; nullptr = since the start). 0 when there are none.
        double Mean(const LogHistogram* older = nullptr) const
        {
            const uint64_t n = count_ - (older ? older->count_ : 0);
            const uint64_t s = sum_ - (older ? older->sum_ : 0);
            return n ? (double)s / (double)n : 0.0;
        }
        uint64_t CountSince(const LogHistogram* older = nullptr) const
        {
            return count_ - (older ? older->count_ : 0);
        }
        // The q-quantile (q in [0,1]) of the samples added since `older`: the midpoint
        // of the bucket holding that rank, so within about 5% of the true value (0 for
        // an empty window).
        double Quantile(double q, const LogHistogram* older = nullptr) const
        {
            const uint64_t n = CountSince(older);
            if (n == 0) return 0.0;
            if (q < 0) q = 0;
            if (q > 1) q = 1;
            uint64_t rank = (uint64_t)(q * (double)n);
            if (rank >= n) rank = n - 1;
            uint64_t seen = 0;
            for (int i = 0; i < kBuckets; i++)
            {
                seen += counts_[i] - (older ? older->counts_[i] : 0);
                if (seen > rank) return Mid(i);
            }
            return (double)max_;
        }

        static int Index(uint64_t v)
        {
            if (v == 0) return 0;
            int oct = 0;
            for (uint64_t t = v; t > 1; t >>= 1) oct++;
            const uint64_t sub = oct >= 3 ? ((v >> (oct - 3)) & 7u) : ((v << (3 - oct)) & 7u);
            const int idx = 1 + oct * kPerOctave + (int)sub;
            return idx < kBuckets ? idx : kBuckets - 1;
        }
        static double Lower(int idx)
        {
            if (idx <= 0) return 0.0;
            const int i = idx - 1;
            const int oct = i / kPerOctave, sub = i % kPerOctave;
            double scale = 1.0;
            for (int k = 0; k < oct - 3; k++) scale *= 2.0;
            for (int k = 0; k < 3 - oct; k++) scale *= 0.5;
            return (double)(8 + sub) * scale;
        }
        static double Mid(int idx)
        {
            if (idx <= 0) return 0.0;
            return (Lower(idx) + Lower(idx + 1)) * 0.5;
        }

    private:
        uint32_t counts_[kBuckets] = {};
        uint64_t count_ = 0, sum_ = 0, max_ = 0;
    };

    // Everything the server counts about its own performance. See docs/api/server.md
    // ("stats") for what each field means in a report.
    struct Metrics
    {
        // ---- the tick loop ----
        uint64_t ticks = 0;          // ticks executed
        uint64_t ticksMissed = 0;    // whole tick periods skipped because the loop ran late
        LogHistogram tickWorkUs;     // per tick: time spent replicating (build + hand to ENet + flush)
        LogHistogram tickLateUs;     // per tick: how late the tick started against its slot

        // ---- replication ----
        uint64_t worldPackets = 0;       // WorldState packets handed to ENet
        uint64_t worldEntries = 0;       // player entries in them
        uint64_t worldPayloadBytes = 0;  // their payload bytes
        LogHistogram worldBytes;         // per WorldState packet: payload bytes
        LogHistogram worldEntryCount;    // per WorldState packet: entries
        uint64_t coarsePackets = 0;      // WorldCoarse packets (protocol v2)
        uint64_t coarseEntries = 0;
        uint64_t coarsePayloadBytes = 0;
        uint64_t deltaEntries = 0;       // entries sent as a delta against an acked baseline
        uint64_t absoluteEntries = 0;    // entries sent in full
        uint64_t budgetDeferred = 0;     // due entries held back by the bandwidth budget
        uint64_t relevanceEnters = 0;    // (receiver, subject) pairs that became relevant
        uint64_t relevanceLeaves = 0;    // pairs that stopped being relevant
        uint64_t relevantPairs = 0;      // gauge: pairs relevant right now
        uint64_t playersJoined = 0;      // gauge: joined players at the last tick

        // ---- sync diagnostics (docs/SYNC-PLAN.md, network N6 counters) ----
        // What became of every relevant (receiver, subject) pair on every tick: exactly one of these six.
        uint64_t pairSent = 0;               // an entry went into the receiver's WorldState
        uint64_t pairIdleSuppressed = 0;     // not due, and the idle rate (rateIdleHz) is what held it back
        uint64_t pairRateNotDue = 0;         // not due at its distance class's rate
        uint64_t pairBudgetDeferred = 0;     // due, left out: the receiver's byte budget was the limit
        uint64_t pairPacketCapDeferred = 0;  // due, left out: the one-packet size cap was the limit
        uint64_t pairNoState = 0;            // skipped: the subject or the receiver has sent no state yet
        // Idle wake-ups. wakes: an idle player's sample left its dead-band, so its pairs are due at once.
        // noiseWakesHeld: the wake-ups a bit-exact idle test would have caused (a sample that differs from
        // what is replicated but stays inside the dead-band), counted at most once per idleAfterMs per player.
        uint64_t wakes = 0;
        uint64_t noiseWakesHeld = 0;
        // Samples (accepted PlayerStates, plus the server's own re-pushes of changed vitals). A forwarded
        // sample went into at least one WorldState; forwardedTwice counts a sample re-sent on a later tick
        // (idle refreshes, a sender slower than the tick). Received minus forwarded = never forwarded: the
        // samples a newer one replaced before any tick sent them (by design for idle and far pairs; at near
        // range a 30 Hz sender into a 20 Hz tick loses a third).
        uint64_t samplesReceived = 0;
        uint64_t samplesForwarded = 0;
        uint64_t samplesForwardedTwice = 0;

        // ---- traffic ----
        uint64_t payloadBytesOut = 0;    // application bytes handed to ENet, all peers, all messages
        uint64_t packetsOut = 0;         // application packets handed to ENet
        uint64_t payloadBytesIn = 0;     // application bytes received from joined peers
        uint64_t packetsIn = 0;
        uint64_t wireBytesOut = 0;       // ENet's own counters: bytes in the UDP datagrams sent (ENet headers included, IP/UDP not)
        uint64_t wireDatagramsOut = 0;
        uint64_t wireBytesIn = 0;
        uint64_t wireDatagramsIn = 0;
        LogHistogram clientBytesPerSec;  // one sample per joined player per second: payload bytes sent to it

        // ---- CPU ----
        double cpuSeconds = 0;           // CPU time of the thread that calls Poll(), since Start()
        double wallSeconds = 0;          // wall time since Start() at the last tick

        // Clears the peaks that cannot be windowed.
        void ResetPeaks()
        {
            tickWorkUs.ResetMax();
            tickLateUs.ResetMax();
            worldBytes.ResetMax();
        }
    };

    // The `stats` console text for the interval from `since` (a copy of the metrics taken
    // at the previous report; pass a default Metrics for "since start") to `now`, over
    // `windowSeconds` of wall time. One block of `key value` lines, easy to grep.
    std::string FormatMetrics(const Metrics& now, const Metrics& since, double windowSeconds, uint32_t tickHz,
                              size_t players);

    // The same numbers as one JSON object on one line (for the admin channel and scripts).
    std::string FormatMetricsJson(const Metrics& now, const Metrics& since, double windowSeconds, uint32_t tickHz,
                                  size_t players);

}
