// r3_t1_health_tests.h - Desync overhaul round 3, T1: "HEALTH STUCK AT 50 %".
// Test-only, header-only. Included by gameplay_test after t6_health_tests.h (it reuses that harness: the real VitalsTracker, VitalsInbox,
// RespawnGate and the new HealthController against a FAKE server and a simulated ped with SWITCHABLE GTA behaviours, t6health::Model).
//
// The live 0.4.7 log (GI): `Welcome.playerHealth 200 -> the ped's maximum will be 300`, `SET_ENTITY_MAX_HEALTH(ped 514, 300) read back 300;
// ped health 200`, then every 10 s `ped 200 / max 300, server health 195 of playerHealth 200, bar 50%; reconciles 12 (gave up 4)`.
// What GTA does with a health above 200 is MEASURED by the probe (HealthController::Begin) and the engine runs DIRECT (GTA holds 100 + ph) or
// SCALED (the server's number is the truth, the ped carries a proportional value inside what it can hold). Evidence level of every check
// here: SIMULATED (the GTA behaviours are models; the point is that the code is right under each of them).
#pragma once

#include "t6_health_tests.h"

#include <cstdlib>
#include <string>

namespace r3t1 {

using namespace gage::sync;
using t6health::Model;
using t6health::Client;
using t6health::FakeLink;

static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static bool AnyLine(const Client& c, const char* part)
{
    for (const std::string& l : c.log)
        if (Has(l, part))
            return true;
    return false;
}

static std::string Now(const Client& c, FakeLink& l)
{
    return t6health::Got(c, l) + t6health::Fmt(" | ceiling %d behaviour %d probes %u drops %u reconciles %u gaveUp %u reprobes %u", c.hc.Ceiling(),
                                                (int)c.hc.Behaviour(), c.hc.Probes(), c.hc.CeilingDrops(), c.v.Stats().reconciles,
                                                c.v.Stats().reconcileGaveUp, c.v.Stats().reprobes);
}

constexpr int kPureChecks = 33;

static int TestPure()
{
    std::printf("-- R3 T1 health: measured GTA ceiling -> DIRECT or SCALED (simulated ped, 5 GTA models, fake server)\n");
    int n = 0;
    const int before = g_run;

    // ---- 1. the live state, reproduced and fixed --------------------------------------------------------------------------------
    {
        // GTA caps the player ped at 200 whatever the maxima read; the server took 5 damage. health_mode=direct is the pre-R3-T1 behaviour.
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Cap200;
        c.hc.wish = HealthMode::Direct;
        c.Welcome(200);
        c.ResetV4();
        c.Run(500);
        l.HurtBy(5);
        c.Run(60000);
        const std::string line = c.hc.StatusLine(c.v, 200, true);
        Check("live 0.4.7 state reproduced (health_mode=direct, GTA caps at 200): ped 200 / max 300, server 195 of 200, bar 50 %, the reconciler tried and gave up",
              c.ped.Health() == 200 && c.ped.max == 300 && l.ServerHealth() == 195 && std::fabs(c.Bar() - 0.5f) < 0.01f &&
                  c.v.Stats().reconciles == 12 && c.v.Stats().reconcileGaveUp == 4 && c.selfHits.empty() && !c.hc.Scaled() &&
                  Has(line, "mode DIRECT (health_mode=direct)") && Has(line, "ped 200 / max 300") && Has(line, "bar 50%") &&
                  Has(line, "server health 195 of playerHealth 200"),
              Now(c, l) + " | " + line);
        ++n;
    }
    {
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Cap200;
        c.Welcome(200);
        c.ResetV4();
        c.Run(500);
        l.HurtBy(5);
        c.Run(15000);
        const std::string line = c.hc.StatusLine(c.v, 200, true);
        Check("the same GTA (cap 200), health_mode=auto: SCALED with the measured ceiling 100, server 195 -> ped 198 on a max of 200, the bar draws the server's 97.5 %, "
              "nothing to reconcile, no self-hit",
              c.hc.Scaled() && c.hc.Ceiling() == 100 && c.hc.Behaviour() == HealthBehaviour::Capped && c.ped.Health() == 198 && c.ped.max == 200 &&
                  l.ServerHealth() == 195 && std::fabs(c.Bar() - 0.975f) < 0.001f && c.v.Stats().reconciles == 0 && c.v.Stats().reconcileGaveUp == 0 &&
                  c.selfHits.empty() && Has(line, "mode SCALED (health_mode=auto)") && Has(line, "measured ceiling 100") &&
                  Has(line, "server health 195 of playerHealth 200") && Has(line, "HEALTH CAPPED"),
              Now(c, l) + " | " + line);
        ++n;
    }
    {
        // health_mode=scaled on a GTA that holds everything: the ceiling reads 200 >= playerHealth, so there is nothing to scale (DIRECT, identity).
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Direct;
        c.hc.wish = HealthMode::Scaled;
        c.Welcome(200);
        c.ResetV4();
        c.Run(1500);
        Check("health_mode=scaled on a GTA that holds 300: nothing to scale, DIRECT, ped 300 / max 300, bar 100 %",
              !c.hc.Scaled() && c.ped.Health() == 300 && c.ped.max == 300 && c.Bar() > 0.995f && c.hc.Behaviour() == HealthBehaviour::Holds, Now(c, l));
        ++n;
    }

    // ---- 2. the probe tells which behaviour is true ------------------------------------------------------------------------------
    {
        struct Row { Model m; HealthBehaviour b; int ceiling; bool scaled; };
        const Row rows[] = {
            { Model::Direct, HealthBehaviour::Holds, 200, false },
            { Model::Cap200, HealthBehaviour::Capped, 100, true },
            { Model::FollowsPedMax, HealthBehaviour::FollowsPedMax, 200, false },
            { Model::CapNextFrame, HealthBehaviour::Capped, 100, true },     // accepted at first, taken back a frame later
            { Model::Cap150, HealthBehaviour::Capped, 50, true },
        };
        for (const Row& r : rows)
        {
            FakeLink l(200);
            Client c(l);
            c.ped.model = r.m;
            c.Welcome(200);
            c.ResetV4();
            c.Run(1500);
            Check((std::string("probe [") + t6health::ModelName(r.m) + "], playerHealth 200: classifies the behaviour, measures the ceiling, picks the mode; the ped is at the top of what it can hold, "
                   "the bar is full, nothing was sent")
                      .c_str(),
                  c.hc.Behaviour() == r.b && c.hc.Ceiling() == r.ceiling && c.hc.Scaled() == r.scaled && c.ped.Health() == 100 + t6health::Span(r.m, 200) &&
                      c.Bar() > 0.995f && c.selfHits.empty() && (r.m != Model::CapNextFrame || c.hc.CeilingDrops() == 1),
                  Now(c, l));
            ++n;
        }
    }
    {
        // ONE log tells which behaviour is true: every step of the sequence with its read-back, then +1 frame and +1 s.
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Cap200;
        c.Welcome(200);
        c.ResetV4();
        c.Run(2000);
        std::string all;
        for (const std::string& s : c.log)
            all += s + "\n";
        Check("the probe lines name every step with its read-back (SET_ENTITY_MAX_HEALTH(300), SET_ENTITY_HEALTH(300), SET_PED_MAX_HEALTH(300), the real hp and max after each), "
              "the verdict, then the +1 frame and +1 s read-backs",
              Has(all, "[session start]") && Has(all, "SET_ENTITY_MAX_HEALTH(300) ok -> hp 200 max 300") && Has(all, "SET_ENTITY_HEALTH(300) ok -> hp 200 max 300") &&
                  Has(all, "SET_PED_MAX_HEALTH(300) ok -> hp 200 max 300") && Has(all, "HEALTH CAPPED") && Has(all, "mode SCALED") &&
                  Has(all, "probe +1 frame: ped hp 200") && Has(all, "probe +1 s: ped hp 200 max 200, tracker wants hp 200"),
              all);
        ++n;
    }

    // ---- 3. death, damage, bar under SCALED ---------------------------------------------------------------------------------------
    for (int ph : { 200, 255 })
    {
        const Model m = Model::Cap200;
        {
            // The server holds 1: the ped is alive (GTA's death line is 100; the proportional value never touches it) and the bar is the server's 1/ph.
            FakeLink l(ph);
            Client c(l);
            c.ped.model = m;
            c.Welcome(ph);
            c.ResetV4();
            c.Run(500);
            l.HurtBy(ph - 1);
            c.Run(1500);
            Check(t6health::Fmt("SCALED death line [playerHealth %d]: the server at 1 keeps the ped ALIVE at GTA 101 (never dies in GTA before the server says dead), bar = 1/ph, nothing sent", ph).c_str(),
                  l.ServerHealth() == 1 && !c.ped.dead && c.ped.Health() == 101 && std::fabs(c.Bar() - 1.0f / (float)ph) < 0.002f && c.selfHits.empty() &&
                      c.life == Client::Life::Alive,
                  Now(c, l));
            ++n;
            l.HurtBy(1);
            c.Run(6000);
            Check(t6health::Fmt("SCALED server death [playerHealth %d]: the last point kills the ped (server number 0 -> ped dead), it respawns at full health and a full bar", ph).c_str(),
                  c.respawns == 1 && !c.ped.dead && l.ServerHealth() == ph && c.ped.Health() == 100 + t6health::Span(m, ph) && c.Bar() > 0.995f && c.selfHits.empty(),
                  Now(c, l));
            ++n;
        }
        {
            FakeLink l(ph);
            Client c(l);
            c.ped.model = m;
            c.Welcome(ph);
            c.ResetV4();
            c.Run(500);
            c.ped.Hurt(99);                 // 99 of 100 GTA units: ONE point from GTA's death line
            c.Run(1500);
            const int loss = t6health::ExpLoss(m, ph, 99);
            Check(t6health::Fmt("SCALED environmental damage [playerHealth %d]: a fall of 99 GTA units is scaled back to %d server points (in 250-point chunks), the server keeps %d, the ped lives on GTA 101-ish", ph, loss, ph - loss).c_str(),
                  c.Hits() == loss && c.selfHits.size() == (size_t)(loss > 250 ? 2 : 1) && l.ServerHealth() == ph - loss && l.ServerHealth() >= 1 && !c.ped.dead &&
                      t6health::PedIs(c, ph - loss) && t6health::BarIs(c, ph - loss),
                  Now(c, l));
            ++n;
        }
        {
            FakeLink l(ph);
            Client c(l);
            c.ped.model = m;
            c.Welcome(ph);
            c.ResetV4();
            c.Run(500);
            c.ped.Hurt(100);                // the whole range: GTA kills the ped
            c.Run(6500);
            Check(t6health::Fmt("SCALED lethal fall [playerHealth %d]: GTA's death is the server's whole health (the whole ph; the wire carries at most 250 per self-hit), the ped respawns at full health", ph).c_str(),
                  c.Hits() == ph && c.selfHits.size() == (size_t)(ph > 250 ? 2 : 1) && c.respawns == 1 && l.ServerHealth() == ph && c.ped.Health() == 100 + t6health::Span(m, ph) && c.Bar() > 0.995f,
                  Now(c, l));
            ++n;
        }
    }
    {
        // Rounding must not drift or make the reconciler fight: 30 falls of ONE GTA unit (2.55 server points at playerHealth 255).
        for (int ph : { 255, 200 })
        {
            FakeLink l(ph);
            Client c(l);
            c.ped.model = Model::Cap200;
            c.Welcome(ph);
            c.ResetV4();
            c.Run(500);
            for (int i = 0; i < 30; ++i)
            {
                c.ped.Hurt(1);
                c.Run(250);
            }
            bool each = c.selfHits.size() == 30;
            int sum = 0;
            for (int h : c.selfHits)
            {
                each = each && h == t6health::ExpLoss(Model::Cap200, ph, 1);
                sum += h;
            }
            Check(t6health::Fmt("SCALED rounding [playerHealth %d]: 30 one-unit falls are 30 self-hits of %d, the server is exactly ph - their sum, the ped follows each ack, the reconciler never writes",
                                ph, t6health::ExpLoss(Model::Cap200, ph, 1)).c_str(),
                  each && l.ServerHealth() == ph - sum && t6health::PedIs(c, ph - sum) && t6health::BarIs(c, ph - sum) && c.v.Stats().reconciles == 0 &&
                      c.v.Stats().reconcileGaveUp == 0 && c.v.Stats().clampsDown == 0 && c.v.Pending() == 0,
                  Now(c, l) + t6health::Fmt(" sum %d", sum));
            ++n;
        }
    }
    {
        // The bar follows the SERVER number and the self-damage in flight: a 30-unit fall (60 server points) whose answer is 1 s late draws 70 % at once.
        FakeLink l(200);
        l.ackDelay = 60;
        Client c(l);
        c.ped.model = Model::Cap200;
        c.Welcome(200);
        c.ResetV4();
        c.Run(500);
        c.ped.Hurt(30);
        c.Run(200);
        const bool inFlight = l.ServerHealth() == 140 && c.v.Health() == 200 && c.v.Pending() == 60 && std::fabs(c.Bar() - 0.70f) < 0.01f && c.ped.Health() == 170;
        const std::string mid = Now(c, l);
        c.Run(2000);
        Check("SCALED bar with the answer in flight: 70 % before the Damage arrives (client still knows 200, pending 60, ped 170), 70 % after (client knows 140): no jump, no reconcile",
              inFlight && c.v.Health() == 140 && c.v.Pending() == 0 && std::fabs(c.Bar() - 0.70f) < 0.01f && c.ped.Health() == 170 && c.v.Stats().reconciles == 0,
              mid + " -> " + Now(c, l));
        ++n;
    }

    // ---- 4. no sticky state ---------------------------------------------------------------------------------------------------------
    {
        // GTA starts keeping the ped below the start's measurement (a clamp that was not there at the probe): the reconciler gives up once, the tracker asks
        // for ONE re-probe, the ceiling is measured again and the mode follows. No self-hit on the way.
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Direct;
        c.Welcome(200);
        c.ResetV4();
        c.Run(1000);
        const bool direct = !c.hc.Scaled() && c.ped.Health() == 300;
        c.ped.model = Model::Cap200;        // GTA now clamps
        l.HurtBy(20);                       // the next apply (280) is cut to 200
        c.Run(20000);
        Check("a clamp that appears after the start: the reconciler gives up, ONE re-probe measures the ceiling again, SCALED follows (server 180 -> ped 190, bar 90 %), no self-hit",
              direct && c.hc.Scaled() && c.hc.Ceiling() == 100 && c.v.Stats().reprobes == 1 && c.hc.Probes() == 2 && c.ped.Health() == 190 && l.ServerHealth() == 180 &&
                  std::fabs(c.Bar() - 0.90f) < 0.001f && c.selfHits.empty(),
              Now(c, l));
        ++n;
    }
    {
        // The probe leaves the ped at its own value for one frame; the tracker must not put the server's value on in that frame, or the one-frame
        // check reads a LOWER server value as GTA taking health back (a false ceiling). A Damage that lands in the very tick of the session start.
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Direct;
        c.Welcome(200);
        l.Hurt(2, 50);                      // answered in the first tick
        c.ResetV4();
        c.Run(1500);
        Check("a Damage in the tick of the probe does not read as GTA taking health back: still DIRECT, no take-back counted, the ped carries 100 + 150, the bar is 75 %",
              !c.hc.Scaled() && c.hc.CeilingDrops() == 0 && c.hc.Behaviour() == HealthBehaviour::Holds && c.ped.Health() == 250 && std::fabs(c.Bar() - 0.75f) < 0.01f &&
                  c.selfHits.empty(),
              Now(c, l));
        ++n;
    }
    {
        // A ped the engine never probed (the entry's SetPlayerModel, a story-character swap nobody acted on): GTA's default 200 / 200 under a server
        // that needs 300. The tracker sees the new handle and asks for the probe by itself.
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Direct;
        c.Welcome(200);
        c.ResetV4();
        c.Run(500);
        c.ped.Swap();                       // not through the guard: nothing calls ApplyMax
        c.Run(2500);
        Check("a silent ped swap (handle changes, nobody probed): the tracker asks for the probe, the new ped gets max 300 and health 300, bar 100 %, no self-hit",
              c.probesAsked == 1 && c.hc.Probes() == 2 && c.ped.max == 300 && c.ped.Health() == 300 && c.Bar() > 0.995f && c.selfHits.empty() && !c.hc.Scaled(),
              Now(c, l));
        ++n;
    }
    {
        // One process, two sessions: the second session's Welcome and ped must not inherit the first's mode.
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Cap200;
        c.Welcome(200);
        c.ResetV4();
        c.Run(1000);
        const bool first = c.hc.Scaled() && c.ped.Health() == 200 && c.ped.max == 200;
        l.ph = 100;                         // the server (or the next one) runs playerHealth 100 and GTA holds everything
        l.health = 100;
        c.ped.Swap();
        c.ped.model = Model::Direct;
        c.Welcome(100);
        c.ResetV4();
        c.Run(1500);
        Check("reconnect to another server: the mode is measured again (SCALED -> DIRECT), ped 200 / max 200 for playerHealth 100, bar 100 %, no self-hit",
              first && !c.hc.Scaled() && c.ped.max == 200 && c.ped.Health() == 200 && c.Bar() > 0.995f && c.selfHits.empty() && c.hc.Probes() == 2,
              Now(c, l));
        ++n;
    }
    {
        // GET_ENTITY_HEALTH unreadable at the start: nothing is learnt (Unknown, DIRECT, a WARN line), and the later re-probe settles it.
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Cap200;
        c.Welcome(200);
        c.ped.healthReadFails = true;
        c.ResetV4();
        c.ped.healthReadFails = false;
        const bool unknown = c.hc.Behaviour() == HealthBehaviour::Unknown && !c.hc.Scaled() && AnyLine(c, "unknown (health unreadable)");
        c.Run(25000);
        Check("an unreadable health at the start learns nothing (Unknown, DIRECT), the reconciler's give-up asks for the probe again and the mode settles on SCALED, no self-hit",
              unknown && c.hc.Scaled() && c.hc.Ceiling() == 100 && c.selfHits.empty() && c.Bar() > 0.995f && c.ped.Health() == 200,
              Now(c, l));
        ++n;
    }

    // ---- 5. the diagnostic line, the registry value, the scale -------------------------------------------------------------------------
    {
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::CapNextFrame;
        c.Welcome(200);
        c.ResetV4();
        c.Run(2500);
        const std::string line = c.hc.StatusLine(c.v, 200, true);
        Check("the 10 s line carries the mode, the measured ceiling, the ped's hp / max, the server's number, the bar, the last read-backs, the reconciler and the take-backs "
              "(a GTA that clamps a frame later: +1 frame took back 1)",
              Has(line, "mode SCALED") && Has(line, "measured ceiling 100") && Has(line, "ped 200 / max 200") && Has(line, "server health 200 of playerHealth 200") &&
                  Has(line, "bar 100%") && Has(line, "read-backs: probe hp 200 max 200") && Has(line, "probes 1 (+1 frame took back 1") && Has(line, "reconciles 0 (gave up 0") &&
                  AnyLine(c, "GTA TOOK IT BACK"),
              line);
        ++n;
    }
    {
        Check("health_mode parses direct / scaled (any case) and treats everything else as auto",
              ParseHealthMode("direct") == HealthMode::Direct && ParseHealthMode("DIRECT") == HealthMode::Direct && ParseHealthMode("Scaled") == HealthMode::Scaled &&
                  ParseHealthMode("") == HealthMode::Auto && ParseHealthMode("auto") == HealthMode::Auto && ParseHealthMode("directx") == HealthMode::Auto &&
                  ParseHealthMode("dire") == HealthMode::Auto && ParseHealthMode("0") == HealthMode::Auto);
        ++n;
    }
    {
        // The scale itself, for every playerHealth 1..255 and every ceiling 10..254 below it.
        bool inRange = true, monotone = true, top = true, loss = true, back = true;
        long checked = 0;
        for (int ph = 2; ph <= 255; ++ph)
            for (int c = 10; c < ph; ++c)
            {
                HealthScale s;
                s.ph = ph;
                s.ceiling = c;
                int prev = 0;
                for (int hp = 1; hp <= ph; ++hp)
                {
                    const int g = s.ToGta(hp);
                    inRange = inRange && g >= 1 && g <= c;
                    monotone = monotone && g >= prev;
                    prev = g;
                    const int round = s.ToServer(g);
                    back = back && std::abs(round - hp) <= (ph + c - 1) / c;
                    ++checked;
                }
                top = top && s.ToGta(ph) == c && s.ToGta(0) == 0 && s.GtaMax() == 100 + c;
                loss = loss && s.LossToServer(1) >= 1 && s.LossToServer(c) == ph;
            }
        Check("HealthScale for every playerHealth 2..255 and ceiling 10..ph-1: a living player maps into 1..ceiling (never the death line), monotone, the top maps to the ceiling, "
              "a loss of the whole range is the whole health, and the round trip is within one step",
              inRange && monotone && top && loss && back && checked > 1000000, t6health::Fmt("checked %ld", checked));
        ++n;
    }
    {
        const HealthScale d = ChooseHealthScale(HealthMode::Auto, 200, 0);
        const HealthScale u = ChooseHealthScale(HealthMode::Scaled, 200, 0);
        const HealthScale f = ChooseHealthScale(HealthMode::Direct, 200, 100);
        const HealthScale a = ChooseHealthScale(HealthMode::Auto, 200, 100);
        const HealthScale hold = ChooseHealthScale(HealthMode::Auto, 200, 200);
        const HealthScale mis = ChooseHealthScale(HealthMode::Auto, 200, 3);
        Check("ChooseHealthScale: auto with nothing measured stays DIRECT, scaled with nothing measured assumes the 200 cap (ceiling 100), direct ignores the measurement, "
              "auto scales a ceiling below playerHealth, a ceiling at or above it stays DIRECT, a misread ceiling (< 10) is not a cap",
              !d.Scaled() && u.Scaled() && u.ceiling == 100 && !f.Scaled() && a.Scaled() && a.ceiling == 100 && !hold.Scaled() && !mis.Scaled(),
              t6health::Fmt("auto/0 %d scaled/0 %d direct/100 %d auto/100 %d auto/200 %d auto/3 %d", d.Scaled(), u.Scaled(), f.Scaled(), a.Scaled(), hold.Scaled(), mis.Scaled()));
        ++n;
    }
    {
        // The pure tracker: DIRECT behaves exactly as before for ped handle 0 (no probe is ever asked) and a SCALED tracker scales a loss.
        VitalsTracker v;
        v.Reset(200, 0);
        VitalsObs o;
        o.nowMs = 16;
        o.health = 200;
        o.maxHealth = 300;
        VitalsOut out = v.Tick(o);
        const bool applied = out.apply && out.setHealth == 300 && !out.probe;
        HealthScale s;
        s.ph = 200;
        s.ceiling = 100;
        v.SetScale(s);
        o.nowMs = 32;
        o.maxHealth = 200;
        out = v.Tick(o);
        const bool scaledApply = out.apply && out.setHealth == 200 && !out.probe;   // server 200 -> 100 + ceiling 100
        o.nowMs = 48;
        out = v.Tick(o);                                  // baseline
        o.nowMs = 64;
        o.health = 170;                                   // 30 GTA units lost
        out = v.Tick(o);
        Check("tracker: no probe is asked for an unknown ped handle (0); SetScale makes the next tick put the server's value on in the new units (server 200 -> GTA 200 under a ceiling of 100) and a "
              "30-unit loss is a 60-point self-hit",
              applied && scaledApply && out.selfHitCount == 1 && out.selfHits[0] == 60 && !out.probe,
              t6health::Fmt("applied %d scaledApply %d hits %d first %d", applied, scaledApply, out.selfHitCount, out.selfHits[0]));
        ++n;
    }
    {
        // A handle nobody probed is measured exactly once.
        VitalsTracker v;
        v.Reset(200, 0);
        VitalsObs o;
        o.nowMs = 16;
        o.health = 200;
        o.maxHealth = 200;
        o.ped = 77;
        VitalsOut first = v.Tick(o);
        o.nowMs = 32;
        VitalsOut second = v.Tick(o);
        v.NoteProbed(77);
        o.ped = 78;                                       // a swap the engine did not probe itself
        o.nowMs = 48;
        VitalsOut third = v.Tick(o);
        o.nowMs = 64;
        VitalsOut fourth = v.Tick(o);
        Check("tracker: an unprobed ped handle asks for a probe ONCE (and does nothing else that tick); a probed one never; a swap to another handle asks again",
              first.probe && !first.apply && !second.probe && third.probe && !fourth.probe && v.Stats().probeAsks == 2,
              t6health::Fmt("first %d second %d third %d fourth %d asks %u", first.probe, second.probe, third.probe, fourth.probe, v.Stats().probeAsks));
        ++n;
    }
    {
        // SET_PLAYER_MAX_ARMOUR / the armour: not scaled. A SCALED tracker reports a lost armour point as one point.
        VitalsTracker v;
        v.Reset(200, 50);
        HealthScale s;
        s.ph = 200;
        s.ceiling = 100;
        v.SetScale(s);
        VitalsObs o;
        o.nowMs = 16;
        o.health = 200;
        o.maxHealth = 200;
        o.armour = 50;
        v.Tick(o);
        o.nowMs = 32;
        v.Tick(o);
        o.nowMs = 48;
        o.armour = 40;
        const VitalsOut out = v.Tick(o);
        Check("SCALED tracker: armour is not scaled (10 armour lost = a self-hit of 10, the health part still scales)",
              out.selfHitCount == 1 && out.selfHits[0] == 10, t6health::Fmt("hits %d first %d", out.selfHitCount, out.selfHits[0]));
        ++n;
    }
    {
        // The scenario matrix still passes with every model at a playerHealth where nothing needs scaling: playerHealth 100 is DIRECT under all models but Cap150.
        int scaledCount = 0;
        bool allFull = true;
        for (Model m : t6health::kModels)
        {
            FakeLink l(100);
            Client c(l);
            c.ped.model = m;
            c.Welcome(100);
            c.ResetV4();
            c.Run(1200);
            scaledCount += c.hc.Scaled() ? 1 : 0;
            allFull = allFull && c.Bar() > 0.995f && c.ped.Health() == 100 + t6health::Span(m, 100) && c.selfHits.empty();
        }
        Check("playerHealth 100 (GTA's default 200 max): DIRECT under every model except the 150 ceiling, a full bar and no self-hit under all five",
              scaledCount == 1 && allFull, t6health::Fmt("scaled in %d of 5", scaledCount));
        ++n;
    }
    return g_run - before == n ? n : -1;
}

}   // namespace r3t1
