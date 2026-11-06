// hud_ring.h (0.4.31, hud_ring): the vitals ring, GI's REV1 design (HUD_DESIGN_COMPONENTS/REV1/BUILD_SPEC.md, "SOLAR RP HUD: Vitals + Minimap",
// the PNG goldens in REV1/png). Pure C++17, no ImGui: the numbers, the state rules and the pulse, so tests/hud_ring_tests.h pins them against the
// spec's own state table; apps/engine/overlay_ui.cpp DrawVitalsRing draws from them.
//
// The spec's conventions, kept here as written: a 1920 x 1080 design, origin at the screen's BOTTOM-LEFT, y UP, angles in degrees counter-clockwise
// from +x. Every other resolution scales everything by screenHeight / 1080 (ToScreen). GI's call for REV1 (2026-11-06): the design's exact place and
// size with GTA's radar hidden, i.e. the spec's "map off" state (the map circle, its ring, vignette and marks are not drawn): GTA's radar cannot be
// moved into the circle yet (libs/sync/minimap_size.h: the 0.4.25-0.4.27 table writes never moved the drawn radar).
#pragma once

#include <cmath>
#include <cstdint>

namespace gage::sync::hudring {

    // ---- geometry (design px, y up) -------------------------------------------------------------------------------------------------
    constexpr float kDesignH = 1080.0f;
    constexpr float kCompX = 56.0f, kCompY = 56.0f, kCompW = 288.0f, kCompH = 352.0f;   // the component box (bottom-left corner + size)
    constexpr float kCx = 200.0f, kCy = 264.0f;                                          // the disc and the arcs share this centre
    constexpr float kDiscR = 140.0f;                                                     // 3.1 glass disc
    constexpr float kArcR = 130.0f, kArcW = 10.0f;                                       // 3.2 / 3.3 tracks and fills (round caps, cap radius 5)
    constexpr float kSweepDeg = 140.0f;                                                  // each track
    constexpr float kHpStartDeg = 250.0f;                                                // HP fills from its bottom end (a 250) towards a 110
    constexpr float kSatStartDeg = -70.0f;                                               // SAT fills from its bottom end (a -70) towards a 70
    // 3.3 glow: the fill blurred with sigma 4.5 (CSS 9 px) in the glow colour, under the crisp arc. The overlay draws the blur's own falloff as a
    // mesh (overlay_ui.cpp GlowProfile), not the spec's two-stroke fallback (width 14 at 28 %, 22 at 12 %), which read as hard bands next to the goldens.
    // The spec writes "sigma 4.5, CSS 9 px", but CSS filter: drop-shadow(0 0 9px) IS a Gaussian of standard deviation 9 (Filter Effects 1: feGaussianBlur
    // stdDeviation = the radius; only box-shadow halves it), and the goldens were rendered that way: the critic measured the golden's falloff (glow
    // alpha 0.17 / 0.09 / 0.04 at 6 / 10 / 14 px past the stroke) as a sigma of about 9. So 9, the goldens being the truth.
    constexpr float kGlowSigma = 9.0f;
    // 3.5 the strip: a pill (corner radius 30), 1 px border inside the box
    constexpr float kStripX = 56.0f, kStripY = 56.0f, kStripW = 288.0f, kStripH = 60.0f, kStripRound = 30.0f;
    constexpr float kTextBaseY = 77.0f;                                                  // every label and numeral sits on this baseline
    constexpr float kHpLabelX = 83.19f, kHpNumX = 113.31f, kSatLabelX = 225.11f, kSatNumX = 262.80f;
    constexpr float kNumSlotW = 54.0f;                                                   // numerals left aligned in this slot ("100" overruns it, as designed)
    constexpr float kDividerX = 195.70f, kDividerY = 73.0f, kDividerW = 1.0f, kDividerH = 26.0f;
    constexpr float kLabelPx = 13.0f, kNumPx = 32.0f, kLabelTrackPx = 2.08f;             // Sofia Sans Bold; the label's letter-spacing (also after the last letter)
    constexpr float kTabularEm = 60.5f / 3.0f / 32.0f;                                   // one tabular digit's advance ("100" is 60.5 px wide at 32 px)

