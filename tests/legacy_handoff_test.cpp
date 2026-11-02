// legacy_handoff_test — the launcher's one-time clean-up of the values older
// GAGE-MP launchers wrote into HKCU\Software\RAGE-MP (apps/launcher/legacy_handoff.cpp),
// run on a scratch key under HKCU\Software\GAGE-MP-Selftest, never on the real
// RAGE-MP key. Build target: legacy_handoff_test (selftest/). Exit 0 = pass.
//
// The point of every case is the same line: values are deleted only when the
// key's rage_path is inside the install dir, and only the values GAGE-MP wrote.

#include "../apps/launcher/legacy_handoff.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace gage;
using namespace gage::legacy;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got = "") {
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", got.c_str());
}

static const wchar_t* kScratchRoot = L"Software\\GAGE-MP-Selftest";
static const wchar_t* kKey = L"Software\\GAGE-MP-Selftest\\legacy_handoff";
static const std::wstring kInstall = L"C:\\Users\\Test\\AppData\\Local\\Programs\\GAGE-MP\\";
static const std::wstring kOurRagePath = L"C:\\Users\\Test\\AppData\\Local\\Programs\\GAGE-MP\\rage";

static void Wipe() { RegDeleteTreeW(HKEY_CURRENT_USER, kScratchRoot); }

static void Put(const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        std::printf("FAIL  cannot create the scratch key\n");
        std::exit(2);
    }
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

static bool KeyExists() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

