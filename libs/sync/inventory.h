// inventory.h (0.6 inventory, GI's REV3 design: HUD_DESIGN_COMPONENTS/REV3/BUILD_SPEC.md section 8 "Rules and behaviour"). Pure C++17, no game,
// no ImGui, no network: the item catalog, the containers, and every rule that decides whether a move, a merge, a swap, a split, an equip or a
// quick-slot binding is allowed. The SERVER runs these rules on its own copy (it is the authority: a client only asks); the overlay runs the same
// functions to show what would happen (drop-target / denied states) before it asks. tests/inventory_tests.h pins them to the spec.
//
// Units: weight in GRAMS per unit (shown in kg to one decimal), quantities as counts, condition 0..100 (kNoCondition = the item has none).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gage::sync::inv {

    // ---- the catalog -----------------------------------------------------------------------------------------------------------------
    enum class Category : uint8_t { Tools, Food, Medical, Docs, Clothing };
    // The 14 equipment slots, in the CHARACTER panel's order (left column top to bottom, then the right column).
    enum class Equip : uint8_t { None, Hat, Glasses, Mask, Ears, Neck, Watch, Wrist, Jacket, Shirt, Armor, Pack, Gloves, Pants, Shoes };
    constexpr int kEquipCount = 14;   // Hat .. Shoes
    inline int EquipIndex(Equip e) { return e == Equip::None ? -1 : (int)e - 1; }
    inline Equip EquipAt(int i) { return i >= 0 && i < kEquipCount ? (Equip)(i + 1) : Equip::None; }
    // Caption under an empty equipment slot (5.2) and its glyph (icons/svg/<glyph>.svg).
    inline const char* EquipCaption(Equip e)
    {
        static const char* const k[kEquipCount] = {"HAT", "GLASSES", "MASK", "EARS", "NECK", "WATCH", "WRIST", "JACKET", "SHIRT", "ARMOR", "PACK", "GLOVES", "PANTS", "SHOES"};
        const int i = EquipIndex(e);
        return i < 0 ? "" : k[i];
    }
    inline const char* EquipGlyph(Equip e)
    {
        static const char* const k[kEquipCount] = {"hat", "glasses", "mask", "ears", "chain", "watch", "bracelet", "jacket", "tee", "vest", "backpack", "gloves", "pants", "shoe"};
        const int i = EquipIndex(e);
        return i < 0 ? "" : k[i];
    }

    constexpr uint8_t kNoCondition = 255;
    using ItemId = uint16_t;   // 0 = no item

    struct ItemDef
    {
        ItemId id;
        const char* key;        // stable name for commands and saves ("medkit")
        const char* name;       // shown ("Medkit")
        const char* glyph;      // icons/svg/<glyph>.svg
        uint32_t unitGrams;
        uint32_t stackMax;      // 1 = never stacks
        Category cat;
        bool condition;         // carries a 0..100 condition (tools that wear); such items never stack
        Equip equip;            // the equipment slot it is worn in (None = not wearable)
        uint8_t packTier;       // 4 / 8 / 16 for a backpack, else 0
        const char* desc;
    };

    // Ids are part of saves and the wire: append, never renumber. The first 17 are the spec's reference items (section 9); the stack sizes are
    // the spec's where it shows one (Medkit "2 / 5") and assumptions otherwise (assumption list in the spec, section 11).
    inline const std::vector<ItemDef>& Catalog()
    {
        static const std::vector<ItemDef> k = {
            // The descriptions say only what is true today: an item whose use is not built yet is described, never promised (USE is off for
            // it: UseEffect below). The spec's own texts ("Restores 35 HP over 8 s") were placeholders.
            {1, "phone", "Phone", "phone", 200, 1, Category::Tools, false, Equip::None, 0, "A smartphone."},
            {2, "id_card", "ID card", "card", 20, 1, Category::Docs, false, Equip::None, 0, "Your name and photo."},
            {3, "car_key", "Car key", "key", 50, 1, Category::Tools, false, Equip::None, 0, "A key for one vehicle."},
            {4, "cash", "Cash", "cash", 0, 1000000, Category::Docs, false, Equip::None, 0, "Paper money. Weighs nothing."},
            {5, "water", "Water", "bottle", 500, 10, Category::Food, false, Equip::None, 0, "Restores 15 satiety. Single use."},
            {6, "sandwich", "Sandwich", "burger", 300, 10, Category::Food, false, Equip::None, 0, "Restores 40 satiety. Single use."},
            {7, "medkit", "Medkit", "medkit", 600, 5, Category::Medical, false, Equip::None, 0, "Restores 35 HP. Single use."},
            {8, "bandage", "Bandage", "bandage", 50, 20, Category::Medical, false, Equip::None, 0, "Restores 10 HP. Single use."},
            {9, "flashlight", "Flashlight", "torch", 400, 1, Category::Tools, true, Equip::None, 0, "A hand torch."},
            {10, "radio", "Radio", "radio", 350, 1, Category::Tools, true, Equip::None, 0, "A two-way radio."},
            {11, "notebook", "Notebook", "notebook", 200, 1, Category::Docs, false, Equip::None, 0, "Paper for notes."},
            {12, "city_map", "City map", "map", 100, 1, Category::Docs, false, Equip::None, 0, "A folded map of the city."},
            {13, "fuel_can", "Fuel can", "can", 4000, 1, Category::Tools, true, Equip::None, 0, "A full jerry can."},
            {14, "toolkit", "Toolkit", "wrench", 2200, 1, Category::Tools, true, Equip::None, 0, "Spanners, pliers and a jack."},
            {15, "battery", "Battery", "battery", 200, 10, Category::Tools, false, Equip::None, 0, "A spare battery."},
            {16, "camera", "Camera", "camera", 500, 1, Category::Tools, true, Equip::None, 0, "A compact camera."},
            {17, "supply_crate", "Supply crate", "box", 1500, 1, Category::Tools, false, Equip::None, 0, "A sealed crate of supplies."},
            // backpacks: worn in PACK (section 8: POUCH / DAYPACK / RUCKSACK are the spec's placeholder names)
            {30, "pouch", "Pouch", "backpack", 300, 1, Category::Clothing, false, Equip::Pack, 4, "4 extra slots, 5 kg."},
            {31, "daypack", "Daypack", "backpack", 600, 1, Category::Clothing, false, Equip::Pack, 8, "8 extra slots, 12 kg."},
            {32, "rucksack", "Rucksack", "backpack", 1200, 1, Category::Clothing, false, Equip::Pack, 16, "16 extra slots, 25 kg."},
            // clothing: one generic item per slot (the ped component / prop it maps to is section 8's table; looks come later)
            {40, "cap", "Cap", "hat", 150, 1, Category::Clothing, false, Equip::Hat, 0, "A plain cap."},
            {41, "sunglasses", "Sunglasses", "glasses", 50, 1, Category::Clothing, false, Equip::Glasses, 0, "Dark lenses."},
            {42, "bandana", "Bandana", "mask", 50, 1, Category::Clothing, false, Equip::Mask, 0, "Covers the face."},
            {43, "earpiece", "Earpiece", "ears", 20, 1, Category::Clothing, false, Equip::Ears, 0, "A small earpiece."},
            {44, "chain", "Chain", "chain", 80, 1, Category::Clothing, false, Equip::Neck, 0, "A neck chain."},
            {45, "watch", "Watch", "watch", 100, 1, Category::Clothing, false, Equip::Watch, 0, "Tells the time."},
            {46, "bracelet", "Bracelet", "bracelet", 40, 1, Category::Clothing, false, Equip::Wrist, 0, "A wrist bracelet."},
            {47, "jacket", "Jacket", "jacket", 900, 1, Category::Clothing, false, Equip::Jacket, 0, "A warm jacket."},
            // 0.5.4 outfit_items: tshirt / jeans / sneakers are the spawn outfit's own pieces (libs/sync/outfit.h Worn): taking one off
            // really takes it off the ped. Keys stay (saves and the starter kit name them); the shown names say what the ped wears.
            {48, "tshirt", "T-shirt", "tee", 200, 1, Category::Clothing, false, Equip::Shirt, 0, "A sky-blue tee."},
            {49, "vest", "Vest", "vest", 3000, 1, Category::Clothing, true, Equip::Armor, 0, "Body armour."},
            {50, "gloves", "Gloves", "gloves", 150, 1, Category::Clothing, false, Equip::Gloves, 0, "Work gloves."},
            {51, "jeans", "Black pants", "pants", 600, 1, Category::Clothing, false, Equip::Pants, 0, "Plain black trousers."},
            {52, "sneakers", "Sneakers", "shoe", 700, 1, Category::Clothing, false, Equip::Shoes, 0, "White athletic shoes."},
        };
        return k;
    }
    inline const ItemDef* Find(ItemId id)
    {
        if (id == 0)
            return nullptr;
        for (const ItemDef& d : Catalog())
            if (d.id == id)
                return &d;
        return nullptr;
    }
    inline const ItemDef* FindKey(const std::string& key)
    {
        for (const ItemDef& d : Catalog())
            if (key == d.key)
                return &d;
        return nullptr;
    }
    inline const char* CategoryName(Category c)
    {
        switch (c) {
        case Category::Tools: return "TOOLS";
        case Category::Food: return "FOOD";
        case Category::Medical: return "MEDICAL";
        case Category::Docs: return "DOCS";
        case Category::Clothing: return "CLOTHING";
        }
        return "";
    }

    // ---- containers ------------------------------------------------------------------------------------------------------------------
    enum class Kind : uint8_t { Pockets, Pack, Glovebox, Trunk, Stash, Ground };
    struct Slot
    {
        ItemId item = 0;
        uint32_t qty = 0;
        uint8_t cond = kNoCondition;
        bool Empty() const { return item == 0 || qty == 0; }
        void Clear() { item = 0; qty = 0; cond = kNoCondition; }
    };
    struct Container
    {
        Kind kind = Kind::Pockets;
        uint32_t maxGrams = 0;   // 0 = no weight cap (the ground)
        std::vector<Slot> slots;
    };

    // Section 8: pockets are always 16 slots / 15 kg; the backpack tiers.
    constexpr int kPocketSlots = 16;
    constexpr uint32_t kPocketGrams = 15000;
    struct PackTier { int slots; const char* name; uint32_t maxGrams; };
    inline PackTier PackFor(int tier)
    {
        switch (tier) {
        case 4: return {4, "POUCH", 5000};
        case 8: return {8, "DAYPACK", 12000};
        case 16: return {16, "RUCKSACK", 25000};
        default: return {0, "", 0};
        }
    }
    inline Container MakePockets() { Container c; c.kind = Kind::Pockets; c.maxGrams = kPocketGrams; c.slots.resize(kPocketSlots); return c; }
    inline Container MakePack(int tier) { const PackTier t = PackFor(tier); Container c; c.kind = Kind::Pack; c.maxGrams = t.maxGrams; c.slots.resize((size_t)t.slots); return c; }

    inline uint64_t SlotGrams(const Slot& s)
    {
        const ItemDef* d = Find(s.item);
        return s.Empty() || !d ? 0 : (uint64_t)d->unitGrams * s.qty;
    }
    inline uint64_t Grams(const Container& c)
    {
        uint64_t g = 0;
        for (const Slot& s : c.slots)
            g += SlotGrams(s);
        return g;
    }
    inline int UsedSlots(const Container& c)
    {
        int n = 0;
        for (const Slot& s : c.slots)
            n += s.Empty() ? 0 : 1;
        return n;
    }

    // Section 5.4: under 0.9 normal, 0.9 or more heavy (ember), 1.0 full (ember + pulsing bar, pickups blocked).
    enum class Load : uint8_t { Normal, Heavy, Full };
    inline Load LoadOf(uint64_t grams, uint64_t cap)
    {
        if (cap == 0)
            return Load::Normal;
        if (grams >= cap)
            return Load::Full;
        return grams * 10 >= cap * 9 ? Load::Heavy : Load::Normal;
    }

    // ---- moves -------------------------------------------------------------------------------------------------------------------------
    enum class Result : uint8_t {
        Moved,          // the whole asked quantity went
        MovedPart,      // only the part that fits (weight or stack room)
        Merged,         // onto the same item (whole)
        Swapped,
        Nothing,        // same slot, or quantity 0
        BadSlot,        // an index outside a container, or an empty source
        Weight,         // nothing fits by weight
        StackFull,      // the target stack has no room
        Occupied,       // a part of a stack onto a different item (only whole stacks swap)
        WrongSlot,      // an item that is not worn there / a pack into a pack
        PackNotEmpty,   // a pack must be empty to take off
        NoRoom,         // a quick move / unequip found no slot that takes it
        NotUsable,      // USE on an item with no use (yet)
        Bound,          // a quick-slot bind / unbind (no item moved)
        Used,           // one unit consumed by USE (the effect is the server's: ApplyOut::used)
        Dropped,        // removed from the inventory (round 1 has no ground: a drop discards)
    };
    inline bool Ok(Result r)
    {
        return r == Result::Moved || r == Result::MovedPart || r == Result::Merged || r == Result::Swapped || r == Result::Bound || r == Result::Used ||
               r == Result::Dropped;
    }
    inline const char* ResultName(Result r)
    {
        switch (r) {
        case Result::Moved: return "moved";
        case Result::MovedPart: return "moved part";
        case Result::Merged: return "merged";
        case Result::Swapped: return "swapped";
        case Result::Nothing: return "nothing to do";
        case Result::BadSlot: return "no such slot";
        case Result::Weight: return "too heavy";
        case Result::StackFull: return "stack full";
        case Result::Occupied: return "slot taken";
        case Result::WrongSlot: return "does not go there";
        case Result::PackNotEmpty: return "empty the pack first";
        case Result::NoRoom: return "no room";
        case Result::NotUsable: return "cannot be used";
        case Result::Bound: return "bound";
        case Result::Used: return "used";
        case Result::Dropped: return "dropped";
        }
        return "";
    }

    // How many units of an item fit into a container by weight (all of them when it weighs nothing or the container has no cap).
    inline uint32_t FitByWeight(const Container& dst, ItemId item, uint32_t want, uint64_t freedGrams = 0)
    {
        const ItemDef* d = Find(item);
        if (!d || d->unitGrams == 0 || dst.maxGrams == 0)
            return want;
        const uint64_t used = Grams(dst) - freedGrams;
        if (used >= dst.maxGrams)
            return 0;
        const uint64_t n = (dst.maxGrams - used) / d->unitGrams;
        return n < want ? (uint32_t)n : want;
    }
    // A backpack never goes into a pack (no packs in packs). Everything else goes anywhere.
    inline bool Accepts(const Container& dst, ItemId item)
    {
        const ItemDef* d = Find(item);
        return d && !(dst.kind == Kind::Pack && d->packTier != 0);
    }
    inline bool Stacks(const ItemDef& d) { return d.stackMax > 1 && !d.condition; }

    // Move qty units (0 = the whole stack) from src[si] to dst[di]. src and dst may be the same container. The rules (section 8):
    // a stack moves whole if it fits, otherwise the part that fits; the same item merges up to its stack size; a different item swaps, whole
    // stacks only, when both sides' weights still fit.
    inline Result Move(Container& src, int si, Container& dst, int di, uint32_t qty = 0)
    {
        if (si < 0 || di < 0 || si >= (int)src.slots.size() || di >= (int)dst.slots.size() || src.slots[(size_t)si].Empty())
            return Result::BadSlot;
        Slot& a = src.slots[(size_t)si];
        Slot& b = dst.slots[(size_t)di];
        const bool same = &src == &dst;
        if (same && si == di)
            return Result::Nothing;
        if (qty == 0 || qty > a.qty)
            qty = a.qty;
        const ItemDef* da = Find(a.item);
        if (!da)
            return Result::BadSlot;
        if (!Accepts(dst, a.item))
            return Result::WrongSlot;
        if (b.Empty()) {
            const uint32_t n = same ? qty : FitByWeight(dst, a.item, qty);
            if (n == 0)
                return Result::Weight;
            b.item = a.item;
            b.qty = n;
            b.cond = a.cond;
            a.qty -= n;
            if (a.qty == 0)
                a.Clear();
            return n == qty ? Result::Moved : Result::MovedPart;
        }
        if (b.item == a.item && Stacks(*da)) {
            const uint32_t room = b.qty >= da->stackMax ? 0 : da->stackMax - b.qty;
            if (room == 0)
                return Result::StackFull;
            uint32_t n = qty < room ? qty : room;
            if (!same)
                n = FitByWeight(dst, a.item, n);
            if (n == 0)
                return Result::Weight;
            b.qty += n;
            a.qty -= n;
            if (a.qty == 0)
                a.Clear();
            return n == qty ? Result::Merged : Result::MovedPart;
        }
        // a different item (or a non-stacking one of the same kind): a swap of whole stacks
        if (qty != a.qty)
            return Result::Occupied;
        if (!Accepts(src, b.item))
            return Result::WrongSlot;
        if (!same) {
            const uint64_t ga = SlotGrams(a), gb = SlotGrams(b);
            const bool dstOk = dst.maxGrams == 0 || Grams(dst) - gb + ga <= dst.maxGrams;
            const bool srcOk = src.maxGrams == 0 || Grams(src) - ga + gb <= src.maxGrams;
            if (!dstOk || !srcOk)
                return Result::Weight;
        }
        const Slot t = a;
        a = b;
        b = t;
        return Result::Swapped;
    }

    // Split n units of src[si] into the EMPTY slot dst[di] (Ctrl+drag / the split dialog). 1 <= n < qty.
    inline Result Split(Container& src, int si, Container& dst, int di, uint32_t n)
    {
        if (si < 0 || di < 0 || si >= (int)src.slots.size() || di >= (int)dst.slots.size() || src.slots[(size_t)si].Empty())
            return Result::BadSlot;
        if (n == 0 || n >= src.slots[(size_t)si].qty)
            return Result::Nothing;
        if (!dst.slots[(size_t)di].Empty())
            return Result::Occupied;
        return Move(src, si, dst, di, n);
    }
    // The split dialog's fraction chips (5.5): 1/2, 1/3, 2/3, 1/4, 3/4 of the stack, at least 1 and at most qty - 1.
    inline uint32_t SplitAmount(uint32_t qty, int num, int den)
    {
        if (qty < 2 || den <= 0 || num <= 0)
            return 0;
        uint64_t n = ((uint64_t)qty * (uint64_t)num + (uint64_t)den / 2) / (uint64_t)den;
        if (n < 1)
            n = 1;
        if (n > qty - 1)
            n = qty - 1;
        return (uint32_t)n;
    }

    // Shift+click quick move: where an item from one side lands on the other: the first stack of the same item with room, else the first empty
    // slot that accepts it. -1 = nowhere.
    inline int QuickTarget(const Container& dst, ItemId item)
    {
        const ItemDef* d = Find(item);
        if (!d || !Accepts(dst, item))
            return -1;
        if (Stacks(*d))
            for (size_t i = 0; i < dst.slots.size(); ++i)
                if (dst.slots[i].item == item && dst.slots[i].qty < d->stackMax)
                    return (int)i;
        for (size_t i = 0; i < dst.slots.size(); ++i)
            if (dst.slots[i].Empty())
                return (int)i;
        return -1;
    }

    // Add n units of an item (a pickup, an admin grant): fills stacks with room, then empty slots, within the weight cap. Returns what went in.
    inline uint32_t Add(Container& c, ItemId item, uint32_t n, uint8_t cond = kNoCondition)
    {
        const ItemDef* d = Find(item);
        if (!d || n == 0 || !Accepts(c, item))
            return 0;
        uint32_t left = FitByWeight(c, item, n);
        const uint32_t fit = left;
        if (Stacks(*d))
            for (Slot& s : c.slots)
                if (left && s.item == item && s.qty < d->stackMax) {
                    const uint32_t k = d->stackMax - s.qty < left ? d->stackMax - s.qty : left;
                    s.qty += k;
                    left -= k;
                }
        for (Slot& s : c.slots) {
            if (!left)
                break;
            if (!s.Empty())
                continue;
            const uint32_t k = d->stackMax < left ? d->stackMax : left;
            s.item = item;
            s.qty = k;
            s.cond = d->condition ? (cond == kNoCondition ? 100 : cond) : kNoCondition;
            left -= k;
        }
        return fit - left;
    }

    // ---- the character: pockets, equipment, the pack, quick slots ------------------------------------------------------------------------
    struct QuickRef
    {
        ItemId item = 0;          // what the slot is bound to (kept when the stack runs out: drawn at 0.35 until the same item arrives)
        Kind kind = Kind::Pockets;
        int index = -1;           // where it was last seen (pockets or pack)
    };
    struct Character
    {
        Container pockets = MakePockets();
        Slot equip[kEquipCount];
        Container pack;           // valid while equip[Pack] holds a backpack: its tier's slots
        QuickRef quick[4];

        int PackTierNow() const
        {
            const ItemDef* d = Find(equip[EquipIndex(Equip::Pack)].item);
            return d ? d->packTier : 0;
        }
        bool HasPack() const { return PackTierNow() != 0; }
        uint64_t CarryCap() const { return kPocketGrams + (HasPack() ? PackFor(PackTierNow()).maxGrams : 0); }
        uint64_t Carried() const { return Grams(pockets) + (HasPack() ? Grams(pack) : 0); }
    };

    // Put the item in c[si] on (equipment slot e). Only an item worn there; a pack only when none is worn or the worn one is empty (the worn
    // item comes back to the source slot, a swap). Equipping a pack creates its container.
    inline Result EquipFrom(Character& ch, Container& c, int si, Equip e)
    {
        const int ei = EquipIndex(e);
        if (ei < 0 || si < 0 || si >= (int)c.slots.size() || c.slots[(size_t)si].Empty())
            return Result::BadSlot;
        const ItemDef* d = Find(c.slots[(size_t)si].item);
        if (!d || d->equip != e)
            return Result::WrongSlot;
        if (e == Equip::Pack && &c == &ch.pack)
            return Result::WrongSlot;   // a pack cannot be worn out of the pack it replaces
        Slot& w = ch.equip[ei];
        if (e == Equip::Pack && !w.Empty() && UsedSlots(ch.pack) != 0)
            return Result::PackNotEmpty;
        if (!w.Empty()) {
            // the worn one comes back into c: it must be allowed there and fit by weight once the new one has left
            if (!Accepts(c, w.item))
                return Result::WrongSlot;
            if (FitByWeight(c, w.item, 1, d->unitGrams) == 0)
                return Result::Weight;
        }
        Slot item = c.slots[(size_t)si];
        item.qty = 1;
        c.slots[(size_t)si].qty -= 1;
        if (c.slots[(size_t)si].qty == 0)
            c.slots[(size_t)si].Clear();
        if (!w.Empty()) {
            // the worn one goes back where the new one came from (that slot is empty now: a worn item is a single unit)
            c.slots[(size_t)si] = w;
        }
        w = item;
        if (e == Equip::Pack)
            ch.pack = MakePack(d->packTier);
        return Result::Swapped;
    }
    // Take the worn item off into c[di] (an empty slot that accepts it). A pack must be empty.
    inline Result Unequip(Character& ch, Equip e, Container& c, int di)
    {
        const int ei = EquipIndex(e);
        if (ei < 0 || di < 0 || di >= (int)c.slots.size() || ch.equip[ei].Empty())
            return Result::BadSlot;
        if (!c.slots[(size_t)di].Empty())
            return Result::Occupied;
        if (e == Equip::Pack && UsedSlots(ch.pack) != 0)
            return Result::PackNotEmpty;
        if (e == Equip::Pack && &c == &ch.pack)
            return Result::WrongSlot;
        if (!Accepts(c, ch.equip[ei].item))
            return Result::WrongSlot;
        if (FitByWeight(c, ch.equip[ei].item, 1) == 0)
            return Result::Weight;
        c.slots[(size_t)di] = ch.equip[ei];
        ch.equip[ei].Clear();
        if (e == Equip::Pack)
            ch.pack = Container();
        return Result::Moved;
    }

    // Quick slots are references (section 8): bind to an item in pockets or pack; Resolve finds it again after moves (the remembered place
    // first, then the first stack of that item in pockets, then pack). false = the item ran out (drawn dim, re-links when it comes back).
    inline bool Resolve(const Character& ch, QuickRef& q)
    {
        if (q.item == 0)
            return false;
        const auto at = [&](Kind k, int i) -> const Slot* {
            const Container& c = k == Kind::Pack ? ch.pack : ch.pockets;
            return i >= 0 && i < (int)c.slots.size() ? &c.slots[(size_t)i] : nullptr;
        };
        if (const Slot* s = at(q.kind, q.index))
            if (s->item == q.item && !s->Empty())
                return true;
        const Container* order[2] = {&ch.pockets, ch.HasPack() ? &ch.pack : nullptr};
        for (int k = 0; k < 2; ++k) {
            if (!order[k])
                continue;
            for (size_t i = 0; i < order[k]->slots.size(); ++i)
                if (order[k]->slots[i].item == q.item && !order[k]->slots[i].Empty()) {
                    q.kind = k == 0 ? Kind::Pockets : Kind::Pack;
                    q.index = (int)i;
                    return true;
                }
        }
        return false;
    }

    // ---- operations: what a client asks for and what the server applies -------------------------------------------------------------------
    // ONE function (Apply) on both sides: the server runs it on its authoritative copy, the overlay runs it on its prediction so a drag lands at
    // once, and drops its prediction for the server's next snapshot. Round 1 (0.6) has no nearby containers: Where names the four boxes a
    // character owns. Indexes: a container slot, an equipment slot (EquipIndex), or a quick slot 0..3.
    enum class Where : uint8_t { Pockets, Pack, Equip, Quick };
    enum class OpKind : uint8_t {
        Move,       // src -> dst, qty (0 = whole): container <-> container, container -> equip (equip), equip -> container (take off / swap), quick <-> quick
        Split,      // container -> EMPTY container slot, qty = n (1..qty-1)
        QuickMove,  // Shift+click: pockets <-> pack, a worn item to the first free slot (pockets, then pack)
        Use,        // src: one unit of a usable item, or equip a wearable / take a worn one off
        Drop,       // src, qty (0 = whole): round 1 discards (no ground yet)
        Bind,       // quick slot di <- the item in src (a reference, nothing moves)
        Unbind,     // quick slot di cleared
        UseQuick,   // Use on whatever quick slot si resolves to (keys 1..4)
    };
    constexpr int kOpKinds = 8;
    struct Op
    {
        OpKind kind = OpKind::Move;
        Where src = Where::Pockets;
        int16_t si = -1;
        Where dst = Where::Pockets;
        int16_t di = -1;
        uint32_t qty = 0;
    };
    struct ApplyOut
    {
        Result r = Result::BadSlot;
        ItemId used = 0;   // Result::Used: the item consumed (the server applies its UseEffect)
    };

    // What USE does (the server applies it: health is the server's, satiety is the client's, told by the server). Zero = not usable yet.
    struct Effect { int hp; int satiety; };
    inline Effect UseEffect(ItemId id)
    {
        switch (id) {
        case 5: return {0, 15};    // water
        case 6: return {0, 40};    // sandwich
        case 7: return {35, 0};    // medkit
        case 8: return {10, 0};    // bandage
        default: return {0, 0};
        }
    }
    inline bool Usable(ItemId id) { const Effect e = UseEffect(id); return e.hp != 0 || e.satiety != 0; }
    inline bool Wearable(ItemId id) { const ItemDef* d = Find(id); return d && d->equip != Equip::None; }

    inline Container* BoxOf(Character& ch, Where w)
    {
        if (w == Where::Pockets)
            return &ch.pockets;
        if (w == Where::Pack)
            return ch.HasPack() ? &ch.pack : nullptr;
        return nullptr;
    }
    inline const Container* BoxOf(const Character& ch, Where w) { return BoxOf(const_cast<Character&>(ch), w); }
    inline const Slot* SlotAt(const Character& ch, Where w, int i)
    {
        if (w == Where::Equip)
            return i >= 0 && i < kEquipCount ? &ch.equip[i] : nullptr;
        const Container* c = BoxOf(ch, w);
        return c && i >= 0 && i < (int)c->slots.size() ? &c->slots[(size_t)i] : nullptr;
    }

    // Take the worn item in equipment slot ei off into the first empty slot that takes it (pockets, then the pack).
    inline Result TakeOffAnywhere(Character& ch, int ei)
    {
        if (ei < 0 || ei >= kEquipCount || ch.equip[ei].Empty())
            return Result::BadSlot;
        Result last = Result::NoRoom;
        for (Where w : {Where::Pockets, Where::Pack}) {
            Container* c = BoxOf(ch, w);
            if (!c)
                continue;
            for (size_t i = 0; i < c->slots.size(); ++i)
                if (c->slots[i].Empty()) {
                    const Result r = Unequip(ch, EquipAt(ei), *c, (int)i);
                    if (Ok(r) || r == Result::PackNotEmpty)
                        return r;
                    last = r;   // too heavy here / not accepted here: try the next container
                    break;
                }
        }
        return last == Result::WrongSlot || last == Result::Weight ? last : Result::NoRoom;
    }

    inline ApplyOut Apply(Character& ch, const Op& op)
    {
        ApplyOut out;
        const auto isBox = [](Where w) { return w == Where::Pockets || w == Where::Pack; };
        switch (op.kind) {
        case OpKind::Move: {
            if (isBox(op.src) && isBox(op.dst)) {
                Container* a = BoxOf(ch, op.src);
                Container* b = BoxOf(ch, op.dst);
                out.r = a && b ? Move(*a, op.si, *b, op.di, op.qty) : Result::BadSlot;
            } else if (isBox(op.src) && op.dst == Where::Equip) {
                Container* a = BoxOf(ch, op.src);
                out.r = a ? EquipFrom(ch, *a, op.si, EquipAt(op.di)) : Result::BadSlot;
            } else if (op.src == Where::Equip && isBox(op.dst)) {
                Container* b = BoxOf(ch, op.dst);
                const Equip e = EquipAt(op.si);
                if (!b || e == Equip::None || ch.equip[op.si].Empty() || op.di < 0 || op.di >= (int)b->slots.size())
                    out.r = Result::BadSlot;
                else if (b->slots[(size_t)op.di].Empty())
                    out.r = Unequip(ch, e, *b, op.di);
                else {
                    // onto an item worn in the same slot: wear that one, the worn one takes its place (a swap)
                    const ItemDef* d = Find(b->slots[(size_t)op.di].item);
                    out.r = d && d->equip == e ? EquipFrom(ch, *b, op.di, e) : Result::Occupied;
                }
            } else if (op.src == Where::Quick && op.dst == Where::Quick) {
                if (op.si < 0 || op.si > 3 || op.di < 0 || op.di > 3)
                    out.r = Result::BadSlot;
                else if (op.si == op.di)
                    out.r = Result::Nothing;
                else {
                    const QuickRef t = ch.quick[op.si];
                    ch.quick[op.si] = ch.quick[op.di];
                    ch.quick[op.di] = t;
                    out.r = Result::Bound;
                }
            } else {
                out.r = Result::WrongSlot;
            }
            return out;
        }
        case OpKind::Split: {
            Container* a = isBox(op.src) ? BoxOf(ch, op.src) : nullptr;
            Container* b = isBox(op.dst) ? BoxOf(ch, op.dst) : nullptr;
            out.r = a && b ? Split(*a, op.si, *b, op.di, op.qty) : Result::BadSlot;
            return out;
        }
        case OpKind::QuickMove: {
            if (op.src == Where::Equip) {
                out.r = TakeOffAnywhere(ch, op.si);
                return out;
            }
            Container* a = isBox(op.src) ? BoxOf(ch, op.src) : nullptr;
            Container* b = op.src == Where::Pockets ? BoxOf(ch, Where::Pack) : (op.src == Where::Pack ? &ch.pockets : nullptr);
            if (!a || op.si < 0 || op.si >= (int)a->slots.size() || a->slots[(size_t)op.si].Empty()) {
                out.r = Result::BadSlot;
                return out;
            }
            if (!b) {
                out.r = Result::NoRoom;
                return out;
            }
            const ItemId item = a->slots[(size_t)op.si].item;
            if (!Accepts(*b, item)) {
                out.r = Result::WrongSlot;
                return out;
            }
            const int di = QuickTarget(*b, item);
            out.r = di < 0 ? Result::NoRoom : Move(*a, op.si, *b, di, 0);
            return out;
        }
        case OpKind::UseQuick: {
            if (op.si < 0 || op.si > 3) {
                out.r = Result::BadSlot;
                return out;
            }
            QuickRef& q = ch.quick[op.si];
            if (!Resolve(ch, q)) {
                out.r = Result::NotUsable;
                return out;
            }
            Op u;
            u.kind = OpKind::Use;
            u.src = q.kind == Kind::Pack ? Where::Pack : Where::Pockets;
            u.si = (int16_t)q.index;
            return Apply(ch, u);
        }
        case OpKind::Use: {
            if (op.src == Where::Equip) {
                out.r = TakeOffAnywhere(ch, op.si);
                return out;
            }
            Container* a = isBox(op.src) ? BoxOf(ch, op.src) : nullptr;
            if (!a || op.si < 0 || op.si >= (int)a->slots.size() || a->slots[(size_t)op.si].Empty()) {
                out.r = Result::BadSlot;
                return out;
            }
            Slot& s = a->slots[(size_t)op.si];
            const ItemDef* d = Find(s.item);
            if (d && d->equip != Equip::None) {
                out.r = EquipFrom(ch, *a, op.si, d->equip);
                return out;
            }
            if (!Usable(s.item)) {
                out.r = Result::NotUsable;
                return out;
            }
            out.used = s.item;
            if (--s.qty == 0)
                s.Clear();
            out.r = Result::Used;
            return out;
        }
        case OpKind::Drop: {
            if (op.src == Where::Equip) {
                if (op.si < 0 || op.si >= kEquipCount || ch.equip[op.si].Empty())
                    out.r = Result::BadSlot;
                else if (EquipAt(op.si) == Equip::Pack && UsedSlots(ch.pack) != 0)
                    out.r = Result::PackNotEmpty;
                else {
                    ch.equip[op.si].Clear();
                    if (EquipAt(op.si) == Equip::Pack)
                        ch.pack = Container();
                    out.r = Result::Dropped;
                }
                return out;
            }
            Container* a = isBox(op.src) ? BoxOf(ch, op.src) : nullptr;
            if (!a || op.si < 0 || op.si >= (int)a->slots.size() || a->slots[(size_t)op.si].Empty()) {
                out.r = Result::BadSlot;
                return out;
            }
            Slot& s = a->slots[(size_t)op.si];
            if (op.qty == 0 || op.qty >= s.qty)
                s.Clear();
            else
                s.qty -= op.qty;
            out.r = Result::Dropped;
            return out;
        }
        case OpKind::Bind: {
            const Slot* s = isBox(op.src) ? SlotAt(ch, op.src, op.si) : nullptr;
            if (!s || s->Empty() || op.di < 0 || op.di > 3) {
                out.r = Result::BadSlot;
                return out;
            }
            for (QuickRef& q : ch.quick)   // one slot per item: binding it again moves the binding
                if (q.item == s->item)
                    q = QuickRef();
            ch.quick[op.di].item = s->item;
            ch.quick[op.di].kind = op.src == Where::Pack ? Kind::Pack : Kind::Pockets;
            ch.quick[op.di].index = op.si;
            out.r = Result::Bound;
            return out;
        }
        case OpKind::Unbind: {
            if (op.di < 0 || op.di > 3) {
                out.r = Result::BadSlot;
                return out;
            }
            ch.quick[op.di] = QuickRef();
            out.r = Result::Bound;
            return out;
        }
        }
        return out;
    }

    // ---- wire and save form ---------------------------------------------------------------------------------------------------------------
    // A Character as bytes (the server's InvState message and its save files): version, the 16 pocket slots, the 14 equipment slots, the pack's
    // slot count and slots, the 4 quick refs. Little-endian. Decode refuses anything the rules could not have produced (an unknown item, a
    // quantity over its stack, a worn item in the wrong slot, a pack whose size is not its tier's), so a corrupt save or packet never becomes
    // an inventory.
    constexpr uint8_t kCodecVersion = 1;
    namespace codec {
        inline void Put8(std::string& b, uint8_t v) { b.push_back((char)v); }
        inline void Put16(std::string& b, uint16_t v) { Put8(b, (uint8_t)v); Put8(b, (uint8_t)(v >> 8)); }
        inline void Put32(std::string& b, uint32_t v) { Put16(b, (uint16_t)v); Put16(b, (uint16_t)(v >> 16)); }
        struct Rd
        {
            const std::string& b;
            size_t p = 0;
            bool ok = true;
            uint8_t U8() { if (p + 1 > b.size()) { ok = false; return 0; } return (uint8_t)b[p++]; }
            uint16_t U16() { const uint16_t lo = U8(); return (uint16_t)(lo | (uint16_t)U8() << 8); }
            uint32_t U32() { const uint32_t lo = U16(); return lo | (uint32_t)U16() << 16; }
        };
        inline void PutSlot(std::string& b, const Slot& s)
        {
            const bool e = s.Empty();
            Put16(b, e ? 0 : s.item);
            Put32(b, e ? 0 : s.qty);
            Put8(b, e ? kNoCondition : s.cond);
        }
        inline bool GetSlot(Rd& r, Slot& s)
        {
            s.item = r.U16();
            s.qty = r.U32();
            s.cond = r.U8();
            if (!r.ok)
                return false;
            if (s.item == 0 || s.qty == 0) {
                s.Clear();
                return true;
            }
            const ItemDef* d = Find(s.item);
            if (!d || s.qty > d->stackMax)
                return false;
            if (d->condition ? s.cond > 100 : s.cond != kNoCondition)
                return false;
            return true;
        }
    }
    inline std::string Encode(const Character& ch)
    {
        std::string b;
        codec::Put8(b, kCodecVersion);
        for (const Slot& s : ch.pockets.slots)
            codec::PutSlot(b, s);
        for (const Slot& s : ch.equip)
            codec::PutSlot(b, s);
        codec::Put8(b, (uint8_t)(ch.HasPack() ? ch.pack.slots.size() : 0));
        if (ch.HasPack())
            for (const Slot& s : ch.pack.slots)
                codec::PutSlot(b, s);
        for (const QuickRef& q : ch.quick) {
            codec::Put16(b, q.item);
            codec::Put8(b, (uint8_t)q.kind);
            codec::Put8(b, (uint8_t)(q.index < 0 || q.index > 254 ? 255 : q.index));
        }
        return b;
    }
    inline bool Decode(const std::string& bytes, Character& out)
    {
        codec::Rd r{bytes};
        if (r.U8() != kCodecVersion)
            return false;
        Character ch;
        if ((int)ch.pockets.slots.size() != kPocketSlots)
            return false;
        for (Slot& s : ch.pockets.slots)
            if (!codec::GetSlot(r, s))
                return false;
        for (int i = 0; i < kEquipCount; ++i) {
            if (!codec::GetSlot(r, ch.equip[i]))
                return false;
            const ItemDef* d = Find(ch.equip[i].item);
            if (!ch.equip[i].Empty() && (!d || d->equip != EquipAt(i) || ch.equip[i].qty != 1))
                return false;
        }
        const int packN = r.U8();
        const int tier = ch.PackTierNow();
        if (packN != (tier ? PackFor(tier).slots : 0))
            return false;
        if (tier) {
            ch.pack = MakePack(tier);
            for (Slot& s : ch.pack.slots) {
                if (!codec::GetSlot(r, s))
                    return false;
                if (!s.Empty() && !Accepts(ch.pack, s.item))
                    return false;
            }
        }
        for (QuickRef& q : ch.quick) {
            q.item = r.U16();
            const uint8_t k = r.U8();
            const uint8_t ix = r.U8();
            if (q.item != 0 && !Find(q.item))
                return false;
            if (k != (uint8_t)Kind::Pockets && k != (uint8_t)Kind::Pack)
                return false;
            q.kind = (Kind)k;
            q.index = ix == 255 ? -1 : ix;
        }
        if (!r.ok || r.p != bytes.size())
            return false;
        out = ch;
        return true;
    }
    // An Op as 11 bytes (kind, src, si, dst, di, qty).
    inline std::string EncodeOp(const Op& op)
    {
        std::string b;
        codec::Put8(b, (uint8_t)op.kind);
        codec::Put8(b, (uint8_t)op.src);
        codec::Put16(b, (uint16_t)op.si);
        codec::Put8(b, (uint8_t)op.dst);
        codec::Put16(b, (uint16_t)op.di);
        codec::Put32(b, op.qty);
        return b;
    }
    inline bool DecodeOp(const std::string& bytes, Op& op)
    {
        codec::Rd r{bytes};
        const uint8_t k = r.U8(), s = r.U8();
        const uint16_t si = r.U16();
        const uint8_t d = r.U8();
        const uint16_t di = r.U16();
        const uint32_t q = r.U32();
        if (!r.ok || r.p != bytes.size() || k >= kOpKinds || s > (uint8_t)Where::Quick || d > (uint8_t)Where::Quick)
            return false;
        op.kind = (OpKind)k;
        op.src = (Where)s;
        op.si = (int16_t)si;
        op.dst = (Where)d;
        op.di = (int16_t)di;
        op.qty = q;
        return true;
    }

    // ---- grants (the server's admin /give and the starter kit) ----------------------------------------------------------------------------
    // n units of an item into the pockets, the rest into the pack (when one is worn and accepts it), within slots and weight. Returns what went in.
    inline uint32_t Give(Character& ch, ItemId item, uint32_t n)
    {
        uint32_t in = Add(ch.pockets, item, n);
        if (in < n && ch.HasPack())
            in += Add(ch.pack, item, n - in);
        return in;
    }
    // A new character: phone and ID in the pockets, a little food and two bandages, and the spawn outfit worn as items (0.5.4 outfit_items:
    // the tee, pants and shoes the ped spawns in; taking one off takes it off the ped, libs/sync/outfit.h). No backpack: one is an item to find
    // or buy.
    inline Character StarterKit()
    {
        Character ch;
        static const struct { const char* key; uint32_t n; } give[] = {
            {"phone", 1}, {"id_card", 1}, {"water", 2}, {"sandwich", 1}, {"bandage", 2},
        };
        for (const auto& g : give)
            if (const ItemDef* d = FindKey(g.key))
                Add(ch.pockets, d->id, g.n);
        static const char* const wear[] = {"tshirt", "jeans", "sneakers"};
        for (const char* key : wear)
            if (const ItemDef* d = FindKey(key)) {
                Slot& s = ch.equip[EquipIndex(d->equip)];
                s.item = d->id;
                s.qty = 1;
                s.cond = kNoCondition;
            }
        return ch;
    }

    // ---- vehicle storage (section 8, by GTA vehicle class GET_VEHICLE_CLASS 0..22) ------------------------------------------------------------
    struct VehicleStorage { int trunkSlots; uint32_t trunkGrams; bool glovebox; bool frontTrunkLabel; const char* trunkLabel; };
    inline VehicleStorage StorageForClass(int cls)
    {
        switch (cls) {
        case 0: return {12, 40000, true, false, "TRUNK"};                          // compacts
        case 1: return {24, 80000, true, false, "TRUNK"};                          // sedans
        case 2: case 9: case 11: return {32, 110000, true, false, "TRUNK"};        // SUVs, off-road, utility
        case 3: case 4: return {16, 60000, true, false, "TRUNK"};                  // coupes, muscle
        case 5: case 6: case 7: return {12, 40000, true, false, "TRUNK"};          // sports classics (assumed as sports), sports, super
        case 8: return {4, 15000, false, false, "TOP CASE"};                       // motorcycles: no glovebox
        case 10: case 12: case 20: return {48, 160000, true, false, "CARGO"};      // industrial (assumed as commercial), vans, commercial
        case 17: case 18: return {24, 80000, true, false, "TRUNK"};                // service, emergency (assumed as sedans)
        case 19: return {32, 110000, true, false, "TRUNK"};                        // military (assumed as SUVs)
        default: return {0, 0, false, false, ""};                                  // cycles, boats, aircraft, trains, open wheel: no storage
        }
    }
    constexpr int kGloveboxSlots = 8;
    constexpr uint32_t kGloveboxGrams = 12000;
    constexpr int kStashSlots = 48;
    constexpr uint32_t kStashGrams = 100000;

    // ---- text ----------------------------------------------------------------------------------------------------------------------------
    // The quantity tag (5.1): nothing for 1, "×N" (U+00D7, UTF-8) for 2..999, "1.2k" from 1000 (one decimal, ".0" dropped), "1.2M" from a million.
    inline std::string QtyTag(uint32_t n)
    {
        if (n <= 1)
            return std::string();
        char b[32];
        if (n < 1000) {
            std::snprintf(b, sizeof b, "\xC3\x97%u", (unsigned)n);
            return b;
        }
        const bool mega = n >= 1000000u;
        const double v = mega ? n / 1000000.0 : n / 1000.0;
        const long tenths = (long)(v * 10.0);   // truncated, so 1999 is 1.9k, never 2.0k
        if (tenths % 10 == 0)
            std::snprintf(b, sizeof b, "%ld%s", tenths / 10, mega ? "M" : "k");
        else
            std::snprintf(b, sizeof b, "%ld.%ld%s", tenths / 10, tenths % 10, mega ? "M" : "k");
        return b;
    }
    // Kilograms to one decimal ("14.2"); a cap as %g ("27", "12.5").
    inline std::string Kg(uint64_t grams)
    {
        char b[32];
        std::snprintf(b, sizeof b, "%.1f", (double)grams / 1000.0);
        return b;
    }
    inline std::string KgCap(uint64_t grams)
    {
        char b[32];
        std::snprintf(b, sizeof b, "%g", (double)grams / 1000.0);
        return b;
    }

} // namespace gage::sync::inv
