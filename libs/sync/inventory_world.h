// inventory_world.h (0.5.4 "inventory world", GI 2026-11-07): what lies around the player and what the ped holds, on top of the item rules in
// inventory.h. Pure C++17, no game, no network; the server runs World / Reconcile, the engine runs PlanSnapshot / PlanRestore, and
// tests/inventory_world_tests.h pins all of it (gameplay_test).
//
// 1. GROUND PILES. A Drop puts the stack into the nearest pile within Rules::groundReach of the dropper (2 m, BUILD_SPEC section 8), or a new
//    pile of 24 slots at the dropper's feet. A pile is listed in NEARBY to anyone within groundReach and anyone there may take from it. It
//    vanishes Rules::groundLifeMs (10 min) after the last thing was put in it, or as soon as it is empty. EPHEMERAL: piles live in the server's
//    memory only (a restart clears them), like the vehicles they sit next to.
// 2. TRUNKS. A server vehicle's trunk is a container keyed by the server's vehicle id (and its creation time, so a reused id never inherits
//    another car's trunk), sized by the GTA vehicle class a client reported (StorageForClass; until a class is known, or for a class with no
//    storage, no trunk is listed). It is in reach when the player SITS in the vehicle (any seat, driver or passenger) or stands within
//    Rules::trunkReach (4 m) of the vehicle's position, from any side: the server has the car's centre, not its boot, and a 4 m radius covers
//    behind and beside a sedan or an SUV. A LOCKED vehicle's trunk is listed as locked (no contents, every op refused) to everyone outside it
//    except its key holders (the spawner and the last driver: vehicle_lock.h HoldsKey). Ephemeral with the vehicle: when the vehicle goes
//    (despawned, destroyed, its owner left), whatever was in the trunk is dumped into a ground pile where the car last was.
// 3. WEAPONS (the reconcile). The server's inventory is the truth for which weapons a player owns; the ped's weapon wheel shows the weapons
//    the character carries (hand, pockets, pack) and the HAND is the ped's current weapon. Two directions, gated by a revision number:
//    * ped -> server (Report, Reconcile): every InvReport carries the rev of the last InvSnap the client APPLIED to its ped. The server
//      reconciles only a report whose rev is its current one (so a report taken before a drop, a move or a give reached the ped is never
//      read as "the ped still has it" = no duplicate). It then (a) adds a weapon the ped holds that the character does not carry (an admin
//      /gun, a pickup, a script) to the hand (when it is the current weapon) or the pockets / pack, or, with no room, drops it on the ground
//      and tells the client to strip it from the ped; (b) takes each ammo pool the ped reports; (c) consumes a thrown weapon the ped no
//      longer has when its count was 1 or 0 (the last grenade was thrown); (d) moves the ped's current weapon into the hand, or holsters
//      the hand when the ped is unarmed. A non-thrown weapon the ped lost is NOT removed: the client gives it back.
//    * server -> ped (PlanSnapshot, PlanRestore): on each new snapshot the client removes the weapons that left the character since the
//      previous snapshot (and the strip list), gives the ones it carries that the ped lacks, and, unless the snapshot came from its own
//      report, sets the changed ammo pools and selects the hand. Between snapshots a carried weapon the ped lost comes back (death, a
//      respawned ped), except a thrown one at 1 or 0 (just thrown: the server consumes it).
//    Fixed point: ped weapons == carried weapons, ped current == hand (or unarmed). The server changes the character only when a report
//    differs from it, and bumps rev only on a change, so a stable ped sends nothing and nothing loops.
//    AMMO: GTA keeps one pool per ammo TYPE (every pistol shares AMMO_PISTOL). So per ammo type the whole pool sits on ONE carried item
//    (the hand's if it is of that type, else the first in pockets, then pack) and the others of that type hold 0 (NormaliseAmmo): dropping
//    the holder takes the pool with it and the ped's pool goes to 0; dropping another takes none. Types come from the ped (the report).
#pragma once

