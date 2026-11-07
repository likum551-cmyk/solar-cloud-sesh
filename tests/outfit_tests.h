// 0.5.4 outfit_items (libs/sync/outfit.h): the worn shirt / pants / shoes items decide the local ped's components. The bare table against its
// cited sources, the starter kit against the spawn outfit, and the engine's loop (Desired -> Mismatch -> set what differs) on a simulated ped,
// where the 0.5.3 behaviour (the items change nothing) must FAIL the take-off checks.
// Included by tests/gameplay_test.cpp after its Check() and F(); run by outfitt::RunPure(), kChecks checks.
#pragma once

#include "sync/inventory.h"
#include "sync/outfit.h"

#include <initializer_list>
#include <string>

namespace outfitt {

    namespace inv = gage::sync::inv;
    namespace of = gage::sync::outfit;

    constexpr int kChecks = 11;   // 0.5.6: + 2 (RunFeed)

    inline void RunFeed(bool snapsFeed = true);

    // The engine's spawn outfit (apps/engine/engine.cpp kPlayerOutfit), drawable / texture per component 0..11.
    constexpr int kSpawnD[12] = {0, 0, 1, 8, 10, 0, 8, 0, 15, 0, 0, 8};
    constexpr int kSpawnT[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10};

    // A ped as the engine sees it: what ReadPedLook returns, what SetPedComponentChecked changes.
    struct SimPed
    {
        int16_t d[12];
        uint8_t t[12];
        SimPed() { for (int c = 0; c < 12; ++c) { d[c] = (int16_t)kSpawnD[c]; t[c] = (uint8_t)kSpawnT[c]; } }
        bool IsSpawn() const { for (int c = 0; c < 12; ++c) if (d[c] != kSpawnD[c] || t[c] != kSpawnT[c]) return false; return true; }
    };
    // The engine's TickOutfitItems loop; itemsDriveTheLook false = the 0.5.3 behaviour (nothing reads the items).
    inline int Tick(SimPed& ped, const inv::Character& ch, of::Sex sex, bool itemsDriveTheLook = true)
    {
        if (!itemsDriveTheLook)
            return 0;
        const of::Want w = of::Desired(ch, sex);
        const uint32_t m = of::Mismatch(w, ped.d, ped.t);
        int n = 0;
        for (int c = 0; c < 12; ++c)
            if (m & (1u << c)) {
                ped.d[c] = w.drawable[c];
                ped.t[c] = w.texture[c];
                ++n;
            }
        return n;
    }
    inline inv::Op TakeOff(inv::Equip e)
    {
        inv::Op op;
        op.kind = inv::OpKind::Use;   // USE on a worn item = take it off into the pockets (section 8)
        op.src = inv::Where::Equip;
        op.si = (int16_t)inv::EquipIndex(e);
        return op;
    }
    inline int PocketOf(const inv::Character& ch, const char* key)
    {
        const inv::ItemDef* d = inv::FindKey(key);
        for (size_t i = 0; d && i < ch.pockets.slots.size(); ++i)
            if (ch.pockets.slots[i].item == d->id)
                return (int)i;
        return -1;
    }
    inline bool PartsAre(const of::Parts& p, std::initializer_list<of::Part> want)
    {
        if (p.n != (int)want.size())
            return false;
        int i = 0;
        for (const of::Part& q : want) {
            if (p.p[i].component != q.component || p.p[i].drawable != q.drawable || p.p[i].texture != q.texture)
                return false;
            ++i;
        }
        return true;
    }

