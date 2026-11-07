// 0.5.9 seatbelt sound (GI 2026-11-07: "add a sound for fastening unfastening a seatbelt with B no need for a menu since speedo shows all").
// Pure C++17, header-only, no OS calls: which vehicle-controls event clicks, the switches, and the two sounds themselves, synthesized in code (no
// game audio, no third-party asset). The engine plays the bytes BuildWav makes on its own worker thread (apps/engine/belt_audio.{h,cpp}).
// Tested offline in tests/belt_sound_tests.h (gameplay_test).
//
// WHEN IT CLICKS: only the player's own B press, i.e. VcEv::Belt from VehControls::Press (1 = buckled -> Buckle, 0 = unbuckled -> Unbuckle).
// Never: the flag re-assert (VcEv::BeltReassert, the game moved PED config flag 32), the automatic unbuckle when the exit task starts (F,
// VcEv::BeltAuto: the player did not press B, and GTA's own exit animation is what they watch), leaving / being pulled from the car (VcEv::Left),
// sitting in (VcEv::Entered sets the flag to "unbuckled" silently), a B press the machine refused (chat / pause menu / not in a car). The machine
// takes at most one B press per tick (presses are a bit mask of fresh edges), so a tick yields at most one sound.
//
// THE SOUNDS (one table, kBuckle / kUnbuckle below; GI's "sharper" / "longer" / "more metal" is a one-line change there, "louder" is
// belt_sound_vol): each sound is a sum of layers placed in time:
//   Click  a white-noise burst through a one-pole high-pass, decaying with time constant timeMs: the broadband snap of metal on metal / plastic
//   Ring   a damped sine (frequency hz, decay time constant timeMs): one resonance of the latch plate or the plastic housing. Several inharmonic
//          rings at one moment are what makes a click "metallic" instead of a beep (a beep is one harmonic tone with a slow envelope)
//   Slide  white noise through a band-pass (hz .. hz2), a raised-cosine attack and a falling tail over timeMs, modulated by a random grain
//          (kSlideGrainHz): the tongue scraping into the buckle, or the webbing running back through the pillar loop
// The sum is peak-normalised to level x volume x kPeakFull (no clipping by construction), then a short fade-out ends it at zero.
#pragma once

