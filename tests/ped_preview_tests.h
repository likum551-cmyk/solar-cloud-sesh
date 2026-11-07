// 0.5.8 inv_ped_mode (libs/sync/ped_preview.h): the inventory's ped preview on GTA's frontend ped renderer. The menu hash, the gate (a vehicle
// is no reason any more), the slot switch, the backdrop rect, the look watch, the window probe on synthetic frames, the state machine (claim
// before the activation, the slot search, give-up, the frontend lost, the release, the re-clone) and the pause-menu gate's exclusion of our
// frontend (libs/sync/pausemenu.h).
// Included by tests/gameplay_test.cpp after its Check() and F(); run by pedpreview_t::RunPure(), kChecks checks.
#pragma once

#include "sync/pausemenu.h"
#include "sync/ped_preview.h"

#include <cmath>
#include <string>
#include <vector>

namespace pedpreview_t {

    namespace pp = gage::sync::pedpreview;

    constexpr int kChecks = 17;

    // A synthetic window: the backdrop (or `bg`), with a "ped" (a skin-toned head over darker clothes) in the box [x0, x1) x [y0, y1).
    struct Frame
    {
        int w, h;
        std::vector<uint8_t> px;   // B G R, top row first
        Frame(int w_, int h_, int r, int g, int b) : w(w_), h(h_), px((size_t)w_ * h_ * 3)
        {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    Set(x, y, r, g, b);
        }
        void Set(int x, int y, int r, int g, int b)
        {
            uint8_t* p = &px[((size_t)y * w + x) * 3];
            p[0] = (uint8_t)b;
            p[1] = (uint8_t)g;
            p[2] = (uint8_t)r;
        }
        void Ped(int x0, int y0, int x1, int y1)
        {
            const int head = y0 + (y1 - y0) / 8;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    if (y < head)
                        Set(x, y, 196, 150, 120);     // face
                    else
                        Set(x, y, 52, 50, 46);        // a dark shirt / pants: only 42 from the backdrop
                }
        }
        pp::ProbeStats Analyze() const { return pp::AnalyzeWindow(px.data(), w, h, (size_t)w * 3); }
    };

    // A driver for the machine: one tick every 16 ms, a game model that reads the frontend active `activeAfter` ms after an open and the
    // clone existing the tick after a clone, and a probe answered on the next tick by `answer(slot)`.
    struct Rig
    {
        pp::Machine m;
        double now = 0;
        bool menu = false, clone = false;
        double openAt = -1;
        double activeAfter = 50;
        bool frontendClosesItself = false;
        int askedSlot = -1;
        bool askPending = false;
        std::vector<int> gives;          // the slots given, in order
        int opens = 0, clones = 0, teardowns = 0, probes = 0, forced = 0, released = 0, gaveUp = 0;
        bool openedOnClaimTick = false;
        std::string lastWhy;
        pp::Verdict (*answer)(int slot) = nullptr;
        explicit Rig(int forcedSlot = -1) : m(forcedSlot) {}

        pp::MachineOut Tick(bool want, bool lookDue = false, float W = 1920, float H = 1080)
        {
            now += 16;
            if (openAt >= 0 && !menu && !frontendClosesItself && now - openAt >= activeAfter)
                menu = true;
            pp::MachineIn in;
            in.nowMs = now;
            in.want = want;
            in.W = W;
            in.H = H;
            in.menuActive = menu;
            in.cloneExists = clone;
            in.lookDue = lookDue;
            if (askPending && answer) {
                in.probeAnswered = true;
                in.probe.verdict = answer(askedSlot);
                askPending = false;
            }
            const pp::Step before = m.StepNow();
            const pp::MachineOut o = m.Tick(in);
            if (o.teardown) {
                ++teardowns;
                lastWhy = o.why;
                clone = false;
                openAt = -1;
                menu = false;                  // the frontend closes at once in this model (the release reads it next tick)
            }
            if (o.open) {
                ++opens;
                openAt = now;
                if (before == pp::Step::Idle)
                    openedOnClaimTick = true;
            }
            if (o.clone) { ++clones; clone = true; }
            if (o.give) gives.push_back(o.slot);
            if (o.probe) { ++probes; askedSlot = o.slot; askPending = true; }
            if (o.forceClose) ++forced;
            if (o.released) ++released;
            if (o.gaveUpNow) ++gaveUp;
            return o;
        }
        void Run(bool want, double ms) { for (double t = 0; t < ms; t += 16) Tick(want); }
    };

    inline pp::Verdict FoundOnlyIn2(int slot) { return slot == 2 ? pp::Verdict::Found : pp::Verdict::NotFound; }
    inline pp::Verdict NeverFound(int) { return pp::Verdict::NotFound; }
    inline pp::Verdict AlwaysFound(int) { return pp::Verdict::Found; }

    inline void RunPure()
    {
        {
            // the menu hash is GET_HASH_KEY of the name; Joaat reproduces the SP pause menu hash the 0.4.x engine logged live (0xd528c7e2)
            Check("ped preview: FE_MENU_VERSION_EMPTY_NO_BACKGROUND hashes to 0x853523aa with the Joaat that gives the live-logged SP_PAUSE 0xd528c7e2",
                  pp::kFeMenuEmptyNoBackground == 0x853523aau && gage::sync::kFeMenuVersionSpPause == 0xd528c7e2u,
                  F("0x%08x", (unsigned)pp::kFeMenuEmptyNoBackground));
        }
        {
            // the gate: precedence, and a vehicle is not an input any more (GI: "doesn't work in cars"); dead tears down
            pp::GateIn g;
            g.pageOpen = true;
            g.drawnAgoMs = 16;
            g.ped = true;
            const bool live = pp::Decide(g) == pp::Gate::Live;
            pp::GateIn off = g; off.switchOn = false;
            pp::GateIn closed = g; closed.pageOpen = false; closed.dead = true; closed.playerPauseMenu = true;
            pp::GateIn stale = g; stale.drawnAgoMs = 1000;
            pp::GateIn noPed = g; noPed.ped = false;
            pp::GateIn dead = g; dead.dead = true; dead.playerPauseMenu = true;
            pp::GateIn pause = g; pause.playerPauseMenu = true; pause.gaveUp = true;
            pp::GateIn gave = g; gave.gaveUp = true;
            Check("ped preview: live with the page open and drawn, a ped, alive, no pause menu of the player's and not given up; closed > stale > "
                  "no ped > dead > pause menu > gave up",
                  live && pp::Decide(off) == pp::Gate::Off && pp::Decide(closed) == pp::Gate::Closed && pp::Decide(stale) == pp::Gate::Stale &&
                      pp::Decide(noPed) == pp::Gate::NoPed && pp::Decide(dead) == pp::Gate::Dead && pp::Decide(pause) == pp::Gate::PauseMenu &&
                      pp::Decide(gave) == pp::Gate::GaveUp);
        }
        {
            Check("ped preview: inv_ped_slot \"0\" / \"1\" / \"2\" force that slot, unset / \"3\" / \"x\" / \"-1\" search",
                  pp::ParseSlot(L"0") == 0 && pp::ParseSlot(L"1") == 1 && pp::ParseSlot(L"2") == 2 && pp::ParseSlot(L"") == -1 &&
                      pp::ParseSlot(L"3") == -1 && pp::ParseSlot(L"x") == -1 && pp::ParseSlot(L"-1") == -1);
        }
        {
            // the backdrop covers the window (ped_view.h Hole) with 2 px to spare on every side, at 1080p, 1440p and on an ultrawide
            bool ok = true;
            std::string worst;
            struct Disp { float W, H; };
            for (const Disp d : {Disp{1920, 1080}, Disp{2560, 1440}, Disp{3440, 1440}}) {
                const float W = d.W, H = d.H;
                const pp::NormRect r = pp::BackdropRect(W, H);
                const gage::sync::pedview::ScreenRect h = gage::sync::pedview::Hole(W, H);
                const float x0 = (r.cx - r.w * 0.5f) * W, x1 = (r.cx + r.w * 0.5f) * W, y0 = (r.cy - r.h * 0.5f) * H, y1 = (r.cy + r.h * 0.5f) * H;
                const bool c = r.ok && std::fabs(x0 - (h.x0 - 2)) < 0.05f && std::fabs(x1 - (h.x1 + 2)) < 0.05f && std::fabs(y0 - (h.y0 - 2)) < 0.05f &&
                               std::fabs(y1 - (h.y1 + 2)) < 0.05f;
                if (!c && worst.empty())
                    worst = F("%.0fx%.0f: rect %.1f..%.1f x %.1f..%.1f, window %.0f..%.0f x %.0f..%.0f", W, H, x0, x1, y0, y1, h.x0, h.x1, h.y0, h.y1);
                ok = ok && c;
            }
            Check("ped preview: the backdrop rect (DRAW_RECT centre + size) is the CHARACTER window plus 2 px on every side; none on a 0 x 0 display",
                  ok && !pp::BackdropRect(0, 0).ok, worst);
        }
        {
            // the look watch: nothing for an unchanged look; one re-clone after a change settled 400 ms; a burst of changes is one re-clone; a
            // change undone before it settled is none; never twice within 1 s
            pp::LookWatch w;
            w.Cloned(1, 0);
            int due = 0;
            for (double t = 500; t <= 3000; t += 500) due += w.Feed(1, t) ? 1 : 0;
            const bool quiet = due == 0;
            int afterChange = 0;
            double at = -1;
            for (double t = 3500; t <= 5000; t += 100)
                if (w.Feed(2, t)) { ++afterChange; if (at < 0) at = t; w.Cloned(2, t); }
            pp::LookWatch b;
            b.Cloned(1, 0);
            int burst = 0;
            const uint64_t seq[] = {2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5};
            double bt = 2000;
            for (uint64_t s : seq) { if (b.Feed(s, bt)) { ++burst; b.Cloned(s, bt); } bt += 100; }
            pp::LookWatch u;
            u.Cloned(1, 0);
            int undone = 0;
            undone += u.Feed(2, 2000) ? 1 : 0;
            undone += u.Feed(1, 2200) ? 1 : 0;
            for (double t = 2400; t <= 4000; t += 100) undone += u.Feed(1, t) ? 1 : 0;
            Check("ped preview: the look watch re-clones once 400 ms after a change, once for a burst, never for an undone change or an unchanged look",
                  quiet && afterChange == 1 && at >= 3900 && at <= 4000 && burst == 1 && undone == 0, F("change at %.0f, %d; burst %d; undone %d", at, afterChange, burst, undone));
            pp::LookWatch gap;
            gap.Cloned(1, 0);
            const bool early = gap.Feed(2, 100) || gap.Feed(2, 600) || gap.Feed(2, 900);   // settled at 600 but within 1 s of the clone
            const bool later = gap.Feed(2, 1000);
            Check("ped preview: the look watch waits 1 s after the last clone even when the change has settled", !early && later);
        }
        {
            // the probe: a ped on our backdrop is found with its box; an empty window is not; the world behind = no backdrop
            Frame f(232, 556, pp::kBackdrop[0], pp::kBackdrop[1], pp::kBackdrop[2]);
            f.Ped(80, 40, 150, 520);
            const pp::ProbeStats s = f.Analyze();
            Frame e(232, 556, pp::kBackdrop[0], pp::kBackdrop[1], pp::kBackdrop[2]);
            const pp::ProbeStats es = e.Analyze();
            Frame wld(232, 556, 120, 135, 150);       // a sky-blue world behind the frontend ped: our rect did not draw behind it
            wld.Ped(80, 40, 150, 520);
            const pp::ProbeStats ws = wld.Analyze();
            Check("ped preview: the window probe finds a ped on our backdrop (box 80..150 x 40..520, the dark clothes included), none in an empty "
                  "window, and calls a non-backdrop background unverifiable",
                  s.verdict == pp::Verdict::Found && s.x0 == 80 && s.x1 == 150 && s.y0 == 40 && s.y1 == 520 && !s.cutTop && !s.cutBottom &&
                      es.verdict == pp::Verdict::NotFound && es.pedPx == 0 && ws.verdict == pp::Verdict::NoBackdrop,
                  F("found %s box %d..%d x %d..%d; empty %s; world %s bg %d,%d,%d", pp::VerdictName(s.verdict), s.x0, s.x1, s.y0, s.y1,
                    pp::VerdictName(es.verdict), pp::VerdictName(ws.verdict), ws.bg[0], ws.bg[1], ws.bg[2]));
            // a ped taller than the window crosses both edges: still found (the median of the edge band stays the backdrop), cut top and bottom;
            // a few stray pixels (a cursor, a speck) are not a ped
            Frame t(232, 556, pp::kBackdrop[0], pp::kBackdrop[1], pp::kBackdrop[2]);
            t.Ped(70, 0, 160, 556);
            const pp::ProbeStats ts = t.Analyze();
            Frame sp(232, 556, pp::kBackdrop[0], pp::kBackdrop[1], pp::kBackdrop[2]);
            for (int y = 300; y < 310; ++y)
                for (int x = 100; x < 110; ++x)
                    sp.Set(x, y, 255, 255, 255);
            const pp::ProbeStats ss = sp.Analyze();
            Check("ped preview: a ped crossing the top and bottom edges is found and marked cut; a 10 x 10 speck is not a ped",
                  ts.verdict == pp::Verdict::Found && ts.cutTop && ts.cutBottom && !ts.cutLeft && ss.verdict == pp::Verdict::NotFound && ss.pedPx == 100,
                  F("tall %s bg %d,%d,%d; speck %s %d px", pp::VerdictName(ts.verdict), ts.bg[0], ts.bg[1], ts.bg[2], pp::VerdictName(ss.verdict), ss.pedPx));
        }
        {
            // the happy path: claim (owns the frontend, calls nothing), the activation on the NEXT tick, the clone once active, the give 200 ms
            // after the clone exists, the probe 1.5 s later, found -> the window opens; no probe again on the next open, same slot
            Rig r;
            r.answer = AlwaysFound;
            r.Tick(true);
            const bool claimOwns = r.m.OwnsFrontend() && r.m.StepNow() == pp::Step::Claim && r.opens == 0;
            r.Run(true, 2500);
            const bool open1 = r.m.WindowOpen() && r.gives.size() == 1 && r.gives[0] == 0 && r.probes == 1 && !r.openedOnClaimTick;
            r.Run(false, 200);
            const bool closed = !r.m.OwnsFrontend() && r.teardowns == 1 && r.released == 1 && !r.m.WindowOpen();
            r.Run(true, 1000);
            const bool open2 = r.m.WindowOpen() && r.gives.size() == 2 && r.gives[1] == 0 && r.probes == 1;
            Check("ped preview: claim first (no call), activate on the next tick, clone, give slot 0, probe once, window open; closed and released; "
                  "the next open needs no probe",
                  claimOwns && open1 && closed && open2,
                  F("claim %d open1 %d closed %d open2 %d gives %zu probes %d", claimOwns, open1, closed, open2, r.gives.size(), r.probes));
        }
        {
            // the slot search: 0 and 1 miss, 2 finds; the window stays closed while searching; the search is kept for the next open
            Rig r;
            r.answer = FoundOnlyIn2;
            bool openWhileSearching = false;
            for (double t = 0; t < 7000; t += 16) {
                r.Tick(true);
                if (!r.m.SearchDone() && r.m.WindowOpen())
                    openWhileSearching = true;
            }
            const bool found = r.m.WindowOpen() && r.m.Slot() == 2 && r.gives.size() == 3 && r.gives[0] == 0 && r.gives[1] == 1 && r.gives[2] == 2;
            r.Run(false, 200);
            r.Run(true, 1000);
            const bool kept = r.m.WindowOpen() && r.gives.back() == 2 && r.probes == 3;
            Check("ped preview: the search gives slots 0, 1, 2 until the probe finds the ped (2), the window closed meanwhile; the next open gives 2 directly",
                  found && kept && !openWhileSearching, F("gives %zu, slot %d, probes %d", r.gives.size(), r.m.Slot(), r.probes));
        }
        {
            // no slot finds it: give up (teardown) -> the gate says GaveUp, no reopen; a forced slot is kept whatever the probe says
            Rig r;
            r.answer = NeverFound;
            r.Run(true, 9000);
            const bool gave = r.m.GaveUp() && r.gaveUp == 1 && r.teardowns == 1 && r.opens == 1 && !r.m.WindowOpen();
            Rig f(1);
            f.answer = NeverFound;
            f.Run(true, 4000);
            Check("ped preview: no slot puts the ped in the window -> give up once (the camera instead), no reopen; a forced slot 1 stays given and open",
                  gave && !f.m.GaveUp() && f.m.WindowOpen() && f.gives.size() == 1 && f.gives[0] == 1,
                  F("gave %d (teardowns %d opens %d); forced open %d gives %zu", gave, r.teardowns, r.opens, f.m.WindowOpen(), f.gives.size()));
        }
        {
            // a probe never answered (D3D12: Unavailable comes back; here: silence) times out after 2 s: the slot is kept unverified, window open
            Rig r;
            r.answer = nullptr;
            r.Run(true, 2600);
            const bool waiting = !r.m.WindowOpen();
            r.Run(true, 2200);
            Check("ped preview: an unanswered probe keeps the window closed for 2 s, then the slot stays unverified and the window opens",
                  waiting && r.m.WindowOpen() && r.m.LastVerdict() == pp::Verdict::Unavailable && r.gives.size() == 1);
        }
        {
            // the frontend never opens: SET_FRONTEND_ACTIVE once after 500 ms, a failure at 1.5 s; 3 failures in a row give up
            Rig r;
            r.frontendClosesItself = true;
            int nudges = 0;
            for (double t = 0; t < 8000; t += 16)
                nudges += r.Tick(true).nudge ? 1 : 0;
            Check("ped preview: a frontend that never reads active is nudged once per open (SET_FRONTEND_ACTIVE) and given up on after 3 failed opens",
                  r.m.GaveUp() && r.opens == 3 && nudges == 3 && r.teardowns == 3 && r.lastWhy == std::string("the frontend never read active"),
                  F("opens %d nudges %d teardowns %d gave %d", r.opens, nudges, r.teardowns, r.m.GaveUp()));
        }
        {
            // the frontend closes under us while showing: teardown after 300 ms, then a fresh open (one failure, not a give-up)
            Rig r;
            r.answer = AlwaysFound;
            r.Run(true, 2500);
            r.menu = false;
            r.openAt = -1;
            r.Run(true, 250);
            const bool notYet = r.teardowns == 0;
            r.Run(true, 100);
            const bool torn = r.teardowns == 1 && r.lastWhy == std::string("the frontend closed under us");
            r.Run(true, 1000);
            Check("ped preview: a frontend that closes under us is torn down after 300 ms and reopened; one failure is no give-up",
                  notYet && torn && r.opens == 2 && r.m.WindowOpen() && !r.m.GaveUp());
        }
        {
            // the release: the frontend reading active 1 s after the teardown is forced closed once
            Rig r;
            r.answer = AlwaysFound;
            r.Run(true, 2500);
            pp::MachineIn in;
            in.nowMs = r.now + 16;
            in.want = false;
            in.W = 1920;
            in.H = 1080;
            in.menuActive = true;
            const pp::MachineOut t = r.m.Tick(in);
            bool forced = false, lostEarly = false, ownsAfter = true;
            int forcedAt = 0;
            for (int i = 1; i <= 80 && !forced; ++i) {
                in.nowMs += 16;
                const pp::MachineOut o = r.m.Tick(in);
                if (o.forceClose) { forced = true; forcedAt = i * 16; ownsAfter = r.m.OwnsFrontend(); }
                else if (!r.m.OwnsFrontend())
                    lostEarly = true;
            }
            Check("ped preview: after the teardown the frontend stays ours until it reads closed; still open after 1 s -> one forced close, then released",
                  t.teardown && forced && forcedAt > 1000 && forcedAt <= 1040 && !lostEarly && !ownsAfter && r.m.StepNow() == pp::Step::Idle,
                  F("forced at %d ms, lost early %d", forcedAt, lostEarly));
        }
        {
            // the look changes while showing: a new clone, given again in the same slot, the window open throughout (the old copy still shows)
            Rig r;
            r.answer = AlwaysFound;
            r.Run(true, 2500);
            r.Tick(true, true);
            bool closedDuring = false;
            for (int i = 0; i < 20; ++i) {
                r.Tick(true);
                closedDuring = closedDuring || !r.m.WindowOpen();
            }
            Check("ped preview: a look change re-clones and gives the same slot again, the window open throughout, no teardown",
                  r.clones == 2 && r.gives.size() == 2 && r.gives[1] == 0 && !closedDuring && r.teardowns == 0);
        }
        {
            // the pause-menu gate (sync/pausemenu.h): while our preview owns the frontend it is never blocked nor counted as the player's menu; the
            // same tick without the flag blocks it (the 0.5.7 code would have eaten the preview's activation every frame)
            gage::sync::PauseMenuGate g;
            gage::sync::PauseMenuIn in;
            in.connected = true;
            in.canOpen = true;
            bool blocked = false, opened = false, activated = false;
            for (int i = 0; i < 60; ++i) {
                in.nowMs = 1000.0 + 16.0 * i;
                in.frontendIsOurs = true;
                in.menuActive = false;      // the engine passes false while ours
                in.openPressed = i == 30;   // a P press while the inventory shows the preview
                const gage::sync::PauseMenuOut o = g.Tick(in);
                blocked = blocked || o.disableFrontend;
                opened = opened || o.justOpened;
                activated = activated || o.activate;
            }
            gage::sync::PauseMenuGate g2;
            gage::sync::PauseMenuIn base;
            base.connected = true;
            base.canOpen = true;
            base.nowMs = 1000.0;
            const bool oldBlocks = g2.Tick(base).disableFrontend;
            Check("ped preview: the pause-menu gate never blocks, counts or opens over the preview's frontend; without the flag it blocks every frame",
                  !blocked && !opened && !activated && oldBlocks);
        }
    }

}  // namespace pedpreview_t