    inline void RunPure()
    {
        {
            // the bare table, as cited in outfit.h (Colbss/FiveM-ClothingData renders + dpclothing)
            const bool male = PartsAre(of::Bare(inv::Equip::Shirt, of::Sex::Male), {{11, 15, 0}, {3, 15, 0}, {8, 15, 0}}) &&
                              PartsAre(of::Bare(inv::Equip::Pants, of::Sex::Male), {{4, 61, 0}}) &&
                              PartsAre(of::Bare(inv::Equip::Shoes, of::Sex::Male), {{6, 34, 0}});
            const bool female = PartsAre(of::Bare(inv::Equip::Shirt, of::Sex::Female), {{11, 15, 0}, {3, 15, 0}, {8, 14, 0}}) &&
                                PartsAre(of::Bare(inv::Equip::Pants, of::Sex::Female), {{4, 15, 0}}) &&
                                PartsAre(of::Bare(inv::Equip::Shoes, of::Sex::Female), {{6, 35, 0}});
            bool others = true;
            for (int i = 0; i < inv::kEquipCount; ++i)
                if (!of::Managed(inv::EquipAt(i)))
                    others = others && of::Bare(inv::EquipAt(i), of::Sex::Male).n == 0;
            Check("outfit: bare = male top 15 / arms 15 / undershirt 15, legs 61 (boxers), feet 34; female top 15 (bra) / arms 15 / undershirt 14, "
                  "legs 15 (briefs), feet 35; only shirt / pants / shoes are managed",
                  male && female && others);
        }
        {
            // every component a worn item sets is reset by its slot's bare state (taking it off leaves nothing of it behind)
            bool covered = true;
            int looks = 0;
            std::string bad;
            for (const inv::ItemDef& d : inv::Catalog()) {
                const of::Parts w = of::Worn(d.id, of::Sex::Male);
                if (!w.n)
                    continue;
                ++looks;
                const of::Parts b = of::Bare(d.equip, of::Sex::Male);
                for (int i = 0; i < w.n; ++i) {
                    bool in = false;
                    for (int j = 0; j < b.n; ++j)
                        in = in || b.p[j].component == w.p[i].component;
                    if (!in) {
                        covered = false;
                        bad += std::string(d.key) + " ";
                    }
                }
            }
            Check("outfit: every component a worn item sets is one its slot's bare state resets (3 items with a look)", covered && looks == 3, bad);
        }
        {
            // the starter kit IS the spawn outfit: a freshly dressed ped needs nothing
            SimPed ped;
            const inv::Character kit = inv::StarterKit();
            const of::Want w = of::Desired(kit, of::Sex::Male);
            const int n = Tick(ped, kit, of::Sex::Male);
            Check("outfit: the starter kit (tee, black pants, sneakers worn) wants exactly the spawn outfit's top / arms / undershirt / legs / shoes; "
                  "a freshly dressed ped needs no change",
                  w.Count() == 5 && n == 0 && ped.IsSpawn(), F("wants %d, changed %d", w.Count(), n));
        }
        {
            // taking each item off takes it off the ped; the 0.5.3 behaviour (items ignored) leaves the ped dressed and must fail
            inv::Character ch = inv::StarterKit();
            SimPed ped, old;
            bool ok = true;
            for (inv::Equip e : {inv::Equip::Shirt, inv::Equip::Pants, inv::Equip::Shoes}) {
                const inv::ApplyOut r = inv::Apply(ch, TakeOff(e));
                ok = ok && inv::Ok(r.r);
                Tick(ped, ch, of::Sex::Male);
                Tick(old, ch, of::Sex::Male, false);
            }
            const bool bare = ped.d[11] == 15 && ped.d[3] == 15 && ped.d[8] == 15 && ped.d[4] == 61 && ped.d[6] == 34 && ped.t[11] == 0;
            const bool restUntouched = ped.d[0] == 0 && ped.d[1] == 0 && ped.d[2] == 1 && ped.d[5] == 0 && ped.d[7] == 0 && ped.d[9] == 0 &&
                                       ped.d[10] == 0;
            const bool oldFails = old.IsSpawn();   // the old code: still fully dressed
            Check("outfit: taking the tee, pants and shoes off in the inventory puts top 15 / arms 15 / undershirt 15 / legs 61 / feet 34 on the ped, "
                  "head / hair / bag / neck / armour / decal untouched (0.5.3, items ignored: the ped stays dressed)",
                  ok && bare && restUntouched && oldFails && !(old.d[11] == 15));
        }
        {
            // putting them back on (USE from the pockets) restores the spawn outfit exactly
            inv::Character ch = inv::StarterKit();
            SimPed ped;
            for (inv::Equip e : {inv::Equip::Shirt, inv::Equip::Pants, inv::Equip::Shoes})
                inv::Apply(ch, TakeOff(e));
            Tick(ped, ch, of::Sex::Male);
            bool ok = true;
            for (const char* key : {"tshirt", "jeans", "sneakers"}) {
                inv::Op op;
                op.kind = inv::OpKind::Use;   // USE on a wearable in the pockets = wear it
                op.src = inv::Where::Pockets;
                op.si = (int16_t)PocketOf(ch, key);
                ok = ok && op.si >= 0 && inv::Ok(inv::Apply(ch, op).r);
            }
            const int n = Tick(ped, ch, of::Sex::Male);
            // 4 changes: top, arms, legs, shoes (the undershirt is 8/15 both with the tee and bare)
            Check("outfit: worn again, the tee (top 8/10, arms 8), pants and shoes come back as the spawn outfit exactly (4 components change)",
                  ok && n == 4 && ped.IsSpawn(), F("changed %d", n));
        }
        {
            // one item at a time touches only its own components
            inv::Character ch = inv::StarterKit();
            SimPed ped;
            inv::Apply(ch, TakeOff(inv::Equip::Shoes));
            const int n = Tick(ped, ch, of::Sex::Male);
            bool restSpawn = true;
            for (int c = 0; c < 12; ++c)
                if (c != 6)
                    restSpawn = restSpawn && ped.d[c] == kSpawnD[c] && ped.t[c] == kSpawnT[c];
            Check("outfit: only the shoes off changes component 6 alone (bare feet 34), the tee and pants stay", n == 1 && ped.d[6] == 34 && restSpawn);
        }
        {
            // a ped GTA re-dressed (respawn, ped guard) is corrected again; a second tick has nothing to do
            inv::Character ch = inv::StarterKit();
            inv::Apply(ch, TakeOff(inv::Equip::Shirt));
            SimPed ped;
            Tick(ped, ch, of::Sex::Male);
            ped = SimPed();   // DressPlayer: the full outfit is back on
            const int again = Tick(ped, ch, of::Sex::Male);
            const int idle = Tick(ped, ch, of::Sex::Male);
            Check("outfit: after a re-dress (respawn / ped guard) the next check takes the tee off again (top and arms), then nothing is left to do",
                  again == 2 && idle == 0 && ped.d[11] == 15 && ped.d[3] == 15, F("again %d idle %d", again, idle));
        }
        {
            // 0.5.9 female characters: the worn starter items are the female pieces (Blue T-Shirt 11/0/6 + arms 3/0 + no undershirt 8/14,
            // Black Chinos 4/3, White Sports 6/4/1); an empty pants slot is briefs 4/15; the male table is untouched
            inv::Character ch = inv::StarterKit();
            const of::Want worn = of::Desired(ch, of::Sex::Female);
            const of::Want male = of::Desired(ch, of::Sex::Male);
            inv::Apply(ch, TakeOff(inv::Equip::Pants));
            const of::Want noPants = of::Desired(ch, of::Sex::Female);
            of::Sex s1, s2, s3;
            const bool models = of::SexOfModel(0x705E61F2u, s1) && s1 == of::Sex::Male && of::SexOfModel(0x9C9EFFD8u, s2) && s2 == of::Sex::Female &&
                                !of::SexOfModel(0x12345678u, s3);
            const bool fem = worn.Count() == 5 && worn.drawable[11] == 0 && worn.texture[11] == 6 && worn.drawable[3] == 0 && worn.drawable[8] == 14 &&
                             worn.drawable[4] == 3 && worn.drawable[6] == 4 && worn.texture[6] == 1;
            Check("outfit: on the female freemode ped the worn starter items are the female tee / chinos / sports shoes, an empty pants slot is "
                  "briefs 4/15, the male pieces stay 11/8/10, 4/10, 6/8; only the two freemode models have a sex",
                  fem && noPants.Count() == 5 && noPants.want[4] && noPants.drawable[4] == 15 && male.drawable[11] == 8 && male.texture[11] == 10 &&
                      male.drawable[4] == 10 && male.drawable[6] == 8 && models);
        }
        {
            // the engine reads the SERVER's snapshot: Desired over Encode -> Decode equals Desired over the character; an unmanaged item
            // (a cap) and a backpack change nothing
            inv::Character ch = inv::StarterKit();
            inv::Apply(ch, TakeOff(inv::Equip::Shirt));
            inv::Give(ch, inv::FindKey("cap")->id, 1);
            inv::Op wear;
            wear.kind = inv::OpKind::Use;
            wear.src = inv::Where::Pockets;
            wear.si = (int16_t)PocketOf(ch, "cap");
            const bool capOn = inv::Ok(inv::Apply(ch, wear).r);
            inv::Character back;
            const bool decoded = inv::Decode(inv::Encode(ch), back);
            const of::Want a = of::Desired(ch, of::Sex::Male), b = of::Desired(back, of::Sex::Male);
            Check("outfit: Desired over the server's encoded snapshot equals Desired over the character; a worn cap adds no component (5 wanted)",
                  capOn && decoded && a == b && a.Count() == 5 && !a.want[0] && !a.want[1]);
        }
        RunFeed();
    }

