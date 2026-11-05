// Solar Launcher (SolarLauncher.exe; the internal code name stays GAGE-MP)
//
// Single-exe GUI launcher: a React + Tailwind SPA (built to one self-contained
// HTML file, embedded below) hosted in an embedded WebView2. All I/O — server
// list, ping, injection, favorites, recents, settings — happens here in native
// C++; the SPA is stateless (see apps/launcher/BRIDGE.md for the exact contract).
//
// Windows native, C++, camelCase, worker threads posting results back to the
// UI thread via PostMessageW.
//
// Build: wWinMain → needs -municode at compile AND link, --subsystem,windows.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>   // SHCreateMemStream
#include <commdlg.h>
#include <dwmapi.h>    // MARGINS only; the dwmapi calls are resolved at runtime
#include <psapi.h>
#include <wincrypt.h>  // CryptProtectData: the session token (DPAPI)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <combaseapi.h>
#include <unknwn.h>
#include <WebView2.h>

#include "common/common.h"
#include "common/logger.h"
#include "common/config.h"
#include "common/registry.h"
#include "common/inject.h"
#include "common/json.h"  // JVal / JParser (shared with net/serverfinder.cpp)
#include "net/serverfinder.h"
#include "protocol/protocol.h"  // gage::proto::kDefaultPort
#include "game_launch.h"
#include "integrity.h"
#include "legacy_handoff.h"
#include "updater.h"
#include "pack_sync.h"
#include "version_gate.h"  // the lockstep policy: protocol gate, every update mandatory, check timing (pure; tests/version_gate_test.cpp)
#include "build_config.h"  // generated: GAGE_UPDATE_URL, GAGE_UPDATE_PUBKEY
#include "ui_html.h"  // generated: embedded SPA bytes (kUiHtml / kUiHtmlLen)

using namespace gage;
using net::ServerInfo;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const wchar_t* kClientVersion = L"0.4.25";
static const wchar_t* kClass = L"GAGE_MP_Launcher";
static const wchar_t* kTitle = L"Solar Launcher";
static const int kAppIconId = 1;  // gage_gui.rc
using net::kDefaultListUrl;  // the GAGE master server list (net/serverfinder.h)
static const wchar_t* kDefaultVersionUrl = GAGE_UPDATE_URL;  // baked at configure time (build_config.h)
static const char* kUpdatePubKey = GAGE_UPDATE_PUBKEY;        // the key every update manifest must be signed with
static const wchar_t* kSingleInstanceMutex = L"Local\\GAGE-MP.Launcher.SingleInstance";
static const wchar_t* kShowMessageName = L"GAGE_MP_ShowWindow";  // registered message: "bring your window forward"

// The SPA is served from memory under this origin (WebResourceRequested); it is
// also the only origin allowed to navigate or to talk over the bridge.
static const wchar_t* kAppOrigin = L"https://app.gage/";
static const wchar_t* kAppUrl = L"https://app.gage/index.html";
static const wchar_t* kAppFilter = L"https://app.gage/*";

// Window: base size in DIP (the client area is this size), startup watchdog.
static const int kBaseClientW = 1240;
static const int kBaseClientH = 800;
// Smallest client area the SPA lays out without clipping (apps/launcher-ui,
// verified at 1024x700, 1240x800 and 1440x900).
static const int kMinClientW = 1024;
static const int kMinClientH = 700;
static const UINT kStartupTimeoutMs = 20000;   // controller must arrive within this
static const UINT_PTR kTimerStartup = 1;
static const UINT_PTR kTimerUpdateRetry = 2;   // while an installed-ready update waits for the game to close
static const UINT_PTR kTimerUpdateExit = 3;    // after the installer started: leave so it can replace us
static const UINT_PTR kTimerUpdatePoll = 4;    // re-check while the launcher stays open (tray): releases reach it
static const UINT_PTR kTimerLaunchWait = 5;    // PLAY waits for the update check it started, at most kLaunchCheckWaitMs
static const UINT kUpdatePollMs = gage::vgate::kUpdatePollMs;           // 2 min (version_gate.h)
static const UINT kLaunchCheckWaitMs = gage::vgate::kLaunchCheckWaitMs;
static const UINT kUpdateRetryMs = 15000;
static const UINT kUpdateExitDelayMs = 1500;   // setup's second stage opens its payload file within this
static const COREWEBVIEW2_COLOR kBgColor = { 255, 10, 9, 7 };  // #0a0907, the SPA's ground (NOON)

// Async result posts (UI thread applies them; stale generations are dropped).
#define WM_LAUNCH_DONE  (WM_APP + 1)
#define WM_STATUS_POST  (WM_APP + 2)   // lparam: std::wstring* (UI thread deletes)
#define WM_BROWSE       (WM_APP + 3)   // run the file dialog outside the WebView2 callback
#define WM_STARTUP_ABORT (WM_APP + 4)  // fatal WebView failure: leave WITHOUT touching the game
#define WM_WV_FAILED    (WM_APP + 5)   // wparam: COREWEBVIEW2_PROCESS_FAILED_KIND
#define WM_GAME_CLOSED  (WM_APP + 6)   // close-game worker finished; destroy the window
#define WM_JSON_POST    (WM_APP + 7)   // lparam: std::string* bridge JSON (UI thread deletes)
#define WM_FETCH_DONE   (WM_APP + 10)
#define WM_PING_DONE    (WM_APP + 11)
#define WM_UPDATE_DONE  (WM_APP + 12)  // lparam: UpdateCheckDone* (UI thread deletes)
#define WM_PING_BATCH   (WM_APP + 13)  // lparam: PingBatch* (UI thread deletes)
#define WM_UPDATE_DL_DONE (WM_APP + 14)  // lparam: UpdateDlDone* (UI thread deletes)
#define WM_UPDATE_PROGRESS (WM_APP + 15) // wparam: progress in 1/1000, lparam: 0 downloading, 1 verifying
#define WM_GAME_EXITED  (WM_APP + 16)  // the game the launcher watched has ended (retry a deferred update)
#define WM_TRAY_ACTION  (WM_APP + 20)  // legacy tray callback: LOWORD(lparam) = mouse message
#define WM_WINDOW_STATE (WM_APP + 21)  // re-send {"type":"window"} once the change has landed
#define WM_SERVER_PROTO (WM_APP + 22)  // wparam: the protocol a server reported in its UDP query (> 0); lparam: the latest client it reported (packed, 0 = unknown); see version_gate.h

// ---------------------------------------------------------------------------
// JSON helpers (parser in common/json.h)
// ---------------------------------------------------------------------------

static const JVal* jGet(const JVal* o, const char* key) {
    if (!o || o->t != JVal::Obj) return 0;
    for (size_t i = 0; i < o->o.size(); i++)
        if (o->o[i].first == key) return &o->o[i].second;
    return 0;
}

static std::wstring jStr(const JVal* v) {
    if (!v || v->t != JVal::Str) return L"";
    return Widen(v->s);
}
static long long jNum(const JVal* v, long long def) {
    if (!v || v->t != JVal::Num) return def;
    return (long long)llround(v->n);
}
static bool jBool(const JVal* v, bool def) {
    if (!v || v->t != JVal::Bool) return def;
    return v->b;
}

static std::string jEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    o.push_back('"');
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    o += b;
                } else {
                    o.push_back(c);
                }
        }
    }
    o.push_back('"');
    return o;
}

static std::string jStrW(const std::wstring& w) { return jEsc(Narrow(w)); }

static std::string numStr(long long v) {
    char b[24];
    snprintf(b, sizeof(b), "%lld", v);
    return b;
}

static std::string serverJson(const ServerInfo& s) {
    return "{\"host\":" + jStrW(s.host) +
           ",\"port\":" + numStr(s.port) +
           ",\"name\":" + jStrW(s.name) +
           ",\"players\":" + numStr(s.players) +
           ",\"ping\":" + numStr(s.ping) +
           ",\"url\":" + jStrW(s.url) +
           ",\"maxPlayers\":" + numStr(s.maxPlayers) +
           ",\"mode\":" + jStrW(s.mode) +
           ",\"lang\":" + jStrW(s.lang) + "}";
}

static std::string serverListJson(const std::vector<ServerInfo>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) o += ",";
        o += serverJson(v[i]);
    }
    o += "]";
    return o;
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

static HINSTANCE g_hInst = 0;
static HWND g_hwnd = 0;
static DWORD g_uiThread = 0;  // WebView2 is STA: only this thread may touch g_wv/g_ctrl/g_env
static HICON g_trayIcon = 0;
static NOTIFYICONDATAW g_nid = {};
static bool g_trayOk = false;           // icon actually added; "close" may hide to tray only then
static UINT g_taskbarCreated = 0;       // re-add the icon after an Explorer restart
static std::atomic<bool> g_launching{false};  // one launch at a time (cleared on WM_LAUNCH_DONE)
static UINT g_showMsg = 0;              // registered message: a second instance asks us to show the window
static int g_exitCode = 0;              // process exit code (PostQuitMessage in WM_DESTROY)
static bool g_closing = false;          // WM_CLOSE accepted; waiting for the close-game worker
static bool g_frameless = true;         // false = fall back to the native caption (see ApplyFrame)

// The game process DoLaunch started or injected into: a handle owned by the UI
// thread. close_game acts on exactly this process (the handle pins the process
// object, so a recycled PID can never be hit) and never on a game the launcher
// did not touch.
static HANDLE g_gameProc = 0;

static std::vector<ServerInfo> g_servers;
static std::vector<ServerInfo> g_favs;
static std::vector<ServerInfo> g_recs;
// invalidates stale fetch/ping posts; written on the UI thread, read by the
// list pinger to stop once a newer fetch starts
static std::atomic<int> g_gen{0};

// WebView2 (UI-thread owned; the static loader keeps the runtime bound to it).
static ICoreWebView2Environment* g_env = 0;
static ICoreWebView2Controller* g_ctrl = 0;
static ICoreWebView2* g_wv = 0;
static int g_wvVisible = -1;  // last put_IsVisible value (-1 = never set)
// Event handlers: host-owned (ref=1 from creation), released in WM_DESTROY after
// their registrations are removed.
struct WebMsgHandler;  // fwd
struct NavHandler;
struct NewWinHandler;
struct ResHandler;
struct ProcFailHandler;
static WebMsgHandler* g_msgHandler = 0;
static NavHandler* g_navHandler = 0;
static NewWinHandler* g_newWinHandler = 0;
static ResHandler* g_resHandler = 0;
static ProcFailHandler* g_failHandler = 0;
static EventRegistrationToken g_msgToken = {};
static EventRegistrationToken g_navToken = {};
static EventRegistrationToken g_newWinToken = {};
static EventRegistrationToken g_resToken = {};
static EventRegistrationToken g_failToken = {};
static bool g_resFilterAdded = false;

static std::wstring baseName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static const wchar_t* kGameExes[] = { L"GTA5.exe", L"GTA5_Enhanced.exe" };

// True when the path's file name is one of the two game executables.
static bool IsGameExeName(const std::wstring& p) {
    std::wstring b = baseName(p);
    for (const wchar_t* e : kGameExes)
        if (_wcsicmp(b.c_str(), e) == 0) return true;
    return false;
}

// The SPA (and OnBrowse) store the game *folder* (or the exact exe for
// GTA5_Enhanced.exe); gage_v and older configs store the exe. Accept both and
// return the exe, or "" if neither resolves. A file only counts when it IS
// GTA5.exe / GTA5_Enhanced.exe: injecting into an arbitrary executable that
// someone typed into the settings is never what the user meant.
static std::wstring ResolveGameExe(const std::wstring& p) {
    if (p.empty()) return L"";
    if (IsDir(p)) {
        std::wstring d = p;
        if (d.back() != L'\\' && d.back() != L'/') d += L'\\';
        for (const wchar_t* e : kGameExes)
            if (FileExists(d + e)) return d + e;
        return L"";
    }
    if (FileExists(p) && IsGameExeName(p)) return p;
    return L"";
}

// ---------------------------------------------------------------------------
// Settings (native = source of truth). SPA camelCase keys <-> cfg keys.
// ---------------------------------------------------------------------------

static std::wstring GetGamePath() {
    std::wstring v = cfg::Get(L"game_path");
    if (v.empty()) v = reg::Get(L"game_path");
    return v;
}
static std::wstring GetPlayerName() {
    std::wstring v = cfg::Get(L"player_name");
    if (v.empty()) v = reg::Get(L"player_name");
    return v;
}
static bool GetAutoUpdate() {
    std::wstring v = cfg::Get(L"auto_update");
    if (v.empty()) v = reg::Get(L"auto_update", L"1");
    return v != L"0";
}
static bool GetStartMinimized() {
    std::wstring v = cfg::Get(L"start_min");
    if (v.empty()) v = reg::Get(L"start_min");
    return v == L"1";
}
static bool GetCloseGame() {
    std::wstring v = cfg::Get(L"close_game");
    if (v.empty()) v = reg::Get(L"close_game", L"1");
    return v != L"0";
}
static std::wstring GetLanguage() {
    std::wstring v = cfg::Get(L"language");
    if (v.empty()) v = reg::Get(L"language");
    if (v.empty()) v = L"en";
    return v;
}
// The server list comes from kDefaultListUrl (the GAGE master server's
// GET /servers) unless gage_mp.cfg carries a hand-edited `list_url` (a deployed
// master, a private list). The
// app no longer offers a list URL setting, so the value its old Settings row
// saved (`server_endpoint`, cfg + registry) is ignored on purpose: a stale one
// could otherwise never be cleared from the UI. Read on every fetch.
static std::wstring GetListOverride() {
    return cfg::Get(L"list_url");
}

// Testing before the platform is up: `test_server=host[:port]` in gage_mp.cfg,
// else the build's GAGE_TEST_SERVER (port defaults to gage::proto::kDefaultPort).
// Used only in demo mode, where PLAY would otherwise have no real server.
// False when unset or malformed.
static bool GetTestServer(ServerInfo* out) {
    std::wstring v = cfg::Get(L"test_server");  // cfg trims values on read
    if (v.empty()) v = GAGE_TEST_SERVER;
    if (v.empty()) return false;
    std::wstring host = v;
    int port = gage::proto::kDefaultPort;
    size_t colon = v.find_last_of(L':');
    if (colon != std::wstring::npos) {
        host = v.substr(0, colon);
        std::wstring p = v.substr(colon + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of(L"0123456789") != std::wstring::npos) host.clear();
        else port = _wtoi(p.c_str());
    }
    bool ok = !host.empty() && host.size() <= 253 && port >= 1 && port <= 65535;
    for (wchar_t c : host)
        if (!(iswalnum(c) || c == L'.' || c == L'-')) ok = false;
    if (!ok) {
        GLOGW("test_server '%s' in gage_mp.cfg is not host[:port]; ignored", Narrow(v).c_str());
        return false;
    }
    out->host = host;
    out->port = port;
    out->name = L"Test server " + host;
    return true;
}

// Appended to the game's arguments where the store passes them on (Settings).
static const size_t kMaxLaunchOptions = 512;
static std::wstring GetLaunchOptions() {
    std::wstring v = cfg::Get(L"launch_options");
    if (v.empty()) v = reg::Get(L"launch_options");
    return v.size() > kMaxLaunchOptions ? L"" : v;
}

// `origin` is an https:// origin, or http:// on 127.0.0.1 / localhost for
// development: a host with an optional :port and nothing after it.
static bool ValidApiOrigin(const std::wstring& origin) {
    std::wstring host;
    bool loopbackOnly = false;
    if (_wcsnicmp(origin.c_str(), L"https://", 8) == 0) host = origin.substr(8);
    else if (_wcsnicmp(origin.c_str(), L"http://", 7) == 0) { host = origin.substr(7); loopbackOnly = true; }
    else return false;
    if (host.empty() || host.size() > 253) return false;
    for (wchar_t c : host)
        if (!(iswalnum(c) || c == L'.' || c == L'-' || c == L':')) return false;
    size_t colon = host.find(L':');
    std::wstring name = host.substr(0, colon);
    if (name.empty()) return false;
    if (colon != std::wstring::npos) {
        std::wstring port = host.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
        if (_wtoi(port.c_str()) < 1 || _wtoi(port.c_str()) > 65535) return false;
    }
    return !loopbackOnly || _wcsicmp(name.c_str(), L"127.0.0.1") == 0 || _wcsicmp(name.c_str(), L"localhost") == 0;
}

// The platform API origin the SPA calls (apps/platform): an `api_base=` line in
// gage_mp.cfg, else the build's GAGE_API_BASE. "" when neither is set or the
// value is not a valid origin: the SPA then runs its labelled demo data.
static std::wstring GetApiBase() {
    std::wstring v = cfg::Get(L"api_base");
    const bool fromCfg = !v.empty();
    if (!fromCfg) v = GAGE_API_BASE;
    while (!v.empty() && v.back() == L'/') v.pop_back();
    if (v.empty()) return L"";
    if (!ValidApiOrigin(v)) {
        GLOGW("api base: '%s' (%s) is not an https:// or loopback http:// origin; ignored",
              Narrow(v).c_str(), fromCfg ? "gage_mp.cfg api_base" : "GAGE_API_BASE");
        return L"";
    }
    return v;
}

