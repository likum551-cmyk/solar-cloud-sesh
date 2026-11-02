// GTA V name hashes the engine's decisions need, pure C++17 (no game or OS headers), tested by
// tests/gameplay_test.cpp.
//
//   Joaat      the game's GET_HASH_KEY (Jenkins one-at-a-time over the lower-cased bytes), constexpr.
//              Verified in the test against hashes printed by a source, not against itself:
//              ScriptHookV SDK enums.h (WeaponRPG 0xB1CA77B1, WeaponGrenade 0x93E220BD, ...), and
//              GameplayFixesV inc/globals.h:585-586 (WEAPONGROUP_HEAVY 0xA27A4F9F, WEAPONGROUP_THROWN
//              0x5C4C5883), and the -1171018317 of the ACTIVATE_FRONTEND_MENU doc example, which is
//              Joaat("FE_MENU_VERSION_MP_PAUSE").
//   Weapons    IsExplosiveOrThrownWeapon: the weapons a remote ped's shots must never be played with.
//              IsMeleeWeapon (fists / GROUP_MELEE / GROUP_UNARMED), MuzzleFlashEffect (per group),
//              WeaponLabel (kill-feed names of the common weapons).
//
// Weapon source: GameplayFixesV res/weapons.json (GPLv3; used as FACTS only, no code copied): every
// ped-holdable entry whose "DamageType" is EXPLOSIVE, FIRE or SMOKE, whose "Category" is GROUP_THROWN, or
// that is a projectile launcher whose damage type reads NONE (the smoke grenade launcher, the snowball
// launcher). The hash of each is Joaat(name). A bullet weapon of GROUP_HEAVY (the minigun, the ray
// minigun) is NOT in the list: its shots are bullets and are shown.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gage::sync {

    constexpr uint32_t Joaat(const char* s)
    {
        uint32_t h = 0;
        for (; *s; ++s)
        {
            uint32_t c = (uint8_t)*s;
            if (c >= 'A' && c <= 'Z')
                c = c - 'A' + 'a';
            h += c;
            h += h << 10;
            h ^= h >> 6;
        }
        h += h << 3;
        h ^= h >> 11;
        h += h << 15;
        return h;
    }

    // ACTIVATE_FRONTEND_MENU's menu hash for the singleplayer pause menu (GameplayFixesV
    // source/utils/player.cpp: ACTIVATE_FRONTEND_MENU(FE_MENU_VERSION_SP_PAUSE, false, -1)).
    constexpr uint32_t kFeMenuVersionSpPause = Joaat("FE_MENU_VERSION_SP_PAUSE");

    // GET_WEAPONTYPE_GROUP results (GameplayFixesV inc/globals.h:585-586).
    constexpr uint32_t kWeaponGroupHeavy = 0xA27A4F9F;   // GROUP_HEAVY
    constexpr uint32_t kWeaponGroupThrown = 0x5C4C5883;  // GROUP_THROWN
    // The other groups are Joaat of the group name; the test checks each against the decimal value NativeDB
    // lists for GET_WEAPONTYPE_GROUP (melee 3566412244, unarmed 2685387236, handguns 416676503, ...), which
    // also agree with the two constants above.
    constexpr uint32_t kWeaponGroupMelee = Joaat("GROUP_MELEE");
    constexpr uint32_t kWeaponGroupUnarmed = Joaat("GROUP_UNARMED");
    constexpr uint32_t kWeaponGroupPistol = Joaat("GROUP_PISTOL");
    constexpr uint32_t kWeaponGroupSmg = Joaat("GROUP_SMG");
    constexpr uint32_t kWeaponGroupShotgun = Joaat("GROUP_SHOTGUN");
    constexpr uint32_t kWeaponGroupRifle = Joaat("GROUP_RIFLE");
    constexpr uint32_t kWeaponGroupMg = Joaat("GROUP_MG");
    constexpr uint32_t kWeaponGroupSniper = Joaat("GROUP_SNIPER");
    constexpr uint32_t kWeaponUnarmed = Joaat("WEAPON_UNARMED");   // 0xA2719263

    constexpr uint32_t kExplosiveOrThrownWeapons[] = {
        // projectile launchers: DamageType EXPLOSIVE (GROUP_HEAVY), plus the two that read NONE
        Joaat("WEAPON_GRENADELAUNCHER"), Joaat("WEAPON_GRENADELAUNCHER_SMOKE"), Joaat("WEAPON_RPG"),
        Joaat("WEAPON_PASSENGER_ROCKET"), Joaat("WEAPON_AIRSTRIKE_ROCKET"), Joaat("WEAPON_STINGER"),
        Joaat("WEAPON_VEHICLE_ROCKET"), Joaat("WEAPON_RAILGUNXM3"), Joaat("WEAPON_HOMINGLAUNCHER"),
        Joaat("WEAPON_FIREWORK"), Joaat("WEAPON_RAILGUN"), Joaat("WEAPON_COMPACTLAUNCHER"),
        Joaat("WEAPON_EMPLAUNCHER"), Joaat("WEAPON_SNOWLAUNCHER"), Joaat("WEAPON_REMOTESNIPER"),
        // thrown and placed: GROUP_THROWN (EXPLOSIVE, FIRE, SMOKE or NONE)
        Joaat("WEAPON_GRENADE"), Joaat("WEAPON_STICKYBOMB"), Joaat("WEAPON_SMOKEGRENADE"), Joaat("WEAPON_BZGAS"),
        Joaat("WEAPON_MOLOTOV"), Joaat("WEAPON_BALL"), Joaat("WEAPON_FLARE"), Joaat("WEAPON_BIRD_CRAP"),
        Joaat("WEAPON_ACIDPACKAGE"), Joaat("WEAPON_SNOWBALL"), Joaat("WEAPON_PROXMINE"), Joaat("WEAPON_PIPEBOMB"),
    };
    constexpr size_t kExplosiveOrThrownCount = sizeof(kExplosiveOrThrownWeapons) / sizeof(kExplosiveOrThrownWeapons[0]);

    // True for a weapon that must not be played as a bullet from a remote ped's muzzle: an explosive or
    // thrown one would fire a REAL rocket or throw a real grenade on this machine. `group` is
    // GET_WEAPONTYPE_GROUP(weapon), 0 when it could not be read (the list still applies); any weapon in
    // GROUP_THROWN is excluded even when a newer DLC one is not in the list.
    inline bool IsExplosiveOrThrownWeapon(uint32_t weapon, uint32_t group)
    {
        if (group == kWeaponGroupThrown)
            return true;
        for (size_t i = 0; i < kExplosiveOrThrownCount; ++i)
            if (kExplosiveOrThrownWeapons[i] == weapon)
                return true;
        return false;
    }

    // A melee-class weapon: the fists, or anything in GROUP_MELEE / GROUP_UNARMED. `group` is
    // GET_WEAPONTYPE_GROUP(weapon), 0 when it could not be read (the fists are still recognised by hash).
    inline bool IsMeleeWeapon(uint32_t weapon, uint32_t group)
    {
        if (!weapon)
            return false;
        return weapon == kWeaponUnarmed || group == kWeaponGroupMelee || group == kWeaponGroupUnarmed;
    }

    // The muzzle-flash effect of the "core" particle asset for a weapon group (INFERRED names, to be
    // confirmed live: the engine logs once per name whether the game started it). An unknown group takes
    // the rifle one.
    inline const char* MuzzleFlashEffect(uint32_t group)
    {
        if (group == kWeaponGroupPistol) return "muz_pistol";
        if (group == kWeaponGroupSmg) return "muz_smg";
        if (group == kWeaponGroupShotgun) return "muz_shotgun";
        if (group == kWeaponGroupMg) return "muz_machinegun";
        if (group == kWeaponGroupSniper) return "muz_sniper";
        if (group == kWeaponGroupHeavy) return "muz_minigun";
        return "muz_assault_rifle";
    }

    // ---- weapon names for the kill feed ----------------------------------------------------------------
    //
    // The common GTA V weapons and damage sources by hash, with the name a player knows. The names are
    // public game data; each hash is Joaat of the WEAPON_ name. A weapon not listed has no label (the feed
    // line then leaves "with ..." out).
    struct WeaponLabelEntry
    {
        uint32_t hash;
        const char* label;
    };
