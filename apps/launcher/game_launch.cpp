// game_launch.cpp — start GTA V through its store, find the running game.
// See game_launch.h for why the game is never started as a bare GTA5.exe.

#include "game_launch.h"
#include "common/config.h"
#include "common/json.h"
#include "common/logger.h"
#include "common/registry.h"
#include "common/inject.h"

#include <shellapi.h>
#include <tlhelp32.h>
#include <vector>

namespace gage::launch {

    namespace {

        // BattlEye stays off for story mode; Rockstar honours it for modded play.
        const wchar_t* kNoBattlEye = L"-nobattleye";

        // Epic's catalog ids for Grand Theft Auto V (Legacy): namespace, catalog item, app name.
        // Product ids, identical for every Epic owner (EpicLaunchUrl's last resort).
        const char* kEpicGtaNamespace  = "0584d2013f0149a791e7b9bad0eec102";
        const char* kEpicGtaLegacyItem = "6e563a2c0f5f46e3b4e88b5f4ed50cca";
        const char* kEpicGtaLegacyApp  = "9d2d0eb64d5c44529cece33fe2a46482";

        const wchar_t* kGameImages[] = { L"GTA5.exe", L"GTA5_Enhanced.exe" };

        // BattlEye's launcher stub: GTA5_BE.exe, GTA5_Enhanced_BE.exe (sources
        // also spell the Enhanced one GTAV_Enhanced_BE.exe), so match the shape
        // GTA*_BE.exe rather than a fixed list.
        bool IsBattlEyeImage(const std::wstring& image) {
            const size_t n = image.size();
            return n > 10 && _wcsnicmp(image.c_str(), L"GTA", 3) == 0 && _wcsicmp(image.c_str() + n - 7, L"_BE.exe") == 0;
        }

        // Steam app ids, used only when no appmanifest names the game folder.
        const wchar_t* kSteamAppLegacy = L"271590";
        const wchar_t* kSteamAppEnhanced = L"3240220";

        std::wstring DirOf(const std::wstring& path) {
            size_t i = path.find_last_of(L"\\/");
            return i == std::wstring::npos ? std::wstring() : path.substr(0, i + 1);
        }

        std::wstring LeafOf(std::wstring dir) {
            while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
            size_t i = dir.find_last_of(L"\\/");
            return i == std::wstring::npos ? dir : dir.substr(i + 1);
        }

        bool IsDir(const std::wstring& p) {
            DWORD a = GetFileAttributesW(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        }

        bool IsAnyOf(const wchar_t* name, const wchar_t* const* list, size_t n) {
            for (size_t i = 0; i < n; ++i)
                if (_wcsicmp(name, list[i]) == 0) return true;
            return false;
        }

        // Whole small file (manifests are a few KB); empty on any failure.
        std::string ReadSmallFile(const std::wstring& path, DWORD maxBytes = 256 * 1024) {
            std::string out;
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
            if (h == INVALID_HANDLE_VALUE) return out;
            LARGE_INTEGER size{};
            if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= maxBytes) {
                out.resize((size_t)size.QuadPart);
                DWORD got = 0;
                if (!ReadFile(h, &out[0], (DWORD)out.size(), &got, 0)) got = 0;
                out.resize(got);
            }
            CloseHandle(h);
            return out;
        }

        // Ids that go into a URL: only [A-Za-z0-9], so a hand-edited manifest
        // cannot smuggle extra URL parts in.
        bool IsPlainId(const std::string& s) {
            if (s.empty() || s.size() > 64) return false;
            for (char c : s)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
            return true;
        }

