// ring_round.h (0.5.5 ring_map_round): the round mask for the ring's map. Pure C++17 (no D3D, no Windows): the radii, the radar-box fit, the gate
// that lets the map grow, the mask texture (analysis of the stock one, our circle, the BC1 / BC3 / BC4 / 8-bit codecs) and the blip-clip flag's pattern.
// tests/ring_round_tests.h (gameplay_test) pins it; apps/engine/radar_mask.cpp, minimap_size.cpp, engine.cpp and overlay_ui.cpp use it.
//
// Why (GI 2026-11-07, after 0.5.4: "fill out the current design HUD circle to the point where it doesn't go over hp and satiety and there's no gap
// either"): GTA's radar draws a SQUARE, so 0.5.2-0.5.4 kept the square's corners inside the disc and covered them with an opaque annulus; the map
// showed as the square's inscribed circle, about r 97 design px. A circle that fills the ring to the arcs (r 123.5) needs a square whose corners
// reach r 175, far past the disc, so the corners must not be drawn at all: GTA multiplies the radar by the texture radarmasksm
// (platform:/textures/graphics.ytd) stretched over the radar's box (nuikit PR #7; FiveM's circle-minimap resources replace exactly that texture
// through ADD_REPLACE_TEXTURE, a FiveM-only native). We do the same one layer down: apps/engine/radar_mask.cpp identifies the game's own mask
// texture at run time, builds a circle mask of the same size / format / mips, and the shared D3D11 bind hook (brand_apparel.cpp) hands the radar
// our view instead. Only while the radar is CONFIRMED to draw with the identified mask in recent frames does the placement grow, together with
// our mask being handed out (RoundDecide / RoundGate); otherwise 0.5.4 stays, the game's own mask included.
#pragma once

