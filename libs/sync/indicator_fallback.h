// 0.4.27 indicator fallback (GI, live 0.4.26: "left / right indicators and hazards don't work on custom cars; they do on stock cars").
// Pure C++17, header-only, no game calls; the engine (apps/engine/engine.cpp TickIndicatorFallback) feeds it the bone reads, the model
// dimensions and the indicator bits, and draws what it names with DRAW_LIGHT_WITH_RANGE. Tested in tests/vehstate_row_tests.h.
//
// WHAT THE LIVE LOG AND THE PACK FILES SAY (2026-11-05):
//   * The call is not the problem: gage.log 20:05:32-20:05:45 (jesko, vehicle 1282) has 11 indicator changes and "SET_VEHICLE_INDICATOR_LIGHTS calls
//     22 (0 failed)"; the same keys on ZENTORNO a minute later blink.
//   * Every loose pack's carcols.meta (jesko 576, lp700r 924, m3gtr 943, rs6 933, bmwm5 900, mbc63s 9977, mbe420 9976, mbg63 9975, teslas 9967)
//     sets <frontIndicatorCorona> and <rearIndicatorCorona> to size 0 / intensity 0 and the <indicator> light to intensity 0.375-0.575 with a
//     2.5 m falloff (taycan has no <Lights> entry at all). So nothing the game draws for an indicator on these cars comes from the meta except a
//     faint 2.5 m point light: what blinks is the model's own vehicle_lightsemissive geometry, bound to the indicator bones.
//   * The bones (skeleton bone names and 16-bit tags read out of the .yft resources): jesko and rs6 have NO indicator_lf / indicator_rf (only the
//     rear pair); bmwm5, diabloSV, lp700r, m3gtr, mbc63s, mbe420, mbg63, taycan and teslas have all four with correct tags. Every model uses
//     vehicle_lightsemissive. So "missing bones" explains the front of two cars, not the report; whether the game binds the emissive parts of a
//     late-mounted pack model to those bones is what the per-model log line (bone indices as the GAME resolves them, the call results) settles.
// THE FIX, independent of which of those it is: on a car whose indicators the game does not show, draw our own blinking amber light at each lit
// corner every frame, for the driver and for every copy (the bits ride flags2 already). Corners come from the indicator bones when the game
// resolves them, else from the model's bounding box (GET_MODEL_DIMENSIONS). Switch: HKCU\Software\GAGE-MP veh_ind_fallback.
#pragma once

#include "protocol/protocol.h"

#include <cmath>
#include <cstdint>

namespace gage::sync {

    // veh_ind_fallback: "0" off; "2" every car (a comparison aid: stock cars then blink twice, theirs and ours); unset / anything else = pack cars
    // (models a mounted content pack carries).
    enum class IndFallbackMode
    {
        Off,
        PackCars,
        AllCars,
    };

    inline IndFallbackMode IndFallbackModeFrom(const wchar_t* reg)
    {
        if (reg && reg[0] == L'0' && reg[1] == 0)
            return IndFallbackMode::Off;
        if (reg && reg[0] == L'2' && reg[1] == 0)
            return IndFallbackMode::AllCars;
        return IndFallbackMode::PackCars;
    }

    inline const char* IndFallbackModeName(IndFallbackMode m)
    {
        switch (m)
        {
        case IndFallbackMode::Off: return "0 (off)";
        case IndFallbackMode::PackCars: return "1 (pack cars)";
        case IndFallbackMode::AllCars: return "2 (every car)";
        }
        return "?";
    }

    inline bool UseIndFallback(IndFallbackMode m, bool packModel)
    {
        return m == IndFallbackMode::AllCars || (m == IndFallbackMode::PackCars && packModel);
    }

    // ---- the blink ---------------------------------------------------------------------------------------------------
    // GTA's own indicator period is not readable (no getter in either crossmap) and has not been measured; this is the HUD arrows' 1.5 Hz (overlay
    // HudPhase), so the light on the car and the arrow on the speedometer beat at the same rate. On for the first half: a switch-on lights at once.
    constexpr double kIndPeriodMs = 1000.0 / 1.5;
    inline bool IndBlinkLit(double nowMs, double onSinceMs)
    {
        const double t = nowMs - onSinceMs;
        if (!(t >= 0.0))
            return true;                                   // a clock step backwards: lit, never stuck dark
        return std::fmod(t, kIndPeriodMs) < kIndPeriodMs * 0.5;
    }

    // ---- corners -----------------------------------------------------------------------------------------------------
    // Order everywhere: 0 left front, 1 left rear, 2 right front, 3 right rear = the bones indicator_lf, indicator_lr, indicator_rf, indicator_rr.
    constexpr int kIndCorners = 4;
    constexpr const char* kIndBoneNames[kIndCorners] = {"indicator_lf", "indicator_lr", "indicator_rf", "indicator_rr"};
    inline bool IndCornerIsLeft(int i) { return i == 0 || i == 1; }

