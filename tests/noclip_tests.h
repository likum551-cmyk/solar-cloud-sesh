// 0.4.22 noclip (libs/sync/noclip.h): the movement math, the state machine, the saved / restored flags and the ground
// placement. Included by tests/gameplay_test.cpp after its Check() and F(); run by noclipt::RunPure(), kChecks checks.
// (The namespace is not "noclip": gameplay_test has `using namespace gage::sync`, which already holds a noclip.)
#pragma once

#include "sync/noclip.h"

#include <cmath>
#include <string>

namespace noclipt {

    using namespace gage::sync::noclip;

    constexpr int kChecks = 31;

    static bool Near(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }

    static Keys K(bool fwd = false, bool back = false, bool left = false, bool right = false, bool up = false, bool down = false,
                  bool fast = false, bool slow = false)
    {
        Keys k;
        k.fwd = fwd; k.back = back; k.left = left; k.right = right; k.up = up; k.down = down; k.fast = fast; k.slow = slow;
        return k;
    }

    // One tick the way the engine builds it: alive, entered, the ped (handle 100) as the subject, flags as the game has them.
    static In Tick(double nowMs, Vec3 pos, bool toggle = false)
    {
        In in;
        in.nowMs = nowMs;
        in.toggle = toggle;
        in.ready = true;
        in.entity = 100;
        in.subject = Subject::Ped;
        in.pos = pos;
        return in;
    }

    static Vec3 P(float x, float y, float z)
    {
        Vec3 v;
        v.x = x; v.y = y; v.z = z;
        return v;
    }

    static GroundRead G(bool found, float z)
    {
        GroundRead g;
        g.found = found;
        g.z = z;
        return g;
    }

    // A gate that is already flying: started at `at` with these game flags, then one idle tick.
    static Gate Flying(Vec3 at, Flags read, double* nowMs, int32_t entity = 100, Subject subject = Subject::Ped)
    {
        Gate g;
        In in = Tick(1000.0, at, true);
        in.read = read;
        in.entity = entity;
        in.subject = subject;
        g.Tick(in);
        *nowMs = 1016.0;
        return g;
    }

    static bool HasControl(int c)
    {
        for (int i = 0; i < kDisabledControlCount; ++i)
            if (kDisabledControls[i] == c)
                return true;
        return false;
    }

