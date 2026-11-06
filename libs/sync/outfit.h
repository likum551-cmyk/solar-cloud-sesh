// outfit.h (0.5.4 outfit_items, GI 2026-11-07: "taking them off char in menu should actually take them off"). Pure C++17, no game: which ped
// components the worn clothing ITEMS put on the local freemode ped, and what a slot with nothing worn in it shows. The engine reads the
// server's inventory (the authority), asks Desired() for the components, compares them with what the ped really wears (read back) and sets
// the ones that differ; the 2 s look publisher then sends the new look to every peer (PlayerLook / PeerLook, unchanged since v3).
// tests/outfit_tests.h pins the table.
//
// Managed slots (round 1): SHIRT, PANTS and SHOES, the three things the spawn outfit (engine.cpp kPlayerOutfit) wears. Every other slot is
// left alone: its items have no look yet, and the ped keeps whatever the spawn outfit put there.
//
// Component ids are ePedVarComp (alloc8or SET_PED_COMPONENT_VARIATION): 3 UPPR torso / arms, 4 LOWR legs, 6 FEET shoes, 8 ACCS undershirt,
// 11 JBIB top. Drawables are GLOBAL indices (DLC only appends to them), texture 0 unless named.
//
// Sources for the bare ("nothing worn") drawables, both read 2026-11-07:
//   * Colbss/FiveM-ClothingData (game build 3407; renders images/<model>/<collection>/D_<comp>_<local>_<tex>.webp, data male_drawables.json /
//     female_drawables.json with each item's globalId), each render LOOKED AT:
//       male   11/15 base D_11_15_0: no mesh (an empty render)       male   3/15 base D_3_15_0: the bare torso with both arms
//       male    8/15 base D_8_15_0: no mesh                           male   4/61 mp_m_valentines_02 D_4_1_0 (globalId 61): white boxer shorts
//       male    6/34 Male_Apt01 D_6_1_0 (globalId 34): "No Shoes", bare feet
//       female 11/15 base D_11_15_0: a black bra (the female freemode body has no bare top; the bra IS the base layer)
//       female  3/15 base D_3_15_0: the bare torso with both arms     female 8/14 base D_8_14_0: no mesh
//       female  4/15 base D_4_15_0: black briefs, bare legs           female 6/35 Female_Apt01 D_6_1_0 (globalId 35): "No Shoes", bare feet
//   * andristum/dpclothing Client/Clothing.lua (the FiveM clothing-toggle script RP servers use): Shoes Male 34 / Female 35, Pants Male 61,
//     and the shirt's extras undershirt 8 -> 15 and arms 3 -> 15 agree. Where the two disagree (dpclothing: shirt Male 252 / Female 74, pants
//     Female 14) the rendered item wins: 252 and 74 are DLC tops, female 4/14 renders as shorts.
//
// The REV3 spec (section 8) maps SHIRT to component 8 and JACKET to 11. GTA's tee items exist as tops (11); a component-8 undershirt is only
// the layer under a jacket. So a worn item names ALL its parts (the T-shirt: top 11/8/10, the arms that pair with a tee 3/8, no undershirt
// 8/15), and the slot's bare state clears every component its item may have set. When jacket items get looks, the shirt has to switch to its
// undershirt form under a jacket: a rule for that round.
#pragma once

#include "sync/inventory.h"

#include <cstdint>

namespace gage::sync::outfit {

    enum class Sex : uint8_t { Male, Female };

    constexpr uint32_t kFreemodeMale = 0x705E61F2;     // mp_m_freemode_01
    constexpr uint32_t kFreemodeFemale = 0x9C9EFFD8;   // mp_f_freemode_01
    // The sex of a freemode model; false for any other model (the items have no look on it: the engine leaves such a ped alone).
    inline bool SexOfModel(uint32_t model, Sex& out)
    {
        if (model == kFreemodeMale) { out = Sex::Male; return true; }
        if (model == kFreemodeFemale) { out = Sex::Female; return true; }
        return false;
    }

    constexpr int kComponents = 12;
    struct Part
    {
        int8_t component;
        int16_t drawable;
        uint8_t texture;
    };
    constexpr int kMaxParts = 3;
    struct Parts
    {
        Part p[kMaxParts] = {};
        int n = 0;
    };

    // The slots this layer drives. Hat / glasses / ... / gloves: not yet (no looks).
    inline bool Managed(inv::Equip e) { return e == inv::Equip::Shirt || e == inv::Equip::Pants || e == inv::Equip::Shoes; }

