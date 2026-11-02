// game_launch_test — the store detection and launch URLs of
// apps/launcher/game_launch.cpp, against fixture folders made in %TEMP%.
// Nothing is launched: only the pure helpers and the PlayGTAV.exe-missing
// error path run. Build target: game_launch_test (selftest/). Exit 0 = pass.

#include "../apps/launcher/game_launch.cpp"  // the TU itself, so its internals are reachable

#include <cstdio>

using namespace gage;
using namespace gage::launch;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::wstring& got = L"") {
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", Narrow(got).c_str());
}

static void WriteText(const std::wstring& path, const char* text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("FAIL  cannot write fixture %s\n", Narrow(path).c_str());
        std::exit(2);
    }
    DWORD n = 0;
    WriteFile(h, text, (DWORD)strlen(text), &n, 0);
    CloseHandle(h);
}

static void MakeDirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); ++i)
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), 0);
}

// Remove the fixture tree this test made (and only that: the root is our own
// unique folder under %TEMP%).
static void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(dir + n + L"\\");
            else DeleteFileW((dir + n).c_str());
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = std::wstring(tmp) + L"gage_launch_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";

    // Steam library with both editions' manifests
    std::wstring lib = root + L"SteamLibrary\\steamapps\\";
    std::wstring legacy = lib + L"common\\Grand Theft Auto V\\";
    std::wstring enhanced = lib + L"common\\Grand Theft Auto V Enhanced\\";
    MakeDirs(legacy);
    MakeDirs(enhanced);
    WriteText(lib + L"appmanifest_271590.acf",
              "\"AppState\"\n{\n\t\"appid\"\t\t\"271590\"\n\t\"name\"\t\t\"Grand Theft Auto V Legacy\"\n"
              "\t\"installdir\"\t\t\"Grand Theft Auto V\"\n}\n");
    WriteText(lib + L"appmanifest_3240220.acf",
              "\"AppState\"\n{\n\t\"appid\"\t\t\"3240220\"\n\t\"installdir\"\t\t\"Grand Theft Auto V Enhanced\"\n}\n");
    WriteText(legacy + L"GTA5.exe", "x");
    WriteText(legacy + L"steam_api64.dll", "x");

    // the manifest wins over the edition guess (enhanced=false here on purpose)
    std::wstring id = SteamAppId(enhanced, false);
    Check("steam appid from appmanifest (Enhanced folder)", id == L"3240220", id);
    id = SteamAppId(legacy, true);
    Check("steam appid from appmanifest (Legacy folder)", id == L"271590", id);
    std::wstring url = SteamRunUrl(legacy, false);
    Check("steam run url carries -nobattleye", url == L"steam://run/271590//-nobattleye/", url);
    std::wstring args = SteamApplaunchArgs(enhanced, false);
    Check("steam -applaunch args", args == L"-applaunch 3240220 -nobattleye", args);

    // BattlEye's stub, by shape (never the game images or unrelated *_BE.exe)
    bool beYes = IsBattlEyeImage(L"GTA5_BE.exe") && IsBattlEyeImage(L"gta5_enhanced_be.exe") &&
                 IsBattlEyeImage(L"GTAV_Enhanced_BE.exe");
    bool beNo = !IsBattlEyeImage(L"GTA5.exe") && !IsBattlEyeImage(L"GTA5_Enhanced.exe") &&
                !IsBattlEyeImage(L"BEService.exe") && !IsBattlEyeImage(L"Other_BE.exe") && !IsBattlEyeImage(L"_BE.exe");
    Check("battleye stub names matched", beYes);
    Check("game and unrelated names not matched", beNo);

    // no manifest names the folder: the edition's known id
    std::wstring orphan = root + L"Other\\steamapps\\common\\GTAV\\";
    MakeDirs(orphan);
    id = SteamAppId(orphan, true);
    Check("steam appid fallback, Enhanced", id == L"3240220", id);
    id = SteamAppId(orphan, false);
    Check("steam appid fallback, Legacy", id == L"271590", id);

    Check("store: steam_api64.dll => Steam", DetectStore(legacy + L"GTA5.exe") == Store::Steam);

    // Epic install
    std::wstring epic = root + L"Epic Games\\GTAV\\";
    MakeDirs(epic + L".egstore");
    WriteText(epic + L"GTA5.exe", "x");
    WriteText(epic + L".egstore\\ABCDEF.mancpn",
              "{\"FormatVersion\":0,\"AppName\":\"9d2d0eb64d5c44529cece33fe2a46482\","
              "\"CatalogNamespace\":\"0584d2013f0149a791e7b9bad0eec102\",\"CatalogItemId\":\"6e563a2c0f5f46e3b4e88b5f4ed50cca\"}");
    Check("store: .egstore => Epic", DetectStore(epic + L"GTA5.exe") == Store::Epic);
    url = EpicLaunchUrl(epic);
    Check("epic launch url", url == L"com.epicgames.launcher://apps/0584d2013f0149a791e7b9bad0eec102%3A"
                                   L"6e563a2c0f5f46e3b4e88b5f4ed50cca%3A9d2d0eb64d5c44529cece33fe2a46482"
                                   L"?action=launch&silent=true", url);

    // a manifest that would inject URL parts is refused
    std::wstring evil = root + L"Evil\\GTAV\\";
    MakeDirs(evil + L".egstore");
    WriteText(evil + L".egstore\\X.mancpn", "{\"AppName\":\"abc?action=x&y=1\"}");
    url = EpicLaunchUrl(evil);
    Check("epic url refuses non-plain AppName", url.empty(), url);

    // An Epic copy no record names (moved/copied install: .egstore holds only the binary
    // .manifest, no .mancpn, no Epic .item, nothing in gage_mp.cfg) still gets the authenticated
    // URI from the built-in GTA V Legacy catalog ids -- without it the launch fell back to a bare
    // PlayGTAV and Epic showed "UNABLE TO LAUNCH GAME" (installed 0.3.1, 2026-11-01).
    std::wstring moved = root + L"gta epic\\GTAV\\";
    MakeDirs(moved + L".egstore");
    WriteText(moved + L".egstore\\C85B16264777AA65975CA25146609D65.manifest", "binary");
    WriteText(moved + L"GTA5.exe", "x");
    url = EpicLaunchUrl(moved);
    Check("epic url for an unrecorded Epic copy (built-in Legacy ids)",
          url == L"com.epicgames.launcher://apps/0584d2013f0149a791e7b9bad0eec102%3A"
                 L"6e563a2c0f5f46e3b4e88b5f4ed50cca%3A9d2d0eb64d5c44529cece33fe2a46482"
                 L"?action=launch&silent=true", url);
    // ... but never for an Enhanced-only Epic folder (different product) or a non-Epic folder
    // (Rockstar / disk copies take the bare PlayGTAV ride, which has no Epic bounce).
    std::wstring movedEnh = root + L"gta epic enh\\GTAV\\";
    MakeDirs(movedEnh + L".egstore");
    WriteText(movedEnh + L"GTA5_Enhanced.exe", "x");
    url = EpicLaunchUrl(movedEnh);
    Check("no built-in epic url for an Enhanced-only folder", url.empty(), url);
    std::wstring plain = root + L"Plain\\GTAV\\";
    MakeDirs(plain);
    WriteText(plain + L"GTA5.exe", "x");
    url = EpicLaunchUrl(plain);
    Check("no epic url without .egstore", url.empty(), url);

    // A Steam copy with no steam_api64.dll but a PlayGTAV.exe (what sent a
    // Steam player through the Rockstar launcher): Steam's own records find
    // it, in a second library listed by libraryfolders.vdf.
    std::wstring steamRoot = root + L"Steam\\";
    std::wstring lib2 = root + L"Games Lib\\";
    std::wstring lib2Game = lib2 + L"steamapps\\common\\Grand Theft Auto V\\";
    MakeDirs(steamRoot + L"steamapps");
    MakeDirs(lib2Game);
    std::string lib2Esc;  // KeyValues escape: every backslash doubled
    for (char c : Narrow(lib2.substr(0, lib2.size() - 1))) {
        lib2Esc += c;
        if (c == '\\') lib2Esc += '\\';
    }
    std::string vdf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + lib2Esc +
                      "\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"271590\"\t\t\"1\"\n\t\t}\n\t}\n}\n";
    WriteText(steamRoot + L"steamapps\\libraryfolders.vdf", vdf.c_str());
    WriteText(lib2 + L"steamapps\\appmanifest_271590.acf",
              "\"AppState\"\n{\n\t\"appid\"\t\t\"271590\"\n\t\"installdir\"\t\t\"Grand Theft Auto V\"\n}\n");
    WriteText(lib2Game + L"GTA5.exe", "x");
    WriteText(lib2Game + L"PlayGTAV.exe", "x");
    std::vector<std::wstring> libs = SteamLibrariesOf(steamRoot);
    std::string app;
    std::wstring upper = lib2Game;
    CharUpperBuffW(&upper[0], (DWORD)upper.size());
    bool owned = SteamOwnsIn(CanonicalDir(upper), libs, &app);
    Check("steam library from libraryfolders.vdf owns the folder (any case)", libs.size() == 2 && owned && app == "271590",
          Widen(app));
    Check("a folder in no steam library is not steam's", !SteamOwnsIn(CanonicalDir(orphan), libs, &app));

    // Epic's install list, written with forward slashes and other case
    std::string list = "{\"InstallationList\":[{\"InstallLocation\":\"";
    for (char c : Narrow(legacy.substr(0, legacy.size() - 1)) + "\\OTHER") {
        list += c;
        if (c == '\\') list += '\\';  // the launcher's own JSON escaping
    }
    list += "\",\"AppName\":\"x\"},{\"InstallLocation\":\"";
    for (char c : Narrow(epic.substr(0, epic.size() - 1))) list += (c == '\\') ? '/' : (char)toupper((unsigned char)c);
    list += "\",\"AppName\":\"9d2d\"}]}";
    Check("epic install list owns the folder", EpicListOwns(CanonicalDir(epic), list));
    Check("epic install list does not own another folder", !EpicListOwns(CanonicalDir(legacy), list));

    // Rockstar install without PlayGTAV.exe: refused with a reason, nothing run
    std::wstring rs = root + L"Rockstar Games\\Grand Theft Auto V\\";
    MakeDirs(rs);
    WriteText(rs + L"GTA5.exe", "x");
    Check("store: plain folder => Rockstar", DetectStore(rs + L"GTA5.exe") == Store::Rockstar);
    std::wstring err;
    bool started = StartThroughStore(rs + L"GTA5.exe", Store::Rockstar, &err);
    Check("rockstar without PlayGTAV.exe is refused", !started && err.find(L"PlayGTAV.exe is missing") == 0, err);

    // Disk / repack install: a stray steam_api64.dll next to PlayGTAV.exe, in
    // no store's records (what routed a non-Steam copy through Steam). The
    // bootstrapper is the stronger signal and wins: Rockstar, not Steam.
    std::wstring disk = root + L"Disk\\Grand Theft Auto V Legacy\\";
    MakeDirs(disk);
    WriteText(disk + L"GTA5.exe", "x");
    WriteText(disk + L"steam_api64.dll", "x");
    WriteText(disk + L"PlayGTAV.exe", "x");
    Check("store: PlayGTAV.exe beats stray steam_api64.dll => Rockstar",
          DetectStore(disk + L"GTA5.exe") == Store::Rockstar);

    // Orphaned Epic copy: a complete install (.egstore ships inside the files,
    // plus PlayGTAV.exe) that Epic's own library does NOT list -- moved or copied
    // in, or its manifest lost. The epicgames:// URL is a dead end (Epic thinks it
    // is not installed: "the store did not answer"), so the bootstrapper wins and
    // DetectStore must pick Rockstar, not Epic. (Fails on the old order, where a
    // bare .egstore returned Epic before PlayGTAV.exe was ever considered.)
    std::wstring orphanEpic = root + L"Orphan Epic\\GTAV\\";
    MakeDirs(orphanEpic + L".egstore");
    WriteText(orphanEpic + L"GTA5.exe", "x");
    WriteText(orphanEpic + L"PlayGTAV.exe", "x");
    WriteText(orphanEpic + L".egstore\\Y.mancpn",
              "{\"AppName\":\"abc\",\"CatalogNamespace\":\"def\",\"CatalogItemId\":\"ghi\"}");
    Check("store: orphaned Epic copy (.egstore, not in Epic list) + PlayGTAV.exe => Rockstar",
          DetectStore(orphanEpic + L"GTA5.exe") == Store::Rockstar);

    // Installed-game search: Steam's manifests in every library (Legacy before
    // Enhanced), Epic's install list, the Legacy preference across stores.
    WriteText(enhanced + L"GTA5_Enhanced.exe", "x");
    std::wstring found = SteamGameIn({ root + L"SteamLibrary" });
    Check("steam search: Legacy before Enhanced in one library", found == legacy + L"GTA5.exe", found);
    found = SteamGameIn(libs);
    Check("steam search: game in a second library from libraryfolders.vdf", found == lib2Game + L"GTA5.exe", found);
    found = SteamGameIn({ steamRoot.substr(0, steamRoot.size() - 1) });
    Check("steam search: a library without a GTA V manifest finds nothing", found.empty(), found);
    std::vector<std::wstring> epics = EpicGamesIn(list);
    Check("epic search: only listed folders that hold the game",
          epics.size() == 1 && CanonicalDir(DirOf(epics[0])) == CanonicalDir(epic), epics.empty() ? L"" : epics[0]);
    std::wstring pick = PreferLegacy({ L"", enhanced + L"GTA5_Enhanced.exe", legacy + L"GTA5.exe" });
    Check("search prefers Legacy GTA5.exe over Enhanced", pick == legacy + L"GTA5.exe", pick);
    pick = PreferLegacy({ L"", enhanced + L"GTA5_Enhanced.exe" });
    Check("search falls back to Enhanced", pick == enhanced + L"GTA5_Enhanced.exe", pick);
    Check("search with nothing found is empty", PreferLegacy({ L"", L"" }).empty());

    // InjectShimIntoLaunchers targets ONLY the Rockstar Games Launcher's own
    // Launcher.exe, by full image path -- so the redirect shim reaches the
    // process that actually spawns GTA5_BE.exe and nothing else.
    Check("rgl launcher path (Program Files)",
          IsRockstarLauncherPath(L"H:\\Program Files\\Rockstar Games\\Launcher\\Launcher.exe"));
    Check("rgl launcher path (any case)",
          IsRockstarLauncherPath(L"c:\\program files\\ROCKSTAR GAMES\\launcher\\LAUNCHER.EXE"));
    Check("not rgl: the Epic launcher",
          !IsRockstarLauncherPath(L"C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win64\\EpicGamesLauncher.exe"));
    Check("not rgl: a bare Launcher.exe elsewhere",
          !IsRockstarLauncherPath(L"D:\\Games\\SomeGame\\Launcher.exe"));
    Check("not rgl: empty path", !IsRockstarLauncherPath(L""));

    RemoveTree(root);

    const int expected = 35;
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
