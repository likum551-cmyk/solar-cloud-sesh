// gameplay_test: the engine's protocol-v4 decision units, pure and native-free:
//   libs/sync/combat.h        VitalsTracker (server-owned health vs the local ped), RespawnGate,
//                             HitBatcher, ProbePeerDamage
//   libs/sync/emote.h         EmoteArbiter (anim / scenario vs the locomotion controller)
//   libs/sync/vehicle_link.h  LocalVehicleLink (Enter / Register / Exit and the server's answers, the
//                             seat-change revert)
//   libs/sync/vehicle_entry.h PickEntry (F / G: which seat of which car), DecideVehicleAim (a seated
//                             remote puppet's drive-by)
//   libs/sync/gtaids.h        Joaat, the explosive / thrown weapon exclusion, FE_MENU_VERSION_SP_PAUSE,
//                             WeaponLabel, weapon groups, melee class, muzzle effect names
//   libs/sync/pausemenu.h     PauseMenuGate (the no-pause pause menu)
//   sync plan W3 (combat client): WeaponShowCache, ShotDebt, HitReact::Decide, HitReactArbiter,
//                             MeleeSwingGate, SegmentTouchesCapsule, DeathDisplayAtMs, VitalsTracker's
//                             puppet-loss grace (all in combat.h)
//   sync plan W6:             vehicle_body.h (lights / doors / roof encode, decode diff), NotOwner in
//                             vehicle_link.h, combat.h's v5 inputs (Damage bone / flags, playerHealth,
//                             ClassifyDamage, KillFeedLine), pedstate.h (Tier B edges, vehicle moves)
//   live test 0.4.2 fixes W10: nametag.h (distance fade and scale, projection validity, the 5 Hz line-of-sight cache, the
//                             eased alpha, "[id] Name" with a safe cut, the nearest-N list)
//   live test 0.4.2 fixes W9:  pedstate.h EnterWarpDue (the sender's move), vehicle_w9.h (re-warp limiter, F / G press
//                             guard, passenger engine re-assert, bail-out decision, vehicle impact, bike roll log)
// Build target: gameplay_test (selftest/, never shipped). Simulated time, no randomness. Every
// check names what it proves; the run asserts the total number of checks so a skipped block fails.

#include "sync/combat.h"
#include "sync/emote.h"
#include "sync/gtaids.h"
#include "sync/nametag.h"
#include "sync/pausemenu.h"
#include "sync/pedstate.h"
#include "sync/vehicle_body.h"
#include "sync/vehicle_entry.h"
#include "sync/vehicle_link.h"
#include "sync/vehicle_w9.h"
#include "sync/vehicle_syncd.h"
#include "protocol/protocol.h"
#include "melee_sim.h"
#include "../apps/server/anticheat.h"

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using namespace gage::sync;
namespace proto = gage::proto;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got = "")
{
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", got.c_str());
}

#define R3T6_CLIENT
#include "r3_t6_tests.h"   // R3 T6 melee: the swing ladder against a puppet with switchable behaviours
#define R4T3_CLIENT
#include "r4_t3_tests.h"   // R4 T3 punches: the swing display's progress check, the survey and the ladder under switchable GTA models
#include "t6_health_tests.h"   // T6 health: the server -> wire -> client apply -> HUD chain on a simulated ped (needs Check above)
#include "r3_t1_health_tests.h"   // R3 T1 health: the measured GTA ceiling, DIRECT / SCALED, under 5 switchable GTA models
#include "map_clear_tests.h"   // 0.4.20 map clean-up: blip verdicts, sweep plan, schedule, tally (needs Check above)
#include "minimap_tests.h"     // 0.4.22 minimap: radar decision, bars gate, radar rectangle, vitals dock (needs Check above)

static std::string F(const char* fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static VitalsObs Obs(double now, int health, int armour = 0, bool dead = false, int maxHealth = 200)
{
    VitalsObs o;
    o.nowMs = now;
    o.health = health;
    o.armour = armour;
    o.dead = dead;
    o.maxHealth = maxHealth;
    return o;
}

static int SumHits(const VitalsOut& o)
{
    int s = 0;
    for (int i = 0; i < o.selfHitCount; ++i) s += o.selfHits[i];
    return s;
}

// ---- VitalsTracker ---------------------------------------------------------------------------
static void TestVitals()
{
    {
        VitalsTracker v;
        v.Reset();
        VitalsOut o = v.Tick(Obs(0, 180, 0));
        Check("vitals: the first tick puts the server's 100/0 on the ped (GTA 200, armour 0)",
              o.apply && o.setHealth == 200 && o.setArmour == 0 && o.selfHitCount == 0,
              F("apply %d health %d armour %d hits %d", o.apply, o.setHealth, o.setArmour, o.selfHitCount));
        o = v.Tick(Obs(16, 200, 0));
        VitalsOut o2 = v.Tick(Obs(32, 200, 0));
        Check("vitals: after the write only a baseline is taken, a steady ped sends nothing",
              !o.apply && !o.selfHitCount && !o2.apply && !o2.selfHitCount);
        o = v.Tick(Obs(48, 150, 0));
        Check("vitals: a 50-point fall is one self-hit of 50, pending 50",
              o.selfHitCount == 1 && o.selfHits[0] == 50 && v.Pending() == 50,
              F("hits %d first %d pending %d", o.selfHitCount, o.selfHits[0], v.Pending()));
        o = v.Tick(Obs(64, 150, 0));
        Check("vitals: the same lowered health next tick is not reported twice", o.selfHitCount == 0 && !o.apply);
        v.OnDamage(50, 0, 50, true, false);
        o = v.Tick(Obs(80, 150, 0));
        Check("vitals: the server's ack (Damage 50, health 50) clears pending and applies GTA 150",
              v.Pending() == 0 && o.apply && o.setHealth == 150, F("pending %d apply %d health %d", v.Pending(), o.apply, o.setHealth));
        v.Tick(Obs(96, 150, 0));
        o = v.Tick(Obs(112, 160, 0));
        Check("vitals: a gain nobody granted (GTA regeneration) is undone to the server's 150",
              o.apply && o.setHealth == 150 && o.selfHitCount == 0 && v.Stats().clampsDown == 1,
              F("apply %d health %d clamps %u", o.apply, o.setHealth, v.Stats().clampsDown));
    }
    {
        // Live 0.4.3: server playerHealth 200, the first write landed while the ped's max was still GTA's 200
        // (cut to GTA 200 = 100 of 200), then ApplyServerMaxHealth raised the max to 300 and the ped sat at
        // 200 / 300 all session (a half bar; it would die at half the server's health).
        VitalsTracker v;
        v.Reset(200, 0);
        VitalsOut o = v.Tick(Obs(0, 200, 0, false, 200));
        Check("vitals: under a GTA max of 200 the server's 200 is written as GTA 200 (all the ped can show)",
              o.apply && o.setHealth == 200, F("apply %d health %d", o.apply, o.setHealth));
        v.Tick(Obs(16, 200, 0, false, 200));
        o = v.Tick(Obs(32, 200, 0, false, 300));
        Check("vitals: when the max is raised to 300 the server's 200 is put on again (GTA 300), no self-hit",
              o.apply && o.setHealth == 300 && o.selfHitCount == 0 && v.Stats().maxChanges == 1,
              F("apply %d health %d hits %d maxChanges %u", o.apply, o.setHealth, o.selfHitCount, v.Stats().maxChanges));
        v.Tick(Obs(48, 300, 0, false, 300));
        o = v.Tick(Obs(64, 300, 0, false, 300));
        Check("vitals: once the ped matches, a steady max sends nothing more", !o.apply && o.selfHitCount == 0);
    }
    {
        // GTA clamps armour at the player's max (100): the server's 200 cannot show. A comparison
        // with the server's values would report a phantom loss of 100 every tick.
        VitalsTracker v;
        v.Reset(100, 200);
        VitalsOut o = v.Tick(Obs(0, 200, 0));
        bool phantom = false;
        for (int i = 1; i <= 60; ++i)
        {
            o = v.Tick(Obs(i * 16.0, 200, 100));
            if (o.selfHitCount || o.apply) phantom = true;
        }
        Check("vitals: armour clamped by GTA (server 200, ped 100) never becomes a phantom self-hit (60 ticks)",
              !phantom && v.Stats().selfHits == 0, F("selfHits %u", v.Stats().selfHits));
    }
    {
        VitalsTracker v;
        v.Reset(200, 100);                      // a 400-max ped: server health 200 + armour 100 = 300
        v.Tick(Obs(0, 300, 100, false, 400));
        v.Tick(Obs(16, 300, 100, false, 400));
        VitalsOut o = v.Tick(Obs(32, 0, 0, true, 400));
        Check("vitals: a lethal 300 splits at the wire cap: 250 + 50",
              o.selfHitCount == 2 && o.selfHits[0] == 250 && o.selfHits[1] == 50,
              F("hits %d [%d, %d]", o.selfHitCount, o.selfHits[0], o.selfHits[1]));
    }
    {
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        VitalsOut o = v.Tick(Obs(32, 0, 0, true));
        Check("vitals: dying locally sends the server's whole 100 as a self-hit", SumHits(o) == 100, F("sum %d", SumHits(o)));
        o = v.Tick(Obs(1000, 0, 0, true));
        Check("vitals: still dead before the ack timeout: nothing new", o.selfHitCount == 0);
        o = v.Tick(Obs(2100, 0, 0, true));
        Check("vitals: no Killed after 2 s: the lethal self-hit goes out again (resync)",
              SumHits(o) == 100 && v.Stats().resyncs == 1, F("sum %d resyncs %u", SumHits(o), v.Stats().resyncs));
        v.OnKilled();
        o = v.Tick(Obs(2200, 0, 0, true));
        Check("vitals: once the server has us dead a dead ped needs nothing", o.selfHitCount == 0 && !o.kill && !o.apply);
    }
    {
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        v.OnDamage(0, 0, 100, false, true);     // a remote player's hit killed us
        VitalsOut o = v.Tick(Obs(32, 200));
        Check("vitals: the server's kill on a living ped asks the engine to kill it (no self-hit)",
              o.kill && o.selfHitCount == 0 && v.ServerDead(), F("kill %d hits %d", o.kill, o.selfHitCount));
        v.OnRespawn(100, 0);
        o = v.Tick(Obs(5000, 200));
        Check("vitals: RespawnOk applies its values and the server is alive again",
              o.apply && o.setHealth == 200 && !v.ServerDead());
    }
    {
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        v.Tick(Obs(32, 170));                   // self-hit 30 in flight
        v.OnDamage(80, 0, 20, false, false);    // meanwhile a shooter's 20: the server has 100 - 20
        VitalsOut o = v.Tick(Obs(48, 170));
        Check("vitals: a shooter's Damage while our self-hit is in flight waits (no flicker apply)",
              !o.apply && o.selfHitCount == 0 && v.Pending() == 30);
        v.OnDamage(50, 0, 30, true, false);
        o = v.Tick(Obs(64, 170));
        Check("vitals: once the ack is in, the server's combined value is applied (100 - 20 - 30 = 50, GTA 150)",
              o.apply && o.setHealth == 150, F("apply %d health %d", o.apply, o.setHealth));
    }
    {
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        v.Tick(Obs(32, 180));                   // 20 pending, never answered (server rate-limited it)
        VitalsOut o = v.Tick(Obs(2100, 180));
        Check("vitals: an unanswered self-hit is dropped after 2 s and the server's value re-applied",
              o.apply && o.setHealth == 200 && v.Pending() == 0 && v.Stats().resyncs == 1);
    }
    // ---- review item 1: a loss a remote puppet caused is not ours to report -----------------------
    {
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        VitalsObs hit = Obs(32, 150);
        hit.puppetDamage = true;                // a remote player's car rammed us: the attacker's client sends the Hit
        VitalsOut o = v.Tick(hit);
        Check("item 1: a puppet-caused loss (ram) is re-baselined and NOT sent as a self-hit",
              o.selfHitCount == 0 && v.Pending() == 0 && v.Stats().selfHits == 0,
              F("hits %d pending %d selfHits %u", o.selfHitCount, v.Pending(), v.Stats().selfHits));
        o = v.Tick(Obs(48, 150));
        Check("item 1: the same lowered health on the next tick (flag gone) sends nothing either",
              o.selfHitCount == 0 && !o.apply, F("hits %d apply %d", o.selfHitCount, o.apply));
        v.OnDamage(50, 0, 50, false, false);    // the attacker's Hit, answered by the server
        o = v.Tick(Obs(64, 150));
        Check("item 1: the attacker's Damage then puts the server's value on the ped (GTA 150)",
              o.apply && o.setHealth == 150, F("apply %d health %d", o.apply, o.setHealth));
    }
    {
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        VitalsObs ram = Obs(32, 150);
        ram.puppetDamage = true;
        v.Tick(ram);
        const VitalsOut o = v.Tick(Obs(48, 120));   // a fall right after, nobody to blame
        Check("item 1: the flag is not sticky: a further loss without it is still a self-hit (30)",
              SumHits(o) == 30 && v.Pending() == 30, F("sum %d pending %d", SumHits(o), v.Pending()));
    }
    {
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        VitalsObs dead = Obs(32, 0, 0, true);
        dead.puppetDamage = true;               // rammed to death
        VitalsOut o = v.Tick(dead);
        int sent = SumHits(o);
        o = v.Tick(Obs(48, 0, 0, true));
        sent += SumHits(o);
        o = v.Tick(Obs(800, 0, 0, true));
        sent += SumHits(o);
        Check("item 1: a puppet-caused death sends no lethal self-hit inside the grace for the attacker's Hit",
              sent == 0, F("sent %d", sent));
        o = v.Tick(Obs(1700, 0, 0, true));
        Check("item 1: ... but still one after the grace if the server never killed us (never a stuck dead player)",
              SumHits(o) == 100, F("sum %d", SumHits(o)));
        VitalsTracker k;
        k.Reset();
        k.Tick(Obs(0, 200));
        k.Tick(Obs(16, 200));
        VitalsObs d2 = Obs(32, 0, 0, true);
        d2.puppetDamage = true;
        k.Tick(d2);
        k.OnDamage(0, 0, 100, false, true);     // the attacker's Hit killed us
        o = k.Tick(Obs(48, 0, 0, true));
        Check("item 1: the attacker's kill arrives inside the grace: no self-hit at all", SumHits(o) == 0 && k.ServerDead());
    }
    // ---- sync plan W3 (combat 2): a puppet-caused loss no Damage ever followed is put back -----------
    {
        // A cosmetic 1-HP flinch bullet (owner = the attacker's puppet) is a "puppet loss": the tracker
        // re-baselined it for good, so the ped stayed 1 HP below the server for the rest of the life and
        // a later fall could read as a death. After puppetGraceMs without a Damage the server's value is
        // put back.
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        VitalsObs bullet = Obs(32, 199);
        bullet.puppetDamage = true;
        VitalsOut o = v.Tick(bullet);
        Check("W3 grace: the cosmetic 1-HP puppet loss is not reported and nothing is applied at once",
              o.selfHitCount == 0 && !o.apply && v.Stats().selfHits == 0,
              F("hits %d apply %d", o.selfHitCount, o.apply));
        o = v.Tick(Obs(1000, 199));
        Check("W3 grace: still inside puppetGraceMs (1.5 s): left alone", !o.apply && o.selfHitCount == 0,
              F("apply %d hits %d", o.apply, o.selfHitCount));
        o = v.Tick(Obs(1650, 199));
        Check("W3 grace: 1.6 s after, no Damage came: the server's 100 is put back (apply, GTA 200)",
              o.apply && o.setHealth == 200 && o.selfHitCount == 0, F("apply %d health %d hits %d", o.apply, o.setHealth, o.selfHitCount));
        o = v.Tick(Obs(1666, 200));
        VitalsOut o2 = v.Tick(Obs(1700, 200));
        Check("W3 grace: it applies once, then a steady ped sends nothing (no repeat)",
              !o.apply && !o2.apply && o.selfHitCount == 0 && o2.selfHitCount == 0);
        // The attacker's Hit DID arrive inside the grace: its Damage applies at once and the schedule is spent.
        VitalsTracker w;
        w.Reset();
        w.Tick(Obs(0, 200));
        w.Tick(Obs(16, 200));
        VitalsObs ram = Obs(32, 150);
        ram.puppetDamage = true;
        w.Tick(ram);
        w.OnDamage(50, 0, 50, false, false);
        o = w.Tick(Obs(48, 150));
        Check("W3 grace: a Damage inside the window applies the server's value (GTA 150) at once",
              o.apply && o.setHealth == 150, F("apply %d health %d", o.apply, o.setHealth));
        o = w.Tick(Obs(64, 150));
        o2 = w.Tick(Obs(1800, 150));
        Check("W3 grace: ... and the earlier schedule does not apply again 1.5 s later",
              !o.apply && !o2.apply, F("apply %d / %d", o.apply, o2.apply));
        // The engine's own flinch bullet lands the frame after the tracker's apply write: that tick only takes
        // a baseline, so the 1-HP loss is never flagged as a puppet loss. NotePuppetLoss arms the put-back itself.
        VitalsTracker f;
        f.Reset();
        o = f.Tick(Obs(0, 150));                   // the first tick writes the server's 100 (GTA 200)
        f.NotePuppetLoss(10);                      // ... and the engine shot the cosmetic bullet this frame
        o = f.Tick(Obs(16, 199));                  // landed the next frame: absorbed as the baseline
        o2 = f.Tick(Obs(1000, 199));
        Check("W3 grace: a flinch bullet absorbed as the baseline is not a self-hit and is left alone for 1.5 s",
              o.selfHitCount == 0 && o2.selfHitCount == 0 && !o.apply && !o2.apply && f.Stats().selfHits == 0,
              F("hits %d/%d apply %d/%d", o.selfHitCount, o2.selfHitCount, o.apply, o2.apply));
        o = f.Tick(Obs(1600, 199));
        Check("W3 grace: ... then the server's value is put back (GTA 200), counted as a restore",
              o.apply && o.setHealth == 200 && f.Stats().puppetRestores == 1, F("apply %d health %d restores %u", o.apply, o.setHealth, f.Stats().puppetRestores));
    }
    {
        // FindPuppetDamager: peer peds and server vehicles are asked, our own vehicle never is.
        const int32_t handles[] = { 11, 0, 22, 33, 44 };
        int asked = 0;
        const int32_t hit = FindPuppetDamager(handles, 5, 33, [&](int32_t h) { ++asked; return h == 33 || h == 44; });
        Check("item 1: the scan skips our own vehicle (33) even though the game says it hurt us, finds 44",
              hit == 44 && asked == 3, F("hit %d asked %d", hit, asked));
        const int32_t none = FindPuppetDamager(handles, 5, 33, [](int32_t h) { return h == 33; });
        Check("item 1: only our own vehicle damaged us: no puppet", none == 0);
    }
    // ---- review item 3: the tracker keeps ticking while Dying until the server has us dead ----------
    {
        Check("item 3: the tracker is due while Alive, and while Dying with the server not yet dead",
              VitalsTickDue(true, true, false, false) && VitalsTickDue(true, false, true, false));
        Check("item 3: ... not once the server has us dead, not offline, not while respawning / loading",
              !VitalsTickDue(true, false, true, true) && !VitalsTickDue(false, true, false, false) &&
              !VitalsTickDue(true, false, false, false));
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        int total = 0;
        double t = 32;
        // the engine's loop: the ped is dead from t=32 (life Dying); the first lethal self-hit is lost
        for (; t < 2300; t += 16)
            if (VitalsTickDue(true, false, true, v.ServerDead()))
                total += SumHits(v.Tick(Obs(t, 0, 0, true)));
        Check("item 3: a lost lethal self-hit is resent after 2 s while Dying (100 + 100), not left for the 30 s give-up",
              total == 200 && v.Stats().resyncs == 1, F("total %d resyncs %u", total, v.Stats().resyncs));
        v.OnKilled();
        int after = 0;
        for (; t < 3000; t += 16)
            if (VitalsTickDue(true, false, true, v.ServerDead()))
                after += SumHits(v.Tick(Obs(t, 0, 0, true)));
        Check("item 3: once the server has us dead the tracker is not ticked on the dead path again", after == 0);
    }
}

// ---- RespawnGate -----------------------------------------------------------------------------
static void TestRespawn()
{
    RespawnGate g;
    Check("respawn: alive never sends", !g.ShouldSend(0));
    g.OnDeath(1000);
    int early = 0;
    for (double t = 1000; t < 6250; t += 16) early += g.ShouldSend(t);
    Check("respawn: nothing before the server's 5 s (+250 ms margin), 329 ticks", early == 0, F("%d", early));
    Check("respawn: the first request at 5.25 s", g.ShouldSend(6250) && g.Attempts() == 1);
    Check("respawn: then not again within a second", !g.ShouldSend(6800));
    Check("respawn: and again after a second", g.ShouldSend(7250) && g.Attempts() == 2);
    g.OnDeath(7000);
    Check("respawn: a second death report while dead keeps the clock", g.DeathMs() == 1000);
    Check("respawn: gives up after 30 s without RespawnOk", !g.GaveUp(30999) && g.GaveUp(31000));
    g.OnAlive();
    Check("respawn: alive again stops the requests", !g.Dead() && !g.ShouldSend(40000));
}

// ---- HitBatcher ------------------------------------------------------------------------------
static BatchedHit Hit(uint16_t target, uint16_t damage, uint8_t shot = 0)
{
    BatchedHit h;
    h.target = target;
    h.damage = damage;
    h.weapon = 0x1B06D571;
    h.shotSeq = shot;
    return h;
}

static void TestBatcher()
{
    BatchedHit out[128];
    {
        HitBatcher b;
        b.Add(Hit(7, 30, 1), 0);
        b.Add(Hit(7, 30, 2), 10);
        b.Add(Hit(7, 30, 3), 20);
        Check("batch: nothing leaves inside the 50 ms hold", b.Flush(40, out, 128) == 0);
        const size_t n = b.Flush(50, out, 128);
        Check("batch: three pellets on one target leave as one hit of 90, newest shotSeq",
              n == 1 && out[0].damage == 90 && out[0].shotSeq == 3 && out[0].target == 7,
              F("n %zu damage %u shot %u", n, n ? out[0].damage : 0, n ? out[0].shotSeq : 0));
    }
    {
        HitBatcher b;
        b.Add(Hit(3, 250), 0);
        b.Add(Hit(3, 150), 1);
        const size_t n = b.Flush(60, out, 128);
        Check("batch: 400 on one target splits at the cap: 250 then 150",
              n == 2 && out[0].damage == 250 && out[1].damage == 150 && b.Pending() == 0, F("n %zu", n));
    }
    {
        HitBatcher b;
        for (uint16_t i = 0; i < 40; ++i) b.Add(Hit(i, 10), 0);
        const size_t first = b.Flush(50, out, 128);
        Check("batch: 40 targets at once: the bucket lets 25 out", first == 25 && b.Pending() == 15, F("%zu", first));
        const size_t later = b.Flush(50 + 400, out, 128);
        Check("batch: 400 ms later 10 more tokens", later == 10 && b.Pending() == 5, F("%zu", later));
        int total = (int)first + (int)later;
        for (int t = 1; t <= 10; ++t) total += (int)b.Flush(450 + t * 100.0, out, 128);
        Check("batch: every hit leaves eventually (40)", total == 40 && b.Stats().sent == 40, F("%d", total));
    }
    {
        HitBatcher b;
        for (uint16_t i = 0; i < 70; ++i) b.Add(Hit(i, 5), 0);
        Check("batch: past 64 waiting targets the oldest are dropped", b.Pending() == 64 && b.Stats().dropped == 6);
        b.Add(Hit(1000, 0), 0);
        Check("batch: damage 0 is never queued (the session would refuse it)", b.Stats().added == 70);
    }
}

static void TestPeerDamage()
{
    PeerDamageOut o = ProbePeerDamage(1000, 1000, true);
    Check("peer: untouched pool: nothing", !o.report && !o.restore && o.damage == 0);
    o = ProbePeerDamage(974, 1000, true);
    Check("peer: our 26-point shot is reported and the pool restored", o.report && o.restore && o.damage == 26);
    o = ProbePeerDamage(974, 1000, false);
    Check("peer: damage we did not deal (a fall on our copy) is restored, never reported", !o.report && o.restore);
    o = ProbePeerDamage(1010, 1000, true);
    Check("peer: a gain is restored, no hit", !o.report && o.restore && o.damage == 0);
    o = ProbePeerDamage(0, 1000, true);
    Check("peer: an unreadable health is ignored", !o.report && !o.restore);

    // ---- review item 4: a headshot kills the puppet in one blow above the pool ---------------------
    o = ProbePeerDamage(0, 1000, true, true);
    Check("item 4: a puppet we killed in one blow is reported (the pool dealt) and respawned, not dropped",
          o.report && o.damage == 1000 && o.respawn && !o.restore,
          F("report %d damage %d respawn %d restore %d", o.report, o.damage, o.respawn, o.restore));
    o = ProbePeerDamage(0, 1000, false, true);
    Check("item 4: a puppet that died on our machine from something else is respawned, never reported",
          !o.report && o.respawn);
    o = ProbePeerDamage(0, 1000, true, false);
    Check("item 4: health 0 with the ped not dead is still an unreadable value (ignored)", !o.report && !o.respawn);
    o = ProbePeerDamage(974, 1000, true, false);
    Check("item 4: an ordinary hit does not respawn the puppet", o.report && !o.respawn);
    {
        BatchedHit out[16];
        HitBatcher b;
        o = ProbePeerDamage(0, 1000, true, true);
        b.Add(Hit(5, (uint16_t)o.damage), 0);
        const size_t n = b.Flush(60, out, 16);
        Check("item 4: the killing blow leaves as 4 hits of 250 (the wire cap), 1000 in all",
              n == 4 && out[0].damage == 250 && out[3].damage == 250 && b.Pending() == 0, F("n %zu", n));
    }

    // ---- review item 2: only the driver is blamed for the car's hits -------------------------------
    Check("item 2: the driver (seat 0, server-linked) is blamed for the car's hits", VehicleBlameAllowed(0, true));
    Check("item 2: a passenger is not (seat 1..7)", !VehicleBlameAllowed(1, true) && !VehicleBlameAllowed(3, true));
    Check("item 2: a driver the server has not linked (pending / refused) is not either",
          !VehicleBlameAllowed(0, false) && !VehicleBlameAllowed(-1, true));
}

// ---- EmoteArbiter ----------------------------------------------------------------------------
static EmoteIn EIn(double now, bool loaded = false, int playing = -1, float drift = 0, bool dead = false, bool veh = false)
{
    EmoteIn in;
    in.nowMs = now;
    in.dictLoaded = loaded;
    in.playing = playing;
    in.driftM = drift;
    in.dead = dead;
    in.inVehicle = veh;
    return in;
}

static void TestEmote()
{
    {
        EmoteArbiter a;
        a.StartAnim(0, 2000, 0);
        EmoteOut o = a.Tick(EIn(0));
        Check("emote: a new anim streams its dictionary and pauses the gait", o.requestDict && o.locoPaused && !o.issueAnim);
        o = a.Tick(EIn(100, true));
        Check("emote: dictionary in -> TASK_PLAY_ANIM once", o.issueAnim && a.Issued());
        o = a.Tick(EIn(1900, false, 1));
        Check("emote: playing: no re-issue, gait still paused", !o.issueAnim && o.locoPaused && !o.ended);
        o = a.Tick(EIn(2100, false, 1));
        Check("emote: a 2000 ms anim ends after its duration (the gait resumes)", o.ended && !o.clearTasks && !a.Active());
    }
    {
        EmoteArbiter a;
        a.StartAnim(0, -1, 1);
        int requests = 0;
        EmoteOut o;
        for (double t = 0; t < 3000; t += 16) { o = a.Tick(EIn(t)); requests += o.requestDict; }
        Check("emote: the dictionary is requested every tick while waiting (188 ticks)", requests == 188, F("%d", requests));
        o = a.Tick(EIn(3000));
        Check("emote: a dictionary that never loads is given up at 3 s", o.gaveUp && o.ended && !a.Active());
    }
    {
        EmoteArbiter a;
        a.StartAnim(0, -1, 1);
        a.Tick(EIn(0, true));
        EmoteOut o = a.Tick(EIn(300, false, 0));
        Check("emote: not playing inside the start grace is tolerated", !o.ended);
        a.Tick(EIn(700, false, 0));
        o = a.Tick(EIn(1150, false, 0));
        Check("emote: a loop not playing for 400 ms after the grace ends (bad clip name)", o.ended && a.Stats().notPlaying == 1);
    }
    {
        EmoteArbiter a;
        a.StartAnim(0, -1, 1);
        a.Tick(EIn(0, true));
        a.Stop();
        EmoteOut o = a.Tick(EIn(500, false, 1));
        Check("emote: Stop clears the task and hands the ped back", o.clearTasks && o.ended);
        o = a.Tick(EIn(600));
        Check("emote: and stays quiet after", !o.clearTasks && !o.ended && !o.locoPaused);
    }
    {
        EmoteArbiter a;
        a.StartAnim(0, -1, 48);
        EmoteOut o = a.Tick(EIn(0, true));
        Check("emote: an upper-body anim (flags 16|32) keeps the gait running", o.issueAnim && !o.locoPaused && !o.place);
    }
    {
        EmoteArbiter a;
        a.StartAnim(0, -1, 1);
        a.Tick(EIn(0, true));
        EmoteOut o = a.Tick(EIn(100, false, 1, 3.0f));
        Check("emote: a paused ped 3 m off its pose is placed onto it", o.locoPaused && o.place);
        o = a.Tick(EIn(200, false, 1, 1.0f));
        Check("emote: 1 m off is left alone", o.locoPaused && !o.place);
        o = a.Tick(EIn(300, false, 1, 0, true));
        Check("emote: death ends it without a clear (the game replaced the task)", o.ended && !o.clearTasks);
    }
    {
        EmoteArbiter a;
        a.StartScenario(0);
        EmoteOut o = a.Tick(EIn(0));
        Check("emote: a scenario is issued at once (no dictionary)", o.issueScenario && o.locoPaused && !o.requestDict);
        o = a.Tick(EIn(2500, false, 0));
        Check("emote: not using the scenario during its 3 s enter grace is fine", !o.ended);
        a.Tick(EIn(3100, false, 0));
        o = a.Tick(EIn(3600, false, 0));
        Check("emote: after the grace, not using it for 400 ms ends it", o.ended);
    }
    {
        EmoteArbiter a;
        a.StartAnim(0, -1, 1);
        a.Tick(EIn(0, true));
        a.OnNewPed(500);
        Check("emote: a new ped re-streams and re-issues a loop", a.WaitingForDict());
        EmoteOut o = a.Tick(EIn(516, true));
        Check("emote: ... and issues it again", o.issueAnim);
        EmoteArbiter b;
        b.StartAnim(0, 0, 0);
        b.Tick(EIn(0, true));
        b.OnNewPed(500);
        Check("emote: a one-shot is dropped on a new ped", !b.Active());
    }
    {
        EmoteArbiter a;
        a.StartAnim(0, 0, 0);
        a.Tick(EIn(0, true));
        EmoteOut o = a.Tick(EIn(29000, false, -1));
        Check("emote: a one-shot whose state cannot be read runs to the 30 s cap", !o.ended);
        o = a.Tick(EIn(30000, false, -1));
        Check("emote: ... and ends there", o.ended);
    }
    Check("emote: wire -> GTA: -1 loops (flag 1), 0 plays once at full length, 1500 stays",
          EmoteArbiter::GtaDuration(-1) == -1 && (EmoteArbiter::GtaFlags(-1, 16) & 1u) &&
          EmoteArbiter::GtaDuration(0) == -1 && EmoteArbiter::GtaFlags(0, 16) == 16 &&
          EmoteArbiter::GtaDuration(1500) == 1500);
}

// ---- LocalVehicleLink ------------------------------------------------------------------------
static VehLinkIn VIn(double now, int32_t veh, int seat = 0, int netId = -1, bool dead = false)
{
    VehLinkIn in;
    in.nowMs = now;
    in.veh = veh;
    in.seat = seat;
    in.netId = netId;
    in.dead = dead;
    return in;
}

static void TestLink()
{
    {
        LocalVehicleLink l;
        VehLinkOut o = l.Tick(VIn(0, 0));
        Check("link: on foot sends nothing", o.cmd == VehLinkCmd::None);
        o = l.Tick(VIn(16, 501, 0, -1));
        Check("link: into an ambient car (driver seat) -> Register seat 0", o.cmd == VehLinkCmd::Register && o.seat == 0 && o.veh == 501);
        o = l.Tick(VIn(32, 501, 0, -1));
        Check("link: no repeat while the answer is out", o.cmd == VehLinkCmd::None && !l.Linked());
        l.OnResult(8, 0, 5);
        o = l.Tick(VIn(48, 501, 0, -1));
        Check("link: Register Ok binds the car to id 5 and makes us the driver",
              o.bindVeh == 501 && o.bindNetId == 5 && l.Driver() && l.NetId() == 5);
        o = l.Tick(VIn(64, 0));
        Check("link: getting out sends Exit(5)", o.cmd == VehLinkCmd::Exit && o.netId == 5 && !l.Linked());
    }
    {
        LocalVehicleLink l;
        VehLinkOut o = l.Tick(VIn(0, 777, 2, 9));
        Check("link: into a server car (id 9) as a passenger -> Enter(9, seat 2)", o.cmd == VehLinkCmd::Enter && o.netId == 9 && o.seat == 2);
        l.OnResult(3, 2, 9);  // SeatTaken
        o = l.Tick(VIn(16, 777, 2, 9));
        Check("link: Enter refused (seat taken) -> eject, no retry", o.eject && o.cmd == VehLinkCmd::None && !l.Linked());
        o = l.Tick(VIn(3000, 777, 2, 9));
        Check("link: ... still no retry seconds later", o.cmd == VehLinkCmd::None);
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 501, 0, -1));
        l.OnResult(8, 7, 0);  // RateLimited
        VehLinkOut o = l.Tick(VIn(16, 501, 0, -1));
        Check("link: Register rate-limited is not an eject", !o.eject && o.cmd == VehLinkCmd::None);
        o = l.Tick(VIn(1000, 501, 0, -1));
        Check("link: ... no retry before 1.5 s", o.cmd == VehLinkCmd::None);
        o = l.Tick(VIn(1600, 501, 0, -1));
        Check("link: ... Register again after 1.5 s", o.cmd == VehLinkCmd::Register);
        l.OnResult(8, 7, 0);
        l.Tick(VIn(1616, 501, 0, -1));
        o = l.Tick(VIn(3200, 501, 0, -1));
        Check("link: third and last try", o.cmd == VehLinkCmd::Register);
        l.OnResult(8, 7, 0);
        l.Tick(VIn(3216, 501, 0, -1));
        o = l.Tick(VIn(9000, 501, 0, -1));
        Check("link: three tries is the cap", o.cmd == VehLinkCmd::None && l.Stats().registers == 3);
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 501, 0, -1));
        l.OnResult(8, 6, 0);  // Limit
        l.Tick(VIn(16, 501, 0, -1));
        VehLinkOut o = l.Tick(VIn(5000, 501, 0, -1));
        Check("link: Register at the server's cap (Limit) is not retried", o.cmd == VehLinkCmd::None && !o.eject);
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 501, 0, -1));
        VehLinkOut o = l.Tick(VIn(100, 0));
        Check("link: out of the car before the answer: wait, no Exit yet", o.cmd == VehLinkCmd::None);
        l.OnResult(8, 0, 12);
        o = l.Tick(VIn(200, 0));
        Check("link: ... the Ok arrives: bind and Exit(12) at once",
              o.bindNetId == 12 && o.cmd == VehLinkCmd::Exit && o.netId == 12);
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 777, 0, 9));
        l.OnResult(3, 0, 9);
        l.Tick(VIn(16, 777, 0, 9));
        VehLinkOut o = l.Tick(VIn(32, 777, 1, 9));
        Check("link: a seat change inside the car -> Enter(9, seat 1), no longer the driver",
              o.cmd == VehLinkCmd::Enter && o.netId == 9 && o.seat == 1 && !l.Driver());
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 777, 0, 9));
        VehLinkOut o = l.Tick(VIn(5100, 777, 0, 9));
        Check("link: no answer in 5 s -> asked again", o.cmd == VehLinkCmd::Enter && l.Stats().timeouts == 1);
    }
    {
        LocalVehicleLink l;
        VehLinkOut o = l.Tick(VIn(0, 888, -1, -1));
        Check("link: a seat the wire cannot carry sends nothing", o.cmd == VehLinkCmd::None);
        LocalVehicleLink d;
        o = d.Tick(VIn(0, 888, 0, -1, true));
        Check("link: dead in a car sends nothing (the server refuses the dead)", o.cmd == VehLinkCmd::None);
        o = d.Tick(VIn(1600, 888, 0, -1, false));
        Check("link: ... alive again: Register", o.cmd == VehLinkCmd::Register);
        d.OnResult(4, 0, 3);
        o = d.Tick(VIn(1616, 888, 0, -1));
        Check("link: an answer for another op is stale, ignored", o.resultOp == 0 && d.Stats().stale == 1 && !d.Linked());
    }

    // ---- review item 5: the server must not keep us seated in a car we left ---------------------
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 777, 0, 9));
        l.OnResult(3, 0, 9);
        l.Tick(VIn(16, 777, 0, 9));             // linked in server vehicle 9
        VehLinkOut o = l.Tick(VIn(32, 888, 0, -1));   // /car: SetPedIntoVehicle into a local car, no frame on foot
        Check("item 5: the vehicle changes while linked: Exit(9) goes out first",
              o.cmd == VehLinkCmd::Exit && o.netId == 9 && o.veh == 777,
              F("cmd %d netId %d", (int)o.cmd, o.netId));
        o = l.Tick(VIn(48, 888, 0, -1));
        Check("item 5: ... and the new car is registered on the next tick",
              o.cmd == VehLinkCmd::Register && o.veh == 888, F("cmd %d veh %d", (int)o.cmd, o.veh));
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 777, 0, 9));              // Enter(9) out
        l.OnResult(3, 0, 4);                    // an old Ok: Enter(4), a car we left before
        VehLinkOut o = l.Tick(VIn(16, 777, 0, 9));
        Check("item 5: an Ok for another vehicle id does not link us to this one", !l.Linked(),
              F("state %s", l.StateName()));
        Check("item 5: ... it is answered with Exit(4), the car we are not in",
              o.cmd == VehLinkCmd::Exit && o.netId == 4, F("cmd %d netId %d", (int)o.cmd, o.netId));
        l.OnResult(3, 0, 9);
        l.Tick(VIn(32, 777, 0, 9));
        Check("item 5: the Ok for id 9 then links", l.Linked() && l.NetId() == 9);
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 0));
        l.OnResult(3, 0, 12);                   // a late Ok: the server seated us in 12 after we had left
        VehLinkOut o = l.Tick(VIn(16, 0));
        Check("item 5: a stale Ok while on foot is answered with Exit(12)",
              o.cmd == VehLinkCmd::Exit && o.netId == 12, F("cmd %d netId %d", (int)o.cmd, o.netId));
        l.OnResult(8, 0, 13);                   // the same for a Register we gave up on
        o = l.Tick(VIn(32, 0));
        Check("item 5: ... and a stale Register Ok is answered with Exit(13)",
              o.cmd == VehLinkCmd::Exit && o.netId == 13, F("cmd %d netId %d", (int)o.cmd, o.netId));
        l.OnResult(4, 0, 12);
        o = l.Tick(VIn(48, 0));
        Check("item 5: an Exit's own answer is never answered", o.cmd == VehLinkCmd::None);
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 501, 0, -1));             // Register(501) out
        l.Tick(VIn(16, 502, 0, -1));            // into another ambient car before the answer
        VehLinkOut o = l.Tick(VIn(32, 502, 0, -1));
        l.OnResult(8, 0, 7);                    // the answer for 501 (the older command)
        o = l.Tick(VIn(48, 502, 0, -1));
        Check("item 5: the older Register's Ok binds the car it was for (501 -> 7) and Exit(7) follows",
              o.bindVeh == 501 && o.bindNetId == 7 && o.cmd == VehLinkCmd::Exit && o.netId == 7,
              F("bind %d->%d cmd %d netId %d", o.bindVeh, o.bindNetId, (int)o.cmd, o.netId));
        Check("item 5: ... and it does not link us to the newer car's pending Register", !l.Linked());
    }
    {
        // A Register whose position or rotation could not be read is not sent: the link goes back to
        // retrying instead of waiting 5 s for an answer that cannot come (review item 13).
        LocalVehicleLink l;
        VehLinkOut o = l.Tick(VIn(0, 501, 0, -1));
        l.OnNotSent(0);
        o = l.Tick(VIn(16, 501, 0, -1));
        Check("item 13: a command the engine could not send is not waited for", o.cmd == VehLinkCmd::None && !l.Linked());
        o = l.Tick(VIn(1600, 501, 0, -1));
        Check("item 13: ... and is tried again after the retry delay", o.cmd == VehLinkCmd::Register,
              F("cmd %d", (int)o.cmd));
    }

    // ---- sync plan W2 A2: a refused seat change is reverted, never an eject --------------------
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 777, 1, 9));              // Enter(9, seat 1)
        l.OnResult(3, 0, 9);
        l.Tick(VIn(16, 777, 1, 9));             // linked in seat 1
        VehLinkOut o = l.Tick(VIn(32, 777, 0, 9));   // GTA shuffled us to the driver seat: Enter(9, 0) goes out
        const bool sent = o.cmd == VehLinkCmd::Enter && o.seat == 0;
        l.OnResult(3, 2, 9);                    // SeatTaken: a player is driving
        o = l.Tick(VIn(48, 777, 0, 9));
        Check("W2 A2: a refused seat change inside the car reverts to the old seat: no eject, stays linked",
              sent && !o.eject && o.revertSeat == 1 && l.Linked() && l.Seat() == 1 && o.cmd == VehLinkCmd::None,
              F("sent %d eject %d revert %d linked %d seat %d cmd %d", (int)sent, (int)o.eject, o.revertSeat,
                (int)l.Linked(), l.Seat(), (int)o.cmd));
        o = l.Tick(VIn(64, 777, 0, 9));         // the warp back has not happened yet: the game still reads seat 0
        Check("W2 A2: ... while the game still reads the refused seat nothing is re-sent (hold)",
              o.cmd == VehLinkCmd::None && o.revertSeat == -1 && l.Linked() && l.Seat() == 1,
              F("cmd %d revert %d seat %d", (int)o.cmd, o.revertSeat, l.Seat()));
        o = l.Tick(VIn(80, 777, 1, 9));         // the engine warped us back: the game reads seat 1 again
        Check("W2 A2: ... the warp-back took: quiet, linked in seat 1", o.cmd == VehLinkCmd::None && l.Linked() && l.Seat() == 1);
        o = l.Tick(VIn(3000, 777, 0, 9));       // a real seat change long after the hold
        Check("W2 A2: ... a new seat change after the hold is asked for again",
              o.cmd == VehLinkCmd::Enter && o.seat == 0, F("cmd %d seat %d", (int)o.cmd, o.seat));
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 777, 0, 9));
        l.OnResult(3, 0, 9);
        l.Tick(VIn(16, 777, 0, 9));
        VehLinkOut o = l.Tick(VIn(32, 777, 1, 9));   // seat change 0 -> 1
        l.OnResult(3, 0, 9);
        o = l.Tick(VIn(48, 777, 1, 9));
        Check("W2 A2: a seat change the server grants links the new seat, no revert",
              o.revertSeat == -1 && !o.eject && l.Linked() && l.Seat() == 1 && !l.Driver());
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 777, 0, 9));
        l.OnResult(3, 0, 9);
        l.Tick(VIn(16, 777, 0, 9));             // we drive
        l.Tick(VIn(32, 777, 1, 9));             // a shuffle to the passenger seat, refused
        l.OnResult(3, 2, 9);
        const VehLinkOut o = l.Tick(VIn(48, 777, 1, 9));
        Check("W2 A2: a refused shuffle away from the driver seat reverts to the driver seat (Driver() again)",
              o.revertSeat == 0 && !o.eject && l.Driver() && l.Linked(),
              F("revert %d driver %d", o.revertSeat, (int)l.Driver()));
    }
    {
        LocalVehicleLink l;
        l.Tick(VIn(0, 777, 1, 9));
        l.OnResult(3, 0, 9);
        l.Tick(VIn(16, 777, 1, 9));
        l.Tick(VIn(32, 777, 0, 9));             // seat change, rate limited
        l.OnResult(3, 7, 9);
        l.Tick(VIn(48, 777, 0, 9));
        VehLinkOut o = l.Tick(VIn(1600, 777, 0, 9));   // retried after 1.5 s
        const bool again = o.cmd == VehLinkCmd::Enter && o.seat == 0;
        l.OnResult(3, 2, 9);                    // and now refused for the seat itself
        o = l.Tick(VIn(1616, 777, 0, 9));
        Check("W2 A2: a seat change that was rate limited first and refused after the retry still reverts",
              again && !o.eject && o.revertSeat == 1 && l.Linked() && l.Seat() == 1,
              F("again %d eject %d revert %d linked %d", (int)again, (int)o.eject, o.revertSeat, (int)l.Linked()));
    }
    {
        // First entry stays an eject (the car is not ours to sit in); every ejecting code has its own line.
        bool distinct = true, nonEmpty = true;
        int ejecting = 0;
        for (int a = 0; a < 16; ++a)
        {
            if (!LocalVehicleLink::Ejects((uint8_t)a)) continue;
            ++ejecting;
            const char* ta = LocalVehicleLink::RefusalText((uint8_t)a);
            nonEmpty = nonEmpty && ta && *ta;
            for (int b = a + 1; b < 16; ++b)
                if (LocalVehicleLink::Ejects((uint8_t)b) && ta && std::string(ta) == LocalVehicleLink::RefusalText((uint8_t)b))
                    distinct = false;
        }
        // W6: NotOwner (12) joined the six, so 7 codes.
        Check("W2 A2: every refusal that ejects has its own non-empty chat line (7 codes)",
              ejecting == 7 && nonEmpty && distinct, F("%d ejecting codes", ejecting));
    }
}

