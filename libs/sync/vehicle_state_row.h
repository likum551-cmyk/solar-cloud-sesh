// 0.4.27 vehicle state row (GI, live 0.4.26: "L locks the car, B the belt, Y the engine: the state changes, but nothing on screen says so").
// Pure C++17, header-only, no game or ImGui calls: the overlay (apps/engine/overlay_ui.cpp DrawStateRow) asks it where the row goes, how
// its three cells are laid out and how bright each cell's change pulse is; tested in tests/vehstate_row_tests.h.
//
// WHY A ROW: 0.4.22 put the belt and engine glyphs at the foot of the speedometer plate (bottom-right), each a 12 % ghost that turns 92 % ink when
// on, and the lock nowhere at all (overlay.h VehicleHud had no lock field; the L key's only answer is a chat line and a frontend sound). A state
// flip there is a glyph changing brightness in the corner the driver is not looking at, with no motion. The row puts all three in one place
// next to the vitals, each as a glyph plus a toggle (gold track, knob right = on; ghost track, knob left = off), and a cell pulses gold for
// kRowPulseMs when its state flips.
//
// WHERE (PlaceStateRow): the row takes the vitals strip's left edge and width, and the strip's height (one family). With the strip docked under
// GTA's radar it goes directly under the strip when the margin below allows; at the default safe zone it does not (2560x1440, live 0.4.26: the strip
// ends 8 px above the screen edge), so it rests `gap` above the radar's top edge instead: it never covers the map (0.4.23 rule). With the vitals in
// the bottom-right plate (radar off) it sits `gap` above the topmost plate of that cluster.
#pragma once

#include <cmath>
#include <cstdint>

namespace gage::sync {

    enum class RowWhere
    {
        None,          // nowhere fits (or no anchor): not drawn
        BelowStrip,    // under the docked vitals strip
        AboveRadar,    // above GTA's radar (the strip's margin is too small)
        AbovePlate,    // above the bottom-right cluster (radar off / vitals not docked)
    };

    inline const char* RowWhereName(RowWhere w)
    {
        switch (w)
        {
        case RowWhere::None: return "nowhere";
        case RowWhere::BelowStrip: return "under the vitals strip";
        case RowWhere::AboveRadar: return "above the radar";
        case RowWhere::AbovePlate: return "above the bottom-right plates";
        }
        return "?";
    }

    struct RowAnchor
    {
        float screenH = 0;
        bool  docked = false;          // the vitals are the strip under the radar
        float x = 0, w = 0;            // the strip's (or plate's) left edge and width: the row's
        float stripBottom = 0;         // docked: the strip's resting bottom edge
        float radarTop = 0;            // docked: the radar rectangle's top edge
        float plateTop = 0;            // not docked: the top of the topmost bottom-right plate
        float rowH = 0;                // the row's height (the strip's)
        float gap = 0;                 // between the row and what it sits next to
        float edge = 0;                // kept clear above the screen's bottom edge
    };

    struct RowRect
    {
        RowWhere where = RowWhere::None;
        float x = 0, y = 0, w = 0, h = 0;   // whole pixels
    };

    inline RowRect PlaceStateRow(const RowAnchor& a)
    {
        RowRect r;
        if (!(a.screenH > 0.0f) || !(a.w > 0.0f) || !(a.rowH > 0.0f))
            return r;
        float y = 0.0f;
        if (a.docked)
        {
            const float below = a.stripBottom + a.gap;
            if (below + a.rowH <= a.screenH - a.edge)
            {
                r.where = RowWhere::BelowStrip;
                y = below;
            }
            else if (a.radarTop - a.gap - a.rowH >= 0.0f)
            {
                r.where = RowWhere::AboveRadar;
                y = a.radarTop - a.gap - a.rowH;
            }
            else
                return r;
        }
        else
        {
            if (a.plateTop - a.gap - a.rowH < 0.0f)
                return r;
            r.where = RowWhere::AbovePlate;
            y = a.plateTop - a.gap - a.rowH;
        }
        r.x = std::floor(a.x);
        r.y = std::floor(y);
        r.w = std::floor(a.w);
        r.h = std::floor(a.rowH);
        return r;
    }

    // ---- the three cells ----------------------------------------------------------------------------------------------
    // The hull is the strip's (a plate with the bottom-right corner cut at `cut`); inside, padL from the left edge, padR from the right (clear of
    // the cut), cells of equal width separated by `sep`. Each cell: the glyph's box (gs square, its top at gy) at the cell's left edge, the
    // toggle right-aligned in the cell. The toggle's track is th tall (the vitals bar's height: gs - 2) and tw = 2 * th + 2 wide; the knob is a
    // (th - 2) square inset 1 px, at the track's right end when on. `toggles` false: the cell is too narrow for glyph + gap + toggle, so only the
    // glyph is drawn (its colour still carries the state).
    constexpr int kRowCells = 3;
    enum RowCell : int { kRowLock = 0, kRowBelt = 1, kRowEngine = 2 };

