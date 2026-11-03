// melee_sim.h - the engine's per-frame melee / hit composition as a pure, scripted client (0.4.5 melee audit). Test-only,
// header-only. gameplay_test drives it on simulated time; netcode_test drives it against a real in-process Server.
//
// It is a MIRROR of apps/engine/engine.cpp, built from the very libs/sync functions the engine calls:
//   TickLocalMelee   MeleeSwingGate -> ++shotSeq, the last counted swing's time        (engine.cpp TickLocalMelee)
//   ProbePeerHits    MeleeEventWindow, MeleeEventGate, ProbePeerDamage, DecideMeleeHit,
//                    HitBatcher::Add with the Hit's weapon / damage / shotSeq            (engine.cpp ProbePeerHits)
//   hit flush        HitBatcher::Flush every frame                                       (engine.cpp, the g_hits flush)
// The game is the Puppet below: a remote player's ped held at a 1000-point pool, whose "damaged by the local ped" event
// stays set until it is cleared (CLEAR_ENTITY_LAST_DAMAGE_ENTITY), exactly the property the stale-event bug lived on. A
// change to the engine's composition that is not made here makes these tests lie: keep them in step.
#pragma once

#include "sync/combat.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace meleesim {

using namespace gage::sync;

constexpr uint32_t kUnarmedHash = 0xA2719263u;   // WEAPON_UNARMED
constexpr uint32_t kBatHash = 0x958A4A8Fu;       // WEAPON_BAT
constexpr uint32_t kPistolHash = 0x1B06D571u;    // WEAPON_PISTOL

// What GTA keeps for a remote player's ped on the attacker's machine.
struct Puppet
{
    int pool = 1000, health = 1000;
    bool event = false;     // HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(puppet, local ped): set by a blow, stays until cleared
    int blows = 0;
    // A blow: the pool drops by `poolDamage` (0 = the AI melee modifier at 0, fists take nothing off the pool) and the
    // damage event is recorded unless `recordEvent` is false (a blow GTA gave no event for).
    void Blow(int poolDamage, bool recordEvent = true)
    {
        health -= poolDamage;
        if (recordEvent)
            event = true;
        ++blows;
    }
};

struct Sent
{
    double atMs = 0;
    BatchedHit hit;
};

struct Client
{
    MeleeSwingGate gate;
    MeleeEventGate eventGate;
    HitBatcher batcher;
    uint16_t target = 1;
    uint32_t weapon = kUnarmedHash;
    bool held = true;                 // a melee-class weapon in hand (g_meleeHeld); false = a gun
    uint8_t shotSeq = 0;              // g_shotSeq (only the low 4 bits travel)
    double lastSwingMs = -1e9;        // g_lastSwingMs
    bool actionNow = false;           // IS_PED_PERFORMING_MELEE_ACTION
    Puppet pup;
    std::vector<Sent> sent;           // every Hit that left the batcher
    std::function<void(const BatchedHit&)> sink;   // netcode_test: the real session's SendHit
    uint32_t swings = 0, stale = 0, eventReads = 0, hitsEvent = 0, hitsPool = 0;

    // A gun shot (IS_PED_SHOOTING): the counter steps.
    void Shot() { ++shotSeq; }

    // One script-tick frame at `nowMs`: `press` = a melee control pressed this frame, `action` = the melee action runs.
    void Frame(double nowMs, bool press, bool action)
    {
        // TickLocalMelee
        actionNow = held && action;
        if (gate.Tick(nowMs, held && press, actionNow))
        {
            lastSwingMs = nowMs;
            ++shotSeq;
            ++swings;
        }
        // ProbePeerHits
        const double sinceSwing = nowMs - lastSwingMs;
        const bool eventWindow = MeleeEventWindow(held, sinceSwing, actionNow);
        bool eventDamaged = false;
        switch (eventGate.Tick(eventWindow))
        {
        case MeleeEventGate::Step::ClearStale:
            if (pup.event)
            {
                ++stale;
                pup.event = false;
            }
            break;
        case MeleeEventGate::Step::Read:
            ++eventReads;
            eventDamaged = pup.event;
            break;
        case MeleeEventGate::Step::Closed:
            break;
        }
        const int h = pup.health;
        if (!(h == pup.pool && !eventDamaged))
        {
            bool byMe = false;
            if (h > 0 && h < pup.pool)
                byMe = pup.event;
            PeerDamageOut o = ProbePeerDamage(h, pup.pool, byMe, false, kNoDeadCap);
            MeleeHitIn mi;
            mi.meleeWeaponHeld = held;
            mi.eventDamaged = eventDamaged;
            mi.sinceSwingMs = sinceSwing;
            mi.meleeActionNow = actionNow;
            mi.poolReport = o.report;
            mi.poolDamage = o.damage;
            const MeleeHitOut md = DecideMeleeHit(mi);
            o.report = md.source != MeleeHitSource::None;
            if (o.report)
                o.damage = md.damage;
            if (md.source == MeleeHitSource::Event)
                ++hitsEvent;
            else if (md.source == MeleeHitSource::Pool && held)
                ++hitsPool;
            if (o.report)
            {
                BatchedHit hit;
                hit.target = target;
                hit.kind = 1;
                hit.weapon = weapon;
                hit.damage = (uint16_t)(o.damage > 65535 ? 65535 : o.damage);
                hit.bone = -1;
                hit.shotSeq = (uint8_t)(shotSeq & 15);
                batcher.Add(hit, nowMs);
            }
            if (o.damage > 0 || h <= 0 || md.clearEvent)
                pup.event = false;
            if (o.restore)
                pup.health = pup.pool;
        }
        // the hit flush
        BatchedHit out[16];
        const size_t n = batcher.Flush(nowMs, out, 16);
        for (size_t i = 0; i < n; ++i)
        {
            sent.push_back({ nowMs, out[i] });
            if (sink)
                sink(out[i]);
        }
    }
};

} // namespace meleesim
