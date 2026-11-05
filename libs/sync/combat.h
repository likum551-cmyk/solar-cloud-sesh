// Combat decisions for the in-game engine (protocol v4: the SERVER owns health and armour).
// Pure C++17, no game or OS headers, single-threaded (the engine's script tick). Tested by
// tests/gameplay_test.cpp. Parts (the last block, "sync plan W3", is the cosmetic combat client: weapon
// show cache, shot debt, hit reactions, melee swing gate):
//
//   VitalsTracker  the local player's health and armour against the server's values. GTA still
//                  hurts the local ped (falls, fire, explosions, a car): any loss the server did
//                  not cause is reported as an ENVIRONMENTAL self-hit (Hit with targetId = own id,
//                  protocol section 4.20), and the server's answer (Damage) is put back on the ped.
//                  A gain the server did not grant (GTA's regeneration, /heal) is undone.
//   RespawnGate    when to ask the server to bring us back (Respawn): not before the server's
//                  respawnDelaySeconds (5) after the death, then about once a second until
//                  RespawnOk, and a give-up deadline so a lost answer cannot leave the player dead.
//   HitBatcher     every Hit leaves through here: hits on the same target inside a short window are
//                  merged (a shotgun's pellets, a minigun's frames), damage is split at the wire's
//                  cap (kMaxHitDamage 250), and a token bucket keeps the client under the server's
//                  hitsPerSecond (30), whose excess the server would drop silently.
//   ProbePeerDamage  a remote player's ped is held at a large local health pool; any drop is damage
//                  dealt on THIS machine, reported only when the local player (or its vehicle) did it.
//
// Units: "GTA health" is entity health (100 = the death threshold; a freemode ped's max is 200).
// "Server health" is the wire's 0..255, GTA health minus 100 (protocol section 4.6).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gage::sync {

    // ---- R3 T1: the health scale (DIRECT or SCALED) ------------------------------------------------------------
    //
    // Live 0.4.7: Welcome.playerHealth 200 -> the ped's maximum 300, GET_ENTITY_MAX_HEALTH read 300, yet the player
    // ped's health never rose above 200 (bar 50 %, the reconciler gave up). What GTA does with a health above 200 is
    // therefore MEASURED at the start (HealthController) and the engine runs in one of two modes:
    //   DIRECT  GTA holds 100 + playerHealth: the ped's health IS the server's number plus 100 (the 0.4.7 behaviour).
    //   SCALED  GTA holds only 100 + ceiling (ceiling < playerHealth): the server's number stays the truth and the ped
    //           carries a PROPORTIONAL value in 101..100+ceiling, never above the ceiling (so it never reads as a
    //           mismatch). Damage the tracker sees in GTA units is scaled back by playerHealth / ceiling.
    enum class HealthMode : uint8_t { Auto, Direct, Scaled };

    inline HealthMode ParseHealthMode(std::string_view s)
    {
        auto eq = [&](const char* w) {
            size_t i = 0;
            for (; w[i]; ++i)
                if (i >= s.size() || (s[i] | 0x20) != w[i])
                    return false;
            return i == s.size();
        };
        return eq("direct") ? HealthMode::Direct : eq("scaled") ? HealthMode::Scaled : HealthMode::Auto;
    }
    inline const char* HealthModeName(HealthMode m)
    {
        return m == HealthMode::Direct ? "direct" : m == HealthMode::Scaled ? "scaled" : "auto";
    }

    struct HealthScale
    {
        int ph = 100;       // Welcome.playerHealth (server units)
        int ceiling = 0;    // GTA health above 100 the ped can hold; 0 (unknown) or >= ph means DIRECT

        bool Scaled() const { return ceiling > 0 && ph > 0 && ceiling < ph; }
        int  Span() const { return Scaled() ? ceiling : ph; }     // GTA units that stand for the server's 1..ph
        int  GtaMax() const { return 100 + Span(); }               // the maximum the ped is given
        // The GTA health above 100 for a server value: 0 only for 0, else at least 1 (a living player never sits on
        // GTA's death line) and at most the ceiling.
        int ToGta(int serverHp) const
        {
            if (serverHp <= 0)
                return 0;
            if (!Scaled())
                return serverHp;
            const int g = (int)(((long long)serverHp * ceiling + ph / 2) / ph);
            return g < 1 ? 1 : (g > ceiling ? ceiling : g);
        }
        // A GTA health above 100 as a server value (nearest).
        int ToServer(int gta) const
        {
            if (gta <= 0)
                return 0;
            if (!Scaled())
                return gta;
            const long long v = ((long long)gta * ph + ceiling / 2) / ceiling;
            return v > ph ? ph : (int)v;
        }
        // GTA health lost -> server health lost (nearest, at least 1 for any loss).
        int LossToServer(int gtaLoss) const
        {
            if (gtaLoss <= 0)
                return 0;
            if (!Scaled())
                return gtaLoss;
            const long long v = ((long long)gtaLoss * ph + ceiling / 2) / ceiling;
            return v < 1 ? 1 : (int)v;
        }
    };

    // ---- VitalsTracker ----------------------------------------------------------------------

    struct VitalsConfig
    {
        double ackTimeoutMs = 2000.0; // a self-hit with no Damage back by then is given up: resync
        int    maxHitDamage = 250;    // proto::kMaxHitDamage: one self-hit carries at most this
        int    maxChunks = 4;         // at most this many self-hits per tick (a 1000-point fall = 4 x 250)
        double puppetGraceMs = 1500.0; // after a puppet-caused loss a lethal self-hit waits this long for the attacker's Hit,
                                       // and a loss no Damage followed is undone (the server's value is put back)
        // T6 (absolute reconciliation): a ped whose health sits, steady, off the server's value for this long with
        // nothing pending and no puppet in play gets the server's value put on again (the 0e fix only reacted to an
        // OBSERVED max change; a ped swap that lands max and health in one tick left 200 / 300 for good).
        double reconcileAfterMs = 1000.0;
        double reconcileRetryMs = 3000.0; // between two reconciliations of the same mismatch
        int    reconcileTries = 3;        // a mismatch that survives this many writes is GTA holding the ped there: stop writing
        // R3 T1: a mismatch GTA kept (the give-up above) asks the engine to MEASURE the ped again (the ceiling may differ from
        // what the start measured) at most this many times per ped; the ped handle not yet probed asks once.
        int    reprobes = 3;
    };

    // What the ped shows this tick (the engine reads it with natives).
    struct VitalsObs
    {
        double nowMs = 0;
        int    health = 200;     // GET_ENTITY_HEALTH (GTA scale)
        int    maxHealth = 200;  // GET_ENTITY_MAX_HEALTH; 100 or less (unreadable) counts as 200
        int    armour = 0;       // GET_PED_ARMOUR; negative = unreadable (the tick is skipped)
        bool   dead = false;     // IS_PED_DEAD_OR_DYING
        // A remote puppet ped or vehicle (not ours) caused this tick's loss (HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY
        // on a peer ped or a server vehicle other than ours): its owner's Hit is the authority, so the
        // loss is taken as the new baseline and NOT reported as an environmental self-hit.
        bool   puppetDamage = false;
        // T6: the local ped's entity handle (0 = unknown). A handle that changes between two ticks is a NEW ped
        // (SET_PLAYER_MODEL, the default-ped guard, a respawn that re-created it): GTA gives it its own default
        // health (200), which is not a loss, so the tick takes a baseline and the server's value is put on it.
        int32_t ped = 0;
    };

    struct VitalsOut
    {
        static constexpr int kMaxSelfHits = 8;
        int  selfHits[kMaxSelfHits] = {};  // damage of each environmental self-hit to send now
        int  selfHitCount = 0;
        bool apply = false;                // put setHealth / setArmour on the ped (skip if it is dead)
        int  setHealth = 0;                // GTA scale
        int  setArmour = 0;
        bool kill = false;                 // the server has us dead but the ped lives: kill it
        // R3 T1: measure this ped's health ceiling now (HealthController::Begin) and do nothing else this tick: the ped's
        // handle was never probed, or a mismatch survived the reconciliation (probeWhy names which).
        bool        probe = false;
        const char* probeWhy = "";
    };

    struct VitalsStats
    {
        uint32_t selfHits = 0, selfDamage = 0;     // self-hits handed out and their damage
        uint32_t maxChanges = 0;                    // the ped's max health changed and the server's value was put back on
        uint32_t acks = 0, ackedDamage = 0;        // our own self-hits the server answered
        uint32_t damageEvents = 0, damageTaken = 0; // every Damage for us (any attacker)
        uint32_t resyncs = 0;                      // pending self-damage given up after ackTimeoutMs
        uint32_t clampsDown = 0;                   // a local gain undone (regeneration, /heal)
        uint32_t kills = 0;                        // the ped killed because the server says dead
        uint32_t puppetLosses = 0;                 // ticks whose loss a remote puppet caused (the attacker's Hit reports it)
        uint32_t puppetRestores = 0;               // ... of which no Damage followed in puppetGraceMs: the server's value put back
        uint32_t skippedReads = 0;                 // ticks skipped for an unreadable health or armour
        // T6 diagnostics
        uint32_t pedSwaps = 0;                     // the ped handle changed (a model swap): baseline + the server's value, no self-hit
        uint32_t maxUnreadable = 0;                // ticks whose GET_ENTITY_MAX_HEALTH read 100 or less (0 = the native failed)
        uint32_t reconciles = 0;                   // steady mismatches with the server's value corrected (absolute check)
        uint32_t reconcileGaveUp = 0;              // ... mismatches GTA kept (the write did not take reconcileTries times)
        uint32_t largestSelfHit = 0;               // the biggest single environmental self-hit handed out
        uint32_t maxWrites = 0, maxWriteFails = 0, maxReadBackOff = 0; // SET_ENTITY_MAX_HEALTH calls, failed calls, reads that differ from the wish
        int      lastMaxWanted = 0, lastMaxReadBack = 0;
        int      obsHealth = 0, obsMax = 0;        // the ped's health and max at the last readable tick
        // R3 T1
        uint32_t probeAsks = 0;                    // ticks that asked for a probe (a ped handle never probed)
        uint32_t reprobes = 0;                     // ... because a mismatch survived the reconciliation
        uint32_t scaleChanges = 0;                 // SetScale calls that changed the mode or the ceiling
    };

    class VitalsTracker
    {
    public:
        explicit VitalsTracker(const VitalsConfig& cfg = VitalsConfig());

        // A fresh session or entry: the server's values (default 100 / 0), nothing pending, and the
        // next Tick puts them on the ped.
        void Reset(uint8_t health = 100, uint8_t armour = 0);
        // A Damage whose victim is us. selfInflicted: its attacker is us too (our environmental hit).
        void OnDamage(uint8_t health, uint8_t armour, uint16_t damage, bool selfInflicted, bool killed);
        // Several Damage events about us folded into one (VitalsInbox): the newest health and armour,
        // `damage` the sum of all of them, `selfAckDamage` the sum of those our own self-hits caused.
        void OnDamageMerged(uint8_t health, uint8_t armour, uint32_t damage, uint32_t selfAckDamage, uint32_t events,
                            bool killed);
        // The engine has just shot a cosmetic flinch bullet (1 HP) at our own ped. It lands the frame after the
        // tracker's last write, where the tick only takes a baseline, so the loss would never be flagged as a
        // puppet loss: put the server's value back after puppetGraceMs unless a Damage arrives first.
        void NotePuppetLoss(double nowMs);
        // A Killed whose victim is us.
        void OnKilled();
        // RespawnOk: alive with these values; applied on the next Tick.
        void OnRespawn(uint8_t health, uint8_t armour);
        // The engine replaced the local ped (default-ped guard swap): the next tick takes a baseline and the
        // server's value goes on the new ped. Also detected by VitalsObs::ped changing; this covers a reused handle.
        void NotePedReplaced();
        // The result of a SET_ENTITY_MAX_HEALTH the engine just made (wanted value, the native's result, and
        // GET_ENTITY_MAX_HEALTH read straight back): counted for the 10 s diagnostics.
        void NoteMaxWrite(int wanted, bool ok, int readBack);
        // R3 T1: the health scale the engine measured for the ped (HealthController): DIRECT or SCALED. A change makes the
        // next tick put the server's value on the ped in the new units, never a loss.
        void SetScale(const HealthScale& s);
        // The engine has just probed this ped handle: no probe is asked for it.
        void NoteProbed(int32_t ped);
        const HealthScale& Scale() const { return scale_; }

        // Once per script tick while the player is in the world (not while respawning).
        VitalsOut Tick(const VitalsObs& o);

        bool ServerDead() const { return serverDead_; }
        uint8_t Health() const { return health_; }
        uint8_t Armour() const { return armour_; }
        int Pending() const { return pending_; }
        const VitalsStats& Stats() const { return stats_; }
        // The newest readable max health of the ped (0 = none yet): the HUD's fallback when a frame reads 0.
        int LastGoodMax() const { return lastGoodMax_; }

    private:
        VitalsConfig cfg_;
        uint8_t health_ = 100, armour_ = 0;
        bool serverDead_ = false;
        bool applyPending_ = true;
        bool rebase_ = true;       // the next Tick only takes a baseline (after our own write)
        int lastLocal_ = 0;        // the ped's total (health above 100 + armour) at the last Tick
        int lastMax_ = 0;          // the ped's max health at the last readable Tick (0 = none yet)
        int lastGoodMax_ = 0;      // T6: the newest max above 100 (a failed read substitutes it, not a blind 200)
        int32_t pedHandle_ = 0;    // T6: the ped handle of the last tick
        HealthScale scale_;        // R3 T1: DIRECT (the default) or SCALED
        int lastHG_ = 0, lastAG_ = 0; // R3 T1: the ped's health above 100 / armour at the last Tick (GTA units; the SCALED loss detection)
        int32_t probedPed_ = 0, askedPed_ = 0;
        int reprobeBudget_ = 3;
        double mismatchSinceMs_ = 0; // T6: when the ped's health first sat steady off the server's value (0 = it does not)
        int mismatchHealth_ = -1;  // ... and what it read then
        int reconcileTries_ = 0;
        double lastReconcileMs_ = -1e9;
        int pending_ = 0;          // self-damage sent and not yet answered
        double pendingSinceMs_ = 0;
        double puppetUntilMs_ = 0; // a lethal self-hit waits until then after a puppet-caused loss
        double puppetApplyAtMs_ = 0; // > 0: the server's value is put back then unless a Damage arrived first
        VitalsStats stats_;
    };

    // ---- RespawnGate -----------------------------------------------------------------------

    struct RespawnConfig
    {
        double firstAfterMs = 5250.0; // the server's respawnDelaySeconds (5) plus a margin
        double retryMs = 1000.0;      // "about once a second is plenty" (session.h)
        double giveUpMs = 30000.0;    // no RespawnOk by then: the engine revives on its own (unless the server says dead)
        double maxRetryMs = 10000.0;  // past giveUpMs the requests back off 1, 2, 4, 8 s ... up to this
    };

    // What the engine does at the give-up deadline.
    enum class GiveUpAction : uint8_t
    {
        Wait,           // not yet
        ReviveLocally,  // the server has not said we are dead: revive at the default spawn (old behaviour)
        KeepAsking,     // the server says dead: never revive locally, keep requesting (backed off)
    };

    class RespawnGate
    {
    public:
        explicit RespawnGate(const RespawnConfig& cfg = RespawnConfig());
        // The death was seen (server Damage/Killed or the ped itself). Starts the clock on the first
        // call while alive; later calls while dead change nothing.
        void OnDeath(double nowMs);
        // RespawnOk (or the engine's own give-up revive): alive again, the clock stops.
        void OnAlive();
        // While dead, every tick: true when a Respawn should go out now.
        bool ShouldSend(double nowMs);
        bool GaveUp(double nowMs) const { return dead_ && nowMs - deathMs_ >= cfg_.giveUpMs; }
        GiveUpAction GiveUp(double nowMs, bool serverDead) const
        {
            if (!GaveUp(nowMs))
                return GiveUpAction::Wait;
            return serverDead ? GiveUpAction::KeepAsking : GiveUpAction::ReviveLocally;
        }
        bool Dead() const { return dead_; }
        int Attempts() const { return attempts_; }
        double DeathMs() const { return deathMs_; }

    private:
        RespawnConfig cfg_;
        bool dead_ = false;
        double deathMs_ = 0, lastSendMs_ = 0;
        int attempts_ = 0;
        int lateSends_ = 0;      // requests sent past the give-up deadline: the backoff step
    };

    // ---- HitBatcher ------------------------------------------------------------------------

    struct BatchedHit
    {
        uint16_t target = 0;   // player id (own id = environmental) or vehicle id
        uint8_t  kind = 1;     // proto::HitKind: 1 player, 2 vehicle
        uint32_t weapon = 0;
        uint16_t damage = 0;
        int16_t  bone = -1;
        uint8_t  shotSeq = 0;
    };

    struct HitBatchConfig
    {
        double holdMs = 50.0;       // a target's first hit waits this long for more to merge
        double ratePerSec = 25.0;   // under the server's hitsPerSecond (30)
        double burst = 25.0;
        int    maxDamage = 250;     // proto::kMaxHitDamage
        size_t maxPending = 64;     // distinct targets waiting; the oldest is dropped past it
    };

    struct HitBatchStats { uint32_t added = 0, merged = 0, sent = 0, dropped = 0, rateHeld = 0; };

    class HitBatcher
    {
    public:
        explicit HitBatcher(const HitBatchConfig& cfg = HitBatchConfig());
        // Queue a hit (damage 0 is ignored). One on a target already waiting adds its damage there
        // (the newest weapon, bone and shotSeq win).
        void Add(const BatchedHit& h, double nowMs);
        // The hits due now (hold expired), within the token bucket; at most `cap`. A merged damage
        // above maxDamage goes out as maxDamage and the rest stays queued.
        size_t Flush(double nowMs, BatchedHit* out, size_t cap);
        size_t Pending() const { return count_; }
        void Clear() { count_ = 0; }
        const HitBatchStats& Stats() const { return stats_; }

    private:
        struct Slot { BatchedHit h; uint32_t damage = 0; double sinceMs = 0; };
        HitBatchConfig cfg_;
        Slot slots_[64];
        size_t count_ = 0;
        double tokens_ = 0, tokensAtMs_ = -1;
        HitBatchStats stats_;
    };

    // ---- remote ped damage probe ----------------------------------------------------------

    struct PeerDamageOut
    {
        int  damage = 0;     // health the ped lost since it was last restored (0 = none)
        bool restore = false; // put the pool back (SET_ENTITY_HEALTH)
        bool report = false;  // damage > 0 and the local player (or its vehicle) did it: send a Hit
        bool respawn = false; // the puppet died on this machine: delete it and spawn a fresh one
    };
    // health: the remote ped's GET_ENTITY_HEALTH now; pool: the health it is held at;
    // damagedByLocal: HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(ped, local ped or its vehicle).
    // A reading at or below 0 is "cannot call" and ignored, UNLESS dead (IS_PED_DEAD_OR_DYING): a
    // puppet the game killed in one blow above the pool (a headshot). Then the whole pool counts as
    // dealt (HitBatcher splits it at the wire cap), the puppet is respawned, never restored.
    // deadMaxDamage (W8 item 7, live test 0.4.2): the most a DEAD puppet's report may carry. The pool-minus-health rule
    // made a puppet that GTA killed with a melee takedown or a running blow (its health set to 0, not a damage
    // number) report ~the whole pool, and the server saw two-punch kills. The engine passes kDeadMeleeCapDamage when
    // the local ped's weapon is a melee class (the server then clamps again by weapon); a gun or an explosive keeps
    // the pool (a real one-blow headshot or rocket). kNoDeadCap = the old behaviour.
    constexpr int kNoDeadCap = 0x7fffffff;
    constexpr int kDeadMeleeCapDamage = 40;   // the fist ceiling of the server (apps/server/anticheat.h MeleeNominalMax)
    PeerDamageOut ProbePeerDamage(int health, int pool, bool damagedByLocal, bool dead = false, int deadMaxDamage = kNoDeadCap);

    // ---- small engine decisions kept here so they are testable ---------------------------------

    // Whether the vitals tracker runs this script tick: while Alive, and while Dying until the server
    // has us dead (the lethal self-hit is resent after ackTimeoutMs on that path only).
    bool VitalsTickDue(bool serverMode, bool lifeAlive, bool lifeDying, bool serverDead);

    // Damage from our own vehicle is attributed to us only when we drive it AND the server has us in
    // seat 0 (a passenger's car hits are the driver's).
    bool VehicleBlameAllowed(int localSeat, bool linkIsDriver);

    // The first handle (a remote peer ped or a server vehicle) that `damagedBy(handle)` says hurt the
    // local ped, skipping `ownVehicle` and 0; 0 when none. The engine passes HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY.
    template <class DamagedByFn>
    int32_t FindPuppetDamager(const int32_t* handles, size_t n, int32_t ownVehicle, DamagedByFn damagedBy)
    {
        for (size_t i = 0; i < n; ++i)
        {
            const int32_t h = handles[i];
            if (h != 0 && h != ownVehicle && damagedBy(h))
                return h;
        }
        return 0;
    }

    // The local state carries an aim point while free-aiming and for holdMs after a shot, on foot AND
    // from a vehicle: without it a drive-by shot is shown firing along the ped's heading on the other
    // machines (PlayPeerShots).
    inline bool AimPointDue(bool freeAiming, double nowMs, double lastShotMs, double holdMs = 500.0)
    {
        return freeAiming || nowMs - lastShotMs < holdMs;
    }

    // ---- sync plan W3: shots, hit reactions, melee (client side; no wire change) ----------------------
    //
    // Model: shooter-reported hits, server-validated, the server owns health and armour. Everything in
    // this section is COSMETIC: a reaction must never feed a Hit and never change server-owned health.

    // The answer of one weapon probe: `callable` says every native the answer needs could be called (an
    // unresolved native reads as "no", which must not be remembered as the weapon's truth).
    struct WeaponProbe
    {
        bool callable = false;
        bool show = false;       // only meaningful when callable
    };

    // Whether a weapon's shots are shown on a remote ped (a bullet weapon, not melee, not explosive). Only
    // CALLABLE answers are cached: the old engine cache remembered a "no" given while IsWeaponValid /
    // WeaponDamage were still uncallable (natives not resolved yet) forever, so a peer's shots were never
    // shown for the whole session. An uncallable probe answers false for now and is asked again next time.
    class WeaponShowCache
    {
    public:
        explicit WeaponShowCache(size_t cap = 256) : cap_(cap) {}
        // `probe(weapon)` returns a WeaponProbe. Weapon 0 is never shown and never probed. Past the cap an
        // answer is returned but not stored (the cache does not grow).
        template <class ProbeFn>
        bool Show(uint32_t weapon, ProbeFn probe)
        {
            if (!weapon)
                return false;
            const auto it = c_.find(weapon);
            if (it != c_.end())
                return it->second;
            const WeaponProbe p = probe(weapon);
            if (!p.callable)
            {
                ++uncallable_;
                return false;
            }
            if (c_.size() < cap_)
                c_[weapon] = p.show;
            return p.show;
        }
        size_t Size() const { return c_.size(); }
        uint32_t Uncallable() const { return uncallable_; }   // probes that could not be answered (not cached)
    private:
        size_t cap_;
        std::map<uint32_t, bool> c_;
        uint32_t uncallable_ = 0;
    };

    // Shots owed to a remote ped. The pose's 4-bit shotSeq can jump by up to 15 between two frames (mid
    // and far relevance deliver bursts); the old code fired at most 3 and DISCARDED the rest. Every shot
    // is owed here (up to maxOwed, the counter's own range) and fired perFrame at a time.
    class ShotDebt
    {
    public:
        explicit ShotDebt(int maxOwed = 15, int perFrame = 3) : max_(maxOwed), per_(perFrame) {}
        void Add(int shots)
        {
            if (shots <= 0)
                return;
            owed_ += shots;
            if (owed_ > max_)
                owed_ = max_;
        }
        // The shots to fire this frame (at most perFrame); they are no longer owed.
        int Take()
        {
            const int n = owed_ < per_ ? owed_ : per_;
            owed_ -= n;
            return n;
        }
        int Owed() const { return owed_; }
        void Clear() { owed_ = 0; }
    private:
        int max_, per_, owed_ = 0;
    };

    // ---- hit reactions ---------------------------------------------------------------------------

    // Stagger (W8): a MELEE blow too light for a knockdown. A short, light ragdoll (the ped stumbles and gets up), never
    // a flinch bullet: a bullet at the chest is the wrong picture for a punch and the live test showed no reaction
    // for punches (each one was a flinch bullet).
    enum class ReactKind : uint8_t { None, Flinch, Knockdown, Stagger };

    struct HitReaction
    {
        ReactKind kind = ReactKind::None;
        int ragdollMs = 0;      // Knockdown / Stagger: how long the ped ragdolls
    };

    // hit_react=off|bullet|ragdoll|both, as numbers.
    constexpr int kHitReactOff = 0, kHitReactBullet = 1, kHitReactRagdoll = 2, kHitReactBoth = 3;
    // What a mode lets a decision show: a knockdown or a stagger ("ragdoll"/"both") as a ragdoll, a flinch bullet
    // ("bullet"/"both"; in "bullet" a knockdown or a stagger is shown as a flinch). Neither = nothing is shown.
    // 0.4.30 staggerClip (stagger_clip, default on): a melee Stagger is a stumble CLIP on the feet (`clip`), never a ragdoll: SET_PED_TO_RAGDOLL
    // type 0 is CTaskNMRelax, so a punch dropped the ped (live 0.4.29 trace: tasks 389 NMRelax -> 16 GetUp). Off = the W8 ragdoll stagger.
    inline void ReactionToShow(int mode, const HitReaction& r, bool& ragdoll, bool& bullet, bool& clip, bool staggerClip)
    {
        const bool body = mode == kHitReactRagdoll || mode == kHitReactBoth;
        const bool stagger = r.kind == ReactKind::Stagger;
        clip = stagger && body && staggerClip;
        ragdoll = !clip && body && (r.kind == ReactKind::Knockdown || stagger);
        bullet = !ragdoll && !clip && r.kind != ReactKind::None && (mode == kHitReactBullet || mode == kHitReactBoth);
    }
    // The W8 rule (no clip): a stagger is a ragdoll.
    inline void ReactionToShow(int mode, const HitReaction& r, bool& ragdoll, bool& bullet)
    {
        bool clip = false;
        ReactionToShow(mode, r, ragdoll, bullet, clip, false);
    }

    // What the reacting ped is doing. sinceKnockdownMs is the time since THIS ped's last Knockdown
    // (HitReactArbiter::SinceKnockdownMs; large = never).
    struct HitReactState
    {
        bool   dead = false;
        bool   inVehicle = false;
        bool   climbing = false;
        double sinceKnockdownMs = 1e9;
    };

    // INFERRED tunables (the sync plan's combat spec section 2); a live session decides the final values.
    struct HitReactConfig
    {
        double flinchMinPct = 3.0;           // a hit of at least this % of max health flinches
        double flinchSpacingMs = 400.0;      // reactions at least this far apart
        double knockdownPct = 20.0;          // at least this % knocks down
        double meleeKnockdownPct = 15.0;     // a melee blow of at least this % knocks down (heavy melee; W8: was 10, a fist at the server's
                                             // ceiling is 10% of 200 and every punch would have been a knockdown)
        double meleeStaggerMinPct = 3.0;     // W8: a lighter melee blow of at least this % staggers
        int    staggerBaseMs = 250;          // stagger ragdoll ms = clamp(base + perDamage * damage, base, max)
        int    staggerMsPerDamage = 5;
        int    staggerMaxMs = 450;
        double knockdownImmunityMs = 1500.0; // after a knockdown only a flinch
        int    ragdollBaseMs = 300;          // ragdoll ms = clamp(base + perDamage * damage, 0, max)
        int    ragdollMsPerDamage = 8;
        int    ragdollMaxMs = 1500;
    };

    struct HitReact
    {
        // The Damage.flags bits of protocol v5 (the same values as proto::kDmgHeadshot ... kDmgArmour).
        static constexpr uint8_t kFlagHeadshot = 1, kFlagMelee = 2, kFlagExplosion = 4, kFlagArmourAbsorbed = 8;
        // What a hit of `damage` (server health units) on a player whose maximum is `maxHp` shows.
        // sinceLastMs = time since the ped's last reaction of any kind (large = none). Never for a dead,
        // seated or climbing ped, for no damage or an unknown maximum. A knockdown inside the immunity
        // window of the previous one is a flinch; a flinch needs flinchSpacingMs since the last reaction.
        // W8: a melee blow (kFlagMelee, no explosion) never flinches: one too light for a knockdown, or a knockdown
        // inside the immunity window, is a Stagger.
        static HitReaction Decide(int damage, int maxHp, uint8_t flags, double sinceLastMs, const HitReactState& state,
                                  const HitReactConfig& cfg = HitReactConfig());
    };

    struct HitReactOut
    {
        bool holds = false;   // a reaction is playing: issue no gait / aim task over it (DrivePeer returns early)
        bool ended = false;   // true once, on the tick the window closes
    };

    // One per remote ped (like EmoteArbiter): the window in which a reaction owns the ped. A flinch holds
    // windowMs (600); a knockdown its ragdoll time plus knockdownTailMs (standing up). Also remembers
    // when the last reaction and the last knockdown were, for HitReact::Decide.
    class HitReactArbiter
    {
    public:
        static constexpr double kLongAgoMs = 1e9;
        explicit HitReactArbiter(double windowMs = 600.0, double knockdownTailMs = 300.0)
            : windowMs_(windowMs), tailMs_(knockdownTailMs) {}
        void Start(double nowMs, const HitReaction& r);     // ReactKind::None is ignored
        HitReactOut Tick(double nowMs);
        bool Active() const { return active_; }
        double SinceLastMs(double nowMs) const { return haveLast_ ? nowMs - lastAtMs_ : kLongAgoMs; }
        double SinceKnockdownMs(double nowMs) const { return haveKd_ ? nowMs - lastKdAtMs_ : kLongAgoMs; }
        void Clear();                                       // the ped is gone: forget everything, no outputs
    private:
        double windowMs_, tailMs_;
        bool active_ = false, haveLast_ = false, haveKd_ = false;
        double untilMs_ = 0, lastAtMs_ = 0, lastKdAtMs_ = 0;
    };

    // ---- melee (sender) ----------------------------------------------------------------------------

    // Counts a melee swing for shotSeq: a press of a melee control (INPUT_MELEE_ATTACK_LIGHT / HEAVY /
    // ALTERNATE, 140 / 141 / 142, INFERRED) or the rising edge of IS_PED_PERFORMING_MELEE_ACTION. The two
    // triggers see the same swing a few frames apart, so a trigger within dedupeMs of a counted swing is
    // the same swing.
    class MeleeSwingGate
    {
    public:
        explicit MeleeSwingGate(double dedupeMs = 300.0) : dedupeMs_(dedupeMs) {}
        // Once per frame while on foot with a melee-class weapon. True = count one swing now.
        bool Tick(double nowMs, bool controlPressed, bool actionNow)
        {
            const bool rising = actionNow && !prevAction_;
            prevAction_ = actionNow;
            if (!controlPressed && !rising)
                return false;
            if (nowMs - lastSwingMs_ < dedupeMs_)
                return false;
            lastSwingMs_ = nowMs;
            return true;
        }
        void Reset() { prevAction_ = false; lastSwingMs_ = -1e9; }
    private:
        double dedupeMs_;
        bool prevAction_ = false;
        double lastSwingMs_ = -1e9;
    };

    // Receiver (0c): whether a remote ped's swing is due now. A swing is a step of the sender's shotSeq OR the rising
    // edge of its kPfMelee flag (sent since protocol v5, ignored by the receiver until 0.4.4: a swing whose counter
    // step was missed was never shown); the two are one swing within the gate's dedupe. Only with a melee-class weapon.
    // Once per frame per remote ped (the gate tracks the flag's edge).
    inline bool PeerSwingDue(MeleeSwingGate& gate, double nowMs, int shotSteps, bool meleeWeapon, bool meleeFlag)
    {
        return gate.Tick(nowMs, meleeWeapon && shotSteps > 0, meleeWeapon && meleeFlag);
    }

    // ---- melee hit on a puppet, by the damage EVENT (0.4.4 item 0c) ----------------------------------
    //
    // Live test 0.4.3: punches at a friend's puppet counted swings but sent no Hit. A puppet's health pool is
    // restored after every hit and the pool drop (ProbePeerDamage) is the hit; with the game-wide AI melee damage
    // modifier at 0 (W8) a fist took nothing off the pool, so nothing was ever seen. A melee blow is therefore also
    // detected by the damage EVENT (HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(puppet, local ped), cleared after it is read):
    // a melee-class weapon in hand, the event, and our own swing (counted at the press, TickLocalMelee) or melee
    // action still in progress => one Hit of the weapon's nominal damage. The server's per-weapon melee ceiling
    // (apps/server/anticheat.h MeleeNominalMax: fists 40, light 60, heavy 80) and its need for a swing decide the
    // rest. Guns (a non-melee weapon) keep the pool probe only.
    constexpr double kMeleeHitWindowMs = 900.0;     // a blow lands within this of the swing counted at the press
    constexpr int kMeleeNominalDamage = kDeadMeleeCapDamage;   // the claim of an event-detected blow (fists ceiling)

    // Whether the damage event of a puppet is worth reading now: a melee-class weapon in hand and our swing recent
    // (sinceSwingMs since the last counted swing) or a melee action running. Keeps the native calls to swing windows.
    inline bool MeleeEventWindow(bool meleeWeaponHeld, double sinceSwingMs, bool meleeActionNow)
    {
        return meleeWeaponHeld && (meleeActionNow || (sinceSwingMs >= 0.0 && sinceSwingMs <= kMeleeHitWindowMs));
    }

    // The stale damage event (0.4.5, melee audit). HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY stays true until it is cleared, and
    // the event is read ONLY in a swing window (MeleeEventWindow) and cleared only once read. A blow that lands outside any
    // window (a heavy swing past the 900 ms, a blow the pool did not show because fists took nothing off it) therefore left
    // its event standing, and the very first frame of the NEXT swing's window read it: a swing that missed was reported as a
    // hit (a ghost hit, claimed at the nominal 40 on the first frame of the new swing). One per puppet: the first frame a
    // window opens after it was closed discards whatever event is standing (nothing in a swing can land on its own first
    // frame: the animation needs 100 ms or more); a new swing counted while the window is still open keeps reading, because
    // the standing event then belongs to the blow of the swing before it (a combo).
    class MeleeEventGate
    {
    public:
        enum class Step : uint8_t
        {
            Closed,      // no window: do not touch the event
            ClearStale,  // the window just opened: read the event for the log, clear it, report nothing
            Read,        // the window is open: the event, if set, is a blow of this window
        };
        Step Tick(bool windowOpen)
        {
            if (!windowOpen)
            {
                open_ = false;
                return Step::Closed;
            }
            const bool fresh = !open_;
            open_ = true;
            return fresh ? Step::ClearStale : Step::Read;
        }
        void Reset() { open_ = false; }
    private:
        bool open_ = false;
    };

    enum class MeleeHitSource : uint8_t { None, Pool, Event };
    struct MeleeHitIn
    {
        bool   meleeWeaponHeld = false;   // the local ped's weapon is melee-class (fists, GROUP_MELEE / GROUP_UNARMED)
        bool   eventDamaged = false;      // HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(puppet, local ped), read in the window only
        double sinceSwingMs = 1e9;        // since our last counted swing (TickLocalMelee)
        bool   meleeActionNow = false;    // IS_PED_PERFORMING_MELEE_ACTION(local ped)
        bool   poolReport = false;        // ProbePeerDamage says "report" (a pool drop we caused)
        int    poolDamage = 0;            // ... and its damage
    };
    struct MeleeHitOut
    {
        MeleeHitSource source = MeleeHitSource::None;   // which detector produced the report
        int  damage = 0;                                // 0 with source None
        bool clearEvent = false;                        // CLEAR_ENTITY_LAST_DAMAGE_ENTITY: the event was read (also a stale one)
    };
    inline MeleeHitOut DecideMeleeHit(const MeleeHitIn& in)
    {
        MeleeHitOut o;
        if (in.meleeWeaponHeld && in.eventDamaged)
        {
            o.clearEvent = true;                        // read once; a stale event must not fire on the next swing
            if (MeleeEventWindow(true, in.sinceSwingMs, in.meleeActionNow))
            {
                o.source = MeleeHitSource::Event;
                o.damage = kMeleeNominalDamage;         // the event replaces the pool number: never both for one blow
                return o;
            }
        }
        if (in.poolReport && in.poolDamage > 0)
        {
            o.source = MeleeHitSource::Pool;            // guns, vehicles, and a melee drop with no event in the window
            o.damage = in.poolDamage;
        }
        return o;
    }

    // ---- geometry and timing helpers -----------------------------------------------------------------

    // Whether the segment a -> b passes within `radius` of the vertical axis of a standing player (centre
    // c, half height halfHeight). peer_shoot=ped fires a REAL shot, which must not cross anybody's body.
    bool SegmentTouchesCapsule(float ax, float ay, float az, float bx, float by, float bz,
                               float cx, float cy, float cz, float radius, float halfHeight);

    // When a puppet's death is shown: the Killed arrived at receivedMs, the puppet's pose is delayMs
    // behind (its smoother's interpolation delay), so the death is shown that much later. delayMs is
    // clamped to 0..500.
    inline double DeathDisplayAtMs(double receivedMs, double delayMs)
    {
        return receivedMs + (delayMs < 0.0 ? 0.0 : delayMs > 500.0 ? 500.0 : delayMs);
    }

    // ---- sync plan W6: the inputs protocol v5 adds -------------------------------------------------------

    // The maximum health GTA must give our ped for the server's `playerHealth` (Welcome.playerHealth, 1..255, in
    // PedState units): the ped's death line is 100, so its maximum is 100 + playerHealth. 0 (unknown) is the
    // default 100.
    inline int PedMaxHealth(int playerHealth) { return 100 + (playerHealth >= 1 && playerHealth <= 255 ? playerHealth : 100); }

    // The HUD's health fraction in [0, 1] from GET_ENTITY_HEALTH and GET_ENTITY_MAX_HEALTH: GTA ped health runs
    // from 100 (dead) up to the maximum, so the span is (hp - 100) / (maxHp - 100); a maximum of 100 or less has
    // no such span and reads hp / maxHp (0 for an unreadable maximum).
    inline float HealthFraction(int hp, int maxHp)
    {
        float f;
        if (maxHp > 100)
            f = (float)(hp - 100) / (float)(maxHp - 100);
        else
            f = maxHp > 0 ? (float)hp / (float)maxHp : 0.0f;
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

    // Put the server's maximum on a ped and read it straight back (T6: the old code ignored both the native's
    // result and whether GTA kept the value). `P` offers `bool SetMax(int)` and `int GetMax()`; the engine wraps
    // World::SetEntityMaxHealth / GetEntityMaxHealth, the tests a simulated ped. Returns the max read back.
    template <class P>
    inline int ApplyMaxHealth(P& ped, int playerHealth, VitalsTracker& tracker)
    {
        const int wanted = PedMaxHealth(playerHealth);
        const bool ok = ped.SetMax(wanted);
        const int back = ped.GetMax();
        tracker.NoteMaxWrite(wanted, ok, back);
        return back;
    }

    // The HUD's health fraction (DrawHud): the ped's own health over its own maximum, except that a frame whose
    // GET_ENTITY_MAX_HEALTH reads 100 or less (0 = the native failed) must not draw an empty bar. The newest good
    // maximum stands in, then the maximum the server's playerHealth implies.
    inline float HudHealthFraction(int hp, int maxHp, int playerHealth, int lastGoodMax = 0)
    {
        const int m = maxHp > 100 ? maxHp : (lastGoodMax > 100 ? lastGoodMax : PedMaxHealth(playerHealth));
        return HealthFraction(hp, m);
    }

    // ---- R3 T1: measuring what GTA really holds, and the controller that runs the DIRECT / SCALED choice ----------------------
    //
    // Round 2 assumed "SET_ENTITY_MAX_HEALTH(100 + ph) lets the ped hold 100 + ph" and the read-back of the MAX proved nothing about the
    // HEALTH. The probe writes in the order the engine uses and reads the real health after every write:
    //   SET_ENTITY_MAX_HEALTH(w), SET_ENTITY_HEALTH(w), read    -> h1   (h1 >= w: the entity maximum is enough)
    //   SET_PED_MAX_HEALTH(w),    SET_ENTITY_HEALTH(w), read    -> h2   (h2 >= w, h1 < w: the ped maximum lifts the cap)
    // and what stays below w after both is the CEILING (the ped is capped whatever the maxima say).
    enum class HealthBehaviour : uint8_t
    {
        Unknown,        // the health could not be read: nothing was learnt
        Holds,          // SET_ENTITY_MAX_HEALTH alone is enough (the round 2 model)
        FollowsPedMax,  // only SET_PED_MAX_HEALTH lifts the cap
        Capped,         // health stays at a ceiling below the wish whatever the maxima read (the live 0.4.7 log: 200)
    };
    inline const char* HealthBehaviourName(HealthBehaviour b)
    {
        return b == HealthBehaviour::Holds ? "GTA holds the health (SET_ENTITY_MAX_HEALTH is enough)"
             : b == HealthBehaviour::FollowsPedMax ? "the cap follows SET_PED_MAX_HEALTH"
             : b == HealthBehaviour::Capped ? "HEALTH CAPPED below the wish whatever the maxima read"
             : "unknown (health unreadable)";
    }

    struct ProbeStep
    {
        const char* what = "";   // "start", "SET_ENTITY_MAX_HEALTH", "SET_ENTITY_HEALTH", "SET_PED_MAX_HEALTH", "+1 frame", "+1 s"
        int  arg = 0;
        bool ok = true;          // the native's result
        int  hp = 0, max = 0;    // GET_ENTITY_HEALTH / GET_ENTITY_MAX_HEALTH read straight after
    };

    struct HealthProbeResult
    {
        static constexpr int kMaxSteps = 8;
        ProbeStep steps[kMaxSteps];
        int stepCount = 0;
        int wanted = 0;                              // 100 + playerHealth
        int hpAfter = 0;                             // GET_ENTITY_HEALTH after the last write (0 = unreadable)
        int ceiling = 0;                             // hpAfter - 100 when readable, else 0
        HealthBehaviour behaviour = HealthBehaviour::Unknown;
        bool anyWriteFailed = false;
        void Add(const char* what, int arg, bool ok, int hp, int max)
        {
            if (!ok)
                anyWriteFailed = true;
            if (stepCount < kMaxSteps)
            {
                ProbeStep& st = steps[stepCount++];
                st.what = what; st.arg = arg; st.ok = ok; st.hp = hp; st.max = max;
            }
        }
    };

    // P (the engine wraps World, the tests a simulated ped): bool SetEntityMax(int), bool SetPedMax(int), bool SetHealth(int),
    // int GetHealth() (<= 0 unreadable), int GetMax().
    template <class P>
    inline HealthProbeResult RunHealthProbe(P& ped, int wanted)
    {
        HealthProbeResult r;
        r.wanted = wanted;
        r.Add("start", 0, true, ped.GetHealth(), ped.GetMax());
        bool ok = ped.SetEntityMax(wanted);
        r.Add("SET_ENTITY_MAX_HEALTH", wanted, ok, ped.GetHealth(), ped.GetMax());
        ok = ped.SetHealth(wanted);
        const int h1 = ped.GetHealth();
        r.Add("SET_ENTITY_HEALTH", wanted, ok, h1, ped.GetMax());
        ok = ped.SetPedMax(wanted);
        r.Add("SET_PED_MAX_HEALTH", wanted, ok, ped.GetHealth(), ped.GetMax());
        ok = ped.SetHealth(wanted);
        const int h2 = ped.GetHealth();
        r.Add("SET_ENTITY_HEALTH", wanted, ok, h2, ped.GetMax());
        r.hpAfter = h2 > 0 ? h2 : (h1 > 0 ? h1 : 0);
        if (r.hpAfter <= 100)
        {
            r.hpAfter = 0;                            // unreadable, or a ped the write killed: nothing learnt
            r.behaviour = HealthBehaviour::Unknown;
        }
        else
        {
            r.ceiling = r.hpAfter - 100;
            r.behaviour = h1 >= wanted ? HealthBehaviour::Holds : h2 >= wanted ? HealthBehaviour::FollowsPedMax : HealthBehaviour::Capped;
        }
        return r;
    }

    // The scale for a measured ceiling (GTA units above 100; 0 = unknown) and the registry wish (health_mode).
    //   direct  always DIRECT (the 0.4.7 behaviour, the roll-back)
    //   scaled  always SCALED; an unmeasurable ceiling is taken as 100 (the 200 cap of the live log)
    //   auto    SCALED when the ped holds less than playerHealth, else DIRECT; an unmeasurable ceiling stays DIRECT
    inline HealthScale ChooseHealthScale(HealthMode wish, int ph, int ceiling)
    {
        HealthScale s;
        s.ph = (ph >= 1 && ph <= 255) ? ph : 100;
        if (wish == HealthMode::Direct)
            return s;
        int c = ceiling;
        if (c < 10)
            c = wish == HealthMode::Scaled ? 100 : 0;   // a ceiling this low is a misread, not a cap
        s.ceiling = c;
        return s;
    }

    // The bar the SERVER's number draws: health minus the self-damage already reported (the answer is in flight), over playerHealth.
    inline float ServerBarFraction(int health, int pending, int ph)
    {
        if (ph <= 0)
            return 0.0f;
        const int v = health - (pending > 0 ? pending : 0);
        const float f = (float)v / (float)ph;
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

    // Runs the write sequence, measures, chooses DIRECT or SCALED and tells the tracker; keeps the 1-frame / 1-second
    // read-backs and the log lines (the engine prints them). Script tick only. The engine and the tests both drive it.
    class HealthController
    {
    public:
        struct Line { bool warn = false; std::string text; };

        HealthMode wish = HealthMode::Auto;

        const HealthScale& Scale() const { return scale_; }
        bool Scaled() const { return scale_.Scaled(); }
        int  Ceiling() const { return ceiling_; }                  // GTA health above 100 the ped held at the last measurement (0 = none yet)
        HealthBehaviour Behaviour() const { return behaviour_; }
        uint32_t Probes() const { return probes_; }
        uint32_t CeilingDrops() const { return drops_; }          // the +1 frame read found less than the probe left
        uint32_t LateOff() const { return lateOff_; }             // the +1 s read disagreed with the tracker
        int LastHp() const { return lastHp_; }
        int LastMax() const { return lastMax_; }
        bool Settling() const { return recheckPending_; }

        // Session start / ped swap / respawn / the tracker's request. `tick` numbers the script tick in progress.
        template <class P>
        void Begin(P& ped, int32_t handle, int ph, VitalsTracker& tr, uint32_t tick, double nowMs, const char* why)
        {
            ph_ = (ph >= 1 && ph <= 255) ? ph : 100;
            const int wanted = PedMaxHealth(ph_);
            HealthProbeResult r = RunHealthProbe(ped, wanted);
            ++probes_;
            behaviour_ = r.behaviour;
            ceiling_ = r.ceiling;
            scale_ = ChooseHealthScale(wish, ph_, r.ceiling);
            if (scale_.Scaled())
            {
                // The ped's maximum is what its top is called: the ceiling, so the ped at 100 + ceiling is "full" for GTA too.
                const int gm = scale_.GtaMax();
                const bool a = ped.SetEntityMax(gm);
                const bool b = ped.SetPedMax(gm);
                r.Add("SET_*_MAX_HEALTH (scaled)", gm, a && b, ped.GetHealth(), ped.GetMax());
            }
            lastHp_ = ped.GetHealth();
            lastMax_ = ped.GetMax();
            tr.SetScale(scale_);
            tr.NoteProbed(handle);
            tr.NoteMaxWrite(wanted, !r.anyWriteFailed, lastMax_);
            handle_ = handle;
            probeHp_ = r.hpAfter;
            recheckPending_ = r.hpAfter > 0;
            recheckTick_ = tick + 1;
            latePending_ = true;
            lateAt_ = nowMs + 1000.0;
            // One line with every step.
            char b[160];
            std::string t = "engine: health: probe [";
            t += why;
            std::snprintf(b, sizeof(b), "] ped %d, playerHealth %d -> wish max %d, health_mode=%s: ", (int)handle, ph_, wanted, HealthModeName(wish));
            t += b;
            for (int i = 0; i < r.stepCount; ++i)
            {
                const ProbeStep& st = r.steps[i];
                if (st.arg)
                    std::snprintf(b, sizeof(b), "%s(%d) %s -> hp %d max %d; ", st.what, st.arg, st.ok ? "ok" : "FAILED", st.hp, st.max);
                else
                    std::snprintf(b, sizeof(b), "%s: hp %d max %d; ", st.what, st.hp, st.max);
                t += b;
            }
            std::snprintf(b, sizeof(b), "=> %s; ceiling %s%d; mode %s (ceiling %d of playerHealth %d)", HealthBehaviourName(r.behaviour),
                          r.ceiling ? "" : "n/a ", r.ceiling, scale_.Scaled() ? "SCALED" : "DIRECT", scale_.ceiling, ph_);
            t += b;
            const bool warn = r.behaviour == HealthBehaviour::Capped || r.behaviour == HealthBehaviour::Unknown || r.anyWriteFailed;
            Push(warn, t);
        }

        // Every script tick before the tracker. Returns true when the tracker must skip this tick (the probe's one-frame check is due
        // next tick and the ped still holds the probe's value, not the server's).
        template <class P>
        bool Pre(P& ped, int32_t handle, VitalsTracker& tr, uint32_t tick, double nowMs)
        {
            if ((recheckPending_ || latePending_) && handle != 0 && handle != handle_)
                recheckPending_ = latePending_ = false;              // another ped now: its own probe follows
            if (recheckPending_ && tick >= recheckTick_)
            {
                recheckPending_ = false;
                const int hp = ped.GetHealth(), mx = ped.GetMax();
                lastHp_ = hp;
                lastMax_ = mx;
                char b[256];
                if (hp > 100 && probeHp_ > 100 && hp < probeHp_)
                {
                    // GTA took part of it back after a frame: the ceiling is what is left.
                    ++drops_;
                    behaviour_ = HealthBehaviour::Capped;
                    ceiling_ = hp - 100;
                    scale_ = ChooseHealthScale(wish, ph_, ceiling_);
                    if (scale_.Scaled())
                    {
                        const int gm = scale_.GtaMax();
                        ped.SetEntityMax(gm);
                        ped.SetPedMax(gm);
                    }
                    tr.SetScale(scale_);
                    std::snprintf(b, sizeof(b), "engine: health: probe +1 frame: ped hp %d max %d, the probe left hp %d => GTA TOOK IT BACK, ceiling now %d; mode %s",
                                  hp, mx, probeHp_, ceiling_, scale_.Scaled() ? "SCALED" : "DIRECT");
                    Push(true, b);
                }
                else
                {
                    std::snprintf(b, sizeof(b), "engine: health: probe +1 frame: ped hp %d max %d (the probe left hp %d): unchanged", hp, mx, probeHp_);
                    Push(false, b);
                }
            }
            if (latePending_ && !recheckPending_ && nowMs >= lateAt_)
            {
                latePending_ = false;
                const int hp = ped.GetHealth(), mx = ped.GetMax();
                lastHp_ = hp;
                lastMax_ = mx;
                const int want = 100 + scale_.ToGta((int)tr.Health());
                const bool settled = tr.Pending() == 0 && !tr.ServerDead();
                const bool off = settled && hp > 0 && hp != want;
                if (off)
                    ++lateOff_;
                char b[256];
                std::snprintf(b, sizeof(b), "engine: health: probe +1 s: ped hp %d max %d, tracker wants hp %d (server %u, %s)%s", hp, mx, want,
                              (unsigned)tr.Health(), scale_.Scaled() ? "SCALED" : "DIRECT", off ? "  <-- differs" : "");
                Push(off, b);
            }
            return recheckPending_;
        }

        // The newest read-backs the 10 s line shows.
        void NoteRead(int hp, int mx) { lastHp_ = hp; lastMax_ = mx; }

        // The 10 s line (`engine: health: ...`): ONE line that tells which GTA behaviour is true. `serverMode`: connected and entered
        // (the bar is then the server's number when SCALED).
        std::string StatusLine(const VitalsTracker& tr, int ph, bool serverMode) const
        {
            const VitalsStats& vs = tr.Stats();
            const HealthScale& sc = tr.Scale();
            const float bar = (sc.Scaled() && serverMode && !tr.ServerDead())
                ? ServerBarFraction((int)tr.Health(), tr.Pending(), ph)
                : HudHealthFraction(vs.obsHealth, vs.obsMax, ph, tr.LastGoodMax());
            char b[900];
            std::snprintf(b, sizeof(b),
                "engine: health: mode %s (health_mode=%s), measured ceiling %d (%s), ped %d / max %d (GTA wants %d / %d), server health %u of "
                "playerHealth %d, bar %.0f%%; read-backs: probe hp %d max %d; probes %u (+1 frame took back %u, +1 s differed %u); "
                "totals: max changes %u, gains undone %u, largest self-hit %u, ped swaps %u, reconciles %u (gave up %u, re-probes %u), "
                "max reads unreadable %u, max writes %u (failed %u, read-back differs %u, last wish %d / read %d), scale changes %u",
                sc.Scaled() ? "SCALED" : "DIRECT", HealthModeName(wish), ceiling_, HealthBehaviourName(behaviour_), vs.obsHealth, vs.obsMax,
                100 + sc.ToGta((int)tr.Health()), sc.GtaMax(), (unsigned)tr.Health(), ph, bar * 100.0f, lastHp_, lastMax_, probes_, drops_,
                lateOff_, vs.maxChanges, vs.clampsDown, vs.largestSelfHit, vs.pedSwaps, vs.reconciles, vs.reconcileGaveUp, vs.reprobes,
                vs.maxUnreadable, vs.maxWrites, vs.maxWriteFails, vs.maxReadBackOff, vs.lastMaxWanted, vs.lastMaxReadBack, vs.scaleChanges);
            return b;
        }

        bool TakeLine(Line& out)
        {
            if (lines_.empty())
                return false;
            out = lines_.front();
            lines_.erase(lines_.begin());
            return true;
        }
        void Reset()                                   // a session began or ended
        {
            recheckPending_ = latePending_ = false;
            lines_.clear();
        }

    private:
        void Push(bool warn, const std::string& t)
        {
            if (lines_.size() < 32)
            {
                Line l;
                l.warn = warn;
                l.text = t;
                lines_.push_back(l);
            }
        }
        HealthScale scale_;
        int ph_ = 100, ceiling_ = 0, probeHp_ = 0, lastHp_ = 0, lastMax_ = 0;
        HealthBehaviour behaviour_ = HealthBehaviour::Unknown;
        int32_t handle_ = 0;
        bool recheckPending_ = false, latePending_ = false;
        uint32_t recheckTick_ = 0;
        double lateAt_ = 0;
        uint32_t probes_ = 0, drops_ = 0, lateOff_ = 0;
        std::vector<Line> lines_;
    };

    // What a protocol v5 Damage is to this client (it reaches every relevant player, not only the two involved).
    enum class DamageAbout : uint8_t
    {
        Me,       // we are the victim (a self-hit included): the inbox, the vitals tracker
        MyHit,    // we shot somebody else: confirmed damage, and the victim's puppet reacts on our screen
        Onlook,   // victim and attacker are both other players: the victim's puppet reacts on our screen
        None,     // no known victim
    };
    inline DamageAbout ClassifyDamage(uint16_t me, uint16_t victim, uint16_t attacker)
    {
        if (victim == 0xFFFF)               // proto::kNoPlayer: nobody
            return DamageAbout::None;
        if (victim == me)
            return DamageAbout::Me;
        return attacker == me ? DamageAbout::MyHit : DamageAbout::Onlook;
    }
    // Whether a Damage makes the victim's puppet flinch / fall: not a killing blow (the death display owns
    // that), not 0 damage, and for an onlooker not a self-inflicted or server / unattributed one (nobody to
    // react AT; the ped would only flinch without a direction).
    inline bool ReactsPuppet(DamageAbout about, bool killed, uint16_t damage, uint16_t victim, uint16_t attacker)
    {
        if (killed || damage == 0)
            return false;
        if (about == DamageAbout::MyHit)
            return true;
        return about == DamageAbout::Onlook && attacker != victim && attacker != 0xFFFF;   // proto::kServerId / kNoPlayer
    }

    // Killed.flags (protocol v5; the same bits as proto::kKilledHeadshot / kKilledMelee).
    struct KillFeedFlags
    {
        static constexpr uint8_t kHeadshot = 1, kMelee = 2;
    };

    // The kill-feed line of a Killed: "A killed B with Carbine Rifle." / "... (headshot)." / melee "A beat B to
    // death with Knife." / a death with no killer (suicide, environment, the console) "B died.". `weaponLabel`
    // is WeaponLabel(weapon), "" when unknown. `victim` / `killer` are display names ("You" for ourselves).
    inline std::string KillFeedLine(const std::string& killer, const std::string& victim, std::string_view weaponLabel,
                                    uint8_t flags, bool noKiller)
    {
        if (noKiller)
            return victim + " died.";
        const bool melee = (flags & KillFeedFlags::kMelee) != 0;
        std::string line = killer + (melee ? " beat " : " killed ") + victim + (melee ? " to death" : "");
        if (!weaponLabel.empty())
            line += " with " + std::string(weaponLabel);
        if (flags & KillFeedFlags::kHeadshot)
            line += " (headshot)";
        return line + ".";
    }

    // ---- hand-off from the join worker to the script tick ------------------------------------------

    // Damage about us, folded into one record so a full event queue can never lose it (a lost ack
    // would leave a self-hit pending for ackTimeoutMs, a lost lethal Damage a living ped on a dead
    // player): the newest health and armour win, the damage sums add up. Single-threaded; the caller
    // holds its own lock around each call.
    struct DamageSlot
    {
        bool     any = false;
        uint8_t  health = 0, armour = 0;   // from the newest Damage
        uint32_t damage = 0;               // sum of every Damage about us since the last Take
        uint32_t selfAckDamage = 0;        // ... of those our own environmental self-hits caused
        uint32_t events = 0;
        bool     killed = false;           // a Damage with `killed` set, or a Killed about us
        uint32_t otherEvents = 0;          // Damage from somebody else (the log line)
        uint32_t otherDamage = 0;
        uint16_t lastAttacker = 0;
        uint32_t lastWeapon = 0;
        // Protocol v5 Damage.bone / Damage.flags of the damage from SOMEBODY ELSE (a self-hit adds neither):
        // the newest bone (-1 = none or unknown) and the OR of the flags (HitReact::kFlag*), which the
        // victim's own hit reaction decides on (ReactLocalHit).
        int16_t  lastBone = -1;
        uint8_t  otherFlags = 0;
    };

    class VitalsInbox
    {
    public:
        void OnDamage(uint8_t health, uint8_t armour, uint16_t damage, bool selfInflicted, bool killed, uint16_t attacker,
                      uint32_t weapon, int16_t bone = -1, uint8_t flags = 0);
        void OnKilled();                    // a Killed whose victim is us
        bool Take(DamageSlot& out);         // false when nothing arrived; clears the slot
        void Clear() { s_ = DamageSlot(); }
    private:
        DamageSlot s_;
    };

    // A bounded queue with two priorities for the events that may not be lost to a flood of others:
    // at its cap an important event makes room by evicting the oldest droppable one (a kill-feed
    // line, another player's damage), a droppable event is refused. Single-threaded.
    template <class T>
    class DropQueue
    {
    public:
        explicit DropQueue(size_t cap) : cap_(cap) {}
        bool Push(T item, bool important)
        {
            if (q_.size() >= cap_)
            {
                size_t victim = q_.size();
                if (important)
                    for (size_t i = 0; i < q_.size(); ++i)
                        if (!q_[i].important)
                        {
                            victim = i;
                            break;
                        }
                if (victim == q_.size())
                {
                    ++refused_;
                    return false;
                }
                q_.erase(q_.begin() + (std::ptrdiff_t)victim);
                ++evicted_;
            }
            q_.push_back(Entry{ std::move(item), important });
            return true;
        }
        // Moves everything out in arrival order.
        void Drain(std::vector<T>& out)
        {
            for (Entry& e : q_)
                out.push_back(std::move(e.item));
            q_.clear();
        }
        void Clear() { q_.clear(); }
        size_t Size() const { return q_.size(); }
        uint32_t Refused() const { return refused_; }
        uint32_t Evicted() const { return evicted_; }
    private:
        struct Entry { T item; bool important; };
        size_t cap_;
        std::vector<Entry> q_;
        uint32_t refused_ = 0, evicted_ = 0;
    };

}