    inline void RunPure()
    {
        std::printf("-- 0.4.22 noclip: movement math, state machine, saved flags, ground placement\n");

        // ---- movement math ---------------------------------------------------------------------------------------
        Check("noclip: speeds are 12 m/s, Shift 60, Alt 2.4, and Shift + Alt cancel back to 12",
              Near(Speed(K()), 12.0f) && Near(Speed(K(0, 0, 0, 0, 0, 0, 1, 0)), 60.0f) && Near(Speed(K(0, 0, 0, 0, 0, 0, 0, 1)), 2.4f) &&
                  Near(Speed(K(0, 0, 0, 0, 0, 0, 1, 1)), 12.0f));
        {
            const Vec3 n = Velocity(K(1), 0, 0), w = Velocity(K(1), 90, 0), s = Velocity(K(1), 180, 0);
            Check("noclip: W at yaw 0 / 90 / 180 flies +Y / -X / -Y (GTA heading 0 is north, counter-clockwise)",
                  Near(n.x, 0) && Near(n.y, 12) && Near(w.x, -12) && Near(w.y, 0) && Near(s.x, 0) && Near(s.y, -12),
                  F("%.2f,%.2f %.2f,%.2f %.2f,%.2f", n.x, n.y, w.x, w.y, s.x, s.y));
        }
        {
            const Vec3 r0 = Velocity(K(0, 0, 0, 1), 0, 0), l0 = Velocity(K(0, 0, 1, 0), 0, 0), r90 = Velocity(K(0, 0, 0, 1), 90, 0);
            const Vec3 b = Velocity(K(0, 1), 0, 0);
            Check("noclip: D strafes right (+X at yaw 0, +Y at yaw 90), A left, S backs off",
                  Near(r0.x, 12) && Near(r0.y, 0) && Near(l0.x, -12) && Near(r90.x, 0) && Near(r90.y, 12) && Near(b.y, -12),
                  F("%.2f,%.2f %.2f %.2f,%.2f %.2f", r0.x, r0.y, l0.x, r90.x, r90.y, b.y));
        }
        {
            const Vec3 up30 = Velocity(K(1), 0, 30), sp = Velocity(K(0, 0, 0, 0, 1), 0, 45), ct = Velocity(K(0, 0, 0, 0, 0, 1), 0, -45);
            Check("noclip: W follows the pitch (30 deg up: 6 m/s climb, 10.39 along the ground); Space / Ctrl are world up / down whatever the pitch",
                  Near(up30.z, 6.0f) && Near(up30.y, 10.39f) && Near(sp.z, 12) && Near(sp.x, 0) && Near(ct.z, -12),
                  F("%.2f %.2f %.2f %.2f", up30.z, up30.y, sp.z, ct.z));
        }
        {
            const Vec3 d = Velocity(K(1, 0, 0, 1), 0, 0), c = Velocity(K(1, 1), 0, 0), none = Velocity(K(), 0, 0);
            const float dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            Check("noclip: a diagonal (W + D) is still 12 m/s, opposite keys cancel, no key is a stop",
                  Near(dl, 12.0f) && Near(c.x, 0) && Near(c.y, 0) && Near(c.z, 0) && Near(none.x, 0) && Near(none.y, 0),
                  F("diag %.2f", dl));
        }
        {
            const Stepped a = Advance(P(0, 0, 0), P(0, 12, 0), 0.016f), hitch = Advance(P(0, 0, 0), P(0, 12, 0), 1.0f);
            Check("noclip: a step is velocity x dt, and a 1 s hitch moves one 0.1 s step (1.2 m, not 12 m)",
                  Near(a.pos.y, 0.192f, 0.001f) && Near(hitch.pos.y, 1.2f, 0.001f), F("%.3f %.3f", a.pos.y, hitch.pos.y));
        }
        {
            const Stepped e = Advance(P(15895, 0, 100), P(60, 0, 0), 0.1f), f = Advance(P(0, 0, -1895), P(0, 0, -60), 0.1f);
            Check("noclip: the step stays inside the world box minus 100 m (the server drops a state outside it) and stops that axis",
                  Near(e.pos.x, proto::kWorldMaxXY - kBoxMarginM) && e.clamped && Near(e.vel.x, 0) && Near(f.pos.z, proto::kWorldMinZ + kBoxMarginM) &&
                      Near(f.vel.z, 0),
                  F("%.1f %.1f", e.pos.x, f.pos.z));
        }
        Check("noclip: a camera yaw becomes a heading in 0..360",
              Near(NormHeading(-90.0f), 270.0f) && Near(NormHeading(725.0f), 5.0f) && Near(NormHeading(0.0f), 0.0f));
        {
            bool dup = false;
            for (int i = 0; i < kDisabledControlCount; ++i)
                for (int j = i + 1; j < kDisabledControlCount; ++j)
                    dup = dup || kDisabledControls[i] == kDisabledControls[j];
            Check("noclip: the held-disabled controls cover attack, aim, jump, cover, enter, exit, phone, the radio wheel, the move keys, duck and sprint, once each",
                  !dup && HasControl(24) && HasControl(25) && HasControl(22) && HasControl(44) && HasControl(23) && HasControl(75) && HasControl(27) &&
                      HasControl(85) && HasControl(32) && HasControl(35) && HasControl(36) && HasControl(21));
            Check("noclip: ... and leave the vehicle headlight (H) and the phone's arrow controls alone (other work reads them)",
                  !HasControl(74) && !HasControl(172) && !HasControl(173) && !HasControl(174) && !HasControl(175));
        }

        // ---- starting ---------------------------------------------------------------------------------------------
        {
            Gate g;
            In off = Tick(1000, P(0, 0, 10), true);
            off.enabled = false;
            const Out o = g.Tick(off);
            Check("noclip: the switch off (noclip=0) refuses the toggle and nothing starts", o.refused == Refusal::Disabled && !o.begin && !g.Active());
        }
        {
            Gate g;
            In nr = Tick(1000, P(0, 0, 10), true);
            nr.ready = false;
            const Out a = g.Tick(nr);
            In pa = Tick(1000, P(0, 0, 10), true);
            pa.passenger = true;
            const Out b = g.Tick(pa);
            In ns = Tick(1000, P(0, 0, 10), true);
            ns.subject = Subject::None;
            ns.entity = 0;
            const Out c = g.Tick(ns);
            Check("noclip: not ready (dead, respawning), a passenger seat and no subject are each refused with their own reason",
                  a.refused == Refusal::NotReady && b.refused == Refusal::Passenger && c.refused == Refusal::NotReady && !g.Active());
        }
        {
            Gate g;
            In idle = Tick(1000, P(0, 0, 10));
            idle.keys = K(1);
            const Out o = g.Tick(idle);
            Check("noclip: while off the keys and the camera do nothing at all", !o.begin && !o.move && !o.end && !g.Active());
        }
        {
            Gate g;
            In on = Tick(1000, P(5, 6, 40), true);
            on.read.collision = false;     // a non-default start: nothing here may be assumed
            on.read.gravity = false;
            on.read.invincible = true;
            const Out o = g.Tick(on);
            Check("noclip: the toggle starts it once: begin, the flags as read are saved, the subject named",
                  o.begin && g.GetPhase() == Phase::On && !g.Saved().collision && !g.Saved().gravity && g.Saved().invincible && o.entity == 100 &&
                      o.subject == Subject::Ped && !o.move);
        }
        {
            double now = 0;
            Flags none;
            Gate g = Flying(P(0, 0, 50), none, &now, 777, Subject::Vehicle);
            In stop = Tick(now, P(0, 0, 50), true);
            stop.entity = 777;
            stop.subject = Subject::Vehicle;
            stop.probed = true;
            stop.below = G(true, 20.0f);
            const Out o = g.Tick(stop);
            Check("noclip: a vehicle subject is flown, and handed back, under the vehicle's handle", o.end && o.entity == 777 && o.subject == Subject::Vehicle);
        }

        // ---- flying -----------------------------------------------------------------------------------------------
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 50), fl, &now);
            Vec3 at = P(0, 0, 50);
            Out last;
            for (int i = 0; i < 10; ++i, now += 16.0)
            {
                In in = Tick(now, at);
                in.keys = K(1);
                in.yawDeg = 0;
                last = g.Tick(in);
                at = last.pos;
            }
            Check("noclip: 10 ticks of 16 ms on W move it 1.92 m along the view, report the velocity and face the camera",
                  last.move && Near(last.pos.y, 1.92f, 0.01f) && Near(last.vel.y, 12.0f) && Near(last.heading, 0.0f) && last.entity == 100,
                  F("y %.3f vy %.2f", last.pos.y, last.vel.y));
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 50), fl, &now);
            Vec3 tracked = P(0, 0, 50);
            Out last;
            for (int i = 0; i < 10; ++i, now += 16.0)
            {
                In in = Tick(now, P(tracked.x, tracked.y + 0.5f, tracked.z));   // the game's physics step put it 0.5 m ahead of what we wrote
                in.keys = K(1);
                last = g.Tick(in);
                tracked = last.pos;
            }
            Check("noclip: the game's own physics lead (0.5 m ahead of the last write) is not adopted: the flight stays on our tracked line",
                  Near(last.pos.y, 1.92f, 0.01f) && g.Resyncs() == 0, F("y %.3f resyncs %u", last.pos.y, g.Resyncs()));
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 50), fl, &now);
            In in = Tick(now, P(0, 0, 50));
            g.Tick(in);
            now += 16.0;
            In jump = Tick(now, P(500, 700, 30));      // a server correction / respawn moved the subject
            jump.keys = K();
            const Out o = g.Tick(jump);
            Check("noclip: a move by something else (500 m away) is adopted, not undone", o.resynced && Near(o.pos.x, 500) && Near(o.pos.y, 700) && Near(o.pos.z, 30),
                  F("%.1f %.1f %.1f", o.pos.x, o.pos.y, o.pos.z));
        }

        // ---- ending: ground placement and the flags given back ----------------------------------------------------
        {
            double now = 0;
            Flags fl;
            fl.collision = false;          // GI had it that way before: it must come back that way
            fl.gravity = false;
            fl.invincible = true;
            Gate g = Flying(P(120, -40, 85), fl, &now);
            In stop = Tick(now, P(130, -45, 85), true);
            stop.probed = true;
            stop.below = G(true, 31.4f);
            stop.above = G(true, 33.0f);
            const Out o = g.Tick(stop);
            Check("noclip: toggling off with ground under it: placed there (ground + 1 m, same x and y) and the flags come back exactly as read",
                  o.end && o.why == EndWhy::Toggled && o.land.kind == LandKind::Place && Near(o.land.z, 32.4f) && Near(o.land.x, 130) && Near(o.land.y, -45) &&
                      !o.restore.collision && !o.restore.gravity && o.restore.invincible && !g.Active(),
                  F("kind %d z %.2f", (int)o.land.kind, o.land.z));
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 85), fl, &now);
            In stop = Tick(now, P(0, 0, 85), true);
            stop.probed = true;
            stop.below = G(false, 0);
            stop.above = G(false, 0);
            const Out w = g.Tick(stop);
            const bool waiting = w.hold && !w.end && g.GetPhase() == Phase::Landing;
            now += 16.0;
            In next = Tick(now, P(0, 0, 85));
            next.probed = true;
            next.below = G(true, 40.0f);
            const Out p = g.Tick(next);
            Check("noclip: no ground read yet (collision still streaming in): it holds still, keeps flags, and lands when the ground arrives",
                  waiting && p.end && p.land.kind == LandKind::Place && Near(p.land.z, 41.0f) && p.why == EndWhy::Toggled);
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(10, 20, 30), fl, &now);       // noclip began at (10, 20, 30)
            // a verified probe while flying over a road at z 12: that is the safe spot from now on
            In fly = Tick(now, P(300, 400, 150));
            fly.probed = true;
            fly.below = G(true, 12.0f);
            g.Tick(fly);
            now += 16.0;
            In stop = Tick(now, P(310, 410, 150), true);
            stop.probed = true;
            g.Tick(stop);
            now += kLandWaitMs - 100.0;
            In mid = Tick(now, P(310, 410, 150));
            mid.probed = true;
            const Out stillWaiting = g.Tick(mid);
            now += 200.0;
            In late = Tick(now, P(310, 410, 150));
            late.probed = true;
            const Out o = g.Tick(late);
            Check("noclip: with no ground for 1.5 s it falls back to the last place a probe verified (not where it began), never in mid-air",
                  stillWaiting.hold && o.end && o.land.kind == LandKind::Fallback && Near(o.land.x, 300) && Near(o.land.y, 400) && Near(o.land.z, 13.0f),
                  F("kind %d %.1f %.1f %.1f", (int)o.land.kind, o.land.x, o.land.y, o.land.z));
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(10, 20, 30), fl, &now);
            In fly = Tick(now, P(300, 400, 150));
            fly.probed = true;
            fly.below = G(true, 190.0f);                    // a roof ABOVE the subject (190 > 150): not a place to come back to
            g.Tick(fly);
            now += 16.0;
            In fly2 = Tick(now, P(310, 410, 150));
            fly2.probed = true;
            fly2.below = G(true, 0.0f);                    // a 0 read below the map: not solid ground either
            g.Tick(fly2);
            Check("noclip: the safe spot is only moved by a valid ground read below the subject (a roof above it or a 0 read leave it where noclip began)",
                  Near(g.Safe().x, 10) && Near(g.Safe().y, 20) && Near(g.Safe().z, 30));
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 60), fl, &now);
            In stop = Tick(now, P(0, 0, 60), true);
            stop.probed = true;
            stop.below = G(true, 0.0f);                     // below the map: the native writes 0
            stop.above = G(true, 55.0f);                    // the high probe finds the surface
            const Out o = g.Tick(stop);
            Check("noclip: under the terrain the high probe finds the surface and it lands on that, not at 0",
                  o.end && o.land.kind == LandKind::Place && Near(o.land.z, 56.0f), F("kind %d z %.2f", (int)o.land.kind, o.land.z));
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 60), fl, &now);
            In stop = Tick(now, P(0, 0, 60), true);
            stop.probed = true;
            stop.below = G(true, 80.0f);                    // a hill above us (we are inside it): not "under it"
            stop.above = G(true, 85.0f);
            const Out o = g.Tick(stop);
            Check("noclip: a ground reading above the subject (it is inside a hill) is not used; the high probe's surface is",
                  o.end && Near(o.land.z, 86.0f), F("z %.2f", o.land.z));
        }
        {
            double now = 0;
            Flags fl;
            Gate gSea = Flying(P(0, 0, 8), fl, &now);
            In sea = Tick(now, P(0, 0, 8), true);
            sea.probed = true;
            sea.below = G(true, 0.0f);
            sea.above = G(true, 0.0f);
            const Out s = gSea.Tick(sea);
            double now2 = 0;
            Gate gHill = Flying(P(0, 0, 300), fl, &now2);
            In hill = Tick(now2, P(0, 0, 300), true);
            hill.probed = true;
            hill.below = G(true, 0.0f);
            hill.above = G(true, 0.0f);
            const Out h = gHill.Tick(hill);
            Check("noclip: both probes reading exactly 0 is the sea only near sea level (8 m: placed at the surface; 300 m: waits, never dropped to sea level)",
                  s.end && s.land.kind == LandKind::Place && Near(s.land.z, 1.0f) && h.hold && !h.end);
        }
        {
            double now = 0;
            Flags fl;
            fl.invincible = true;
            fl.collision = false;
            Gate g = Flying(P(0, 0, 60), fl, &now);
            In dead = Tick(now, P(0, 0, 60));
            dead.ready = false;
            const Out o = g.Tick(dead);
            Check("noclip: dying while flying ends it at once: the flags come back, nothing is placed (no teleport of a corpse)",
                  o.end && o.why == EndWhy::NotReady && o.land.kind == LandKind::Skip && o.restore.invincible && !o.restore.collision && !g.Active());
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 60), fl, &now);
            In swapped = Tick(now, P(0, 0, 60));
            swapped.entity = 555;                           // the ped was swapped / the vehicle is gone
            const Out o = g.Tick(swapped);
            Check("noclip: a vanished subject ends it at once with the original handle named and nothing placed",
                  o.end && o.why == EndWhy::SubjectGone && o.entity == 100 && o.land.kind == LandKind::Skip);
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 60), fl, &now);
            In sw = Tick(now, P(0, 0, 60));
            sw.enabled = false;                             // noclip=0 re-read at a Welcome while flying
            sw.probed = true;
            sw.below = G(true, 30.0f);
            const Out o = g.Tick(sw);
            Check("noclip: the switch turned off while flying lands the player first (it is not dropped), then gives the flags back",
                  o.end && o.why == EndWhy::SwitchedOff && o.land.kind == LandKind::Place && Near(o.land.z, 31.0f));
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 60), fl, &now);
            In stop = Tick(now, P(0, 0, 60), true);
            const Out w = g.Tick(stop);                     // not probed: waits
            now += 16.0;
            In again = Tick(now, P(0, 0, 60), true);        // the player presses it again while landing
            const Out o = g.Tick(again);
            Check("noclip: a second toggle while landing is ignored (counted) and the landing carries on",
                  w.hold && o.hold && !o.end && g.GetPhase() == Phase::Landing && g.IgnoredToggles() == 1);
        }
        {
            double now = 0;
            Flags first;
            first.collision = false;
            Gate g = Flying(P(0, 0, 60), first, &now);
            In stop = Tick(now, P(0, 0, 60), true);
            stop.probed = true;
            stop.below = G(true, 30.0f);
            g.Tick(stop);
            In again = Tick(now + 100.0, P(0, 0, 31), true);
            again.read.collision = true;                    // this time the game reads collision on
            const Out o = g.Tick(again);
            now += 200.0;
            In stop2 = Tick(now, P(0, 0, 31), true);
            stop2.probed = true;
            stop2.below = G(true, 30.0f);
            const Out e = g.Tick(stop2);
            Check("noclip: a second flight saves the flags it finds then, not the first flight's (and starts / ends are counted 2 / 2)",
                  o.begin && e.end && e.restore.collision && g.Starts() == 2 && g.Ends() == 2);
        }
        {
            double now = 0;
            Flags fl;
            Gate g = Flying(P(0, 0, 60), fl, &now);
            In in = Tick(now, P(0, 0, 60));
            in.keys = K(1);
            in.yawDeg = -90.0f;
            const Out o = g.Tick(in);
            Check("noclip: the heading handed to the engine is the camera yaw normalised (-90 -> 270)", o.move && Near(o.heading, 270.0f));
        }
    }

} // namespace noclipt
