// t6_health_tests.h - T6 (desync overhaul round 2, GI's symptom 5: "health sticks at 50 instead of 100").
// Test-only, header-only. Included by gameplay_test (pure: a fake server) and, with T6_HEALTH_NETWORK defined, by
// netcode_test (the REAL gage::server::Server and the real net::ClientSession).
//
// What it mirrors (keep in step with apps/engine/engine.cpp; every decision is the real libs/sync code, only the
// game's ped is simulated):
//   Welcome                  OnWelcome: g_playerHealth = Welcome.playerHealth (0 = the default 100)
//   ResetV4Units             VitalsTracker::Reset(playerHealth) + ApplyServerMaxHealth (SET_ENTITY_MAX_HEALTH + read-back)
//   DrainEvents              Damage about us -> VitalsInbox -> OnDamageMerged; Killed -> OnKilled; RespawnOk
//   TickVitals               VitalsObs from the ped (handle, health, max, armour, dead) -> Tick -> self-hits out as Hits,
//                            apply -> SET_ENTITY_HEALTH / SET_PED_ARMOUR, kill -> SET_ENTITY_HEALTH(0)
//   TickLife                 RespawnGate; RespawnOk -> OnRespawn, RESURRECT, ApplyServerMaxHealth, SET_ENTITY_HEALTH(100 + health)
//   TickEntryGuard           GTA swaps the player ped back to a story character -> SET_PLAYER_MODEL gives a NEW ped
//                            (default health 200, default max 200), then ApplyServerMaxHealth in the same script tick
//   DrawHud                  HudHealthFraction(hp, max, playerHealth, lastGoodMax)
// The SimPed is the ONLY assumption about GTA, and since R3 T1 (live 0.4.7: ped 200 / max 300 / server 195 / bar 50 %, the
// health never rose above 200 although GET_ENTITY_MAX_HEALTH read 300) it is a set of SWITCHABLE MODELS (SimPed::model):
//   Direct          the round 2 model: SET_ENTITY_MAX_HEALTH raises the health cap (GTA holds 100 + ph)
//   Cap200          the live log: the health stays at 200 whatever the maxima read
//   FollowsPedMax   only SET_PED_MAX_HEALTH raises the cap
//   CapNextFrame    SET_ENTITY_HEALTH is accepted and GTA clamps it to 200 a frame later
//   Cap150          a ceiling BELOW 200 (health 150: a span of 50)
// and every scenario runs under each of them (Scenarios(env, full, model)). Since R3 T1 the Client mirrors the engine's
// HealthController wiring (Begin at session start / swap / respawn / the tracker's request, Pre before every tracker tick).
// (The T6_BASELINE switch of round 2 is gone: the pre-fix combat.* it compiled against is history; the pre-R3-T1 behaviour is
// the Direct health_mode wish, which tests/r3_t1_health_tests.h runs against the capped models.)
#pragma once

