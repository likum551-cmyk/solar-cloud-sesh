// 0.5.7 world clock (GI 2026-11-07: "each player has their own night / day, it's desynced"): the SERVER owns the time of day and the weather;
// every client shows the server's minute and the server's weather. Pure C++17 (no game, OS or protocol headers), header-only, used by the
// server (apps/server: the clock, the weather plan, the config parsers), the engine (apps/engine/world_clock.cpp: the received state and the
// two holds that decide which natives to call) and the tests (tests/world_clock_tests.h in gameplay_test, tests/world_clock_net_tests.h in
// server_test).
//
// The model:
//   * The clock is a line, not a counter: game time of day = anchorGameMs + (now - anchorMs) * scale, wrapped to a day (kDayMs). scale is game
//     ms per real ms in thousandths (scaleMilli; kGtaScaleMilli 30000 = GTA's own rate, 1 game minute per 2 real seconds, a 48-minute day;
//     0 = frozen). An admin change (a new time, a new scale) re-anchors at that moment, so nothing jumps except what the admin asked for.
//   * The server sends the game time AT THE MOMENT IT SENDS (proto::WorldClock), plus the scale; the client places that moment at its receive
//     time minus half its round trip (Received / FromWire) and runs the same line on its own monotonic clock. Error: the one-way latency's
//     variation times the scale, about 1 game second per 33 ms at the default rate, so every machine shows the same minute. A resync every 30 s
//     (and every change) re-anchors it.
//   * Weather: either one fixed weather or a cycle of (weather, real minutes) entries. A change (an admin's, or the cycle reaching its next
//     entry) is a TRANSITION from the weather shown at that moment to the new one over transMs, sent as (from, to, elapsedMs, totalMs) so a
//     player who joins in the middle of it starts at the same blend as everybody else.
//   * Client holds (ClockHold, WeatherHold): what to call this tick so the game shows the target with as few native calls as needed, and what
//     a read-back must show (a mismatch is counted and re-asserted at once). The engine maps their plans onto PAUSE_CLOCK / SET_CLOCK_TIME /
//     GET_CLOCK_* and SET_WEATHER_TYPE_NOW_PERSIST / SET_CURR_WEATHER_STATE / GET_CURR_WEATHER_STATE.
#pragma once

