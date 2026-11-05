// 0.4.22 minimap (libs/sync/minimap.h): when GTA's radar is shown, how its health / armour bars are hidden, where the radar is on screen and
// where our vitals plate docks under it. Included by tests/gameplay_test.cpp after its Check(); run by minimaptest::RunPure(), kChecks checks.
// Every check below was run against a mutant of the rule it pins (the list is in the 0.4.22 report); a check that survives its mutant is a bug here.
#pragma once

#include "sync/minimap.h"
#include "minimap_size_tests.h"   // 0.4.25 minimap_scale: the registry value, the component scaling, the write gate, the table pattern (needs Check above)

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace minimaptest {

    // 28 for 0.4.22, 8 for the 0.4.25 radar scale (rectangle, vitals strip, flip request), then minimap_size_tests.h's own.
    constexpr int kScaleChecks = 8;
    constexpr int kChecks = 28 + kScaleChecks + minimapsizetest::kChecks;

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

        // ---- 0.4.25 minimap_scale: the radar rectangle, the strip under it, the flip request ----
        std::printf("-- 0.4.25 minimap_scale: radar rectangle at a scale, vitals strip flush under it, big-radar flip request\n");
        {
            const ScreenRect full = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f), s8 = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f, 0.8f);
            const ScreenRect pad = RadarKeepOut(1920, 1080, 0.9f, true, 0.02f, 0.8f);
            Check("minimap scale 0.8 at 1920x1080, safe zone 0.9: the radar keeps its left edge (96) and bottom edge (1026) and shrinks to 216 x 152 px; the 2 % gutter is added after the shrink",
                  Near(s8.x0, full.x0, 0.01f) && Near(s8.y1, full.y1, 0.01f) && Near(s8.x1 - s8.x0, 0.8f * (full.x1 - full.x0), 0.01f) && Near(s8.x1 - s8.x0, 216.0f, 0.01f)
                      && Near(s8.y1 - s8.y0, 0.8f * (full.y1 - full.y0), 0.01f) && Near(s8.y1 - s8.y0, 152.3f, 0.6f) && Near(s8.x0 - pad.x0, 21.6f, 0.1f)
                      && Near(pad.x1 - s8.x1, 21.6f, 0.1f) && Near(s8.y0 - pad.y0, 21.6f, 0.1f) && Near(pad.y1 - s8.y1, 21.6f, 0.1f),
                  std::to_string(s8.x0) + "," + std::to_string(s8.y0) + "," + std::to_string(s8.x1) + "," + std::to_string(s8.y1));
        }
        {
            const ScreenRect full = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f), def = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f, 1.0f);
            const ScreenRect low = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f, 0.2f), half = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f, 0.5f);
            const ScreenRect high = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f, 3.0f), nan = RadarKeepOut(1920, 1080, 0.9f, true, 0.0f, std::nanf(""));
            auto same = [](const ScreenRect& a, const ScreenRect& b) { return Near(a.x0, b.x0, 0.01f) && Near(a.y0, b.y0, 0.01f) && Near(a.x1, b.x1, 0.01f) && Near(a.y1, b.y1, 0.01f); };
            Check("minimap scale: the default is the game's own size; below 0.5 is held to 0.5, above 1 and NaN count as 1 (the rectangle never grows past the game's)",
                  same(full, def) && same(low, half) && Near(half.x1 - half.x0, 0.5f * (full.x1 - full.x0), 0.01f) && same(high, full) && same(nan, full));
        }
        // The strip's size and place, as ClusterLayout (apps/engine/overlay_ui.cpp) computes them: a 7 % strip (never under 11 px) that shrinks to the free margin under
        // the radar, resting 2 px or more above the screen edge, never covering the map.
        struct Strip { VitalsDock d; ScreenRect radar; };
        auto stripFor = [](float W, float H, float safe, float scale) {
            Strip s;
            s.radar = RadarKeepOut(W, H, safe, true, 0.0f, scale);
            const float edge = std::fmax(2.0f, std::floor(S(2.0f, H)));
            const float room = std::floor(H - edge - s.radar.y1);
            const float stripH = std::fmax(11.0f, std::fmin(std::floor((s.radar.y1 - s.radar.y0) * 0.07f), room));
            s.d = DockVitals(W, H, s.radar, stripH, S(120.0f, H), S(400.0f, H), 0.0f, edge, 0.0f);
            return s;
        };
        {
            const Strip a = stripFor(1920, 1080, 0.9f, 0.8f), b = stripFor(1920, 1080, 0.9f, 1.0f);
            Check("minimap scale 0.8 at 1080p: the vitals strip stays flush under the smaller map (top = the radar's bottom edge, 0 px into it), takes its left edge and its 216 px width",
                  a.d.docked && Near(a.d.y, a.radar.y1, 0.01f) && a.d.coverPx == 0.0f && Near(a.d.x, a.radar.x0, 0.01f) && Near(a.d.w, 216.0f, 0.5f) && Near(a.d.y, b.d.y, 0.01f)
                      && a.d.w < b.d.w && a.d.h >= 11.0f,
                  std::to_string(a.d.y) + " vs radar bottom " + std::to_string(a.radar.y1) + ", w " + std::to_string(a.d.w));
        }
        {
            struct Disp { float w, h; };
            const Disp displays[] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {1920, 1200}, {1024, 768}, {2560, 1080}, {3440, 1440}};
            int good = 0;
            std::string bad;
            for (float scale : {0.8f, 0.5f})
                for (const Disp& d : displays)
                {
                    const Strip s = stripFor(d.w, d.h, 0.9f, scale);
                    const float rw = s.radar.x1 - s.radar.x0;
                    const float want = std::fmax(S(120.0f, d.h), std::fmin(rw, S(400.0f, d.h)));
                    const bool fine = s.d.docked && s.d.coverPx == 0.0f && Near(s.d.y, s.radar.y1, 0.01f) && Near(s.d.x, s.radar.x0, 0.01f) && Near(s.d.w, want, 0.01f)
                                      && s.d.y + s.d.h <= d.h - 1.0f;
                    good += fine ? 1 : 0;
                    if (!fine)
                        bad += std::to_string((int)d.w) + "x" + std::to_string((int)d.h) + "@" + std::to_string(scale) + " ";
                }
            Check("minimap scale 0.8 and 0.5 on 10 common displays (720p to 4K, 4:3, 16:10, 21:9): the strip is docked flush under the shrunken radar, aligned to its left edge, on screen",
                  good == 20, bad);
        }
        {
            // The big-radar flip a size change asks for.
            auto drive = [](BarsGate& g, int ticks, double& t, const BarsInputs& in) { return Drive(g, ticks, t, in); };
            BarsInputs on;
            on.radarOn = true;
            on.haveHandle = true;
            on.loaded = true;
            BarsInputs off = on;
            off.radarOn = false;
            BarsGate g;
            g.SetMode(BarsMode::Off);
            double t = 0;
            g.RequestFlip();
            const auto hidden = drive(g, 10, t, off);       // the radar is not shown: the flip waits
            const bool waiting = Count(hidden, BarsAction::None) == 10 && g.FlipAsked();
            const auto shown = drive(g, 10, t, on);
            Check("minimap scale: a flip asked for in bars-off mode waits for the radar to show, then is exactly one big-radar on, off pair (and no SETUP_HEALTH_ARMOUR)",
                  waiting && shown[0] == BarsAction::RefreshOn && shown[1] == BarsAction::RefreshOff && Count(shown, BarsAction::None) == 8 && g.Counters().refreshOn == 1
                      && g.Counters().refreshOff == 1 && !g.FlipAsked() && !g.BigmapOn(),
                  Names(shown));
        }
        {
            // Default mode, per-entry flip still owed (the sizes are written before the entry): the ask is absorbed, one flip in total.
            BarsGate g;
            BarsInputs on;
            on.radarOn = true;
            on.haveHandle = true;
            on.loaded = true;
            g.RequestFlip();
            double t = 0;
            const auto v = Drive(g, 30, t, on);
            Check("minimap scale: in the default mode the ask made before the per-entry flip is absorbed by it (one flip, not two), SETUP_HEALTH_ARMOUR follows as before",
                  !g.FlipAsked() && Count(v, BarsAction::RefreshOn) == 1 && Count(v, BarsAction::RefreshOff) == 1 && Count(v, BarsAction::Setup) == 28, Names(v));
        }
        {
            // Default mode, the entry's flip done: a later change (the game reset the table and we wrote it again) asks for one more.
            BarsGate g;
            BarsInputs on;
            on.radarOn = true;
            on.haveHandle = true;
            on.loaded = true;
            double t = 0;
            Drive(g, 20, t, on);
            g.RequestFlip();
            const auto v = Drive(g, 5, t, on);
            Check("minimap scale: in the default mode a later ask (the table was written again) is one more flip, then SETUP_HEALTH_ARMOUR resumes",
                  g.FlipAsked() == false && v[0] == BarsAction::RefreshOn && v[1] == BarsAction::RefreshOff && v[2] == BarsAction::Setup && g.Counters().refreshOn == 2
                      && g.Counters().refreshOff == 2,
                  Names(v));
        }
        {
            // Bars setup-only mode (minimap_bars=2): no per-entry flip, so the ask is honoured.
            BarsGate g;
            g.SetMode(BarsMode::Setup);
            BarsInputs on;
            on.radarOn = true;
            on.haveHandle = true;
            on.loaded = true;
            double t = 0;
            Drive(g, 5, t, on);
            g.RequestFlip();
            const auto v = Drive(g, 5, t, on);
            Check("minimap scale: in bars setup-only mode the ask is honoured: one flip pair, the setup calls go on", v[0] == BarsAction::RefreshOn && v[1] == BarsAction::RefreshOff
                  && Count(v, BarsAction::Setup) == 3 && g.Counters().refreshOn == 1, Names(v));
        }

        minimapsizetest::RunPure();
        minimapsizetest::RunDump();
    }

} // namespace minimaptest