#include "sync/hud_ring.h"
#include "sync/minimap_size.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gage::sync::ringround {

    // ---- 1. radii (design px, hud_ring.h conventions) --------------------------------------------------------------------------------
    // The overlay's map hole: hudring::MapRoundRadiusPx (r 123.5 at 1080p and up: the 1.5 px amber ring ends ON the arcs' inner edge r 125).
    // What GTA draws: our mask's circle, inscribed in the radar's box, aimed midway between the hole and the disc's edge so a box-fit error of
    // up to about 6 % either way still leaves the map under the whole hole and inside the disc; the overlay's opaque annulus (hole -> disc) covers
    // the rest of the circle exactly as it covered 0.5.4's square corners.
    constexpr float kMaskCircleR = 131.75f;                   // (123.5 + 140) / 2

    // ---- 2. the radar box fit ----------------------------------------------------------------------------------------------------------
    // sync::RingPlacement maps the community "visible map" (H / 4 x H / 5.674 px) onto the asked square. The two 0.5.4 radar probes
    // (%LOCALAPPDATA%\GAGE-MP\radar_probe_1/2.bmp, GI 2026-11-07, 2560 x 1440, safe zone 0.97, asked a 213.1 px square centred at (233.07, 1136.53))
    // show the stock mask's 50 % contour at 185.5 / 182 x 191 / 193 px, centred within 1.5 px, and its soft ramp about 13 px wide on every side.
    // The mask covers the radar's whole box and fades to 0 at the box's edge, so the box is the contour plus one ramp: (0.925, 0.96) of the asked
    // square. A step-edged stock mask would make it the contour alone, (0.863, 0.899): the worst case, which kMaskCircleR's margin still covers
    // on the hole's side. HKCU ring_map_round_fit = "fx,fy" overrides (ParseBoxFit); the next probes show the new circle and pin it.
    constexpr float kBoxFitX = 0.925f, kBoxFitY = 0.96f;
    constexpr float kBoxFitLo = 0.70f, kBoxFitHi = 1.20f;     // a typed override outside this is refused

    struct BoxFit
    {
        float fx = kBoxFitX, fy = kBoxFitY;
        bool overridden = false;
        const char* why = nullptr;   // non-null: the value was refused (the default is used)
    };
    // "" -> the default; "0.93,0.95" (or "0.93 0.95", "0.93;0.95") -> those; anything else -> the default with a reason.
    inline BoxFit ParseBoxFit(const std::wstring& v)
    {
        BoxFit f;
        if (v.empty())
            return f;
        std::string s;
        for (wchar_t c : v)
            s += (c >= 0 && c < 128) ? (char)c : '?';
        float a = 0.0f, b = 0.0f;
        char sep = 0;
        int used = 0;
        if (std::sscanf(s.c_str(), " %f %c %f %n", &a, &sep, &b, &used) == 3 && (sep == ',' || sep == ';') && used == (int)s.size())
        {
        }
        else if (std::sscanf(s.c_str(), " %f %f %n", &a, &b, &used) == 2 && used == (int)s.size())
        {
        }
        else
        {
            f.why = " (unreadable: want \"fx,fy\")";
            return f;
        }
        if (!(a >= kBoxFitLo && a <= kBoxFitHi && b >= kBoxFitLo && b <= kBoxFitHi))
        {
            f.why = " (outside 0.70..1.20)";
            return f;
        }
        f.fx = a;
        f.fy = b;
        f.overridden = true;
        return f;
    }

    // The square to ASK RingPlacementBox for, per axis (screen px), so the radar's real box is a square of 2 * kMaskCircleR design px.
    struct BoxPx
    {
        float w = 0.0f, h = 0.0f;
    };
    inline BoxPx RoundAskPx(float displayH, const BoxFit& fit)
    {
        BoxPx b;
        const float d = 2.0f * kMaskCircleR * hudring::Scale(displayH);
        if (!(fit.fx > 0.0f) || !(fit.fy > 0.0f))
            return b;
        b.w = d / fit.fx;
        b.h = d / fit.fy;
        return b;
    }

    // ---- 3. the gate: grow only while the swap is confirmed ----------------------------------------------------------------------------
    enum class RoundWhy
    {
        Grow,            // the round mask is substituted in recent frames: the round placement and the big hole
        SwitchOff,       // ring_map_round=0 (or ring_map / hud_ring off): 0.5.4
        NotD3D11,        // Enhanced (D3D12) or no swap chain yet: the swap is D3D11 only
        NoMask,          // the minimap's mask is not identified / replaced (yet, or it failed)
        NoBigMask,       // ring_map_big mirrors the placement into the big radar, whose own mask (radarmasklg) is not replaced
        NotConfirmed,    // replaced, but the radar has bound the identified mask fewer than kConfirmBinds times so far
        Stale,           // the radar is drawn but has not bound the identified mask for kStaleMs (the game draws it with something else now)
    };
    inline const char* RoundWhyName(RoundWhy w)
    {
        switch (w)
        {
        case RoundWhy::Grow: return "GROWN (the radar binds the identified mask, ours is handed out)";
        case RoundWhy::SwitchOff: return "off (ring_map_round=0, or no ring map)";
        case RoundWhy::NotD3D11: return "not D3D11 (Enhanced / D3D12: skipped)";
        case RoundWhy::NoMask: return "no round mask (radarmasksm not identified or not replaced)";
        case RoundWhy::NoBigMask: return "big radar mask (radarmasklg) not replaced while ring_map_big mirrors the placement";
        case RoundWhy::NotConfirmed: return "replaced, the radar's binds of the mask not confirmed yet";
        case RoundWhy::Stale: return "STALE (the radar is drawn, but not with the identified mask lately)";
        }
        return "?";
    }

    constexpr uint64_t kConfirmBinds = 30;     // about half a second of radar frames
    constexpr double kShowGraceMs = 500.0;     // after the radar starts to show: the first binds arrive a frame or two later
    constexpr double kStaleMs = 300.0;         // radar drawn, no bind of the mask for this long = the game draws it with something else now

    // The evidence is the hook's count of the radar's binds of the STOCK view (swap::Counters seen), not of substitutions: the replacement is
    // handed out only while the map is grown (RoundGate), and the binds are what says the identified view is the one the radar really draws.
    struct RoundInputs
    {
        bool switchOn = false;       // ring_map_round && ring_map && hud_ring
        bool d3d11 = false;          // the swap layer's hooks are installed on a D3D11 device
        bool smallReplaced = false;  // radarmasksm: our view registered for the game's
        bool bigReplaced = false;    // radarmasklg: likewise
        bool bigMirror = false;      // ring_map_big
        uint64_t smallBinds = 0;     // the radar's binds of the minimap mask since it was registered
        double lastBindMs = -1.0;    // the last bind of either identified mask (same clock as nowMs; < 0 = never): the big radar draws with radarmasklg
        bool radarShown = false;     // DISPLAY_RADAR(true) this tick and not in the engine's own big-radar flip
        double shownSinceMs = 0.0;   // since when, continuously
        double nowMs = 0.0;
    };
    inline RoundWhy RoundDecide(const RoundInputs& in)
    {
        if (!in.switchOn)
            return RoundWhy::SwitchOff;
        if (!in.d3d11)
            return RoundWhy::NotD3D11;
        if (!in.smallReplaced)
            return RoundWhy::NoMask;
        if (in.bigMirror && !in.bigReplaced)
            return RoundWhy::NoBigMask;
        if (in.smallBinds < kConfirmBinds)
            return RoundWhy::NotConfirmed;
        if (in.radarShown)
        {
            // staleness counts from the later of the last bind and the end of the show grace
            double from = in.shownSinceMs + kShowGraceMs;
            if (in.lastBindMs > from)
                from = in.lastBindMs;
            if (in.nowMs - from > kStaleMs)
                return RoundWhy::Stale;
        }
        return RoundWhy::Grow;
    }

    // The state machine on RoundDecide (engine.cpp TickRingRound). `round` = the grown placement is configured AND our masks are handed out:
    // never one without the other (the bigger square with the game's mask puts its corners past the disc; our circle at the 0.5.4 size sits
    // inside the 0.5.4 hole and leaves a gap). The engine hands the masks out once the grown placement is written (before the radar's flip
    // re-reads it) and opens the round hole once it has landed.
    //   grow:   only on POSITIVE evidence of this showing: Grow, the radar shown, a bind of the mask since it appeared and within kStaleMs
    //           (a verdict reached while the radar is hidden, or in the show grace, is no evidence);
    //   shrink: on any other verdict; the radar is held hidden (hold) from that tick until `landed` says the 0.5.4 placement is in the game's
    //           table (written and read back since the shrink, the flip over), and no grow is considered until then.
    // landed: the placement configured LAST (by the engine, from this gate's previous output) has landed. On the shrink tick it still refers to
    // the grown placement, so a shrink never ends on the tick it starts.
    struct RoundGateOut
    {
        bool round = false;        // configure the grown placement and hand our masks out
        bool shrinking = false;    // back to 0.5.4, not landed yet: keep the masks handed out, hold the radar hidden
        bool hold = false;         // the radar hidden this tick
        bool changed = false;      // round flipped this tick
        RoundWhy why = RoundWhy::SwitchOff;
    };
    class RoundGate
    {
    public:
        RoundGateOut Update(const RoundInputs& in, bool landed)
        {
            RoundGateOut o;
            o.why = RoundDecide(in);
            if (round_ && o.why != RoundWhy::Grow)
            {
                round_ = false;
                shrinking_ = true;
                o.changed = true;
                ++shrinks_;
            }
            else if (!round_ && !shrinking_ && o.why == RoundWhy::Grow && in.radarShown && in.lastBindMs >= in.shownSinceMs &&
                     in.nowMs - in.lastBindMs <= kStaleMs)
            {
                round_ = true;
                o.changed = true;
                ++grows_;
            }
            else if (shrinking_ && landed)
                shrinking_ = false;
            o.round = round_;
            o.shrinking = shrinking_;
            o.hold = shrinking_;
            return o;
        }
        bool Round() const { return round_; }
        uint32_t Grows() const { return grows_; }
        uint32_t Shrinks() const { return shrinks_; }

    private:
        bool round_ = false, shrinking_ = false;
        uint32_t grows_ = 0, shrinks_ = 0;
    };

    // ---- 4. textures: formats, codecs ---------------------------------------------------------------------------------------------------
    // The formats the stock mask can be in (the community's copies of graphics.ytd say 512 x 256 DXT1 for radarmasksm and 512 x 512 DXT1 for
    // radarmasklg; the code takes any of these and refuses the rest, logged).
    enum class TexFmt
    {
        Unknown,
        RGBA8,
        BGRA8,
        R8,
        A8,
        BC1,
        BC3,
        BC4,
    };
    inline const char* TexFmtName(TexFmt f)
    {
        switch (f)
        {
        case TexFmt::RGBA8: return "RGBA8";
        case TexFmt::BGRA8: return "BGRA8";
        case TexFmt::R8: return "R8";
        case TexFmt::A8: return "A8";
        case TexFmt::BC1: return "BC1";
        case TexFmt::BC3: return "BC3";
        case TexFmt::BC4: return "BC4";
        case TexFmt::Unknown: break;
        }
        return "unsupported";
    }
    inline bool IsBlockFmt(TexFmt f) { return f == TexFmt::BC1 || f == TexFmt::BC3 || f == TexFmt::BC4; }
    inline int BlockBytes(TexFmt f) { return f == TexFmt::BC3 ? 16 : (f == TexFmt::BC1 || f == TexFmt::BC4) ? 8 : 0; }
    inline int PixelBytes(TexFmt f) { return (f == TexFmt::RGBA8 || f == TexFmt::BGRA8) ? 4 : (f == TexFmt::R8 || f == TexFmt::A8) ? 1 : 0; }
    inline int LevelDim(int base, int level) { const int v = base >> level; return v < 1 ? 1 : v; }
    // Bytes in one row (of blocks or of pixels) and in the whole level, tightly packed.
    inline size_t RowBytes(TexFmt f, int w) { return IsBlockFmt(f) ? (size_t)((w + 3) / 4) * (size_t)BlockBytes(f) : (size_t)w * (size_t)PixelBytes(f); }
    inline int Rows(TexFmt f, int h) { return IsBlockFmt(f) ? (h + 3) / 4 : h; }
    inline size_t LevelBytes(TexFmt f, int w, int h) { return RowBytes(f, w) * (size_t)Rows(f, h); }

    struct Rgba
    {
        uint8_t r = 0, g = 0, b = 0, a = 0;
        bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    };

    inline void Unpack565(uint16_t c, int& r, int& g, int& b)
    {
        const int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
        r = (r5 << 3) | (r5 >> 2);
        g = (g6 << 2) | (g6 >> 4);
        b = (b5 << 3) | (b5 >> 2);
    }
    inline uint16_t Pack565(int r, int g, int b)
    {
        auto q = [](int v, int bits) { const int m = (1 << bits) - 1; int x = (v * m + 127) / 255; return x < 0 ? 0 : (x > m ? m : x); };
        return (uint16_t)((q(r, 5) << 11) | (q(g, 6) << 5) | q(b, 5));
    }

    // One BC1 colour block (8 bytes) -> 16 texels (row-major). bc3Colour: the colour half of a BC3 block (always the 4-colour mode, alpha untouched).
    inline void DecodeBC1Block(const uint8_t* in, Rgba out[16], bool bc3Colour)
    {
        const uint16_t c0 = (uint16_t)(in[0] | (in[1] << 8)), c1 = (uint16_t)(in[2] | (in[3] << 8));
        int p[4][4];
        Unpack565(c0, p[0][0], p[0][1], p[0][2]);
        Unpack565(c1, p[1][0], p[1][1], p[1][2]);
        p[0][3] = p[1][3] = 255;
        const bool four = bc3Colour || c0 > c1;
        for (int k = 0; k < 3; ++k)
        {
            if (four)
            {
                p[2][k] = (2 * p[0][k] + p[1][k] + 1) / 3;
                p[3][k] = (p[0][k] + 2 * p[1][k] + 1) / 3;
            }
            else
            {
                p[2][k] = (p[0][k] + p[1][k]) / 2;
                p[3][k] = 0;
            }
        }
        p[2][3] = 255;
        p[3][3] = four ? 255 : 0;
        const uint32_t idx = (uint32_t)in[4] | ((uint32_t)in[5] << 8) | ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);
        for (int i = 0; i < 16; ++i)
        {
            const int c = (int)((idx >> (2 * i)) & 3u);
            out[i].r = (uint8_t)p[c][0];
            out[i].g = (uint8_t)p[c][1];
            out[i].b = (uint8_t)p[c][2];
            if (!bc3Colour)
                out[i].a = (uint8_t)p[c][3];
        }
    }
    // A BC3 alpha / BC4 block (8 bytes) -> 16 values.
    inline void DecodeAlphaBlock(const uint8_t* in, uint8_t out[16])
    {
        const int a0 = in[0], a1 = in[1];
        int pal[8];
        pal[0] = a0;
        pal[1] = a1;
        if (a0 > a1)
            for (int i = 1; i < 7; ++i)
                pal[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
        else
        {
            for (int i = 1; i < 5; ++i)
                pal[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
            pal[6] = 0;
            pal[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i)
            bits |= (uint64_t)in[2 + i] << (8 * i);
        for (int i = 0; i < 16; ++i)
            out[i] = (uint8_t)pal[(bits >> (3 * i)) & 7u];
    }

    // An alpha / BC4 block from 16 values: endpoints = the block's extremes (8-value mode), each value to its nearest palette entry.
    inline void EncodeAlphaBlock(const uint8_t v[16], uint8_t out[8])
    {
        int lo = 255, hi = 0;
        for (int i = 0; i < 16; ++i)
        {
            if (v[i] < lo) lo = v[i];
            if (v[i] > hi) hi = v[i];
        }
        out[0] = (uint8_t)hi;
        out[1] = (uint8_t)lo;
        uint64_t bits = 0;
        if (hi > lo)
        {
            int pal[8];
            pal[0] = hi;
            pal[1] = lo;
            for (int i = 1; i < 7; ++i)
                pal[i + 1] = ((7 - i) * hi + i * lo + 3) / 7;
            for (int i = 0; i < 16; ++i)
            {
                int best = 0, be = 1 << 30;
                for (int k = 0; k < 8; ++k)
                {
                    const int e = std::abs(pal[k] - (int)v[i]);
                    if (e < be)
                    {
                        be = e;
                        best = k;
                    }
                }
                bits |= (uint64_t)best << (3 * i);
            }
        }
        for (int i = 0; i < 6; ++i)
            out[2 + i] = (uint8_t)(bits >> (8 * i));
    }

    // A BC1 colour block in the 4-colour mode (opaque) from 16 texels whose colours lie on one line (our mask: every texel is a blend of two
    // colours). Endpoints = the texels of least and greatest luminance; every texel to its nearest of the four palette colours.
    inline void EncodeBC1Block(const Rgba px[16], uint8_t out[8])
    {
        auto lum = [](const Rgba& c) { return 299 * c.r + 587 * c.g + 114 * c.b; };
        int iLo = 0, iHi = 0;
        for (int i = 1; i < 16; ++i)
        {
            if (lum(px[i]) < lum(px[iLo])) iLo = i;
            if (lum(px[i]) > lum(px[iHi])) iHi = i;
        }
        uint16_t c0 = Pack565(px[iHi].r, px[iHi].g, px[iHi].b), c1 = Pack565(px[iLo].r, px[iLo].g, px[iLo].b);
        uint32_t idx = 0;
        if (c0 < c1)
        {
            const uint16_t t = c0;
            c0 = c1;
            c1 = t;
        }
        if (c0 != c1)
        {
            int p[4][3];
            Unpack565(c0, p[0][0], p[0][1], p[0][2]);
            Unpack565(c1, p[1][0], p[1][1], p[1][2]);
            for (int k = 0; k < 3; ++k)
            {
                p[2][k] = (2 * p[0][k] + p[1][k] + 1) / 3;
                p[3][k] = (p[0][k] + 2 * p[1][k] + 1) / 3;
            }
            for (int i = 0; i < 16; ++i)
            {
                int best = 0, be = 1 << 30;
                for (int c = 0; c < 4; ++c)
                {
                    const int dr = p[c][0] - px[i].r, dg = p[c][1] - px[i].g, db = p[c][2] - px[i].b;
                    const int e = dr * dr + dg * dg + db * db;
                    if (e < be)
                    {
                        be = e;
                        best = c;
                    }
                }
                idx |= (uint32_t)best << (2 * i);
            }
        }
        // c0 == c1 (a flat block): the 3-colour mode's index 0 is c0, opaque, so every index stays 0
        out[0] = (uint8_t)(c0 & 0xff);
        out[1] = (uint8_t)(c0 >> 8);
        out[2] = (uint8_t)(c1 & 0xff);
        out[3] = (uint8_t)(c1 >> 8);
        for (int i = 0; i < 4; ++i)
            out[4 + i] = (uint8_t)(idx >> (8 * i));
    }

    // One level, tightly packed (RowBytes / Rows) -> w * h texels. A8 decodes as (0, 0, 0, a) and R8 as (r, 0, 0, 255), as D3D samples them.
    inline bool DecodeLevel(TexFmt f, const uint8_t* data, size_t n, int w, int h, std::vector<Rgba>& out)
    {
        if (!data || w <= 0 || h <= 0 || n < LevelBytes(f, w, h) || f == TexFmt::Unknown)
            return false;
        out.assign((size_t)w * (size_t)h, Rgba{});
        if (!IsBlockFmt(f))
        {
            const int pb = PixelBytes(f);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const uint8_t* s = data + (size_t)y * RowBytes(f, w) + (size_t)x * (size_t)pb;
                    Rgba& o = out[(size_t)y * (size_t)w + (size_t)x];
                    switch (f)
                    {
                    case TexFmt::RGBA8: o = {s[0], s[1], s[2], s[3]}; break;
                    case TexFmt::BGRA8: o = {s[2], s[1], s[0], s[3]}; break;
                    case TexFmt::R8: o = {s[0], 0, 0, 255}; break;
                    case TexFmt::A8: o = {0, 0, 0, s[0]}; break;
                    default: return false;
                    }
                }
            return true;
        }
        const int bw = (w + 3) / 4, bh = (h + 3) / 4, bb = BlockBytes(f);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
            {
                const uint8_t* blk = data + ((size_t)by * (size_t)bw + (size_t)bx) * (size_t)bb;
                Rgba t[16];
                if (f == TexFmt::BC1)
                    DecodeBC1Block(blk, t, false);
                else if (f == TexFmt::BC3)
                {
                    uint8_t a[16];
                    DecodeAlphaBlock(blk, a);
                    DecodeBC1Block(blk + 8, t, true);
                    for (int i = 0; i < 16; ++i)
                        t[i].a = a[i];
                }
                else
                {
                    uint8_t r[16];
                    DecodeAlphaBlock(blk, r);
                    for (int i = 0; i < 16; ++i)
                        t[i] = {r[i], 0, 0, 255};
                }
                for (int ty = 0; ty < 4; ++ty)
                    for (int tx = 0; tx < 4; ++tx)
                    {
                        const int X = bx * 4 + tx, Y = by * 4 + ty;
                        if (X < w && Y < h)
                            out[(size_t)Y * (size_t)w + (size_t)X] = t[ty * 4 + tx];
                    }
            }
        return true;
    }

    // w * h texels -> one tightly packed level. Blocks past the edge repeat the last texel.
    inline bool EncodeLevel(TexFmt f, const std::vector<Rgba>& px, int w, int h, std::vector<uint8_t>& out)
    {
        if (w <= 0 || h <= 0 || px.size() < (size_t)w * (size_t)h || f == TexFmt::Unknown)
            return false;
        out.assign(LevelBytes(f, w, h), 0);
        if (!IsBlockFmt(f))
        {
            const int pb = PixelBytes(f);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    uint8_t* d = out.data() + (size_t)y * RowBytes(f, w) + (size_t)x * (size_t)pb;
                    const Rgba& s = px[(size_t)y * (size_t)w + (size_t)x];
                    switch (f)
                    {
                    case TexFmt::RGBA8: d[0] = s.r; d[1] = s.g; d[2] = s.b; d[3] = s.a; break;
                    case TexFmt::BGRA8: d[0] = s.b; d[1] = s.g; d[2] = s.r; d[3] = s.a; break;
                    case TexFmt::R8: d[0] = s.r; break;
                    case TexFmt::A8: d[0] = s.a; break;
                    default: return false;
                    }
                }
            return true;
        }
        const int bw = (w + 3) / 4, bh = (h + 3) / 4, bb = BlockBytes(f);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
            {
                Rgba t[16];
                for (int ty = 0; ty < 4; ++ty)
                    for (int tx = 0; tx < 4; ++tx)
                    {
                        int X = bx * 4 + tx, Y = by * 4 + ty;
                        if (X >= w) X = w - 1;
                        if (Y >= h) Y = h - 1;
                        t[ty * 4 + tx] = px[(size_t)Y * (size_t)w + (size_t)X];
                    }
                uint8_t* blk = out.data() + ((size_t)by * (size_t)bw + (size_t)bx) * (size_t)bb;
                if (f == TexFmt::BC1)
                    EncodeBC1Block(t, blk);
                else if (f == TexFmt::BC3)
                {
                    uint8_t a[16];
                    for (int i = 0; i < 16; ++i)
                        a[i] = t[i].a;
                    EncodeAlphaBlock(a, blk);
                    EncodeBC1Block(t, blk + 8);
                }
                else
                {
                    uint8_t r[16];
                    for (int i = 0; i < 16; ++i)
                        r[i] = t[i].r;
                    EncodeAlphaBlock(r, blk);
                }
            }
        return true;
    }

    // ---- 5. the stock mask: is it one, and what are its inside / outside -----------------------------------------------------------------
    // The "value" of a texel: what the radar's multiply sees. A8 masks carry it in alpha, R8 / BC4 in red (the GPU reads them as (r, 0, 0, 1)),
    // every other format in the grey level.
    inline int TexelValue(TexFmt f, const Rgba& c)
    {
        if (f == TexFmt::A8)
            return c.a;
        if (f == TexFmt::R8 || f == TexFmt::BC4)
            return c.r;
        return (299 * c.r + 587 * c.g + 114 * c.b + 500) / 1000;
    }

    struct StockMask
    {
        bool ok = false;
        std::string why;          // !ok: what failed (logged)
        Rgba inside, outside;     // the centre's and the corners' average texel
        int insideValue = 0, outsideValue = 0, edgeValue = 0;   // the edge: the mean value along the four sides
    };
    // A mask looks like: grey (r, g, b within 24 of each other at the centre and corners), a bright centre (value >= 64), and edges and corners well
    // below it (each <= 60 % of the centre). Anything else is not the radar mask: the swap is refused (a wrong texture made round would be worse
    // than a square map).
    inline StockMask AnalyzeStockMask(TexFmt f, const std::vector<Rgba>& px, int w, int h)
    {
        StockMask m;
        if (w < 8 || h < 8 || px.size() < (size_t)w * (size_t)h)
        {
            m.why = "too small to be a mask";
            return m;
        }
        auto at = [&](int x, int y) -> const Rgba& { return px[(size_t)y * (size_t)w + (size_t)x]; };
        auto avg = [&](int x0, int y0, int x1, int y1) {
            long s[4] = {0, 0, 0, 0}, n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                {
                    const Rgba& c = at(x, y);
                    s[0] += c.r; s[1] += c.g; s[2] += c.b; s[3] += c.a;
                    ++n;
                }
            Rgba o;
            if (n)
            {
                o.r = (uint8_t)((s[0] + n / 2) / n); o.g = (uint8_t)((s[1] + n / 2) / n);
                o.b = (uint8_t)((s[2] + n / 2) / n); o.a = (uint8_t)((s[3] + n / 2) / n);
            }
            return o;
        };
        const int cw = w / 8 > 0 ? w / 8 : 1, ch = h / 8 > 0 ? h / 8 : 1;
        m.inside = avg(w / 2 - cw / 2, h / 2 - ch / 2, w / 2 - cw / 2 + cw, h / 2 - ch / 2 + ch);
        const Rgba c4[4] = {at(0, 0), at(w - 1, 0), at(0, h - 1), at(w - 1, h - 1)};
        {
            int s[4] = {0, 0, 0, 0};
            for (const Rgba& c : c4)
            {
                s[0] += c.r; s[1] += c.g; s[2] += c.b; s[3] += c.a;
            }
            m.outside = {(uint8_t)((s[0] + 2) / 4), (uint8_t)((s[1] + 2) / 4), (uint8_t)((s[2] + 2) / 4), (uint8_t)((s[3] + 2) / 4)};
        }
        long es = 0, en = 0;
        for (int x = 0; x < w; ++x)
        {
            es += TexelValue(f, at(x, 0)) + TexelValue(f, at(x, h - 1));
            en += 2;
        }
        for (int y = 1; y + 1 < h; ++y)
        {
            es += TexelValue(f, at(0, y)) + TexelValue(f, at(w - 1, y));
            en += 2;
        }
        m.insideValue = TexelValue(f, m.inside);
        m.outsideValue = TexelValue(f, m.outside);
        m.edgeValue = (int)(en ? es / en : 0);
        auto grey = [&](const Rgba& c) {
            if (f == TexFmt::A8 || f == TexFmt::R8 || f == TexFmt::BC4)
                return true;
            const int a = c.r, b = c.g, d = c.b;
            return std::abs(a - b) <= 24 && std::abs(b - d) <= 24 && std::abs(a - d) <= 24;
        };
        char buf[200];
        std::snprintf(buf, sizeof(buf), "centre %d (rgba %u %u %u %u), corners %d, edges %d", m.insideValue, m.inside.r, m.inside.g, m.inside.b,
                      m.inside.a, m.outsideValue, m.edgeValue);
        if (!grey(m.inside) || !grey(m.outside))
            m.why = std::string("not grey: ") + buf;
        else if (m.insideValue < 64)
            m.why = std::string("centre too dark for a mask: ") + buf;
        else if (m.outsideValue * 10 > m.insideValue * 6 || m.edgeValue * 10 > m.insideValue * 6)
            m.why = std::string("edges not dark (not a mask shape): ") + buf;
        else
        {
            m.ok = true;
            m.why = buf;
        }
        return m;
    }

    // ---- 6. our mask ---------------------------------------------------------------------------------------------------------------------
    // Coverage of the ellipse inscribed in a w x h texture at texel (x, y), 4 x 4 supersampled: 1 inside, 0 outside, a linear ramp of
    // kFeather (fraction of the semi-axis) INSIDE the ellipse, so the texture's edge texels are 0 and the shape touches the box's edge at 4 points.
    constexpr float kFeather = 0.02f;
    inline float EllipseCoverage(int x, int y, int w, int h)
    {
        float s = 0.0f;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
            {
                const float u = ((float)x + (i + 0.5f) / 4.0f) / (float)w * 2.0f - 1.0f;
                const float v = ((float)y + (j + 0.5f) / 4.0f) / (float)h * 2.0f - 1.0f;
                const float d = std::sqrt(u * u + v * v);
                float c = (1.0f - d) / kFeather;
                c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
                s += c;
            }
        return s / 16.0f;
    }
    inline Rgba Blend(const Rgba& out, const Rgba& in, float c)
    {
        auto l = [&](uint8_t a, uint8_t b) { const float v = (float)a + ((float)b - (float)a) * c; return (uint8_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v + 0.5f)); };
        return {l(out.r, in.r), l(out.g, in.g), l(out.b, in.b), l(out.a, in.a)};
    }
    // Every level of the circle mask (level 0 = w x h): texel = Blend(outside, inside, coverage). The outside is black with the stock corners'
    // alpha (0 where the stock fades alpha, 255 for an opaque BC1), the inside is the stock centre's own texel, so the map keeps its stock brightness.
    inline Rgba MaskOutside(const StockMask& m) { return {0, 0, 0, m.outside.a}; }
    inline void BuildRoundMask(int w, int h, int levels, const Rgba& inside, const Rgba& outside, std::vector<std::vector<Rgba>>& mips)
    {
        mips.assign(levels > 0 ? (size_t)levels : 0, {});
        for (int l = 0; l < levels; ++l)
        {
            const int lw = LevelDim(w, l), lh = LevelDim(h, l);
            std::vector<Rgba>& v = mips[(size_t)l];
            v.resize((size_t)lw * (size_t)lh);
            for (int y = 0; y < lh; ++y)
                for (int x = 0; x < lw; ++x)
                    v[(size_t)y * (size_t)lw + (size_t)x] = Blend(outside, inside, EllipseCoverage(x, y, lw, lh));
        }
    }

    // FNV-1a 64 over bytes: the stock mask's fingerprint, LOGGED so a later round can pin it (no Rockstar pixel enters the repo, only a hash).
    inline uint64_t Fnv64(const uint8_t* p, size_t n, uint64_t h = 0xcbf29ce484222325ULL)
    {
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    // ---- 7. the blip clip flag (FiveM SET_MINIMAP_CLIP_TYPE) -----------------------------------------------------------------------------
    // FiveM extra-natives-five/src/InterfaceExtraNatives.cpp (master 5c3065c): `minimapIsRect = hook::get_address<bool*>(hook::get_pattern(
    // "8A 15 ? ? ? ? F3 0F 10 15", 2))`, SET_MINIMAP_CLIP_TYPE writes `*minimapIsRect = type == 0` (0 = rectangle, the default, restored at
    // session end). Its doc: the minimap's BLIP clipping object, rectangular or rounded: with a round map the off-radar blips (waypoint, far
    // blips) are clamped to the circle instead of the square's corners. Read in both Legacy 3889 dumps (%LOCALAPPDATA%\GAGE-MP\dump, 2026-11-07):
    // the pattern hits ONCE, at RVA 0x2f2969, `mov dl, [rip+disp]` -> RVA 0x1d40a43 in .data holding 01, followed by `movss xmm2, [rip+...]`,
    // `xor ecx, ecx; test dl, dl; je` (the bool decides a branch right there). Legacy only (Enhanced is not in this round).
    constexpr const char* kClipSig = "8A 15 ?? ?? ?? ?? F3 0F 10 15";
    constexpr LoaderCheck kClipChecks[] = {
        {0x0E, "33 C9 84 D2 74", "the bool is tested right after the load (xor ecx, ecx; test dl, dl; je)"},
    };
    constexpr uint32_t kClipHit3889 = 0x2f2969, kClipFlag3889 = 0x1d40a43;
    constexpr MinimapLayout kClipLayoutLegacy = {"Legacy 1.0.3889 minimap blip clip (minimapIsRect)", kClipSig, 10, 0, 2, 6, 0x10, 0x20, kClipChecks,
                                                 (int)(sizeof(kClipChecks) / sizeof(kClipChecks[0])), false, kClipHit3889, kClipFlag3889};

    struct ClipResolve
    {
        bool ok = false;
        size_t flagOff = 0;   // the bool's address - image base
        std::string why;
    };
    // hit: where kClipSig matched (the caller proved [hit - spanBefore, hit + spanAfter) readable). The flag must lie inside the image and the
    // instructions after the load must be the ones it was read from. The flag's VALUE is checked by the caller (0 or 1, live).
    inline ClipResolve ResolveClipFlag(const uint8_t* hit, const uint8_t* base, size_t imageSize)
    {
        const MinimapLayout& L = kClipLayoutLegacy;
        ClipResolve r;
        if (!hit || !base || hit < base + L.spanBefore || hit + L.spanAfter > base + imageSize)
        {
            r.why = "the hit is too close to the image edge";
            return r;
        }
        for (int k = 0; k < L.nChecks; ++k)
        {
            const LoaderCheck& c = L.checks[k];
            const int n = HexByteCount(c.bytes);
            for (int i = 0; i < n; ++i)
                if (hit[c.off + i] != (uint8_t)HexByteAt(c.bytes, i))
                {
                    r.why = std::string("the code after the load differs from 3889's: ") + c.what;
                    return r;
                }
        }
        int32_t disp = 0;
        std::memcpy(&disp, hit + L.dispAt, 4);
        const int64_t t = (int64_t)(hit - base) + L.endAt + (int64_t)disp;
        if (t < 0 || (uint64_t)t >= (uint64_t)imageSize)
        {
            r.why = "the flag's address is outside the image";
            return r;
        }
        r.ok = true;
        r.flagOff = (size_t)t;
        return r;
    }

} // namespace gage::sync::ringround
