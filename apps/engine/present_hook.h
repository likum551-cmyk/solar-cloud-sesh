// present_hook: the per-frame hook on IDXGISwapChain::Present, built to coexist with every other overlay.
//
// Why this exists (2026-11-03): RivaTuner Statistics Server, MSI Afterburner, Steam's and Discord's overlays,
// NVIDIA's capture hook and OBS's game capture all patch the first bytes of dxgi's Present function. Our old
// inline E9 patch landed on the same bytes: it copied their E9 into its trampoline, wrote its own E9 over it,
// and the other hooker restored its own bytes (restore, call the original, re-patch). One frame ran, then the
// tick was gone for good: no loading screen, no overlay, no join (gage.log "no frame in the last 10 s (1 in
// total)"). Turning RTSS off fixed it.
//
// The fix is to not fight over the code at all. dxgi's swap chain class has ONE vtable, shared by every swap
// chain of that class, and slot 8 is Present. We replace that slot (a pointer in dxgi's data, which no
// code-patching hooker touches) with PresentDetour, which runs the tick and then calls the function the slot
// held before: the real Present, or whoever had hooked the slot ahead of us. Inline hookers keep working
// untouched underneath.
//
// Supervision (Poll, run by a small watchdog thread; tests call it directly):
//   * the slot no longer holds our detour and frames stopped: someone restored or replaced it and did not chain
//     to us. Take it again (at most Options::maxRetakes times), chaining to whatever is there now;
//   * the slot is ours and no frame ever arrived (Options::neverTickedMs) or frames stopped (stalledMs): the
//     game's swap chain does not go through this vtable (a wrapper object). Install the last-resort inline hook
//     (the caller's installInline), once. Both paths then run, and a per-thread guard makes the tick run exactly
//     once per Present.
// Every decision is logged with the module that owns the foreign hook (DescribeEntry) and the overlay modules
// loaded (LoadedOverlayModules), so a stalled log names the culprit.
#pragma once

#include <cstdint>
#include <string>

namespace gage::presenthook {

struct Callbacks {
    void (*tick)() = nullptr;                      // once per Present, on the presenting thread, ahead of the game's Present
    // Optional last resort: inline-hook the function at `fn`. Its detour thunk must store the swap chain (RCX) into
    // SwapChainStorage() and call OnInlineEntry() once per entry. Returns false when it could not hook.
    bool (*installInline)(uint8_t* fn) = nullptr;
    void (*removeInline)() = nullptr;              // undo installInline
};

struct Options {
    bool     watchdog = true;           // run the supervision thread (tests turn it off and call Poll themselves)
    uint64_t pollMs = 500;
    uint64_t neverTickedMs = 45000;     // vtable hook installed, not one frame seen: the game's chain bypasses it
    uint64_t stalledMs = 8000;          // frames were seen, then none for this long
    int      maxRetakes = 8;            // re-take attempts before only the inline fallback is left
};
Options& DefaultOptions();              // what Install(...) without Options uses

// Take over *slot (a pointer-sized vtable entry: IDXGISwapChain::Present is slot 8 of the swap chain vtable).
// `inlineTarget` is the Present function the caller found (vtable[8] of its dummy swap chain), the target for the
// last-resort inline hook; may be null. False when the slot is null/unwritable or a hook is already installed.
bool Install(void** slot, void* inlineTarget, const Callbacks& cb);
bool Install(void** slot, void* inlineTarget, const Callbacks& cb, const Options& opt);

// Stop the watchdog, undo the inline fallback, and put the slot back to what it held before our (last) take, but
// only if it still holds our detour (somebody chained to us otherwise; their code now calls ours, which stays).
void Uninstall();

bool  Active();                 // the vtable hook is installed
bool  InlineActive();           // the last-resort inline hook is installed too
void* SwapChain();              // the IDXGISwapChain* of the latest Present, or null before the first
void** SwapChainStorage();      // where an inline thunk stores RCX (the same variable SwapChain() reads)

// The inline fallback's per-entry callback (a `void (*)()` for the inline thunk's callback slot): runs the tick
// unless PresentDetour already did on this thread for this Present.
void OnInlineEntry();

struct Stats {
    uint64_t ticks = 0;         // Presents that ran the tick
    uint64_t retakes = 0;       // slot re-takes by Poll
    uint64_t chainLoops = 0;    // PresentDetour re-entered on the same thread (a hooker chained back to us)
    uint64_t lastTickMs = 0;    // GetTickCount64 of the latest tick, 0 = none
    bool     inlineInstalled = false;
    bool     slotIsOurs = false;
};
Stats GetStats();

enum class PollResult {
    NotInstalled,
    Healthy,            // frames are arriving (or the quiet period is still short)
    Retaken,            // the slot was replaced and quiet: taken again
    InlineInstalled,    // the inline fallback went in
    Stalled,            // quiet, nothing left to try (already logged)
};
// One supervision step. `nowMs` is GetTickCount64() (tests pass a shifted clock).
PollResult Poll(uint64_t nowMs);

// ---- diagnostics (also used by the tests) ----------------------------------
// "dxgi.dll+0x19530", or "unbacked memory 0x..." for an address outside every module.
std::string DescribeAddress(const void* p);
// Follows up to three jumps (E9, EB, FF 25, 48 B8 ... FF E0) from the first bytes at `fn` and describes the chain,
// e.g. "dxgi.dll+0x19530 -> jmp -> RTSSHooks64.dll+0x1a2b0". `foreign` is true when the entry jumps out of its own
// module, i.e. another hooker has inline-patched it.
struct EntryInfo { std::string text; bool foreign = false; };
EntryInfo DescribeEntry(const void* fn);
// Overlay / capture / injector modules loaded in the process ("RTSSHooks64.dll, GameOverlayRenderer64.dll"), or "".
std::string LoadedOverlayModules();

} // namespace gage::presenthook
