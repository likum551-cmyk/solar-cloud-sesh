// Internal types of gage_scripting. Not part of the public interface (scripting.h is).
//
// LUA ERROR SAFETY (read this before touching any .cpp that includes lua_include.h)
// Lua is compiled as C and raises errors with longjmp; this library is C++17 with
// -fno-exceptions. A longjmp skips C++ destructors, so:
//  1. Host code never touches a resource's Lua stack outside a protected call. Everything that
//     may allocate in Lua (push a string, create a table, luaL_ref) runs inside Resource::Run(),
//     which executes it under lua_pcall. Run/Invoke themselves are non-raising.
//  2. Inside a function that can be unwound by a Lua error (every lua_CFunction, every Run()
//     body) no object with a non-trivial destructor may be alive across a call that can raise:
//     luaL_error/lua_error, luaL_check*, lua_push*/lua_createtable/lua_settable/luaL_ref
//     (memory errors), lua_call (never used: only lua_pcall). Such objects either live in an
//     outer frame that is NOT unwound (the caller of Run(), which a protected call returns
//     into normally), in a Resource member (the *Scratch fields), or in an inner block that has
//     ended before the next raising call.
//  3. FromLua() and Invoke() never raise; ToLua(), json decode and the glue helpers may.
//
// Thread-safety: single server thread, see scripting.h. The pattern-budget counter is a plain
// global for that reason.
#pragma once

#include "lua_include.h"
#include "manifest.h"
#include "scripting.h"
#include "storage.h"
#include "value.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern "C" {
// Defined by the patched third_party/lua/src/lstrlib.c (GAGE_LUA_PATTERN_BUDGET).
extern unsigned long gage_lua_pattern_steps_left;
}

namespace gage::scripting::detail {

    // Instructions between hook calls; the budget is checked at this granularity.
    constexpr int kHookCount = 1000;
    // Nesting limit of cross-resource calls (exports, custom events) so a ping-pong cannot
    // recurse the C stack without bound.
    constexpr int kMaxCallDepth = 8;
    // The largest script or module source the runtime loads.
    constexpr size_t kMaxScriptBytes = 1024 * 1024;
    // table.move is refused beyond this many elements (a C loop the hook cannot interrupt).
    constexpr lua_Integer kMaxMoveCount = 4 * 1024 * 1024;

    class Engine;
    struct Resource;

    // Lua memory accounting of one state (the allocator's user data).
    struct MemState
    {
        size_t used = 0;
        size_t peak = 0;
        size_t limit = 0;
        bool exceeded = false; // the allocator refused a request since the last arm
    };

    // The execution budget of one resource (see ResourceLimits). Armed by the outermost entry.
    struct Budget
    {
        bool armed = false;
        bool tripped = false;       // the limit was hit; the hook now raises on every instruction
        int depth = 0;              // nested Invoke() calls on this state
        uint64_t used = 0;          // instructions so far in this entry
        uint64_t limit = 0;
        int64_t deadlineNs = 0;     // steady-clock time at which the time budget ends
        uint32_t timeMs = 0;        // the time budget, for messages
        unsigned long savedPatternSteps = 0;
        char reason[128] = {0};
    };

    // Pushes the arguments of a handler call and returns how many it pushed. Runs inside the
    // protected region of the target state, so it may raise (memory) but must obey rule 2.
    using ArgPusher = std::function<int(lua_State*)>;

    struct HandlerRec
    {
        std::string event;
        int ref = LUA_NOREF;
    };

    struct TimerRec
    {
        int ref = LUA_NOREF;
        bool repeat = false;
        uint32_t intervalMs = 0;
        uint64_t due = 0;
        uint64_t seq = 0;
    };

    struct CommandRec
    {
        int ref = LUA_NOREF;
        bool restricted = false;
        std::string help;
    };

    // Who ran a command: the console, or a player.
    struct Source
    {
        bool console = false;
        PlayerInfo player;
    };

    // One running (or loading) resource: its Lua state and everything registered through it.
    struct Resource
    {
        Engine* rt = nullptr;
        Manifest manifest;
        std::string name;
        std::string dir;      // resources/<name>
        std::string dataDir;  // data/<name>
        lua_State* L = nullptr;
        MemState mem;
        Budget budget;
        uint32_t violations = 0;   // consecutive entries aborted by the budget
        bool faulted = false;      // marked for stopping at the end of the outermost call
        uint64_t startSeq = 0;

        Storage storage;
        uint64_t storageDirtySinceMs = 0;

        std::map<uint64_t, HandlerRec> handlers;
        std::map<std::string, std::vector<uint64_t>> handlerOrder; // event -> ids, registration order
        uint64_t nextHandlerId = 1;
        std::map<uint32_t, TimerRec> timers;
        uint32_t nextTimerId = 1;
        std::map<std::string, CommandRec> commands;
        std::map<std::string, int> exportRefs;

