// Alt+F4 / window close: which window messages mean "the player wants out", and a once-only gate
// for the engine's quit handler. Header-only and free of <windows.h> (the message ids are the
// documented Win32 values), so tests/gage_mp_test.cpp checks it directly.
//
// GTA's own handling of these messages shows a "quit game?" warning screen that pauses the game
// (the world, the script thread, our sender: remote players see us frozen). The overlay's WndProc
// subclass (overlay.cpp) keeps them from the game: the first opens the overlay's own confirm card
// (which pauses nothing), a second one or Enter hands them to the engine, which leaves the server and
// ends the process at once, as FiveM does.
#pragma once
#include <atomic>
#include <cstdint>

namespace gage::game::quit {

    enum class Kind { None, AltF4, Close, SysClose };

    constexpr unsigned kWmClose = 0x0010;       // WM_CLOSE
    constexpr unsigned kWmSysKeyDown = 0x0104;  // WM_SYSKEYDOWN
    constexpr unsigned kWmSysCommand = 0x0112;  // WM_SYSCOMMAND
    constexpr uintptr_t kVkF4 = 0x73;           // VK_F4
    constexpr uintptr_t kScClose = 0xF060;      // SC_CLOSE
    constexpr intptr_t kAltDown = 1 << 29;      // WM_SYSKEYDOWN lParam bit 29: the context code (Alt held)

    // Alt+F4 (WM_SYSKEYDOWN, VK_F4, Alt held), WM_CLOSE, or the title bar's X / the system menu's
    // Close (WM_SYSCOMMAND SC_CLOSE; the low 4 bits of wParam are the system's own).
    inline Kind Classify(unsigned msg, uintptr_t wp, intptr_t lp)
    {
        if (msg == kWmSysKeyDown && wp == kVkF4 && (lp & kAltDown))
            return Kind::AltF4;
        if (msg == kWmClose)
            return Kind::Close;
        if (msg == kWmSysCommand && (wp & 0xFFF0) == kScClose)
            return Kind::SysClose;
        return Kind::None;
    }

    // True for exactly one caller, ever (any thread): a second Alt+F4 while the first quit is
    // leaving the server must not disconnect again.
    class OnceGate
    {
    public:
        bool First() { return !taken_.exchange(true, std::memory_order_acq_rel); }
        bool Taken() const { return taken_.load(std::memory_order_acquire); }

    private:
        std::atomic<bool> taken_{false};
    };

}
