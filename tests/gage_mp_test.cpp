// gage_mp_test — the in-game patches of apps/engine/gage_mp.cpp, run on
// this test exe's own image (the patch scans the main module, as it does in
// the game). Build target: gage_mp_test (selftest/). Exit 0 = pass.

#include "../apps/engine/gage_mp.cpp"  // the TU itself, so its internals are reachable

#include "../apps/engine/quit_gate.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

// What the game's image holds: the logo movie path as a whole string, and a
// longer string that only starts the same (must be left alone). Written as
// initialisers of writable globals so they sit in this exe's image; the
// test reads them through volatile pointers so nothing is constant-folded.
char g_logo[32] = "platform:/movies/rockstar_logos";
char g_longer[40] = "platform:/movies/rockstar_logos_extra";

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const char* got = "") {
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, *got ? "  got: " : "", got);
}

static std::string Read(const char* p, size_t n) {
    const volatile char* v = p;
    std::string s;
    for (size_t i = 0; i < n && v[i]; ++i) s += v[i];
    return s;
}

int main() {
    int patched = SkipLogoMovie(GetModuleHandleW(nullptr));
    std::string logo = Read(g_logo, sizeof(g_logo));
    std::string longer = Read(g_longer, sizeof(g_longer));
    Check("logo path replaced", logo == "./nonexistingfilenonexistingfil", logo.c_str());
    // (compared with a literal that has no NUL after "logos", so the scan never matches it)
    Check("longer string left alone", longer == "platform:/movies/rockstar_logos_extra", longer.c_str());
    Check("exactly one copy patched", patched == 1, std::to_string(patched).c_str());
    // a second pass finds nothing left
    Check("second pass patches nothing", SkipLogoMovie(GetModuleHandleW(nullptr)) == 0);

    // Alt+F4 / close (apps/engine/quit_gate.h): the hand-written message ids equal <windows.h>'s,
    // and only the quit messages classify.
    {
        namespace q = gage::game::quit;
        Check("quit ids match windows.h", q::kWmClose == WM_CLOSE && q::kWmSysKeyDown == WM_SYSKEYDOWN &&
              q::kWmSysCommand == WM_SYSCOMMAND && q::kVkF4 == VK_F4 && q::kScClose == SC_CLOSE);
        const intptr_t alt = (intptr_t)1 << 29;
        Check("Alt+F4 is a quit", q::Classify(WM_SYSKEYDOWN, VK_F4, alt | 1) == q::Kind::AltF4);
        Check("F4 sys key without Alt is not", q::Classify(WM_SYSKEYDOWN, VK_F4, 1) == q::Kind::None);
        Check("plain F4 is not", q::Classify(WM_KEYDOWN, VK_F4, 1) == q::Kind::None);
        Check("Alt+F10 is not", q::Classify(WM_SYSKEYDOWN, VK_F10, alt | 1) == q::Kind::None);
        Check("WM_CLOSE is a quit", q::Classify(WM_CLOSE, 0, 0) == q::Kind::Close);
        Check("title-bar X (SC_CLOSE, system low bits) is a quit",
              q::Classify(WM_SYSCOMMAND, SC_CLOSE | 2, 0) == q::Kind::SysClose);
        Check("SC_MINIMIZE is not", q::Classify(WM_SYSCOMMAND, SC_MINIMIZE, 0) == q::Kind::None);

        // The once gate under a race: 8 threads released together, exactly one gets through.
        q::OnceGate gate;
        std::atomic<bool> go{false};
        std::atomic<int> winners{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i)
            ts.emplace_back([&] {
                while (!go.load()) {}
                if (gate.First()) winners.fetch_add(1);
            });
        Check("gate not taken before any caller", !gate.Taken());
        go.store(true);
        for (std::thread& t : ts) t.join();
        Check("once gate: exactly one of 8 racing callers", winners.load() == 1,
              std::to_string(winners.load()).c_str());
        Check("once gate: a later caller is refused", !gate.First() && gate.Taken());
    }

    // Sync plan W0 (apps/engine/hook.h): the 27 new World wrappers (+ 7 of the 0.4.3 fixes, W8). This exe has no game, so the native
    // table is never resolved and every call must come back false without touching its out parameters.
    // Calling each one here also proves every declaration has a definition linked in (an undefined
    // member function that nothing calls links fine).
    {
        game::World w;
        Check("W0: native table is not resolved in the test exe (false below means 'cannot call')",
              !w.HaveNatives());
        const int32_t e = 1;  // non-zero: past the handle-0 guards
        const char* const s = "x";
        int wrappers = 0, truthy = 0;
        bool lightsOn = true, highbeamsOn = true;
        float ratio = 1.5f;
        int roof = 7;
#define W0_CALL(expr) do { ++wrappers; if (expr) ++truthy; } while (0)
        W0_CALL(w.IsPedReloading(e));
        W0_CALL(w.TaskReloadWeapon(e, true));
        W0_CALL(w.GetPedStealthMovement(e));
        W0_CALL(w.SetPedStealthMovement(e, true, s));
        W0_CALL(w.IsPedSwimming(e));
        W0_CALL(w.IsPedInCover(e, false));
        W0_CALL(w.TaskPutPedDirectlyIntoCover(e, 1.f, 2.f, 3.f, -1, true, 0.f, false, false, 0, true));
        W0_CALL(w.IsPedPerformingMeleeAction(e));
        W0_CALL(w.SetPedMoveRateOverride(e, 1.f));
        W0_CALL(w.GetVehicleLightsState(e, lightsOn, highbeamsOn));
        W0_CALL(w.SetVehicleFullbeam(e, true));
        W0_CALL(w.SetVehicleInteriorLight(e, true));
        W0_CALL(w.SetVehicleIndicatorLights(e, 1, true));
        W0_CALL(w.GetVehicleDoorAngleRatio(e, 0, ratio));
        W0_CALL(w.SetVehicleDoorOpen(e, 0, false, true));
        W0_CALL(w.GetConvertibleRoofState(e, roof));
        W0_CALL(w.RaiseConvertibleRoof(e, true));
        W0_CALL(w.LowerConvertibleRoof(e, true));
        W0_CALL(w.TaskDriveBy(e, e, 0, 1.f, 2.f, 3.f, 50.f, 50, false, 0u));
        W0_CALL(w.SetDrivebyTaskTarget(e, e, 0, 1.f, 2.f, 3.f));
        W0_CALL(w.TaskVehicleAimAtCoord(e, 1.f, 2.f, 3.f));
        W0_CALL(w.ClearDrivebyTaskUnderneathDrivingTask(e));
        W0_CALL(w.GetIsTaskActive(e, 0));
        W0_CALL(w.RequestNamedPtfxAsset(s));
        W0_CALL(w.HasNamedPtfxAssetLoaded(s));
        W0_CALL(w.UseParticleFxAsset(s));
        W0_CALL(w.StartParticleFxNonLoopedOnEntity(s, e, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, false, false, false));
        // 0.4.3 fixes (W8): vault / climb, the AI damage modifiers, the takedown diagnostic.
        W0_CALL(w.IsPedVaulting(e));
        W0_CALL(w.TaskClimb(e, false));
        W0_CALL(w.SetAiWeaponDamageModifier(0.0f));
        W0_CALL(w.SetAiMeleeWeaponDamageModifier(0.0f));
        W0_CALL(w.ResetAiWeaponDamageModifier());
        W0_CALL(w.ResetAiMeleeWeaponDamageModifier());
        W0_CALL(w.WasPedKilledByTakedown(e));
        W0_CALL(w.ModifyVehicleTopSpeed(e, 100.f));
        W0_CALL(w.SetVehicleCheatPowerIncrease(e, 4.f));
        W0_CALL(w.SetVehicleMaxSpeed(e, 138.9f));
        W0_CALL(w.GetVehicleEstimatedMaxSpeed(e));
#undef W0_CALL
        Check("W0: all 38 wrappers link and return false without natives", wrappers == 38 && truthy == 0,
              (std::to_string(wrappers) + " called, " + std::to_string(truthy) + " true").c_str());
        Check("W0: failed calls leave their out parameters untouched",
              lightsOn && highbeamsOn && ratio == 1.5f && roof == 7);
    }

    const int expected = 18;
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