#define GAGE_WL(NAME, LABEL) { Joaat("WEAPON_" NAME), LABEL }
    constexpr WeaponLabelEntry kWeaponLabels[] = {
        // melee
        GAGE_WL("UNARMED", "Fists"), GAGE_WL("KNIFE", "Knife"), GAGE_WL("NIGHTSTICK", "Nightstick"),
        GAGE_WL("HAMMER", "Hammer"), GAGE_WL("BAT", "Baseball Bat"), GAGE_WL("CROWBAR", "Crowbar"),
        GAGE_WL("GOLFCLUB", "Golf Club"), GAGE_WL("BOTTLE", "Broken Bottle"), GAGE_WL("DAGGER", "Antique Cavalry Dagger"),
        GAGE_WL("HATCHET", "Hatchet"), GAGE_WL("KNUCKLE", "Brass Knuckles"), GAGE_WL("MACHETE", "Machete"),
        GAGE_WL("FLASHLIGHT", "Flashlight"), GAGE_WL("SWITCHBLADE", "Switchblade"), GAGE_WL("POOLCUE", "Pool Cue"),
        GAGE_WL("WRENCH", "Pipe Wrench"), GAGE_WL("BATTLEAXE", "Battle Axe"), GAGE_WL("STONE_HATCHET", "Stone Hatchet"),
        // handguns
        GAGE_WL("PISTOL", "Pistol"), GAGE_WL("PISTOL_MK2", "Pistol Mk II"), GAGE_WL("COMBATPISTOL", "Combat Pistol"),
        GAGE_WL("APPISTOL", "AP Pistol"), GAGE_WL("PISTOL50", "Pistol .50"), GAGE_WL("SNSPISTOL", "SNS Pistol"),
        GAGE_WL("SNSPISTOL_MK2", "SNS Pistol Mk II"), GAGE_WL("HEAVYPISTOL", "Heavy Pistol"),
        GAGE_WL("VINTAGEPISTOL", "Vintage Pistol"), GAGE_WL("FLAREGUN", "Flare Gun"),
        GAGE_WL("MARKSMANPISTOL", "Marksman Pistol"), GAGE_WL("REVOLVER", "Heavy Revolver"),
        GAGE_WL("REVOLVER_MK2", "Heavy Revolver Mk II"), GAGE_WL("DOUBLEACTION", "Double Action Revolver"),
        GAGE_WL("CERAMICPISTOL", "Ceramic Pistol"), GAGE_WL("NAVYREVOLVER", "Navy Revolver"),
        GAGE_WL("GADGETPISTOL", "Perico Pistol"), GAGE_WL("STUNGUN", "Stun Gun"),
        // submachine guns and machine guns
        GAGE_WL("MICROSMG", "Micro SMG"), GAGE_WL("SMG", "SMG"), GAGE_WL("SMG_MK2", "SMG Mk II"),
        GAGE_WL("ASSAULTSMG", "Assault SMG"), GAGE_WL("COMBATPDW", "Combat PDW"), GAGE_WL("MACHINEPISTOL", "Machine Pistol"),
        GAGE_WL("MINISMG", "Mini SMG"), GAGE_WL("MG", "MG"), GAGE_WL("COMBATMG", "Combat MG"),
        GAGE_WL("COMBATMG_MK2", "Combat MG Mk II"), GAGE_WL("GUSENBERG", "Gusenberg Sweeper"),
        // rifles
        GAGE_WL("ASSAULTRIFLE", "Assault Rifle"), GAGE_WL("ASSAULTRIFLE_MK2", "Assault Rifle Mk II"),
        GAGE_WL("CARBINERIFLE", "Carbine Rifle"), GAGE_WL("CARBINERIFLE_MK2", "Carbine Rifle Mk II"),
        GAGE_WL("ADVANCEDRIFLE", "Advanced Rifle"), GAGE_WL("SPECIALCARBINE", "Special Carbine"),
        GAGE_WL("SPECIALCARBINE_MK2", "Special Carbine Mk II"), GAGE_WL("BULLPUPRIFLE", "Bullpup Rifle"),
        GAGE_WL("BULLPUPRIFLE_MK2", "Bullpup Rifle Mk II"), GAGE_WL("COMPACTRIFLE", "Compact Rifle"),
        GAGE_WL("MILITARYRIFLE", "Military Rifle"), GAGE_WL("HEAVYRIFLE", "Heavy Rifle"),
        // shotguns
        GAGE_WL("PUMPSHOTGUN", "Pump Shotgun"), GAGE_WL("PUMPSHOTGUN_MK2", "Pump Shotgun Mk II"),
        GAGE_WL("SAWNOFFSHOTGUN", "Sawed-Off Shotgun"), GAGE_WL("ASSAULTSHOTGUN", "Assault Shotgun"),
        GAGE_WL("BULLPUPSHOTGUN", "Bullpup Shotgun"), GAGE_WL("MUSKET", "Musket"), GAGE_WL("HEAVYSHOTGUN", "Heavy Shotgun"),
        GAGE_WL("DBSHOTGUN", "Double Barrel Shotgun"), GAGE_WL("AUTOSHOTGUN", "Sweeper Shotgun"),
        GAGE_WL("COMBATSHOTGUN", "Combat Shotgun"),
        // snipers
        GAGE_WL("SNIPERRIFLE", "Sniper Rifle"), GAGE_WL("HEAVYSNIPER", "Heavy Sniper"),
        GAGE_WL("HEAVYSNIPER_MK2", "Heavy Sniper Mk II"), GAGE_WL("MARKSMANRIFLE", "Marksman Rifle"),
        GAGE_WL("MARKSMANRIFLE_MK2", "Marksman Rifle Mk II"), GAGE_WL("PRECISIONRIFLE", "Precision Rifle"),
        // heavy
        GAGE_WL("RPG", "RPG"), GAGE_WL("GRENADELAUNCHER", "Grenade Launcher"), GAGE_WL("MINIGUN", "Minigun"),
        GAGE_WL("FIREWORK", "Firework Launcher"), GAGE_WL("RAILGUN", "Railgun"), GAGE_WL("HOMINGLAUNCHER", "Homing Launcher"),
        GAGE_WL("COMPACTLAUNCHER", "Compact Grenade Launcher"),
        // thrown and placed
        GAGE_WL("GRENADE", "Grenade"), GAGE_WL("STICKYBOMB", "Sticky Bomb"), GAGE_WL("PROXMINE", "Proximity Mine"),
        GAGE_WL("PIPEBOMB", "Pipe Bomb"), GAGE_WL("SMOKEGRENADE", "Tear Gas"), GAGE_WL("BZGAS", "BZ Gas"),
        GAGE_WL("MOLOTOV", "Molotov"), GAGE_WL("FLARE", "Flare"), GAGE_WL("PETROLCAN", "Jerry Can"),
        GAGE_WL("FIREEXTINGUISHER", "Fire Extinguisher"), GAGE_WL("BALL", "Ball"), GAGE_WL("SNOWBALL", "Snowball"),
        // damage sources that are not a carried weapon
        GAGE_WL("RUN_OVER_BY_CAR", "Vehicle"), GAGE_WL("RAMMED_BY_CAR", "Vehicle"), GAGE_WL("FALL", "Fall"),
        GAGE_WL("EXPLOSION", "Explosion"), GAGE_WL("FIRE", "Fire"), GAGE_WL("DROWNING", "Drowning"),
        GAGE_WL("BLEEDING", "Bleeding"), GAGE_WL("ELECTRIC_FENCE", "Electric Fence"),
    };
#undef GAGE_WL
    constexpr size_t kWeaponLabelCount = sizeof(kWeaponLabels) / sizeof(kWeaponLabels[0]);

    // The label of a weapon hash ("Carbine Rifle"), or an empty view when it is unknown (or 0).
    inline std::string_view WeaponLabel(uint32_t weapon)
    {
        if (!weapon)
            return std::string_view();
        for (size_t i = 0; i < kWeaponLabelCount; ++i)
            if (kWeaponLabels[i].hash == weapon)
                return kWeaponLabels[i].label;
        return std::string_view();
    }

}
