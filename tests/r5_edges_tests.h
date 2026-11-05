// R5 telemetry: libs/sync/state_mirror.h (swim and ragdoll edges: what the sender's flag did against what the puppet's read-back showed).
// Included by gameplay_test.cpp after Check() is defined. Pure: a scripted 60 fps clock, a scripted wire flag and a scripted "game".
#pragma once

#include "sync/state_mirror.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace r5edge {

using namespace gage::sync;

constexpr int kChecks = 9;   // asserted by gameplay_test's total

// The game's reaction to the wire flag: the puppet follows a rise after riseLagMs and a fall after fallLagMs (a negative lag = never).
struct Game
{
    double riseLagMs = 200, fallLagMs = 300;
    bool   readable = true;
    // extra puppet-on stretches that nothing asked for: [fromMs, toMs)
    double byItselfFromMs = -1, byItselfToMs = -1;
};

struct Run
{
    StateMirror m;
    int reads = 0;
    int frames = 0;
};

// wire(t) -> the sender's flag. 60 fps from 0 to endMs. Reads only when WantRead says so (as the engine does).
template <class WireFn>
inline void Drive(Run& r, const Game& g, WireFn wire, double endMs)
{
    bool puppet = false;
    bool prevWire = false;
    double wireChangeMs = -1e9;
    for (double t = 0; t < endMs; t += 1000.0 / 60.0)
    {
        const bool w = wire(t);
        if (w != prevWire)
        {
            prevWire = w;
            wireChangeMs = t;
        }
        // the game: follows the wire after its lag
        const double lag = w ? g.riseLagMs : g.fallLagMs;
        if (lag >= 0 && t - wireChangeMs >= lag)
            puppet = w;
        bool on = puppet;
        if (g.byItselfFromMs >= 0 && t >= g.byItselfFromMs && t < g.byItselfToMs)
            on = true;
        ++r.frames;
        int read = -2;
        if (r.m.WantRead(t))
        {
            ++r.reads;
            read = g.readable ? (on ? 1 : 0) : -1;
        }
        r.m.Update(t, w, read);
    }
}

inline bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