        bool cancellable = false;  // inside a cancellable event dispatch
        bool cancelled = false;

        double logTokens = 0;
        uint64_t logRefillMs = 0;
        uint32_t logSuppressed = 0;

        // Scratch the glue uses so nothing with a destructor is alive across a raising call
        // (rule 2): these members survive a longjmp.
        std::string warnBuf;
        std::string scratch;
        std::string scratch2;
        std::string xferErr;
        std::string jsonOut;
        std::vector<Value> xfer;
        std::vector<PlayerInfo> playersScratch;
        std::vector<std::string> stringsScratch;
        PlayerInfo playerScratch;

        // Calls fn(L, arg) in a protected call on the main thread. Returns true when fn
        // returned; false with *err (when non-null) when a Lua error (typically memory) was
        // raised inside. Never raises.
        bool Run(void (*fn)(lua_State*, void*), void* arg, std::string* err);

        // Calls the function below `nargs` arguments on top of the stack under the budget and
        // a traceback message handler, then removes the function and arguments and (on
        // success) leaves `nresults` results (LUA_MULTRET allowed). Failures are logged here
        // (what = "handler for 'playerJoined'" etc.); *errOut (when non-null) gets the message
        // without traceback. Returns true on success. Never raises. Must be called from inside
        // Run() (or from another non-raising frame holding the state's stack consistent).
        bool Invoke(int nargs, int nresults, const char* what, std::string* errOut);
    };

    // The implementation behind ScriptRuntime.
    class Engine
    {
    public:
        Engine(IServerApi& api, const RuntimeOptions& options);
        ~Engine();

        IServerApi& api;
        RuntimeOptions opt;

        // ---- logging (runtime.cpp)
        // A runtime message, never rate limited: "script: <text>".
        void LogSys(LogLevel level, const std::string& text);
        // A message from a resource (its own log.* calls, handler errors): "[name] <text>",
        // rate limited per resource, control characters removed.
        void LogScript(Resource& r, LogLevel level, const std::string& text);

        // ---- resources (runtime.cpp)
        size_t Refresh();
        size_t StartAll();
        bool Start(const std::string& name, std::string* err);
        bool Stop(const std::string& name, std::string* err);
        bool Restart(const std::string& name, std::string* err);
        void Shutdown();
        std::vector<ResourceStatus> Resources() const;
        bool IsRunning(const std::string& name) const;
        RuntimeStats Stats() const;
        Resource* FindRunning(const std::string& name) const;
        bool IsLive(const Resource* r) const;

        // ---- events (runtime.cpp)
        void Tick();
        ConnectVerdict OnPlayerConnecting(const ConnectInfo& info);
        void OnPlayerJoined(uint16_t id);
        void OnPlayerLeft(const PlayerInfo& info, LeaveReason why);
        bool OnChatMessage(uint16_t playerId, const std::string& text);
        void OnPlayerState(uint16_t id, const PlayerState& s);
        bool WantsPlayerState() const;
        ConsoleResult ExecuteConsole(const std::string& line, std::string& output);

        // Delivers `event` to every handler of `r` (a snapshot of the handler list). Returns
        // true when a handler called cancelEvent() (only possible when `cancellable`).
        bool DispatchTo(Resource& r, const std::string& event, const ArgPusher& push, bool cancellable);
        // DispatchTo() for every running resource in start order. Returns true when cancelled.
        bool DispatchAll(const std::string& event, const ArgPusher& push, bool cancellable);
        // A custom event from `from` to every running resource (including `from`), values
        // copied into each. False with *err when nested too deeply.
        bool EmitCustom(Resource& from, const std::string& event, const std::vector<Value>& args, std::string* err);
        // exports.<target>.<fn>(args): runs the exported function in `target`'s state and
        // copies its results into `results`. False with *err when it could not be called or
        // raised an error.
        bool CallExport(const std::string& target, const std::string& fn, const std::vector<Value>& args,
                        std::vector<Value>& results, std::string* err);

        // ---- timers (runtime.cpp)
        uint32_t AddTimer(Resource& r, int ref, uint32_t delayMs, bool repeat);
        // Removes timer `id` of `r`; true when it existed, with its callback's registry reference
        // in *refOut (the caller, running inside the state, releases it).
        bool ClearTimer(Resource& r, uint32_t id, int* refOut);

        // ---- commands (runtime.cpp)
        // Registers (or replaces, for the same resource) a command. False with *err for a
        // reserved name or one owned by another resource.
        bool RegisterCommand(Resource& r, const std::string& name, int ref, bool restricted, const std::string& help,
                             std::string* err);
        // Output of `reply()` to the console source while a console command runs.
        void ConsoleReply(const std::string& text);

