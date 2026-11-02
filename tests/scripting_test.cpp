// scripting_test: the gamemode scripting runtime (libs/scripting) against a fake server.
//
// Every test builds a throw-away resources/ and data/ folder under the system temp directory,
// starts a ScriptRuntime on a FakeApi (the IServerApi the real server will implement) and drives
// it the way the server would: OnPlayerJoined(), OnChatMessage(), Tick() with a simulated clock.
// Scripts report what they saw through log.info(); the fake collects every log line, so a check
// reads real runtime output and never the intent of the script.
//
// Run from the repository root (it reads libs/scripting/examples and docs/api/gamemode.md), or
// pass the root as argv[1]. Exit code 0 only when every check passed AND the expected number of
// checks ran. A watchdog thread kills the process if a runaway script is not aborted
// (GAGE_TEST_WATCHDOG_S, default 120 s).
#include "scripting/fsutil.h"
#include "scripting/scripting.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define GAGE_GETPID _getpid
#else
#include <unistd.h>
#define GAGE_GETPID getpid
#endif

namespace fs = std::filesystem;
using namespace gage::scripting;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got = "") {
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, (ok || got.empty()) ? "" : "  got: ", ok ? "" : got.c_str());
    std::fflush(stdout);
}

static void Eq(const char* name, const std::string& got, const std::string& want) {
    Check(name, got == want, "\"" + got + "\"  want \"" + want + "\"");
}

static bool Has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ---- the fake server ------------------------------------------------------------------------

struct FakeApi : IServerApi {
    struct Line { LogLevel level; std::string text; };
    struct Chat { int to; std::string text; };       // to < 0: everyone
    struct Kicked { int id; std::string reason; };
    struct Moved { int id; Position pos; };

    std::vector<Line> logs;
    uint64_t now = 100000;
    std::vector<PlayerInfo> players;
    std::vector<Chat> chats;
    std::vector<Kicked> kicks;
    std::vector<std::string> bans, banIps, unbans;
    std::vector<Moved> moves;
    bool allowMove = false;
    std::map<std::string, std::string> convars;
    ScriptRuntime* rt = nullptr;
    bool kickReenters = false;

    void Log(LogLevel level, const std::string& line) override { logs.push_back({level, line}); }
    uint64_t NowMs() override { return now; }
    std::vector<PlayerInfo> Players() override { return players; }

    PlayerInfo* Find(uint16_t id) {
        for (PlayerInfo& p : players)
            if (p.id == id) return &p;
        return nullptr;
    }
    bool GetPlayer(uint16_t id, PlayerInfo& out) override {
        PlayerInfo* p = Find(id);
        if (!p) return false;
        out = *p;
        return true;
    }
    bool SendChat(uint16_t to, const std::string& text, std::string* err) override {
        if (!Find(to)) {
            if (err) *err = "no player " + std::to_string(to);
            return false;
        }
        chats.push_back({to, text});
        return true;
    }
    bool SendChatAll(const std::string& text, std::string*) override {
        chats.push_back({-1, text});
        return true;
    }
    bool Kick(uint16_t id, const std::string& reason, std::string* err) override {
        PlayerInfo* p = Find(id);
        if (!p) {
            if (err) *err = "no player " + std::to_string(id);
            return false;
        }
        kicks.push_back({id, reason});
        if (kickReenters && rt) {
            // What the real server does: the connection closes and RemovePlayer() runs at once,
            // from inside this call, calling back into the runtime.
            const PlayerInfo info = *p;
            for (size_t i = 0; i < players.size(); i++)
                if (players[i].id == id) { players.erase(players.begin() + static_cast<std::ptrdiff_t>(i)); break; }
            rt->OnPlayerLeft(info, LeaveReason::Kicked);
        }
        return true;
    }
    bool Ban(uint16_t id, const std::string& reason, std::string* msg) override {
        if (!Find(id)) {
            if (msg) *msg = "no player " + std::to_string(id);
            return false;
        }
        bans.push_back(std::to_string(id) + ":" + reason);
        return true;
    }
    bool BanIp(const std::string& ip, const std::string& reason, std::string*) override {
        banIps.push_back(ip + ":" + reason);
        return true;
    }
    bool Unban(const std::string& target, std::string*) override {
        unbans.push_back(target);
        return true;
    }
    bool SetPosition(uint16_t id, const Position& pos, std::string* err) override {
        if (!allowMove) {
            if (err) *err = "setPosition needs protocol support the client does not have yet";
            return false;
        }
        moves.push_back({id, pos});
        return true;
    }
    bool GetConvar(const std::string& name, std::string& out) override {
        auto it = convars.find(name);
        if (it == convars.end()) return false;
        out = it->second;
        return true;
    }
};

// ---- log queries --------------------------------------------------------------------------------

static size_t Count(const FakeApi& a, const std::string& needle) {
    size_t n = 0;
    for (const auto& l : a.logs)
        if (Has(l.text, needle)) n++;
    return n;
}

static long First(const FakeApi& a, const std::string& needle) {
    for (size_t i = 0; i < a.logs.size(); i++)
        if (Has(a.logs[i].text, needle)) return static_cast<long>(i);
    return -1;
}

// Log lines containing both strings.
static size_t CountAll(const FakeApi& a, const std::string& x, const std::string& y) {
    size_t n = 0;
    for (const auto& l : a.logs)
        if (Has(l.text, x) && Has(l.text, y)) n++;
    return n;
}

// The text after `prefix` on the first log line containing it, up to the end of that line.
// A missing line yields "<missing: prefix>", so an equality check cannot pass by accident.
static std::string Mark(const FakeApi& a, const std::string& prefix) {
    for (const auto& l : a.logs) {
        const size_t p = l.text.find(prefix);
        if (p == std::string::npos) continue;
        std::string rest = l.text.substr(p + prefix.size());
        const size_t nl = rest.find('\n');
        return nl == std::string::npos ? rest : rest.substr(0, nl);
    }
    return "<missing: " + prefix + ">";
}

// Every log line containing `needle`, newline separated (for failure messages).
static std::string Dump(const FakeApi& a, const std::string& needle) {
    std::string out;
    for (const auto& l : a.logs)
        if (Has(l.text, needle)) out += l.text.substr(0, 300) + " | ";
    return out.empty() ? "<no line contains \"" + needle + "\">" : out;
}

// The values after `marker` on every line containing it, joined with ','.
static std::string Marks(const FakeApi& a, const std::string& marker) {
    std::string out;
    for (const auto& l : a.logs) {
        const size_t p = l.text.find(marker);
        if (p == std::string::npos) continue;
        std::string rest = l.text.substr(p + marker.size());
        const size_t nl = rest.find('\n');
        if (nl != std::string::npos) rest = rest.substr(0, nl);
        out += (out.empty() ? "" : ",") + rest;
    }
    return out;
}

// ---- scratch folders ----------------------------------------------------------------------------

static fs::path TestRoot() {
    static fs::path root;
    if (root.empty()) {
        std::error_code ec;
        root = fs::temp_directory_path(ec) / ("gage_scripting_test_" + std::to_string(GAGE_GETPID()));
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
    }
    return root;
}

static void WriteFile(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

struct Env {
    fs::path root;
    FakeApi api;
    RuntimeOptions opt;
    std::unique_ptr<ScriptRuntime> rt;
    std::string name_;
    int failedAtStart_ = g_failed;

    explicit Env(const std::string& name) : name_(name) {
        root = TestRoot() / name;
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "resources", ec);
        opt.resourcesDir = (root / "resources").generic_string();
        opt.dataDir = (root / "data").generic_string();
    }
    void Make() {
        rt.reset(new ScriptRuntime(api, opt));
        api.rt = rt.get();
    }
    fs::path Dir(const std::string& res) const { return root / "resources" / res; }
    fs::path Data(const std::string& res) const { return root / "data" / res; }
    ~Env() {
        api.rt = nullptr;
        rt.reset();
        if (g_failed > failedAtStart_) {
            // A check of this scenario failed: show what the runtime logged as a warning or error.
            std::printf("      -- warnings and errors logged in scenario '%s':\n", name_.c_str());
            for (const auto& l : api.logs)
                if (l.level != LogLevel::Info) std::printf("      %s\n", l.text.substr(0, 400).c_str());
            std::fflush(stdout);
        }
    }
};

static std::string Quote(const std::string& s) { return "\"" + s + "\""; }

// Writes resources/<name>/resource.json and main.lua.
static void Res(Env& e, const std::string& name, const std::string& lua, std::vector<std::string> deps = {},
                const std::string& configJson = "") {
    std::string m = "{\"name\":" + Quote(name) + ",\"version\":\"1.0.0\",\"server_scripts\":[\"main.lua\"]";
    if (!deps.empty()) {
        m += ",\"dependencies\":[";
        for (size_t i = 0; i < deps.size(); i++) m += (i ? "," : "") + Quote(deps[i]);
        m += "]";
    }
    if (!configJson.empty()) m += ",\"config\":" + configJson;
    m += "}";
    WriteFile(e.Dir(name) / "resource.json", m);
    WriteFile(e.Dir(name) / "main.lua", lua);
}

static PlayerInfo MkPlayer(uint16_t id, const std::string& name, bool admin = false) {
    PlayerInfo p;
    p.id = id;
    p.name = name;
    p.address = "10.0.0." + std::to_string(id);
    p.accountId = "name:" + name;
    p.pingMs = 20u + id;
    p.isAdmin = admin;
    return p;
}

static void Join(Env& e, const PlayerInfo& p) {
    e.api.players.push_back(p);
    e.rt->OnPlayerJoined(p.id);
}

static void Leave(Env& e, uint16_t id, LeaveReason why) {
    PlayerInfo* p = e.api.Find(id);
    if (!p) return;
    const PlayerInfo info = *p;
    for (size_t i = 0; i < e.api.players.size(); i++)
        if (e.api.players[i].id == id) { e.api.players.erase(e.api.players.begin() + static_cast<std::ptrdiff_t>(i)); break; }
    e.rt->OnPlayerLeft(info, why);
}

static ResourceStatus Status(Env& e, const std::string& name) {
    for (const ResourceStatus& s : e.rt->Resources())
        if (s.name == name) return s;
    return ResourceStatus();
}

static const char* StateName(ResourceState s) {
    switch (s) {
    case ResourceState::Stopped: return "Stopped";
    case ResourceState::Running: return "Running";
    case ResourceState::Failed: return "Failed";
    case ResourceState::Blocked: return "Blocked";
    }
    return "?";
}