    // ---- colours (#RRGGBBAA, straight alpha) ----------------------------------------------------------------------------------------
    constexpr uint32_t kGround = 0x0A0907FFu, kAmber = 0xF2B134FFu, kBone = 0xD9D1C3FFu, kCream = 0xF7F2E8FFu, kEmber = 0xE5603AFFu;
    // Disc glass: the spec's #0A0907A8 assumes a backdrop blur (sigma 14) ImGui does not have; its own note: "raise the fill alpha to about 0.74-0.80 if the
    // scene is bright". 0.4.31 first cut used 0.76 (0xC2) for GTA's daylight; on a dusk scene that read as an opaque black puck (critic), so 0.71
    // (0xB4) splits the two cases. The spec's value stays named for the record.
    constexpr uint32_t kDiscGlassSpec = 0x0A0907A8u, kDiscGlass = 0x0A0907B4u;
    constexpr uint32_t kStripGlass = 0x0A0907BDu, kArcTrack = 0xF7F2E829u, kStripBorder = 0xF7F2E824u, kStripDivider = 0xF7F2E82Eu;
    constexpr uint32_t kHpGlow = 0xF2B134CCu, kSatGlow = 0xD9D1C38Cu, kLowGlow = 0xE5603AD9u;

    // ---- values and states ----------------------------------------------------------------------------------------------------------
    // The integer a gauge shows: clamp(round(v), 0, 100) of the fraction times 100; NaN is 0.
    inline int Value(float frac)
    {
        if (!(frac == frac))
            return 0;
        const float v = std::floor(frac * 100.0f + 0.5f);
        return v < 0.0f ? 0 : (v > 100.0f ? 100 : (int)v);
    }
    inline bool Low(int v) { return v <= 25; }                     // arc, glow, label and numeral all ember
    inline bool Critical(int v) { return v >= 1 && v <= 10; }      // the fill arc pulses; 0 = no fill, no pulse

    // The angle (degrees, y up) where a fill ends at value v.
    inline float HpEndDeg(int v) { return kHpStartDeg - kSweepDeg / 100.0f * (float)v; }
    inline float SatEndDeg(int v) { return kSatStartDeg + kSweepDeg / 100.0f * (float)v; }

    struct Pt { float x, y; };
    constexpr float kPi = 3.14159265358979f;
    // A point on a circle about the component's centre (design px, y up).
    inline Pt OnArc(float deg, float r = kArcR)
    {
        const float a = deg * kPi / 180.0f;
        return { kCx + r * std::cos(a), kCy + r * std::sin(a) };
    }

    // 0.4.34 GI after the first live look (2026-11-06): "make map 25% smaller". The whole component (disc, arcs, strip, text) is drawn at kSize of
    // the design about its own bottom-left corner (kCompX, kCompY), so its 56 px margin to the screen corner stays as designed.
    // 0.5.1 GI after the 0.5.0 look: "make map and hp and satiety stuff 10% bigger": 0.75 x 1.1.
    constexpr float kSize = 0.825f;

    // Design px (y up) to screen px (y down) on a display H px tall. DesignScale = H / 1080 (the spec's rule); Scale = DesignScale * kSize, the factor
    // for every size inside the component.
    inline float DesignScale(float displayH) { return displayH > 0.0f ? displayH / kDesignH : 1.0f; }
    inline float Scale(float displayH) { return DesignScale(displayH) * kSize; }
    inline Pt ToScreen(Pt p, float displayH)
    {
        const float d = DesignScale(displayH), k = Scale(displayH);
        return { kCompX * d + (p.x - kCompX) * k, displayH - (kCompY * d + (p.y - kCompY) * k) };
    }