#include "sync/combat.h"
#include "protocol/protocol.h"
#include "../apps/server/vehicles.h"   // gage::server::ApplyDamage, the real armour-then-health rule

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace t6health {

using namespace gage::sync;
namespace proto = gage::proto;

static std::string Fmt(const char* fmt, ...)
{
    char buf[500];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// ---- the game's ped ----------------------------------------------------------------------------------------------

// The GTA behaviours the harness can be switched between (see the header comment).
enum class Model { Direct, Cap200, FollowsPedMax, CapNextFrame, Cap150 };
static const Model kModels[] = { Model::Direct, Model::Cap200, Model::FollowsPedMax, Model::CapNextFrame, Model::Cap150 };
static const char* ModelName(Model m)
{
    return m == Model::Direct ? "direct" : m == Model::Cap200 ? "cap200" : m == Model::FollowsPedMax ? "pedmax" : m == Model::CapNextFrame ? "nextframe" : "cap150";
}
// The GTA health above 100 a player ped can hold under the model once the code has done everything right (999 = as much as asked).
static int Ceil(Model m)
{
    return m == Model::Cap200 || m == Model::CapNextFrame ? 100 : m == Model::Cap150 ? 50 : 999;
}
static int Span(Model m, int ph) { return std::min(ph, Ceil(m)); }
// What the ped must carry (GTA above 100) for a server value, computed here independently of libs/sync.
static int ExpG(Model m, int ph, int sh)
{
    const int c = Ceil(m);
    if (c >= ph)
        return sh;
    if (sh <= 0)
        return 0;
    const int g = (int)std::floor((double)sh * c / ph + 0.5);
    return std::max(1, std::min(c, g));
}
// A GTA-unit loss as the server must see it.
static int ExpLoss(Model m, int ph, int g)
{
    const int c = Ceil(m);
    return c >= ph ? g : std::max(1, (int)std::floor((double)g * ph / c + 0.5));
}

struct SimPed
{
    int  handle = 1000;
    int  hp = 200, max = 200, armour = 0;   // a fresh freemode ped: GTA's default health 200 and maximum 200
    int  pedMax = 200;                      // what SET_PED_MAX_HEALTH set (the cap of FollowsPedMax)
    bool dead = false;
    Model model = Model::Direct;
    // alternatives to the assumed GTA behaviour
    int  healthCap = 0;                     // > 0: SET_ENTITY_HEALTH never keeps more than this, whatever the maximum
    bool maxReadFails = false;              // GET_ENTITY_MAX_HEALTH answers 0
    bool healthReadFails = false;           // R3 T1: GET_ENTITY_HEALTH answers 0 (the probe's read; the tracker's own read is not affected)
    bool setMaxFails = false;               // SET_ENTITY_MAX_HEALTH / SET_PED_MAX_HEALTH do nothing and report failure
    int  ignoreWrites = 0;                  // the next N SET_ENTITY_HEALTH calls are dropped (a ped in a transition)
    bool resurrectResetsMax = false;        // RESURRECT_PED puts the maximum back to 200
    int  healthWrites = 0, maxWrites = 0, pedMaxWrites = 0;

    int Cap() const                         // the most health GTA keeps on this ped right now
    {
        int cap = max;
        switch (model)
        {
        case Model::Direct: break;
        case Model::Cap200: cap = std::min(cap, 200); break;
        case Model::FollowsPedMax: cap = std::min(cap, pedMax); break;
        case Model::CapNextFrame: break;    // the write is accepted, Frame() clamps
        case Model::Cap150: cap = std::min(cap, 150); break;
        }
        if (healthCap > 0 && healthCap < cap)
            cap = healthCap;
        return cap;
    }
    bool SetMax(int m)                      // SET_ENTITY_MAX_HEALTH
    {
        ++maxWrites;
        if (setMaxFails)
            return false;
        max = m;
        if (!dead && hp > Cap())
            hp = Cap();
        return true;
    }
    bool SetPedMax(int m)                   // SET_PED_MAX_HEALTH
    {
        ++pedMaxWrites;
        if (setMaxFails)
            return false;
        pedMax = m;
        if (model == Model::Direct)
            max = m;
        if (!dead && hp > Cap())
            hp = Cap();
        return true;
    }
    int GetMax() const { return maxReadFails ? 0 : max; }
    bool SetHealth(int v)
    {
        ++healthWrites;
        if (ignoreWrites > 0)
        {
            --ignoreWrites;
            return true;
        }
        if (v <= 100)
        {
            dead = true;
            hp = 0;
            return true;
        }
        hp = std::min(v, Cap());
        return true;
    }
    int  Health() const { return dead ? 0 : hp; }
    void Frame()                            // GTA's own per-frame processing
    {
        if (!dead && model == Model::CapNextFrame && hp > 200)
            hp = 200;
    }
    void Hurt(int d)                        // a fall, fire, a car
    {
        if (dead)
            return;
        hp -= d;
        if (hp <= 100)
        {
            dead = true;
            hp = 0;
        }
    }
    void Swap()                             // SET_PLAYER_MODEL: a new ped with GTA's defaults
    {
        ++handle;
        hp = 200;
        max = 200;
        pedMax = 200;
        armour = 0;
        dead = false;
    }
    void Resurrect()
    {
        dead = false;
        if (resurrectResetsMax)
        {
            max = 200;
            pedMax = 200;
        }
        hp = std::min(max, Cap());
    }
};

// ---- the server as the client sees it -------------------------------------------------------------------------------

struct Link
{
    virtual ~Link() = default;
    virtual uint16_t Me() const = 0;
    virtual void Poll(std::vector<proto::Damage>& dmg, std::vector<proto::Killed>& killed, std::vector<proto::RespawnOk>& ok) = 0;
    virtual void SendSelfHit(int damage, uint32_t weapon) = 0;
    virtual void SendRespawn() = 0;
    virtual void Idle() = 0;                // one slice of transport time per script tick
    // the other side of the scenario
    virtual void HurtBy(int damage) = 0;    // another player shoots us for about `damage`
    virtual void Heal() = 0;                // console `heal`
    virtual void Kill() = 0;                // console `kill`
    virtual int  ServerHealth() = 0;        // the server's own number for us
};

// A pure stand-in with the server's rules (the real ApplyDamage; Heal / Respawn set Config::playerHealth).
struct FakeLink final : Link
{
    int ph;
    uint8_t health, armor = 0;
    bool dead = false;
    int ticks = 0, deathTick = 0;
    int ackDelay = 0;                       // ticks a self-hit's Damage is held before the client hears it (a slow link)
    struct Held { int due; proto::Damage d; };
    std::vector<Held> held;
    std::vector<proto::Damage> dmg;
    std::vector<proto::Killed> killed;
    std::vector<proto::RespawnOk> ok;
    explicit FakeLink(int playerHealth) : ph(playerHealth), health((uint8_t)playerHealth) {}
    uint16_t Me() const override { return 7; }
    void Poll(std::vector<proto::Damage>& d, std::vector<proto::Killed>& k, std::vector<proto::RespawnOk>& o) override
    {
        for (size_t i = 0; i < held.size();)
            if (held[i].due <= ticks)
            {
                d.push_back(held[i].d);
                held.erase(held.begin() + (long)i);
            }
            else
                ++i;
        d.insert(d.end(), dmg.begin(), dmg.end());
        k.insert(k.end(), killed.begin(), killed.end());
        o.insert(o.end(), ok.begin(), ok.end());
        dmg.clear();
        killed.clear();
        ok.clear();
    }
    void Hurt(uint16_t attacker, int damage)
    {
        if (dead)
            return;
        const uint16_t applied = gage::server::ApplyDamage(health, armor, (uint16_t)damage);
        proto::Damage d;
        d.victimId = Me();
        d.attackerId = attacker;
        d.damage = applied;
        d.health = health;
        d.armor = armor;
        d.killed = health == 0;
        if (attacker == Me() && ackDelay > 0)
            held.push_back({ticks + ackDelay, d});
        else
            dmg.push_back(d);
        if (health == 0)
        {
            dead = true;
            deathTick = ticks;
            proto::Killed k;
            k.victimId = Me();
            k.killerId = attacker;
            killed.push_back(k);
        }
    }
    void SendSelfHit(int damage, uint32_t) override { Hurt(Me(), damage); }
    void SendRespawn() override
    {
        if (!dead || ticks - deathTick < 20)    // the server's respawn delay (about 0.3 s of ticks)
            return;
        dead = false;
        health = (uint8_t)ph;
        armor = 0;
        proto::RespawnOk r;
        r.health = health;
        r.armor = armor;
        ok.push_back(r);
    }
    void Idle() override { ++ticks; }
    void HurtBy(int damage) override { Hurt(2, damage); }
    void Heal() override
    {
        if (dead)
            return;
        health = (uint8_t)ph;
        proto::Damage d;
        d.victimId = Me();
        d.attackerId = proto::kServerId;
        d.health = health;
        d.armor = armor;
        dmg.push_back(d);
    }
    void Kill() override { Hurt(proto::kServerId, health + armor); }
    int  ServerHealth() override { return health; }
};

// ---- the client (the engine's wiring) ------------------------------------------------------------------------------

struct PedPort
{
    SimPed& p;
    bool SetEntityMax(int m) { return p.SetMax(m); }
    bool SetPedMax(int m) { return p.SetPedMax(m); }
    bool SetHealth(int h) { return p.SetHealth(h); }
    int  GetHealth() { return p.healthReadFails ? 0 : p.Health(); }
    int  GetMax() { return p.GetMax(); }
    // the T6 era names (ApplyMaxHealth)
    bool SetMax(int m) { return p.SetMax(m); }
};

struct Client
{
    Link& link;
    VitalsTracker v;
    VitalsInbox inbox;
    HealthController hc;                    // R3 T1: the probe and the DIRECT / SCALED choice (the engine's g_health)
    SimPed ped;
    RespawnGate gate;
    int playerHealth = 100;                 // g_playerHealth
    double now = 0;
    uint32_t tick = 0;                      // TickVitals' script tick number (g_healthTick)
    enum class Life { Alive, Dying } life = Life::Alive;
    std::vector<int> selfHits;              // every environmental self-hit sent
    std::vector<std::string> log;           // the lines the engine would put in gage.log
    bool swapAtGuard = false;               // GTA swapped the ped back: the guard acts on its next pass
    bool tellHandle = true;                 // TickVitals passes the ped handle (VitalsObs::ped)
    bool tellSwap = true;                   // the guard calls VitalsTracker::NotePedReplaced
    bool haveRespawn = false;
    proto::RespawnOk respawnOk;
    int swaps = 0, respawns = 0, probesAsked = 0;

    static RespawnConfig FastRespawn()
    {
        RespawnConfig c;
        c.firstAfterMs = 400.0;
        c.retryMs = 100.0;
        return c;
    }
    explicit Client(Link& l, const RespawnConfig& rc = FastRespawn()) : link(l), gate(rc) {}

    void Welcome(int ph) { playerHealth = ph ? ph : proto::kDefaultHealth; }
    void Drain()
    {
        HealthController::Line l;
        while (hc.TakeLine(l))
            log.push_back(l.text);
    }
    int ApplyMax(const char* why = "test", bool beforeVitals = false)   // ApplyServerMaxHealth
    {
        PedPort pp{ped};
        hc.Begin(pp, ped.handle, playerHealth, v, tick + (beforeVitals ? 1u : 0u), now, why);
        Drain();
        return ped.GetMax();
    }
    void ResetV4()                          // ResetV4Units
    {
        v.Reset((uint8_t)playerHealth, 0);
        hc.Reset();
        ApplyMax("session start", true);
        gate.OnAlive();
        life = Life::Alive;
    }
    void DrainEvents()
    {
        std::vector<proto::Damage> d;
        std::vector<proto::Killed> k;
        std::vector<proto::RespawnOk> ok;
        link.Poll(d, k, ok);
        const uint16_t me = link.Me();
        for (const proto::Damage& x : d)
            if (x.victimId == me)
                inbox.OnDamage(x.health, x.armor, x.damage, x.attackerId == me, x.killed, x.attackerId, x.weapon, x.bone, x.flags);
        for (const proto::Killed& x : k)
            if (x.victimId == me)
                inbox.OnKilled();
        for (const proto::RespawnOk& x : ok)
        {
            respawnOk = x;
            haveRespawn = true;
        }
        DamageSlot ds;
        if (inbox.Take(ds))
        {
            if (ds.events > 0)
                v.OnDamageMerged(ds.health, ds.armour, ds.damage, ds.selfAckDamage, ds.events, ds.killed);
            else if (ds.killed)
                v.OnKilled();
        }
    }
    void TickVitals()
    {
        ++tick;
        PedPort pp{ped};
        const bool hold = hc.Pre(pp, ped.handle, v, tick, now);
        Drain();
        if (hold || !VitalsTickDue(true, life == Life::Alive, life == Life::Dying, v.ServerDead()))
            return;
        VitalsObs ob;
        ob.nowMs = now;
        ob.health = ped.Health();
        ob.maxHealth = ped.GetMax();
        ob.armour = ped.armour;
        ob.dead = ped.dead;
        if (tellHandle)
            ob.ped = ped.handle;
        const VitalsOut o = v.Tick(ob);
        if (o.probe)
        {
            ++probesAsked;
            ApplyMax(o.probeWhy);
            return;
        }
        for (int i = 0; i < o.selfHitCount; ++i)
        {
            selfHits.push_back(o.selfHits[i]);
            link.SendSelfHit(o.selfHits[i], 0);
        }
        if (o.apply && !ob.dead)
        {
            ped.SetHealth(o.setHealth);
            ped.armour = o.setArmour;
        }
        if (o.kill)
            ped.SetHealth(0);
    }
    void TickLife()
    {
        if (life == Life::Alive && ped.dead)
        {
            life = Life::Dying;
            gate.OnDeath(now);
        }
        if (life != Life::Dying)
            return;
        if (haveRespawn)
        {
            haveRespawn = false;
            v.OnRespawn(respawnOk.health, respawnOk.armor);
            ped.Resurrect();                // ReviveLocalPlayer
            gate.OnAlive();
            life = Life::Alive;
            ++respawns;
            ApplyMax("respawn");            // before the health: a value above the old maximum would be cut
            ped.SetHealth(100 + v.Scale().ToGta((int)v.Health()));
            ped.armour = (int)v.Armour();
            return;
        }
        if (gate.ShouldSend(now))
            link.SendRespawn();
    }
    void TickGuard()
    {
        if (!swapAtGuard)
            return;
        swapAtGuard = false;
        ped.Swap();                         // SetPlayerModel(kFreemodeMale): a new ped
        ++swaps;
        ApplyMax("ped guard swap");         // ... and the server maximum health, in the same script tick
        if (tellSwap)
            v.NotePedReplaced();
    }
    void Tick()
    {
        now += 16.0;
        link.Idle();
        ped.Frame();
        DrainEvents();
        TickVitals();
        TickLife();
        if (life == Life::Alive)
            TickGuard();
    }
    void Run(double ms)
    {
        const double end = now + ms;
        while (now < end)
            Tick();
    }
    float Bar() const                       // DrawHud
    {
        if (v.Scale().Scaled() && !v.ServerDead())
            return ServerBarFraction((int)v.Health(), v.Pending(), playerHealth);
        return HudHealthFraction(ped.Health(), ped.GetMax(), playerHealth, v.LastGoodMax());
    }
    int Hits() const { int s = 0; for (int h : selfHits) s += h; return s; }
    int Span() const { return t6health::Span(ped.model, playerHealth); }
};

// The T6 counters; all zero against the pre-fix tracker (T6_BASELINE has none of them).
struct Cnt
{
    uint32_t reconciles = 0, gaveUp = 0, maxUnreadable = 0, maxWrites = 0, maxWriteFails = 0, maxReadBackOff = 0, maxChanges = 0, swaps = 0;
    int wanted = 0, readBack = 0;
};
static Cnt CountersOf(const VitalsTracker& v)
{
    Cnt c;
    const VitalsStats& s = v.Stats();
    c.maxChanges = s.maxChanges;
    c.reconciles = s.reconciles;
    c.gaveUp = s.reconcileGaveUp;
    c.maxUnreadable = s.maxUnreadable;
    c.maxWrites = s.maxWrites;
    c.maxWriteFails = s.maxWriteFails;
    c.maxReadBackOff = s.maxReadBackOff;
    c.swaps = s.pedSwaps;
    c.wanted = s.lastMaxWanted;
    c.readBack = s.lastMaxReadBack;
    return c;
}

// The bar the server's number implies, and whether the ped agrees with it.
static float ExpectedBar(int serverHealth, int ph) { return (float)serverHealth / (float)ph; }
static bool BarIs(const Client& c, int serverHealth) { return std::fabs(c.Bar() - ExpectedBar(serverHealth, c.playerHealth)) < 0.01f; }
static bool PedIs(const Client& c, int serverHealth) { return c.ped.Health() == 100 + ExpG(c.ped.model, c.playerHealth, serverHealth); }
static std::string Got(const Client& c, Link& l)
{
    return Fmt("[%s %s] ped %d/%d bar %.0f%% server %d self-hits %d (%d)", ModelName(c.ped.model), c.hc.Scaled() ? "SCALED" : "DIRECT", c.ped.Health(),
               c.ped.max, c.Bar() * 100.0f, l.ServerHealth(), (int)c.selfHits.size(), c.Hits());
}

// ---- scenarios (shared by the pure run and the real-server run) -------------------------------------------------------

// Opens a session: a fresh Link whose server has this player at full health (the real run joins; the pure run builds one).
struct Env
{
    virtual ~Env() = default;
    virtual Link& Open(int ph) = 0;         // join a server running playerHealth `ph`
    virtual void Close() = 0;               // leave
    virtual bool Real() const { return false; }
};

template <class F>
static void Section(const char* what, F&& body) { std::printf("-- T6 health: %s\n", what); body(); }

static std::string Tag(int ph, Model m = Model::Direct) { return Fmt("[%s] playerHealth %d", ModelName(m), ph); }

// Join: the ped is sized to the server and sits at full health.
static void Join(Env& env, int ph, Model m)
{
    Link& l = env.Open(ph);
    Client c(l);
    c.ped.model = m;
    c.Welcome(ph);
    c.ResetV4();
    c.Run(1500);
    const int top = 100 + Span(m, ph);
    Check(("join: " + Tag(ph, m) + ": the ped has the maximum it can hold (100 + min(ph, ceiling)) and is at it, the bar reads 100 %, nothing was sent").c_str(),
          c.ped.max == top && c.ped.Health() == top && c.Bar() > 0.995f && c.selfHits.empty() && l.ServerHealth() == ph,
          Got(c, l));
    env.Close();
}

// T0's S1 and the reproduction of "sticks at 50": GTA gives the player a new default ped (hp 200, max 200) and the
// guard raises its maximum to the server's in the same script tick. Damaged first (the server holds ph - 40) or not.
static void SwapDefaultPed(Env& env, int ph, bool damagedFirst, Model m)
{
    Link& l = env.Open(ph);
    Client c(l);
    c.ped.model = m;
    c.Welcome(ph);
    c.ResetV4();
    c.Run(500);
    if (damagedFirst)
    {
        l.HurtBy(40);
        c.Run(500);
    }
    const int before = l.ServerHealth();
    const bool setup = (damagedFirst ? before == ph - 40 : before == ph) && PedIs(c, before) && BarIs(c, before);
    c.swapAtGuard = true;
    c.Run(3000);
    const int after = l.ServerHealth();
    Check(("swap: " + Tag(ph, m) + (damagedFirst ? ", hurt for 40 first" : ", at full health") +
           ": the swapped ped gets the server's health back on the new ped (hp, bar), the server's number is untouched, "
           "no self-hit is sent, and the swap really happened")
              .c_str(),
          setup && c.swaps == 1 && after == before && PedIs(c, before) && BarIs(c, before) && c.selfHits.empty(), Got(c, l));
    env.Close();
}

// Environmental damage still works (the PRESENCE check for everything that suppresses self-hits): a fall of 30 is one
// self-hit of 30, the server takes it, the ped and the bar follow.
static void Fall(Env& env, int ph, Model m)
{
    Link& l = env.Open(ph);
    Client c(l);
    c.ped.model = m;
    c.Welcome(ph);
    c.ResetV4();
    c.Run(500);
    c.ped.Hurt(30);                         // 30 GTA units
    c.Run(1500);
    const int loss = ExpLoss(m, ph, 30);    // ... which is this many server points when the ped runs in a smaller range
    Check(("fall: " + Tag(ph, m) + ": a 30-unit fall is exactly one self-hit (30 scaled back by ph / ceiling when SCALED), the server holds ph - that, the ped and the bar agree")
              .c_str(),
          c.selfHits.size() == 1 && c.selfHits[0] == loss && l.ServerHealth() == ph - loss && PedIs(c, ph - loss) && BarIs(c, ph - loss) &&
              c.v.Pending() == 0,
          Got(c, l));
    env.Close();
}

// A big fall during a swap's aftermath is still reported: after the swap settles, a 60-point fall is a self-hit of 60.
static void FallAfterSwap(Env& env, int ph, Model m)
{
    Link& l = env.Open(ph);
    Client c(l);
    c.ped.model = m;
    c.Welcome(ph);
    c.ResetV4();
    c.Run(300);
    c.swapAtGuard = true;
    c.Run(2000);
    const int fall = std::min(60, Span(m, ph) * 3 / 5);   // 60 GTA units (a 50-unit span cannot take 60)
    c.ped.Hurt(fall);
    c.Run(1500);
    const int loss = ExpLoss(m, ph, fall);
    Check(("fall after a swap: " + Tag(ph, m) + ": the swap itself costs nothing, the later big fall is one self-hit (scaled back when SCALED) and the server holds ph - that")
              .c_str(),
          c.swaps == 1 && c.selfHits.size() == 1 && c.selfHits[0] == loss && l.ServerHealth() == ph - loss && PedIs(c, ph - loss) &&
              BarIs(c, ph - loss),
          Got(c, l));
    env.Close();
}

// Shot, healed, killed, respawned.
static void LifeCycle(Env& env, int ph, bool resurrectResetsMax, Model m)
{
    Link& l = env.Open(ph);
    Client c(l);
    c.ped.model = m;
    c.ped.resurrectResetsMax = resurrectResetsMax;
    c.Welcome(ph);
    c.ResetV4();
    c.Run(300);
    l.HurtBy(50);
    c.Run(400);
    const bool shot = l.ServerHealth() == ph - 50 && PedIs(c, ph - 50) && BarIs(c, ph - 50);
    l.Heal();
    c.Run(400);
    const bool healed = l.ServerHealth() == ph && PedIs(c, ph) && BarIs(c, ph) && c.Bar() > 0.995f;
    l.Kill();
    c.Run(300);
    const bool died = c.ped.dead && c.life == Client::Life::Dying;
    c.Run(4000);                            // the respawn gate asks, the server answers, the ped comes back
    Check(("life cycle: " + Tag(ph, m) + (resurrectResetsMax ? ", RESURRECT resets the max" : "") +
           ": shot (ph-50, bar follows), healed (full), killed (ped dies), respawned (full health on max 100+ph, bar 100 %)")
              .c_str(),
          shot && healed && died && c.respawns == 1 && c.ped.max == 100 + Span(m, ph) && c.ped.Health() == 100 + Span(m, ph) && c.Bar() > 0.995f &&
              l.ServerHealth() == ph && c.selfHits.empty(),
          Fmt("%s [shot %d healed %d died %d respawns %d]", Got(c, l).c_str(), shot, healed, died, c.respawns));
    env.Close();
}

// A second session: Welcome, ResetV4Units, the bar is full again. A different playerHealth in the second session
// (an ops change from 100 to 200) is picked up at once: the maximum follows the NEW Welcome.
static void Reconnect(Env& env, int ph1, int ph2, Model m)
{
    {
        Link& l = env.Open(ph1);
        Client c(l);
        c.ped.model = m;
        c.Welcome(ph1);
        c.ResetV4();
        c.Run(300);
        l.HurtBy(20);
        c.Run(400);
        env.Close();
    }
    Link& l = env.Open(ph2);
    Client c2(l);
    c2.ped.model = m;
    c2.Welcome(ph2);
    c2.ResetV4();
    c2.Run(1500);
    Check(("reconnect: [" + std::string(ModelName(m)) + "] playerHealth " + std::to_string(ph1) + " then " + std::to_string(ph2) +
           ": the new session sizes the ped to the new Welcome and the bar is full")
              .c_str(),
          c2.ped.max == 100 + Span(m, ph2) && c2.ped.Health() == 100 + Span(m, ph2) && c2.Bar() > 0.995f && c2.selfHits.empty() && l.ServerHealth() == ph2,
          Got(c2, l));
    env.Close();
}

// Pure-only scenarios: the GTA behaviours a server cannot drive.
static int PureOnly()
{
    int n = 0;
    {
        // ABSOLUTE RECONCILIATION: a TRACKER write GTA dropped leaves the ped steady off the server's value (the server took 20
        // from a shooter, the ped keeps its old health), the loss detector sees no change and never notices. The old code stayed
        // there for the session. Presence: it is corrected within about 3 s with no self-hit. Under both the DIRECT and the
        // SCALED model of GTA (R3 T1: the probe of the start must not eat this write: it is dropped AFTER the start).
        for (Model m : { Model::Direct, Model::Cap200 })
        {
            FakeLink l(200);
            Client c(l);
            c.ped.model = m;
            c.Welcome(200);
            c.ResetV4();
            c.Run(500);
            c.ped.ignoreWrites = 1;             // the tracker's next apply (after the Damage) is lost
            l.HurtBy(20);
            c.Run(300);
            const bool stuckFirst = l.ServerHealth() == 180 && c.ped.Health() == 100 + Span(m, 200) && !PedIs(c, 180);   // the symptom, before the check
            c.Run(3000);
            Check((std::string("absolute check: a dropped write (ped steady off the server's 180) is put right within 4 s, no self-hit [") + ModelName(m) + "]").c_str(),
                  stuckFirst && PedIs(c, 180) && BarIs(c, 180) && c.selfHits.empty() && CountersOf(c.v).reconciles >= 1, Got(c, l));
            ++n;
        }
    }
    {
        // GET_ENTITY_MAX_HEALTH reads 0 on a few frames mid-session (damaged, server 150 of 200): the old tracker
        // substituted a max of 200, clamped the expectation to 100 and then re-raised; the bar (hp / 0) drew empty.
        FakeLink l(200);
        Client c(l);
        c.Welcome(200);
        c.ResetV4();
        c.Run(300);
        l.HurtBy(50);
        c.Run(400);
        const bool setup = l.ServerHealth() == 150 && PedIs(c, 150) && BarIs(c, 150);
        const uint32_t applies0 = CountersOf(c.v).maxChanges + CountersOf(c.v).reconciles;
        c.ped.maxReadFails = true;
        float worstBar = 1.0f;
        for (int i = 0; i < 20; ++i)
        {
            c.Tick();
            worstBar = std::min(worstBar, c.Bar());
        }
        c.ped.maxReadFails = false;
        c.Run(2000);
        Check("max read 0: twenty frames of an unreadable maximum change nothing (no self-hit, no write, the bar stays at 75 %, not empty), "
              "and the ped is still right afterwards",
              setup && c.ped.max == 300 && c.ped.Health() == 250 && worstBar > 0.74f && c.selfHits.empty() &&
                  CountersOf(c.v).maxChanges + CountersOf(c.v).reconciles == applies0 && CountersOf(c.v).maxUnreadable == 20,
              Fmt("worst bar %.2f %s unreadable %u", worstBar, Got(c, l).c_str(), CountersOf(c.v).maxUnreadable));
        ++n;
    }
    {
        // A GTA that refuses to keep more than 200 on the player (MEASURED LIVE in 0.4.7), health_mode=direct (the pre-R3-T1 behaviour):
        // the bar stays at 50 % and the tracker must not fight it forever or turn it into damage. R3 T1: every give-up asks for one
        // re-probe (bounded to 3 per ped), so 4 episodes of 3 writes.
        FakeLink l(200);
        Client c(l);
        c.ped.model = Model::Cap200;
        c.hc.wish = HealthMode::Direct;
        c.Welcome(200);
        c.ResetV4();
        c.Run(60000);
        const Cnt st = CountersOf(c.v);
        Check("GTA holds the ped at 200 on a max of 300, health_mode=direct: reconciliation tries 3 times per episode then gives up (counted), at most 3 re-probes, "
              "no self-hit, the server keeps 200, the bar stays at 50 % (the live 0.4.7 state)",
              c.ped.Health() == 200 && c.selfHits.empty() && l.ServerHealth() == 200 && st.reconciles == 12 && st.gaveUp == 4 &&
                  c.v.Stats().reprobes == 3 && c.Bar() < 0.51f && !c.hc.Scaled(),
              Fmt("%s reconciles %u gaveUp %u reprobes %u", Got(c, l).c_str(), st.reconciles, st.gaveUp, c.v.Stats().reprobes));
        ++n;
    }
    for (int mode = 0; mode < 2; ++mode)
    {
        // The two independent ways the tracker learns of a swap, each alone: the changed ped handle (VitalsObs::ped), and the
        // engine's explicit NotePedReplaced. Either one keeps the swap from becoming a self-hit of the max difference.
        FakeLink l(200);
        Client c(l);
        c.tellHandle = mode == 0;
        c.tellSwap = mode == 1;
        c.Welcome(200);
        c.ResetV4();
        c.Run(300);
        c.swapAtGuard = true;
        c.Run(3000);
        Check(mode == 0 ? "swap, the ped handle alone (no explicit notice): no self-hit, the new ped gets the server's 200 on max 300"
                        : "swap, the explicit NotePedReplaced alone (handle not passed): no self-hit, the new ped gets the server's 200 on max 300",
              c.swaps == 1 && c.selfHits.empty() && l.ServerHealth() == 200 && PedIs(c, 200) && c.Bar() > 0.995f, Got(c, l));
        ++n;
    }
    {
        // SET_ENTITY_MAX_HEALTH that fails: logged by the counters, and the tracker works with the maximum the ped really has
        // (200): the server's 200 shows as 200 (a full bar), a hit still lowers it.
        FakeLink l(200);
        Client c(l);
        c.ped.setMaxFails = true;
        c.Welcome(200);
        c.ResetV4();
        c.Run(500);
        const Cnt st = CountersOf(c.v);
        const bool full = c.ped.max == 200 && c.Bar() > 0.995f;
        Check("SET_ENTITY_MAX_HEALTH fails: the failure is counted with the wish and the read-back (300 / 200), the ped keeps its own 200 and "
              "shows a full bar, no self-hit",
              full && st.maxWrites == 1 && st.maxWriteFails == 1 && st.maxReadBackOff == 1 && st.wanted == 300 &&
                  st.readBack == 200 && c.selfHits.empty(),
              Got(c, l));
        ++n;
    }
    {
        // A slow link: a 30-point fall's Damage comes back 90 ticks (1.4 s) late, longer than the absolute check's 1 s. While the self-hit is
        // pending the ped is BELOW the server's number on purpose: the check must wait for the answer (it would put the old value back).
        FakeLink l(200);
        l.ackDelay = 90;
        Client c(l);
        c.Welcome(200);
        c.ResetV4();
        c.Run(500);
        c.ped.Hurt(30);
        c.Run(1300);
        const bool heldDown = c.ped.Health() == 270 && c.selfHits.size() == 1 && c.v.Pending() == 30 && CountersOf(c.v).reconciles == 0;
        c.Run(1500);
        Check("absolute check waits for a pending self-hit: with the ack 1.4 s late the ped stays lowered (270) and nothing is written, then the "
              "ack lands and ped, server and bar agree (170)",
              heldDown && l.ServerHealth() == 170 && PedIs(c, 170) && BarIs(c, 170) && c.selfHits.size() == 1 && c.v.Pending() == 0,
              Got(c, l));
        ++n;
    }
    {
        // Environmental damage next to the absolute check: damage that arrives while a mismatch is being timed is still
        // reported (the check never swallows a loss), and a steady ped at the server's value is never "reconciled".
        FakeLink l(200);
        Client c(l);
        c.Welcome(200);
        c.ResetV4();
        c.Run(5000);
        const uint32_t idleReconciles = CountersOf(c.v).reconciles;
        c.ped.Hurt(80);
        c.Run(1200);
        Check("steady at the server's value for 5 s: zero reconciliations and zero writes beyond the first; a later 80-point fall is one self-hit of 80",
              idleReconciles == 0 && c.selfHits.size() == 1 && c.selfHits[0] == 80 && l.ServerHealth() == 120 && PedIs(c, 120),
              Got(c, l));
        ++n;
    }
    return n;
}

// The whole set against one Env under one GTA model. `full` runs the repeats for the extra playerHealth values.
static void Scenarios(Env& env, bool full, Model m)
{
    for (int ph : {100, 200, 255})
    {
        Join(env, ph, m);
        SwapDefaultPed(env, ph, false, m);
        SwapDefaultPed(env, ph, true, m);
        Fall(env, ph, m);
        if (full || ph == 200)
        {
            FallAfterSwap(env, ph, m);
            LifeCycle(env, ph, false, m);
        }
    }
    LifeCycle(env, 200, true, m);
    Reconnect(env, 100, 200, m);
    Reconnect(env, 200, 255, m);
}

struct FakeEnv final : Env
{
    std::unique_ptr<FakeLink> l;
    Link& Open(int ph) override { l.reset(new FakeLink(ph)); return *l; }
    void Close() override { l.reset(); }
};

// Scenarios() runs: 3 x (join, swap x2, fall) + 3 x (fall after swap, life cycle) with full=true + 1 + 2, under each of the 5 models.
constexpr int kScenarioChecks = 5 * (3 * 4 + 3 * 2 + 1 + 2);
constexpr int kPureOnlyChecks = 9;
constexpr int kPureChecks = kScenarioChecks + kPureOnlyChecks;

static void TestPure()
{
    std::printf("-- T6 health: server -> wire -> client apply -> HUD, simulated ped (5 GTA models), fake server\n");
    FakeEnv env;
    for (Model m : kModels)
        Scenarios(env, true, m);
    PureOnly();
}

}   // namespace t6health

// ---- the real server and the real session --------------------------------------------------------------------------

#ifdef T6_HEALTH_NETWORK
// Needs netrig.h (Rig, Probe) and the netcode_test helpers (Place, Fire, ShotHit, HitOn, HealthOf, IdOf) above this line.
namespace t6health {

struct RigLink final : Link
{
    Rig& rig;
    Probe& alice;
    std::unique_ptr<Probe> me;
    uint16_t id = 0;
    size_t seen = 0;
    std::string name;
    RigLink(Rig& r, Probe& a, const std::string& n) : rig(r), alice(a), name(n) {}
    bool Join()
    {
        me.reset(new Probe(rig));
        if (!me->Join(name.c_str()))
            return false;
        id = IdOf(*me);
        const uint16_t aId = IdOf(alice);
        return Place(rig, alice, Kin(0, 0, 30)) && Place(rig, *me, Kin(2, 0, 30)) &&
               rig.Until([&] { return rig.srv.IsRelevant(aId, id) && rig.srv.IsRelevant(id, aId); }, 3000);   // the shooter must see the target
    }
    uint16_t Me() const override { return id; }
    void Poll(std::vector<proto::Damage>& dmg, std::vector<proto::Killed>& killed, std::vector<proto::RespawnOk>& ok) override
    {
        for (; seen < me->events.size(); ++seen)
        {
            const net::Event& e = me->events[seen];
            if (e.type == ET::Damage) dmg.push_back(e.damage);
            else if (e.type == ET::Killed) killed.push_back(e.killed);
            else if (e.type == ET::RespawnOk) ok.push_back(e.respawn);
        }
    }
    void SendSelfHit(int damage, uint32_t weapon) override { me->s.SendHit(HitOn(id, (uint16_t)damage, proto::HitKind::Player, weapon)); }
    void SendRespawn() override { me->s.SendRespawn(); }
    void Idle() override { for (int i = 0; i < 3; ++i) rig.Step(); }
    void HurtBy(int damage) override
    {
        Fire(rig, alice, Kin(0, 0, 30));
        alice.s.SendHit(ShotHit(alice, id, (uint16_t)damage));
        rig.Pump(60);
    }
    void Heal() override { std::string out; rig.srv.Execute("heal " + name, out); rig.Pump(60); }
    void Kill() override { std::string out; rig.srv.Execute("kill " + name, out); rig.Pump(60); }
    int  ServerHealth() override { return HealthOf(rig, id); }
};

struct RigEnv final : Env
{
    std::unique_ptr<Rig> rig;
    std::unique_ptr<Probe> alice;
    std::unique_ptr<RigLink> link;
    int phNow = -1, n = 0;
    bool ok = true;
    bool Real() const override { return true; }
    Link& Open(int ph) override
    {
        if (!rig || phNow != ph)
        {
            link.reset();
            alice.reset();
            rig.reset(new Rig());
            server::Config c = Rig::BaseConfig();     // exact numbers: percent 100, so the server applies what the shooter claims
            c.playerHealth = (uint16_t)ph;
            c.respawnDelayMs = 300;
            c.maxHitDistance = 100;
            phNow = ph;
            if (!rig->Start(c)) ok = false;
        }
        alice.reset();                                // a fresh shooter per session: the damage budget of a weapon class is per player
        alice.reset(new Probe(*rig));
        if (!alice->Join(("Shooter" + std::to_string(n)).c_str())) ok = false;
        link.reset(new RigLink(*rig, *alice, "Me" + std::to_string(++n)));
        if (!link->Join()) ok = false;
        return *link;
    }
    void Close() override
    {
        if (link && link->me)
        {
            link->me->s.Disconnect();
            link->me->WaitClosed(1000);
        }
        link.reset();
        if (alice)
        {
            alice->s.Disconnect();
            alice->WaitClosed(1000);
        }
        alice.reset();
    }
};

// Scenarios() with full=false: ph 100 and 255 run join / swap x2 / fall, ph 200 adds fall-after-swap and the life cycle; + 1 + 2;
// under the models in kNetModels (Direct: the round 2 chain, Cap200: the live 0.4.7 behaviour).
static const Model kNetModels[] = { Model::Direct, Model::Cap200 };
constexpr int kNetworkChecks = 2 * (3 * 4 + 2 + 1 + 2) + 1;

static void NetworkTests()
{
    std::printf("-- T6 health: the real server (Welcome.playerHealth, Hit, Damage, heal, kill, Respawn) into the client's tracker and HUD\n");
    const auto t0 = std::chrono::steady_clock::now();
    RigEnv env;
    for (Model m : kNetModels)
        Scenarios(env, false, m);
    std::printf("-- T6 health network: %lld ms for %d checks over %d models\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(),
                kNetworkChecks - 1, (int)(sizeof(kNetModels) / sizeof(kNetModels[0])));
    // The wire itself: Welcome carries the configured value (it is what sizes the ped), a self-hit is accepted.
    bool welcome = false;
    {
        Rig rig;
        server::Config c = Rig::BaseConfig();
        c.playerHealth = 255;
        welcome = rig.Start(c);
        Probe p(rig);
        welcome = welcome && p.Join("W") && p.s.playerHealth() == 255 && HealthOf(rig, IdOf(p)) == 255;
    }
    Check("wire: Welcome.playerHealth 255 reaches the session unchanged (a u8: no clamp at 200) and the server's own number for the player is 255",
          welcome);
}

}   // namespace t6health
#endif
