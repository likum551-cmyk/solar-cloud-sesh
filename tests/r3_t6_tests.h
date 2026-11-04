// r3_t6_tests.h - desync round 3, T6 (melee: "punch animations aint there and hits do no damage"). Test-only, header-only. Three parts, each
// compiled where its helpers exist:
//   R3T6_SERVER  (server_test.cpp, after the netrig helpers)  melee.h as pure units: the track, the judgement (reach, cone, lag
//                compensation, dead / vehicle targets), the swing book, claims, the config keys.
//   R3T6_NET     (netcode_test.cpp)  the REAL server over loopback: a swing with NO Hit message hurts the player in front of it, a swing at a
//                player behind / out of reach does not, a client-reported Hit for a judged blow is not counted twice, a Hit that comes first
//                is not doubled by the judgement, a spamming client is held to the swing gap, and the `melee:` 10 s line. A negative control
//                with meleeAdjudicate 0 (the 0.4.7 behaviour) proves the damage comes from the new path. About 12 s of wall time.
//   R3T6_CLIENT  (gameplay_test.cpp)  the swing ladder against a puppet whose behaviours are switchable (clip exists, secondary honoured, a
//                locomotion task cancels it, the melee task works), mirroring PeerSwingService with the real libs/sync/swing.h classes.
#pragma once

