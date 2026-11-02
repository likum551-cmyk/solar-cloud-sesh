// gage_scripting: the server-side gamemode runtime of GAGE-MP (Lua 5.4, one lua_State per
// resource). docs/api/gamemode.md is the reference for gamemode authors; this header is the
// reference for the host (the server) that embeds the runtime.
//
// Shape: the server owns a ScriptRuntime and an implementation of IServerApi (the things
// scripts may ask of the server). The server calls the runtime's On*() methods when something
// happens (a player connects, chats, leaves), Tick() once per server tick, and
// ExecuteConsole() for console lines the built-in commands did not handle. The runtime calls
// back through IServerApi. Nothing here depends on ENet or on the GAGE protocol, so the whole
// runtime is testable with a fake IServerApi.
//
// THREAD RULES (the whole library is single-threaded by design):
//  * Every method of ScriptRuntime, and every IServerApi callback it makes, runs on ONE thread:
//    the server tick thread (the thread that calls Server::Poll()). There are no locks and no
//    background threads.
//  * Script code runs only inside those calls, never in the background; a script that runs too
//    long is aborted by its budget (ResourceLimits), it cannot hold up the tick beyond that.
//  * The runtime is re-entrant in one direction: while a script runs, an IServerApi call such as
//    Kick() may make the server call back into On*() (a kick produces playerLeft). The server
//    must therefore invoke the runtime only at points where it holds no iterator or pointer into
//    its player tables across the call, and must re-check any Conn/state it keeps after the call
//    returns (a handler may have kicked that very player).
//  * Errors never propagate as exceptions (the build uses -fno-exceptions); failures are bool
//    returns with a reason string, and a failing script is logged and contained.
#pragma once