    // What a managed slot shows with nothing worn in it.
    inline Parts Bare(inv::Equip e, Sex s)
    {
        Parts r;
        const bool m = s == Sex::Male;
        switch (e) {
        case inv::Equip::Shirt:
            r.p[0] = {11, 15, 0};                              // top: none (male) / the bra (female)
            r.p[1] = {3, 15, 0};                               // arms: the whole bare torso (the tee's 3/8 is forearms only)
            r.p[2] = {8, (int16_t)(m ? 15 : 14), 0};          // undershirt: none
            r.n = 3;
            break;
        case inv::Equip::Pants:
            r.p[0] = {4, (int16_t)(m ? 61 : 15), 0};          // legs: white boxers (male) / black briefs (female)
            r.n = 1;
            break;
        case inv::Equip::Shoes:
            r.p[0] = {6, (int16_t)(m ? 34 : 35), 0};          // feet: bare
            r.n = 1;
            break;
        default:
            break;
        }
        return r;
    }

    // What a worn item puts on the ped. n == 0: the item has no look on this sex (the ped's component is left as it is).
    // The three starter items are the spawn outfit's own pieces (engine.cpp kPlayerOutfit; the engine checks once that they still agree and
    // logs a warning if someone changes one side only). Female looks: none yet (the engine never makes a female ped today).
    inline Parts Worn(inv::ItemId id, Sex s)
    {
        Parts r;
        if (s != Sex::Male)
            return r;
        const inv::ItemDef* d = inv::Find(id);
        if (!d)
            return r;
        const std::string key = d->key;
        if (key == "tshirt") {
            r.p[0] = {11, 8, 10};   // top: U_FMM_8_10 Sky Blue Tee (the branded tee: brand_apparel.h kTop*)
            r.p[1] = {3, 8, 0};     // arms: forearms, the pair of the Tee tops (v-besttorso)
            r.p[2] = {8, 15, 0};    // undershirt: none under the tee
            r.n = 3;
        } else if (key == "jeans") {
            r.p[0] = {4, 10, 0};    // legs: L_FMM_10_0 Black Pants
            r.n = 1;
        } else if (key == "sneakers") {
            r.p[0] = {6, 8, 0};     // feet: F_FMM_8_0 All-White Athletic Shoes
            r.n = 1;
        }
        return r;
    }

    // The components the worn items ask for: per managed slot, the worn item's parts (bare when nothing is worn). want[c] false = this layer
    // has no opinion about component c (the spawn outfit's value stands).
    struct Want
    {
        bool want[kComponents] = {};
        int16_t drawable[kComponents] = {};
        uint8_t texture[kComponents] = {};
        int Count() const
        {
            int n = 0;
            for (bool w : want)
                n += w ? 1 : 0;
            return n;
        }
        bool operator==(const Want& o) const
        {
            for (int c = 0; c < kComponents; ++c)
                if (want[c] != o.want[c] || (want[c] && (drawable[c] != o.drawable[c] || texture[c] != o.texture[c])))
                    return false;
            return true;
        }
        bool operator!=(const Want& o) const { return !(*this == o); }
    };
    inline void Put(Want& w, const Parts& p)
    {
        for (int i = 0; i < p.n; ++i) {
            const Part& q = p.p[i];
            if (q.component < 0 || q.component >= kComponents)
                continue;
            w.want[q.component] = true;
            w.drawable[q.component] = q.drawable;
            w.texture[q.component] = q.texture;
        }
    }
    inline Want Desired(const inv::Character& ch, Sex s)
    {
        Want w;
        for (int i = 0; i < inv::kEquipCount; ++i) {
            const inv::Equip e = inv::EquipAt(i);
            if (!Managed(e))
                continue;
            const inv::Slot& slot = ch.equip[i];
            if (slot.Empty()) {
                Put(w, Bare(e, s));
                continue;
            }
            Put(w, Worn(slot.item, s));   // an item without a look on this sex: no opinion (left as it is)
        }
        return w;
    }

    // The components (bit c) where the ped (drawable / texture as read back) differs from what is wanted.
    inline uint32_t Mismatch(const Want& w, const int16_t drawable[kComponents], const uint8_t texture[kComponents])
    {
        uint32_t m = 0;
        for (int c = 0; c < kComponents; ++c)
            if (w.want[c] && (drawable[c] != w.drawable[c] || texture[c] != w.texture[c]))
                m |= 1u << c;
        return m;
    }

}  // namespace gage::sync::outfit