// ---- F / G vehicle entry (libs/sync/vehicle_entry.h) -----------------------------------------
// A car at the origin facing +y; wire seat s sits at x = -1 (driver side, even s) or +1 (passenger
// side, odd s), y = 0.5 - 1.0 * (s / 2): the "door" of a seat is where one stands to use it.
static EntryCandidate Car(int netId, int maxSeats, float ox = 0, float oy = 0)
{
    EntryCandidate c;
    c.netId = netId;
    c.maxSeats = maxSeats;
    for (int s = 0; s < kEntrySeats; ++s)
    {
        c.seats[s] = kEntryNobody;
        c.door[s] = { ox + ((s & 1) ? 1.0f : -1.0f), oy + 0.5f - 1.0f * (float)(s / 2), 0.0f };
    }
    return c;
}

static void TestEntry()
{
    const uint16_t me = 5, other = 9;
    const EntryVec atPassengerFront{ 1.6f, 0.4f, 0 };    // beside seat 1's door
    const EntryVec atRearDriverSide{ -1.6f, -0.5f, 0 };  // beside seat 2's door
    {
        EntryCandidate c = Car(3, 6);
        c.seats[0] = other;                     // a player drives
        EntryPick p = PickEntry(atRearDriverSide, &c, 1, me, true);
        Check("entry: F beside an occupied car seats the nearest FREE passenger seat, never the driver's",
              p.reason == EntryReason::Ok && p.netId == 3 && p.seat == 2, F("reason %d seat %d", (int)p.reason, p.seat));
        c.seats[2] = 11;                        // the nearest passenger seat is taken too
        p = PickEntry(atRearDriverSide, &c, 1, me, true);
        Check("entry: ... a taken seat is skipped even when its door is nearer (next nearest by door distance)",
              p.reason == EntryReason::Ok && p.seat == 4, F("reason %d seat %d", (int)p.reason, p.seat));
    }
    {
        EntryCandidate c = Car(3, 4);
        EntryPick f = PickEntry(atPassengerFront, &c, 1, me, true);
        EntryPick g = PickEntry(atPassengerFront, &c, 1, me, false);
        Check("entry: F at an empty car takes the driver's seat even when a passenger door is nearer",
              f.reason == EntryReason::Ok && f.seat == 0, F("reason %d seat %d", (int)f.reason, f.seat));
        Check("entry: G at the same empty car takes the nearest passenger seat (never the driver's)",
              g.reason == EntryReason::Ok && g.seat == 1, F("reason %d seat %d", (int)g.reason, g.seat));
        const EntryPick g2 = PickEntry(EntryVec{ -1.2f, 0.5f, 0 }, &c, 1, me, false);
        Check("entry: G beside the driver's door still goes to a passenger seat (seat 0 is F's)",
              g2.reason == EntryReason::Ok && g2.seat >= 1, F("seat %d", g2.seat));
    }
    {
        EntryCandidate c = Car(3, 4);
        c.ownerId = other;                      // someone else's car, empty
        const EntryPick f = PickEntry(atPassengerFront, &c, 1, me, true);
        const EntryPick g = PickEntry(EntryVec{ -1.2f, 0.5f, 0 }, &c, 1, me, false);
        const EntryPick f2 = PickEntry(EntryVec{ -1.2f, 0.5f, 0 }, &c, 1, me, true);
        Check("entry: an OWNED car (owner not me): F picks a passenger seat, never the driver's",
              f.reason == EntryReason::Ok && f.seat >= 1 && f2.reason == EntryReason::Ok && f2.seat >= 1,
              F("F %d/%d, F at the driver door %d/%d", (int)f.reason, f.seat, (int)f2.reason, f2.seat));
        Check("entry: ... and so does G", g.reason == EntryReason::Ok && g.seat >= 1, F("seat %d", g.seat));
        c.ownerId = me;
        const EntryPick own = PickEntry(atPassengerFront, &c, 1, me, true);
        Check("entry: the owner's F on its own empty car takes the driver's seat",
              own.reason == EntryReason::Ok && own.seat == 0, F("seat %d", own.seat));
    }
    {
        EntryCandidate c = Car(3, 4);
        c.locked = true;
        const EntryPick f = PickEntry(atPassengerFront, &c, 1, me, true);
        const EntryPick g = PickEntry(atPassengerFront, &c, 1, me, false);
        Check("entry: a locked car refuses F and G for a non-owner (Locked), no seat",
              f.reason == EntryReason::Locked && f.seat == -1 && g.reason == EntryReason::Locked && g.seat == -1,
              F("F %d G %d", (int)f.reason, (int)g.reason));
        c.ownerId = me;
        const EntryPick own = PickEntry(atPassengerFront, &c, 1, me, true);
        Check("entry: ... the owner may enter its own locked car (the server lets the spawner in)",
              own.reason == EntryReason::Ok && own.seat == 0);
    }
    {
        EntryCandidate c = Car(3, 4);
        for (int s = 0; s < 4; ++s) c.seats[s] = (uint16_t)(20 + s);
        const EntryPick f = PickEntry(atPassengerFront, &c, 1, me, true);
        const EntryPick g = PickEntry(atPassengerFront, &c, 1, me, false);
        Check("entry: a full car refuses F and G (Full)", f.reason == EntryReason::Full && g.reason == EntryReason::Full &&
              f.seat == -1 && g.seat == -1, F("F %d G %d", (int)f.reason, (int)g.reason));
        EntryCandidate bike = Car(4, 1);
        bike.seats[0] = other;
        EntryCandidate owned = Car(5, 4);
        owned.ownerId = other;
        for (int s = 1; s < 4; ++s) owned.seats[s] = (uint16_t)(20 + s);   // only the driver's seat is free
        const EntryPick b = PickEntry(atPassengerFront, &bike, 1, me, true);
        const EntryPick o = PickEntry(atPassengerFront, &owned, 1, me, true);
        Check("entry: a one-seat vehicle with its driver is Full; an owned car with only the driver's seat free is Owned",
              b.reason == EntryReason::Full && o.reason == EntryReason::Owned && o.seat == -1,
              F("bike %d owned %d", (int)b.reason, (int)o.reason));
    }
    {
        EntryCandidate c = Car(3, 2);           // two seats: seat 2's door being nearest must not matter
        c.seats[0] = other;
        const EntryPick p = PickEntry(atRearDriverSide, &c, 1, me, true);
        Check("entry: seats at or above maxSeats do not exist (a 2-seater with a driver offers seat 1 only)",
              p.reason == EntryReason::Ok && p.seat == 1, F("reason %d seat %d", (int)p.reason, p.seat));
    }
    {
        EntryCandidate cars[2] = { Car(7, 4, 0, 0), Car(8, 4, 6, 0) };
        EntryPick p = PickEntry(EntryVec{ 1.5f, 0.5f, 0 }, cars, 2, me, true);   // 0.5 m from car 7's door, 3.5 m from car 8's
        Check("entry: with two cars in reach the nearer one is used", p.reason == EntryReason::Ok && p.netId == 7,
              F("netId %d", p.netId));
        for (int s = 0; s < 4; ++s) cars[0].seats[s] = (uint16_t)(20 + s);
        p = PickEntry(EntryVec{ 1.5f, 0.5f, 0 }, cars, 2, me, true);
        Check("entry: ... a full nearer car is skipped for a farther car with a free seat",
              p.reason == EntryReason::Ok && p.netId == 8 && p.seat == 0, F("netId %d seat %d", p.netId, p.seat));
        const EntryPick none = PickEntry(atPassengerFront, nullptr, 0, me, true);
        Check("entry: no car in reach is NoCar", none.reason == EntryReason::NoCar && none.netId == -1 && none.seat == -1);
    }
    {
        // Exhaustive: every occupancy mask x owner (nobody / other / me) x locked x F or G.
        int cases = 0, bad = 0;
        const uint16_t owners[3] = { kEntryNobody, other, me };
        for (int mask = 0; mask < 256; ++mask)
            for (int oi = 0; oi < 3; ++oi)
                for (int lk = 0; lk < 2; ++lk)
                    for (int wd = 0; wd < 2; ++wd)
                    {
                        EntryCandidate c = Car(1, 8);
                        for (int s = 0; s < 8; ++s)
                            c.seats[s] = (mask >> s) & 1 ? (uint16_t)(30 + s) : kEntryNobody;
                        c.ownerId = owners[oi];
                        c.locked = lk != 0;
                        const EntryPick p = PickEntry(atPassengerFront, &c, 1, me, wd != 0);
                        ++cases;
                        // The rule restated independently of the implementation.
                        const bool ownedByOther = owners[oi] != kEntryNobody && owners[oi] != me;
                        const bool lockedOut = lk && owners[oi] != me;
                        const bool driverFree = !((mask >> 0) & 1);
                        bool passengerFree = false;
                        for (int s = 1; s < 8; ++s)
                            passengerFree = passengerFree || !((mask >> s) & 1);
                        const bool wantsDriverSeat = wd && driverFree && !ownedByOther;
                        const bool feasible = !lockedOut && (wantsDriverSeat || passengerFree);
                        if (feasible != (p.reason == EntryReason::Ok)) ++bad;
                        else if (feasible)
                        {
                            if (p.seat < 0 || p.seat > 7 || ((mask >> p.seat) & 1)) ++bad;          // a taken seat: a jack
                            else if (wantsDriverSeat != (p.seat == 0)) ++bad;                        // F gets seat 0 when it may, G and the rest never
                        }
                        else if (p.seat != -1) ++bad;
                    }
        Check("entry: over all 3072 occupancy / owner / lock / F-G cases a pick is exactly the rule: a free seat, "
              "the driver's only for F on a car not owned by someone else, nothing on a locked car of someone else",
              cases == 3072 && bad == 0, F("%d cases, %d wrong", cases, bad));
    }
    {
        const char* a = EntryRefusalText(EntryReason::Full);
        const char* b = EntryRefusalText(EntryReason::Locked);
        const char* c = EntryRefusalText(EntryReason::Owned);
        Check("entry: Full / Locked / Owned each have their own chat line",
              a && b && c && *a && *b && *c && std::string(a) != b && std::string(b) != c && std::string(a) != c);
    }
}

