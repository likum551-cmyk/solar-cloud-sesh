// 0.5.4 overlay_combat_block: the controls the engine holds disabled while an overlay panel is open (libs/sync/overlay_input.h). Included by
// tests/gameplay_test.cpp after its Check() and F(); run by overlayinput_t::RunPure(), kChecks checks.
#pragma once

#include "sync/overlay_input.h"

#include <set>

namespace overlayinput_t {

    using namespace gage::sync::overlayinput;

    constexpr int kChecks = 11;   // 0.5.6: + 6 (RunInputHolds)

    inline void RunInputHolds(gage::sync::overlayinput::TickPlan (*plan)(bool, bool, bool, bool) = gage::sync::overlayinput::Plan);
    // The 0.5.5 tick, for the fail-first replay (RunInputHolds(Plan055)): an open panel held its control list only, a closed one held
    // nothing of the weapon selection.
    inline TickPlan Plan055(bool panelOpen, bool, bool, bool) { TickPlan p; p.holdPanel = panelOpen; return p; }

    inline std::set<int> Held(bool combat, bool pause)
    {
        int out[kMaxHeld + 8];
        const int n = HeldControls(combat, pause, out, kMaxHeld + 8);
        return std::set<int>(out, out + n);
    }

    inline void RunPure()
    {
        {
            // GI's report: a left click in the Tab inventory fired the gun. The 0.5.3 set (combatBlock off) leaves INPUT_ATTACK2 (257, LMB) and the
            // melee / weapon-switch controls enabled: this is the defect, pinned so a later edit cannot quietly bring it back as the default.
            const std::set<int> old = Held(false, false);
            const bool leak = old.count(24) && old.count(25) && !old.count(257) && !old.count(263) && !old.count(264) && !old.count(346) &&
                              old.size() == (size_t)kHeldUpTo0_5_3;
            Check("overlay input: the 0.5.3 set (overlay_combat_block=0) is 0..226 and leaves LMB's INPUT_ATTACK2 (257) and melee 263 / 264 / 346 enabled",
                  leak, F("size %d", (int)old.size()));
        }
        {
            const std::set<int> now = Held(true, false);
            const int want[] = {24, 25, 257, 261, 262, 263, 264, 274, 275, 276, 277, 331, 345, 346, 347, 140, 141, 142, 143, 37, 47, 58, 68, 69, 70, 114};
            int missing = 0;
            for (int c : want)
                missing += now.count(c) == 0;
            Check("overlay input: with overlay_combat_block on, every attack / aim / melee / weapon-switch / vehicle-attack control is held (24, 25, 257, "
                  "261-264, 274-277, 331, 345-347 and the 0..226 ones)",
                  missing == 0 && now.size() == (size_t)kMaxHeld, F("missing %d, size %d of %d", missing, (int)now.size(), kMaxHeld));
        }
        {
            // No duplicates (the set size equals the count written) and nothing outside the game's table (FiveM's highest index is 360).
            int out[kMaxHeld + 8];
            const int n = HeldControls(true, false, out, kMaxHeld + 8);
            bool inRange = true;
            for (int i = 0; i < n; ++i)
                inRange = inRange && out[i] >= 0 && out[i] <= 360;
            Check("overlay input: the list has no duplicates and every index is inside 0..360", n == (int)std::set<int>(out, out + n).size() && inRange && n == kMaxHeld,
                  F("n %d", n));
        }
        {
            // The pause menu keeps P / Esc / Backspace enabled (both sets), and nothing else is dropped.
            const std::set<int> p = Held(true, true), q = Held(false, true);
            Check("overlay input: while the pause menu is open 199 / 200 / 202 stay enabled and the combat extras are still held",
                  !p.count(199) && !p.count(200) && !p.count(202) && p.count(257) && p.size() == (size_t)kMaxHeld - 3 && !q.count(200) &&
                      q.size() == (size_t)kHeldUpTo0_5_3 - 3,
                  F("p %d q %d", (int)p.size(), (int)q.size()));
        }
        {
            // A short buffer is never overrun.
            int out[10];
            for (int& v : out)
                v = -7;
            const int n = HeldControls(true, false, out, 5);
            Check("overlay input: a buffer of 5 gets exactly 5 entries and nothing past it", n == 5 && out[5] == -7 && out[9] == -7, F("n %d", n));
        }
        RunInputHolds();
    }