inline void RunPure()
{
    // 1. follow: the puppet shows the rise 200 ms and the fall 300 ms after the wire (16.7 ms frames)
    {
        Run r;
        Game g;
        Drive(r, g, [](double t) { return t >= 1000.0 && t < 5000.0; }, 8000.0);
        const StateMirror::Stats& s = r.m.Total();
        Check("edges: a wire rise and fall the puppet follows 200 / 300 ms later: 1/1 wire edges, 1/1 puppet edges, both followed with those delays, nothing missed or unasked",
              s.wireOn == 1 && s.wireOff == 1 && s.puppetOn == 1 && s.puppetOff == 1 && s.followedOn == 1 && s.followedOff == 1 && s.missedOn == 0 && s.missedOff == 0 &&
              s.unasked == 0 && s.superseded == 0 && Near(s.lagOnSumMs, 200.0, 20.0) && Near(s.lagOffSumMs, 300.0, 20.0),
              "wire " + std::to_string(s.wireOn) + "/" + std::to_string(s.wireOff) + " puppet " + std::to_string(s.puppetOn) + "/" + std::to_string(s.puppetOff) +
              " followed " + std::to_string(s.followedOn) + "/" + std::to_string(s.followedOff) + " missed " + std::to_string(s.missedOn) + "/" + std::to_string(s.missedOff) +
              " unasked " + std::to_string(s.unasked) + " lag " + std::to_string(s.lagOnSumMs) + "/" + std::to_string(s.lagOffSumMs));
    }
    // 2. missed: the puppet never shows the rise; judged only after kFollowMs (a late-but-in-time puppet is not a miss: check 1)
    {
        Run r;
        Game g;
        g.riseLagMs = -1;
        Drive(r, g, [](double t) { return t >= 1000.0 && t < 6000.0; }, 8000.0);
        const StateMirror::Stats& s = r.m.Total();
        Check("edges: a puppet that never shows the rise: missed 1, followed 0, puppet edges 0; the fall of a flag the puppet never raised counts as matched at once",
              s.wireOn == 1 && s.missedOn == 1 && s.followedOn == 0 && s.puppetOn == 0 && s.puppetOff == 0 && s.followedOff == 1 && s.missedOff == 0,
              "missed " + std::to_string(s.missedOn) + " followed " + std::to_string(s.followedOn) + " puppet " + std::to_string(s.puppetOn) + "/" + std::to_string(s.puppetOff));
    }
    // 3. unasked: the puppet rises 4 s after the wire went off (nothing asked); a rise within kFollowMs of a wire fall is not counted
    {
        Run r;
        Game g;
        g.byItselfFromMs = 6000.0;
        g.byItselfToMs = 7000.0;
        Drive(r, g, [](double t) { return t >= 1000.0 && t < 2000.0; }, 9000.0);
        const StateMirror::Stats a = r.m.Total();
        Run r2;
        Game g2;
        g2.byItselfFromMs = 2400.0;                   // 400 ms after the wire fall (and the puppet followed the fall at 2300): a re-rise right behind it
        g2.byItselfToMs = 2800.0;
        Drive(r2, g2, [](double t) { return t >= 1000.0 && t < 2000.0; }, 5000.0);
        const StateMirror::Stats b = r2.m.Total();
        Check("edges: a puppet that rises 4 s after the wire went off is 'on by itself' once; a rise 400 ms after a wire fall is not counted",
              a.unasked == 1 && a.puppetOn == 2 && b.unasked == 0 && b.puppetOn == 2,
              "unasked " + std::to_string(a.unasked) + " (puppet rises " + std::to_string(a.puppetOn) + "), near a fall: unasked " + std::to_string(b.unasked) + " (puppet rises " + std::to_string(b.puppetOn) + ")");
    }
    // 4. a flicker: wire on 300 ms then off, the puppet (lag 600 ms) never gets there: the rise is replaced, not missed
    {
        Run r;
        Game g;
        g.riseLagMs = 600;
        Drive(r, g, [](double t) { return t >= 1000.0 && t < 1300.0; }, 5000.0);
        const StateMirror::Stats& s = r.m.Total();
        Check("edges: a 300 ms flicker the puppet is too slow for: the rise is 'replaced by the next edge' (1), not missed; no puppet edge",
              s.wireOn == 1 && s.wireOff == 1 && s.superseded == 1 && s.missedOn == 0 && s.puppetOn == 0,
              "superseded " + std::to_string(s.superseded) + " missed " + std::to_string(s.missedOn) + " puppet on " + std::to_string(s.puppetOn));
    }
    // 5. the read policy: idle = about 2 reads a second, an on flag = every frame
    {
        Run idle;
        Game g;
        Drive(idle, g, [](double) { return false; }, 10000.0);
        Run busy;
        Drive(busy, g, [](double) { return true; }, 10000.0);
        Check("edges: reads while nothing happens are 2 a second (10 s: 19..21 of 600 frames); with the flag on every frame is read (600)",
              idle.reads >= 19 && idle.reads <= 21 && busy.reads == busy.frames && busy.frames >= 599,
              "idle " + std::to_string(idle.reads) + " of " + std::to_string(idle.frames) + ", on " + std::to_string(busy.reads) + " of " + std::to_string(busy.frames));
    }
    // 6. unreadable reads are counted and judge nothing
    {
        Run r;
        Game g;
        g.readable = false;
        Drive(r, g, [](double t) { return t >= 1000.0 && t < 5000.0; }, 8000.0);
        const StateMirror::Stats& s = r.m.Total();
        Check("edges: an unreadable puppet: every read counted as unreadable, no puppet edge, nothing followed and nothing missed",
              s.unreadable == s.reads && s.reads > 100 && s.puppetOn == 0 && s.puppetOff == 0 && s.followedOn == 0 && s.missedOn == 0 && s.wireOn == 1,
              "reads " + std::to_string(s.reads) + " unreadable " + std::to_string(s.unreadable) + " missed " + std::to_string(s.missedOn));
    }
    // 7. windows: TakeWindow returns what happened since the last call and the largest delay per window
    {
        Run r;
        Game g;
        Drive(r, g, [](double t) { return t >= 1000.0 && t < 2000.0; }, 4000.0);
        const StateMirror::Stats w1 = r.m.TakeWindow();
        const StateMirror::Stats w2 = r.m.TakeWindow();
        Check("edges: TakeWindow returns the first window's edges and delays once (wire 1/1, max rise delay about 200 ms), the next call is empty",
              w1.wireOn == 1 && w1.wireOff == 1 && w1.followedOn == 1 && Near(w1.lagOnMaxMs, 200.0, 20.0) && w2.wireOn == 0 && w2.wireOff == 0 && w2.followedOn == 0 &&
              w2.lagOnMaxMs == 0.0 && w2.reads == 0,
              "w1 wire " + std::to_string(w1.wireOn) + "/" + std::to_string(w1.wireOff) + " max " + std::to_string(w1.lagOnMaxMs) + "; w2 wire " + std::to_string(w2.wireOn) + " max " +
              std::to_string(w2.lagOnMaxMs) + " reads " + std::to_string(w2.reads));
    }
    // 8. Rebase (a new ped): a puppet already on at the first read after it is no edge and not 'on by itself'
    {
        StateMirror m;
        m.Update(0.0, false, 0);
        m.Rebase();
        m.Update(5000.0, false, 1);                   // the new ped is already swimming
        const StateMirror::Stats& s = m.Total();
        Check("edges: after Rebase() the first read is a baseline: a puppet that is already on counts no edge and is not 'on by itself'",
              s.puppetOn == 0 && s.unasked == 0 && s.reads == 2, "puppet on " + std::to_string(s.puppetOn) + " unasked " + std::to_string(s.unasked));
    }
    // 9. the line: silent when nothing happened, names both sides when something did
    {
        StateMirror::Stats z;
        Run r;
        Game g;
        Drive(r, g, [](double t) { return t >= 1000.0 && t < 3000.0; }, 6000.0);
        const std::string quiet = StateMirrorLine("swim", z);
        const std::string line = StateMirrorLine("swim", r.m.TakeWindow());
        Check("edges: the log line is empty for a quiet window and for an active one starts 'swim: sender on 1 / off 1, puppet on 1 / off 1' and shows 'on 1 of 1'",
              quiet.empty() && line.find("swim: sender on 1 / off 1, puppet on 1 / off 1") == 0 && line.find("on 1 of 1") != std::string::npos, line);
    }
}

}  // namespace r5edge
