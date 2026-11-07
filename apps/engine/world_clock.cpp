// world_clock: 0.5.7, the server's time of day and weather on this machine. The design is in world_clock.h and libs/sync/world_clock.h.
#include "world_clock.h"

#include "hook.h"
#include "common/logger.h"
#include "common/registry.h"
#include "sync/world_clock.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace gage::game::worldclock {

    namespace wc = gage::sync::wclock;

    namespace {

        int64_t SteadyMs()
        {
            return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        std::atomic<bool> g_on{true};          // the world_clock switch (read at every Welcome)

        // Join worker -> script tick.
        std::mutex g_mtx;
        wc::Received g_recv;                   // the latest WorldClock, placed on the steady clock (have = false: none this session)
        bool g_releaseAsked = false;           // the session ended: release what is held
        uint32_t g_received = 0;               // WorldClocks this 10 s window
        uint32_t g_revChanges = 0;             // received with a rev other than the previous one (admin changes) this window

        // Script tick only.
        wc::ClockHold g_clock;
        wc::WeatherHold g_weather;
        int64_t g_windowAt = 0;
        struct Window {
            uint32_t pauses = 0, sets = 0, clockReads = 0, clockForced = 0;
            uint32_t persists = 0, mixes = 0, weatherReads = 0, weatherForced = 0;
            uint32_t failed = 0, unknownWeather = 0, releases = 0;
        } g_w;
        uint32_t g_clockMisTotal = 0, g_weatherMisTotal = 0;   // mismatches since the switch line
        bool g_haveRead = false;
        wc::Hms g_lastRead;
        uint32_t g_lastH1 = 0, g_lastH2 = 0;
        float g_lastPct = 0.0f;
        bool g_haveWeatherRead = false;

        const char* NameOfHash(uint32_t h)
        {
            const int i = wc::WeatherOfHash(h);
            return i >= 0 ? wc::WeatherName((uint8_t)i) : "other";
        }

        void Release(World& w, const char* why)
        {
            const bool clock = g_clock.Holding(), weather = g_weather.Holding();
            if (!clock && !weather) return;
            bool okPause = true, okClear = true;
            if (clock) okPause = w.ClockPause(false);
            if (weather) okClear = w.WeatherClearPersist();
            g_clock.Release();
            g_weather.Release();
            g_w.releases++;
            GLOGI("engine: world clock (0.5.7): released (%s): PAUSE_CLOCK(false) %s, CLEAR_WEATHER_TYPE_PERSIST %s; the game's own clock and weather "
                  "run again", why, !clock ? "not needed" : okPause ? "ok" : "FAILED", !weather ? "not needed" : okClear ? "ok" : "FAILED");
        }

        void LogWindow(int64_t now, const wc::Received& r, uint32_t received, uint32_t revChanges)
        {
            const bool active = r.have || g_clock.Holding() || g_weather.Holding() || received || g_w.releases;
            if (!active) return;
            char clockPart[256], weatherPart[320];
            if (r.have)
            {
                const wc::WeatherView v = wc::WeatherAt(r, now);
                const std::string target = wc::FormatHms(wc::GameMsAt(r, now));
                const wc::Hms ls = g_clock.LastSet();
                std::snprintf(clockPart, sizeof clockPart,
                              "server rev %u (%u received, %u changes), target %s at x%s, held %02d:%02d:%02d; clock: %u pauses, %u sets, %u read-backs "
                              "(last %02d:%02d:%02d, diff %+d s), %u mismatches (%u in total)",
                              (unsigned)r.rev, received, revChanges, target.c_str(), wc::FormatScale(r.scaleMilli).c_str(), ls.h, ls.m, ls.s, g_w.pauses,
                              g_w.sets, g_w.clockReads, g_haveRead ? g_lastRead.h : -1, g_haveRead ? g_lastRead.m : -1, g_haveRead ? g_lastRead.s : -1,
                              g_clock.LastDiffSec(), g_w.clockForced, g_clockMisTotal);
                if (v.Steady())
                    std::snprintf(weatherPart, sizeof weatherPart, "weather %s (%s)", wc::WeatherName(v.to), (r.flags & proto::kClockCycle) ? "cycle" : "fixed");
                else
                    std::snprintf(weatherPart, sizeof weatherPart, "weather %s -> %s %d%% (%s)", wc::WeatherName(v.from), wc::WeatherName(v.to),
                                  (int)(v.Progress() * 100.0f), (r.flags & proto::kClockCycle) ? "cycle" : "fixed");
            }
            else
            {
                std::snprintf(clockPart, sizeof clockPart, "no server clock (none received since the last Welcome, or the session ended)");
                weatherPart[0] = '\0';
            }
            char tail[320];
            std::snprintf(tail, sizeof tail, "%s%s: %u persist, %u mix calls, %u read-backs (last %s -> %s %d%%), %u mismatches (%u in total); "
                                             "natives failed %u, unknown weather ids %u, releases %u",
                          weatherPart, weatherPart[0] ? "" : "weather", g_w.persists, g_w.mixes, g_w.weatherReads,
                          g_haveWeatherRead ? NameOfHash(g_lastH1) : "-", g_haveWeatherRead ? NameOfHash(g_lastH2) : "-", (int)(g_lastPct * 100.0f),
                          g_w.weatherForced, g_weatherMisTotal, g_w.failed, g_w.unknownWeather, g_w.releases);
            GLOGI("engine: world clock (0.5.7), last 10 s: %s; %s", clockPart, tail);
        }

        void HoldClock(World& w, int64_t now, const wc::Received& r)
        {
            const wc::Hms target = wc::ToHms(wc::GameMsAt(r, now));
            wc::ClockHold::Plan p = g_clock.Next(now, target);
            if (p.readBack)
            {
                int h = 0, m = 0, s = 0;
                const bool ok = w.ClockGet(h, m, s) && h >= 0 && h < 24 && m >= 0 && m < 60 && s >= 0 && s < 60;
                g_w.clockReads++;
                if (!ok) g_w.failed++;
                wc::Hms got;
                got.h = h;
                got.m = m;
                got.s = s;
                if (ok)
                {
                    g_lastRead = got;
                    g_haveRead = true;
                }
                if (g_clock.OnReadBack(now, ok, got))
                {
                    g_w.clockForced++;
                    g_clockMisTotal++;
                    p.pause = p.set = true;   // something unpaused or moved the clock: take it back this tick
                }
            }
            bool pauseOk = true, setOk = true;
            if (p.pause)
            {
                pauseOk = w.ClockPause(true);
                g_w.pauses++;
                if (!pauseOk) g_w.failed++;
            }
            if (p.set)
            {
                setOk = w.ClockSet(p.hms.h, p.hms.m, p.hms.s);
                g_w.sets++;
                if (!setOk) g_w.failed++;
            }
            g_clock.Done(now, p, pauseOk, setOk);
        }

        void HoldWeather(World& w, int64_t now, const wc::Received& r)
        {
            const wc::WeatherView v = wc::WeatherAt(r, now);
            if (!wc::ValidWeather(v.to) || (!v.Steady() && !wc::ValidWeather(v.from)))
            {
                g_w.unknownWeather++;   // a newer server's weather we have no name for: keep what is shown
                return;
            }
            wc::WeatherHold::Plan p = g_weather.Next(now, v);
            if (p.readBack)
            {
                uint32_t h1 = 0, h2 = 0;
                float pct = 0.0f;
                const bool ok = w.WeatherGet(h1, h2, pct);
                g_w.weatherReads++;
                if (!ok) g_w.failed++;
                else
                {
                    g_lastH1 = h1;
                    g_lastH2 = h2;
                    g_lastPct = pct;
                    g_haveWeatherRead = true;
                }
                if (g_weather.OnReadBack(now, ok, h1, h2, pct))
                {
                    g_w.weatherForced++;
                    g_weatherMisTotal++;
                    p = g_weather.Force(v);
                }
            }
            if (p.kind == wc::WeatherHold::None) return;
            bool ok = false;
            if (p.kind == wc::WeatherHold::Persist)
            {
                ok = w.WeatherNowPersist(wc::WeatherName(p.a));
                g_w.persists++;
            }
            else
            {
                ok = w.WeatherMix(wc::WeatherHash(p.a), wc::WeatherHash(p.b), p.pct);
                g_w.mixes++;
            }
            if (!ok) g_w.failed++;
            g_weather.Done(now, p, ok);
        }

    }  // namespace

    void ReadSwitch()
    {
        const bool on = reg::Get(L"world_clock") != L"0";
        g_on.store(on);
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_recv = wc::Received();
        }
        GLOGI("engine: world clock (0.5.7): world_clock=%s", on ? "on (the server's time of day and weather: PAUSE_CLOCK + SET_CLOCK_TIME, "
                                                                 "SET_WEATHER_TYPE_NOW_PERSIST / SET_CURR_WEATHER_STATE, read back; LIVE-UNVERIFIED)"
                                                               : "OFF (0.5.6: the server's clock is ignored, the game keeps its own)");
    }

    void OnClock(const proto::WorldClock& m, uint32_t rttMs)
    {
        if (!g_on.load()) return;
        const wc::Received r = wc::FromWire(m.gameMs, m.scaleMilli, m.weatherFrom, m.weatherTo, m.transElapsedMs, m.transTotalMs, m.rev, m.flags,
                                            SteadyMs(), rttMs);
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_recv.have && g_recv.rev != m.rev) g_revChanges++;
        g_recv = r;
        g_releaseAsked = false;
        g_received++;
    }

    void OnEnded()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_recv = wc::Received();
        g_releaseAsked = true;
    }

    void Tick(World& w)
    {
        const int64_t now = SteadyMs();
        wc::Received r;
        bool release = false;
        uint32_t received = 0, revChanges = 0;
        const bool windowDue = g_windowAt == 0 || now - g_windowAt >= 10000;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            r = g_recv;
            release = g_releaseAsked;
            g_releaseAsked = false;
            if (windowDue)
            {
                received = g_received;
                revChanges = g_revChanges;
                g_received = g_revChanges = 0;
            }
        }
        if (release || !g_on.load() || !r.have)
            Release(w, release ? "the session ended" : !g_on.load() ? "world_clock=0" : "no server clock");
        else
        {
            HoldClock(w, now, r);
            HoldWeather(w, now, r);
        }
        if (windowDue)
        {
            if (g_windowAt != 0) LogWindow(now, r, received, revChanges);
            g_windowAt = now;
            g_w = Window();
        }
    }

}  // namespace gage::game::worldclock
