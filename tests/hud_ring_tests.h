// 0.4.31: the vitals ring (libs/sync/hud_ring.h) against GI's REV1 BUILD_SPEC.md (HUD_DESIGN_COMPONENTS/REV1). Included by tests/gameplay_test.cpp after its
// Check() and F(); run by hudring_t::RunPure(), kChecks checks.
//   A. the state table (spec 4): every fill's end point in screen px (y up), the track ends, and the colours per state
//   B. the rules: the value (round, clamp, NaN), low v <= 25, critical 1..10, v == 0 draws nothing
//   C. the pulse (spec 5): the closed form at its corners, the ease's shape, the restart on entering critical
//   D. the scale to other displays (H / 1080, origin bottom-left)
#pragma once

#include "sync/hud_ring.h"

#include <cmath>
#include <cstdio>

namespace hudring_t {

    using namespace gage::sync::hudring;

    constexpr int kChecks = 9;

    inline bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
    inline bool At(Pt p, float x, float y) { return Near(p.x, x, 0.01f) && Near(p.y, y, 0.01f); }

    inline void RunPure()
    {
        // A. the spec's table (end point = the centre of the cap at the end of the fill), to 0.01 px
        {
            const Pt hp100 = OnArc(HpEndDeg(100)), hp50 = OnArc(HpEndDeg(50)), hp20 = OnArc(HpEndDeg(20)), hp8 = OnArc(HpEndDeg(8));
            const Pt sat100 = OnArc(SatEndDeg(100)), sat20 = OnArc(SatEndDeg(20));
            const bool ok = At(hp100, 155.54f, 386.16f) && At(hp50, 70.00f, 264.00f) && At(hp20, 103.39f, 177.01f) && At(hp8, 132.66f, 152.80f) &&
                            At(sat100, 244.46f, 386.16f) && At(sat20, 296.61f, 177.01f) && Near(HpEndDeg(8), 238.8f, 0.001f) && Near(SatEndDeg(20), -42.0f, 0.001f);
            Check("hud ring: every fill ends where the spec's state table says (HP 100 / 50 / 20 / 8 at (155.54, 386.16) / (70, 264) / (103.39, 177.01) / "
                  "(132.66, 152.80); SAT 100 / 20 at (244.46, 386.16) / (296.61, 177.01))",
                  ok, F("hp8 (%.2f, %.2f) sat20 (%.2f, %.2f) hp20 (%.2f, %.2f)", hp8.x, hp8.y, sat20.x, sat20.y, hp20.x, hp20.y));
        }
        {
            // the tracks: HP 250 -> 110, SAT -70 -> 70, mirrors across x = 200
            const Pt hb = OnArc(kHpStartDeg), ht = OnArc(kHpStartDeg - kSweepDeg), sb = OnArc(kSatStartDeg), st = OnArc(kSatStartDeg + kSweepDeg);
            Check("hud ring: the tracks end at the spec's points (HP (155.54, 141.84) / (155.54, 386.16), SAT (244.46, 141.84) / (244.46, 386.16)), mirror images",
                  At(hb, 155.54f, 141.84f) && At(ht, 155.54f, 386.16f) && At(sb, 244.46f, 141.84f) && At(st, 244.46f, 386.16f),
                  F("hp bottom (%.2f, %.2f) sat top (%.2f, %.2f)", hb.x, hb.y, st.x, st.y));
        }
        {
            const GaugeLook h100 = Look(true, 100), h26 = Look(true, 26), h25 = Look(true, 25), h0 = Look(true, 0), s100 = Look(false, 100), s20 = Look(false, 20);
            const bool ok = h100.arc == kAmber && h100.glow == kHpGlow && h100.label == kAmber && h100.numeral == kCream && h100.fill &&
                            h26.arc == kAmber && h25.arc == kEmber && h25.glow == kLowGlow && h25.label == kEmber && h25.numeral == kEmber &&
                            !h0.fill && h0.label == kEmber && h0.numeral == kEmber && s100.arc == kBone && s100.glow == kSatGlow && s100.numeral == kCream &&
                            s20.arc == kEmber && s20.numeral == kEmber;
            Check("hud ring: colours per state (HP amber / SAT bone with cream numerals; 25 and under ember for arc, glow, label and numeral; 26 not; 0 draws no fill "
                  "with an ember label and numeral)",
                  ok, F("h25 arc %08x numeral %08x, h0 fill %d", (unsigned)h25.arc, (unsigned)h25.numeral, (int)h0.fill));
        }
        // B. the rules
        {
            const float nan = std::nanf("");
            const bool ok = Value(1.0f) == 100 && Value(0.0f) == 0 && Value(0.824f) == 82 && Value(0.825f) == 83 && Value(1.3f) == 100 && Value(-0.2f) == 0 &&
                            Value(nan) == 0 && Value(0.004f) == 0 && Value(0.005f) == 1;
            Check("hud ring: the shown value is clamp(round(v), 0, 100) of the fraction (NaN = 0; 0.4 % shows 0, 0.5 % shows 1)", ok,
                  F("0.825 -> %d, 1.3 -> %d, 0.005 -> %d", Value(0.825f), Value(1.3f), Value(0.005f)));
        }
        {
            const bool ok = Low(25) && !Low(26) && Low(0) && Critical(10) && !Critical(11) && Critical(1) && !Critical(0);
            Check("hud ring: low is v <= 25, critical (the pulse) is 1 <= v <= 10; 0 is low but never pulses", ok, "");
        }
        // C. the pulse
        {
            const bool corners = Near(PulseOpacity(0.0), 1.0f, 1e-5f) && Near(PulseOpacity(800.0), 0.4f, 1e-4f) && Near(PulseOpacity(1600.0), 1.0f, 1e-4f) &&
                                 Near(PulseOpacity(400.0), 0.7f, 1e-3f) && Near(PulseOpacity(1200.0), 0.7f, 1e-3f) && Near(PulseOpacity(3200.0 + 800.0), 0.4f, 1e-4f);
            Check("hud ring: the pulse's closed form: 1.0 at 0, 0.4 at 800 ms, 1.0 at 1600 ms (period 1600), 0.7 half way down and half way up", corners,
                  F("400 ms %.4f, 800 ms %.4f, 1200 ms %.4f", PulseOpacity(400.0), PulseOpacity(800.0), PulseOpacity(1200.0)));
        }
        {
            // cubic-bezier(0.42, 0, 0.58, 1): symmetric about (0.5, 0.5), slow at both ends (ease-in-out, not linear), monotone
            bool mono = true;
            float prev = -1.0f;
            for (int i = 0; i <= 100; ++i)
            {
                const float e = EaseInOut((float)i / 100.0f);
                mono = mono && e >= prev - 1e-6f;
                prev = e;
            }
            const float e25 = EaseInOut(0.25f), e75 = EaseInOut(0.75f);
            const bool ok = mono && Near(EaseInOut(0.5f), 0.5f, 1e-4f) && Near(e25 + e75, 1.0f, 1e-4f) && e25 < 0.2f && e25 > 0.1f && EaseInOut(0.0f) == 0.0f &&
                            EaseInOut(1.0f) == 1.0f;
            Check("hud ring: the ease is cubic-bezier(0.42, 0, 0.58, 1): monotone, 0.5 at the middle, symmetric, slow at the ends (0.25 -> about 0.13)", ok,
                  F("ease(0.25) %.4f, ease(0.75) %.4f", e25, e75));
        }
        {
            // the phase restarts at 1.0 when the gauge ENTERS critical; leaving resets it; a gauge that stays critical runs on
            PulseClock pc;
            const float a = pc.Step(true, 5000.0);          // enters: 1.0
            const float b = pc.Step(true, 5800.0);          // 800 ms in: 0.4
            const float c = pc.Step(false, 5900.0);         // leaves: 1.0
            const float d = pc.Step(true, 6000.0);          // enters again: restarts at 1.0 (not at 0.4 + 200 ms)
            const float e = pc.Step(true, 6400.0);          // 400 ms in: 0.7
            const bool ok = Near(a, 1.0f, 1e-5f) && Near(b, 0.4f, 1e-4f) && Near(c, 1.0f, 1e-5f) && Near(d, 1.0f, 1e-5f) && Near(e, 0.7f, 1e-3f);
            Check("hud ring: the pulse restarts at opacity 1.0 each time the gauge enters critical, and reads 1.0 outside it", ok,
                  F("enter %.3f, +800 %.3f, out %.3f, re-enter %.3f, +400 %.3f", a, b, c, d, e));
        }
        // D. other displays: H / 1080, origin bottom-left (the component keeps 56 px * k from the left and the bottom)
        {
            const Pt c1440 = ToScreen({kCx, kCy}, 1440.0f), b720 = ToScreen({kCompX, kCompY}, 720.0f), c1080 = ToScreen({kCx, kCy}, 1080.0f);
            const bool ok = At(c1080, 200.0f, 816.0f) && At(c1440, 266.6667f, 1440.0f - 352.0f) && At(b720, 37.3333f, 720.0f - 37.3333f) &&
                            Near(Scale(1440.0f), 1.3333333f, 1e-5f) && Scale(0.0f) == 1.0f;
            Check("hud ring: screen placement is the design point times H / 1080 from the bottom-left (the centre at (200, 816) on 1080p, (266.67, 1088) on 1440p)",
                  ok, F("1440 centre (%.2f, %.2f), 720 corner (%.2f, %.2f)", c1440.x, c1440.y, b720.x, b720.y));
        }
    }

} // namespace hudring_t
