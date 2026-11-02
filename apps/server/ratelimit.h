// Token-bucket rate limiting for the server: one bucket (chat flood, failed admin
// logins) and a per-source limiter with a bounded table (UDP query replies).
// Header-only, portable, driven by the caller's millisecond clock so tests and the
// server decide what "now" is.
//
// Thread-safety: neither class locks. Use each instance from one thread (the server
// uses them only from its Poll() thread). Errors: none; every call is total.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace gage::server {

    // Holds up to `capacity` tokens and refills `perSecond` of them per second. It
    // starts full: the first `capacity` takes in a burst succeed, then one per
    // 1/perSecond seconds. The clock is whatever monotonic millisecond counter the
    // caller passes in (the first call only anchors it); a clock that goes backwards
    // refills nothing.
    class TokenBucket
    {
    public:
        // A bucket with no capacity: Take() always fails until Configure() is called.
        TokenBucket() = default;
        // Same as default-constructing, then Configure(capacity, perSecond).
        TokenBucket(double capacity, double perSecond) { Configure(capacity, perSecond); }

        // (Re)sets the size and refill rate and refills the bucket completely. `capacity`
        // is the burst size in tokens; `perSecond` the sustained rate in tokens/s.
        void Configure(double capacity, double perSecond)
        {
            capacity_ = capacity;
            perMs_ = perSecond / 1000.0;
            tokens_ = capacity;
            started_ = false;
        }

        // Refills to `nowMs`, then takes `n` tokens if they are there. Returns true when
        // taken; false (taking nothing) when fewer than `n` tokens are available.
        bool Take(uint64_t nowMs, double n = 1.0)
        {
            Refill(nowMs);
            if (tokens_ < n) return false;
            tokens_ -= n;
            return true;
        }

        // True when `n` tokens are there at `nowMs`; takes nothing.
        bool Has(uint64_t nowMs, double n = 1.0)
        {
            Refill(nowMs);
            return tokens_ >= n;
        }

        // The tokens there are at `nowMs` (a damage budget takes what is left of it); takes nothing.
        double Available(uint64_t nowMs)
        {
            Refill(nowMs);
            return tokens_;
        }

        // True when the bucket is back to full at `nowMs` (an idle source: its entry
        // can be forgotten without changing any later decision).
        bool Full(uint64_t nowMs)
        {
            Refill(nowMs);
            return tokens_ >= capacity_;
        }

    private:
        void Refill(uint64_t nowMs)
        {
            if (!started_)
            {
                started_ = true;
                lastMs_ = nowMs;
                return;
            }
            if (nowMs > lastMs_)
            {
                tokens_ += (double)(nowMs - lastMs_) * perMs_;
                if (tokens_ > capacity_) tokens_ = capacity_;
            }
            lastMs_ = nowMs;
        }

        double capacity_ = 0;
        double perMs_ = 0;
        double tokens_ = 0;
        uint64_t lastMs_ = 0;
        bool started_ = false;
    };

    // Per-source (an IPv4 address) plus overall limit, both token buckets with one
    // second of burst. Memory is bounded for spoofed-source floods: at most
    // `kMaxSources` distinct sources are tracked; when the table is full, idle
    // entries (back at full) are swept at most twice a second and, while it stays
    // full, packets from untracked sources are refused rather than the table grown.
    class SourceLimiter
    {
    public:
        // The most distinct sources remembered at once.
        static constexpr size_t kMaxSources = 4096;

        // Sets both limits (events per second; the burst is one second's worth) and
        // forgets every tracked source. Defaults, before any call: 20 per source,
        // 2000 in total.
        void Configure(double perSourcePerSecond, double totalPerSecond)
        {
            perSource_ = perSourcePerSecond;
            total_.Configure(totalPerSecond, totalPerSecond);
            sources_.clear();
        }

        // True when one more event from `source` (an IPv4 address as a 32-bit integer,
        // any consistent byte order) is within both the per-source and the overall
        // limit at `nowMs`; the event is then counted. False means drop it. An event
        // refused by the per-source limit does not use up overall budget.
        bool Allow(uint32_t source, uint64_t nowMs)
        {
            auto it = sources_.find(source);
            if (it == sources_.end())
            {
                if (sources_.size() >= kMaxSources)
                {
                    Sweep(nowMs);
                    if (sources_.size() >= kMaxSources) return false;
                }
                it = sources_.emplace(source, TokenBucket(perSource_, perSource_)).first;
            }
            if (!it->second.Take(nowMs)) return false;
            return total_.Take(nowMs);
        }

        // How many sources are remembered right now (never more than kMaxSources).
        size_t tracked() const { return sources_.size(); }

    private:
        void Sweep(uint64_t nowMs)
        {
            if (swept_ && nowMs - lastSweepMs_ < 500) return;
            swept_ = true;
            lastSweepMs_ = nowMs;
            for (auto it = sources_.begin(); it != sources_.end();)
            {
                if (it->second.Full(nowMs))
                    it = sources_.erase(it);
                else
                    ++it;
            }
        }

        double perSource_ = 20;
        TokenBucket total_{2000, 2000};
        std::unordered_map<uint32_t, TokenBucket> sources_;
        uint64_t lastSweepMs_ = 0;
        bool swept_ = false;
    };

}
