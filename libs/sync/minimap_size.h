// 0.4.25 minimap_scale (GI: "make the minimap smaller"). Vanilla GTA has no native for the radar's size; FiveM's SET_MINIMAP_COMPONENT_POSITION
// and RAGE:MP's setMinimapComponentPosition both patch the game's own table of minimap components in memory. This header is the pure half
// (no Windows, no game memory): where the table is, what an entry looks like, how it is scaled, and when the engine writes it again.
// apps/engine/minimap_size.cpp is the native half; tests/minimap_tests.h pins both, including the pattern against the decrypted 3889 dump.
//
// Source (FiveM, citizenfx/fivem master, code/components/extra-natives-five/src/InterfaceExtraNatives.cpp, read 2026-11-05):
//   struct MinimapData { char name[100]; float posX, posY, sizeX, sizeY; char alignX, alignY; char pad[2]; };   // 120 bytes
//   minimapArray = get_address<MinimapData*>(get_pattern("48 8D 54 24 38 41 B8 64 00 00 00 48 8B 48 08 48 8D 05", 18));   // 11 entries
//   SET_MINIMAP_COMPONENT_POSITION: the entry whose name matches (stricmp) gets alignX / alignY (first char), posX, posY, sizeX, sizeY.
//   Its own header comment: "Overrides the minimap component data (from common:/data/ui/frontend.xml)".
// Checked against Legacy 1.0.3889 (the decrypted dumps in %LOCALAPPDATA%\GAGE-MP\dump): the pattern hits exactly once, at RVA 0x2cf6ad, inside the
// game's own frontend.xml reader. That reader copies the entry's name (100 bytes), then stores posX +100, posY +104, sizeX +108, sizeY +112, alignX
// +116, alignY +117 and steps 0x78 per element: the layout above, read from the game's code (kLoaderChecks pins those instructions). The table
// itself (RVA 0x1f7bc40, .data) is zero in the dumps because they were taken before the game parsed frontend.xml, so the default VALUES can only
// be read live: the engine logs them before and after the write.
//
// What is scaled: all three components (minimap, minimap_mask, minimap_blur), posX / posY / sizeX / sizeY each times the factor. Every position
// is an offset from the same safe-zone corner (all three are aligned L / B), so a uniform factor is a scaled replica of the stock layout: the
// corner stays put and the mask and the blur keep their place inside the minimap frame. Scaling the sizes alone would move the mask's top edge
// out of the frame (the mask sits 0.0019 below the frame's top at the stock values).
//
// GTA V Enhanced (0.4.26, GI: every game version identical): the same table, found by its own pattern (MinimapLayout kMinimapLayoutEnhanced, below); the
// shape and the rules are shared. Read from the Enhanced dump only, never run live yet (see the layout comment).
#pragma once