    // ---- 0.5.6: the engine's outfit source (WornFeed) over the message order a 0.5.4 server sends ----
    // The join's InvState (codec v1, the starter kit), then, once the client answered the inventory-world offer, InvSnap only (codec v2):
    // here the shirt comes off in an InvSnap. `snapsFeed` false = the 0.5.4 / 0.5.5 engine (OnInvSnap never reached the outfit): the ped must
    // stay dressed with it, which is GI's report.
    inline void RunFeed(bool snapsFeed)
    {
        {
            inv::Character ch = inv::StarterKit();
            of::WornFeed feed;
            feed.OnInvState(inv::EncodeLegacy(ch));
            const uint32_t seqJoin = feed.seq;
            inv::Apply(ch, TakeOff(inv::Equip::Shirt));
            if (snapsFeed)
                feed.OnInvSnap(inv::Encode(ch));
            SimPed ped;
            const int changed = feed.have ? Tick(ped, feed.ch, of::Sex::Male) : 0;
            Check("outfit (0.5.6): the shirt taken off in an InvSnap (the server's only form once the inventory world is answered) reaches the "
                  "outfit source: the next tick puts top 15 / arms 15 on the ped (0.5.4 / 0.5.5 read InvState only: the ped stayed dressed)",
                  feed.have && feed.seq == seqJoin + 1 && feed.snaps == 1 && changed == 2 && ped.d[11] == 15 && ped.d[3] == 15 &&
                      std::string(of::WornKey(feed.ch, inv::Equip::Shirt)) == "bare",
                  F("seq %u snaps %u changed %d top %d", feed.seq, feed.snaps, changed, (int)ped.d[11]));
        }
        {
            // an undecodable snapshot keeps the last good one (and counts it); the session's end clears it and moves seq
            of::WornFeed feed;
            feed.OnInvSnap(inv::Encode(inv::StarterKit()));
            const uint32_t s1 = feed.seq;
            const bool bad = !feed.OnInvSnap(std::string("\x07garbage", 8));
            const bool kept = feed.have && feed.seq == s1 && feed.undecodable == 1 && std::string(of::WornKey(feed.ch, inv::Equip::Shirt)) == "tshirt";
            feed.End();
            Check("outfit (0.5.6): an undecodable inventory keeps the last good one (counted), the session's end drops it and moves seq",
                  bad && kept && !feed.have && feed.seq == s1 + 1);
        }
    }

}  // namespace outfitt
