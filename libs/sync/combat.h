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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gage::sync {

    // ---- VitalsTracker ----------------------------------------------------------------------

    struct VitalsConfig
    {
        double ackTimeoutMs = 2000.0; // a self-hit with no Damage back by then is given up: resync
        int    maxHitDamage = 250;    // proto::kMaxHitDamage: one self-hit carries at most this
        int    maxChunks = 4;         // at most this many self-hits per tick (a 1000-point fall = 4 x 250)
        double puppetGraceMs = 1500.0; // after a puppet-caused loss a lethal self-hit waits this long for the attacker's Hit,
                                       // and a loss no Damage followed is undone (the server's value is put back)
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
    };

    struct VitalsStats
    {
        uint32_t selfHits = 0, selfDamage = 0;     // self-hits handed out and their damage
        uint32_t acks = 0, ackedDamage = 0;        // our own self-hits the server answered
        uint32_t damageEvents = 0, damageTaken = 0; // every Damage for us (any attacker)
        uint32_t resyncs = 0;                      // pending self-damage given up after ackTimeoutMs
        uint32_t clampsDown = 0;                   // a local gain undone (regeneration, /heal)
        uint32_t kills = 0;                        // the ped killed because the server says dead
        uint32_t puppetLosses = 0;                 // ticks whose loss a remote puppet caused (the attacker's Hit reports it)
        uint32_t puppetRestores = 0;               // ... of which no Damage followed in puppetGraceMs: the server's value put back
        uint32_t skippedReads = 0;                 // ticks skipped for an unreadable health or armour
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

        // Once per script tick while the player is in the world (not while respawning).
        VitalsOut Tick(const VitalsObs& o);

        bool ServerDead() const { return serverDead_; }
        uint8_t Health() const { return health_; }
        uint8_t Armour() const { return armour_; }
        int Pending() const { return pending_; }
        const VitalsStats& Stats() const { return stats_; }

    private:
        VitalsConfig cfg_;
        uint8_t health_ = 100, armour_ = 0;
        bool serverDead_ = false;
        bool applyPending_ = true;
        bool rebase_ = true;       // the next Tick only takes a baseline (after our own write)
        int lastLocal_ = 0;        // the ped's total (health above 100 + armour) at the last Tick
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
    inline void ReactionToShow(int mode, const HitReaction& r, bool& ragdoll, bool& bullet)
    {
        const bool ragdollKind = r.kind == ReactKind::Knockdown || r.kind == ReactKind::Stagger;
        ragdoll = ragdollKind && (mode == kHitReactRagdoll || mode == kHitReactBoth);
        bullet = !ragdoll && r.kind != ReactKind::None && (mode == kHitReactBullet || mode == kHitReactBoth);
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