static std::wstring TrimWs(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static void SaveSetting(const std::wstring& spaKey, const std::wstring& rawValue, bool isBool) {
    // cfg trims values on read; trim here too so the registry copy cannot differ.
    std::wstring value = isBool ? rawValue : TrimWs(rawValue);
    std::wstring key;
    if (spaKey == L"gamePath") key = L"game_path";
    else if (spaKey == L"playerName" || spaKey == L"player_name") key = L"player_name";
    else if (spaKey == L"autoUpdate") key = L"auto_update";
    else if (spaKey == L"startMinimized") key = L"start_min";
    else if (spaKey == L"closeGame") key = L"close_game";
    else if (spaKey == L"language") key = L"language";
    else if (spaKey == L"launchOptions") {
        if (value.size() > kMaxLaunchOptions) {
            GLOGW("saveSetting: launchOptions refused (%zu characters, at most %zu)", value.size(), kMaxLaunchOptions);
            return;
        }
        key = L"launch_options";
    } else {
        // Allowlist only: the bridge must not write arbitrary cfg/registry keys
        // (server_host, inject_target, ... are launcher-owned handoff values).
        GLOGW("saveSetting: ignoring unknown key '%s'", Narrow(spaKey).c_str());
        return;
    }
    if (isBool) {
        std::wstring b = value == L"1" || value == L"true" ? L"1" : L"0";
        cfg::Set(key, b);
        reg::Set(key, b);
    } else {
        cfg::Set(key, value);
        reg::Set(key, value);
    }
}

// ---------------------------------------------------------------------------
// Bridge: native -> JS
// ---------------------------------------------------------------------------

static void PostJson(const std::string& json) {
    if (GetCurrentThreadId() != g_uiThread) {
        // ICoreWebView2 is UI-thread only; workers must marshal (see PostStatusFromWorker).
        GLOGE("PostJson called off the UI thread — dropped: %.120s", json.c_str());
        return;
    }
    if (!g_wv) return;
    HRESULT hr = g_wv->PostWebMessageAsJson(Widen(json).c_str());
    if (FAILED(hr)) GLOGE("PostWebMessageAsJson failed hr=0x%08lX", (unsigned long)hr);
}

static void PostStatus(const std::wstring& text) {
    PostJson("{\"type\":\"status\",\"text\":" + jStrW(text) + "}");
}

// Worker threads: hand the text to the UI thread, which owns the WebView.
static void PostStatusFromWorker(const std::wstring& text) {
    std::wstring* p = new std::wstring(text);
    if (!PostMessageW(g_hwnd, WM_STATUS_POST, 0, (LPARAM)p)) delete p;
}

// Worker threads: any bridge message, sent on the UI thread.
static void PostJsonFromWorker(const std::string& json) {
    std::string* p = new std::string(json);
    if (!PostMessageW(g_hwnd, WM_JSON_POST, 0, (LPARAM)p)) delete p;
}

static void PostSettings() {
    PostJson("{\"type\":\"settings\""
             ",\"gamePath\":" + jStrW(GetGamePath()) +
             ",\"playerName\":" + jStrW(GetPlayerName()) +
             ",\"language\":" + jStrW(GetLanguage()) +
             ",\"autoUpdate\":" + std::string(GetAutoUpdate() ? "true" : "false") +
             ",\"startMinimized\":" + std::string(GetStartMinimized() ? "true" : "false") +
             ",\"closeGame\":" + std::string(GetCloseGame() ? "true" : "false") +
             ",\"version\":" + jStrW(kClientVersion) +
             ",\"launchOptions\":" + jStrW(GetLaunchOptions()) +
             ",\"apiBase\":" + jStrW(GetApiBase()) + "}");
}

// {"type":"window",...}: drives the SPA's maximize icon and its render pause.
// Sent on `ready` (force: a reloaded page knows nothing) and whenever one of
// the three values changes.
static void PostWindowState(bool force = false) {
    static int last = -1;
    if (!g_hwnd) return;
    if (force) last = -1;
    bool maximized = IsZoomed(g_hwnd) != FALSE;
    bool visible = IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd);
    bool focused = visible && GetForegroundWindow() == g_hwnd;
    int now = (maximized ? 1 : 0) | (focused ? 2 : 0) | (visible ? 4 : 0);
    if (now == last) return;
    last = now;
    PostJson(std::string("{\"type\":\"window\",\"maximized\":") + (maximized ? "true" : "false") +
             ",\"focused\":" + (focused ? "true" : "false") + ",\"visible\":" + (visible ? "true" : "false") + "}");
}

static void PostServers(const std::string& status, const std::wstring& error) {
    PostJson("{\"type\":\"servers\",\"status\":\"" + status + "\""
             ",\"servers\":" + serverListJson(g_servers) +
             ",\"error\":" + jStrW(error) + "}");
}

static void PostFavorites() { PostJson("{\"type\":\"favorites\",\"servers\":" + serverListJson(g_favs) + "}"); }
static void PostRecents() { PostJson("{\"type\":\"recents\",\"servers\":" + serverListJson(g_recs) + "}"); }

static void PostLaunchResult(bool ok, long code, const std::wstring& error) {
    PostJson("{\"type\":\"launchResult\",\"ok\":" + std::string(ok ? "true" : "false") +
             ",\"code\":" + numStr(code) +
             ",\"error\":" + jStrW(error) + "}");
}

// ---------------------------------------------------------------------------
// Favorites / recents persistence (cfg list, same encoding as before)
// ---------------------------------------------------------------------------

static std::wstring KeyOf(const ServerInfo& s) { return s.host + L":" + std::to_wstring(s.port); }

static std::wstring EncServer(const ServerInfo& s) { return KeyOf(s) + L"|" + s.name; }

static bool DecServer(const std::wstring& enc, ServerInfo& out) {
    size_t bar = enc.find(L'|');
    std::wstring hp = bar == std::wstring::npos ? enc : enc.substr(0, bar);
    std::wstring name = bar == std::wstring::npos ? L"" : enc.substr(bar + 1);
    if (!net::ParseDirect(hp, out)) return false;
    if (name.empty()) name = out.host;
    out.name = name;
    out.players = -1;  // not persisted; BRIDGE.md: -1 = unknown
    out.ping = -1;
    return true;
}

static void LoadFavsRecents() {
    g_favs.clear();
    g_recs.clear();
    std::vector<std::wstring> f = cfg::GetList(L"favorites");
    for (size_t i = 0; i < f.size(); i++) { ServerInfo s; if (DecServer(f[i], s)) g_favs.push_back(s); }
    std::vector<std::wstring> r = cfg::GetList(L"recents");
    for (size_t i = 0; i < r.size(); i++) { ServerInfo s; if (DecServer(r[i], s)) g_recs.push_back(s); }
}

static void SaveFavorites() {
    std::vector<std::wstring> v;
    for (size_t i = 0; i < g_favs.size(); i++) v.push_back(EncServer(g_favs[i]));
    cfg::SetList(L"favorites", v);
}

static void SaveRecents() {
    std::vector<std::wstring> v;
    for (size_t i = 0; i < g_recs.size(); i++) v.push_back(EncServer(g_recs[i]));
    cfg::SetList(L"recents", v);
}

// A stored entry must be reconnectable: a host and a real port (1..65535).
static bool ValidEntry(const ServerInfo& s) { return !s.host.empty() && s.port >= 1 && s.port <= 65535; }

static void AddFavorite(const ServerInfo& s) {
    if (!ValidEntry(s)) {
        GLOGW("addFavorite rejected: host='%s' port=%d", Narrow(s.host).c_str(), s.port);
        PostFavorites();  // reply with the current list so the SPA stays in sync
        return;
    }
    std::wstring key = KeyOf(s);
    for (size_t i = 0; i < g_favs.size(); i++)
        if (KeyOf(g_favs[i]) == key) { g_favs[i] = s; SaveFavorites(); PostFavorites(); return; }
    g_favs.push_back(s);
    SaveFavorites();
    PostFavorites();
}

static void AddRecent(const ServerInfo& s) {
    if (!ValidEntry(s)) {
        GLOGW("addRecent rejected: host='%s' port=%d", Narrow(s.host).c_str(), s.port);
        PostRecents();
        return;
    }
    std::wstring key = KeyOf(s);
    for (size_t i = 0; i < g_recs.size();) {
        if (KeyOf(g_recs[i]) == key) g_recs.erase(g_recs.begin() + i);
        else i++;
    }
    g_recs.insert(g_recs.begin(), s);
    while (g_recs.size() > 10) g_recs.pop_back();
    SaveRecents();
    PostRecents();
}

// ---------------------------------------------------------------------------
// Launch (injection chain). Runs on a worker thread; posts results to UI.
// ---------------------------------------------------------------------------

// hProc/pid: the game process this launch started or injected into. Ownership of
// the handle travels with the message; the UI thread keeps it (g_gameProc).
struct LaunchResult { bool ok; long code; std::wstring error; HANDLE hProc = 0; DWORD pid = 0; };

static void PostLaunch(const LaunchResult& r) {
    LaunchResult* p = new LaunchResult(r);
    if (!PostMessageW(g_hwnd, WM_LAUNCH_DONE, 0, (LPARAM)p)) {
        // The window is gone (the user quit mid-launch): nobody will ever pick
        // this up, so free it here instead of leaking payload and handle.
        GLOGW("launch result dropped: launcher window is gone (ok=%d)", r.ok ? 1 : 0);
        if (p->hProc) CloseHandle(p->hProc);
        delete p;
    }
}

static void ApplyDefaultPort(ServerInfo& s) {
    if (s.port >= 1) return;
    std::wstring def = cfg::Get(L"default_port", std::to_wstring(proto::kDefaultPort));
    long p = _wtoi(def.c_str());
    s.port = (p >= 1 && p <= 65535) ? (int)p : (int)proto::kDefaultPort;
}

// How long DoLaunch waits for the store to bring the game up (it may update
// or ask the player to sign in first).
static const DWORD kGameStartWaitMs = 180000;