// ---- seated puppet drive-by (DecideVehicleAim) -----------------------------------------------
static void TestVehicleAim()
{
    const uint16_t aiming = gage::proto::kPfAiming;
    Check("drive-by: seated + aiming + a gun, no task yet -> Issue",
          DecideVehicleAim(true, aiming, true, false, 0) == VehAim::Issue);
    Check("drive-by: ... a running task is left alone inside the 500 ms window",
          DecideVehicleAim(true, aiming, true, true, 100) == VehAim::None &&
          DecideVehicleAim(true, aiming, true, true, 499) == VehAim::None);
    Check("drive-by: ... and re-issued once 500 ms have passed",
          DecideVehicleAim(true, aiming, true, true, 500) == VehAim::Issue &&
          DecideVehicleAim(true, aiming, true, true, 4000) == VehAim::Issue);
    Check("drive-by: stopped aiming with the task running -> Clear; not aiming and no task -> None",
          DecideVehicleAim(true, 0, true, true, 100) == VehAim::Clear &&
          DecideVehicleAim(true, 0, true, false, 100) == VehAim::None);
    Check("drive-by: unarmed (or not a gun) -> None, and Clear when a task is still running",
          DecideVehicleAim(true, aiming, false, false, 0) == VehAim::None &&
          DecideVehicleAim(true, aiming, false, true, 0) == VehAim::Clear);
    Check("drive-by: not seated -> None (the on-foot path owns the ped), even aiming with a gun",
          DecideVehicleAim(false, aiming, true, false, 0) == VehAim::None &&
          DecideVehicleAim(false, aiming, true, true, 900) == VehAim::None);
    Check("drive-by: a ragdolling ped is not aiming (Clear when a task runs, else None)",
          DecideVehicleAim(true, (uint16_t)(aiming | gage::proto::kPfRagdoll), true, true, 900) == VehAim::Clear &&
          DecideVehicleAim(true, (uint16_t)(aiming | gage::proto::kPfRagdoll), true, false, 900) == VehAim::None);
}

// ---- the remaining review items: dictionary refs, unreadable reads, give-up, queues, ids, pause menu ----
static EmoteIn DIn(double now, bool loaded = false)
{
    EmoteIn i;
    i.nowMs = now;
    i.dictLoaded = loaded;
    return i;
}

static void TestMore()
{
    // ---- review item 6: animation dictionaries are reference-counted across arbiters ---------------
    {
        const std::string dict = "amb@world_human_hang_out_street@male_a@base";
        std::string dictUpper = dict;
        for (char& c : dictUpper) c = (char)std::toupper((unsigned char)c);
        AnimDictRefs refs;
        EmoteDictHold a, b;
        std::string r = a.Sync(refs, dict);
        Check("item 6: the first emote acquires the dictionary, nothing to remove", r.empty() && refs.Count(dict) == 1);
        r = b.Sync(refs, dictUpper);
        Check("item 6: a second emote on the same dictionary (any letter case) shares it", r.empty() && refs.Count(dict) == 2);
        r = a.Sync(refs, "");
        Check("item 6: the first ends: the dictionary stays loaded for the second", r.empty() && refs.Count(dict) == 1);
        r = b.Sync(refs, "");
        Check("item 6: the last one ends: RemoveAnimDict is asked for exactly that dictionary", r == dictUpper || r == dict,
              r);
        r = b.Sync(refs, "");
        Check("item 6: ending twice removes nothing more", r.empty() && refs.Count(dict) == 0);

        EmoteDictHold c;
        c.Sync(refs, "dict_one");
        r = c.Sync(refs, "dict_two");
        Check("item 6: an emote switching dictionaries frees the old one only", r == "dict_one" && refs.Count("dict_two") == 1);
        r = c.Sync(refs, "dict_two");
        Check("item 6: re-syncing the same dictionary changes nothing", r.empty() && refs.Count("dict_two") == 1);

        // The engine's rule: the dictionary is wanted exactly while the arbiter is an active animation.
        // A dictionary that never loads is given up at 3 s and must be freed then.
        EmoteArbiter arb;
        EmoteDictHold h;
        AnimDictRefs refs2;
        arb.StartAnim(0, -1, 1);
        h.Sync(refs2, arb.Active() && arb.Kind() == EmoteKind::Anim ? "dict_x" : "");
        EmoteOut o;
        for (double t = 0; t < 3000; t += 16) arb.Tick(DIn(t));
        o = arb.Tick(DIn(3000));
        r = h.Sync(refs2, arb.Active() && arb.Kind() == EmoteKind::Anim ? "dict_x" : "");
        Check("item 6: given up after 3 s: the dictionary is freed", o.gaveUp && r == "dict_x" && refs2.Count("dict_x") == 0);
        // Its ped despawned while the emote loops: the hold is released, the arbiter keeps looping.
        EmoteArbiter loop;
        loop.StartAnim(0, -1, 1);
        EmoteDictHold lh;
        AnimDictRefs refs3;
        lh.Sync(refs3, "dict_y");
        r = lh.Sync(refs3, "");
        Check("item 6: a despawned ped frees its dictionary while the loop stays active", r == "dict_y" && loop.Active());
    }

    // ---- review item 7: a failed read is not damage -------------------------------------------------
    {
        VitalsTracker v;
        v.Reset();
        v.Tick(Obs(0, 200));
        v.Tick(Obs(16, 200));
        VitalsOut o = v.Tick(Obs(32, 0, 0, false));     // GET_ENTITY_HEALTH answered 0 (uncallable) on a living ped
        Check("item 7: health 0 on a ped that is not dead (failed read) sends nothing and applies nothing",
              SumHits(o) == 0 && !o.apply && v.Stats().skippedReads == 1, F("skipped %u", v.Stats().skippedReads));
        o = v.Tick(Obs(48, 200, -1));                    // GET_PED_ARMOUR answered -1
        Check("item 7: armour -1 (failed read) is skipped too", SumHits(o) == 0 && v.Stats().skippedReads == 2);
        o = v.Tick(Obs(64, 200, 0));
        Check("item 7: the next good read sees no loss (nothing was baselined from the bad ones)",
              SumHits(o) == 0 && v.Pending() == 0);
        o = v.Tick(Obs(80, 150, 0));
        Check("item 7: a real loss after that is still reported (50)", SumHits(o) == 50, F("%d", SumHits(o)));
    }

    // ---- review item 8: the dead path sends the server's WHOLE value --------------------------------
    {
        VitalsTracker v;
        v.Reset(200, 50);                                // a gamemode set server health 200 (GTA shows 100 above the line)
        v.Tick(Obs(0, 200, 50));
        v.Tick(Obs(16, 200, 50));
        const VitalsOut o = v.Tick(Obs(32, 0, 0, true));
        Check("item 8: dead with server health 200 + armour 50 sends 250, not the 150 the ped showed",
              SumHits(o) == 250, F("%d", SumHits(o)));
    }

    // ---- review item 9: past the give-up the server's word wins; requests back off -------------------
    {
        RespawnGate g;
        g.OnDeath(1000);
        Check("item 9: before 30 s: Wait", g.GiveUp(30999, false) == GiveUpAction::Wait &&
              g.GiveUp(30999, true) == GiveUpAction::Wait);
        Check("item 9: 30 s and the server has not said dead: revive locally (the old rescue)",
              g.GiveUp(31000, false) == GiveUpAction::ReviveLocally);
        Check("item 9: 30 s and the server says dead: keep asking, never revive locally",
              g.GiveUp(31000, true) == GiveUpAction::KeepAsking);
        std::string sends;
        int early = 0;
        for (double t = 1000; t < 80000; t += 50)
            if (g.ShouldSend(t))
            {
                if (t < 31000) ++early;
                else sends += F("%.0f ", t);
            }
        Check("item 9: once a second until the give-up (25 requests, 6.25 s to 30.25 s)", early == 25, F("%d", early));
        Check("item 9: then 1, 2, 4, 8 ... s capped at 10 s between requests",
              sends == "31250 33250 37250 45250 55250 65250 75250 ", sends);
        g.OnAlive();
        Check("item 9: alive again: no more requests", !g.ShouldSend(90000) && g.GiveUp(90000, true) == GiveUpAction::Wait);
    }

    // ---- review item 10: our own Damage and RespawnOk are never lost to a full queue ----------------
    {
        VitalsInbox in;
        for (int i = 0; i < 1000; ++i)
            in.OnDamage(150, 0, 1, false, false, 3, 0xAB);   // a flood of other players' hits
        in.OnDamage(130, 5, 20, true, false, 7, 0);          // our own self-hit's answer, newest
        DamageSlot s;
        const bool got = in.Take(s);
        Check("item 10: 1001 Damage events fold into one record: damage summed, newest health and armour win",
              got && s.events == 1001 && s.damage == 1020 && s.health == 130 && s.armour == 5,
              F("events %u damage %u health %u armour %u", s.events, s.damage, s.health, s.armour));
        Check("item 10: ... the self-hit's ack is recognised (20) apart from the others' (1000)",
              s.selfAckDamage == 20 && s.otherEvents == 1000 && s.otherDamage == 1000 && s.lastAttacker == 3 &&
              s.lastWeapon == 0xAB);
        Check("item 10: Take empties it", !in.Take(s));
        in.OnDamage(0, 0, 200, false, true, 4, 1);
        in.OnKilled();
        Check("item 10: a lethal Damage and a Killed both survive as killed", in.Take(s) && s.killed);
        in.OnKilled();
        Check("item 10: a Killed alone is kept (no Damage events)", in.Take(s) && s.killed && s.events == 0);

        struct Ev { int id; };
        DropQueue<Ev> q(256);
        int accepted = 0;
        for (int i = 0; i < 300; ++i)
            accepted += q.Push(Ev{ i }, false) ? 1 : 0;
        Check("item 10: a full queue refuses further kill-feed lines", accepted == 256 && q.Refused() == 44,
              F("accepted %d refused %u", accepted, q.Refused()));
        const bool ok = q.Push(Ev{ 9001 }, true);
        std::vector<Ev> drained;
        q.Drain(drained);
        Check("item 10: ... but a vehicle answer evicts the OLDEST feed line and goes in, order kept",
              ok && q.Evicted() == 1 && drained.size() == 256 && drained.front().id == 1 && drained.back().id == 9001,
              F("size %zu first %d last %d", drained.size(), drained.empty() ? -1 : drained.front().id,
                drained.empty() ? -1 : drained.back().id));
        DropQueue<Ev> full(2);
        full.Push(Ev{ 1 }, true);
        full.Push(Ev{ 2 }, true);
        Check("item 10: a queue full of answers refuses a further one rather than dropping an answer",
              !full.Push(Ev{ 3 }, true) && full.Size() == 2);
    }

    // ---- review item 11: a reused vehicle id with another model replaces the entity -------------------
    {
        Check("item 11: the server's model changed on a reused id: stale (replace the entity)",
              NetVehicleModelStale(0x2C3731D9, 0xB779A091, false, false));
        Check("item 11: same model: not stale", !NetVehicleModelStale(0xB779A091, 0xB779A091, false, false));
        Check("item 11: our own adopted car keeps its entity", !NetVehicleModelStale(1, 2, true, false));
        Check("item 11: a car we sit in keeps its entity", !NetVehicleModelStale(1, 2, false, true));
        Check("item 11: an unknown model on either side is not stale", !NetVehicleModelStale(0, 2, false, false) &&
              !NetVehicleModelStale(1, 0, false, false));
    }

    // ---- review item 16: a drive-by shot carries its aim point ----------------------------------------
    {
        Check("item 16: free aiming sends the aim point", AimPointDue(true, 1000, -1e9));
        Check("item 16: 400 ms after a shot (hip fire from a car) still sends it", AimPointDue(false, 1400, 1000));
        Check("item 16: 500 ms after a shot it stops", !AimPointDue(false, 1500, 1000));
        Check("item 16: never shot, not aiming: no aim point", !AimPointDue(false, 1000, -1e9));
    }

    // ---- review item 17: explosive and thrown weapons are never played as bullets ------------------
    {
        // Hashes printed in GameplayFixesV inc/shv/enums.h (the ScriptHookV SDK Weapon enum): each must
        // equal Joaat of its game name AND be excluded.
        struct W { const char* name; uint32_t cited; };
        const W explosive[] = {
            { "WEAPON_GRENADELAUNCHER", 0xA284510B }, { "WEAPON_GRENADELAUNCHER_SMOKE", 0x4DD2DC56 },
            { "WEAPON_RPG", 0xB1CA77B1 },             { "WEAPON_HOMINGLAUNCHER", 0x63AB0442 },
            { "WEAPON_FIREWORK", 0x7F7497E5 },        { "WEAPON_RAILGUN", 0x6D544C99 },
            { "WEAPON_GRENADE", 0x93E220BD },         { "WEAPON_STICKYBOMB", 0x2C3731D9 },
            { "WEAPON_SMOKEGRENADE", 0xFDBC8A50 },    { "WEAPON_BZGAS", 0xA0973D5E },
            { "WEAPON_MOLOTOV", 0x24B17070 },         { "WEAPON_SNOWBALL", 0x0787F0BB },
            { "WEAPON_PROXMINE", 0xAB564B93 },
        };
        int n = 0, bad = 0;
        for (const W& w : explosive)
        {
            ++n;
            if (Joaat(w.name) != w.cited || !IsExplosiveOrThrownWeapon(w.cited, 0)) ++bad;
        }
        Check("item 17: 13 explosive / thrown hashes cited from SHV enums.h equal Joaat(name) and are excluded",
              n == 13 && bad == 0, F("n %d bad %d", n, bad));
        const W bullets[] = {
            { "WEAPON_PISTOL", 0x1B06D571 }, { "WEAPON_MINIGUN", 0x42BF8A85 }, { "WEAPON_SMG", 0x2BE6766B },
            { "WEAPON_PUMPSHOTGUN", 0x1D073A89 }, { "WEAPON_SNIPERRIFLE", 0x05FC3C11 },
        };
        n = bad = 0;
        for (const W& w : bullets)
        {
            ++n;
            if (Joaat(w.name) != w.cited || IsExplosiveOrThrownWeapon(w.cited, 0)) ++bad;
        }
        Check("item 17: 5 bullet weapons cited from SHV enums.h are NOT excluded", n == 5 && bad == 0, F("n %d bad %d", n, bad));
        Check("item 17: the minigun in GROUP_HEAVY (bullets) is still shown; the heavy group is not the exclusion",
              !IsExplosiveOrThrownWeapon(0x42BF8A85, kWeaponGroupHeavy));
        Check("item 17: any weapon in GROUP_THROWN is excluded even if a newer DLC one is not in the list",
              IsExplosiveOrThrownWeapon(0x12345678, kWeaponGroupThrown));
        Check("item 17: the group hashes are the ones GameplayFixesV prints (globals.h:585-586)",
              kWeaponGroupHeavy == 0xA27A4F9F && kWeaponGroupThrown == 0x5C4C5883);
        Check("item 17: the list holds the 27 names of weapons.json's explosive / fire / smoke / thrown set",
              kExplosiveOrThrownCount == 27, F("%zu", kExplosiveOrThrownCount));
        Check("item 17: Joaat is case-insensitive like GET_HASH_KEY", Joaat("weapon_rpg") == Joaat("WEAPON_RPG"));
    }

    // ---- the pause menu: ids, then the gate -----------------------------------------------------------
    {
        Check("pause: Joaat reproduces the -1171018317 of the ACTIVATE_FRONTEND_MENU doc example (MP pause)",
              Joaat("FE_MENU_VERSION_MP_PAUSE") == (uint32_t)-1171018317);
        Check("pause: FE_MENU_VERSION_SP_PAUSE is Joaat of its name (GameplayFixesV globals.h:131) = 0xD528C7E2",
              kFeMenuVersionSpPause == Joaat("FE_MENU_VERSION_SP_PAUSE") && kFeMenuVersionSpPause == 0xD528C7E2u,
              F("0x%08X", (unsigned)kFeMenuVersionSpPause));
        Check("pause: the controls are 199 (P), 200 (Esc) and 202 (Backspace)",
              kControlFrontendPause == 199 && kControlFrontendPauseAlt == 200 && kControlFrontendCancel == 202);
        Check("pause: 199 / 200 are held disabled only while connected and the menu is closed",
              PauseMenuGate::ControlsHeld(true, false) && !PauseMenuGate::ControlsHeld(true, true) &&
              !PauseMenuGate::ControlsHeld(false, false));

        PauseMenuGate g;
        PauseMenuIn in;
        in.nowMs = 0;
        in.connected = true;
        in.canOpen = true;
        PauseMenuOut o = g.Tick(in);
        Check("pause: connected, closed, no press: the game's own open is blocked this frame", o.disableFrontend && !o.activate);
        in.nowMs = 16;
        in.openPressed = true;
        o = g.Tick(in);
        Check("pause: Esc / P opens it ourselves (togglePause false); this frame's frontend is NOT disabled",
              o.activate && !o.disableFrontend);
        in.openPressed = false;
        in.nowMs = 100;
        o = g.Tick(in);
        Check("pause: during the 500 ms after the request the frontend is not disabled either", !o.disableFrontend && !o.activate);
        in.nowMs = 600;
        o = g.Tick(in);
        Check("pause: ... and again blocked once the grace is over and the menu never showed", o.disableFrontend);
        in.menuActive = true;
        in.nowMs = 700;
        o = g.Tick(in);
        Check("pause: the menu reads active: justOpened, nothing disabled (Esc / Backspace close it)",
              o.justOpened && !o.disableFrontend && !o.activate && !o.logProof && g.Open());
        int proofs = 0;
        for (double t = 716; t <= 10700; t += 16)
        {
            in.nowMs = t;
            o = g.Tick(in);
            proofs += o.logProof;
            if (o.disableFrontend || o.activate || o.justOpened) proofs += 1000;
        }
        Check("pause: one proof line per 2 s while open (5 in 10 s), and never a disable or a second open", proofs == 5,
              F("%d", proofs));
        in.menuActive = false;
        in.nowMs = 10716;
        o = g.Tick(in);
        Check("pause: closed again: justClosed, and the block resumes", o.justClosed && o.disableFrontend && !g.Open());

        PauseMenuGate g2;
        PauseMenuIn p;
        p.connected = true;
        p.canOpen = true;
        p.openPressed = true;
        p.overlayInteractive = true;
        p.nowMs = 1000;
        o = g2.Tick(p);
        Check("pause: Esc with the chat open is the chat's: no menu", !o.activate && o.disableFrontend);
        p.overlayInteractive = false;
        p.nowMs = 1100;
        o = g2.Tick(p);
        Check("pause: ... and not 100 ms later either (the same press, the panel just closed)", !o.activate);
        p.nowMs = 1300;
        o = g2.Tick(p);
        Check("pause: a press 300 ms after the panel closed opens it", o.activate);

        PauseMenuGate g3;
        PauseMenuIn e;
        e.connected = true;
        e.canOpen = false;                               // the entry is not done: the world is not live
        e.openPressed = true;
        e.nowMs = 5000;
        o = g3.Tick(e);
        Check("pause: before the entry is done a press opens nothing, the frontend stays blocked", !o.activate && o.disableFrontend);
        PauseMenuGate g4;
        PauseMenuIn off;
        off.connected = false;
        off.openPressed = true;
        off.canOpen = true;
        off.nowMs = 1;
        o = g4.Tick(off);
        Check("pause: offline the gate does nothing at all (the game's own menu is the normal one)",
              !o.activate && !o.disableFrontend && !o.justOpened && !o.logProof);
    }
}

