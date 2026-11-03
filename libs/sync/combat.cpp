// Combat decisions (see combat.h for the design).

#include "combat.h"

namespace gage::sync {

namespace {
    int MaxI(int a, int b) { return a > b ? a : b; }
    int MinI(int a, int b) { return a < b ? a : b; }
}

// ---- VitalsTracker ----------------------------------------------------------------------------
//
// Detection works on the CHANGE of the ped's total (health above 100 plus armour) between two
// ticks, never on a comparison with the server's values: GTA clamps what we put on the ped (max
// armour, max health), and a comparison would then see a permanent "loss" and report it every
// tick. A tick right after our own write (apply) only takes a new baseline.

VitalsTracker::VitalsTracker(const VitalsConfig& cfg) : cfg_(cfg) {}

void VitalsTracker::Reset(uint8_t health, uint8_t armour)
{
    health_ = health;
    armour_ = armour;
    serverDead_ = false;
    applyPending_ = true;
    rebase_ = true;
    pending_ = 0;
    pendingSinceMs_ = 0;
    puppetUntilMs_ = 0;
    puppetApplyAtMs_ = 0;
    lastLocal_ = 0;
    mismatchSinceMs_ = 0;
    mismatchHealth_ = -1;
    reconcileTries_ = 0;
}

void VitalsTracker::NotePedReplaced()
{
    // A new ped has GTA's default health, not ours, and its maximum may not be ours yet: nothing about the
    // change from the old ped is a loss. Baseline next tick, and the server's value goes on (once nothing is
    // pending; a self-hit in flight is answered first, the absolute check then corrects what is left).
    applyPending_ = true;
    rebase_ = true;
    lastMax_ = 0;
    lastGoodMax_ = 0;
    mismatchSinceMs_ = 0;
    mismatchHealth_ = -1;
    reconcileTries_ = 0;
}

void VitalsTracker::NoteMaxWrite(int wanted, bool ok, int readBack)
{
    ++stats_.maxWrites;
    if (!ok)
        ++stats_.maxWriteFails;
    if (readBack != wanted)
        ++stats_.maxReadBackOff;
    stats_.lastMaxWanted = wanted;
    stats_.lastMaxReadBack = readBack;
}

void VitalsTracker::OnDamage(uint8_t health, uint8_t armour, uint16_t damage, bool selfInflicted, bool killed)
{
    OnDamageMerged(health, armour, damage, selfInflicted ? damage : 0, 1, killed);
}

void VitalsTracker::OnDamageMerged(uint8_t health, uint8_t armour, uint32_t damage, uint32_t selfAckDamage, uint32_t events,
                                   bool killed)
{
    health_ = health;
    armour_ = armour;
    stats_.damageEvents += events;
    stats_.damageTaken += damage;
    if (selfAckDamage > 0)
    {
        ++stats_.acks;
        stats_.ackedDamage += selfAckDamage;
        pending_ = MaxI(0, pending_ - (int)(selfAckDamage > 100000u ? 100000u : selfAckDamage));
    }
    if (killed)
        OnKilled();
    applyPending_ = true;
}

void VitalsTracker::NotePuppetLoss(double nowMs)
{
    puppetApplyAtMs_ = nowMs + cfg_.puppetGraceMs;
}

void VitalsTracker::OnKilled()
{
    serverDead_ = true;
    health_ = 0;
    pending_ = 0;
}

void VitalsTracker::OnRespawn(uint8_t health, uint8_t armour)
{
    Reset(health, armour);
}

VitalsOut VitalsTracker::Tick(const VitalsObs& o)
{
    VitalsOut out;
    // A failed read is not a fall: GetEntityHealth answers 0 and GetPedArmour -1 when the native cannot
    // be called. Taking either as a value would report the whole health as a self-hit.
    if ((!o.dead && o.health <= 0) || o.armour < 0)
    {
        ++stats_.skippedReads;
        return out;
    }
    // A max of 100 or less is a failed read (0) or a ped with no span: never take it for the real maximum. The
    // newest good one stands in (a blind 200 clamped the server's 300 to 200 and the next good read re-raised it).
    const bool maxReadable = o.maxHealth > 100;
    if (maxReadable)
        lastGoodMax_ = o.maxHealth;
    else
        ++stats_.maxUnreadable;
    const int maxH = maxReadable ? o.maxHealth : (lastGoodMax_ > 100 ? lastGoodMax_ : 200);
    if (o.ped != 0)
    {
        if (pedHandle_ != 0 && o.ped != pedHandle_)
        {
            ++stats_.pedSwaps;
            NotePedReplaced();
        }
        pedHandle_ = o.ped;
    }
    stats_.obsHealth = o.health;
    stats_.obsMax = o.maxHealth;
    const int expHealth = MinI((int)health_, maxH - 100);   // what the ped can show of the server's health
    const int localTotal = o.dead ? 0 : MaxI(0, o.health - 100) + MaxI(0, o.armour);

    if (serverDead_)
    {
        pending_ = 0;
        rebase_ = true;
        if (!o.dead)
        {
            out.kill = true;
            ++stats_.kills;
        }
        return out;
    }
    if (pending_ > 0 && o.nowMs - pendingSinceMs_ >= cfg_.ackTimeoutMs)
    {
        pending_ = 0;                  // the server dropped it (rate, a race with a death): resync
        ++stats_.resyncs;
        applyPending_ = true;
    }
    auto applyServer = [&] {
        out.apply = true;
        out.setHealth = 100 + expHealth;
        out.setArmour = armour_;
        rebase_ = true;
        puppetApplyAtMs_ = 0;          // the server's value is on the ped: a scheduled put-back is spent
    };
    const bool maxChanged = maxReadable && lastMax_ != 0 && maxH != lastMax_;
    if (maxReadable)
        lastMax_ = maxH;
    if (applyPending_ && pending_ == 0 && !o.dead)
    {
        applyPending_ = false;
        applyServer();
        return out;
    }
    // The ped's maximum moved (ApplyServerMaxHealth after a model swap, entry or respawn): the value written
    // under the old maximum may have been cut to it (server 200 on a GTA max of 200 shows as 100 of 200), and
    // raising the maximum later does not raise the health. Put the server's value on again. Live 0.4.3: the
    // ped sat at 200 / 300 (a half bar) for the whole session and would have died at half the server's health.
    if (maxChanged && pending_ == 0 && !o.dead && o.health != 100 + expHealth)
    {
        ++stats_.maxChanges;
        applyServer();
        return out;
    }
    if (puppetApplyAtMs_ > 0 && o.nowMs >= puppetApplyAtMs_ && pending_ == 0 && !o.dead)
    {
        // A puppet-caused loss (a ram, or our own cosmetic 1-HP flinch bullet) whose attacker's Hit/Damage
        // never came: it was re-baselined, so without this the ped would stay below the server's value for
        // the rest of its life and a later fall could read as a death. Put the server's value back.
        ++stats_.puppetRestores;
        applyServer();
        return out;
    }
    if (rebase_)
    {
        rebase_ = false;
        lastLocal_ = localTotal;
        if (!o.dead)
            return out;
    }
    if (o.puppetDamage)
    {
        // A remote puppet (a ram, a thrown car) hurt us: its owner's client sends the Hit, and the
        // server's Damage then puts the value on the ped. Reporting the loss too would count it twice.
        lastLocal_ = localTotal;
        puppetUntilMs_ = o.nowMs + cfg_.puppetGraceMs;
        if (!o.dead)
            puppetApplyAtMs_ = o.nowMs + cfg_.puppetGraceMs;   // no Damage by then: put the server's value back
        ++stats_.puppetLosses;
        return out;
    }
    const int delta = lastLocal_ - localTotal;
    lastLocal_ = localTotal;
    if (o.dead && o.nowMs < puppetUntilMs_)
        return out;                    // rammed to death: the attacker's Hit gets its grace to kill us first

    // Absolute check (T6). Loss detection above works on CHANGES, so a ped that sits off the server's value without
    // any change (a swapped ped's default 200 under a max of 300, a write GTA cut) is never noticed by it. A steady
    // mismatch with nothing pending, no puppet in play and a living ped is put right after reconcileAfterMs; if the
    // write does not take reconcileTries times GTA is holding the ped there and the tracker stops fighting it.
    if (delta == 0 && !o.dead && pending_ == 0 && puppetApplyAtMs_ == 0 && o.nowMs >= puppetUntilMs_ &&
        o.health != 100 + expHealth)
    {
        if (mismatchSinceMs_ == 0 || o.health != mismatchHealth_)
        {
            mismatchSinceMs_ = o.nowMs > 0 ? o.nowMs : 1e-3;
            mismatchHealth_ = o.health;
            reconcileTries_ = 0;
        }
        else if (o.nowMs - mismatchSinceMs_ >= cfg_.reconcileAfterMs && o.nowMs - lastReconcileMs_ >= cfg_.reconcileRetryMs)
        {
            if (reconcileTries_ < cfg_.reconcileTries)
            {
                ++reconcileTries_;
                lastReconcileMs_ = o.nowMs;
                ++stats_.reconciles;
                applyServer();
                return out;
            }
            if (reconcileTries_ == cfg_.reconcileTries)
            {
                ++reconcileTries_;          // counted once per mismatch
                ++stats_.reconcileGaveUp;
            }
        }
    }
    else
    {
        mismatchSinceMs_ = 0;
        mismatchHealth_ = -1;
        reconcileTries_ = 0;
    }

    int send = 0;
    if (o.dead)
        send = MaxI(delta, (int)health_ + (int)armour_ - pending_); // dead here: the server's WHOLE value (it may exceed what a ped shows)
    else if (delta > 0)
        send = delta;
    else if (delta < 0)
    {
        if (pending_ == 0)
        {
            applyServer();                 // a gain nobody granted (regeneration, /heal): undo it
            ++stats_.clampsDown;
        }
        return out;
    }
    const int chunks = MinI(cfg_.maxChunks, VitalsOut::kMaxSelfHits);
    while (send > 0 && out.selfHitCount < chunks)
    {
        const int d = MinI(send, cfg_.maxHitDamage);
        out.selfHits[out.selfHitCount++] = d;
        if ((uint32_t)d > stats_.largestSelfHit)
            stats_.largestSelfHit = (uint32_t)d;
        send -= d;
        pending_ += d;
        ++stats_.selfHits;
        stats_.selfDamage += (uint32_t)d;
    }
    if (out.selfHitCount)
        pendingSinceMs_ = o.nowMs;
    return out;
}

// ---- RespawnGate ------------------------------------------------------------------------------

RespawnGate::RespawnGate(const RespawnConfig& cfg) : cfg_(cfg) {}

void RespawnGate::OnDeath(double nowMs)
{
    if (dead_)
        return;
    dead_ = true;
    deathMs_ = nowMs;
    lastSendMs_ = 0;
    attempts_ = 0;
    lateSends_ = 0;
}

void RespawnGate::OnAlive()
{
    dead_ = false;
    attempts_ = 0;
    lateSends_ = 0;
}

bool RespawnGate::ShouldSend(double nowMs)
{
    if (!dead_ || nowMs - deathMs_ < cfg_.firstAfterMs)
        return false;
    const bool late = nowMs - deathMs_ >= cfg_.giveUpMs;
    double interval = cfg_.retryMs;
    if (late)                                   // the server keeps saying no: 1, 2, 4, 8 s ... capped
        for (int i = 0; i < lateSends_ && interval < cfg_.maxRetryMs; ++i)
            interval *= 2.0;
    if (interval > cfg_.maxRetryMs)
        interval = cfg_.maxRetryMs;
    if (attempts_ > 0 && nowMs - lastSendMs_ < interval)
        return false;
    lastSendMs_ = nowMs;
    ++attempts_;
    if (late)
        ++lateSends_;
    return true;
}

// ---- HitBatcher -------------------------------------------------------------------------------

HitBatcher::HitBatcher(const HitBatchConfig& cfg) : cfg_(cfg)
{
    const size_t cap = sizeof(slots_) / sizeof(slots_[0]);
    if (cfg_.maxPending == 0 || cfg_.maxPending > cap)
        cfg_.maxPending = cap;
    if (cfg_.maxDamage < 1)
        cfg_.maxDamage = 1;
}

void HitBatcher::Add(const BatchedHit& h, double nowMs)
{
    if (h.damage == 0)
        return;
    ++stats_.added;
    for (size_t i = 0; i < count_; ++i)
    {
        Slot& s = slots_[i];
        if (s.h.target == h.target && s.h.kind == h.kind)
        {
            s.damage += h.damage;
            s.h.weapon = h.weapon;
            s.h.bone = h.bone;
            s.h.shotSeq = h.shotSeq;
            ++stats_.merged;
            return;
        }
    }
    if (count_ >= cfg_.maxPending)
    {
        for (size_t i = 1; i < count_; ++i)  // drop the oldest
            slots_[i - 1] = slots_[i];
        --count_;
        ++stats_.dropped;
    }
    Slot& s = slots_[count_++];
    s.h = h;
    s.damage = h.damage;
    s.sinceMs = nowMs;
}

size_t HitBatcher::Flush(double nowMs, BatchedHit* out, size_t cap)
{
    if (tokensAtMs_ < 0)
        tokens_ = cfg_.burst;
    else if (nowMs > tokensAtMs_)
    {
        tokens_ += (nowMs - tokensAtMs_) * cfg_.ratePerSec / 1000.0;
        if (tokens_ > cfg_.burst)
            tokens_ = cfg_.burst;
    }
    tokensAtMs_ = nowMs;

    size_t n = 0, keep = 0;
    for (size_t i = 0; i < count_; ++i)
    {
        Slot s = slots_[i];
        while (s.damage > 0 && nowMs - s.sinceMs >= cfg_.holdMs && n < cap)
        {
            if (tokens_ < 1.0)
            {
                ++stats_.rateHeld;
                break;
            }
            const uint32_t d = s.damage > (uint32_t)cfg_.maxDamage ? (uint32_t)cfg_.maxDamage : s.damage;
            BatchedHit h = s.h;
            h.damage = (uint16_t)d;
            out[n++] = h;
            s.damage -= d;
            tokens_ -= 1.0;
            ++stats_.sent;
        }
        if (s.damage > 0)
            slots_[keep++] = s;
    }
    count_ = keep;
    return n;
}

// ---- remote ped damage probe ------------------------------------------------------------------

PeerDamageOut ProbePeerDamage(int health, int pool, bool damagedByLocal, bool dead, int deadMaxDamage)
{
    PeerDamageOut o;
    if (pool <= 0)
        return o;
    if (dead)
    {
        // One blow above the pool (a headshot, a rocket) killed the puppet on this machine. The pool
        // counts as dealt; a puppet is respawned either way, whoever killed it.
        o.damage = MaxI(pool - MaxI(health, 0), 0);
        if (deadMaxDamage >= 0 && o.damage > deadMaxDamage)
            o.damage = deadMaxDamage;                 // W8 item 7: a melee kill is not "the whole pool in one blow"
        o.respawn = true;
        o.report = o.damage > 0 && damagedByLocal;
        return o;
    }
    if (health <= 0)
        return o;                         // unreadable (a dead ped is the `dead` case above)
    if (health < pool)
        o.damage = pool - health;
    o.restore = health != pool;
    o.report = o.damage > 0 && damagedByLocal;
    return o;
}

// ---- hit reactions (sync plan W3, combat 2) -------------------------------------------------------

HitReaction HitReact::Decide(int damage, int maxHp, uint8_t flags, double sinceLastMs, const HitReactState& st,
                             const HitReactConfig& c)
{
    HitReaction r;
    if (damage <= 0 || maxHp <= 0 || st.dead || st.inVehicle || st.climbing)
        return r;
    const double pct = 100.0 * (double)damage / (double)maxHp;
    const bool explosion = (flags & kFlagExplosion) != 0;
    const bool melee = (flags & kFlagMelee) != 0;
    const bool heavy = explosion || pct >= c.knockdownPct || (melee && pct >= c.meleeKnockdownPct);
    if (heavy && st.sinceKnockdownMs >= c.knockdownImmunityMs)
    {
        int ms = c.ragdollBaseMs + c.ragdollMsPerDamage * damage;
        if (ms > c.ragdollMaxMs)
            ms = c.ragdollMaxMs;
        if (ms < 0)
            ms = 0;
        r.kind = ReactKind::Knockdown;
        r.ragdollMs = ms;
        return r;
    }
    if (sinceLastMs < c.flinchSpacingMs)
        return r;                                  // too soon after the last reaction
    if (melee && !explosion)
    {
        // W8: a melee blow staggers (short ragdoll), it never gets a flinch bullet.
        if (heavy || pct >= c.meleeStaggerMinPct)
        {
            int ms = c.staggerBaseMs + c.staggerMsPerDamage * damage;
            if (ms > c.staggerMaxMs)
                ms = c.staggerMaxMs;
            if (ms < c.staggerBaseMs)
                ms = c.staggerBaseMs;
            r.kind = ReactKind::Stagger;
            r.ragdollMs = ms;
        }
        return r;
    }
    if (heavy || pct >= c.flinchMinPct)
        r.kind = ReactKind::Flinch;                // a knockdown inside the immunity window is only a flinch
    return r;
}

void HitReactArbiter::Start(double nowMs, const HitReaction& r)
{
    if (r.kind == ReactKind::None)
        return;
    double window = windowMs_;
    if (r.kind == ReactKind::Knockdown || r.kind == ReactKind::Stagger)
    {
        const double kd = (double)r.ragdollMs + tailMs_;
        if (kd > window)
            window = kd;
        if (r.kind == ReactKind::Knockdown)        // only a knockdown starts the immunity window; a stagger does not
        {
            haveKd_ = true;
            lastKdAtMs_ = nowMs;
        }
    }
    const double until = nowMs + window;
    untilMs_ = active_ && untilMs_ > until ? untilMs_ : until;   // a reaction never shortens the one playing
    active_ = true;
    haveLast_ = true;
    lastAtMs_ = nowMs;
}

HitReactOut HitReactArbiter::Tick(double nowMs)
{
    HitReactOut out;
    if (!active_)
        return out;
    if (nowMs >= untilMs_)
    {
        active_ = false;
        out.ended = true;
        return out;
    }
    out.holds = true;
    return out;
}

void HitReactArbiter::Clear()
{
    active_ = haveLast_ = haveKd_ = false;
    untilMs_ = lastAtMs_ = lastKdAtMs_ = 0;
}

// ---- geometry -------------------------------------------------------------------------------------

// Closest points of two segments (Ericson, Real-Time Collision Detection 5.1.9): the shot a -> b and the
// player's vertical axis c0 -> c1.
bool SegmentTouchesCapsule(float ax, float ay, float az, float bx, float by, float bz,
                           float cx, float cy, float cz, float radius, float halfHeight)
{
    const double hh = halfHeight > 0.0f ? (double)halfHeight : 0.0;
    const double d1x = (double)bx - ax, d1y = (double)by - ay, d1z = (double)bz - az;
    const double d2z = 2.0 * hh;                        // the axis runs straight up: d2 = (0, 0, 2hh)
    const double rx = (double)ax - cx, ry = (double)ay - cy, rz = (double)az - ((double)cz - hh);
    const double a = d1x * d1x + d1y * d1y + d1z * d1z;
    const double e = d2z * d2z;
    const double f = d2z * rz;
    const double eps = 1e-12;
    double s = 0.0, t = 0.0;
    auto clamp01 = [](double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; };
    if (a <= eps && e <= eps)
    {
        s = t = 0.0;
    }
    else if (a <= eps)
    {
        s = 0.0;
        t = clamp01(f / e);
    }
    else
    {
        const double c = d1z * rz + d1x * rx + d1y * ry;
        if (e <= eps)
        {
            t = 0.0;
            s = clamp01(-c / a);
        }
        else
        {
            const double b = d1z * d2z;
            const double denom = a * e - b * b;
            s = denom > eps ? clamp01((b * f - c * e) / denom) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0)
            {
                t = 0.0;
                s = clamp01(-c / a);
            }
            else if (t > 1.0)
            {
                t = 1.0;
                s = clamp01((b - c) / a);
            }
        }
    }
    const double px = (double)ax + d1x * s, py = (double)ay + d1y * s, pz = (double)az + d1z * s;
    const double qx = (double)cx, qy = (double)cy, qz = ((double)cz - hh) + d2z * t;
    const double dx = px - qx, dy = py - qy, dz = pz - qz;
    const double r = (double)radius;
    return dx * dx + dy * dy + dz * dz <= r * r;
}