#include "sync/vehicle_controls.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gage::sync::beltsound {

    // ---- the decision ---------------------------------------------------------------------------------------------------
    enum class Sound : uint8_t { None = 0, Buckle, Unbuckle };

    inline const char* Name(Sound s)
    {
        return s == Sound::Buckle ? "buckle" : (s == Sound::Unbuckle ? "unbuckle" : "none");
    }

    // The sound of one vehicle-controls event: only the player's own B press makes one.
    inline Sound ForEvent(const VcEvent& e)
    {
        if (e.kind != VcEv::Belt)
            return Sound::None;
        return e.a ? Sound::Buckle : Sound::Unbuckle;
    }

    // The sound of one tick: the last B press of the tick (the machine takes at most one), None when the tick holds none.
    inline Sound ForTick(const VehControlsOut& o)
    {
        Sound s = Sound::None;
        for (int i = 0; i < o.nev && i < VehControlsOut::kMaxEvents; ++i)
        {
            const Sound e = ForEvent(o.ev[i]);
            if (e != Sound::None)
                s = e;
        }
        return s;
    }

    // ---- the switches (HKCU\Software\GAGE-MP) ------------------------------------------------------------------------------
    // belt_sound: unset / anything but "0" = on. belt_sound_vol: 0..100 (a linear amplitude percentage of kPeakFull), unset = kVolDefault.
    constexpr int kVolDefault = 60, kVolMax = 100;

    inline bool ParseOn(const std::wstring& v) { return v != L"0"; }

    struct Vol
    {
        int pct = kVolDefault;
        const char* why = nullptr;   // null = as asked (or unset)
    };
    inline Vol ParseVol(const std::wstring& v)
    {
        Vol r;
        size_t b = 0, e = v.size();
        while (b < e && (v[b] == L' ' || v[b] == L'\t')) ++b;
        while (e > b && (v[e - 1] == L' ' || v[e - 1] == L'\t')) --e;
        if (b == e)
            return r;
        long x = 0;
        for (size_t i = b; i < e; ++i)
        {
            if (v[i] < L'0' || v[i] > L'9' || i - b >= 6)
            {
                r.why = "; belt_sound_vol unreadable (a whole number 0..100), the default taken";
                return r;
            }
            x = x * 10 + (long)(v[i] - L'0');
        }
        if (x > kVolMax)
        {
            r.pct = kVolMax;
            r.why = "; belt_sound_vol above 100, held to 100";
            return r;
        }
        r.pct = (int)x;
        return r;
    }

    // ---- the synthesis table ----------------------------------------------------------------------------------------------
    constexpr int   kRate = 44100;               // mono, 16-bit PCM
    constexpr float kPeakFull = 0.89f;           // volume 100 and level 1.0 peak at -1 dBFS
    constexpr float kAttackMs = 0.2f;            // every Click / Ring layer rises over this (a hard 1-sample step aliases into a harsh tick)
    constexpr float kFadeOutMs = 4.0f;           // the last few ms of the sound fade to exactly 0 (the longest layer is cut there)
    constexpr float kSlideAttack = 0.18f;        // a Slide's raised-cosine attack, as a fraction of its length
    constexpr float kSlideGrainHz = 70.0f;       // a Slide's random amplitude grain (the webbing's weave over the loop)
    constexpr float kSlideGrainDepth = 0.45f;    // ... the grain dips down to 1 - depth
    constexpr int   kMaxLayers = 10;

    enum class LayerKind : uint8_t { Click, Ring, Slide };
    struct Layer
    {
        LayerKind kind;
        float atMs;     // when it starts, from the start of the sound
        float timeMs;   // Click / Ring: the decay time constant (1/e); Slide: its whole length
        float hz;       // Click: the high-pass corner; Ring: the frequency; Slide: the low band edge
        float hz2;      // Slide: the high band edge (unused otherwise)
        float gain;     // linear, relative to the other layers of the same sound (the sum is peak-normalised)
    };
    struct Recipe
    {
        const char* name;
        float       level;     // the peak relative to the other sound (unbuckle is the lighter one)
        uint32_t    seed;      // the noise generator's seed: the bytes are identical every run
        int         n;
        Layer       layers[kMaxLayers];
    };

    // BUCKLE: the tongue scrapes into the buckle, the latch snaps (a bright metal plate rung at ~3.2 / 5.0 / 6.7 kHz, inharmonic ratios
    // 1 : 1.59 : 2.13, plus a short ~950 Hz knock of the plastic housing), and 7 ms later the tongue seats with a second, smaller click.
    // Audible (above -40 dB of the peak) for about 90 ms.
    constexpr Recipe kBuckle = { "buckle", 1.0f, 0x5eb1u, 9, {
        { LayerKind::Slide, 0.0f, 22.0f, 1800.0f, 6500.0f, 0.18f },   // the tongue sliding in
        { LayerKind::Click, 22.0f, 1.1f, 2000.0f, 0.0f, 1.00f },      // the latch: the snap
        { LayerKind::Ring, 22.0f, 14.0f, 3150.0f, 0.0f, 0.55f },      // the latch plate, first mode
        { LayerKind::Ring, 22.0f, 7.0f, 5010.0f, 0.0f, 0.30f },       // second mode
        { LayerKind::Ring, 22.0f, 4.0f, 6720.0f, 0.0f, 0.18f },       // third mode
        { LayerKind::Ring, 22.0f, 6.0f, 950.0f, 0.0f, 0.30f },        // the plastic housing's knock
        { LayerKind::Click, 29.0f, 0.7f, 2500.0f, 0.0f, 0.50f },      // the tongue seating: the second click
        { LayerKind::Ring, 29.0f, 9.0f, 2620.0f, 0.0f, 0.22f },       // ... its ring
        { LayerKind::Ring, 29.0f, 5.0f, 4380.0f, 0.0f, 0.12f },
    } };

    // UNBUCKLE: the thumb presses the release button (a soft, lower plastic click), the spring kicks the tongue out 9 ms later (a lighter metal
    // click), then the webbing runs back through the pillar loop (a band-passed slide, about 0.2 s). Audible for about 240 ms; peaks lower.
    constexpr Recipe kUnbuckle = { "unbuckle", 0.75f, 0x0b1eu, 8, {
        { LayerKind::Click, 0.0f, 1.5f, 800.0f, 0.0f, 0.45f },        // the release button
        { LayerKind::Ring, 0.0f, 7.0f, 1300.0f, 0.0f, 0.30f },        // ... the plastic's ring
        { LayerKind::Click, 9.0f, 1.0f, 1800.0f, 0.0f, 0.75f },       // the spring kicks the tongue out
        { LayerKind::Ring, 9.0f, 10.0f, 2400.0f, 0.0f, 0.40f },       // the tongue plate, first mode
        { LayerKind::Ring, 9.0f, 6.0f, 3890.0f, 0.0f, 0.22f },        // second mode
        { LayerKind::Ring, 9.0f, 3.5f, 5300.0f, 0.0f, 0.10f },        // third mode
        { LayerKind::Slide, 16.0f, 215.0f, 700.0f, 4000.0f, 0.26f },  // the webbing running back
        { LayerKind::Click, 230.0f, 0.8f, 1500.0f, 0.0f, 0.08f },     // the tongue's stop button reaching the loop
    } };

    inline const Recipe& RecipeFor(Sound s) { return s == Sound::Unbuckle ? kUnbuckle : kBuckle; }

    // ---- the synthesis -----------------------------------------------------------------------------------------------------
    struct Noise
    {
        uint32_t s;
        explicit Noise(uint32_t seed) : s(seed ? seed : 1u) {}
        float Next()   // uniform in [-1, 1)
        {
            s = s * 1664525u + 1013904223u;
            return (float)(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
        }
    };

    inline int MsToSamples(float ms) { return (int)std::lround((double)ms * kRate / 1000.0); }

    // Where a layer ends (its contribution is below -60 dB of its own start after 7 time constants).
    inline float LayerEndMs(const Layer& l)
    {
        return l.kind == LayerKind::Slide ? l.atMs + l.timeMs : l.atMs + 7.0f * l.timeMs;
    }

    // The sound as floats (before the volume): peak-normalised to 1.0 x level, faded to 0 at the end. Deterministic.
    inline std::vector<float> SynthFloat(const Recipe& r)
    {
        const double kPi = 3.14159265358979323846;
        float endMs = 0.0f;
        for (int i = 0; i < r.n && i < kMaxLayers; ++i)
            endMs = std::fmax(endMs, LayerEndMs(r.layers[i]));
        const int n = MsToSamples(endMs);   // the last layer is cut at its end (-61 dB): the fade below takes that step to exactly 0
        std::vector<float> out((size_t)(n > 0 ? n : 0), 0.0f);
        Noise noise(r.seed);
        const double dt = 1.0 / kRate;
        const int attackN = MsToSamples(kAttackMs) > 0 ? MsToSamples(kAttackMs) : 1;
        for (int li = 0; li < r.n && li < kMaxLayers; ++li)
        {
            const Layer& l = r.layers[li];
            const int start = MsToSamples(l.atMs);
            const int end = MsToSamples(LayerEndMs(l)) < n ? MsToSamples(LayerEndMs(l)) : n;
            if (l.kind == LayerKind::Click)
            {
                const double rc = 1.0 / (2.0 * kPi * (double)l.hz);
                const double a = rc / (rc + dt);
                double xPrev = 0.0, y = 0.0;
                for (int i = start; i < end; ++i)
                {
                    const double t = (i - start) * dt * 1000.0;
                    const double env = std::exp(-t / (double)l.timeMs) * (i - start < attackN ? (double)(i - start + 1) / attackN : 1.0);
                    const double x = noise.Next() * env;
                    y = a * (y + x - xPrev);
                    xPrev = x;
                    out[(size_t)i] += (float)(y * l.gain);
                }
            }
            else if (l.kind == LayerKind::Ring)
            {
                const double w = 2.0 * kPi * (double)l.hz;
                for (int i = start; i < end; ++i)
                {
                    const double t = (i - start) * dt;
                    const double env = std::exp(-t * 1000.0 / (double)l.timeMs) * (i - start < attackN ? (double)(i - start + 1) / attackN : 1.0);
                    out[(size_t)i] += (float)(std::sin(w * t) * env * l.gain);
                }
            }
            else
            {
                // RBJ band-pass (constant 0 dB peak gain) centred on the geometric mean of the edges, bandwidth in octaves.
                const double f0 = std::sqrt((double)l.hz * (double)l.hz2);
                const double bw = std::log2((double)l.hz2 / (double)l.hz);
                const double w0 = 2.0 * kPi * f0 / kRate;
                const double alpha = std::sin(w0) * std::sinh(std::log(2.0) / 2.0 * bw * w0 / std::sin(w0));
                const double a0 = 1.0 + alpha;
                const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2.0 * std::cos(w0) / a0, a2 = (1.0 - alpha) / a0;
                double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
                const int len = end - start;
                const int grainN = MsToSamples(1000.0f / kSlideGrainHz) > 0 ? MsToSamples(1000.0f / kSlideGrainHz) : 1;
                float g0 = 1.0f, g1 = 1.0f - kSlideGrainDepth * (0.5f + 0.5f * noise.Next());
                for (int i = start; i < end; ++i)
                {
                    const int k = i - start;
                    if (k % grainN == 0)
                    {
                        g0 = g1;
                        g1 = 1.0f - kSlideGrainDepth * (0.5f + 0.5f * noise.Next());
                    }
                    const float grain = g0 + (g1 - g0) * (float)(k % grainN) / (float)grainN;
                    const double u = len > 1 ? (double)k / (double)(len - 1) : 1.0;
                    const double env = u < kSlideAttack ? 0.5 - 0.5 * std::cos(kPi * u / kSlideAttack)
                                                        : (1.0 - (u - kSlideAttack) / (1.0 - kSlideAttack)) * (1.0 - (u - kSlideAttack) / (1.0 - kSlideAttack));
                    const double x = noise.Next();
                    const double y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
                    x2 = x1; x1 = x; y2 = y1; y1 = y;
                    out[(size_t)i] += (float)(y * env * grain * l.gain);
                }
            }
        }
        // The fade-out, then the peak normalisation (to level).
        const int fadeN = MsToSamples(kFadeOutMs);
        for (int i = 0; i < fadeN && i < n; ++i)
            out[(size_t)(n - 1 - i)] *= (float)i / (float)fadeN;
        float peak = 0.0f;
        for (float v : out)
            peak = std::fmax(peak, std::fabs(v));
        if (peak > 0.0f)
            for (float& v : out)
                v = v / peak * r.level;
        return out;
    }

    // 16-bit samples at a volume (0..100): peak = level x kPeakFull x vol / 100 of full scale.
    inline std::vector<int16_t> Synth(const Recipe& r, int volPct)
    {
        if (volPct < 0) volPct = 0;
        if (volPct > kVolMax) volPct = kVolMax;
        const std::vector<float> f = SynthFloat(r);
        std::vector<int16_t> s(f.size());
        const double scale = 32767.0 * kPeakFull * volPct / 100.0;
        for (size_t i = 0; i < f.size(); ++i)
        {
            const double v = std::lround(f[i] * scale);
            s[i] = (int16_t)(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
        }
        return s;
    }

    // A RIFF / WAVE image (PCM, mono, 16-bit, kRate): the bytes PlaySound(SND_MEMORY) takes.
    inline std::vector<uint8_t> BuildWav(const std::vector<int16_t>& s)
    {
        const uint32_t dataBytes = (uint32_t)(s.size() * 2u);
        std::vector<uint8_t> w(44u + dataBytes);
        auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) w[at + i] = (uint8_t)(v >> (8 * i)); };
        auto put16 = [&](size_t at, uint16_t v) { w[at] = (uint8_t)v; w[at + 1] = (uint8_t)(v >> 8); };
        std::memcpy(&w[0], "RIFF", 4);
        put32(4, 36u + dataBytes);
        std::memcpy(&w[8], "WAVEfmt ", 8);
        put32(16, 16);                       // fmt chunk size
        put16(20, 1);                        // WAVE_FORMAT_PCM
        put16(22, 1);                        // mono
        put32(24, (uint32_t)kRate);
        put32(28, (uint32_t)kRate * 2u);     // bytes per second
        put16(32, 2);                        // block align
        put16(34, 16);                       // bits per sample
        std::memcpy(&w[36], "data", 4);
        put32(40, dataBytes);
        for (size_t i = 0; i < s.size(); ++i)
            put16(44 + 2 * i, (uint16_t)s[i]);
        return w;
    }

    inline std::vector<uint8_t> BuildWav(Sound s, int volPct) { return BuildWav(Synth(RecipeFor(s), volPct)); }

}
