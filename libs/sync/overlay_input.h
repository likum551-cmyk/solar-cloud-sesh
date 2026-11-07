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
#include <cstdint>

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

    // ---- 0.5.6 (GI 2026-11-07: "clicking inventory slots shoots the gun", "tab by default switches guns ... remove all other func besides our
    // server menu", "binding to slot and then clicking 1-2-3-4 doesn't bring the gun / item into active slot") ------------------------------
    //
    // overlay_fire_block (default on; 0 = 0.5.5): while a panel is open the engine also calls DISABLE_PLAYER_FIRING(PLAYER_ID(), true) every
    // tick ("inhibits the player from using any method of combat including melee and firearms", one frame per call). GI's 0.5.5 log
    // (07:34:39-49) counted 3 IS_PED_SHOOTING shots and the server judged a melee swing (07:35:25) while the page was open and all 240
    // controls above were held: the control list alone did not stop the attack. The player-level block does not depend on which control GTA
    // fired through.
    //
    // weapon_keys (default on; 0 = 0.5.5): while the server's inventory owns the weapons (it offered the inventory world and we answered), GTA's
    // own weapon selection is held disabled EVERY tick, panel open or not, so Tab is only our menu and the number keys 1-4 only our quick
    // slots. Indices and PC bindings from the FiveM controls table (docs.fivem.net/docs/game-references/controls, read 2026-11-07):
    //   12 INPUT_WEAPON_WHEEL_UD (mouse)      13 INPUT_WEAPON_WHEEL_LR (mouse)      14 INPUT_WEAPON_WHEEL_NEXT (wheel down)
    //   15 INPUT_WEAPON_WHEEL_PREV (wheel up) 16 INPUT_SELECT_NEXT_WEAPON (wheel)   17 INPUT_SELECT_PREV_WEAPON (wheel)
    //   37 INPUT_SELECT_WEAPON (TAB)          99 INPUT_VEH_SELECT_NEXT_WEAPON (wheel up)   100 INPUT_VEH_SELECT_PREV_WEAPON ([)
    //   115 INPUT_VEH_FLY_SELECT_NEXT_WEAPON (wheel up)   116 INPUT_VEH_FLY_SELECT_PREV_WEAPON ([)
    //   157 INPUT_SELECT_WEAPON_UNARMED (1)   158 _MELEE (2)   159 _HANDGUN (6)   160 _SHOTGUN (3)   161 _SMG (7)   162 _AUTO_RIFLE (8)
    //   163 _SNIPER (9)   164 _HEAVY (4)   165 _SPECIAL (5)
    //   261 INPUT_PREV_WEAPON (wheel up)      262 INPUT_NEXT_WEAPON (wheel down)
    // plus HUD_SUPPRESS_WEAPON_WHEEL_RESULTS_THIS_FRAME (a wheel that opens anyway selects nothing). Ctrl+Tab (the admin pages) is the overlay's
    // own key handling and does not go through these controls.
    constexpr int kGameWeaponSelect[] = {
        12, 13, 14, 15, 16, 17,                          // the weapon wheel's axes and the mouse-wheel next / previous
        37,                                              // INPUT_SELECT_WEAPON: Tab (tap = switch, hold = the wheel)
        99, 100, 115, 116,                               // the vehicle / aircraft weapon next / previous
        157, 158, 159, 160, 161, 162, 163, 164, 165,     // the number keys 1-9: weapon categories
        261, 262,                                        // INPUT_PREV_WEAPON / INPUT_NEXT_WEAPON (mouse wheel)
    };
    constexpr int kGameWeaponSelectCount = (int)(sizeof(kGameWeaponSelect) / sizeof(kGameWeaponSelect[0]));

    // What the engine does this tick about input, from the panel state and the switches.
    struct TickPlan
    {
        bool holdPanel = false;      // the panel list above (HeldControls)
        bool disableFiring = false;  // DISABLE_PLAYER_FIRING(PLAYER_ID(), true)
        bool holdWeaponKeys = false; // kGameWeaponSelect + HUD_SUPPRESS_WEAPON_WHEEL_RESULTS_THIS_FRAME
    };
    // panelOpen: a panel owns the keyboard and mouse (chat, Tab menu); fireBlock / weaponKeys: the switches; weaponsAreItems: this session's
    // server owns the weapons (inventory world offered and answered). Without it GTA's own weapon wheel is the only way to pick a weapon, so it
    // stays (an old server, or inv_world=0).
    inline TickPlan Plan(bool panelOpen, bool fireBlock, bool weaponKeys, bool weaponsAreItems)
    {
        TickPlan p;
        p.holdPanel = panelOpen;
        p.disableFiring = panelOpen && fireBlock;
        p.holdWeaponKeys = weaponKeys && weaponsAreItems;
        return p;
    }

    // The quick slot a number key opens (digits 1-4 -> slots 0-3), -1 for any other key.
    inline int QuickSlotOfDigit(int digit) { return digit >= 1 && digit <= 4 ? digit - 1 : -1; }

    // How many GAME frames each script tick covers (GET_FRAME_COUNT read once per tick). DISABLE_CONTROL_ACTION and DISABLE_PLAYER_FIRING last
    // one frame: if the script tick ran on fewer frames than the game, every other frame would leave the controls live. The 0.5.5 log shows
    // two Presents per script tick (e.g. 1995 frames, 998 ticks in 10 s); this tells live whether that is two game frames or one frame
    // presented twice. Pure: feed it the counter (-1 = unreadable).
    struct FrameCadence
    {
        uint32_t ticks = 0, frames = 0, gapsOver1 = 0, maxGap = 0, unreadable = 0;
        int64_t last = -1;
        void Note(int64_t frameCount)
        {
            ++ticks;
            if (frameCount < 0) {
                ++unreadable;
                return;
            }
            if (last >= 0 && frameCount > last) {
                const int64_t step = frameCount - last;   // no std::min here: windows.h may define it as a macro
                const uint32_t gap = (uint32_t)(step < 1000000 ? step : 1000000);
                frames += gap;
                if (gap > 1)
                    ++gapsOver1;
                if (gap > maxGap)
                    maxGap = gap;
            }
            last = frameCount;   // a counter that went backwards (a new session's game) restarts the count
        }
        void ResetWindow() { const int64_t keep = last; *this = FrameCadence(); last = keep; }
    };

    // A change of the ped's selected weapon, attributed: ours (the inventory's reconcile asked for something within kOursMs) or the game's
    // own (Tab, a number key, the mouse wheel, an empty clip...). With weapon_keys on, a foreign switch while nothing else explains it is the
    // live sign that a selection still got through.
    struct SwitchWatch
    {
        static constexpr double kOursMs = 1500;
        uint32_t last = 0;
        double oursAtMs = -1e9;
        uint32_t ours = 0, foreign = 0;
        void OursNow(double nowMs) { oursAtMs = nowMs; }
        // current = GET_SELECTED_PED_WEAPON (0 = unreadable: ignored). Returns true for a foreign switch.
        bool Note(uint32_t current, double nowMs)
        {
            if (!current)
                return false;
            if (!last || current == last) {
                last = current;
                return false;
            }
            last = current;
            if (nowMs - oursAtMs <= kOursMs) {
                ++ours;
                return false;
            }
            ++foreign;
            return true;
        }
    };

} // namespace gage::sync::overlayinput
