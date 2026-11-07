// 0.5.5 ring_map_round (libs/sync/ring_round.h, hud_ring.h MapRoundRadiusPx, minimap_size.h RingPlacementBox). Included by tests/gameplay_test.cpp
// after its Check() / F() and after minimap_tests.h (the dump helpers of minimapsizetest); run by ringround_t::RunPure(), kChecks checks, then
// ringround_t::RunDump() (the blip clip pattern against the Legacy 3889 dumps: not counted, a loud SKIP when this machine has none).
//   A. the radii: the round hole's amber ring ends ON the arcs' inner edge (no gap, no overlap) at 720 / 1080 / 1440; GTA's circle between the
//      hole and the disc for a box-fit error up to 6 % either way
//   B. the asked box (RoundAskPx, RingPlacementBox: the square case is RingPlacement exactly), ParseBoxFit
//   C. the gate: every RoundDecide reason; RoundGate grows only on positive evidence, holds the radar on a shrink until it lands, never flaps
//   D. the mask: AnalyzeStockMask accepts a soft grey rectangle and refuses colour / bright edges / lopsided / blob shapes; BuildRoundMask's
//      circle; the codecs
//   F. (0.5.6) the census pick: exactly one mask-like view in a complete census; an incomplete census never picks; a pinned hash + shape wins
//   E. the blip clip pattern on a synthetic image (resolves, a changed check byte is refused)
#pragma once

