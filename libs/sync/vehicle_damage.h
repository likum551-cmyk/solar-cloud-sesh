// Vehicle damage state for the OTHER clients (Desync Overhaul round 3, T4; protocol v7), and the SANITY layer that decides which
// reads of the game may reach the wire at all. Pure C++17, header-only, no game calls: the engine reads the natives and passes the
// values in, executes the commands this module names and tells it what worked. Everything is testable offline
// (tests/r3_t4_tests.h).
//
// WHY A SANITY LAYER (measured live, GI's 0.4.7 log: "199 updates sent (0 with a burst tyre, 199 with a smashed window, 96 with brake
// lights, 96 with the handbrake)"): IS_VEHICLE_WINDOW_INTACT is called for slots 0..7 whatever the model has glass for, and the
// round-2 code reported every "not intact" slot as a smashed window from the very first sample, so a clean car showed 199 of 199
// updates with smashed windows and its copies smashed windows on someone else's clean car. Nothing in the old chain could tell a
// slot that does not exist (or that always reads the same) from a window somebody broke. SlotGuard learns each slot's BASELINE at the
// first read: a bit that is already set at the first sight, while the server does not show it, is an "unreliable slot" (counted,
// logged once, never sent, and masked out of the copy's own readback); a bit is damage only when it CHANGES from a baseline that
// read intact, and (without a corroborating signal: the body health fell, the local player shot lately) only after it held for a
// second read. PedalGuard does the same for the handbrake: a handbrake that is "held" at the first sample of a moving car, or that
// stays held while the car does not slow down, is a stuck read and is not sent; the brake lights are then the foot brake alone.
//
// THE DAMAGE BLOCK (what decides how a damaged car looks): petrol tank health, which doors were broken off, which headlights are
// broken, and a coarse dent map of kDentProbes fixed points around the hull. Body and engine health were already on the wire.
//   controller (ctrl == me)   DamageReporter: reads the car (ReadDamage), runs every read through its baselines and corroboration,
//                             and returns the block for VehicleUpdate. Damage only goes UP: the report starts from the server's
//                             block and only ever adds a verified rise (the server merges monotonically too).
//   copy (somebody else's)    DamageApplier: brings the car to the server's block. Doors: SET_VEHICLE_DOOR_BROKEN; lamps: no setter
//                             exists in the game's script API, so a small SET_VEHICLE_DAMAGE at the lamp, checked by the headlight
//                             read-back; petrol: SET_VEHICLE_PETROL_TANK_HEALTH; dents: SET_VEHICLE_DAMAGE at the probe, CLOSED LOOP:
//                             after each replay the probe is read back, the damage-per-metre gain is learned from what the replay
//                             really did, and a probe that does not respond stops after kMaxRounds (a native that does nothing
//                             costs a handful of calls, never a stream). A copy never reports damage (it only follows the controller).
//
// Natives are never assumed: a failed read is "no information", a failed command round is retried and then the car is left alone for
// a while (VisualSync's rule).
#pragma once

