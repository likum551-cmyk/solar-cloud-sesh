// launch_shim_test — two local proofs for gage_launch_shim before any live run
// (project rule: a hook change gets local proofs first).
//
//   Proof 1  ComputeRedirect: the pure redirect decision + command rewrite,
//            across the cases PlayGTAV can present (appName set, command-line
//            only, case-insensitive, and the no-redirect cases).
//   Proof 2  The IAT hook actually intercepts a real CreateProcessW call in
//            this process and redirects it: a launch of a (nonexistent)
//            GTA5_BE.exe is rewritten to cmd.exe, which runs and exits 42; a
//            non-matching launch is passed straight through (and fails on the
//            missing file). This exercises HookIAT + the thunk + ComputeRedirect
//            end to end, without GTA.
//
// Includes the shim TU directly (GAGE_SHIM_TEST drops its DllMain), the same
// way the other GAGE tests include the app source they cover.

#define GAGE_SHIM_TEST
#include "../apps/launcher-shim/gage_launch_shim.cpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace gage_shim;

static int g_fails = 0;
#define CHECK(cond, msg)                                       \
    do {                                                       \
        if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_fails; } \
        else         { printf("ok   %s\n", (msg)); }           \
    } while (0)

static std::wstring SystemCmd() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(sys) + L"\\cmd.exe";
}

