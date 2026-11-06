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
#include <utility>
#include <vector>

namespace gage::sync::inv {

    // ---- the catalog -----------------------------------------------------------------------------------------------------------------
    enum class Category : uint8_t { Tools, Food, Medical, Docs, Clothing, Weapons /* 0.5.4: WeaponCatalog() */ };
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
        uint32_t weapon = 0;    // 0.5.4: the GTA weapon hash (joaat of the key) for a WeaponCatalog() item, else 0
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
            {48, "tshirt", "T-shirt", "tee", 200, 1, Category::Clothing, false, Equip::Shirt, 0, "A cotton tee."},
            {49, "vest", "Vest", "vest", 3000, 1, Category::Clothing, true, Equip::Armor, 0, "Body armour."},
            {50, "gloves", "Gloves", "gloves", 150, 1, Category::Clothing, false, Equip::Gloves, 0, "Work gloves."},
            {51, "jeans", "Jeans", "pants", 600, 1, Category::Clothing, false, Equip::Pants, 0, "Blue jeans."},
            {52, "sneakers", "Sneakers", "shoe", 700, 1, Category::Clothing, false, Equip::Shoes, 0, "Everyday shoes."},
        };
        return k;
    }

    // ---- weapons (0.5.4 "inventory world": every weapon the ped holds is an item) ---------------------------------------------------------
    // GTA's weapon hash is the joaat of the weapon name, lower-cased (WEAPON_PISTOL = 0x1B06D571; tests/inventory_world_tests.h pins three).
    constexpr uint32_t Joaat(const char* s)
    {
        uint32_t h = 0;
        for (; *s; ++s) {
            char c = *s;
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
            h += (uint8_t)c;
            h += h << 10;
            h ^= h >> 6;
        }
        h += h << 3;
        h ^= h >> 11;
        h += h << 15;
        return h;
    }
    constexpr uint32_t kWeaponUnarmed = Joaat("weapon_unarmed");
    constexpr ItemId kFirstWeaponId = 1000;
    // One item per weapon, never stacking; the item carries the ammo (Slot::ammo). Ids 1000.. are part of saves and the wire: append only.
    // The glyph is "gun" (firearms, launchers), "blade" (melee) or "grenade" (thrown): drawn shapes, not REV3 PNGs (apps/engine overlay).
    inline const std::vector<ItemDef>& WeaponCatalog()
    {
        struct W { ItemId id; const char* key; const char* name; uint32_t grams; char kind; };   // kind: m melee, h handgun, s smg, g shotgun,
        static const W kW[] = {                                                                     // r rifle, l mg, x sniper, v heavy, t thrown
            {1000, "weapon_knife", "Knife", 300, 'm'}, {1001, "weapon_bat", "Baseball bat", 1000, 'm'}, {1002, "weapon_crowbar", "Crowbar", 1500, 'm'},
            {1003, "weapon_golfclub", "Golf club", 600, 'm'}, {1004, "weapon_hammer", "Hammer", 700, 'm'}, {1005, "weapon_nightstick", "Nightstick", 600, 'm'},
            {1006, "weapon_bottle", "Broken bottle", 300, 'm'}, {1007, "weapon_dagger", "Antique dagger", 400, 'm'}, {1008, "weapon_hatchet", "Hatchet", 900, 'm'},
            {1009, "weapon_knuckle", "Knuckle duster", 200, 'm'}, {1010, "weapon_machete", "Machete", 800, 'm'}, {1011, "weapon_flashlight", "Heavy flashlight", 600, 'm'},
            {1012, "weapon_switchblade", "Switchblade", 200, 'm'}, {1013, "weapon_poolcue", "Pool cue", 500, 'm'}, {1014, "weapon_wrench", "Pipe wrench", 1500, 'm'},
            {1015, "weapon_battleaxe", "Battle axe", 1800, 'm'}, {1016, "weapon_stone_hatchet", "Stone hatchet", 1000, 'm'},
            {1020, "weapon_pistol", "Pistol", 900, 'h'}, {1021, "weapon_pistol_mk2", "Pistol Mk II", 950, 'h'}, {1022, "weapon_combatpistol", "Combat pistol", 900, 'h'},
            {1023, "weapon_appistol", "AP pistol", 1000, 'h'}, {1024, "weapon_stungun", "Stun gun", 600, 'h'}, {1025, "weapon_pistol50", "Pistol .50", 1300, 'h'},
            {1026, "weapon_snspistol", "SNS pistol", 600, 'h'}, {1027, "weapon_snspistol_mk2", "SNS pistol Mk II", 650, 'h'}, {1028, "weapon_heavypistol", "Heavy pistol", 1100, 'h'},
            {1029, "weapon_vintagepistol", "Vintage pistol", 800, 'h'}, {1030, "weapon_flaregun", "Flare gun", 700, 'h'}, {1031, "weapon_marksmanpistol", "Marksman pistol", 1000, 'h'},
            {1032, "weapon_revolver", "Heavy revolver", 1300, 'h'}, {1033, "weapon_revolver_mk2", "Heavy revolver Mk II", 1350, 'h'}, {1034, "weapon_doubleaction", "Double-action revolver", 1100, 'h'},
            {1035, "weapon_ceramicpistol", "Ceramic pistol", 700, 'h'}, {1036, "weapon_navyrevolver", "Navy revolver", 1200, 'h'}, {1037, "weapon_gadgetpistol", "Perico pistol", 800, 'h'},
            {1038, "weapon_raypistol", "Up-n-Atomizer", 1200, 'h'}, {1039, "weapon_pistolxm3", "WM 29 pistol", 900, 'h'},
            {1050, "weapon_microsmg", "Micro SMG", 1800, 's'}, {1051, "weapon_smg", "SMG", 2600, 's'}, {1052, "weapon_smg_mk2", "SMG Mk II", 2700, 's'},
            {1053, "weapon_assaultsmg", "Assault SMG", 2800, 's'}, {1054, "weapon_combatpdw", "Combat PDW", 2700, 's'}, {1055, "weapon_machinepistol", "Machine pistol", 1500, 's'},
            {1056, "weapon_minismg", "Mini SMG", 1700, 's'}, {1057, "weapon_raycarbine", "Unholy Hellbringer", 3500, 's'}, {1058, "weapon_tecpistol", "Tactical SMG", 1600, 's'},
            {1070, "weapon_pumpshotgun", "Pump shotgun", 3300, 'g'}, {1071, "weapon_pumpshotgun_mk2", "Pump shotgun Mk II", 3400, 'g'}, {1072, "weapon_sawnoffshotgun", "Sawed-off shotgun", 2500, 'g'},
            {1073, "weapon_assaultshotgun", "Assault shotgun", 4200, 'g'}, {1074, "weapon_bullpupshotgun", "Bullpup shotgun", 3200, 'g'}, {1075, "weapon_musket", "Musket", 4500, 'g'},
            {1076, "weapon_heavyshotgun", "Heavy shotgun", 4000, 'g'}, {1077, "weapon_dbshotgun", "Double barrel shotgun", 2800, 'g'}, {1078, "weapon_autoshotgun", "Sweeper shotgun", 3600, 'g'},
            {1079, "weapon_combatshotgun", "Combat shotgun", 3800, 'g'},
            {1090, "weapon_assaultrifle", "Assault rifle", 3500, 'r'}, {1091, "weapon_assaultrifle_mk2", "Assault rifle Mk II", 3600, 'r'}, {1092, "weapon_carbinerifle", "Carbine rifle", 3300, 'r'},
            {1093, "weapon_carbinerifle_mk2", "Carbine rifle Mk II", 3400, 'r'}, {1094, "weapon_advancedrifle", "Advanced rifle", 3200, 'r'}, {1095, "weapon_specialcarbine", "Special carbine", 3300, 'r'},
            {1096, "weapon_specialcarbine_mk2", "Special carbine Mk II", 3400, 'r'}, {1097, "weapon_bullpuprifle", "Bullpup rifle", 3300, 'r'}, {1098, "weapon_bullpuprifle_mk2", "Bullpup rifle Mk II", 3400, 'r'},
            {1099, "weapon_compactrifle", "Compact rifle", 2800, 'r'}, {1100, "weapon_militaryrifle", "Military rifle", 3600, 'r'}, {1101, "weapon_heavyrifle", "Heavy rifle", 3900, 'r'},
            {1102, "weapon_tacticalrifle", "Service carbine", 3300, 'r'},
            {1110, "weapon_mg", "MG", 8000, 'l'}, {1111, "weapon_combatmg", "Combat MG", 8500, 'l'}, {1112, "weapon_combatmg_mk2", "Combat MG Mk II", 8800, 'l'},
            {1113, "weapon_gusenberg", "Gusenberg sweeper", 4900, 'l'},
            {1120, "weapon_sniperrifle", "Sniper rifle", 6000, 'x'}, {1121, "weapon_heavysniper", "Heavy sniper", 9000, 'x'}, {1122, "weapon_heavysniper_mk2", "Heavy sniper Mk II", 9300, 'x'},
            {1123, "weapon_marksmanrifle", "Marksman rifle", 5000, 'x'}, {1124, "weapon_marksmanrifle_mk2", "Marksman rifle Mk II", 5200, 'x'}, {1125, "weapon_precisionrifle", "Precision rifle", 5500, 'x'},
            {1130, "weapon_rpg", "RPG", 7000, 'v'}, {1131, "weapon_grenadelauncher", "Grenade launcher", 6500, 'v'}, {1132, "weapon_minigun", "Minigun", 14000, 'v'},
            {1133, "weapon_firework", "Firework launcher", 5000, 'v'}, {1134, "weapon_railgun", "Railgun", 9000, 'v'}, {1135, "weapon_hominglauncher", "Homing launcher", 8000, 'v'},
            {1136, "weapon_compactlauncher", "Compact launcher", 3500, 'v'}, {1137, "weapon_rayminigun", "Widowmaker", 12000, 'v'}, {1138, "weapon_emplauncher", "EMP launcher", 4000, 'v'},
            {1150, "weapon_grenade", "Grenade", 400, 't'}, {1151, "weapon_bzgas", "BZ gas", 400, 't'}, {1152, "weapon_molotov", "Molotov", 700, 't'},
            {1153, "weapon_stickybomb", "Sticky bomb", 500, 't'}, {1154, "weapon_proxmine", "Proximity mine", 600, 't'}, {1155, "weapon_snowball", "Snowball", 100, 't'},
            {1156, "weapon_pipebomb", "Pipe bomb", 500, 't'}, {1157, "weapon_ball", "Ball", 100, 't'}, {1158, "weapon_smokegrenade", "Tear gas", 400, 't'},
            {1159, "weapon_flare", "Flare", 200, 't'},
            {1170, "weapon_petrolcan", "Jerry can", 4000, 'v'}, {1171, "weapon_fireextinguisher", "Fire extinguisher", 3000, 'v'}, {1172, "weapon_hazardcan", "Hazardous jerry can", 4000, 'v'},
        };
        static const std::vector<ItemDef> k = [] {
            std::vector<ItemDef> v;
            for (const W& w : kW) {
                const char* glyph = w.kind == 'm' ? "blade" : (w.kind == 't' ? "grenade" : "gun");
                const char* desc = w.kind == 'm' ? "A melee weapon." : w.kind == 'h' ? "A handgun." : w.kind == 's' ? "A submachine gun."
                                 : w.kind == 'g' ? "A shotgun." : w.kind == 'r' ? "A rifle." : w.kind == 'l' ? "A machine gun."
                                 : w.kind == 'x' ? "A sniper rifle." : w.kind == 't' ? "Thrown. The count is how many you carry." : "A heavy weapon.";
                ItemDef d{w.id, w.key, w.name, glyph, w.grams, 1, Category::Weapons, false, Equip::None, 0, desc};
                d.weapon = Joaat(w.key);
                v.push_back(d);
            }
            return v;
        }();
        return k;
    }
    // A thrown weapon (its ammo is the count; GTA removes the weapon from the ped when the last one is thrown).
    inline bool IsThrown(const ItemDef& d) { return d.weapon != 0 && std::string(d.glyph) == "grenade"; }

    inline const ItemDef* Find(ItemId id)
    {
        if (id == 0)
            return nullptr;
        for (const ItemDef& d : Catalog())
            if (d.id == id)
                return &d;
        if (id >= kFirstWeaponId)
            for (const ItemDef& d : WeaponCatalog())
                if (d.id == id)
                    return &d;
        return nullptr;
    }
    inline const ItemDef* FindKey(const std::string& key)
    {
        for (const ItemDef& d : Catalog())
            if (key == d.key)
                return &d;
        for (const ItemDef& d : WeaponCatalog())
            if (key == d.key)
                return &d;
        return nullptr;
    }
    inline const ItemDef* FindWeapon(uint32_t hash)
    {
        if (hash == 0)
            return nullptr;
        for (const ItemDef& d : WeaponCatalog())
            if (d.weapon == hash)
                return &d;
        return nullptr;
    }
    inline bool IsWeapon(ItemId id) { const ItemDef* d = Find(id); return d && d->weapon != 0; }
    inline const char* CategoryName(Category c)
    {
        switch (c) {
        case Category::Tools: return "TOOLS";
        case Category::Food: return "FOOD";
        case Category::Medical: return "MEDICAL";
        case Category::Docs: return "DOCS";
        case Category::Clothing: return "CLOTHING";
        case Category::Weapons: return "WEAPONS";
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
        uint16_t ammo = 0;   // 0.5.4: a weapon item's rounds (codec v2; 0 for everything else)
        bool Empty() const { return item == 0 || qty == 0; }
        void Clear() { item = 0; qty = 0; cond = kNoCondition; ammo = 0; }
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
            b.ammo = a.ammo;   // weapons never split (stackMax 1): the rounds go with the item
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
    inline uint32_t Add(Container& c, ItemId item, uint32_t n, uint8_t cond = kNoCondition, uint16_t ammo = 0)
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
            s.ammo = d->weapon ? ammo : 0;
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
        Slot hand;                // 0.5.4: the weapon in hand (the ped's current weapon); weighs nothing against the caps

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
    // 0.5.4: Hand (index 0) and Nearby (a world container the server named in InvNearby: Op::srcBox / dstBox say which). An old (0.6.0) server
    // refuses both (its DecodeOp stops at Quick), so the client sends them only to a server that advertised the inventory world.
    enum class Where : uint8_t { Pockets, Pack, Equip, Quick, Hand, Nearby };
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
        uint32_t srcBox = 0, dstBox = 0;   // 0.5.4: the world container of a Nearby side (0 otherwise)
    };
    struct ApplyOut
    {
        Result r = Result::BadSlot;
        ItemId used = 0;   // Result::Used: the item consumed (the server applies its UseEffect)
        Slot dropped;      // 0.5.4, Result::Dropped: what left the character (the server puts it on the ground)
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
        if (w == Where::Hand)
            return i == 0 ? &ch.hand : nullptr;
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

    // ---- 0.5.4: the hand ----------------------------------------------------------------------------------------------------------------
    // Put the weapon in c[si] into the hand. A weapon already in hand goes back into c[si] (a swap): it must be allowed there and fit by
    // weight once the new one has left.
    inline Result HandFrom(Character& ch, Container& c, int si)
    {
        if (si < 0 || si >= (int)c.slots.size() || c.slots[(size_t)si].Empty())
            return Result::BadSlot;
        const ItemDef* d = Find(c.slots[(size_t)si].item);
        if (!d || d->weapon == 0)
            return Result::WrongSlot;
        if (!ch.hand.Empty()) {
            if (!Accepts(c, ch.hand.item))
                return Result::WrongSlot;
            if (FitByWeight(c, ch.hand.item, 1, d->unitGrams) == 0)
                return Result::Weight;
            std::swap(ch.hand, c.slots[(size_t)si]);
            return Result::Swapped;
        }
        ch.hand = c.slots[(size_t)si];
        c.slots[(size_t)si].Clear();
        return Result::Moved;
    }
    // The weapon in hand into c[di]: an empty slot (within the weight cap), or a slot holding another weapon (a swap: that one is taken).
    inline Result HandTo(Character& ch, Container& c, int di)
    {
        if (ch.hand.Empty() || di < 0 || di >= (int)c.slots.size())
            return Result::BadSlot;
        if (!c.slots[(size_t)di].Empty())
            return IsWeapon(c.slots[(size_t)di].item) ? HandFrom(ch, c, di) : Result::Occupied;
        if (!Accepts(c, ch.hand.item))
            return Result::WrongSlot;
        if (FitByWeight(c, ch.hand.item, 1) == 0)
            return Result::Weight;
        c.slots[(size_t)di] = ch.hand;
        ch.hand.Clear();
        return Result::Moved;
    }
    // Holster: the weapon in hand into the first empty slot that takes it (pockets, then the pack).
    inline Result HolsterAnywhere(Character& ch)
    {
        if (ch.hand.Empty())
            return Result::BadSlot;
        Result last = Result::NoRoom;
        Container* order[2] = {&ch.pockets, ch.HasPack() ? &ch.pack : nullptr};
        for (Container* c : order) {
            if (!c)
                continue;
            for (size_t i = 0; i < c->slots.size(); ++i)
                if (c->slots[i].Empty()) {
                    const Result r = HandTo(ch, *c, (int)i);
                    if (Ok(r))
                        return r;
                    last = r;   // too heavy here: try the pack
                    break;
                }
        }
        return last == Result::Weight ? last : Result::NoRoom;
    }

    // ApplyEx: Apply with the world containers of a Nearby side (0.5.4). srcNear / dstNear are the containers the op's srcBox / dstBox name
    // (the server resolves them after its reach and lock checks, the overlay from its InvNearby copy); null = that side has none (refused).
    inline ApplyOut ApplyEx(Character& ch, const Op& op, Container* srcNear, Container* dstNear)
    {
        ApplyOut out;
        const auto isBox = [](Where w) { return w == Where::Pockets || w == Where::Pack || w == Where::Nearby; };
        Container* const S = op.src == Where::Nearby ? srcNear : (isBox(op.src) ? BoxOf(ch, op.src) : nullptr);
        Container* const D = op.dst == Where::Nearby ? dstNear : (isBox(op.dst) ? BoxOf(ch, op.dst) : nullptr);
        const auto validSrc = [&] { return S && op.si >= 0 && op.si < (int)S->slots.size() && !S->slots[(size_t)op.si].Empty(); };
        switch (op.kind) {
        case OpKind::Move: {
            if (op.src == Where::Hand || op.dst == Where::Hand) {
                if (op.src == Where::Hand && op.dst == Where::Hand)
                    out.r = Result::Nothing;
                else if (op.dst == Where::Hand)
                    out.r = S ? HandFrom(ch, *S, op.si) : (isBox(op.src) ? Result::BadSlot : Result::WrongSlot);
                else
                    out.r = D ? HandTo(ch, *D, op.di) : (isBox(op.dst) ? Result::BadSlot : Result::WrongSlot);
                return out;
            }
            if (isBox(op.src) && isBox(op.dst)) {
                out.r = S && D ? Move(*S, op.si, *D, op.di, op.qty) : Result::BadSlot;
            } else if (isBox(op.src) && op.dst == Where::Equip) {
                out.r = S ? EquipFrom(ch, *S, op.si, EquipAt(op.di)) : Result::BadSlot;
            } else if (op.src == Where::Equip && isBox(op.dst)) {
                Container* b = D;
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
            out.r = S && D ? Split(*S, op.si, *D, op.di, op.qty) : Result::BadSlot;
            return out;
        }
        case OpKind::QuickMove: {
            if (op.src == Where::Equip) {
                out.r = TakeOffAnywhere(ch, op.si);
                return out;
            }
            if (op.src == Where::Hand) {
                out.r = HolsterAnywhere(ch);
                return out;
            }
            if (!validSrc()) {
                out.r = Result::BadSlot;
                return out;
            }
            const ItemId item = S->slots[(size_t)op.si].item;
            if (op.src == Where::Nearby && op.dst != Where::Nearby) {
                // off the ground / out of the trunk: pockets first, then the pack
                Result last = Result::NoRoom;
                Container* order[2] = {&ch.pockets, ch.HasPack() ? &ch.pack : nullptr};
                for (Container* b : order) {
                    if (!b)
                        continue;
                    const int di = QuickTarget(*b, item);
                    if (di < 0)
                        continue;
                    const Result r = Move(*S, op.si, *b, di, 0);
                    if (Ok(r)) {
                        out.r = r;
                        return out;
                    }
                    last = r;
                }
                out.r = last;
                return out;
            }
            Container* b = nullptr;
            if (op.dst == Where::Nearby)
                b = op.src == Where::Nearby ? nullptr : D;   // Shift+click while a world container is open: into it
            else
                b = op.src == Where::Pockets ? BoxOf(ch, Where::Pack) : (op.src == Where::Pack ? &ch.pockets : nullptr);
            if (!b) {
                out.r = Result::NoRoom;
                return out;
            }
            if (!Accepts(*b, item)) {
                out.r = Result::WrongSlot;
                return out;
            }
            const int di = QuickTarget(*b, item);
            out.r = di < 0 ? Result::NoRoom : Move(*S, op.si, *b, di, 0);
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
            return ApplyEx(ch, u, nullptr, nullptr);
        }
        case OpKind::Use: {
            if (op.src == Where::Equip) {
                out.r = TakeOffAnywhere(ch, op.si);
                return out;
            }
            if (op.src == Where::Hand) {
                out.r = HolsterAnywhere(ch);   // USE on the hand = put it away
                return out;
            }
            if (op.src == Where::Nearby || !validSrc()) {
                out.r = Result::BadSlot;   // USE works on what the player carries, not on the ground
                return out;
            }
            Slot& s = S->slots[(size_t)op.si];
            const ItemDef* d = Find(s.item);
            if (d && d->weapon != 0) {
                out.r = HandFrom(ch, *S, op.si);   // USE on a weapon = take it in hand
                return out;
            }
            if (d && d->equip != Equip::None) {
                out.r = EquipFrom(ch, *S, op.si, d->equip);
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
                    out.dropped = ch.equip[op.si];
                    ch.equip[op.si].Clear();
                    if (EquipAt(op.si) == Equip::Pack)
                        ch.pack = Container();
                    out.r = Result::Dropped;
                }
                return out;
            }
            if (op.src == Where::Hand) {
                if (ch.hand.Empty() || op.si != 0)
                    out.r = Result::BadSlot;
                else {
                    out.dropped = ch.hand;
                    ch.hand.Clear();
                    out.r = Result::Dropped;
                }
                return out;
            }
            if (op.src == Where::Nearby || !validSrc()) {
                out.r = op.src == Where::Nearby ? Result::WrongSlot : Result::BadSlot;
                return out;
            }
            Slot& s = S->slots[(size_t)op.si];
            out.dropped = s;
            if (op.qty == 0 || op.qty >= s.qty)
                s.Clear();
            else {
                out.dropped.qty = op.qty;
                s.qty -= op.qty;
            }
            out.r = Result::Dropped;
            return out;
        }
        case OpKind::Bind: {
            const Slot* s = op.src == Where::Pockets || op.src == Where::Pack ? SlotAt(ch, op.src, op.si) : nullptr;
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
    inline ApplyOut Apply(Character& ch, const Op& op) { return ApplyEx(ch, op, nullptr, nullptr); }

    // ---- wire and save form ---------------------------------------------------------------------------------------------------------------
    // A Character as bytes (the server's InvState / InvSnap messages and its save files): version, the 16 pocket slots, the 14 equipment
    // slots, the pack's slot count and slots, the 4 quick refs, and (v2) the hand. Little-endian. A slot is item u16, qty u32, cond u8, and in
    // v2 a weapon item's ammo u16 after it. Decode refuses anything the rules could not have produced (an unknown item, a quantity over its
    // stack, a worn item in the wrong slot, a pack whose size is not its tier's, a hand that is not one weapon), so a corrupt save or packet
    // never becomes an inventory.
    //
    // Versions: 1 = 0.6.0 (no weapons, no hand); 2 = 0.5.4's inventory world. Decode reads both. A 0.6.0 client decodes only v1, so the server
    // sends it EncodeLegacy (v1 with every weapon left out: those slots read as empty and quick refs to weapons as unbound).
    constexpr uint8_t kCodecVersion = 2;
    constexpr uint8_t kCodecVersionLegacy = 1;
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
        // ver 1 writes a weapon as an empty slot (a 0.6.0 reader does not know the item).
        inline void PutSlot(std::string& b, const Slot& s, uint8_t ver)
        {
            const ItemDef* d = s.Empty() ? nullptr : Find(s.item);
            const bool e = s.Empty() || (ver < 2 && d && d->weapon != 0);
            Put16(b, e ? 0 : s.item);
            Put32(b, e ? 0 : s.qty);
            Put8(b, e ? kNoCondition : s.cond);
            if (ver >= 2 && !e && d && d->weapon != 0)
                Put16(b, s.ammo);
        }
        inline bool GetSlot(Rd& r, Slot& s, uint8_t ver)
        {
            s.item = r.U16();
            s.qty = r.U32();
            s.cond = r.U8();
            s.ammo = 0;
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
            if (d->weapon != 0) {
                if (ver < 2)
                    return false;   // v1 never carried weapons
                s.ammo = r.U16();
                return r.ok;
            }
            return true;
        }
    }
    inline std::string EncodeAs(const Character& ch, uint8_t ver)
    {
        std::string b;
        codec::Put8(b, ver);
        for (const Slot& s : ch.pockets.slots)
            codec::PutSlot(b, s, ver);
        for (const Slot& s : ch.equip)
            codec::PutSlot(b, s, ver);
        codec::Put8(b, (uint8_t)(ch.HasPack() ? ch.pack.slots.size() : 0));
        if (ch.HasPack())
            for (const Slot& s : ch.pack.slots)
                codec::PutSlot(b, s, ver);
        for (const QuickRef& q : ch.quick) {
            const bool hide = ver < 2 && IsWeapon(q.item);
            codec::Put16(b, hide ? 0 : q.item);
            codec::Put8(b, (uint8_t)q.kind);
            codec::Put8(b, (uint8_t)(hide || q.index < 0 || q.index > 254 ? 255 : q.index));
        }
        if (ver >= 2)
            codec::PutSlot(b, ch.hand, ver);
        return b;
    }
    inline std::string Encode(const Character& ch) { return EncodeAs(ch, kCodecVersion); }
    inline std::string EncodeLegacy(const Character& ch) { return EncodeAs(ch, kCodecVersionLegacy); }
    // Any weapon item anywhere (the hand, pockets, pack, worn slots never hold one)?
    inline bool HasWeapons(const Character& ch)
    {
        if (!ch.hand.Empty())
            return true;
        for (const Slot& s : ch.pockets.slots)
            if (!s.Empty() && IsWeapon(s.item))
                return true;
        for (const Slot& s : ch.pack.slots)
            if (!s.Empty() && IsWeapon(s.item))
                return true;
        return false;
    }
    // The save form: v1 when the character holds no weapon (v1 is then lossless, and a 0.6.0 / 0.5.3 server rolled back to can
    // still read the save), v2 otherwise.
    inline std::string EncodeForSave(const Character& ch) { return HasWeapons(ch) ? Encode(ch) : EncodeLegacy(ch); }
    inline bool Decode(const std::string& bytes, Character& out)
    {
        codec::Rd r{bytes};
        const uint8_t ver = r.U8();
        if (ver != kCodecVersion && ver != kCodecVersionLegacy)
            return false;
        Character ch;
        if ((int)ch.pockets.slots.size() != kPocketSlots)
            return false;
        for (Slot& s : ch.pockets.slots)
            if (!codec::GetSlot(r, s, ver))
                return false;
        for (int i = 0; i < kEquipCount; ++i) {
            if (!codec::GetSlot(r, ch.equip[i], ver))
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
                if (!codec::GetSlot(r, s, ver))
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
        if (ver >= 2) {
            if (!codec::GetSlot(r, ch.hand, ver))
                return false;
            if (!ch.hand.Empty() && (!IsWeapon(ch.hand.item) || ch.hand.qty != 1))
                return false;
        }
        if (!r.ok || r.p != bytes.size())
            return false;
        out = ch;
        return true;
    }

    // A world container (0.5.4 InvNearby: a ground pile, a trunk) as bytes: version 2, kind, maxGrams u32, slot count u8 (1..64), the slots.
    constexpr int kMaxBoxSlots = 64;
    inline std::string EncodeBox(const Container& c)
    {
        std::string b;
        codec::Put8(b, kCodecVersion);
        codec::Put8(b, (uint8_t)c.kind);
        codec::Put32(b, c.maxGrams);
        const size_t n = c.slots.size() > (size_t)kMaxBoxSlots ? (size_t)kMaxBoxSlots : c.slots.size();
        codec::Put8(b, (uint8_t)n);
        for (size_t i = 0; i < n; ++i)
            codec::PutSlot(b, c.slots[i], kCodecVersion);
        return b;
    }
    inline bool DecodeBox(const std::string& bytes, Container& out)
    {
        codec::Rd r{bytes};
        if (r.U8() != kCodecVersion)
            return false;
        Container c;
        const uint8_t k = r.U8();
        if (k > (uint8_t)Kind::Ground || k == (uint8_t)Kind::Pockets || k == (uint8_t)Kind::Pack)
            return false;
        c.kind = (Kind)k;
        c.maxGrams = r.U32();
        const int n = r.U8();
        if (!r.ok || n < 1 || n > kMaxBoxSlots)
            return false;
        c.slots.resize((size_t)n);
        for (Slot& s : c.slots)
            if (!codec::GetSlot(r, s, kCodecVersion))
                return false;
        if (!r.ok || r.p != bytes.size())
            return false;
        out = c;
        return true;
    }

    // An Op as 11 bytes (kind, src, si, dst, di, qty), or (0.5.4) 19 bytes when a side is Nearby: the same 11, then srcBox u32, dstBox u32.
    constexpr size_t kOpBytes = 11, kOpBytesExt = 19;
    inline std::string EncodeOp(const Op& op)
    {
        std::string b;
        codec::Put8(b, (uint8_t)op.kind);
        codec::Put8(b, (uint8_t)op.src);
        codec::Put16(b, (uint16_t)op.si);
        codec::Put8(b, (uint8_t)op.dst);
        codec::Put16(b, (uint16_t)op.di);
        codec::Put32(b, op.qty);
        if (op.src == Where::Nearby || op.dst == Where::Nearby) {
            codec::Put32(b, op.srcBox);
            codec::Put32(b, op.dstBox);
        }
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
        const bool ext = bytes.size() == kOpBytesExt;
        const uint32_t sb = ext ? r.U32() : 0, db = ext ? r.U32() : 0;
        if (!r.ok || r.p != bytes.size() || k >= kOpKinds || s > (uint8_t)Where::Nearby || d > (uint8_t)Where::Nearby)
            return false;
        const bool hasWorld = s == (uint8_t)Where::Nearby || d == (uint8_t)Where::Nearby;
        if (hasWorld != ext)
            return false;   // the long form exactly when a side is a world container
        op.kind = (OpKind)k;
        op.src = (Where)s;
        op.si = (int16_t)si;
        op.dst = (Where)d;
        op.di = (int16_t)di;
        op.qty = q;
        op.srcBox = sb;
        op.dstBox = db;
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
    // A new character. 0.5.4 (GI: "remove dummy items from inventory"): the pockets start EMPTY; the phone, ID, food and bandages the 0.6.0 kit
    // put there were placeholders. What it wears is the starting outfit. No backpack: one is an item to find or buy.
    inline Character StarterKit()
    {
        Character ch;
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