    struct RowCellBox
    {
        float x0 = 0, x1 = 0;                     // the cell
        float gx = 0;                             // the glyph box's left edge (= x0)
        float tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0; // the toggle track
        float kOff = 0, kOn = 0;                  // the knob's left edge when off / on
    };

    struct RowLayout
    {
        bool  ok = false;
        bool  toggles = false;
        float gs = 0, gy = 0;                     // glyph box side and top
        float pad = 0, cut = 0;
        float knob = 0;                           // the knob's side
        RowCellBox cell[kRowCells];
    };

    inline RowLayout LayoutStateRow(float x, float y, float w, float h, float padL, float sep, float glyphGap)
    {
        RowLayout L;
        if (!(w > 0.0f) || !(h >= 5.0f))
            return L;
        L.cut = std::floor(h * 0.55f);
        L.pad = h >= 11.0f ? 2.0f : 1.0f;
        L.gs = std::fmax(4.0f, h - 2.0f * L.pad);
        L.gy = y + L.pad;
        const float padR = std::fmax(padL, L.cut + 2.0f);
        const float cellW = std::floor((w - padL - padR - sep * (float)(kRowCells - 1)) / (float)kRowCells);
        if (cellW < L.gs)
            return L;
        const float th = std::fmax(3.0f, L.gs - 2.0f);
        const float tw = 2.0f * th + 2.0f;
        L.knob = std::fmax(1.0f, th - 2.0f);
        L.toggles = cellW >= L.gs + glyphGap + tw;
        for (int i = 0; i < kRowCells; ++i)
        {
            RowCellBox& c = L.cell[i];
            c.x0 = std::floor(x + padL + (float)i * (cellW + sep));
            c.x1 = c.x0 + cellW;
            c.gx = c.x0;
            c.tx1 = c.x1;
            c.tx0 = c.x1 - tw;
            c.ty0 = L.gy + 1.0f;                  // the vitals bar row: one px under the glyph box's top
            c.ty1 = c.ty0 + th;
            c.kOff = c.tx0 + 1.0f;
            c.kOn = c.tx1 - 1.0f - L.knob;
        }
        L.ok = true;
        return L;
    }

    // ---- the change pulse ---------------------------------------------------------------------------------------------
    // 0 before the change, up to 1 within kRowPulseRiseMs, then an ease-out back to 0 at kRowPulseMs. A flip is seen at once (a 60 ms rise reads as a
    // flash, not a fade-in) and gone before the next flip a player can make by hand.
    constexpr double kRowPulseRiseMs = 60.0;
    constexpr double kRowPulseMs = 700.0;
    inline float RowPulse(double sinceMs)
    {
        if (!(sinceMs >= 0.0) || sinceMs >= kRowPulseMs)
            return 0.0f;
        if (sinceMs < kRowPulseRiseMs)
            return (float)(sinceMs / kRowPulseRiseMs);
        const double t = (sinceMs - kRowPulseRiseMs) / (kRowPulseMs - kRowPulseRiseMs);   // 0..1
        const double k = 1.0 - t;
        return (float)(k * k);                                                              // ease out
    }

    // What the row shows: lock -1 = not known (no networked car), 0 unlocked, 1 locked; belt and engine as booleans. lockOnly = the on-foot flash
    // after a lock answer (only the lock cell is live).
    struct RowState
    {
        bool visible = false;
        bool lockOnly = false;
        int  lock = -1;
        bool belt = false;
        bool engine = false;
    };

    // Stamps each cell's last change. Appearing (getting into a car) is not a change: the baseline is taken without a pulse, so entering a car does
    // not flash all three. The on-foot lock flash IS the news, so its lock cell pulses when it appears. Render thread only.
    class RowTracker
    {
    public:
        void Update(const RowState& s, double nowMs)
        {
            if (!s.visible)
            {
                seen_ = false;
                return;
            }
            const bool flashStart = s.lockOnly && !(seen_ && last_.lockOnly);
            if (!seen_ || last_.lockOnly != s.lockOnly)
            {
                // a new baseline (no pulse), except the lock flash
                for (double& t : changedMs_)
                    t = -1e18;
                if (flashStart)
                    changedMs_[kRowLock] = nowMs;
            }
            else
            {
                if (s.lock != last_.lock && s.lock >= 0 && last_.lock >= 0)
                    changedMs_[kRowLock] = nowMs;
                if (s.belt != last_.belt)
                    changedMs_[kRowBelt] = nowMs;
                if (s.engine != last_.engine)
                    changedMs_[kRowEngine] = nowMs;
            }
            last_ = s;
            seen_ = true;
        }
        float Pulse(int cell, double nowMs) const
        {
            if (cell < 0 || cell >= kRowCells)
                return 0.0f;
            return RowPulse(nowMs - changedMs_[cell]);
        }

    private:
        bool     seen_ = false;
        RowState last_;
        double   changedMs_[kRowCells] = {-1e18, -1e18, -1e18};
    };

    // How long the on-foot lock flash stays after the server's answer (the car is usually out of the driver's sight by then).
    constexpr double kLockFlashMs = 3000.0;

} // namespace gage::sync