    // The corners a state lights: left = 0 and 1, right = 2 and 3, hazards all four.
    inline unsigned IndCornerMask(uint8_t indBits)
    {
        unsigned m = 0;
        if (indBits & proto::kVeh2IndicatorL)
            m |= 1u | 2u;
        if (indBits & proto::kVeh2IndicatorR)
            m |= 4u | 8u;
        return m;
    }

    struct V3
    {
        float x = 0, y = 0, z = 0;
    };

    // Model-space corners from GET_MODEL_DIMENSIONS (x = right, y = forward, z = up, origin the model's): inset kIndSideInsetM from the sides,
    // kIndEndOutM beyond the front and rear faces (a point light inside the body lights nothing anyone sees), at kIndHeightFrac of the box's height.
    constexpr float kIndSideInsetM = 0.15f, kIndEndOutM = 0.10f, kIndHeightFrac = 0.40f;
    inline bool IndDimsOk(const V3& mn, const V3& mx)
    {
        const float w = mx.x - mn.x, l = mx.y - mn.y, h = mx.z - mn.z;
        return w > 0.5f && w < 6.0f && l > 1.0f && l < 30.0f && h > 0.3f && h < 8.0f;   // a car-sized box (a zero / garbage read is refused)
    }
    inline V3 IndCornerFromDims(int i, const V3& mn, const V3& mx)
    {
        V3 o;
        o.x = IndCornerIsLeft(i) ? mn.x + kIndSideInsetM : mx.x - kIndSideInsetM;
        o.y = (i == 0 || i == 2) ? mx.y + kIndEndOutM : mn.y - kIndEndOutM;
        o.z = mn.z + (mx.z - mn.z) * kIndHeightFrac;
        return o;
    }

    // A bone's world position pushed kIndEndOutM further from the car's centre, horizontally (the bone sits inside the lamp).
    inline V3 IndPushOut(const V3& bone, const V3& centre, float dist = kIndEndOutM)
    {
        const float dx = bone.x - centre.x, dy = bone.y - centre.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        V3 o = bone;
        if (d > 0.01f)
        {
            o.x += dx / d * dist;
            o.y += dy / d * dist;
        }
        return o;
    }

    // The light: the packs' own indicator colour (carcols <indicator><color> 0xFFFF8000), a short range so it reads as a lamp on the bodywork, not a
    // floodlight on the road. Intensity and range are not live-tuned (INFERRED); the per-model log line names them.
    constexpr int   kIndR = 255, kIndG = 128, kIndB = 0;
    constexpr float kIndRangeM = 1.8f;
    constexpr float kIndIntensity = 5.0f;

    // ---- per car: when its indicator state last changed (the blink's phase) -------------------------------------------------------------------
    // Keyed by the entity handle; entries not touched since the previous Sweep go. Fixed capacity: a car past it is not drawn (counted).
    class IndPhaseTable
    {
    public:
        static constexpr int kCap = 32;
        // The time the car's current state began (nowMs when it is new or changed). -1 when the table is full.
        double Touch(int32_t handle, uint8_t ind, double nowMs)
        {
            ind = (uint8_t)(ind & (proto::kVeh2IndicatorL | proto::kVeh2IndicatorR));
            for (Entry& e : e_)
                if (e.used && e.handle == handle)
                {
                    if (e.ind != ind)
                    {
                        e.ind = ind;
                        e.sinceMs = nowMs;
                    }
                    e.touched = true;
                    return e.sinceMs;
                }
            for (Entry& e : e_)
                if (!e.used)
                {
                    e = Entry{true, true, handle, ind, nowMs};
                    return nowMs;
                }
            ++full_;
            return -1.0;
        }
        void Sweep()
        {
            for (Entry& e : e_)
            {
                if (e.used && !e.touched)
                    e.used = false;
                e.touched = false;
            }
        }
        int Used() const
        {
            int n = 0;
            for (const Entry& e : e_)
                n += e.used ? 1 : 0;
            return n;
        }
        unsigned TakeFull()
        {
            const unsigned f = full_;
            full_ = 0;
            return f;
        }

    private:
        struct Entry
        {
            bool    used = false, touched = false;
            int32_t handle = 0;
            uint8_t ind = 0;
            double  sinceMs = 0.0;
        };
        Entry    e_[kCap];
        unsigned full_ = 0;
    };

    // How far from the local player a car's fallback is drawn (a lamp is a few pixels beyond this).
    constexpr float kIndDrawRangeM = 150.0f;

} // namespace gage::sync
