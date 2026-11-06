// ped_view.h (0.5.4 inv_ped_view, GI 2026-11-07: "render ur char in the inventory left side instead of this weird figure"). Pure C++17, no
// game: where the REV3 CHARACTER panel's window onto the world is, and where a scripted camera has to stand so the player's own ped fills it.
//
// The approach: while the inventory page is open, the engine renders a scripted camera (CREATE_CAM_WITH_PARAMS + RENDER_SCRIPT_CAMS) in front
// of the local ped, aimed so the ped lands inside the panel's window, and the overlay leaves that window un-dimmed (a light tint, a thin
// border) instead of drawing the mannequin. The game renders the ped itself, so it is the real ped with its real clothes on both editions
// and both overlay backends (D3D11 and D3D12 only draw LESS: no render-to-texture, no second scene). Restored by RENDER_SCRIPT_CAMS(false) +
// DESTROY_CAM when the page closes, the ped dies, gets into a vehicle, the overlay stops drawing the page (Alt+Tab with the game minimised),
// the session ends, or the switch is off.
//
// Coordinates: the design is 1920 x 1080 with the origin at the BOTTOM-LEFT, y UP (overlay_inventory.cpp's convention); the overlay maps it
// with one scale s = min(H / 1080, W / 1920), centred. The camera math uses GTA's conventions: world z up, heading in degrees counter-
// clockwise from north (+y), forward of heading h = (-sin h, cos h); cam rotation order 2 with rot.x = pitch (up positive) and rot.z = yaw
// (heading); the cam FOV is VERTICAL (an assumption: live-unverified, the 10 s log line prints what was set).
// tests/ped_view_tests.h pins both halves (the window against the overlay's own panel layout; the framing by projecting the ped back).
#pragma once

#include <cmath>

namespace gage::sync::pedview {

    // The window in the CHARACTER panel (design px, bottom-left + size): between the two equipment columns (x 97..161 and 417..481), above the
    // QUICK SLOTS label (baseline 314) and below the name chip (y 914). The panel itself is (64, 172, 448, 800), r 32.
    constexpr float kHoleX = 173.0f, kHoleY = 343.0f, kHoleW = 232.0f, kHoleH = 556.0f, kHoleR = 24.0f;
    constexpr float kPanelX = 64.0f, kPanelY = 172.0f, kPanelW = 448.0f, kPanelH = 800.0f;

