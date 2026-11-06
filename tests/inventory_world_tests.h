// 0.5.4 inventory world (libs/sync/inventory_world.h and the weapon / hand / codec v2 parts of libs/sync/inventory.h), GI 2026-11-07: "any gun
// ... goes into inventory ... active hand slot", "remove dummy items", "anything from inv should be dropped on the ground and be available for
// pickup in nearby", "car trunk isn't being shown when in / near a car". Included by tests/gameplay_test.cpp after inventory_tests.h; run by
// invworld::RunPure(), kChecks checks. The wire half (the real server and session) is tests/inventory_world_net_tests.h in server_test.
#pragma once

#include "sync/inventory_world.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace invworld {

    using namespace gage::sync::inv;
    namespace invw = gage::sync::invw;
    namespace inv = gage::sync::inv;

    constexpr int kChecks = 15;

    inline ItemId Id(const char* key) { const ItemDef* d = FindKey(key); return d ? d->id : 0; }
    inline uint32_t H(const char* key) { return inv::Joaat(key); }
    inline uint32_t QtyOf(const Character& ch, ItemId id)
    {
        uint32_t n = 0;
        for (const Slot& s : ch.pockets.slots) n += !s.Empty() && s.item == id ? s.qty : 0;
        return n;
    }
    inline Slot W(const char* key, uint16_t ammo = 0) { Slot s; s.item = Id(key); s.qty = 1; s.ammo = ammo; return s; }
    inline Op MkOp(OpKind k, Where src, int si, Where dst = Where::Pockets, int di = 0, uint32_t qty = 0, uint32_t sb = 0, uint32_t db = 0)
    {
        Op o;
        o.kind = k; o.src = src; o.si = (int16_t)si; o.dst = dst; o.di = (int16_t)di; o.qty = qty; o.srcBox = sb; o.dstBox = db;
        return o;
    }

    // ---- a two-sided simulation of the reconcile: the server's copy + rev, the client's ped, the messages between them ----
    // GTA's ammo: one pool per ammo type; every ped weapon of a type shows that pool. The sim's types: the pistols share 1, the knife has none,
    // the grenade is its own 3 (its pool is the count).
    inline uint32_t SimType(uint32_t hash)
    {
        if (hash == H("weapon_pistol") || hash == H("weapon_combatpistol")) return 0xA1;
        if (hash == H("weapon_grenade")) return 0xA3;
        if (hash == H("weapon_carbinerifle")) return 0xA4;
        return 0;
    }
    struct Snap { uint32_t rev; Character ch; bool fromReport; std::vector<uint32_t> strip; };
    struct Sim
    {
        bool revGate = true;           // the rule under test: a report is reconciled only when stamped with the server's current rev
        // server
        Character ch;
        uint32_t rev = 0;
        std::string last;
        invw::AmmoTypes srvTypes;
        invw::World world;
        std::vector<Snap> toClient;    // in flight, in order
        int stale = 0, snaps = 0, reconciled = 0;
        // client
        invw::PedState ped;
        uint32_t applied = 0;
        Character prev;
        bool hasPrev = false;
        invw::AmmoTypes cliTypes;
        std::vector<invw::Report> toServer;
        int reports = 0;
        std::map<uint32_t, int> pools;   // ammo type -> rounds (the ped's pools)

        Sim() { ped.valid = true; ped.current = inv::kWeaponUnarmed; }
        void SendSnap(bool fromReport, const std::vector<uint32_t>& strip)
        {
            const std::string b = Encode(ch);
            if (b != last) { rev++; last = b; }
            toClient.push_back({rev, ch, fromReport, strip});
            snaps++;
        }
        // ---- the ped ----
        void PedSync()   // every ped weapon shows its type's pool
        {
            for (invw::PedWeapon& w : ped.have)
                if (SimType(w.hash)) w.ammo = (uint16_t)pools[SimType(w.hash)];
        }
        void PedGive(uint32_t hash, int ammo, bool select)
        {
            if (!ped.Has(hash)) ped.have.push_back({hash, SimType(hash), 0});
            if (SimType(hash)) pools[SimType(hash)] += ammo;
            if (select) ped.current = hash;
            PedSync();
        }
        void PedRemove(uint32_t hash)
        {
            for (size_t i = 0; i < ped.have.size(); i++)
                if (ped.have[i].hash == hash) { ped.have.erase(ped.have.begin() + (long)i); break; }
            if (ped.current == hash) ped.current = inv::kWeaponUnarmed;
        }
        void PedFire(uint32_t type, int n) { pools[type] = std::max(0, pools[type] - n); PedSync(); }
        void Act(const std::vector<invw::Act>& acts)
        {
            for (const invw::Act& a : acts) {
                if (a.k == invw::ActK::Remove) PedRemove(a.hash);
                else if (a.k == invw::ActK::Give) PedGive(a.hash, 0, false);
                else if (a.k == invw::ActK::SetAmmo) { if (SimType(a.hash)) pools[SimType(a.hash)] = a.ammo; PedSync(); }
                else if (a.k == invw::ActK::Select) { if (a.hash == inv::kWeaponUnarmed || ped.Has(a.hash)) ped.current = a.hash; }
            }
        }
        // ---- the client ----
        void Learn() { for (const invw::PedWeapon& w : ped.have) cliTypes.Learn(w.hash, w.ammoType); }
        bool Consistent() const
        {
            if (!hasPrev) return false;
            std::vector<uint32_t> carried = invw::CarriedHashes(prev), have;
            for (const invw::PedWeapon& w : ped.have) have.push_back(w.hash);
            std::sort(carried.begin(), carried.end());
            std::sort(have.begin(), have.end());
            const uint32_t hand = invw::HashOf(prev.hand) ? invw::HashOf(prev.hand) : inv::kWeaponUnarmed;
            if (carried != have || hand != (ped.current ? ped.current : inv::kWeaponUnarmed)) return false;
            for (const invw::PedWeapon& w : ped.have)
                if (invw::GroupTotalOf(prev, w.hash, cliTypes) != w.ammo) return false;
            return true;
        }
        // The engine's rule (invw::ReportGate, as TickInventoryWorld uses it): report when the ped changed since the last report, or when a
        // newer snapshot came in and the ped still disagrees with it (a report the server ignored as stale is then repeated with the new rev).
        // The sim's clock moves 3 s per call, so the gate's 250 ms / 2 s gaps never bind here (the gate's own check pins them).
        invw::ReportGate gate;
        double clock = 0, appliedAt = -1e9;
        void MaybeReport()
        {
            Learn();
            clock += 3000;
            if (!gate.Due(ped, applied, hasPrev ? &prev : nullptr, appliedAt, cliTypes, clock)) return;
            toServer.push_back(invw::BuildReport(applied, ped));
            gate.Sent(ped, applied, clock);
            reports++;
        }
        void ClientApplyOne()
        {
            const Snap s = toClient.front();
            toClient.erase(toClient.begin());
            Learn();
            Act(invw::PlanSnapshot(hasPrev ? &prev : nullptr, s.ch, s.fromReport, s.strip, ped, cliTypes));
            prev = s.ch;
            hasPrev = true;
            applied = s.rev;
            appliedAt = clock;
            MaybeReport();
        }
        // ---- the server ----
        void ServerTakeOne()
        {
            const invw::Report r = toServer.front();
            toServer.erase(toServer.begin());
            if (!r.valid || (revGate && r.rev != rev)) { stale++; return; }
            const invw::ReconcileOut out = invw::Reconcile(ch, r, srvTypes);
            reconciled++;
            for (const Slot& s : out.overflow) world.Drop({0, 0, 0}, s, 1000);
            if (out.changed || !out.strip.empty()) SendSnap(true, out.strip);
        }
        bool ServerOp(const Op& op)   // an op of the player's (the overlay), applied like OnInvOp / WorldOp
        {
            Character t = ch;
            const ApplyOut out = Apply(t, op);
            if (!Ok(out.r)) { SendSnap(false, {}); return false; }
            ch = t;
            if (out.r == Result::Dropped) world.Drop({0, 0, 0}, out.dropped, 1000);
            invw::NormaliseAmmo(ch, srvTypes);
            SendSnap(false, {});
            return true;
        }
        // Deliver everything until nothing moves; returns the messages exchanged.
        int Settle(int cap = 200)
        {
            int n = 0;
            while ((!toClient.empty() || !toServer.empty()) && n < cap) {
                if (!toServer.empty()) { ServerTakeOne(); n++; }
                if (!toClient.empty()) { ClientApplyOne(); n++; }
            }
            return n;
        }
        // How many of `hash` exist anywhere: the character, the ped (when the character does not account for it), the ground.
        int Count(uint32_t hash) const
        {
            int n = 0;
            for (const Slot* s : invw::CarriedWeapons(const_cast<Character&>(ch))) n += invw::HashOf(*s) == hash ? 1 : 0;
            for (const invw::Pile& p : world.Piles())
                for (const Slot& s : p.box.slots) n += !s.Empty() && invw::HashOf(s) == hash ? 1 : 0;
            return n;
        }
    };

    // The dupe race: the ped holds a pistol the server knows; the client sends a report (rev R) after firing; before it arrives the player
    // drops the pistol (rev R+1); then the stale report lands. Returns the pistols in existence afterwards (character + ground).
    inline int DropRace(bool gate, std::string* trace)
    {
        Sim s;
        s.revGate = gate;
        s.PedGive(H("weapon_pistol"), 250, true);
        s.MaybeReport();
        s.Settle();
        const int afterGain = s.Count(H("weapon_pistol"));
        s.PedFire(0xA1, 50);
        s.MaybeReport();                                  // in flight with rev R
        s.ServerOp(MkOp(OpKind::Drop, Where::Hand, 0));    // the drop overtakes it (R + 1)
        s.ServerTakeOne();                                 // the stale report arrives
        s.Settle();
        if (trace)
            *trace = F("gain %d stale %d reconciled %d char %d ground piles %zu ped has %d", afterGain, s.stale, s.reconciled,
                       (int)invw::Carries(s.ch, H("weapon_pistol")), s.world.Piles().size(), (int)s.ped.Has(H("weapon_pistol")));
        return s.Count(H("weapon_pistol")) + (s.ped.Has(H("weapon_pistol")) && !invw::Carries(s.ch, H("weapon_pistol")) ? 1 : 0);
    }

    inline void RunPure()
    {
        {
            // GTA's hashes: joaat of the lower-cased name (pins from the game's own tables: WEAPON_PISTOL 0x1B06D571, WEAPON_UNARMED
            // 0xA2719263, WEAPON_KNIFE 0x99B507EA); the weapon catalog: ids from 1000, unique ids / keys / hashes, none shared with the 0.6
            // catalog, stack 1, one of three glyphs, FindWeapon finds each by its hash
            bool ok = inv::Joaat("weapon_pistol") == 0x1B06D571u && inv::kWeaponUnarmed == 0xA2719263u && inv::Joaat("WEAPON_KNIFE") == 0x99B507EAu &&
                      inv::Joaat("WEAPON_KNIFE") == inv::Joaat("weapon_knife");
            std::set<int> ids;
            std::set<std::string> keys;
            std::set<uint32_t> hashes;
            for (const ItemDef& d : Catalog()) { ids.insert(d.id); keys.insert(d.key); ok = ok && d.weapon == 0; }
            std::string bad;
            for (const ItemDef& d : WeaponCatalog()) {
                const std::string g = d.glyph;
                const bool one = d.id >= kFirstWeaponId && ids.insert(d.id).second && keys.insert(d.key).second && hashes.insert(d.weapon).second &&
                                 d.stackMax == 1 && (g == "gun" || g == "blade" || g == "grenade") && FindWeapon(d.weapon) == &d && Find(d.id) == &d &&
                                 d.weapon == inv::Joaat(d.key) && d.cat == Category::Weapons;
                if (!one) bad += std::string(d.key) + " ";
                ok = ok && one;
            }
            Check("inventory world: weapon hashes are GTA's joaat (pistol 0x1B06D571, unarmed 0xA2719263, knife 0x99B507EA); the weapon catalog's ids, "
                  "keys and hashes are unique and apart from the 0.6 items, each stacks 1 and draws a gun / blade / grenade glyph",
                  ok && WeaponCatalog().size() >= 90, F("%zu weapons; bad: %s", WeaponCatalog().size(), bad.c_str()));
        }
        {
            // The hand: USE on a pocket weapon takes it in hand; USE on the hand holsters it; a second weapon dragged onto the hand swaps; an
            // item that is not a weapon never goes in hand; Drop from the hand empties it (and reports the dropped weapon with its ammo);
            // a quick slot cannot be bound from the hand
            Character ch;
            Add(ch.pockets, Id("weapon_pistol"), 1, kNoCondition, 120);
            Add(ch.pockets, Id("weapon_knife"), 1);
            Add(ch.pockets, Id("water"), 1);
            const int pistolAt = 0, knifeAt = 1, waterAt = 2;
            const ApplyOut u = Apply(ch, MkOp(OpKind::Use, Where::Pockets, pistolAt));
            const bool inHand = u.r == Result::Moved && ch.hand.item == Id("weapon_pistol") && ch.hand.ammo == 120 && ch.pockets.slots[0].Empty();
            const ApplyOut sw = Apply(ch, MkOp(OpKind::Move, Where::Pockets, knifeAt, Where::Hand, 0));
            const bool swapped = sw.r == Result::Swapped && ch.hand.item == Id("weapon_knife") && ch.pockets.slots[(size_t)knifeAt].item == Id("weapon_pistol") &&
                                 ch.pockets.slots[(size_t)knifeAt].ammo == 120;
            const ApplyOut wa = Apply(ch, MkOp(OpKind::Move, Where::Pockets, waterAt, Where::Hand, 0));
            const ApplyOut hol = Apply(ch, MkOp(OpKind::Use, Where::Hand, 0));
            const bool holstered = hol.r == Result::Moved && ch.hand.Empty() && UsedSlots(ch.pockets) == 3;
            Apply(ch, MkOp(OpKind::Use, Where::Pockets, knifeAt));   // the pistol in hand again
            const ApplyOut bind = Apply(ch, MkOp(OpKind::Bind, Where::Hand, 0, Where::Quick, 1));
            const ApplyOut dr = Apply(ch, MkOp(OpKind::Drop, Where::Hand, 0));
            const bool dropped = dr.r == Result::Dropped && dr.dropped.item == Id("weapon_pistol") && dr.dropped.ammo == 120 && ch.hand.Empty();
            Check("inventory world: USE takes a weapon in hand and USE on the hand holsters it; another weapon onto the hand swaps (ammo travels); "
                  "water never goes in hand; Drop from the hand gives the weapon with its ammo; no quick slot is bound from the hand",
                  inHand && swapped && wa.r == Result::WrongSlot && holstered && bind.r == Result::BadSlot && dropped,
                  F("use %s swap %s water %s holster %s bind %s drop %s", ResultName(u.r), ResultName(sw.r), ResultName(wa.r), ResultName(hol.r),
                    ResultName(bind.r), ResultName(dr.r)));
        }
        {
            // Codec v2 and the 0.6.0 form: a character with a pistol in hand (ammo 120), a knife in the pockets, grenades in a pouch and a
            // quick slot on the knife round-trips in v2; its legacy (v1) form hides every weapon (slots empty, the quick ref unbound) and
            // keeps the rest; a v1 blob that carries a weapon, a v2 hand that holds water, and version 3 are refused; the save form is v1
            // without weapons and v2 with them; the size grows by exactly 2 per weapon slot plus the 9-byte hand
            Character ch;
            ch.hand = W("weapon_pistol", 120);
            Add(ch.pockets, Id("weapon_knife"), 1);
            Add(ch.pockets, Id("water"), 2);
            ch.equip[EquipIndex(Equip::Pack)] = Slot{Id("pouch"), 1, kNoCondition};
            ch.pack = MakePack(4);
            Add(ch.pack, Id("weapon_grenade"), 1, kNoCondition, 3);
            ch.quick[0] = QuickRef{Id("weapon_knife"), Kind::Pockets, 0};
            ch.quick[1] = QuickRef{Id("water"), Kind::Pockets, 1};
            Character back;
            const std::string v2 = Encode(ch), v1 = EncodeLegacy(ch);
            const bool round = Decode(v2, back) && Encode(back) == v2 && back.hand.ammo == 120 && back.pack.slots[0].ammo == 3;
            Character old;
            const bool legacy = Decode(v1, old) && old.hand.Empty() && old.pockets.slots[0].Empty() && old.pockets.slots[1].item == Id("water") &&
                                old.pack.slots[0].Empty() && old.quick[0].item == 0 && old.quick[1].item == Id("water") && (uint8_t)v1[0] == 1 && (uint8_t)v2[0] == 2;
            std::string forged = v1;   // a v1 blob whose pocket 0 holds the knife (7-byte slot: item u16 at byte 1)
            forged[1] = (char)(Id("weapon_knife") & 0xFF);
            forged[2] = (char)(Id("weapon_knife") >> 8);
            forged[3] = 1;
            Character waterHand = ch;
            waterHand.hand = Slot{Id("water"), 1, kNoCondition};
            std::string v3 = v2;
            v3[0] = 3;
            Character x;
            const bool refuses = !Decode(forged, x) && !Decode(Encode(waterHand), x) && !Decode(v3, x);
            Character plain;
            Add(plain.pockets, Id("water"), 1);
            const bool saveForm = (uint8_t)EncodeForSave(plain)[0] == 1 && (uint8_t)EncodeForSave(ch)[0] == 2 && Decode(EncodeForSave(plain), x) &&
                                  Encode(x) == Encode(plain);
            const bool size = v2.size() == v1.size() + 2 * 2 + 9;   // the knife and the grenades: +2 each; the hand (with its ammo): +9
            Check("inventory world: codec v2 round-trips weapons with ammo and the hand; the 0.6.0 (v1) form hides every weapon and its quick ref and keeps "
                  "the rest; a weapon in a v1 blob, water in hand and version 3 are refused; saves stay v1 until a weapon is carried; v2 = v1 + 2 per "
                  "boxed weapon + the 9-byte hand",
                  round && legacy && refuses && saveForm && size, F("v1 %zu v2 %zu round %d legacy %d refuses %d save %d", v1.size(), v2.size(), (int)round,
                                                                    (int)legacy, (int)refuses, (int)saveForm));
        }
        {
            // The op with a world side: 19 bytes carrying both box ids; the 11-byte form with a Nearby side and the 19-byte form without one are
            // refused, as is a Where past Nearby; the plain op is still 11 bytes. A world container round-trips (a trunk: 24 slots, 80 kg, a
            // rifle with 90 rounds and 3 water); kinds Pockets / Pack, 0 or 65 slots and a trailing byte are refused
            const Op o = MkOp(OpKind::Move, Where::Nearby, 5, Where::Pockets, 2, 0, 0xDEADBEEFu, 0);
            const std::string b = EncodeOp(o);
            Op back;
            const bool ext = b.size() == kOpBytesExt && DecodeOp(b, back) && back.src == Where::Nearby && back.srcBox == 0xDEADBEEFu && back.si == 5 && back.di == 2;
            const std::string shortForm = b.substr(0, kOpBytes);
            std::string longPlain = EncodeOp(MkOp(OpKind::Move, Where::Pockets, 0, Where::Pockets, 1)) + std::string(8, '\0');
            std::string past = b;
            past[1] = (char)((uint8_t)Where::Nearby + 1);
            const bool refuses = !DecodeOp(shortForm, back) && !DecodeOp(longPlain, back) && !DecodeOp(past, back) &&
                                 EncodeOp(MkOp(OpKind::Move, Where::Pockets, 0, Where::Hand, 0)).size() == kOpBytes;
            Container t;
            t.kind = Kind::Trunk;
            t.maxGrams = 80000;
            t.slots.resize(24);
            Add(t, Id("weapon_carbinerifle"), 1, kNoCondition, 90);
            Add(t, Id("water"), 3);
            Container tb;
            const std::string bx = EncodeBox(t);
            const bool box = DecodeBox(bx, tb) && EncodeBox(tb) == bx && tb.slots.size() == 24 && tb.slots[0].ammo == 90 && tb.kind == Kind::Trunk;
            Container pk = t;
            pk.kind = Kind::Pockets;
            Container none = t;
            none.slots.clear();
            std::string n65 = bx;
            n65[6] = 65;
            const bool boxRefuses = !DecodeBox(EncodeBox(pk), tb) && !DecodeBox(EncodeBox(none), tb) && !DecodeBox(n65, tb) && !DecodeBox(bx + "x", tb);
            Check("inventory world: a Nearby op is 19 bytes with both box ids (11 with a Nearby side, 19 without one, a Where past Nearby: refused; a plain "
                  "op stays 11); a world box round-trips (trunk, 24 slots, a rifle with 90 rounds) and refuses pockets / pack kinds, 0 or 65 slots, a "
                  "trailing byte",
                  ext && refuses && box && boxRefuses, F("ext %d refuses %d box %d boxRefuses %d", (int)ext, (int)refuses, (int)box, (int)boxRefuses));
        }
        {
            // Ground piles: a drop opens a pile at the dropper's feet; a second drop within reach (2 m) merges into it (same id, same stack);
            // one 3 m away opens another; NEARBY shows the nearest pile in reach and how many are in reach; a pile goes 10 min after the
            // LAST thing was put in it (a merge restarts the clock), and an emptied pile goes at once
            invw::World w;
            const uint32_t a = w.Drop({0, 0, 0}, Slot{Id("water"), 2, kNoCondition}, 1000);
            const uint32_t a2 = w.Drop({1.5f, 0, 0}, Slot{Id("water"), 1, kNoCondition}, 2000);
            const uint32_t b = w.Drop({3, 0, 0}, Slot{Id("sandwich"), 1, kNoCondition}, 3000);
            const bool merged = a != 0 && a2 == a && b != 0 && b != a && w.Piles().size() == 2 && w.Piles()[0].box.slots[0].qty == 3;
            invw::Viewer v;
            v.id = 1;
            v.pos = {1.5f, 0, 0};
            const std::vector<invw::NearBox> both = w.Nearby(v, {});
            v.pos = {-1.0f, 0, 0};
            const std::vector<invw::NearBox> one = w.Nearby(v, {});
            v.pos = {10, 0, 0};
            const std::vector<invw::NearBox> none = w.Nearby(v, {});
            const bool listed = both.size() == 1 && both[0].piles == 2 && both[0].kind == Kind::Ground && one.size() == 1 && one[0].id == a &&
                                one[0].piles == 1 && none.empty();
            const invw::World::SweepOut s1 = w.Sweep(2000 + 599999, {});
            const size_t kept = w.Piles().size();   // a (touched at 2000) kept, b (3000) kept
            const invw::World::SweepOut s2 = w.Sweep(2000 + 600000, {});
            const bool aGone = w.Piles().size() == 1 && w.Piles()[0].id == b;
            const invw::World::SweepOut s3 = w.Sweep(3000 + 600000, {});
            const uint32_t c = w.Drop({0, 0, 0}, Slot{Id("bandage"), 1, kNoCondition}, 5000);
            w.Box(c)->slots[0].Clear();
            w.DropIfEmpty(c);
            Check("inventory world: a drop opens a pile; within 2 m it merges (same pile), 3 m away a new one; NEARBY shows the nearest pile in reach "
                  "and the count in reach; a pile expires 10 min after the last drop into it (not before), and an emptied pile goes at once",
                  merged && listed && s1.expired == 0 && kept == 2 && s2.expired == 1 && aGone && s3.expired == 1 && w.Piles().empty(),
                  F("merged %d listed %d kept %zu exp %d %d %d left %zu", (int)merged, (int)listed, kept, s1.expired, s2.expired, s3.expired, w.Piles().size()));
        }
        {
            // Pile cap and reach: with room for 2 piles a third drop elsewhere is refused (nothing placed, CanDrop says so first) while a drop
            // into a pile in reach still merges; the unsigned age of a pile "touched after now" is 0, not a wrap
            invw::Rules r;
            r.maxPiles = 2;
            invw::World w(r);
            w.Drop({0, 0, 0}, Slot{Id("water"), 1, kNoCondition}, 1000);
            w.Drop({50, 0, 0}, Slot{Id("water"), 1, kNoCondition}, 1000);
            const bool can = w.CanDrop({100, 0, 0}, Slot{Id("water"), 1, kNoCondition});
            const uint32_t refused = w.Drop({100, 0, 0}, Slot{Id("water"), 1, kNoCondition}, 1000);
            const uint32_t merged = w.Drop({0.5f, 0, 0}, Slot{Id("water"), 1, kNoCondition}, 1000);
            const invw::World::SweepOut s = w.Sweep(500, {});   // a clock read before the drops
            Check("inventory world: at the pile cap a drop elsewhere is refused (CanDrop false, nothing placed) and a drop within reach of a pile still "
                  "merges; a pile touched after `now` is not expired (no unsigned wrap)",
                  !can && refused == 0 && merged != 0 && w.Piles().size() == 2 && s.expired == 0, F("can %d refused %u merged %u", (int)can, refused, merged));
        }
        {
            // Taking and putting through ApplyEx: Shift+click a pile stack into the pockets; a rifle from the pile straight into the hand; a
            // pocket stack onto the pile; USE and DROP on a pile slot are refused (use and drop work on what the player carries); without the
            // box (null: out of reach) the op is refused
            Container pile;
            pile.kind = Kind::Ground;
            pile.slots.resize(24);
            Add(pile, Id("water"), 3);
            Add(pile, Id("weapon_carbinerifle"), 1, kNoCondition, 60);
            Character ch;
            Add(ch.pockets, Id("sandwich"), 1);
            const ApplyOut take = ApplyEx(ch, MkOp(OpKind::QuickMove, Where::Nearby, 0, Where::Pockets, 0, 0, 7, 0), &pile, nullptr);
            const ApplyOut gun = ApplyEx(ch, MkOp(OpKind::Move, Where::Nearby, 1, Where::Hand, 0, 0, 7, 0), &pile, nullptr);
            const ApplyOut put = ApplyEx(ch, MkOp(OpKind::Move, Where::Pockets, 0, Where::Nearby, 5, 0, 0, 7), nullptr, &pile);
            Add(pile, Id("water"), 1);
            const ApplyOut use = ApplyEx(ch, MkOp(OpKind::Use, Where::Nearby, 0, Where::Pockets, 0, 0, 7, 0), &pile, nullptr);
            const ApplyOut drop = ApplyEx(ch, MkOp(OpKind::Drop, Where::Nearby, 0, Where::Pockets, 0, 0, 7, 0), &pile, nullptr);
            const ApplyOut gone = ApplyEx(ch, MkOp(OpKind::QuickMove, Where::Nearby, 0, Where::Pockets, 0, 0, 7, 0), nullptr, nullptr);
            const bool state = QtyOf(ch, Id("water")) == 3 && ch.hand.item == Id("weapon_carbinerifle") && ch.hand.ammo == 60 && pile.slots[5].item == Id("sandwich") &&
                               pile.slots[1].Empty();
            Check("inventory world: Shift+click takes a pile stack into the pockets, a rifle goes from the pile straight into the hand (with its 60 rounds), "
                  "a pocket stack goes onto the pile; USE / DROP on a pile slot and any op on a box out of reach are refused",
                  Ok(take.r) && Ok(gun.r) && Ok(put.r) && !Ok(use.r) && !Ok(drop.r) && !Ok(gone.r) && state,
                  F("take %s gun %s put %s use %s drop %s gone %s", ResultName(take.r), ResultName(gun.r), ResultName(put.r), ResultName(use.r),
                    ResultName(drop.r), ResultName(gone.r)));
        }
        {
            // THE TRUNK CASE LIST (GI: "car trunk isn't being shown when in / near a car"). One SUV (class 2: 32 slots) of player 7's, a
            // viewer = player 1:
            //  1 driver: seated in it, even with a stale position 30 m off -> listed, unlocked, distance 0
            //  2 passenger of a LOCKED car that is not the viewer's -> listed and open (an occupant is inside the car)
            //  3 standing 3 m from an unlocked car -> listed;  4 at 5 m -> not listed (reach 4 m)
            //  5 a remote player's LOCKED car, standing next to it, no key -> listed as locked, no contents
            //  6 the same car, the viewer is its last driver (holds the key) -> open
            //  7 a parked car nobody drives (spawner gone) -> listed like any other
            //  8 a class not reported yet (-1), a bicycle (13) -> no trunk; a motorbike (8) -> a 4-slot top case
            //  9 two cars in reach -> the nearer; seated in the farther -> that one (the seat wins)
            invw::World w;
            invw::VehView suv;
            suv.id = 3; suv.born = 100; suv.pos = {0, 0, 0}; suv.model = 0x1234; suv.cls = 2; suv.spawner = 7; suv.lastDriver = 7;
            invw::Viewer me;
            me.id = 1;
            auto list = [&](const invw::Viewer& who, const std::vector<invw::VehView>& vs) { return w.Nearby(who, vs); };
            me.seatedIn = 3; me.pos = {30, 0, 0};
            const auto c1 = list(me, {suv});
            const bool k1 = c1.size() == 1 && c1[0].kind == Kind::Trunk && !c1[0].locked && c1[0].box && c1[0].box->slots.size() == 32 && c1[0].dist == 0 && c1[0].veh == 3;
            invw::VehView locked = suv;
            locked.locked = true;
            me.seatedIn = 3; me.pos = {0.5f, 0, 0};
            const auto c2 = list(me, {locked});
            const bool k2 = c2.size() == 1 && !c2[0].locked && c2[0].box;
            me.seatedIn = invw::kNoVehicle; me.pos = {3, 0, 0};
            const auto c3 = list(me, {suv});
            me.pos = {5, 0, 0};
            const auto c4 = list(me, {suv});
            const bool k34 = c3.size() == 1 && c3[0].dist == 3.0f && c4.empty();
            me.pos = {1, 0, 0};
            const auto c5 = list(me, {locked});
            const bool k5 = c5.size() == 1 && c5[0].locked && !c5[0].box;
            invw::VehView mine = locked;
            mine.lastDriver = 1;
            const auto c6 = list(me, {mine});
            const bool k6 = c6.size() == 1 && !c6[0].locked && c6[0].box;
            invw::VehView parked = suv;
            parked.spawner = invw::kNoPlayer; parked.lastDriver = invw::kNoPlayer;
            const auto c7 = list(me, {parked});
            const bool k7 = c7.size() == 1 && c7[0].box;
            invw::VehView unknown = suv, cycle = suv, bike = suv;
            unknown.id = 10; unknown.cls = -1;
            cycle.id = 11; cycle.cls = 13;
            bike.id = 12; bike.born = 5; bike.cls = 8;
            const bool k8 = list(me, {unknown}).empty() && list(me, {cycle}).empty() && list(me, {bike}).size() == 1 && list(me, {bike})[0].box->slots.size() == 4;
            invw::VehView farCar = suv;
            farCar.id = 20; farCar.born = 7; farCar.pos = {3.5f, 0, 0};
            me.pos = {1, 0, 0};
            const auto c9a = list(me, {farCar, suv});
            me.seatedIn = 20;
            const auto c9b = list(me, {suv, farCar});
            const bool k9 = c9a.size() == 1 && c9a[0].veh == 3 && c9b.size() == 1 && c9b[0].veh == 20;
            Check("inventory world: trunk case list: driver (seated, stale position) listed at 0 m; passenger of a locked car listed open; standing at 3 m "
                  "listed, at 5 m not; a stranger's locked car listed locked with no contents, open to its last driver; a parked car listed; no trunk until "
                  "the class is known or for a bicycle, a 4-slot top case on a motorbike; the nearer car, but the one the viewer sits in wins",
                  k1 && k2 && k34 && k5 && k6 && k7 && k8 && k9,
                  F("1:%d 2:%d 3/4:%d 5:%d 6:%d 7:%d 8:%d 9:%d", (int)k1, (int)k2, (int)k34, (int)k5, (int)k6, (int)k7, (int)k8, (int)k9));
        }
        {
            // A vehicle that despawns (or is destroyed): its trunk's contents go into a pile where it last stood (its position learned by the
            // sweeps while it existed) and the trunk is gone; a NEW vehicle that gets the same id (a later creation time) starts with an empty
            // trunk, never the old one's contents
            invw::World w;
            invw::VehView car;
            car.id = 4; car.born = 50; car.pos = {0, 0, 0}; car.cls = 1;
            invw::Viewer me;
            me.id = 1; me.pos = {1, 0, 0};
            invw::Trunk* t = w.TrunkOf(car);
            const uint32_t trunkId = t ? t->id : 0;
            if (t) { Add(t->box, Id("water"), 4); Add(t->box, Id("weapon_pistol"), 1, kNoCondition, 30); }
            car.pos = {100, 0, 0};
            w.Sweep(1000, {car});   // it drove away
            const invw::World::SweepOut gone = w.Sweep(2000, {});
            const bool dumped = gone.trunksDumped == 1 && gone.stacksDumped == 2 && w.Trunks().empty() && w.Piles().size() == 1 &&
                                w.Piles()[0].pos.x == 100 && w.Box(trunkId) == nullptr;
            invw::VehView reborn = car;
            reborn.born = 3000;
            invw::Trunk* t2 = w.TrunkOf(reborn);
            const bool fresh = t2 && t2->id != trunkId && UsedSlots(t2->box) == 0;
            Check("inventory world: a vehicle that goes empties its trunk into a pile where it last stood (2 stacks) and the trunk id is gone; a new "
                  "vehicle with the same id starts with an empty trunk",
                  trunkId != 0 && dumped && fresh, F("dumped %d stacks %d piles %zu fresh %d", gone.trunksDumped, gone.stacksDumped, w.Piles().size(), (int)fresh));
        }
        {
            // Reconcile, the server's half: (a) a weapon the ped gained (the current one, 60 rounds) goes in hand; (b) the same report again
            // changes nothing (a stable ped sends nothing back = no loop); (c) a second gained weapon goes to the pockets; (d) with no room it
            // goes to the overflow (the ground) and the strip list; (e) current = unarmed holsters the hand; (f) the current weapon in the
            // pockets moves to the hand
            Character ch;
            invw::AmmoTypes t;
            invw::Report r;
            r.current = H("weapon_pistol");
            r.weapons = {{H("weapon_pistol"), 0xA1, 60}};
            const invw::ReconcileOut a = invw::Reconcile(ch, r, t);
            const bool ra = a.changed && a.gains == 1 && ch.hand.item == Id("weapon_pistol") && ch.hand.ammo == 60;
            const invw::ReconcileOut b = invw::Reconcile(ch, r, t);
            r.weapons.push_back({H("weapon_knife"), 0, 0});
            const invw::ReconcileOut c = invw::Reconcile(ch, r, t);
            const bool rc = c.changed && c.gains == 1 && invw::Carries(ch, H("weapon_knife")) && !ch.pockets.slots[0].Empty();
            Character full = ch;
            for (Slot& s : full.pockets.slots)
                if (s.Empty()) s = Slot{Id("notebook"), 1, kNoCondition};
            invw::Report rf = r;
            rf.weapons.push_back({H("weapon_grenade"), 0xA3, 2});
            const invw::ReconcileOut d = invw::Reconcile(full, rf, t);
            const bool rd = d.overflow.size() == 1 && d.overflow[0].item == Id("weapon_grenade") && d.strip.size() == 1 && d.strip[0] == H("weapon_grenade") &&
                            !invw::Carries(full, H("weapon_grenade"));
            invw::Report un = r;
            un.current = inv::kWeaponUnarmed;
            const invw::ReconcileOut e = invw::Reconcile(ch, un, t);
            const bool re = e.holstered == 1 && ch.hand.Empty() && invw::Carries(ch, H("weapon_pistol"));
            invw::Report kn = r;
            kn.current = H("weapon_knife");
            const invw::ReconcileOut f = invw::Reconcile(ch, kn, t);
            const bool rfh = f.toHand == 1 && ch.hand.item == Id("weapon_knife");
            Check("inventory world: Reconcile: a gained current weapon goes in hand with its rounds; the same report again changes nothing; a second gain "
                  "goes to the pockets; with no room it is dropped and stripped; unarmed holsters the hand; the current pocket weapon moves into the hand",
                  ra && !b.changed && rc && rd && re && rfh,
                  F("a %d b %d c %d d %d e %d f %d", (int)ra, (int)b.changed, (int)rc, (int)rd, (int)re, (int)rfh));
        }
        {
            // Ammo per type: a pistol and a combat pistol share GTA's pistol pool, so the pool (80) sits on ONE item (the hand's), the other
            // holds 0 and the total stays 80 (no doubling); dropping the holder takes the 80 with it and the pool on the ped goes to 0; a
            // thrown weapon the ped no longer has at a count of 1 was thrown (consumed); at a count of 3 it is kept (the client gives it back)
            Character ch;
            invw::AmmoTypes t;
            invw::Report r;
            r.current = H("weapon_pistol");
            r.weapons = {{H("weapon_pistol"), 0xA1, 80}, {H("weapon_combatpistol"), 0xA1, 80}};
            invw::Reconcile(ch, r, t);
            const uint32_t total = invw::GroupTotalOf(ch, H("weapon_pistol"), t);
            const bool shared = total == 80 && ch.hand.ammo == 80 && ch.pockets.slots[0].ammo == 0;
            const ApplyOut dr = Apply(ch, MkOp(OpKind::Drop, Where::Hand, 0));
            invw::NormaliseAmmo(ch, t);
            const bool poolGone = dr.dropped.ammo == 80 && invw::GroupTotalOf(ch, H("weapon_combatpistol"), t) == 0;
            Character g1, g3;
            g1.pockets.slots[0] = W("weapon_grenade", 1);
            g3.pockets.slots[0] = W("weapon_grenade", 3);
            invw::Report none;
            none.current = inv::kWeaponUnarmed;
            const invw::ReconcileOut c1 = invw::Reconcile(g1, none, t);
            const invw::ReconcileOut c3 = invw::Reconcile(g3, none, t);
            invw::PedState ped;
            ped.valid = true;
            const std::vector<invw::Act> back = invw::PlanRestore(g3, ped, t);
            const std::vector<invw::Act> notBack = invw::PlanRestore(g1, ped, t);
            Check("inventory world: two pistols share one pool: 80 on the hand's, 0 on the other (total 80, not 160); dropping the holder takes the 80; "
                  "a thrown weapon gone from the ped at 1 is used up, at 3 it is kept and the client gives it back with 3",
                  shared && poolGone && c1.consumed == 1 && !invw::Carries(g1, H("weapon_grenade")) && c3.consumed == 0 && back.size() == 2 &&
                      back[1].ammo == 3 && notBack.empty(),
                  F("total %u hand %u other %u dropped %u consumed %d/%d restore %zu/%zu", total, ch.hand.ammo, ch.pockets.slots[0].ammo, dr.dropped.ammo,
                    c1.consumed, c3.consumed, back.size(), notBack.size()));
        }
        {
            // The client's half (PlanSnapshot): a snapshot that drops the hand's pistol removes it from the ped, sets the pistol pool and selects
            // unarmed; a snapshot answering our own report touches neither ammo nor the current weapon; a carried weapon the ped lacks is given
            // with its group's rounds; a strip-listed weapon the character never carried is removed; a weapon the ped has that the character
            // never carried is NOT removed (it is a gain the server has not answered yet)
            invw::AmmoTypes t;
            t.Learn(H("weapon_pistol"), 0xA1);
            Character before, after;
            before.hand = W("weapon_pistol", 100);
            invw::PedState ped;
            ped.valid = true;
            ped.current = H("weapon_pistol");
            ped.have = {{H("weapon_pistol"), 0xA1, 100}, {H("weapon_knife"), 0, 0}, {H("weapon_grenade"), 0xA3, 2}};
            const std::vector<invw::Act> a = invw::PlanSnapshot(&before, after, false, {H("weapon_grenade")}, ped, t);
            bool removedPistol = false, removedGrenade = false, removedKnife = false, unarmed = false;
            for (const invw::Act& x : a) {
                removedPistol |= x.k == invw::ActK::Remove && x.hash == H("weapon_pistol");
                removedGrenade |= x.k == invw::ActK::Remove && x.hash == H("weapon_grenade");
                removedKnife |= x.k == invw::ActK::Remove && x.hash == H("weapon_knife");
                unarmed |= x.k == invw::ActK::Select && x.hash == inv::kWeaponUnarmed;
            }
            Character carry;
            carry.hand = W("weapon_pistol", 100);
            Add(carry.pockets, Id("weapon_carbinerifle"), 1, kNoCondition, 45);
            invw::PedState p2;
            p2.valid = true;
            p2.current = H("weapon_pistol");
            p2.have = {{H("weapon_pistol"), 0xA1, 100}};
            invw::PedState both = p2;   // the ped holds everything the character carries, at other pools than the inventory's
            both.have.push_back({H("weapon_carbinerifle"), 0, 12});
            both.have[0].ammo = 77;
            Character moved = carry;    // the answer to our report moved the rifle and changed the pistol's rounds
            std::swap(moved.pockets.slots[0], moved.pockets.slots[3]);
            moved.hand.ammo = 77;
            const std::vector<invw::Act> own = invw::PlanSnapshot(&carry, moved, true, {}, both, t);
            Character noRifle = carry;
            noRifle.pockets.slots[0].Clear();
            const std::vector<invw::Act> give = invw::PlanSnapshot(&noRifle, carry, true, {}, p2, t);
            const bool gaveRifle = give.size() == 2 && give[0].k == invw::ActK::Give && give[0].hash == H("weapon_carbinerifle") && give[1].k == invw::ActK::SetAmmo &&
                                   give[1].ammo == 45;
            Check("inventory world: PlanSnapshot: a dropped hand pistol is removed, its pool set and unarmed selected; the strip list removes a weapon never "
                  "carried, an unanswered gain is left alone; our own report's answer changes nothing; a carried rifle the ped lacks is given with its 45",
                  removedPistol && removedGrenade && !removedKnife && unarmed && own.empty() && gaveRifle,
                  F("acts %zu pistol %d grenade %d knife %d unarmed %d own %zu give %zu", a.size(), (int)removedPistol, (int)removedGrenade, (int)removedKnife,
                    (int)unarmed, own.size(), give.size()));
        }
        {
            // THE DUPE RACE (the rev gate is the rule under test): the ped's pistol is in the inventory; a report sent after some shots is
            // overtaken by the player's DROP; the stale report must not put the pistol back. With the gate: one pistol in existence (on the
            // ground). Control: the same sequence without the gate makes two (the stale report re-adds it) - the gate is what decides.
            std::string tg, tn;
            const int gated = DropRace(true, &tg);
            const int ungated = DropRace(false, &tn);
            Check("inventory world: a report stamped before a drop is ignored by the rev gate: one pistol after the race (on the ground), and the "
                  "control without the gate duplicates it (2)",
                  gated == 1 && ungated == 2, F("gated %d (%s) | ungated %d (%s)", gated, tg.c_str(), ungated, tn.c_str()));
        }
        {
            // Convergence, no loop: a ped that picks up a pistol, a carbine and 3 grenades, switches weapons, throws 2 grenades and then the
            // last, while the player moves things around; after each step everything is delivered; at the end the ped and the inventory agree
            // (same weapons, hand == current, pools == group totals) and ten more idle rounds exchange NO message
            Sim s;
            bool ok = true;
            std::string why;
            auto step = [&](const char* what) {
                s.MaybeReport();
                s.Settle();
                if (!s.Consistent()) { ok = false; why += std::string(what) + " "; }
            };
            s.PedGive(H("weapon_pistol"), 100, true);
            step("pistol");
            s.PedGive(H("weapon_carbinerifle"), 200, true);
            step("carbine");
            s.PedGive(H("weapon_grenade"), 3, false);
            step("grenades");
            s.ped.current = H("weapon_pistol");
            step("switch");
            s.PedFire(0xA1, 30);
            step("fire");
            s.PedFire(0xA3, 2);
            step("throw2");
            // the last grenade: GTA removes the weapon from the ped
            s.PedFire(0xA3, 1);
            s.PedRemove(H("weapon_grenade"));
            step("throw last");
            // the player holsters the hand from the overlay (USE on the hand)
            s.ServerOp(MkOp(OpKind::Use, Where::Hand, 0));
            s.Settle();
            step("holster");
            const int before = s.reports + s.snaps;
            for (int i = 0; i < 10; i++) { s.MaybeReport(); s.Settle(); }
            const int idle = s.reports + s.snaps - before;
            const bool final = s.Consistent() && !invw::Carries(s.ch, H("weapon_grenade")) && s.ch.hand.Empty() && s.ped.current == inv::kWeaponUnarmed &&
                               invw::GroupTotalOf(s.ch, H("weapon_pistol"), s.srvTypes) == 70 && s.Count(H("weapon_pistol")) == 1;
            Check("inventory world: gains, a switch, shots, thrown grenades (the last one used up) and a holster converge: ped == inventory (weapons, hand, "
                  "pools: pistol 70) after every step, and ten idle rounds exchange no message (no loop)",
                  ok && final && idle == 0, F("ok %d (%s) final %d idle msgs %d reports %d snaps %d stale %d", (int)ok, why.c_str(), (int)final, idle, s.reports,
                                              s.snaps, s.stale));
        }
        {
            // ReportGate's timing and catch-up (the engine's report rule): the first report at once; a stable ped never again; an ammo-only
            // change waits 2 s after the last ammo report, a presence change 250 ms after the last report; after a newer snapshot the ped
            // agrees with: nothing, and the gate takes that rev as reported; one it disagrees with: a report, but not within 250 ms of the apply
            invw::ReportGate gt;
            invw::AmmoTypes t;
            t.Learn(H("weapon_pistol"), 0xA1);
            invw::PedState p;
            p.valid = true;
            p.current = H("weapon_pistol");
            p.have = {{H("weapon_pistol"), 0xA1, 100}};
            const bool first = gt.Due(p, 0, nullptr, -1e9, t, 0);
            gt.Sent(p, 0, 0);
            bool idle = false;
            for (int i = 1; i <= 40; i++) idle |= gt.Due(p, 0, nullptr, -1e9, t, i * 250.0);
            invw::PedState shot = p;
            shot.have[0].ammo = 90;
            const bool ammoEarly = gt.Due(shot, 0, nullptr, -1e9, t, 1900);
            const bool ammoLate = gt.Due(shot, 0, nullptr, -1e9, t, 2000);
            gt.Sent(shot, 0, 2000);
            invw::PedState two = shot;
            two.have.push_back({H("weapon_knife"), 0, 0});
            const bool presEarly = gt.Due(two, 0, nullptr, -1e9, t, 2100);
            const bool presLate = gt.Due(two, 0, nullptr, -1e9, t, 2250);
            Character agree;
            agree.hand = W("weapon_pistol", 90);
            const bool agreeDue = gt.Due(shot, 1, &agree, 2300, t, 2600);
            const bool caughtUp = gt.lastSentRev == 1;
            const bool tooSoon = gt.Due(two, 2, &agree, 2900, t, 3000);
            const bool disagreeDue = gt.Due(two, 2, &agree, 2900, t, 3200);
            Check("inventory world: ReportGate: first report at once, a stable ped never (40 asks), ammo-only after 2 s, a new weapon after 250 ms; a newer "
                  "snapshot the ped agrees with sends nothing and is taken as reported; one it disagrees with is reported, not within 250 ms of its apply",
                  first && !idle && !ammoEarly && ammoLate && !presEarly && presLate && !agreeDue && caughtUp && !tooSoon && disagreeDue,
                  F("first %d idle %d ammo %d/%d presence %d/%d agree %d caught up %d (rev %u) disagree soon %d later %d", (int)first, (int)idle,
                    (int)ammoEarly, (int)ammoLate, (int)presEarly, (int)presLate, (int)agreeDue, (int)caughtUp, (unsigned)gt.lastSentRev, (int)tooSoon,
                    (int)disagreeDue));
        }
    }

} // namespace invworld