#ifdef R3T6_CLIENT
#include "sync/swing.h"
#endif

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace r3t6 {

static std::string TF(const char* fmt, ...)
{
    char buf[700];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

#ifdef R3T6_SERVER
// ======================================================================================================================================
// pure server units
// ======================================================================================================================================
namespace mel = gage::server::melee;
namespace sac = gage::server::ac;

constexpr int kServerChecks = 27;

// A track of a player standing at (x, y, z) facing `heading` for [fromMs, toMs] at 33 ms samples.
static mel::Track Stand(float x, float y, float heading, double fromMs, double toMs, bool inVehicle = false)
{
    mel::Track t;
    for (double ms = fromMs; ms <= toMs; ms += 33.0)
    {
        mel::Sample s;
        s.ms = ms; s.x = x; s.y = y; s.z = 30.0f; s.heading = heading; s.inVehicle = inVehicle;
        t.Push(s);
    }
    return t;
}

static mel::Verdict Judge(const mel::Track& atk, std::initializer_list<std::pair<uint16_t, const mel::Track*>> targets, uint32_t weapon = sac::Joaat("WEAPON_UNARMED"),
                          uint32_t reachPercent = 100, double renderDelay = mel::kRenderDelayMs, std::initializer_list<uint16_t> deadIds = {})
{
    std::vector<mel::Candidate> c;
    for (const auto& p : targets)
    {
        mel::Candidate cd;
        cd.id = p.first;
        cd.track = p.second;
        for (uint16_t d : deadIds) if (d == p.first) cd.dead = true;
        c.push_back(cd);
    }
    return mel::Adjudicate(atk, 1000.0, weapon, c.data(), c.size(), reachPercent, renderDelay);
}

static void ServerTests()
{
    using mel::Outcome;
    // ---- the track ----
    {
        mel::Track t;
        mel::Sample a, b, o;
        a.ms = 0; a.x = 0; a.y = 0; a.heading = 350;
        b.ms = 100; b.x = 10; b.y = 20; b.heading = 10;
        t.Push(a); t.Push(b);
        t.At(50, o);
        Check("R3 T6 track: a pose between two samples is interpolated (position) and its heading takes the short way round (350 -> 10 at the middle is 0, not 180)",
              std::fabs(o.x - 5.0f) < 1e-3f && std::fabs(o.y - 10.0f) < 1e-3f && (std::fabs(o.heading) < 0.01f || std::fabs(o.heading - 360.0f) < 0.01f),
              TF("x %.2f y %.2f heading %.2f", o.x, o.y, o.heading));
        mel::Sample c;
        c.ms = 50;   // not newer than the newest: ignored
        t.Push(c);
        mel::Sample before, after;
        t.At(-500, before);
        t.At(5000, after);
        Check("R3 T6 track: outside the stored range the oldest / newest sample stands, and a sample that is not newer is ignored",
              t.Size() == 2 && before.x == 0.0f && after.x == 10.0f);
        mel::Track g;
        a.ms = 0; b.ms = 1000; b.heading = 350;   // a gap of 1 s: no interpolation across it
        g.Push(a); g.Push(b);
        g.At(300, o);
        const bool nearA = o.x == 0.0f;
        g.At(800, o);
        Check("R3 T6 track: across a gap longer than 250 ms the nearer sample is used, not a blend of two places (a respawn or a stall is no path)", nearA && o.x == 10.0f);
        mel::Track ring;
        for (int i = 0; i < 100; i++)
        {
            mel::Sample s;
            s.ms = i * 33.0; s.x = (float)i;
            ring.Push(s);
        }
        mel::Sample first;
        ring.At(0, first);
        Check("R3 T6 track: the ring keeps the newest 48 samples (the oldest has gone)", ring.Size() == mel::kTrackCap && first.x == (float)(100 - (int)mel::kTrackCap),
              TF("size %zu, oldest x %.0f", ring.Size(), first.x));
    }
    // ---- the judgement ----
    const mel::Track atk = Stand(0, 0, 0, 0, 2000);                  // at the origin facing +y (heading 0 = (-sin h, cos h) = +y)
    {
        const mel::Track front = Stand(0, 1.0f, 180, 0, 2000), back = Stand(0, -1.0f, 0, 0, 2000), close = Stand(0, -0.6f, 0, 0, 2000);
        const mel::Verdict f = Judge(atk, {{7, &front}});
        Check("R3 T6 judge: a player 1 m in front of a fist is hit (at 1.0 m, straight ahead)", f.outcome == Outcome::Hit && f.target == 7 && std::fabs(f.distM - 1.0f) < 0.05f,
              TF("%s %.2f m", mel::OutcomeName(f.outcome), f.distM));
        const mel::Verdict b = Judge(atk, {{7, &back}});
        Check("R3 T6 judge: a player 1 m BEHIND the attacker is not hit (outside the facing cone), and the verdict says Behind", b.outcome == Outcome::Behind,
              mel::OutcomeName(b.outcome));
        const mel::Verdict c = Judge(atk, {{7, &close}});
        Check("R3 T6 judge: inside 0.9 m the bodies overlap and facing is not required (0.6 m behind is hit)", c.outcome == Outcome::Hit);
    }
    {
        const mel::Track at20 = Stand(0, 2.0f, 180, 0, 2000), at24 = Stand(0, 2.4f, 180, 0, 2000);
        const mel::Verdict fist = Judge(atk, {{7, &at20}});
        const mel::Verdict knife = Judge(atk, {{7, &at20}}, sac::Joaat("WEAPON_KNIFE"));
        const mel::Verdict knife24 = Judge(atk, {{7, &at24}}, sac::Joaat("WEAPON_KNIFE"));
        const mel::Verdict bat24 = Judge(atk, {{7, &at24}}, sac::Joaat("WEAPON_BAT"));
        Check("R3 T6 judge: reach follows the weapon class (centre to centre, plus 0.5 m slack): at 2.0 m a fist (1.4 + 0.5) misses and a knife (1.7 + 0.5) hits; at 2.4 m the knife misses and a bat (2.1 + 0.5) hits",
              fist.outcome == Outcome::NoTarget && knife.outcome == Outcome::Hit && knife24.outcome == Outcome::NoTarget && bat24.outcome == Outcome::Hit,
              TF("fist@2.0 %s knife@2.0 %s knife@2.4 %s bat@2.4 %s", mel::OutcomeName(fist.outcome), mel::OutcomeName(knife.outcome), mel::OutcomeName(knife24.outcome),
                 mel::OutcomeName(bat24.outcome)));
        const mel::Track at15 = Stand(0, 1.5f, 180, 0, 2000);
        const mel::Verdict full = Judge(atk, {{7, &at15}});
        const mel::Verdict half = Judge(atk, {{7, &at15}}, sac::Joaat("WEAPON_UNARMED"), 50);
        Check("R3 T6 judge: meleeReachPercent scales the reach (1.5 m is a hit at 100 %, a miss at 50 %), and a miss reports how far the nearest player was (the tuning number)",
              full.outcome == Outcome::Hit && half.outcome == Outcome::NoTarget && std::fabs(half.distM - 1.5f) < 0.05f, TF("%.2f m", half.distM));
    }
    {
        const mel::Track nearT = Stand(0, 1.2f, 180, 0, 2000), farT = Stand(0, 0.5f, 180, 0, 2000), side = Stand(1.0f, 0.2f, 180, 0, 2000);
        const mel::Verdict v = Judge(atk, {{3, &nearT}, {4, &farT}});
        Check("R3 T6 judge: of two players in reach the nearest in front is the one hit, and only one hit is judged per swing", v.outcome == Outcome::Hit && v.target == 4, TF("target %u", (unsigned)v.target));
        const mel::Verdict alone = Judge(atk, {{5, &side}});
        const mel::Verdict s2 = Judge(atk, {{5, &side}, {3, &nearT}});
        Check("R3 T6 judge: a player 1 m to the side (79 degrees off the heading, outside the 65 degree cone) is not hit alone, and does not take the blow from the one straight ahead",
              alone.outcome == Outcome::Behind && s2.outcome == Outcome::Hit && s2.target == 3, TF("alone %s, with one ahead %s target %u", mel::OutcomeName(alone.outcome),
                                                                                             mel::OutcomeName(s2.outcome), (unsigned)s2.target));
    }
    {
        const mel::Track t1 = Stand(0, 1.0f, 180, 0, 2000), t2 = Stand(0, 1.3f, 180, 0, 2000), up = Stand(0, 1.0f, 180, 0, 2000);
        const mel::Verdict dead = Judge(atk, {{7, &t1}}, sac::Joaat("WEAPON_UNARMED"), 100, mel::kRenderDelayMs, {7});
        Check("R3 T6 judge: a dead player in reach is not hit (TargetDead)", dead.outcome == Outcome::TargetDead, mel::OutcomeName(dead.outcome));
        const mel::Verdict alive = Judge(atk, {{7, &t1}, {8, &t2}}, sac::Joaat("WEAPON_UNARMED"), 100, mel::kRenderDelayMs, {7});
        Check("R3 T6 judge: a dead player next to a living one does not shield it (the living one is hit)", alive.outcome == Outcome::Hit && alive.target == 8);
        const mel::Track veh = Stand(0, 1.0f, 180, 0, 2000, true);
        const mel::Verdict iv = Judge(atk, {{7, &veh}});
        Check("R3 T6 judge: a player in a vehicle is not hit by a fist (TargetInVehicle)", iv.outcome == Outcome::TargetInVehicle, mel::OutcomeName(iv.outcome));
        mel::Track high;
        for (double ms = 0; ms <= 2000; ms += 33)
        {
            mel::Sample s;
            s.ms = ms; s.y = 1.0f; s.z = 33.0f;
            high.Push(s);
        }
        const mel::Verdict hv = Judge(atk, {{7, &high}});
        Check("R3 T6 judge: a player 3 m above (another floor) is no target", hv.outcome == Outcome::NoTarget, mel::OutcomeName(hv.outcome));
        const mel::Track drive = Stand(0, 0, 0, 0, 2000, true);
        const mel::Verdict av = Judge(drive, {{7, &t1}});
        Check("R3 T6 judge: an attacker in a vehicle at the blow throws no punch (AttackerOut)", av.outcome == Outcome::AttackerOut, mel::OutcomeName(av.outcome));
    }
    {
        // Lag compensation: the victim runs away along +y at 8 m/s. At the blow (1000 ms) he is really at 2.4 m, but the attacker's screen
        // shows him kRenderDelayMs earlier, at 1.6 m: a hit as the attacker saw it. Without the compensation the punch lands on air.
        mel::Track run;
        for (double ms = 0; ms <= 2000; ms += 33)
        {
            mel::Sample s;
            s.ms = ms; s.y = 0.008f * (float)ms - 5.6f; s.z = 30.0f; s.heading = 0;
            run.Push(s);
        }
        const mel::Verdict comp = Judge(atk, {{7, &run}});
        const mel::Verdict raw = Judge(atk, {{7, &run}}, sac::Joaat("WEAPON_UNARMED"), 100, 0.0);
        Check("R3 T6 judge: lag compensation: a victim running away is judged where the attacker saw him (render delay back), not where he is now: hit with the delay, a miss without",
              comp.outcome == Outcome::Hit && raw.outcome != Outcome::Hit, TF("with %s (%.2f m), without %s (%.2f m)", mel::OutcomeName(comp.outcome), comp.distM,
                                                                              mel::OutcomeName(raw.outcome), raw.distM));
    }
    // ---- the swing book ----
    {
        mel::SwingBook b;
        const uint32_t fist = sac::Joaat("WEAPON_UNARMED");
        auto o1 = b.Observe(1000, 1, false, true, 1, fist);
        auto none = b.Observe(1010, 0, false, true, 1, fist);
        auto gun = b.Observe(5000, 1, false, false, 2, sac::Joaat("WEAPON_PISTOL"));
        Check("R3 T6 swings: a counter step with a melee weapon is one swing to judge; no step is none; a step with a gun is no swing",
              o1.seen == 1 && o1.queued == 1 && none.seen == 0 && gun.seen == 0 && b.PendingCount() == 1);
        auto close = b.Observe(1100, 1, false, true, 2, fist);
        auto later = b.Observe(1400, 1, false, true, 3, fist);
        Check("R3 T6 swings: a second step 100 ms later is the same swing spammed (too close, not judged); one 400 ms later is a new swing",
              close.seen == 1 && close.queued == 0 && close.tooClose == 1 && later.queued == 1 && b.PendingCount() == 2);
        mel::SwingBook f;
        auto edge = f.Observe(2000, 0, true, true, 4, fist);
        auto stepAfter = f.Observe(2050, 1, false, true, 5, fist);
        mel::SwingBook g;
        auto step = g.Observe(3000, 1, false, true, 4, fist);
        auto edgeAfter = g.Observe(3100, 0, true, true, 4, fist);
        Check("R3 T6 swings: a kPfMelee edge alone is a swing; a counter step right after it (or an edge right after a step) is the same swing, never two",
              edge.queued == 1 && stepAfter.queued == 0 && step.queued == 1 && edgeAfter.seen == 0 && f.PendingCount() == 1 && g.PendingCount() == 1);
        mel::SwingBook t;
        t.Observe(10000, 1, false, true, 6, fist);
        mel::SwingBook::Pending p;
        const bool early = t.Take(10100, 10100, p);
        const bool bySample = t.Take(10100, 10230, p);
        mel::SwingBook t2;
        t2.Observe(10000, 1, false, true, 6, fist);
        const bool byClockEarly = t2.Take(10300, 10100, p);
        const bool byClock = t2.Take(10320, 10100, p);
        Check("R3 T6 swings: a swing is judged at its blow time: not at 100 ms, at 220 ms once the attacker's sample for it is in, or 90 ms after the blow time on the clock when that sample never comes",
              !early && bySample && !byClockEarly && byClock);
        mel::SwingBook q;
        uint32_t queued = 0;
        for (int i = 0; i < 8; i++) queued += q.Observe(20000.0 + i * 300.0, 1, false, true, (uint8_t)i, fist).queued;
        Check("R3 T6 swings: at most kMaxPending swings wait for their blow (a stalled judge cannot be made to grow without bound)", queued == mel::kMaxPending && q.PendingCount() == mel::kMaxPending);
    }
    // ---- claims ----
    {
        mel::SwingBook b;
        const uint32_t fist = sac::Joaat("WEAPON_UNARMED");
        const double now = 1000;
        const auto unknown = mel::ClassifyClaim(b, 3, 7, now);
        b.Observe(1000, 1, false, true, 3, fist);
        const auto before = mel::ClassifyClaim(b, 3, 7, now + 100);
        mel::SwingBook::Pending p;
        b.Take(1400, 1400, p);
        mel::Verdict hit;
        hit.outcome = Outcome::Hit;
        hit.target = 7;
        b.Record(p, hit);
        const auto confirms = mel::ClassifyClaim(b, 3, 7, now + 300);
        const auto other = mel::ClassifyClaim(b, 3, 9, now + 300);
        mel::SwingBook m;
        m.Observe(1000, 1, false, true, 5, fist);
        m.Take(1400, 1400, p);
        mel::Verdict miss;
        miss.outcome = Outcome::NoTarget;
        miss.distM = 2.6f;
        m.Record(p, miss);
        const auto contra = mel::ClassifyClaim(m, 5, 7, now + 300);
        const auto expired = mel::ClassifyClaim(b, 3, 7, now + 6000);
        Check("R3 T6 claims: a Hit naming no swing the book holds is Unknown; one that comes before the judgement is BeforeJudging",
              unknown == mel::ClaimKind::Unknown && before == mel::ClaimKind::BeforeJudging);
        Check("R3 T6 claims: a Hit for the swing AND target the server hit is Confirms (no second count), for another target OtherTarget, for a swing judged a miss Contradicts, and an old swing is forgotten",
              confirms == mel::ClaimKind::Confirms && other == mel::ClaimKind::OtherTarget && contra == mel::ClaimKind::Contradicts && expired == mel::ClaimKind::Unknown);
    }
    // ---- the config keys ----
    {
        gage::server::Config d;
        std::string err;
        gage::server::Config bad1 = d, bad2 = d, bad3 = d;
        bad1.meleeAdjudicate = 2;
        bad2.meleeReachPercent = 24;
        bad3.meleeReachPercent = 401;
        const bool okDefault = gage::server::Validate(d, &err);
        Check("R3 T6 config: meleeAdjudicate defaults on (1) and meleeReachPercent to 100; meleeAdjudicate 2, reach 24 and reach 401 are refused",
              okDefault && d.meleeAdjudicate == 1 && d.meleeReachPercent == 100 && !gage::server::Validate(bad1, &err) && !gage::server::Validate(bad2, &err) &&
                  !gage::server::Validate(bad3, &err));
        const std::string path = "r3t6_cfg.json";
        WriteTextFile(path, "{ \"meleeAdjudicate\": 0, \"meleeReachPercent\": 150 }");
        gage::server::Config c;
        const bool loaded = gage::server::LoadConfigFile(path, c, &err) && c.meleeAdjudicate == 0 && c.meleeReachPercent == 150;
        int rejected = 0;
        for (const char* text : { "{ \"meleeAdjudicate\": 2 }", "{ \"meleeAdjudicate\": 0.5 }", "{ \"meleeReachPercent\": 10 }", "{ \"meleeReachPercent\": 500 }" })
        {
            WriteTextFile(path, text);
            gage::server::Config cc;
            cc.maxVehicles = 77;
            std::string e;
            if (!gage::server::LoadConfigFile(path, cc, &e) && cc.maxVehicles == 77) rejected++;
        }
        std::remove(path.c_str());
        Check("R3 T6 config: both keys load from server.json, and bad values are refused untouched (4 cases)", loaded && rejected == 4, err);
    }
    // ---- the refusal counters cover the paths a melee Hit can die on ----
    {
        // Every HitReason has a name (the `combat:` line prints them) and the one added by T6 is among them.
        bool named = true;
        for (size_t i = 0; i < gage::server::kHitReasons; i++)
            if (std::string(gage::server::HitReasonName((gage::server::HitReason)i)) == "?") named = false;
        Check("R3 T6 counters: every refusal reason has a log name, including defer_dropped (a Hit that waited for its shot and died with its shooter used to vanish uncounted)",
              named && std::string(gage::server::HitReasonName(gage::server::HitReason::DeferDropped)) == "defer_dropped");
    }
}
#endif  // R3T6_SERVER

#ifdef R3T6_NET
// ======================================================================================================================================
// the real server, over loopback
// ======================================================================================================================================
constexpr int kNetChecks = 12;

static constexpr uint32_t kFist = 0xA2719263u;   // WEAPON_UNARMED

// One attacker's states for `ms` of wall time every 30 ms: position/heading `k`, the fist, the counter `seq`, kPfMelee while `flagUntil` has not passed.
static void SendFor(Rig& rig, Probe& atk, const proto::Kinematics& k, uint8_t seq, int ms, int flagMs = 0)
{
    const auto t0 = Clock::now();
    auto next = t0;
    while (MsSince(t0) < ms)
    {
        if (Clock::now() >= next)
        {
            proto::PedState ped;
            ped.weapon = kFist;
            ped.shotSeq = seq & 15;
            if (MsSince(t0) < flagMs) ped.flags |= proto::kPfMelee;
            atk.s.SendState(k, ped);
            next += std::chrono::milliseconds(30);
        }
        rig.Step();
    }
}

static int MeleeDamages(const Probe& p, uint16_t attacker)
{
    int n = 0;
    for (const proto::Damage& d : p.Damages())
        if (d.attackerId == attacker && (d.flags & proto::kDmgMelee) && d.damage > 0) n++;
    return n;
}

static server::Config AdjConfig(bool on)
{
    server::Config c = ShippedCombatConfig();
    c.meleeAdjudicate = on ? 1 : 0;
    return c;
}

static void NetTests()
{
    using gage::server::melee::Outcome;
    const proto::Kinematics atkAt = Kin(0, 0, 30, 0), atkAway = Kin(0, 0, 30, 180);
    {
        Rig rig;
        if (!rig.Start(AdjConfig(true))) { Check("R3 T6 net: server starts", false); return; }
        Probe alice(rig), bob(rig), carol(rig);
        if (!alice.Join("Alice") || !bob.Join("Bob") || !carol.Join("Carol")) { Check("R3 T6 net: three players join", false); return; }
        const uint16_t aId = IdOf(alice), bId = IdOf(bob), cId = IdOf(carol);
        const bool placed = Place(rig, alice, atkAt) && Place(rig, bob, Kin(0, 1.0f, 30, 180)) && Place(rig, carol, Kin(8, 0, 30, 0));
        const bool rel = rig.Until([&] { return rig.srv.IsRelevant(aId, bId); }, 3000);
        if (!placed || !rel) { Check("R3 T6 net: players placed and relevant", false); return; }
        const server::Stats& st = rig.srv.stats();
        SendFor(rig, alice, atkAt, 0, 500);   // the baseline counter value and the held weapon

        // 1. a swing, NO Hit message: Bob (1.0 m in front) is hurt by the server's own judgement; kPfMelee rising with the step is the same swing
        SendFor(rig, alice, atkAt, 1, 700, 350);
        const int dmg1 = MeleeDamages(bob, aId);
        uint16_t amount = 0;
        for (const proto::Damage& d : bob.Damages()) if (d.attackerId == aId) amount = d.damage;
        const server::Config shipped = ShippedCombatConfig();
        const uint16_t expect = (uint16_t)(server::ac::MeleeNominalMax(kFist, shipped.hitDamageScalePercent) * shipped.meleeDamagePercent / 100);
        Check("R3 T6 net: a swing with NO Hit message hurts the player 1 m in front of it: the victim gets exactly one Damage (melee flag) of the scaled fist number, from the server's judgement",
              dmg1 == 1 && amount == expect && st.meleeOutcome[(size_t)Outcome::Hit] == 1 && st.hitsAccepted == 1 && st.meleeSwingsSeen == 1,
              TF("damages %d of %u (want %u), judged hits %llu, accepted %llu, swings seen %llu", dmg1, (unsigned)amount, (unsigned)expect,
                 (unsigned long long)st.meleeOutcome[(size_t)Outcome::Hit], (unsigned long long)st.hitsAccepted, (unsigned long long)st.meleeSwingsSeen));
        int carolHurt = 0;
        for (const proto::Damage& d : alice.Damages()) if (d.victimId == cId) carolHurt++;
        for (const proto::Damage& d : carol.Damages()) if (d.victimId == cId) carolHurt++;
        Check("R3 T6 net: the ATTACKER is told too (the same Damage reaches Alice: the confirmed-damage counter of the client line), and the bystander 8 m away is not hurt",
              MeleeDamages(alice, aId) == 1 && carolHurt == 0, TF("alice %d, carol hurt %d", MeleeDamages(alice, aId), carolHurt));

        // 2. a Hit for that same blow (the game's damage event reaching the client late) is the same blow: not counted twice
        proto::Hit h;
        h.kind = proto::HitKind::Player; h.targetId = bId; h.weapon = kFist; h.damage = 40; h.shotSeq = 1;
        alice.s.SendHit(h);
        SendFor(rig, alice, atkAt, 1, 400);
        Check("R3 T6 net: a client-reported Hit for the swing and target the server already hit is a confirmation: no second Damage, counted as such",
              MeleeDamages(bob, aId) == 1 && st.meleeClaimConfirm == 1, TF("damages %d, confirms %llu", MeleeDamages(bob, aId), (unsigned long long)st.meleeClaimConfirm));

        // 3. a swing with Bob BEHIND the attacker: no damage, judged Behind
        SendFor(rig, alice, atkAway, 2, 700);
        Check("R3 T6 net: a swing at a player behind the attacker (outside the facing cone) does no damage and is judged Behind",
              MeleeDamages(bob, aId) == 1 && st.meleeOutcome[(size_t)Outcome::Behind] == 1,
              TF("damages %d, behind %llu", MeleeDamages(bob, aId), (unsigned long long)st.meleeOutcome[(size_t)Outcome::Behind]));

        // 4. out of reach (5 m away)
        Place(rig, bob, Kin(0, 5.0f, 30, 180));
        SendFor(rig, alice, atkAt, 3, 700);
        Check("R3 T6 net: a swing at a player 5 m away does no damage and is judged NoTarget (the miss is counted by reason)",
              MeleeDamages(bob, aId) == 1 && st.meleeOutcome[(size_t)Outcome::NoTarget] == 1,
              TF("damages %d, no_target %llu", MeleeDamages(bob, aId), (unsigned long long)st.meleeOutcome[(size_t)Outcome::NoTarget]));

        // 5. a Hit that comes BEFORE the judgement pays; the judgement then skips the swing (one hit, not two)
        Place(rig, bob, Kin(0, 1.0f, 30, 180));
        std::string out;
        rig.srv.Execute("heal Bob", out);
        const int before5 = MeleeDamages(bob, aId);
        {
            const auto t0 = Clock::now();
            auto next = t0;
            bool sentHit = false;
            while (MsSince(t0) < 800)
            {
                if (Clock::now() >= next)
                {
                    proto::PedState ped;
                    ped.weapon = kFist;
                    ped.shotSeq = 4;
                    alice.s.SendState(atkAt, ped);
                    next += std::chrono::milliseconds(30);
                    if (!sentHit && MsSince(t0) >= 60)   // 60 ms after the press: well before the 220 ms blow time
                    {
                        h.shotSeq = 4;
                        alice.s.SendHit(h);
                        sentHit = true;
                    }
                }
                rig.Step();
            }
        }
        Check("R3 T6 net: a client-reported Hit that arrives before the server's judgement pays for the swing and the judgement skips it: one Damage, claimed_first counted",
              MeleeDamages(bob, aId) == before5 + 1 && st.meleeClaimFirst == 1 && st.meleeOutcome[(size_t)Outcome::ClaimedFirst] == 1,
              TF("damages +%d, claimed first %llu, judged claimed_first %llu", MeleeDamages(bob, aId) - before5, (unsigned long long)st.meleeClaimFirst,
                 (unsigned long long)st.meleeOutcome[(size_t)Outcome::ClaimedFirst]));

        // 6. a client that steps the counter every 30 ms is held to the swing gap
        rig.srv.Execute("heal Bob", out);
        const uint64_t judgedBefore = st.meleeJudged, closeBefore = st.meleeSwingsTooClose;
        const int before6 = MeleeDamages(bob, aId);
        {
            uint8_t seq = 4;
            const auto t0 = Clock::now();
            auto next = t0;
            while (MsSince(t0) < 1100)
            {
                if (Clock::now() >= next && MsSince(t0) < 330)
                {
                    proto::PedState ped;
                    ped.weapon = kFist;
                    ped.shotSeq = (uint8_t)(++seq & 15);
                    alice.s.SendState(atkAt, ped);
                    next += std::chrono::milliseconds(30);
                }
                else if (Clock::now() >= next)
                {
                    proto::PedState ped;
                    ped.weapon = kFist;
                    ped.shotSeq = (uint8_t)(seq & 15);
                    alice.s.SendState(atkAt, ped);
                    next += std::chrono::milliseconds(30);
                }
                rig.Step();
            }
        }
        const uint64_t judged6 = st.meleeJudged - judgedBefore;
        Check("R3 T6 net: a client stepping its swing counter every 30 ms (11 steps in 330 ms) gets at most 2 swings judged and at most 2 hits; the rest are counted as too close",
              judged6 >= 1 && judged6 <= 2 && MeleeDamages(bob, aId) - before6 <= 2 && st.meleeSwingsTooClose - closeBefore >= 8,
              TF("judged %llu, damages +%d, too close +%llu", (unsigned long long)judged6, MeleeDamages(bob, aId) - before6,
                 (unsigned long long)(st.meleeSwingsTooClose - closeBefore)));

        // 7. the server explains itself: the `melee:` 10 s line
        rig.Until([&] { return rig.LogHas("melee: last 10 s"); }, 6000);
        std::string line;
        for (const std::string& l : rig.logs)
            if (l.find("melee: last 10 s") != std::string::npos) line = l;
        Check("R3 T6 net: the server's 10 s `melee:` line says what happened to the swings: swings seen, judged by outcome (hit, behind, no_target, claimed_first), the client-reported Hits (confirming, paid before) and the damage",
              line.find("swings seen") != std::string::npos && line.find("hit=") != std::string::npos && line.find("behind=1") != std::string::npos &&
                  line.find("no_target=1") != std::string::npos && line.find("confirming a judged hit 1") != std::string::npos &&
                  line.find("paid before the judgement 1") != std::string::npos && line.find("adjudication on") != std::string::npos,
              line);
        Check("R3 T6 net: the first judged hits and near misses are logged with distance and angle (the tuning data), once per swing",
              rig.LogHas("judged hit on id") && rig.LogHas("degrees off the heading"));
    }
    {
        // negative control: the 0.4.7 server (meleeAdjudicate 0) does nothing for the same swing without a Hit
        Rig rig;
        if (!rig.Start(AdjConfig(false))) { Check("R3 T6 net: control server starts", false); return; }
        Probe alice(rig), bob(rig);
        if (!alice.Join("Alice") || !bob.Join("Bob")) { Check("R3 T6 net: control players join", false); return; }
        const uint16_t aId = IdOf(alice), bId = IdOf(bob);
        const bool placed = Place(rig, alice, atkAt) && Place(rig, bob, Kin(0, 1.0f, 30, 180));
        rig.Until([&] { return rig.srv.IsRelevant(aId, bId); }, 3000);
        SendFor(rig, alice, atkAt, 0, 400);
        SendFor(rig, alice, atkAt, 1, 800, 350);
        const server::Stats& st = rig.srv.stats();
        Check("R3 T6 net (control): with meleeAdjudicate 0 (the 0.4.7 behaviour) the same swing with no Hit message hurts nobody: this is the defect the judgement fixes",
              placed && MeleeDamages(bob, aId) == 0 && st.meleeJudged == 0 && st.meleeSwingsSeen == 0 && st.hitsAccepted == 0);
        // ... and the client-reported path still works with it off
        proto::Hit h;
        h.kind = proto::HitKind::Player; h.targetId = bId; h.weapon = kFist; h.damage = 40; h.shotSeq = 1;
        alice.s.SendHit(h);
        rig.Until([&] { return MeleeDamages(bob, aId) >= 1; }, 1500);
        Check("R3 T6 net (control): ... and a client-reported Hit for that swing is still accepted the old way (the claim path is unchanged)",
              MeleeDamages(bob, aId) == 1 && st.hitsAccepted == 1 && st.meleeClaimConfirm == 0);
    }
    {
        // a Hit that waited for a shot and died with its shooter is counted (it used to vanish)
        Rig rig;
        if (!rig.Start(AdjConfig(true))) { Check("R3 T6 net: server starts (defer)", false); return; }
        Probe alice(rig), bob(rig);
        if (!alice.Join("Alice") || !bob.Join("Bob")) { Check("R3 T6 net: defer players join", false); return; }
        const uint16_t aId = IdOf(alice), bId = IdOf(bob);
        Place(rig, alice, atkAt);
        Place(rig, bob, Kin(0, 1.0f, 30, 180));
        rig.Until([&] { return rig.srv.IsRelevant(aId, bId); }, 3000);
        SendFor(rig, alice, atkAt, 0, 300);
        proto::Hit h;
        h.kind = proto::HitKind::Player; h.targetId = bId; h.weapon = kFist; h.damage = 40; h.shotSeq = 5;   // a swing the server will see soon (5 ahead of its counter): the Hit waits for it
        alice.s.SendHit(h);
        rig.Pump(60);
        std::string out;
        rig.srv.Execute("kill Alice", out);
        rig.Pump(500);
        const server::Stats& st = rig.srv.stats();
        Check("R3 T6 net: a melee Hit that was waiting for its swing when its shooter died is counted as defer_dropped (hitRefusals sums to hitsRejected)",
              st.hitRefusals[(size_t)server::HitReason::DeferDropped] == 1 && st.hitsRejected == 1, TF("dropped %llu rejected %llu",
              (unsigned long long)st.hitRefusals[(size_t)server::HitReason::DeferDropped], (unsigned long long)st.hitsRejected));
    }
}
#endif  // R3T6_NET

#ifdef R3T6_CLIENT
// ======================================================================================================================================
// the swing ladder against a puppet with switchable behaviours
// ======================================================================================================================================
constexpr int kClientChecks = 13;

// The game, as far as a swing is concerned. Each flag is one hypothesis about GTA that the engine must survive.
struct SimPuppet
{
    bool clipExists = true;          // the INFERRED clip name is right
    bool secondaryHonoured = true;   // an upper-body + secondary TASK_PLAY_ANIM shows (IS_ENTITY_PLAYING_ANIM true)
    bool locoCancels = false;        // a locomotion task issued during the swing replaces a clip that is not "not interruptable" (flag 8)
    bool fullBodyWorks = true;       // flag 8 alone plays
    bool meleeTaskWorks = true;      // TASK_PUT_PED_DIRECTLY_INTO_MELEE puts the puppet in melee combat
    bool ragdoll = false;            // the ped ragdolls: nothing plays
    int readback = 1;                // 1 normal, -1 the read-back native cannot be called
    double locoEveryMs = 0;          // the locomotion controller issues a task this often (0 = never): T2's "13 in 10 s" is ~770 ms
};

// Mirrors PlayPeerSwing / IssuePeerSwing / PeerSwingService (apps/engine/engine.cpp) with the real classes. Time in ms.
struct SwingSim
{
    SimPuppet game;
    gage::sync::SwingLadder ladder;
    gage::sync::SwingBook book;
    gage::sync::SwingProbe probe;
    gage::sync::SwingDiag diag;
    gage::sync::SwingVariant issuedVariant = gage::sync::SwingVariant::Upper;
    std::vector<int> variants;       // the variant of every issued swing, in order (the no-oscillation check)
    uint32_t locoTasks = 0, locoMark = 0;
    double issuedMs = 0, nextLoco = 0;
    bool retried = false;
    bool taskAlive = false;          // the swing clip / task is still in the puppet's task tree
    double clipEndMs = 0;

    SwingSim() { book.Reset(2); book.SetExists(0, 1); book.SetExists(1, 1); }

    bool Playing(double now) const
    {
        using gage::sync::SwingVariant;
        if (game.ragdoll || !taskAlive || now >= clipEndMs) return false;
        if (issuedVariant == SwingVariant::MeleeTask) return game.meleeTaskWorks;
        if (!game.clipExists && book.Pick() == 0) return false;
        if (issuedVariant == SwingVariant::FullBody) return game.fullBodyWorks;
        return game.secondaryHonoured;
    }
    void Issue(double now, bool retry)
    {
        using gage::sync::SwingVariant;
        const SwingVariant v = ladder.Current();
        taskAlive = true;
        clipEndMs = now + 400;
        if (retry) ++diag.retried; else { ++diag.issued; ++diag.variantIssued[(size_t)v]; retried = false; }
        issuedVariant = v;
        locoMark = locoTasks;
        issuedMs = now;
        probe.Begin(now, (size_t)(book.Pick() < 0 ? 0 : book.Pick()));
        if (!retry) variants.push_back((int)v);
    }
    // A locomotion task is issued on the puppet: it replaces a swing that is not "not interruptable".
    void LocoTask(double now)
    {
        ++locoTasks;
        using gage::sync::SwingVariant;
        if (game.locoCancels && issuedVariant != SwingVariant::UpperHard && issuedVariant != SwingVariant::MeleeTask) taskAlive = false;
        (void)now;
    }
    // One swing seen at `t0`, 16 ms frames for 300 ms.
    void Swing(double t0)
    {
        using namespace gage::sync;
        ++diag.seen;
        if (!ladder.Enabled()) { ++diag.notIssuedOff; return; }
        if (game.ragdoll) { ++diag.notIssuedRagdoll; return; }
        Issue(t0, false);
        if (game.locoEveryMs > 0) nextLoco = t0 + 8;      // the controller's next task lands 8 ms after the swing (before the first read-back)
        for (double now = t0 + 16; now <= t0 + 300; now += 16)
        {
            if (game.locoEveryMs > 0)
                while (nextLoco <= now) { LocoTask(nextLoco); nextLoco += game.locoEveryMs; }
            const int playing = game.readback < 0 ? -1 : Playing(now) ? 1 : 0;
            const SwingProbe::Result r = probe.Tick(now, playing);
            if (r == SwingProbe::Result::Pending)
            {
                if (SwingRetryDue(issuedVariant == SwingVariant::MeleeTask, retried, playing, now - issuedMs, locoTasks, locoMark))
                {
                    retried = true;
                    Issue(now, true);
                }
                continue;
            }
            if (r == SwingProbe::Result::Shown)
            {
                ++diag.shown;
                ++diag.variantShown[(size_t)issuedVariant];
                if (retried) ++diag.retriedShown;
                ladder.OnResult(r, false);
                book.OnProbe(probe.Candidate(), r);
            }
            else if (r == SwingProbe::Result::NotShown)
            {
                ++diag.unseen;
                const bool busy = game.ragdoll;
                const SwingMiss why = ClassifySwingMiss(busy, retried, locoTasks, locoMark);
                if (why == SwingMiss::Busy) ++diag.unseenBusy; else if (why == SwingMiss::Overwritten) ++diag.unseenOverwritten; else ++diag.unseenPlain;
                const bool demoted = issuedVariant != SwingVariant::MeleeTask && why == SwingMiss::Plain ? book.OnProbe(probe.Candidate(), r) : false;
                if (!demoted && ladder.OnResult(r, busy)) ++diag.escalations;
            }
            else if (r == SwingProbe::Result::Unknown)
                ++diag.unreadable;
            if (r != SwingProbe::Result::Idle) break;
        }
    }
    // n swings 600 ms apart
    void Swings(int n, double start = 0)
    {
        for (int i = 0; i < n; i++) Swing(start + i * 600.0);
    }
    bool Monotone() const
    {
        for (size_t i = 1; i < variants.size(); i++)
            if (variants[i] < variants[i - 1]) return false;
        return true;
    }
};

static void ClientTests()
{
    using namespace gage::sync;
    {
        SwingSim s;
        s.Swings(10);
        Check("R3 T6 ladder: a puppet that plays the upper-body clip: 10 swings, 10 SEEN playing, no escalation, the first variant stays (the 0c behaviour is the default)",
              s.diag.issued == 10 && s.diag.shown == 10 && s.diag.escalations == 0 && s.ladder.Current() == SwingVariant::Upper && s.ladder.Pinned(),
              TF("issued %u shown %u escalations %u", s.diag.issued, s.diag.shown, s.diag.escalations));
    }
    {
        SwingSim s;
        s.game.secondaryHonoured = false;           // the upper-body secondary task is ignored; full body works
        s.Swings(14);
        Check("R3 T6 ladder: secondary tasks not honoured: the ladder climbs Upper -> UpperHard -> FullBody by the read-back alone and pins FullBody, SEEN playing from then on; every issued swing is counted, only the seen ones as shown",
              s.ladder.Current() == SwingVariant::FullBody && s.ladder.Pinned() && s.diag.escalations == 2 && s.diag.shown >= 8 && s.diag.issued == 14 &&
                  s.diag.variantShown[(size_t)SwingVariant::Upper] == 0 && s.diag.variantShown[(size_t)SwingVariant::FullBody] >= 8,
              TF("variant %d, escalations %u, shown %u of %u, full-body shown %u", (int)s.ladder.Current(), s.diag.escalations, s.diag.shown, s.diag.issued,
                 s.diag.variantShown[(size_t)SwingVariant::FullBody]));
        Check("R3 T6 ladder: no oscillation: the variant of the swings never goes back down while climbing",
              s.Monotone());
    }
    {
        SwingSim s;
        s.game.locoCancels = true;
        s.game.locoEveryMs = 10;                    // a locomotion task every frame replaces any swing that may be interrupted (the retry cannot outlive it)
        s.Swings(10);
        Check("R3 T6 ladder: a locomotion task re-issued every frame cancels the plain upper-body swing: the read-back counts them as 'overwritten by a locomotion task' (not 'wrong clip'), the one bounded retry is spent, and the not-interruptable variant is reached and seen",
              s.diag.unseenOverwritten >= 2 && s.diag.retried >= 1 && s.diag.unseenPlain == 0 && s.ladder.Current() == SwingVariant::UpperHard &&
                  s.diag.variantShown[(size_t)SwingVariant::UpperHard] >= 5 && s.Monotone(),
              TF("overwritten %u plain %u retried %u variant %d shown by upper-hard %u", s.diag.unseenOverwritten, s.diag.unseenPlain, s.diag.retried,
                 (int)s.ladder.Current(), s.diag.variantShown[(size_t)SwingVariant::UpperHard]));
    }
    {
        SwingSim s;
        s.game.locoCancels = true;
        s.game.locoEveryMs = 100;
        s.Swing(0);
        Check("R3 T6 retry: when a locomotion task replaced the swing within 150 ms of its issue the swing is issued ONCE more (bounded: never a second retry)",
              s.diag.retried == 1 && s.diag.issued == 1);
        Check("R3 T6 retry rule: needs a read of 'not playing' (0), a young swing, a locomotion task since the issue, no spent retry, and never for the melee task",
              SwingRetryDue(false, false, 0, 100, 3, 2) && !SwingRetryDue(false, true, 0, 100, 3, 2) && !SwingRetryDue(false, false, 1, 100, 3, 2) &&
                  !SwingRetryDue(false, false, -1, 100, 3, 2) && !SwingRetryDue(false, false, 0, 200, 3, 2) && !SwingRetryDue(false, false, 0, 100, 2, 2) &&
                  !SwingRetryDue(true, false, 0, 100, 3, 2));
    }
    {
        SwingSim s;
        s.game.clipExists = false;
        s.game.secondaryHonoured = false;
        s.game.fullBodyWorks = false;               // only the melee task plays
        s.Swings(14);
        const bool reached = s.ladder.Current() == SwingVariant::MeleeTask && s.ladder.Pinned() && s.diag.variantShown[(size_t)SwingVariant::MeleeTask] >= 5;
        SwingSim p;
        p.ladder.SetMode(SwingMode::Play);
        p.game = s.game;
        p.Swings(20);
        SwingSim o;
        o.ladder.SetMode(SwingMode::Off);
        o.Swings(5);
        SwingSim m;
        m.ladder.SetMode(SwingMode::MeleeTask);
        m.Swings(3);
        Check("R3 T6 ladder: nothing but the melee task plays: auto reaches it and pins it; melee_anim=play never takes the melee rung (stays on the animations, 0 shown, exhausted counted); "
              "off issues nothing; melee_task goes straight to it",
              reached && p.ladder.Current() == SwingVariant::FullBody && p.diag.shown == 0 && p.ladder.Exhausted() > 0 && o.diag.issued == 0 && o.diag.notIssuedOff == 5 &&
                  m.ladder.Current() == SwingVariant::MeleeTask && m.diag.shown == 3,
              TF("auto at %d pinned %d, play at %d shown %u exhausted %u, off issued %u, melee_task at %d shown %u", (int)s.ladder.Current(), (int)s.ladder.Pinned(),
                 (int)p.ladder.Current(), p.diag.shown, p.ladder.Exhausted(), o.diag.issued, (int)m.ladder.Current(), m.diag.shown));
    }
    {
        SwingLadder a, p, off, tripped;
        p.SetMode(SwingMode::Play);
        off.SetMode(SwingMode::Off);
        tripped.TripMeleeTask();
        const bool moved = a.OnNoClips();
        Check("R3 T6 ladder: when no candidate clip dictionary exists nothing can be issued and nothing could climb, so auto goes straight to the melee task; play, off and a tripped breaker stay put",
              moved && a.Current() == SwingVariant::MeleeTask && !a.OnNoClips() && !p.OnNoClips() && p.Current() == SwingVariant::Upper && !off.OnNoClips() &&
                  !tripped.OnNoClips() && tripped.Current() == SwingVariant::Upper);
    }
    {
        SwingSim s;
        s.game.secondaryHonoured = false;
        s.game.fullBodyWorks = false;
        s.Swings(8);                                // on the melee task now
        const bool onMelee = s.ladder.Current() == SwingVariant::MeleeTask;
        s.ladder.TripMeleeTask();                   // our health dropped while a puppet was in melee
        const bool back = s.ladder.Current() == SwingVariant::Upper && s.ladder.MeleeTripped();
        s.game.meleeTaskWorks = true;
        s.Swings(30, 10000);
        Check("R3 T6 breaker: harm to the local player trips the melee-task rung: the ladder drops to the animations and never takes the melee rung again, however many swings go unseen",
              onMelee && back && s.ladder.Current() != SwingVariant::MeleeTask && s.ladder.MeleeTripped());
    }
    {
        SwingSim s;
        s.game.ragdoll = true;
        s.Swings(10);
        SwingLadder busy;
        for (int i = 0; i < 10; i++) busy.OnResult(SwingProbe::Result::NotShown, ClassifySwingMiss(true, false, 0, 0) == SwingMiss::Busy);
        Check("R3 T6 ladder: a ragdolling puppet plays no clip and that is not the variant's fault: nothing is issued (counted as ragdoll), and ten busy misses climb nothing",
              s.diag.issued == 0 && s.diag.notIssuedRagdoll == 10 && busy.Current() == SwingVariant::Upper && busy.Escalations() == 0 &&
                  ClassifySwingMiss(true, true, 5, 0) == SwingMiss::Busy && ClassifySwingMiss(false, false, 5, 0) == SwingMiss::Overwritten &&
                  ClassifySwingMiss(false, false, 0, 0) == SwingMiss::Plain,
              TF("ragdoll skips %u, issued %u, variant %d", s.diag.notIssuedRagdoll, s.diag.issued, (int)busy.Current()));
    }
    {
        SwingSim s;
        s.game.readback = -1;
        s.Swings(10);
        Check("R3 T6 ladder: a read-back that cannot be called is counted as unreadable and is no evidence: ten swings never climb the ladder",
              s.diag.unreadable == 10 && s.diag.shown == 0 && s.diag.unseen == 0 && s.ladder.Current() == SwingVariant::Upper,
              TF("unreadable %u unseen %u variant %d", s.diag.unreadable, s.diag.unseen, (int)s.ladder.Current()));
    }
    {
        // the live log's case: tasks issued, nothing on screen. The old counter said "shown N"; the effect counter says 0.
        SwingSim s;
        s.game.clipExists = false;
        s.game.secondaryHonoured = false;
        s.game.fullBodyWorks = false;
        s.game.meleeTaskWorks = false;
        s.Swings(16);
        Check("R3 T6 counters: the live case (swing tasks issued, no animation on screen): issued 16, SEEN PLAYING 0, every miss classified (plain), the ladder climbed to its last rung, found nothing there and stays (exhausted)",
              s.diag.issued == 16 && s.diag.shown == 0 && s.diag.unseen == 16 && s.diag.unseenPlain == 16 && s.ladder.Exhausted() >= 1 &&
                  s.ladder.Current() == SwingVariant::MeleeTask,
              TF("issued %u shown %u unseen %u plain %u exhausted %u", s.diag.issued, s.diag.shown, s.diag.unseen, s.diag.unseenPlain, s.ladder.Exhausted()));
    }
    {
        SwingSim s;
        s.game.secondaryHonoured = false;
        s.Swings(6);
        const std::string line = SwingDiagLine(s.diag, SwingDiag(), s.ladder);
        Check("R3 T6 diag: the 10 s swing line names the effect (SEEN PLAYING), the reasons, the variant counts, the mode and the rung in use",
              line.find("SEEN PLAYING") != std::string::npos && line.find("melee_anim=auto") != std::string::npos && line.find("escalations 2") != std::string::npos &&
                  line.find("full body (8)") != std::string::npos && line.find("locomotion task was issued meanwhile") != std::string::npos,
              line);
    }
}
#endif  // R3T6_CLIENT

}  // namespace r3t6
