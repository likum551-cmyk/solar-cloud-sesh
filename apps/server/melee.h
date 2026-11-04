// Server-side melee adjudication (desync round 3, T6). Pure logic over the data it is fed (no sockets, no clocks of
// its own), header-only like anticheat.h and damage.h.
//
// Why. The client used to be the only source of a melee hit: it read GTA's "damaged by entity" event (or the puppet's
// health pool) of the remote player's puppet and sent a Hit. Live (0.4.7, one log): about 25 swings counted, 4 Hits
// sent, 0 by the damage event, 1 by the pool drop, 0 damage ever confirmed by the server. Nothing on the receiving
// side of a punch depends on the game's own hit detection for a REAL player (the server owns health), so the melee
// hit must not either. The server already holds everything a punch needs: the swing (PedState.shotSeq steps, and
// kPfMelee), the weapon, and both players' tracks. It decides the hit itself:
//
//   swing seen (a state whose shotSeq stepped with a melee weapon in hand, or the rising kPfMelee)
//     -> the blow lands kStrikeDelayMs after the press: judged then, from the stored tracks:
//        the attacker's pose at the blow, the victim's pose at the blow minus kRenderDelayMs (the attacker sees the
//        others that far in the past), the nearest living player on foot within the weapon's reach AND inside a facing
//        cone of the attacker's heading, one hit per swing, a minimum gap between swings, the same damage ceilings,
//        budget, scale and message flow as ever (Server::FinishHit).
//
// A client-reported Hit (the game's own damage event) stays as an additional input and is never counted twice:
// a claim for the swing and target the server already hit is a confirmation (no damage); a claim that arrives first
// pays, and the judgement of that swing is then skipped; a claim the server's tracks contradict is accepted on the
// game's word and counted (its margins are logged so reach and cone can be tuned from one log).
//
// Cheating surface: a client can only cause a hit its own reported position, heading and swing counter could have
// caused; reach and facing are checked on the server's tracks; a spamming client is held to one judged swing per
// kMinSwingGapMs and to the melee damage budget (300 per second of the unscaled units) and hitsPerSecond.
//
// Thread-safety: none (the server's Poll thread). Errors: none throw. Tests: tests/r3_t6_tests.h.
#pragma once