#include "sync/minimap.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace gage::sync {

    // ---- the registry value ------------------------------------------------------------------------------------------

    constexpr float kMinimapScaleMin = kRadarScaleMin;   // 0.5
    constexpr float kMinimapScaleMax = kRadarScaleMax;   // 1.0 = the game's own size (untouched)
    // 0.4.28: unset = 1.0, the game's own size. The 0.4.25-0.4.27 table writes (0.80, then 0.50) were read back in the table but the drawn
    // radar never changed on 3889 (GI's live log 2026-11-05: big-radar flip on 1 / off 1 after the write, map still full size), so the
    // vitals band sized to the scaled radar came out half the map's width. A scale returns as the default once a live run shows a
    // refresh trigger that makes the drawn radar follow the table; minimap_scale=N still writes it for that test.
    constexpr float kMinimapScaleDefault = 1.00f;

    enum class ScaleSource
    {
        Unset,     // no value: the default
        Set,       // a number inside 0.5..1.0
        Clamped,   // a number outside, held to the nearest end
        Junk,      // not a plain decimal number: the default
    };

    inline const char* ScaleSourceName(ScaleSource s)
    {
        switch (s)
        {
        case ScaleSource::Unset: return "unset, default";
        case ScaleSource::Set: return "set";
        case ScaleSource::Clamped: return "set, held to 0.5..1.0";
        case ScaleSource::Junk: return "unreadable, default";
        }
        return "?";
    }

    struct ParsedScale
    {
        float scale = kMinimapScaleDefault;
        ScaleSource src = ScaleSource::Unset;
    };

    // minimap_scale (HKCU\Software\GAGE-MP): a plain decimal ("0.8", "1", "1.0", ".7"); no comma, no exponent, no sign. 1 and above = untouched.
    inline ParsedScale ParseMinimapScale(const std::wstring& v)
    {
        ParsedScale p;
        size_t b = 0, e = v.size();
        while (b < e && (v[b] == L' ' || v[b] == L'\t')) ++b;
        while (e > b && (v[e - 1] == L' ' || v[e - 1] == L'\t')) --e;
        if (b == e)
            return p;
        double ip = 0.0, fp = 0.0, place = 0.1;
        int intDigits = 0, fracDigits = 0;
        size_t i = b;
        for (; i < e && v[i] >= L'0' && v[i] <= L'9'; ++i, ++intDigits)
            ip = ip * 10.0 + (double)(v[i] - L'0');
        if (i < e && v[i] == L'.')
        {
            for (++i; i < e && v[i] >= L'0' && v[i] <= L'9'; ++i, ++fracDigits, place *= 0.1)
                fp += place * (double)(v[i] - L'0');
        }
        if (i != e || (intDigits == 0 && fracDigits == 0) || intDigits > 6 || fracDigits > 9)
        {
            p.src = ScaleSource::Junk;
            return p;
        }
        const double x = ip + fp;
        if (x < (double)kMinimapScaleMin)
        {
            p.scale = kMinimapScaleMin;
            p.src = ScaleSource::Clamped;
        }
        else if (x > (double)kMinimapScaleMax)
        {
            p.scale = kMinimapScaleMax;
            p.src = ScaleSource::Clamped;
        }
        else
        {
            p.scale = (float)x;
            p.src = ScaleSource::Set;
        }
        return p;
    }

    // ---- one entry of the game's table -------------------------------------------------------------------------------

    constexpr int kMinimapEntries = 11;       // FiveM's bound
    constexpr int kMinimapStride = 120;       // sizeof(MinimapData); the game's reader steps 0x78
    constexpr int kOffName = 0, kNameCap = 100, kOffPosX = 100, kOffPosY = 104, kOffSizeX = 108, kOffSizeY = 112, kOffAlignX = 116, kOffAlignY = 117;

    struct MinimapComp
    {
        std::string name;                 // empty = an unused slot (or no usable name)
        char alignX = 0, alignY = 0;
        float posX = 0, posY = 0, sizeX = 0, sizeY = 0;
    };

    // e points at one 120-byte entry.
    inline MinimapComp DecodeMinimapComp(const uint8_t* e)
    {
        MinimapComp c;
        int n = 0;
        while (n < kNameCap && e[kOffName + n] != 0)
            ++n;
        bool printable = n < kNameCap;     // no terminator inside the 100 bytes = not a name
        for (int i = 0; i < n && printable; ++i)
            printable = e[kOffName + i] >= 0x20 && e[kOffName + i] < 0x7f;
        if (printable)
            c.name.assign(reinterpret_cast<const char*>(e + kOffName), (size_t)n);
        std::memcpy(&c.posX, e + kOffPosX, 4);
        std::memcpy(&c.posY, e + kOffPosY, 4);
        std::memcpy(&c.sizeX, e + kOffSizeX, 4);
        std::memcpy(&c.sizeY, e + kOffSizeY, 4);
        c.alignX = (char)e[kOffAlignX];
        c.alignY = (char)e[kOffAlignY];
        return c;
    }

    // Writes the four floats only: the name and the alignment are never touched.
    inline void EncodeMinimapGeometry(uint8_t* e, const MinimapComp& c)
    {
        std::memcpy(e + kOffPosX, &c.posX, 4);
        std::memcpy(e + kOffPosY, &c.posY, 4);
        std::memcpy(e + kOffSizeX, &c.sizeX, 4);
        std::memcpy(e + kOffSizeY, &c.sizeY, 4);
    }

    // Exact: the engine compares what it wrote with what it reads back.
    inline bool SameComp(const MinimapComp& a, const MinimapComp& b)
    {
        return a.name == b.name && a.alignX == b.alignX && a.alignY == b.alignY && a.posX == b.posX && a.posY == b.posY && a.sizeX == b.sizeX &&
               a.sizeY == b.sizeY;
    }

    // Position and size times the factor, about the shared L / B corner. The factor is held to 0.5..1.
    inline MinimapComp ScaledComp(const MinimapComp& c, float scale)
    {
        const float s = ClampRadarScale(scale);
        MinimapComp o = c;
        o.posX = c.posX * s;
        o.posY = c.posY * s;
        o.sizeX = c.sizeX * s;
        o.sizeY = c.sizeY * s;
        return o;
    }

    // A component that looks like a stock one (frontend.xml: aligned left / bottom, a few hundredths of the screen from the corner, a tenth to a third of the
    // screen in size). Anything else is not written: a half-parsed table, or a build whose layout differs.
    inline bool PlausibleComp(const MinimapComp& c, std::string* why = nullptr)
    {
        auto fail = [&](const char* m) {
            if (why)
                *why = c.name + ": " + m;
            return false;
        };
        if (c.alignX != 'L' || c.alignY != 'B')
            return fail("not aligned L / B");
        const float v[4] = {c.posX, c.posY, c.sizeX, c.sizeY};
        for (int i = 0; i < 4; ++i)
            if (!std::isfinite(v[i]))
                return fail("a value is not a finite number");
        if (std::fabs(c.posX) > 0.25f || std::fabs(c.posY) > 0.25f)
            return fail("position outside +-0.25");
        if (!(c.sizeX >= 0.02f && c.sizeX <= 0.6f) || !(c.sizeY >= 0.02f && c.sizeY <= 0.6f))
            return fail("size outside 0.02..0.6");
        return true;
    }

    inline std::string FormatComp(const MinimapComp& c)
    {
        char b[200];
        std::snprintf(b, sizeof(b), "\"%s\" %c/%c pos (%.4f, %.4f) size (%.6f, %.6f)", c.name.c_str(), c.alignX ? c.alignX : '?', c.alignY ? c.alignY : '?',
                      (double)c.posX, (double)c.posY, (double)c.sizeX, (double)c.sizeY);
        return b;
    }

    // ---- the three components we resize ------------------------------------------------------------------------------

    constexpr int kRoleMinimap = 0, kRoleMask = 1, kRoleBlur = 2;
    // The names of the three slots of one radar mode, in role order: the minimap (what 0.4.25 - 0.5.3 write) and, 0.5.4 ring_map_big, the big
    // (expanded) radar, slots 3 / 4 / 5 of frontend.xml ("bigmap" L/B pos (-0.004, 0.022) size (0.364, 0.460417) in GI's 2026-11-07 log).
    constexpr const char* kMinimapRoleNames[3] = {"minimap", "minimap_mask", "minimap_blur"};
    constexpr const char* kBigmapRoleNames[3] = {"bigmap", "bigmap_mask", "bigmap_blur"};
    inline const char* RoleName(int r, const char* const* names = kMinimapRoleNames)
    {
        return r >= 0 && r < 3 ? names[r] : "?";
    }

    inline bool NameEqualsNoCase(const std::string& a, const char* b)
    {
        size_t i = 0;
        for (; i < a.size() && b[i]; ++i)
        {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
            if (x != y)
                return false;
        }
        return i == a.size() && b[i] == 0;
    }

    struct MinimapTrio
    {
        MinimapComp c[3];
        int index[3] = {-1, -1, -1};     // the slot in the table
        bool AllFound() const { return index[0] >= 0 && index[1] >= 0 && index[2] >= 0; }
    };

    // all: the table's entries (n of them). The first entry of each name wins. names: the mode's three slot names (default: the minimap's).
    inline MinimapTrio PickTrio(const MinimapComp* all, int n, const char* const* names = kMinimapRoleNames)
    {
        MinimapTrio t;
        for (int r = 0; r < 3; ++r)
            for (int i = 0; i < n; ++i)
                if (NameEqualsNoCase(all[i].name, RoleName(r, names)))
                {
                    t.c[r] = all[i];
                    t.index[r] = i;
                    break;
                }
        return t;
    }

    inline MinimapTrio ScaledTrio(const MinimapTrio& t, float scale)
    {
        MinimapTrio o = t;
        for (int r = 0; r < 3; ++r)
            o.c[r] = ScaledComp(t.c[r], scale);
        return o;
    }

    // ---- 0.5.2 ring_map: the radar moved into the REV1 ring (GI 2026-11-06: "do the real round map") ------------------------------------
    // Evidence (Legacy 3889 dump, read 2026-11-06): the game reads this table through six getters (frame pos / size +0x2cca98 / +0x2ccb64, mask
    // +0x2cc928 / +0x2cca14, blur +0x2cc7bc / +0x2cc890; the slot comes from the radar mode at +0x1f77600 through +0x2cc6f0: 0 minimap, 3 bigmap,
    // 6 pausemap, 8, 10 golf), and the per-frame radar update (+0x2eccb8 -> +0x2f8080) calls all six every frame and hands the result to the render
    // thread. The scaleform movie's own rectangle is set only on a re-layout (+0x2f6514 -> +0x3f8924, and the movie's creation +0x2c7988), which is
    // what the big-radar flip (or a pause-menu round trip) triggers: the Cfx / alt:V reports say the same ("mask and blur follow, the minimap
    // component only after a re-render").
    //
    // The table's L / B space, read from the alignment helper +0x2c3fb0 (the frame getter's last call): x = safeLeft + posX ('L'), and for 'B'
    // y_top = safeBottom - sizeY + posY, screen y DOWN. So posX is the left edge right of the safe zone's left edge, posY the BOTTOM edge measured
    // DOWN from the safe zone's bottom edge (a radar raised 5 % of the height has posY -0.05), both in fractions of the screen.
    // A placement maps each component's position about an anchor and per axis, in that space:
    //   pos' = t + (pos - a) * s, size' = size * s (per axis). Scale mode (minimap_scale) is a = t = 0, sx = sy = the factor.
    struct MinimapPlacement
    {
        float sx = 1.0f, sy = 1.0f, ax = 0.0f, ay = 0.0f, tx = 0.0f, ty = 0.0f;
        bool blurToVisible = false;                        // the blur is set to the visible square itself (stock, it is twice the map's width)
        float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;  // that square in table space (posX = left edge, posY = bottom edge, size)
        bool operator==(const MinimapPlacement& o) const
        {
            return sx == o.sx && sy == o.sy && ax == o.ax && ay == o.ay && tx == o.tx && ty == o.ty && blurToVisible == o.blurToVisible && vx == o.vx &&
                   vy == o.vy && vw == o.vw && vh == o.vh;
        }
    };

    inline MinimapComp PlacedComp(const MinimapComp& c, const MinimapPlacement& p)
    {
        MinimapComp o = c;
        o.posX = p.tx + (c.posX - p.ax) * p.sx;
        o.posY = p.ty + (c.posY - p.ay) * p.sy;
        o.sizeX = c.sizeX * p.sx;
        o.sizeY = c.sizeY * p.sy;
        return o;
    }

    inline MinimapTrio PlacedTrio(const MinimapTrio& t, const MinimapPlacement& p)
    {
        MinimapTrio o = t;
        for (int r = 0; r < 3; ++r)
            o.c[r] = PlacedComp(t.c[r], p);
        if (p.blurToVisible)
        {
            o.c[kRoleBlur].posX = p.vx;
            o.c[kRoleBlur].posY = p.vy;
            o.c[kRoleBlur].sizeX = p.vw;
            o.c[kRoleBlur].sizeY = p.vh;
        }
        return o;
    }

    // The stock VISIBLE map (RadarKeepOut's community formula: H / 4 px wide, H / 5.674 px tall, its bottom-left corner on the safe-zone corner) is
    // mapped onto a square of squarePx centred at (cxPx, cyPx) (screen px, y down) on a W x H display: per-axis scale about the visible map's centre,
    // then that centre moved onto the target. Every component keeps its place relative to the visible map (the frame and the mask render as stock,
    // only squeezed to a square). W, H > 0 and squarePx > 0, else the identity.
    // Table space (above): the visible map spans x 0..wv, y -hv..0 (it sits ON the safe zone's bottom edge, y down), so its centre is (wv / 2, -hv / 2);
    // the safe zone's left / bottom edges are at inset / 1 - inset of the screen, so a screen point (x, y) (fractions, y down) is (x - inset, y - 1 + inset).
    // Round-1 assumption, measured by the radar probe: the frame getter's aspect step (+0x2c342c) is the identity on a 16:9 display.
    // 0.5.5 ring_map_round: the same with the visible map mapped onto a w x h px RECTANGLE (ringround::RoundAskPx asks a different size per axis,
    // so the radar's real box comes out square; libs/sync/ring_round.h). RingPlacement is the square case.
    inline MinimapPlacement RingPlacementBox(float displayW, float displayH, float safeZone, bool safeZoneOk, float cxPx, float cyPx, float wPx, float hPx)
    {
        MinimapPlacement p;
        if (!(displayW > 0.0f) || !(displayH > 0.0f) || !(wPx > 0.0f) || !(hPx > 0.0f))
            return p;
        const float sz = SafeZoneClamped(safeZone, safeZoneOk);
        const float inset = 0.05f * std::fabs(sz - 1.0f) * 10.0f;   // as RadarKeepOut: the same fraction of W (x) and of H (y)
        const float wv = displayH / 4.0f / displayW, hv = 1.0f / 5.674f;
        p.ax = wv * 0.5f;
        p.ay = -hv * 0.5f;
        p.sx = wPx / displayW / wv;
        p.sy = hPx / displayH / hv;
        p.tx = cxPx / displayW - inset;
        p.ty = cyPx / displayH - 1.0f + inset;
        p.blurToVisible = true;
        p.vw = wPx / displayW;
        p.vh = hPx / displayH;
        p.vx = p.tx - p.vw * 0.5f;
        p.vy = p.ty + p.vh * 0.5f;   // the bottom edge, y down
        return p;
    }
    inline MinimapPlacement RingPlacement(float displayW, float displayH, float safeZone, bool safeZoneOk, float cxPx, float cyPx, float squarePx)
    {
        return RingPlacementBox(displayW, displayH, safeZone, safeZoneOk, cxPx, cyPx, squarePx, squarePx);
    }

    // ---- 0.5.4 ring map round 2 (GI 2026-11-07: "map is just a square over the hp and sat bar, not a circle inside of it + make it more zoomed out") ----
    // What the two 0.5.2 probes showed (radar_probe_1/2.bmp, 2560 x 1440, safe zone 0.97, the crop x 0-768 y 792-1440): the radar's map about 620 px
    // wide from the safe-zone's left edge, its bottom on the safe-zone bottom (y 1418) and its top above the crop (y < 792), the player arrow at
    // about (324, 1117), on both probes 68 s apart; the placement written to slots 0-2 asked for a 213 px square centred at (233, 1136) whose
    // bottom is 173 px ABOVE the safe-zone bottom. Nothing of it moved with the write: it is the shape of the stock BIG radar (slot 3 "bigmap",
    // frame top = 1418 + 0.022 H - 0.4604 H = 787, bottom on the corner; its blur 0.262 W = 671 px wide), whose slots 0.5.2 never wrote. The
    // radar getters pick the slot by the radar mode each frame (Legacy +0x2cc6f0, read from the 3889 dump: mode 0 reads slot 3 while the byte at
    // +0x1f6c8f7 is set, presumably the big-map flag; mode 3 reads slot 3 too), so a radar left in big mode ignores slots 0-2 (this also fits 0.4.25-0.4.27: "the table was written, the map never changed"). Why the big
    // mode stays on is not proven (the engine's own flip logs "off 1"); MirroredTrio makes it irrelevant: the big radar's slots get the SAME frame /
    // mask / blur geometry as the minimap's placement (names, alignment and slot indices of the big slots kept), so either mode lands in the ring.
    inline MinimapTrio MirroredTrio(const MinimapTrio& bigOrig, const MinimapTrio& geometry)
    {
        MinimapTrio o = bigOrig;
        for (int r = 0; r < 3; ++r)
        {
            o.c[r].posX = geometry.c[r].posX;
            o.c[r].posY = geometry.c[r].posY;
            o.c[r].sizeX = geometry.c[r].sizeX;
            o.c[r].sizeY = geometry.c[r].sizeY;
        }
        return o;
    }
    inline bool SameGeometry(const MinimapTrio& a, const MinimapTrio& b)
    {
        for (int r = 0; r < 3; ++r)
            if (!(a.c[r].posX == b.c[r].posX && a.c[r].posY == b.c[r].posY && a.c[r].sizeX == b.c[r].sizeX && a.c[r].sizeY == b.c[r].sizeY))
                return false;
        return true;
    }

    // ring_map_zoom (HKCU\Software\GAGE-MP): the SET_RADAR_ZOOM level held every tick while the ring map shows. alloc8or: "zoomLevel ranges from 0 to
    // 1400 in R* Scripts" (0 = the game's own, speed-driven zoom; larger = further out). Unset = kRingMapZoomDefault; "0" = the game's own zoom (no
    // call, the 0.5.3 behaviour); 1..1400 as given; above 1400 held to 1400; anything else the default (why says so).
    constexpr int kRingMapZoomDefault = 1100, kRingMapZoomMax = 1400;
    struct RingZoom
    {
        int level = kRingMapZoomDefault;
        const char* why = nullptr;   // null = as asked (or unset)
    };
    inline RingZoom ParseRingMapZoom(const std::wstring& v)
    {
        RingZoom z;
        size_t b = 0, e = v.size();
        while (b < e && (v[b] == L' ' || v[b] == L'\t')) ++b;
        while (e > b && (v[e - 1] == L' ' || v[e - 1] == L'\t')) --e;
        if (b == e)
            return z;
        long x = 0;
        for (size_t i = b; i < e; ++i)
        {
            if (v[i] < L'0' || v[i] > L'9' || i - b >= 6)
            {
                z.why = "; ring_map_zoom unreadable (a whole number 0..1400), the default taken";
                return z;
            }
            x = x * 10 + (long)(v[i] - L'0');
        }
        if (x > kRingMapZoomMax)
        {
            z.level = kRingMapZoomMax;
            z.why = "; ring_map_zoom above 1400, held to 1400";
            return z;
        }
        z.level = (int)x;
        return z;
    }

    // ---- when to write -----------------------------------------------------------------------------------------------

    // The engine feeds it what the table holds (script tick) and performs what it answers. The first complete, plausible read becomes the
    // "original"; the target is always the original times the factor, never the target times the factor, so a game that resets the table (or a
    // second write of ours) cannot shrink the map twice.
    class MinimapSizeGate
    {
    public:
        enum class Phase { Off, Waiting, Applied, GaveUp };
        enum class Act { None, Apply, Reassert };

        struct Stats
        {
            uint32_t polls = 0, applies = 0, checks = 0, reasserts = 0, adopted = 0, unreadable = 0, implausible = 0, writeFailures = 0;
        };

        static constexpr double kPollMs = 250.0;            // while waiting for the table to be populated
        static constexpr double kCheckMs = 1000.0;          // between looks at an applied table
        static constexpr double kWaitGiveUpMs = 120000.0;   // a table that never fills is left alone
        static constexpr uint32_t kMaxReasserts = 8;        // a game that keeps rewriting it is not fought
        static constexpr uint32_t kMaxWriteFailures = 3;

        // The factor (0.5..1). 1 = untouched. Changed after an apply: the saved originals are re-targeted (and written back once at 1).
        void Configure(float scale)
        {
            const float was = scale_;
            const bool wasPlaced = placed_;
            const bool wasMirrored = mirrored_;
            scale_ = ClampRadarScale(scale);
            placed_ = false;
            mirrored_ = false;
            Retarget(wasPlaced || wasMirrored || was != scale_);
        }

        // 0.5.2 ring_map: an absolute placement (RingPlacement) instead of a factor. Configure(float) switches back to the factor.
        void ConfigurePlacement(const MinimapPlacement& p)
        {
            const bool changed = !placed_ || mirrored_ || !(place_ == p);
            place_ = p;
            placed_ = true;
            mirrored_ = false;
            Retarget(changed);
        }

        // 0.5.4 ring_map_big: the target is another mode's geometry as it stands (MirroredTrio: the big radar's slots get the minimap placement's
        // frame / mask / blur), not something computed from this trio's own stock values. Configure(float) switches back to the factor.
        void ConfigureMirror(const MinimapTrio& geometry)
        {
            const bool changed = !mirrored_ || !SameGeometry(mirror_, geometry);
            mirror_ = geometry;
            mirrored_ = true;
            placed_ = false;
            Retarget(changed);
        }

        Act Next(const MinimapTrio& cur, bool readable, double nowMs)
        {
            switch (phase_)
            {
            case Phase::Off:
            case Phase::GaveUp:
                return Act::None;
            case Phase::Waiting:
                return NextWaiting(cur, readable, nowMs);
            case Phase::Applied:
                return NextApplied(cur, readable, nowMs);
            }
            return Act::None;
        }

        // The engine could not write or could not read the write back (it has put the old bytes back). Retried on the next call, a few times.
        void WriteFailed()
        {
            ++stats_.writeFailures;
            if (stats_.writeFailures >= kMaxWriteFailures)
            {
                phase_ = Phase::GaveUp;
                why_ = "the write or its read-back failed " + std::to_string(stats_.writeFailures) + " times";
                return;
            }
            forced_ = true;
        }

        Phase PhaseNow() const { return phase_; }
        float Scale() const { return placed_ || mirrored_ ? 1.0f : scale_; }   // the factor (1 in placement / mirror mode: the dock rectangle is not used)
        bool Placed() const { return placed_; }
        bool Mirrored() const { return mirrored_; }
        const MinimapPlacement& Placement() const { return place_; }
        bool HasOriginal() const { return hasOrig_; }
        const MinimapTrio& Original() const { return orig_; }
        const MinimapTrio& Target() const { return target_; }
        const Stats& Counters() const { return stats_; }
        const std::string& Why() const { return why_; }   // why it is waiting / gave up (last)

    private:
        // Untouched: the factor 1 in scale mode (a placement always writes).
        bool Untouched() const { return !placed_ && !mirrored_ && scale_ >= 1.0f; }
        MinimapTrio MakeTarget(const MinimapTrio& orig) const
        {
            return mirrored_ ? MirroredTrio(orig, mirror_) : placed_ ? PlacedTrio(orig, place_) : ScaledTrio(orig, scale_);
        }
        void Retarget(bool changed)
        {
            if (phase_ == Phase::GaveUp || (hasOrig_ && !changed))
                return;   // nothing new to write (a Welcome re-reads the registry; the same value is not written again)
            if (!hasOrig_)
            {
                phase_ = Untouched() ? Phase::Off : Phase::Waiting;
                return;
            }
            target_ = Untouched() ? orig_ : MakeTarget(orig_);
            phase_ = Phase::Applied;
            forced_ = true;
        }

        Act NextWaiting(const MinimapTrio& cur, bool readable, double nowMs)
        {
            if (polled_ && nowMs - lastPollMs_ < kPollMs)
                return Act::None;
            polled_ = true;
            lastPollMs_ = nowMs;
            if (firstPollMs_ < 0.0)
                firstPollMs_ = nowMs;
            ++stats_.polls;
            std::string why;
            if (readable && cur.AllFound())
            {
                bool ok = true;
                for (int r = 0; r < 3 && ok; ++r)
                    ok = PlausibleComp(cur.c[r], &why);
                if (ok)
                {
                    orig_ = cur;
                    hasOrig_ = true;
                    target_ = MakeTarget(orig_);
                    phase_ = Phase::Applied;
                    lastCheckMs_ = nowMs;
                    ++stats_.applies;
                    return Act::Apply;
                }
                ++stats_.implausible;
            }
            else
                why = readable ? "minimap / minimap_mask / minimap_blur not all in the table yet" : "table not readable";
            why_ = why;
            if (nowMs - firstPollMs_ >= kWaitGiveUpMs)
            {
                phase_ = Phase::GaveUp;
                why_ = "after 120 s: " + why;
            }
            return Act::None;
        }

        Act NextApplied(const MinimapTrio& cur, bool readable, double nowMs)
        {
            if (forced_)
            {
                forced_ = false;
                lastCheckMs_ = nowMs;
                return Act::Reassert;
            }
            if (Untouched())
            {
                phase_ = Phase::Off;   // the originals have been written back
                return Act::None;
            }
            if (nowMs - lastCheckMs_ < kCheckMs)
                return Act::None;
            lastCheckMs_ = nowMs;
            ++stats_.checks;
            if (!readable || !cur.AllFound())
            {
                ++stats_.unreadable;
                return Act::None;
            }
            // Per component: ours = fine, the defaults we saw = the game reset it, anything else plausible = the game's new stock value (adopted),
            // anything else implausible = wait (a table caught mid-write).
            bool changed = false;
            MinimapTrio nextOrig = orig_, nextTarget = target_;
            uint32_t adopted = 0;
            for (int r = 0; r < 3; ++r)
            {
                if (SameComp(cur.c[r], target_.c[r]))
                    continue;
                changed = true;
                if (!SameComp(cur.c[r], orig_.c[r]))
                {
                    if (!PlausibleComp(cur.c[r], nullptr))
                    {
                        ++stats_.implausible;
                        return Act::None;
                    }
                    nextOrig.c[r] = cur.c[r];
                    ++adopted;
                }
            }
            if (!changed)
                return Act::None;
            if (adopted)
                nextTarget = MakeTarget(nextOrig);   // a placement's blur is set from the visible square, not from its own stock value
            if (stats_.reasserts >= kMaxReasserts)
            {
                phase_ = Phase::GaveUp;
                why_ = "the game rewrote the table " + std::to_string(stats_.reasserts + 1) + " times; left as it is";
                return Act::None;
            }
            orig_ = nextOrig;
            target_ = nextTarget;
            stats_.adopted += adopted;
            ++stats_.reasserts;
            return Act::Reassert;
        }

        Phase phase_ = Phase::Off;
        float scale_ = 1.0f;
        bool placed_ = false;          // 0.5.2: place_ (ConfigurePlacement) instead of scale_
        MinimapPlacement place_;
        bool mirrored_ = false;        // 0.5.4: mirror_'s geometry (ConfigureMirror) instead of scale_ / place_
        MinimapTrio mirror_;
        bool hasOrig_ = false, forced_ = false, polled_ = false;
        double lastPollMs_ = 0.0, firstPollMs_ = -1.0, lastCheckMs_ = 0.0;
        MinimapTrio orig_, target_;
        Stats stats_;
        std::string why_;
    };

    // ---- finding the table in a Legacy 1.0.3889 image ----------------------------------------------------------------

    // FiveM's pattern (the last three bytes are `lea rax, [rip+disp32]`: the table's address, disp32 at +18). Unique in the 3889 image.
    constexpr const char* kMinimapTableSig = "48 8D 54 24 38 41 B8 64 00 00 00 48 8B 48 08 48 8D 05";
    constexpr int kMinimapTableSigLen = 18, kMinimapTableDispAt = 18;
    constexpr uint32_t kMinimapHit3889 = 0x2cf6ad, kMinimapTable3889 = 0x1f7bc40;   // RVAs seen in the 3889 dumps (diagnostics, not used to find it)

    // The game's frontend.xml reader, the function the pattern sits in. Offsets are from the pattern's first byte. This pins the layout above to the
    // code that fills the table: the first line is the stride (imul rcx, rcx, 0x78), the next five the stores to rsi-4 / rsi / rsi+4 / rsi+8 / rsi+0xc
    // (posX, posY, sizeX, sizeY, alignX, with rsi = element + 104), then the step (lea rsi, [rsi+0x78]) and the store of alignY at rsi-0x6b.
    struct LoaderCheck
    {
        int off;
        const char* bytes;   // "48 6B C9 78"
        const char* what;
    };
    constexpr LoaderCheck kLoaderChecks[] = {
        {0x1e, "48 6B C9 78", "stride 0x78 (imul rcx, rcx, 0x78)"},
        {0x53, "F3 0F 11 4E FC", "posX  at element+100 (movss [rsi-4], xmm1)"},
        {0x77, "F3 0F 11 0E", "posY  at element+104 (movss [rsi], xmm1)"},
        {0x9a, "F3 0F 11 4E 04", "sizeX at element+108 (movss [rsi+4], xmm1)"},
        {0xbe, "F3 0F 11 4E 08", "sizeY at element+112 (movss [rsi+8], xmm1)"},
        {0xd8, "88 46 0C", "alignX at element+116 (mov [rsi+0xc], al)"},
        {0xf6, "48 8D 76 78", "next element (lea rsi, [rsi+0x78])"},
        {0xfa, "88 46 95", "alignY at element+117 (mov [rsi-0x6b], al, after the step)"},
    };
    constexpr int kLoaderSpanBefore = 0x20, kLoaderSpanAfter = 0x100;   // bytes around the hit ResolveMinimapTable reads

    inline int HexByteAt(const char* s, int tok)
    {
        int n = 0;
        for (const char* p = s; *p;)
        {
            while (*p == ' ') ++p;
            if (!*p)
                break;
            const char* q = p;
            while (*q && *q != ' ') ++q;
            if (n == tok)
            {
                auto hv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1)); };
                return (q - p == 2 && hv(p[0]) >= 0 && hv(p[1]) >= 0) ? hv(p[0]) * 16 + hv(p[1]) : -1;
            }
            ++n;
            p = q;
        }
        return -1;
    }

    inline int HexByteCount(const char* s)
    {
        int n = 0;
        while (HexByteAt(s, n) >= 0)
            ++n;
        return n;
    }

    // ---- one layout per game build family ----------------------------------------------------------------------------
    // The table has the same 11 x 120-byte shape in Legacy 1.0.3889 and in GTA V Enhanced (the Enhanced dump, 2026-11-05: table at RVA 0x3deb800, filled by the
    // frontend.xml reader at 0x567d60 with the same offsets: name 100 bytes, floats +100 / +104 / +108 / +112, align chars +116 / +117, stride 0x78; the radar
    // placement code reads it live as `imul rcx, rcx, 0x78; movsd xmm, [rcx + table + 0x64 / 0x6c]`). Only the pattern that finds it and the instructions
    // that prove the layout differ: Enhanced's reader is a different compile (it indexes [r14 + rbp + off] from the table itself, with no `lea rsi, [table + 104]`).

    // One token of a pattern: two hex digits (the byte), "??" (any byte, -2), anything else -1 (also past the end).
    inline int PatTokenAt(const char* s, int tok)
    {
        int n = 0;
        for (const char* p = s; *p;)
        {
            while (*p == ' ') ++p;
            if (!*p)
                break;
            const char* q = p;
            while (*q && *q != ' ') ++q;
            if (n == tok)
            {
                if (q - p == 2 && p[0] == '?' && p[1] == '?')
                    return -2;
                auto hv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1)); };
                return (q - p == 2 && hv(p[0]) >= 0 && hv(p[1]) >= 0) ? hv(p[0]) * 16 + hv(p[1]) : -1;
            }
            ++n;
            p = q;
        }
        return -1;
    }

    // The number of tokens, or -1 when one of them is not a token.
    inline int PatTokenCount(const char* s)
    {
        int n = 0;
        for (const char* p = s; *p;)
        {
            while (*p == ' ') ++p;
            if (!*p)
                break;
            if (PatTokenAt(s, n) == -1)
                return -1;
            while (*p && *p != ' ') ++p;
            ++n;
        }
        return n;
    }

    struct MinimapLayout
    {
        const char* label;          // for the log
        const char* sig;            // hex tokens, "??" = any byte
        int sigLen;                 // the token count (the tests check it against the text)
        int leadTok;                // a fixed (non-wildcard) token the scan looks for first
        int dispAt, endAt;          // the rip-relative disp32 that names the table, and the end of its instruction, from the hit's first byte
        int spanBefore, spanAfter;  // bytes around the hit that must be inside the image (and readable)
        const LoaderCheck* checks;  // the reader's instructions the layout was read from
        int nChecks;
        bool readerLea;             // Legacy only: the reader's `lea rsi, [table + 104]` 0x16 bytes before the hit
        uint32_t expectHit, expectTable;   // the RVAs the layout was analysed at (diagnostics, not used to find it)
    };

    constexpr MinimapLayout kMinimapLayoutLegacy = {"Legacy 1.0.3889", kMinimapTableSig, kMinimapTableSigLen, 6, kMinimapTableDispAt, kMinimapTableDispAt + 4,
                                                    kLoaderSpanBefore, kLoaderSpanAfter, kLoaderChecks, (int)(sizeof(kLoaderChecks) / sizeof(kLoaderChecks[0])), true,
                                                    kMinimapHit3889, kMinimapTable3889};

    // Enhanced: the start of the frontend.xml reader's table walk, `mov rdi, rax; lea rbp, [rip+table]; xor r13d, r13d; mov r15, [rip+...]; mov r12, rbp; jmp`.
    // Unique in the Enhanced dump (one hit, RVA 0x567d8e). Offsets below are from that hit (the reader starts 0x1e earlier, at 0x567d60).
    constexpr const char* kMinimapTableSigEnhanced = "48 89 C7 48 8D 2D ?? ?? ?? ?? 45 31 ED 4C 8B 3D ?? ?? ?? ?? 49 89 EC E9";
    constexpr LoaderCheck kLoaderChecksEnhanced[] = {
        {0x47, "F3 41 0F 11 44 2E 64", "posX  at element+100 (movss [r14+rbp+0x64], xmm0)"},
        {0x68, "F3 41 0F 11 44 2E 68", "posY  at element+104 (movss [r14+rbp+0x68], xmm0)"},
        {0x89, "F3 41 0F 11 44 2E 6C", "sizeX at element+108 (movss [r14+rbp+0x6c], xmm0)"},
        {0xaa, "F3 41 0F 11 44 2E 70", "sizeY at element+112 (movss [r14+rbp+0x70], xmm0)"},
        {0xc7, "41 88 44 2E 74", "alignX at element+116 (mov [r14+rbp+0x74], al)"},
        {0xe2, "41 88 44 2E 75", "alignY at element+117 (mov [r14+rbp+0x75], al)"},
        {0xe7, "49 FF C5", "next element (inc r13)"},
        {0x102, "49 83 C4 78", "name cursor step (add r12, 0x78)"},
        {0x122, "4D 6B F5 78", "stride 0x78 (imul r14, r13, 0x78)"},
    };
    constexpr uint32_t kMinimapHitEnhanced = 0x567d8e, kMinimapTableEnhanced = 0x3deb800;
    constexpr MinimapLayout kMinimapLayoutEnhanced = {"Enhanced", kMinimapTableSigEnhanced, 24, 12, 6, 10, 0x10, 0x130, kLoaderChecksEnhanced,
                                                      (int)(sizeof(kLoaderChecksEnhanced) / sizeof(kLoaderChecksEnhanced[0])), false, kMinimapHitEnhanced,
                                                      kMinimapTableEnhanced};

    // Appends to hits[have..cap) every place in data[0..n) where the layout's pattern matches; returns the new count. A pattern with a bad token matches nothing.
    inline int FindLayoutHits(const MinimapLayout& L, const uint8_t* data, size_t n, const uint8_t** hits, int cap, int have)
    {
        const int len = L.sigLen;
        int tok[64];
        if (!data || len <= 0 || len > 64 || L.leadTok < 0 || L.leadTok >= len || PatTokenCount(L.sig) != len)
            return have;
        for (int i = 0; i < len; ++i)
            tok[i] = PatTokenAt(L.sig, i);
        if (tok[L.leadTok] < 0 || n < (size_t)len)
            return have;
        const uint8_t lead = (uint8_t)tok[L.leadTok];
        const uint8_t* p = data + L.leadTok;
        const uint8_t* last = data + n - (size_t)(len - L.leadTok);
        while (p <= last && have < cap)
        {
            p = static_cast<const uint8_t*>(std::memchr(p, lead, (size_t)(last - p) + 1));
            if (!p)
                break;
            const uint8_t* s = p - L.leadTok;
            bool ok = true;
            for (int i = 0; i < len && ok; ++i)
                ok = tok[i] == -2 || s[i] == (uint8_t)tok[i];
            if (ok)
                hits[have++] = s;
            ++p;
        }
        return have;
    }

    struct TableResolve
    {
        bool ok = false;
        size_t tableOff = 0;   // table address - image base
        std::string why;
    };

    // hit: where the layout's pattern matched (base <= hit - spanBefore and hit + spanAfter <= base + imageSize is checked here; the caller has proven the bytes
    // readable). Returns the table when the pattern's disp32 points inside the image AND the loader's instructions are the ones the layout was read from;
    // otherwise says which check failed.
    inline TableResolve ResolveMinimapTableFor(const MinimapLayout& L, const uint8_t* hit, const uint8_t* base, size_t imageSize)
    {
        TableResolve r;
        if (!hit || !base || hit < base + L.spanBefore || hit + L.spanAfter > base + imageSize)
        {
            r.why = "the hit is too close to the image edge to check the loader";
            return r;
        }
        int32_t disp = 0;
        std::memcpy(&disp, hit + L.dispAt, 4);
        const int64_t t = (int64_t)(hit - base) + L.endAt + (int64_t)disp;
        if (t < 0 || (uint64_t)t + (uint64_t)kMinimapEntries * (uint64_t)kMinimapStride > (uint64_t)imageSize)
        {
            r.why = "the table address from the pattern is outside the image";
            return r;
        }
        for (int k = 0; k < L.nChecks; ++k)
        {
            const LoaderCheck& c = L.checks[k];
            const int n = HexByteCount(c.bytes);
            for (int i = 0; i < n; ++i)
                if (hit[c.off + i] != (uint8_t)HexByteAt(c.bytes, i))
                {
                    r.why = std::string("the game's frontend.xml reader differs from the one the layout was read from: ") + c.what;
                    return r;
                }
        }
        if (L.readerLea)
        {
            // The reader's `lea rsi, [rip+disp32]` (48 8D 35) 0x16 bytes before the hit must point at element 0 + 104.
            if (hit[-0x16] != 0x48 || hit[-0x15] != 0x8D || hit[-0x14] != 0x35)
            {
                r.why = "the reader does not start with lea rsi, [rip+disp32]";
                return r;
            }
            int32_t rd = 0;
            std::memcpy(&rd, hit - 0x13, 4);
            const int64_t rsiTarget = (int64_t)(hit - base) - 0x16 + 7 + (int64_t)rd;
            if (rsiTarget != t + kOffPosY)
            {
                r.why = "the reader's element pointer is not table + 104";
                return r;
            }
        }
        r.ok = true;
        r.tableOff = (size_t)t;
        return r;
    }

    // The Legacy 1.0.3889 layout (what 0.4.25 shipped).
    inline TableResolve ResolveMinimapTable(const uint8_t* hit, const uint8_t* base, size_t imageSize)
    {
        return ResolveMinimapTableFor(kMinimapLayoutLegacy, hit, base, imageSize);
    }

} // namespace gage::sync
