// 0.4.25 minimap_scale (libs/sync/minimap_size.h): the registry value, the scaling of the game's three minimap components, the gate that decides when to
// write the table and when to write it again, and the pattern + the game's frontend.xml reader checked on a synthetic image and, when it is present,
// on the decrypted 3889 dump. Included by tests/minimap_tests.h (after gameplay_test.cpp's Check()); run by minimapsizetest::RunPure() (kChecks checks) and
// minimapsizetest::RunDump() (extra, counted apart: it prints SKIP when the dump is not on this machine and adds to g_failed on a real failure).
// Every check was run against a mutant of the rule it pins (the list is in the 0.4.25 report).
#pragma once

#include "sync/minimap_size.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace minimapsizetest {

    constexpr int kChecks = 30;   // 23 for 0.4.25 + 7 for the Enhanced layout (0.4.26)

    using namespace gage::sync;

    inline bool Near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; }

    // The values the community documents for frontend.xml (FiveM forum / Lua snippets; the real stock values are logged live on first use).
    inline MinimapComp Comp(const char* name, float px, float py, float sx, float sy, char ax = 'L', char ay = 'B')
    {
        MinimapComp c;
        c.name = name;
        c.alignX = ax;
        c.alignY = ay;
        c.posX = px;
        c.posY = py;
        c.sizeX = sx;
        c.sizeY = sy;
        return c;
    }
    inline MinimapComp StockMinimap() { return Comp("minimap", -0.0045f, 0.002f, 0.150f, 0.188888f); }
    inline MinimapComp StockMask() { return Comp("minimap_mask", 0.020f, 0.030f, 0.111f, 0.159f); }
    inline MinimapComp StockBlur() { return Comp("minimap_blur", -0.03f, 0.022f, 0.266f, 0.237f); }

    // A simulated game table: 11 elements of 120 bytes, laid out like the game's.
    struct SimTable
    {
        std::vector<uint8_t> b = std::vector<uint8_t>((size_t)kMinimapEntries * kMinimapStride, 0);

        void Put(int slot, const MinimapComp& c)
        {
            uint8_t* e = &b[(size_t)slot * kMinimapStride];
            std::memset(e, 0, kMinimapStride);
            std::memcpy(e, c.name.c_str(), c.name.size());
            EncodeMinimapGeometry(e, c);
            e[kOffAlignX] = (uint8_t)c.alignX;
            e[kOffAlignY] = (uint8_t)c.alignY;
        }
        void PutStock()
        {
            Put(0, StockMinimap());
            Put(1, StockMask());
            Put(2, StockBlur());
            Put(3, Comp("big_minimap", 0.0f, 0.0f, 0.5f, 0.5f));
        }
        MinimapTrio Read() const
        {
            MinimapComp all[kMinimapEntries];
            for (int i = 0; i < kMinimapEntries; ++i)
                all[i] = DecodeMinimapComp(&b[(size_t)i * kMinimapStride]);
            return PickTrio(all, kMinimapEntries);
        }
        MinimapComp Slot(int i) const { return DecodeMinimapComp(&b[(size_t)i * kMinimapStride]); }
    };

    // One engine step: read the table, ask the gate, perform what it says (the engine's write, minus the guards).
    inline MinimapSizeGate::Act Step(MinimapSizeGate& g, SimTable& t, double now)
    {
        const MinimapTrio cur = t.Read();
        const MinimapSizeGate::Act a = g.Next(cur, true, now);
        if (a != MinimapSizeGate::Act::None)
            for (int r = 0; r < 3; ++r)
                EncodeMinimapGeometry(&t.b[(size_t)g.Target().index[r] * kMinimapStride], g.Target().c[r]);
        return a;
    }

    // Every position where the pattern's bytes (no wildcards) occur in a flat image.
    inline std::vector<size_t> FindAll(const uint8_t* img, size_t n, const char* sig)
    {
        std::vector<uint8_t> pat;
        const int len = HexByteCount(sig);
        for (int i = 0; i < len; ++i)
            pat.push_back((uint8_t)HexByteAt(sig, i));
        std::vector<size_t> hits;
        if (pat.empty() || n < pat.size())
            return hits;
        for (size_t i = 0; i + pat.size() <= n; ++i)
        {
            const uint8_t* p = static_cast<const uint8_t*>(std::memchr(img + i, pat[0], n - pat.size() - i + 1));
            if (!p)
                break;
            i = (size_t)(p - img);
            if (std::memcmp(p, pat.data(), pat.size()) == 0)
                hits.push_back(i);
        }
        return hits;
    }

    // A small image with the pattern, the game's reader around it and a table: what ResolveMinimapTable needs, laid out as in the 3889 image.
    struct SynthImage
    {
        std::vector<uint8_t> img = std::vector<uint8_t>(0x20000, 0);
        size_t hit = 0x4000, table = 0x10000;

        void Build()
        {
            std::fill(img.begin(), img.end(), (uint8_t)0);
            const int n = HexByteCount(kMinimapTableSig);
            for (int i = 0; i < n; ++i)
                img[hit + (size_t)i] = (uint8_t)HexByteAt(kMinimapTableSig, i);
            const int32_t disp = (int32_t)((int64_t)table - ((int64_t)hit + 22));
            std::memcpy(&img[hit + kMinimapTableDispAt], &disp, 4);
            for (const LoaderCheck& c : kLoaderChecks)
            {
                const int m = HexByteCount(c.bytes);
                for (int i = 0; i < m; ++i)
                    img[hit + (size_t)c.off + (size_t)i] = (uint8_t)HexByteAt(c.bytes, i);
            }
            img[hit - 0x16] = 0x48;
            img[hit - 0x15] = 0x8D;
            img[hit - 0x14] = 0x35;
            const int32_t rd = (int32_t)((int64_t)table + kOffPosY - ((int64_t)hit - 0x16 + 7));
            std::memcpy(&img[hit - 0x13], &rd, 4);
        }
        TableResolve Resolve() const { return ResolveMinimapTable(&img[hit], img.data(), img.size()); }
    };

    // The same for the Enhanced layout (0.4.26): the pattern (wildcards filled with 0xA5, the table disp32 computed) and the reader's instructions at their offsets.
    struct SynthImageEnh
    {
        std::vector<uint8_t> img = std::vector<uint8_t>(0x20000, 0);
        size_t hit = 0x4000, table = 0x10000;

        void Build()
        {
            const MinimapLayout& L = kMinimapLayoutEnhanced;
            std::fill(img.begin(), img.end(), (uint8_t)0);
            for (int i = 0; i < L.sigLen; ++i)
            {
                const int t = PatTokenAt(L.sig, i);
                img[hit + (size_t)i] = t >= 0 ? (uint8_t)t : (uint8_t)0xA5;
            }
            const int32_t disp = (int32_t)((int64_t)table - ((int64_t)hit + L.endAt));
            std::memcpy(&img[hit + (size_t)L.dispAt], &disp, 4);
            for (int k = 0; k < L.nChecks; ++k)
            {
                const int m = HexByteCount(L.checks[k].bytes);
                for (int i = 0; i < m; ++i)
                    img[hit + (size_t)L.checks[k].off + (size_t)i] = (uint8_t)HexByteAt(L.checks[k].bytes, i);
            }
        }
        TableResolve Resolve() const { return ResolveMinimapTableFor(kMinimapLayoutEnhanced, &img[hit], img.data(), img.size()); }
        int Hits(const MinimapLayout& L) const
        {
            const uint8_t* h[4] = {};
            return FindLayoutHits(L, img.data(), img.size(), h, 4, 0);
        }
    };

    inline void RunPure()
    {
        std::printf("-- 0.4.25 minimap_scale: registry value, component scaling, write gate, table pattern\n");

        // 1. the registry value.
        {
            const ParsedScale unset = ParseMinimapScale(L""), blank = ParseMinimapScale(L"  "), p08 = ParseMinimapScale(L"0.8"), dot = ParseMinimapScale(L".7");
            const ParsedScale one = ParseMinimapScale(L"1"), one0 = ParseMinimapScale(L"1.0"), padded = ParseMinimapScale(L" 0.65\t"), half = ParseMinimapScale(L"0.5");
            Check("minimap_scale: unset or blank = 0.80 (GI asked for smaller); 0.8, .7, 0.65 (padded), 0.5 read as numbers; 1 and 1.0 = 1 (untouched)",
                  Near(unset.scale, 0.80f) && unset.src == ScaleSource::Unset && Near(blank.scale, 0.80f) && blank.src == ScaleSource::Unset && Near(p08.scale, 0.8f)
                      && p08.src == ScaleSource::Set && Near(dot.scale, 0.7f) && Near(padded.scale, 0.65f) && Near(half.scale, 0.5f) && half.src == ScaleSource::Set
                      && one.scale == 1.0f && one0.scale == 1.0f && one.src == ScaleSource::Set,
                  std::to_string(unset.scale) + " " + std::to_string(p08.scale) + " " + std::to_string(one.scale));
        }
        {
            const ParsedScale lo = ParseMinimapScale(L"0.3"), zero = ParseMinimapScale(L"0"), hi = ParseMinimapScale(L"2"), huge = ParseMinimapScale(L"99999");
            Check("minimap_scale: below 0.5 is held to 0.5, above 1 to 1.0 (untouched), each reported as clamped",
                  lo.scale == 0.5f && lo.src == ScaleSource::Clamped && zero.scale == 0.5f && zero.src == ScaleSource::Clamped && hi.scale == 1.0f
                      && hi.src == ScaleSource::Clamped && huge.scale == 1.0f);
        }
        {
            const wchar_t* junk[] = {L"abc", L"0.8x", L"0,8", L"-0.7", L"1e0", L".", L"0.8.1", L"8 0", L"1234567", L"0.12345678901"};
            int good = 0;
            for (const wchar_t* j : junk)
            {
                const ParsedScale p = ParseMinimapScale(j);
                good += (p.src == ScaleSource::Junk && Near(p.scale, 0.80f)) ? 1 : 0;
            }
            Check("minimap_scale: anything that is not a plain decimal (letters, comma, sign, exponent, two dots, too many digits) falls back to 0.80 as junk",
                  good == 10, std::to_string(good) + " of 10");
        }

        // 2. scaling the components.
        {
            const MinimapComp m = ScaledComp(StockMinimap(), 0.8f);
            Check("minimap_scale: 0.8 turns the stock minimap frame (0.150 x 0.188888 at -0.0045, 0.002) into 0.120 x 0.151110 at -0.0036, 0.0016; name and alignment stay",
                  Near(m.sizeX, 0.12f) && Near(m.sizeY, 0.1511104f) && Near(m.posX, -0.0036f) && Near(m.posY, 0.0016f) && m.name == "minimap" && m.alignX == 'L'
                      && m.alignY == 'B');
        }
        {
            // The mask and the blur stay where they were inside the frame: every edge, as a fraction of the frame, is unchanged, whatever the factor.
            // The control scales only the sizes: the mask's top edge then leaves the frame (0.030 + 0.159 > 0.002 + 0.188888 * 0.8 ... for small factors).
            auto top = [](const MinimapComp& c) { return c.posY + c.sizeY; };
            auto right = [](const MinimapComp& c) { return c.posX + c.sizeX; };
            int good = 0, controlBroken = 0;
            for (float s : {1.0f, 0.9f, 0.8f, 0.65f, 0.5f})
            {
                const MinimapComp f = ScaledComp(StockMinimap(), s), k = ScaledComp(StockMask(), s);
                const bool inside = k.posX >= f.posX && k.posY >= f.posY && right(k) <= right(f) && top(k) <= top(f);
                const MinimapComp f0 = StockMinimap(), k0 = StockMask();
                const bool sameFractions = Near((top(k) - f.posY) / f.sizeY, (top(k0) - f0.posY) / f0.sizeY, 2e-4f)
                                           && Near((k.posX - f.posX) / f.sizeX, (k0.posX - f0.posX) / f0.sizeX, 2e-4f);
                good += (inside && sameFractions) ? 1 : 0;
                MinimapComp ks = StockMask(), fs = StockMinimap();   // sizes only
                ks.sizeX *= s; ks.sizeY *= s; fs.sizeX *= s; fs.sizeY *= s;
                controlBroken += (top(ks) > top(fs)) ? 1 : 0;
            }
            Check("minimap_scale: scaling position AND size keeps the mask inside the frame at the same fractions for 1.0..0.5; the sizes-only control pushes the mask out of the frame",
                  good == 5 && controlBroken >= 3, std::to_string(good) + " of 5 inside, control broken " + std::to_string(controlBroken));
        }
        {
            Check("minimap_scale: the factor is held to 0.5..1 inside ScaledComp too (0.2 acts as 0.5, 3 as 1, NaN as 1)",
                  Near(ScaledComp(StockMinimap(), 0.2f).sizeX, 0.075f) && Near(ScaledComp(StockMinimap(), 3.0f).sizeX, 0.150f)
                      && Near(ScaledComp(StockMinimap(), std::nanf("")).sizeX, 0.150f));
        }

        // 3. what counts as a stock component.
        {
            MinimapComp zero;                                  // an empty slot
            MinimapComp nan = StockMinimap();
            nan.sizeX = std::nanf("");
            MinimapComp right = StockMinimap();
            right.alignX = 'R';
            MinimapComp big = StockMinimap();
            big.sizeY = 0.9f;
            MinimapComp offCorner = StockMinimap();
            offCorner.posX = 0.5f;
            std::string why;
            const bool stockOk = PlausibleComp(StockMinimap()) && PlausibleComp(StockMask()) && PlausibleComp(StockBlur());
            Check("minimap_scale: the stock components are plausible; an empty slot, a NaN, a right-aligned one, a 0.9-high one and one 0.5 from the corner are not",
                  stockOk && !PlausibleComp(zero, &why) && !why.empty() && !PlausibleComp(nan) && !PlausibleComp(right) && !PlausibleComp(big) && !PlausibleComp(offCorner));
        }

        // 4. the element codec.
        {
            std::vector<uint8_t> e(kMinimapStride, 0xAA);
            std::memset(e.data(), 0, kMinimapStride);
            std::memcpy(e.data(), "minimap_mask", 12);
            const float v[4] = {0.020f, 0.030f, 0.111f, 0.159f};
            std::memcpy(&e[100], &v[0], 4); std::memcpy(&e[104], &v[1], 4); std::memcpy(&e[108], &v[2], 4); std::memcpy(&e[112], &v[3], 4);
            e[116] = 'L';
            e[117] = 'B';
            const MinimapComp c = DecodeMinimapComp(e.data());
            // A name without a terminator in its 100 bytes, and one with a control character, are not names.
            std::vector<uint8_t> full(kMinimapStride, 'a');
            std::vector<uint8_t> ctrl(kMinimapStride, 0);
            std::memcpy(ctrl.data(), "mini\x01map", 8);
            Check("minimap_scale: an element decodes as the game lays it out (name, posX +100, posY +104, sizeX +108, sizeY +112, alignX +116, alignY +117); an unterminated or "
                  "control-character name is empty",
                  c.name == "minimap_mask" && c.posX == v[0] && c.posY == v[1] && c.sizeX == v[2] && c.sizeY == v[3] && c.alignX == 'L' && c.alignY == 'B'
                      && DecodeMinimapComp(full.data()).name.empty() && DecodeMinimapComp(ctrl.data()).name.empty());
        }
        {
            SimTable t;
            t.PutStock();
            const std::vector<uint8_t> before = t.b;
            MinimapComp m = StockMinimap();
            m.posX = 1.0f; m.posY = 2.0f; m.sizeX = 3.0f; m.sizeY = 4.0f; m.name = "ZZZ"; m.alignX = 'R';
            EncodeMinimapGeometry(&t.b[0], m);
            size_t changed = 0, outside = 0;
            for (size_t i = 0; i < t.b.size(); ++i)
                if (t.b[i] != before[i])
                {
                    ++changed;
                    if (!(i >= 100 && i < 116)) ++outside;
                }
            float f = 0;
            std::memcpy(&f, &t.b[112], 4);
            Check("minimap_scale: EncodeMinimapGeometry writes exactly bytes 100..115 of the element (the four floats), never the name, the alignment or another element",
                  changed > 0 && outside == 0 && f == 4.0f && t.Slot(0).name == "minimap" && t.Slot(0).alignX == 'L');
        }
        {
            SimTable t;
            t.PutStock();
            t.Put(5, Comp("MINIMAP_Mask", 0.5f, 0.5f, 0.5f, 0.5f));      // a later duplicate (different case) must lose to slot 1
            t.Put(6, Comp("minimap_blur", 0.1f, 0.1f, 0.1f, 0.1f));
            const MinimapTrio tr = t.Read();
            SimTable empty;
            SimTable partial;
            partial.Put(0, StockMinimap());
            Check("minimap_scale: the three components are found by name (case-insensitive, the first one wins, other names ignored); a table without all three is not 'found'",
                  tr.AllFound() && tr.index[0] == 0 && tr.index[1] == 1 && tr.index[2] == 2 && tr.c[1].posX == 0.020f && !empty.Read().AllFound() && !partial.Read().AllFound());
        }

        // 5. the gate.
        {
            MinimapSizeGate g;
            g.Configure(0.8f);
            SimTable t;                                             // unpopulated: the game has not parsed frontend.xml yet
            int acts = 0;
            double now = 0;
            for (int i = 0; i < 40; ++i, now += 250.0)
                acts += Step(g, t, now) != MinimapSizeGate::Act::None ? 1 : 0;
            const bool waited = acts == 0 && g.PhaseNow() == MinimapSizeGate::Phase::Waiting && !g.Why().empty() && !g.HasOriginal();
            t.PutStock();
            const MinimapSizeGate::Act first = Step(g, t, now);
            now += 250.0;
            const MinimapSizeGate::Act second = Step(g, t, now);
            const bool sized = Near(t.Slot(0).sizeX, 0.12f) && Near(t.Slot(1).sizeX, 0.111f * 0.8f) && Near(t.Slot(2).sizeY, 0.237f * 0.8f) && t.Slot(3).sizeX == 0.5f;
            Check("minimap_scale gate: it waits while the table is empty (10 s, nothing written); the first populated plausible read is applied once, the originals kept, "
                  "and only the three components change",
                  waited && first == MinimapSizeGate::Act::Apply && second == MinimapSizeGate::Act::None && g.HasOriginal() && sized
                      && SameComp(g.Original().c[0], StockMinimap()) && g.Counters().applies == 1);
        }
        {
            MinimapSizeGate g;
            g.Configure(0.8f);
            SimTable t;
            t.PutStock();
            double now = 0;
            Step(g, t, now);
            const std::vector<uint8_t> applied = t.b;
            int acts = 0;
            for (int i = 0; i < 60; ++i)
            {
                now += 250.0;
                acts += Step(g, t, now) != MinimapSizeGate::Act::None ? 1 : 0;
            }
            Check("minimap_scale gate: a table that still holds our sizes is looked at once a second for 15 s and never written again",
                  acts == 0 && t.b == applied && g.Counters().checks >= 14 && g.Counters().checks <= 16 && g.Counters().reasserts == 0, std::to_string(g.Counters().checks));
        }
        {
            // The game resets the table to the stock sizes (a reload of frontend.xml): written again from the ORIGINALS. Three resets give the same sizes as one:
            // scaling the target instead of the original would shrink the map a little more every time.
            MinimapSizeGate g;
            g.Configure(0.8f);
            SimTable t;
            t.PutStock();
            double now = 0;
            Step(g, t, now);
            const std::vector<uint8_t> applied = t.b;
            int reasserts = 0;
            for (int k = 0; k < 3; ++k)
            {
                t.PutStock();
                for (int i = 0; i < 8; ++i)
                {
                    now += 250.0;
                    reasserts += Step(g, t, now) == MinimapSizeGate::Act::Reassert ? 1 : 0;
                }
            }
            Check("minimap_scale gate: a table the game reset to the stock sizes is written again within a second, from the saved originals: after 3 resets the sizes are the "
                  "same as after the first write",
                  reasserts == 3 && t.b == applied && g.Counters().reasserts == 3 && g.Counters().adopted == 0);
        }
        {
            // Only the frame goes back to stock; the mask and the blur still hold ours and must not be scaled a second time.
            MinimapSizeGate g;
            g.Configure(0.8f);
            SimTable t;
            t.PutStock();
            double now = 0;
            Step(g, t, now);
            const MinimapComp maskOurs = t.Slot(1), blurOurs = t.Slot(2);
            t.Put(0, StockMinimap());
            MinimapSizeGate::Act a = MinimapSizeGate::Act::None;
            for (int i = 0; i < 8 && a == MinimapSizeGate::Act::None; ++i)
            {
                now += 250.0;
                a = Step(g, t, now);
            }
            Check("minimap_scale gate: when only one component goes back to stock only that one is rewritten; the other two keep exactly the sizes written before",
                  a == MinimapSizeGate::Act::Reassert && Near(t.Slot(0).sizeX, 0.12f) && SameComp(t.Slot(1), maskOurs) && SameComp(t.Slot(2), blurOurs)
                      && SameComp(g.Original().c[1], StockMask()));
        }
        {
            // A new plausible stock value from the game is adopted (scaled from there); garbage caught mid-write is waited out without a write.
            MinimapSizeGate g;
            g.Configure(0.8f);
            SimTable t;
            t.PutStock();
            double now = 0;
            Step(g, t, now);
            t.Put(0, Comp("minimap", -0.0045f, 0.002f, 0.200f, 0.250f));        // e.g. a different aspect ratio's stock size
            MinimapSizeGate::Act a = MinimapSizeGate::Act::None;
            for (int i = 0; i < 8 && a == MinimapSizeGate::Act::None; ++i)
            {
                now += 250.0;
                a = Step(g, t, now);
            }
            const bool adopted = a == MinimapSizeGate::Act::Reassert && Near(t.Slot(0).sizeX, 0.16f) && Near(t.Slot(0).sizeY, 0.2f) && g.Counters().adopted == 1
                                 && Near(g.Original().c[0].sizeX, 0.2f);
            t.Put(1, Comp("minimap_mask", 0, 0, 0, 0, 0, 0));                    // half-written by the game
            const std::vector<uint8_t> garbage = t.b;
            int acts = 0;
            for (int i = 0; i < 12; ++i)
            {
                now += 250.0;
                acts += Step(g, t, now) != MinimapSizeGate::Act::None ? 1 : 0;
            }
            Check("minimap_scale gate: a new plausible value from the game is adopted as the stock size and scaled from there; an implausible (half-written) component is "
                  "waited out with no write",
                  adopted && acts == 0 && t.b == garbage && g.Counters().implausible >= 2);
        }
        {
            // A game that rewrites the table over and over is not fought for ever.
            MinimapSizeGate g;
            g.Configure(0.8f);
            SimTable t;
            t.PutStock();
            double now = 0;
            Step(g, t, now);
            int writes = 0;
            for (int k = 0; k < 20 && g.PhaseNow() == MinimapSizeGate::Phase::Applied; ++k)
            {
                t.PutStock();
                for (int i = 0; i < 6; ++i)
                {
                    now += 250.0;
                    writes += Step(g, t, now) != MinimapSizeGate::Act::None ? 1 : 0;
                }
            }
            const std::vector<uint8_t> left = t.b;
            t.PutStock();
            const std::vector<uint8_t> stock = t.b;
            for (int i = 0; i < 12; ++i)
            {
                now += 250.0;
                Step(g, t, now);
            }
            Check("minimap_scale gate: after 8 re-asserts the next reset makes it give up (a reason is kept) and the table is never written again",
                  g.PhaseNow() == MinimapSizeGate::Phase::GaveUp && writes == (int)MinimapSizeGate::kMaxReasserts && !g.Why().empty() && t.b == stock && !left.empty(),
                  std::to_string(writes));
        }
        {
            MinimapSizeGate g1;
            g1.Configure(1.0f);
            SimTable t;
            t.PutStock();
            const std::vector<uint8_t> stock = t.b;
            int acts = 0;
            double now = 0;
            for (int i = 0; i < 100; ++i, now += 250.0)
                acts += Step(g1, t, now) != MinimapSizeGate::Act::None ? 1 : 0;
            // A factor changed after the write: the originals are re-targeted. 1.0 writes the stock sizes back once and then stops.
            MinimapSizeGate g;
            g.Configure(0.8f);
            SimTable u;
            u.PutStock();
            now = 0;
            Step(g, u, now);
            g.Configure(0.6f);
            now += 250.0;
            const MinimapSizeGate::Act toSix = Step(g, u, now);
            const bool six = Near(u.Slot(0).sizeX, 0.09f) && Near(u.Slot(0).posX, -0.0045f * 0.6f);
            g.Configure(1.0f);
            now += 250.0;
            const MinimapSizeGate::Act back = Step(g, u, now);
            now += 250.0;
            const MinimapSizeGate::Act done = Step(g, u, now);
            Check("minimap_scale gate: scale 1 never writes anything; a factor changed after the write re-targets the saved originals (0.8 -> 0.6, not 0.48), and 1 writes the "
                  "stock sizes back exactly once",
                  acts == 0 && g1.PhaseNow() == MinimapSizeGate::Phase::Off && t.b == stock && toSix == MinimapSizeGate::Act::Reassert && six
                      && back == MinimapSizeGate::Act::Reassert && done == MinimapSizeGate::Act::None && g.PhaseNow() == MinimapSizeGate::Phase::Off && u.b == stock);
        }
        {
            // A write that fails (the engine put the old bytes back) is tried again; three failures give up.
            MinimapSizeGate g;
            g.Configure(0.8f);
            SimTable t;
            t.PutStock();
            double now = 0;
            const MinimapSizeGate::Act a = g.Next(t.Read(), true, now);
            g.WriteFailed();
            now += 250.0;
            const MinimapSizeGate::Act retry = g.Next(t.Read(), true, now);
            g.WriteFailed();
            const bool stillTrying = g.PhaseNow() == MinimapSizeGate::Phase::Applied;
            now += 250.0;
            const MinimapSizeGate::Act retry2 = g.Next(t.Read(), true, now);
            g.WriteFailed();
            now += 250.0;
            const MinimapSizeGate::Act after = g.Next(t.Read(), true, now);
            Check("minimap_scale gate: a failed write is retried on the next call with the same target; the third failure gives up for good",
                  a == MinimapSizeGate::Act::Apply && retry == MinimapSizeGate::Act::Reassert && stillTrying && retry2 == MinimapSizeGate::Act::Reassert
                      && g.PhaseNow() == MinimapSizeGate::Phase::GaveUp && after == MinimapSizeGate::Act::None && g.Counters().writeFailures == 3
                      && SameComp(g.Target().c[0], ScaledComp(StockMinimap(), 0.8f)));
        }
        {
            // A table that never fills, or never looks like frontend.xml's (everything right-aligned), is given up on after 120 s with no write.
            MinimapSizeGate empty, wrong;
            empty.Configure(0.8f);
            wrong.Configure(0.8f);
            SimTable e, w;
            w.Put(0, Comp("minimap", -0.0045f, 0.002f, 0.150f, 0.188888f, 'R', 'T'));
            w.Put(1, StockMask());
            w.Put(2, StockBlur());
            int acts = 0;
            double now = 0;
            for (int i = 0; i < 520; ++i, now += 250.0)
                acts += (Step(empty, e, now) != MinimapSizeGate::Act::None ? 1 : 0) + (Step(wrong, w, now) != MinimapSizeGate::Act::None ? 1 : 0);
            Check("minimap_scale gate: a table that stays empty, or whose components are not left / bottom aligned, is never written and the gate gives up after 120 s with the reason",
                  acts == 0 && empty.PhaseNow() == MinimapSizeGate::Phase::GaveUp && wrong.PhaseNow() == MinimapSizeGate::Phase::GaveUp
                      && wrong.Why().find("not aligned") != std::string::npos && !empty.Why().empty());
        }
        {
            // Pacing: calls closer than 250 ms are one poll; the originals are read once.
            MinimapSizeGate g;
            g.Configure(0.8f);
            SimTable t;
            for (double now = 0; now < 240.0; now += 16.0)
                g.Next(t.Read(), true, now);
            const uint32_t p1 = g.Counters().polls;
            g.Next(t.Read(), true, 260.0);
            Check("minimap_scale gate: while waiting it polls at most every 250 ms (16 ms calls over 240 ms = 1 poll, the next at 260 ms = 2)", p1 == 1 && g.Counters().polls == 2,
                  std::to_string(p1));
        }

        // 6. the pattern and the game's reader.
        {
            SynthImage s;
            s.Build();
            const TableResolve ok = s.Resolve();
            const std::vector<size_t> hits = FindAll(s.img.data(), s.img.size(), kMinimapTableSig);
            Check("minimap_scale pattern: on an image laid out like 3889's (pattern, the game's frontend.xml reader around it, the table) the pattern hits once and resolves to the table",
                  hits.size() == 1 && hits[0] == s.hit && ok.ok && ok.tableOff == s.table, ok.why);
        }
        {
            int rejected = 0, total = 0;
            std::string missed;
            for (const LoaderCheck& c : kLoaderChecks)
            {
                SynthImage s;
                s.Build();
                s.img[s.hit + (size_t)c.off] ^= 0x01;
                const TableResolve r = s.Resolve();
                ++total;
                if (!r.ok && r.why.find("differs") != std::string::npos)
                    ++rejected;
                else
                    missed += std::string(c.what) + "; ";
            }
            Check("minimap_scale pattern: changing one byte of any of the 8 pinned reader instructions (stride, the five stores, the step, alignY) refuses the table with the reason",
                  rejected == total && total == 8, missed);
        }
        {
            SynthImage a, b, c, d, e;
            a.Build(); b.Build(); c.Build(); d.Build(); e.Build();
            a.img[a.hit - 0x14] = 0x3D;                                              // not `lea rsi, [rip+...]`
            const int32_t wrong = (int32_t)((int64_t)b.table + kOffPosY + 4 - ((int64_t)b.hit - 0x16 + 7));
            std::memcpy(&b.img[b.hit - 0x13], &wrong, 4);                             // the reader's element pointer is not table + 104
            const int32_t beyond = 0x7ff00000;
            std::memcpy(&c.img[c.hit + kMinimapTableDispAt], &beyond, 4);            // the table address leaves the image
            const TableResolve edge = ResolveMinimapTable(&d.img[0x10], d.img.data(), d.img.size());   // too close to the start to check the reader
            const TableResolve tail = ResolveMinimapTable(&e.img[e.img.size() - 0x40], e.img.data(), e.img.size());
            Check("minimap_scale pattern: no `lea rsi` before the hit, a reader whose element pointer is not table + 104, a table address outside the image and a hit at either "
                  "edge of the image are each refused",
                  !a.Resolve().ok && !b.Resolve().ok && b.Resolve().why.find("table + 104") != std::string::npos && !c.Resolve().ok && !edge.ok && !tail.ok);
        }

        // 7. the Enhanced layout (0.4.26): same table, its own pattern and reader instructions.
        {
            int wild = 0;
            for (int i = 0; i < kMinimapLayoutEnhanced.sigLen; ++i)
                wild += PatTokenAt(kMinimapLayoutEnhanced.sig, i) == -2 ? 1 : 0;
            // every byte a layout reads (its checks, the Legacy reader lea 0x16 before the hit, the pattern itself, the disp32) lies inside the span the resolver demands
            bool spans = true;
            for (const MinimapLayout* L : {&kMinimapLayoutLegacy, &kMinimapLayoutEnhanced})
            {
                for (int k = 0; k < L->nChecks; ++k)
                    spans = spans && L->checks[k].off >= -L->spanBefore && L->checks[k].off + HexByteCount(L->checks[k].bytes) <= L->spanAfter;
                spans = spans && L->sigLen <= L->spanAfter && L->dispAt + 4 == L->endAt && L->endAt <= L->spanAfter && (!L->readerLea || L->spanBefore >= 0x16);
            }
            Check("minimap_scale Enhanced layout: the pattern is 24 tokens with 8 wildcards (the two rip-relative disp32), Legacy's is 18 with none, both lead on a fixed byte, "
                  "a malformed token is -1, and every byte either layout reads lies inside its span",
                  spans &&
                  PatTokenCount(kMinimapLayoutEnhanced.sig) == 24 && kMinimapLayoutEnhanced.sigLen == 24 && wild == 8 && PatTokenCount(kMinimapLayoutLegacy.sig) == 18 &&
                      kMinimapLayoutLegacy.sigLen == 18 && PatTokenAt(kMinimapLayoutEnhanced.sig, kMinimapLayoutEnhanced.leadTok) >= 0 &&
                      PatTokenAt(kMinimapLayoutLegacy.sig, kMinimapLayoutLegacy.leadTok) >= 0 && PatTokenAt("48 ?x", 1) == -1 && PatTokenCount("48 ?x") == -1 &&
                      PatTokenAt("48 ?? 1", 2) == -1);
        }
        {
            SynthImageEnh s;
            s.Build();
            const uint8_t* h[4] = {};
            const int n = FindLayoutHits(kMinimapLayoutEnhanced, s.img.data(), s.img.size(), h, 4, 0);
            const TableResolve r = s.Resolve();
            Check("minimap_scale Enhanced pattern: on an image laid out like the Enhanced dump (pattern, the reader's stores around it, the table) the pattern hits once and "
                  "resolves to the table, with no `lea rsi` before the hit required",
                  n == 1 && h[0] == &s.img[s.hit] && r.ok && r.tableOff == s.table, r.why);
        }
        {
            int rejected = 0, total = 0;
            std::string missed;
            for (int k = 0; k < kMinimapLayoutEnhanced.nChecks; ++k)
            {
                SynthImageEnh s;
                s.Build();
                s.img[s.hit + (size_t)kMinimapLayoutEnhanced.checks[k].off] ^= 0x01;
                const TableResolve r = s.Resolve();
                ++total;
                if (!r.ok && r.why.find("differs") != std::string::npos)
                    ++rejected;
                else
                    missed += std::string(kMinimapLayoutEnhanced.checks[k].what) + "; ";
            }
            Check("minimap_scale Enhanced pattern: changing one byte of any of the 9 pinned reader instructions (the five float / char stores, alignY, the element counter, "
                  "the name cursor, the stride) refuses the table with the reason",
                  rejected == total && total == 9, missed);
        }
        {
            // Wildcards match anything, fixed tokens match exactly: flip every byte of the pattern in turn.
            int wildHit = 0, wildN = 0, fixedMiss = 0, fixedN = 0;
            for (int i = 0; i < kMinimapLayoutEnhanced.sigLen; ++i)
            {
                SynthImageEnh s;
                s.Build();
                s.img[s.hit + (size_t)i] ^= 0x5A;
                const bool hit = s.Hits(kMinimapLayoutEnhanced) == 1;
                if (PatTokenAt(kMinimapLayoutEnhanced.sig, i) == -2)
                {
                    ++wildN;
                    wildHit += hit ? 1 : 0;
                }
                else
                {
                    ++fixedN;
                    fixedMiss += hit ? 0 : 1;
                }
            }
            Check("minimap_scale Enhanced pattern: altering any wildcard byte still hits, altering any of the 16 fixed bytes loses the hit",
                  wildN == 8 && wildHit == 8 && fixedN == 16 && fixedMiss == 16, std::to_string(wildHit) + "/" + std::to_string(wildN) + " wildcards, " + std::to_string(fixedMiss) + "/" + std::to_string(fixedN) + " fixed");
        }
        {
            SynthImageEnh a, b, c, d, e, two;
            a.Build(); b.Build(); c.Build(); d.Build(); e.Build(); two.Build();
            const int32_t beyond = 0x7ff00000;
            std::memcpy(&a.img[a.hit + (size_t)kMinimapLayoutEnhanced.dispAt], &beyond, 4);                  // the table address leaves the image
            const TableResolve nearStart = ResolveMinimapTableFor(kMinimapLayoutEnhanced, &d.img[0x8], d.img.data(), d.img.size());
            const TableResolve nearEnd = ResolveMinimapTableFor(kMinimapLayoutEnhanced, &e.img[e.img.size() - 0x40], e.img.data(), e.img.size());   // the reader would run off the image
            // a second copy of the pattern further on: two hits (the engine refuses an ambiguous match)
            std::memcpy(&two.img[0x9000], &two.img[two.hit], (size_t)kMinimapLayoutEnhanced.sigLen);
            Check("minimap_scale Enhanced pattern: a table address outside the image and a hit at either edge are refused; a second copy of the pattern makes 2 hits",
                  !a.Resolve().ok && a.Resolve().why.find("outside the image") != std::string::npos && !nearStart.ok && !nearEnd.ok && two.Hits(kMinimapLayoutEnhanced) == 2);
        }
        {
            // The two layouts do not find each other, and the generic matcher agrees with the byte-exact search on the Legacy image.
            SynthImage leg;
            leg.Build();
            SynthImageEnh enh;
            enh.Build();
            const uint8_t* h[4] = {};
            const int legInLeg = FindLayoutHits(kMinimapLayoutLegacy, leg.img.data(), leg.img.size(), h, 4, 0);
            const std::vector<size_t> exact = FindAll(leg.img.data(), leg.img.size(), kMinimapTableSig);
            const bool same = legInLeg == (int)exact.size() && legInLeg == 1 && h[0] == &leg.img[exact[0]];
            Check("minimap_scale layouts: the Legacy pattern finds only the Legacy image (the same single hit as the byte-exact search), the Enhanced pattern only the Enhanced image",
                  same && FindLayoutHits(kMinimapLayoutLegacy, enh.img.data(), enh.img.size(), h, 4, 0) == 0 &&
                      FindLayoutHits(kMinimapLayoutEnhanced, leg.img.data(), leg.img.size(), h, 4, 0) == 0 && enh.Hits(kMinimapLayoutEnhanced) == 1);
        }
        {
            // Several runs: hits accumulate across calls and stop at the cap.
            SynthImageEnh a, b;
            a.Build();
            b.Build();
            const uint8_t* h[4] = {};
            int have = FindLayoutHits(kMinimapLayoutEnhanced, a.img.data(), a.img.size(), h, 4, 0);
            have = FindLayoutHits(kMinimapLayoutEnhanced, b.img.data(), b.img.size(), h, 4, have);
            const uint8_t* g[2] = {};
            const int capped = FindLayoutHits(kMinimapLayoutEnhanced, a.img.data(), a.img.size(), g, 1, 1);
            // a run that is exactly the pattern is a hit (the last byte counts); one byte shorter is not
            std::vector<uint8_t> exact((size_t)kMinimapLayoutEnhanced.sigLen);
            for (int i = 0; i < kMinimapLayoutEnhanced.sigLen; ++i)
                exact[(size_t)i] = PatTokenAt(kMinimapLayoutEnhanced.sig, i) >= 0 ? (uint8_t)PatTokenAt(kMinimapLayoutEnhanced.sig, i) : (uint8_t)0xA5;
            const uint8_t* e1[4] = {};
            const uint8_t* e2[4] = {};
            const int atEnd = FindLayoutHits(kMinimapLayoutEnhanced, exact.data(), exact.size(), e1, 4, 0);
            const int tooShort = FindLayoutHits(kMinimapLayoutEnhanced, exact.data(), exact.size() - 1, e2, 4, 0);
            Check("minimap_scale layouts: hits found in separate runs add up (2), a full hit buffer is left alone, a run that is exactly the pattern hits and one byte shorter does not",
                  have == 2 && h[0] == &a.img[a.hit] && h[1] == &b.img[b.hit] && capped == 1 && atEnd == 1 && e1[0] == exact.data() && tooShort == 0);
        }
    }

    // ---- the real thing: the decrypted 3889 dump (skipped loudly when this machine has none) --------------------------------------------------

    inline bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        out.assign((size_t)(n > 0 ? n : 0), 0);
        const size_t got = n > 0 ? std::fread(out.data(), 1, (size_t)n, f) : 0;
        std::fclose(f);
        return n > 0 && got == (size_t)n;
    }

    // %LOCALAPPDATA%\GAGE-MP\dump (or GAGE_DUMP_DIR): the Legacy images are named GTA5.exe-<size>-<crc>.img.
    inline std::vector<std::string> FindLegacyDumps()
    {
        std::vector<std::string> out;
        std::string dir;
        if (const char* d = std::getenv("GAGE_DUMP_DIR"))
            dir = d;
        else if (const char* l = std::getenv("LOCALAPPDATA"))
            dir = std::string(l) + "/GAGE-MP/dump";
        if (dir.empty())
            return out;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name.rfind("GTA5.exe-", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".img") == 0)
                out.push_back(it->path().string());
        }
        return out;
    }

    inline void DumpFail(const char* what, const std::string& got = "")
    {
        std::printf("FAIL  minimap_scale dump: %s%s%s\n", what, got.empty() ? "" : "  got: ", got.c_str());
        ++g_failed;
    }

    // The Enhanced dump: $GAGE_ENH_DUMP, $GAGE_ENH_IMG, else the repo-relative default searched from the working directory upwards (as pack_profile_test does).
    inline std::string FindEnhancedDump()
    {
        for (const char* v : {"GAGE_ENH_DUMP", "GAGE_ENH_IMG"})
            if (const char* e = std::getenv(v))
                return e;
        const char* rel = "enhanced EXE/dump and bin/GTA5_Enhanced.exe-95703552-959889125.img";
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::current_path(ec);
        for (int up = 0; !ec && up < 8 && !dir.empty(); ++up, dir = dir.parent_path())
        {
            const std::filesystem::path cand = dir / rel;
            if (std::filesystem::exists(cand, ec))
                return cand.string();
            if (dir == dir.parent_path())
                break;
        }
        return "";
    }

    // The Enhanced image (95703552 bytes): the layout's pattern is unique and resolves to the table; the reader's instructions match; the table holds the three
    // components with stock-looking values; the Legacy pattern has no hit (the negative check is paired with the positive one above it).
    inline void RunEnhancedDump()
    {
        const std::string path = FindEnhancedDump();
        std::vector<uint8_t> img;
        if (path.empty() || !ReadFileBytes(path, img) || img.size() != 95703552)
        {
            std::printf("SKIP  minimap_scale dump (Enhanced): no readable 95703552-byte GTA5_Enhanced image (GAGE_ENH_DUMP / GAGE_ENH_IMG / the repo's \"enhanced EXE\" folder); the "
                        "Enhanced pattern and reader checks were NOT run against the real image\n");
            return;
        }
        const MinimapLayout& L = kMinimapLayoutEnhanced;
        const uint8_t* h[4] = {};
        const int n = FindLayoutHits(L, img.data(), img.size(), h, 4, 0);
        if (n != 1)
        {
            DumpFail("Enhanced: the pattern must hit exactly once", std::to_string(n) + " hits in " + path);
            return;
        }
        const size_t hitRva = (size_t)(h[0] - img.data());
        const TableResolve r = ResolveMinimapTableFor(L, h[0], img.data(), img.size());
        if (!r.ok)
        {
            DumpFail("Enhanced: the reader / layout check", r.why);
            return;
        }
        if (hitRva != L.expectHit || r.tableOff != L.expectTable)
            DumpFail("Enhanced: RVAs differ from the ones the layout was analysed at", std::to_string(hitRva) + " / " + std::to_string(r.tableOff));
        MinimapComp all[kMinimapEntries];
        for (int i = 0; i < kMinimapEntries; ++i)
            all[i] = DecodeMinimapComp(&img[r.tableOff + (size_t)i * kMinimapStride]);
        const MinimapTrio t = PickTrio(all, kMinimapEntries);
        std::string why = t.AllFound() ? "" : "minimap / minimap_mask / minimap_blur not all named in the image";
        bool plausible = t.AllFound();
        for (int k = 0; k < 3 && plausible; ++k)
            plausible = PlausibleComp(t.c[k], &why);
        if (!plausible)
        {
            DumpFail("Enhanced: the table in the image does not hold three plausible minimap components", why);
            return;
        }
        const bool stock = Near(t.c[0].posX, -0.0045f) && Near(t.c[0].posY, 0.002f) && Near(t.c[0].sizeX, 0.150f) && Near(t.c[0].sizeY, 0.188888f, 1e-4f);
        if (!stock)
            DumpFail("Enhanced: the minimap component is not the stock frontend.xml one", FormatComp(t.c[0]));
        const bool legacyAbsent = FindAll(img.data(), img.size(), kMinimapTableSig).empty();
        if (!legacyAbsent)
            DumpFail("Enhanced: FiveM's Legacy pattern must have no hit here (the layouts would be ambiguous)");
        std::printf("ok    minimap_scale dump (Enhanced %s): pattern unique at RVA 0x%zx, reader pinned (9 instructions), table at RVA 0x%zx; %s | %s | %s; Legacy pattern absent: %s\n", path.c_str(),
                    hitRva, r.tableOff, FormatComp(t.c[0]).c_str(), FormatComp(t.c[1]).c_str(), FormatComp(t.c[2]).c_str(), legacyAbsent ? "yes" : "NO");
    }

    // Not counted in kChecks (the dump is not in the repo). A failure is a failure; an absent dump is a loud SKIP, never a pass.
    inline void RunDump()
    {
        const std::vector<std::string> dumps = FindLegacyDumps();
        if (dumps.empty())
        {
            std::printf("SKIP  minimap_scale dump: no Legacy 3889 dump (GTA5.exe-*.img) in %%LOCALAPPDATA%%\\GAGE-MP\\dump or GAGE_DUMP_DIR; the pattern and the reader checks "
                        "were NOT run against the real image\n");
        }
        int ran = 0;
        for (const std::string& path : dumps)
        {
            std::vector<uint8_t> img;
            if (!ReadFileBytes(path, img) || img.size() != 65387520)
            {
                std::printf("SKIP  minimap_scale dump: %s is not a readable 65387520-byte Legacy image\n", path.c_str());
                continue;
            }
            ++ran;
            const std::vector<size_t> hits = FindAll(img.data(), img.size(), kMinimapTableSig);
            if (hits.size() != 1)
            {
                DumpFail("the pattern must hit exactly once", std::to_string(hits.size()) + " hits in " + path);
                continue;
            }
            const TableResolve r = ResolveMinimapTable(&img[hits[0]], img.data(), img.size());
            if (!r.ok)
            {
                DumpFail("the reader / layout check", r.why);
                continue;
            }
            // The table is in .data and the dumps were taken before the game parsed frontend.xml: every element is an empty slot, so the DEFAULT VALUES
            // are not in the dump (they are logged live). What the dump does prove: the layout and the address.
            int named = 0;
            for (int i = 0; i < kMinimapEntries; ++i)
                named += DecodeMinimapComp(&img[r.tableOff + (size_t)i * kMinimapStride]).name.empty() ? 0 : 1;
            std::printf("ok    minimap_scale dump %s: pattern unique at RVA 0x%zx, game reader pinned (stride 0x78, floats +100..+112, align +116/+117), table at RVA 0x%zx "
                        "(%d of %d slots named in the image: it predates the frontend.xml parse)\n",
                        path.c_str(), hits[0], r.tableOff, named, kMinimapEntries);
            if (hits[0] != kMinimapHit3889 || r.tableOff != kMinimapTable3889)
                DumpFail("RVAs differ from the ones this build was analysed at", std::to_string(hits[0]) + " / " + std::to_string(r.tableOff));
        }
        if (!dumps.empty() && ran == 0)
            DumpFail("a Legacy dump was found but none could be read");
        RunEnhancedDump();
    }

} // namespace minimapsizetest
