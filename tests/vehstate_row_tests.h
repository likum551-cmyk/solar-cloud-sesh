// 0.4.27: the vehicle state row (libs/sync/vehicle_state_row.h) and the indicator fallback (libs/sync/indicator_fallback.h). Included by
// tests/gameplay_test.cpp after its Check() and F(); run by vehrow::RunPure(), kChecks checks.
//
//   A. where the row goes: the live 0.4.26 geometry (2560x1440, radar scale 0.8), below the strip when the margin allows, the bottom-right
//      cluster, refusals, and a grid of displays x safe zones x radar scales x strip fractions (it never covers the radar, never leaves the
//      screen, always takes the strip's left edge and width)
//   B. the cells: equal, inside the padding, clear of the cut corner, toggles inside their cells, knobs inside their tracks
//   C. the change pulse and the tracker (getting in is not a change; a flip pulses its own cell only; the on-foot lock flash pulses)
//   D. the fallback: the switch, which cars, the blink, the corners (bones / box), the per-car phase table
#pragma once

#include "sync/indicator_fallback.h"
#include "sync/minimap.h"
#include "sync/vehicle_state_row.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace vehrow {

    using namespace gage::sync;
    namespace proto = gage::proto;

    constexpr int kChecks = 31;
    constexpr int kDockedCases = 66;   // of 72: zone 0.97 on 1280x720 leaves 8 px under the radar, less than the 11 px strip (3 scales x 2 fractions): not docked

    // The overlay's scale (overlay_ui.cpp S()).
    inline float Ui(float v, float displayH) { return std::floor(v * std::fmin(3.0f, std::fmax(0.75f, displayH / 1080.0f)) + 0.5f); }

    // The docked strip exactly as ClusterLayout computes it (kStripMinH 11, the margin rule of 0.4.23, DockVitals with gap 0 and no cover), and the
    // row's anchor exactly as DrawStateRow builds it from that.
    struct Docked
    {
        bool ok = false;
        ScreenRect radar;
        float sx = 0, sy = 0, sw = 0, sh = 0;
        RowAnchor an;
    };
    inline Docked DockedAnchor(float W, float H, float safeZone, float scale, float stripFrac)
    {
        Docked o;
        o.radar = RadarKeepOut(W, H, safeZone, true, 0.0f, scale);
        const float edge = std::fmax(2.0f, std::floor(Ui(2.0f, H)));
        const float room = std::floor(H - edge - o.radar.y1);
        const float stripH = std::fmax(11.0f, std::fmin(std::floor((o.radar.y1 - o.radar.y0) * stripFrac), room));
        const VitalsDock d = DockVitals(W, H, o.radar, stripH, Ui(120.0f, H), Ui(400.0f, H), 0.0f, edge, 0.0f);
        if (!d.docked)
            return o;
        o.ok = true;
        o.sx = std::floor(d.x);
        o.sw = std::floor(d.w);
        o.sh = std::floor(d.h);
        o.sy = std::floor(d.y);
        o.an.screenH = H;
        o.an.docked = true;
        o.an.edge = edge;
        o.an.x = o.sx;
        o.an.w = o.sw;
        o.an.rowH = o.sh;
        o.an.stripBottom = o.sy + o.sh;
        o.an.radarTop = o.radar.y0;
        o.an.gap = std::fmax(2.0f, std::floor(Ui(4.0f, H)));
        return o;
    }

    inline void TestPlace()
    {
        // A1. The live 0.4.26 picture (gage.log: strip x 38-326 y 1418-1432 under radar y 1215-1418, 2560x1440): 6 px under the strip, no room for a
        // 14 px row, so it sits above the radar, its bottom `gap` above the radar's top, the strip's x and width.
        {
            RowAnchor a;
            a.screenH = 1440; a.docked = true; a.x = 38; a.w = 288; a.rowH = 14; a.stripBottom = 1432; a.radarTop = 1215; a.gap = 5; a.edge = 2;
            const RowRect r = PlaceStateRow(a);
            Check("row: 2560x1440 live geometry: no room under the strip -> above the radar, clear of the map, the strip's left edge / width / height",
                  r.where == RowWhere::AboveRadar && r.y + r.h <= 1215 - 5 && r.y == 1196 && r.x == 38 && r.w == 288 && r.h == 14,
                  F("where %s y %.0f-%.0f x %.0f w %.0f", RowWhereName(r.where), (double)r.y, (double)(r.y + r.h), (double)r.x, (double)r.w));
        }
        // A2. A safe zone of 0.9 leaves 5 % of the height under the radar: the row goes directly under the strip.
        {
            const Docked d = DockedAnchor(1920, 1080, 0.9f, 1.0f, 0.07f);
            const RowRect r = PlaceStateRow(d.an);
            Check("row: safe zone 0.9 (54 px margin at 1080p) -> directly under the strip, gap below its bottom",
                  d.ok && r.where == RowWhere::BelowStrip && r.y == d.an.stripBottom + d.an.gap && r.y + r.h <= 1080 - d.an.edge,
                  F("docked %d where %s y %.0f strip bottom %.0f", d.ok ? 1 : 0, RowWhereName(r.where), (double)r.y, (double)d.an.stripBottom));
        }
        // A3. The whole grid: 4 displays x 3 safe zones x 3 radar scales x 2 strip fractions (0.07 shipped, 0.0875 the pending minimap work). 0.97 is about
        // the live zone (radar bottom 1418 of 1440); a zone whose margin cannot hold the 11 px strip does not dock (DockVitals: CoversRadar), so the docked
        // count is pinned (kDockedCases) and a change in it is a change in the dock rule, not in the row.
        {
            const float disp[4][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
            const float zones[3] = {0.9f, 0.95f, 0.97f}, scales[3] = {0.5f, 0.8f, 1.0f}, fracs[2] = {0.07f, 0.0875f};
            int cases = 0, bad = 0, below = 0, above = 0;
            std::string first;
            for (const auto& dd : disp)
                for (const float z : zones)
                    for (const float s : scales)
                        for (const float fr : fracs)
                        {
                            const Docked d = DockedAnchor(dd[0], dd[1], z, s, fr);
                            if (!d.ok)
                                continue;
                            ++cases;
                            const RowRect r = PlaceStateRow(d.an);
                            const ScreenRect rr{r.x, r.y, r.x + r.w, r.y + r.h};
                            const ScreenRect strip{d.sx, d.sy, d.sx + d.sw, d.sy + d.sh};
                            const bool okCase = r.where != RowWhere::None && !RectsOverlap(rr, d.radar) && !RectsOverlap(rr, strip) && r.y >= 0.0f &&
                                                r.y + r.h <= dd[1] - d.an.edge && r.x == d.sx && r.w == d.sw && r.h == d.sh;
                            below += r.where == RowWhere::BelowStrip ? 1 : 0;
                            above += r.where == RowWhere::AboveRadar ? 1 : 0;
                            if (!okCase && ++bad == 1)
                                first = F("%.0fx%.0f zone %.2f scale %.2f frac %.4f: %s y %.0f h %.0f", (double)dd[0], (double)dd[1], (double)z, (double)s,
                                          (double)fr, RowWhereName(r.where), (double)r.y, (double)r.h);
                        }
            Check("row: every docked layout of the grid: never over the radar or the strip, on screen, the strip's x / width / height, both placements used",
                  cases == kDockedCases && bad == 0 && below > 0 && above > 0, F("%d cases, %d bad (%s), %d under the strip, %d above the radar", cases, bad, first.c_str(), below, above));
        }
        // A4. The bottom-right cluster: above the topmost plate; nothing when that is at the top of the screen; nothing on garbage.
        {
            RowAnchor a;
            a.screenH = 1080; a.docked = false; a.x = 1620; a.w = 272; a.rowH = 14; a.plateTop = 900; a.gap = 10;
            const RowRect r = PlaceStateRow(a);
            a.plateTop = 20;
            const RowRect n = PlaceStateRow(a);
            RowAnchor z;
            const RowRect g = PlaceStateRow(z);
            Check("row: not docked -> gap above the top plate with its edge and width; refused when it would leave the screen or on an empty anchor",
                  r.where == RowWhere::AbovePlate && r.y == 900 - 10 - 14 && r.x == 1620 && r.w == 272 && n.where == RowWhere::None && g.where == RowWhere::None,
                  F("%s y %.0f / %s / %s", RowWhereName(r.where), (double)r.y, RowWhereName(n.where), RowWhereName(g.where)));
        }
        // A5. Docked with no room under the strip and none above the radar (a radar at the top): nothing, never a row over the map.
        {
            RowAnchor a;
            a.screenH = 400; a.docked = true; a.x = 10; a.w = 120; a.rowH = 11; a.stripBottom = 398; a.radarTop = 8; a.gap = 4; a.edge = 2;
            Check("row: docked, no room under the strip or above the radar -> not drawn", PlaceStateRow(a).where == RowWhere::None);
        }
    }

    inline void TestCells()
    {
        // B1-B4 at the live size (288 x 14) and at the smallest (0.5 radar on 1440p: 180 x 11).
        const float sizes[2][2] = {{288, 14}, {180, 11}};
        for (const auto& sz : sizes)
        {
            const float x = 38, y = 1196, w = sz[0], h = sz[1], padL = 7, sep = 13, gap = 5;
            const RowLayout L = LayoutStateRow(x, y, w, h, padL, sep, gap);
            bool equal = true, inside = true, ordered = true, toggleIn = true, knobIn = true;
            for (int i = 0; i < kRowCells; ++i)
            {
                const RowCellBox& c = L.cell[i];
                equal &= std::fabs((c.x1 - c.x0) - (L.cell[0].x1 - L.cell[0].x0)) < 0.5f;
                inside &= c.x0 >= x + padL && c.x1 <= x + w - L.cut - 2.0f;
                if (i > 0)
                    ordered &= c.x0 >= L.cell[i - 1].x1 + sep - 0.5f;
                toggleIn &= c.tx0 >= c.x0 + L.gs + gap && c.tx1 == c.x1 && c.ty0 >= y && c.ty1 <= y + h;
                knobIn &= c.kOff >= c.tx0 && c.kOn + L.knob <= c.tx1 && c.kOn > c.kOff;
            }
            Check(F("cells %.0fx%.0f: laid out, toggles fit", (double)w, (double)h).c_str(), L.ok && L.toggles, F("ok %d toggles %d gs %.0f", L.ok, L.toggles, (double)L.gs));
            Check(F("cells %.0fx%.0f: equal widths, inside the padding and clear of the cut corner, sep apart", (double)w, (double)h).c_str(), equal && inside && ordered);
            Check(F("cells %.0fx%.0f: each toggle right-aligned in its cell after the glyph box, inside the row's height", (double)w, (double)h).c_str(), toggleIn);
            Check(F("cells %.0fx%.0f: knobs inside the tracks, on to the right of off", (double)w, (double)h).c_str(), knobIn);
        }
        // B5. Too narrow for glyph + gap + toggle: the glyphs stay, the toggles go; too thin: nothing.
        {
            const RowLayout narrow = LayoutStateRow(0, 0, 80, 14, 7, 13, 5);
            const RowLayout thin = LayoutStateRow(0, 0, 288, 4, 7, 13, 5);
            Check("cells: 80 px wide -> glyphs only; 4 px tall -> refused", narrow.ok && !narrow.toggles && !thin.ok);
        }
    }

    inline void TestPulse()
    {
        // C1. The envelope.
        {
            const bool shape = RowPulse(-1.0) == 0.0f && RowPulse(0.0) == 0.0f && std::fabs(RowPulse(kRowPulseRiseMs) - 1.0f) < 1e-4f &&
                               RowPulse(30.0) > 0.4f && RowPulse(30.0) < 0.6f && RowPulse(kRowPulseMs) == 0.0f && RowPulse(5000.0) == 0.0f;
            bool falling = true;
            float prev = 2.0f;
            for (double t = kRowPulseRiseMs; t < kRowPulseMs; t += 10.0)
            {
                const float v = RowPulse(t);
                falling &= v <= prev && v >= 0.0f;
                prev = v;
            }
            Check("pulse: 0 before, 1 at 60 ms, falls monotonically to 0 at 700 ms", shape && falling, F("30 ms %.2f, 300 ms %.2f", RowPulse(30.0), RowPulse(300.0)));
        }
        // C2. Getting in is not a change.
        RowTracker t;
        RowState s;
        s.visible = true; s.lock = 1; s.belt = false; s.engine = true;
        t.Update(s, 1000.0);
        Check("tracker: the row appearing pulses nothing", t.Pulse(kRowLock, 1030.0) == 0.0f && t.Pulse(kRowBelt, 1030.0) == 0.0f && t.Pulse(kRowEngine, 1030.0) == 0.0f);
        // C3. B: only the belt cell pulses.
        s.belt = true;
        t.Update(s, 2000.0);
        Check("tracker: a belt flip pulses the belt cell only", t.Pulse(kRowBelt, 2060.0) > 0.99f && t.Pulse(kRowLock, 2060.0) == 0.0f && t.Pulse(kRowEngine, 2060.0) == 0.0f);
        // C4. Y and L.
        s.engine = false;
        t.Update(s, 3000.0);
        s.lock = 0;
        t.Update(s, 3100.0);
        Check("tracker: Y pulses the engine cell, an answered L the lock cell", t.Pulse(kRowEngine, 3060.0) > 0.99f && t.Pulse(kRowLock, 3160.0) > 0.99f);
        // C5. The lock becoming known (the car's first state) is not a flip.
        {
            RowTracker u2;
            RowState a;
            a.visible = true; a.lock = -1;
            u2.Update(a, 0.0);
            a.lock = 1;
            u2.Update(a, 100.0);
            Check("tracker: lock unknown -> known pulses nothing", u2.Pulse(kRowLock, 130.0) == 0.0f);
        }
        // C6. Hidden, changed, shown again: a new baseline, no pulse.
        s.visible = false;
        t.Update(s, 4000.0);
        s.visible = true;
        s.belt = false;
        t.Update(s, 5000.0);
        Check("tracker: a change while hidden does not pulse when the row comes back", t.Pulse(kRowBelt, 5030.0) == 0.0f);
        // C7. The on-foot lock flash pulses its lock cell when it appears; entering a car from it does not.
        {
            RowTracker f;
            RowState a;
            a.visible = true; a.lockOnly = true; a.lock = 1;
            f.Update(a, 100.0);
            const float flashP = f.Pulse(kRowLock, 160.0);
            a.lockOnly = false;
            f.Update(a, 1000.0);
            Check("tracker: the on-foot lock flash pulses the lock cell; getting into the car from it pulses nothing",
                  flashP > 0.99f && f.Pulse(kRowLock, 1030.0) == 0.0f && f.Pulse(kRowBelt, 1030.0) == 0.0f, F("flash %.2f", flashP));
        }
    }

    inline void TestFallback()
    {
        // D1. The switch.
        Check("fallback switch: unset / 1 / junk = pack cars, 0 = off, 2 = every car",
              IndFallbackModeFrom(nullptr) == IndFallbackMode::PackCars && IndFallbackModeFrom(L"") == IndFallbackMode::PackCars &&
                  IndFallbackModeFrom(L"1") == IndFallbackMode::PackCars && IndFallbackModeFrom(L"00") == IndFallbackMode::PackCars &&
                  IndFallbackModeFrom(L"0") == IndFallbackMode::Off && IndFallbackModeFrom(L"2") == IndFallbackMode::AllCars);
        // D2. Which cars.
        Check("fallback: pack cars by default, stock cars keep the game's own indicators; off draws none; 2 draws every car",
              UseIndFallback(IndFallbackMode::PackCars, true) && !UseIndFallback(IndFallbackMode::PackCars, false) && !UseIndFallback(IndFallbackMode::Off, true) &&
                  UseIndFallback(IndFallbackMode::AllCars, false) && UseIndFallback(IndFallbackMode::AllCars, true));
        // D3. The blink: lit at once, dark at the half, lit again at the period; a 50 % duty cycle over 10 s; a step backwards stays lit.
        {
            const double t0 = 5000.0;
            int lit = 0, n = 0;
            for (double t = t0; t < t0 + 10000.0; t += 1.0, ++n)
                lit += IndBlinkLit(t, t0) ? 1 : 0;
            const double duty = (double)lit / (double)n;
            Check("blink: 1.5 Hz, lit at the switch-on, dark from the half period, lit again at the period, 50 % duty, lit on a clock step back",
                  IndBlinkLit(t0, t0) && IndBlinkLit(t0 + 330.0, t0) && !IndBlinkLit(t0 + 340.0, t0) && !IndBlinkLit(t0 + 660.0, t0) &&
                      IndBlinkLit(t0 + 670.0, t0) && std::fabs(duty - 0.5) < 0.01 && IndBlinkLit(t0 - 50.0, t0) && std::fabs(kIndPeriodMs - 666.667) < 0.01,
                  F("duty %.3f", duty));
        }
        // D4. Corners: left = lf + lr, right = rf + rr, hazards all, off none; the order is the bone names'.
        Check("corners: left lights 0 / 1, right 2 / 3, hazards all four, off none; names indicator_lf lr rf rr",
              IndCornerMask(proto::kVeh2IndicatorL) == 3u && IndCornerMask(proto::kVeh2IndicatorR) == 12u &&
                  IndCornerMask((uint8_t)(proto::kVeh2IndicatorL | proto::kVeh2IndicatorR)) == 15u && IndCornerMask(0) == 0u &&
                  std::string(kIndBoneNames[0]) == "indicator_lf" && std::string(kIndBoneNames[1]) == "indicator_lr" &&
                  std::string(kIndBoneNames[2]) == "indicator_rf" && std::string(kIndBoneNames[3]) == "indicator_rr");
        // D5. The box corners (a Jesko-sized box: 2.0 x 4.6 x 1.1 m, origin at the wheel centres).
        {
            const V3 mn{-1.0f, -2.3f, -0.5f}, mx{1.0f, 2.3f, 0.6f};
            const V3 lf = IndCornerFromDims(0, mn, mx), lr = IndCornerFromDims(1, mn, mx), rf = IndCornerFromDims(2, mn, mx), rr = IndCornerFromDims(3, mn, mx);
            const bool sides = lf.x < 0 && lr.x < 0 && rf.x > 0 && rr.x > 0 && lf.x > mn.x && rf.x < mx.x;
            const bool ends = lf.y > mx.y && rf.y > mx.y && lr.y < mn.y && rr.y < mn.y;
            const bool height = lf.z > mn.z && lf.z < mx.z && std::fabs(lf.z - (mn.z + 1.1f * kIndHeightFrac)) < 1e-4f;
            Check("box corners: left on the left, front past the front face, rear past the rear face, inside the box's height", sides && ends && height,
                  F("lf (%.2f %.2f %.2f) rr (%.2f %.2f %.2f)", (double)lf.x, (double)lf.y, (double)lf.z, (double)rr.x, (double)rr.y, (double)rr.z));
            Check("box: a car-sized box is used, a zero / inverted / huge box is refused",
                  IndDimsOk(mn, mx) && !IndDimsOk(V3{}, V3{}) && !IndDimsOk(mx, mn) && !IndDimsOk(V3{-50, -50, -50}, V3{50, 50, 50}));
        }
        // D6. A bone's light is pushed away from the car's centre, horizontally only.
        {
            const V3 c{100.0f, 200.0f, 30.0f};
            const V3 b{100.8f, 202.1f, 30.4f};
            const V3 p = IndPushOut(b, c, 0.10f);
            const float d0 = std::hypot(b.x - c.x, b.y - c.y), d1 = std::hypot(p.x - c.x, p.y - c.y);
            const V3 same = IndPushOut(c, c, 0.10f);
            Check("bone light: 0.10 m further from the centre, same height; a bone at the centre is left where it is",
                  std::fabs(d1 - d0 - 0.10f) < 1e-3f && p.z == b.z && same.x == c.x && same.y == c.y, F("%.3f -> %.3f", (double)d0, (double)d1));
        }
        // D7. The per-car phase: a car keeps its phase while its state holds, restarts lit on a change, is forgotten when not touched in a sweep.
        {
            IndPhaseTable t;
            const uint8_t L = proto::kVeh2IndicatorL, R = proto::kVeh2IndicatorR;
            const double a = t.Touch(11, L, 1000.0);
            t.Sweep();
            const double b = t.Touch(11, L, 1500.0);
            t.Sweep();
            const double c = t.Touch(11, R, 2000.0);
            t.Sweep();
            t.Sweep();                                       // a frame where car 11 had no indicator: it is not touched
            const double d = t.Touch(11, R, 3000.0);
            Check("phase table: kept while the state holds, restarted on a change, forgotten after a frame without an indicator",
                  a == 1000.0 && b == 1000.0 && c == 2000.0 && d == 3000.0 && t.Used() == 1, F("%.0f %.0f %.0f %.0f", a, b, c, d));
        }
        {
            IndPhaseTable t;
            bool allNew = true;
            for (int i = 0; i < IndPhaseTable::kCap; ++i)
                allNew &= t.Touch(100 + i, proto::kVeh2IndicatorL, 10.0) == 10.0;
            const double over = t.Touch(999, proto::kVeh2IndicatorL, 10.0);
            Check("phase table: holds kCap cars, the next one is refused and counted",
                  allNew && over < 0.0 && t.TakeFull() == 1u && t.TakeFull() == 0u && t.Used() == IndPhaseTable::kCap);
        }
        // D8. The whole chain for the Jesko as the pack files have it (no indicator_lf / indicator_rf bones, a box): with the left indicator on, the
        // lit frames draw two lights per frame, the front one from the box and the rear one from the bone; with the fallback off, none.
        {
            const int bones[4] = {-1, 38, -1, 39};
            const V3 mn{-1.0f, -2.3f, -0.5f}, mx{1.0f, 2.3f, 0.6f};
            auto run = [&](IndFallbackMode mode, int* fromBone, int* fromBox) {
                IndPhaseTable t;
                int lights = 0;
                *fromBone = *fromBox = 0;
                for (double now = 0.0; now < 2000.0; now += 16.0)
                {
                    if (UseIndFallback(mode, true))
                    {
                        const double since = t.Touch(7, proto::kVeh2IndicatorL, now);
                        if (since >= 0.0 && IndBlinkLit(now, since))
                        {
                            const unsigned m = IndCornerMask(proto::kVeh2IndicatorL);
                            for (int i = 0; i < kIndCorners; ++i)
                                if (m & (1u << i))
                                {
                                    ++lights;
                                    ++(bones[i] >= 0 ? *fromBone : (IndDimsOk(mn, mx) ? *fromBox : lights));
                                }
                        }
                    }
                    t.Sweep();
                }
                return lights;
            };
            int b1 = 0, x1 = 0, b0 = 0, x0 = 0;
            const int on = run(IndFallbackMode::PackCars, &b1, &x1);
            const int off = run(IndFallbackMode::Off, &b0, &x0);
            Check("Jesko chain: left indicator, 2 s: two lights per lit frame (front from the box, rear from the bone), half the frames lit; off: none",
                  on > 0 && b1 == x1 && b1 + x1 == on && std::abs(on / 2 - 63) <= 2 && off == 0, F("lights %d (bone %d, box %d), off %d", on, b1, x1, off));
        }
    }

    inline void RunPure()
    {
        std::printf("-- 0.4.27 vehicle state row + indicator fallback\n");
        TestPlace();
        TestCells();
        TestPulse();
        TestFallback();
    }

} // namespace vehrow