    struct ScreenRect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };   // screen px, y DOWN, (x0, y0) top-left

    // overlay_inventory.cpp SetGeo + Box: the design box (x, y, w, h) on a W x H display, snapped to whole pixels as Box() snaps it.
    inline float DesignScale(float W, float H)
    {
        const float s = H / 1080.0f < W / 1920.0f ? H / 1080.0f : W / 1920.0f;
        return s < 0.25f ? 0.25f : s;
    }
    inline ScreenRect DesignBox(float W, float H, float x, float y, float w, float h)
    {
        const float s = DesignScale(W, H);
        const float ox = std::floor((W - 1920.0f * s) * 0.5f), oy = std::floor((H - 1080.0f * s) * 0.5f);
        const auto rn = [](float v) { return std::floor(v + 0.5f); };
        ScreenRect r;
        r.x0 = rn(ox + x * s);
        r.x1 = rn(ox + (x + w) * s);
        r.y0 = rn(oy + (1080.0f - (y + h)) * s);
        r.y1 = rn(oy + (1080.0f - y) * s);
        return r;
    }
    inline ScreenRect Hole(float W, float H) { return DesignBox(W, H, kHoleX, kHoleY, kHoleW, kHoleH); }

    // The ped and the shot. Heights are relative to GET_ENTITY_COORDS of the player ped (its root, about 1 m above the feet).
    constexpr float kFeetZ = -1.0f;        // the soles
    constexpr float kHeadZ = 0.80f;        // the top of the head (hair included)
    constexpr float kFill = 0.86f;         // the ped's height as a share of the window's height (room above the head, under the feet)
    constexpr float kFovDeg = 40.0f;       // vertical FOV of the shot: a short telephoto (less wide-angle stretch than GTA's ~50)

    struct CamIn
    {
        float W = 1920, H = 1080;          // display px
        float px = 0, py = 0, pz = 0;      // the ped's root (GET_ENTITY_COORDS)
        float headingDeg = 0;              // the heading the camera faces the ped from (frozen when the page opened)
    };
    struct CamOut
    {
        bool ok = false;
        float x = 0, y = 0, z = 0;         // camera position
        float pitchDeg = 0, yawDeg = 0;    // rot.x, rot.z (roll 0)
        float fovDeg = kFovDeg;
        float dist = 0;                    // camera to the ped's middle, metres
        float ndcX = 0, ndcY = 0;          // where the ped's middle lands (normalised device coords, y up), for the log
    };

    // The camera stands on the ped's forward line (it sees the ped from the front, not at an angle) and is turned so the ped lands at the
    // window's centre; the distance makes the ped kFill of the window's height.
    inline CamOut Frame(const CamIn& in)
    {
        CamOut o;
        if (!(in.W > 0.0f) || !(in.H > 0.0f))
            return o;
        constexpr float kDeg = 3.14159265358979f / 180.0f;
        const ScreenRect h = Hole(in.W, in.H);
        const float aspect = in.W / in.H;
        const float tanV = std::tan(kFovDeg * 0.5f * kDeg), tanH = tanV * aspect;
        // the window's centre and height in NDC (x right, y up, -1..1)
        const float nx = ((h.x0 + h.x1) * 0.5f) / in.W * 2.0f - 1.0f;
        const float ny = 1.0f - ((h.y0 + h.y1) * 0.5f) / in.H * 2.0f;
        const float nh = (h.y1 - h.y0) / in.H * 2.0f;
        if (!(nh > 0.0f))
            return o;
        // pitch so the ray to the ped's middle is level: tan(pitch) = -ny * tanV (the camera looks a little down to put the ped high)
        const float pitch = -std::atan(ny * tanV);
        // in the yaw frame that ray is (nx * tanH, 1 / cos(pitch)): the ped is alpha to the right of the camera's heading
        const float alpha = std::atan(nx * tanH * std::cos(pitch));
        // depth along the camera axis that makes the ped's span kFill of the window, then the distance along the ray
        const float span = kHeadZ - kFeetZ;
        const float depth = span / (nh * kFill * tanV);
        const float rayLen = std::sqrt(nx * tanH * nx * tanH + 1.0f + ny * tanV * ny * tanV);
        const float dist = depth * rayLen;
        const float hd = in.headingDeg * kDeg;
        const float fx = -std::sin(hd), fy = std::cos(hd);   // the ped's forward
        const float midZ = in.pz + (kHeadZ + kFeetZ) * 0.5f;
        o.x = in.px + fx * dist;
        o.y = in.py + fy * dist;
        o.z = midZ;
        // the camera faces the ped (heading + 180), then turns by alpha: the ped is to the camera's right by alpha, and a heading grows to the
        // left (counter-clockwise), so the ped's direction = camYaw - alpha  =>  camYaw = heading + 180 + alpha
        float yaw = in.headingDeg + 180.0f + alpha / kDeg;
        while (yaw > 180.0f) yaw -= 360.0f;
        while (yaw <= -180.0f) yaw += 360.0f;
        o.yawDeg = yaw;
        o.pitchDeg = pitch / kDeg;
        o.dist = dist;
        o.ndcX = nx;
        o.ndcY = ny;
        o.ok = std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z) && std::isfinite(o.yawDeg) && std::isfinite(o.pitchDeg);
        return o;
    }

    // A pinhole projection with the conventions above (the test's ground truth, and the log's "where would the ped's feet / head land").
    // false = behind the camera.
    inline bool Project(const CamOut& c, float W, float H, float wx, float wy, float wz, float& ndcX, float& ndcY)
    {
        constexpr float kDeg = 3.14159265358979f / 180.0f;
        const float yw = c.yawDeg * kDeg, pt = c.pitchDeg * kDeg;
        const float fwd[3] = {-std::sin(yw) * std::cos(pt), std::cos(yw) * std::cos(pt), std::sin(pt)};
        const float right[3] = {std::cos(yw), std::sin(yw), 0.0f};
        // up = right x forward
        const float up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0]};
        const float d[3] = {wx - c.x, wy - c.y, wz - c.z};
        const float zc = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (!(zc > 0.01f))
            return false;
        const float xc = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        const float yc = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        const float tanV = std::tan(c.fovDeg * 0.5f * kDeg), tanH = tanV * (W / H);
        ndcX = xc / (zc * tanH);
        ndcY = yc / (zc * tanV);
        return true;
    }

    // Why the view is (not) wanted this tick. Exhaustive; the engine logs the name on every change.
    enum class Gate : unsigned char { Off, Closed, Stale, NoPed, Dead, Vehicle, Live };
    inline const char* GateName(Gate g)
    {
        switch (g) {
        case Gate::Off: return "switch off";
        case Gate::Closed: return "page closed";
        case Gate::Stale: return "page not drawn lately";
        case Gate::NoPed: return "no ped";
        case Gate::Dead: return "dead or dying";
        case Gate::Vehicle: return "in or entering a vehicle";
        case Gate::Live: return "live";
        }
        return "?";
    }
    struct GateIn
    {
        bool switchOn = true;
        bool pageOpen = false;        // Overlay::InventoryOpen()
        double drawnAgoMs = 1e9;      // since the overlay last drew the page
        bool ped = false;
        bool dead = false;
        bool vehicle = false;         // in one, or getting into one
    };
    constexpr double kStaleMs = 400.0;   // the overlay draws every frame: 400 ms without one = it stopped (minimised, device lost)
    inline Gate Decide(const GateIn& g)
    {
        if (!g.switchOn) return Gate::Off;
        if (!g.pageOpen) return Gate::Closed;
        if (g.drawnAgoMs > kStaleMs) return Gate::Stale;
        if (!g.ped) return Gate::NoPed;
        if (g.dead) return Gate::Dead;
        if (g.vehicle) return Gate::Vehicle;
        return Gate::Live;
    }

}  // namespace gage::sync::pedview
