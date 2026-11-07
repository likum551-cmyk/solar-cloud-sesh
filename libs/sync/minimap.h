// 0.4.22 minimap (GI: "add a minimap"): GTA's own radar (bottom-left) shows during play, on foot and in a vehicle.
// Pure C++17, single-threaded (the engine's script tick), tested by tests/gameplay_test.cpp (tests/minimap_tests.h). The
// natives live in apps/engine/hook.cpp (World::DisplayRadar / ReadRadar / the scaleform helpers); this header only decides.
//
// Three decisions:
//   * RadarWanted: when DISPLAY_RADAR(true) is issued. The engine owns that native every script tick (the 0.4.20 code hid the
//     radar after the entry; before it nothing touched it). Shown only after the entry is Done (the loader covers the screen
//     until then, and a session that ends sets the entry back to Waiting, which hides it again) and never while the pause
//     menu is up (the frontend draws the map itself; the waypoint is set there and survives). minimap=0 = hidden, as before.
//   * BarsGate: GTA draws a health and an armour bar next to the radar, from the "minimap" scaleform movie. We draw our own
//     vitals, so the bars go: the movie's SETUP_HEALTH_ARMOUR method called with 3 (no bar), the community recipe used by FiveM
//     servers (Cfx forum "Minimap without Health & armour bar", glitchdetector dep_minimap.lua; the method is in no native
//     database, only in minimap.gfx). The recipe also flips the big radar on and off once (SET_BIGMAP_ACTIVE(true) one frame,
//     (false) the next) so the movie rebuilds; that pair is the part to switch off first when a map looks wrong
//     (minimap_bars=2). LIVE-UNVERIFIED: no getter shows the bars, only that the method was accepted.
//   * RadarKeepOut: where the radar sits on screen (the FiveM "minimap anchor" formula over GET_SAFE_ZONE_SIZE: community
//     knowledge, not a first-party number, good to a few percent), so the engine can check the overlay's persistent plates
//     (chat, mark, speedometer) stay clear of it and the overlay can dock under it.
//   * DockVitals: where our health / food plate goes when the radar is on (GI: "under the minimap on the left"): the radar's
//     left edge and width, resting a few pixels above the screen edge, or back in the bottom-right cluster when it would hide too much of
//     the radar. Used by the overlay (apps/engine/overlay_ui.cpp ClusterLayout) and read back by the engine's 10 s line.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace gage::sync {

    // ---- radar on / off ----------------------------------------------------------------------------------------------

    struct RadarInputs
    {
        bool switchOn = false;         // HKCU\Software\GAGE-MP minimap (unset / anything but "0" = on)
        bool entryDone = false;        // the entry into the world finished (g_entry == Done)
        bool pauseMenuActive = false;  // IS_PAUSE_MENU_ACTIVE as of this tick
    };

    inline bool RadarWanted(const RadarInputs& in) { return in.switchOn && in.entryDone && !in.pauseMenuActive; }

    // ---- GTA's health / armour bars ---------------------------------------------------------------------------------

    enum class BarsMode
    {
        Off,            // minimap_bars=0: GTA's bars stay as the game draws them
        Setup,          // minimap_bars=2: SETUP_HEALTH_ARMOUR(3) only
        SetupRefresh,   // unset / 1: the big-radar flip once per entry, then SETUP_HEALTH_ARMOUR(3) every tick
    };

    inline const char* BarsModeName(BarsMode m)
    {
        switch (m)
        {
        case BarsMode::Off: return "off";
        case BarsMode::Setup: return "setup";
        case BarsMode::SetupRefresh: return "setup+refresh";
        }
        return "?";
    }

    inline BarsMode ParseBarsMode(const std::wstring& v)
    {
        if (v == L"0")
            return BarsMode::Off;
        if (v == L"2")
            return BarsMode::Setup;
        return BarsMode::SetupRefresh;
    }

    constexpr int kBarsHideType = 3;                 // SETUP_HEALTH_ARMOUR(3): neither bar
    constexpr double kBarsRequestEveryMs = 1000.0;   // REQUEST_SCALEFORM_MOVIE again at most this often while the movie is missing / not loaded

    enum class BarsAction
    {
        None,
        Request,      // REQUEST_SCALEFORM_MOVIE("minimap") (idempotent)
        RefreshOn,    // SET_BIGMAP_ACTIVE(true, false)
        RefreshOff,   // SET_BIGMAP_ACTIVE(false, false)
        Setup,        // BEGIN_SCALEFORM_MOVIE_METHOD(h, "SETUP_HEALTH_ARMOUR"), ADD_PARAM_INT(3), END
    };

    inline const char* BarsActionName(BarsAction a)
    {
        switch (a)
        {
        case BarsAction::None: return "none";
        case BarsAction::Request: return "request";
        case BarsAction::RefreshOn: return "refresh-on";
        case BarsAction::RefreshOff: return "refresh-off";
        case BarsAction::Setup: return "setup";
        }
        return "?";
    }

    struct BarsInputs
    {
        bool radarOn = false;     // the radar is wanted this tick (RadarWanted)
        bool haveHandle = false;  // REQUEST_SCALEFORM_MOVIE gave a non-zero handle
        bool loaded = false;      // HAS_SCALEFORM_MOVIE_LOADED(handle)
    };

    // What to issue this tick. One action per call; the caller performs it and reports nothing back (the gate decides on the
    // inputs alone), so a native that cannot be called costs a counter in the engine, never a stuck state here.
    class BarsGate
    {
    public:
        struct Stats
        {
            uint32_t requests = 0, refreshOn = 0, refreshOff = 0, setups = 0;
        };

        void SetMode(BarsMode m) { mode_ = m; }
        void SetFlipHoldMs(double ms) { flipHoldMs_ = ms < 0.0 ? 0.0 : ms; }   // 0 = the one-tick flip of 0.4.22-0.4.26
        BarsMode Mode() const { return mode_; }

        // A new session / entry: the refresh is owed again. A big radar left ON stays owed its OFF (bigmapOn_ is not cleared).
        void Reset()
        {
            refreshDone_ = false;
            lastRequestMs_ = -1e18;
        }

        // 0.4.25: the radar's component sizes changed under the movie (minimap_scale): the "minimap" component is only re-read by the movie on a
        // big-radar flip. In the default mode the per-entry flip (after the movie loads, which is after the sizes are written) does it, so the ask is
        // absorbed while that flip is still owed; in every other mode one flip is issued as soon as the radar shows. Independent of Reset().
        void RequestFlip()
        {
            if (mode_ == BarsMode::SetupRefresh && !refreshDone_)
                return;
            flipAsked_ = true;
        }
        bool FlipAsked() const { return flipAsked_; }

        BarsAction Next(const BarsInputs& in, double nowMs)
        {
            // The flip's second half is not optional: a radar left in big mode is worse than a bar left on screen.
            // 0.4.27: held flipHoldMs_ first (live 0.4.26: a one-tick flip left the resized table unread; the big radar needs frames to lay out).
            if (bigmapOn_)
            {
                if (nowMs - bigmapOnMs_ < flipHoldMs_)
                    return BarsAction::None;
                bigmapOn_ = false;
                ++stats_.refreshOff;
                return BarsAction::RefreshOff;
            }
            if (flipAsked_ && in.radarOn)
            {
                flipAsked_ = false;
                bigmapOn_ = true;
                bigmapOnMs_ = nowMs;
                ++stats_.refreshOn;
                return BarsAction::RefreshOn;
            }
            if (mode_ == BarsMode::Off || !in.radarOn)
                return BarsAction::None;
            if (!in.haveHandle || !in.loaded)
            {
                if (nowMs - lastRequestMs_ < kBarsRequestEveryMs)
                    return BarsAction::None;
                lastRequestMs_ = nowMs;
                ++stats_.requests;
                return BarsAction::Request;
            }
            if (mode_ == BarsMode::SetupRefresh && !refreshDone_)
            {
                bigmapOn_ = true;
                bigmapOnMs_ = nowMs;
                refreshDone_ = true;   // the flip is under way: its OFF is owed on the next call whatever happens
                ++stats_.refreshOn;
                return BarsAction::RefreshOn;
            }
            ++stats_.setups;
            return BarsAction::Setup;
        }

        bool BigmapOn() const { return bigmapOn_; }
        const Stats& Counters() const { return stats_; }
        void ClearCounters() { stats_ = Stats(); }

    private:
        BarsMode mode_ = BarsMode::SetupRefresh;
        bool bigmapOn_ = false;     // SET_BIGMAP_ACTIVE(true) issued, the OFF not yet
        bool flipAsked_ = false;    // RequestFlip(): one flip owed as soon as the radar shows (any mode)
        bool refreshDone_ = false;  // the flip finished for this entry
        double lastRequestMs_ = -1e18;
        double bigmapOnMs_ = 0.0;
        double flipHoldMs_ = 0.0;
        Stats stats_;
    };

    // ---- 0.5.8: when the ring's map hole may open -------------------------------------------------------------------------
    // 0.5.2 opened the hole only after the conditions (the table holds the placement, no big-radar flip, the radar shown) held for kHoleSettleMs,
    // because a write or a flip re-lays the radar out over several frames. GI after 0.5.7: "pressing tab (opening inv) and closing lags out the
    // map for 0.5s": closing the inventory only re-shows the radar (DISPLAY_RADAR(true)); nothing re-laid it out, yet the full settle ran again.
    // Now a hole that was OPEN and closed only because the radar was hidden reopens kHoleReshowMs after the radar shows again; anything that
    // touches the layout in between (a write, a flip, a switch: layoutOk false) or a hole that never opened takes the full settle.
    constexpr double kHoleSettleMs = 800.0, kHoleReshowMs = 50.0;
    class RingHoleTimer
    {
    public:
        // layoutOk: the switches are on, the table holds the placement, no big-radar flip runs. shown: the radar is wanted this tick.
        bool Update(bool layoutOk, bool shown, double nowMs)
        {
            if (!layoutOk)
            {
                since_ = -1.0;
                resume_ = false;
                return false;
            }
            if (!shown)
            {
                if (since_ >= 0.0 && nowMs - since_ >= kHoleSettleMs)
                    resume_ = true;   // it was open: only the radar's hide closes it
                since_ = -1.0;
                return false;
            }
            if (since_ < 0.0)
            {
                since_ = resume_ ? nowMs - (kHoleSettleMs - kHoleReshowMs) : nowMs;
                if (resume_)
                    ++reshows_;
                resume_ = false;
            }
            return nowMs - since_ >= kHoleSettleMs;
        }
        double Since() const { return since_; }     // < 0: not counting (the hole is shut)
        uint32_t Reshows() const { return reshows_; }

    private:
        double since_ = -1.0;
        bool resume_ = false;
        uint32_t reshows_ = 0;
    };

    // ---- where the radar is, and whether anything of ours is in the way -----------------------------------------------

    struct ScreenRect
    {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // pixels, y down
    };

    // Touching edges do not overlap; an empty rect (x1 <= x0 or y1 <= y0) overlaps nothing.
    inline bool RectsOverlap(const ScreenRect& a, const ScreenRect& b)
    {
        if (a.x1 <= a.x0 || a.y1 <= a.y0 || b.x1 <= b.x0 || b.y1 <= b.y0)
            return false;
        return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
    }

    constexpr float kSafeZoneWorst = 0.9f;   // the narrowest safe zone the settings slider gives (the biggest margin)

    // GET_SAFE_ZONE_SIZE as the radar uses it: 0.9..1.0, anything else (unreadable, NaN, outside) = the worst case.
    inline float SafeZoneClamped(float safeZone, bool readOk)
    {
        if (!readOk || !(safeZone >= kSafeZoneWorst) || !(safeZone <= 1.0f))
            return kSafeZoneWorst;
        return safeZone;
    }

    // 0.4.25 minimap_scale (libs/sync/minimap_size.h): the radar's three components are shrunk about the safe-zone's bottom-left corner,
    // so the rectangle below keeps its left edge and bottom edge and loses width and height by the factor. The factor the engine passes is the
    // one the game's table really holds (1.0 until the write was read back), never the one asked for.
    constexpr float kRadarScaleMin = 0.5f, kRadarScaleMax = 1.0f;
    inline float ClampRadarScale(float s)
    {
        if (!(s == s))
            return 1.0f;                        // NaN: untouched
        return s < kRadarScaleMin ? kRadarScaleMin : (s > kRadarScaleMax ? kRadarScaleMax : s);
    }

    // The radar's rectangle (the FiveM "minimap anchor" formula): its width is a quarter of the screen HEIGHT, its height
    // 1 / 5.674 of it, and it sits at the bottom-left, inset by 5 % of the screen per tenth of safe-zone shrink. padFrac grows
    // the rectangle on every side by that fraction of the screen height (the gutter we keep clear). scale (0.5..1, default 1 = the
    // game's own size) shrinks width and height about the bottom-left corner; the padding is added after the shrink.
    inline ScreenRect RadarKeepOut(float displayW, float displayH, float safeZone, bool safeZoneOk, float padFrac, float scale = 1.0f)
    {
        ScreenRect r;
        if (!(displayW > 0.0f) || !(displayH > 0.0f))
            return r;
        const float sz = SafeZoneClamped(safeZone, safeZoneOk);
        const float sc = ClampRadarScale(scale);
        const float inset = 0.05f * std::fabs(sz - 1.0f) * 10.0f;   // fraction of the width / height
        const float w = displayH / 4.0f * sc, h = displayH / 5.674f * sc;
        const float left = displayW * inset, bottom = displayH - displayH * inset;
        const float pad = displayH * padFrac;
        r.x0 = left - pad;
        r.y0 = bottom - h - pad;
        r.x1 = left + w + pad;
        r.y1 = bottom + pad;
        return r;
    }

    // ---- our vitals plate under the radar ------------------------------------------------------------------------------
    // GI: "the health and food bar under the minimap on the left", in the place GTA's own bars were. The plate takes the
    // radar's left edge and width. GTA's radar sits one bottom margin above the screen edge (5 % of the height at the
    // default safe zone, 0 at the maximum) and our plate is taller than that margin, so it rests on the bottom edge and
    // reaches up into the radar's lower strip by the missing amount ("cover"). A plate that would cover more than
    // maxCover of the radar (a safe zone near 1.0, a short display) stays in the bottom-right cluster instead, as with
    // minimap=0. The speedometer is not part of this: it rests on the bottom gutter whichever way the plate goes.
    enum class DockWhy
    {
        Docked,        // under the radar
        NoRadar,       // no usable radar rectangle (display size unknown)
        TooNarrow,     // the radar's width does not fit on the display with the plate
        TooShort,      // the plate does not fit the display height
        CoversRadar,   // it would hide more than maxCover of the radar
    };

    inline const char* DockWhyName(DockWhy w)
    {
        switch (w)
        {
        case DockWhy::Docked: return "docked under the radar";
        case DockWhy::NoRadar: return "no radar rectangle";
        case DockWhy::TooNarrow: return "display too narrow";
        case DockWhy::TooShort: return "display too short";
        case DockWhy::CoversRadar: return "would cover too much of the radar";
        }
        return "?";
    }

    // The largest fraction of the radar's height the plate may cover: a third. At the default safe zone the plate covers 24 % at
    // 1080p and 32 % at 720p (the overlay's scale floor of 0.75 makes the plate relatively taller there); 0.95 would need 38 %.
    constexpr float kDockMaxCover = 0.34f;

    struct VitalsDock
    {
        bool docked = false;
        DockWhy why = DockWhy::NoRadar;
        float x = 0, y = 0, w = 0, h = 0;    // the plate (valid when docked)
        float coverPx = 0;                   // how far it reaches up into the radar rectangle (0 = wholly below)
        float coverFrac = 0;                 // coverPx / the radar's height
    };

    // radar: the unpadded rectangle (RadarKeepOut with padFrac 0). plateH: the plate's height. minW / maxW: the width band the plate
    // may take from the radar's width. gap: the space between the radar and a plate that fits wholly below it. edge: the space kept above the screen's bottom edge.
    inline VitalsDock DockVitals(float displayW, float displayH, const ScreenRect& radar, float plateH, float minW, float maxW, float gap,
                                 float edge, float maxCover)
    {
        VitalsDock d;
        const float rw = radar.x1 - radar.x0, rh = radar.y1 - radar.y0;
        if (!(displayW > 0.0f) || !(displayH > 0.0f) || !(rw > 0.0f) || !(rh > 0.0f) || !(plateH > 0.0f))
        {
            d.why = DockWhy::NoRadar;
            return d;
        }
        const float w = rw < minW ? minW : (rw > maxW ? maxW : rw);
        float x = radar.x0 < 0.0f ? 0.0f : radar.x0;
        if (x + w > displayW)
        {
            d.why = DockWhy::TooNarrow;
            return d;
        }
        const float lowestTop = displayH - edge - plateH;   // the plate's top when it rests `edge` above the screen's bottom edge
        if (lowestTop < 0.0f)
        {
            d.why = DockWhy::TooShort;
            return d;
        }
        const float belowTop = radar.y1 + gap;               // directly under the radar, when that fits
        const float top = belowTop <= lowestTop ? belowTop : lowestTop;
        d.coverPx = radar.y1 > top ? radar.y1 - top : 0.0f;
        d.coverFrac = d.coverPx / rh;
        if (d.coverFrac > maxCover)
        {
            d.why = DockWhy::CoversRadar;
            return d;
        }
        d.docked = true;
        d.why = DockWhy::Docked;
        d.x = x;
        d.y = top;
        d.w = w;
        d.h = plateH;
        return d;
    }

} // namespace gage::sync
