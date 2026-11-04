// r3_t4_tests.h: Desync Overhaul round 3, T4 (vehicle damage state). Included by tests/t3_vehvisual_test.cpp (one line) and run from its
// main (one call): it uses that file's helpers (Check, F, Kin, Drive, At, StoreRig, VM, FakeCar, VCmd, PlaceAt, Rig, Probe).
//
//   A. REPRODUCTION of the two live reads of GI's 0.4.7 log ("199 updates sent ... 199 with a smashed window, 96 with brake lights, 96 with
//      the handbrake"): the OLD code path (VisualSync + ReadReport + ClassifyBrake) driven with the inputs that produce those counts, next to
//      the NEW path (the sanity layer) on the same inputs.
//   B. The sanity layer: SlotGuard / VisualGuard baselines, PedalGuard, HandbrakeLimiter.
//   C. The v7 wire: byte accounting, refusals, truncation, bit flips.
//   D. The server: VehicleStore merge (monotonic, clean-car refusal), BuildFrame, FullBytes.
//   E. DamageReporter (the controller), DamageApplier (the copy; closed loop, bounded when a native does nothing or is missing).
//   F. End to end on a SIMULATED clock: controller sim -> encode -> loss -> VehicleStore (the server's own code) -> BuildFrame -> encode -> decode
//      -> copy sim, and the same chain over the REAL server on loopback for the wire (short).
//
// Evidence level: everything here is SIMULATED (fake cars that obey the model written in the test). What it cannot prove is how the GAME's
// natives behave (see docs/api/protocol.md "R3 T4").
#pragma once

#include "sync/vehicle_damage.h"

#include <cmath>
#include <functional>
#include <random>

static void Check(const std::string& name, bool ok, const std::string& got = "") { Check(name.c_str(), ok, got); }   // a formatted name (F(...))

constexpr int kR3T4Checks = 51;   // the number of Check() calls RunR3T4Tests makes (set from the first run; a skipped section fails main's count)

// ---------------------------------------------------------------------------------------------------------------------------
// fakes
// ---------------------------------------------------------------------------------------------------------------------------

// A car whose window / tyre natives answer like the live one: slots the model has no glass for read "not intact" (the round-2 flag in the
// code comment: "IS_VEHICLE_WINDOW_INTACT is true for a window the model does not have" is the OPPOSITE of what the live count shows).
struct JunkGlassCar : FakeCar
{
    uint8_t junk = 0xC0;        // window slots that always read "not intact"
    uint8_t junkTyres = 0;      // tyre slots that always read "burst"
    bool WindowIntact(int s, bool& intact)
    {
        const bool ok = FakeCar::WindowIntact(s, intact);
        if (ok && ((junk >> s) & 1)) intact = false;
        return ok;
    }
    bool TyreBurst(int s, bool completely, bool& burst)
    {
        const bool ok = FakeCar::TyreBurst(s, completely, burst);
        if (ok && !completely && ((junkTyres >> s) & 1)) burst = true;
        return ok;
    }
};

// A car as the damage natives see it. `trueGain` = metres of dent per unit of SET_VEHICLE_DAMAGE damage at a probe.
struct DmgCar
{
    float body = 1000.0f, petrol = 1000.0f, engine = 1000.0f;
    bool doorBroken[6] = {}, doorValid[6] = { true, true, true, true, true, true }, lampBroken[2] = {};
    float dent[8] = {};
    bool junkDoors = false;                 // IS_VEHICLE_DOOR_DAMAGED reads true for every door
    bool junkDent = false;                  // every probe reads junkDentM even when the hull is clean
    float junkDentM = 0.05f;
    float petrolJunk = -1.0f;               // >= 0: GET_VEHICLE_PETROL_TANK_HEALTH always reads this
    bool readNatives = true, writeNatives = true, ineffective = false;
    float trueGain = 0.002f;
    float lampThreshold = 200.0f;           // a lamp replay breaks the lamp at or above this damage
    int replayCalls = 0, breakCalls = 0, reads = 0, petrolSets = 0, begins = 0, ends = 0, depthCalls = 0;
    bool BodyHealth(float& f) { if (!readNatives) return false; ++reads; f = body; return true; }
    bool PetrolHealth(float& f) { if (!readNatives) return false; ++reads; f = petrolJunk >= 0.0f ? petrolJunk : petrol; return true; }
    bool DoorValid(int d, bool& v) { if (!readNatives) return false; ++reads; v = doorValid[d]; return true; }
    bool DoorBroken(int d, bool& b) { if (!readNatives) return false; ++reads; b = junkDoors ? true : doorBroken[d]; return true; }
    bool LampBroken(int l, bool& b) { if (!readNatives) return false; ++reads; b = lampBroken[l]; return true; }
    bool Dent(int i, float& m) { if (!readNatives) return false; ++reads; ++depthCalls; m = junkDent ? junkDentM : dent[i]; return true; }
    bool BreakDoor(int d) { ++breakCalls; if (!writeNatives) return false; if (!ineffective) doorBroken[d] = true; return true; }
    bool SetPetrol(float f) { ++petrolSets; if (!writeNatives) return false; if (!ineffective) petrol = f; return true; }
    bool ReplayLamp(int l, float dmg)
    {
        ++replayCalls;
        if (!writeNatives) return false;
        if (!ineffective && dmg >= lampThreshold) lampBroken[l] = true;
        return true;
    }
    bool ReplayDent(int i, float dmg)
    {
        ++replayCalls;
        if (!writeNatives) return false;
        if (!ineffective)
        {
            dent[i] += trueGain * dmg;
            body -= dmg * 0.05f;                // SET_VEHICLE_DAMAGE lowers the body health too
        }
        return true;
    }
    void BeginReplay() { ++begins; }
    void EndReplay() { ++ends; }
    // A crash: dents at some probes and the health that goes with it.
    void Crash(int probe, float metres, float healthLost)
    {
        dent[probe] += metres;
        body -= healthLost;
        engine -= healthLost * 0.5f;
    }
};