// ---- sync plan W3: combat client (shots, hit reactions, melee, kill feed) ---------------------------
static void TestW3Combat()
{
    // ---- combat 3: WeaponShowCache caches only callable answers ------------------------------------
    {
        WeaponShowCache c;
        int calls = 0;
        bool callable = false;
        auto probe = [&](uint32_t) {
            ++calls;
            WeaponProbe p;
            p.callable = callable;
            p.show = callable;
            return p;
        };
        const uint32_t carbine = Joaat("WEAPON_CARBINERIFLE");
        const bool first = c.Show(carbine, probe);
        Check("W3 shots: an uncallable probe answers false and caches nothing (IsGunWeapon cached false forever)",
              !first && c.Size() == 0 && calls == 1, F("first %d size %zu calls %d", first, c.Size(), calls));
        const bool again = c.Show(carbine, probe);
        Check("W3 shots: ... and the weapon is asked again on the next frame", !again && calls == 2 && c.Size() == 0,
              F("again %d calls %d", again, calls));
        callable = true;
        const bool now = c.Show(carbine, probe);
        Check("W3 shots: callable at last: true, and cached", now && c.Size() == 1 && calls == 3,
              F("now %d size %zu calls %d", now, c.Size(), calls));
        callable = false;
        Check("W3 shots: a cached answer is not asked again", c.Show(carbine, probe) && calls == 3);

        WeaponShowCache d;
        int mcalls = 0;
        auto melee = [&](uint32_t) {
            ++mcalls;
            WeaponProbe p;
            p.callable = true;
            p.show = false;
            return p;
        };
        const uint32_t bat = Joaat("WEAPON_BAT");
        d.Show(bat, melee);
        const bool batShown = d.Show(bat, melee);
        Check("W3 shots: a callable 'no' (a bat) is a real answer: cached, asked once", !batShown && mcalls == 1 && d.Size() == 1,
              F("shown %d calls %d size %zu", batShown, mcalls, d.Size()));
        Check("W3 shots: weapon 0 is never shown and never probed", !d.Show(0, melee) && mcalls == 1);

        WeaponShowCache small(2);
        small.Show(1, [](uint32_t) { WeaponProbe p; p.callable = true; p.show = true; return p; });
        small.Show(2, [](uint32_t) { WeaponProbe p; p.callable = true; p.show = true; return p; });
        int over = 0;
        auto third = [&](uint32_t) { ++over; WeaponProbe p; p.callable = true; p.show = true; return p; };
        const bool a3 = small.Show(3, third);
        small.Show(3, third);
        Check("W3 shots: past its cap an answer is still returned (and the cache does not grow)",
              a3 && small.Size() == 2 && over == 2, F("a3 %d size %zu probes %d", a3, small.Size(), over));
    }

    // ---- combat 3: ShotDebt fires every owed shot, three a frame -----------------------------------
    {
        ShotDebt d;
        d.Add(9);
        const int f1 = d.Take(), f2 = d.Take(), f3 = d.Take(), f4 = d.Take();
        Check("W3 shots: a shotSeq jump of 9 is fired 3, 3, 3 over three frames (today 3 in total), then nothing",
              f1 == 3 && f2 == 3 && f3 == 3 && f4 == 0 && d.Owed() == 0, F("%d %d %d %d owed %d", f1, f2, f3, f4, d.Owed()));
        d.Add(20);
        int frames = 0, fired = 0, per = 0;
        bool over3 = false;
        while (d.Owed() > 0 && frames < 50)
        {
            per = d.Take();
            over3 = over3 || per > 3;
            fired += per;
            ++frames;
        }
        Check("W3 shots: a 20-shot debt is capped at 15 (the 4-bit counter's range), 5 frames of 3, never more than 3",
              fired == 15 && frames == 5 && !over3, F("fired %d frames %d over3 %d", fired, frames, over3));
        // A steady stream under the cap loses nothing: 2 + 5 + 1 owed over three frames.
        ShotDebt s;
        int total = 0;
        s.Add(2);
        total += s.Take();
        s.Add(5);
        total += s.Take();
        s.Add(1);
        total += s.Take();
        total += s.Take();
        Check("W3 shots: shots added across frames are all fired (2 + 5 + 1 = 8, none discarded)", total == 8 && s.Owed() == 0,
              F("total %d owed %d", total, s.Owed()));
        ShotDebt z;
        z.Add(0);
        z.Add(-4);
        Check("W3 shots: zero or negative adds owe nothing", z.Owed() == 0 && z.Take() == 0);
        z.Add(7);
        z.Clear();
        Check("W3 shots: Clear forgets the debt (a dead ped, a non-gun weapon)", z.Owed() == 0 && z.Take() == 0);
    }

    // ---- combat 2: HitReact::Decide ---------------------------------------------------------------
    {
        const HitReactState standing;
        HitReaction r = HitReact::Decide(25, 200, 0, 1e9, standing);
        Check("W3 react: 25 at max 200 (12.5%) is a flinch", r.kind == ReactKind::Flinch, F("kind %d", (int)r.kind));
        r = HitReact::Decide(60, 200, 0, 1e9, standing);
        Check("W3 react: 60 at max 200 (30%) is a knockdown of 780 ms (300 + 8 x 60)",
              r.kind == ReactKind::Knockdown && r.ragdollMs == 780, F("kind %d ms %d", (int)r.kind, r.ragdollMs));
        HitReactState down;
        down.sinceKnockdownMs = 500;
        r = HitReact::Decide(60, 200, 0, 500, down);
        Check("W3 react: a second 60 within 1.5 s of the knockdown is a flinch only", r.kind == ReactKind::Flinch,
              F("kind %d", (int)r.kind));
        r = HitReact::Decide(60, 200, 0, 300, down);
        Check("W3 react: ... and under 400 ms after any reaction nothing at all", r.kind == ReactKind::None, F("kind %d", (int)r.kind));
        down.sinceKnockdownMs = 1600;
        r = HitReact::Decide(60, 200, 0, 1600, down);
        Check("W3 react: 1.6 s after the knockdown the immunity is over: knocked down again", r.kind == ReactKind::Knockdown,
              F("kind %d", (int)r.kind));
        Check("W3 react: 5 at max 200 (2.5%) is below the 3% flinch line",
              HitReact::Decide(5, 200, 0, 1e9, standing).kind == ReactKind::None);
        Check("W3 react: 6 at max 200 (exactly 3%) flinches",
              HitReact::Decide(6, 200, 0, 1e9, standing).kind == ReactKind::Flinch);
        Check("W3 react: 39 (19.5%) flinches, 40 (20%) knocks down for 620 ms",
              HitReact::Decide(39, 200, 0, 1e9, standing).kind == ReactKind::Flinch &&
              HitReact::Decide(40, 200, 0, 1e9, standing).kind == ReactKind::Knockdown &&
              HitReact::Decide(40, 200, 0, 1e9, standing).ragdollMs == 620);
        r = HitReact::Decide(10, 200, HitReact::kFlagExplosion, 1e9, standing);
        Check("W3 react: an explosion knocks down whatever its damage (10 -> 380 ms)",
              r.kind == ReactKind::Knockdown && r.ragdollMs == 380, F("kind %d ms %d", (int)r.kind, r.ragdollMs));
        // W8: a melee blow never flinches (a bullet at the chest is the wrong picture for a punch): see TestW8. 15% and above knocks down.
        Check("W3 react: a heavy melee blow (30 = 15%) knocks down for 540 ms (W8: was 20 = 10% for 460 ms; lighter ones stagger)",
              HitReact::Decide(30, 200, HitReact::kFlagMelee, 1e9, standing).kind == ReactKind::Knockdown &&
              HitReact::Decide(29, 200, HitReact::kFlagMelee, 1e9, standing).kind == ReactKind::Stagger &&
              HitReact::Decide(30, 200, HitReact::kFlagMelee, 1e9, standing).ragdollMs == 540);
        Check("W3 react: the ragdoll time is clamped to 1.5 s",
              HitReact::Decide(200, 200, 0, 1e9, standing).ragdollMs == 1500);
        Check("W3 react: the same 25 damage is a larger share of a 100-health player (25%): a knockdown",
              HitReact::Decide(25, 100, 0, 1e9, standing).kind == ReactKind::Knockdown);
        HitReactState dead, car, climb;
        dead.dead = true;
        car.inVehicle = true;
        climb.climbing = true;
        Check("W3 react: never while dead, in a vehicle or climbing",
              HitReact::Decide(100, 200, 0, 1e9, dead).kind == ReactKind::None &&
              HitReact::Decide(100, 200, 0, 1e9, car).kind == ReactKind::None &&
              HitReact::Decide(100, 200, 0, 1e9, climb).kind == ReactKind::None);
        Check("W3 react: no damage, or an unknown max health, is no reaction",
              HitReact::Decide(0, 200, 0, 1e9, standing).kind == ReactKind::None &&
              HitReact::Decide(50, 0, 0, 1e9, standing).kind == ReactKind::None);
    }

    // ---- combat 2: HitReactArbiter (the 600 ms window that keeps DrivePeer off the ped) -------------
    {
        HitReactArbiter a;
        HitReactOut o = a.Tick(0);
        Check("W3 react: an idle arbiter holds nothing and ends nothing", !o.holds && !o.ended && !a.Active());
        HitReaction fl;
        fl.kind = ReactKind::Flinch;
        a.Start(1000, fl);
        const bool h0 = a.Tick(1000).holds, h1 = a.Tick(1599).holds;
        o = a.Tick(1600);
        const HitReactOut o2 = a.Tick(1616);
        Check("W3 react: a flinch holds the gait for 600 ms, then ends once",
              h0 && h1 && !o.holds && o.ended && !o2.ended && !o2.holds, F("%d %d holds %d ended %d / %d", h0, h1, o.holds, o.ended, o2.ended));
        Check("W3 react: time since the last reaction is tracked (700 ms at 1700); no knockdown yet is 'long ago'",
              a.SinceLastMs(1700) == 700.0 && a.SinceKnockdownMs(1700) > 1e8, F("%g %g", a.SinceLastMs(1700), a.SinceKnockdownMs(1700)));
        HitReaction kd;
        kd.kind = ReactKind::Knockdown;
        kd.ragdollMs = 780;
        a.Start(5000, kd);
        const bool k1 = a.Tick(5000 + 1079).holds;
        o = a.Tick(5000 + 1080);
        Check("W3 react: a knockdown holds for its ragdoll time plus 300 ms, then ends once",
              k1 && !o.holds && o.ended, F("held %d holds %d ended %d", k1, o.holds, o.ended));
        a.Start(8000, kd);
        a.Start(8100, fl);
        const bool still = a.Tick(8000 + 1050).holds;
        Check("W3 react: a flinch inside a knockdown does not shorten it, and the knockdown clock stays at 8000",
              still && a.SinceKnockdownMs(8500) == 500.0 && a.SinceLastMs(8500) == 400.0,
              F("still %d sinceKd %g sinceLast %g", still, a.SinceKnockdownMs(8500), a.SinceLastMs(8500)));
        a.Clear();
        o = a.Tick(8200);
        Check("W3 react: Clear (the ped is gone) holds and ends nothing", !o.holds && !o.ended && !a.Active());
    }

    // ---- combat 4: the melee swing gate --------------------------------------------------------------
    {
        MeleeSwingGate g;
        const bool press = g.Tick(1000, true, false);
        const bool action = g.Tick(1100, false, true);          // IS_PED_PERFORMING_MELEE_ACTION rises 100 ms later
        const bool hold = g.Tick(1116, false, true);
        Check("W3 melee: a press counts once; the action edge 100 ms later is the same swing (de-duplicated)",
              press && !action && !hold, F("%d %d %d", press, action, hold));
        g.Tick(1200, false, false);                              // the action ended
        const bool second = g.Tick(1500, false, true);           // a new swing started by a combo: 500 ms after the first
        Check("W3 melee: the action edge alone (a combo the control did not report) is a second swing", second);
        const bool pressLater = g.Tick(1550, true, true);
        Check("W3 melee: a press within 300 ms of a counted swing is the same swing", !pressLater);
        const bool press3 = g.Tick(2000, true, true);
        Check("W3 melee: a press 500 ms after the last counted swing is a new swing", press3);
        g.Reset();
        Check("W3 melee: after Reset the first press counts again at once", g.Tick(2010, true, false));
        MeleeSwingGate h;
        const bool held = h.Tick(100, false, true);
        const bool steady = h.Tick(116, false, true);
        const bool steady2 = h.Tick(5000, false, true);
        Check("W3 melee: an action that stays true is one swing, not one per frame", held && !steady && !steady2,
              F("%d %d %d", held, steady, steady2));
    }

    // ---- combat 3: a real shot (peer_shoot=ped) only when the line passes nobody -------------------
    {
        // A standing player: capsule centre z 0, half height 0.9 m, radius 0.5 m, at (10, 0).
        Check("W3 line: a shot straight through a player's chest touches the capsule",
              SegmentTouchesCapsule(0, 0, 0, 20, 0, 0, 10, 0, 0, 0.5f, 0.9f));
        Check("W3 line: 3 m to the side misses; 0.4 m to the side (inside the radius) hits, 0.6 m misses",
              !SegmentTouchesCapsule(0, 3, 0, 20, 3, 0, 10, 0, 0, 0.5f, 0.9f) &&
              SegmentTouchesCapsule(0, 0.4f, 0, 20, 0.4f, 0, 10, 0, 0, 0.5f, 0.9f) &&
              !SegmentTouchesCapsule(0, 0.6f, 0, 20, 0.6f, 0, 10, 0, 0, 0.5f, 0.9f));
        Check("W3 line: a shot 3 m above the head misses; one at 0.8 m (inside the half height) hits",
              !SegmentTouchesCapsule(0, 0, 3, 20, 0, 3, 10, 0, 0, 0.5f, 0.9f) &&
              SegmentTouchesCapsule(0, 0, 0.8f, 20, 0, 0.8f, 10, 0, 0, 0.5f, 0.9f));
        Check("W3 line: a segment that ends 3 m short of the player misses; a point inside it hits",
              !SegmentTouchesCapsule(0, 0, 0, 7, 0, 0, 10, 0, 0, 0.5f, 0.9f) &&
              SegmentTouchesCapsule(10, 0.1f, 0, 10, 0.1f, 0, 10, 0, 0, 0.5f, 0.9f));
        Check("W3 line: a downward diagonal that reaches the player's body from above hits",
              SegmentTouchesCapsule(0, 0, 5, 10, 0, 0, 10, 0, 0, 0.5f, 0.9f));
    }

    // ---- combat 5: weapon labels, kill-feed delay, muzzle flash class, melee class -------------------
    {
        Check("W3 label: WeaponLabel(Joaat(\"WEAPON_CARBINERIFLE\")) is \"Carbine Rifle\"",
              WeaponLabel(Joaat("WEAPON_CARBINERIFLE")) == "Carbine Rifle");
        // Hashes cited from ScriptHookV enums.h, not from Joaat: the table is keyed by the real game hash.
        Check("W3 label: cited game hashes resolve (0x83BF0278 carbine, 0x1B06D571 pistol, 0xA2719263 fists, 0x42BF8A85 minigun)",
              WeaponLabel(0x83BF0278) == "Carbine Rifle" && WeaponLabel(0x1B06D571) == "Pistol" &&
              WeaponLabel(0xA2719263) == "Fists" && WeaponLabel(0x42BF8A85) == "Minigun");
        Check("W3 label: more cited hashes (0xBFEFFF6D assault rifle, 0x1D073A89 pump shotgun, 0x05FC3C11 sniper, 0xB1CA77B1 RPG, 0x99B507EA knife)",
              WeaponLabel(0xBFEFFF6D) == "Assault Rifle" && WeaponLabel(0x1D073A89) == "Pump Shotgun" &&
              WeaponLabel(0x05FC3C11) == "Sniper Rifle" && WeaponLabel(0xB1CA77B1) == "RPG" && WeaponLabel(0x99B507EA) == "Knife");
        Check("W3 label: an unknown hash and 0 have no label (the feed line leaves the weapon out)",
              WeaponLabel(0x12345678).empty() && WeaponLabel(0).empty());
        bool allOk = kWeaponLabelCount >= 60;
        int dupes = 0, empty = 0;
        for (size_t i = 0; i < kWeaponLabelCount; ++i)
        {
            if (!kWeaponLabels[i].label || !*kWeaponLabels[i].label) ++empty;
            if (WeaponLabel(kWeaponLabels[i].hash).empty()) ++empty;
            for (size_t j = i + 1; j < kWeaponLabelCount; ++j)
                if (kWeaponLabels[i].hash == kWeaponLabels[j].hash) ++dupes;
        }
        Check("W3 label: the table has 60+ weapons, every label is non-empty and no hash is listed twice",
              allOk && !dupes && !empty, F("count %zu dupes %d empty %d", kWeaponLabelCount, dupes, empty));
        Check("W3 label: weapon group hashes equal the values NativeDB lists for GET_WEAPONTYPE_GROUP",
              kWeaponGroupMelee == 3566412244u && kWeaponGroupUnarmed == 2685387236u && kWeaponGroupPistol == 416676503u &&
              kWeaponGroupSmg == 3337201093u && kWeaponGroupShotgun == 860033945u && kWeaponGroupRifle == 970310034u &&
              kWeaponGroupMg == 1159398588u && kWeaponGroupSniper == 3082541095u && kWeaponGroupHeavy == 2725924767u &&
              kWeaponGroupThrown == 1548507267u);
        Check("W3 melee class: fists, a bat in GROUP_MELEE and the unarmed group count; a carbine and a grenade do not",
              IsMeleeWeapon(0xA2719263, 0) && IsMeleeWeapon(0x958A4A8F, kWeaponGroupMelee) &&
              IsMeleeWeapon(0x12345678, kWeaponGroupUnarmed) && !IsMeleeWeapon(0x83BF0278, kWeaponGroupRifle) &&
              !IsMeleeWeapon(0x93E220BD, kWeaponGroupThrown) && !IsMeleeWeapon(0, 0));
        Check("W3 muzzle: each gun class names a core muzzle effect; an unknown class falls back to the rifle one",
              std::string(MuzzleFlashEffect(kWeaponGroupPistol)) == "muz_pistol" &&
              std::string(MuzzleFlashEffect(kWeaponGroupRifle)) == "muz_assault_rifle" &&
              std::string(MuzzleFlashEffect(kWeaponGroupSmg)) == "muz_smg" &&
              std::string(MuzzleFlashEffect(kWeaponGroupShotgun)) == "muz_shotgun" &&
              std::string(MuzzleFlashEffect(kWeaponGroupMg)) == "muz_machinegun" &&
              std::string(MuzzleFlashEffect(kWeaponGroupSniper)) == "muz_sniper" &&
              std::string(MuzzleFlashEffect(0)) == "muz_assault_rifle");
        Check("W3 death delay: the puppet's death is shown at receipt + its pose delay (120 ms), a bad delay is clamped to 0..500",
              DeathDisplayAtMs(1000, 120) == 1120.0 && DeathDisplayAtMs(1000, -5) == 1000.0 && DeathDisplayAtMs(1000, 9000) == 1500.0);
    }
}

// ---- sync plan W6: vehicle body, ownership, combat inputs of protocol v5, on-foot Tier B ------------

