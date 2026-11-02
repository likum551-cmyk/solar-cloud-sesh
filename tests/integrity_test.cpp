// integrity_test — the launcher's own check of its injection chain, driven
// directly (no launcher window). Verifies that every DLL the launcher loads
// into the game (gen/chain_hashes.h) is present in a directory and hashes to
// its baked-in SHA-256. Build target: integrity_test (selftest/, never shipped).
//
//   integrity_test [dir]
//
// `dir` defaults to the directory containing this exe. Exit 0 = chain is
// intact, 1 = a DLL is missing or its hash differs, 2 = usage error.

#include "../apps/launcher/integrity.cpp"  // the TU itself, so VerifyChain + Sha256File are reachable

#include <windows.h>
#include <cstdio>
#include <cwchar>

using namespace gage;
using namespace gage::integrity;

int wmain(int argc, wchar_t** argv)
{
    // Locate the exe's own directory (selftest/), unless a dir was passed.
    std::wstring dir;
    if (argc >= 2)
        dir = argv[1];
    else
    {
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
        {
            std::printf("FAIL  GetModuleFileNameW (%lu)\n", (unsigned long)GetLastError());
            return 2;
        }
        const std::wstring path(exe);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            std::printf("FAIL  no directory component in %s\n", Narrow(path).c_str());
            return 2;
        }
        dir = path.substr(0, slash + 1);
    }
    if (dir.empty() || (dir.back() != L'\\' && dir.back() != L'/'))
        dir += L'\\';

    std::printf("integrity: verifying %d DLL(s) in %s\n", kGageChainCount, Narrow(dir).c_str());

    // Per-file detail (the launcher only gets the first bad file; the test prints all).
    int bad = 0;
    for (int i = 0; i < kGageChainCount; ++i)
    {
        const auto& f = kGageChain[i];
        const std::string got = Sha256File(dir + std::wstring(f.name));
        const bool ok = got == f.sha256;
        if (!ok) ++bad;
        std::printf("  %s  %-20s %s\n",
            ok ? "ok  " : "FAIL",
            Narrow(std::wstring(f.name)).c_str(),
            got.empty() ? "(missing)" : got.c_str());
    }

    std::wstring firstBad;
    const bool chain = VerifyChain(dir, &firstBad);
    if (!chain)
        std::printf("integrity: first bad: %s\n", Narrow(firstBad).c_str());

    std::printf("%s\n", chain ? "integrity: PASS" : "integrity: FAIL");
    return chain ? 0 : 1;
}
