// overlay_input.h (0.5.4, overlay_combat_block): which of the game's pad-0 controls the engine holds disabled while an overlay panel (the chat,
// the Tab inventory / menu) owns the keyboard and mouse. Pure C++17; the engine's UiTick performs it (DISABLE_CONTROL_ACTION(0, c, true) for
// every control in the list, every script tick: the native is a per-frame effect). tests/overlay_input_tests.h pins it.
//
// GI 2026-11-07: "when you left click while holding a gun or whatever it sends shoot / attack action to bg game when in inventory overlay". The
// 0.4.x loop disabled controls 0..226 only (kHeldUpTo0_5_3), but GTA binds the left mouse button to more than INPUT_ATTACK (24): INPUT_ATTACK2
// (257), and the melee / vehicle-melee and weapon-switch controls also sit above 226. Indices and default PC bindings: the FiveM controls table
// (docs.fivem.net/docs/game-references/controls, read 2026-11-07; the highest index there is 360):
//   257 INPUT_ATTACK2 (LMB)            261 INPUT_PREV_WEAPON (wheel up)    262 INPUT_NEXT_WEAPON (wheel down)
//   263 INPUT_MELEE_ATTACK1 (R)        264 INPUT_MELEE_ATTACK2 (Q)
//   274-277 INPUT_SNIPER_ZOOM_IN / _OUT / _IN_ALTERNATE / _OUT_ALTERNATE
//   331 INPUT_VEH_FLY_ATTACK2 (RMB)    345 INPUT_VEH_MELEE_HOLD (X)        346 INPUT_VEH_MELEE_LEFT (LMB)    347 INPUT_VEH_MELEE_RIGHT (RMB)
// Below 227 (already held): 24 ATTACK, 25 AIM, 12-17 weapon wheel / select, 37 SELECT_WEAPON, 39-43 sniper zoom, 47 DETONATE, 50 ACCURATE_AIM,
// 53 / 54 WEAPON_SPECIAL, 58 THROW_GRENADE, 68-70 VEH_AIM / ATTACK / ATTACK2, 91 / 92 passenger aim / attack, 114 VEH_FLY_ATTACK, 140-143 melee.
// Only named indices are added, never a blind 0..360 loop: an index the build does not know is not something to hand the game every frame.
// While the game's pause menu is up, 199 / 200 / 202 stay enabled (Esc, P and Backspace close it), as before.
#pragma once

#include "sync/pausemenu.h"

#include <cstddef>

namespace gage::sync::overlayinput {

    constexpr int kHeldUpTo0_5_3 = 227;   // the 0.4.x - 0.5.3 loop: controls 0 .. 226

    constexpr int kCombatAbove226[] = {
        257,                    // INPUT_ATTACK2: the left mouse button's second attack binding (the shot that reached the game)
        261, 262,               // INPUT_PREV_WEAPON / INPUT_NEXT_WEAPON (mouse wheel)
        263, 264,               // INPUT_MELEE_ATTACK1 / 2
        274, 275, 276, 277,     // sniper zoom in / out (and alternates)
        331,                    // INPUT_VEH_FLY_ATTACK2
        345, 346, 347,          // INPUT_VEH_MELEE_HOLD / LEFT / RIGHT
    };
    constexpr int kCombatAbove226Count = (int)(sizeof(kCombatAbove226) / sizeof(kCombatAbove226[0]));
    constexpr int kMaxHeld = kHeldUpTo0_5_3 + kCombatAbove226Count;

    // Fills out[0..n) with the controls to hold disabled this tick and returns n (<= kMaxHeld). combatBlock = the overlay_combat_block switch
    // (false = the 0.5.3 set: 0 .. 226 only); pauseMenuOpen keeps 199 / 200 / 202 enabled so the menu can be closed.
    inline int HeldControls(bool combatBlock, bool pauseMenuOpen, int* out, int cap)
    {
        int n = 0;
        auto add = [&](int c) {
            if (pauseMenuOpen && (c == kControlFrontendPause || c == kControlFrontendPauseAlt || c == kControlFrontendCancel))
                return;
            if (n < cap)
                out[n++] = c;
        };
        for (int c = 0; c < kHeldUpTo0_5_3; ++c)
            add(c);
        if (combatBlock)
            for (int i = 0; i < kCombatAbove226Count; ++i)
                add(kCombatAbove226[i]);
        return n;
    }

} // namespace gage::sync::overlayinput