static void TestW6()
{
    // ---- lights, doors, roof (libs/sync/vehicle_body.h) ----
    {
        const float ratios[kBodyDoors] = { 0.5f, 0.0f, 0.11f, 0.1f, 1.0f, NAN };   // FL FR RL RR hood trunk
        const uint8_t doors = DoorsFromRatios(ratios);
        Check("W6 doors: a door is open above ratio 0.1 (FL, RL, hood); exactly 0.1, shut and NaN are closed",
              doors == (proto::kDoorFrontLeft | proto::kDoorRearLeft | proto::kDoorHood), F("doors 0x%02x", doors));
        const uint8_t f2 = Flags2FromState(true, true, false, false, false, true);
        const uint8_t f2b = Flags2FromState(false, false, true, true, true, false);
        Check("W6 flags2: headlights, high beam, interior, indicator L / R and roof down land on the protocol's bits and nothing else",
              f2 == (proto::kVeh2Headlights | proto::kVeh2HighBeam | proto::kVeh2RoofDown) &&
                  f2b == (proto::kVeh2Interior | proto::kVeh2IndicatorL | proto::kVeh2IndicatorR) && (f2 & ~proto::kVeh2Known) == 0,
              F("0x%02x 0x%02x", f2, f2b));
        Check("W6 roof: lowering (1) and down (2) read as down, up (0), raising (3), -1 and junk as up; the bit follows the target",
              RoofDownFromState(0) == false && RoofDownFromState(1) == true && RoofDownFromState(2) == true &&
                  RoofDownFromState(3) == false && RoofDownFromState(-1) == false && RoofDownFromState(77) == false);
        Check("W6 headlights shown = the server's override OR the controller's actual state",
              !HeadlightsShown(false, false) && HeadlightsShown(true, false) && HeadlightsShown(false, true) && HeadlightsShown(true, true));
        BodyShown shown;
        const BodyDelta first = DiffBody(shown, proto::kVeh2Headlights, proto::kDoorHood);
        shown = BodyShown{ proto::kVeh2Headlights, proto::kDoorHood, true };
        const BodyDelta same = DiffBody(shown, proto::kVeh2Headlights, proto::kDoorHood);
        const BodyDelta one = DiffBody(shown, (uint8_t)(proto::kVeh2Headlights | proto::kVeh2IndicatorL),
                                       (uint8_t)(proto::kDoorHood | proto::kDoorTrunk));
        const BodyDelta off = DiffBody(shown, 0, proto::kDoorHood);
        Check("W6 body diff: the first sight applies every known bit; an unchanged state nothing; a change only its bits (set or cleared)",
              first.flags2 == proto::kVeh2Known && first.doors == proto::kDoorsKnown && !same.Any() &&
                  one.flags2 == proto::kVeh2IndicatorL && one.doors == proto::kDoorTrunk &&
                  off.flags2 == proto::kVeh2Headlights && off.doors == 0,
              F("first %02x/%02x same %d one %02x/%02x off %02x/%02x", first.flags2, first.doors, (int)same.Any(), one.flags2,
                one.doors, off.flags2, off.doors));
    }
    // ---- NotOwner (VehicleResultCode 12) in the link ----
    {
        const char* text = LocalVehicleLink::RefusalText(12);
        Check("W6 link: NotOwner (12) ejects, is not retryable and has its own line",
              LocalVehicleLink::Ejects(12) && !LocalVehicleLink::Retryable(12) && text && std::string(text) == "That vehicle belongs to someone else.",
              text ? text : "(null)");
        LocalVehicleLink l;
        VehLinkOut o = l.Tick(VIn(0, 777, 0, 9));
        const bool entered = o.cmd == VehLinkCmd::Enter && o.seat == 0;
        l.OnResult(3, 12, 9);
        o = l.Tick(VIn(16, 777, 0, 9));
        const bool ejected = o.eject && o.cmd == VehLinkCmd::None && !l.Linked();
        o = l.Tick(VIn(3000, 777, 0, 9));
        Check("W6 link: a first entry refused NotOwner ejects (once, no retry seconds later)",
              entered && ejected && o.cmd == VehLinkCmd::None && !o.eject, F("entered %d ejected %d", (int)entered, (int)ejected));
        LocalVehicleLink m;
        m.Tick(VIn(0, 777, 1, 9));
        m.OnResult(3, 0, 9);
        m.Tick(VIn(16, 777, 1, 9));                        // linked as a passenger in seat 1
        VehLinkOut p = m.Tick(VIn(32, 777, 0, 9));         // moved to the driver seat of somebody else's car
        const bool asked = p.cmd == VehLinkCmd::Enter && p.seat == 0;
        m.OnResult(3, 12, 9);
        p = m.Tick(VIn(48, 777, 0, 9));
        Check("W6 link: a seat change refused NotOwner reverts to the old seat, stays linked, does not eject",
              asked && !p.eject && p.revertSeat == 1 && m.Linked() && m.Seat() == 1,
              F("asked %d eject %d revert %d linked %d seat %d", (int)asked, (int)p.eject, p.revertSeat, (int)m.Linked(), m.Seat()));
    }
    // ---- combat inputs of protocol v5 ----
    {
        VitalsInbox in;
        in.OnDamage(150, 0, 20, false, false, 3, 0xAB, 31086, HitReact::kFlagHeadshot);
        in.OnDamage(140, 0, 10, true, false, 7, 0, 5, HitReact::kFlagExplosion);                         // our own self-hit
        in.OnDamage(120, 0, 20, false, false, 4, 0xCD, 11816, HitReact::kFlagMelee | HitReact::kFlagArmourAbsorbed);
        DamageSlot ds;
        const bool got = in.Take(ds);
        VitalsInbox old;
        old.OnDamage(150, 0, 20, false, false, 3, 0xAB);
        DamageSlot ods;
        old.Take(ods);
        Check("W6 inbox: bone and flags of OTHER players' damage fold in (newest bone, OR of the flags; a self-hit adds neither); old calls read -1 / 0",
              got && ds.lastBone == 11816 && ds.otherFlags == (HitReact::kFlagHeadshot | HitReact::kFlagMelee | HitReact::kFlagArmourAbsorbed) &&
                  ods.lastBone == -1 && ods.otherFlags == 0,
              F("bone %d flags 0x%02x / old bone %d flags 0x%02x", ds.lastBone, ds.otherFlags, ods.lastBone, ods.otherFlags));
        static_assert(HitReact::kFlagHeadshot == proto::kDmgHeadshot && HitReact::kFlagMelee == proto::kDmgMelee &&
                          HitReact::kFlagExplosion == proto::kDmgExplosion && HitReact::kFlagArmourAbsorbed == proto::kDmgArmour,
                      "HitReact flags are the wire's Damage.flags");
        // the same inputs through Decide: an explosion knocks down at 5 percent, a melee blow at 15 (W8: was 10; a lighter one staggers), a bullet needs 20
        const HitReactState st;
        const HitReaction expl = HitReact::Decide(10, 200, proto::kDmgExplosion, 1e9, st);
        const HitReaction melee = HitReact::Decide(30, 200, proto::kDmgMelee, 1e9, st);
        const HitReaction punch = HitReact::Decide(20, 200, proto::kDmgMelee, 1e9, st);
        const HitReaction bullet = HitReact::Decide(20, 200, 0, 1e9, st);
        Check("W6 reaction flags: the wire's explosion and melee bits reach Decide (knockdown at 5% / 15%, a melee 10% staggers, a bullet of 10% only flinches)",
              expl.kind == ReactKind::Knockdown && melee.kind == ReactKind::Knockdown && punch.kind == ReactKind::Stagger && bullet.kind == ReactKind::Flinch,
              F("%d %d %d %d", (int)expl.kind, (int)melee.kind, (int)punch.kind, (int)bullet.kind));
    }
    {
        Check("W6 max health: the ped's maximum is 100 + the server's playerHealth (0 and junk read as the default 100)",
              PedMaxHealth(100) == 200 && PedMaxHealth(150) == 250 && PedMaxHealth(255) == 355 && PedMaxHealth(0) == 200);
        const float a = HealthFraction(250, 250), b = HealthFraction(175, 250), c = HealthFraction(100, 250), d = HealthFraction(90, 250),
                    e = HealthFraction(300, 250), g = HealthFraction(50, 100), h = HealthFraction(50, 0);
        Check("W6 HUD: the health fraction runs over the ped's own span (100 = empty): full 1, half 0.5, dead 0, clamped; a max <= 100 falls back to hp / max",
              a == 1.0f && b == 0.5f && c == 0.0f && d == 0.0f && e == 1.0f && g == 0.5f && h == 0.0f,
              F("%g %g %g %g %g %g %g", a, b, c, d, e, g, h));
        // a server with playerHealth 150: the tracker and the ped agree on a maximum of 250
        VitalsTracker v;
        v.Reset(150, 0);
        VitalsObs ob = Obs(0, 200, 0, false, PedMaxHealth(150));
        VitalsOut o = v.Tick(ob);                          // the first tick puts the server's values on the ped: 100 + 150
        const bool put = o.apply && o.setHealth == 250 && o.selfHitCount == 0;
        ob.nowMs = 50; ob.health = 250;
        v.Tick(ob);                                        // the baseline after our own write
        ob.nowMs = 100; ob.health = 240;                   // a fall of 10
        o = v.Tick(ob);
        Check("W6 vitals: with playerHealth 150 (ped max 250) the ped is given 250, and a loss of 10 is a self-hit of 10",
              put && SumHits(o) == 10 && v.Health() == 150, F("put %d hits %d health %u", (int)put, SumHits(o), (unsigned)v.Health()));
    }
    {
        const uint16_t me = 1, other = 5, third = 6;
        Check("W6 damage routing: about us / our hit on somebody / an onlooker's / unknown victim",
              ClassifyDamage(me, me, other) == DamageAbout::Me && ClassifyDamage(me, me, me) == DamageAbout::Me &&
                  ClassifyDamage(me, other, me) == DamageAbout::MyHit && ClassifyDamage(me, other, third) == DamageAbout::Onlook &&
                  ClassifyDamage(me, proto::kNoPlayer, third) == DamageAbout::None);
        Check("W6 puppet reaction: our hit and an onlooker's both react (not a killing blow, not 0 damage, not a self or server hit)",
              ReactsPuppet(DamageAbout::MyHit, false, 10, other, me) && ReactsPuppet(DamageAbout::Onlook, false, 10, other, third) &&
                  !ReactsPuppet(DamageAbout::Onlook, true, 10, other, third) && !ReactsPuppet(DamageAbout::Onlook, false, 0, other, third) &&
                  !ReactsPuppet(DamageAbout::Onlook, false, 10, other, other) && !ReactsPuppet(DamageAbout::Onlook, false, 10, other, proto::kServerId) &&
                  !ReactsPuppet(DamageAbout::Onlook, false, 10, other, proto::kNoPlayer) && !ReactsPuppet(DamageAbout::Me, false, 10, me, other));
        static_assert(KillFeedFlags::kHeadshot == proto::kKilledHeadshot && KillFeedFlags::kMelee == proto::kKilledMelee,
                      "KillFeedFlags are the wire's Killed.flags");
        Check("W6 kill feed: headshot and melee wording, with and without a weapon name; a suicide or an environment death is just 'died'",
              KillFeedLine("Ann", "Bob", "Carbine Rifle", 0, false) == "Ann killed Bob with Carbine Rifle." &&
                  KillFeedLine("Ann", "Bob", "Carbine Rifle", proto::kKilledHeadshot, false) == "Ann killed Bob with Carbine Rifle (headshot)." &&
                  KillFeedLine("Ann", "Bob", "", proto::kKilledHeadshot, false) == "Ann killed Bob (headshot)." &&
                  KillFeedLine("Ann", "Bob", "Knife", proto::kKilledMelee, false) == "Ann beat Bob to death with Knife." &&
                  KillFeedLine("Ann", "Bob", "", proto::kKilledMelee, false) == "Ann beat Bob to death." &&
                  KillFeedLine("Ann", "Bob", "", proto::kKilledMelee | proto::kKilledHeadshot, false) == "Ann beat Bob to death (headshot)." &&
                  KillFeedLine("", "Bob", "", proto::kKilledHeadshot, true) == "Bob died." &&
                  KillFeedLine("", "You", "", 0, true) == "You died.");
    }
    // ---- on-foot Tier B (libs/sync/pedstate.h) ----
    {
        PedStateEdges e;
        int reloads = 0;
        const uint16_t seq[] = { 0, proto::kPfReloading, proto::kPfReloading, 0, proto::kPfReloading };
        for (uint16_t f : seq)
            if (e.Update({ f, proto::kMoveIdle, 0 }).reloadStart) ++reloads;
        Check("W6 edges: reloading starts one reload per rising edge (2 in 0,1,1,0,1)", reloads == 2, F("%d", reloads));
        PedStateEdges s;
        const PedEdges s0 = s.Update({ 0, 0, 0 });
        const PedEdges s1 = s.Update({ proto::kPfStealth, 0, 0 });
        const PedEdges s2 = s.Update({ proto::kPfStealth, 0, 0 });
        const PedEdges s3 = s.Update({ 0, 0, 0 });
        Check("W6 edges: stealth reports a change (with its new value) only when it flips",
              !s0.stealthChanged && s1.stealthChanged && s1.stealthOn && !s2.stealthChanged && s3.stealthChanged && !s3.stealthOn);
        PedStateEdges c;
        const PedEdges c1 = c.Update({ proto::kPfCover, 0, 0 });
        const PedEdges c2 = c.Update({ proto::kPfCover, 0, 0 });
        const PedEdges c3 = c.Update({ 0, 0, 0 });
        Check("W6 edges: cover enter and exit are edges", c1.coverEnter && !c1.coverExit && !c2.coverEnter && !c2.coverExit && c3.coverExit && !c3.coverEnter);
        PedStateEdges v;
        const PedEdges v1 = v.Update({ 0, proto::kMoveEnteringVehicle, 7 });
        const PedEdges v2 = v.Update({ 0, proto::kMoveEnteringVehicle, 7 });
        const PedEdges v3 = v.Update({ proto::kPfDriver, proto::kMoveInVehicle, 0 });
        const PedEdges v4 = v.Update({ 0, proto::kMoveExitingVehicle, 7 });
        const PedEdges v5 = v.Update({ 0, proto::kMoveEnteringVehicle, 9 });                 // a different car while entering: a new start
        const PedEdges v6 = v.Update({ 0, proto::kMoveEnteringVehicle, 8 });
        Check("W6 edges: entering starts once per car (move 5 + vehId), in-vehicle starts nothing, exiting starts once with its car",
              v1.enterStart && v1.enterVeh == 7 && !v2.enterStart && !v3.enterStart && !v3.exitStart && v4.exitStart && v4.exitVeh == 7 &&
                  !v4.enterStart && v5.enterStart && v5.enterVeh == 9 && v6.enterStart && v6.enterVeh == 8,
              F("%d%d %d %d%d %d%d %d%d %d%d", v1.enterStart, v1.enterVeh, v2.enterStart, v3.enterStart, v3.exitStart, v4.exitStart,
                v4.exitVeh, v5.enterStart, v5.enterVeh, v6.enterStart, v6.enterVeh));
        PedStateEdges r;
        r.Update({ proto::kPfReloading | proto::kPfSwimming, 0, 0 });
        r.Reset();
        const PedEdges r1 = r.Update({ proto::kPfReloading | proto::kPfSwimming, 0, 0 });
        Check("W6 edges: Reset forgets the previous state (a flag held across it is an edge again); swimming mirrors its flag",
              r1.reloadStart && r1.swimming && !PedStateEdges().Update({ 0, 0, 0 }).swimming);
    }
    {
        VehicleMoveIn in;
        in.seated = true; in.tryingNetId = 7; in.walkNetId = 9; in.exitNetId = 4; in.sinceExitMs = 100;
        const VehicleMoveOut seated = DecideVehicleMove(in);
        in.seated = false;
        const VehicleMoveOut trying = DecideVehicleMove(in);                    // GetVehiclePedIsTryingToEnter wins
        in.tryingNetId = -1;
        const VehicleMoveOut walking = DecideVehicleMove(in);                   // the F / G walk to the door
        in.walkNetId = -1;
        const VehicleMoveOut exiting = DecideVehicleMove(in);
        in.sinceExitMs = 1600;
        const VehicleMoveOut late = DecideVehicleMove(in);
        in.sinceExitMs = 100; in.exitNetId = 300;
        const VehicleMoveOut badId = DecideVehicleMove(in);                     // not a networked car: nothing
        in.exitNetId = 4; in.tryingNetId = 256;
        const VehicleMoveOut bigId = DecideVehicleMove(in);
        Check("W6 vehicle move: seated sends nothing of its own; entering wins over the walk over the exit; exit holds 1.5 s; non-networked ids send nothing",
              !seated.use && trying.use && trying.move == proto::kMoveEnteringVehicle && trying.vehId == 7 &&
                  walking.use && walking.move == proto::kMoveEnteringVehicle && walking.vehId == 9 &&
                  exiting.use && exiting.move == proto::kMoveExitingVehicle && exiting.vehId == 4 && !late.use && !badId.use &&
                  bigId.use && bigId.move == proto::kMoveExitingVehicle && bigId.vehId == 4,
              F("%d %d%d %d%d %d%d %d %d %d", seated.use, trying.move, trying.vehId, walking.move, walking.vehId, exiting.move,
                exiting.vehId, late.use, badId.use, bigId.use));
        Check("W6 enter task: the seat warp is due at once with no enter task, after 2.5 s with one, never when seated or not yet seated by the server",
              EnterWarpDue(true, false, -1) && !EnterWarpDue(true, false, 2000) && EnterWarpDue(true, false, 2500) &&
                  !EnterWarpDue(true, true, 9999) && !EnterWarpDue(false, false, 9999));
    }
}

// ---- W8 (docs/sync-specs/live-0.4.2-fixes.md): melee reaction, dead-puppet pool damage, water / climb edges, alt-tab fallback ----
static void TestW8()
{
    const HitReactState standing;
    const uint8_t melee = HitReact::kFlagMelee;

    // Melee reaction decision (GI: "melee hits show no hit animation on either screen"; each punch was a flinch bullet).
    {
        const HitReaction a = HitReact::Decide(15, 200, melee, 1e9, standing);   // 7.5%
        const HitReaction b = HitReact::Decide(20, 200, melee, 1e9, standing);   // 10%: a fist at the server's ceiling
        const HitReaction c = HitReact::Decide(30, 200, melee, 1e9, standing);   // 15%: a heavy blow
        const HitReaction d = HitReact::Decide(5, 200, melee, 1e9, standing);    // 2.5%
        const HitReaction e = HitReact::Decide(6, 200, melee, 1e9, standing);    // exactly 3%
        int n = 0;
        bool neverFlinch = true;
        for (int dmg = 1; dmg <= 120; ++dmg, ++n)
            if (HitReact::Decide(dmg, 200, melee, 1e9, standing).kind == ReactKind::Flinch) neverFlinch = false;
        const bool gunsSame = HitReact::Decide(15, 200, 0, 1e9, standing).kind == ReactKind::Flinch &&
                              HitReact::Decide(25, 200, 0, 1e9, standing).kind == ReactKind::Flinch &&
                              HitReact::Decide(60, 200, 0, 1e9, standing).kind == ReactKind::Knockdown;
        Check("W8 melee react: a punch (15 = 7.5%, 20 = 10%) is a stagger of 325 / 350 ms, 30 (15%) a knockdown of 540 ms, 5 (2.5%) nothing, 6 (3%) a stagger of 280 ms; "
              "a melee blow is never a flinch bullet (damage 1..120); bullets keep flinching (15, 25) and knocking down (60)",
              a.kind == ReactKind::Stagger && a.ragdollMs == 325 && b.kind == ReactKind::Stagger && b.ragdollMs == 350 && c.kind == ReactKind::Knockdown &&
                  c.ragdollMs == 540 && d.kind == ReactKind::None && e.kind == ReactKind::Stagger && e.ragdollMs == 280 && n == 120 && neverFlinch && gunsSame,
              F("15: %d/%d 20: %d/%d 30: %d/%d 5: %d 6: %d/%d never flinch %d (%d) guns %d", (int)a.kind, a.ragdollMs, (int)b.kind, b.ragdollMs, (int)c.kind,
                c.ragdollMs, (int)d.kind, (int)e.kind, e.ragdollMs, (int)neverFlinch, n, (int)gunsSame));
        HitReactState down;
        down.sinceKnockdownMs = 500;
        const HitReaction inImmunity = HitReact::Decide(40, 200, melee, 500, down);     // a knockdown-sized blow inside the immunity window
        const HitReaction tooSoon = HitReact::Decide(40, 200, melee, 300, down);
        down.sinceKnockdownMs = 1600;
        const HitReaction later = HitReact::Decide(40, 200, melee, 1600, down);
        Check("W8 melee react: a knockdown-sized melee blow inside the 1.5 s immunity is a stagger (not a flinch), under 400 ms after any reaction nothing, after the immunity a knockdown again",
              inImmunity.kind == ReactKind::Stagger && inImmunity.ragdollMs == 450 && tooSoon.kind == ReactKind::None && later.kind == ReactKind::Knockdown,
              F("%d/%d %d %d", (int)inImmunity.kind, inImmunity.ragdollMs, (int)tooSoon.kind, (int)later.kind));
    }
    // The arbiter holds a stagger for its ragdoll plus the stand-up tail and does NOT start the knockdown immunity.
    {
        HitReactArbiter arb;
        HitReaction st;
        st.kind = ReactKind::Stagger;
        st.ragdollMs = 350;
        arb.Start(1000, st);
        const bool holds = arb.Tick(1640).holds;
        const HitReactOut end = arb.Tick(1700);
        const bool noImmunity = arb.SinceKnockdownMs(1700) >= HitReactArbiter::kLongAgoMs && std::fabs(arb.SinceLastMs(1700) - 700.0) < 1e-9;
        HitReactArbiter kd;
        HitReaction k;
        k.kind = ReactKind::Knockdown;
        k.ragdollMs = 350;
        kd.Start(1000, k);
        Check("W8 melee react: a stagger of 350 ms holds the ped for 650 ms (ragdoll + 300 ms tail), ends once, and starts no knockdown immunity (a knockdown does)",
              holds && end.ended && !end.holds && noImmunity && std::fabs(kd.SinceKnockdownMs(1700) - 700.0) < 1e-9,
              F("holds %d ended %d noImmunity %d", (int)holds, (int)end.ended, (int)noImmunity));
    }
    // hit_react modes: what each shows.
    {
        HitReaction fl, kn, sg;
        fl.kind = ReactKind::Flinch;
        kn.kind = ReactKind::Knockdown;
        sg.kind = ReactKind::Stagger;
        bool rg = false, bu = false;
        std::string got;
        auto shows = [&](int mode, const HitReaction& r) {
            ReactionToShow(mode, r, rg, bu);
            return std::make_pair(rg, bu);
        };
        bool ok = true;
        ok = ok && shows(kHitReactBoth, sg) == std::make_pair(true, false) && shows(kHitReactRagdoll, sg) == std::make_pair(true, false);   // a stagger is a ragdoll
        ok = ok && shows(kHitReactBullet, sg) == std::make_pair(false, true) && shows(kHitReactOff, sg) == std::make_pair(false, false);     // or a flinch in bullet mode, or nothing
        ok = ok && shows(kHitReactBoth, kn) == std::make_pair(true, false) && shows(kHitReactBullet, kn) == std::make_pair(false, true);
        ok = ok && shows(kHitReactBoth, fl) == std::make_pair(false, true) && shows(kHitReactRagdoll, fl) == std::make_pair(false, false);
        ok = ok && shows(kHitReactBoth, HitReaction()) == std::make_pair(false, false);
        Check("W8 melee react: hit_react=both/ragdoll show a stagger as a ragdoll, bullet as a flinch bullet, off nothing; knockdown and flinch as before",
              ok, got);
    }
    {   // 0.4.30: a punch (Stagger) is a clip on the feet, never a ragdoll (live 0.4.29: SET_PED_TO_RAGDOLL type 0 = NMRelax, tasks 389 -> 16 GetUp)
        HitReaction sg, kn;
        sg.kind = ReactKind::Stagger;
        sg.ragdollMs = 350;
        kn.kind = ReactKind::Knockdown;
        bool rg = true, bu = true, cl = false;
        ReactionToShow(kHitReactBoth, sg, rg, bu, cl, true);
        const bool both = cl && !rg && !bu;
        ReactionToShow(kHitReactRagdoll, sg, rg, bu, cl, true);
        const bool rag = cl && !rg && !bu;
        ReactionToShow(kHitReactBullet, sg, rg, bu, cl, true);
        const bool bulletMode = !cl && !rg && bu;                      // bullet mode: still the flinch
        ReactionToShow(kHitReactOff, sg, rg, bu, cl, true);
        const bool off = !cl && !rg && !bu;
        ReactionToShow(kHitReactBoth, sg, rg, bu, cl, false);
        const bool oldWay = !cl && rg && !bu;                           // stagger_clip=0
        ReactionToShow(kHitReactBoth, kn, rg, bu, cl, true);
        const bool kdSame = !cl && rg && !bu;
        Check("0.4.30 punch: a melee stagger is a clip, never a ragdoll (bullet mode a flinch, off nothing; stagger_clip=0 restores the ragdoll); a "
              "knockdown still ragdolls",
              both && rag && bulletMode && off && oldWay && kdSame,
              F("both %d rag %d bullet %d off %d old %d kd %d", both, rag, bulletMode, off, oldWay, kdSame));
    }
    // Item 7: a puppet that GTA killed with a melee takedown reports at most the melee ceiling, not the pool.
    {
        const PeerDamageOut melee0 = ProbePeerDamage(0, 1000, true, true, kDeadMeleeCapDamage);
        const PeerDamageOut gun0 = ProbePeerDamage(0, 1000, true, true);                      // a gun / rocket keeps the pool (a real one-blow kill)
        const PeerDamageOut notMine = ProbePeerDamage(0, 1000, false, true, kDeadMeleeCapDamage);
        const PeerDamageOut partial = ProbePeerDamage(-5, 1000, true, true, kDeadMeleeCapDamage);
        BatchedHit out[16];
        HitBatcher b;
        BatchedHit h;
        h.target = 5;
        h.damage = (uint16_t)melee0.damage;
        b.Add(h, 0);
        const size_t n = b.Flush(60, out, 16);
        Check("W8 item 7: ProbePeerDamage(health 0, pool 1000, ours, dead, melee cap) reports 40 and respawns (was 1000 = 2-punch kills; leaves as ONE hit of 40), a gun kill keeps 1000, "
              "one that was not ours reports nothing",
              melee0.report && melee0.damage == 40 && melee0.respawn && !melee0.restore && gun0.damage == 1000 && !notMine.report && notMine.respawn && partial.damage == 40 &&
                  n == 1 && out[0].damage == 40,
              F("melee %d gun %d n %zu", melee0.damage, gun0.damage, n));
    }

    // Item 4: entering and leaving water are edges (a soft reset each).
    {
        WaterEdge w;
        const auto first = w.Update(true);      // a peer first seen swimming: a baseline, not an edge
        const auto in = w.Update(true);
        const auto out = w.Update(false);
        const auto again = w.Update(true);
        const auto stay = w.Update(true);
        WaterEdge d;
        d.Update(false);
        const auto enter = d.Update(true);
        Check("W8 item 4: WaterEdge: the first reading is a baseline, entering and leaving water are one edge each, staying in is none",
              !first.entered && !first.left && !in.entered && !in.left && !out.entered && out.left && again.entered && !again.left && !stay.entered && !stay.left &&
                  enter.entered && w.InWater(),
              F("first %d%d out %d%d again %d%d", (int)first.entered, (int)first.left, (int)out.entered, (int)out.left, (int)again.entered, (int)again.left));
    }
    // Item 5: the climb window: TASK_CLIMB once on the rising edge, correction suspended until the flag falls plus the settle, then one reset.
    {
        ClimbWindow w;                                   // 300 ms start timeout, 250 ms settle
        const auto idle = w.Update(false, false, 0);
        const auto rise = w.Update(true, false, 1000);
        const auto pend = w.Update(true, false, 1100);
        const auto act = w.Update(true, true, 1150);
        const auto fall = w.Update(false, false, 1500);
        const auto settling = w.Update(false, false, 1700);
        const auto end = w.Update(false, false, 1760);
        const auto after = w.Update(false, false, 1780);
        Check("W8 item 5: climb window: nothing while idle; the rising edge issues TASK_CLIMB once and suspends correction; no second issue, no placement while the task runs; "
              "the flag falling starts a 250 ms settle (still suspended), then exactly one reset",
              !idle.issueClimb && !idle.suspend && rise.issueClimb && rise.suspend && !pend.issueClimb && pend.suspend && !pend.fallbackPlace && act.suspend &&
                  !act.fallbackPlace && !fall.issueClimb && fall.suspend && !fall.reset && settling.suspend && !settling.reset && end.reset && !end.suspend &&
                  !after.reset && !after.suspend && !w.Busy(),
              F("rise %d%d pend %d%d%d fall %d%d settle %d%d end %d%d", (int)rise.issueClimb, (int)rise.suspend, (int)pend.issueClimb, (int)pend.suspend,
                (int)pend.fallbackPlace, (int)fall.suspend, (int)fall.reset, (int)settling.suspend, (int)settling.reset, (int)end.reset, (int)end.suspend));
        ClimbWindow f;
        f.Update(true, false, 0);
        const auto early = f.Update(true, false, 299);
        const auto late = f.Update(true, false, 300);
        const auto hold = f.Update(true, false, 500);
        const auto released = f.Update(false, false, 600);
        const auto done = f.Update(false, false, 900);
        ClimbWindow fail;
        fail.Update(true, false, 0);
        fail.NoteIssueFailed();
        const auto direct = fail.Update(true, false, 10);
        ClimbWindow fl;
        fl.Update(true, true, 0);
        fl.Update(false, false, 100);
        const auto again = fl.Update(true, true, 150);          // the flag rises again inside the settle: the same climb
        const auto stillIn = fl.Update(true, true, 400);
        fl.Update(false, false, 500);
        const auto fin = fl.Update(false, false, 760);
        Check("W8 item 5: the placement fallback only when the task did not start within 300 ms (or could not be issued), kept until the flag falls; a flag that flickers inside the settle is the same climb "
              "(no second TASK_CLIMB), one reset at the end",
              !early.fallbackPlace && late.fallbackPlace && late.suspend && hold.fallbackPlace && !released.fallbackPlace && released.suspend && done.reset && direct.fallbackPlace &&
                  !again.issueClimb && again.suspend && stillIn.suspend && !stillIn.reset && fin.reset,
              F("early %d late %d hold %d released %d done %d direct %d again %d fin %d", (int)early.fallbackPlace, (int)late.fallbackPlace, (int)hold.fallbackPlace,
                (int)released.fallbackPlace, (int)done.reset, (int)direct.fallbackPlace, (int)again.issueClimb, (int)fin.reset));
    }
    // 0d: the puppet goes onto the sender's pose of the edge before TASK_CLIMB / the cover task, and the task windows are counted started-vs-fallback.
    {
        const PrePlace keep = DecidePrePlace(0.05f, 3.0f);
        const PrePlace turn = DecidePrePlace(0.05f, 30.0f);
        const PrePlace nudge = DecidePrePlace(0.5f, 2.0f);
        const PrePlace edgeBelow = DecidePrePlace(0.99f, 0.0f);
        const PrePlace set1 = DecidePrePlace(1.0f, 0.0f);
        const PrePlace far5 = DecidePrePlace(5.0f, 0.0f);
        const PrePlace unreadable = DecidePrePlace(-1.0f, 0.0f);
        Check("0d: pre-vault placement: within 0.12 m the position is left (the heading only when > 8 deg off); 0.12..1 m is a nudge that keeps the task and the heading is set; "
              "1 m and more (or an unreadable ped) is set outright with the old task cleared",
              keep.mode == PrePlaceMode::Keep && !keep.setHeading && !keep.clearTasks && turn.mode == PrePlaceMode::Keep && turn.setHeading &&
                  nudge.mode == PrePlaceMode::Blend && nudge.setHeading && !nudge.clearTasks && edgeBelow.mode == PrePlaceMode::Blend && !edgeBelow.clearTasks &&
                  set1.mode == PrePlaceMode::Set && set1.clearTasks && set1.setHeading && far5.mode == PrePlaceMode::Set && far5.clearTasks &&
                  unreadable.mode == PrePlaceMode::Set && unreadable.clearTasks,
              F("keep %d turn %d%d nudge %d%d below %d set %d%d far %d unreadable %d", (int)keep.mode, (int)turn.mode, (int)turn.setHeading, (int)nudge.mode, (int)nudge.setHeading,
                (int)edgeBelow.mode, (int)set1.mode, (int)set1.clearTasks, (int)far5.mode, (int)unreadable.mode));

        // Three vaults: one starts (the puppet vaults 100 ms after the task), one never starts (300 ms), one cannot be issued. The tally says 3 issued, 1 started, 2 fell back.
        TaskWindowTally tally;
        auto step = [&](ClimbWindow& w, bool flag, bool climbing, double t) {
            const ClimbWindow::Phase before = w.CurrentPhase();
            const ClimbWindow::Out o = w.Update(flag, climbing, t);
            CountClimb(tally, before, w.CurrentPhase(), o.issueClimb);
            return o;
        };
        ClimbWindow a, b, c;
        step(a, true, false, 0);                               // issued
        step(a, true, true, 100);                              // started
        step(a, false, false, 600);
        step(a, false, false, 1000);                           // settle over: ended
        step(b, true, false, 0);                               // issued
        step(b, true, false, 299);
        step(b, true, false, 300);                             // never started: fallback
        step(c, true, false, 0);                               // issued ...
        {
            const ClimbWindow::Phase before = c.CurrentPhase();
            c.NoteIssueFailed();                               // ... but the native could not be called
            CountClimb(tally, before, c.CurrentPhase(), false);
        }
        Check("0d: climb accounting: 3 TASK_CLIMB windows = 3 issued, 1 started (the puppet really vaulted), 2 placement fallbacks (timeout, uncallable), 1 settled to its end",
              tally.issued == 3 && tally.started == 1 && tally.fellBack == 2 && tally.ended == 1,
              F("issued %u started %u fellBack %u ended %u", tally.issued, tally.started, tally.fellBack, tally.ended));
    }
    // 0d cover: the window around TASK_PUT_PED_DIRECTLY_INTO_COVER. The old hold was set when the native was CALLED and kept the controller, the velocity push and the anchor
    // off the ped for as long as the flag stayed up, in cover or not.
    {
        using CP = CoverWindow::Phase;
        CoverWindow w;                                         // 350 ms start timeout, one retry
        const auto idle = w.Update(false, false, 0.0f, 0);
        const auto rise = w.Update(true, false, 2.0f, 1000);
        const auto pend = w.Update(true, false, 2.0f, 1200);
        const auto retry = w.Update(true, false, 2.0f, 1350);    // 350 ms in, still not in cover: the pose moved on, once more
        const auto pend2 = w.Update(true, false, 2.0f, 1500);
        const auto fail = w.Update(true, false, 2.0f, 1700);     // 350 ms after the retry: it never starts
        const auto after1 = w.Update(true, false, 2.0f, 1800);
        const auto after2 = w.Update(true, false, 2.0f, 5000);
        const auto fell = w.Update(false, false, 2.0f, 5100);
        Check("0d cover: the rising flag issues the task once and suspends the controller / velocity push / anchor; after one retry a puppet that never got into cover is RELEASED "
              "(failed once, no more suspend, no more issue) instead of being held frozen for as long as the flag stays up; the flag falling after that is quiet",
              !idle.issue && !idle.suspend && rise.issue && rise.suspend && !pend.issue && pend.suspend && retry.issue && retry.suspend && !pend2.issue && pend2.suspend &&
                  fail.failed && !fail.suspend && !after1.suspend && !after1.issue && !after1.failed && !after2.suspend && !after2.issue && !fell.ended && !fell.suspend &&
                  w.CurrentPhase() == CP::Idle && w.Tally().issued == 2 && w.Tally().started == 0 && w.Tally().fellBack == 1,
              F("rise %d%d retry %d%d fail %d%d after %d%d%d fell %d tally %u/%u/%u", (int)rise.issue, (int)rise.suspend, (int)retry.issue, (int)retry.suspend, (int)fail.failed,
                (int)fail.suspend, (int)after1.suspend, (int)after1.issue, (int)after1.failed, (int)fell.ended, w.Tally().issued, w.Tally().started, w.Tally().fellBack));

        CoverWindow h;
        h.Update(true, false, 0.0f, 0);
        const auto in = h.Update(true, true, 0.3f, 100);         // IS_PED_IN_COVER: started
        bool heldAll = h.CurrentPhase() == CP::Held && in.suspend && !in.issue;
        for (int i = 1; i <= 100; ++i)                           // 3 s in cover, the sender stands still: suspended every frame, no task churn
        {
            const auto o = h.Update(true, true, 0.4f, 100 + i * 30);
            heldAll = heldAll && o.suspend && !o.issue && !o.failed && !o.ended;
        }
        const auto out = h.Update(false, true, 0.4f, 3300);
        const auto out2 = h.Update(false, false, 0.4f, 3330);
        Check("0d cover: a puppet that IS in cover stays suspended frame after frame without a second issue; the flag falling ends the window once (clear the task, Reset the controller)",
              heldAll && out.ended && !out.suspend && !out2.ended && !out2.suspend && h.Tally().started == 1 && h.Tally().ended == 1 && h.Tally().issued == 1,
              F("heldAll %d out %d%d out2 %d%d tally %u/%u/%u", (int)heldAll, (int)out.ended, (int)out.suspend, (int)out2.ended, (int)out2.suspend, h.Tally().issued,
                h.Tally().started, h.Tally().ended));

        CoverWindow l;                                           // the puppet leaves cover by itself, or the sender shuffles away
        l.Update(true, false, 0.0f, 0);
        l.Update(true, true, 0.0f, 50);
        const auto brief = l.Update(true, false, 0.0f, 1100);    // out of cover (first sight) ...
        const auto brief2 = l.Update(true, false, 0.0f, 1499);   // ... 399 ms: not yet
        const auto lost = l.Update(true, false, 0.0f, 1500);     // 400 ms: issue again
        CoverWindow d;
        d.Update(true, false, 0.0f, 0);
        d.Update(true, true, 0.0f, 50);
        const auto near1 = d.Update(true, true, 1.0f, 1100);      // 1 m off: fine
        const auto d1 = d.Update(true, true, 2.0f, 1200);         // 2 m off starts the clock
        const auto d2 = d.Update(true, true, 2.0f, 1799);
        const auto d3 = d.Update(true, true, 2.0f, 1800);         // 600 ms: issue again, from the sender's pose
        CoverWindow g;
        g.Update(true, false, 0.0f, 0);
        g.Update(true, true, 0.0f, 50);
        const auto gap = g.Update(true, true, 5.0f, 400);         // > 4 m: at once, even inside the 1 s gap
        Check("0d cover: leaving cover for 400 ms, or drifting > 1.5 m for 600 ms, issues again (rate-limited to once a second); > 4 m at once",
              !brief.issue && brief.suspend && !brief2.issue && lost.issue && lost.suspend && !near1.issue && !d1.issue && !d2.issue && d3.issue && gap.issue && gap.suspend,
              F("brief %d %d lost %d near %d d %d%d%d gap %d", (int)brief.issue, (int)brief2.issue, (int)lost.issue, (int)near1.issue, (int)d1.issue, (int)d2.issue, (int)d3.issue,
                (int)gap.issue));

        CoverWindow u;                                            // the native could not be called: no suspend, the controller keeps the ped on the pose
        u.Update(true, false, 0.0f, 0);
        u.NoteIssueFailed();
        const auto free1 = u.Update(true, false, 0.0f, 10);
        CoverWindow e;                                            // the sender left cover before the puppet got in: ended once
        e.Update(true, false, 0.0f, 0);
        const auto early = e.Update(false, false, 0.0f, 100);
        Check("0d cover: an uncallable native releases the ped at once; a flag that falls while the task is pending ends the window once",
              free1.suspend == false && !free1.issue && u.CurrentPhase() == CP::Fallback && early.ended && !early.suspend && e.CurrentPhase() == CP::Idle && !e.Busy(),
              F("free %d early %d", (int)free1.suspend, (int)early.ended));
    }
    // Item 8: the aim task is only wanted with a weapon; the alt-tab fallback state.
    {
        Check("W8 item 8: the aim task needs the aiming flag, no ragdoll and a weapon (weapon 0 = none / not reported clears a stale one)",
              AimWanted(proto::kPfAiming, 123) && !AimWanted(proto::kPfAiming, 0) && !AimWanted(0, 123) && !AimWanted(proto::kPfAiming | proto::kPfRagdoll, 123));
        proto::PedState last;
        last.move = proto::kMoveRun;
        last.flags = proto::kPfAiming | proto::kPfShooting | proto::kPfStealth | proto::kPfSwimming | proto::kPfMelee | proto::kPfClimbing | proto::kPfVaulting |
                     proto::kPfJumping | proto::kPfCover | proto::kPfReloading | proto::kPfFalling | proto::kPfLadder;
        last.weapon = 0x1B06D571u;
        last.aimX = 3; last.aimY = 4; last.aimZ = 5;
        last.jumpSeq = 2;
        last.shotSeq = 5;
        last.health = 150;
        last.armor = 20;
        const proto::PedState fb = FallbackPedState(last, true);
        proto::PedState seat;
        seat.move = proto::kMoveInVehicle;
        seat.flags = proto::kPfDriver | proto::kPfAiming;
        seat.weapon = 77;
        seat.aimX = 1; seat.aimY = 1; seat.aimZ = 1;
        const proto::PedState fs = FallbackPedState(seat, true);
        proto::PedState entering;
        entering.move = proto::kMoveEnteringVehicle;
        entering.vehId = 9;
        entering.flags = 0;
        const proto::PedState fe = FallbackPedState(entering, true);
        proto::PedState dead;
        dead.flags = proto::kPfDead | proto::kPfRagdoll | proto::kPfParachute;
        const proto::PedState fd = FallbackPedState(dead, true);
        const proto::PedState none = FallbackPedState(last, false);
        Check("W8 item 8: the alt-tab fallback sends the LAST real state with the transient bits gone (idle, no aim / shoot / melee / jump / climb / vault / ladder / reload / fall, aim point 0) and keeps "
              "weapon, counters, health, armour, stealth, cover, swimming; a seat stays a seat; entering a vehicle becomes idle; dead / ragdoll / parachute stay; with no last state the default",
              fb.move == proto::kMoveIdle && fb.flags == (proto::kPfStealth | proto::kPfSwimming | proto::kPfCover) && fb.weapon == 0x1B06D571u && fb.jumpSeq == 2 && fb.shotSeq == 5 &&
                  fb.health == 150 && fb.armor == 20 && fb.aimX == 0 && fb.aimY == 0 && fb.aimZ == 0 && fs.move == proto::kMoveInVehicle && fs.flags == proto::kPfDriver &&
                  fs.weapon == 77 && fe.move == proto::kMoveIdle && fe.vehId == 0 && fd.flags == (proto::kPfDead | proto::kPfRagdoll | proto::kPfParachute) && none.weapon == 0 &&
                  none.move == proto::kMoveIdle && none.flags == 0 && proto::ValidPedState(fb) && proto::ValidPedState(fs) && proto::ValidPedState(fe) && proto::ValidPedState(fd),
              F("move %d flags %04x weapon %x seat flags %04x entering move %d", fb.move, (unsigned)fb.flags, (unsigned)fb.weapon, (unsigned)fs.flags, fe.move));
    }
    // Item 3 (a): the per-site tally of the locomotion resets.
    {
        SiteTally<4> t;
        t.Add(1);
        t.Add(1);
        t.Add(3);
        t.Add(4);          // out of range: ignored
        t.Add(99);
        Check("W8 item 3: SiteTally counts per site and ignores a site out of range", t.Count(1) == 2 && t.Count(3) == 1 && t.Count(0) == 0 && t.Count(4) == 0 && t.Total() == 3,
              F("1:%u 3:%u total %u", t.Count(1), t.Count(3), t.Total()));
    }
}