static proto::VehicleDamage Dmg(int petrol = 250, int parts = 0, std::initializer_list<int> dents = {})
{
    proto::VehicleDamage d;
    d.petrol = (uint8_t)petrol;
    d.parts = (uint8_t)parts;
    int i = 0;
    for (int v : dents)
        if (i < 8) d.dent[i++] = (uint8_t)v;
    return d;
}
static proto::VehicleDamage DentAt(int probe, int steps)
{
    proto::VehicleDamage d;
    d.dent[probe] = (uint8_t)steps;
    return d;
}
static int DentSum(const DmgCar& c)
{
    int n = 0;
    for (int i = 0; i < 8; i++) n += (int)sync::DentToWire(c.dent[i]);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------------
// A. reproduction of the live counts
// ---------------------------------------------------------------------------------------------------------------------------

static void R3A_Reproduce()
{
    std::printf("-- R3 A. reproducing the live reads (199 of 199 smashed windows, 96 and 96)\n");
    // A1. the controller of a CLEAN car whose window natives read some slots as "not intact" from the very first sample: the old chain
    // (ReadReport -> the update's `windows`) reports a smashed window in EVERY update, and a copy smashes it on its own clean car.
    for (const uint8_t junk : { (uint8_t)0xC0, (uint8_t)0x30, (uint8_t)0xFF, (uint8_t)0x01 })
    {
        JunkGlassCar ctl;
        ctl.junk = junk;
        VisualSync vs;
        int oldWith = 0, newWith = 0;
        VisualGuard guard;
        VisualSync vs2;
        VisualMask lastOld, lastNew;
        for (int i = 0; i < 199; i++)
        {
            const VisualMask a = ReadReport(vs, ctl);
            if (a.windows) ++oldWith;
            lastOld = a;
            const VisualMask b = ReadReportGuarded(vs2, guard, ctl, VisualMask(), i > 1000);
            if (b.windows) ++newWith;
            lastNew = b;
        }
        // the copy (somebody else's clean car) is told "windows = the controller's report"
        JunkGlassCar clean;
        clean.junk = 0;
        VisualSync copyOld;
        StepVisual(copyOld, clean, lastOld, false, true, false, 0.0);
        JunkGlassCar clean2;
        clean2.junk = 0;
        VisualSync copyNew;
        StepVisual(copyNew, clean2, lastNew, false, true, false, 0.0);
        Check(F("A1 reproduced (junk window slots %02x): the old chain reports a smashed window in %d of 199 updates and smashes it on a clean copy (%d calls); the sanity layer reports %d and the copy smashes %d",
                junk, oldWith, clean.smashCalls, newWith, clean2.smashCalls),
              oldWith == 199 && clean.smashCalls > 0 && newWith == 0 && clean2.smashCalls == 0 && clean.m.windows == lastOld.windows && clean2.m.windows == 0);
    }
    // A1b. the same for tyres: a slot that always reads burst.
    {
        JunkGlassCar ctl;
        ctl.junk = 0;
        ctl.junkTyres = 0x0C;
        VisualSync vs, vs2;
        VisualGuard guard;
        int oldWith = 0, newWith = 0;
        for (int i = 0; i < 100; i++)
        {
            if (ReadReport(vs, ctl).tyres) ++oldWith;
            if (ReadReportGuarded(vs2, guard, ctl, VisualMask(), false).tyres) ++newWith;
        }
        Check(F("A1b tyres: a slot that always reads burst is reported by the old chain in %d of 100 updates, by the sanity layer in %d", oldWith, newWith),
              oldWith == 100 && newWith == 0);
    }
    // A2. "96 brake-light updates and 96 handbrake updates": ClassifyBrake sets the brake lights whenever the handbrake is held, so the two counts are
    // EQUAL whenever the foot brake never acted while the car rolled forward - a parked car with the handbrake held for 96 of 199 samples gives it.
    {
        int brakeBits = 0, handBits = 0;
        for (int i = 0; i < 199; i++)
        {
            const bool parked = i < 96;
            const BrakeState b = parked ? ClassifyBrake(true, 0.0f, 0.0f, 1.0f, 0.0f) : ClassifyBrake(true, 1.0f, 0.0f, 0.0f, 6.0f);
            brakeBits += b.brakeLights;
            handBits += b.handbrake;
        }
        int brakeBits2 = 0, handBits2 = 0;
        for (int i = 0; i < 199; i++)
        {
            const bool braking = i >= 96 && i < 126;
            const BrakeState b = i < 96 ? ClassifyBrake(true, 0.0f, 0.0f, 1.0f, 0.0f)
                                        : (braking ? ClassifyBrake(true, 0.0f, 0.8f, 0.0f, 12.0f) : ClassifyBrake(true, 1.0f, 0.0f, 0.0f, 6.0f));
            brakeBits2 += b.brakeLights;
            handBits2 += b.handbrake;
        }
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u01(0.0f, 1.0f), sp(-20.0f, 30.0f);
        bool superset = true;
        for (int i = 0; i < 20000; i++)
        {
            const BrakeState b = ClassifyBrake(true, u01(rng), u01(rng), u01(rng), sp(rng));
            if (b.handbrake && !b.brakeLights) superset = false;
        }
        Check(F("A2 reproduced: a car parked with the handbrake held for 96 of 199 samples and never foot-braked while rolling gives 96 brake-light and 96 handbrake updates (got %d / %d); "
                "30 forward foot-brake samples make it %d / %d; brake lights are a superset of the handbrake in 20000 random pedal states, so equal counts only say 'no forward foot brake'",
                brakeBits, handBits, brakeBits2, handBits2),
              brakeBits == 96 && handBits == 96 && brakeBits2 == 126 && handBits2 == 96 && superset);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// B. the sanity layer
// ---------------------------------------------------------------------------------------------------------------------------

static void R3B_Guards()
{
    std::printf("-- R3 B. the sanity layer\n");
    {
        // SlotGuard: set at the first read and nobody shows it = unreliable (flagged once, never trusted); clear at the first read = a real slot.
        SlotGuard g;
        const SlotGuard::Out first = g.Update(0xC4, 0x00, false);
        const SlotGuard::Out again = g.Update(0xC4, 0x00, false);
        Check("B1 SlotGuard: bits set at the very first read while the server shows none are unreliable (flagged exactly once, never in the trusted set, not after 50 more reads)",
              first.bits == 0 && first.newlyUnreliable == 0xC4 && again.bits == 0 && again.newlyUnreliable == 0 && g.Unreliable() == 0xC4 && g.FlaggedTotal() == 3);
        for (int i = 0; i < 50; i++) g.Update(0xC4, 0x00, false);
        Check("B1b ... and stay blocked for ever while they read the same", g.Trusted() == 0);
        // a real change on a slot that read intact: held on a second read, or at once when corroborated; a one-read flicker never counts
        SlotGuard h;
        h.Update(0x00, 0x00, false);
        const uint8_t r1 = h.Update(0x02, 0x00, false).bits;
        const uint8_t r2 = h.Update(0x02, 0x00, false).bits;
        h.Update(0x00, 0x00, false);
        const uint8_t flick = h.Update(0x08, 0x00, false).bits;
        const uint8_t flickEnd = h.Update(0x00, 0x00, false).bits;
        const uint8_t corr = h.Update(0x10, 0x00, true).bits;
        Check("B2 SlotGuard: a bit that CHANGES from an intact baseline is believed on its second read (not the first), a one-read flicker never, and at once with a corroborating signal",
              r1 == 0 && r2 == 0x02 && flick == 0 && flickEnd == 0 && corr == 0x10);
        // a bit that is set at the first read AND shown by the server (a hand-over of a really damaged car) is trusted
        SlotGuard k;
        const uint8_t t = k.Update(0x05, 0x01, false).bits;
        Check("B3 SlotGuard: at the first read a bit the server also shows is a real fact and is trusted at once; the one it does not show is unreliable", t == 0x01 && k.Unreliable() == 0x04);
        // unreliable slot: believed only while the server shows it; reads clear once = a normal slot afterwards
        SlotGuard m;
        m.Update(0x40, 0x00, false);
        const uint8_t withSrv = m.Update(0x40, 0x40, false).bits;
        const uint8_t srvGone = m.Update(0x40, 0x00, false).bits;
        m.Update(0x00, 0x00, false);                              // it reads clear once: a real slot
        m.Update(0x40, 0x00, false);
        const uint8_t real = m.Update(0x40, 0x00, false).bits;
        Check("B4 SlotGuard: an unreliable slot is believed only while the server itself shows the bit; once it has read clear it is an ordinary slot (a real change counts after 2 reads)",
              withSrv == 0x40 && srvGone == 0 && real == 0x40);
    }
    {
        // VisualGuard on the COPY path: a copy whose natives read junk slots must not fix, smash or REPORT them; a real pop is still reported once.
        JunkGlassCar car;
        car.junk = 0xC0;
        VisualSync vs;
        VisualGuard guard;
        int reports = 0, cmds = 0;
        for (int i = 0; i < 40; i++)
        {
            const VisualOut o = StepVisualGuarded(vs, guard, car, VisualMask(), false, true, false, 1000.0 * i, false);
            if (o.report.Any()) ++reports;
            if (o.cmd.Any()) ++cmds;
        }
        const bool quiet = reports == 0 && cmds == 0 && car.fixWinCalls == 0 && car.smashCalls == 0;
        car.m.tyres = 0x04;                                       // the local player shoots a tyre of this copy
        VisualOut popped;
        for (int i = 40; i < 50; i++)
        {
            const VisualOut o = StepVisualGuarded(vs, guard, car, VisualMask(), false, true, true, 1000.0 * i, true);
            if (o.report.Any()) popped = o;
        }
        // the same car through the OLD path: junk windows are fixed (calls on slots with no glass) and reported as vehicle Hits
        JunkGlassCar car2;
        car2.junk = 0xC0;
        VisualSync vs2;
        int reportsOld = 0;
        for (int i = 0; i < 40; i++)
        {
            const VisualOut o = StepVisual(vs2, car2, VisualMask(), false, true, false, 1000.0 * i);
            if (o.report.Any()) ++reportsOld;
        }
        Check(F("B5 copy path: junk window slots on a copy cause no fix / smash call and no report in 40 checks (old path: %d vehicle Hits reported, %d fix calls); a real pop right after is still reported (tyre bit %02x)",
                reportsOld, car2.fixWinCalls, popped.report.tyres),
              quiet && popped.report.tyres == 0x04 && reportsOld > 0 && car2.fixWinCalls > 0);
    }
    {
        // PedalGuard: a handbrake that is held at the first sample of a rolling car, or stays held while the car does not slow down, is stuck.
        PedalGuard pg;
        int sent = 0;
        for (int i = 0; i < 100; i++)
            if (pg.Classify(true, 0.0f, 0.0f, 1.0f, 20.0f, 33.0 * i).handbrake) ++sent;
        const bool baselineSuspect = sent == 0 && pg.Stats().stuckRuns == 1 && pg.Stats().handDropped == 100;
        PedalGuard pg2;
        int sent2 = 0, lights2 = 0;
        for (int i = 0; i < 300; i++)
        {
            // starts released, then holds: the speed does not fall -> stuck after 2 s, and the brake lights stay off (no foot brake)
            const float hand = i < 10 ? 0.0f : 1.0f;
            const BrakeState b = pg2.Classify(true, 0.5f, 0.0f, hand, 20.0f, 33.0 * i);
            sent2 += b.handbrake;
            lights2 += b.brakeLights;
        }
        PedalGuard pg3;
        int sent3 = 0;
        float v = 20.0f;
        for (int i = 0; i < 90; i++)
        {
            v = std::max(0.0f, v - 0.25f);                        // a real handbrake slows the car down
            if (pg3.Classify(true, 0.0f, 0.0f, i == 0 ? 0.0f : 1.0f, v, 33.0 * i).handbrake) ++sent3;
        }
        PedalGuard pg4;
        int sent4 = 0;
        for (int i = 0; i < 300; i++)
            if (pg4.Classify(true, 0.0f, 0.0f, 1.0f, 0.0f, 100.0 * i).handbrake) ++sent4;
        Check(F("B6 PedalGuard: held at the first sample of a car rolling at 20 m/s -> withheld (0 of 100 sent); held 2 s without the car slowing -> withheld after 2 s (%d of 290 sent, brake lights %d); "
                "a real handbrake that slows the car passes (%d of 89); a parked car with the handbrake held for 30 s passes (%d of 300)", sent2, lights2, sent3, sent4),
              baselineSuspect && sent2 > 55 && sent2 < 70 && lights2 == sent2 && sent3 == 89 && sent4 == 300);
        PedalGuard pg5;
        for (int i = 0; i < 100; i++) pg5.Classify(true, 0.0f, 0.0f, 1.0f, 20.0f, 33.0 * i);
        const bool dropped = !pg5.Classify(true, 0.0f, 0.0f, 1.0f, 20.0f, 4000.0).handbrake;
        pg5.Classify(true, 0.0f, 0.0f, 0.0f, 20.0f, 4033.0);      // released
        const bool again = pg5.Classify(true, 0.0f, 0.0f, 1.0f, 12.0f, 4066.0).handbrake;
        const BrakeState foot = pg5.Classify(true, 0.0f, 0.9f, 0.0f, 12.0f, 4100.0);
        Check("B7 PedalGuard: after a release the next handbrake is believed again; the foot brake still lights the brake lights while the handbrake is withheld; unreadable input = both clear",
              dropped && again && foot.brakeLights && !foot.handbrake && !pg5.Classify(false, 0, 0, 1, 20, 4200).handbrake);
        PedalGuard pg6;
        pg6.Classify(true, 0.0f, 0.8f, 1.0f, 20.0f, 0.0);
        const BrakeState both = pg6.Classify(true, 0.0f, 0.8f, 1.0f, 20.0f, 33.0);
        Check("B8 PedalGuard: a withheld handbrake keeps the brake lights the FOOT brake earns (brake lights on, handbrake off)", both.brakeLights && !both.handbrake);
        // a foot-brake axis stuck high: the car keeps its speed for 3 s under a "held" brake -> no more brake lights from it; a real braking car passes
        PedalGuard pg7;
        int lightsStuck = 0;
        for (int i = 0; i < 300; i++)
            if (pg7.Classify(true, 0.0f, 0.9f, 0.0f, 25.0f, 33.0 * i).brakeLights) ++lightsStuck;
        PedalGuard pg8;
        int lightsReal = 0;
        float sp = 25.0f;
        for (int i = 0; i < 300; i++)
        {
            sp = std::max(0.0f, sp - 0.06f);                      // a braking car slows down (22% in 3 s)
            if (pg8.Classify(true, 0.0f, 0.9f, 0.0f, sp, 33.0 * i).brakeLights) ++lightsReal;
        }
        Check(F("B8b PedalGuard: a foot brake read as held at 25 m/s while the car does not slow down gives brake lights for 3 s (%d of 300 samples) and then none; a car that really slows down keeps them (%d of 300)", lightsStuck, lightsReal),
              lightsStuck > 85 && lightsStuck < 100 && lightsReal > 280);
    }
    {
        // HandbrakeLimiter (the copy): held on a standing car, released after 4 s of rolling, until the controller clears the bit.
        HandbrakeLimiter hl;
        bool stand = true;
        for (int i = 0; i < 100; i++) stand = stand && hl.Effective(true, 0.0f, 500.0 * i);
        bool rolling = true;
        HandbrakeLimiter h2;
        int onFrames = 0;
        for (int i = 0; i < 300; i++)
            if (h2.Effective(true, 15.0f, 33.0 * i)) ++onFrames;
        const bool released = !h2.Effective(true, 15.0f, 10000.0) && h2.Suppressed() && h2.Releases() == 1;
        h2.Effective(false, 15.0f, 10100.0);
        const bool back = h2.Effective(true, 15.0f, 10200.0);
        (void)rolling;
        Check(F("B9 HandbrakeLimiter: a standing car keeps a held handbrake for ever; a rolling car keeps it for about 4 s (%d of 300 frames at 33 ms), then it is released (1 release) until the bit is clear once; "
                "the next claim is honoured", onFrames),
              stand && onFrames >= 120 && onFrames <= 125 && released && back);
    }
    {
        Check("B10 ShouldAdoptServerHealth: the controller adopts a server health that is more than 25 lower, ignores 1-point hits, a higher value and NaN",
              ShouldAdoptServerHealth(700.0f, 1000.0f) && !ShouldAdoptServerHealth(999.0f, 1000.0f) && !ShouldAdoptServerHealth(1000.0f, 800.0f) &&
                  !ShouldAdoptServerHealth(NAN, 800.0f) && !ShouldAdoptServerHealth(500.0f, NAN));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// C. the wire
// ---------------------------------------------------------------------------------------------------------------------------

static void R3C_Wire()
{
    std::printf("-- R3 C. the v7 wire\n");
    proto::VehicleUpdate base = SampleUpdate();
    base.hasDmg = false;
    const proto::Bytes plain = proto::Encode(base);
    proto::VehicleUpdate withBlock = base;
    withBlock.hasDmg = true;
    withBlock.dmg = Dmg(100, 0x41, { 0x10, 0, 3 });
    const proto::Bytes blk = proto::Encode(withBlock);
    proto::VehicleUpdate clean = base;
    clean.hasDmg = true;
    const proto::Bytes cleanBlk = proto::Encode(clean);
    proto::VehicleUpdate maxBlock = base;
    maxBlock.hasDmg = true;
    maxBlock.dmg = Dmg(0, 0xFF, { 1, 2, 3, 4, 5, 6, 7, 255 });
    const proto::Bytes maxBytes = proto::Encode(maxBlock);
    proto::VehicleUpdate back, back2;
    const bool d1 = proto::Decode(plain.data(), plain.size(), back);
    const bool d2 = proto::Decode(blk.data(), blk.size(), back2);
    Check(F("C1 VehicleUpdate bytes: no block = %zu (v6: 40, unchanged: an undamaged car costs nothing); clean block 43; block with 2 dents %zu (=3+2 more); maximum %zu (=3+8 more)",
            plain.size(), blk.size(), maxBytes.size()),
          plain.size() == 40 && !blk.empty() && blk.size() == plain.size() + 3 + 2 && cleanBlk.size() == plain.size() + 3 && maxBytes.size() == plain.size() + 11 && d1 && !back.hasDmg &&
              d2 && back2.hasDmg && back2.dmg == withBlock.dmg && back2.flags == withBlock.flags && (blk[1 + 4 + 1 + 22] & proto::kVehDamageBlock));
    Check("C2 the flag byte: kVehDamageBlock is bit 5 of the update's `flags` on the wire only (the decoded struct keeps the kVeh* bits), a block announced but not present is refused, and flags bits 6 / 7 are still refused",
          [&] {
              proto::Bytes announced = plain;
              announced[1 + 4 + 1 + 22] |= proto::kVehDamageBlock;
              proto::VehicleUpdate o;
              proto::Bytes bit6 = plain;
              bit6[1 + 4 + 1 + 22] |= 0x40;
              return !proto::Decode(announced.data(), announced.size(), o) && !proto::Decode(bit6.data(), bit6.size(), o) && back2.flags == proto::kVehEngine;
          }());
    // refusals
    {
        proto::VehicleUpdate bad = withBlock;
        bad.dmg.petrol = 251;
        const bool encRefuses = proto::Encode(bad).empty() && !proto::ValidVehicleUpdate(bad);
        proto::Bytes hostile = blk;
        const size_t petrolAt = plain.size();                      // first byte of the block
        hostile[petrolAt] = 251;
        proto::VehicleUpdate o;
        const bool petrolRefused = !proto::Decode(hostile.data(), hostile.size(), o);
        proto::Bytes zeroDent = blk;
        zeroDent[petrolAt + 3] = 0;                                // the first dent byte
        const bool zeroRefused = !proto::Decode(zeroDent.data(), zeroDent.size(), o);
        size_t trunc = 0, trail = 0;
        for (size_t n = 0; n < blk.size(); n++)
            if (proto::Decode(blk.data(), n, o)) ++trunc;
        proto::Bytes plus = blk;
        plus.push_back(0);
        if (proto::Decode(plus.data(), plus.size(), o)) ++trail;
        Check("C3 refusals: petrol above 250 (encode, validate, decode), a dent byte of 0 under a set mask bit, every truncation of an update with a block, one trailing byte",
              encRefuses && petrolRefused && zeroRefused && trunc == 0 && trail == 0, F("trunc %zu trail %zu", trunc, trail));
        size_t accepted = 0, nonCanonical = 0;
        for (size_t byte = petrolAt; byte < blk.size(); byte++)
            for (int bit = 0; bit < 8; bit++)
            {
                proto::Bytes f = blk;
                f[byte] ^= (uint8_t)(1u << bit);
                proto::VehicleUpdate m;
                if (!proto::Decode(f.data(), f.size(), m)) continue;
                ++accepted;
                if (proto::Encode(m) != f) ++nonCanonical;
            }
        // petrol 8 flips + parts 8 + mask 0 + dent bytes (0x10: 7 of 8; 0x03: 8) = 31
        Check(F("C4 each of the 40 single-bit flips of the damage block bytes is refused or decodes to a message that re-encodes to exactly the flipped bytes (accepted %zu = 8 petrol + 8 parts + 0 mask + 7 + 8 dent bits)", accepted),
              accepted == 31 && nonCanonical == 0);
    }
    // VehicleWorld full entry
    {
        proto::VehicleWorld w;
        w.tick = 5;
        w.present = { 3 };
        proto::VehicleState v = SampleFull(3);
        v.dmg = Dmg(40, 0x82, { 0, 9, 0, 0, 200 });
        w.vehicles = { v };
        const proto::Bytes enc = proto::Encode(w);
        proto::VehicleWorld o;
        const bool dec = proto::Decode(enc.data(), enc.size(), o);
        proto::VehicleWorld wc = w;
        wc.vehicles[0].dmg = proto::VehicleDamage();
        const proto::Bytes encClean = proto::Encode(wc);
        proto::VehicleWorld wm = w;
        wm.vehicles[0].motionOnly = true;
        const proto::Bytes encMotion = proto::Encode(wm);
        proto::VehicleWorld wx = w;
        wx.vehicles[0].dmg = Dmg(0, 0xFF, { 1, 2, 3, 4, 5, 6, 7, 8 });
        const proto::Bytes encMax = proto::Encode(wx);
        Check(F("C5 VehicleWorld full entry: clean car %zu bytes (v6 %zu: +3), two dents +%zu, maximum +%zu; a motion-only entry carries no block (%zu, 27 less than a full entry with one occupant); round trip equal",
                encClean.size(), encClean.size() - 3, enc.size() - encClean.size(), encMax.size() - encClean.size(), encMotion.size()),
              dec && o.vehicles.size() == 1 && o.vehicles[0].dmg == v.dmg && enc.size() == encClean.size() + 2 && encMax.size() == encClean.size() + 8 &&
                  encMotion.size() + 27 == encClean.size());
        // truncation / trailing / flips of a frame with a damage block
        size_t trunc = 0;
        for (size_t n = 0; n < enc.size(); n++)
            if (proto::Decode(enc.data(), n, o)) ++trunc;
        proto::Bytes plus = enc;
        plus.push_back(1);
        size_t flipsBad = 0;
        const size_t blockAt = enc.size() - (size_t)proto::VehicleDamageBytes(v.dmg) - 1 /*seat mask*/ - 2 /*one occupant id*/;
        for (size_t byte = blockAt; byte < blockAt + proto::VehicleDamageBytes(v.dmg); byte++)
            for (int bit = 0; bit < 8; bit++)
            {
                proto::Bytes f = enc;
                f[byte] ^= (uint8_t)(1u << bit);
                proto::VehicleWorld m;
                if (proto::Decode(f.data(), f.size(), m) && proto::Encode(m) != f) ++flipsBad;
            }
        Check("C6 VehicleWorld: every truncation and a trailing byte are refused; no flip of the block decodes to a non-canonical message; petrol 251 / a zero dent byte are refused by Encode and Decode",
              trunc == 0 && !proto::Decode(plus.data(), plus.size(), o) && flipsBad == 0 && [&] {
                  proto::VehicleWorld b = w;
                  b.vehicles[0].dmg.petrol = 251;
                  proto::Bytes hostile = enc;
                  hostile[blockAt] = 251;
                  proto::Bytes zero = enc;
                  zero[blockAt + 3] = 0;
                  proto::VehicleWorld x;
                  return proto::Encode(b).empty() && !proto::Decode(hostile.data(), hostile.size(), x) && !proto::Decode(zero.data(), zero.size(), x);
              }());
    }
    Check("C7 quantisers: petrol health maps to 0..250 (4 per step, NaN reads full), a dent to 2 mm steps up to 255 (0.51 m), negatives and NaN to 0, and they invert within a step",
          PetrolToWire(1000.0f) == 250 && PetrolToWire(0.0f) == 0 && PetrolToWire(-5.0f) == 0 && PetrolToWire(NAN) == 250 && PetrolToWire(650.0f) == 163 &&
              std::fabs(PetrolFromWire(PetrolToWire(650.0f)) - 650.0f) <= 2.0f && DentToWire(0.01f) == 5 && DentToWire(9.0f) == 255 && DentToWire(-1.0f) == 0 &&
              DentToWire(NAN) == 0 && std::fabs(DentFromWire(DentToWire(0.123f)) - 0.123f) <= 0.0011f);
    // the byte table
    std::printf("      bytes, v6 -> v7: VehicleUpdate 40 -> 40 (no block) / 43 (clean block) / 45 (2 dents) / 51 (max); VehicleWorld full entry 46 -> 49 (clean) / 51 (2 dents) / 57 (max), + 2 per occupant;\n"
                "      motion-only entry 24 -> 24. A parked clean car: 3 B per full entry (about one a second per receiver); a damaged car driven by its controller: one 3..11 B block per change (6 repeats), then a 3 s refresh only while the server lacks it.\n");
}

// ---------------------------------------------------------------------------------------------------------------------------
// D. the server's store
// ---------------------------------------------------------------------------------------------------------------------------

struct StoreRig3
{
    server::VehicleStore vs;
    uint16_t veh = 0;
    uint32_t tick = 1;
    explicit StoreRig3(bool needHealthLoss = true)
    {
        server::VehicleStore::Rules r = StoreRules();
        r.damageNeedsHealthLoss = needHealthLoss;
        vs.Configure(r, 8);
        for (uint16_t p = 0; p < 4; p++) vs.AddReceiver(p);
        vs.Create(1, At(0, 0), proto::QuatFromHeading(0.0f), 0, 0, proto::kNoPlayer, 0, true, 0, tick++, &veh);
        vs.Enter(0, veh, 0, At(0, 0), 500, tick++);
    }
    bool Frame(uint16_t receiver, uint64_t nowMs, proto::VehicleWorld& w) { return vs.BuildFrame(receiver, true, At(10, 0), tick++, nowMs, w); }
};

static proto::VehicleUpdate DriveD(uint16_t veh, float y, uint16_t body, uint8_t windows = 0)
{
    proto::VehicleUpdate u = Drive(veh, 0, y, 10, 0, 0, windows);
    u.bodyHealth = body;
    return u;
}
static proto::VehicleUpdate DriveB(uint16_t veh, float y, uint16_t body, const proto::VehicleDamage& d, uint8_t windows = 0)
{
    proto::VehicleUpdate u = DriveD(veh, y, body, windows);
    u.hasDmg = true;
    u.dmg = d;
    return u;
}

static void R3D_Server()
{
    std::printf("-- R3 D. the server's store\n");
    {
        // monotonic merge on a damaged car; the report never clears; a repair path clears it
        StoreRig3 R;
        R.vs.Update(0, DriveD(R.veh, 1, 800), 1000);
        R.vs.Update(0, DriveB(R.veh, 2, 800, Dmg(100, 0x01, { 30 })), 1033);
        const server::VehicleStore::Vehicle* V = R.vs.Get(R.veh);
        const bool first = V->s.dmg == Dmg(100, 0x01, { 30 });
        R.vs.Update(0, DriveB(R.veh, 3, 800, Dmg(200, 0x40, { 10, 0, 50 })), 1066);
        const bool merged = V->s.dmg == Dmg(100, 0x41, { 30, 0, 50 });
        R.vs.Update(0, DriveB(R.veh, 4, 800, proto::VehicleDamage()), 1100);   // a clean report does not clear
        const bool kept = V->s.dmg == Dmg(100, 0x41, { 30, 0, 50 });
        R.vs.RepairDamage(R.veh, R.tick++);
        Check("D1 store: the controller's block is merged monotonically (petrol the lower, parts OR-ed, dents the deeper) and a clean or milder report clears nothing; RepairDamage resets it",
              first && merged && kept && V->s.dmg == proto::VehicleDamage());
    }
    {
        // A clean car (body health ~full): a report that ADDS a smashed window / door / dent / lower petrol is refused and counted; after the health fell it is taken.
        StoreRig3 R;
        R.vs.Update(0, DriveD(R.veh, 1, 1000, 0x40), 1000);
        const bool winRefused = R.vs.Get(R.veh)->s.windows == 0 && R.vs.windowRejects() == 1;
        R.vs.Update(0, DriveB(R.veh, 2, 999, Dmg(250, 0x02, { 0, 0, 20 })), 1033);
        const bool dmgRefused = R.vs.Get(R.veh)->s.dmg == proto::VehicleDamage() && R.vs.damageRejects() == 1;
        R.vs.Update(0, DriveB(R.veh, 3, 700, Dmg(250, 0x02, { 0, 0, 20 }), 0x40), 1066);
        const bool taken = R.vs.Get(R.veh)->s.dmg == Dmg(250, 0x02, { 0, 0, 20 }) && R.vs.Get(R.veh)->s.windows == 0x40;
        Check("D2 store: with a full body health a report that adds a smashed window, a broken door or a dent is refused and counted (window 1, damage 1); once the body health is below 995 the same report is taken",
              winRefused && dmgRefused && taken);
        // a Hit's window bit is not 'rising' (HitPart set it): the controller echoing it on a car with health 999 is fine
        StoreRig3 H;
        H.vs.Update(0, DriveD(H.veh, 1, 1000), 1000);
        H.vs.HitPart(H.veh, proto::VehiclePart::Window, 2, 1100, H.tick++);
        H.vs.Update(0, DriveD(H.veh, 2, 1000, 0x04), 1133);
        Check("D3 store: a window bit set by a validated Hit still stands on a car with full health (it is acked by the controller, never 'rising')", H.vs.Get(H.veh)->s.windows == 0x04 && H.vs.windowRejects() == 0);
        // the switch: damageNeedsHealthLoss=false = the v6 trust
        StoreRig3 O(false);
        O.vs.Update(0, DriveD(O.veh, 1, 1000, 0x40), 1000);
        Check("D4 store: damageNeedsHealthLoss=false trusts the report as in v6 (a window on a clean car is taken)", O.vs.Get(O.veh)->s.windows == 0x40);
    }
    {
        // BuildFrame: a change is told in full for 3 frames, motion-only entries carry no block; FullBytes = the encoded size.
        StoreRig3 R;
        R.vs.Update(0, DriveD(R.veh, 1, 800), 1000);
        proto::VehicleWorld w;
        for (int i = 0; i < 6; i++) { R.Frame(1, 1030 + i * 50, w); R.vs.Update(0, DriveD(R.veh, 1.0f + i, 800), 1050 + i * 50); }
        R.vs.Update(0, DriveB(R.veh, 8, 700, Dmg(120, 0x04, { 5, 6 })), 1400);
        int fulls = 0, motion = 0, fullsWithBlock = 0;
        size_t sizeOk = 0, entryBytes = 0;
        for (int i = 0; i < 8; i++)
        {
            R.Frame(1, 1430 + i * 50, w);
            R.vs.Update(0, DriveB(R.veh, 9.0f + i, 700, Dmg(120, 0x04, { 5, 6 })), 1450 + i * 50);
            if (w.vehicles.empty()) continue;
            const proto::VehicleState& e = w.vehicles[0];
            if (e.motionOnly) { ++motion; continue; }
            ++fulls;
            if (e.dmg == Dmg(120, 0x04, { 5, 6 })) ++fullsWithBlock;
            proto::VehicleWorld one;
            one.present = w.present;
            one.vehicles = w.vehicles;
            entryBytes = proto::Encode(one).size() - (1 + 4 + 1 + 2) - 2;
            if (server::VehicleStore::FullBytes(e) == entryBytes) ++sizeOk;
        }
        Check(F("D5 store: a damage change is told in 3-4 full entries that all carry the block, then motion-only entries; FullBytes equals the encoded size (%zu = 46 + 3 + 2 dents + 2 for the driver) in every full entry", entryBytes),
              fulls >= 3 && fulls <= 4 && fullsWithBlock == fulls && motion >= 4 && sizeOk == (size_t)fulls);
    }
    {
        // A car with no controller keeps its block; stream-in carries it.
        StoreRig3 R;
        R.vs.Update(0, DriveB(R.veh, 1, 700, Dmg(100, 0x01, { 0, 0, 0, 0, 0, 9 })), 1000);
        R.vs.Exit(0, R.veh, 1100, R.tick++);
        std::vector<server::VehicleStore::ControlCandidate> none;
        R.vs.AssignControllers(60000, R.tick++, none);
        proto::VehicleWorld w;
        R.Frame(3, 60100, w);
        Check("D6 store: a car nobody controls keeps its damage block and a receiver that streams it in gets it in the first full entry",
              !w.vehicles.empty() && !w.vehicles[0].motionOnly && w.vehicles[0].dmg == Dmg(100, 0x01, { 0, 0, 0, 0, 0, 9 }));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// E. reporter and applier
// ---------------------------------------------------------------------------------------------------------------------------

static proto::VehicleDamage RunReporter(DamageReporter& rep, DmgCar& car, const proto::VehicleDamage& server, double t)
{
    return StepDamageController(rep, car, server, t);
}

static void R3E_ReporterApplier()
{
    std::printf("-- R3 E. the controller's reporter and the copy's applier\n");
    {
        // clean car: nothing reported; a real crash (dents + health) is reported after it happened; dents never shrink
        DamageReporter rep;
        DmgCar car;
        proto::VehicleDamage out;
        for (int i = 0; i < 20; i++) out = RunReporter(rep, car, proto::VehicleDamage(), 500.0 * i);
        const bool cleanQuiet = out == proto::VehicleDamage();
        car.Crash(2, 0.12f, 220.0f);                              // front middle, 12 cm, body 1000 -> 780
        car.petrol = 700.0f;
        car.lampBroken[0] = true;
        car.doorBroken[4] = true;
        proto::VehicleDamage after = RunReporter(rep, car, out, 10500.0);
        const bool crash = after.dent[2] == sync::DentToWire(0.12f) && after.parts == (proto::kVehDmgLampL | 0x10) && after.petrol == PetrolToWire(700.0f);
        car.dent[2] = 0.0f;                                        // read noise / a repair: the report does not shrink
        const proto::VehicleDamage later = RunReporter(rep, car, after, 11000.0);
        Check("E1 reporter: a clean car reports nothing in 20 reads; a crash (a 12 cm dent at probe 2, body health -220, a broken lamp, door and a leaking tank) is reported at once (corroborated by the health fall); a later clean read does not shrink it",
              cleanQuiet && crash && later.dent[2] == after.dent[2]);
    }
    {
        // junk at the first read on a clean car: never reported
        DamageReporter rep;
        DmgCar car;
        car.junkDent = true;
        car.junkDoors = true;
        car.petrolJunk = 300.0f;
        proto::VehicleDamage out;
        DamageReporter::Flagged fl, firstFlag;
        for (int i = 0; i < 40; i++)
        {
            out = StepDamageController(rep, car, proto::VehicleDamage(), 500.0 * i, &fl);
            if (i == 0) firstFlag = fl;
        }
        Check(F("E2 reporter: dent probes that read 5 cm everywhere, doors that all read 'damaged' and a tank stuck at 300 from the first read are flagged unreliable (flagged once: parts %02x dents %02x petrol %d) and NEVER reported in 40 reads",
                firstFlag.parts, firstFlag.dents, (int)firstFlag.petrol),
              out == proto::VehicleDamage() && firstFlag.parts == 0x3F && firstFlag.dents == 0xFF && firstFlag.petrol && rep.UnreliableDents() == 0xFF);
        // ... and the same junk through the OLD behaviour would be a block of dents on a clean car (what a v7 client without the layer would send)
        proto::VehicleDamage naive;
        for (int i = 0; i < 8; i++) naive.dent[i] = DentToWire(0.05f);
        Check("E2b ... the junk is what an unguarded reader would send: a dent at all 8 probes (25 steps each) on a clean car", naive.DentMask() == 0xFF);
    }
    {
        // a dent rise with no health loss anywhere is not a crash; with the health below the first-sight value for 2 reads it is
        DamageReporter rep;
        DmgCar car;
        proto::VehicleDamage out;
        for (int i = 0; i < 4; i++) out = RunReporter(rep, car, proto::VehicleDamage(), 500.0 * i);
        car.dent[5] = 0.05f;                                       // appears with the health untouched
        for (int i = 4; i < 12; i++) out = RunReporter(rep, car, out, 500.0 * i);
        const bool held = out.dent[5] == 0;
        car.body = 960.0f;                                         // now the health went down (slowly, no 'fall' within the window at read time)
        proto::VehicleDamage o2;
        double t = 6000.0;
        for (int i = 0; i < 6; i++) o2 = RunReporter(rep, car, out, t += 500.0);
        Check("E3 reporter: a new dent while the body health never moved is not reported, in 8 reads; once the body health fell the dent is believed",
              held && o2.dent[5] == DentToWire(0.05f));
    }
    {
        // a copy brought to the server's block; then it becomes the controller: no echo of what it applied
        DmgCar copy;
        DamageApplier ap;
        const proto::VehicleDamage srv = Dmg(150, 0x43, { 0, 0, 40, 0, 0, 0, 25, 0 });
        double t = 1000.0;
        int steps = 0;
        for (; steps < 60; steps++)
        {
            StepDamageCopy(ap, copy, srv, true, t);
            t += 100.0;
        }
        const bool doors = copy.doorBroken[0] && copy.doorBroken[1] && !copy.doorBroken[2];
        const bool lamp = copy.lampBroken[0] && !copy.lampBroken[1];
        const bool petrol = std::fabs(copy.petrol - PetrolFromWire(150)) < 1.0f;
        const bool dents = std::abs((int)DentToWire(copy.dent[2]) - 40) <= kDentTolSteps + 1 && std::abs((int)DentToWire(copy.dent[6]) - 25) <= kDentTolSteps + 1 &&
                           DentToWire(copy.dent[0]) == 0;
        DamageReporter rep;
        proto::VehicleDamage out;
        for (int i = 0; i < 12; i++) out = RunReporter(rep, copy, srv, t += 500.0);
        Check(F("E4 applier: a copy is brought to the server's block (doors 0 and 1 broken, the left lamp by a replay, the tank to 600, dents at probes 2 and 6 within 3 steps: %d / %d), then it becomes the controller and reports no change (no echo)",
                (int)DentToWire(copy.dent[2]), (int)DentToWire(copy.dent[6])),
              doors && lamp && petrol && dents && out == srv && ap.Stats().dentsReplayed <= 8);
        // a clean server block costs nothing
        DmgCar clean;
        DamageApplier ap2;
        for (int i = 0; i < 100; i++) StepDamageCopy(ap2, clean, proto::VehicleDamage(), true, 100.0 * i);
        Check("E5 applier: a car whose server block is clean costs no native call at all in 100 checks", clean.reads == 0 && clean.replayCalls == 0 && clean.breakCalls == 0 && clean.petrolSets == 0);
        // the replay is bracketed (a copy that cannot be damaged by the game must be damageable for the length of the replay)
        Check("E6 applier: every replay is bracketed by BeginReplay / EndReplay (the self-damage guard is lifted only for the replay)", copy.begins == copy.ends && copy.begins >= 1 && copy.begins <= 12);
    }
    {
        // closed loop: the gain of the game's SET_VEHICLE_DAMAGE is unknown; the applier learns it from the read-back. Converges from below for
        // gains up to ~2.5x the first guess without overshoot, and at most 4 rounds per probe.
        std::string detail;
        bool allOk = true;
        for (const float ratio : { 0.25f, 0.5f, 1.0f, 2.0f })
        {
            DmgCar copy;
            copy.trueGain = 0.002f * ratio;                        // the guess is 1/500 per damage unit
            DamageApplier ap;
            const proto::VehicleDamage srv = DentAt(2, 60);
            double t = 0.0;
            for (int i = 0; i < 200; i++) { StepDamageCopy(ap, copy, srv, true, t); t += 100.0; }
            const int got = DentSum(copy);
            const bool ok = got >= 60 - kDentTolSteps && got <= 60 + 6 && copy.replayCalls <= 4;
            allOk = allOk && ok;
            detail += F(" ratio %.2f -> %d steps in %d replays;", (double)ratio, got, copy.replayCalls);
        }
        DmgCar over;
        over.trueGain = 0.002f * 6.0f;                             // a game 6x stronger than the guess overshoots ONCE (nothing can undo a dent), then stops
        DamageApplier ap;
        double t = 0.0;
        for (int i = 0; i < 200; i++) { StepDamageCopy(ap, over, DentAt(2, 60), true, t); t += 100.0; }
        Check("E7 applier closed loop: for a game 0.25x, 0.5x, 1x and 2x as strong as the first guess the dent reaches 60 steps (within 3 below / 6 above) in at most 4 replays;" + detail +
                  F(" a 6x game overshoots once to %d steps (documented limit) and then stops", DentSum(over)),
              allOk && over.replayCalls <= 4 && DentSum(over) > 60);
    }
    {
        // a native that does nothing: bounded; a native that is missing: bounded and inert; reads missing: one blind replay per server value
        DmgCar dead;
        dead.ineffective = true;
        DamageApplier ap;
        double t = 0.0;
        for (int i = 0; i < 1200; i++) { StepDamageCopy(ap, dead, DentAt(3, 50), true, t); t += 100.0; }       // 120 s
        const bool stuck = dead.replayCalls <= 4 && ap.Stats().dentNoEffect >= 1 && ap.Stats().dentGaveUp == 1;
        DmgCar missing;
        missing.writeNatives = false;
        DamageApplier ap2;
        t = 0.0;
        for (int i = 0; i < 1000; i++) { StepDamageCopy(ap2, missing, Dmg(100, 0x41, { 0, 0, 50 }), true, t); t += 100.0; }   // 100 s
        const int calls = missing.replayCalls + missing.breakCalls + missing.petrolSets;
        DmgCar blind;
        blind.readNatives = false;
        DamageApplier ap3;
        t = 0.0;
        for (int i = 0; i < 300; i++) { StepDamageCopy(ap3, blind, DentAt(4, 40), true, t); t += 100.0; }
        const bool blindOnce = blind.replayCalls == 1;
        Check(F("E8 applier: a replay that does nothing is tried at most 4 times and then given up (%d calls in 120 s); unresolved write natives cost %d calls in 100 s (3 rounds, then 5 s of silence), not a stream; with unreadable depth the dent is replayed once (%d)",
                dead.replayCalls, calls, blind.replayCalls),
              stuck && calls > 0 && calls <= 60 && blindOnce);
    }
    {
        // lamps: no setter in the game: the replay escalates 120, 240, 480 until the read-back says broken; a model without the door is not an error
        DmgCar copy;
        copy.lampThreshold = 400.0f;
        copy.doorValid[5] = false;
        DamageApplier ap;
        double t = 0.0;
        for (int i = 0; i < 80; i++) { StepDamageCopy(ap, copy, Dmg(250, proto::kVehDmgLampR | 0x20, {}), true, t); t += 100.0; }
        Check(F("E9 applier: a lamp that needs 400 damage is broken by the third replay (120, 240, 480) and then left alone (%d replay calls); a door the model does not have is accepted without a break call (%d)",
                copy.replayCalls, copy.breakCalls),
              copy.lampBroken[1] && !copy.lampBroken[0] && copy.replayCalls == 3 && copy.breakCalls == 0);
    }
    {
        // the send policy: nothing for a car whose report equals the server's; a change rides 6 updates; a lost block is refreshed every 3 s
        DamageSendPolicy sp;
        const proto::VehicleDamage srv, rep = Dmg(250, 0x01, {});
        int carried = 0;
        bool quiet = true;
        for (int i = 0; i < 100; i++)
        {
            sp.Observe(srv, srv);
            if (sp.Due(srv, srv, 33.0 * i)) quiet = false;
        }
        sp.Observe(rep, srv);
        const bool fresh = sp.Fresh();
        for (int i = 0; i < 6; i++)
            if (sp.Due(rep, srv, 4000.0 + 33.0 * i)) { sp.Sent(4000.0 + 33.0 * i); ++carried; }
        const bool idle = !sp.Due(rep, srv, 4300.0);
        const bool refresh = sp.Due(rep, srv, 4000.0 + 33.0 * 5 + 3000.0);
        Check("E10 send policy: a report equal to the server's is never sent, a change is fresh and rides exactly 6 updates, then waits, and is repeated 3 s later while the server still lacks it",
              quiet && fresh && carried == 6 && idle && refresh);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// F. end to end on a simulated clock
// ---------------------------------------------------------------------------------------------------------------------------

// The controller's machine in miniature: the SAME classes the engine uses (VisualGuard, DamageReporter, DamageSendPolicy, PedalGuard) over a fake
// car, producing VehicleUpdates that go through the real codec and the real VehicleStore.
struct SimController
{
    DmgCar car;
    JunkGlassCar glass;
    VisualSync vs;
    VisualGuard vguard;
    DamageReporter rep;
    DamageSendPolicy policy;
    proto::VehicleDamage report;
    uint32_t seq = 100;
    double y = 1.0;
    bool guard = true;                       // false: the v6 chain (ReadReport) plus a naive block (what an unguarded reader would send)
    int sentBlocks = 0, sent = 0;
    size_t bytes = 0;
    proto::VehicleUpdate Make(const proto::VehicleDamage& serverBlock, const VisualMask& serverVis, double nowMs, bool readDamage)
    {
        proto::VehicleUpdate u = Drive(0, 0, (float)(y += 0.3), 10.0f);
        float b = 1000.0f;
        car.BodyHealth(b);
        u.bodyHealth = (uint16_t)std::max(0.0f, std::min(1000.0f, b));
        float e = 1000.0f;
        e = car.engine;
        u.engineHealth = (uint16_t)std::max(0.0f, std::min(1000.0f, e));
        const VisualMask m = guard ? ReadReportGuarded(vs, vguard, glass, serverVis, nowMs < 0) : ReadReport(vs, glass);
        u.tyres = m.tyres;
        u.tyresRim = m.rim;
        u.windows = m.windows;
        if (readDamage)
        {
            report = StepDamageController(rep, car, serverBlock, nowMs);
            policy.Observe(report, serverBlock);
        }
        if (policy.Due(report, serverBlock, nowMs))
        {
            u.hasDmg = true;
            u.dmg = report;
        }
        return u;
    }
};

struct SimCopy
{
    DmgCar car;
    DamageApplier ap;
    proto::VehicleState held;
    bool have = false;
    void OnEntry(const proto::VehicleState& e)
    {
        if (e.motionOnly) { if (have) { held.k = e.k; held.flags = e.flags; } return; }
        held = e;
        have = true;
    }
    void Step(double nowMs) { if (have) StepDamageCopy(ap, car, held.dmg, true, nowMs); }
};

static void R3F_EndToEnd()
{
    std::printf("-- R3 F. end to end on a simulated clock (controller -> codec -> VehicleStore -> BuildFrame -> codec -> copy)\n");
    {
        // A crash on the controller reaches the copy: dents, a broken door and lamp, the tank. 30 Hz updates, 30% of the state packets lost.
        StoreRig3 R;
        SimController A;
        SimCopy B;
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        double now = 1000.0;
        size_t updBytes = 0, worldBytes = 0;
        int updates = 0, blocks = 0;
        bool crashed = false;
        for (int tick = 0; tick < 600; tick++)
        {
            now += 33.0;
            if (tick == 60 && !crashed)
            {
                crashed = true;
                A.car.Crash(0, 0.10f, 150.0f);
                A.car.Crash(6, 0.06f, 80.0f);
                A.car.doorBroken[0] = true;
                A.car.lampBroken[0] = true;
                A.car.petrol = 520.0f;
            }
            const proto::VehicleDamage* srv = R.vs.Get(R.veh) ? &R.vs.Get(R.veh)->s.dmg : nullptr;
            proto::VehicleUpdate u = A.Make(srv ? *srv : proto::VehicleDamage(), VisualMask(), now, tick % 15 == 0);
            u.vehId = R.veh;
            const proto::Bytes enc = proto::Encode(u);
            updBytes += enc.size();
            ++updates;
            proto::VehicleUpdate dec;
            if (u.hasDmg)
            {
                A.policy.Sent(now);                            // (the engine marks it sent when the SEND call succeeds, lost packets included)
                ++blocks;
            }
            if (u01(rng) >= 0.30f && proto::Decode(enc.data(), enc.size(), dec))
                R.vs.Update(0, dec, (uint64_t)now);
            proto::VehicleWorld w;
            if (R.Frame(1, (uint64_t)now, w))
            {
                const proto::Bytes we = proto::Encode(w);
                worldBytes += we.size();
                proto::VehicleWorld got;
                if (proto::Decode(we.data(), we.size(), got))
                    for (const proto::VehicleState& e : got.vehicles) B.OnEntry(e);
            }
            B.Step(now);
        }
        const proto::VehicleDamage truth = A.rep.Last();
        const server::VehicleStore::Vehicle* V = R.vs.Get(R.veh);
        const bool reached = V->s.dmg.parts == (0x01 | proto::kVehDmgLampL) && V->s.dmg.dent[0] >= DentToWire(0.10f) - 2 && V->s.dmg.petrol == PetrolToWire(520.0f);
        const bool copyOk = B.car.doorBroken[0] && B.car.lampBroken[0] && std::fabs(B.car.petrol - PetrolFromWire(PetrolToWire(520.0f))) < 1.0f &&
                            std::abs((int)DentToWire(B.car.dent[0]) - (int)DentToWire(0.10f)) <= kDentTolSteps + 1 &&
                            std::abs((int)DentToWire(B.car.dent[6]) - (int)DentToWire(0.06f)) <= kDentTolSteps + 1;
        Check(F("F1 e2e (30%% of the state packets lost, 600 ticks): the controller's crash reaches the server (parts %02x, dent0 %d, petrol %d) and the copy's car shows the broken door and lamp, the tank, and dents within 4 steps of the controller's (probe 0: %d vs %d, probe 6: %d vs %d)",
                V->s.dmg.parts, V->s.dmg.dent[0], V->s.dmg.petrol, (int)DentToWire(B.car.dent[0]), (int)DentToWire(A.car.dent[0]), (int)DentToWire(B.car.dent[6]), (int)DentToWire(A.car.dent[6])),
              reached && copyOk && truth.dent[0] > 0);
        std::printf("      cost: %d VehicleUpdates, %.1f B each on average (v6 clean = 40), the copy's VehicleWorld stream %zu B in 20 s for one car at 1 receiver; the block rode %d updates\n",
                    updates, (double)updBytes / updates, worldBytes, blocks);
    }
    {
        // the root cause end to end: a clean car with junk window slots. v6 chain (guard off, server trusting): the copy smashes windows. v7: nobody does.
        for (int guarded = 0; guarded < 2; guarded++)
        {
            StoreRig3 R(guarded == 0 ? false : true);            // the old chain also had the old server
            SimController A;
            A.guard = guarded != 0;
            A.glass.junk = 0xC0;
            SimCopy B;
            JunkGlassCar copyGlass;
            copyGlass.junk = 0;
            VisualSync copyVs;
            double now = 1000.0;
            int updatesWithWindows = 0;
            for (int tick = 0; tick < 300; tick++)
            {
                now += 33.0;
                const server::VehicleStore::Vehicle* V0 = R.vs.Get(R.veh);
                proto::VehicleUpdate u = A.Make(proto::VehicleDamage(), VisualMask{ V0->s.tyres, V0->s.tyresRim, V0->s.windows }, now, tick % 15 == 0);
                u.vehId = R.veh;
                if (u.windows) ++updatesWithWindows;
                R.vs.Update(0, u, (uint64_t)now);
                proto::VehicleWorld w;
                if (R.Frame(1, (uint64_t)now, w) && !w.vehicles.empty() && !w.vehicles[0].motionOnly)
                {
                    const proto::VehicleState& e = w.vehicles[0];
                    StepVisual(copyVs, copyGlass, VisualMask{ e.tyres, e.tyresRim, e.windows }, false, true, false, now);
                }
            }
            if (guarded == 0)
                Check(F("F2a e2e, the v6 chain (no sanity layer, a server that trusts the report): a clean car with window slots that read 'not intact' sends %d of 300 updates with a smashed window and the other player's copy smashes %d window(s)",
                        updatesWithWindows, copyGlass.smashCalls),
                      updatesWithWindows == 300 && copyGlass.smashCalls > 0);
            else
                Check(F("F2b e2e, v7: the same car sends %d of 300 updates with a smashed window, the server shows windows %02x, the copy smashes %d",
                        updatesWithWindows, R.vs.Get(R.veh)->s.windows, copyGlass.smashCalls),
                      updatesWithWindows == 0 && R.vs.Get(R.veh)->s.windows == 0 && copyGlass.smashCalls == 0);
        }
    }
    {
        // the server's own defence for a client WITHOUT the layer (an old build): the same junk is refused at the store on a car with full health
        StoreRig3 R;
        SimController A;
        A.guard = false;
        A.glass.junk = 0xC0;
        double now = 1000.0;
        for (int tick = 0; tick < 100; tick++)
        {
            now += 33.0;
            proto::VehicleUpdate u = A.Make(proto::VehicleDamage(), VisualMask(), now, false);
            u.vehId = R.veh;
            R.vs.Update(0, u, (uint64_t)now);
        }
        Check(F("F3 e2e: an old-build controller (junk windows, no layer) against the v7 server: the store refuses the bit (%llu refusals) and no receiver ever sees a smashed window",
                (unsigned long long)R.vs.windowRejects()),
              R.vs.Get(R.veh)->s.windows == 0 && R.vs.windowRejects() >= 90);
    }
    {
        // hand-over: the copy that applied the damage becomes the controller; the server's block does not move and nothing is echoed
        StoreRig3 R;
        SimController A;
        SimCopy B;
        double now = 1000.0;
        for (int tick = 0; tick < 200; tick++)
        {
            now += 33.0;
            if (tick == 30)
            {
                A.car.Crash(1, 0.08f, 120.0f);
                A.car.doorBroken[2] = true;
            }
            const server::VehicleStore::Vehicle* V0 = R.vs.Get(R.veh);
            proto::VehicleUpdate u = A.Make(V0->s.dmg, VisualMask(), now, tick % 15 == 0);
            u.vehId = R.veh;
            R.vs.Update(0, u, (uint64_t)now);
            proto::VehicleWorld w;
            if (R.Frame(1, (uint64_t)now, w))
                for (const proto::VehicleState& e : w.vehicles) B.OnEntry(e);
            B.Step(now);
        }
        const proto::VehicleDamage serverBefore = R.vs.Get(R.veh)->s.dmg;
        // B takes the wheel: its OWN reporter starts with its first read of the car it applied the damage to
        DamageReporter repB;
        proto::VehicleDamage outB;
        for (int i = 0; i < 20; i++) outB = StepDamageController(repB, B.car, serverBefore, now += 500.0);
        Check("F4 e2e hand-over: the copy that applied the damage becomes the controller and reports exactly the server's block (no echo, no new rise), whatever small difference its closed loop left",
              B.have && B.car.doorBroken[2] && outB == serverBefore && serverBefore.dent[1] >= DentToWire(0.08f) - 2);
    }
}

// The same wire through the REAL server over loopback (short): the controller's block reaches an observer in the VehicleWorld full entry, a clean car's
// junk window does not, and a milder second report does not clear the block.
static void R3F_RealServer()
{
    std::printf("-- R3 F. the real server over loopback (controller A, observer B)\n");
    Rig rig;
    server::Config cfg = Rig::BaseConfig();
    if (!rig.Start(cfg)) { Check("r3 real server: starts", false); return; }
    Probe alice(rig), bob(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob")) { Check("r3 real server: two players join", false); return; }
    const bool placed = PlaceAt(rig, alice, Kin(100, 100, 30)) && PlaceAt(rig, bob, Kin(103, 100, 30));
    const proto::VehicleResult spawn = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
    const uint16_t vid = spawn.vehId;
    rig.Until([&] { return alice.Vehicle(vid) && bob.Vehicle(vid); }, 2000);
    const proto::VehicleResult enter = VCmd(rig, alice, proto::VehicleOp::Enter, vid, 0);
    uint32_t seq = 500;
    const auto t0 = Clock::now();
    auto sendA = [&](uint16_t body, const proto::VehicleDamage* d, uint8_t windows) {
        proto::VehicleUpdate u = Drive(vid, 100.0f, 104.0f, 0.0f, 0, 0, windows);
        u.bodyHealth = body;
        if (d) { u.hasDmg = true; u.dmg = *d; }
        u.seq = ++seq;
        alice.s.SendVehicleState(u);
    };
    auto pumpA = [&](uint16_t body, const proto::VehicleDamage* d, uint8_t windows, const std::function<bool()>& pred, int ms) {
        const auto s = Clock::now();
        auto next = s;
        while (MsSince(s) < ms)
        {
            if (Clock::now() >= next) { sendA(body, d, windows); next += std::chrono::milliseconds(33); }
            rig.Step();
            if (pred && pred()) return true;
        }
        return pred ? pred() : false;
    };
    Check("r3 real server: A spawned a car and drives it", placed && spawn.code == proto::VehicleResultCode::Ok && enter.code == proto::VehicleResultCode::Ok && rig.srv.FindVehicle(vid) &&
                                                          rig.srv.FindVehicle(vid)->s.ctrl == IdOf(alice));
    // a clean car whose controller (an old build) claims a smashed window: the server refuses it
    pumpA(1000, nullptr, 0x40, nullptr, 700);
    const proto::VehicleState* bs = bob.Vehicle(vid);
    Check("r3 real server: a smashed window claimed by the controller of a car with full health never reaches the observer", bs && bs->windows == 0 && rig.srv.FindVehicle(vid)->s.windows == 0);
    // a real crash: body health falls, the block rides along
    const proto::VehicleDamage crash = Dmg(120, 0x21, { 0, 0, 33, 0, 0, 0, 0, 90 });
    const bool seen = pumpA(640, &crash, 0, [&] { const proto::VehicleState* v = bob.Vehicle(vid); return v && v->dmg == crash; }, 3000);
    Check("r3 real server: after a crash (body health 640) the controller's damage block reaches the observer's copy in a VehicleWorld full entry", seen);
    const proto::VehicleDamage milder = Dmg(250, 0, { 0, 0, 10 });
    pumpA(640, &milder, 0, nullptr, 700);
    const proto::VehicleState* after = bob.Vehicle(vid);
    Check("r3 real server: a milder / cleaner second report from the controller does not clear the block (damage only goes up)", after && after->dmg == crash);
    (void)t0;
}

static void RunR3T4Tests()
{
    R3A_Reproduce();
    R3B_Guards();
    R3C_Wire();
    R3D_Server();
    R3E_ReporterApplier();
    R3F_EndToEnd();
    R3F_RealServer();
}