// Log when the game exits, how long it ran and its exit code: a crash right
// after start (e.g. "Unrecoverable fault") then shows in gage.log. Works on
// its own handle, so close_game's handle is never touched. On exit it also
// tells the SPA the game stopped ({"type":"game","running":false}), so the
// launch overlay leaves its "In game" state instead of staying stuck there
// (launchReducer GAME: launched + running:false -> idle). The post is marshalled
// to the UI thread because this runs on a detached worker (PostJson is UI-only).
static void WatchGameExit(HANDLE game, DWORD pid) {
    HANDLE w = 0;
    if (!DuplicateHandle(GetCurrentProcess(), game, GetCurrentProcess(), &w,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) return;
    DWORD t0 = GetTickCount();
    std::thread([w, pid, t0]() {
        WaitForSingleObject(w, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(w, &code);
        GLOGI("game pid %lu exited after %lu s, exit code 0x%08lX", (unsigned long)pid,
              (unsigned long)((GetTickCount() - t0) / 1000), (unsigned long)code);
        PostJsonFromWorker("{\"type\":\"game\",\"running\":false}");
        PostMessageW(g_hwnd, WM_GAME_EXITED, 0, 0);  // a deferred update may install now
        CloseHandle(w);
    }).detach();
}

// This launcher's own version (kClientVersion) as vgate's packed number; 0 (= never updates because of a server's
// say-so) if the constant ever fails to parse.
static uint32_t ClientVersionPacked() {
    static const uint32_t v = vgate::ParseVersion(Narrow(kClientVersion).c_str());
    return v;
}
// "0.4.4" for a packed version, "unknown" for 0 (log lines).
static std::string VersionText(uint32_t packed) {
    if (packed == vgate::kUnknownVersion) return "unknown";
    char b[32];
    snprintf(b, sizeof(b), "%u.%u.%u", (packed >> 16) & 255u, (packed >> 8) & 255u, packed & 255u);
    return b;
}

static std::vector<std::string> UpdateUrls();  // with the update code below

// Download the car packs the server publishes (packs.json next to the update manifest) into
// %LOCALAPPDATA%\GAGE-MP\packs, with progress as launch status lines. Launch worker thread.
// gage_mp.cfg pack_sync=0 switches it off (an operator staging packs by hand).
// 0.4.24 (GI: a friend joined with no packs and saw no Jesko): returns false = refuse the launch when the server's packs
// could not be checked or installed; pack_required=0 in gage_mp.cfg restores the old "start without them".
static bool SyncCarPacks() {
    if (cfg::Get(L"pack_sync", L"1") == L"0") { GLOGI("packs: pack_sync=0 in gage_mp.cfg, not syncing"); return true; }
    if (!kUpdatePubKey || !*kUpdatePubKey) { GLOGW("packs: no update key baked into this build, not syncing"); return true; }
    const bool required = cfg::Get(L"pack_required", L"1") != L"0";
    PostStatusFromWorker(L"Checking cars…");
    ULONGLONG last = 0;
    const packsync::Result r = packsync::Sync(packsync::DefaultRoot(), UpdateUrls(), kUpdatePubKey, packsync::DefaultIo(),
        [&last](const packsync::Progress& p) {
            const ULONGLONG now = GetTickCount64();
            if (p.totalBytes == 0 || (now - last < 400 && p.doneBytes < p.totalBytes)) return;
            last = now;
            wchar_t b[160];
            swprintf(b, 160, L"Downloading cars: %llu of %llu MB (%d%%)…", p.doneBytes >> 20, (p.totalBytes + (1u << 20) - 1) >> 20,
                     (int)(p.doneBytes * 100 / p.totalBytes));
            PostStatusFromWorker(b);
        });
    if (!r.published) {
        // No error = the server publishes no packs (nothing to require); an error = packs.json was not reachable.
        if (r.error.empty() || !required) {
            GLOGI("packs: no packs.json reachable (%s); starting with the packs already on this PC", r.error.c_str());
            return true;
        }
        GLOGW("packs: packs.json unreachable (%s); launch refused (pack_required=1)", r.error.c_str());
        return false;
    }
    if (!r.ok) {
        GLOGW("packs: %s%s", r.error.c_str(), required ? "; launch refused (pack_required=1)" : "");
        if (!required) PostStatusFromWorker(L"Some cars could not be downloaded. Starting without them…");
        return !required;
    }
    if (r.packsInstalled > 0 || r.packsRemoved > 0) PostStatusFromWorker(L"Cars up to date.");
    return true;
}

static void DoLaunch(const ServerInfo& srv, const std::wstring& gamePath, const std::wstring& playerName) {
    // 0. The lockstep gate (version_gate.h). Ask the server which protocol it enforces: a server AHEAD of this
    //    launcher means a release is out that we have not installed (refuse, and have the UI thread look for the
    //    update now); a server BEHIND has not been deployed yet (refuse, try again in a minute). A server that
    //    does not answer the query is not gated: the engine reports an unreachable server, and the server's own
    //    Refuse(VersionMismatch) is the backstop. Nothing has been written or started yet.
    {
        ServerInfo probe = srv;
        ApplyDefaultPort(probe);
        net::QueryResult q;
        if (probe.port >= 1 && probe.port <= 65535 && net::QueryServer(probe.host, probe.port, 1500, q)) {
            if (q.protocol > 0) PostMessageW(g_hwnd, WM_SERVER_PROTO, (WPARAM)q.protocol, (LPARAM)q.latestClient);
            // The protocol gate, then the update ping: a server released with a client newer than this one (same
            // protocol, like 0.4.3) also refuses PLAY with code 14 until the launcher has updated. An older server
            // (v1 reply) reports latestClient 0 = unknown and is never gated by it.
            const vgate::Gate gate = vgate::DecideServer(q.protocol, (int)proto::kVersion, (uint32_t)q.latestClient,
                                                         ClientVersionPacked(), false);
            if (gate.refuse) {
                GLOGW("launch refused: %s:%d enforces protocol %d (this launcher speaks %d), latest client %s (this launcher is %s)",
                      Narrow(probe.host).c_str(), probe.port, q.protocol, (int)proto::kVersion,
                      VersionText((uint32_t)q.latestClient).c_str(), Narrow(kClientVersion).c_str());
                PostLaunch({ false, gate.code, gate.text });
                return;
            }
        }
    }

    std::wstring dir = ExeDir();
    if (!reg::Set(L"install_dir", dir)) {
        PostLaunch({ false, 6, L"Could not write launcher settings to the registry (install_dir)." });
        return;
    }

    // 1. Resolve the game exe (bridge value > saved setting > the stores' records).
    //    Either a folder (what the SPA stores) or the exe itself is accepted.
    std::wstring game = ResolveGameExe(gamePath);
    if (game.empty()) game = ResolveGameExe(GetGamePath());
    if (game.empty()) game = launch::FindInstalledGame();
    if (game.empty()) {
        PostLaunch({ false, 1, gamePath.empty()
                                   ? L"Could not find GTA5.exe. Set the game folder in Settings."
                                   : L"No GTA5.exe or GTA5_Enhanced.exe in " + gamePath + L"." });
        return;
    }

    // 1b. GAGE-MP's own chain: every DLL it loads into the game must be the
    //     one this build made (gen/chain_hashes.h). Checked before the
    //     download and the game start, so a broken install fails at once.
    std::wstring badFile;
    if (!integrity::VerifyChain(dir, &badFile)) {
        PostLaunch({ false, 11, L"Solar RP's own file " + badFile +
                                    L" is missing or was changed after install. Reinstall Solar RP." });
        return;
    }

    // 1c. The server's car packs (pack_sync.h): Legacy only (GTA5.exe; Enhanced cannot mount them), and only while the
    //     game is not running (a running engine has its packs mounted). Never fatal: without them the game still starts.
    if (_wcsicmp(baseName(game).c_str(), L"GTA5.exe") == 0) {
        if (launch::FindGame().pid) GLOGI("packs: the game is already running; not syncing car packs now");
        else if (!SyncCarPacks()) {
            PostLaunch({ false, 16, L"The server's cars could not be downloaded. Check your connection and press Play again." });
            return;
        }
    }

    // 2. Server handoff (the injected chain reads these from the registry; the
    //    cfg copy is one batched write instead of one file rewrite per key).
    ServerInfo s = srv;
    ApplyDefaultPort(s);
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    if (!gamePath.empty() && ResolveGameExe(gamePath) == game) kv.push_back({ L"game_path", gamePath });
    if (!playerName.empty()) kv.push_back({ L"player_name", playerName });
    kv.push_back({ L"server_host", s.host });
    kv.push_back({ L"server_port", std::to_wstring(s.port) });
    kv.push_back({ L"server_name", s.name });
    // The engine (gage_mp.dll) reads server_host, server_port and player_name
    // from HKCU\Software\GAGE-MP (apps/engine/gage_mp.cpp Run()); it needs
    // nothing else. Written before the game starts so they are set when the
    // engine loads.
    for (const auto& p : kv) {
        if (!reg::Set(p.first, p.second)) {
            PostLaunch({ false, 6, L"Could not write launcher settings to the registry (" + p.first + L")." });
            return;
        }
    }
    if (!cfg::SetMany(kv)) GLOGW("gage_mp.cfg handoff write failed (registry copy is authoritative)");

    // skip_landing is a gage_mp.cfg flag, but the engine reads it from the
    // registry (gage_mp.cpp: reg::Get(L"skip_landing")). Copy it across so the
    // cfg value actually takes effect; best-effort (absence just means the
    // landing page is not auto-skipped and GTA stops at its menu). Default on since 0.3.1: proven
    // live on 3889 (2026-11-01, "landing-page SKIPPED"); a build without the sig skips nothing.
    if (!reg::Set(L"skip_landing", cfg::Get(L"skip_landing", L"1")))
        GLOGW("handoff: could not write skip_landing to the registry");
    // net_session: the engine reads it from the registry too (hook.cpp). Copy the
    // cfg value across so net_session=1 in gage_mp.cfg runs the session recon pass.
    if (!reg::Set(L"net_session", cfg::Get(L"net_session", L"0")))
        GLOGW("handoff: could not write net_session to the registry");
    // net_host: the engine reads it from the registry too (hook.cpp). Copy the cfg
    // value across so net_host=1 in gage_mp.cfg installs the local session-entry stubs.
    if (!reg::Set(L"net_host", cfg::Get(L"net_host", L"0")))
        GLOGW("handoff: could not write net_host to the registry");
    // kill_sp_hijack (default on): the engine terminates the story-character scripts after entry.
    if (!reg::Set(L"kill_sp_hijack", cfg::Get(L"kill_sp_hijack", L"1")))
        GLOGW("handoff: could not write kill_sp_hijack to the registry");
    // no_sp_scripts (default on): the engine stops every stock script thread (FiveM JustNoScript).
    if (!reg::Set(L"no_sp_scripts", cfg::Get(L"no_sp_scripts", L"1")))
        GLOGW("handoff: could not write no_sp_scripts to the registry");
    // peer_drive: how remote players are moved (engine.cpp DrivePeer): "tasks" (default) walks
    // them with locomotion tasks + velocity steering, "place" sets the smoothed position each frame.
    if (!reg::Set(L"peer_drive", cfg::Get(L"peer_drive", L"tasks")))
        GLOGW("handoff: could not write peer_drive to the registry");
    // brand_apparel (default on): the GAGE-MP tee texture swap (engine brand_apparel.h); 0 = stock tee.
    if (!reg::Set(L"brand_apparel", cfg::Get(L"brand_apparel", L"1")))
        GLOGW("handoff: could not write brand_apparel to the registry");
    // net_probe (default off): the read-only G0 network baseline (net_guard.h). Opt-in per run.
    if (!reg::Set(L"net_probe", cfg::Get(L"net_probe", L"0")))
        GLOGW("handoff: could not write net_probe to the registry");

    GLOGI("handoff -> server=%s:%d name='%s' game=%s", Narrow(s.host).c_str(), s.port,
          Narrow(playerName).c_str(), Narrow(game).c_str());

    // 3. The game: attach to a running one, else have its store start it.
    //    Never GTA5.exe directly: the game expects its launcher to start it
    //    (PlayGTAV.exe / steam:// / the Epic launcher, with -nobattleye).
    PostStatusFromWorker(L"Starting GTA V…");
    launch::GameProc g = launch::FindGame();
    bool launched = false;  // true = we started the game; false = it was already running
    if (!g.pid && !g.battleye) {
        std::wstring startErr;
        std::wstring waitWhere;  // where to point the player if the game never shows
        bool started;
        const std::wstring shim = dir + L"gage_launch_shim.dll";
        // Each store disables BattlEye its own way, so route by store (which install
        // owns the folder is detected from the stores' own records, not the path).
        // Every non-Steam store ends up BattlEye-free the same way: the redirect shim
        // ridden into the Rockstar Games Launcher (Launcher.exe), which is what actually
        // spawns the GTA*_BE.exe wrapper. `rideLaunchers` drives that poll-injection
        // (below) independently of HOW the launch is kicked off:
        //  * Steam: launch through Steam with -nobattleye (Steam forwards it to the
        //    game). No shim ride -- launching PlayGTAV ourselves makes it re-launch
        //    THROUGH Steam, spawning the wrapper in a process the shim is not in.
        //  * Epic: launch through the Epic Games Launcher's own URI so PlayGTAV starts
        //    WITH an Epic entitlement token. A bare PlayGTAV has none, so the Rockstar
        //    launcher bounces the real launch back through Epic -- and that bounce pops
        //    an Epic error dialog (the game still starts, but it is confusing). The
        //    authenticated Epic launch avoids the bounce; the shim still rides into
        //    Launcher.exe (Epic -> authenticated PlayGTAV -> Launcher.exe -> GTA5_BE.exe)
        //    for the BattlEye-free spawn.
        //  * Rockstar Games Launcher / standalone disk: ride a bare PlayGTAV (the legit
        //    Rockstar bootstrapper -- no entitlement bounce there) + the Launcher.exe shim.
        // shim_launch=0 forces a plain store launch everywhere (no shim ride).
        const launch::Store store = launch::DetectStore(game);
        const bool shimOn = cfg::Get(L"shim_launch", L"1") != L"0";
        // Every non-Steam store is BattlEye-freed the same way: the shim ridden into
        // Launcher.exe (the poll below). Steam is handled by -nobattleye through Steam;
        // shim_launch=0 forces a plain store launch everywhere.
        const bool rideLaunchers = shimOn && store != launch::Store::Steam;
        if (rideLaunchers && GetFileAttributesW(shim.c_str()) == INVALID_FILE_ATTRIBUTES) {
            PostLaunch({ false, 8, L"gage_launch_shim.dll not found next to SolarLauncher.exe." });
            return;
        }
        if (rideLaunchers) {
            // The shim reads these from HKCU\Software\GAGE-MP: redirect the BattlEye
            // wrapper (matched by shape, so shim_redirect_from stays empty) to the real
            // game exe. Set here because either launch path below rides the same shim.
            reg::Set(L"shim_redirect_from", L"");
            reg::Set(L"shim_redirect_to", game);
            PostStatusFromWorker(L"Starting GTA V (BattlEye-free)…");
            // Prefer the Epic Games Launcher's authenticated URI: it starts PlayGTAV WITH
            // an Epic entitlement token. A bare PlayGTAV has none, so the Rockstar launcher
            // bounces the launch back through Epic, which pops an "UNABLE TO LAUNCH GAME,
            // PLEASE TRY REINSTALLING" dialog (the game still starts, but it alarms the
            // player). The authenticated launch avoids that bounce; the shim still rides
            // into Launcher.exe (the poll below) for the BattlEye-free GTA*_BE.exe spawn.
            // Pure Rockstar/disk copies have no Epic manifest, so StartThroughStore(Epic)
            // returns false and we fall back to the bare PlayGTAV ride (no bounce there).
            // epic_launch=0 forces the bare ride for Epic copies too.
            const bool epicPref = cfg::Get(L"epic_launch", L"1") != L"0";
            started = epicPref && launch::StartThroughStore(game, launch::Store::Epic, &startErr);
            if (started) {
                waitWhere = L"the Epic Games Launcher";
            } else {
                startErr.clear();
                started = launch::StartViaShim(game, shim, &startErr);
                waitWhere = L"PlayGTAV";
            }
        } else {
            const std::wstring storeName = launch::StoreName(store);
            PostStatusFromWorker(L"Starting GTA V through " + storeName + L"…");
            started = launch::StartThroughStore(game, store, &startErr);
            waitWhere = storeName;
        }
        if (!started) {
            PostLaunch({ false, 8, startErr });
            return;
        }
        launched = true;
        // The store/launcher may update, sign in or show its own window first. A
        // status every few seconds also keeps the SPA's no-response timer from firing.
        // While waiting, the shim also has to reach the Rockstar Games Launcher
        // (Launcher.exe) — on Epic that, not PlayGTAV, is what spawns GTA5_BE.exe,
        // in a separate process tree — so inject it into every launcher that
        // appears during the wait (there is a ~16 s gap before the game spawns).
        std::vector<DWORD> injectedLaunchers;
        DWORD t0 = GetTickCount(), lastNote = 0;
        for (;;) {
            Sleep(500);
            if (rideLaunchers) launch::InjectShimIntoLaunchers(shim, injectedLaunchers);
            g = launch::FindGame();
            if (g.pid || g.battleye) break;
            DWORD waited = GetTickCount() - t0;
            if (waited >= kGameStartWaitMs) break;
            if (waited - lastNote >= 5000) {
                lastNote = waited;
                PostStatusFromWorker(L"Waiting for GTA V to start (" + std::to_wstring(waited / 1000) + L" s)…");
            }
        }
        if (!g.pid && !g.battleye) {
            PostLaunch({ false, 9, L"GTA V did not start within 3 minutes. Check " + waitWhere + L", then press Connect again." });
            return;
        }
    }
    // Never load into a BattlEye session: it is GTA Online's anti-cheat.
    if (g.battleye) {
        GLOGW("launch: BattlEye is running; not loading into the game");
        PostLaunch({ false, 7, L"GTA V is running with BattlEye, so Solar RP did not load into it. Close the game, turn "
                               L"BattlEye off (Rockstar Games Launcher settings, or add -nobattleye to the launch options "
                               L"in Steam or Epic), then press Connect again." });
        return;
    }
    GLOGI("launch: game %s pid %lu, parent %s (%s)", Narrow(g.image).c_str(), (unsigned long)g.pid,
          g.parentImage.empty() ? "(gone)" : Narrow(g.parentImage).c_str(),
          launched ? "started through its store" : "already running");

    // Diagnostic switch: `inject=0` in gage_mp.cfg starts the game through its
    // store but loads nothing into it, to tell a launch problem from one the
    // injected DLLs cause.
    if (cfg::Get(L"inject", L"1") == L"0") {
        GLOGW("launch: inject=0 in gage_mp.cfg: game left without Solar RP (diagnostic)");
        HANDLE track = launched ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g.pid) : 0;
        LaunchResult r = { true, 0, L"" };
        if (track) { r.hProc = track; r.pid = g.pid; WatchGameExit(track, g.pid); }
        PostStatusFromWorker(L"Game started without Solar RP (inject=0).");
        PostLaunch(r);
        return;
    }
    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g.pid);
    if (!h) {
        DWORD openErr = GetLastError();
        PostLaunch({ false, 2, openErr == ERROR_ACCESS_DENIED
                                   ? L"The game is running with higher privileges. Close it, or start Solar Launcher as administrator."
                                   : L"Could not open the game process: " + FormatError(openErr) });
        return;
    }
    DWORD pid = g.pid;
    if (!reg::Set(L"inject_target", std::to_wstring(pid))) {
        // A game we just started stays tracked so close_game still reaches it.
        LaunchResult r = { false, 6, L"Could not write launcher settings to the registry (inject_target)." };
        if (launched) { r.hProc = h; r.pid = pid; } else CloseHandle(h);
        PostLaunch(r);
        return;
    }

    std::wstring injector = dir + L"gageinjector.dll";
    if (!FileExists(injector)) {
        LaunchResult r = { false, 3, L"gageinjector.dll not found next to SolarLauncher.exe." };
        if (launched) { r.hProc = h; r.pid = pid; } else CloseHandle(h);
        PostLaunch(r);
        return;
    }

    // InjectDll waits (up to 10 s) for the new process to expose kernel32 before it injects.
    // Player-facing wording; the mechanics (pid, injector) go to gage.log.
    PostStatusFromWorker(L"Loading Solar RP into the game…");
    DWORD err = 0;
    DWORD tInject = GetTickCount();
    bool ok = inject::InjectDll(h, injector, 30000, &err);
    DWORD exitCode = 0;
    bool alive = GetExitCodeProcess(h, &exitCode) && exitCode == STILL_ACTIVE;
    GLOGI("launch: inject %s in %lu ms (err=0x%08lX), game %s", ok ? "ok" : "FAILED",
          (unsigned long)(GetTickCount() - tInject), (unsigned long)err,
          alive ? "running" : ("exited, code " + std::to_string(exitCode)).c_str());

    // The UI thread tracks the game for close_game when we started it, or when
    // we injected into it. A pre-existing game we failed to inject is left alone.
    LaunchResult r = { ok, ok ? 0 : 4, L"" };
    if (alive) WatchGameExit(h, pid);
    if (launched || ok) { r.hProc = h; r.pid = pid; } else CloseHandle(h);

    if (ok) {
        // The engine (gage_mp.dll) performs the join after injection: it reads
        // the target from HKCU\Software\GAGE-MP and runs the GAGE protocol
        // session in the background (join.cpp). Say only that injection succeeded.
        GLOGI("injected into pid %lu (engine will join %s:%d)", (unsigned long)pid,
              Narrow(s.host).c_str(), s.port);
    } else {
        // InjectDll leaves err at 0 on several early failures; don't print
        // "The operation completed successfully." as the reason.
        r.error = err == inject::kErrTargetNotReady
                      ? L"Injection failed: the game process did not finish starting within 10 seconds."
                  : err == inject::kErrRemoteTimeout
                      ? L"Injection failed: the game did not load the injector within 30 seconds."
                  : err ? L"Injection failed: " + FormatError(err)
                        : L"Injection failed (the game process refused the loader).";
    }
    PostLaunch(r);
}

static ServerInfo FromJServer(const JVal* sv) {
    ServerInfo s;
    if (!sv || sv->t != JVal::Obj) return s;
    s.host = jStr(jGet(sv, "host"));
    s.port = (int)jNum(jGet(sv, "port"), 0);
    s.name = jStr(jGet(sv, "name"));
    s.players = (int)jNum(jGet(sv, "players"), -1);
    s.ping = (int)jNum(jGet(sv, "ping"), -1);
    s.url = jStr(jGet(sv, "url"));
    if (s.name.empty()) s.name = s.host;
    return s;
}

// ---------------------------------------------------------------------------
// Server list / ping / update workers
// ---------------------------------------------------------------------------

// gen = the fetch generation it belongs to (never overloaded as an error flag:
// an error result must still match g_gen, or the SPA never leaves "loading").
struct FetchResult { int gen; bool failed = false; std::vector<ServerInfo> servers; std::wstring error; };
struct PingResult { int gen; int row; std::wstring key; int ms; };
struct PingHit { int row; std::wstring key; int ms; };
struct PingBatch { int gen; std::vector<PingHit> hits; };
static void KickFetch() {
    int gen = ++g_gen;
    PostServers("loading", L"");
    // Resolve the endpoint here on the UI thread; the worker only does network I/O.
    std::wstring url = GetListOverride();
    if (url.empty()) url = kDefaultListUrl;
    else GLOGI("server list: using list_url from gage_mp.cfg");
    FetchResult* r = new FetchResult();
    r->gen = gen;
    std::thread([r, url]() {
        std::wstring err;
        bool ok = net::ListServers(url, r->servers, &err);
        if (!ok || !err.empty()) {
            r->failed = true;
            r->error = err.empty() ? L"Could not reach the server list." : err;
            GLOGW("server list fetch failed: %s (%s)", Narrow(r->error).c_str(), Narrow(url).c_str());
        }
        if (!PostMessageW(g_hwnd, WM_FETCH_DONE, 0, (LPARAM)r)) delete r;
    }).detach();
}

static void StartPing(int row) {
    if (row < 0 || row >= (int)g_servers.size()) {
        // no such row: answer "no reply" so the SPA's slot frees at once
        PostJson("{\"type\":\"ping\",\"row\":" + numStr(row) + ",\"ms\":-1}");
        return;
    }
    std::wstring host = g_servers[row].host;
    int port = g_servers[row].port;
    PingResult* r = new PingResult();
    r->gen = g_gen;
    r->row = row;
    r->key = KeyOf(g_servers[row]);  // the row may point at a different server once a new list lands
    std::thread([r, host, port]() {
        r->ms = net::PingServer(host, port, 1200);
        if (!PostMessageW(g_hwnd, WM_PING_DONE, 0, (LPARAM)r)) delete r;
    }).detach();
}

// After each fetch: ping the whole list at once, busiest servers first, and
// stream the replies to the SPA in batches (net::PingMany: ~0.3 s for the
// 64-server master list once its hosts know us, ~1.5 s on first contact,
// measured 2026-10-25). A newer fetch stops it.
static const size_t kAutoPingMax = 250;

static void StartPingAll() {
    const int gen = g_gen;
    std::vector<int> order(g_servers.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(),
        [](int a, int b) { return g_servers[a].players > g_servers[b].players; });
    if (order.size() > kAutoPingMax) order.resize(kAutoPingMax);
    std::vector<net::PingTarget> targets;
    std::vector<std::wstring> keys;
    for (int row : order) {
        targets.push_back({g_servers[row].host, g_servers[row].port, true});
        keys.push_back(KeyOf(g_servers[row]));  // rows may move once a new list lands
    }
    if (targets.empty()) return;
    std::thread([gen, order = std::move(order), targets = std::move(targets), keys = std::move(keys)]() {
        net::PingMany(targets, 1200, [gen] { return g_gen.load() != gen; },
            [&](const std::vector<std::pair<int, int>>& results) {
                PingBatch* b = new PingBatch();
                b->gen = gen;
                for (const auto& [i, ms] : results) b->hits.push_back({order[i], keys[i], ms});
                if (!PostMessageW(g_hwnd, WM_PING_BATCH, 0, (LPARAM)b)) delete b;
            });
    }).detach();
}

// ---------------------------------------------------------------------------
// Self-update (apps/launcher/updater.h, docs/UPDATES.md). All state lives on the UI thread;
// workers only fetch/verify/download and post the outcome. The flow is
//   check -> download + verify (every available update, whatever autoUpdate says) -> install when no game runs
//   with our DLL. Lockstep releases: the policy (mandatory for all, the protocol gate, when to check) is
//   apps/launcher/version_gate.h.
// ---------------------------------------------------------------------------

