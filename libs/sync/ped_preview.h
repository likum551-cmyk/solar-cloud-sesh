// ped_preview.h (0.5.8 inv_ped_mode, GI 2026-11-07 after 0.5.7: "the character in inventory should be only char no BG" and "that preview doesn't
// work in cars"). Pure C++17, no game, no Windows: the rules of the inventory's ped preview, driven by the engine's script tick (engine.cpp
// TickInvPedPreview) and pinned by tests/ped_preview_tests.h.
//
// The approach (the standard GTA way to show a ped with no world behind it): a hidden CLONE of the player's ped is handed to GTA's frontend
// ped renderer. While the inventory page is open:
//   ACTIVATE_FRONTEND_MENU(FE_MENU_VERSION_EMPTY_NO_BACKGROUND, togglePause = FALSE, -1)   (we run a single-player session: TRUE would pause
//                                                                                          the world, the other players would see us frozen)
//   CLONE_PED(player, isNetwork false, bScriptHostPed false, copyHeadBlend true), the clone made invisible, frozen, without collision and
//   10 m under the player (it is never seen in the world; the frontend draws its own copy)
//   GIVE_PED_TO_PAUSE_MENU(clone, slot), SET_PAUSE_MENU_PED_LIGHTING(true), SET_PAUSE_MENU_PED_SLEEP_STATE(true = wake up)
//   HUD colour 117 (the pause menu's background) made transparent, the frontend cursor hidden every tick
//   behind the pause menu (SET_SCRIPT_GFX_DRAW_BEHIND_PAUSEMENU): one DRAW_RECT in the panel's own colour over the CHARACTER window, so the
//   window shows the frontend ped on the panel's colour and no world
// and the teardown: CLEAR_PED_IN_PAUSE_MENU, SET_FRONTEND_ACTIVE(false), the HUD colour restored, the clone deleted. The clone is a separate
// ped, so the player's seat does not matter (in a car, on foot, as a passenger): the 0.5.4 camera could only frame a ped on foot.
//
// Sources (facts only): alloc8or natives.json (GIVE_PED_TO_PAUSE_MENU "p1 is either 1 or 2 in the PC scripts", SET_PAUSE_MENU_PED_SLEEP_STATE
// "0 will make the ped slowly fall asleep, 1 will slowly wake the ped up", SET_MOUSE_CURSOR_VISIBLE "shows/hides the frontend cursor on the
// pause menu", SET_SCRIPT_GFX_DRAW_BEHIND_PAUSEMENU); citizenfx/natives HUD/GivePedToPauseMenu.md ("you first must utilize the CLONE_PED");
// the RAGE MP wiki Ui::givePedToPauseMenu ("position 0 = left, 1 = center, 2 = right"); forum.cfx.re "Change native Gta frontend position"
// (the ped can only be put in those three places, ReplaceHudColourWithRgba(117, 0, 0, 0, 0) = transparent background, the slot is the only
// way to move it; nobody shows a way to move or scale it), a qb inventory gist (illie070) whose close restores colour 117 to (31, 31, 31, 100);
// alt:V issue 1202 (EMPTY and EMPTY_NO_BACKGROUND both draw that background; the pause menu hides the game's HUD).
//
// WHERE GTA draws the frontend ped is fixed by its frontend layout and is NOT established offline (no source gives coordinates). So the
// first open of a session SEARCHES the slots: after each GIVE the overlay reads the CHARACTER window back (D3D11, before it draws: the frame
// the game made) and AnalyzeWindow decides whether the ped is in it (on our backdrop colour). The first slot whose ped is in the window is
// kept; none = the session falls back to the 0.5.4 camera (logged with the measured boxes and inv_ped_probe_<n>.bmp, so the next round can
// pin it). A probe that cannot be taken (D3D12 / Enhanced, an odd back buffer format) or a window whose background is not our backdrop
// (the rect did not draw behind the frontend) keeps the slot unverified (the window opens, the log says so). The registry value inv_ped_slot
// (0 / 1 / 2) skips the search.
#pragma once