        // The quoted tokens of a Valve KeyValues (.acf) file, in order.
        std::vector<std::string> AcfTokens(const std::string& text) {
            std::vector<std::string> out;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '"') continue;
                std::string tok;
                for (++i; i < text.size() && text[i] != '"'; ++i) {
                    if (text[i] == '\\' && i + 1 < text.size()) ++i;
                    tok.push_back(text[i]);
                }
                out.push_back(tok);
            }
            return out;
        }

        std::wstring CanonicalDir(std::wstring dir);
        bool SameDir(const std::wstring& canonical, const std::wstring& other);
        bool SteamOwns(const std::wstring& canonical, std::string* appidOut);

        // The Steam app id of the game in `gameDir`: the appmanifest, in any
        // Steam library, whose steamapps\common\<installdir> is that folder;
        // else one next to the folder; else the edition's known id.
        std::wstring SteamAppId(const std::wstring& gameDir, bool enhanced) {
            std::string fromLibrary;
            if (SteamOwns(CanonicalDir(gameDir), &fromLibrary) && !fromLibrary.empty() &&
                fromLibrary.find_first_not_of("0123456789") == std::string::npos) {
                GLOGI("launch: steam app %s (steam library)", fromLibrary.c_str());
                return Widen(fromLibrary);
            }
            std::wstring common = DirOf(gameDir.substr(0, gameDir.size() - 1));   // ...\common\ .
            std::wstring steamapps = DirOf(common.substr(0, common.size() - 1));  // ...\steamapps\ .
            std::string folder = Narrow(LeafOf(gameDir));
            WIN32_FIND_DATAW fd{};
            HANDLE f = steamapps.empty() ? INVALID_HANDLE_VALUE
                                         : FindFirstFileW((steamapps + L"appmanifest_*.acf").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    std::vector<std::string> t = AcfTokens(ReadSmallFile(steamapps + fd.cFileName));
                    std::string appid, installdir;
                    for (size_t i = 0; i + 1 < t.size(); ++i) {
                        if (appid.empty() && _stricmp(t[i].c_str(), "appid") == 0) appid = t[i + 1];
                        if (installdir.empty() && _stricmp(t[i].c_str(), "installdir") == 0) installdir = t[i + 1];
                    }
                    bool digits = !appid.empty() && appid.find_first_not_of("0123456789") == std::string::npos;
                    if (digits && _stricmp(installdir.c_str(), folder.c_str()) == 0) {
                        FindClose(f);
                        GLOGI("launch: steam app %s from %s", appid.c_str(), Narrow(fd.cFileName).c_str());
                        return Widen(appid);
                    }
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            GLOGW("launch: no steam appmanifest names '%s'; using the %s app id", folder.c_str(),
                  enhanced ? "Enhanced" : "Legacy");
            return enhanced ? kSteamAppEnhanced : kSteamAppLegacy;
        }

        std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
            wchar_t buf[MAX_PATH * 2] = L"";
            DWORD bytes = sizeof(buf);
            if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, 0, buf, &bytes) != ERROR_SUCCESS) return L"";
            return buf;
        }

        // Run `exe` with `args`; an exe that asks for elevation goes through
        // ShellExecute so Windows shows its UAC prompt.
        bool RunProgram(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, DWORD* err) {
            std::wstring cmd = L"\"" + exe + L"\" " + args;
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(exe.c_str(), &cmd[0], 0, 0, FALSE, 0, 0, cwd.empty() ? 0 : cwd.c_str(), &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return true;
            }
            DWORD e = GetLastError();
            if (e == ERROR_ELEVATION_REQUIRED) {
                SHELLEXECUTEINFOW sei{};
                sei.cbSize = sizeof(sei);
                sei.lpFile = exe.c_str();
                sei.lpParameters = args.c_str();
                sei.lpDirectory = cwd.empty() ? 0 : cwd.c_str();
                sei.nShow = SW_SHOWNORMAL;
                if (ShellExecuteExW(&sei)) return true;
                e = GetLastError();
            }
            if (err) *err = e;
            return false;
        }

        bool OpenUrl(const std::wstring& url) {
            return (INT_PTR)ShellExecuteW(0, L"open", url.c_str(), 0, 0, SW_SHOWNORMAL) > 32;
        }

        bool StartRockstar(const std::wstring& gameDir, std::wstring* err) {
            std::wstring play = gameDir + L"PlayGTAV.exe";
            if (!FileExists(play)) {
                if (err) *err = L"PlayGTAV.exe is missing from " + gameDir +
                                L". Start GTA V from the Rockstar Games Launcher, then press Connect again.";
                return false;
            }
            DWORD e = 0;
            GLOGI("launch: run \"%s\" %s", Narrow(play).c_str(), Narrow(kNoBattlEye).c_str());
            if (RunProgram(play, kNoBattlEye, gameDir, &e)) return true;
            GLOGE("launch: PlayGTAV.exe failed: %s", Narrow(FormatError(e)).c_str());
            if (err) *err = L"Could not start PlayGTAV.exe: " + FormatError(e);
            return false;
        }

        // steam.exe -applaunch <appid> <args>: starts the game with arguments
        // and no confirmation dialog (steam://run/<id>//<args>/ asks first).
        std::wstring SteamApplaunchArgs(const std::wstring& gameDir, bool enhanced) {
            return L"-applaunch " + SteamAppId(gameDir, enhanced) + L" " + kNoBattlEye;
        }

        // Fallback when steam.exe is not registered: the URL protocol, which
        // asks the player to confirm the arguments.
        std::wstring SteamRunUrl(const std::wstring& gameDir, bool enhanced) {
            return L"steam://run/" + SteamAppId(gameDir, enhanced) + L"//" + kNoBattlEye + L"/";
        }

        bool StartSteam(const std::wstring& gameDir, bool enhanced, std::wstring* err) {
            // HKCU\Software\Valve\Steam\SteamExe is lowercase with forward slashes
            std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe");
            for (wchar_t& c : steam)
                if (c == L'/') c = L'\\';
            DWORD e = 0;
            std::wstring args = SteamApplaunchArgs(gameDir, enhanced);
            if (!steam.empty() && FileExists(steam)) {
                GLOGI("launch: run \"%s\" %s", Narrow(steam).c_str(), Narrow(args).c_str());
                if (RunProgram(steam, args, L"", &e)) return true;
                GLOGW("launch: steam.exe failed: %s; trying the steam:// URL", Narrow(FormatError(e)).c_str());
            } else {
                GLOGW("launch: no SteamExe in HKCU\\Software\\Valve\\Steam ('%s'); trying the steam:// URL",
                      Narrow(steam).c_str());
            }
            std::wstring url = SteamRunUrl(gameDir, enhanced);
            GLOGI("launch: open %s", Narrow(url).c_str());
            if (OpenUrl(url)) return true;
            GLOGE("launch: steam:// URL could not be opened");
            if (err) *err = L"Could not start GTA V through Steam. Is Steam installed? Start the game from Steam, then press Connect again.";
            return false;
        }

        // The Epic launcher URI from a namespace/item/app triple, or from just
        // the app name when either the namespace or the item is missing; "" when
        // `app` is not a plain id. ns and item are path-joined with %3A exactly
        // as the Epic launcher writes them.
        std::wstring EpicUrlFromIds(const std::string& app, const std::string& ns, const std::string& item) {
            if (!IsPlainId(app)) return L"";
            std::string id = IsPlainId(ns) && IsPlainId(item) ? ns + "%3A" + item + "%3A" + app : app;
            return L"com.epicgames.launcher://apps/" + Widen(id) + L"?action=launch&silent=true";
        }

        // The Epic triple from the game folder's own .egstore\*.mancpn
        // ({"AppName":..,"CatalogNamespace":..,"CatalogItemId":..}); "" when no
        // manifest yields a usable AppName.
        std::wstring EpicUrlFromMancpn(const std::wstring& gameDir) {
            std::string app, ns, item;
            WIN32_FIND_DATAW fd{};
            std::wstring store = gameDir + L".egstore\\";
            HANDLE f = FindFirstFileW((store + L"*.mancpn").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    std::string text = ReadSmallFile(store + fd.cFileName);
                    JParser jp;
                    jp.maxNodes = 256;
                    if (!jp.parse(text.data(), text.size()) || jp.v.t != JVal::Obj) continue;
                    for (const auto& kv : jp.v.o) {
                        if (kv.second.t != JVal::Str) continue;
                        if (kv.first == "AppName") app = kv.second.s;
                        else if (kv.first == "CatalogNamespace") ns = kv.second.s;
                        else if (kv.first == "CatalogItemId") item = kv.second.s;
                    }
                    if (IsPlainId(app)) break;
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            return EpicUrlFromIds(app, ns, item);
        }

        // The Epic triple from Epic's own per-app install manifests
        // (<ProgramData>\Epic\EpicGamesLauncher\Data\Manifests\*.item, each a JSON
        // object with InstallLocation / CatalogNamespace / CatalogItemId / AppName)
        // whose InstallLocation is `canonical` (already from CanonicalDir). This
        // reaches a copy whose .egstore ships no .mancpn but which Epic still
        // tracks. Both the system drive's %ProgramData% and H:\ProgramData are
        // scanned (Epic may be installed under either). "" when no .item names the
        // folder.
        std::wstring EpicUrlFromManifests(const std::wstring& canonical) {
            if (canonical.empty()) return L"";
            std::vector<std::wstring> dirs;
            wchar_t pd[MAX_PATH] = L"";
            if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH))
                dirs.push_back(std::wstring(pd) + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests\\");
            dirs.push_back(L"H:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests\\");
            for (const std::wstring& dir : dirs) {
                WIN32_FIND_DATAW fd{};
                HANDLE f = FindFirstFileW((dir + L"*.item").c_str(), &fd);
                if (f == INVALID_HANDLE_VALUE) continue;
                do {
                    std::string text = ReadSmallFile(dir + fd.cFileName);
                    JParser jp;
                    jp.maxNodes = 4096;
                    if (!jp.parse(text.data(), text.size()) || jp.v.t != JVal::Obj) continue;
                    std::string app, ns, item, loc;
                    for (const auto& kv : jp.v.o) {
                        if (kv.second.t != JVal::Str) continue;
                        if (kv.first == "AppName") app = kv.second.s;
                        else if (kv.first == "CatalogNamespace") ns = kv.second.s;
                        else if (kv.first == "CatalogItemId") item = kv.second.s;
                        else if (kv.first == "InstallLocation") loc = kv.second.s;
                    }
                    if (!loc.empty() && SameDir(canonical, Widen(loc))) {
                        FindClose(f);
                        GLOGI("launch: epic .item %s names %s", Narrow(fd.cFileName).c_str(),
                              Narrow(canonical).c_str());
                        return EpicUrlFromIds(app, ns, item);
                    }
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            return L"";
        }

        // The Epic launcher URI for the game in `gameDir`. Sources, in order:
        //   1. the copy's own .egstore\*.mancpn (needs no outside configuration);
        //   2. gage_mp.cfg, for a copy whose .egstore holds no .mancpn (Epic does
        //      not always write one) -- epic_launch_url is used verbatim, else
        //      epic_app_id (+ optional epic_namespace / epic_catalog_item) builds it;
        //   3. Epic's own install manifests that name this folder.
        // "" when no source yields a usable URI.
        std::wstring EpicLaunchUrl(const std::wstring& gameDir) {
            std::wstring url = EpicUrlFromMancpn(gameDir);
            if (!url.empty()) return url;

            // gage_mp.cfg is owner-managed; still require the verbatim override to
            // be an Epic launcher URI so a stray value cannot be shell-opened.
            std::wstring verbatim = cfg::Get(L"epic_launch_url");
            const std::wstring scheme = L"com.epicgames.launcher://";
            if (verbatim.size() > scheme.size() && verbatim.compare(0, scheme.size(), scheme) == 0) {
                GLOGI("launch: epic_launch_url from gage_mp.cfg");
                return verbatim;
            }
            url = EpicUrlFromIds(Narrow(cfg::Get(L"epic_app_id")), Narrow(cfg::Get(L"epic_namespace")),
                                 Narrow(cfg::Get(L"epic_catalog_item")));
            if (!url.empty()) {
                GLOGI("launch: epic url from gage_mp.cfg epic_app_id");
                return url;
            }

            url = EpicUrlFromManifests(CanonicalDir(gameDir));
            if (!url.empty()) return url;

            // 4. Epic's catalog ids for GTA V (Legacy) -- the same for every Epic owner, not per user
            //    -- for an Epic copy (.egstore present) holding the Legacy GTA5.exe that no record
            //    above names: Epic writes no .mancpn for it and stops tracking a moved or copied
            //    install, yet its authenticated URI still starts the game (proven 2026-11-01 on
            //    C:\gta epic\GTAV, whose only record is .egstore\*.manifest). Without it the launch
            //    falls back to a bare PlayGTAV and Epic shows "UNABLE TO LAUNCH GAME".
            if (IsDir(gameDir + L".egstore") && FileExists(gameDir + L"GTA5.exe")) {
                GLOGI("launch: epic url from the built-in GTA V (Legacy) catalog ids (no Epic record names %s)",
                      Narrow(gameDir).c_str());
                return EpicUrlFromIds(kEpicGtaLegacyApp, kEpicGtaNamespace, kEpicGtaLegacyItem);
            }
            return L"";
        }

        bool StartEpic(const std::wstring& gameDir, std::wstring* err) {
            std::wstring url = EpicLaunchUrl(gameDir);
            if (url.empty()) {
                if (err) *err = L"Could not read the Epic Games manifest in " + gameDir +
                                L". Start GTA V from the Epic Games Launcher, then press Connect again.";
                return false;
            }
            GLOGI("launch: open %s", Narrow(url).c_str());
            if (OpenUrl(url)) return true;
            GLOGE("launch: Epic URI could not be opened");
            if (err) *err = L"Could not start GTA V through the Epic Games Launcher. Start the game from Epic, then press Connect again.";
            return false;
        }

        // ---- which store installed a folder: the stores' own records ----

        // The folder as the file system names it (case, 8.3 names, junctions
        // and symlinks resolved), lowercase, no trailing backslash; "" when
        // it does not exist.
        std::wstring CanonicalDir(std::wstring dir) {
            while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
            if (dir.empty()) return L"";
            HANDLE h = CreateFileW(dir.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
            if (h == INVALID_HANDLE_VALUE) return L"";
            std::wstring out(1024, L'\0');
            DWORD n = GetFinalPathNameByHandleW(h, &out[0], (DWORD)out.size(), FILE_NAME_NORMALIZED);
            CloseHandle(h);
            if (n == 0 || n >= out.size()) return L"";
            out.resize(n);
            if (out.compare(0, 8, L"\\\\?\\UNC\\") == 0) out = L"\\\\" + out.substr(8);
            else if (out.compare(0, 4, L"\\\\?\\") == 0) out = out.substr(4);
            while (out.size() > 3 && out.back() == L'\\') out.pop_back();
            CharLowerBuffW(&out[0], (DWORD)out.size());
            return out;
        }

        bool SameDir(const std::wstring& canonical, const std::wstring& other) {
            if (canonical.empty() || other.empty()) return false;
            return canonical == CanonicalDir(other);
        }

        // Steam library roots: `steamRoot` plus every "path" in its
        // steamapps\libraryfolders.vdf.
        std::vector<std::wstring> SteamLibrariesOf(std::wstring steamRoot) {
            std::vector<std::wstring> libs;
            for (wchar_t& c : steamRoot)
                if (c == L'/') c = L'\\';
            while (!steamRoot.empty() && steamRoot.back() == L'\\') steamRoot.pop_back();
            if (steamRoot.empty()) return libs;
            libs.push_back(steamRoot);
            std::vector<std::string> t =
                AcfTokens(ReadSmallFile(steamRoot + L"\\steamapps\\libraryfolders.vdf", 1024 * 1024));
            for (size_t i = 0; i + 1 < t.size(); ++i)
                if (_stricmp(t[i].c_str(), "path") == 0) libs.push_back(Widen(t[i + 1]));
            return libs;
        }

        std::vector<std::wstring> SteamLibraries() {
            std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
            if (steam.empty()) steam = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
            return SteamLibrariesOf(steam);
        }

        // One of `libs` holds the game in `canonical`: an appmanifest_*.acf
        // whose steamapps\common\<installdir> is that folder.
        bool SteamOwnsIn(const std::wstring& canonical, const std::vector<std::wstring>& libs, std::string* appidOut) {
            for (const std::wstring& lib : libs) {
                std::wstring steamapps = lib + L"\\steamapps\\";
                WIN32_FIND_DATAW fd{};
                HANDLE f = FindFirstFileW((steamapps + L"appmanifest_*.acf").c_str(), &fd);
                if (f == INVALID_HANDLE_VALUE) continue;
                do {
                    std::vector<std::string> t = AcfTokens(ReadSmallFile(steamapps + fd.cFileName));
                    std::string appid, installdir;
                    for (size_t i = 0; i + 1 < t.size(); ++i) {
                        if (appid.empty() && _stricmp(t[i].c_str(), "appid") == 0) appid = t[i + 1];
                        if (installdir.empty() && _stricmp(t[i].c_str(), "installdir") == 0) installdir = t[i + 1];
                    }
                    if (!installdir.empty() && SameDir(canonical, steamapps + L"common\\" + Widen(installdir))) {
                        FindClose(f);
                        if (appidOut) *appidOut = appid;
                        return true;
                    }
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            return false;
        }

        bool SteamOwns(const std::wstring& canonical, std::string* appidOut) {
            return SteamOwnsIn(canonical, SteamLibraries(), appidOut);
        }

        // LauncherInstalled.dat text ({"InstallationList":[{"InstallLocation":..},..]})
        // lists the folder.
        bool EpicListOwns(const std::wstring& canonical, const std::string& text) {
            JParser jp;
            jp.maxNodes = 65536;
            if (text.empty() || !jp.parse(text.data(), text.size()) || jp.v.t != JVal::Obj) return false;
            for (const auto& kv : jp.v.o) {
                if (kv.first != "InstallationList" || kv.second.t != JVal::Arr) continue;
                for (const JVal& item : kv.second.a) {
                    if (item.t != JVal::Obj) continue;
                    for (const auto& f : item.o)
                        if (f.first == "InstallLocation" && f.second.t == JVal::Str && SameDir(canonical, Widen(f.second.s)))
                            return true;
                }
            }
            return false;
        }

        bool EpicOwns(const std::wstring& canonical) {
            wchar_t pd[MAX_PATH] = L"";
            if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) return false;
            return EpicListOwns(canonical, ReadSmallFile(std::wstring(pd) + L"\\Epic\\UnrealEngineLauncher\\LauncherInstalled.dat",
                                                         4 * 1024 * 1024));
        }

        // Rockstar's own registry record of the folder: which value names it
        // says which store installed it.
        bool RockstarRecord(const std::wstring& canonical, Store* out) {
            static const wchar_t* keys[] = {
                L"SOFTWARE\\WOW6432Node\\Rockstar Games\\Grand Theft Auto V",
                L"SOFTWARE\\WOW6432Node\\Rockstar Games\\GTAV",
                L"SOFTWARE\\WOW6432Node\\Rockstar Games\\GTAV Enhanced",
                L"SOFTWARE\\Rockstar Games\\Grand Theft Auto V",
                L"SOFTWARE\\Rockstar Games\\GTAV Enhanced",
            };
            static const struct { const wchar_t* value; Store store; } values[] = {
                { L"InstallFolderSteam", Store::Steam },
                { L"InstallFolderEpic", Store::Epic },
                { L"InstallFolder", Store::Rockstar },
            };
            for (const wchar_t* key : keys)
                for (const auto& v : values)
                    if (SameDir(canonical, RegString(HKEY_LOCAL_MACHINE, key, v.value))) {
                        *out = v.store;
                        return true;
                    }
            return false;
        }

        // The game exe in `dir` (GTA5.exe first), or "". Forward slashes and
        // trailing separators, as the stores' records write them, are fine.
        std::wstring GameExeInDir(std::wstring dir) {
            for (wchar_t& c : dir)
                if (c == L'/') c = L'\\';
            while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
            if (dir.empty()) return L"";
            for (const wchar_t* image : kGameImages)
                if (FileExists(dir + L"\\" + image)) return dir + L"\\" + image;
            return L"";
        }

        // The game in one of `libs`: steamapps\common\<installdir> named by a
        // GTA V app manifest, Legacy before Enhanced; "" when none.
        std::wstring SteamGameIn(const std::vector<std::wstring>& libs) {
            for (const wchar_t* appid : { kSteamAppLegacy, kSteamAppEnhanced })
                for (const std::wstring& lib : libs) {
                    std::wstring steamapps = lib + L"\\steamapps\\";
                    std::vector<std::string> t =
                        AcfTokens(ReadSmallFile(steamapps + L"appmanifest_" + appid + L".acf"));
                    for (size_t i = 0; i + 1 < t.size(); ++i) {
                        if (_stricmp(t[i].c_str(), "installdir") != 0) continue;
                        std::wstring exe = GameExeInDir(steamapps + L"common\\" + Widen(t[i + 1]));
                        if (!exe.empty()) return exe;
                        break;
                    }
                }
            return L"";
        }

        // Every InstallLocation in LauncherInstalled.dat text that holds a game
        // exe, in list order.
        std::vector<std::wstring> EpicGamesIn(const std::string& text) {
            std::vector<std::wstring> out;
            JParser jp;
            jp.maxNodes = 65536;
            if (text.empty() || !jp.parse(text.data(), text.size()) || jp.v.t != JVal::Obj) return out;
            for (const auto& kv : jp.v.o) {
                if (kv.first != "InstallationList" || kv.second.t != JVal::Arr) continue;
                for (const JVal& item : kv.second.a) {
                    if (item.t != JVal::Obj) continue;
                    for (const auto& f : item.o)
                        if (f.first == "InstallLocation" && f.second.t == JVal::Str) {
                            std::wstring exe = GameExeInDir(Widen(f.second.s));
                            if (!exe.empty()) out.push_back(exe);
                        }
                }
            }
            return out;
        }

        // The first Legacy GTA5.exe among `found` (the engine targets Legacy),
        // else the first GTA5_Enhanced.exe; "" when `found` holds neither.
        std::wstring PreferLegacy(const std::vector<std::wstring>& found) {
            for (const std::wstring& exe : found)
                if (_wcsicmp(LeafOf(exe).c_str(), L"GTA5.exe") == 0) return exe;
            for (const std::wstring& exe : found)
                if (!exe.empty()) return exe;
            return L"";
        }

        // A full image path that is the Rockstar Games Launcher's own
        // Launcher.exe (...\Rockstar Games\Launcher\Launcher.exe), matched
        // case-insensitively. On Epic and retail this launcher -- not PlayGTAV
        // -- is what spawns the BattlEye wrapper, so the redirect shim must
        // reach it. A bare "Launcher.exe" from another game or tool is
        // deliberately not matched.
        bool IsRockstarLauncherPath(std::wstring path) {
            if (path.empty()) return false;
            CharLowerBuffW(&path[0], (DWORD)path.size());
            return path.find(L"\\rockstar games\\launcher\\launcher.exe") != std::wstring::npos;
        }

    }

    std::wstring FindInstalledGame() {
        std::vector<std::wstring> found;
        found.push_back(FindGameExe());  // Rockstar's registry values, then the standard folders
        found.push_back(SteamGameIn(SteamLibraries()));
        wchar_t pd[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH))
            for (const std::wstring& exe : EpicGamesIn(ReadSmallFile(
                     std::wstring(pd) + L"\\Epic\\UnrealEngineLauncher\\LauncherInstalled.dat", 4 * 1024 * 1024)))
                found.push_back(exe);
        std::wstring exe = PreferLegacy(found);
        GLOGI("launch: installed game search -> %s", exe.empty() ? "none found" : Narrow(exe).c_str());
        return exe;
    }

    const wchar_t* StoreName(Store s) {
        switch (s) {
            case Store::Steam: return L"Steam";
            case Store::Epic: return L"the Epic Games Launcher";
            case Store::Rockstar: return L"the Rockstar Games Launcher";
        }
        return L"the Rockstar Games Launcher";
    }

    // The stores' own records first (Steam's libraries and app manifests,
    // Epic's install list, Rockstar's registry values); the files in the
    // folder only when no record names it.
    Store DetectStore(const std::wstring& gameExe) {
        std::wstring dir = DirOf(gameExe);
        std::wstring canonical = CanonicalDir(dir);
        std::string steamApp;
        bool steamOwns = SteamOwns(canonical, &steamApp);
        bool epicOwns = EpicOwns(canonical);
        Store rs = Store::Rockstar;
        bool rsRecord = RockstarRecord(canonical, &rs);
        bool egstore = IsDir(dir + L".egstore");
        bool steamApi = FileExists(dir + L"steam_api64.dll");
        bool playGtav = FileExists(dir + L"PlayGTAV.exe");
        GLOGI("launch: game folder %s -> %s (steam library=%d%s%s epic list=%d rockstar registry=%s "
              ".egstore=%d steam_api64.dll=%d PlayGTAV.exe=%d)",
              Narrow(dir).c_str(), Narrow(canonical).c_str(), steamOwns ? 1 : 0, steamOwns ? " app " : "",
              steamApp.c_str(), epicOwns ? 1 : 0, rsRecord ? Narrow(StoreName(rs)).c_str() : "none",
              egstore ? 1 : 0, steamApi ? 1 : 0, playGtav ? 1 : 0);
        // Launch through a store only when that store's OWN library actually
        // owns the folder -- the one signal that proves the store can start the
        // copy. Everything below is weaker and must not force a store launch:
        // a Rockstar-launcher registry record only names the store a copy CAME
        // FROM (InstallFolderEpic/InstallFolderSteam), a bare .egstore ships
        // inside the game files, a steam_api64.dll can be a copied-out build or
        // a repack shim. None mean the attributed store will actually launch the
        // install -- an orphaned or moved Epic copy that Epic never registered
        // answers an epicgames:// URL with nothing ("the store did not answer").
        // PlayGTAV.exe is the Rockstar/retail bootstrapper that ships in every
        // non-Steam install (Epic copies included); with the Rockstar Games
        // Launcher present (which a registry record proves) it launches the copy
        // directly, settles entitlement and forwards our args. So when neither
        // store library owns the folder, PlayGTAV.exe wins over the registry
        // attribution, .egstore and steam_api64.dll; those decide only when the
        // bootstrapper is absent.
        if (steamOwns) return Store::Steam;
        if (epicOwns) return Store::Epic;
        if (playGtav) return Store::Rockstar;
        if (rsRecord) return rs;
        if (egstore) return Store::Epic;
        if (steamApi) return Store::Steam;
        return Store::Rockstar;
    }

    bool StartThroughStore(const std::wstring& gameExe, Store store, std::wstring* err) {
        std::wstring dir = DirOf(gameExe);
        bool enhanced = _wcsicmp(LeafOf(gameExe).c_str(), L"GTA5_Enhanced.exe") == 0;
        GLOGI("launch: starting %s through %s", Narrow(gameExe).c_str(), Narrow(StoreName(store)).c_str());
        switch (store) {
            case Store::Steam: return StartSteam(dir, enhanced, err);
            case Store::Epic: return StartEpic(dir, err);
            case Store::Rockstar: return StartRockstar(dir, err);
        }
        return StartRockstar(dir, err);
    }

    bool StartViaShim(const std::wstring& gameExe, const std::wstring& shimDll, std::wstring* err) {
        std::wstring dir = DirOf(gameExe);
        std::wstring play = dir + L"PlayGTAV.exe";
        if (!FileExists(play)) {
            if (err) *err = L"PlayGTAV.exe is missing from " + dir +
                            L". GTA V must be a Legacy install the Rockstar/retail bootstrapper can start.";
            return false;
        }
        if (!FileExists(shimDll)) {
            if (err) *err = L"gage_launch_shim.dll not found next to SolarLauncher.exe.";
            return false;
        }

        // Handoff for the shim (read inside PlayGTAV / the Rockstar Launcher from
        // HKCU\Software\GAGE-MP): intercept the BattlEye wrapper and launch the real
        // game exe instead. The shim matches the wrapper by shape (GTA*_BE.exe), so it
        // catches both editions (GTA5_BE.exe and GTA5_Enhanced_BE.exe) without an
        // edition-specific name here; shim_redirect_from is cleared (empty = shape only)
        // and overwritten every launch so no stale value from an older build lingers.
        reg::Set(L"shim_redirect_from", L"");
        reg::Set(L"shim_redirect_to", gameExe);

        // Launch PlayGTAV ourselves (not through a store URL) so we hold its
        // handle and can inject. PlayGTAV runs its ROS / Social Club + update
        // startup for several seconds before it ever spawns the game, so the
        // shim installs its hook in time. (If a live run shows the spawn is
        // missed, switch to a suspended launch + ntdll-based injection.)
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + play + L"\"";
        std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
        cmdbuf.push_back(L'\0');
        if (!CreateProcessW(play.c_str(), cmdbuf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            dir.c_str(), &si, &pi)) {
            DWORD e = GetLastError();
            GLOGE("launch: CreateProcess PlayGTAV.exe failed: %s", Narrow(FormatError(e)).c_str());
            if (err) *err = L"Could not start PlayGTAV.exe: " + FormatError(e);
            return false;
        }
        CloseHandle(pi.hThread);
        GLOGI("launch: started PlayGTAV pid %lu (BattlEye-free ride), injecting redirect shim",
              (unsigned long)pi.dwProcessId);

        DWORD ierr = 0;
        bool ok = inject::InjectDll(pi.hProcess, shimDll, 30000, &ierr);
        GLOGI("launch: redirect shim %s into PlayGTAV pid %lu (err=0x%08lX)",
              ok ? "injected" : "FAILED", (unsigned long)pi.dwProcessId, (unsigned long)ierr);
        CloseHandle(pi.hProcess);
        if (!ok) {
            if (err) *err = L"Could not load the Solar RP launch helper into PlayGTAV (code " +
                            std::to_wstring(ierr) + L"). Try again, or set shim_launch=0 in gage_mp.cfg.";
            return false;
        }
        return true;
    }

    int InjectShimIntoLaunchers(const std::wstring& shimDll, std::vector<DWORD>& done) {
        int injected = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"Launcher.exe") != 0) continue;
                DWORD pid = pe.th32ProcessID;
                bool seen = false;
                for (DWORD d : done) if (d == pid) { seen = true; break; }
                if (seen) continue;
                HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                       PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
                if (!h) continue;  // transiently unopenable (mid-start); retry next tick
                wchar_t path[MAX_PATH * 2] = L"";
                DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
                bool isRgl = QueryFullProcessImageNameW(h, 0, path, &len) && IsRockstarLauncherPath(path);
                if (!isRgl) { CloseHandle(h); done.push_back(pid); continue; }
                DWORD ierr = 0;
                bool ok = inject::InjectDll(h, shimDll, 15000, &ierr);
                GLOGI("launch: redirect shim %s into Launcher.exe pid %lu (err=0x%08lX)",
                      ok ? "injected" : "FAILED", (unsigned long)pid, (unsigned long)ierr);
                CloseHandle(h);
                done.push_back(pid);  // one attempt per pid
                if (ok) ++injected;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return injected;
    }

    GameProc FindGame() {
        GameProc g;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return g;
        struct Entry { DWORD pid, parent; std::wstring image; };
        std::vector<Entry> all;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do all.push_back({ pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile });
            while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);

        const size_t nGame = sizeof(kGameImages) / sizeof(kGameImages[0]);
        for (const Entry& e : all) {
            if (IsBattlEyeImage(e.image)) g.battleye = true;
            if (!g.pid && IsAnyOf(e.image.c_str(), kGameImages, nGame)) {
                g.pid = e.pid;
                g.image = e.image;
                for (const Entry& p : all) {
                    if (p.pid != e.parent) continue;
                    g.parentImage = p.image;
                    if (IsBattlEyeImage(p.image)) g.battleye = true;
                }
            }
        }
        return g;
    }

}