enum class UpState { Idle, Checking, Downloading, Verifying, Ready, Installing, Deferred, Error };

static const char* UpStateName(UpState s) {
    switch (s) {
        case UpState::Idle: return "idle";
        case UpState::Checking: return "checking";
        case UpState::Downloading: return "downloading";
        case UpState::Verifying: return "verifying";
        case UpState::Ready: return "ready";
        case UpState::Installing: return "installing";
        case UpState::Deferred: return "deferred";
        case UpState::Error: return "error";
    }
    return "idle";
}

struct UpdateView {
    UpState state = UpState::Idle;
    double progress = 0;        // 0..1, meaningful while downloading
    bool available = false;     // a verified manifest names a strictly newer version
    bool mandatory = false;     // == available (vgate::UpdateIsMandatory: every update is mandatory): PLAY is refused until it is installed
    bool started = false;       // the first check of this process has been kicked
    bool installerStarted = false;  // setup is running; the launcher is about to exit
    std::wstring latest;
    std::wstring error;
    std::string notes;          // UTF-8, from the signed payload
    updater::Manifest man;
    updater::Installer installer;  // verified, held open (FILE_SHARE_READ only) until setup starts
};
static UpdateView g_up;
// When the last check STARTED (GetTickCount64), for the minimum gap between event-driven checks (version_gate.h).
static ULONGLONG g_updateCheckStartedAt = 0;
static bool g_updateCheckEver = false;

struct UpdateCheckDone { updater::CheckResult result; };
struct UpdateDlDone { bool ok = false; std::string why; updater::Installer inst; };

// {"type":"update",...}: the BRIDGE.md message; sent on every state or progress change.
static void PostUpdate() {
    char prog[16];
    snprintf(prog, sizeof(prog), "%.3f", g_up.progress);
    PostJson("{\"type\":\"update\",\"current\":" + jStrW(kClientVersion) +
             ",\"latest\":" + jStrW(g_up.latest) +
             ",\"available\":" + std::string(g_up.available ? "true" : "false") +
             ",\"error\":" + jStrW(g_up.error) +
             ",\"state\":\"" + UpStateName(g_up.state) + "\"" +
             ",\"progress\":" + prog +
             ",\"mandatory\":" + std::string(g_up.mandatory ? "true" : "false") +
             ",\"notes\":" + jEsc(g_up.notes) + "}");
}

// The manifests to try, in order: the URL a signed manifest moved us to (HKCU update_url), then
// the baked one.
static std::vector<std::string> UpdateUrls() {
    std::vector<std::string> urls;
    const std::string stored = Narrow(reg::Get(L"update_url"));
    if (!stored.empty()) {
        if (updater::ValidHttpUrl(stored, updater::kMaxUpdateUrlChars)) urls.push_back(stored);
        else GLOGW("update: ignoring the invalid update_url in the registry");
    }
    urls.push_back(Narrow(kDefaultVersionUrl));
    return urls;
}

