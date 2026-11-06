// 0.4.31: the car cluster (libs/sync/hud_car.h) against GI's REV2 BUILD_SPEC_CAR.md (HUD_DESIGN_COMPONENTS/REV2). Included by tests/gameplay_test.cpp
// after its Check() and F(); run by hudcar_t::RunPure(), kChecks checks.
#pragma once

#include "sync/hud_car.h"

#include <cmath>
#include <cstdio>

namespace hudcar_t {

    using namespace gage::sync::hudcar;

    constexpr int kChecks = 10;

    inline bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
    inline bool At(Pt p, float x, float y, float tol = 0.01f) { return Near(p.x, x, tol) && Near(p.y, y, tol); }

    inline void RunPure()
    {
        {
            // 3.3: fill end points and needle centres per speed (the spec's table)
            const bool fill = At(OnDial(SpeedDeg(125), kArcR), 1535.37f, 377.31f) && At(OnDial(SpeedDeg(184), kArcR), 1591.53f, 443.45f) &&
                              At(OnDial(SpeedDeg(250), kArcR), 1684.00f, 472.00f) && At(OnDial(SpeedDeg(400), kArcR), 1844.42f, 342.10f) &&
                              At(OnDial(SpeedDeg(436), kArcR), 1846.87f, 288.81f) && At(OnDial(SpeedDeg(500), kArcR), 1809.63f, 202.58f);
            const bool needle = At(OnDial(SpeedDeg(0), kNeedleR), 1576.75f, 218.01f) && At(OnDial(SpeedDeg(125), kNeedleR), 1557.12f, 367.17f) &&
                                At(OnDial(SpeedDeg(400), kNeedleR), 1820.94f, 337.11f) && At(OnDial(SpeedDeg(500), kNeedleR), 1791.25f, 218.01f);
            Check("hud car: the speed fill ends and the needle centres sit where the spec's table says (125 / 184 / 250 / 400 / 436 / 500 km/h)", fill && needle,
                  F("184 end (%.2f, %.2f), 0 needle (%.2f, %.2f)", OnDial(SpeedDeg(184), kArcR).x, OnDial(SpeedDeg(184), kArcR).y, OnDial(SpeedDeg(0), kNeedleR).x,
                    OnDial(SpeedDeg(0), kNeedleR).y));
        }
        {
            // the track ends and the redline (speed 400 = a 12)
            const bool ok = At(OnDial(kSpeedStartDeg, kArcR), 1558.37f, 202.58f) && At(OnDial(kSpeedStartDeg - kSpeedSweepDeg, kArcR), 1809.63f, 202.58f) &&
                            Near(SpeedDeg(400), kRedFromDeg, 1e-4f) && !Redline(400) && Redline(401) && Speed(612.0f) == 500 && Speed(-3.0f) == 0 &&
                            Speed(std::nanf("")) == 0 && Speed(183.5f) == 184;
            Check("hud car: the track runs 220 -> -40 (ends (1558.37, 202.58) / (1809.63, 202.58)), the redline starts at 400 (a 12) and ember is ABOVE 400; "
                  "speed is rounded and held to 0..500",
                  ok, "");
        }
        {
            // 3.4 ticks: a few rows of the table, both ends, width class and colour class
            bool ok = true;
            const struct { int i; float x0, y0, x1, y1; } rows[] = {
                {0, 1582.88f, 223.15f, 1570.63f, 212.87f}, {1, 1558.37f, 243.99f, 1552.13f, 240.81f}, {8, 1626.14f, 426.64f, 1619.12f, 441.02f},
                {10, 1684.00f, 449.00f, 1684.00f, 456.00f}, {16, 1813.12f, 335.44f, 1828.77f, 338.77f}, {19, 1809.63f, 243.99f, 1815.87f, 240.81f}};
            for (const auto& r : rows)
            {
                const float rin = TickMajor(r.i) ? kTickMajorInnerR : kTickMinorInnerR;
                ok = ok && At(OnDial(TickDeg(r.i), rin), r.x0, r.y0) && At(OnDial(TickDeg(r.i), kTickOuterR), r.x1, r.y1);
            }
            ok = ok && TickMajor(0) && TickMajor(4) && !TickMajor(10) && TickMajor(20) && !TickRed(15) && TickRed(16);
            Check("hud car: the 21 ticks match the spec's table (ends, major every 100 km/h, red from 400)", ok, "");
        }
        {
            // 3.5 dial numerals at r 114
            const bool ok = At(OnDial(TickDeg(0), kDialNumR), 1596.67f, 234.72f) && At(OnDial(TickDeg(4), kDialNumR), 1572.49f, 331.70f) &&
                            At(OnDial(TickDeg(12), kDialNumR), 1733.97f, 410.46f) && At(OnDial(TickDeg(20), kDialNumR), 1771.33f, 234.72f);
            Check("hud car: the dial numerals' anchors (0 / 100 / 300 / 500) match the spec", ok, "");
        }
        {
            // 3.6 fuel: track ends, fill ends, low / pulse rules
            const bool ok = At(OnDial(kFuelStartDeg, kArcR), 1589.93f, 173.66f) && At(OnDial(kFuelStartDeg + kFuelSweepDeg, kArcR), 1778.07f, 173.66f) &&
                            At(OnDial(FuelDeg(50), kArcR), 1684.00f, 144.00f) && At(OnDial(FuelDeg(25), kArcR), 1634.68f, 151.59f) &&
                            At(OnDial(FuelDeg(8), kArcR), 1603.49f, 165.12f) && FuelLow(25) && !FuelLow(26) && FuelArcPulse(10) && !FuelArcPulse(11) &&
                            !FuelArcPulse(0) && PumpChip(25) == Chip::Warn && PumpChip(26) == Chip::OnCalm && PumpChip(0) == Chip::Warn;
            Check("hud car: the fuel arc (E 235 -> F 305) ends where the spec says, low at 25 and under (the pump warns), the arc pulses only at 1..10", ok, "");
        }
        {
            // 3.8 the readout run, 3.10 the fuel row
            const FuelRow r62 = LayoutFuelRow(62), r100 = LayoutFuelRow(100);
            const bool ok = Near(SpeedRunX(184), 1596.16f, 0.01f) && Near(r62.pumpX, 1645.0f, 0.01f) && Near(r62.numX, 1675.0f, 0.01f) &&
                            Near(r62.pctX, 1709.78f, 0.02f) && Near(r100.pumpX, 1636.80f, 0.01f);
            Check("hud car: the speed numeral run starts at 1596.16 for \"184\"; the fuel row puts the pump at 1645 and the value at 1675 for 62, the row "
                  "starts at 1636.80 for 100",
                  ok, F("run %.2f, 62: pump %.2f num %.2f pct %.2f, 100: %.2f", SpeedRunX(184), r62.pumpX, r62.numX, r62.pctX, r100.pumpX));
        }
        {
            // 4: the chip mapping
            CarState s;
            s.locked = true; s.engineOn = true; s.belted = true; s.lights = 2;
            const bool base = LockChip(s) == Chip::OnAmber && EngineChip(s) == Chip::OnAmber && BeltChip(s) == Chip::OnCalm && LightsChip(s) == Chip::OnAmber;
            CarState w = s; w.belted = false;
            CarState o = w; o.engineOn = false;
            CarState u = s; u.locked = false; u.lights = 0;
            const bool ok = base && BeltChip(w) == Chip::Warn && BeltChip(o) == Chip::Off && EngineChip(o) == Chip::Off && LockChip(u) == Chip::Off &&
                            LightsChip(u) == Chip::Off;
            Check("hud car: chips: locked / running / lit = amber on, buckled = calm on, unbuckled with the engine running = warn, with it stopped = off", ok, "");
        }
        {
            const ChipLook off = LookOf(Chip::Off), arrowOff = LookOf(Chip::Off, true), warn = LookOf(Chip::Warn), calm = LookOf(Chip::OnCalm);
            const bool ok = off.icon == kMuted && Near(off.iconAlpha, 0.55f, 1e-6f) && Near(arrowOff.iconAlpha, 0.28f, 1e-6f) && warn.glow == 0xE5603A59u &&
                            calm.glow == 0u && calm.fill == 0xD9D1C31Au;
            Check("hud car: the chip looks (off muted at 0.55, arrows 0.28; calm has no outer glow; warn glows ember)", ok, "");
        }
        {
            // 7: the blink
            const bool ok = Near(BlinkOpacity(0.0), 1.0f, 1e-6f) && Near(BlinkOpacity(391.0), 1.0f, 1e-6f) && Near(BlinkOpacity(396.0), 0.6f, 1e-4f) &&
                            Near(BlinkOpacity(400.0), 0.2f, 1e-6f) && Near(BlinkOpacity(799.0), 0.2f, 1e-6f) && Near(BlinkOpacity(800.0), 1.0f, 1e-6f);
            BlinkClock bc;
            const float a = bc.Step(true, 1000.0), b = bc.Step(true, 1500.0), c = bc.Step(false, 1600.0), d = bc.Step(true, 1700.0);
            Check("hud car: the blink is 1.0 to 392 ms, 0.2 from 400 to 800 ms (period 800), and restarts at 1.0 when switched on",
                  ok && Near(a, 1.0f, 1e-6f) && Near(b, 0.2f, 1e-6f) && Near(c, 1.0f, 1e-6f) && Near(d, 1.0f, 1e-6f), F("%.3f %.3f %.3f %.3f", a, b, c, d));
        }
        {
            // bottom-RIGHT anchor; 0.4.34 the cluster at kSize (0.5) about its own bottom-right corner (56 * H / 1080 from the right and the bottom):
            // on 1080p the dial centre is (1774, 898) screen y-down; on a 2560x1080 ultrawide it keeps 146 px from the right edge
            const Pt c1080 = ToScreen({kCx, kCy}, 1920.0f, 1080.0f), cUw = ToScreen({kCx, kCy}, 2560.0f, 1080.0f), c1440 = ToScreen({kCx, kCy}, 2560.0f, 1440.0f);
            const Pt corner = ToScreen({kCompX + kCompW, kCompY}, 1920.0f, 1080.0f), tl = ToScreen({kCompX, kCompY + kCompH}, 1920.0f, 1080.0f);
            const bool ok = kSize == 0.5f && At(c1080, 1774.0f, 898.0f) && At(cUw, 2414.0f, 898.0f) && At(c1440, 2365.3333f, 1440.0f - 242.6667f) &&
                            At(corner, 1864.0f, 1024.0f) && At(tl, 1684.0f, 808.0f) && Near(Scale(1080.0f), 0.5f, 1e-6f);
            Check("hud car: the cluster is the design at 0.5 about its bottom-right corner (56 * H / 1080 from the right edge and the bottom; box 180 x 216)", ok,
                  F("1080 (%.2f, %.2f), uw (%.2f, %.2f), 1440 (%.2f, %.2f), top-left (%.2f, %.2f)", c1080.x, c1080.y, cUw.x, cUw.y, c1440.x, c1440.y, tl.x, tl.y));
        }
    }

} // namespace hudcar_t