#include "storage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gage::scripting {

    // Severity of a log line handed to IServerApi::Log().
    enum class LogLevel { Info, Warn, Error };

    // Why a player left; the runtime hands it to scripts as "quit", "timeout" or "kicked".
    enum class LeaveReason { Quit, Timeout, Kicked };

    // A player's last accepted movement state: position (m), heading (degrees) and velocity
    // (m/s). Mirrors proto::Kinematics without depending on it.
    struct PlayerState
    {
        float x = 0, y = 0, z = 0;
        float heading = 0;
        float vx = 0, vy = 0, vz = 0;
    };

    // A teleport target for IServerApi::SetPosition().
    struct Position
    {
        float x = 0, y = 0, z = 0;
        float heading = 0;
    };

    // A joined player as the server describes it to the runtime (a snapshot, never a handle).
    struct PlayerInfo
    {
        uint16_t id = 0;
        std::string name;
        std::string address;     // dotted IPv4 the player connected from
        // Placeholder identity. The platform has no accounts yet, so the server fills this from
        // the (unauthenticated) player name, e.g. "name:alice". Stable per name, NOT proof of
        // identity. Scripts use it as a storage key; real accounts will replace the value
        // without changing the field.
        std::string accountId;
        uint32_t pingMs = 0;     // smoothed round-trip estimate
        bool isAdmin = false;    // may use restricted commands. Today the server never sets this.
        bool hasPosition = false; // false until the player has sent a state
        PlayerState state;       // meaningful only when hasPosition
    };

    // A connection that has said Hello and passed the server's own checks, about to be accepted.
    struct ConnectInfo
    {
        std::string name;
        std::string address;
        std::string accountId; // see PlayerInfo::accountId
    };

    // The answer to a playerConnecting event.
    struct ConnectVerdict
    {
        bool accept = true;
        std::string reason; // shown to the player when !accept; 1..200 bytes, no control characters
    };

    // What the runtime needs from the server. Implemented by the server (apps/server) and by
    // the fake in tests/scripting_test.cpp. All methods are called on the server tick thread
    // and may be called re-entrantly from inside a runtime call (see THREAD RULES). None may
    // throw. `err`/`msg` out-parameters may be null.
    class IServerApi
    {
    public:
        virtual ~IServerApi() = default;

        // Writes one line to the server log (the server adds its timestamp). The text may
        // contain newlines (error tracebacks); control characters are already removed.
        virtual void Log(LogLevel level, const std::string& line) = 0;

        // A monotonic millisecond clock. Timers, rate limits and the state-update throttle read
        // it, so a test can drive them with a simulated clock. Must never go backwards.
        virtual uint64_t NowMs() = 0;

        // The joined players, ordered by id.
        virtual std::vector<PlayerInfo> Players() = 0;
        // The joined player with `id`; false when there is none.
        virtual bool GetPlayer(uint16_t id, PlayerInfo& out) = 0;

        // A server notice (chat message from the server id) to one player / to everyone. The
        // text has already been checked by the runtime for length (1..200 bytes) but the server
        // must still validate it (control characters, UTF-8, ...). Returns false with *err when
        // it was not sent (no such player, invalid text).
        virtual bool SendChat(uint16_t to, const std::string& text, std::string* err) = 0;
        virtual bool SendChatAll(const std::string& text, std::string* err) = 0;

        // Cuts a player off after telling them `reason` (a server notice). Returns false with
        // *err ("no player <id>") when nobody has that id.
        virtual bool Kick(uint16_t id, const std::string& reason, std::string* err) = 0;
        // Bans the player's name and address (like the console `ban`), then cuts them off.
        // *msg gets the human-readable result or the reason for a false return.
        virtual bool Ban(uint16_t id, const std::string& reason, std::string* msg) = 0;
        // Bans an IPv4 address (like the console `banip`). *msg as for Ban().
        virtual bool BanIp(const std::string& ip, const std::string& reason, std::string* msg) = 0;
        // Lifts the bans that match a name or an address (like the console `unban`).
        virtual bool Unban(const std::string& target, std::string* msg) = 0;

        // Server-authoritative teleport. NEEDS PROTOCOL SUPPORT the client does not have yet
        // (a server-to-client SetPosition message and a client that applies it and then
        // ignores its own stale state until acknowledged). The server adapter returns false
        // with an explanatory *err until then; scripts see `nil, err`.
        virtual bool SetPosition(uint16_t id, const Position& pos, std::string* err) = 0;

        // A read-only server setting by name (name, gamemode, lang, maxPlayers, tickHz, port,
        // ...). Returns false when `name` is not a setting.
        virtual bool GetConvar(const std::string& name, std::string& out) = 0;
    };

    // The budget every entry into script code runs under. An "entry" is one event handler, timer
    // callback, command handler, export call or the loading of a resource's scripts.
    struct ResourceLimits
    {
        // Lua VM instructions per entry. A runaway loop is aborted at about this many
        // instructions (checked every 1000). ~10 M is roughly 30-100 ms of plain Lua.
        uint64_t instructionBudget = 10000000;
        // Wall-clock milliseconds per entry: a backstop for slow work the instruction count
        // underestimates (heavy string/table functions).
        uint32_t timeBudgetMs = 100;
        // Bytes of Lua memory per resource (a custom allocator refuses to grow past it; the
        // script sees "not enough memory").
        size_t memoryBytes = static_cast<size_t>(64) * 1024 * 1024;
        // Matcher steps per entry for string.find/match/gmatch/gsub: Lua patterns backtrack
        // and run inside one C call the instruction hook cannot interrupt (third_party/lua
        // GAGE-PATCHES.md).
        uint64_t patternSteps = 10000000;
        // A resource whose entries are aborted by the budget this many times in a row is
        // stopped automatically (state Failed). 0 never stops it.
        uint32_t maxConsecutiveViolations = 3;
    };

    // Everything configurable about the runtime. Defaults are production values.
    struct RuntimeOptions
    {
        std::string resourcesDir = "resources"; // resources/<name>/resource.json
        std::string dataDir = "data";           // data/<name>/storage.json
        ResourceLimits limits;
        // playerStateUpdate events per player per second at most (the latest state wins; the
        // ones in between are dropped). 1..60.
        uint32_t stateUpdateHz = 4;
        // A dirty storage is written at most this long after the first change (ms), and always
        // on stop, on Shutdown() and on storage.flush().
        uint32_t storageFlushMs = 2000;
        size_t storageMaxBytes = static_cast<size_t>(4) * 1024 * 1024; // per resource
        uint32_t maxTimersPerResource = 256;
        uint32_t maxHandlersPerResource = 256;
        // Script log lines: a token bucket of `logBurst` lines refilling `logPerSecond` per
        // second per resource; the excess is counted and reported in one summary line.
        uint32_t logBurst = 100;
        uint32_t logPerSecond = 100;
        // Writes files atomically; empty means fsutil::AtomicWrite. Tests replace it to
        // simulate a crash in the middle of a write.
        AtomicWriteFn atomicWrite;
    };

    enum class ResourceState
    {
        Stopped, // found and valid, not running
        Running,
        Failed,  // its manifest is invalid, it failed to start, or it was stopped for exceeding its budget
        Blocked, // cannot start: a dependency is missing, failed or part of a cycle
    };

    // One row of ScriptRuntime::Resources().
    struct ResourceStatus
    {
        std::string name;
        std::string version;
        ResourceState state = ResourceState::Stopped;
        std::string detail; // why Failed/Blocked; empty otherwise
        std::vector<std::string> dependencies;
        // Live figures (zero unless Running).
        size_t memoryBytes = 0;
        size_t handlers = 0;
        size_t timers = 0;
        size_t commands = 0;
        size_t exports = 0;
    };

    // Totals over all running resources, for tests and diagnostics.
    struct RuntimeStats
    {
        size_t running = 0;
        size_t handlers = 0;
        size_t timers = 0;       // pending timers, in the global queue
        size_t commands = 0;     // registered commands, in the global registry
        size_t luaBytes = 0;     // Lua memory in use across resources
    };

    // What ExecuteConsole() made of a line.
    enum class ConsoleResult
    {
        NotHandled, // not ours: the server continues with its own commands / "unknown command"
        Ok,
        Error,      // handled, but failed; `output` says why
    };

    namespace detail {
        class Engine; // the implementation (internal.h); not part of the public interface
    }

    // The runtime. Construct one, optionally StartAll(), then feed it the server's events.
    class ScriptRuntime
    {
    public:
        // `api` must outlive the runtime. `options` is copied; out-of-range values are clamped
        // (use Validate() first to reject them instead).
        ScriptRuntime(IServerApi& api, const RuntimeOptions& options);
        // Shutdown()s.
        ~ScriptRuntime();
        ScriptRuntime(const ScriptRuntime&) = delete;
        ScriptRuntime& operator=(const ScriptRuntime&) = delete;

        // True when `options` is acceptable. Returns false with a reason in *err naming the
        // field, e.g. `scripting: limits.memoryBytes must be at least 1048576`.
        static bool Validate(const RuntimeOptions& options, std::string* err);

        // ---- resources -------------------------------------------------------------------

        // Re-reads resources/<name>/resource.json for every subfolder and updates the list of
        // known resources (what the console `refresh` does). Folders with an invalid manifest
        // are listed as Failed with the reason (and logged once). Returns the number of valid
        // resources. Running resources are not touched.
        size_t Refresh();

        // Refresh()es, then starts every valid resource in dependency order (dependencies
        // first, ties by name). Resources in a dependency cycle, with a missing dependency, or
        // depending on one of those are not started and are logged as Blocked; others are
        // unaffected. Returns the number of resources running afterwards.
        size_t StartAll();

        // Starts `name` (re-reading its manifest from disk) after starting its dependencies, in
        // order. Returns true when `name` is running afterwards (already running counts).
        // Returns false with *err for: an unknown or invalid resource, a missing dependency,
        // a dependency cycle, a damaged storage file, a script that fails to load or exceeds its
        // budget while loading. Nothing of a failed start stays registered. Dependencies that did
        // start stay running.
        bool Start(const std::string& name, std::string* err = nullptr);

        // Stops `name`: its dependents first (reverse order), then it. Each resource gets a
        // resourceStop event, its storage is flushed, and its handlers, timers, commands and
        // exports are removed with its Lua state. Returns false with *err for a resource that
        // is not running.
        bool Stop(const std::string& name, std::string* err = nullptr);

        // Stop() then Start(), and starts the dependents that Stop() took down again.
        bool Restart(const std::string& name, std::string* err = nullptr);

        // Stops every running resource (reverse start order) and flushes storage. Idempotent.
        void Shutdown();

        // Every known resource, sorted by name.
        std::vector<ResourceStatus> Resources() const;
        // True when `name` is running.
        bool IsRunning(const std::string& name) const;
        // Totals, see RuntimeStats.
        RuntimeStats Stats() const;

        // ---- events from the server --------------------------------------------------------

        // Call once per server tick, after the network was serviced and before the world state
        // is broadcast. Runs due timers (in due-time order), delivers throttled
        // playerStateUpdate events, writes dirty storage, and stops resources that were
        // marked for stopping.
        void Tick();

        // A connection passed the server's checks and is about to be accepted: runs the
        // playerConnecting handlers. Returns {accept=false, reason} when a handler called
        // deferral.reject(reason); an error in a handler does not reject (fail-open, logged).
        // Does nothing (accepts) when no resource is running.
        ConnectVerdict OnPlayerConnecting(const ConnectInfo& info);

        // The player is now in the server's player table (Players()/GetPlayer() find it).
        void OnPlayerJoined(uint16_t id);

        // The player has been removed from the server's player table; `info` is the snapshot
        // taken just before. Also forgets the state-update throttle of that id.
        void OnPlayerLeft(const PlayerInfo& info, LeaveReason why);

        // A chat line from a player that passed the server's flood and validity checks. Returns
        // true when the server should broadcast it as usual, false when it must not: a
        // `/command` (handled by a command handler, or answered "Unknown command") or a
        // chatMessage handler called cancelEvent(). When no resource is running it returns true.
        bool OnChatMessage(uint16_t playerId, const std::string& text);

        // An accepted state of a player. Cheap: it only records the state; delivery to
        // playerStateUpdate handlers happens from Tick(), at most stateUpdateHz per player.
        void OnPlayerState(uint16_t id, const PlayerState& state);

        // True when at least one running resource listens to playerStateUpdate, so a server
        // may skip calling OnPlayerState() when false.
        bool WantsPlayerState() const;

        // ---- console -------------------------------------------------------------------------

        // Runs a console / admin-channel line that the server's own commands did not match.
        // Handles `start <res>`, `stop <res>`, `restart <res>`, `refresh`, `resources`, and
        // every command registered by a resource (as the console, which counts as an admin).
        // `stop` without an argument is NotHandled (the server's own `stop` quits). The
        // human-readable result goes to `output`.
        ConsoleResult ExecuteConsole(const std::string& line, std::string& output);

    private:
        std::unique_ptr<detail::Engine> impl_;
    };

}