// ---- 0.4.4 item 0c: melee hits by the damage event, the receiver's swing trigger ----------------------
static void Test0cMelee()
{
    // The 0.4.3 live case: swings counted, the AI melee modifier at 0 left the pool untouched (poolReport false), but
    // the game DID record the damage event. That is a hit of the nominal damage.
    {
        MeleeHitIn in;
        in.meleeWeaponHeld = true;
        in.eventDamaged = true;
        in.sinceSwingMs = 180;
        in.poolReport = false;
        const MeleeHitOut o = DecideMeleeHit(in);
        Check("0c melee: damage event + swing 180 ms ago + melee weapon in hand + NO pool drop => a Hit of the nominal damage (40), event read cleared",
              o.source == MeleeHitSource::Event && o.damage == kMeleeNominalDamage && kMeleeNominalDamage == 40 && o.clearEvent,
              F("source %d damage %d clear %d", (int)o.source, o.damage, (int)o.clearEvent));
    }
    {
        MeleeHitIn in;
        in.meleeWeaponHeld = true;
        in.eventDamaged = false;
        in.sinceSwingMs = 180;
        const MeleeHitOut o = DecideMeleeHit(in);
        Check("0c melee: a swing with NO damage event and no pool drop is no hit (nothing to clear)",
              o.source == MeleeHitSource::None && o.damage == 0 && !o.clearEvent);
    }
    {
        // A gun in hand: the event path is for melee only; the pool probe stays the gun's detector.
        MeleeHitIn gunEvent;
        gunEvent.meleeWeaponHeld = false;
        gunEvent.eventDamaged = true;
        gunEvent.sinceSwingMs = 100;
        const MeleeHitOut a = DecideMeleeHit(gunEvent);
        MeleeHitIn gunPool = gunEvent;
        gunPool.poolReport = true;
        gunPool.poolDamage = 28;
        const MeleeHitOut b = DecideMeleeHit(gunPool);
        Check("0c melee: with a gun in hand an event alone is no melee hit and clears nothing; a pool drop reports as before (28, by pool)",
              a.source == MeleeHitSource::None && !a.clearEvent && b.source == MeleeHitSource::Pool && b.damage == 28 && !b.clearEvent,
              F("a %d b %d/%d", (int)a.source, (int)b.source, b.damage));
    }
    {
        // Outside the swing window: a stale event is cleared but is not a hit; a melee action still running keeps the window open.
        MeleeHitIn stale;
        stale.meleeWeaponHeld = true;
        stale.eventDamaged = true;
        stale.sinceSwingMs = 5000;
        const MeleeHitOut a = DecideMeleeHit(stale);
        MeleeHitIn action = stale;
        action.meleeActionNow = true;
        const MeleeHitOut b = DecideMeleeHit(action);
        MeleeHitIn edge = stale;
        edge.sinceSwingMs = kMeleeHitWindowMs;
        MeleeHitIn past = stale;
        past.sinceSwingMs = kMeleeHitWindowMs + 1;
        Check("0c melee: an event 5 s after the swing is stale (cleared, no hit); the melee action still running is a hit; the window edge is inclusive",
              a.source == MeleeHitSource::None && a.clearEvent && b.source == MeleeHitSource::Event && b.damage == 40 &&
                  DecideMeleeHit(edge).source == MeleeHitSource::Event && DecideMeleeHit(past).source == MeleeHitSource::None,
              F("a %d b %d", (int)a.source, (int)b.source));
    }
    {
        // The same blow seen by both detectors (a modifier left at 1.0 lets the pool drop too): ONE report, not the sum.
        MeleeHitIn both;
        both.meleeWeaponHeld = true;
        both.eventDamaged = true;
        both.sinceSwingMs = 200;
        both.poolReport = true;
        both.poolDamage = 30;
        const MeleeHitOut o = DecideMeleeHit(both);
        MeleeHitIn poolOnly = both;
        poolOnly.eventDamaged = false;
        poolOnly.sinceSwingMs = 5000;
        const MeleeHitOut p = DecideMeleeHit(poolOnly);
        Check("0c melee: event and pool drop for one blow report once, as the event (40, not 70); a pool drop with no event keeps its own number (30, by pool)",
              o.source == MeleeHitSource::Event && o.damage == 40 && p.source == MeleeHitSource::Pool && p.damage == 30,
              F("o %d/%d p %d/%d", (int)o.source, o.damage, (int)p.source, p.damage));
    }
    {
        // The window gate that keeps the native call out of every other frame.
        Check("0c melee: the event is read only with a melee weapon in hand AND (a swing within the window OR a melee action running)",
              MeleeEventWindow(true, 100, false) && MeleeEventWindow(true, 1e9, true) && !MeleeEventWindow(true, 1e9, false) &&
                  !MeleeEventWindow(false, 100, true) && !MeleeEventWindow(true, kMeleeHitWindowMs + 1, false));
    }
    // Receiver: a rising kPfMelee shows a swing even when the shotSeq step was missed; step and flag are one swing; a gun never swings.
    {
        MeleeSwingGate g;
        const bool flagOnly = PeerSwingDue(g, 1000, 0, true, true);          // no counter step, the flag rises
        const bool flagHeld = PeerSwingDue(g, 1033, 0, true, true);
        const bool flagDown = PeerSwingDue(g, 1500, 0, true, false);
        MeleeSwingGate h;
        const bool step = PeerSwingDue(h, 2000, 1, true, false);             // the counter steps first ...
        const bool flagAfter = PeerSwingDue(h, 2100, 0, true, true);         // ... the flag rises 100 ms later: the same swing
        MeleeSwingGate k;
        const bool gunFlag = PeerSwingDue(k, 3000, 1, false, true);          // a pistol-whip with a gun in hand: shots, not a swing
        MeleeSwingGate m;
        PeerSwingDue(m, 4000, 1, true, true);
        const bool second = PeerSwingDue(m, 4600, 1, true, true);            // 600 ms later, a new step: a second swing
        Check("0c melee: receiver swing = a shotSeq step OR a rising kPfMelee (flag alone shows it, step + flag 100 ms apart is one, a gun never swings, a second swing shows)",
              flagOnly && !flagHeld && !flagDown && step && !flagAfter && !gunFlag && second,
              F("flagOnly %d held %d down %d step %d after %d gun %d second %d", flagOnly, flagHeld, flagDown, step, flagAfter, gunFlag, second));
    }
}


// ---- 0.4.5 melee audit: the client's whole melee pipeline on scripted fights (tests/melee_sim.h) -------------------------
// What the engine does per frame (TickLocalMelee + the melee part of ProbePeerHits + the Hit flush), fed a script of
// presses, blows and pool drops on a simulated clock at 62.5 frames a second. Asserts what the owner's three complaints are
// about: a legit blow is reported exactly ONCE (never zero, never two), within the nominal damage band, a swing that
// missed is never reported, and the guns' path is the same as before.
namespace {
    struct FightBlow { double t; int pool; bool event; };
    struct FightScript
    {
        std::vector<double> presses;     // the melee control pressed (the frame containing t)
        std::vector<FightBlow> blows;    // a blow lands on the puppet at t: pool drop and whether GTA records the damage event
        double actionMs = 350.0;         // IS_PED_PERFORMING_MELEE_ACTION stays true this long after a press (0 = never reads true)
    };
    void RunFight(meleesim::Client& c, const FightScript& s, double endMs)
    {
        std::size_t pi = 0, bi = 0;
        for (double t = 0.0; t <= endMs; t += 16.0)
        {
            bool press = false, action = false;
            while (pi < s.presses.size() && s.presses[pi] <= t)
            {
                press = true;
                ++pi;
            }
            for (double p : s.presses)
                if (s.actionMs > 0.0 && t >= p && t < p + s.actionMs)
                    action = true;
            while (bi < s.blows.size() && s.blows[bi].t <= t)
            {
                c.pup.Blow(s.blows[bi].pool, s.blows[bi].event);
                ++bi;
            }
            c.Frame(t, press, action);
        }
    }
    int SumClaimed(const meleesim::Client& c)
    {
        int n = 0;
        for (const meleesim::Sent& x : c.sent) n += x.hit.damage;
        return n;
    }
}

static void Test05MeleeSim()
{
    // one punch; the AI melee modifier at 0 leaves the pool alone (the 0.4.3 live case), and with it live the pool drops too
    {
        meleesim::Client a, b;
        FightScript s0;
        s0.presses = { 200 };
        s0.blows = { { 520, 0, true } };
        RunFight(a, s0, 2000);
        FightScript s1;
        s1.presses = { 200 };
        s1.blows = { { 520, 30, true } };
        RunFight(b, s1, 2000);
        Check("0.4.5 sim: one punch is ONE Hit of the nominal 40 (shotSeq 1, left within 100 ms of the blow), whether the pool did not move (modifier 0) or dropped 30 "
              "(one report, not 70); the puppet's pool is back at 1000",
              a.sent.size() == 1 && a.sent[0].hit.damage == 40 && a.sent[0].hit.shotSeq == 1 && a.sent[0].atMs - 520 <= 100 &&
                  b.sent.size() == 1 && b.sent[0].hit.damage == 40 && b.pup.health == 1000 && a.hitsEvent == 1 && b.hitsEvent == 1 && b.hitsPool == 0,
              F("modifier 0: %zu hits (%d), live: %zu hits (%d), pool %d", a.sent.size(), a.sent.empty() ? 0 : a.sent[0].hit.damage, b.sent.size(),
                b.sent.empty() ? 0 : b.sent[0].hit.damage, b.pup.health));
    }
    // a combo: three punches 500 ms apart, each lands 300 ms after its press
    {
        meleesim::Client c;
        FightScript s;
        s.presses = { 100, 600, 1100 };
        s.blows = { { 400, 0, true }, { 900, 0, true }, { 1400, 0, true } };
        RunFight(c, s, 2500);
        bool seqs = c.sent.size() == 3;
        for (std::size_t i = 0; seqs && i < 3; ++i)
            seqs = c.sent[i].hit.shotSeq == i + 1 && c.sent[i].hit.damage == 40 && c.sent[i].atMs - s.blows[i].t <= 100;
        Check("0.4.5 sim: a combo of 3 punches 0.5 s apart is exactly 3 Hits of 40 on shotSeq 1, 2, 3, each within 100 ms of its blow (120 claimed, not 0, not 240)",
              seqs && SumClaimed(c) == 120 && c.swings == 3, F("%zu hits, %d claimed, %u swings", c.sent.size(), SumClaimed(c), c.swings));
    }
    // a swing that misses, then one that hits
    {
        meleesim::Client c;
        FightScript s;
        s.presses = { 100, 900 };
        s.blows = { { 1200, 0, true } };
        RunFight(c, s, 2500);
        Check("0.4.5 sim: a swing that misses (no blow) reports nothing; the next swing that hits is one Hit, on its own shotSeq (2)",
              c.sent.size() == 1 && c.sent[0].hit.shotSeq == 2 && c.sent[0].hit.damage == 40, F("%zu hits", c.sent.size()));
    }
    // THE STALE EVENT: a blow lands outside every window (1.3 s after the press) and leaves its damage event standing; a later swing
    // that misses must not be reported as a hit on its first frame
    {
        meleesim::Client c;
        FightScript s;
        s.presses = { 100, 3000 };
        s.blows = { { 1400, 0, true } };               // 1.3 s after the press: outside the 900 ms window, the pool did not move
        RunFight(c, s, 4500);
        Check("0.4.5 sim: a blow outside every swing window leaves its damage event standing; the NEXT swing, a miss, is not reported as a hit (ghost hit on its first "
              "frame); the stale event is cleared and counted",
              c.sent.empty() && c.stale == 1 && !c.pup.event, F("%zu hits, %u stale cleared, event left %d", c.sent.size(), c.stale, (int)c.pup.event));
    }
    // the window edges: a blow 780 ms after the press (inside the 900 ms window) is a hit, one 950 ms after is not and leaves no ghost behind
    {
        meleesim::Client inside, outside;
        FightScript a;
        a.presses = { 100 };
        a.blows = { { 880, 0, true } };
        a.actionMs = 0;                                // the action native may read false: the window alone must carry it
        RunFight(inside, a, 2500);
        FightScript b;
        b.presses = { 100, 3000 };
        b.blows = { { 1050, 0, true } };
        b.actionMs = 0;
        RunFight(outside, b, 4500);
        Check("0.4.5 sim: the swing window edge: a blow 780 ms after the press is one Hit, one 950 ms after is none and does not fire on the next swing's first frame either "
              "(the action flag reading false does not matter)",
              inside.sent.size() == 1 && outside.sent.empty(), F("inside %zu, outside %zu", inside.sent.size(), outside.sent.size()));
    }
    // the guns are not touched by any of it: the pool probe reports every shot once; the event is never read with a gun in hand
    {
        meleesim::Client c;
        c.held = false;
        c.weapon = meleesim::kPistolHash;
        for (int i = 0; i < 5; ++i)                    // 5 shots 120 ms apart, each takes 30 off the pool
        {
            const double t = 100.0 + 120.0 * i;
            for (double f = t - 8.0; f <= t + 140.0; f += 16.0)
            {
                if (f < t)
                    continue;
                if (f - t < 16.0)
                {
                    c.Shot();
                    c.pup.Blow(30, true);
                }
                c.Frame(f, false, false);
            }
        }
        for (double f = 900.0; f < 1500.0; f += 16.0) c.Frame(f, false, false);
        bool all30 = c.sent.size() == 5;
        for (const meleesim::Sent& x : c.sent) all30 = all30 && x.hit.damage == 30 && x.hit.weapon == meleesim::kPistolHash;
        meleesim::Client burst;                        // 6 shots 30 ms apart: the batcher merges them, no damage is lost or doubled
        burst.held = false;
        burst.weapon = meleesim::kPistolHash;
        double t = 100.0;
        int shots = 0;
        for (double f = 0.0; f < 1200.0; f += 16.0)
        {
            if (shots < 6 && f >= t)
            {
                burst.Shot();
                burst.pup.Blow(30, true);
                ++shots;
                t += 32.0;
            }
            burst.Frame(f, false, false);
        }
        Check("0.4.5 sim: guns: 5 shots 120 ms apart are 5 Hits of exactly 30 each by the pool drop, the damage event is never read (no melee window); a 6-shot burst 32 ms "
              "apart is merged by the batcher into Hits that add up to exactly 180",
              all30 && c.eventReads == 0 && c.hitsEvent == 0 && SumClaimed(burst) == 180 && burst.eventReads == 0,
              F("%zu hits, event reads %u, burst claimed %d in %zu hits", c.sent.size(), c.eventReads, SumClaimed(burst), burst.sent.size()));
    }
    // the event gate itself
    {
        MeleeEventGate g;
        using S = MeleeEventGate::Step;
        const S a = g.Tick(false), b = g.Tick(true), c = g.Tick(true), d = g.Tick(true), e = g.Tick(false), f = g.Tick(true);
        Check("0.4.5 sim: MeleeEventGate: closed does nothing, the first open frame clears the stale event, later frames of that window read, a closed gap and a reopen clears again "
              "(a combo whose window never closes keeps reading)",
              a == S::Closed && b == S::ClearStale && c == S::Read && d == S::Read && e == S::Closed && f == S::ClearStale,
              F("%d %d %d %d %d %d", (int)a, (int)b, (int)c, (int)d, (int)e, (int)f));
    }
    // client and server agree on how long a swing counts
    {
        const double need = kMeleeHitWindowMs + HitBatchConfig().holdMs + 33.0 /* one script tick of flush */ + 300.0 /* link and jitter */;
        Check("0.4.5 sim: the server's melee swing window covers the client's swing window plus the batcher hold, a tick and 300 ms of link (the 500 ms shot window did not: "
              "a blow later than ~450 ms after the press was refused)",
              (double)gage::server::ac::kMeleeShotWindowMs >= need && gage::server::ac::ShotWindowFor(gage::server::ac::WeaponKind::Melee, 500) >= need &&
                  gage::server::ac::ShotWindowFor(gage::server::ac::WeaponKind::Handgun, 500) == 500 && gage::server::ac::ShotWindowFor(gage::server::ac::WeaponKind::Melee, 0) == 0 &&
                  gage::server::ac::ShotWindowFor(gage::server::ac::WeaponKind::Melee, 3000) == 3000,
              F("need %.0f ms, window %u ms", need, (unsigned)gage::server::ac::kMeleeShotWindowMs));
    }
}