static bool GameHasOurDll(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return true;  // cannot look inside: assume it is ours
    HMODULE mods[1024];
    DWORD needed = 0;
    bool found = false;
    if (!EnumProcessModulesEx(h, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
        CloseHandle(h);
        return true;
    }
    const size_t n = std::min<size_t>(needed / sizeof(HMODULE), sizeof(mods) / sizeof(mods[0]));
    for (size_t i = 0; i < n && !found; ++i) {
        wchar_t path[MAX_PATH] = L"";
        if (!GetModuleFileNameExW(h, mods[i], path, MAX_PATH)) continue;
        const std::wstring b = baseName(path);
        found = _wcsicmp(b.c_str(), L"gage_mp.dll") == 0 || _wcsicmp(b.c_str(), L"gage_loader.dll") == 0 ||
                _wcsicmp(b.c_str(), L"gageinjector.dll") == 0;
    }
    CloseHandle(h);
    return found;
}

// True (and *why) while replacing the launcher's files could hit a game that has them loaded:
// a launch in progress, the game this launcher started or injected still running, or any
// GTA V process with a GAGE-MP DLL in it.
static bool GameBlocksUpdate(std::string* why) {
    if (g_launching) { *why = "a launch is in progress"; return true; }
    if (g_gameProc && WaitForSingleObject(g_gameProc, 0) == WAIT_TIMEOUT) { *why = "the game Solar RP started is running"; return true; }
    const launch::GameProc gp = launch::FindGame();
    if (gp.pid && GameHasOurDll(gp.pid)) { *why = "GTA V (pid " + std::to_string(gp.pid) + ") is running with Solar RP loaded"; return true; }
    return false;
}

static void FailUpdate(const std::string& why) {
    GLOGW("update: %s", why.c_str());
    g_up.installer.Close();
    g_up.state = UpState::Error;
    g_up.error = Widen(why);
    PostUpdate();
}

// Verified installer in hand: start it unless a game has our DLL loaded (then Deferred, retried
// when that game exits and every kUpdateRetryMs).
static void TryInstallUpdate() {
    if (!g_up.installer.Open() || g_up.installerStarted) return;
    if (g_up.state != UpState::Ready && g_up.state != UpState::Deferred) return;
    std::string why;
    if (GameBlocksUpdate(&why)) {
        if (g_up.state != UpState::Deferred) GLOGI("update: %s; installing %s when it ends", why.c_str(), Narrow(g_up.latest).c_str());
        g_up.state = UpState::Deferred;
        SetTimer(g_hwnd, kTimerUpdateRetry, kUpdateRetryMs, nullptr);
        PostUpdate();
        return;
    }
    KillTimer(g_hwnd, kTimerUpdateRetry);
    g_up.state = UpState::Installing;
    g_up.error.clear();
    PostUpdate();
    const std::wstring path = g_up.installer.path;
    const std::wstring log = path.substr(0, path.find_last_of(L'\\')) + L"\\" + updater::InstallerLogName();
    unsigned long pid = 0;
    if (!updater::StartInstaller(g_up.installer, log, &pid, &why)) {
        FailUpdate(why);
        return;
    }
    g_up.installerStarted = true;
    GLOGI("update: started %s (pid %lu, log %s); Solar Launcher exits so setup can replace it", Narrow(path).c_str(), pid,
          Narrow(log).c_str());
    SetTimer(g_hwnd, kTimerUpdateExit, kUpdateExitDelayMs, nullptr);
}

static void StartUpdateDownload() {
    std::string why;
    const std::wstring dir = updater::ResolveUpdatesDir(ExeDir(), &why);
    if (dir.empty()) { FailUpdate(why); return; }
    g_up.state = UpState::Downloading;
    g_up.progress = 0;
    g_up.error.clear();
    PostUpdate();
    GLOGI("update: downloading %s (%llu bytes) to %s", g_up.man.version.c_str(), g_up.man.size, Narrow(dir).c_str());
    const updater::Manifest m = g_up.man;
    std::thread([m, dir]() {
        UpdateDlDone* d = new UpdateDlDone();
        int lastPct = -1;
        d->ok = updater::Download(m, dir,
            [&lastPct](double f) {
                const int permille = (int)(f * 1000.0);
                if (permille / 10 == lastPct) return;  // one post per percent
                lastPct = permille / 10;
                PostMessageW(g_hwnd, WM_UPDATE_PROGRESS, (WPARAM)permille, 0);
            },
            []() { PostMessageW(g_hwnd, WM_UPDATE_PROGRESS, 1000, 1); }, d->inst, &d->why);
        if (!PostMessageW(g_hwnd, WM_UPDATE_DL_DONE, 0, (LPARAM)d)) delete d;
    }).detach();
}

static void ResumeParkedLaunch();  // PLAY waiting for a check (defined with the bridge handlers)

// The result of a check, applied to g_up (UI thread).
static void ApplyUpdateCheck(const updater::CheckResult& c) {
    if (!c.migrateUrl.empty()) {
        const std::wstring w = Widen(c.migrateUrl);
        if (reg::Get(L"update_url") != w) {
            if (reg::Set(L"update_url", w)) GLOGI("update: manifest URL moved to %s (updateUrl in the signed manifest)", c.migrateUrl.c_str());
            else GLOGW("update: could not store the new manifest URL %s", c.migrateUrl.c_str());
        }
    }
    if (!c.ok) {
        g_up.state = UpState::Error;
        if (!g_up.mandatory) {  // a mandatory update learned earlier stays pending when a re-check fails
            g_up.available = false;
            g_up.latest.clear();
        }
        g_up.error = c.error.empty() ? L"Could not reach the update server." : Widen(c.error);
        PostUpdate();
        return;
    }
    g_up.latest = Widen(c.manifest.version);
    g_up.available = c.available;
    g_up.notes = c.manifest.notes;
    // Lockstep releases: EVERY available update is mandatory, whatever the signed `mandatory` flag or the
    // "install updates automatically" setting say (version_gate.h rule 1). PLAY is refused until the launcher
    // runs the newest version; the download starts now and setup runs as soon as no game has our DLLs loaded.
    g_up.mandatory = vgate::UpdateIsMandatory(c.available, c.manifest.mandatory);
    g_up.error.clear();
    if (!c.available) {
        g_up.state = UpState::Idle;
        PostUpdate();
        return;
    }
    g_up.man = c.manifest;
    if (!c.manifest.mandatory || !GetAutoUpdate())
        GLOGI("update: %s is available and is installed now (every update is mandatory; manifest mandatory=%d, autoUpdate=%d)",
              c.manifest.version.c_str(), c.manifest.mandatory ? 1 : 0, GetAutoUpdate() ? 1 : 0);
    StartUpdateDownload();
}

// A check has finished (UI thread): apply it, then let a PLAY that was waiting for it go on.
static void OnUpdateChecked(const updater::CheckResult& c) {
    ApplyUpdateCheck(c);
    ResumeParkedLaunch();
}

// Check the manifest(s) now. While any step of an earlier pass is still going (or waiting for the
// game to close) this only reports where it stands.
static void KickUpdate() {
    g_up.started = true;
    switch (g_up.state) {
        case UpState::Checking: case UpState::Downloading: case UpState::Verifying:
        case UpState::Ready: case UpState::Installing: case UpState::Deferred:
            PostUpdate();
            return;
        case UpState::Idle: case UpState::Error: break;
    }
    g_up.state = UpState::Checking;
    g_up.error.clear();
    g_updateCheckStartedAt = GetTickCount64();
    g_updateCheckEver = true;
    PostUpdate();
    const std::vector<std::string> urls = UpdateUrls();
    static bool cleaned = false;
    const bool clean = !cleaned;
    cleaned = true;
    std::thread([urls, clean]() {
        if (clean) {  // leftovers of an earlier update (a finished setup, an interrupted download)
            updater::CleanUpdatesDirs(ExeDir());
        }
        UpdateCheckDone* r = new UpdateCheckDone();
        r->result = updater::Check(urls, kUpdatePubKey, Narrow(kClientVersion), updater::DefaultFetch());
        if (!PostMessageW(g_hwnd, WM_UPDATE_DONE, 0, (LPARAM)r)) delete r;
    }).detach();
}

// A check can start only when none is under way and no verified installer is waiting.
static bool UpdateCanCheck() { return g_up.state == UpState::Idle || g_up.state == UpState::Error; }
static bool UpdateCheckDue() { return vgate::CheckDue(GetTickCount64(), g_updateCheckStartedAt, g_updateCheckEver); }

// The event-driven check: the launcher window was brought forward, the game exited, a server reported a
// protocol ahead of ours. Starts a check unless one is under way, an installer is already waiting, or the last
// check started less than vgate::kMinCheckGapMs ago. True when a check was started. (The 2-minute poll, the first
// check at start and the SPA's own `checkUpdate` call KickUpdate directly.)
static bool KickUpdateLimited(const char* why) {
    if (!UpdateCanCheck() || !UpdateCheckDue()) return false;
    GLOGI("update: checking now (%s)", why);
    KickUpdate();
    return g_up.state == UpState::Checking;
}

// PLAY is refused while a mandatory update is pending or setup is already starting. Fills the
// player-facing text; a failed mandatory update is re-tried by the press itself.
static bool UpdateBlocksLaunch(std::wstring* text) {
    if (g_up.installerStarted || g_up.state == UpState::Installing) {
        *text = L"Solar Launcher is restarting to install an update. Try again in a moment.";
        return true;
    }
    if (!(g_up.available && g_up.mandatory)) return false;
    *text = L"Solar Launcher " + g_up.latest + L" is required to play.";
    switch (g_up.state) {
        case UpState::Deferred: *text += L" It installs when GTA V is closed."; break;
        case UpState::Error: *text += L" The update failed (" + g_up.error + L"); trying again."; break;
        default: *text += L" It is being installed now."; break;
    }
    if (g_up.state == UpState::Error) KickUpdate();
    return true;
}

// Real server status (demo mode): the SPA's live server (servers.ts) is the GAGE VPS, whose
// counts and ping a TCP ping cannot read (ENet/UDP). A background thread sends the real GAGE
// UDP query (net::QueryServer) to the test server every few seconds and marshals the result to
// the SPA as {"type":"serverStatus",...}, which the UI overlays onto the matching server,
// replacing the fabricated demo counts. A configured platform API supplies its own status, so
// this runs in demo mode only. Started once from the SPA's `ready`; the thread lives with the
// process (detached), and a post to a gone window simply drops (PostJsonFromWorker).
static const int kServerStatusPeriodMs = 5000;
static void StartServerStatusWorker() {
    if (!GetApiBase().empty()) return;   // platform mode: status comes from the API
    ServerInfo test;
    if (!GetTestServer(&test)) return;   // nothing live to query in this build
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;  // one worker for the process, survives SPA reloads
    const std::wstring host = test.host;
    const int port = test.port;
    GLOGI("server status: polling %s:%d by UDP query every %d ms", Narrow(host).c_str(), port, kServerStatusPeriodMs);
    std::thread([host, port]() {
        bool firstResult = true, lastOnline = false;
        for (;;) {
            net::QueryResult q;
            const bool ok = net::QueryServer(host, port, 1500, q);
            // Log the first result and every online<->offline flip (not every poll),
            // so the log proves the real ping without reading the UI.
            if (firstResult || ok != lastOnline) {
                if (ok)
                    GLOGI("server status: %s:%d online — %d/%d players, %d ms (proto %d)",
                          Narrow(host).c_str(), port, q.players, q.maxPlayers, q.ms, q.protocol);
                else
                    GLOGW("server status: %s:%d not answering (no UDP reply within 1500 ms)",
                          Narrow(host).c_str(), port);
                firstResult = false;
                lastOnline = ok;
            }
            std::string json = "{\"type\":\"serverStatus\",\"servers\":[{"
                "\"host\":" + jStrW(host) +
                ",\"port\":" + numStr(port) +
                ",\"online\":" + std::string(ok ? "true" : "false") +
                ",\"pingMs\":" + numStr(ok ? q.ms : -1) +
                ",\"players\":" + numStr(ok ? q.players : -1) +
                ",\"maxPlayers\":" + numStr(ok ? q.maxPlayers : -1) +
                ",\"protocol\":" + numStr(ok ? q.protocol : -1) +
                "}]}";
            PostJsonFromWorker(json);
            // The UI thread compares the protocol and the latest client the server reports (the update ping) with ours;
            // a server ahead of this launcher in either starts an update check (rate limited there).
            if (ok && q.protocol > 0 && g_hwnd) PostMessageW(g_hwnd, WM_SERVER_PROTO, (WPARAM)q.protocol, (LPARAM)q.latestClient);
            Sleep(kServerStatusPeriodMs);
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// Bridge: JS -> native (commands from apps/launcher/BRIDGE.md)
// ---------------------------------------------------------------------------

// GTA5.exe: store the folder (the SPA's convention; ResolveGameExe prefers
// GTA5.exe there). GTA5_Enhanced.exe: store the exact exe, otherwise a folder
// that also holds GTA5.exe would silently launch the other edition.
static void SaveGameExe(const std::wstring& exe) {
    size_t i = exe.find_last_of(L"\\/");
    std::wstring dir = i == std::wstring::npos ? L"." : exe.substr(0, i);
    bool enhanced = _wcsicmp(baseName(exe).c_str(), L"GTA5_Enhanced.exe") == 0;
    SaveSetting(L"gamePath", enhanced ? exe : dir, false);
}

// No game folder set (first run, or the player cleared it): fill it from the
// stores' records so Settings shows the folder PLAY will use. The player can
// still change it; when nothing is found, Settings asks for it.
static void DetectGamePathIfUnset() {
    if (!GetGamePath().empty()) return;
    std::wstring exe = launch::FindInstalledGame();
    if (exe.empty()) {
        GLOGW("game path: not set and not found in the Rockstar, Steam or Epic records");
        return;
    }
    GLOGI("game path: not set; using the detected %s", Narrow(exe).c_str());
    SaveGameExe(exe);
}

static void OnBrowse() {
    OPENFILENAMEW ofn = {};
    wchar_t file[MAX_PATH * 2] = L"GTA5.exe";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"GTA V executable\0GTA5.exe;GTA5_Enhanced.exe\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    // NOCHANGEDIR: the game is later started with the launcher's cwd.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    std::wstring exe = file;
    if (!IsGameExeName(exe)) {
        // The filter offers "All files"; only the two game executables are usable.
        GLOGW("browse: '%s' is not GTA5.exe / GTA5_Enhanced.exe; setting unchanged", Narrow(baseName(exe)).c_str());
        MessageBoxW(g_hwnd, L"Please select GTA5.exe or GTA5_Enhanced.exe.", L"Solar Launcher", MB_ICONINFORMATION | MB_OK);
        PostSettings();
        return;
    }
    SaveGameExe(exe);
    PostSettings();
}

// ---------------------------------------------------------------------------
// Session token (tokenGet / tokenSet / tokenClear): the platform session the
// SPA logged in with, kept only here, DPAPI-encrypted for this Windows user in
// %LOCALAPPDATA%\GAGE-MP\session.bin. The token itself is never logged.
// ---------------------------------------------------------------------------

static const size_t kMaxSessionToken = 512;

static std::wstring SessionFile() {
    wchar_t base[MAX_PATH] = L"";
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(base) + L"\\GAGE-MP\\session.bin";
}

// Printable ASCII only: what the platform issues, and nothing that could
// break the JSON or a header.
static bool ValidSessionToken(const std::string& t) {
    if (t.empty() || t.size() > kMaxSessionToken) return false;
    for (unsigned char c : t)
        if (c < 0x21 || c > 0x7E) return false;
    return true;
}

static bool StoreSessionToken(const std::string& token) {
    if (!ValidSessionToken(token)) {
        GLOGW("tokenSet: refused (empty, over %zu characters, or not printable ASCII)", kMaxSessionToken);
        return false;
    }
    std::wstring file = SessionFile();
    if (file.empty()) {
        GLOGE("tokenSet: LOCALAPPDATA is not set");
        return false;
    }
    CreateDirectoryW(file.substr(0, file.find_last_of(L'\\')).c_str(), nullptr);
    DATA_BLOB in = { (DWORD)token.size(), (BYTE*)token.data() };
    DATA_BLOB out = {};
    if (!CryptProtectData(&in, L"GAGE-MP session", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        GLOGE("tokenSet: CryptProtectData failed (%lu)", GetLastError());
        return false;
    }
    std::wstring tmp = file + L".tmp";
    bool ok = false;
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(h, out.pbData, out.cbData, &written, nullptr) && written == out.cbData && FlushFileBuffers(h);
        CloseHandle(h);
        ok = ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) DeleteFileW(tmp.c_str());
    }
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (!ok) GLOGE("tokenSet: could not write %s (%lu)", Narrow(file).c_str(), GetLastError());
    return ok;
}

// "" when no token is stored, or the file is unreadable, foreign or damaged.
static std::string LoadSessionToken() {
    std::wstring file = SessionFile();
    if (file.empty()) return "";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string blob(16 * 1024, '\0');
    DWORD got = 0;
    BOOL read = ReadFile(h, &blob[0], (DWORD)blob.size(), &got, nullptr);
    CloseHandle(h);
    if (!read || got == 0 || got == blob.size()) {
        GLOGW("tokenGet: %s is empty or oversized; ignored", Narrow(file).c_str());
        return "";
    }
    DATA_BLOB in = { got, (BYTE*)blob.data() };
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        GLOGW("tokenGet: %s could not be decrypted for this user (%lu); ignored", Narrow(file).c_str(), GetLastError());
        return "";
    }
    std::string token((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (!ValidSessionToken(token)) {
        GLOGW("tokenGet: stored token is malformed; ignored");
        return "";
    }
    return token;
}

// True when no token is stored afterwards (also when none ever was).
static bool ClearSessionToken() {
    std::wstring file = SessionFile();
    if (file.empty()) return false;
    if (DeleteFileW(file.c_str())) return true;
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
    GLOGE("tokenClear: could not delete %s (%lu)", Narrow(file).c_str(), e);
    return false;
}

// openExternal: only https:// URLs (checkout pages, Discord's authorize page),
// at most 2048 characters, no spaces or control characters; opened in the
// player's default browser.
static bool OpenExternalUrl(const std::wstring& url) {
    if (url.size() <= 8 || url.size() > 2048 || _wcsnicmp(url.c_str(), L"https://", 8) != 0) {
        GLOGW("openExternal: refused a non-https or oversized URL (%zu characters)", url.size());
        return false;
    }
    for (wchar_t c : url)
        if (c <= 0x20 || c == 0x7F) {
            GLOGW("openExternal: refused a URL with a space or control character");
            return false;
        }
    INT_PTR r = (INT_PTR)ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (r <= 32) GLOGE("openExternal: ShellExecute failed (%ld)", (long)r);
    return r > 32;
}

// The menu's "Copy address": plain text on the Windows clipboard. Native, so
// it never depends on the page's clipboard permission. Capped: the bridge
// copies addresses, not documents.
static bool CopyToClipboard(const std::wstring& text) {
    if (text.empty() || text.size() > 512) {
        GLOGW("copyText: refused %zu characters", text.size());
        return false;
    }
    if (!OpenClipboard(g_hwnd)) {
        GLOGW("copyText: OpenClipboard failed (err=%lu)", GetLastError());
        return false;
    }
    bool ok = false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            // on success the clipboard owns `mem`
            ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) {
            GLOGW("copyText: SetClipboardData failed (err=%lu)", GetLastError());
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

// PLAY that is waiting for the update check it needs (one at a time; a second press is ignored like a second
// press during a launch). Resumed by the check's result, or by kTimerLaunchWait if the check is slow.
struct ParkedLaunch { bool active = false; ServerInfo s; std::wstring gp, pn; };
static ParkedLaunch g_parked;

// The `launch` command (UI thread). Order: a pending update refuses PLAY (code 14); else, if the launcher has
// not looked for an update in the last minute, look now and wait for the answer (at most
// kLaunchCheckWaitMs), so a release published a minute ago is never skipped; then the demo/test-server
// substitution, the address check, and the launch worker, whose first step is the protocol gate (DoLaunch).
// `afterCheck`: this press already waited for its check; do not wait again.
static void HandleLaunch(ServerInfo s, const std::wstring& gp, const std::wstring& pn, bool afterCheck) {
    std::wstring updateText;
    if (UpdateBlocksLaunch(&updateText)) {
        GLOGW("launch refused: update pending (%s)", Narrow(updateText).c_str());
        PostLaunchResult(false, vgate::kCodeUpdate, updateText);
        return;
    }
    if (!afterCheck && !g_launching) {
        const vgate::PlayStep step = vgate::BeforePlay(g_up.state == UpState::Checking, UpdateCanCheck(), UpdateCheckDue());
        if (step != vgate::PlayStep::Go) {
            if (g_parked.active) {
                GLOGW("launch ignored: PLAY is already waiting for the update check");
                return;
            }
            if (step == vgate::PlayStep::CheckThenGo) KickUpdateLimited("PLAY pressed");
            if (g_up.state == UpState::Checking) {
                g_parked.active = true;
                g_parked.s = s;
                g_parked.gp = gp;
                g_parked.pn = pn;
                SetTimer(g_hwnd, kTimerLaunchWait, kLaunchCheckWaitMs, nullptr);
                PostStatus(L"Checking for updates…");
                return;
            }
        }
    }
    if (GetApiBase().empty()) {
        // Demo mode (no platform API configured): the server list is demo
        // data. A `test_server=host[:port]` line in gage_mp.cfg sends PLAY
        // straight to that server instead (testing before the platform is up).
        ServerInfo test;
        if (!GetTestServer(&test)) {
            GLOGW("launch refused: demo mode, no api_base and no valid test_server in gage_mp.cfg");
            PostLaunchResult(false, 13, L"No Solar RP server is set up yet.");
            return;
        }
        GLOGI("launch: demo mode, joining test_server %s:%d instead of '%s'", Narrow(test.host).c_str(), test.port,
              Narrow(s.name).c_str());
        s = test;
    }
    if (s.host.empty() || s.port < 0 || s.port > 65535) {
        PostLaunchResult(false, 5, L"Invalid server address.");
        return;
    }
    if (g_launching.exchange(true)) {
        GLOGW("launch ignored: a launch is already in progress");
        return;
    }
    PostStatus(L"Connecting…");
    std::thread([s, gp, pn]() { DoLaunch(s, gp, pn); }).detach();
}

// The check PLAY was waiting for has finished, or kTimerLaunchWait ran out: go on with that press.
static void ResumeParkedLaunch() {
    if (!g_parked.active) return;
    KillTimer(g_hwnd, kTimerLaunchWait);
    const ParkedLaunch p = g_parked;
    g_parked = ParkedLaunch();
    HandleLaunch(p.s, p.gp, p.pn, true);
}

// A server answered the UDP query with `protocol` and `latestClient` (WM_SERVER_PROTO; latestClient is the packed
// version of the newest client it was released with, 0 = unknown). A protocol AHEAD of this launcher, or a latest client
// newer than kClientVersion (the update ping: a same-protocol release such as 0.4.3), means a release was published that
// this launcher has not installed: look for it now (rate-limited, vgate::kMinCheckGapMs) instead of waiting for the
// 2-minute poll or for PLAY.
static void OnServerProtocol(int protocol, uint32_t latestClient) {
    static int lastSeen = 0;
    static uint32_t lastLatest = 0;
    const vgate::Gate gate = vgate::DecideServer(protocol, (int)proto::kVersion, latestClient, ClientVersionPacked(), g_up.available);
    if (protocol != lastSeen || latestClient != lastLatest) {
        lastSeen = protocol;
        lastLatest = latestClient;
        GLOGI("server protocol %d, this launcher speaks %d; server's latest client %s, this launcher %s%s", protocol, (int)proto::kVersion,
              VersionText(latestClient).c_str(), Narrow(kClientVersion).c_str(),
              gate.refuse ? (gate.code == vgate::kCodeUpdate ? " (server ahead: PLAY is refused until the launcher updates)"
                                                             : " (server behind: PLAY is refused)")
                          : "");
    }
    if (gate.checkNow) {
        const bool byProtocol = vgate::Decide(protocol, (int)proto::kVersion, g_up.available).refuse;
        KickUpdateLimited(byProtocol ? "a server reports a newer protocol" : "a server reports a newer client");
    }
}

static void Dispatch(const std::string& json) {
    JParser jp;
    if (!jp.parse(json.c_str(), json.size())) return;
    const JVal& v = jp.v;
    if (v.t != JVal::Obj) return;
    const JVal* tv = jGet(&v, "type");
    if (!tv || tv->t != JVal::Str) return;
    std::string type = tv->s;

    if (type == "ready") {
        DetectGamePathIfUnset();
        PostSettings();
        PostWindowState(true);
        PostFavorites();
        PostRecents();
        KickFetch();
        StartServerStatusWorker();
        // The first check starts with the process (wWinMain), whatever the autoUpdate setting:
        // a mandatory update has to be found either way. A (re)loaded page just gets the state.
        if (g_up.started) PostUpdate();
        else KickUpdate();
    } else if (type == "fetch" || type == "refresh") {
        KickFetch();
    } else if (type == "ping") {
        StartPing((int)jNum(jGet(&v, "row"), -1));
    } else if (type == "launch") {
        HandleLaunch(FromJServer(jGet(&v, "server")), jStr(jGet(&v, "gamePath")), jStr(jGet(&v, "playerName")), false);
    } else if (type == "browseGamePath") {
        // Not inside the WebMessageReceived callback: a modal dialog there
        // pumps messages while the WebView2 call is still on the stack.
        PostMessageW(g_hwnd, WM_BROWSE, 0, 0);
    } else if (type == "getSettings") {
        PostSettings();
    } else if (type == "saveSetting") {
        const JVal* key = jGet(&v, "key");
        const JVal* val = jGet(&v, "value");
        if (key && key->t == JVal::Str) {
            if (val && val->t == JVal::Str) SaveSetting(Widen(key->s), Widen(val->s), false);
            else if (val && val->t == JVal::Bool) SaveSetting(Widen(key->s), val->b ? L"1" : L"0", true);
            else if (val && val->t == JVal::Num) SaveSetting(Widen(key->s), std::to_wstring(jNum(val, 0)), false);
        }
        PostSettings();
        // Automatic updates switched on while a newer version is already known: fetch it now.
        if (key && key->t == JVal::Str && key->s == "autoUpdate" && GetAutoUpdate() && g_up.available &&
            g_up.state == UpState::Idle)
            StartUpdateDownload();
    } else if (type == "addFavorite") {
        AddFavorite(FromJServer(jGet(&v, "server")));
    } else if (type == "removeFavorite") {
        std::wstring key = jStr(jGet(&v, "key"));
        for (size_t i = 0; i < g_favs.size(); i++) {
            if (KeyOf(g_favs[i]) == key) { g_favs.erase(g_favs.begin() + i); break; }
        }
        SaveFavorites();
        PostFavorites();
    } else if (type == "clearFavorites") {
        g_favs.clear();
        SaveFavorites();
        PostFavorites();
    } else if (type == "addRecent") {
        AddRecent(FromJServer(jGet(&v, "server")));
    } else if (type == "clearRecents") {
        g_recs.clear();
        SaveRecents();
        PostRecents();
    } else if (type == "checkUpdate") {
        KickUpdate();
    } else if (type == "copyText") {
        bool ok = CopyToClipboard(jStr(jGet(&v, "text")));
        PostJson(std::string("{\"type\":\"copied\",\"ok\":") + (ok ? "true" : "false") + "}");
    } else if (type == "tokenGet") {
        std::string token = LoadSessionToken();
        PostJson("{\"type\":\"token\",\"token\":" + (token.empty() ? std::string("null") : jEsc(token)) + "}");
        SecureZeroMemory(&token[0], token.size());
    } else if (type == "tokenSet") {
        const JVal* t = jGet(&v, "token");
        bool ok = t && t->t == JVal::Str && StoreSessionToken(t->s);
        PostJson(std::string("{\"type\":\"tokenSaved\",\"ok\":") + (ok ? "true" : "false") + "}");
    } else if (type == "tokenClear") {
        bool ok = ClearSessionToken();
        PostJson(std::string("{\"type\":\"tokenSaved\",\"ok\":") + (ok ? "true" : "false") + "}");
    } else if (type == "openExternal") {
        std::wstring url = jStr(jGet(&v, "url"));
        bool ok = OpenExternalUrl(url);
        PostJson(std::string("{\"type\":\"opened\",\"ok\":") + (ok ? "true" : "false") + ",\"url\":" + jStrW(url) + "}");
    } else if (type == "log") {
        // UI errors (uncaught, unhandled rejections, console.error) into gage.log.
        std::string level = Narrow(jStr(jGet(&v, "level")));
        std::string text = Narrow(jStr(jGet(&v, "message")));
        if (text.size() > 2000) text.resize(2000);
        if (level == "warn") GLOGW("ui: %s", text.c_str());
        else GLOGE("ui: %s", text.c_str());
    } else if (type == "toggleMaximize") {
        ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        PostWindowState();
    } else if (type == "minimize") {
        ShowWindow(g_hwnd, SW_MINIMIZE);
    } else if (type == "close") {
        // Hide to tray only when the icon really exists — otherwise the
        // process would be left running with no way back to the window.
        ShowWindow(g_hwnd, g_trayOk ? SW_HIDE : SW_MINIMIZE);
    } else if (type == "quit") {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Frameless window + DPI helpers.
//
// The window keeps WS_OVERLAPPEDWINDOW (taskbar button, Alt+Tab, Aero Snap,
// minimize animation, DWM shadow and rounded corners stay native). WM_NCCALCSIZE
// removes the caption: the client area starts at the very top, while left/right/
// bottom keep the standard sizing frame (on Win10/11 that is invisible padding
// outside the 1 px DWM border, so content stays flush and native resize works).
// Dragging comes from the page: CSS `app-region: drag` -> HTCAPTION, served by
// WebView2 itself (ICoreWebView2Settings9), which also answers the top edge.
// ---------------------------------------------------------------------------

template <typename Fn>
static Fn ResolveUser32(const char* name) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    return u ? (Fn)(void*)GetProcAddress(u, name) : 0;
}

static UINT SystemDpi() {
    typedef UINT(WINAPI * Fn)();
    static Fn fn = ResolveUser32<Fn>("GetDpiForSystem");
    if (fn) return fn();
    HDC dc = GetDC(0);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(0, dc);
    return d > 0 ? (UINT)d : 96;
}

static UINT WindowDpi(HWND hwnd) {
    typedef UINT(WINAPI * Fn)(HWND);
    static Fn fn = ResolveUser32<Fn>("GetDpiForWindow");
    UINT d = (fn && hwnd) ? fn(hwnd) : 0;
    return d ? d : SystemDpi();
}

static int MetricForDpi(int index, UINT dpi) {
    typedef int(WINAPI * Fn)(int, UINT);
    static Fn fn = ResolveUser32<Fn>("GetSystemMetricsForDpi");
    return fn ? fn(index, dpi) : GetSystemMetrics(index);
}

// Thickness of the sizing frame (the invisible padding plus the 1 px border).
static int FrameX(UINT dpi) { return MetricForDpi(SM_CXSIZEFRAME, dpi) + MetricForDpi(SM_CXPADDEDBORDER, dpi); }
static int FrameY(UINT dpi) { return MetricForDpi(SM_CYSIZEFRAME, dpi) + MetricForDpi(SM_CXPADDEDBORDER, dpi); }

// (Re)apply the frame state: DWM shadow margin + corner preference, then make
// Windows re-run WM_NCCALCSIZE. dwmapi is resolved at runtime like the DPI APIs.
static void ApplyFrame(HWND hwnd) {
    typedef HRESULT(WINAPI * ExtendFn)(HWND, const MARGINS*);
    typedef HRESULT(WINAPI * AttrFn)(HWND, DWORD, LPCVOID, DWORD);
    static HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dwm) {
        ExtendFn extend = (ExtendFn)(void*)GetProcAddress(dwm, "DwmExtendFrameIntoClientArea");
        AttrFn attr = (AttrFn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
        // A 1 px extended frame keeps the drop shadow on a window whose client area
        // covers the caption. Zero margins when the native caption is in use.
        MARGINS m = g_frameless ? MARGINS{ 0, 0, 1, 0 } : MARGINS{ 0, 0, 0, 0 };
        if (extend) extend(hwnd, &m);
        int round = 2;  // DWMWCP_ROUND (Windows 11; harmlessly rejected on Windows 10)
        if (attr) attr(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    }
    SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// The WebView covers the whole client area. Left/right/bottom resize comes from
// the native sizing frame outside the client area; the top edge is served by
// WebView2 itself (with non-client region support on it lays resize strips over
// the first pixels of the page, verified on runtime 155: HTTOP for the top rows,
// HTCAPTION below them where the page says `app-region: drag`).
static RECT WebBounds(HWND hwnd) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    return rc;
}

// Minimized or hidden to tray: tell WebView2 so the page stops rendering.
static void SetWebVisible(bool visible) {
    if (!g_ctrl || g_wvVisible == (visible ? 1 : 0)) return;
    g_wvVisible = visible ? 1 : 0;
    g_ctrl->put_IsVisible(visible ? TRUE : FALSE);
}

static void SyncWebVisible() {
    SetWebVisible(g_hwnd && IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd));
}

// ---------------------------------------------------------------------------
// WebView2 COM handlers (each: IUnknown + Invoke, atomic refcount).
//
// Standard COM ownership: a handler starts at ref=1 owned by its creator; the
// creator Releases that ref right after handing the handler to WebView2,
// which AddRefs/Releases its own. Invoke never releases itself. Interface
// pointers received as Invoke arguments are borrowed — AddRef to keep them.
// The message-received handler is long-lived and owned by the host
// (released in WM_DESTROY).
// ---------------------------------------------------------------------------

// True when `uri` is on the app origin (https://app.gage/...). The host part
// must end at the slash, so https://app.gage.evil.example/ never matches.
static bool IsAppUrl(const wchar_t* uri) {
    return uri && _wcsnicmp(uri, kAppOrigin, wcslen(kAppOrigin)) == 0;
}

// Leave WITHOUT touching the game: the message loop destroys the window directly
// (WM_STARTUP_ABORT), never through WM_CLOSE, which is the close-game path.
// Modal box first, so the user reads why; callable from any WebView2 callback.
static void FatalExit(const std::wstring& text, int exitCode) {
    static bool shown = false;
    if (shown) return;
    shown = true;
    g_exitCode = exitCode;
    bool haveWindow = g_hwnd && IsWindow(g_hwnd);
    MessageBoxW(haveWindow ? g_hwnd : nullptr, text.c_str(), L"Solar Launcher", MB_ICONERROR | MB_OK);
    if (g_hwnd && IsWindow(g_hwnd)) PostMessageW(g_hwnd, WM_STARTUP_ABORT, 0, 0);
}

// One place for every startup failure: log it, tell the user what failed and
// why, and close instead of leaving a blank window running.
static void StartupFailed(const wchar_t* stage, HRESULT hr) {
    GLOGE("startup failed at %s hr=0x%08lX", Narrow(stage).c_str(), (unsigned long)hr);
    const wchar_t* hint =
        hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? L"The Microsoft Edge WebView2 Runtime was not found. Install it from https://go.microsoft.com/fwlink/p/?LinkId=2124703 and start Solar Launcher again." :
        hr == E_ACCESSDENIED ? L"Solar Launcher cannot write its WebView2 data folder. Move Solar RP to a writable folder." :
        hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) ? L"Another Solar Launcher instance is using the same data folder with different options. Close it and try again." :
        hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ? L"The WebView2 runtime did not respond in time. Try again; if it keeps happening, repair or reinstall the Microsoft Edge WebView2 Runtime." :
        L"See gage.log next to SolarLauncher.exe for details.";
    wchar_t m[768];
    swprintf(m, sizeof(m) / sizeof(m[0]), L"Solar Launcher could not start its window (%ls).\r\n\r\nHRESULT 0x%08lX\r\n\r\n%ls",
             stage, (unsigned long)hr, hint);
    FatalExit(m, 2);
}

// COM boilerplate for the host-owned event handlers below (IUnknown + atomic ref).
#define GAGE_COM_HANDLER(Iface, IidName)                                                    \
    std::atomic<long> ref{1};                                                               \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {            \
        if (!out) return E_POINTER;                                                         \
        if (riid == IID_IUnknown || riid == IidName) {                                      \
            *out = (Iface*)this;                                                            \
            AddRef();                                                                       \
            return S_OK;                                                                    \
        }                                                                                   \
        *out = 0;                                                                           \
        return E_NOINTERFACE;                                                               \
    }                                                                                       \
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }                             \
    ULONG STDMETHODCALLTYPE Release() override {                                            \
        long r = --ref;                                                                     \
        if (r == 0) delete this;                                                            \
        return r;                                                                           \
    }

struct CtrlHandler;  // fwd (used inside EnvHandler::Invoke)

struct WebMsgHandler final : ICoreWebView2WebMessageReceivedEventHandler {
    std::atomic<long> ref{1};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2WebMessageReceivedEventHandler) {
            *out = (ICoreWebView2WebMessageReceivedEventHandler*)this;
            AddRef();
            return S_OK;
        }
        *out = 0;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        long r = --ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        if (args) {
            // Only the app origin may drive the bridge (launch, saveSetting, ...).
            LPWSTR src = 0;
            bool fromApp = SUCCEEDED(args->get_Source(&src)) && IsAppUrl(src);
            if (!fromApp) {
                static int dropped = 0;
                if (dropped++ < 5) GLOGW("bridge message from foreign source ignored: %.120s", src ? Narrow(src).c_str() : "(none)");
            }
            if (src) CoTaskMemFree(src);
            if (!fromApp) return S_OK;

            LPWSTR json = 0;
            if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                std::string utf8 = Narrow(json);
                CoTaskMemFree(json);
                static bool first = true;
                if (first) {
                    first = false;
                    GLOGI("first WebMessageReceived: %.200s", utf8.c_str());
                }
                Dispatch(utf8);
            } else if (json) {
                CoTaskMemFree(json);
            }
        }
        return S_OK;
    }
};

// NavigationStarting: this window only ever shows the app origin.
struct NavHandler final : ICoreWebView2NavigationStartingEventHandler {
    GAGE_COM_HANDLER(ICoreWebView2NavigationStartingEventHandler, IID_ICoreWebView2NavigationStartingEventHandler)

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) override {
        if (!args) return S_OK;
        LPWSTR uri = 0;
        bool ok = SUCCEEDED(args->get_Uri(&uri)) && IsAppUrl(uri);  // fail closed when the URI is unreadable
        if (!ok) {
            GLOGW("navigation blocked: %.200s", uri ? Narrow(uri).c_str() : "(unreadable)");
            args->put_Cancel(TRUE);
        }
        if (uri) CoTaskMemFree(uri);
        return S_OK;
    }
};

// NewWindowRequested: never open a second WebView window. A user-initiated
// http(s) link goes to the default browser; every other scheme is dropped.
struct NewWinHandler final : ICoreWebView2NewWindowRequestedEventHandler {
    GAGE_COM_HANDLER(ICoreWebView2NewWindowRequestedEventHandler, IID_ICoreWebView2NewWindowRequestedEventHandler)

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) override {
        if (!args) return S_OK;
        args->put_Handled(TRUE);
        LPWSTR uri = 0;
        BOOL userInitiated = FALSE;
        if (SUCCEEDED(args->get_Uri(&uri)) && uri && SUCCEEDED(args->get_IsUserInitiated(&userInitiated)) && userInitiated) {
            bool web = _wcsnicmp(uri, L"https://", 8) == 0 || _wcsnicmp(uri, L"http://", 7) == 0;
            if (web) ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
            else GLOGW("new window to non-web URL dropped: %.80s", Narrow(uri).c_str());
        }
        if (uri) CoTaskMemFree(uri);
        return S_OK;
    }
};

