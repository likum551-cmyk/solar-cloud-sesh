// The server's authority rules against modded clients (protocol v4). Pure logic over the data it is fed
// (no sockets, no clocks of its own), header-only so the server core needs no new translation unit.
// docs/api/server.md "Anti-cheat rules" is the user-facing description; this is the code behind it.
//
//  * Joaat           GTA's GET_HASH_KEY (Jenkins one-at-a-time over the lower-cased bytes), constexpr, so
//                    weapon and model NAMES can be written in source and in server.json.
//  * Weapon rules    a table of GTA weapon hashes -> a class (melee, handgun, ... explosive, run-over,
//                    environment) and, per class, the largest damage ONE Hit may carry, the largest sustained
//                    damage per second, and how many Hits one shot may be credited with. THE NUMBERS ARE
//                    ESTIMATES (see WeaponRule): the shipped game files were not available to measure them.
//                    Operators scale them with Config::hitDamageScalePercent.
//  * ShotLog         the shooter's recent shots, rebuilt from the changes of PedState.shotSeq the server
//                    receives (a ring of 16, one slot per 4-bit value), so a Hit can be tied to a real shot.
//  * WeaponHistory   the last few weapons a player reported, for the weapons that have no shot (melee) or whose
//                    damage arrives long after the shot (grenades, rockets).
//  * Movement        the displacement budget between two accepted positions (on foot / in a vehicle).
//  * ConnectLimiter  a per-address connect rate (token bucket per source, bounded table).
//  * SanitizeAnim    the TASK_PLAY_ANIM flag allow-list and the playback-rate range a relayed animation gets.
//
// Thread-safety: none (the server's Poll thread). Errors: none throw.
#pragma once

#include "protocol/protocol.h"
#include "ratelimit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace gage::server::ac {

    // ---- GTA's joaat ---------------------------------------------------------------------------------

    // GET_HASH_KEY: ASCII letters are lower-cased, then Jenkins' one-at-a-time. constexpr, so a name in
    // source is a hash at compile time. Stops at the first NUL.
    constexpr uint32_t Joaat(const char* s)
    {
        uint32_t h = 0;
        for (; *s; ++s)
        {
            const char c = (*s >= 'A' && *s <= 'Z') ? (char)(*s - 'A' + 'a') : *s;
            h += (uint32_t)(unsigned char)c;
            h += h << 10;
            h ^= h >> 6;
        }
        h += h << 3;
        h ^= h >> 11;
        h += h << 15;
        return h;
    }
    inline uint32_t Joaat(const std::string& s) { return Joaat(s.c_str()); }

    // Known values (the weapon and model hashes the client and the tests already use) prove the function.
    static_assert(Joaat("WEAPON_PISTOL") == 0x1B06D571u, "joaat");
    static_assert(Joaat("WEAPON_UNARMED") == 0xA2719263u, "joaat");
    static_assert(Joaat("WEAPON_RUN_OVER_BY_CAR") == 0xA36D413Eu, "joaat (the engine's run-over cause)");
    static_assert(Joaat("mp_m_freemode_01") == 0x705E61F2u, "joaat");
    static_assert(Joaat("ADDER") == 0xB779A091u, "joaat is case-insensitive");

    // ---- weapon rules --------------------------------------------------------------------------------

    enum class WeaponKind : uint8_t
    {
        Unknown,     // not in the table: treated like a generic firearm (a shot is required)
        Melee,       // fists, blades, clubs: no shot; the target must be within kMeleeRange
        Handgun,
        Smg,
        Rifle,
        MachineGun,
        Shotgun,
        Sniper,
        Explosive,   // launchers, grenades, mines, molotovs, ...: the damage arrives long after the shot
        RunOver,     // WEAPON_RUN_OVER_BY_CAR / WEAPON_RAMMED_BY_CAR: the shooter must drive a moving vehicle nearby
        Environment, // fall, fire, explosion, drowning, ...: only ever valid as a hit on oneself
    };

    // What one class may do. All damage numbers are "GTA health units" as the engine reports them (the
    // health a remote ped lost from its local pool, headshot multipliers included, several shots merged by the
    // engine's 50 ms HitBatcher window and split at kMaxHitDamage). They are ESTIMATES: the stock body damage
    // of each class (pistols ~26-50 with the revolver at ~160, SMGs ~20-25, assault rifles ~30-40, MGs ~40-45,
    // a shotgun blast 8 pellets of ~30, snipers 100-216 per shot) was recalled from memory of weapons.meta and
    // multiplied by headroom for a headshot and for 2-4 merged shots; none of it was measured in-game. The
    // sustained rate allows roughly twice the class's real fire rate times its damage. Err towards too generous:
    // a refused legitimate hit costs a player a kill, a loose cap only costs a cheater a little less.
    struct WeaponRule
    {
        WeaponKind kind = WeaponKind::Unknown;
        const char* label = "unknown";
        uint16_t maxHit = 100;     // largest damage one Hit may carry (more is clamped to it, not refused)
        uint16_t maxPerSec = 400;  // sustained damage per second (a token bucket in seconds of this rate)
        uint8_t hitsPerShot = 2;   // Hits one shot may be credited with (pellets, targets of one blast, 250-point splits)
    };

    constexpr WeaponRule RuleOf(WeaponKind k)
    {
        switch (k)
        {
        case WeaponKind::Melee:       return {k, "melee",        100,  300, 2}; // v5: a swing pays for 2 Hits (a combo, two targets)
        case WeaponKind::Handgun:     return {k, "handgun",      160,  500, 2};
        case WeaponKind::Smg:         return {k, "smg",           80,  800, 2};
        case WeaponKind::Rifle:       return {k, "rifle",        120,  900, 2};
        case WeaponKind::MachineGun:  return {k, "machine gun",  160, 1500, 3};
        case WeaponKind::Shotgun:     return {k, "shotgun",      250,  700, 8};
        case WeaponKind::Sniper:      return {k, "sniper",       250,  700, 4};
        case WeaponKind::Explosive:   return {k, "explosive",    250, 1000, 16};
        case WeaponKind::RunOver:     return {k, "run-over",     250,  600, 0};
        case WeaponKind::Environment: return {k, "environment",  250,  600, 0};
        case WeaponKind::Unknown:     break;
        }
        return {WeaponKind::Unknown, "unknown weapon", 100, 400, 2};
    }

    struct WeaponEntry
    {
        uint32_t hash;
        WeaponKind kind;
    };

