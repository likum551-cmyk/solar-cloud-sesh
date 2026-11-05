// 0.4.22 minimap (libs/sync/minimap.h): when GTA's radar is shown, how its health / armour bars are hidden, where the radar is on screen and
// where our vitals plate docks under it. Included by tests/gameplay_test.cpp after its Check(); run by minimaptest::RunPure(), kChecks checks.
// Every check below was run against a mutant of the rule it pins (the list is in the 0.4.22 report); a check that survives its mutant is a bug here.
#pragma once

#include "sync/minimap.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace minimaptest {

    constexpr int kChecks = 28;

    // The engine's scale (overlay_ui.cpp: S(v) = round(v * scale), scale = displayH / 1080 clamped to 0.75..3).
    inline float UiScale(float displayH) { return std::fmin(3.0f, std::fmax(0.75f, displayH / 1080.0f)); }
    inline float S(float v, float displayH) { return std::floor(v * UiScale(displayH) + 0.5f); }

    inline std::vector<gage::sync::BarsAction> Drive(gage::sync::BarsGate& g, int ticks, double& t, const gage::sync::BarsInputs& in, double dt = 16.0)
    {
        std::vector<gage::sync::BarsAction> out;
        for (int i = 0; i < ticks; ++i)
        {
            out.push_back(g.Next(in, t));
            t += dt;
        }
        return out;
    }

    inline int Count(const std::vector<gage::sync::BarsAction>& v, gage::sync::BarsAction a)
    {
        int n = 0;
        for (const gage::sync::BarsAction x : v)
            n += x == a ? 1 : 0;
        return n;
    }

    inline std::string Names(const std::vector<gage::sync::BarsAction>& v, size_t maxN = 8)
    {
        std::string s;
        for (size_t i = 0; i < v.size() && i < maxN; ++i)
            s += std::string(i ? "," : "") + gage::sync::BarsActionName(v[i]);
        return s;
    }

    inline bool Near(float a, float b, float tol = 0.5f) { return std::fabs(a - b) <= tol; }

    inline void RunPure()
    {
        using namespace gage::sync;
        std::printf("-- 0.4.22 minimap: radar decision, bars gate, radar rectangle, vitals dock\n");

        // 1-4. RadarWanted: each of the three conditions alone decides.
        RadarInputs ok;
        ok.switchOn = true;
        ok.entryDone = true;
        ok.pauseMenuActive = false;
        Check("minimap: the radar is wanted once the entry is done, the switch is on and the pause menu is closed", RadarWanted(ok));
        {
            RadarInputs i = ok;
            i.switchOn = false;
            Check("minimap: minimap=0 (switch off) never shows the radar", !RadarWanted(i));
        }
        {
            RadarInputs i = ok;
            i.entryDone = false;
            Check("minimap: no radar before the entry is done (loading screen, or a session that ended)", !RadarWanted(i));
        }
        {
            RadarInputs i = ok;
            i.pauseMenuActive = true;
            Check("minimap: no radar while the pause menu is up", !RadarWanted(i));
        }

        // 5. the minimap_bars switch.
        Check("minimap: minimap_bars: unset / 1 / junk = setup+refresh, 0 = off, 2 = setup only",
              ParseBarsMode(L"") == BarsMode::SetupRefresh && ParseBarsMode(L"1") == BarsMode::SetupRefresh && ParseBarsMode(L"x") == BarsMode::SetupRefresh
                  && ParseBarsMode(L"0") == BarsMode::Off && ParseBarsMode(L"2") == BarsMode::Setup);

        BarsInputs ready;
        ready.radarOn = true;
        ready.haveHandle = true;
        ready.loaded = true;

        // 6. Off issues nothing, however ready the movie is.
        {
            BarsGate g;
            g.SetMode(BarsMode::Off);
            double t = 0;
            const auto v = Drive(g, 60, t, ready);
            Check("minimap: bars off: nothing is ever issued", Count(v, BarsAction::None) == 60, Names(v));
        }
        // 7. no radar, no bars work (not even a movie request).
        {
            BarsGate g;
            BarsInputs i = ready;
            i.radarOn = false;
            i.haveHandle = false;
            double t = 0;
            const auto v = Drive(g, 60, t, i);
            Check("minimap: radar not shown: no request, no refresh, no setup", Count(v, BarsAction::None) == 60 && g.Counters().requests == 0, Names(v));
        }
        // 8. a missing / unloaded movie is requested at most once a second.
        {
            BarsGate g;
            BarsInputs none = ready;
            none.haveHandle = false;
            none.loaded = false;
            BarsInputs notLoaded = ready;
            notLoaded.loaded = false;
            const BarsAction a0 = g.Next(none, 0.0), a1 = g.Next(none, 500.0), a2 = g.Next(notLoaded, 999.0), a3 = g.Next(notLoaded, 1000.0);
            const BarsAction a4 = g.Next(ready, 1016.0);
            Check("minimap: a missing or unloaded movie is requested once a second, never set up",
                  a0 == BarsAction::Request && a1 == BarsAction::None && a2 == BarsAction::None && a3 == BarsAction::Request && a4 != BarsAction::Setup
                      && g.Counters().requests == 2,
                  std::string(BarsActionName(a0)) + "," + BarsActionName(a1) + "," + BarsActionName(a2) + "," + BarsActionName(a3) + "," + BarsActionName(a4));
        }
        // 9. the default sequence: one flip, then the setup call every tick.
        {
            BarsGate g;
            double t = 0;
            const auto v = Drive(g, 100, t, ready);
            Check("minimap: setup+refresh: big radar on, big radar off, then SETUP_HEALTH_ARMOUR every tick (one flip only)",
                  v[0] == BarsAction::RefreshOn && v[1] == BarsAction::RefreshOff && Count(v, BarsAction::RefreshOn) == 1
                      && Count(v, BarsAction::RefreshOff) == 1 && Count(v, BarsAction::Setup) == 98 && !g.BigmapOn(),
                  Names(v));
        }
        // 10. minimap_bars=2: no flip.
        {
            BarsGate g;
            g.SetMode(BarsMode::Setup);
            double t = 0;
            const auto v = Drive(g, 20, t, ready);
            Check("minimap: bars setup-only: the call from the first ready tick, never a big-radar flip",
                  Count(v, BarsAction::Setup) == 20 && g.Counters().refreshOn == 0 && g.Counters().refreshOff == 0, Names(v));
        }
        // 11. fail-safe: the flip's OFF is issued even when the radar went away between the two halves.
        {
            BarsGate g;
            BarsInputs gone = ready;
            gone.radarOn = false;
            const BarsAction on = g.Next(ready, 0.0);
            const bool wasOn = g.BigmapOn();
            const BarsAction off = g.Next(gone, 16.0);   // the pause menu opened on this tick
            Check("minimap: a big radar switched on is switched off on the next call even when the radar is no longer wanted",
                  on == BarsAction::RefreshOn && wasOn && off == BarsAction::RefreshOff && !g.BigmapOn());
        }
        // 12. fail-safe across a new session.
        {
            BarsGate g;
            g.Next(ready, 0.0);
            g.Reset();                                   // a Welcome arrives between the two halves
            const BarsAction off = g.Next(BarsInputs(), 16.0);
            Check("minimap: Reset between the two halves of the flip still owes the OFF", off == BarsAction::RefreshOff && !g.BigmapOn());
        }
        // 13. every new entry flips once more.
        {
            BarsGate g;
            double t = 0;
            Drive(g, 10, t, ready);
            g.Reset();
            Drive(g, 10, t, ready);
            Check("minimap: a new entry (Reset) re-arms the flip: two entries, two flips", g.Counters().refreshOn == 2 && g.Counters().refreshOff == 2);
        }
        // 14. the pause menu: setup stops and resumes without another flip.
        {
            BarsGate g;
            BarsInputs paused = ready;
            paused.radarOn = false;
            double t = 0;
            Drive(g, 10, t, ready);
            const auto during = Drive(g, 10, t, paused);
            const auto after = Drive(g, 10, t, ready);
            Check("minimap: no setup call while the radar is hidden; it resumes after, with no second flip",
                  Count(during, BarsAction::None) == 10 && Count(after, BarsAction::Setup) == 10 && g.Counters().refreshOn == 1, Names(after));
        }

        // 15-18. where the radar is.
        {
            const ScreenRect r = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f);
            Check("minimap: radar rectangle at 1920x1080, safe zone 0.9: 96 px in from the left, 54 px up, 270 x 190 px",
                  Near(r.x0, 96.0f) && Near(r.x1, 366.0f) && Near(r.y1, 1026.0f) && Near(r.y1 - r.y0, 190.3f, 0.6f),
                  std::to_string(r.x0) + "," + std::to_string(r.y0) + "," + std::to_string(r.x1) + "," + std::to_string(r.y1));
        }
        {
            const ScreenRect a = RadarKeepOut(1920, 1080, 1.0f, true, 0.0f), b = RadarKeepOut(1920, 1080, 0.95f, true, 0.0f);
            Check("minimap: safe zone 1.0 puts the radar in the corner, 0.95 halfway between that and 0.9",
                  Near(a.x0, 0.0f) && Near(a.y1, 1080.0f) && Near(b.x0, 48.0f) && Near(b.y1, 1053.0f));
        }
        {
            const ScreenRect w = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f);
            const ScreenRect unread = RadarKeepOut(1920, 1080, 1.0f, false, 0.0f), nan = RadarKeepOut(1920, 1080, std::nanf(""), true, 0.0f);
            const ScreenRect low = RadarKeepOut(1920, 1080, 0.5f, true, 0.0f), high = RadarKeepOut(1920, 1080, 1.4f, true, 0.0f);
            auto same = [&](const ScreenRect& o) { return Near(o.x0, w.x0) && Near(o.y0, w.y0) && Near(o.x1, w.x1) && Near(o.y1, w.y1); };
            Check("minimap: an unreadable, NaN or out-of-range safe zone takes the widest margin (0.9)", same(unread) && same(nan) && same(low) && same(high));
        }
        {
            const ScreenRect a = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f), b = RadarKeepOut(3840, 2160, 0.9f, true, 0.0f);
            const ScreenRect wide = RadarKeepOut(3440, 1440, 0.9f, true, 0.0f), std16 = RadarKeepOut(2560, 1440, 0.9f, true, 0.0f);
            Check("minimap: the rectangle scales with the display height (4K = 2 x 1080p) and its size does not depend on the width",
                  Near(b.x1 - b.x0, 2.0f * (a.x1 - a.x0), 1.0f) && Near(b.y1 - b.y0, 2.0f * (a.y1 - a.y0), 1.0f) && Near(wide.x1 - wide.x0, std16.x1 - std16.x0, 0.01f)
                      && wide.x0 > std16.x0);
        }
        {
            ScreenRect a{0, 0, 10, 10}, in{5, 5, 15, 15}, apart{20, 0, 30, 10}, touch{10, 0, 20, 10}, empty{4, 4, 4, 9};
            Check("minimap: RectsOverlap: overlapping true; apart, edge-touching and empty false",
                  RectsOverlap(a, in) && RectsOverlap(in, a) && !RectsOverlap(a, apart) && !RectsOverlap(a, touch) && !RectsOverlap(a, empty) && !RectsOverlap(empty, a));
        }

        // 19-27. the vitals plate under the radar.
        const float kMinW = 180.0f, kMaxW = 340.0f, kGap = 6.0f, kEdge = 10.0f;
        {
            const ScreenRect radar = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f);
            const VitalsDock d = DockVitals(1920, 1080, radar, 90.0f, kMinW, kMaxW, kGap, kEdge, kDockMaxCover);
            Check("minimap: dock at 1080p, safe zone 0.9: the radar's left edge and width, resting 10 px above the bottom, 46 px (24 %) up into the radar",
                  d.docked && Near(d.x, radar.x0) && Near(d.w, radar.x1 - radar.x0) && Near(d.y, 980.0f) && Near(d.y + d.h, 1070.0f) && Near(d.coverPx, 46.0f)
                      && Near(d.coverFrac, 0.2417f, 0.01f),
                  std::to_string(d.x) + "," + std::to_string(d.y) + "," + std::to_string(d.w) + " cover " + std::to_string(d.coverPx));
        }
        {
            // A plate short enough to fit under the radar sits the gap below it and covers nothing.
            const ScreenRect radar = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f);
            const VitalsDock d = DockVitals(1920, 1080, radar, 30.0f, kMinW, kMaxW, kGap, kEdge, kDockMaxCover);
            Check("minimap: a plate that fits under the radar rests the gap below it and covers nothing",
                  d.docked && Near(d.y, radar.y1 + kGap) && d.coverPx == 0.0f && d.coverFrac == 0.0f);
        }
        {
            const VitalsDock a = DockVitals(1920, 1080, RadarKeepOut(1920, 1080, 0.95f, true, 0.0f), 90.0f, kMinW, kMaxW, kGap, kEdge, kDockMaxCover);
            const VitalsDock b = DockVitals(1920, 1080, RadarKeepOut(1920, 1080, 1.0f, true, 0.0f), 90.0f, kMinW, kMaxW, kGap, kEdge, kDockMaxCover);
            Check("minimap: a safe zone of 0.95 or 1.0 would hide over a third of the radar (38 % / 53 %): the plate stays bottom-right (not docked, with the reason)",
                  !a.docked && a.why == DockWhy::CoversRadar && !b.docked && b.why == DockWhy::CoversRadar,
                  std::to_string(a.coverFrac) + " / " + std::to_string(b.coverFrac));
        }
        {
            const VitalsDock lim = DockVitals(1920, 1080, RadarKeepOut(1920, 1080, 0.9f, true, 0.0f), 90.0f, kMinW, kMaxW, kGap, kEdge, 0.2f);
            Check("minimap: the cover limit is the argument: 24 % docks at the default limit and not at 20 %", !lim.docked && lim.why == DockWhy::CoversRadar);
        }
        {
            const ScreenRect narrow{100, 800, 160, 1000}, wideR{100, 800, 500, 1000};
            const VitalsDock a = DockVitals(1920, 1080, narrow, 40.0f, kMinW, kMaxW, kGap, kEdge, kDockMaxCover);
            const VitalsDock b = DockVitals(1920, 1080, wideR, 40.0f, kMinW, kMaxW, kGap, kEdge, kDockMaxCover);
            Check("minimap: the plate's width is the radar's, held to the 180..340 px band", a.docked && Near(a.w, 180.0f) && b.docked && Near(b.w, 340.0f) && Near(a.x, 100.0f));
        }
        {
            // Every common display at the default safe zone: docked, wholly on screen, cover within the limit, aligned to the radar.
            struct Disp { float w, h; };
            const Disp displays[] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {1920, 1200}, {1024, 768}, {2560, 1080}, {3440, 1440}};
            int good = 0;
            std::string bad;
            for (const Disp& d : displays)
            {
                const ScreenRect radar = RadarKeepOut(d.w, d.h, 0.9f, true, 0.0f);
                const float plateH = std::floor(S(20.0f, d.h) + 2.0f * S(22.0f, d.h) + S(10.0f, d.h) + S(16.0f, d.h));
                const VitalsDock v = DockVitals(d.w, d.h, radar, plateH, S(180.0f, d.h), S(340.0f, d.h), S(6.0f, d.h), S(10.0f, d.h), kDockMaxCover);
                const bool fine = v.docked && v.x >= 0.0f && v.x + v.w <= d.w && v.y >= 0.0f && v.y + v.h <= d.h - S(10.0f, d.h) + 0.01f && v.coverFrac <= kDockMaxCover
                                  && Near(v.x, radar.x0, 0.01f);
                good += fine ? 1 : 0;
                if (!fine)
                    bad += std::to_string((int)d.w) + "x" + std::to_string((int)d.h) + " ";
            }
            Check("minimap: dock at safe zone 0.9 on 10 common displays (720p to 4K, 4:3, 16:10, 21:9): docked, on screen, cover within the limit, aligned to the radar",
                  good == 10, bad);
        }
        {
            // The docked plate and the speedometer (bottom-right: right edge at W - 28 px, width up to 272 px, in design pixels) never share columns.
            struct Disp { float w, h; };
            const Disp displays[] = {{1280, 720}, {1920, 1080}, {3840, 2160}, {1024, 768}, {1280, 1024}};
            int good = 0;
            for (const Disp& d : displays)
            {
                const ScreenRect radar = RadarKeepOut(d.w, d.h, 0.9f, true, 0.0f);
                const float plateH = S(90.0f, d.h);
                const VitalsDock v = DockVitals(d.w, d.h, radar, plateH, S(180.0f, d.h), S(340.0f, d.h), S(6.0f, d.h), S(10.0f, d.h), kDockMaxCover);
                const float gut = S(28.0f, d.h), sw = std::floor(std::fmin(S(272.0f, d.h), std::fmax(S(160.0f, d.h), d.w - 2.0f * gut)));
                const float speedoX0 = std::floor(d.w - gut - sw);
                good += (v.docked && v.x + v.w < speedoX0) ? 1 : 0;
            }
            Check("minimap: the docked plate ends left of the speedometer's column on 5 displays (incl. 4:3 and 5:4)", good == 5);
        }
        {
            const ScreenRect radar = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f);
            const VitalsDock tooShort = DockVitals(1920, 80, RadarKeepOut(1920, 80, 0.9f, true, 0.0f), 90.0f, kMinW, kMaxW, kGap, kEdge, kDockMaxCover);
            const VitalsDock tooNarrow = DockVitals(300, 1080, radar, 90.0f, kMinW, kMaxW, kGap, kEdge, kDockMaxCover);
            const VitalsDock noRadar = DockVitals(0, 0, ScreenRect(), 90.0f, kMinW, kMaxW, kGap, kEdge, kDockMaxCover);
            Check("minimap: a display too short, too narrow or unknown never docks, each with its own reason",
                  !tooShort.docked && tooShort.why == DockWhy::TooShort && !tooNarrow.docked && tooNarrow.why == DockWhy::TooNarrow && !noRadar.docked
                      && noRadar.why == DockWhy::NoRadar);
        }
        {
            // The radar rectangle the clearance check uses (padded by a 2 % gutter) contains the unpadded one and grows on every side.
            const ScreenRect a = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f), p = RadarKeepOut(1920, 1080, 0.9f, true, 0.02f);
            Check("minimap: the padded keep-out grows the radar by 2 % of the height (21.6 px) on every side",
                  Near(a.x0 - p.x0, 21.6f, 0.1f) && Near(p.x1 - a.x1, 21.6f, 0.1f) && Near(a.y0 - p.y0, 21.6f, 0.1f) && Near(p.y1 - a.y1, 21.6f, 0.1f));
        }
    }

} // namespace minimaptest