#include "sync/inventory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace gage::sync::invw {

    namespace inv = gage::sync::inv;

    struct Vec3 { float x = 0, y = 0, z = 0; };
    inline float Dist(const Vec3& a, const Vec3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    struct Rules
    {
        float groundReach = 2.0f;          // m: a pile is listed (and a drop merges into it) within this distance
        float trunkReach = 4.0f;           // m from the vehicle's position; seated players always reach their vehicle's trunk
        uint64_t groundLifeMs = 600000;    // a pile goes this long after something was last put in it
        int groundSlots = 24;              // slots of a new pile (BUILD_SPEC: the ground grid)
        size_t maxPiles = 2048;            // more refuses the drop (the item stays with the player)
    };

    constexpr uint16_t kNoVehicle = 0xFFFF;
    constexpr uint16_t kNoPlayer = 0xFFFF;

    struct Pile { uint32_t id = 0; Vec3 pos; uint64_t touchedMs = 0; inv::Container box; bool timedOut = false; };
    struct Trunk { uint32_t id = 0; uint16_t veh = kNoVehicle; uint64_t born = 0; int cls = -1; Vec3 lastPos; inv::Container box; };

    // A server vehicle, as Nearby / Sweep need it.
    struct VehView
    {
        uint16_t id = kNoVehicle;
        uint64_t born = 0;            // its creation time: the trunk's identity with the id
        Vec3 pos;
        uint32_t model = 0;
        int cls = -1;                 // GTA vehicle class a client reported (-1 = none yet)
        bool locked = false;
        uint16_t spawner = kNoPlayer, lastDriver = kNoPlayer;   // the key holders
    };
    struct Viewer { uint16_t id = kNoPlayer; Vec3 pos; uint16_t seatedIn = kNoVehicle; };

    // One entry of a player's NEARBY panel.
    struct NearBox
    {
        uint32_t id = 0;
        inv::Kind kind = inv::Kind::Ground;
        bool locked = false;
        uint16_t veh = kNoVehicle;
        uint32_t model = 0;
        int cls = -1;
        uint8_t piles = 0;            // Ground: how many piles are in reach (the nearest one is shown)
        float dist = 0;
        const inv::Container* box = nullptr;   // the contents (null while locked)
    };

    class World
    {
    public:
        explicit World(Rules r = Rules()) : rules_(r) {}
        void SetRules(const Rules& r) { rules_ = r; }
        const Rules& rules() const { return rules_; }
        const std::vector<Pile>& Piles() const { return piles_; }
        const std::vector<Trunk>& Trunks() const { return trunks_; }

        // Put the stack s on the ground at `at` (a Drop, a weapon with no room, a despawned trunk). Merges into the nearest piles in reach
        // with room, the rest into a new pile. Returns the pile that took the last of it, 0 = refused (Rules::maxPiles): nothing was placed.
        uint32_t Drop(const Vec3& at, const inv::Slot& s, uint64_t nowMs)
        {
            if (s.Empty() || !inv::Find(s.item))
                return 0;
            uint32_t left = s.qty, last = 0;
            std::vector<std::pair<float, size_t>> order;
            for (size_t i = 0; i < piles_.size(); ++i) {
                const float d = Dist(piles_[i].pos, at);
                if (d <= rules_.groundReach)
                    order.push_back({d, i});
            }
            std::sort(order.begin(), order.end());
            for (const auto& o : order) {
                Pile& p = piles_[o.second];
                const uint32_t in = inv::Add(p.box, s.item, left, s.cond, s.ammo);
                if (in) {
                    p.touchedMs = nowMs;
                    p.timedOut = false;
                    last = p.id;
                    left -= in;
                }
                if (!left)
                    return last;
            }
            if (piles_.size() >= rules_.maxPiles)
                return 0;
            Pile p;
            p.id = NextId();
            p.pos = at;
            p.touchedMs = nowMs;
            p.box.kind = inv::Kind::Ground;
            p.box.maxGrams = 0;
            p.box.slots.resize((size_t)std::max(1, std::min(rules_.groundSlots, inv::kMaxBoxSlots)));
            // the rest is at most one stack and a new pile has no weight cap: one empty slot takes it whole
            inv::Add(p.box, s.item, left, s.cond, s.ammo);
            piles_.push_back(p);
            return p.id;
        }
        // Would Drop place all of s? (the server asks before it commits a Drop op).
        bool CanDrop(const Vec3& at, const inv::Slot& s) const
        {
            if (s.Empty() || !inv::Find(s.item))
                return false;
            if (piles_.size() < rules_.maxPiles)
                return true;
            uint32_t room = 0;
            for (const Pile& p : piles_)
                if (Dist(p.pos, at) <= rules_.groundReach) {
                    inv::Container c = p.box;
                    room += inv::Add(c, s.item, s.qty, s.cond, s.ammo);
                }
            return room >= s.qty;
        }

        // The container behind an id (a pile or a trunk), null when gone.
        inv::Container* Box(uint32_t id)
        {
            for (Pile& p : piles_)
                if (p.id == id)
                    return &p.box;
            for (Trunk& t : trunks_)
                if (t.id == id)
                    return &t.box;
            return nullptr;
        }
        Pile* FindPile(uint32_t id)
        {
            for (Pile& p : piles_)
                if (p.id == id)
                    return &p;
            return nullptr;
        }
        // The trunk of v (created with its class's size on first use). Null when its class is unknown or has no storage.
        Trunk* TrunkOf(const VehView& v)
        {
            for (Trunk& t : trunks_)
                if (t.veh == v.id && t.born == v.born)
                    return &t;
            if (v.cls < 0)
                return nullptr;
            const inv::VehicleStorage st = inv::StorageForClass(v.cls);
            if (st.trunkSlots <= 0)
                return nullptr;
            Trunk t;
            t.id = NextId();
            t.veh = v.id;
            t.born = v.born;
            t.cls = v.cls;
            t.lastPos = v.pos;
            t.box.kind = inv::Kind::Trunk;
            t.box.maxGrams = st.trunkGrams;
            t.box.slots.resize((size_t)std::min(st.trunkSlots, inv::kMaxBoxSlots));
            trunks_.push_back(t);
            return &trunks_.back();
        }

        // Once in a while (the server: every 250 ms): expired and empty piles go; the trunk of a vehicle that is no longer in `vehicles` (or
        // whose id now belongs to a newer vehicle) is emptied into a pile where it last stood; the trunks of the rest learn their position.
        struct SweepOut { int expired = 0, emptied = 0, trunksDumped = 0, stacksDumped = 0, stacksLost = 0; };
        SweepOut Sweep(uint64_t nowMs, const std::vector<VehView>& vehicles)
        {
            SweepOut out;
            for (size_t i = 0; i < trunks_.size();) {
                Trunk& t = trunks_[i];
                const VehView* v = nullptr;
                for (const VehView& x : vehicles)
                    if (x.id == t.veh && x.born == t.born)
                        v = &x;
                if (v) {
                    t.lastPos = v->pos;
                    ++i;
                    continue;
                }
                const Trunk gone = t;
                trunks_.erase(trunks_.begin() + (std::ptrdiff_t)i);
                out.trunksDumped++;
                for (const inv::Slot& s : gone.box.slots)
                    if (!s.Empty()) {
                        if (Drop(gone.lastPos, s, nowMs))
                            out.stacksDumped++;
                        else
                            out.stacksLost++;
                    }
            }
            for (size_t i = 0; i < piles_.size();) {
                const Pile& p = piles_[i];
                const bool empty = inv::UsedSlots(p.box) == 0;
                // unsigned age that cannot go negative: a pile touched "after" now (a caller's clock read before the drop) is fresh
                const uint64_t age = nowMs > p.touchedMs ? nowMs - p.touchedMs : 0;
                if (empty || p.timedOut || age >= rules_.groundLifeMs) {
                    (empty ? out.emptied : out.expired)++;
                    piles_.erase(piles_.begin() + (std::ptrdiff_t)i);
                } else {
                    ++i;
                }
            }
            return out;
        }
        // Tests: every pile counts as timed out at the next Sweep (as if its lifetime had passed; the clock itself is not touched).
        void ExpireAll()
        {
            for (Pile& p : piles_)
                p.timedOut = true;
        }
        // An op just emptied a pile: it goes now (not at the next sweep), so nobody is shown an empty pile.
        void DropIfEmpty(uint32_t id)
        {
            for (size_t i = 0; i < piles_.size(); ++i)
                if (piles_[i].id == id && inv::UsedSlots(piles_[i].box) == 0) {
                    piles_.erase(piles_.begin() + (std::ptrdiff_t)i);
                    return;
                }
        }

        // What one player's NEARBY shows: the trunk of the vehicle it sits in, else of the nearest vehicle within trunkReach that has one;
        // and the nearest ground pile within groundReach (with the number of piles in reach). At most these two.
        std::vector<NearBox> Nearby(const Viewer& who, const std::vector<VehView>& vehicles)
        {
            std::vector<NearBox> out;
            const VehView* best = nullptr;
            float bestD = 0;
            for (const VehView& v : vehicles) {
                const bool seated = who.seatedIn == v.id;
                const float d = seated ? 0.0f : Dist(who.pos, v.pos);
                if (!seated && d > rules_.trunkReach)
                    continue;
                if (v.cls < 0 || inv::StorageForClass(v.cls).trunkSlots <= 0)
                    continue;   // no trunk to show (class not reported yet, or a bicycle / boat / aircraft)
                if (!best || d < bestD) {
                    best = &v;
                    bestD = d;
                }
            }
            if (best) {
                if (Trunk* t = TrunkOf(*best)) {
                    NearBox b;
                    b.id = t->id;
                    b.kind = inv::Kind::Trunk;
                    b.veh = best->id;
                    b.model = best->model;
                    b.cls = t->cls;
                    b.dist = bestD;
                    const bool holdsKey = who.id != kNoPlayer && (best->spawner == who.id || best->lastDriver == who.id);
                    b.locked = best->locked && who.seatedIn != best->id && !holdsKey;
                    b.box = b.locked ? nullptr : &t->box;
                    out.push_back(b);
                }
            }
            const Pile* nearest = nullptr;
            float nd = 0;
            uint8_t inReach = 0;
            for (const Pile& p : piles_) {
                const float d = Dist(who.pos, p.pos);
                if (d > rules_.groundReach)
                    continue;
                if (inReach < 255)
                    inReach++;
                if (!nearest || d < nd) {
                    nearest = &p;
                    nd = d;
                }
            }
            if (nearest) {
                NearBox b;
                b.id = nearest->id;
                b.kind = inv::Kind::Ground;
                b.piles = inReach;
                b.dist = nd;
                b.box = &nearest->box;
                out.push_back(b);
            }
            return out;
        }
        // The entry of `id` in who's NEARBY now, or null (out of reach, gone): the server's check before any op on a world container.
        static const NearBox* InReach(const std::vector<NearBox>& list, uint32_t id)
        {
            for (const NearBox& b : list)
                if (b.id == id)
                    return &b;
            return nullptr;
        }

    private:
        uint32_t NextId()
        {
            if (++nextId_ == 0)
                nextId_ = 1;
            return nextId_;
        }
        Rules rules_;
        std::vector<Pile> piles_;
        std::vector<Trunk> trunks_;
        uint32_t nextId_ = 0;
    };

    // ---- weapons ---------------------------------------------------------------------------------------------------------------------------
    struct PedWeapon { uint32_t hash = 0; uint32_t ammoType = 0; uint16_t ammo = 0; };
    // What the client read off its ped, taken after it applied snapshot `rev`.
    struct Report
    {
        uint32_t rev = 0;
        uint32_t current = 0;    // GET_SELECTED_PED_WEAPON (kWeaponUnarmed / 0 = nothing in hand)
        bool valid = true;       // false: the ped is dead, gone or new (nothing in this report is read)
        std::vector<PedWeapon> weapons;   // catalog weapons only (the client checks every WeaponCatalog() hash)
    };
    // weapon hash -> GTA ammo type, learned from the ped (0 = none / unknown).
    struct AmmoTypes
    {
        std::vector<std::pair<uint32_t, uint32_t>> m;
        uint32_t Of(uint32_t hash) const
        {
            for (const auto& kv : m)
                if (kv.first == hash)
                    return kv.second;
            return 0;
        }
        void Learn(uint32_t hash, uint32_t type)
        {
            if (!hash || !type)
                return;
            for (auto& kv : m)
                if (kv.first == hash) {
                    kv.second = type;
                    return;
                }
            m.push_back({hash, type});
        }
    };
    // The ammo group of a weapon: its ammo type, or the weapon itself when the type is not known (melee has none).
    inline uint64_t GroupOf(uint32_t hash, const AmmoTypes& t)
    {
        const uint32_t type = t.Of(hash);
        return type ? (uint64_t)type : (0x100000000ull | hash);
    }

    // Every carried weapon slot, in holder order: the hand, the pockets, the pack.
    inline std::vector<inv::Slot*> CarriedWeapons(inv::Character& ch)
    {
        std::vector<inv::Slot*> v;
        if (!ch.hand.Empty() && inv::IsWeapon(ch.hand.item))
            v.push_back(&ch.hand);
        for (inv::Slot& s : ch.pockets.slots)
            if (!s.Empty() && inv::IsWeapon(s.item))
                v.push_back(&s);
        if (ch.HasPack())
            for (inv::Slot& s : ch.pack.slots)
                if (!s.Empty() && inv::IsWeapon(s.item))
                    v.push_back(&s);
        return v;
    }
    inline uint32_t HashOf(const inv::Slot& s) { const inv::ItemDef* d = inv::Find(s.item); return d ? d->weapon : 0; }
    inline bool Carries(const inv::Character& ch, uint32_t hash)
    {
        for (const inv::Slot* s : CarriedWeapons(const_cast<inv::Character&>(ch)))
            if (HashOf(*s) == hash)
                return true;
        return false;
    }
    // The total rounds of each ammo group the character carries.
    inline std::vector<std::pair<uint64_t, uint32_t>> GroupTotals(const inv::Character& ch, const AmmoTypes& t)
    {
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (const inv::Slot* s : CarriedWeapons(const_cast<inv::Character&>(ch))) {
            const uint64_t g = GroupOf(HashOf(*s), t);
            bool found = false;
            for (auto& kv : v)
                if (kv.first == g) {
                    kv.second += s->ammo;
                    found = true;
                }
            if (!found)
                v.push_back({g, s->ammo});
        }
        return v;
    }
    inline uint32_t GroupTotalOf(const inv::Character& ch, uint32_t hash, const AmmoTypes& t)
    {
        const uint64_t g = GroupOf(hash, t);
        for (const auto& kv : GroupTotals(ch, t))
            if (kv.first == g)
                return kv.second;
        return 0;
    }
    // Per ammo group, the whole pool on the first carried item (hand, pockets, pack), 0 on the others. totals (optional) overrides a group's
    // total (the ped's pool from a report).
    inline void NormaliseAmmo(inv::Character& ch, const AmmoTypes& t, const std::vector<std::pair<uint64_t, uint32_t>>* totals = nullptr)
    {
        std::vector<std::pair<uint64_t, uint32_t>> sum = GroupTotals(ch, t);
        if (totals)
            for (auto& kv : sum)
                for (const auto& o : *totals)
                    if (o.first == kv.first)
                        kv.second = o.second;
        std::vector<uint64_t> held;
        for (inv::Slot* s : CarriedWeapons(ch)) {
            const uint64_t g = GroupOf(HashOf(*s), t);
            if (std::find(held.begin(), held.end(), g) != held.end()) {
                s->ammo = 0;
                continue;
            }
            held.push_back(g);
            uint32_t total = 0;
            for (const auto& kv : sum)
                if (kv.first == g)
                    total = kv.second;
            s->ammo = (uint16_t)std::min<uint32_t>(total, 65535u);
        }
    }

    struct ReconcileOut
    {
        bool changed = false;
        std::vector<inv::Slot> overflow;   // gained weapons with no room: the server drops them on the ground ...
        std::vector<uint32_t> strip;       // ... and the client removes them from the ped
        int gains = 0, ammoSet = 0, consumed = 0, toHand = 0, holstered = 0;
    };
    // The server's half (see the header). The caller has already checked r.rev against its own and r.valid.
    inline ReconcileOut Reconcile(inv::Character& ch, const Report& r, AmmoTypes& types)
    {
        ReconcileOut out;
        const std::string before = inv::Encode(ch);
        for (const PedWeapon& w : r.weapons)
            types.Learn(w.hash, w.ammoType);
        const auto reported = [&](uint32_t hash) {
            for (const PedWeapon& w : r.weapons)
                if (w.hash == hash)
                    return true;
            return false;
        };
        // (a) gains
        for (const PedWeapon& w : r.weapons) {
            const inv::ItemDef* d = inv::FindWeapon(w.hash);
            if (!d || Carries(ch, w.hash))
                continue;
            inv::Slot s;
            s.item = d->id;
            s.qty = 1;
            out.gains++;
            if (w.hash == r.current && ch.hand.Empty()) {
                ch.hand = s;
                continue;
            }
            if (inv::Give(ch, d->id, 1) == 1)
                continue;
            out.overflow.push_back(s);
            out.strip.push_back(w.hash);
        }
        // (c) a thrown weapon the ped no longer has, at 1 or 0: thrown, consumed. Anything else the ped lost stays (the client gives it back).
        for (inv::Slot* s : CarriedWeapons(ch)) {
            const inv::ItemDef* d = inv::Find(s->item);
            if (d && inv::IsThrown(*d) && !reported(d->weapon) && GroupTotalOf(ch, d->weapon, types) <= 1) {
                s->Clear();
                out.consumed++;
            }
        }
        // (b) the ped's pools (one value per group; the stripped overflow is not carried, so its pool is not ours)
        std::vector<std::pair<uint64_t, uint32_t>> pools;
        for (const PedWeapon& w : r.weapons) {
            if (!Carries(ch, w.hash))
                continue;
            const uint64_t g = GroupOf(w.hash, types);
            bool found = false;
            for (auto& kv : pools)
                if (kv.first == g) {
                    kv.second = std::max<uint32_t>(kv.second, w.ammo);
                    found = true;
                }
            if (!found)
                pools.push_back({g, w.ammo});
        }
        // (d) the hand follows the ped's current weapon
        const inv::ItemDef* cur = inv::FindWeapon(r.current);
        if (cur) {
            if (HashOf(ch.hand) != cur->weapon) {
                for (inv::Container* c : {&ch.pockets, ch.HasPack() ? &ch.pack : (inv::Container*)nullptr}) {
                    if (!c)
                        continue;
                    bool done = false;
                    for (size_t i = 0; i < c->slots.size() && !done; ++i)
                        if (!c->slots[i].Empty() && HashOf(c->slots[i]) == cur->weapon) {
                            if (inv::Ok(inv::HandFrom(ch, *c, (int)i)))
                                out.toHand++;
                            done = true;
                        }
                    if (done)
                        break;
                }
            }
        } else if (!ch.hand.Empty()) {
            if (inv::Ok(inv::HolsterAnywhere(ch)))
                out.holstered++;
        }
        const std::vector<std::pair<uint64_t, uint32_t>> totalsBefore = GroupTotals(ch, types);
        NormaliseAmmo(ch, types, &pools);
        for (const auto& kv : pools)
            for (const auto& b : totalsBefore)
                if (b.first == kv.first && b.second != kv.second)
                    out.ammoSet++;
        out.changed = inv::Encode(ch) != before;
        return out;
    }

    // ---- the client's half ------------------------------------------------------------------------------------------------------------------
    struct PedState
    {
        bool valid = false;               // alive and not just (re)created
        uint32_t current = 0;
        std::vector<PedWeapon> have;      // catalog weapons on the ped
        bool Has(uint32_t hash) const
        {
            for (const PedWeapon& w : have)
                if (w.hash == hash)
                    return true;
            return false;
        }
    };
    enum class ActK : uint8_t { Remove, Give, SetAmmo, Select };
    struct Act { ActK k; uint32_t hash; int ammo; };

    inline std::vector<uint32_t> CarriedHashes(const inv::Character& ch)
    {
        std::vector<uint32_t> v;
        for (const inv::Slot* s : CarriedWeapons(const_cast<inv::Character&>(ch))) {
            const uint32_t h = HashOf(*s);
            if (std::find(v.begin(), v.end(), h) == v.end())
                v.push_back(h);
        }
        return v;
    }

    // A new snapshot `cur` (prev = the one applied before it, null for the first of a session). In order: removes, gives, ammo, select.
    inline std::vector<Act> PlanSnapshot(const inv::Character* prev, const inv::Character& cur, bool fromReport, const std::vector<uint32_t>& strip,
                                         const PedState& ped, const AmmoTypes& types)
    {
        std::vector<Act> acts;
        if (!ped.valid)
            return acts;
        const std::vector<uint32_t> now = CarriedHashes(cur);
        const std::vector<uint32_t> before = prev ? CarriedHashes(*prev) : std::vector<uint32_t>();
        const auto in = [](const std::vector<uint32_t>& v, uint32_t h) { return std::find(v.begin(), v.end(), h) != v.end(); };
        for (const PedWeapon& w : ped.have)
            if (!in(now, w.hash) && (in(strip, w.hash) || in(before, w.hash)))
                acts.push_back({ActK::Remove, w.hash, 0});
        // A weapon given to the ped gets its group's rounds (GTA keeps one pool per ammo type, so this sets the type's pool to what the
        // inventory holds for it). On a snapshot that is not the answer to our own report, every group whose total changed is set too
        // (a pickup, a drop, a /gun); on our own report's answer the ped's pools ARE the source, so they are left alone.
        std::vector<uint64_t> setGroups;
        for (uint32_t h : now)
            if (!ped.Has(h)) {
                acts.push_back({ActK::Give, h, 0});
                setGroups.push_back(GroupOf(h, types));
            }
        const auto totNow = GroupTotals(cur, types);
        if (!fromReport) {
            const auto totBefore = prev ? GroupTotals(*prev, types) : std::vector<std::pair<uint64_t, uint32_t>>();
            for (const auto& kv : totNow) {
                int b = -1;
                for (const auto& o : totBefore)
                    if (o.first == kv.first)
                        b = (int)o.second;
                if ((int)kv.second != b)
                    setGroups.push_back(kv.first);
            }
        }
        std::vector<uint64_t> done;
        for (uint32_t h : now) {
            const uint64_t g = GroupOf(h, types);
            if (std::find(setGroups.begin(), setGroups.end(), g) == setGroups.end() || std::find(done.begin(), done.end(), g) != done.end())
                continue;
            done.push_back(g);
            uint32_t a = 0;
            for (const auto& kv : totNow)
                if (kv.first == g)
                    a = kv.second;
            acts.push_back({ActK::SetAmmo, h, (int)a});
        }
        if (!fromReport) {
            const uint32_t want = HashOf(cur.hand) ? HashOf(cur.hand) : inv::kWeaponUnarmed;
            const uint32_t wasHand = prev ? (HashOf(prev->hand) ? HashOf(prev->hand) : inv::kWeaponUnarmed) : 0;
            const uint32_t pedCur = ped.current ? ped.current : inv::kWeaponUnarmed;
            if (want != wasHand && want != pedCur)
                acts.push_back({ActK::Select, want, 0});
        }
        return acts;
    }
    // Between snapshots: carried weapons the ped lacks come back with their group's rounds, except a thrown one at 1 or 0.
    inline std::vector<Act> PlanRestore(const inv::Character& cur, const PedState& ped, const AmmoTypes& types)
    {
        std::vector<Act> acts;
        if (!ped.valid)
            return acts;
        for (uint32_t h : CarriedHashes(cur)) {
            if (ped.Has(h))
                continue;
            const inv::ItemDef* d = inv::FindWeapon(h);
            if (d && inv::IsThrown(*d) && GroupTotalOf(cur, h, types) <= 1)
                continue;
            acts.push_back({ActK::Give, h, 0});
            acts.push_back({ActK::SetAmmo, h, (int)GroupTotalOf(cur, h, types)});
        }
        return acts;
    }
    inline Report BuildReport(uint32_t rev, const PedState& ped)
    {
        Report r;
        r.rev = rev;
        r.current = ped.current;
        r.valid = ped.valid;
        r.weapons = ped.have;
        return r;
    }
    // The report form of the overlay-free comparison the client uses to send only on a change: presence and current exactly, ammo only
    // when allowAmmo (the engine sends an ammo-only change at most every 2 s).
    inline bool SameForReport(const PedState& a, const PedState& b, bool compareAmmo)
    {
        if (a.valid != b.valid || a.current != b.current || a.have.size() != b.have.size())
            return false;
        for (size_t i = 0; i < a.have.size(); ++i)
            if (a.have[i].hash != b.have[i].hash || (compareAmmo && a.have[i].ammo != b.have[i].ammo))
                return false;
        return true;
    }

    // Does the ped differ from the inventory `cur` (the snapshot applied to it)? presence: a weapon too many or too few, or another weapon
    // selected than the hand holds; ammo: a carried weapon's pool differs from its group's total.
    inline void DiffFromInventory(const PedState& ped, const inv::Character& cur, const AmmoTypes& types, bool& presence, bool& ammo)
    {
        presence = ammo = false;
        const std::vector<uint32_t> carried = CarriedHashes(cur);
        if (carried.size() != ped.have.size())
            presence = true;
        for (uint32_t h : carried)
            if (!ped.Has(h))
                presence = true;
        const inv::ItemDef* sel = inv::FindWeapon(ped.current);
        if ((sel ? sel->weapon : 0u) != HashOf(cur.hand))
            presence = true;
        for (const PedWeapon& w : ped.have)
            if (Carries(cur, w.hash) && GroupTotalOf(cur, w.hash, types) != w.ammo)
                ammo = true;
    }

    // When the client reports (the engine's TickInventoryWorld asks every read, ~4 Hz). The first report answers the server's offer. Then:
    // with no newer snapshot applied since the last report, a ped that changed since that report (presence at once but kPresenceGapMs apart,
    // ammo only kAmmoGapMs apart); once a newer snapshot was applied, a ped that still differs from it (the earlier report was stale at the
    // server: its rev gate dropped it), else nothing and the gate catches up. A stable ped that agrees with the inventory never reports: no loop.
    struct ReportGate
    {
        static constexpr double kPresenceGapMs = 250, kAmmoGapMs = 2000;
        bool sentOnce = false;
        PedState lastSent;
        uint32_t lastSentRev = 0;
        double lastSentMs = -1e9, lastAmmoSentMs = -1e9;

        // ped: valid (the caller does not ask otherwise); cur: the applied snapshot (null before the first); appliedAtMs: when it was applied.
        bool Due(const PedState& ped, uint32_t rev, const inv::Character* cur, double appliedAtMs, const AmmoTypes& types, double nowMs)
        {
            if (!sentOnce)
                return true;
            if (lastSentRev == rev) {
                const bool presence = !SameForReport(ped, lastSent, false);
                const bool ammo = !SameForReport(ped, lastSent, true);
                return (presence && nowMs - lastSentMs >= kPresenceGapMs) || (ammo && nowMs - lastAmmoSentMs >= kAmmoGapMs);
            }
            if (!cur || nowMs - appliedAtMs < kPresenceGapMs)
                return false;   // let the ped take the snapshot first
            bool presence = false, ammo = false;
            DiffFromInventory(ped, *cur, types, presence, ammo);
            if (presence || (ammo && nowMs - lastAmmoSentMs >= kAmmoGapMs))
                return true;
            if (!presence && !ammo)
                lastSentRev = rev;   // the ped agrees with the new snapshot: nothing to tell
            return false;
        }
        void Sent(const PedState& ped, uint32_t rev, double nowMs)
        {
            if (!sentOnce || !SameForReport(ped, lastSent, true))
                lastAmmoSentMs = nowMs;
            sentOnce = true;
            lastSent = ped;
            lastSentRev = rev;
            lastSentMs = nowMs;
        }
    };

} // namespace gage::sync::invw
