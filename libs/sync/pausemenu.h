// The no-pause pause menu: while connected to a GAGE server Esc / P must not pause the world (the other
// players would see us frozen: the pause menu stops the script thread and our sender). Pure C++17,
// single-threaded (the engine's script tick), tested by tests/gameplay_test.cpp.
//
// Facts (GameplayFixesV source/utils/player.cpp nHUD::AllowGameExecutionOnPauseMenu, GPLv3: facts only;
// citizenfx/natives HUD/ActivateFrontendMenu.md and HUD/SetPauseMenuActive.md, which call
// SET_PAUSE_MENU_ACTIVE deprecated and name DISABLE_FRONTEND_THIS_FRAME as the block):
//   ACTIVATE_FRONTEND_MENU(menuHash, togglePause, component): togglePause false opens the menu WITHOUT
//   pausing the game; menuHash FE_MENU_VERSION_SP_PAUSE (gtaids.h), component -1 = the default tab.
//   DISABLE_FRONTEND_THIS_FRAME blocks the engine's own (pausing) open for one frame.
//
// Every script tick, while connected:
//   * the menu is NOT open: controls 199 (P, INPUT_FRONTEND_PAUSE) and 200 (Esc,
//     INPUT_FRONTEND_PAUSE_ALTERNATE) are held disabled on pad 0, and the frontend is disabled this
//     frame, so the game never opens its own pausing menu;
//   * a press of 199 or 200 (IS_DISABLED_CONTROL_JUST_PRESSED, which still reads a disabled control)
//     opens it ourselves with togglePause false: on that frame the frontend is NOT disabled, and for
//     activateGraceMs after it (the request may take a frame or more to show as active) it is not
//     disabled either;
//   * not while a chat / palette panel is interactive (Esc closes the panel first), nor for
//     overlayDebounceMs after one was (the panel closes on the same Esc that this frame still reads);
//   * not before the entry is done (canOpen): the world is not live yet;
//   * the menu IS open: nothing is disabled, so Esc, Backspace (202) and P close it again;
//   * while it is open, one proof line every proofEveryMs (the engine logs the game timer's advance
//     and the local speed: a paused world would show 0 / no line at all).
#pragma once

#include "gtaids.h"

namespace gage::sync {

    constexpr int kControlFrontendPause = 199;       // INPUT_FRONTEND_PAUSE (P)
    constexpr int kControlFrontendPauseAlt = 200;    // INPUT_FRONTEND_PAUSE_ALTERNATE (Esc)
    constexpr int kControlFrontendCancel = 202;      // INPUT_FRONTEND_CANCEL (Backspace)

    struct PauseMenuConfig
    {
        double activateGraceMs = 500.0;
        double overlayDebounceMs = 250.0;
        double proofEveryMs = 2000.0;
    };

    struct PauseMenuIn
    {
        double nowMs = 0;
        bool   connected = false;           // a GAGE session is up
        bool   canOpen = false;             // and the entry is done
        bool   menuActive = false;          // IS_PAUSE_MENU_ACTIVE
        bool   openPressed = false;         // IS_DISABLED_CONTROL_JUST_PRESSED(0, 199) or (0, 200); read only when ControlsHeld
        bool   overlayInteractive = false;  // a chat / palette panel owns the keyboard
    };

    struct PauseMenuOut
    {
        bool disableFrontend = false;   // DISABLE_FRONTEND_THIS_FRAME (or the SET_PAUSE_MENU_ACTIVE(false) fallback)
        bool activate = false;          // ACTIVATE_FRONTEND_MENU(kFeMenuVersionSpPause, false, -1) now
        bool justOpened = false;        // the menu reads active for the first time: take the game-timer baseline
        bool justClosed = false;
        bool logProof = false;          // write one "pause menu open" proof line now
    };

    class PauseMenuGate
    {
    public:
        explicit PauseMenuGate(const PauseMenuConfig& cfg = PauseMenuConfig()) : cfg_(cfg) {}

        // Whether 199 and 200 are held disabled this frame (decided BEFORE the engine reads openPressed,
        // since the press is read from the disabled control): connected and the menu not open.
        static bool ControlsHeld(bool connected, bool menuActive) { return connected && !menuActive; }

        PauseMenuOut Tick(const PauseMenuIn& in)
        {
            PauseMenuOut out;
            if (!in.connected)
            {
                if (wasActive_)
                    out.justClosed = true;
                wasActive_ = false;
                graceUntilMs_ = 0;
                return out;
            }
            if (in.menuActive)
            {
                out.justOpened = !wasActive_;
                wasActive_ = true;
                graceUntilMs_ = 0;
                if (out.justOpened)
                    nextProofMs_ = in.nowMs + cfg_.proofEveryMs;
                else if (in.nowMs >= nextProofMs_)
                {
                    out.logProof = true;
                    nextProofMs_ = in.nowMs + cfg_.proofEveryMs;
                }
                return out;
            }
            if (wasActive_)
            {
                out.justClosed = true;
                wasActive_ = false;
            }
            const bool debounced = in.nowMs - lastInteractiveMs_ >= cfg_.overlayDebounceMs;
            if (in.overlayInteractive)
                lastInteractiveMs_ = in.nowMs;
            if (in.openPressed && in.canOpen && !in.overlayInteractive && debounced)
            {
                out.activate = true;                    // this frame's frontend stays enabled
                graceUntilMs_ = in.nowMs + cfg_.activateGraceMs;
                return out;
            }
            out.disableFrontend = in.nowMs >= graceUntilMs_;
            return out;
        }

        bool Open() const { return wasActive_; }

    private:
        PauseMenuConfig cfg_;
        bool wasActive_ = false;
        double nextProofMs_ = 0, graceUntilMs_ = 0, lastInteractiveMs_ = -1e9;
    };

}