static double Seconds(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ---- 1. manifests, load order, cycles -----------------------------------------------------------

static void TestLoadOrder() {
    {
        Env e("order");
        Res(e, "zeta", R"LUA(log.info("LOAD zeta"))LUA");
        Res(e, "alpha", R"LUA(log.info("LOAD alpha"))LUA");
        Res(e, "core", R"LUA(log.info("LOAD core"))LUA");
        Res(e, "lib", R"LUA(log.info("LOAD lib"))LUA", {"core"});
        Res(e, "app", R"LUA(log.info("LOAD app"))LUA", {"lib", "core"});
        e.Make();
        const size_t n = e.rt->StartAll();
        Check("StartAll starts every valid resource", n == 5, std::to_string(n));
        Eq("dependencies load first, ties by name", Marks(e.api, "LOAD "), "alpha,core,lib,app,zeta");
        Check("every resource is Running", Status(e, "app").state == ResourceState::Running && Status(e, "zeta").state == ResourceState::Running);
    }
    {
        Env e("cycle");
        Res(e, "c1", R"LUA(log.info("LOAD c1"))LUA", {"c2"});
        Res(e, "c2", R"LUA(log.info("LOAD c2"))LUA", {"c1"});
        Res(e, "c3", R"LUA(log.info("LOAD c3"))LUA", {"c1"});
        Res(e, "fine", R"LUA(log.info("LOAD fine"))LUA");
        e.rt.reset();
        e.Make();
        const size_t n = e.rt->StartAll();
        Check("a dependency cycle does not stop unrelated resources", n == 1 && e.rt->IsRunning("fine"), std::to_string(n));
        Check("no resource of the cycle ran", Count(e.api, "LOAD c") == 0, Dump(e.api, "LOAD c"));
        const ResourceStatus c1 = Status(e, "c1");
        Check("cycle member is Blocked with the cycle in its detail", c1.state == ResourceState::Blocked && Has(c1.detail, "cycle"),
              std::string(StateName(c1.state)) + " " + c1.detail);
        const ResourceStatus c3 = Status(e, "c3");
        Check("a dependent of the cycle is Blocked and names it", c3.state == ResourceState::Blocked && Has(c3.detail, "c1"),
              std::string(StateName(c3.state)) + " " + c3.detail);
        std::string err;
        const bool ok = e.rt->Start("c1", &err);
        Check("Start() of a cycle member is rejected with a reason", !ok && Has(err, "cycle"), err);
        Check("the cycle is logged as an error", Count(e.api, "dependency cycle") >= 1);
    }
    {
        Env e("missingdep");
        Res(e, "needs", R"LUA(log.info("LOAD needs"))LUA", {"ghost"});
        e.Make();
        std::string err;
        const bool ok = e.rt->Start("needs", &err);
        Check("a missing dependency refuses the start and names it", !ok && Has(err, "ghost"), err);
        Check("the dependent did not run", Count(e.api, "LOAD needs") == 0);
        Check("it is reported Blocked", Status(e, "needs").state == ResourceState::Blocked, StateName(Status(e, "needs").state));
        std::string err2;
        const bool ok2 = e.rt->Start("nonexistent", &err2);
        Check("starting an unknown resource fails with a reason", !ok2 && !err2.empty(), err2);
        Check("an unknown resource leaves no entry", Status(e, "nonexistent").name.empty());
    }
    {
        Env e("badmanifest");
        WriteFile(e.Dir("typo") / "resource.json", R"JSON({"name":"typo","version":"1","server_script":["main.lua"]})JSON");
        WriteFile(e.Dir("wrongname") / "resource.json", R"JSON({"name":"other","version":"1"})JSON");
        WriteFile(e.Dir("selfdep") / "resource.json", R"JSON({"name":"selfdep","version":"1","dependencies":["selfdep"]})JSON");
        WriteFile(e.Dir("escape") / "resource.json", R"JSON({"name":"escape","version":"1","server_scripts":["../x.lua"]})JSON");
        Res(e, "good", R"LUA(log.info("LOAD good"))LUA");
        e.Make();
        const size_t valid = e.rt->Refresh();
        Check("Refresh counts only the valid manifest", valid == 1, std::to_string(valid));
        const ResourceStatus typo = Status(e, "typo");
        Check("an unknown manifest key is an error that lists the known keys",
              typo.state == ResourceState::Failed && Has(typo.detail, "unknown key") && Has(typo.detail, "server_scripts"), typo.detail);
        Check("a name that differs from the folder is an error", Status(e, "wrongname").state == ResourceState::Failed && Has(Status(e, "wrongname").detail, "folder"),
              Status(e, "wrongname").detail);
        Check("a resource cannot depend on itself", Status(e, "selfdep").state == ResourceState::Failed, Status(e, "selfdep").detail);
        Check("a script path that leaves the folder is refused", Status(e, "escape").state == ResourceState::Failed, Status(e, "escape").detail);
        std::string err;
        Check("Start() of an invalid manifest fails", !e.rt->Start("typo", &err) && Has(err, "unknown key"), err);
        Check("the invalid manifest was logged once by Refresh", Count(e.api, "resource 'typo' ignored") == 1, std::to_string(Count(e.api, "resource 'typo' ignored")));
    }
    {
        Env e("loadfail");
        Res(e, "good", R"LUA(on("playerJoined", function() log.info("GOOD saw join") end))LUA");
        Res(e, "syn", "this is not lua");
        Res(e, "rt", R"LUA(error("load boom"))LUA");
        Res(e, "partial", R"LUA(
on("playerJoined", function() log.info("PARTIAL SHOULD NOT RUN") end)
setInterval(function() log.info("PARTIAL TIMER SHOULD NOT RUN") end, 10)
registerCommand("pc", function() end)
error("late failure")
)LUA");
        WriteFile(e.Dir("nofile") / "resource.json", R"JSON({"name":"nofile","version":"1","server_scripts":["absent.lua"]})JSON");
        e.Make();
        e.rt->Start("good");
        std::string err;
        Check("a syntax error fails the start and names the file", !e.rt->Start("syn", &err) && Has(err, "main.lua"), err);
        Check("a runtime error at load fails the start with its message", !e.rt->Start("rt", &err) && Has(err, "load boom"), err);
        Check("a missing script file fails the start", !e.rt->Start("nofile", &err) && Has(err, "absent.lua"), err);
        Check("a failed start leaves the resource Failed, not running",
              Status(e, "syn").state == ResourceState::Failed && !e.rt->IsRunning("syn"), StateName(Status(e, "syn").state));
        Check("a start that fails after registering leaves nothing registered", !e.rt->Start("partial", &err));
        const RuntimeStats st = e.rt->Stats();
        Check("no handler, timer or command of the failed start remains", st.running == 1 && st.handlers == 1 && st.timers == 0 && st.commands == 0,
              "running=" + std::to_string(st.running) + " handlers=" + std::to_string(st.handlers) + " timers=" + std::to_string(st.timers) +
                  " commands=" + std::to_string(st.commands));
        Join(e, MkPlayer(1, "Alice"));
        Check("the leftovers of the failed start do not run", Count(e.api, "PARTIAL") == 0 && Count(e.api, "GOOD saw join") == 1);
        e.api.now += 1000;
        e.rt->Tick();
        Check("its timer does not fire", Count(e.api, "PARTIAL TIMER") == 0);
        Check("the failed start is explained in the log", Count(e.api, "failed to start") >= 4);
    }
}

// ---- 2. start / stop / restart cleanup -------------------------------------------------------------

static void TestStopCleanup() {
    {
        Env e("cleanup");
        Res(e, "cl", R"LUA(
on("playerJoined", function(p) log.info("CL joined " .. p.name) end)
setInterval(function() log.info("CL tick") end, 100)
setTimeout(function() log.info("CL once") end, 100)
registerCommand("cl", function(src) reply(src, "cl here") end)
export("f", function() return 1 end)
log.info("CL loaded")
)LUA");
        Res(e, "other", R"LUA(on("playerJoined", function(p) log.info("OTHER joined " .. p.name) end))LUA");
        e.Make();
        e.rt->StartAll();
        RuntimeStats st = e.rt->Stats();
        Check("running resource has its handlers, timers and command registered",
              st.running == 2 && st.handlers == 2 && st.timers == 2 && st.commands == 1,
              "running=" + std::to_string(st.running) + " handlers=" + std::to_string(st.handlers) + " timers=" + std::to_string(st.timers) +
                  " commands=" + std::to_string(st.commands));
        Check("Resources() reports its live figures", Status(e, "cl").exports == 1 && Status(e, "cl").commands == 1 && Status(e, "cl").timers == 2 &&
                                                          Status(e, "cl").memoryBytes > 0);
        e.api.now += 100;
        e.rt->Tick();
        Check("timers fire while it runs", Count(e.api, "CL tick") == 1 && Count(e.api, "CL once") == 1);

        std::string err;
        Check("Stop() succeeds", e.rt->Stop("cl", &err), err);
        st = e.rt->Stats();
        Check("Stop() removes its handlers, timers and commands", st.running == 1 && st.handlers == 1 && st.timers == 0 && st.commands == 0,
              "running=" + std::to_string(st.running) + " handlers=" + std::to_string(st.handlers) + " timers=" + std::to_string(st.timers) +
                  " commands=" + std::to_string(st.commands));
        Check("the stopped resource is Stopped, not Failed", Status(e, "cl").state == ResourceState::Stopped, StateName(Status(e, "cl").state));
        e.api.now += 1000;
        e.rt->Tick();
        Check("its interval no longer fires", Count(e.api, "CL tick") == 1, std::to_string(Count(e.api, "CL tick")));
        Join(e, MkPlayer(1, "Alice"));
        Check("its handler no longer runs, others still do", Count(e.api, "CL joined") == 0 && Count(e.api, "OTHER joined Alice") == 1);
        const bool broadcast = e.rt->OnChatMessage(1, "/cl");
        Check("its command is gone: the server is told not to broadcast and the player hears so",
              !broadcast && !e.api.chats.empty() && Has(e.api.chats.back().text, "Unknown command"),
              e.api.chats.empty() ? "no chat" : e.api.chats.back().text);
        Check("stopping twice reports it is not running", !e.rt->Stop("cl", &err) && Has(err, "not running"), err);
        Check("closing the state left no memory accounting mismatch", Count(e.api, "memory accounting mismatch") == 0);

        Res(e, "cl2", R"LUA(log.info("CL2 reg " .. tostring(registerCommand("cl", function() end))))LUA");
        Check("the command name is free again for another resource", e.rt->Start("cl2") && Mark(e.api, "CL2 reg ") == "true", Mark(e.api, "CL2 reg "));
        e.rt->Stop("cl2");

        Check("a stopped resource can be started again", e.rt->Start("cl", &err), err);
        Check("Restart() succeeds", e.rt->Restart("cl", &err), err);
        st = e.rt->Stats();
        Check("a restart does not duplicate timers, handlers or commands", st.handlers == 2 && st.timers == 2 && st.commands == 1,
              "handlers=" + std::to_string(st.handlers) + " timers=" + std::to_string(st.timers) + " commands=" + std::to_string(st.commands));
        Check("each start ran the script once", Count(e.api, "CL loaded") == 3, std::to_string(Count(e.api, "CL loaded")));
    }
    {
        Env e("dependents");
        Res(e, "base", R"LUA(
export("v", function() return 7 end)
on("resourceStop", function(name) log.info("STOPEVT " .. resource.name .. " saw " .. name) end)
)LUA");
        Res(e, "dep", R"LUA(
on("resourceStop", function(name) log.info("STOPEVT " .. resource.name .. " saw " .. name) end)
log.info("DEP sees " .. exports.base.v())
)LUA",
            {"base"});
        e.Make();
        e.rt->StartAll();
        Check("the dependent started after its dependency and could call it", Mark(e.api, "DEP sees ") == "7", Mark(e.api, "DEP sees "));
        std::string err;
        Check("stopping a dependency succeeds", e.rt->Stop("base", &err), err);
        Check("its dependent stops with it", !e.rt->IsRunning("dep") && !e.rt->IsRunning("base"));
        Check("the dependent stops first", First(e.api, "resource 'dep' stopped") >= 0 && First(e.api, "resource 'dep' stopped") < First(e.api, "resource 'base' stopped"));
        Check("both got the resourceStop event while still alive", Count(e.api, "STOPEVT dep saw dep") == 1 && Count(e.api, "STOPEVT base saw base") == 1 &&
                                                                       Count(e.api, "STOPEVT base saw dep") == 1,
              Dump(e.api, "STOPEVT"));
        Check("Start() of a dependent starts its dependency first", e.rt->Start("dep", &err) && e.rt->IsRunning("base") && e.rt->IsRunning("dep"), err);
        Check("Restart() of a dependency restarts the dependents it took down",
              e.rt->Restart("base", &err) && e.rt->IsRunning("base") && e.rt->IsRunning("dep") && Count(e.api, "DEP sees ") == 3, err + " " + std::to_string(Count(e.api, "DEP sees ")));
        e.rt->Shutdown();
        e.rt->Shutdown();
        Check("Shutdown() stops everything and is idempotent", e.rt->Stats().running == 0);
    }
}

// ---- 3. error isolation ---------------------------------------------------------------------------------

static void TestErrorIsolation() {
    Env e("errors");
    Res(e, "bad", R"LUA(
on("playerJoined", function(p) error("boom from bad") end)
on("playerJoined", function(p) log.info("BAD second handler ran") end)
on("chatMessage", function(p, t) local x = nil; return x.field end)
registerCommand("crash", function() error("command boom") end)
setTimeout(function() error("timer boom") end, 10)
setTimeout(function() log.info("BAD second timer ran") end, 10)
)LUA");
    Res(e, "good", R"LUA(
on("playerJoined", function(p) log.info("GOOD saw " .. p.name) end)
on("chatMessage", function(p, t) log.info("GOOD chat " .. t) end)
)LUA");
    e.Make();
    e.rt->StartAll();
    Join(e, MkPlayer(1, "Alice"));
    Check("a handler error is logged with the resource and the message",
          Count(e.api, "[bad]") >= 1 && Has(Dump(e.api, "boom from bad"), "playerJoined"), Dump(e.api, "boom"));
    Check("the log carries a traceback", Has(Dump(e.api, "boom from bad"), "stack traceback"));
    Check("the error is logged at Error level", [&] {
        for (const auto& l : e.api.logs)
            if (Has(l.text, "boom from bad")) return l.level == LogLevel::Error;
        return false;
    }());
    Check("the next handler of the same resource still runs", Count(e.api, "BAD second handler ran") == 1);
    Check("another resource still gets the event", Count(e.api, "GOOD saw Alice") == 1);
    Check("both resources are still running", e.rt->IsRunning("bad") && e.rt->IsRunning("good"));

    const bool broadcast = e.rt->OnChatMessage(1, "hello");
    Check("an error in a chat handler does not cancel the message", broadcast);
    Check("the other chat handler ran", Count(e.api, "GOOD chat hello") == 1);
    Check("the chat error was logged", Count(e.api, "attempt to index a nil value") >= 1);

    e.rt->OnChatMessage(1, "/crash");
    Check("a command error is logged", Count(e.api, "command boom") >= 1);
    Check("the player is told the command failed", !e.api.chats.empty() && Has(e.api.chats.back().text, "command failed"),
          e.api.chats.empty() ? "no chat" : e.api.chats.back().text);

    e.api.now += 50;
    e.rt->Tick();
    Check("a timer error is logged and the next timer still fires", Count(e.api, "timer boom") >= 1 && Count(e.api, "BAD second timer ran") == 1);
    Check("no error escaped to the host: the runtime is still usable", e.rt->Stats().running == 2);
}

// ---- 4. budgets: instruction count, time, pattern steps, memory ---------------------------------------