int main() {
    g_logOn = false;  // no gage_shim.log from the test

    // ---- Proof 1: ComputeRedirect ----------------------------------------
    {
        std::wstring a, c;

        CHECK(ComputeRedirect(L"C:\\gta\\GTA5_BE.exe", L"\"C:\\gta\\GTA5_BE.exe\" -foo bar",
                              L"GTA5_BE.exe", L"C:\\gta\\GTA5.exe", a, c),
              "redirect when lpApplicationName is the BattlEye wrapper");
        CHECK(a == L"C:\\gta\\GTA5.exe", "outApp is the redirect target");
        CHECK(c == L"\"C:\\gta\\GTA5.exe\" -foo bar", "arguments preserved after redirect");

        // The real Rockstar Games Launcher command line: lpApplicationName set,
        // a quoted full path in the command line, and the long RGL/Epic arg list
        // (response files, -fromRGL). Every argument must survive the rewrite.
        a.clear(); c.clear();
        CHECK(ComputeRedirect(
                  L"C:\\gta epic\\GTAV\\GTA5_BE.exe",
                  L"\"C:\\gta epic\\GTAV\\GTA5_BE.exe\" -skipPatcherCheck -epicAppId=gta5 @args.txt "
                  L"-enableCrashpad -useEpic @commandline.txt -fromRGL -rglLanguage=en-US",
                  L"GTA5_BE.exe", L"C:\\gta epic\\GTAV\\GTA5.exe", a, c),
              "redirect the real RGL GTA5_BE.exe launch");
        CHECK(a == L"C:\\gta epic\\GTAV\\GTA5.exe", "RGL redirect target is GTA5.exe");
        CHECK(c == L"\"C:\\gta epic\\GTAV\\GTA5.exe\" -skipPatcherCheck -epicAppId=gta5 @args.txt "
                   L"-enableCrashpad -useEpic @commandline.txt -fromRGL -rglLanguage=en-US",
              "all RGL/Epic args (response files, -fromRGL) preserved");

        a.clear(); c.clear();
        CHECK(ComputeRedirect(nullptr, L"\"C:\\x y\\GTA5_BE.exe\" -scOffline",
                              L"GTA5_BE.exe", L"C:\\x y\\GTA5.exe", a, c),
              "redirect from a quoted command line (no appName)");
        CHECK(c == L"\"C:\\x y\\GTA5.exe\" -scOffline", "command-line-only args preserved");

        a.clear(); c.clear();
        CHECK(ComputeRedirect(L"C:\\x\\gta5_be.EXE", nullptr,
                              L"GTA5_BE.exe", L"C:\\x\\GTA5.exe", a, c),
              "base-name match is case-insensitive");
        CHECK(a == L"C:\\x\\GTA5.exe" && c == L"\"C:\\x\\GTA5.exe\"",
              "no args -> just the quoted target");

        a.clear(); c.clear();
        CHECK(!ComputeRedirect(L"C:\\x\\GTA5.exe", nullptr, L"GTA5_BE.exe", L"C:\\x\\GTA5.exe", a, c),
              "no redirect for the game itself (never double-redirect)");
        CHECK(!ComputeRedirect(L"C:\\x\\PlayGTAV.exe", nullptr, L"GTA5_BE.exe", L"C:\\x\\GTA5.exe", a, c),
              "no redirect for PlayGTAV");
        CHECK(!ComputeRedirect(nullptr, nullptr, L"GTA5_BE.exe", L"C:\\x\\GTA5.exe", a, c),
              "no redirect when there is no exe to inspect");
        CHECK(!ComputeRedirect(L"C:\\x\\GTA5_BE.exe", nullptr, L"GTA5_BE.exe", L"", a, c),
              "no redirect when the target is empty");

        // Shape matching — the new default (shim_redirect_from empty). The BattlEye
        // wrapper is caught by its GTA*_BE.exe shape, so BOTH editions redirect with
        // no configured from-name, while off-shape images are left alone.
        a.clear(); c.clear();
        CHECK(ComputeRedirect(L"C:\\gta\\GTA5_Enhanced_BE.exe", nullptr, L"",
                              L"C:\\gta\\GTA5_Enhanced.exe", a, c),
              "redirect the Enhanced wrapper by shape with no from-name");
        CHECK(a == L"C:\\gta\\GTA5_Enhanced.exe" && c == L"\"C:\\gta\\GTA5_Enhanced.exe\"",
              "Enhanced redirect target is GTA5_Enhanced.exe");
        a.clear(); c.clear();
        CHECK(ComputeRedirect(L"C:\\gta\\GTAV_Enhanced_BE.exe", nullptr, L"",
                              L"C:\\gta\\GTA5_Enhanced.exe", a, c),
              "redirect the GTAV_Enhanced_BE.exe spelling by shape");
        a.clear(); c.clear();
        CHECK(ComputeRedirect(nullptr, L"\"C:\\x y\\GTA5_BE.exe\" -scOffline", L"",
                              L"C:\\x y\\GTA5.exe", a, c),
              "redirect the Legacy wrapper by shape with no from-name");
        CHECK(c == L"\"C:\\x y\\GTA5.exe\" -scOffline", "args preserved on a shape redirect");
        a.clear(); c.clear();
        CHECK(!ComputeRedirect(L"C:\\x\\GTA5_Enhanced.exe", nullptr, L"",
                               L"C:\\x\\GTA5_Enhanced.exe", a, c),
              "no redirect for the Enhanced game exe itself (not a _BE wrapper)");
        CHECK(!ComputeRedirect(L"C:\\x\\RockstarService.exe", nullptr, L"",
                               L"C:\\x\\GTA5.exe", a, c),
              "no redirect for an off-shape sibling process with no from-name");

        // An explicit off-shape override still works (shim_redirect_from set).
        a.clear(); c.clear();
        CHECK(ComputeRedirect(L"C:\\x\\Weird_Wrapper.exe", nullptr, L"Weird_Wrapper.exe",
                              L"C:\\x\\GTA5.exe", a, c),
              "explicit shim_redirect_from override redirects an off-shape name");
    }

    // ---- Proof 2: the IAT hook redirects a real CreateProcessW call ------
    {
        g_configLoaded = true;         // bypass the registry
        g_fromBase = L"";              // shape-only, the new default (shim catches GTA*_BE.exe)
        g_toPath   = SystemCmd();
        g_logOn    = false;

        int n = InstallProcessCreationHooks(GetModuleHandleW(nullptr));
        CHECK(n >= 1, "InstallProcessCreationHooks patched this process's IAT");
        CHECK(g_realCPW != nullptr, "saved the original CreateProcessW");

        // Control: a non-matching exe is passed straight through to the real
        // CreateProcessW, which fails because the file does not exist.
        {
            std::wstring cmd = L"\"C:\\gage_nope\\NOTGTA.exe\" /c exit 7";
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(L'\0');
            STARTUPINFOW si{}; si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CHECK(!ok, "non-matching launch passes through and fails on the missing file");
            if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        }

        // Redirect: launching a (nonexistent) GTA5_BE.exe is rewritten to
        // cmd.exe, which runs and exits 42.
        {
            std::wstring cmd = L"\"C:\\gage_nope\\GTA5_BE.exe\" /c exit 42";
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(L'\0');
            STARTUPINFOW si{}; si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CHECK(ok, "matching launch is redirected and starts");
            if (ok) {
                WaitForSingleObject(pi.hProcess, 10000);
                DWORD code = 1234;
                GetExitCodeProcess(pi.hProcess, &code);
                CHECK(code == 42, "redirected process actually ran (cmd.exe exited 42)");
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }

    printf(g_fails ? "\nRESULT: %d FAIL\n" : "\nRESULT: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
