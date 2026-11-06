// 0.5.4 inv_ped_view (libs/sync/ped_view.h): the CHARACTER panel's window and the scripted camera that frames the player's ped in it.
// The window against the panel's own layout (overlay_inventory.cpp SlotPos / the quick-slot pill / the name chip), its screen rect on several
// displays, the framing projected back through a pinhole camera (the ped's middle on the window's centre, its height the window's kFill, seen
// from the front), and the gate that decides when the camera may render.
// Included by tests/gameplay_test.cpp after its Check() and F(); run by pedview_t::RunPure(), kChecks checks.
#pragma once

#include "sync/ped_view.h"

#include <cmath>
#include <string>

namespace pedview_t {

    namespace pv = gage::sync::pedview;

    constexpr int kChecks = 7;

    inline bool Overlaps(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh)
    {
        return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
    }

    inline void RunPure()
    {
        {
            // the window sits inside the panel and clear of every slot, the quick-slot pill and its label, the name chip and the title
            bool inside = pv::kHoleX > pv::kPanelX && pv::kHoleY > pv::kPanelY && pv::kHoleX + pv::kHoleW < pv::kPanelX + pv::kPanelW &&
                          pv::kHoleY + pv::kHoleH < pv::kPanelY + pv::kPanelH;
            bool clear = true;
            for (int i = 0; i < 14; ++i) {   // overlay_inventory.cpp SlotPos(Equip)
                const float x = i < 7 ? 97.0f : 417.0f, y = 823.0f - 78.0f * (float)(i % 7);
                clear = clear && !Overlaps(pv::kHoleX, pv::kHoleY, pv::kHoleW, pv::kHoleH, x, y, 64.0f, 64.0f);
            }
            clear = clear && !Overlaps(pv::kHoleX, pv::kHoleY, pv::kHoleW, pv::kHoleH, 97.0f, 203.0f, 384.0f, 96.0f);   // quick-slot pill
            clear = clear && pv::kHoleY > 314.0f + 12.0f;                                                                   // QUICK SLOTS label
            clear = clear && pv::kHoleY + pv::kHoleH < 914.0f;                                                              // name chip / title
            Check("ped view: the window (173, 343, 232 x 556) is inside the CHARACTER panel and clear of the 14 slots, the quick-slot pill and "
                  "label, the name chip and the title",
                  inside && clear);
        }
        {
            // its screen rect: the design numbers at 1080p, x4/3 at 1440p, centred with side margins on an ultrawide (as SetGeo / Box)
            const pv::ScreenRect a = pv::Hole(1920, 1080), b = pv::Hole(2560, 1440), c = pv::Hole(3440, 1440);
            const bool p1080 = a.x0 == 173 && a.x1 == 405 && a.y0 == 1080 - 899 && a.y1 == 1080 - 343;
            const bool p1440 = std::fabs(b.x0 - std::floor(173 * 4.0f / 3.0f + 0.5f)) < 0.01f && std::fabs(b.y1 - std::floor((1080 - 343) * 4.0f / 3.0f + 0.5f)) < 0.01f;
            const bool wide = std::fabs((c.x0 - b.x0) - 440.0f) < 0.01f && c.y0 == b.y0 && c.y1 == b.y1;
            Check("ped view: the window's screen rect is the design's at 1920x1080, scaled 4/3 at 2560x1440, shifted 440 px right on 3440x1440",
                  p1080 && p1440 && wide, F("1080p %.0f %.0f %.0f %.0f", a.x0, a.y0, a.x1, a.y1));
        }
        {
            // the framing, projected back: the ped's middle on the window's centre, its height kFill of the window's, the whole ped inside
            struct Disp { float W, H; };
            const Disp disps[] = {{1920, 1080}, {2560, 1440}, {1280, 720}, {3440, 1440}, {1024, 768}, {1920, 1200}};
            const float headings[] = {0.0f, 90.0f, 217.5f, -45.0f, 359.0f};
            int cases = 0, good = 0;
            std::string worst;
            for (const Disp& d : disps)
                for (float h : headings) {
                    ++cases;
                    pv::CamIn in;
                    in.W = d.W;
                    in.H = d.H;
                    in.px = 195.2f;
                    in.py = -933.8f;
                    in.pz = 30.7f;
                    in.headingDeg = h;
                    const pv::CamOut o = pv::Frame(in);
                    const pv::ScreenRect r = pv::Hole(d.W, d.H);
                    const float cx = (r.x0 + r.x1) / d.W - 1.0f, cy = 1.0f - (r.y0 + r.y1) / d.H, hh = (r.y1 - r.y0) / d.H * 2.0f;
                    const float hx0 = r.x0 / d.W * 2.0f - 1.0f, hx1 = r.x1 / d.W * 2.0f - 1.0f;
                    float mx, my, fx, fy, tx, ty;
                    const bool p = pv::Project(o, d.W, d.H, in.px, in.py, in.pz + (pv::kHeadZ + pv::kFeetZ) * 0.5f, mx, my) &&
                                   pv::Project(o, d.W, d.H, in.px, in.py, in.pz + pv::kFeetZ, fx, fy) &&
                                   pv::Project(o, d.W, d.H, in.px, in.py, in.pz + pv::kHeadZ, tx, ty);
                    const float span = ty - fy;
                    const bool ok = o.ok && p && std::fabs(mx - cx) < 0.01f && std::fabs(my - cy) < 0.01f && std::fabs(span / hh - pv::kFill) < 0.02f &&
                                    fx > hx0 && fx < hx1 && tx > hx0 && tx < hx1;
                    good += ok ? 1 : 0;
                    if (!ok && worst.empty())
                        worst = F("%.0fx%.0f h%.1f: mid %.3f %.3f want %.3f %.3f, span %.3f of %.3f", d.W, d.H, h, mx, my, cx, cy, span, hh);
                }
            Check("ped view: on 6 displays x 5 headings the framed ped's middle lands within 0.01 NDC of the window's centre, its height is 86 % of "
                  "the window's (+-2 %), feet and head inside the window",
                  good == cases && cases == 30, worst);
        }
        {
            // the camera stands on the ped's forward line (it sees the ped's front, not an angle), a few metres away, looking a little down
            int good = 0, cases = 0;
            for (float h : {0.0f, 90.0f, 217.5f, -45.0f}) {
                ++cases;
                pv::CamIn in;
                in.headingDeg = h;
                const pv::CamOut o = pv::Frame(in);
                const float k = 3.14159265f / 180.0f;
                const float fx = -std::sin(h * k), fy = std::cos(h * k);
                const float along = o.x * fx + o.y * fy, side = -o.x * fy + o.y * fx;
                good += (along > 3.0f && along < 8.0f && std::fabs(side) < 1e-3f && o.pitchDeg < 0.0f && o.pitchDeg > -10.0f) ? 1 : 0;
            }
            Check("ped view: the camera stands 3..8 m out on the ped's forward line (front view) and looks slightly down", good == cases);
        }
        {
            // a mirrored framing (the window's x flipped) must miss: the sign of the turn is pinned, not just its size
            pv::CamIn in;
            pv::CamOut o = pv::Frame(in);
            o.yawDeg = in.headingDeg + 180.0f - (o.yawDeg - in.headingDeg - 180.0f);
            float mx, my;
            pv::Project(o, in.W, in.H, in.px, in.py, in.pz + (pv::kHeadZ + pv::kFeetZ) * 0.5f, mx, my);
            Check("ped view: a camera turned the other way puts the ped on the RIGHT half (the turn's sign matters)", mx > 0.5f, F("mid x %.2f", mx));
        }
        {
            // the gate: every reason, in order of precedence
            pv::GateIn g;
            g.pageOpen = true;
            g.drawnAgoMs = 16;
            g.ped = true;
            const bool live = pv::Decide(g) == pv::Gate::Live;
            pv::GateIn off = g; off.switchOn = false;
            pv::GateIn closed = g; closed.pageOpen = false;
            pv::GateIn stale = g; stale.drawnAgoMs = 1000;
            pv::GateIn noPed = g; noPed.ped = false;
            pv::GateIn dead = g; dead.dead = true;
            pv::GateIn veh = g; veh.vehicle = true;
            pv::GateIn both = g; both.dead = true; both.vehicle = true; both.pageOpen = false;
            Check("ped view: live only with the switch on, the page open AND drawn in the last 400 ms, a ped, alive, on foot; closed wins over dead",
                  live && pv::Decide(off) == pv::Gate::Off && pv::Decide(closed) == pv::Gate::Closed && pv::Decide(stale) == pv::Gate::Stale &&
                      pv::Decide(noPed) == pv::Gate::NoPed && pv::Decide(dead) == pv::Gate::Dead && pv::Decide(veh) == pv::Gate::Vehicle &&
                      pv::Decide(both) == pv::Gate::Closed);
        }
        {
            // a display the overlay never reported (0 x 0) frames nothing (the engine keeps the mannequin instead of a camera at NaN)
            pv::CamIn in;
            in.W = 0;
            in.H = 0;
            Check("ped view: a 0 x 0 display gives no framing", !pv::Frame(in).ok);
        }
    }

}  // namespace pedview_t