static void TestBudgets() {
    {
        Env e("budget");
        e.opt.limits.instructionBudget = 2000000;
        e.opt.limits.timeBudgetMs = 10000;
        Res(e, "hog", R"LUA(
on("playerJoined", function() log.info("HOG entered"); while true do end end)
on("playerLeft", function() log.info("HOG left handler") end)
)LUA");
        Res(e, "catcher", R"LUA(
on("playerJoined", function()
  local ok = pcall(function() while true do end end)
  log.info("AFTER PCALL ok=" .. tostring(ok))
end)
)LUA");
        Res(e, "good", R"LUA(on("playerJoined", function(p) log.info("GOOD still runs for " .. p.name) end))LUA");
        e.Make();
        e.rt->StartAll();
        const auto t0 = std::chrono::steady_clock::now();
        Join(e, MkPlayer(1, "Alice"));
        const double secs = Seconds(t0);
        Check("an infinite loop is aborted by the instruction budget", Count(e.api, "instruction budget exceeded") >= 1, Dump(e.api, "hog"));
        Check("it returned quickly instead of hanging the tick", secs < 10.0, std::to_string(secs));
        Check("the abort is logged with the resource and the entry",
              Count(e.api, "'hog' aborted in handler for 'playerJoined'") == 1, Dump(e.api, "aborted"));
        Check("a pcall around the runaway loop does not swallow the abort", Count(e.api, "AFTER PCALL") == 0 && CountAll(e.api, "'catcher' aborted", "instruction budget") == 1, Dump(e.api, "catcher"));
        Check("other resources still get the event after a runaway one", Count(e.api, "GOOD still runs for Alice") == 1);
        Check("a single violation does not stop the resource", e.rt->IsRunning("hog"));
        Join(e, MkPlayer(2, "Bob"));
        Check("the next violation is counted", CountAll(e.api, "'hog' aborted", "violation 2 in a row") == 1, Dump(e.api, "violation"));
        Join(e, MkPlayer(3, "Cara"));
        Check("three violations in a row stop the resource", !e.rt->IsRunning("hog"));
        const ResourceStatus hs = Status(e, "hog");
        Check("it is reported Failed with the reason", hs.state == ResourceState::Failed && Has(hs.detail, "budget"), std::string(StateName(hs.state)) + " " + hs.detail);
        const size_t entered = Count(e.api, "HOG entered");
        Join(e, MkPlayer(4, "Dora"));
        Check("a stopped resource is not entered again", Count(e.api, "HOG entered") == entered && entered == 3, std::to_string(Count(e.api, "HOG entered")));
        Check("the rest of the server is unaffected", e.rt->IsRunning("good") && Count(e.api, "GOOD still runs for Dora") == 1);
        Check("an aborted entry left no memory accounting mismatch", Count(e.api, "memory accounting mismatch") == 0);
    }
    {
        Env e("coroutine");
        e.opt.limits.instructionBudget = 1000000;
        Res(e, "co", R"LUA(
on("playerJoined", function()
  local co = coroutine.create(function() while true do end end)
  local ok, err = coroutine.resume(co)
  log.info("CO resumed ok=" .. tostring(ok))
  log.info("CO AFTER")
end)
on("playerLeft", function()
  local wrapped = coroutine.wrap(function() local n = 0 while true do n = n + 1 end end)
  wrapped()
  log.info("CO WRAP AFTER")
end)
)LUA");
        e.Make();
        e.rt->StartAll();
        Join(e, MkPlayer(1, "Alice"));
        Check("a runaway loop inside a coroutine is aborted", Count(e.api, "'co' aborted in handler for 'playerJoined'") == 1 && Count(e.api, "instruction budget exceeded") >= 1, Dump(e.api, "co"));
        Check("a script that survives coroutine.resume() is stopped at its next instruction", Count(e.api, "CO resumed") == 0 && Count(e.api, "CO AFTER") == 0, Dump(e.api, "CO "));
        Leave(e, 1, LeaveReason::Quit);
        Check("coroutine.wrap propagates the abort and nothing after it runs", Count(e.api, "CO WRAP AFTER") == 0 && Count(e.api, "'co' aborted in handler for 'playerLeft'") == 1, Dump(e.api, "co"));
    }
    {
        Env e("flaky");
        e.opt.limits.instructionBudget = 1000000;
        Res(e, "flaky", R"LUA(
local calls = 0
on("playerJoined", function()
  calls = calls + 1
  if calls % 3 ~= 0 then while true do end end
  log.info("FLAKY ok on call " .. calls)
end)
)LUA");
        e.Make();
        e.rt->StartAll();
        for (uint16_t i = 1; i <= 9; i++) Join(e, MkPlayer(i, "P" + std::to_string(i)));
        Check("violations must be consecutive: a good entry resets the count", e.rt->IsRunning("flaky") && Count(e.api, "FLAKY ok on call") == 3,
              std::to_string(Count(e.api, "FLAKY ok on call")));
    }
    {
        Env e("loadloop");
        e.opt.limits.instructionBudget = 1000000;
        Res(e, "spin", R"LUA(while true do end)LUA");
        Res(e, "good", R"LUA(log.info("GOOD loaded"))LUA");
        e.Make();
        const size_t n = e.rt->StartAll();
        Check("a script that loops at load time is aborted and not left running", n == 1 && !e.rt->IsRunning("spin") && e.rt->IsRunning("good"), std::to_string(n));
        Check("the load failure names the budget", Has(Status(e, "spin").detail, "budget") || Count(e.api, "budget exceeded") >= 1, Status(e, "spin").detail);
    }
    {
        Env e("timeloop");
        e.opt.limits.instructionBudget = (1ull << 40);
        e.opt.limits.timeBudgetMs = 50;
        Res(e, "slow", R"LUA(on("playerJoined", function() while true do end end))LUA");
        e.Make();
        e.rt->StartAll();
        const auto t0 = std::chrono::steady_clock::now();
        Join(e, MkPlayer(1, "Alice"));
        const double secs = Seconds(t0);
        Check("the wall-clock budget aborts a loop the instruction budget would not", Count(e.api, "time budget exceeded") >= 1, Dump(e.api, "slow"));
        Check("and does so close to the limit", secs < 5.0, std::to_string(secs));
    }
    {
        Env e("timerloop");
        e.opt.limits.instructionBudget = 1000000;
        Res(e, "tl", R"LUA(
setTimeout(function() while true do end end, 10)
setTimeout(function() log.info("TL second timer") end, 10)
)LUA");
        e.Make();
        e.rt->StartAll();
        e.api.now += 20;
        e.rt->Tick();
        Check("a runaway timer callback is aborted and logged", Count(e.api, "instruction budget exceeded") >= 1 && Count(e.api, "timer #1") >= 1, Dump(e.api, "budget"));
        Check("the next timer of the tick still fires", Count(e.api, "TL second timer") == 1);
    }
    {
        Env e("pattern");
        e.opt.limits.patternSteps = 1000000;
        Res(e, "pat", R"LUA(
on("playerJoined", function()
  local t0 = os.clock()
  local ok, err = pcall(string.find, string.rep("a", 200), string.rep("a*", 12) .. "b")
  log.info("PAT ok=" .. tostring(ok) .. " err=" .. tostring(err))
  local ok2, err2 = pcall(string.gsub, string.rep("a", 200), string.rep("a*", 12) .. "b", "x")
  log.info("PAT2 ok=" .. tostring(ok2))
  log.info("PAT3 normal=" .. tostring(string.find("hello world", "o w")))
  log.info("PAT4 elapsed_ok=" .. tostring(os.clock() - t0 < 5))
end)
)LUA");
        e.Make();
        e.rt->StartAll();
        Join(e, MkPlayer(1, "Alice"));
        Check("a backtracking pattern is cut off by the pattern budget", Mark(e.api, "PAT ok=") == "false err=pattern matching budget exceeded" ||
                                                                          Has(Mark(e.api, "PAT ok="), "pattern matching budget exceeded"),
              Mark(e.api, "PAT ok="));
        Check("gsub is budgeted too", Mark(e.api, "PAT2 ok=") == "false", Mark(e.api, "PAT2 ok="));
        Check("ordinary patterns are unaffected", Mark(e.api, "PAT3 normal=") == "5\t7" || Mark(e.api, "PAT3 normal=") == "5 7" || Has(Mark(e.api, "PAT3 normal="), "5"),
              Mark(e.api, "PAT3 normal="));
        Check("the pattern abort was fast", Mark(e.api, "PAT4 elapsed_ok=") == "true", Mark(e.api, "PAT4 elapsed_ok="));
        Join(e, MkPlayer(2, "Bob"));
        Check("the pattern budget is re-armed for the next entry", Count(e.api, "PAT3 normal=") == 2);
    }
    {
        Env e("memory");
        e.opt.limits.memoryBytes = 2 * 1024 * 1024;
        Res(e, "mem", R"LUA(
on("playerJoined", function()
  local t = {}
  local ok, err = pcall(function() for i = 1, 100000000 do t[i] = ("x"):rep(64) .. i end end)
  log.info("MEM1 ok=" .. tostring(ok) .. " err=" .. tostring(err))
  t = nil
  collectgarbage()
  local ok2, err2 = pcall(string.rep, "x", 64 * 1024 * 1024)
  log.info("MEM2 ok=" .. tostring(ok2) .. " err=" .. tostring(err2))
  log.info("MEM3 alive")
end)
on("playerLeft", function() log.info("MEM4 second entry works") end)
)LUA");
        Res(e, "neighbour", R"LUA(on("playerJoined", function() log.info("NEIGHBOUR fine") end))LUA");
        e.Make();
        e.rt->StartAll();
        Join(e, MkPlayer(1, "Alice"));
        Check("growing past the cap fails with a memory error", Mark(e.api, "MEM1 ok=").rfind("false", 0) == 0 && Has(Mark(e.api, "MEM1 ok="), "not enough memory"),
              Mark(e.api, "MEM1 ok="));
        Check("one huge allocation is refused too", Mark(e.api, "MEM2 ok=").rfind("false", 0) == 0 && Has(Mark(e.api, "MEM2 ok="), "not enough memory"),
              Mark(e.api, "MEM2 ok="));
        Check("the script survived and kept running after the memory error", Mark(e.api, "MEM3 alive") == "" && Count(e.api, "MEM3 alive") == 1);
        Check("the hit is logged by the runtime", Count(e.api, "hit its memory limit") >= 1);
        Check("memory in use stays under the cap", e.rt->Stats().luaBytes <= 2 * 1024 * 1024, std::to_string(e.rt->Stats().luaBytes));
        Check("another resource has its own budget", Count(e.api, "NEIGHBOUR fine") == 1);
        Leave(e, 1, LeaveReason::Quit);
        Check("the resource works on the next entry", Count(e.api, "MEM4 second entry works") == 1);
    }
    {
        // Every API function that copies data runs at the edge of the memory cap: an allocation
        // failure in the middle of json, storage, exports or events must never crash the host.
        Env e("oom");
        e.opt.limits.memoryBytes = 1024 * 1024;
        e.opt.limits.timeBudgetMs = 30000;                 // the point is memory, not the time budget
        e.opt.limits.instructionBudget = 1000000000;
        Res(e, "oom", R"LUA(
export("noop", function(t) return t end)
on("ping", function(t) end)
on("playerJoined", function()
  local big = {}
  for i = 1, 2300 do big[i] = { id = i, name = "n" .. i, tags = { "a", "b", "c" } } end
  local ok = 0
  local failed = 0
  for round = 1, 60 do
    local good = pcall(function()
      local which = round % 6
      if which == 0 then local s = json.encode(big); assert(json.decode(s))
      elseif which == 1 then assert(#players.list() >= 0)
      elseif which == 2 then assert(exports.oom.noop(big))
      elseif which == 3 then emit("ping", big)
      elseif which == 4 then storage.set("k" .. round, big)
      else assert(#json.encode(big, { pretty = true }) > 0) end
    end)
    if good then ok = ok + 1 else failed = failed + 1 end
  end
  log.info("OOM done ok>0=" .. tostring(ok > 0) .. " failed>0=" .. tostring(failed > 0))
end)
)LUA");
        e.Make();
        e.rt->StartAll();
        for (uint16_t i = 1; i <= 3; i++) e.api.players.push_back(MkPlayer(i, "P" + std::to_string(i)));
        e.rt->OnPlayerJoined(1);
        Check("json, storage, exports and events survive allocation failures at the memory cap", Count(e.api, "OOM done") == 1 && e.rt->IsRunning("oom"), Dump(e.api, "oom"));
        Check("some of those calls really failed for lack of memory and some succeeded", Has(Mark(e.api, "OOM done "), "ok>0=true") && Has(Mark(e.api, "OOM done "), "failed>0=true"),
              Mark(e.api, "OOM done "));
        e.rt->OnPlayerJoined(2);
        Check("the resource keeps working afterwards", Count(e.api, "OOM done") == 2);
        std::string err;
        e.rt->Stop("oom", &err);
        Check("closing the state after the failures leaves no memory accounting mismatch", Count(e.api, "memory accounting mismatch") == 0, Dump(e.api, "mismatch"));
    }
}

// ---- 5. sandbox ---------------------------------------------------------------------------------------------

static void TestSandbox() {
    Env e("sandbox");
    WriteFile(e.root / "resources" / "secret.lua", R"LUA(log.info("SECRET RAN"))LUA");
    WriteFile(e.root / "outside.lua", R"LUA(log.info("OUTSIDE RAN"))LUA");
    Res(e, "sb", R"LUA(
local function chk(name, v) log.info("SB " .. name .. "=" .. tostring(v)) end
chk("io", io == nil)
chk("os.execute", os.execute == nil)
chk("os.remove", os.remove == nil)
chk("os.rename", os.rename == nil)
chk("os.getenv", os.getenv == nil)
chk("os.exit", os.exit == nil)
chk("os.tmpname", os.tmpname == nil)
chk("os.time", type(os.time) == "function")
chk("os.clock", type(os.clock) == "function")
chk("loadfile", loadfile == nil)
chk("dofile", dofile == nil)
chk("package", package == nil)
chk("package.loadlib", package == nil or package.loadlib == nil)
chk("debug.sethook", debug.sethook == nil)
chk("debug.getinfo", debug.getinfo == nil)
chk("debug.getregistry", debug.getregistry == nil)
chk("debug.traceback", type(debug.traceback) == "function")
chk("string.dump", string.dump == nil)
chk("binary chunk", load(string.char(27) .. "Lua") == nil)
chk("binary chunk with mode b", load(string.char(27) .. "Lua", "x", "bt") == nil and load(string.char(27) .. "Lua", "x", "b") == nil)
chk("text chunk", load("return 41 + 1")() == 42)
chk("gc metamethod", pcall(setmetatable, {}, { __gc = function() end }) == false)
chk("metatables still work", getmetatable(setmetatable({}, { __index = { v = 5 } })).__index.v == 5)
chk("gc stop", pcall(collectgarbage, "stop") == false)
chk("gc count", type(collectgarbage("count")) == "number")
chk("string.rep empty is instant", #string.rep("", 1000000000) == 0)
chk("table.move range cap", pcall(table.move, {}, 1, 100000000, 2) == false)
chk("require", type(require) == "function")
chk("_ENV is the global table", _ENV == _G)
for _, name in ipairs({ "../secret", "..\\secret", "/secret", "C:/secret", "..", "sub/../../secret", "a..b", "x/y" }) do
  local ok = pcall(require, name)
  log.info("SB require-refused " .. name .. "=" .. tostring(not ok))
end
local env = {}
local f = load("x = 1", "chunk", "t", env)
f()
chk("load with a custom env", env.x == 1 and x == nil)
)LUA");
    e.Make();
    e.rt->StartAll();
    struct Row { const char* mark; const char* label; };
    const Row rows[] = {
        {"SB io=", "io library is not available"},
        {"SB os.execute=", "os.execute is not available"},
        {"SB os.remove=", "os.remove is not available"},
        {"SB os.rename=", "os.rename is not available"},
        {"SB os.getenv=", "os.getenv is not available"},
        {"SB os.exit=", "os.exit is not available"},
        {"SB os.tmpname=", "os.tmpname is not available"},
        {"SB os.time=", "os.time is kept"},
        {"SB os.clock=", "os.clock is kept"},
        {"SB loadfile=", "loadfile is not available"},
        {"SB dofile=", "dofile is not available"},
        {"SB package=", "package is not available"},
        {"SB package.loadlib=", "package.loadlib is not available"},
        {"SB debug.sethook=", "debug.sethook is not available"},
        {"SB debug.getinfo=", "debug.getinfo is not available"},
        {"SB debug.getregistry=", "debug.getregistry is not available"},
        {"SB debug.traceback=", "debug.traceback is kept"},
        {"SB string.dump=", "string.dump is not available"},
        {"SB binary chunk=", "load refuses binary chunks"},
        {"SB binary chunk with mode b=", "load cannot be switched to binary mode"},
        {"SB text chunk=", "load runs text chunks"},
        {"SB gc metamethod=", "setmetatable refuses __gc"},
        {"SB metatables still work=", "ordinary metatables still work"},
        {"SB gc stop=", "collectgarbage('stop') is refused"},
        {"SB gc count=", "collectgarbage('count') works"},
        {"SB string.rep empty is instant=", "string.rep with an empty subject cannot hang the tick"},
        {"SB table.move range cap=", "table.move refuses an enormous range"},
        {"SB require=", "require exists"},
        {"SB _ENV is the global table=", "_ENV is the global table"},
        {"SB load with a custom env=", "load honours a custom environment"},
    };
    for (const Row& r : rows) Eq(r.label, Mark(e.api, r.mark), "true");
    Check("every require escape attempt was refused", Count(e.api, "SB require-refused") == 8 && CountAll(e.api, "SB require-refused", "=true") == 8, Dump(e.api, "require-refused"));
    Check("no file outside the resource folder ever ran", Count(e.api, "SECRET RAN") == 0 && Count(e.api, "OUTSIDE RAN") == 0);
}

// ---- 6. require inside the resource ----------------------------------------------------------------------------

static void TestRequire() {
    {
        Env e("require");
        Res(e, "rq", R"LUA(
local u = require("util")
local u2 = require("util")
log.info("RQ same=" .. tostring(u == u2) .. " value=" .. u.value)
log.info("RQ deep=" .. require("lib.deep").name)
log.info("RQ init=" .. require("lib").name)
local ok, err = pcall(require, "missing")
log.info("RQ missing=" .. tostring(ok) .. " " .. tostring(err))
local ok3 = pcall(require, "bad")
log.info("RQ bad-module=" .. tostring(ok3))
local ok4, err4 = pcall(require, "loop_a")
log.info("RQ loop=" .. tostring(ok4) .. " " .. tostring(err4))
log.info("RQ util-loads=" .. UTIL_LOADS)
log.info("RQ sees-api=" .. tostring(require("api_user").has_api))
)LUA");
        WriteFile(e.Dir("rq") / "util.lua", "UTIL_LOADS = (UTIL_LOADS or 0) + 1\nreturn { value = 42 }\n");
        WriteFile(e.Dir("rq") / "lib" / "deep.lua", "return { name = 'deep' }\n");
        WriteFile(e.Dir("rq") / "lib" / "init.lua", "return { name = 'lib-init' }\n");
        WriteFile(e.Dir("rq") / "bad.lua", "error('module exploded')\n");
        WriteFile(e.Dir("rq") / "loop_a.lua", "return require('loop_b')\n");
        WriteFile(e.Dir("rq") / "loop_b.lua", "return require('loop_a')\n");
        WriteFile(e.Dir("rq") / "api_user.lua", "return { has_api = type(log.info) == 'function' and type(storage.get) == 'function' }\n");
        WriteFile(e.root / "resources" / "secret.lua", R"LUA(log.info("SECRET RAN"))LUA");
        e.Make();
        Check("a resource with modules starts", e.rt->StartAll() == 1);
        Eq("require loads a module inside the resource and caches it", Mark(e.api, "RQ same="), "true value=42");
        Eq("require loads once", Mark(e.api, "RQ util-loads="), "1");
        Eq("dotted names map to folders", Mark(e.api, "RQ deep="), "deep");
        Eq("a folder with init.lua is a module", Mark(e.api, "RQ init="), "lib-init");
        Check("a missing module is an error that names it", Has(Mark(e.api, "RQ missing="), "false") && Has(Mark(e.api, "RQ missing="), "missing"), Mark(e.api, "RQ missing="));
        Eq("an error inside a module propagates to the requirer", Mark(e.api, "RQ bad-module="), "false");
        Check("a circular require is an error, not a hang", Has(Mark(e.api, "RQ loop="), "false") && Has(Mark(e.api, "RQ loop="), "loop"), Mark(e.api, "RQ loop="));
        Eq("modules see the resource API", Mark(e.api, "RQ sees-api="), "true");
        Check("the module error was not swallowed silently", Has(Dump(e.api, "RQ bad-module"), "false"));
    }
    {
        Env e("junction");
        // A junction inside the resource that points outside it: require must not follow it.
        const fs::path outside = e.root / "resources" / "outside_dir";
        WriteFile(outside / "mod.lua", R"LUA(log.info("JUNCTION RAN"); return {})LUA");
        Res(e, "jn", R"LUA(
local ok, err = pcall(require, "ext.mod")
log.info("JN refused=" .. tostring(not ok) .. " " .. tostring(err))
)LUA");
        // A directory link (junction on Windows, symlink elsewhere); false when it cannot be made here.
        auto makeLink = [](const fs::path& link, const fs::path& target) {
#ifdef _WIN32
            fs::path l = link, t = target;
            l.make_preferred();
            t.make_preferred();
            const std::string cmd = "cmd /c mklink /J \"" + l.string() + "\" \"" + t.string() + "\" > NUL 2>&1";
            return std::system(cmd.c_str()) == 0 && Exists(link);
#else
            std::error_code ec;
            fs::create_directory_symlink(target, link, ec);
            return !ec && Exists(link);
#endif
        };
        const bool made = makeLink(e.Dir("jn") / "ext", outside) && Exists(e.Dir("jn") / "ext" / "mod.lua");
        const bool madeRes = makeLink(e.root / "resources" / "linkres", e.Dir("jn"));
        e.Make();
        e.rt->StartAll();
        if (made) {
            Check("require refuses a junction/symlink that leaves the resource folder",
                  Mark(e.api, "JN refused=").rfind("true", 0) == 0 && Has(Mark(e.api, "JN refused="), "link") && Count(e.api, "JUNCTION RAN") == 0, Mark(e.api, "JN refused=") + " " + Dump(e.api, "JUNCTION"));
        } else {
            Check("SKIPPED (could not create a link on this machine): require refuses a link out of the folder", true);
        }
        if (madeRes) {
            Check("a linked folder under resources/ is never listed as a resource", Status(e, "linkres").name.empty() && Count(e.api, "linkres") == 0, Dump(e.api, "linkres"));
        } else {
            Check("SKIPPED (could not create a link on this machine): a linked folder is not a resource", true);
        }
        // Remove the links themselves first so the final clean-up never walks through one.
        std::error_code ec;
        fs::remove(e.root / "resources" / "linkres", ec);
        fs::remove(e.Dir("jn") / "ext", ec);
    }
}

// ---- 7. timers on the simulated tick ------------------------------------------------------------------------------

static void TestTimers() {
    {
        Env e("timers");
        Res(e, "tm", R"LUA(
setTimeout(function() log.info("TM 300") end, 300)
setTimeout(function() log.info("TM 100a") end, 100)
setTimeout(function() log.info("TM 100b") end, 100)
local n = 0
local id
id = setInterval(function()
  n = n + 1
  log.info("TM I" .. n)
  if n == 3 then log.info("TM cleared=" .. tostring(clearTimer(id))) end
end, 250)
setTimeout(function()
  setTimeout(function() log.info("TM NESTED") end, 0)
  log.info("TM OUTER")
end, 50)
)LUA");
        e.Make();
        e.rt->StartAll();
        Check("five timers are queued", e.rt->Stats().timers == 5, std::to_string(e.rt->Stats().timers));
        e.rt->Tick();
        Check("nothing fires before its time", Count(e.api, "TM ") == 0);
        e.api.now += 50;
        e.rt->Tick();
        Eq("a due timer fires on the tick", Marks(e.api, "TM "), "OUTER");
        e.rt->Tick();
        Eq("a timer created by a timer waits for the next tick", Marks(e.api, "TM "), "OUTER,NESTED");
        e.api.now += 50;
        e.rt->Tick();
        e.api.now += 150;
        e.rt->Tick();
        e.api.now += 50;
        e.rt->Tick();
        e.api.now += 200;
        e.rt->Tick();
        e.api.now += 250;
        e.rt->Tick();
        e.api.now += 250;
        e.rt->Tick();
        Eq("timers fire in due-time order, equal times in registration order, the interval repeats and clears itself",
           Marks(e.api, "TM "), "OUTER,NESTED,100a,100b,I1,300,I2,I3,cleared=true");
        Check("no timer is left after the last one cleared itself", e.rt->Stats().timers == 0, std::to_string(e.rt->Stats().timers));
        e.api.now += 100000;
        e.rt->Tick();
        Check("nothing fires after that", Count(e.api, "TM I4") == 0);
    }
    {
        Env e("burst");
        Res(e, "bt", R"LUA(setInterval(function() log.info("BURST fire") end, 100))LUA");
        e.Make();
        e.rt->StartAll();
        e.api.now += 10000;
        e.rt->Tick();
        Check("a long stall fires an interval once, not once per missed period", Count(e.api, "BURST fire") == 1, std::to_string(Count(e.api, "BURST fire")));
        e.rt->Tick();
        Check("the same tick time does not fire it again", Count(e.api, "BURST fire") == 1);
        e.api.now += 100;
        e.rt->Tick();
        Check("it resumes its period afterwards", Count(e.api, "BURST fire") == 2, std::to_string(Count(e.api, "BURST fire")));
    }
    {
        Env e("timererr");
        Res(e, "te", R"LUA(
setInterval(function() log.info("TE int"); error("interval error") end, 10)
local calls = 0
setTimeout(function() log.info("TE once") end, 10)
)LUA");
        e.Make();
        e.rt->StartAll();
        e.api.now += 10;
        e.rt->Tick();
        e.api.now += 10;
        e.rt->Tick();
        Check("an interval whose callback errors keeps firing", Count(e.api, "TE int") == 2 && Count(e.api, "interval error") >= 2, std::to_string(Count(e.api, "TE int")));
        Check("a one-shot beside it still fires once", Count(e.api, "TE once") == 1);
    }
    {
        Env e("timerargs");
        Res(e, "ta", R"LUA(
local f = function() end
log.info("TA neg=" .. tostring(pcall(setTimeout, f, -1)))
log.info("TA str=" .. tostring(pcall(setTimeout, "x", 1)))
log.info("TA zero-interval=" .. tostring(pcall(setInterval, f, 0)))
log.info("TA huge=" .. tostring(pcall(setTimeout, f, 86400001)))
log.info("TA float=" .. tostring(pcall(setTimeout, f, 1.5)))
log.info("TA clear-unknown=" .. tostring(clearTimer(12345)))
local id = setTimeout(f, 1000)
log.info("TA clear=" .. tostring(clearTimer(id)) .. " again=" .. tostring(clearTimer(id)))
local okc = 0
for i = 1, 300 do if pcall(setTimeout, f, 100000) then okc = okc + 1 end end
log.info("TA limit=" .. okc)
local okh = 0
for i = 1, 300 do if pcall(on, "evt", f) then okh = okh + 1 end end
log.info("TA handlers=" .. okh)
)LUA");
        e.Make();
        e.rt->StartAll();
        Eq("a negative delay is refused", Mark(e.api, "TA neg="), "false");
        Eq("a non-function callback is refused", Mark(e.api, "TA str="), "false");
        Eq("a zero interval is refused", Mark(e.api, "TA zero-interval="), "false");
        Eq("a delay beyond a day is refused", Mark(e.api, "TA huge="), "false");
        Eq("a fractional delay is refused", Mark(e.api, "TA float="), "false");
        Eq("clearing an unknown timer is false, not an error", Mark(e.api, "TA clear-unknown="), "false");
        Eq("clearTimer is true once, then false", Mark(e.api, "TA clear="), "true again=false");
        Eq("timers per resource are capped", Mark(e.api, "TA limit="), "256");
        Eq("handlers per resource are capped", Mark(e.api, "TA handlers="), "256");
    }
}

// ---- 8. chat ------------------------------------------------------------------------------------------------------------

static void TestChat() {
    Env e("chat");
    Res(e, "filter", R"LUA(
on("chatMessage", function(p, text)
  log.info("FILTER saw " .. p.name .. ": " .. text)
  if text:find("badword", 1, true) then cancelEvent() end
end)
on("playerJoined", function(p)
  log.info("FILTER cancel-outside=" .. tostring(pcall(cancelEvent)))
end)
)LUA");
    Res(e, "logger", R"LUA(on("chatMessage", function(p, text) log.info("LOGGER saw " .. text) end))LUA");
    e.Make();
    e.rt->StartAll();
    Join(e, MkPlayer(1, "Alice"));
    Check("a normal message is broadcast", e.rt->OnChatMessage(1, "hello there"));
    Check("handlers receive the player and the text", Count(e.api, "FILTER saw Alice: hello there") == 1);
    Check("cancelEvent() stops the broadcast", !e.rt->OnChatMessage(1, "this has a badword inside"));
    Check("every resource still sees a cancelled message", Count(e.api, "LOGGER saw this has a badword inside") == 1);
    Eq("cancelEvent() outside a cancellable event is an error", Mark(e.api, "FILTER cancel-outside="), "false");
    const size_t before = e.api.chats.size();
    Check("a /command is never broadcast as chat", !e.rt->OnChatMessage(1, "/nothing here"));
    Check("an unknown /command is answered to the player", e.api.chats.size() == before + 1 && e.api.chats.back().to == 1 &&
                                                                Has(e.api.chats.back().text, "Unknown command: /nothing"),
          e.api.chats.empty() ? "no chat" : e.api.chats.back().text);
    Check("a lone slash is an unknown command too", !e.rt->OnChatMessage(1, "/"));
    Check("command handlers are not given /commands as chatMessage", Count(e.api, "FILTER saw Alice: /nothing") == 0);
    {
        Env none("chatnone");
        none.Make();
        Join(none, MkPlayer(1, "Alice"));
        Check("with no resource running every chat line goes through to the server", none.rt->OnChatMessage(1, "hi") && none.rt->OnChatMessage(1, "/whatever"));
    }
    Check("a chat line from an id the server no longer knows is not swallowed", e.rt->OnChatMessage(77, "ghost"));
}

// ---- 9. commands, restricted commands, console -----------------------------------------------------------------------

static void TestCommands() {
    Env e("commands");
    Res(e, "cmds", R"LUA(
registerCommand("echo", function(src, args, raw)
  reply(src, "ECHO:" .. raw .. "|" .. #args .. "|" .. tostring(args[1]) .. "|" .. tostring(args[3]))
end)
registerCommand("adm", function(src, args)
  log.info("ADM RAN by " .. src.name .. " console=" .. tostring(src.console) .. " admin=" .. tostring(src.isAdmin))
  reply(src, "adm done")
end, { restricted = true })
registerCommand("MixedCase", function(src) reply(src, "mixed") end)
log.info("CMDS reserved=" .. tostring(pcall(registerCommand, "kick", function() end)))
log.info("CMDS badname=" .. tostring(pcall(registerCommand, "has space", function() end)))
log.info("CMDS nofn=" .. tostring(pcall(registerCommand, "nofn", "x")))
)LUA");
    Res(e, "rival", R"LUA(
log.info("RIVAL dup=" .. tostring(pcall(registerCommand, "echo", function() end)))
local ok, err = pcall(registerCommand, "echo", function() end)
log.info("RIVAL err=" .. tostring(err))
)LUA",
        {"cmds"});
    e.Make();
    e.rt->StartAll();
    Join(e, MkPlayer(1, "Alice", false));
    Join(e, MkPlayer(2, "Root", true));

    Eq("a built-in command name cannot be registered", Mark(e.api, "CMDS reserved="), "false");
    Eq("an invalid command name is refused", Mark(e.api, "CMDS badname="), "false");
    Eq("a non-function handler is refused", Mark(e.api, "CMDS nofn="), "false");
    Eq("a second resource cannot take a registered command", Mark(e.api, "RIVAL dup="), "false");
    Check("the refusal names the owner", Has(Mark(e.api, "RIVAL err="), "already registered by resource 'cmds'"), Mark(e.api, "RIVAL err="));

    Check("/echo is handled, not broadcast", !e.rt->OnChatMessage(1, "/echo  a   b  c "));
    Check("arguments are split on spaces, the raw text is kept", !e.api.chats.empty() && e.api.chats.back().to == 1 &&
                                                                    e.api.chats.back().text == "ECHO:a   b  c|3|a|c",
          e.api.chats.empty() ? "no chat" : e.api.chats.back().text);
    e.rt->OnChatMessage(1, "/ECHO shout");
    Check("command names are case-insensitive", !e.api.chats.empty() && Has(e.api.chats.back().text, "ECHO:shout|1|shout|nil"), e.api.chats.back().text);
    e.rt->OnChatMessage(1, "/mixedcase");
    Check("a name registered in mixed case is stored lowercase", !e.api.chats.empty() && e.api.chats.back().text == "mixed", e.api.chats.back().text);

    Check("a restricted command refuses a non-admin", !e.rt->OnChatMessage(1, "/adm") && Count(e.api, "ADM RAN") == 0);
    Check("and tells them so", !e.api.chats.empty() && e.api.chats.back().to == 1 && Has(e.api.chats.back().text, "permission"), e.api.chats.back().text);
    Check("the refusal is logged", Count(e.api, "was refused restricted command /adm") == 1);
    e.rt->OnChatMessage(2, "/adm");
    Check("a restricted command runs for an admin", Count(e.api, "ADM RAN by Root console=false admin=true") == 1, Dump(e.api, "ADM RAN"));
    Check("and the admin gets the reply", e.api.chats.back().to == 2 && e.api.chats.back().text == "adm done");

    std::string out;
    ConsoleResult r = e.rt->ExecuteConsole("adm", out);
    Check("the console may run a restricted command", r == ConsoleResult::Ok && Count(e.api, "ADM RAN by console console=true admin=true") == 1 && out == "adm done", out);
    r = e.rt->ExecuteConsole("echo hello world", out);
    Check("console commands return their reply as output", r == ConsoleResult::Ok && out == "ECHO:hello world|2|hello|nil", out);
    r = e.rt->ExecuteConsole("nosuchcommand", out);
    Check("an unknown console command is not handled", r == ConsoleResult::NotHandled);
    Check("`stop` without an argument stays with the server", e.rt->ExecuteConsole("stop", out) == ConsoleResult::NotHandled);
    Check("a built-in word is not handled by the runtime", e.rt->ExecuteConsole("kick 1", out) == ConsoleResult::NotHandled &&
                                                              e.rt->ExecuteConsole("list", out) == ConsoleResult::NotHandled);
    Check("an empty line is not handled", e.rt->ExecuteConsole("   ", out) == ConsoleResult::NotHandled);
}

static void TestConsoleManagement() {
    Env e("console");
    Res(e, "a", R"LUA(log.info("A loaded"))LUA");
    Res(e, "b", R"LUA(log.info("B loaded"))LUA", {"a"});
    e.Make();
    e.rt->StartAll();
    std::string out;
    Check("`resources` lists them with their state", e.rt->ExecuteConsole("resources", out) == ConsoleResult::Ok && Has(out, "a  1.0.0  started") &&
                                                         Has(out, "b  1.0.0  started"), out);
    Check("`stop <res>` stops it and its dependents", e.rt->ExecuteConsole("stop a", out) == ConsoleResult::Ok && out == "stopped a" &&
                                                          !e.rt->IsRunning("a") && !e.rt->IsRunning("b"), out);
    Check("`start <res>` starts its dependencies first", e.rt->ExecuteConsole("start b", out) == ConsoleResult::Ok && e.rt->IsRunning("a") && e.rt->IsRunning("b"), out);
    Check("`restart <res>` restarts it", e.rt->ExecuteConsole("restart a", out) == ConsoleResult::Ok && out == "restarted a" && e.rt->IsRunning("a") && e.rt->IsRunning("b"), out);
    Check("the command word is case-insensitive", e.rt->ExecuteConsole("STOP b", out) == ConsoleResult::Ok && !e.rt->IsRunning("b"), out);
    Check("stopping a stopped resource is an error with a reason", e.rt->ExecuteConsole("stop b", out) == ConsoleResult::Error && Has(out, "not running"), out);
    Check("starting an unknown resource is an error with a reason", e.rt->ExecuteConsole("start nosuch", out) == ConsoleResult::Error && Has(out, "nosuch"), out);
    Check("`start` without a name shows usage", e.rt->ExecuteConsole("start", out) == ConsoleResult::Error && Has(out, "usage"), out);
    Res(e, "fresh", R"LUA(log.info("FRESH loaded"))LUA");
    Check("a resource added on disk is unknown until `refresh`", e.rt->ExecuteConsole("resources", out) == ConsoleResult::Ok && !Has(out, "fresh"), out);
    Check("`refresh` finds it", e.rt->ExecuteConsole("refresh", out) == ConsoleResult::Ok && Has(out, "found 3"), out);
    Check("and `start` runs it", e.rt->ExecuteConsole("start fresh", out) == ConsoleResult::Ok && Count(e.api, "FRESH loaded") == 1, out);
    // The manifest is re-read on every start: an edit takes effect on restart.
    Res(e, "fresh", R"LUA(log.info("FRESH v2 loaded"))LUA");
    e.rt->ExecuteConsole("restart fresh", out);
    Check("a restart runs the edited script", Count(e.api, "FRESH v2 loaded") == 1);
    WriteFile(e.Dir("fresh") / "resource.json", "{ not json");
    Check("a restart with a broken manifest fails and leaves it stopped", e.rt->ExecuteConsole("restart fresh", out) == ConsoleResult::Error && !e.rt->IsRunning("fresh"), out);
}

// ---- 10. exports and events between resources ---------------------------------------------------------------------------

static void TestExports() {
    Env e("exports");
    e.opt.limits.instructionBudget = 1000000;
    Res(e, "provider", R"LUA(
shared = { 1, 2, 3 }
export("add", function(a, b) return a + b end)
export("tbl", function(t) t.x = 99; return t, #t end)
export("getShared", function() return shared end)
export("boom", function() error("provider failed") end)
export("fn", function() return function() end end)
export("loop", function() while true do end end)
export("multi", function() return 1, "two", { three = 3 }, nil, true end)
export("who", function() return resource.name end)
export("meta", function() return setmetatable({ a = 1 }, { __index = function() return "leak" end }) end)
on("custom:ping", function(data) log.info("PROVIDER got ping n=" .. data.n) ; data.n = 1000 end)
)LUA");
    Res(e, "consumer", R"LUA(
log.info("EXP add=" .. exports.provider.add(2, 3))
local mine = { 1, 2, 3 }
local back, n = exports.provider.tbl(mine)
log.info("EXP copy mine.x=" .. tostring(mine.x) .. " back.x=" .. tostring(back.x) .. " n=" .. n)
local s1 = exports.provider.getShared()
s1[1] = 100
local s2 = exports.provider.getShared()
log.info("EXP shared s2[1]=" .. s2[1] .. " same=" .. tostring(s1 == s2))
log.info("EXP boom=" .. tostring(pcall(exports.provider.boom)))
local _, e1 = pcall(exports.provider.boom)
log.info("EXP boom-err=" .. tostring(e1))
local _, e2 = pcall(exports.provider.fn)
log.info("EXP fn-err=" .. tostring(e2))
local _, e3 = pcall(exports.provider.loop)
log.info("EXP loop-err=" .. tostring(e3))
local _, e4 = pcall(function() return exports.nobody.x() end)
log.info("EXP nobody-err=" .. tostring(e4))
local _, e5 = pcall(function() return exports.provider.missing() end)
log.info("EXP missing-err=" .. tostring(e5))
local a, b, c, d, f = exports.provider.multi()
log.info("EXP multi=" .. tostring(a) .. "," .. tostring(b) .. "," .. tostring(c.three) .. "," .. tostring(d) .. "," .. tostring(f))
log.info("EXP who=" .. exports.provider.who())
local m = exports.provider.meta()
log.info("EXP meta a=" .. tostring(m.a) .. " zz=" .. tostring(m.zz) .. " mt=" .. tostring(getmetatable(m)))
local cyc = {}; cyc.me = cyc
local _, e6 = pcall(exports.provider.tbl, cyc)
log.info("EXP cycle-err=" .. tostring(e6))
local _, e7 = pcall(exports.provider.add, print, 1)
log.info("EXP fnarg-err=" .. tostring(e7))
log.info("EXP readonly=" .. tostring(pcall(function() exports.provider = 1 end)))
local payload = { n = 1 }
emit("custom:ping", payload)
log.info("EXP emit-copy payload.n=" .. payload.n)
log.info("EXP emit-builtin=" .. tostring(pcall(emit, "playerJoined")))
)LUA",
        {"provider"});
    e.Make();
    e.rt->StartAll();
    Check("both resources are running after the calls", e.rt->IsRunning("provider") && e.rt->IsRunning("consumer"));
    Eq("an export returns its result", Mark(e.api, "EXP add="), "5");
    Eq("arguments are copied: the callee's change is not visible to the caller", Mark(e.api, "EXP copy "), "mine.x=nil back.x=99 n=3");
    Eq("results are copied: two calls never share a table", Mark(e.api, "EXP shared "), "s2[1]=1 same=false");
    Eq("an error in the exported function reaches the caller as an error", Mark(e.api, "EXP boom="), "false");
    Check("with the provider's message and the export name", Has(Mark(e.api, "EXP boom-err="), "provider failed") && Has(Mark(e.api, "EXP boom-err="), "provider.boom"),
          Mark(e.api, "EXP boom-err="));
    Check("a function cannot be returned across resources", Has(Mark(e.api, "EXP fn-err="), "function"), Mark(e.api, "EXP fn-err="));
    Check("an export that loops is aborted and the caller continues", Has(Mark(e.api, "EXP loop-err="), "budget"), Mark(e.api, "EXP loop-err="));
    Check("a call into a resource that is not running is an error", Has(Mark(e.api, "EXP nobody-err="), "not running"), Mark(e.api, "EXP nobody-err="));
    Check("a missing export is an error naming it", Has(Mark(e.api, "EXP missing-err="), "no export 'missing'"), Mark(e.api, "EXP missing-err="));
    Eq("multiple results, nil and nested tables survive the copy", Mark(e.api, "EXP multi="), "1,two,3,nil,true");
    Eq("the export runs in the provider's own state", Mark(e.api, "EXP who="), "provider");
    Eq("metatables are not copied", Mark(e.api, "EXP meta "), "a=1 zz=nil mt=nil");
    Check("a table containing itself is refused", Has(Mark(e.api, "EXP cycle-err="), "deeply") || Has(Mark(e.api, "EXP cycle-err="), "cycle"), Mark(e.api, "EXP cycle-err="));
    Check("a function argument is refused", Has(Mark(e.api, "EXP fnarg-err="), "function"), Mark(e.api, "EXP fnarg-err="));
    Eq("the exports table is read-only", Mark(e.api, "EXP readonly="), "false");
    Check("a custom event reaches another resource", Mark(e.api, "PROVIDER got ping n=") == "1");
    Eq("event data is copied too", Mark(e.api, "EXP emit-copy "), "payload.n=1");
    Eq("a built-in event cannot be emitted by a resource", Mark(e.api, "EXP emit-builtin="), "false");
    Check("the provider's budget was charged for the looping export", Count(e.api, "'provider' aborted in export 'loop'") == 1, Dump(e.api, "aborted"));
}

// ---- 11. storage ------------------------------------------------------------------------------------------------------------------

static void TestStorage() {
    {
        Env e("storage");
        bool crash = false;
        e.opt.atomicWrite = [&crash](const std::string& path, const std::string& content, std::string* err) {
            if (!crash) return gage::scripting::fsutil::AtomicWrite(path, content, err);
            // The process "dies" after writing half of the temp file: the rename never happens.
            std::ofstream f(path + ".tmp", std::ios::binary | std::ios::trunc);
            f.write(content.data(), static_cast<std::streamsize>(content.size() / 2));
            f.close();
            if (err) *err = "simulated crash";
            return false;
        };
        Res(e, "st", R"LUA(
local n = storage.get("count", 0) + 1
storage.set("count", n)
storage.set("cfg", { a = 1, b = { true, "x" } })
local ok, err = storage.flush()
log.info("ST flush ok=" .. tostring(ok) .. " err=" .. tostring(err) .. " count=" .. n)
registerCommand("bump", function(src)
  local c = storage.get("count") + 1
  storage.set("count", c)
  local ok2, err2 = storage.flush()
  reply(src, "bump " .. c .. " flush=" .. tostring(ok2) .. " " .. tostring(err2))
end)
)LUA");
        e.Make();
        std::string err;
        Check("a resource with storage starts", e.rt->Start("st", &err), err);
        Eq("first start: count is 1 and the flush succeeded", Mark(e.api, "ST flush ok="), "true err=nil count=1");
        const fs::path file = e.Data("st") / "storage.json";
        Check("storage is written under data/<resource>/", Exists(file));
        const std::string text1 = ReadText(file);
        Check("the file is JSON holding the values", Has(text1, "\"count\": 1") && Has(text1, "\"cfg\"") && Has(text1, "\"a\":1"), text1);
        Check("no temp file is left behind by a normal write", !Exists(file.string() + ".tmp"));
        Check("stop and start again", e.rt->Stop("st", &err) && e.rt->Start("st", &err), err);
        Check("values persist across a restart", Count(e.api, "count=2") == 1, Dump(e.api, "ST flush"));

        const std::string before = ReadText(file);
        crash = true;
        std::string out;
        e.rt->ExecuteConsole("bump", out);
        Check("a write that crashes mid-way is reported to the script as nil plus the reason", Has(out, "flush=nil") && Has(out, "simulated crash"), out);
        Check("the old file is untouched (temp + rename)", ReadText(file) == before, ReadText(file));
        Check("the half-written temp file is the only debris", Exists(file.string() + ".tmp"));
        Check("stopping with a failing disk still stops the resource and says why", e.rt->Stop("st", &err) && !e.rt->IsRunning("st") && Count(e.api, "storage not saved") >= 1,
              Dump(e.api, "storage not saved"));
        crash = false;
        Check("after the crash the resource starts from the last good file", e.rt->Start("st", &err), err);
        Check("with the old values", Count(e.api, "count=3") == 1, Dump(e.api, "ST flush"));
        Check("a stray temp file does not prevent the next write", ReadText(file).find("\"count\": 3") != std::string::npos, ReadText(file));
    }
    {
        Env e("storagewb");
        Res(e, "wb", R"LUA(storage.set("k", "v1"))LUA");
        Res(e, "sd", R"LUA(storage.set("k", "shutdown-value"))LUA");
        e.Make();
        e.rt->Start("wb");
        const fs::path wbFile = e.Data("wb") / "storage.json";
        Check("storage is write-behind: nothing on disk right after set()", !Exists(wbFile));
        e.api.now += 500;
        e.rt->Tick();
        Check("still not written before the flush interval", !Exists(wbFile));
        e.api.now += 2000;
        e.rt->Tick();
        Check("written by Tick() after the flush interval", Exists(wbFile) && Has(ReadText(wbFile), "\"k\": \"v1\""), ReadText(wbFile));
        e.rt->Start("sd");
        e.rt.reset();
        e.api.rt = nullptr;
        Check("Shutdown flushes dirty storage", Has(ReadText(e.Data("sd") / "storage.json"), "shutdown-value"), ReadText(e.Data("sd") / "storage.json"));
    }
    {
        Env e("storagedmg");
        WriteFile(e.Data("dmg") / "storage.json", "{\"a\": ");
        Res(e, "dmg", R"LUA(log.info("DMG loaded"))LUA");
        e.Make();
        std::string err;
        Check("a damaged storage file refuses the start and says so", !e.rt->Start("dmg", &err) && Has(err, "storage") && Count(e.api, "DMG loaded") == 0, err);
        Check("and is not overwritten", ReadText(e.Data("dmg") / "storage.json") == "{\"a\": ");
    }
    {
        Env e("storagelimits");
        e.opt.storageMaxBytes = 2048;
        Res(e, "q", R"LUA(
local ok1 = pcall(storage.set, "big", string.rep("x", 5000))
local ok2 = pcall(storage.set, string.rep("k", 200), 1)
local ok3 = pcall(storage.set, "fn", print)
local ok4 = pcall(storage.set, "nan", 0/0)
local ok5 = pcall(storage.set, "small", "fine")
local ok6 = pcall(storage.set, "holes", { [1] = 1, [3] = 3 })
storage.set("t", { list = { 1, 2, 3 }, x = "y" })
local t = storage.get("t")
storage.set("gone", 1)
storage.set("gone", nil)
local keys = storage.keys()
table.sort(keys)
log.info("SQ " .. tostring(ok1) .. tostring(ok2) .. tostring(ok3) .. tostring(ok4) .. tostring(ok5) .. tostring(ok6))
log.info("SQ2 t=" .. #t.list .. t.x .. " gone=" .. tostring(storage.get("gone")) .. " default=" .. storage.get("nope", "dflt") .. " keys=" .. table.concat(keys, ","))
log.info("SQ3 delete=" .. tostring(storage.delete("small")) .. tostring(storage.delete("small")))
)LUA");
        e.Make();
        e.rt->StartAll();
        Eq("quota, key length, functions, NaN and sparse arrays are refused; a small value is accepted", Mark(e.api, "SQ "), "falsefalsefalsefalsetruefalse");
        Eq("tables round-trip, nil deletes, defaults work, keys are listed", Mark(e.api, "SQ2 "), "t=3y gone=nil default=dflt keys=small,t");
        Eq("storage.delete reports whether the key existed", Mark(e.api, "SQ3 "), "delete=truefalse");
    }
    {
        // fsutil::AtomicWrite on its own.
        Env e("atomic");
        const std::string dir = (e.root / "atomicdir").generic_string();
        std::string err;
        Check("EnsureDir creates nested folders", gage::scripting::fsutil::EnsureDir(dir + "/a/b", &err), err);
        const std::string path = dir + "/x.json";
        Check("AtomicWrite creates a file", gage::scripting::fsutil::AtomicWrite(path, "one", &err), err);
        Check("AtomicWrite replaces an existing file", gage::scripting::fsutil::AtomicWrite(path, "two-two", &err) && ReadText(path) == "two-two", err);
        size_t entries = 0;
        std::error_code ec;
        for (const auto& it : fs::directory_iterator(dir, ec)) {
            (void)it;
            entries++;
        }
        Check("no temp file remains next to it", entries == 2 /* a/ and x.json */, std::to_string(entries));
        Check("AtomicWrite into a missing folder fails with a reason and leaves nothing", !gage::scripting::fsutil::AtomicWrite(dir + "/nodir/x.json", "x", &err) && !err.empty() && !Exists(dir + "/nodir"), err);
    }
}

// ---- 12. json, config, convars ----------------------------------------------------------------------------------------------------------

static void TestJsonAndConfig() {
    Env e("json");
    e.api.convars["name"] = "Test Server";
    Res(e, "js", R"LUA(
log.info("JS enc=" .. json.encode({ b = { 1, 2, 3 }, a = 1, c = "x\"y\n", d = true }))
local d = json.decode('{"i":1,"f":1.5,"big":9007199254740993,"s":"\\u00e9","n":null,"arr":[1,[2]],"neg":-0}')
log.info("JS dec int=" .. math.type(d.i) .. " flt=" .. math.type(d.f) .. " big=" .. string.format("%d", d.big) .. " s=" .. #d.s .. " null=" .. tostring(d.n == json.null) .. " arr=" .. d.arr[2][1])
local v, err = json.decode("{bad")
log.info("JS bad=" .. tostring(v) .. " err=" .. tostring(err))
local dv, derr = json.decode(string.rep("[", 100) .. string.rep("]", 100))
log.info("JS deep=" .. tostring(dv) .. " " .. tostring(derr))
log.info("JS enc-fn=" .. tostring(pcall(json.encode, print)))
log.info("JS enc-nested-fn=" .. tostring(pcall(json.encode, { f = print })))
local cyc = {}; cyc.self = cyc
log.info("JS enc-cycle=" .. tostring(pcall(json.encode, cyc)))
log.info("JS enc-nan=" .. tostring(pcall(json.encode, 0 / 0)))
log.info("JS enc-mixed=" .. tostring(pcall(json.encode, { 1, 2, x = 3 })))
log.info("JS invalid-utf8=" .. tostring(json.decode('"\xff"')))
log.info("JS trailing=" .. tostring(json.decode("1 2")))
log.info("JS comment=" .. tostring(json.decode("[1,/*x*/2]")))
log.info("JS leading-zero=" .. tostring(json.decode("[01]")))
log.info("JS trailing-comma=" .. tostring(json.decode("[1,]")))
log.info("JS single-quote=" .. tostring(json.decode("['a']")))
log.info("JS dup-key=" .. tostring(json.decode('{"a":1,"a":2}')))
log.info("JS roundtrip=" .. json.encode(json.decode('[1,2.5,"a",null,{"k":[]}]')))
log.info("JS empty=" .. json.encode({}) .. json.encode(json.decode("[]")))
log.info("JS pretty=" .. json.encode({ a = 1 }, { pretty = true }):gsub("\n", "~"))
log.info("JS escape=" .. json.encode("tab\tbell\7 nul\0 \u{e9}"))
log.info("JS int-float=" .. json.encode(3) .. json.encode(3.0) .. json.encode(-0.5) .. json.encode(1e300))
log.info("JS big-int=" .. json.encode(math.maxinteger))
log.info("JS enc-null=" .. json.encode({ json.null, 1 }))

log.info("CF greeting=" .. config.get("greeting") .. " max=" .. config.get("limits").max .. " list=" .. #config.get("limits").list)
log.info("CF default=" .. config.get("missing", "dflt") .. " nil=" .. tostring(config.get("missing")))
local keys = config.keys()
table.sort(keys)
log.info("CF keys=" .. table.concat(keys, ","))
log.info("CF flag=" .. tostring(config.get("flag")))
log.info("CV name=" .. getConvar("name") .. " default=" .. getConvar("nope", "dflt") .. " nil=" .. tostring(getConvar("nope")))
log.info("RES name=" .. resource.name .. " version=" .. resource.version .. " deps=" .. #resource.dependencies)
)LUA",
        {}, R"JSON({"greeting":"hi","limits":{"max":5,"list":[1,2]},"flag":true})JSON");
    WriteFile(e.Dir("js") / "config.json", R"JSON({"greeting":"yo","extra":"added"})JSON");
    e.Make();
    e.rt->StartAll();
    Eq("encode sorts object keys and escapes strings", Mark(e.api, "JS enc="), R"({"a":1,"b":[1,2,3],"c":"x\"y\n","d":true})");
    Eq("decode keeps integers and floats apart, reads escapes, null and nesting", Mark(e.api, "JS dec "),
       "int=integer flt=float big=9007199254740993 s=2 null=true arr=2");
    Check("invalid JSON returns nil and a message with a position", Mark(e.api, "JS bad=").rfind("nil err=", 0) == 0 && Has(Mark(e.api, "JS bad="), "at byte"), Mark(e.api, "JS bad="));
    Check("deep nesting is refused, not a stack overflow", Mark(e.api, "JS deep=").rfind("nil ", 0) == 0, Mark(e.api, "JS deep="));
    Eq("encoding a function is an error", Mark(e.api, "JS enc-fn="), "false");
    Eq("encoding a table that holds a function is an error", Mark(e.api, "JS enc-nested-fn="), "false");
    Eq("encoding a cycle is an error", Mark(e.api, "JS enc-cycle="), "false");
    Eq("encoding NaN is an error", Mark(e.api, "JS enc-nan="), "false");
    Eq("encoding a table that is both array and object is an error", Mark(e.api, "JS enc-mixed="), "false");
    Eq("invalid UTF-8 is rejected", Mark(e.api, "JS invalid-utf8="), "nil");
    Eq("trailing data is rejected", Mark(e.api, "JS trailing="), "nil");
    Eq("comments are rejected", Mark(e.api, "JS comment="), "nil");
    Eq("leading zeros are rejected", Mark(e.api, "JS leading-zero="), "nil");
    Eq("a trailing comma is rejected", Mark(e.api, "JS trailing-comma="), "nil");
    Eq("single quotes are rejected", Mark(e.api, "JS single-quote="), "nil");
    Eq("duplicate keys are rejected", Mark(e.api, "JS dup-key="), "nil");
    Eq("a decoded value re-encodes", Mark(e.api, "JS roundtrip="), R"([1,2.5,"a",null,{"k":{}}])");
    Eq("an empty table encodes as an object", Mark(e.api, "JS empty="), "{}{}");
    Eq("pretty output is indented", Mark(e.api, "JS pretty="), "{~  \"a\": 1~}");
    Eq("control characters are escaped, other text stays as UTF-8", Mark(e.api, "JS escape="), std::string(R"("tab\tbell\u0007 nul\u0000 )") + "\xc3\xa9\"");
    Eq("integers and floats keep their form", Mark(e.api, "JS int-float="), "33.0-0.51e+300");
    Eq("the largest integer encodes exactly", Mark(e.api, "JS big-int="), "9223372036854775807");
    Eq("json.null encodes as null", Mark(e.api, "JS enc-null="), "[null,1]");
    Eq("config.get reads the manifest, config.json overrides key by key", Mark(e.api, "CF greeting="), "yo max=5 list=2");
    Eq("config.get has defaults", Mark(e.api, "CF default="), "dflt nil=nil");
    Eq("config.keys lists manifest and override keys", Mark(e.api, "CF keys="), "extra,flag,greeting,limits");
    Eq("booleans survive", Mark(e.api, "CF flag="), "true");
    Eq("getConvar reads server settings and has defaults", Mark(e.api, "CV name="), "Test Server default=dflt nil=nil");
    Eq("resource.* describes the resource", Mark(e.api, "RES name="), "js version=1.0.0 deps=0");
}

// ---- 13. players, events, actions, re-entrancy ------------------------------------------------------------------------------------------

static void TestPlayersAndActions() {
    {
        Env e("players");
        Res(e, "pl", R"LUA(
on("playerJoined", function(p)
  log.info("PL self id=" .. p.id .. " name=" .. p.name .. " ip=" .. p.ip .. " acct=" .. p.accountId .. " ping=" .. p.ping .. " admin=" .. tostring(p.isAdmin) .. " console=" .. tostring(p.console))
  if p.id ~= 2 then return end
  log.info("PL count=" .. players.count() .. " list=" .. #players.list())
  log.info("PL byname=" .. tostring(players.get("ALICE").id) .. " byid=" .. players.get(2).name .. " byTable=" .. players.get(p).name)
  log.info("PL missing=" .. tostring(players.get(99)) .. " missingName=" .. tostring(players.get("nobody")))
  log.info("PL chat=" .. tostring(sendChat(p, "hello")) .. " all=" .. tostring(sendChat("all", "everyone")))
  local ok, err = sendChat(99, "x")
  log.info("PL chat-missing ok=" .. tostring(ok) .. " err=" .. tostring(err))
  log.info("PL chat-long=" .. tostring(pcall(sendChat, p, string.rep("x", 201))))
  log.info("PL chat-empty=" .. tostring(pcall(sendChat, p, "")))
  log.info("PL chat-badtarget=" .. tostring(pcall(sendChat, "bob", "x")))
  local x, y, z, h = getPosition(p)
  log.info("PL pos=" .. x .. "," .. y .. "," .. z .. "," .. h)
  local nx, nerr = getPosition(1)
  log.info("PL nopos=" .. tostring(nx) .. " " .. tostring(nerr))
  local mx, merr = getPosition(99)
  log.info("PL pos-missing=" .. tostring(mx) .. " " .. tostring(merr))
  local sok, serr = setPosition(p, 1, 2, 3)
  log.info("PL setpos-unsupported=" .. tostring(sok) .. " " .. tostring(serr))
  log.info("PL setpos-nan=" .. tostring(pcall(setPosition, p, 0 / 0, 0, 0)))
  log.info("PL kick=" .. tostring(kick(1, "bye\nnow")))
  local kk, kerr = kick(99, "x")
  log.info("PL kick-missing=" .. tostring(kk) .. " " .. tostring(kerr))
  log.info("PL kick-badid=" .. tostring(pcall(kick, -1)) .. tostring(pcall(kick, 70000)) .. tostring(pcall(kick, "x")) .. tostring(pcall(kick, 1.5)))
  log.info("PL ban=" .. tostring(ban(1, "cheating")) .. " banIp=" .. tostring(banIp("1.2.3.4", "spam")) .. " unban=" .. tostring(unban("alice")))
end)
)LUA");
        e.Make();
        e.rt->StartAll();
        PlayerInfo alice = MkPlayer(1, "Alice");
        PlayerInfo bob = MkPlayer(2, "Bob", true);
        bob.hasPosition = true;
        bob.state.x = 1.5f;
        bob.state.y = 2.5f;
        bob.state.z = 3.5f;
        bob.state.heading = 90.0f;
        Join(e, alice);
        Join(e, bob);
        Eq("the player table has id, name, ip, accountId, ping, isAdmin and console", Mark(e.api, "PL self id=1 "),
           "name=Alice ip=10.0.0.1 acct=name:Alice ping=21 admin=false console=false");
        Eq("players.count and players.list", Mark(e.api, "PL count="), "2 list=2");
        Eq("players.get by name (case-insensitive), id and table", Mark(e.api, "PL byname="), "1 byid=Bob byTable=Bob");
        Eq("players.get returns nil for nobody", Mark(e.api, "PL missing="), "nil missingName=nil");
        Eq("sendChat to a player and to all", Mark(e.api, "PL chat="), "true all=true");
        Check("the fake received exactly those chats", e.api.chats.size() == 2 && e.api.chats[0].to == 2 && e.api.chats[0].text == "hello" && e.api.chats[1].to == -1 &&
                                                           e.api.chats[1].text == "everyone",
              std::to_string(e.api.chats.size()));
        Check("sendChat to a missing player returns nil and the server's reason", Has(Mark(e.api, "PL chat-missing "), "ok=nil") && Has(Mark(e.api, "PL chat-missing "), "no player 99"),
              Mark(e.api, "PL chat-missing "));
        Eq("a chat line over 200 bytes is refused", Mark(e.api, "PL chat-long="), "false");
        Eq("an empty chat line is refused", Mark(e.api, "PL chat-empty="), "false");
        Eq("an unknown string target is refused", Mark(e.api, "PL chat-badtarget="), "false");
        Eq("getPosition returns x, y, z, heading", Mark(e.api, "PL pos="), "1.5,2.5,3.5,90.0");
        Eq("getPosition says when there is no position yet", Mark(e.api, "PL nopos="), "nil no position yet");
        Eq("getPosition says when there is no such player", Mark(e.api, "PL pos-missing="), "nil no such player");
        Check("setPosition reports that the protocol cannot do it yet", Has(Mark(e.api, "PL setpos-unsupported="), "nil") && Has(Mark(e.api, "PL setpos-unsupported="), "protocol support"),
              Mark(e.api, "PL setpos-unsupported="));
        Eq("setPosition refuses non-finite numbers", Mark(e.api, "PL setpos-nan="), "false");
        Check("kick reaches the server with a cleaned reason", e.api.kicks.size() == 1 && e.api.kicks[0].id == 1 && e.api.kicks[0].reason == "bye now",
              e.api.kicks.empty() ? "no kick" : e.api.kicks[0].reason);
        Eq("kick returns true", Mark(e.api, "PL kick="), "true");
        Check("kick of a missing player returns nil and the reason", Has(Mark(e.api, "PL kick-missing="), "nil") && Has(Mark(e.api, "PL kick-missing="), "no player 99"), Mark(e.api, "PL kick-missing="));
        Eq("kick refuses an id that is not an id", Mark(e.api, "PL kick-badid="), "falsefalsefalsefalse");
        Check("ban, banIp and unban pass through", e.api.bans.size() == 1 && e.api.bans[0] == "1:cheating" && e.api.banIps.size() == 1 && e.api.banIps[0] == "1.2.3.4:spam" &&
                                                       e.api.unbans.size() == 1 && e.api.unbans[0] == "alice");
        Eq("ban, banIp and unban return true", Mark(e.api, "PL ban="), "true banIp=true unban=true");
    }
    {
        Env e("connecting");
        Res(e, "gate", R"LUA(
local saved
on("playerConnecting", function(info, deferral)
  log.info("PC saw " .. info.name .. " ip=" .. info.ip .. " acct=" .. info.accountId)
  saved = deferral
  if info.name:find("bad", 1, true) then deferral.reject("Name not allowed") end
  if info.name == "Boom" then error("connect handler boom") end
  if info.name == "Double" then deferral.reject("first"); deferral.reject("second") end
  if info.name == "Ctl" then deferral.reject("a\nb\1c") end
  if info.name == "Empty" then deferral.reject("") end
end)
on("playerJoined", function(p)
  log.info("PC late-reject=" .. tostring(pcall(saved.reject, "late")))
end)
)LUA");
        Res(e, "gate2", R"LUA(on("playerConnecting", function(info, deferral) if info.name == "Double" then deferral.reject("from gate2") end end))LUA");
        e.Make();
        e.rt->StartAll();
        ConnectInfo ci;
        ci.name = "Alice";
        ci.address = "1.2.3.4";
        ci.accountId = "name:Alice";
        ConnectVerdict v = e.rt->OnPlayerConnecting(ci);
        Check("a connection nobody objects to is accepted", v.accept, v.reason);
        Eq("the handler gets name, ip and accountId", Mark(e.api, "PC saw Alice "), "ip=1.2.3.4 acct=name:Alice");
        ci.name = "badguy";
        v = e.rt->OnPlayerConnecting(ci);
        Check("deferral.reject refuses the connection with its reason", !v.accept && v.reason == "Name not allowed", v.reason);
        ci.name = "Boom";
        v = e.rt->OnPlayerConnecting(ci);
        Check("an error in a connecting handler fails open and is logged", v.accept && Count(e.api, "connect handler boom") >= 1, v.reason);
        ci.name = "Double";
        v = e.rt->OnPlayerConnecting(ci);
        Check("the first reject wins, also across resources", !v.accept && v.reason == "first", v.reason);
        ci.name = "Ctl";
        v = e.rt->OnPlayerConnecting(ci);
        Check("control characters are removed from a reject reason", !v.accept && v.reason == "a b c", v.reason);
        ci.name = "Empty";
        v = e.rt->OnPlayerConnecting(ci);
        Check("an empty reason becomes a default message", !v.accept && !v.reason.empty(), v.reason);
        Join(e, MkPlayer(1, "Alice"));
        Check("a deferral kept past the event cannot reject later", Mark(e.api, "PC late-reject=") == "false");
        e.rt->Shutdown();
        const ConnectVerdict after = e.rt->OnPlayerConnecting(ci);
        Check("with no resource running every connection is accepted", after.accept);
    }
    {
        Env e("reentrant");
        e.api.kickReenters = true;
        Res(e, "kicker", R"LUA(
on("playerJoined", function(p)
  if p.name == "Troll" then
    log.info("KICKER kick=" .. tostring(kick(p, "no trolls")))
    log.info("KICKER after-kick still running")
  end
end)
)LUA");
        Res(e, "watch", R"LUA(
on("playerLeft", function(p, reason) log.info("WATCH left " .. p.name .. " " .. reason .. " count=" .. players.count()) end)
on("playerJoined", function(p) log.info("WATCH joined " .. p.name) end)
)LUA");
        e.Make();
        e.rt->StartAll();
        Join(e, MkPlayer(1, "Troll"));
        Check("a kick from a handler re-enters the runtime (playerLeft) without trouble", Mark(e.api, "WATCH left ") == "Troll kicked count=0", Mark(e.api, "WATCH left "));
        Check("playerLeft was delivered exactly once", Count(e.api, "WATCH left") == 1);
        Check("the kicking handler carried on afterwards", Mark(e.api, "KICKER kick=") == "true" && Count(e.api, "KICKER after-kick") == 1);
        Check("the kick reached the server with its reason", e.api.kicks.size() == 1 && e.api.kicks[0].reason == "no trolls");
        Check("both resources are fine", e.rt->IsRunning("kicker") && e.rt->IsRunning("watch"));
        Join(e, MkPlayer(2, "Alice"));
        Leave(e, 2, LeaveReason::Timeout);
        Check("the leave reason reaches scripts as text", Mark(e.api, "WATCH left Alice ") == "timeout count=0", Mark(e.api, "WATCH left Alice "));
    }
}

// ---- 14. playerStateUpdate throttle ----------------------------------------------------------------------------------------------------------

static void TestStateUpdates() {
    Env e("state");
    Res(e, "quiet", R"LUA(on("playerJoined", function() end))LUA");
    e.Make();
    e.rt->StartAll();
    Check("the runtime does not want state updates until a script asks", !e.rt->WantsPlayerState());
    Res(e, "tracker", R"LUA(
on("playerStateUpdate", function(p, s)
  log.info("STATE p=" .. p.id .. " x=" .. s.x .. " heading=" .. s.heading .. " vx=" .. s.vx)
end)
)LUA");
    e.rt->Start("tracker");
    Check("a resource listening to playerStateUpdate makes the runtime ask for them", e.rt->WantsPlayerState());
    Join(e, MkPlayer(1, "Alice"));
    PlayerState s;
    s.x = 1;
    s.heading = 10;
    s.vx = 0.5f;
    e.rt->OnPlayerState(1, s);
    s.x = 2;
    e.rt->OnPlayerState(1, s);
    e.rt->Tick();
    Check("many updates in one tick are delivered once, the latest", Count(e.api, "STATE ") == 1 && Has(Mark(e.api, "STATE p=1 "), "x=2.0"), Dump(e.api, "STATE"));
    Check("the state carries heading and velocity", Has(Mark(e.api, "STATE p=1 "), "heading=10.0") && Has(Mark(e.api, "STATE p=1 "), "vx=0.5"), Mark(e.api, "STATE p=1 "));
    e.api.now += 100;
    s.x = 3;
    e.rt->OnPlayerState(1, s);
    e.rt->Tick();
    Check("a second update inside the rate-limit window is held back (4 Hz = 250 ms)", Count(e.api, "STATE ") == 1, std::to_string(Count(e.api, "STATE ")));
    e.api.now += 160;
    e.rt->Tick();
    Check("it is delivered once the window has passed, and it is the latest state", Count(e.api, "STATE ") == 2 && Has(Mark(e.api, "STATE p=1 x=3"), "heading"), Dump(e.api, "STATE"));
    e.api.now += 1000;
    e.rt->Tick();
    Check("nothing is redelivered when nothing changed", Count(e.api, "STATE ") == 2);
    s.x = 4;
    e.rt->OnPlayerState(1, s);
    Leave(e, 1, LeaveReason::Quit);
    e.api.now += 1000;
    e.rt->Tick();
    Check("a pending update of a player who left is dropped", Count(e.api, "STATE ") == 2, std::to_string(Count(e.api, "STATE ")));
    e.rt->Stop("tracker");
    Check("after the listener stops the runtime stops asking", !e.rt->WantsPlayerState());
}

// ---- 15. logging, isolation ------------------------------------------------------------------------------------------------------------------------------

static void TestLoggingAndIsolation() {
    {
        Env e("logs");
        e.opt.logBurst = 20;
        e.opt.logPerSecond = 5;
        Res(e, "spam", R"LUA(
on("playerJoined", function() for i = 1, 50 do log.info("SPAM " .. i) end end)
on("playerLeft", function() log.warn("WARNLINE"); log.error("ERRLINE"); print("PRINTLINE", 42, nil, true) end)
on("chatMessage", function() log.info("ctl:\27[31mred\0end\nnext line") end)
)LUA");
        e.Make();
        e.rt->StartAll();
        Join(e, MkPlayer(1, "Alice"));
        Check("a flood of log lines is limited to the burst size", Count(e.api, "SPAM ") == 20, std::to_string(Count(e.api, "SPAM ")));
        e.api.now += 1000;
        Join(e, MkPlayer(2, "Bob"));
        Check("the limit refills over time", Count(e.api, "SPAM ") == 25, std::to_string(Count(e.api, "SPAM ")));
        Check("the suppressed lines are reported once", Count(e.api, "30 log lines suppressed") == 1, Dump(e.api, "suppressed"));
        e.api.now += 60000;
        Leave(e, 2, LeaveReason::Quit);
        bool warnOk = false, errOk = false;
        for (const auto& l : e.api.logs) {
            if (Has(l.text, "WARNLINE")) warnOk = l.level == LogLevel::Warn;
            if (Has(l.text, "ERRLINE")) errOk = l.level == LogLevel::Error;
        }
        Check("log.warn and log.error map to the severities", warnOk && errOk);
        Eq("print() joins its arguments with tabs", Mark(e.api, "PRINTLINE"), "\t42\tnil\ttrue");
        e.api.now += 60000;
        e.rt->OnChatMessage(1, "hi");
        bool hasEsc = false, hasNul = false, hasNewline = false;
        for (const auto& l : e.api.logs)
            if (Has(l.text, "ctl:")) {
                hasEsc = l.text.find('\x1b') != std::string::npos;
                hasNul = l.text.find('\0') != std::string::npos;
                hasNewline = l.text.find('\n') != std::string::npos;
            }
        Check("terminal escape and NUL characters cannot reach the server log", Count(e.api, "ctl:") == 1 && !hasEsc && !hasNul, Dump(e.api, "ctl:"));
        Check("newlines are kept (tracebacks need them)", hasNewline);
    }
    {
        Env e("isolation");
        Res(e, "a", R"LUA(
shared_global = 1
string.evil = true
os.time = nil
math.pi = 3
)LUA");
        Res(e, "b", R"LUA(
log.info("ISO " .. tostring(shared_global) .. " " .. tostring(string.evil) .. " " .. type(os.time) .. " " .. math.pi)
log.info("ISO2 " .. tostring(("x").evil))
)LUA",
            {"a"});
        e.Make();
        e.rt->StartAll();
        Eq("globals and library tables are per resource", Mark(e.api, "ISO "), "nil nil function 3.1415926535898");
        Eq("string metatable changes do not leak", Mark(e.api, "ISO2 "), "nil");
    }
}

// ---- 16. the documented example ----------------------------------------------------------------------------------------------------------------------------------

static std::string NormalizeNl(std::string s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r') continue;
        o.push_back(s[i]);
    }
    while (!o.empty() && o.back() == '\n') o.pop_back();
    return o;
}

static void TestExample(const fs::path& repo) {
    Env e("example");
    e.opt.resourcesDir = (repo / "libs" / "scripting" / "examples").generic_string();
    e.Make();
    std::string err;
    Check("the example resource starts", e.rt->Start("welcome", &err), err);
    Check("it logs its version", Count(e.api, "welcome loaded, version 1.0.0") == 1, Dump(e.api, "welcome"));

    PlayerInfo alice = MkPlayer(1, "Alice");
    Join(e, alice);
    Check("a joining player is welcomed by name, from the config template", e.api.chats.size() >= 1 && e.api.chats[0].to == 1 &&
                                                                              e.api.chats[0].text == "Welcome to the server, Alice! Type /me <action> to emote.",
          e.api.chats.empty() ? "no chat" : e.api.chats[0].text);
    Check("everyone is told, with the count", e.api.chats.size() >= 2 && e.api.chats[1].to == -1 && e.api.chats[1].text == "Alice joined (1 online).",
          e.api.chats.size() < 2 ? "no chat" : e.api.chats[1].text);
    Check("a first visit has no visit counter line", e.api.chats.size() == 2);

    Check("/me is handled and not broadcast raw", !e.rt->OnChatMessage(1, "/me waves hello"));
    Check("/me broadcasts the action", e.api.chats.back().to == -1 && e.api.chats.back().text == "* Alice waves hello", e.api.chats.back().text);
    e.rt->OnChatMessage(1, "/me");
    Check("/me without text shows usage to the player", e.api.chats.back().to == 1 && e.api.chats.back().text == "Usage: /me <action>", e.api.chats.back().text);
    e.rt->OnChatMessage(1, "/me " + std::string(400, 'x'));
    Check("/me clips a very long action to the chat limit", e.api.chats.back().to == -1 && e.api.chats.back().text.size() <= 200, std::to_string(e.api.chats.back().text.size()));

    e.rt->OnChatMessage(1, "/tp 1 2 3");
    Check("/tp is refused for a non-admin before the script runs", e.api.chats.back().to == 1 && Has(e.api.chats.back().text, "permission") && e.api.moves.empty(), e.api.chats.back().text);

    PlayerInfo root = MkPlayer(2, "Root", true);
    Join(e, root);
    e.rt->OnChatMessage(2, "/tp 10 20 30");
    Check("/tp for an admin reports the missing protocol support", e.api.chats.back().to == 2 && Has(e.api.chats.back().text, "Teleport failed") && Has(e.api.chats.back().text, "protocol") &&
                                                                    e.api.moves.empty(),
          e.api.chats.back().text);
    e.api.allowMove = true;
    e.rt->OnChatMessage(2, "/tp 10 20 30");
    Check("/tp x y z moves the admin", e.api.moves.size() == 1 && e.api.moves[0].id == 2 && e.api.moves[0].pos.x == 10.0f && e.api.moves[0].pos.y == 20.0f &&
                                           e.api.moves[0].pos.z == 30.0f && e.api.moves[0].pos.heading == 0.0f,
          std::to_string(e.api.moves.size()));
    Check("and confirms it", e.api.chats.back().to == 2 && e.api.chats.back().text == "Teleported to 10.0, 20.0, 30.0.", e.api.chats.back().text);
    e.rt->OnChatMessage(2, "/tp Alice");
    Check("/tp <player> needs the target's position", e.api.chats.back().text == "Alice has no position yet." && e.api.moves.size() == 1, e.api.chats.back().text);
    PlayerInfo* ap = e.api.Find(1);
    ap->hasPosition = true;
    ap->state.x = 5;
    ap->state.y = 6;
    ap->state.z = 7;
    ap->state.heading = 45;
    e.rt->OnChatMessage(2, "/tp alice");
    Check("/tp <name> teleports to that player, heading included", e.api.moves.size() == 2 && e.api.moves[1].pos.x == 5.0f && e.api.moves[1].pos.z == 7.0f && e.api.moves[1].pos.heading == 45.0f,
          std::to_string(e.api.moves.size()));
    e.rt->OnChatMessage(2, "/tp 1");
    Check("/tp <id> works the same", e.api.moves.size() == 3 && e.api.moves[2].id == 2, std::to_string(e.api.moves.size()));
    e.rt->OnChatMessage(2, "/tp nobody");
    Check("an unknown player is reported", e.api.chats.back().text == "No such player: nobody", e.api.chats.back().text);
    e.rt->OnChatMessage(2, "/tp 1 2");
    Check("a wrong argument count shows usage", Has(e.api.chats.back().text, "Usage: /tp"), e.api.chats.back().text);
    e.rt->OnChatMessage(2, "/tp a b c");
    Check("non-numeric coordinates show usage", Has(e.api.chats.back().text, "Usage: /tp") && e.api.moves.size() == 3, e.api.chats.back().text);
    std::string out;
    e.rt->ExecuteConsole("tp 1 2 3", out);
    Check("/tp from the console explains it needs a player", Has(out, "needs a player"), out);

    Leave(e, 1, LeaveReason::Quit);
    Check("a leaving player is announced with the reason", e.api.chats.back().to == -1 && e.api.chats.back().text == "Alice left (quit).", e.api.chats.back().text);

    Check("restarting the resource keeps the visit counter (storage)", e.rt->Restart("welcome", &err), err);
    const size_t before = e.api.chats.size();
    Join(e, alice);
    bool visit2 = false;
    for (size_t i = before; i < e.api.chats.size(); i++)
        if (e.api.chats[i].to == 1 && e.api.chats[i].text == "This is visit number 2.") visit2 = true;
    Check("the returning player is greeted with visit number 2", visit2);

    // The reference documents this exact resource: the doc must contain both files verbatim.
    const std::string doc = NormalizeNl(ReadText(repo / "docs" / "api" / "gamemode.md"));
    const std::string lua = NormalizeNl(ReadText(repo / "libs" / "scripting" / "examples" / "welcome" / "main.lua"));
    const std::string json = NormalizeNl(ReadText(repo / "libs" / "scripting" / "examples" / "welcome" / "resource.json"));
    Check("docs/api/gamemode.md exists", !doc.empty());
    Check("the doc contains the example main.lua verbatim", !lua.empty() && Has(doc, lua));
    Check("the doc contains the example resource.json verbatim", !json.empty() && Has(doc, json));
}

// ---- 17. options and helpers ------------------------------------------------------------------------------------------------------------------------------------------

static void TestValidateAndHelpers() {
    namespace fu = gage::scripting::fsutil;
    RuntimeOptions o;
    std::string err;
    Check("Validate accepts the defaults", ScriptRuntime::Validate(o, &err), err);
    o.limits.memoryBytes = 1024;
    Check("Validate names the field when memoryBytes is too small", !ScriptRuntime::Validate(o, &err) && Has(err, "limits.memoryBytes"), err);
    o = RuntimeOptions();
    o.stateUpdateHz = 0;
    Check("Validate rejects a zero state-update rate", !ScriptRuntime::Validate(o, &err) && Has(err, "stateUpdateHz"), err);
    o = RuntimeOptions();
    o.resourcesDir.clear();
    Check("Validate rejects an empty resources folder", !ScriptRuntime::Validate(o, &err) && Has(err, "resourcesDir"), err);
    o = RuntimeOptions();
    o.limits.timeBudgetMs = 0;
    Check("Validate rejects a zero time budget", !ScriptRuntime::Validate(o, &err) && Has(err, "timeBudgetMs"), err);

    Check("resource names: ordinary names are valid", fu::ValidResourceName("good-name_1") && fu::ValidResourceName("A") && fu::ValidResourceName(std::string(64, 'a')));
    Check("resource names: empty, spaces, dots, slashes and over-long names are not", !fu::ValidResourceName("") && !fu::ValidResourceName("a b") && !fu::ValidResourceName("a.b") &&
                                                                                          !fu::ValidResourceName("../x") && !fu::ValidResourceName("x/y") && !fu::ValidResourceName(std::string(65, 'a')));
    Check("resource names: Windows device names are not", !fu::ValidResourceName("con") && !fu::ValidResourceName("NUL") && !fu::ValidResourceName("com1") && !fu::ValidResourceName("LPT9"));
    Check("relative paths: plain nested paths are safe", fu::IsSafeRelPath("main.lua") && fu::IsSafeRelPath("lib/util.lua"));
    Check("relative paths: parent, absolute, drive, backslash and ADS forms are not",
          !fu::IsSafeRelPath("../x.lua") && !fu::IsSafeRelPath("a/../b.lua") && !fu::IsSafeRelPath("/x.lua") && !fu::IsSafeRelPath("C:/x.lua") && !fu::IsSafeRelPath("a\\b.lua") &&
              !fu::IsSafeRelPath("a.lua:stream") && !fu::IsSafeRelPath("") && !fu::IsSafeRelPath("a//b.lua") && !fu::IsSafeRelPath("./a.lua"));
    Check("UTF-8 validation: good text passes, overlong, surrogates and truncation fail",
          fu::Utf8Valid("h\xc3\xa9llo", 6) && !fu::Utf8Valid("\xc0\xaf", 2) && !fu::Utf8Valid("\xed\xa0\x80", 3) && !fu::Utf8Valid("\xe2\x82", 2) && !fu::Utf8Valid("\xff", 1));
    Check("Utf8Truncate never splits a character", fu::Utf8Truncate("h\xc3\xa9llo", 2) == "h" && fu::Utf8Truncate("h\xc3\xa9llo", 3) == "h\xc3\xa9");
    Check("StripBom removes only a leading byte-order mark", fu::StripBom("\xef\xbb\xbf" "abc") == "abc" && fu::StripBom("abc") == "abc");
}

// ---- main ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

static fs::path FindRepoRoot(int argc, char** argv) {
    std::vector<fs::path> candidates;
    if (argc > 1) candidates.push_back(argv[1]);
    candidates.push_back(fs::current_path());
#ifdef GAGE_REPO_ROOT
    candidates.push_back(GAGE_REPO_ROOT);
#endif
    for (const fs::path& c : candidates)
        if (Exists(c / "libs" / "scripting" / "examples" / "welcome" / "resource.json")) return c;
    return fs::path();
}

int main(int argc, char** argv) {
    int watchdog = 120;
    if (const char* w = std::getenv("GAGE_TEST_WATCHDOG_S")) watchdog = std::atoi(w) > 0 ? std::atoi(w) : watchdog;
    std::thread([watchdog] {
        std::this_thread::sleep_for(std::chrono::seconds(watchdog));
        std::printf("FAIL  watchdog: the run did not finish in %d s (a runaway script was not aborted?)\n", watchdog);
        std::fflush(stdout);
        std::_Exit(3);
    }).detach();

    const fs::path repo = FindRepoRoot(argc, argv);
    if (repo.empty()) {
        std::printf("FAIL  cannot find libs/scripting/examples: run from the repository root or pass it as argv[1]\n");
        return 1;
    }

    TestLoadOrder();
    TestStopCleanup();
    TestErrorIsolation();
    TestBudgets();
    TestSandbox();
    TestRequire();
    TestTimers();
    TestChat();
    TestCommands();
    TestConsoleManagement();
    TestExports();
    TestStorage();
    TestJsonAndConfig();
    TestPlayersAndActions();
    TestStateUpdates();
    TestLoggingAndIsolation();
    TestExample(repo);
    TestValidateAndHelpers();

    const int expected = 384;
    std::printf("%d/%d passed  (scratch: %s)\n", g_run - g_failed, g_run, TestRoot().generic_string().c_str());
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    if (!g_failed) {
        std::error_code ec;
        fs::remove_all(TestRoot(), ec);
    }
    return g_failed ? 1 : 0;
}