// ---- small engine decisions ---------------------------------------------------------------------

bool VitalsTickDue(bool serverMode, bool lifeAlive, bool lifeDying, bool serverDead)
{
    return serverMode && (lifeAlive || (lifeDying && !serverDead));
}

bool VehicleBlameAllowed(int localSeat, bool linkIsDriver)
{
    return localSeat == 0 && linkIsDriver;
}

// ---- VitalsInbox ------------------------------------------------------------------------------

void VitalsInbox::OnDamage(uint8_t health, uint8_t armour, uint16_t damage, bool selfInflicted, bool killed, uint16_t attacker,
                           uint32_t weapon, int16_t bone, uint8_t flags)
{
    s_.any = true;
    s_.health = health;
    s_.armour = armour;
    s_.damage += damage;
    ++s_.events;
    if (selfInflicted)
        s_.selfAckDamage += damage;
    else
    {
        ++s_.otherEvents;
        s_.otherDamage += damage;
        s_.lastAttacker = attacker;
        s_.lastWeapon = weapon;
        s_.lastBone = bone;
        s_.otherFlags = (uint8_t)(s_.otherFlags | flags);
    }
    s_.killed = s_.killed || killed;
}

void VitalsInbox::OnKilled()
{
    s_.any = true;
    s_.killed = true;
}

bool VitalsInbox::Take(DamageSlot& out)
{
    if (!s_.any)
        return false;
    out = s_;
    s_ = DamageSlot();
    return true;
}

}