#define GAGE_W(name, k) {Joaat(name), WeaponKind::k}
    // The stock GTA V weapons (base game and the common DLC) by name; anything else is WeaponKind::Unknown.
    // Names are from memory of the game's weapon list; a misspelled one is merely Unknown (a generic firearm).
    inline constexpr WeaponEntry kWeapons[] = {
        GAGE_W("WEAPON_UNARMED", Melee), GAGE_W("WEAPON_KNIFE", Melee), GAGE_W("WEAPON_NIGHTSTICK", Melee),
        GAGE_W("WEAPON_HAMMER", Melee), GAGE_W("WEAPON_BAT", Melee), GAGE_W("WEAPON_GOLFCLUB", Melee),
        GAGE_W("WEAPON_CROWBAR", Melee), GAGE_W("WEAPON_BOTTLE", Melee), GAGE_W("WEAPON_DAGGER", Melee),
        GAGE_W("WEAPON_HATCHET", Melee), GAGE_W("WEAPON_KNUCKLE", Melee), GAGE_W("WEAPON_MACHETE", Melee),
        GAGE_W("WEAPON_FLASHLIGHT", Melee), GAGE_W("WEAPON_SWITCHBLADE", Melee), GAGE_W("WEAPON_POOLCUE", Melee),
        GAGE_W("WEAPON_WRENCH", Melee), GAGE_W("WEAPON_BATTLEAXE", Melee), GAGE_W("WEAPON_STONE_HATCHET", Melee),
        GAGE_W("WEAPON_CANDYCANE", Melee),

        GAGE_W("WEAPON_PISTOL", Handgun), GAGE_W("WEAPON_PISTOL_MK2", Handgun), GAGE_W("WEAPON_COMBATPISTOL", Handgun),
        GAGE_W("WEAPON_APPISTOL", Handgun), GAGE_W("WEAPON_PISTOL50", Handgun), GAGE_W("WEAPON_SNSPISTOL", Handgun),
        GAGE_W("WEAPON_SNSPISTOL_MK2", Handgun), GAGE_W("WEAPON_HEAVYPISTOL", Handgun),
        GAGE_W("WEAPON_VINTAGEPISTOL", Handgun), GAGE_W("WEAPON_MARKSMANPISTOL", Handgun),
        GAGE_W("WEAPON_REVOLVER", Handgun), GAGE_W("WEAPON_REVOLVER_MK2", Handgun), GAGE_W("WEAPON_DOUBLEACTION", Handgun),
        GAGE_W("WEAPON_CERAMICPISTOL", Handgun), GAGE_W("WEAPON_NAVYREVOLVER", Handgun),
        GAGE_W("WEAPON_GADGETPISTOL", Handgun), GAGE_W("WEAPON_STUNGUN", Handgun), GAGE_W("WEAPON_STUNGUN_MP", Handgun),
        GAGE_W("WEAPON_FLAREGUN", Handgun), GAGE_W("WEAPON_RAYPISTOL", Handgun),

        GAGE_W("WEAPON_MICROSMG", Smg), GAGE_W("WEAPON_SMG", Smg), GAGE_W("WEAPON_SMG_MK2", Smg),
        GAGE_W("WEAPON_ASSAULTSMG", Smg), GAGE_W("WEAPON_COMBATPDW", Smg), GAGE_W("WEAPON_MACHINEPISTOL", Smg),
        GAGE_W("WEAPON_MINISMG", Smg), GAGE_W("WEAPON_RAYCARBINE", Smg),

        GAGE_W("WEAPON_ASSAULTRIFLE", Rifle), GAGE_W("WEAPON_ASSAULTRIFLE_MK2", Rifle),
        GAGE_W("WEAPON_CARBINERIFLE", Rifle), GAGE_W("WEAPON_CARBINERIFLE_MK2", Rifle),
        GAGE_W("WEAPON_ADVANCEDRIFLE", Rifle), GAGE_W("WEAPON_SPECIALCARBINE", Rifle),
        GAGE_W("WEAPON_SPECIALCARBINE_MK2", Rifle), GAGE_W("WEAPON_BULLPUPRIFLE", Rifle),
        GAGE_W("WEAPON_BULLPUPRIFLE_MK2", Rifle), GAGE_W("WEAPON_COMPACTRIFLE", Rifle),
        GAGE_W("WEAPON_MILITARYRIFLE", Rifle), GAGE_W("WEAPON_HEAVYRIFLE", Rifle), GAGE_W("WEAPON_TACTICALRIFLE", Rifle),

        GAGE_W("WEAPON_MG", MachineGun), GAGE_W("WEAPON_COMBATMG", MachineGun), GAGE_W("WEAPON_COMBATMG_MK2", MachineGun),
        GAGE_W("WEAPON_GUSENBERG", MachineGun), GAGE_W("WEAPON_MINIGUN", MachineGun), GAGE_W("WEAPON_RAYMINIGUN", MachineGun),

        GAGE_W("WEAPON_PUMPSHOTGUN", Shotgun), GAGE_W("WEAPON_PUMPSHOTGUN_MK2", Shotgun),
        GAGE_W("WEAPON_SAWNOFFSHOTGUN", Shotgun), GAGE_W("WEAPON_ASSAULTSHOTGUN", Shotgun),
        GAGE_W("WEAPON_BULLPUPSHOTGUN", Shotgun), GAGE_W("WEAPON_MUSKET", Shotgun), GAGE_W("WEAPON_HEAVYSHOTGUN", Shotgun),
        GAGE_W("WEAPON_DBSHOTGUN", Shotgun), GAGE_W("WEAPON_AUTOSHOTGUN", Shotgun), GAGE_W("WEAPON_COMBATSHOTGUN", Shotgun),

        GAGE_W("WEAPON_SNIPERRIFLE", Sniper), GAGE_W("WEAPON_HEAVYSNIPER", Sniper), GAGE_W("WEAPON_HEAVYSNIPER_MK2", Sniper),
        GAGE_W("WEAPON_MARKSMANRIFLE", Sniper), GAGE_W("WEAPON_MARKSMANRIFLE_MK2", Sniper),

        GAGE_W("WEAPON_RPG", Explosive), GAGE_W("WEAPON_GRENADELAUNCHER", Explosive),
        GAGE_W("WEAPON_GRENADELAUNCHER_SMOKE", Explosive), GAGE_W("WEAPON_FIREWORK", Explosive),
        GAGE_W("WEAPON_RAILGUN", Explosive), GAGE_W("WEAPON_HOMINGLAUNCHER", Explosive),
        GAGE_W("WEAPON_COMPACTLAUNCHER", Explosive), GAGE_W("WEAPON_EMPLAUNCHER", Explosive),
        GAGE_W("WEAPON_GRENADE", Explosive), GAGE_W("WEAPON_STICKYBOMB", Explosive), GAGE_W("WEAPON_PROXMINE", Explosive),
        GAGE_W("WEAPON_PIPEBOMB", Explosive), GAGE_W("WEAPON_MOLOTOV", Explosive), GAGE_W("WEAPON_SMOKEGRENADE", Explosive),
        GAGE_W("WEAPON_BZGAS", Explosive), GAGE_W("WEAPON_FLARE", Explosive), GAGE_W("WEAPON_PETROLCAN", Explosive),
        GAGE_W("WEAPON_HAZARDCAN", Explosive), GAGE_W("WEAPON_FIREEXTINGUISHER", Explosive),
        GAGE_W("WEAPON_SNOWBALL", Explosive), GAGE_W("WEAPON_BALL", Explosive),

        GAGE_W("WEAPON_RUN_OVER_BY_CAR", RunOver), GAGE_W("WEAPON_RAMMED_BY_CAR", RunOver),

        GAGE_W("WEAPON_FALL", Environment), GAGE_W("WEAPON_EXPLOSION", Environment), GAGE_W("WEAPON_FIRE", Environment),
        GAGE_W("WEAPON_DROWNING", Environment), GAGE_W("WEAPON_DROWNING_IN_VEHICLE", Environment),
        GAGE_W("WEAPON_BLEEDING", Environment), GAGE_W("WEAPON_BARBED_WIRE", Environment),
        GAGE_W("WEAPON_ELECTRIC_FENCE", Environment), GAGE_W("WEAPON_EXHAUSTION", Environment),
    };