#include "sync/ring_round.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ringround_t {

    using namespace gage::sync::ringround;
    namespace hr = gage::sync::hudring;

    constexpr int kChecks = 19;

    inline bool NearF(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

    // A synthetic stock-like mask: grey, value `in` inside a soft rectangle that fades to `out` over `ramp` texels at the box's edge.
    inline std::vector<Rgba> SoftRect(int w, int h, int in, int out, int ramp, uint8_t alphaIn = 255, uint8_t alphaOut = 255)
    {
        std::vector<Rgba> v((size_t)w * (size_t)h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const int dx = x < w - 1 - x ? x : w - 1 - x, dy = y < h - 1 - y ? y : h - 1 - y;
                const int d = dx < dy ? dx : dy;
                const float t = d >= ramp ? 1.0f : (float)d / (float)ramp;
                const uint8_t g = (uint8_t)(out + (in - out) * t + 0.5f);
                const uint8_t a = (uint8_t)(alphaOut + ((int)alphaIn - (int)alphaOut) * t + 0.5f);
                v[(size_t)y * (size_t)w + (size_t)x] = {g, g, g, a};
            }
        return v;
    }

    // channel: 0 = the mean of r, g, b; 1 = alpha; 2 = red alone (R8 / BC4 carry one channel)
    inline double MeanAbsErr(const std::vector<Rgba>& a, const std::vector<Rgba>& b, int channel)
    {
        if (a.size() != b.size() || a.empty())
            return 1e9;
        double s = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            s += channel == 1   ? std::fabs((double)a[i].a - b[i].a)
                 : channel == 2 ? std::fabs((double)a[i].r - b[i].r)
                                : (std::fabs((double)a[i].r - b[i].r) + std::fabs((double)a[i].g - b[i].g) + std::fabs((double)a[i].b - b[i].b)) / 3.0;
        return s / (double)a.size();
    }

    // A run of the gate: binds arrive every 16 ms while `bindsOn(t)` says so and the radar is shown (not held); landed follows `landDelayMs`
    // after each placement change. Returns the number of round flips; the tick log in `trace`.
    struct GateSim
    {
        RoundGate gate;
        RoundGateOut out;
        double nowMs = 0.0, shownSinceMs = 0.0, lastBindMs = -1.0, cfgChangeMs = 0.0;
        uint64_t binds = 0;
        bool shown = false, cfgRound = false;
        int flips = 0, heldTicks = 0, roundTicksWithoutBinds = 0;
        template <class BindsOn>
        void Run(double untilMs, bool wanted, BindsOn bindsOn, double landDelayMs = 400.0)
        {
            for (; nowMs < untilMs; nowMs += 16.0)
            {
                if (out.round != cfgRound)
                {
                    cfgRound = out.round;
                    cfgChangeMs = nowMs;
                }
                const bool landed = nowMs - cfgChangeMs >= landDelayMs;
                const bool isShown = wanted && !out.hold;
                if (isShown && !shown)
                    shownSinceMs = nowMs;
                shown = isShown;
                if (isShown && bindsOn(nowMs))
                {
                    ++binds;
                    lastBindMs = nowMs;
                }
                RoundInputs in;
                in.switchOn = true;
                in.d3d11 = true;
                in.smallReplaced = true;
                in.bigReplaced = true;
                in.bigMirror = true;
                in.smallBinds = binds;
                in.lastBindMs = lastBindMs;
                in.radarShown = isShown;
                in.shownSinceMs = shownSinceMs;
                in.nowMs = nowMs;
                out = gate.Update(in, landed);
                if (out.changed)
                    ++flips;
                if (out.hold)
                    ++heldTicks;
                if (out.round && isShown && !bindsOn(nowMs) && nowMs - lastBindMs > kShowGraceMs + kStaleMs + 50.0)
                    ++roundTicksWithoutBinds;
            }
        }
    };

    inline void RunPure()
    {
        std::printf("-- 0.5.5 ring map round mask (radii, box, gate, mask, codecs, blip clip pattern)\n");
        // A1. the hole's ring ends exactly on the arcs' inner edge, at every display; the hole is the grown one
        {
            bool ok = true;
            std::string got;
            for (float H : {720.0f, 1080.0f, 1440.0f, 2160.0f})
            {
                const double r = hr::MapRoundRadiusPx(H), w = hr::MapRingWidthPx(H), edge = (hr::kArcR - hr::kArcW * 0.5f) * hr::Scale(H);
                if (!NearF(r + w, edge, 1e-3) || !(r > hr::MapRadiusPx(H) * 1.2))
                    ok = false;
                char b[96];
                std::snprintf(b, sizeof(b), "H %.0f: r %.3f + ring %.3f vs edge %.3f; ", (double)H, r, w, edge);
                got += b;
            }
            Check("ring map round: the hole's amber ring ends ON the arcs' inner edge r 125 (r + ring width == 125 * scale at 720 / 1080 / 1440 / 2160), "
                  "and the hole is the grown one (over 1.2 x the 0.5.4 square's inscribed circle)",
                  ok, got);
        }
        // A2. the pinned numbers: 123.5 design px at 1080p and up, 123.18 at 720p (the 1 px floor of the ring)
        {
            const double d1080 = hr::MapRoundRadiusPx(1080.0f) / hr::Scale(1080.0f), d1440 = hr::MapRoundRadiusPx(1440.0f) / hr::Scale(1440.0f);
            const double d720 = hr::MapRoundRadiusPx(720.0f) / hr::Scale(720.0f);
            Check("ring map round: hole r 123.5 design px at 1080p (101.89 screen px) and 1440p, 123.18 at 720p; 0.5.4's was 96.87",
                  NearF(d1080, 123.5, 1e-3) && NearF(d1440, 123.5, 1e-3) && NearF(d720, 123.182, 2e-3) && NearF(hr::MapRoundRadiusPx(1080.0f), 101.8875, 1e-3) &&
                      NearF(hr::MapRadiusPx(1080.0f) / hr::Scale(1080.0f), 96.874, 2e-3),
                  F("1080 %.4f 1440 %.4f 720 %.4f", d1080, d1440, d720));
        }
        // A3. GTA's circle: midway between the hole and the disc, inside both bounds for a box-fit error of 6 % either way
        {
            const double hole = 123.5, disc = hr::kDiscR;
            const bool mid = NearF(kMaskCircleR, (hole + disc) * 0.5, 1e-4);
            const bool lo = kMaskCircleR * 0.94 >= hole, hi = kMaskCircleR * 1.06 <= disc;
            Check("ring map round: GTA's circle r 131.75 is midway between the hole (123.5) and the disc (140), still covering the hole at -6 % and inside "
                  "the disc at +6 % box-fit error",
                  mid && lo && hi, F("-6 %% %.2f +6 %% %.2f", kMaskCircleR * 0.94, kMaskCircleR * 1.06));
        }
        // B1. the asked box: the circle the box's real size inscribes is 2 * 131.75 design px across, per axis
        {
            const BoxFit fit;
            const BoxPx b = RoundAskPx(1080.0f, fit);
            const double s = hr::Scale(1080.0f);
            const bool ok = NearF(b.w * fit.fx / s, 2.0 * kMaskCircleR, 1e-3) && NearF(b.h * fit.fy / s, 2.0 * kMaskCircleR, 1e-3) && NearF(b.w, 235.014, 1e-2) &&
                            NearF(b.h, 226.445, 1e-2);
            const BoxFit bad = {0.0f, 0.9f, false, nullptr};
            const BoxPx z = RoundAskPx(1080.0f, bad);
            Check("ring map round: the asked box at 1080p is 235.01 x 226.45 px (2 * 131.75 design px / fit (0.925, 0.96)); a zero fit asks nothing", ok && z.w == 0.0f && z.h == 0.0f,
                  F("%.3f x %.3f", (double)b.w, (double)b.h));
        }
        // B2. RingPlacementBox: the square case IS RingPlacement (0.5.4 unchanged), a non-square box scales the axes apart about the same centre
        {
            const float W = 2560.0f, H = 1440.0f, cx = 233.07f, cy = 1136.53f, sq = hr::MapSquarePx(H);
            const gage::sync::MinimapPlacement a = gage::sync::RingPlacement(W, H, 0.97f, true, cx, cy, sq);
            const gage::sync::MinimapPlacement b = gage::sync::RingPlacementBox(W, H, 0.97f, true, cx, cy, sq, sq);
            const gage::sync::MinimapPlacement c = gage::sync::RingPlacementBox(W, H, 0.97f, true, cx, cy, sq * 1.2f, sq * 1.1f);
            const bool same = a == b;
            // the visible box's centre: x = left + w / 2, y = bottom - h / 2 (table space, y down); the same as the target point (tx, ty)
            const bool axes = NearF(c.sx / a.sx, 1.2, 1e-4) && NearF(c.sy / a.sy, 1.1, 1e-4) && c.tx == a.tx && c.ty == a.ty &&
                              NearF(c.vx + c.vw * 0.5f, a.vx + a.vw * 0.5f, 1e-5) && NearF(c.vy - c.vh * 0.5f, a.vy - a.vh * 0.5f, 1e-5) &&
                              NearF(c.vw / a.vw, 1.2, 1e-4) && NearF(c.vh / a.vh, 1.1, 1e-4);
            Check("ring map round: RingPlacementBox(square) == RingPlacement (0.5.4 placement byte-identical); (1.2, 1.1) x the square scales x and y apart "
                  "about the same centre",
                  same && axes, F("sx %.4f/%.4f sy %.4f/%.4f", (double)c.sx, (double)a.sx, (double)c.sy, (double)a.sy));
        }
        // B3. ParseBoxFit
        {
            const BoxFit u = ParseBoxFit(L""), a = ParseBoxFit(L"0.93,0.95"), s = ParseBoxFit(L" 0.9 1.0 "), lo = ParseBoxFit(L"0.5,0.9"), junk = ParseBoxFit(L"wide");
            const bool ok = !u.overridden && !u.why && u.fx == kBoxFitX && a.overridden && NearF(a.fx, 0.93, 1e-6) && NearF(a.fy, 0.95, 1e-6) && s.overridden &&
                            NearF(s.fy, 1.0, 1e-6) && !lo.overridden && lo.why && lo.fx == kBoxFitX && !junk.overridden && junk.why;
            Check("ring_map_round_fit: unset = (0.925, 0.96); \"0.93,0.95\" and \" 0.9 1.0 \" taken; 0.5 (outside 0.70..1.20) and junk refused with a reason", ok, "");
        }
        // C1. every RoundDecide reason, in order
        {
            RoundInputs g;
            g.switchOn = g.d3d11 = g.smallReplaced = g.bigReplaced = g.bigMirror = true;
            g.smallBinds = kConfirmBinds;
            g.radarShown = true;
            g.shownSinceMs = 0.0;
            g.nowMs = 2000.0;
            g.lastBindMs = 1990.0;
            RoundInputs off = g, d3d = g, nomask = g, nobig = g, nobigOk = g, unconf = g, stale = g, grace = g, hidden = g;
            off.switchOn = false;
            d3d.d3d11 = false;
            nomask.smallReplaced = false;
            nobig.bigReplaced = false;
            nobigOk.bigReplaced = false;
            nobigOk.bigMirror = false;
            unconf.smallBinds = kConfirmBinds - 1;
            stale.lastBindMs = 2000.0 - kStaleMs - 1.0;
            grace.shownSinceMs = 1800.0;
            grace.lastBindMs = -1.0;      // 200 ms into a show: no bind yet is no staleness
            hidden.radarShown = false;
            hidden.lastBindMs = 100.0;    // hidden: nothing to be stale about
            const bool ok = RoundDecide(g) == RoundWhy::Grow && RoundDecide(off) == RoundWhy::SwitchOff && RoundDecide(d3d) == RoundWhy::NotD3D11 &&
                            RoundDecide(nomask) == RoundWhy::NoMask && RoundDecide(nobig) == RoundWhy::NoBigMask && RoundDecide(nobigOk) == RoundWhy::Grow &&
                            RoundDecide(unconf) == RoundWhy::NotConfirmed && RoundDecide(stale) == RoundWhy::Stale && RoundDecide(grace) == RoundWhy::Grow &&
                            RoundDecide(hidden) == RoundWhy::Grow;
            Check("ring map round: RoundDecide says grow / off / not D3D11 / no mask / no big mask (only with ring_map_big) / not confirmed (29 binds) / stale "
                  "(301 ms without a bind while shown) and not stale in the 500 ms show grace or while hidden",
                  ok, F("stale %d grace %d", (int)RoundDecide(stale), (int)RoundDecide(grace)));
        }
        // C2. RoundGate: no grow on a verdict reached while hidden or in the grace without a bind of this showing; a bind of this showing grows
        {
            RoundGate gt;
            RoundInputs in;
            in.switchOn = in.d3d11 = in.smallReplaced = in.bigReplaced = in.bigMirror = true;
            in.smallBinds = 100;
            in.lastBindMs = 900.0;      // binds from the last showing
            in.radarShown = false;
            in.nowMs = 1000.0;
            const RoundGateOut h = gt.Update(in, true);
            in.radarShown = true;
            in.shownSinceMs = 1000.0;
            in.nowMs = 1016.0;
            const RoundGateOut g1 = gt.Update(in, true);   // shown, in the grace, the last bind is from before this showing
            in.lastBindMs = 1032.0;
            in.nowMs = 1032.0;
            const RoundGateOut g2 = gt.Update(in, true);
            Check("ring map round: RoundGate grows only on a bind of THIS showing (hidden: no; shown 16 ms with only older binds: no; a fresh bind: yes)",
                  !h.round && !g1.round && g2.round && g2.changed && gt.Grows() == 1, F("%d %d %d", (int)h.round, (int)g1.round, (int)g2.round));
        }
        // C3. a shrink holds the radar from its own tick (even when the old placement reads as landed) until the 0.5.4 one lands; no grow meanwhile
        {
            RoundGate gt;
            RoundInputs in;
            in.switchOn = in.d3d11 = in.smallReplaced = in.bigReplaced = in.bigMirror = true;
            in.smallBinds = 100;
            in.radarShown = true;
            in.shownSinceMs = 0.0;
            in.lastBindMs = 1000.0;
            in.nowMs = 1000.0;
            gt.Update(in, true);                            // grows
            RoundInputs sw = in;
            sw.switchOn = false;
            sw.nowMs = 1016.0;
            const RoundGateOut s0 = gt.Update(sw, true);   // the shrink tick: "landed" still means the grown placement
            sw.nowMs = 1032.0;
            const RoundGateOut s1 = gt.Update(sw, false);
            RoundInputs back = in;                          // the switch back on mid-shrink, binds fresh: still no grow until landed
            back.nowMs = 1048.0;
            back.radarShown = false;
            const RoundGateOut s2 = gt.Update(back, false);
            back.nowMs = 1064.0;
            const RoundGateOut s3 = gt.Update(back, true);  // landed: the hold ends
            Check("ring map round: a shrink holds the radar hidden from its own tick until the 0.5.4 placement lands, and nothing grows in between",
                  !s0.round && s0.hold && s0.shrinking && s0.changed && s1.hold && !s2.round && s2.hold && !s3.hold && !s3.round && gt.Shrinks() == 1,
                  F("s0 hold %d s1 %d s2 %d/%d s3 %d/%d", (int)s0.hold, (int)s1.hold, (int)s2.round, (int)s2.hold, (int)s3.round, (int)s3.hold));
        }
        // C4. the whole loop: binds stop for good while round -> one shrink, the radar shows again at 0.5.4 and stays there (no flapping); binds
        //     resume -> one grow
        {
            GateSim sim;
            const double stopAt = 5000.0, resumeAt = 20000.0;
            sim.Run(stopAt, true, [](double) { return true; });
            const bool grewFirst = sim.out.round && sim.flips == 1;
            sim.Run(resumeAt, true, [&](double t) { return t < stopAt; });
            const int flipsWhileStale = sim.flips - 1;
            const bool smallWhileStale = !sim.out.round && !sim.out.hold;
            sim.Run(30000.0, true, [&](double t) { return t < stopAt || t >= resumeAt; });
            Check("ring map round: a simulated 30 s: grows once, binds stop at 5 s -> ONE shrink and 15 s at 0.5.4 with the radar shown (no flapping), binds "
                  "resume at 20 s -> one grow; the grown map never shows past the stale limit without binds",
                  grewFirst && flipsWhileStale == 1 && smallWhileStale && sim.flips == 3 && sim.out.round && sim.roundTicksWithoutBinds == 0,
                  F("grew %d flips stale %d small %d total %d round %d without binds %d", (int)grewFirst, flipsWhileStale, (int)smallWhileStale, sim.flips,
                    (int)sim.out.round, sim.roundTicksWithoutBinds));
        }
        // D1. AnalyzeStockMask: a soft grey rectangle (512 x 256, BC1-like opaque) is a mask; a coloured one and a bright-edged one are not
        {
            const std::vector<Rgba> m = SoftRect(512, 256, 255, 0, 13);
            std::vector<Rgba> red = m;
            for (Rgba& c : red)
                c.g = c.b = 0;
            const std::vector<Rgba> flat = SoftRect(512, 256, 255, 200, 13);
            const StockMask a = AnalyzeStockMask(TexFmt::BC1, m, 512, 256), b = AnalyzeStockMask(TexFmt::BC1, red, 512, 256),
                            c = AnalyzeStockMask(TexFmt::BC1, flat, 512, 256), d = AnalyzeStockMask(TexFmt::BC1, m, 4, 4);
            Check("ring map round: AnalyzeStockMask takes a soft grey rectangle (centre 255, corners 0) and refuses a red one, one whose edges stay at 200, and a 4 x 4",
                  a.ok && a.insideValue == 255 && a.outsideValue == 0 && !b.ok && !c.ok && !d.ok, a.why + " | " + b.why + " | " + c.why);
        }
        // D2. BuildRoundMask: centre = the stock centre texel, corners = outside, the edge midpoints dark (ramp INSIDE the ellipse), 0.9 of the
        //     semi-axis fully inside, a circle in normalised space (the same coverage on both axes), and the whole mip chain
        {
            const Rgba in = {200, 200, 200, 255}, out = {0, 0, 0, 255};
            std::vector<std::vector<Rgba>> mips;
            BuildRoundMask(512, 256, 10, in, out, mips);
            const std::vector<Rgba>& l0 = mips[0];
            auto at = [&](int x, int y) { return l0[(size_t)y * 512 + (size_t)x]; };
            const bool centre = at(256, 128).r == 200 && at(255, 127).r == 200;
            // (40, 20) is at about 1.19 of the semi-axes on the diagonal: outside
            const bool corners = at(0, 0).r == 0 && at(511, 0).r == 0 && at(0, 255).r == 0 && at(511, 255).r == 0 && at(40, 20).r == 0;
            const bool edges = at(0, 128).r < 50 && at(256, 0).r < 50 && at(511, 128).r < 50 && at(256, 255).r < 50;   // under a quarter of the inside
            const bool inner = at(256 + 230, 128).r == 200 && at(256, 128 + 115).r == 200;   // 0.9 of each semi-axis
            const bool chain = mips.size() == 10 && mips[9].size() == 1 && mips[8].size() == 2 && mips[1].size() == 256u * 128u;
            // a circle in normalised space: the half-value crossing along +x and along +y at the same fraction of the semi-axis (within a texel of y)
            int xh = 256, yv = 128;
            while (xh < 511 && at(xh, 128).r >= 100)
                ++xh;
            while (yv < 255 && at(256, yv).r >= 100)
                ++yv;
            const double fx = (xh - 256) / 256.0, fy = (yv - 128) / 128.0;
            const bool sym = std::fabs(fx - fy) <= 1.0 / 128.0 && fx > 0.97;
            Check("ring map round: our mask is the ellipse inscribed in the texture (centre = the stock centre texel, corners outside, edge midpoints dark, "
                  "0.9 of each semi-axis fully in), the same shape on both axes, every mip",
                  centre && corners && edges && inner && chain && sym,
                  F("c %d k %d e %d (%d %d) i %d chain %d sym %d (%.4f / %.4f)", (int)centre, (int)corners, (int)edges, at(0, 128).r, at(256, 0).r, (int)inner,
                    (int)chain, (int)sym, fx, fy));
        }
        // D3. the codecs: BC1 / BC3 / BC4 / RGBA8 / A8 round trips of our mask (exact where lossless, small mean error where block-compressed),
        //     the stock-looking soft rectangle survives BC1 encode -> decode -> AnalyzeStockMask
        {
            const Rgba in = {200, 200, 200, 255}, out = {0, 0, 0, 255}, inA = {255, 255, 255, 255}, outA = {0, 0, 0, 0};
            std::vector<std::vector<Rgba>> m, ma;
            BuildRoundMask(64, 32, 1, in, out, m);
            BuildRoundMask(64, 32, 1, inA, outA, ma);
            std::string got;
            bool ok = true;
            struct Case { TexFmt f; int channel; double tol; };
            for (const Case c : {Case{TexFmt::RGBA8, 0, 0.0}, Case{TexFmt::BGRA8, 0, 0.0}, Case{TexFmt::A8, 1, 0.0}, Case{TexFmt::R8, 2, 0.0},
                                 Case{TexFmt::BC1, 0, 3.0}, Case{TexFmt::BC3, 1, 3.0}, Case{TexFmt::BC4, 2, 3.0}})
            {
                const std::vector<Rgba>& src = (c.f == TexFmt::A8 || c.f == TexFmt::BC3) ? ma[0] : m[0];
                std::vector<uint8_t> enc;
                std::vector<Rgba> dec;
                const bool e = EncodeLevel(c.f, src, 64, 32, enc), d = e && DecodeLevel(c.f, enc.data(), enc.size(), 64, 32, dec);
                const double err = d ? MeanAbsErr(src, dec, c.channel) : 1e9;
                const bool sized = enc.size() == LevelBytes(c.f, 64, 32);
                if (!(e && d && sized && err <= c.tol))
                    ok = false;
                char b[64];
                std::snprintf(b, sizeof(b), "%s %.2f%s; ", TexFmtName(c.f), err, sized ? "" : " (size!)");
                got += b;
            }
            const std::vector<Rgba> soft = SoftRect(512, 256, 255, 0, 13);
            std::vector<uint8_t> enc;
            std::vector<Rgba> dec;
            const bool sr = EncodeLevel(TexFmt::BC1, soft, 512, 256, enc) && DecodeLevel(TexFmt::BC1, enc.data(), enc.size(), 512, 256, dec) &&
                            AnalyzeStockMask(TexFmt::BC1, dec, 512, 256).ok;
            // a one-channel BC4 mask (the GPU reads (r, 0, 0, 1)) analyses by its red channel, not by a luminance of it
            std::vector<uint8_t> enc4;
            std::vector<Rgba> dec4;
            const StockMask s4 = EncodeLevel(TexFmt::BC4, soft, 512, 256, enc4) && DecodeLevel(TexFmt::BC4, enc4.data(), enc4.size(), 512, 256, dec4)
                                     ? AnalyzeStockMask(TexFmt::BC4, dec4, 512, 256)
                                     : StockMask{};
            const bool r4 = s4.ok && s4.insideValue >= 250;
            Check("ring map round: codec round trips (RGBA8 / BGRA8 / A8 / R8 exact, BC1 / BC3 alpha / BC4 mean error <= 3 of 255, sizes per format); a soft "
                  "rectangle still analyses as a mask after BC1 and after BC4 (by its red channel: centre 255, not 76)",
                  ok && sr && r4, got + F("BC4 centre %d", s4.insideValue));
        }
        // D4. (0.5.6) the stricter mask shape: the soft rectangle is symmetric and flat inside; a soft rectangle with one long ramp (bright centre,
        //     dark edges, but lopsided) and a grey Gaussian blob (bright centre, dark rim, no flat inside) are refused, each for its own reason
        {
            const std::vector<Rgba> m = SoftRect(512, 512, 255, 0, 33);
            std::vector<Rgba> lop((size_t)512 * 512), blob((size_t)512 * 512);
            for (int y = 0; y < 512; ++y)
                for (int x = 0; x < 512; ++x)
                {
                    const int dl = x, dr = 511 - x, dy = y < 511 - y ? y : 511 - y;
                    const float tl = dl >= 13 ? 1.0f : dl / 13.0f, tr = dr >= 180 ? 1.0f : dr / 180.0f, ty = dy >= 13 ? 1.0f : dy / 13.0f;
                    float t = tl < tr ? tl : tr;
                    t = t < ty ? t : ty;
                    const uint8_t g = (uint8_t)(255.0f * t + 0.5f);
                    lop[(size_t)y * 512 + (size_t)x] = {g, g, g, 255};
                    const double rx = (x - 255.5) / 256.0, ry = (y - 255.5) / 256.0;
                    const uint8_t b = (uint8_t)(255.0 * std::exp(-(rx * rx + ry * ry) / (2.0 * 0.3 * 0.3)) + 0.5);
                    blob[(size_t)y * 512 + (size_t)x] = {b, b, b, 255};
                }
            const StockMask a = AnalyzeStockMask(TexFmt::BC1, m, 512, 512), b = AnalyzeStockMask(TexFmt::BC1, lop, 512, 512),
                            c = AnalyzeStockMask(TexFmt::BC1, blob, 512, 512);
            Check("ring map round 0.5.6: the soft rectangle is mirror-symmetric and flat inside (a mask); a lopsided one and a grey Gaussian blob are not",
                  a.ok && a.symErr < 0.5 && a.flat > 0.6 && !b.ok && b.why.find("symmetric") != std::string::npos && !c.ok &&
                      c.why.find("flat inside") != std::string::npos,
                  a.why + " | " + b.why + " | " + c.why);
        }
        // F1. (0.5.6) the census pick, complete census: exactly one mask-like view is found (wherever it sits among 70); none, two, and an empty
        //     census are no pick
        {
            std::vector<CensusEntry> e(70);
            for (int i = 0; i < 70; ++i)
                e[(size_t)i] = {0x1000u + (uint64_t)i, true, false};
            std::vector<CensusEntry> one = e, none = e, two = e;
            one[41].shape = true;
            two[41].shape = two[7].shape = true;
            const CensusPick p1 = PickMask(one, 0, nullptr, 0), p0 = PickMask(none, 0, nullptr, 0), p2 = PickMask(two, 0, nullptr, 0),
                             pe = PickMask({}, 0, nullptr, 0);
            Check("ring map round 0.5.6: census pick: the one mask-like view of 70 is found; none -> no mask, two -> ambiguous, an empty census -> no view",
                  p1.verdict == PickVerdict::Found && p1.index == 41 && p1.Ok() && p0.verdict == PickVerdict::NoMaskShape && !p0.Ok() &&
                      p2.verdict == PickVerdict::Ambiguous && !p2.Ok() && pe.verdict == PickVerdict::NoViews && !pe.Ok(),
                  PickText(p1) + " | " + PickText(p0) + " | " + PickText(p2) + " | " + PickText(pe));
        }
        // F2. (0.5.6) an incomplete census never picks, even with exactly one mask-like view among those it holds: views past the cap (or binds it
        //     could not classify), and a view whose readback failed
        {
            std::vector<CensusEntry> e(64);
            for (int i = 0; i < 64; ++i)
                e[(size_t)i] = {0x2000u + (uint64_t)i, true, i == 9};
            std::vector<CensusEntry> unread = e;
            unread[50].read = false;
            const CensusPick po = PickMask(e, 3, nullptr, 0), pu = PickMask(unread, 0, nullptr, 0), pc = PickMask(e, 0, nullptr, 0);
            Check("ring map round 0.5.6: census pick: 3 views not recorded -> incomplete (no pick), one view not read back -> incomplete; the same census "
                  "complete -> found (the control)",
                  po.verdict == PickVerdict::Incomplete && !po.Ok() && pu.verdict == PickVerdict::Incomplete && !pu.Ok() && pc.verdict == PickVerdict::Found &&
                      pc.index == 9,
                  PickText(po) + " | " + PickText(pu) + " | " + PickText(pc));
        }
        // F3. (0.5.6) a pinned stock hash: the one mask-like view carrying it wins over other mask-like views and over an incomplete census; a pinned
        //     hash on a view that does not look like a mask does not count; two pinned views are ambiguous
        {
            const uint64_t pins[2] = {0xabcdef0123456789ULL, 0x1111222233334444ULL};
            std::vector<CensusEntry> e(20);
            for (int i = 0; i < 20; ++i)
                e[(size_t)i] = {0x3000u + (uint64_t)i, true, false};
            e[3].shape = e[12].shape = true;
            e[12].hash = pins[0];
            std::vector<CensusEntry> noShape = e;
            noShape[12].shape = false;
            noShape[3].shape = false;
            std::vector<CensusEntry> twoPins = e;
            twoPins[3].hash = pins[1];
            const CensusPick pa = PickMask(e, 0, pins, 2), pi = PickMask(e, 5, pins, 2), pn = PickMask(noShape, 0, pins, 2), p2 = PickMask(twoPins, 0, pins, 2),
                             pz = PickMask(e, 0, nullptr, 0), p2i = PickMask(twoPins, 5, pins, 2);
            Check("ring map round 0.5.6: census pick: a pinned hash + mask shape wins over a second mask-like view and over 5 unrecorded views; pinned "
                  "without the shape -> no mask; two pinned -> ambiguous (also in an incomplete census: the pinned conflict is the reason); no pins -> ambiguous (the control)",
                  pa.verdict == PickVerdict::FoundPinned && pa.index == 12 && pi.verdict == PickVerdict::FoundPinned && pi.index == 12 &&
                      pn.verdict == PickVerdict::NoMaskShape && !pn.Ok() && p2.verdict == PickVerdict::Ambiguous && !p2.Ok() &&
                      pz.verdict == PickVerdict::Ambiguous && p2i.verdict == PickVerdict::Ambiguous && !p2i.Ok(),
                  PickText(pa) + " | " + PickText(pi) + " | " + PickText(pn) + " | " + PickText(p2) + " | " + PickText(pz) + " | " + PickText(p2i));
        }
        // E1. the blip clip pattern on a synthetic image: unique hit, the flag resolved through the rip-relative displacement; a changed check
        //     byte is refused (the pin is binding)
        {
            std::vector<uint8_t> img(0x4000, 0xCC);
            const size_t hit = 0x1200;
            const uint8_t code[] = {0x8A, 0x15, 0, 0, 0, 0, 0xF3, 0x0F, 0x10, 0x15, 0x11, 0x22, 0x33, 0x44, 0x33, 0xC9, 0x84, 0xD2, 0x74, 0x05};
            std::memcpy(&img[hit], code, sizeof(code));
            const int32_t disp = (int32_t)(0x3000 - (hit + 6));
            std::memcpy(&img[hit + 2], &disp, 4);
            img[0x3000] = 1;
            const uint8_t* hits[4] = {};
            const int n = gage::sync::FindLayoutHits(kClipLayoutLegacy, img.data(), img.size(), hits, 4, 0);
            const ClipResolve r = n == 1 ? ResolveClipFlag(hits[0], img.data(), img.size()) : ClipResolve{};
            std::vector<uint8_t> bad = img;
            bad[hit + 0x10] = 0x85;   // test dl, dl -> test edx, edx: not the code the pin was read from
            const ClipResolve rb = ResolveClipFlag(bad.data() + hit, bad.data(), bad.size());
            Check("ring map round: the blip clip pattern hits once in a synthetic image and resolves the flag at its rip-relative address (holding 1); a "
                  "changed instruction after the load is refused",
                  n == 1 && hits[0] == img.data() + hit && r.ok && r.flagOff == 0x3000 && img[r.flagOff] == 1 && !rb.ok,
                  F("hits %d off 0x%zx ok %d bad %d (%s)", n, r.flagOff, (int)r.ok, (int)rb.ok, rb.why.c_str()));
        }
        // E2. a flag displacement past the image is refused
        {
            std::vector<uint8_t> img(0x4000, 0xCC);
            const size_t hit = 0x1200;
            const uint8_t code[] = {0x8A, 0x15, 0, 0, 0, 0, 0xF3, 0x0F, 0x10, 0x15, 0x11, 0x22, 0x33, 0x44, 0x33, 0xC9, 0x84, 0xD2, 0x74, 0x05};
            std::memcpy(&img[hit], code, sizeof(code));
            const int32_t disp = 0x100000;
            std::memcpy(&img[hit + 2], &disp, 4);
            const ClipResolve r = ResolveClipFlag(img.data() + hit, img.data(), img.size());
            Check("ring map round: a blip clip flag address outside the image is refused", !r.ok, r.why);
        }
    }

    // The real thing: both Legacy 3889 dumps. Not counted in kChecks (the dumps are not in the repo): a failure is a failure, no dump is a loud SKIP.
    inline void RunDump()
    {
        const std::vector<std::string> dumps = minimapsizetest::FindLegacyDumps();
        if (dumps.empty())
        {
            std::printf("SKIP  ring map round blip clip dump: no Legacy 3889 dump (GTA5.exe-*.img) in %%LOCALAPPDATA%%\\GAGE-MP\\dump or GAGE_DUMP_DIR; the "
                        "pattern was NOT checked against the real image\n");
            return;
        }
        for (const std::string& path : dumps)
        {
            std::vector<uint8_t> img;
            if (!minimapsizetest::ReadFileBytes(path, img) || img.size() != 65387520)
            {
                std::printf("SKIP  ring map round blip clip dump: %s is not a readable 65387520-byte Legacy image\n", path.c_str());
                continue;
            }
            const uint8_t* hits[4] = {};
            const int n = gage::sync::FindLayoutHits(kClipLayoutLegacy, img.data(), img.size(), hits, 4, 0);
            const ClipResolve r = n == 1 ? ResolveClipFlag(hits[0], img.data(), img.size()) : ClipResolve{};
            const size_t hitRva = n == 1 ? (size_t)(hits[0] - img.data()) : 0;
            if (n != 1 || !r.ok || hitRva != kClipHit3889 || r.flagOff != kClipFlag3889 || img[r.flagOff] != 1)
            {
                std::printf("FAIL  ring map round blip clip dump %s: hits %d, hit RVA 0x%zx (want 0x%x), resolve %s, flag RVA 0x%zx (want 0x%x) value %d (want 1)\n",
                            path.c_str(), n, hitRva, kClipHit3889, r.ok ? "ok" : r.why.c_str(), r.flagOff, kClipFlag3889, r.ok ? img[r.flagOff] : -1);
                ++g_failed;
                continue;
            }
            std::printf("ok    ring map round blip clip dump (%s): pattern unique at RVA 0x%zx, flag at RVA 0x%zx holding 1 (minimapIsRect = rectangle)\n",
                        path.c_str(), hitRva, r.flagOff);
        }
    }

} // namespace ringround_t