    // ---- 0.5.6 overlay_fire_block / weapon_keys ----
    inline void RunInputHolds(TickPlan (*plan)(bool, bool, bool, bool))
    {
        {
            // GI 0.5.5: a click in the open inventory still shot (3 IS_PED_SHOOTING in 10 s with 240 controls held) and punched. With the switch on
            // an open panel also blocks firing at the player level; with it off, or with no panel, nothing changes.
            const TickPlan open = plan(true, true, true, true), off = plan(true, false, true, true), closed = plan(false, true, true, true);
            Check("input holds (0.5.6): an open panel calls DISABLE_PLAYER_FIRING (overlay_fire_block on), not with the switch off, not with no panel",
                  open.holdPanel && open.disableFiring && off.holdPanel && !off.disableFiring && !closed.holdPanel && !closed.disableFiring);
        }
        {
            // GI: "tab by default switches guns", "clicking 1-2-3-4 doesn't bring the gun": while the server owns the weapons GTA's own selection
            // is held EVERY tick, page open or closed; an old server (no inventory world) or weapon_keys=0 leaves GTA's wheel as the only way.
            const TickPlan world = plan(false, true, true, true), oldServer = plan(false, true, true, false), off = plan(false, true, false, true);
            Check("input holds (0.5.6): GTA's weapon selection is held with the page CLOSED once the server owns the weapons; not on an old server, "
                  "not with weapon_keys=0",
                  world.holdWeaponKeys && !oldServer.holdWeaponKeys && !off.holdWeaponKeys);
        }
        {
            // The list: the digit keys 1-9 (FiveM table: 1 157, 2 158, 3 160, 4 164, 5 165, 6 159, 7 161, 8 162, 9 163), Tab 37, the wheel
            // and the mouse-wheel switches; no duplicates; nothing that moves, aims or shoots (those stay the player's while the page is closed).
            const std::set<int> s(kGameWeaponSelect, kGameWeaponSelect + kGameWeaponSelectCount);
            int missing = 0;
            for (int c : {157, 158, 159, 160, 161, 162, 163, 164, 165, 37, 12, 13, 14, 15, 16, 17, 99, 100, 115, 116, 261, 262})
                missing += s.count(c) == 0;
            int gameplay = 0;
            for (int c : {1, 2, 21, 22, 24, 25, 30, 31, 32, 33, 34, 35, 44, 45, 140, 141, 142, 257})
                gameplay += (int)s.count(c);
            Check("input holds (0.5.6): the weapon-select list is the digit keys 157-165, Tab 37, the wheel 12-17, vehicle 99 / 100 / 115 / 116 and "
                  "261 / 262, without duplicates, and holds no look / move / jump / sprint / aim / attack / melee control",
                  missing == 0 && gameplay == 0 && (int)s.size() == kGameWeaponSelectCount && kGameWeaponSelectCount == 22,
                  F("missing %d gameplay %d size %d", missing, gameplay, kGameWeaponSelectCount));
        }
        Check("input holds (0.5.6): the number keys 1-4 are quick slots 0-3, every other digit is none",
              QuickSlotOfDigit(1) == 0 && QuickSlotOfDigit(4) == 3 && QuickSlotOfDigit(0) == -1 && QuickSlotOfDigit(5) == -1 && QuickSlotOfDigit(-1) == -1);
        {
            // The cadence probe: one tick per game frame reads as such; a tick every other frame (the case where per-frame disables leak) shows
            // up as frames = 2 x ticks; an unreadable counter is counted, never folded in; a counter that restarts (a new game) is not a gap.
            FrameCadence every, other, odd;
            for (int f = 100; f < 200; ++f) every.Note(f);
            for (int f = 100; f < 300; f += 2) other.Note(f);
            odd.Note(50); odd.Note(-1); odd.Note(51); odd.Note(5); odd.Note(6);
            Check("input holds (0.5.6): the cadence probe tells one tick per frame (99 frames, no gap) from one tick per two frames (198 frames, 99 "
                  "gaps of 2); unreadable counted; a restarted counter is no gap",
                  every.frames == 99 && every.gapsOver1 == 0 && every.maxGap == 1 && other.frames == 198 && other.gapsOver1 == 99 && other.maxGap == 2 &&
                      odd.ticks == 5 && odd.unreadable == 1 && odd.frames == 2 && odd.maxGap == 1,
                  F("every %u/%u other %u/%u/%u odd %u/%u/%u", every.frames, every.gapsOver1, other.frames, other.gapsOver1, other.maxGap, odd.frames,
                    odd.unreadable, odd.maxGap));
        }
        {
            // A switch the inventory asked for (within 1.5 s) is ours; Tab / a number key / the wheel is foreign; 0 (unreadable) is ignored.
            SwitchWatch w;
            w.Note(0xA2719263u, 0);              // first read: no switch
            w.OursNow(1000);
            w.Note(0x1B06D571u, 1200);           // our select
            w.Note(0u, 1300);                    // unreadable
            const bool foreign = w.Note(0xA2719263u, 5000);   // GTA's own (Tab tap / key 1)
            Check("input holds (0.5.6): a weapon switch within 1.5 s of an inventory act is ours, a later one is FOREIGN, an unreadable read is "
                  "ignored",
                  w.ours == 1 && w.foreign == 1 && foreign && w.last == 0xA2719263u, F("ours %u foreign %u", w.ours, w.foreign));
        }
    }

} // namespace overlayinput_t
