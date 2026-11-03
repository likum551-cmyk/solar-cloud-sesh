// The server's damage scaling (protocol v5): what a shooter's Hit reports versus what the victim loses.
// Pure functions over the config and the Hit's numbers, header-only, no clocks, no state.
//
// The client's number is the damage the single-player game would have done to a 1000-point puppet, without
// Online's freemode factor, so a carbine killed a 100-health player in 4 hits and fists in 4 punches. ScaleHit
// turns it into the number the server applies:
//
//   damage = round(claimed * classPercent / 100) , at least 1
//   class  = firearms (handgun, smg, rifle, machine gun, shotgun, sniper, and a weapon not in the table)
//            Config::firearmDamagePercent, melee Config::meleeDamagePercent, explosives
//            Config::explosiveDamagePercent, run-over Config::runOverDamagePercent; Environment (a fall, fire,
//            drowning: only ever a hit on oneself) is never scaled.
//
// A firearm Hit on the head bone (kBoneHead, SKEL_Head) multiplies that by Config::headshotPercent / 100 at
// close range, falling linearly to x1 at the far distance (kHeadshotNearM .. kHeadshotFarM, or
// kHeadshotFarSniperM for the sniper class): a rifle's headshot is full up to 5 m and a plain body shot from
// 40 m on. Fists, blades, explosives and run-overs never get it. The caller decides whether the bone counts
// (the server passes the bone only for the first Hit of a shot, so a shotgun's pellets cannot all be headshots).
//
// Defaults (Config{}): firearms 72, melee 50, explosives 100, run-over 100, headshot 300, playerHealth 200.
// At 200 health and no armour that is a carbine (32) in 9 hits, a pistol (26) in 11, fists (30) in 14, a
// carbine headshot at 3 m in 3. Every percent at 100 (headshot 100) is the v4 behaviour.
//
// Thread-safety: none needed (pure). Errors: none throw.
#pragma once

#include "anticheat.h"
#include "config.h"
#include "protocol/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gage::server {

    constexpr int16_t kBoneHead = 31086;           // GTA's SKEL_Head bone id (a Hit.bone naming the head)
    constexpr float kHeadshotNearM = 5.0f;         // the full headshot multiplier up to this distance
    constexpr float kHeadshotFarM = 40.0f;         // ... falling linearly to x1 at this distance
    constexpr float kHeadshotFarSniperM = 300.0f;  // ... or at this one for the sniper class

    // The result of ScaleHit: the damage to apply and whether the headshot bonus was given.
    struct HitScale
    {
        uint16_t damage = 0;
        bool headshot = false;
        bool floored = false;   // the scaled value rounded to under 1 and the 1-point floor was applied instead (0.4.5 diagnostics)
    };

    // A weapon class that can score a headshot: every firearm (and a weapon not in the table, which is treated
    // as a generic firearm like everywhere else in the rules).
    constexpr bool CanHeadshot(ac::WeaponKind k)
    {
        return k == ac::WeaponKind::Unknown || k == ac::WeaponKind::Handgun || k == ac::WeaponKind::Smg || k == ac::WeaponKind::Rifle ||
               k == ac::WeaponKind::MachineGun || k == ac::WeaponKind::Shotgun || k == ac::WeaponKind::Sniper;
    }

    // The headshot multiplier in percent at `distM` metres: Config::headshotPercent up to kHeadshotNearM, then
    // linear down to 100 at the class's far distance, 100 beyond.
    inline double HeadshotMultiplierPercent(const Config& cfg, ac::WeaponKind eff, float distM)
    {
        // (not named `far`: windows.h defines that as a macro)
        const double reachM = eff == ac::WeaponKind::Sniper ? (double)kHeadshotFarSniperM : (double)kHeadshotFarM;
        const double top = (double)cfg.headshotPercent;
        if (!(distM > kHeadshotNearM)) return top; // also a NaN distance: the close-range value
        if (distM >= reachM) return 100.0;
        return 100.0 + (top - 100.0) * (1.0 - ((double)distM - (double)kHeadshotNearM) / (reachM - (double)kHeadshotNearM));
    }

    // `eff` is the class whose rules the Hit passed (a gun used as a club is Melee), `claimed` the Hit's damage
    // after the protocol and class caps and the budget, `bone` the Hit's bone (-1 unknown), `distM` the
    // shooter-to-target distance in metres.
    inline HitScale ScaleHit(const Config& cfg, ac::WeaponKind eff, uint16_t claimed, int16_t bone, float distM)
    {
        uint32_t percent = 100;
        switch (eff)
        {
        case ac::WeaponKind::Melee:       percent = cfg.meleeDamagePercent; break;
        case ac::WeaponKind::Explosive:   percent = cfg.explosiveDamagePercent; break;
        case ac::WeaponKind::RunOver:     percent = cfg.runOverDamagePercent; break;
        case ac::WeaponKind::Environment: percent = 100; break;
        case ac::WeaponKind::Unknown:
        case ac::WeaponKind::Handgun:
        case ac::WeaponKind::Smg:
        case ac::WeaponKind::Rifle:
        case ac::WeaponKind::MachineGun:
        case ac::WeaponKind::Shotgun:
        case ac::WeaponKind::Sniper:      percent = cfg.firearmDamagePercent; break;
        }
        HitScale out;
        double scaled = (double)claimed * (double)percent / 100.0;
        if (bone == kBoneHead && CanHeadshot(eff))
        {
            out.headshot = true;
            scaled = (double)claimed * (double)percent * HeadshotMultiplierPercent(cfg, eff, distM) / 10000.0;
        }
        // Never 0 for a Hit that carried damage; the largest result is kMaxHitDamage (250) x 1000 percent x 1000
        // percent = 25000, inside a u16, and the min below keeps it so whatever the caller passes.
        const double rounded = std::floor(scaled + 0.5);
        out.floored = claimed > 0 && rounded < 1.0;
        out.damage = (uint16_t)std::min<double>(std::max<double>(rounded, claimed > 0 ? 1.0 : 0.0), 65535.0);
        return out;
    }

    // The proto::kDmg* bits of a Damage message: the head bone got the bonus, the class is melee / explosive,
    // armour took part of the hit.
    constexpr uint8_t DamageFlags(bool headshot, ac::WeaponKind eff, bool armourAbsorbed)
    {
        return (uint8_t)((headshot ? proto::kDmgHeadshot : 0) | (eff == ac::WeaponKind::Melee ? proto::kDmgMelee : 0) |
                         (eff == ac::WeaponKind::Explosive ? proto::kDmgExplosion : 0) | (armourAbsorbed ? proto::kDmgArmour : 0));
    }

}