#include "sync/gtaids.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace gage::sync::wclock {

    constexpr int64_t kDayMs = 86400000;               // one game day in game milliseconds
    constexpr uint32_t kGtaScaleMilli = 30000;         // GTA's own rate: 60000 game ms per 2000 real ms = 30.000
    constexpr uint32_t kMaxScaleMilli = 3600000;       // 3600.000: a game day in 24 real seconds (the fastest a server may set)
    constexpr uint32_t kMaxTransMs = 600000;           // a weather transition is at most 10 real minutes
    constexpr size_t kMaxCycleEntries = 32;
    constexpr uint32_t kMaxCycleMinutes = 1440;        // one cycle entry lasts 1..1440 real minutes
    constexpr int64_t kDefaultStartMs = 12LL * 3600 * 1000;   // 12:00
    constexpr uint32_t kDefaultTransMs = 45000;        // 45 s

    // ---- weathers (wire-stable indices: WorldClock carries the index, never the name) -------------------------------------------------
    // The names SET_WEATHER_TYPE_NOW_PERSIST takes (alloc8or MISC:: doc, "the following weatherTypes are used in the scripts"); the hash is
    // GET_HASH_KEY of the name, what SET_CURR_WEATHER_STATE and GET_CURR_WEATHER_STATE use (the doc lists 12 of them; the test pins those).
    struct WeatherDef { const char* name; uint32_t hash; };
    constexpr WeatherDef kWeathers[] = {
        {"EXTRASUNNY", Joaat("EXTRASUNNY")},   // 0
        {"CLEAR", Joaat("CLEAR")},             // 1
        {"CLOUDS", Joaat("CLOUDS")},           // 2
        {"SMOG", Joaat("SMOG")},               // 3
        {"FOGGY", Joaat("FOGGY")},             // 4
        {"OVERCAST", Joaat("OVERCAST")},       // 5
        {"RAIN", Joaat("RAIN")},               // 6
        {"THUNDER", Joaat("THUNDER")},         // 7
        {"CLEARING", Joaat("CLEARING")},       // 8
        {"NEUTRAL", Joaat("NEUTRAL")},         // 9
        {"SNOW", Joaat("SNOW")},               // 10
        {"BLIZZARD", Joaat("BLIZZARD")},       // 11
        {"SNOWLIGHT", Joaat("SNOWLIGHT")},     // 12
        {"XMAS", Joaat("XMAS")},               // 13
        {"HALLOWEEN", Joaat("HALLOWEEN")},     // 14
    };
    constexpr uint8_t kWeatherCount = (uint8_t)(sizeof(kWeathers) / sizeof(kWeathers[0]));
    enum : uint8_t { kExtraSunny = 0, kClear = 1, kClouds = 2, kSmog = 3, kFoggy = 4, kOvercast = 5, kRain = 6, kThunder = 7, kClearing = 8 };

    inline bool ValidWeather(uint8_t i) { return i < kWeatherCount; }
    inline const char* WeatherName(uint8_t i) { return ValidWeather(i) ? kWeathers[i].name : "?"; }
    inline uint32_t WeatherHash(uint8_t i) { return ValidWeather(i) ? kWeathers[i].hash : 0u; }
    // The index of a weather name (any case), -1 when unknown.
    inline int FindWeather(const std::string& name)
    {
        for (uint8_t i = 0; i < kWeatherCount; i++)
        {
            const char* n = kWeathers[i].name;
            size_t k = 0;
            for (; n[k] && k < name.size(); k++)
            {
                char c = name[k];
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                if (c != n[k]) break;
            }
            if (!n[k] && k == name.size()) return i;
        }
        return -1;
    }
    // The weather a GET_CURR_WEATHER_STATE hash stands for, -1 for none of ours.
    inline int WeatherOfHash(uint32_t h)
    {
        for (uint8_t i = 0; i < kWeatherCount; i++)
            if (kWeathers[i].hash == h) return i;
        return -1;
    }

    // One entry of a weather cycle: a weather for `minutes` REAL minutes (the transition into it is the first transMs of them).
    struct CycleEntry { uint8_t weather = 0; uint32_t minutes = 0; };
    // The shipped cycle: the everyday sky (sun, clear, clouds, overcast, a short rain and its clearing), 90 real minutes a lap. No snow, xmas,
    // halloween, blizzard or thunder (a server opts into them with server.json weatherCycle).
    inline std::vector<CycleEntry> DefaultCycle()
    {
        return {{kExtraSunny, 20}, {kClear, 15}, {kClouds, 12}, {kOvercast, 8}, {kRain, 6}, {kClearing, 6}, {kClouds, 10}, {kClear, 13}};
    }

    // ---- time of day ------------------------------------------------------------------------------------------------------------------
    inline int64_t WrapDay(int64_t ms)
    {
        ms %= kDayMs;
        return ms < 0 ? ms + kDayMs : ms;
    }
    struct Hms { int h = 0, m = 0, s = 0; };
    inline Hms ToHms(int64_t gameMs)
    {
        const int64_t sec = WrapDay(gameMs) / 1000;
        Hms t;
        t.h = (int)(sec / 3600);
        t.m = (int)(sec / 60 % 60);
        t.s = (int)(sec % 60);
        return t;
    }
    inline int SecOfDay(const Hms& t) { return t.h * 3600 + t.m * 60 + t.s; }
    // a - b in seconds, wrapped to [-43200, 43200): 23:59:59 against 00:00:01 is -2, not 86398.
    inline int DiffSec(const Hms& a, const Hms& b)
    {
        int d = (SecOfDay(a) - SecOfDay(b)) % 86400;
        if (d < -43200) d += 86400;
        if (d >= 43200) d -= 86400;
        return d;
    }
    inline std::string FormatHms(int64_t gameMs)
    {
        const Hms t = ToHms(gameMs);
        char b[16];
        std::snprintf(b, sizeof b, "%02d:%02d:%02d", t.h, t.m, t.s);
        return b;
    }
    inline std::string FormatScale(uint32_t scaleMilli)
    {
        char b[24];
        std::snprintf(b, sizeof b, "%u.%03u", scaleMilli / 1000, scaleMilli % 1000);
        return b;
    }
    // "H:MM", "HH:MM" or "HH:MM:SS" (00..23, 00..59, 00..59) -> game ms of day. False for anything else.
    inline bool ParseClockTime(const std::string& s, int64_t& gameMs)
    {
        int part[3] = {0, 0, 0};
        int parts = 0, digits = 0;
        for (size_t i = 0; i <= s.size(); i++)
        {
            const char c = i < s.size() ? s[i] : ':';
            if (c >= '0' && c <= '9')
            {
                if (++digits > 2) return false;
                part[parts] = part[parts] * 10 + (c - '0');
            }
            else if (c == ':')
            {
                if (digits == 0 || (parts > 0 && digits != 2)) return false;   // minutes and seconds are two digits
                digits = 0;
                if (++parts > 3) return false;
            }
            else
                return false;
        }
        if (parts < 2 || part[0] > 23 || part[1] > 59 || part[2] > 59) return false;
        gameMs = ((int64_t)part[0] * 3600 + part[1] * 60 + part[2]) * 1000;
        return true;
    }
    // A time scale as typed ("30", "0", "1.5", "60.25") -> thousandths, 0..kMaxScaleMilli. False for anything else.
    inline bool ParseScale(const std::string& s, uint32_t& milli)
    {
        if (s.empty() || s.size() > 16) return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || c == '.')) return false;
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(v) || v < 0.0 || v * 1000.0 > (double)kMaxScaleMilli + 0.5) return false;
        milli = (uint32_t)std::llround(v * 1000.0);
        return true;
    }

    // The clock line (server side; the client runs the same line from a Received).
    struct Clock
    {
        int64_t anchorMs = 0;                 // a real (monotonic) ms ...
        int64_t anchorGameMs = kDefaultStartMs; // ... and the game ms of day at it
        uint32_t scaleMilli = kGtaScaleMilli;

        int64_t GameMsAt(int64_t nowMs) const
        {
            const double run = (double)(nowMs - anchorMs) * (double)scaleMilli / 1000.0;
            return WrapDay(anchorGameMs + (int64_t)std::floor(run));
        }
        void SetTime(int64_t nowMs, int64_t gameMs)
        {
            anchorMs = nowMs;
            anchorGameMs = WrapDay(gameMs);
        }
        // A new rate from now on: the time shown now does not move.
        void SetScale(int64_t nowMs, uint32_t milli)
        {
            anchorGameMs = GameMsAt(nowMs);
            anchorMs = nowMs;
            scaleMilli = milli;
        }
    };

    // ---- weather ----------------------------------------------------------------------------------------------------------------------
    // What the sky is doing at one moment: steady on `to` (totalMs 0, or elapsed >= total), or a blend from `from` to `to`.
    struct WeatherView
    {
        uint8_t from = kClear, to = kClear;
        uint32_t elapsedMs = 0, totalMs = 0;
        bool Steady() const { return totalMs == 0 || elapsedMs >= totalMs || from == to; }
        float Progress() const { return Steady() ? 1.0f : (float)elapsedMs / (float)totalMs; }
        // The one weather a new transition starts from (the blend's dominant side).
        uint8_t Shown() const { return Steady() || Progress() >= 0.5f ? to : from; }
        bool SamePhase(const WeatherView& o) const
        {
            return Steady() == o.Steady() && to == o.to && (Steady() || from == o.from);
        }
    };

    class WeatherPlan
    {
    public:
        // fixed >= 0: that weather, steady from the start; fixed < 0: the cycle from its first entry (no transition at the start).
        void Configure(int fixed, const std::vector<CycleEntry>& cycle, uint32_t transMs, int64_t nowMs)
        {
            list_ = cycle.empty() ? DefaultCycle() : cycle;
            transMs_ = transMs > kMaxTransMs ? kMaxTransMs : transMs;
            sinceMs_ = nowMs;
            if (fixed >= 0 && ValidWeather((uint8_t)fixed))
            {
                cycle_ = false;
                fixed_ = fixedFrom_ = (uint8_t)fixed;
            }
            else
            {
                cycle_ = true;
                startFrom_ = list_[0].weather;
            }
        }
        // An admin's fixed weather: a transition from what is shown now.
        void SetFixed(int64_t nowMs, uint8_t w)
        {
            const uint8_t from = At(nowMs).Shown();
            cycle_ = false;
            fixed_ = w;
            fixedFrom_ = from;
            sinceMs_ = nowMs;
        }
        // Back to the cycle, from its first entry, with a transition from what is shown now.
        void SetCycle(int64_t nowMs)
        {
            startFrom_ = At(nowMs).Shown();
            cycle_ = true;
            sinceMs_ = nowMs;
        }
        bool Cycling() const { return cycle_; }
        uint8_t Fixed() const { return fixed_; }
        uint32_t TransMs() const { return transMs_; }
        const std::vector<CycleEntry>& Cycle() const { return list_; }
        int64_t CycleLapMs() const
        {
            int64_t total = 0;
            for (const CycleEntry& e : list_) total += (int64_t)e.minutes * 60000;
            return total;
        }

        WeatherView At(int64_t nowMs) const
        {
            WeatherView v;
            const int64_t pos = nowMs > sinceMs_ ? nowMs - sinceMs_ : 0;
            if (!cycle_)
            {
                v.from = v.to = fixed_;
                if (fixedFrom_ != fixed_ && transMs_ > 0 && pos < (int64_t)transMs_)
                {
                    v.from = fixedFrom_;
                    v.elapsedMs = (uint32_t)pos;
                    v.totalMs = transMs_;
                }
                return v;
            }
            const int64_t lap = CycleLapMs();
            if (lap <= 0)
            {
                v.from = v.to = kClear;
                return v;
            }
            const int64_t laps = pos / lap;
            int64_t within = pos % lap;
            for (size_t i = 0; i < list_.size(); i++)
            {
                const int64_t len = (int64_t)list_[i].minutes * 60000;
                if (within >= len && i + 1 < list_.size())
                {
                    within -= len;
                    continue;
                }
                const uint8_t w = list_[i].weather;
                const uint8_t prev = i > 0 ? list_[i - 1].weather : (laps == 0 ? startFrom_ : list_.back().weather);
                const int64_t trans = (int64_t)transMs_ < len ? (int64_t)transMs_ : len;
                v.from = v.to = w;
                if (prev != w && within < trans)
                {
                    v.from = prev;
                    v.elapsedMs = (uint32_t)within;
                    v.totalMs = (uint32_t)trans;
                }
                return v;
            }
            return v;
        }

    private:
        bool cycle_ = true;
        uint8_t fixed_ = kClear, fixedFrom_ = kClear, startFrom_ = kExtraSunny;
        int64_t sinceMs_ = 0;            // fixed: when the transition began; cycle: when entry 0 of the first lap began
        uint32_t transMs_ = kDefaultTransMs;
        std::vector<CycleEntry> list_ = DefaultCycle();
    };

    // ---- the client's copy ------------------------------------------------------------------------------------------------------------
    // One WorldClock as the client keeps it: the server's moment placed on the client's monotonic clock.
    struct Received
    {
        bool have = false;
        int64_t atLocalMs = 0;      // the local ms the server's `gameMs` stands for (receive time - half the round trip)
        int64_t gameMs = 0;
        uint32_t scaleMilli = kGtaScaleMilli;
        WeatherView weather;        // as of atLocalMs
        uint16_t rev = 0;
        uint8_t flags = 0;
    };
    inline Received FromWire(uint32_t gameMs, uint32_t scaleMilli, uint8_t from, uint8_t to, uint32_t elapsedMs, uint32_t totalMs, uint16_t rev,
                             uint8_t flags, int64_t recvLocalMs, uint32_t rttMs)
    {
        Received r;
        r.have = true;
        r.atLocalMs = recvLocalMs - (int64_t)(rttMs / 2);
        r.gameMs = WrapDay(gameMs);
        r.scaleMilli = scaleMilli;
        r.weather.from = from;
        r.weather.to = to;
        r.weather.elapsedMs = elapsedMs;
        r.weather.totalMs = totalMs;
        r.rev = rev;
        r.flags = flags;
        return r;
    }
    inline int64_t GameMsAt(const Received& r, int64_t localMs)
    {
        const double run = (double)(localMs - r.atLocalMs) * (double)r.scaleMilli / 1000.0;
        return WrapDay(r.gameMs + (int64_t)std::floor(run));
    }
    inline WeatherView WeatherAt(const Received& r, int64_t localMs)
    {
        WeatherView v = r.weather;
        if (v.Steady())
        {
            v.from = v.to;
            v.elapsedMs = v.totalMs = 0;
            return v;
        }
        const int64_t el = (int64_t)v.elapsedMs + (localMs > r.atLocalMs ? localMs - r.atLocalMs : 0);
        if (el >= (int64_t)v.totalMs)
        {
            v.from = v.to;
            v.elapsedMs = v.totalMs = 0;
        }
        else
            v.elapsedMs = (uint32_t)el;
        return v;
    }

    // ---- the client's holds: which natives to call this tick -----------------------------------------------------------------------
    // The clock: PAUSE_CLOCK(true) (so the game's own clock never runs between our sets: one source of time) and SET_CLOCK_TIME whenever the
    // target's whole second changes (the game clock has whole seconds; at the default rate that is up to 30 calls a real second, the cost of one
    // native each). Every kReadBackMs the clock is read back BEFORE this tick's set: a paused clock must still show the last second we set;
    // more than kTolSec off is a mismatch (something unpaused or set it): counted, and paused + set again at once. Re-paused every kRepauseMs.
    class ClockHold
    {
    public:
        static constexpr int64_t kReadBackMs = 1000, kRepauseMs = 5000;
        static constexpr int kTolSec = 2;
        struct Plan { bool readBack = false, pause = false, set = false; Hms hms; };

        Plan Next(int64_t nowMs, const Hms& target) const
        {
            Plan p;
            p.hms = target;
            if (!holding_)
            {
                p.pause = p.set = true;
                return p;
            }
            p.readBack = nowMs - lastReadMs_ >= kReadBackMs;
            p.pause = nowMs - lastPauseMs_ >= kRepauseMs;
            p.set = SecOfDay(target) != SecOfDay(lastSet_);
            return p;
        }
        // The read-back of a plan that asked for one (ok = the three natives answered). True = a mismatch: pause and set this tick.
        bool OnReadBack(int64_t nowMs, bool ok, const Hms& got)
        {
            lastReadMs_ = nowMs;
            if (!ok) return false;
            lastDiffSec_ = DiffSec(got, lastSet_);
            if (lastDiffSec_ > kTolSec || lastDiffSec_ < -kTolSec)
            {
                ++mismatches_;
                return true;
            }
            return false;
        }
        void Done(int64_t nowMs, const Plan& p, bool pauseOk, bool setOk)
        {
            if (!holding_) lastReadMs_ = nowMs;   // the first read-back a second after the take-over
            holding_ = true;
            if (p.pause && pauseOk) lastPauseMs_ = nowMs;
            if (p.set && setOk) lastSet_ = p.hms;
        }
        void Release() { holding_ = false; }
        bool Holding() const { return holding_; }
        const Hms& LastSet() const { return lastSet_; }
        int LastDiffSec() const { return lastDiffSec_; }
        uint32_t Mismatches() const { return mismatches_; }

    private:
        bool holding_ = false;
        Hms lastSet_;
        int64_t lastReadMs_ = 0, lastPauseMs_ = 0;
        int lastDiffSec_ = 0;
        uint32_t mismatches_ = 0;
    };

    // The weather: steady -> SET_WEATHER_TYPE_NOW_PERSIST(name) once (persist: the game's own cycle stops on it); a transition ->
    // SET_CURR_WEATHER_STATE(fromHash, toHash, progress) whenever the pair changes, the progress moved by kMixStep, or kMixEveryMs passed
    // (the game advances its own blend between our calls; every machine is pulled back to the server's progress). Every kCheckMs the state is
    // read back (GET_CURR_WEATHER_STATE) BEFORE this tick's call and compared with what was last issued (Matches): a mismatch is counted and
    // issued again at once.
    class WeatherHold
    {
    public:
        static constexpr int64_t kMixEveryMs = 500, kCheckMs = 2000;
        static constexpr float kMixStep = 0.01f, kMixTol = 0.15f;
        enum Kind : uint8_t { None = 0, Persist = 1, Mix = 2 };
        struct Plan { Kind kind = None; uint8_t a = 0, b = 0; float pct = 0.0f; bool readBack = false; };

        Plan Next(int64_t nowMs, const WeatherView& v) const
        {
            Plan p;
            p.readBack = have_ && nowMs - lastCheckMs_ >= kCheckMs;
            if (v.Steady())
            {
                if (!have_ || kind_ != Persist || a_ != v.to)
                {
                    p.kind = Persist;
                    p.a = p.b = v.to;
                    p.pct = 1.0f;
                }
                return p;
            }
            const float pct = v.Progress();
            const float step = pct - pct_;
            if (!have_ || kind_ != Mix || a_ != v.from || b_ != v.to || step >= kMixStep || step <= -kMixStep || nowMs - lastIssueMs_ >= kMixEveryMs)
            {
                p.kind = Mix;
                p.a = v.from;
                p.b = v.to;
                p.pct = pct;
            }
            return p;
        }
        // Does a GET_CURR_WEATHER_STATE (h1, h2, pct2) show what was last issued?
        bool Matches(uint32_t h1, uint32_t h2, float pct2) const
        {
            if (kind_ == Persist)
            {
                const uint32_t w = WeatherHash(a_);
                return (h1 == w && h2 == w) || (h2 == w && pct2 >= 0.99f) || (h1 == w && pct2 <= 0.01f);
            }
            if (kind_ == Mix)
            {
                const float d = pct2 - pct_;
                return h1 == WeatherHash(a_) && h2 == WeatherHash(b_) && d <= kMixTol && d >= -kMixTol;
            }
            return true;
        }
        // The read-back of a plan that asked for one. True = a mismatch: the caller issues the current target this tick (Force).
        bool OnReadBack(int64_t nowMs, bool ok, uint32_t h1, uint32_t h2, float pct2)
        {
            lastCheckMs_ = nowMs;
            if (!ok || Matches(h1, h2, pct2)) return false;
            ++mismatches_;
            return true;
        }
        // The plan for `v` with the call forced (after a mismatch).
        Plan Force(const WeatherView& v) const
        {
            Plan p;
            if (v.Steady())
            {
                p.kind = Persist;
                p.a = p.b = v.to;
                p.pct = 1.0f;
            }
            else
            {
                p.kind = Mix;
                p.a = v.from;
                p.b = v.to;
                p.pct = v.Progress();
            }
            return p;
        }
        void Done(int64_t nowMs, const Plan& p, bool ok)
        {
            if (p.kind == None || !ok) return;
            if (!have_) lastCheckMs_ = nowMs;
            have_ = true;
            kind_ = p.kind;
            a_ = p.a;
            b_ = p.b;
            pct_ = p.pct;
            lastIssueMs_ = nowMs;
        }
        void Release() { have_ = false; kind_ = None; }
        bool Holding() const { return have_; }
        uint32_t Mismatches() const { return mismatches_; }

    private:
        bool have_ = false;
        Kind kind_ = None;
        uint8_t a_ = 0, b_ = 0;
        float pct_ = 0.0f;
        int64_t lastIssueMs_ = 0, lastCheckMs_ = 0;
        uint32_t mismatches_ = 0;
    };

}
