// crash_filter_test -- proves gageinjector's vectored exception handler declines
// benign, continuable exceptions WITHOUT poisoning its re-entrancy guard.
//
// The old handler took the one-shot `owner` guard on every entry and released it
// only by terminating. A benign exception (which returns EXCEPTION_CONTINUE_SEARCH)
// therefore left `owner` stuck, so the NEXT exception on that thread looked like a
// re-entrant dump fault and the game was killed with the benign code. GTA raises
// DBG_PRINTEXCEPTION_C (0x40010006) via OutputDebugString constantly during
// startup, and a VEH sees it before OutputDebugString's own handler -- so two
// debug prints on the main thread killed the game ~1 s into the first
// BattlEye-free boot (exit code 0x40010006, no crash log).
//
// Build target: crash_filter_test (selftest/, never shipped).

#include "../apps/injector/gageinjector.cpp"  // the TU itself, so CrashFilter is reachable

#include <cstdio>

static int g_run = 0, g_fail = 0;
static void Check(const char* name, bool ok) {
    ++g_run;
    if (!ok) ++g_fail;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", name);
}

int main() {
    // The code the crashed game carried, its wide-string sibling, and C++ throws:
    // all benign/continuable, none of them ours to turn fatal.
    Check("DBG_PRINTEXCEPTION_C (0x40010006) is not fatal", !IsFatalExceptionCode(0x40010006));
    Check("DBG_PRINTEXCEPTION_WIDE_C (0x4001000A) is not fatal", !IsFatalExceptionCode(0x4001000A));
    Check("C++ exception (0xE06D7363) is not fatal", !IsFatalExceptionCode(0xE06D7363));
    // Real faults stay fatal -- the handler must still catch and dump these.
    Check("access violation (0xC0000005) is fatal", IsFatalExceptionCode(0xC0000005));
    Check("stack overflow (0xC00000FD) is fatal", IsFatalExceptionCode(0xC00000FD));
    Check("breakpoint (0x80000003) is fatal", IsFatalExceptionCode(0x80000003));

    // The exact repro: deliver a benign exception to CrashFilter repeatedly on
    // this one thread. The old guard bug would TerminateProcess on the 2nd call,
    // so a buggy binary would be killed here and the lines below would never run.
    // The fix must return EXCEPTION_CONTINUE_SEARCH every time and never take the
    // guard.
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0x40010006;  // DBG_PRINTEXCEPTION_C, as OutputDebugString raises
    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;
    ep.ContextRecord = nullptr;      // benign path must never dereference it
    bool allContinue = true;
    for (int i = 0; i < 5; ++i)
        if (CrashFilter(&ep) != EXCEPTION_CONTINUE_SEARCH) allContinue = false;
    Check("CrashFilter declined 5 benign exceptions (process alive, guard intact)", allContinue);

    std::printf(g_fail ? "\nRESULT: %d FAIL\n" : "\nRESULT: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