static bool HasValue(const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExW(k, name, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static int CountWritten() {
    int n = 0;
    for (const wchar_t* const* v = WrittenValueNames(); *v; ++v) n += HasValue(*v) ? 1 : 0;
    return n;
}

static int WrittenTotal() {
    int n = 0;
    for (const wchar_t* const* v = WrittenValueNames(); *v; ++v) ++n;
    return n;
}

// What the old launcher left: every value it wrote, rage_path as given.
static void PutOldLauncherValues(const std::wstring& ragePath) {
    for (const wchar_t* const* v = WrittenValueNames(); *v; ++v) Put(*v, L"x");
    Put(L"rage_path", ragePath);
}

static std::string N(const std::wstring& s) { return Narrow(s); }

int main() {
    Wipe(); // a crashed earlier run must not leak into this one

    // ---- IsInside --------------------------------------------------------
    Check("inside: the rage folder of the install", IsInside(kOurRagePath, kInstall));
    Check("inside: the install dir itself", IsInside(L"C:\\Users\\Test\\AppData\\Local\\Programs\\GAGE-MP", kInstall));
    Check("inside: case, forward slashes and a trailing slash do not matter",
          IsInside(L"c:/USERS/test/appdata/local/programs/gage-mp/RAGE/", kInstall));
    Check("outside: a sibling that starts with the same letters",
          !IsInside(L"C:\\Users\\Test\\AppData\\Local\\Programs\\GAGE-MP2\\rage", kInstall));
    Check("outside: '..' climbs out of the install dir",
          !IsInside(L"C:\\Users\\Test\\AppData\\Local\\Programs\\GAGE-MP\\..\\Other\\rage", kInstall));
    Check("outside: another drive", !IsInside(L"D:\\Users\\Test\\AppData\\Local\\Programs\\GAGE-MP\\rage", kInstall));
    Check("outside: an empty path", !IsInside(L"", kInstall));
    Check("outside: an empty install dir", !IsInside(kOurRagePath, L""));
    Check("outside: a relative install dir", !IsInside(kOurRagePath, L".\\"));
    Check("outside: a drive-root install dir contains nothing", !IsInside(L"C:\\Games\\RAGEMP", L"C:\\"));

    // ---- MigrateHandoffKey ------------------------------------------------
    {   // A: no key at all
        Result r = MigrateHandoffKey(kKey, kInstall);
        Check("no key: NoKey", r.outcome == Outcome::NoKey, N(r.detail));
        Check("no key: nothing was created", !KeyExists());
    }
    {   // B: ours, plus a value somebody else put there
        PutOldLauncherValues(kOurRagePath);
        Put(L"someone_else", L"keep me");
        Check("setup B: all values present", CountWritten() == WrittenTotal(), std::to_string(CountWritten()));
        Result r = MigrateHandoffKey(kKey, kInstall);
        Check("ours + foreign value: Cleaned", r.outcome == Outcome::Cleaned, N(r.detail));
        Check("ours + foreign value: every written value deleted", r.deleted == WrittenTotal() && CountWritten() == 0,
              std::to_string(r.deleted));
        Check("ours + foreign value: the foreign value survives", HasValue(L"someone_else"));
        Check("ours + foreign value: the key is kept", KeyExists() && !r.keyRemoved);
        Wipe();
    }
    {   // C: ours only -> the emptied key goes too; a second call finds nothing
        PutOldLauncherValues(kOurRagePath);
        Result r = MigrateHandoffKey(kKey, kInstall);
        Check("ours only: Cleaned", r.outcome == Outcome::Cleaned, N(r.detail));
        Check("ours only: the emptied key was removed", r.keyRemoved && !KeyExists());
        Check("ours only: a second run finds no key", MigrateHandoffKey(kKey, kInstall).outcome == Outcome::NoKey);
        Wipe();
    }
    {   // D: a real RAGE-MP install: same value names, rage_path elsewhere
        PutOldLauncherValues(L"D:\\Games\\RAGEMP");
        Result r = MigrateHandoffKey(kKey, kInstall);
        Check("someone else's RAGE-MP: NotOurs", r.outcome == Outcome::NotOurs, N(r.detail));
        Check("someone else's RAGE-MP: nothing deleted", r.deleted == 0 && CountWritten() == WrittenTotal(),
              std::to_string(CountWritten()));
        Wipe();
    }
    {   // E: no rage_path: cannot prove it is ours
        PutOldLauncherValues(kOurRagePath);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, L"rage_path");
        Check("setup E: rage_path is gone", !HasValue(L"rage_path"));
        Result r = MigrateHandoffKey(kKey, kInstall);
        Check("no rage_path: NotOurs", r.outcome == Outcome::NotOurs, N(r.detail));
        Check("no rage_path: nothing deleted", r.deleted == 0 && CountWritten() == WrittenTotal() - 1,
              std::to_string(CountWritten()));
        Wipe();
    }
    {   // F: sibling folder with the same prefix
        PutOldLauncherValues(L"C:\\Users\\Test\\AppData\\Local\\Programs\\GAGE-MP2\\rage");
        Result r = MigrateHandoffKey(kKey, kInstall);
        Check("sibling folder GAGE-MP2: NotOurs, nothing deleted",
              r.outcome == Outcome::NotOurs && CountWritten() == WrittenTotal(), N(r.detail));
        Wipe();
    }
    {   // G: path spelled differently but still inside -> ours
        PutOldLauncherValues(L"c:/USERS/test/appdata/local/programs/gage-mp/rage/");
        Result r = MigrateHandoffKey(kKey, kInstall);
        Check("same folder, other spelling: Cleaned", r.outcome == Outcome::Cleaned && r.keyRemoved, N(r.detail));
        Wipe();
    }
    {   // H: the install dir is unusable: never delete on a guess
        PutOldLauncherValues(kOurRagePath);
        Result rel = MigrateHandoffKey(kKey, L".\\");
        Check("relative install dir: NotOurs, nothing deleted",
              rel.outcome == Outcome::NotOurs && CountWritten() == WrittenTotal(), N(rel.detail));
        Result root = MigrateHandoffKey(kKey, L"C:\\");
        Check("drive-root install dir: NotOurs, nothing deleted",
              root.outcome == Outcome::NotOurs && CountWritten() == WrittenTotal(), N(root.detail));
        Result empty = MigrateHandoffKey(kKey, L"");
        Check("empty install dir: NotOurs, nothing deleted",
              empty.outcome == Outcome::NotOurs && CountWritten() == WrittenTotal(), N(empty.detail));
        Wipe();
    }
    {   // I: only some of the values exist (an older launcher wrote fewer)
        Put(L"rage_path", kOurRagePath);
        Put(L"launch.ip", L"1.2.3.4");
        Result r = MigrateHandoffKey(kKey, kInstall);
        Check("partial set: Cleaned, 2 deleted, key removed",
              r.outcome == Outcome::Cleaned && r.deleted == 2 && r.keyRemoved, N(r.detail) + " deleted=" + std::to_string(r.deleted));
        Wipe();
    }
    {   // J: the list itself: exactly the twelve names, none of them a value another program would own by accident
        Check("the written-value list has 12 names", WrittenTotal() == 12, std::to_string(WrittenTotal()));
    }

    Wipe();
    const int expected = 32;
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