// ---------------------------------------------------------------------------
// Launcher 3D assets: apps/launcher-ui/dist/assets, shipped next to the exe as
// launcher-assets\ and served under https://app.gage/assets/. Only the paths
// its manifest.json lists are served: that list is the whole path check, so no
// "..", backslash, %-escape or drive letter from a URL reaches the file system.
// ---------------------------------------------------------------------------

static std::map<std::string, ULONGLONG> g_assets;  // "assets/models/car.glb" -> bytes

// A manifest path we are willing to map onto the disk: "assets/" + plain
// segments of [A-Za-z0-9._-], none of them "." or "..".
static bool SafeAssetPath(const std::string& p) {
    if (p.size() <= 7 || p.size() > 200 || p.compare(0, 7, "assets/") != 0) return false;
    size_t seg = 7;
    for (size_t i = 7; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            std::string s = p.substr(seg, i - seg);
            if (s.empty() || s == "." || s == "..") return false;
            seg = i + 1;
            continue;
        }
        char c = p[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

static std::wstring AssetFile(const std::string& path) {
    std::wstring rel = Widen(path.substr(7));
    for (wchar_t& c : rel)
        if (c == L'/') c = L'\\';
    return ExeDir() + L"launcher-assets\\" + rel;
}

// Read once at startup. Missing or unreadable: every asset is a 404 and the SPA
// shows its designed "3D set unavailable" fallback.
static void LoadAssetManifest() {
    g_assets.clear();
    std::wstring file = ExeDir() + L"launcher-assets\\manifest.json";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        GLOGW("assets: %s not found (%lu); the 3D set is unavailable", Narrow(file).c_str(), GetLastError());
        return;
    }
    std::string text(1024 * 1024, '\0');
    DWORD got = 0;
    BOOL read = ReadFile(h, &text[0], (DWORD)text.size(), &got, nullptr);
    CloseHandle(h);
    JParser jp;
    jp.maxNodes = 65536;
    if (!read || got == 0 || got == text.size() || !jp.parse(text.data(), got) || jp.v.t != JVal::Obj) {
        GLOGE("assets: %s is unreadable or not JSON; the 3D set is unavailable", Narrow(file).c_str());
        return;
    }
    size_t refused = 0;
    for (const auto& kv : jp.v.o) {
        if (kv.first != "files" || kv.second.t != JVal::Arr) continue;
        for (const JVal& f : kv.second.a) {
            const JVal* p = jGet(&f, "path");
            const JVal* b = jGet(&f, "bytes");
            if (!p || p->t != JVal::Str || !b || b->t != JVal::Num || b->n < 0 || !SafeAssetPath(p->s)) {
                ++refused;
                continue;
            }
            g_assets[p->s] = (ULONGLONG)b->n;
        }
    }
    GLOGI("assets: %zu file(s) listed in %s%s", g_assets.size(), Narrow(file).c_str(),
          refused ? " (some entries refused: unsafe or malformed)" : "");
}

static const wchar_t* AssetContentType(const std::string& path) {
    size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    if (ext == ".glb") return L"model/gltf-binary";
    if (ext == ".hdr") return L"image/vnd.radiance";
    if (ext == ".webp") return L"image/webp";
    if (ext == ".json") return L"application/json";
    if (ext == ".png") return L"image/png";
    if (ext == ".jpg") return L"image/jpeg";
    if (ext == ".ktx2") return L"image/ktx2";
    if (ext == ".woff2") return L"font/woff2";
    if (ext == ".avif") return L"image/avif";
    if (ext == ".mp4") return L"video/mp4";
    if (ext == ".webm") return L"video/webm";
    return L"application/octet-stream";
}

// The stream for https://app.gage/<path>, or null (the caller answers 404).
// manifest.json itself is served too; every other file must be listed and
// still have the size the manifest records.
static IStream* OpenAsset(const std::string& path) {
    const bool manifest = path == "assets/manifest.json";
    auto it = g_assets.find(path);
    if (!manifest && it == g_assets.end()) return nullptr;
    std::wstring file = AssetFile(path);
    if (!manifest) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa)) {
            GLOGW("assets: %s is listed but missing", path.c_str());
            return nullptr;
        }
        ULONGLONG size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        if (size != it->second) {
            GLOGW("assets: %s is %llu bytes, the manifest says %llu; not served", path.c_str(), size, it->second);
            return nullptr;
        }
    }
    IStream* s = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &s);
    if (FAILED(hr)) {
        GLOGW("assets: could not open %s hr=0x%08lX", path.c_str(), (unsigned long)hr);
        return nullptr;
    }
    return s;
}

// WebResourceRequested: serves the embedded SPA for https://app.gage/ (and
// /index.html) and the listed 3D assets under /assets/; anything else under the
// origin is a 404. Runs on the UI thread.
struct ResHandler final : ICoreWebView2WebResourceRequestedEventHandler {
    GAGE_COM_HANDLER(ICoreWebView2WebResourceRequestedEventHandler, IID_ICoreWebView2WebResourceRequestedEventHandler)

    // "https://app.gage/", "https://app.gage/index.html", either with ?query or #fragment.
    static bool IsIndexUrl(const wchar_t* uri) {
        if (!IsAppUrl(uri)) return false;
        std::wstring rest = uri + wcslen(kAppOrigin);
        size_t cut = rest.find_first_of(L"?#");
        if (cut != std::wstring::npos) rest.resize(cut);
        return rest.empty() || rest == L"index.html";
    }

    // "assets/..." for https://app.gage/assets/... (query and fragment cut), else "".
    static std::string AssetPath(const wchar_t* uri) {
        if (!IsAppUrl(uri)) return "";
        std::wstring rest = uri + wcslen(kAppOrigin);
        size_t cut = rest.find_first_of(L"?#");
        if (cut != std::wstring::npos) rest.resize(cut);
        if (rest.compare(0, 7, L"assets/") != 0) return "";
        for (wchar_t c : rest)
            if (c > 0x7E) return "";  // manifest paths are ASCII
        return Narrow(rest);
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) override {
        if (!args || !g_env) return S_OK;
        LPWSTR uri = 0;
        ICoreWebView2WebResourceRequest* req = 0;
        if (SUCCEEDED(args->get_Request(&req)) && req) {
            req->get_Uri(&uri);
            req->Release();
        }
        bool index = uri && IsIndexUrl(uri);
        std::string asset = index || !uri ? std::string() : AssetPath(uri);
        IStream* assetBody = asset.empty() ? nullptr : OpenAsset(asset);

        ICoreWebView2WebResourceResponse* resp = 0;
        HRESULT hr;
        if (assetBody) {
            std::wstring headers = std::wstring(L"Content-Type: ") + AssetContentType(asset) +
                                   L"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff";
            hr = g_env->CreateWebResourceResponse(assetBody, 200, L"OK", headers.c_str(), &resp);
            assetBody->Release();
        } else if (index) {
            IStream* body = SHCreateMemStream(kUiHtml, (UINT)kUiHtmlLen);  // copies the bytes
            if (!body) {
                hr = E_OUTOFMEMORY;
            } else {
                hr = g_env->CreateWebResourceResponse(
                    body, 200, L"OK", L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store", &resp);
                body->Release();
            }
        } else {
            GLOGW("app origin: 404 for %.200s", uri ? Narrow(uri).c_str() : "(unreadable)");
            hr = g_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"Content-Type: text/plain; charset=utf-8", &resp);
        }
        if (SUCCEEDED(hr) && resp) {
            args->put_Response(resp);
            resp->Release();
        } else {
            GLOGE("could not build the app-origin response hr=0x%08lX", (unsigned long)hr);
        }
        if (uri) CoTaskMemFree(uri);
        return S_OK;
    }
};

// ProcessFailed: log here, act from the message loop (a modal box or Reload()
// inside the event would run re-entrantly).
struct ProcFailHandler final : ICoreWebView2ProcessFailedEventHandler {
    GAGE_COM_HANDLER(ICoreWebView2ProcessFailedEventHandler, IID_ICoreWebView2ProcessFailedEventHandler)

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) override {
        COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
        if (args) args->get_ProcessFailedKind(&kind);
        GLOGE("WebView2 process failed: kind=%d", (int)kind);
        if (IsWindow(g_hwnd)) PostMessageW(g_hwnd, WM_WV_FAILED, (WPARAM)kind, 0);
        return S_OK;
    }
};

struct EnvHandler final : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    std::atomic<long> ref{1};
    HWND hwnd;
    explicit EnvHandler(HWND h) : hwnd(h) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) {
            *out = (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)this;
            AddRef();
            return S_OK;
        }
        *out = 0;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        long r = --ref;
        if (r == 0) delete this;
        return r;
    }
    // Defined out-of-line after CtrlHandler so `new CtrlHandler` sees a complete type.
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Environment* env) override;
};