#include "protocol/protocol.h"
#include "sync/vehicle_visual.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gage::sync {

    // ======================================================================================================================
    // 1. The sanity layer: per-slot baselines
    // ======================================================================================================================

    // One bit group (8 slots). See the file comment. `raw` = the bits the game reports now, `serverBits` = the bits the server shows
    // (a bit the server shows is corroborated), `corroborated` = an outside signal that damage just happened.
    class SlotGuard
    {
    public:
        struct Out
        {
            uint8_t bits = 0;             // the bits that may be believed (reported by a controller / compared by a copy)
            uint8_t newlyUnreliable = 0;  // slots flagged unreliable by THIS call (log them once)
        };

        void Reset() { *this = SlotGuard(); }

        Out Update(uint8_t raw, uint8_t serverBits, bool corroborated, int confirmReads = 2)
        {
            Out o;
            if (!seen_)
            {
                seen_ = true;
                for (int s = 0; s < 8; ++s)
                {
                    const uint8_t b = (uint8_t)(1u << s);
                    if (!(raw & b)) continue;
                    if (serverBits & b)
                        trusted_ = (uint8_t)(trusted_ | b);                 // the server shows it too: a real, known fact
                    else
                    {
                        unreliable_ = (uint8_t)(unreliable_ | b);           // set from the very first read and nobody else sees it
                        o.newlyUnreliable = (uint8_t)(o.newlyUnreliable | b);
                        ++flaggedTotal_;
                    }
                }
                o.bits = trusted_;
                return o;
            }
            for (int s = 0; s < 8; ++s)
            {
                const uint8_t b = (uint8_t)(1u << s);
                if (!(raw & b))
                {
                    // A slot that reads clear is a real slot: whatever it read before was a transient, not a permanent lie.
                    trusted_ = (uint8_t)(trusted_ & ~b);
                    unreliable_ = (uint8_t)(unreliable_ & ~b);
                    streak_[s] = 0;
                    continue;
                }
                if (unreliable_ & b)
                {
                    // Believed only while the server itself shows the bit (a Hit by somebody else, a hand-over).
                    trusted_ = (uint8_t)((serverBits & b) ? (trusted_ | b) : (trusted_ & ~b));
                    continue;
                }
                if (trusted_ & b) continue;
                if ((serverBits & b) || corroborated || ++streak_[s] >= confirmReads)
                {
                    trusted_ = (uint8_t)(trusted_ | b);
                    streak_[s] = 0;
                }
            }
            o.bits = trusted_;
            return o;
        }

        bool Seen() const { return seen_; }
        uint8_t Unreliable() const { return unreliable_; }
        uint8_t Trusted() const { return trusted_; }
        uint32_t FlaggedTotal() const { return flaggedTotal_; }

    private:
        bool seen_ = false;
        uint8_t trusted_ = 0, unreliable_ = 0;
        uint8_t streak_[8] = {};
        uint32_t flaggedTotal_ = 0;
    };

    // The three tyre / window groups of one car. Filter() rewrites a readback in place: a slot that is not believed reads as intact.
    class VisualGuard
    {
    public:
        void Reset() { *this = VisualGuard(); }

        // Returns the slots flagged unreliable by this call (for one log line each).
        VisualMask Filter(VisualMask& local, bool readTyres, bool readWindows, const VisualMask& server, bool corroborated)
        {
            VisualMask fresh;
            if (readTyres)
            {
                const SlotGuard::Out t = tyres_.Update(local.tyres, server.tyres, corroborated);
                const SlotGuard::Out r = rim_.Update(local.rim, server.rim, corroborated);
                fresh.tyres = t.newlyUnreliable;
                fresh.rim = r.newlyUnreliable;
                local.tyres = t.bits;
                local.rim = (uint8_t)(r.bits & t.bits);
                local.tyres = (uint8_t)(local.tyres | local.rim);
            }
            if (readWindows)
            {
                const SlotGuard::Out w = windows_.Update(local.windows, server.windows, corroborated);
                fresh.windows = w.newlyUnreliable;
                local.windows = w.bits;
            }
            return fresh;
        }

        VisualMask Unreliable() const { return VisualMask{ tyres_.Unreliable(), rim_.Unreliable(), windows_.Unreliable() }; }
        uint32_t FlaggedTotal() const { return tyres_.FlaggedTotal() + rim_.FlaggedTotal() + windows_.FlaggedTotal(); }

    private:
        SlotGuard tyres_, rim_, windows_;
    };

    // StepVisual / ReadReport (vehicle_visual.h) with every readback run through a VisualGuard first. The COPY uses StepVisualGuarded (its
    // own readback is what it compares with the server's mask AND what it reports as a pop of the local player's: a junk slot would otherwise
    // be fixed, smashed, or reported as a vehicle Hit); the CONTROLLER sends ReadReportGuarded (what goes on the wire). `corroborated`: an
    // outside signal that damage just happened (a copy: the local player shot lately; a controller: its body health fell). `flagged`
    // (optional) receives the slots this call flagged unreliable.
    template <class Game>
    inline VisualOut StepVisualGuarded(VisualSync& vs, VisualGuard& guard, Game& g, const VisualMask& server, bool iControl, bool isNear,
                                       bool shotRecent, double nowMs, bool corroborated, bool* ran = nullptr, VisualMask* flagged = nullptr)
    {
        if (ran) *ran = false;
        if (!vs.Due(nowMs, server, iControl, isNear, shotRecent))
            return VisualOut();
        if (ran) *ran = true;
        VisualIn in;
        in.server = server;
        in.iControl = iControl;
        in.nowMs = nowMs;
        ReadVisual(g, in.local, in.readTyres, in.readWindows);
        const VisualMask fresh = guard.Filter(in.local, in.readTyres, in.readWindows, NormalizeVisual(server), corroborated);
        if (flagged) *flagged = fresh;
        const VisualOut out = vs.Update(in);
        const VisualCmd done = out.cmd.Any() ? ExecuteVisual(g, out.cmd) : VisualCmd();
        vs.Commit(out.cmd, done, nowMs);
        return out;
    }

    template <class Game>
    inline VisualMask ReadReportGuarded(const VisualSync& vs, VisualGuard& guard, Game& g, const VisualMask& server, bool corroborated,
                                        VisualMask* flagged = nullptr)
    {
        VisualMask m;
        bool t = false, w = false;
        ReadVisual(g, m, t, w);
        const VisualMask fresh = guard.Filter(m, t, w, NormalizeVisual(server), corroborated);
        if (flagged) *flagged = fresh;
        return vs.Reported(t, w, m);
    }

    // ---- the handbrake -----------------------------------------------------------------------------------------------

    struct PedalStats
    {
        uint32_t samples = 0;           // Classify calls with a good input
        uint32_t moving = 0;            // ... with the car rolling faster than 1 m/s
        uint32_t accelHigh = 0, brakeHigh = 0, handHigh = 0;   // raw pedal reads above 0.1 / 0.1 / 0.5
        uint32_t brakeRolling = 0;      // foot brake above 0.1 while rolling forward (the case that makes brake lights without the handbrake)
        uint32_t handDropped = 0;       // handbrake samples NOT sent because the read is a stuck one
        uint32_t stuckRuns = 0;         // runs judged stuck (a baseline suspect counts too)
        uint32_t brakeDropped = 0;      // foot-brake samples NOT turned into brake lights because the read is a stuck one
        float maxAccel = 0.0f, maxBrake = 0.0f, maxHand = 0.0f;
    };

    struct PedalGuardCfg
    {
        double stuckAfterMs = 2000.0;   // a held handbrake that has not slowed the car after this long...
        float stuckMinSpeed = 5.0f;     // ... at least this fast (m/s)...
        float stuckSlowFrac = 0.85f;    // ... and still above this fraction of the speed it had when the run began, is stuck
        float baselineMoving = 3.0f;    // a handbrake that is already held at the FIRST sample of a car rolling this fast is suspect
        double brakeStuckAfterMs = 3000.0;   // the foot brake held this long while the car rolls forward and does not slow down is stuck too
    };
    class PedalGuard
    {
    public:
        using Cfg = PedalGuardCfg;
        explicit PedalGuard(const Cfg& cfg = Cfg()) : cfg_(cfg) {}

        void Reset() { const Cfg c = cfg_; *this = PedalGuard(c); }

        // The brake state to send: ClassifyBrake, with a stuck handbrake withheld (and the brake lights then from the foot only).
        BrakeState Classify(bool inputOk, float accel, float brake, float hand, float vFwd, double nowMs)
        {
            const BrakeState raw = ClassifyBrake(inputOk, accel, brake, hand, vFwd);
            if (!inputOk || !std::isfinite(accel) || !std::isfinite(brake) || !std::isfinite(hand) || !std::isfinite(vFwd))
            {
                runOn_ = false;
                return raw;
            }
            ++stats_.samples;
            const float speed = std::fabs(vFwd);
            if (speed > 1.0f) ++stats_.moving;
            if (accel > 0.1f) ++stats_.accelHigh;
            if (brake > 0.1f) ++stats_.brakeHigh;
            if (hand > 0.5f) ++stats_.handHigh;
            if (brake > 0.1f && vFwd > 0.5f) ++stats_.brakeRolling;
            stats_.maxAccel = std::max(stats_.maxAccel, accel);
            stats_.maxBrake = std::max(stats_.maxBrake, brake);
            stats_.maxHand = std::max(stats_.maxHand, hand);
            const bool high = hand > 0.5f;
            if (!seen_)
            {
                seen_ = true;
                if (high && speed >= cfg_.baselineMoving)
                {
                    suspect_ = true;
                    ++stats_.stuckRuns;
                }
            }
            if (high)
            {
                if (!runOn_)
                {
                    runOn_ = true;
                    runStartMs_ = nowMs;
                    runSpeed_ = speed;
                }
                else if (!suspect_ && nowMs - runStartMs_ >= cfg_.stuckAfterMs && speed >= cfg_.stuckMinSpeed &&
                         speed > runSpeed_ * cfg_.stuckSlowFrac)
                {
                    suspect_ = true;
                    ++stats_.stuckRuns;
                }
            }
            else
            {
                runOn_ = false;
                suspect_ = false;
            }
            // The foot brake: held while the car rolls forward at speed and the car does not slow down (it keeps its speed or gains it).
            const bool brakeHeld = brake > 0.1f && vFwd >= cfg_.stuckMinSpeed;
            if (brakeHeld)
            {
                if (!brakeRunOn_)
                {
                    brakeRunOn_ = true;
                    brakeRunStartMs_ = nowMs;
                    brakeRunSpeed_ = vFwd;
                }
                else if (!brakeSuspect_ && nowMs - brakeRunStartMs_ >= cfg_.brakeStuckAfterMs && vFwd > brakeRunSpeed_ * cfg_.stuckSlowFrac)
                {
                    brakeSuspect_ = true;
                    ++stats_.stuckRuns;
                }
            }
            else
            {
                brakeRunOn_ = false;
                if (brake <= 0.1f) brakeSuspect_ = false;
            }
            BrakeState b = raw;
            if (suspect_ && raw.handbrake)
            {
                ++stats_.handDropped;
                b.handbrake = false;
                b.brakeLights = (brake > 0.1f && vFwd > 0.5f && !brakeSuspect_) || (accel > 0.1f && vFwd < -0.5f);
            }
            if (brakeSuspect_ && brake > 0.1f)
            {
                ++stats_.brakeDropped;
                b.brakeLights = b.handbrake || (accel > 0.1f && vFwd < -0.5f);
            }
            return b;
        }

        const PedalStats& Stats() const { return stats_; }

    private:
        Cfg cfg_;
        bool seen_ = false, suspect_ = false, runOn_ = false;
        bool brakeRunOn_ = false, brakeSuspect_ = false;
        double runStartMs_ = 0.0, brakeRunStartMs_ = 0.0;
        float runSpeed_ = 0.0f, brakeRunSpeed_ = 0.0f;
        PedalStats stats_;
    };

    // The COPY's side of the same worry: a handbrake bit that outlives a moving car (a stuck read from an old client, a lost clear) must not
    // lock a puppet's wheels for ever, and a locked wheel on a kinematic copy fights the drive commands. A held handbrake is honoured on a
    // standing car (nothing to see, nothing to fight) and for at most maxMovingMs of continuous movement; then it is released and ignored
    // until the controller clears the bit once.
    struct HandbrakeLimiterCfg { float movingMps = 2.0f; double maxMovingMs = 4000.0; };
    class HandbrakeLimiter
    {
    public:
        using Cfg = HandbrakeLimiterCfg;
        explicit HandbrakeLimiter(const Cfg& cfg = Cfg()) : cfg_(cfg) {}
        void Reset() { const Cfg c = cfg_; *this = HandbrakeLimiter(c); }

        // `serverBit` = kVeh2Handbrake in the newest state, `speed` the car's speed (m/s). Returns the handbrake state the copy should show.
        bool Effective(bool serverBit, float speed, double nowMs)
        {
            if (!serverBit)
            {
                suppressed_ = false;
                movingSince_ = -1.0;
                return false;
            }
            if (suppressed_) return false;
            if (std::isfinite(speed) && speed >= cfg_.movingMps)
            {
                if (movingSince_ < 0.0) movingSince_ = nowMs;
                else if (nowMs - movingSince_ > cfg_.maxMovingMs)
                {
                    suppressed_ = true;
                    ++releases_;
                    return false;
                }
            }
            else
                movingSince_ = -1.0;
            return true;
        }
        uint32_t Releases() const { return releases_; }
        bool Suppressed() const { return suppressed_; }

    private:
        Cfg cfg_;
        bool suppressed_ = false;
        double movingSince_ = -1.0;
        uint32_t releases_ = 0;
    };

    // ======================================================================================================================
    // 2. The damage block
    // ======================================================================================================================

    constexpr int kDentProbes = (int)proto::kVehDentProbes;
    static_assert(kDentProbes == 8, "probe count");
    constexpr int kDentTolSteps = 3;               // 6 mm: a difference below this is read noise, not a dent

    // What the entity shows, the groups that could be read. dent[i] is the probe's depth in wire steps (2 mm). `doors` = broken-off
    // doors (IS_VEHICLE_DOOR_DAMAGED and GET_IS_DOOR_VALID), `lamps` bit 0 = left headlight broken, bit 1 = right.
    struct DamageRaw
    {
        bool petrolOk = false, doorsOk = false, lampsOk = false, dentOk = false, bodyOk = false;
        float petrol = 1000.0f;
        uint8_t doors = 0, lamps = 0;
        uint8_t dent[kDentProbes] = {};
        float body = 1000.0f;
        bool Any() const { return petrolOk || doorsOk || lampsOk || dentOk; }
    };

    inline uint8_t PetrolToWire(float health)
    {
        if (!std::isfinite(health)) return proto::kPetrolFullWire;
        const float h = std::max(0.0f, std::min(1000.0f, health));
        return (uint8_t)std::min<int>(proto::kPetrolFullWire, (int)std::lround(h / 4.0f));
    }
    inline float PetrolFromWire(uint8_t w) { return (float)std::min<int>(w, proto::kPetrolFullWire) * 4.0f; }
    inline uint8_t DentToWire(float metres)
    {
        if (!std::isfinite(metres) || metres <= 0.0f) return 0;
        return (uint8_t)std::min<long>(255, std::lround(metres / proto::kVehDentStepM));
    }
    inline float DentFromWire(uint8_t steps) { return (float)steps * proto::kVehDentStepM; }

    // proto parts byte <-> the two read groups.
    inline uint8_t PartsFromRaw(const DamageRaw& r)
    {
        return (uint8_t)((r.doorsOk ? (r.doors & proto::kVehDmgDoorsMask) : 0) | (r.lampsOk ? ((r.lamps & 1) ? proto::kVehDmgLampL : 0) : 0) |
                         (r.lampsOk ? ((r.lamps & 2) ? proto::kVehDmgLampR : 0) : 0));
    }

    // ---- the probe geometry (shared by sender and receiver, so a probe means the same place on both) -----------------------
    // The eight probes sit ON the hull at bumper height, in the car's own frame (x right, y forward, z up, metres from the origin):
    //   0 front left, 1 front right, 2 front middle, 3 rear left, 4 rear right, 5 rear middle, 6 left middle, 7 right middle.
    struct DentHull
    {
        float halfWidth = 0.95f, front = 2.2f, rear = -2.2f, z = 0.1f;
    };
    struct DentPoint { float x, y, z; };
    inline DentPoint DentProbePoint(int i, const DentHull& h)
    {
        const float hw = h.halfWidth, f = h.front, r = h.rear;
        switch (i)
        {
        case 0:  return { -hw * 0.75f, f, h.z };
        case 1:  return { hw * 0.75f, f, h.z };
        case 2:  return { 0.0f, f, h.z };
        case 3:  return { -hw * 0.75f, r, h.z };
        case 4:  return { hw * 0.75f, r, h.z };
        case 5:  return { 0.0f, r, h.z };
        case 6:  return { -hw, 0.0f, h.z };
        default: return { hw, 0.0f, h.z };
        }
    }
    // The headlights' places (front corners, higher than the bumper): lamp 0 = left, 1 = right.
    inline DentPoint LampPoint(int lamp, const DentHull& h) { return { (lamp ? 1.0f : -1.0f) * h.halfWidth * 0.62f, h.front - 0.15f, h.z + 0.35f }; }

    // ---- the controller's reporter ----------------------------------------------------------------------------------------

    struct DamageStats
    {
        uint32_t reads = 0;
        uint32_t unreliableParts = 0, unreliableDents = 0, unreliablePetrol = 0;   // slots flagged (once each, counted on the call that flagged them)
        uint32_t sentRises = 0;                                                    // verified rises added to the report
    };

    struct DamageReporterCfg { int confirmReads = 2; double corroborateMs = 3000.0; float bodyDropForDents = 5.0f; };
    class DamageReporter
    {
    public:
        using Cfg = DamageReporterCfg;
        explicit DamageReporter(const Cfg& cfg = Cfg()) : cfg_(cfg) {}

        void Reset() { const Cfg c = cfg_; *this = DamageReporter(c); }

        struct Flagged { uint8_t parts = 0, dents = 0; bool petrol = false; };

        // One read of the entity. `server` = the block the server shows now. Returns the block to report; `flagged` (optional) names
        // what this call flagged unreliable. A group that could not be read keeps its previous report.
        proto::VehicleDamage Update(const DamageRaw& raw, const proto::VehicleDamage& server, double nowMs, Flagged* flagged = nullptr)
        {
            if (flagged) *flagged = Flagged();
            ++stats_.reads;
            const bool first = !seen_;
            if (first)
            {
                seen_ = true;
                out_ = server;                                   // neutral: report what the server already shows
                bodyBase_ = raw.bodyOk ? raw.body : 1000.0f;
                lastBody_ = bodyBase_;
            }
            if (raw.bodyOk)
            {
                if (raw.body < lastBody_ - 0.5f) fellAtMs_ = nowMs;
                lastBody_ = raw.body;
            }
            const bool fell = fellAtMs_ > -1e8 && nowMs - fellAtMs_ <= cfg_.corroborateMs;
            const bool bodyDropped = raw.bodyOk && bodyBase_ - raw.body >= cfg_.bodyDropForDents;
            // ---- doors / headlights
            if (raw.doorsOk || raw.lampsOk)
            {
                const SlotGuard::Out p = parts_.Update(PartsFromRaw(raw), server.parts, fell, cfg_.confirmReads);
                if (flagged) flagged->parts = p.newlyUnreliable;
                stats_.unreliableParts += (uint32_t)PopCount8(p.newlyUnreliable);
                const uint8_t add = (uint8_t)(p.bits & ~out_.parts);
                if (add) stats_.sentRises += (uint32_t)PopCount8(add);
                out_.parts = (uint8_t)(out_.parts | p.bits);     // monotonic
            }
            // ---- dents
            if (raw.dentOk)
            {
                for (int i = 0; i < kDentProbes; ++i)
                {
                    const int s = raw.dent[i], sv = server.dent[i];
                    if (first)
                    {
                        if (s > sv + kDentTolSteps)
                        {
                            dentUnreliable_ = (uint8_t)(dentUnreliable_ | (1u << i));      // dented at the first sight and nobody else sees it
                            if (flagged) flagged->dents = (uint8_t)(flagged->dents | (1u << i));
                            ++stats_.unreliableDents;
                        }
                        continue;
                    }
                    const uint8_t b = (uint8_t)(1u << i);
                    if (s <= kDentTolSteps)
                    {
                        dentUnreliable_ = (uint8_t)(dentUnreliable_ & ~b);                // it reads clear: a real probe
                        dentStreak_[i] = 0;
                        continue;
                    }
                    if (dentUnreliable_ & b) continue;                                    // blocked: the server's own value is already in out_
                    if (s <= (int)out_.dent[i] + kDentTolSteps) { dentStreak_[i] = 0; continue; }   // no news
                    // A rise: believed when the server agrees, when the body health just fell, or when it held on a second read with the
                    // body health below what it was at the first sight (a dent with no health loss anywhere is not a crash).
                    const bool agrees = s <= sv + kDentTolSteps;
                    if (agrees || fell || (bodyDropped && ++dentStreak_[i] >= cfg_.confirmReads))
                    {
                        out_.dent[i] = (uint8_t)std::min(255, s);
                        dentStreak_[i] = 0;
                        ++stats_.sentRises;
                    }
                }
            }
            // ---- petrol
            if (raw.petrolOk && std::isfinite(raw.petrol))
            {
                const int steps = PetrolToWire(raw.petrol);
                const int sv = server.petrol;
                if (first)
                {
                    if (steps + 12 < sv && !fell)
                    {
                        petrolUnreliable_ = true;                                          // a tank that is half empty at the first sight of a clean car
                        if (flagged) flagged->petrol = true;
                        ++stats_.unreliablePetrol;
                    }
                }
                else if (steps >= (int)out_.petrol)
                {
                    petrolStreak_ = 0;
                    if (steps >= sv - 4) petrolUnreliable_ = false;                        // it reads like the server again: a real tank
                }
                else if (!petrolUnreliable_)
                {
                    const bool agrees = steps >= sv - 4;
                    if (agrees || fell || (bodyDropped && ++petrolStreak_ >= cfg_.confirmReads))
                    {
                        out_.petrol = (uint8_t)steps;
                        petrolStreak_ = 0;
                        ++stats_.sentRises;
                    }
                }
            }
            return out_;
        }

        const proto::VehicleDamage& Last() const { return out_; }
        uint8_t UnreliableParts() const { return parts_.Unreliable(); }
        uint8_t UnreliableDents() const { return dentUnreliable_; }
        const DamageStats& Stats() const { return stats_; }
        bool Seen() const { return seen_; }

    private:
        Cfg cfg_;
        bool seen_ = false;
        proto::VehicleDamage out_;
        SlotGuard parts_;
        uint8_t dentUnreliable_ = 0;
        uint8_t dentStreak_[kDentProbes] = {};
        bool petrolUnreliable_ = false;
        uint8_t petrolStreak_ = 0;
        float bodyBase_ = 1000.0f, lastBody_ = 1000.0f;
        double fellAtMs_ = -1e9;
        DamageStats stats_;
    };

    // ---- when the block rides a VehicleUpdate --------------------------------------------------------------------------------
    // A change of the report is queued for `repeats` updates (the state channel may lose one), and while the server still does not show the
    // report it is repeated every refreshMs (a controller whose block was lost for good). A car whose report equals the server's sends none:
    // an undamaged car costs not one byte more than in v6.
    struct DamageSendPolicyCfg { int repeats = 6; double refreshMs = 3000.0; };
    class DamageSendPolicy
    {
    public:
        using Cfg = DamageSendPolicyCfg;
        explicit DamageSendPolicy(const Cfg& cfg = Cfg()) : cfg_(cfg) {}
        void Reset() { const Cfg c = cfg_; *this = DamageSendPolicy(c); }

        void Observe(const proto::VehicleDamage& report, const proto::VehicleDamage& server)
        {
            if (report == last_) return;
            last_ = report;
            if (report != server)
            {
                repeat_ = cfg_.repeats;
                fresh_ = true;
            }
        }
        bool Due(const proto::VehicleDamage& report, const proto::VehicleDamage& server, double nowMs) const
        {
            return repeat_ > 0 || (report != server && nowMs - sentMs_ >= cfg_.refreshMs);
        }
        bool Fresh() const { return fresh_; }              // a new change that no update has carried yet: send at once, also for a still car
        void Sent(double nowMs)
        {
            if (repeat_ > 0) --repeat_;
            fresh_ = false;
            sentMs_ = nowMs;
        }

    private:
        Cfg cfg_;
        proto::VehicleDamage last_;
        int repeat_ = 0;
        bool fresh_ = false;
        double sentMs_ = -1e9;
    };

    // ---- the copy's applier -------------------------------------------------------------------------------------------------

    struct DamageCmd
    {
        uint8_t breakDoors = 0;                    // SET_VEHICLE_DOOR_BROKEN for door i
        uint8_t lamps = 0;                         // bit 0 left, bit 1 right: a lamp replay
        float   lampDamage[2] = {};
        uint8_t dents = 0;                         // probe bits: a dent replay
        float   dentDamage[kDentProbes] = {};
        bool    petrol = false;
        float   petrolTo = 1000.0f;
        bool Any() const { return breakDoors || lamps || dents || petrol; }
        bool Replays() const { return lamps || dents; }
    };

    struct ApplierStats
    {
        uint32_t reads = 0, divergent = 0;
        uint32_t doorsBroken = 0, lampsReplayed = 0, dentsReplayed = 0, petrolSet = 0;
        uint32_t dentConverged = 0;                // probes that reached the server's depth (within tolerance) after a replay
        uint32_t dentNoEffect = 0;                 // a replay after which the probe did not move
        uint32_t dentGaveUp = 0;                   // probes left alone after kMaxRounds
        uint32_t gainUpdates = 0;
        uint32_t cmdFailures = 0, suspends = 0;
    };

    struct DamageApplierCfg
    {
        double verifyMs = 450.0;               // after a replay, the probes are read back this long after
        double readCopyMs = 2500.0;            // a damaged near copy is re-read this often (a level check against the server's block)
        int    maxRounds = 4;                  // replays per probe / lamp / door per server value
        int    failLimit = 3;
        double failBackoffMs = 5000.0;
        float  initialGain = 500.0f;           // SET_VEHICLE_DAMAGE damage per metre of dent: a first guess, learned from read-backs
        float  firstStep = 0.4f, nextStep = 0.9f;   // the share of the missing depth the first / later replays aim for (never overshoot)
        float  minDamage = 8.0f, maxDamage = 900.0f;
    };
    class DamageApplier
    {
    public:
        using Cfg = DamageApplierCfg;
        explicit DamageApplier(const Cfg& cfg = Cfg()) : cfg_(cfg), gain_(cfg.initialGain) {}

        void Reset() { const Cfg c = cfg_; *this = DamageApplier(c); }

        // Whether the engine should read the entity and call Update this frame. A car whose server block is clean costs nothing.
        bool Due(double nowMs, const proto::VehicleDamage& server, bool isNear) const
        {
            if (nowMs < suspendUntil_) return false;
            if (server == proto::VehicleDamage()) return false;
            if (!seen_ || server != lastServer_) return true;
            if (verifyAtMs_ > 0.0 && nowMs >= verifyAtMs_) return true;
            return isNear && nowMs - lastReadMs_ >= cfg_.readCopyMs && Unfinished();
        }

        DamageCmd Update(const DamageRaw& raw, const proto::VehicleDamage& server, double nowMs)
        {
            DamageCmd cmd;
            seen_ = true;
            lastReadMs_ = nowMs;
            verifyAtMs_ = 0.0;
            if (raw.Any()) ++stats_.reads;
            if (server != lastServer_)
            {
                for (int i = 0; i < kDentProbes; ++i)
                    if (server.dent[i] != lastServer_.dent[i]) rounds_[i] = 0;
                for (int b = 0; b < 8; ++b)
                    if (((server.parts ^ lastServer_.parts) >> b) & 1) partRounds_[b] = 0;
                if (server.petrol != lastServer_.petrol) petrolRounds_ = 0;
                lastServer_ = server;
            }
            const bool suspended = nowMs < suspendUntil_;
            // ---- learn from the replays of the previous round
            if (raw.dentOk)
                for (int i = 0; i < kDentProbes; ++i)
                {
                    if (!pending_[i].on) continue;
                    pending_[i].on = false;
                    const int delta = (int)raw.dent[i] - pending_[i].before;
                    if (delta >= kDentTolSteps)
                    {
                        const float g = pending_[i].damage / (DentFromWire((uint8_t)delta));
                        if (std::isfinite(g) && g > 0.0f)
                        {
                            gain_ = std::max(30.0f, std::min(6000.0f, 0.5f * gain_ + 0.5f * g));
                            ++stats_.gainUpdates;
                        }
                    }
                    else
                        ++stats_.dentNoEffect;
                }
            else
                for (auto& p : pending_) p.on = false;
            // ---- doors
            {
                const uint8_t want = (uint8_t)(server.parts & proto::kVehDmgDoorsMask);
                const uint8_t have = raw.doorsOk ? (uint8_t)(raw.doors & proto::kVehDmgDoorsMask) : appliedDoors_;
                const uint8_t miss = (uint8_t)(want & ~have);
                for (int d = 0; d < 6 && !suspended; ++d)
                    if ((miss & (1u << d)) && partRounds_[d] < cfg_.maxRounds) cmd.breakDoors = (uint8_t)(cmd.breakDoors | (1u << d));
                if (miss) ++stats_.divergent;
            }
            // ---- headlights (a replay, escalating)
            {
                const uint8_t have = raw.lampsOk ? raw.lamps : appliedLamps_;
                const uint8_t wantLamps = (uint8_t)(((server.parts & proto::kVehDmgLampL) ? 1 : 0) | ((server.parts & proto::kVehDmgLampR) ? 2 : 0));
                const uint8_t miss = (uint8_t)(wantLamps & ~have);
                for (int l = 0; l < 2 && !suspended; ++l)
                {
                    if (!(miss & (1u << l))) continue;
                    const int r = partRounds_[6 + l];
                    if (r >= cfg_.maxRounds) continue;
                    cmd.lamps = (uint8_t)(cmd.lamps | (1u << l));
                    cmd.lampDamage[l] = 120.0f * (float)(1 << r);          // 120, 240, 480, 960: only as much as the read-back says it takes
                }
            }
            // ---- petrol
            if (!suspended && petrolRounds_ < cfg_.maxRounds)
            {
                const float want = PetrolFromWire(server.petrol);
                const float have = raw.petrolOk ? raw.petrol : appliedPetrol_;
                if (std::isfinite(have) && have - want > 30.0f)
                {
                    cmd.petrol = true;
                    cmd.petrolTo = want;
                }
            }
            // ---- dents (closed loop)
            bool unfinished = false;
            for (int i = 0; i < kDentProbes; ++i)
            {
                const int want = server.dent[i];
                if (!want) continue;
                const int have = raw.dentOk ? (int)raw.dent[i] : (int)appliedDent_[i];
                const int need = want - have;
                if (need <= kDentTolSteps)
                {
                    if (rounds_[i] > 0 && !converged_[i])
                    {
                        converged_[i] = true;
                        ++stats_.dentConverged;
                    }
                    continue;
                }
                converged_[i] = false;
                ++stats_.divergent;
                if (rounds_[i] >= cfg_.maxRounds || (!raw.dentOk && rounds_[i] >= 1))
                {
                    if (!gaveUp_[i])
                    {
                        gaveUp_[i] = true;
                        ++stats_.dentGaveUp;
                    }
                    continue;
                }
                unfinished = true;
                if (suspended) continue;
                const float metres = DentFromWire((uint8_t)std::min(255, need));
                const float share = rounds_[i] == 0 ? cfg_.firstStep : cfg_.nextStep;
                const float dmg = std::max(cfg_.minDamage, std::min(cfg_.maxDamage, metres * gain_ * share));
                cmd.dents = (uint8_t)(cmd.dents | (1u << i));
                cmd.dentDamage[i] = dmg;
                pending_[i].before = have;
                pending_[i].damage = dmg;
            }
            unfinished_ = unfinished || cmd.Any();
            return cmd;
        }

        // The engine's answer after it called the natives for `issued`.
        void Commit(const DamageCmd& issued, const DamageCmd& done, double nowMs)
        {
            appliedDoors_ = (uint8_t)(appliedDoors_ | done.breakDoors);
            appliedLamps_ = (uint8_t)(appliedLamps_ | done.lamps);
            for (int b = 0; b < 6; ++b)
                if (issued.breakDoors & (1u << b)) ++partRounds_[b];
            for (int l = 0; l < 2; ++l)
                if (issued.lamps & (1u << l)) ++partRounds_[6 + l];
            for (int i = 0; i < kDentProbes; ++i)
            {
                if (!(issued.dents & (1u << i))) continue;
                ++rounds_[i];
                const bool ok = (done.dents & (1u << i)) != 0;
                pending_[i].on = ok;                                                      // the next read-back says what the replay did
                if (ok)
                {
                    appliedDent_[i] = lastServer_.dent[i];                                // the blind estimate: we aimed at the server's depth
                    verifyAtMs_ = nowMs + cfg_.verifyMs;
                }
            }
            if (issued.petrol)
            {
                ++petrolRounds_;
                if (done.petrol) appliedPetrol_ = issued.petrolTo;
            }
            if (issued.lamps || issued.breakDoors) verifyAtMs_ = nowMs + cfg_.verifyMs;
            stats_.doorsBroken += (uint32_t)PopCount8(done.breakDoors);
            stats_.lampsReplayed += (uint32_t)PopCount8(done.lamps);
            stats_.dentsReplayed += (uint32_t)PopCount8(done.dents);
            stats_.petrolSet += done.petrol ? 1u : 0u;
            if (!issued.Any()) return;
            if (done.Any())
            {
                failStreak_ = 0;
                return;
            }
            ++stats_.cmdFailures;
            if (++failStreak_ >= cfg_.failLimit)
            {
                failStreak_ = 0;
                suspendUntil_ = nowMs + cfg_.failBackoffMs;
                ++stats_.suspends;
            }
        }

        float Gain() const { return gain_; }
        const ApplierStats& Stats() const { return stats_; }
        bool Unfinished() const { return unfinished_; }

    private:
        struct Pending { bool on = false; int before = 0; float damage = 0.0f; };

        Cfg cfg_;
        float gain_;
        bool seen_ = false, unfinished_ = false;
        proto::VehicleDamage lastServer_;
        double lastReadMs_ = -1e9, verifyAtMs_ = 0.0, suspendUntil_ = 0.0;
        int failStreak_ = 0;
        int rounds_[kDentProbes] = {};
        int partRounds_[8] = {};
        int petrolRounds_ = 0;
        bool converged_[kDentProbes] = {}, gaveUp_[kDentProbes] = {};
        Pending pending_[kDentProbes];
        uint8_t appliedDoors_ = 0, appliedLamps_ = 0, appliedDent_[kDentProbes] = {};
        float appliedPetrol_ = 1000.0f;
        ApplierStats stats_;
    };

    // ---- the orchestration the engine runs per car (templates over the game, so a test drives the SAME code) -----------------
    //
    // `Game` is one entity: bool PetrolHealth(float&), bool BodyHealth(float&), bool DoorValid(int, bool&), bool DoorBroken(int, bool&),
    // bool LampBroken(int lamp, bool&), bool Dent(int probe, float& metres) (reads: false = the native could not be called);
    // bool BreakDoor(int), bool SetPetrol(float), bool ReplayLamp(int lamp, float damage), bool ReplayDent(int probe, float damage),
    // void BeginReplay() / void EndReplay() (the copy may be damaged for the length of a replay).

    template <class Game>
    inline void ReadDamage(Game& g, DamageRaw& raw, bool withDents)
    {
        raw = DamageRaw();
        float f = 0.0f;
        if (g.BodyHealth(f)) { raw.bodyOk = true; raw.body = f; }
        if (g.PetrolHealth(f)) { raw.petrolOk = true; raw.petrol = f; }
        for (int d = 0; d < 6; ++d)
        {
            bool valid = true, broken = false;
            if (!g.DoorValid(d, valid) || !g.DoorBroken(d, broken))
            {
                if (d == 0) break;                                   // the group's natives are missing: leave it alone
                continue;
            }
            raw.doorsOk = true;
            if (valid && broken) raw.doors = (uint8_t)(raw.doors | (1u << d));
        }
        for (int l = 0; l < 2; ++l)
        {
            bool broken = false;
            if (!g.LampBroken(l, broken))
            {
                if (l == 0) break;
                continue;
            }
            raw.lampsOk = true;
            if (broken) raw.lamps = (uint8_t)(raw.lamps | (1u << l));
        }
        if (!withDents) return;
        for (int i = 0; i < kDentProbes; ++i)
        {
            float m = 0.0f;
            if (!g.Dent(i, m))
            {
                if (i == 0) break;
                continue;
            }
            raw.dentOk = true;
            raw.dent[i] = DentToWire(m);
        }
    }

    template <class Game>
    inline DamageCmd ExecuteDamage(Game& g, const DamageCmd& cmd)
    {
        DamageCmd done;
        for (int d = 0; d < 6; ++d)
        {
            if (!(cmd.breakDoors & (1u << d))) continue;
            bool valid = true;
            if (g.DoorValid(d, valid) && !valid)
            {
                done.breakDoors = (uint8_t)(done.breakDoors | (1u << d));          // this model has no such door: nothing to break
                continue;
            }
            if (g.BreakDoor(d)) done.breakDoors = (uint8_t)(done.breakDoors | (1u << d));
        }
        if (cmd.petrol && g.SetPetrol(cmd.petrolTo)) done.petrol = true;
        if (cmd.Replays())
        {
            g.BeginReplay();
            for (int l = 0; l < 2; ++l)
                if ((cmd.lamps & (1u << l)) && g.ReplayLamp(l, cmd.lampDamage[l])) done.lamps = (uint8_t)(done.lamps | (1u << l));
            for (int i = 0; i < kDentProbes; ++i)
                if ((cmd.dents & (1u << i)) && g.ReplayDent(i, cmd.dentDamage[i])) done.dents = (uint8_t)(done.dents | (1u << i));
            g.EndReplay();
        }
        return done;
    }

    // One copy, one frame: when due, read, decide, call the natives, tell the module what worked.
    template <class Game>
    inline DamageCmd StepDamageCopy(DamageApplier& da, Game& g, const proto::VehicleDamage& server, bool isNear, double nowMs, bool* ran = nullptr,
                                    DamageRaw* rawOut = nullptr)
    {
        if (ran) *ran = false;
        if (!da.Due(nowMs, server, isNear)) return DamageCmd();
        if (ran) *ran = true;
        DamageRaw raw;
        ReadDamage(g, raw, true);
        if (rawOut) *rawOut = raw;
        const DamageCmd cmd = da.Update(raw, server, nowMs);
        const DamageCmd done = cmd.Any() ? ExecuteDamage(g, cmd) : DamageCmd();
        da.Commit(cmd, done, nowMs);
        return cmd;
    }

    // The controller's report: one read of the car, run through the reporter. `withDents` false skips the 8 deformation reads.
    template <class Game>
    inline proto::VehicleDamage StepDamageController(DamageReporter& rep, Game& g, const proto::VehicleDamage& server, double nowMs,
                                                     DamageReporter::Flagged* flagged = nullptr, bool withDents = true,
                                                     DamageRaw* rawOut = nullptr)
    {
        DamageRaw raw;
        ReadDamage(g, raw, withDents);
        if (rawOut) *rawOut = raw;
        return rep.Update(raw, server, nowMs, flagged);
    }

    // ---- health the controller adopts from the server --------------------------------------------------------------------
    // A vehicle Hit lowers the SERVER's body / engine health; the controller's own car took no bullet, so it would keep its full health
    // (and show no smoke) while every copy shows the damage. True when the controller should write the server's lower value.
    inline bool ShouldAdoptServerHealth(float serverHealth, float localHealth, float tolerance = 25.0f)
    {
        return std::isfinite(serverHealth) && std::isfinite(localHealth) && serverHealth + tolerance < localHealth;
    }

}