#include "gtaids.h"
#include "ped_view.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace gage::sync::pedpreview {

    constexpr uint32_t kFeMenuEmptyNoBackground = Joaat("FE_MENU_VERSION_EMPTY_NO_BACKGROUND");
    constexpr int kPauseBgHudColour = 117;               // HUD_COLOUR_PAUSE_BG (the cfx thread and the gist)
    constexpr int kDefaultPauseBg[4] = {31, 31, 31, 100};  // the gist's restore value, used only when GET_HUD_COLOUR cannot be read
    constexpr int kBackdrop[3] = {10, 9, 7};             // overlay_inventory.cpp kGround: the CHARACTER panel's colour (the rect behind the ped)

    // The backdrop: DRAW_RECT's arguments (the rect's CENTRE and size, 0..1 of the screen) covering the CHARACTER window (ped_view.h Hole)
    // with kBackdropPadPx to spare on every side (the overlay's window border and rounded corners lie over the overhang). ok false = no display.
    constexpr float kBackdropPadPx = 2.0f;
    struct NormRect { bool ok = false; float cx = 0, cy = 0, w = 0, h = 0; };
    inline NormRect BackdropRect(float W, float H)
    {
        NormRect n;
        if (!(W > 0.0f) || !(H > 0.0f))
            return n;
        const pedview::ScreenRect r = pedview::Hole(W, H);
        const float x0 = r.x0 - kBackdropPadPx, x1 = r.x1 + kBackdropPadPx, y0 = r.y0 - kBackdropPadPx, y1 = r.y1 + kBackdropPadPx;
        n.cx = (x0 + x1) * 0.5f / W;
        n.cy = (y0 + y1) * 0.5f / H;
        n.w = (x1 - x0) / W;
        n.h = (y1 - y0) / H;
        n.ok = true;
        return n;
    }

    // inv_ped_slot: "0" / "1" / "2" force that slot; unset or anything else = search (kSearchOrder).
    constexpr int kSlotCount = 3;
    constexpr int kSearchOrder[kSlotCount] = {0, 1, 2};   // the window is on the LEFT of the page: left first (RAGE MP wiki: 0 left)
    inline int ParseSlot(const std::wstring& v)
    {
        if (v == L"0") return 0;
        if (v == L"1") return 1;
        if (v == L"2") return 2;
        return -1;
    }

    // ---- when the preview is wanted ---------------------------------------------------------------------------------------------------------
    // Exhaustive; the engine logs the name on every change. NOT a reason: a vehicle (any seat: the clone is a separate ped) and a ragdoll (the
    // clone is cloned standing; the frontend poses its own copy). Dead tears down: the death screen and the respawn own the frame.
    enum class Gate : unsigned char { Off, Closed, Stale, NoPed, Dead, PauseMenu, GaveUp, Live };
    inline const char* GateName(Gate g)
    {
        switch (g) {
        case Gate::Off: return "switch off";
        case Gate::Closed: return "page closed";
        case Gate::Stale: return "page not drawn lately";
        case Gate::NoPed: return "no ped";
        case Gate::Dead: return "dead or dying";
        case Gate::PauseMenu: return "the player's pause menu is up";
        case Gate::GaveUp: return "gave up this session (the 0.5.4 camera instead)";
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
        bool playerPauseMenu = false; // the player's own pause menu (TickPauseMenu's, never ours) is up
        bool gaveUp = false;
    };
    constexpr double kStaleMs = 400.0;
    inline Gate Decide(const GateIn& g)
    {
        if (!g.switchOn) return Gate::Off;
        if (!g.pageOpen) return Gate::Closed;
        if (g.drawnAgoMs > kStaleMs) return Gate::Stale;
        if (!g.ped) return Gate::NoPed;
        if (g.dead) return Gate::Dead;
        if (g.playerPauseMenu) return Gate::PauseMenu;
        if (g.gaveUp) return Gate::GaveUp;
        return Gate::Live;
    }

    // ---- the player's look: when the clone must be taken again ----------------------------------------------------------------------------
    // The frontend shows a COPY taken at GIVE time, so a shirt taken off while the page is open needs a new clone. The engine feeds a
    // signature of the ped's components and props (SigOf) every kLookSampleMs; a new clone is due once a signature different from the
    // cloned one has held for kLookSettleMs (a burst of component writes is one re-clone) and kLookGapMs have passed since the last one.
    constexpr double kLookSampleMs = 500.0, kLookSettleMs = 400.0, kLookGapMs = 1000.0;
    inline uint64_t SigOf(const int* v, int n)
    {
        uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a 64 over the values' little-endian bytes
        for (int i = 0; i < n; ++i)
            for (int b = 0; b < 4; ++b) {
                h ^= (uint64_t)(((uint32_t)v[i] >> (8 * b)) & 0xFFu);
                h *= 0x100000001b3ull;
            }
        return h;
    }
    class LookWatch
    {
    public:
        void Cloned(uint64_t sig, double nowMs)
        {
            shown_ = sig;
            have_ = true;
            pendingSince_ = -1.0;
            lastClone_ = nowMs;
        }
        void Reset() { have_ = false; pendingSince_ = -1.0; }
        // True = take a new clone now (the caller then calls Cloned with the signature it cloned from).
        bool Feed(uint64_t sig, double nowMs)
        {
            if (!have_ || sig == shown_) {
                pendingSince_ = -1.0;
                return false;
            }
            if (pendingSince_ < 0.0 || sig != pending_) {
                pending_ = sig;
                pendingSince_ = nowMs;
                return false;
            }
            return nowMs - pendingSince_ >= kLookSettleMs && nowMs - lastClone_ >= kLookGapMs;
        }

    private:
        uint64_t shown_ = 0, pending_ = 0;
        bool have_ = false;
        double pendingSince_ = -1.0, lastClone_ = -1e9;
    };

    // ---- the probe: is the frontend ped in the CHARACTER window? ---------------------------------------------------------------------------
    // The window's pixels (B, G, R, top row first) as the game drew them, before the overlay. The background is the per-channel median of a
    // band along the window's edge (robust to a ped that crosses an edge); a ped pixel differs from it by more than kPedTol in some channel.
    enum class Verdict : unsigned char { Pending, Found, NotFound, NoBackdrop, Unavailable };
    inline const char* VerdictName(Verdict v)
    {
        switch (v) {
        case Verdict::Pending: return "pending";
        case Verdict::Found: return "the ped is in the window";
        case Verdict::NotFound: return "NO ped in the window";
        case Verdict::NoBackdrop: return "the window's background is not our backdrop (unverifiable)";
        case Verdict::Unavailable: return "no probe on this backend (unverifiable)";
        }
        return "?";
    }
    constexpr int kPedTol = 28;            // a ped pixel: some channel this far from the background
    constexpr int kBackdropTol = 12;       // the background is ours: every channel this close to kBackdrop
    constexpr double kFoundShare = 0.01;   // at least 1 % of the window's pixels are ped ...
    constexpr double kFoundTall = 0.20;    // ... spanning at least 20 % of its height
    struct ProbeStats
    {
        Verdict verdict = Verdict::Pending;
        int w = 0, h = 0;                  // the window, px
        int pedPx = 0;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the ped's box in the window (px, y down; x1 / y1 exclusive), valid when pedPx > 0
        int bg[3] = {0, 0, 0};             // the measured background, R G B
        bool cutTop = false, cutBottom = false, cutLeft = false, cutRight = false;   // the box touches that edge: the ped may be larger
    };
    // bgr: w x h pixels, 3 bytes each (B, G, R), rows `stride` bytes apart, the top row first.
    inline ProbeStats AnalyzeWindow(const uint8_t* bgr, int w, int h, size_t stride)
    {
        ProbeStats s;
        s.w = w;
        s.h = h;
        if (!bgr || w < 8 || h < 8) {
            s.verdict = Verdict::Unavailable;
            return s;
        }
        const int band = (w < h ? w : h) / 40 > 2 ? (w < h ? w : h) / 40 : 2;
        // the edge band: 4 strips, every pixel once per channel histogram (a counting median: no allocation of the band)
        int hist[3][256] = {};
        int n = 0;
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = bgr + (size_t)y * stride;
            const bool edgeRow = y < band || y >= h - band;
            for (int x = 0; x < w; ++x) {
                if (!edgeRow && x >= band && x < w - band)
                    continue;
                const uint8_t* p = row + (size_t)x * 3u;
                ++hist[0][p[2]];
                ++hist[1][p[1]];
                ++hist[2][p[0]];
                ++n;
            }
        }
        for (int c = 0; c < 3; ++c) {
            int seen = 0;
            for (int v = 0; v < 256; ++v) {
                seen += hist[c][v];
                if (seen * 2 > n) {
                    s.bg[c] = v;
                    break;
                }
            }
        }
        int bx0 = w, by0 = h, bx1 = 0, by1 = 0;
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = bgr + (size_t)y * stride;
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = row + (size_t)x * 3u;
                const int dr = p[2] - s.bg[0], dg = p[1] - s.bg[1], db = p[0] - s.bg[2];
                const int d = (dr < 0 ? -dr : dr) > (dg < 0 ? -dg : dg) ? (dr < 0 ? -dr : dr) : (dg < 0 ? -dg : dg);
                const int dd = d > (db < 0 ? -db : db) ? d : (db < 0 ? -db : db);
                if (dd <= kPedTol)
                    continue;
                ++s.pedPx;
                if (x < bx0) bx0 = x;
                if (y < by0) by0 = y;
                if (x + 1 > bx1) bx1 = x + 1;
                if (y + 1 > by1) by1 = y + 1;
            }
        }
        if (s.pedPx > 0) {
            s.x0 = bx0; s.y0 = by0; s.x1 = bx1; s.y1 = by1;
            s.cutTop = by0 <= band;
            s.cutBottom = by1 >= h - band;
            s.cutLeft = bx0 <= band;
            s.cutRight = bx1 >= w - band;
        }
        bool ours = true;
        for (int c = 0; c < 3; ++c) {
            const int d = s.bg[c] - kBackdrop[c];
            ours = ours && (d < 0 ? -d : d) <= kBackdropTol;
        }
        if (!ours)
            s.verdict = Verdict::NoBackdrop;
        else if ((double)s.pedPx >= kFoundShare * (double)w * (double)h && (double)(s.y1 - s.y0) >= kFoundTall * (double)h)
            s.verdict = Verdict::Found;
        else
            s.verdict = Verdict::NotFound;
        return s;
    }

    // ---- the state machine (one per session; the slot search is kept across opens) ---------------------------------------------------------
    // Idle -> Claim (the frontend is ours from this tick: TickPauseMenu stops DISABLE_FRONTEND_THIS_FRAME, which runs BEFORE us in the tick
    // and would eat an activation made in the same frame) -> Opening (ACTIVATE once; wait for IS_PAUSE_MENU_ACTIVE) -> Cloning (CLONE_PED; wait
    // kGiveDelayMs with the clone existing) -> Showing (GIVE; probe after kProbeDelayMs while the slot is unsearched) -> Releasing (teardown;
    // wait for IS_PAUSE_MENU_ACTIVE to read false, force SET_PAUSE_MENU_ACTIVE(false) once after kReleaseMs) -> Idle.
    enum class Step : unsigned char { Idle, Claim, Opening, Cloning, Showing, Releasing };
    inline const char* StepName(Step s)
    {
        switch (s) {
        case Step::Idle: return "idle";
        case Step::Claim: return "claim";
        case Step::Opening: return "opening";
        case Step::Cloning: return "cloning";
        case Step::Showing: return "showing";
        case Step::Releasing: return "releasing";
        }
        return "?";
    }
    constexpr double kOpenTimeoutMs = 1500.0;    // ACTIVATE -> IS_PAUSE_MENU_ACTIVE
    constexpr double kNudgeMs = 500.0;           // ... not yet: SET_FRONTEND_ACTIVE(true) once
    constexpr double kCloneTimeoutMs = 1000.0;   // CLONE_PED -> DOES_ENTITY_EXIST
    constexpr double kGiveDelayMs = 200.0;       // the clone exists this long before GIVE (the cfx snippets wait 200 ms)
    constexpr double kLostMs = 300.0;            // Showing with the frontend reading inactive this long = it closed under us
    constexpr double kProbeDelayMs = 1500.0;     // after GIVE: the ped wakes up (sleep state) before the window is read
    constexpr double kProbeTimeoutMs = 2000.0;   // a probe asked for and never answered = Unavailable
    constexpr double kReleaseMs = 1000.0;        // teardown -> IS_PAUSE_MENU_ACTIVE false, else one SET_PAUSE_MENU_ACTIVE(false)
    constexpr int kMaxFailures = 3;              // consecutive failed opens / lost frontends -> give up for the session

    struct MachineIn
    {
        double nowMs = 0;
        bool want = false;            // Decide() == Live
        float W = 0, H = 0;           // the display the window is on (a new size searches again)
        bool menuActive = false;      // IS_PAUSE_MENU_ACTIVE (read every tick outside Idle)
        bool cloneExists = false;     // DOES_ENTITY_EXIST(clone); false with no clone
        bool lookDue = false;         // LookWatch::Feed
        bool probeAnswered = false;   // the overlay answered the probe asked for
        ProbeStats probe;             // its result (valid with probeAnswered)
    };
    struct MachineOut
    {
        bool open = false;            // ACTIVATE_FRONTEND_MENU(kFeMenuEmptyNoBackground, false, -1) (+ colour 117 transparent)
        bool nudge = false;           // still not active kNudgeMs after the activation: SET_FRONTEND_ACTIVE(true) once
        bool clone = false;           // (delete the old clone) CLONE_PED + hide it
        bool give = false;            // (CLEAR_PED_IN_PAUSE_MENU when re-giving) GIVE_PED_TO_PAUSE_MENU(clone, slot) + lighting + wake
        int slot = 0;
        bool probe = false;           // ask the overlay to read the window back
        bool teardown = false;        // CLEAR_PED_IN_PAUSE_MENU, SET_FRONTEND_ACTIVE(false), colour 117 back, delete the clone
        bool forceClose = false;      // SET_PAUSE_MENU_ACTIVE(false): the frontend outlived the teardown by kReleaseMs
        bool released = false;        // the frontend reads inactive after a teardown (or was forced): back to Idle
        bool gaveUpNow = false;       // edge: the session falls back to the camera from now on
        const char* why = "";         // the reason of a teardown / failure / give-up (a literal)
    };

    class Machine
    {
    public:
        explicit Machine(int forcedSlot = -1) { SetForcedSlot(forcedSlot); }
        void SetForcedSlot(int s)
        {
            forced_ = s >= 0 && s < kSlotCount ? s : -1;
            ResetSearch();
        }

        MachineOut Tick(const MachineIn& in)
        {
            MachineOut o;
            if (in.W > 0 && in.H > 0 && (in.W != searchW_ || in.H != searchH_)) {
                if (searchW_ > 0)
                    ResetSearch();             // another display: the frontend's layout may differ, search again
                searchW_ = in.W;
                searchH_ = in.H;
            }
            if (!in.want && step_ == Step::Claim) {
                step_ = Step::Idle;            // nothing was called yet
                return o;
            }
            // anything past the claim tears down when the preview is no longer wanted
            if (!in.want && step_ != Step::Idle && step_ != Step::Releasing) {
                Teardown(o, in.nowMs, "no longer wanted");
                return o;
            }
            switch (step_) {
            case Step::Idle:
                if (in.want && !gaveUp_) {
                    step_ = Step::Claim;
                    since_ = in.nowMs;
                }
                break;
            case Step::Claim:
                o.open = true;
                step_ = Step::Opening;
                since_ = in.nowMs;
                nudged_ = false;
                break;
            case Step::Opening:
                if (in.menuActive) {
                    o.clone = true;
                    step_ = Step::Cloning;
                    since_ = in.nowMs;
                } else if (in.nowMs - since_ > kOpenTimeoutMs)
                    Fail(o, in.nowMs, "the frontend never read active");
                else if (!nudged_ && in.nowMs - since_ >= kNudgeMs) {
                    o.nudge = true;            // once: SET_FRONTEND_ACTIVE(true), as the cfx snippets call it with the activation
                    nudged_ = true;
                }
                break;
            case Step::Cloning:
                if (!in.cloneExists) {
                    if (in.nowMs - since_ > kCloneTimeoutMs)
                        Fail(o, in.nowMs, "the clone never existed");
                    break;
                }
                if (in.nowMs - since_ >= kGiveDelayMs) {
                    o.give = true;
                    o.slot = Slot();
                    step_ = Step::Showing;
                    reclone_ = false;
                    since_ = in.nowMs;
                    gaveAt_ = in.nowMs;
                    inactiveSince_ = -1.0;
                    probeAskedAt_ = -1.0;
                }
                break;
            case Step::Showing:
                ShowingTick(in, o);
                break;
            case Step::Releasing:
                if (!in.menuActive) {
                    o.released = true;
                    step_ = Step::Idle;
                } else if (in.nowMs - since_ > kReleaseMs) {
                    o.forceClose = true;
                    o.released = true;
                    o.why = "the frontend outlived the teardown";
                    step_ = Step::Idle;
                }
                break;
            }
            return o;
        }

        // The frontend is ours (TickPauseMenu must not treat it as the player's menu nor block it): from the claim until it is released.
        bool OwnsFrontend() const { return step_ != Step::Idle; }
        // The CHARACTER window may show the frontend ped: showing (or taking a new clone while the old copy still shows), and the slot is
        // forced (inv_ped_slot: trusted) or its probe found the ped in the window or could not check. Closed during the search.
        bool WindowOpen() const
        {
            const bool showing = step_ == Step::Showing || (step_ == Step::Cloning && reclone_);
            return showing && (forced_ >= 0 || (searchDone_ && verdict_ != Verdict::NotFound));
        }
        bool GaveUp() const { return gaveUp_; }
        Step StepNow() const { return step_; }
        int Slot() const { return forced_ >= 0 ? forced_ : kSearchOrder[slotIdx_ < kSlotCount ? slotIdx_ : kSlotCount - 1]; }
        bool SearchDone() const { return searchDone_; }
        Verdict LastVerdict() const { return verdict_; }
        int Failures() const { return failures_; }

    private:
        void ShowingTick(const MachineIn& in, MachineOut& o)
        {
            // the frontend closed under us (Esc reached it, the game closed it): tear down, and count it as a failure
            if (!in.menuActive) {
                if (inactiveSince_ < 0.0)
                    inactiveSince_ = in.nowMs;
                else if (in.nowMs - inactiveSince_ >= kLostMs) {
                    Fail(o, in.nowMs, "the frontend closed under us");
                    return;
                }
            } else
                inactiveSince_ = -1.0;
            if (in.nowMs - since_ >= 1000.0 && in.menuActive)
                failures_ = 0;                 // a showing that lasted: the next trouble starts a new count
            if (!in.cloneExists || in.lookDue) {
                o.clone = true;                // a new clone, given again by the Cloning step (same slot)
                reclone_ = true;
                step_ = Step::Cloning;
                since_ = in.nowMs;
                return;
            }
            if (searchDone_)
                return;
            if (probeAskedAt_ < 0.0) {
                if (in.nowMs - gaveAt_ >= kProbeDelayMs) {
                    o.probe = true;
                    o.slot = Slot();
                    probeAskedAt_ = in.nowMs;
                }
                return;
            }
            Verdict v = Verdict::Pending;
            if (in.probeAnswered)
                v = in.probe.verdict;
            else if (in.nowMs - probeAskedAt_ > kProbeTimeoutMs)
                v = Verdict::Unavailable;
            if (v == Verdict::Pending)
                return;
            verdict_ = v;
            if (v != Verdict::NotFound || forced_ >= 0) {
                searchDone_ = true;            // found, or unverifiable: keep this slot (a forced slot is kept whatever the probe says)
                return;
            }
            if (slotIdx_ + 1 < kSlotCount) {
                ++slotIdx_;                    // the next slot: give the same clone again there, probe again
                o.give = true;
                o.slot = Slot();
                gaveAt_ = in.nowMs;
                probeAskedAt_ = -1.0;
                return;
            }
            gaveUp_ = true;                    // no slot puts the ped in the window
            o.gaveUpNow = true;
            Teardown(o, in.nowMs, "no slot puts the ped in the window");
        }
        void Fail(MachineOut& o, double nowMs, const char* why)
        {
            if (++failures_ >= kMaxFailures && !gaveUp_) {
                gaveUp_ = true;
                o.gaveUpNow = true;
            }
            Teardown(o, nowMs, why);
        }
        void Teardown(MachineOut& o, double nowMs, const char* why)
        {
            o.teardown = true;
            o.why = why;
            reclone_ = false;
            step_ = Step::Releasing;
            since_ = nowMs;
        }
        void ResetSearch()
        {
            slotIdx_ = 0;
            searchDone_ = false;
            verdict_ = Verdict::Pending;
            probeAskedAt_ = -1.0;
        }

        Step step_ = Step::Idle;
        double since_ = 0, gaveAt_ = 0, inactiveSince_ = -1.0, probeAskedAt_ = -1.0;
        int forced_ = -1, slotIdx_ = 0, failures_ = 0;
        bool searchDone_ = false, gaveUp_ = false, reclone_ = false, nudged_ = false;
        Verdict verdict_ = Verdict::Pending;
        float searchW_ = 0, searchH_ = 0;
    };

}  // namespace gage::sync::pedpreview