// ---- W9 (docs/sync-specs/live-0.4.2-fixes.md section W9): passengers, seats, leaving a moving vehicle, impacts ----
static void TestW9()
{
    // item 1: the door walk ends when the sender stops entering
    {
        const bool released = EnterWarpDue(true, false, 300, false);        // the sender is seated (its move is no longer entering)
        const bool walking = EnterWarpDue(true, false, 300, true);          // it is still walking in: the walk plays
        const bool slow = EnterWarpDue(true, false, 2500, true);            // a walk that long gives way anyway
        const bool notListed = EnterWarpDue(false, false, 300, false);      // the server does not list the seat yet
        const bool inside = EnterWarpDue(true, true, 300, false);           // already inside here
        const bool noTask = EnterWarpDue(true, false, -1, true);            // no enter task: at once, as before
        Check("W9 item 1: the seat warp is due as soon as the sender's move is no longer entering and the server lists the seat (300 ms, not 2.5 s); still entering waits; not listed / already inside never",
              released && !walking && slow && !notListed && !inside && noTask,
              F("released %d walking %d slow %d notListed %d inside %d noTask %d", released, walking, slow, notListed, inside, noTask));
    }
    // item 2: a ped GTA keeps moving out of its seat is not re-warped every 100 ms
    {
        SeatRewarpLimiter lim;
        int warps = 0, naive = 0;
        for (double t = 0; t < 10000; t += 100.0)
        {
            ++naive;                                                        // the old loop: every retry warps
            if (lim.Allow(t, 7, 1, t > 0))
                ++warps;
        }
        const uint32_t held = lim.Suppressed();
        SeatRewarpLimiter fresh;
        const bool first = fresh.Allow(0, 7, 1, false);                     // the first warp of a visit
        const bool again = fresh.Allow(100, 7, 1, true);                    // 100 ms later, shuffled: held back
        const bool other = fresh.Allow(150, 7, 2, true);                    // a different seat (the server moved it): at once
        const bool otherVeh = fresh.Allow(160, 8, 2, true);                 // a different vehicle: at once
        const bool afterGap = fresh.Allow(160 + 400, 8, 2, true);           // the gap passed: allowed ...
        const bool doubled = fresh.Allow(160 + 400 + 500, 8, 2, true);      // ... then the gap doubled (800): 500 ms is too soon
        const bool calm = fresh.Allow(160 + 400 + 500 + 6000, 8, 2, true);  // 6 s of quiet: starts over
        Check("W9 item 2: 100 ms retries of a shuffled ped give at most 6 re-warps in 10 s (100 before); a seat or vehicle change is not held back; the gap doubles and starts over after 5 s of calm",
              warps <= 6 && warps >= 3 && naive == 100 && held == (uint32_t)(100 - warps) && first && !again && other && otherVeh && afterGap &&
                  !doubled && calm,
              F("warps %d of %d, held %u; %d %d %d %d %d %d %d", warps, naive, (unsigned)held, first, again, other, otherVeh, afterGap, doubled, calm));
    }
    // item 3: the F press and the controls
    {
        EntryPressGuard g;
        const bool before = g.DisableEnter(0);
        g.OnPress(1000);
        const bool walking = g.DisableEnter(1500);                          // the walk to the door: still ours
        g.OnSeated(2000);
        const bool at300 = g.DisableEnter(2300), at499 = g.DisableEnter(2499), at500 = g.DisableEnter(2500);
        const bool quick = g.NoteExit(2800);                                // an exit 800 ms after the entry
        const bool second = g.NoteExit(2900);                               // nothing seated now
        EntryPressGuard h;
        h.OnPress(0);
        h.OnCancel();
        const bool cancelled = h.DisableEnter(10);
        h.OnPress(100);
        h.OnSeated(200);
        const bool slow = h.NoteExit(200 + kQuickExitMs + 1);              // out much later: not a quick exit
        Check("W9 item 3: control 23 is disabled from the handled press until 500 ms after the ped sat; a cancelled walk releases it; a quick exit after an entry is counted (once), a late one is not",
              !before && walking && at300 && at499 && !at500 && quick && !second && !cancelled && !slow && g.Presses() == 1 &&
                  g.QuickExits() == 1 && h.QuickExits() == 0 && h.Presses() == 2,
              F("%d %d %d %d %d quick %d %d cancelled %d slow %d presses %u/%u quick %u/%u", before, walking, at300, at499, at500, quick, second,
                cancelled, slow, g.Presses(), h.Presses(), g.QuickExits(), h.QuickExits()));
    }
    // item 4: the passenger's engine
    {
        EngineReassert e;
        const bool a0 = e.Due(0, true, true, true);                         // first sight: assert
        const bool a500 = e.Due(500, true, true, true);
        const bool a1000 = e.Due(1000, true, true, true);                   // ~1 s
        const bool stopped = e.Due(1100, true, true, false);                // the car says it is not running: not yet (250 ms)
        const bool stopped2 = e.Due(1260, true, true, false);               // ... and sooner than a second
        const bool serverOff = e.Due(3000, true, false, false);             // the server says off: never forced on
        const bool driver = e.Due(3100, false, true, true);                 // not a passenger of a remote-driven car
        const bool again = e.Due(3200, true, true, true);                   // a new ride asserts at once
        Check("W9 item 4: the engine of a car the local ped rides in is asserted at once, then every 1 s (250 ms while it reads not running); never when the server says off or when not a passenger",
              a0 && !a500 && a1000 && !stopped && stopped2 && !serverOff && !driver && again && e.Asserts() == 4,
              F("%d %d %d %d %d %d %d %d asserts %u", a0, a500, a1000, stopped, stopped2, serverOff, driver, again, e.Asserts()));
    }
    // item 5: leaving a moving vehicle
    {
        UnseatIn in;
        in.haveVehicle = true;
        in.vehSpeedMps = 0.4f;
        const UnseatPlan still = DecideUnseat(in);                           // a parked car: the animated climb-out
        in.vehSpeedMps = 12.0f;
        const UnseatPlan moving = DecideUnseat(in);                          // 12 m/s: bail out with the car's velocity
        in.vehSpeedMps = 1.0f;
        in.senderFlags = proto::kPfFalling;
        const UnseatPlan fell = DecideUnseat(in);                            // slow car, but the sender's first sample is falling
        in.senderFlags = proto::kPfRagdoll;
        const UnseatPlan rag = DecideUnseat(in);                             // ... or ragdolling: no pause, the ragdoll edge takes it
        in.senderFlags = proto::kPfJumping;
        const UnseatPlan jumped = DecideUnseat(in);
        in.senderFlags = 0;
        in.vehSpeedMps = 3.0f;
        const UnseatPlan edge = DecideUnseat(in);                            // exactly 3 m/s is not above the threshold
        UnseatIn gone;
        gone.vehSpeedMps = 30.0f;
        const UnseatPlan none = DecideUnseat(gone);                          // no vehicle here: the pop
        in.vehSpeedMps = NAN;
        const UnseatPlan nan = DecideUnseat(in);
        Check("W9 item 5: a parked car is left with flag 0 (1.5 s pause); above 3 m/s, or with a falling / ragdolling / jumping sender, it is a bail-out (4160, fallback 16, velocity carried, 600 ms pause, none when ragdolling); no vehicle = clear",
              still.mode == UnseatMode::Leave && still.leaveFlags == 0 && !still.carryVelocity && still.pauseMs == kLeavePauseMs &&
                  moving.mode == UnseatMode::Bail && moving.leaveFlags == 4160 && moving.fallbackFlags == 16 && moving.carryVelocity &&
                  moving.pauseMs == kBailPauseMs && fell.mode == UnseatMode::Bail && rag.mode == UnseatMode::Bail && rag.pauseMs == 0.0 &&
                  jumped.mode == UnseatMode::Bail && edge.mode == UnseatMode::Leave && none.mode == UnseatMode::Clear &&
                  nan.mode == UnseatMode::Leave,
              F("%d %d %d %d %d %d %d %d", (int)still.mode, (int)moving.mode, (int)fell.mode, (int)rag.mode, (int)jumped.mode, (int)edge.mode,
                (int)none.mode, (int)nan.mode));
        Check("W9 item 5: the carried velocity fades linearly over 300 ms; the local forced exit bails out of a moving car on the first try and warps on a retry or when slow",
              CarryVelocityScale(0) == 1.0f && std::fabs(CarryVelocityScale(150) - 0.5f) < 1e-6 && CarryVelocityScale(300) == 0.0f &&
                  CarryVelocityScale(-1) == 0.0f && CarryVelocityScale(NAN) == 0.0f && LocalLeaveFlags(15.0f, false) == 4160 &&
                  LocalLeaveFlags(15.0f, true) == 16 && LocalLeaveFlags(1.0f, false) == 16 && LocalLeaveFlags(NAN, false) == 16);
    }
    // item 6: a puppet run over by the local player's vehicle
    {
        ImpactIn in;
        in.vehSpeedMps = 15.0f;
        in.damagedByVehicle = true;
        const ImpactOut hit = DecideVehicleImpact(in);
        ImpactIn slow = in;
        slow.vehSpeedMps = 4.0f;
        ImpactIn noEvent = in;
        noEvent.damagedByVehicle = false;
        ImpactIn seated = in;
        seated.puppetSeated = true;
        ImpactIn dead = in;
        dead.puppetDead = true;
        ImpactIn again = in;
        again.sinceLastImpactMs = 500;
        ImpactIn fast = in;
        fast.vehSpeedMps = 60.0f;
        const ImpactOut capped = DecideVehicleImpact(fast);
        Check("W9 item 6: a puppet damaged by the local player's vehicle above 5 m/s ragdolls (800..2500 ms, thrown with half the car's velocity); a slow car, no damage event, a seated or dead puppet, or a second impact within 1.5 s does not",
              hit.ragdoll && hit.ragdollMs >= 800 && hit.ragdollMs <= 2500 && hit.velocityScale == 0.5f && !DecideVehicleImpact(slow).ragdoll &&
                  !DecideVehicleImpact(noEvent).ragdoll && !DecideVehicleImpact(seated).ragdoll && !DecideVehicleImpact(dead).ragdoll &&
                  !DecideVehicleImpact(again).ragdoll && capped.ragdoll && capped.ragdollMs == 2500,
              F("%d %d ms, capped %d ms", hit.ragdoll, hit.ragdollMs, capped.ragdollMs));
    }
    // item 7: bike classes
    {
        Check("W9 item 7: GET_VEHICLE_CLASS 8 (motorcycles) and 13 (cycles) are bikes; cars, boats and the rest are not",
              IsBikeClass(8) && IsBikeClass(13) && !IsBikeClass(0) && !IsBikeClass(7) && !IsBikeClass(14) && !IsBikeClass(-1));
    }
}


// ---- W10: player name tags (libs/sync/nametag.h) ----

// A head at distM from the camera, projected to the middle of the screen, with a fresh LOS answer `clear` recorded at `nowMs`.
static TagFrame TagAt(double nowMs, float distM, double dtMs = 16.7)
{
    TagFrame f;
    f.nowMs = nowMs;
    f.dtMs = dtMs;
    f.placed = true;
    f.projOk = true;
    f.sx = 0.5f;
    f.sy = 0.4f;
    f.distM = distM;
    return f;
}