    // 0.5.2 ring_map (GI 2026-11-06: "do the real round map"): GTA's own radar is moved into the disc (libs/sync/minimap_size.h RingPlacement). Its
    // visible map is a SQUARE whose corners stay inside the disc (half-diagonal = disc radius - kMapCornerClear), so the map shows as that square's
    // inscribed circle and an opaque annulus out to the disc hides the corners. Round 1 of the round map: the circle is smaller than the spec's r 120
    // (about r 97 design px); the later rounds magnify GTA's square or give it a round mask.
    constexpr float kMapCornerClear = 3.0f;                 // design px between the square's corner and the disc's edge
    constexpr uint32_t kMapAnnulus = 0x0A0907F0u;           // the annulus over the square's corners: the ground at 94 % (the glass would show them)
    // 3.4 at the hole's edge, as specified: the vignette (transparent to 78 % of the radius, then to #0A09076B at the edge), the 1.5 px amber ring
    // just outside the map and its glow (sigma 11, outside only)
    constexpr float kMapVignetteFrom = 0.78f, kMapRingW = 1.5f, kMapGlowSigma = 11.0f;
    constexpr uint32_t kMapVignette = 0x0A09076Bu, kMapRing = 0xF2B134D9u, kMapRingGlow = 0xF2B1344Du;
    inline float MapSquarePx(float displayH) { return (kDiscR - kMapCornerClear) * Scale(displayH) * 1.41421356f; }
    inline float MapRadiusPx(float displayH) { return MapSquarePx(displayH) * 0.5f; }
    inline Pt MapCentrePx(float displayH) { return ToScreen({kCx, kCy}, displayH); }

    struct GaugeLook
    {
        uint32_t arc, glow, label, numeral;
        bool fill;      // anything to draw at all (v > 0)
    };
    // isHp: HP (amber) or SAT (bone). The spec's state table: low (v <= 25) = ember for all four; v == 0 draws no fill.
    inline GaugeLook Look(bool isHp, int v)
    {
        GaugeLook g;
        const bool low = Low(v);
        g.arc = low ? kEmber : (isHp ? kAmber : kBone);
        g.glow = low ? kLowGlow : (isHp ? kHpGlow : kSatGlow);
        g.label = g.arc;
        g.numeral = low ? kEmber : kCream;
        g.fill = v > 0;
        return g;
    }

    // ---- the critical pulse (5) -----------------------------------------------------------------------------------------------------
    // ease-in-out = cubic-bezier(0.42, 0, 0.58, 1): solve x(t) = u for t (x is monotone), return y(t).
    inline float EaseInOut(float u)
    {
        if (!(u > 0.0f))
            return 0.0f;
        if (u >= 1.0f)
            return 1.0f;
        const auto bx = [](float t) { const float s = 1.0f - t; return 3.0f * s * s * t * 0.42f + 3.0f * s * t * t * 0.58f + t * t * t; };
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 40; ++i)
        {
            const float m = 0.5f * (lo + hi);
            (bx(m) < u ? lo : hi) = m;
        }
        const float t = 0.5f * (lo + hi), s = 1.0f - t;
        return 3.0f * s * t * t + t * t * t;   // y(t) with control y's 0 and 1
    }
    constexpr double kPulseLegMs = 800.0, kPulsePeriodMs = 1600.0;
    constexpr float kPulseLow = 0.4f;
    // t = (now - start) mod 1600; u = t < 800 ? t / 800 : (1600 - t) / 800; opacity = 1 - 0.6 * ease(u)
    inline float PulseOpacity(double msSinceStart)
    {
        if (!(msSinceStart >= 0.0))
            return 1.0f;
        const double t = std::fmod(msSinceStart, kPulsePeriodMs);
        const float u = (float)(t < kPulseLegMs ? t / kPulseLegMs : (kPulsePeriodMs - t) / kPulseLegMs);
        return 1.0f - (1.0f - kPulseLow) * EaseInOut(u);
    }
    // Per gauge: the phase restarts at opacity 1.0 each time the gauge ENTERS the critical state.
    struct PulseClock
    {
        bool on = false;
        double startMs = 0.0;
        float Step(bool critical, double nowMs)
        {
            if (!critical)
            {
                on = false;
                return 1.0f;
            }
            if (!on)
            {
                on = true;
                startMs = nowMs;
            }
            return PulseOpacity(nowMs - startMs);
        }
    };

} // namespace gage::sync::hudring
