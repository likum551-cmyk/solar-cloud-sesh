// ambient_test: the pure decision logic of apps/engine/ambient.h (T5, no ambient NPCs / traffic): when the
// suppression runs (no dependence on the connection), the exemption list, the sweep planner, the circuit
// breaker, the counters and the `engine: ambient` line, plus a source-order guard on engine.cpp. The checks
// live in tests/t5_ambient_tests.h (the same header runs the World-wrapper checks inside gage_mp_test).
// Build target: ambient_test (selftest/, never shipped). Exit 0 = pass.

#include "t5_ambient_tests.h"

#include <cstdio>

#ifndef T5_REPO_DIR
#define T5_REPO_DIR "."
#endif

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got)
{
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", got.c_str());
}

int main()
{
    const int ran = t5::RunPure(Check, T5_REPO_DIR);
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (ran != t5::kPureChecks || g_run != t5::kPureChecks)
    {
        std::printf("FAIL  ran %d checks (reported %d), expected %d\n", g_run, ran, t5::kPureChecks);
        return 1;
    }
    return g_failed ? 1 : 0;
}
