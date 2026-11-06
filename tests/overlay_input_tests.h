// 0.5.4 overlay_combat_block: the controls the engine holds disabled while an overlay panel is open (libs/sync/overlay_input.h). Included by
// tests/gameplay_test.cpp after its Check() and F(); run by overlayinput_t::RunPure(), kChecks checks.
#pragma once

#include "sync/overlay_input.h"

#include <set>

namespace overlayinput_t {

    using namespace gage::sync::overlayinput;

    constexpr int kChecks = 5;

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
    }

} // namespace overlayinput_t
