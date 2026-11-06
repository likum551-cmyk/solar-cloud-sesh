// hud_car.h (0.4.31, hud_ring): the car cluster, GI's REV2 design (HUD_DESIGN_COMPONENTS/REV2/BUILD_SPEC_CAR.md, "SOLAR RP HUD: Car cluster",
// the PNG goldens in REV2/png). Pure C++17, no ImGui: the numbers, the state rules, the blink and the pulse; tests/hud_car_tests.h pins them against the
// spec's tables, apps/engine/overlay_ui.cpp DrawCarCluster draws from them. Same conventions as hud_ring.h: a 1920 x 1080 design, origin bottom-left,
// y up, degrees counter-clockwise from +x, everything scaled by H / 1080 -- but this component is anchored to the bottom-RIGHT corner (56 px from the
// right and the bottom), so on a display that is not 16:9 its x is measured from the right edge (ToScreen).
#pragma once

#include "sync/hud_ring.h"   // PulseOpacity / PulseClock / EaseInOut / Value-style helpers and the shared colour tokens

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace gage::sync::hudcar {

    using hudring::Pt;
    using hudring::kPi;

    // ---- geometry (design px, y up) -------------------------------------------------------------------------------------------------
    constexpr float kDesignW = 1920.0f, kDesignH = 1080.0f;
    constexpr float kCompX = 1504.0f, kCompY = 56.0f, kCompW = 360.0f, kCompH = 432.0f;
    constexpr float kCx = 1684.0f, kCy = 308.0f;                         // the dial centre
    constexpr float kDiscR = 176.0f;
    constexpr float kArcR = 164.0f, kArcW = 12.0f;                       // speed and fuel arcs
    constexpr float kSpeedMax = 500.0f;
    constexpr float kSpeedStartDeg = 220.0f, kSpeedSweepDeg = 260.0f;    // 220 -> -40, clockwise on screen
    constexpr float kRedFromDeg = 12.0f;                                 // the redline track: 400..500 = a 12 -> -40
    constexpr float kRedline = 400.0f;                                   // above this (not at) the fill, needle and numeral go ember
    constexpr float kSpeedGlowSigma = 10.0f, kFuelGlowSigma = 9.0f;   // drop-shadow R = sigma R (see hud_ring.h kGlowSigma), not the spec's R / 2
    constexpr float kFuelStartDeg = 235.0f, kFuelSweepDeg = 70.0f;       // 235 -> 305, counter-clockwise on screen, E to F
    constexpr int   kTicks = 21;
    constexpr float kTickStepDeg = 13.0f;
    constexpr float kTickOuterR = 148.0f, kTickMajorInnerR = 132.0f, kTickMinorInnerR = 141.0f;
    constexpr float kTickMajorW = 2.4f, kTickMinorW = 1.6f;
    constexpr float kDialNumR = 114.0f, kDialNumPx = 14.0f, kDialNumBaseDrop = 4.5f;
    constexpr float kNeedleR = 140.0f, kNeedleLen = 28.0f, kNeedleW = 4.0f, kNeedleGlowSigma = 5.0f;   // spec 3.7 "glow 0 0 10px (sigma 5)"; the golden fits 4.5-5 (the arcs fit sigma = R)
    // the speed readout
    constexpr float kSpeedPx = 96.0f, kSpeedDigitAdv = 58.5625f, kSpeedBaseY = 318.0f;
    constexpr float kSpeedTrack = -1.92f;   // the numeral's letter-spacing, included in kSpeedDigitAdv (CSS adds it after each glyph)
    constexpr float kUnitPx = 14.0f, kUnitTrack = 3.36f, kUnitX = 1661.69f, kUnitBaseY = 276.0f;
    constexpr float kEngineOffDim = 0.4f;
    // blinkers (bare 30 px arrows)
    constexpr float kArrowLX = 1611.0f, kArrowRX = 1727.0f, kArrowY = 265.0f, kArrowSize = 30.0f;
    // the fuel row
    constexpr float kRowX = 1604.0f, kRowW = 160.0f, kRowY = 208.0f, kRowH = 32.0f;
    constexpr float kPumpSize = 22.0f, kPumpGap = 8.0f, kPumpY = 213.0f;
    constexpr float kFuelPx = 26.0f, kFuelDigitAdv = 16.3906f, kPctGap = 2.0f, kPctPx = 14.0f, kPctAdv = 13.23f, kFuelBaseY = 217.0f;
    constexpr float kEFPx = 12.0f, kEFTrack = 1.2f, kEX = 1607.16f, kFX = 1753.30f, kEFBaseY = 189.0f;
    // the strip and its chips
    constexpr float kStripX = 1504.0f, kStripY = 56.0f, kStripW = 360.0f, kStripH = 64.0f, kStripRound = 32.0f;
    // kChipBorder: the spec says 1.5 px; Chrome draws a 1.5 css px border as 1 device px (the goldens' rim is 1 px)
    constexpr float kChipD = 46.0f, kChipY = 65.0f, kChipBorder = 1.0f, kChipGlowSigma = 8.0f;
    constexpr float kChipX[4] = {1568.0f, 1630.0f, 1692.0f, 1754.0f};    // lock, engine, seatbelt, headlights

    // ---- colours --------------------------------------------------------------------------------------------------------------------
    constexpr uint32_t kAmber = hudring::kAmber, kBone = hudring::kBone, kCream = hudring::kCream, kEmber = hudring::kEmber;
    constexpr uint32_t kMuted = 0xA9A295FFu;
    constexpr uint32_t kRedTrack = 0xE5603A61u;
    constexpr uint32_t kSpeedGlow = 0xF2B134CCu, kRedGlow = 0xE5603AD9u, kFuelGlow = 0xD9D1C38Cu, kLowGlow = 0xE5603AD9u;
    constexpr uint32_t kTickMajor = 0xF7F2E8D9u, kTickMinor = 0xF7F2E86Bu, kTickRedMajor = 0xE5603AF2u, kTickRedMinor = 0xE5603AA6u;
    constexpr uint32_t kDialNum = 0xF7F2E8D9u, kDialNumRed = 0xE5603AFFu;

    // ---- design to screen -----------------------------------------------------------------------------------------------------------
    // 0.4.34 GI after the first live look (2026-11-06): "make speedometer and its thing below about 50% smaller". The whole cluster (dial, fuel row,
    // strip, chips) is drawn at kSize of the design about its own bottom-right corner (kCompX + kCompW, kCompY), so the 56 px margins stay.
    // 0.5.1 GI after the 0.5.0 look: "make speedo about 20% bigger": 0.5 x 1.2.
    // 0.5.4 GI (2026-11-07): "enlarge speedometer and its things by 15%": 0.6 x 1.15. The Sofia faces and the icon glows are baked at kSize
    // (overlay_ui.cpp UiInit), so they follow; the anchor corner (56 px margins) does not move.
    constexpr float kSize = 0.69f;
    inline float DesignScale(float displayH) { return hudring::DesignScale(displayH); }
    inline float Scale(float displayH) { return DesignScale(displayH) * kSize; }
    // x is measured from the RIGHT edge (the component's anchor), y from the bottom.
    inline Pt ToScreen(Pt p, float displayW, float displayH)
    {
        const float d = DesignScale(displayH), k = Scale(displayH);
        const float ax = kCompX + kCompW;   // the anchor corner, 56 design px from the right edge
        return { displayW - (kDesignW - ax) * d + (p.x - ax) * k, displayH - (kCompY * d + (p.y - kCompY) * k) };
    }
    inline Pt OnDial(float deg, float r)
    {
        const float a = deg * kPi / 180.0f;
        return { kCx + r * std::cos(a), kCy + r * std::sin(a) };
    }

    // ---- values ---------------------------------------------------------------------------------------------------------------------
    inline int Speed(float kmh)
    {
        if (!(kmh == kmh))
            return 0;
        const float v = std::floor(kmh + 0.5f);
        return v < 0.0f ? 0 : (v > kSpeedMax ? (int)kSpeedMax : (int)v);
    }
    inline int Fuel(float frac) { return hudring::Value(frac); }
    inline float SpeedDeg(float v) { return kSpeedStartDeg - kSpeedSweepDeg / kSpeedMax * v; }   // 220 - 0.52 v
    inline float FuelDeg(int v) { return kFuelStartDeg + kFuelSweepDeg / 100.0f * (float)v; }    // 235 + 0.7 v
    inline bool Redline(int v) { return (float)v > kRedline; }
    inline bool FuelLow(int v) { return v <= 25; }
    inline bool FuelArcPulse(int v) { return v >= 1 && v <= 10; }

    // tick i (0..20): angle, major every 4th, red from 400 (i >= 16)
    inline float TickDeg(int i) { return kSpeedStartDeg - kTickStepDeg * (float)i; }
    inline bool TickMajor(int i) { return i % 4 == 0; }
    inline bool TickRed(int i) { return i >= 16; }

    // the speed readout: n digits, each 58.5625 wide, centred on x = 1684
    inline int Digits(int v) { int n = 1; for (int x = v; x >= 10; x /= 10) ++n; return n; }
    inline float SpeedRunX(int v) { return kCx - kSpeedDigitAdv * (float)Digits(v) / 2.0f; }

    // the fuel row: pump, 8 px, the value group (digits, 2 px, "%"), centred in the 160 px row
    struct FuelRow { float pumpX, numX, pctX; };
    inline FuelRow LayoutFuelRow(int v)
    {
        const float group = kFuelDigitAdv * (float)Digits(v) + kPctGap + kPctAdv;
        const float content = kPumpSize + kPumpGap + group;
        FuelRow r;
        r.pumpX = kRowX + (kRowW - content) / 2.0f;
        r.numX = r.pumpX + kPumpSize + kPumpGap;
        r.pctX = r.numX + kFuelDigitAdv * (float)Digits(v) + kPctGap;
        return r;
    }

    // ---- chips (section 4) ----------------------------------------------------------------------------------------------------------
    enum class Chip { Off, OnAmber, OnCalm, Warn };
    struct ChipLook
    {
        uint32_t icon, fill, border, glow;   // glow 0 = none
        float iconAlpha;
        uint32_t iconGlow;                   // 0 = none
        float iconGlowSigma;
    };
    inline ChipLook LookOf(Chip c, bool arrow = false)
    {
        switch (c)
        {
        case Chip::OnAmber: return {kAmber, 0xF2B13424u, 0xF2B13499u, 0xF2B13459u, 1.0f, 0xF2B134CCu, 5.0f};
        case Chip::OnCalm: return {kBone, 0xD9D1C31Au, 0xD9D1C34Du, 0u, 1.0f, 0xD9D1C366u, 4.0f};
        case Chip::Warn: return {kEmber, 0xE5603A29u, 0xE5603A99u, 0xE5603A59u, 1.0f, 0xE5603ABFu, 5.0f};
        case Chip::Off: break;
        }
        return {kMuted, 0xF7F2E80Du, 0xF7F2E81Fu, 0u, arrow ? 0.28f : 0.55f, 0u, 0.0f};
    }
    struct CarState
    {
        bool locked = false, engineOn = true, belted = true;
        int lights = 0;   // 0 off, 1 low, 2 high
    };
    inline Chip LockChip(const CarState& s) { return s.locked ? Chip::OnAmber : Chip::Off; }
    inline Chip EngineChip(const CarState& s) { return s.engineOn ? Chip::OnAmber : Chip::Off; }
    // buckled -> on (calm); unbuckled with the engine running -> warn (pulses); unbuckled with the engine stopped -> off
    inline Chip BeltChip(const CarState& s) { return s.belted ? Chip::OnCalm : (s.engineOn ? Chip::Warn : Chip::Off); }
    inline Chip LightsChip(const CarState& s) { return s.lights > 0 ? Chip::OnAmber : Chip::Off; }
    inline Chip PumpChip(int fuel) { return FuelLow(fuel) ? Chip::Warn : Chip::OnCalm; }

    // ---- the blink (section 7): period 800 ms, 1.0 to 392 ms, linear to 0.2 at 400 ms, 0.2 to 800 ms; restarts at 1.0 when switched on ---------
    constexpr double kBlinkPeriodMs = 800.0, kBlinkHoldMs = 392.0, kBlinkRampMs = 8.0;
    constexpr float kBlinkLow = 0.2f;
    inline float BlinkOpacity(double msSinceOn)
    {
        if (!(msSinceOn >= 0.0))
            return 1.0f;
        const double t = std::fmod(msSinceOn, kBlinkPeriodMs);
        if (t < kBlinkHoldMs)
            return 1.0f;
        if (t < kBlinkHoldMs + kBlinkRampMs)
            return 1.0f - (1.0f - kBlinkLow) * (float)((t - kBlinkHoldMs) / kBlinkRampMs);
        return kBlinkLow;
    }
    struct BlinkClock
    {
        bool on = false;
        double startMs = 0.0;
        // returns the arrow's opacity factor while on (the "off" look is separate: a different colour at 0.28)
        float Step(bool active, double nowMs)
        {
            if (!active) { on = false; return 1.0f; }
            if (!on) { on = true; startMs = nowMs; }
            return BlinkOpacity(nowMs - startMs);
        }
    };

} // namespace gage::sync::hudcar