struct CtrlHandler final : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    std::atomic<long> ref{1};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) {
            *out = (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*)this;
            AddRef();
            return S_OK;
        }
        *out = 0;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        long r = --ref;
        if (r == 0) delete this;
        return r;
    }

    static void ApplySettings(ICoreWebView2* wv) {
        ICoreWebView2Settings* st = 0;
        if (FAILED(wv->get_Settings(&st)) || !st) return;
        st->put_IsWebMessageEnabled(TRUE);
        // DevTools stay available only when explicitly requested.
        wchar_t dt[8] = {};
        bool devtools = GetEnvironmentVariableW(L"GAGE_DEVTOOLS", dt, 8) > 0 && dt[0] == L'1';
        st->put_AreDevToolsEnabled(devtools ? TRUE : FALSE);
        st->put_IsStatusBarEnabled(FALSE);
        st->put_IsZoomControlEnabled(FALSE);
        // An app, not a browser: no F5/Ctrl+R reload (it would drop the SPA's
        // state mid-launch), Ctrl+P print, Ctrl+F find bar or Alt+Left back.
        // Editing keys (Ctrl+C/V/X/A/Z) are not browser accelerators and keep
        // working. F12 & co. stay with the DevTools opt-in.
        ICoreWebView2Settings3* s3 = 0;
        if (SUCCEEDED(st->QueryInterface(IID_ICoreWebView2Settings3, (void**)&s3)) && s3) {
            s3->put_AreBrowserAcceleratorKeysEnabled(devtools ? TRUE : FALSE);
            s3->Release();
        }

        // Frameless window: CSS `app-region: drag` becomes the title bar (move, Aero
        // Snap, double-click maximize, system menu on right click). Takes effect on the
        // next navigation, so this must run before Navigate().
        bool nonClient = false;
        ICoreWebView2Settings9* s9 = 0;
        if (SUCCEEDED(st->QueryInterface(IID_ICoreWebView2Settings9, (void**)&s9)) && s9) {
            nonClient = SUCCEEDED(s9->put_IsNonClientRegionSupportEnabled(TRUE));
            s9->Release();
        }
        st->Release();
        if (!nonClient && g_frameless) {
            // An old runtime would leave a window nobody can drag: keep the native caption.
            GLOGW("WebView2 runtime lacks non-client region support (Settings9); keeping the native title bar");
            g_frameless = false;
            ApplyFrame(g_hwnd);
        }
    }

    // Registers the five event handlers (and the app-origin resource filter) on
    // the new WebView. Returns the failing stage or nullptr.
    static const wchar_t* AddHandlers(ICoreWebView2* wv, HRESULT* hrOut) {
        HRESULT hr;
        g_msgHandler = new WebMsgHandler();  // host owns (ref=1)
        if (FAILED(hr = wv->add_WebMessageReceived(g_msgHandler, &g_msgToken))) { *hrOut = hr; return L"add_WebMessageReceived"; }
        g_navHandler = new NavHandler();
        if (FAILED(hr = wv->add_NavigationStarting(g_navHandler, &g_navToken))) { *hrOut = hr; return L"add_NavigationStarting"; }
        g_newWinHandler = new NewWinHandler();
        if (FAILED(hr = wv->add_NewWindowRequested(g_newWinHandler, &g_newWinToken))) { *hrOut = hr; return L"add_NewWindowRequested"; }
        g_failHandler = new ProcFailHandler();
        if (FAILED(hr = wv->add_ProcessFailed(g_failHandler, &g_failToken))) { *hrOut = hr; return L"add_ProcessFailed"; }
        g_resHandler = new ResHandler();
        if (FAILED(hr = wv->add_WebResourceRequested(g_resHandler, &g_resToken))) { *hrOut = hr; return L"add_WebResourceRequested"; }
        if (FAILED(hr = wv->AddWebResourceRequestedFilter(kAppFilter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL))) {
            *hrOut = hr;
            return L"AddWebResourceRequestedFilter";
        }
        g_resFilterAdded = true;
        return nullptr;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Controller* ctrl) override {
        if (!IsWindow(g_hwnd)) {
            // The window closed while the controller was being created: nothing to
            // show, nobody to tell. Close what we were handed (borrowed, no AddRef).
            GLOGI("controller callback after the window closed (hr=0x%08lX): closing it", (unsigned long)errorCode);
            if (ctrl) ctrl->Close();
            return S_OK;
        }
        GLOGI("controller callback: hr=0x%08lX ctrl=%p", (unsigned long)errorCode, (void*)ctrl);
        if (FAILED(errorCode) || !ctrl) {
            StartupFailed(L"CreateCoreWebView2Controller callback", FAILED(errorCode) ? errorCode : E_POINTER);
            return S_OK;
        }
        KillTimer(g_hwnd, kTimerStartup);  // the controller arrived: disarm the watchdog
        ctrl->AddRef();  // borrowed argument -> host-owned; released in WM_DESTROY
        g_ctrl = ctrl;
        ctrl->put_Bounds(WebBounds(g_hwnd));
        SyncWebVisible();  // not TRUE blindly: the window may be starting minimized
        // Paint the page colour (not white) before the SPA's first frame.
        ICoreWebView2Controller2* c2 = 0;
        if (SUCCEEDED(ctrl->QueryInterface(IID_ICoreWebView2Controller2, (void**)&c2)) && c2) {
            c2->put_DefaultBackgroundColor(kBgColor);
            c2->Release();
        }

        ICoreWebView2* wv = 0;
        HRESULT hr = ctrl->get_CoreWebView2(&wv);
        if (FAILED(hr) || !wv) {
            StartupFailed(L"get_CoreWebView2", FAILED(hr) ? hr : E_POINTER);
            return S_OK;
        }
        g_wv = wv;  // keeps the get_ reference; released in WM_DESTROY
        ApplySettings(wv);
        const wchar_t* failed = AddHandlers(wv, &hr);
        if (failed) {
            StartupFailed(failed, hr);
            return S_OK;
        }
        // The SPA is served from memory by ResHandler; NavigateToString has a ~2 MB cap.
        static_assert(kUiHtmlLen > 0, "embedded SPA is empty — check GAGE_UI_HTML_IN in CMakeLists.txt (apps/launcher-ui/dist/index.html)");
        LoadAssetManifest();
        // No platform API configured yet: the SPA runs its labelled demo data
        // (?demo=1) with this real bridge; launch refuses with code 13.
        std::wstring url = kAppUrl;
        if (GetApiBase().empty()) {
            url += L"?demo=1";
            GLOGW("no api_base configured: the launcher UI runs in demo mode");
        }
        hr = wv->Navigate(url.c_str());
        if (FAILED(hr)) {
            StartupFailed(L"Navigate", hr);
            return S_OK;
        }
        GLOGI("SPA navigating to %s (%lu bytes served from memory)", Narrow(url).c_str(), (unsigned long)kUiHtmlLen);
        return S_OK;
    }
};

HRESULT STDMETHODCALLTYPE EnvHandler::Invoke(HRESULT errorCode, ICoreWebView2Environment* env) {
    if (!IsWindow(g_hwnd)) {
        // Window gone before the runtime answered. `env` is borrowed and we have not
        // AddRef'd it, so there is nothing to release; no error box either.
        GLOGI("WebView2 env callback after the window closed (hr=0x%08lX): ignored", (unsigned long)errorCode);
        return S_OK;
    }
    GLOGI("WebView2 env callback: hr=0x%08lX env=%p", (unsigned long)errorCode, (void*)env);
    if (FAILED(errorCode) || !env) {
        StartupFailed(L"CreateCoreWebView2Environment callback", FAILED(errorCode) ? errorCode : E_POINTER);
        return S_OK;
    }
    env->AddRef();  // borrowed argument -> host-owned; released in WM_DESTROY
    g_env = env;
    CtrlHandler* ch = new CtrlHandler();
    HRESULT hr = env->CreateCoreWebView2Controller(hwnd, ch);
    ch->Release();  // creator's ref; WebView2 holds its own until the callback ran
    if (FAILED(hr)) StartupFailed(L"CreateCoreWebView2Controller", hr);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Tray
// ---------------------------------------------------------------------------

// The brand icon at a system metric's size (SM_CXICON / SM_CXSMICON), picked
// from the .ico's sizes; Windows' generic app icon if the resource is missing.
// LR_SHARED: the system owns it, nothing to destroy.
static HICON AppIcon(int cxMetric, int cyMetric) {
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON,
        GetSystemMetrics(cxMetric), GetSystemMetrics(cyMetric), LR_SHARED);
    if (icon) return icon;
    GLOGW("app icon %d not loaded (err=%lu); using the system default", kAppIconId, GetLastError());
    return (HICON)LoadImageW(0, IDI_APPLICATION, IMAGE_ICON, 0, 0, LR_SHARED);
}

static void AddTray() {
    g_trayIcon = AppIcon(SM_CXSMICON, SM_CYSMICON);
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_ACTION;
    g_nid.hIcon = g_trayIcon;
    lstrcpynW(g_nid.szTip, L"Solar Launcher", sizeof(g_nid.szTip) / sizeof(wchar_t));
    g_trayOk = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    if (!g_trayOk) GLOGW("tray icon could not be added (err=%lu)", GetLastError());
}

static void RemoveTray() {
    if (g_trayOk) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayOk = false;
}

static void RestoreWindow() {
    if (g_closing) return;  // already leaving: don't pop the window back up
    ShowWindow(g_hwnd, SW_SHOW);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    KickUpdateLimited("the launcher was restored");  // rate-limited, so the WM_ACTIVATE that follows adds nothing
}