#undef GAGE_W

    inline WeaponKind KindOf(uint32_t weapon)
    {
        for (const WeaponEntry& e : kWeapons)
            if (e.hash == weapon) return e.kind;
        return WeaponKind::Unknown;
    }

    constexpr size_t kWeaponKinds = 11; // the WeaponKind enumerators (indexes of per-kind tables)
    static_assert((size_t)WeaponKind::Environment + 1 == kWeaponKinds, "kWeaponKinds must cover every WeaponKind");

    // The rule of class `k` with its damage numbers scaled by `scalePercent` (Config::hitDamageScalePercent).
    inline WeaponRule RuleForKind(WeaponKind k, uint32_t scalePercent = 100)
    {
        WeaponRule r = RuleOf(k);
        const auto scale = [&](uint16_t v) { return (uint16_t)std::min<uint64_t>(65535u, (uint64_t)v * scalePercent / 100u); };
        r.maxHit = std::max<uint16_t>(1, scale(r.maxHit));
        r.maxPerSec = std::max<uint16_t>(1, scale(r.maxPerSec));
        return r;
    }
    inline WeaponRule RuleFor(uint32_t weapon, uint32_t scalePercent = 100) { return RuleForKind(KindOf(weapon), scalePercent); }

    // W8 (live test 0.4.2: "a punch while running one-shots and kills"): the per-Hit ceiling of a MELEE Hit by weapon,
    // in the units of WeaponRule::maxHit (the client's claim, before ScaleHit). The class cap alone (100) let one
    // running punch carry 100 x the melee percent (50) = 50 per Hit and 100 per swing (2 Hits), half of a 200-health
    // player, whatever the weapon: the client reports what its 1000-point puppet lost, and a takedown or a running
    // blow can take a lot of that in one go. The server decides what a swing is worth, not the client's number:
    // fists and knuckles 40 (x50% = 20: 10 hits to kill at 200 health), light weapons (knives, sticks, bottles,
    // tools) 60 (30), heavy ones (bat, hatchet, machete, battle axe, crowbar, golf club) 80 (40), and a firearm used
    // as a club or a weapon not in this table 40. ESTIMATES, like the other caps: loose enough that a real blow is not
    // clipped, tight enough that no single blow is a large share of a life. Scaled by hitDamageScalePercent
    // (the operator's tolerance), never below 1.
    inline uint16_t MeleeNominalMax(uint32_t weapon, uint32_t scalePercent = 100)
    {
        struct Entry { uint32_t hash; uint16_t maxHit; };
        static const Entry kTable[] = {
            {Joaat("WEAPON_UNARMED"), 40},     {Joaat("WEAPON_KNUCKLE"), 40},
            {Joaat("WEAPON_KNIFE"), 60},       {Joaat("WEAPON_DAGGER"), 60},      {Joaat("WEAPON_SWITCHBLADE"), 60}, {Joaat("WEAPON_NIGHTSTICK"), 60},
            {Joaat("WEAPON_HAMMER"), 60},      {Joaat("WEAPON_WRENCH"), 60},      {Joaat("WEAPON_BOTTLE"), 60},      {Joaat("WEAPON_FLASHLIGHT"), 60},
            {Joaat("WEAPON_POOLCUE"), 60},     {Joaat("WEAPON_CANDYCANE"), 60},
            {Joaat("WEAPON_BAT"), 80},         {Joaat("WEAPON_HATCHET"), 80},     {Joaat("WEAPON_MACHETE"), 80},     {Joaat("WEAPON_BATTLEAXE"), 80},
            {Joaat("WEAPON_STONE_HATCHET"), 80}, {Joaat("WEAPON_CROWBAR"), 80},   {Joaat("WEAPON_GOLFCLUB"), 80},
        };
        uint16_t base = 40; // a firearm used as a club, or a weapon this table does not know
        for (const Entry& e : kTable)
            if (e.hash == weapon) { base = e.maxHit; break; }
        const uint64_t scaled = (uint64_t)base * scalePercent / 100u;
        return (uint16_t)std::max<uint64_t>(1, std::min<uint64_t>(65535u, scaled));
    }
    // A firearm hit needs a recent shot of the shooter's own, and since v5 so does a melee hit (the client counts
    // a punch or a swing as a "shot" in PedState.shotSeq; before, anybody within 4 m could claim 300 damage a
    // second without swinging); run-over and explosives do not.
    inline bool NeedsShot(WeaponKind k)
    {
        return k == WeaponKind::Unknown || k == WeaponKind::Melee || k == WeaponKind::Handgun || k == WeaponKind::Smg ||
               k == WeaponKind::Rifle || k == WeaponKind::MachineGun || k == WeaponKind::Shotgun || k == WeaponKind::Sniper;
    }

    // ---- constants of the hit rules ------------------------------------------------------------------

    constexpr uint32_t kShotWindowMs = 500;      // default Config::hitShotWindowMs: a Hit's shot was received this recently
    constexpr uint64_t kShotDeferMs = 300;       // a Hit whose shot (or weapon) has not arrived yet waits this long for it
    constexpr size_t   kMaxDeferredHits = 4;     // per player
    constexpr uint64_t kWeaponHeldMs = 500;      // the claimed weapon was reported in a state this recently
    constexpr uint64_t kExplosiveHeldMs = 15000; // a thrown / launched explosive's launch counts this long
    constexpr uint64_t kRespawnGraceMs = 5000;   // after RespawnOk: states still in flight from the old place are not a jump
    constexpr float    kMeleeRange = 4.0f;       // m, shooter to a player target (positions are the server's last)
    constexpr float    kMeleeRangeVehicle = 7.0f; // m, to a vehicle's centre (vehicles are big)
    constexpr float    kRunOverRange = 12.0f;    // m, the shooter's vehicle to the target
    constexpr uint64_t kRunOverMovedMs = 2000;   // the vehicle must have moved this recently ...
    constexpr float    kMovingSpeed = 3.0f;      // ... at least this fast (m/s, measured from displacement)
    constexpr double   kDamageBudgetSec = 2.0;   // burst of the damage-rate bucket, in seconds of a class's rate
    constexpr float    kRegisterSeparation = 4.0f; // m: Register this close to a networked vehicle is refused
    constexpr uint64_t kOrphanDefaultMs = 60000;

    // ---- shots ---------------------------------------------------------------------------------------

    // PedState.shotSeq is a 4-bit counter, so a ring of 16 slots indexed by the value is the whole history:
    // every increment the server sees stamps the slot of the value it passed through.
    struct ShotLog
    {
        struct Slot
        {
            uint64_t atMs = 0;
            uint32_t weapon = 0;
            uint8_t hits = 0;   // Hits credited to this shot so far
            bool valid = false;
        };
        Slot slot[16];
        uint8_t last = 0;
        bool have = false;

        void Reset() { *this = ShotLog(); }
        // A state arrived carrying `seq` (low 4 bits) and `weapon`: the shots between the previous value and
        // this one (at most 15) are stamped now. The first state only sets the baseline (a joiner whose
        // counter is not 0 has not fired anything).
        void OnState(uint8_t seq, uint32_t weapon, uint64_t nowMs)
        {
            seq &= 15;
            if (!have)
            {
                have = true;
                last = seq;
                return;
            }
            const uint8_t d = (uint8_t)((seq - last) & 15);
            for (uint8_t i = 1; i <= d; i++)
            {
                Slot& s = slot[(last + i) & 15];
                s.atMs = nowMs;
                s.weapon = weapon;
                s.hits = 0;
                s.valid = true;
            }
            last = seq;
        }
        // The slot of value `seq` when it was stamped within `windowMs`, else null.
        Slot* Find(uint8_t seq, uint64_t nowMs, uint64_t windowMs)
        {
            Slot& s = slot[seq & 15];
            return s.valid && nowMs >= s.atMs && nowMs - s.atMs <= windowMs ? &s : nullptr;
        }
        // The OLDEST slot stamped within `windowMs` by an explosive-class weapon that has credited fewer hits
        // than an explosive may (a grenade or rocket whose damage arrives long after the launch), else null.
        Slot* FindLaunch(uint64_t nowMs, uint64_t windowMs)
        {
            Slot* best = nullptr;
            for (Slot& s : slot)
            {
                if (!s.valid || nowMs < s.atMs || nowMs - s.atMs > windowMs) continue;
                if (KindOf(s.weapon) != WeaponKind::Explosive || s.hits >= RuleOf(WeaponKind::Explosive).hitsPerShot) continue;
                if (!best || s.atMs < best->atMs) best = &s;
            }
            return best;
        }
        // True when `seq` is 1..8 shots beyond the newest value the server knows: a shot it has probably not
        // been told about yet (its state is late, lost, or behind the reliable Hit in the same datagram).
        bool Ahead(uint8_t seq) const
        {
            if (!have) return false;
            const uint8_t d = (uint8_t)((seq - last) & 15);
            return d >= 1 && d <= 8;
        }
    };

    // The last few distinct weapons a player reported, each with the time it was last reported.
    struct WeaponHistory
    {
        struct E { uint32_t weapon = 0; uint64_t ms = 0; bool used = false; };
        E e[4];
        void Note(uint32_t weapon, uint64_t nowMs)
        {
            E* oldest = &e[0];
            for (E& x : e)
            {
                if (x.used && x.weapon == weapon)
                {
                    x.ms = nowMs;
                    return;
                }
                if (!x.used || (oldest->used && x.ms < oldest->ms)) oldest = &x;
            }
            *oldest = E{weapon, nowMs, true};
        }
        bool Held(uint32_t weapon, uint64_t nowMs, uint64_t windowMs) const
        {
            for (const E& x : e)
                if (x.used && x.weapon == weapon && nowMs >= x.ms && nowMs - x.ms <= windowMs) return true;
            return false;
        }
    };

    // ---- movement ------------------------------------------------------------------------------------

    constexpr double kJitterSec = 0.25;  // arrival jitter the displacement budget forgives (a burst after a stall)
    constexpr float  kSlackMeters = 2.0f; // quantisation, a step or a snap the budget forgives

    // The farthest a position may be from the previous accepted one `dtSec` of SERVER time later, at a speed
    // limit of `speedLimit` m/s. The reported velocity is not used: it is the client's claim.
    inline float AllowedMove(float speedLimit, double dtSec)
    {
        return (float)((double)speedLimit * (std::max(0.0, dtSec) + kJitterSec) + (double)kSlackMeters);
    }
    // Vehicles, getting in or out, ragdolls, falls and parachutes move at vehicle speeds; the rest is on foot.
    inline bool FastPed(const proto::PedState& p)
    {
        return p.move >= proto::kMoveInVehicle ||
               (p.flags & (proto::kPfRagdoll | proto::kPfFalling | proto::kPfParachute)) != 0;
    }
    inline float Dist(const proto::Kinematics& a, const proto::Kinematics& b)
    {
        const double dx = (double)a.x - b.x, dy = (double)a.y - b.y, dz = (double)a.z - b.z;
        return (float)std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // ---- connect rate --------------------------------------------------------------------------------

    // At most `perMinute` connects per source address per minute: a token bucket of that size refilled evenly
    // over 60 s. The table is bounded; when it is full of busy sources a NEW source is let through (an
    // anti-abuse limiter must not lock real players out because someone filled its table).
    class ConnectLimiter
    {
    public:
        static constexpr size_t kMaxSources = 4096;
        void Configure(uint32_t perMinute)
        {
            perMinute_ = perMinute;
            sources_.clear();
        }
        bool Allow(uint32_t source, uint64_t nowMs)
        {
            if (perMinute_ == 0) return true;
            auto it = sources_.find(source);
            if (it == sources_.end())
            {
                if (sources_.size() >= kMaxSources)
                {
                    Sweep(nowMs);
                    if (sources_.size() >= kMaxSources) return true;
                }
                it = sources_.emplace(source, TokenBucket((double)perMinute_, (double)perMinute_ / 60.0)).first;
            }
            return it->second.Take(nowMs);
        }
        size_t tracked() const { return sources_.size(); }

    private:
        void Sweep(uint64_t nowMs)
        {
            for (auto it = sources_.begin(); it != sources_.end();)
            {
                if (it->second.Full(nowMs)) it = sources_.erase(it);
                else ++it;
            }
        }
        uint32_t perMinute_ = 0;
        std::unordered_map<uint32_t, TokenBucket> sources_;
    };

    // ---- animations ----------------------------------------------------------------------------------

    // TASK_PLAY_ANIM flag bits (Rockstar's eScriptedAnimFlags, as the community documents them; from memory,
    // not verified against the game's scripts). The allowed ones only change how the clip itself plays:
    //   1 loop, 2 hold the last frame, 4 reposition when finished, 8 not interruptable, 16 upper body only,
    //   32 secondary slot (the player keeps control), 64 reorient when finished, 128 abort on ped movement,
    //   256 additive.
    // Everything above (512 turn off collision, 1024 override physics, 2048 ignore gravity, 4096 extract
    // initial offset, 8192 exit after interrupted, tag syncs, 131072 force start, kinematic physics, mover
    // extraction, 1048576 hide weapon, 2097152 ends in dead pose, 4194304 ragdoll on collision, 8388608 don't
    // exit on death, abort on weapon damage, ... ) can pin, float, kill-pose or un-collide the sender's ped on
    // every other screen, so the server strips it.
    constexpr uint32_t kAnimFlagsAllowed = 0x1FFu;
    // playbackRate is x100 on the wire (100 = normal speed): 0.10x .. 2.00x. A frozen (0) or absurdly fast clip
    // is not an emote.
    constexpr uint8_t kAnimRateMin = 10, kAnimRateMax = 200;

    // Strips the disallowed flag bits and clamps the playback rate in place. True when anything changed.
    inline bool SanitizeAnim(proto::Anim& a)
    {
        bool changed = false;
        if (a.flags & ~kAnimFlagsAllowed)
        {
            a.flags &= kAnimFlagsAllowed;
            changed = true;
        }
        const uint8_t rate = std::min(kAnimRateMax, std::max(kAnimRateMin, a.playbackRate));
        if (rate != a.playbackRate)
        {
            a.playbackRate = rate;
            changed = true;
        }
        return changed;
    }

}
