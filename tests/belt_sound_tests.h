// 0.5.9 seatbelt sound (libs/sync/belt_sound.h + the player apps/engine/belt_audio.cpp). Included by tests/gameplay_test.cpp after its Check();
// run by beltsound_t::RunPure(), kChecks checks. Nothing is ever audible: the player runs against a fake sink that only records the bytes.
//   A. the decision: which vehicle-controls events click, driven through the real VehControls machine (re-asserts, the exit auto-unbuckle,
//      leaving, refused presses stay silent; each one paired with a presence check of the event it would have clicked on)
//   B. the switches (belt_sound, belt_sound_vol)
//   C. the sounds: level, no clipping, a sharp latch, length, spectrum, the unbuckle lighter / longer / lower, ends at 0, deterministic, the WAV
//   D. the player: one press = one play of the right image, muted, a failing sink, a busy sink never blocks the caller and plays only the
//      newest waiting sound, a volume change stops before it rebuilds, Stop
#pragma once

#define GAGE_BELT_AUDIO_NO_WINMM
#include "../apps/engine/belt_audio.cpp"

#include "sync/belt_sound.h"
#include "sync/vehicle_controls.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace beltsound_t {

    using namespace gage::sync;
    namespace bs = gage::sync::beltsound;
    namespace ba = gage::beltaudio;

    constexpr int kChecks = 24;   // A 8, B 1, C 8, D 7

    // ---- A. the machine driven tick by tick ----------------------------------------------------------------------------------------
    struct Drive
    {
        VehControls vc;
        double now = 0.0;
        VehControlsOut last;
        VehControlsOut Tick(uint32_t veh, unsigned presses, bool exiting = false, bool blocked = false, bool flagOk = false, bool flag = true)
        {
            VehControlsIn in;
            now += 33.0;
            in.nowMs = now;
            in.presses = presses;
            in.vehicle = veh;
            in.seat = veh ? 0 : -1;
            in.exiting = exiting;
            in.blocked = blocked;
            in.beltFlagOk = flagOk;
            in.beltFlag = flag;
            in.engineOk = true;
            in.engineRunning = true;
            last = vc.Tick(in);
            return last;
        }
        bool Has(VcEv k) const
        {
            for (int i = 0; i < last.nev; ++i)
                if (last.ev[i].kind == k)
                    return true;
            return false;
        }
    };

    inline void TestDecision()
    {
        {
            bool ok = bs::ForEvent(VcEvent{ VcEv::Belt, 1 }) == bs::Sound::Buckle && bs::ForEvent(VcEvent{ VcEv::Belt, 0 }) == bs::Sound::Unbuckle;
            const VcEv silent[] = { VcEv::None, VcEv::Entered, VcEv::Left, VcEv::Indicator, VcEv::AutoCancel, VcEv::EngineKey, VcEv::EngineGuard,
                                    VcEv::FuelCut, VcEv::BeltAuto, VcEv::BeltReassert, VcEv::Rejected };
            for (VcEv k : silent)
                for (uint8_t a = 0; a < 5; ++a)
                    ok = ok && bs::ForEvent(VcEvent{ k, a, 0 }) == bs::Sound::None;
            Check("belt sound: only VcEv::Belt clicks (1 = buckle, 0 = unbuckle); every other event kind is silent", ok);
        }
        Drive d;
        d.Tick(7, 0, false, false, true, true);                                  // sit in: the machine sets "unbuckled" silently
        const bool enterSilent = d.Has(VcEv::Entered) && bs::ForTick(d.last) == bs::Sound::None;
        d.Tick(7, kVcKeyBelt);
        Check("belt sound: sitting in is silent, then B buckles with the buckle click", enterSilent && d.Has(VcEv::Belt) && bs::ForTick(d.last) == bs::Sound::Buckle);
        d.Tick(7, 0, false, false, true, true);                                  // the game moved flag 32 under a buckled belt
        Check("belt sound: the flag re-assert is silent (the BeltReassert event IS there)", d.Has(VcEv::BeltReassert) && bs::ForTick(d.last) == bs::Sound::None);
        d.Tick(7, kVcKeyBelt);
        Check("belt sound: B again unbuckles with the unbuckle click", d.Has(VcEv::Belt) && !d.vc.Belt() && bs::ForTick(d.last) == bs::Sound::Unbuckle);
        d.Tick(7, kVcKeyBelt);
        const bool rebuckled = d.vc.Belt() && bs::ForTick(d.last) == bs::Sound::Buckle;
        d.Tick(7, 0, true);                                                      // F: the exit task starts, the machine unbuckles by itself
        Check("belt sound: the automatic unbuckle when the exit task starts is silent (the BeltAuto event IS there)",
              rebuckled && d.Has(VcEv::BeltAuto) && !d.vc.Belt() && bs::ForTick(d.last) == bs::Sound::None);
        d.Tick(0, 0);
        const bool leftAfterExit = d.Has(VcEv::Left) && bs::ForTick(d.last) == bs::Sound::None;
        d.Tick(9, 0);
        d.Tick(9, kVcKeyBelt);
        const bool buckled9 = d.vc.Belt();
        d.Tick(0, 0);                                                            // gone from the seat while buckled (death, a teleport)
        Check("belt sound: leaving the car is silent, also while buckled (the Left event IS there and says buckled)",
              leftAfterExit && buckled9 && d.Has(VcEv::Left) && d.last.ev[0].b == 1 && bs::ForTick(d.last) == bs::Sound::None);
        d.Tick(0, kVcKeyBelt);
        const bool notInCar = d.Has(VcEv::Rejected) && bs::ForTick(d.last) == bs::Sound::None;
        d.Tick(9, 0);
        d.Tick(9, kVcKeyBelt, false, true);                                      // the chat has the keyboard
        Check("belt sound: a refused B (not in a car, keyboard held) is silent (the Rejected event IS there)",
              notInCar && d.Has(VcEv::Rejected) && !d.vc.Belt() && bs::ForTick(d.last) == bs::Sound::None);
        // One press, one sound: 40 ticks, B on every 4th, flag read-backs that disagree on every 3rd tick (re-asserts) - exactly 10 sounds,
        // alternating buckle / unbuckle, each on the tick of its press.
        Drive e;
        e.Tick(5, 0);
        int sounds = 0, presses = 0, reasserts = 0;
        bool alternating = true, onPressTick = true;
        bs::Sound prev = bs::Sound::Unbuckle;
        for (int t = 1; t <= 40; ++t)
        {
            const bool press = t % 4 == 0;
            const bool flagRead = t % 3 == 0;
            e.Tick(5, press ? kVcKeyBelt : 0u, false, false, flagRead, !e.vc.BeltFlagWanted());   // a read that disagrees with the machine
            presses += press ? 1 : 0;
            reasserts += e.Has(VcEv::BeltReassert) ? 1 : 0;
            const bs::Sound s = bs::ForTick(e.last);
            if (s != bs::Sound::None)
            {
                ++sounds;
                alternating = alternating && s != prev;
                prev = s;
            }
            onPressTick = onPressTick && ((s != bs::Sound::None) == press);
        }
        Check("belt sound: 40 ticks with 10 B presses and re-asserts between them: exactly 10 sounds, alternating, each on its press's tick",
              sounds == 10 && presses == 10 && reasserts > 0 && alternating && onPressTick,
              "sounds " + std::to_string(sounds) + " presses " + std::to_string(presses) + " reasserts " + std::to_string(reasserts));
    }

    // ---- B. the switches ---------------------------------------------------------------------------------------------------------
    inline void TestSwitches()
    {
        const bool on = bs::ParseOn(L"") && bs::ParseOn(L"1") && !bs::ParseOn(L"0");
        const bs::Vol a = bs::ParseVol(L""), b = bs::ParseVol(L" 35 "), c = bs::ParseVol(L"0"), d = bs::ParseVol(L"150"), e = bs::ParseVol(L"loud");
        Check("belt sound: belt_sound unset / 1 = on, 0 = off; belt_sound_vol unset = 60, ' 35 ' = 35, 0 = 0, 150 = 100 (said), 'loud' = 60 (said)",
              on && a.pct == bs::kVolDefault && !a.why && b.pct == 35 && !b.why && c.pct == 0 && !c.why && d.pct == 100 && d.why && e.pct == bs::kVolDefault && e.why);
    }

    // ---- C. the sounds -------------------------------------------------------------------------------------------------------------
    struct Shape
    {
        int peak = 0, peakAt = 0, first20 = -1, last40 = 0, n = 0;
        double total = 0, after = 0, lowBand = 0, mainBand = 0, highBand = 0, centroid = 0;
    };
    inline double Goertzel(const std::vector<int16_t>& s, double f)
    {
        const double w = 2.0 * 3.14159265358979 * f / bs::kRate, c = 2.0 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (int16_t v : s)
        {
            const double s0 = v / 32768.0 + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    // afterMs: the share of the energy after this time (the tail).
    inline Shape Measure(const std::vector<int16_t>& s, double afterMs)
    {
        Shape r;
        r.n = (int)s.size();
        for (int i = 0; i < r.n; ++i)
            if (std::abs((int)s[(size_t)i]) > r.peak) { r.peak = std::abs((int)s[(size_t)i]); r.peakAt = i; }
        double eAll = 0, eAfter = 0;
        for (int i = 0; i < r.n; ++i)
        {
            const int a = std::abs((int)s[(size_t)i]);
            if (a > r.peak / 100) r.last40 = i;
            if (a > r.peak / 10 && r.first20 < 0) r.first20 = i;
            const double q = (double)s[(size_t)i] * s[(size_t)i];
            eAll += q;
            if (i * 1000.0 / bs::kRate >= afterMs) eAfter += q;
        }
        r.after = eAll > 0 ? eAfter / eAll : 0;
        double cen = 0;
        for (double f = 50; f < 16000; f += 50)
        {
            const double g = Goertzel(s, f);
            r.total += g;
            cen += g * f;
            if (f < 1000) r.lowBand += g;
            else if (f >= 2000 && f < 4500) r.mainBand += g;
            else if (f >= 4500) r.highBand += g;
        }
        r.centroid = r.total > 0 ? cen / r.total : 0;
        return r;
    }
    inline double Ms(int samples) { return samples * 1000.0 / bs::kRate; }

    inline void TestSounds()
    {
        const std::vector<int16_t> b60 = bs::Synth(bs::kBuckle, 60), b100 = bs::Synth(bs::kBuckle, 100);
        const std::vector<int16_t> u60 = bs::Synth(bs::kUnbuckle, 60), u100 = bs::Synth(bs::kUnbuckle, 100);
        const Shape b = Measure(b100, 60.0), u = Measure(u100, 40.0);
        const Shape bq = Measure(b60, 60.0);
        const int want100 = (int)std::lround(32767.0 * bs::kPeakFull), want60 = (int)std::lround(32767.0 * bs::kPeakFull * 0.6);
        Check("belt sound: the buckle peaks at 0.89 x full scale x volume (+-1) at 100 and 60, and nothing reaches full scale",
              std::abs(b.peak - want100) <= 1 && std::abs(bq.peak - want60) <= 1 && b.peak < 32767,
              "peak " + std::to_string(b.peak) + " / " + std::to_string(bq.peak));
        const double latch = bs::kBuckle.layers[1].atMs;
        Check("belt sound: the buckle is a sharp latch: its peak lands within 1 ms of the latch, the rise from -20 dB to the peak takes under 1 ms",
              std::fabs(Ms(b.peakAt) - latch) < 1.0 && b.first20 >= 0 && Ms(b.peakAt - b.first20) < 1.0,
              "peak at " + std::to_string(Ms(b.peakAt)) + " ms, -20 dB at " + std::to_string(Ms(b.first20)) + " ms");
        Check("belt sound: the buckle is audible (above -40 dB of its peak) for 60..120 ms and nearly all of it is over 60 ms in",
              Ms(b.last40) >= 60.0 && Ms(b.last40) <= 120.0 && b.after < 0.02,
              "audible to " + std::to_string(Ms(b.last40)) + " ms, energy after 60 ms " + std::to_string(b.after));
        Check("belt sound: the buckle is metal, not a beep: most energy at 2-4.5 kHz, a broadband transient above 4.5 kHz, little below 1 kHz",
              b.mainBand / b.total > 0.5 && b.highBand / b.total > 0.05 && b.lowBand / b.total < 0.15,
              "2-4.5k " + std::to_string(b.mainBand / b.total) + " >4.5k " + std::to_string(b.highBand / b.total) + " <1k " + std::to_string(b.lowBand / b.total));
        Check("belt sound: the unbuckle is lighter (peak 70-80 % of the buckle's), longer (audible 150-300 ms: the strap slide carries > 10 % of its "
              "energy after 40 ms, the buckle < 2 % after 60 ms) and lower (centroid below the buckle's)",
              u.peak >= 0.7 * b.peak && u.peak <= 0.8 * b.peak && Ms(u.last40) >= 150.0 && Ms(u.last40) <= 300.0 && u.after > 0.1 && u.centroid < b.centroid,
              "peak " + std::to_string(u.peak) + " audible to " + std::to_string(Ms(u.last40)) + " ms, after 40 ms " + std::to_string(u.after) +
              ", centroid " + std::to_string(u.centroid) + " vs " + std::to_string(b.centroid));
        {
            const bool ends = b100.back() == 0 && u100.back() == 0 && b60.back() == 0 && u60.back() == 0;
            // The first sample is the first step of the 0.2 ms attack ramp (1 / 9 of a noise sample): small, never a jump to the peak.
            const bool starts = std::abs((int)b100.front()) < b.peak / 10 && std::abs((int)u100.front()) < u.peak / 10;
            Check("belt sound: both sounds start near 0 (under 10 % of the peak: the attack ramp) and end at exactly 0", ends && starts,
                  "first samples " + std::to_string(b100.front()) + " / " + std::to_string(u100.front()) + ", last " + std::to_string(b100.back()) + " / " +
                  std::to_string(u100.back()));
        }
        {
            const std::vector<int16_t> again = bs::Synth(bs::kBuckle, 100), silent = bs::Synth(bs::kUnbuckle, 0);
            bool zero = !silent.empty();
            for (int16_t v : silent) zero = zero && v == 0;
            Check("belt sound: the synthesis is deterministic (two builds identical) and volume 0 is all zeros", again == b100 && zero);
        }
        {
            const std::vector<uint8_t> w = bs::BuildWav(u60);
            auto rd32 = [&](size_t at) { return (uint32_t)w[at] | ((uint32_t)w[at + 1] << 8) | ((uint32_t)w[at + 2] << 16) | ((uint32_t)w[at + 3] << 24); };
            auto rd16 = [&](size_t at) { return (uint16_t)(w[at] | (w[at + 1] << 8)); };
            bool ok = w.size() == 44 + 2 * u60.size() && std::memcmp(&w[0], "RIFF", 4) == 0 && rd32(4) == w.size() - 8 && std::memcmp(&w[8], "WAVEfmt ", 8) == 0 &&
                      rd32(16) == 16 && rd16(20) == 1 && rd16(22) == 1 && rd32(24) == 44100 && rd32(28) == 88200 && rd16(32) == 2 && rd16(34) == 16 &&
                      std::memcmp(&w[36], "data", 4) == 0 && rd32(40) == 2 * u60.size();
            for (size_t i = 0; ok && i < u60.size(); ++i)
                ok = (int16_t)rd16(44 + 2 * i) == u60[i];
            Check("belt sound: the WAV image is RIFF PCM mono 16-bit 44.1 kHz with the right sizes, and its samples are the synthesis", ok);
        }
    }

    // ---- D. the player against a fake sink -----------------------------------------------------------------------------------------
    struct Fake
    {
        std::mutex mu;
        std::condition_variable cv;
        std::vector<std::vector<uint8_t>> plays;   // what was given (a stop is an empty entry)
        int stops = 0;
        bool fail = false;
        bool hold = false;                          // a busy sink: blocks until released
        bool inside = false;
    };
    inline Fake& F() { static Fake f; return f; }
    inline bool FakeSink(const uint8_t* wav, size_t n)
    {
        Fake& f = F();
        std::unique_lock<std::mutex> lk(f.mu);
        if (!wav)
        {
            ++f.stops;
            f.plays.emplace_back();
            f.cv.notify_all();
            return true;
        }
        f.plays.emplace_back(wav, wav + n);
        f.inside = true;
        f.cv.notify_all();
        f.cv.wait(lk, [&f] { return !f.hold; });
        f.inside = false;
        return !f.fail;
    }
    inline void ResetFake()
    {
        Fake& f = F();
        std::lock_guard<std::mutex> lk(f.mu);
        f.plays.clear();
        f.stops = 0;
        f.fail = false;
        f.hold = false;
        f.inside = false;
    }
    inline int Played()
    {
        Fake& f = F();
        std::lock_guard<std::mutex> lk(f.mu);
        int n = 0;
        for (auto& p : f.plays) n += p.empty() ? 0 : 1;
        return n;
    }
    // Waits (bounded) until the fake has seen `n` plays; then 60 ms more, so a second play that should not come has had its chance.
    inline bool WaitPlays(int n, int settleMs = 60)
    {
        for (int i = 0; i < 2000 && Played() < n; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
        return Played() == n;
    }
    inline bool WaitBuilt(ba::Player& p, int vol)
    {
        for (int i = 0; i < 2000 && p.BuiltInfo().vol != vol; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return p.BuiltInfo().vol == vol;
    }
    inline std::vector<uint8_t> PlayAt(size_t i)
    {
        Fake& f = F();
        std::lock_guard<std::mutex> lk(f.mu);
        size_t k = 0;
        for (auto& p : f.plays)
            if (!p.empty() && k++ == i)
                return p;
        return {};
    }

    inline void TestPlayer()
    {
        {
            ResetFake();
            ba::Player p(FakeSink);
            p.Configure(true, 60);
            const bool built = WaitBuilt(p, 60);
            const bool req = p.Request(bs::Sound::Buckle);
            const bool once = WaitPlays(1);
            const ba::Counters c = p.Take();
            Check("belt sound player: one request = exactly one play of the buckle image at the volume (nothing more 60 ms later), counted",
                  built && req && once && PlayAt(0) == bs::BuildWav(bs::Sound::Buckle, 60) && c.requested == 1 && c.played == 1 && c.failed == 0 && c.buckles == 1,
                  "plays " + std::to_string(Played()));
            p.Request(bs::Sound::Unbuckle);
            const bool two = WaitPlays(2);
            Check("belt sound player: an unbuckle request plays the unbuckle image", two && PlayAt(1) == bs::BuildWav(bs::Sound::Unbuckle, 60));
        }
        {
            ResetFake();
            ba::Player p(FakeSink);
            p.Configure(false, 60);
            WaitBuilt(p, 60);
            const bool r1 = p.Request(bs::Sound::Buckle);
            p.Configure(true, 0);
            const bool r2 = p.Request(bs::Sound::Unbuckle);
            const bool none = WaitPlays(0);
            const ba::Counters c = p.Take();
            Check("belt sound player: belt_sound=0 and belt_sound_vol=0 play nothing (muted, counted)",
                  !r1 && !r2 && none && c.muted == 2 && c.requested == 0 && !p.Request(bs::Sound::None));
        }
        {
            ResetFake();
            { std::lock_guard<std::mutex> lk(F().mu); F().fail = true; }
            ba::Player p(FakeSink);
            p.Configure(true, 60);
            WaitBuilt(p, 60);
            p.Request(bs::Sound::Buckle);
            WaitPlays(1);
            const ba::Counters c = p.Take();
            Check("belt sound player: a sink that refuses is counted as failed, not played", c.failed == 1 && c.played == 0);
        }
        {
            // A busy sink (the first play blocks for as long as the test holds it): requests still return at once, and when it frees only the
            // newest waiting sound plays, once.
            ResetFake();
            { std::lock_guard<std::mutex> lk(F().mu); F().hold = true; }
            ba::Player p(FakeSink);
            p.Configure(true, 60);
            WaitBuilt(p, 60);
            p.Request(bs::Sound::Buckle);
            {
                std::unique_lock<std::mutex> lk(F().mu);
                F().cv.wait_for(lk, std::chrono::seconds(2), [] { return F().inside; });
            }
            const auto t0 = std::chrono::steady_clock::now();
            const bool r2 = p.Request(bs::Sound::Unbuckle);
            const bool r3 = p.Request(bs::Sound::Buckle);
            const double callMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const int whileBusy = Played();
            {
                std::lock_guard<std::mutex> lk(F().mu);
                F().hold = false;
                F().cv.notify_all();
            }
            const bool two = WaitPlays(2);
            const ba::Counters c = p.Take();
            Check("belt sound player: requests during a busy sink return at once (< 20 ms for two) and only the newest waiting one plays, once",
                  r2 && r3 && callMs < 20.0 && whileBusy == 1 && two && PlayAt(1) == bs::BuildWav(bs::Sound::Buckle, 60) && c.superseded == 1 && c.played == 2,
                  "calls " + std::to_string(callMs) + " ms, plays " + std::to_string(Played()) + ", superseded " + std::to_string(c.superseded));
        }
        {
            // A volume change: what may still play is stopped BEFORE the images are rebuilt, and the next play is the new image.
            ResetFake();
            ba::Player p(FakeSink);
            p.Configure(true, 60);
            WaitBuilt(p, 60);
            p.Request(bs::Sound::Buckle);
            WaitPlays(1, 0);
            p.Configure(true, 100);
            const bool rebuilt = WaitBuilt(p, 100);
            p.Request(bs::Sound::Buckle);
            WaitPlays(2);
            bool stopBetween = false;
            {
                std::lock_guard<std::mutex> lk(F().mu);
                stopBetween = F().plays.size() == 3 && !F().plays[0].empty() && F().plays[1].empty() && !F().plays[2].empty();
            }
            const ba::Built bi = p.BuiltInfo();
            Check("belt sound player: a volume change stops the playback before the rebuild; the next play is the new image (peak 29163)",
                  rebuilt && stopBetween && PlayAt(1) == bs::BuildWav(bs::Sound::Buckle, 100) && bi.bucklePeak == (int)std::lround(32767.0 * bs::kPeakFull),
                  "peak " + std::to_string(bi.bucklePeak));
            p.Stop();
            int stops = 0;
            {
                std::lock_guard<std::mutex> lk(F().mu);
                stops = F().stops;
            }
            Check("belt sound player: Stop ends the worker with a final stop of the playback; a request after it plays nothing",
                  stops == 2 && !p.Request(bs::Sound::Buckle) && WaitPlays(2, 30));
        }
    }

    inline void RunPure()
    {
        std::printf("-- 0.5.9 seatbelt sound (decision, switches, the synthesized sounds, the player against a fake sink)\n");
        TestDecision();
        TestSwitches();
        TestSounds();
        TestPlayer();
    }

}