        // ---- state / budget (sandbox.cpp)
        bool CreateState(Resource& r, std::string* err);
        void CloseState(Resource& r);
        void ArmBudget(Resource& r);
        void DisarmBudget(Resource& r);
        // Bookkeeping after an outermost entry: budget violations and memory-limit hits.
        void NoteEntry(Resource& r, bool tripped, const char* what, const std::string& trippedReason);

        // Per-tick/entry bookkeeping
        int depth = 0;       // public-call nesting (EntryScope)
        int callDepth = 0;   // cross-resource call nesting
        void FlushDeferred();

        // ---- active-call state
        ConnectVerdict* activeVerdict = nullptr; // set while playerConnecting handlers run
        std::string* consoleOut = nullptr;       // set while a console command runs

        // ---- tables
        struct Known
        {
            Manifest manifest;
            bool valid = false;
            std::string error;      // invalid manifest, or why the last start failed
            bool startFailed = false;
            bool blocked = false;
            std::string blockedReason;
        };
        std::map<std::string, Known> known;
        std::vector<std::unique_ptr<Resource>> running; // start order
        std::map<std::string, std::string> commandOwner; // command -> resource name
        struct TimerKey
        {
            uint64_t due;
            uint64_t seq;
            Resource* res;
            uint32_t id;
            bool operator<(const TimerKey& o) const { return due != o.due ? due < o.due : seq < o.seq; }
        };
        std::set<TimerKey> timerQueue;
        uint64_t nextSeq = 1;
        uint64_t nextStartSeq = 1;
        std::vector<std::string> pendingStops;
        bool flushing = false;

        struct StateTrack
        {
            PlayerState state;
            bool dirty = false;
            uint64_t lastDeliveredMs = 0;
            bool everDelivered = false;
        };
        std::map<uint16_t, StateTrack> stateTracks;

    private:
        // lifecycle internals
        // Reads the manifest (and config.json) of `name` into `known`. False with the reason in
        // lastLoadError: an invalid name or a missing resource.json leaves no entry in `known`;
        // an unreadable or invalid manifest leaves an entry with valid=false and the reason.
        bool LoadKnown(const std::string& name);
        std::string lastLoadError;
        // Removes the timers and command names a resource registered in the global tables.
        void Unregister(Resource& r);
        bool PlanStart(const std::vector<std::string>& roots, std::vector<std::string>& order,
                       std::map<std::string, std::string>& blocked);
        bool StartOne(const std::string& name, std::string* err);
        bool LoadScripts(Resource& r, std::string* err);
        void Teardown(Resource& r, bool deliverStop);
        void StopClosure(const std::string& name, std::vector<std::string>* stoppedDependents);
        std::vector<std::string> DependentsOf(const std::string& name) const;
        void FireDueTimers();
        void DeliverStateUpdates();
        void FlushStorages(bool all);
        bool RunCommand(const std::string& name, const Source& src, const std::vector<std::string>& args, const std::string& raw);
        bool IsReservedCommand(const std::string& name) const;
        std::string ListResources() const;
    };

    // ---- sandbox.cpp -------------------------------------------------------------------------

    // The Resource that owns `L` (any thread of its state).
    inline Resource* ResourceOf(lua_State* L)
    {
        return *reinterpret_cast<Resource**>(lua_getextraspace(L));
    }

    // Registers the script-facing API (on, emit, timers, commands, players, storage, log, json,
    // config, exports, ...) into the state. Runs inside the protected setup call.
    void RegisterApi(lua_State* L, Resource* r);

    // ---- luaapi.cpp --------------------------------------------------------------------------

    // Pushes the player table scripts see: {id, name, ip, accountId, ping, isAdmin, console=false}.
    void PushPlayerTable(lua_State* L, const PlayerInfo& p);
    // The table for the console as a command source: {id=-1, name="console", console=true, isAdmin=true, ...}.
    void PushConsoleSource(lua_State* L);
    // The `deferral` argument of playerConnecting handlers: {reject = function(reason) end}.
    void PushDeferral(lua_State* L);
    // Pushes the table for a command source.
    void PushSource(lua_State* L, const Source& s);
    // True when `event` is one of the built-in event names (not emittable by scripts).
    bool IsBuiltinEvent(const std::string& event);
    // Replaces control characters, validates UTF-8 and truncates a script-provided refuse or
    // kick reason; empty or invalid text yields `fallback`.
    std::string CleanReason(const char* p, size_t n, const std::string& fallback);

    // The text of an error up to its traceback ("msg\nstack traceback:..." -> "msg").
    std::string FirstPart(const std::string& text);

    // RAII for the public entry points: while one is alive, a stop requested by a budget
    // violation waits until the outermost one ends.
    struct EntryScope
    {
        Engine& e;
        explicit EntryScope(Engine& engine) : e(engine) { e.depth++; }
        ~EntryScope()
        {
            if (--e.depth == 0) e.FlushDeferred();
        }
    };

}