static bool Utf8Valid(const std::string& s)
{
    for (std::size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        const std::size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

static std::size_t CodePoints(const std::string& s)
{
    std::size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

static void TestW10()
{
    const NameTagConfig cfg;
    // fade and scale
    {
        Check("W10 fade: full alpha up to 25 m, linear to 0 at 40 m (32.5 m is half), 0 beyond, and 0 for a NaN or negative distance",
              DistanceAlpha(0.0f) == 1.0f && DistanceAlpha(25.0f) == 1.0f && std::fabs(DistanceAlpha(32.5f) - 0.5f) < 1e-5f &&
                  DistanceAlpha(40.0f) == 0.0f && DistanceAlpha(75.0f) == 0.0f && DistanceAlpha(std::nanf("")) == 0.0f &&
                  DistanceAlpha(-3.0f) == 0.0f,
              F("%.3f %.3f %.3f", DistanceAlpha(25.0f), DistanceAlpha(32.5f), DistanceAlpha(40.0f)));
        bool inLimits = true, nonIncreasing = true;
        float prev = 2.0f;
        for (float d = 0.0f; d <= 80.0f; d += 0.5f)
        {
            const float sc = DistanceScale(d);
            inLimits = inLimits && sc <= 1.0f && sc >= cfg.scaleFar;
            nonIncreasing = nonIncreasing && sc <= prev;
            prev = sc;
        }
        Check("W10 scale: 1.0 up to 8 m, 0.76 at 40 m and beyond, never above 1 or below 0.76 and never growing with distance; a NaN gets the smallest",
              inLimits && nonIncreasing && DistanceScale(8.0f) == 1.0f && std::fabs(DistanceScale(24.0f) - 0.88f) < 1e-5f &&
                  DistanceScale(40.0f) == cfg.scaleFar && DistanceScale(std::nanf("")) == cfg.scaleFar);
    }
    // projection
    {
        Check("W10 projection: the native's false (behind the camera, off screen), NaN / inf, and a point well outside the screen are unusable; the "
              "middle, the edges and a point just outside (the text has width) are usable",
              !ProjectionUsable(false, 0.5f, 0.5f) && !ProjectionUsable(true, std::nanf(""), 0.5f) &&
                  !ProjectionUsable(true, 0.5f, INFINITY) && !ProjectionUsable(true, -0.2f, 0.5f) && !ProjectionUsable(true, 1.2f, 0.5f) &&
                  !ProjectionUsable(true, 0.5f, -0.1f) && !ProjectionUsable(true, 0.5f, 1.1f) && ProjectionUsable(true, 0.5f, 0.4f) &&
                  ProjectionUsable(true, 0.0f, 1.0f) && ProjectionUsable(true, -0.03f, 0.5f) && ProjectionUsable(true, 1.03f, -0.02f));
    }
    // LOS cache
    {
        LosCache c;
        const bool pendingHidden = !c.Visible(0.0) && c.Due(0.0);
        c.Record(1000.0, true);
        const bool notDue = !c.Due(1100.0) && !c.Due(1199.0), due = c.Due(1200.0);
        const bool freshClear = c.Visible(1100.0) && c.Visible(2000.0);   // reused for up to losStaleMs
        const bool stale = !c.Visible(2001.0) && !c.Visible(9000.0);       // then a missing answer reads hidden (fail closed)
        c.Record(3000.0, false);
        const bool blocked = !c.Visible(3010.0);
        c.Record(4000.0, true);
        const bool backwards = c.Due(3000.0) && !c.Visible(3000.0);       // a clock that went back asks again and trusts nothing
        c.Reset();
        const bool reset = !c.Have() && c.Due(4000.0) && !c.Visible(4000.0);
        Check("W10 occlusion cache: no answer yet reads hidden and is due; a fresh answer is reused for 200 ms without asking, kept for 1 s, then "
              "reads hidden (never a guess); a blocked answer hides; a clock that went backwards asks again; Reset forgets",
              pendingHidden && notDue && due && freshClear && stale && blocked && backwards && reset,
              F("%d %d %d %d %d %d %d %d", pendingHidden, notDue, due, freshClear, stale, blocked, backwards, reset));
    }
    {
        // 60 fps for 10 s with the engine's loop (ask when Due): 5 Hz per peer, not 60.
        LosCache c;
        int asks = 0;
        for (double t = 0.0; t < 10000.0; t += 1000.0 / 60.0)
            if (c.Due(t))
            {
                ++asks;
                c.Record(t, true);
            }
        LosCache slow;     // 20 fps: every frame is later than 200 ms apart? no, 50 ms apart: still 5 Hz
        int slowAsks = 0;
        for (double t = 0.0; t < 10000.0; t += 50.0)
            if (slow.Due(t))
            {
                ++slowAsks;
                slow.Record(t, true);
            }
        Check("W10 occlusion rate: over 10 s at 60 fps and at 20 fps one peer is asked about 50 times (5 Hz), not once per frame",
              asks >= 48 && asks <= 52 && slowAsks >= 48 && slowAsks <= 52, F("%d, %d", asks, slowAsks));
    }
    // the eased alpha
    {
        NameTagTrack t;
        t.los.Record(0.0, true);
        TagOut first = t.Step(TagAt(16.7, 10.0f));
        TagOut mid;
        for (int i = 1; i < 5; ++i) mid = t.Step(TagAt(16.7 * (i + 1), 10.0f));
        TagOut full;
        for (int i = 5; i < 14; ++i) { t.los.Record(16.7 * (i + 1), true); full = t.Step(TagAt(16.7 * (i + 1), 10.0f)); }
        const bool fadeIn = first.alpha > 0.0f && first.alpha < 0.2f && mid.alpha > first.alpha && mid.alpha < 0.8f && full.alpha == 1.0f;
        // blocked from now on: it does not vanish in one frame, and it is gone within ~110 ms
        t.los.Record(300.0, false);
        TagOut o1 = t.Step(TagAt(316.7, 10.0f));
        TagOut o;
        double now = 316.7;
        int framesToGone = 1;
        for (; o1.draw && framesToGone < 20; ++framesToGone) { now += 16.7; o1 = t.Step(TagAt(now, 10.0f)); o = o1; }
        const TagOut afterOne = [&] { NameTagTrack u; u.los.Record(0.0, true); for (int i = 0; i < 12; ++i) u.Step(TagAt(16.7 * (i + 1), 10.0f));
                                       u.los.Record(300.0, false); return u.Step(TagAt(316.7, 10.0f)); }();
        Check("W10 fade: a tag eases in over ~160 ms (never pops to full) and out over ~110 ms when the line of sight breaks (no frame-sized flash)",
              fadeIn && afterOne.alpha > 0.8f && afterOne.draw && framesToGone >= 5 && framesToGone <= 9 && !o1.draw,
              F("in %.2f/%.2f/%.2f, after one frame %.2f, gone after %d frames", first.alpha, mid.alpha, full.alpha, afterOne.alpha, framesToGone));
    }
    // hidden rules
    {
        // Each case: a tag that has been fully visible for a second, then ONE thing changes and it must be gone within 300 ms.
        auto goneAfter = [&](auto change) {
            NameTagTrack t;
            double now = 0.0;
            for (int i = 0; i < 60; ++i) { now += 16.7; t.los.Record(now, true); t.Step(TagAt(now, 12.0f)); }
            if (!t.Step(TagAt(now + 16.7, 12.0f)).draw) return -1;      // the premise: it was shown
            int frames = 0;
            TagOut o;
            for (; frames < 40; ++frames)
            {
                now += 16.7;
                TagFrame f = TagAt(now, 12.0f);
                change(f, t, now);
                o = t.Step(f);
                if (!o.draw) break;
            }
            return frames;
        };
        const int dead = goneAfter([](TagFrame& f, NameTagTrack&, double) { f.dead = true; });
        const int unplaced = goneAfter([](TagFrame& f, NameTagTrack&, double) { f.placed = false; });
        const int far = goneAfter([](TagFrame& f, NameTagTrack&, double) { f.distM = 41.0f; });
        const int offscreen = goneAfter([](TagFrame& f, NameTagTrack&, double) { f.projOk = false; });
        const int behindWall = goneAfter([](TagFrame&, NameTagTrack& t, double now) { t.los.Record(now, false); });
        const auto ok = [](int n) { return n >= 1 && n <= 18; };    // 18 frames = 300 ms
        Check("W10 hidden: a dead peer, a missing ped, a peer beyond 40 m, one behind the camera / off screen and one behind a wall each lose their tag "
              "within 300 ms (and not in zero frames)",
              ok(dead) && ok(unplaced) && ok(far) && ok(offscreen) && ok(behindWall),
              F("dead %d, unplaced %d, far %d, offscreen %d, wall %d frames", dead, unplaced, far, offscreen, behindWall));
        Check("W10 hidden: a peer with no line-of-sight answer yet never shows (the engine asks in the same frame, so the wait is zero frames)",
              [&] { NameTagTrack t; bool shown = false; for (int i = 1; i <= 30; ++i) shown = shown || t.Step(TagAt(16.7 * i, 10.0f)).draw; return !shown; }());
        Check("W10 hidden: an answer older than 1 s is not trusted -- the checks stopped, so the tag fades out instead of hanging through a wall",
              [&] {
                  NameTagTrack t;
                  double now = 0.0;
                  t.los.Record(0.0, true);
                  for (int i = 0; i < 20; ++i) { now += 16.7; t.Step(TagAt(now, 10.0f)); }
                  const bool shownWhileFresh = t.Step(TagAt(now + 16.7, 10.0f)).draw;
                  TagOut o;
                  for (now = 1100.0; now < 1400.0; now += 16.7) o = t.Step(TagAt(now, 10.0f));    // no new Record since t=0
                  return shownWhileFresh && !o.draw;
              }());
    }
    // walking away and the wall, end to end
    {
        NameTagTrack t;
        double now = 0.0;
        float prevAlpha = 2.0f, prevScale = 2.0f;
        bool mono = true, fullTo25 = true, goneAt40 = true, scaleLimits = true;
        // 5 m -> 50 m at 5 m/s, 60 fps, LOS asked when due (the engine's loop)
        for (float d = 5.0f; d <= 50.0f; d += 5.0f / 60.0f)
        {
            now += 1000.0 / 60.0;
            if (t.los.Due(now)) t.los.Record(now, true);
            const TagOut o = t.Step(TagAt(now, d, 1000.0 / 60.0));
            if (d > 1.0f && d < 25.0f && now > 400.0 && o.alpha < 0.999f) fullTo25 = false;
            if (d >= 25.0f) { mono = mono && o.alpha <= prevAlpha + 1e-4f; prevAlpha = o.alpha; }
            if (d >= 40.0f && o.draw) goneAt40 = false;
            if (o.draw) { scaleLimits = scaleLimits && o.scale <= 1.0f && o.scale >= cfg.scaleFar && o.scale <= prevScale + 1e-4f; prevScale = o.scale; }
        }
        Check("W10 walk-away 5 m -> 50 m at 60 fps: full alpha up to 25 m, then only ever dimmer, gone at 40 m, the scale shrinks within 1.0..0.76",
              fullTo25 && mono && goneAt40 && scaleLimits, F("full %d mono %d gone %d scale %d", fullTo25, mono, goneAt40, scaleLimits));
    }
    {
        // A wall appears at t = 2 s: the answers are asked at 5 Hz, so the tag must be hidden within 200 ms (next ask) + 110 ms (fade) + a frame.
        NameTagTrack t;
        double now = 0.0, hiddenAt = -1.0;
        for (int i = 0; i < 300; ++i)
        {
            now += 1000.0 / 60.0;
            if (t.los.Due(now)) t.los.Record(now, now < 2000.0);
            const TagOut o = t.Step(TagAt(now, 15.0f, 1000.0 / 60.0));
            if (now >= 2000.0 && !o.draw && hiddenAt < 0.0) hiddenAt = now - 2000.0;
        }
        Check("W10 occlusion latency: a wall that appears hides the tag within 350 ms with 5 Hz checks, and it stays hidden",
              hiddenAt > 0.0 && hiddenAt <= 350.0, F("%.0f ms", hiddenAt));
    }
    // candidates
    {
        Check("W10 candidates: a puppet that is driven, alive and within 40 m is worth the natives; not driven, dead, or beyond 40 m is not",
              NameTagTrack::Worth(true, false, 0.0f) && NameTagTrack::Worth(true, false, 40.0f) && !NameTagTrack::Worth(false, false, 5.0f) &&
                  !NameTagTrack::Worth(true, true, 5.0f) && !NameTagTrack::Worth(true, false, 40.1f) && !NameTagTrack::Worth(true, false, -1.0f));
    }
    // text
    {
        const TagText a = FormatTag(7, "Alex"), b = FormatTag(123, "Mara_K.Smith-2");
        Check("W10 text: the tag reads \"[id] Name\" (\"[7] Alex\", \"[123] Mara_K.Smith-2\")", a.Full() == "[7] Alex" && b.Full() == "[123] Mara_K.Smith-2",
              a.Full() + " / " + b.Full());
        const std::string exact(24, 'a'), longer(40, 'b');
        const std::string cut = TruncateName(longer.c_str(), 24);
        std::string geo;                                  // 30 Georgian letters, 3 bytes each
        for (int i = 0; i < 30; ++i) geo += "\xE1\x83\x90";
        const std::string geoCut = TruncateName(geo.c_str(), 24);
        std::string emoji;                                // 30 four-byte code points
        for (int i = 0; i < 30; ++i) emoji += "\xF0\x9F\x98\x80";
        const std::string emojiCut = TruncateName(emoji.c_str(), 24);
        Check("W10 text: a valid name (24 chars) is never cut; a longer one ends in an ellipsis within 24 code points, also for 3-byte and 4-byte "
              "characters, and the cut never leaves half a UTF-8 sequence",
              TruncateName(exact.c_str(), 24) == exact && CodePoints(cut) == 24 && cut.substr(0, 23) == std::string(23, 'b') &&
                  cut.compare(cut.size() - 3, 3, "\xE2\x80\xA6") == 0 && Utf8Valid(geoCut) && CodePoints(geoCut) == 24 && Utf8Valid(emojiCut) &&
                  CodePoints(emojiCut) == 24,
              F("%zu %zu %zu", CodePoints(cut), CodePoints(geoCut), CodePoints(emojiCut)));
        Check("W10 text: control characters become spaces, doubled and edge spaces go, a stray byte or a truncated sequence is dropped, and an empty "
              "or null name reads \"Player\"",
              TruncateName("A\tB\nC", 24) == "A B C" && TruncateName("  Bob   Ross  ", 24) == "Bob Ross" && TruncateName("Al\xC3", 24) == "Al" &&
                  TruncateName("\x80Zed", 24) == "Zed" && Utf8Valid(TruncateName("a\xE1\x83", 24)) && TruncateName("", 24) == "Player" &&
                  TruncateName(nullptr, 24) == "Player" && TruncateName("\t \n", 24) == "Player" && TruncateName("x\xC2\x85y", 24) == "x y");
    }
    // the list
    {
        std::vector<TagItem> v;
        for (int i = 0; i < 20; ++i)
        {
            TagItem it;
            it.id = i;
            it.distM = (float)((i * 7) % 20) + 1.0f;      // 20 distinct distances in a scrambled order
            v.push_back(it);
        }
        TagItem tie;
        tie.id = 99;
        tie.distM = 1.0f;
        v.push_back(tie);
        KeepNearest(v, cfg.maxTags);
        bool sorted = true;
        for (std::size_t i = 1; i < v.size(); ++i) sorted = sorted && v[i - 1].distM <= v[i].distM;
        std::vector<TagCandidate> c;
        for (int i = 0; i < 30; ++i) c.push_back({ i, (float)(30 - i) });
        KeepNearestCandidates(c, cfg.maxCandidates);
        Check("W10 cap: at most 12 tags are kept, the nearest first, ties by id; at most 16 peers are asked about, the nearest",
              v.size() == 12 && sorted && v[0].distM == 1.0f && v[11].distM <= 11.0f && c.size() == 16 && c[0].id == 29 && c[15].id == 14,
              F("%zu tags, last %.0f m; %zu candidates", v.size(), v.back().distM, c.size()));
    }
}


// ---- Sync D: occupant flows (event-sequence fuzz of the local link against a server model) and the door walk -------------

namespace {

struct FuzzRng {
    uint64_t s;
    explicit FuzzRng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t Next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint32_t)(s >> 11);
    }
    int Range(int n) { return (int)(Next() % (uint32_t)n); }
    bool Chance(int percent) { return Range(100) < percent; }
};

// Three server cars with four seats each, the local player (id 0) and two other players (1, 2). The client side is the engine's: a
// ped in a car (handle 10 + car), a seat the game reads, the LocalVehicleLink ticked every 100 ms, and what the engine does with its
// output (an eject takes the ped out, a revert puts it back, an Enter / Exit goes to the server after 100-400 ms and the answer comes
// back after 100-400 ms).
struct OccupantFuzz {
    static constexpr int kCars = 3, kSeats = 4;
    uint16_t seat[kCars][kSeats];
    int32_t veh = 0;        // the local ped's vehicle handle (10 + car), 0 = on foot
    int mySeat = -1;        // the seat the game reads for the local ped
    LocalVehicleLink link;
    struct Up { int due; uint8_t op; int car; int seatNo; };
    struct Dn { int due; uint8_t op, code; int car; };
    std::vector<Up> up;
    std::vector<Dn> dn;
    FuzzRng rng;
    int tick = 0, lastUpDue = 0, lastDnDue = 0, noActionUntil = 0;   // the commands and answers travel on one reliable ordered channel each: never reordered
    int enterCmds = 0, exitCmds = 0, ejects = 0, reverts = 0, doubleSeat = 0, localEnters = 0, cmdsThisTick = 0, maxCmdsPerTick = 0;
    explicit OccupantFuzz(uint64_t seed) : rng(seed) {
        for (auto& c : seat) for (auto& x : c) x = 0xFFFF;
    }
    int MeSeats() const {
        int n = 0;
        for (auto& c : seat) for (auto x : c) n += (x == 0);
        return n;
    }
    void Server() {
        for (size_t i = 0; i < up.size();) {
            if (up[i].due > tick) { ++i; continue; }
            const Up m = up[i];
            up.erase(up.begin() + (long)i);
            uint8_t code = 0;
            if (m.op == 3) {                                   // Enter
                if (seat[m.car][m.seatNo] != 0xFFFF && seat[m.car][m.seatNo] != 0) code = 2;   // SeatTaken
                else {
                    for (auto& c : seat) for (auto& x : c) if (x == 0) x = 0xFFFF;            // one seat at a time
                    seat[m.car][m.seatNo] = 0;
                }
            } else if (m.op == 4) {                            // Exit
                for (auto& x : seat[m.car]) if (x == 0) x = 0xFFFF;
            }
            lastDnDue = std::max(lastDnDue, tick + 1 + rng.Range(4));
            dn.push_back({lastDnDue, m.op, code, m.car});
        }
        if (MeSeats() > 1) ++doubleSeat;
    }
    void Others() {
        if (!rng.Chance(8)) return;
        const int c = rng.Range(kCars), q = rng.Range(kSeats);
        const uint16_t who = (uint16_t)(1 + rng.Range(2));
        if (seat[c][q] == 0xFFFF) seat[c][q] = who;
        else if (seat[c][q] == who) seat[c][q] = 0xFFFF;
    }
    void Actions() {
        if (tick < noActionUntil || !rng.Chance(7)) return;           // an eject / revert needs a frame to take effect before the player can act again
        switch (rng.Range(5)) {
        case 0: case 1: {                                      // F / G: warp into some seat of some car (the server's list may be stale)
            const int c = rng.Range(kCars), q = rng.Range(kSeats);
            veh = 10 + c; mySeat = q; ++localEnters;
            break;
        }
        case 2:                                                // GTA's own shuffle inside the car
            if (veh) mySeat = rng.Range(kSeats);
            break;
        case 3:                                                // out
            veh = 0; mySeat = -1;
            break;
        case 4:                                                // the car vanished under us / straight into another car
            if (veh) { veh = 10 + rng.Range(kCars); mySeat = rng.Range(kSeats); ++localEnters; }
            break;
        }
    }
    void Client() {
        for (size_t i = 0; i < dn.size();) {
            if (dn[i].due > tick) { ++i; continue; }
            link.OnResult(dn[i].op, dn[i].code, (uint16_t)dn[i].car);
            dn.erase(dn.begin() + (long)i);
        }
        VehLinkIn in;
        in.nowMs = tick * 100.0;
        in.veh = veh;
        in.seat = veh ? mySeat : -1;
        in.netId = veh ? veh - 10 : -1;
        const VehLinkOut o = link.Tick(in);
        cmdsThisTick = 0;
        if (o.cmd == VehLinkCmd::Enter || o.cmd == VehLinkCmd::Exit) {
            lastUpDue = std::max(lastUpDue, tick + 1 + rng.Range(4));
            up.push_back({lastUpDue, (uint8_t)(o.cmd == VehLinkCmd::Enter ? 3 : 4), o.netId, o.seat});
            if (o.cmd == VehLinkCmd::Enter) ++enterCmds; else ++exitCmds;
            ++cmdsThisTick;
        }
        maxCmdsPerTick = std::max(maxCmdsPerTick, cmdsThisTick);
        if (o.eject) { veh = 0; mySeat = -1; ++ejects; noActionUntil = tick + 2; }
        if (o.revertSeat >= 0 && veh) { mySeat = o.revertSeat; ++reverts; noActionUntil = tick + 2; }
    }
    void Step(bool act) {
        ++tick;
        if (act) { Actions(); Others(); }
        Client();
        Server();
    }
};

}   // namespace

constexpr int kSyncDGameplayChecks = 4;

static void TestSyncD()
{
    // Two focused cases the fuzz found (the first 641 of 20000 random sequences ended with a ghost seat or a lost one before the fix):
    {
        // 1. A seat change still unanswered when we get out: the server keeps us in the old seat if it refuses the change, and nothing owed
        //    the Exit, so we stood outside while every other client kept our puppet in the car.
        LocalVehicleLink l;
        l.Tick(VIn(0, 777, 1, 9));                    // Enter(9, seat 1)
        l.OnResult(3, 0, 9);
        l.Tick(VIn(16, 777, 1, 9));                   // linked in seat 1
        VehLinkOut o = l.Tick(VIn(32, 777, 0, 9));    // the game shuffled us to seat 0: Enter(9, 0) is out
        const bool sent = o.cmd == VehLinkCmd::Enter && o.seat == 0;
        o = l.Tick(VIn(48, 0));                       // out of the car before the answer
        const bool exitAtOnce = o.cmd == VehLinkCmd::Exit && o.netId == 9;
        l.OnResult(3, 2, 9);                          // SeatTaken: the server kept us in seat 1
        int moreExits = 0;
        for (int t = 1; t <= 6; ++t)
            moreExits += l.Tick(VIn(48 + 16 * t, 0)).cmd == VehLinkCmd::Exit;
        Check("link: a seat change still unanswered when we get out owes the old seat an Exit at once (the server keeps us there if it refuses); the refusal then owes nothing more",
              sent && exitAtOnce && moreExits == 0 && !l.Linked(), F("sent %d, exit at once %d, further exits %d", (int)sent, (int)exitAtOnce, moreExits));
    }
    {
        // 2. Out and back into the same car within a round trip: the first Enter's Ok must not Exit the car again (it would remove the new seating)
        LocalVehicleLink l;
        VehLinkOut o = l.Tick(VIn(0, 777, 1, 9));     // Enter(9, seat 1)
        const bool e1 = o.cmd == VehLinkCmd::Enter;
        o = l.Tick(VIn(16, 0));                       // out before the answer
        const bool x1 = o.cmd == VehLinkCmd::Exit && o.netId == 9;
        o = l.Tick(VIn(32, 777, 1, 9));               // and back into the same car
        const bool e2 = o.cmd == VehLinkCmd::Enter && o.netId == 9;
        int exits = 0;
        l.OnResult(3, 0, 9);                          // the answer to the first Enter (the older command: the orphan)
        exits += l.Tick(VIn(48, 777, 1, 9)).cmd == VehLinkCmd::Exit;
        l.OnResult(3, 0, 9);                          // the answer to the second
        exits += l.Tick(VIn(64, 777, 1, 9)).cmd == VehLinkCmd::Exit;
        for (int t = 1; t <= 4; ++t)
            exits += l.Tick(VIn(64 + 16 * t, 777, 1, 9)).cmd == VehLinkCmd::Exit;
        Check("link: out and back into the same car within a round trip: one Exit (owed when we left), none from the first Enter's late Ok, and the second Enter links",
              e1 && x1 && e2 && exits == 0 && l.Linked() && l.NetId() == 9 && l.Seat() == 1,
              F("enter %d exit %d enter %d, later exits %d, linked %d", (int)e1, (int)x1, (int)e2, exits, (int)l.Linked()));
    }
    // The occupant flows: random but legal sequences of F / G warps, shuffles, exits, straight car changes, other players taking and
    // leaving seats, answers 100-400 ms late. After 10 s of quiet the server and the local ped agree (the local ped sits where the server
    // has it, or is on foot and the server has nobody), the server never had the local player in two seats, and the link never sent
    // more than one command per tick.
    {
        int fails = 0, doubleSeat = 0, perTick = 0, withEject = 0, withRevert = 0, withEnter = 0, firstBad = -1;
        std::string bad;
        constexpr int kSeeds = 4000;
        for (int seed = 1; seed <= kSeeds; ++seed) {
            OccupantFuzz f((uint64_t)seed);
            for (int t = 0; t < 300; ++t) f.Step(true);
            for (int t = 0; t < 120; ++t) f.Step(false);
            const int meSeats = f.MeSeats();
            bool ok = true;
            if (f.veh == 0) ok = meSeats == 0;                                       // on foot: the server has nobody of ours
            else ok = meSeats == 1 && f.seat[f.veh - 10][f.mySeat] == 0;             // in a car: the server has us there, in that seat
            doubleSeat += f.doubleSeat;
            perTick = std::max(perTick, f.maxCmdsPerTick);
            withEject += f.ejects > 0;
            withRevert += f.reverts > 0;
            withEnter += f.enterCmds > 0;
            if (!ok) {
                ++fails;
                if (firstBad < 0) {
                    firstBad = seed;
                    bad = F("seed %d: ped %s seat %d, server seats of ours %d, link %s seat %d id %d, enters %d exits %d ejects %d reverts %d", seed,
                            f.veh ? "in a car" : "on foot", f.mySeat, meSeats, f.link.StateName(), f.link.Seat(), f.link.NetId(), f.enterCmds, f.exitCmds,
                            f.ejects, f.reverts);
                }
            }
        }
        Check("occupant fuzz: 4000 random legal event sequences, then 10 s of quiet: the server and the local ped agree, never two seats at once, one command per tick",
              fails == 0 && doubleSeat == 0 && perTick <= 1 && withEject > 1000 && withRevert > 200 && withEnter >= kSeeds * 99 / 100,
              F("%d disagreements (%s), %d double-seat ticks, %d cmds/tick max, %d runs with an eject, %d with a revert", fails, bad.c_str(), doubleSeat, perTick,
                withEject, withRevert));
    }
    // The puppet's door walk. The sender RUNS to the door (TASK_GO_STRAIGHT_TO_COORD at blend 2.0, about 3.5 m/s) and is warped into
    // the seat there; a puppet that walks to the door with its own TASK_ENTER_VEHICLE (blend 1.0, about 1.5 m/s, started one render
    // delay late, INFERRED GTA speeds) is warped from where it has got to when the seat list names the player: the teleport. Following
    // the sender's pose it is within the pose lag of the door.
    {
        const double senderMps = 3.5, taskMps = 1.5, delayS = 0.11, doorM = 4.0;
        const double walkS = doorM / senderMps;                                      // the sender arrives, warps, the seat list follows
        const double taskTeleportM = doorM - taskMps * std::max(0.0, walkS - delayS);   // the task puppet started `delay` late
        const double followLagM = senderMps * 0.03;                                  // the seat list leads the pose timeline by about 30 ms
        const bool taskDefault = PuppetUsesEnterTask(EnterWalkMode::Task), followDefault = PuppetUsesEnterTask(EnterWalkMode::Follow);
        Check("door walk: the default (Follow) shows the sender's own run to the door (no TASK_ENTER_VEHICLE), so the seat warp moves the puppet about 0.1 m instead of about 2.6 m (INFERRED speeds: blend 2.0 run 3.5 m/s, blend 1.0 walk 1.5 m/s)",
              taskDefault && !followDefault && taskTeleportM > 2.0 && followLagM < 0.2, F("task %.2f m, follow %.2f m", taskTeleportM, followLagM));
    }
}
#include "anim_audit_tests.h"   // 0e: the animation audit's simulations (after Check / F)
#include "t1_seats_tests.h"   // T1 seats (wire v6): server reservations, puppet replay, local flow, the three-client scenario
#include "r3_t3_cover_tests.h"   // R3 T3 cover: sender classifier, verified cover window vs the 0d code on a simulated puppet
#include "radio_off_tests.h"   // 0.4.19 radio off: the rules, the gate, a drive against a game model (old behaviour must fail)
#include "noclip_tests.h"   // 0.4.22 noclip: movement math, state machine, saved flags, ground placement (needs Check and F above)
#include "doors_seats_tests.h"   // 0.4.22 doors and seats: rear-seat choice, the lock rules, the lock keeper (needs Check / F above)
#include "vehcontrols_pure_tests.h"   // 0.4.22 vehicle controls: keys -> indicators / engine / belt, auto-cancel, fuel gauge, wire helpers
#include "r5_edges_tests.h"   // R5 on-foot telemetry: swim / ragdoll edges, the sender's flag against the puppet's read-back (libs/sync/state_mirror.h)
#include "r5_swing_tests.h"   // R5 punches: the looped swing plan / ladder order, the explicit stop, the survey with 7 entries, under switchable GTA models
#include "hud_car_tests.h"    // 0.4.31 the car cluster (GI REV2 design): dial, ticks, fuel row, chips, blink, anchor
#include "hud_ring_tests.h"   // 0.4.31 the vitals ring (GI REV1 design): state table, rules, pulse, scale
#include "vehstate_row_tests.h"   // 0.4.27 vehicle state row (lock / belt / engine) and the indicator fallback (needs Check / F above)
#include "inventory_tests.h"   // 0.6 the inventory rules (GI REV3 design): catalog, moves, caps, packs, equipment, quick slots, Apply, codec
#include "overlay_input_tests.h"   // 0.5.4 overlay_combat_block: the controls held while an overlay panel is open
#include "outfit_tests.h"      // 0.5.4 outfit_items: worn shirt / pants / shoes -> the ped's components (bare table, spawn outfit, take off / on)
#include "ped_view_tests.h"    // 0.5.4 inv_ped_view: the CHARACTER panel's window and the camera framing the ped in it
#include "inventory_world_tests.h"   // 0.5.4 inventory world: weapons as items, the hand, codec v2, ground piles, trunks, the weapon reconcile

int main()
{
    std::printf("-- vitals\n");
    TestVitals();
    std::printf("-- respawn gate\n");
    TestRespawn();
    std::printf("-- hit batcher\n");
    TestBatcher();
    std::printf("-- peer damage probe\n");
    TestPeerDamage();
    std::printf("-- emote arbiter\n");
    TestEmote();
    std::printf("-- vehicle link\n");
    TestLink();
    std::printf("-- F / G vehicle entry\n");
    TestEntry();
    std::printf("-- seated drive-by\n");
    TestVehicleAim();
    std::printf("-- review items 6-11, 16, 17 and the pause menu\n");
    TestMore();
    std::printf("-- sync plan W3: combat client\n");
    TestW3Combat();
    std::printf("-- sync plan W6: vehicle body, ownership, v5 combat inputs, on-foot Tier B\n");
    TestW6();
    std::printf("-- W8: melee reaction, dead-puppet damage, water / climb edges, alt-tab fallback\n");
    TestW8();
    std::printf("-- 0.4.4 item 0c: melee hit by the damage event, receiver swing trigger\n");
    Test0cMelee();
    std::printf("-- 0.4.5 melee audit: scripted fights through the client's melee pipeline\n");
    Test05MeleeSim();
    std::printf("-- W9: passenger enter, re-warp limiter, F press guard, passenger engine, bail-out, vehicle impact, bikes\n");
    TestW9();
    std::printf("-- W10: player name tags (fade, projection, occlusion cache, text, list)\n");
    TestW10();
    std::printf("-- Sync D: occupant event fuzz, the puppet's door walk\n");
    TestSyncD();
    std::printf("-- 0e: animation audit (scripted emote / swing simulations, dictionary streaming, diagnostics)\n");
    anim_audit::TestAnimAudit();
    t6health::TestPure();
    if (r3t1::TestPure() != r3t1::kPureChecks)
        ++g_failed;                                 // R3 T1 health (tests/r3_t1_health_tests.h)
    t1::RunPure();                                  // T1 seats
    std::printf("-- R3 T3: cover (sender classifier, verified window, breaker, modes, diagnostics)\n");
    r3cover::RunPure();
    std::printf("-- R3 T6: swing ladder, read-back reasons, bounded retry\n");
    r3t6::ClientTests();
    std::printf("-- R4 T3: swing progress check (+100 / +250 / +450 ms), survey, per-swing lines, under switchable GTA models\n");
    r4t3::ClientTests();
    radiooff::RunPure();                            // 0.4.19 radio off (tests/radio_off_tests.h)
    mapclear::RunPure();                            // 0.4.20 map clean-up (tests/map_clear_tests.h)
    minimaptest::RunPure();                         // 0.4.22 minimap (tests/minimap_tests.h)
    noclipt::RunPure();                             // 0.4.22 noclip (tests/noclip_tests.h)
    doorsseats::RunPure();                          // 0.4.22 doors and seats (tests/doors_seats_tests.h)
    vehctl::RunPure();                              // 0.4.22 vehicle controls (tests/vehcontrols_pure_tests.h)
    std::printf("-- R5: swim / ragdoll edges (sender flag against the puppet's read-back)\n");
    r5edge::RunPure();
    std::printf("-- R5: looped punches (plan order, explicit stop, survey of 7, under switchable GTA models)\n");
    r5swing::RunPure();
    hudring_t::RunPure();                           // 0.4.31 the vitals ring (tests/hud_ring_tests.h)
    hudcar_t::RunPure();                            // 0.4.31 the car cluster (tests/hud_car_tests.h)
    vehrow::RunPure();                              // 0.4.27 vehicle state row + indicator fallback (tests/vehstate_row_tests.h)
    std::printf("-- 0.6 inventory rules (catalog, moves, caps, packs, equipment, quick slots, Apply, codec)\n");
    invtest::RunPure();                             // 0.6 the inventory (tests/inventory_tests.h)
    overlayinput_t::RunPure();                      // 0.5.4 overlay input (tests/overlay_input_tests.h)
    std::printf("-- 0.5.4 outfit items and the inventory ped view\n");
    outfitt::RunPure();                             // 0.5.4 outfit_items (tests/outfit_tests.h)
    pedview_t::RunPure();                           // 0.5.4 inv_ped_view (tests/ped_view_tests.h)
    std::printf("-- 0.5.4 inventory world (weapons as items, hand, codec v2, ground, trunks, reconcile)\n");
    invworld::RunPure();                            // 0.5.4 (tests/inventory_world_tests.h)

    // vitals 41 (3 for the max-raise re-apply, live 0.4.3) + respawn 8 + batcher 8 + peer 13 + emote 23 + link 42 + entry 18 + drive-by 7 + more 60 + W3 52 + W6 23
    // + W8 11 (melee react 4, dead-puppet damage 1, water 1, climb 2, aim + fallback 2, site tally 1; the old melee check was reworded, not added)
    // + 0c 7 (hit by event, no event, gun, stale / action / edge, one report for both detectors, window gate, receiver swing trigger)
    // + 0d 6 (in TestW8 beside the climb window: pre-vault placement 1, climb accounting 1, cover window 4)
    // + W9 8 (enter warp 1, re-warp limiter 1, press guard 1, passenger engine 1, unseat 2, impact 1, bike class 1; EnterWarpDue's old W6 check stays)
    // + W10 16 (fade 1, scale 1, projection 1, cache 1, cache rate 1, eased alpha 1, hidden rules 3, walk-away 1, wall latency 1, candidates 1, text 3, cap 1)
    // + 0.4.5 melee audit 8 (punch, combo, miss then hit, stale event, window edge, guns, event gate, client/server window)
    const int expected = 41 + 8 + 8 + 13 + 23 + 42 + 18 + 7 + 60 + 52 + 23 + 11 + 7 + 6 + 8 + 16 + kSyncDGameplayChecks + 8
                       + mapclear::kChecks    // + 0.4.20 map clean-up 19
                       + minimaptest::kChecks // + 0.4.22 minimap 28
                       + noclipt::kChecks     // + 0.4.22 noclip 31
                       + doorsseats::kPureChecks    // + 0.4.22 doors and seats 19
                       + vehctl::kPureChecks  // + 0.4.22 vehicle controls 39
                       + r5edge::kChecks      // + R5 swim / ragdoll edge telemetry
                       + r5swing::kChecks    // + R5 looped punches
                       + vehrow::kChecks      // + 0.4.27 vehicle state row + indicator fallback 31
                       + 1                    // + 0.4.30 punch: a melee stagger is a clip (tests W8 block)
                       + hudring_t::kChecks   // + 0.4.31 the vitals ring 9
                       + hudcar_t::kChecks    // + 0.4.31 the car cluster 10
                       + invtest::kChecks     // + 0.6 the inventory rules 15
                       + overlayinput_t::kChecks   // + 0.5.4 overlay input 5
                       + outfitt::kChecks     // + 0.5.4 outfit items 9
                       + pedview_t::kChecks   // + 0.5.4 inventory ped view 7
                       + invworld::kChecks;   // + 0.5.4 the inventory world 14
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected + anim_audit::kAnimAuditChecks + t6health::kPureChecks + r3t1::kPureChecks + t1::kPureChecks + r3cover::kPureChecks + r3t6::kClientChecks + r4t3::kClientChecks + radiooff::kPureChecks)   // + 0e animation audit; + T6 health; + R3 T1 health; + T1 seats; + R3 T3 cover; + R3 T6 melee client (tests/r3_t6_tests.h)
    {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected + anim_audit::kAnimAuditChecks + t6health::kPureChecks + r3t1::kPureChecks + t1::kPureChecks + r3cover::kPureChecks + r3t6::kClientChecks + r4t3::kClientChecks + radiooff::kPureChecks);
        return 1;
    }
    return g_failed ? 1 : 0;
}