#include "anticheat.h"
#include "protocol/protocol.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gage::server::melee {

    // ---- tunables (ESTIMATES: nothing here was measured in a game; the `melee:` log line reports the margins) ----------
    constexpr double kStrikeDelayMs = 220.0;   // the button press to the blow landing (a punch is about a quarter second in)
    constexpr double kStrikeLateMs = 90.0;     // judge this long after the blow's time even when the attacker's sample for it is missing
    constexpr double kRenderDelayMs = 100.0;   // the attacker sees other players this far in the past (live: shared delay 84 ms + a frame)
    constexpr double kMinSwingGapMs = 250.0;   // the client's own swing gate is 300 ms; closer swings are one spammed swing
    constexpr double kSwingDedupeMs = 400.0;   // a counter step and a kPfMelee edge within this are one swing
    constexpr double kResultKeepMs = 4000.0;   // how long the judgement of a swing is kept for a Hit that names it
    constexpr double kTrackGapMs = 250.0;      // two samples further apart than this are not interpolated across
    constexpr float  kConeHalfDeg = 65.0f;     // the facing cone (half angle) beyond kCloseM
    constexpr float  kCloseM = 0.9f;           // inside this the bodies overlap: facing is not required
    constexpr float  kMaxDz = 1.6f;            // vertical separation that is no longer the same floor
    constexpr float  kSlackM = 0.5f;           // position quantisation, interpolation and the attacker's step forward
    constexpr size_t kTrackCap = 48;           // samples kept per player (about 1.5 s at the 30 Hz state rate)
    constexpr size_t kMaxPending = 4;          // swings waiting for their blow time, per attacker

    // ---- one stored sample -------------------------------------------------------------------------------------------
    struct Sample
    {
        double ms = 0;                 // the sample time on the server's clock (the sender's stamp placed by SampleClock)
        float x = 0, y = 0, z = 0;
        float heading = 0;             // degrees, GTA's: forward = (-sin h, cos h)
        bool inVehicle = false;
    };

    // A player's recent samples, newest last. A sample that is not newer than the newest is ignored.
    class Track
    {
    public:
        void Clear() { n_ = 0; }
        void Push(const Sample& s)
        {
            if (n_ && s.ms <= buf_[(head_ + n_ - 1) % kTrackCap].ms) return;
            if (n_ < kTrackCap)
                buf_[(head_ + n_++) % kTrackCap] = s;
            else
            {
                buf_[head_] = s;
                head_ = (head_ + 1) % kTrackCap;
            }
        }
        bool Empty() const { return n_ == 0; }
        size_t Size() const { return n_; }
        // R4 T3: how far `ms` is from the nearest stored sample (ms): 0 inside a dense run, large when the pose at `ms` was
        // extrapolated from an old sample (a stalled or alt-tabbed player). -1 when empty. The per-swing log line prints it.
        double GapMs(double ms) const
        {
            if (!n_) return -1.0;
            double best = 1e18;
            for (size_t i = 0; i < n_; ++i)
            {
                const double d = std::fabs(At_(i).ms - ms);
                if (d < best) best = d;
            }
            return best;
        }
        double NewestMs() const { return n_ ? At_(n_ - 1).ms : 0.0; }
        // The pose at `ms`: interpolated between the two samples around it, the nearest one when they are further
        // apart than kTrackGapMs, the oldest / newest outside the stored range. False when empty.
        bool At(double ms, Sample& out) const
        {
            if (!n_) return false;
            if (ms <= At_(0).ms)
            {
                out = At_(0);
                return true;
            }
            if (ms >= At_(n_ - 1).ms)
            {
                out = At_(n_ - 1);
                return true;
            }
            for (size_t i = n_ - 1; i > 0; --i)
            {
                const Sample& a = At_(i - 1);
                const Sample& b = At_(i);
                if (ms < a.ms) continue;
                const double span = b.ms - a.ms;
                if (span > kTrackGapMs)
                {
                    out = (ms - a.ms <= b.ms - ms) ? a : b;
                    return true;
                }
                const float t = span > 0 ? (float)((ms - a.ms) / span) : 0.0f;
                out.ms = ms;
                out.x = a.x + (b.x - a.x) * t;
                out.y = a.y + (b.y - a.y) * t;
                out.z = a.z + (b.z - a.z) * t;
                float dh = std::fmod(b.heading - a.heading, 360.0f);   // the short way round
                if (dh > 180.0f) dh -= 360.0f;
                if (dh < -180.0f) dh += 360.0f;
                out.heading = a.heading + dh * t;
                out.inVehicle = t < 0.5f ? a.inVehicle : b.inVehicle;
                return true;
            }
            out = At_(0);
            return true;
        }

    private:
        const Sample& At_(size_t i) const { return buf_[(head_ + i) % kTrackCap]; }
        std::array<Sample, kTrackCap> buf_{};
        size_t head_ = 0, n_ = 0;
    };

    // ---- reach by weapon class ---------------------------------------------------------------------------------------
    struct Rules
    {
        float reachM = 1.4f;           // centre to centre, before kSlackM
        float coneHalfDeg = kConeHalfDeg;
    };
    // Fists and knuckles 1.4 m, light weapons (knife, stick, bottle, tool) 1.7 m, heavy ones (bat, axe, club) 2.1 m, scaled by
    // Config::meleeReachPercent. The class is the melee ceiling table's (anticheat.h MeleeNominalMax: 40 / 60 / 80).
    inline Rules RulesFor(uint32_t weapon, uint32_t reachPercent = 100)
    {
        const uint16_t nominal = ac::MeleeNominalMax(weapon, 100);
        Rules r;
        r.reachM = nominal <= 40 ? 1.4f : nominal <= 60 ? 1.7f : 2.1f;
        r.reachM = r.reachM * (float)reachPercent / 100.0f;
        return r;
    }

    // ---- the judgement -----------------------------------------------------------------------------------------------
    enum class Outcome : uint8_t
    {
        Hit,               // a target found: damage applies
        NoTarget,          // nobody on foot and alive within reach
        Behind,            // somebody within reach, but outside the facing cone
        TargetDead,        // only a dead player within reach
        TargetInVehicle,   // only a player in a vehicle within reach
        AttackerOut,       // the attacker is dead, untrusted, in a vehicle or ragdolling at the blow
        Budget,            // the damage budget / hit rate refused the damage of an otherwise valid hit
        ClaimedFirst,      // a client-reported Hit already paid for this swing and target
        Count
    };
    constexpr size_t kOutcomes = (size_t)Outcome::Count;
    inline const char* OutcomeName(Outcome o)
    {
        switch (o)
        {
        case Outcome::Hit:             return "hit";
        case Outcome::NoTarget:        return "no_target";
        case Outcome::Behind:          return "behind";
        case Outcome::TargetDead:      return "target_dead";
        case Outcome::TargetInVehicle: return "target_in_vehicle";
        case Outcome::AttackerOut:     return "attacker_out";
        case Outcome::Budget:          return "budget";
        case Outcome::ClaimedFirst:    return "claimed_first";
        case Outcome::Count:           break;
        }
        return "?";
    }

    struct Candidate
    {
        uint16_t id = 0;
        const Track* track = nullptr;
        bool dead = false;             // the server has the player dead
    };

    struct Verdict
    {
        Outcome outcome = Outcome::NoTarget;
        uint16_t target = proto::kNoPlayer;
        float distM = -1;              // to the chosen target, or to the nearest living one on foot when none was chosen (-1: nobody near)
        float angleDeg = -1;           // the same player's angle off the attacker's heading
        float reachM = 0;              // the reach that applied (with the slack)
        // R4 T3: the data a miss needs to explain itself (the per-swing log line). All filled by Adjudicate.
        uint16_t nearestId = proto::kNoPlayer;   // the nearest living player on foot (the one distM / angleDeg describe), whatever the reach
        float nearestDz = 0;           // its vertical separation from the attacker
        double attackerGapMs = -1;     // the attacker's pose at the blow came from a sample this far from the blow time (-1: no pose)
        double targetGapMs = -1;       // the same for the nearest player's pose at blow - render delay
        float attackerX = 0, attackerY = 0, attackerHeading = 0;
        float nearestX = 0, nearestY = 0;
        uint32_t considered = 0;       // candidates with a track
        uint32_t verticalOut = 0;      // ... of which further than kMaxDz above or below the attacker (another floor)
    };

    // Which player a blow at `blowMs` hit. `attacker` must hold samples; a candidate without any is skipped.
    inline Verdict Adjudicate(const Track& attacker, double blowMs, uint32_t weapon, const Candidate* cands, size_t n,
                              uint32_t reachPercent = 100, double renderDelayMs = kRenderDelayMs)
    {
        Verdict v;
        Sample a;
        const Rules rules = RulesFor(weapon, reachPercent);
        v.reachM = rules.reachM + kSlackM;
        if (!attacker.At(blowMs, a))
            return v;
        v.attackerGapMs = attacker.GapMs(blowMs);
        v.attackerX = a.x; v.attackerY = a.y; v.attackerHeading = a.heading;
        if (a.inVehicle)
        {
            v.outcome = Outcome::AttackerOut;
            return v;
        }
        const float hr = a.heading * 3.14159265f / 180.0f;
        const float fx = -std::sin(hr), fy = std::cos(hr);
        bool sawBehind = false, sawDead = false, sawVehicle = false;
        float bestScore = 1e9f, nearestM = -1.0f, nearestAngle = -1.0f;
        for (size_t i = 0; i < n; ++i)
        {
            const Candidate& c = cands[i];
            Sample t;
            if (!c.track || !c.track->At(blowMs - renderDelayMs, t))
                continue;
            ++v.considered;
            const float dx = t.x - a.x, dy = t.y - a.y, dz = t.z - a.z;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (std::fabs(dz) > kMaxDz)
            {
                ++v.verticalOut;
                continue;
            }
            float angle = 0.0f;
            if (d > 1e-3f)
            {
                float cs = (fx * dx + fy * dy) / d;
                cs = cs > 1.0f ? 1.0f : cs < -1.0f ? -1.0f : cs;
                angle = std::acos(cs) * 180.0f / 3.14159265f;
            }
            if (!c.dead && !t.inVehicle && (nearestM < 0 || d < nearestM))
            {
                nearestM = d;
                nearestAngle = angle;
                v.nearestId = c.id;
                v.nearestDz = dz;
                v.nearestX = t.x; v.nearestY = t.y;
                v.targetGapMs = c.track->GapMs(blowMs - renderDelayMs);
            }
            if (d > v.reachM)
                continue;
            if (c.dead)
            {
                sawDead = true;
                continue;
            }
            if (t.inVehicle)
            {
                sawVehicle = true;
                continue;
            }
            if (d > kCloseM && angle > rules.coneHalfDeg)
            {
                sawBehind = true;
                continue;
            }
            const float score = d * (1.0f + angle / 180.0f);   // nearer first, the one in front of the fist before the one at the edge
            if (score < bestScore)
            {
                bestScore = score;
                v.outcome = Outcome::Hit;
                v.target = c.id;
                v.distM = d;
                v.angleDeg = angle;
            }
        }
        if (v.outcome == Outcome::Hit)
            return v;
        v.distM = nearestM;
        v.angleDeg = nearestAngle;
        v.outcome = sawBehind ? Outcome::Behind : sawDead ? Outcome::TargetDead : sawVehicle ? Outcome::TargetInVehicle : Outcome::NoTarget;
        return v;
    }

    // ---- one attacker's swings -----------------------------------------------------------------------------------------
    //
    // Observe() is fed every accepted state of the player; Take() hands out the swings whose blow time has come; Record() keeps the
    // judgement by the swing's 4-bit counter value so a Hit that names it can be matched (a confirmation, or a claim the server
    // contradicts).
    class SwingBook
    {
    public:
        static constexpr int16_t kNone = -1;
        struct Pending
        {
            uint8_t seq = 0;
            double pressMs = 0;
            uint32_t weapon = 0;
            bool byTimer = false;      // R4 T3: judged because the clock passed the blow time, not because the attacker's samples reached it
        };
        struct Result
        {
            bool valid = false;
            bool judged = false;           // false = queued, waiting for the blow time
            double pressMs = 0;
            Outcome outcome = Outcome::NoTarget;
            int32_t target = kNone;        // the player the server hit
            int32_t claimTarget = kNone;   // the player a client-reported Hit paid for (before or after the judgement)
            float distM = -1, angleDeg = -1;
        };
        struct Observed
        {
            uint32_t seen = 0;             // swings the state showed
            uint32_t queued = 0;           // ... that will be judged
            uint32_t tooClose = 0;         // ... dropped: closer than kMinSwingGapMs to the previous one
        };

        void Reset() { *this = SwingBook(); }

        // A state arrived: `steps` shotSeq increments since the previous one (0..15), `flagEdge` the rising kPfMelee,
        // `meleeWeapon` the state's weapon is a melee-class one, `seq` the state's counter value, `weapon` its weapon.
        // Without a melee weapon nothing is a swing.
        Observed Observe(double sampleMs, int steps, bool flagEdge, bool meleeWeapon, uint8_t seq, uint32_t weapon)
        {
            Observed o;
            if (!meleeWeapon)
                return o;
            const bool step = steps > 0;
            if (!step && !flagEdge)
                return o;
            if (step)
            {
                o.seen = (uint32_t)steps;
                lastStepMs_ = sampleMs;
            }
            else
            {
                // A kPfMelee edge with no counter step: one swing, unless a step already told of it (either order).
                if (sampleMs - lastStepMs_ < kSwingDedupeMs)
                    return o;
                o.seen = 1;
            }
            if (haveSwing_ && sampleMs - lastSwingMs_ < kMinSwingGapMs)
            {
                o.tooClose = o.seen;
                return o;
            }
            if (nPending_ >= kMaxPending)
            {
                o.tooClose = o.seen;
                return o;
            }
            haveSwing_ = true;
            lastSwingMs_ = sampleMs;
            pending_[nPending_++] = Pending{seq, sampleMs, weapon};
            Result& r = results_[seq & 15];
            r = Result();
            r.valid = true;
            r.pressMs = sampleMs;
            o.queued = 1;
            o.tooClose = o.seen > 1 ? o.seen - 1 : 0;   // several steps in one state: one swing, the rest bunched by lost states
            return o;
        }

        // The oldest pending swing whose blow is due: the attacker's newest sample reaches the blow time, or the blow time plus
        // kStrikeLateMs has passed on the server's clock. False when none.
        bool Take(double nowMs, double attackerNewestMs, Pending& out)
        {
            if (!nPending_)
                return false;
            const Pending& p = pending_[0];
            const double blow = p.pressMs + kStrikeDelayMs;
            if (attackerNewestMs < blow && nowMs < blow + kStrikeLateMs)
                return false;
            out = p;
            out.byTimer = attackerNewestMs < blow;
            for (size_t i = 1; i < nPending_; ++i)
                pending_[i - 1] = pending_[i];
            --nPending_;
            return true;
        }
        size_t PendingCount() const { return nPending_; }

        // The judgement of the swing `p`.
        void Record(const Pending& p, const Verdict& v)
        {
            Result& r = results_[p.seq & 15];
            if (!r.valid || r.pressMs != p.pressMs)
                return;                    // the slot was reused by a newer swing
            r.judged = true;
            r.outcome = v.outcome;
            r.distM = v.distM;
            r.angleDeg = v.angleDeg;
            if (v.outcome == Outcome::Hit)
                r.target = v.target;
        }
        // The result of the swing with counter value `seq`, when it is recent (within kResultKeepMs of `nowMs`).
        Result* Find(uint8_t seq, double nowMs)
        {
            Result& r = results_[seq & 15];
            return r.valid && nowMs >= r.pressMs && nowMs - r.pressMs <= kResultKeepMs ? &r : nullptr;
        }
        // Whether the swing `p` was already paid for by a client-reported Hit on any target (so the judgement skips it).
        bool ClaimedBefore(const Pending& p) const
        {
            const Result& r = results_[p.seq & 15];
            return r.valid && r.pressMs == p.pressMs && r.claimTarget != kNone;
        }

    private:
        Pending pending_[kMaxPending];
        size_t nPending_ = 0;
        Result results_[16];
        bool haveSwing_ = false;
        double lastSwingMs_ = 0, lastStepMs_ = -1e9;
    };

    // ---- what a client-reported melee Hit means to the book -------------------------------------------------------------
    enum class ClaimKind : uint8_t
    {
        Unknown,        // the book has no swing for it (an old swing, adjudication off for it): the normal rules decide
        Confirms,       // the server already hit this target with this swing: no damage, no second count
        BeforeJudging,  // the swing is still waiting for its blow time: the claim pays, the judgement will skip it
        OtherTarget,    // the server hit somebody else with this swing: the normal rules decide (a second target)
        Contradicts,    // the server judged the swing a miss; the normal rules decide (the game's damage is the stronger evidence)
    };
    inline ClaimKind ClassifyClaim(SwingBook& book, uint8_t seq, uint16_t target, double nowMs)
    {
        SwingBook::Result* r = book.Find(seq, nowMs);
        if (!r)
            return ClaimKind::Unknown;
        if (!r->judged)
            return ClaimKind::BeforeJudging;
        if (r->outcome == Outcome::Hit)
            return r->target == (int32_t)target ? ClaimKind::Confirms : ClaimKind::OtherTarget;
        return ClaimKind::Contradicts;
    }

    // ---- the per-swing log line (R4 T3) --------------------------------------------------------------------------------------
    //
    // Live 0.4.9: GI swung 11 times, the server confirmed 0 blows, and the log could not say why (only hits and misses within 3 m were
    // logged, 40 lines in all). One line per judged swing now, with the reason and the numbers the reason rests on: distance, angle, the
    // target id, the time window (how late the swing was judged and from how far a sample each pose was taken) and how many players were
    // left out before the judgement (no state, beyond the 8 m pre-filter, not relevant). The first part keeps the R3 wording
    // ("judged hit on id N: nearest living player on foot X m (reach R m), A degrees off the heading"): the tests and the tuning notes grep it.
    struct SwingLogCtx
    {
        const char* name = "";
        uint8_t seq = 0;
        uint32_t weapon = 0;
        double pressMs = 0, nowMs = 0;
        bool byTimer = false;                 // judged by the clock (+90 ms after the blow time), not by the attacker's samples
        const char* attackerWhy = "";         // for AttackerOut: dead / untrusted / ragdoll ... / in a vehicle
        uint32_t others = 0, noState = 0, beyondPrefilter = 0, notRelevant = 0;   // the other players and why they were not candidates
        uint16_t nearestAnyId = proto::kNoPlayer;   // the nearest other player by current position, even one the pre-filter dropped
        float nearestAnyM = -1;
    };
    inline std::string FormatSwingLine(const SwingLogCtx& c, const Verdict& v)
    {
        char buf[1100], w[12];
        std::snprintf(w, sizeof w, "%08X", (unsigned)c.weapon);
        std::string line = std::string("melee: ") + c.name + " swing (shotSeq " + std::to_string((int)c.seq) + ", weapon 0x" + w + ") judged " + OutcomeName(v.outcome) +
                           (v.target != proto::kNoPlayer && v.outcome == Outcome::Hit ? " on id " + std::to_string(v.target) : std::string());
        if (v.distM < 0.0f)
            std::snprintf(buf, sizeof buf, ": no living player on foot with a pose (reach %.2f m)", (double)v.reachM);
        else
            std::snprintf(buf, sizeof buf, ": nearest living player on foot %.2f m (reach %.2f m), %.1f degrees off the heading", (double)v.distM, (double)v.reachM, (double)v.angleDeg);
        line += buf;
        // why
        switch (v.outcome)
        {
        case Outcome::Hit:
            std::snprintf(buf, sizeof buf, "; why: id %u inside the reach and the cone", (unsigned)v.target);
            break;
        case Outcome::NoTarget:
            if (v.nearestId != proto::kNoPlayer)
                std::snprintf(buf, sizeof buf, "; why: id %u is the nearest and stands %.2f m away, %.2f m beyond the reach (dz %.1f)", (unsigned)v.nearestId,
                              (double)v.distM, (double)(v.distM - v.reachM), (double)v.nearestDz);
            else
                std::snprintf(buf, sizeof buf, "; why: no living player on foot had a pose to judge against (considered %u of %u others: no state %u, beyond 8 m %u, "
                              "not relevant %u, another floor %u)", v.considered, c.others, c.noState, c.beyondPrefilter, c.notRelevant, v.verticalOut);
            break;
        case Outcome::Behind:
            std::snprintf(buf, sizeof buf, "; why: id %u at %.2f m is %.1f degrees off the heading, outside the facing cone", (unsigned)v.nearestId, (double)v.distM, (double)v.angleDeg);
            break;
        case Outcome::TargetDead:        std::snprintf(buf, sizeof buf, "; why: only a dead player was within the reach"); break;
        case Outcome::TargetInVehicle:   std::snprintf(buf, sizeof buf, "; why: only a player in a vehicle was within the reach"); break;
        case Outcome::AttackerOut:       std::snprintf(buf, sizeof buf, "; why: the attacker is out (%s)", c.attackerWhy); break;
        case Outcome::Budget:            std::snprintf(buf, sizeof buf, "; why: the hit rate or the damage budget refused an otherwise valid hit"); break;
        case Outcome::ClaimedFirst:      std::snprintf(buf, sizeof buf, "; why: a client-reported Hit already paid for this swing"); break;
        default:                         buf[0] = 0; break;
        }
        line += buf;
        // the time window and the geometry
        std::snprintf(buf, sizeof buf,
                      "; timing: judged %.0f ms after the blow time (press + %.0f ms), %s; attacker pose from a sample %.0f ms off the blow, target pose (blow - %.0f ms) from a sample %.0f ms off; "
                      "attacker at %.1f, %.1f heading %.0f%s",
                      c.nowMs - (c.pressMs + kStrikeDelayMs), kStrikeDelayMs, c.byTimer ? "by the clock (the attacker's samples had not reached it)" : "by the attacker's samples",
                      v.attackerGapMs, kRenderDelayMs, v.targetGapMs, (double)v.attackerX, (double)v.attackerY, (double)v.attackerHeading,
                      v.nearestId != proto::kNoPlayer ? "" : "; ");
        line += buf;
        if (v.nearestId != proto::kNoPlayer)
        {
            std::snprintf(buf, sizeof buf, ", id %u at %.1f, %.1f; ", (unsigned)v.nearestId, (double)v.nearestX, (double)v.nearestY);
            line += buf;
        }
        std::snprintf(buf, sizeof buf, "%u other players, nearest by position id %d at %.2f m", c.others, c.nearestAnyId == proto::kNoPlayer ? -1 : (int)c.nearestAnyId, (double)c.nearestAnyM);
        line += buf;
        return line;
    }

}
