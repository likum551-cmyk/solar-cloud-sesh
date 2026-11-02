// Small helpers shared by the GAGE server and client session: ENet's global
// init as an RAII scope, a monotonic millisecond clock, and a deterministic
// packet-drop simulator for the loss tests. Header-only, portable (ENet is the
// only platform-specific dependency).
#pragma once

#include <enet/enet.h>

#include <chrono>
#include <cstdint>

namespace gage::net {

    // enet_initialize is WSAStartup + timeBeginPeriod on Windows and a no-op
    // elsewhere; both halves are reference counted by the OS, so every owner
    // (each server, each client session) can hold its own scope.
    class EnetScope
    {
    public:
        EnetScope() : ok_(enet_initialize() == 0) {}
        ~EnetScope() { if (ok_) enet_deinitialize(); }
        EnetScope(const EnetScope&) = delete;
        EnetScope& operator=(const EnetScope&) = delete;
        bool ok() const { return ok_; }

    private:
        bool ok_;
    };

    // Monotonic milliseconds (64-bit, so no wrap-around handling at call sites).
    inline uint64_t NowMs()
    {
        using namespace std::chrono;
        return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // Drops `percent`% of the packets it is asked about, reproducibly: the same
    // seed and the same number of asks give the same answers. Test knob only;
    // percent 0 (the default) never drops.
    class DropSim
    {
    public:
        void Configure(uint32_t percent, uint32_t seed)
        {
            percent_ = percent > 100 ? 100 : percent;
            state_ = seed ? seed : 1u; // xorshift32 must not start at 0
        }
        bool Drop()
        {
            if (percent_ == 0) return false;
            state_ ^= state_ << 13;
            state_ ^= state_ >> 17;
            state_ ^= state_ << 5;
            return state_ % 100u < percent_;
        }

    private:
        uint32_t percent_ = 0;
        uint32_t state_ = 1;
    };

}