static void OnTray() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 101, L"Show");
    AppendMenuW(m, MF_STRING, 102, L"Exit");
    SetForegroundWindow(g_hwnd);
    // RETURNCMD: without it TrackPopupMenu returns BOOL, never the item id.
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, 0);
    DestroyMenu(m);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);  // documented: dismisses the menu correctly next time
    if (cmd == 101) {
        RestoreWindow();
    } else if (cmd == 102) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) {  // Explorer restarted: icon is gone
        if (!g_closing) {
            g_trayOk = false;
            AddTray();
        }
        return 0;
    }
    if (g_showMsg && msg == g_showMsg) {  // a second instance asked us to come forward
        RestoreWindow();
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;  // WM_CREATE runs before CreateWindowExW returns
            AddTray();
            LoadFavsRecents();
            return 0;
        }
        case WM_NCCALCSIZE: {
            if (!g_frameless) return DefWindowProcW(hwnd, msg, wp, lp);
            // Standard sizing frame on left/right/bottom (DefWindowProc), no caption and
            // no top frame: the client area starts at the top of the window. Maximized,
            // the top frame still hangs off-screen and must be subtracted, or the
            // content would start above the monitor.
            RECT* rc = wp ? &((NCCALCSIZE_PARAMS*)lp)->rgrc[0] : (RECT*)lp;
            LONG top = rc->top;
            LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            if (r != 0) return r;
            rc->top = IsZoomed(hwnd) ? top + FrameY(WindowDpi(hwnd)) : top;
            if (rc->top > rc->bottom) rc->top = rc->bottom;
            return 0;
        }
        case WM_DPICHANGED: {
            // Moved to a monitor with another scale: take the size Windows suggests.
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, 0, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            // Arrives before WM_CREATE too; WindowDpi falls back to the system DPI then.
            UINT dpi = WindowDpi(hwnd);
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = MulDiv(kMinClientW, (int)dpi, 96) + 2 * FrameX(dpi);
            mm->ptMinTrackSize.y = MulDiv(kMinClientH, (int)dpi, 96) + FrameY(dpi) +
                                   (g_frameless ? 0 : MetricForDpi(SM_CYCAPTION, dpi));
            return 0;
        }
        case WM_SHOWWINDOW: {
            SetWebVisible(wp != 0 && !IsIconic(hwnd));
            PostMessageW(hwnd, WM_WINDOW_STATE, 0, 0);  // the visible flag changes after this message
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_ACTIVATE: {
            PostMessageW(hwnd, WM_WINDOW_STATE, 0, 0);
            // Brought forward (clicked, restored from the tray, alt-tabbed to): a player who opens the launcher
            // is about to press PLAY, so look for a release now (at most once a minute, version_gate.h).
            if (LOWORD(wp) != WA_INACTIVE && HIWORD(wp) == 0 && !g_closing) KickUpdateLimited("the launcher window was brought forward");
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_WINDOW_STATE: {
            PostWindowState();
            return 0;
        }
        case WM_SIZE: {
            PostMessageW(hwnd, WM_WINDOW_STATE, 0, 0);
            if (wp == SIZE_MINIMIZED) {  // client rect is 0x0 while minimized: keep the old bounds
                SetWebVisible(false);
                return 0;
            }
            if (g_ctrl) {
                g_ctrl->put_Bounds(WebBounds(hwnd));
                SetWebVisible(IsWindowVisible(hwnd) != FALSE);
            }
            return 0;
        }
        case WM_TIMER: {
            if (wp == kTimerStartup) {
                KillTimer(hwnd, kTimerStartup);
                if (!g_ctrl) {
                    GLOGE("WebView2 controller did not arrive within %u ms", (unsigned)kStartupTimeoutMs);
                    StartupFailed(L"WebView2 start timed out", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                }
                return 0;
            }
            if (wp == kTimerUpdateRetry) {
                KillTimer(hwnd, kTimerUpdateRetry);
                TryInstallUpdate();  // re-arms the timer itself while the game is still there
                return 0;
            }
            if (wp == kTimerUpdatePoll) {
                KickUpdate();  // no-op while a check/download/install is already under way
                return 0;
            }
            if (wp == kTimerLaunchWait) {
                if (g_parked.active)
                    GLOGW("update: the check PLAY waits for is still running after %u ms; launching without waiting for it",
                          kLaunchCheckWaitMs);
                ResumeParkedLaunch();  // kills the timer; with nothing parked this is a no-op
                KillTimer(hwnd, kTimerLaunchWait);
                return 0;
            }
            if (wp == kTimerUpdateExit) {
                KillTimer(hwnd, kTimerUpdateExit);
                GLOGI("update: leaving for setup");
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_STARTUP_ABORT: {
            // Fatal WebView2 failure: leave through DestroyWindow, never through
            // WM_CLOSE, so a game that is running is not closed with the launcher.
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_WV_FAILED: {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = (COREWEBVIEW2_PROCESS_FAILED_KIND)wp;
            if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                // The browser process owns the whole WebView2 environment: nothing to reload.
                FatalExit(L"The WebView2 browser process exited unexpectedly, so the Solar Launcher window cannot continue.\r\n\r\n"
                          L"A running game is not affected. Start Solar Launcher again; see gage.log if it keeps happening.", 3);
            } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                       kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                // The page is dead or hung: reload it (the SPA announces `ready` again and
                // native re-sends settings/lists). Give up if it keeps failing.
                static ULONGLONG windowStart = 0;
                static int count = 0;
                ULONGLONG now = GetTickCount64();
                if (now - windowStart > 60000) { windowStart = now; count = 0; }
                if (++count > 3) {
                    FatalExit(L"The Solar Launcher window keeps failing (the WebView2 renderer crashed repeatedly).\r\n\r\n"
                              L"A running game is not affected. See gage.log for details.", 3);
                } else if (g_wv) {
                    GLOGW("reloading the SPA after WebView2 process failure (kind=%d, attempt %d/3)", (int)kind, count);
                    g_wv->Reload();
                }
            }
            // GPU / utility / frame-renderer / plugin processes: WebView2 recovers them itself (logged only).
            return 0;
        }
        case WM_MOVE: {
            if (g_ctrl) g_ctrl->NotifyParentWindowPositionChanged();
            return 0;
        }
        case WM_SETFOCUS: {
            if (g_ctrl) g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;
        }
        case WM_STATUS_POST: {
            std::wstring* p = (std::wstring*)lp;
            PostStatus(*p);
            delete p;
            return 0;
        }
        case WM_JSON_POST: {
            std::string* p = (std::string*)lp;
            PostJson(*p);
            delete p;
            return 0;
        }
        case WM_BROWSE: {
            OnBrowse();
            return 0;
        }
        case WM_LAUNCH_DONE: {
            LaunchResult* r = (LaunchResult*)lp;
            g_launching = false;
            if (r->hProc) {
                // This is the process close_game may close later (UI-thread owned).
                if (g_closing) {
                    CloseHandle(r->hProc);  // already leaving: nobody will use it
                } else {
                    if (g_gameProc) CloseHandle(g_gameProc);
                    g_gameProc = r->hProc;
                    GLOGI("tracking game pid %lu for close_game", (unsigned long)r->pid);
                }
                r->hProc = 0;
            }
            PostStatus(r->ok ? L"Game started." : L"Launch failed.");
            PostLaunchResult(r->ok, r->code, r->error);
            delete r;
            return 0;
        }
        case WM_FETCH_DONE: {
            FetchResult* r = (FetchResult*)lp;
            if (r->gen == g_gen) {  // a newer fetch supersedes this one
                if (!r->failed) g_servers = r->servers;
                PostServers(r->failed ? "error" : "ok", r->failed ? r->error : L"");
                if (!r->failed) StartPingAll();
            }
            delete r;
            return 0;
        }
        case WM_PING_DONE: {
            PingResult* r = (PingResult*)lp;
            if (r->gen == g_gen && r->row >= 0 && r->row < (int)g_servers.size() &&
                KeyOf(g_servers[r->row]) == r->key) {
                g_servers[r->row].ping = r->ms;
                // the SPA patches the row itself: no whole-list re-send per ping
                PostJson("{\"type\":\"ping\",\"row\":" + numStr(r->row) + ",\"ms\":" + numStr(r->ms) + "}");
            }
            delete r;
            return 0;
        }
        case WM_PING_BATCH: {
            PingBatch* b = (PingBatch*)lp;
            if (b->gen == g_gen) {
                std::string results;
                for (const PingHit& h : b->hits) {
                    if (h.row < 0 || h.row >= (int)g_servers.size() || KeyOf(g_servers[h.row]) != h.key) continue;
                    g_servers[h.row].ping = h.ms;
                    if (!results.empty()) results += ',';
                    results += "{\"row\":" + numStr(h.row) + ",\"ms\":" + numStr(h.ms) + "}";
                }
                if (!results.empty()) PostJson("{\"type\":\"pings\",\"results\":[" + results + "]}");
            }
            delete b;
            return 0;
        }
        case WM_UPDATE_DONE: {
            UpdateCheckDone* r = (UpdateCheckDone*)lp;
            OnUpdateChecked(r->result);
            delete r;
            return 0;
        }
        case WM_UPDATE_PROGRESS: {
            if (lp == 1 && g_up.state == UpState::Downloading) {
                g_up.state = UpState::Verifying;
                g_up.progress = 1;
                PostUpdate();
            } else if (lp == 0 && g_up.state == UpState::Downloading) {
                g_up.progress = (double)wp / 1000.0;
                PostUpdate();
            }
            return 0;
        }
        case WM_UPDATE_DL_DONE: {
            UpdateDlDone* d = (UpdateDlDone*)lp;
            if (!d->ok) {
                FailUpdate(d->why);
            } else {
                g_up.installer = std::move(d->inst);
                g_up.state = UpState::Ready;
                g_up.progress = 1;
                GLOGI("update: %s verified (size and sha256 match the signed manifest)", Narrow(g_up.installer.path).c_str());
                PostUpdate();
                TryInstallUpdate();
            }
            delete d;
            return 0;
        }
        case WM_GAME_EXITED: {
            // A verified installer that waited for the game goes in now; otherwise look for a release right
            // away (at most once a minute): the player closed the game, and the next thing they do is PLAY.
            if (g_up.state == UpState::Deferred) TryInstallUpdate();
            else KickUpdateLimited("the game exited");
            return 0;
        }
        case WM_SERVER_PROTO: {
            OnServerProtocol((int)wp, (uint32_t)lp);
            return 0;
        }
        case WM_TRAY_ACTION: {
            // Legacy (pre-NIM_SETVERSION) callback: wparam = icon id, LOWORD(lparam) = mouse message.
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK: RestoreWindow(); break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: OnTray(); break;
                default: break;
            }
            return 0;
        }
        case WM_CLOSE: {
            if (g_closing) return 0;  // the close-game worker is already running
            // close_game closes exactly the process this launcher started / injected
            // (g_gameProc), never a GTA5.exe it did not touch.
            HANDLE game = g_gameProc;
            if (game && GetCloseGame() && WaitForSingleObject(game, 0) == WAIT_TIMEOUT) {
                g_closing = true;
                g_gameProc = 0;
                // Disappear now; the up-to-3 s grace wait runs on a worker so the UI
                // thread never blocks, and the window is destroyed when it reports back.
                ShowWindow(hwnd, SW_HIDE);
                RemoveTray();
                std::thread([game, hwnd]() {
                    bool gone = inject::CloseProcessByHandle(game, 3000);
                    GLOGI("close_game: game process %s", gone ? "closed" : "still running");
                    CloseHandle(game);
                    PostMessageW(hwnd, WM_GAME_CLOSED, 0, 0);
                }).detach();
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_GAME_CLOSED: {
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: {
            RemoveTray();
            KillTimer(hwnd, kTimerStartup);
            KillTimer(hwnd, kTimerUpdateRetry);
            KillTimer(hwnd, kTimerUpdateExit);
            KillTimer(hwnd, kTimerUpdatePoll);
            KillTimer(hwnd, kTimerLaunchWait);
            // Order: remove the handlers, Close the controller, then drop our references.
            if (g_wv) {
                if (g_msgHandler) g_wv->remove_WebMessageReceived(g_msgToken);
                if (g_navHandler) g_wv->remove_NavigationStarting(g_navToken);
                if (g_newWinHandler) g_wv->remove_NewWindowRequested(g_newWinToken);
                if (g_failHandler) g_wv->remove_ProcessFailed(g_failToken);
                if (g_resHandler) g_wv->remove_WebResourceRequested(g_resToken);
                if (g_resFilterAdded) g_wv->RemoveWebResourceRequestedFilter(kAppFilter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            }
            g_resFilterAdded = false;
            if (g_msgHandler) { g_msgHandler->Release(); g_msgHandler = 0; }
            if (g_navHandler) { g_navHandler->Release(); g_navHandler = 0; }
            if (g_newWinHandler) { g_newWinHandler->Release(); g_newWinHandler = 0; }
            if (g_failHandler) { g_failHandler->Release(); g_failHandler = 0; }
            if (g_resHandler) { g_resHandler->Release(); g_resHandler = 0; }
            if (g_ctrl) g_ctrl->Close();
            if (g_wv) { g_wv->Release(); g_wv = 0; }
            if (g_ctrl) { g_ctrl->Release(); g_ctrl = 0; }
            if (g_env) { g_env->Release(); g_env = 0; }
            if (g_gameProc) { CloseHandle(g_gameProc); g_gameProc = 0; }  // close_game off: just let go of it
            PostQuitMessage(g_exitCode);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// Crash capture — if anything dies (loader, COM, our own code), record the
// signature (code, address, containing module + offset) to gage.log and show
// it, so every crash is diagnosable without attaching a debugger.
// ---------------------------------------------------------------------------

static const char* ExcName(DWORD code) {
    switch (code) {
        case 0xC0000005: return "access violation";
        case 0xC000001F: return "illegal instruction";
        case 0xC00000FD: return "stack overflow";
        case 0xC0000374: return "heap corruption";
        case 0xC0000409: return "stack buffer overrun / fast fail";
        case 0xC0000602: return "fail-fast exception";
        case 0x80000003: return "breakpoint";
        case 0xE06D7363: return "C++ exception (unhandled)";
        default: return "unknown";
    }
}

// Precomputed in wWinMain: the filter must not allocate (the heap may be the
// thing that is broken).
static wchar_t g_crashLog[MAX_PATH * 2] = L"";

// "module+0xRVA" for an address, without allocating.
static void DescribeAddr(DWORD64 addr, char* out, size_t cap) {
    HMODULE m = 0;
    char name[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && m) {
        char full[MAX_PATH] = "";
        if (GetModuleFileNameA(m, full, sizeof(full))) {
            const char* b = strrchr(full, '\\');
            lstrcpynA(name, b ? b + 1 : full, sizeof(name));
        }
        snprintf(out, cap, "%s+0x%llx", name[0] ? name : "?", (unsigned long long)(addr - (DWORD64)(uintptr_t)m));
    } else {
        snprintf(out, cap, "<unmapped>0x%llx", (unsigned long long)addr);
    }
}

static void CrashWrite(HANDLE f, const char* s) {
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, s, (DWORD)strlen(s), &w, 0);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    // Owner = id of the thread that entered first (0 = free).
    static volatile LONG owner = 0;
    LONG self = (LONG)GetCurrentThreadId();
    LONG prev = InterlockedCompareExchange(&owner, self, 0);
    if (prev == self) {
        // The filter itself faulted (or its MessageBox pump re-entered a crashing
        // window procedure): waiting for ourselves would hang forever. End the process.
        TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
        for (;;) Sleep(1000);  // not reached; TerminateProcess does not return
    }
    if (prev != 0) {  // another thread is reporting its crash: let it finish
        Sleep(INFINITE);
    }

    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    DWORD code = er->ExceptionCode;
    DWORD64 addr = (DWORD64)(uintptr_t)er->ExceptionAddress;

    char where[MAX_PATH + 32];
    DescribeAddr(addr, where, sizeof(where));
    char line[1024];
    int n = snprintf(line, sizeof(line), "CRASH: code=0x%08lX (%s) at %s addr=0x%llx thread=%lu",
                     (unsigned long)code, ExcName(code), where, (unsigned long long)addr,
                     (unsigned long)GetCurrentThreadId());
    if ((code == 0xC0000005 || code == 0xC0000006) && er->NumberParameters >= 2 && n > 0 && n < (int)sizeof(line)) {
        ULONG_PTR kind = er->ExceptionInformation[0];
        snprintf(line + n, sizeof(line) - n, " %s 0x%llx",
                 kind == 0 ? "reading" : kind == 1 ? "writing" : kind == 8 ? "executing (DEP)" : "accessing",
                 (unsigned long long)er->ExceptionInformation[1]);
    }

    HANDLE f = g_crashLog[0]
                   ? CreateFileW(g_crashLog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, 0)
                   : INVALID_HANDLE_VALUE;
    CrashWrite(f, line);
    CrashWrite(f, "\n");

    // Unwind the faulting context (not this filter's stack) via .pdata.
    CONTEXT c = *ep->ContextRecord;
    for (int i = 0; i < 32; ++i) {
        // Rip == 0 ends the walk, EXCEPT on the faulting frame: a call through a
        // null pointer lands at 0 with the return address still at [rsp], so that
        // frame takes the leaf path below instead of stopping the unwind.
        if (c.Rip == 0 && i != 0) break;
        char fr[MAX_PATH + 48], frame[MAX_PATH + 64];
        DescribeAddr(c.Rip, fr, sizeof(fr));
        snprintf(frame, sizeof(frame), "  #%02d %s\n", i, fr);
        CrashWrite(f, frame);
        DWORD64 base = 0;
        PRUNTIME_FUNCTION rf = c.Rip ? RtlLookupFunctionEntry(c.Rip, &base, 0) : 0;
        if (rf) {
            PVOID hd = 0;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &est, 0);
        } else {  // leaf function: return address is at [rsp]
            DWORD64 ret = 0;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)c.Rsp, &ret, sizeof(ret), &got) ||
                got != sizeof(ret))
                break;
            c.Rip = ret;
            c.Rsp += 8;
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);

    wchar_t wb[1200];
    swprintf(wb, sizeof(wb) / sizeof(wb[0]), L"Solar Launcher crashed.\r\n\r\n%hs\r\n\r\nDetails and stack were written to gage.log.",
             line);
    MessageBoxW(0, wb, L"Solar Launcher — crash details", MB_ICONERROR | MB_OK | MB_TOPMOST);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Log which WebView2 runtime the loader will use (same lookup as the loader).
static void LogWebView2Runtime() {
    LPWSTR v = 0;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(0, &v);
    if (SUCCEEDED(hr) && v) {
        GLOGI("WebView2 runtime available: %s", Narrow(v).c_str());
        CoTaskMemFree(v);
    } else {
        GLOGW("no WebView2 runtime found (hr=0x%08lX)", (unsigned long)hr);
    }
}

// The exe folder when writable (portable install), else %LOCALAPPDATA%\GAGE-MP.
static std::wstring WebViewDataDir() {
    std::wstring d = ExeDir() + L"webview2";
    if (CreateDirectoryW(d.c_str(), 0) || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::wstring probe = d + L"\\.gage_write_test";
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, 0);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return d;
        }
    }
    wchar_t la[MAX_PATH] = L"";
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return d;  // nothing better; StartupFailed reports E_ACCESSDENIED
    std::wstring root = std::wstring(la) + L"\\GAGE-MP";
    CreateDirectoryW(root.c_str(), 0);
    GLOGW("exe folder not writable; WebView2 data goes to %s\\webview2", Narrow(root).c_str());
    return root + L"\\webview2";
}

// Per-monitor DPI awareness (no manifest in this build). Resolved at runtime
// so the exe still starts on systems without the API.
static void EnableDpiAwareness() {
    typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    SetCtxFn fn = u ? (SetCtxFn)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext") : 0;
    if (fn) fn((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
}

// ---------------------------------------------------------------------------
// wWinMain
// ---------------------------------------------------------------------------

// One launcher per session: it owns the WebView2 data folder and the tray icon.
// A second start asks the first window to come forward (registered message) and
// exits. The first instance may still be creating its window, so retry briefly.
// The launcher window of another process: prefer one whose exe is this same file
// (a different copy of the launcher shares the window class), else the first.
// FindWindowEx also returns a window that is hidden to the tray.
static HWND FindOtherInstanceWindow() {
    wchar_t self[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, self, (DWORD)(sizeof(self) / sizeof(self[0])));
    HWND first = nullptr;
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, kClass, nullptr)) != nullptr;) {
        if (!first) first = h;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) continue;
        wchar_t path[MAX_PATH * 2] = L"";
        DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
        bool same = QueryFullProcessImageNameW(p, 0, path, &len) && self[0] && _wcsicmp(path, self) == 0;
        CloseHandle(p);
        if (same) return h;
    }
    return first;
}

static void ActivateRunningInstance() {
    for (int i = 0; i < 30; i++) {
        HWND other = FindOtherInstanceWindow();
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            if (pid) AllowSetForegroundWindow(pid);  // we were just started by the user: lend them the right
            if (PostMessageW(other, g_showMsg, 0, 0)) GLOGI("another instance is running (pid %lu): asked it to show its window", (unsigned long)pid);
            else GLOGW("another instance is running (pid %lu) but could not be signalled (err=%lu)", (unsigned long)pid, GetLastError());
            return;
        }
        Sleep(100);
    }
    GLOGW("another instance holds the launcher mutex but shows no window; exiting anyway");
}

// The window opens on about a third of the work area, in the menu's own
// 1240x800 proportions (the SPA scales its layout up to match), never below
// kBaseClient{W,H} DIP and never past kMaxWorkShare of either side. On a
// 2560x1392 work area that is 1356x875; on 1920x1040, 1240x800.
static const double kOpenAreaShare = 1.0 / 3.0;
static const double kMaxWorkShare = 0.9;

// Client area as above at `dpi`, plus the invisible sizing frame
// (left/right/bottom), clamped to the work area and centred in it.
static void PlaceWindow(UINT dpi, int* x, int* y, int* W, int* H) {
    RECT wa = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
    int cw = MulDiv(kBaseClientW, (int)dpi, 96);
    int ch = MulDiv(kBaseClientH, (int)dpi, 96);
    // width for a third of the area at the base aspect: w*h = A/3, h = w*800/1240
    const double share = std::sqrt((double)waW * waH * kOpenAreaShare * kBaseClientW / kBaseClientH);
    if (share > cw) {
        cw = (int)share;
        ch = MulDiv(cw, kBaseClientH, kBaseClientW);
    }
    if (cw > waW * kMaxWorkShare || ch > waH * kMaxWorkShare) {
        const double fit = std::min(waW * kMaxWorkShare / cw, waH * kMaxWorkShare / ch);
        if (fit < 1.0 && cw * fit >= MulDiv(kBaseClientW, (int)dpi, 96)) {
            cw = (int)(cw * fit);
            ch = (int)(ch * fit);
        }
    }
    *W = cw + 2 * FrameX(dpi);
    *H = ch + FrameY(dpi);
    if (wa.right - wa.left < *W) *W = wa.right - wa.left;
    if (wa.bottom - wa.top < *H) *H = wa.bottom - wa.top;
    *x = (wa.left + wa.right - *W) / 2;
    *y = (wa.top + wa.bottom - *H) / 2;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nShow) {
    g_hInst = hInstance;
    g_uiThread = GetCurrentThreadId();
    lstrcpynW(g_crashLog, (ExeDir() + L"gage.log").c_str(), (int)(sizeof(g_crashLog) / sizeof(g_crashLog[0])));
    SetUnhandledExceptionFilter(CrashFilter);
    ULONG guarantee = 64 * 1024;  // room for CrashFilter after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    EnableDpiAwareness();

    // Single instance (the handle is held until the process ends).
    g_showMsg = RegisterWindowMessageW(kShowMessageName);
    SetLastError(0);
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    DWORD mutexErr = GetLastError();
    // ACCESS_DENIED: the other instance runs at a higher integrity level (elevated).
    if ((instanceMutex && mutexErr == ERROR_ALREADY_EXISTS) || (!instanceMutex && mutexErr == ERROR_ACCESS_DENIED)) {
        ActivateRunningInstance();
        ExitProcess(0);
    }
    if (!instanceMutex) GLOGW("single-instance mutex unavailable (err=%lu); continuing without it", mutexErr);

    GLOGI("Solar Launcher (GUI) starting, pid=%lu", (unsigned long)GetCurrentProcessId());
    legacy::RunOnce(ExeDir());  // one-time: drop the old launcher's values from the RAGE-MP registry key
    HRESULT hir = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    GLOGI("CoInitializeEx(hr=0x%08lX)", (unsigned long)hir);
    LogWebView2Runtime();
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = AppIcon(SM_CXICON, SM_CYICON);
    wc.hIconSm = AppIcon(SM_CXSMICON, SM_CYSMICON);
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    // The page colour, not white: this is what shows while the window is resized
    // faster than the WebView repaints.
    wc.hbrBackground = CreateSolidBrush(RGB(kBgColor.R, kBgColor.G, kBgColor.B));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    UINT dpi = SystemDpi();
    int x, y, W, H;
    PlaceWindow(dpi, &x, &y, &W, &H);

    // WS_CLIPCHILDREN: the WebView child is never painted over by this window.
    g_hwnd = CreateWindowExW(0, kClass, kTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, W, H, 0, 0, hInstance, 0);
    if (!g_hwnd) {
        GLOGE("CreateWindowExW failed err=%lu", GetLastError());
        if (SUCCEEDED(hir)) CoUninitialize();
        return 1;
    }
    UINT wdpi = WindowDpi(g_hwnd);
    if (wdpi != dpi) {  // landed on a monitor whose scale differs from the system's
        PlaceWindow(wdpi, &x, &y, &W, &H);
        SetWindowPos(g_hwnd, 0, x, y, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ApplyFrame(g_hwnd);
    // An elevated launcher would otherwise never see these from a lower-integrity sender.
    if (g_taskbarCreated) ChangeWindowMessageFilterEx(g_hwnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    if (g_showMsg) ChangeWindowMessageFilterEx(g_hwnd, g_showMsg, MSGFLT_ALLOW, nullptr);

    if (nShow == SW_SHOWDEFAULT) nShow = SW_SHOW;
    ShowWindow(g_hwnd, GetStartMinimized() ? SW_SHOWMINIMIZED : nShow);
    UpdateWindow(g_hwnd);

    // Look for an update from the first second, whatever the page is doing (docs/UPDATES.md).
    GLOGI("update: client %s, manifest %s, signing key fingerprint %s", Narrow(kClientVersion).c_str(),
          Narrow(kDefaultVersionUrl).c_str(), updater::Sha256Hex(kUpdatePubKey, strlen(kUpdatePubKey)).substr(0, 12).c_str());
    KickUpdate();
    SetTimer(g_hwnd, kTimerUpdatePoll, kUpdatePollMs, nullptr);

    // Embedded SPA in WebView2; loader is in-tree (webview2_loader.cpp). If the
    // controller never arrives (hung runtime, blocked browser process), say so
    // instead of leaving a blank window.
    SetTimer(g_hwnd, kTimerStartup, kStartupTimeoutMs, nullptr);
    std::wstring ud = WebViewDataDir();
    GLOGI("requesting WebView2 environment, userDataFolder=%s", Narrow(ud).c_str());
    EnvHandler* eh = new EnvHandler(g_hwnd);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(0, ud.c_str(), 0, eh);
    eh->Release();  // creator's ref; WebView2 holds its own across the async callback
    if (FAILED(hr)) StartupFailed(L"CreateCoreWebView2EnvironmentWithOptions", hr);
    else GLOGI("env request dispatched — waiting for async callback (browser host launch)");

    MSG m = {};
    BOOL r;
    while ((r = GetMessageW(&m, 0, 0, 0)) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (r < 0) GLOGE("GetMessageW failed err=%lu", GetLastError());

    if (SUCCEEDED(hir)) CoUninitialize();
    int code = r < 0 ? 1 : (int)m.wParam;
    GLOGI("Solar Launcher (GUI) exiting, code=%d", code);
    // Detached workers (fetch, ping, update, launch) may still be running. Returning
    // would run CRT teardown under them; ExitProcess ends them with the process.
    ExitProcess((UINT)code);
    return code;  // not reached
}
